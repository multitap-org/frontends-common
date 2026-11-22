// Checks net_share_link.h across the internet, over loopback against the
// real beacon server code: a piccolo paired on the LAN is reached again
// through the beacon (net_share_link_connect_paired), and a new piccolo
// pairs across the internet with its own code, which the host's user types
// in INVITE (net_access_invite, net_share_link_pair_beacon).
// Linux only, like the server.
//
// Usage: frontend_net_share_link_beacon_test <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "beacon_net.h"
#include "beacon_server.h"
#include "host_fs.h"
#include "net_service.h"
#include "net_share_link.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

struct World
{
    BeaconServer server;
    BeaconNet    net;
    uint16_t     port = 0;
    NetService*  service = nullptr;
};

static std::string g_code;   // the code the client shows: the host's user types it

// One round: the beacon answers what came, the host's frontend drains its
// queue (as piccolo's loop does) and types the client's code at SHARE.
static void pump(World* world)
{
    uint64_t now = beacon_now_ms();
    beacon_net_drain(&world->net, &world->server, now, BeaconSocket::PRIMARY, 64);
    beacon_net_drain(&world->net, &world->server, now, BeaconSocket::SECONDARY, 64);
    beacon_server_tick(&world->server, now);
    net_service_lock(world->service);
    NetAccessEvent event;
    while (net_service_next(world->service, &event))
    {
        if (event.type == NetAccessEventType::pairing_requested)
        {
            net_access_answer_pairing(&world->service->access, event.peer, g_code.c_str(), NetAccessLevel::share);
        }
    }
    net_service_unlock(world->service);
}

static void pump_for(World* world, int milliseconds)
{
    for (int i = 0; i < milliseconds / 5; i++)
    {
        pump(world);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

static NetShareLinkState wait_state(World* world, NetShareLink* link, NetShareLinkState want, NetShareLinkStatus* status)
{
    for (int i = 0; i < 2000; i++)
    {
        pump(world);
        net_share_link_status(link, status);
        if (status->state == want)
        {
            return status->state;
        }
        if (status->state == NetShareLinkState::idle && status->refusal != NetClientRefusal::none)
        {
            return status->state;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return status->state;
}

static bool open_beacon(World* world)
{
    BeaconServerConfig config;
    if (!beacon_server_create(&world->server, &config, beacon_now_ms()))
    {
        return false;
    }
    // net_beacon finds the secondary port at primary + 1: try a few pairs.
    for (uint16_t attempt = 0; attempt < 20; attempt++)
    {
        uint16_t port = static_cast<uint16_t>(43000 + (beacon_now_ms() % 9000) + attempt * 2);
        if (beacon_net_open(&world->net, "::", port, static_cast<uint16_t>(port + 1), -1))
        {
            world->port = port;
            return true;
        }
    }
    return false;
}

// The host's beacon registration covers its current paired devices.
static bool wait_registered(World* world)
{
    for (int i = 0; i < 1000; i++)
    {
        pump(world);
        net_service_lock(world->service);
        NetBeaconStatus status = net_access_beacon_status(&world->service->access);
        net_service_unlock(world->service);
        if (status == NetBeaconStatus::registered)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2 || !net_init())
    {
        std::printf("FAIL usage: frontend_net_share_link_beacon_test <scratch folder>\n");
        return 1;
    }
    std::string base = std::string(argv[1]) + "/run-" + std::to_string(std::time(nullptr)) + "-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    check(host_fs_make_directories(base.c_str()), "scratch folder");

    World* world = new World();
    check(open_beacon(world), "beacon listens");
    std::string beacon_name = "127.0.0.1:" + std::to_string(world->port);

    NetKeypair host_key;
    NetKeypair client_key;
    NetKeypair second_key;
    check(net_identity_load_or_create((base + "/host_id.cfg").c_str(), &host_key) &&
              net_identity_load_or_create((base + "/client_id.cfg").c_str(), &client_key) &&
              net_identity_load_or_create((base + "/second_id.cfg").c_str(), &second_key),
          "identities");

    world->service = new NetService();
    check(net_service_start(world->service, 0, "HOST PICCOLO", &host_key, (base + "/host_devices.cfg").c_str()),
          "host started");
    net_service_set_beacon(world->service, beacon_name.c_str());
    net_service_lock(world->service);
    uint16_t port = net_host_port(&world->service->access.session.host);
    net_service_unlock(world->service);
    check(wait_registered(world), "the host registered with the beacon");

    // Paired on the LAN first.
    NetShareLink* link = new NetShareLink();
    check(net_share_link_start(link, &client_key, "CLIENT PICCOLO", (base + "/client_hosts.cfg").c_str()), "link started");
    NetAddress address;
    net_address_from_text("127.0.0.1", port, &address);
    NetShareLinkStatus status;
    char code[NET_PAIR_CODE_DIGITS + 1];
    net_pair_code_generate(code);
    g_code = code;
    net_share_link_connect(link, address, code);
    check(wait_state(world, link, NetShareLinkState::ready, &status) == NetShareLinkState::ready, "paired on the LAN");
    check(status.paired.size() == 1 && status.paired[0] == "HOST PICCOLO", "the paired piccolo listed");
    net_share_link_disconnect(link);
    pump_for(world, 300);
    check(wait_registered(world), "the host registered its new device");

    // Away from home: through the beacon.
    check(!net_share_link_connect_paired(link, 0), "no beacon set: refused");
    net_share_link_set_beacon(link, beacon_name.c_str());
    check(!net_share_link_connect_paired(link, 5), "no such paired piccolo");
    check(net_share_link_connect_paired(link, 0), "connect through the beacon starts");
    check(wait_state(world, link, NetShareLinkState::ready, &status) == NetShareLinkState::ready &&
              status.host_name == "HOST PICCOLO",
          "connected through the beacon");
    if (status.state != NetShareLinkState::ready)
    {
        std::printf("beacon connect: state %u refusal %u\n", static_cast<unsigned>(status.state),
                    static_cast<unsigned>(status.refusal));
    }
    net_share_link_disconnect(link);

    // A new piccolo pairs across the internet: the host's user types its code
    // in INVITE, which waits in the code's pairing room.
    NetShareLink* second = new NetShareLink();
    check(net_share_link_start(second, &second_key, "SECOND PICCOLO", (base + "/second_hosts.cfg").c_str()),
          "second link started");
    net_share_link_set_beacon(second, beacon_name.c_str());
    check(!net_share_link_pair_beacon(second, "123456"), "a code that isn't 9 digits is refused");
    net_pair_code_generate(code);
    g_code = code;
    net_service_lock(world->service);
    check(net_access_invite(&world->service->access, code), "the host invites with that code");
    net_service_unlock(world->service);
    check(wait_registered(world), "the host's pairing room registered");
    pump_for(world, 300);
    check(net_share_link_pair_beacon(second, code), "pairing across the internet starts");
    check(wait_state(world, second, NetShareLinkState::ready, &status) == NetShareLinkState::ready,
          "paired across the internet");
    if (status.state != NetShareLinkState::ready)
    {
        std::printf("internet pairing: state %u refusal %u\n", static_cast<unsigned>(status.state),
                    static_cast<unsigned>(status.refusal));
    }
    check(status.paired.size() == 1, "the second piccolo lists its host");

    net_share_link_stop(second);
    net_share_link_stop(link);
    net_service_stop(world->service);
    delete second;
    delete link;
    delete world->service;
    beacon_net_close(&world->net);
    beacon_server_destroy(&world->server);
    delete world;
    net_keypair_wipe(&host_key);
    net_keypair_wipe(&client_key);
    net_keypair_wipe(&second_key);
    net_shutdown();
    if (g_failures == 0)
    {
        std::printf("net share link beacon: all checks passed\n");
        return 0;
    }
    return 1;
}

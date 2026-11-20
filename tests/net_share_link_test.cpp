// Checks net_share_link.h over loopback: a frontend's client connection to
// another piccolo (a net_service host) on its own thread. A wrong code is
// refused, the right one pairs (as GAMEPAD: no listing), raised to OWNER it
// lists and copies a file, and disconnecting ends it cleanly.
//
// Usage: frontend_net_share_link_test <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

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

static bool g_pairing_failed = false;   // the host reported a failed attempt (its code changed)

// The host's frontend drains its queue, as piccolo's loop does.
static void host_round(NetService* service)
{
    net_service_lock(service);
    NetAccessEvent event;
    while (net_service_next(service, &event))
    {
        if (event.type == NetAccessEventType::pairing_failed)
        {
            g_pairing_failed = true;
        }
    }
    net_service_unlock(service);
}

static NetShareLinkState wait_state(NetShareLink* link, NetService* service, NetShareLinkState want,
                                    NetShareLinkStatus* status)
{
    for (int i = 0; i < 600; i++)
    {
        host_round(service);
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

static bool wait_listing(NetShareLink* link, NetService* service, std::vector<NetShareEntry>* entries,
                         NetShareStatus* status, int rounds)
{
    std::string path;
    for (int i = 0; i < rounds; i++)
    {
        host_round(service);
        if (net_share_link_listing(link, &path, status, entries))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2 || !net_init())
    {
        std::printf("FAIL usage: frontend_net_share_link_test <scratch folder>\n");
        return 1;
    }
    std::string run = std::string(argv[1]) + "/run-" + std::to_string(std::time(nullptr)) + "-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    char canonical[4096] = {};
    if (!host_fs_make_directories(run.c_str()) || !host_fs_canonical(run.c_str(), canonical, sizeof(canonical)))
    {
        std::printf("FAIL scratch folder\n");
        return 1;
    }
    std::string base = canonical;
    std::string host_games = base + "/host/roms";
    std::string client_games = base + "/client/games";
    host_fs_make_directories(host_games.c_str());
    host_fs_make_directories(client_games.c_str());
    std::string game = host_games + "/Game.sfc";
    host_fs_write_file(game.c_str(), "SUPER GAME BYTES", 16);

    NetKeypair host_key;
    NetKeypair client_key;
    check(net_identity_load_or_create((base + "/host_id.cfg").c_str(), &host_key) &&
              net_identity_load_or_create((base + "/client_id.cfg").c_str(), &client_key),
          "identities");
    std::string host_devices = base + "/host_devices.cfg";
    std::string client_hosts = base + "/client_hosts.cfg";

    NetService* service = new NetService();
    check(net_service_start(service, 0, "HOST PICCOLO", &host_key, host_devices.c_str()), "host started");
    std::vector<std::string> games;
    games.push_back(host_games);
    net_service_lock(service);
    net_service_set_share_folders(service, &games, "", "");
    net_access_open_pairing(&service->access);
    std::string code = service->access.code;
    uint16_t port = net_host_port(&service->access.session.host);
    net_service_unlock(service);

    NetShareLink* link = new NetShareLink();
    check(net_share_link_start(link, &client_key, "CLIENT PICCOLO", client_hosts.c_str()), "link started");
    std::vector<std::string> client_game_paths;
    client_game_paths.push_back(client_games);
    net_share_link_set_folders(link, &client_game_paths, "", "");
    NetAddress address;
    net_address_from_text("127.0.0.1", port, &address);

    NetShareLinkStatus status;
    std::string wrong = code == "000000" ? "111111" : "000000";
    net_share_link_connect(link, address, wrong.c_str());
    wait_state(link, service, NetShareLinkState::ready, &status);
    check(status.state == NetShareLinkState::idle && status.refusal == NetClientRefusal::wrong_code,
          "a wrong code is refused");

    // The host changes its code once it has counted the failure: wait for
    // that before asking for a new one.
    for (int i = 0; i < 400 && !g_pairing_failed; i++)
    {
        host_round(service);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(g_pairing_failed, "the host counted the failed attempt");
    net_service_lock(service);
    net_access_open_pairing(&service->access);   // a new code after the failure
    code = service->access.code;
    net_service_unlock(service);
    net_share_link_connect(link, address, code.c_str());
    check(wait_state(link, service, NetShareLinkState::ready, &status) == NetShareLinkState::ready &&
              status.host_name == "HOST PICCOLO" && status.access == NetAccessLevel::gamepad,
          "paired with the code, as GAMEPAD");
    if (status.state != NetShareLinkState::ready)
    {
        std::printf("pairing: state %u refusal %u\n", static_cast<unsigned>(status.state),
                    static_cast<unsigned>(status.refusal));
    }

    std::vector<NetShareEntry> entries;
    NetShareStatus list_status = NetShareStatus::failed;
    net_share_link_list(link, "");
    check(!wait_listing(link, service, &entries, &list_status, 100), "no listing at GAMEPAD");

    net_service_lock(service);
    net_access_set_level(&service->access, 0, NetAccessLevel::owner);
    net_service_unlock(service);
    for (int i = 0; i < 100 && status.access != NetAccessLevel::owner; i++)
    {
        host_round(service);
        net_share_link_status(link, &status);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(status.access == NetAccessLevel::owner, "told it's OWNER now");
    net_share_link_list(link, "");
    check(wait_listing(link, service, &entries, &list_status, 1000) && list_status == NetShareStatus::ok &&
              entries.size() == 1 && entries[0].name == "roms",
          "lists the host's shared folders");

    net_share_link_copy(link, false, "games/Game.sfc", "roms/Game.sfc", 16, 0);
    std::vector<NetShareClientDone> done;
    for (int i = 0; i < 2000 && done.empty(); i++)
    {
        host_round(service);
        net_share_link_take_done(link, &done);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    uint8_t* data = nullptr;
    size_t size = 0;
    std::string got = client_games + "/Game.sfc";
    check(done.size() == 1 && done[0].status == NetShareStatus::ok && host_fs_read_file(got.c_str(), &data, &size) &&
              size == 16 && std::memcmp(data, "SUPER GAME BYTES", 16) == 0,
          "a file copied through the link");
    delete[] data;

    // Paired now: connecting again needs no code.
    net_share_link_disconnect(link);
    net_share_link_status(link, &status);
    check(status.state == NetShareLinkState::idle, "disconnected");
    net_share_link_connect(link, address, "");
    check(wait_state(link, service, NetShareLinkState::ready, &status) == NetShareLinkState::ready,
          "reconnects without a code");
    if (status.state != NetShareLinkState::ready)
    {
        std::printf("state %u refusal %u\n", static_cast<unsigned>(status.state), static_cast<unsigned>(status.refusal));
    }

    net_share_link_stop(link);
    net_service_stop(service);
    delete link;
    delete service;
    net_shutdown();
    if (g_failures == 0)
    {
        std::printf("net share link: all checks passed\n");
        return 0;
    }
    return 1;
}

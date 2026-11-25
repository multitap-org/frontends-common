// Checks net_service.h over loopback: a paired client connects to a host
// whose network runs on its own thread; its events arrive through the queue
// (copied: valid after the thread polled again), sending under the lock
// works, and stopping joins the thread cleanly.
//
// Usage: frontend_net_service_test <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "host_fs.h"
#include "net_client.h"
#include "net_service.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

// A device as these tests describe it: a piccolo (offers everything) or a
// phone (offers nothing), using everything they could.
static NetDeviceInfo test_device(const char* name, bool phone)
{
    NetDeviceInfo info;
    net_device_info_set_name(&info, name);
    info.kind = phone ? NetAppKind::piccoletto : NetAppKind::piccolo;
    info.offers = phone ? 0 : NET_CAPS_ALL;
    info.uses = NET_CAPS_ALL;
    return info;
}

int main(int argc, char** argv)
{
    if (argc < 2 || !net_init())
    {
        std::printf("FAIL usage: frontend_net_service_test <scratch folder>\n");
        return 1;
    }
    std::string folder = argv[1];
    host_fs_make_directories(folder.c_str());
    std::string host_identity_path = folder + "/host_identity.cfg";
    std::string client_identity_path = folder + "/client_identity.cfg";
    std::string host_devices = folder + "/host_devices.cfg";
    std::string client_hosts = folder + "/client_hosts.cfg";
    std::remove(host_devices.c_str());
    std::remove(client_hosts.c_str());
    NetKeypair host_key;
    NetKeypair client_key;
    check(net_identity_load_or_create(host_identity_path.c_str(), &host_key) &&
              net_identity_load_or_create(client_identity_path.c_str(), &client_key),
          "identities");
    // Already paired, both ways.
    NetTrust host_trust;
    net_trust_add(&host_trust, client_key.public_key, "CLIENT");
    host_trust.devices[0].grants = NET_CAPS_ALL;
    NetTrust client_trust;
    net_trust_add(&client_trust, host_key.public_key, "HOST");
    check(net_trust_save(&host_trust, host_devices.c_str()) && net_trust_save(&client_trust, client_hosts.c_str()),
          "lists saved");

    NetService* service = new NetService();
    NetDeviceInfo host_info = test_device("HOST", false);
    check(net_service_start(service, 0, &host_info, &host_key, host_devices.c_str()), "service started");
    net_service_lock(service);
    uint16_t port = net_host_port(&service->access.session.host);
    net_service_unlock(service);

    NetClient* client = new NetClient();
    NetDeviceInfo client_info = test_device("CLIENT", true);
    check(net_client_create(client, &client_key, &client_info, client_hosts.c_str()), "client created");
    NetAddress address;
    net_address_from_text("127.0.0.1", port, &address);
    check(net_client_connect(client, address, false), "client connecting");

    bool host_authorized = false;
    bool host_got_message = false;
    bool client_ready = false;
    bool client_got_reply = false;
    uint32_t host_peer = NET_NO_PEER;
    for (int i = 0; i < 400 && !(host_got_message && client_got_reply); i++)
    {
        // The client: polled here, like an app's loop.
        NetClientEvent client_event;
        while (net_client_poll(client, &client_event))
        {
            if (client_event.type == NetClientEventType::ready)
            {
                client_ready = true;
                const uint8_t hello[] = {NET_MESSAGE_FIRST_APP, 'h', 'i'};
                net_client_send(client, NetChannel::control, hello, sizeof(hello));
            }
            if (client_event.type == NetClientEventType::message && client_event.size == 3 &&
                client_event.data[1] == 'o' && client_event.data[2] == 'k')
            {
                client_got_reply = true;
            }
        }
        // The host: what its thread queued, as a frontend's loop drains it.
        net_service_lock(service);
        NetAccessEvent event;
        while (net_service_next(service, &event))
        {
            if (event.type == NetAccessEventType::authorized)
            {
                host_authorized = true;
                host_peer = event.peer;
            }
            if (event.type == NetAccessEventType::message && event.size == 3 && event.data != nullptr &&
                std::memcmp(event.data + 1, "hi", 2) == 0)
            {
                host_got_message = true;
                const uint8_t reply[] = {NET_MESSAGE_FIRST_APP, 'o', 'k'};
                net_access_send(&service->access, event.peer, NetChannel::control, reply, sizeof(reply));
            }
        }
        net_service_unlock(service);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(client_ready, "the client is welcomed (the thread answered without the frontend polling)");
    check(host_authorized && host_peer != NET_NO_PEER, "the host's authorized event came through the queue");
    check(host_got_message, "a message came through the queue, its payload intact");
    check(client_got_reply, "a reply sent under the lock reached the client");

    net_client_close(client);
    for (int i = 0; i < 20; i++)
    {
        NetClientEvent client_event;
        while (net_client_poll(client, &client_event))
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    net_service_stop(service);
    check(!service->thread.joinable(), "stopped: the thread is joined");
    net_service_stop(service);   // twice is fine
    net_client_destroy(client);
    delete client;
    delete service;
    net_shutdown();
    if (g_failures == 0)
    {
        std::printf("net service: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}

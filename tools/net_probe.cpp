// net_probe: checks LAN discovery on a real network, without piccolo.
//
//   net_probe [--port N] [--seconds S]          ask the LAN who's there (broadcast)
//   net_probe [--port N] --at ADDRESS           ask one machine
//   net_probe [--port N] --listen NAME          be a listener that answers
//
// Run --listen on one machine and the plain form on another: the first one's
// name should show up. A piccolo with networking on answers like --listen.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "host_log.h"
#include "net_host.h"

static void usage()
{
    std::printf("usage: net_probe [--port N] [--seconds S] [--at ADDRESS | --listen NAME]\n");
}

static int run_listener(uint16_t port, const char* name)
{
    NetHost host;
    if (!net_host_create_listener(&host, port, name))
    {
        return 1;
    }
    std::printf("answering discovery on port %u as \"%s\" (Ctrl+C to stop)\n", static_cast<unsigned>(port),
                host.name.c_str());
    uint32_t answered = 0;
    for (;;)
    {
        NetEvent event;
        while (net_host_poll(&host, &event))
        {
            char text[64];
            net_address_to_text(event.address, text, sizeof(text));
            if (event.type == NetEventType::connected)
            {
                // Nothing is authenticated yet: say goodbye right away.
                std::printf("connection from %s, closing it\n", text);
                net_host_disconnect(&host, event.peer);
            }
        }
        if (host.discovery_requests_answered != answered)
        {
            answered = host.discovery_requests_answered;
            std::printf("answered %u discovery request(s)\n", answered);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static int run_probe(uint16_t port, double seconds, const char* at)
{
    NetHost host;
    if (!net_host_create_client(&host))
    {
        return 1;
    }
    bool sent = false;
    if (at != nullptr)
    {
        NetAddress address;
        sent = net_address_from_text(at, port, &address) && net_host_discover_at(&host, address);
    }
    else
    {
        sent = net_host_discover(&host, port);
    }
    if (!sent)
    {
        net_host_destroy(&host);
        return 1;
    }
    std::printf("asked %s on port %u, waiting %.1f s...\n", at != nullptr ? at : "the LAN", static_cast<unsigned>(port),
                seconds);
    uint32_t found = 0;
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0));
    while (std::chrono::steady_clock::now() < end)
    {
        NetEvent event;
        while (net_host_poll(&host, &event))
        {
            if (event.type == NetEventType::discovered)
            {
                char text[64];
                net_address_to_text(event.address, text, sizeof(text));
                std::printf("  \"%s\" at %s (ENet port %u)\n", event.discovered.name, text,
                            static_cast<unsigned>(event.discovered.port));
                found++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("%u found\n", found);
    net_host_destroy(&host);
    return found > 0 ? 0 : 2;
}

int main(int argc, char** argv)
{
    uint16_t port = NET_DEFAULT_PORT;
    double seconds = 1.0;
    const char* at = nullptr;
    const char* listen = nullptr;
    for (int i = 1; i < argc; i++)
    {
        bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--port") == 0 && has_value)
        {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--seconds") == 0 && has_value)
        {
            seconds = std::atof(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--at") == 0 && has_value)
        {
            at = argv[++i];
        }
        else if (std::strcmp(argv[i], "--listen") == 0 && has_value)
        {
            listen = argv[++i];
        }
        else
        {
            usage();
            return 1;
        }
    }
    if (!net_init())
    {
        return 1;
    }
    int result = listen != nullptr ? run_listener(port, listen) : run_probe(port, seconds, at);
    net_shutdown();
    return result;
}

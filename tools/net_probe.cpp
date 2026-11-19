// net_probe: checks LAN discovery on a real network, without piccolo.
//
//   net_probe [--port N] [--seconds S]          ask the LAN who's there (broadcast)
//   net_probe [--port N] --at ADDRESS           ask one machine
//   net_probe [--port N] --listen NAME          be a listener that answers
//   net_probe [--port N] --connect ADDRESS      connect, run the handshake,
//                                               send a message on each channel
//
// Run --listen on one machine and the plain form on another: the first one's
// name should show up. A piccolo with networking on answers like --listen
// (and closes a connection once its handshake is done: nobody is paired yet).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "host_log.h"
#include "net_session.h"

static void usage()
{
    std::printf("usage: net_probe [--port N] [--seconds S] [--at ADDRESS | --listen NAME | --connect ADDRESS]\n");
}

static void print_key(const char* what, const uint8_t* key)
{
    char fingerprint[32];
    net_key_fingerprint(key, fingerprint, sizeof(fingerprint));
    std::printf("%s %s\n", what, fingerprint);
}

// Messages are shown as text, anything unprintable as '.'.
static void print_message(const NetSessionEvent* event)
{
    std::printf("message on %s: %zu bytes \"", event->channel == NetChannel::control ? "control" : "input", event->size);
    for (size_t i = 0; i < event->size; i++)
    {
        uint8_t c = event->data[i];
        std::putchar(c >= 32 && c < 127 ? c : '.');
    }
    std::printf("\"\n");
}

static void send_text(NetSession* session, uint32_t peer, NetChannel channel, const char* text)
{
    std::vector<uint8_t> bytes;
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        bytes.push_back(static_cast<uint8_t>(text[i]));
    }
    net_session_send(session, peer, channel, bytes.data(), bytes.size());
}

static int run_listener(uint16_t port, const char* name)
{
    NetKeypair identity;
    NetSession* session = new NetSession();
    if (!net_keypair_generate(&identity) || !net_session_create_listener(session, port, name, &identity))
    {
        delete session;
        return 1;
    }
    std::printf("answering discovery on port %u as \"%s\" (Ctrl+C to stop)\n", static_cast<unsigned>(port),
                session->host.name.c_str());
    print_key("this device is", identity.public_key);
    uint32_t answered = 0;
    for (;;)
    {
        NetSessionEvent event;
        while (net_session_poll(session, &event))
        {
            char text[64];
            net_address_to_text(event.address, text, sizeof(text));
            if (event.type == NetSessionEventType::secured)
            {
                std::printf("handshake done with %s, ", text);
                print_key("device", event.remote_key);
            }
            else if (event.type == NetSessionEventType::message)
            {
                print_message(&event);
            }
            else if (event.type == NetSessionEventType::closed)
            {
                std::printf("%s closed (%s)\n", text, event.was_secured ? "after the handshake" : "no handshake");
            }
        }
        if (session->host.discovery_requests_answered != answered)
        {
            answered = session->host.discovery_requests_answered;
            std::printf("answered %u discovery request(s)\n", answered);
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static int run_connect(uint16_t port, double seconds, const char* at)
{
    NetKeypair identity;
    NetAddress address;
    if (!net_keypair_generate(&identity) || !net_address_from_text(at, port, &address))
    {
        return 1;
    }
    NetSession* session = new NetSession();
    uint32_t peer = NET_NO_PEER;
    if (!net_session_create_client(session, &identity) || !net_session_connect(session, address, &peer))
    {
        net_session_destroy(session);
        delete session;
        return 1;
    }
    print_key("this device is", identity.public_key);
    bool secured = false;
    bool closed = false;
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0));
    while (!closed && std::chrono::steady_clock::now() < end)
    {
        NetSessionEvent event;
        while (net_session_poll(session, &event))
        {
            if (event.type == NetSessionEventType::secured)
            {
                secured = true;
                print_key("handshake done, the other side is", event.remote_key);
                send_text(session, peer, NetChannel::control, "hello on control");
                send_text(session, peer, NetChannel::input, "hello on input");
            }
            else if (event.type == NetSessionEventType::closed)
            {
                std::printf("closed by the other side%s\n", event.was_secured ? "" : " before the handshake ended");
                closed = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    net_session_destroy(session);
    delete session;
    return secured ? 0 : 2;
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
    const char* connect = nullptr;
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
        else if (std::strcmp(argv[i], "--connect") == 0 && has_value)
        {
            connect = argv[++i];
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
    int result = 0;
    if (listen != nullptr)
    {
        result = run_listener(port, listen);
    }
    else if (connect != nullptr)
    {
        result = run_connect(port, seconds < 2.0 ? 2.0 : seconds, connect);
    }
    else
    {
        result = run_probe(port, seconds, at);
    }
    net_shutdown();
    return result;
}

// net_probe: checks LAN discovery on a real network, without piccolo.
//
//   net_probe [--port N] [--seconds S]          ask the LAN who's there (broadcast)
//   net_probe [--port N] --at ADDRESS           ask one machine
//   net_probe [--port N] --listen NAME          be a host: answer discovery,
//                                               pairing open (prints the code)
//   net_probe [--port N] --pair ADDRESS CODE    pair with a host showing CODE
//   net_probe [--port N] --connect ADDRESS      connect as an already paired
//                                               device, send a message on
//                                               each channel
//   --pad       with --connect / --pair: once in, act as a gamepad for
//               --seconds: START (joins a port), then the d-pad, A and the
//               left stick in turn, 60 states a second
//   --cores     with --connect: list the host's cores (needs FULL access)
//   --list PATH with --connect: list a folder of the host's game folders
//               ("" = the game folders themselves; net_content.h)
//   --load CORE PATH  with --connect: start a game ("" = the core alone)
//   (what the host runs is printed whenever it says: at connection and on changes)
//   --dir DIR   where net_probe keeps its device key and paired devices
//               (net_probe_identity.cfg, net_probe_devices.cfg; default ".")
//
// A piccolo with networking on is a host like --listen: CONFIG > NETWORK >
// PAIR A DEVICE shows the code for --pair.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "host_log.h"
#include "net_access.h"
#include "net_client.h"
#include "net_content.h"
#include "net_link.h"
#include "net_session.h"
#include "input_devices.h"
#include "net_input.h"
#include "net_trust.h"

static void usage()
{
    std::printf("usage: net_probe [--port N] [--seconds S] [--dir DIR]\n"
                "                 [--at ADDRESS | --listen NAME | --pair ADDRESS CODE | --connect ADDRESS]\n"
                "                 [--pad | --cores | --list PATH | --load CORE PATH]\n");
}

static void print_key(const char* what, const uint8_t* key)
{
    char fingerprint[32];
    net_key_fingerprint(key, fingerprint, sizeof(fingerprint));
    std::printf("%s %s\n", what, fingerprint);
}

// Messages are shown as text, anything unprintable as '.'.
static void print_message(NetChannel channel, const uint8_t* data, size_t size)
{
    std::printf("message on %s: %zu bytes \"", channel == NetChannel::control ? "control" : "input", size);
    for (size_t i = 0; i < size; i++)
    {
        uint8_t c = data[i];
        std::putchar(c >= 32 && c < 127 ? c : '.');
    }
    std::printf("\"\n");
}

// An application message: its type byte, then the text.
static std::vector<uint8_t> app_message(const char* text)
{
    std::vector<uint8_t> bytes;
    bytes.push_back(NET_MESSAGE_FIRST_APP);
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        bytes.push_back(static_cast<uint8_t>(text[i]));
    }
    return bytes;
}

static bool load_identity(const std::string* dir, NetKeypair* identity)
{
    std::string path = *dir + "/net_probe_identity.cfg";
    if (!net_identity_load_or_create(path.c_str(), identity))
    {
        return false;
    }
    print_key("this device is", identity->public_key);
    return true;
}

static int run_listener(uint16_t port, const char* name, const std::string* dir)
{
    NetKeypair identity;
    if (!load_identity(dir, &identity))
    {
        return 1;
    }
    NetAccess* access = new NetAccess();
    std::string devices = *dir + "/net_probe_devices.cfg";
    if (!net_access_create(access, port, name, &identity, devices.c_str()) || !net_access_open_pairing(access))
    {
        net_access_destroy(access);
        delete access;
        return 1;
    }
    std::printf("host \"%s\" on port %u, %zu paired device(s); pairing code %s (Ctrl+C to stop)\n", name,
                static_cast<unsigned>(port), access->trust.devices.size(), access->code);
    uint32_t answered = 0;
    for (;;)
    {
        NetAccessEvent event;
        while (net_access_poll(access, &event))
        {
            const char* device = event.device >= 0 ? access->trust.devices[static_cast<size_t>(event.device)].name.c_str()
                                                   : "?";
            switch (event.type)
            {
            case NetAccessEventType::authorized:
                std::printf("\"%s\" connected (paired before)\n", device);
                break;
            case NetAccessEventType::paired:
                std::printf("paired with \"%s\"\n", device);
                break;
            case NetAccessEventType::message:
                print_message(event.channel, event.data, event.size);
                break;
            case NetAccessEventType::closed:
                std::printf("\"%s\" disconnected\n", device);
                break;
            case NetAccessEventType::pairing_failed:
                std::printf("pairing failed; %s\n", access->pairing_open ? "new code" : "pairing closed");
                break;
            case NetAccessEventType::pairing_closed:
            case NetAccessEventType::none:
                break;
            }
            if (access->pairing_open && event.type == NetAccessEventType::pairing_failed)
            {
                std::printf("pairing code %s\n", access->code);
            }
        }
        if (access->session.host.discovery_requests_answered != answered)
        {
            answered = access->session.host.discovery_requests_answered;
            std::printf("answered %u discovery request(s)\n", answered);
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static const char* refusal_text(NetClientRefusal refusal)
{
    switch (refusal)
    {
    case NetClientRefusal::none:
        return "?";
    case NetClientRefusal::not_paired:
        return "not paired with this host (use --pair with its code)";
    case NetClientRefusal::pairing_closed:
        return "the host isn't pairing now";
    case NetClientRefusal::wrong_code:
        return "wrong code";
    case NetClientRefusal::unknown_host:
        return "this host's key isn't one we paired with";
    case NetClientRefusal::connection:
        return "the connection failed";
    }
    return "?";
}

static uint32_t button_bit(InputButton button)
{
    return 1u << static_cast<uint32_t>(button);
}

// The scripted pad at `t` seconds after it started: START first (a port
// joins on it), then each input in turn.
static void pad_script(double t, NetPadState* state)
{
    *state = {};
    if (t < 0.3)
    {
        return;
    }
    if (t < 0.6)
    {
        state->buttons = button_bit(InputButton::start);
        return;
    }
    double cycle = std::fmod(t - 0.6, 1.2);
    if (cycle < 0.3)
    {
        state->buttons = button_bit(InputButton::dpad_right);
    }
    else if (cycle < 0.4)
    {
        state->buttons = button_bit(InputButton::south);   // a short press
    }
    else if (cycle < 0.8)
    {
        state->axes[static_cast<uint32_t>(InputAxis::left_x)] = -32767;   // full left
    }
}

// --cores / --list / --load: one request, its pages printed as they come.
enum class ContentAsk
{
    none,
    cores,
    list,
    load,
};

struct ContentProbe
{
    ContentAsk  ask = ContentAsk::none;
    const char* path = "";
    const char* core = "";
    uint32_t    received = 0;
};

static const char* status_text(NetContentStatus status)
{
    switch (status)
    {
    case NetContentStatus::ok:
        return "ok";
    case NetContentStatus::not_found:
        return "not found";
    case NetContentStatus::refused:
        return "refused";
    case NetContentStatus::failed:
        return "failed";
    }
    return "?";
}

static void content_send(NetClient* client, ContentProbe* probe)
{
    std::vector<uint8_t> out(NET_LINK_MAX_PAYLOAD);
    size_t size = 0;
    if (probe->ask == ContentAsk::cores)
    {
        size = net_content_write_cores_request(out.data(), out.size(), 1, probe->received);
    }
    else if (probe->ask == ContentAsk::list)
    {
        size = net_content_write_list_request(out.data(), out.size(), 1, probe->received, probe->path);
    }
    else if (probe->ask == ContentAsk::load)
    {
        size = net_content_write_load_request(out.data(), out.size(), 1, probe->core, probe->path);
    }
    if (size > 0)
    {
        net_client_send(client, NetChannel::control, out.data(), size);
    }
}

// True when the answer is complete (or wrong): the probe is done.
static bool content_receive(NetClient* client, ContentProbe* probe, const uint8_t* data, size_t size)
{
    uint32_t id = 0;
    uint32_t start = 0;
    uint32_t total = 0;
    NetContentStatus status = NetContentStatus::failed;
    std::vector<NetContentCore> cores;
    std::vector<NetContentEntry> entries;
    if (net_content_read_cores_result(data, size, &id, &status, &start, &total, &cores))
    {
        for (size_t i = 0; i < cores.size(); i++)
        {
            std::printf("core %s: %s %s [%s]%s\n", cores[i].file.c_str(), cores[i].name.c_str(),
                        cores[i].version.c_str(), cores[i].extensions.c_str(),
                        (cores[i].flags & NET_CONTENT_CORE_NO_GAME) != 0 ? " (no game too)" : "");
        }
        probe->received += static_cast<uint32_t>(cores.size());
    }
    else if (net_content_read_list_result(data, size, &id, &status, &start, &total, &entries))
    {
        for (size_t i = 0; i < entries.size(); i++)
        {
            const char* kind = entries[i].kind == NetContentKind::folder
                                   ? "folder "
                                   : (entries[i].kind == NetContentKind::archive ? "archive" : "file   ");
            std::printf("%s %12llu  %s\n", kind, static_cast<unsigned long long>(entries[i].size),
                        entries[i].name.c_str());
        }
        probe->received += static_cast<uint32_t>(entries.size());
    }
    else if (net_content_read_load_result(data, size, &id, &status))
    {
        std::printf("load: %s\n", status_text(status));
        return true;
    }
    else
    {
        return false;   // something else: keep waiting
    }
    if (status != NetContentStatus::ok)
    {
        std::printf("%s\n", status_text(status));
        return true;
    }
    if (probe->received >= total || (cores.empty() && entries.empty()))
    {
        std::printf("%u in all\n", total);
        return true;
    }
    content_send(client, probe);   // the next page
    return false;
}

static int run_client(uint16_t port, double seconds, const char* at, const char* code, const std::string* dir,
                      bool pad, ContentProbe* content)
{
    NetKeypair identity;
    NetAddress address;
    if (!load_identity(dir, &identity) || !net_address_from_text(at, port, &address))
    {
        return 1;
    }
    NetClient* client = new NetClient();
    std::string devices = *dir + "/net_probe_devices.cfg";
    if (!net_client_create(client, &identity, "NET PROBE", devices.c_str()) || !net_client_connect(client, address, code))
    {
        net_client_destroy(client);
        delete client;
        return 1;
    }
    int result = 2;
    bool done = false;
    bool ready = false;
    NetPadSender sender;
    std::chrono::steady_clock::time_point pad_start;
    std::chrono::steady_clock::time_point next_state;
    uint32_t states_sent = 0;
    NetGameState game_state;
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0));
    while (!done && std::chrono::steady_clock::now() < end)
    {
        NetClientEvent event;
        while (net_client_poll(client, &event))
        {
            if (event.type == NetClientEventType::ready)
            {
                std::printf("%s \"%s\" (access: %s), ", event.newly_paired ? "paired with" : "welcomed by",
                            client->host_name, net_access_level_name(event.access));
                print_key("its key is", client->host_key);
                std::vector<uint8_t> control = app_message("hello on control");
                std::vector<uint8_t> input = app_message("hello on input");
                net_client_send(client, NetChannel::control, control.data(), control.size());
                if (!pad)
                {
                    net_client_send(client, NetChannel::input, input.data(), input.size());
                }
                result = 0;
                ready = true;
                content_send(client, content);
                pad_start = std::chrono::steady_clock::now();
                next_state = pad_start;
            }
            else if (event.type == NetClientEventType::refused)
            {
                std::printf("refused: %s\n", refusal_text(event.refusal));
                done = true;
            }
            else if (event.type == NetClientEventType::message && event.channel == NetChannel::control &&
                     net_content_read_game_state(event.data, event.size, &game_state))
            {
                if (game_state.running)
                {
                    std::printf("running: %s %s (%s)%s%s\n", game_state.core_name.c_str(),
                                game_state.core_version.c_str(), game_state.core_file.c_str(),
                                game_state.game.empty() ? "" : ", ", game_state.game.c_str());
                }
                else
                {
                    std::printf("running: nothing\n");
                }
            }
            else if (event.type == NetClientEventType::message && content->ask != ContentAsk::none &&
                     event.channel == NetChannel::control)
            {
                done = content_receive(client, content, event.data, event.size);
            }
            else if (event.type == NetClientEventType::message)
            {
                print_message(event.channel, event.data, event.size);
            }
            else if (event.type == NetClientEventType::closed)
            {
                std::printf("the host closed the connection\n");
                done = true;
            }
        }
        // The pad: one state every 1/60 s, like a frontend's frame.
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (pad && ready && now >= next_state)
        {
            NetPadState state;
            pad_script(std::chrono::duration<double>(now - pad_start).count(), &state);
            uint8_t packet[NET_INPUT_MAX_PACKET];
            size_t size = net_input_push(&sender, 0, &state, packet, sizeof(packet));
            if (net_client_send(client, NetChannel::input, packet, size))
            {
                states_sent++;
            }
            next_state += std::chrono::microseconds(16667);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pad && ready ? 2 : 10));
    }
    if (pad)
    {
        std::printf("sent %u pad states\n", states_sent);
    }
    net_client_close(client);
    for (int i = 0; i < 20; i++)   // let the goodbye go out
    {
        NetClientEvent event;
        while (net_client_poll(client, &event))
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    net_client_destroy(client);
    delete client;
    return result;
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
    const char* pair_code = nullptr;
    std::string dir = ".";
    bool pad = false;
    ContentProbe content;
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
        else if (std::strcmp(argv[i], "--pair") == 0 && i + 2 < argc)
        {
            connect = argv[++i];
            pair_code = argv[++i];
        }
        else if (std::strcmp(argv[i], "--pad") == 0)
        {
            pad = true;
        }
        else if (std::strcmp(argv[i], "--cores") == 0)
        {
            content.ask = ContentAsk::cores;
        }
        else if (std::strcmp(argv[i], "--list") == 0 && has_value)
        {
            content.ask = ContentAsk::list;
            content.path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--load") == 0 && i + 2 < argc)
        {
            content.ask = ContentAsk::load;
            content.core = argv[++i];
            content.path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--dir") == 0 && has_value)
        {
            dir = argv[++i];
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
        result = run_listener(port, listen, &dir);
    }
    else if (connect != nullptr)
    {
        result = run_client(port, seconds < 3.0 ? 3.0 : seconds, connect, pair_code, &dir, pad, &content);
    }
    else
    {
        result = run_probe(port, seconds, at);
    }
    net_shutdown();
    return result;
}

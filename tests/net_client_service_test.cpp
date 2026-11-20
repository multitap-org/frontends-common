// Checks net_client_service.h over loopback against a host on net_service.h:
// the client's thread connects and its events come through the queue;
// remote play: stream_start reaches the host, stream_info arrives, frames
// the host hands over arrive whole and exact (keyframe, then deltas), audio
// pieces are counted (not played: no device here), the picture queue's
// depth (in order, or only the newest), stream_stop stops it,
// and stopping joins the thread cleanly.
//
// Usage: frontend_net_client_service_test <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "host_fs.h"
#include "net_client_service.h"
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

constexpr uint32_t WIDTH = 64;
constexpr uint32_t HEIGHT = 48;
constexpr uint32_t SAMPLES = 534;   // a frame's audio at 32040 Hz

// Frame n: a gradient with a block that moves (deltas have something to send).
static void make_frame(std::vector<uint8_t>* pixels, uint32_t n)
{
    pixels->resize(WIDTH * HEIGHT * 2);
    for (uint32_t y = 0; y < HEIGHT; y++)
    {
        for (uint32_t x = 0; x < WIDTH; x++)
        {
            uint32_t value = (x + y * 3) & 0x1F;
            if (x >= (n * 2) % WIDTH && x < (n * 2) % WIDTH + 8 && y >= 10 && y < 18)
            {
                value = 0xF800 >> 11;
            }
            uint16_t pixel = static_cast<uint16_t>(value | (value << 11));
            (*pixels)[(y * WIDTH + x) * 2] = static_cast<uint8_t>(pixel & 0xFF);
            (*pixels)[(y * WIDTH + x) * 2 + 1] = static_cast<uint8_t>(pixel >> 8);
        }
    }
}

// The host frontend's part: what its thread queued (stream_start / stop,
// keyframe requests) as a loop drains it.
static void host_drain(NetService* service)
{
    net_service_lock(service);
    NetAccessEvent event;
    while (net_service_next(service, &event))
    {
        if (event.type != NetAccessEventType::message || event.size == 0 || event.data == nullptr)
        {
            continue;
        }
        uint8_t flags = 0;
        if (event.data[0] == NET_STREAM_START && net_stream_read_start(event.data, event.size, &flags))
        {
            net_service_set_stream(service, event.peer, flags);
        }
        else if (event.data[0] == NET_STREAM_STOP)
        {
            net_service_set_stream(service, event.peer, 0);
        }
        else if (event.data[0] == NET_STREAM_KEYFRAME_REQUEST)
        {
            net_service_request_keyframe(service, event.peer);
        }
    }
    net_service_unlock(service);
}

int main(int argc, char** argv)
{
    if (argc < 2 || !net_init())
    {
        std::printf("FAIL usage: frontend_net_client_service_test <scratch folder>\n");
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
    // Already paired, both ways, with STREAM access.
    NetTrust host_trust;
    net_trust_add(&host_trust, client_key.public_key, "CLIENT");
    host_trust.devices[0].access = NetAccessLevel::stream;
    NetTrust client_trust;
    net_trust_add(&client_trust, host_key.public_key, "HOST");
    check(net_trust_save(&host_trust, host_devices.c_str()) && net_trust_save(&client_trust, client_hosts.c_str()),
          "lists saved");

    NetService* host = new NetService();
    check(net_service_start(host, 0, "HOST", &host_key, host_devices.c_str()), "host started");
    net_service_lock(host);
    uint16_t port = net_host_port(&host->access.session.host);
    net_service_unlock(host);

    NetClientService* client = new NetClientService();
    client->stream.play_audio = false;
    check(net_client_service_start(client, &client_key, "CLIENT", client_hosts.c_str()), "client started");
    NetAddress address;
    net_address_from_text("127.0.0.1", port, &address);
    net_client_service_lock(client);
    check(net_client_connect(&client->client, address, nullptr), "client connecting");
    net_client_service_unlock(client);

    // Connect: the ready event through the client's queue.
    bool ready = false;
    for (int i = 0; i < 400 && !ready; i++)
    {
        host_drain(host);
        net_client_service_lock(client);
        NetClientEvent event;
        while (net_client_service_next(client, &event))
        {
            ready = ready || event.type == NetClientEventType::ready;
        }
        net_client_service_unlock(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(ready, "ready came through the client's queue (its thread did the handshake)");

    net_client_service_lock(client);
    check(net_client_service_set_stream(client, NET_STREAM_WANT_VIDEO | NET_STREAM_WANT_AUDIO), "stream asked for");
    net_client_service_unlock(client);
    for (int i = 0; i < 100 && !net_service_streaming(host); i++)
    {
        host_drain(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(net_service_streaming(host), "the host streams to us");

    // 60 frames, as a host's loop hands them over; each one must arrive exact.
    NetStreamInfo info;
    info.width = WIDTH;
    info.height = HEIGHT;
    info.format = FramePixelFormat::rgb565;
    info.aspect = 4.0 / 3.0;
    info.fps = 60.0;
    info.sample_rate = 32040.0;
    std::vector<uint8_t> sent;
    std::vector<int16_t> audio(SAMPLES * 2, 100);
    std::vector<uint8_t> got;
    uint32_t got_frames = 0;
    uint32_t exact = 0;
    uint32_t last_frame = 0;
    for (uint32_t n = 1; n <= 60; n++)
    {
        make_frame(&sent, n);
        net_service_submit_frame(host, sent.data(), WIDTH, HEIGHT, FramePixelFormat::rgb565, n, &info);
        net_service_submit_audio(host, audio.data(), SAMPLES, n);
        host_drain(host);
        // Wait for this frame (on loopback it comes within a few ms).
        for (int wait = 0; wait < 50; wait++)
        {
            uint32_t width = 0;
            uint32_t height = 0;
            FramePixelFormat format = FramePixelFormat::xrgb8888;
            uint32_t frame = 0;
            NetStreamInfo frame_info;
            if (net_client_service_take_frame(client, &got, &width, &height, &format, &frame, &frame_info))
            {
                got_frames++;
                last_frame = frame;
                if (frame == n && width == WIDTH && height == HEIGHT && format == FramePixelFormat::rgb565 &&
                    got.size() == sent.size() && std::memcmp(got.data(), sent.data(), sent.size()) == 0 &&
                    frame_info.sample_rate == info.sample_rate)
                {
                    exact++;
                }
                if (frame == n)
                {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    check(got_frames >= 55, "frames arrived");
    check(exact >= 55, "they arrived exact, with their stream_info");
    check(last_frame == 60, "the last one too");

    NetStreamInfo client_info;
    NetClientStreamStats stats;
    net_client_service_lock(client);
    bool has_info = net_client_service_stream_info(client, &client_info);
    net_client_service_stream_stats(client, &stats);
    net_client_service_unlock(client);
    check(has_info && client_info.width == WIDTH && client_info.fps == 60.0, "stream_info taken by the thread");
    check(stats.frames >= 55 && stats.lost == 0, "counted, none lost");
    check(stats.audio_samples >= 50ull * SAMPLES, "audio arrived");
    check(stats.bytes > 0, "bytes counted");

    // Pacing: with a queue of 3, frames that come together are all taken, in
    // order; with 1 only the newest.
    net_client_service_set_frame_queue(client, 3);
    for (uint32_t n = 61; n <= 63; n++)
    {
        make_frame(&sent, n);
        net_service_submit_frame(host, sent.data(), WIDTH, HEIGHT, FramePixelFormat::rgb565, n, &info);
        // Each one coded before the next is handed over (the host's loop runs a frame at a time).
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint32_t in_order[3] = {};
    uint32_t taken = 0;
    for (int32_t i = 0; i < 3; i++)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        FramePixelFormat format = FramePixelFormat::rgb565;
        uint32_t frame = 0;
        NetStreamInfo frame_info;
        if (net_client_service_take_frame(client, &got, &width, &height, &format, &frame, &frame_info))
        {
            in_order[taken++] = frame;
        }
    }
    check(taken == 3 && in_order[0] == 61 && in_order[1] == 62 && in_order[2] == 63, "a queue of 3: all taken, in order");
    net_client_service_set_frame_queue(client, 1);
    for (uint32_t n = 64; n <= 66; n++)
    {
        make_frame(&sent, n);
        net_service_submit_frame(host, sent.data(), WIDTH, HEIGHT, FramePixelFormat::rgb565, n, &info);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        uint32_t width = 0;
        uint32_t height = 0;
        FramePixelFormat format = FramePixelFormat::rgb565;
        uint32_t frame = 0;
        NetStreamInfo frame_info;
        bool newest = net_client_service_take_frame(client, &got, &width, &height, &format, &frame, &frame_info) &&
                      frame == 66;
        bool more = net_client_service_take_frame(client, &got, &width, &height, &format, &frame, &frame_info);
        check(newest && !more, "a queue of 1: only the newest");
    }

    // Stop: the host stops streaming, the counts start over.
    net_client_service_lock(client);
    check(net_client_service_set_stream(client, 0), "stream stopped");
    net_client_service_stream_stats(client, &stats);
    net_client_service_unlock(client);
    for (int i = 0; i < 100 && net_service_streaming(host); i++)
    {
        host_drain(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(!net_service_streaming(host), "the host stopped");
    check(stats.frames == 0, "the counts start over");

    net_client_service_lock(client);
    net_client_close(&client->client);
    net_client_service_unlock(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    net_client_service_stop(client);
    check(!client->thread.joinable(), "stopped: the thread is joined");
    net_client_service_stop(client);   // twice is fine
    net_service_stop(host);
    delete client;
    delete host;
    net_shutdown();
    if (g_failures == 0)
    {
        std::printf("net client service: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}

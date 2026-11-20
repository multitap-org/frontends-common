// Checks piccolo-share end to end over loopback: a host whose network runs
// on its own thread (net_service, with net_share_host) and a client
// (NetClient + net_share_client) paired at OWNER. The client lists the
// host's shared folders (roots with their kinds, a folder paged), gets a
// file and puts one (byte for byte, on the files channel), copies wait while
// the device streams, a busy host says so, and a device at FULL gets no
// answer at all (NetAccess drops OWNER's messages).
//
// Usage: frontend_net_share_service_test <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "host_fs.h"
#include "net_client.h"
#include "net_link.h"
#include "net_service.h"
#include "net_share_client.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

struct Side
{
    NetService*     service = nullptr;
    NetClient*      client = nullptr;
    NetShareClient* share = nullptr;
    std::vector<uint8_t> out;
    bool ready = false;
    bool closed = false;
};

// One round of the client's loop: what the host sent, then what to send.
static void client_round(Side* side, bool may_send)
{
    NetClientEvent event;
    while (net_client_poll(side->client, &event))
    {
        if (event.type == NetClientEventType::ready)
        {
            side->ready = true;
        }
        if (event.type == NetClientEventType::closed)
        {
            side->closed = true;
        }
        if (event.type == NetClientEventType::message)
        {
            net_share_client_handle(side->share, event.data, event.size);
        }
    }
    for (;;)
    {
        bool allowed = may_send && net_client_pending(side->client) < NET_SHARE_HOST_PENDING;
        NetChannel channel = NetChannel::control;
        size_t size = net_share_client_take(side->share, allowed, side->out.data(), side->out.size(), &channel);
        if (size == 0)
        {
            break;
        }
        net_client_send(side->client, channel, side->out.data(), size);
    }
    // The host's frontend drains its queue (share messages never show up there).
    net_service_lock(side->service);
    NetAccessEvent host_event;
    while (net_service_next(side->service, &host_event))
    {
        if (host_event.type == NetAccessEventType::message && host_event.size > 0 && host_event.data != nullptr &&
            host_event.data[0] >= 192)
        {
            check(false, "a share message reached the frontend's queue");
        }
    }
    net_service_unlock(side->service);
}

static bool wait_listing(Side* side, std::vector<NetShareEntry>* entries, NetShareStatus* status)
{
    std::string path;
    for (int i = 0; i < 2000; i++)
    {
        client_round(side, true);
        if (net_share_client_listing(side->share, &path, status, entries))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static bool wait_done(Side* side, NetShareClientDone* done, int rounds)
{
    for (int i = 0; i < rounds; i++)
    {
        client_round(side, true);
        if (!side->share->done.empty())
        {
            *done = side->share->done.front();
            side->share->done.erase(side->share->done.begin());
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static void make_file(const std::string* path, size_t size, uint32_t seed)
{
    std::vector<uint8_t> data(size);
    uint32_t x = seed;
    for (size_t i = 0; i < size; i++)
    {
        x = x * 1664525u + 1013904223u;
        data[i] = static_cast<uint8_t>(x >> 24);
    }
    host_fs_write_file(path->c_str(), data.data(), data.size());
}

static bool same_files(const std::string* a, const std::string* b)
{
    uint8_t* x = nullptr;
    uint8_t* y = nullptr;
    size_t x_size = 0;
    size_t y_size = 0;
    bool same = host_fs_read_file(a->c_str(), &x, &x_size) && host_fs_read_file(b->c_str(), &y, &y_size) &&
                x_size == y_size && std::memcmp(x, y, x_size) == 0;
    delete[] x;
    delete[] y;
    return same;
}

static bool has(const std::vector<NetShareEntry>* entries, const char* name, NetShareKind kind)
{
    for (size_t i = 0; i < entries->size(); i++)
    {
        if ((*entries)[i].name == name && (*entries)[i].kind == kind)
        {
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 2 || !net_init())
    {
        std::printf("FAIL usage: frontend_net_share_service_test <scratch folder>\n");
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
    // The host's shared folders.
    std::string host_games = base + "/host/roms";
    std::string host_saves = base + "/host/saves";
    std::string host_states = base + "/host/states";
    host_fs_make_directories((host_games + "/gba").c_str());
    host_fs_make_directories(host_saves.c_str());
    host_fs_make_directories(host_states.c_str());
    std::string host_game = host_games + "/gba/Metroid Fusion.gba";
    make_file(&host_game, 2 * 1024 * 1024 + 77, 11);
    for (int i = 0; i < 1500; i++)
    {
        std::string name = host_games + "/gba/many " + std::to_string(i) + ".bin";
        host_fs_write_file(name.c_str(), "x", 1);
    }
    // The client's.
    std::string client_games = base + "/client/games";
    std::string client_saves = base + "/client/saves";
    host_fs_make_directories(client_games.c_str());
    host_fs_make_directories((client_saves + "/mGBA").c_str());
    std::string client_save = client_saves + "/mGBA/Metroid Fusion.srm";
    make_file(&client_save, 131072 + 5, 12);

    std::string host_identity = base + "/host_identity.cfg";
    std::string client_identity = base + "/client_identity.cfg";
    std::string host_devices = base + "/host_devices.cfg";
    std::string client_hosts = base + "/client_hosts.cfg";
    NetKeypair host_key;
    NetKeypair client_key;
    check(net_identity_load_or_create(host_identity.c_str(), &host_key) &&
              net_identity_load_or_create(client_identity.c_str(), &client_key),
          "identities");
    NetTrust host_trust;
    net_trust_add(&host_trust, client_key.public_key, "CLIENT");
    host_trust.devices[0].access = NetAccessLevel::owner;
    NetTrust client_trust;
    net_trust_add(&client_trust, host_key.public_key, "HOST");
    check(net_trust_save(&host_trust, host_devices.c_str()) && net_trust_save(&client_trust, client_hosts.c_str()),
          "lists saved");

    Side side;
    side.out.assign(NET_LINK_MAX_PAYLOAD, 0);
    side.service = new NetService();
    check(net_service_start(side.service, 0, "HOST", &host_key, host_devices.c_str()), "service started");
    std::vector<std::string> games;
    games.push_back(host_games);
    net_service_lock(side.service);
    net_service_set_share_folders(side.service, &games, host_saves.c_str(), host_states.c_str());
    uint16_t port = net_host_port(&side.service->access.session.host);
    net_service_unlock(side.service);

    NetShareFolders client_folders;
    std::vector<std::string> client_game_paths;
    client_game_paths.push_back(client_games);
    net_share_set_folders(&client_folders, &client_game_paths, client_saves.c_str(), "");
    side.share = new NetShareClient();
    net_share_client_start(side.share, &client_folders);
    side.client = new NetClient();
    check(net_client_create(side.client, &client_key, "CLIENT", client_hosts.c_str()), "client created");
    NetAddress address;
    net_address_from_text("127.0.0.1", port, &address);
    check(net_client_connect(side.client, address, nullptr), "client connecting");
    for (int i = 0; i < 400 && !side.ready; i++)
    {
        client_round(&side, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(side.ready && side.client->access == NetAccessLevel::owner, "connected at OWNER");

    // Listing.
    std::vector<NetShareEntry> entries;
    NetShareStatus status = NetShareStatus::failed;
    net_share_client_list(side.share, "");
    check(wait_listing(&side, &entries, &status) && status == NetShareStatus::ok &&
              has(&entries, "roms", NetShareKind::games) && has(&entries, "SAVES", NetShareKind::saves) &&
              has(&entries, "STATES", NetShareKind::states),
          "roots listed with their kinds");
    net_share_client_list(side.share, "roms/gba");
    check(wait_listing(&side, &entries, &status) && status == NetShareStatus::ok && entries.size() == 1501 &&
              has(&entries, "Metroid Fusion.gba", NetShareKind::file),
          "a big folder arrives whole (paged)");
    net_share_client_list(side.share, "roms/../..");
    check(wait_listing(&side, &entries, &status) && status == NetShareStatus::refused, "an escape is refused");

    // Get: the host's game into our games.
    net_share_client_copy(side.share, false, "games/Metroid Fusion.gba", "roms/gba/Metroid Fusion.gba",
                          2 * 1024 * 1024 + 77, 0);
    NetShareClientDone done;
    check(wait_done(&side, &done, 20000) && done.status == NetShareStatus::ok, "get finished");
    std::string got = client_games + "/Metroid Fusion.gba";
    check(same_files(&host_game, &got), "got byte for byte");

    // Put: our save into the host's SAVES (its core folder made there).
    net_share_client_copy(side.share, true, "SAVES/mGBA/Metroid Fusion.srm", "SAVES/mGBA/Metroid Fusion.srm", 0, 0);
    check(wait_done(&side, &done, 20000) && done.status == NetShareStatus::ok, "put finished");
    std::string put = host_saves + "/mGBA/Metroid Fusion.srm";
    check(same_files(&client_save, &put), "put byte for byte");
    net_share_client_copy(side.share, true, "SAVES/mGBA/Metroid Fusion.srm", "SAVES/mGBA/Metroid Fusion.srm", 0, 0);
    check(wait_done(&side, &done, 20000) && done.status == NetShareStatus::exists, "no overwrite unless asked");

    // While the device streams, the copy waits (and goes once it stops).
    net_service_lock(side.service);
    uint32_t peer = NET_NO_PEER;
    for (uint32_t i = 0; i < NET_MAX_PEERS; i++)
    {
        if (side.service->access.peers[i].state == NetAccessPeerState::authorized)
        {
            peer = i;
        }
    }
    net_service_set_stream(side.service, peer, NET_STREAM_WANT_AUDIO);
    net_service_unlock(side.service);
    std::string second = client_games + "/second.gba";
    net_share_client_copy(side.share, false, "games/second.gba", "roms/gba/Metroid Fusion.gba", 2 * 1024 * 1024 + 77, 0);
    for (int i = 0; i < 300; i++)
    {
        client_round(&side, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    NetShareProgress progress;
    NetShareClientCopy copy;
    uint32_t queued = 0;
    net_share_client_progress(side.share, &progress, &copy, &queued);
    check(progress.role == NetShareRole::receiver && progress.done == 0 && side.share->done.empty(),
          "the copy waits while the device streams");
    net_service_lock(side.service);
    net_service_set_stream(side.service, peer, 0);
    net_service_unlock(side.service);
    check(wait_done(&side, &done, 20000) && done.status == NetShareStatus::ok && same_files(&host_game, &second),
          "and goes once it stops");

    // At FULL, the host doesn't answer OWNER's messages at all.
    net_service_lock(side.service);
    net_access_set_level(&side.service->access, 0, NetAccessLevel::full);
    uint32_t dropped_before = side.service->access.dropped_by_access;
    net_service_unlock(side.service);
    for (int i = 0; i < 100; i++)
    {
        client_round(&side, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    net_share_client_list(side.share, "");
    check(!wait_listing(&side, &entries, &status), "no listing at FULL");
    net_service_lock(side.service);
    check(side.service->access.dropped_by_access > dropped_before, "the request was dropped by access");
    net_service_unlock(side.service);

    net_client_close(side.client);
    for (int i = 0; i < 20; i++)
    {
        client_round(&side, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    net_share_client_disconnected(side.share);
    net_share_client_stop(side.share);
    net_service_stop(side.service);
    net_client_destroy(side.client);
    delete side.client;
    delete side.share;
    delete side.service;
    net_shutdown();
    if (g_failures == 0)
    {
        std::printf("net share service: all checks passed\n");
        return 0;
    }
    return 1;
}

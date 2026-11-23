// Guest links end to end on loopback, against the real `beacon` binary
// (argv[1]): a host (NetAccess) opens a link with two seats, and guests
// (NetClient, never paired) join it over UDP and over WebRTC through the
// beacon's signaling. Then the rules: the third guest is turned away (full),
// a guest may only send what the link allows (no headless, no pad at WATCH),
// the host sends a guest nothing private, a level change reaches the guests,
// KICK sticks for that guest's key, a new link (NEW) keeps the connected
// guests and kills the old token, more seats let one more in, a wrong token
// is closed without a word, the card goes up while the room lives and not
// after, and END drops everyone with its reason.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_fs.h"
#include "net_access.h"
#include "net_client.h"
#include "net_guest.h"
#if defined(MULTITAP_TEST_HTTP)
#include "net_http.h"
#endif

extern char** environ;

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition)
    {
        g_failures++;
    }
}

// One guest: a NetClient of its own (its own device key, no paired hosts).
struct Guest
{
    NetClient client;
    bool      ready = false;
    bool      refused = false;
    bool      closed = false;
    bool      access_changed = false;
    NetClientRefusal refusal = NetClientRefusal::none;
    NetGuestLevel level = NetGuestLevel::watch;
    NetGuestBye   bye = NetGuestBye::none;
    uint8_t   home_key[NET_KEY_SIZE] = {};
};

struct World
{
    NetAccess* host = nullptr;
    std::vector<Guest*> guests;
    uint32_t joined = 0;
    uint32_t full = 0;
    uint32_t guest_closed = 0;
    uint32_t guest_inputs = 0;
    uint32_t guest_controls = 0;
    uint32_t other_events = 0;   // anything about a guest that isn't one of the above
    uint32_t last_guest_peer = NET_NO_PEER;
    std::vector<uint32_t> guest_peers;
};

static void reset_guest_flags(Guest* guest)
{
    guest->ready = false;
    guest->refused = false;
    guest->closed = false;
    guest->access_changed = false;
    guest->refusal = NetClientRefusal::none;
    guest->bye = NetGuestBye::none;
}

static void pump(World* world)
{
    NetAccessEvent event;
    while (net_access_poll(world->host, &event))
    {
        switch (event.type)
        {
        case NetAccessEventType::guest_joined:
            world->joined++;
            world->last_guest_peer = event.peer;
            break;
        case NetAccessEventType::guest_full:
            world->full++;
            break;
        case NetAccessEventType::closed:
            if (event.guest)
            {
                world->guest_closed++;
            }
            break;
        case NetAccessEventType::message:
            if (event.guest && event.channel == NetChannel::input)
            {
                world->guest_inputs++;
            }
            else if (event.guest && event.channel == NetChannel::control)
            {
                world->guest_controls++;
            }
            break;
        case NetAccessEventType::pairing_requested:
        case NetAccessEventType::authorized:
        case NetAccessEventType::paired:
            world->other_events++;
            break;
        default:
            break;
        }
    }
    for (size_t i = 0; i < world->guests.size(); i++)
    {
        Guest* guest = world->guests[i];
        NetClientEvent guest_event;
        while (net_client_poll(&guest->client, &guest_event))
        {
            if (guest_event.type == NetClientEventType::ready)
            {
                guest->ready = guest_event.guest;
                guest->level = guest_event.guest_level;
            }
            else if (guest_event.type == NetClientEventType::access)
            {
                guest->access_changed = true;
                guest->level = guest_event.guest_level;
            }
            else if (guest_event.type == NetClientEventType::refused)
            {
                guest->refused = true;
                guest->refusal = guest_event.refusal;
            }
            else if (guest_event.type == NetClientEventType::closed)
            {
                guest->closed = true;
                guest->bye = guest_event.guest_bye;
            }
        }
    }
}

static bool pump_until(World* world, const bool* flag, int milliseconds)
{
    for (int i = 0; i < milliseconds / 5 && !*flag; i++)
    {
        pump(world);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return *flag;
}

static void pump_for(World* world, int milliseconds)
{
    for (int i = 0; i < milliseconds / 5; i++)
    {
        pump(world);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

static bool wait_for_listener(uint16_t port, int milliseconds)
{
    for (int i = 0; i < milliseconds / 20; i++)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return false;
        }
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool connected = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        close(fd);
        if (connected)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

static Guest* new_guest(World* world, const char* name, const std::string& signal_url, const std::string& stun)
{
    Guest* guest = new Guest();
    NetKeypair key;
    net_keypair_generate(&key);
    std::memcpy(guest->home_key, key.public_key, NET_KEY_SIZE);
    net_client_create(&guest->client, &key, name, "");
    net_keypair_wipe(&key);
    net_client_set_signal(&guest->client, signal_url.c_str(), stun.c_str());
    world->guests.push_back(guest);
    return guest;
}

static void delete_guests(World* world)
{
    for (size_t i = 0; i < world->guests.size(); i++)
    {
        net_client_destroy(&world->guests[i]->client);
        delete world->guests[i];
    }
    world->guests.clear();
}

// Joins `link`: UDP through the beacon (`udp`), or WebRTC through its
// signaling only (like a browser).
static bool join(Guest* guest, const NetGuestLink* link, const NetAddress* beacon, bool udp)
{
    reset_guest_flags(guest);
    return net_client_join_guest(&guest->client, link, beacon, udp ? 1 : 0);
}

static void test_guest_links(const char* beacon_path, const std::string& dir)
{
    uint16_t port = static_cast<uint16_t>(46000 + (getpid() % 3000) * 2);
    uint16_t signal_port = static_cast<uint16_t>(port + 1);
    std::string port_text = std::to_string(port);
    std::string signal_text = std::to_string(signal_port);
    const char* beacon_argv[] = {beacon_path, "--bind", "127.0.0.1", "--port", port_text.c_str(), "--secondary-port",
                                 "0", "--signal-port", signal_text.c_str(), "--stats-interval", "0", "--log-level",
                                 "warning", "--public-url", "http://127.0.0.1", nullptr};
    pid_t beacon = 0;
    if (posix_spawn(&beacon, beacon_path, nullptr, nullptr, const_cast<char**>(beacon_argv), environ) != 0)
    {
        check(false, "beacon started");
        return;
    }
    check(wait_for_listener(signal_port, 10000), "beacon listening");
    std::string url = "ws://127.0.0.1:" + signal_text + "/signal";
    std::string stun = "stun:127.0.0.1:" + port_text;
    NetAddress beacon_address;
    net_address_from_text("127.0.0.1", port, &beacon_address);

    World world;
    world.host = new NetAccess();
    NetKeypair host_key;
    net_keypair_generate(&host_key);
    std::string trust = dir + "/guest_host_devices.cfg";
    host_fs_remove_file(trust.c_str());
    check(net_access_create(world.host, 0, "LIVING ROOM", &host_key, trust.c_str()), "host created");
    world.host->accept_pairing = false;   // guests don't need pairing open
    net_access_set_beacon(world.host, &beacon_address, 1);
    check(net_access_set_signal(world.host, url.c_str(), stun.c_str()), "host signaling on");

    NetGuestLink link;
    check(net_guest_link_make(&link), "a link");
    net_access_guest_open(world.host, &link, 2, NetGuestLevel::play);
    bool reachable = false;
    for (int i = 0; i < 2000 && !reachable; i++)
    {
        pump(&world);
        reachable = net_access_beacon_status(world.host) == NetBeaconStatus::registered &&
                    net_signal_rooms_joined(&world.host->signal);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(reachable, "the host's link is on the beacon (UDP id and guest room)");
    check(net_access_guest_ready(world.host), "the link reports ready (its room joined)");

    // Two guests: one over UDP, one over WebRTC only.
    Guest* a = new_guest(&world, "PIXEL 8", url, stun);
    check(join(a, &link, &beacon_address, true), "guest A joins over UDP");
    check(pump_until(&world, &a->ready, 10000), "guest A is in");
    check(a->level == NetGuestLevel::play, "at WATCH + PLAY");
    check(std::memcmp(a->client.session.identity.public_key, a->home_key, NET_KEY_SIZE) != 0,
          "with a throwaway key, not its own");
    uint32_t peer_a = world.last_guest_peer;
    check(world.joined == 1 && net_access_guest(world.host, peer_a) != nullptr &&
              std::strcmp(net_access_guest(world.host, peer_a)->guest_name, "PIXEL 8") == 0,
          "the host sees it, by its name");
    check(world.host->trust.devices.empty() && !host_fs_exists(trust.c_str()), "nothing saved on the host");
    check(a->client.trust.devices.empty(), "nothing saved on the guest");

    Guest* b = new_guest(&world, "FIREFOX", url, stun);
    check(join(b, &link, &beacon_address, false), "guest B joins over WebRTC (no UDP, like a browser)");
    check(pump_until(&world, &b->ready, 20000), "guest B is in");
    uint32_t peer_b = world.last_guest_peer;
    check(world.joined == 2 && net_access_guest_count(world.host) == 2, "two seats taken");

    Guest* c = new_guest(&world, "GALAXY S22", url, stun);
    check(join(c, &link, &beacon_address, true), "guest C tries");
    check(pump_until(&world, &c->refused, 10000), "guest C is turned away");
    check(c->refusal == NetClientRefusal::guest_full, "as session full");
    check(world.full == 1, "the host counted it");

    // What a guest may send.
    uint8_t pad[8] = {2, 0, 0, 0, 1, 0, 0, 0};
    net_client_send(&a->client, NetChannel::input, pad, sizeof(pad));
    uint8_t start[2] = {70, 3};   // NET_STREAM_START, video + audio
    net_client_send(&a->client, NetChannel::control, start, sizeof(start));
    uint8_t headless[2] = {76, 1};   // NET_STREAM_GO_HEADLESS
    net_client_send(&a->client, NetChannel::control, headless, sizeof(headless));
    uint8_t settings[1] = {136};     // settings_request (FULL)
    net_client_send(&a->client, NetChannel::control, settings, sizeof(settings));
    uint32_t dropped = world.host->dropped_by_access;
    pump_for(&world, 500);
    check(world.guest_inputs == 1, "a PLAY guest's pad reaches the host");
    check(world.guest_controls == 1, "stream start does; headless and settings don't");
    check(world.host->dropped_by_access == dropped + 2, "both dropped by the host");

    // What the host may send a guest.
    uint8_t game_state[2] = {16, 0};
    uint8_t devices[1] = {142};
    check(net_access_send(world.host, peer_a, NetChannel::control, game_state, sizeof(game_state)),
          "the host may send a guest what runs");
    check(!net_access_send(world.host, peer_a, NetChannel::control, devices, sizeof(devices)),
          "but not its devices");

    // WATCH: the guests are told, and pads stop.
    net_access_guest_set_level(world.host, NetGuestLevel::watch);
    check(pump_until(&world, &a->access_changed, 3000) && a->level == NetGuestLevel::watch, "guest A hears WATCH");
    check(pump_until(&world, &b->access_changed, 3000) && b->level == NetGuestLevel::watch, "guest B too");
    net_client_send(&a->client, NetChannel::input, pad, sizeof(pad));
    pump_for(&world, 300);
    check(world.guest_inputs == 1, "a WATCH guest's pad doesn't reach the host");
    net_access_guest_set_level(world.host, NetGuestLevel::play);
    pump_for(&world, 200);

    // KICK: closed with its reason, and the same guest can't come back.
    net_access_guest_kick(world.host, peer_b);
    check(pump_until(&world, &b->closed, 5000), "guest B is closed");
    check(b->bye == NetGuestBye::kicked, "it knows it was kicked");
    check(std::memcmp(b->client.session.identity.public_key, b->home_key, NET_KEY_SIZE) == 0,
          "its own key is back once the connection is over");
    check(join(b, &link, &beacon_address, true), "guest B tries again");
    check(pump_until(&world, &b->refused, 10000), "refused");
    check(b->refusal == NetClientRefusal::guest_kicked, "as kicked");

    // The freed seat takes guest C.
    check(join(c, &link, &beacon_address, true), "guest C tries again");
    check(pump_until(&world, &c->ready, 10000), "guest C takes the freed seat");

    // NEW: the old token stops working, the connected guests stay.
    NetGuestLink link2;
    check(net_guest_link_make(&link2), "a new link");
    uint32_t closed_before = world.guest_closed;
    net_access_guest_open(world.host, &link2, 2, NetGuestLevel::play);
    pump_for(&world, 1500);
    check(world.guest_closed == closed_before && !a->closed && !c->closed, "the guests in stay");
    Guest* d = new_guest(&world, "TABLET", url, stun);
    check(join(d, &link, &beacon_address, true), "guest D comes with the old link");
    check(pump_until(&world, &d->refused, 15000), "refused");
    check(d->refusal == NetClientRefusal::guest_link, "the link doesn't work any more");
    check(join(d, &link2, &beacon_address, true), "guest D with the new link");
    check(pump_until(&world, &d->refused, 10000) && d->refusal == NetClientRefusal::guest_full,
          "full: both seats are taken");
    net_access_guest_set_seats(world.host, 3);
    check(join(d, &link2, &beacon_address, true), "one more seat: guest D again");
    check(pump_until(&world, &d->ready, 10000), "guest D is in");

    // A wrong token for the right room: closed without a word.
    Guest* e = new_guest(&world, "STRANGER", url, stun);
    NetGuestLink forged = link2;
    forged.mac_key[0] ^= 1;
    uint32_t joined_before = world.joined;
    uint32_t full_before = world.full;
    check(join(e, &forged, &beacon_address, true), "a guest with a wrong token");
    check(pump_until(&world, &e->refused, 10000), "refused");
    check(e->refusal == NetClientRefusal::guest_link, "as a link that doesn't work");
    check(world.joined == joined_before && world.full == full_before && world.other_events == 0,
          "the host reported nothing (no dialog)");

#if defined(MULTITAP_TEST_HTTP)
    // The card: up while the room lives, served on the join page.
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                0, 0, 1, 0, 0, 0, 0, 224, 8, 2, 0, 0, 0, 1, 2, 3, 4};
    std::vector<uint8_t> body;
    check(net_guest_card_body(&link2, "Zelda", png.data(), png.size(), &body), "a card body");
    std::string card_url = "http://127.0.0.1:" + signal_text + "/join/" + link2.room + "/card";
    uint32_t status = 0;
    char error[256] = {};
    check(net_http_post(card_url.c_str(), "application/octet-stream", body.data(), body.size(), &status, error,
                        sizeof(error)) && status == 204,
          "the card goes up (204)");
    std::string page_path = dir + "/guest_page.html";
    std::string page_url = "http://127.0.0.1:" + signal_text + "/join/" + link2.room;
    check(net_http_download(page_url.c_str(), page_path.c_str(), 1 << 20, error, sizeof(error)), "the join page");
    std::string page;
    uint8_t* page_data = nullptr;
    size_t page_size = 0;
    if (host_fs_read_file(page_path.c_str(), &page_data, &page_size))
    {
        page.assign(reinterpret_cast<const char*>(page_data), page_size);
        delete[] page_data;
    }
    check(page.find("<meta property=\"og:title\" content=\"Zelda\">") != std::string::npos,
          "it carries the card's title");
    std::string old_card = "http://127.0.0.1:" + signal_text + "/join/" + link.room + "/card";
    std::vector<uint8_t> old_body;
    net_guest_card_body(&link, "Zelda", png.data(), png.size(), &old_body);
    check(net_http_post(old_card.c_str(), "application/octet-stream", old_body.data(), old_body.size(), &status,
                        error, sizeof(error)) && status == 404,
          "the old link's room is gone: no card for it (404)");
#endif

    // END: everyone goes, told why; the link is dead.
    net_access_guest_close(world.host);
    check(pump_until(&world, &a->closed, 5000) && a->bye == NetGuestBye::ended, "guest A: the session ended");
    check(pump_until(&world, &c->closed, 5000) && c->bye == NetGuestBye::ended, "guest C too");
    check(pump_until(&world, &d->closed, 5000) && d->bye == NetGuestBye::ended, "guest D too");
    check(net_access_guest_count(world.host) == 0, "no guests left");
    check(join(a, &link2, &beacon_address, true), "guest A tries the link after the end");
    check(pump_until(&world, &a->refused, 15000) && a->refusal == NetClientRefusal::guest_link,
          "the link doesn't work any more");
#if defined(MULTITAP_TEST_HTTP)
    check(net_http_post(card_url.c_str(), "application/octet-stream", body.data(), body.size(), &status, error,
                        sizeof(error)) && status == 404,
          "no card after the end (404)");
#endif

    delete_guests(&world);
    net_access_destroy(world.host);
    delete world.host;
    net_keypair_wipe(&host_key);
    kill(beacon, SIGTERM);
    int status_code = 0;
    waitpid(beacon, &status_code, 0);
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("FAIL usage: net_guest_link_test <beacon binary> <scratch folder>\n");
        return 1;
    }
    host_fs_make_directories(argv[2]);
    net_init();
#if defined(MULTITAP_TEST_HTTP)
    net_http_init("net_guest_link_test");
#endif
    test_guest_links(argv[1], argv[2]);
#if defined(MULTITAP_TEST_HTTP)
    net_http_shutdown();
#endif
    net_shutdown();
    if (g_failures == 0)
    {
        std::printf("net guest link: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

// Checks net_content.h (games from a client) and, with archives,
// net_content_folders.h: every message written and read back, damaged
// messages refused, results paged so every entry arrives once, and the game
// folders resolved and listed without ever leaving them: "..", symlinks
// pointing out, backslashes, drive letters and unknown folders are refused.
//
// Usage: frontend_net_content_test <tests/data folder> <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>   // symlink
#endif

#include "host_fs.h"
#include "net_content.h"
#include "net_link.h"
#if defined(MULTITAP_TEST_ARCHIVES)
#include "net_content_folders.h"
#endif

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

static void test_requests()
{
    uint8_t out[NET_LINK_MAX_PAYLOAD];
    uint32_t id = 0;
    uint32_t start = 0;
    std::string path;
    std::string core;

    size_t size = net_content_write_cores_request(out, sizeof(out), 7, 40);
    check(size == 9 && out[0] == 128, "cores_request written");
    check(net_content_read_cores_request(out, size, &id, &start) && id == 7 && start == 40, "cores_request read");
    check(!net_content_read_cores_request(out, size - 1, &id, &start), "cores_request cut refused");

    size = net_content_write_list_request(out, sizeof(out), 9, 3, "ROMS/snes/G\xC3\xA2me.sfc");
    check(size > 0 && net_content_read_list_request(out, size, &id, &start, &path) && id == 9 && start == 3 &&
              path == "ROMS/snes/G\xC3\xA2me.sfc",
          "list_request read");
    out[size] = 0;
    check(!net_content_read_list_request(out, size + 1, &id, &start, &path), "list_request with a byte more refused");
    out[12] = 0;   // a NUL inside the path ("ROMS" starts at 11)
    check(!net_content_read_list_request(out, size, &id, &start, &path), "list_request with a NUL refused");

    size = net_content_write_load_request(out, sizeof(out), 11, "snes9x_libretro.so", "ROMS/a.zip/b.sfc");
    check(size > 0 && net_content_read_load_request(out, size, &id, &core, &path) && id == 11 &&
              core == "snes9x_libretro.so" && path == "ROMS/a.zip/b.sfc",
          "load_request read");
    check(!net_content_read_list_request(out, size, &id, &start, &path), "load_request isn't a list_request");

    NetContentStatus status = NetContentStatus::ok;
    size = net_content_write_load_result(out, sizeof(out), 11, NetContentStatus::refused);
    check(net_content_read_load_result(out, size, &id, &status) && id == 11 && status == NetContentStatus::refused,
          "load_result read");
    out[5] = 9;
    check(!net_content_read_load_result(out, size, &id, &status), "unknown status refused");

    NetGameState state;
    state.running = true;
    state.core_file = "snes9x_libretro.so";
    state.core_name = "Snes9x";
    state.core_version = "1.63";
    state.game = "Chrono Trigger (USA)";
    size = net_content_write_game_state(out, sizeof(out), &state);
    NetGameState read;
    check(size > 0 && net_content_read_game_state(out, size, &read) && read.running &&
              read.core_file == "snes9x_libretro.so" && read.core_name == "Snes9x" && read.core_version == "1.63" &&
              read.game == "Chrono Trigger (USA)",
          "game_state read");
    out[1] = 2;
    check(!net_content_read_game_state(out, size, &read), "game_state with a bad flag refused");
    NetGameState idle;
    size = net_content_write_game_state(out, sizeof(out), &idle);
    check(net_content_read_game_state(out, size, &read) && !read.running && read.game.empty(), "idle game_state read");

    std::string long_path(NET_CONTENT_PATH_MAX + 1, 'a');
    check(net_content_write_list_request(out, sizeof(out), 1, 0, long_path.c_str()) == 0, "too long a path not written");
    check(net_content_write_cores_request(out, 8, 1, 0) == 0, "too small a buffer not written");
}

// Asks for pages of at most `capacity` bytes until every entry came, as a
// client does.
static void test_paging()
{
    std::vector<NetContentEntry> entries;
    for (uint32_t i = 0; i < 500; i++)
    {
        NetContentEntry entry;
        entry.kind = i % 3 == 0 ? NetContentKind::folder : (i % 3 == 1 ? NetContentKind::file : NetContentKind::archive);
        entry.size = 1000ull * i;
        entry.name = "entry number " + std::to_string(i);
        entries.push_back(entry);
    }
    uint8_t out[1000];
    std::vector<NetContentEntry> received;
    uint32_t pages = 0;
    bool ok = true;
    while (ok && received.size() < entries.size() && pages < 1000)
    {
        size_t size = net_content_write_list_result(out, sizeof(out), 5, NetContentStatus::ok, &entries,
                                                    static_cast<uint32_t>(received.size()));
        uint32_t id = 0;
        uint32_t start = 0;
        uint32_t total = 0;
        NetContentStatus status = NetContentStatus::failed;
        std::vector<NetContentEntry> page;
        ok = size > 0 && net_content_read_list_result(out, size, &id, &status, &start, &total, &page) && id == 5 &&
             status == NetContentStatus::ok && total == 500 && start == received.size() && !page.empty();
        for (size_t i = 0; i < page.size(); i++)
        {
            received.push_back(page[i]);
        }
        pages++;
    }
    check(ok, "list pages read");
    check(pages > 1, "a big list takes several pages");
    bool same = received.size() == entries.size();
    for (size_t i = 0; same && i < entries.size(); i++)
    {
        same = received[i].name == entries[i].name && received[i].kind == entries[i].kind &&
               received[i].size == entries[i].size;
    }
    check(same, "every entry arrived once, in order");

    // Past the end: an empty page, still ok.
    size_t size = net_content_write_list_result(out, sizeof(out), 5, NetContentStatus::ok, &entries, 500);
    uint32_t id = 0;
    uint32_t start = 0;
    uint32_t total = 0;
    NetContentStatus status = NetContentStatus::failed;
    std::vector<NetContentEntry> page;
    check(net_content_read_list_result(out, size, &id, &status, &start, &total, &page) && page.empty() && total == 500,
          "a page past the end is empty");

    // A failed result carries nothing.
    size = net_content_write_list_result(out, sizeof(out), 6, NetContentStatus::not_found, &entries, 0);
    check(net_content_read_list_result(out, size, &id, &status, &start, &total, &page) &&
              status == NetContentStatus::not_found && total == 0 && page.empty(),
          "a failed result is empty");

    std::vector<NetContentCore> cores;
    for (uint32_t i = 0; i < 40; i++)
    {
        NetContentCore core;
        core.flags = i == 3 ? NET_CONTENT_CORE_NO_GAME : 0;
        core.file = "core" + std::to_string(i) + "_libretro.so";
        core.name = "Core " + std::to_string(i);
        core.version = "1." + std::to_string(i);
        core.extensions = "sfc|smc|zip";
        cores.push_back(core);
    }
    std::vector<NetContentCore> got;
    std::vector<NetContentCore> core_page;
    ok = true;
    while (ok && got.size() < cores.size())
    {
        size = net_content_write_cores_result(out, sizeof(out), 2, NetContentStatus::ok, &cores,
                                              static_cast<uint32_t>(got.size()));
        ok = net_content_read_cores_result(out, size, &id, &status, &start, &total, &core_page) && !core_page.empty();
        for (size_t i = 0; i < core_page.size(); i++)
        {
            got.push_back(core_page[i]);
        }
    }
    check(ok && got.size() == 40 && got[3].flags == NET_CONTENT_CORE_NO_GAME && got[39].file == "core39_libretro.so" &&
              got[39].extensions == "sfc|smc|zip",
          "cores pages read");
}

// ---------------------------------------------------------------------------
// Game folders
// ---------------------------------------------------------------------------

#if defined(MULTITAP_TEST_ARCHIVES)

static bool write_text(const std::string* path, const char* text)
{
    return host_fs_write_file(path->c_str(), text, std::strlen(text));
}

static bool has_entry(const std::vector<NetContentEntry>* entries, const char* name, NetContentKind kind)
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

static NetContentStatus resolve(const NetContentFolders* folders, const char* path)
{
    std::string real_path;
    NetContentKind kind = NetContentKind::file;
    return net_content_resolve(folders, path, &real_path, &kind);
}

static void test_folders(const char* data_folder, const char* scratch)
{
    // scratch/games/{snes/{a.sfc, b.sfc}, .hidden, files.zip, folder.zip/inside.sfc}
    // scratch/secret/key.txt                    outside every game folder
    // scratch/other/games/x.sfc                 a second folder called "games"
    // Absolute, so the symlinks below point where they should from any folder.
    char absolute[4096] = {};
    check(host_fs_make_directories(scratch) && host_fs_canonical(scratch, absolute, sizeof(absolute)),
          "scratch folder made");
    std::string root = absolute;
    std::string games = root + "/games";
    std::string secret = root + "/secret/key.txt";
    std::string a = games + "/snes/a.sfc";
    std::string b = games + "/snes/B.sfc";
    std::string hidden = games + "/.hidden";
    std::string folder_zip = games + "/folder.zip/inside.sfc";
    std::string other = root + "/other/games/x.sfc";
    check(write_text(&a, "a") && write_text(&b, "b") && write_text(&hidden, "h") && write_text(&secret, "secret") &&
              write_text(&folder_zip, "f") && write_text(&other, "x"),
          "scratch files written");
    std::string zip_from = std::string(data_folder) + "/files.zip";
    uint8_t* zip = nullptr;
    size_t zip_size = 0;
    if (host_fs_read_file(zip_from.c_str(), &zip, &zip_size))
    {
        std::string zip_to = games + "/files.zip";
        check(host_fs_write_file(zip_to.c_str(), zip, zip_size), "zip copied");
        delete[] zip;
    }
    else
    {
        check(false, "tests/data/files.zip read");
    }
#if !defined(_WIN32)
    // A link inside a game folder that leads out of it, and one that stays in.
    std::string out_link = games + "/escape";
    std::string in_link = games + "/snes_again";
    std::remove(out_link.c_str());
    std::remove(in_link.c_str());
    std::string secret_folder = root + "/secret";
    std::string snes_folder = games + "/snes";
    check(symlink(secret_folder.c_str(), out_link.c_str()) == 0, "symlink out made");
    check(symlink(snes_folder.c_str(), in_link.c_str()) == 0, "symlink in made");
#endif

    NetContentFolders folders;
    std::vector<std::string> paths;
    paths.push_back(games + "/");                  // a trailing '/'
    paths.push_back(root + "/other/games");        // the same name
    paths.push_back(root + "/not there");          // left out
    net_content_set_folders(&folders, &paths);
    check(folders.roots.size() == 2 && folders.roots[0].name == "games" && folders.roots[1].name == "games 2",
          "roots named, a missing one left out");

    std::vector<NetContentEntry> entries;
    check(net_content_list(&folders, "", &entries) == NetContentStatus::ok && entries.size() == 2 &&
              entries[0].name == "games" && entries[1].name == "games 2",
          "the roots listed");
    check(net_content_list(&folders, "games", &entries) == NetContentStatus::ok, "a root listed");
    check(has_entry(&entries, "snes", NetContentKind::folder), "a folder listed");
    check(has_entry(&entries, "files.zip", NetContentKind::archive), "an archive listed as one");
    check(has_entry(&entries, "folder.zip", NetContentKind::folder), "a folder called .zip is a folder");
    check(!has_entry(&entries, ".hidden", NetContentKind::file), "hidden files left out");
#if !defined(_WIN32)
    check(!has_entry(&entries, "escape", NetContentKind::folder), "a link leading out left out");
    check(has_entry(&entries, "snes_again", NetContentKind::folder), "a link staying inside listed");
#endif
    check(!entries.empty() && entries[0].kind == NetContentKind::folder, "folders first");

    check(net_content_list(&folders, "games/snes", &entries) == NetContentStatus::ok && entries.size() == 2 &&
              entries[0].name == "a.sfc" && entries[1].name == "B.sfc" && entries[0].size == 1,
          "files sorted ignoring case, with sizes");
    check(net_content_list(&folders, "games/files.zip", &entries) == NetContentStatus::ok &&
              has_entry(&entries, "Game One.sfc", NetContentKind::file) &&
              has_entry(&entries, "sub", NetContentKind::folder),
          "an archive listed like a folder");
    check(net_content_list(&folders, "games/files.zip/sub", &entries) == NetContentStatus::ok && entries.size() == 1 &&
              entries[0].name == "G\xC3\xA2me Two.md",
          "a folder inside an archive listed");
    check(net_content_list(&folders, "games/folder.zip", &entries) == NetContentStatus::ok && entries.size() == 1 &&
              entries[0].name == "inside.sfc",
          "a folder called .zip listed as a folder");

    std::string real_path;
    NetContentKind kind = NetContentKind::folder;
    check(net_content_resolve(&folders, "games/snes/a.sfc", &real_path, &kind) == NetContentStatus::ok &&
              kind == NetContentKind::file && real_path.size() > 10 &&
              real_path.compare(real_path.size() - 10, 10, "snes/a.sfc") == 0,
          "a file resolved");
    check(net_content_resolve(&folders, "games/files.zip/sub/G\xC3\xA2me Two.md", &real_path, &kind) ==
                  NetContentStatus::ok &&
              kind == NetContentKind::file && real_path.find("files.zip#sub/G\xC3\xA2me Two.md") != std::string::npos,
          "a file inside an archive resolved to an archive path");
    check(net_content_resolve(&folders, "games/files.zip", &real_path, &kind) == NetContentStatus::ok &&
              kind == NetContentKind::archive,
          "an archive resolved as one");
    check(net_content_resolve(&folders, "games 2/x.sfc", &real_path, &kind) == NetContentStatus::ok,
          "the second root resolved");

    // Ways out, and things that aren't there.
    check(resolve(&folders, "games/../secret/key.txt") == NetContentStatus::refused, "'..' refused");
    check(resolve(&folders, "games/snes/../../secret/key.txt") == NetContentStatus::refused, "deep '..' refused");
    check(resolve(&folders, "games/./snes") == NetContentStatus::refused, "'.' refused");
    check(resolve(&folders, "games//snes") == NetContentStatus::refused, "empty part refused");
    check(resolve(&folders, "games/snes/") == NetContentStatus::refused, "trailing '/' refused");
    check(resolve(&folders, "/games/snes") == NetContentStatus::refused, "leading '/' refused");
    check(resolve(&folders, "games\\..\\secret") == NetContentStatus::refused, "backslashes refused");
    check(resolve(&folders, "C:/Windows") == NetContentStatus::refused, "a drive refused");
    check(resolve(&folders, "games/snes/a.sfc:stream") == NetContentStatus::refused, "':' refused");
    check(resolve(&folders, "secret/key.txt") == NetContentStatus::not_found, "a folder that isn't a root not found");
    check(resolve(&folders, "games/nothing.sfc") == NetContentStatus::not_found, "a missing file not found");
    check(resolve(&folders, "games/files.zip/nothing") == NetContentStatus::not_found,
          "a missing file inside an archive not found");
    check(resolve(&folders, "games/files.zip/../../secret/key.txt") == NetContentStatus::refused,
          "'..' after an archive refused");
#if !defined(_WIN32)
    check(resolve(&folders, "games/escape/key.txt") == NetContentStatus::refused, "a link leading out refused");
    check(resolve(&folders, "games/escape") == NetContentStatus::refused, "the link itself refused");
    check(resolve(&folders, "games/snes_again/a.sfc") == NetContentStatus::ok, "a link staying inside resolved");
#endif
    check(net_content_list(&folders, "games/snes/a.sfc", &entries) == NetContentStatus::refused,
          "listing a file refused");

    // The host's whole answer, over pages that reuse the listing.
    uint8_t request[256];
    uint8_t out[NET_LINK_MAX_PAYLOAD];
    size_t size = net_content_write_list_request(request, sizeof(request), 3, 0, "games/snes");
    size_t answer = net_content_answer_list(&folders, request, size, out, sizeof(out));
    uint32_t id = 0;
    uint32_t start = 0;
    uint32_t total = 0;
    NetContentStatus status = NetContentStatus::failed;
    check(answer > 0 && net_content_read_list_result(out, answer, &id, &status, &start, &total, &entries) && id == 3 &&
              status == NetContentStatus::ok && total == 2 && entries.size() == 2,
          "list answered");
    size = net_content_write_list_request(request, sizeof(request), 4, 1, "games/snes");
    answer = net_content_answer_list(&folders, request, size, out, sizeof(out));
    check(answer > 0 && net_content_read_list_result(out, answer, &id, &status, &start, &total, &entries) &&
              start == 1 && entries.size() == 1 && entries[0].name == "B.sfc",
          "second page from the kept listing");
    size = net_content_write_list_request(request, sizeof(request), 5, 0, "games/../secret");
    answer = net_content_answer_list(&folders, request, size, out, sizeof(out));
    check(answer > 0 && net_content_read_list_result(out, answer, &id, &status, &start, &total, &entries) &&
              status == NetContentStatus::refused && entries.empty(),
          "a way out answered refused");
    check(net_content_answer_list(&folders, out, answer, out, sizeof(out)) == 0, "not a request: no answer");
}

#endif

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: frontend_net_content_test <tests/data folder> <scratch folder>\n");
        return 2;
    }
    test_requests();
    test_paging();
#if defined(MULTITAP_TEST_ARCHIVES)
    test_folders(argv[1], argv[2]);
#else
    (void)argv;   // the folders are only for the archive checks
#endif
    if (g_failures == 0)
    {
        std::printf("net content: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}

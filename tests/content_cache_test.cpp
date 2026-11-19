// Checks content_cache.h with tests/data/lzma2_solid.7z: a file is extracted
// once under its own name, found again the second time, and trimming removes
// the least recently used copies but never the one being kept.
//
// Usage: frontend_content_cache_test <tests/data folder> <scratch folder>
// (the scratch folder is emptied by a CTest fixture first).
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "content_cache.h"
#include "host_fs.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static uint64_t file_size(const char* path)
{
    SDL_PathInfo info = {};
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE ? info.size : 0;
}

static bool ends_with(const std::string* text, const char* suffix)
{
    size_t length = std::strlen(suffix);
    return text->size() >= length && text->compare(text->size() - length, length, suffix) == 0;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf("usage: %s <tests/data folder> <scratch folder>\n", argv[0]);
        return 2;
    }
    std::string archive = std::string(argv[1]) + "/lzma2_solid.7z";
    std::string cache = std::string(argv[2]) + "/cache";

    std::string game;
    check(content_cache_extract(cache.c_str(), archive.c_str(), "Game One.sfc", &game), "extracts");
    check(ends_with(&game, "/Game One.sfc"), "keeps the file's own name");
    check(file_size(game.c_str()) == 100000, "extracted size");
    std::string folder = game.substr(0, game.size() - std::strlen("/Game One.sfc"));
    check(host_fs_exists((folder + "/last_used").c_str()), "last_used written");

    // A changed copy of the right size would be taken as is (the key covers
    // the archive), so the second call must not extract: prove it by
    // replacing the copy with other bytes of the same size.
    std::string marker(100000, 'x');
    check(host_fs_write_file(game.c_str(), marker.data(), marker.size()), "replace the copy");
    std::string again;
    check(content_cache_extract(cache.c_str(), archive.c_str(), "Game One.sfc", &again) && again == game,
          "found again, same path");
    uint8_t* data = nullptr;
    size_t size = 0;
    check(host_fs_read_file(game.c_str(), &data, &size) && size == 100000 && data[0] == 'x',
          "second call reused the copy");
    delete[] data;

    // A copy of the wrong size (cut short) is extracted again.
    check(host_fs_write_file(game.c_str(), "short", 5), "cut the copy");
    check(content_cache_extract(cache.c_str(), archive.c_str(), "Game One.sfc", &again), "re-extracts");
    check(file_size(game.c_str()) == 100000, "re-extracted at full size");

    std::string second;
    check(content_cache_extract(cache.c_str(), archive.c_str(), "sub/G\xC3\xA2me Two.md", &second),
          "second file extracts");
    check(second != game, "different files, different folders");
    check(!content_cache_extract(cache.c_str(), archive.c_str(), "nope.bin", &again), "missing file fails");
    std::string missing = std::string(argv[2]) + "/missing.7z";
    check(!content_cache_extract(cache.c_str(), missing.c_str(), "Game One.sfc", &again), "missing archive fails");

    // A folder that isn't ours is never touched.
    std::string foreign = cache + "/not-a-key";
    check(host_fs_write_file((foreign + "/file").c_str(), "keep", 4), "foreign folder");

    // Over budget: the game is kept (running), the other copy goes.
    content_cache_trim(cache.c_str(), 1, game.c_str());
    check(file_size(game.c_str()) == 100000, "trim keeps the running game");
    check(!host_fs_exists(second.c_str()), "trim removes the other copy");
    check(host_fs_exists((foreign + "/file").c_str()), "trim leaves unknown folders alone");

    // Within budget: nothing goes.
    content_cache_trim(cache.c_str(), 1024 * 1024, nullptr);
    check(file_size(game.c_str()) == 100000, "within budget, nothing removed");

    // No keep: everything of ours goes.
    content_cache_trim(cache.c_str(), 0, nullptr);
    check(!host_fs_exists(folder.c_str()), "trim to 0 removes the folder");

    if (g_failures == 0)
    {
        std::printf("content cache: all checks passed\n");
        return 0;
    }
    std::printf("content cache: %d check(s) failed\n", g_failures);
    return 1;
}

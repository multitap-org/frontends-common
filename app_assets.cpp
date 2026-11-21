#include "app_assets.h"

#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "host_fs.h"

const char* app_assets_apk_stamp()
{
    const char* stamp = SDL_getenv("MULTITAP_APK_STAMP");
    return stamp != nullptr ? stamp : "";
}

#if defined(SDL_PLATFORM_ANDROID)

// Every file under the asset folder `folder` to destination/folder/. SDL
// knows the APK's whole tree, so the glob lists subfolders' files too.
static bool copy_asset_folder(const char* folder, const std::string* destination, uint32_t* copied)
{
    int count = 0;
    char** names = SDL_GlobDirectory(folder, nullptr, 0, &count);
    if (names == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Assets: no %s in the APK (%s)", folder, SDL_GetError());
        return true;   // (an APK without that folder: nothing to copy)
    }
    bool ok = true;
    for (int i = 0; i < count; i++)
    {
        std::string from = std::string(folder) + "/" + names[i];
        SDL_PathInfo info = {};
        if (!SDL_GetPathInfo(from.c_str(), &info) || info.type != SDL_PATHTYPE_FILE)
        {
            continue;   // folders are made as their files are written
        }
        std::string to = *destination + from;
        size_t slash = to.rfind('/');
        size_t size = 0;
        void* data = SDL_LoadFile(from.c_str(), &size);
        if (data == nullptr || (slash != std::string::npos && !host_fs_make_directories(to.substr(0, slash).c_str())) ||
            !host_fs_write_file(to.c_str(), data, size))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Assets: can't copy %s to %s", from.c_str(), to.c_str());
            ok = false;
        }
        else
        {
            (*copied)++;
        }
        SDL_free(data);
    }
    SDL_free(names);
    return ok;
}

bool app_assets_unpack(const char* const* folders, uint32_t count, const char* destination, const char* stamp)
{
    std::string root = destination;
    std::string stamp_path = root + "stamp";
    bool have_stamp = stamp != nullptr && stamp[0] != '\0';
    if (have_stamp)
    {
        uint8_t* data = nullptr;
        size_t size = 0;
        bool same = false;
        if (host_fs_read_file(stamp_path.c_str(), &data, &size))
        {
            same = size == std::strlen(stamp) && std::memcmp(data, stamp, size) == 0;
            delete[] data;
        }
        if (same)
        {
            return true;
        }
    }
    bool ok = true;
    uint32_t copied = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        ok = copy_asset_folder(folders[i], &root, &copied) && ok;
    }
    if (ok && have_stamp && !host_fs_write_file(stamp_path.c_str(), stamp, std::strlen(stamp)))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Assets: can't write %s", stamp_path.c_str());
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Assets: %u files copied to %s", copied, root.c_str());
    return ok;
}

#else

bool app_assets_unpack(const char* const* folders, uint32_t count, const char* destination, const char* stamp)
{
    (void)folders;
    (void)count;
    (void)destination;
    (void)stamp;
    return true;
}

#endif

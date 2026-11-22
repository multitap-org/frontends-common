#include "app_paths.h"

#include <cstring>

#include <SDL3/SDL.h>

#include "host_fs.h"
#include "settings_file.h"

#if defined(_WIN32)
#include <windows.h>   // GetLogicalDrives (the drives, in app_paths_storage_volumes)
#endif

static bool is_separator(char c)
{
    return c == '/' || c == '\\';
}

// base + "/" + name, without doubling a separator `base` already ends with.
static std::string join(const char* base, const char* name)
{
    std::string result = base;
    while (result.size() > 1 && is_separator(result.back()))
    {
        result.pop_back();
    }
    result += "/";
    result += name;
    return result;
}

bool app_paths_xdg_root(const char* xdg_data_home, const char* home, const char* program_name, std::string* root)
{
    // The XDG spec: a relative path in XDG_DATA_HOME is invalid and ignored.
    if (xdg_data_home != nullptr && xdg_data_home[0] == '/')
    {
        *root = join(xdg_data_home, program_name);
        return true;
    }
    if (home != nullptr && home[0] != '\0')
    {
        std::string data_home = join(home, ".local/share");
        *root = join(data_home.c_str(), program_name);
        return true;
    }
    return false;
}

void app_paths_fill(AppPaths* paths)
{
    const char* root = paths->root.c_str();
    paths->settings = join(root, "settings");
    paths->cores = join(root, "cores");
    paths->saves = join(root, "saves");
    paths->states = join(root, "states");
    paths->system = join(root, "system");
    paths->screenshots = join(root, "screenshots");
    paths->cache = join(root, "cache");
}

// `value` with a leading `prefix` (followed by a separator or the end)
// replaced by `replacement`. False if it doesn't start with it.
static bool replace_prefix(std::string* value, const char* prefix, const char* replacement)
{
    size_t length = std::strlen(prefix);
    if (length == 0 || value->compare(0, length, prefix) != 0)
    {
        return false;
    }
    if (value->size() > length && !is_separator((*value)[length]))
    {
        return false;   // "/home/u/.host-libretro-old/..." is not inside the old folder
    }
    value->replace(0, length, replacement);
    return true;
}

// Copies the old "last used" file (key=value lines) to the new one, pointing
// paths that were inside the old folder at the new folder.
static bool migrate_last_used(const char* from, const char* to, const char* old_root, const char* new_root)
{
    SettingsFile file = {};
    if (!settings_file_load(&file, from))
    {
        return false;
    }
    int32_t global = settings_file_find_section(&file, "");
    if (global >= 0)
    {
        SettingsFileSection* section = &file.sections[static_cast<size_t>(global)];
        for (size_t i = 0; i < section->entries.size(); i++)
        {
            std::string* value = &section->entries[i].value;
            if (!replace_prefix(value, old_root, new_root))
            {
                replace_prefix(value, "~/.host-libretro", new_root);
            }
        }
    }
    return settings_file_save(&file, to, nullptr);
}

bool app_paths_setup(const AppPaths* paths, const char* old_root, const char* old_last_used_name)
{
    bool ok = true;
    const char* root = paths->root.c_str();

    if (old_root != nullptr && old_root[0] != '\0' && host_fs_exists(old_root) &&
        !host_fs_exists(paths->settings.c_str()))
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "First run with %s: copying cores, saves and states from %s (the old folder is left as it is)",
                    root, old_root);
        static const char* const FOLDERS[] = {"cores", "saves", "states"};
        const std::string* destinations[] = {&paths->cores, &paths->saves, &paths->states};
        for (size_t i = 0; i < sizeof(FOLDERS) / sizeof(FOLDERS[0]); i++)
        {
            std::string from = join(old_root, FOLDERS[i]);
            if (host_fs_exists(from.c_str()) && !host_fs_copy(from.c_str(), destinations[i]->c_str()))
            {
                ok = false;
            }
        }
        if (old_last_used_name != nullptr)
        {
            std::string from = join(old_root, old_last_used_name);
            std::string to = join(paths->settings.c_str(), "last_used.cfg");
            if (host_fs_exists(from.c_str()) && !migrate_last_used(from.c_str(), to.c_str(), old_root, root))
            {
                ok = false;
            }
        }
        if (!ok)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Copying from %s was not complete (see above)", old_root);
        }
    }

    const std::string* folders[] = {&paths->root,  &paths->settings, &paths->cores,
                                    &paths->saves, &paths->states,   &paths->system};
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
    {
        if (!host_fs_make_directories(folders[i]->c_str()))
        {
            ok = false;
        }
    }
    return ok;
}

const char* app_paths_home_folder()
{
#if defined(__ANDROID__)
    const char* home = SDL_getenv("MULTITAP_HOME_DIR");   // set by the Java activity
    return home != nullptr && home[0] != '\0' ? home : nullptr;
#else
    return SDL_GetUserFolder(SDL_FOLDER_HOME);
#endif
}

void app_paths_storage_volumes(std::vector<std::string>* volumes)
{
    volumes->clear();
#if defined(__ANDROID__)
    // Every volume is mounted under /storage: "emulated" holds the internal
    // shared storage (/sdcard, HOME) and "self" links to it; the rest are SD
    // cards and USB drives. SDL_GlobDirectory, not host_fs_list_directory:
    // no log line when /storage can't be read (no storage access).
    int count = 0;
    char** names = SDL_GlobDirectory("/storage", nullptr, 0, &count);
    if (names == nullptr)
    {
        return;
    }
    for (int i = 0; i < count; i++)
    {
        if (std::strcmp(names[i], "emulated") == 0 || std::strcmp(names[i], "self") == 0)
        {
            continue;
        }
        std::string path = join("/storage", names[i]);
        if (host_fs_is_directory(path.c_str()))
        {
            volumes->push_back(path);
        }
    }
    SDL_free(names);
#elif defined(_WIN32)
    // Windows: the drives ("C:/", "D:/"...), the ones ready now (an empty
    // card reader or DVD drive isn't a folder).
    DWORD drives = GetLogicalDrives();
    for (int letter = 0; letter < 26; letter++)
    {
        if ((drives & (1u << letter)) == 0)
        {
            continue;
        }
        char path[4] = {static_cast<char>('A' + letter), ':', '/', '\0'};
        if (host_fs_is_directory(path))
        {
            volumes->push_back(path);
        }
    }
#endif
}

bool app_paths_init(AppPaths* paths, const char* program_name, const char* old_last_used_name)
{
    *paths = AppPaths{};
    const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);   // ends with a separator; SDL owns it

#if defined(_WIN32)
    // Portable: the folder the executable is in.
    const char* base = SDL_GetBasePath();
    if (base == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Can't find the program's folder: %s", SDL_GetError());
        return false;
    }
    paths->root = base;
    host_fs_forward_slashes(paths->root.data());   // "C:\Users\me\piccolo\" -> "C:/Users/me/piccolo/"
    while (paths->root.size() > 3 && is_separator(paths->root.back()))
    {
        paths->root.pop_back();
    }
#elif defined(__ANDROID__)
    // Set by the Java activity before SDL_main runs (see PiccoloActivity.java).
    const char* data_dir = SDL_getenv("MULTITAP_DATA_DIR");
    if (data_dir != nullptr && data_dir[0] != '\0')
    {
        paths->root = join(data_dir, program_name);
    }
    else
    {
        char* preferences = SDL_GetPrefPath("multitap", program_name);
        if (preferences == nullptr)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Can't find a folder for %s: %s", program_name, SDL_GetError());
            return false;
        }
        paths->root = preferences;
        SDL_free(preferences);
        while (paths->root.size() > 1 && is_separator(paths->root.back()))
        {
            paths->root.pop_back();
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No folder from the activity: using the app's private folder (deleted on uninstall)");
    }
#elif defined(__APPLE__)
    if (home == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Can't find the home folder: %s", SDL_GetError());
        return false;
    }
    std::string support = join(home, "Library/Application Support");
    paths->root = join(support.c_str(), program_name);
#else
    if (!app_paths_xdg_root(SDL_getenv("XDG_DATA_HOME"), SDL_getenv("HOME"), program_name, &paths->root))
    {
        // No HOME (some sandboxes, Android): the platform's own writable folder.
        char* preferences = SDL_GetPrefPath("multitap", program_name);
        if (preferences == nullptr)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Can't find a folder for %s: %s", program_name, SDL_GetError());
            return false;
        }
        paths->root = preferences;
        SDL_free(preferences);
        while (paths->root.size() > 1 && is_separator(paths->root.back()))
        {
            paths->root.pop_back();
        }
    }
#endif

    app_paths_fill(paths);
#if defined(__ANDROID__)
    // Cores can't be loaded from shared storage (noexec): the private folder.
    const char* internal = SDL_GetAndroidInternalStoragePath();
    if (internal != nullptr)
    {
        paths->cores = join(internal, "cores");
    }
    // Extracted games and downloads aren't the user's files: the app's own
    // cache, off shared storage (FUSE is slower for big files, and the media
    // scanner indexes everything written there). Same partition, same space.
    const char* cache = SDL_GetAndroidCachePath();
    if (cache != nullptr)
    {
        paths->cache = join(cache, program_name);
    }
#endif
    std::string old_root = home != nullptr ? join(home, ".host-libretro") : std::string();
    bool ok = app_paths_setup(paths, old_root.c_str(), old_last_used_name);
#if defined(__ANDROID__)
    // The folder is on shared storage (/sdcard or Android/media): keep state
    // thumbnails and screenshots out of the gallery apps.
    std::string no_media = join(paths->root.c_str(), ".nomedia");
    if (ok && !host_fs_exists(no_media.c_str()) && !host_fs_write_file(no_media.c_str(), "", 0))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Can't write %s", no_media.c_str());
    }
#endif
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s folder: %s", program_name, paths->root.c_str());
    return ok;
}

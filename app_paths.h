#pragma once

// Where a program keeps its files: ONE folder per program, so moving it to
// another machine is moving one folder.
//
//   Linux (and other Unix): $XDG_DATA_HOME/<program>, default
//                           ~/.local/share/<program>. Everything in it, not
//                           split across XDG config/data/state.
//   Windows:                next to the executable (portable).
//   macOS:                  ~/Library/Application Support/<program>.
//   Android:                $MULTITAP_DATA_DIR/<program>: the activity sets it to
//                           /sdcard with storage access, else to the app's
//                           Android/media folder (no permission needed, browsable
//                           with file managers, deleted on uninstall). Not set: the
//                           app's private folder. A .nomedia file keeps it out of
//                           gallery apps. cores/ is always in the app's private
//                           storage: shared storage is mounted noexec, so cores
//                           (.so files) can't be loaded from it. cache/ is the
//                           app's own cache folder (<cache>/<program>).
//
// Inside it: settings/, cores/, saves/, states/, system/, cache/. The settings may
// later point some of them elsewhere; these are the defaults.
// <program> is "piccolo" for piccolo and "host-libretro" for the test frontend.
//
// Migration (one time): before these folders existed, everything lived in
// ~/.host-libretro. If that exists and the program has never run here yet
// (no settings/ folder: only the program creates it, while the build may
// already have put the program and the test core in), its cores/, saves/ and
// states/ are copied over. Copied, not moved: the old folder is left for the
// user to delete. Files already in the new folder are never overwritten.

#include <string>
#include <vector>

struct AppPaths
{
    std::string root;       // the program folder
    std::string settings;   // <root>/settings
    std::string cores;      // <root>/cores
    std::string saves;      // <root>/saves
    std::string states;     // <root>/states
    std::string system;     // <root>/system (BIOS and other files cores ask for)
    std::string cache;      // <root>/cache (files extracted from archives, content_cache.h; safe to delete)
};

// The old folder's "last used" file (e.g. "piccolo.cfg") is copied to
// settings/last_used.cfg, with paths into the old folder rewritten to the new
// one. nullptr = nothing to carry over.
//
// Finds the program folder, migrates if needed, creates the folders.
// False (logged) if the folder can't be found or created.
bool app_paths_init(AppPaths* paths, const char* program_name, const char* old_last_used_name);

// The user's own folder, where browsing for files starts: the home folder on
// desktop (SDL_GetUserFolder), the shared storage on Android (/sdcard, passed
// by the Java activity as MULTITAP_HOME_DIR: SDL has no user folders there).
// nullptr if unknown.
const char* app_paths_home_folder();

// Other storage the user can browse, as folders: on Android the SD cards and
// USB drives mounted now (/storage/XXXX-XXXX; readable with storage access),
// on Windows the drives (C:/, D:/... the ones ready); read again at every call
// so a drive plugged in later shows up. Empty on other desktops (and when
// nothing is mounted or readable).
void app_paths_storage_volumes(std::vector<std::string>* volumes);

// --- The pieces of app_paths_init, separate so they can be tested ---------

// The Linux rule. `xdg_data_home` is ignored unless it's an absolute path (as
// the XDG spec says); then `home` + "/.local/share" is used. Either may be
// nullptr. False if neither gives a folder.
bool app_paths_xdg_root(const char* xdg_data_home, const char* home, const char* program_name, std::string* root);

// Fills the subfolder paths from paths->root.
void app_paths_fill(AppPaths* paths);

// Migrates from `old_root` when it exists and paths->settings doesn't, then
// creates every folder. `old_root` may be nullptr (nothing to migrate).
bool app_paths_setup(const AppPaths* paths, const char* old_root, const char* old_last_used_name);

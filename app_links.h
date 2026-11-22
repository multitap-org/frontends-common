#pragma once

// The app's link scheme (piccolo://...): a link clicked in a browser or a
// chat opens the app, or reaches the copy already running (Andrés,
// 2026-11-22: links to come, such as joining a game from the lobby; the way
// RetroArch's Discord integration registered discord-<id>://).
//
// Who opens the app with a link:
//   Windows  HKCU\Software\Classes\<scheme> (app_links_register, per user,
//            no administrator): "<program>" "<link>".
//   Linux    the .desktop file: MimeType=x-scheme-handler/<scheme>, Exec=... %u
//            (the Flatpak exports it; a package installs it).
//   Android  an intent filter (the link arrives as an argument, or as a
//            dropped file while running: app_links_push).
//   macOS    CFBundleURLTypes in the bundle (none yet); the link would come as
//            an Apple event, which SDL doesn't pass on.
//
// One copy takes the links (Windows and Linux): the first one listens on a
// local socket (AF_UNIX, also on Windows 10+) in a per-user folder. A later
// launch with a link hands it over there and quits without opening a window;
// one without a link runs as usual (and doesn't listen: the first copy does).
// A socket left by a copy that crashed is found dead and taken over.
//
// Nothing here blocks the frame: app_links_next polls.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr size_t APP_LINKS_MAX = 2048;   // the longest link taken (bytes)

struct AppLinks
{
    std::string scheme;                // "piccolo"
    std::string socket_path;           // where this copy listens ("" = it doesn't)
    intptr_t    listener = -1;         // that socket (-1 = none)
    std::vector<std::string> pending;  // links not taken by app_links_next yet
};

// Whether `text` is a link of `scheme` ("piccolo://...", the scheme in any
// case), at most APP_LINKS_MAX bytes, without control characters.
bool app_links_is_link(const char* text, const char* scheme);

// The first of the program's arguments (argv[1] on) that is a link of
// `scheme`, or nullptr.
const char* app_links_from_args(int argc, char** argv, const char* scheme);

// At start, before the window. `link` (from the arguments, or nullptr) is
// queued for app_links_next. Returns false when another copy runs and took
// `link`: the caller quits. Otherwise true (listening when it could).
bool app_links_start(AppLinks* links, const char* scheme, const char* link);
void app_links_stop(AppLinks* links);

// A link that came another way (dropped on the window, Android's intent).
// Ignored if it isn't one of the scheme.
void app_links_push(AppLinks* links, const char* link);

// The next link, queued or handed over by a later launch. Once a frame.
bool app_links_next(AppLinks* links, std::string* link);

// Windows: points the scheme at this program for this user, unless it does
// already. Elsewhere nothing to do here (true). False if it couldn't.
bool app_links_register(const char* scheme, const char* app_name);

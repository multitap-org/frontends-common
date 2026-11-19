#pragma once

// The host's side of net_content.h: its game folders as the virtual paths a
// client sees, resolved and listed without ever leaving them. Needs the
// archives (archive.h): built only when frontend_common has them (piccoletto,
// a client only, doesn't).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net_content.h"

struct NetContentRoot
{
    std::string name;   // what the client sees ("ROMS", "ROMS 2")
    std::string path;   // canonical (host_fs_canonical)
};

struct NetContentFolders
{
    std::vector<NetContentRoot> roots;

    // The last folder listed, kept for its next pages.
    std::string                  listed_path;
    std::vector<NetContentEntry> listed;
};

// The game folders from the settings. Folders that don't exist now are left
// out (a disk that isn't mounted); call again when the setting changes.
void net_content_set_folders(NetContentFolders* folders, const std::vector<std::string>* paths);

// A virtual path -> the host's path: a file system path, or "a.7z#dir/x.sfc"
// for something inside an archive (archive.h). `kind` says what it is
// ("" is a folder: the roots). Anything but ok leaves both untouched.
NetContentStatus net_content_resolve(const NetContentFolders* folders, const char* path, std::string* real_path,
                                     NetContentKind* kind);

// What's in a virtual folder (or archive): folders first, then files, each
// sorted ignoring case. Hidden entries (".name") and symlinks that lead out
// of the game folders are left out.
NetContentStatus net_content_list(const NetContentFolders* folders, const char* path,
                                  std::vector<NetContentEntry>* entries);

// The host's whole answer to a list_request: lists the folder (start 0, or
// another path than last time) or reuses the last listing (the next pages),
// and writes the list_result. Returns its size; 0 if the request isn't one.
size_t net_content_answer_list(NetContentFolders* folders, const uint8_t* request, size_t size, uint8_t* out,
                               size_t capacity);

#pragma once

// Files extracted from archives (archive.h), kept so loading the same game
// again doesn't extract it again (a 7z solid block can take seconds).
// Read-only copies: nothing is ever written back into an archive, so cores
// must keep their saves in the saves folder, as libretro expects.
//
// Layout: <cache>/<key>/<file name>, plus <cache>/<key>/last_used (the
// time it was last handed out, for trimming). The file keeps its own name
// ("Chrono Trigger (USA).sfc") because cores name their saves after it. The
// key (16 hex digits) is a hash of the archive's path, size and modification
// time and the file's name, size and CRC: a changed archive gets a new key
// and its old copy just ages out.

#include <cstdint>
#include <string>

// Extracts `name` from `archive_path` into the cache (or finds it there
// already) and gives the extracted file's path. Creates `cache_directory`.
bool content_cache_extract(const char* cache_directory, const char* archive_path, const char* name,
                           std::string* out_path);

// Removes the least recently used copies until the cache holds at most
// `max_bytes`. `keep_path` (may be nullptr) is never removed: the running
// game's copy. Folders it doesn't recognize are left alone.
void content_cache_trim(const char* cache_directory, uint64_t max_bytes, const char* keep_path);

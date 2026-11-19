#pragma once

// Zip and 7z archives as read-only folders: list what's inside, extract one
// file. Used to load games straight from archives (the frontend extracts the
// file into its content cache, content_cache.h) and to browse them in file
// pickers.
//
// Paths into an archive use RetroArch's convention: the archive's path, '#',
// then the file's path inside it with '/' separators:
//
//   /home/ana/roms/snes.7z#Chrono Trigger (USA).sfc
//   /home/ana/roms/dos.zip#games/DOOM/DOOM.EXE
//
// Backends (deps/, both read through SDL_IOStream, so UTF-8 paths work on
// Windows too):
//   zip  miniz (MIT). Names are taken as stored: UTF-8 when the archive says
//        so, else usually plain ASCII (old CP437 names with accents show as
//        they are).
//   7z   7-Zip's C code, the LZMA SDK (public domain): LZMA, LZMA2 and the
//        BCJ / delta filters, which covers 7z files as people make them
//        (7-Zip's defaults). PPMd isn't built: such files are refused.
//        A file is extracted by decoding its whole solid block into memory
//        (the SDK has no streaming 7z decoder), so a block larger than
//        ARCHIVE_7Z_MAX_BLOCK is refused with a message instead of
//        exhausting memory.
//
// Every failure is logged with the archive's path.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 1 GiB: far above any single cartridge or disc image packed on its own, and
// what a 32-bit build can still allocate. Huge solid sets (a whole system in
// one solid block) hit it; recompressing them non-solid (7z -ms=off) fixes it.
constexpr uint64_t ARCHIVE_7Z_MAX_BLOCK = 1024ull * 1024ull * 1024ull;

enum class ArchiveFormat
{
    none,
    zip,
    seven_zip,
};

struct ArchiveEntry
{
    std::string name;            // path inside the archive, '/' separators ("games/DOOM/DOOM.EXE")
    uint64_t    size = 0;        // uncompressed
    uint32_t    crc32 = 0;       // of the uncompressed data, when has_crc32
    bool        has_crc32 = false;
    bool        is_directory = false;
};

// By the file's extension (".zip", ".7z", any case). Doesn't open the file.
ArchiveFormat archive_format(const char* path);

// Everything inside, in the archive's order. Folders appear only when the
// archive stores them; callers derive folders from the files' paths.
bool archive_list(const char* archive_path, std::vector<ArchiveEntry>* entries);

// Extracts the file `name` (as archive_list gives it) to `out_path`. The data
// goes to "<out_path>.tmp" first, renamed once complete and checked against
// the stored CRC, so `out_path` is never left half-written.
bool archive_extract(const char* archive_path, const char* name, const char* out_path);

// Splits "a/b.7z#dir/x.sfc" into "a/b.7z" and "dir/x.sfc". The split is at the
// first ".zip#" or ".7z#" (any case); the part after '#' may be empty (the
// archive's top folder). False, with both outputs untouched, if `path` isn't
// a path into an archive.
bool archive_split_path(const char* path, std::string* archive_path, std::string* name);

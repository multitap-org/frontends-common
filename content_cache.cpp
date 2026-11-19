#include "content_cache.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL3/SDL.h>

#include "archive.h"
#include "host_fs.h"

static const char* LAST_USED_NAME = "last_used";
constexpr size_t   KEY_LENGTH = 16;   // hex digits of a 64-bit hash

// FNV-1a, 64 bits: a cache key, not security. Every field is followed by a
// separator so ("ab", "c") and ("a", "bc") hash differently.
constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
constexpr uint64_t FNV_PRIME  = 1099511628211ull;

static uint64_t hash_bytes(uint64_t hash, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++)
    {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

static uint64_t hash_text(uint64_t hash, const char* text)
{
    return hash_bytes(hash, text, std::strlen(text) + 1);   // with its terminating zero
}

static uint64_t hash_number(uint64_t hash, uint64_t value)
{
    uint8_t bytes[8] = {};
    for (size_t i = 0; i < 8; i++)
    {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));   // fixed byte order on every platform
    }
    return hash_bytes(hash, bytes, sizeof(bytes));
}

// Writes the current time into <folder>/last_used.
static void mark_used(const std::string* folder)
{
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    char text[32] = {};
    int length = std::snprintf(text, sizeof(text), "%" PRId64 "\n", static_cast<int64_t>(now));
    std::string path = *folder + "/" + LAST_USED_NAME;
    host_fs_write_file(path.c_str(), text, length > 0 ? static_cast<size_t>(length) : 0);
}

bool content_cache_extract(const char* cache_directory, const char* archive_path, const char* name,
                           std::string* out_path)
{
    SDL_PathInfo archive_info = {};
    if (!SDL_GetPathInfo(archive_path, &archive_info) || archive_info.type != SDL_PATHTYPE_FILE)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Content cache: no archive at %s", archive_path);
        return false;
    }
    std::vector<ArchiveEntry> entries;
    if (!archive_list(archive_path, &entries))
    {
        return false;
    }
    const ArchiveEntry* entry = nullptr;
    for (size_t i = 0; i < entries.size() && entry == nullptr; i++)
    {
        if (!entries[i].is_directory && entries[i].name == name)
        {
            entry = &entries[i];
        }
    }
    if (entry == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Content cache: %s has no file \"%s\"", archive_path, name);
        return false;
    }

    uint64_t key = FNV_OFFSET;
    key = hash_text(key, archive_path);
    key = hash_number(key, archive_info.size);
    key = hash_number(key, static_cast<uint64_t>(archive_info.modify_time));
    key = hash_text(key, name);
    key = hash_number(key, entry->size);
    key = hash_number(key, entry->crc32);

    const char* slash = std::strrchr(name, '/');
    const char* file_name = slash != nullptr ? slash + 1 : name;
    char key_text[KEY_LENGTH + 1] = {};
    std::snprintf(key_text, sizeof(key_text), "%016" PRIx64, key);
    std::string folder = std::string(cache_directory) + "/" + key_text;
    std::string path = folder + "/" + file_name;

    // Already there: the key covers the archive's contents, so a file of the
    // right size is the right file (extraction renames into place only when
    // complete and CRC-checked).
    SDL_PathInfo cached = {};
    if (SDL_GetPathInfo(path.c_str(), &cached) && cached.type == SDL_PATHTYPE_FILE && cached.size == entry->size)
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Content cache: %s#%s already extracted", archive_path, name);
    }
    else
    {
        if (!host_fs_make_directories(folder.c_str()))
        {
            return false;
        }
        uint64_t start = SDL_GetTicksNS();
        if (!archive_extract(archive_path, name, path.c_str()))
        {
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Content cache: extracted %s#%s (%" PRIu64 " KiB, %" PRIu64 " ms)",
                    archive_path, name, entry->size / 1024, (SDL_GetTicksNS() - start) / 1000000);
    }
    mark_used(&folder);
    *out_path = path;
    return true;
}

// --- Trimming -------------------------------------------------------------------

struct CacheFolder
{
    std::string path;
    uint64_t    bytes = 0;
    int64_t     last_used = 0;   // SDL_Time; 0 = unknown (oldest)
    bool        keep = false;
};

static bool is_key_name(const char* name)
{
    if (std::strlen(name) != KEY_LENGTH)
    {
        return false;
    }
    for (size_t i = 0; i < KEY_LENGTH; i++)
    {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

// std::sort's comparator (the library forces references here).
static bool used_earlier(const CacheFolder& a, const CacheFolder& b)
{
    return a.last_used < b.last_used;
}

static void read_folder(CacheFolder* folder)
{
    int count = 0;
    char** names = SDL_GlobDirectory(folder->path.c_str(), "*", 0, &count);
    for (int i = 0; names != nullptr && i < count; i++)
    {
        std::string path = folder->path + "/" + names[i];
        SDL_PathInfo info = {};
        if (SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE)
        {
            folder->bytes += info.size;
        }
        if (std::strcmp(names[i], LAST_USED_NAME) == 0)
        {
            uint8_t* data = nullptr;
            size_t size = 0;
            if (host_fs_read_file(path.c_str(), &data, &size))
            {
                std::string text(static_cast<const char*>(static_cast<const void*>(data)), size);
                folder->last_used = static_cast<int64_t>(SDL_strtoll(text.c_str(), nullptr, 10));
                delete[] data;
            }
        }
    }
    SDL_free(names);
}

static void remove_folder(const CacheFolder* folder)
{
    int count = 0;
    char** names = SDL_GlobDirectory(folder->path.c_str(), "*", 0, &count);
    for (int i = 0; names != nullptr && i < count; i++)
    {
        std::string path = folder->path + "/" + names[i];
        SDL_RemovePath(path.c_str());
    }
    SDL_free(names);
    if (!SDL_RemovePath(folder->path.c_str()))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Content cache: can't remove %s: %s", folder->path.c_str(),
                    SDL_GetError());
    }
}

void content_cache_trim(const char* cache_directory, uint64_t max_bytes, const char* keep_path)
{
    int count = 0;
    char** names = SDL_GlobDirectory(cache_directory, "*", 0, &count);
    if (names == nullptr)
    {
        return;   // no cache yet
    }
    std::vector<CacheFolder> folders;
    uint64_t total = 0;
    for (int i = 0; i < count; i++)
    {
        if (!is_key_name(names[i]))
        {
            continue;
        }
        CacheFolder folder = {};
        folder.path = std::string(cache_directory) + "/" + names[i];
        std::string prefix = folder.path + "/";
        folder.keep = keep_path != nullptr && std::strncmp(keep_path, prefix.c_str(), prefix.size()) == 0;
        read_folder(&folder);
        total += folder.bytes;
        folders.push_back(folder);
    }
    SDL_free(names);

    std::sort(folders.begin(), folders.end(), used_earlier);
    for (size_t i = 0; i < folders.size() && total > max_bytes; i++)
    {
        if (folders[i].keep)
        {
            continue;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Content cache: removing %s (%" PRIu64 " KiB, least recently used)",
                    folders[i].path.c_str(), folders[i].bytes / 1024);
        remove_folder(&folders[i]);
        total -= folders[i].bytes;
    }
}

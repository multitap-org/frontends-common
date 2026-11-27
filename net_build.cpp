#include "net_build.h"

#include <cstdio>
#include <cstring>
#include <thread>

#include <SDL3/SDL.h>

#if defined(__ANDROID__)
#include <dlfcn.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include "host_fs.h"
#include "host_log.h"

#if defined(__ANDROID__)

// Its address is inside the library this code is linked into: libmain.so.
static const char LIBRARY_MARK = 0;

// The file mapped at `address`, from /proc/self/maps ("start-end perms offset
// dev inode path"): for an Android whose dladdr gives the library's name
// without its folder.
static bool mapped_file(const void* address, std::string* out)
{
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr)
    {
        return false;
    }
    uintptr_t at = reinterpret_cast<uintptr_t>(address);
    char line[1024];
    bool found = false;
    while (!found && std::fgets(line, sizeof(line), maps) != nullptr)
    {
        unsigned long long start = 0;
        unsigned long long end = 0;
        int path_at = 0;
        if (std::sscanf(line, "%llx-%llx %*s %*s %*s %*s %n", &start, &end, &path_at) >= 2 && path_at > 0 &&
            at >= start && at < end && line[path_at] == '/')
        {
            std::string path = line + path_at;
            while (!path.empty() && (path.back() == '\n' || path.back() == ' '))
            {
                path.pop_back();
            }
            *out = path;
            found = true;
        }
    }
    std::fclose(maps);
    return found;
}

bool net_build_find_program(const char* program, NetBuildIdentity* build)
{
    (void)program;   // the activity's library, whatever the app is called
    build->exe_sha256.clear();
    build->exe_path.clear();
    Dl_info info = {};
    if (dladdr(&LIBRARY_MARK, &info) != 0 && info.dli_fname != nullptr && info.dli_fname[0] == '/')
    {
        build->exe_path = info.dli_fname;
    }
    else if (!mapped_file(&LIBRARY_MARK, &build->exe_path))
    {
        host_log(LogLevel::warning, "Network: can't find this app's own library (unsigned)");
    }
    return true;
}

#elif defined(__EMSCRIPTEN__)

EM_JS_DEPS(net_build, "$stringToUTF8,$UTF8ToString,$FS");

// The page's hash of this build (web/multitap-page.js), into `out`: its
// length (0 when the page has none), or -1 while the page works it out. On
// the page's main thread, where the frontend's loop runs.
EM_JS(int, net_build_web_hash, (char* out, int size), {
    var hash = globalThis.multitap_build_sha256;
    if (hash === undefined)
    {
        return -1;
    }
    stringToUTF8(hash, out, size);
    return hash.length;
});

bool net_build_find_program(const char* program, NetBuildIdentity* build)
{
    (void)program;
    char hash[96] = {};
    int length = net_build_web_hash(hash, static_cast<int>(sizeof(hash)));
    if (length < 0)
    {
        return false;
    }
    build->exe_path.clear();
    build->exe_sha256 = length == 64 ? hash : "";
    return true;
}

#else

bool net_build_find_program(const char* program, NetBuildIdentity* build)
{
    const char* base = SDL_GetBasePath();
    build->exe_sha256.clear();
    build->exe_path = std::string(base != nullptr ? base : "") + program;
#if defined(_WIN32)
    build->exe_path += ".exe";
#endif
    return true;
}

#endif

#if defined(MULTITAP_NET_BUILD_PROOF)
#if !defined(__EMSCRIPTEN__)
#include "file_hash.h"
#include "net_http.h"
#endif
#include "signature.h"

// The releases key's public half (index_sign keygen on the releases
// publisher, the VPS's /srv/multitap/sites/releases-keys/public.key), in hex;
// a next one goes beside it before a key change. Builds read as unsigned until
// their records exist (they come with the first tag).
static const char* const RELEASES_KEYS[] = {
    "b3eaa7821837257719c663b3c99aa27cf261fc63d78175f452ccca45c2466669",   // key_id e41ad6ee967abf8d (2026-11-20)
};

static int32_t load_keys(SignatureKey* keys, int32_t capacity)
{
    int32_t count = 0;
    for (size_t i = 0; i < sizeof(RELEASES_KEYS) / sizeof(RELEASES_KEYS[0]) && count < capacity; i++)
    {
        if (RELEASES_KEYS[i] != nullptr && signature_key_from_hex(RELEASES_KEYS[i], &keys[count]))
        {
            count++;
        }
    }
    return count;
}

// The value of `key` in a record ("key=value" lines), or "".
static std::string record_value(const uint8_t* record, size_t size, const char* key)
{
    std::string text(reinterpret_cast<const char*>(record), size);
    std::string prefix = std::string(key) + "=";
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        if (text.compare(at, prefix.size(), prefix) == 0)
        {
            return text.substr(at + prefix.size(), end - at - prefix.size());
        }
        at = end + 1;
    }
    return "";
}

static bool signed_by_releases(const uint8_t* record, size_t record_size, const uint8_t* signature,
                               size_t signature_size)
{
    SignatureKey keys[4];
    int32_t key_count = load_keys(keys, 4);
    if (key_count == 0)
    {
        return false;
    }
    std::string signature_text(reinterpret_cast<const char*>(signature), signature_size);
    return signature_verify(keys, key_count, record, record_size, signature_text.c_str(), signature_text.size()) ==
           SignatureResult::ok;
}

bool net_build_verify(void* user, const NetDeviceInfo* info)
{
    (void)user;
    if (info->record_size == 0 || info->signature_size == 0 ||
        !signed_by_releases(info->record, info->record_size, info->signature, info->signature_size))
    {
        return false;
    }
    // A signed piccolo record doesn't make a device that says it's a piccolino official.
    return record_value(info->record, info->record_size, "app") == net_app_kind_name(info->kind);
}

static bool read_small_file(const std::string* path, std::string* out)
{
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!host_fs_exists(path->c_str()) || !host_fs_read_file(path->c_str(), &data, &size))
    {
        return false;
    }
    bool fits = size <= static_cast<size_t>(NET_BUILD_RECORD_MAX_BYTES);
    if (fits)
    {
        out->assign(reinterpret_cast<const char*>(data), size);
    }
    delete[] data;
    return fits;
}

// A record and signature that are this build's and signed: into `info`.
static bool take_proof(const NetBuildIdentity* build, const std::string* exe_hash, const std::string* record,
                       const std::string* signature, NetDeviceInfo* info)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(record->data());
    if (record->size() > NET_BUILD_RECORD_MAX || signature->size() > NET_BUILD_SIGNATURE_MAX ||
        record_value(bytes, record->size(), "app") != build->app ||
        record_value(bytes, record->size(), "version") != build->version ||
        record_value(bytes, record->size(), "target") != build->target ||
        record_value(bytes, record->size(), "exe_sha256") != *exe_hash ||
        !signed_by_releases(bytes, record->size(), reinterpret_cast<const uint8_t*>(signature->data()),
                            signature->size()))
    {
        return false;
    }
    std::memcpy(info->record, record->data(), record->size());
    info->record_size = static_cast<uint16_t>(record->size());
    std::memcpy(info->signature, signature->data(), signature->size());
    info->signature_size = static_cast<uint16_t>(signature->size());
    return true;
}

// The program's hash: the one the platform gave, or its file's.
static bool program_hash(const NetBuildIdentity* build, std::string* out)
{
    if (!build->exe_sha256.empty())
    {
        *out = build->exe_sha256;
        return true;
    }
#if defined(__EMSCRIPTEN__)
    return false;   // only the page can hash the program
#else
    char hash[FILE_HASH_SHA256_HEX];
    if (build->exe_path.empty() || !file_hash_sha256(build->exe_path.c_str(), hash))
    {
        return false;
    }
    *out = hash;
    return true;
#endif
}

static std::string record_url(const NetBuildIdentity* build)
{
    return std::string(NET_BUILD_RELEASES_URL) + "/v1/" + build->app + "/builds/" + build->version + "-" +
           build->target + ".cfg";
}

#if defined(__EMSCRIPTEN__)

// The record and its signature, fetched by the page into the folder (kept
// on OPFS with it) as they come: net_build_proof_load checks them each time
// it reads them. The releases server must allow other origins (CORS).
EM_JS(void, net_build_web_fetch, (const char* url_text, const char* record_text, const char* signature_text,
                                  int max_bytes), {
    var url = UTF8ToString(url_text);
    var record = UTF8ToString(record_text);
    var signature = UTF8ToString(signature_text);
    async function get(from)
    {
        var response = await fetch(from, { mode: "cors", cache: "no-cache" });
        if (!response.ok)
        {
            throw new Error(from + ": HTTP " + response.status);
        }
        var data = new Uint8Array(await response.arrayBuffer());
        if (data.length > max_bytes)
        {
            throw new Error(from + ": too large");
        }
        return data;
    }
    Promise.all([get(url), get(url + ".sig")]).then(function(parts) {
        FS.writeFile(record, parts[0]);
        FS.writeFile(signature, parts[1]);
        console.log("Network: this build's record is kept in " + record);
    }).catch(function(error) {
        console.log("Network: no signed record for this build (" + error.message + ")");
    });
});

#endif

bool net_build_proof_load(const NetBuildIdentity* build, const char* cache_dir, bool fetch, NetDeviceInfo* info)
{
    info->record_size = 0;
    info->signature_size = 0;
    if (build->version.empty() || build->version.find("-dev") != std::string::npos)
    {
        return false;   // only tags are signed
    }
    std::string exe_hash;
    if (!program_hash(build, &exe_hash))
    {
        return false;
    }
    std::string record_path = std::string(cache_dir) + "/build_proof.cfg";
    std::string signature_path = record_path + ".sig";
    std::string record;
    std::string signature;
    if (read_small_file(&record_path, &record) && read_small_file(&signature_path, &signature) &&
        take_proof(build, &exe_hash, &record, &signature, info))
    {
        return true;
    }
    if (!fetch)
    {
        return false;
    }
#if defined(__EMSCRIPTEN__)
    // Nothing blocks in the browser: the page fetches, the next load reads it.
    net_build_web_fetch(record_url(build).c_str(), record_path.c_str(), signature_path.c_str(),
                        static_cast<int>(NET_BUILD_RECORD_MAX_BYTES));
    return false;
#else
    std::string url = record_url(build);
    std::string record_part = record_path + ".part";
    std::string signature_part = signature_path + ".part";
    char error[256] = {};
    if (!net_http_download(url.c_str(), record_part.c_str(), NET_BUILD_RECORD_MAX_BYTES, error, sizeof(error)) ||
        !net_http_download((url + ".sig").c_str(), signature_part.c_str(), NET_BUILD_RECORD_MAX_BYTES, error,
                           sizeof(error)))
    {
        host_log(LogLevel::info, "Network: no signed record for this build (%s)", error);
        return false;
    }
    if (!read_small_file(&record_part, &record) || !read_small_file(&signature_part, &signature) ||
        !take_proof(build, &exe_hash, &record, &signature, info))
    {
        host_log(LogLevel::warning, "Network: the releases server's record isn't this build's, or isn't signed");
        return false;
    }
    host_fs_rename(record_part.c_str(), record_path.c_str());
    host_fs_rename(signature_part.c_str(), signature_path.c_str());
    host_log(LogLevel::info, "Network: this build's signed record is kept in %s", record_path.c_str());
    return true;
#endif
}

void net_build_proof_fetch_later(const NetBuildIdentity* build, const char* cache_dir)
{
    if (build->version.empty() || build->version.find("-dev") != std::string::npos)
    {
        return;   // never signed: nothing to ask the server for
    }
#if defined(__EMSCRIPTEN__)
    NetDeviceInfo scratch;
    net_build_proof_load(build, cache_dir, true, &scratch);   // returns at once: the page fetches
#else
    NetBuildIdentity copy = *build;
    std::string dir = cache_dir;
    std::thread([copy, dir]() {
        NetDeviceInfo scratch;
        net_build_proof_load(&copy, dir.c_str(), true, &scratch);
    }).detach();
#endif
}

#else   // no downloads in this build (frontend_download): no proofs, every build unsigned

bool net_build_verify(void* user, const NetDeviceInfo* info)
{
    (void)user;
    (void)info;
    return false;
}

bool net_build_proof_load(const NetBuildIdentity* build, const char* cache_dir, bool fetch, NetDeviceInfo* info)
{
    (void)build;
    (void)cache_dir;
    (void)fetch;
    info->record_size = 0;
    info->signature_size = 0;
    return false;
}

void net_build_proof_fetch_later(const NetBuildIdentity* build, const char* cache_dir)
{
    (void)build;
    (void)cache_dir;
}

#endif

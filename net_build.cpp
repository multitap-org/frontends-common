#include "net_build.h"

#include <cstring>

#include "host_fs.h"
#include "host_log.h"

#if defined(MULTITAP_NET_BUILD_PROOF)
#include "file_hash.h"
#include "net_http.h"
#include "signature.h"

// The releases key's public half (index_sign keygen on the releases stack,
// docs/sites.md: sites/releases-keys/public.key), current and next, in hex.
// Not in any repo yet: until it's filled in, every build reads as unsigned
// (and none is signed: the records come with the first tag).
static const char* const RELEASES_KEYS[] = {nullptr};

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

bool net_build_proof_load(const NetBuildIdentity* build, const char* cache_dir, bool fetch, NetDeviceInfo* info)
{
    info->record_size = 0;
    info->signature_size = 0;
    if (build->version.empty() || build->version.find("-dev") != std::string::npos)
    {
        return false;   // only tags are signed
    }
    char hash[FILE_HASH_SHA256_HEX];
    if (!file_hash_sha256(build->exe_path.c_str(), hash))
    {
        return false;
    }
    std::string exe_hash = hash;
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
    std::string url = std::string(NET_BUILD_RELEASES_URL) + "/v1/" + build->app + "/builds/" + build->version + "-" +
                      build->target + ".cfg";
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

#endif

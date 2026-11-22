// net_http's libcurl backend (Linux: the system's libcurl). See net_http_backend.h.

#include "net_http_backend.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <curl/curl.h>

static std::string g_user_agent;

static size_t on_write(char* data, size_t size, size_t count, void* user)
{
    NetHttpTransfer* transfer = static_cast<NetHttpTransfer*>(user);
    size_t bytes = size * count;
    // Anything but `bytes` makes curl stop with CURLE_WRITE_ERROR.
    return net_http_transfer_write(transfer, data, bytes) ? bytes : 0;
}

// Called by curl about once a second and on every read: the total size (when
// the server says) and the chance to cancel (non-zero = stop).
static int on_progress(void* user, curl_off_t download_total, curl_off_t, curl_off_t, curl_off_t)
{
    NetHttpTransfer* transfer = static_cast<NetHttpTransfer*>(user);
    if (download_total > 0)
    {
        net_http_transfer_set_total(transfer, static_cast<int64_t>(download_total));
    }
    return net_http_transfer_cancelled(transfer) ? 1 : 0;
}

bool net_http_backend_init(const char* user_agent, char* description, size_t description_size,
                           char* error, size_t error_size)
{
    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK)
    {
        std::snprintf(error, error_size, "curl won't start: %s", curl_easy_strerror(result));
        return false;
    }
    g_user_agent = user_agent;
    curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    std::snprintf(description, description_size, "curl %s, %s", version->version,
                  version->ssl_version != nullptr ? version->ssl_version : "no TLS");
    return true;
}

void net_http_backend_shutdown()
{
    curl_global_cleanup();
}

// A POST's response body: read and dropped.
static size_t on_discard(char*, size_t size, size_t count, void*)
{
    return size * count;
}

bool net_http_backend_post(const char* url, const char* content_type, const uint8_t* body, size_t size,
                           uint32_t* status, char* error, size_t error_size)
{
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        std::snprintf(error, error_size, "curl_easy_init failed");
        return false;
    }
    char curl_error[CURL_ERROR_SIZE] = {};
    std::string type_header = std::string("Content-Type: ") + content_type;
    curl_slist* headers = curl_slist_append(nullptr, type_header.c_str());
    // No "Expect: 100-continue" round trip for a small body.
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_discard);
    CURLcode result = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (result != CURLE_OK)
    {
        std::snprintf(error, error_size, "%s", curl_error[0] != '\0' ? curl_error : curl_easy_strerror(result));
        return false;
    }
    *status = static_cast<uint32_t>(code);
    return true;
}

NetHttpFetch net_http_backend_fetch(const char* url, NetHttpTransfer* transfer, char* error, size_t error_size)
{
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        std::snprintf(error, error_size, "curl_easy_init failed");
        return NetHttpFetch::failed;
    }
    char curl_error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    // http -> https is fine, https -> http isn't (as WinHTTP's default policy).
    const char* redirects = std::strncmp(url, "https://", 8) == 0 ? "https" : "http,https";
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, redirects);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);           // 404 etc. are errors, not files
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    // A stalled connection fails instead of hanging forever: under 1 byte/s
    // for 30 seconds.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);              // we're on our own thread
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (transfer->max_bytes > 0)
    {
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(transfer->max_bytes));
    }

    CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (result == CURLE_ABORTED_BY_CALLBACK)
    {
        return NetHttpFetch::cancelled;
    }
    if (result == CURLE_FILESIZE_EXCEEDED)
    {
        transfer->too_large = true;
        return NetHttpFetch::failed;
    }
    if (result != CURLE_OK)
    {
        std::snprintf(error, error_size, "%s", curl_error[0] != '\0' ? curl_error : curl_easy_strerror(result));
        return NetHttpFetch::failed;
    }
    return NetHttpFetch::ok;
}

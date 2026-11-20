#include "net_http.h"

#include <cstdio>
#include <curl/curl.h>

#include "host_log.h"

static bool        g_initialized = false;
static std::string g_user_agent;

// What one transfer needs inside curl's callbacks.
struct Transfer
{
    SDL_IOStream* file      = nullptr;
    int64_t       written   = 0;
    int64_t       max_bytes = 0;
    bool          too_large = false;
    bool          write_failed = false;
    NetHttpJob*   job       = nullptr;   // progress and cancel; nullptr for net_http_download
};

static int32_t clamp_to_int32(int64_t value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > INT32_MAX)
    {
        return INT32_MAX;
    }
    return static_cast<int32_t>(value);
}

static size_t on_write(char* data, size_t size, size_t count, void* user)
{
    Transfer* transfer = static_cast<Transfer*>(user);
    size_t bytes = size * count;
    if (transfer->max_bytes > 0 && transfer->written + static_cast<int64_t>(bytes) > transfer->max_bytes)
    {
        transfer->too_large = true;
        return 0;   // anything but `bytes` makes curl stop with CURLE_WRITE_ERROR
    }
    if (SDL_WriteIO(transfer->file, data, bytes) != bytes)
    {
        transfer->write_failed = true;
        return 0;
    }
    transfer->written += static_cast<int64_t>(bytes);
    if (transfer->job != nullptr)
    {
        SDL_SetAtomicInt(&transfer->job->received, clamp_to_int32(transfer->written));
    }
    return bytes;
}

// Called by curl about once a second and on every read: the total size (when
// the server says) and the chance to cancel (non-zero = stop).
static int on_progress(void* user, curl_off_t download_total, curl_off_t, curl_off_t, curl_off_t)
{
    Transfer* transfer = static_cast<Transfer*>(user);
    if (transfer->job == nullptr)
    {
        return 0;
    }
    if (download_total > 0)
    {
        SDL_SetAtomicInt(&transfer->job->total, clamp_to_int32(static_cast<int64_t>(download_total)));
    }
    return SDL_GetAtomicInt(&transfer->job->cancel) != 0 ? 1 : 0;
}

bool net_http_init(const char* user_agent)
{
    if (g_initialized)
    {
        return true;
    }
    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK)
    {
        host_log(LogLevel::error, "net_http: curl won't start: %s", curl_easy_strerror(result));
        return false;
    }
    g_user_agent  = user_agent != nullptr ? user_agent : "multitap";
    g_initialized = true;
    curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    host_log(LogLevel::info, "net_http: curl %s, %s", version->version,
             version->ssl_version != nullptr ? version->ssl_version : "no TLS");
    return true;
}

void net_http_shutdown()
{
    if (g_initialized)
    {
        curl_global_cleanup();
        g_initialized = false;
    }
}

// The download itself, shared by both entry points. `job` may be nullptr.
static bool download(const char* url, const char* path, int64_t max_bytes, NetHttpJob* job, bool* cancelled,
                     char* error, size_t error_size)
{
    *cancelled = false;
    error[0] = '\0';
    if (!g_initialized)
    {
        std::snprintf(error, error_size, "net_http_init wasn't called");
        return false;
    }
    std::string part = std::string(path) + ".part";
    Transfer transfer;
    transfer.max_bytes = max_bytes;
    transfer.job       = job;
    transfer.file      = SDL_IOFromFile(part.c_str(), "wb");
    if (transfer.file == nullptr)
    {
        std::snprintf(error, error_size, "can't create %s: %s", part.c_str(), SDL_GetError());
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        SDL_CloseIO(transfer.file);
        SDL_RemovePath(part.c_str());
        std::snprintf(error, error_size, "curl_easy_init failed");
        return false;
    }
    char curl_error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,file");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
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
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (max_bytes > 0)
    {
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_bytes));
    }

    CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    bool closed = SDL_CloseIO(transfer.file);

    bool ok = false;
    if (result == CURLE_ABORTED_BY_CALLBACK)
    {
        *cancelled = true;
        std::snprintf(error, error_size, "cancelled");
    }
    else if (transfer.too_large || result == CURLE_FILESIZE_EXCEEDED)
    {
        std::snprintf(error, error_size, "larger than %lld bytes", static_cast<long long>(max_bytes));
    }
    else if (transfer.write_failed || !closed)
    {
        std::snprintf(error, error_size, "can't write %s", part.c_str());
    }
    else if (result != CURLE_OK)
    {
        std::snprintf(error, error_size, "%s", curl_error[0] != '\0' ? curl_error : curl_easy_strerror(result));
    }
    else if (!SDL_RenamePath(part.c_str(), path))
    {
        std::snprintf(error, error_size, "can't rename to %s: %s", path, SDL_GetError());
    }
    else
    {
        ok = true;
    }
    if (!ok)
    {
        SDL_RemovePath(part.c_str());
    }
    return ok;
}

bool net_http_download(const char* url, const char* path, int64_t max_bytes, char* error, size_t error_size)
{
    bool cancelled = false;
    bool ok = download(url, path, max_bytes, nullptr, &cancelled, error, error_size);
    if (!ok)
    {
        host_log(LogLevel::warning, "net_http: %s: %s", url, error);
    }
    return ok;
}

static int job_thread(void* user)
{
    NetHttpJob* job = static_cast<NetHttpJob*>(user);
    bool cancelled = false;
    bool ok = download(job->url.c_str(), job->path.c_str(), job->max_bytes, job, &cancelled,
                       job->error, sizeof(job->error));
    if (!ok && !cancelled)
    {
        host_log(LogLevel::warning, "net_http: %s: %s", job->url.c_str(), job->error);
    }
    // Last: the main thread reads `error` only after it sees this.
    NetHttpState state = ok ? NetHttpState::done : (cancelled ? NetHttpState::cancelled : NetHttpState::failed);
    SDL_SetAtomicInt(&job->state, static_cast<int>(state));
    return 0;
}

bool net_http_job_start(NetHttpJob* job, const char* url, const char* path, int64_t max_bytes)
{
    net_http_job_finish(job);   // a previous run's thread, if any
    job->url       = url;
    job->path      = path;
    job->max_bytes = max_bytes;
    job->error[0]  = '\0';
    SDL_SetAtomicInt(&job->cancel, 0);
    SDL_SetAtomicInt(&job->received, 0);
    SDL_SetAtomicInt(&job->total, 0);
    SDL_SetAtomicInt(&job->state, static_cast<int>(NetHttpState::running));
    job->thread = SDL_CreateThread(job_thread, "net_http", job);
    if (job->thread == nullptr)
    {
        std::snprintf(job->error, sizeof(job->error), "can't start a thread: %s", SDL_GetError());
        SDL_SetAtomicInt(&job->state, static_cast<int>(NetHttpState::failed));
        return false;
    }
    return true;
}

NetHttpState net_http_job_state(NetHttpJob* job)
{
    return static_cast<NetHttpState>(SDL_GetAtomicInt(&job->state));
}

void net_http_job_progress(NetHttpJob* job, int64_t* received, int64_t* total)
{
    *received = SDL_GetAtomicInt(&job->received);
    *total    = SDL_GetAtomicInt(&job->total);
}

void net_http_job_cancel(NetHttpJob* job)
{
    SDL_SetAtomicInt(&job->cancel, 1);
}

NetHttpState net_http_job_finish(NetHttpJob* job)
{
    if (job->thread != nullptr)
    {
        SDL_WaitThread(job->thread, nullptr);
        job->thread = nullptr;
    }
    return net_http_job_state(job);
}

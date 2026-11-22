#include "net_http.h"

#include <cstdio>
#include <cstring>

#include "host_log.h"
#include "net_http_backend.h"

static bool g_initialized = false;

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

bool net_http_transfer_write(NetHttpTransfer* transfer, const void* data, size_t size)
{
    if (transfer->max_bytes > 0 && transfer->written + static_cast<int64_t>(size) > transfer->max_bytes)
    {
        transfer->too_large = true;
        return false;
    }
    if (SDL_WriteIO(transfer->file, data, size) != size)
    {
        transfer->write_failed = true;
        return false;
    }
    transfer->written += static_cast<int64_t>(size);
    if (transfer->job != nullptr)
    {
        SDL_SetAtomicInt(&transfer->job->received, clamp_to_int32(transfer->written));
    }
    return true;
}

void net_http_transfer_set_total(NetHttpTransfer* transfer, int64_t total)
{
    if (transfer->job != nullptr && total > 0)
    {
        SDL_SetAtomicInt(&transfer->job->total, clamp_to_int32(total));
    }
}

bool net_http_transfer_cancelled(const NetHttpTransfer* transfer)
{
    return transfer->job != nullptr && SDL_GetAtomicInt(&transfer->job->cancel) != 0;
}

bool net_http_init(const char* user_agent)
{
    if (g_initialized)
    {
        return true;
    }
    char description[256] = {};
    char error[256] = {};
    if (!net_http_backend_init(user_agent != nullptr ? user_agent : "multitap", description, sizeof(description),
                               error, sizeof(error)))
    {
        host_log(LogLevel::error, "net_http: %s", error);
        return false;
    }
    g_initialized = true;
    host_log(LogLevel::info, "net_http: %s", description);
    return true;
}

void net_http_shutdown()
{
    if (g_initialized)
    {
        net_http_backend_shutdown();
        g_initialized = false;
    }
}

// file:// URLs, the same on every platform (tests and local mirrors):
// "file:///home/x" is /home/x; on Windows "file:///C:/x" and "file://C:/x"
// are C:/x. No host part, no %-decoding.
static NetHttpFetch fetch_file(const char* url, NetHttpTransfer* transfer, char* error, size_t error_size)
{
    const char* path = url + 7;   // after "file://"
#ifdef _WIN32
    if (path[0] == '/' && path[1] != '\0' && path[2] == ':')
    {
        path++;
    }
#endif
    SDL_IOStream* source = SDL_IOFromFile(path, "rb");
    if (source == nullptr)
    {
        std::snprintf(error, error_size, "can't open %s: %s", path, SDL_GetError());
        return NetHttpFetch::failed;
    }
    net_http_transfer_set_total(transfer, SDL_GetIOSize(source));
    NetHttpFetch result = NetHttpFetch::ok;
    uint8_t buffer[16384];
    while (true)
    {
        if (net_http_transfer_cancelled(transfer))
        {
            result = NetHttpFetch::cancelled;
            break;
        }
        size_t got = SDL_ReadIO(source, buffer, sizeof(buffer));
        if (got > 0 && !net_http_transfer_write(transfer, buffer, got))
        {
            result = NetHttpFetch::failed;
            break;
        }
        if (got < sizeof(buffer))
        {
            if (SDL_GetIOStatus(source) == SDL_IO_STATUS_ERROR)
            {
                std::snprintf(error, error_size, "can't read %s: %s", path, SDL_GetError());
                result = NetHttpFetch::failed;
            }
            if (got == 0 || SDL_GetIOStatus(source) != SDL_IO_STATUS_READY)
            {
                break;
            }
        }
    }
    SDL_CloseIO(source);
    return result;
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
    bool is_file = std::strncmp(url, "file://", 7) == 0;
    if (!is_file && std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0)
    {
        std::snprintf(error, error_size, "unsupported URL (http, https and file only)");
        return false;
    }
    std::string part = std::string(path) + ".part";
    NetHttpTransfer transfer;
    transfer.max_bytes = max_bytes;
    transfer.job       = job;
    transfer.file      = SDL_IOFromFile(part.c_str(), "wb");
    if (transfer.file == nullptr)
    {
        std::snprintf(error, error_size, "can't create %s: %s", part.c_str(), SDL_GetError());
        return false;
    }

    NetHttpFetch result = is_file ? fetch_file(url, &transfer, error, error_size)
                                  : net_http_backend_fetch(url, &transfer, error, error_size);
    bool closed = SDL_CloseIO(transfer.file);

    bool ok = false;
    if (result == NetHttpFetch::cancelled)
    {
        *cancelled = true;
        std::snprintf(error, error_size, "cancelled");
    }
    else if (transfer.too_large)
    {
        std::snprintf(error, error_size, "larger than %lld bytes", static_cast<long long>(max_bytes));
    }
    else if (transfer.write_failed || !closed)
    {
        std::snprintf(error, error_size, "can't write %s", part.c_str());
    }
    else if (result != NetHttpFetch::ok)
    {
        if (error[0] == '\0')
        {
            std::snprintf(error, error_size, "download failed");
        }
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

bool net_http_post(const char* url, const char* content_type, const uint8_t* body, size_t size, uint32_t* status,
                   char* error, size_t error_size)
{
    *status = 0;
    if (std::strncmp(url, "https://", 8) != 0 && std::strncmp(url, "http://", 7) != 0)
    {
        std::snprintf(error, error_size, "only http(s) URLs can be posted to");
        return false;
    }
    bool ok = net_http_backend_post(url, content_type, body, size, status, error, error_size);
    if (!ok)
    {
        host_log(LogLevel::warning, "net_http: POST %s: %s", url, error);
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

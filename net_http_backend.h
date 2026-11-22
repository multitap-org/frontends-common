#pragma once

// net_http's backends: what talks HTTP(S) for one platform. Internal to
// net_http (not for frontends). net_http.cpp does everything else: the
// threads, the <path>.part file and its rename, the size limit, file:// URLs.
//
//   net_http_curl.cpp     libcurl, the system's (Linux)
//   net_http_winhttp.cpp  WinHTTP, part of Windows (TLS by Schannel, the
//                         system's certificates and proxy settings)
//   net_http_android.cpp  java.net.HttpURLConnection through JNI, part of
//                         Android (the system's certificates and proxy)
//
// A backend fetches http:// and https:// URLs only, follows redirects (never
// from https to http), fails on HTTP errors (404 is an error, not a file) and
// hands every piece of the body to net_http_transfer_write().

#include <cstddef>
#include <cstdint>

#include <SDL3/SDL.h>

struct NetHttpJob;

// One download, shared by net_http.cpp and the backend.
struct NetHttpTransfer
{
    SDL_IOStream* file         = nullptr;
    int64_t       written      = 0;
    int64_t       max_bytes    = 0;        // 0 = no limit
    bool          too_large    = false;
    bool          write_failed = false;
    NetHttpJob*   job          = nullptr;  // progress and cancel; nullptr for net_http_download
};

enum class NetHttpFetch
{
    ok,
    cancelled,
    failed,      // the reason in `error` (or transfer->too_large / write_failed)
};

// Once, from net_http_init (main thread). `description`: e.g. "curl 8.9.1,
// OpenSSL/3.3.1", for the log. False with the reason in `error`.
bool net_http_backend_init(const char* user_agent, char* description, size_t description_size,
                           char* error, size_t error_size);
void net_http_backend_shutdown();

// POSTs `body` to `url` (http:// or https://, no redirects) and gives the
// status; the response body is dropped. False (reason in `error`) without a
// response. Called from any thread.
bool net_http_backend_post(const char* url, const char* content_type, const uint8_t* body, size_t size,
                           uint32_t* status, char* error, size_t error_size);
// Downloads `url` (http:// or https://) into `transfer`. Called from any thread.
NetHttpFetch net_http_backend_fetch(const char* url, NetHttpTransfer* transfer, char* error, size_t error_size);

// For the backends, from net_http.cpp:
// Writes a piece of the body. False = stop (too large, or the disk failed:
// the flag in `transfer` says which).
bool net_http_transfer_write(NetHttpTransfer* transfer, const void* data, size_t size);
// The size the server announced (call when known).
void net_http_transfer_set_total(NetHttpTransfer* transfer, int64_t total);
// True once the job was asked to stop.
bool net_http_transfer_cancelled(const NetHttpTransfer* transfer);

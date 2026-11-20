#pragma once

// HTTP(S) downloads to a file, for the core downloader (and later anything
// else a frontend fetches). Backend: libcurl, the system's (Linux; later
// built with Schannel on Windows, and a JNI backend on Android, behind this
// same API). Only GET to a file: no uploads, no cookies, nothing kept between
// downloads.
//
// A download runs on its own thread (net_http_job_*) so the UI keeps
// drawing; the frontend polls its state and progress once per frame. It
// writes <path>.part and renames it to <path> only when it's complete, so a
// failed or cancelled download never leaves a half file under the real name.
// Redirects are followed (GitHub release files redirect); http, https and
// file URLs are accepted (callers that want HTTPS only check the URL first).

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Once at startup, from the main thread (curl's global setup isn't thread
// safe). `user_agent`: e.g. "piccolo/0.3". False (logged) if curl won't start.
bool net_http_init(const char* user_agent);
void net_http_shutdown();

enum class NetHttpState
{
    idle,        // never started
    running,
    done,        // the file is at `path`
    failed,      // see `error`
    cancelled,
};

struct NetHttpJob
{
    std::string   url;
    std::string   path;
    int64_t       max_bytes = 0;      // a larger download fails (0 = no limit)
    SDL_Thread*   thread    = nullptr;
    SDL_AtomicInt state     = {};     // NetHttpState
    SDL_AtomicInt cancel    = {};     // set by net_http_job_cancel
    SDL_AtomicInt received  = {};     // bytes so far (capped at INT32_MAX)
    SDL_AtomicInt total     = {};     // bytes expected, 0 = unknown
    char          error[256] = {};    // written by the thread before `state` becomes failed
};

// Starts downloading `url` to `path` on a new thread. False if it couldn't
// start (the job is then failed, with the reason in `error`). The job must
// not be running already.
bool net_http_job_start(NetHttpJob* job, const char* url, const char* path, int64_t max_bytes);

NetHttpState net_http_job_state(NetHttpJob* job);

// Bytes received so far and expected (0 if the server didn't say).
void net_http_job_progress(NetHttpJob* job, int64_t* received, int64_t* total);

// Asks the download to stop; it ends as cancelled soon after.
void net_http_job_cancel(NetHttpJob* job);

// Waits for the thread (call once the state isn't running, or to wait) and
// returns the final state. Safe to call on a job that never started.
NetHttpState net_http_job_finish(NetHttpJob* job);

// The same download on the calling thread (tools, tests, a worker thread of
// the caller's own). False with the reason in `error`.
bool net_http_download(const char* url, const char* path, int64_t max_bytes, char* error, size_t error_size);

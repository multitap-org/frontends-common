#pragma once

// Screenshots written as PNG files on a thread of their own: encoding a big
// picture takes long enough (a 4K PNG, 100 ms and more) that the frame loop
// must not do it. The frontend hands over the pixels (a copy of the core's
// frame, or a GPU capture: video_output.h) and polls for the result.
//
// Up to SCREENSHOT_WRITER_JOBS pictures wait at a time (a RAW and a GPU
// picture for one press); more are refused, so a held key can't pile up work.

#include <cstdint>
#include <string>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>

constexpr uint32_t SCREENSHOT_WRITER_JOBS = 2;

// How the pixels are laid out (4 bytes a pixel unless said otherwise).
enum class ScreenshotPixels
{
    rgba8,      // R, G, B, A bytes
    bgra8,      // B, G, R, A bytes
    xrgb8888,   // libretro's XRGB8888: 32-bit words
    rgb565,     // 16-bit words
    xrgb1555,   // libretro's 0RGB1555: 16-bit words
};

struct ScreenshotImage
{
    uint8_t*         pixels = nullptr;   // new[]: the writer frees it
    uint32_t         width  = 0;
    uint32_t         height = 0;
    uint32_t         pitch  = 0;         // bytes a row
    ScreenshotPixels format = ScreenshotPixels::rgba8;
    std::string      path;               // where the PNG goes (its folder must exist)
};

// One finished picture.
struct ScreenshotResult
{
    bool        ok = false;
    std::string path;
};

struct ScreenshotWriter
{
    SDL_Thread*    thread = nullptr;
    SDL_Mutex*     mutex  = nullptr;
    SDL_Condition* wake   = nullptr;   // a job came in, or quit
    bool           quit   = false;

    // Guarded by the mutex.
    ScreenshotImage  jobs[SCREENSHOT_WRITER_JOBS] = {};
    uint32_t         job_count = 0;
    bool             writing   = false;   // the thread holds a job it took out
    ScreenshotResult results[SCREENSHOT_WRITER_JOBS] = {};
    uint32_t         result_count = 0;
};

// Starts the thread. False (logged) if it can't.
bool screenshot_writer_create(ScreenshotWriter* writer);

// Writes what's still waiting, then stops the thread. Safe on an empty writer.
void screenshot_writer_destroy(ScreenshotWriter* writer);

// True while pictures wait or one is being written.
bool screenshot_writer_busy(ScreenshotWriter* writer);

// Hands `image` to the writer (its pixels are the writer's from now on, even
// when refused). False (logged) when the queue is full.
bool screenshot_writer_submit(ScreenshotWriter* writer, ScreenshotImage* image);

// A picture the thread has finished (written or failed). False when there's none.
bool screenshot_writer_poll(ScreenshotWriter* writer, ScreenshotResult* result);

// Writes one picture as a PNG now, on the calling thread (the thread's
// work; also for tests). False (logged) on failure. Doesn't free the pixels.
bool screenshot_write_png(const ScreenshotImage* image);

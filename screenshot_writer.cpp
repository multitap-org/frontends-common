#include "screenshot_writer.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_surface.h>

static SDL_PixelFormat sdl_format(ScreenshotPixels format)
{
    switch (format)
    {
    case ScreenshotPixels::rgba8:    return SDL_PIXELFORMAT_RGBA32;
    case ScreenshotPixels::bgra8:    return SDL_PIXELFORMAT_BGRA32;
    case ScreenshotPixels::xrgb8888: return SDL_PIXELFORMAT_XRGB8888;
    case ScreenshotPixels::rgb565:   return SDL_PIXELFORMAT_RGB565;
    case ScreenshotPixels::xrgb1555: return SDL_PIXELFORMAT_XRGB1555;
    }
    return SDL_PIXELFORMAT_RGBA32;
}

bool screenshot_write_png(const ScreenshotImage* image)
{
    if (image->pixels == nullptr || image->width == 0 || image->height == 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Screenshot: no picture for %s", image->path.c_str());
        return false;
    }
    SDL_Surface* source = SDL_CreateSurfaceFrom(static_cast<int>(image->width), static_cast<int>(image->height),
                                                sdl_format(image->format), image->pixels,
                                                static_cast<int>(image->pitch));
    // RGB: the picture is opaque, and a PNG without alpha is smaller (and
    // never shows a core's unused X bits as transparency).
    SDL_Surface* rgb = source != nullptr ? SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGB24) : nullptr;
    bool ok = rgb != nullptr && SDL_SavePNG(rgb, image->path.c_str());
    if (!ok)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Screenshot: could not write %s: %s", image->path.c_str(),
                    SDL_GetError());
    }
    SDL_DestroySurface(rgb);
    SDL_DestroySurface(source);
    return ok;
}

static int writer_thread(void* data)
{
    ScreenshotWriter* writer = static_cast<ScreenshotWriter*>(data);
    SDL_LockMutex(writer->mutex);
    while (true)
    {
        while (writer->job_count == 0 && !writer->quit)
        {
            SDL_WaitCondition(writer->wake, writer->mutex);
        }
        if (writer->job_count == 0)
        {
            break;   // quit, and nothing left to write
        }
        ScreenshotImage job = writer->jobs[0];
        for (uint32_t i = 1; i < writer->job_count; i++)
        {
            writer->jobs[i - 1] = writer->jobs[i];
        }
        writer->job_count--;
        writer->jobs[writer->job_count] = ScreenshotImage{};
        writer->writing = true;
        SDL_UnlockMutex(writer->mutex);

        bool ok = screenshot_write_png(&job);
        delete[] job.pixels;

        SDL_LockMutex(writer->mutex);
        writer->writing = false;
        if (writer->result_count < SCREENSHOT_WRITER_JOBS)
        {
            writer->results[writer->result_count] = ScreenshotResult{ok, job.path};
            writer->result_count++;
        }
    }
    SDL_UnlockMutex(writer->mutex);
    return 0;
}

bool screenshot_writer_create(ScreenshotWriter* writer)
{
    *writer = ScreenshotWriter{};
    writer->mutex = SDL_CreateMutex();
    writer->wake = SDL_CreateCondition();
    if (writer->mutex == nullptr || writer->wake == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Screenshot writer: %s", SDL_GetError());
        screenshot_writer_destroy(writer);
        return false;
    }
    writer->thread = SDL_CreateThread(writer_thread, "screenshots", writer);
    if (writer->thread == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Screenshot writer thread: %s", SDL_GetError());
        screenshot_writer_destroy(writer);
        return false;
    }
    return true;
}

void screenshot_writer_destroy(ScreenshotWriter* writer)
{
    if (writer->thread != nullptr)
    {
        SDL_LockMutex(writer->mutex);
        writer->quit = true;
        SDL_SignalCondition(writer->wake);
        SDL_UnlockMutex(writer->mutex);
        SDL_WaitThread(writer->thread, nullptr);
    }
    for (uint32_t i = 0; i < writer->job_count; i++)
    {
        delete[] writer->jobs[i].pixels;   // only when the thread never started
    }
    SDL_DestroyCondition(writer->wake);
    SDL_DestroyMutex(writer->mutex);
    *writer = ScreenshotWriter{};
}

bool screenshot_writer_busy(ScreenshotWriter* writer)
{
    if (writer->mutex == nullptr)
    {
        return false;
    }
    SDL_LockMutex(writer->mutex);
    bool busy = writer->job_count > 0 || writer->writing;
    SDL_UnlockMutex(writer->mutex);
    return busy;
}

bool screenshot_writer_submit(ScreenshotWriter* writer, ScreenshotImage* image)
{
    bool accepted = false;
    if (writer->thread != nullptr)
    {
        SDL_LockMutex(writer->mutex);
        if (writer->job_count < SCREENSHOT_WRITER_JOBS)
        {
            writer->jobs[writer->job_count] = *image;
            writer->job_count++;
            accepted = true;
            SDL_SignalCondition(writer->wake);
        }
        SDL_UnlockMutex(writer->mutex);
    }
    if (!accepted)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Screenshot: still writing the last ones; %s skipped",
                    image->path.c_str());
        delete[] image->pixels;
    }
    *image = ScreenshotImage{};
    return accepted;
}

bool screenshot_writer_poll(ScreenshotWriter* writer, ScreenshotResult* result)
{
    if (writer->mutex == nullptr)
    {
        return false;
    }
    SDL_LockMutex(writer->mutex);
    bool found = writer->result_count > 0;
    if (found)
    {
        *result = writer->results[0];
        for (uint32_t i = 1; i < writer->result_count; i++)
        {
            writer->results[i - 1] = writer->results[i];
        }
        writer->result_count--;
        writer->results[writer->result_count] = ScreenshotResult{};
    }
    SDL_UnlockMutex(writer->mutex);
    return found;
}

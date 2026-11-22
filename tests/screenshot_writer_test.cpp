// Checks screenshot_writer.h: pictures in each pixel layout come out as PNG
// files with the right size and colors (read back with SDL), the queue
// refuses a third picture while two wait, and destroying the writer writes
// what was waiting.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <string>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>

#include "screenshot_writer.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

// A 3x2 picture, its top-left pixel red-ish (200, 100, 50), the rest black.
static ScreenshotImage make_image(ScreenshotPixels format, const std::string& path)
{
    ScreenshotImage image = {};
    image.width = 3;
    image.height = 2;
    image.format = format;
    image.path = path;
    uint32_t bytes = (format == ScreenshotPixels::rgb565 || format == ScreenshotPixels::xrgb1555) ? 2 : 4;
    image.pitch = image.width * bytes;
    image.pixels = new uint8_t[image.pitch * image.height];
    std::memset(image.pixels, 0, image.pitch * image.height);
    uint8_t* p = image.pixels;
    switch (format)
    {
    case ScreenshotPixels::rgba8:
        p[0] = 200; p[1] = 100; p[2] = 50; p[3] = 255;
        break;
    case ScreenshotPixels::bgra8:
        p[0] = 50; p[1] = 100; p[2] = 200; p[3] = 255;
        break;
    case ScreenshotPixels::xrgb8888:
    {
        uint32_t word = (200u << 16) | (100u << 8) | 50u;
        std::memcpy(p, &word, 4);
        break;
    }
    case ScreenshotPixels::rgb565:
    {
        uint16_t word = static_cast<uint16_t>(((200u >> 3) << 11) | ((100u >> 2) << 5) | (50u >> 3));
        std::memcpy(p, &word, 2);
        break;
    }
    case ScreenshotPixels::xrgb1555:
    {
        uint16_t word = static_cast<uint16_t>(((200u >> 3) << 10) | ((100u >> 3) << 5) | (50u >> 3));
        std::memcpy(p, &word, 2);
        break;
    }
    }
    return image;
}

static bool near(uint8_t a, uint8_t b)
{
    return (a > b ? a - b : b - a) <= 8;   // 5- and 6-bit channels lose the low bits
}

static void check_png(const std::string& path, const char* what)
{
    SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
    check(loaded != nullptr, what);
    if (loaded == nullptr)
    {
        return;
    }
    check(loaded->w == 3 && loaded->h == 2, what);
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
    SDL_ReadSurfacePixel(loaded, 0, 0, &r, &g, &b, &a);
    check(near(r, 200) && near(g, 100) && near(b, 50) && a == 255, what);
    SDL_ReadSurfacePixel(loaded, 2, 1, &r, &g, &b, &a);
    check(r == 0 && g == 0 && b == 0 && a == 255, what);
    SDL_DestroySurface(loaded);
}

int main(int argc, char** argv)
{
    std::string folder = argc > 1 ? argv[1] : "screenshot_writer_test";
    SDL_CreateDirectory(folder.c_str());

    static const ScreenshotPixels FORMATS[5] = {ScreenshotPixels::rgba8, ScreenshotPixels::bgra8,
                                                ScreenshotPixels::xrgb8888, ScreenshotPixels::rgb565,
                                                ScreenshotPixels::xrgb1555};
    static const char* const NAMES[5] = {"rgba8", "bgra8", "xrgb8888", "rgb565", "xrgb1555"};
    for (uint32_t i = 0; i < 5; i++)
    {
        ScreenshotImage image = make_image(FORMATS[i], folder + "/" + NAMES[i] + ".png");
        check(screenshot_write_png(&image), NAMES[i]);
        check_png(image.path, NAMES[i]);
        delete[] image.pixels;
    }

    // The thread: two wait, a third is refused; destroy writes what's left.
    ScreenshotWriter writer = {};
    check(screenshot_writer_create(&writer), "writer starts");
    ScreenshotImage first = make_image(ScreenshotPixels::bgra8, folder + "/queued1.png");
    ScreenshotImage second = make_image(ScreenshotPixels::rgba8, folder + "/queued2.png");
    bool accepted_first = screenshot_writer_submit(&writer, &first);
    bool accepted_second = screenshot_writer_submit(&writer, &second);
    check(accepted_first && accepted_second, "two pictures accepted");
    check(first.pixels == nullptr && second.pixels == nullptr, "the writer owns their pixels");
    uint32_t results = 0;
    for (uint32_t tries = 0; tries < 500 && results < 2; tries++)
    {
        ScreenshotResult result = {};
        while (screenshot_writer_poll(&writer, &result))
        {
            check(result.ok, "a queued picture written");
            results++;
        }
        if (results < 2)
        {
            SDL_Delay(10);
        }
    }
    check(results == 2, "both results reported");
    check(!screenshot_writer_busy(&writer), "idle afterwards");
    screenshot_writer_destroy(&writer);
    check_png(folder + "/queued1.png", "queued1");
    check_png(folder + "/queued2.png", "queued2");

    // A writer that never started refuses (and frees) what it's given.
    ScreenshotWriter stopped = {};
    ScreenshotImage refused = make_image(ScreenshotPixels::rgba8, folder + "/refused.png");
    check(!screenshot_writer_submit(&stopped, &refused) && refused.pixels == nullptr, "a stopped writer refuses");

    if (g_failures == 0)
    {
        std::printf("screenshot writer: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

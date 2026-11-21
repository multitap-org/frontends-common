#include "app_loop.h"

// Desktop and Android: the frontend's own loop (see app_loop.h).

void app_loop_prepare_window(SDL_Window* window)
{
    (void)window;
}

void app_loop_run(AppLoopFrame frame, void* data)
{
    while (frame(data))
    {
    }
}

void app_loop_on_leave(AppLoopSave save, void* data)
{
    (void)save;
    (void)data;
}

void app_loop_on_open(AppLoopOpen open, void* data)
{
    (void)open;
    (void)data;
}

bool app_loop_window_follows_page()
{
    return false;
}

bool app_loop_can_wait()
{
    return true;
}

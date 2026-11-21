#pragma once

// Who drives a frontend's loop (piccolo, piccoletto).
//
// Desktop and Android (app_loop.cpp): a plain loop, app_loop_run returns
// when the frame function says to stop, and the frontend paces itself (vsync
// presents, frame timing waits, frame delay).
//
// The browser (app_loop_web.cpp): the page must get control back after
// every frame, so the browser calls the frame function from its animation
// frames (requestAnimationFrame, at the display's refresh) and
// app_loop_run never returns. Nothing may block there: the browser paces.

struct SDL_Window;

typedef bool (*AppLoopFrame)(void* data);
typedef void (*AppLoopSave)(void* data, bool unloading);
typedef void (*AppLoopOpen)(void* data, const char* path);

// Right after the window exists. Web: the page sizes the canvas with CSS
// and SDL follows it (the frontend's window size settings don't apply
// there). Desktop: nothing.
void app_loop_prepare_window(SDL_Window* window);

// Calls frame(data) until it returns false (desktop), or hands it to the
// browser for good (web: the code after this call never runs).
void app_loop_run(AppLoopFrame frame, void* data);

// The browser never runs the frontend's shutdown (closing the tab just stops
// it): save(data) is called, right then, whenever the page is hidden (tab
// switched, minimized, closed on a phone) or unloaded (reload, close), to
// write what would otherwise be written at exit (SRAM, settings);
// `unloading` is true only for the unload, the one sure end. Desktop:
// nothing (the frontend saves at exit).
void app_loop_on_leave(AppLoopSave save, void* data);

// The browser: the page opens files (a LOAD button, a file dropped on it)
// by writing them into the virtual filesystem and calling the exported
// multitap_web_open(path), which calls open(data, path). Desktop: nothing
// (the frontend browses the real filesystem).
void app_loop_on_open(AppLoopOpen open, void* data);

// True in the browser: the window is the page's size, the frontend's window
// size settings don't apply.
bool app_loop_window_follows_page();

// False in the browser: the frontend's own waits (frame timing, frame delay)
// must be skipped there.
bool app_loop_can_wait();

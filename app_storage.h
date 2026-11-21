#pragma once

// Where a frontend's folder lives, before anything reads it.
//
// Desktop and Android (app_storage.cpp): the real filesystem, nothing to do.
//
// The browser (app_storage_web.cpp): the frontend's folder (app_paths:
// settings, saves, states, games) lives in the virtual filesystem's memory,
// and the page keeps it on the Origin Private File System (OPFS):
// web/multitap-storage.js copies OPFS into memory before main(), and writes
// what changed back every few seconds, when the tab is hidden, and on
// app_storage_flush(). Without OPFS the frontend still runs, but nothing is
// kept after the tab closes (the page logs it).

// folder_name: the frontend's folder name under XDG_DATA_HOME ("piccolo").
void app_storage_init(const char* folder_name);

// After the frontend saved (leaving, or the tab hidden): keep it now, not at
// the next periodic write.
void app_storage_flush();

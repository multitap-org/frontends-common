#pragma once

// Files packed in an Android APK as assets (layouts, themes, shader presets)
// can be read through SDL (SDL_LoadFile, SDL_GlobDirectory with relative
// paths) but not by host_fs (hosts have no SDL) or librashader, which want
// plain files. So frontends copy them out once, into their own storage.
// piccolo and piccoletto did this each their own way (a stamp from the
// activity vs a version string plus a file list made at build time).

#include <cstdint>

// Android: copies every file under each asset folder in `folders` (e.g.
// "layouts", "shaders"; subfolders kept) to destination/<folder>/, unless
// `stamp` is the one this function left in destination/stamp last time.
// `stamp` says which APK this is (its install time from the activity, or
// the version); "" copies at every start. `destination` ends with a
// separator. Files a newer APK no longer ships stay behind (harmless).
// False if a file couldn't be written (logged); the stamp is then not
// written, so the next start tries again.
//
// Elsewhere (desktop, the browser): nothing to do, true.
bool app_assets_unpack(const char* const* folders, uint32_t count, const char* destination, const char* stamp);

// The stamp to pass above: the APK's install time, which the frontend's
// activity puts in the environment as MULTITAP_APK_STAMP (from
// PackageInfo.lastUpdateTime) before SDL_main runs. "" if it didn't.
const char* app_assets_apk_stamp();

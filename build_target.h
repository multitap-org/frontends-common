#pragma once

// The platform this program was built for, named as the release assets are
// (<app>-<version>-<target>.<ext>, release.yml in piccolo and piccoletto) and
// as the releases server names a build's signed record (net_build.h):
//   windows-x86_64  windows-arm64  linux-x86_64  linux-aarch64
//   android-arm64-v8a  android-armeabi-v7a-legacy  web
// "" for a platform nothing is released for. Set by CMake
// (MULTITAP_BUILD_TARGET). Not the cores' target (frontend-libretro's
// core_index_build_target), which names what multitap-org/cores builds.

const char* build_target();

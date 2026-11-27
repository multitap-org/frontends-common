#pragma once

// A build's proof that Multitap released it (Andrés, 2026-11-25; design
// notes 11.9 and 11.13, capabilities plan section 11).
//
// Tagged releases and the builds of main (snapshots, Andrés, 2026-11-27) are
// signed; dev, branch and local builds aren't. The releases server
// (cores-server's publish-apps.sh) signs one record per build: a release's
// with the releases key it already signs the apps' index.cfg with, a
// snapshot's (version <base>-snapshot.<date>.<commit>, tag=snapshot) with the
// snapshot key, which counts only for snapshot versions:
//   <releases>/v1/<app>/builds/<version>-<target>.cfg (+ .sig)
//   app=piccolo
//   version=0.4.0
//   tag=v0.4.0
//   target=linux-x86_64
//   exe_sha256=<64 hex>   the program itself (release.yml writes it in manifest.cfg)
// "The program" (net_build_find_program; Andrés, 2026-11-27):
//   Windows, Linux  the executable (piccolo.exe, the Flatpak's piccolo)
//   Android         the native activity's library, libmain.so: not the APK,
//                   so a store that signs the APK again doesn't change it
//   web             the list of the zip's wasm files with their hashes
//                   (wasm.sha256, web/package.sh): the page checks that its
//                   own .wasm is listed and hands over the list's hash
// A build finds its record (the program's own hash must match), keeps it in
// its settings folder, and sends record + signature in its hello
// (net_pairing.h: NetDeviceInfo). The other device checks the signature with
// the releases public key built in (net_build_verify) and that the record
// names the kind of device it says it is.
//
// What it proves: that some official build's record was sent. A hostile
// build can copy one, so this warns about forks and self-built copies, not
// attackers; each device still refuses path changes itself (net_share's
// CONFIG refuses the device's own files).

#include <cstdint>
#include <string>

#include "net_pairing.h"

constexpr const char* NET_BUILD_RELEASES_URL = "https://releases.get-piccolo.app";
constexpr int64_t     NET_BUILD_RECORD_MAX_BYTES = 4096;

struct NetBuildIdentity
{
    std::string app;        // "piccolo", "piccoletto", "piccolino"
    std::string version;    // "0.4.0"; empty or with "-dev" for an unreleased build (never signed)
    std::string target;     // "linux-x86_64", ... (build_target.h)
    std::string exe_path;   // the program file, hashed to match the record
    std::string exe_sha256; // or the program's hash as the platform gives it (web); then exe_path isn't read
};

// Where this build's program is (exe_path) or its hash (exe_sha256), as
// the release hashed it (above): `program` ("piccolo") in SDL's base path on
// the desktop (with .exe on Windows), the library this code is linked into on
// Android, the page's hash in the browser (globalThis.multitap_build_sha256,
// web/multitap-page.js). False while the page is still working it out: ask
// again later. A browser page with no hash (a dev build, a missing list)
// gives true and an empty identity: never signed.
bool net_build_find_program(const char* program, NetBuildIdentity* build);

// NetVerifyBuildFn: the record's signature checks out with a releases key
// (or a snapshot key, for a snapshot version) and the record's app is the
// device's kind. `user` is unused.
bool net_build_verify(void* user, const NetDeviceInfo* info);

// This build's proof into `info` (record and signature), from
// <cache_dir>/build_proof.cfg (+ .sig) when it is this build's (version,
// target, program hash) and signed. With `fetch`, a missing or stale one is
// downloaded from the releases server first (blocking: run it on a thread or
// before networking starts) and kept there. False (info left without a proof)
// for a dev build or when there is none.
bool net_build_proof_load(const NetBuildIdentity* build, const char* cache_dir, bool fetch, NetDeviceInfo* info);
// net_build_proof_load with `fetch`, on a thread of its own (detached, for
// the program's start): the proof lands in <cache_dir> for the next launch.
// Nothing happens for a dev build or without downloads.
void net_build_proof_fetch_later(const NetBuildIdentity* build, const char* cache_dir);

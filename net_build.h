#pragma once

// A build's proof that Multitap released it (Andrés, 2026-11-25; design
// notes 11.9 and 11.13, capabilities plan section 11).
//
// Only tagged releases are signed; dev and debug builds aren't. The releases
// server (cores-server's publish-apps.sh) signs, with the releases key it
// already signs the apps' index.cfg with, one record per build:
//   <releases>/v1/<app>/builds/<version>-<target>.cfg (+ .sig)
//   app=piccolo
//   version=0.4.0
//   tag=v0.4.0
//   target=linux-x86_64
//   exe_sha256=<64 hex>   the program itself (release.yml writes it in manifest.cfg)
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
    std::string target;     // "linux-x86_64", ... (core_index_build_target)
    std::string exe_path;   // the program file, hashed to match the record
};

// NetVerifyBuildFn: the record's signature checks out with a releases key
// and the record's app is the device's kind. `user` is unused.
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

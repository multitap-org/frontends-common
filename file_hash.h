#pragma once

// SHA-256 of a file, for checking downloads against what a signed index says.
// The LZMA SDK's implementation (deps/7zip/C/Sha256.c, public domain), the
// one already built for 7z reading; its portable code path (no CPU dispatch:
// that needs a one-time global setup and hashing a core takes milliseconds
// anyway).

#include <cstddef>

constexpr size_t FILE_HASH_SHA256_HEX = 64 + 1;   // 64 hex characters + NUL

// Lowercase hex, as sha256sum prints it. False (logged) if the file can't be read.
bool file_hash_sha256(const char* path, char out[FILE_HASH_SHA256_HEX]);

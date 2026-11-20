#include "file_hash.h"

#include <SDL3/SDL.h>

#include "Sha256.h"
#include "host_log.h"

bool file_hash_sha256(const char* path, char out[FILE_HASH_SHA256_HEX])
{
    out[0] = '\0';
    SDL_IOStream* file = SDL_IOFromFile(path, "rb");
    if (file == nullptr)
    {
        host_log(LogLevel::error, "file_hash: can't open %s: %s", path, SDL_GetError());
        return false;
    }
    CSha256 sha;
    Sha256_Init(&sha);
    Byte buffer[65536];
    bool ok = true;
    // A short read isn't the end by itself: only 0 bytes with the EOF status is.
    while (true)
    {
        size_t got = SDL_ReadIO(file, buffer, sizeof(buffer));
        if (got > 0)
        {
            Sha256_Update(&sha, buffer, got);
            continue;
        }
        if (SDL_GetIOStatus(file) != SDL_IO_STATUS_EOF)
        {
            host_log(LogLevel::error, "file_hash: can't read %s: %s", path, SDL_GetError());
            ok = false;
        }
        break;
    }
    SDL_CloseIO(file);
    if (!ok)
    {
        return false;
    }
    Byte digest[SHA256_DIGEST_SIZE];
    Sha256_Final(&sha, digest);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        out[i * 2]     = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    out[SHA256_DIGEST_SIZE * 2] = '\0';
    return true;
}

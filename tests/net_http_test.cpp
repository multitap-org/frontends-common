// net_http + file_hash, with file:// URLs (no network in tests):
//   net_http_test <tests/data folder> <scratch folder>
// A download lands under its name with the right bytes; one over max_bytes,
// a missing file and a refused scheme fail and leave nothing (no .part); the
// threaded job reports done and its progress. With MULTITAP_NET_HTTP_ONLINE_URL
// set, a real download over the network too (CI).

#include <cstdio>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "file_hash.h"
#include "host_fs.h"
#include "net_http.h"

static int32_t failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static const char* const FILES_ZIP_SHA256 = "6fdf181df66fe8740a59c24d0b4566fa6979a6663e910fa1e66bb82cc06c023f";
constexpr int64_t FILES_ZIP_SIZE = 1245;

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf("usage: net_http_test <tests/data folder> <scratch folder>\n");
        return 2;
    }
    std::string data    = argv[1];
    std::string scratch = argv[2];
    check(host_fs_make_directories(scratch.c_str()), "scratch folder");
    check(net_http_init("multitap-test"), "net_http_init");

    char hash[FILE_HASH_SHA256_HEX];
    std::string source = data + "/files.zip";
    check(file_hash_sha256(source.c_str(), hash) && std::strcmp(hash, FILES_ZIP_SHA256) == 0, "file_hash of files.zip");
    check(!file_hash_sha256((scratch + "/missing").c_str(), hash), "file_hash of a missing file fails");

    std::string url  = "file://" + source;
    std::string dest = scratch + "/downloaded.zip";
    char error[256];
    check(net_http_download(url.c_str(), dest.c_str(), 0, error, sizeof(error)), "download");
    check(file_hash_sha256(dest.c_str(), hash) && std::strcmp(hash, FILES_ZIP_SHA256) == 0, "downloaded bytes");
    check(!host_fs_exists((dest + ".part").c_str()), "no .part after a download");

    std::string small = scratch + "/too_large.zip";
    check(!net_http_download(url.c_str(), small.c_str(), 100, error, sizeof(error)), "over max_bytes fails");
    check(!host_fs_exists(small.c_str()) && !host_fs_exists((small + ".part").c_str()), "nothing left after too large");

    std::string missing = scratch + "/missing.zip";
    check(!net_http_download(("file://" + data + "/no_such_file").c_str(), missing.c_str(), 0, error, sizeof(error)),
          "missing file fails");
    check(!host_fs_exists(missing.c_str()) && !host_fs_exists((missing + ".part").c_str()), "nothing left after missing");

    check(!net_http_download("ftp://example.invalid/x", missing.c_str(), 0, error, sizeof(error)), "ftp refused");

    NetHttpJob job;
    check(net_http_job_state(&job) == NetHttpState::idle, "job starts idle");
    check(net_http_job_finish(&job) == NetHttpState::idle, "finish on a job that never ran");
    std::string threaded = scratch + "/threaded.zip";
    check(net_http_job_start(&job, url.c_str(), threaded.c_str(), 0), "job starts");
    check(net_http_job_finish(&job) == NetHttpState::done, "job done");
    int64_t received = 0;
    int64_t total = 0;
    net_http_job_progress(&job, &received, &total);
    check(received == FILES_ZIP_SIZE, "job progress = file size");
    check(file_hash_sha256(threaded.c_str(), hash) && std::strcmp(hash, FILES_ZIP_SHA256) == 0, "job's bytes");

    check(net_http_job_start(&job, ("file://" + data + "/no_such_file").c_str(), missing.c_str(), 0), "failing job starts");
    check(net_http_job_finish(&job) == NetHttpState::failed && job.error[0] != '\0', "failing job reports failed");

    // Over the network, only when asked (CI sets it; tests don't need a
    // network otherwise): MULTITAP_NET_HTTP_ONLINE_URL=https://... must download.
    const char* online = SDL_getenv("MULTITAP_NET_HTTP_ONLINE_URL");
    if (online != nullptr && online[0] != '\0')
    {
        std::string fetched = scratch + "/online";
        bool ok = net_http_download(online, fetched.c_str(), 64 * 1024 * 1024, error, sizeof(error));
        if (!ok)
        {
            std::printf("online download of %s: %s\n", online, error);
        }
        int64_t size = 0;
        SDL_PathInfo info;
        if (ok && SDL_GetPathInfo(fetched.c_str(), &info))
        {
            size = static_cast<int64_t>(info.size);
        }
        std::printf("online download of %s: %lld bytes\n", online, static_cast<long long>(size));
        check(ok && size > 0, "online download");
        check(!net_http_download("https://cores.get-piccolo.app/no/such/file", (scratch + "/online404").c_str(), 0,
                                 error, sizeof(error)),
              "online 404 fails");
    }

    net_http_shutdown();
    if (failures == 0)
    {
        std::printf("net_http: all checks passed\n");
        return 0;
    }
    return 1;
}

// Checks app_links.h on Linux, two "copies" in one process (each an
// AppLinks with its own sockets, as two programs would have):
//   - what is a link of the scheme, and which argument is one;
//   - the first copy listens; a later launch with a link hands it over (and
//     would quit); one without a link runs on and doesn't listen;
//   - links pushed (dropped, Android) come out in order with the rest;
//   - a socket left by a copy that crashed is taken over.
//
// Usage: frontend_app_links_test (its runtime folder: a new one in /tmp, as a
// socket's path must be short).
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "app_links.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    char folder[] = "/tmp/app_links_XXXXXX";
    if (mkdtemp(folder) == nullptr)
    {
        std::printf("FAIL no runtime folder\n");
        return 1;
    }
    std::string runtime = folder;
    setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);

    check(app_links_is_link("piccolo://join/abc", "piccolo"), "a link");
    check(app_links_is_link("PICCOLO://join/abc", "piccolo"), "the scheme in any case");
    check(!app_links_is_link("piccolo:join", "piccolo"), "no //: not one");
    check(!app_links_is_link("piccolo://", "piccolo"), "nothing after the scheme");
    check(!app_links_is_link("piccoletto://pair", "piccolo"), "another scheme");
    check(!app_links_is_link("piccolo://a\nb", "piccolo"), "control characters");
    std::string longest(APP_LINKS_MAX + 1, 'a');
    longest.replace(0, 10, "piccolo://");
    check(!app_links_is_link(longest.c_str(), "piccolo"), "too long");
    char program[] = "piccolo";   // (argv's are the test's own)
    char flag[] = "--fullscreen";
    char link[] = "piccolo://join/room1";
    char* args[3] = {program, flag, link};
    check(app_links_from_args(3, args, "piccolo") == link, "the link among the arguments");
    check(app_links_from_args(2, args, "piccolo") == nullptr, "none there");

    // The first copy, started with a link: it keeps it and listens.
    AppLinks* first = new AppLinks();
    check(app_links_start(first, "piccolo", "piccolo://join/at-start"), "the first copy runs");
    check(first->listener != -1 && first->socket_path == runtime + "/piccolo-links.sock", "it listens in the runtime folder");
    std::string got;
    check(app_links_next(first, &got) && got == "piccolo://join/at-start", "its own link first");

    // A later launch with a link: handed over, it would quit.
    AppLinks* second = new AppLinks();
    check(!app_links_start(second, "piccolo", "piccolo://join/later"), "a launch with a link hands it over");
    app_links_stop(second);
    bool arrived = false;
    for (int i = 0; i < 100 && !arrived; i++)
    {
        arrived = app_links_next(first, &got);
        if (!arrived)
        {
            usleep(5000);
        }
    }
    check(arrived && got == "piccolo://join/later", "the running copy gets it");

    // A launch without a link runs on (and doesn't take the socket).
    AppLinks* plain = new AppLinks();
    check(app_links_start(plain, "piccolo", nullptr), "a launch without a link runs");
    check(plain->listener == -1, "it doesn't listen: the first copy does");
    check(!app_links_next(first, &got), "its check left nothing behind");
    app_links_stop(plain);

    // Pushed links (dropped, Android) in order; not a link: ignored.
    app_links_push(first, "piccolo://join/dropped1");
    app_links_push(first, "https://example.org/");
    app_links_push(first, "piccolo://join/dropped2");
    check(app_links_next(first, &got) && got == "piccolo://join/dropped1" && app_links_next(first, &got) &&
              got == "piccolo://join/dropped2" && !app_links_next(first, &got),
          "pushed links in order, others ignored");
    app_links_stop(first);
    struct stat info;
    check(stat((runtime + "/piccolo-links.sock").c_str(), &info) != 0, "the socket removed when it stops");

    // A socket a crashed copy left: bound, never listened on, not removed.
    std::string path = runtime + "/piccolo-links.sock";
    int dead = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    check(bind(dead, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "a dead socket left behind");
    close(dead);
    AppLinks* after = new AppLinks();
    check(app_links_start(after, "piccolo", "piccolo://join/after-crash"), "with only a dead socket there, it runs");
    check(after->listener != -1, "and takes the socket over");
    check(app_links_next(after, &got) && got == "piccolo://join/after-crash", "its link kept");
    app_links_stop(after);

    check(app_links_register("piccolo", "piccolo"), "registering: nothing to do on Linux");

    delete first;
    delete second;
    delete plain;
    delete after;
    rmdir(runtime.c_str());
    if (g_failures == 0)
    {
        std::printf("app links: all checks passed\n");
        return 0;
    }
    return 1;
}

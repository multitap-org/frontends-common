#include "app_links.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host_log.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#define APP_LINKS_SOCKETS 1
typedef SOCKET LinkSocket;
typedef int LinkLength;      // send / recv's length
static const LinkSocket NO_SOCKET = INVALID_SOCKET;
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#define APP_LINKS_SOCKETS 1
typedef int LinkSocket;
typedef size_t LinkLength;   // send / recv's length
static const LinkSocket NO_SOCKET = -1;
#endif

bool app_links_is_link(const char* text, const char* scheme)
{
    if (text == nullptr || scheme == nullptr)
    {
        return false;
    }
    size_t scheme_size = std::strlen(scheme);
    size_t size = std::strlen(text);
    if (scheme_size == 0 || size <= scheme_size + 3 || size > APP_LINKS_MAX)
    {
        return false;
    }
    for (size_t i = 0; i < scheme_size; i++)
    {
        char c = text[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != scheme[i])
        {
            return false;
        }
    }
    if (std::strncmp(text + scheme_size, "://", 3) != 0)
    {
        return false;
    }
    for (size_t i = 0; i < size; i++)
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F)
        {
            return false;
        }
    }
    return true;
}

const char* app_links_from_args(int argc, char** argv, const char* scheme)
{
    for (int i = 1; i < argc; i++)
    {
        if (app_links_is_link(argv[i], scheme))
        {
            return argv[i];
        }
    }
    return nullptr;
}

void app_links_push(AppLinks* links, const char* link)
{
    if (app_links_is_link(link, links->scheme.c_str()))
    {
        links->pending.push_back(link);
    }
}

#if defined(APP_LINKS_SOCKETS)

static void close_socket(LinkSocket fd)
{
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
}

static void remove_file(const char* path)
{
#if defined(_WIN32)
    DeleteFileA(path);
#else
    unlink(path);
#endif
}

// A per-user place: Linux's runtime folder (the Flatpak's own one inside it),
// else /tmp with the user's id; Windows' temporary folder.
static std::string socket_path(const char* scheme)
{
#if defined(_WIN32)
    char folder[MAX_PATH + 1] = {};
    DWORD size = GetTempPathA(sizeof(folder), folder);
    if (size == 0 || size >= sizeof(folder))
    {
        return "";
    }
    return std::string(folder) + scheme + "-links.sock";
#else
    // (A socket's path has room for about 100 bytes: a longer runtime
    // folder falls back to /tmp.)
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] == '/')
    {
        std::string path = std::string(runtime) + "/" + scheme + "-links.sock";
        if (path.size() < sizeof(sockaddr_un::sun_path))
        {
            return path;
        }
    }
    return std::string("/tmp/") + scheme + "-links-" + std::to_string(static_cast<unsigned long>(getuid())) + ".sock";
#endif
}

static bool make_address(const std::string& path, sockaddr_un* address)
{
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address->sun_path))
    {
        return false;
    }
    std::memcpy(address->sun_path, path.c_str(), path.size());
    return true;
}

static void set_receive_timeout(LinkSocket fd, int milliseconds)
{
#if defined(_WIN32)
    DWORD value = static_cast<DWORD>(milliseconds);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
    u_long blocking = 0;
    ioctlsocket(fd, FIONBIO, &blocking);   // accept() passes the listener's non-blocking mode on
#else
    timeval value = {};
    value.tv_sec = milliseconds / 1000;
    value.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
}

// Whether a copy listens at `path` (connecting succeeds); `link` (or nothing)
// is sent to it then.
static bool hand_over(const std::string& path, const char* link)
{
    sockaddr_un address;
    if (!make_address(path, &address))
    {
        return false;
    }
    LinkSocket fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == NO_SOCKET)
    {
        return false;
    }
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        close_socket(fd);
        return false;
    }
    size_t size = link != nullptr ? std::strlen(link) : 0;
    size_t sent = 0;
    while (sent < size)
    {
        int n = static_cast<int>(send(fd, link + sent, static_cast<LinkLength>(size - sent), 0));
        if (n <= 0)
        {
            break;
        }
        sent += static_cast<size_t>(n);
    }
    close_socket(fd);
    return true;
}

static bool listen_at(AppLinks* links, const std::string& path)
{
    sockaddr_un address;
    if (!make_address(path, &address))
    {
        host_log(LogLevel::warning, "Links: no place for the socket (%s)", path.c_str());
        return false;
    }
    LinkSocket fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == NO_SOCKET)
    {
        return false;
    }
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        // Taken: by a copy that runs (it takes the links), or left by one
        // that crashed (nobody answers there: it's ours now).
        if (hand_over(path, nullptr))
        {
            close_socket(fd);
            return false;
        }
        remove_file(path.c_str());
        if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            host_log(LogLevel::warning, "Links: can't listen at %s", path.c_str());
            close_socket(fd);
            return false;
        }
    }
    if (listen(fd, 4) != 0)
    {
        close_socket(fd);
        remove_file(path.c_str());
        return false;
    }
#if defined(_WIN32)
    u_long non_blocking = 1;
    ioctlsocket(fd, FIONBIO, &non_blocking);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);   // not into the programs we start
#endif
    links->listener = static_cast<intptr_t>(fd);
    links->socket_path = path;
    return true;
}

// What a later launch sent: the link, until it closes (or 300 ms).
static void take_one(AppLinks* links, LinkSocket client)
{
    set_receive_timeout(client, 300);
    char text[APP_LINKS_MAX + 1];
    size_t size = 0;
    while (size < APP_LINKS_MAX)
    {
        int n = static_cast<int>(recv(client, text + size, static_cast<LinkLength>(APP_LINKS_MAX - size), 0));
        if (n <= 0)
        {
            break;
        }
        size += static_cast<size_t>(n);
    }
    text[size] = '\0';
    close_socket(client);
    if (size == 0)
    {
        return;   // a launch only checking that we're here
    }
    if (!app_links_is_link(text, links->scheme.c_str()))
    {
        host_log(LogLevel::warning, "Links: something that isn't a %s link came to the socket", links->scheme.c_str());
        return;
    }
    links->pending.push_back(text);
}

bool app_links_start(AppLinks* links, const char* scheme, const char* link)
{
    links->scheme = scheme;
    links->pending.clear();
    links->listener = -1;
    links->socket_path.clear();
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        app_links_push(links, link);
        return true;
    }
#endif
    std::string path = socket_path(scheme);
    if (link != nullptr && app_links_is_link(link, scheme) && hand_over(path, link))
    {
        host_log(LogLevel::info, "Links: handed to the copy already running");
#if defined(_WIN32)
        WSACleanup();
#endif
        return false;
    }
    app_links_push(links, link);
    if (listen_at(links, path))
    {
        host_log(LogLevel::info, "Links: %s:// links come here (%s)", scheme, path.c_str());
    }
    return true;
}

void app_links_stop(AppLinks* links)
{
    if (links->listener != -1)
    {
        close_socket(static_cast<LinkSocket>(links->listener));
        remove_file(links->socket_path.c_str());
        links->listener = -1;
        links->socket_path.clear();
    }
#if defined(_WIN32)
    WSACleanup();
#endif
    links->pending.clear();
}

bool app_links_next(AppLinks* links, std::string* link)
{
    if (links->listener != -1)
    {
        for (int i = 0; i < 4; i++)   // a few per frame
        {
            LinkSocket client = accept(static_cast<LinkSocket>(links->listener), nullptr, nullptr);
            if (client == NO_SOCKET)
            {
                break;
            }
            take_one(links, client);
        }
    }
    if (links->pending.empty())
    {
        return false;
    }
    *link = links->pending.front();
    links->pending.erase(links->pending.begin());
    return true;
}

#else   // the browser, Android: no hand-over (Android keeps one activity itself)

bool app_links_start(AppLinks* links, const char* scheme, const char* link)
{
    links->scheme = scheme;
    links->pending.clear();
    app_links_push(links, link);
    return true;
}

void app_links_stop(AppLinks* links)
{
    links->pending.clear();
}

bool app_links_next(AppLinks* links, std::string* link)
{
    if (links->pending.empty())
    {
        return false;
    }
    *link = links->pending.front();
    links->pending.erase(links->pending.begin());
    return true;
}

#endif

#if defined(_WIN32)

static std::wstring wide(const char* text)
{
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 0)
    {
        return L"";
    }
    std::wstring out(static_cast<size_t>(count - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], count);
    return out;
}

bool app_links_register(const char* scheme, const char* app_name)
{
    wchar_t program[MAX_PATH * 2] = {};
    DWORD size = GetModuleFileNameW(nullptr, program, MAX_PATH * 2);
    if (size == 0 || size >= MAX_PATH * 2)
    {
        return false;
    }
    std::wstring key = L"Software\\Classes\\" + wide(scheme);
    std::wstring command_key = key + L"\\shell\\open\\command";
    std::wstring command = L"\"" + std::wstring(program) + L"\" \"%1\"";
    // Already this program: nothing to write.
    wchar_t current[MAX_PATH * 3] = {};
    DWORD current_size = sizeof(current);
    if (RegGetValueW(HKEY_CURRENT_USER, command_key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, current, &current_size) ==
            ERROR_SUCCESS &&
        command == current)
    {
        return true;
    }
    std::wstring description = L"URL:" + wide(app_name);
    const wchar_t* empty = L"";
    bool ok = RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, REG_SZ, description.c_str(),
                              static_cast<DWORD>((description.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS &&
              RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"URL Protocol", REG_SZ, empty, sizeof(wchar_t)) ==
                  ERROR_SUCCESS &&
              RegSetKeyValueW(HKEY_CURRENT_USER, command_key.c_str(), nullptr, REG_SZ, command.c_str(),
                              static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    host_log(ok ? LogLevel::info : LogLevel::warning, "Links: %s:// %s", scheme,
             ok ? "now opens this program" : "couldn't be registered");
    return ok;
}

#else

bool app_links_register(const char* scheme, const char* app_name)
{
    (void)scheme;
    (void)app_name;
    return true;
}

#endif

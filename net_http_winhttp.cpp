// net_http's WinHTTP backend (Windows). See net_http_backend.h.
//
// WinHTTP is part of Windows: no DLLs to ship, TLS by Schannel with the
// system's certificate store, the system's proxy settings. It follows
// redirects by itself and by default never from https to http.
//
// Synchronous use, on net_http's own thread: one session for the program
// (WinHttpOpen), a connection + request per download. Cancelling is checked
// between reads, so it takes effect when the next piece arrives (or when the
// 30-second receive timeout ends a stalled connection).

#include "net_http_backend.h"

#include <cstdio>
#include <string>

#include <windows.h>
#include <winhttp.h>

static HINTERNET g_session = nullptr;

// Values from Microsoft's winhttp.h that older MinGW headers don't have.
constexpr DWORD ACCESS_TYPE_AUTOMATIC_PROXY = 4;            // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
constexpr DWORD SECURE_PROTOCOL_TLS1_3      = 0x00002000;   // WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
constexpr DWORD WINHTTP_ERRORS_FIRST        = 12000;        // WINHTTP_ERROR_BASE
constexpr DWORD WINHTTP_ERRORS_LAST         = 12999;

// UTF-8 -> UTF-16 for WinHTTP's wide-only API.
static std::wstring widen(const char* text)
{
    int32_t length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1)
    {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length - 1));   // without the terminator
    return wide;
}

// "<what>: <WinHTTP's or Windows' message> (<code>)" for the last error.
static void describe_error(const char* what, char* error, size_t error_size)
{
    DWORD code = GetLastError();
    char message[256] = {};
    // WinHTTP's own errors (12000-12999) have their messages in winhttp.dll.
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE module = nullptr;
    if (code >= WINHTTP_ERRORS_FIRST && code <= WINHTTP_ERRORS_LAST)
    {
        module = GetModuleHandleW(L"winhttp.dll");
        flags  = FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS;
    }
    DWORD length = FormatMessageA(flags, module, code, 0, message, sizeof(message), nullptr);
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r' || message[length - 1] == '.'))
    {
        message[--length] = '\0';
    }
    if (code == ERROR_WINHTTP_TIMEOUT)
    {
        std::snprintf(error, error_size, "%s: timed out", what);
    }
    else if (length > 0)
    {
        std::snprintf(error, error_size, "%s: %s (%lu)", what, message, static_cast<unsigned long>(code));
    }
    else
    {
        std::snprintf(error, error_size, "%s: error %lu", what, static_cast<unsigned long>(code));
    }
}

bool net_http_backend_init(const char* user_agent, char* description, size_t description_size,
                           char* error, size_t error_size)
{
    std::wstring agent = widen(user_agent);
    // The system's proxy settings, auto-detection included (Windows 8.1+);
    // older Windows only knows the static ones.
    g_session = WinHttpOpen(agent.c_str(), ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    const char* proxy = "automatic proxy";
    if (g_session == nullptr)
    {
        g_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        proxy = "default proxy";
    }
    if (g_session == nullptr)
    {
        describe_error("WinHttpOpen", error, error_size);
        return false;
    }
    // Resolve: no limit (the system's), connect 20 s, send and receive 30 s:
    // a stalled connection fails instead of hanging forever.
    WinHttpSetTimeouts(g_session, 0, 20000, 30000, 30000);
    // TLS 1.2 and 1.3; Windows 10 has no 1.3 in WinHTTP and refuses the flag.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | SECURE_PROTOCOL_TLS1_3;
    const char* tls = "TLS 1.2 / 1.3";
    if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
    {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        tls = "TLS 1.2";
    }
    std::snprintf(description, description_size, "WinHTTP, %s, %s", tls, proxy);
    return true;
}

void net_http_backend_shutdown()
{
    if (g_session != nullptr)
    {
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
    }
}

NetHttpFetch net_http_backend_fetch(const char* url, NetHttpTransfer* transfer, char* error, size_t error_size)
{
    std::wstring wide_url = widen(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize      = sizeof(parts);
    parts.dwHostNameLength  = static_cast<DWORD>(-1);   // -1: point into wide_url
    parts.dwUrlPathLength   = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wide_url.empty() || !WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts))
    {
        describe_error("bad URL", error, error_size);
        return NetHttpFetch::failed;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    // Path + query ("/v1/index/x.cfg?y"); "/" when the URL has none.
    std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
    object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (object.empty())
    {
        object = L"/";
    }
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    NetHttpFetch result     = NetHttpFetch::failed;
    HINTERNET    connection = WinHttpConnect(g_session, host.c_str(), parts.nPort, 0);
    HINTERNET    request    = nullptr;
    if (connection == nullptr)
    {
        describe_error("can't connect", error, error_size);
        return NetHttpFetch::failed;
    }
    request = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
    {
        describe_error("can't make the request", error, error_size);
        WinHttpCloseHandle(connection);
        return NetHttpFetch::failed;
    }
    DWORD max_redirects = 5;
    WinHttpSetOption(request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &max_redirects, sizeof(max_redirects));

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        describe_error("can't send the request", error, error_size);
    }
    else if (!WinHttpReceiveResponse(request, nullptr))
    {
        describe_error("no response", error, error_size);
    }
    else
    {
        DWORD status = 0;
        DWORD size   = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        // Content-Length as text: the number flag only gives 32 bits.
        wchar_t length_text[32] = {};
        DWORD   length_size     = sizeof(length_text);
        int64_t length          = 0;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text,
                                &length_size, WINHTTP_NO_HEADER_INDEX))
        {
            length = static_cast<int64_t>(_wcstoui64(length_text, nullptr, 10));
        }

        if (status != 200)
        {
            // 404 etc. are errors, not files (redirects were followed already).
            std::snprintf(error, error_size, "the server answered HTTP %lu", static_cast<unsigned long>(status));
        }
        else if (transfer->max_bytes > 0 && length > transfer->max_bytes)
        {
            transfer->too_large = true;
        }
        else
        {
            net_http_transfer_set_total(transfer, length);
            uint8_t buffer[16384];
            result = NetHttpFetch::ok;
            while (true)
            {
                if (net_http_transfer_cancelled(transfer))
                {
                    result = NetHttpFetch::cancelled;
                    break;
                }
                DWORD got = 0;
                if (!WinHttpReadData(request, buffer, sizeof(buffer), &got))
                {
                    describe_error("download interrupted", error, error_size);
                    result = NetHttpFetch::failed;
                    break;
                }
                if (got == 0)
                {
                    break;   // the whole body
                }
                if (!net_http_transfer_write(transfer, buffer, got))
                {
                    result = NetHttpFetch::failed;
                    break;
                }
            }
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    return result;
}

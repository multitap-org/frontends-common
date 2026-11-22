// net_http's Android backend: java.net.HttpURLConnection through JNI. See
// net_http_backend.h.
//
// HttpURLConnection is part of Android: TLS with the system's certificates,
// the system's proxy settings, no libraries to ship. Only system classes are
// used (java.net.*, java.io.InputStream), so FindClass works from any thread
// and the APK needs no Java of its own for this.
//
// Redirects are followed here, not by HttpURLConnection: its own following
// has no limit we can set and refuses http -> https too. Ours: at most 5,
// never from https to http.
//
// net_http calls fetch on its own thread: SDL_GetAndroidJNIEnv attaches that
// thread to the VM (and SDL detaches it when the thread ends). Cancelling is
// checked between reads, so it takes effect when the next piece arrives (or
// when the 30-second read timeout ends a stalled connection).

#include "net_http_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jni.h>

#include <SDL3/SDL.h>

constexpr int32_t MAX_REDIRECTS      = 5;
constexpr int32_t CONNECT_TIMEOUT_MS = 20000;
constexpr int32_t READ_TIMEOUT_MS    = 30000;
constexpr int32_t BUFFER_SIZE        = 16384;
// Local references made in one fetch: a few per redirect and the buffer.
constexpr int32_t LOCAL_FRAME_SIZE   = 64;

// Classes (global references) and methods, looked up once in init.
struct HttpJni
{
    jclass    url_class             = nullptr;   // java.net.URL
    jmethodID url_new               = nullptr;   // URL(String)
    jmethodID url_new_relative      = nullptr;   // URL(URL context, String spec): resolves Location
    jmethodID url_open              = nullptr;   // openConnection()
    jmethodID url_get_protocol      = nullptr;   // getProtocol()
    jclass    connection_class      = nullptr;   // java.net.HttpURLConnection
    jmethodID set_connect_timeout   = nullptr;
    jmethodID set_read_timeout      = nullptr;
    jmethodID set_follow_redirects  = nullptr;   // setInstanceFollowRedirects(boolean)
    jmethodID set_request_property  = nullptr;
    jmethodID get_response_code     = nullptr;
    jmethodID get_header_field      = nullptr;   // getHeaderField(String)
    jmethodID get_input_stream      = nullptr;
    jmethodID disconnect            = nullptr;
    jclass    stream_class          = nullptr;   // java.io.InputStream
    jmethodID stream_read           = nullptr;   // read(byte[])
    jmethodID stream_close          = nullptr;
    jclass    throwable_class       = nullptr;   // java.lang.Throwable
    jmethodID throwable_to_string   = nullptr;   // for the error text
    // POST (net_http_backend_post).
    jmethodID set_request_method    = nullptr;   // setRequestMethod(String)
    jmethodID set_do_output         = nullptr;   // setDoOutput(boolean)
    jmethodID set_fixed_length      = nullptr;   // setFixedLengthStreamingMode(int)
    jmethodID get_output_stream     = nullptr;   // getOutputStream()
    jclass    output_class          = nullptr;   // java.io.OutputStream
    jmethodID output_write          = nullptr;   // write(byte[])
    jmethodID output_close          = nullptr;
};

static HttpJni g_jni;
static char    g_user_agent[256] = {};

static JNIEnv* jni_env()
{
    return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

// A global reference to a system class, nullptr (exception cleared) if missing.
static jclass find_class(JNIEnv* env, const char* name)
{
    jclass local = env->FindClass(name);
    if (local == nullptr)
    {
        env->ExceptionClear();
        return nullptr;
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

static jmethodID find_method(JNIEnv* env, jclass type, const char* name, const char* signature)
{
    if (type == nullptr)
    {
        return nullptr;
    }
    jmethodID method = env->GetMethodID(type, name, signature);
    if (method == nullptr)
    {
        env->ExceptionClear();
    }
    return method;
}

// Copies a Java string (or nothing for null) into `text`.
static void copy_java_string(JNIEnv* env, jstring value, char* text, size_t text_size)
{
    text[0] = '\0';
    if (value == nullptr)
    {
        return;
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars != nullptr)
    {
        std::snprintf(text, text_size, "%s", chars);
        env->ReleaseStringUTFChars(value, chars);
    }
}

// True if the last call threw; then clears it and writes "<what>: <exception>"
// (e.g. "can't connect: java.net.SocketTimeoutException: timeout").
static bool threw(JNIEnv* env, const char* what, char* error, size_t error_size)
{
    jthrowable exception = env->ExceptionOccurred();
    if (exception == nullptr)
    {
        return false;
    }
    env->ExceptionClear();
    char text[256] = {};
    jstring description = static_cast<jstring>(env->CallObjectMethod(exception, g_jni.throwable_to_string));
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        description = nullptr;
    }
    copy_java_string(env, description, text, sizeof(text));
    std::snprintf(error, error_size, "%s: %s", what, text[0] != '\0' ? text : "Java exception");
    env->DeleteLocalRef(exception);
    if (description != nullptr)
    {
        env->DeleteLocalRef(description);
    }
    return true;
}

static bool is_https(JNIEnv* env, jobject url)
{
    jstring protocol = static_cast<jstring>(env->CallObjectMethod(url, g_jni.url_get_protocol));
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return false;
    }
    char text[16] = {};
    copy_java_string(env, protocol, text, sizeof(text));
    env->DeleteLocalRef(protocol);
    return std::strcmp(text, "https") == 0;
}

bool net_http_backend_init(const char* user_agent, char* description, size_t description_size,
                           char* error, size_t error_size)
{
    JNIEnv* env = jni_env();
    if (env == nullptr)
    {
        std::snprintf(error, error_size, "no JNI environment (%s)", SDL_GetError());
        return false;
    }
    g_jni.url_class        = find_class(env, "java/net/URL");
    g_jni.connection_class = find_class(env, "java/net/HttpURLConnection");
    g_jni.stream_class     = find_class(env, "java/io/InputStream");
    g_jni.throwable_class  = find_class(env, "java/lang/Throwable");

    g_jni.url_new              = find_method(env, g_jni.url_class, "<init>", "(Ljava/lang/String;)V");
    g_jni.url_new_relative     = find_method(env, g_jni.url_class, "<init>", "(Ljava/net/URL;Ljava/lang/String;)V");
    g_jni.url_open             = find_method(env, g_jni.url_class, "openConnection", "()Ljava/net/URLConnection;");
    g_jni.url_get_protocol     = find_method(env, g_jni.url_class, "getProtocol", "()Ljava/lang/String;");
    g_jni.set_connect_timeout  = find_method(env, g_jni.connection_class, "setConnectTimeout", "(I)V");
    g_jni.set_read_timeout     = find_method(env, g_jni.connection_class, "setReadTimeout", "(I)V");
    g_jni.set_follow_redirects = find_method(env, g_jni.connection_class, "setInstanceFollowRedirects", "(Z)V");
    g_jni.set_request_property = find_method(env, g_jni.connection_class, "setRequestProperty",
                                             "(Ljava/lang/String;Ljava/lang/String;)V");
    g_jni.get_response_code    = find_method(env, g_jni.connection_class, "getResponseCode", "()I");
    g_jni.get_header_field     = find_method(env, g_jni.connection_class, "getHeaderField",
                                             "(Ljava/lang/String;)Ljava/lang/String;");
    g_jni.get_input_stream     = find_method(env, g_jni.connection_class, "getInputStream", "()Ljava/io/InputStream;");
    g_jni.disconnect           = find_method(env, g_jni.connection_class, "disconnect", "()V");
    g_jni.stream_read          = find_method(env, g_jni.stream_class, "read", "([B)I");
    g_jni.stream_close         = find_method(env, g_jni.stream_class, "close", "()V");
    g_jni.throwable_to_string  = find_method(env, g_jni.throwable_class, "toString", "()Ljava/lang/String;");
    g_jni.output_class         = find_class(env, "java/io/OutputStream");
    g_jni.set_request_method   = find_method(env, g_jni.connection_class, "setRequestMethod", "(Ljava/lang/String;)V");
    g_jni.set_do_output        = find_method(env, g_jni.connection_class, "setDoOutput", "(Z)V");
    g_jni.set_fixed_length     = find_method(env, g_jni.connection_class, "setFixedLengthStreamingMode", "(I)V");
    g_jni.get_output_stream    = find_method(env, g_jni.connection_class, "getOutputStream", "()Ljava/io/OutputStream;");
    g_jni.output_write         = find_method(env, g_jni.output_class, "write", "([B)V");
    g_jni.output_close         = find_method(env, g_jni.output_class, "close", "()V");

    const jmethodID methods[] = {
        g_jni.url_new, g_jni.url_new_relative, g_jni.url_open, g_jni.url_get_protocol,
        g_jni.set_connect_timeout, g_jni.set_read_timeout, g_jni.set_follow_redirects, g_jni.set_request_property,
        g_jni.get_response_code, g_jni.get_header_field, g_jni.get_input_stream, g_jni.disconnect,
        g_jni.stream_read, g_jni.stream_close, g_jni.throwable_to_string,
        g_jni.set_request_method, g_jni.set_do_output, g_jni.set_fixed_length, g_jni.get_output_stream,
        g_jni.output_write, g_jni.output_close,
    };
    for (jmethodID method : methods)
    {
        if (method == nullptr)
        {
            std::snprintf(error, error_size, "java.net.HttpURLConnection is missing a method");
            net_http_backend_shutdown();
            return false;
        }
    }
    std::snprintf(g_user_agent, sizeof(g_user_agent), "%s", user_agent);
    std::snprintf(description, description_size, "HttpURLConnection (Android API %d)",
                  SDL_GetAndroidSDKVersion());
    return true;
}

void net_http_backend_shutdown()
{
    JNIEnv* env = jni_env();
    if (env != nullptr)
    {
        const jclass classes[] = { g_jni.url_class, g_jni.connection_class, g_jni.stream_class,
                                   g_jni.throwable_class, g_jni.output_class };
        for (jclass type : classes)
        {
            if (type != nullptr)
            {
                env->DeleteGlobalRef(type);
            }
        }
    }
    g_jni = HttpJni();
}

// Opens `url` and gets the response, following up to MAX_REDIRECTS redirects.
// Returns the connection (a local reference, or nullptr with `error` set) and
// its status in `status`.
static jobject open_following_redirects(JNIEnv* env, const char* url_text, int32_t* status,
                                        char* error, size_t error_size)
{
    jstring text = env->NewStringUTF(url_text);
    jobject url  = env->NewObject(g_jni.url_class, g_jni.url_new, text);
    if (threw(env, "bad URL", error, error_size))
    {
        return nullptr;
    }
    jstring agent_key    = env->NewStringUTF("User-Agent");
    jstring agent_value  = env->NewStringUTF(g_user_agent);
    jstring location_key = env->NewStringUTF("Location");

    for (int32_t redirects = 0;; redirects++)
    {
        bool    secure     = is_https(env, url);
        jobject connection = env->CallObjectMethod(url, g_jni.url_open);
        if (threw(env, "can't connect", error, error_size))
        {
            return nullptr;
        }
        if (!env->IsInstanceOf(connection, g_jni.connection_class))
        {
            std::snprintf(error, error_size, "not an http(s) URL");
            return nullptr;
        }
        env->CallVoidMethod(connection, g_jni.set_connect_timeout, CONNECT_TIMEOUT_MS);
        env->CallVoidMethod(connection, g_jni.set_read_timeout, READ_TIMEOUT_MS);
        env->CallVoidMethod(connection, g_jni.set_follow_redirects, JNI_FALSE);
        env->CallVoidMethod(connection, g_jni.set_request_property, agent_key, agent_value);
        // getResponseCode connects, sends the request and reads the headers.
        int32_t code = env->CallIntMethod(connection, g_jni.get_response_code);
        if (threw(env, "no response", error, error_size))
        {
            env->CallVoidMethod(connection, g_jni.disconnect);
            env->ExceptionClear();
            return nullptr;
        }
        bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        if (!redirect)
        {
            *status = code;
            return connection;
        }

        jstring location = static_cast<jstring>(env->CallObjectMethod(connection, g_jni.get_header_field,
                                                                      location_key));
        env->ExceptionClear();
        env->CallVoidMethod(connection, g_jni.disconnect);
        env->ExceptionClear();
        env->DeleteLocalRef(connection);
        if (location == nullptr)
        {
            std::snprintf(error, error_size, "the server answered HTTP %d without a Location", code);
            return nullptr;
        }
        if (redirects >= MAX_REDIRECTS)
        {
            std::snprintf(error, error_size, "more than %d redirects", MAX_REDIRECTS);
            return nullptr;
        }
        // Location may be relative: resolved against the current URL.
        jobject next = env->NewObject(g_jni.url_class, g_jni.url_new_relative, url, location);
        env->DeleteLocalRef(location);
        if (threw(env, "bad redirect", error, error_size))
        {
            return nullptr;
        }
        if (secure && !is_https(env, next))
        {
            std::snprintf(error, error_size, "redirected from https to http (refused)");
            return nullptr;
        }
        env->DeleteLocalRef(url);
        url = next;
    }
}

bool net_http_backend_post(const char* url, const char* content_type, const uint8_t* body, size_t size,
                           uint32_t* status, char* error, size_t error_size)
{
    JNIEnv* env = jni_env();
    if (env == nullptr)
    {
        std::snprintf(error, error_size, "no JNI environment (%s)", SDL_GetError());
        return false;
    }
    if (size > 0x7fffffff || env->PushLocalFrame(LOCAL_FRAME_SIZE) != 0)
    {
        env->ExceptionClear();
        std::snprintf(error, error_size, "can't post that");
        return false;
    }
    bool ok = false;
    jstring text = env->NewStringUTF(url);
    jobject java_url = env->NewObject(g_jni.url_class, g_jni.url_new, text);
    jobject connection = nullptr;
    if (!threw(env, "bad URL", error, error_size))
    {
        connection = env->CallObjectMethod(java_url, g_jni.url_open);
        if (threw(env, "can't connect", error, error_size) || !env->IsInstanceOf(connection, g_jni.connection_class))
        {
            connection = nullptr;
        }
    }
    if (connection != nullptr)
    {
        env->CallVoidMethod(connection, g_jni.set_connect_timeout, CONNECT_TIMEOUT_MS);
        env->CallVoidMethod(connection, g_jni.set_read_timeout, READ_TIMEOUT_MS);
        env->CallVoidMethod(connection, g_jni.set_follow_redirects, JNI_FALSE);
        env->CallVoidMethod(connection, g_jni.set_request_method, env->NewStringUTF("POST"));
        env->CallVoidMethod(connection, g_jni.set_do_output, JNI_TRUE);
        env->CallVoidMethod(connection, g_jni.set_fixed_length, static_cast<jint>(size));
        env->CallVoidMethod(connection, g_jni.set_request_property, env->NewStringUTF("User-Agent"),
                            env->NewStringUTF(g_user_agent));
        env->CallVoidMethod(connection, g_jni.set_request_property, env->NewStringUTF("Content-Type"),
                            env->NewStringUTF(content_type));
        jbyteArray java_body = env->NewByteArray(static_cast<jsize>(size));
        if (java_body != nullptr)
        {
            env->SetByteArrayRegion(java_body, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(body));
        }
        jobject output = env->CallObjectMethod(connection, g_jni.get_output_stream);
        if (!threw(env, "can't send the request", error, error_size) && java_body != nullptr)
        {
            env->CallVoidMethod(output, g_jni.output_write, java_body);
            bool sent = !threw(env, "can't send the request", error, error_size);
            env->CallVoidMethod(output, g_jni.output_close);
            env->ExceptionClear();
            if (sent)
            {
                int32_t code = env->CallIntMethod(connection, g_jni.get_response_code);
                if (!threw(env, "no response", error, error_size))
                {
                    *status = static_cast<uint32_t>(code);
                    ok = true;
                }
            }
        }
        env->CallVoidMethod(connection, g_jni.disconnect);
        env->ExceptionClear();
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

NetHttpFetch net_http_backend_fetch(const char* url, NetHttpTransfer* transfer, char* error, size_t error_size)
{
    JNIEnv* env = jni_env();
    if (env == nullptr)
    {
        std::snprintf(error, error_size, "no JNI environment (%s)", SDL_GetError());
        return NetHttpFetch::failed;
    }
    // net_http's thread never returns to Java, so local references would pile
    // up across downloads: one frame per fetch, popped at the end.
    if (env->PushLocalFrame(LOCAL_FRAME_SIZE) != 0)
    {
        env->ExceptionClear();
        std::snprintf(error, error_size, "out of JNI local references");
        return NetHttpFetch::failed;
    }

    NetHttpFetch result     = NetHttpFetch::failed;
    int32_t      status     = 0;
    jobject      connection = open_following_redirects(env, url, &status, error, error_size);
    if (connection != nullptr)
    {
        // Content-Length as text: getContentLengthLong needs API 24.
        jstring length_key  = env->NewStringUTF("Content-Length");
        jstring length_text = static_cast<jstring>(env->CallObjectMethod(connection, g_jni.get_header_field,
                                                                         length_key));
        env->ExceptionClear();
        char text[32] = {};
        copy_java_string(env, length_text, text, sizeof(text));
        int64_t length = static_cast<int64_t>(std::strtoll(text, nullptr, 10));

        if (status != 200)
        {
            // 404 etc. are errors, not files (redirects were followed already).
            std::snprintf(error, error_size, "the server answered HTTP %d", status);
        }
        else if (transfer->max_bytes > 0 && length > transfer->max_bytes)
        {
            transfer->too_large = true;
        }
        else
        {
            jobject stream = env->CallObjectMethod(connection, g_jni.get_input_stream);
            if (!threw(env, "can't read the body", error, error_size))
            {
                net_http_transfer_set_total(transfer, length);
                jbyteArray java_buffer = env->NewByteArray(BUFFER_SIZE);
                uint8_t    buffer[BUFFER_SIZE];
                result = NetHttpFetch::ok;
                while (true)
                {
                    if (net_http_transfer_cancelled(transfer))
                    {
                        result = NetHttpFetch::cancelled;
                        break;
                    }
                    int32_t got = env->CallIntMethod(stream, g_jni.stream_read, java_buffer);
                    if (threw(env, "download interrupted", error, error_size))
                    {
                        result = NetHttpFetch::failed;
                        break;
                    }
                    if (got < 0)
                    {
                        break;   // the whole body
                    }
                    env->GetByteArrayRegion(java_buffer, 0, got, reinterpret_cast<jbyte*>(buffer));
                    if (!net_http_transfer_write(transfer, buffer, static_cast<size_t>(got)))
                    {
                        result = NetHttpFetch::failed;
                        break;
                    }
                }
                env->CallVoidMethod(stream, g_jni.stream_close);
                env->ExceptionClear();
            }
        }
        env->CallVoidMethod(connection, g_jni.disconnect);
        env->ExceptionClear();
    }
    env->PopLocalFrame(nullptr);
    return result;
}

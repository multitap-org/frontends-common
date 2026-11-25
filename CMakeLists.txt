# frontend_common: code shared by every frontend (piccolo, libretro-test-frontend,
# piccoletto, later Multitap): Vulkan instance/device, swapchain, frame
# rendering, Dear ImGui glue, settings, audio output, the input router.
# Plain static library, no frontend-specific code. Links host_common for its
# file helpers (host_fs: atomic writes); the other direction (hosts using
# frontend or SDL code) is what's not allowed.
# A library: it doesn't build alone. The frontend adds hosts-common first,
# defines imgui, SDL3::SDL3 and Vulkan::Vulkan (unless MULTITAP_GPU_VULKAN is OFF), and sets MULTITAP_DEPS_DIR to
# its deps folder (libsamplerate, gamemode, miniz, 7zip, monocypher; for the
# resampler tool also libretro-common), built here from their sources.
# Networking: a frontend that also adds multitap-net (deps/common/multitap-net,
# before this folder) gets frontend_net, the network's frontend half.

# Vulkan in the build (default): the window tries it first, OpenGL ES after.
# OFF: OpenGL ES only, nothing links libvulkan (the Android 5 APK: Android 5
# has no libvulkan.so). The Vulkan modules' entry points are then stubs
# (vulkan_none.cpp, app_shell_vulkan_none.cpp, debug_ui_vulkan_none.cpp);
# their headers stay (vulkan.h comes with the NDK and the system).
if(NOT DEFINED MULTITAP_GPU_VULKAN)
    set(MULTITAP_GPU_VULKAN ON)
endif()
if(MULTITAP_GPU_VULKAN)
    multitap_require(frontends-common host_common imgui SDL3::SDL3 Vulkan::Vulkan VAR:MULTITAP_DEPS_DIR)
else()
    multitap_require(frontends-common host_common imgui SDL3::SDL3 VAR:MULTITAP_DEPS_DIR)
endif()

# libsamplerate (deps/, BSD-2-Clause): audio_output's resampler, chosen by
# measurement (tools/resampler_measure.cpp). Built from its sources without
# our warning flags; it wants a generated config.h: ours, in the build folder.
set(LIBSAMPLERATE_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/libsamplerate_config")
file(WRITE "${LIBSAMPLERATE_CONFIG}/config.h"
     "#define PACKAGE \"libsamplerate\"\n#define VERSION \"0.2.2\"\n"
     "#define CPU_CLIPS_NEGATIVE 0\n#define CPU_CLIPS_POSITIVE 0\n"
     "#define HAVE_STDBOOL_H 1\n#define HAVE_STDINT_H 1\n"
     "#define ENABLE_SINC_FAST_CONVERTER 1\n#define ENABLE_SINC_MEDIUM_CONVERTER 1\n"
     "#define ENABLE_SINC_BEST_CONVERTER 1\n")
add_library(multitap_dep_libsamplerate STATIC
    "${MULTITAP_DEPS_DIR}/libsamplerate/src/samplerate.c"
    "${MULTITAP_DEPS_DIR}/libsamplerate/src/src_linear.c"
    "${MULTITAP_DEPS_DIR}/libsamplerate/src/src_sinc.c"
    "${MULTITAP_DEPS_DIR}/libsamplerate/src/src_zoh.c")
target_include_directories(multitap_dep_libsamplerate SYSTEM PUBLIC "${MULTITAP_DEPS_DIR}/libsamplerate/include")
target_include_directories(multitap_dep_libsamplerate PRIVATE "${LIBSAMPLERATE_CONFIG}")
target_compile_definitions(multitap_dep_libsamplerate PRIVATE HAVE_CONFIG_H)
# Optimized even in Debug builds: it runs for every audio frame and we don't debug it.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(multitap_dep_libsamplerate PRIVATE -O2)
endif()
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(multitap_dep_libsamplerate PUBLIC ${MATH_LIBRARY})
endif()

# Zip and 7z reading (archive.cpp, content_cache.cpp): only for frontends
# that have deps/miniz and deps/7zip (piccoletto runs no games: it doesn't).
set(MULTITAP_ARCHIVES OFF)
if(EXISTS "${MULTITAP_DEPS_DIR}/miniz/miniz_zip.c" AND EXISTS "${MULTITAP_DEPS_DIR}/7zip/C/7zDec.c")
    set(MULTITAP_ARCHIVES ON)

    # miniz (deps/, MIT): reading zip archives (archive.cpp), nothing else:
    # inflate + the zip reader. miniz.c only adds its CRC-32 and allocation
    # helpers (the zlib-style API, deflate and zip writing are switched off).
    # Its headers include a CMake-generated miniz_export.h: ours, in the build
    # folder (a static library: nothing to export).
    set(MINIZ_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/miniz_config")
    file(WRITE "${MINIZ_CONFIG}/miniz_export.h" "#pragma once\n#define MINIZ_EXPORT\n")
    add_library(multitap_dep_miniz STATIC
        "${MULTITAP_DEPS_DIR}/miniz/miniz.c"
        "${MULTITAP_DEPS_DIR}/miniz/miniz_tinfl.c"
        "${MULTITAP_DEPS_DIR}/miniz/miniz_zip.c")
    target_include_directories(multitap_dep_miniz SYSTEM PUBLIC "${MULTITAP_DEPS_DIR}/miniz" "${MINIZ_CONFIG}")
    # No stdio either: archives are read through SDL (UTF-8 paths on Windows).
    # (MINIZ_NO_DEFLATE_APIS also turns off zip writing, miniz.h does that itself.)
    target_compile_definitions(multitap_dep_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_APIS
                               MINIZ_NO_DEFLATE_APIS)

    # 7-Zip's C code (deps/7zip/C, the LZMA SDK, public domain): reading 7z
    # archives (archive.cpp). Only the decoder: LZMA, LZMA2 and the BCJ / BCJ2 /
    # delta filters (7zDec.c needs Bra*, Delta and CpuArch for those). No PPMd
    # (rare in 7z files people make: refused as unsupported), no asm, no encoder.
    set(SEVEN_ZIP_C "${MULTITAP_DEPS_DIR}/7zip/C")
    add_library(multitap_dep_7z STATIC
        "${SEVEN_ZIP_C}/7zArcIn.c" "${SEVEN_ZIP_C}/7zBuf.c" "${SEVEN_ZIP_C}/7zBuf2.c"
        "${SEVEN_ZIP_C}/7zCrc.c" "${SEVEN_ZIP_C}/7zCrcOpt.c" "${SEVEN_ZIP_C}/7zDec.c"
        "${SEVEN_ZIP_C}/7zStream.c" "${SEVEN_ZIP_C}/Bcj2.c" "${SEVEN_ZIP_C}/Bra.c"
        "${SEVEN_ZIP_C}/Bra86.c" "${SEVEN_ZIP_C}/BraIA64.c" "${SEVEN_ZIP_C}/CpuArch.c"
        "${SEVEN_ZIP_C}/Delta.c" "${SEVEN_ZIP_C}/Lzma2Dec.c" "${SEVEN_ZIP_C}/LzmaDec.c"
        # SHA-256 (file_hash.cpp: checking downloads).
        "${SEVEN_ZIP_C}/Sha256.c" "${SEVEN_ZIP_C}/Sha256Opt.c")
    target_include_directories(multitap_dep_7z SYSTEM PUBLIC "${SEVEN_ZIP_C}")
    target_compile_definitions(multitap_dep_7z PRIVATE Z7_EXTRACT_ONLY)
    # Decoding runs when a game loads; optimized even in Debug builds, like libsamplerate.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(multitap_dep_miniz PRIVATE -O2)
        target_compile_options(multitap_dep_7z PRIVATE -O2)
    endif()
endif()

# The network core (ENet, Monocypher, the beacon's wire library,
# libdatachannel, Mbed TLS on Android) is built by multitap-net, which also
# tells us which transports it has (MULTITAP_HAVE_RTC, MULTITAP_HAVE_WEBRTC_JS).

add_library(frontend_common STATIC
    debug_ui.cpp
    text_input.cpp
    app_shell.cpp
    app_links.cpp
    audio_output.cpp
    haptics.cpp
    frame_timing.cpp
    settings.cpp
    app_paths.cpp
    app_assets.cpp
    input_hold.cpp
    shader_check.cpp
    viewport.cpp
    frame_delta.cpp
    state_delta.cpp
    rewind_buffer.cpp
    rewind_recorder.cpp
    video_texture.cpp
    video_output.cpp
    shown_image.cpp
    screen_effect_params.cpp
    screenshot_writer.cpp
    gl_context.cpp
    gl_texture.cpp
    gl_picture.cpp
    gl_screen_effect.cpp
    gl_shader_chain.cpp
    window_size.cpp
    localization.cpp
    rate_control.cpp
    frame_delay.cpp
    input_devices.cpp
    input_bindings.cpp
    input_sdl.cpp
    input_config.cpp
    input_sdl_mapping.cpp
    input_layouts.cpp
)
# app_links: AF_UNIX sockets (Winsock) and the per-user scheme in the registry.
if(WIN32)
    target_link_libraries(frontend_common PRIVATE ws2_32 advapi32)
endif()
if(MULTITAP_GPU_VULKAN)
    target_sources(frontend_common PRIVATE vk_context.cpp vk_barrier.cpp vk_swapchain.cpp renderer.cpp vk_texture.cpp
                   picture.cpp screen_effect.cpp app_shell_vulkan.cpp debug_ui_vulkan.cpp)
    target_link_libraries(frontend_common PUBLIC Vulkan::Vulkan)
else()
    target_sources(frontend_common PRIVATE vulkan_none.cpp app_shell_vulkan_none.cpp debug_ui_vulkan_none.cpp)
endif()

# Who drives the loop and where the frontend's folder lives (app_loop.h,
# app_storage.h): the browser's own versions under Emscripten; the page's half
# is in web/ (multitap-page.js, multitap-storage.js, sw.js), which a frontend's
# web package copies next to its page.
if(EMSCRIPTEN)
    target_sources(frontend_common PRIVATE app_loop_web.cpp app_storage_web.cpp)
else()
    target_sources(frontend_common PRIVATE app_loop.cpp app_storage.cpp)
endif()

# GameMode (deps/gamemode, BSD-3-Clause): only its client header, which
# loads libgamemode at run time (nothing linked). Linux only; elsewhere a
# stub, chosen here instead of #if in the code. (The Android build says
# "Linux" to CMake, see hosts-common's cmake/android-toolchain.cmake: hence NOT ANDROID.)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    target_sources(frontend_common PRIVATE game_mode_linux.cpp)
    target_include_directories(frontend_common SYSTEM PRIVATE "${MULTITAP_DEPS_DIR}/gamemode/lib")
    target_link_libraries(frontend_common PRIVATE ${CMAKE_DL_LIBS})
else()
    target_sources(frontend_common PRIVATE game_mode_none.cpp)
endif()

# Shader presets (shader_chain): only librashader's MIT C header from
# deps/librashader (nothing built from its Rust tree; the library is loaded
# at run time). Built only for frontends that have that submodule.
set(MULTITAP_LIBRASHADER_INCLUDE "${MULTITAP_DEPS_DIR}/librashader/include")
if(EXISTS "${MULTITAP_LIBRASHADER_INCLUDE}/librashader.h")
    if(MULTITAP_GPU_VULKAN)
        target_sources(frontend_common PRIVATE shader_chain.cpp)
    endif()
    target_include_directories(frontend_common SYSTEM PUBLIC "${MULTITAP_LIBRASHADER_INCLUDE}")
    target_compile_definitions(frontend_common PUBLIC LIBRA_RUNTIME_VULKAN
        PRIVATE "MULTITAP_LIBRASHADER_FILE=\"librashader${CMAKE_SHARED_LIBRARY_SUFFIX}\"")
    set(MULTITAP_HAS_SHADER_CHAIN ON PARENT_SCOPE)
endif()

target_include_directories(frontend_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The GL path (gl_context.h) takes GLES 2's types and constants from SDL's own
# copy of the Khronos headers, not the system's: the functions are looked up
# at run time, so no GL library or header is needed to build (MSYS2's UCRT64
# has no GLES2 headers). The guards are Khronos's, so a file that also
# includes the system's (ImGui's GLES 2 backend) still builds.
target_compile_definitions(frontend_common PUBLIC SDL_USE_BUILTIN_OPENGL_DEFINITIONS)
target_link_libraries(frontend_common PUBLIC imgui SDL3::SDL3 host_common multitap_dep_libsamplerate)
if(MULTITAP_ARCHIVES)
    target_sources(frontend_common PRIVATE archive.cpp content_cache.cpp)
    target_link_libraries(frontend_common PRIVATE multitap_dep_miniz multitap_dep_7z)
endif()

# Downloads (frontend_download: signature checks, HTTP downloads, SHA-256 of
# files) for the core downloader. Only for frontends that have deps/monocypher
# and zip reading, and where net_http has a backend (net_http_backend.h):
# WinHTTP on Windows (part of the system), HttpURLConnection through JNI on
# Android (part of the system), libcurl where it's found (Linux: the system's;
# the Flatpak runtime has it).
# Monocypher (deps/monocypher, BSD-2-Clause or CC0): EdDSA signature checks.
# The same target as multitap-net's, defined once.
set(MULTITAP_DOWNLOADS OFF)
set(MULTITAP_HTTP_BACKEND "")
if(MULTITAP_ARCHIVES AND EXISTS "${MULTITAP_DEPS_DIR}/monocypher/src/monocypher.c")
    if(WIN32)
        set(MULTITAP_HTTP_BACKEND winhttp)
    elseif(ANDROID)
        set(MULTITAP_HTTP_BACKEND android)
    else()
        find_package(CURL QUIET)
        if(CURL_FOUND)
            set(MULTITAP_HTTP_BACKEND curl)
        endif()
    endif()
endif()
if(MULTITAP_HTTP_BACKEND)
    set(MULTITAP_DOWNLOADS ON)
    if(NOT TARGET multitap_dep_monocypher)
        add_library(multitap_dep_monocypher STATIC "${MULTITAP_DEPS_DIR}/monocypher/src/monocypher.c")
        target_include_directories(multitap_dep_monocypher SYSTEM PUBLIC "${MULTITAP_DEPS_DIR}/monocypher/src")
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(multitap_dep_monocypher PRIVATE -O2)
        endif()
    endif()

    add_library(frontend_download STATIC
        signature.cpp
        net_http.cpp
        net_http_${MULTITAP_HTTP_BACKEND}.cpp
        file_hash.cpp
    )
    target_include_directories(frontend_download PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(frontend_download PUBLIC host_common SDL3::SDL3
                          PRIVATE multitap_dep_monocypher multitap_dep_7z)
    if(MULTITAP_HTTP_BACKEND STREQUAL "winhttp")
        target_link_libraries(frontend_download PRIVATE winhttp)
    elseif(MULTITAP_HTTP_BACKEND STREQUAL "curl")
        target_link_libraries(frontend_download PRIVATE CURL::libcurl)
    endif()
    message(STATUS "Downloads: net_http with ${MULTITAP_HTTP_BACKEND}")
    multitap_set_project_flags(frontend_download)
endif()

# Vulkan validation layers + debug messenger are enabled in Debug builds only.
target_compile_definitions(frontend_common PRIVATE $<$<CONFIG:Debug>:MULTITAP_VULKAN_VALIDATION=1>)
if(MULTITAP_GL_ES2)
    # ImGui's GL backend is built for GLES 2 (top-level option): ask SDL for a GLES 2.0 context.
    target_compile_definitions(frontend_common PRIVATE MULTITAP_GL_ES2=1)
endif()

multitap_set_project_flags(frontend_common)

if(TARGET multitap_net)
    # frontend_net: the network's frontend half, on top of multitap-net (the
    # core: connections, pairing, access, SHARE). What needs frontend code:
    # remote play (net_stream, frame_delta), remote pads in the router
    # (net_input, input_net), settings from a client (net_settings), the game
    # folders with archives (net_content_folders), and the threads that run it
    # all (net_service for the host, net_client_service for piccoletto).
    add_library(frontend_net STATIC
        net_settings.cpp
        net_service.cpp
        net_client_service.cpp
        net_stream.cpp
        net_input.cpp
        input_net.cpp
    )
    if(MULTITAP_ARCHIVES)
        target_sources(frontend_net PRIVATE net_content_folders.cpp)   # the host's game folders browse archives
    endif()
    target_include_directories(frontend_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    find_package(Threads REQUIRED)   # net_service's and net_client_service's threads
    target_link_libraries(frontend_net PUBLIC multitap_net frontend_common Threads::Threads)
    multitap_set_project_flags(frontend_net)

    # net_probe: LAN discovery on a real network, without a frontend.
    add_executable(net_probe tools/net_probe.cpp)
    target_link_libraries(net_probe PRIVATE frontend_net)
    multitap_set_project_flags(net_probe)
endif()

# Build-time tool: a frontend's strings.txt -> its string IDs, English table
# and .pot (see tools/strings_generator.cpp).
# Cross builds (Android, Emscripten) can't run what they build: there it's an
# imported program, built for the build machine first (piccolo's
# android/build-apk.sh, web/build.sh).
# A frontend without translated strings (piccoletto, for now) doesn't pass
# it: then there's no generator target.
if(ANDROID OR EMSCRIPTEN)
    if(MULTITAP_STRINGS_GENERATOR)
        if(NOT EXISTS "${MULTITAP_STRINGS_GENERATOR}")
            message(FATAL_ERROR "MULTITAP_STRINGS_GENERATOR: ${MULTITAP_STRINGS_GENERATOR} doesn't exist")
        endif()
        add_executable(multitap_strings_generator IMPORTED GLOBAL)
        set_target_properties(multitap_strings_generator PROPERTIES IMPORTED_LOCATION "${MULTITAP_STRINGS_GENERATOR}")
    endif()
else()
    add_executable(multitap_strings_generator tools/strings_generator.cpp)
    target_link_libraries(multitap_strings_generator PRIVATE host_common)
    multitap_set_project_flags(multitap_strings_generator)
endif()

if(MULTITAP_BUILD_TESTS)
    add_executable(frontend_settings_test tests/settings_test.cpp)
    target_link_libraries(frontend_settings_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_settings_test)

    add_test(NAME frontend_settings
             COMMAND frontend_settings_test "${CMAKE_CURRENT_BINARY_DIR}/settings_test")
    set_tests_properties(frontend_settings PROPERTIES
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # The migration check needs a scratch folder that doesn't exist yet: a
    # fixture empties it first.
    # The link scheme's hand-over between copies (app_links.h): sockets, so
    # not in the browser; Android has no hand-over.
    if(NOT EMSCRIPTEN AND NOT ANDROID AND NOT WIN32)
        add_executable(frontend_app_links_test tests/app_links_test.cpp)
        target_link_libraries(frontend_app_links_test PRIVATE frontend_common)
        multitap_set_project_flags(frontend_app_links_test)
        add_test(NAME frontend_app_links COMMAND frontend_app_links_test)
        set_tests_properties(frontend_app_links PROPERTIES
                             PASS_REGULAR_EXPRESSION "app links: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endif()

    add_executable(frontend_app_paths_test tests/app_paths_test.cpp)
    target_link_libraries(frontend_app_paths_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_app_paths_test)

    set(APP_PATHS_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/app_paths_test")
    add_test(NAME frontend_app_paths_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${APP_PATHS_SCRATCH}")
    set_tests_properties(frontend_app_paths_clean PROPERTIES FIXTURES_SETUP app_paths_scratch)
    add_test(NAME frontend_app_paths COMMAND frontend_app_paths_test "${APP_PATHS_SCRATCH}")
    set_tests_properties(frontend_app_paths PROPERTIES
                         FIXTURES_REQUIRED app_paths_scratch
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_viewport_test tests/viewport_test.cpp)
    target_link_libraries(frontend_viewport_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_viewport_test)
    add_test(NAME frontend_viewport COMMAND frontend_viewport_test)
    set_tests_properties(frontend_viewport PROPERTIES
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # The slow shader check: a fast preset goes back to idle, a slow one asks.
    add_executable(frontend_shader_check_test tests/shader_check_test.cpp)
    target_link_libraries(frontend_shader_check_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_shader_check_test)
    add_test(NAME frontend_shader_check COMMAND frontend_shader_check_test)
    set_tests_properties(frontend_shader_check PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_input_test tests/input_test.cpp)
    target_link_libraries(frontend_input_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_input_test)
    add_test(NAME frontend_input COMMAND frontend_input_test)

    add_executable(frontend_input_sdl_mapping_test tests/input_sdl_mapping_test.cpp)
    target_link_libraries(frontend_input_sdl_mapping_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_input_sdl_mapping_test)
    add_test(NAME frontend_input_sdl_mapping COMMAND frontend_input_sdl_mapping_test)
    set_tests_properties(frontend_input PROPERTIES
                         PASS_REGULAR_EXPRESSION "input: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_window_size_test tests/window_size_test.cpp)
    target_link_libraries(frontend_window_size_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_window_size_test)
    add_test(NAME frontend_window_size COMMAND frontend_window_size_test)
    set_tests_properties(frontend_window_size PROPERTIES
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_localization_test tests/localization_test.cpp)
    target_link_libraries(frontend_localization_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_localization_test)
    add_test(NAME frontend_localization
             COMMAND frontend_localization_test "${CMAKE_CURRENT_BINARY_DIR}/localization_test")
    set_tests_properties(frontend_localization PROPERTIES
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_frame_delay_test tests/frame_delay_test.cpp)
    target_link_libraries(frontend_frame_delay_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_frame_delay_test)
    add_test(NAME frontend_frame_delay COMMAND frontend_frame_delay_test)
    set_tests_properties(frontend_frame_delay PROPERTIES PASS_REGULAR_EXPRESSION "frame delay: all checks passed")

    # Needs a display and OpenGL ES; skipped (77) without them.
    add_executable(frontend_gl_video_test tests/gl_video_test.cpp)
    target_link_libraries(frontend_gl_video_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_gl_video_test)
    add_test(NAME frontend_gl_video COMMAND frontend_gl_video_test "${CMAKE_CURRENT_BINARY_DIR}/gl_video_test")
    set_tests_properties(frontend_gl_video PROPERTIES
                         SKIP_RETURN_CODE 77
                         PASS_REGULAR_EXPRESSION "gl video: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_screen_effect_test tests/screen_effect_test.cpp)
    target_link_libraries(frontend_screen_effect_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_screen_effect_test)
    foreach(api IN ITEMS vulkan vulkan11 gl)
        add_test(NAME frontend_screen_effect_${api} COMMAND frontend_screen_effect_test ${api})
        set_tests_properties(frontend_screen_effect_${api} PROPERTIES
                             SKIP_RETURN_CODE 77
                             PASS_REGULAR_EXPRESSION "screen effect \\(${api}\\): all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endforeach()

    add_executable(frontend_frame_delta_test tests/frame_delta_test.cpp)
    target_link_libraries(frontend_frame_delta_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_frame_delta_test)
    add_test(NAME frontend_frame_delta COMMAND frontend_frame_delta_test)
    set_tests_properties(frontend_frame_delta PROPERTIES
                         PASS_REGULAR_EXPRESSION "frame delta: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_state_delta_test tests/state_delta_test.cpp)
    target_link_libraries(frontend_state_delta_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_state_delta_test)
    add_test(NAME frontend_state_delta COMMAND frontend_state_delta_test)
    set_tests_properties(frontend_state_delta PROPERTIES
                         PASS_REGULAR_EXPRESSION "state delta: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_screenshot_writer_test tests/screenshot_writer_test.cpp)
    target_link_libraries(frontend_screenshot_writer_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_screenshot_writer_test)
    add_test(NAME frontend_screenshot_writer COMMAND frontend_screenshot_writer_test
             "${CMAKE_CURRENT_BINARY_DIR}/screenshot_writer_test")
    set_tests_properties(frontend_screenshot_writer PROPERTIES
                         PASS_REGULAR_EXPRESSION "screenshot writer: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_rewind_buffer_test tests/rewind_buffer_test.cpp)
    target_link_libraries(frontend_rewind_buffer_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_rewind_buffer_test)
    add_test(NAME frontend_rewind_buffer COMMAND frontend_rewind_buffer_test)
    set_tests_properties(frontend_rewind_buffer PROPERTIES
                         PASS_REGULAR_EXPRESSION "rewind buffer: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_rewind_recorder_test tests/rewind_recorder_test.cpp)
    target_link_libraries(frontend_rewind_recorder_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_rewind_recorder_test)
    add_test(NAME frontend_rewind_recorder COMMAND frontend_rewind_recorder_test)
    set_tests_properties(frontend_rewind_recorder PROPERTIES
                         PASS_REGULAR_EXPRESSION "rewind recorder: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|ThreadSanitizer")

    add_executable(frontend_rate_control_test tests/rate_control_test.cpp)
    target_link_libraries(frontend_rate_control_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_rate_control_test)
    add_test(NAME frontend_rate_control COMMAND frontend_rate_control_test)
    set_tests_properties(frontend_rate_control PROPERTIES
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    if(MULTITAP_ARCHIVES)
        # Zip and 7z files in tests/data (README.md there says how they were made).
        add_executable(frontend_archive_test tests/archive_test.cpp)
        target_link_libraries(frontend_archive_test PRIVATE frontend_common multitap_dep_miniz)
        multitap_set_project_flags(frontend_archive_test)
        add_test(NAME frontend_archive
                 COMMAND frontend_archive_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/data" "${CMAKE_CURRENT_BINARY_DIR}/archive_test")
        set_tests_properties(frontend_archive PROPERTIES
                             PASS_REGULAR_EXPRESSION "archive: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

        add_executable(frontend_content_cache_test tests/content_cache_test.cpp)
        target_link_libraries(frontend_content_cache_test PRIVATE frontend_common)
        multitap_set_project_flags(frontend_content_cache_test)
        set(CONTENT_CACHE_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/content_cache_test")
        add_test(NAME frontend_content_cache_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${CONTENT_CACHE_SCRATCH}")
        set_tests_properties(frontend_content_cache_clean PROPERTIES FIXTURES_SETUP content_cache_scratch)
        add_test(NAME frontend_content_cache
                 COMMAND frontend_content_cache_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/data" "${CONTENT_CACHE_SCRATCH}")
        set_tests_properties(frontend_content_cache PROPERTIES
                             FIXTURES_REQUIRED content_cache_scratch
                             PASS_REGULAR_EXPRESSION "content cache: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endif()

    if(MULTITAP_DOWNLOADS)
        add_executable(frontend_signature_test tests/signature_test.cpp)
        target_link_libraries(frontend_signature_test PRIVATE frontend_download)
        multitap_set_project_flags(frontend_signature_test)
        add_test(NAME frontend_signature COMMAND frontend_signature_test)
        set_tests_properties(frontend_signature PROPERTIES
                             PASS_REGULAR_EXPRESSION "signature: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

        add_executable(frontend_net_http_test tests/net_http_test.cpp)
        target_link_libraries(frontend_net_http_test PRIVATE frontend_download)
        multitap_set_project_flags(frontend_net_http_test)
        set(NET_HTTP_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/net_http_test")
        add_test(NAME frontend_net_http_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${NET_HTTP_SCRATCH}")
        set_tests_properties(frontend_net_http_clean PROPERTIES FIXTURES_SETUP net_http_scratch)
        add_test(NAME frontend_net_http
                 COMMAND frontend_net_http_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/data" "${NET_HTTP_SCRATCH}")
        set_tests_properties(frontend_net_http PROPERTIES
                             FIXTURES_REQUIRED net_http_scratch
                             PASS_REGULAR_EXPRESSION "net_http: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endif()

    # Only for frontends that have deps/libretro-common (piccoletto doesn't:
    # it runs no cores).
    if(EXISTS "${MULTITAP_DEPS_DIR}/libretro-common/audio/resampler/drivers/sinc_resampler.c")
        # Resampler comparison (plan-milestones, milestone 2): every candidate is
        # built here from deps/ (read-only, without our warning flags) only for
        # this tool. The one chosen moves into the real audio path later.
        set(MULTITAP_DEPS "${MULTITAP_DEPS_DIR}")

        # libretro-common's sinc driver (MIT-style license in the file itself):
        # its C path only (no SIMD mask), plus the aligned allocator it needs.
        add_library(multitap_dep_retro_sinc STATIC
            "${MULTITAP_DEPS}/libretro-common/audio/resampler/drivers/sinc_resampler.c"
            "${MULTITAP_DEPS}/libretro-common/memmap/memalign.c")
        target_include_directories(multitap_dep_retro_sinc SYSTEM PUBLIC "${MULTITAP_DEPS}/libretro-common/include")

        foreach(dep multitap_dep_retro_sinc)
            if(MATH_LIBRARY)
                target_link_libraries(${dep} PUBLIC ${MATH_LIBRARY})
            endif()
        endforeach()

        add_executable(resampler_measure tools/resampler_measure.cpp)
        target_link_libraries(resampler_measure PRIVATE SDL3::SDL3 multitap_dep_libsamplerate
                              multitap_dep_retro_sinc)
        multitap_set_project_flags(resampler_measure)
        add_test(NAME frontend_resampler_measure COMMAND resampler_measure --quick)
        set_tests_properties(frontend_resampler_measure PROPERTIES
                             PASS_REGULAR_EXPRESSION "all candidates ran"
                             FAIL_REGULAR_EXPRESSION "FAILED|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endif()
endif()

if(MULTITAP_BUILD_TESTS AND TARGET frontend_net)
    # Games from a client: messages, pages, and the game folders (with archives).
    add_executable(frontend_net_content_test tests/net_content_test.cpp)
    target_link_libraries(frontend_net_content_test PRIVATE frontend_net)
    if(MULTITAP_ARCHIVES)
        target_compile_definitions(frontend_net_content_test PRIVATE MULTITAP_TEST_ARCHIVES)
    endif()
    multitap_set_project_flags(frontend_net_content_test)
    add_test(NAME frontend_net_content
             COMMAND frontend_net_content_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/data" "${CMAKE_CURRENT_BINARY_DIR}/net_content_test")
    set_tests_properties(frontend_net_content PROPERTIES
                         PASS_REGULAR_EXPRESSION "net content: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # SHARE through net_service (multitap-net checks the protocol and the engine).
    add_executable(frontend_net_share_service_test tests/net_share_service_test.cpp)
    target_link_libraries(frontend_net_share_service_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_share_service_test)
    add_test(NAME frontend_net_share_service
             COMMAND frontend_net_share_service_test "${CMAKE_CURRENT_BINARY_DIR}/net_share_service_test")
    set_tests_properties(frontend_net_share_service PROPERTIES
                         PASS_REGULAR_EXPRESSION "net share service: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    add_executable(frontend_net_share_link_test tests/net_share_link_test.cpp)
    target_link_libraries(frontend_net_share_link_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_share_link_test)
    add_test(NAME frontend_net_share_link
             COMMAND frontend_net_share_link_test "${CMAKE_CURRENT_BINARY_DIR}/net_share_link_test")
    set_tests_properties(frontend_net_share_link PROPERTIES
                         PASS_REGULAR_EXPRESSION "net share link: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    # Guest links end to end: the real `beacon` binary, a host and guests over
    # UDP and WebRTC (and the card through net_http where it's built).
    if(MULTITAP_HAVE_RTC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(frontend_net_guest_link_test tests/net_guest_link_test.cpp)
        target_link_libraries(frontend_net_guest_link_test PRIVATE frontend_net)
        if(TARGET frontend_download)
            target_link_libraries(frontend_net_guest_link_test PRIVATE frontend_download)
            target_compile_definitions(frontend_net_guest_link_test PRIVATE MULTITAP_TEST_HTTP)
        endif()
        add_dependencies(frontend_net_guest_link_test beacon)
        multitap_set_project_flags(frontend_net_guest_link_test)
        add_test(NAME frontend_net_guest_link
                 COMMAND frontend_net_guest_link_test $<TARGET_FILE:beacon> "${CMAKE_CURRENT_BINARY_DIR}/net_guest_link_test")
        set_tests_properties(frontend_net_guest_link PROPERTIES
                             PASS_REGULAR_EXPRESSION "net guest link: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endif()

    # SHARE across the internet: against the real beacon server code (Linux
    # only, like the server and multitap-net's multitap_net_beacon).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(frontend_net_share_link_beacon_test tests/net_share_link_beacon_test.cpp)
        target_link_libraries(frontend_net_share_link_beacon_test PRIVATE frontend_net beacon_core)
        multitap_set_project_flags(frontend_net_share_link_beacon_test)
        add_test(NAME frontend_net_share_link_beacon
                 COMMAND frontend_net_share_link_beacon_test "${CMAKE_CURRENT_BINARY_DIR}/net_share_link_beacon_test")
        set_tests_properties(frontend_net_share_link_beacon PROPERTIES
                             PASS_REGULAR_EXPRESSION "net share link beacon: all checks passed"
                             FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
    endif()
    # Settings and actions from a client: messages, pages, the host's side.
    add_executable(frontend_net_settings_test tests/net_settings_test.cpp)
    target_link_libraries(frontend_net_settings_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_settings_test)
    add_test(NAME frontend_net_settings COMMAND frontend_net_settings_test)
    set_tests_properties(frontend_net_settings PROPERTIES
                         PASS_REGULAR_EXPRESSION "net settings: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # The network on its own thread: events through the queue, sending under the lock.
    add_executable(frontend_net_service_test tests/net_service_test.cpp)
    target_link_libraries(frontend_net_service_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_service_test)
    add_test(NAME frontend_net_service
             COMMAND frontend_net_service_test "${CMAKE_CURRENT_BINARY_DIR}/net_service_test")
    set_tests_properties(frontend_net_service PROPERTIES
                         PASS_REGULAR_EXPRESSION "net service: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # The client's network on its own thread, with remote play against a net_service host.
    add_executable(frontend_net_client_service_test tests/net_client_service_test.cpp)
    target_link_libraries(frontend_net_client_service_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_client_service_test)
    add_test(NAME frontend_net_client_service
             COMMAND frontend_net_client_service_test "${CMAKE_CURRENT_BINARY_DIR}/net_client_service_test")
    set_tests_properties(frontend_net_client_service PROPERTIES
                         PASS_REGULAR_EXPRESSION "net client service: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # Remote play's stream: chunks, reassembly, loss and keyframes, audio.
    add_executable(frontend_net_stream_test tests/net_stream_test.cpp)
    target_link_libraries(frontend_net_stream_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_stream_test)
    add_test(NAME frontend_net_stream COMMAND frontend_net_stream_test)
    set_tests_properties(frontend_net_stream PROPERTIES
                         PASS_REGULAR_EXPRESSION "net stream: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # Remote pads: packets, then into the router.
    add_executable(frontend_input_net_test tests/input_net_test.cpp)
    target_link_libraries(frontend_input_net_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_input_net_test)
    add_test(NAME frontend_input_net COMMAND frontend_input_net_test)
    set_tests_properties(frontend_input_net PROPERTIES
                         PASS_REGULAR_EXPRESSION "all input_net checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
endif()

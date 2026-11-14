# frontend_common: code shared by every frontend (piccolo, libretro-test-frontend,
# piccoletto, later Multitap): Vulkan instance/device, swapchain, frame
# rendering, Dear ImGui glue, settings, audio output, the input router.
# Plain static library, no frontend-specific code. Links host_common for its
# file helpers (host_fs: atomic writes); the other direction (hosts using
# frontend or SDL code) is what's not allowed.
# A library: it doesn't build alone. The frontend adds hosts-common first,
# defines imgui, SDL3::SDL3 and Vulkan::Vulkan, and sets MULTITAP_DEPS_DIR to
# its deps folder (libsamplerate, gamemode, miniz, 7zip, enet, monocypher; for the
# resampler tool also libretro-common), built here from their sources.

multitap_require(frontends-common host_common imgui SDL3::SDL3 Vulkan::Vulkan VAR:MULTITAP_DEPS_DIR)

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

# ENet (deps/enet, MIT): connections over UDP for frontend_net. Built from
# its sources without our warning flags. Its configure checks are replaced by
# what every POSIX system we target has (Linux, macOS, Android); Windows uses
# win32.c and Winsock instead.
# Optional: only frontends whose deps/ has enet get frontend_net (others,
# like libretro-test-frontend, build as before).
set(MULTITAP_ENET "${MULTITAP_DEPS_DIR}/enet")
if(EXISTS "${MULTITAP_ENET}/host.c")
    set(MULTITAP_HAVE_ENET ON)
else()
    set(MULTITAP_HAVE_ENET OFF)
endif()
if(MULTITAP_HAVE_ENET)
    add_library(multitap_dep_enet STATIC
        "${MULTITAP_ENET}/callbacks.c"
        "${MULTITAP_ENET}/compress.c"
        "${MULTITAP_ENET}/host.c"
        "${MULTITAP_ENET}/list.c"
        "${MULTITAP_ENET}/packet.c"
        "${MULTITAP_ENET}/peer.c"
        "${MULTITAP_ENET}/protocol.c")
    target_include_directories(multitap_dep_enet SYSTEM PUBLIC "${MULTITAP_ENET}/include")
    if(WIN32)
        target_sources(multitap_dep_enet PRIVATE "${MULTITAP_ENET}/win32.c")
        target_link_libraries(multitap_dep_enet PUBLIC ws2_32 winmm)
    else()
        target_sources(multitap_dep_enet PRIVATE "${MULTITAP_ENET}/unix.c")
        target_compile_definitions(multitap_dep_enet PRIVATE
            HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1
            HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
    endif()
endif()

# Monocypher (deps/monocypher, BSD-2-Clause or CC0): X25519, ChaCha20-Poly1305
# and BLAKE2b for frontend_net's Noise handshake. One C file; built without
# our warning flags. Optional like ENet.
set(MULTITAP_MONOCYPHER "${MULTITAP_DEPS_DIR}/monocypher")
if(MULTITAP_HAVE_ENET AND NOT EXISTS "${MULTITAP_MONOCYPHER}/src/monocypher.c")
    message(FATAL_ERROR "deps/enet is there but deps/monocypher isn't: frontend_net needs both")
endif()
if(MULTITAP_HAVE_ENET)
    # optional/: SHA-512, for CPace (pairing).
    add_library(multitap_dep_monocypher STATIC "${MULTITAP_MONOCYPHER}/src/monocypher.c"
                "${MULTITAP_MONOCYPHER}/src/optional/monocypher-ed25519.c")
    target_include_directories(multitap_dep_monocypher SYSTEM PUBLIC "${MULTITAP_MONOCYPHER}/src"
                               "${MULTITAP_MONOCYPHER}/src/optional")
    # Optimized even in Debug builds: handshakes and every packet go through it.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(multitap_dep_monocypher PRIVATE -O2)
    endif()
endif()

add_library(frontend_common STATIC
    vk_context.cpp
    vk_barrier.cpp
    vk_swapchain.cpp
    renderer.cpp
    debug_ui.cpp
    vk_texture.cpp
    app_shell.cpp
    audio_output.cpp
    frame_timing.cpp
    settings_file.cpp
    settings.cpp
    app_paths.cpp
    viewport.cpp
    picture.cpp
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

# GameMode (deps/gamemode, BSD-3-Clause): only its client header, which
# loads libgamemode at run time (nothing linked). Linux only; elsewhere a
# stub, chosen here instead of #if in the code.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(frontend_common PRIVATE game_mode_linux.cpp)
    target_include_directories(frontend_common SYSTEM PRIVATE "${MULTITAP_DEPS_DIR}/gamemode/lib")
    target_link_libraries(frontend_common PRIVATE ${CMAKE_DL_LIBS})
else()
    target_sources(frontend_common PRIVATE game_mode_none.cpp)
endif()

target_include_directories(frontend_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frontend_common PUBLIC imgui SDL3::SDL3 Vulkan::Vulkan host_common multitap_dep_libsamplerate)
if(MULTITAP_ARCHIVES)
    target_sources(frontend_common PRIVATE archive.cpp content_cache.cpp)
    target_link_libraries(frontend_common PRIVATE multitap_dep_miniz multitap_dep_7z)
endif()

# Downloads (frontend_download: signature checks, HTTP downloads, SHA-256 of
# files) for the core downloader. Only for frontends that have deps/monocypher
# and zip reading, and where libcurl is found: the system's on Linux (the
# Flatpak runtime has it). Windows (curl built with Schannel) and Android (JNI)
# come later; until then frontend_download isn't built there.
# Monocypher (deps/monocypher, BSD-2-Clause or CC0): EdDSA signature checks.
# The same target as frontend_net's (netcode), defined once.
set(MULTITAP_DOWNLOADS OFF)
if(MULTITAP_ARCHIVES AND EXISTS "${MULTITAP_DEPS_DIR}/monocypher/src/monocypher.c")
    find_package(CURL QUIET)
endif()
if(MULTITAP_ARCHIVES AND CURL_FOUND AND EXISTS "${MULTITAP_DEPS_DIR}/monocypher/src/monocypher.c")
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
        file_hash.cpp
    )
    target_include_directories(frontend_download PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(frontend_download PUBLIC host_common SDL3::SDL3
                          PRIVATE multitap_dep_monocypher multitap_dep_7z CURL::libcurl)
    multitap_set_project_flags(frontend_download)
endif()

# Vulkan validation layers + debug messenger are enabled in Debug builds only.
target_compile_definitions(frontend_common PRIVATE $<$<CONFIG:Debug>:MULTITAP_VULKAN_VALIDATION=1>)

multitap_set_project_flags(frontend_common)

if(MULTITAP_HAVE_ENET)
    # frontend_net: the network layer (one UDP port: ENet + our own datagrams,
    # see net_host.h). Its own library so frontends without networking don't
    # link ENet. It calls no SDL itself (it links frontend_common only for
    # settings_file).
    add_library(frontend_net STATIC
        net_discovery.cpp
        net_host.cpp
        net_memory.cpp
        net_noise.cpp
        net_link.cpp
        net_session.cpp
        net_cpace.cpp
        net_trust.cpp
        net_pairing.cpp
        net_access.cpp
        net_client.cpp
        net_content.cpp
        net_wire.cpp
        net_settings.cpp
        net_speed.cpp
        net_input.cpp
        input_net.cpp
    )
    if(MULTITAP_ARCHIVES)
        target_sources(frontend_net PRIVATE net_content_folders.cpp)   # the host's game folders browse archives
    endif()
    if(WIN32)
        target_sources(frontend_net PRIVATE net_random_windows.cpp)
        target_link_libraries(frontend_net PRIVATE bcrypt)
    else()
        target_sources(frontend_net PRIVATE net_random_posix.cpp)
    endif()
    target_include_directories(frontend_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # frontend_common: settings_file (the trust and identity files).
    target_link_libraries(frontend_net PUBLIC host_common frontend_common PRIVATE multitap_dep_enet multitap_dep_monocypher)
    multitap_set_project_flags(frontend_net)

    # net_probe: LAN discovery on a real network, without a frontend.
    add_executable(net_probe tools/net_probe.cpp)
    target_link_libraries(net_probe PRIVATE frontend_net)
    multitap_set_project_flags(net_probe)
endif()

# Build-time tool: a frontend's strings.txt -> its string IDs, English table
# and .pot (see tools/strings_generator.cpp).
add_executable(multitap_strings_generator tools/strings_generator.cpp)
target_link_libraries(multitap_strings_generator PRIVATE host_common)
multitap_set_project_flags(multitap_strings_generator)

if(MULTITAP_BUILD_TESTS)
    add_executable(frontend_settings_file_test tests/settings_file_test.cpp)
    target_link_libraries(frontend_settings_file_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_settings_file_test)

    add_test(NAME frontend_settings_file
             COMMAND frontend_settings_file_test "${CMAKE_CURRENT_BINARY_DIR}/settings_file_test")
    set_tests_properties(frontend_settings_file PROPERTIES
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    add_executable(frontend_settings_test tests/settings_test.cpp)
    target_link_libraries(frontend_settings_test PRIVATE frontend_common)
    multitap_set_project_flags(frontend_settings_test)

    add_test(NAME frontend_settings
             COMMAND frontend_settings_test "${CMAKE_CURRENT_BINARY_DIR}/settings_test")
    set_tests_properties(frontend_settings PROPERTIES
                         FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # The migration check needs a scratch folder that doesn't exist yet: a
    # fixture empties it first.
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

if(MULTITAP_BUILD_TESTS AND MULTITAP_HAVE_ENET)
    # Discovery packets, then a listener and a client over loopback.
    add_executable(frontend_net_test tests/net_test.cpp)
    target_link_libraries(frontend_net_test PRIVATE frontend_net multitap_dep_enet)
    multitap_set_project_flags(frontend_net_test)
    add_test(NAME frontend_net COMMAND frontend_net_test "${CMAKE_CURRENT_BINARY_DIR}/net_test")
    set_tests_properties(frontend_net PROPERTIES
                         PASS_REGULAR_EXPRESSION "all net checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

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

    # Settings and actions from a client: messages, pages, the host's side.
    add_executable(frontend_net_settings_test tests/net_settings_test.cpp)
    target_link_libraries(frontend_net_settings_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_settings_test)
    add_test(NAME frontend_net_settings COMMAND frontend_net_settings_test)
    set_tests_properties(frontend_net_settings PROPERTIES
                         PASS_REGULAR_EXPRESSION "net settings: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # Speed test: messages, pacing, counting.
    add_executable(frontend_net_speed_test tests/net_speed_test.cpp)
    target_link_libraries(frontend_net_speed_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_speed_test)
    add_test(NAME frontend_net_speed COMMAND frontend_net_speed_test)
    set_tests_properties(frontend_net_speed PROPERTIES
                         PASS_REGULAR_EXPRESSION "net speed: all checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # Remote pads: packets, then into the router.
    add_executable(frontend_input_net_test tests/input_net_test.cpp)
    target_link_libraries(frontend_input_net_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_input_net_test)
    add_test(NAME frontend_input_net COMMAND frontend_input_net_test)
    set_tests_properties(frontend_input_net PROPERTIES
                         PASS_REGULAR_EXPRESSION "all input_net checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

    # The Noise handshake against the official test vector, then failures.
    add_executable(frontend_net_noise_test tests/net_noise_test.cpp)
    target_link_libraries(frontend_net_noise_test PRIVATE frontend_net)
    multitap_set_project_flags(frontend_net_noise_test)
    add_test(NAME frontend_net_noise COMMAND frontend_net_noise_test)
    set_tests_properties(frontend_net_noise PROPERTIES
                         PASS_REGULAR_EXPRESSION "all noise checks passed"
                         FAIL_REGULAR_EXPRESSION "FAIL|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
endif()

# frontend_common: code shared by every frontend (piccolo, libretro-test-frontend,
# piccoletto, later Multitap): Vulkan instance/device, swapchain, frame
# rendering, Dear ImGui glue, settings, audio output, the input router.
# Plain static library, no frontend-specific code. Links host_common for its
# file helpers (host_fs: atomic writes); the other direction (hosts using
# frontend or SDL code) is what's not allowed.
# A library: it doesn't build alone. The frontend adds hosts-common first,
# defines imgui, SDL3::SDL3 and Vulkan::Vulkan, and sets MULTITAP_DEPS_DIR to
# its deps folder (libsamplerate, gamemode, miniz, 7zip; for the resampler
# tool also libretro-common), built here from their sources.

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
        "${SEVEN_ZIP_C}/Delta.c" "${SEVEN_ZIP_C}/Lzma2Dec.c" "${SEVEN_ZIP_C}/LzmaDec.c")
    target_include_directories(multitap_dep_7z SYSTEM PUBLIC "${SEVEN_ZIP_C}")
    target_compile_definitions(multitap_dep_7z PRIVATE Z7_EXTRACT_ONLY)
    # Decoding runs when a game loads; optimized even in Debug builds, like libsamplerate.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(multitap_dep_miniz PRIVATE -O2)
        target_compile_options(multitap_dep_7z PRIVATE -O2)
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
# stub, chosen here instead of #if in the code. (The Android build says
# "Linux" to CMake, see hosts-common's cmake/android-toolchain.cmake: hence NOT ANDROID.)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
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

# Vulkan validation layers + debug messenger are enabled in Debug builds only.
target_compile_definitions(frontend_common PRIVATE $<$<CONFIG:Debug>:MULTITAP_VULKAN_VALIDATION=1>)

multitap_set_project_flags(frontend_common)

# Build-time tool: a frontend's strings.txt -> its string IDs, English table
# and .pot (see tools/strings_generator.cpp).
# Cross builds (Android) can't run what they build: there it's an imported
# program, built for the build machine first (piccolo's android/build-apk.sh).
# A frontend without translated strings (piccoletto, for now) doesn't pass
# it: then there's no generator target.
if(ANDROID)
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

# frontends-common

Code shared by multitap's frontends (piccolo, libretro-test-frontend,
piccolette, later Multitap): Vulkan rendering, the window, Dear ImGui glue,
audio output, settings, localization, and the input router (devices, ports,
bindings, remaps, combos, macros, turbo, the cursor). No host code.

A library: it doesn't build on its own. A frontend adds it as a submodule,
after hosts-common, defines `imgui`, `SDL3::SDL3` and `Vulkan::Vulkan`, and
sets `MULTITAP_DEPS_DIR` to its `deps/` folder (libsamplerate, gamemode; for
the resampler measurement tool also speexdsp and libretro-common).

MIT License (see LICENSE).

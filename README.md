# frontends-common

Code shared by multitap's frontends (piccolo, libretro-test-frontend,
piccoletto, later Multitap): Vulkan rendering, the window, Dear ImGui glue,
audio output, settings, localization, and the input router (devices, ports,
bindings, remaps, combos, macros, turbo, the cursor). No host code.

A library: it doesn't build on its own. A frontend adds it as a submodule,
after hosts-common, defines `imgui`, `SDL3::SDL3` and `Vulkan::Vulkan`, and
sets `MULTITAP_DEPS_DIR` to its `deps/` folder (libsamplerate, gamemode; for
the resampler measurement tool also libretro-common).

Networking: the network core (connections, pairing, access, SHARE) is
[multitap-net](https://github.com/multitap-org/multitap-net). A frontend that
adds it before this library also gets `frontend_net`: remote play, remote pads
in the router, settings from a client, and the threads that run them.

MIT License (see LICENSE).

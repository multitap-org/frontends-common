#pragma once

// The input router's network backend (like input_sdl for local devices):
// gamepads of authorized network peers (net_access.h) become gamepads in the
// router, so everything local controllers have works for them too: START
// joins the first free port (N:N), profiles, remaps, turbo, combos. Only for
// playing (InputDevice::game_only): a remote pad never drives the interface,
// even on port 1; on port 1 it has the game's hotkeys (save / load state,
// fast forward). (Configuring the frontend from the remote app
// will be its own messages, later, not pad input.)
//
// A remote pad's identity (its guid) is "net:<16 hex digits of the device
// key>:<pad>": the same phone's first pad is the same controller every time
// it connects, so it keeps its profile and comes back to its ports.
// When the peer leaves, its pads are unplugged (they leave every port and
// rejoin when it comes back, like a local pad).

#include <cstddef>
#include <cstdint>

#include "input_bindings.h"
#include "net_host.h"
#include "net_input.h"
#include "net_noise.h"

struct InputNetPad
{
    bool     active = false;     // has a device in the router
    uint32_t device = 0;         // its id
    uint32_t last_frame = 0;     // the newest state applied
};

struct InputNet
{
    InputNetPad pads[NET_MAX_PEERS][NET_INPUT_MAX_PADS];
    uint32_t    dropped = 0;     // packets that weren't pad packets
};

// A packet on an authorized peer's input channel. `key` is its device key,
// `name` what it's called (the paired device's name).
void input_net_receive(InputNet* net, InputSystem* system, uint32_t peer, const uint8_t* key, const char* name,
                       const uint8_t* data, size_t size);

// The peer left (or networking stopped): its pads are unplugged.
void input_net_peer_gone(InputNet* net, InputSystem* system, uint32_t peer);

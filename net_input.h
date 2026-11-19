#pragma once

// Remote input packets, as pure functions: a client (piccoletto, net_probe)
// sends the state of its gamepads to a host, which feeds them to its input
// router as gamepads (input_net.h). They travel on the input channel
// (unreliable, sequenced, encrypted: net_link.h).
//
// The state is a gamepad's, in the router's own codes (InputButton bits,
// InputAxis values), not a RetroPad: the host maps it like any controller
// (profiles, remaps, N:N ports). Rollback netplay will send per-port
// RetroPad frames instead, in its own message type, over the same channel.
//
// Layout (little-endian):
//   type 1 (pad) | pad index (0..3) | count (1..NET_INPUT_HISTORY)
//   count x { frame u32 | buttons u32 | 6 axes i16 }        newest first
//
// Every packet repeats the last few states: a lost packet costs nothing,
// and a press and release between two packets (a tap) still reaches the
// host, which applies the states it hasn't seen yet, oldest first.

#include <cstddef>
#include <cstdint>

constexpr uint8_t  NET_INPUT_TYPE_PAD = 1;
constexpr uint32_t NET_INPUT_MAX_PADS = 4;
constexpr uint32_t NET_INPUT_HISTORY = 4;
constexpr uint32_t NET_INPUT_AXES = 6;   // InputAxis order
constexpr size_t   NET_INPUT_STATE_SIZE = 4 + 4 + 2 * NET_INPUT_AXES;
constexpr size_t   NET_INPUT_MAX_PACKET = 3 + NET_INPUT_HISTORY * NET_INPUT_STATE_SIZE;

struct NetPadState
{
    uint32_t frame = 0;     // the sender's counter, one per state sent
    uint32_t buttons = 0;   // bit n = InputButton n held
    int16_t  axes[NET_INPUT_AXES] = {};   // sticks -32767..32767, triggers 0..32767
};

// The sender's side: the last states, newest first.
struct NetPadSender
{
    uint32_t    next_frame = 1;
    NetPadState history[NET_INPUT_HISTORY];
    uint32_t    count = 0;
};

// Adds `state` (its frame is set here) and writes the packet to send.
// Returns its size.
size_t net_input_push(NetPadSender* sender, uint8_t pad, const NetPadState* state, uint8_t* out, size_t capacity);

// Reads a pad packet: `states` gets them oldest first. False unless it's
// exactly a pad packet.
bool net_input_read(const uint8_t* data, size_t size, uint8_t* pad, NetPadState* states, uint32_t* count);

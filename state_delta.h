#pragma once

// state_delta: the difference between two savestates of the same size, small
// and fast enough to keep one per frame (rewind).
//
// The delta is the XOR of the two states, so the same delta turns A into B
// and B back into A: a rewind buffer keeps only the newest full state plus
// one delta per step and walks either way (back while rewinding, forward
// while scrubbing a paused timeline), and dropping the oldest step is just
// forgetting its delta (no keyframes).
//
// Between two frames most of a state doesn't change, so the XOR is mostly
// zeros. It's written in 32-bit words as runs:
//
//   [zero words: varint] [changed words: varint] [the changed words' XOR]...
//
// until the state ends; a last partial word (size not a multiple of 4) counts
// as a word padded with zeros. A run of changed words ends at the first two
// zero words in a row (a single zero word inside a run is cheaper kept than
// split). Encoding reads both states once; applying touches only the changed
// words. No allocation: the caller gives the output buffer, at most
// state_delta_max_size() bytes.
//
// Not a general binary diff (xdelta / VCDIFF look for moved data, far too slow
// per frame): savestates keep their layout from frame to frame.

#include <cstddef>
#include <cstdint>

// The largest delta of a `state_size` bytes state (every word changed).
size_t state_delta_max_size(size_t state_size);

// Writes the delta between `from` and `to` (`size` bytes each) into `out`
// (room for state_delta_max_size(size)). Returns its size: 2 or 3 bytes when
// nothing changed.
size_t state_delta_encode(const uint8_t* from, const uint8_t* to, size_t size, uint8_t* out);

// Applies `delta` (`delta_size` bytes) to `state` (`size` bytes) in place: A
// becomes B, or B becomes A. False (state left part-way) if the delta is
// malformed or doesn't fit `size`.
bool state_delta_apply(uint8_t* state, size_t size, const uint8_t* delta, size_t delta_size);

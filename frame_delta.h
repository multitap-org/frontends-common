#pragma once

// A lossless codec for a core's frames, at their own size and in their own
// pixel format, for remote play (netcode milestone 6, part one). Retro
// pictures change little from one frame to the next and use few colors, so
// sending what changed, coded simply, is small and costs almost nothing to
// make or undo; the picture arrives exactly as the core made it (shaders run
// on the client, so a lossy codec's smears would be magnified).
//
// A frame is either a keyframe (everything) or a delta against the frame
// before it. Both sides keep the previous frame; a delta that can't be
// applied (a lost one, a size change) needs a keyframe, which the caller
// asks for.
//
// Layout (little-endian):
//
//   header  flags u8 (bit 0 keyframe) | format u8 (FramePixelFormat) | width u16 | height u16
//   map     (deltas only) one bit per 8x8 tile, row by row, bit 0 first:
//           1 = the tile changed. Tiles at the right and bottom edges may be
//           smaller.
//   pixels  the changed tiles (all of them in a keyframe), in map order, each
//           tile's pixels row by row, as ops:
//
//     00iiiiii  INDEX  the color in slot i of the recent-colors table
//     01nnnnnn  RUN    the pixel before, n+1 more times
//     10nnnnnn  SAME   n+1 pixels as they were in the previous frame (deltas only)
//     11000000  RAW    the pixel follows (2 or 4 bytes, as the format)
//
// Ops run across tile edges (both sides walk the same pixels in the same
// order). The recent-colors table (64 slots, by a hash of the pixel) and
// "the pixel before" start at zero every frame; every RAW pixel and every
// SAME pixel goes into the table.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "frame_pixel_format.h"

constexpr uint32_t FRAME_DELTA_TILE = 8;
constexpr size_t   FRAME_DELTA_HEADER_SIZE = 6;
constexpr uint32_t FRAME_DELTA_MAX_SIDE = 4096;

// The largest a frame can code to (every pixel RAW, plus header and map).
size_t frame_delta_max_size(uint32_t width, uint32_t height, FramePixelFormat format);

struct FrameDeltaEncoder
{
    uint32_t             width = 0;
    uint32_t             height = 0;
    FramePixelFormat     format = FramePixelFormat::rgb565;
    std::vector<uint8_t> previous;           // the last frame coded, tightly packed
    bool                 has_previous = false;
};

struct FrameDeltaDecoder
{
    uint32_t             width = 0;
    uint32_t             height = 0;
    FramePixelFormat     format = FramePixelFormat::rgb565;
    std::vector<uint8_t> frame;              // the picture so far, tightly packed
    bool                 has_frame = false;
};

// Codes `pixels` (width x height, tightly packed, in `format`) into `out`,
// which must hold frame_delta_max_size bytes. A keyframe when `keyframe` is
// true, when there's no previous frame, or when the size or format changed.
// Returns the size (0 for a bad size).
size_t frame_delta_encode(FrameDeltaEncoder* encoder, const uint8_t* pixels, uint32_t width, uint32_t height,
                          FramePixelFormat format, bool keyframe, uint8_t* out);

// Applies a coded frame. False (the picture unchanged where it can be) for a
// damaged frame, or a delta without the frame it was made against (a keyframe
// is needed).
bool frame_delta_decode(FrameDeltaDecoder* decoder, const uint8_t* data, size_t size);

// Is this coded frame a keyframe? (False for anything too short.)
bool frame_delta_is_keyframe(const uint8_t* data, size_t size);

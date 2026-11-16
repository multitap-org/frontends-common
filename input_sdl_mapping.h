#pragma once

// Mapping a controller as it is (milestone 3, step 5c): the raw joystick's
// buttons, hats and axes, one control at a time, into an SDL gamepad mapping
// for its GUID (kept in a mappings file the input router's SDL side loads).
// SDL's own database follows whatever convention each entry chose (by label,
// by position); a mapping made here is by position, always.
//
// The steps, in order: the four face buttons (bottom, right, left, top), the
// d-pad, L / R, SELECT / START, then the ones a pad may not have (L2 / R2,
// stick clicks, HOME, the two sticks), which are skipped after a few seconds
// without input. Each step waits until everything is let go, so one press
// never lands on two steps.

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Joystick;

enum class InputMapControl
{
    south,
    east,
    west,
    north,
    dpad_up,
    dpad_down,
    dpad_left,
    dpad_right,
    left_shoulder,
    right_shoulder,
    back,
    start,
    left_trigger,
    right_trigger,
    left_stick,
    right_stick,
    guide,
    left_x,    // the stick pushed right
    left_y,    // the stick pushed down
    right_x,
    right_y,
    count,
};

constexpr uint32_t INPUT_MAP_CONTROL_COUNT = static_cast<uint32_t>(InputMapControl::count);

// May the pad lack it? (Then the step is skipped after a few seconds.)
bool input_map_control_optional(InputMapControl control);

// Its name in SDL's mapping text ("a", "dpup", "leftx"...).
const char* input_map_control_sdl_name(InputMapControl control);

struct InputSdlMapper
{
    bool          active = false;
    SDL_Joystick* joystick = nullptr;
    uint32_t      joystick_id = 0;
    std::string   guid;
    std::string   name;
    uint32_t      step = 0;                          // the InputMapControl being asked
    uint64_t      step_started_ms = 0;
    bool          waiting_release = false;           // a step just ended: wait for everything to be let go
    std::vector<int16_t> rest;                       // each axis at rest (when mapping started)
    std::string   elements[INPUT_MAP_CONTROL_COUNT]; // "b3", "h0.1", "+a2", "a5", "a1~" ("" = none)
    bool          wanted[INPUT_MAP_CONTROL_COUNT] = {};   // the steps to ask (the controller's layout has them)
};

// Starts on a connected joystick (any: known to SDL as a gamepad or not),
// asking for the controls in `wanted` (nullptr = all of them).
bool input_sdl_mapper_begin(InputSdlMapper* mapper, uint32_t joystick_id, const bool* wanted, uint64_t now_ms);

// Once a frame: looks for the current step's control. Returns true when the
// mapping is complete (every step answered or skipped).
bool input_sdl_mapper_update(InputSdlMapper* mapper, uint64_t now_ms);

// The seconds the current (optional) step still waits before skipping, or 0
// for a step that waits as long as needed.
uint32_t input_sdl_mapper_seconds_left(const InputSdlMapper* mapper, uint64_t now_ms);

void input_sdl_mapper_back(InputSdlMapper* mapper, uint64_t now_ms);   // the previous step again
void input_sdl_mapper_skip(InputSdlMapper* mapper, uint64_t now_ms);   // this step: none
void input_sdl_mapper_cancel(InputSdlMapper* mapper);

// The mapping as SDL's text, given to SDL now and kept in `mappings_path`
// (replacing that GUID's line). False if SDL refused it or nothing was mapped.
bool input_sdl_mapper_finish(InputSdlMapper* mapper, const char* mappings_path, std::string* line);

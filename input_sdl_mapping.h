#pragma once

// Mapping a controller as it is (milestone 3, step 5c): the raw joystick's
// buttons, hats and axes, one control at a time, into an SDL gamepad mapping
// for its GUID (kept in the controller's profile, input.cfg, and given to SDL
// at start).
// SDL's own database follows whatever convention each entry chose (by label,
// by position); a mapping made here is by position, always.
//
// The position steps come first; then optional *role* steps (the
// interface's controls: navigate, accept, back, the hotkey modifier...): the
// button for each, whatever its position, but only one the positions mapped.
// Every step takes a control held hold_ms (the drawn pad shows it
// meanwhile, to confirm it; let go sooner and it isn't taken).
// Each role is told as the position (or axis half) it is
// (input_sdl_mapper_role_source).
//
// The position steps, in order: the four face buttons (bottom, right, left, top), the
// d-pad, L / R, SELECT / START, then the ones a pad may not have (L2 / R2,
// stick clicks, HOME, the two sticks), which are skipped after a few seconds
// without input. Each step waits until everything is let go, so one press
// never lands on two steps.

#include <cstdint>
#include <string>
#include <vector>

#include "input_devices.h"

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

// One of the four stick axes (not count)?
bool input_map_control_is_stick(InputMapControl control);

// Where a gamepad source is on the pad (a button, a trigger, a stick pushed
// either way), or count for anything else (a key, a paddle...).
InputMapControl input_map_control_for_source(InputSource source);

constexpr uint32_t INPUT_MAP_ROLES_MAX = 16;

struct InputSdlMapper
{
    bool          active = false;
    SDL_Joystick* joystick = nullptr;
    uint32_t      joystick_id = 0;
    std::string   guid;
    std::string   name;
    uint32_t      step = 0;                          // the InputMapControls first, then the roles
    uint64_t      step_started_ms = 0;
    bool          waiting_release = false;           // a step just ended: wait for everything to be let go
    std::vector<int16_t> rest;                       // each axis at rest (when mapping started)
    std::string   elements[INPUT_MAP_CONTROL_COUNT]; // "b3", "h0.1", "+a2", "a5", "a1~" ("" = none)
    bool          wanted[INPUT_MAP_CONTROL_COUNT] = {};   // the steps to ask (the controller's layout has them)
    uint32_t      role_count = 0;
    bool          role_optional[INPUT_MAP_ROLES_MAX] = {};
    std::string   roles[INPUT_MAP_ROLES_MAX];        // each role's raw element ("" = skipped)
    std::string   holding;                           // the control being held (not taken yet)
    uint64_t      holding_since_ms = 0;
    uint64_t      hold_ms = 500;                // how long a step's control must be held (positions and roles)
    uint64_t      optional_wait_ms = 5000;           // an optional step is skipped after this long
};

// Starts on a connected joystick (any: known to SDL as a gamepad or not):
// first the position steps in `wanted` (nullptr = all of them), then
// `role_count` role steps (optional ones skip after a few seconds).
bool input_sdl_mapper_begin(InputSdlMapper* mapper, uint32_t joystick_id, uint32_t role_count, const bool* role_optional,
                            const bool* wanted, uint64_t now_ms);

// The role being asked (0 ..), or -1 during the position steps (and once done).
int32_t input_sdl_mapper_role(const InputSdlMapper* mapper);

// The position being asked, or InputMapControl::count once the roles have begun.
InputMapControl input_sdl_mapper_control(const InputSdlMapper* mapper);

// What a role's button is on the mapping (a gamepad button, or an axis
// half), or a none source if it was skipped.
InputSource input_sdl_mapper_role_source(const InputSdlMapper* mapper, uint32_t role);

// What is held right now, on the mapping made so far (for the drawn pad
// during the roles), or a none source.
InputSource input_sdl_mapper_held_source(const InputSdlMapper* mapper);

// How far the current step's hold has come: 0 (nothing held) .. 1 (taken).
float input_sdl_mapper_hold_progress(const InputSdlMapper* mapper, uint64_t now_ms);

// Once a frame: looks for the current step's control. Returns true when the
// mapping is complete (every step answered or skipped).
bool input_sdl_mapper_update(InputSdlMapper* mapper, uint64_t now_ms);

// The seconds the current (optional) step still waits before skipping, or 0
// for a step that waits as long as needed.
uint32_t input_sdl_mapper_seconds_left(const InputSdlMapper* mapper, uint64_t now_ms);

void input_sdl_mapper_back(InputSdlMapper* mapper, uint64_t now_ms);   // the previous step again
void input_sdl_mapper_skip(InputSdlMapper* mapper, uint64_t now_ms);   // this step: none
void input_sdl_mapper_cancel(InputSdlMapper* mapper);

// The current step starts over (after a pause in which update wasn't called:
// waits for everything to be let go, and an optional step's time restarts).
void input_sdl_mapper_restart_step(InputSdlMapper* mapper, uint64_t now_ms);

// The mapping as SDL's text (`line`, for the controller's profile), given to
// SDL now. False if SDL refused it or nothing was mapped.
bool input_sdl_mapper_finish(InputSdlMapper* mapper, std::string* line);

#pragma once

// Mapping a controller as it is (milestone 3, step 5c): the raw joystick's
// buttons, hats and axes, one control at a time, into an SDL gamepad mapping
// for its GUID (kept in the controller's profile, input.cfg, and given to SDL
// at start).
// SDL's own database follows whatever convention each entry chose (by label,
// by position); a mapping made here is by position, always.
//
// Optional *role* steps come first (the interface's controls: navigate,
// accept, back, the hotkey modifier...): the button for each, whatever its
// position; then the position steps. After finishing, each role is told as
// the position (or axis half) it turned out to be (input_sdl_mapper_role_source).
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
    uint32_t      step = 0;                          // roles first (0 .. role_count - 1), then the InputMapControls
    uint64_t      step_started_ms = 0;
    bool          waiting_release = false;           // a step just ended: wait for everything to be let go
    std::vector<int16_t> rest;                       // each axis at rest (when mapping started)
    std::string   elements[INPUT_MAP_CONTROL_COUNT]; // "b3", "h0.1", "+a2", "a5", "a1~" ("" = none)
    bool          wanted[INPUT_MAP_CONTROL_COUNT] = {};   // the steps to ask (the controller's layout has them)
    uint32_t      role_count = 0;
    bool          role_optional[INPUT_MAP_ROLES_MAX] = {};
    std::string   roles[INPUT_MAP_ROLES_MAX];        // each role's raw element ("" = skipped)
};

// Starts on a connected joystick (any: known to SDL as a gamepad or not):
// first `role_count` role steps (optional ones skip after a few seconds),
// then the position steps in `wanted` (nullptr = all of them).
bool input_sdl_mapper_begin(InputSdlMapper* mapper, uint32_t joystick_id, uint32_t role_count, const bool* role_optional,
                            const bool* wanted, uint64_t now_ms);

// The role being asked (0 ..), or -1 once the position steps have begun.
int32_t input_sdl_mapper_role(const InputSdlMapper* mapper);

// The position being asked, or InputMapControl::count during the roles.
InputMapControl input_sdl_mapper_control(const InputSdlMapper* mapper);

// After finishing: what a role's button turned out to be in the mapping (a
// gamepad button, or an axis half), or a none source if it isn't there.
InputSource input_sdl_mapper_role_source(const InputSdlMapper* mapper, uint32_t role);

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

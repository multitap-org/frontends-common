#pragma once

// The SDL side of the input router: the keyboard (all keyboards as
// one device, with one controller using the default keys) and every gamepad
// SDL sees (each with a controller using the default layout, by position),
// fed into an InputSystem from SDL events.

#include <cstdint>
#include <vector>

#include "input_bindings.h"

struct SDL_Gamepad;
union SDL_Event;

struct InputSdlGamepad
{
    uint32_t     joystick_id = 0;   // SDL_JoystickID
    SDL_Gamepad* gamepad = nullptr;
    uint32_t     device = 0;        // InputDevice id
    uint16_t     rumble[2] = {};    // what it plays now: strong, weak
};

struct InputSdl
{
    uint32_t                     keyboard_device = 0;
    std::vector<InputSdlGamepad> gamepads;
    uint32_t                     last_active_device = 0;    // the last device a key or button came from
    uint32_t                     last_active_gamepad = 0;   // the last gamepad a button came from (0 = none yet)
};

// Adds the keyboard (device and controller) and the gamepads already
// connected. Needs SDL_INIT_GAMEPAD. `mappings_path`: a file of SDL gamepad
// mappings for controllers SDL doesn't know (may be missing; nullptr = none).
void input_sdl_init(InputSdl* sdl, InputSystem* system, const char* mappings_path);
void input_sdl_shutdown(InputSdl* sdl);

// Keys, gamepad buttons and axes, gamepads coming and going, focus lost
// (everything released: the key-ups go to another window).
void input_sdl_handle_event(InputSdl* sdl, InputSystem* system, const SDL_Event* event);

// The SDL gamepad behind a device (for its button labels), or nullptr.
SDL_Gamepad* input_sdl_gamepad(const InputSdl* sdl, uint32_t device_id);

// Plays the rumble of each port on its controller's gamepad (strong and weak
// motor, 0..0xffff), if it changed. `rumble` is [port][strong, weak].
void input_sdl_rumble(InputSdl* sdl, const InputSystem* system, const uint16_t rumble[][2], uint32_t port_count);

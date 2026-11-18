#pragma once

// The SDL side of the input router: the keyboard (all keyboards as
// one device, with one controller using the default keys) and every gamepad
// SDL sees (each with a controller using the default layout, by position),
// fed into an InputSystem from SDL events.

#include <cstdint>
#include <string>
#include <vector>

#include "input_bindings.h"

struct SDL_Gamepad;
struct SDL_Joystick;
union SDL_Event;

struct InputSdlGamepad
{
    uint32_t     joystick_id = 0;   // SDL_JoystickID
    SDL_Gamepad* gamepad = nullptr;
    uint32_t     device = 0;        // InputDevice id
    uint16_t     rumble[2] = {};    // what it plays now: strong, weak
};

// A mouse SDL tells apart (by its id; 0 = the system's, when it doesn't).
struct InputSdlMouse
{
    uint32_t which = 0;    // SDL_MouseID
    uint32_t device = 0;   // InputDevices id
};

struct InputSdl
{
    uint32_t                     keyboard_device = 0;
    std::vector<InputSdlMouse>   mice;                      // each its own device (several: one cursor each)
    std::vector<InputSdlGamepad> gamepads;
    uint32_t                     last_active_device = 0;    // the last device a key or button came from
    uint32_t                     last_active_gamepad = 0;   // the last gamepad a button came from (0 = none yet)
    std::vector<uint32_t>        unmapped;                  // joysticks SDL doesn't know as gamepads (to be mapped)
};

// Adds the keyboard (device and controller) and the gamepads already
// connected. Needs SDL_INIT_GAMEPAD. Call after the profiles are read: their
// SDL mappings (MAP CONTROLLER's) are given to SDL first. `database_path`: a
// gamecontrollerdb.txt the user may drop in (SDL's format, read only; may be
// missing; nullptr = none); the profiles' mappings win over it.
void input_sdl_init(InputSdl* sdl, InputSystem* system, const char* database_path);
void input_sdl_shutdown(InputSdl* sdl);

// Keys, gamepad buttons and axes, gamepads coming and going, focus lost
// (everything released: the key-ups go to another window).
void input_sdl_handle_event(InputSdl* sdl, InputSystem* system, const SDL_Event* event);

// A joystick's name and GUID (for one in `unmapped`), "" if it's gone.
std::string input_sdl_joystick_name(uint32_t joystick_id);
std::string input_sdl_joystick_guid(uint32_t joystick_id);

// Any button (or hat) of a raw joystick pressed: for a controller SDL
// doesn't know as a gamepad, which can't drive the interface.
struct InputSdlPress
{
    SDL_Joystick* joystick = nullptr;
    uint32_t      joystick_id = 0;
    bool          held = true;   // down at the last look (starts true: a press needs a release first)
};

// True once per press, when something goes down after everything was up.
// Opens the joystick on first use (or another id).
bool input_sdl_press_poll(InputSdlPress* press, uint32_t joystick_id);

// The next press must start from everything let go.
void input_sdl_press_wait_release(InputSdlPress* press);

void input_sdl_press_close(InputSdlPress* press);

// The SDL joystick behind a gamepad device (to map it again), or 0.
uint32_t input_sdl_joystick_of(const InputSdl* sdl, uint32_t device_id);

// The SDL gamepad behind a device (for its button labels), or nullptr.
SDL_Gamepad* input_sdl_gamepad(const InputSdl* sdl, uint32_t device_id);

// Plays the rumble of each port on its controller's gamepad (strong and weak
// motor, 0..0xffff), if it changed. `rumble` is [port][strong, weak].
void input_sdl_rumble(InputSdl* sdl, const InputSystem* system, const uint16_t rumble[][2], uint32_t port_count);

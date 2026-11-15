#pragma once

// From raw device state (input_devices.h) to what it means: the RetroPad of
// each port, the interface controls and the hotkeys. No libretro and no SDL:
// the libretro side reads InputFrame::pad_* and builds what the core sees.
//
// A *controller* is one set of bindings on one device. A gamepad is one
// controller; the keyboard can be split into several (each with its own
// keys and its own START), for any number of players.
//
// Rules (decided for milestone 3, docs/plan-milestones.md):
//   - A controller plays only after it joins a port: START (its pad_start
//     binding) while it has none, into the first free port. The frontend can
//     also join one directly (the device that started the game).
//   - With gamepad_takes_keyboard_port, a gamepad that joins while a
//     keyboard controller holds port 1 takes port 1 (the keyboard leaves).
//   - A disconnected device frees its port; when it comes back it gets that
//     port again if it is still free.
//   - The press that joins is not passed to the game.
//   - Hotkeys on a gamepad need HOTKEY ENABLE (hotkey_enable): HOLD = they
//     work while it is held; DOUBLE TAP = the next one hotkey works, then the
//     normal layout is back (or after a few seconds). While enabled the
//     controller's RetroPad goes quiet (after a short delay in HOLD, so a
//     quick tap of the enable button still reaches the game).
//   - Keyboard hotkeys work without HOTKEY ENABLE, unless their key is also
//     bound to the RetroPad or the core takes the keyboard as a keyboard.
//   - Any binding can share a source with others: one key on two RetroPad
//     buttons is a combo (L as A + B on a GBA core).
// Turbo and macros (a source that plays a sequence) are planned on top of
// this, as binding modes.

#include <cstdint>
#include <string>
#include <vector>

#include "input_devices.h"

enum class InputAction
{
    // RetroPad buttons, in RETRO_DEVICE_ID_JOYPAD order (the action's number is the bit).
    pad_b,
    pad_y,
    pad_select,
    pad_start,
    pad_up,
    pad_down,
    pad_left,
    pad_right,
    pad_a,
    pad_x,
    pad_l,
    pad_r,
    pad_l2,
    pad_r2,
    pad_l3,
    pad_r3,
    // RetroPad sticks, one direction each.
    pad_left_x_plus,
    pad_left_x_minus,
    pad_left_y_plus,
    pad_left_y_minus,
    pad_right_x_plus,
    pad_right_x_minus,
    pad_right_y_plus,
    pad_right_y_minus,
    // Interface.
    ui_up,
    ui_down,
    ui_left,
    ui_right,
    ui_accept,
    ui_back,
    ui_page_left,
    ui_page_right,
    ui_open_menu,   // the main menu bar's first menu, to start navigating without a mouse
    // Hotkeys.
    hotkey_enable,
    hotkey_menu,
    hotkey_quick_menu,
    hotkey_save_state,
    hotkey_load_state,
    hotkey_fast_forward,
    count,
};

constexpr uint32_t INPUT_ACTION_COUNT = static_cast<uint32_t>(InputAction::count);
constexpr uint32_t INPUT_PAD_BUTTON_COUNT = 16;
constexpr uint32_t INPUT_PORT_COUNT = 4;
constexpr uint32_t INPUT_SOURCES_PER_ACTION = 4;
constexpr int32_t  INPUT_NO_PORT = -1;

// "pad.b", "ui.accept", "hotkey.quick_menu"... (for settings files).
const char* input_action_name(InputAction action);
bool        input_action_parse(const char* name, InputAction* action);

bool input_action_is_pad(InputAction action);
bool input_action_is_hotkey(InputAction action);

struct InputBindingSet
{
    InputSource sources[INPUT_ACTION_COUNT][INPUT_SOURCES_PER_ACTION] = {};
};

// Adds `source` to the action (if it isn't there and a slot is free).
bool input_bind(InputBindingSet* set, InputAction action, InputSource source);
void input_unbind_all(InputBindingSet* set, InputAction action);
bool input_is_bound(const InputBindingSet* set, InputAction action, InputSource source);

// The defaults for the keyboard (arrows, Z X A S Q W E R, Enter, Right Shift;
// F1 quick menu, F2 save, F4 load, Escape menu, Space fast forward).
void input_default_keyboard_bindings(InputBindingSet* set);

// The defaults for a gamepad, by position (the RetroPad's SNES layout: B
// bottom, A right, Y left, X top; triggers L2 / R2, stick clicks L3 / R3,
// sticks analog). Interface: d-pad and left stick, accept = bottom, back =
// right, shoulders page, START opens the menu bar's first menu. Hotkeys with HOTKEY ENABLE = Select: Start quick
// menu, right button menu, R save, L load, R2 fast forward.
void input_default_gamepad_bindings(InputBindingSet* set);

enum class InputHotkeyMode
{
    hold,
    double_tap,
};

struct InputSettings
{
    bool            gamepad_takes_keyboard_port = true;
    InputHotkeyMode hotkey_mode = InputHotkeyMode::hold;
    bool            core_takes_keyboard = false;   // the core reads the keyboard as a keyboard
    // False while the frontend's own UI is up (a menu or a dialog): no
    // controller plays or joins (they drive the UI); hotkeys still work. When
    // it comes back, each controller stays quiet until it lets go of everything,
    // so the button that closed the menu isn't the game's.
    bool            game_has_input = true;
};

struct InputController
{
    uint32_t        device_id = 0;
    std::string     name;               // shown in the UI, editable
    InputBindingSet bindings;
    float           deadzone = 0.15f;   // sticks, 0..1

    // Kept between frames by input_update().
    int32_t  last_port = INPUT_NO_PORT;            // the port it had when its device went away
    bool     held_last[INPUT_ACTION_COUNT] = {};
    bool     quiet_until_start_released = false;   // the START that joined is not the game's
    bool     quiet_until_released = false;         // back from the UI: nothing to the game until all is let go
    uint64_t enable_down_ms = 0;
    uint64_t enable_last_tap_ms = 0;
    bool     armed = false;                        // DOUBLE TAP: the next hotkey works
    uint64_t armed_ms = 0;
};

struct InputSystem
{
    InputDevices                 devices;
    std::vector<InputController> controllers;
    int32_t                      ports[INPUT_PORT_COUNT] = {INPUT_NO_PORT, INPUT_NO_PORT, INPUT_NO_PORT, INPUT_NO_PORT};
    InputSettings                settings;
    bool                         game_had_input = true;   // settings.game_has_input last frame
};

struct InputFrame
{
    uint16_t pad_buttons[INPUT_PORT_COUNT] = {};    // bit N = RETRO_DEVICE_ID_JOYPAD_N
    int16_t  analog[INPUT_PORT_COUNT][2][2] = {};   // [port][left/right][x/y], -0x7fff..0x7fff
    bool     held[INPUT_ACTION_COUNT] = {};         // interface and hotkeys, any controller (hotkeys when allowed)
    bool     pressed[INPUT_ACTION_COUNT] = {};      // went down this frame
    bool     gamepad_held[INPUT_ACTION_COUNT] = {}; // interface actions held on gamepads (not the keyboard): to drive a UI
    int32_t  joined_port = INPUT_NO_PORT;           // a controller joined this frame (for a message)
    int32_t  joined_controller = -1;
};

// A new controller on a device; returns its index in system->controllers.
uint32_t input_add_controller(InputSystem* system, uint32_t device_id, const char* name, const InputBindingSet* bindings);

// The first controller on a device, or -1.
int32_t input_first_controller(const InputSystem* system, uint32_t device_id);

// The controller's port, or INPUT_NO_PORT.
int32_t input_controller_port(const InputSystem* system, uint32_t controller);

// Joins a controller now (as START would), preferring port 1. False if no
// port could be had.
bool input_join(InputSystem* system, uint32_t controller);
void input_leave(InputSystem* system, uint32_t controller);

// Every port free, nothing remembered for devices that come back (a new game).
void input_leave_all(InputSystem* system);

// Swaps two ports' controllers (reordering from the quick menu).
void input_swap_ports(InputSystem* system, uint32_t a, uint32_t b);

// One frame: joins, ports of devices that left or came back, hotkey state,
// and what every action says. Call once per frame, then
// input_devices_end_frame(&system->devices).
void input_update(InputSystem* system, uint64_t now_ms, InputFrame* frame);

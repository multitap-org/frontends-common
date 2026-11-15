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
//   - A controller does nothing until it joins a port: START (its pad_start
//     binding) while it has none, into the first free port, in the UI or in
//     a game. The press that joins is not passed on.
//   - With keyboard_shares_port1 (the default) the keyboard's first player
//     plays on port 1 without joining, together with whatever controller
//     joined port 1. Without it, it joins with START like the others. The
//     keyboard's interface and hotkey keys always work.
//   - Only the controller on port 1 drives the UI (interface actions); the
//     keyboard always does.
//   - A disconnected device frees its port (reported in the frame); when it
//     comes back it gets that port again if it is still free.
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
    bool            keyboard_shares_port1 = true;
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

// What is kept of a controller between runs (settings/input.cfg), by id: a
// gamepad's GUID (same model = same profile), "keyboard" for the keyboard's
// first controller, "keyboard.2"... for the next parts of a split keyboard.
// Bindings are kept as changes to the defaults for the device's kind, so
// better defaults reach every controller that wasn't changed.
struct InputBindingOverride
{
    InputAction action = InputAction::pad_b;
    InputSource sources[INPUT_SOURCES_PER_ACTION] = {};   // all none = unbound
};

struct InputProfile
{
    std::string                       id;
    std::string                       name;               // "" = the device's own name
    int32_t                           deadzone_percent = -1;   // -1 = default
    InputIgnore                       ignore = InputIgnore::automatic;   // gamepads
    std::vector<InputBindingOverride> overrides;
};

// Waiting for the next control pressed on one device, to bind it
// (input_capture_*). While active, input_update reports no actions and no
// RetroPad: the press belongs to the binding.
struct InputCapture
{
    bool     active = false;
    uint32_t device_id = 0;
    uint8_t  held_at_start[INPUT_KEY_COUNT] = {};
    float    axes_at_start[INPUT_AXIS_COUNT] = {};
};

struct InputSystem
{
    InputDevices                 devices;
    std::vector<InputController> controllers;
    std::vector<InputProfile>    profiles;   // loaded, plus what input_config_write took from the controllers
    int32_t                      ports[INPUT_PORT_COUNT] = {INPUT_NO_PORT, INPUT_NO_PORT, INPUT_NO_PORT, INPUT_NO_PORT};
    InputSettings                settings;
    bool                         game_had_input = true;   // settings.game_has_input last frame
    InputCapture                 capture;
};

struct InputFrame
{
    uint16_t pad_buttons[INPUT_PORT_COUNT] = {};    // bit N = RETRO_DEVICE_ID_JOYPAD_N
    int16_t  analog[INPUT_PORT_COUNT][2][2] = {};   // [port][left/right][x/y], -0x7fff..0x7fff
    bool     held[INPUT_ACTION_COUNT] = {};         // interface and hotkeys, any controller (hotkeys when allowed)
    bool     pressed[INPUT_ACTION_COUNT] = {};      // went down this frame
    bool     gamepad_held[INPUT_ACTION_COUNT] = {}; // interface actions held on port 1's gamepad: to drive a UI
    int32_t  joined_port = INPUT_NO_PORT;           // a controller joined this frame (for a message)
    int32_t  joined_controller = -1;
    int32_t  left_port = INPUT_NO_PORT;             // a controller's device went away and freed this port
    int32_t  left_controller = -1;
};

// The defaults for a device's kind (keyboard or gamepad).
void input_default_bindings(InputDeviceKind kind, InputBindingSet* set);

// The controllers a device gets when it first appears: from its profiles
// (every keyboard part, or the gamepad's GUID), else one with the defaults.
// Nothing if it already has controllers (a device coming back).
void input_add_controllers_for_device(InputSystem* system, uint32_t device_id);

// The profile id of a controller ("keyboard", "keyboard.2", or the GUID).
std::string input_controller_profile_id(const InputSystem* system, uint32_t controller);

// A new controller on a device; returns its index in system->controllers.
uint32_t input_add_controller(InputSystem* system, uint32_t device_id, const char* name, const InputBindingSet* bindings);

// The first controller on a device, or -1.
int32_t input_first_controller(const InputSystem* system, uint32_t device_id);

// The controller's port, or INPUT_NO_PORT.
int32_t input_controller_port(const InputSystem* system, uint32_t controller);

// Is this controller the keyboard's first player while it shares port 1?
bool input_controller_shares_port1(const InputSystem* system, uint32_t controller);

// Joins a controller now (as START would), preferring port 1. False if no
// port could be had.

bool input_join(InputSystem* system, uint32_t controller);
void input_leave(InputSystem* system, uint32_t controller);

// Every port free, nothing remembered for devices that come back (a new game).
void input_leave_all(InputSystem* system);

// Removes a controller (a keyboard part); ports after it are renumbered.
void input_remove_controller(InputSystem* system, uint32_t controller);

// Binding by pressing: start waiting on a device, then poll each frame until
// it gives a source (a key or button that wasn't held when it started, or
// an axis pushed past half from where it was). Cancel to give up.
void input_capture_begin(InputSystem* system, uint32_t device_id);
bool input_capture_poll(InputSystem* system, InputSource* source);
void input_capture_cancel(InputSystem* system);

// Swaps two ports' controllers (reordering from the quick menu).
void input_swap_ports(InputSystem* system, uint32_t a, uint32_t b);

// One frame: joins, ports of devices that left or came back, hotkey state,
// and what every action says. Call once per frame, then
// input_devices_end_frame(&system->devices).
void input_update(InputSystem* system, uint64_t now_ms, InputFrame* frame);

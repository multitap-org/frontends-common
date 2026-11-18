#pragma once

// The input router, from raw device state (input_devices.h) to what it
// means: the RetroPad of each port, the interface controls and the hotkeys.
// No libretro and no SDL: the libretro side reads InputFrame::pad_* and
// builds what the core sees.
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
//   - Layers (inspired by Steam Input and reWASD): each controller is in one
//     layer at a time and only that layer's bindings do anything:
//       HOTKEY     its modifier (hotkey_enable) is active: hotkeys only;
//       INTERFACE  the UI is up (settings.ui_active): interface actions, for
//                  the keyboard and port 1's controller;
//       GAME       otherwise, once joined: the RetroPad of its port;
//       NONE       nothing (not joined, another port while the UI is up,
//                  binding by pressing, mapping).
//     Entering the GAME or INTERFACE layer from another one, what was already
//     held is ignored, each until let go (the button that closed the menu,
//     the START that opened it); joining, only the START that joined.
//   - The modifier's mode is the controller's (keyboard too): HOLD = the
//     HOTKEY layer while it is held (after a short delay, so a quick tap of
//     it still reaches the game; at once if a hotkey is pressed with it);
//     ONE HOTKEY = a double tap turns it on until one hotkey fires (or a few
//     seconds pass); STAYS ON = a double tap turns it on, another turns it off.
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

// One action's sources (INPUT_SOURCES_PER_ACTION of them) as rows, the way
// the binding tables show them: one row per source, in order.
uint32_t input_sources_count(const InputSource* sources);
// Puts `source` on `row` (row == count: a new row, if there is room). The
// same control on another row moves here.
void input_sources_set(InputSource* sources, uint32_t row, InputSource source);
void input_sources_remove(InputSource* sources, uint32_t row);
bool input_is_bound(const InputBindingSet* set, InputAction action, InputSource source);

// The defaults for the keyboard (arrows, Z X A S Q W E R, Enter, Right Shift;
// Escape goes back in the UI (Backspace is kept for rewind), F1 opens the
// menu bar's first menu; the hotkey
// modifier is Left Shift:
// Shift + F1 quick menu, F2 save, F4 load, Escape menu, Space fast forward).
void input_default_keyboard_bindings(InputBindingSet* set);

// The defaults for a gamepad, by position (the RetroPad's SNES layout: B
// bottom, A right, Y left, X top; triggers L2 / R2, stick clicks L3 / R3,
// sticks analog). Interface: d-pad and left stick, accept = bottom, back =
// right, shoulders page, START opens the menu bar's first menu. Hotkeys with
// HOTKEY ENABLE = Select: Start quick menu, right button menu, R save, L
// load, R2 fast forward.
void input_default_gamepad_bindings(InputBindingSet* set);

enum class InputHotkeyMode
{
    hold,
    one_hotkey,
    stays_on,
};

// "hold", "one_hotkey", "stays_on" (for settings files).
const char* input_hotkey_mode_name(InputHotkeyMode mode);
bool        input_hotkey_mode_parse(const char* name, InputHotkeyMode* mode);

enum class InputLayer
{
    none,
    interface,
    hotkey,
    game,
};

struct InputSettings
{
    bool keyboard_shares_port1 = true;
    bool ui_active = false;   // the frontend's UI is up (a menu or a dialog): the INTERFACE layer
    uint64_t press_ms = 5000;   // binding: how long a control waits for a press before going on
    uint64_t hold_ms = 500;     // binding: how long a gamepad's control must be held to be taken
};

struct InputController
{
    uint32_t        device_id = 0;
    std::string     name;               // shown in the UI, editable
    InputBindingSet bindings;
    float           deadzone = 0.15f;   // sticks, 0..1
    InputHotkeyMode hotkey_mode = InputHotkeyMode::hold;

    // Kept between frames by input_update().
    int32_t    last_port = INPUT_NO_PORT;            // the port it had when its device went away
    bool       held_last[INPUT_ACTION_COUNT] = {};
    InputLayer layer = InputLayer::none;
    bool       held_at_switch[INPUT_ACTION_COUNT] = {};   // held when it entered `layer`: ignored until let go
    bool       paused = false;   // was in no layer for binding or mapping: what's held as it comes back is ignored too
    uint64_t   modifier_down_ms = 0;
    uint64_t   modifier_tap_ms = 0;                  // the modifier's last tap (double taps)
    bool       modifier_on = false;                  // ONE HOTKEY / STAYS ON: turned on
    uint64_t   modifier_on_ms = 0;
    bool       modifier_used = false;                // a hotkey fired since the modifier went down
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
    std::string                       appearance;   // gamepads: how it looks ("xbox", "sony"...); "none" = declined; "" = never asked
    InputHotkeyMode                   hotkey_mode = InputHotkeyMode::hold;
    std::string                       sdl_mapping;  // gamepads: our SDL mapping line (MAP CONTROLLER); "" = SDL's own
    std::vector<InputBindingOverride> overrides;
};

// A controller's bindings for one core and one emulated device (step 5 of
// milestone 3): what it does when that core asks for that device on its
// port. `custom` = changes to its RetroPad bindings (the overrides apply);
// otherwise the RetroPad bindings are used as they are, and `decided` says
// the user was asked and chose that (so they aren't asked again).
struct InputCoreProfile
{
    std::string                       core;          // the core's short name
    uint32_t                          device = 0;    // the emulated device's id (the core's own numbers)
    std::string                       profile_id;    // the controller's (input_controller_profile_id)
    bool                              decided = false;
    bool                              custom = false;
    std::vector<InputBindingOverride> overrides;     // RetroPad actions only
};

// Waiting for the next control pressed on one device, to bind it
// (input_capture_*). While active, input_update reports no actions and no
// RetroPad: the press belongs to the binding.
struct InputCapture
{
    bool     active = false;
    uint32_t device_id = 0;
    uint8_t  held_at_start[INPUT_KEY_COUNT] = {};
    // Axis halves already pushed past half when it started (they count
    // only once let go and pushed again; going back to the center isn't a push).
    bool     plus_at_start[INPUT_AXIS_COUNT] = {};
    bool     minus_at_start[INPUT_AXIS_COUNT] = {};
    // A gamepad's control is taken once held settings.hold_ms (the frontend
    // shows which one meanwhile, to confirm it; let go sooner and it isn't).
    InputSource candidate = {};
    uint64_t    candidate_since_ms = 0;
};

struct InputSystem
{
    InputDevices                 devices;
    std::vector<InputController> controllers;
    std::vector<InputProfile>    profiles;   // loaded, plus what input_config_write took from the controllers
    std::vector<InputCoreProfile> core_profiles;

    // What runs now (set by the frontend): the core and the emulated device
    // on each port. The RetroPad of a port comes from the controller's
    // InputCoreProfile for them, when it is custom.
    std::string core_name;
    uint32_t    port_devices[INPUT_PORT_COUNT] = {1, 1, 1, 1};   // 1 = the libretro joypad
    int32_t                      ports[INPUT_PORT_COUNT] = {INPUT_NO_PORT, INPUT_NO_PORT, INPUT_NO_PORT, INPUT_NO_PORT};
    InputSettings                settings;
    InputCapture                 capture;
    // Nothing is routed (no actions, no RetroPad) while set: a controller is
    // being mapped raw, so its presses must not do anything else.
    bool                         suspended = false;
};

struct InputFrame
{
    uint16_t pad_buttons[INPUT_PORT_COUNT] = {};    // bit N = RETRO_DEVICE_ID_JOYPAD_N
    int16_t  analog[INPUT_PORT_COUNT][2][2] = {};   // [port][left/right][x/y], -0x7fff..0x7fff
    bool     held[INPUT_ACTION_COUNT] = {};         // interface and hotkey actions, from controllers in those layers
    bool     pressed[INPUT_ACTION_COUNT] = {};      // went down this frame
    bool     keyboard_pressed[INPUT_ACTION_COUNT] = {};   // the same, from the keyboard only
    bool     gamepad_held[INPUT_ACTION_COUNT] = {}; // interface actions held on port 1's gamepad: to drive a UI
    bool     gamepad_enable_alone = false;          // port 1's gamepad holds HOTKEY ENABLE and no hotkey fired with it
    int32_t  joined_port = INPUT_NO_PORT;           // a controller joined this frame (for a message)
    int32_t  joined_controller = -1;
    int32_t  left_port = INPUT_NO_PORT;             // a controller's device went away and freed this port
    int32_t  left_controller = -1;
};

// The profile with this id, or nullptr; _for makes one.
const InputProfile* input_profile(const InputSystem* system, const std::string& id);
InputProfile*       input_profile_for(InputSystem* system, const std::string& id);

// The defaults for a device's kind (keyboard or gamepad).
void input_default_bindings(InputDeviceKind kind, InputBindingSet* set);

// The defaults for this device: its kind's, and for a gamepad without d-pad
// buttons (has_dpad false) the RetroPad's directions on the left stick.
void input_default_bindings_for(const InputDevice* device, InputBindingSet* set);

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

// The controller's profile for a core and device, or nullptr. _for makes one.
const InputCoreProfile* input_core_profile(const InputSystem* system, const char* core, uint32_t device,
                                           const std::string& profile_id);
InputCoreProfile*       input_core_profile_for(InputSystem* system, const char* core, uint32_t device,
                                               const std::string& profile_id);

// The bindings a controller plays with on a port now: its own, with the
// RetroPad changes of its custom profile for the running core and the
// port's device.
void input_effective_bindings(const InputSystem* system, uint32_t controller, uint32_t port, InputBindingSet* out);

// Is this action held on this controller right now (its own bindings)?
bool input_controller_action_held(const InputSystem* system, uint32_t controller, InputAction action);

// Removes a controller (a keyboard part); ports after it are renumbered.
void input_remove_controller(InputSystem* system, uint32_t controller);

// Binding by pressing: start waiting on a device, then poll each frame until
// it gives a source (a key or button that wasn't held when it started, or
// an axis half pushed past half from the center that wasn't when it
// started). A key counts at once; a gamepad's control once held
// settings.hold_ms (the frontend shows it meanwhile, to confirm it). Cancel
// to give up. When it ends, what is still held does nothing until let go
// (it doesn't close the window it was for).
void input_capture_begin(InputSystem* system, uint32_t device_id);
bool input_capture_poll(InputSystem* system, uint64_t now_ms, InputSource* source);
void input_capture_cancel(InputSystem* system);

// The gamepad control being held to be taken (false if none), and how far
// its hold has come (0 .. 1).
bool  input_capture_candidate(const InputSystem* system, InputSource* source);
float input_capture_hold_progress(const InputSystem* system, uint64_t now_ms);

// Swaps two ports' controllers (reordering from the quick menu).
void input_swap_ports(InputSystem* system, uint32_t a, uint32_t b);

// One frame: joins, ports of devices that left or came back, hotkey state,
// and what every action says. Call once per frame, then
// input_devices_end_frame(&system->devices).
void input_update(InputSystem* system, uint64_t now_ms, InputFrame* frame);

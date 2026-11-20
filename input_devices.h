#pragma once

// The input router (input_devices, input_bindings, input_config; backends
// like input_sdl feed it): takes input from any source and routes it to the
// game's ports, the UI and the hotkeys. This part: the physical controllers
// and their raw state, with no libretro and no SDL in it: the keyboard, gamepads, later a phone or a network client. A backend
// (input_sdl for SDL) adds devices and feeds their keys, buttons and axes;
// input_bindings turns that into actions.
//
// Codes:
//   keys     USB HID usage ids (SDL scancodes use the same numbers);
//   buttons  InputButton, by position (SDL's gamepad layout);
//   axes     InputAxis, -1..1 (triggers 0..1);
//   mouse    InputMouseButton (the wheel's two ways as taps), plus its
//            motion and position (in window pixels).
//
// Short taps: a press that comes and goes between two frames is remembered
// (tapped) until input_devices_end_frame(), so it still counts once.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class InputDeviceKind
{
    keyboard,
    gamepad,
    mouse,
};

enum class InputMouseButton
{
    left,
    right,
    middle,
    x1,
    x2,
    wheel_up,     // the wheel: a tap per notch
    wheel_down,
    count,
};

constexpr uint32_t INPUT_MOUSE_BUTTON_COUNT = static_cast<uint32_t>(InputMouseButton::count);

constexpr uint32_t INPUT_KEY_COUNT = 512;

enum class InputButton
{
    south,
    east,
    west,
    north,
    back,
    guide,
    start,
    left_stick,
    right_stick,
    left_shoulder,
    right_shoulder,
    dpad_up,
    dpad_down,
    dpad_left,
    dpad_right,
    misc1,
    right_paddle1,
    left_paddle1,
    right_paddle2,
    left_paddle2,
    touchpad,
    misc2,          // SDL3's extra buttons; MAP CONTROLLER's L4 / R4 are misc5 / misc6
    misc3,
    misc4,
    misc5,
    misc6,
    count,
};

enum class InputAxis
{
    left_x,
    left_y,
    right_x,
    right_y,
    left_trigger,
    right_trigger,
    count,
};

enum class InputIgnore
{
    automatic,
    yes,
    no,
};

constexpr uint32_t INPUT_BUTTON_COUNT = static_cast<uint32_t>(InputButton::count);
constexpr uint32_t INPUT_AXIS_COUNT = static_cast<uint32_t>(InputAxis::count);

struct InputDevice
{
    uint32_t        id = 0;          // unique while the program runs (input_devices_add)
    InputDeviceKind kind = InputDeviceKind::keyboard;
    std::string     guid;            // identity across runs: same model = same guid
    std::string     name;            // as the system reports it
    bool            connected = false;

    uint8_t held[INPUT_KEY_COUNT] = {};     // keyboard keys, or buttons (first INPUT_BUTTON_COUNT)
    uint8_t tapped[INPUT_KEY_COUNT] = {};   // pressed since the last end_frame
    float   axes[INPUT_AXIS_COUNT] = {};
    // Axes pinned at an extreme since the device appeared and never moved:
    // a device that isn't really a gamepad (a keyboard's thumbstick receiver).
    bool    axes_moved = false;
    bool    axes_pinned_at_start = false;

    // A gamepad whose d-pad SDL reports as d-pad buttons (false: some pads
    // report it as the left stick, so the defaults use that instead).
    bool    has_dpad = true;

    // The user's word on it: auto = ignored only if it looks fake.
    InputIgnore ignore = InputIgnore::automatic;

    // Only for playing: never the interface, even on port 1, and of the
    // hotkeys only the game's (SAVE STATE, LOAD STATE, FAST FORWARD). Set for
    // remote devices (input_net): someone on the network plays, the menus
    // stay with the people at this machine.
    bool    game_only = false;
    // A game-only device may use those game hotkeys at all (input_net: only
    // devices with STREAM access or above; Andrés, 2026-11-20).
    bool    game_hotkeys = true;

    // A RetroPad from the network (input_net): the remote app already mapped
    // its controls to the core's RetroPad (Andrés: the phone does the
    // mapping), so the router hands these to the device's port as they are:
    // no bindings, remaps, turbo or combos. START still joins a port.
    bool     retropad = false;
    uint16_t retropad_buttons = 0;        // bit N = RETRO_DEVICE_ID_JOYPAD_N, held now
    uint16_t retropad_tapped = 0;         // went down since the last end_frame (a tap inside one frame counts)
    int16_t  retropad_analog[2][2] = {};  // [left/right][x/y], -0x7fff..0x7fff

    // A mouse: how far it moved since the last end_frame, and where it is
    // in the window (when the system says; not while it is grabbed).
    float   motion_x = 0.0f;
    float   motion_y = 0.0f;
    bool    has_position = false;
    float   position_x = 0.0f;
    float   position_y = 0.0f;
};

struct InputDevices
{
    std::vector<InputDevice> devices;
    uint32_t                 next_id = 1;
};

// A new device (connected). Returns its id. A device that comes back (same
// guid, disconnected) is reused, so it keeps its id and its port.
uint32_t input_devices_add(InputDevices* devices, InputDeviceKind kind, const char* guid, const char* name);
void     input_devices_disconnect(InputDevices* devices, uint32_t id);

InputDevice*       input_devices_find(InputDevices* devices, uint32_t id);
const InputDevice* input_devices_find_const(const InputDevices* devices, uint32_t id);

// A RetroPad device's state (see InputDevice::retropad).
void input_devices_set_retropad(InputDevices* devices, uint32_t id, uint16_t buttons, const int16_t analog[2][2]);

// Keys (keyboard) and buttons (gamepad) share the code space of `held`.
void input_devices_set_code(InputDevices* devices, uint32_t id, uint32_t code, bool down);
void input_devices_set_axis(InputDevices* devices, uint32_t id, InputAxis axis, float value);

// A mouse: motion (added up until end_frame), its position, a wheel notch.
void input_devices_add_motion(InputDevices* devices, uint32_t id, float dx, float dy);
void input_devices_set_position(InputDevices* devices, uint32_t id, float x, float y);
void input_devices_wheel(InputDevices* devices, uint32_t id, bool up);

// Releases everything a device holds (focus lost: the key-ups go elsewhere).
void input_devices_release_all(InputDevices* devices, uint32_t id);

// After the frame's input was read: forget the taps and the mice's motion.
void input_devices_end_frame(InputDevices* devices);

// A device whose axes sat at an extreme from the start and never moved.
bool input_devices_looks_fake(const InputDevice* device);

// Not used for input: the user said so, or (automatic) it looks fake.
bool input_devices_ignored(const InputDevice* device);

// ---------------------------------------------------------------------------
// Sources: one physical control, and its text form for settings files
// ---------------------------------------------------------------------------

enum class InputSourceType
{
    none,
    key,          // code = HID usage
    button,       // code = InputButton
    axis_plus,    // code = InputAxis, the positive half
    axis_minus,   // the negative half
    mouse,        // code = InputMouseButton
};

struct InputSource
{
    InputSourceType type = InputSourceType::none;
    uint16_t        code = 0;
    // A key or button pushes an analog action (a stick direction) this far
    // (1..100): D-pad right at 20 = the stick pushed a fifth of the way.
    // Settings text: "button:dpad_right@20". Axes give their own value.
    uint8_t         percent = 100;
    // A key or button on a RetroPad button with turbo: pressed and let go
    // every this many frames while held (0 = no turbo). Text: "button:x*2".
    uint8_t         turbo = 0;
};

// The same control (the percent doesn't count).
bool input_source_equal(InputSource a, InputSource b);

// How far an axis is pushed toward the source's direction, 0..1 (keys and
// buttons: 0 or 1; taps count as 1).
float input_source_value(const InputDevice* device, InputSource source);

// "key:Z", "key:Return", "button:south", "axis:left_x+", "axis:right_trigger+".
// Unknown key codes are written as numbers ("key:#231"). False if it doesn't fit.
bool input_source_format(InputSource source, char* out, size_t out_size);
bool input_source_parse(const char* text, InputSource* source);

// The name a key has in the text form ("Z", "Return", "F1"...), or nullptr.
const char* input_key_name(uint32_t code);

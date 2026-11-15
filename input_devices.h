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
//   axes     InputAxis, -1..1 (triggers 0..1).
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
};

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

    // The user's word on it: auto = ignored only if it looks fake.
    InputIgnore ignore = InputIgnore::automatic;
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

// Keys (keyboard) and buttons (gamepad) share the code space of `held`.
void input_devices_set_code(InputDevices* devices, uint32_t id, uint32_t code, bool down);
void input_devices_set_axis(InputDevices* devices, uint32_t id, InputAxis axis, float value);

// Releases everything a device holds (focus lost: the key-ups go elsewhere).
void input_devices_release_all(InputDevices* devices, uint32_t id);

// After the frame's input was read: forget the taps.
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
};

struct InputSource
{
    InputSourceType type = InputSourceType::none;
    uint16_t        code = 0;
};

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

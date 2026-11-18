#include "input_devices.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// An axis this close to an extreme counts as pinned there.
constexpr float PINNED_AXIS = 0.99f;

static void release(InputDevice* device)
{
    std::memset(device->held, 0, sizeof(device->held));
    for (uint32_t a = 0; a < INPUT_AXIS_COUNT; a++)
    {
        device->axes[a] = 0.0f;
    }
}

uint32_t input_devices_add(InputDevices* devices, InputDeviceKind kind, const char* guid, const char* name)
{
    for (size_t i = 0; i < devices->devices.size(); i++)
    {
        InputDevice* device = &devices->devices[i];
        if (!device->connected && device->kind == kind && device->guid == guid)
        {
            device->connected = true;
            device->name = name;
            release(device);
            std::memset(device->tapped, 0, sizeof(device->tapped));
            device->axes_moved = false;
            device->axes_pinned_at_start = false;
            return device->id;
        }
    }
    InputDevice device = {};
    device.id = devices->next_id++;
    device.kind = kind;
    device.guid = guid;
    device.name = name;
    device.connected = true;
    devices->devices.push_back(device);
    return device.id;
}

void input_devices_disconnect(InputDevices* devices, uint32_t id)
{
    InputDevice* device = input_devices_find(devices, id);
    if (device != nullptr)
    {
        device->connected = false;
        release(device);
    }
}

InputDevice* input_devices_find(InputDevices* devices, uint32_t id)
{
    for (size_t i = 0; i < devices->devices.size(); i++)
    {
        if (devices->devices[i].id == id)
        {
            return &devices->devices[i];
        }
    }
    return nullptr;
}

const InputDevice* input_devices_find_const(const InputDevices* devices, uint32_t id)
{
    for (size_t i = 0; i < devices->devices.size(); i++)
    {
        if (devices->devices[i].id == id)
        {
            return &devices->devices[i];
        }
    }
    return nullptr;
}

void input_devices_set_code(InputDevices* devices, uint32_t id, uint32_t code, bool down)
{
    InputDevice* device = input_devices_find(devices, id);
    if (device == nullptr || code >= INPUT_KEY_COUNT)
    {
        return;
    }
    if (down && device->held[code] == 0)
    {
        device->tapped[code] = 1;
    }
    device->held[code] = down ? 1 : 0;
}

void input_devices_set_axis(InputDevices* devices, uint32_t id, InputAxis axis, float value)
{
    InputDevice* device = input_devices_find(devices, id);
    uint32_t index = static_cast<uint32_t>(axis);
    if (device == nullptr || index >= INPUT_AXIS_COUNT)
    {
        return;
    }
    value = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
    bool pinned = value >= PINNED_AXIS || value <= -PINNED_AXIS;
    if (!device->axes_moved)
    {
        // The first values decide whether it starts pinned; any value away
        // from the extremes afterwards means a real stick.
        if (pinned && device->axes[index] == 0.0f)
        {
            device->axes_pinned_at_start = true;
        }
        else if (!pinned && value != 0.0f)
        {
            device->axes_moved = true;
        }
    }
    device->axes[index] = value;
}

void input_devices_release_all(InputDevices* devices, uint32_t id)
{
    InputDevice* device = input_devices_find(devices, id);
    if (device != nullptr)
    {
        release(device);
    }
}

void input_devices_end_frame(InputDevices* devices)
{
    for (size_t i = 0; i < devices->devices.size(); i++)
    {
        std::memset(devices->devices[i].tapped, 0, sizeof(devices->devices[i].tapped));
    }
}

bool input_devices_looks_fake(const InputDevice* device)
{
    return device->kind == InputDeviceKind::gamepad && device->axes_pinned_at_start && !device->axes_moved;
}

bool input_devices_ignored(const InputDevice* device)
{
    if (device->ignore != InputIgnore::automatic)
    {
        return device->ignore == InputIgnore::yes;
    }
    return input_devices_looks_fake(device);
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

bool input_source_equal(InputSource a, InputSource b)
{
    return a.type == b.type && (a.type == InputSourceType::none || a.code == b.code);
}

float input_source_value(const InputDevice* device, InputSource source)
{
    switch (source.type)
    {
    case InputSourceType::key:
    case InputSourceType::button:
        if (source.code >= INPUT_KEY_COUNT)
        {
            return 0.0f;
        }
        if (device->held[source.code] == 0 && device->tapped[source.code] == 0)
        {
            return 0.0f;
        }
        return source.percent >= 100 || source.percent == 0 ? 1.0f : static_cast<float>(source.percent) / 100.0f;
    case InputSourceType::axis_plus:
    case InputSourceType::axis_minus:
    {
        if (source.code >= INPUT_AXIS_COUNT)
        {
            return 0.0f;
        }
        float value = device->axes[source.code];
        value = source.type == InputSourceType::axis_plus ? value : -value;
        return value > 0.0f ? value : 0.0f;
    }
    case InputSourceType::none:
        break;
    }
    return 0.0f;
}

struct KeyName
{
    uint16_t    code;
    const char* name;
};

// HID usage ids (the keyboard page).
static const KeyName KEY_NAMES[] = {
    {4, "A"}, {5, "B"}, {6, "C"}, {7, "D"}, {8, "E"}, {9, "F"}, {10, "G"}, {11, "H"}, {12, "I"},
    {13, "J"}, {14, "K"}, {15, "L"}, {16, "M"}, {17, "N"}, {18, "O"}, {19, "P"}, {20, "Q"}, {21, "R"},
    {22, "S"}, {23, "T"}, {24, "U"}, {25, "V"}, {26, "W"}, {27, "X"}, {28, "Y"}, {29, "Z"},
    {30, "1"}, {31, "2"}, {32, "3"}, {33, "4"}, {34, "5"}, {35, "6"}, {36, "7"}, {37, "8"}, {38, "9"}, {39, "0"},
    {40, "Return"}, {41, "Escape"}, {42, "Backspace"}, {43, "Tab"}, {44, "Space"},
    {45, "Minus"}, {46, "Equals"}, {47, "LeftBracket"}, {48, "RightBracket"}, {49, "Backslash"},
    {51, "Semicolon"}, {52, "Apostrophe"}, {53, "Grave"}, {54, "Comma"}, {55, "Period"}, {56, "Slash"},
    {57, "CapsLock"},
    {58, "F1"}, {59, "F2"}, {60, "F3"}, {61, "F4"}, {62, "F5"}, {63, "F6"}, {64, "F7"}, {65, "F8"},
    {66, "F9"}, {67, "F10"}, {68, "F11"}, {69, "F12"},
    {70, "PrintScreen"}, {71, "ScrollLock"}, {72, "Pause"}, {73, "Insert"}, {74, "Home"}, {75, "PageUp"},
    {76, "Delete"}, {77, "End"}, {78, "PageDown"}, {79, "Right"}, {80, "Left"}, {81, "Down"}, {82, "Up"},
    {83, "NumLock"}, {84, "KeypadDivide"}, {85, "KeypadMultiply"}, {86, "KeypadMinus"}, {87, "KeypadPlus"},
    {88, "KeypadEnter"}, {89, "Keypad1"}, {90, "Keypad2"}, {91, "Keypad3"}, {92, "Keypad4"}, {93, "Keypad5"},
    {94, "Keypad6"}, {95, "Keypad7"}, {96, "Keypad8"}, {97, "Keypad9"}, {98, "Keypad0"}, {99, "KeypadPeriod"},
    {224, "LeftCtrl"}, {225, "LeftShift"}, {226, "LeftAlt"}, {227, "LeftGui"},
    {228, "RightCtrl"}, {229, "RightShift"}, {230, "RightAlt"}, {231, "RightGui"},
};
constexpr size_t KEY_NAME_COUNT = sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]);

static const char* const BUTTON_NAMES[INPUT_BUTTON_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "left_stick", "right_stick",
    "left_shoulder", "right_shoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1",
    "right_paddle1", "left_paddle1", "right_paddle2", "left_paddle2", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6",
};

static const char* const AXIS_NAMES[INPUT_AXIS_COUNT] = {
    "left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger",
};

const char* input_key_name(uint32_t code)
{
    for (size_t i = 0; i < KEY_NAME_COUNT; i++)
    {
        if (KEY_NAMES[i].code == code)
        {
            return KEY_NAMES[i].name;
        }
    }
    return nullptr;
}

static bool format_control(InputSource source, char* out, size_t out_size)
{
    int written = 0;
    switch (source.type)
    {
    case InputSourceType::key:
    {
        const char* name = input_key_name(source.code);
        written = name != nullptr ? std::snprintf(out, out_size, "key:%s", name)
                                  : std::snprintf(out, out_size, "key:#%u", static_cast<unsigned>(source.code));
        break;
    }
    case InputSourceType::button:
        if (source.code >= INPUT_BUTTON_COUNT)
        {
            return false;
        }
        written = std::snprintf(out, out_size, "button:%s", BUTTON_NAMES[source.code]);
        break;
    case InputSourceType::axis_plus:
    case InputSourceType::axis_minus:
        if (source.code >= INPUT_AXIS_COUNT)
        {
            return false;
        }
        written = std::snprintf(out, out_size, "axis:%s%c", AXIS_NAMES[source.code],
                                source.type == InputSourceType::axis_plus ? '+' : '-');
        break;
    case InputSourceType::none:
        written = std::snprintf(out, out_size, "none");
        break;
    }
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool input_source_format(InputSource source, char* out, size_t out_size)
{
    if (!format_control(source, out, out_size))
    {
        return false;
    }
    bool digital = source.type == InputSourceType::key || source.type == InputSourceType::button;
    if (digital && source.percent > 0 && source.percent < 100)
    {
        size_t length = std::strlen(out);
        int written = std::snprintf(out + length, out_size - length, "@%u", static_cast<unsigned>(source.percent));
        return written > 0 && static_cast<size_t>(written) < out_size - length;
    }
    return true;
}

static bool parse_control(const char* text, InputSource* source)
{
    *source = InputSource{};
    if (std::strcmp(text, "none") == 0)
    {
        return true;
    }
    if (std::strncmp(text, "key:", 4) == 0)
    {
        const char* name = text + 4;
        if (name[0] == '#')
        {
            char* end = nullptr;
            unsigned long code = std::strtoul(name + 1, &end, 10);
            if (end == name + 1 || *end != '\0' || code >= INPUT_KEY_COUNT)
            {
                return false;
            }
            source->type = InputSourceType::key;
            source->code = static_cast<uint16_t>(code);
            return true;
        }
        for (size_t i = 0; i < KEY_NAME_COUNT; i++)
        {
            if (std::strcmp(KEY_NAMES[i].name, name) == 0)
            {
                source->type = InputSourceType::key;
                source->code = KEY_NAMES[i].code;
                return true;
            }
        }
        return false;
    }
    if (std::strncmp(text, "button:", 7) == 0)
    {
        for (uint32_t i = 0; i < INPUT_BUTTON_COUNT; i++)
        {
            if (std::strcmp(BUTTON_NAMES[i], text + 7) == 0)
            {
                source->type = InputSourceType::button;
                source->code = static_cast<uint16_t>(i);
                return true;
            }
        }
        return false;
    }
    if (std::strncmp(text, "axis:", 5) == 0)
    {
        const char* name = text + 5;
        size_t length = std::strlen(name);
        if (length < 2 || (name[length - 1] != '+' && name[length - 1] != '-'))
        {
            return false;
        }
        for (uint32_t i = 0; i < INPUT_AXIS_COUNT; i++)
        {
            if (std::strlen(AXIS_NAMES[i]) == length - 1 && std::strncmp(AXIS_NAMES[i], name, length - 1) == 0)
            {
                source->type = name[length - 1] == '+' ? InputSourceType::axis_plus : InputSourceType::axis_minus;
                source->code = static_cast<uint16_t>(i);
                return true;
            }
        }
        return false;
    }
    return false;
}

bool input_source_parse(const char* text, InputSource* source)
{
    // "<control>@<percent>": the percent apart.
    const char* at = std::strchr(text, '@');
    if (at == nullptr)
    {
        return parse_control(text, source);
    }
    char control[96] = {};
    size_t length = static_cast<size_t>(at - text);
    if (length == 0 || length >= sizeof(control))
    {
        return false;
    }
    std::memcpy(control, text, length);
    char* end = nullptr;
    unsigned long percent = std::strtoul(at + 1, &end, 10);
    if (end == at + 1 || *end != '\0' || percent < 1 || percent > 100 || !parse_control(control, source))
    {
        return false;
    }
    source->percent = static_cast<uint8_t>(percent);
    return true;
}

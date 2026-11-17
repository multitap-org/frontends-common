#include "input_bindings.h"

#include <cmath>
#include <cstring>

// A digital action counts as pressed past this (keys and buttons are 0 or 1).
constexpr float PRESSED_THRESHOLD = 0.5f;
// HOLD: how long the modifier must be held before the HOTKEY layer is on
// (unless a hotkey is pressed with it).
constexpr uint64_t HOTKEY_BLOCK_DELAY_MS = 80;
// ONE HOTKEY / STAYS ON: two presses of the modifier this close turn it on;
// ONE HOTKEY turns itself off after this long without a hotkey.
constexpr uint64_t DOUBLE_TAP_MS = 300;
constexpr uint64_t ARMED_TIMEOUT_MS = 3000;

static const char* const ACTION_NAMES[INPUT_ACTION_COUNT] = {
    "pad.b", "pad.y", "pad.select", "pad.start", "pad.up", "pad.down", "pad.left", "pad.right",
    "pad.a", "pad.x", "pad.l", "pad.r", "pad.l2", "pad.r2", "pad.l3", "pad.r3",
    "pad.left_x+", "pad.left_x-", "pad.left_y+", "pad.left_y-",
    "pad.right_x+", "pad.right_x-", "pad.right_y+", "pad.right_y-",
    "ui.up", "ui.down", "ui.left", "ui.right", "ui.accept", "ui.back", "ui.page_left", "ui.page_right", "ui.open_menu",
    "hotkey.enable", "hotkey.menu", "hotkey.quick_menu", "hotkey.save_state", "hotkey.load_state",
    "hotkey.fast_forward",
};

static uint32_t index_of(InputAction action)
{
    return static_cast<uint32_t>(action);
}

const char* input_hotkey_mode_name(InputHotkeyMode mode)
{
    switch (mode)
    {
    case InputHotkeyMode::one_hotkey: return "one_hotkey";
    case InputHotkeyMode::stays_on:   return "stays_on";
    case InputHotkeyMode::hold:       break;
    }
    return "hold";
}

bool input_hotkey_mode_parse(const char* name, InputHotkeyMode* mode)
{
    static const InputHotkeyMode MODES[] = {InputHotkeyMode::hold, InputHotkeyMode::one_hotkey, InputHotkeyMode::stays_on};
    for (int m = 0; m < 3; m++)
    {
        if (std::strcmp(input_hotkey_mode_name(MODES[m]), name) == 0)
        {
            *mode = MODES[m];
            return true;
        }
    }
    return false;
}

const char* input_action_name(InputAction action)
{
    uint32_t index = index_of(action);
    return index < INPUT_ACTION_COUNT ? ACTION_NAMES[index] : "";
}

bool input_action_parse(const char* name, InputAction* action)
{
    for (uint32_t i = 0; i < INPUT_ACTION_COUNT; i++)
    {
        if (std::strcmp(ACTION_NAMES[i], name) == 0)
        {
            *action = static_cast<InputAction>(i);
            return true;
        }
    }
    return false;
}

bool input_action_is_pad(InputAction action)
{
    return index_of(action) <= index_of(InputAction::pad_right_y_minus);
}

bool input_action_is_hotkey(InputAction action)
{
    return index_of(action) >= index_of(InputAction::hotkey_enable) && index_of(action) < INPUT_ACTION_COUNT;
}

// ---------------------------------------------------------------------------
// Binding sets
// ---------------------------------------------------------------------------

bool input_bind(InputBindingSet* set, InputAction action, InputSource source)
{
    uint32_t index = index_of(action);
    if (index >= INPUT_ACTION_COUNT || source.type == InputSourceType::none)
    {
        return false;
    }
    InputSource* slots = set->sources[index];
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        if (input_source_equal(slots[s], source))
        {
            return true;
        }
    }
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        if (slots[s].type == InputSourceType::none)
        {
            slots[s] = source;
            return true;
        }
    }
    return false;
}

void input_unbind_all(InputBindingSet* set, InputAction action)
{
    uint32_t index = index_of(action);
    if (index >= INPUT_ACTION_COUNT)
    {
        return;
    }
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        set->sources[index][s] = InputSource{};
    }
}

bool input_is_bound(const InputBindingSet* set, InputAction action, InputSource source)
{
    uint32_t index = index_of(action);
    if (index >= INPUT_ACTION_COUNT)
    {
        return false;
    }
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        if (set->sources[index][s].type != InputSourceType::none && input_source_equal(set->sources[index][s], source))
        {
            return true;
        }
    }
    return false;
}

static InputSource key(uint16_t code)
{
    InputSource source = {};
    source.type = InputSourceType::key;
    source.code = code;
    return source;
}

// HID usage ids for the defaults.
constexpr uint16_t HID_A = 4, HID_E = 8, HID_Q = 20, HID_R = 21, HID_S = 22, HID_W = 26, HID_X = 27, HID_Z = 29;
constexpr uint16_t HID_RETURN = 40, HID_ESCAPE = 41, HID_SPACE = 44;
constexpr uint16_t HID_F1 = 58, HID_F2 = 59, HID_F4 = 61;
constexpr uint16_t HID_PAGE_UP = 75, HID_PAGE_DOWN = 78;
constexpr uint16_t HID_RIGHT = 79, HID_LEFT = 80, HID_DOWN = 81, HID_UP = 82;
constexpr uint16_t HID_RIGHT_SHIFT = 229;
constexpr uint16_t HID_LEFT_SHIFT = 225;

void input_default_keyboard_bindings(InputBindingSet* set)
{
    *set = InputBindingSet{};
    input_bind(set, InputAction::pad_up, key(HID_UP));
    input_bind(set, InputAction::pad_down, key(HID_DOWN));
    input_bind(set, InputAction::pad_left, key(HID_LEFT));
    input_bind(set, InputAction::pad_right, key(HID_RIGHT));
    input_bind(set, InputAction::pad_a, key(HID_X));
    input_bind(set, InputAction::pad_b, key(HID_Z));
    input_bind(set, InputAction::pad_x, key(HID_S));
    input_bind(set, InputAction::pad_y, key(HID_A));
    input_bind(set, InputAction::pad_l, key(HID_Q));
    input_bind(set, InputAction::pad_r, key(HID_W));
    input_bind(set, InputAction::pad_l2, key(HID_E));
    input_bind(set, InputAction::pad_r2, key(HID_R));
    input_bind(set, InputAction::pad_start, key(HID_RETURN));
    input_bind(set, InputAction::pad_select, key(HID_RIGHT_SHIFT));

    input_bind(set, InputAction::ui_up, key(HID_UP));
    input_bind(set, InputAction::ui_down, key(HID_DOWN));
    input_bind(set, InputAction::ui_left, key(HID_LEFT));
    input_bind(set, InputAction::ui_right, key(HID_RIGHT));
    input_bind(set, InputAction::ui_accept, key(HID_RETURN));
    input_bind(set, InputAction::ui_back, key(HID_ESCAPE));   // Backspace is kept for rewind
    input_bind(set, InputAction::ui_page_left, key(HID_PAGE_UP));
    input_bind(set, InputAction::ui_page_right, key(HID_PAGE_DOWN));
    input_bind(set, InputAction::ui_open_menu, key(HID_F1));   // like the quick menu's hotkey (Shift + F1) in a game

    // The modifier: Left Shift ("shift" to the HOTKEY layer; Right Shift is SELECT).
    input_bind(set, InputAction::hotkey_enable, key(HID_LEFT_SHIFT));
    input_bind(set, InputAction::hotkey_menu, key(HID_ESCAPE));
    input_bind(set, InputAction::hotkey_quick_menu, key(HID_F1));
    input_bind(set, InputAction::hotkey_save_state, key(HID_F2));
    input_bind(set, InputAction::hotkey_load_state, key(HID_F4));
    input_bind(set, InputAction::hotkey_fast_forward, key(HID_SPACE));
}

static InputSource pad_button(InputButton b)
{
    InputSource source = {};
    source.type = InputSourceType::button;
    source.code = static_cast<uint16_t>(b);
    return source;
}

static InputSource pad_axis(InputAxis a, bool plus)
{
    InputSource source = {};
    source.type = plus ? InputSourceType::axis_plus : InputSourceType::axis_minus;
    source.code = static_cast<uint16_t>(a);
    return source;
}

void input_default_gamepad_bindings(InputBindingSet* set)
{
    *set = InputBindingSet{};
    input_bind(set, InputAction::pad_up, pad_button(InputButton::dpad_up));
    input_bind(set, InputAction::pad_down, pad_button(InputButton::dpad_down));
    input_bind(set, InputAction::pad_left, pad_button(InputButton::dpad_left));
    input_bind(set, InputAction::pad_right, pad_button(InputButton::dpad_right));
    input_bind(set, InputAction::pad_b, pad_button(InputButton::south));
    input_bind(set, InputAction::pad_a, pad_button(InputButton::east));
    input_bind(set, InputAction::pad_y, pad_button(InputButton::west));
    input_bind(set, InputAction::pad_x, pad_button(InputButton::north));
    input_bind(set, InputAction::pad_l, pad_button(InputButton::left_shoulder));
    input_bind(set, InputAction::pad_r, pad_button(InputButton::right_shoulder));
    input_bind(set, InputAction::pad_l2, pad_axis(InputAxis::left_trigger, true));
    input_bind(set, InputAction::pad_r2, pad_axis(InputAxis::right_trigger, true));
    input_bind(set, InputAction::pad_l3, pad_button(InputButton::left_stick));
    input_bind(set, InputAction::pad_r3, pad_button(InputButton::right_stick));
    input_bind(set, InputAction::pad_start, pad_button(InputButton::start));
    input_bind(set, InputAction::pad_select, pad_button(InputButton::back));
    input_bind(set, InputAction::pad_left_x_plus, pad_axis(InputAxis::left_x, true));
    input_bind(set, InputAction::pad_left_x_minus, pad_axis(InputAxis::left_x, false));
    input_bind(set, InputAction::pad_left_y_plus, pad_axis(InputAxis::left_y, true));
    input_bind(set, InputAction::pad_left_y_minus, pad_axis(InputAxis::left_y, false));
    input_bind(set, InputAction::pad_right_x_plus, pad_axis(InputAxis::right_x, true));
    input_bind(set, InputAction::pad_right_x_minus, pad_axis(InputAxis::right_x, false));
    input_bind(set, InputAction::pad_right_y_plus, pad_axis(InputAxis::right_y, true));
    input_bind(set, InputAction::pad_right_y_minus, pad_axis(InputAxis::right_y, false));

    input_bind(set, InputAction::ui_up, pad_button(InputButton::dpad_up));
    input_bind(set, InputAction::ui_up, pad_axis(InputAxis::left_y, false));
    input_bind(set, InputAction::ui_down, pad_button(InputButton::dpad_down));
    input_bind(set, InputAction::ui_down, pad_axis(InputAxis::left_y, true));
    input_bind(set, InputAction::ui_left, pad_button(InputButton::dpad_left));
    input_bind(set, InputAction::ui_left, pad_axis(InputAxis::left_x, false));
    input_bind(set, InputAction::ui_right, pad_button(InputButton::dpad_right));
    input_bind(set, InputAction::ui_right, pad_axis(InputAxis::left_x, true));
    // The bottom button accepts, the right one goes back (the frontend may
    // swap them: REVERSE ACCEPT / BACK).
    input_bind(set, InputAction::ui_accept, pad_button(InputButton::south));
    input_bind(set, InputAction::ui_back, pad_button(InputButton::east));
    input_bind(set, InputAction::ui_page_left, pad_button(InputButton::left_shoulder));
    input_bind(set, InputAction::ui_page_right, pad_button(InputButton::right_shoulder));
    input_bind(set, InputAction::ui_open_menu, pad_button(InputButton::start));

    input_bind(set, InputAction::hotkey_enable, pad_button(InputButton::back));
    input_bind(set, InputAction::hotkey_quick_menu, pad_button(InputButton::start));
    input_bind(set, InputAction::hotkey_menu, pad_button(InputButton::east));
    input_bind(set, InputAction::hotkey_save_state, pad_button(InputButton::right_shoulder));
    input_bind(set, InputAction::hotkey_load_state, pad_button(InputButton::left_shoulder));
    input_bind(set, InputAction::hotkey_fast_forward, pad_axis(InputAxis::right_trigger, true));
}

// ---------------------------------------------------------------------------
// Controllers and ports
// ---------------------------------------------------------------------------

void input_default_bindings(InputDeviceKind kind, InputBindingSet* set)
{
    if (kind == InputDeviceKind::keyboard)
    {
        input_default_keyboard_bindings(set);
    }
    else
    {
        input_default_gamepad_bindings(set);
    }
}

void input_default_bindings_for(const InputDevice* device, InputBindingSet* set)
{
    input_default_bindings(device->kind, set);
    if (device->kind != InputDeviceKind::gamepad || device->has_dpad)
    {
        return;
    }
    // Its d-pad comes as the left stick: the RetroPad's directions there.
    input_unbind_all(set, InputAction::pad_up);
    input_unbind_all(set, InputAction::pad_down);
    input_unbind_all(set, InputAction::pad_left);
    input_unbind_all(set, InputAction::pad_right);
    input_bind(set, InputAction::pad_up, pad_axis(InputAxis::left_y, false));
    input_bind(set, InputAction::pad_down, pad_axis(InputAxis::left_y, true));
    input_bind(set, InputAction::pad_left, pad_axis(InputAxis::left_x, false));
    input_bind(set, InputAction::pad_right, pad_axis(InputAxis::left_x, true));
}

const InputProfile* input_profile(const InputSystem* system, const std::string& id)
{
    for (size_t p = 0; p < system->profiles.size(); p++)
    {
        if (system->profiles[p].id == id)
        {
            return &system->profiles[p];
        }
    }
    return nullptr;
}

InputProfile* input_profile_for(InputSystem* system, const std::string& id)
{
    for (size_t p = 0; p < system->profiles.size(); p++)
    {
        if (system->profiles[p].id == id)
        {
            return &system->profiles[p];
        }
    }
    InputProfile profile = {};
    profile.id = id;
    system->profiles.push_back(profile);
    return &system->profiles.back();
}

static const InputProfile* find_profile(const InputSystem* system, const std::string& id)
{
    for (size_t p = 0; p < system->profiles.size(); p++)
    {
        if (system->profiles[p].id == id)
        {
            return &system->profiles[p];
        }
    }
    return nullptr;
}

static void add_from_profile(InputSystem* system, const InputDevice* device, const InputProfile* profile)
{
    InputBindingSet bindings = {};
    input_default_bindings_for(device, &bindings);
    std::string name = device->name;
    int32_t deadzone = -1;
    if (profile != nullptr)
    {
        for (size_t o = 0; o < profile->overrides.size(); o++)
        {
            const InputBindingOverride* change = &profile->overrides[o];
            input_unbind_all(&bindings, change->action);
            for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
            {
                input_bind(&bindings, change->action, change->sources[s]);
            }
        }
        name = profile->name.empty() ? name : profile->name;
        deadzone = profile->deadzone_percent;
    }
    uint32_t index = input_add_controller(system, device->id, name.c_str(), &bindings);
    if (profile != nullptr)
    {
        system->controllers[index].hotkey_mode = profile->hotkey_mode;
    }
    if (deadzone >= 0 && deadzone < 100)
    {
        system->controllers[index].deadzone = static_cast<float>(deadzone) / 100.0f;
    }
}

void input_add_controllers_for_device(InputSystem* system, uint32_t device_id)
{
    const InputDevice* device = input_devices_find_const(&system->devices, device_id);
    if (device == nullptr || input_first_controller(system, device_id) >= 0)
    {
        return;
    }
    if (device->kind == InputDeviceKind::gamepad)
    {
        const InputProfile* profile = find_profile(system, device->guid);
        InputDevice* writable = input_devices_find(&system->devices, device_id);
        if (profile != nullptr && writable != nullptr)
        {
            writable->ignore = profile->ignore;
        }
        add_from_profile(system, device, profile);
        return;
    }
    // The keyboard: "keyboard", then "keyboard.2", "keyboard.3"... while they exist.
    add_from_profile(system, device, find_profile(system, "keyboard"));
    for (uint32_t part = 2; part < 64; part++)
    {
        const InputProfile* profile = find_profile(system, "keyboard." + std::to_string(part));
        if (profile == nullptr)
        {
            break;
        }
        add_from_profile(system, device, profile);
    }
}

std::string input_controller_profile_id(const InputSystem* system, uint32_t controller)
{
    if (controller >= system->controllers.size())
    {
        return "";
    }
    uint32_t device_id = system->controllers[controller].device_id;
    const InputDevice* device = input_devices_find_const(&system->devices, device_id);
    if (device == nullptr)
    {
        return "";
    }
    if (device->kind == InputDeviceKind::gamepad)
    {
        return device->guid;
    }
    uint32_t part = 1;
    for (uint32_t c = 0; c < controller; c++)
    {
        part += system->controllers[c].device_id == device_id ? 1u : 0u;
    }
    return part == 1 ? std::string("keyboard") : "keyboard." + std::to_string(part);
}

uint32_t input_add_controller(InputSystem* system, uint32_t device_id, const char* name, const InputBindingSet* bindings)
{
    InputController controller = {};
    controller.device_id = device_id;
    controller.name = name;
    controller.bindings = *bindings;
    system->controllers.push_back(controller);
    return static_cast<uint32_t>(system->controllers.size() - 1);
}

int32_t input_first_controller(const InputSystem* system, uint32_t device_id)
{
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        if (system->controllers[c].device_id == device_id)
        {
            return static_cast<int32_t>(c);
        }
    }
    return -1;
}

int32_t input_controller_port(const InputSystem* system, uint32_t controller)
{
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        if (system->ports[p] == static_cast<int32_t>(controller))
        {
            return static_cast<int32_t>(p);
        }
    }
    return INPUT_NO_PORT;
}

static bool is_keyboard(const InputSystem* system, int32_t controller)
{
    if (controller < 0 || static_cast<size_t>(controller) >= system->controllers.size())
    {
        return false;
    }
    const InputDevice* device = input_devices_find_const(&system->devices, system->controllers[static_cast<size_t>(controller)].device_id);
    return device != nullptr && device->kind == InputDeviceKind::keyboard;
}

static int32_t join_port(InputSystem* system, uint32_t controller, int32_t preferred)
{
    if (input_controller_port(system, controller) != INPUT_NO_PORT)
    {
        return input_controller_port(system, controller);
    }
    int32_t index = static_cast<int32_t>(controller);
    if (preferred >= 0 && preferred < static_cast<int32_t>(INPUT_PORT_COUNT) && system->ports[preferred] == INPUT_NO_PORT)
    {
        system->ports[preferred] = index;
        return preferred;
    }
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        if (system->ports[p] == INPUT_NO_PORT)
        {
            system->ports[p] = index;
            return static_cast<int32_t>(p);
        }
    }
    return INPUT_NO_PORT;
}

bool input_controller_shares_port1(const InputSystem* system, uint32_t controller)
{
    if (!system->settings.keyboard_shares_port1 || controller >= system->controllers.size())
    {
        return false;
    }
    return is_keyboard(system, static_cast<int32_t>(controller)) &&
           input_first_controller(system, system->controllers[controller].device_id) == static_cast<int32_t>(controller);
}

bool input_join(InputSystem* system, uint32_t controller)
{
    if (controller >= system->controllers.size())
    {
        return false;
    }
    return join_port(system, controller, 0) != INPUT_NO_PORT;
}

void input_leave(InputSystem* system, uint32_t controller)
{
    int32_t port = input_controller_port(system, controller);
    if (port != INPUT_NO_PORT)
    {
        system->ports[port] = INPUT_NO_PORT;
    }
}

void input_leave_all(InputSystem* system)
{
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        system->ports[p] = INPUT_NO_PORT;
    }
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        system->controllers[c].last_port = INPUT_NO_PORT;
    }
}

static float action_value(const InputController* controller, const InputDevice* device, InputAction action);

const InputCoreProfile* input_core_profile(const InputSystem* system, const char* core, uint32_t device,
                                           const std::string& profile_id)
{
    for (size_t p = 0; p < system->core_profiles.size(); p++)
    {
        const InputCoreProfile* profile = &system->core_profiles[p];
        if (profile->device == device && profile->core == core && profile->profile_id == profile_id)
        {
            return profile;
        }
    }
    return nullptr;
}

InputCoreProfile* input_core_profile_for(InputSystem* system, const char* core, uint32_t device, const std::string& profile_id)
{
    for (size_t p = 0; p < system->core_profiles.size(); p++)
    {
        InputCoreProfile* profile = &system->core_profiles[p];
        if (profile->device == device && profile->core == core && profile->profile_id == profile_id)
        {
            return profile;
        }
    }
    InputCoreProfile profile = {};
    profile.core = core;
    profile.device = device;
    profile.profile_id = profile_id;
    system->core_profiles.push_back(profile);
    return &system->core_profiles.back();
}

void input_effective_bindings(const InputSystem* system, uint32_t controller, uint32_t port, InputBindingSet* out)
{
    if (controller >= system->controllers.size())
    {
        *out = InputBindingSet{};
        return;
    }
    *out = system->controllers[controller].bindings;
    if (system->core_name.empty() || port >= INPUT_PORT_COUNT)
    {
        return;
    }
    const InputCoreProfile* profile = input_core_profile(system, system->core_name.c_str(), system->port_devices[port],
                                                         input_controller_profile_id(system, controller));
    if (profile == nullptr || !profile->custom)
    {
        return;
    }
    for (size_t o = 0; o < profile->overrides.size(); o++)
    {
        const InputBindingOverride* change = &profile->overrides[o];
        if (!input_action_is_pad(change->action))
        {
            continue;
        }
        input_unbind_all(out, change->action);
        for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
        {
            input_bind(out, change->action, change->sources[s]);
        }
    }
}

bool input_controller_action_held(const InputSystem* system, uint32_t controller, InputAction action)
{
    if (controller >= system->controllers.size())
    {
        return false;
    }
    const InputDevice* device = input_devices_find_const(&system->devices, system->controllers[controller].device_id);
    return device != nullptr && device->connected &&
           action_value(&system->controllers[controller], device, action) >= PRESSED_THRESHOLD;
}

void input_remove_controller(InputSystem* system, uint32_t controller)
{
    if (controller >= system->controllers.size())
    {
        return;
    }
    input_leave(system, controller);
    system->controllers.erase(system->controllers.begin() + static_cast<long>(controller));
    int32_t removed = static_cast<int32_t>(controller);
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        if (system->ports[p] > removed)
        {
            system->ports[p]--;
        }
    }
}

void input_capture_begin(InputSystem* system, uint32_t device_id)
{
    InputCapture* capture = &system->capture;
    *capture = InputCapture{};
    const InputDevice* device = input_devices_find_const(&system->devices, device_id);
    if (device == nullptr)
    {
        return;
    }
    capture->active = true;
    capture->device_id = device_id;
    for (uint32_t code = 0; code < INPUT_KEY_COUNT; code++)
    {
        capture->held_at_start[code] = device->held[code];
    }
    for (uint32_t a = 0; a < INPUT_AXIS_COUNT; a++)
    {
        capture->plus_at_start[a] = device->axes[a] > 0.5f;
        capture->minus_at_start[a] = device->axes[a] < -0.5f;
    }
}

bool input_capture_poll(InputSystem* system, InputSource* source)
{
    InputCapture* capture = &system->capture;
    const InputDevice* device = input_devices_find_const(&system->devices, capture->device_id);
    if (!capture->active || device == nullptr || !device->connected)
    {
        return false;
    }
    for (uint32_t code = 0; code < INPUT_KEY_COUNT; code++)
    {
        if (device->held[code] == 0 && device->tapped[code] == 0)
        {
            capture->held_at_start[code] = 0;   // let go: a new press of it counts
            continue;
        }
        if (capture->held_at_start[code] == 0)
        {
            source->type = device->kind == InputDeviceKind::keyboard ? InputSourceType::key : InputSourceType::button;
            source->code = static_cast<uint16_t>(code);
            capture->active = false;
            return true;
        }
    }
    for (uint32_t a = 0; a < INPUT_AXIS_COUNT; a++)
    {
        float value = device->axes[a];
        capture->plus_at_start[a] = capture->plus_at_start[a] && value > 0.5f;     // let go: counts again
        capture->minus_at_start[a] = capture->minus_at_start[a] && value < -0.5f;
        bool plus = value > 0.5f && !capture->plus_at_start[a];
        bool minus = value < -0.5f && !capture->minus_at_start[a];
        if (plus || minus)
        {
            source->type = plus ? InputSourceType::axis_plus : InputSourceType::axis_minus;
            source->code = static_cast<uint16_t>(a);
            capture->active = false;
            return true;
        }
    }
    return false;
}

void input_capture_cancel(InputSystem* system)
{
    system->capture.active = false;
}

void input_swap_ports(InputSystem* system, uint32_t a, uint32_t b)
{
    if (a < INPUT_PORT_COUNT && b < INPUT_PORT_COUNT)
    {
        int32_t controller = system->ports[a];
        system->ports[a] = system->ports[b];
        system->ports[b] = controller;
    }
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------

static float set_value(const InputBindingSet* set, const InputDevice* device, InputAction action)
{
    float value = 0.0f;
    const InputSource* slots = set->sources[index_of(action)];
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        float v = input_source_value(device, slots[s]);
        value = v > value ? v : value;
    }
    return value;
}

static float action_value(const InputController* controller, const InputDevice* device, InputAction action)
{
    float value = 0.0f;
    const InputSource* slots = controller->bindings.sources[index_of(action)];
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        float v = input_source_value(device, slots[s]);
        value = v > value ? v : value;
    }
    return value;
}

static int16_t to_axis(float value)
{
    float scaled = value * 32767.0f;
    scaled = scaled > 32767.0f ? 32767.0f : (scaled < -32767.0f ? -32767.0f : scaled);
    return static_cast<int16_t>(scaled);
}

// One stick of a port: plus minus minus per axis, then a round deadzone
// (the rest rescaled so movement starts at 0 past it).
static void stick(const InputController* controller, const InputBindingSet* set, const InputDevice* device,
                  InputAction x_plus, int16_t* out)
{
    uint32_t base = index_of(x_plus);
    float x = set_value(set, device, static_cast<InputAction>(base)) - set_value(set, device, static_cast<InputAction>(base + 1));
    float y = set_value(set, device, static_cast<InputAction>(base + 2)) - set_value(set, device, static_cast<InputAction>(base + 3));
    float length = std::sqrt(x * x + y * y);
    float deadzone = controller->deadzone;
    if (length <= deadzone || length <= 0.0f)
    {
        out[0] = 0;
        out[1] = 0;
        return;
    }
    float clamped = length > 1.0f ? 1.0f : length;
    float scale = (clamped - deadzone) / (1.0f - deadzone) / length;
    out[0] = to_axis(x * scale);
    out[1] = to_axis(y * scale);
}

// The modifier, by the controller's mode: is the HOTKEY layer on?
static bool modifier_active(InputController* controller, const bool* held, const bool* pressed, uint64_t now_ms)
{
    uint32_t modifier = index_of(InputAction::hotkey_enable);
    if (pressed[modifier])
    {
        controller->modifier_down_ms = now_ms;
        controller->modifier_used = false;
    }
    if (controller->hotkey_mode == InputHotkeyMode::hold)
    {
        if (!held[modifier])
        {
            return false;
        }
        // At once with a hotkey; otherwise after a short delay, so a quick
        // tap of the modifier (SELECT on a pad) still reaches the game.
        bool hotkey_pressed = false;
        for (uint32_t a = index_of(InputAction::hotkey_enable) + 1; a < INPUT_ACTION_COUNT; a++)
        {
            hotkey_pressed = hotkey_pressed || pressed[a];
        }
        return hotkey_pressed || controller->layer == InputLayer::hotkey ||
               now_ms - controller->modifier_down_ms >= HOTKEY_BLOCK_DELAY_MS;
    }
    // ONE HOTKEY / STAYS ON: a double tap of the modifier.
    if (pressed[modifier])
    {
        if (controller->modifier_tap_ms != 0 && now_ms - controller->modifier_tap_ms <= DOUBLE_TAP_MS)
        {
            controller->modifier_on = controller->hotkey_mode == InputHotkeyMode::stays_on ? !controller->modifier_on : true;
            controller->modifier_on_ms = now_ms;
            controller->modifier_tap_ms = 0;
        }
        else
        {
            controller->modifier_tap_ms = now_ms;
        }
    }
    if (controller->hotkey_mode == InputHotkeyMode::one_hotkey && controller->modifier_on &&
        now_ms - controller->modifier_on_ms > ARMED_TIMEOUT_MS)
    {
        controller->modifier_on = false;
    }
    return controller->modifier_on;
}

void input_update(InputSystem* system, uint64_t now_ms, InputFrame* frame)
{
    *frame = InputFrame{};

    // Ports of devices that went away; back when they return, if still free.
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        InputController* controller = &system->controllers[c];
        const InputDevice* device = input_devices_find_const(&system->devices, controller->device_id);
        int32_t port = input_controller_port(system, static_cast<uint32_t>(c));
        bool connected = device != nullptr && device->connected;
        if (!connected && port != INPUT_NO_PORT)
        {
            controller->last_port = port;
            system->ports[port] = INPUT_NO_PORT;
            frame->left_port = port;
            frame->left_controller = static_cast<int32_t>(c);
        }
        else if (connected && port == INPUT_NO_PORT && controller->last_port != INPUT_NO_PORT)
        {
            if (system->ports[controller->last_port] == INPUT_NO_PORT)
            {
                system->ports[controller->last_port] = static_cast<int32_t>(c);
            }
            controller->last_port = INPUT_NO_PORT;
        }
    }

    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        InputController* controller = &system->controllers[c];
        const InputDevice* device = input_devices_find_const(&system->devices, controller->device_id);
        if (device == nullptr || !device->connected || input_devices_ignored(device))
        {
            controller->layer = InputLayer::none;
            continue;
        }
        uint32_t index = static_cast<uint32_t>(c);

        bool held[INPUT_ACTION_COUNT] = {};
        bool pressed[INPUT_ACTION_COUNT] = {};
        for (uint32_t a = 0; a < INPUT_ACTION_COUNT; a++)
        {
            held[a] = action_value(controller, device, static_cast<InputAction>(a)) >= PRESSED_THRESHOLD;
            pressed[a] = held[a] && !controller->held_last[a];
            controller->held_last[a] = held[a];
        }

        // Binding by pressing, or mapping: no layer at all.
        if (system->capture.active || system->suspended)
        {
            controller->layer = InputLayer::none;
            continue;
        }

        // Joining (any time: in the UI or in a game).
        bool keyboard = device->kind == InputDeviceKind::keyboard;
        bool shared = input_controller_shares_port1(system, index);
        if (shared && input_controller_port(system, index) != INPUT_NO_PORT)
        {
            input_leave(system, index);   // it plays on port 1 anyway
        }
        int32_t port = input_controller_port(system, index);
        if (!shared && port == INPUT_NO_PORT && pressed[index_of(InputAction::pad_start)])
        {
            port = join_port(system, index, INPUT_NO_PORT);
            if (port != INPUT_NO_PORT)
            {
                frame->joined_port = port;
                frame->joined_controller = static_cast<int32_t>(index);
            }
        }
        bool joined = shared || port != INPUT_NO_PORT;

        // The layer: HOTKEY > INTERFACE > GAME.
        bool hotkeys = (joined || keyboard) && modifier_active(controller, held, pressed, now_ms);
        bool drives_ui = keyboard || port == 0;
        InputLayer layer = InputLayer::none;
        if (hotkeys)
        {
            layer = InputLayer::hotkey;
        }
        else if (system->settings.ui_active)
        {
            layer = drives_ui ? InputLayer::interface : InputLayer::none;
        }
        else if (joined)
        {
            layer = InputLayer::game;
        }
        if (layer != controller->layer)
        {
            // What's held as it enters a layer stays out of it until let go:
            // all of it coming from another layer, only the START that joined
            // when it comes in from nothing. (Into HOTKEY: nothing; the press
            // that turned it on is the hotkey.)
            bool from_layer = controller->layer != InputLayer::none;
            bool joined_now = frame->joined_controller == static_cast<int32_t>(index);
            for (uint32_t a = 0; a < INPUT_ACTION_COUNT; a++)
            {
                bool hold = layer != InputLayer::hotkey && held[a] &&
                            (from_layer || (joined_now && a == index_of(InputAction::pad_start)));
                controller->held_at_switch[a] = hold;
            }
            controller->layer = layer;
        }
        for (uint32_t a = 0; a < INPUT_ACTION_COUNT; a++)
        {
            controller->held_at_switch[a] = controller->held_at_switch[a] && held[a];
            if (controller->held_at_switch[a])
            {
                held[a] = false;
                pressed[a] = false;
            }
        }
        uint32_t modifier = index_of(InputAction::hotkey_enable);

        switch (layer)
        {
        case InputLayer::hotkey:
        {
            bool fired = false;
            for (uint32_t a = modifier + 1; a < INPUT_ACTION_COUNT; a++)
            {
                frame->held[a] = frame->held[a] || held[a];
                frame->pressed[a] = frame->pressed[a] || pressed[a];
                frame->keyboard_pressed[a] = frame->keyboard_pressed[a] || (keyboard && pressed[a]);
                fired = fired || pressed[a];
            }
            if (fired)
            {
                controller->modifier_used = true;
                if (controller->hotkey_mode == InputHotkeyMode::one_hotkey)
                {
                    controller->modifier_on = false;   // one hotkey, then back
                }
            }
            break;
        }
        case InputLayer::interface:
            for (uint32_t a = index_of(InputAction::ui_up); a <= index_of(InputAction::ui_open_menu); a++)
            {
                frame->held[a] = frame->held[a] || held[a];
                frame->pressed[a] = frame->pressed[a] || pressed[a];
                frame->keyboard_pressed[a] = frame->keyboard_pressed[a] || (keyboard && pressed[a]);
                if (!keyboard)
                {
                    frame->gamepad_held[a] = frame->gamepad_held[a] || held[a];
                }
            }
            break;
        case InputLayer::game:
        {
            // The port's RetroPad: the controller's bindings for the running
            // core and the port's device (its own, unless a custom core profile).
            int32_t play_port = shared ? 0 : port;
            InputBindingSet effective = {};
            input_effective_bindings(system, index, static_cast<uint32_t>(play_port), &effective);
            uint16_t buttons = 0;
            for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
            {
                if (set_value(&effective, device, static_cast<InputAction>(b)) >= PRESSED_THRESHOLD)
                {
                    buttons = static_cast<uint16_t>(buttons | (1u << b));
                }
            }
            for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
            {
                if (controller->held_at_switch[b])
                {
                    buttons = static_cast<uint16_t>(buttons & ~(1u << b));   // still held from another layer
                }
            }
            frame->pad_buttons[play_port] = static_cast<uint16_t>(frame->pad_buttons[play_port] | buttons);
            for (int s = 0; s < 2; s++)
            {
                uint32_t first = index_of(s == 0 ? InputAction::pad_left_x_plus : InputAction::pad_right_x_plus);
                bool stuck = controller->held_at_switch[first] || controller->held_at_switch[first + 1] ||
                             controller->held_at_switch[first + 2] || controller->held_at_switch[first + 3];
                if (!stuck)
                {
                    stick(controller, &effective, device, static_cast<InputAction>(first), frame->analog[play_port][s]);
                }
            }
            break;
        }
        case InputLayer::none:
            break;
        }

        // SELECT held by itself on port 1's gamepad (the UI may use it: hold to go back).
        if (!keyboard && port == 0 && controller->held_last[modifier] && !controller->modifier_used)
        {
            frame->gamepad_enable_alone = true;
        }
    }
}

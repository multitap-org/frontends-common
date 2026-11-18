#include "input_bindings.h"

#include <cmath>
#include <cstdlib>
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

// The sources in order, none after the last.
static void pack_sources(InputSource* sources)
{
    uint32_t kept = 0;
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        if (sources[s].type != InputSourceType::none)
        {
            sources[kept++] = sources[s];
        }
    }
    for (uint32_t s = kept; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        sources[s] = InputSource{};
    }
}

uint32_t input_sources_count(const InputSource* sources)
{
    uint32_t count = 0;
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        count += sources[s].type != InputSourceType::none ? 1u : 0u;
    }
    return count;
}

void input_sources_remove(InputSource* sources, uint32_t row)
{
    pack_sources(sources);
    if (row < INPUT_SOURCES_PER_ACTION)
    {
        sources[row] = InputSource{};
    }
    pack_sources(sources);
}

void input_sources_set(InputSource* sources, uint32_t row, InputSource source)
{
    pack_sources(sources);
    if (source.type == InputSourceType::none)
    {
        input_sources_remove(sources, row);
        return;
    }
    // The same control elsewhere moves to this row.
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        if (s != row && sources[s].type != InputSourceType::none && input_source_equal(sources[s], source))
        {
            sources[s] = InputSource{};
            if (s < row)
            {
                row--;
            }
        }
    }
    pack_sources(sources);
    uint32_t count = input_sources_count(sources);
    if (row > count)
    {
        row = count;
    }
    if (row < INPUT_SOURCES_PER_ACTION)
    {
        sources[row] = source;
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

    // The interface moves with the D-pad only (the stick is added by hand: +).
    input_bind(set, InputAction::ui_up, pad_button(InputButton::dpad_up));
    input_bind(set, InputAction::ui_down, pad_button(InputButton::dpad_down));
    input_bind(set, InputAction::ui_left, pad_button(InputButton::dpad_left));
    input_bind(set, InputAction::ui_right, pad_button(InputButton::dpad_right));
    // The bottom button accepts, the right one goes back (the frontend may
    // swap them: REVERSE ACCEPT / BACK).
    input_bind(set, InputAction::ui_accept, pad_button(InputButton::south));
    input_bind(set, InputAction::ui_back, pad_button(InputButton::east));
    input_bind(set, InputAction::ui_page_left, pad_button(InputButton::left_shoulder));
    input_bind(set, InputAction::ui_page_right, pad_button(InputButton::right_shoulder));
    // The main menu: the left face button (RetroPad Y) in the interface, and
    // HOTKEY ENABLE + the same button in a game (MAP CONTROLLER sets both).
    input_bind(set, InputAction::ui_open_menu, pad_button(InputButton::west));

    input_bind(set, InputAction::hotkey_enable, pad_button(InputButton::back));
    input_bind(set, InputAction::hotkey_quick_menu, pad_button(InputButton::start));
    input_bind(set, InputAction::hotkey_menu, pad_button(InputButton::west));
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
    // Its d-pad comes as the left stick: the RetroPad's and the interface's
    // directions there.
    static const InputAction DIRECTIONS[8] = {
        InputAction::pad_up, InputAction::pad_down, InputAction::pad_left, InputAction::pad_right,
        InputAction::ui_up,  InputAction::ui_down,  InputAction::ui_left,  InputAction::ui_right,
    };
    for (uint32_t d = 0; d < 8; d++)
    {
        input_unbind_all(set, DIRECTIONS[d]);
        uint32_t way = d % 4;   // up, down, left, right
        input_bind(set, DIRECTIONS[d], pad_axis(way < 2 ? InputAxis::left_y : InputAxis::left_x, way == 1 || way == 3));
    }
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
        if (input_port_has(system, p, controller))
        {
            return static_cast<int32_t>(p);
        }
    }
    return INPUT_NO_PORT;
}

bool input_port_has(const InputSystem* system, uint32_t port, uint32_t controller)
{
    if (port >= INPUT_PORT_COUNT)
    {
        return false;
    }
    const std::vector<uint32_t>* members = &system->ports[port];
    for (size_t m = 0; m < members->size(); m++)
    {
        if ((*members)[m] == controller)
        {
            return true;
        }
    }
    return false;
}

void input_port_add(InputSystem* system, uint32_t port, uint32_t controller)
{
    if (port < INPUT_PORT_COUNT && controller < system->controllers.size() && !input_port_has(system, port, controller))
    {
        system->ports[port].push_back(controller);
    }
}

void input_port_remove(InputSystem* system, uint32_t port, uint32_t controller)
{
    if (port >= INPUT_PORT_COUNT)
    {
        return;
    }
    std::vector<uint32_t>* members = &system->ports[port];
    for (size_t m = 0; m < members->size(); m++)
    {
        if ((*members)[m] == controller)
        {
            members->erase(members->begin() + static_cast<long>(m));
            return;
        }
    }
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

// The keyboard's first player (the one keyboard_shares_port1 puts on port 1), or -1.
static int32_t keyboard_player(const InputSystem* system)
{
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        if (is_keyboard(system, static_cast<int32_t>(c)))
        {
            return static_cast<int32_t>(c);
        }
    }
    return -1;
}

// Free for START: no members but the keyboard put there to share it.
static bool port_free(const InputSystem* system, uint32_t port)
{
    const std::vector<uint32_t>* members = &system->ports[port];
    int32_t keyboard = system->keyboard_seated ? keyboard_player(system) : -1;
    for (size_t m = 0; m < members->size(); m++)
    {
        if (static_cast<int32_t>((*members)[m]) != keyboard)
        {
            return false;
        }
    }
    return true;
}

// START: the first free port, if it is on none. INPUT_NO_PORT if none was free.
static int32_t join_port(InputSystem* system, uint32_t controller)
{
    if (input_controller_port(system, controller) != INPUT_NO_PORT)
    {
        return INPUT_NO_PORT;
    }
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        if (port_free(system, p))
        {
            system->ports[p].push_back(controller);
            return static_cast<int32_t>(p);
        }
    }
    return INPUT_NO_PORT;
}

bool input_join(InputSystem* system, uint32_t controller)
{
    if (controller >= system->controllers.size())
    {
        return false;
    }
    return join_port(system, controller) != INPUT_NO_PORT;
}

void input_leave(InputSystem* system, uint32_t controller)
{
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        input_port_remove(system, p, controller);
    }
}

void input_leave_all(InputSystem* system)
{
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        system->ports[p].clear();
    }
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
        {
            system->controllers[c].last_ports[p] = false;
        }
    }
    system->keyboard_seated = false;   // back on port 1 on the next update, if it shares it
}

// The same, from only the axes (`analog`) or only the keys and buttons.
static float set_value_of(const InputBindingSet* set, const InputDevice* device, InputAction action, bool analog)
{
    float value = 0.0f;
    const InputSource* slots = set->sources[index_of(action)];
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        bool axis = slots[s].type == InputSourceType::axis_plus || slots[s].type == InputSourceType::axis_minus;
        if (axis != analog)
        {
            continue;
        }
        float v = input_source_value(device, slots[s]);
        value = v > value ? v : value;
    }
    return value;
}

static float action_value(const InputController* controller, const InputDevice* device, InputAction action);

const InputCoreProfile* input_core_profile(const InputSystem* system, const char* core, const char* game, uint32_t device,
                                           const std::string& profile_id)
{
    for (size_t p = 0; p < system->core_profiles.size(); p++)
    {
        const InputCoreProfile* profile = &system->core_profiles[p];
        if (profile->device == device && profile->core == core && profile->game == game && profile->profile_id == profile_id)
        {
            return profile;
        }
    }
    return nullptr;
}

InputCoreProfile* input_core_profile_for(InputSystem* system, const char* core, const char* game, uint32_t device,
                                         const std::string& profile_id)
{
    for (size_t p = 0; p < system->core_profiles.size(); p++)
    {
        InputCoreProfile* profile = &system->core_profiles[p];
        if (profile->device == device && profile->core == core && profile->game == game && profile->profile_id == profile_id)
        {
            return profile;
        }
    }
    InputCoreProfile profile = {};
    profile.core = core;
    profile.game = game;
    profile.device = device;
    profile.profile_id = profile_id;
    system->core_profiles.push_back(profile);
    return &system->core_profiles.back();
}

const InputCoreProfile* input_active_core_profile(const InputSystem* system, uint32_t controller, uint32_t port)
{
    if (system->core_name.empty() || port >= INPUT_PORT_COUNT || controller >= system->controllers.size())
    {
        return nullptr;
    }
    std::string id = input_controller_profile_id(system, controller);
    const InputCoreProfile* profile = nullptr;
    if (!system->game_name.empty())
    {
        profile = input_core_profile(system, system->core_name.c_str(), system->game_name.c_str(), system->port_devices[port], id);
    }
    if (profile == nullptr)
    {
        profile = input_core_profile(system, system->core_name.c_str(), "", system->port_devices[port], id);
    }
    return profile;
}

void input_core_profile_remove(InputSystem* system, const InputCoreProfile* profile)
{
    for (size_t p = 0; p < system->core_profiles.size(); p++)
    {
        if (&system->core_profiles[p] == profile)
        {
            system->core_profiles.erase(system->core_profiles.begin() + static_cast<long>(p));
            return;
        }
    }
}

const InputCombo* input_combo(const InputSystem* system, const char* core, const std::string& id)
{
    for (size_t c = 0; c < system->combos.size(); c++)
    {
        if (system->combos[c].core == core && system->combos[c].id == id)
        {
            return &system->combos[c];
        }
    }
    return nullptr;
}

InputCombo* input_combo_add(InputSystem* system, const char* core, const std::string& name, uint16_t buttons)
{
    // The next free "c<n>" of the core.
    unsigned long next = 1;
    for (size_t c = 0; c < system->combos.size(); c++)
    {
        const InputCombo* combo = &system->combos[c];
        if (combo->core == core && combo->id.size() > 1 && combo->id[0] == 'c')
        {
            unsigned long number = std::strtoul(combo->id.c_str() + 1, nullptr, 10);
            next = number >= next ? number + 1 : next;
        }
    }
    InputCombo combo = {};
    combo.core = core;
    combo.id = "c" + std::to_string(next);
    combo.name = name;
    combo.buttons = buttons;
    system->combos.push_back(combo);
    return &system->combos.back();
}

// A combo's or macro's bindings out of every remap of its core.
static void unbind_everywhere(InputSystem* system, const char* core, const std::string& id)
{
    for (size_t p = 0; p < system->core_profiles.size(); p++)
    {
        InputCoreProfile* profile = &system->core_profiles[p];
        if (profile->core != core)
        {
            continue;
        }
        for (size_t b = profile->combos.size(); b > 0; b--)
        {
            if (profile->combos[b - 1].combo == id)
            {
                profile->combos.erase(profile->combos.begin() + static_cast<long>(b - 1));
            }
        }
    }
}

void input_combo_remove(InputSystem* system, const char* core, const std::string& id)
{
    for (size_t c = 0; c < system->combos.size(); c++)
    {
        if (system->combos[c].core == core && system->combos[c].id == id)
        {
            system->combos.erase(system->combos.begin() + static_cast<long>(c));
            break;
        }
    }
    unbind_everywhere(system, core, id);
}

const InputMacro* input_macro(const InputSystem* system, const char* core, const std::string& id)
{
    for (size_t m = 0; m < system->macros.size(); m++)
    {
        if (system->macros[m].core == core && system->macros[m].id == id)
        {
            return &system->macros[m];
        }
    }
    return nullptr;
}

InputMacro* input_macro_add(InputSystem* system, const char* core, const std::string& name)
{
    // The next free "m<n>" of the core.
    unsigned long next = 1;
    for (size_t m = 0; m < system->macros.size(); m++)
    {
        const InputMacro* macro = &system->macros[m];
        if (macro->core == core && macro->id.size() > 1 && macro->id[0] == 'm')
        {
            unsigned long number = std::strtoul(macro->id.c_str() + 1, nullptr, 10);
            next = number >= next ? number + 1 : next;
        }
    }
    InputMacro macro = {};
    macro.core = core;
    macro.id = "m" + std::to_string(next);
    macro.name = name;
    system->macros.push_back(macro);
    return &system->macros.back();
}

void input_macro_remove(InputSystem* system, const char* core, const std::string& id)
{
    for (size_t m = 0; m < system->macros.size(); m++)
    {
        if (system->macros[m].core == core && system->macros[m].id == id)
        {
            system->macros.erase(system->macros.begin() + static_cast<long>(m));
            break;
        }
    }
    unbind_everywhere(system, core, id);
}

uint32_t input_macro_frames(const InputMacro* macro)
{
    uint32_t total = 0;
    for (size_t s = 0; s < macro->steps.size(); s++)
    {
        total += macro->steps[s].frames;
    }
    return total;
}

void input_effective_bindings(const InputSystem* system, uint32_t controller, uint32_t port, InputBindingSet* out)
{
    if (controller >= system->controllers.size())
    {
        *out = InputBindingSet{};
        return;
    }
    *out = system->controllers[controller].bindings;
    const InputCoreProfile* profile = input_active_core_profile(system, controller, port);
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
    for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
    {
        for (size_t m = 0; m < system->ports[p].size(); m++)
        {
            if (system->ports[p][m] > controller)
            {
                system->ports[p][m]--;
            }
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

// The first control pressed since the capture started (nothing taken yet).
static bool capture_pressed(InputCapture* capture, const InputDevice* device, InputSource* source)
{
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
            return true;
        }
    }
    return false;
}

bool input_capture_poll(InputSystem* system, uint64_t now_ms, InputSource* source)
{
    InputCapture* capture = &system->capture;
    const InputDevice* device = input_devices_find_const(&system->devices, capture->device_id);
    if (!capture->active || device == nullptr || !device->connected)
    {
        return false;
    }
    InputSource found = {};
    if (!capture_pressed(capture, device, &found))
    {
        capture->candidate = InputSource{};   // let go before it was taken
        return false;
    }
    if (device->kind != InputDeviceKind::keyboard)
    {
        if (capture->candidate.type == InputSourceType::none || !input_source_equal(capture->candidate, found))
        {
            capture->candidate = found;
            capture->candidate_since_ms = now_ms;
            return false;
        }
        if (now_ms - capture->candidate_since_ms < system->settings.hold_ms)
        {
            return false;
        }
    }
    *source = found;
    capture->candidate = InputSource{};
    capture->active = false;
    return true;
}

bool input_capture_candidate(const InputSystem* system, InputSource* source)
{
    const InputCapture* capture = &system->capture;
    if (!capture->active || capture->candidate.type == InputSourceType::none)
    {
        return false;
    }
    *source = capture->candidate;
    return true;
}

float input_capture_hold_progress(const InputSystem* system, uint64_t now_ms)
{
    const InputCapture* capture = &system->capture;
    if (!capture->active || capture->candidate.type == InputSourceType::none)
    {
        return 0.0f;
    }
    uint64_t held = now_ms - capture->candidate_since_ms;
    uint64_t needed = system->settings.hold_ms > 0 ? system->settings.hold_ms : 1;
    return held >= needed ? 1.0f : static_cast<float>(held) / static_cast<float>(needed);
}

void input_capture_cancel(InputSystem* system)
{
    system->capture.active = false;
}

void input_swap_ports(InputSystem* system, uint32_t a, uint32_t b)
{
    if (a < INPUT_PORT_COUNT && b < INPUT_PORT_COUNT)
    {
        system->ports[a].swap(system->ports[b]);
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
// A RetroPad stick from its four directions. The deadzone is the real
// stick's: it applies to the axes bound to it, on their own; keys and buttons
// (at their percent) are added after it, untouched. (Applied to the sum, a
// D-pad press would push the vector past the deadzone and let the resting
// stick's drift through on the other axis.)
static void stick(const InputController* controller, const InputBindingSet* set, const InputDevice* device,
                  InputAction x_plus, int16_t* out)
{
    uint32_t base = index_of(x_plus);
    InputAction x_minus = static_cast<InputAction>(base + 1);
    InputAction y_plus = static_cast<InputAction>(base + 2);
    InputAction y_minus = static_cast<InputAction>(base + 3);
    float x = set_value_of(set, device, x_plus, true) - set_value_of(set, device, x_minus, true);
    float y = set_value_of(set, device, y_plus, true) - set_value_of(set, device, y_minus, true);
    float length = std::sqrt(x * x + y * y);
    float deadzone = controller->deadzone;
    if (length <= deadzone || length <= 0.0f)
    {
        x = 0.0f;
        y = 0.0f;
    }
    else
    {
        float clamped = length > 1.0f ? 1.0f : length;
        float scale = (clamped - deadzone) / (1.0f - deadzone) / length;
        x *= scale;
        y *= scale;
    }
    x += set_value_of(set, device, x_plus, false) - set_value_of(set, device, x_minus, false);
    y += set_value_of(set, device, y_plus, false) - set_value_of(set, device, y_minus, false);
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    y = y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y);
    out[0] = to_axis(x);
    out[1] = to_axis(y);
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

// One frame of a macro on a port: starts when its input goes down, plays
// to the end (and again while held, if it repeats). Its buttons this frame.
static uint16_t macro_step(InputController* controller, uint32_t port, const InputMacro* macro, bool down)
{
    InputMacroPlay* play = nullptr;
    for (size_t p = 0; p < controller->macro_plays.size(); p++)
    {
        if (controller->macro_plays[p].port == port && controller->macro_plays[p].macro == macro->id)
        {
            play = &controller->macro_plays[p];
        }
    }
    if (play == nullptr)
    {
        InputMacroPlay fresh = {};
        fresh.port = port;
        fresh.macro = macro->id;
        controller->macro_plays.push_back(fresh);
        play = &controller->macro_plays.back();
    }
    bool pressed = down && !play->held_last;
    play->held_last = down;
    uint32_t total = input_macro_frames(macro);
    if (!play->playing && pressed && total > 0)
    {
        play->playing = true;
        play->frame = 0;
    }
    if (!play->playing)
    {
        return 0;
    }
    if (play->frame >= total)
    {
        if (macro->repeat && down)
        {
            play->frame = 0;   // again, while held
        }
        else
        {
            play->playing = false;
            return 0;
        }
    }
    uint32_t at = 0;
    uint16_t buttons = 0;
    for (size_t s = 0; s < macro->steps.size(); s++)
    {
        if (play->frame < at + macro->steps[s].frames)
        {
            buttons = macro->steps[s].buttons;
            break;
        }
        at += macro->steps[s].frames;
    }
    play->frame++;
    return buttons;
}

uint16_t input_controller_pad_buttons(const InputSystem* system, uint32_t controller, uint32_t port)
{
    if (controller >= system->controllers.size())
    {
        return 0;
    }
    const InputDevice* device = input_devices_find_const(&system->devices, system->controllers[controller].device_id);
    if (device == nullptr || !device->connected)
    {
        return 0;
    }
    InputBindingSet effective = {};
    input_effective_bindings(system, controller, port, &effective);
    uint16_t buttons = 0;
    for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
    {
        if (set_value(&effective, device, static_cast<InputAction>(b)) >= PRESSED_THRESHOLD)
        {
            buttons = static_cast<uint16_t>(buttons | (1u << b));
        }
    }
    return buttons;
}

void input_update(InputSystem* system, uint64_t now_ms, InputFrame* frame)
{
    *frame = InputFrame{};

    // The keyboard on port 1 when it shares it (once it is there to put),
    // off it when that's turned off.
    int32_t keyboard_shared = keyboard_player(system);
    if (system->settings.keyboard_shares_port1 && !system->keyboard_seated && keyboard_shared >= 0)
    {
        input_port_add(system, 0, static_cast<uint32_t>(keyboard_shared));
        system->keyboard_seated = true;
    }
    else if (!system->settings.keyboard_shares_port1 && system->keyboard_seated)
    {
        if (keyboard_shared >= 0)
        {
            input_port_remove(system, 0, static_cast<uint32_t>(keyboard_shared));
        }
        system->keyboard_seated = false;
    }

    // Devices that went away leave their ports; back on all of them when they return.
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        InputController* controller = &system->controllers[c];
        uint32_t index = static_cast<uint32_t>(c);
        const InputDevice* device = input_devices_find_const(&system->devices, controller->device_id);
        int32_t port = input_controller_port(system, index);
        bool connected = device != nullptr && device->connected;
        if (!connected && port != INPUT_NO_PORT)
        {
            for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
            {
                controller->last_ports[p] = input_port_has(system, p, index);
            }
            input_leave(system, index);
            frame->left_port = port;
            frame->left_controller = static_cast<int32_t>(c);
        }
        else if (connected && port == INPUT_NO_PORT)
        {
            for (uint32_t p = 0; p < INPUT_PORT_COUNT; p++)
            {
                if (controller->last_ports[p])
                {
                    input_port_add(system, p, index);
                }
                controller->last_ports[p] = false;
            }
        }
    }
    // Sticks so far, per port: the member pushing furthest wins.
    int32_t stick_reach[INPUT_PORT_COUNT][2] = {};

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
            controller->paused = true;
            continue;
        }

        // Joining (any time: in the UI or in a game).
        bool keyboard = device->kind == InputDeviceKind::keyboard;
        int32_t port = input_controller_port(system, index);
        if (port == INPUT_NO_PORT && pressed[index_of(InputAction::pad_start)])
        {
            port = join_port(system, index);
            if (port != INPUT_NO_PORT)
            {
                frame->joined_port = port;
                frame->joined_controller = static_cast<int32_t>(index);
            }
        }
        bool joined = port != INPUT_NO_PORT;
        bool on_port1 = input_port_has(system, 0, index);

        // The layer: HOTKEY > INTERFACE > GAME.
        bool hotkeys = (joined || keyboard) && modifier_active(controller, held, pressed, now_ms);
        bool drives_ui = keyboard || on_port1;
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
            // (After binding or mapping, as from a layer: the control just
            // bound may still be held, and mustn't act, e.g. close the window.)
            bool from_layer = controller->layer != InputLayer::none || controller->paused;
            controller->paused = false;
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
            // The RetroPad of each port it is on: the controller's bindings for
            // the running core and the port's device (its own, unless a custom
            // core profile), together with the port's other members.
            for (uint32_t play_port = 0; play_port < INPUT_PORT_COUNT; play_port++)
            {
                if (!input_port_has(system, play_port, index))
                {
                    continue;
                }
                InputBindingSet effective = {};
                input_effective_bindings(system, index, play_port, &effective);
                uint16_t buttons = 0;
                for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
                {
                    if (set_value(&effective, device, static_cast<InputAction>(b)) >= PRESSED_THRESHOLD &&
                        !controller->held_at_switch[b])   // (not: still held from another layer)
                    {
                        buttons = static_cast<uint16_t>(buttons | (1u << b));
                    }
                }
                // Its combos (any of a combo's inputs presses all its buttons)
                // and macros (an input plays its steps, a frame per update).
                const InputCoreProfile* profile = input_active_core_profile(system, index, play_port);
                for (size_t k = 0; profile != nullptr && k < profile->combos.size(); k++)
                {
                    const InputComboBinding* binding = &profile->combos[k];
                    const InputCombo* combo = input_combo(system, system->core_name.c_str(), binding->combo);
                    const InputMacro* macro = input_macro(system, system->core_name.c_str(), binding->combo);
                    float value = 0.0f;
                    for (uint32_t s = 0; (combo != nullptr || macro != nullptr) && s < INPUT_SOURCES_PER_ACTION; s++)
                    {
                        float v = input_source_value(device, binding->sources[s]);
                        value = v > value ? v : value;
                    }
                    bool down = value >= PRESSED_THRESHOLD;
                    uint16_t adds = 0;
                    if (combo != nullptr && down)
                    {
                        adds = combo->buttons;
                    }
                    if (macro != nullptr)
                    {
                        adds = macro_step(controller, play_port, macro, down);
                    }
                    for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
                    {
                        if ((adds & (1u << b)) != 0 && !controller->held_at_switch[b])
                        {
                            buttons = static_cast<uint16_t>(buttons | (1u << b));
                        }
                    }
                }
                frame->pad_buttons[play_port] = static_cast<uint16_t>(frame->pad_buttons[play_port] | buttons);
                for (int s = 0; s < 2; s++)
                {
                    uint32_t first = index_of(s == 0 ? InputAction::pad_left_x_plus : InputAction::pad_right_x_plus);
                    bool stuck = controller->held_at_switch[first] || controller->held_at_switch[first + 1] ||
                                 controller->held_at_switch[first + 2] || controller->held_at_switch[first + 3];
                    if (stuck)
                    {
                        continue;
                    }
                    int16_t axes[2] = {};
                    stick(controller, &effective, device, static_cast<InputAction>(first), axes);
                    int32_t reach = static_cast<int32_t>(axes[0]) * axes[0] / 2 + static_cast<int32_t>(axes[1]) * axes[1] / 2;
                    if (reach > stick_reach[play_port][s])
                    {
                        stick_reach[play_port][s] = reach;
                        frame->analog[play_port][s][0] = axes[0];
                        frame->analog[play_port][s][1] = axes[1];
                    }
                }
            }
            break;
        }
        case InputLayer::none:
            break;
        }

        // SELECT held by itself on port 1's gamepad (the UI may use it: hold to go back).
        if (!keyboard && on_port1 && controller->held_last[modifier] && !controller->modifier_used)
        {
            frame->gamepad_enable_alone = true;
        }
    }
}

#include "input_bindings.h"

#include <cmath>
#include <cstring>

// A digital action counts as pressed past this (keys and buttons are 0 or 1).
constexpr float PRESSED_THRESHOLD = 0.5f;
// HOLD: how long HOTKEY ENABLE must be held before the RetroPad goes quiet.
constexpr uint64_t HOTKEY_BLOCK_DELAY_MS = 80;
// DOUBLE TAP: two presses this close arm it; armed for this long at most.
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
constexpr uint16_t HID_RETURN = 40, HID_ESCAPE = 41, HID_BACKSPACE = 42, HID_SPACE = 44;
constexpr uint16_t HID_F1 = 58, HID_F2 = 59, HID_F4 = 61;
constexpr uint16_t HID_PAGE_UP = 75, HID_PAGE_DOWN = 78;
constexpr uint16_t HID_RIGHT = 79, HID_LEFT = 80, HID_DOWN = 81, HID_UP = 82;
constexpr uint16_t HID_RIGHT_SHIFT = 229;

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
    input_bind(set, InputAction::ui_back, key(HID_BACKSPACE));
    input_bind(set, InputAction::ui_page_left, key(HID_PAGE_UP));
    input_bind(set, InputAction::ui_page_right, key(HID_PAGE_DOWN));

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
    bool gamepad = !is_keyboard(system, index);
    if (gamepad && system->settings.gamepad_takes_keyboard_port && is_keyboard(system, system->ports[0]))
    {
        system->ports[0] = index;   // the keyboard leaves port 1
        return 0;
    }
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

// Is any of this hotkey's sources usable without HOTKEY ENABLE? (Keyboard
// keys not bound to the RetroPad, while the core doesn't take the keyboard.)
static bool hotkey_free(const InputSystem* system, const InputController* controller, const InputDevice* device,
                        InputAction action)
{
    if (device->kind != InputDeviceKind::keyboard || system->settings.core_takes_keyboard)
    {
        return false;
    }
    const InputSource* slots = controller->bindings.sources[index_of(action)];
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        if (slots[s].type == InputSourceType::none || input_source_value(device, slots[s]) < PRESSED_THRESHOLD)
        {
            continue;
        }
        bool on_pad = false;
        for (uint32_t a = 0; a <= index_of(InputAction::pad_right_y_minus) && !on_pad; a++)
        {
            on_pad = input_is_bound(&controller->bindings, static_cast<InputAction>(a), slots[s]);
        }
        if (!on_pad)
        {
            return true;
        }
    }
    return false;
}

static int16_t to_axis(float value)
{
    float scaled = value * 32767.0f;
    scaled = scaled > 32767.0f ? 32767.0f : (scaled < -32767.0f ? -32767.0f : scaled);
    return static_cast<int16_t>(scaled);
}

// One stick of a port: plus minus minus per axis, then a round deadzone
// (the rest rescaled so movement starts at 0 past it).
static void stick(const InputController* controller, const InputDevice* device, InputAction x_plus, int16_t* out)
{
    uint32_t base = index_of(x_plus);
    float x = action_value(controller, device, static_cast<InputAction>(base)) -
              action_value(controller, device, static_cast<InputAction>(base + 1));
    float y = action_value(controller, device, static_cast<InputAction>(base + 2)) -
              action_value(controller, device, static_cast<InputAction>(base + 3));
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

void input_update(InputSystem* system, uint64_t now_ms, InputFrame* frame)
{
    *frame = InputFrame{};
    bool back_to_game = system->settings.game_has_input && !system->game_had_input;
    system->game_had_input = system->settings.game_has_input;

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
        if (device == nullptr || !device->connected || input_devices_looks_fake(device))
        {
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

        // Joining.
        bool plays = system->settings.game_has_input;
        bool any_pad_held = false;
        for (uint32_t a = 0; a <= index_of(InputAction::pad_right_y_minus); a++)
        {
            any_pad_held = any_pad_held || held[a];
        }
        if (back_to_game)
        {
            controller->quiet_until_released = any_pad_held;
        }
        else if (controller->quiet_until_released && !any_pad_held)
        {
            controller->quiet_until_released = false;
        }
        int32_t port = input_controller_port(system, index);
        if (plays && port == INPUT_NO_PORT && pressed[index_of(InputAction::pad_start)])
        {
            port = join_port(system, index, INPUT_NO_PORT);
            if (port != INPUT_NO_PORT)
            {
                controller->quiet_until_start_released = true;
                frame->joined_port = port;
                frame->joined_controller = static_cast<int32_t>(index);
            }
        }
        if (controller->quiet_until_start_released && !held[index_of(InputAction::pad_start)])
        {
            controller->quiet_until_start_released = false;
        }

        // HOTKEY ENABLE.
        uint32_t enable = index_of(InputAction::hotkey_enable);
        bool enabled = false;
        bool quiet = controller->quiet_until_start_released || controller->quiet_until_released;
        if (system->settings.hotkey_mode == InputHotkeyMode::hold)
        {
            if (pressed[enable])
            {
                controller->enable_down_ms = now_ms;
            }
            enabled = held[enable];
            quiet = quiet || (held[enable] && now_ms - controller->enable_down_ms >= HOTKEY_BLOCK_DELAY_MS);
        }
        else
        {
            if (pressed[enable])
            {
                if (controller->enable_last_tap_ms != 0 && now_ms - controller->enable_last_tap_ms <= DOUBLE_TAP_MS)
                {
                    controller->armed = true;
                    controller->armed_ms = now_ms;
                    controller->enable_last_tap_ms = 0;
                }
                else
                {
                    controller->enable_last_tap_ms = now_ms;
                }
            }
            if (controller->armed && now_ms - controller->armed_ms > ARMED_TIMEOUT_MS)
            {
                controller->armed = false;
            }
            enabled = controller->armed;
            quiet = quiet || controller->armed;
        }

        // Interface and hotkeys, from every controller.
        bool fired = false;
        for (uint32_t a = index_of(InputAction::ui_up); a < INPUT_ACTION_COUNT; a++)
        {
            InputAction action = static_cast<InputAction>(a);
            if (action == InputAction::hotkey_enable)
            {
                continue;
            }
            if (input_action_is_hotkey(action) && !enabled && !hotkey_free(system, controller, device, action))
            {
                continue;
            }
            if (!input_action_is_hotkey(action) && enabled)
            {
                continue;   // the hotkey layer is on: its buttons aren't interface controls now
            }
            frame->held[a] = frame->held[a] || held[a];
            frame->pressed[a] = frame->pressed[a] || pressed[a];
            if (device->kind != InputDeviceKind::keyboard && !input_action_is_hotkey(action))
            {
                frame->gamepad_held[a] = frame->gamepad_held[a] || held[a];
            }
            fired = fired || (input_action_is_hotkey(action) && pressed[a]);
        }
        if (fired && controller->armed)
        {
            controller->armed = false;   // DOUBLE TAP: one hotkey, then back to normal
        }

        // The RetroPad of its port.
        if (port == INPUT_NO_PORT || quiet || !plays)
        {
            continue;
        }
        uint16_t buttons = 0;
        for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
        {
            if (held[b])
            {
                buttons = static_cast<uint16_t>(buttons | (1u << b));
            }
        }
        frame->pad_buttons[port] = static_cast<uint16_t>(frame->pad_buttons[port] | buttons);
        stick(controller, device, InputAction::pad_left_x_plus, frame->analog[port][0]);
        stick(controller, device, InputAction::pad_right_x_plus, frame->analog[port][1]);
    }
}

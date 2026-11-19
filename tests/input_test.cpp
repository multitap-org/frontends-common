// Checks the neutral input layer (input_devices, input_bindings) with no
// hardware: sources and their text form, joining ports, a gamepad taking the
// keyboard's port, devices leaving and coming back, hotkeys (keyboard rules,
// HOLD, DOUBLE TAP), combos, the stick deadzone, short taps and fake gamepads.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>

#include "input_bindings.h"
#include "input_config.h"
#include "input_devices.h"
#include "input_layouts.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

constexpr uint32_t HID_Z = 29, HID_X = 27, HID_RETURN = 40, HID_F1 = 58, HID_F2 = 59, HID_UP = 82, HID_LEFT_SHIFT = 225;
constexpr uint32_t PAD_BIT_B = 1u << 0, PAD_BIT_START = 1u << 3, PAD_BIT_A = 1u << 8;

static InputSource button(InputButton b)
{
    InputSource source = {};
    source.type = InputSourceType::button;
    source.code = static_cast<uint16_t>(b);
    return source;
}

static InputSource axis(InputAxis a, bool plus)
{
    InputSource source = {};
    source.type = plus ? InputSourceType::axis_plus : InputSourceType::axis_minus;
    source.code = static_cast<uint16_t>(a);
    return source;
}

static void gamepad_bindings(InputBindingSet* set)
{
    *set = InputBindingSet{};
    input_bind(set, InputAction::pad_b, button(InputButton::south));
    input_bind(set, InputAction::pad_a, button(InputButton::east));
    input_bind(set, InputAction::pad_start, button(InputButton::start));
    input_bind(set, InputAction::pad_select, button(InputButton::back));
    input_bind(set, InputAction::pad_left_x_plus, axis(InputAxis::left_x, true));
    input_bind(set, InputAction::pad_left_x_minus, axis(InputAxis::left_x, false));
    input_bind(set, InputAction::pad_left_y_plus, axis(InputAxis::left_y, true));
    input_bind(set, InputAction::pad_left_y_minus, axis(InputAxis::left_y, false));
    input_bind(set, InputAction::hotkey_enable, button(InputButton::back));
    input_bind(set, InputAction::hotkey_quick_menu, button(InputButton::start));
}

// One frame: update, then forget the taps (as a frontend does).
static void frame(InputSystem* system, uint64_t now_ms, InputFrame* out)
{
    input_update(system, now_ms, out);
    input_devices_end_frame(&system->devices);
}

static void test_sources()
{
    char text[64] = {};
    InputSource source = {};
    check(input_source_parse("key:Return", &source) && source.type == InputSourceType::key && source.code == HID_RETURN,
          "sources: key by name");
    check(input_source_format(source, text, sizeof(text)) && std::strcmp(text, "key:Return") == 0, "sources: key written back");
    check(input_source_parse("key:#250", &source) && source.code == 250, "sources: key by number");
    check(input_source_format(source, text, sizeof(text)) && std::strcmp(text, "key:#250") == 0, "sources: number written back");
    check(input_source_parse("button:left_shoulder", &source) && source.type == InputSourceType::button &&
              source.code == static_cast<uint16_t>(InputButton::left_shoulder),
          "sources: button");
    check(input_source_parse("axis:right_trigger+", &source) && source.type == InputSourceType::axis_plus &&
              source.code == static_cast<uint16_t>(InputAxis::right_trigger),
          "sources: axis");
    check(input_source_format(source, text, sizeof(text)) && std::strcmp(text, "axis:right_trigger+") == 0, "sources: axis written back");
    check(!input_source_parse("key:NoSuchKey", &source), "sources: unknown key refused");
    check(!input_source_parse("axis:left_x", &source), "sources: axis without direction refused");
    check(!input_source_parse("wheel:up", &source), "sources: unknown type refused");

    // A percent on a key or button: how far it pushes a stick.
    check(input_source_parse("button:dpad_right@20", &source) && source.type == InputSourceType::button &&
              source.code == static_cast<uint16_t>(InputButton::dpad_right) && source.percent == 20,
          "sources: a percent");
    check(input_source_format(source, text, sizeof(text)) && std::strcmp(text, "button:dpad_right@20") == 0,
          "sources: a percent written back");
    InputSource full = source;
    full.percent = 100;
    check(input_source_equal(source, full), "sources: the percent isn't part of the control");
    check(input_source_format(full, text, sizeof(text)) && std::strcmp(text, "button:dpad_right") == 0,
          "sources: 100 % isn't written");
    check(!input_source_parse("button:dpad_right@0", &source) && !input_source_parse("button:dpad_right@101", &source) &&
              !input_source_parse("button:dpad_right@", &source),
          "sources: a percent out of 1..100 refused");
    InputDevice device = {};
    device.held[static_cast<uint32_t>(InputButton::dpad_right)] = 1;
    InputSource walk = {};
    input_source_parse("button:dpad_right@20", &walk);
    float value = input_source_value(&device, walk);
    check(value > 0.19f && value < 0.21f, "sources: a button at 20 % gives 0.2");

    // An action's sources as rows.
    InputSource rows[INPUT_SOURCES_PER_ACTION] = {};
    InputSource up = {};
    input_source_parse("button:dpad_up", &up);
    InputSource stick = {};
    input_source_parse("axis:left_y-", &stick);
    InputSource north = {};
    input_source_parse("button:north", &north);
    input_sources_set(rows, 0, up);
    input_sources_set(rows, 1, stick);
    check(input_sources_count(rows) == 2 && input_source_equal(rows[1], stick), "rows: a second row added");
    input_sources_set(rows, 0, north);
    check(input_sources_count(rows) == 2 && input_source_equal(rows[0], north), "rows: a row replaced");
    input_sources_set(rows, 2, north);
    check(input_sources_count(rows) == 2 && input_source_equal(rows[0], stick) && input_source_equal(rows[1], north),
          "rows: the same control moves, not repeated");
    input_sources_remove(rows, 0);
    check(input_sources_count(rows) == 1 && input_source_equal(rows[0], north), "rows: removed, the rest moves up");

    InputAction action = InputAction::count;
    check(input_action_parse("hotkey.quick_menu", &action) && action == InputAction::hotkey_quick_menu, "actions: by name");
    check(std::strcmp(input_action_name(InputAction::pad_l2), "pad.l2") == 0, "actions: name");
}

static void test_keyboard_join_and_play()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;   // the keyboard joins like any controller
    uint32_t keyboard = input_devices_add(&system->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    InputBindingSet keys = {};
    input_default_keyboard_bindings(&keys);
    input_add_controller(system, keyboard, "Keyboard", &keys);
    InputFrame out = {};

    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 0, &out);
    check(out.pad_buttons[0] == 0, "keyboard: nothing before joining");

    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    frame(system, 16, &out);
    check(out.joined_port == 0 && input_port_has(system, 0, 0), "keyboard: START joins port 1");
    check((out.pad_buttons[0] & PAD_BIT_START) == 0, "keyboard: the joining START isn't the game's");
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, false);
    frame(system, 32, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) != 0, "keyboard: Z is B once joined");

    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    frame(system, 48, &out);
    check((out.pad_buttons[0] & PAD_BIT_START) != 0, "keyboard: START reaches the game after joining");
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, false);

    // A tap shorter than a frame still counts once.
    input_devices_set_code(&system->devices, keyboard, HID_X, true);
    input_devices_set_code(&system->devices, keyboard, HID_X, false);
    frame(system, 64, &out);
    check((out.pad_buttons[0] & PAD_BIT_A) != 0, "keyboard: a short tap counts");
    frame(system, 80, &out);
    check((out.pad_buttons[0] & PAD_BIT_A) == 0, "keyboard: ...once");

    // Keyboard hotkeys need the modifier (Left Shift): F1 alone is nothing in the game.
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 96, &out);
    check(!out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "keyboard: F1 alone is no hotkey");
    input_devices_set_code(&system->devices, keyboard, HID_F1, false);
    frame(system, 112, &out);
    input_devices_set_code(&system->devices, keyboard, HID_LEFT_SHIFT, true);
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 128, &out);
    check(out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "keyboard: Shift + F1 is the quick menu");
    check(system->controllers[0].layer == InputLayer::hotkey, "keyboard: in the HOTKEY layer with Shift");
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 144, &out);
    check(out.pad_buttons[0] == 0, "keyboard: no RetroPad in the HOTKEY layer");
    input_devices_set_code(&system->devices, keyboard, HID_LEFT_SHIFT, false);
    input_devices_set_code(&system->devices, keyboard, HID_F1, false);
    frame(system, 160, &out);
    check(out.pad_buttons[0] == 0, "keyboard: back in the game, Z held since the switch is ignored");
    input_devices_set_code(&system->devices, keyboard, HID_Z, false);
    frame(system, 176, &out);
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 192, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) != 0, "keyboard: pressed again, it plays");
    delete system;
}

static void test_split_keyboard()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    uint32_t keyboard = input_devices_add(&system->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    InputBindingSet left = {};
    input_default_keyboard_bindings(&left);
    InputBindingSet right = {};
    InputSource up = {};
    up.type = InputSourceType::key;
    up.code = HID_UP;
    InputSource f1 = {};
    f1.type = InputSourceType::key;
    f1.code = HID_F1;
    input_bind(&right, InputAction::pad_start, f1);
    input_bind(&right, InputAction::pad_up, up);
    input_add_controller(system, keyboard, "Keyboard 1", &left);
    input_add_controller(system, keyboard, "Keyboard 2", &right);
    InputFrame out = {};

    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    frame(system, 0, &out);
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, false);
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 16, &out);
    check(input_port_has(system, 0, 0) && input_port_has(system, 1, 1), "split keyboard: two controllers on two ports");
    delete system;
}

static void test_shared_port_and_reconnects()
{
    InputSystem* system = new InputSystem{};
    uint32_t keyboard = input_devices_add(&system->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    InputBindingSet keys = {};
    input_default_keyboard_bindings(&keys);
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    uint32_t keyboard_controller = input_add_controller(system, keyboard, "Keyboard", &keys);
    uint32_t pad_controller = input_add_controller(system, pad, "Pad", &pad_set);
    InputFrame out = {};

    // The keyboard is put on port 1 without joining.
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 0, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) != 0 && input_port_has(system, 0, keyboard_controller) && system->ports[0].size() == 1,
          "shared: the keyboard is on port 1");
    input_devices_set_code(&system->devices, keyboard, HID_Z, false);

    // A gamepad does nothing until START; then it's on port 1 too.
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::south), true);
    frame(system, 16, &out);
    check(out.pad_buttons[0] == 0, "shared: a gamepad that hasn't joined does nothing");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::south), false);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), true);
    frame(system, 32, &out);
    check(out.joined_port == 0 && input_port_has(system, 0, pad_controller), "shared: the gamepad joins port 1 (only the keyboard there)");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), false);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), true);
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 48, &out);
    check((out.pad_buttons[0] & (PAD_BIT_A | PAD_BIT_B)) == (PAD_BIT_A | PAD_BIT_B), "shared: both press port 1");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), false);
    input_devices_set_code(&system->devices, keyboard, HID_Z, false);

    // Joining works with the UI up; only port 1's gamepad drives the UI.
    system->settings.ui_active = true;
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::dpad_down), true);
    frame(system, 64, &out);
    check(!out.gamepad_held[static_cast<uint32_t>(InputAction::ui_down)], "ui: a gamepad without ui bindings doesn't drive it");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::dpad_down), false);
    system->settings.ui_active = false;
    frame(system, 72, &out);

    // Unplugged: port freed and reported; plugged back: same port, same id.
    input_devices_disconnect(&system->devices, pad);
    frame(system, 80, &out);
    check(!input_port_has(system, 0, pad_controller) && out.left_port == 0 && out.left_controller == static_cast<int32_t>(pad_controller),
          "reconnect: unplugging frees the port and says so");
    uint32_t again = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    check(again == pad, "reconnect: same device id");
    frame(system, 96, &out);
    check(input_port_has(system, 0, pad_controller), "reconnect: back on its port");

    // Without sharing, the keyboard needs START.
    system->settings.keyboard_shares_port1 = false;
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 112, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) == 0, "not shared: the keyboard plays only after joining");
    input_devices_set_code(&system->devices, keyboard, HID_Z, false);
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    frame(system, 128, &out);
    check(!input_port_has(system, 0, keyboard_controller) && input_port_has(system, 1, keyboard_controller),
          "not shared: off port 1; START puts it on the next free port");

    input_swap_ports(system, 0, 1);
    check(input_port_has(system, 0, keyboard_controller) && input_port_has(system, 1, pad_controller), "swap: ports reordered");
    delete system;
}

static void test_gamepad_hotkeys()
{
    InputSystem* system = new InputSystem{};
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    uint32_t controller = input_add_controller(system, pad, "Pad", &pad_set);
    input_join(system, controller);
    InputFrame out = {};
    uint32_t quick_menu = static_cast<uint32_t>(InputAction::hotkey_quick_menu);
    uint32_t back = static_cast<uint32_t>(InputButton::back);
    uint32_t start = static_cast<uint32_t>(InputButton::start);

    // START alone: the game's, not a hotkey.
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 0, &out);
    check(!out.pressed[quick_menu] && (out.pad_buttons[0] & PAD_BIT_START) != 0, "hold: START alone is the game's");
    input_devices_set_code(&system->devices, pad, start, false);
    frame(system, 16, &out);

    // A quick SELECT tap reaches the game.
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 32, &out);
    check((out.pad_buttons[0] & (1u << 2)) != 0, "hold: a quick SELECT reaches the game");
    // Held past the delay: the RetroPad goes quiet, SELECT+START = quick menu.
    frame(system, 200, &out);
    check(out.pad_buttons[0] == 0, "hold: RetroPad quiet while enable is held");
    check(out.gamepad_enable_alone, "hold: SELECT held alone is reported (port 1)");
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 216, &out);
    check(out.pressed[quick_menu], "hold: SELECT+START is the quick menu");
    check(!out.gamepad_enable_alone, "hold: not alone once a hotkey fired with it");
    input_devices_set_code(&system->devices, pad, start, false);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 232, &out);

    // ONE HOTKEY: a double tap of SELECT turns hotkeys on for one hotkey.
    system->controllers[controller].hotkey_mode = InputHotkeyMode::one_hotkey;
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 1000, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 1050, &out);
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 1100, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 1150, &out);
    check(system->controllers[controller].layer == InputLayer::hotkey, "one hotkey: on after a double tap");
    check(out.pad_buttons[0] == 0, "one hotkey: no RetroPad while on");
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 1200, &out);
    check(out.pressed[quick_menu], "one hotkey: START fires the hotkey");
    input_devices_set_code(&system->devices, pad, start, false);
    frame(system, 1216, &out);
    check(system->controllers[controller].layer == InputLayer::game, "one hotkey: back to the game after one");
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 1232, &out);
    check(!out.pressed[quick_menu] && (out.pad_buttons[0] & PAD_BIT_START) != 0, "one hotkey: START is the game's again");
    input_devices_set_code(&system->devices, pad, start, false);
    frame(system, 1248, &out);

    // Slow taps don't turn it on; it turns itself off after a while.
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 3000, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 3100, &out);
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 3500, &out);
    check(system->controllers[controller].layer != InputLayer::hotkey, "one hotkey: slow taps don't turn it on");
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 3550, &out);
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 3600, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 3650, &out);
    check(system->controllers[controller].layer == InputLayer::hotkey, "one hotkey: on again");
    frame(system, 7000, &out);
    check(system->controllers[controller].layer == InputLayer::game, "one hotkey: times out");

    // STAYS ON: on after a double tap, through hotkeys, off after another.
    system->controllers[controller].hotkey_mode = InputHotkeyMode::stays_on;
    for (uint64_t t = 8000; t <= 8150; t += 50)
    {
        input_devices_set_code(&system->devices, pad, back, (t / 50) % 2 == 0);
        frame(system, t, &out);
    }
    check(system->controllers[controller].layer == InputLayer::hotkey, "stays on: on after a double tap");
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 8200, &out);
    input_devices_set_code(&system->devices, pad, start, false);
    frame(system, 8250, &out);
    check(out.pressed[quick_menu] || system->controllers[controller].layer == InputLayer::hotkey, "stays on: a hotkey fires");
    check(system->controllers[controller].layer == InputLayer::hotkey, "stays on: still on after a hotkey");
    for (uint64_t t = 9000; t <= 9150; t += 50)
    {
        input_devices_set_code(&system->devices, pad, back, (t / 50) % 2 == 0);
        frame(system, t, &out);
    }
    check(system->controllers[controller].layer == InputLayer::game, "stays on: off after another double tap");
    delete system;
}

static void test_combo_and_stick()
{
    InputSystem* system = new InputSystem{};
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    // L as A + B (a combo): the same source on two actions.
    input_bind(&pad_set, InputAction::pad_a, button(InputButton::left_shoulder));
    input_bind(&pad_set, InputAction::pad_b, button(InputButton::left_shoulder));
    uint32_t controller = input_add_controller(system, pad, "Pad", &pad_set);
    input_join(system, controller);
    InputFrame out = {};

    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::left_shoulder), true);
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, 0.1f);   // inside the deadzone
    frame(system, 0, &out);
    check((out.pad_buttons[0] & (PAD_BIT_A | PAD_BIT_B)) == (PAD_BIT_A | PAD_BIT_B), "combo: L presses A and B");
    check(out.analog[0][0][0] == 0, "stick: inside the deadzone is 0");
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, 1.0f);
    frame(system, 16, &out);
    check(out.analog[0][0][0] == 32767, "stick: all the way is full");
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, -0.575f);
    frame(system, 32, &out);
    check(out.analog[0][0][0] < -16000 && out.analog[0][0][0] > -17000, "stick: rescaled past the deadzone");

    // D-pad right on the stick at 50 %, the real stick resting a little off on Y:
    // the drift stays inside the deadzone, only X moves.
    InputController* bound = &system->controllers[controller];
    InputSource half = button(InputButton::dpad_right);
    half.percent = 50;
    input_bind(&bound->bindings, InputAction::pad_left_x_plus, half);
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, 0.0f);
    input_devices_set_axis(&system->devices, pad, InputAxis::left_y, -0.04f);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::dpad_right), true);
    frame(system, 48, &out);
    check(out.analog[0][0][0] > 16000 && out.analog[0][0][0] < 16700, "stick: a button at 50 % pushes half way");
    check(out.analog[0][0][1] == 0, "stick: the resting drift on the other axis stays out");
    delete system;
}

// Presses START on a gamepad and lets go (a frame each).
static void press_start(InputSystem* system, uint32_t pad, uint64_t now_ms, InputFrame* out)
{
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), true);
    frame(system, now_ms, out);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), false);
    InputFrame after = {};
    frame(system, now_ms + 8, &after);
}

// N:N: several controllers on a port (N:1), a controller on several ports
// (1:N), START with every port taken, unplugging from several ports.
static void test_shared_ports()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    uint32_t pads[5] = {};
    const char* guids[5] = {"guid-a", "guid-b", "guid-c", "guid-d", "guid-e"};
    for (uint32_t i = 0; i < 5; i++)
    {
        pads[i] = input_devices_add(&system->devices, InputDeviceKind::gamepad, guids[i], "Pad");
        input_add_controller(system, pads[i], "Pad", &pad_set);   // controller i
    }
    InputFrame out = {};
    press_start(system, pads[0], 0, &out);
    check(out.joined_port == 0, "n:n: the first START joins port 1");
    press_start(system, pads[1], 100, &out);
    check(out.joined_port == 1, "n:n: the next one port 2");

    // B also on port 1 (N:1 there, 1:N for B).
    input_port_add(system, 0, 1);
    input_port_add(system, 0, 1);
    check(system->ports[0].size() == 2 && system->ports[1].size() == 1, "n:n: added once");
    input_devices_set_code(&system->devices, pads[0], static_cast<uint32_t>(InputButton::east), true);
    input_devices_set_code(&system->devices, pads[1], static_cast<uint32_t>(InputButton::south), true);
    frame(system, 200, &out);
    check(out.pad_buttons[0] == (PAD_BIT_A | PAD_BIT_B), "n:1: both members' buttons on port 1");
    check(out.pad_buttons[1] == PAD_BIT_B, "1:n: B plays port 2 as well");
    input_devices_set_code(&system->devices, pads[0], static_cast<uint32_t>(InputButton::east), false);
    input_devices_set_code(&system->devices, pads[1], static_cast<uint32_t>(InputButton::south), false);

    // Sticks: the member pushing furthest, not a sum.
    input_devices_set_axis(&system->devices, pads[1], InputAxis::left_x, -0.5f);   // (a real stick: not pinned from the start)
    input_devices_set_axis(&system->devices, pads[0], InputAxis::left_x, 0.6f);
    input_devices_set_axis(&system->devices, pads[1], InputAxis::left_x, -1.0f);
    frame(system, 216, &out);
    check(out.analog[0][0][0] == -32767, "n:1: the stick pushed furthest wins");
    input_devices_set_axis(&system->devices, pads[1], InputAxis::left_x, -0.3f);
    frame(system, 232, &out);
    check(out.analog[0][0][0] > 16000, "n:1: ...whichever it is now");
    check(out.analog[1][0][0] < 0 && out.analog[1][0][0] > -16000, "1:n: port 2 has B's stick alone");
    input_devices_set_axis(&system->devices, pads[0], InputAxis::left_x, 0.0f);
    input_devices_set_axis(&system->devices, pads[1], InputAxis::left_x, 0.0f);

    // B drives the UI now (a member of port 1).
    system->settings.ui_active = true;
    input_bind(&system->controllers[1].bindings, InputAction::ui_down, button(InputButton::dpad_down));
    frame(system, 248, &out);
    input_devices_set_code(&system->devices, pads[1], static_cast<uint32_t>(InputButton::dpad_down), true);
    frame(system, 264, &out);
    check(out.gamepad_held[static_cast<uint32_t>(InputAction::ui_down)], "n:n: any member of port 1 drives the UI");
    input_devices_set_code(&system->devices, pads[1], static_cast<uint32_t>(InputButton::dpad_down), false);
    system->settings.ui_active = false;
    frame(system, 280, &out);

    // Unplugged: off both ports; back on both.
    input_devices_disconnect(&system->devices, pads[1]);
    frame(system, 300, &out);
    check(!input_port_has(system, 0, 1) && !input_port_has(system, 1, 1) && out.left_port == 0, "n:n: unplugged leaves every port");
    input_devices_add(&system->devices, InputDeviceKind::gamepad, guids[1], "Pad");
    frame(system, 316, &out);
    check(input_port_has(system, 0, 1) && input_port_has(system, 1, 1), "n:n: plugged back on every port");

    // Off one port only.
    input_port_remove(system, 0, 1);
    check(!input_port_has(system, 0, 1) && input_port_has(system, 1, 1) && input_port_has(system, 0, 0), "n:n: - leaves that port only");

    // Every port taken: START doesn't join (it can still be added by hand).
    press_start(system, pads[2], 400, &out);
    press_start(system, pads[3], 500, &out);
    check(input_port_has(system, 2, 2) && input_port_has(system, 3, 3), "n:n: ports 3 and 4 taken");
    press_start(system, pads[4], 600, &out);
    check(out.joined_port == INPUT_NO_PORT && input_controller_port(system, 4) == INPUT_NO_PORT, "n:n: all taken, START doesn't join");
    input_port_add(system, 2, 4);
    check(input_port_has(system, 2, 4), "n:n: added by hand to a taken port");

    // Hotkeys are port 1's: port 4's SELECT + START is the game's.
    input_devices_set_code(&system->devices, pads[3], static_cast<uint32_t>(InputButton::back), true);
    frame(system, 700, &out);
    frame(system, 900, &out);
    input_devices_set_code(&system->devices, pads[3], static_cast<uint32_t>(InputButton::start), true);
    frame(system, 916, &out);
    check(!out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)] && (out.pad_buttons[3] & PAD_BIT_START) != 0,
          "n:n: hotkeys only from port 1");
    input_devices_set_code(&system->devices, pads[3], static_cast<uint32_t>(InputButton::back), false);
    input_devices_set_code(&system->devices, pads[3], static_cast<uint32_t>(InputButton::start), false);
    frame(system, 932, &out);

    // Swapping ports moves whole member lists.
    input_swap_ports(system, 1, 2);
    check(input_port_has(system, 1, 2) && input_port_has(system, 1, 4) && input_port_has(system, 2, 1), "n:n: swap moves the lists");
    delete system;
}

static void test_fake_gamepad()
{
    InputSystem* system = new InputSystem{};
    uint32_t fake = input_devices_add(&system->devices, InputDeviceKind::gamepad, "k83", "Keyboard receiver");
    input_devices_set_axis(&system->devices, fake, InputAxis::left_x, -1.0f);
    input_devices_set_axis(&system->devices, fake, InputAxis::left_y, -1.0f);
    check(input_devices_looks_fake(input_devices_find(&system->devices, fake)), "fake: pinned axes from the start");
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    uint32_t controller = input_add_controller(system, fake, "Receiver", &pad_set);
    InputFrame out = {};
    input_devices_set_code(&system->devices, fake, static_cast<uint32_t>(InputButton::start), true);
    frame(system, 0, &out);
    check(input_controller_port(system, controller) == INPUT_NO_PORT, "fake: can't join");

    uint32_t real = input_devices_add(&system->devices, InputDeviceKind::gamepad, "real", "Pad");
    input_devices_set_axis(&system->devices, real, InputAxis::left_x, 0.02f);
    input_devices_set_axis(&system->devices, real, InputAxis::left_x, 1.0f);
    check(!input_devices_looks_fake(input_devices_find(&system->devices, real)), "fake: a real stick isn't");
    delete system;
}

static void test_menus_and_defaults()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    uint32_t keyboard = input_devices_add(&system->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    InputBindingSet keys = {};
    input_default_keyboard_bindings(&keys);
    uint32_t controller = input_add_controller(system, keyboard, "Keyboard", &keys);
    InputFrame out = {};

    // Menus up: the keyboard neither joins nor plays, but its hotkeys work.
    system->settings.ui_active = true;
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 0, &out);
    check(input_controller_port(system, controller) == 0, "menus: START joins with the UI up too");
    check(!out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "menus: F1 alone is no hotkey in the UI either");
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, false);
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 16, &out);
    check(out.pad_buttons[0] == 0, "menus: the keyboard doesn't play");
    system->settings.ui_active = false;
    frame(system, 32, &out);
    check(out.pad_buttons[0] == 0, "menus closed: quiet while Z is still held");
    input_devices_set_code(&system->devices, keyboard, HID_Z, false);
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, false);   // START, a RetroPad button too
    frame(system, 48, &out);
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 64, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) != 0, "menus closed: it plays again once let go");

    input_leave_all(system);
    check(system->ports[0].empty() && input_first_controller(system, keyboard) == static_cast<int32_t>(controller),
          "leave all: ports free, controller kept");

    // A gamepad's interface actions are reported apart (to drive the UI).
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    InputBindingSet defaults = {};
    input_default_gamepad_bindings(&defaults);
    uint32_t pad_controller = input_add_controller(system, pad, "Pad", &defaults);
    input_leave_all(system);
    input_join(system, pad_controller);   // port 1 drives the UI
    system->settings.ui_active = true;   // the INTERFACE layer
    frame(system, 72, &out);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::dpad_down), true);
    input_devices_set_code(&system->devices, keyboard, HID_UP, true);
    frame(system, 80, &out);
    check(out.gamepad_held[static_cast<uint32_t>(InputAction::ui_down)], "ui: the gamepad's down is reported");
    check(!out.gamepad_held[static_cast<uint32_t>(InputAction::ui_up)], "ui: the keyboard's up is not in gamepad_held");
    check(out.held[static_cast<uint32_t>(InputAction::ui_up)], "ui: ...but is in held");

    // A gamepad without d-pad buttons: its directions on the left stick.
    InputDevice no_dpad = {};
    no_dpad.kind = InputDeviceKind::gamepad;
    no_dpad.has_dpad = false;
    InputBindingSet stick_dpad = {};
    input_default_bindings_for(&no_dpad, &stick_dpad);
    check(input_is_bound(&stick_dpad, InputAction::pad_up, axis(InputAxis::left_y, false)) &&
              !input_is_bound(&stick_dpad, InputAction::pad_up, button(InputButton::dpad_up)),
          "defaults: no d-pad buttons, directions on the left stick");
    check(input_is_bound(&stick_dpad, InputAction::ui_left, axis(InputAxis::left_x, false)),
          "defaults: no d-pad buttons, the interface on the left stick too");
    InputBindingSet with_dpad = {};
    input_default_gamepad_bindings(&with_dpad);
    check(input_sources_count(with_dpad.sources[static_cast<uint32_t>(InputAction::ui_up)]) == 1 &&
              input_is_bound(&with_dpad, InputAction::ui_up, button(InputButton::dpad_up)),
          "defaults: the interface moves with the D-pad only");

    // The gamepad defaults: B bottom, A right, R2 on the trigger, SELECT+START quick menu, Y the main menu.
    InputBindingSet pad_set = {};
    input_default_gamepad_bindings(&pad_set);
    check(input_is_bound(&pad_set, InputAction::pad_b, button(InputButton::south)), "defaults: B is the bottom button");
    check(input_is_bound(&pad_set, InputAction::pad_a, button(InputButton::east)), "defaults: A is the right button");
    check(input_is_bound(&pad_set, InputAction::pad_r2, axis(InputAxis::right_trigger, true)), "defaults: R2 is the trigger");
    check(input_is_bound(&pad_set, InputAction::hotkey_enable, button(InputButton::back)), "defaults: HOTKEY ENABLE is SELECT");
    check(input_is_bound(&pad_set, InputAction::hotkey_quick_menu, button(InputButton::start)), "defaults: + START = quick menu");
    check(input_is_bound(&pad_set, InputAction::ui_open_menu, button(InputButton::west)), "defaults: Y (left) opens the menu bar");
    check(input_is_bound(&pad_set, InputAction::hotkey_menu, button(InputButton::west)), "defaults: + Y (left) = main menu");
    InputAction last = InputAction::count;
    check(input_action_parse("hotkey.fast_forward", &last) && last == InputAction::hotkey_fast_forward, "actions: names line up");
    delete system;
}

static void test_config()
{
    // A split keyboard, a renamed gamepad with a changed binding and deadzone.
    InputSystem* first = new InputSystem{};
    InputSource f1 = {};
    f1.type = InputSourceType::key;
    f1.code = HID_F1;
    InputProfile second_part = {};
    second_part.id = "keyboard.2";
    second_part.name = "Keyboard right";
    InputBindingOverride start = {};
    start.action = InputAction::pad_start;
    start.sources[0] = f1;
    second_part.overrides.push_back(start);
    first->profiles.push_back(second_part);
    uint32_t keyboard = input_devices_add(&first->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    uint32_t pad = input_devices_add(&first->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    input_add_controllers_for_device(first, keyboard);
    input_add_controllers_for_device(first, pad);
    check(first->controllers.size() == 3, "config: two keyboard parts and the gamepad");
    check(first->controllers[1].name == "Keyboard right", "config: profile name used");
    check(input_is_bound(&first->controllers[1].bindings, InputAction::pad_start, f1), "config: profile binding used");
    InputController* gamepad = &first->controllers[2];
    gamepad->name = "My pad";
    gamepad->deadzone = 0.25f;
    input_unbind_all(&gamepad->bindings, InputAction::pad_y);
    input_bind(&gamepad->bindings, InputAction::pad_y, button(InputButton::north));
    input_unbind_all(&gamepad->bindings, InputAction::hotkey_save_state);
    const char* MAPPING = "pad-guid,Pad,a:b1,b:b0,dpup:h0.1,leftx:a0,platform:Linux,";
    input_profile_for(first, "pad-guid")->sdl_mapping = MAPPING;
    input_profile_for(first, "pad-guid")->button_labels[static_cast<uint32_t>(InputButton::misc1)] = "VOLUME UP";

    SettingsFile file = {};
    input_config_write(first, &file);
    std::string text;
    settings_file_to_text(&file, nullptr, &text);
    check(text.find("[controller/pad-guid]") != std::string::npos && text.find("deadzone = 25") != std::string::npos,
          "config: name and deadzone written");
    check(text.find("pad.y = button:north") != std::string::npos, "config: changed binding written");
    check(text.find("hotkey.save_state = none") != std::string::npos, "config: unbound written as none");
    check(text.find("pad.b") == std::string::npos, "config: unchanged bindings not written");
    check(text.find("[retropad/keyboard]") == std::string::npos, "config: the default keyboard writes no bindings");

    // Read back into a fresh system: the same controllers.
    SettingsFile parsed = {};
    settings_file_parse(&parsed, text.c_str(), text.size(), "test");
    InputSystem* second = new InputSystem{};
    input_config_read(second, &parsed);
    uint32_t keyboard2 = input_devices_add(&second->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    uint32_t pad2 = input_devices_add(&second->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    input_add_controllers_for_device(second, keyboard2);
    input_add_controllers_for_device(second, pad2);
    check(second->controllers.size() == 3, "config: same controllers after reading");
    if (second->controllers.size() == 3)
    {
        const InputController* back = &second->controllers[2];
        check(back->name == "My pad", "config: name back");
        check(back->deadzone > 0.24f && back->deadzone < 0.26f, "config: deadzone back");
        check(input_is_bound(&back->bindings, InputAction::pad_y, button(InputButton::north)) &&
                  !input_is_bound(&back->bindings, InputAction::pad_y, button(InputButton::west)),
              "config: binding back");
        check(back->bindings.sources[static_cast<uint32_t>(InputAction::hotkey_save_state)][0].type == InputSourceType::none,
              "config: unbound back");
        check(input_is_bound(&back->bindings, InputAction::pad_b, button(InputButton::south)), "config: defaults kept");
        check(second->controllers[1].name == "Keyboard right", "config: keyboard part back");
    }

    // A profile for a gamepad not connected now survives a write.
    InputSystem* third = new InputSystem{};
    input_config_read(third, &parsed);
    SettingsFile again = {};
    input_config_write(third, &again);
    std::string text_again;
    settings_file_to_text(&again, nullptr, &text_again);
    check(text_again.find("[controller/pad-guid]") != std::string::npos, "config: absent controller kept");
    const InputProfile* kept = input_profile(third, "pad-guid");
    check(kept != nullptr && kept->sdl_mapping == MAPPING, "config: SDL mapping read back");
    check(kept != nullptr && kept->button_labels[static_cast<uint32_t>(InputButton::misc1)] == "VOLUME UP",
          "config: a button's name read back");
    check(text_again.find("label.misc1 = VOLUME UP") != std::string::npos, "config: a button's name written");
    check(text_again.find(std::string("sdl_mapping = ") + MAPPING) != std::string::npos, "config: SDL mapping kept");
    delete first;
    delete second;
    delete third;
}

static void test_capture_and_remove()
{
    InputSystem* system = new InputSystem{};
    uint32_t keyboard = input_devices_add(&system->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    input_add_controllers_for_device(system, keyboard);
    input_add_controllers_for_device(system, pad);
    InputFrame out = {};
    InputSource source = {};

    // A key held when capture starts doesn't count until pressed again.
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    input_capture_begin(system, keyboard);
    frame(system, 0, &out);
    check(!input_capture_poll(system, 0, &source), "capture: a key already held doesn't count");
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 16, &out);
    check(!out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "capture: no hotkeys while capturing");
    check(input_capture_poll(system, 16, &source) && source.type == InputSourceType::key && source.code == HID_F1,
          "capture: the new key (a key counts at once)");
    check(!system->capture.active, "capture: done after one");

    // An axis pushed past half.
    input_devices_set_axis(&system->devices, pad, InputAxis::left_trigger, 0.0f);
    input_capture_begin(system, pad);
    input_devices_set_axis(&system->devices, pad, InputAxis::left_trigger, 0.8f);
    check(!input_capture_poll(system, 1000, &source), "capture: a gamepad's control isn't taken at once");
    check(input_capture_poll(system, 1000 + system->settings.hold_ms, &source) && source.type == InputSourceType::axis_plus &&
              source.code == static_cast<uint16_t>(InputAxis::left_trigger),
          "capture: an axis, held");
    // An axis held when it starts: going back to the center is not the
    // other direction (the d-pad of a pad that reports it as an axis).
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, -1.0f);
    input_capture_begin(system, pad);
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, 0.0f);
    check(!input_capture_poll(system, 5000, &source), "capture: an axis let go is not a push");
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, -1.0f);
    input_capture_poll(system, 5000, &source);
    check(input_capture_poll(system, 5000 + system->settings.hold_ms, &source) && source.type == InputSourceType::axis_minus,
          "capture: pushed again after letting go");
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, 0.0f);
    input_capture_begin(system, pad);
    input_capture_cancel(system);
    check(!input_capture_poll(system, 9000, &source), "capture: cancelled");

    // A gamepad's button is taken once held 2 s (let go sooner, it isn't);
    // still held when the capture ends, it does nothing (BACK mustn't close
    // the window it was bound in) until pressed again.
    system->settings.ui_active = true;
    system->ports[0].clear();
    input_port_add(system, 0, 1);   // the gamepad drives the interface
    uint32_t back = static_cast<uint32_t>(InputAction::ui_back);
    input_capture_begin(system, pad);
    frame(system, 10000, &out);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), true);
    frame(system, 10016, &out);
    check(!input_capture_poll(system, 10016, &source), "capture: held, not taken yet");
    float half = input_capture_hold_progress(system, 10016 + system->settings.hold_ms / 2);
    check(half > 0.4f && half < 0.6f, "capture: the hold's progress");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), false);
    frame(system, 11000, &out);
    check(!input_capture_poll(system, 11000, &source) && input_capture_hold_progress(system, 11000) == 0.0f,
          "capture: let go too soon, not taken");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), true);
    frame(system, 12000, &out);
    input_capture_poll(system, 12000, &source);
    check(input_capture_poll(system, 12000 + system->settings.hold_ms, &source) && source.type == InputSourceType::button &&
              source.code == static_cast<uint16_t>(InputButton::east),
          "capture: a button held 2 s");
    frame(system, 14100, &out);
    frame(system, 14200, &out);
    check(!out.gamepad_held[back], "capture: the button still held after it does nothing");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), false);
    frame(system, 14300, &out);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), true);
    frame(system, 14400, &out);
    check(out.gamepad_held[back], "capture: pressed again, it works");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), false);
    frame(system, 14500, &out);

    system->settings.ui_active = false;

    // Removing a keyboard part renumbers the ports after it.
    InputBindingSet empty = {};
    uint32_t part = input_add_controller(system, keyboard, "Keyboard 2", &empty);
    input_leave_all(system);
    input_port_add(system, 0, part);
    input_port_add(system, 1, 1);   // the gamepad
    input_port_add(system, 1, 0);   // the first keyboard controller, removed next
    input_remove_controller(system, 0);
    check(input_port_has(system, 0, part - 1) && system->ports[1].size() == 1 && input_port_has(system, 1, 0),
          "remove: off its ports, the others renumbered");

    // Ignoring by hand, and using a fake one anyway.
    InputDevice* device = input_devices_find(&system->devices, pad);
    device->ignore = InputIgnore::yes;
    check(input_devices_ignored(device), "ignore: by hand");
    device->ignore = InputIgnore::no;
    device->axes_pinned_at_start = true;
    check(!input_devices_ignored(device), "ignore: a fake one used anyway");
    delete system;
}

static void test_core_profiles()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    input_add_controllers_for_device(system, pad);
    input_join(system, 0);
    InputFrame out = {};

    // A custom profile for snes9x's device 257 on this pad: B on the top button.
    InputCoreProfile* profile = input_core_profile_for(system, "snes9x", "", 257, "pad-guid");
    profile->decided = true;
    profile->custom = true;
    InputBindingOverride change = {};
    change.action = InputAction::pad_b;
    change.sources[0] = button(InputButton::north);
    profile->overrides.push_back(change);

    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::north), true);
    frame(system, 0, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) == 0, "core profile: not used without that core running");
    system->core_name = "snes9x";
    system->port_devices[0] = 257;
    frame(system, 16, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) != 0, "core profile: used for that core and device");
    system->port_devices[0] = 1;
    frame(system, 32, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) == 0, "core profile: not for another device");
    system->port_devices[0] = 257;
    profile->custom = false;
    frame(system, 48, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) == 0, "core profile: kept the RetroPad");
    profile->custom = true;

    // Written and read back.
    SettingsFile file = {};
    input_config_write(system, &file);
    std::string text;
    settings_file_to_text(&file, nullptr, &text);
    check(text.find("[core:snes9x/257/pad-guid]") != std::string::npos && text.find("remap = custom") != std::string::npos &&
              text.find("pad.b = button:north") != std::string::npos,
          "core profile: written");
    SettingsFile parsed = {};
    settings_file_parse(&parsed, text.c_str(), text.size(), "test");
    InputSystem* back = new InputSystem{};
    input_config_read(back, &parsed);
    const InputCoreProfile* read = input_core_profile(back, "snes9x", "", 257, "pad-guid");
    check(read != nullptr && read->decided && read->custom && read->overrides.size() == 1 &&
              read->overrides[0].action == InputAction::pad_b,
          "core profile: read back");
    delete system;
    delete back;
}

// Combos (per core, bound in a remap) and a game's own remap over the core's.
static void test_combos_and_game_remaps()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    input_add_controller(system, pad, "Pad", &pad_set);
    input_join(system, 0);
    system->core_name = "fbneo";
    InputFrame out = {};
    constexpr uint32_t PAD_BIT_Y = 1u << 1, PAD_BIT_X = 1u << 9, PAD_BIT_L = 1u << 10;

    // THREE PUNCHES on the core, bound to R1 in the core's remap (not custom: combos work anyway).
    std::string id = input_combo_add(system, "fbneo", "THREE PUNCHES", static_cast<uint16_t>(PAD_BIT_Y | PAD_BIT_X | PAD_BIT_L))->id;
    check(id == "c1" && input_combo_add(system, "fbneo", "", static_cast<uint16_t>(PAD_BIT_A))->id == "c2" &&
              input_combo_add(system, "snes9x", "", static_cast<uint16_t>(PAD_BIT_A))->id == "c1",
          "combos: ids per core");
    InputCoreProfile* core = input_core_profile_for(system, "fbneo", "", 1, "pad-guid");
    InputComboBinding binding = {};
    binding.combo = id;
    binding.sources[0] = button(InputButton::right_shoulder);
    core->combos.push_back(binding);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::right_shoulder), true);
    frame(system, 0, &out);
    check(out.pad_buttons[0] == (PAD_BIT_Y | PAD_BIT_X | PAD_BIT_L), "combos: one input presses all its buttons");
    system->core_name = "snes9x";
    frame(system, 16, &out);
    check(out.pad_buttons[0] == 0, "combos: only for its core");
    system->core_name = "fbneo";

    // sf2's own remap (a copy, then changed): used in sf2 only.
    system->game_name = "sf2";
    InputCoreProfile copy = *core;
    copy.game = "sf2";
    copy.combos.clear();
    system->core_profiles.push_back(copy);
    frame(system, 32, &out);
    check(out.pad_buttons[0] == 0, "game remap: the game's, without the combo");
    system->game_name = "kof98";
    frame(system, 48, &out);
    check(out.pad_buttons[0] == (PAD_BIT_Y | PAD_BIT_X | PAD_BIT_L), "game remap: other games use the core's");

    // Written and read back.
    SettingsFile file = {};
    input_config_write(system, &file);
    std::string text;
    settings_file_to_text(&file, nullptr, &text);
    check(text.find("[combos:fbneo]") != std::string::npos && text.find("c1 = pad.y+pad.x+pad.l | THREE PUNCHES") != std::string::npos &&
              text.find("[game:fbneo/1/pad-guid/sf2]") != std::string::npos &&
              text.find("combo.c1 = button:right_shoulder") != std::string::npos,
          "combos: written");
    SettingsFile parsed = {};
    settings_file_parse(&parsed, text.c_str(), text.size(), "test");
    InputSystem* back = new InputSystem{};
    input_config_read(back, &parsed);
    const InputCombo* combo = input_combo(back, "fbneo", "c1");
    check(combo != nullptr && combo->name == "THREE PUNCHES" && combo->buttons == (PAD_BIT_Y | PAD_BIT_X | PAD_BIT_L) &&
              back->combos.size() == 3,
          "combos: read back");
    const InputCoreProfile* read = input_core_profile(back, "fbneo", "", 1, "pad-guid");
    check(read != nullptr && read->combos.size() == 1 && read->combos[0].combo == "c1" &&
              input_core_profile(back, "fbneo", "sf2", 1, "pad-guid") != nullptr,
          "game remap: read back");

    // Removing a combo unbinds it everywhere.
    input_combo_remove(system, "fbneo", "c1");
    core = input_core_profile_for(system, "fbneo", "", 1, "pad-guid");   // (the list grew since: look it up again)
    check(input_combo(system, "fbneo", "c1") == nullptr && core->combos.empty(), "combos: removed and unbound");
    delete system;
    delete back;
}

// Macros: steps over frames, once or again while held, written and read back.
static void test_macros()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    input_add_controller(system, pad, "Pad", &pad_set);
    input_join(system, 0);
    system->core_name = "fbneo";
    InputFrame out = {};
    constexpr uint32_t DOWN = 1u << 5, RIGHT = 1u << 7;

    InputMacro* macro = input_macro_add(system, "fbneo", "HADOUKEN");
    InputMacroStep steps[4] = {{static_cast<uint16_t>(DOWN), 2}, {static_cast<uint16_t>(DOWN | RIGHT), 2},
                               {static_cast<uint16_t>(RIGHT | PAD_BIT_A), 2}, {0, 1}};
    for (int i = 0; i < 4; i++)
    {
        macro->steps.push_back(steps[i]);
    }
    std::string id = macro->id;
    check(id == "m1" && input_macro_frames(macro) == 7, "macros: id and length");
    InputCoreProfile* core = input_core_profile_for(system, "fbneo", "", 1, "pad-guid");
    InputComboBinding binding = {};
    binding.combo = id;
    binding.sources[0] = button(InputButton::north);
    core->combos.push_back(binding);

    // Pressed once (and let go at once): the whole sequence plays.
    uint16_t seen[9] = {};
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::north), true);
    for (int f = 0; f < 9; f++)
    {
        frame(system, static_cast<uint64_t>(f) * 16, &out);
        seen[f] = out.pad_buttons[0];
        input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::north), false);
    }
    check(seen[0] == DOWN && seen[1] == DOWN && seen[2] == (DOWN | RIGHT) && seen[4] == (RIGHT | PAD_BIT_A) &&
              seen[6] == 0 && seen[7] == 0 && seen[8] == 0,
          "macros: plays its steps once");

    // Repeat: again while held, not after.
    system->macros[0].repeat = true;
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::north), true);
    for (int f = 0; f < 9; f++)
    {
        frame(system, 200 + static_cast<uint64_t>(f) * 16, &out);
        seen[f] = out.pad_buttons[0];
    }
    check(seen[7] == DOWN && seen[8] == DOWN, "macros: repeat plays again while held");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::north), false);
    for (int f = 0; f < 9; f++)
    {
        frame(system, 400 + static_cast<uint64_t>(f) * 16, &out);
    }
    check(out.pad_buttons[0] == 0, "macros: ...and stops at the end once let go");

    // Written and read back.
    SettingsFile file = {};
    input_config_write(system, &file);
    std::string text;
    settings_file_to_text(&file, nullptr, &text);
    check(text.find("m1 = pad.down:2 pad.down+pad.right:2 pad.right+pad.a:2 -:1 | repeat | HADOUKEN") != std::string::npos &&
              text.find("macro.m1 = button:north") != std::string::npos,
          "macros: written");
    SettingsFile parsed = {};
    settings_file_parse(&parsed, text.c_str(), text.size(), "test");
    InputSystem* back = new InputSystem{};
    input_config_read(back, &parsed);
    const InputMacro* read = input_macro(back, "fbneo", "m1");
    check(read != nullptr && read->repeat && read->name == "HADOUKEN" && read->steps.size() == 4 && read->steps[3].buttons == 0 &&
              read->steps[1].buttons == (DOWN | RIGHT) && input_macro_frames(read) == 7,
          "macros: read back");
    const InputCoreProfile* read_core = input_core_profile(back, "fbneo", "", 1, "pad-guid");
    check(read_core != nullptr && read_core->combos.size() == 1 && read_core->combos[0].combo == "m1", "macros: binding read back");

    // The controller's RetroPad as a recording sees it (no layers).
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::east), true);
    check(input_controller_pad_buttons(system, 0, 0) == PAD_BIT_A, "macros: what a recording takes");
    input_macro_remove(system, "fbneo", "m1");
    core = input_core_profile_for(system, "fbneo", "", 1, "pad-guid");
    check(input_macro(system, "fbneo", "m1") == nullptr && core->combos.empty(), "macros: removed and unbound");
    delete system;
    delete back;
}

// Turbo: a remap row's (its speed by double / triple taps) and TURBO
// BUTTON's for the session.
static void test_turbo()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    InputBindingSet pad_set = {};
    gamepad_bindings(&pad_set);
    input_add_controller(system, pad, "Pad", &pad_set);
    input_join(system, 0);
    system->core_name = "nestopia";
    InputFrame out = {};
    uint32_t west = static_cast<uint32_t>(InputButton::west);
    uint32_t east = static_cast<uint32_t>(InputButton::east);
    uint32_t r1 = static_cast<uint32_t>(InputButton::right_shoulder);

    // A on east as it is, and on west with turbo (2 frames); TURBO BUTTON on R.
    InputCoreProfile* profile = input_core_profile_for(system, "nestopia", "", 1, "pad-guid");
    profile->decided = true;
    profile->custom = true;
    InputBindingOverride a = {};
    a.action = InputAction::pad_a;
    a.sources[0] = button(InputButton::east);
    a.sources[1] = button(InputButton::west);
    a.sources[1].turbo = 2;
    profile->overrides.push_back(a);
    InputBindingOverride turbo = {};
    turbo.action = InputAction::pad_turbo;
    turbo.sources[0] = button(InputButton::right_shoulder);
    profile->overrides.push_back(turbo);

    uint64_t now = 0;
    bool pattern[6] = {};
    input_devices_set_code(&system->devices, pad, west, true);
    for (int f = 0; f < 6; f++)
    {
        now += 16;
        frame(system, now, &out);
        pattern[f] = (out.pad_buttons[0] & PAD_BIT_A) != 0;
    }
    check(pattern[0] && pattern[1] && !pattern[2] && !pattern[3] && pattern[4] && pattern[5], "turbo: 2 frames on, 2 off, from the first");
    input_devices_set_code(&system->devices, pad, west, false);
    now += 500;
    frame(system, now, &out);

    // Double tap: faster (1 frame), kept in the remap, saved.
    for (int t = 0; t < 2; t++)
    {
        input_devices_set_code(&system->devices, pad, west, true);
        now += 16;
        frame(system, now, &out);
        input_devices_set_code(&system->devices, pad, west, false);
        now += 16;
        frame(system, now, &out);
    }
    now += INPUT_TURBO_TAP_MS + 16;
    frame(system, now, &out);
    profile = input_core_profile_for(system, "nestopia", "", 1, "pad-guid");
    check(out.profiles_changed && profile->overrides[0].sources[1].turbo == 1, "turbo: a double tap makes it faster");
    for (int t = 0; t < 3; t++)
    {
        input_devices_set_code(&system->devices, pad, west, true);
        now += 16;
        frame(system, now, &out);
        input_devices_set_code(&system->devices, pad, west, false);
        now += 16;
        frame(system, now, &out);
    }
    now += INPUT_TURBO_TAP_MS + 16;
    frame(system, now, &out);
    check(profile->overrides[0].sources[1].turbo == 2, "turbo: a triple tap makes it slower");

    // TURBO BUTTON + east: east's turbo on for the session, the press not passed on.
    input_devices_set_code(&system->devices, pad, r1, true);
    now += 16;
    frame(system, now, &out);
    input_devices_set_code(&system->devices, pad, east, true);
    now += 16;
    frame(system, now, &out);
    check(out.pad_buttons[0] == 0 && system->controllers[0].turbo_session.size() == 1, "turbo button: east's turbo on, not passed on");
    input_devices_set_code(&system->devices, pad, r1, false);
    input_devices_set_code(&system->devices, pad, east, false);
    now += 16;
    frame(system, now, &out);
    input_devices_set_code(&system->devices, pad, east, true);
    for (int f = 0; f < 6; f++)
    {
        now += 16;
        frame(system, now, &out);
        pattern[f] = (out.pad_buttons[0] & PAD_BIT_A) != 0;
    }
    check(pattern[0] && pattern[1] && !pattern[2] && !pattern[3], "turbo button: east now pulses");
    input_devices_set_code(&system->devices, pad, east, false);
    now += 500;
    frame(system, now, &out);
    input_devices_set_code(&system->devices, pad, r1, true);
    input_devices_set_code(&system->devices, pad, east, true);
    now += 16;
    frame(system, now, &out);
    check(system->controllers[0].turbo_session.empty(), "turbo button: again turns it off");
    input_leave_all(system);
    check(system->controllers[0].turbo_session.empty(), "turbo button: a new game forgets it");

    // Text form.
    InputSource parsed = {};
    char text[64] = {};
    InputSource fast = button(InputButton::west);
    fast.turbo = 3;
    check(input_source_format(fast, text, sizeof(text)) && std::strcmp(text, "button:west*3") == 0 &&
              input_source_parse(text, &parsed) && parsed.turbo == 3 && parsed.code == static_cast<uint16_t>(InputButton::west),
          "turbo: written and read as *frames");
    delete system;
}

// The ports' cursors: a mouse (on port 1 by itself; its position when it is
// the only one, else its motion), a stick moving it, the buttons.
static void test_cursor()
{
    InputSystem* system = new InputSystem{};
    system->settings.keyboard_shares_port1 = false;
    system->view_x = 100.0f;
    system->view_y = 50.0f;
    system->view_width = 640.0f;
    system->view_height = 480.0f;
    uint32_t mouse = input_devices_add(&system->devices, InputDeviceKind::mouse, "mouse", "Mouse");
    input_add_controllers_for_device(system, mouse);
    InputFrame out = {};
    frame(system, 0, &out);
    check(input_port_has(system, 0, 0) && input_controller_profile_id(system, 0) == "mouse", "cursor: the mouse is on port 1");
    check(input_controller_label(system, 0) == "Mouse 1", "names: always numbered");

    // The only mouse: its position on the image, its motion, its buttons.
    input_devices_add_motion(&system->devices, mouse, 12.0f, -3.0f);
    input_devices_set_position(&system->devices, mouse, 420.0f, 290.0f);   // the image's centre
    input_devices_set_code(&system->devices, mouse, static_cast<uint32_t>(InputMouseButton::left), true);
    input_devices_wheel(&system->devices, mouse, true);
    frame(system, 16, &out);
    check(out.mouse_dx[0] == 12 && out.mouse_dy[0] == -3 && out.mouse_moved[0], "cursor: a mouse's motion");
    check(out.cursor_x[0] == 0 && out.cursor_y[0] == 0 && !out.cursor_off[0], "cursor: a lone mouse's position");
    check((out.mouse_buttons[0] & (1u << static_cast<uint32_t>(InputFrameMouse::left))) != 0 &&
              (out.mouse_buttons[0] & (1u << static_cast<uint32_t>(InputFrameMouse::wheel_up))) != 0,
          "cursor: click and wheel");
    frame(system, 32, &out);
    check(out.mouse_dx[0] == 0 && (out.mouse_buttons[0] & (1u << static_cast<uint32_t>(InputFrameMouse::wheel_up))) == 0,
          "cursor: motion and wheel last a frame");
    input_devices_add_motion(&system->devices, mouse, 1.0f, 0.0f);
    input_devices_set_position(&system->devices, mouse, 50.0f, 290.0f);   // left of the image
    frame(system, 48, &out);
    check(out.cursor_off[0] && out.cursor_x[0] == -32767, "cursor: off the image (a lightgun's reload aim)");
    input_devices_set_code(&system->devices, mouse, static_cast<uint32_t>(InputMouseButton::left), false);
    input_devices_set_code(&system->devices, mouse, static_cast<uint32_t>(InputMouseButton::right), true);
    frame(system, 64, &out);
    check((out.mouse_buttons[0] & (1u << static_cast<uint32_t>(InputFrameMouse::reload))) != 0 &&
              (out.mouse_buttons[0] & (1u << static_cast<uint32_t>(InputFrameMouse::right))) != 0,
          "cursor: right button is right click and reload");
    input_devices_set_code(&system->devices, mouse, static_cast<uint32_t>(InputMouseButton::right), false);

    // A gamepad on port 2: its right stick moves port 2's cursor, R2 clicks.
    uint32_t pad = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    input_add_controllers_for_device(system, pad);
    input_port_add(system, 1, 1);
    input_devices_set_axis(&system->devices, pad, InputAxis::right_x, 0.5f);   // (a real stick first)
    input_devices_set_axis(&system->devices, pad, InputAxis::right_x, 1.0f);
    input_devices_set_axis(&system->devices, pad, InputAxis::right_trigger, 1.0f);
    for (int f = 0; f < 20; f++)
    {
        frame(system, 100 + static_cast<uint64_t>(f) * 16, &out);
    }
    check(out.cursor_x[1] > 16000 && out.cursor_y[1] == 0 && out.mouse_dx[1] > 0, "cursor: a stick moves it");
    check((out.mouse_buttons[1] & (1u << static_cast<uint32_t>(InputFrameMouse::left))) != 0, "cursor: R2 is the click / trigger");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), true);
    frame(system, 430, &out);
    check((out.mouse_buttons[1] & (1u << static_cast<uint32_t>(InputFrameMouse::gun_start))) != 0, "cursor: the lightgun's START");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), false);
    for (int f = 0; f < 60; f++)
    {
        frame(system, 500 + static_cast<uint64_t>(f) * 16, &out);
    }
    check(out.cursor_x[1] == 32767, "cursor: a stick keeps it on the image");
    check(out.cursor_x[0] == -32767, "cursor: each port its own");
    input_devices_set_axis(&system->devices, pad, InputAxis::right_x, 0.0f);
    input_devices_set_axis(&system->devices, pad, InputAxis::right_trigger, 0.0f);

    // A second mouse: positions are the system's then, motion moves each port's.
    uint32_t second = input_devices_add(&system->devices, InputDeviceKind::mouse, "mouse", "Mouse 2");
    input_add_controllers_for_device(system, second);
    check(second != mouse && system->controllers.size() == 3, "cursor: a second mouse, its own controller");
    check(input_controller_label(system, 2) == "Mouse 2" && input_controller_label(system, 1) == "Pad 1", "names: the second of a name");
    input_leave(system, 2);
    input_port_add(system, 2, 2);
    input_devices_add_motion(&system->devices, second, 32.0f, 0.0f);
    input_devices_set_position(&system->devices, second, 420.0f, 290.0f);
    frame(system, 2000, &out);
    check(out.cursor_x[2] == 3276 && out.mouse_dx[2] == 32, "cursor: with two mice, motion moves each");

    // Text form; a mouse button is taken once held (a click may be for the window).
    InputSource parsed = {};
    check(input_source_parse("mouse:wheel_down", &parsed) && parsed.type == InputSourceType::mouse &&
              parsed.code == static_cast<uint16_t>(InputMouseButton::wheel_down),
          "cursor: mouse sources as text");
    input_capture_begin(system, mouse);
    input_devices_set_code(&system->devices, mouse, static_cast<uint32_t>(InputMouseButton::middle), true);
    InputSource taken = {};
    check(!input_capture_poll(system, 3000, &taken), "cursor: a click isn't taken at once");
    check(input_capture_poll(system, 3000 + system->settings.hold_ms, &taken) && taken.type == InputSourceType::mouse,
          "cursor: ...held, it is");
    delete system;
}

static void test_layouts()
{
    InputLayouts layouts = {};
    static const char GENESIS[] =
        "[layout]\nname = GENESIS\norder = 40\nsdl_types = segagenesis\ndpad = yes\nsticks = 0\nface = six\n"
        "[labels]\na = A\nb = B\nrightshoulder = C\nx = X\ny = Y\nleftshoulder = Z\nstart = START\n";
    SettingsFile file = {};
    settings_file_parse(&file, GENESIS, sizeof(GENESIS) - 1, "test");
    input_layouts_add(&layouts, "Sega", "genesis6", &file);
    input_layouts_ensure_generic(&layouts);
    check(layouts.layouts.size() == 2 && layouts.layouts[0].id == "Generic/generic", "layouts: generic built in, sorted first");
    std::vector<std::string> makers;
    input_layouts_makers(&layouts, &makers);
    check(makers.size() == 2 && makers[0] == "Generic" && makers[1] == "Sega", "layouts: makers in order");
    const InputLayout* genesis = input_layouts_find(&layouts, "Sega/genesis6");
    check(genesis != nullptr && genesis->name == "GENESIS", "layouts: read by id");
    check(input_layouts_for_type(&layouts, "segagenesis") == genesis, "layouts: picked by SDL type");
    check(genesis != nullptr && input_layout_has(genesis, InputMapControl::right_shoulder) &&
              genesis->labels[static_cast<uint32_t>(InputMapControl::right_shoulder)] == "C",
          "layouts: a labelled button is on the pad");
    check(genesis != nullptr && !input_layout_has(genesis, InputMapControl::left_trigger), "layouts: an unlisted button isn't");
    check(genesis != nullptr && !input_layout_has(genesis, InputMapControl::left_x) && input_layout_has(genesis, InputMapControl::dpad_up),
          "layouts: no sticks, a d-pad");
    check(genesis != nullptr && genesis->six_face && !layouts.layouts[0].six_face, "layouts: a six-button face");
    check(!input_layout_has(&layouts.layouts[0], InputMapControl::right_paddle1), "layouts: extras aren't on a layout");
}

static void test_pad_positions()
{
    // Where a source is on the drawn pad.
    check(input_map_control_for_source(button(InputButton::south)) == InputMapControl::south, "pad: bottom button");
    check(input_map_control_for_source(button(InputButton::guide)) == InputMapControl::guide, "pad: guide");
    check(input_map_control_for_source(button(InputButton::touchpad)) == InputMapControl::count, "pad: not drawn");
    check(input_map_control_for_source(button(InputButton::left_paddle1)) == InputMapControl::left_paddle1 &&
              input_map_control_for_source(button(InputButton::misc5)) == InputMapControl::l4,
          "pad: the extras");
    check(!input_map_control_is_stick(InputMapControl::right_paddle1), "pad: an extra isn't a stick");
    check(std::strcmp(input_map_control_sdl_name(InputMapControl::left_paddle1), "paddle2") == 0 &&
              static_cast<uint32_t>(InputMapControl::left_paddle1) < static_cast<uint32_t>(InputMapControl::right_paddle1) &&
              std::strcmp(input_map_control_sdl_name(InputMapControl::l4), "misc5") == 0,
          "pad: the paddles asked left first (SDL's paddle2), L4 on misc5");
    InputSource up = {};
    up.type = InputSourceType::axis_minus;
    up.code = static_cast<uint16_t>(InputAxis::left_y);
    check(input_map_control_for_source(up) == InputMapControl::left_y, "pad: stick pushed up");
    InputSource trigger = {};
    trigger.type = InputSourceType::axis_plus;
    trigger.code = static_cast<uint16_t>(InputAxis::right_trigger);
    check(input_map_control_for_source(trigger) == InputMapControl::right_trigger, "pad: trigger");
    InputSource key = {};
    key.type = InputSourceType::key;
    key.code = HID_F1;
    check(input_map_control_for_source(key) == InputMapControl::count, "pad: a key isn't on it");
    check(input_map_control_is_stick(InputMapControl::right_y) && !input_map_control_is_stick(InputMapControl::count),
          "pad: sticks");
}

int main()
{
    test_sources();
    test_keyboard_join_and_play();
    test_split_keyboard();
    test_shared_port_and_reconnects();
    test_gamepad_hotkeys();
    test_combo_and_stick();
    test_shared_ports();
    test_fake_gamepad();
    test_menus_and_defaults();
    test_config();
    test_capture_and_remove();
    test_core_profiles();
    test_combos_and_game_remaps();
    test_macros();
    test_turbo();
    test_cursor();
    test_layouts();
    test_pad_positions();
    if (g_failures == 0)
    {
        std::printf("input: all checks passed\n");
        return 0;
    }
    std::printf("input: %d check(s) failed\n", g_failures);
    return 1;
}

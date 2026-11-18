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
    check(out.joined_port == 0 && system->ports[0] == 0, "keyboard: START joins port 1");
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
    check(system->ports[0] == 0 && system->ports[1] == 1, "split keyboard: two controllers on two ports");
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

    // The keyboard plays on port 1 without joining.
    check(input_controller_shares_port1(system, keyboard_controller), "shared: the keyboard shares port 1");
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 0, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) != 0 && system->ports[0] == INPUT_NO_PORT, "shared: keyboard on port 1, port still free");
    input_devices_set_code(&system->devices, keyboard, HID_Z, false);

    // A gamepad does nothing until START; then it's on port 1 too.
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::south), true);
    frame(system, 16, &out);
    check(out.pad_buttons[0] == 0, "shared: a gamepad that hasn't joined does nothing");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::south), false);
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), true);
    frame(system, 32, &out);
    check(out.joined_port == 0 && system->ports[0] == static_cast<int32_t>(pad_controller), "shared: the gamepad joins port 1");
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
    check(system->ports[0] == INPUT_NO_PORT && out.left_port == 0 && out.left_controller == static_cast<int32_t>(pad_controller),
          "reconnect: unplugging frees the port and says so");
    uint32_t again = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    check(again == pad, "reconnect: same device id");
    frame(system, 96, &out);
    check(system->ports[0] == static_cast<int32_t>(pad_controller), "reconnect: back on its port");

    // Without sharing, the keyboard needs START.
    system->settings.keyboard_shares_port1 = false;
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 112, &out);
    check((out.pad_buttons[0] & PAD_BIT_B) == 0, "not shared: the keyboard plays only after joining");
    input_devices_set_code(&system->devices, keyboard, HID_Z, false);
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    frame(system, 128, &out);
    check(system->ports[1] == static_cast<int32_t>(keyboard_controller), "not shared: START puts it on the next free port");

    input_swap_ports(system, 0, 1);
    check(system->ports[0] == static_cast<int32_t>(keyboard_controller), "swap: ports reordered");
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
    check(system->ports[0] == INPUT_NO_PORT && input_first_controller(system, keyboard) == static_cast<int32_t>(controller),
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
    system->ports[0] = 1;   // the gamepad drives the interface
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
    system->ports[0] = static_cast<int32_t>(part);
    system->ports[1] = 1;   // the gamepad
    input_remove_controller(system, 0);   // the first keyboard controller
    check(system->ports[0] == static_cast<int32_t>(part) - 1 && system->ports[1] == 0, "remove: ports renumbered");

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
    InputCoreProfile* profile = input_core_profile_for(system, "snes9x", 257, "pad-guid");
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
    const InputCoreProfile* read = input_core_profile(back, "snes9x", 257, "pad-guid");
    check(read != nullptr && read->decided && read->custom && read->overrides.size() == 1 &&
              read->overrides[0].action == InputAction::pad_b,
          "core profile: read back");
    delete system;
    delete back;
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
    test_fake_gamepad();
    test_menus_and_defaults();
    test_config();
    test_capture_and_remove();
    test_core_profiles();
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

// Checks the neutral input layer (input_devices, input_bindings) with no
// hardware: sources and their text form, joining ports, a gamepad taking the
// keyboard's port, devices leaving and coming back, hotkeys (keyboard rules,
// HOLD, DOUBLE TAP), combos, the stick deadzone, short taps and fake gamepads.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>

#include "input_bindings.h"
#include "input_devices.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

constexpr uint32_t HID_Z = 29, HID_X = 27, HID_RETURN = 40, HID_F1 = 58, HID_F2 = 59, HID_UP = 82;
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

    InputAction action = InputAction::count;
    check(input_action_parse("hotkey.quick_menu", &action) && action == InputAction::hotkey_quick_menu, "actions: by name");
    check(std::strcmp(input_action_name(InputAction::pad_l2), "pad.l2") == 0, "actions: name");
}

static void test_keyboard_join_and_play()
{
    InputSystem* system = new InputSystem{};
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

    // Keyboard hotkeys work without HOTKEY ENABLE.
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 96, &out);
    check(out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "keyboard: F1 works alone");
    input_devices_set_code(&system->devices, keyboard, HID_F1, false);
    frame(system, 112, &out);

    // ...not when the core takes the keyboard.
    system->settings.core_takes_keyboard = true;
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 128, &out);
    check(!out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "keyboard: F1 needs enable when the core takes the keyboard");
    input_devices_set_code(&system->devices, keyboard, HID_F1, false);
    system->settings.core_takes_keyboard = false;
    frame(system, 144, &out);

    // ...not when the key is also on the RetroPad.
    InputSource f2 = {};
    f2.type = InputSourceType::key;
    f2.code = HID_F2;
    input_bind(&system->controllers[0].bindings, InputAction::pad_x, f2);
    input_devices_set_code(&system->devices, keyboard, HID_F2, true);
    frame(system, 160, &out);
    check(!out.pressed[static_cast<uint32_t>(InputAction::hotkey_save_state)], "keyboard: a key on the RetroPad isn't a hotkey");
    delete system;
}

static void test_split_keyboard()
{
    InputSystem* system = new InputSystem{};
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

static void test_gamepad_takes_port_and_reconnects()
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

    check(input_join(system, keyboard_controller), "takeover: keyboard joined directly");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), true);
    frame(system, 0, &out);
    check(system->ports[0] == static_cast<int32_t>(pad_controller), "takeover: the gamepad takes port 1");
    check(input_controller_port(system, keyboard_controller) == INPUT_NO_PORT, "takeover: the keyboard left");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), false);
    frame(system, 16, &out);

    // Without the setting the gamepad goes to the next free port.
    input_leave(system, pad_controller);
    input_join(system, keyboard_controller);
    system->settings.gamepad_takes_keyboard_port = false;
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), true);
    frame(system, 32, &out);
    check(system->ports[0] == static_cast<int32_t>(keyboard_controller) && system->ports[1] == static_cast<int32_t>(pad_controller),
          "takeover off: the gamepad joins port 2");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::start), false);
    frame(system, 48, &out);

    // Unplugged: port freed; plugged back: same port, same id.
    input_devices_disconnect(&system->devices, pad);
    frame(system, 64, &out);
    check(system->ports[1] == INPUT_NO_PORT, "reconnect: unplugging frees the port");
    uint32_t again = input_devices_add(&system->devices, InputDeviceKind::gamepad, "pad-guid", "Pad");
    check(again == pad, "reconnect: same device id");
    frame(system, 80, &out);
    check(system->ports[1] == static_cast<int32_t>(pad_controller), "reconnect: back on its port");

    input_swap_ports(system, 0, 1);
    check(system->ports[0] == static_cast<int32_t>(pad_controller), "swap: ports reordered");
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
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 216, &out);
    check(out.pressed[quick_menu], "hold: SELECT+START is the quick menu");
    input_devices_set_code(&system->devices, pad, start, false);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 232, &out);

    // DOUBLE TAP: two SELECT taps arm one hotkey.
    system->settings.hotkey_mode = InputHotkeyMode::double_tap;
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 1000, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 1050, &out);
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 1100, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 1150, &out);
    check(system->controllers[controller].armed, "double tap: armed");
    check(out.pad_buttons[0] == 0, "double tap: RetroPad quiet while armed");
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 1200, &out);
    check(out.pressed[quick_menu], "double tap: START fires the hotkey");
    check(!system->controllers[controller].armed, "double tap: back to normal after one");
    input_devices_set_code(&system->devices, pad, start, false);
    frame(system, 1216, &out);
    input_devices_set_code(&system->devices, pad, start, true);
    frame(system, 1232, &out);
    check(!out.pressed[quick_menu] && (out.pad_buttons[0] & PAD_BIT_START) != 0, "double tap: START is the game's again");
    input_devices_set_code(&system->devices, pad, start, false);

    // Slow taps don't arm; arming times out.
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 3000, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 3100, &out);
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 3500, &out);
    check(!system->controllers[controller].armed, "double tap: slow taps don't arm");
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 3550, &out);
    input_devices_set_code(&system->devices, pad, back, true);
    frame(system, 3600, &out);
    input_devices_set_code(&system->devices, pad, back, false);
    frame(system, 3650, &out);
    check(system->controllers[controller].armed, "double tap: armed again");
    frame(system, 7000, &out);
    check(!system->controllers[controller].armed, "double tap: times out");
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

int main()
{
    test_sources();
    test_keyboard_join_and_play();
    test_split_keyboard();
    test_gamepad_takes_port_and_reconnects();
    test_gamepad_hotkeys();
    test_combo_and_stick();
    test_fake_gamepad();
    if (g_failures == 0)
    {
        std::printf("input: all checks passed\n");
        return 0;
    }
    std::printf("input: %d check(s) failed\n", g_failures);
    return 1;
}

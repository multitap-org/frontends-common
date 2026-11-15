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
    system->settings.game_has_input = false;
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::dpad_down), true);
    frame(system, 64, &out);
    check(!out.gamepad_held[static_cast<uint32_t>(InputAction::ui_down)], "ui: a gamepad without ui bindings doesn't drive it");
    input_devices_set_code(&system->devices, pad, static_cast<uint32_t>(InputButton::dpad_down), false);
    system->settings.game_has_input = true;
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
    system->settings.game_has_input = false;
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, true);
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 0, &out);
    check(input_controller_port(system, controller) == 0, "menus: START joins with the UI up too");
    check(out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "menus: hotkeys still work");
    input_devices_set_code(&system->devices, keyboard, HID_RETURN, false);
    input_devices_set_code(&system->devices, keyboard, HID_Z, true);
    frame(system, 16, &out);
    check(out.pad_buttons[0] == 0, "menus: the keyboard doesn't play");
    system->settings.game_has_input = true;
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

    // The gamepad defaults: B bottom, A right, R2 on the trigger, SELECT+START quick menu.
    InputBindingSet pad_set = {};
    input_default_gamepad_bindings(&pad_set);
    check(input_is_bound(&pad_set, InputAction::pad_b, button(InputButton::south)), "defaults: B is the bottom button");
    check(input_is_bound(&pad_set, InputAction::pad_a, button(InputButton::east)), "defaults: A is the right button");
    check(input_is_bound(&pad_set, InputAction::pad_r2, axis(InputAxis::right_trigger, true)), "defaults: R2 is the trigger");
    check(input_is_bound(&pad_set, InputAction::hotkey_enable, button(InputButton::back)), "defaults: HOTKEY ENABLE is SELECT");
    check(input_is_bound(&pad_set, InputAction::hotkey_quick_menu, button(InputButton::start)), "defaults: + START = quick menu");
    check(input_is_bound(&pad_set, InputAction::ui_open_menu, button(InputButton::start)), "defaults: START opens the menu bar");
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
    check(!input_capture_poll(system, &source), "capture: a key already held doesn't count");
    input_devices_set_code(&system->devices, keyboard, HID_F1, true);
    frame(system, 16, &out);
    check(!out.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)], "capture: no hotkeys while capturing");
    check(input_capture_poll(system, &source) && source.type == InputSourceType::key && source.code == HID_F1,
          "capture: the new key");
    check(!system->capture.active, "capture: done after one");

    // An axis pushed past half.
    input_devices_set_axis(&system->devices, pad, InputAxis::left_trigger, 0.0f);
    input_capture_begin(system, pad);
    input_devices_set_axis(&system->devices, pad, InputAxis::left_trigger, 0.8f);
    check(input_capture_poll(system, &source) && source.type == InputSourceType::axis_plus &&
              source.code == static_cast<uint16_t>(InputAxis::left_trigger),
          "capture: an axis");
    // An axis held when it starts: going back to the center is not the
    // other direction (the d-pad of a pad that reports it as an axis).
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, -1.0f);
    input_capture_begin(system, pad);
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, 0.0f);
    check(!input_capture_poll(system, &source), "capture: an axis let go is not a push");
    input_devices_set_axis(&system->devices, pad, InputAxis::left_x, -1.0f);
    check(input_capture_poll(system, &source) && source.type == InputSourceType::axis_minus,
          "capture: pushed again after letting go");
    input_capture_begin(system, pad);
    input_capture_cancel(system);
    check(!input_capture_poll(system, &source), "capture: cancelled");

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
    if (g_failures == 0)
    {
        std::printf("input: all checks passed\n");
        return 0;
    }
    std::printf("input: %d check(s) failed\n", g_failures);
    return 1;
}

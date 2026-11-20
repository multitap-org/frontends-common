// Checks remote pads (net_input packets, input_net into the router):
//   - packets round-trip, newest-first on the wire, oldest-first when read;
//     malformed ones are refused;
//   - a peer's first pad packet plugs in a gamepad with a guid from its key
//     and a controller for it; held buttons and axes arrive;
//   - a tap (press and release between two delivered packets, the packet in
//     between lost) still reaches the router as a tap;
//   - an old packet arriving late changes nothing;
//   - the peer leaving unplugs the pad; coming back reuses the same device;
//   - a remote pad only plays: on port 1 with the UI up it drives nothing
//     (a local pad on port 1 does); in a game its HOTKEY ENABLE + R fires
//     SAVE STATE only once its device may (STREAM: input_net_set_hotkeys),
//     and + START (QUICK MENU) and + Y (MENU) fire nothing.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>

#include "input_bindings.h"
#include "input_net.h"
#include "net_input.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static uint32_t bit(InputButton button)
{
    return 1u << static_cast<uint32_t>(button);
}

int main()
{
    // Packets.
    NetPadSender sender;
    uint8_t packet[NET_INPUT_MAX_PACKET];
    NetPadState state;
    state.buttons = bit(InputButton::south);
    state.axes[0] = 16000;
    size_t size = net_input_push(&sender, 1, &state, packet, sizeof(packet));
    check(size == 3 + NET_INPUT_STATE_SIZE, "first packet holds one state");
    for (int i = 0; i < 5; i++)
    {
        size = net_input_push(&sender, 1, &state, packet, sizeof(packet));
    }
    check(size == NET_INPUT_MAX_PACKET, "later packets hold the full history");
    uint8_t pad = 0;
    NetPadState read[NET_INPUT_HISTORY];
    uint32_t count = 0;
    check(net_input_read(packet, size, &pad, read, &count) && pad == 1 && count == NET_INPUT_HISTORY, "packet read");
    check(read[0].frame == 3 && read[3].frame == 6, "states come out oldest first");
    check(read[3].buttons == bit(InputButton::south) && read[3].axes[0] == 16000, "state fields");
    check(!net_input_read(packet, size - 1, &pad, read, &count), "short packet refused");
    packet[1] = NET_INPUT_MAX_PADS;
    check(!net_input_read(packet, size, &pad, read, &count), "bad pad index refused");

    // Into the router.
    InputSystem* system = new InputSystem{};
    InputNet* net = new InputNet{};
    uint8_t key[32];
    for (uint32_t i = 0; i < 32; i++)
    {
        key[i] = static_cast<uint8_t>(0xA0 + i);
    }
    NetPadSender phone;
    NetPadState held;
    held.buttons = bit(InputButton::start) | bit(InputButton::dpad_left);
    held.axes[static_cast<uint32_t>(InputAxis::left_x)] = -32767;
    size = net_input_push(&phone, 0, &held, packet, sizeof(packet));
    input_net_receive(net, system, 2, key, "ANA'S PHONE", packet, size);
    check(net->pads[2][0].active, "pad plugged in");
    uint32_t device_id = net->pads[2][0].device;
    const InputDevice* device = input_devices_find_const(&system->devices, device_id);
    check(device != nullptr && device->kind == InputDeviceKind::gamepad && device->connected, "it's a connected gamepad");
    check(device != nullptr && device->guid == "net:a0a1a2a3a4a5a6a7:0", "guid from the device key");
    check(device != nullptr && device->name == "ANA'S PHONE", "named after the device");
    check(input_first_controller(system, device_id) >= 0, "it got a controller");
    check(device != nullptr && device->held[static_cast<uint32_t>(InputButton::start)] &&
              device->held[static_cast<uint32_t>(InputButton::dpad_left)],
          "held buttons arrived");
    check(device != nullptr && device->axes[static_cast<uint32_t>(InputAxis::left_x)] < -0.99f, "axis arrived");

    // A tap: press A in one state, let go in the next; the packet with the
    // press is lost, the next one (which repeats it) arrives.
    input_devices_end_frame(&system->devices);
    NetPadState press = held;
    press.buttons |= bit(InputButton::south);
    uint8_t lost[NET_INPUT_MAX_PACKET];
    size_t lost_size = net_input_push(&phone, 0, &press, lost, sizeof(lost));
    size = net_input_push(&phone, 0, &held, packet, sizeof(packet));
    input_net_receive(net, system, 2, key, "ANA'S PHONE", packet, size);
    device = input_devices_find_const(&system->devices, device_id);
    uint32_t south = static_cast<uint32_t>(InputButton::south);
    check(device != nullptr && !device->held[south] && device->tapped[south], "a tap inside a lost packet still counts");

    // The lost packet arrives late: older than what's applied, nothing changes.
    input_devices_end_frame(&system->devices);
    input_net_receive(net, system, 2, key, "ANA'S PHONE", lost, lost_size);
    device = input_devices_find_const(&system->devices, device_id);
    check(device != nullptr && !device->held[south] && !device->tapped[south], "a late packet changes nothing");

    input_net_receive(net, system, 2, key, "ANA'S PHONE", packet, 2);
    check(net->dropped == 1, "garbage counted, ignored");

    input_net_peer_gone(net, system, 2);
    device = input_devices_find_const(&system->devices, device_id);
    check(device != nullptr && !device->connected && !device->held[static_cast<uint32_t>(InputButton::start)],
          "peer gone: pad unplugged and released");
    check(!net->pads[2][0].active, "slot freed");

    // Back (maybe on another peer slot): the same device comes back.
    NetPadSender again;
    size = net_input_push(&again, 0, &held, packet, sizeof(packet));
    input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
    check(net->pads[5][0].active && net->pads[5][0].device == device_id, "the same device comes back");

    // Only for playing. A local pad and the remote one, both on port 1.
    device = input_devices_find_const(&system->devices, net->pads[5][0].device);
    check(device != nullptr && device->game_only, "remote pads are game-only");
    check(device != nullptr && !input_devices_ignored(device), "a stick held at plug-in doesn't make it look fake");
    uint32_t local = input_devices_add(&system->devices, InputDeviceKind::gamepad, "local-pad", "LOCAL PAD");
    input_add_controllers_for_device(system, local);
    int32_t local_controller = input_first_controller(system, local);
    int32_t remote_controller = input_first_controller(system, net->pads[5][0].device);
    check(local_controller >= 0 && remote_controller >= 0, "both have controllers");
    if (local_controller >= 0 && remote_controller >= 0)
    {
        input_port_add(system, 0, static_cast<uint32_t>(remote_controller));
        system->settings.ui_active = true;
        uint32_t down = static_cast<uint32_t>(InputButton::dpad_down);
        NetPadState pressing;
        pressing.buttons = bit(InputButton::dpad_down);
        size = net_input_push(&again, 0, &pressing, packet, sizeof(packet));
        input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
        InputFrame frame;
        input_update(system, 1000, &frame);
        bool any_ui = false;
        for (uint32_t a = 0; a < INPUT_ACTION_COUNT; a++)
        {
            any_ui = any_ui || frame.held[a] || frame.pressed[a] || frame.gamepad_held[a];
        }
        check(!any_ui, "a remote pad on port 1 doesn't drive the interface");

        // The same press on a local pad on port 1 does.
        input_port_add(system, 0, static_cast<uint32_t>(local_controller));
        input_devices_set_code(&system->devices, local, down, true);
        InputFrame local_frame;
        input_update(system, 1016, &local_frame);
        bool local_ui = false;
        for (uint32_t a = 0; a < INPUT_ACTION_COUNT; a++)
        {
            local_ui = local_ui || local_frame.held[a];
        }
        check(local_ui, "a local pad on port 1 does (the check itself works)");

        // In a game: the local pad leaves port 1, the remote one holds SELECT
        // (HOTKEY ENABLE) and presses R (SAVE STATE), START (QUICK MENU) and
        // Y (MENU) at once.
        input_devices_set_code(&system->devices, local, down, false);
        input_port_remove(system, 0, static_cast<uint32_t>(local_controller));
        system->settings.ui_active = false;
        NetPadState idle;
        size = net_input_push(&again, 0, &idle, packet, sizeof(packet));
        input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
        InputFrame settle;
        input_update(system, 1100, &settle);
        NetPadState enable;
        enable.buttons = bit(InputButton::back);
        size = net_input_push(&again, 0, &enable, packet, sizeof(packet));
        input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
        input_update(system, 1116, &settle);
        NetPadState hotkey = enable;
        hotkey.buttons |= bit(InputButton::right_shoulder) | bit(InputButton::start) | bit(InputButton::west);
        // A GAMEPAD device (hotkeys not allowed: the default) can't.
        size = net_input_push(&again, 0, &hotkey, packet, sizeof(packet));
        input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
        InputFrame denied_frame;
        input_update(system, 1132, &denied_frame);
        check(!denied_frame.pressed[static_cast<uint32_t>(InputAction::hotkey_save_state)],
              "a GAMEPAD device's pad can't SAVE STATE");
        input_devices_end_frame(&system->devices);   // as the frontend does after each update: taps clear
        // STREAM and up: it can (everything let go, then the same again).
        input_net_set_hotkeys(net, system, 5, true);
        size = net_input_push(&again, 0, &idle, packet, sizeof(packet));
        input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
        input_update(system, 1500, &settle);
        input_devices_end_frame(&system->devices);
        size = net_input_push(&again, 0, &enable, packet, sizeof(packet));
        input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
        input_update(system, 1900, &settle);
        input_devices_end_frame(&system->devices);
        size = net_input_push(&again, 0, &hotkey, packet, sizeof(packet));
        input_net_receive(net, system, 5, key, "ANA'S PHONE", packet, size);
        InputFrame hotkey_frame;
        input_update(system, 1916, &hotkey_frame);
        check(hotkey_frame.pressed[static_cast<uint32_t>(InputAction::hotkey_save_state)],
              "a remote pad on port 1 can SAVE STATE");
        check(!hotkey_frame.pressed[static_cast<uint32_t>(InputAction::hotkey_quick_menu)] &&
                  !hotkey_frame.pressed[static_cast<uint32_t>(InputAction::hotkey_menu)],
              "but not open the menus");
    }

    // RetroPad packets (type 2): the phone mapped its controls itself; the
    // router passes them to the port as they are.
    NetRetroPadSender retro;
    NetRetroPadState start_state;
    start_state.buttons = 1u << 3;   // RETRO_DEVICE_ID_JOYPAD_START
    uint8_t retro_packet[3 + NET_INPUT_HISTORY * NET_RETROPAD_STATE_SIZE];
    size_t retro_size = net_input_push_retropad(&retro, 1, &start_state, retro_packet, sizeof(retro_packet));
    uint8_t retro_pad = 0;
    NetRetroPadState retro_read[NET_INPUT_HISTORY];
    uint32_t retro_count = 0;
    check(net_input_read_retropad(retro_packet, retro_size, &retro_pad, retro_read, &retro_count) && retro_pad == 1 &&
              retro_count == 1 && retro_read[0].buttons == (1u << 3),
          "RetroPad packet read");
    check(!net_input_read(retro_packet, retro_size, &pad, read, &count), "a RetroPad packet isn't a pad packet");
    uint8_t key2[32];
    for (uint32_t i = 0; i < 32; i++)
    {
        key2[i] = static_cast<uint8_t>(0x10 + i);
    }
    input_net_receive(net, system, 6, key2, "BEN'S PHONE", retro_packet, retro_size);
    check(net->pads[6][1].active, "RetroPad device plugged in");
    uint32_t retro_device = net->pads[6][1].device;
    device = input_devices_find_const(&system->devices, retro_device);
    check(device != nullptr && device->retropad && device->game_only, "it's a game-only RetroPad device");
    InputFrame join_frame;
    input_update(system, 2000, &join_frame);
    int32_t retro_port = join_frame.joined_port;
    check(retro_port != INPUT_NO_PORT, "START on the RetroPad joins a port");
    input_devices_end_frame(&system->devices);
    if (retro_port != INPUT_NO_PORT)
    {
        uint32_t p = static_cast<uint32_t>(retro_port);
        NetRetroPadState playing;
        playing.buttons = (1u << 0) | (1u << 8);   // B and A
        playing.analog[0][0] = 12345;
        playing.analog[1][1] = -20000;
        retro_size = net_input_push_retropad(&retro, 1, &playing, retro_packet, sizeof(retro_packet));
        input_net_receive(net, system, 6, key2, "BEN'S PHONE", retro_packet, retro_size);
        InputFrame play_frame;
        input_update(system, 2016, &play_frame);
        check(play_frame.pad_buttons[p] == ((1u << 0) | (1u << 8)), "its RetroPad buttons reach the port as they are");
        check(play_frame.analog[p][0][0] == 12345 && play_frame.analog[p][1][1] == -20000, "and its sticks");
        input_devices_end_frame(&system->devices);

        // A tap between two frames: pressed and let go in two states that
        // arrive together.
        NetRetroPadState none;
        NetRetroPadState tap;
        tap.buttons = 1u << 9;   // X
        retro_size = net_input_push_retropad(&retro, 1, &none, retro_packet, sizeof(retro_packet));
        input_net_receive(net, system, 6, key2, "BEN'S PHONE", retro_packet, retro_size);
        InputFrame idle_frame;
        input_update(system, 2032, &idle_frame);
        input_devices_end_frame(&system->devices);
        net_input_push_retropad(&retro, 1, &tap, retro_packet, sizeof(retro_packet));
        retro_size = net_input_push_retropad(&retro, 1, &none, retro_packet, sizeof(retro_packet));
        input_net_receive(net, system, 6, key2, "BEN'S PHONE", retro_packet, retro_size);
        InputFrame tap_frame;
        input_update(system, 2048, &tap_frame);
        check((tap_frame.pad_buttons[p] & (1u << 9)) != 0, "a tap between frames reaches the core for a frame");
        input_devices_end_frame(&system->devices);
        InputFrame after_tap;
        input_update(system, 2064, &after_tap);
        check((after_tap.pad_buttons[p] & (1u << 9)) == 0, "and is let go after it");
    }
    input_net_peer_gone(net, system, 6);
    device = input_devices_find_const(&system->devices, retro_device);
    check(device != nullptr && !device->connected && device->retropad_buttons == 0, "RetroPad device unplugged");

    delete net;
    delete system;
    if (g_failures == 0)
    {
        std::printf("all input_net checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

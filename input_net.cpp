#include "input_net.h"

#include <cstdio>

#include "host_log.h"

static uint32_t pad_device(InputNet* net, InputSystem* system, uint32_t peer, uint8_t pad, const uint8_t* key,
                           const char* name)
{
    InputNetPad* entry = &net->pads[peer][pad];
    if (entry->active)
    {
        return entry->device;
    }
    char guid[48];
    std::snprintf(guid, sizeof(guid), "net:%02x%02x%02x%02x%02x%02x%02x%02x:%u", key[0], key[1], key[2], key[3], key[4],
                  key[5], key[6], key[7], static_cast<unsigned>(pad));
    char label[96];
    if (pad == 0)
    {
        std::snprintf(label, sizeof(label), "%s", name);
    }
    else
    {
        std::snprintf(label, sizeof(label), "%s %u", name, static_cast<unsigned>(pad + 1));
    }
    entry->device = input_devices_add(&system->devices, InputDeviceKind::gamepad, guid, label);
    InputDevice* device = input_devices_find(&system->devices, entry->device);
    if (device != nullptr)
    {
        device->game_only = true;   // plays; never the menus or hotkeys
        // Never "looks fake": a phone's stick may be held at an edge when its
        // first state arrives, which the router would take for a stuck receiver.
        device->ignore = InputIgnore::no;
    }
    entry->active = true;
    entry->last_frame = 0;
    input_add_controllers_for_device(system, entry->device);
    host_log(LogLevel::info, "Network: pad \"%s\" (%s) plugged in", label, guid);
    return entry->device;
}

static void apply(InputSystem* system, uint32_t device, const NetPadState* state)
{
    for (uint32_t b = 0; b < INPUT_BUTTON_COUNT; b++)
    {
        input_devices_set_code(&system->devices, device, b, (state->buttons & (1u << b)) != 0);
    }
    for (uint32_t a = 0; a < NET_INPUT_AXES && a < INPUT_AXIS_COUNT; a++)
    {
        input_devices_set_axis(&system->devices, device, static_cast<InputAxis>(a),
                               static_cast<float>(state->axes[a]) / 32767.0f);
    }
}

void input_net_receive(InputNet* net, InputSystem* system, uint32_t peer, const uint8_t* key, const char* name,
                       const uint8_t* data, size_t size)
{
    uint8_t pad = 0;
    NetPadState states[NET_INPUT_HISTORY];
    uint32_t count = 0;
    if (peer >= NET_MAX_PEERS || !net_input_read(data, size, &pad, states, &count))
    {
        net->dropped++;
        return;
    }
    uint32_t device = pad_device(net, system, peer, pad, key, name);
    InputNetPad* entry = &net->pads[peer][pad];
    // Oldest first, only what's new: a press and release that came in one
    // packet (the one before it was lost) both happen, and the router counts
    // the tap.
    for (uint32_t i = 0; i < count; i++)
    {
        if (states[i].frame > entry->last_frame)
        {
            apply(system, device, &states[i]);
            entry->last_frame = states[i].frame;
        }
    }
}

void input_net_peer_gone(InputNet* net, InputSystem* system, uint32_t peer)
{
    if (peer >= NET_MAX_PEERS)
    {
        return;
    }
    for (uint32_t pad = 0; pad < NET_INPUT_MAX_PADS; pad++)
    {
        InputNetPad* entry = &net->pads[peer][pad];
        if (entry->active)
        {
            input_devices_release_all(&system->devices, entry->device);
            input_devices_disconnect(&system->devices, entry->device);
            host_log(LogLevel::info, "Network: pad unplugged");
        }
        *entry = {};
    }
}

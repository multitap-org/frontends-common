#include "input_sdl.h"

#include <SDL3/SDL.h>

#include <cstdio>

// Libretro rumble is a state ("this strong until changed"), SDL's has a
// duration: long enough to last until the core changes it.
constexpr uint32_t RUMBLE_DURATION_MS = 5000;

static InputSdlGamepad* find_gamepad(InputSdl* sdl, SDL_JoystickID id)
{
    for (size_t i = 0; i < sdl->gamepads.size(); i++)
    {
        if (sdl->gamepads[i].joystick_id == id)
        {
            return &sdl->gamepads[i];
        }
    }
    return nullptr;
}

static void add_gamepad(InputSdl* sdl, InputSystem* system, SDL_JoystickID id)
{
    if (find_gamepad(sdl, id) != nullptr)
    {
        return;
    }
    SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
    if (gamepad == nullptr)
    {
        SDL_Log("Gamepad %u: can't open (%s)", static_cast<unsigned>(id), SDL_GetError());
        return;
    }
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    const char* name = SDL_GetGamepadName(gamepad);
    name = name != nullptr ? name : "Gamepad";

    InputSdlGamepad entry = {};
    entry.joystick_id = id;
    entry.gamepad = gamepad;
    entry.device = input_devices_add(&system->devices, InputDeviceKind::gamepad, guid, name);
    InputDevice* added = input_devices_find(&system->devices, entry.device);
    if (added != nullptr)
    {
        // Some pads report their d-pad as the left stick (no d-pad buttons).
        added->has_dpad = SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    }
    input_add_controllers_for_device(system, entry.device);
    // The axes as they are now: a device pinned at an extreme from the
    // start (not a real gamepad) is told apart this way.
    for (uint32_t a = 0; a < INPUT_AXIS_COUNT; a++)
    {
        int16_t value = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(a));
        input_devices_set_axis(&system->devices, entry.device, static_cast<InputAxis>(a), static_cast<float>(value) / 32767.0f);
    }
    sdl->gamepads.push_back(entry);
    const InputDevice* device = input_devices_find_const(&system->devices, entry.device);
    SDL_Log("Gamepad: %s (%s)%s", name, guid,
            device == nullptr || !input_devices_ignored(device) ? ""
            : device->ignore == InputIgnore::yes ? ", ignored (CONFIG > INPUT)"
                                                  : ", ignored: its axes are pinned (not a real gamepad)");
}

// A joystick SDL has no gamepad mapping for can't be used until it is mapped.
static void report_unmapped(SDL_JoystickID id)
{
    if (SDL_IsGamepad(id))
    {
        return;
    }
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    const char* name = SDL_GetJoystickNameForID(id);
    SDL_Log("Controller %s (%s) isn't known as a gamepad: map it with piccolo_map_gamepad to use it",
            name != nullptr ? name : "(no name)", guid);
}

static void remove_gamepad(InputSdl* sdl, InputSystem* system, SDL_JoystickID id)
{
    for (size_t i = 0; i < sdl->gamepads.size(); i++)
    {
        if (sdl->gamepads[i].joystick_id == id)
        {
            input_devices_disconnect(&system->devices, sdl->gamepads[i].device);
            SDL_CloseGamepad(sdl->gamepads[i].gamepad);
            sdl->gamepads.erase(sdl->gamepads.begin() + static_cast<long>(i));
            return;
        }
    }
}

void input_sdl_init(InputSdl* sdl, InputSystem* system, const char* mappings_path)
{
    if (mappings_path != nullptr)
    {
        int added = SDL_AddGamepadMappingsFromFile(mappings_path);
        if (added > 0)
        {
            SDL_Log("Gamepad mappings: %d from %s", added, mappings_path);
        }
    }
    sdl->keyboard_device = input_devices_add(&system->devices, InputDeviceKind::keyboard, "keyboard", "Keyboard");
    input_add_controllers_for_device(system, sdl->keyboard_device);
    sdl->last_active_device = sdl->keyboard_device;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids != nullptr && i < count; i++)
    {
        if (SDL_IsGamepad(ids[i]))
        {
            add_gamepad(sdl, system, ids[i]);
        }
        else
        {
            report_unmapped(ids[i]);
        }
    }
    SDL_free(ids);
}

void input_sdl_shutdown(InputSdl* sdl)
{
    for (size_t i = 0; i < sdl->gamepads.size(); i++)
    {
        SDL_CloseGamepad(sdl->gamepads[i].gamepad);
    }
    sdl->gamepads.clear();
}

void input_sdl_handle_event(InputSdl* sdl, InputSystem* system, const SDL_Event* event)
{
    switch (event->type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        bool down = event->type == SDL_EVENT_KEY_DOWN;
        if (down && event->key.repeat)
        {
            break;
        }
        input_devices_set_code(&system->devices, sdl->keyboard_device, static_cast<uint32_t>(event->key.scancode), down);
        if (down)
        {
            sdl->last_active_device = sdl->keyboard_device;
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_devices_release_all(&system->devices, sdl->keyboard_device);
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        report_unmapped(event->jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        add_gamepad(sdl, system, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        remove_gamepad(sdl, system, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    {
        InputSdlGamepad* gamepad = find_gamepad(sdl, event->gbutton.which);
        // SDL's buttons up to the touchpad are InputButton in the same order.
        if (gamepad == nullptr || event->gbutton.button >= INPUT_BUTTON_COUNT)
        {
            break;
        }
        bool down = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        input_devices_set_code(&system->devices, gamepad->device, event->gbutton.button, down);
        if (down)
        {
            sdl->last_active_device = gamepad->device;
            sdl->last_active_gamepad = gamepad->device;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    {
        InputSdlGamepad* gamepad = find_gamepad(sdl, event->gaxis.which);
        if (gamepad == nullptr || event->gaxis.axis >= INPUT_AXIS_COUNT)
        {
            break;
        }
        input_devices_set_axis(&system->devices, gamepad->device, static_cast<InputAxis>(event->gaxis.axis),
                               static_cast<float>(event->gaxis.value) / 32767.0f);
        break;
    }
    default:
        break;
    }
}

SDL_Gamepad* input_sdl_gamepad(const InputSdl* sdl, uint32_t device_id)
{
    for (size_t i = 0; i < sdl->gamepads.size(); i++)
    {
        if (sdl->gamepads[i].device == device_id)
        {
            return sdl->gamepads[i].gamepad;
        }
    }
    return nullptr;
}

void input_sdl_rumble(InputSdl* sdl, const InputSystem* system, const uint16_t rumble[][2], uint32_t port_count)
{
    for (size_t i = 0; i < sdl->gamepads.size(); i++)
    {
        InputSdlGamepad* gamepad = &sdl->gamepads[i];
        uint16_t strong = 0;
        uint16_t weak = 0;
        int32_t controller = input_first_controller(system, gamepad->device);
        int32_t port = controller >= 0 ? input_controller_port(system, static_cast<uint32_t>(controller)) : INPUT_NO_PORT;
        if (port != INPUT_NO_PORT && static_cast<uint32_t>(port) < port_count)
        {
            strong = rumble[port][0];
            weak = rumble[port][1];
        }
        if (strong != gamepad->rumble[0] || weak != gamepad->rumble[1])
        {
            SDL_RumbleGamepad(gamepad->gamepad, strong, weak, strong != 0 || weak != 0 ? RUMBLE_DURATION_MS : 0);
            gamepad->rumble[0] = strong;
            gamepad->rumble[1] = weak;
        }
    }
}

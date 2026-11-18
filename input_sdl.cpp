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

static void forget_unmapped(InputSdl* sdl, uint32_t id)
{
    for (size_t i = 0; i < sdl->unmapped.size(); i++)
    {
        if (sdl->unmapped[i] == id)
        {
            sdl->unmapped.erase(sdl->unmapped.begin() + static_cast<long>(i));
            return;
        }
    }
}

// A joystick SDL has no gamepad mapping for can't be used until it is mapped.
static void report_unmapped(InputSdl* sdl, SDL_JoystickID id)
{
    if (SDL_IsGamepad(id))
    {
        return;
    }
    forget_unmapped(sdl, id);
    sdl->unmapped.push_back(id);
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    const char* name = SDL_GetJoystickNameForID(id);
    SDL_Log("Controller %s (%s) isn't known as a gamepad: map it (SETTINGS > INPUT > DEVICES) to use it",
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

// The device of a mouse by SDL's id, added the first time it is seen.
// (Touch and pen pretend to be a mouse too: not this.)
static uint32_t mouse_device(InputSdl* sdl, InputSystem* system, SDL_MouseID which)
{
    if (which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID)
    {
        return 0;
    }
    for (size_t m = 0; m < sdl->mice.size(); m++)
    {
        if (sdl->mice[m].which == which)
        {
            const InputDevice* device = input_devices_find_const(&system->devices, sdl->mice[m].device);
            if (device != nullptr && device->connected)
            {
                return sdl->mice[m].device;
            }
            sdl->mice.erase(sdl->mice.begin() + static_cast<long>(m));
            break;
        }
    }
    const char* name = which != 0 ? SDL_GetMouseNameForID(which) : nullptr;
    InputSdlMouse mouse = {};
    mouse.which = which;
    mouse.device = input_devices_add(&system->devices, InputDeviceKind::mouse, "mouse",
                                     name != nullptr && name[0] != '\0' ? name : "Mouse");
    input_add_controllers_for_device(system, mouse.device);
    sdl->mice.push_back(mouse);
    return mouse.device;
}

static void remove_mouse(InputSdl* sdl, InputSystem* system, SDL_MouseID which)
{
    for (size_t m = 0; m < sdl->mice.size(); m++)
    {
        if (sdl->mice[m].which == which)
        {
            input_devices_disconnect(&system->devices, sdl->mice[m].device);
            sdl->mice.erase(sdl->mice.begin() + static_cast<long>(m));
            return;
        }
    }
}

void input_sdl_init(InputSdl* sdl, InputSystem* system, const char* database_path)
{
    if (database_path != nullptr)
    {
        int added = SDL_AddGamepadMappingsFromFile(database_path);
        if (added > 0)
        {
            SDL_Log("Gamepad mappings: %d from %s", added, database_path);
        }
    }
    for (size_t p = 0; p < system->profiles.size(); p++)
    {
        const std::string& mapping = system->profiles[p].sdl_mapping;
        if (!mapping.empty() && SDL_AddGamepadMapping(mapping.c_str()) < 0)
        {
            SDL_Log("Gamepad mapping refused (%s): %s", SDL_GetError(), mapping.c_str());
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
            report_unmapped(sdl, ids[i]);
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
        for (size_t m = 0; m < sdl->mice.size(); m++)
        {
            input_devices_release_all(&system->devices, sdl->mice[m].device);
        }
        break;
    case SDL_EVENT_MOUSE_ADDED:
        mouse_device(sdl, system, event->mdevice.which);
        break;
    case SDL_EVENT_MOUSE_REMOVED:
        remove_mouse(sdl, system, event->mdevice.which);
        break;
    case SDL_EVENT_MOUSE_MOTION:
    {
        uint32_t device = mouse_device(sdl, system, event->motion.which);
        if (device != 0)
        {
            input_devices_add_motion(&system->devices, device, event->motion.xrel, event->motion.yrel);
            input_devices_set_position(&system->devices, device, event->motion.x, event->motion.y);
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        uint32_t device = mouse_device(sdl, system, event->button.which);
        InputMouseButton button = InputMouseButton::count;
        switch (event->button.button)
        {
        case SDL_BUTTON_LEFT:   button = InputMouseButton::left; break;
        case SDL_BUTTON_RIGHT:  button = InputMouseButton::right; break;
        case SDL_BUTTON_MIDDLE: button = InputMouseButton::middle; break;
        case SDL_BUTTON_X1:     button = InputMouseButton::x1; break;
        case SDL_BUTTON_X2:     button = InputMouseButton::x2; break;
        default:                break;
        }
        if (device != 0 && button != InputMouseButton::count)
        {
            bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            input_devices_set_code(&system->devices, device, static_cast<uint32_t>(button), down);
            input_devices_set_position(&system->devices, device, event->button.x, event->button.y);
            if (down)
            {
                sdl->last_active_device = device;
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
    {
        uint32_t device = mouse_device(sdl, system, event->wheel.which);
        float y = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y;
        if (device != 0 && y != 0.0f)
        {
            input_devices_wheel(&system->devices, device, y > 0.0f);
        }
        break;
    }
    case SDL_EVENT_JOYSTICK_ADDED:
        report_unmapped(sdl, event->jdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        forget_unmapped(sdl, event->jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        forget_unmapped(sdl, event->gdevice.which);   // mapped now
        add_gamepad(sdl, system, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        remove_gamepad(sdl, system, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    {
        InputSdlGamepad* gamepad = find_gamepad(sdl, event->gbutton.which);
        // SDL's buttons are InputButton in the same order (up to MISC6).
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

std::string input_sdl_joystick_name(uint32_t joystick_id)
{
    const char* name = SDL_GetJoystickNameForID(static_cast<SDL_JoystickID>(joystick_id));
    return name != nullptr ? name : "";
}

std::string input_sdl_joystick_guid(uint32_t joystick_id)
{
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(static_cast<SDL_JoystickID>(joystick_id)), guid, sizeof(guid));
    return guid;
}

uint32_t input_sdl_joystick_of(const InputSdl* sdl, uint32_t device_id)
{
    for (size_t i = 0; i < sdl->gamepads.size(); i++)
    {
        if (sdl->gamepads[i].device == device_id)
        {
            return sdl->gamepads[i].joystick_id;
        }
    }
    return 0;
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
        for (uint32_t port = 0; controller >= 0 && port < port_count && port < INPUT_PORT_COUNT; port++)
        {
            // On several ports: the strongest of them.
            if (input_port_has(system, port, static_cast<uint32_t>(controller)))
            {
                strong = rumble[port][0] > strong ? rumble[port][0] : strong;
                weak = rumble[port][1] > weak ? rumble[port][1] : weak;
            }
        }
        if (strong != gamepad->rumble[0] || weak != gamepad->rumble[1])
        {
            SDL_RumbleGamepad(gamepad->gamepad, strong, weak, strong != 0 || weak != 0 ? RUMBLE_DURATION_MS : 0);
            gamepad->rumble[0] = strong;
            gamepad->rumble[1] = weak;
        }
    }
}

bool input_sdl_press_poll(InputSdlPress* press, uint32_t joystick_id)
{
    if (press->joystick == nullptr || press->joystick_id != joystick_id)
    {
        input_sdl_press_close(press);
        press->joystick = SDL_OpenJoystick(static_cast<SDL_JoystickID>(joystick_id));
        press->joystick_id = joystick_id;
        press->held = true;
    }
    if (press->joystick == nullptr)
    {
        return false;
    }
    bool down = false;
    int buttons = SDL_GetNumJoystickButtons(press->joystick);
    for (int b = 0; b < buttons && !down; b++)
    {
        down = SDL_GetJoystickButton(press->joystick, b);
    }
    int hats = SDL_GetNumJoystickHats(press->joystick);
    for (int h = 0; h < hats && !down; h++)
    {
        down = SDL_GetJoystickHat(press->joystick, h) != SDL_HAT_CENTERED;
    }
    bool pressed = down && !press->held;
    press->held = down;
    return pressed;
}

void input_sdl_press_wait_release(InputSdlPress* press)
{
    press->held = true;
}

void input_sdl_press_close(InputSdlPress* press)
{
    if (press->joystick != nullptr)
    {
        SDL_CloseJoystick(press->joystick);
    }
    press->joystick = nullptr;
    press->held = true;
}

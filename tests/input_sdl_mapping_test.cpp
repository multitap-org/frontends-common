// MAP CONTROLLER's mapper on an SDL virtual joystick: the positions first,
// then the roles, which only take a button the positions mapped.

#include <SDL3/SDL.h>

#include <cstdio>

#include "input_sdl_mapping.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAILED: %s\n", what);
        g_failures++;
    }
}

static SDL_Joystick* g_joystick = nullptr;

// Holds (or lets go of) a button on the virtual joystick and lets SDL see it.
static void set_button(int button, bool down)
{
    SDL_SetJoystickVirtualButton(g_joystick, button, down);
    SDL_UpdateJoysticks();
}

// A press and its release, each followed by a mapper update. Returns what the
// update after the press said (the mapping is complete).
static bool press(InputSdlMapper* mapper, int button)
{
    set_button(button, true);
    bool done = input_sdl_mapper_update(mapper, 0);
    set_button(button, false);
    input_sdl_mapper_update(mapper, 0);
    return done;
}

static bool is_button(InputSource source, InputButton button)
{
    return source.type == InputSourceType::button && source.code == static_cast<uint16_t>(button);
}

static void test_positions_then_roles()
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.nbuttons = 12;
    desc.name = "Virtual test pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    check(id != 0, "mapper: a virtual joystick");
    g_joystick = SDL_OpenJoystick(id);
    check(g_joystick != nullptr, "mapper: opened");
    if (g_joystick == nullptr)
    {
        return;
    }

    // Only the four face buttons; two roles, the second optional.
    bool wanted[INPUT_MAP_CONTROL_COUNT] = {};
    wanted[static_cast<uint32_t>(InputMapControl::south)] = true;
    wanted[static_cast<uint32_t>(InputMapControl::east)] = true;
    wanted[static_cast<uint32_t>(InputMapControl::west)] = true;
    wanted[static_cast<uint32_t>(InputMapControl::north)] = true;
    bool optional[2] = {false, true};
    InputSdlMapper* mapper = new InputSdlMapper;
    check(input_sdl_mapper_begin(mapper, id, 2, optional, wanted, 0), "mapper: begins");
    input_sdl_mapper_update(mapper, 0);   // everything at rest: the first step starts

    check(input_sdl_mapper_role(mapper) == -1 && input_sdl_mapper_control(mapper) == InputMapControl::south,
          "mapper: the positions come first");
    press(mapper, 0);   // bottom
    press(mapper, 1);   // right
    press(mapper, 1);   // the same button again: not a second position
    check(input_sdl_mapper_control(mapper) == InputMapControl::west, "mapper: a position can't repeat another");
    press(mapper, 2);   // left
    press(mapper, 3);   // top
    check(input_sdl_mapper_role(mapper) == 0, "mapper: then the roles");

    press(mapper, 9);   // not on the mapping
    check(input_sdl_mapper_role(mapper) == 0, "mapper: a role ignores a button the positions didn't map");

    set_button(3, true);
    check(is_button(input_sdl_mapper_held_source(mapper), InputButton::north), "mapper: the button held, as its position");
    set_button(3, false);
    check(input_sdl_mapper_held_source(mapper).type == InputSourceType::none, "mapper: nothing held");

    press(mapper, 1);   // role 0 on the right button (shared with a position)
    bool done = press(mapper, 2);   // role 1 on the left one
    check(done, "mapper: complete after the last role");
    check(is_button(input_sdl_mapper_role_source(mapper, 0), InputButton::east), "mapper: role 0 is the right button");
    check(is_button(input_sdl_mapper_role_source(mapper, 1), InputButton::west), "mapper: role 1 is the left button");

    input_sdl_mapper_cancel(mapper);
    delete mapper;
    SDL_CloseJoystick(g_joystick);
    SDL_DetachVirtualJoystick(id);
}

int main()
{
    if (!SDL_Init(SDL_INIT_JOYSTICK))
    {
        std::printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    test_positions_then_roles();
    SDL_Quit();
    if (g_failures == 0)
    {
        std::printf("input_sdl_mapping: all checks passed\n");
        return 0;
    }
    std::printf("input_sdl_mapping: %d check(s) failed\n", g_failures);
    return 1;
}

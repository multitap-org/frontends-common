// MAP CONTROLLER's mapper on an SDL virtual joystick: the positions first,
// then the roles, which only take a button the positions mapped; every
// step takes a control held long enough.

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

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

static uint64_t g_now = 0;   // the mapper's clock, moved by the presses

// A press held `held_ms`, then let go, each followed by a mapper update.
// Returns what the update before the release said (the mapping is complete).
static bool press_for(InputSdlMapper* mapper, int button, uint64_t held_ms)
{
    set_button(button, true);
    input_sdl_mapper_update(mapper, g_now);
    g_now += held_ms;
    bool done = input_sdl_mapper_update(mapper, g_now);
    set_button(button, false);
    input_sdl_mapper_update(mapper, g_now);
    return done;
}

// A press held long enough to be taken.
static bool press(InputSdlMapper* mapper, int button)
{
    return press_for(mapper, button, mapper->hold_ms);
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
    press_for(mapper, 0, mapper->hold_ms / 2);   // let go halfway: not taken
    check(input_sdl_mapper_control(mapper) == InputMapControl::south, "mapper: a position let go too soon isn't taken");
    check(input_sdl_mapper_at_phase_start(mapper), "mapper: the first position is the phase's start");
    press(mapper, 0);   // bottom
    check(!input_sdl_mapper_at_phase_start(mapper), "mapper: past the phase's start");
    input_sdl_mapper_restart_phase(mapper, g_now);
    input_sdl_mapper_update(mapper, g_now);
    check(input_sdl_mapper_control(mapper) == InputMapControl::south &&
              mapper->elements[static_cast<uint32_t>(InputMapControl::south)].empty(),
          "mapper: RESET goes back to the phase's first input, cleared");
    press(mapper, 0);   // bottom again
    press(mapper, 1);   // right
    press(mapper, 1);   // the same button again: not a second position
    check(input_sdl_mapper_control(mapper) == InputMapControl::west, "mapper: a position can't repeat another");
    press(mapper, 2);   // left
    press(mapper, 3);   // top
    check(input_sdl_mapper_role(mapper) == 0, "mapper: then the roles");
    check(input_sdl_mapper_at_phase_start(mapper), "mapper: the first role is its phase's start (BACK goes no further)");

    press(mapper, 9);   // not on the mapping
    check(input_sdl_mapper_role(mapper) == 0, "mapper: a role ignores a button the positions didn't map");

    set_button(3, true);
    check(is_button(input_sdl_mapper_held_source(mapper), InputButton::north), "mapper: the button held, as its position");
    set_button(3, false);
    check(input_sdl_mapper_held_source(mapper).type == InputSourceType::none, "mapper: nothing held");

    press_for(mapper, 1, mapper->hold_ms / 2);   // let go halfway: not taken
    check(input_sdl_mapper_role(mapper) == 0, "mapper: a role's button let go too soon isn't taken");
    set_button(1, true);
    input_sdl_mapper_update(mapper, g_now);
    float half = input_sdl_mapper_hold_progress(mapper, g_now + mapper->hold_ms / 2);
    check(half > 0.4f && half < 0.6f, "mapper: the hold's progress");
    set_button(1, false);
    input_sdl_mapper_update(mapper, g_now);
    press(mapper, 1);   // role 0 on the right button (shared with a position)
    check(input_sdl_mapper_role(mapper) == 1, "mapper: a role's button held is taken");
    bool done = press(mapper, 2);   // role 1 on the left one
    check(done, "mapper: complete after the last role");
    check(is_button(input_sdl_mapper_role_source(mapper, 0), InputButton::east), "mapper: role 0 is the right button");
    check(is_button(input_sdl_mapper_role_source(mapper, 1), InputButton::west), "mapper: role 1 is the left button");

    input_sdl_mapper_cancel(mapper);
    delete mapper;
    SDL_CloseJoystick(g_joystick);
    SDL_DetachVirtualJoystick(id);
}

// STEP 3 on a mapping that exists: the extras are added, the rest (and what
// the mapper doesn't know, like the touchpad) is written back as it was;
// "other buttons" end when one doesn't come.
static void test_extras_and_skipping()
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.nbuttons = 12;
    desc.name = "Virtual extras pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    g_joystick = SDL_OpenJoystick(id);
    check(g_joystick != nullptr, "extras: opened");
    if (g_joystick == nullptr)
    {
        return;
    }
    const char* existing = "03000000000000000000000000000000,Pad,a:b0,b:b1,touchpad:b11,platform:Linux,";

    bool wanted[INPUT_MAP_CONTROL_COUNT] = {};
    wanted[static_cast<uint32_t>(InputMapControl::right_paddle1)] = true;
    wanted[static_cast<uint32_t>(InputMapControl::misc1)] = true;
    wanted[static_cast<uint32_t>(InputMapControl::misc2)] = true;
    wanted[static_cast<uint32_t>(InputMapControl::misc3)] = true;
    InputSdlMapper* mapper = new InputSdlMapper;
    check(input_sdl_mapper_begin(mapper, id, 0, nullptr, wanted, g_now), "extras: begins");
    mapper->end_on_skip_from = InputMapControl::misc1;
    input_sdl_mapper_load(mapper, existing);
    input_sdl_mapper_update(mapper, g_now);
    check(input_sdl_mapper_control(mapper) == InputMapControl::right_paddle1, "extras: the paddles first");
    press(mapper, 0);   // already the bottom button: not a paddle
    check(input_sdl_mapper_control(mapper) == InputMapControl::right_paddle1, "extras: a mapped button isn't an extra");
    press(mapper, 5);
    press(mapper, 6);   // misc1
    check(input_sdl_mapper_control(mapper) == InputMapControl::misc2, "extras: then the other buttons");
    g_now += mapper->optional_wait_ms;   // none comes
    bool done = input_sdl_mapper_update(mapper, g_now);
    check(done, "extras: an other button that doesn't come ends them");
    std::string line;
    check(input_sdl_mapper_finish(mapper, &line), "extras: finished");
    check(line.find("a:b0,") != std::string::npos && line.find("b:b1,") != std::string::npos, "extras: the mapping kept");
    check(line.find("paddle1:b5,") != std::string::npos && line.find("misc1:b6,") != std::string::npos, "extras: added");
    check(line.find("touchpad:b11,") != std::string::npos, "extras: a field the mapper doesn't know, kept");
    check(line.find("misc2:") == std::string::npos, "extras: none for the one that didn't come");

    // STEP 1 skipped (SDL's mapping loaded), STEP 2 skipped: complete, no roles.
    bool none[INPUT_MAP_CONTROL_COUNT] = {};
    bool optional[1] = {false};
    check(input_sdl_mapper_begin(mapper, id, 1, optional, none, g_now), "skip: begins");
    input_sdl_mapper_load(mapper, existing);
    input_sdl_mapper_update(mapper, g_now);
    check(input_sdl_mapper_role(mapper) == 0, "skip: no positions, straight to the roles");
    press(mapper, 1);   // on the loaded mapping: the right button
    check(is_button(input_sdl_mapper_role_source(mapper, 0), InputButton::east), "skip: a role found on the loaded mapping");
    check(input_sdl_mapper_begin(mapper, id, 1, optional, none, g_now), "skip: again");
    input_sdl_mapper_skip_roles(mapper);
    check(input_sdl_mapper_update(mapper, g_now), "skip: all roles skipped, complete");
    check(input_sdl_mapper_role_source(mapper, 0).type == InputSourceType::none, "skip: a skipped role is none");

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
    test_extras_and_skipping();
    SDL_Quit();
    if (g_failures == 0)
    {
        std::printf("input_sdl_mapping: all checks passed\n");
        return 0;
    }
    std::printf("input_sdl_mapping: %d check(s) failed\n", g_failures);
    return 1;
}

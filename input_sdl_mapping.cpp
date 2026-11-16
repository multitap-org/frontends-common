#include "input_sdl_mapping.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <fstream>

constexpr int      AXIS_PUSHED = 16000;          // past this from rest, an axis counts as pushed
constexpr uint64_t OPTIONAL_WAIT_MS = 5000;      // an optional step is skipped after this long

// The name SDL's mapping text uses for each control.
static const char* const SDL_NAMES[INPUT_MAP_CONTROL_COUNT] = {
    "a", "b", "x", "y", "dpup", "dpdown", "dpleft", "dpright", "leftshoulder", "rightshoulder", "back", "start",
    "lefttrigger", "righttrigger", "leftstick", "rightstick", "guide", "leftx", "lefty", "rightx", "righty",
};

const char* input_map_control_sdl_name(InputMapControl control)
{
    uint32_t index = static_cast<uint32_t>(control);
    return index < INPUT_MAP_CONTROL_COUNT ? SDL_NAMES[index] : "";
}

bool input_map_control_optional(InputMapControl control)
{
    return static_cast<uint32_t>(control) >= static_cast<uint32_t>(InputMapControl::left_trigger);
}

static bool is_stick(InputMapControl control)
{
    return static_cast<uint32_t>(control) >= static_cast<uint32_t>(InputMapControl::left_x);
}

// Everything back at rest: buttons up, hats centered, axes near where they started.
static bool at_rest(const InputSdlMapper* mapper)
{
    SDL_Joystick* joystick = mapper->joystick;
    for (int b = 0; b < SDL_GetNumJoystickButtons(joystick); b++)
    {
        if (SDL_GetJoystickButton(joystick, b))
        {
            return false;
        }
    }
    for (int h = 0; h < SDL_GetNumJoystickHats(joystick); h++)
    {
        if (SDL_GetJoystickHat(joystick, h) != SDL_HAT_CENTERED)
        {
            return false;
        }
    }
    for (int a = 0; a < SDL_GetNumJoystickAxes(joystick) && static_cast<size_t>(a) < mapper->rest.size(); a++)
    {
        int delta = SDL_GetJoystickAxis(joystick, a) - mapper->rest[static_cast<size_t>(a)];
        if (delta > AXIS_PUSHED || delta < -AXIS_PUSHED)
        {
            return false;
        }
    }
    return true;
}

// What the joystick shows now for this step: "b3", "h0.1", "+a2", "a5", "a1~", or "".
static std::string pressed_element(const InputSdlMapper* mapper, InputMapControl control)
{
    SDL_Joystick* joystick = mapper->joystick;
    char text[32] = {};
    if (!is_stick(control))
    {
        for (int b = 0; b < SDL_GetNumJoystickButtons(joystick); b++)
        {
            if (SDL_GetJoystickButton(joystick, b))
            {
                std::snprintf(text, sizeof(text), "b%d", b);
                return text;
            }
        }
        for (int h = 0; h < SDL_GetNumJoystickHats(joystick); h++)
        {
            uint8_t value = SDL_GetJoystickHat(joystick, h);
            if (value == SDL_HAT_UP || value == SDL_HAT_RIGHT || value == SDL_HAT_DOWN || value == SDL_HAT_LEFT)
            {
                std::snprintf(text, sizeof(text), "h%d.%u", h, static_cast<unsigned>(value));
                return text;
            }
        }
    }
    for (int a = 0; a < SDL_GetNumJoystickAxes(joystick) && static_cast<size_t>(a) < mapper->rest.size(); a++)
    {
        int rest = mapper->rest[static_cast<size_t>(a)];
        int delta = SDL_GetJoystickAxis(joystick, a) - rest;
        if (delta <= AXIS_PUSHED && delta >= -AXIS_PUSHED)
        {
            continue;
        }
        if (is_stick(control))
        {
            std::snprintf(text, sizeof(text), delta > 0 ? "a%d" : "a%d~", a);   // pushed right / down
        }
        else if (rest < -AXIS_PUSHED)
        {
            std::snprintf(text, sizeof(text), "a%d", a);   // a trigger resting at one end: its whole range
        }
        else
        {
            std::snprintf(text, sizeof(text), delta > 0 ? "+a%d" : "-a%d", a);   // half an axis as a button
        }
        return text;
    }
    return "";
}

// The next step is the next wanted one.
static void start_step(InputSdlMapper* mapper, uint64_t now_ms)
{
    while (mapper->step < INPUT_MAP_CONTROL_COUNT && !mapper->wanted[mapper->step])
    {
        mapper->step++;
    }
    mapper->waiting_release = true;
    mapper->step_started_ms = now_ms;
}

bool input_sdl_mapper_begin(InputSdlMapper* mapper, uint32_t joystick_id, const bool* wanted, uint64_t now_ms)
{
    for (uint32_t c = 0; c < INPUT_MAP_CONTROL_COUNT; c++)
    {
        mapper->wanted[c] = wanted == nullptr || wanted[c];
    }
    input_sdl_mapper_cancel(mapper);
    SDL_Joystick* joystick = SDL_OpenJoystick(static_cast<SDL_JoystickID>(joystick_id));
    if (joystick == nullptr)
    {
        SDL_Log("Mapping: can't open joystick %u (%s)", static_cast<unsigned>(joystick_id), SDL_GetError());
        return false;
    }
    mapper->active = true;
    mapper->joystick = joystick;
    mapper->joystick_id = joystick_id;
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));
    mapper->guid = guid;
    const char* name = SDL_GetJoystickName(joystick);
    mapper->name = name != nullptr ? name : "Controller";
    for (size_t i = 0; i < mapper->name.size(); i++)
    {
        mapper->name[i] = mapper->name[i] == ',' ? ' ' : mapper->name[i];   // a comma would split the mapping
    }
    mapper->rest.clear();
    for (int a = 0; a < SDL_GetNumJoystickAxes(joystick); a++)
    {
        mapper->rest.push_back(SDL_GetJoystickAxis(joystick, a));
    }
    for (uint32_t c = 0; c < INPUT_MAP_CONTROL_COUNT; c++)
    {
        mapper->elements[c].clear();
    }
    mapper->step = 0;
    start_step(mapper, now_ms);
    return true;
}

bool input_sdl_mapper_update(InputSdlMapper* mapper, uint64_t now_ms)
{
    if (!mapper->active)
    {
        return false;
    }
    if (mapper->step >= INPUT_MAP_CONTROL_COUNT)
    {
        return true;
    }
    if (mapper->waiting_release)
    {
        if (!at_rest(mapper))
        {
            return false;
        }
        mapper->waiting_release = false;
        mapper->step_started_ms = now_ms;   // the time to answer starts once all is let go
    }
    InputMapControl control = static_cast<InputMapControl>(mapper->step);
    std::string element = pressed_element(mapper, control);
    bool used = false;
    for (uint32_t c = 0; c < mapper->step && !element.empty(); c++)
    {
        used = used || mapper->elements[c] == element;
    }
    if (!element.empty() && !used)
    {
        mapper->elements[mapper->step] = element;
        mapper->step++;
        start_step(mapper, now_ms);
    }
    else if (input_map_control_optional(control) && element.empty() && now_ms - mapper->step_started_ms >= OPTIONAL_WAIT_MS)
    {
        mapper->step++;   // the pad doesn't have it
        start_step(mapper, now_ms);
    }
    return mapper->step >= INPUT_MAP_CONTROL_COUNT;
}

uint32_t input_sdl_mapper_seconds_left(const InputSdlMapper* mapper, uint64_t now_ms)
{
    if (!mapper->active || mapper->step >= INPUT_MAP_CONTROL_COUNT ||
        !input_map_control_optional(static_cast<InputMapControl>(mapper->step)) || mapper->waiting_release)
    {
        return 0;
    }
    uint64_t elapsed = now_ms - mapper->step_started_ms;
    return elapsed >= OPTIONAL_WAIT_MS ? 0 : static_cast<uint32_t>((OPTIONAL_WAIT_MS - elapsed + 999) / 1000);
}

void input_sdl_mapper_back(InputSdlMapper* mapper, uint64_t now_ms)
{
    // The previous wanted step.
    uint32_t step = mapper->step;
    while (mapper->active && step > 0)
    {
        step--;
        if (mapper->wanted[step])
        {
            mapper->step = step;
            mapper->elements[step].clear();
            mapper->waiting_release = true;
            mapper->step_started_ms = now_ms;
            return;
        }
    }
}

void input_sdl_mapper_skip(InputSdlMapper* mapper, uint64_t now_ms)
{
    if (!mapper->active || mapper->step >= INPUT_MAP_CONTROL_COUNT)
    {
        return;
    }
    mapper->elements[mapper->step].clear();
    mapper->step++;
    start_step(mapper, now_ms);
}

void input_sdl_mapper_cancel(InputSdlMapper* mapper)
{
    if (mapper->joystick != nullptr)
    {
        SDL_CloseJoystick(mapper->joystick);
    }
    mapper->joystick = nullptr;
    mapper->active = false;
}

// The mappings file with this GUID's line replaced (or added).
static bool keep_mapping(const char* path, const std::string& guid, const std::string& line)
{
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string existing;
    while (std::getline(in, existing))
    {
        if (!existing.empty() && existing.compare(0, guid.size(), guid) != 0)
        {
            lines.push_back(existing);   // other controllers' mappings stay
        }
    }
    in.close();
    lines.push_back(line);
    std::ofstream out(path, std::ios::trunc);
    for (size_t i = 0; i < lines.size(); i++)
    {
        out << lines[i] << "\n";
    }
    return static_cast<bool>(out);
}

bool input_sdl_mapper_finish(InputSdlMapper* mapper, const char* mappings_path, std::string* line)
{
    if (!mapper->active)
    {
        return false;
    }
    std::string text = mapper->guid + "," + mapper->name + ",";
    bool any = false;
    for (uint32_t c = 0; c < INPUT_MAP_CONTROL_COUNT; c++)
    {
        if (!mapper->elements[c].empty())
        {
            text += std::string(SDL_NAMES[c]) + ":" + mapper->elements[c] + ",";
            any = true;
        }
    }
    text += std::string("platform:") + SDL_GetPlatform() + ",";
    std::string guid = mapper->guid;
    input_sdl_mapper_cancel(mapper);
    if (!any || SDL_AddGamepadMapping(text.c_str()) < 0)
    {
        SDL_Log("Mapping refused (%s): %s", SDL_GetError(), text.c_str());
        return false;
    }
    if (mappings_path != nullptr && mappings_path[0] != '\0' && !keep_mapping(mappings_path, guid, text))
    {
        SDL_Log("Mapping: can't write %s", mappings_path);
    }
    SDL_Log("Mapping kept: %s", text.c_str());
    *line = text;
    return true;
}

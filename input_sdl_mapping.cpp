#include "input_sdl_mapping.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

constexpr int      AXIS_PUSHED = 16000;          // past this from rest, an axis counts as pushed

// The name SDL's mapping text uses for each control.
static const char* const SDL_NAMES[INPUT_MAP_CONTROL_COUNT] = {
    "a", "b", "x", "y", "dpup", "dpdown", "dpleft", "dpright", "leftshoulder", "rightshoulder", "back", "start",
    "lefttrigger", "righttrigger", "leftstick", "rightstick", "guide", "leftx", "lefty", "rightx", "righty",
    "paddle1", "paddle2", "paddle3", "paddle4", "misc5", "misc6", "misc1", "misc2", "misc3", "misc4",
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
    return static_cast<uint32_t>(control) >= static_cast<uint32_t>(InputMapControl::left_x) &&
           static_cast<uint32_t>(control) <= static_cast<uint32_t>(InputMapControl::right_y);
}

bool input_map_control_is_stick(InputMapControl control)
{
    return is_stick(control);
}

InputMapControl input_map_control_for_source(InputSource source)
{
    if (source.type == InputSourceType::button)
    {
        switch (static_cast<InputButton>(source.code))
        {
        case InputButton::south: return InputMapControl::south;
        case InputButton::east: return InputMapControl::east;
        case InputButton::west: return InputMapControl::west;
        case InputButton::north: return InputMapControl::north;
        case InputButton::dpad_up: return InputMapControl::dpad_up;
        case InputButton::dpad_down: return InputMapControl::dpad_down;
        case InputButton::dpad_left: return InputMapControl::dpad_left;
        case InputButton::dpad_right: return InputMapControl::dpad_right;
        case InputButton::left_shoulder: return InputMapControl::left_shoulder;
        case InputButton::right_shoulder: return InputMapControl::right_shoulder;
        case InputButton::back: return InputMapControl::back;
        case InputButton::start: return InputMapControl::start;
        case InputButton::left_stick: return InputMapControl::left_stick;
        case InputButton::right_stick: return InputMapControl::right_stick;
        case InputButton::guide: return InputMapControl::guide;
        case InputButton::right_paddle1: return InputMapControl::paddle1;
        case InputButton::left_paddle1: return InputMapControl::paddle2;
        case InputButton::right_paddle2: return InputMapControl::paddle3;
        case InputButton::left_paddle2: return InputMapControl::paddle4;
        case InputButton::misc5: return InputMapControl::l4;
        case InputButton::misc6: return InputMapControl::r4;
        case InputButton::misc1: return InputMapControl::misc1;
        case InputButton::misc2: return InputMapControl::misc2;
        case InputButton::misc3: return InputMapControl::misc3;
        case InputButton::misc4: return InputMapControl::misc4;
        default: return InputMapControl::count;
        }
    }
    if (source.type == InputSourceType::axis_plus || source.type == InputSourceType::axis_minus)
    {
        switch (static_cast<InputAxis>(source.code))
        {
        case InputAxis::left_x: return InputMapControl::left_x;
        case InputAxis::left_y: return InputMapControl::left_y;
        case InputAxis::right_x: return InputMapControl::right_x;
        case InputAxis::right_y: return InputMapControl::right_y;
        case InputAxis::left_trigger: return InputMapControl::left_trigger;
        case InputAxis::right_trigger: return InputMapControl::right_trigger;
        default: return InputMapControl::count;
        }
    }
    return InputMapControl::count;
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

static uint32_t total_steps(const InputSdlMapper* mapper)
{
    return mapper->role_count + INPUT_MAP_CONTROL_COUNT;
}

// The steps: the positions first (0 .. INPUT_MAP_CONTROL_COUNT - 1), then the roles.
static bool is_role_step(uint32_t step)
{
    return step >= INPUT_MAP_CONTROL_COUNT;
}

int32_t input_sdl_mapper_role(const InputSdlMapper* mapper)
{
    if (!is_role_step(mapper->step) || mapper->step >= total_steps(mapper))
    {
        return -1;
    }
    return static_cast<int32_t>(mapper->step - INPUT_MAP_CONTROL_COUNT);
}

InputMapControl input_sdl_mapper_control(const InputSdlMapper* mapper)
{
    return is_role_step(mapper->step) ? InputMapControl::count : static_cast<InputMapControl>(mapper->step);
}

// Is this step wanted (a position if the layout has it; a role always)?
static bool step_wanted(const InputSdlMapper* mapper, uint32_t step)
{
    return is_role_step(step) || mapper->wanted[step];
}

static bool step_optional(const InputSdlMapper* mapper, uint32_t step)
{
    return is_role_step(step) ? mapper->role_optional[step - INPUT_MAP_CONTROL_COUNT]
                              : input_map_control_optional(static_cast<InputMapControl>(step));
}

// The next step is the next wanted one.
static void start_step(InputSdlMapper* mapper, uint64_t now_ms)
{
    while (mapper->step < total_steps(mapper) && !step_wanted(mapper, mapper->step))
    {
        mapper->step++;
    }
    mapper->waiting_release = true;
    mapper->step_started_ms = now_ms;
    mapper->holding.clear();
}

bool input_sdl_mapper_begin(InputSdlMapper* mapper, uint32_t joystick_id, uint32_t role_count, const bool* role_optional,
                            const bool* wanted, uint64_t now_ms)
{
    for (uint32_t c = 0; c < INPUT_MAP_CONTROL_COUNT; c++)
    {
        mapper->wanted[c] = wanted != nullptr ? wanted[c] : c < static_cast<uint32_t>(INPUT_MAP_FIRST_EXTRA);
    }
    mapper->role_count = role_count < INPUT_MAP_ROLES_MAX ? role_count : INPUT_MAP_ROLES_MAX;
    for (uint32_t r = 0; r < INPUT_MAP_ROLES_MAX; r++)
    {
        mapper->role_optional[r] = role_optional != nullptr && r < mapper->role_count && role_optional[r];
        mapper->roles[r].clear();
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
    mapper->kept.clear();
    mapper->end_on_skip_from = InputMapControl::count;
    mapper->step = 0;
    start_step(mapper, now_ms);
    return true;
}

static InputSource element_source(const InputSdlMapper* mapper, const std::string& element);

bool input_sdl_mapper_update(InputSdlMapper* mapper, uint64_t now_ms)
{
    if (!mapper->active)
    {
        return false;
    }
    if (mapper->step >= total_steps(mapper))
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
    bool role = is_role_step(mapper->step);
    InputMapControl control = role ? InputMapControl::south : static_cast<InputMapControl>(mapper->step);
    std::string element = pressed_element(mapper, control);
    // Positions can't repeat each other. A role may share a button with
    // anything (the menu's button is also a face button), but only one the
    // positions mapped: anything else wouldn't reach the interface.
    bool used = false;
    for (uint32_t c = 0; !role && c < mapper->step && !element.empty(); c++)
    {
        used = used || mapper->elements[c] == element;
    }
    if (role && !element.empty() && element_source(mapper, element).type == InputSourceType::none)
    {
        used = true;   // not on the mapping: wait for one that is
    }
    // A step takes a control held for hold_ms (the drawn pad shows
    // which one meanwhile): let go sooner and nothing is taken.
    if (!element.empty() && !used)
    {
        if (element != mapper->holding)
        {
            mapper->holding = element;
            mapper->holding_since_ms = now_ms;
        }
        mapper->step_started_ms = now_ms;   // an optional step doesn't skip while a control is held
        if (now_ms - mapper->holding_since_ms < mapper->hold_ms)
        {
            return false;
        }
    }
    else
    {
        mapper->holding.clear();
    }
    if (!element.empty() && !used)
    {
        mapper->holding.clear();
        if (role)
        {
            mapper->roles[mapper->step - INPUT_MAP_CONTROL_COUNT] = element;
        }
        else
        {
            mapper->elements[mapper->step] = element;
        }
        mapper->step++;
        start_step(mapper, now_ms);
    }
    else if (step_optional(mapper, mapper->step) && element.empty() && now_ms - mapper->step_started_ms >= mapper->optional_wait_ms)
    {
        if (!is_role_step(mapper->step) && mapper->step >= static_cast<uint32_t>(mapper->end_on_skip_from))
        {
            // "Any other buttons": none came, so there are no more. Nothing after it either.
            for (uint32_t c = mapper->step; c < INPUT_MAP_CONTROL_COUNT; c++)
            {
                mapper->wanted[c] = false;
            }
        }
        mapper->step++;   // the pad doesn't have it
        start_step(mapper, now_ms);
    }
    return mapper->step >= total_steps(mapper);
}

uint32_t input_sdl_mapper_seconds_left(const InputSdlMapper* mapper, uint64_t now_ms)
{
    if (!mapper->active || mapper->step >= total_steps(mapper) || !step_optional(mapper, mapper->step) || mapper->waiting_release)
    {
        return 0;
    }
    uint64_t elapsed = now_ms - mapper->step_started_ms;
    return elapsed >= mapper->optional_wait_ms ? 0 : static_cast<uint32_t>((mapper->optional_wait_ms - elapsed + 999) / 1000);
}

static void clear_step(InputSdlMapper* mapper, uint32_t step)
{
    if (!is_role_step(step))
    {
        mapper->elements[step].clear();
    }
    else if (step < total_steps(mapper))
    {
        mapper->roles[step - INPUT_MAP_CONTROL_COUNT].clear();
    }
}

void input_sdl_mapper_back(InputSdlMapper* mapper, uint64_t now_ms)
{
    // The previous wanted step.
    uint32_t step = mapper->step;
    while (mapper->active && step > 0)
    {
        step--;
        if (step_wanted(mapper, step))
        {
            mapper->step = step;
            clear_step(mapper, step);
            mapper->holding.clear();
            mapper->waiting_release = true;
            mapper->step_started_ms = now_ms;
            return;
        }
    }
}

void input_sdl_mapper_skip(InputSdlMapper* mapper, uint64_t now_ms)
{
    if (!mapper->active || mapper->step >= total_steps(mapper))
    {
        return;
    }
    clear_step(mapper, mapper->step);
    mapper->step++;
    start_step(mapper, now_ms);
}

void input_sdl_mapper_restart_step(InputSdlMapper* mapper, uint64_t now_ms)
{
    mapper->holding.clear();
    mapper->waiting_release = true;
    mapper->step_started_ms = now_ms;
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
// A position as the gamepad source SDL's mapping makes of it.
static InputSource position_source(InputMapControl control)
{
    static const InputButton BUTTONS[] = {
        InputButton::south, InputButton::east, InputButton::west, InputButton::north, InputButton::dpad_up,
        InputButton::dpad_down, InputButton::dpad_left, InputButton::dpad_right, InputButton::left_shoulder,
        InputButton::right_shoulder, InputButton::back, InputButton::start,
    };
    InputSource source = {};
    uint32_t index = static_cast<uint32_t>(control);
    if (index < sizeof(BUTTONS) / sizeof(BUTTONS[0]))
    {
        source.type = InputSourceType::button;
        source.code = static_cast<uint16_t>(BUTTONS[index]);
        return source;
    }
    switch (control)
    {
    case InputMapControl::left_trigger:
    case InputMapControl::right_trigger:
        source.type = InputSourceType::axis_plus;
        source.code = static_cast<uint16_t>(control == InputMapControl::left_trigger ? InputAxis::left_trigger : InputAxis::right_trigger);
        break;
    case InputMapControl::left_stick:
        source.type = InputSourceType::button;
        source.code = static_cast<uint16_t>(InputButton::left_stick);
        break;
    case InputMapControl::right_stick:
        source.type = InputSourceType::button;
        source.code = static_cast<uint16_t>(InputButton::right_stick);
        break;
    case InputMapControl::guide:
        source.type = InputSourceType::button;
        source.code = static_cast<uint16_t>(InputButton::guide);
        break;
    default:
        break;
    }
    // The extras: buttons, in the order of InputMapControl's extras.
    static const InputButton EXTRAS[] = {
        InputButton::right_paddle1, InputButton::left_paddle1, InputButton::right_paddle2, InputButton::left_paddle2,
        InputButton::misc5, InputButton::misc6, InputButton::misc1, InputButton::misc2, InputButton::misc3, InputButton::misc4,
    };
    uint32_t first_extra = static_cast<uint32_t>(INPUT_MAP_FIRST_EXTRA);
    if (index >= first_extra && index < INPUT_MAP_CONTROL_COUNT)
    {
        source.type = InputSourceType::button;
        source.code = static_cast<uint16_t>(EXTRAS[index - first_extra]);
    }
    return source;
}

// What a raw element is on the mapping made so far: a gamepad button, or an
// axis half (a button pressed on a stick's axis), or none.
static InputSource element_source(const InputSdlMapper* mapper, const std::string& element)
{
    InputSource none = {};
    if (element.empty())
    {
        return none;
    }
    for (uint32_t c = 0; c < INPUT_MAP_CONTROL_COUNT; c++)
    {
        InputMapControl control = static_cast<InputMapControl>(c);
        const std::string& mapped = mapper->elements[c];
        if (mapped.empty())
        {
            continue;
        }
        if (mapped == element && position_source(control).type != InputSourceType::none)
        {
            return position_source(control);
        }
        // A role on half of an axis that turned out to be a stick ("+a0" on leftx "a0").
        bool stick = is_stick(control);
        if (stick && element.size() >= 3 && (element[0] == '+' || element[0] == '-'))
        {
            std::string axis = element.substr(1);
            bool inverted = mapped.size() > 1 && mapped[mapped.size() - 1] == '~';
            std::string base = inverted ? mapped.substr(0, mapped.size() - 1) : mapped;
            if (base == axis)
            {
                static const InputAxis AXES[] = {InputAxis::left_x, InputAxis::left_y, InputAxis::right_x, InputAxis::right_y};
                bool plus = (element[0] == '+') != inverted;
                InputSource source = {};
                source.type = plus ? InputSourceType::axis_plus : InputSourceType::axis_minus;
                source.code = static_cast<uint16_t>(AXES[c - static_cast<uint32_t>(InputMapControl::left_x)]);
                return source;
            }
        }
    }
    return none;
}

InputSource input_sdl_mapper_role_source(const InputSdlMapper* mapper, uint32_t role)
{
    InputSource none = {};
    if (role >= mapper->role_count)
    {
        return none;
    }
    return element_source(mapper, mapper->roles[role]);
}

InputSource input_sdl_mapper_held_source(const InputSdlMapper* mapper)
{
    InputSource none = {};
    if (!mapper->active || mapper->joystick == nullptr)
    {
        return none;
    }
    return element_source(mapper, pressed_element(mapper, InputMapControl::south));
}

bool input_sdl_mapper_finish(InputSdlMapper* mapper, std::string* line)
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
    text += mapper->kept;   // what a loaded mapping had that this mapper doesn't know
    text += std::string("platform:") + SDL_GetPlatform() + ",";
    input_sdl_mapper_cancel(mapper);
    if (!any || SDL_AddGamepadMapping(text.c_str()) < 0)
    {
        SDL_Log("Mapping refused (%s): %s", SDL_GetError(), text.c_str());
        return false;
    }
    SDL_Log("Mapping kept: %s", text.c_str());
    *line = text;
    return true;
}

float input_sdl_mapper_hold_progress(const InputSdlMapper* mapper, uint64_t now_ms)
{
    if (!mapper->active || mapper->holding.empty())
    {
        return 0.0f;
    }
    uint64_t held = now_ms - mapper->holding_since_ms;
    return held >= mapper->hold_ms ? 1.0f : static_cast<float>(held) / static_cast<float>(mapper->hold_ms);
}

void input_sdl_mapper_load(InputSdlMapper* mapper, const char* mapping)
{
    mapper->kept.clear();
    if (mapping == nullptr)
    {
        return;
    }
    // "<guid>,<name>,field:value,...": skip the GUID and the name.
    const char* field = mapping;
    for (int commas = 0; commas < 2 && field != nullptr; commas++)
    {
        field = std::strchr(field, ',');
        field = field != nullptr ? field + 1 : nullptr;
    }
    while (field != nullptr && *field != '\0')
    {
        const char* end = std::strchr(field, ',');
        std::string text = end != nullptr ? std::string(field, static_cast<size_t>(end - field)) : std::string(field);
        field = end != nullptr ? end + 1 : nullptr;
        size_t colon = text.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        std::string name = text.substr(0, colon);
        std::string value = text.substr(colon + 1);
        if (name == "platform")
        {
            continue;   // finish writes this platform's
        }
        bool known = false;
        for (uint32_t c = 0; c < INPUT_MAP_CONTROL_COUNT && !known; c++)
        {
            if (name == SDL_NAMES[c])
            {
                known = true;
                if (!mapper->wanted[c])
                {
                    mapper->elements[c] = value;   // a position asked keeps what is pressed now
                }
            }
        }
        if (!known)
        {
            mapper->kept += text + ",";
        }
    }
}

void input_sdl_mapper_skip_roles(InputSdlMapper* mapper)
{
    if (!mapper->active)
    {
        return;
    }
    for (uint32_t r = 0; r < mapper->role_count; r++)
    {
        mapper->roles[r].clear();
    }
    mapper->step = total_steps(mapper);
    mapper->holding.clear();
}

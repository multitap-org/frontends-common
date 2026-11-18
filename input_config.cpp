#include "input_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

constexpr int32_t DEFAULT_DEADZONE_PERCENT = 15;

static const char PREFIX_CONTROLLER[] = "controller/";
static const char PREFIX_RETROPAD[] = "retropad/";
static const char PREFIX_UI[] = "ui/";
static const char PREFIX_CORE[] = "core:";
static const char PREFIX_GAME[] = "game:";      // game:<core>/<device>/<controller>/<game> (a game's own remap)
static const char PREFIX_COMBOS[] = "combos:";  // combos:<core>, <id> = pad.x+pad.y | NAME
static const char PREFIX_MACROS[] = "macros:";  // macros:<core>, <id> = pad.down:3 pad.down+pad.right:3 -:4 | once | NAME

static void write_core_profiles(const InputSystem* system, SettingsFile* file);

// "snes9x/257/<profile id>" -> core, device, profile id.
static bool split_core_section(const std::string& rest, std::string* core, uint32_t* device, std::string* profile_id)
{
    size_t first = rest.find('/');
    size_t second = first == std::string::npos ? std::string::npos : rest.find('/', first + 1);
    if (second == std::string::npos || first == 0 || second == first + 1 || second + 1 >= rest.size())
    {
        return false;
    }
    std::string number = rest.substr(first + 1, second - first - 1);
    char* end = nullptr;
    unsigned long value = std::strtoul(number.c_str(), &end, 10);
    if (end == number.c_str() || *end != '\0')
    {
        return false;
    }
    *core = rest.substr(0, first);
    *device = static_cast<uint32_t>(value);
    *profile_id = rest.substr(second + 1);
    return true;
}

static bool starts_with(const std::string& text, const char* prefix, std::string* rest)
{
    size_t length = std::strlen(prefix);
    if (text.compare(0, length, prefix) != 0)
    {
        return false;
    }
    *rest = text.substr(length);
    return !rest->empty();
}

static InputProfile* profile_for(InputSystem* system, const std::string& id)
{
    for (size_t p = 0; p < system->profiles.size(); p++)
    {
        if (system->profiles[p].id == id)
        {
            return &system->profiles[p];
        }
    }
    InputProfile profile = {};
    profile.id = id;
    system->profiles.push_back(profile);
    return &system->profiles.back();
}

// "button:east key:A" -> up to INPUT_SOURCES_PER_ACTION sources; "none" = none.
static bool parse_sources(const std::string& text, InputSource* sources)
{
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        sources[s] = InputSource{};
    }
    uint32_t count = 0;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(' ', start);
        end = end == std::string::npos ? text.size() : end;
        std::string word = text.substr(start, end - start);
        start = end + 1;
        if (word.empty())
        {
            continue;
        }
        InputSource source = {};
        if (!input_source_parse(word.c_str(), &source))
        {
            return false;
        }
        if (source.type != InputSourceType::none && count < INPUT_SOURCES_PER_ACTION)
        {
            sources[count++] = source;
        }
    }
    return true;
}

static void read_bindings(InputProfile* profile, const SettingsFileSection* section, bool retropad)
{
    for (size_t e = 0; e < section->entries.size(); e++)
    {
        const SettingsFileEntry* entry = &section->entries[e];
        InputAction action = InputAction::count;
        if (!input_action_parse(entry->key.c_str(), &action) || input_action_is_pad(action) != retropad)
        {
            std::fprintf(stderr, "input.cfg: [%s] %s: not a binding here, ignored\n", section->name.c_str(),
                         entry->key.c_str());
            continue;
        }
        InputBindingOverride change = {};
        change.action = action;
        if (!parse_sources(entry->value, change.sources))
        {
            std::fprintf(stderr, "input.cfg: [%s] %s = %s: not understood, ignored\n", section->name.c_str(),
                         entry->key.c_str(), entry->value.c_str());
            continue;
        }
        bool replaced = false;
        for (size_t o = 0; o < profile->overrides.size(); o++)
        {
            if (profile->overrides[o].action == action)
            {
                profile->overrides[o] = change;
                replaced = true;
            }
        }
        if (!replaced)
        {
            profile->overrides.push_back(change);
        }
    }
}

// `with_game`: game:<core>/<device>/<controller>/<game>, else core:<core>/<device>/<controller>.
static void read_core_profile(InputSystem* system, const SettingsFileSection* section, const std::string& rest, bool with_game)
{
    std::string core;
    uint32_t device = 0;
    std::string profile_id;
    std::string game;
    bool understood = split_core_section(rest, &core, &device, &profile_id);
    if (understood && with_game)
    {
        size_t slash = profile_id.find('/');
        understood = slash != std::string::npos && slash > 0 && slash + 1 < profile_id.size();
        if (understood)
        {
            game = profile_id.substr(slash + 1);
            profile_id = profile_id.substr(0, slash);
        }
    }
    if (!understood)
    {
        std::fprintf(stderr, "input.cfg: [%s]: not core/device/controller(/game), ignored\n", section->name.c_str());
        return;
    }
    InputCoreProfile* profile = input_core_profile_for(system, core.c_str(), game.c_str(), device, profile_id);
    for (size_t e = 0; e < section->entries.size(); e++)
    {
        const SettingsFileEntry* entry = &section->entries[e];
        if (entry->key == "remap")
        {
            profile->decided = true;
            profile->custom = entry->value == "custom";
            continue;
        }
        bool combo_key = entry->key.compare(0, 6, "combo.") == 0 && entry->key.size() > 6;
        bool macro_key = entry->key.compare(0, 6, "macro.") == 0 && entry->key.size() > 6;
        if (combo_key || macro_key)
        {
            InputComboBinding binding = {};
            binding.combo = entry->key.substr(6);
            if (parse_sources(entry->value, binding.sources))
            {
                profile->combos.push_back(binding);
            }
            else
            {
                std::fprintf(stderr, "input.cfg: [%s] %s: not understood, ignored\n", section->name.c_str(), entry->key.c_str());
            }
            continue;
        }
        InputAction action = InputAction::count;
        InputBindingOverride change = {};
        if (!input_action_parse(entry->key.c_str(), &action) || !input_action_is_pad(action) ||
            !parse_sources(entry->value, change.sources))
        {
            std::fprintf(stderr, "input.cfg: [%s] %s: not understood, ignored\n", section->name.c_str(), entry->key.c_str());
            continue;
        }
        change.action = action;
        profile->overrides.push_back(change);
    }
}

// "pad.x+pad.y" as RetroPad button bits; false if not understood.
static bool parse_buttons(const std::string& text, uint16_t* buttons)
{
    *buttons = 0;
    size_t at = 0;
    while (at <= text.size())
    {
        size_t plus = text.find('+', at);
        std::string part = text.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
        size_t first = part.find_first_not_of(' ');
        size_t last = part.find_last_not_of(' ');
        part = first != std::string::npos ? part.substr(first, last - first + 1) : std::string();
        InputAction action = InputAction::count;
        if (!input_action_parse(part.c_str(), &action) || static_cast<uint32_t>(action) >= INPUT_PAD_BUTTON_COUNT)
        {
            return false;
        }
        *buttons = static_cast<uint16_t>(*buttons | (1u << static_cast<uint32_t>(action)));
        if (plus == std::string::npos)
        {
            return true;
        }
        at = plus + 1;
    }
    return true;
}

static std::string buttons_text(uint16_t buttons)
{
    std::string text;
    for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
    {
        if ((buttons & (1u << b)) != 0)
        {
            text += text.empty() ? "" : "+";
            text += input_action_name(static_cast<InputAction>(b));
        }
    }
    return text;
}

static std::string trimmed(const std::string& text)
{
    size_t first = text.find_first_not_of(' ');
    size_t last = text.find_last_not_of(' ');
    return first != std::string::npos ? text.substr(first, last - first + 1) : std::string();
}

// <id> = pad.down:3 pad.down+pad.right:3 -:4 | once | HADOUKEN ("-" = a
// pause; "repeat" = again while held; the name may be missing).
static void read_macros(InputSystem* system, const SettingsFileSection* section, const std::string& core)
{
    for (size_t e = 0; e < section->entries.size(); e++)
    {
        const SettingsFileEntry* entry = &section->entries[e];
        const std::string& value = entry->value;
        size_t bar = value.find('|');
        size_t bar2 = bar != std::string::npos ? value.find('|', bar + 1) : std::string::npos;
        std::string steps_text = value.substr(0, bar);
        std::string mode = bar != std::string::npos ? trimmed(value.substr(bar + 1, bar2 == std::string::npos ? std::string::npos
                                                                                                            : bar2 - bar - 1))
                                                    : std::string("once");
        InputMacro macro = {};
        macro.core = core;
        macro.id = entry->key;
        macro.name = bar2 != std::string::npos ? trimmed(value.substr(bar2 + 1)) : std::string();
        macro.repeat = mode == "repeat";
        bool understood = !entry->key.empty() && (mode == "once" || mode == "repeat");
        size_t at = 0;
        while (understood && at < steps_text.size())
        {
            size_t end = steps_text.find(' ', at);
            end = end == std::string::npos ? steps_text.size() : end;
            std::string word = steps_text.substr(at, end - at);
            at = end + 1;
            if (word.empty())
            {
                continue;
            }
            size_t colon = word.rfind(':');
            InputMacroStep step = {};
            char* number_end = nullptr;
            unsigned long frames = colon != std::string::npos ? std::strtoul(word.c_str() + colon + 1, &number_end, 10) : 0;
            understood = colon != std::string::npos && number_end != nullptr && *number_end == '\0' && frames >= 1 && frames <= 9999;
            std::string buttons = colon != std::string::npos ? word.substr(0, colon) : std::string();
            understood = understood && (buttons == "-" || parse_buttons(buttons, &step.buttons));
            step.frames = static_cast<uint16_t>(frames);
            if (understood)
            {
                macro.steps.push_back(step);
            }
        }
        if (!understood || macro.steps.empty())
        {
            std::fprintf(stderr, "input.cfg: [%s] %s: not understood, ignored\n", section->name.c_str(), entry->key.c_str());
            continue;
        }
        system->macros.push_back(macro);
    }
}

// <id> = pad.x+pad.y+pad.l | THREE PUNCHES (the name may be missing).
static void read_combos(InputSystem* system, const SettingsFileSection* section, const std::string& core)
{
    for (size_t e = 0; e < section->entries.size(); e++)
    {
        const SettingsFileEntry* entry = &section->entries[e];
        const std::string& value = entry->value;
        size_t bar = value.find('|');
        std::string buttons_text = value.substr(0, bar);
        std::string name = bar != std::string::npos ? value.substr(bar + 1) : std::string();
        size_t name_start = name.find_first_not_of(' ');
        name = name_start != std::string::npos ? name.substr(name_start) : std::string();
        uint16_t buttons = 0;
        size_t at = 0;
        bool understood = true;
        while (at <= buttons_text.size())
        {
            size_t plus = buttons_text.find('+', at);
            std::string part = buttons_text.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
            size_t first = part.find_first_not_of(' ');
            size_t last = part.find_last_not_of(' ');
            part = first != std::string::npos ? part.substr(first, last - first + 1) : std::string();
            InputAction action = InputAction::count;
            if (!input_action_parse(part.c_str(), &action) || static_cast<uint32_t>(action) >= INPUT_PAD_BUTTON_COUNT)
            {
                understood = false;
                break;
            }
            buttons = static_cast<uint16_t>(buttons | (1u << static_cast<uint32_t>(action)));
            if (plus == std::string::npos)
            {
                break;
            }
            at = plus + 1;
        }
        if (!understood || buttons == 0 || entry->key.empty())
        {
            std::fprintf(stderr, "input.cfg: [%s] %s: not understood, ignored\n", section->name.c_str(), entry->key.c_str());
            continue;
        }
        InputCombo combo = {};
        combo.core = core;
        combo.id = entry->key;
        combo.name = name;
        combo.buttons = buttons;
        system->combos.push_back(combo);
    }
}

void input_config_read(InputSystem* system, const SettingsFile* file)
{
    system->profiles.clear();
    system->core_profiles.clear();
    system->combos.clear();
    system->macros.clear();
    for (size_t s = 0; s < file->sections.size(); s++)
    {
        const SettingsFileSection* section = &file->sections[s];
        std::string id;
        if (starts_with(section->name, PREFIX_CONTROLLER, &id))
        {
            InputProfile* profile = profile_for(system, id);
            const char* name = settings_file_get(file, section->name.c_str(), "name");
            profile->name = name != nullptr ? name : "";
            const char* mode = settings_file_get(file, section->name.c_str(), "hotkey_mode");
            if (mode != nullptr)
            {
                input_hotkey_mode_parse(mode, &profile->hotkey_mode);
            }
            const char* appearance = settings_file_get(file, section->name.c_str(), "appearance");
            profile->appearance = appearance != nullptr ? appearance : "";
            const char* mapping = settings_file_get(file, section->name.c_str(), "sdl_mapping");
            profile->sdl_mapping = mapping != nullptr ? mapping : "";
            const char* ignore = settings_file_get(file, section->name.c_str(), "ignore");
            if (ignore != nullptr)
            {
                profile->ignore = std::strcmp(ignore, "yes") == 0 ? InputIgnore::yes
                                  : std::strcmp(ignore, "no") == 0 ? InputIgnore::no : InputIgnore::automatic;
            }
            const char* deadzone = settings_file_get(file, section->name.c_str(), "deadzone");
            if (deadzone != nullptr)
            {
                char* end = nullptr;
                long percent = std::strtol(deadzone, &end, 10);
                profile->deadzone_percent = end != deadzone && *end == '\0' && percent >= 0 && percent < 100
                                                ? static_cast<int32_t>(percent) : -1;
            }
            // label.<button> = a name given to it.
            for (size_t e = 0; e < section->entries.size(); e++)
            {
                const std::string& key = section->entries[e].key;
                InputSource button = {};
                if (key.compare(0, 6, "label.") == 0 && input_source_parse(("button:" + key.substr(6)).c_str(), &button) &&
                    button.type == InputSourceType::button && button.code < INPUT_BUTTON_COUNT)
                {
                    profile->button_labels[button.code] = section->entries[e].value;
                }
            }
        }
        else if (starts_with(section->name, PREFIX_RETROPAD, &id))
        {
            read_bindings(profile_for(system, id), section, true);
        }
        else if (starts_with(section->name, PREFIX_UI, &id))
        {
            read_bindings(profile_for(system, id), section, false);
        }
        else if (starts_with(section->name, PREFIX_CORE, &id))
        {
            read_core_profile(system, section, id, false);
        }
        else if (starts_with(section->name, PREFIX_GAME, &id))
        {
            read_core_profile(system, section, id, true);
        }
        else if (starts_with(section->name, PREFIX_COMBOS, &id))
        {
            read_combos(system, section, id);
        }
        else if (starts_with(section->name, PREFIX_MACROS, &id))
        {
            read_macros(system, section, id);
        }
    }
}

// The same sources, in any order?
static bool same_sources(const InputSource* a, const InputSource* b)
{
    uint32_t count_a = 0;
    uint32_t count_b = 0;
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        count_a += a[s].type != InputSourceType::none ? 1u : 0u;
        count_b += b[s].type != InputSourceType::none ? 1u : 0u;
        if (a[s].type == InputSourceType::none)
        {
            continue;
        }
        bool found = false;
        for (uint32_t t = 0; t < INPUT_SOURCES_PER_ACTION && !found; t++)
        {
            found = b[t].type != InputSourceType::none && input_source_equal(a[s], b[t]);
        }
        if (!found)
        {
            return false;
        }
    }
    return count_a == count_b;
}

// The controllers as profiles (replacing those with the same id). The
// keyboard's profiles are all made again: parts may have been removed.
static void take_controllers(InputSystem* system)
{
    bool keyboard_here = false;
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        const InputDevice* device = input_devices_find_const(&system->devices, system->controllers[c].device_id);
        keyboard_here = keyboard_here || (device != nullptr && device->kind == InputDeviceKind::keyboard);
    }
    for (size_t p = system->profiles.size(); keyboard_here && p > 0; p--)
    {
        const std::string& id = system->profiles[p - 1].id;
        if (id == "keyboard" || id.compare(0, 9, "keyboard.") == 0)
        {
            system->profiles.erase(system->profiles.begin() + static_cast<long>(p - 1));
        }
    }
    for (size_t c = 0; c < system->controllers.size(); c++)
    {
        const InputController* controller = &system->controllers[c];
        const InputDevice* device = input_devices_find_const(&system->devices, controller->device_id);
        if (device == nullptr)
        {
            continue;
        }
        InputProfile* profile = profile_for(system, input_controller_profile_id(system, static_cast<uint32_t>(c)));
        profile->name = controller->name == device->name ? "" : controller->name;
        profile->ignore = device->ignore;
        profile->hotkey_mode = controller->hotkey_mode;
        int32_t percent = static_cast<int32_t>(controller->deadzone * 100.0f + 0.5f);
        profile->deadzone_percent = percent == DEFAULT_DEADZONE_PERCENT ? -1 : percent;
        profile->overrides.clear();
        InputBindingSet defaults = {};
        input_default_bindings_for(device, &defaults);
        for (uint32_t a = 0; a < INPUT_ACTION_COUNT; a++)
        {
            if (same_sources(controller->bindings.sources[a], defaults.sources[a]))
            {
                continue;
            }
            InputBindingOverride change = {};
            change.action = static_cast<InputAction>(a);
            for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
            {
                change.sources[s] = controller->bindings.sources[a][s];
            }
            profile->overrides.push_back(change);
        }
    }
}

static std::string sources_text(const InputSource* sources)
{
    std::string text;
    for (uint32_t s = 0; s < INPUT_SOURCES_PER_ACTION; s++)
    {
        char word[64] = {};
        if (sources[s].type == InputSourceType::none || !input_source_format(sources[s], word, sizeof(word)))
        {
            continue;
        }
        text += text.empty() ? "" : " ";
        text += word;
    }
    return text.empty() ? std::string("none") : text;
}

void input_config_write(InputSystem* system, SettingsFile* file)
{
    take_controllers(system);
    *file = SettingsFile{};
    for (size_t p = 0; p < system->profiles.size(); p++)
    {
        const InputProfile* profile = &system->profiles[p];
        std::string controller = PREFIX_CONTROLLER + profile->id;
        if (!profile->name.empty())
        {
            settings_file_set(file, controller.c_str(), "name", profile->name.c_str());
        }
        if (profile->hotkey_mode != InputHotkeyMode::hold)
        {
            settings_file_set(file, controller.c_str(), "hotkey_mode", input_hotkey_mode_name(profile->hotkey_mode));
        }
        if (!profile->appearance.empty())
        {
            settings_file_set(file, controller.c_str(), "appearance", profile->appearance.c_str());
        }
        for (uint32_t b = 0; b < INPUT_BUTTON_COUNT; b++)
        {
            InputSource button = {};
            button.type = InputSourceType::button;
            button.code = static_cast<uint16_t>(b);
            char name[64] = {};
            if (!profile->button_labels[b].empty() && input_source_format(button, name, sizeof(name)))
            {
                settings_file_set(file, controller.c_str(), (std::string("label.") + (name + 7)).c_str(),
                                  profile->button_labels[b].c_str());   // name + 7: past "button:"
            }
        }
        if (!profile->sdl_mapping.empty())
        {
            settings_file_set(file, controller.c_str(), "sdl_mapping", profile->sdl_mapping.c_str());
        }
        if (profile->ignore != InputIgnore::automatic)
        {
            settings_file_set(file, controller.c_str(), "ignore", profile->ignore == InputIgnore::yes ? "yes" : "no");
        }
        if (profile->deadzone_percent >= 0)
        {
            settings_file_set(file, controller.c_str(), "deadzone", std::to_string(profile->deadzone_percent).c_str());
        }
        for (size_t o = 0; o < profile->overrides.size(); o++)
        {
            const InputBindingOverride* change = &profile->overrides[o];
            std::string section = (input_action_is_pad(change->action) ? PREFIX_RETROPAD : PREFIX_UI) + profile->id;
            settings_file_set(file, section.c_str(), input_action_name(change->action), sources_text(change->sources).c_str());
        }
    }
    write_core_profiles(system, file);
}

static void write_core_profiles(const InputSystem* system, SettingsFile* file)
{
    for (size_t p = 0; p < system->core_profiles.size(); p++)
    {
        const InputCoreProfile* profile = &system->core_profiles[p];
        bool game = !profile->game.empty();
        if (!game && !profile->decided && profile->overrides.empty() && profile->combos.empty())
        {
            continue;   // (a game's is kept even untouched: it was asked for)
        }
        std::string section = (game ? PREFIX_GAME : PREFIX_CORE) + profile->core + "/" + std::to_string(profile->device) + "/" +
                              profile->profile_id + (game ? "/" + profile->game : std::string());
        settings_file_set(file, section.c_str(), "remap", profile->custom ? "custom" : "retropad");
        for (size_t o = 0; o < profile->overrides.size(); o++)
        {
            const InputBindingOverride* change = &profile->overrides[o];
            settings_file_set(file, section.c_str(), input_action_name(change->action), sources_text(change->sources).c_str());
        }
        for (size_t k = 0; k < profile->combos.size(); k++)
        {
            const InputComboBinding* binding = &profile->combos[k];
            const char* kind = !binding->combo.empty() && binding->combo[0] == 'm' ? "macro." : "combo.";
            settings_file_set(file, section.c_str(), (kind + binding->combo).c_str(), sources_text(binding->sources).c_str());
        }
    }
    for (size_t c = 0; c < system->combos.size(); c++)
    {
        const InputCombo* combo = &system->combos[c];
        std::string buttons;
        for (uint32_t b = 0; b < INPUT_PAD_BUTTON_COUNT; b++)
        {
            if ((combo->buttons & (1u << b)) != 0)
            {
                buttons += (buttons.empty() ? "" : "+");
                buttons += input_action_name(static_cast<InputAction>(b));
            }
        }
        std::string value = combo->name.empty() ? buttons : buttons + " | " + combo->name;
        settings_file_set(file, (PREFIX_COMBOS + combo->core).c_str(), combo->id.c_str(), value.c_str());
    }
    for (size_t m = 0; m < system->macros.size(); m++)
    {
        const InputMacro* macro = &system->macros[m];
        std::string value;
        for (size_t s = 0; s < macro->steps.size(); s++)
        {
            const InputMacroStep* step = &macro->steps[s];
            value += value.empty() ? "" : " ";
            value += (step->buttons == 0 ? std::string("-") : buttons_text(step->buttons)) + ":" + std::to_string(step->frames);
        }
        if (value.empty())
        {
            continue;   // (nothing to play)
        }
        value += macro->repeat ? " | repeat" : " | once";
        value += macro->name.empty() ? std::string() : " | " + macro->name;
        settings_file_set(file, (PREFIX_MACROS + macro->core).c_str(), macro->id.c_str(), value.c_str());
    }
}

bool input_config_load(InputSystem* system, const char* path)
{
    SettingsFile file = {};
    if (!settings_file_load(&file, path))
    {
        return false;
    }
    input_config_read(system, &file);
    return true;
}

bool input_config_save(InputSystem* system, const char* path)
{
    SettingsFile file = {};
    input_config_write(system, &file);
    return settings_file_save(&file, path, "Input: controllers and their bindings (only what differs from the defaults)");
}

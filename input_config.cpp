#include "input_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

constexpr int32_t DEFAULT_DEADZONE_PERCENT = 15;

static const char PREFIX_CONTROLLER[] = "controller/";
static const char PREFIX_RETROPAD[] = "retropad/";
static const char PREFIX_UI[] = "ui/";
static const char PREFIX_CORE[] = "core:";

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

static void read_core_profile(InputSystem* system, const SettingsFileSection* section, const std::string& rest)
{
    std::string core;
    uint32_t device = 0;
    std::string profile_id;
    if (!split_core_section(rest, &core, &device, &profile_id))
    {
        std::fprintf(stderr, "input.cfg: [%s]: not core/device/controller, ignored\n", section->name.c_str());
        return;
    }
    InputCoreProfile* profile = input_core_profile_for(system, core.c_str(), device, profile_id);
    for (size_t e = 0; e < section->entries.size(); e++)
    {
        const SettingsFileEntry* entry = &section->entries[e];
        if (entry->key == "remap")
        {
            profile->decided = true;
            profile->custom = entry->value == "custom";
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

void input_config_read(InputSystem* system, const SettingsFile* file)
{
    system->profiles.clear();
    system->core_profiles.clear();
    for (size_t s = 0; s < file->sections.size(); s++)
    {
        const SettingsFileSection* section = &file->sections[s];
        std::string id;
        if (starts_with(section->name, PREFIX_CONTROLLER, &id))
        {
            InputProfile* profile = profile_for(system, id);
            const char* name = settings_file_get(file, section->name.c_str(), "name");
            profile->name = name != nullptr ? name : "";
            const char* appearance = settings_file_get(file, section->name.c_str(), "appearance");
            profile->appearance = appearance != nullptr ? appearance : "";
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
            read_core_profile(system, section, id);
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
        if (!profile->appearance.empty())
        {
            settings_file_set(file, controller.c_str(), "appearance", profile->appearance.c_str());
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
        if (!profile->decided && profile->overrides.empty())
        {
            continue;
        }
        std::string section = PREFIX_CORE + profile->core + "/" + std::to_string(profile->device) + "/" + profile->profile_id;
        settings_file_set(file, section.c_str(), "remap", profile->custom ? "custom" : "retropad");
        for (size_t o = 0; o < profile->overrides.size(); o++)
        {
            const InputBindingOverride* change = &profile->overrides[o];
            settings_file_set(file, section.c_str(), input_action_name(change->action), sources_text(change->sources).c_str());
        }
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

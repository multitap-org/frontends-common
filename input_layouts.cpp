#include "input_layouts.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>

// The controls a layout names in [labels] (the d-pad and the stick axes
// have no label; dpad / sticks say whether the pad has them).
static bool labelled(InputMapControl control)
{
    switch (control)
    {
    case InputMapControl::dpad_up:
    case InputMapControl::dpad_down:
    case InputMapControl::dpad_left:
    case InputMapControl::dpad_right:
    case InputMapControl::left_x:
    case InputMapControl::left_y:
    case InputMapControl::right_x:
    case InputMapControl::right_y:
        return false;
    default:
        return true;
    }
}

static void sort_layouts(InputLayouts* layouts)
{
    // Insertion sort: a handful of layouts.
    for (size_t i = 1; i < layouts->layouts.size(); i++)
    {
        for (size_t j = i; j > 0; j--)
        {
            const InputLayout& a = layouts->layouts[j - 1];
            const InputLayout& b = layouts->layouts[j];
            if (a.order < b.order || (a.order == b.order && a.name <= b.name))
            {
                break;
            }
            InputLayout moved = layouts->layouts[j];
            layouts->layouts[j] = layouts->layouts[j - 1];
            layouts->layouts[j - 1] = moved;
        }
    }
}

bool input_layouts_add(InputLayouts* layouts, const char* maker, const char* model, const SettingsFile* file)
{
    InputLayout layout = {};
    layout.maker = maker;
    layout.id = std::string(maker) + "/" + model;
    const char* name = settings_file_get(file, "layout", "name");
    layout.name = name != nullptr ? name : model;
    const char* order = settings_file_get(file, "layout", "order");
    if (order != nullptr)
    {
        layout.order = static_cast<int32_t>(std::strtol(order, nullptr, 10));
    }
    const char* types = settings_file_get(file, "layout", "sdl_types");
    if (types != nullptr)
    {
        std::string text = types;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(' ', start);
            end = end == std::string::npos ? text.size() : end;
            if (end > start)
            {
                layout.sdl_types.push_back(text.substr(start, end - start));
            }
            start = end + 1;
        }
    }
    const char* dpad = settings_file_get(file, "layout", "dpad");
    layout.dpad = dpad == nullptr || std::strcmp(dpad, "no") != 0;
    const char* face = settings_file_get(file, "layout", "face");
    layout.six_face = face != nullptr && std::strcmp(face, "six") == 0;
    const char* sticks = settings_file_get(file, "layout", "sticks");
    if (sticks != nullptr)
    {
        long count = std::strtol(sticks, nullptr, 10);
        layout.sticks = count <= 0 ? 0u : (count >= 2 ? 2u : 1u);
    }
    for (uint32_t c = 0; c < INPUT_MAP_CONTROL_COUNT; c++)
    {
        InputMapControl control = static_cast<InputMapControl>(c);
        const char* label = labelled(control) ? settings_file_get(file, "labels", input_map_control_sdl_name(control)) : nullptr;
        layout.labels[c] = label != nullptr ? label : "";
    }
    for (size_t l = 0; l < layouts->layouts.size(); l++)
    {
        if (layouts->layouts[l].id == layout.id)
        {
            layouts->layouts[l] = layout;
            sort_layouts(layouts);
            return true;
        }
    }
    layouts->layouts.push_back(layout);
    sort_layouts(layouts);
    return true;
}

void input_layouts_load_directory(InputLayouts* layouts, const char* directory)
{
    int count = 0;
    char** files = SDL_GlobDirectory(directory, "*/*.cfg", 0, &count);
    for (int f = 0; files != nullptr && f < count; f++)
    {
        std::string relative = files[f];   // "<Maker>/<model>.cfg"
        for (size_t i = 0; i < relative.size(); i++)
        {
            relative[i] = relative[i] == '\\' ? '/' : relative[i];
        }
        size_t slash = relative.rfind('/');
        if (slash == std::string::npos || slash == 0 || relative.size() <= slash + 5)
        {
            continue;
        }
        std::string maker = relative.substr(0, slash);
        std::string model = relative.substr(slash + 1, relative.size() - slash - 5);   // without ".cfg"
        std::string path = std::string(directory) + "/" + relative;
        SettingsFile file = {};
        if (settings_file_load(&file, path.c_str()))
        {
            input_layouts_add(layouts, maker.c_str(), model.c_str(), &file);
        }
    }
    SDL_free(files);
}

void input_layouts_ensure_generic(InputLayouts* layouts)
{
    if (input_layouts_find(layouts, "Generic/generic") != nullptr)
    {
        return;
    }
    static const char TEXT[] =
        "[layout]\nname = GENERIC\norder = 0\n"
        "[labels]\na = BOTTOM\nb = RIGHT\nx = LEFT\ny = TOP\nleftshoulder = L\nrightshoulder = R\n"
        "lefttrigger = L2\nrighttrigger = R2\nback = SELECT\nstart = START\nguide = HOME\nleftstick = L3\nrightstick = R3\n";
    SettingsFile file = {};
    settings_file_parse(&file, TEXT, sizeof(TEXT) - 1, "built-in generic layout");
    input_layouts_add(layouts, "Generic", "generic", &file);
}

void input_layouts_makers(const InputLayouts* layouts, std::vector<std::string>* makers)
{
    makers->clear();
    for (size_t l = 0; l < layouts->layouts.size(); l++)
    {
        bool seen = false;
        for (size_t m = 0; m < makers->size() && !seen; m++)
        {
            seen = (*makers)[m] == layouts->layouts[l].maker;
        }
        if (!seen)
        {
            makers->push_back(layouts->layouts[l].maker);
        }
    }
}

const InputLayout* input_layouts_find(const InputLayouts* layouts, const char* id)
{
    for (size_t l = 0; id != nullptr && l < layouts->layouts.size(); l++)
    {
        if (layouts->layouts[l].id == id)
        {
            return &layouts->layouts[l];
        }
    }
    return nullptr;
}

const InputLayout* input_layouts_for_type(const InputLayouts* layouts, const char* sdl_type)
{
    for (size_t l = 0; sdl_type != nullptr && l < layouts->layouts.size(); l++)
    {
        for (size_t t = 0; t < layouts->layouts[l].sdl_types.size(); t++)
        {
            if (layouts->layouts[l].sdl_types[t] == sdl_type)
            {
                return &layouts->layouts[l];
            }
        }
    }
    return nullptr;
}

bool input_layout_has(const InputLayout* layout, InputMapControl control)
{
    switch (control)
    {
    case InputMapControl::dpad_up:
    case InputMapControl::dpad_down:
    case InputMapControl::dpad_left:
    case InputMapControl::dpad_right:
        return layout->dpad;
    case InputMapControl::left_x:
    case InputMapControl::left_y:
        return layout->sticks >= 1;
    case InputMapControl::right_x:
    case InputMapControl::right_y:
        return layout->sticks >= 2;
    default:
        return !layout->labels[static_cast<uint32_t>(control)].empty();
    }
}

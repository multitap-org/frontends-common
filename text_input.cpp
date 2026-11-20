#include "text_input.h"

#include <SDL3/SDL.h>

void text_input_set_osk(TextInput* input, TextInputOsk osk)
{
    input->osk = osk;
}

void text_input_set_ui_field(TextInput* input, bool focused, const TextInputRect* area)
{
    if (focused && !input->ui_field)
    {
        input->ui_serial++;   // a new field (or the same one again): a new request
    }
    input->ui_field = focused;
    if (area != nullptr)
    {
        input->ui_area = *area;
    }
}

void text_input_pointer_down(TextInput* input, SDL_Window* window, float y)
{
    if (!input->ui_field || input->started_owner != TextInputOwner::ui_field || window == nullptr ||
        SDL_TextInputActive(window))
    {
        return;
    }
    // ImGui gives the caret's position and the line height, not the field's
    // width: the field's line, across the window.
    float top = static_cast<float>(input->ui_area.y);
    float bottom = top + static_cast<float>(input->ui_area.h);
    if (y >= top && y < bottom)
    {
        input->ui_serial++;
    }
}

void text_input_ui_field_number(TextInput* input)
{
    input->next_ui_type = TextInputType::number;
}

void text_input_set_game_chars(TextInput* input, bool wanted)
{
    input->game_chars = wanted;
}

void text_input_set_core_keys(TextInput* input, bool wanted, TextInputType type, uint32_t serial)
{
    input->core_keys = wanted;
    input->core_type = type;
    input->core_serial = serial;
}

TextInputOwner text_input_current(const TextInput* input)
{
    return input->started_owner;
}

static const char* osk_hint(TextInputOsk osk)
{
    switch (osk)
    {
    case TextInputOsk::on:
        return "1";
    case TextInputOsk::off:
        return "0";
    case TextInputOsk::system:
        break;
    }
    return "auto";
}

static bool same_rect(const TextInputRect* a, const TextInputRect* b)
{
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

static void set_area(SDL_Window* window, const TextInputRect* area)
{
    SDL_Rect rect = {area->x, area->y, area->w, area->h};
    SDL_SetTextInputArea(window, &rect, 0);
}

static void stop(TextInput* input, SDL_Window* window)
{
    // SDL hides a shown OSK on stop only if the hint allows OSKs then.
    if (SDL_ScreenKeyboardShown(window))
    {
        SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");
    }
    SDL_StopTextInput(window);
    input->started_owner = TextInputOwner::none;
}

static void start(TextInput* input, SDL_Window* window, TextInputOwner owner, uint32_t serial, TextInputType type,
                  TextInputOsk osk)
{
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
                          type == TextInputType::number ? SDL_TEXTINPUT_TYPE_NUMBER : SDL_TEXTINPUT_TYPE_TEXT);
    if (owner != TextInputOwner::ui_field)
    {
        // Keys go straight to a game, not into a sentence.
        SDL_SetNumberProperty(properties, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, SDL_CAPITALIZE_NONE);
        SDL_SetBooleanProperty(properties, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
    }
    else
    {
        // The OSK places itself from the area (Android scrolls / pans so it isn't covered).
        set_area(window, &input->ui_area);
        input->started_area = input->ui_area;
    }
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, osk_hint(osk));
    SDL_StartTextInputWithProperties(window, properties);
    SDL_DestroyProperties(properties);
    input->started_owner = owner;
    input->started_serial = serial;
    input->started_type = type;
    input->started_osk = osk;
}

void text_input_apply(TextInput* input, SDL_Window* window)
{
    input->ui_type = input->next_ui_type;
    input->next_ui_type = TextInputType::text;
    if (window == nullptr)
    {
        return;
    }

    TextInputOwner owner = TextInputOwner::none;
    uint32_t serial = 0;
    TextInputType type = TextInputType::text;
    TextInputOsk osk = input->osk;
    if (input->ui_field)
    {
        owner = TextInputOwner::ui_field;
        serial = input->ui_serial;
        type = input->ui_type;
    }
    else if (input->core_keys)
    {
        owner = TextInputOwner::core_keys;
        serial = input->core_serial;
        type = input->core_type;
    }
    else if (input->game_chars)
    {
        owner = TextInputOwner::game_chars;
        osk = TextInputOsk::off;
    }

    if (owner == TextInputOwner::none)
    {
        if (input->started_owner != TextInputOwner::none || SDL_TextInputActive(window))
        {
            stop(input, window);
        }
        return;
    }

    bool changed = owner != input->started_owner || serial != input->started_serial || type != input->started_type ||
                   osk != input->started_osk;
    if (changed)
    {
        // Stop first: going from an OSK to characters only must hide the OSK,
        // and a new request must show it again even if the last one was hidden.
        if (input->started_owner != TextInputOwner::none)
        {
            stop(input, window);
        }
        start(input, window, owner, serial, type, osk);
        return;
    }
    if (!SDL_TextInputActive(window))
    {
        // Something else stopped it (the window lost focus on some platforms).
        // Characters only: start again quietly. With an OSK the user may have
        // dismissed it, so it waits for the next request.
        if (owner == TextInputOwner::game_chars)
        {
            start(input, window, owner, serial, type, osk);
        }
        return;
    }
    if (owner == TextInputOwner::ui_field && !same_rect(&input->ui_area, &input->started_area))
    {
        set_area(window, &input->ui_area);
        input->started_area = input->ui_area;
    }
}

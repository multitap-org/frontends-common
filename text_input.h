#pragma once

// SDL text input for one window, with one owner: the frontend's UI text
// fields (Dear ImGui), a core that asked for the keyboard, and the game's
// characters all want it, sometimes with an on-screen keyboard (OSK) and
// sometimes without. Each states what it wants; text_input_apply() turns
// that into SDL_StartTextInput / SDL_StopTextInput once per frame.
//
// Who wins, best first:
//   1. A UI text field has focus (the OSK if enabled, at the field).
//   2. A core asked for the keyboard (KEYS): the OSK if enabled, its keypad type.
//   3. The game has the keyboard: characters only, never an OSK (a desktop
//      wants typed text in the user's layout; nothing shows).
//   4. Nothing: text input off.
//
// SDL shows an OSK from SDL_StartTextInput only where its video driver has one
// (Android, iOS, the Steam keyboard on X11, ...; on Wayland the compositor
// decides from text-input-v3), and only if SDL_HINT_ENABLE_SCREEN_KEYBOARD
// allows it, read at each start and stop. So the hint is set right before
// each call. A start happens once per request: an OSK the user hid stays
// hidden until the next field gets focus or the core asks again.
//
// For a later piece: a request could be answered by a paired phone instead
// (piccoletto's keyboard). text_input_current() is what it would forward.

#include <cstdint>

struct SDL_Window;

// INPUT > ENABLE OSK as the frontend resolved it.
enum class TextInputOsk : uint8_t
{
    system,   // leave it to SDL ("auto": an OSK only without a physical keyboard)
    on,       // show the system's OSK where there is one
    off,      // never show one
};

enum class TextInputType : uint8_t
{
    text,
    number,
};

// What text input is for right now (text_input_current()).
enum class TextInputOwner : uint8_t
{
    none,
    ui_field,     // a UI text field has focus
    core_keys,    // a core asked for the keyboard
    game_chars,   // the game has the keyboard (characters, no OSK)
};

struct TextInputRect
{
    int32_t x = 0;   // window pixels
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
};

struct TextInput
{
    TextInputOsk osk = TextInputOsk::system;

    // Wanted (set by the frontend and the core session; kept until changed).
    bool          ui_field = false;               // a UI text field has focus
    TextInputRect ui_area = {};                   // where it is (the OSK avoids covering it)
    TextInputType ui_type = TextInputType::text;  // reset to text by each apply
    TextInputType next_ui_type = TextInputType::text;
    uint32_t      ui_serial = 0;                  // counts field focus changes
    bool          game_chars = false;
    bool          core_keys = false;
    TextInputType core_type = TextInputType::text;
    uint32_t      core_serial = 0;                // the core's request

    // What SDL was given.
    TextInputOwner started_owner = TextInputOwner::none;
    uint32_t       started_serial = 0;
    TextInputType  started_type = TextInputType::text;
    TextInputOsk   started_osk = TextInputOsk::system;
    TextInputRect  started_area = {};
};

// INPUT > ENABLE OSK. Takes effect at the next start.
void text_input_set_osk(TextInput* input, TextInputOsk osk);

// A UI text field got or lost focus, and where it is (the IME hook in
// debug_ui calls this; it only calls when something changed).
void text_input_set_ui_field(TextInput* input, bool focused, const TextInputRect* area);

// A click or touch at height y, window pixels (app_shell, for every press).
// On the focused field's line while its OSK was dismissed: shows it again
// (tapping a field that already has focus is how a phone user asks for it).
void text_input_pointer_down(TextInput* input, SDL_Window* window, float y);

// The focused UI field takes numbers: call each frame while drawing it.
void text_input_ui_field_number(TextInput* input);

// The game has the keyboard (characters to the core).
void text_input_set_game_chars(TextInput* input, bool wanted);

// A core asked for the keyboard (or stopped asking). `serial` tells one
// request from the next: a new one shows the OSK again.
void text_input_set_core_keys(TextInput* input, bool wanted, TextInputType type, uint32_t serial);

// Once per frame: starts, changes or stops SDL text input on `window`.
void text_input_apply(TextInput* input, SDL_Window* window);

// What text input is for now (after the last apply).
TextInputOwner text_input_current(const TextInput* input);

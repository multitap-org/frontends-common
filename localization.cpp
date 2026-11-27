#include "localization.h"

#include <cstring>
#include <string_view>

#include <SDL3/SDL.h>

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

bool localization_init(Localization* localization, const LocalizedString* table, uint32_t count)
{
    *localization = Localization{};
    localization->table = table;
    localization->count = count;
    localization->translations.resize(count);

    bool ok = true;
    for (uint32_t i = 0; i < count; i++)
    {
        if (table[i].key == nullptr || table[i].key[0] == '\0' || table[i].english == nullptr)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Strings table: entry %u has no key or no English text", i);
            ok = false;
            continue;
        }
        for (uint32_t j = 0; j < i; j++)
        {
            if (table[j].key != nullptr && std::strcmp(table[j].key, table[i].key) == 0)
            {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Strings table: key %s appears twice", table[i].key);
                ok = false;
            }
        }
    }
    return ok;
}

void localization_use_english(Localization* localization)
{
    localization->translations.assign(localization->count, std::string());
    localization->language.clear();
    localization->refused = 0;
}

uint32_t localization_find(const Localization* localization, const char* key)
{
    for (uint32_t i = 0; i < localization->count; i++)
    {
        if (std::strcmp(localization->table[i].key, key) == 0)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

const char* localization_get(const Localization* localization, uint32_t id)
{
    if (id >= localization->count)
    {
        return "?";
    }
    const std::string* translated = &localization->translations[id];
    return translated->empty() ? localization->table[id].english : translated->c_str();
}

// ---------------------------------------------------------------------------
// .po reading
// ---------------------------------------------------------------------------

// Which string the "..." continuation lines add to.
enum class PoField
{
    none,
    context,
    id,
    translation,
    ignored,   // msgid_plural, msgstr[1..]
};

struct PoEntry
{
    std::string context;
    std::string id;
    std::string translation;
    bool        has_context = false;
    bool        has_id = false;
    bool        has_translation = false;
    bool        fuzzy = false;
};

// Appends the quoted string at the start of `text` ("..." with escapes) to
// `out`. False if it isn't a well-formed quoted string.
static bool append_quoted(std::string_view text, std::string* out)
{
    if (text.empty() || text.front() != '"')
    {
        return false;
    }
    for (size_t i = 1; i < text.size(); i++)
    {
        char c = text[i];
        if (c == '"')
        {
            return true;
        }
        if (c != '\\')
        {
            out->push_back(c);
            continue;
        }
        i++;
        if (i >= text.size())
        {
            return false;
        }
        switch (text[i])
        {
        case 'n':
            out->push_back('\n');
            break;
        case 't':
            out->push_back('\t');
            break;
        case 'r':
            out->push_back('\r');
            break;
        default:
            out->push_back(text[i]);   // \" \\ and anything else: the character itself
            break;
        }
    }
    return false;   // no closing quote
}

static std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

static bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

// Reads the printf conversions of `text` as comparable data: for each one the
// optional '*' width / precision (each consumes an argument), the length
// modifier and the conversion letter, as "<stars><length><letter>," appended to
// `signature`. Flags, width digits and precision digits are left out: a
// language may change them ("%5.1f" vs "%.2f") without changing which
// arguments are read. "%%" is skipped. False when the text has something no
// translation may have: "%n" (writes through a pointer) or a positional "%1$s"
// form (we never use them, so any '$' is treated as a mismatch).
//
// The space flag is deliberately not a flag here: "0.5% nobody" and "20 % walks"
// are literal percent signs in help texts (strings.txt), and read as "% n" that
// would be a "%n". No text of ours uses "% d". A '%' that is followed by
// something that is no conversion (")", " ", the end) is a literal percent and
// is skipped, the same way in English and in the translation.
static bool printf_signature(const char* text, std::string* signature)
{
    size_t i = 0;
    while (text[i] != '\0')
    {
        if (text[i] != '%')
        {
            i++;
            continue;
        }
        i++;   // past the '%'
        if (text[i] == '%')
        {
            i++;
            continue;
        }

        size_t p = i;
        std::string conversion;
        while (text[p] == '-' || text[p] == '+' || text[p] == '#' || text[p] == '0')
        {
            p++;
        }
        if (text[p] == '*')
        {
            conversion += '*';
            p++;
        }
        else
        {
            while (text[p] >= '0' && text[p] <= '9')
            {
                p++;
            }
        }
        if (text[p] == '$')
        {
            return false;
        }
        if (text[p] == '.')
        {
            p++;
            if (text[p] == '*')
            {
                conversion += '*';
                p++;
            }
            else
            {
                while (text[p] >= '0' && text[p] <= '9')
                {
                    p++;
                }
            }
            if (text[p] == '$')
            {
                return false;
            }
        }
        // Length modifiers: h hh l ll j z t L q (two characters at most).
        for (uint32_t k = 0; k < 2 && text[p] != '\0' && std::strchr("hljztLq", text[p]) != nullptr; k++)
        {
            conversion += text[p];
            p++;
        }

        char letter = text[p];
        if (letter == 'n')
        {
            return false;
        }
        if (letter == '%')
        {
            i = p + 1;   // "%5%" prints a percent sign and reads nothing, as printf does
        }
        else if (letter != '\0' && std::strchr("diuoxXcsSCpfFeEgGaA", letter) != nullptr)
        {
            conversion += letter;
            *signature += conversion;
            *signature += ',';
            i = p + 1;
        }
        // Anything else: not a conversion, a literal '%'. Continue right after it.
    }
    return true;
}

// True when a translation reads the same arguments as the English text, so
// the English text's snprintf() arguments are safe for it. A text that
// printf_signature() rejects never matches (even if English is the one with it).
static bool same_printf_conversions(const char* english, const char* translation)
{
    std::string english_signature;
    std::string translation_signature;
    if (!printf_signature(english, &english_signature) || !printf_signature(translation, &translation_signature))
    {
        return false;
    }
    return english_signature == translation_signature;
}

// Uses a finished entry and starts a new one. Many texts go straight into
// snprintf() as the format, and a user .po can replace the bundled one: a
// translation whose conversions differ from the English text's would be
// undefined behaviour when shown, so it is refused (English stays) and counted.
static void finish_entry(Localization* localization, PoEntry* entry, const char* source_name, uint32_t* translated,
                         uint32_t* unknown, uint32_t* refused)
{
    bool usable = entry->has_id && entry->has_context && !entry->fuzzy && !entry->id.empty() &&
                  !entry->translation.empty();
    if (usable)
    {
        uint32_t id = localization_find(localization, entry->context.c_str());
        if (id == UINT32_MAX)
        {
            (*unknown)++;
        }
        else if (!same_printf_conversions(localization->table[id].english, entry->translation.c_str()))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s: translation of %s refused: its printf conversions differ from the English text",
                        source_name, entry->context.c_str());
            (*refused)++;
        }
        else
        {
            localization->translations[id] = entry->translation;
            (*translated)++;
        }
    }
    *entry = PoEntry{};
}

uint32_t localization_parse_po(Localization* localization, const char* text, size_t size, const char* source_name,
                               const char* language)
{
    localization_use_english(localization);
    localization->language = language;

    std::string_view rest(text, size);
    PoEntry entry = {};
    PoField field = PoField::none;
    uint32_t translated = 0;
    uint32_t unknown = 0;
    uint32_t refused = 0;
    uint32_t line_number = 0;

    while (!rest.empty())
    {
        line_number++;
        size_t line_end = rest.find('\n');
        std::string_view line = trim(rest.substr(0, line_end));
        rest = line_end == std::string_view::npos ? std::string_view() : rest.substr(line_end + 1);

        if (line.empty())
        {
            finish_entry(localization, &entry, source_name, &translated, &unknown, &refused);
            field = PoField::none;
            continue;
        }
        if (line.front() == '#')
        {
            // A comment after a translation starts the next entry.
            if (entry.has_translation)
            {
                finish_entry(localization, &entry, source_name, &translated, &unknown, &refused);
                field = PoField::none;
            }
            if (starts_with(line, "#,") && line.find("fuzzy") != std::string_view::npos)
            {
                entry.fuzzy = true;
            }
            continue;
        }

        std::string_view value;
        if (starts_with(line, "msgctxt "))
        {
            if (entry.has_id)
            {
                finish_entry(localization, &entry, source_name, &translated, &unknown, &refused);
            }
            field = PoField::context;
            entry.has_context = true;
            value = trim(line.substr(8));
        }
        else if (starts_with(line, "msgid_plural "))
        {
            field = PoField::ignored;
            value = trim(line.substr(13));
        }
        else if (starts_with(line, "msgid "))
        {
            if (entry.has_translation)
            {
                finish_entry(localization, &entry, source_name, &translated, &unknown, &refused);
            }
            field = PoField::id;
            entry.has_id = true;
            value = trim(line.substr(6));
        }
        else if (starts_with(line, "msgstr[0] "))
        {
            field = PoField::translation;
            entry.has_translation = true;
            value = trim(line.substr(10));
        }
        else if (starts_with(line, "msgstr["))
        {
            field = PoField::ignored;
            size_t close = line.find(']');
            value = close == std::string_view::npos ? std::string_view() : trim(line.substr(close + 1));
        }
        else if (starts_with(line, "msgstr "))
        {
            field = PoField::translation;
            entry.has_translation = true;
            value = trim(line.substr(7));
        }
        else if (line.front() == '"')
        {
            value = line;   // continues the current field
        }
        else
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s:%u: line not understood, ignored", source_name, line_number);
            continue;
        }

        std::string* target = field == PoField::context       ? &entry.context
                              : field == PoField::id          ? &entry.id
                              : field == PoField::translation ? &entry.translation
                                                              : nullptr;
        std::string ignored;
        if (!append_quoted(value, target != nullptr ? target : &ignored))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s:%u: bad quoted string", source_name, line_number);
        }
    }
    finish_entry(localization, &entry, source_name, &translated, &unknown, &refused);

    localization->refused = refused;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s: %u of %u strings translated%s%s", source_name, translated,
                localization->count, unknown > 0 ? " (some keys are unknown: another version?)" : "",
                refused > 0 ? " (some were refused: their printf conversions differ from English)" : "");
    if (refused > 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s: %u translation(s) refused for their printf conversions", source_name,
                    refused);
    }
    return translated;
}

bool localization_load_po(Localization* localization, const char* path, const char* language)
{
    // SDL's file functions rather than host_fs: on Android the translations
    // that come with the program are inside the APK (assets), which only SDL
    // can read (a relative path there means the APK's assets).
    size_t size = 0;
    void* data = SDL_LoadFile(path, &size);
    if (data == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No translation file %s: English", path);
        localization_use_english(localization);
        return false;
    }
    localization_parse_po(localization, static_cast<const char*>(data), size, path, language);
    SDL_free(data);
    return true;
}

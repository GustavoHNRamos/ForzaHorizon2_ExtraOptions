#include "fork_menu.h"
#include "features.h"
#include "fork_text.h"
#include "key_bindings.h"
#include "logging.h"
#include "support.h"

/* The fork's rows in the game's HUD options menu, below the original mod's drift and FOV rows.
   The rows are spinners bound to properties registered on the HUD options page type with the
   same native calls video_options.c uses. A choice is written back to the INI file it came
   from, so the menu and the file always agree: feature switches need a restart (their hooks
   are installed at startup), keys and the music volume apply at once. */

enum {
    NATIVE_DELETE_OBJECT = 0x257730,
    NATIVE_COLLECTION_TYPE = 0x1476110,
    NATIVE_ALLOCATE_TYPED = 0x14424e0,
    NATIVE_COLLECTION_INIT = 0xaefd60,
    NATIVE_MAKE_STRING = 0x13fe8e0,
    NATIVE_INSERT_ITEM = 0xb52170,
    NATIVE_INT_PROPERTY = 0xed81f0,
    NATIVE_COLLECTION_PROPERTY = 0xed77d0,
    NATIVE_REGISTER_PROPERTY = 0x155ee30,
    HUD_TYPE_REGISTRY = 0x35235e0,
    NATIVE_OBJECT_INIT = 0x143eb80,
    NATIVE_BOX_INT = 0x4702d0,
    NATIVE_PROPERTY_CHANGED = 0xcf9c60,
};

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    void *getter, *setter, *unused;
    const char *name;
    void *type;
    uint64_t flags;
} NativeProperty;

typedef struct NativePropertyLink {
    NativeProperty *property;
    struct NativePropertyLink *next;
} NativePropertyLink;

_Static_assert(sizeof(NativeProperty) == 0x58, "Native property descriptor");

typedef void **(*BoxValue)(void **, const void *);
typedef void **(*MakeString)(void **, const wchar_t *);

enum {
    ROW_HUD,
    ROW_WORLD,
    ROW_MUSIC,
    ROW_VOLUME,
    ROW_FIRST_KEY,
    ROW_COUNT = ROW_FIRST_KEY + KEY_ACTION_COUNT,
    LIST_VOLUME = ROW_COUNT,
    LIST_KEYS,
    PROPERTY_COUNT
};

static const char *const property_names[PROPERTY_COUNT] = {
    "FxHudKey",       "FxWorldKeys",    "FxLocalMusic",       "FxMusicVolume",
    "FxKeyToggleHud", "FxKeyNextHour",  "FxKeyWeather",       "FxKeyClock",
    "FxKeyClockSpeed", "FxKeyRain",     "FxKeyMusicToggle",   "FxKeyMusicNext",
    "FxVolumeChoices", "FxKeyChoices"};

/* INI names of each row: feature switches, then [Keys]. */
static const char *const ini_sections[ROW_FIRST_KEY - 1] = {"HUD", "World", "Music"};
static const char *const ini_keys[ROW_COUNT] = {
    "ToggleHUD", "TimeAndWeatherKeys", "LocalMusic", "Volume",     "ToggleHUD",  "NextHour",
    "Weather",   "Clock",              "ClockSpeed", "Rain",       "MusicToggle", "MusicNext"};

#define VOLUME_STEP 50
#define VOLUME_CHOICES 16 /* 50% to 800% */

static const int key_choices[] = {0,
                                  VK_F1,
                                  VK_F2,
                                  VK_F3,
                                  VK_F4,
                                  VK_F5,
                                  VK_F6,
                                  VK_F7,
                                  VK_F8,
                                  VK_F9,
                                  VK_F10,
                                  VK_F11,
                                  VK_F12,
                                  VK_INSERT,
                                  VK_DELETE,
                                  VK_HOME,
                                  VK_END,
                                  VK_PRIOR,
                                  VK_NEXT,
                                  KEY_CHARACTER | '[',
                                  KEY_CHARACTER | ']',
                                  VK_NUMPAD0,
                                  VK_NUMPAD1,
                                  VK_NUMPAD2,
                                  VK_NUMPAD3,
                                  VK_NUMPAD4,
                                  VK_NUMPAD5,
                                  VK_NUMPAD6,
                                  VK_NUMPAD7,
                                  VK_NUMPAD8,
                                  VK_NUMPAD9};
/* The last entry stands for a key set in the INI that is not in this list. */
#define KEY_OTHER ARRAY_COUNT(key_choices)

static unsigned char *image;
static void *registered_image;
static NativeProperty properties[PROPERTY_COUNT];
static NativePropertyLink links[PROPERTY_COUNT];
static void *choices[2];
static int switches[3];
static wchar_t settings_ini[PATH_CAPACITY], temporary[PATH_CAPACITY];

void fork_menu_prepare(const wchar_t *settings_directory, const ModOptions *options) {
    if (!path_join(settings_ini, settings_directory, MOD_INI_NAME)) {
        settings_ini[0] = 0;
    }
    switches[ROW_HUD] = options->hud_toggle;
    switches[ROW_WORLD] = options->world_controls;
    switches[ROW_MUSIC] = options->local_music;
}

/* ---- INI writing: replaces one value, keeping every other line and comment. ---- */

#define INI_CAPACITY 65536
static char ini_text[INI_CAPACITY], ini_output[INI_CAPACITY + 512];

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int span_equal(const char *text, size_t length, const char *name) {
    size_t i = 0;
    for (; i < length && name[i]; ++i) {
        char a = text[i], b = name[i];
        a = (char)(a >= 'A' && a <= 'Z' ? a + 32 : a);
        b = (char)(b >= 'A' && b <= 'Z' ? b + 32 : b);
        if (a != b) {
            return 0;
        }
    }
    return i == length && !name[i];
}

static size_t append(char *out, size_t at, const char *text, size_t length) {
    if (at + length > sizeof(ini_output)) {
        return (size_t)-1;
    }
    memory_copy(out + at, text, length);
    return at + length;
}

static int ini_write(const wchar_t *path, const char *section, const char *key, const char *value) {
    if (!path[0]) {
        return 0;
    }
    size_t length = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        DWORD read = 0;
        int valid = GetFileSizeEx(file, &size) && size.QuadPart < INI_CAPACITY &&
                    ReadFile(file, ini_text, (DWORD)size.QuadPart, &read, NULL) &&
                    read == size.QuadPart;
        CloseHandle(file);
        if (!valid) {
            return 0;
        }
        length = read;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return 0;
    }
    /* Find the section, then the key's line inside it. */
    size_t section_body = (size_t)-1, value_start = 0, value_end = 0;
    int in_section = 0, found = 0;
    for (size_t line = 0; line < length && !found;) {
        size_t end = line;
        while (end < length && ini_text[end] != '\n') {
            ++end;
        }
        size_t next = end < length ? end + 1 : end;
        size_t start = line;
        if (start == 0 && length >= 3 && memory_equal(ini_text, "\xef\xbb\xbf", 3)) {
            start = 3;
        }
        while (start < end && is_blank(ini_text[start])) {
            ++start;
        }
        if (start < end && ini_text[start] == '[') {
            size_t close = start + 1;
            while (close < end && ini_text[close] != ']') {
                ++close;
            }
            in_section = close < end && span_equal(ini_text + start + 1, close - start - 1, section);
            if (in_section) {
                section_body = next;
            }
        } else if (in_section) {
            size_t equals = start;
            while (equals < end && ini_text[equals] != '=') {
                ++equals;
            }
            size_t name_end = equals;
            while (name_end > start && is_blank(ini_text[name_end - 1])) {
                --name_end;
            }
            if (equals < end && span_equal(ini_text + start, name_end - start, key)) {
                value_start = equals + 1;
                value_end = value_start;
                while (value_end < end && ini_text[value_end] != ';' &&
                       ini_text[value_end] != '#' && ini_text[value_end] != '\r') {
                    ++value_end;
                }
                found = 1;
            }
        }
        line = next;
    }
    size_t out = 0, value_length = text_length(value);
    /* New lines follow the file's own line ends. */
    const char *newline = text_find(ini_text, length, "\r\n") || !length ? "\r\n" : "\n";
    size_t newline_length = text_length(newline);
    if (found) {
        int comment = value_end < length && ini_text[value_end] != '\r' &&
                      ini_text[value_end] != '\n';
        out = append(ini_output, out, ini_text, value_start);
        out = out == (size_t)-1 ? out : append(ini_output, out, value, value_length);
        out = out == (size_t)-1 || !comment ? out : append(ini_output, out, " ", 1);
        out = out == (size_t)-1
                  ? out
                  : append(ini_output, out, ini_text + value_end, length - value_end);
    } else if (section_body != (size_t)-1) {
        out = append(ini_output, out, ini_text, section_body);
        if (section_body && ini_text[section_body - 1] != '\n') {
            out = out == (size_t)-1 ? out : append(ini_output, out, newline, newline_length);
        }
        out = out == (size_t)-1 ? out : append(ini_output, out, key, text_length(key));
        out = out == (size_t)-1 ? out : append(ini_output, out, "=", 1);
        out = out == (size_t)-1 ? out : append(ini_output, out, value, value_length);
        out = out == (size_t)-1 ? out : append(ini_output, out, newline, newline_length);
        out = out == (size_t)-1
                  ? out
                  : append(ini_output, out, ini_text + section_body, length - section_body);
    } else {
        out = append(ini_output, out, ini_text, length);
        if (length && ini_text[length - 1] != '\n') {
            out = out == (size_t)-1 ? out : append(ini_output, out, newline, newline_length);
        }
        if (length) {
            out = out == (size_t)-1 ? out : append(ini_output, out, newline, newline_length);
        }
        out = out == (size_t)-1 ? out : append(ini_output, out, "[", 1);
        out = out == (size_t)-1 ? out : append(ini_output, out, section, text_length(section));
        out = out == (size_t)-1 ? out : append(ini_output, out, "]", 1);
        out = out == (size_t)-1 ? out : append(ini_output, out, newline, newline_length);
        out = out == (size_t)-1 ? out : append(ini_output, out, key, text_length(key));
        out = out == (size_t)-1 ? out : append(ini_output, out, "=", 1);
        out = out == (size_t)-1 ? out : append(ini_output, out, value, value_length);
        out = out == (size_t)-1 ? out : append(ini_output, out, newline, newline_length);
    }
    if (out == (size_t)-1) {
        return 0;
    }
    /* Write a temporary file, then replace, so a failed write never leaves half a file. */
    size_t path_length = 0;
    while (path[path_length] && path_length + 5 < PATH_CAPACITY) {
        temporary[path_length] = path[path_length];
        ++path_length;
    }
    if (path[path_length]) {
        return 0;
    }
    memory_copy(temporary + path_length, L".tmp", 5 * sizeof(wchar_t));
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD written = 0;
    int saved = WriteFile(file, ini_output, (DWORD)out, &written, NULL) && written == out;
    CloseHandle(file);
    if (!saved ||
        !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return 0;
    }
    return 1;
}

/* ---- Menu values ---- */

static int row_value(unsigned row) {
    if (row < ROW_VOLUME) {
        return switches[row];
    }
    if (row == ROW_VOLUME) {
        int index = ((int)local_music_volume_percent() + VOLUME_STEP / 2) / VOLUME_STEP - 1;
        return index < 0 ? 0 : index >= VOLUME_CHOICES ? VOLUME_CHOICES - 1 : index;
    }
    int key = key_bindings_get((KeyAction)(row - ROW_FIRST_KEY));
    for (unsigned index = 0; index < ARRAY_COUNT(key_choices); ++index) {
        if (key_choices[index] == key) {
            return (int)index;
        }
    }
    return KEY_OTHER;
}

static int set_row(unsigned row, int value) {
    char text[16];
    if (row < ROW_VOLUME) {
        if (value < 0 || value > 1 || value == switches[row]) {
            return 0;
        }
        text[0] = (char)('0' + value);
        text[1] = 0;
        if (!ini_write(settings_ini, ini_sections[row], ini_keys[row], text)) {
            log_append("MENU: could not write FH2_ExtraOptions.ini; choice not kept.\r\n");
            return 0;
        }
        switches[row] = value;
        log_append("MENU: feature switch saved; restart the game to apply it.\r\n");
        return 1;
    }
    if (row == ROW_VOLUME) {
        if (value < 0 || value >= VOLUME_CHOICES || value == row_value(row)) {
            return 0;
        }
        unsigned percent = (unsigned)(value + 1) * VOLUME_STEP;
        local_music_set_volume_percent(percent);
        text[0] = (char)('0' + percent / 100);
        text[1] = (char)('0' + percent / 10 % 10);
        text[2] = '0';
        text[3] = 0;
        if (!ini_write(local_music_ini_path(), "Music", "Volume", text)) {
            log_append("MENU: could not write FH2_Music.ini; the volume lasts until the game "
                       "closes.\r\n");
        }
        return 1;
    }
    if (value < 0 || value >= (int)KEY_OTHER || value == row_value(row)) {
        return 0;
    }
    KeyAction action = (KeyAction)(row - ROW_FIRST_KEY);
    key_bindings_set(action, key_choices[value]);
    key_binding_describe(key_choices[value], text, sizeof(text));
    if (!ini_write(settings_ini, "Keys", ini_keys[row], text)) {
        log_append("MENU: could not write FH2_ExtraOptions.ini; the key lasts until the game "
                   "closes.\r\n");
    }
    return 1;
}

static unsigned property_index(const NativeProperty *property) {
    uintptr_t address = (uintptr_t)property, first = (uintptr_t)properties;
    size_t offset = address - first;
    if (address < first || offset >= sizeof(properties) || offset % sizeof(*properties)) {
        return PROPERTY_COUNT;
    }
    return (unsigned)(offset / sizeof(*properties));
}

static void **get_value(void **output, NativeProperty *property, void *page) {
    (void)page;
    unsigned index = property_index(property);
    *output = NULL;
    if (index >= ROW_COUNT && index < PROPERTY_COUNT) {
        *output = choices[index - ROW_COUNT];
        if (*output) {
            InterlockedIncrement((volatile LONG *)((unsigned char *)*output + 0x10));
        }
    } else if (index < ROW_COUNT) {
        int value = row_value(index);
        ((BoxValue)(image + NATIVE_BOX_INT))(output, &value);
    }
    return output;
}

static void set_value(NativeProperty *property, void *page, void *boxed) {
    unsigned index = property_index(property);
    int value;
    if (boxed && index < ROW_COUNT &&
        memory_read((unsigned char *)boxed + 0x28, &value, sizeof(value)) &&
        set_row(index, value)) {
        ((void (*)(void *, void *))(image + NATIVE_PROPERTY_CHANGED))(page, property);
    }
}

static void release_object(void *object) {
    if (object && InterlockedDecrement((volatile LONG *)((unsigned char *)object + 0x10)) == 0) {
        ((void (*)(void *))(image + NATIVE_DELETE_OBJECT))(object);
    }
}

static void choice_label(unsigned list, unsigned index, wchar_t *label, size_t capacity) {
    const ForkMenuText *text = &fork_text(image)->menu;
    const wchar_t *fixed = list == 1 && index == 0          ? text->none
                           : list == 1 && index == KEY_OTHER ? text->other
                                                              : NULL;
    char ascii[16];
    if (fixed) {
        size_t i = 0;
        for (; fixed[i] && i + 1 < capacity; ++i) {
            label[i] = fixed[i];
        }
        label[i] = 0;
        return;
    }
    if (list == 0) {
        unsigned percent = (index + 1) * VOLUME_STEP, at = 0;
        if (percent >= 100) {
            ascii[at++] = (char)('0' + percent / 100);
        }
        ascii[at++] = (char)('0' + percent / 10 % 10);
        ascii[at++] = '0';
        ascii[at++] = '%';
        ascii[at] = 0;
    } else {
        key_binding_describe(key_choices[index], ascii, sizeof(ascii));
    }
    size_t i = 0;
    for (; ascii[i] && i + 1 < capacity; ++i) {
        label[i] = (wchar_t)ascii[i];
    }
    label[i] = 0;
}

/* Same collection layout as the original mod's spinner choices in video_options.c. */
static void *make_choices(unsigned list) {
    void *type = ((void *(*)(void *))(image + NATIVE_COLLECTION_TYPE))(NULL);
    void *items = ((void *(*)(size_t, void *))(image + NATIVE_ALLOCATE_TYPED))(0x58, type);
    if (!items) {
        return NULL;
    }
    ((void (*)(void *))(image + NATIVE_COLLECTION_INIT))(items);
    static const unsigned vtables[] = {0x2db73f8, 0x2db7470, 0x2db74d0, 0x2db74e8, 0x2db7508};
    static const unsigned offsets[] = {0, 0x30, 0x38, 0x40, 0x48};
    for (unsigned i = 0; i < ARRAY_COUNT(vtables); ++i) {
        *(void **)((unsigned char *)items + offsets[i]) = image + vtables[i];
    }
    unsigned count = list == 0 ? VOLUME_CHOICES : KEY_OTHER + 1;
    for (unsigned index = 0; index < count; ++index) {
        wchar_t label[48];
        void *string = NULL;
        choice_label(list, index, label, ARRAY_COUNT(label));
        ((MakeString)(image + NATIVE_MAKE_STRING))(&string, label);
        if (!string) {
            release_object(items);
            return NULL;
        }
        ((void (*)(void *, int, void *))(image + NATIVE_INSERT_ITEM))(items, (int)index, string);
        release_object(string);
    }
    return items;
}

int fork_menu_register(unsigned char *game_image) {
    if (registered_image == game_image) {
        return 1;
    }
    image = game_image;
    for (unsigned list = 0; list < ARRAY_COUNT(choices); ++list) {
        choices[list] = make_choices(list);
        if (!choices[list]) {
            return 0;
        }
    }
    for (unsigned index = 0; index < PROPERTY_COUNT; ++index) {
        NativeProperty *property = &properties[index];
        NativeProperty *original = ((NativeProperty * (*)(void))(
            image + (index >= ROW_COUNT ? NATIVE_COLLECTION_PROPERTY : NATIVE_INT_PROPERTY)))();
        ((void (*)(void *))(image + NATIVE_OBJECT_INIT))(property);
        property->vtable = original->vtable;
        memory_copy((unsigned char *)property + 0x28, (unsigned char *)original + 0x28, 0x30);
        property->name = property_names[index];
        property->getter = (void *)get_value;
        property->setter = index >= ROW_COUNT ? NULL : (void *)set_value;
        links[index].property = property;
        ((void (*)(void *, NativePropertyLink *))(image + NATIVE_REGISTER_PROPERTY))(
            image + HUD_TYPE_REGISTRY, &links[index]);
    }
    registered_image = game_image;
    log_append("MENU: fork controls added to the native HUD menu.\r\n");
    return 1;
}

/* ---- Layout ---- */

typedef struct {
    char *bytes;
    size_t length, capacity;
    int valid;
} Buffer;

static char resources[16384], rows[16384];

static void put(Buffer *buffer, const char *text, size_t length) {
    if (!buffer->valid || length >= buffer->capacity - buffer->length) {
        buffer->valid = 0;
        return;
    }
    memory_copy(buffer->bytes + buffer->length, text, length);
    buffer->length += length;
}

static void put_text(Buffer *buffer, const char *text) {
    put(buffer, text, text_length(text));
}

/* UTF-8 attribute text; XML's special characters are ASCII, so escaping after converting is safe. */
static void put_wide(Buffer *buffer, const wchar_t *text) {
    char utf8[512];
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    for (int i = 0; i + 1 < length; ++i) {
        const char *escaped = utf8[i] == '&'   ? "&amp;"
                              : utf8[i] == '<' ? "&lt;"
                              : utf8[i] == '>' ? "&gt;"
                              : utf8[i] == '"' ? "&quot;"
                                               : NULL;
        if (escaped) {
            put_text(buffer, escaped);
        } else {
            put(buffer, utf8 + i, 1);
        }
    }
    if (length <= 0) {
        buffer->valid = 0;
    }
}

static void put_heading(Buffer *buffer, const ForkMenuText *text, unsigned row) {
    const wchar_t *const labels[ROW_FIRST_KEY] = {text->hud, text->world, text->music,
                                                  text->volume};
    if (row < ROW_FIRST_KEY) {
        put_wide(buffer, labels[row]);
        return;
    }
    put_wide(buffer, text->key);
    put_text(buffer, ": ");
    put_wide(buffer, text->actions[row - ROW_FIRST_KEY]);
}

static void put_description(Buffer *buffer, const ForkMenuText *text, unsigned row) {
    const wchar_t *const descriptions[ROW_FIRST_KEY] = {text->hud_info, text->world_info,
                                                        text->music_info, text->volume_info};
    put_wide(buffer, row < ROW_FIRST_KEY ? descriptions[row] : text->key_info);
}

static const char *row_choices(unsigned row) {
    /* ToggleChoices (Off, On) belongs to the original mod's HUD rows. */
    return row < ROW_VOLUME    ? "ToggleChoices"
           : row == ROW_VOLUME ? "FxVolumeChoices"
                               : "FxKeyChoices";
}

size_t fork_menu_extend_hud(unsigned char *game_image, char *bytes, size_t length,
                            size_t capacity) {
    if (text_find(bytes, length, "FX_FxHudKey")) {
        return length;
    }
    const char *dictionary = text_find(bytes, length, "</ResourceDictionary>");
    const char *list = text_find(bytes, length, "<local:CommonListBox x:Name=\"OptionsList\"");
    const char *end =
        list ? text_find(list, length - (size_t)(list - bytes), "</local:CommonListBox>") : NULL;
    if (!dictionary || !end || dictionary > end) {
        log_append("MENU: HUD menu layout did not match; fork controls not added.\r\n");
        return length;
    }
    const ForkMenuText *text = &fork_text(game_image)->menu;
    Buffer added = {resources, 0, sizeof(resources), 1}, listed = {rows, 0, sizeof(rows), 1};
    for (unsigned row = 0; row < ROW_COUNT; ++row) {
        put_text(&added, "<local:TileMenuDataItem x:Key=\"FX_");
        put_text(&added, property_names[row]);
        put_text(&added, "\" Heading=\"");
        put_heading(&added, text, row);
        put_text(&added, "\" Description=\"");
        put_description(&added, text, row);
        put_text(&added, "\"/>");

        put_text(&listed, "<local:SpinnerControl Tag=\"{StaticResource FX_");
        put_text(&listed, property_names[row]);
        put_text(&listed, "}\" Style=\"{StaticResource A_Style_SpinnerControl_Default}\" Label=\"");
        put_heading(&listed, text, row);
        put_text(&listed, "\" SelectedIndex=\"{Binding ");
        put_text(&listed, property_names[row]);
        put_text(&listed, "}\" ItemsSource=\"{Binding ");
        put_text(&listed, row_choices(row));
        put_text(&listed, "}\"/>");
    }
    if (!added.valid || !listed.valid || length + added.length + listed.length >= capacity) {
        log_append("MENU: HUD menu has no room; fork controls not added.\r\n");
        return length;
    }
    /* Insert the rows first (later in the text), then the resources; move tails backwards. */
    size_t at_rows = (size_t)(end - bytes), at_resources = (size_t)(dictionary - bytes);
    for (size_t i = length; i > at_rows; --i) {
        bytes[i - 1 + listed.length] = bytes[i - 1];
    }
    memory_copy(bytes + at_rows, rows, listed.length);
    length += listed.length;
    for (size_t i = length; i > at_resources; --i) {
        bytes[i - 1 + added.length] = bytes[i - 1];
    }
    memory_copy(bytes + at_resources, resources, added.length);
    length += added.length;
    bytes[length] = 0;
    return length;
}

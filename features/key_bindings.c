#include "key_bindings.h"
#include "logging.h"
#include "support.h"

/* Keyboard keys for the fork's actions, set in the [Keys] section. INI parsing happens during
   DLL initialization, so names are resolved without User32; layout-dependent characters and
   the key names written to the log are resolved on the first UI frame, where User32 is loaded
   lazily (like Autodrive's F2). Keys are read once per frame, only while the game window is in
   front, and each action sees a press once. */

typedef SHORT(WINAPI *KeyStateFn)(int);
typedef HWND(WINAPI *ForegroundFn)(void);
typedef DWORD(WINAPI *WindowProcessFn)(HWND, LPDWORD);
typedef SHORT(WINAPI *KeyScanFn)(WCHAR);
typedef UINT(WINAPI *MapKeyFn)(UINT, UINT);
typedef int(WINAPI *KeyNameFn)(LONG, LPWSTR, int);

static const struct {
    const char *name;
    int code;
} named_keys[] = {{"Home", VK_HOME},
                  {"End", VK_END},
                  {"Insert", VK_INSERT},
                  {"Delete", VK_DELETE},
                  {"PageUp", VK_PRIOR},
                  {"PageDown", VK_NEXT},
                  {"Up", VK_UP},
                  {"Down", VK_DOWN},
                  {"Left", VK_LEFT},
                  {"Right", VK_RIGHT},
                  {"Tab", VK_TAB},
                  {"Space", VK_SPACE},
                  {"Backspace", VK_BACK},
                  {"Enter", VK_RETURN},
                  {"Pause", VK_PAUSE},
                  {"ScrollLock", VK_SCROLL},
                  {"CapsLock", VK_CAPITAL},
                  {"NumpadAdd", VK_ADD},
                  {"NumpadSubtract", VK_SUBTRACT},
                  {"NumpadMultiply", VK_MULTIPLY},
                  {"NumpadDivide", VK_DIVIDE},
                  {"NumpadDecimal", VK_DECIMAL}};

static const char *const action_names[KEY_ACTION_TOTAL] = {
    "ToggleHUD", "NextHour", "Weather", "Clock", "ClockSpeed", "Rain", "MusicToggle", "MusicNext"};

static int keys[KEY_ACTION_TOTAL], used[KEY_ACTION_TOTAL];
static int was_down[KEY_ACTION_TOTAL], pressed[KEY_ACTION_TOTAL];
static int input_state; /* 0 untried, 1 ready, -1 unavailable */
static KeyStateFn key_state;
static ForegroundFn foreground_window;
static WindowProcessFn window_process;

void key_bindings_defaults(int defaults[KEY_ACTION_COUNT]) {
    static const int values[KEY_ACTION_TOTAL] = {VK_F3,
                                                 VK_F5,
                                                 VK_F6,
                                                 VK_F7,
                                                 KEY_CHARACTER | '[',
                                                 KEY_CHARACTER | ']',
                                                 VK_HOME,
                                                 VK_END};
    for (unsigned i = 0; i < KEY_ACTION_TOTAL; ++i) {
        defaults[i] = values[i];
    }
}

int key_binding_parse(const char *text, int *value) {
    size_t length = text_length(text);
    if ((text[0] == 'F' || text[0] == 'f') && length >= 2 && length <= 3) {
        int number = 0;
        for (size_t i = 1; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                number = 0;
                break;
            }
            number = number * 10 + (text[i] - '0');
        }
        if (number >= 1 && number <= 24) {
            *value = VK_F1 + number - 1;
            return 1;
        }
    }
    for (unsigned i = 0; i < ARRAY_COUNT(named_keys); ++i) {
        if (text_equal_ignore_case(text, named_keys[i].name)) {
            *value = named_keys[i].code;
            return 1;
        }
    }
    if (length == 7 && text_equal_ignore_case(text, "Numpad0")) {
        *value = VK_NUMPAD0;
        return 1;
    }
    if (length == 7 && (text[0] == 'N' || text[0] == 'n') && text[6] >= '1' && text[6] <= '9') {
        char prefix[7];
        memory_copy(prefix, text, 6);
        prefix[6] = 0;
        if (text_equal_ignore_case(prefix, "Numpad")) {
            *value = VK_NUMPAD0 + (text[6] - '0');
            return 1;
        }
    }
    if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        int code = 0;
        for (size_t i = 2; i < length; ++i) {
            char c = text[i];
            int digit = c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : -1;
            if (digit < 0 || code > 0xff) {
                return 0;
            }
            code = code * 16 + digit;
        }
        if (code >= 1 && code <= 0xfe) {
            *value = code;
            return 1;
        }
        return 0;
    }
    if (length == 1 && text[0] > ' ' && text[0] < 0x7f) {
        char c = text[0];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        *value = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : KEY_CHARACTER | c;
        return 1;
    }
    return 0;
}

void key_binding_describe(int value, char *out, size_t capacity) {
    const char *name = 0;
    char buffer[8] = {0};
    if (value & KEY_CHARACTER) {
        buffer[0] = (char)(value & 0x7f);
        name = buffer;
    } else if (value >= VK_F1 && value <= VK_F24) {
        int number = value - VK_F1 + 1;
        buffer[0] = 'F';
        buffer[1] = (char)(number >= 10 ? '0' + number / 10 : '0' + number);
        buffer[2] = (char)(number >= 10 ? '0' + number % 10 : 0);
        name = buffer;
    } else if (value >= VK_NUMPAD0 && value <= VK_NUMPAD9) {
        static char numpad[] = "Numpad0";
        numpad[6] = (char)('0' + value - VK_NUMPAD0);
        name = numpad;
    } else if ((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')) {
        buffer[0] = (char)value;
        name = buffer;
    } else {
        for (unsigned i = 0; i < ARRAY_COUNT(named_keys); ++i) {
            if (named_keys[i].code == value) {
                name = named_keys[i].name;
            }
        }
    }
    if (!name) {
        static const char hex[] = "0123456789ABCDEF";
        buffer[0] = '0';
        buffer[1] = 'x';
        buffer[2] = hex[(value >> 4) & 0xf];
        buffer[3] = hex[value & 0xf];
        name = buffer;
    }
    size_t length = 0;
    while (name[length] && length + 1 < capacity) {
        out[length] = name[length];
        ++length;
    }
    out[length] = 0;
}

void key_bindings_prepare(const ModOptions *options) {
    for (unsigned i = 0; i < KEY_ACTION_TOTAL; ++i) {
        keys[i] = options->keys[i];
    }
    used[KEY_TOGGLE_HUD] = options->hud_toggle;
    for (unsigned i = KEY_NEXT_HOUR; i <= KEY_RAIN; ++i) {
        used[i] = options->world_controls;
    }
    used[KEY_MUSIC_TOGGLE] = used[KEY_MUSIC_NEXT] = options->local_music;
}

/* Logs each enabled action with the name Windows gives its key, and any key shared by two. */
static void log_keys(MapKeyFn map_key, KeyNameFn key_name) {
    for (unsigned i = 0; i < KEY_ACTION_TOTAL; ++i) {
        if (!used[i]) {
            continue;
        }
        char line[128] = "KEYS: ";
        size_t length = 6;
        for (const char *c = action_names[i]; *c && length < 40; ++c) {
            line[length++] = *c;
        }
        line[length++] = ' ';
        line[length++] = '=';
        line[length++] = ' ';
        wchar_t name[48] = {0};
        UINT scan = map_key ? map_key((UINT)keys[i], 0) : 0;
        int extended = keys[i] == VK_HOME || keys[i] == VK_END || keys[i] == VK_INSERT ||
                       keys[i] == VK_DELETE || keys[i] == VK_PRIOR || keys[i] == VK_NEXT ||
                       (keys[i] >= VK_LEFT && keys[i] <= VK_DOWN) || keys[i] == VK_DIVIDE;
        int count = scan && key_name
                        ? key_name((LONG)(scan << 16 | (extended ? 1u << 24 : 0)), name, 48)
                        : 0;
        if (count > 0) {
            for (int c = 0; c < count && length < sizeof(line) - 3; ++c) {
                line[length++] = name[c] >= 0x20 && name[c] < 0x7f ? (char)name[c] : '?';
            }
        } else {
            key_binding_describe(keys[i], line + length, sizeof(line) - length - 3);
            while (line[length]) {
                ++length;
            }
        }
        line[length++] = '\r';
        line[length++] = '\n';
        line[length] = 0;
        log_append(line);
        for (unsigned j = 0; j < i; ++j) {
            if (used[j] && keys[j] == keys[i]) {
                char warning[96] = "KEYS: warning, the same key is set for ";
                size_t at = text_length(warning);
                for (const char *c = action_names[j]; *c; ++c) {
                    warning[at++] = *c;
                }
                for (const char *c = " and "; *c; ++c) {
                    warning[at++] = *c;
                }
                for (const char *c = action_names[i]; *c; ++c) {
                    warning[at++] = *c;
                }
                warning[at++] = '\r';
                warning[at++] = '\n';
                warning[at] = 0;
                log_append(warning);
            }
        }
    }
}

static int initialize_input(void) {
    if (input_state) {
        return input_state > 0;
    }
    input_state = -1;
    HMODULE module = LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        log_append("KEYS: User32 unavailable; keyboard actions are inactive.\r\n");
        return 0;
    }
    key_state = (KeyStateFn)(uintptr_t)GetProcAddress(module, "GetAsyncKeyState");
    foreground_window = (ForegroundFn)(uintptr_t)GetProcAddress(module, "GetForegroundWindow");
    window_process =
        (WindowProcessFn)(uintptr_t)GetProcAddress(module, "GetWindowThreadProcessId");
    KeyScanFn key_scan = (KeyScanFn)(uintptr_t)GetProcAddress(module, "VkKeyScanW");
    MapKeyFn map_key = (MapKeyFn)(uintptr_t)GetProcAddress(module, "MapVirtualKeyW");
    KeyNameFn key_name = (KeyNameFn)(uintptr_t)GetProcAddress(module, "GetKeyNameTextW");
    if (!key_state || !foreground_window || !window_process) {
        log_append("KEYS: User32 unavailable; keyboard actions are inactive.\r\n");
        return 0;
    }
    for (unsigned i = 0; i < KEY_ACTION_TOTAL; ++i) {
        if (keys[i] & KEY_CHARACTER) {
            SHORT scanned = key_scan ? key_scan((WCHAR)(keys[i] & 0xffff)) : -1;
            keys[i] = scanned != -1 ? scanned & 0xff : 0;
        }
    }
    log_keys(map_key, key_name);
    input_state = 1;
    return 1;
}

void key_bindings_frame(void) {
    for (unsigned i = 0; i < KEY_ACTION_TOTAL; ++i) {
        pressed[i] = 0;
    }
    if (!initialize_input()) {
        return;
    }
    HWND window = foreground_window();
    DWORD process = 0;
    if (window) {
        window_process(window, &process);
    }
    int focused = process == GetCurrentProcessId();
    for (unsigned i = 0; i < KEY_ACTION_TOTAL; ++i) {
        int down = used[i] && keys[i] && focused && (key_state(keys[i]) & 0x8000) != 0;
        pressed[i] = down && !was_down[i];
        was_down[i] = down;
    }
}

int key_bindings_pressed(KeyAction action) {
    return pressed[action];
}

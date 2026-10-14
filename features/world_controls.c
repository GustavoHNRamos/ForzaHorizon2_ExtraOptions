#include "features.h"
#include "autodrive_notice.h"
#include "fork_text.h"
#include "key_bindings.h"
#include "logging.h"
#include "support.h"

/* Time and weather keys (defaults in brackets, set in [Keys]). NextHour [F5] advances the clock
   one hour; Clock [F7] cycles it between running, stopped and following the PC's local time;
   ClockSpeed [[] cycles its speed. Weather [F6] cycles automatic weather, the current weather
   frozen, then each preset; Rain []] cycles the rain intensity. They write the same TimeOfDay
   fields the "timespeed"/"timestopped" debug options set, and send the same WeatherManager
   requests the Bucket List challenges and the "pauseweather"/"weather*intensity" debug options
   send. Keys are read on the UI frame and queued; ForzaSimulation's update performs the change
   on the game thread. Each change is shown for a moment in the HUD's status pop-up, the one
   Autodrive uses, in the game's text language. */

/* ForzaSimulation vtable slot 15, the per-frame update. Slot 16 only runs while loading. */
#define SIMULATION_UPDATE_SLOT_RVA 0x2c5fb58u
#define SIMULATION_UPDATE_RVA 0x2fb8f0u
#define GET_TIME_RVA 0x197a880u
#define SET_TIME_RVA 0x197aec0u
#define APPLY_WEATHER_RVA 0x1981860u
#define SIMULATION_CLOCK_OFFSET 0x480u
#define SIMULATION_WEATHER_OFFSET 0x488u
#define SECONDS_PER_DAY 86400.0f

/* TimeOfDay fields: +0x10 time (s), +0x18 game seconds per real second, +0x24 stopped,
   +0x25 time locked, +0x26 speed overridden, +0x27 stopped state overridden. */
#define CLOCK_SPEED 0x18u
#define CLOCK_STOPPED 0x24u
#define CLOCK_LOCKED 0x25u
#define CLOCK_SPEED_SET 0x26u
#define CLOCK_STOP_SET 0x27u

/* WeatherManager fields: +0x08 atmosphere, +0x18 environment wetness, +0x5c precipitation,
   +0x158 active preset list (vector of 200-byte entries, mainland or Storm Island). */
#define WEATHER_ATMOSPHERE 0x08u
#define WEATHER_ENVIRONMENT 0x18u
#define WEATHER_PRECIPITATION 0x5cu
#define WEATHER_PRESETS 0x158u
#define WEATHER_PRESET_SIZE 200

enum { WEATHER_AUTOMATIC = -2, WEATHER_FROZEN = -1 };
enum { RAIN_AUTOMATIC = -1, RAIN_LEVELS = 4, SPEED_LEVELS = 4 };
enum { CLOCK_RUNNING, CLOCK_HALTED, CLOCK_REAL_TIME };
enum { ACTION_HOUR, ACTION_WEATHER, ACTION_CLOCK, ACTION_SPEED, ACTION_RAIN, ACTION_COUNT };

typedef void (*UpdateFn)(void *, float, float, float, int, char, char, char, char);
typedef float (*GetTimeFn)(void *, int);
typedef void (*SetTimeFn)(void *, float, int);
typedef void (*ApplyWeatherFn)(void *, void *);

static const float rain_levels[RAIN_LEVELS] = {0.0f, 0.3f, 0.65f, 1.0f};
static const char *const rain_names[RAIN_LEVELS] = {"none", "light", "medium", "heavy"};
static const float speed_factors[SPEED_LEVELS] = {1.0f, 4.0f, 16.0f, 0.25f};
static const char *const speed_names[SPEED_LEVELS] = {"normal", "4x", "16x", "0.25x"};
static const KeyAction action_keys[ACTION_COUNT] = {KEY_NEXT_HOUR, KEY_WEATHER, KEY_CLOCK,
                                                    KEY_CLOCK_SPEED, KEY_RAIN};

static unsigned char *game_image;
static int enabled;
static volatile LONG pending[ACTION_COUNT], update_seen;
static int weather_mode = WEATHER_AUTOMATIC, rain_level = RAIN_AUTOMATIC, speed_level;
static int real_time;
static ULONGLONG real_time_synced;
static float frozen_atmosphere, frozen_environment, frozen_precipitation, normal_speed;
static unsigned char original_pointer[8], hook_pointer[8];
static const unsigned char *accepted_pointers[2];
PatchDefinition world_controls_hook_patch;

static void log_parts(const char *first, const char *second, const char *third) {
    char line[128];
    size_t length = 0;
    const char *parts[3] = {first, second, third};
    for (unsigned i = 0; i < 3; ++i) {
        for (const char *c = parts[i]; c && *c && length < sizeof(line) - 3; ++c) {
            line[length++] = *c;
        }
    }
    line[length++] = '\r';
    line[length++] = '\n';
    line[length] = 0;
    log_append(line);
}

static void two_digits(char *out, int value) {
    out[0] = (char)('0' + value / 10 % 10);
    out[1] = (char)('0' + value % 10);
}

static void wide_append(wchar_t *out, size_t capacity, const wchar_t *text) {
    size_t length = 0;
    while (out[length]) {
        ++length;
    }
    while (*text && length + 1 < capacity) {
        out[length++] = *text++;
    }
    out[length] = 0;
}

static void wide_number(wchar_t *out, size_t capacity, int value) {
    wchar_t digits[12];
    size_t count = 0;
    do {
        digits[count++] = (wchar_t)(L'0' + value % 10);
        value /= 10;
    } while (value && count < ARRAY_COUNT(digits));
    wchar_t text[12];
    for (size_t i = 0; i < count; ++i) {
        text[i] = digits[count - 1 - i];
    }
    text[count] = 0;
    wide_append(out, capacity, text);
}

static const ForkText *text(void) {
    return fork_text(game_image);
}

/* Real time keeps the clock stopped and sets the PC's local time once per second, because the
   game's own speed runs on a warped day (shorter nights), so it cannot track a wall clock. */
static void sync_real_time(unsigned char *clock) {
    real_time_synced = GetTickCount64();
    if (clock[CLOCK_LOCKED]) {
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    float seconds = (float)(now.wHour * 3600 + now.wMinute * 60 + now.wSecond);
    ((SetTimeFn)(game_image + SET_TIME_RVA))(clock, seconds, 1);
}

static void resume_clock(unsigned char *clock) {
    /* Hand the stopped state back to the game (Bucket List, events). */
    real_time = 0;
    clock[CLOCK_STOPPED] = 0;
    clock[CLOCK_STOP_SET] = 0;
}

static void advance_hour(unsigned char *clock) {
    /* The game's setter ignores the request while the clock is locked (events, replays). */
    if (clock[CLOCK_LOCKED]) {
        log_append("WORLD: next hour ignored; the game has locked the time of day.\r\n");
        autodrive_notice_show_text(text()->hour, text()->locked);
        return;
    }
    if (real_time) {
        resume_clock(clock);
    }
    float seconds = ((GetTimeFn)(game_image + GET_TIME_RVA))(clock, 1) + 3600.0f;
    while (seconds >= SECONDS_PER_DAY) {
        seconds -= SECONDS_PER_DAY;
    }
    while (seconds < 0.0f) {
        seconds += SECONDS_PER_DAY;
    }
    ((SetTimeFn)(game_image + SET_TIME_RVA))(clock, seconds, 1);
    int minutes = (int)(seconds / 60.0f);
    char clock_text[6] = {0, 0, ':', 0, 0, 0};
    two_digits(clock_text, minutes / 60);
    two_digits(clock_text + 3, minutes % 60);
    log_parts("WORLD: time set to ", clock_text, ".");
    wchar_t value[6] = {(wchar_t)clock_text[0], (wchar_t)clock_text[1], L':',
                        (wchar_t)clock_text[3], (wchar_t)clock_text[4], 0};
    autodrive_notice_show_text(text()->hour, value);
}

static void cycle_clock(unsigned char *clock) {
    int state = real_time ? CLOCK_REAL_TIME : clock[CLOCK_STOPPED] ? CLOCK_HALTED : CLOCK_RUNNING;
    if (state == CLOCK_REAL_TIME) {
        resume_clock(clock);
        log_append("WORLD: clock resumed.\r\n");
    } else if (state == CLOCK_HALTED) {
        real_time = 1;
        clock[CLOCK_STOPPED] = 1;
        clock[CLOCK_STOP_SET] = 1;
        sync_real_time(clock);
        log_append("WORLD: clock follows the PC's local time.\r\n");
    } else {
        clock[CLOCK_STOPPED] = 1;
        clock[CLOCK_STOP_SET] = 1;
        log_append("WORLD: clock stopped.\r\n");
    }
    autodrive_notice_show_text(text()->clock, text()->clock_states[(state + 1) % 3]);
}

static void cycle_speed(unsigned char *clock) {
    float *speed = (float *)(clock + CLOCK_SPEED);
    if (speed_level == 0) {
        normal_speed = *speed;
    }
    speed_level = (speed_level + 1) % SPEED_LEVELS;
    *speed = normal_speed * speed_factors[speed_level];
    clock[CLOCK_SPEED_SET] = speed_level != 0;
    log_parts("WORLD: clock speed ", speed_names[speed_level], ".");
    autodrive_notice_show_text(text()->clock_speed, text()->speeds[speed_level]);
}

/* Builds the whole request each time, because the game replaces every override with the
   request it receives. Layout copied into WeatherManager+0xe0, as the Bucket List builds it:
   +0x00/+0x04 preset, +0x10/+0x14 atmosphere, +0x18/+0x1c environment wetness,
   +0x20/+0x24 precipitation; a clear flag leaves that value to the automatic weather. */
static void apply_weather(unsigned char *weather) {
    unsigned char request[0x40] __attribute__((aligned(8))) = {0};
    float blend = 1.0f;
    if (weather_mode >= 0) {
        request[0] = 1;
        memory_copy(request + 0x04, &weather_mode, 4);
    } else if (weather_mode == WEATHER_FROZEN) {
        request[0x10] = 1;
        memory_copy(request + 0x14, &frozen_atmosphere, 4);
        request[0x18] = 1;
        memory_copy(request + 0x1c, &frozen_environment, 4);
        request[0x20] = 1;
        memory_copy(request + 0x24, &frozen_precipitation, 4);
    }
    if (rain_level != RAIN_AUTOMATIC) {
        request[0x20] = 1;
        memory_copy(request + 0x24, &rain_levels[rain_level], 4);
    }
    request[0x32] = 1;
    memory_copy(request + 0x34, &blend, 4);
    /* +0x38 (WeatherManager+0x118) clamps precipitation to the range the current atmosphere
       allows (FUN_141985de0): zero under a clear sky. A forced rain level skips that clamp. */
    request[0x38] = rain_level == RAIN_AUTOMATIC;
    ((ApplyWeatherFn)(game_image + APPLY_WEATHER_RVA))(weather, request);
}

static void cycle_weather(unsigned char *weather) {
    unsigned char **presets = *(unsigned char ***)(weather + WEATHER_PRESETS);
    int count = presets ? (int)((presets[1] - presets[0]) / WEATHER_PRESET_SIZE) : 0;
    if (count <= 0) {
        log_append("WORLD: weather key ignored; no weather presets are loaded.\r\n");
        return;
    }
    if (weather_mode == WEATHER_AUTOMATIC) {
        weather_mode = WEATHER_FROZEN;
        memory_copy(&frozen_atmosphere, weather + WEATHER_ATMOSPHERE, 4);
        memory_copy(&frozen_environment, weather + WEATHER_ENVIRONMENT, 4);
        memory_copy(&frozen_precipitation, weather + WEATHER_PRECIPITATION, 4);
    } else if (weather_mode + 1 >= count) {
        weather_mode = WEATHER_AUTOMATIC;
    } else {
        ++weather_mode;
    }
    apply_weather(weather);
    if (weather_mode == WEATHER_AUTOMATIC) {
        log_append("WORLD: weather returned to automatic.\r\n");
        autodrive_notice_show_text(text()->weather, text()->weather_automatic);
        return;
    }
    if (weather_mode == WEATHER_FROZEN) {
        log_append("WORLD: current weather frozen.\r\n");
        autodrive_notice_show_text(text()->weather, text()->weather_frozen);
        return;
    }
    char log_text[9] = {0, 0, ' ', 'o', 'f', ' ', 0, 0, 0};
    two_digits(log_text, weather_mode + 1);
    two_digits(log_text + 6, count);
    log_parts("WORLD: weather preset ", log_text, ".");
    /* The name follows the preset order of the XML; the position is shown too, so a list
       that differs still reads correctly. */
    const wchar_t *name = fork_text_preset(text(), weather_mode, count);
    wchar_t value[64] = {0};
    wide_append(value, ARRAY_COUNT(value), name ? name : L"Preset");
    wide_append(value, ARRAY_COUNT(value), L" (");
    wide_number(value, ARRAY_COUNT(value), weather_mode + 1);
    wide_append(value, ARRAY_COUNT(value), L"/");
    wide_number(value, ARRAY_COUNT(value), count);
    wide_append(value, ARRAY_COUNT(value), L")");
    autodrive_notice_show_text(text()->weather, value);
}

static void cycle_rain(unsigned char *weather) {
    rain_level = rain_level + 1 >= RAIN_LEVELS ? RAIN_AUTOMATIC : rain_level + 1;
    apply_weather(weather);
    log_parts("WORLD: rain ", rain_level == RAIN_AUTOMATIC ? "automatic" : rain_names[rain_level],
              ".");
    autodrive_notice_show_text(text()->rain, rain_level == RAIN_AUTOMATIC
                                                 ? text()->rain_automatic
                                                 : text()->rain_levels[rain_level]);
}

static void simulation_update(void *simulation, float a, float b, float c, int d, char e, char f,
                              char g, char h) {
    ((UpdateFn)(game_image + SIMULATION_UPDATE_RVA))(simulation, a, b, c, d, e, f, g, h);
    if (!InterlockedExchange(&update_seen, 1)) {
        log_append("WORLD: simulation update hook active.\r\n");
    }
    autodrive_notice_expire_text();
    unsigned char *base = (unsigned char *)simulation;
    unsigned char *clock = *(unsigned char **)(base + SIMULATION_CLOCK_OFFSET);
    unsigned char *weather = *(unsigned char **)(base + SIMULATION_WEATHER_OFFSET);
    if (real_time && clock && GetTickCount64() - real_time_synced >= 1000) {
        sync_real_time(clock);
    }
    for (unsigned action = 0; action < ACTION_COUNT; ++action) {
        for (LONG presses = InterlockedExchange(&pending[action], 0); presses > 0; --presses) {
            if (action == ACTION_HOUR && clock) {
                advance_hour(clock);
            } else if (action == ACTION_CLOCK && clock) {
                cycle_clock(clock);
            } else if (action == ACTION_SPEED && clock) {
                cycle_speed(clock);
            } else if (action == ACTION_WEATHER && weather) {
                cycle_weather(weather);
            } else if (action == ACTION_RAIN && weather) {
                cycle_rain(weather);
            }
        }
    }
    local_music_update();
}

void world_controls_frame(void) {
    if (!enabled) {
        return;
    }
    for (unsigned action = 0; action < ACTION_COUNT; ++action) {
        if (key_bindings_pressed(action_keys[action])) {
            InterlockedIncrement(&pending[action]);
        }
    }
}

void world_controls_prepare_hook(unsigned char *image, const ModOptions *options) {
    game_image = image;
    enabled = options->world_controls;
    uintptr_t original = (uintptr_t)(image + SIMULATION_UPDATE_RVA);
    uintptr_t hook = (uintptr_t)simulation_update;
    memory_copy(original_pointer, &original, 8);
    memory_copy(hook_pointer, &hook, 8);
    accepted_pointers[0] = original_pointer;
    accepted_pointers[1] = hook_pointer;
    world_controls_hook_patch = (PatchDefinition){SIMULATION_UPDATE_SLOT_RVA,
                                                  0,
                                                  8,
                                                  GROUP_WORLD_CONTROLS,
                                                  "world_controls_simulation_update",
                                                  original_pointer,
                                                  hook_pointer,
                                                  hook_pointer,
                                                  accepted_pointers,
                                                  2};
}

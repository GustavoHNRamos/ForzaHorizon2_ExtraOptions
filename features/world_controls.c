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
   on the game thread, or PhotoModeScene's update while photo mode has paused the world. Each
   change is shown for a moment in the HUD's status pop-up, the one Autodrive uses, in the
   game's text language. */

/* ForzaSimulation vtable slot 15, the per-frame update. Slot 16 only runs while loading. */
#define SIMULATION_UPDATE_SLOT_RVA 0x2c5fb58u
#define SIMULATION_UPDATE_RVA 0x2fb8f0u
#define GET_TIME_RVA 0x197a880u
#define SET_TIME_RVA 0x197aec0u
#define APPLY_WEATHER_RVA 0x1981860u
#define SIMULATION_CLOCK_OFFSET 0x480u
#define SIMULATION_WEATHER_OFFSET 0x488u
#define SECONDS_PER_DAY 86400.0f
/* PhotoModeScene vtable slot 153, its per-frame update (FUN_140dcf3e0). The simulation update
   stops while photo mode is open, so this one applies the keys there. */
#define PHOTO_UPDATE_SLOT_RVA 0x2e172e8u
#define PHOTO_UPDATE_RVA 0xdcf3e0u
#define PAUSED_AFTER_MS 200u
#define PHOTO_STATE_RVA 0x34b3e70u
#define PHOTO_REFRESH_OFFSET 0x14u

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
enum { CHANGED_CLOCK = 1, CHANGED_WEATHER = 2 };
enum { ACTION_HOUR, ACTION_WEATHER, ACTION_CLOCK, ACTION_SPEED, ACTION_RAIN, ACTION_COUNT };

typedef void (*UpdateFn)(void *, float, float, float, int, char, char, char, char);
typedef void (*PhotoUpdateFn)(void *);
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
static unsigned char *last_simulation;
static volatile LONG64 last_update_tick;
static int paused_apply;
/* What photo mode found, put back when it closes if a key changed it there. */
static struct {
    int open, changed, weather_mode, rain_level, speed_level, real_time;
    float time, frozen[3], sky[3], normal_speed;
    unsigned char clock[0x28];
} photo;
static volatile LONG64 photo_tick;
static unsigned char original_pointer[8], hook_pointer[8];
static const unsigned char *accepted_pointers[2];
static unsigned char photo_original_pointer[8], photo_hook_pointer[8];
static const unsigned char *photo_accepted_pointers[2];
PatchDefinition world_controls_hook_patch;
PatchDefinition world_controls_photo_patch;

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

/* Photo mode hides the HUD, and a pop-up shown there would only expire after leaving it. */
static void notice(const wchar_t *title, const wchar_t *value) {
    if (!paused_apply) {
        autodrive_notice_show_text(title, value);
    }
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
        notice(text()->hour, text()->locked);
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
    notice(text()->hour, value);
}

static void cycle_clock(unsigned char *clock) {
    int state = real_time ? CLOCK_REAL_TIME : clock[CLOCK_STOPPED] ? CLOCK_HALTED : CLOCK_RUNNING;
    if (state == CLOCK_REAL_TIME) {
        resume_clock(clock);
        log_append("WORLD: clock resumed.\r\n");
    } else if (state == CLOCK_HALTED && paused_apply) {
        /* No real time while the world is paused. */
        resume_clock(clock);
        state = CLOCK_REAL_TIME;
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
    notice(text()->clock, text()->clock_states[(state + 1) % 3]);
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
    notice(text()->clock_speed, text()->speeds[speed_level]);
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
        if (paused_apply) {
            /* Wetness builds up with time, which stands still in photo mode. */
            request[0x18] = 1;
            memory_copy(request + 0x1c, &rain_levels[rain_level], 4);
        }
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
    if (weather_mode == WEATHER_AUTOMATIC && paused_apply) {
        /* Freezing means nothing while the world is paused. */
        weather_mode = 0;
    } else if (weather_mode == WEATHER_AUTOMATIC) {
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
        notice(text()->weather, text()->weather_automatic);
        return;
    }
    if (weather_mode == WEATHER_FROZEN) {
        log_append("WORLD: current weather frozen.\r\n");
        notice(text()->weather, text()->weather_frozen);
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
    notice(text()->weather, value);
}

static void cycle_rain(unsigned char *weather) {
    rain_level = rain_level + 1 >= RAIN_LEVELS ? RAIN_AUTOMATIC : rain_level + 1;
    apply_weather(weather);
    log_parts("WORLD: rain ", rain_level == RAIN_AUTOMATIC ? "automatic" : rain_names[rain_level],
              ".");
    notice(text()->rain, rain_level == RAIN_AUTOMATIC
                                                 ? text()->rain_automatic
                                                 : text()->rain_levels[rain_level]);
}

static void perform(unsigned action, unsigned char *clock, unsigned char *weather) {
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

/* Photo mode accumulates samples while the camera is still and then keeps the finished image.
   Its update restarts the accumulation, as a camera move does, when this flag is set. */
static void refresh_photo(void) {
    unsigned char *state = *(unsigned char **)(game_image + PHOTO_STATE_RVA);
    if (state) {
        state[PHOTO_REFRESH_OFFSET] = 1;
    }
}

static void read_sky(unsigned char *weather, float *sky) {
    memory_copy(&sky[0], weather + WEATHER_ATMOSPHERE, 4);
    memory_copy(&sky[1], weather + WEATHER_ENVIRONMENT, 4);
    memory_copy(&sky[2], weather + WEATHER_PRECIPITATION, 4);
}

static void photo_open(unsigned char *clock, unsigned char *weather) {
    photo.open = 1;
    photo.changed = 0;
    photo.weather_mode = weather_mode;
    photo.rain_level = rain_level;
    photo.speed_level = speed_level;
    photo.real_time = real_time;
    photo.normal_speed = normal_speed;
    photo.frozen[0] = frozen_atmosphere;
    photo.frozen[1] = frozen_environment;
    photo.frozen[2] = frozen_precipitation;
    if (weather) {
        read_sky(weather, photo.sky);
    }
    if (clock) {
        photo.time = ((GetTimeFn)(game_image + GET_TIME_RVA))(clock, 1);
        memory_copy(photo.clock, clock, sizeof(photo.clock));
    }
}

static void photo_close(unsigned char *clock, unsigned char *weather) {
    photo.open = 0;
    if (!photo.changed) {
        return;
    }
    if (photo.changed & CHANGED_CLOCK) {
        speed_level = photo.speed_level;
        real_time = photo.real_time;
        normal_speed = photo.normal_speed;
    }
    if (weather && (photo.changed & CHANGED_WEATHER)) {
        /* Releasing the overrides leaves the photo mode sky in place, and the automatic weather
           would only drift away from it. Force the sky photo mode found first, then hand it
           back to the mode that was active. */
        weather_mode = WEATHER_FROZEN;
        rain_level = RAIN_AUTOMATIC;
        frozen_atmosphere = photo.sky[0];
        frozen_environment = photo.sky[1];
        frozen_precipitation = photo.sky[2];
        apply_weather(weather);
        weather_mode = photo.weather_mode;
        rain_level = photo.rain_level;
        frozen_atmosphere = photo.frozen[0];
        frozen_environment = photo.frozen[1];
        frozen_precipitation = photo.frozen[2];
        apply_weather(weather);
    }
    if (clock && (photo.changed & CHANGED_CLOCK)) {
        /* Speed and the stopped state; the time itself goes through the game's setter. */
        memory_copy(clock + CLOCK_SPEED, photo.clock + CLOCK_SPEED, 4);
        clock[CLOCK_STOPPED] = photo.clock[CLOCK_STOPPED];
        clock[CLOCK_SPEED_SET] = photo.clock[CLOCK_SPEED_SET];
        clock[CLOCK_STOP_SET] = photo.clock[CLOCK_STOP_SET];
        if (!clock[CLOCK_LOCKED]) {
            ((SetTimeFn)(game_image + SET_TIME_RVA))(clock, photo.time, 1);
        }
    }
    log_append("WORLD: photo mode changes undone.\r\n");
}

/* Applies the queued keys while photo mode has paused the world: no frozen weather or real
   time (nothing moves), wetness set at once with the rain, no HUD pop-up (photo mode hides
   the HUD), and the image accumulated again. */
static void photo_update(void *scene) {
    InterlockedExchange64(&photo_tick, (LONG64)GetTickCount64());
    ULONGLONG idle =
        GetTickCount64() - (ULONGLONG)InterlockedCompareExchange64(&last_update_tick, 0, 0);
    if (last_simulation && idle > PAUSED_AFTER_MS) {
        unsigned char *clock = *(unsigned char **)(last_simulation + SIMULATION_CLOCK_OFFSET);
        unsigned char *weather = *(unsigned char **)(last_simulation + SIMULATION_WEATHER_OFFSET);
        int applied = 0;
        if (!photo.open) {
            photo_open(clock, weather);
        }
        paused_apply = 1;
        for (unsigned action = 0; action < ACTION_COUNT; ++action) {
            for (LONG presses = InterlockedExchange(&pending[action], 0); presses > 0; --presses) {
                perform(action, clock, weather);
                applied = 1;
                /* Only what a key touched is put back, so an event's own weather stays. */
                photo.changed |= action == ACTION_WEATHER || action == ACTION_RAIN ? CHANGED_WEATHER
                                                                                  : CHANGED_CLOCK;
            }
        }
        paused_apply = 0;
        if (applied) {
            refresh_photo();
        }
    }
    ((PhotoUpdateFn)(game_image + PHOTO_UPDATE_RVA))(scene);
}

static void simulation_update(void *simulation, float a, float b, float c, int d, char e, char f,
                              char g, char h) {
    ((UpdateFn)(game_image + SIMULATION_UPDATE_RVA))(simulation, a, b, c, d, e, f, g, h);
    if (!InterlockedExchange(&update_seen, 1)) {
        log_append("WORLD: simulation update hook active.\r\n");
    }
    last_simulation = (unsigned char *)simulation;
    InterlockedExchange64(&last_update_tick, (LONG64)GetTickCount64());
    autodrive_notice_expire_text();
    unsigned char *base = (unsigned char *)simulation;
    unsigned char *clock = *(unsigned char **)(base + SIMULATION_CLOCK_OFFSET);
    unsigned char *weather = *(unsigned char **)(base + SIMULATION_WEATHER_OFFSET);
    if (photo.open && GetTickCount64() - (ULONGLONG)InterlockedCompareExchange64(&photo_tick, 0, 0) >
                          PAUSED_AFTER_MS) {
        photo_close(clock, weather);
    }
    if (real_time && clock && GetTickCount64() - real_time_synced >= 1000) {
        sync_real_time(clock);
    }
    for (unsigned action = 0; action < ACTION_COUNT; ++action) {
        for (LONG presses = InterlockedExchange(&pending[action], 0); presses > 0; --presses) {
            perform(action, clock, weather);
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
    uintptr_t photo_original = (uintptr_t)(image + PHOTO_UPDATE_RVA);
    uintptr_t photo_hook = (uintptr_t)photo_update;
    memory_copy(photo_original_pointer, &photo_original, 8);
    memory_copy(photo_hook_pointer, &photo_hook, 8);
    photo_accepted_pointers[0] = photo_original_pointer;
    photo_accepted_pointers[1] = photo_hook_pointer;
    world_controls_photo_patch = (PatchDefinition){PHOTO_UPDATE_SLOT_RVA,
                                                   0,
                                                   8,
                                                   GROUP_WORLD_CONTROLS,
                                                   "world_controls_photo_update",
                                                   photo_original_pointer,
                                                   photo_hook_pointer,
                                                   photo_hook_pointer,
                                                   photo_accepted_pointers,
                                                   2};
}

#ifndef FH2_FORK_TEXT_H
#define FH2_FORK_TEXT_H
#include "extra_options.h"

/* Labels and descriptions for the fork's rows in the game's HUD options menu. */
typedef struct {
    const wchar_t *hud, *world, *music, *volume;
    const wchar_t *hud_info, *world_info, *music_info, *volume_info, *key_info;
    const wchar_t *key, *actions[8], *none, *other;
} ForkMenuText;

typedef struct {
    const wchar_t *hour, *weather, *rain, *clock, *clock_speed, *music;
    const wchar_t *locked, *weather_automatic, *weather_frozen, *rain_automatic;
    const wchar_t *rain_levels[4];
    const wchar_t *clock_states[3];
    const wchar_t *speeds[4];
    const wchar_t *music_off, *music_empty, *music_unavailable, *music_failed;
    const wchar_t *presets[14];
    ForkMenuText menu;
} ForkText;

const ForkText *fork_text(unsigned char *game_image);
const wchar_t *fork_text_preset(const ForkText *text, int index, int count);
#endif

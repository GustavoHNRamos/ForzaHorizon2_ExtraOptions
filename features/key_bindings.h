#ifndef FH2_KEY_BINDINGS_H
#define FH2_KEY_BINDINGS_H
#include "extra_options.h"

typedef enum {
    KEY_TOGGLE_HUD,
    KEY_NEXT_HOUR,
    KEY_WEATHER,
    KEY_CLOCK,
    KEY_CLOCK_SPEED,
    KEY_RAIN,
    KEY_MUSIC_TOGGLE,
    KEY_MUSIC_NEXT,
    KEY_ACTION_TOTAL
} KeyAction;

/* A key setting is a virtual-key code, or KEY_CHARACTER | character for punctuation that
   depends on the keyboard layout, resolved with VkKeyScanW once User32 is loaded. */
#define KEY_CHARACTER 0x10000
_Static_assert(KEY_ACTION_TOTAL == KEY_ACTION_COUNT, "KeyAction and ModOptions.keys differ");

void key_bindings_defaults(int keys[KEY_ACTION_COUNT]);
int key_binding_parse(const char *text, int *value);
void key_binding_describe(int value, char *out, size_t capacity);
void key_bindings_prepare(const ModOptions *options);
void key_bindings_frame(void);
int key_bindings_pressed(KeyAction action);
#endif

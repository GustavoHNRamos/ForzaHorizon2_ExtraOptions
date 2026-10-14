#include "features.h"
#include "key_bindings.h"
#include "logging.h"
#include "support.h"

/* The HUD key (F3 by default) hides the Hud and Notifications controls the same way the game's own "nohud" debug
   option does: their AVUI Opacity is set to zero. Other UI (menus, in-car displays) is
   untouched. Both controls are captured when the game initializes them and forgotten when
   they are destroyed. All calls run on the UI render path. */

#define OPACITY_PROPERTY_RVA 0x36a5a08u
#define BOX_FLOAT_RVA 0x336ac0u
#define SET_VALUE_RVA 0x15f8a70u
#define DESTROY_UI_OBJECT_RVA 0x257730u

typedef void (*ControlFn)(void *);
typedef void *(*DestroyFn)(void *, unsigned);
typedef void **(*BoxFloatFn)(void **, const float *);
typedef void (*SetValueFn)(void *, void *, void *);
typedef void (*ReleaseFn)(void *);

enum { CONTROL_HUD, CONTROL_NOTIFICATIONS, CONTROL_COUNT };

static unsigned char *game_image;
static int enabled, hidden;
static void *controls[CONTROL_COUNT];

/* Slot 145 is the initializer that checks "nohud"; slot 0 is the deleting destructor. */
static const unsigned hook_slots[4] = {0x2defe50, 0x2def9c8, 0x2e03c68, 0x2e037e0};
static const unsigned hook_originals[4] = {0xd38dd0, 0xd244c0, 0xd81e80, 0xd6ddf0};
static const char *const hook_names[4] = {"hud_toggle_hud_init", "hud_toggle_hud_destroy",
                                          "hud_toggle_notifications_init",
                                          "hud_toggle_notifications_destroy"};

static void set_opacity(void *control, float value) {
    void *boxed = 0;
    ((BoxFloatFn)(game_image + BOX_FLOAT_RVA))(&boxed, &value);
    if (!boxed) {
        return;
    }
    ((SetValueFn)(game_image + SET_VALUE_RVA))(
        control, *(void **)(game_image + OPACITY_PROPERTY_RVA), boxed);
    if (InterlockedDecrement((volatile LONG *)((unsigned char *)boxed + 0x10)) == 0) {
        ((ReleaseFn)(game_image + DESTROY_UI_OBJECT_RVA))(boxed);
    }
}

static void control_init(unsigned index, void *self) {
    ((ControlFn)(game_image + hook_originals[index * 2]))(self);
    controls[index] = self;
    if (hidden) {
        set_opacity(self, 0.0f);
    }
}

static void *control_destroy(unsigned index, void *self, unsigned flags) {
    if (controls[index] == self) {
        controls[index] = 0;
    }
    return ((DestroyFn)(game_image + hook_originals[index * 2 + 1]))(self, flags);
}

static void hud_init(void *self) {
    control_init(CONTROL_HUD, self);
}

static void *hud_destroy(void *self, unsigned flags) {
    return control_destroy(CONTROL_HUD, self, flags);
}

static void notifications_init(void *self) {
    control_init(CONTROL_NOTIFICATIONS, self);
}

static void *notifications_destroy(void *self, unsigned flags) {
    return control_destroy(CONTROL_NOTIFICATIONS, self, flags);
}

void hud_toggle_frame(void) {
    if (!enabled) {
        return;
    }
    if (key_bindings_pressed(KEY_TOGGLE_HUD)) {
        hidden = !hidden;
        log_append(hidden ? "HUD: hidden.\r\n" : "HUD: shown.\r\n");
        if (!hidden) {
            for (unsigned i = 0; i < CONTROL_COUNT; ++i) {
                if (controls[i]) {
                    set_opacity(controls[i], 1.0f);
                }
            }
        }
    }
    /* Reapplied every frame so game-driven fades (menus, rewind) cannot bring it back. */
    if (hidden) {
        for (unsigned i = 0; i < CONTROL_COUNT; ++i) {
            if (controls[i]) {
                set_opacity(controls[i], 0.0f);
            }
        }
    }
}

static unsigned char original_pointers[4][8], hook_pointers[4][8];
static const unsigned char *accepted_pointers[4][2];
PatchDefinition hud_toggle_hook_patches[4];

void hud_toggle_prepare_hooks(unsigned char *image, const ModOptions *options) {
    uintptr_t hooks[4] = {(uintptr_t)hud_init, (uintptr_t)hud_destroy,
                          (uintptr_t)notifications_init, (uintptr_t)notifications_destroy};
    game_image = image;
    enabled = options->hud_toggle;
    for (unsigned i = 0; i < 4; ++i) {
        uintptr_t original = (uintptr_t)(image + hook_originals[i]);
        memory_copy(original_pointers[i], &original, 8);
        memory_copy(hook_pointers[i], &hooks[i], 8);
        accepted_pointers[i][0] = original_pointers[i];
        accepted_pointers[i][1] = hook_pointers[i];
        hud_toggle_hook_patches[i] = (PatchDefinition){hook_slots[i],
                                                       0,
                                                       8,
                                                       GROUP_HUD_TOGGLE,
                                                       hook_names[i],
                                                       original_pointers[i],
                                                       hook_pointers[i],
                                                       hook_pointers[i],
                                                       accepted_pointers[i],
                                                       2};
    }
}

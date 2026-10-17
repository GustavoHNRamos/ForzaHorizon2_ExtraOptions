#ifndef FH2_FORK_MENU_H
#define FH2_FORK_MENU_H
#include "extra_options.h"

void fork_menu_prepare(const wchar_t *settings_directory, const ModOptions *options);
size_t fork_menu_extend_hud(unsigned char *game_image, char *bytes, size_t length,
                            size_t capacity);
int fork_menu_register(unsigned char *game_image);
#endif

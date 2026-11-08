#ifndef MC_UI_FONT_H
#define MC_UI_FONT_H

#include "raylib.h"

bool ui_font_init(void);
void ui_font_unload(void);
void ui_font_set_scale(float scale);
void ui_text_draw(const char *text, int x, int y, int size, Color color);
int ui_text_width(const char *text, int size);

#endif

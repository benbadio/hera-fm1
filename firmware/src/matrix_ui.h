/* SPDX-License-Identifier: GPL-3.0-only */
/* MORPHEUS: 240x240 LCD Reason Matrix UI Engine */
#ifndef MATRIX_UI_H
#define MATRIX_UI_H

#include <stdint.h>

void matrix_ui_init(void);
void matrix_ui_draw(void);
void matrix_ui_dismiss_splash(void);
uint8_t matrix_ui_is_splash_active(void);
void matrix_ui_notify(const char *msg);
void matrix_ui_set_held_status(const char *msg);
void matrix_ui_clear_held_status(void);

#endif /* MATRIX_UI_H */

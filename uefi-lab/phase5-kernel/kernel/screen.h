/* screen.h - text console drawn straight into the UEFI framebuffer. */
#pragma once
#include <stdint.h>
#include "../include/boot.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

int  screen_init(const BOOT_INFO *bi);      /* 0 if there is no framebuffer */
void screen_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void screen_text_at(uint32_t col, uint32_t row, const char *s, uint32_t fg, uint32_t bg);
uint32_t screen_get_pixel(uint32_t x, uint32_t y);
void screen_clear_console(void);
void screen_panic(const char *title);       /* paint the whole screen red */

/* Console: scrolling text area below the title bar. */
void con_putc(char c);
void con_set_color(uint32_t fg);
uint32_t con_cols(void);

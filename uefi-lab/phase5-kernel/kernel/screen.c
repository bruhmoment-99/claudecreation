/* screen.c - drawing pixels and text into the framebuffer.
 *
 * The framebuffer is plain memory: pixel (x, y) is the 32-bit word at
 * fb_base + (y * pitch + x) * 4. Write to it and the picture changes.
 * No driver, no GPU commands - that's the whole "graphics stack". */
#include "screen.h"
#include "font.h"

#define SCALE   2                    /* each font pixel = 2x2 screen pixels */
#define CELL_W  (6 * SCALE)          /* 5 px glyph + 1 px gap */
#define CELL_H  (9 * SCALE)          /* 7 px glyph + 2 px gap */
#define TOP     (2 * CELL_H)         /* rows used by the title bar */

static volatile uint32_t *fb;
static uint32_t W, H, PITCH, FMT;
static uint32_t cur_col, cur_row, con_fg = RGB(230, 230, 230);
static uint32_t con_bg = RGB(16, 24, 48);

static inline uint32_t native(uint32_t rgb) {
    if (FMT == FB_FORMAT_BGRX) return rgb;   /* 0x00RRGGBB in memory is B,G,R,x */
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

int screen_init(const BOOT_INFO *bi) {
    if (bi->fb_format == FB_FORMAT_NONE || bi->fb_base == 0) return 0;
    fb = (volatile uint32_t *)(uintptr_t)bi->fb_base;
    W = bi->fb_width; H = bi->fb_height; PITCH = bi->fb_pitch; FMT = bi->fb_format;
    return 1;
}

void screen_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    if (!fb) return;
    uint32_t px = native(rgb);
    for (uint32_t j = y; j < y + h && j < H; j++)
        for (uint32_t i = x; i < x + w && i < W; i++)
            fb[(uint64_t)j * PITCH + i] = px;
}

uint32_t screen_get_pixel(uint32_t x, uint32_t y) { return fb ? native(fb[(uint64_t)y * PITCH + x]) : 0; }

static const GLYPH *find_glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    for (unsigned i = 0; i < sizeof FONT / sizeof FONT[0]; i++) if (FONT[i].ch == c) return &FONT[i];
    return c == '?' ? &FONT[0] : find_glyph('?');   /* unknown characters show as '?' */
}

static void draw_char(uint32_t px, uint32_t py, char c, uint32_t fg, uint32_t bg) {
    const GLYPH *g = find_glyph(c);
    screen_fill_rect(px, py, CELL_W, CELL_H, bg);
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 5; k++)
            if (g->rows[r][k] == '#') screen_fill_rect(px + k * SCALE, py + SCALE + r * SCALE, SCALE, SCALE, fg);
}

void screen_text_at(uint32_t col, uint32_t row, const char *s, uint32_t fg, uint32_t bg) {
    for (; *s; s++, col++) draw_char(col * CELL_W, row * CELL_H, *s, fg, bg);
}

uint32_t con_cols(void) { return fb ? W / CELL_W : 80; }
static uint32_t con_rows(void) { return fb ? (H - TOP) / CELL_H : 25; }

void screen_clear_console(void) {
    screen_fill_rect(0, TOP, W, H - TOP, con_bg);
    cur_col = cur_row = 0;
}

void con_set_color(uint32_t fg) { con_fg = fg; }

static void scroll(void) {
    /* Move every console line up by one text row: plain memory copy. */
    uint32_t y0 = TOP, rows_px = con_rows() * CELL_H;
    for (uint32_t y = y0; y < y0 + rows_px - CELL_H; y++)
        for (uint32_t x = 0; x < W; x++)
            fb[(uint64_t)y * PITCH + x] = fb[(uint64_t)(y + CELL_H) * PITCH + x];
    screen_fill_rect(0, y0 + rows_px - CELL_H, W, CELL_H, con_bg);
}

void con_putc(char c) {
    if (!fb) return;
    if (c == '\n') { cur_col = 0; cur_row++; }
    else if (c == '\b') { if (cur_col) { cur_col--; draw_char(cur_col * CELL_W, TOP + cur_row * CELL_H, ' ', con_fg, con_bg); } }
    else {
        draw_char(cur_col * CELL_W, TOP + cur_row * CELL_H, c, con_fg, con_bg);
        if (++cur_col >= con_cols()) { cur_col = 0; cur_row++; }
    }
    if (cur_row >= con_rows()) { scroll(); cur_row = con_rows() - 1; }
}

void screen_panic(const char *title) {
    if (!fb) return;
    screen_fill_rect(0, 0, W, H, RGB(150, 0, 0));
    screen_text_at(1, 1, title, RGB(255, 255, 255), RGB(150, 0, 0));
    cur_col = 0; cur_row = 2; con_fg = RGB(255, 255, 255); con_bg = RGB(150, 0, 0);
}

/*
 * Prayer time (祷告时光 · 与耶稣一同祷告) on the 1.50" 240x280 GC9306/GC9307 panel.
 *
 * Start screen: the picture of Jesus praying, with rising motes of light and a "开始祷告" button. Entering the
 * prayer scene shows "今日经文": soft rays of light and a Bible verse (Chinese Union Version) on a card over the
 * candle rack, below the face in the portrait.
 * The user prays in silence, then presses the key to light a candle - the press is not the prayer itself: a small
 * light rises and lights one of the red votive candles on the rack, "阿们" fades in and out at the top right and the
 * count of lit candles bumps. The first 24 light the whole rack. Before the first candle the panel shows the hint
 * "默祷之后，按一下点亮蜡烛". The count is kept in NVS; holding the key 1 s clears it.
 *
 * Art: tools/make_art.py, verses and fonts: tools/make_text.py. Frames are composed strip by strip into two
 * DMA buffers.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board.h"
#include "button.h"
#include "lcd.h"
#include "sprites.h"
#include "ui.h"
#include "verses.h"

static const char* TAG = "pray";

#define W BOARD_LCD_H_RES
#define H BOARD_LCD_V_RES

#define LINES_PER_CHUNK   40
#define MAX_TRANSFER_SIZE (W * LINES_PER_CHUNK * sizeof(uint16_t))

#define FLAME_DY      (-13)  // flame center above the cup center
#define RISE_MS       700    // the light rising to its candle
#define MAX_LIGHTS    6
#define KINDLE_MS     260    // new flame growing
#define RING_MS       520
#define BUMP_MS       160
#define POP_MS        1100   // "阿们" fades in and out where it is
#define MAX_POPS      4
#define TOAST_MS      1400
#define TOAST_FADE    350
#define SAVE_IDLE_MS  1000

#define VERSE_MS      8000   // rays + "今日经文" card when the prayer scene opens
#define VERSE_IN_MS   450
#define VERSE_OUT_MS  600
#define MOTES         7

#define COLOR_COUNT      UI_RGB(184, 50, 42)
#define COLOR_COUNT_BUMP UI_RGB(226, 150, 40)
#define C_VERSE          0x59A3  // #5a3418, true order
#define C_REF            0xABA5  // #a8742a

extern const uint8_t logo_start[] asm("_binary_logo_rgb565_start");
extern const uint8_t church_start[] asm("_binary_church_rgb565_start");
extern const uint8_t spr_start[] asm("_binary_sprites_bin_start");

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t         s_trans_done;

/* ---------------------------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------------------------*/

typedef struct {
    int64_t t0;  // 0 = free
    int     candle;
    float   sway;
} light_t;

typedef struct {
    float x0, y0, speed, phase;
} mote_t;

#define VERSE_LINES 6

static struct {
    bool     logo;
    uint32_t count;
    bool     dirty;
    int64_t  last_press;
    light_t  lights[MAX_LIGHTS];
    int64_t  kindle_t[CANDLE_COUNT];  // when each candle was (re)lit, for the flame growing in
    int64_t  ring_t;
    int      ring_candle;
    int64_t  bump_t;
    int64_t  pop_t[MAX_POPS];
    int64_t  toast_t;
    int64_t  verse_t;
    int      verse;
    const char* vline[VERSE_LINES];   // wrapped verse, pointers into s_vbuf
    int      vlines;
    mote_t   motes[MOTES];
} s;

static char    s_vbuf[512];
static int64_t s_now;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float ease_in_out(float t)
{
    t = clampf(t, 0, 1);
    return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (esp_random() & 0xFFFF) / 65535.0f;
}

static inline uint16_t blend(uint16_t bg, uint16_t fg, int a)
{
    const uint16_t b  = UI_SWAP16(bg);
    const int      rb = b >> 11, gb = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int      r  = rb + ((((fg >> 11) - rb) * a + 128) >> 8);
    const int      g  = gb + (((((fg >> 5) & 0x3F) - gb) * a + 128) >> 8);
    const int      bl = bb + ((((fg & 0x1F) - bb) * a + 128) >> 8);
    return UI_SWAP16((uint16_t)((r << 11) | (g << 5) | bl));
}

/* ---------------------------------------------------------------------------------------------
 * Text (UTF-8, fonts from tools/make_text.py)
 * -------------------------------------------------------------------------------------------*/

static uint32_t utf8_next(const char** p)
{
    const uint8_t* s8 = (const uint8_t*)*p;
    uint32_t       c  = *s8++;
    if (c >= 0xE0 && s8[0] && s8[1]) {
        c = ((c & 0x0F) << 12) | ((s8[0] & 0x3F) << 6) | (s8[1] & 0x3F);
        s8 += 2;
    } else if (c >= 0xC0 && s8[0]) {
        c = ((c & 0x1F) << 6) | (s8[0] & 0x3F);
        s8 += 1;
    }
    *p = (const char*)s8;
    return c;
}

static const cjk_glyph_t* glyph(const cjk_font_t* f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (f->cps[mid] == cp) {
            return &f->glyphs[mid];
        }
        if (f->cps[mid] < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static int char_adv(const cjk_font_t* f, uint32_t cp)
{
    const cjk_glyph_t* g = glyph(f, cp);
    return g ? g->adv : f->line_h / 2;
}

static int text_width(const cjk_font_t* f, const char* str)
{
    int w = 0;
    while (*str) {
        w += char_adv(f, utf8_next(&str));
    }
    return w;
}

static void text_draw(const cjk_font_t* f, const char* str, int x, int top, uint16_t color, int mul, uint16_t* strip,
                      int y0, int y1)
{
    if (top >= y1 || top + f->line_h <= y0) {
        return;
    }
    while (*str) {
        const cjk_glyph_t* g = glyph(f, utf8_next(&str));
        if (!g) {
            x += f->line_h / 2;
            continue;
        }
        const int gx = x + g->x, gy = top + g->y, stride = (g->w + 1) / 2;
        const int ya = gy > y0 ? gy : y0, yb = gy + g->h < y1 ? gy + g->h : y1;
        for (int py = ya; py < yb; py++) {
            const uint8_t* src = f->bits + g->off + (py - gy) * stride;
            uint16_t*      out = strip + (py - y0) * W;
            for (int i = 0; i < g->w; i++) {
                const int px = gx + i;
                int       a  = (i & 1) ? src[i / 2] & 0x0F : src[i / 2] >> 4;
                if (!a || px < 0 || px >= W) {
                    continue;
                }
                a       = a * 17 * mul >> 8;
                out[px] = blend(out[px], color, a + (a >> 7));
            }
        }
        x += g->adv;
    }
}

static bool is_closing_punct(uint32_t cp)
{
    return cp == 0xFF0C || cp == 0x3002 || cp == 0xFF01 || cp == 0x3001 || cp == 0xFF1B || cp == 0xFF1A ||
           cp == 0xFF1F;
}

/* Wrap the verse to the card width into s.vline[] */
static void layout_verse(int idx)
{
    const cjk_font_t* f     = &font_verse;
    const int         max_w = CARD_W - 36;
    const char*       p     = s_verses[idx].text;
    int               used  = 0;
    s.vlines                = 0;
    while (*p && s.vlines < VERSE_LINES) {
        const char* start = p;
        const char* cut   = p;
        int         w     = 0;
        while (*cut) {
            const char*    q   = cut;
            const uint32_t cp  = utf8_next(&q);
            const int      adv = char_adv(f, cp);
            if (w + adv > max_w && cut != start && !is_closing_punct(cp)) {
                break;
            }
            w += adv;
            cut = q;
        }
        const int len = (int)(cut - start);
        if (used + len + 1 > (int)sizeof(s_vbuf)) {
            break;
        }
        memcpy(s_vbuf + used, start, len);
        s_vbuf[used + len]   = 0;
        s.vline[s.vlines++]  = s_vbuf + used;
        used                += len + 1;
        p                    = cut;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Counting, NVS
 * -------------------------------------------------------------------------------------------*/

#define NVS_NS  "pray"
#define NVS_KEY "prayers"

static void count_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY, &s.count);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "prayers so far: %lu", (unsigned long)s.count);
}

static void count_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u32(h, NVS_KEY, s.count) == ESP_OK && nvs_commit(h) == ESP_OK) {
        s.dirty = false;
        ESP_LOGI(TAG, "saved %lu prayers", (unsigned long)s.count);
    }
    nvs_close(h);
}

static void count_reset(int64_t now)
{
    s.count   = 0;
    s.dirty   = false;
    s.bump_t  = now;
    s.toast_t = now;
    s.verse_t = now - 100000;
    memset(s.lights, 0, sizeof(s.lights));
    for (int i = 0; i < MAX_POPS; i++) {
        s.pop_t[i] = now - 100000;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_erase_key(h, NVS_KEY);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "count reset, erased from flash");
}

static void show_verse(int64_t now)
{
    int v = esp_random() % VERSE_COUNT;
    if (v == s.verse) {
        v = (v + 1) % VERSE_COUNT;
    }
    s.verse   = v;
    s.verse_t = now;
    layout_verse(v);
    ESP_LOGI(TAG, "verse of the day: %s", s_verses[v].ref);
}

static void arrive(const light_t* l, int64_t now)
{
    s.count++;
    s.dirty                = true;
    s.kindle_t[l->candle]  = now;
    s.ring_t               = now;
    s.ring_candle          = l->candle;
    s.bump_t               = now;
    int oldest             = 0;
    for (int i = 1; i < MAX_POPS; i++) {
        if (s.pop_t[i] < s.pop_t[oldest]) {
            oldest = i;
        }
    }
    s.pop_t[oldest] = now;
}

static void pray(int64_t now)
{
    int in_air = 0, free_i = -1;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (s.lights[i].t0) {
            in_air++;
        } else if (free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        return;  // every light that rises is counted; beyond that the press is dropped
    }
    const uint32_t n = s.count + in_air;
    light_t*       l = &s.lights[free_i];
    l->t0            = now;
    l->candle        = n < CANDLE_COUNT ? (int)n : (int)(esp_random() % CANDLE_COUNT);  // full rack: rekindle one
    l->sway          = frand(-30, 30);
    s.last_press     = now;
}

static void update(int64_t now)
{
    for (int i = 0; i < MAX_LIGHTS; i++) {
        light_t* l = &s.lights[i];
        if (l->t0 && now - l->t0 >= RISE_MS) {
            arrive(l, now);
            l->t0 = 0;
        }
    }
    if (s.dirty && now - s.last_press > SAVE_IDLE_MS) {
        count_save();
    }
}

/* ---------------------------------------------------------------------------------------------
 * Drawing
 * -------------------------------------------------------------------------------------------*/

static void blit_at(int id, int dcx, int dcy, int cx, int cy, int scale, int mul, uint16_t* strip, int y0, int y1)
{
    const sprite_t* sp = &s_sprites[id];
    if (scale <= 0 || mul <= 0) {
        return;
    }
    const int      left = cx + (sp->x - dcx) * scale / 256, top = cy + (sp->y - dcy) * scale / 256;
    const int      w = sp->w * scale / 256, h = sp->h * scale / 256;
    const int      ya = top > y0 ? top : y0, yb = top + h < y1 ? top + h : y1;
    const int      xa = left > 0 ? left : 0, xb = left + w < W ? left + w : W;
    const uint8_t* color = spr_start + sp->off;
    const uint8_t* alpha = color + (uint32_t)sp->w * sp->h * 2;
    for (int py = ya; py < yb; py++) {
        const int sy  = scale == 256 ? py - top : (py - top) * 256 / scale;
        uint16_t* out = strip + (py - y0) * W;
        for (int px = xa; px < xb; px++) {
            const int sx = scale == 256 ? px - left : (px - left) * 256 / scale;
            if (sx >= sp->w || sy >= sp->h) {
                continue;
            }
            const int si = sy * sp->w + sx;
            int       a  = alpha[si];
            if (!a) {
                continue;
            }
            if (mul < 256) {
                a = a * mul >> 8;
            }
            const uint16_t c = (uint16_t)(color[2 * si] << 8 | color[2 * si + 1]);
            out[px]          = a >= 255 ? UI_SWAP16(c) : blend(out[px], c, a + (a >> 7));
        }
    }
}

static void blit(int id, int dx, int dy, int mul, uint16_t* strip, int y0, int y1)
{
    blit_at(id, 0, 0, dx, dy, 256, mul, strip, y0, y1);
}

/* Motes of light drifting slowly upwards, twinkling */
static void draw_motes(uint16_t* strip, int y0, int y1)
{
    const float t_s = s_now / 1000.0f;
    for (int i = 0; i < MOTES; i++) {
        const mote_t* m    = &s.motes[i];
        const float   span = H + 20;
        const float   y    = span - fmodf(m->y0 + m->speed * t_s, span) - 10;
        const float   x    = m->x0 + 6 * sinf(t_s * 0.7f + m->phase);
        const float   tw   = 0.5f + 0.5f * sinf(t_s * 2.2f + m->phase * 3);
        blit_at(SPR_MOTE, FX, FY, (int)x, (int)y, 256, (int)(80 + 150 * tw), strip, y0, y1);
    }
}

static void render_logo(uint16_t* strip, int y0, int y1)
{
    memcpy(strip, logo_start + y0 * W * 2, (size_t)(y1 - y0) * W * 2);
    draw_motes(strip, y0, y1);
    const float pulse = 0.5f + 0.5f * sinf(s_now * (2 * (float)M_PI) / 1800);
    blit(SPR_BUTTON, 0, 0, 180 + (int)(76 * pulse), strip, y0, y1);
}

static void render_church(uint16_t* strip, int y0, int y1)
{
    const int64_t now = s_now;
    memcpy(strip, church_start + y0 * W * 2, (size_t)(y1 - y0) * W * 2);

    const int64_t tv   = now - s.verse_t;
    int           fade = 0;
    if (tv >= 0 && tv < VERSE_MS) {
        fade = tv < VERSE_IN_MS ? (int)(tv * 256 / VERSE_IN_MS)
                                : (tv > VERSE_MS - VERSE_OUT_MS ? (int)((VERSE_MS - tv) * 256 / VERSE_OUT_MS) : 256);
        const float breathe = 0.8f + 0.2f * sinf(tv * 0.003f);
        blit(SPR_RAYS, 0, 0, (int)(fade * breathe), strip, y0, y1);
    }

    const int lit = s.count < CANDLE_COUNT ? (int)s.count : CANDLE_COUNT;
    for (int i = 0; i < lit; i++) {
        const int cx = s_candles[i][0], cy = s_candles[i][1];
        blit_at(SPR_CUP_LIT, FX, FY, cx, cy, 256, 256, strip, y0, y1);
        const float flick = 0.8f + 0.14f * sinf(now * 0.011f + i * 1.7f) + 0.06f * sinf(now * 0.029f + i);
        const int64_t kt  = now - s.kindle_t[i];
        const int   scale = kt >= 0 && kt < KINDLE_MS ? (int)(256 * (0.2f + 0.8f * kt / KINDLE_MS)) : 256;
        blit_at(SPR_FLAME, FX, FY, cx, cy + FLAME_DY, scale, (int)(256 * flick), strip, y0, y1);
    }
    if (now - s.ring_t < RING_MS) {  // a soft ring of light around the candle just lit
        const float p = (float)(now - s.ring_t) / RING_MS;
        blit_at(SPR_RING, FX, FY, s_candles[s.ring_candle][0], s_candles[s.ring_candle][1] + FLAME_DY,
                (int)(256 * (0.7f + 0.8f * p)), (int)(256 * (1 - p)), strip, y0, y1);
    }

    for (int i = 0; i < MAX_LIGHTS; i++) {  // lights rising to their candles
        const light_t* l = &s.lights[i];
        if (!l->t0) {
            continue;
        }
        const float e  = ease_in_out((float)(now - l->t0) / RISE_MS);
        const float tx = s_candles[l->candle][0], ty = s_candles[l->candle][1] + FLAME_DY;
        const float x  = 120 + (tx - 120) * e + l->sway * sinf(e * (float)M_PI);
        const float y  = 292 + (ty - 292) * e;
        blit_at(SPR_ORB, FX, FY, (int)x, (int)y, 256, 230, strip, y0, y1);
    }

    draw_motes(strip, y0, y1);

    /* counter panel: "已点亮 N 支", or before the first candle the hint to pray first */
    bool waiting = s.count == 0;
    for (int i = 0; i < MAX_LIGHTS && waiting; i++) {
        waiting = !s.lights[i].t0;
    }
    const int label_top = PILL_Y + PILL_H / 2 - font_ref.line_h / 2;
    if (waiting) {
        text_draw(&font_ref, TXT_HINT, W / 2 - text_width(&font_ref, TXT_HINT) / 2, label_top, C_VERSE, 256, strip, y0,
                  y1);
    } else {
        char txt[12];
        snprintf(txt, sizeof(txt), "%lu", (unsigned long)s.count);
        const ui_font_t* font  = strlen(txt) > 4 ? &ui_font_lb : &ui_font_xl;
        int              dy    = 0;
        uint16_t         color = COLOR_COUNT;
        if (now - s.bump_t < BUMP_MS) {
            dy    = -(int)lroundf(6 * sinf((float)(now - s.bump_t) / BUMP_MS * (float)M_PI));
            color = COLOR_COUNT_BUMP;
        }
        ui_text(font, txt, W / 2 - ui_text_width(font, txt) / 2,
                PILL_Y + PILL_H / 2 - (font->cap + font->ascent) / 2 + dy, color, strip, y0, y1);
        text_draw(&font_ref, TXT_LIT_L, PILL_X + 16, label_top, C_REF, 256, strip, y0, y1);
        text_draw(&font_ref, TXT_LIT_R, PILL_X + PILL_W - 18 - text_width(&font_ref, TXT_LIT_R), label_top, C_REF, 256,
                  strip, y0, y1);
    }

    for (int i = 0; i < MAX_POPS; i++) {  // "阿们": a quiet fade in and out, no movement
        const int64_t t = now - s.pop_t[i];
        if (t < 0 || t >= POP_MS) {
            continue;
        }
        const float a = t < 250 ? t / 250.0f : (t > POP_MS - 500 ? (POP_MS - t) / 500.0f : 1);
        blit(SPR_AMEN, 0, 0, (int)(230 * clampf(a, 0, 1)), strip, y0, y1);
    }

    if (fade) {  // "今日经文" card
        const int rise    = (256 - fade) * 8 / 256;
        const int card_y  = CARD_CY - CARD_H / 2 + rise;
        blit(SPR_CARD, 0, rise, fade, strip, y0, y1);
        text_draw(&font_ref, TXT_TODAY, CARD_CX - text_width(&font_ref, TXT_TODAY) / 2, card_y + 8, C_REF, fade, strip,
                  y0, y1);
        const int line_step = 20;
        const int block     = s.vlines * line_step + 18;  // verse lines + reference
        const int area_top  = card_y + 26, area_bot = card_y + CARD_H - 4;
        int       top       = area_top + (area_bot - area_top - block) / 2;
        for (int i = 0; i < s.vlines; i++, top += line_step) {
            text_draw(&font_verse, s.vline[i], CARD_CX - text_width(&font_verse, s.vline[i]) / 2, top, C_VERSE, fade,
                      strip, y0, y1);
        }
        char ref[48];
        snprintf(ref, sizeof(ref), "—— %s", s_verses[s.verse].ref);
        text_draw(&font_ref, ref, CARD_CX + CARD_W / 2 - 18 - text_width(&font_ref, ref), top + 1, C_REF, fade, strip,
                  y0, y1);
    }

    const int64_t tt = now - s.toast_t;
    if (tt >= 0 && tt < TOAST_MS) {
        blit(SPR_TOAST_RESET, 0, 0, tt < TOAST_MS - TOAST_FADE ? 256 : (int)((TOAST_MS - tt) * 256 / TOAST_FADE), strip,
             y0, y1);
    }
}

static void render_strip(uint16_t* strip, int y0, int y1)
{
    if (s.logo) {
        render_logo(strip, y0, y1);
    } else {
        render_church(strip, y0, y1);
    }
}

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

static void draw_frame(uint16_t* bufs[2])
{
    int  cur     = 0;
    bool pending = false;
    for (int y0 = 0; y0 < H; y0 += LINES_PER_CHUNK) {
        int y1 = y0 + LINES_PER_CHUNK < H ? y0 + LINES_PER_CHUNK : H;
        render_strip(bufs[cur], y0, y1);
        if (pending) {
            xSemaphoreTake(s_trans_done, portMAX_DELAY);
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, W, y1, bufs[cur]);
        pending = true;
        cur ^= 1;
    }
    xSemaphoreTake(s_trans_done, portMAX_DELAY);
}

/* ---------------------------------------------------------------------------------------------*/

void app_main(void)
{
    count_load();
    ESP_ERROR_CHECK(lcd_init(&s_panel, &s_io, MAX_TRANSFER_SIZE));

    s_trans_done      = xSemaphoreCreateBinary();
    uint16_t* bufs[2] = {heap_caps_malloc(MAX_TRANSFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                         heap_caps_malloc(MAX_TRANSFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)};
    assert(s_trans_done && bufs[0] && bufs[1]);
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_trans_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    const int64_t t0 = now_ms();
    s.logo           = true;
    s.verse          = -1;
    s.ring_t = s.bump_t = s.toast_t = s.verse_t = t0 - 100000;
    for (int i = 0; i < MAX_POPS; i++) {
        s.pop_t[i] = t0 - 100000;
    }
    for (int i = 0; i < CANDLE_COUNT; i++) {
        s.kindle_t[i] = t0 - 100000;
    }
    for (int i = 0; i < MOTES; i++) {
        s.motes[i] = (mote_t){frand(10, W - 10), frand(0, H), frand(8, 16), frand(0, 6.28f)};
    }

    s_now = t0;
    draw_frame(bufs);
    lcd_backlight(true);
    button_init();
    ESP_LOGI(TAG, "ready: click BOOT to pray, hold 1 s to reset the count");

    bool long_fired = false;
    while (1) {
        const int64_t  now = now_ms();
        button_event_t ev;
        while ((ev = button_get(0)) != BUTTON_NONE) {
            if (ev == BUTTON_DOWN) {
                long_fired = false;
                if (!s.logo) {
                    pray(now);
                }
            } else if (ev == BUTTON_LONG) {  // BUTTON_LONG_MS = 1000
                long_fired = true;
                if (!s.logo) {
                    count_reset(now);
                }
            } else if (ev == BUTTON_UP && !long_fired && s.logo) {
                s.logo = false;  // enter the prayer scene (not counted) with the verse of the day
                show_verse(now);
            }
        }
        update(now);
        s_now = now;
        draw_frame(bufs);
        vTaskDelay(1);
    }
}

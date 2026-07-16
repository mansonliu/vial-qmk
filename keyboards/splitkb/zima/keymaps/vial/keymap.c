/* Vial keymap + Claude Code status display for splitkb Zima
 *
 * Coexists with VIA/Vial on the shared Raw HID interface: host messages use
 * command id 0x63 ('c'), which VIA routes to raw_hid_receive_kb(). 32-byte
 * reports, layout:
 *   byte 0 = 0x63 magic
 *   byte 1 = command: 0x01 model / 0x02 status / 0x03 info
 *   byte 2.. = payload (NUL-terminated ASCII for text, max 21 chars;
 *              status byte: 0 idle / 1 working / 2 waiting / 3 error)
 */
#include QMK_KEYBOARD_H
#include <string.h>

#define CLAUDE_MAGIC 0x63

enum claude_cmd {
    CMD_MODEL  = 0x01,
    CMD_STATUS = 0x02,
    CMD_INFO   = 0x03,
    CMD_USAGE  = 0x04, // payload: five-hour %, seven-day % (0-100, 255 = unknown)
};

enum claude_status {
    ST_IDLE = 0,
    ST_WORKING,
    ST_WAITING,
    ST_ERROR,
};

#define TEXT_COLS 21
// Consider host gone after this long without any push.
#define HOST_STALE_MS 3600000UL

static char     model_str[TEXT_COLS + 1];
static char     info_str[TEXT_COLS + 1];
static uint8_t  claude_status = ST_IDLE;
static uint8_t  usage_5h      = 255;
static uint8_t  usage_7d      = 255;
static uint32_t last_hid_time = 0;
static bool     hid_seen      = false;

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT_ortho_4x3( /* Claude control; top-left is the encoder push */
        KC_ENT,  TG(1),   TG(2),
        KC_ESC,  KC_UP,   S(KC_TAB),
        KC_TAB,  KC_DOWN, KC_ENT,
        KC_1,    KC_2,    KC_3
    ),
    [1] = LAYOUT_ortho_4x3( /* Firmware / misc */
        QK_BOOT, _______, XXXXXXX,
        XXXXXXX, XXXXXXX, XXXXXXX,
        XXXXXXX, XXXXXXX, XXXXXXX,
        XXXXXXX, XXXXXXX, XXXXXXX
    ),
    [2] = LAYOUT_ortho_4x3( /* RGB / haptic */
        UG_TOGG, UG_NEXT, _______,
        UG_HUEU, UG_SATU, UG_VALU,
        UG_HUED, UG_SATD, UG_VALD,
        HF_TOGG, HF_FDBK, HF_CONT
    ),
    [3] = LAYOUT_ortho_4x3(
        _______, _______, _______,
        _______, _______, _______,
        _______, _______, _______,
        _______, _______, _______
    )
};

#ifdef ENCODER_MAP_ENABLE
// CCW = up, CW = down: matches walking through Claude Code menu options.
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [0] = {ENCODER_CCW_CW(KC_UP, KC_DOWN)},
    [1] = {ENCODER_CCW_CW(KC_VOLD, KC_VOLU)},
    [2] = {ENCODER_CCW_CW(UG_VALD, UG_VALU)},
    [3] = {ENCODER_CCW_CW(_______, _______)}
};
#endif

#ifdef RGBLIGHT_ENABLE
static void apply_status_rgb(void) {
    switch (claude_status) {
        case ST_WORKING:
            rgblight_enable_noeeprom();
            rgblight_mode_noeeprom(RGBLIGHT_MODE_BREATHING + 1);
            rgblight_sethsv_noeeprom(128, 255, 120); // cyan breathing
            break;
        case ST_WAITING:
            rgblight_enable_noeeprom();
            rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);
            rgblight_sethsv_noeeprom(43, 255, 200); // yellow
            break;
        case ST_ERROR:
            rgblight_enable_noeeprom();
            rgblight_mode_noeeprom(RGBLIGHT_MODE_STATIC_LIGHT);
            rgblight_sethsv_noeeprom(0, 255, 150); // red
            break;
        default:
            rgblight_disable_noeeprom();
            break;
    }
}

// While waiting for input, blink the underglow in phase with the OLED text
// (600ms on / 300ms off — same timer phase as the NEED INPUT blink).
void housekeeping_task_user(void) {
    static bool blink_on = true;
    if (claude_status == ST_WAITING) {
        bool on = (timer_read32() % 900) < 600;
        if (on != blink_on) {
            blink_on = on;
            rgblight_sethsv_noeeprom(43, 255, on ? 200 : 0);
        }
    } else {
        blink_on = true;
    }
}
#endif

static void copy_payload(char *dst, const uint8_t *src, uint8_t maxlen) {
    uint8_t i = 0;
    for (; i < maxlen && src[i] != 0; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = '\0';
}

void raw_hid_receive_kb(uint8_t *data, uint8_t length) {
    if (data[0] != CLAUDE_MAGIC) {
        data[0] = 0xFF; // id_unhandled
        return;
    }
    switch (data[1]) {
        case CMD_MODEL:
            copy_payload(model_str, data + 2, TEXT_COLS);
            break;
        case CMD_STATUS: {
            uint8_t new_status = data[2] <= ST_ERROR ? data[2] : ST_ERROR;
            if (new_status != claude_status) {
                claude_status = new_status;
#ifdef HAPTIC_ENABLE
                if (claude_status == ST_WAITING) haptic_play();
#endif
#ifdef RGBLIGHT_ENABLE
                apply_status_rgb();
#endif
            }
            break;
        }
        case CMD_INFO:
            copy_payload(info_str, data + 2, TEXT_COLS);
            break;
        case CMD_USAGE:
            usage_5h = data[2];
            usage_7d = data[3];
            break;
        default:
            data[0] = 0xFF;
            return;
    }
    last_hid_time = timer_read32();
    hid_seen      = true;
    oled_on();
}

#ifdef OLED_ENABLE
#    include "bigfont.h"
#    include "spark.h"

// Portrait mode (keyboard lies sideways): logical canvas is 32 wide x 128
// tall (16 pages). Model letters stack vertically like a signboard, version
// number is a small line below, and a fixed status cell sits at the bottom.
#    define P_WIDTH 32

oled_rotation_t oled_init_user(oled_rotation_t rotation) {
    return OLED_ROTATION_270;
}

static const unsigned char *glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 0x20;
    if (c < 0x20 || c > 0x5F) c = '?';
    return &big_font[(c - 0x20) * 6];
}

// One scaled char (width 6*scale px, height scale pages), centered,
// pixel-stretched and bolded, drawn on pages page..page+scale-1.
static void draw_scaled_char(char c, uint8_t page, uint8_t scale) {
    const unsigned char *g    = glyph(c);
    uint16_t             base = (uint16_t)page * P_WIDTH;
    uint8_t              w    = 6 * scale;
    uint8_t              x0   = (P_WIDTH - w) / 2;
    uint32_t             prev = 0;
    for (uint8_t x = 0; x < P_WIDTH; x++) {
        uint32_t col = 0;
        if (x >= x0 && x < x0 + w) {
            uint8_t fb = pgm_read_byte(&g[(x - x0) / scale]);
            for (uint8_t i = 0; i < 8; i++) {
                if (fb & (1 << i)) col |= (uint32_t)((1UL << scale) - 1) << (i * scale);
            }
        }
        uint32_t out = col | prev; // horizontal smear = bold
        prev         = col;
        for (uint8_t p = 0; p < scale; p++) {
            oled_write_raw_byte((out >> (8 * p)) & 0xFF, base + (uint16_t)p * P_WIDTH + x);
        }
    }
}

// Small 6x8 text at an explicit x position on a single page.
static void draw_small_at(const char *s, uint8_t page, uint8_t x0) {
    uint16_t base = (uint16_t)page * P_WIDTH;
    for (uint8_t k = 0; s[k]; k++) {
        for (uint8_t c = 0; c < 6; c++) {
            uint8_t x = x0 + k * 6 + c;
            if (x >= P_WIDTH) return;
            oled_write_raw_byte(pgm_read_byte(&glyph(s[k])[c]), base + x);
        }
    }
}

// Vertical bar with 1px outline, filled bottom-up to pct (0-100).
static void draw_vbar(uint8_t x0, uint8_t w, uint8_t page0, uint8_t npages, uint8_t pct) {
    uint16_t total     = npages * 8;
    uint16_t fill_from = total - ((uint32_t)pct * total + 50) / 100;
    for (uint8_t p = 0; p < npages; p++) {
        uint16_t base = (uint16_t)(page0 + p) * P_WIDTH;
        for (uint8_t x = x0; x < x0 + w; x++) {
            uint8_t b = 0;
            for (uint8_t i = 0; i < 8; i++) {
                uint16_t y      = p * 8 + i;
                bool     border = (x == x0 || x == x0 + w - 1 || y == 0 || y == total - 1);
                if (border || y >= fill_from) b |= (1 << i);
            }
            oled_write_raw_byte(b, base + x);
        }
    }
}

// Compact period for version strings: 6x4 px dot in a single 8px page.
static void draw_dot(uint8_t page) {
    uint16_t base = (uint16_t)page * P_WIDTH;
    for (uint8_t x = 13; x < 19; x++)
        oled_write_raw_byte(0x3C, base + x);
}

// One usage column: small label above, full-height vertical bar.
static void draw_usage_col(const char *label, uint8_t v, uint8_t bar_x, uint8_t bar_w) {
    uint8_t cx = bar_x + bar_w / 2;
    draw_small_at(label, 0, cx - 6);
    draw_vbar(bar_x, bar_w, 1, 15, v);
}

// Blit one 32x48 spark frame centered on pages 5-10.
static void draw_spark(uint8_t frame) {
    for (uint8_t p = 0; p < 6; p++) {
        uint16_t base = (uint16_t)(5 + p) * P_WIDTH;
        for (uint8_t x = 0; x < P_WIDTH; x++)
            oled_write_raw_byte(pgm_read_byte(&spark_frames[frame][p * P_WIDTH + x]), base + x);
    }
}

bool oled_task_user(void) {
    bool stale = !hid_seen || timer_elapsed32(last_hid_time) > HOST_STALE_MS;
    uint8_t status = stale ? ST_IDLE : claude_status;

    // Repaint only when the scene changes; animations key off frame counters.
    static uint32_t shown_key = 0xFFFFFFFF;
    uint32_t        key;

    if (status == ST_WORKING) {
        // Pulsing Claude spark, 32x48 centered (pages 5-10): 0-1-2-3-2-1 loop.
        uint8_t t     = (timer_read32() / 150) % 6;
        uint8_t frame = t < 4 ? t : 6 - t;
        key           = 0x10000UL | frame;
        if (key != shown_key) {
            shown_key = key;
            oled_clear();
            draw_spark(frame);
        }
    } else if (status == ST_WAITING) {
        // Full-screen "INPUT" in 18x24 caps, blinking 600ms on / 300ms off.
        bool on = (timer_read32() % 900) < 600;
        key     = 0x20000UL | on;
        if (key != shown_key) {
            shown_key = key;
            oled_clear();
            if (on) {
                static const char input[5] = {'I', 'N', 'P', 'U', 'T'};
                for (uint8_t k = 0; k < 5; k++)
                    draw_scaled_char(input[k], k * 3, 3);
            }
        }
    } else if (status == ST_ERROR) {
        key = 0x30000UL;
        if (key != shown_key) {
            shown_key = key;
            oled_clear();
            static const char err[5] = {'E', 'R', 'R', 'O', 'R'};
            for (uint8_t k = 0; k < 5; k++)
                draw_scaled_char(err[k], k * 3, 3);
        }
    } else if (!stale && usage_5h <= 100 && ((timer_read32() / 5000) & 1)) {
        // Idle, alternate phase: current-session (5h window) usage bar.
        key = 0x50000UL | usage_5h;
        if (key != shown_key) {
            shown_key = key;
            oled_clear();
            draw_usage_col("5H", usage_5h, 0, P_WIDTH);
        }
    } else if (stale) {
        // No host signal: static Claude spark as the standby screen.
        key = 0x60000UL;
        if (key != shown_key) {
            shown_key = key;
            oled_clear();
            draw_spark(3); // full-size frame, no pulse
        }
    } else {
        // Idle: model name fills the screen — stacked letters and version
        // digits all at 12x16, centered vertically.
        char letters[7], version[6];
        uint8_t li = 0, vi = 0;
        bool    in_ver = false;
        for (uint8_t i = 0; model_str[i]; i++) {
            char c = model_str[i];
            if (c == ' ') continue;
            if (!in_ver && c >= '0' && c <= '9') in_ver = true;
            if (!in_ver) {
                if (li < 6) letters[li++] = c;
            } else if (vi < 5) {
                version[vi++] = c;
            }
        }
        letters[li] = '\0';
        version[vi] = '\0';
        if (!letters[0]) {
            strcpy(letters, "CLAUDE");
            li         = 6;
            version[0] = '\0';
            vi         = 0;
        }
        uint8_t ver_pages = 0;
        for (uint8_t i = 0; version[i]; i++) ver_pages += version[i] == '.' ? 1 : 2;
        // Simple hash of the text so model changes repaint.
        uint32_t h = 0x40000UL;
        for (uint8_t i = 0; letters[i]; i++) h = h * 31 + letters[i];
        for (uint8_t i = 0; version[i]; i++) h = h * 31 + version[i];
        key = h;
        if (key != shown_key) {
            shown_key = key;
            oled_clear();
            uint8_t total = li * 2 + ver_pages;
            uint8_t page  = total <= 16 ? (16 - total) / 2 : 0;
            for (uint8_t k = 0; letters[k] && page + 2 <= 16; k++, page += 2)
                draw_scaled_char(letters[k], page, 2);
            for (uint8_t k = 0; version[k]; k++) {
                if (version[k] == '.') {
                    if (page + 1 > 16) break;
                    draw_dot(page);
                    page += 1;
                } else {
                    if (page + 2 > 16) break;
                    draw_scaled_char(version[k], page, 2);
                    page += 2;
                }
            }
        }
    }
    return false;
}
#endif

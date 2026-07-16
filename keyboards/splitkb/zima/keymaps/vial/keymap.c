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
            rgblight_sethsv_noeeprom(21, 255, 150); // orange
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

// 12x16 pixel-doubled + bolded rendering: two big lines on the 128x32 panel.
#    define BIG_COLS 10 // 128 / 12

// Vertically stretch an 8-bit font column into 16 bits (each pixel doubled).
static uint16_t stretch_col(uint8_t b) {
    uint16_t r = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (b & (1 << i)) r |= (uint16_t)3 << (i * 2);
    }
    return r;
}

// Draw s (uppercased, max 10 chars) centered on OLED pages `page` and page+1.
static void draw_big_line(const char *s, uint8_t page) {
    uint8_t len = 0;
    while (s[len] && len < BIG_COLS)
        len++;
    uint8_t  x0    = (OLED_DISPLAY_WIDTH - len * 12) / 2;
    uint16_t base  = (uint16_t)page * OLED_DISPLAY_WIDTH;
    uint16_t prev  = 0;
    for (uint8_t x = 0; x < OLED_DISPLAY_WIDTH; x++) {
        uint16_t col = 0;
        if (x >= x0 && x < x0 + len * 12) {
            uint8_t rel = x - x0;
            char    c   = s[rel / 12];
            if (c >= 'a' && c <= 'z') c -= 0x20;
            if (c < 0x20 || c > 0x5F) c = '?';
            uint8_t fb = pgm_read_byte(&big_font[(c - 0x20) * 6 + (rel % 12) / 2]);
            col        = stretch_col(fb);
        }
        uint16_t out = col | prev; // horizontal smear = bold
        prev         = col;
        oled_write_raw_byte(out & 0xFF, base + x);
        oled_write_raw_byte(out >> 8, base + OLED_DISPLAY_WIDTH + x);
    }
}

bool oled_task_user(void) {
    static const char spinner[4] = {'|', '/', '-', '\\'};
    char              top[BIG_COLS + 1];
    char              bottom[BIG_COLS + 1];

    if (!hid_seen || timer_elapsed32(last_hid_time) > HOST_STALE_MS) {
        strcpy(top, "CLAUDE");
        strcpy(bottom, "NO HOST");
    } else {
        memcpy(top, model_str, BIG_COLS);
        top[BIG_COLS] = '\0';
        if (!top[0]) strcpy(top, "CLAUDE");
        switch (claude_status) {
            case ST_WORKING:
                strcpy(bottom, "WORKING ");
                bottom[8] = spinner[(timer_read32() / 250) & 3];
                bottom[9] = '\0';
                break;
            case ST_WAITING:
                // Blink for attention: text 600ms, blank 300ms.
                strcpy(bottom, (timer_read32() % 900) < 600 ? "NEED INPUT" : "");
                break;
            case ST_ERROR:
                strcpy(bottom, "! ERROR !");
                break;
            default:
                strcpy(bottom, "idle");
                break;
        }
    }

    // Only repaint when content changed: full-buffer writes are expensive.
    static char shown[2 * (BIG_COLS + 1)];
    char        now[2 * (BIG_COLS + 1)];
    memcpy(now, top, BIG_COLS + 1);
    memcpy(now + BIG_COLS + 1, bottom, BIG_COLS + 1);
    if (memcmp(now, shown, sizeof(now)) != 0) {
        memcpy(shown, now, sizeof(now));
        draw_big_line(top, 0);
        draw_big_line(bottom, 2);
    }
    return false;
}
#endif

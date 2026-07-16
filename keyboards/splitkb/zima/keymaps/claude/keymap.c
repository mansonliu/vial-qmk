/* Claude Code status display + control pad for splitkb Zima
 *
 * Host pushes state over Raw HID (usage page 0xFF60, usage 0x61, 32-byte
 * reports). Protocol, first byte = command:
 *   0x01  model name   — bytes 1..: NUL-terminated ASCII, max 21 chars
 *   0x02  agent status — byte 1: 0 idle / 1 working / 2 waiting / 3 error
 *   0x03  info line    — bytes 1..: NUL-terminated ASCII (cwd, context %, ...)
 * Firmware never talks back; keys are plain keystrokes into the focused app.
 */
#include QMK_KEYBOARD_H
#include "raw_hid.h"
#include <string.h>

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
    [1] = LAYOUT_ortho_4x3( /* Audio / firmware */
        QK_BOOT, _______, XXXXXXX,
        AU_ON,   AU_OFF,  XXXXXXX,
        CK_TOGG, XXXXXXX, CK_UP,
        CK_RST,  XXXXXXX, CK_DOWN
    ),
    [2] = LAYOUT_ortho_4x3( /* RGB / haptic */
        UG_TOGG, UG_NEXT, _______,
        UG_HUEU, UG_SATU, UG_VALU,
        UG_HUED, UG_SATD, UG_VALD,
        HF_TOGG, HF_FDBK, HF_CONT
    )
};

// Clockwise = down: matches walking through Claude Code menu options.
bool encoder_update_user(uint8_t index, bool clockwise) {
    tap_code(clockwise ? KC_DOWN : KC_UP);
    return false;
}

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

void raw_hid_receive(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case CMD_MODEL:
            copy_payload(model_str, data + 1, TEXT_COLS);
            break;
        case CMD_STATUS: {
            uint8_t new_status = data[1] <= ST_ERROR ? data[1] : ST_ERROR;
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
            copy_payload(info_str, data + 1, TEXT_COLS);
            break;
        default:
            return;
    }
    last_hid_time = timer_read32();
    hid_seen      = true;
    oled_on();
}

#ifdef OLED_ENABLE
static void write_padded_ln(const char *s) {
    uint8_t len = 0;
    for (; s[len] && len < TEXT_COLS; len++) {
        oled_write_char(s[len], false);
    }
    for (; len < TEXT_COLS; len++) {
        oled_write_char(' ', false);
    }
}

bool oled_task_user(void) {
    if (!hid_seen || timer_elapsed32(last_hid_time) > HOST_STALE_MS) {
        write_padded_ln("Claude Code");
        write_padded_ln("");
        write_padded_ln("waiting for host...");
        char layer_line[TEXT_COLS + 1];
        snprintf(layer_line, sizeof(layer_line), "layer %d", get_highest_layer(layer_state));
        write_padded_ln(layer_line);
        return false;
    }

    char line[TEXT_COLS + 1];
    snprintf(line, sizeof(line), "Claude Code   L%d", get_highest_layer(layer_state));
    write_padded_ln(line);
    write_padded_ln(model_str[0] ? model_str : "(no model)");

    static const char spinner[4] = {'|', '/', '-', '\\'};
    switch (claude_status) {
        case ST_WORKING:
            snprintf(line, sizeof(line), "working %c", spinner[(timer_read32() / 250) & 3]);
            break;
        case ST_WAITING:
            snprintf(line, sizeof(line), ">> NEEDS INPUT <<");
            break;
        case ST_ERROR:
            snprintf(line, sizeof(line), "!! ERROR !!");
            break;
        default:
            snprintf(line, sizeof(line), "idle");
            break;
    }
    write_padded_ln(line);
    write_padded_ln(info_str);
    return false;
}
#endif

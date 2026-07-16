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

/* Copyright 2020 KiwiKeebs
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include QMK_KEYBOARD_H

enum layer_names {
    _BASE,
    _FN1,
    _FN2,
    _FN3
};

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [_BASE] = LAYOUT(
        KC_DEL,  KC_HOME, LT(_FN1,KC_END),
        KC_MPRV, KC_MPLY, KC_MNXT, KC_MUTE
    ),
    [_FN1] = LAYOUT(
        QK_BOOT, KC_UP,   _______,
        KC_LEFT, KC_DOWN, KC_RGHT, NK_TOGG
    ),
    [_FN2] = LAYOUT(
        _______, _______, _______,
        _______, _______, _______, _______
    ),
    [_FN3] = LAYOUT(
        _______, _______, _______,
        _______, _______, _______, _______
    )
};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [_BASE] = { ENCODER_CCW_CW(KC_VOLD, KC_VOLU) },
    [_FN1]  = { ENCODER_CCW_CW(_______, _______) },
    [_FN2]  = { ENCODER_CCW_CW(_______, _______) },
    [_FN3]  = { ENCODER_CCW_CW(_______, _______) },
};
#endif

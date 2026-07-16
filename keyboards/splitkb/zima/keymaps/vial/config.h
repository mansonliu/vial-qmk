#pragma once

#define VIAL_KEYBOARD_UID {0xDB, 0x5A, 0x56, 0x20, 0x2D, 0x2B, 0xEF, 0xD1}

// Unlock: hold Esc (row1,col0) + bottom-right key (row3,col2)
#define VIAL_UNLOCK_COMBO_ROWS {1, 3}
#define VIAL_UNLOCK_COMBO_COLS {0, 2}

#define DYNAMIC_KEYMAP_LAYER_COUNT 4

// Screen stays on longer than default 60s; host pushes re-wake it anyway.
#define OLED_TIMEOUT 180000

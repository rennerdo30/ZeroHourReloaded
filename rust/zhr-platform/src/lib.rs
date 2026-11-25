//! Cross-platform SDL3 windowing and input translation helpers for ZeroHourReloaded.
//!
//! Maps USB HID / `SDL_Scancode` values directly to Zero Hour's legacy `KeyDefType`
//! (DirectInput scancode byte values defined in `GameClient/KeyDefs.h`), enabling
//! `SDL3Keyboard` to replace Windows-only `DirectInput8` on Windows, Linux, and macOS.

/// Maps an `SDL_Scancode` (USB HID Usage Page 0x07) to a Zero Hour `KeyDefType` byte (`DIK_*`).
/// Returns `0x00` (`KEY_NONE`) for unmapped scancodes.
pub const fn sdl_scancode_to_zh_key(scancode: u32) -> u8 {
    match scancode {
        // Letters A..Z (SDL_SCANCODE_A = 4 .. SDL_SCANCODE_Z = 29)
        4 => 0x1E,  // A -> DIK_A
        5 => 0x30,  // B -> DIK_B
        6 => 0x2E,  // C -> DIK_C
        7 => 0x20,  // D -> DIK_D
        8 => 0x12,  // E -> DIK_E
        9 => 0x21,  // F -> DIK_F
        10 => 0x22, // G -> DIK_G
        11 => 0x23, // H -> DIK_H
        12 => 0x17, // I -> DIK_I
        13 => 0x24, // J -> DIK_J
        14 => 0x25, // K -> DIK_K
        15 => 0x26, // L -> DIK_L
        16 => 0x32, // M -> DIK_M
        17 => 0x31, // N -> DIK_N
        18 => 0x18, // O -> DIK_O
        19 => 0x19, // P -> DIK_P
        20 => 0x10, // Q -> DIK_Q
        21 => 0x13, // R -> DIK_R
        22 => 0x1F, // S -> DIK_S
        23 => 0x14, // T -> DIK_T
        24 => 0x16, // U -> DIK_U
        25 => 0x2F, // V -> DIK_V
        26 => 0x11, // W -> DIK_W
        27 => 0x2D, // X -> DIK_X
        28 => 0x15, // Y -> DIK_Y
        29 => 0x2C, // Z -> DIK_Z

        // Top row digits 1..9, 0 (SDL_SCANCODE_1 = 30 .. SDL_SCANCODE_0 = 39)
        30 => 0x02, // 1
        31 => 0x03, // 2
        32 => 0x04, // 3
        33 => 0x05, // 4
        34 => 0x06, // 5
        35 => 0x07, // 6
        36 => 0x08, // 7
        37 => 0x09, // 8
        38 => 0x0A, // 9
        39 => 0x0B, // 0

        // Control & punctuation keys
        40 => 0x1C, // RETURN -> DIK_RETURN
        41 => 0x01, // ESCAPE -> DIK_ESCAPE
        42 => 0x0E, // BACKSPACE -> DIK_BACK
        43 => 0x0F, // TAB -> DIK_TAB
        44 => 0x39, // SPACE -> DIK_SPACE
        45 => 0x0C, // MINUS -> DIK_MINUS
        46 => 0x0D, // EQUALS -> DIK_EQUALS
        47 => 0x1A, // LEFTBRACKET -> DIK_LBRACKET
        48 => 0x1B, // RIGHTBRACKET -> DIK_RBRACKET
        49 => 0x2B, // BACKSLASH -> DIK_BACKSLASH
        50 => 0x2B, // NONUSHASH -> DIK_BACKSLASH
        51 => 0x27, // SEMICOLON -> DIK_SEMICOLON
        52 => 0x28, // APOSTROPHE -> DIK_APOSTROPHE
        53 => 0x29, // GRAVE -> DIK_GRAVE
        54 => 0x33, // COMMA -> DIK_COMMA
        55 => 0x34, // PERIOD -> DIK_PERIOD
        56 => 0x35, // SLASH -> DIK_SLASH
        57 => 0x3A, // CAPSLOCK -> DIK_CAPSLOCK

        // Function keys F1..F12 (SDL_SCANCODE_F1 = 58 .. SDL_SCANCODE_F12 = 69)
        58 => 0x3B, // F1
        59 => 0x3C, // F2
        60 => 0x3D, // F3
        61 => 0x3E, // F4
        62 => 0x3F, // F5
        63 => 0x40, // F6
        64 => 0x41, // F7
        65 => 0x42, // F8
        66 => 0x43, // F9
        67 => 0x44, // F10
        68 => 0x57, // F11
        69 => 0x58, // F12

        // Navigation & system keys
        70 => 0xB7, // PRINTSCREEN -> DIK_SYSRQ
        71 => 0x46, // SCROLLLOCK -> DIK_SCROLL
        72 => 0xC5, // PAUSE -> DIK_PAUSE
        73 => 0xD2, // INSERT -> DIK_INSERT
        74 => 0xC7, // HOME -> DIK_HOME
        75 => 0xC9, // PAGEUP -> DIK_PGUP
        76 => 0xD3, // DELETE -> DIK_DELETE
        77 => 0xCF, // END -> DIK_END
        78 => 0xD1, // PAGEDOWN -> DIK_PGDN
        79 => 0xCD, // RIGHT -> DIK_RIGHTARROW
        80 => 0xCB, // LEFT -> DIK_LEFTARROW
        81 => 0xD0, // DOWN -> DIK_DOWNARROW
        82 => 0xC8, // UP -> DIK_UPARROW

        // Numpad
        83 => 0x45,  // NUMLOCKCLEAR -> DIK_NUMLOCK
        84 => 0xB5,  // KP_DIVIDE -> DIK_NUMPADSLASH
        85 => 0x37,  // KP_MULTIPLY -> DIK_NUMPADSTAR
        86 => 0x4A,  // KP_MINUS -> DIK_NUMPADMINUS
        87 => 0x4E,  // KP_PLUS -> DIK_NUMPADPLUS
        88 => 0x9C,  // KP_ENTER -> DIK_NUMPADENTER
        89 => 0x4F,  // KP_1 -> DIK_NUMPAD1
        90 => 0x50,  // KP_2 -> DIK_NUMPAD2
        91 => 0x51,  // KP_3 -> DIK_NUMPAD3
        92 => 0x4B,  // KP_4 -> DIK_NUMPAD4
        93 => 0x4C,  // KP_5 -> DIK_NUMPAD5
        94 => 0x4D,  // KP_6 -> DIK_NUMPAD6
        95 => 0x47,  // KP_7 -> DIK_NUMPAD7
        96 => 0x48,  // KP_8 -> DIK_NUMPAD8
        97 => 0x49,  // KP_9 -> DIK_NUMPAD9
        98 => 0x52,  // KP_0 -> DIK_NUMPAD0
        99 => 0x53,  // KP_PERIOD -> DIK_NUMPADPERIOD
        100 => 0x56, // NONUSBACKSLASH -> DIK_OEM_102

        // Modifiers (SDL_SCANCODE_LCTRL = 224 .. SDL_SCANCODE_RALT = 230)
        224 => 0x1D, // LCTRL -> DIK_LCONTROL
        225 => 0x2A, // LSHIFT -> DIK_LSHIFT
        226 => 0x38, // LALT -> DIK_LALT
        228 => 0x9D, // RCTRL -> DIK_RCONTROL
        229 => 0x36, // RSHIFT -> DIK_RSHIFT
        230 => 0xB8, // RALT -> DIK_RALT

        _ => 0x00,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_core_rts_keys_and_hotkeys_accurately() {
        assert_eq!(sdl_scancode_to_zh_key(41), 0x01); // ESC
        assert_eq!(sdl_scancode_to_zh_key(4), 0x1E); // A (Attack-move)
        assert_eq!(sdl_scancode_to_zh_key(22), 0x1F); // S (Stop)
        assert_eq!(sdl_scancode_to_zh_key(10), 0x22); // G (Guard)
        assert_eq!(sdl_scancode_to_zh_key(44), 0x39); // SPACE (Jump to event)
        assert_eq!(sdl_scancode_to_zh_key(40), 0x1C); // ENTER
        assert_eq!(sdl_scancode_to_zh_key(66), 0x43); // F9
        assert_eq!(sdl_scancode_to_zh_key(68), 0x57); // F11
        assert_eq!(sdl_scancode_to_zh_key(69), 0x58); // F12
        assert_eq!(sdl_scancode_to_zh_key(72), 0xC5); // PAUSE
        assert_eq!(sdl_scancode_to_zh_key(82), 0xC8); // UP
        assert_eq!(sdl_scancode_to_zh_key(224), 0x1D); // LCTRL
        assert_eq!(sdl_scancode_to_zh_key(225), 0x2A); // LSHIFT
        assert_eq!(sdl_scancode_to_zh_key(999), 0x00); // Unmapped -> KEY_NONE
    }
}

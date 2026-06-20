// mg.text -- pure, constexpr leaf utilities lifted from the C core (task C1).
//
// The modern form of chrdef.h's ISWORD/ISCTRL/... macros (which read the
// runtime-mutable global `char cinfo[256]` in cinfo.c) plus util.c's pure
// ntabstop(). Everything here is constexpr and state-free: no curwp/curbp, no
// global mutation, usable in constant expressions. Greenfield + isolated
// (ENABLE_CPP_UPGRADES); not yet wired into the C core.

module;
#include <array>
#include <cstdint>

export module mg.text;

namespace mg::text::detail {

// Class flag bits -- identical to chrdef.h's _MG_* so the table below can be
// transcribed verbatim from cinfo.c.
inline constexpr std::uint8_t W = 0x01; // word constituent
inline constexpr std::uint8_t U = 0x02; // upper-case letter
inline constexpr std::uint8_t L = 0x04; // lower-case letter
inline constexpr std::uint8_t C = 0x08; // control
inline constexpr std::uint8_t P = 0x10; // end-of-sentence punctuation
inline constexpr std::uint8_t D = 0x20; // decimal digit

// Character-class table, indexed by byte. Transcribed BYTE-FOR-BYTE from
// cinfo.c (same 4-per-row layout) so it can be diffed side by side. Quirks of
// the original DEC-multinational table are preserved deliberately: '_' is not a
// word char by default; 0xD7 (x) / 0xF7 (/) are marked word/letter; 0xD0 / 0xDE
// (Eth/Thorn) are not. The ctags '_'-toggle hack stays in C (out of scope).
inline constexpr std::array<std::uint8_t, 256> cinfo = {{
    C, C, C, C,                            /* 0x0X */
    C, C, C, C,
    C, C, C, C,
    C, C, C, C,
    C, C, C, C,                            /* 0x1X */
    C, C, C, C,
    C, C, C, C,
    C, C, C, C,
    0, P, 0, 0,                            /* 0x2X */
    W, W, 0, W,
    0, 0, 0, 0,
    0, 0, P, 0,
    D | W, D | W, D | W, D | W,            /* 0x3X */
    D | W, D | W, D | W, D | W,
    D | W, D | W, 0, 0,
    0, 0, 0, P,
    0, U | W, U | W, U | W,                /* 0x4X */
    U | W, U | W, U | W, U | W,
    U | W, U | W, U | W, U | W,
    U | W, U | W, U | W, U | W,
    U | W, U | W, U | W, U | W,            /* 0x5X */
    U | W, U | W, U | W, U | W,
    U | W, U | W, U | W, 0,
    0, 0, 0, 0,
    0, L | W, L | W, L | W,                /* 0x6X */
    L | W, L | W, L | W, L | W,
    L | W, L | W, L | W, L | W,
    L | W, L | W, L | W, L | W,
    L | W, L | W, L | W, L | W,            /* 0x7X */
    L | W, L | W, L | W, L | W,
    L | W, L | W, L | W, 0,
    0, 0, 0, C,
    0, 0, 0, 0,                            /* 0x8X */
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,                            /* 0x9X */
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,                            /* 0xAX */
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,                            /* 0xBX */
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    U | W, U | W, U | W, U | W,            /* 0xCX */
    U | W, U | W, U | W, U | W,
    U | W, U | W, U | W, U | W,
    U | W, U | W, U | W, U | W,
    0, U | W, U | W, U | W,                /* 0xDX */
    U | W, U | W, U | W, U | W,
    U | W, U | W, U | W, U | W,
    U | W, U | W, 0, W,
    L | W, L | W, L | W, L | W,            /* 0xEX */
    L | W, L | W, L | W, L | W,
    L | W, L | W, L | W, L | W,
    L | W, L | W, L | W, L | W,
    0, L | W, L | W, L | W,                /* 0xFX */
    L | W, L | W, L | W, L | W,
    L | W, L | W, L | W, L | W,
    L | W, L | W, 0, 0,
}};

// CHARMASK: read the table at the unsigned-byte index (matches chrdef.h, which
// casts to unsigned char so sign-extended chars index correctly).
constexpr std::uint8_t info(int c)
{
    return cinfo[static_cast<unsigned char>(c)];
}

} // namespace mg::text::detail

export namespace mg::text {

constexpr bool is_word(int c)  { return (detail::info(c) & detail::W) != 0; }
constexpr bool is_ctrl(int c)  { return (detail::info(c) & detail::C) != 0; }
constexpr bool is_upper(int c) { return (detail::info(c) & detail::U) != 0; }
constexpr bool is_lower(int c) { return (detail::info(c) & detail::L) != 0; }
constexpr bool is_eosp(int c)  { return (detail::info(c) & detail::P) != 0; }
constexpr bool is_digit(int c) { return (detail::info(c) & detail::D) != 0; }

// Column of the next tab stop after `col` for a tab width of `tabw` (ntabstop).
constexpr int next_tabstop(int col, int tabw)
{
    return ((col + tabw) / tabw) * tabw;
}

} // namespace mg::text

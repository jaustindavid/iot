"""
Bitmap font renderer — megafont 5x6, ported from the C++ bitmasks.
"""
from __future__ import annotations

# Each glyph is 6 rows of bitmasks, MSB-first, 5 columns wide.
# Bit 7 (0x80) = leftmost column, bit 3 (0x08) = rightmost.
MEGAFONT_5X6: dict[str, list[int]] = {
    "0": [0x70, 0x98, 0x98, 0x98, 0x98, 0x70],
    "1": [0x30, 0x70, 0x30, 0x30, 0x30, 0x78],
    "2": [0xF0, 0x18, 0x70, 0xC0, 0xC0, 0xF8],
    "3": [0xF8, 0x18, 0x70, 0x18, 0x98, 0x70],
    "4": [0x80, 0x98, 0xF8, 0x18, 0x18, 0x18],
    "5": [0xF8, 0xC0, 0xF0, 0x18, 0x98, 0x70],
    "6": [0x70, 0xC0, 0xF0, 0xC8, 0xC8, 0x70],
    "7": [0xF8, 0x18, 0x30, 0x60, 0x60, 0x60],
    "8": [0x70, 0x98, 0x70, 0x98, 0x98, 0x70],
    "9": [0x70, 0x98, 0x78, 0x18, 0x98, 0x70],
    ":": [0x00, 0x18, 0x00, 0x00, 0x18, 0x00],
}


def render_glyph(char: str, x_offset: int, y_offset: int,
                 grid_w: int, grid_h: int) -> set[tuple[int, int]]:
    """Render a single character, return set of (x, y) positions that are lit."""
    bitmaps = MEGAFONT_5X6.get(char)
    if bitmaps is None:
        return set()
    pixels: set[tuple[int, int]] = set()
    for row, bits in enumerate(bitmaps):
        for col in range(5):
            if bits & (0x80 >> col):
                px = x_offset + col
                py = y_offset + row
                if 0 <= px < grid_w and 0 <= py < grid_h:
                    pixels.add((px, py))
    return pixels


def render_time(hour: int, minute: int, grid_w: int, grid_h: int,
                layout: str = "horizontal") -> set[tuple[int, int]]:
    """Render HH:MM into a set of lit pixel positions."""
    h_str = f"{hour:02d}"
    m_str = f"{minute:02d}"
    pixels: set[tuple[int, int]] = set()

    if layout == "horizontal":
        # Wide layout (32x8): digits at x=4,10,17,23 with y_offset=1
        pixels |= render_glyph(h_str[0], 4, 1, grid_w, grid_h)
        pixels |= render_glyph(h_str[1], 10, 1, grid_w, grid_h)
        pixels |= render_glyph(m_str[0], 17, 1, grid_w, grid_h)
        pixels |= render_glyph(m_str[1], 23, 1, grid_w, grid_h)
    elif layout == "grid":
        # Square layout (16x16): 2x2 grid
        pixels |= render_glyph(h_str[0], 2, 1, grid_w, grid_h)
        pixels |= render_glyph(h_str[1], 9, 1, grid_w, grid_h)
        pixels |= render_glyph(m_str[0], 2, 8, grid_w, grid_h)
        pixels |= render_glyph(m_str[1], 9, 8, grid_w, grid_h)

    return pixels

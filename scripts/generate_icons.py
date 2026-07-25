#!/usr/bin/env python3
"""Generate NekoDrag pixel-art SVG and ICO files with the standard library."""

from pathlib import Path
import struct


TRANSPARENT = (0x00, 0x00, 0x00, 0x00)
PALETTE = {
    "O": (0x2F, 0x25, 0x29, 0xFF),  # outline   #2F2529
    "C": (0xF4, 0xE6, 0xC7, 0xFF),  # cream    #F4E6C7
    "S": (0x80, 0x64, 0x5A, 0xFF),  # seal     #80645A
    "B": (0x55, 0xC7, 0xF3, 0xFF),  # blue eye #55C7F3
    "H": (0xFF, 0xF9, 0xEC, 0xFF),  # highlight #FFF9EC
    "P": (0xD7, 0x8E, 0x98, 0xFF),  # nose     #D78E98
}

# The single 16x16 source of truth for every generated asset.
PIXEL_ART = (
    "................",
    ".OO..........OO.",
    ".OSO........OSO.",
    ".OSSO......OSSO.",
    ".OSSSOOOOOOSSSO.",
    ".OSSSCCCCCCSSSO.",
    ".OCCSSSSSSSSCCO.",
    ".OCSSHB..BHSSCO.",
    ".OCSSBBSSBBSSCO.",
    ".OCCSSSSSSSSCCO.",
    ".OCCCCSPPSCCCCO.",
    ".OCCCCCSSCCCCCO.",
    ".OOCCCCCCCCCCOO.",
    "..OOCCCCCCCCOO..",
    "....OOOOOOOO....",
    "................",
)


def validate_pixel_art():
    """Fail fast if the canonical matrix or palette is inconsistent."""
    if len(PIXEL_ART) != 16 or any(len(row) != 16 for row in PIXEL_ART):
        raise ValueError("PIXEL_ART must be exactly 16x16")
    unknown = set("".join(PIXEL_ART)) - set(PALETTE) - {"."}
    if unknown:
        raise ValueError(f"PIXEL_ART uses unknown symbols: {sorted(unknown)}")


def render_icon(size):
    """Render a square BGRA bitmap using nearest-neighbor pixel mapping."""
    pixels = bytearray(size * size * 4)
    for y in range(size):
        source_y = y * 16 // size
        for x in range(size):
            source_x = x * 16 // size
            rgba = PALETTE.get(PIXEL_ART[source_y][source_x], TRANSPARENT)
            r, g, b, a = rgba
            offset = (y * size + x) * 4
            pixels[offset:offset + 4] = bytes((b, g, r, a))
    return pixels


def build_bmp_data(width, height, bgra_pixels):
    """Build BITMAPINFOHEADER + XOR mask + AND mask for a 32bpp icon."""
    xor_size = width * height * 4
    row_stride = ((width + 31) // 32) * 4
    and_size = row_stride * height

    header = struct.pack(
        "<IiiHHIIiiII",
        40,             # biSize
        width,          # biWidth
        height * 2,     # biHeight (XOR + AND)
        1,              # biPlanes
        32,             # biBitCount
        0,              # biCompression
        0,              # biSizeImage
        0,              # biXPelsPerMeter
        0,              # biYPelsPerMeter
        0,              # biClrUsed
        0,              # biClrImportant
    )

    # ICO BMP rows are bottom-up.
    xor = bytearray(xor_size)
    for row in range(height):
        source_offset = (height - 1 - row) * width * 4
        target_offset = row * width * 4
        xor[target_offset:target_offset + width * 4] = (
            bgra_pixels[source_offset:source_offset + width * 4]
        )

    # AND mask: 1bpp, 1 = transparent, 0 = opaque.
    and_mask = bytearray(and_size)
    for row in range(height):
        source_row = height - 1 - row
        for column in range(width):
            alpha = bgra_pixels[(source_row * width + column) * 4 + 3]
            if alpha < 128:
                byte_index = row * row_stride + column // 8
                bit = 7 - (column % 8)
                and_mask[byte_index] |= 1 << bit

    return header + xor + and_mask


def write_ico(path, sizes):
    """Write an ICO containing one nearest-neighbor bitmap per size."""
    images = []
    for size in sizes:
        bitmap = build_bmp_data(size, size, render_icon(size))
        images.append((size, bitmap))

    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = bytearray()
    data = bytearray()
    for size, bitmap in images:
        entries += struct.pack(
            "<BBBBHHII",
            size if size < 256 else 0,
            size if size < 256 else 0,
            0,
            0,
            1,
            32,
            len(bitmap),
            offset,
        )
        data += bitmap
        offset += len(bitmap)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + entries + data)


def write_svg(path):
    """Write a crisp SVG composed only of canonical pixel rectangles."""
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256"',
        '     viewBox="0 0 16 16" shape-rendering="crispEdges">',
    ]
    for y, row in enumerate(PIXEL_ART):
        for x, symbol in enumerate(row):
            if symbol == ".":
                continue
            r, g, b, _ = PALETTE[symbol]
            lines.append(
                f'  <rect x="{x}" y="{y}" width="1" height="1" '
                f'fill="#{r:02X}{g:02X}{b:02X}"/>'
            )
    lines.append("</svg>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    validate_pixel_art()
    root = Path(__file__).resolve().parent.parent
    svg = root / "assets" / "nekodrag.svg"
    app_ico = root / "src" / "assets" / "app.ico"
    tray_ico = root / "src" / "assets" / "tray.ico"

    write_svg(svg)
    write_ico(app_ico, (16, 20, 24, 32, 48, 256))
    write_ico(tray_ico, (16, 20, 24))
    print(f"Generated {svg}")
    print(f"Generated {app_ico}")
    print(f"Generated {tray_ico}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate SuperDrag icon ICO files using only the Python standard library."""

import os
import struct

ACCENT = (0x00, 0x78, 0xD4)  # #0078D4 as (B, G, R)
WHITE = (0xFF, 0xFF, 0xFF)
TRANSPARENT = (0x00, 0x00, 0x00, 0x00)


def inside_rounded_rect(u, v, margin, radius):
    """Return True if (u,v) is inside an axis-aligned rounded rectangle."""
    if u < margin or u > 1.0 - margin or v < margin or v > 1.0 - margin:
        return False
    # Distance from the nearest straight edge before rounding
    dx = min(u - margin, 1.0 - margin - u)
    dy = min(v - margin, 1.0 - margin - v)
    if dx >= radius and dy >= radius:
        return True
    # Corner center
    cx = margin + radius if u < 0.5 else 1.0 - margin - radius
    cy = margin + radius if v < 0.5 else 1.0 - margin - radius
    return (u - cx) ** 2 + (v - cy) ** 2 <= radius * radius


def inside_plus(u, v):
    """Return True if (u,v) is inside the white plus/move symbol."""
    vertical = abs(u - 0.5) <= 0.07 and 0.28 <= v <= 0.72
    horizontal = abs(v - 0.5) <= 0.07 and 0.28 <= u <= 0.72
    return vertical or horizontal


def render_reference(size):
    """Render a high-resolution reference image and downscale to size."""
    ref = 512
    ref_pixels = bytearray(ref * ref * 4)

    for y in range(ref):
        for x in range(ref):
            u = (x + 0.5) / ref
            v = (y + 0.5) / ref
            idx = (y * ref + x) * 4
            if inside_rounded_rect(u, v, 0.05, 0.16):
                ref_pixels[idx:idx + 4] = ACCENT + (0xFF,)
                if inside_plus(u, v):
                    ref_pixels[idx:idx + 4] = WHITE + (0xFF,)
            else:
                ref_pixels[idx:idx + 4] = TRANSPARENT

    # Downsample by averaging blocks
    scale = ref // size
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            sx0, sx1 = x * scale, (x + 1) * scale
            sy0, sy1 = y * scale, (y + 1) * scale
            b = g = r = a = 0
            count = 0
            for sy in range(sy0, sy1):
                for sx in range(sx0, sx1):
                    idx = (sy * ref + sx) * 4
                    b += ref_pixels[idx]
                    g += ref_pixels[idx + 1]
                    r += ref_pixels[idx + 2]
                    a += ref_pixels[idx + 3]
                    count += 1
            idx = (y * size + x) * 4
            out[idx] = b // count
            out[idx + 1] = g // count
            out[idx + 2] = r // count
            out[idx + 3] = a // count
    return out


def build_bmp_data(width, height, bgra_pixels):
    """Build BITMAPINFOHEADER + XOR mask + AND mask for a 32bpp icon image."""
    xor_size = width * height * 4
    row_stride = ((width + 31) // 32) * 4
    and_size = row_stride * height

    header = struct.pack(
        '<IiiHHIIiiII',
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

    # XOR mask is bottom-up BGRA
    xor = bytearray(xor_size)
    for row in range(height):
        src_offset = (height - 1 - row) * width * 4
        dst_offset = row * width * 4
        xor[dst_offset:dst_offset + width * 4] = bgra_pixels[src_offset:src_offset + width * 4]

    # AND mask: 1bpp, 1 = transparent, 0 = opaque. Use alpha channel.
    and_mask = bytearray(and_size)
    for row in range(height):
        src_row = height - 1 - row
        for col in range(width):
            alpha = bgra_pixels[(src_row * width + col) * 4 + 3]
            if alpha < 128:
                byte_index = row * row_stride + col // 8
                bit = 7 - (col % 8)
                and_mask[byte_index] |= (1 << bit)

    return header + xor + and_mask


def write_ico(path, sizes):
    """Write an ICO file containing the given image sizes."""
    images = []
    for size in sizes:
        bgra = render_reference(size)
        bmp = build_bmp_data(size, size, bgra)
        images.append((size, bmp))

    count = len(images)
    header = struct.pack('<HHH', 0, 1, count)
    offset = 6 + 16 * count
    entries = bytearray()
    data = bytearray()
    for size, bmp in images:
        entries += struct.pack(
            '<BBBBHHII',
            size if size < 256 else 0,  # width
            size if size < 256 else 0,  # height
            0,                          # colors
            0,                          # reserved
            1,                          # planes
            32,                         # bit count
            len(bmp),                   # bytes in res
            offset,                     # image offset
        )
        data += bmp
        offset += len(bmp)

    with open(path, 'wb') as f:
        f.write(header + entries + data)


if __name__ == '__main__':
    # script is in <repo>/scripts; repo root is one level up
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    app_ico = os.path.join(base, 'src', 'assets', 'app.ico')
    tray_ico = os.path.join(base, 'src', 'assets', 'tray.ico')
    write_ico(app_ico, [16, 20, 24, 32, 48, 256])
    write_ico(tray_ico, [16, 20, 24])
    print(f'Generated {app_ico}')
    print(f'Generated {tray_ico}')

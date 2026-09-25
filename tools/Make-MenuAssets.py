#!/usr/bin/env python3
"""Build the GoodHead main-menu background from a source image.

    python tools/Make-MenuAssets.py <image> [--out dist/menu]

Writes background01.vtf (1024x1024, for 4:3 screens) and
background01_widescreen.vtf (2048x1024, for 16:9 and 16:10), which is what the
Source engine hard-codes for the main menu background. The strings in engine.dll
are "materials/console/%s.vtf" and "materials/console/%s_widescreen.vtf", with
the name coming from scripts/ChapterBackgrounds.txt or, when that file is absent
as it is in GE:S, from the built-in "background01".

Both files are VTF 7.2, DXT1, one mip, NOMIP|NOLOD, with a DXT1 thumbnail: byte
for byte the same shape as the two GE:S ships, so the engine sees nothing new.

ASPECT. The engine stretches the texture across the whole screen, so the texture
has to be pre-distorted by the inverse of that stretch. A 2048x1024 (2.00)
texture on a 16:9 (1.778) screen is squeezed horizontally by 0.889, so a 16:9
source stretched by 1.125 going in comes out correct. The 4:3 file gets a 4:3
centre crop squashed into a square for the same reason.

Needs Pillow and numpy. Players never run this; the .vtf files are committed.
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

VTF_SIG = b"VTF\0"
IMAGE_FORMAT_DXT1 = 13
TEXTUREFLAGS_NOMIP = 0x0100
TEXTUREFLAGS_NOLOD = 0x0200


# --- DXT1 ------------------------------------------------------------------
# One 8-byte block per 4x4 pixels: two RGB565 endpoints followed by 16 two-bit
# indices into the four-colour palette they span. Emitting c0 > c1 selects the
# opaque mode, which is the only one used here.

def _pack565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)


def _unpack565(p):
    r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)],
                    axis=-1).astype(np.float32)


def _assign(blocks, a, b):
    """Nearest palette index for every pixel, and the palette it chose from."""
    pal = np.stack([a, b, (2 * a + b) / 3.0, (a + 2 * b) / 3.0], axis=1)   # (N,4,3)
    d = ((blocks[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)        # (N,16,4)
    return d.argmin(-1), pal


def encode_dxt1(rgb, refine=2):
    h, w, _ = rgb.shape
    assert h % 4 == 0 and w % 4 == 0
    blocks = (rgb.astype(np.float32)
              .reshape(h // 4, 4, w // 4, 4, 3)
              .transpose(0, 2, 1, 3, 4)
              .reshape(-1, 16, 3))

    # Endpoints along each block's principal axis. Better than the bounding-box
    # diagonal wherever a block holds two hues rather than one ramp, which on
    # this artwork is most of the red-against-black edges.
    mean = blocks.mean(axis=1, keepdims=True)
    x = blocks - mean
    v = blocks.max(axis=1) - blocks.min(axis=1)
    for _ in range(4):
        v = np.einsum('nij,ni->nj', x, np.einsum('nij,nj->ni', x, v))
        n = np.linalg.norm(v, axis=1, keepdims=True)
        v = np.where(n > 1e-6, v / np.maximum(n, 1e-6),
                     np.array([1.0, 0.0, 0.0], np.float32))
    t = np.einsum('nij,nj->ni', x, v)
    a = mean[:, 0, :] + v * t.max(1)[:, None]
    b = mean[:, 0, :] + v * t.min(1)[:, None]

    # Refit the endpoints to the indices they produced, least squares, twice.
    for _ in range(refine):
        idx, _ = _assign(blocks, a, b)
        wa = np.choose(idx, [1.0, 0.0, 2 / 3, 1 / 3]).astype(np.float32)
        wb = 1.0 - wa
        saa = (wa * wa).sum(1)
        sbb = (wb * wb).sum(1)
        sab = (wa * wb).sum(1)
        sac = np.einsum('ni,nij->nj', wa, blocks)
        sbc = np.einsum('ni,nij->nj', wb, blocks)
        det = saa * sbb - sab * sab
        ok = np.abs(det) > 1e-6
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)[:, None]
        na = (sbb[:, None] * sac - sab[:, None] * sbc) * inv
        nb = (saa[:, None] * sbc - sab[:, None] * sac) * inv
        a = np.where(ok[:, None], np.clip(na, 0, 255), a)
        b = np.where(ok[:, None], np.clip(nb, 0, 255), b)

    p0, p1 = _pack565(a), _pack565(b)
    swap = p0 < p1
    p0, p1 = np.where(swap, p1, p0), np.where(swap, p0, p1)
    a, b = _unpack565(p0), _unpack565(p1)
    idx, pal = _assign(blocks, a, b)
    # Equal endpoints mean a flat block; index 0 is that colour in either mode.
    idx = np.where((p0 == p1)[:, None], 0, idx)

    bits = (idx.astype(np.uint32) << (2 * np.arange(16, dtype=np.uint32))[None, :]).sum(1)
    out = np.empty((len(blocks), 8), np.uint8)
    out[:, 0] = p0 & 0xFF
    out[:, 1] = (p0 >> 8) & 0xFF
    out[:, 2] = p1 & 0xFF
    out[:, 3] = (p1 >> 8) & 0xFF
    for i in range(4):
        out[:, 4 + i] = (bits >> (8 * i)) & 0xFF

    err = ((np.take_along_axis(pal, idx[:, :, None], 1) - blocks) ** 2).mean()
    psnr = 10 * np.log10(255.0 ** 2 / err) if err > 0 else 99.0
    return out.tobytes(), psnr


# --- VTF -------------------------------------------------------------------

def vtf72(rgb, thumb_w, thumb_h):
    h, w, _ = rgb.shape
    hi, psnr = encode_dxt1(rgb)
    thumb = np.asarray(Image.fromarray(rgb).resize((thumb_w, thumb_h), Image.LANCZOS))
    lo, _ = encode_dxt1(thumb, refine=1)

    refl = (rgb.reshape(-1, 3).mean(0) / 255.0).astype(np.float32)
    hdr = bytearray(80)
    hdr[0:4] = VTF_SIG
    struct.pack_into('<II', hdr, 4, 7, 2)                  # version 7.2
    struct.pack_into('<I', hdr, 12, 80)                    # headerSize
    struct.pack_into('<HH', hdr, 16, w, h)
    struct.pack_into('<I', hdr, 20, TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD)
    struct.pack_into('<HH', hdr, 24, 1, 0)                 # frames, firstFrame
    struct.pack_into('<3f', hdr, 32, *refl)                # reflectivity
    struct.pack_into('<f', hdr, 48, 1.0)                   # bumpmap scale
    struct.pack_into('<I', hdr, 52, IMAGE_FORMAT_DXT1)
    hdr[56] = 1                                            # mipmapCount
    struct.pack_into('<I', hdr, 57, IMAGE_FORMAT_DXT1)     # lowResImageFormat
    hdr[61], hdr[62] = thumb_w, thumb_h
    struct.pack_into('<H', hdr, 63, 1)                     # depth
    return bytes(hdr) + lo + hi, psnr


def fit(img, w, h):
    """Centre-crop to the target ratio and resize. Never letterbox."""
    want = w / h
    if img.width / img.height > want:
        new = int(round(img.height * want))
        off = (img.width - new) // 2
        box = (off, 0, off + new, img.height)
    else:
        new = int(round(img.width / want))
        off = (img.height - new) // 2
        box = (0, off, img.width, off + new)
    return img.crop(box).resize((w, h), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--out', default=os.path.join('dist', 'menu'))
    ap.add_argument('--preview', action='store_true',
                    help='also write PNGs of what the screen will show')
    args = ap.parse_args()

    src = Image.open(args.image).convert('RGB')
    os.makedirs(args.out, exist_ok=True)

    # file name, texture size, aspect of the screen it is stretched onto, thumb
    jobs = [('background01.vtf', (1024, 1024), 4 / 3, (16, 16)),
            ('background01_widescreen.vtf', (2048, 1024), 16 / 9, (16, 8))]

    for name, (tw, th), screen, (lw, lh) in jobs:
        # Take the part of the picture that screen will show, then squeeze it
        # into the texture. The engine's stretch undoes exactly this squeeze.
        shown = fit(src, int(round(th * screen)), th)
        rgb = np.asarray(shown.resize((tw, th), Image.LANCZOS))
        data, psnr = vtf72(rgb, lw, lh)
        path = os.path.join(args.out, name)
        with open(path, 'wb') as f:
            f.write(data)
        print("  %s  %dx%d DXT1  %d bytes  PSNR %.1f dB" % (path, tw, th, len(data), psnr))
        if args.preview:
            shown.save(os.path.splitext(path)[0] + '_preview.png')

    return 0


if __name__ == '__main__':
    sys.exit(main())

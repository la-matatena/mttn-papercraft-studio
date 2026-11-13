"""Pure-stdlib RGBA PNG encoder and asset renderer for PaperCraft Studio Pro.

`render_thumbnail_png` draws the 104x58 Effects Library thumbnails packed
into the `.drfx` by `bundle_builder_lib.build_drfx_entries`; the output is
deterministic (no randomness), so rebuilt bundles are byte-identical.

PNG encoding (`encode_rgba_png`): an 8-byte signature followed by three
chunks, each `length (u32 BE) | type | data | CRC-32 of type+data`:

* `IHDR`: width, height, bit depth 8, color type 6 (RGBA), compression 0,
  filter method 0, interlace 0 (none).
* `IDAT`: zlib-compressed scanlines; every row is prefixed with filter
  byte 0 ("None"), which keeps the encoder trivial at a small size cost.
* `IEND`: empty terminator.

Pixel coordinates are integer pixels with the origin at the top-left and
y growing downward; colors are 0..255 sRGB bytes.
"""

import math
import struct
import zlib

# Thumbnail size in pixels; `drfx_validator.validate_drfx_bytes` rejects
# `.drfx` thumbnails of any other size.
THUMBNAIL_WIDTH = 104
THUMBNAIL_HEIGHT = 58


def _png_chunk(chunk_type: bytes, data_bytes: bytes) -> bytes:
  """Builds a single PNG chunk with length, type, payload, and CRC32.

  Args:
    chunk_type: Four ASCII bytes, e.g. `b"IHDR"`.
    data_bytes: Chunk payload.

  Returns:
    Serialized chunk; the CRC covers type + payload but not the length.
  """
  length_bytes = struct.pack(">I", len(data_bytes))
  crc_value = zlib.crc32(chunk_type + data_bytes) & 0xFFFFFFFF
  crc_bytes = struct.pack(">I", crc_value)
  return length_bytes + chunk_type + data_bytes + crc_bytes


def encode_rgba_png(width: int, height: int, rgba_bytes: bytes) -> bytes:
  """Encodes row-major RGBA pixels into a PNG byte buffer.

  Args:
    width: Image width in pixels.
    height: Image height in pixels.
    rgba_bytes: Tightly packed RGBA bytes of length width * height * 4.

  Returns:
    Complete PNG file bytes.

  Raises:
    ValueError: If dimensions are non-positive or byte length mismatches.
  """
  if width <= 0 or height <= 0:
    raise ValueError(f"PNG dimensions must be positive, got {width}x{height}.")
  expected_len = width * height * 4
  if len(rgba_bytes) != expected_len:
    raise ValueError(
        f"Expected {expected_len} RGBA bytes for {width}x{height}, "
        f"got {len(rgba_bytes)}."
    )

  stride = width * 4
  raw_scanlines = bytearray()
  for row_index in range(height):
    raw_scanlines.append(0)  # Filter type 0 (None) for this scanline.
    start = row_index * stride
    raw_scanlines.extend(rgba_bytes[start : start + stride])

  # >IIBBBBB: width, height, bit depth 8, color type 6 (truecolor + alpha),
  # compression 0 (deflate), filter method 0, interlace 0 (none).
  ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
  compressed_idat = zlib.compress(bytes(raw_scanlines), level=6)
  return b"".join([
      b"\x89PNG\r\n\x1a\n",
      _png_chunk(b"IHDR", ihdr_data),
      _png_chunk(b"IDAT", compressed_idat),
      _png_chunk(b"IEND", b""),
  ])


def _clamp_byte(value: float) -> int:
  """Clamps a floating-point 0..255 channel to an integer byte."""
  if value <= 0.0:
    return 0
  if value >= 255.0:
    return 255
  return int(round(value))


def _blend_over(
    canvas: bytearray,
    width: int,
    height: int,
    px: int,
    py: int,
    rgb: tuple[int, int, int],
    alpha: float,
) -> None:
  """Alpha-composites an RGB pixel onto an RGBA canvas buffer.

  Uses the straight (non-premultiplied) Porter-Duff "over" operator:
  `out_a = src_a + dst_a * (1 - src_a)` and
  `out_rgb = (src_rgb * src_a + dst_rgb * dst_a * (1 - src_a)) / out_a`.
  Out-of-bounds pixels and non-positive alpha are ignored.

  Args:
    canvas: Row-major RGBA bytes, modified in place.
    width: Canvas width in pixels.
    height: Canvas height in pixels.
    px: Pixel column (0 = left).
    py: Pixel row (0 = top).
    rgb: Source color, 0..255 per channel.
    alpha: Source coverage in 0..1; values above 1 are clamped.
  """
  if px < 0 or px >= width or py < 0 or py >= height or alpha <= 0.0:
    return
  src_a = min(1.0, max(0.0, alpha))
  idx = (py * width + px) * 4
  dst_r = canvas[idx]
  dst_g = canvas[idx + 1]
  dst_b = canvas[idx + 2]
  dst_a = canvas[idx + 3] / 255.0

  out_a = src_a + dst_a * (1.0 - src_a)
  if out_a <= 1e-6:
    return
  out_r = (rgb[0] * src_a + dst_r * dst_a * (1.0 - src_a)) / out_a
  out_g = (rgb[1] * src_a + dst_g * dst_a * (1.0 - src_a)) / out_a
  out_b = (rgb[2] * src_a + dst_b * dst_a * (1.0 - src_a)) / out_a

  canvas[idx] = _clamp_byte(out_r)
  canvas[idx + 1] = _clamp_byte(out_g)
  canvas[idx + 2] = _clamp_byte(out_b)
  canvas[idx + 3] = _clamp_byte(out_a * 255.0)


def _pseudo_noise(x: float, y: float) -> float:
  """Evaluates smooth deterministic trigonometric pseudo-noise in [-1, 1].

  A sum of three sines at unrelated frequencies; 1.95 is the sum of the
  amplitudes (1 + 0.6 + 0.35), which bounds the result to [-1, 1]. Used for
  the deckled paper edge and paper fiber shading without a random seed.
  """
  val = (
      math.sin(x * 0.43 + y * 0.17)
      + 0.6 * math.cos(x * 0.89 - y * 0.51)
      + 0.35 * math.sin(x * 1.71 + y * 1.37)
  )
  return val / 1.95


def _bbox_ranges(
    cx: float,
    cy: float,
    half_w: float,
    half_h: float,
    width: int,
    height: int,
) -> tuple[range, range]:
  """Returns clamped (x_range, y_range) covering [cx - half_w, cx + half_w].

  Used to limit per-pixel loops to a shape's bounding box. The upper bound
  gets +1 because `range` is exclusive; both ranges are clamped to the canvas.
  """
  x0 = max(0, math.floor(cx - half_w))
  x1 = min(width, math.ceil(cx + half_w) + 1)
  y0 = max(0, math.floor(cy - half_h))
  y1 = min(height, math.ceil(cy + half_h) + 1)
  return range(x0, x1), range(y0, y1)


def _draw_variant_motif(
    canvas: bytearray,
    width: int,
    height: int,
    accent_rgb: tuple[int, int, int],
    variant: str,
) -> None:
  """Draws a centered vector-style motif inside the thumbnail paper card.

  Args:
    canvas: Row-major RGBA bytes, modified in place.
    width: Canvas width in pixels.
    height: Canvas height in pixels.
    accent_rgb: Motif color, 0..255 per channel.
    variant: `"effect"` draws a four-point diamond star (|dx| + |dy| < 10.5
      px, anti-aliased over ~1.7 px); `"fold"` draws a square sheet with a
      folded corner. Unknown variants draw nothing.
  """
  cx = width * 0.5
  cy = height * 0.5
  xs, ys = _bbox_ranges(cx, cy, 10.5, 10.5, width, height)
  for py in ys:
    for px in xs:
      dx = px - cx
      dy = py - cy
      alpha = 0.0
      if variant == "effect":
        star_dist = abs(dx) + abs(dy)
        if star_dist < 10.5:
          alpha = min(1.0, (10.5 - star_dist) * 0.6)
      elif variant == "fold":
        # Square sheet whose top-right corner folds over the 45-degree line
        # dx - dy = 10, showing a lighter flap with a crease shadow.
        if abs(dx) < 10.5 and abs(dy) < 10.5 and dx - dy <= 10.0:
          alpha = 1.0
          if dx >= -0.5 and dy <= 0.5:
            crease = 10.0 - (dx - dy) < 1.4
            flap_rgb = (88, 66, 150) if crease else (213, 199, 247)
            _blend_over(canvas, width, height, px, py, flap_rgb, 1.0)
            continue

      if alpha > 0.0:
        _blend_over(canvas, width, height, px, py, accent_rgb, alpha)


def render_thumbnail_png(
    accent_rgb: tuple[int, int, int],
    variant: str,
) -> bytes:
  """Renders a 104x58 Effects Library PNG thumbnail for DaVinci Resolve.

  Layers, back to front: dark vertical-gradient backdrop with a 10 px dot
  grid, a soft drop shadow, a cream torn-paper card tilted by -0.07 rad
  (about -4 degrees), and the variant motif in the card center.

  Args:
    accent_rgb: Accent RGB tuple in 0..255 range for the card icon.
    variant: Motif key ('effect' or 'fold').

  Returns:
    PNG file bytes at 104x58 resolution.
  """
  width = THUMBNAIL_WIDTH
  height = THUMBNAIL_HEIGHT
  canvas = bytearray(width * height * 4)

  # Dark DaVinci Resolve UI backdrop with subtle calibration dots.
  for py in range(height):
    for px in range(width):
      idx = (py * width + px) * 4
      grad = int(28 + 8 * (py / height))
      dot_boost = 10 if (px % 10 == 4 and py % 10 == 4) else 0
      canvas[idx] = grad + dot_boost
      canvas[idx + 1] = grad + 2 + dot_boost
      canvas[idx + 2] = grad + 9 + dot_boost
      canvas[idx + 3] = 255

  # Card: 62x35 px rectangle (half extents 31 x 17.5) rotated by -0.07 rad.
  # Each pass maps pixels into card-local (lx, ly) with the inverse rotation.
  cx = width * 0.5
  cy = height * 0.5
  cos_a = math.cos(-0.07)
  sin_a = math.sin(-0.07)

  # Pass 1: Soft offset drop shadow behind tilted torn paper card.
  # Half extents 37.7 x 24.2 = card half size + 1.7 max deckle + 5 px
  # falloff, expanded to the axis-aligned box of the rotated rectangle.
  s_half_w = 37.7 * abs(cos_a) + 24.2 * abs(sin_a)
  s_half_h = 37.7 * abs(sin_a) + 24.2 * abs(cos_a)
  s_xs, s_ys = _bbox_ranges(
      cx + 2.5, cy + 3.0, s_half_w, s_half_h, width, height
  )
  for py in s_ys:
    for px in s_xs:
      sdx = px - (cx + 2.5)
      sdy = py - (cy + 3.0)
      lx = sdx * cos_a + sdy * sin_a
      ly = -sdx * sin_a + sdy * cos_a
      deckle = 1.7 * _pseudo_noise(px * 0.7, py * 0.7)
      dist_x = abs(lx) - (31.0 + deckle)
      dist_y = abs(ly) - (17.5 + deckle)
      outside = math.hypot(max(dist_x, 0.0), max(dist_y, 0.0))
      if outside < 5.0:
        shadow_a = 0.52 * max(0.0, 1.0 - outside / 5.0)
        _blend_over(canvas, width, height, px, py, (8, 9, 14), shadow_a)

  # Pass 2: Warm cream textured paper cutout card.
  # Half extents 33.8 x 20.3 = card half size + 1.8 max deckle + 1 px
  # anti-aliasing ramp (edge_dist in [0, 1) fades alpha).
  c_half_w = 33.8 * abs(cos_a) + 20.3 * abs(sin_a)
  c_half_h = 33.8 * abs(sin_a) + 20.3 * abs(cos_a)
  c_xs, c_ys = _bbox_ranges(cx, cy, c_half_w, c_half_h, width, height)
  for py in c_ys:
    for px in c_xs:
      dx = px - cx
      dy = py - cy
      lx = dx * cos_a + dy * sin_a
      ly = -dx * sin_a + dy * cos_a
      deckle = 1.8 * _pseudo_noise(px * 0.75, py * 0.75)
      edge_dist = max(abs(lx) - (31.0 + deckle), abs(ly) - (17.5 + deckle))
      if edge_dist < 1.0:
        card_a = min(1.0, max(0.0, 1.0 - edge_dist))
        fiber = int(7.0 * _pseudo_noise(px * 1.4, py * 1.4))
        paper_rgb = (
            _clamp_byte(246 + fiber),
            _clamp_byte(240 + fiber),
            _clamp_byte(228 + fiber),
        )
        _blend_over(canvas, width, height, px, py, paper_rgb, card_a)

  _draw_variant_motif(canvas, width, height, accent_rgb, variant)
  return encode_rgba_png(width, height, bytes(canvas))

"""Pure-stdlib RGBA PNG encoder and asset renderer for PaperCraft Studio Pro."""

import math
import struct
import zlib

THUMBNAIL_WIDTH = 104
THUMBNAIL_HEIGHT = 58


def _png_chunk(chunk_type: bytes, data_bytes: bytes) -> bytes:
  """Builds a single PNG chunk with length, type, payload, and CRC32."""
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
    raw_scanlines.append(0)
    start = row_index * stride
    raw_scanlines.extend(rgba_bytes[start : start + stride])

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
  """Alpha-composites an RGB pixel onto an RGBA canvas buffer."""
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
  """Evaluates smooth deterministic trigonometric pseudo-noise in [-1, 1]."""
  val = (
      math.sin(x * 0.43 + y * 0.17)
      + 0.6 * math.cos(x * 0.89 - y * 0.51)
      + 0.35 * math.sin(x * 1.71 + y * 1.37)
  )
  return val / 1.95


def _draw_variant_motif(
    canvas: bytearray,
    width: int,
    height: int,
    accent_rgb: tuple[int, int, int],
    variant: str,
) -> None:
  """Draws a centered vector-style motif inside the thumbnail paper card."""
  cx = width * 0.5
  cy = height * 0.5
  for py in range(height):
    for px in range(width):
      dx = px - cx
      dy = py - cy
      alpha = 0.0
      if variant == "effect":
        star_dist = abs(dx) + abs(dy)
        if star_dist < 10.5:
          alpha = min(1.0, (10.5 - star_dist) * 0.6)

      if alpha > 0.0:
        _blend_over(canvas, width, height, px, py, accent_rgb, alpha)


def render_thumbnail_png(
    accent_rgb: tuple[int, int, int],
    variant: str,
) -> bytes:
  """Renders a 104x58 Effects Library PNG thumbnail for DaVinci Resolve.

  Args:
    accent_rgb: Accent RGB tuple in 0..255 range for the card icon.
    variant: Motif key (currently only 'effect').

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

  cx = width * 0.5
  cy = height * 0.5
  cos_a = math.cos(-0.07)
  sin_a = math.sin(-0.07)

  # Pass 1: Soft offset drop shadow behind tilted torn paper card.
  for py in range(height):
    for px in range(width):
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
  for py in range(height):
    for px in range(width):
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

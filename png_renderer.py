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
      if variant == "generator":
        dist = math.hypot(dx, dy)
        if abs(dist - 10.5) < 1.8:
          alpha = max(0.0, 1.0 - abs(dist - 10.5) / 1.8)
        elif abs(dx) < 5.5 and abs(dy) < 1.6:
          alpha = 1.0
        elif abs(dy) < 5.5 and abs(dx) < 1.6:
          alpha = 1.0
      elif variant == "effect":
        star_dist = abs(dx) + abs(dy)
        if star_dist < 10.5:
          alpha = min(1.0, (10.5 - star_dist) * 0.6)
      elif variant == "unfold":
        if abs(dy) < 9.5 and abs(dx) < 12.0:
          fold_gap = abs(dx)
          if fold_gap > 1.2:
            alpha = 0.92 if dx < 0 else 0.7
      elif variant == "crumple":
        poly = math.hypot(dx, dy) + 2.6 * math.sin(math.atan2(dy, dx) * 5.0)
        if poly < 11.0:
          alpha = min(1.0, (11.0 - poly) * 0.7)
      else:
        if abs(dy) < 10.0 and abs(dx) < 13.0:
          tear_x = 1.8 * math.sin(py * 0.65)
          if abs(dx - tear_x) > 1.6:
            alpha = 0.9 if dx < tear_x else 0.65

      if alpha > 0.0:
        _blend_over(canvas, width, height, px, py, accent_rgb, alpha)


def render_thumbnail_png(
    accent_rgb: tuple[int, int, int],
    variant: str,
) -> bytes:
  """Renders a 104x58 Effects Library PNG thumbnail for DaVinci Resolve.

  Args:
    accent_rgb: Accent RGB tuple in 0..255 range for the card icon.
    variant: Motif key ('generator', 'effect', 'unfold', 'crumple', or 'rip').

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


def render_demo_sticker_png(size: int = 512) -> bytes:
  """Renders a 512x512 RGBA transparent demo sticker for File Browser mode.

  Args:
    size: Square pixel size of the generated sticker PNG.

  Returns:
    PNG file bytes with crisp alpha channel and studio emblem graphic.
  """
  canvas = bytearray(size * size * 4)
  center = size * 0.5
  outer_r = size * 0.40
  inner_r = size * 0.34
  star_r = size * 0.21

  for py in range(size):
    for px in range(size):
      dx = px - center
      dy = py - center
      dist = math.hypot(dx, dy)
      angle = math.atan2(dy, dx)

      if dist < outer_r + 1.5:
        edge_a = min(1.0, max(0.0, (outer_r + 1.0 - dist) * 0.8))
        # Subtle warm coral-orange gradient badge.
        t_y = (py - (center - outer_r)) / max(1.0, 2.0 * outer_r)
        base_r = _clamp_byte(242 - 35 * t_y)
        base_g = _clamp_byte(105 - 30 * t_y)
        base_b = _clamp_byte(62 + 25 * t_y)
        if dist > inner_r:
          # Crisp cream sticker rim.
          _blend_over(canvas, size, size, px, py, (252, 248, 240), edge_a)
        else:
          _blend_over(
              canvas, size, size, px, py, (base_r, base_g, base_b), edge_a
          )

        # Inner tilted folded paper diamond emblem.
        star_bound = star_r * (1.0 + 0.16 * math.cos(angle * 4.0))
        if abs(dx) + abs(dy) < star_bound:
          inner_a = min(1.0, (star_bound - (abs(dx) + abs(dy))) * 0.25)
          shade = (255, 250, 240) if dx + dy < 0 else (232, 224, 210)
          _blend_over(canvas, size, size, px, py, shade, inner_a)

  return encode_rgba_png(size, size, bytes(canvas))

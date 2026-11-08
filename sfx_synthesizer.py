"""Physical-modeling Foley synthesizer for the 24-file Paper SFX pack."""

from collections.abc import Sequence
import io
import math
import random
import wave

SAMPLE_RATE = 48000


def _biquad_coeffs_bandpass(
    sample_rate: int,
    center_hz: float,
    q_factor: float,
) -> tuple[float, float, float, float, float]:
  """Computes normalized RBJ constant-0-dB bandpass biquad coefficients."""
  omega = 2.0 * math.pi * center_hz / sample_rate
  sin_w = math.sin(omega)
  cos_w = math.cos(omega)
  alpha = sin_w / (2.0 * max(0.2, q_factor))
  b0 = alpha
  b1 = 0.0
  b2 = -alpha
  a0 = 1.0 + alpha
  a1 = -2.0 * cos_w
  a2 = 1.0 - alpha
  return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def _biquad_coeffs_lowpass(
    sample_rate: int,
    cutoff_hz: float,
    q_factor: float = 0.707,
) -> tuple[float, float, float, float, float]:
  """Computes normalized RBJ lowpass biquad coefficients."""
  omega = 2.0 * math.pi * min(cutoff_hz, sample_rate * 0.45) / sample_rate
  sin_w = math.sin(omega)
  cos_w = math.cos(omega)
  alpha = sin_w / (2.0 * max(0.2, q_factor))
  b0 = (1.0 - cos_w) * 0.5
  b1 = 1.0 - cos_w
  b2 = (1.0 - cos_w) * 0.5
  a0 = 1.0 + alpha
  a1 = -2.0 * cos_w
  a2 = 1.0 - alpha
  return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def _biquad_coeffs_highpass(
    sample_rate: int,
    cutoff_hz: float,
    q_factor: float = 0.707,
) -> tuple[float, float, float, float, float]:
  """Computes normalized RBJ highpass biquad coefficients."""
  omega = 2.0 * math.pi * min(cutoff_hz, sample_rate * 0.45) / sample_rate
  sin_w = math.sin(omega)
  cos_w = math.cos(omega)
  alpha = sin_w / (2.0 * max(0.2, q_factor))
  b0 = (1.0 + cos_w) * 0.5
  b1 = -(1.0 + cos_w)
  b2 = (1.0 + cos_w) * 0.5
  a0 = 1.0 + alpha
  a1 = -2.0 * cos_w
  a2 = 1.0 - alpha
  return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def _run_biquad(
    samples: Sequence[float],
    coeffs: tuple[float, float, float, float, float],
) -> list[float]:
  """Filters a mono signal with a direct-form-II transposed biquad."""
  b0, b1, b2, a1, a2 = coeffs
  z1 = 0.0
  z2 = 0.0
  out: list[float] = []
  append = out.append
  for x in samples:
    y = b0 * x + z1
    z1 = b1 * x - a1 * y + z2
    z2 = b2 * x - a2 * y
    append(y)
  return out


def apply_biquad_bandpass(
    samples: Sequence[float],
    sample_rate: int,
    center_hz: float,
    q_factor: float,
) -> list[float]:
  """Applies a bandpass filter centered at center_hz."""
  return _run_biquad(
      samples,
      _biquad_coeffs_bandpass(sample_rate, center_hz, q_factor),
  )


def apply_biquad_lowpass(
    samples: Sequence[float],
    sample_rate: int,
    cutoff_hz: float,
    q_factor: float = 0.707,
) -> list[float]:
  """Applies a lowpass filter at cutoff_hz."""
  return _run_biquad(
      samples,
      _biquad_coeffs_lowpass(sample_rate, cutoff_hz, q_factor),
  )


def apply_biquad_highpass(
    samples: Sequence[float],
    sample_rate: int,
    cutoff_hz: float,
    q_factor: float = 0.707,
) -> list[float]:
  """Applies a highpass filter at cutoff_hz."""
  return _run_biquad(
      samples,
      _biquad_coeffs_highpass(sample_rate, cutoff_hz, q_factor),
  )


def _generate_buckling_impulses(
    num_samples: int,
    density_envelope: Sequence[float],
    rng: random.Random,
    power_law_exp: float = 1.8,
    min_burst_ms: float = 0.5,
    max_burst_ms: float = 2.8,
) -> list[float]:
  """Synthesizes discrete elastic paper-ridge buckling micro-impulses."""
  buf = [0.0] * num_samples
  idx = 0
  while idx < num_samples:
    density = max(0.02, density_envelope[idx])
    mean_gap = max(25.0, (SAMPLE_RATE / 260.0) / density)
    step = int(rng.expovariate(1.0 / mean_gap)) + 1
    idx += step
    if idx >= num_samples:
      break
    # Heavy-tailed amplitude simulates sudden elastic snap events in paper.
    raw_u = max(0.002, rng.random())
    amp = min(3.5, raw_u ** (-1.0 / power_law_exp) * 0.22) * (
        1.0 if rng.random() > 0.5 else -1.0
    )
    burst_len = max(
        8,
        int(rng.uniform(min_burst_ms, max_burst_ms) * 0.001 * SAMPLE_RATE),
    )
    freq_hz = rng.uniform(1200.0, 5800.0)
    decay = 5.0 / burst_len
    end = min(num_samples, idx + burst_len)
    for k in range(idx, end):
      local_n = k - idx
      snap = math.exp(-decay * local_n) * math.sin(
          2.0 * math.pi * freq_hz * (local_n / SAMPLE_RATE) + 0.4
      )
      buf[k] += amp * snap * density
  return buf


def _generate_white_noise(num_samples: int, rng: random.Random) -> list[float]:
  """Generates uniform zero-mean white noise."""
  uniform = rng.uniform
  return [uniform(-1.0, 1.0) for _ in range(num_samples)]


def _modal_paper_body(
    exciter: Sequence[float],
    modes: Sequence[tuple[float, float, float]],
) -> list[float]:
  """Passes an exciter signal through a parallel bank of paper modes."""
  out = [0.0] * len(exciter)
  for center_hz, q_factor, gain in modes:
    filtered = apply_biquad_bandpass(exciter, SAMPLE_RATE, center_hz, q_factor)
    for i, val in enumerate(filtered):
      out[i] += val * gain
  return out


def _apply_envelope(
    samples: Sequence[float],
    envelope: Sequence[float],
) -> list[float]:
  """Multiplies sample sequence element-wise by envelope."""
  return [s * e for s, e in zip(samples, envelope, strict=True)]


def _mix_layers(*layers: tuple[Sequence[float], float]) -> list[float]:
  """Mixes weighted mono layers of identical length."""
  if not layers:
    return []
  length = len(layers[0][0])
  out = [0.0] * length
  for buf, gain in layers:
    for i, val in enumerate(buf):
      out[i] += val * gain
  return out


def _stereo_decorrelate(
    mono_main: Sequence[float],
    rng: random.Random,
    delay_ms: float = 1.4,
    width: float = 0.35,
    pan_sweep: float = 0.0,
) -> tuple[list[float], list[float]]:
  """Creates realistic stereo width via Haas offset and subtle air diffusion."""
  num_samples = len(mono_main)
  delay_samples = max(1, int(delay_ms * 0.001 * SAMPLE_RATE))
  air_noise = apply_biquad_bandpass(
      _generate_white_noise(num_samples, rng),
      SAMPLE_RATE,
      3600.0,
      1.1,
  )
  left = [0.0] * num_samples
  right = [0.0] * num_samples
  for i in range(num_samples):
    t_norm = i / max(1, num_samples - 1)
    pan = max(-0.85, min(0.85, pan_sweep * (2.0 * t_norm - 1.0)))
    l_gain = math.sqrt(0.5 * (1.0 - pan))
    r_gain = math.sqrt(0.5 * (1.0 + pan))
    delayed = mono_main[i - delay_samples] if i >= delay_samples else 0.0
    side = (mono_main[i] - delayed) * width + air_noise[i] * 0.04
    left[i] = (mono_main[i] + side) * l_gain
    right[i] = (mono_main[i] - side) * r_gain
  return (left, right)


def encode_wav_24bit_stereo(
    left: Sequence[float],
    right: Sequence[float],
    sample_rate: int = SAMPLE_RATE,
    peak_target: float = 0.84,
) -> bytes:
  """Normalizes, removes DC, fades edges, and writes 24-bit stereo WAV bytes.

  Args:
    left: Left channel float samples.
    right: Right channel float samples.
    sample_rate: Output PCM sample rate in Hz.
    peak_target: Peak normalization amplitude in (0, 0.99].

  Returns:
    Complete RIFF WAV bytes (24-bit signed little-endian stereo).

  Raises:
    ValueError: If channel lengths differ or audio buffer is empty.
  """
  num_samples = len(left)
  if num_samples != len(right):
    raise ValueError("Left and right channel lengths must match.")
  if num_samples == 0:
    raise ValueError("Cannot encode an empty audio buffer.")

  mean_l = sum(left) / num_samples
  mean_r = sum(right) / num_samples
  dc_l = [x - mean_l for x in left]
  dc_r = [x - mean_r for x in right]

  fade_in = min(num_samples // 4, int(0.003 * sample_rate))
  fade_out = min(num_samples // 3, int(0.016 * sample_rate))
  for i in range(fade_in):
    w = 0.5 * (1.0 - math.cos(math.pi * i / max(1, fade_in)))
    dc_l[i] *= w
    dc_r[i] *= w
  for i in range(fade_out):
    idx = num_samples - 1 - i
    w = 0.5 * (1.0 - math.cos(math.pi * i / max(1, fade_out)))
    dc_l[idx] *= w
    dc_r[idx] *= w

  max_abs = max(max(abs(x) for x in dc_l), max(abs(x) for x in dc_r), 1e-9)
  scale = peak_target / max_abs
  max_int24 = 8388607
  pcm_bytes = bytearray(num_samples * 6)
  write_pos = 0
  for i in range(num_samples):
    sl = max(-0.99, min(0.99, dc_l[i] * scale))
    sr = max(-0.99, min(0.99, dc_r[i] * scale))
    il = int(round(sl * max_int24))
    ir = int(round(sr * max_int24))
    # Pack signed 24-bit little-endian directly into 3-byte slices.
    pcm_bytes[write_pos : write_pos + 3] = il.to_bytes(
        3, byteorder="little", signed=True
    )
    pcm_bytes[write_pos + 3 : write_pos + 6] = ir.to_bytes(
        3, byteorder="little", signed=True
    )
    write_pos += 6

  out_stream = io.BytesIO()
  with wave.open(out_stream, "wb") as wav_out:
    wav_out.setnchannels(2)
    wav_out.setsampwidth(3)
    wav_out.setframerate(sample_rate)
    wav_out.writeframes(bytes(pcm_bytes))
  return out_stream.getvalue()


def _bell_env(
    num_samples: int,
    peak_pos: float = 0.45,
    sharpness: float = 2.0,
) -> list[float]:
  """Generates a smooth asymmetric raised-bell envelope in [0, 1]."""
  env: list[float] = []
  for i in range(num_samples):
    t = i / max(1, num_samples - 1)
    if t <= peak_pos:
      u = t / max(1e-4, peak_pos)
    else:
      u = (1.0 - t) / max(1e-4, 1.0 - peak_pos)
    env.append(math.sin(0.5 * math.pi * max(0.0, u)) ** sharpness)
  return env


def _synth_page_turn(
    seed: int,
    duration_s: float,
    modes: Sequence[tuple[float, float, float]],
    pan_sweep: float,
    snap_strength: float,
) -> bytes:
  """Synthesizes a page turn (friction swoosh + mid-lift crinkle + settle)."""
  rng = random.Random(seed)
  n = int(duration_s * SAMPLE_RATE)
  env = _bell_env(n, peak_pos=0.42, sharpness=1.6)
  air = apply_biquad_highpass(_generate_white_noise(n, rng), SAMPLE_RATE, 750.0)
  air_body = _apply_envelope(_modal_paper_body(air, modes), env)

  crackle_env = [0.25 + 0.75 * e for e in env]
  snaps = _modal_paper_body(
      _generate_buckling_impulses(n, crackle_env, rng, power_law_exp=1.9),
      modes,
  )
  # Soft landing thump near 78% of duration.
  thump = [0.0] * n
  hit_idx = int(n * 0.76)
  for k in range(hit_idx, min(n, hit_idx + 1800)):
    dt = (k - hit_idx) / SAMPLE_RATE
    thump[k] = math.exp(-dt * 42.0) * math.sin(2.0 * math.pi * 145.0 * dt)

  mono = _mix_layers(
      (air_body, 1.0),
      (snaps, snap_strength),
      (thump, 0.35),
  )
  left, right = _stereo_decorrelate(
      mono, rng, delay_ms=1.8, width=0.45, pan_sweep=pan_sweep
  )
  return encode_wav_24bit_stereo(left, right)


def _synth_paper_hit(
    seed: int,
    duration_s: float,
    fundamental_hz: float,
    modes: Sequence[tuple[float, float, float]],
    slap_decay: float,
) -> bytes:
  """Synthesizes a crisp paper slap, sticker snap, or stack thud."""
  rng = random.Random(seed)
  n = int(duration_s * SAMPLE_RATE)
  noise = _generate_white_noise(n, rng)
  transient = [0.0] * n
  membrane = [0.0] * n
  for i in range(n):
    t = i / SAMPLE_RATE
    transient[i] = noise[i] * math.exp(-t * slap_decay)
    membrane[i] = math.exp(-t * 24.0) * (
        0.65 * math.sin(2.0 * math.pi * fundamental_hz * t)
        + 0.35 * math.sin(2.0 * math.pi * fundamental_hz * 1.83 * t)
    )

  crisp_modes = _modal_paper_body(transient, modes)
  after_rustle = _apply_envelope(
      _generate_buckling_impulses(n, [0.45] * n, rng, power_law_exp=2.1),
      [math.exp(-i / (SAMPLE_RATE * 0.09)) for i in range(n)],
  )
  mono = _mix_layers((crisp_modes, 1.0), (membrane, 0.55), (after_rustle, 0.4))
  left, right = _stereo_decorrelate(mono, rng, delay_ms=0.9, width=0.3)
  return encode_wav_24bit_stereo(left, right)


def _synth_crumple(
    seed: int,
    duration_s: float,
    unfold: bool,
    modes: Sequence[tuple[float, float, float]],
    density_scale: float,
) -> bytes:
  """Synthesizes crumpling or unfolding paper with elastic ridge bursts."""
  rng = random.Random(seed)
  n = int(duration_s * SAMPLE_RATE)
  env: list[float] = []
  for i in range(n):
    t = i / max(1, n - 1)
    if unfold:
      val = (1.0 - 0.72 * t) * math.sin(math.pi * min(1.0, t * 1.15)) ** 0.7
    else:
      val = (0.35 + 0.65 * t) * math.sin(math.pi * t) ** 0.55
    env.append(max(0.05, val) * density_scale)

  impulses = _generate_buckling_impulses(
      n, env, rng, power_law_exp=1.65, min_burst_ms=0.6, max_burst_ms=3.2
  )
  friction = _apply_envelope(
      apply_biquad_highpass(_generate_white_noise(n, rng), SAMPLE_RATE, 1100.0),
      [0.25 * e for e in env],
  )
  body = _modal_paper_body(impulses, modes)
  mono = _mix_layers((body, 1.0), (friction, 0.55))
  left, right = _stereo_decorrelate(
      mono, rng, delay_ms=1.6, width=0.55, pan_sweep=0.25 if unfold else -0.2
  )
  return encode_wav_24bit_stereo(left, right)


def _synth_rip(
    seed: int,
    duration_s: float,
    modes: Sequence[tuple[float, float, float]],
    fiber_rate: float,
) -> bytes:
  """Synthesizes tearing paper fibers via rapid micro-rupture trains."""
  rng = random.Random(seed)
  n = int(duration_s * SAMPLE_RATE)
  env: list[float] = []
  for i in range(n):
    t = i / max(1, n - 1)
    # Realistic uneven fiber catch-and-release modulation.
    flutter = 0.75 + 0.25 * math.sin(2.0 * math.pi * 17.0 * t + 0.5)
    shape = math.sin(math.pi * t) ** 0.45
    env.append(max(0.05, shape * flutter * fiber_rate))

  ruptures = _generate_buckling_impulses(
      n, env, rng, power_law_exp=1.55, min_burst_ms=0.4, max_burst_ms=1.9
  )
  hiss = _apply_envelope(
      apply_biquad_bandpass(
          _generate_white_noise(n, rng), SAMPLE_RATE, 3200.0, 0.9
      ),
      [0.35 * e for e in env],
  )
  body = _modal_paper_body(ruptures, modes)
  mono = _mix_layers((body, 1.0), (hiss, 0.7))
  left, right = _stereo_decorrelate(
      mono, rng, delay_ms=1.1, width=0.42, pan_sweep=0.45
  )
  return encode_wav_24bit_stereo(left, right)


def _synth_rustle_or_bits(
    seed: int,
    duration_s: float,
    modes: Sequence[tuple[float, float, float]],
    pulses: int,
) -> bytes:
  """Synthesizes rhythmic paper jiggles, confetti bits, or desk scrapes."""
  rng = random.Random(seed)
  n = int(duration_s * SAMPLE_RATE)
  env = [0.0] * n
  for p_idx in range(pulses):
    center = (p_idx + 0.5) / pulses
    width = 0.22 / max(1, pulses)
    for i in range(n):
      t = i / max(1, n - 1)
      dist = (t - center) / max(1e-4, width)
      env[i] = max(env[i], math.exp(-0.5 * dist * dist))

  impulses = _generate_buckling_impulses(
      n, [0.35 + 0.9 * e for e in env], rng, power_law_exp=2.0
  )
  scrape = _apply_envelope(
      apply_biquad_highpass(_generate_white_noise(n, rng), SAMPLE_RATE, 1500.0),
      env,
  )
  mono = _mix_layers(
      (_modal_paper_body(impulses, modes), 0.9),
      (_modal_paper_body(scrape, modes), 0.65),
  )
  left, right = _stereo_decorrelate(mono, rng, delay_ms=1.5, width=0.5)
  return encode_wav_24bit_stereo(left, right)


def _synth_designed_transition(
    seed: int,
    duration_s: float,
    slam_at_end: bool,
    modes: Sequence[tuple[float, float, float]],
) -> bytes:
  """Synthesizes layered motion-graphics paper whoosh/unfold/slam transitions."""
  rng = random.Random(seed)
  n = int(duration_s * SAMPLE_RATE)
  noise = _generate_white_noise(n, rng)
  whoosh = [0.0] * n
  peak_t = 0.72 if slam_at_end else 0.44
  for i in range(n):
    t = i / max(1, n - 1)
    env = math.exp(-((t - peak_t) ** 2) / 0.055)
    whoosh[i] = noise[i] * env

  low_whoosh = apply_biquad_bandpass(whoosh, SAMPLE_RATE, 320.0, 1.3)
  mid_paper = _modal_paper_body(whoosh, modes)
  crackle = _generate_buckling_impulses(
      n,
      [
          0.3 + 0.9 * math.exp(-(((i / n) - peak_t) ** 2) / 0.06)
          for i in range(n)
      ],
      rng,
      power_law_exp=1.7,
  )
  slam = [0.0] * n
  if slam_at_end:
    hit_idx = int(n * 0.70)
    for k in range(hit_idx, n):
      dt = (k - hit_idx) / SAMPLE_RATE
      slam[k] = math.exp(-dt * 22.0) * (
          0.7 * math.sin(2.0 * math.pi * 88.0 * dt)
          + 0.35 * noise[k] * math.exp(-dt * 30.0)
      )

  mono = _mix_layers(
      (low_whoosh, 0.75),
      (mid_paper, 1.0),
      (_modal_paper_body(crackle, modes), 0.75),
      (slam, 0.85),
  )
  left, right = _stereo_decorrelate(
      mono,
      rng,
      delay_ms=2.1,
      width=0.5,
      pan_sweep=0.55 if not slam_at_end else 0.0,
  )
  return encode_wav_24bit_stereo(left, right)


def synthesize_all_paper_sfx() -> dict[str, bytes]:
  """Synthesizes all 24 starter Paper SFX WAV files across 6 categories.

  Returns:
    Mapping from archive-relative WAV path (under Paper_SFX/) to WAV bytes.
  """
  light_modes = ((950.0, 1.8, 0.6), (2400.0, 2.2, 1.0), (5200.0, 1.6, 0.8))
  heavy_modes = ((420.0, 1.7, 0.9), (1450.0, 2.0, 1.0), (3800.0, 1.8, 0.55))
  news_modes = ((680.0, 1.6, 0.75), (2100.0, 2.5, 1.0), (4900.0, 2.1, 0.9))

  return {
      # 01_Page_Turns (4)
      "Paper_SFX/01_Page_Turns/Page_Turn_Crisp_Book_01.wav": _synth_page_turn(
          101, 0.55, light_modes, pan_sweep=0.6, snap_strength=0.45
      ),
      "Paper_SFX/01_Page_Turns/Page_Turn_Heavy_Cardstock_02.wav": (
          _synth_page_turn(
              102, 0.62, heavy_modes, pan_sweep=-0.55, snap_strength=0.35
          )
      ),
      "Paper_SFX/01_Page_Turns/Page_Turn_Newspaper_Broadsheet_03.wav": (
          _synth_page_turn(
              103, 0.78, news_modes, pan_sweep=0.45, snap_strength=0.65
          )
      ),
      "Paper_SFX/01_Page_Turns/Page_Turn_Quick_Notebook_Flip_04.wav": (
          _synth_page_turn(
              104, 0.34, light_modes, pan_sweep=-0.65, snap_strength=0.55
          )
      ),
      # 02_Paper_Hits_Impacts (4)
      "Paper_SFX/02_Paper_Hits_Impacts/Paper_Hit_Table_Slap_01.wav": (
          _synth_paper_hit(201, 0.36, 135.0, heavy_modes, slap_decay=55.0)
      ),
      "Paper_SFX/02_Paper_Hits_Impacts/Paper_Hit_Cardboard_Drop_02.wav": (
          _synth_paper_hit(202, 0.44, 98.0, heavy_modes, slap_decay=38.0)
      ),
      "Paper_SFX/02_Paper_Hits_Impacts/Paper_Hit_Sticker_Snap_03.wav": (
          _synth_paper_hit(203, 0.28, 220.0, light_modes, slap_decay=82.0)
      ),
      "Paper_SFX/02_Paper_Hits_Impacts/Paper_Hit_Stack_Thud_04.wav": (
          _synth_paper_hit(204, 0.48, 84.0, heavy_modes, slap_decay=34.0)
      ),
      # 03_Crumples_Uncrumples (4)
      "Paper_SFX/03_Crumples_Uncrumples/Paper_Crumple_Tight_Ball_01.wav": (
          _synth_crumple(
              301, 0.95, unfold=False, modes=news_modes, density_scale=1.25
          )
      ),
      "Paper_SFX/03_Crumples_Uncrumples/Paper_Crumple_Newspaper_02.wav": (
          _synth_crumple(
              302, 1.15, unfold=False, modes=heavy_modes, density_scale=1.05
          )
      ),
      "Paper_SFX/03_Crumples_Uncrumples/Paper_Uncrumple_Smooth_Reveal_03.wav": (
          _synth_crumple(
              303, 0.90, unfold=True, modes=news_modes, density_scale=1.0
          )
      ),
      "Paper_SFX/03_Crumples_Uncrumples/Paper_Uncrumple_Quick_Open_04.wav": (
          _synth_crumple(
              304, 0.56, unfold=True, modes=light_modes, density_scale=1.15
          )
      ),
      # 04_Rips_Tears (4)
      "Paper_SFX/04_Rips_Tears/Paper_Rip_Fast_Tear_01.wav": _synth_rip(
          401, 0.42, news_modes, fiber_rate=1.35
      ),
      "Paper_SFX/04_Rips_Tears/Paper_Rip_Slow_Fiber_02.wav": _synth_rip(
          402, 1.05, light_modes, fiber_rate=0.95
      ),
      "Paper_SFX/04_Rips_Tears/Paper_Rip_Thick_Kraft_03.wav": _synth_rip(
          403, 0.68, heavy_modes, fiber_rate=1.1
      ),
      "Paper_SFX/04_Rips_Tears/Paper_Rip_Short_Snip_04.wav": _synth_rip(
          404, 0.28, light_modes, fiber_rate=1.45
      ),
      # 05_Little_Bits_Rustles (4)
      "Paper_SFX/05_Little_Bits_Rustles/Paper_Bits_Confetti_Scatter_01.wav": (
          _synth_rustle_or_bits(501, 0.72, light_modes, pulses=7)
      ),
      "Paper_SFX/05_Little_Bits_Rustles/Paper_Rustle_StopMotion_Jiggle_02.wav": _synth_rustle_or_bits(
          502, 0.85, news_modes, pulses=6
      ),
      "Paper_SFX/05_Little_Bits_Rustles/Paper_Slide_Desk_Scrape_03.wav": (
          _synth_rustle_or_bits(503, 0.52, heavy_modes, pulses=2)
      ),
      "Paper_SFX/05_Little_Bits_Rustles/Paper_Crease_Finger_Fold_04.wav": (
          _synth_rustle_or_bits(504, 0.45, light_modes, pulses=3)
      ),
      # 06_Designed_Transitions (4)
      "Paper_SFX/06_Designed_Transitions/Paper_Designed_Whoosh_Unfold_01.wav": (
          _synth_designed_transition(
              601, 0.74, slam_at_end=False, modes=news_modes
          )
      ),
      (
          "Paper_SFX/06_Designed_Transitions/"
          "Paper_Designed_Airplane_Swoosh_02.wav"
      ): _synth_designed_transition(
          602, 0.64, slam_at_end=False, modes=light_modes
      ),
      "Paper_SFX/06_Designed_Transitions/Paper_Designed_Crumple_Slam_03.wav": (
          _synth_designed_transition(
              603, 0.78, slam_at_end=True, modes=heavy_modes
          )
      ),
      "Paper_SFX/06_Designed_Transitions/Paper_Designed_StopMotion_Pop_04.wav": _synth_designed_transition(
          604, 0.42, slam_at_end=True, modes=light_modes
      ),
  }

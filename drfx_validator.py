"""Structural validators for Fusion .setting, .drfx bundles, and WAV audio."""

import io
import math
import re
import struct
import wave
import zipfile

try:
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import png_renderer
except ImportError:
  import png_renderer  # type: ignore[no-redef]


def _check_balanced_delimiters(text: str) -> None:
  """Verifies balanced {}, () and properly closed string literals."""
  in_string = False
  escaped = False
  brace_depth = 0
  paren_depth = 0
  for idx, ch in enumerate(text):
    if in_string:
      if escaped:
        escaped = False
      elif ch == "\\":
        escaped = True
      elif ch == '"':
        in_string = False
      continue
    if ch == '"':
      in_string = True
    elif ch == "{":
      brace_depth += 1
    elif ch == "}":
      brace_depth -= 1
      if brace_depth < 0:
        raise ValueError(f"Unexpected closing brace at index {idx}.")
    elif ch == "(":
      paren_depth += 1
    elif ch == ")":
      paren_depth -= 1
      if paren_depth < 0:
        raise ValueError(f"Unexpected closing paren at index {idx}.")
  if in_string:
    raise ValueError("Unterminated string literal in .setting file.")
  if brace_depth != 0:
    raise ValueError(
        f"Unbalanced braces in .setting file: depth={brace_depth}."
    )
  if paren_depth != 0:
    raise ValueError(
        f"Unbalanced parens in .setting file: depth={paren_depth}."
    )


def _extract_brace_block(text: str, marker: str, start_pos: int = 0) -> str:
  """Extracts the {...} substring immediately following a block marker."""
  idx = text.find(marker, start_pos)
  if idx < 0:
    raise ValueError(f"Missing required block marker: {marker}")
  open_pos = text.find("{", idx)
  if open_pos < 0:
    raise ValueError(f"Missing opening brace after marker: {marker}")
  depth = 0
  in_string = False
  escaped = False
  for pos in range(open_pos, len(text)):
    ch = text[pos]
    if in_string:
      if escaped:
        escaped = False
      elif ch == "\\":
        escaped = True
      elif ch == '"':
        in_string = False
      continue
    if ch == '"':
      in_string = True
    elif ch == "{":
      depth += 1
    elif ch == "}":
      depth -= 1
      if depth == 0:
        return text[open_pos + 1 : pos]
  raise ValueError(f"Unclosed block for marker: {marker}")


def validate_setting_syntax_and_graph(
    setting_text: str,
    template_kind: str,
) -> None:
  """Validates a Fusion .setting template's syntax, graph wiring, and UI.

  Args:
    setting_text: Full contents of a Fusion .setting template file.
    template_kind: One of 'generator', 'effect', or 'transition'.

  Raises:
    ValueError: If syntax, node connections, expressions, or UI groups fail.
  """
  if template_kind not in ("generator", "effect", "transition"):
    raise ValueError(f"Unsupported template_kind: {template_kind!r}")

  _check_balanced_delimiters(setting_text)
  active_match = re.search(r'ActiveTool\s*=\s*"([A-Za-z0-9_]+)"', setting_text)
  if not active_match:
    raise ValueError("Missing ActiveTool declaration in .setting file.")
  macro_name = active_match.group(1)

  if f"{macro_name} = MacroOperator" not in setting_text:
    raise ValueError(f"Expected {macro_name} = MacroOperator in .setting file.")
  if "ViewInfo = GroupInfo" in setting_text:
    raise ValueError("Macro must use ViewInfo = OperatorInfo, not GroupInfo.")

  macro_pos = setting_text.find(f"{macro_name} = MacroOperator")
  macro_inputs_body = _extract_brace_block(
      setting_text, "Inputs = ordered()", macro_pos
  )
  outputs_body = _extract_brace_block(setting_text, "Outputs =", macro_pos)
  inner_tools_body = _extract_brace_block(
      setting_text, "Tools = ordered()", macro_pos
  )
  user_controls_body = _extract_brace_block(
      setting_text, "UserControls = ordered()", macro_pos
  )

  # Check required MainInput / MainOutput declarations by template kind.
  if "MainOutput1 = InstanceOutput" not in outputs_body:
    raise ValueError("Missing MainOutput1 = InstanceOutput declaration.")
  has_main_in_1 = "MainInput1 = InstanceInput" in macro_inputs_body
  has_main_in_2 = "MainInput2 = InstanceInput" in macro_inputs_body
  if template_kind == "generator":
    if has_main_in_1 or has_main_in_2:
      raise ValueError("Generator template must not declare MainInput1/2.")
    if "GlobalEnd = 1000000" not in setting_text:
      raise ValueError("Generator Loader must declare GlobalEnd = 1000000.")
    if "GlobalOut = Input { Value = 1000000" not in setting_text:
      raise ValueError("Generator MediaIn must declare GlobalOut = 1000000.")
  if template_kind == "effect" and (not has_main_in_1 or has_main_in_2):
    raise ValueError("Effect template must declare MainInput1 only.")
  if template_kind == "transition" and (not has_main_in_1 or not has_main_in_2):
    raise ValueError("Transition template must declare MainInput1 and 2.")

  # Collect top-level operator names inside the macro's internal Tools block.
  internal_ops = set(
      re.findall(
          r"^\t{4}([A-Za-z0-9_]+)\s*=\s*[A-Za-z0-9_]+\s*\{",
          inner_tools_body,
          flags=re.MULTILINE,
      )
  )
  if not internal_ops:
    raise ValueError("No internal operators found inside macro Tools block.")

  for src_op in re.findall(r'SourceOp\s*=\s*"([A-Za-z0-9_]+)"', setting_text):
    if src_op not in internal_ops:
      raise ValueError(f"Dangling SourceOp reference: {src_op!r}")

  # Collect declared UserControl identifiers and check LBLC_NumInputs counts.
  control_entries = list(
      re.finditer(
          r"^\t{4}([A-Za-z0-9_]+)\s*=\s*\{",
          user_controls_body,
          flags=re.MULTILINE,
      )
  )
  declared_controls = {m.group(1) for m in control_entries}
  if not declared_controls:
    raise ValueError("UserControls block is empty.")

  control_blocks: list[tuple[str, str]] = []
  for idx, match in enumerate(control_entries):
    start = match.start()
    end = (
        control_entries[idx + 1].start()
        if idx + 1 < len(control_entries)
        else len(user_controls_body)
    )
    control_blocks.append((match.group(1), user_controls_body[start:end]))

  for idx, (ctrl_name, block_text) in enumerate(control_blocks):
    if "LBLC_DropDownButton = true" in block_text:
      num_match = re.search(r"LBLC_NumInputs\s*=\s*(\d+)", block_text)
      if not num_match:
        raise ValueError(f"LabelControl {ctrl_name} missing LBLC_NumInputs.")
      expected_count = int(num_match.group(1))
      actual_count = 0
      for _, next_block in control_blocks[idx + 1 :]:
        if "LBLC_DropDownButton = true" in next_block:
          break
        actual_count += 1
      if expected_count != actual_count:
        raise ValueError(
            f"LabelControl {ctrl_name} declares LBLC_NumInputs={expected_count}"
            f", but has {actual_count} following controls."
        )

  # Validate SimpleExpressions syntax and control references.
  for expr in re.findall(r'Expression\s*=\s*"([^"]+)"', setting_text):
    if "math." in expr:
      raise ValueError(
          f"Illegal Lua 'math.' prefix in SimpleExpression: {expr}"
      )
    if "&&" in expr or "||" in expr:
      raise ValueError(
          f"Illegal C-style '&&' or '||' in SimpleExpression: {expr}"
      )
    depth = 0
    for ch in expr:
      if ch == "(":
        depth += 1
      elif ch == ")":
        depth -= 1
        if depth < 0:
          raise ValueError(f"Unbalanced parens in expression: {expr}")
    if depth != 0:
      raise ValueError(f"Unbalanced parens in expression: {expr}")
    for ctrl_ref in re.findall(
        rf"\b{re.escape(macro_name)}\.([A-Za-z0-9_]+)", expr
    ):
      if ctrl_ref not in declared_controls:
        raise ValueError(
            f"Expression references undeclared control {ctrl_ref!r}: {expr}"
        )


def validate_drfx_bytes(drfx_bytes: bytes) -> None:
  """Validates a .drfx archive layout, .setting graphs, thumbnails, and paths.

  Args:
    drfx_bytes: Complete bytes of a PaperCraft_Studio_Pro.drfx ZIP file.

  Raises:
    ValueError: If any archive path, thumbnail, or .setting validation fails.
  """
  with zipfile.ZipFile(io.BytesIO(drfx_bytes), "r") as zf:
    names = set(zf.namelist())
    setting_files = sorted(n for n in names if n.endswith(".setting"))
    kinds_by_prefix = {
        "Edit/Effects/": "effect",
        "Edit/Transitions/": "transition",
    }
    found_kinds = sorted(
        kind
        for path in setting_files
        for prefix, kind in kinds_by_prefix.items()
        if path.startswith(prefix)
    )
    if found_kinds != ["effect", "transition"] or len(setting_files) != 2:
      raise ValueError(
          "Expected one Edit/Effects and one Edit/Transitions .setting template"
          f" in .drfx, found {setting_files}."
      )

    for setting_path in setting_files:
      kind = next(
          kind
          for prefix, kind in kinds_by_prefix.items()
          if setting_path.startswith(prefix)
      )

      setting_text = zf.read(setting_path).decode("utf-8")
      validate_setting_syntax_and_graph(setting_text, kind)

      thumb_path = setting_path[: -len(".setting")] + ".png"
      if thumb_path not in names:
        raise ValueError(f"Missing thumbnail PNG for {setting_path}")
      png_bytes = zf.read(thumb_path)
      if not png_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"Invalid PNG signature in {thumb_path}")
      width, height = struct.unpack(">II", png_bytes[16:24])
      if (width, height) != (
          png_renderer.THUMBNAIL_WIDTH,
          png_renderer.THUMBNAIL_HEIGHT,
      ):
        raise ValueError(
            f"Thumbnail {thumb_path} must be 104x58, got {width}x{height}."
        )

      setting_dir = setting_path.rsplit("/", 1)[0]
      for rel_asset in re.findall(r'Setting:/([^"]+)', setting_text):
        expected_member = f"{setting_dir}/{rel_asset}"
        if expected_member not in names:
          raise ValueError(
              f"Missing bundled asset {expected_member} referenced by "
              f"{setting_path}."
          )


def validate_wav_bytes(wav_bytes: bytes) -> None:
  """Validates 48kHz 24-bit stereo WAV integrity, clipping, and edges.

  Args:
    wav_bytes: Complete RIFF WAV bytes.

  Raises:
    ValueError: If format, duration, RMS, clipping, or edge fades fail.
  """
  with wave.open(io.BytesIO(wav_bytes), "rb") as wav_in:
    if wav_in.getnchannels() != 2:
      raise ValueError(f"Expected stereo WAV, got {wav_in.getnchannels()} ch.")
    if wav_in.getsampwidth() != 3:
      raise ValueError(
          f"Expected 24-bit WAV (sampwidth=3), got {wav_in.getsampwidth()}."
      )
    if wav_in.getframerate() != 48000:
      raise ValueError(
          f"Expected 48000 Hz WAV, got {wav_in.getframerate()} Hz."
      )
    num_frames = wav_in.getnframes()
    duration_s = num_frames / 48000.0
    if not 0.20 <= duration_s <= 2.0:
      raise ValueError(f"Unexpected SFX duration: {duration_s:.3f}s")
    raw = wav_in.readframes(num_frames)

  scale = 1.0 / 8388607.0
  sq_sum = 0.0
  peak = 0.0
  diff_energy = 0.0
  first_l = 0.0
  last_l = 0.0

  for i in range(num_frames):
    offset = i * 6
    sl = int.from_bytes(raw[offset : offset + 3], "little", signed=True) * scale
    sr = (
        int.from_bytes(raw[offset + 3 : offset + 6], "little", signed=True)
        * scale
    )
    if i == 0:
      first_l = abs(sl)
    if i == num_frames - 1:
      last_l = abs(sl)
    sq_sum += 0.5 * (sl * sl + sr * sr)
    peak = max(peak, abs(sl), abs(sr))
    diff_energy += abs(sl - sr)

  rms = math.sqrt(sq_sum / max(1, num_frames))
  if rms <= 0.01:
    raise ValueError(f"Audio is near-silent: rms={rms:.5f}")
  if peak >= 0.995:
    raise ValueError(f"Audio clips near full scale: peak={peak:.5f}")
  if diff_energy / max(1, num_frames) < 1e-4:
    raise ValueError("Left and right channels are effectively identical mono.")
  if first_l > 0.005 or last_l > 0.005:
    raise ValueError(
        f"Missing smooth zero-crossing fade at edges: {first_l}, {last_l}"
    )

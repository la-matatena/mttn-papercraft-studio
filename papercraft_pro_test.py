"""Tests for PaperCraft Studio Pro OpenFX bundle, installers, .drfx, and SFX.

Run from the repository root (also run by `.github/workflows/ci-release.yml`):

  python3 -B papercraft_pro_test.py -v

Uses absltest when available and falls back to `unittest` with a small shim
for the absltest-only helpers. `PapercraftReleaseTest` builds one full
release per class in a temp directory and checks it in focused tests: output
paths, ZIP members and file modes, relative Markdown links inside the Complete
Repo ZIP, and the extracted Linux installer. A separate test checks those docs
plus `AGENTS.md` on disk, including `#anchor` targets. The installer tests
need `/bin/bash`.
"""

import array
import io
import math
import os
import re
import struct
import subprocess
import tempfile
import types
import unittest
from unittest import mock
import wave
import zipfile

try:
  from absl.testing import absltest  # type: ignore[import-not-found]
except ImportError:

  class _FallbackTestCase(unittest.TestCase):
    """unittest.TestCase with the absltest helpers these tests use."""

    def assertLen(self, container, expected_len, msg=None):
      """Asserts `len(container) == expected_len`."""
      self.assertEqual(len(container), expected_len, msg=msg)

    def assertNotEmpty(self, container, msg=None):
      """Asserts that `container` has at least one element."""
      self.assertGreater(len(container), 0, msg=msg)

    def create_tempdir(self):
      """Returns an object whose `full_path` is a self-cleaning temp dir."""
      td = tempfile.TemporaryDirectory()
      self.addCleanup(td.cleanup)
      return types.SimpleNamespace(full_path=td.name)

  absltest = types.SimpleNamespace(TestCase=_FallbackTestCase, main=unittest.main)

try:
  # Internal monorepo import path; falls back to sibling modules when run
  # from a plain checkout.
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import bundle_builder_lib
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import drfx_validator
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import fusion_templates
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import png_renderer
except ImportError:
  import bundle_builder_lib  # type: ignore[no-redef]
  import drfx_validator  # type: ignore[no-redef]
  import fusion_templates  # type: ignore[no-redef]
  import png_renderer  # type: ignore[no-redef]


def _eval_simple_expr(expr, macro, t, render_end, **ctrl_vals):
  """Evaluates a Fusion SimpleExpression as Python at frame `t`.

  The SimpleExpressions under test only use `iif`, `min`, `max`, `and`, and
  attribute access, a subset Python can run once those names are provided.

  Args:
    expr: SimpleExpression string.
    macro: Macro name the expression reads controls from.
    t: Current frame (`time`).
    render_end: `comp.RenderEnd`; `comp.RenderStart` is 0.
    **ctrl_vals: Macro control values, e.g. `IntroFrames=10`.

  Returns:
    The evaluated value.
  """
  env = {
      "__builtins__": {},
      "iif": lambda c, a, b: a if c else b,
      "min": min,
      "max": max,
      "comp": types.SimpleNamespace(RenderStart=0, RenderEnd=render_end),
      "time": t,
      macro: types.SimpleNamespace(**ctrl_vals),
  }
  return eval(expr, env)  # pylint: disable=eval-used


def _github_slugs(md_text):
  """Returns the set of GitHub heading anchors for Markdown text.

  Mirrors GitHub's rules: lowercase, drop punctuation except `-` and `_`,
  spaces become `-`, and repeated headings get `-1`, `-2`, ... suffixes.
  Headings inside fenced code blocks are ignored.

  Args:
    md_text: Markdown source.

  Returns:
    Anchor slugs (without the leading `#`).
  """
  slugs = set()
  seen: dict[str, int] = {}
  in_fence = False
  for line in md_text.splitlines():
    if line.lstrip().startswith(("```", "~~~")):
      in_fence = not in_fence
      continue
    if in_fence:
      continue
    match = re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
    if not match:
      continue
    # Keep only the visible text of inline links/images.
    title = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", match.group(1))
    slug = re.sub(r"[^\w\- ]", "", title.lower()).replace(" ", "-")
    count = seen.get(slug, 0)
    seen[slug] = count + 1
    slugs.add(slug if count == 0 else f"{slug}-{count}")
  return slugs


class PapercraftProTest(absltest.TestCase):
  """End-to-end and unit checks for the release builder and validators."""

  def test_png_thumbnail(self):
    """Both thumbnail variants are valid 104x58 PNGs and differ."""
    for variant in ("effect", "fold"):
      with self.subTest(variant=variant):
        thumb = png_renderer.render_thumbnail_png((232, 93, 56), variant)
        self.assertTrue(thumb.startswith(b"\x89PNG\r\n\x1a\n"))
        w, h = struct.unpack(">II", thumb[16:24])
        self.assertEqual((w, h), (104, 58))
    self.assertNotEqual(
        png_renderer.render_thumbnail_png((232, 93, 56), "effect"),
        png_renderer.render_thumbnail_png((232, 93, 56), "fold"),
    )

  def test_effect_fusion_template_validates_as_macro_operator(self):
    """The effect template is a MacroOperator and passes the validator."""
    text = fusion_templates.build_effect_setting()
    self.assertIn("= MacroOperator", text)
    self.assertNotIn("= GroupOperator", text)
    self.assertNotIn("ViewInfo = GroupInfo", text)
    drfx_validator.validate_setting_syntax_and_graph(text, "effect")

  def test_fold_transition_fusion_template_validates(self):
    """The fold transition validates and kind/control errors are caught."""
    text = fusion_templates.build_fold_transition_setting()
    self.assertIn("PaperFoldTransition = MacroOperator", text)
    self.assertIn("MainInput2 = InstanceInput", text)
    # Each fold panel is masked, given a paper back, and swung on a DVE.
    for node in ("LeftPanelMask", "LeftBack", "LeftSwing", "RightSwing",
                 "PacketShadow", "ExactBoundaryClamp"):
      self.assertIn(f"\t\t\t\t{node} = ", text)
    self.assertIn('Source = "Mask"', text)
    drfx_validator.validate_setting_syntax_and_graph(text, "transition")

    # The validator must tell transitions and effects apart.
    with self.assertRaisesRegex(ValueError, "MainInput1 only"):
      drfx_validator.validate_setting_syntax_and_graph(text, "effect")
    one_input = text.replace("MainInput2 = InstanceInput", "Spare = Input")
    with self.assertRaisesRegex(ValueError, "MainInput1 and 2"):
      drfx_validator.validate_setting_syntax_and_graph(one_input, "transition")
    bad_ref = text.replace(
        "PaperFoldTransition.SlideAngle", "PaperFoldTransition.NoSuchControl"
    )
    with self.assertRaisesRegex(ValueError, "undeclared control"):
      drfx_validator.validate_setting_syntax_and_graph(bad_ref, "transition")

  def test_drfx_ships_effect_and_fold_transition(self):
    """The .drfx holds exactly the effect and transition with thumbnails."""
    entries = bundle_builder_lib.build_drfx_entries()
    self.assertEqual(
        sorted(entries),
        [
            "Edit/Effects/MTTN PaperCraft Studio/Paper Cutout FX.png",
            "Edit/Effects/MTTN PaperCraft Studio/Paper Cutout FX.setting",
            "Edit/Transitions/MTTN PaperCraft Studio/Paper Fold & Unfold.png",
            "Edit/Transitions/MTTN PaperCraft Studio/"
            "Paper Fold & Unfold.setting",
        ],
    )
    drfx_bytes = bundle_builder_lib._write_deterministic_zip(entries)  # pylint: disable=protected-access
    drfx_validator.validate_drfx_bytes(drfx_bytes)

    effect_only = {k: v for k, v in entries.items() if "/Effects/" in k}
    with self.assertRaisesRegex(ValueError, "Edit/Transitions"):
      drfx_validator.validate_drfx_bytes(
          bundle_builder_lib._write_deterministic_zip(effect_only)  # pylint: disable=protected-access
      )

  def test_validator_rejects_broken_or_group_operator_setting(self):
    """Each class of broken .setting edit raises a specific ValueError."""
    valid = fusion_templates.build_effect_setting()
    group_op = valid.replace("= MacroOperator", "= GroupOperator")
    with self.assertRaisesRegex(ValueError, "MacroOperator"):
      drfx_validator.validate_setting_syntax_and_graph(group_op, "effect")

    group_info = valid.replace(
        "ViewInfo = OperatorInfo { Pos = { 0, 0 } }",
        "ViewInfo = GroupInfo { Pos = { 0, 0 } }",
    )
    with self.assertRaisesRegex(ValueError, "OperatorInfo"):
      drfx_validator.validate_setting_syntax_and_graph(group_info, "effect")

    dangling = valid.replace('SourceOp = "ChromaKeyer"', 'SourceOp = "Ghost"')
    with self.assertRaisesRegex(ValueError, "Dangling SourceOp"):
      drfx_validator.validate_setting_syntax_and_graph(dangling, "effect")

    bad_expr = valid.replace(
        "PaperCutoutFX.KeyGain", "PaperCutoutFX.NoSuchControl"
    )
    with self.assertRaisesRegex(ValueError, "undeclared control"):
      drfx_validator.validate_setting_syntax_and_graph(bad_expr, "effect")

    bad_count = valid.replace("LBLC_NumInputs = 4,", "LBLC_NumInputs = 99,", 1)
    with self.assertRaisesRegex(ValueError, "LBLC_NumInputs=99"):
      drfx_validator.validate_setting_syntax_and_graph(bad_count, "effect")

    bad_op = valid.replace(" and ", " && ", 1)
    with self.assertRaisesRegex(ValueError, "Illegal C-style"):
      drfx_validator.validate_setting_syntax_and_graph(bad_op, "effect")

  def test_bundled_paper_sfx_wavs(self):
    """The four bundled Foley WAVs and credits exist and pass validation."""
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    sfx_map = bundle_builder_lib.build_sfx_entries(pkg_dir)
    wav_paths = {p for p in sfx_map if p.endswith(".wav")}
    self.assertEqual(
        wav_paths,
        {
            "Paper_SFX/01_Paper_Unfold.wav",
            "Paper_SFX/02_Paper_Crumple.wav",
            "Paper_SFX/03_Paper_Glide_Flip.wav",
            "Paper_SFX/04_Paper_Rip_Tear.wav",
        },
    )
    self.assertIn("Paper_SFX/CREDITS.md", sfx_map)
    for rel_path in sorted(wav_paths):
      with self.subTest(wav=rel_path):
        drfx_validator.validate_wav_bytes(sfx_map[rel_path])

  def test_installers_syntax_and_dry_run(self):
    """Installer scripts parse with `bash -n` and support `--dry-run`."""
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    for script in (
        "Install_PaperCraft_OpenFX_macOS.command",
        "install_linux_ofx.sh",
    ):
      path = os.path.join(pkg_dir, "ofx", script)
      subprocess.run(["/bin/bash", "-n", path], check=True)
      proc = subprocess.run(
          ["/bin/bash", path, "--user", "--dry-run"],
          check=True,
          capture_output=True,
          text=True,
      )
      self.assertIn("[dry-run]", proc.stdout)
      self.assertIn("PaperCraftStudioPro.ofx.bundle", proc.stdout)

    uninstall_path = os.path.join(
        pkg_dir, "ofx", "Uninstall_PaperCraft_OpenFX_macOS.command"
    )
    subprocess.run(["/bin/bash", "-n", uninstall_path], check=True)
    uninstall_proc = subprocess.run(
        ["/bin/bash", uninstall_path, "--dry-run"],
        check=True,
        capture_output=True,
        text=True,
    )
    self.assertTrue(
        "[dry-run]" in uninstall_proc.stdout
        or "Nothing to remove." in uninstall_proc.stdout
    )

  def test_validator_rejects_truncated_png_with_value_error(self):
    """Thumbnails shorter than the 24-byte IHDR header raise ValueError."""
    entries = bundle_builder_lib.build_drfx_entries()
    png_key = next(k for k in entries if k.endswith(".png"))
    for bad_len in (8, 16, 23):
      with self.subTest(bad_len=bad_len):
        broken = dict(entries)
        broken[png_key] = b"\x89PNG\r\n\x1a\n" + b"\x00" * (bad_len - 8)
        drfx_bytes = bundle_builder_lib._write_deterministic_zip(broken)  # pylint: disable=protected-access
        with self.assertRaisesRegex(ValueError, "Truncated PNG"):
          drfx_validator.validate_drfx_bytes(drfx_bytes)

  def test_active_trans_style_ignores_zero_intro_frames(self):
    """IntroFrames == 0 never selects IntroStyle, even at RenderStart."""
    macro = "PaperCutoutFX"
    prog_expr = fusion_templates._anim_progress_expr(macro)  # pylint: disable=protected-access
    style_expr = fusion_templates._active_trans_style_expr(macro)  # pylint: disable=protected-access
    ctrls = dict(IntroStyle=2, IntroFrames=0, OutroStyle=3, OutroFrames=10)
    for t, expected_prog in ((0, 1.0), (25, 0.5)):
      with self.subTest(t=t):
        self.assertEqual(
            _eval_simple_expr(style_expr, macro, t, 30, **ctrls), 3
        )
        self.assertAlmostEqual(
            _eval_simple_expr(prog_expr, macro, t, 30, **ctrls), expected_prog
        )

  def test_active_trans_style_matches_anim_progress(self):
    """With overlapping windows the style follows the active progress ramp."""
    macro = "PaperCutoutFX"
    prog_expr = fusion_templates._anim_progress_expr(macro)  # pylint: disable=protected-access
    style_expr = fusion_templates._active_trans_style_expr(macro)  # pylint: disable=protected-access
    # Overlapping intro/outro windows on a 10-frame comp.
    overlap_ctrls = dict(
        IntroStyle=1, IntroFrames=10, OutroStyle=4, OutroFrames=10
    )
    for t, expected_prog, expected_style in ((2, 0.2, 1), (5, 0.5, 1),
                                             (8, 0.2, 4)):
      with self.subTest(t=t):
        self.assertAlmostEqual(
            _eval_simple_expr(prog_expr, macro, t, 10, **overlap_ctrls),
            expected_prog,
        )
        self.assertEqual(
            _eval_simple_expr(style_expr, macro, t, 10, **overlap_ctrls),
            expected_style,
        )

  def test_deterministic_zip_pins_deflate_level(self):
    """Every ZIP entry is compressed with the pinned DEFLATE level 6."""
    real_get_compressor = zipfile._get_compressor  # pylint: disable=protected-access
    levels = []

    def recording_get_compressor(compress_type, compresslevel=None):
      """Records the level zipfile requests, then defers to the real one."""
      levels.append(compresslevel)
      return real_get_compressor(compress_type, compresslevel)

    with mock.patch.object(
        zipfile, "_get_compressor", side_effect=recording_get_compressor
    ):
      bundle_builder_lib._write_deterministic_zip(  # pylint: disable=protected-access
          {"b.txt": b"bbbb" * 64, "a.sh": b"#!/bin/sh\n"}
      )
    self.assertEqual(levels, [6, 6])
    self.assertEqual(bundle_builder_lib._ZIP_DEFLATE_LEVEL, 6)  # pylint: disable=protected-access

  def test_wav_validator_checks_every_channel_edge(self):
    """A click on the right channel only is rejected, as is short data."""

    def make_wav(first_right=0.0, truncate=0):
      """Builds a valid 0.5 s 48 kHz 24-bit stereo WAV, optionally broken.

      Args:
        first_right: Value (full-scale units) forced on the first right
          sample; nonzero simulates a click on the right channel only.
        truncate: Number of trailing PCM bytes to drop after writing.

      Returns:
        RIFF WAV bytes.
      """
      num_frames = 24000
      frames = bytearray()
      for i in range(num_frames):
        # Sine fade envelope: exactly 0 at the first and last frame.
        env = math.sin(math.pi * i / (num_frames - 1))
        left = 0.3 * env * math.sin(i * 0.05)
        right = 0.3 * env * math.sin(i * 0.07 + 1.0)
        if i == 0:
          right = first_right
        for val in (left, right):
          frames += int(round(val * 8388607)).to_bytes(
              3, "little", signed=True
          )
      buf = io.BytesIO()
      with wave.open(buf, "wb") as wav_out:
        wav_out.setnchannels(2)
        wav_out.setsampwidth(3)
        wav_out.setframerate(48000)
        wav_out.writeframes(bytes(frames))
      data = buf.getvalue()
      return data[: len(data) - truncate] if truncate else data

    drfx_validator.validate_wav_bytes(make_wav())
    with self.assertRaisesRegex(ValueError, "zero-crossing fade"):
      drfx_validator.validate_wav_bytes(make_wav(first_right=0.2))
    with self.assertRaisesRegex(ValueError, "Truncated"):
      drfx_validator.validate_wav_bytes(make_wav(truncate=6))

  def test_pcm24_decoder_matches_reference(self):
    """The vectorized 24-bit decoder equals int.from_bytes per sample."""
    values = [0, 1, -1, 8388607, -8388608, 123456, -654321]
    raw = b"".join(v.to_bytes(3, "little", signed=True) for v in values)
    decoded = drfx_validator._decode_pcm24_frames(raw)  # pylint: disable=protected-access
    self.assertIsInstance(decoded, array.array)
    self.assertEqual([s >> 8 for s in decoded], values)
    with self.assertRaisesRegex(ValueError, "Truncated"):
      drfx_validator._decode_pcm24_frames(raw[:-1])  # pylint: disable=protected-access

  def test_expression_quoting_guards(self):
    """`_expr` rejects quotes/empty input; the validator flags empty ones."""
    with self.assertRaisesRegex(ValueError, "must not contain"):
      fusion_templates._expr("Mix", "0", 'iif(x > 0, "a", 1)')  # pylint: disable=protected-access
    for blank in ("", "   "):
      with self.subTest(blank=blank):
        with self.assertRaisesRegex(ValueError, "Empty SimpleExpression"):
          fusion_templates._expr("Mix", "0", blank)  # pylint: disable=protected-access
    self.assertEqual(
        fusion_templates._expr("Mix", "0", "time"),  # pylint: disable=protected-access
        'Mix = Input { Value = 0, Expression = "time", },',
    )

    valid = fusion_templates.build_fold_transition_setting()
    empty_expr = re.sub(
        r'Expression = "[^"]+"', 'Expression = ""', valid, count=1
    )
    self.assertNotEqual(empty_expr, valid)
    with self.assertRaisesRegex(ValueError, "Empty SimpleExpression"):
      drfx_validator.validate_setting_syntax_and_graph(
          empty_expr, "transition"
      )

  def test_unknown_thumbnail_variant_raises(self):
    """An unknown thumbnail variant raises instead of drawing a blank card."""
    with self.assertRaisesRegex(ValueError, "Unknown thumbnail variant"):
      png_renderer.render_thumbnail_png((1, 2, 3), "efect")

  def test_validator_rejects_removed_generator_kind(self):
    """The removed 'generator' template kind is no longer accepted."""
    text = fusion_templates.build_effect_setting()
    with self.assertRaisesRegex(ValueError, "Unsupported template_kind"):
      drfx_validator.validate_setting_syntax_and_graph(text, "generator")

  def test_github_slug_rules(self):
    """Heading slugs drop punctuation, number repeats, and skip fences."""
    self.assertEqual(
        _github_slugs(
            "## What's next\n"
            "### Python `.drfx`, Foley WAV, installer, and link suite\n"
            "```\n# not a heading\n```\n"
            "## What's next\n"
        ),
        {
            "whats-next",
            "python-drfx-foley-wav-installer-and-link-suite",
            "whats-next-1",
        },
    )

  def test_markdown_links_and_anchors_resolve(self):
    """Relative links in docs and AGENTS.md hit real files and headings.

    `#anchor` fragments must match a heading slug from `_github_slugs`
    (rules covered by `test_github_slug_rules`). Each Markdown file is its
    own subTest so a failure names the file.
    """
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    slug_cache: dict[str, set[str]] = {}
    for md_rel in (
        "AGENTS.md",
        "README.md",
        "README.en.md",
        "Paper_SFX/CREDITS.md",
        "docs/design_doc.md",
        "docs/user_guide.md",
        "docs/developer_guide.md",
    ):
      with self.subTest(md=md_rel):
        broken = []
        md_path = os.path.join(pkg_dir, md_rel)
        with open(md_path, "r", encoding="utf-8") as f:
          md_text = f.read()
        for target in re.findall(r"\[[^\]]+\]\(([^)\s]+)[^)]*\)", md_text):
          if target.startswith(("http://", "https://", "mailto:")):
            continue
          path_part, _, anchor = target.partition("#")
          md_dir = os.path.dirname(md_path)
          resolved = (
              os.path.normpath(os.path.join(md_dir, path_part))
              if path_part
              else md_path
          )
          if not os.path.exists(resolved):
            broken.append(f"{md_rel}: missing file in {target!r}")
            continue
          if not anchor:
            continue
          if not resolved.endswith(".md"):
            broken.append(f"{md_rel}: anchor on non-Markdown {target!r}")
            continue
          if resolved not in slug_cache:
            with open(resolved, "r", encoding="utf-8") as f:
              slug_cache[resolved] = _github_slugs(f.read())
          if anchor not in slug_cache[resolved]:
            broken.append(f"{md_rel}: no heading for anchor in {target!r}")
        self.assertEqual(broken, [])


class PapercraftReleaseTest(absltest.TestCase):
  """Focused checks on one full release, built once per class.

  `setUpClass` builds the release into a class-wide temp directory (removed in
  `tearDownClass`) with a mock Linux `.ofx` when no compiled binary exists.
  The build runs with spies on `build_doc_entries` and `build_sfx_entries` so
  the read-once check reuses it instead of building a second release. Tests
  must treat the release as read-only; the installer test writes outside it.

  Attributes:
    scratch_dir: Class temp dir holding the release and installer targets.
    out: Paths returned by `build_and_write_release`.
    doc_reads: `build_doc_entries` call count during the build.
    sfx_reads: `build_sfx_entries` call count during the build.
  """

  @classmethod
  def setUpClass(cls):
    """Builds the shared release; cleans up the temp dir if that fails."""
    super().setUpClass()
    # Class-level temp dir: absltest's create_tempdir and the fallback shim
    # are per-test, so the shared build manages its own directory.
    cls._tmp = tempfile.TemporaryDirectory()
    try:
      cls.scratch_dir = cls._tmp.name
      pkg_dir = os.path.dirname(os.path.abspath(__file__))
      linux_ofx_override = None
      if not os.path.isfile(os.path.join(pkg_dir, "PaperCraftStudioPro.ofx")):
        linux_ofx_override = os.path.join(
            cls.scratch_dir, "PaperCraftStudioPro.ofx"
        )
        with open(linux_ofx_override, "wb") as f:
          f.write(b"\x7fELF-mock-ofx-binary")
      with mock.patch.object(
          bundle_builder_lib,
          "build_doc_entries",
          wraps=bundle_builder_lib.build_doc_entries,
      ) as doc_spy, mock.patch.object(
          bundle_builder_lib,
          "build_sfx_entries",
          wraps=bundle_builder_lib.build_sfx_entries,
      ) as sfx_spy:
        cls.out = bundle_builder_lib.build_and_write_release(
            output_dir=os.path.join(cls.scratch_dir, "out"),
            export_unpacked=True,
            linux_ofx_path=linux_ofx_override,
        )
      cls.doc_reads = doc_spy.call_count
      cls.sfx_reads = sfx_spy.call_count
    except BaseException:
      # tearDownClass does not run when setUpClass raises.
      cls._tmp.cleanup()
      raise

  @classmethod
  def tearDownClass(cls):
    """Removes the shared release temp dir."""
    cls._tmp.cleanup()
    super().tearDownClass()

  def test_release_writes_every_output_path(self):
    """Every always-written output path exists with the right kind."""
    for key, exists in (
        ("drfx", os.path.isfile),
        ("full_zip", os.path.isfile),
        ("repo_zip", os.path.isfile),
        ("ofx_bundle", os.path.isdir),
        ("docs_dir", os.path.isdir),
        ("macos_installer", os.path.isfile),
        ("linux_installer", os.path.isfile),
    ):
      with self.subTest(key=key):
        self.assertTrue(exists(self.out[key]))

  def test_written_drfx_validates(self):
    """The .drfx written to disk passes the structural validator."""
    with open(self.out["drfx"], "rb") as f:
      drfx_validator.validate_drfx_bytes(f.read())

  def test_full_zip_ships_drfx_license_and_docs(self):
    """The Full Bundle ZIP carries the .drfx, LICENSE, README, and docs."""
    with zipfile.ZipFile(self.out["full_zip"], "r") as zf:
      names = set(zf.namelist())
    self.assertIn("MTTN_PaperCraft_Studio.drfx", names)
    self.assertIn("LICENSE", names)
    self.assertIn("README.md", names)
    self.assertIn("docs/design_doc.md", names)
    self.assertIn("docs/user_guide.md", names)
    self.assertIn("docs/developer_guide.md", names)
    self.assertIn("docs/images/paper_fold_unfold.png", names)

  def test_full_zip_docs_image_is_png(self):
    """The docs image inside the Full Bundle ZIP is a real PNG."""
    with zipfile.ZipFile(self.out["full_zip"], "r") as zf:
      self.assertTrue(
          zf.read("docs/images/paper_fold_unfold.png").startswith(
              b"\x89PNG\r\n\x1a\n"
          )
      )

  def test_full_zip_ships_ofx_bundle_installers_and_source(self):
    """The Full Bundle ZIP has the installers, OFX bundle, and source."""
    with zipfile.ZipFile(self.out["full_zip"], "r") as zf:
      names = set(zf.namelist())
    self.assertIn("Install_PaperCraft_OpenFX_macOS.command", names)
    self.assertIn("Uninstall_PaperCraft_OpenFX_macOS.command", names)
    self.assertIn("install_linux_ofx.sh", names)
    self.assertIn("PaperCraftStudioPro.ofx.bundle/Contents/Info.plist", names)
    self.assertIn(
        "PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
        names,
    )
    self.assertIn("OpenFX_Source/papercraft_ofx_plugin.cc", names)
    self.assertIn("OpenFX_Source/papercraft_kernel_internal.h", names)
    self.assertIn("OpenFX_Source/openfx_include/ofxImageEffect.h", names)

  def test_full_zip_ships_four_wavs_and_credits(self):
    """The Full Bundle ZIP has the four Foley WAVs and their credits."""
    with zipfile.ZipFile(self.out["full_zip"], "r") as zf:
      names = set(zf.namelist())
    wav_members = [
        n for n in names if n.startswith("Paper_SFX/") and n.endswith(".wav")
    ]
    self.assertLen(wav_members, 4)
    self.assertIn("Paper_SFX/CREDITS.md", names)

  def test_full_zip_executables_are_0755(self):
    """Installers and the OFX binary in the Full Bundle ZIP are 0o755."""
    with zipfile.ZipFile(self.out["full_zip"], "r") as zf:
      for exe_member in (
          "Install_PaperCraft_OpenFX_macOS.command",
          "Uninstall_PaperCraft_OpenFX_macOS.command",
          "install_linux_ofx.sh",
          "PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
      ):
        with self.subTest(member=exe_member):
          mode = (zf.getinfo(exe_member).external_attr >> 16) & 0o777
          self.assertEqual(mode, 0o755, msg=f"Bad permissions on {exe_member}")

  def test_repo_zip_has_expected_nonempty_entries(self):
    """The Complete Repo ZIP holds every source/release file, non-empty."""
    expected_repo_entries = (
        "papercraft_pro/BUILD",
        "papercraft_pro/LICENSE",
        "papercraft_pro/README.md",
        "papercraft_pro/docs/design_doc.md",
        "papercraft_pro/docs/user_guide.md",
        "papercraft_pro/docs/developer_guide.md",
        "papercraft_pro/docs/images/paper_fold_unfold.png",
        "papercraft_pro/build_papercraft_bundle.py",
        "papercraft_pro/bundle_builder_lib.py",
        "papercraft_pro/drfx_validator.py",
        "papercraft_pro/fusion_templates.py",
        "papercraft_pro/papercraft_pro_test.py",
        "papercraft_pro/png_renderer.py",
        "papercraft_pro/Paper_SFX/CREDITS.md",
        "papercraft_pro/ofx/Info.plist",
        "papercraft_pro/ofx/openfx_sym.lds",
        "papercraft_pro/ofx/papercraft_kernel.h",
        "papercraft_pro/ofx/papercraft_kernel.cc",
        "papercraft_pro/ofx/papercraft_ofx_plugin.cc",
        "papercraft_pro/ofx/papercraft_ofx_test.cc",
        "papercraft_pro/ofx/papercraft_kernel_internal.h",
        "papercraft_pro/ofx/papercraft_kernel_test.cc",
        "papercraft_pro/ofx/run_cpp_tests.sh",
        "papercraft_pro/ofx/testing/gtest_lite/gtest/gtest.h",
        "papercraft_pro/ofx/testing/gtest_lite/gtest_lite_main.cc",
        "papercraft_pro/ofx/Install_PaperCraft_OpenFX_macOS.command",
        "papercraft_pro/ofx/Uninstall_PaperCraft_OpenFX_macOS.command",
        "papercraft_pro/ofx/install_linux_ofx.sh",
        "papercraft_pro/ofx/openfx_include/ofxCore.h",
        "papercraft_pro/release/MTTN_PaperCraft_Studio.drfx",
        "papercraft_pro/release/PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
    )
    with zipfile.ZipFile(self.out["repo_zip"], "r") as rzf:
      rnames = set(rzf.namelist())
      for member in expected_repo_entries:
        with self.subTest(member=member):
          self.assertIn(member, rnames)
          self.assertNotEmpty(rzf.read(member), msg=f"Empty entry {member}")

  def test_repo_zip_ships_release_wavs_and_credits(self):
    """The Complete Repo ZIP release folder has four WAVs and credits."""
    with zipfile.ZipFile(self.out["repo_zip"], "r") as rzf:
      rnames = set(rzf.namelist())
    release_wavs = [
        n
        for n in rnames
        if n.startswith("papercraft_pro/release/Paper_SFX/")
        and n.endswith(".wav")
    ]
    self.assertLen(release_wavs, 4)
    self.assertIn("papercraft_pro/release/Paper_SFX/CREDITS.md", rnames)

  def test_repo_zip_executables_are_0755(self):
    """Installers and the OFX binary in the Complete Repo ZIP are 0o755."""
    with zipfile.ZipFile(self.out["repo_zip"], "r") as rzf:
      for exe_member in (
          "papercraft_pro/ofx/Install_PaperCraft_OpenFX_macOS.command",
          "papercraft_pro/ofx/Uninstall_PaperCraft_OpenFX_macOS.command",
          "papercraft_pro/ofx/install_linux_ofx.sh",
          "papercraft_pro/ofx/run_cpp_tests.sh",
          "papercraft_pro/release/Install_PaperCraft_OpenFX_macOS.command",
          "papercraft_pro/release/Uninstall_PaperCraft_OpenFX_macOS.command",
          "papercraft_pro/release/install_linux_ofx.sh",
          "papercraft_pro/release/PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
      ):
        with self.subTest(member=exe_member):
          mode = (rzf.getinfo(exe_member).external_attr >> 16) & 0o777
          self.assertEqual(mode, 0o755, msg=f"Bad permissions on {exe_member}")

  def test_repo_zip_markdown_links_resolve(self):
    """Relative Markdown links in the Complete Repo ZIP hit ZIP members."""
    with zipfile.ZipFile(self.out["repo_zip"], "r") as rzf:
      rnames = set(rzf.namelist())
      # Only the path is checked here; `#anchor` parts are checked against
      # headings by `test_markdown_links_and_anchors_resolve`.
      for md_entry in (
          "papercraft_pro/README.md",
          "papercraft_pro/README.en.md",
          "papercraft_pro/Paper_SFX/CREDITS.md",
          "papercraft_pro/docs/design_doc.md",
          "papercraft_pro/docs/user_guide.md",
          "papercraft_pro/docs/developer_guide.md",
      ):
        with self.subTest(md=md_entry):
          md_text = rzf.read(md_entry).decode("utf-8")
          md_dir = os.path.dirname(md_entry)
          for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", md_text):
            if target.startswith(("http://", "https://", "mailto:", "#")):
              continue
            clean_target = target.split("#", 1)[0]
            resolved = os.path.normpath(os.path.join(md_dir, clean_target))
            self.assertIn(
                resolved,
                rnames,
                msg=f"Broken relative link {target!r} in {md_entry}",
            )

  def test_linux_installer_creates_missing_dest_parents(self):
    """The extracted Linux installer creates missing `--dest` parents."""
    # Installed outside the release dir so other tests see it unchanged.
    nested_dest = os.path.join(
        self.scratch_dir, "nested_missing_parent", "OFX", "Plugins"
    )
    subprocess.run(
        ["/bin/bash", self.out["linux_installer"], "--dest", nested_dest],
        check=True,
    )
    self.assertTrue(
        os.path.isfile(
            os.path.join(
                nested_dest,
                "PaperCraftStudioPro.ofx.bundle",
                "Contents",
                "Linux-x86-64",
                "PaperCraftStudioPro.ofx",
            )
        )
    )

  def test_release_lists_license_and_reads_docs_once(self):
    """LICENSE is in the returned paths and docs/SFX are read only once."""
    self.assertEqual(self.doc_reads, 1)
    self.assertEqual(self.sfx_reads, 1)
    self.assertIn("license", self.out)
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    with open(self.out["license"], "rb") as got, open(
        os.path.join(pkg_dir, "LICENSE"), "rb"
    ) as want:
      self.assertEqual(got.read(), want.read())
    # The repo ZIP still carries the docs and SFX that were read once.
    with zipfile.ZipFile(self.out["repo_zip"], "r") as rzf:
      rnames = set(rzf.namelist())
    self.assertIn("papercraft_pro/docs/user_guide.md", rnames)
    self.assertIn("papercraft_pro/Paper_SFX/01_Paper_Unfold.wav", rnames)


if __name__ == "__main__":
  absltest.main()

"""Tests for PaperCraft Studio Pro OpenFX bundle, installers, .drfx, and SFX."""

import os
import re
import struct
import subprocess
import tempfile
import types
import unittest
import zipfile

try:
  from absl.testing import absltest  # type: ignore[import-not-found]
except ImportError:

  class _FallbackTestCase(unittest.TestCase):

    def assertLen(self, container, expected_len, msg=None):
      self.assertEqual(len(container), expected_len, msg=msg)

    def assertNotEmpty(self, container, msg=None):
      self.assertGreater(len(container), 0, msg=msg)

    def create_tempdir(self):
      td = tempfile.TemporaryDirectory()
      self.addCleanup(td.cleanup)
      return types.SimpleNamespace(full_path=td.name)

  absltest = types.SimpleNamespace(TestCase=_FallbackTestCase, main=unittest.main)

try:
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import bundle_builder_lib
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import drfx_validator
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import fusion_templates
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import png_renderer
except ImportError:
  import bundle_builder_lib  # type: ignore[no-redef]
  import drfx_validator  # type: ignore[no-redef]
  import fusion_templates  # type: ignore[no-redef]
  import png_renderer  # type: ignore[no-redef]


class PapercraftProTest(absltest.TestCase):

  def test_png_thumbnail(self):
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
    text = fusion_templates.build_effect_setting()
    self.assertIn("= MacroOperator", text)
    self.assertNotIn("= GroupOperator", text)
    self.assertNotIn("ViewInfo = GroupInfo", text)
    drfx_validator.validate_setting_syntax_and_graph(text, "effect")

  def test_fold_transition_fusion_template_validates(self):
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

  def test_end_to_end_bundle_creation(self):
    scratch_dir = self.create_tempdir().full_path
    tmp_dir = os.path.join(scratch_dir, "out")
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    default_ofx = os.path.join(pkg_dir, "PaperCraftStudioPro.ofx")
    linux_ofx_override = None
    if not os.path.isfile(default_ofx):
      linux_ofx_override = os.path.join(scratch_dir, "PaperCraftStudioPro.ofx")
      with open(linux_ofx_override, "wb") as f:
        f.write(b"\x7fELF-mock-ofx-binary")
    out = bundle_builder_lib.build_and_write_release(
        output_dir=tmp_dir,
        export_unpacked=True,
        linux_ofx_path=linux_ofx_override,
    )
    self.assertTrue(os.path.isfile(out["drfx"]))
    self.assertTrue(os.path.isfile(out["full_zip"]))
    self.assertTrue(os.path.isfile(out["repo_zip"]))
    self.assertTrue(os.path.isdir(out["ofx_bundle"]))
    self.assertTrue(os.path.isdir(out["docs_dir"]))
    self.assertTrue(os.path.isfile(out["macos_installer"]))
    self.assertTrue(os.path.isfile(out["linux_installer"]))

    with open(out["drfx"], "rb") as f:
      drfx_validator.validate_drfx_bytes(f.read())

    with zipfile.ZipFile(out["full_zip"], "r") as zf:
      names = set(zf.namelist())
      self.assertIn("PaperCraft_Studio_Pro.drfx", names)
      self.assertIn("LICENSE", names)
      self.assertIn("README.md", names)
      self.assertIn("docs/design_doc.md", names)
      self.assertIn("docs/user_guide.md", names)
      self.assertIn("docs/developer_guide.md", names)
      self.assertIn("docs/images/paper_fold_unfold.png", names)
      self.assertTrue(
          zf.read("docs/images/paper_fold_unfold.png").startswith(
              b"\x89PNG\r\n\x1a\n"
          )
      )
      self.assertIn("Install_PaperCraft_OpenFX_macOS.command", names)
      self.assertIn("install_linux_ofx.sh", names)
      self.assertIn("PaperCraftStudioPro.ofx.bundle/Contents/Info.plist", names)
      self.assertIn(
          "PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
          names,
      )
      self.assertIn("OpenFX_Source/papercraft_ofx_plugin.cc", names)
      self.assertIn("OpenFX_Source/openfx_include/ofxImageEffect.h", names)
      wav_members = [
          n for n in names if n.startswith("Paper_SFX/") and n.endswith(".wav")
      ]
      self.assertLen(wav_members, 4)
      self.assertIn("Paper_SFX/CREDITS.md", names)

      for exe_member in (
          "Install_PaperCraft_OpenFX_macOS.command",
          "install_linux_ofx.sh",
          "PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
      ):
        mode = (zf.getinfo(exe_member).external_attr >> 16) & 0o777
        self.assertEqual(mode, 0o755, msg=f"Bad permissions on {exe_member}")

    with zipfile.ZipFile(out["repo_zip"], "r") as rzf:
      rnames = set(rzf.namelist())
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
          "papercraft_pro/ofx/Install_PaperCraft_OpenFX_macOS.command",
          "papercraft_pro/ofx/install_linux_ofx.sh",
          "papercraft_pro/ofx/openfx_include/ofxCore.h",
          "papercraft_pro/release/PaperCraft_Studio_Pro.drfx",
          "papercraft_pro/release/PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
      )
      for member in expected_repo_entries:
        self.assertIn(member, rnames)
        self.assertNotEmpty(rzf.read(member), msg=f"Empty entry {member}")

      release_wavs = [
          n
          for n in rnames
          if n.startswith("papercraft_pro/release/Paper_SFX/")
          and n.endswith(".wav")
      ]
      self.assertLen(release_wavs, 4)
      self.assertIn("papercraft_pro/release/Paper_SFX/CREDITS.md", rnames)

      for exe_member in (
          "papercraft_pro/ofx/Install_PaperCraft_OpenFX_macOS.command",
          "papercraft_pro/ofx/install_linux_ofx.sh",
          "papercraft_pro/release/Install_PaperCraft_OpenFX_macOS.command",
          "papercraft_pro/release/install_linux_ofx.sh",
          "papercraft_pro/release/PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx",
      ):
        mode = (rzf.getinfo(exe_member).external_attr >> 16) & 0o777
        self.assertEqual(mode, 0o755, msg=f"Bad permissions on {exe_member}")

      for md_entry in (
          "papercraft_pro/README.md",
          "papercraft_pro/README.en.md",
          "papercraft_pro/Paper_SFX/CREDITS.md",
          "papercraft_pro/docs/design_doc.md",
          "papercraft_pro/docs/user_guide.md",
          "papercraft_pro/docs/developer_guide.md",
      ):
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

    nested_dest = os.path.join(
        tmp_dir, "nested_missing_parent", "OFX", "Plugins"
    )
    subprocess.run(
        ["/bin/bash", out["linux_installer"], "--dest", nested_dest],
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


if __name__ == "__main__":
  absltest.main()

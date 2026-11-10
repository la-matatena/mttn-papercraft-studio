"""Bundle assembler for PaperCraft OpenFX, installers, .drfx, and Paper SFX."""

from collections.abc import Mapping
import io
import os
import stat
import zipfile

try:
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import drfx_validator
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import fusion_templates
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import png_renderer
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import sfx_synthesizer
except ImportError:
  import drfx_validator  # type: ignore[no-redef]
  import fusion_templates  # type: ignore[no-redef]
  import png_renderer  # type: ignore[no-redef]
  import sfx_synthesizer  # type: ignore[no-redef]

_ZIP_TIMESTAMP = (2026, 1, 1, 0, 0, 0)

_OFX_SOURCE_RELPATHS = (
    "Info.plist",
    "openfx_sym.lds",
    "papercraft_kernel.h",
    "papercraft_kernel.cc",
    "papercraft_ofx_plugin.cc",
    "openfx_include/LICENSE",
    "openfx_include/ofxCore.h",
    "openfx_include/ofxImageEffect.h",
    "openfx_include/ofxInteract.h",
    "openfx_include/ofxMemory.h",
    "openfx_include/ofxMessage.h",
    "openfx_include/ofxMultiThread.h",
    "openfx_include/ofxParam.h",
    "openfx_include/ofxPixels.h",
    "openfx_include/ofxProperty.h",
)

_INSTALLER_NAMES = (
    "Install_PaperCraft_OpenFX_macOS.command",
    "Uninstall_PaperCraft_OpenFX_macOS.command",
    "install_linux_ofx.sh",
)

_DOC_RELPATHS = (
    "docs/design_doc.md",
    "docs/developer_guide.md",
    "docs/user_guide.md",
)

_REPO_ROOT_FILES = (
    "BUILD",
    "LICENSE",
    "README.en.md",
    "README.md",
    "build_papercraft_bundle.py",
    "bundle_builder_lib.py",
    "drfx_validator.py",
    "fusion_templates.py",
    "papercraft_pro_test.py",
    "png_renderer.py",
    "sfx_synthesizer.py",
)

_REPO_EXTRA_OFX_FILES = (
    "Install_PaperCraft_OpenFX_macOS.command",
    "Uninstall_PaperCraft_OpenFX_macOS.command",
    "install_linux_ofx.sh",
    "papercraft_ofx_test.cc",
)


def _is_executable_entry(rel_path: str) -> bool:
  """Returns whether a packaged relative path requires 0o755 UNIX mode."""
  return rel_path.endswith((".command", ".sh", ".ofx"))


def _write_deterministic_zip(files: Mapping[str, bytes]) -> bytes:
  """Packages relative archive paths into a deterministic ZIP byte buffer."""
  out = io.BytesIO()
  with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
    for rel_path in sorted(files):
      info = zipfile.ZipInfo(filename=rel_path, date_time=_ZIP_TIMESTAMP)
      info.compress_type = zipfile.ZIP_DEFLATED
      info.create_system = (
          3  # UNIX permissions preserved cleanly on macOS/Linux.
      )
      mode = (
          stat.S_IFREG | 0o755
          if _is_executable_entry(rel_path)
          else stat.S_IFREG | 0o644
      )
      info.external_attr = mode << 16
      zf.writestr(info, files[rel_path])
  return out.getvalue()


def _read_bytes(path: str) -> bytes:
  with open(path, "rb") as f:
    return f.read()


def _resolve_package_defaults(
    ofx_source_dir: str | None,
    linux_ofx_path: str | None,
    readme_text: str | None,
) -> tuple[str, str, str | None]:
  """Resolves default OpenFX source, compiled binary, and README paths."""
  pkg_dir = os.path.dirname(os.path.abspath(__file__))
  resolved_ofx_dir = ofx_source_dir or os.path.join(pkg_dir, "ofx")
  resolved_linux_ofx = linux_ofx_path or os.path.join(
      pkg_dir, "PaperCraftStudioPro.ofx"
  )
  resolved_readme = readme_text
  if resolved_readme is None:
    candidate_readme = os.path.join(pkg_dir, "README.md")
    if os.path.isfile(candidate_readme):
      with open(candidate_readme, "r", encoding="utf-8") as f:
        resolved_readme = f.read()
  return resolved_ofx_dir, resolved_linux_ofx, resolved_readme


def build_drfx_entries() -> dict[str, bytes]:
  """Creates the file tree contained inside PaperCraft_Studio_Pro.drfx."""
  fx_dir = "Edit/Effects/MTTN PaperCraft Studio"

  entries: dict[str, bytes] = {
      f"{fx_dir}/Paper Cutout FX.setting": (
          fusion_templates.build_effect_setting().encode("utf-8")
      ),
      f"{fx_dir}/Paper Cutout FX.png": png_renderer.render_thumbnail_png(
          (106, 79, 179), "effect"
      ),
  }
  return entries


def build_ofx_package_entries(
    ofx_source_dir: str,
    linux_ofx_path: str,
) -> dict[str, bytes]:
  """Builds native OpenFX bundle, source, and installer file map entries."""
  if not os.path.isfile(linux_ofx_path):
    raise FileNotFoundError(
        f"Compiled Linux OpenFX binary not found at {linux_ofx_path}"
    )

  entries: dict[str, bytes] = {
      "PaperCraftStudioPro.ofx.bundle/Contents/Info.plist": _read_bytes(
          os.path.join(ofx_source_dir, "Info.plist")
      ),
      "PaperCraftStudioPro.ofx.bundle/Contents/Resources/LICENSE": _read_bytes(
          os.path.join(ofx_source_dir, "openfx_include/LICENSE")
      ),
      "PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx": _read_bytes(
          linux_ofx_path
      ),
  }
  for name in _INSTALLER_NAMES:
    entries[name] = _read_bytes(os.path.join(ofx_source_dir, name))
  for rel in _OFX_SOURCE_RELPATHS:
    entries[f"OpenFX_Source/{rel}"] = _read_bytes(
        os.path.join(ofx_source_dir, rel)
    )
  return entries


def build_doc_entries(pkg_dir: str) -> dict[str, bytes]:
  """Reads all documentation files relative to `pkg_dir`."""
  entries: dict[str, bytes] = {}
  for rel in _DOC_RELPATHS:
    src_path = os.path.join(pkg_dir, rel)
    if not os.path.isfile(src_path):
      raise FileNotFoundError(f"Documentation file not found at {src_path}")
    entries[rel] = _read_bytes(src_path)
  return entries


def build_repo_source_entries(
    pkg_dir: str,
    ofx_source_dir: str,
    readme_override: str | None = None,
) -> dict[str, bytes]:
  """Collects source, test, build, and doc files for the complete repo ZIP."""
  entries: dict[str, bytes] = {}
  for rel in _REPO_ROOT_FILES:
    if rel == "README.md" and readme_override is not None:
      entries[rel] = readme_override.encode("utf-8")
      continue
    src_path = os.path.join(pkg_dir, rel)
    if not os.path.isfile(src_path):
      raise FileNotFoundError(f"Repository file not found at {src_path}")
    entries[rel] = _read_bytes(src_path)

  entries.update(build_doc_entries(pkg_dir))

  for rel in _OFX_SOURCE_RELPATHS + _REPO_EXTRA_OFX_FILES:
    src_path = os.path.join(ofx_source_dir, rel)
    if not os.path.isfile(src_path):
      raise FileNotFoundError(f"OpenFX repository file not found at {src_path}")
    entries[f"ofx/{rel}"] = _read_bytes(src_path)
  return entries


def build_and_write_release(
    output_dir: str,
    export_unpacked: bool = True,
    readme_text: str | None = None,
    linux_ofx_path: str | None = None,
    ofx_source_dir: str | None = None,
) -> dict[str, str]:
  """Builds, validates, and writes the full PaperCraft Studio Pro release.

  Args:
    output_dir: Target output directory path.
    export_unpacked: Whether to also write unpacked .setting/.png files.
    readme_text: Optional README.md contents to include inside the full release.
    linux_ofx_path: Path to compiled PaperCraftStudioPro.ofx Linux binary.
    ofx_source_dir: Directory containing OpenFX source and installer scripts.

  Returns:
    Mapping of logical artifact keys to output file paths.
  """
  os.makedirs(output_dir, exist_ok=True)
  pkg_dir = os.path.dirname(os.path.abspath(__file__))
  resolved_ofx_dir, resolved_linux_ofx, resolved_readme = (
      _resolve_package_defaults(ofx_source_dir, linux_ofx_path, readme_text)
  )

  drfx_entries = build_drfx_entries()
  drfx_bytes = _write_deterministic_zip(drfx_entries)
  drfx_validator.validate_drfx_bytes(drfx_bytes)

  sfx_entries = sfx_synthesizer.synthesize_all_paper_sfx()
  for wav_bytes in sfx_entries.values():
    drfx_validator.validate_wav_bytes(wav_bytes)

  ofx_entries = build_ofx_package_entries(resolved_ofx_dir, resolved_linux_ofx)
  doc_entries = build_doc_entries(pkg_dir)

  drfx_path = os.path.join(output_dir, "PaperCraft_Studio_Pro.drfx")
  with open(drfx_path, "wb") as f:
    f.write(drfx_bytes)

  written_paths: dict[str, str] = {"drfx": drfx_path}

  def _write_disk_tree(entries: Mapping[str, bytes]) -> None:
    for rel_path, payload in sorted(entries.items()):
      abs_path = os.path.join(output_dir, rel_path)
      os.makedirs(os.path.dirname(abs_path), exist_ok=True)
      with open(abs_path, "wb") as f:
        f.write(payload)
      if _is_executable_entry(rel_path):
        os.chmod(abs_path, 0o755)

  _write_disk_tree(sfx_entries)
  _write_disk_tree(ofx_entries)
  _write_disk_tree(doc_entries)
  written_paths["ofx_bundle"] = os.path.join(
      output_dir, "PaperCraftStudioPro.ofx.bundle"
  )
  written_paths["macos_installer"] = os.path.join(
      output_dir, "Install_PaperCraft_OpenFX_macOS.command"
  )
  written_paths["linux_installer"] = os.path.join(
      output_dir, "install_linux_ofx.sh"
  )
  written_paths["docs_dir"] = os.path.join(output_dir, "docs")

  if export_unpacked:
    _write_disk_tree(
        {f"Templates_Unpacked/{k}": v for k, v in drfx_entries.items()}
    )

  if resolved_readme is not None:
    readme_out = os.path.join(output_dir, "README.md")
    with open(readme_out, "w", encoding="utf-8") as f:
      f.write(resolved_readme)
    written_paths["readme"] = readme_out

  readme_en_src = os.path.join(pkg_dir, "README.en.md")
  readme_en_bytes = (
      _read_bytes(readme_en_src) if os.path.isfile(readme_en_src) else None
  )
  if readme_en_bytes is not None:
    readme_en_out = os.path.join(output_dir, "README.en.md")
    with open(readme_en_out, "wb") as f:
      f.write(readme_en_bytes)
    written_paths["readme_en"] = readme_en_out

  license_src = os.path.join(pkg_dir, "LICENSE")
  license_bytes = _read_bytes(license_src) if os.path.isfile(license_src) else None
  if license_bytes is not None:
    license_out = os.path.join(output_dir, "LICENSE")
    with open(license_out, "wb") as f:
      f.write(license_bytes)

  full_zip_map: dict[str, bytes] = {
      "PaperCraft_Studio_Pro.drfx": drfx_bytes,
  }
  if license_bytes is not None:
    full_zip_map["LICENSE"] = license_bytes
  full_zip_map.update(sfx_entries)
  full_zip_map.update(ofx_entries)
  full_zip_map.update(doc_entries)
  if export_unpacked:
    for rel_path, raw_bytes in drfx_entries.items():
      full_zip_map[f"Templates_Unpacked/{rel_path}"] = raw_bytes
  if resolved_readme is not None:
    full_zip_map["README.md"] = resolved_readme.encode("utf-8")
  if readme_en_bytes is not None:
    full_zip_map["README.en.md"] = readme_en_bytes

  full_zip_bytes = _write_deterministic_zip(full_zip_map)
  full_zip_path = os.path.join(
      output_dir, "PaperCraft_Studio_Pro_Full_Bundle.zip"
  )
  with open(full_zip_path, "wb") as f:
    f.write(full_zip_bytes)
  written_paths["full_zip"] = full_zip_path

  repo_source_entries = build_repo_source_entries(
      pkg_dir, resolved_ofx_dir, resolved_readme
  )
  repo_zip_map: dict[str, bytes] = {
      f"papercraft_pro/{rel}": payload
      for rel, payload in repo_source_entries.items()
  }
  for rel, payload in full_zip_map.items():
    repo_zip_map[f"papercraft_pro/release/{rel}"] = payload

  repo_zip_bytes = _write_deterministic_zip(repo_zip_map)
  repo_zip_path = os.path.join(
      output_dir, "PaperCraft_Studio_Pro_Complete_Repo.zip"
  )
  with open(repo_zip_path, "wb") as f:
    f.write(repo_zip_bytes)
  written_paths["repo_zip"] = repo_zip_path
  return written_paths

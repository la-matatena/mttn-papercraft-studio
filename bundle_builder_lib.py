"""Bundle assembler for PaperCraft OpenFX, installers, .drfx, and Paper SFX.

Called by `build_papercraft_bundle.py` (CLI) and by `papercraft_pro_test.py`.
The single public entry point that writes files is `build_and_write_release`;
the `build_*_entries` helpers return in-memory `{archive_path: bytes}` maps.

Release artifacts written to the output directory:

* `MTTN_PaperCraft_Studio.drfx`: ZIP with the Fusion effect and transition
  `.setting` files plus 104x58 thumbnails (validated before writing).
* `PaperCraftStudioPro.ofx.bundle/`, installers, `Paper_SFX/`, `docs/`, and
  optionally `Templates_Unpacked/`, unpacked on disk.
* `MTTN_PaperCraft_Studio_Full_Bundle.zip`: everything above in one ZIP.
* `MTTN_PaperCraft_Studio_Complete_Repo.zip`: repository sources under
  `papercraft_pro/` plus the full release under `papercraft_pro/release/`.

Deterministic ZIP rules (so rebuilding the same inputs yields identical
bytes and stable checksums): entries are written in sorted path order, every
entry gets the fixed `_ZIP_TIMESTAMP` and DEFLATE level `_ZIP_DEFLATE_LEVEL`,
`create_system` is UNIX, and file modes are normalized to 0o644, or 0o755
for executables (see `_is_executable_entry`). Byte-for-byte output still
assumes the same zlib version, because DEFLATE output can differ across zlib
releases.
"""

from collections.abc import Mapping
import io
import os
import stat
import zipfile

try:
  # Internal monorepo import path; falls back to sibling-module imports when
  # run from a plain checkout of this repository.
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import drfx_validator
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import fusion_templates
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import png_renderer
except ImportError:
  import drfx_validator  # type: ignore[no-redef]
  import fusion_templates  # type: ignore[no-redef]
  import png_renderer  # type: ignore[no-redef]

# Fixed mtime for every ZIP entry. ZIP stores local time with 2-second
# resolution and no time zone, so a constant keeps archives reproducible.
_ZIP_TIMESTAMP = (2026, 1, 1, 0, 0, 0)

# DEFLATE level for every ZIP entry. 6 is what zlib uses for
# Z_DEFAULT_COMPRESSION (-1), the level zipfile picked implicitly before it
# was pinned, so pinning it keeps previously published bytes unchanged.
_ZIP_DEFLATE_LEVEL = 6

# OpenFX sources copied into `OpenFX_Source/` (release) and `ofx/` (repo
# ZIP), relative to the `ofx/` source directory.
_OFX_SOURCE_RELPATHS = (
    "Info.plist",
    "openfx_sym.lds",
    "papercraft_kernel.h",
    "papercraft_kernel_internal.h",
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

# Installer scripts shipped at the release root (from the `ofx/` directory).
_INSTALLER_NAMES = (
    "Install_PaperCraft_OpenFX_macOS.command",
    "Uninstall_PaperCraft_OpenFX_macOS.command",
    "install_linux_ofx.sh",
)

# Documentation shipped in every release, relative to the repo root. Keep in
# sync with the Markdown link check in `papercraft_pro_test.py`.
_DOC_RELPATHS = (
    "docs/design_doc.md",
    "docs/developer_guide.md",
    "docs/images/paper_fold_unfold.png",
    "docs/user_guide.md",
)

# Foley clips (validated by `drfx_validator.validate_wav_bytes`) and credits.
# Also listed in `BUILD` (`paper_sfx_files`) and the tests.
_PAPER_SFX_RELPATHS = (
    "Paper_SFX/01_Paper_Unfold.wav",
    "Paper_SFX/02_Paper_Crumple.wav",
    "Paper_SFX/03_Paper_Glide_Flip.wav",
    "Paper_SFX/04_Paper_Rip_Tear.wav",
    "Paper_SFX/CREDITS.md",
)

# Repo-root files copied into the Complete Repo ZIP under `papercraft_pro/`.
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
)

# Extra `ofx/` files that only the repo ZIP carries (C++ test, installers in
# their source location), on top of `_OFX_SOURCE_RELPATHS`.
_REPO_EXTRA_OFX_FILES = (
    "Install_PaperCraft_OpenFX_macOS.command",
    "Uninstall_PaperCraft_OpenFX_macOS.command",
    "install_linux_ofx.sh",
    "papercraft_ofx_test.cc",
)


def _is_executable_entry(rel_path: str) -> bool:
  """Returns whether a packaged relative path requires 0o755 UNIX mode.

  Installers (`.command`, `.sh`) and the OpenFX shared library (`.ofx`) must
  stay executable after unzip; everything else is 0o644.
  """
  return rel_path.endswith((".command", ".sh", ".ofx"))


def _write_deterministic_zip(files: Mapping[str, bytes]) -> bytes:
  """Packages relative archive paths into a deterministic ZIP byte buffer.

  See the module docstring for the determinism rules (sorted order, fixed
  timestamp, UNIX create_system, normalized modes). The DEFLATE level is
  pinned to `_ZIP_DEFLATE_LEVEL` on every entry instead of relying on the
  zipfile default, so a Python change cannot silently alter the output.
  Caveat: identical bytes still require the same zlib version, because
  zlib releases may emit different (equally valid) DEFLATE streams for the
  same level.

  Args:
    files: Mapping of forward-slash archive paths to file contents.

  Returns:
    Complete ZIP archive bytes.
  """
  out = io.BytesIO()
  with zipfile.ZipFile(
      out,
      "w",
      compression=zipfile.ZIP_DEFLATED,
      compresslevel=_ZIP_DEFLATE_LEVEL,
  ) as zf:
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
      # The high 16 bits of external_attr hold the UNIX st_mode (file type +
      # permission bits); unzip tools read them back when create_system == 3.
      info.external_attr = mode << 16
      # A caller-built ZipInfo does not inherit the archive compresslevel,
      # so the level must also be passed per entry.
      zf.writestr(info, files[rel_path], compresslevel=_ZIP_DEFLATE_LEVEL)
  return out.getvalue()


def _read_bytes(path: str) -> bytes:
  """Returns the full binary contents of the file at `path`."""
  with open(path, "rb") as f:
    return f.read()


def _resolve_package_defaults(
    ofx_source_dir: str | None,
    linux_ofx_path: str | None,
    readme_text: str | None,
) -> tuple[str, str, str | None]:
  """Resolves default OpenFX source, compiled binary, and README paths.

  Defaults are relative to this module's directory (the repository root):
  `ofx/`, `PaperCraftStudioPro.ofx`, and `README.md` when it exists.

  Args:
    ofx_source_dir: Explicit OpenFX source directory, or None/"" for default.
    linux_ofx_path: Explicit compiled Linux `.ofx` path, or None/"".
    readme_text: Explicit README contents, or None to read `README.md`.

  Returns:
    `(ofx_source_dir, linux_ofx_path, readme_text)`; `readme_text` is None
    when no override is given and `README.md` does not exist.
  """
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
  """Creates the file tree contained inside MTTN_PaperCraft_Studio.drfx.

  Resolve installs a `.drfx` by unpacking it into its Fusion Templates
  folder; the `Edit/Effects/<category>` and `Edit/Transitions/<category>`
  prefixes decide where the templates appear in the Effects Library. Each
  `.setting` needs a same-named `.png` thumbnail next to it.

  Returns:
    Mapping of archive paths to `.setting` (UTF-8) and `.png` bytes.
  """
  fx_dir = "Edit/Effects/MTTN PaperCraft Studio"
  tr_dir = "Edit/Transitions/MTTN PaperCraft Studio"

  entries: dict[str, bytes] = {
      f"{fx_dir}/Paper Cutout FX.setting": (
          fusion_templates.build_effect_setting().encode("utf-8")
      ),
      f"{fx_dir}/Paper Cutout FX.png": png_renderer.render_thumbnail_png(
          (106, 79, 179), "effect"
      ),
      f"{tr_dir}/Paper Fold & Unfold.setting": (
          fusion_templates.build_fold_transition_setting().encode("utf-8")
      ),
      f"{tr_dir}/Paper Fold & Unfold.png": png_renderer.render_thumbnail_png(
          (131, 103, 199), "fold"
      ),
  }
  return entries


def build_ofx_package_entries(
    ofx_source_dir: str,
    linux_ofx_path: str,
) -> dict[str, bytes]:
  """Builds native OpenFX bundle, source, and installer file map entries.

  The bundle layout follows the OpenFX convention
  `<Name>.ofx.bundle/Contents/<Arch>/<Name>.ofx`; only the Linux-x86-64
  binary is packaged here (macOS installers compile from `OpenFX_Source/`).

  Args:
    ofx_source_dir: Directory with `Info.plist`, sources, and installers.
    linux_ofx_path: Path to the compiled Linux `PaperCraftStudioPro.ofx`.

  Returns:
    Mapping of release-relative paths to file bytes.

  Raises:
    FileNotFoundError: If the Linux binary or any listed source is missing.
  """
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
  """Reads all documentation files relative to `pkg_dir`.

  Args:
    pkg_dir: Repository root directory.

  Returns:
    Mapping of `_DOC_RELPATHS` entries to file bytes.

  Raises:
    FileNotFoundError: If any documentation file is missing.
  """
  entries: dict[str, bytes] = {}
  for rel in _DOC_RELPATHS:
    src_path = os.path.join(pkg_dir, rel)
    if not os.path.isfile(src_path):
      raise FileNotFoundError(f"Documentation file not found at {src_path}")
    entries[rel] = _read_bytes(src_path)
  return entries


def build_sfx_entries(pkg_dir: str) -> dict[str, bytes]:
  """Reads the 4 real Foley WAV clips and credits file under `Paper_SFX/`.

  Args:
    pkg_dir: Repository root directory.

  Returns:
    Mapping of `_PAPER_SFX_RELPATHS` entries to file bytes (not validated
    here; `build_and_write_release` validates the WAVs).

  Raises:
    FileNotFoundError: If any SFX file is missing.
  """
  entries: dict[str, bytes] = {}
  for rel in _PAPER_SFX_RELPATHS:
    src_path = os.path.join(pkg_dir, rel)
    if not os.path.isfile(src_path):
      raise FileNotFoundError(f"Paper SFX file not found at {src_path}")
    entries[rel] = _read_bytes(src_path)
  return entries


def build_repo_source_entries(
    pkg_dir: str,
    ofx_source_dir: str,
    readme_override: str | None = None,
    *,
    doc_entries: Mapping[str, bytes] | None = None,
    sfx_entries: Mapping[str, bytes] | None = None,
) -> dict[str, bytes]:
  """Collects source, test, build, and doc files for the complete repo ZIP.

  Args:
    pkg_dir: Repository root directory.
    ofx_source_dir: OpenFX source directory; files land under `ofx/`.
    readme_override: Replacement contents for `README.md`, or None to read
      the file from disk.
    doc_entries: Already-read `build_doc_entries(pkg_dir)` result, or None
      to read the docs here. Lets `build_and_write_release` reuse its copy
      instead of reading every doc twice.
    sfx_entries: Already-read `build_sfx_entries(pkg_dir)` result, or None
      to read the SFX files here (same reason as `doc_entries`).

  Returns:
    Mapping of repo-relative paths to file bytes (prefixed with
    `papercraft_pro/` later by the caller).

  Raises:
    FileNotFoundError: If any listed repository file is missing.
  """
  entries: dict[str, bytes] = {}
  for rel in _REPO_ROOT_FILES:
    if rel == "README.md" and readme_override is not None:
      entries[rel] = readme_override.encode("utf-8")
      continue
    src_path = os.path.join(pkg_dir, rel)
    if not os.path.isfile(src_path):
      raise FileNotFoundError(f"Repository file not found at {src_path}")
    entries[rel] = _read_bytes(src_path)

  entries.update(
      build_doc_entries(pkg_dir) if doc_entries is None else doc_entries
  )
  entries.update(
      build_sfx_entries(pkg_dir) if sfx_entries is None else sfx_entries
  )

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
  """Builds, validates, and writes the full MTTN PaperCraft Studio release.

  Validation (the `.drfx` structure and every WAV) runs before anything is
  written, so a failing template or clip aborts without a partial `.drfx`.
  Files that already exist in `output_dir` are overwritten.

  Args:
    output_dir: Target output directory path.
    export_unpacked: Whether to also write unpacked .setting/.png files.
    readme_text: Optional README.md contents to include inside the full release.
    linux_ofx_path: Path to compiled PaperCraftStudioPro.ofx Linux binary.
    ofx_source_dir: Directory containing OpenFX source and installer scripts.

  Returns:
    Mapping of logical artifact keys to output file paths: always `drfx`,
    `ofx_bundle`, `macos_installer`, `linux_installer`, `docs_dir`,
    `full_zip`, and `repo_zip`; plus `readme`, `readme_en`, and `license`
    when those files were written.

  Raises:
    ValueError: If the `.drfx` or a WAV fails validation.
    FileNotFoundError: If a required source, doc, SFX, or binary is missing.
  """
  os.makedirs(output_dir, exist_ok=True)
  pkg_dir = os.path.dirname(os.path.abspath(__file__))
  resolved_ofx_dir, resolved_linux_ofx, resolved_readme = (
      _resolve_package_defaults(ofx_source_dir, linux_ofx_path, readme_text)
  )

  # --- Build and validate in memory ---
  drfx_entries = build_drfx_entries()
  drfx_bytes = _write_deterministic_zip(drfx_entries)
  drfx_validator.validate_drfx_bytes(drfx_bytes)

  sfx_entries = build_sfx_entries(pkg_dir)
  for rel_path, payload in sfx_entries.items():
    if rel_path.endswith(".wav"):
      drfx_validator.validate_wav_bytes(payload)

  ofx_entries = build_ofx_package_entries(resolved_ofx_dir, resolved_linux_ofx)
  doc_entries = build_doc_entries(pkg_dir)

  # --- Write unpacked release tree ---
  drfx_path = os.path.join(output_dir, "MTTN_PaperCraft_Studio.drfx")
  with open(drfx_path, "wb") as f:
    f.write(drfx_bytes)

  written_paths: dict[str, str] = {"drfx": drfx_path}

  def _write_disk_tree(entries: Mapping[str, bytes]) -> None:
    """Writes `entries` under `output_dir`, creating parent directories.

    Mirrors the ZIP mode rules: executables get 0o755; other files keep the
    process umask default.
    """
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
    written_paths["license"] = license_out

  # --- Full Bundle ZIP: the same release tree as one archive ---
  full_zip_map: dict[str, bytes] = {
      "MTTN_PaperCraft_Studio.drfx": drfx_bytes,
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
      output_dir, "MTTN_PaperCraft_Studio_Full_Bundle.zip"
  )
  with open(full_zip_path, "wb") as f:
    f.write(full_zip_bytes)
  written_paths["full_zip"] = full_zip_path

  # --- Complete Repo ZIP: sources under papercraft_pro/, release nested ---
  # Reuse the docs/SFX already read above so each file is read only once.
  repo_source_entries = build_repo_source_entries(
      pkg_dir,
      resolved_ofx_dir,
      resolved_readme,
      doc_entries=doc_entries,
      sfx_entries=sfx_entries,
  )
  repo_zip_map: dict[str, bytes] = {
      f"papercraft_pro/{rel}": payload
      for rel, payload in repo_source_entries.items()
  }
  for rel, payload in full_zip_map.items():
    repo_zip_map[f"papercraft_pro/release/{rel}"] = payload

  repo_zip_bytes = _write_deterministic_zip(repo_zip_map)
  repo_zip_path = os.path.join(
      output_dir, "MTTN_PaperCraft_Studio_Complete_Repo.zip"
  )
  with open(repo_zip_path, "wb") as f:
    f.write(repo_zip_bytes)
  written_paths["repo_zip"] = repo_zip_path
  return written_paths

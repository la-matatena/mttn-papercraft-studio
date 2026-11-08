"""CLI binary to generate and validate the PaperCraft Studio Pro bundle."""

import argparse
from collections.abc import Sequence
import logging as std_logging

try:
  from google3.experimental.users.joseluiszapata.davinci_resolve_plugins.papercraft_pro import bundle_builder_lib
except ImportError:
  import bundle_builder_lib  # type: ignore[no-redef]

try:
  from absl import app
  from absl import flags
  from absl import logging

  _HAS_ABSL = True
except ImportError:
  _HAS_ABSL = False

if _HAS_ABSL:
  _OUTPUT_DIR = flags.DEFINE_string(
      "output_dir",
      "",
      "Target directory where OpenFX bundle, .drfx, SFX, and release ZIP are"
      " saved.",
  )
  _EXPORT_UNPACKED = flags.DEFINE_bool(
      "export_unpacked",
      True,
      "Also write unpacked .setting and thumbnail .png templates.",
  )
  _README_PATH = flags.DEFINE_string(
      "readme_path",
      "",
      "Optional README.md path to include inside the full distribution ZIP.",
  )
  _LINUX_OFX_PATH = flags.DEFINE_string(
      "linux_ofx_path",
      "",
      "Optional path to compiled PaperCraftStudioPro.ofx Linux binary.",
  )
  _OFX_SOURCE_DIR = flags.DEFINE_string(
      "ofx_source_dir",
      "",
      "Optional directory containing OpenFX C++ sources and installer scripts.",
  )


def _run_build(
    output_dir: str,
    export_unpacked: bool,
    readme_path: str,
    linux_ofx_path: str,
    ofx_source_dir: str,
) -> dict[str, str]:
  """Reads optional README and executes bundle generation."""
  readme_text: str | None = None
  if readme_path:
    with open(readme_path, "r", encoding="utf-8") as f:
      readme_text = f.read()

  return bundle_builder_lib.build_and_write_release(
      output_dir=output_dir,
      export_unpacked=export_unpacked,
      readme_text=readme_text,
      linux_ofx_path=linux_ofx_path or None,
      ofx_source_dir=ofx_source_dir or None,
  )


def main(argv: Sequence[str]) -> None:
  """Builds and writes the PaperCraft Studio Pro deliverables."""
  if len(argv) > 1:
    raise app.UsageError("Unexpected positional command-line arguments.")
  output_dir = _OUTPUT_DIR.value
  if not output_dir:
    raise app.UsageError("--output_dir must be non-empty.")

  results = _run_build(
      output_dir=output_dir,
      export_unpacked=_EXPORT_UNPACKED.value,
      readme_path=_README_PATH.value,
      linux_ofx_path=_LINUX_OFX_PATH.value,
      ofx_source_dir=_OFX_SOURCE_DIR.value,
  )
  logging.info("Generated PaperCraft Studio Pro bundle: %s", results)


def _standalone_main() -> None:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--output_dir", required=True)
  parser.add_argument(
      "--export_unpacked",
      action=argparse.BooleanOptionalAction,
      default=True,
  )
  parser.add_argument("--readme_path", default="")
  parser.add_argument("--linux_ofx_path", default="")
  parser.add_argument("--ofx_source_dir", default="")
  args = parser.parse_args()
  std_logging.basicConfig(level=std_logging.INFO)
  results = _run_build(
      output_dir=args.output_dir,
      export_unpacked=args.export_unpacked,
      readme_path=args.readme_path,
      linux_ofx_path=args.linux_ofx_path,
      ofx_source_dir=args.ofx_source_dir,
  )
  std_logging.info("Generated PaperCraft Studio Pro bundle: %s", results)


if __name__ == "__main__":
  if _HAS_ABSL:
    app.run(main)
  else:
    _standalone_main()

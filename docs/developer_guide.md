# MTTN PaperCraft Studio developer and build guide

This guide helps engineers build, test, package, compile standalone OpenFX bundles, and extend MTTN PaperCraft Studio 3.1.0 from a cloned repository or release archive. Version 3.1.0 ships two OpenFX plugins in one bundle: the **Paper Cutout FX** filter (`com.papercraft.CutoutFX`) and the **Paper Fold & Unfold** transition (`com.papercraft.PaperFoldTransition`).

## Before you begin

Prepare the following toolchain on Linux or macOS:

- **C++17 compiler**: `g++` on Linux x86-64 (plus `libgtest-dev` for C++ unit tests) or Xcode Command Line Tools `clang++` on macOS.
- **Scripting and packaging**: Bash and Python 3.10 or later (standard library only; no external `pip` packages required).
- **Optional lint tools** (development only): the pinned formatters and linters described in [Formatters and linters](#formatters-and-linters).

## Repository layout

Cloning `la-matatena/mttn-papercraft-studio` provides every source file, test, header, landing page, and asset builder:

| Path in repository | Role |
| :--- | :--- |
| `.github/workflows/` | GitHub Actions workflows for C++/Python CI, release packaging (`ci-release.yml`), and GitHub Pages (`pages.yml`) |
| `README.md` and `README.en.md` | Spanish and English package overviews and quick installation guides |
| `AGENTS.md` | Quick convention and verification guide for AI coding assistants |
| `docs/design_doc.md` | Architecture, OpenFX 1.4 integration details, rendering math, and design trade-offs |
| `docs/user_guide.md` | End-user installation, Edit-page workflows, all Inspector parameters, SFX reference, and troubleshooting |
| `docs/developer_guide.md` | Build, standalone compilation, test execution, and extension guide |
| `docs/images/` | Preview of the **Paper Fold & Unfold** transition rendered by the C++ kernel |
| `site/` | Bilingual GitHub Pages landing page (`site/index.html` in Spanish, `site/en/index.html` in English) |
| `ofx/papercraft_kernel.h` and `ofx/papercraft_kernel.cc` | Platform-independent C++17 paper cutout, SDF border, intro and outro animation, fold and unfold renderer, and compositing kernels |
| `ofx/papercraft_ofx_plugin.cc` | OpenFX 1.4 registration of the filter and transition, parameters, suites, and multi-thread dispatch |
| `ofx/papercraft_ofx_test.cc` | GoogleTest OpenFX mock host and `dlopen` dynamic plugin test suite |
| `ofx/papercraft_kernel_internal.h` and `ofx/papercraft_kernel_test.cc` | Internal kernel helpers exposed for tests (hidden from the plugin exports) and the direct kernel unit test suite |
| `ofx/run_cpp_tests.sh` and `ofx/testing/gtest_lite/` | One-command build and run of both C++ suites, with a minimal GoogleTest stand-in (`--lite`) for machines without GoogleTest |
| `ofx/openfx_include/` | Vendored OpenFX 1.4 C headers and license |
| `ofx/Info.plist` and `ofx/openfx_sym.lds` | macOS bundle metadata and Linux symbol export version script |
| `ofx/Install_PaperCraft_OpenFX_macOS.command`, `ofx/Uninstall_PaperCraft_OpenFX_macOS.command`, and `ofx/install_linux_ofx.sh` | OS-specific installers and uninstaller for macOS and Linux x86-64 |
| `Paper_SFX/` | Four real-world 48 kHz, 24-bit stereo Foley WAV recordings and `CREDITS.md` |
| `png_renderer.py` | Pure-Python PNG generator for the effect and transition thumbnails |
| `fusion_templates.py` and `drfx_validator.py` | Legacy **Paper Cutout FX** and **Paper Fold & Unfold** `.setting` macro builders and structural validator |
| `bundle_builder_lib.py` and `build_papercraft_bundle.py` | Deterministic bundle assembler writing `MTTN_PaperCraft_Studio.drfx`, `MTTN_PaperCraft_Studio_Full_Bundle.zip`, and `MTTN_PaperCraft_Studio_Complete_Repo.zip` |
| `papercraft_pro_test.py` | Python verification suite for assets, `.drfx`, installers, Markdown links and `#anchors` (including `AGENTS.md`), and ZIP packages |
| `tools/lint.sh` | Dev-only formatter and linter runner (`--fix` applies the formatters). CI runs it in check mode. See [Formatters and linters](#formatters-and-linters) |
| `pyproject.toml`, `.pylintrc`, `.clang-format`, `.clang-format-ignore`, `.prettierrc.json`, and `.prettierignore` | Formatter and linter configuration used by `tools/lint.sh` |

## Compile the OpenFX bundle

All C++ runtime sources guard standalone header includes with the `PAPERCRAFT_STANDALONE_OFX` preprocessor macro so you can build directly from the repository root.

### Build on Linux x86-64

Run `g++` from the repository root directory:

```bash
mkdir -p BUILD_DIR/PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64
mkdir -p BUILD_DIR/PaperCraftStudioPro.ofx.bundle/Contents/Resources
cp ofx/Info.plist BUILD_DIR/PaperCraftStudioPro.ofx.bundle/Contents/Info.plist
cp ofx/openfx_include/LICENSE BUILD_DIR/PaperCraftStudioPro.ofx.bundle/Contents/Resources/LICENSE

g++ -std=c++17 -O3 -fPIC -shared -fvisibility=hidden \
  -DPAPERCRAFT_STANDALONE_OFX \
  -Iofx -Iofx/openfx_include \
  -Wl,--version-script=ofx/openfx_sym.lds \
  ofx/papercraft_kernel.cc ofx/papercraft_ofx_plugin.cc \
  -o BUILD_DIR/PaperCraftStudioPro.ofx.bundle/Contents/Linux-x86-64/PaperCraftStudioPro.ofx
```

Replace the following:

- `BUILD_DIR`: the output directory where `PaperCraftStudioPro.ofx.bundle` is assembled.

### Build on macOS

Use the bundled macOS script with the `--build-only` flag to compile an ad-hoc signed bundle (`arm64` and `x86_64` Universal when available) into `BUILD_DIR`:

```bash
./ofx/Install_PaperCraft_OpenFX_macOS.command --build-only BUILD_DIR
```

Replace the following:

- `BUILD_DIR`: the destination folder where `PaperCraftStudioPro.ofx.bundle` is built without touching `/Library/OFX/Plugins`.

### Assemble the full release bundle

Package `MTTN_PaperCraft_Studio.drfx`, the four Foley WAVs, installers, documentation, OpenFX source kit, and `MTTN_PaperCraft_Studio_Full_Bundle.zip`:

```bash
python3 -B build_papercraft_bundle.py \
  --output_dir=OUTPUT_DIR \
  --readme_path=README.md \
  --linux_ofx_path=PaperCraftStudioPro.ofx
```

Replace the following:

- `OUTPUT_DIR`: the destination directory path on your local filesystem.

## Run the test suites

### Python `.drfx`, Foley WAV, installer, and link suite

```bash
python3 -B papercraft_pro_test.py -v
```

### C++ suites with one script

`ofx/run_cpp_tests.sh` builds the plugin binary (the macOS installer with `--build-only`, or the CI `g++` flags on Linux), then compiles and runs both C++ suites: `ofx/papercraft_ofx_test.cc` (mock OpenFX host that loads the built plugin) and `ofx/papercraft_kernel_test.cc` (direct unit tests of the pixel kernels). It exits non-zero on any failure.

```bash
./ofx/run_cpp_tests.sh
./ofx/run_cpp_tests.sh --lite
```

The script uses an installed GoogleTest when it finds one. Otherwise, or with `--lite`, it uses `ofx/testing/gtest_lite`, a tiny GoogleTest stand-in for local runs only. CI always uses real GoogleTest.

### C++ OpenFX host and rendering suite on Linux

```bash
g++ -std=c++17 -O2 -DPAPERCRAFT_STANDALONE_OFX \
  -Iofx -Iofx/openfx_include \
  ofx/papercraft_ofx_test.cc \
  -lgtest -lgtest_main -ldl -lpthread \
  -o papercraft_ofx_test
PAPERCRAFT_OFX_PATH="$PWD/PaperCraftStudioPro.ofx" ./papercraft_ofx_test
g++ -std=c++17 -O2 -DPAPERCRAFT_STANDALONE_OFX -Iofx -Iofx/openfx_include \
  ofx/papercraft_kernel_test.cc ofx/papercraft_kernel.cc \
  -lgtest -lgtest_main -lpthread -o papercraft_kernel_test
./papercraft_kernel_test
```

### C++ OpenFX host and rendering suite on macOS

On macOS, compile GoogleTest from a source checkout in a scratch folder and point the test binary at a locally built `.ofx` shared library:

```bash
clang++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden \
  -DPAPERCRAFT_STANDALONE_OFX -Iofx -Iofx/openfx_include \
  -Wl,-exported_symbol,_OfxGetNumberOfPlugins \
  -Wl,-exported_symbol,_OfxGetPlugin \
  ofx/papercraft_kernel.cc ofx/papercraft_ofx_plugin.cc \
  -o BUILD_DIR/PaperCraftStudioPro.ofx
clang++ -std=c++17 -O2 -DPAPERCRAFT_STANDALONE_OFX \
  -isystem GTEST_DIR/include -IGTEST_DIR -Iofx -Iofx/openfx_include \
  ofx/papercraft_ofx_test.cc GTEST_DIR/src/gtest-all.cc \
  GTEST_DIR/src/gtest_main.cc -lpthread -o BUILD_DIR/papercraft_ofx_test
PAPERCRAFT_OFX_PATH=BUILD_DIR/PaperCraftStudioPro.ofx \
  BUILD_DIR/papercraft_ofx_test
```

Replace the following:

- `BUILD_DIR`: a scratch output directory.
- `GTEST_DIR`: the `googletest` folder of a GoogleTest source release.

## Extend parameters, effects, and Foley sounds

When modifying or extending the plugin, update matching code and tests across each layer so host suites, validators, and packs stay synchronized.

### Add or update an OpenFX Inspector control

1. Add the parameter field and default value to `CutoutParams` (filter) or `PaperFoldTransitionParams` (transition) in `ofx/papercraft_kernel.h`.
2. Read and apply the field in `RenderCutoutFrame` or `RenderPaperFoldTransition` in `ofx/papercraft_kernel.cc`.
3. Define the Inspector control inside `DefineCutoutParameters` or `DefineFoldTransitionParameters`, add its `OfxParamHandle` slot in `PluginInstance`, bind it in `HandleCreateInstance` for the matching `PluginRole`, and populate it in `ReadCutoutParams` or `ReadFoldTransitionParams` inside `ofx/papercraft_ofx_plugin.cc`. Both plugins share identifiers such as `paperColor` and `shadowOpacity`, so a shared control binds once.
4. Add a targeted GoogleTest regression test in `ofx/papercraft_ofx_test.cc`.

> [!NOTE]
> Keep existing parameter identifiers stable, even when their names no longer match their purpose. For example, the intro and outro group is still named `TransitionGroup` and the **Glide Angle** control is still `transitionAngle`, so projects saved with earlier versions keep their values.

### Add a fold pattern

1. Add an enumerator to `FoldPattern` in `ofx/papercraft_kernel.h`.
2. Add its hinge templates to `BuildFoldLayout` in `ofx/papercraft_kernel.cc`. List hinges in folding order (the last ones open first), keep every `frac` above `0` so the shared core stays on each hinge's stationary side, and set `valley = false` for mountain folds.
3. Add the label to `kPatterns` in `DefineFoldTransitionParameters` and raise the `fold_pattern` clamp in `RenderPaperFoldTransition`.
4. Extend the pattern loop in `FoldTransitionPatternsActionsAndByteDepthRender` and update the **Fold Pattern** rows in `docs/user_guide.md` and both READMEs.

New patterns need no extra work for bounds pruning: `FoldSheetBounds` folds any hinge layout. If you change how `UnfoldFlapPoint` lifts or stretches a flap, or how `EvalPaperLeaf` and `EvalFoldCoverage` grow the paper edge, update `FoldedFlapBounds` and the grow margins in `RenderPaperFoldTransition` and `RenderCutoutFrame` to match. Bounds that are too tight clip paper silently.

### Add a Paper SFX Foley sound

1. Place a MIT-compatible (`CC0` or `Public Domain`) 48 kHz, 24-bit stereo WAV clip with peaks below `0.995` and smooth zero-crossing edge fades into `Paper_SFX/`, and document its source in `Paper_SFX/CREDITS.md`.
2. Register the relative path in `_PAPER_SFX_RELPATHS` inside `bundle_builder_lib.py`, `paper_sfx_files` in `BUILD`, and expected paths in `papercraft_pro_test.py`.
3. Run `papercraft_pro_test.py` to verify RMS loudness, stereo channel separation, and click-free edge fades on every channel through `drfx_validator.validate_wav_bytes`.

## Code style and documentation conventions

Write code so a maintainer who has never seen it can follow every file and function without asking. Comments and docstrings are in English; user-facing strings keep their current language.

- **C++** (`ofx/`): follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). Document every public type and function in headers, and put a `//` comment block directly above every function definition in `.cc` files, including anonymous-namespace helpers. State inputs, outputs, units, and coordinate spaces, and say which OpenFX action or `parallel_for` worker runs the code.
- **Python**: follow the [Google Python Style Guide](https://google.github.io/styleguide/pyguide.html) with the existing 2-space indent and 80-column limit. Every module, class, and function gets a docstring with `Args:`, `Returns:`, and `Raises:` sections when they apply. Test methods get a one-line docstring that states the verified behavior.
- **Shell** (installers): follow the [Google Shell Style Guide](https://google.github.io/styleguide/shellguide.html). Start each script with a header covering purpose, usage, and flags, and add a function comment block (`Globals`, `Arguments`, `Outputs`, `Returns`) above every function.
- **Comment content**: explain why, not what. Cover units (px, normalized 0..1, frames, degrees), invariants, magic numbers, host quirks, and how generated Fusion SimpleExpressions behave. Long files group code under section banners such as `# --- Section name ---`.
- **Debt markers**: mark known shortcuts with a `shortcut:` comment and keep existing ones. They form the tracked debt ledger.
- **Doc-only commits**: never change behavior. Don't rename identifiers, OpenFX parameter IDs, or JSON keys, and don't reorder logic. Use a `docs:` commit, and run `python3 -B papercraft_pro_test.py -v` before you commit.

### Formatters and linters

`tools/lint.sh` enforces the style above automatically. The `lint` job in `ci-release.yml` runs it in check mode on every push and pull request, so a formatting or lint error fails CI. The tools are for development only: the plugin, templates, and bundle builder still need nothing beyond the standard library.

| Language | Tool (pinned version) | Configuration | Files checked |
| :--- | :--- | :--- | :--- |
| Python | `pyink` 26.5.1 (formatter) and `pylint` 4.1.3 | `pyproject.toml` (`[tool.pyink]`: 80 columns, 2-space indent, majority quotes) and `.pylintrc` (Google's pylintrc; local changes are marked `# local:`) | `*.py` |
| C++ | `clang-format` 23.1.5 | `.clang-format` (`BasedOnStyle: Google`) and `.clang-format-ignore` (skips the vendored `ofx/openfx_include/`) | `ofx/*.cc`, `ofx/*.h`, `ofx/testing/` |
| Shell | `shellcheck` 0.11.0 (from `shellcheck-py` 0.11.0.1) | Inline `# shellcheck disable=...` comments, each with a reason | `ofx/*.command`, `ofx/*.sh`, `tools/*.sh` |
| CSS | `prettier` 3.10.1 | `.prettierrc.json` and `.prettierignore` | `site/*.css` |

Install the pinned tools into the git-ignored `.venv-dev/` folder once. `tools/lint.sh` adds that folder to `PATH` on its own:

```bash
python3 -m venv .venv-dev
.venv-dev/bin/pip install $(tools/lint.sh --pins pip)
npm install --prefix .venv-dev/node --registry https://registry.npmjs.org/ $(tools/lint.sh --pins npm)
```

Run the checks, or apply the formatters (`pyink`, `clang-format`, and `prettier`) and then lint:

```bash
tools/lint.sh
tools/lint.sh --fix
```

Keep these rules in mind:

- `pylint` and `shellcheck` findings have no auto-fix. Fix them by hand. If a fix would change behavior, add a targeted inline disable comment (`# pylint: disable=...`, `# shellcheck disable=...`, or `// clang-format off` and `// clang-format on`) with a one-line reason.
- `pyink` runs with `unstable = true` because its command line ignores `enable-unstable-feature`, and the existing code relies on the `hug_parens_with_braces_and_square_brackets` layout (`foo([` ... `])`). Pin the version, because unstable output can change between releases.
- `fusion_templates.py` builds both Fusion `.setting` graphs from small documented node builders (`_node`, `_expr`/`_expr_block`, `_link`/`_link_block`). Their output must stay byte-identical: `test_generated_settings_sha256_pinned` pins the sha256 of both generated templates, so update the hashes only after checking an intended template change in DaVinci Resolve.
- The landing page HTML (`site/index.html` and `site/en/index.html`) is not auto-formatted. Prettier would reflow the text and add or remove whitespace text nodes, so the DOM would not stay provably identical. ESLint isn't configured because the repository has no JavaScript.
- Markdown, YAML, `Info.plist`, `openfx_sym.lds`, and generated `.setting` output are not formatted.
- The lint configs and `tools/lint.sh` ship in `MTTN_PaperCraft_Studio_Complete_Repo.zip`, but not in the user-facing `MTTN_PaperCraft_Studio_Full_Bundle.zip`.

## What's next

- Read the [design document](design_doc.md) for rendering algorithms and architectural trade-offs.
- Read the [user guide](user_guide.md) for Edit-page workflows and complete control ranges.

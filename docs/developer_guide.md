# MTTN PaperCraft Studio developer and build guide

This guide helps engineers build, test, package, compile standalone OpenFX bundles, and extend MTTN PaperCraft Studio 3.1.0 from a cloned repository or release archive. Version 3.1.0 ships two OpenFX plugins in one bundle: the **Paper Cutout FX** filter (`com.papercraft.CutoutFX`) and the **Paper Fold & Unfold** transition (`com.papercraft.PaperFoldTransition`).

## Before you begin

Prepare the following toolchain on Linux or macOS:

- **C++17 compiler**: `g++` on Linux x86-64 (plus `libgtest-dev` for C++ unit tests) or Xcode Command Line Tools `clang++` on macOS.
- **Scripting and packaging**: Bash and Python 3.10 or later (standard library only; no external `pip` packages required).

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
| `ofx/openfx_include/` | Vendored OpenFX 1.4 C headers and license |
| `ofx/Info.plist` and `ofx/openfx_sym.lds` | macOS bundle metadata and Linux symbol export version script |
| `ofx/Install_PaperCraft_OpenFX_macOS.command`, `ofx/Uninstall_PaperCraft_OpenFX_macOS.command`, and `ofx/install_linux_ofx.sh` | OS-specific installers and uninstaller for macOS and Linux x86-64 |
| `Paper_SFX/` | Four real-world 48 kHz, 24-bit stereo Foley WAV recordings and `CREDITS.md` |
| `png_renderer.py` | Pure-Python PNG generator for the effect and transition thumbnails |
| `fusion_templates.py` and `drfx_validator.py` | Legacy **Paper Cutout FX** and **Paper Fold & Unfold** `.setting` macro builders and structural validator |
| `bundle_builder_lib.py` and `build_papercraft_bundle.py` | Deterministic bundle assembler writing `MTTN_PaperCraft_Studio.drfx`, `MTTN_PaperCraft_Studio_Full_Bundle.zip`, and `MTTN_PaperCraft_Studio_Complete_Repo.zip` |
| `papercraft_pro_test.py` | Python verification suite for assets, `.drfx`, installers, Markdown links, and ZIP packages |

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

### C++ OpenFX host and rendering suite on Linux

```bash
g++ -std=c++17 -O2 -DPAPERCRAFT_STANDALONE_OFX \
  -Iofx -Iofx/openfx_include \
  ofx/papercraft_ofx_test.cc \
  -lgtest -lgtest_main -ldl -lpthread \
  -o papercraft_ofx_test
PAPERCRAFT_OFX_PATH="$PWD/PaperCraftStudioPro.ofx" ./papercraft_ofx_test
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
3. Run `papercraft_pro_test.py` to verify RMS loudness, stereo channel separation, and click-free edge fades through `drfx_validator.validate_wav_bytes`.

## What's next

- Read the [design document](design_doc.md) for rendering algorithms and architectural trade-offs.
- Read the [user guide](user_guide.md) for Edit-page workflows and complete control ranges.

# PaperCraft Studio Pro developer and build guide

This guide helps engineers build, test, package, compile standalone OpenFX
bundles, and extend PaperCraft Studio Pro 3.1.0 inside Blaze or from the
exported repository archive. Version 3.1.0 ships two OpenFX plugins in one
bundle: the **Paper Cutout FX** filter (`com.papercraft.CutoutFX`) and the
**Paper Fold & Unfold** transition (`com.papercraft.PaperFoldTransition`).

## Before you begin

Prepare one of the following toolchains based on your build environment:

-   **Blaze inside Google3**: Access to
    `/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents` inside the
    workspace root.
-   **Standalone Linux or macOS build**: A C++17 compiler (`g++` on Linux x86-64
    or Xcode Command Line Tools `clang++` on macOS), Bash, and Python 3.10 or
    later for packaging and validation scripts.

## Repository layout

Extracting `PaperCraft_Studio_Pro_Complete_Repo.zip` creates a self-contained
`papercraft_pro/` directory containing every source file, test, header,
document, and prebuilt release deliverable:

| Path in repository                            | Role                                      |
| :-------------------------------------------- | :---------------------------------------- |
| `BUILD`                                       | Blaze targets for C++ kernels, `.ofx`     |
:                                               : shared library, standalone compile check, :
:                                               : tests, and release bundles                :
| `README.md`                                   | Package overview and quick installation   |
:                                               : entry point                               :
| `docs/design_doc.md`                          | Architecture, OpenFX 1.4 integration      |
:                                               : details, rendering math, and design       :
:                                               : trade-offs                                :
| `docs/user_guide.md`                          | End-user installation, Edit-page          |
:                                               : workflows, all Inspector parameters, SFX  :
:                                               : reference, and troubleshooting            :
| `docs/developer_guide.md`                     | Build, standalone compilation, test       |
:                                               : execution, and extension guide            :
| `docs/images/`                                | README preview of the fold transition,    |
:                                               : rendered by the kernel                    :
| `ofx/papercraft_kernel.h` and                 | Platform-independent C++17 paper cutout,  |
: `ofx/papercraft_kernel.cc`                    : SDF border, intro and outro animation,    :
:                                               : fold and unfold renderer, and             :
:                                               : compositing kernels                       :
| `ofx/papercraft_ofx_plugin.cc`                | OpenFX 1.4 registration of the filter and |
:                                               : transition, parameters, suites, and       :
:                                               : multi-thread dispatch                     :
| `ofx/papercraft_ofx_test.cc`                  | GUnit OpenFX mock host and `dlopen`       |
:                                               : dynamic plugin test suite                 :
| `ofx/openfx_include/`                         | Vendored OpenFX 1.4 C headers and license |
| `ofx/Info.plist` and `ofx/openfx_sym.lds`     | macOS bundle metadata and Linux symbol    |
:                                               : export version script                     :
| `ofx/Install_PaperCraft_OpenFX_macOS.command` | OS-specific installers for macOS and      |
: and `ofx/install_linux_ofx.sh`                : Linux x86-64                              :
| `Paper_SFX/`                                  | Four real-world 48 kHz, 24-bit stereo     |
:                                               : Foley WAV recordings and `CREDITS.md`     :
| `png_renderer.py`                             | Pure-Python PNG generator for the effect  |
:                                               : and transition thumbnails                 :
| `fusion_templates.py` and `drfx_validator.py` | Legacy **Paper Cutout FX** and **Paper    |
:                                               : Fold & Unfold** `.setting` macro builders :
:                                               : and structural validator                  :
| `bundle_builder_lib.py` and                   | Deterministic bundle assembler writing    |
: `build_papercraft_bundle.py`                  : `.drfx`,                                  :
:                                               : `PaperCraft_Studio_Pro_Full_Bundle.zip`,  :
:                                               : and                                       :
:                                               : `PaperCraft_Studio_Pro_Complete_Repo.zip` :
| `papercraft_pro_test.py`                      | Python `absltest` verification suite for  |
:                                               : assets, installers, links, and ZIP        :
:                                               : packages                                  :
| `release/` (inside                            | Prebuilt Linux OpenFX bundle, installers, |
: `PaperCraft_Studio_Pro_Complete_Repo.zip`)    : source kit, `.drfx`, documentation, and   :
:                                               : `Paper_SFX/` WAVs                         :

## Build, test, and export with Blaze

Run all C++ OpenFX tests and Python bundle tests sequentially inside the
`google3` workspace root:

```bash
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents test \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:papercraft_ofx_test \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:papercraft_pro_test
```

Verify the standalone macOS `#include` mode (`-DPAPERCRAFT_STANDALONE_OFX`)
alongside complete archive packaging (`.drfx`, runtime release ZIP, and complete
repository ZIP):

```bash
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents build \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:papercraft_standalone_compile_check \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:papercraft_release_bundle
```

Export all unpacked release deliverables, runtime archive, and complete
repository ZIP directly to `OUTPUT_DIR`:

```bash
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents run \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:build_papercraft_bundle -- \
  --output_dir=OUTPUT_DIR
```

Replace the following:

-   `OUTPUT_DIR`: the destination directory path on your local filesystem.

## Compile the OpenFX bundle outside Blaze

All C++ runtime sources guard Google3 versus relative header includes with the
`PAPERCRAFT_STANDALONE_OFX` preprocessor macro so you can build directly from
the extracted `papercraft_pro/` repository.

### Build on Linux x86-64

Run `g++` from the extracted `papercraft_pro/` root directory:

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

-   `BUILD_DIR`: the output directory where `PaperCraftStudioPro.ofx.bundle` is
    assembled.

### Build on macOS

Use the bundled macOS script with the `--build-only` flag to compile a signed
bundle (`arm64` and `x86_64` Universal when available) into `BUILD_DIR`:

```bash
./ofx/Install_PaperCraft_OpenFX_macOS.command --build-only BUILD_DIR
```

Replace the following:

-   `BUILD_DIR`: the destination folder where `PaperCraftStudioPro.ofx.bundle`
    is built without touching `/Library/OFX/Plugins`.

## Extend parameters, effects, and Foley sounds

When modifying or extending the plugin, update matching code and tests across
each layer so host suites, validators, and packs stay synchronized.

### Add or update an OpenFX Inspector control

1.  Add the parameter field and default value to `CutoutParams` (filter) or
    `PaperFoldTransitionParams` (transition) in `ofx/papercraft_kernel.h`.
2.  Read and apply the field in `RenderCutoutFrame` or
    `RenderPaperFoldTransition` in `ofx/papercraft_kernel.cc`.
3.  Define the Inspector control inside `DefineCutoutParameters` or
    `DefineFoldTransitionParameters`, add its `OfxParamHandle` slot in
    `PluginInstance`, bind it in `HandleCreateInstance` for the matching
    `PluginRole`, and populate it in `ReadCutoutParams` or
    `ReadFoldTransitionParams` inside `ofx/papercraft_ofx_plugin.cc`. Both
    plugins share identifiers such as `paperColor` and `shadowOpacity`, so a
    shared control binds once.
4.  Add a targeted GUnit regression test in `ofx/papercraft_ofx_test.cc`.

> [!NOTE] Keep existing parameter identifiers stable, even when their names no
> longer match their purpose. For example, the intro and outro group is still
> named `TransitionGroup` and the **Glide Angle** control is still
> `transitionAngle`, so projects saved with earlier versions keep their values.

### Add a fold pattern

1.  Add an enumerator to `FoldPattern` in `ofx/papercraft_kernel.h`.
2.  Add its hinge templates to `BuildFoldLayout` in `ofx/papercraft_kernel.cc`.
    List hinges in folding order (the last ones open first), keep every `frac`
    above `0` so the shared core stays on each hinge's stationary side, and set
    `valley = false` for mountain folds.
3.  Add the label to `kPatterns` in `DefineFoldTransitionParameters` and raise
    the `fold_pattern` clamp in `RenderPaperFoldTransition`.
4.  Extend the pattern loop in `FoldTransitionPatternsActionsAndByteDepthRender`
    and update the **Fold Pattern** rows in `docs/user_guide.md` and both
    READMEs.

New patterns need no extra work for bounds pruning: `FoldSheetBounds` folds
any hinge layout. If you change how `UnfoldFlapPoint` lifts or stretches a flap,
or how `EvalPaperLeaf` and `EvalFoldCoverage` grow the paper edge, update
`FoldedFlapBounds` and the grow margins in `RenderPaperFoldTransition` and
`RenderCutoutFrame` to match. Bounds that are too tight clip paper silently.

### Run the C++ suite on macOS without Blaze

The CI workflow runs `ofx/papercraft_ofx_test.cc` on Linux with `libgtest-dev`.
On macOS, compile GoogleTest from a source checkout in a scratch folder and
point the test at a locally built bundle binary:

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

-   `BUILD_DIR`: a scratch output directory.
-   `GTEST_DIR`: the `googletest` folder of a GoogleTest source release.

### Add a Paper SFX Foley sound

1.  Place a MIT-compatible (`CC0` or `Public Domain`) 48 kHz, 24-bit stereo WAV
    clip with peaks below `0.995` and smooth zero-crossing edge fades into
    `Paper_SFX/`, and document its source in `Paper_SFX/CREDITS.md`.
2.  Register the relative path in `_PAPER_SFX_RELPATHS` inside
    `bundle_builder_lib.py`, `paper_sfx_files` in `BUILD`, and expected paths in
    `papercraft_pro_test.py`.
3.  Run `papercraft_pro_test` to verify RMS loudness, stereo channel separation,
    and click-free edge fades through `drfx_validator.validate_wav_bytes`.

## What's next

-   Read the [design document](design_doc.md) for rendering algorithms and
    architectural trade-offs.
-   Read the [user guide](user_guide.md) for Edit-page workflows and complete
    control ranges.

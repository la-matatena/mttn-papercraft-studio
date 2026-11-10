# PaperCraft Studio Pro developer and build guide

This guide helps engineers build, test, package, compile standalone OpenFX
bundles, and extend PaperCraft Studio Pro 3.0.0 inside Blaze or from the
exported repository archive. Version 3.0.0 ships a single OpenFX filter,
**Paper Cutout FX** (`com.papercraft.CutoutFX`).

## Before you begin

Prepare one of the following toolchains based on your build environment:

-   **Blaze inside Google3**: Access to
    `/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents` inside the
    workspace root.
-   **Standalone Linux or macOS build**: A C++17 compiler (`g++` on Linux x86-64
    or Xcode Command Line Tools `clang++` on macOS), Bash, and Python 3.10 or
    later for asset synthesis scripts.

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
:                                               : workflows, 44 Inspector parameters, SFX   :
:                                               : reference, and troubleshooting            :
| `docs/developer_guide.md`                     | Build, standalone compilation, test       |
:                                               : execution, and extension guide            :
| `ofx/papercraft_kernel.h` and                 | Platform-independent C++17 paper cutout,  |
: `ofx/papercraft_kernel.cc`                    : SDF border, intro and outro animation,    :
:                                               : and compositing kernels                   :
| `ofx/papercraft_ofx_plugin.cc`                | OpenFX 1.4 plugin registration,           |
:                                               : parameters, suites, and multi-thread      :
:                                               : dispatch                                  :
| `ofx/papercraft_ofx_test.cc`                  | GUnit OpenFX mock host and `dlopen`       |
:                                               : dynamic plugin test suite                 :
| `ofx/openfx_include/`                         | Vendored OpenFX 1.4 C headers and license |
| `ofx/Info.plist` and `ofx/openfx_sym.lds`     | macOS bundle metadata and Linux symbol    |
:                                               : export version script                     :
| `ofx/Install_PaperCraft_OpenFX_macOS.command` | OS-specific installers for macOS and      |
: and `ofx/install_linux_ofx.sh`                : Linux x86-64                              :
| `sfx_synthesizer.py`                          | Procedural synthesizer creating the 24    |
:                                               : stereo 48 kHz, 24-bit Paper SFX WAV files :
| `png_renderer.py`                             | Pure-Python PNG generator for the effect  |
:                                               : thumbnail                                 :
| `fusion_templates.py` and `drfx_validator.py` | Legacy **Paper Cutout FX** `.setting`     |
:                                               : macro builder and structural validator    :
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

1.  Add the parameter field and default value to `CutoutParams` in
    `ofx/papercraft_kernel.h`.
2.  Read and apply the field in `RenderCutoutFrame` in
    `ofx/papercraft_kernel.cc`.
3.  Define the Inspector control inside `DefineCutoutParameters`, add its
    `OfxParamHandle` slot in `PluginInstance`, bind it in
    `HandleCreateInstance`, and populate it in `ReadCutoutParams` inside
    `ofx/papercraft_ofx_plugin.cc`.
4.  Add a targeted GUnit regression test in `ofx/papercraft_ofx_test.cc`.

> [!NOTE] Keep existing parameter identifiers stable, even when their names no
> longer match their purpose. For example, the intro and outro group is still
> named `TransitionGroup` and the **Glide Angle** control is still
> `transitionAngle`, so projects saved with earlier versions keep their values.

### Add a Paper SFX Foley sound

1.  Add a synthesis preset to `synthesize_all_paper_sfx` in
    `sfx_synthesizer.py`, keeping sample format at 48 kHz, 24-bit stereo with
    peaks below `0.995`.
2.  Update expected counts or category checks in `papercraft_pro_test.py`.
3.  Run `papercraft_pro_test` with `blaze-for-agents` to verify RMS loudness,
    stereo channel separation, and click-free zero-crossing edge fades through
    `drfx_validator.validate_wav_bytes`.

## What's next

-   Read the [design document](design_doc.md) for rendering algorithms and
    architectural trade-offs.
-   Read the [user guide](user_guide.md) for Edit-page workflows and complete
    control ranges.

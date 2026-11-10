# MTTN PaperCraft Studio

> 🌐 [Español](README.md) | **English**

[![CI & Release](https://github.com/la-matatena/mttn-papercraft-studio/actions/workflows/ci-release.yml/badge.svg)](https://github.com/la-matatena/mttn-papercraft-studio/actions/workflows/ci-release.yml)
[![Latest Release](https://img.shields.io/github/v/release/la-matatena/mttn-papercraft-studio?color=8367c7&label=release)](https://github.com/la-matatena/mttn-papercraft-studio/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-8367c7.svg)](LICENSE)
[![OpenFX 1.4 C++17](https://img.shields.io/badge/OpenFX-1.4%20C%2B%2B17-6a4fb3.svg)](ofx/Info.plist)
[![DaVinci Resolve 20 | 21](https://img.shields.io/badge/DaVinci%20Resolve-20%20%7C%2021%20Free%20%26%20Studio-8367c7.svg)](docs/user_guide.md)
[![Platform: macOS & Linux](https://img.shields.io/badge/platform-macOS%20Universal%20%7C%20Linux%20x86__64-6a4fb3.svg)](docs/developer_guide.md)
[![Brand: La Matatena](https://img.shields.io/badge/La%20Matatena-MTTN-8367c7.svg)](https://github.com/la-matatena)

**MTTN PaperCraft Studio** (by **[La Matatena](https://la-matatena.com)** · [@la-matatena](https://github.com/la-matatena)) adds organic stop-motion paper cutouts, torn cardstock borders, paper intro and outro animations, and 24-bit stereo paper Foley sound effects directly on the DaVinci Resolve 20 and 21 Edit pages without opening Fusion. Both DaVinci Resolve Free and DaVinci Resolve Studio run the native OpenFX 1.4 C++17 plugin without watermarks.

## Included deliverables

| Component | Package path | Purpose |
| :--- | :--- | :--- |
| Native OpenFX bundle | `PaperCraftStudioPro.ofx.bundle` | Native C++17 **Paper Cutout FX** effect for the Edit page (`MTTN PaperCraft Studio` category) |
| macOS OpenFX installer | `Install_PaperCraft_OpenFX_macOS.command` | Compiles Universal (`x86_64` + `arm64`) or native macOS OpenFX bundle, ad-hoc signs it, and installs to `/Library/OFX/Plugins` |
| macOS OpenFX uninstaller | `Uninstall_PaperCraft_OpenFX_macOS.command` | Removes the OpenFX plugin (system or `--user`) and the `.drfx` templates from DaVinci Resolve |
| Linux OpenFX installer | `install_linux_ofx.sh` | Installs `PaperCraftStudioPro.ofx.bundle` into the Linux OpenFX directory |
| Portable OpenFX source | `OpenFX_Source/` | Standalone C++17 source files and OpenFX 1.4 headers for macOS and Linux builds |
| Paper SFX library | `Paper_SFX/` (24 stereo WAV files) | 48 kHz, 24-bit stereo Foley sounds organized across six tactile paper categories |
| Docs & design | `docs/` (`user_guide.md`, `design_doc.md`, `developer_guide.md`) | End-user guide, developer build guide, and technical design architecture |
| Legacy `.drfx` templates | `MTTN_PaperCraft_Studio.drfx` / `Templates_Unpacked/` | Validated Fusion `MacroOperator` template for **Paper Cutout FX** with a La Matatena purple thumbnail |

## Documentation

Read the full documentation set for installation steps, Edit-page workflows, and engineering architecture:

- [User guide](docs/user_guide.md): Installation steps, the Edit-page Paper Cutout FX workflow, all Inspector parameters, Paper SFX mixing table, and troubleshooting.
- [Design document](docs/design_doc.md): Architecture overview, OpenFX 1.4 host integration, linear-time signed distance field pipeline, Foley audio synthesis, and engineering trade-offs.
- [Developer and build guide](docs/developer_guide.md): Repository layout, standalone Linux and macOS compilation, and verification suites.

## Install the native Edit-page OpenFX plugin

### Install on macOS

Run DaVinci Resolve installed from the Blackmagic Design website rather than the Mac App Store edition, because Apple sandbox restrictions prevent the Mac App Store build from loading third-party OpenFX bundles in `/Library/OFX/Plugins`.

1. Download and extract `MTTN_PaperCraft_Studio_Full_Bundle.zip` from [GitHub Releases](https://github.com/la-matatena/mttn-papercraft-studio/releases/latest) (or clone this repository).
2. Double-click `Install_PaperCraft_OpenFX_macOS.command`, or run it from **Terminal** inside the extracted folder (type `cd `, drag the folder onto the Terminal window, and press Return). In the ZIP the script sits at the top of the folder; in a cloned repository it lives in `ofx/`:

   ```bash
   ./Install_PaperCraft_OpenFX_macOS.command        # Releases ZIP
   ./ofx/Install_PaperCraft_OpenFX_macOS.command    # cloned repository
   ```

   If you prefer user-level installation without administrator privileges, pass `--user` to install into `${HOME}/Library/OFX/Plugins`:

   ```bash
   ./Install_PaperCraft_OpenFX_macOS.command --user
   ```

3. Approve the macOS Command Line Tools prompt if `clang++` is not installed yet, then rerun `Install_PaperCraft_OpenFX_macOS.command`.
4. Restart DaVinci Resolve to scan the OpenFX bundle.

### Uninstall on macOS

1. Quit DaVinci Resolve.
2. Double-click `Uninstall_PaperCraft_OpenFX_macOS.command` (inside `ofx/` in a cloned repository). It removes the OpenFX plugin, whether you installed it system-wide or with `--user`, plus the optional `.drfx` templates. If the plugin is in `/Library/OFX/Plugins`, it asks for your administrator password.

   To preview what would be removed without deleting anything:

   ```bash
   ./Uninstall_PaperCraft_OpenFX_macOS.command --dry-run
   ```

3. Open DaVinci Resolve: the **MTTN PaperCraft Studio** category is gone. Clips that used these effects show them as missing; remove them from your timelines.

### Install on Linux

1. Extract `MTTN_PaperCraft_Studio_Full_Bundle.zip`.
2. Run the Linux OpenFX installer script:

   ```bash
   ./install_linux_ofx.sh
   ```

   Pass `--user` to install into `${HOME}/.local/share/OFX/Plugins` without root privileges, or `--dest CUSTOM_DIR` for a custom location:

   ```bash
   ./install_linux_ofx.sh --dest CUSTOM_DIR
   ```

3. Restart DaVinci Resolve to load the Edit-page OpenFX suite.

## Use MTTN PaperCraft Studio on the Edit page

The OpenFX bundle registers a single effect, **Paper Cutout FX**, in the **MTTN PaperCraft Studio** category. Open **Effects** > **Open FX** > **MTTN PaperCraft Studio** and drag **Paper Cutout FX** directly onto any timeline clip, title, transparent PNG, or green-screen shot.

> [!NOTE]
> Version 3.0 removed the Paper Cutout FX generator and the **Paper Unfold**, **Paper Crumple**, and **Paper Rip Wipe** transitions. If an older project used them, DaVinci Resolve shows them as unavailable; remove them from the timeline.

## Inspector controls reference

| Section | Key controls | Behavior |
| :--- | :--- | :--- |
| **1. Media Source and Placement** | **Source Mode**, **Premultiplied Source Alpha**, **Media Scale**, **Media Position**, **Media Rotation** | Switches between timeline clip input and built-in La Matatena sticker, and frames artwork inside the cutout |
| **2. Green Screen and Matte Cleanup** | **Enable Green Screen Key**, **Screen Color**, **Key Gain**, **Key Balance**, **Edge Choke (px)** | Extracts subject matte from solid green or blue backdrops with spill suppression |
| **3. Torn Paper Cutout Border** | **Enable Paper Border**, **Border Width**, **Torn Edge Roughness**, **Tear Frequency**, **Tear Fiber Detail**, **Edge Boil Speed**, **Paper Cardstock Color** | Expands a procedural ragged cardstock collar around any extracted alpha shape |
| **4. Paper Grain and Fold Creases** | **Paper Grain Strength**, **Grain Scale**, **Fold Crease Strength**, **Crease Scale**, **Media Texture Overlay**, **Texture Seed** | Adds multi-octave fiber grain and ridged fold creases |
| **5. Intro and Outro Paper Animations** | **Intro Style**, **Intro Duration (Frames)**, **Outro Style**, **Outro Duration (Frames)**, **Glide Angle**, **Glide Distance**, **Mirror Animation H**, **Mirror Animation V** | Animates **Paper Unfold (3D)**, **Crumple Ball**, **Paper Airplane Glide**, or **Flat Cutout Pop** entrances and exits |
| **6. 3D Drop Shadow** | **Shadow Opacity**, **Shadow Angle**, **Shadow Distance**, **Shadow Softness** | Casts soft directional depth shadows behind the rotating cutout |
| **7. Stop-Motion and Master Transform** | **Enable Stop-Motion**, **Hold Frames (Step)**, **Position Jitter**, **Rotation Jitter**, **Master Position**, **Master Scale**, **Master Rotation**, **Flip Horizontal**, **Flip Vertical** | Quantizes frame motion into stepped holds and adds subtle frame-by-frame tactile wobble |

## Paper SFX starter library

Import the `Paper_SFX/` directory into a DaVinci Resolve Power Bin to reuse all 24 stereo 48 kHz 24-bit WAV files across projects:

- `01_Page_Turns/`: Four crisp book, cardstock, broadsheet, and quick flip turns.
- `02_Paper_Hits_Impacts/`: Four table slaps, cardboard drops, sticker snaps, and stack thuds.
- `03_Crumples_Uncrumples/`: Four tight crumple balls, newspaper crinkles, and smooth or quick uncrumples.
- `04_Rips_Tears/`: Four fast tears, slow fiber rips, thick kraft rips, and short snips.
- `05_Little_Bits_Rustles/`: Four confetti scatters, stop-motion jiggles, desk scrapes, and finger folds.
- `06_Designed_Transitions/`: Four layered paper whooshes, airplane swooshes, crumple slams, and stop-motion pops.

## Optional legacy `.drfx` templates

The validated `MTTN_PaperCraft_Studio.drfx` archive remains included for Fusion-template workflows. Double-click `MTTN_PaperCraft_Studio.drfx` to install the Edit-page **Paper Cutout FX** `MacroOperator` copy under **Effects** > **Toolbox** > **MTTN PaperCraft Studio**. For everyday Edit-page timelines, prefer the native OpenFX bundle because it executes in multi-threaded C++17 without Fusion graph overhead.

## Improve timeline playback and renders

- Set **Playback** > **Render Cache** to **Smart** or **User** after finalizing your cutout parameters.
- Lower **Playback** > **Timeline Proxy Resolution** to **Half** or **Quarter** when timing stop-motion animations.

## Build & verify from source

Run the Python `.drfx`, PNG thumbnail, Foley WAV, and bundle verification suite:

```bash
python3 -B papercraft_pro_test.py -v
```

Build the macOS Universal OpenFX bundle into a staging directory without touching `/Library/OFX/Plugins`:

```bash
./ofx/Install_PaperCraft_OpenFX_macOS.command --build-only ./dist
```

Assemble the full release bundle (`.drfx`, 24 Foley WAVs, installers, docs, and source archive):

```bash
python3 -B build_papercraft_bundle.py \
  --output_dir=dist \
  --readme_path=README.md \
  --linux_ofx_path=PaperCraftStudioPro.ofx
```

## License

Released under the [MIT License](LICENSE) — Copyright (c) 2026 [La Matatena](https://la-matatena.com). Bundled OpenFX 1.4 C headers retain their permissive license in [`ofx/openfx_include/LICENSE`](ofx/openfx_include/LICENSE).

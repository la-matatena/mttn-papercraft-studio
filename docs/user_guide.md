# MTTN PaperCraft Studio user guide

This guide helps video editors install MTTN PaperCraft Studio 3.1.0, apply the **Paper Cutout FX** torn paper filter and the **Paper Fold & Unfold** transition on the DaVinci Resolve 20 and 21 Edit pages, tune Inspector controls, and mix the four included Paper SFX audio clips.

> [!IMPORTANT]
> Version 3.0 removed the **Paper Cutout FX** generator and the **Paper Unfold Transition**, **Paper Crumple Transition**, and **Paper Rip Wipe Transition** effects. DaVinci Resolve shows these items as unavailable in projects created with earlier versions, so remove them from your timelines. Version 3.1 adds a new transition, **Paper Fold & Unfold**, with its own identifier.

## Before you begin

Review platform and edition requirements before installing the bundle:

- **DaVinci Resolve edition**: Both DaVinci Resolve Free and DaVinci Resolve Studio run the native OpenFX 1.4 plugin with no watermarks or resolution locks.
- **macOS edition**: Install DaVinci Resolve directly from the Blackmagic Design website. The Mac App Store edition runs inside an Apple sandbox that blocks third-party OpenFX bundles stored in `/Library/OFX/Plugins`.
- **macOS hardware**: DaVinci Resolve 21 runs on Apple Silicon (`arm64`) hardware, whereas DaVinci Resolve 20 supports both Apple Silicon and Intel (`x86_64`) Macs. The macOS installer builds a Universal binary when `clang++` supports both architectures and falls back to your active host architecture otherwise.
- **Linux hardware**: The prebuilt `PaperCraftStudioPro.ofx.bundle` targets Linux x86-64 workstations running DaVinci Resolve 20 or 21.

## Install the OpenFX plugin

Choose the installer matching your operating system. Restart DaVinci Resolve after installation so it scans the OpenFX directory.

### Install on macOS

1. Extract the release archive (`MTTN_PaperCraft_Studio_Full_Bundle.zip` from GitHub Releases, or clone the repository).
2. Run the macOS installer script in **Terminal**, or double-click `Install_PaperCraft_OpenFX_macOS.command` to install system-wide into `/Library/OFX/Plugins`. In the extracted release ZIP the script sits at the root; in a cloned repository it lives under `ofx/`:

   ```bash
   ./Install_PaperCraft_OpenFX_macOS.command        # extracted release ZIP
   ./ofx/Install_PaperCraft_OpenFX_macOS.command    # cloned repository
   ```

   To install into your user directory (`${HOME}/Library/OFX/Plugins`) without administrator privileges, pass the `--user` flag:

   ```bash
   ./Install_PaperCraft_OpenFX_macOS.command --user
   ```

3. If macOS prompts you to install Xcode Command Line Tools (`clang++`), approve the dialog and run the installer command again.
4. Restart DaVinci Resolve.

### Uninstall on macOS

1. Quit DaVinci Resolve.
2. Double-click `Uninstall_PaperCraft_OpenFX_macOS.command` (inside `ofx/` in a cloned repository), or run it in **Terminal**. It removes the OpenFX bundle from `/Library/OFX/Plugins` and `${HOME}/Library/OFX/Plugins`, plus any installed `.drfx` templates, and asks for your administrator password when needed:

   ```bash
   ./Uninstall_PaperCraft_OpenFX_macOS.command
   ```

   Pass `--dry-run` to list what would be removed without deleting anything.
3. Restart DaVinci Resolve.

### Install on Linux

1. Extract `MTTN_PaperCraft_Studio_Full_Bundle.zip` containing `PaperCraftStudioPro.ofx.bundle` and `install_linux_ofx.sh`.
2. Install system-wide into `/usr/OFX/Plugins` with administrator privileges, or install into `${HOME}/.local/share/OFX/Plugins` without root access by passing `--user`:

   ```bash
   ./install_linux_ofx.sh --user
   ```

   To install into a custom plugin directory, pass `--dest CUSTOM_DIR`:

   ```bash
   ./install_linux_ofx.sh --dest CUSTOM_DIR
   ```

   Replace the following:

   - `CUSTOM_DIR`: the filesystem directory where you want to store `PaperCraftStudioPro.ofx.bundle`.
3. Restart DaVinci Resolve.

### Optional: Install legacy Fusion templates

To load the optional Fusion `MacroOperator` templates instead of (or alongside) the native OpenFX bundle, double-click `MTTN_PaperCraft_Studio.drfx` and approve the DaVinci Resolve prompt. The `.drfx` file contains two templates:

- The **Paper Cutout FX** effect, in the **Effects** library under **Effects** > **Toolbox** > **MTTN PaperCraft Studio**.
- The **Paper Fold & Unfold** transition, under **Effects** > **Toolbox** > **Video Transitions** > **MTTN PaperCraft Studio**. It is a lighter three-panel letter fold with paper backs, creases, a sliding packet, stop-motion holds, and a drop shadow. It has no torn border, fold patterns, or **Auto** action, so pick **Fold Action** yourself.

### Remove the OpenFX bundle manually

To uninstall the native plugin manually on macOS or Linux, remove the `PaperCraftStudioPro.ofx.bundle` directory from the OpenFX path you selected, then restart DaVinci Resolve:

```bash
# macOS (remove whichever path you installed to)
sudo rm -rf "/Library/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"
rm -rf "${HOME}/Library/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"

# Linux (remove whichever path you installed to)
sudo rm -rf "/usr/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"
rm -rf "${HOME}/.local/share/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"
```

## Create animated paper cutouts on the Edit page

The OpenFX bundle registers a filter, **Paper Cutout FX**, which you apply to clips on the Edit page, and a transition, **Paper Fold & Unfold**, which you place on edit points.

### Apply Paper Cutout FX to a timeline clip

1. On the **Edit** page, select a video clip, transparent PNG graphic, or title on Video Track 1 or higher (`V2+`).
2. Open **Effects** > **Open FX** > **MTTN PaperCraft Studio**.
3. Drag **Paper Cutout FX** onto the clip.
4. Open the **Inspector** panel and click the **Effects** tab to adjust cardstock width, tear frequency, entrance and exit styles, and stop-motion stepping.

> [!NOTE]
> Upper-track clips (`V2+`) preserve transparent alpha channels around the torn cardstock collar and drop shadow so lower timeline tracks stay visible.

### Key green-screen or blue-screen footage

1. Apply **Paper Cutout FX** to a clip filmed against a solid backdrop.
2. In **Inspector** > **Effects**, expand **2. Green Screen & Matte Cleanup**.
3. Select **Enable Green Screen Key**.
4. Pick the backdrop hue with **Screen Color**, adjust separation with **Key Gain** and **Key Balance**, and refine the silhouette edge with **Edge Choke (px)** before the torn paper border is expanded.

### Show the built-in paper sticker

To preview the look without your own artwork, render the procedural La Matatena demo sticker inside the filter:

1. Apply **Paper Cutout FX** to any clip on the timeline.
2. In **Inspector** > **Effects** > **1. Media Source & Placement**, set **Source Mode** to `Built-in Paper Sticker`.
3. Customize paper texture, animation style, and cutout position in the remaining groups.

### Fold the cutout with Paper Unfold (3D)

The **Paper Unfold (3D)** intro and outro style folds the finished cutout along real creases: the cutout arrives as a small folded packet, its halves and corners flip open with the paper back showing, and it settles flat. Round or irregular cutouts fold across their own outline, so corner flaps stay visible. **Glide Angle** also rotates the fold direction (its default, `-22°`, keeps the folds upright), and **Mirror Animation H** and **Mirror Animation V** mirror the fold layout.

## Fold and unfold clips with Paper Fold & Unfold

**Paper Fold & Unfold** is an Edit-page transition that makes a clip unfold from a folded paper packet, or fold up and leave the frame. It works on full-frame video and on clips with transparency, such as PNG logos and titles on upper tracks.

### Apply the transition

1. On the **Edit** page, open **Effects** > **Open FX** > **Transitions** > **MTTN PaperCraft Studio**.
2. Drag **Paper Fold & Unfold** onto an edit point: the start of a clip, the end of a clip, or the cut between two clips.
3. Drag the transition edges on the timeline to set its length. Longer transitions unfold more slowly.
4. Select the transition and open **Inspector** > **Transition** to adjust the fold pattern, paper look, stop-motion steps, and shadow.

The first and last frames of the transition always match the outgoing and incoming clips exactly, so cuts around it don't jump.

### Choose what folds and what unfolds

With **Fold Action** set to **Auto (Unfold In / Fold Out)**, the transition reads both clips and decides:

| Where you place it | What Auto does |
| :--- | :--- |
| At the start of a clip, with nothing before it | The clip unfolds into view |
| At the end of a clip, with nothing after it | The clip folds up and slides out of the frame |
| Between two full-frame clips | The incoming clip unfolds over the outgoing clip |
| Between clips with transparency (PNGs, titles) | The outgoing clip folds away, then the incoming clip unfolds |

To force a behavior, set **Fold Action** to **Unfold Incoming Clip**, **Fold Outgoing Clip**, or **Fold Out, Then Unfold In**.

### Unfold a logo over video

1. Place a transparent PNG or title on Video Track 2 (`V2`) above your video.
2. Drag **Paper Fold & Unfold** onto the start of the PNG clip. The logo unfolds over the video on `V1`, and the paper back, collar, and shadow keep their transparency.
3. To unfold the logo in place instead of sliding it in, set **Packet Slide Distance** to `0`.
4. Drag a second **Paper Fold & Unfold** onto the end of the PNG clip to fold the logo away.

### Combine Paper Cutout FX and Paper Fold & Unfold on the same clip

You can apply **Paper Cutout FX** to a clip and drag **Paper Fold & Unfold** onto its start, end, or cut without duplicate borders or folded shadows:

- **Paper Cutout FX** defaults **Intro Style** and **Outro Style** to `None`, so its built-in entrance and exit animations stay off unless you choose one.
- **Paper Fold & Unfold** automatically detects clips that already carry a cutout and drop shadow: it folds the existing cutout silhouette (keeping one constant torn border and matching ragged paper backsides) and hands its 3D packet shadow directly into the cutout's resting shadow when the transition finishes.

> [!TIP]
> Pair the transition with `01_Paper_Unfold.wav` from the Paper SFX library, and use **Stop-Motion Steps** to match the number of poses to your project frame rate (about one pose every two or three frames looks like classic stop-motion).

## Complete Inspector controls reference

The tables that follow list all controls exposed in the DaVinci Resolve Inspector for **Paper Cutout FX** and **Paper Fold & Unfold**.

### Paper Cutout FX controls

| Group | Control label | Type and range | Default | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **1. Media Source & Placement** | **Source Mode** | Choice (`Timeline Clip`, `Built-in Paper Sticker`) | `Timeline Clip` | Switches between timeline clip media and procedural sticker |
| **1. Media Source & Placement** | **Premultiplied Source Alpha** | Boolean | `True` | Disable if straight-alpha graphics show dark or bright edge fringes |
| **1. Media Source & Placement** | **Media Scale** | Scale (`0.05` to `5.0`) | `0.90` | Scales artwork inside the paper frame |
| **1. Media Source & Placement** | **Media Position** | XY Point (`0.0` to `1.0`) | `0.5, 0.5` | Repositions the source subject inside the frame |
| **1. Media Source & Placement** | **Media Rotation** | Angle (`-3600°` to `3600°`) | `0.0°` | Rotates the subject prior to collar extraction |
| **2. Green Screen & Matte Cleanup** | **Enable Green Screen Key** | Boolean | `False` | Turns on solid-backdrop chroma keying and spill suppression |
| **2. Green Screen & Matte Cleanup** | **Screen Color** | RGB color picker | `0.0, 1.0, 0.0` (green) | Selects the key color to remove |
| **2. Green Screen & Matte Cleanup** | **Key Gain** | Float (`0.0` to `3.0`) | `1.0` | Controls matte contrast against the keyed backdrop |
| **2. Green Screen & Matte Cleanup** | **Key Balance** | Float (`0.0` to `1.0`) | `0.5` | Shifts the soft key threshold midpoint |
| **2. Green Screen & Matte Cleanup** | **Edge Choke (px)** | Float (`-32.0` to `32.0`) | `0.0` | Shrinks (negative) or expands (positive) the extracted matte |
| **3. Torn Paper Cutout Border** | **Enable Paper Border** | Boolean | `True` | Wraps a procedural ragged cardstock collar around the matte |
| **3. Torn Paper Cutout Border** | **Border Width** | Float (`0.0` to `0.15`) | `0.018` | Sets collar thickness relative to frame height |
| **3. Torn Paper Cutout Border** | **Torn Edge Roughness** | Float (`0.0` to `0.20`) | `0.012` | Controls amplitude of torn edge teeth |
| **3. Torn Paper Cutout Border** | **Tear Frequency** | Float (`1.0` to `120.0`) | `24.0` | Sets spatial density of paper teeth along the edge |
| **3. Torn Paper Cutout Border** | **Tear Fiber Detail** | Integer (`1` to `8`) | `5` | Sets octave count for torn paper micro-fibers |
| **3. Torn Paper Cutout Border** | **Edge Boil Speed** | Float (`0.0` to `5.0`) | `0.15` | Animates torn edge variation across stop-motion steps |
| **3. Torn Paper Cutout Border** | **Paper Cardstock Color** | RGB color picker | `0.965, 0.945, 0.895` | Sets the warm paper border fill color |
| **4. Paper Grain & Fold Creases** | **Paper Grain Strength** | Float (`0.0` to `1.0`) | `0.35` | Adjusts cardstock fiber grain depth |
| **4. Paper Grain & Fold Creases** | **Grain Scale** | Scale (`0.1` to `10.0`) | `1.0` | Changes size of surface fiber grain |
| **4. Paper Grain & Fold Creases** | **Fold Crease Strength** | Float (`0.0` to `1.0`) | `0.40` | Controls contrast of ridged fold creases |
| **4. Paper Grain & Fold Creases** | **Crease Scale** | Float (`0.2` to `30.0`) | `3.2` | Controls spacing of folded paper creases |
| **4. Paper Grain & Fold Creases** | **Media Texture Overlay** | Float (`0.0` to `1.0`) | `0.22` | Blends subtle crease shading over subject ink |
| **4. Paper Grain & Fold Creases** | **Texture Seed** | Integer (`0` to `1000`) | `7` | Varies deterministic tear, crease, and jitter patterns |
| **5. Intro & Outro Paper Animations** | **Intro Style** | Choice (`None`, `Paper Unfold (3D)`, `Crumple Ball`, `Paper Airplane Glide`, `Flat Cutout Pop`) | `None` | Selects opening clip entrance animation |
| **5. Intro & Outro Paper Animations** | **Intro Duration (Frames)** | Integer (`0` to `240`) | `14` | Sets opening animation duration in frames |
| **5. Intro & Outro Paper Animations** | **Outro Style** | Choice (same five styles as **Intro Style**) | `None` | Selects closing clip exit animation |
| **5. Intro & Outro Paper Animations** | **Outro Duration (Frames)** | Integer (`0` to `240`) | `14` | Sets closing animation duration in frames |
| **5. Intro & Outro Paper Animations** | **Glide Angle** | Angle (`-360°` to `360°`) | `-22.0°` | Sets directional trajectory for glide moves and fold direction |
| **5. Intro & Outro Paper Animations** | **Glide Distance** | Float (`0.0` to `3.0`) | `0.65` | Sets travel distance in normalized frame units |
| **5. Intro & Outro Paper Animations** | **Mirror Animation H** | Boolean | `False` | Flips intro and outro movement horizontally |
| **5. Intro & Outro Paper Animations** | **Mirror Animation V** | Boolean | `False` | Flips intro and outro movement vertically |
| **6. 3D Drop Shadow** | **Shadow Opacity** | Float (`0.0` to `1.0`) | `0.62` | Controls cast shadow intensity behind the sheet |
| **6. 3D Drop Shadow** | **Shadow Angle** | Angle (`-360°` to `360°`) | `-45.0°` | Controls directional shadow throw angle |
| **6. 3D Drop Shadow** | **Shadow Distance** | Float (`0.0` to `0.20`) | `0.016` | Sets shadow offset distance behind the paper layer |
| **6. 3D Drop Shadow** | **Shadow Softness** | Float (`0.0` to `0.20`) | `0.018` | Controls shadow feather falloff width |
| **7. Stop-Motion & Master Transform** | **Enable Stop-Motion** | Boolean | `True` | Steps animation timing on discrete hold intervals |
| **7. Stop-Motion & Master Transform** | **Hold Frames (Step)** | Integer (`1` to `30`) | `3` | Sets frames per stop-motion step (for example, `3` for an 8 fps cadence on a 24 fps timeline) |
| **7. Stop-Motion & Master Transform** | **Position Jitter** | Float (`0.0` to `0.1`) | `0.0025` | Adds hand-placed positional wobble per step |
| **7. Stop-Motion & Master Transform** | **Rotation Jitter** | Angle (`0.0°` to `30.0°`) | `0.75°` | Adds hand-placed rotational wobble per step |
| **7. Stop-Motion & Master Transform** | **Master Position** | XY Point (`0.0` to `1.0`) | `0.5, 0.5` | Repositions the finished paper cutout on screen |
| **7. Stop-Motion & Master Transform** | **Master Scale** | Scale (`0.01` to `10.0`) | `1.0` | Scales the finished cutout uniformly |
| **7. Stop-Motion & Master Transform** | **Master Rotation** | Angle (`-3600°` to `3600°`) | `0.0°` | Rotates the finished cutout cleanly |
| **7. Stop-Motion & Master Transform** | **Flip Horizontal** | Boolean | `False` | Mirrors the finished cutout left to right |
| **7. Stop-Motion & Master Transform** | **Flip Vertical** | Boolean | `False` | Mirrors the finished cutout top to bottom |

### Paper Fold & Unfold controls

DaVinci Resolve drives the transition's **Progress** parameter from the timeline, so you don't need to keyframe it.

| Group | Control | Type and range | Default | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **1. Fold & Unfold** | **Fold Action** | Choice (`Auto (Unfold In / Fold Out)`, `Unfold Incoming Clip`, `Fold Outgoing Clip`, `Fold Out, Then Unfold In`) | `Auto` | Chooses which clip unfolds or folds |
| **1. Fold & Unfold** | **Fold Pattern** | Choice (`Craft Flaps`, `Map Grid`, `Accordion`, `Origami Corners`) | `Craft Flaps` | Sets how the paper folds into a packet: quarter folds with dog-eared corners, map panels, zigzag pleats, or envelope corners |
| **1. Fold & Unfold** | **Fold Stages** | Integer (`2` to `5`) | `4` | Sets how many folds open one after another |
| **1. Fold & Unfold** | **Fold Direction** | Angle (`-360°` to `360°`) | `0°` | Rotates every fold line |
| **1. Fold & Unfold** | **3D Depth** | Float (`0.0` to `2.0`) | `0.55` | Sets the perspective of lifted flaps; `0` keeps them flat |
| **1. Fold & Unfold** | **Packet Slide Distance** | Float (`0.0` to `2.0`) | `0.6` | Sets how far the folded packet slides in or out, in frame diagonals; `0` unfolds in place |
| **1. Fold & Unfold** | **Mirror Horizontal**, **Mirror Vertical** | Boolean | `False` | Mirror the fold layout |
| **2. Paper Look** | **Torn Paper Border** | Boolean | `True` | Adds a torn cardstock collar while the sheet is folded |
| **2. Paper Look** | **Border Width** | Float (`0.0` to `0.15`) | `0.018` | Sets the collar width relative to the frame height |
| **2. Paper Look** | **Torn Edge Roughness** | Float (`0.0` to `0.20`) | `0.012` | Sets the amplitude of the torn edge teeth |
| **2. Paper Look** | **Tear Frequency** | Float (`1.0` to `120.0`) | `24.0` | Sets the density of the paper tears |
| **2. Paper Look** | **Paper Color** | RGB color picker | `0.965, 0.945, 0.895` | Colors the paper back and collar |
| **2. Paper Look** | **Paper Grain** | Float (`0.0` to `1.0`) | `0.38` | Sets the cardstock fiber grain depth |
| **2. Paper Look** | **Crease Strength** | Float (`0.0` to `1.0`) | `0.52` | Sets the crumple texture and the crease lines along each fold |
| **2. Paper Look** | **Paper Seed** | Integer (`0` to `1000`) | `7` | Varies tears, creases, jitter, and fold angles |
| **2. Paper Look** | **Premultiplied Source Alpha** | Boolean | `True` | Clear it only if your clips have straight alpha |
| **3. Stop-Motion & Shadow** | **Enable Stop-Motion** | Boolean | `True` | Holds each pose like a stop-motion camera |
| **3. Stop-Motion & Shadow** | **Stop-Motion Steps** | Integer (`2` to `60`) | `10` | Sets the number of distinct poses across the transition |
| **3. Stop-Motion & Shadow** | **Hand Jitter** | Float (`0.0` to `1.0`) | `0.25` | Adds a small per-pose position and rotation wobble |
| **3. Stop-Motion & Shadow** | **Shadow Opacity** | Float (`0.0` to `1.0`) | `0.62` | Sets the drop shadow and flap shadow density |
| **3. Stop-Motion & Shadow** | **Shadow Angle** | Angle (`-360°` to `360°`) | `-45°` | Sets the shadow direction and lights the flaps from the opposite side |
| **3. Stop-Motion & Shadow** | **Shadow Distance** | Float (`0.0` to `0.20`) | `0.014` | Sets the drop shadow offset; it grows while the packet is lifted |
| **3. Stop-Motion & Shadow** | **Shadow Softness** | Float (`0.0` to `0.20`) | `0.020` | Sets the drop shadow feather |

The collar, crease lines, jitter, and drop shadow fade out over the last part of an unfold (and fade in at the start of a fold), so the clean clip is never covered when the transition ends.

## Mix Foley audio with the Paper SFX library

All four WAV files in `Paper_SFX/` are real microphone recordings standardized to 48 kHz, 24-bit stereo clips with smooth zero-crossing fades and public-domain licensing (`CC0 1.0` and `Public Domain`; see [audio credits](../Paper_SFX/CREDITS.md)). Drag the `Paper_SFX/` folder into a DaVinci Resolve **Power Bin** to keep every clip accessible across projects.

| WAV file in `Paper_SFX/` | Tactile paper gesture | Matching visual preset |
| :--- | :--- | :--- |
| `01_Paper_Unfold.wav` | Crisp folded sheet spread and settle | **Paper Fold & Unfold** and **Paper Unfold (3D)** |
| `02_Paper_Crumple.wav` | Organic stereo paper-ball crumple | **Crumple Ball** intro and outro |
| `03_Paper_Glide_Flip.wav` | Fast page flip and air swoosh | **Paper Airplane Glide** and quick cuts |
| `04_Paper_Rip_Tear.wav` | Clean dry fiber tear | Torn cardstock border and **Flat Cutout Pop** |

## Improve timeline playback performance

For smooth 4K timeline playback on CPU OpenFX tracks:

- Turn on **Playback** > **Render Cache** > **Smart** or **User** after locking in cutout or transition parameters.
- Lower **Playback** > **Timeline Proxy Resolution** to **Half** or **Quarter** while timing animation beats.
- **Paper Cutout FX** renders about 30% faster in 3.1.0 than in 3.0.0 (~7.0 ms at 1080p at rest on an Apple M4 Pro Mac), and **Paper Fold & Unfold** averages about 11 ms per 1080p frame on the same Mac (~27 ms on four threads). Modest machines, such as a 2017 iMac, benefit from **Render Cache** for real-time playback.

## Troubleshoot common issues

| Symptom | Cause | Resolution |
| :--- | :--- | :--- |
| Plugin missing under **Effects** on macOS | Mac App Store Resolve edition blocks third-party bundles | Install DaVinci Resolve directly from the Blackmagic Design website and restart |
| Dark or bright halo around straight-alpha PNG stickers | Straight RGB channels filtered before premultiplication | Clear **Premultiplied Source Alpha** in **1. Media Source & Placement** |
| Effect shown as unavailable after upgrading to 3.0+ | Project uses the generator or a transition removed in 3.0 | Delete the unavailable generator or transition from the timeline, then use **Paper Cutout FX** intro and outro styles or the **Paper Fold & Unfold** transition instead |
| Paper Fold & Unfold folds when you expected an unfold | Auto read the clips differently than expected | Set **Fold Action** to the behavior you want instead of **Auto** |
| Paper Fold & Unfold missing from the transitions list | OpenFX transitions are listed apart from built-in ones | Look under **Effects** > **Open FX** > **Transitions** > **MTTN PaperCraft Studio**, not under **Video Transitions** |
| Green spill or rough halo on keyed subjects | Unkeyed background pixels entering the signed distance field collar | Increase **Key Gain** and shift **Edge Choke (px)** slightly negative (`-1.5` to `-3.0`) |
| `clang++ not found` during macOS install | Xcode Command Line Tools missing | Run `xcode-select --install`, finish installation, and run `./Install_PaperCraft_OpenFX_macOS.command` again |

## What's next

- Read the [design document](design_doc.md) to explore the signed distance field, perspective warping, and curated Foley audio pipeline.
- Read the [developer and build guide](developer_guide.md) to compile from source or extend the codebase.

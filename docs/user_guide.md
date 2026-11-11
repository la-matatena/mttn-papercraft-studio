# PaperCraft Studio Pro user guide

This guide helps video editors install PaperCraft Studio Pro 3.1.0, apply the
**Paper Cutout FX** torn paper filter and the **Paper Fold & Unfold** transition
on the DaVinci Resolve 20 and 21 Edit pages, tune Inspector controls, and mix
the four included Paper SFX audio clips.

> [!IMPORTANT] Version 3.0 removes the **Paper Cutout FX** generator and the
> **Paper Unfold Transition**, **Paper Crumple Transition**, and **Paper Rip
> Wipe Transition** effects. DaVinci Resolve shows these items as unavailable in
> projects created with earlier versions, so remove them from your timelines.
> Version 3.1 adds a new transition, **Paper Fold & Unfold**, with its own
> identifier.

## Before you begin

Review platform and edition requirements before installing the bundle:

-   **DaVinci Resolve edition**: Both DaVinci Resolve Free and DaVinci Resolve
    Studio run the native OpenFX 1.4 plugin with no watermarks or resolution
    locks.
-   **macOS edition**: Install DaVinci Resolve directly from the Blackmagic
    Design website. The Mac App Store edition runs inside an Apple sandbox that
    blocks third-party OpenFX bundles stored in `/Library/OFX/Plugins`.
-   **macOS hardware**: DaVinci Resolve 21 runs on Apple Silicon (`arm64`)
    hardware, whereas DaVinci Resolve 20 supports both Apple Silicon and Intel
    (`x86_64`) Macs. The macOS installer builds a Universal binary when
    `clang++` supports both architectures and falls back to your active host
    architecture otherwise.
-   **Linux hardware**: The prebuilt `PaperCraftStudioPro.ofx.bundle` targets
    Linux x86-64 workstations running DaVinci Resolve 20 or 21.

## Install the OpenFX plugin

Choose the installer matching your operating system. Restart DaVinci Resolve
after installation so it scans the OpenFX directory.

### Install on macOS

1.  Extract the release archive (`PaperCraft_Studio_Pro_Full_Bundle.zip` or the
    `release/` directory inside `PaperCraft_Studio_Pro_Complete_Repo.zip`).
2.  Run the macOS installer script in **Terminal**, or double-click
    `Install_PaperCraft_OpenFX_macOS.command` to install system-wide into
    `/Library/OFX/Plugins`:

    ```bash
    ./Install_PaperCraft_OpenFX_macOS.command
    ```

    To install into your user directory (`${HOME}/Library/OFX/Plugins`) without
    administrator privileges, pass the `--user` flag:

    ```bash
    ./Install_PaperCraft_OpenFX_macOS.command --user
    ```

3.  If macOS prompts you to install Xcode Command Line Tools (`clang++`),
    approve the dialog and run the installer command again.

4.  Restart DaVinci Resolve.

### Uninstall on macOS

1.  Quit DaVinci Resolve.
2.  Double-click `Uninstall_PaperCraft_OpenFX_macOS.command`, or run it in
    **Terminal**. It removes the OpenFX bundle from `/Library/OFX/Plugins` and
    `${HOME}/Library/OFX/Plugins`, plus any installed `.drfx` templates, and
    asks for your administrator password when needed:

    ```bash
    ./Uninstall_PaperCraft_OpenFX_macOS.command
    ```

    Pass `--dry-run` to list what would be removed without deleting anything.

3.  Restart DaVinci Resolve.

### Install on Linux

1.  Extract the release archive containing `PaperCraftStudioPro.ofx.bundle` and
    `install_linux_ofx.sh`.
2.  Install system-wide into `/usr/OFX/Plugins` with administrator privileges,
    or install into `${HOME}/.local/share/OFX/Plugins` without root access by
    passing `--user`:

    ```bash
    ./install_linux_ofx.sh --user
    ```

    To install into a custom plugin directory, pass `--dest CUSTOM_DIR`:

    ```bash
    ./install_linux_ofx.sh --dest CUSTOM_DIR
    ```

    Replace the following:

    -   `CUSTOM_DIR`: the filesystem directory where you want to store
        `PaperCraftStudioPro.ofx.bundle`.

3.  Restart DaVinci Resolve.

### Optional: Install legacy Fusion templates

To load the optional Fusion `MacroOperator` templates instead of (or alongside)
the native OpenFX bundle, double-click `PaperCraft_Studio_Pro.drfx` and approve
the DaVinci Resolve prompt. The `.drfx` file contains two templates:

-   The **Paper Cutout FX** effect, in the **Effects** library under the **MTTN
    PaperCraft Studio** category.
-   The **Paper Fold & Unfold** transition, under **Effects** > **Toolbox** >
    **Video Transitions** > **MTTN PaperCraft Studio**. It is a lighter
    three-panel letter fold with paper backs, creases, a sliding packet,
    stop-motion holds, and a drop shadow. It has no torn border, fold patterns,
    or **Auto** action, so pick **Fold Action** yourself.

### Remove the OpenFX bundle

To uninstall the native plugin, remove the `PaperCraftStudioPro.ofx.bundle`
directory from the OpenFX path you selected, then restart DaVinci Resolve:

```bash
# macOS (remove whichever path you installed to)
sudo rm -rf "/Library/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"
rm -rf "${HOME}/Library/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"

# Linux (remove whichever path you installed to)
sudo rm -rf "/usr/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"
rm -rf "${HOME}/.local/share/OFX/Plugins/PaperCraftStudioPro.ofx.bundle"
```

## Create animated paper cutouts on the Edit page

The OpenFX bundle registers a filter, **Paper Cutout FX**, which you apply to
clips on the Edit page, and a transition, **Paper Fold & Unfold**, which you
place on edit points.

### Apply Paper Cutout FX to a timeline clip

1.  On the **Edit** page, select a video clip, transparent PNG graphic, or title
    on Video Track 1 or higher (`V2+`).
2.  Open **Effects** > **Open FX** > **MTTN PaperCraft Studio**.
3.  Drag **Paper Cutout FX** onto the clip.
4.  Open the **Inspector** panel and click the **Effects** tab to adjust
    cardstock width, tear frequency, entrance and exit styles, and stop-motion
    stepping.

> [!NOTE] Upper-track clips (`V2+`) preserve transparent alpha channels around
> the torn cardstock collar and drop shadow so lower timeline tracks stay
> visible.

### Key green-screen or blue-screen footage

1.  Apply **Paper Cutout FX** to a clip filmed against a solid backdrop.
2.  In **Inspector** > **Effects**, expand **2. Green Screen & Matte Cleanup**.
3.  Select **Enable Green Screen Key**.
4.  Pick the backdrop hue with **Screen Color**, adjust separation with **Key
    Gain** and **Key Balance**, and refine the silhouette edge with **Edge Choke
    (px)** before the torn paper border is expanded.

### Show the built-in paper sticker

To preview the look without your own artwork, render the procedural demo
sticker inside the filter:

1.  Apply **Paper Cutout FX** to any clip on the timeline.
2.  In **Inspector** > **Effects** > **1. Media Source & Placement**, set
    **Source Mode** to `Built-in Paper Sticker`.
3.  Customize paper texture, animation style, and cutout position in the
    remaining groups.

### Fold the cutout with Paper Unfold (3D)

The **Paper Unfold (3D)** intro and outro style folds the finished cutout along
real creases: the cutout arrives as a small folded packet, its halves and
corners flip open with the paper back showing, and it settles flat. Round or
irregular cutouts fold across their own outline, so corner flaps stay visible.
**Glide Angle** also rotates the fold direction (its default, `-22°`, keeps the
folds upright), and **Mirror Animation H** and **Mirror Animation V** mirror the
fold layout.

## Fold and unfold clips with Paper Fold & Unfold

**Paper Fold & Unfold** is an Edit-page transition that makes a clip unfold from
a folded paper packet, or fold up and leave the frame. It works on full-frame
video and on clips with transparency, such as PNG logos and titles on upper
tracks.

### Apply the transition

1.  On the **Edit** page, open **Effects** > **Open FX** > **Transitions** >
    **MTTN PaperCraft Studio**.
2.  Drag **Paper Fold & Unfold** onto an edit point: the start of a clip, the
    end of a clip, or the cut between two clips.
3.  Drag the transition edges on the timeline to set its length. Longer
    transitions unfold more slowly.
4.  Select the transition and open **Inspector** > **Transition** to adjust the
    fold pattern, paper look, stop-motion steps, and shadow.

The first and last frames of the transition always match the outgoing and
incoming clips exactly, so cuts around it don't jump.

### Choose what folds and what unfolds

With **Fold Action** set to **Auto (Unfold In / Fold Out)**, the transition
reads both clips and decides:

| Where you place it                                | What Auto does                                       |
| :------------------------------------------------ | :--------------------------------------------------- |
| At the start of a clip, with nothing before it    | The clip unfolds into view                           |
| At the end of a clip, with nothing after it       | The clip folds up and slides out of the frame        |
| Between two full-frame clips                      | The incoming clip unfolds over the outgoing clip     |
| Between clips with transparency (PNGs, titles)    | The outgoing clip folds away, then the incoming clip unfolds |

To force a behavior, set **Fold Action** to **Unfold Incoming Clip**, **Fold
Outgoing Clip**, or **Fold Out, Then Unfold In**.

### Unfold a logo over video

1.  Place a transparent PNG or title on Video Track 2 (`V2`) above your video.
2.  Drag **Paper Fold & Unfold** onto the start of the PNG clip. The logo
    unfolds over the video on `V1`, and the paper back, collar, and shadow keep
    their transparency.
3.  To unfold the logo in place instead of sliding it in, set **Packet Slide
    Distance** to `0`.
4.  Drag a second **Paper Fold & Unfold** onto the end of the PNG clip to fold
    the logo away.

> [!TIP] Pair the transition with `01_Paper_Unfold.wav` from the Paper SFX
> library, and use **Stop-Motion Steps** to match the number of poses to your
> project frame rate (about one pose every two or three frames looks like
> classic stop-motion).

## Complete Inspector controls reference

The tables that follow list all controls exposed in the DaVinci Resolve
Inspector for **Paper Cutout FX** and **Paper Fold & Unfold**.

### Paper Cutout FX controls

<!-- disableFinding("Master") -->

| Group        | Control label   | Type and    | Default      | Purpose        |
:              :                 : range       :              :                :
| :----------- | :-------------- | :---------- | :----------- | :------------- |
| **1. Media   | **Source Mode** | Choice      | `Timeline    | Switches       |
: Source &     :                 : (`Timeline  : Clip`        : between        :
: Placement**  :                 : Clip`,      :              : timeline clip  :
:              :                 : `Built-in   :              : media and      :
:              :                 : Paper       :              : procedural     :
:              :                 : Sticker`)   :              : sticker        :
| **1. Media   | **Premultiplied | Boolean     | `True`       | Disable if     |
: Source &     : Source Alpha**  :             :              : straight-alpha :
: Placement**  :                 :             :              : graphics show  :
:              :                 :             :              : dark or bright :
:              :                 :             :              : edge fringes   :
| **1. Media   | **Media Scale** | Scale       | `0.90`       | Scales artwork |
: Source &     :                 : (`0.05` to  :              : inside the     :
: Placement**  :                 : `5.0`)      :              : paper frame    :
| **1. Media   | **Media         | XY Point    | `0.5, 0.5`   | Repositions    |
: Source &     : Position**      : (`0.0` to   :              : the source     :
: Placement**  :                 : `1.0`)      :              : subject inside :
:              :                 :             :              : the frame      :
| **1. Media   | **Media         | Angle       | `0.0°`       | Rotates the    |
: Source &     : Rotation**      : (`-3600°`   :              : subject prior  :
: Placement**  :                 : to `3600°`) :              : to collar      :
:              :                 :             :              : extraction     :
| **2. Green   | **Enable Green  | Boolean     | `False`      | Turns on       |
: Screen &     : Screen Key**    :             :              : solid-backdrop :
: Matte        :                 :             :              : chroma keying  :
: Cleanup**    :                 :             :              : and spill      :
:              :                 :             :              : suppression    :
| **2. Green   | **Screen        | RGB color   | `0.0, 1.0,   | Selects the    |
: Screen &     : Color**         : picker      : 0.0` (green) : key color to   :
: Matte        :                 :             :              : remove         :
: Cleanup**    :                 :             :              :                :
| **2. Green   | **Key Gain**    | Float       | `1.0`        | Controls matte |
: Screen &     :                 : (`0.0` to   :              : contrast       :
: Matte        :                 : `3.0`)      :              : against the    :
: Cleanup**    :                 :             :              : keyed backdrop :
| **2. Green   | **Key Balance** | Float       | `0.5`        | Shifts the     |
: Screen &     :                 : (`0.0` to   :              : soft key       :
: Matte        :                 : `1.0`)      :              : threshold      :
: Cleanup**    :                 :             :              : midpoint       :
| **2. Green   | **Edge Choke    | Float       | `0.0`        | Shrinks        |
: Screen &     : (px)**          : (`-32.0` to :              : (negative) or  :
: Matte        :                 : `32.0`)     :              : expands        :
: Cleanup**    :                 :             :              : (positive) the :
:              :                 :             :              : extracted      :
:              :                 :             :              : matte          :
| **3. Torn    | **Enable Paper  | Boolean     | `True`       | Wraps a        |
: Paper Cutout : Border**        :             :              : procedural     :
: Border**     :                 :             :              : ragged         :
:              :                 :             :              : cardstock      :
:              :                 :             :              : collar around  :
:              :                 :             :              : the matte      :
| **3. Torn    | **Border        | Float       | `0.018`      | Sets collar    |
: Paper Cutout : Width**         : (`0.0` to   :              : thickness      :
: Border**     :                 : `0.15`)     :              : relative to    :
:              :                 :             :              : frame height   :
| **3. Torn    | **Torn Edge     | Float       | `0.012`      | Controls       |
: Paper Cutout : Roughness**     : (`0.0` to   :              : amplitude of   :
: Border**     :                 : `0.20`)     :              : torn edge      :
:              :                 :             :              : teeth          :
| **3. Torn    | **Tear          | Float       | `24.0`       | Sets spatial   |
: Paper Cutout : Frequency**     : (`1.0` to   :              : density of     :
: Border**     :                 : `120.0`)    :              : paper teeth    :
:              :                 :             :              : along the edge :
| **3. Torn    | **Tear Fiber    | Integer     | `5`          | Sets octave    |
: Paper Cutout : Detail**        : (`1` to     :              : count for torn :
: Border**     :                 : `8`)        :              : paper          :
:              :                 :             :              : micro-fibers   :
| **3. Torn    | **Edge Boil     | Float       | `0.15`       | Animates torn  |
: Paper Cutout : Speed**         : (`0.0` to   :              : edge variation :
: Border**     :                 : `5.0`)      :              : across         :
:              :                 :             :              : stop-motion    :
:              :                 :             :              : steps          :
| **3. Torn    | **Paper         | RGB color   | `0.965,      | Sets the warm  |
: Paper Cutout : Cardstock       : picker      : 0.945,       : paper border   :
: Border**     : Color**         :             : 0.895`       : fill color     :
| **4. Paper   | **Paper Grain   | Float       | `0.35`       | Adjusts        |
: Grain & Fold : Strength**      : (`0.0` to   :              : cardstock      :
: Creases**    :                 : `1.0`)      :              : fiber grain    :
:              :                 :             :              : depth          :
| **4. Paper   | **Grain Scale** | Scale       | `1.0`        | Changes size   |
: Grain & Fold :                 : (`0.1` to   :              : of surface     :
: Creases**    :                 : `10.0`)     :              : fiber grain    :
| **4. Paper   | **Fold Crease   | Float       | `0.40`       | Controls       |
: Grain & Fold : Strength**      : (`0.0` to   :              : contrast of    :
: Creases**    :                 : `1.0`)      :              : ridged fold    :
:              :                 :             :              : creases        :
| **4. Paper   | **Crease        | Float       | `3.2`        | Controls       |
: Grain & Fold : Scale**         : (`0.2` to   :              : spacing of     :
: Creases**    :                 : `30.0`)     :              : folded paper   :
:              :                 :             :              : creases        :
| **4. Paper   | **Media Texture | Float       | `0.22`       | Blends subtle  |
: Grain & Fold : Overlay**       : (`0.0` to   :              : crease shading :
: Creases**    :                 : `1.0`)      :              : over subject   :
:              :                 :             :              : ink            :
| **4. Paper   | **Texture       | Integer     | `7`          | Varies         |
: Grain & Fold : Seed**          : (`0` to     :              : deterministic  :
: Creases**    :                 : `1000`)     :              : tear, crease,  :
:              :                 :             :              : and jitter     :
:              :                 :             :              : patterns       :
| **5. Intro & | **Intro Style** | Choice      | `Crumple     | Selects        |
: Outro Paper  :                 : (`None`,    : Ball`        : opening clip   :
: Animations** :                 : `Paper      :              : entrance       :
:              :                 : Unfold      :              : animation      :
:              :                 : (3D)`,      :              :                :
:              :                 : `Crumple    :              :                :
:              :                 : Ball`,      :              :                :
:              :                 : `Paper      :              :                :
:              :                 : Airplane    :              :                :
:              :                 : Glide`,     :              :                :
:              :                 : `Flat       :              :                :
:              :                 : Cutout      :              :                :
:              :                 : Pop`)       :              :                :
| **5. Intro & | **Intro         | Integer     | `14`         | Sets opening   |
: Outro Paper  : Duration        : (`0` to     :              : animation      :
: Animations** : (Frames)**      : `240`)      :              : duration in    :
:              :                 :             :              : frames         :
| **5. Intro & | **Outro Style** | Choice      | `Paper       | Selects        |
: Outro Paper  :                 : (same five  : Unfold (3D)` : closing clip   :
: Animations** :                 : styles as   :              : exit animation :
:              :                 : **Intro     :              :                :
:              :                 : Style**)    :              :                :
| **5. Intro & | **Outro         | Integer     | `14`         | Sets closing   |
: Outro Paper  : Duration        : (`0` to     :              : animation      :
: Animations** : (Frames)**      : `240`)      :              : duration in    :
:              :                 :             :              : frames         :
| **5. Intro & | **Glide Angle** | Angle       | `-22.0°`     | Sets           |
: Outro Paper  :                 : (`-360°` to :              : directional    :
: Animations** :                 : `360°`)     :              : trajectory for :
:              :                 :             :              : glide and      :
:              :                 :             :              : slide moves    :
| **5. Intro & | **Glide         | Float       | `0.65`       | Sets travel    |
: Outro Paper  : Distance**      : (`0.0` to   :              : distance in    :
: Animations** :                 : `3.0`)      :              : normalized     :
:              :                 :             :              : frame units    :
| **5. Intro & | **Mirror        | Boolean     | `False`      | Flips intro    |
: Outro Paper  : Animation H**   :             :              : and outro      :
: Animations** :                 :             :              : movement       :
:              :                 :             :              : horizontally   :
| **5. Intro & | **Mirror        | Boolean     | `False`      | Flips intro    |
: Outro Paper  : Animation V**   :             :              : and outro      :
: Animations** :                 :             :              : movement       :
:              :                 :             :              : vertically     :
| **6. 3D Drop | **Shadow        | Float       | `0.62`       | Controls cast  |
: Shadow**     : Opacity**       : (`0.0` to   :              : shadow         :
:              :                 : `1.0`)      :              : intensity      :
:              :                 :             :              : behind the     :
:              :                 :             :              : sheet          :
| **6. 3D Drop | **Shadow        | Angle       | `-45.0°`     | Controls       |
: Shadow**     : Angle**         : (`-360°` to :              : directional    :
:              :                 : `360°`)     :              : shadow throw   :
:              :                 :             :              : angle          :
| **6. 3D Drop | **Shadow        | Float       | `0.016`      | Sets shadow    |
: Shadow**     : Distance**      : (`0.0` to   :              : offset         :
:              :                 : `0.20`)     :              : distance       :
:              :                 :             :              : behind the     :
:              :                 :             :              : paper layer    :
| **6. 3D Drop | **Shadow        | Float       | `0.018`      | Controls       |
: Shadow**     : Softness**      : (`0.0` to   :              : shadow feather :
:              :                 : `0.20`)     :              : falloff width  :
| **7.         | **Enable        | Boolean     | `True`       | Steps          |
: Stop-Motion  : Stop-Motion**   :             :              : animation      :
: & Master     :                 :             :              : timing on      :
: Transform**  :                 :             :              : discrete hold  :
:              :                 :             :              : intervals      :
| **7.         | **Hold Frames   | Integer     | `3`          | Sets frames    |
: Stop-Motion  : (Step)**        : (`1` to     :              : per            :
: & Master     :                 : `30`)       :              : stop-motion    :
: Transform**  :                 :             :              : step (for      :
:              :                 :             :              : example, `3`   :
:              :                 :             :              : for an 8 fps   :
:              :                 :             :              : cadence on a   :
:              :                 :             :              : 24 fps         :
:              :                 :             :              : timeline)      :
| **7.         | **Position      | Float       | `0.0025`     | Adds           |
: Stop-Motion  : Jitter**        : (`0.0` to   :              : hand-placed    :
: & Master     :                 : `0.1`)      :              : positional     :
: Transform**  :                 :             :              : wobble per     :
:              :                 :             :              : step           :
| **7.         | **Rotation      | Angle       | `0.75°`      | Adds           |
: Stop-Motion  : Jitter**        : (`0.0°` to  :              : hand-placed    :
: & Master     :                 : `30.0°`)    :              : rotational     :
: Transform**  :                 :             :              : wobble per     :
:              :                 :             :              : step           :
| **7.         | **Master        | XY Point    | `0.5, 0.5`   | Repositions    |
: Stop-Motion  : Position**      : (`0.0` to   :              : the finished   :
: & Master     :                 : `1.0`)      :              : paper cutout   :
: Transform**  :                 :             :              : on screen      :
| **7.         | **Master        | Scale       | `1.0`        | Scales the     |
: Stop-Motion  : Scale**         : (`0.01` to  :              : finished       :
: & Master     :                 : `10.0`)     :              : cutout         :
: Transform**  :                 :             :              : uniformly      :
| **7.         | **Master        | Angle       | `0.0°`       | Rotates the    |
: Stop-Motion  : Rotation**      : (`-3600°`   :              : finished       :
: & Master     :                 : to `3600°`) :              : cutout cleanly :
: Transform**  :                 :             :              :                :
| **7.         | **Flip          | Boolean     | `False`      | Mirrors the    |
: Stop-Motion  : Horizontal**    :             :              : finished       :
: & Master     :                 :             :              : cutout left to :
: Transform**  :                 :             :              : right          :
| **7.         | **Flip          | Boolean     | `False`      | Mirrors the    |
: Stop-Motion  : Vertical**      :             :              : finished       :
: & Master     :                 :             :              : cutout top to  :
: Transform**  :                 :             :              : bottom         :

### Paper Fold & Unfold controls

DaVinci Resolve drives the transition's **Progress** parameter from the
timeline, so you don't need to keyframe it.

| Group | Control | Type and range | Default | Purpose |
| :---- | :------ | :------------- | :------ | :------ |
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

The collar, crease lines, jitter, and drop shadow fade out over the last part of
an unfold (and fade in at the start of a fold), so the clean clip is never
covered when the transition ends.

## Mix Foley audio with the Paper SFX library

All four WAV files in `Paper_SFX/` are real microphone recordings standardized to
48 kHz, 24-bit stereo clips with smooth zero-crossing fades and public-domain
licensing (`CC0 1.0` and `Public Domain`; see
[audio credits](../Paper_SFX/CREDITS.md)). Drag the `Paper_SFX/` folder into a
DaVinci Resolve **Power Bin** to keep every clip accessible across projects.

| WAV file in `Paper_SFX/`  | Tactile paper gesture                    | Matching visual preset                      |
| :------------------------ | :--------------------------------------- | :------------------------------------------ |
| `01_Paper_Unfold.wav`     | Crisp folded sheet spread and settle     | **Paper Fold & Unfold** and **Paper Unfold (3D)** |
| `02_Paper_Crumple.wav`    | Organic stereo paper-ball crumple        | **Crumple Ball** intro and outro            |
| `03_Paper_Glide_Flip.wav` | Fast page flip and air swoosh            | **Paper Airplane Glide** and quick cuts     |
| `04_Paper_Rip_Tear.wav`   | Clean dry fiber tear                     | Torn cardstock border and **Flat Cutout Pop** |

## Improve timeline playback performance

For smooth 4K timeline playback on CPU OpenFX tracks:

-   Turn on **Playback** > **Render Cache** > **Smart** or **User** after
    locking in cutout or transition parameters.
-   Lower **Playback** > **Timeline Proxy Resolution** to **Half** or
    **Quarter** while timing animation beats.
-   **Paper Fold & Unfold** renders a 1080p frame in about 11 ms on average on
    an Apple M4 Pro Mac with 14 cores, or about 27 ms on four threads. Slower
    machines, such as a 2017 iMac, may need the render cache to play it back in
    real time.

## Troubleshoot common issues

| Symptom        | Cause             | Resolution                                  |
| :------------- | :---------------- | :------------------------------------------ |
| Plugin missing | Mac App Store     | Install DaVinci Resolve directly from the   |
: under          : Resolve edition   : Blackmagic Design website and restart       :
: **Effects** on : blocks            :                                             :
: macOS          : third-party       :                                             :
:                : bundles           :                                             :
| Dark or bright | Straight RGB      | Clear **Premultiplied Source Alpha** in     |
: halo around    : channels filtered : **1. Media Source & Placement**             :
: straight-alpha : before            :                                             :
: PNG stickers   : premultiplication :                                             :
| Effect shown   | Project uses the  | Delete the unavailable generator or         |
: as unavailable : generator or a    : transition from the timeline, then use      :
: after          : transition        : **Paper Cutout FX** intro and outro styles  :
: upgrading to   : removed in 3.0    : or the **Paper Fold & Unfold** transition   :
: 3.0            :                   : instead                                     :
| Paper Fold &   | Auto read the     | Set **Fold Action** to the behavior you     |
: Unfold folds   : clips differently : want instead of **Auto**                    :
: when you       : than expected     :                                             :
: expected an    :                   :                                             :
: unfold         :                   :                                             :
| Paper Fold &   | OpenFX            | Look under **Effects** > **Open FX** >      |
: Unfold missing : transitions are   : **Transitions** > **MTTN PaperCraft         :
: from the       : listed apart from : Studio**, not under **Video Transitions**   :
: transitions    : the built-in ones :                                             :
| Green spill or | Unkeyed           | Increase **Key Gain** and shift **Edge      |
: rough halo on  : background pixels : Choke (px)** slightly negative (`-1.5` to   :
: keyed subjects : entering the      : `-3.0`)                                     :
:                : signed distance   :                                             :
:                : field collar      :                                             :
| `clang++ not   | Xcode Command     | Run `xcode-select --install`, finish        |
: found` during  : Line Tools        : installation, and run                       :
: macOS install  : missing           : `./Install_PaperCraft_OpenFX_macOS.command` :
:                :                   : again                                       :

## What's next

-   Read the [design document](design_doc.md) to explore the signed distance
    field, perspective warping, and curated Foley audio pipeline.
-   Read the [developer and build guide](developer_guide.md) to compile from
    source or extend the codebase.

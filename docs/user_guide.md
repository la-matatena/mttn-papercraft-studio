# PaperCraft Studio Pro user guide

This guide helps video editors install PaperCraft Studio Pro, apply torn paper
cutouts and 3D paper transitions on the DaVinci Resolve 20 and 21 Edit pages,
tune Inspector controls, and mix the 24 included Paper SFX audio clips.

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
the DaVinci Resolve prompt. Legacy templates appear under **Effects** >
**Toolbox** > **PaperCraft Studio**.

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

The OpenFX bundle registers **Paper Cutout FX** in both Filter and Generator
modes so you can animate timeline media or drop in standalone stickers on the
Edit page.

### Apply Paper Cutout FX to a timeline clip

1.  On the **Edit** page, select a video clip, transparent PNG graphic, or title
    on Video Track 1 or higher (`V2+`).
2.  Open **Effects** > **Open FX** > **PaperCraft Studio**.
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

### Create a standalone sticker cutout

1.  Open **Effects** > **Generators** > **PaperCraft Studio**.
2.  Drag **Paper Cutout FX** directly onto an empty track such as `V2`.
3.  Customize paper texture, animation style, and global cutout position in
    **Inspector** > **Generator**.

## Add paper transitions between clips

Three dedicated CPU OpenFX transitions run directly on Edit-page cuts without
entering Fusion:

1.  Place two clips next to each other on the same timeline track with enough
    trimmed media handles on both sides of the cut.
2.  Open **Effects** > **Video Transitions** > **PaperCraft Studio**.
3.  Drag **Paper Unfold Transition**, **Paper Crumple Transition**, or **Paper
    Rip Wipe Transition** onto the cut point.
4.  Select the transition bar and adjust style, step count, mirror flags, and
    shadow softness in **Inspector** > **Transition**.

## Complete Inspector controls reference

The tables that follow list all controls exposed in the DaVinci Resolve
Inspector for **Paper Cutout FX** and all three paper transitions.

### Paper Cutout FX controls

<!-- disableFinding("Master") -->

| Group        | Control label   | Type and    | Default      | Purpose        |
:              :                 : range       :              :                :
| :----------- | :-------------- | :---------- | :----------- | :------------- |
| **1. Media   | **Source Mode** | Choice      | `Timeline    | Switches       |
: Source &     :                 : (`Timeline  : Clip`        : between        :
: Placement**  :                 : Clip`,      : (Filter) or  : timeline clip  :
:              :                 : `Built-in   : `Built-in    : media and      :
:              :                 : Paper       : Paper        : procedural     :
:              :                 : Sticker`)   : Sticker`     : sticker        :
:              :                 :             : (Generator)  :                :
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

### Paper transition controls

Every paper transition shares the host-animated **Progress** slider alongside
groups **2. Stop-Motion & Orientation** and **3. Shadow & Paper Creases**. Group
**1. Paper Transition Style** changes according to the transition you place on
the timeline cut:

| Transition    | Control label | Type and     | Default       | Purpose       |
:               :               : range        :               :               :
| :------------ | :------------ | :----------- | :------------ | :------------ |
| **All three   | **Enable      | Boolean      | `True`        | Quantizes     |
: transitions** : Stop-Motion** :              :               : transition    :
:               :               :              :               : progress into :
:               :               :              :               : discrete      :
:               :               :              :               : frames        :
| **All three   | **Stop-Motion | Integer (`2` | `12`          | Sets step     |
: transitions** : Steps**       : to `60`)     :               : count across  :
:               :               :              :               : transition    :
:               :               :              :               : duration      :
| **All three   | **Reverse     | Boolean      | `False`       | Inverts       |
: transitions** : Direction**   :              :               : direction     :
:               :               :              :               : across        :
:               :               :              :               : incoming and  :
:               :               :              :               : outgoing      :
:               :               :              :               : clips         :
| **All three   | **Mirror      | Boolean      | `False`       | Flips         |
: transitions** : Horizontal**  :              :               : transition    :
:               : and **Mirror  :              :               : motion axes   :
:               : Vertical**    :              :               : horizontally  :
:               :               :              :               : or vertically :
| **All three   | **Shadow      | Float (`0.0` | `0.60`        | Styles the    |
: transitions** : Opacity** and : to `1.0`     : opacity and   : cast drop     :
:               : **Shadow      : opacity and  : `0.020`       : shadow under  :
:               : Softness**    : `0.0` to     : softness      : moving paper  :
:               :               : `0.20`       :               : sheets        :
:               :               : softness)    :               :               :
| **All three   | **Crease      | Float (`0.0` | `0.45` blend  | Adds crease   |
: transitions** : Texture       : to `1.0`)    : and `7` seed  : shading and   :
:               : Blend** and   : and Integer  :               : deterministic :
:               : **Texture     : (`0` to      :               : variation     :
:               : Seed**        : `1000`)      :               :               :
| **Paper       | **Fold Tilt   | Angle        | `22.0°`       | Sets 3D hinge |
: Unfold        : Angle**       : (`-90°` to   :               : skew during   :
: Transition**  :               : `90°`)       :               : sheet unfold  :
| **Paper       | **Slide       | Float (`0.0` | `0.55`        | Sets          |
: Unfold        : Distance**    : to `3.0`)    :               : horizontal    :
: Transition**  :               :              :               : slide travel  :
:               :               :              :               : during unfold :
| **Paper       | **Fold Crease | Float (`0.2` | `4.0`         | Scales crease |
: Unfold        : Scale**       : to `30.0`)   :               : highlights    :
: Transition**  :               :              :               : across the    :
:               :               :              :               : unfolding     :
:               :               :              :               : sheet         :
| **Paper       | **Crumple     | Float (`0.0` | `0.22`        | Controls      |
: Crumple       : Intensity**   : to `1.0`)    :               : faceted ball  :
: Transition**  :               :              :               : distortion    :
:               :               :              :               : intensity     :
| **Paper       | **Crumple     | Float (`0.5` | `4.0`         | Sets spatial  |
: Crumple       : Facet Scale** : to `30.0`)   :               : density of    :
: Transition**  :               :              :               : crumpled      :
:               :               :              :               : facets        :
| **Paper       | **Paper Ball  | Angle        | `35.0°`       | Sets twist    |
: Crumple       : Spin**        : (`-360°` to  :               : rotation at   :
: Transition**  :               : `360°`)      :               : transition    :
:               :               :              :               : midpoint      :
| **Paper Rip   | **Rip Angle** | Angle        | `12.0°`       | Angles the    |
: Wipe          :               : (`-180°` to  :               : diagonal torn :
: Transition**  :               : `180°`)      :               : paper wipe    :
:               :               :              :               : edge          :
| **Paper Rip   | **Torn White  | Float (`0.0` | `0.028`       | Sets          |
: Wipe          : Edge Width**  : to `0.20`)   :               : thickness of  :
: Transition**  :               :              :               : the exposed   :
:               :               :              :               : torn          :
:               :               :              :               : cardstock rim :
| **Paper Rip   | **Rip         | Float (`0.0` | `0.032`       | Controls      |
: Wipe          : Jaggedness**  : to `0.25`)   :               : tooth height  :
: Transition**  :               :              :               : along the rip :
:               :               :              :               : line          :
| **Paper Rip   | **Tear        | Float (`1.0` | `24.0`        | Controls      |
: Wipe          : Frequency**   : to `120.0`)  :               : tooth density :
: Transition**  :               :              :               : along the rip :
:               :               :              :               : line          :
| **Paper Rip   | **Torn Edge   | RGB color    | `0.960,       | Sets color of |
: Wipe          : Paper Color** : picker       : 0.940, 0.890` : the exposed   :
: Transition**  :               :              :               : torn edge rim :

## Mix Foley audio with the Paper SFX library

All 24 WAV files in `Paper_SFX/` are 48 kHz, 24-bit stereo clips trimmed with
zero-crossing fades for click-free timeline layering. Drag the `Paper_SFX/`
folder into a DaVinci Resolve **Power Bin** to keep every clip accessible across
projects.

| Subfolder                  | Included WAV files                       | Matching     |
:                            :                                          : visual       :
:                            :                                          : preset       :
| :------------------------- | :--------------------------------------- | :----------- |
| `01_Page_Turns/`           | `Page_Turn_Crisp_Book_01.wav`,           | **Paper      |
:                            : `Page_Turn_Heavy_Cardstock_02.wav`,      : Unfold       :
:                            : `Page_Turn_Newspaper_Broadsheet_03.wav`, : Transition** :
:                            : `Page_Turn_Quick_Notebook_Flip_04.wav`   : or **Paper   :
:                            :                                          : Unfold       :
:                            :                                          : (3D)**       :
| `02_Paper_Hits_Impacts/`   | `Paper_Hit_Table_Slap_01.wav`,           | **Flat       |
:                            : `Paper_Hit_Cardboard_Drop_02.wav`,       : Cutout Pop** :
:                            : `Paper_Hit_Sticker_Snap_03.wav`,         : landing      :
:                            : `Paper_Hit_Stack_Thud_04.wav`            : frames       :
| `03_Crumples_Uncrumples/`  | `Paper_Crumple_Tight_Ball_01.wav`,       | **Crumple    |
:                            : `Paper_Crumple_Newspaper_02.wav`,        : Ball**       :
:                            : `Paper_Uncrumple_Smooth_Reveal_03.wav`,  : intro,       :
:                            : `Paper_Uncrumple_Quick_Open_04.wav`      : outro, and   :
:                            :                                          : **Paper      :
:                            :                                          : Crumple      :
:                            :                                          : Transition** :
| `04_Rips_Tears/`           | `Paper_Rip_Fast_Tear_01.wav`,            | **Paper Rip  |
:                            : `Paper_Rip_Slow_Fiber_02.wav`,           : Wipe         :
:                            : `Paper_Rip_Thick_Kraft_03.wav`,          : Transition** :
:                            : `Paper_Rip_Short_Snip_04.wav`            :              :
| `05_Little_Bits_Rustles/`  | `Paper_Bits_Confetti_Scatter_01.wav`,    | Continuous   |
:                            : `Paper_Rustle_StopMotion_Jiggle_02.wav`, : **Paper      :
:                            : `Paper_Slide_Desk_Scrape_03.wav`,        : Cutout FX**  :
:                            : `Paper_Crease_Finger_Fold_04.wav`        : jitter and   :
:                            :                                          : **Paper      :
:                            :                                          : Airplane     :
:                            :                                          : Glide**      :
| `06_Designed_Transitions/` | `Paper_Designed_Whoosh_Unfold_01.wav`,   | Full wipe    |
:                            : `Paper_Designed_Airplane_Swoosh_02.wav`, : and slam     :
:                            : `Paper_Designed_Crumple_Slam_03.wav`,    : accents      :
:                            : `Paper_Designed_StopMotion_Pop_04.wav`   : synced at    :
:                            :                                          : transition   :
:                            :                                          : midpoints    :

## Improve timeline playback performance

For smooth 4K timeline playback on CPU OpenFX tracks:

-   Turn on **Playback** > **Render Cache** > **Smart** or **User** after
    locking in cutout parameters.
-   Lower **Playback** > **Timeline Proxy Resolution** to **Half** or
    **Quarter** while timing animation beats.

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
| Lower track    | Legacy template   | Use the native OpenFX **Paper Unfold        |
: hidden behind  : or mismatched     : Transition**, **Paper Crumple Transition**, :
: `V2` paper     : track composite   : or **Paper Rip Wipe Transition**            :
: transition     :                   :                                             :
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
    field, perspective warping, and Foley synthesis algorithms.
-   Read the [developer and build guide](developer_guide.md) to compile from
    source or extend the codebase.

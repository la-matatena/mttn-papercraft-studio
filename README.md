# PaperCraft Studio Pro

Add organic stop-motion paper cutouts, torn cardstock borders, 3D paper fold
transitions, and paper Foley sound effects directly on the DaVinci Resolve 20
and 21 Edit pages without opening Fusion. Both DaVinci Resolve Free and DaVinci
Resolve Studio run the native OpenFX 1.4 plugin without watermarks.

## Included deliverables

| Component             | Package path                              | Purpose                          |
| :-------------------- | :---------------------------------------- | :------------------------------- |
| Native OpenFX bundle  | `PaperCraftStudioPro.ofx.bundle`          | Direct C++17 Edit-page effects,  |
:                       :                                           : generators, and transitions for  :
:                       :                                           : Linux x86-64                     :
| macOS OpenFX          | `Install_PaperCraft_OpenFX_macOS.command` | Compiles Universal or native     |
: installer             :                                           : macOS OpenFX bundle and installs :
:                       :                                           : to OpenFX folder                 :
| Linux OpenFX          | `install_linux_ofx.sh`                    | Installs                         |
: installer             :                                           : `PaperCraftStudioPro.ofx.bundle` :
:                       :                                           : into the Linux OpenFX directory  :
| Portable OpenFX       | `OpenFX_Source/`                          | Standalone C++17 source files    |
: source                :                                           : and OpenFX 1.4 headers for macOS :
:                       :                                           : builds                           :
| Paper SFX library     | `Paper_SFX/` directory (24 stereo WAV     | 48 kHz, 24-bit stereo Foley      |
:                       : files)                                    : sounds organized across six      :
:                       :                                           : categories                       :
| Docs and              | `docs/` (`user_guide.md`,                 | End-user guide, developer build  |
| : design              | `design_doc.md`, `developer_guide.md`)    | guide, and technical design      |
| Complete repo ZIP \|  | `PaperCraft_Studio_Pro_Complete_Repo.zip` | Full source repository, tests,   |
:                       : documentation, and deliverables           : |                                :
| Optional              | `PaperCraft_Studio_Pro.drfx` and          | Repaired Fusion `MacroOperator`  |
| : legacy :            | templates for legacy project              |                                  |
: `Templates_Unpacked/` :                                           :                                  :

\: templates : : workflows :

## Documentation

Read the full documentation set for installation steps, Edit-page workflows, and
engineering architecture:

-   [User guide](docs/user_guide.md): Installation steps, Edit-page cutout and
    transition workflows, all Inspector parameters, Paper SFX mixing table, and
    troubleshooting.
-   [Design document](docs/design_doc.md): Architecture overview, OpenFX 1.4
    host integration, linear-time signed distance field pipeline, Foley audio
    synthesis, and engineering trade-offs.
-   [Developer and build guide](docs/developer_guide.md): Repository layout,
    Blaze build and test targets, standalone Linux and macOS compilation, and
    extension steps.

## Install the native Edit-page OpenFX plugin

### Install on macOS

Run DaVinci Resolve installed from the Blackmagic Design website rather than the
Mac App Store edition, because Apple sandbox restrictions prevent the Mac App
Store build from loading third-party OpenFX bundles in `/Library/OFX/Plugins`.

1.  Open **Terminal** or extract `PaperCraft_Studio_Pro_Full_Bundle.zip`.
2.  Double-click `Install_PaperCraft_OpenFX_macOS.command`, or run the installer
    script from **Terminal**:

    ```bash
    ./Install_PaperCraft_OpenFX_macOS.command
    ```

    If you prefer user-level installation without administrator privileges, pass
    `--user` to install into `${HOME}/Library/OFX/Plugins`:

    ```bash
    ./Install_PaperCraft_OpenFX_macOS.command --user
    ```

3.  Approve the macOS Command Line Tools prompt if `clang++` is not installed
    yet, then rerun `Install_PaperCraft_OpenFX_macOS.command`.

4.  Restart DaVinci Resolve to scan the OpenFX bundle.

### Install on Linux

1.  Extract `PaperCraft_Studio_Pro_Full_Bundle.zip`.
2.  Run the Linux OpenFX installer script:

    ```bash
    ./install_linux_ofx.sh
    ```

    Pass `--user` to install into `${HOME}/.local/share/OFX/Plugins` without
    root privileges, or `--dest` with `CUSTOM_DIR` for a custom location:

    ```bash
    ./install_linux_ofx.sh --dest CUSTOM_DIR
    ```

    Replace the following:

    -   `CUSTOM_DIR`: the destination directory where you want to store
        `PaperCraftStudioPro.ofx.bundle`.

3.  Restart DaVinci Resolve to load the Edit-page OpenFX suite.

## Use PaperCraft Studio on the Edit page

The OpenFX bundle registers four plugins in the **PaperCraft Studio** category:

-   **Paper Cutout FX** (filter): Open **Effects** > **Open FX** > **PaperCraft
    Studio** and drag **Paper Cutout FX** directly onto any timeline clip,
    title, transparent PNG, or green-screen shot.
-   **Paper Cutout FX** (generator): Open **Effects** > **Generators** >
    **PaperCraft Studio** and drag **Paper Cutout FX** onto an empty video track
    to generate a standalone animated sticker cutout.
-   **Paper Unfold Transition**, **Paper Crumple Transition**, and **Paper Rip
    Wipe Transition**: Open **Effects** > **Video Transitions** > **PaperCraft
    Studio** and drag any transition onto a cut between two Edit-page clips.

## Inspector controls reference

| Section                | Key controls             | Behavior                 |
| :--------------------- | :----------------------- | :----------------------- |
| **1. Media Source and  | **Source Mode**,         | Switches between         |
: Placement**            : **Premultiplied Source   : timeline clip input and  :
:                        : Alpha**, **Media         : built-in sticker, and    :
:                        : Scale**, **Media         : frames artwork inside    :
:                        : Position**, **Media      : the cutout               :
:                        : Rotation**               :                          :
| **2. Green Screen and  | **Enable Green Screen    | Extracts subject matte   |
: Matte Cleanup**        : Key**, **Screen Color**, : from solid green or blue :
:                        : **Key Gain**, **Key      : backdrops with spill     :
:                        : Balance**, **Edge Choke  : suppression              :
:                        : (px)**                   :                          :
| **3. Torn Paper Cutout | **Enable Paper Border**, | Expands a procedural     |
: Border**               : **Border Width**, **Torn : ragged cardstock collar  :
:                        : Edge Roughness**, **Tear : around any extracted     :
:                        : Frequency**, **Tear      : alpha shape              :
:                        : Fiber Detail**, **Edge   :                          :
:                        : Boil Speed**, **Paper    :                          :
:                        : Cardstock Color**        :                          :
| **4. Paper Grain and   | **Paper Grain            | Adds multi-octave fiber  |
: Fold Creases**         : Strength**, **Grain      : grain and ridged fold    :
:                        : Scale**, **Fold Crease   : creases                  :
:                        : Strength**, **Crease     :                          :
:                        : Scale**, **Media Texture :                          :
:                        : Overlay**, **Texture     :                          :
:                        : Seed**                   :                          :
| **5. Intro and Outro   | **Intro Style**, **Intro | Animates **Paper Unfold  |
: Paper Animations**     : Duration (Frames)**,     : (3D)**, **Crumple        :
:                        : **Outro Style**, **Outro : Ball**, **Paper Airplane :
:                        : Duration (Frames)**,     : Glide**, or **Flat       :
:                        : **Glide Angle**, **Glide : Cutout Pop** entrances   :
:                        : Distance**, **Mirror     : and exits                :
:                        : Animation H**, **Mirror  :                          :
:                        : Animation V**            :                          :
| **6. 3D Drop Shadow**  | **Shadow Opacity**,      | Casts soft directional   |
:                        : **Shadow Angle**,        : depth shadows behind the :
:                        : **Shadow Distance**,     : rotating cutout          :
:                        : **Shadow Softness**      :                          :
| **7. Stop-Motion and   | **Enable Stop-Motion**,  | Quantizes frame motion   |
: Master Transform**     : **Hold Frames (Step)**,  : into stepped holds and   :
:                        : **Position Jitter**,     : adds subtle              :
:                        : **Rotation Jitter**,     : frame-by-frame wobble    :
:                        : **Master Position**,     :                          :
:                        : **Master Scale**,        :                          :
:                        : **Master Rotation**,     :                          :
:                        : **Flip Horizontal**,     :                          :
:                        : **Flip Vertical**        :                          :

## Paper SFX starter library

Import the `Paper_SFX/` directory into a DaVinci Resolve Power Bin to reuse all
24 stereo 48 kHz 24-bit WAV files across projects:

-   The `01_Page_Turns/` subfolder: Four crisp book, cardstock, broadsheet, and
    quick flip turns.
-   The `02_Paper_Hits_Impacts/` subfolder: Four table slaps, cardboard drops,
    sticker snaps, and stack thuds.
-   The `03_Crumples_Uncrumples/` subfolder: Four tight crumple balls, newspaper
    crinkles, and smooth or quick uncrumples.
-   The `04_Rips_Tears/` subfolder: Four fast tears, slow fiber rips, thick
    kraft rips, and short snips.
-   The `05_Little_Bits_Rustles/` subfolder: Four confetti scatters, stop-motion
    jiggles, desk scrapes, and finger folds.
-   The `06_Designed_Transitions/` subfolder: Four layered paper whooshes,
    airplane swooshes, crumple slams, and stop-motion pops.

## Optional legacy `.drfx` templates

The repaired `PaperCraft_Studio_Pro.drfx` file remains included for existing
Fusion-template workflows. Double-click `PaperCraft_Studio_Pro.drfx` to install
Edit-page `MacroOperator` copies under **Effects** > **Toolbox** > **PaperCraft
Studio**. For all other Edit-page work, prefer the native OpenFX bundle because
it runs natively in C++ on CPU threads without Fusion macro overhead.

## Improve timeline playback and renders

-   Set **Playback** > **Render Cache** to **Smart** or **User** after
    finalizing your cutout parameters.
-   Lower **Playback** > **Timeline Proxy Resolution** to **Half** or
    **Quarter** when timing stop-motion transitions.

## Build from source

Run both C++ OpenFX verification tests and Python bundle tests from the
repository root:

```bash
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents test \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:papercraft_ofx_test \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:papercraft_pro_test
```

Build the complete release archive targets:

```bash
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents build \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:papercraft_release_bundle
```

Export the OpenFX bundle, installers, source kit, `.drfx` file, and SFX audio
files directly to a directory:

```bash
/google/bin/releases/arca9-local-blaze-cli/blaze-for-agents run \
  //experimental/users/joseluiszapata/davinci_resolve_plugins/papercraft_pro:build_papercraft_bundle -- \
  --output_dir=OUTPUT_DIR
```

Replace the following:

-   `OUTPUT_DIR`: the destination directory path on your local filesystem.

"""Procedural Fusion .setting template builders for DaVinci Resolve.

This module emits the two legacy Edit-page Fusion macros shipped inside
`MTTN_PaperCraft_Studio.drfx` (the OpenFX plugins are the primary product):

* `build_effect_setting()`: the **Paper Cutout FX** Edit effect
  (`Edit/Effects/...`).
* `build_fold_transition_setting()`: the **Paper Fold & Unfold** Edit
  transition (`Edit/Transitions/...`).

`bundle_builder_lib.build_drfx_entries()` calls both builders, and
`drfx_validator.validate_setting_syntax_and_graph()` checks their output.

`.setting` structure
--------------------
A `.setting` file is a Lua-table literal that Fusion pastes into a comp:

  {
    Tools = ordered() {
      <MacroName> = MacroOperator {
        Inputs = ordered() {      -- MainInput1/2 + UserControl defaults
          MainInput1 = InstanceInput { SourceOp = "...", Source = "Input" },
          <ControlId> = Input { Value = ..., },
        },
        Outputs = { MainOutput1 = InstanceOutput { SourceOp = "..." } },
        ViewInfo = OperatorInfo { Pos = { 0, 0 } },
        Tools = ordered() { ... internal nodes, 4 tabs deep ... },
        UserControls = ordered() { ... Inspector sliders/combos ... },
      },
    },
    ActiveTool = "<MacroName>",
  }

Resolve maps `MainInput1` to the clip (effect) or outgoing clip
(transition) and `MainInput2` to the incoming clip. Internal nodes sit at
exactly four leading tabs; the validator relies on that indentation to find
operator and control names, so keep the tab depth when editing templates.

SimpleExpressions
-----------------
Animated values use Fusion SimpleExpressions (`Expression = "..."`), not
Lua. They reference macro controls as `<MacroName>.<ControlId>`, the current
frame as `time`, and the clip range as `comp.RenderStart`/`comp.RenderEnd`.
Rules enforced by the validator: use `iif(cond, a, b)` for branching,
`and`/`or` (never `&&`/`||`), bare `sin`/`cos`/`floor`/`pow`/`pi` (never
`math.`), and balanced parentheses. Booleans from checkboxes and combo indices
are compared against `x > 0.5` or rounded with `floor(x + 0.5)` because
Fusion stores every control as a float.

Expressions must not contain double quotes because they are embedded in a
quoted Lua string without escaping.
"""

from collections.abc import Sequence
import dataclasses


@dataclasses.dataclass(frozen=True)
class ControlSpec:
  """Describes a single Inspector UserControl entry inside a Fusion macro.

  Attributes:
    control_id: Fusion input identifier; expressions read it as
      `<MacroName>.<control_id>`. Keep stable to preserve saved projects.
    label: Inspector label (`LINKS_Name`) shown to users.
    input_control: Fusion `INPID_InputControl` type, e.g. `SliderControl`,
      `CheckboxControl`, or `ComboControl`.
    default: Initial value (`INP_Default`).
    min_scale: Slider range start (`INP_MinScale`); a UI hint, not a clamp.
    max_scale: Slider range end (`INP_MaxScale`); a UI hint, not a clamp.
    min_allowed: Hard lower clamp (`INP_MinAllowed`), or None for no clamp.
    max_allowed: Hard upper clamp (`INP_MaxAllowed`), or None for no clamp.
    integer: Whether the value snaps to integers (`INP_Integer`).
    combo_options: Ordered labels for a `ComboControl`; index 0 is the first.
    center: Slider reset/center tick (`ICD_Center`), or None to omit.
  """

  control_id: str
  label: str
  input_control: str = "SliderControl"
  default: float = 0.0
  min_scale: float = 0.0
  max_scale: float = 1.0
  min_allowed: float | None = None
  max_allowed: float | None = None
  integer: bool = False
  combo_options: tuple[str, ...] = ()
  center: float | None = None


# --- Inspector UserControls serialization ---


def _format_num(value: float, integer: bool = False) -> str:
  """Formats numeric control values cleanly for Fusion .setting Lua tables.

  Args:
    value: Number to format.
    integer: Whether to round and print without a decimal point.

  Returns:
    `"3"` for integers, `"1.0"` for whole floats (keeps the value typed as a
    float in Lua), otherwise up to 6 significant digits (e.g. `"0.016"`).
  """
  if integer:
    return str(int(round(value)))
  if float(value).is_integer():
    return f"{int(value)}.0"
  return f"{value:.6g}"


def _render_control_entry(spec: ControlSpec) -> str:
  """Serializes one UserControls entry in Fusion .setting Lua syntax.

  Checkboxes skip the slider scale keys and add `CBC_TriState = false`;
  combos append one `{ CCS_AddString = "..." }` row per option, in order.

  Args:
    spec: Control description.

  Returns:
    Multi-line Lua table entry indented four tabs deep, ending in `},`.
  """
  lines = [
      f"\t\t\t\t{spec.control_id} = {{",
      f'\t\t\t\t\tLINKS_Name = "{spec.label}",',
      '\t\t\t\t\tLINKID_DataType = "Number",',
      f'\t\t\t\t\tINPID_InputControl = "{spec.input_control}",',
      '\t\t\t\t\tICS_ControlPage = "Controls",',
      (
          "\t\t\t\t\tINP_Default = "
          f"{_format_num(spec.default, integer=spec.integer)},"
      ),
  ]
  if spec.input_control != "CheckboxControl":
    lines.append(
        "\t\t\t\t\tINP_MinScale = "
        f"{_format_num(spec.min_scale, integer=spec.integer)},"
    )
    lines.append(
        "\t\t\t\t\tINP_MaxScale = "
        f"{_format_num(spec.max_scale, integer=spec.integer)},"
    )
  if spec.min_allowed is not None:
    lines.append(
        "\t\t\t\t\tINP_MinAllowed = "
        f"{_format_num(spec.min_allowed, integer=spec.integer)},"
    )
  if spec.max_allowed is not None:
    lines.append(
        "\t\t\t\t\tINP_MaxAllowed = "
        f"{_format_num(spec.max_allowed, integer=spec.integer)},"
    )
  if spec.center is not None:
    lines.append(f"\t\t\t\t\tICD_Center = {_format_num(spec.center)},")
  lines.append(
      f"\t\t\t\t\tINP_Integer = {'true' if spec.integer else 'false'},"
  )
  lines.append("\t\t\t\t\tINP_External = true,")
  if spec.input_control == "CheckboxControl":
    lines.append("\t\t\t\t\tCBC_TriState = false,")
  if spec.input_control == "ComboControl":
    for opt in spec.combo_options:
      lines.append(f'\t\t\t\t\t{{ CCS_AddString = "{opt}", }},')
  lines.append("\t\t\t\t},")
  return "\n".join(lines)


def _render_user_controls(
    groups: Sequence[tuple[str, str, Sequence[ControlSpec]]],
    default_open_groups: Sequence[str] = (),
) -> str:
  """Renders collapsible Inspector LabelControl sections and their controls.

  Each group becomes a `LabelControl` with `LBLC_DropDownButton = true`
  followed by its controls. `LBLC_NumInputs` must equal the number of
  controls that follow the label, or Fusion nests the wrong rows;
  `drfx_validator` checks this count.

  Args:
    groups: Ordered `(group_id, group_title, controls)` tuples.
    default_open_groups: Group IDs whose drop-down starts expanded.

  Returns:
    The full `UserControls = ordered() { ... },` block, three tabs deep.
  """
  open_set = set(default_open_groups)
  blocks: list[str] = ["\t\t\tUserControls = ordered() {"]
  for group_id, group_title, controls in groups:
    is_open = 1 if group_id in open_set else 0
    blocks.extend([
        f"\t\t\t\t{group_id} = {{",
        f'\t\t\t\t\tLINKS_Name = "{group_title}",',
        '\t\t\t\t\tLINKID_DataType = "Number",',
        '\t\t\t\t\tINPID_InputControl = "LabelControl",',
        '\t\t\t\t\tICS_ControlPage = "Controls",',
        f"\t\t\t\t\tINP_Default = {is_open},",
        "\t\t\t\t\tINP_MinAllowed = 0,",
        "\t\t\t\t\tINP_MaxAllowed = 1,",
        "\t\t\t\t\tINP_MinScale = 0,",
        "\t\t\t\t\tINP_MaxScale = 1,",
        "\t\t\t\t\tINP_Integer = false,",
        "\t\t\t\t\tINP_External = false,",
        "\t\t\t\t\tLBLC_NestLevel = 1,",
        f"\t\t\t\t\tLBLC_NumInputs = {len(controls)},",
        "\t\t\t\t\tLBLC_DropDownButton = true,",
        "\t\t\t\t},",
    ])
    for spec in controls:
      blocks.append(_render_control_entry(spec))
  blocks.append("\t\t\t},")
  return "\n".join(blocks)


def _render_group_defaults(
    groups: Sequence[tuple[str, str, Sequence[ControlSpec]]],
    default_open_groups: Sequence[str] = (),
) -> str:
  """Renders default Input values for macro UserControls.

  These rows go in the macro `Inputs` block so a fresh instance starts with
  the same values as `INP_Default`, and group labels start open (1) or
  closed (0).

  Args:
    groups: Ordered `(group_id, group_title, controls)` tuples.
    default_open_groups: Group IDs whose drop-down starts expanded.

  Returns:
    Newline-joined `<id> = Input { Value = ..., },` rows, four tabs deep.
  """
  open_set = set(default_open_groups)
  lines: list[str] = []
  for group_id, _, controls in groups:
    lines.append(
        f"\t\t\t\t{group_id} = Input {{ Value = "
        f"{1 if group_id in open_set else 0}, }},"
    )
    for spec in controls:
      val_str = _format_num(spec.default, integer=spec.integer)
      lines.append(
          f"\t\t\t\t{spec.control_id} = Input {{ Value = {val_str}, }},"
      )
  return "\n".join(lines)


# --- Paper Cutout FX control groups ---


def _core_cutout_control_groups() -> list[tuple[str, str, list[ControlSpec]]]:
  """Builds the Inspector control groups for the Paper Cutout FX effect.

  Positions and offsets are in Fusion normalized frame space (0..1, origin
  bottom-left, 0.5 = center); sizes such as `BorderSize` are fractions of
  frame width; angles are degrees; durations are frames. The Edit effect
  always uses the clip it is applied to, so there is no media-source picker.

  Returns:
    Ordered `(group_id, group_title, controls)` tuples. Group IDs and control
    IDs are referenced by expressions and must stay stable.
  """
  groups: list[tuple[str, str, list[ControlSpec]]] = []
  groups.append((
      "MediaSourceGroup",
      "1. Media Placement",
      [
          ControlSpec(
              "ClipScale",
              "Media Scale",
              default=0.88,
              min_scale=0.1,
              max_scale=2.5,
              min_allowed=0.05,
              max_allowed=5.0,
              center=1.0,
          ),
          ControlSpec(
              "ClipCenterX",
              "Media Offset X",
              default=0.5,
              min_scale=0.0,
              max_scale=1.0,
              center=0.5,
          ),
          ControlSpec(
              "ClipCenterY",
              "Media Offset Y",
              default=0.5,
              min_scale=0.0,
              max_scale=1.0,
              center=0.5,
          ),
          ControlSpec(
              "ClipAngle",
              "Media Rotation",
              default=0.0,
              min_scale=-180.0,
              max_scale=180.0,
              center=0.0,
          ),
      ],
  ))

  groups.extend([
      (
          "KeyerGroup",
          "2. Green Screen and Matte Cleanup",
          [
              ControlSpec(
                  "EnableChromaKey",
                  "Enable Green Screen Key",
                  input_control="CheckboxControl",
                  default=0,
                  min_allowed=0,
                  max_allowed=1,
                  integer=True,
              ),
              ControlSpec(
                  "KeyScreenRed",
                  "Screen Color Red",
                  default=0.0,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "KeyScreenGreen",
                  "Screen Color Green",
                  default=1.0,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "KeyScreenBlue",
                  "Screen Color Blue",
                  default=0.0,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "KeyGain",
                  "Key Gain",
                  default=1.0,
                  min_scale=0.5,
                  max_scale=2.0,
                  min_allowed=0.0,
                  max_allowed=3.0,
                  center=1.0,
              ),
              ControlSpec(
                  "KeyBalance",
                  "Key Balance",
                  default=0.5,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
                  center=0.5,
              ),
              ControlSpec(
                  "SubjectEdgeErode",
                  "Subject Edge Erode",
                  default=0.0,
                  min_scale=-0.015,
                  max_scale=0.015,
                  min_allowed=-0.05,
                  max_allowed=0.05,
                  center=0.0,
              ),
          ],
      ),
      (
          "PaperBorderGroup",
          "3. Torn Paper Cutout Border",
          [
              ControlSpec(
                  "EnablePaperBorder",
                  "Enable Paper Border",
                  input_control="CheckboxControl",
                  default=1,
                  min_allowed=0,
                  max_allowed=1,
                  integer=True,
              ),
              ControlSpec(
                  "BorderSize",
                  "Border Width",
                  default=0.016,
                  min_scale=0.0,
                  max_scale=0.06,
                  min_allowed=0.0,
                  max_allowed=0.15,
              ),
              ControlSpec(
                  "BorderRoundness",
                  "Corner Smoothness",
                  default=3.5,
                  min_scale=0.0,
                  max_scale=20.0,
                  min_allowed=0.0,
                  max_allowed=60.0,
              ),
              ControlSpec(
                  "TearRoughness",
                  "Torn Edge Roughness",
                  default=0.016,
                  min_scale=0.0,
                  max_scale=0.06,
                  min_allowed=0.0,
                  max_allowed=0.2,
              ),
              ControlSpec(
                  "TearScale",
                  "Tear Frequency",
                  default=22.0,
                  min_scale=2.0,
                  max_scale=60.0,
                  min_allowed=1.0,
                  max_allowed=120.0,
              ),
              ControlSpec(
                  "TearDetail",
                  "Tear Fiber Detail",
                  default=6.0,
                  min_scale=1.0,
                  max_scale=10.0,
                  min_allowed=1.0,
                  max_allowed=10.0,
              ),
              ControlSpec(
                  "EdgeBoilAmount",
                  "Edge Boil Speed",
                  default=0.18,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=5.0,
              ),
              ControlSpec(
                  "PaperRed",
                  "Paper Tint Red",
                  default=0.965,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "PaperGreen",
                  "Paper Tint Green",
                  default=0.945,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "PaperBlue",
                  "Paper Tint Blue",
                  default=0.895,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
          ],
      ),
      (
          "PaperTextureGroup",
          "4. Paper Texture and Fold Creases",
          [
              ControlSpec(
                  "FiberStrength",
                  "Paper Grain Strength",
                  default=0.35,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "TextureScale",
                  "Grain Scale",
                  default=1.0,
                  min_scale=0.2,
                  max_scale=3.0,
                  min_allowed=0.1,
                  max_allowed=10.0,
                  center=1.0,
              ),
              ControlSpec(
                  "CreaseStrength",
                  "Fold Crease Strength",
                  default=0.42,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "CreaseScale",
                  "Crease Scale",
                  default=3.2,
                  min_scale=0.8,
                  max_scale=12.0,
                  min_allowed=0.2,
                  max_allowed=30.0,
              ),
              ControlSpec(
                  "MediaTextureOverlay",
                  "Media Texture Overlay",
                  default=0.24,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "TextureBoil",
                  "Texture Boil Speed",
                  default=0.12,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=4.0,
              ),
              ControlSpec(
                  "RandomSeed",
                  "Texture Seed",
                  default=7.0,
                  min_scale=0.0,
                  max_scale=100.0,
                  min_allowed=0.0,
                  max_allowed=1000.0,
              ),
          ],
      ),
      (
          "TransitionGroup",
          "5. Intro and Outro Paper Transitions",
          [
              ControlSpec(
                  control_id="IntroStyle",
                  label="Intro Style",
                  input_control="ComboControl",
                  default=0,
                  min_scale=0,
                  max_scale=4,
                  min_allowed=0,
                  max_allowed=4,
                  integer=True,
                  combo_options=(
                      "None",
                      "Paper Unfold (3D)",
                      "Crumple Ball Unpack",
                      "Paper Airplane Glide",
                      "Flat Cutout Pop",
                  ),
              ),
              ControlSpec(
                  "IntroFrames",
                  "Intro Duration (Frames)",
                  default=14,
                  min_scale=0,
                  max_scale=60,
                  min_allowed=0,
                  max_allowed=240,
                  integer=True,
              ),
              ControlSpec(
                  control_id="OutroStyle",
                  label="Outro Style",
                  input_control="ComboControl",
                  default=0,
                  min_scale=0,
                  max_scale=4,
                  min_allowed=0,
                  max_allowed=4,
                  integer=True,
                  combo_options=(
                      "None",
                      "Paper Unfold (3D)",
                      "Crumple Ball Pack",
                      "Paper Airplane Glide",
                      "Flat Cutout Pop",
                  ),
              ),
              ControlSpec(
                  "OutroFrames",
                  "Outro Duration (Frames)",
                  default=14,
                  min_scale=0,
                  max_scale=60,
                  min_allowed=0,
                  max_allowed=240,
                  integer=True,
              ),
              ControlSpec(
                  "TransitionAngle",
                  "Transition Angle",
                  default=-22.0,
                  min_scale=-180.0,
                  max_scale=180.0,
                  center=0.0,
              ),
              ControlSpec(
                  "TransitionDistance",
                  "Glide Distance",
                  default=0.65,
                  min_scale=0.0,
                  max_scale=1.5,
                  min_allowed=0.0,
                  max_allowed=3.0,
              ),
              ControlSpec(
                  "TransitionMirrorH",
                  "Mirror Transition H",
                  input_control="CheckboxControl",
                  default=0,
                  min_allowed=0,
                  max_allowed=1,
                  integer=True,
              ),
              ControlSpec(
                  "TransitionMirrorV",
                  "Mirror Transition V",
                  input_control="CheckboxControl",
                  default=0,
                  min_allowed=0,
                  max_allowed=1,
                  integer=True,
              ),
          ],
      ),
      (
          "ShadowGroup",
          "6. 3D Drop Shadow",
          [
              ControlSpec(
                  "ShadowStrength",
                  "Shadow Opacity",
                  default=0.65,
                  min_scale=0.0,
                  max_scale=1.0,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "ShadowAngle",
                  "Shadow Angle",
                  default=-45.0,
                  min_scale=-180.0,
                  max_scale=180.0,
                  center=0.0,
              ),
              ControlSpec(
                  "ShadowDistance",
                  "Shadow Distance",
                  default=0.014,
                  min_scale=0.0,
                  max_scale=0.08,
                  min_allowed=0.0,
                  max_allowed=0.2,
              ),
              ControlSpec(
                  "ShadowBlur",
                  "Shadow Softness",
                  default=0.018,
                  min_scale=0.0,
                  max_scale=0.08,
                  min_allowed=0.0,
                  max_allowed=0.2,
              ),
          ],
      ),
      (
          "StopMotionMasterGroup",
          "7. Stop-Motion and Master Transform",
          [
              ControlSpec(
                  "EnableStopMotion",
                  "Enable Stop-Motion",
                  input_control="CheckboxControl",
                  default=1,
                  min_allowed=0,
                  max_allowed=1,
                  integer=True,
              ),
              ControlSpec(
                  "StopMotionHoldFrames",
                  "Hold Frames (Step)",
                  default=3,
                  min_scale=1,
                  max_scale=12,
                  min_allowed=1,
                  max_allowed=30,
                  integer=True,
              ),
              ControlSpec(
                  "JitterPosition",
                  "Position Jitter",
                  default=0.0025,
                  min_scale=0.0,
                  max_scale=0.03,
                  min_allowed=0.0,
                  max_allowed=0.1,
              ),
              ControlSpec(
                  "JitterRotation",
                  "Rotation Jitter",
                  default=0.75,
                  min_scale=0.0,
                  max_scale=8.0,
                  min_allowed=0.0,
                  max_allowed=30.0,
              ),
              ControlSpec(
                  "MasterCenterX",
                  "Master Position X",
                  default=0.5,
                  min_scale=0.0,
                  max_scale=1.0,
                  center=0.5,
              ),
              ControlSpec(
                  "MasterCenterY",
                  "Master Position Y",
                  default=0.5,
                  min_scale=0.0,
                  max_scale=1.0,
                  center=0.5,
              ),
              ControlSpec(
                  "MasterScale",
                  "Master Scale",
                  default=1.0,
                  min_scale=0.05,
                  max_scale=3.0,
                  min_allowed=0.01,
                  max_allowed=10.0,
                  center=1.0,
              ),
              ControlSpec(
                  "MasterAngle",
                  "Master Rotation",
                  default=0.0,
                  min_scale=-180.0,
                  max_scale=180.0,
                  center=0.0,
              ),
              ControlSpec(
                  "MasterFlipH",
                  "Flip Horizontal",
                  input_control="CheckboxControl",
                  default=0,
                  min_allowed=0,
                  max_allowed=1,
                  integer=True,
              ),
              ControlSpec(
                  "MasterFlipV",
                  "Flip Vertical",
                  input_control="CheckboxControl",
                  default=0,
                  min_allowed=0,
                  max_allowed=1,
                  integer=True,
              ),
          ],
      ),
  ])
  return groups


# --- Paper Cutout FX SimpleExpressions and node graph ---


def _stepped_time_expr(macro_name: str) -> str:
  """Returns Fusion SimpleExpression for optional stop-motion stepped time.

  Generated form, with `t = min(comp.RenderEnd, max(comp.RenderStart, time))`
  and `h = max(1, M.StopMotionHoldFrames)`:

    iif(M.EnableStopMotion > 0.5,
        comp.RenderStart + floor((t - comp.RenderStart) / h) * h, t)

  so time advances in steps of `h` frames (a "shot on twos/threes" look) and
  never leaves the clip range. `max(1, ...)` avoids division by zero.

  Args:
    macro_name: Name of the enclosing Fusion MacroOperator (`M` above).

  Returns:
    SimpleExpression evaluating to a frame number.
  """
  clamped_t = "min(comp.RenderEnd, max(comp.RenderStart, time))"
  hold = f"max(1, {macro_name}.StopMotionHoldFrames)"
  return (
      f"iif({macro_name}.EnableStopMotion > 0.5, "
      f"comp.RenderStart + floor(({clamped_t} - comp.RenderStart) / {hold}) * "
      f"{hold}, {clamped_t})"
  )


def _trans_progress_exprs(macro_name: str) -> tuple[str, str]:
  """Returns (in_prog, out_prog) Fusion SimpleExpressions in [0, 1].

  `in_prog` ramps 0 -> 1 over the first `IntroFrames` frames after
  `comp.RenderStart`; `out_prog` ramps 1 -> 0 over the last `OutroFrames`
  frames before `comp.RenderEnd`. Each is a constant 1 when its style is
  "None" (combo index 0) or its duration is 0, so a disabled side never
  hides the cutout. `max(1, frames)` guards against division by zero.

  Args:
    macro_name: Name of the enclosing Fusion MacroOperator.

  Returns:
    `(in_prog, out_prog)` SimpleExpression strings.
  """
  in_prog = (
      f"iif({macro_name}.IntroStyle > 0.5 and {macro_name}.IntroFrames > 0, "
      "min(1, max(0, (time - comp.RenderStart) / "
      f"max(1, {macro_name}.IntroFrames))), 1)"
  )
  out_prog = (
      f"iif({macro_name}.OutroStyle > 0.5 and {macro_name}.OutroFrames > 0, "
      "min(1, max(0, (comp.RenderEnd - time) / "
      f"max(1, {macro_name}.OutroFrames))), 1)"
  )
  return in_prog, out_prog


def _anim_progress_expr(macro_name: str) -> str:
  """Returns normalized 0..1 visibility multiplier for Intro/Outro transitions.

  1.0 means fully open/flat; 0.0 means fully folded/collapsed off-screen.
  Taking `min(in_prog, out_prog)` lets intro and outro windows overlap on
  short clips without the value ever exceeding either ramp.

  Args:
    macro_name: Name of the enclosing Fusion MacroOperator.

  Returns:
    Fusion SimpleExpression string computing normalized 0..1 visibility.
  """
  in_prog, out_prog = _trans_progress_exprs(macro_name)
  return f"min({in_prog}, {out_prog})"


def _active_trans_style_expr(macro_name: str) -> str:
  """Selects IntroStyle when the intro ramp dominates, else OutroStyle.

  "Dominates" means the intro is enabled and `in_prog <= out_prog`, i.e. the
  intro ramp is the one `min()` picked in `_anim_progress_expr`. Ties go to
  the intro. With `IntroFrames == 0` the intro is never chosen, even at
  `comp.RenderStart` (see `test_active_trans_style_matches_anim_progress`).

  Args:
    macro_name: Name of the enclosing Fusion MacroOperator.

  Returns:
    SimpleExpression evaluating to a style combo index (0..4, as a float).
  """
  in_prog, out_prog = _trans_progress_exprs(macro_name)
  return (
      f"iif({macro_name}.IntroStyle > 0.5 and {macro_name}.IntroFrames > 0 "
      f"and ({in_prog}) <= ({out_prog}), "
      f"{macro_name}.IntroStyle, {macro_name}.OutroStyle)"
  )


def _render_core_cutout_nodes(
    macro_name: str,
    source_output_op: str,
) -> str:
  """Generates the shared Fusion node graph downstream of the media source.

  Node chain (each arrow is a `SourceOp` link; `Pos` values only place nodes
  in the Fusion flow view and do not affect rendering):

    source -> ChromaKeyer (DeltaKeyer) -> KeyBypassSwitch (Dissolve picks
      keyed or raw clip) -> SubjectEdgeChoke (ErodeDilate) ->
      MediaClipTransform (Clip* placement)
    MediaClipTransform -> BorderExpand -> BorderCornerSmooth (Blur) ->
      BorderAlphaCrunch (alpha threshold) -> BorderTearDisplace (displaced by
      TearEdgeNoise) = torn border matte
    PaperBaseColor x PaperFiberNoise x PaperCreaseNoise (Multiply merges) ->
      PaperSheetTextured, masked by the border matte -> PaperCutoutShape
    PaperCutoutShape + MediaClipTransform -> CompositeSubjectOnPaper ->
      MediaPaperOverlay (crease multiply) -> CrumpleDisplace -> FoldDVE ->
      GlideTransform -> CutoutShadow -> StopMotionJitter -> MasterTransform
      -> PosterizeTime (TimeStretcher; the macro's MainOutput1)

  CrumpleDisplace, FoldDVE, and GlideTransform implement the intro/outro
  styles; all other nodes are steady-state look controls.

  Args:
    macro_name: Name of the enclosing MacroOperator; expressions read
      controls as `<macro_name>.<ControlId>`.
    source_output_op: Name of the internal node that receives MainInput1.

  Returns:
    Node definitions (four tabs deep) to append inside the macro `Tools`
    block, starting with a newline.
  """
  p_expr = _anim_progress_expr(macro_name)
  style_expr = _active_trans_style_expr(macro_name)
  step_t = _stepped_time_expr(macro_name)

  # Precomputed signed multipliers for horizontal/vertical transition mirror.
  # mh/mv are -1 or 1 (multiply a direction); flip_h/flip_v are 0 or 1
  # because Transform.FlipHoriz/FlipVert are checkbox inputs.
  mh = f"iif({macro_name}.TransitionMirrorH > 0.5, -1, 1)"
  mv = f"iif({macro_name}.TransitionMirrorV > 0.5, -1, 1)"
  flip_h = f"iif({macro_name}.MasterFlipH > 0.5, 1, 0)"
  flip_v = f"iif({macro_name}.MasterFlipV > 0.5, 1, 0)"

  # Transition curve (smooth ease-out cubic: 1 - (1 - p)^3).
  # inv_p is the remaining "closedness" (1 at the hidden end, 0 when flat), so
  # every style amount below is written as `amount * inv_p` and fades to 0.
  ease_p = f"(1 - pow(1 - ({p_expr}), 3))"
  inv_p = f"(1 - ({ease_p}))"

  # 1 = Paper Unfold (3D), 2 = Crumple Ball, 3 = Airplane Glide, 4 = Flat Pop.
  # `floor(style + 0.5) == k` rounds the float combo index before comparing.
  # Amounts: Displace refraction (fraction of frame), DVE rotations in
  # degrees, DVE ZMove in scene units, Transform Center in normalized 0..1.
  crumple_str_expr = (
      f"iif(floor({style_expr} + 0.5) == 2, 0.22 * ({inv_p}), "
      f"iif(floor({style_expr} + 0.5) == 1, 0.045 * ({inv_p}), 0))"
  )
  dve_yrot_expr = (
      f"iif(floor({style_expr} + 0.5) == 1, "
      f"({mh}) * 88 * ({inv_p}), "
      f"iif(floor({style_expr} + 0.5) == 3, "
      f"({mh}) * 35 * ({inv_p}), 0))"
  )
  dve_xrot_expr = (
      f"iif(floor({style_expr} + 0.5) == 1, "
      f"({mv}) * 24 * ({inv_p}), "
      f"iif(floor({style_expr} + 0.5) == 3, "
      f"({mv}) * (-28) * ({inv_p}), 0))"
  )
  dve_zmove_expr = f"iif(floor({style_expr} + 0.5) == 2, 1.6 * ({inv_p}), 0)"
  # Glide (3) travels TransitionDistance along TransitionAngle (degrees ->
  # radians via pi / 180); Flat Pop (4) drops in from 0.35 below center.
  glide_center_expr = (
      "Point("
      f"0.5 + iif(floor({style_expr} + 0.5) == 3, "
      f"({mh}) * {macro_name}.TransitionDistance * ({inv_p}) * "
      f"cos({macro_name}.TransitionAngle * pi / 180), 0), "
      f"0.5 + iif(floor({style_expr} + 0.5) == 3, "
      f"({mv}) * {macro_name}.TransitionDistance * ({inv_p}) * "
      f"sin({macro_name}.TransitionAngle * pi / 180), "
      f"iif(floor({style_expr} + 0.5) == 4, "
      f"({mv}) * (-0.35) * ({inv_p}), 0))"
      ")"
  )
  # Scale-in per style. The max(0.001/0.05, ...) floors keep Transform.Size
  # strictly positive so the cutout never collapses to a zero-size image.
  glide_size_expr = (
      f"iif(floor({style_expr} + 0.5) == 4, "
      f"max(0.001, {ease_p}), "
      f"iif(floor({style_expr} + 0.5) == 2, "
      f"max(0.05, 0.25 + 0.75 * ({ease_p})), "
      f"iif(floor({style_expr} + 0.5) == 1, "
      f"max(0.05, 0.4 + 0.6 * ({ease_p})), 1)))"
  )
  glide_angle_expr = (
      f"iif(floor({style_expr} + 0.5) == 3, "
      f"({mh}) * ({macro_name}.TransitionAngle * 0.4) * ({inv_p}), "
      f"iif(floor({style_expr} + 0.5) == 2, "
      f"({mh}) * 35 * ({inv_p}), 0))"
  )

  # Long SimpleExpression lines inside the template are emitted verbatim into
  # the .setting file; wrapping them would change the generated bytes.
  # pylint: disable=line-too-long
  return f"""
\t\t\t\tChromaKeyer = DeltaKeyer {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tBackgroundRed = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.KeyScreenRed",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBackgroundGreen = Input {{
\t\t\t\t\t\t\tValue = 1.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.KeyScreenGreen",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBackgroundBlue = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.KeyScreenBlue",
\t\t\t\t\t\t}},
\t\t\t\t\t\tGain = Input {{
\t\t\t\t\t\t\tValue = 1.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.KeyGain",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBalance = Input {{
\t\t\t\t\t\t\tValue = 0.5,
\t\t\t\t\t\t\tExpression = "{macro_name}.KeyBalance",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "{source_output_op}",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 220, 49.5 }} }},
\t\t\t\t}},
\t\t\t\tKeyBypassSwitch = Dissolve {{
\t\t\t\t\tTransitions = {{ [0] = "DFTDissolve" }},
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tMix = Input {{
\t\t\t\t\t\t\tValue = 0,
\t\t\t\t\t\t\tExpression = "iif({macro_name}.EnableChromaKey > 0.5, 1, 0)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBackground = Input {{
\t\t\t\t\t\t\tSourceOp = "{source_output_op}",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "ChromaKeyer",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 330, 49.5 }} }},
\t\t\t\t}},
\t\t\t\tSubjectEdgeChoke = ErodeDilate {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tFilter = Input {{ Value = 3, }},
\t\t\t\t\t\tXAmount = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.SubjectEdgeErode",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "KeyBypassSwitch",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 440, 49.5 }} }},
\t\t\t\t}},
\t\t\t\tMediaClipTransform = Transform {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tCenter = Input {{
\t\t\t\t\t\t\tValue = {{ 0.5, 0.5 }},
\t\t\t\t\t\t\tExpression = "Point({macro_name}.ClipCenterX, {macro_name}.ClipCenterY)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tSize = Input {{
\t\t\t\t\t\t\tValue = 0.88,
\t\t\t\t\t\t\tExpression = "{macro_name}.ClipScale",
\t\t\t\t\t\t}},
\t\t\t\t\t\tAngle = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.ClipAngle",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "SubjectEdgeChoke",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 550, 49.5 }} }},
\t\t\t\t}},
\t\t\t\tBorderExpand = ErodeDilate {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tFilter = Input {{ Value = 3, }},
\t\t\t\t\t\tXAmount = Input {{
\t\t\t\t\t\t\tValue = 0.016,
\t\t\t\t\t\t\tExpression = "iif({macro_name}.EnablePaperBorder > 0.5, {macro_name}.BorderSize, 0)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "MediaClipTransform",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 550, 115.5 }} }},
\t\t\t\t}},
\t\t\t\tBorderCornerSmooth = Blur {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tFilter = Input {{ Value = FuID {{ "Fast Gaussian" }}, }},
\t\t\t\t\t\tXBlurSize = Input {{
\t\t\t\t\t\t\tValue = 3.5,
\t\t\t\t\t\t\tExpression = "iif({macro_name}.EnablePaperBorder > 0.5, {macro_name}.BorderRoundness, 0)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "BorderExpand",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 660, 115.5 }} }},
\t\t\t\t}},
\t\t\t\tBorderAlphaCrunch = BrightnessContrast {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tAlpha = Input {{ Value = 1, }},
\t\t\t\t\t\tLow = Input {{ Value = 0.15, }},
\t\t\t\t\t\tHigh = Input {{ Value = 0.25, }},
\t\t\t\t\t\tClipBlack = Input {{ Value = 1, }},
\t\t\t\t\t\tClipWhite = Input {{ Value = 1, }},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "BorderCornerSmooth",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 770, 115.5 }} }},
\t\t\t\t}},
\t\t\t\tTearEdgeNoise = FastNoise {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tUseFrameFormatSettings = Input {{ Value = 1, }},
\t\t\t\t\t\tDetail = Input {{
\t\t\t\t\t\t\tValue = 6.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.TearDetail",
\t\t\t\t\t\t}},
\t\t\t\t\t\tContrast = Input {{ Value = 2.4, }},
\t\t\t\t\t\tXScale = Input {{
\t\t\t\t\t\t\tValue = 22.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.TearScale",
\t\t\t\t\t\t}},
\t\t\t\t\t\tSeethe = Input {{
\t\t\t\t\t\t\tValue = 7.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.RandomSeed + ({step_t}) * {macro_name}.EdgeBoilAmount * 0.15",
\t\t\t\t\t\t}},
\t\t\t\t\t\tColor1Alpha = Input {{ Value = 1, }},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 770, 181.5 }} }},
\t\t\t\t}},
\t\t\t\tBorderTearDisplace = Displace {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tType = Input {{ Value = 1, }},
\t\t\t\t\t\tXOffset = Input {{ Value = -0.5, }},
\t\t\t\t\t\tYOffset = Input {{ Value = -0.5, }},
\t\t\t\t\t\tXRefraction = Input {{
\t\t\t\t\t\t\tValue = 0.016,
\t\t\t\t\t\t\tExpression = "iif({macro_name}.EnablePaperBorder > 0.5, {macro_name}.TearRoughness, 0)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tYRefraction = Input {{
\t\t\t\t\t\t\tValue = 0.016,
\t\t\t\t\t\t\tExpression = "iif({macro_name}.EnablePaperBorder > 0.5, {macro_name}.TearRoughness, 0)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "BorderAlphaCrunch",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "TearEdgeNoise",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 880, 115.5 }} }},
\t\t\t\t}},
\t\t\t\tPaperBaseColor = Background {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tUseFrameFormatSettings = Input {{ Value = 1, }},
\t\t\t\t\t\tTopLeftRed = Input {{
\t\t\t\t\t\t\tValue = 0.965,
\t\t\t\t\t\t\tExpression = "{macro_name}.PaperRed",
\t\t\t\t\t\t}},
\t\t\t\t\t\tTopLeftGreen = Input {{
\t\t\t\t\t\t\tValue = 0.945,
\t\t\t\t\t\t\tExpression = "{macro_name}.PaperGreen",
\t\t\t\t\t\t}},
\t\t\t\t\t\tTopLeftBlue = Input {{
\t\t\t\t\t\t\tValue = 0.895,
\t\t\t\t\t\t\tExpression = "{macro_name}.PaperBlue",
\t\t\t\t\t\t}},
\t\t\t\t\t\tTopLeftAlpha = Input {{ Value = 1, }},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 660, 247.5 }} }},
\t\t\t\t}},
\t\t\t\tPaperFiberNoise = FastNoise {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tUseFrameFormatSettings = Input {{ Value = 1, }},
\t\t\t\t\t\tDetail = Input {{ Value = 8.0, }},
\t\t\t\t\t\tContrast = Input {{ Value = 1.6, }},
\t\t\t\t\t\tXScale = Input {{
\t\t\t\t\t\t\tValue = 45.0,
\t\t\t\t\t\t\tExpression = "45.0 / max(0.1, {macro_name}.TextureScale)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tSeethe = Input {{
\t\t\t\t\t\t\tValue = 7.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.RandomSeed * 1.3 + ({step_t}) * {macro_name}.TextureBoil * 0.12",
\t\t\t\t\t\t}},
\t\t\t\t\t\tColor1Red = Input {{ Value = 0.80, }},
\t\t\t\t\t\tColor1Green = Input {{ Value = 0.77, }},
\t\t\t\t\t\tColor1Blue = Input {{ Value = 0.72, }},
\t\t\t\t\t\tColor1Alpha = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Red = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Green = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Blue = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Alpha = Input {{ Value = 1.0, }},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 770, 247.5 }} }},
\t\t\t\t}},
\t\t\t\tPaperCreaseNoise = FastNoise {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tUseFrameFormatSettings = Input {{ Value = 1, }},
\t\t\t\t\t\tDetail = Input {{ Value = 3.0, }},
\t\t\t\t\t\tContrast = Input {{ Value = 2.8, }},
\t\t\t\t\t\tDiscontinuous = Input {{ Value = 1, }},
\t\t\t\t\t\tInverted = Input {{ Value = 1, }},
\t\t\t\t\t\tXScale = Input {{
\t\t\t\t\t\t\tValue = 3.2,
\t\t\t\t\t\t\tExpression = "{macro_name}.CreaseScale",
\t\t\t\t\t\t}},
\t\t\t\t\t\tSeethe = Input {{
\t\t\t\t\t\t\tValue = 7.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.RandomSeed * 2.1 + ({step_t}) * {macro_name}.TextureBoil * 0.08",
\t\t\t\t\t\t}},
\t\t\t\t\t\tColor1Red = Input {{ Value = 0.76, }},
\t\t\t\t\t\tColor1Green = Input {{ Value = 0.74, }},
\t\t\t\t\t\tColor1Blue = Input {{ Value = 0.70, }},
\t\t\t\t\t\tColor1Alpha = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Red = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Green = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Blue = Input {{ Value = 1.0, }},
\t\t\t\t\t\tColor2Alpha = Input {{ Value = 1.0, }},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 770, 313.5 }} }},
\t\t\t\t}},
\t\t\t\tPaperSheetWithGrain = Merge {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tApplyMode = Input {{ Value = FuID {{ "Multiply" }}, }},
\t\t\t\t\t\tBlend = Input {{
\t\t\t\t\t\t\tValue = 0.35,
\t\t\t\t\t\t\tExpression = "{macro_name}.FiberStrength",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBackground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperBaseColor",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperFiberNoise",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 880, 247.5 }} }},
\t\t\t\t}},
\t\t\t\tPaperSheetTextured = Merge {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tApplyMode = Input {{ Value = FuID {{ "Multiply" }}, }},
\t\t\t\t\t\tBlend = Input {{
\t\t\t\t\t\t\tValue = 0.42,
\t\t\t\t\t\t\tExpression = "{macro_name}.CreaseStrength",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBackground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperSheetWithGrain",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperCreaseNoise",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 990, 247.5 }} }},
\t\t\t\t}},
\t\t\t\tTransparentCanvas = Background {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tUseFrameFormatSettings = Input {{ Value = 1, }},
\t\t\t\t\t\tTopLeftAlpha = Input {{ Value = 0, }},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 880, 16.5 }} }},
\t\t\t\t}},
\t\t\t\tPaperCutoutShape = Merge {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tBlend = Input {{
\t\t\t\t\t\t\tValue = 1,
\t\t\t\t\t\t\tExpression = "iif({macro_name}.EnablePaperBorder > 0.5, 1, 0)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBackground = Input {{
\t\t\t\t\t\t\tSourceOp = "TransparentCanvas",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperSheetTextured",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tEffectMask = Input {{
\t\t\t\t\t\t\tSourceOp = "BorderTearDisplace",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 990, 115.5 }} }},
\t\t\t\t}},
\t\t\t\tCompositeSubjectOnPaper = Merge {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tBackground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperCutoutShape",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "MediaClipTransform",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1100, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tMediaPaperOverlay = Merge {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tApplyMode = Input {{ Value = FuID {{ "Multiply" }}, }},
\t\t\t\t\t\tOperator = Input {{ Value = FuID {{ "Atop" }}, }},
\t\t\t\t\t\tBlend = Input {{
\t\t\t\t\t\t\tValue = 0.24,
\t\t\t\t\t\t\tExpression = "{macro_name}.MediaTextureOverlay",
\t\t\t\t\t\t}},
\t\t\t\t\t\tBackground = Input {{
\t\t\t\t\t\t\tSourceOp = "CompositeSubjectOnPaper",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperCreaseNoise",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1210, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tCrumpleDisplace = Displace {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tType = Input {{ Value = 1, }},
\t\t\t\t\t\tXOffset = Input {{ Value = -0.5, }},
\t\t\t\t\t\tYOffset = Input {{ Value = -0.5, }},
\t\t\t\t\t\tXRefraction = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{crumple_str_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tYRefraction = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{crumple_str_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "MediaPaperOverlay",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t\tForeground = Input {{
\t\t\t\t\t\t\tSourceOp = "PaperCreaseNoise",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1320, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tFoldDVE = DVE {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tXRotation = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{dve_xrot_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tYRotation = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{dve_yrot_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tZMove = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{dve_zmove_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "CrumpleDisplace",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1430, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tGlideTransform = Transform {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tCenter = Input {{
\t\t\t\t\t\t\tValue = {{ 0.5, 0.5 }},
\t\t\t\t\t\t\tExpression = "{glide_center_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tSize = Input {{
\t\t\t\t\t\t\tValue = 1.0,
\t\t\t\t\t\t\tExpression = "{glide_size_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tAngle = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{glide_angle_expr}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "FoldDVE",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1540, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tCutoutShadow = Shadow {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tShadowOffset = Input {{
\t\t\t\t\t\t\tValue = {{ 0.5, 0.5 }},
\t\t\t\t\t\t\tExpression = "Point(0.5 + {macro_name}.ShadowDistance * cos({macro_name}.ShadowAngle * pi / 180), 0.5 + {macro_name}.ShadowDistance * sin({macro_name}.ShadowAngle * pi / 180))",
\t\t\t\t\t\t}},
\t\t\t\t\t\tSoftness = Input {{
\t\t\t\t\t\t\tValue = 0.018,
\t\t\t\t\t\t\tExpression = "{macro_name}.ShadowBlur",
\t\t\t\t\t\t}},
\t\t\t\t\t\tAlpha = Input {{
\t\t\t\t\t\t\tValue = 0.65,
\t\t\t\t\t\t\tExpression = "{macro_name}.ShadowStrength",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "GlideTransform",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1650, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tStopMotionJitter = Transform {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tCenter = Input {{
\t\t\t\t\t\t\tValue = {{ 0.5, 0.5 }},
\t\t\t\t\t\t\tExpression = "Point(0.5 + iif({macro_name}.EnableStopMotion > 0.5, {macro_name}.JitterPosition * sin(({step_t}) * 12.9898 + {macro_name}.RandomSeed), 0), 0.5 + iif({macro_name}.EnableStopMotion > 0.5, {macro_name}.JitterPosition * cos(({step_t}) * 78.233 + {macro_name}.RandomSeed), 0))",
\t\t\t\t\t\t}},
\t\t\t\t\t\tAngle = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "iif({macro_name}.EnableStopMotion > 0.5, {macro_name}.JitterRotation * sin(({step_t}) * 43.17 + {macro_name}.RandomSeed), 0)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "CutoutShadow",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1760, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tMasterTransform = Transform {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tCenter = Input {{
\t\t\t\t\t\t\tValue = {{ 0.5, 0.5 }},
\t\t\t\t\t\t\tExpression = "Point({macro_name}.MasterCenterX, {macro_name}.MasterCenterY)",
\t\t\t\t\t\t}},
\t\t\t\t\t\tSize = Input {{
\t\t\t\t\t\t\tValue = 1.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.MasterScale",
\t\t\t\t\t\t}},
\t\t\t\t\t\tAngle = Input {{
\t\t\t\t\t\t\tValue = 0.0,
\t\t\t\t\t\t\tExpression = "{macro_name}.MasterAngle",
\t\t\t\t\t\t}},
\t\t\t\t\t\tFlipHoriz = Input {{
\t\t\t\t\t\t\tValue = 0,
\t\t\t\t\t\t\tExpression = "{flip_h}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tFlipVert = Input {{
\t\t\t\t\t\t\tValue = 0,
\t\t\t\t\t\t\tExpression = "{flip_v}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "StopMotionJitter",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1870, 82.5 }} }},
\t\t\t\t}},
\t\t\t\tPosterizeTime = TimeStretcher {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t\tSourceTime = Input {{
\t\t\t\t\t\t\tValue = 0,
\t\t\t\t\t\t\tExpression = "{step_t}",
\t\t\t\t\t\t}},
\t\t\t\t\t\tInterpolateBetweenFrames = Input {{ Value = 0, }},
\t\t\t\t\t\tInput = Input {{
\t\t\t\t\t\t\tSourceOp = "MasterTransform",
\t\t\t\t\t\t\tSource = "Output",
\t\t\t\t\t\t}},
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 1980, 82.5 }} }},
\t\t\t\t}},"""
  # pylint: enable=line-too-long


# --- Paper Cutout FX effect template ---


def build_effect_setting() -> str:
  """Builds the Paper Cutout FX direct-clip Edit Effect .setting file.

  Resolve feeds the clip into `MainInput1`, which lands on the pass-through
  `SourceClipInput` node (a BrightnessContrast with default inputs, used as
  a named entry point) and flows through `_render_core_cutout_nodes`.

  Returns:
    Complete `.setting` file contents (UTF-8 text, trailing newline).
  """
  macro_name = "PaperCutoutFX"
  groups = _core_cutout_control_groups()
  default_open = ("MediaSourceGroup", "PaperBorderGroup", "TransitionGroup")
  group_defaults = _render_group_defaults(groups, default_open)
  user_controls = _render_user_controls(groups, default_open)
  core_nodes = _render_core_cutout_nodes(macro_name, "SourceClipInput")

  return f"""{{
\tTools = ordered() {{
\t\t{macro_name} = MacroOperator {{
\t\t\tCtrlWZoom = false,
\t\t\tCustomData = {{
\t\t\t\tHelpPage = "PaperCraft Studio Pro Effect",
\t\t\t}},
\t\t\tInputs = ordered() {{
\t\t\t\tMainInput1 = InstanceInput {{
\t\t\t\t\tSourceOp = "SourceClipInput",
\t\t\t\t\tSource = "Input",
\t\t\t\t}},
{group_defaults}
\t\t\t}},
\t\t\tOutputs = {{
\t\t\t\tMainOutput1 = InstanceOutput {{
\t\t\t\t\tSourceOp = "PosterizeTime",
\t\t\t\t\tSource = "Output",
\t\t\t\t}},
\t\t\t}},
\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 0, 0 }} }},
\t\t\tTools = ordered() {{
\t\t\t\tSourceClipInput = BrightnessContrast {{
\t\t\t\t\tInputs = {{
\t\t\t\t\t}},
\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 110, 49.5 }} }},
\t\t\t\t}},{core_nodes}
\t\t\t}},
{user_controls}
\t\t}},
\t}},
\tActiveTool = "{macro_name}",
}}
"""


# --- Compact node-rendering helpers (used by the fold transition) ---


def _node(
    name: str,
    tool: str,
    inputs: Sequence[str],
    pos: tuple[float, float],
    preamble: str = "",
) -> str:
  """Renders one internal tool of a macro's Tools block.

  Args:
    name: Unique node name; other nodes link to it via `SourceOp`.
    tool: Fusion tool registry ID, e.g. `Merge`, `Transform`, `DVE`.
    inputs: Pre-rendered input rows from `_expr`, `_link`, or `_value`.
    pos: Flow-view position `(x, y)`; cosmetic only.
    preamble: Optional raw line placed before `Inputs`, e.g. `_DISSOLVE`.

  Returns:
    Multi-line node definition, four tabs deep, ending in `},`.
  """
  lines = [f"\t\t\t\t{name} = {tool} {{"]
  if preamble:
    lines.append(f"\t\t\t\t\t{preamble}")
  lines.append("\t\t\t\t\tInputs = {")
  lines.extend(f"\t\t\t\t\t\t{entry}" for entry in inputs)
  lines.append("\t\t\t\t\t},")
  lines.append(
      f"\t\t\t\t\tViewInfo = OperatorInfo {{ Pos = {{ {pos[0]:g}, {pos[1]:g} }}"
      " },"
  )
  lines.append("\t\t\t\t},")
  return "\n".join(lines)


def _expr(name: str, value: str, expression: str) -> str:
  """Renders an input driven by a SimpleExpression.

  Args:
    name: Tool input name, e.g. `Mix` or `Center`.
    value: Static fallback value in Lua syntax (`"0"`, `"{ 0.5, 0.5 }"`).
    expression: SimpleExpression; must not contain double quotes.

  Returns:
    One `<name> = Input { Value = ..., Expression = "...", },` row.

  Raises:
    ValueError: If `expression` is empty/blank or contains a double quote,
      which would end the unescaped Lua string early and corrupt the
      `.setting` file.
  """
  if not expression.strip():
    raise ValueError(f"Empty SimpleExpression for input {name!r}.")
  if '"' in expression:
    raise ValueError(
        f"SimpleExpression for input {name!r} must not contain '\"': "
        f"{expression}"
    )
  return f'{name} = Input {{ Value = {value}, Expression = "{expression}", }},'


def _link(name: str, source_op: str, source: str = "Output") -> str:
  """Renders an input wired to another node's output.

  Args:
    name: Tool input name, e.g. `Background`, `Foreground`, `EffectMask`.
    source_op: Name of the upstream node inside the same macro.
    source: Upstream output name; `"Output"` for images, `"Mask"` for masks.

  Returns:
    One `<name> = Input { SourceOp = "...", Source = "...", },` row.
  """
  return f'{name} = Input {{ SourceOp = "{source_op}", Source = "{source}", }},'


def _value(name: str, value: str) -> str:
  """Renders an input with a constant value.

  Args:
    name: Tool input name.
    value: Value in Lua syntax, e.g. `"1"`, `'FuID { "Atop" }'`.

  Returns:
    One `<name> = Input { Value = ..., },` row.
  """
  return f"{name} = Input {{ Value = {value}, }},"


# Preamble that every Dissolve tool carries in saved comps; selects the plain
# cross-dissolve so `Mix` acts as a 0/1 switch between Background/Foreground.
_DISSOLVE = 'Transitions = { [0] = "DFTDissolve" },'


# --- Paper Fold & Unfold transition template ---


def _fold_transition_control_groups() -> (
    list[tuple[str, str, list[ControlSpec]]]
):
  """Inspector controls of the Paper Fold & Unfold Fusion transition.

  Returns:
    Ordered `(group_id, group_title, controls)` tuples. Distances are
    normalized frame units, angles are degrees, holds are frames.
  """
  checkbox = dict(
      input_control="CheckboxControl",
      min_allowed=0,
      max_allowed=1,
      integer=True,
  )
  return [
      (
          "FoldGroup",
          "1. Fold and Unfold",
          [
              ControlSpec(
                  "FoldAction",
                  "Fold Action",
                  input_control="ComboControl",
                  default=0,
                  min_scale=0,
                  max_scale=2,
                  min_allowed=0,
                  max_allowed=2,
                  integer=True,
                  combo_options=(
                      "Unfold Incoming Clip",
                      "Fold Outgoing Clip",
                      "Fold Out, Then Unfold In",
                  ),
              ),
              ControlSpec(
                  "SlideDistance",
                  "Packet Slide Distance",
                  default=0.6,
                  min_scale=0.0,
                  max_scale=1.5,
                  min_allowed=0.0,
                  max_allowed=3.0,
              ),
              ControlSpec(
                  "SlideAngle",
                  "Slide Angle",
                  default=-120.0,
                  min_scale=-180.0,
                  max_scale=180.0,
                  center=0.0,
              ),
              ControlSpec("MirrorH", "Mirror Horizontal", **checkbox),
          ],
      ),
      (
          "PaperGroup",
          "2. Paper Look",
          [
              ControlSpec(
                  "PaperRed",
                  "Paper Color Red",
                  default=0.965,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "PaperGreen",
                  "Paper Color Green",
                  default=0.945,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "PaperBlue",
                  "Paper Color Blue",
                  default=0.895,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "CreaseStrength",
                  "Crease Strength",
                  default=0.52,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
          ],
      ),
      (
          "MotionGroup",
          "3. Stop-Motion and Shadow",
          [
              ControlSpec(
                  "EnableStopMotion",
                  "Enable Stop-Motion",
                  default=1,
                  **checkbox,
              ),
              ControlSpec(
                  "StopMotionHoldFrames",
                  "Hold Frames (Step)",
                  default=2,
                  min_scale=1,
                  max_scale=8,
                  min_allowed=1,
                  max_allowed=24,
                  integer=True,
              ),
              ControlSpec(
                  "JitterAmount",
                  "Hand Jitter",
                  default=0.25,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "ShadowStrength",
                  "Shadow Opacity",
                  default=0.62,
                  min_allowed=0.0,
                  max_allowed=1.0,
              ),
              ControlSpec(
                  "ShadowAngle",
                  "Shadow Angle",
                  default=-45.0,
                  min_scale=-180.0,
                  max_scale=180.0,
                  center=0.0,
              ),
              ControlSpec(
                  "ShadowDistance",
                  "Shadow Distance",
                  default=0.014,
                  max_scale=0.08,
                  min_allowed=0.0,
                  max_allowed=0.2,
              ),
              ControlSpec(
                  "ShadowSoftness",
                  "Shadow Softness",
                  default=0.02,
                  max_scale=0.08,
                  min_allowed=0.0,
                  max_allowed=0.2,
              ),
          ],
      ),
  ]


def build_fold_transition_setting() -> str:
  """Builds the Paper Fold & Unfold Edit-page Fusion transition .setting.

  A lighter companion to the OpenFX transition: a three-panel letter fold whose
  left panel opens first and right panel second, with paper backs, creases,
  a sliding folded packet, stop-motion holds, jitter, and a drop shadow. The
  hinges are vertical, so the layout holds for any frame aspect ratio.

  Graph outline: OutgoingInput/IncomingInput -> ActiveClip (which clip is
  being folded) -> ActiveCreased -> MirrorIn -> three RectangleMask panels
  (Left/Middle/Right, each a third of the width plus 0.002 overlap to hide
  seams) -> Left/Right flaps get a paper back and swing on a DVE about their
  hinge -> SheetRight/SheetFolded stack the panels -> MirrorOut ->
  PacketMotion (toss/slide) -> PacketShadow -> HandJitter -> composited over
  the underlay clip -> ExactEndSwitch/ExactBoundaryClamp force the pure
  incoming/outgoing clip on the first and last frames.

  Returns:
    Complete .setting file contents.
  """
  m = "PaperFoldTransition"
  groups = _fold_transition_control_groups()
  default_open = ("FoldGroup", "MotionGroup")
  group_defaults = _render_group_defaults(groups, default_open)
  user_controls = _render_user_controls(groups, default_open)

  # Transition progress p in [0, 1] across the transition's comp range. With
  # stop-motion on, p advances only every `hold` frames (step_index counts
  # held steps). p is pinned to exactly 0/1 at the range boundaries.
  dur = "max(1, comp.RenderEnd - comp.RenderStart)"
  hold = f"max(1, {m}.StopMotionHoldFrames)"
  step_index = f"floor((time - comp.RenderStart) / {hold})"
  raw_p = f"(time - comp.RenderStart) / {dur}"
  chosen_p = (
      f"iif({m}.EnableStopMotion > 0.5, {step_index} * {hold} / {dur}, {raw_p})"
  )
  p = (
      "iif(time <= comp.RenderStart, 0, iif(time >= comp.RenderEnd, 1, "
      f"min(1, max(0, {chosen_p}))))"
  )
  # Openness of the active sheet: 1 = flat, 0 = folded packet.
  # FoldAction 0 (unfold in): q = p. 1 (fold out): q = 1 - p. 2 (fold out,
  # then unfold in): q goes 1 -> 0 over the first half and 0 -> 1 after.
  q = (
      f"iif({m}.FoldAction < 0.5, {p}, iif({m}.FoldAction < 1.5, 1 - ({p}), "
      f"iif(({p}) < 0.5, 1 - 2 * ({p}), 2 * ({p}) - 1)))"
  )
  # 1 when the folding sheet shows the outgoing clip (fold out, or first half
  # of fold-out-then-in); drives the ActiveClip Dissolve.
  outgoing_active = (
      f"iif({m}.FoldAction > 0.5 and {m}.FoldAction < 1.5, 1, "
      f"iif({m}.FoldAction > 1.5 and ({p}) < 0.5, 1, 0))"
  )
  # Cosine eases keep each stage's progress inside one reference.
  # Flap angles in degrees (0 = flat; 176/174 stop just short of a full
  # 180-degree fold). The left flap moves while q is in
  # [0.1, 0.64] and the right one in [0.505, 1.0]: as q rises (unfolding),
  # the left panel opens first, then the right.
  angle_left = (
      f"176 * (0.5 + 0.5 * cos(pi * min(1, max(0, (({q}) - 0.1) / 0.54))))"
  )
  angle_right = (
      f"174 * (0.5 + 0.5 * cos(pi * min(1, max(0, (({q}) - 0.505) / 0.495))))"
  )
  # toss: 1 -> 0 over q in [0, 0.18], squared; drives the folded packet's
  # slide, scale, spin, and shadow lift. settle: 0 when flat, 1 once q <= 0.88;
  # fades in creases, shadow, and hand jitter as the sheet folds.
  toss = f"pow(0.5 + 0.5 * cos(pi * min(1, max(0, ({q}) / 0.18))), 2)"
  settle = f"min(1, max(0, (1 - ({q})) / 0.12))"
  mirror_sign = f"iif({m}.MirrorH > 0.5, -1, 1)"
  third = 1.0 / 3.0
  # Panel mask width: one third of the frame plus 0.002 so adjacent panels
  # overlap slightly and no seam shows between them.
  panel_w = f"{third + 0.002:.6f}"

  def panel_mask(name: str, center_x: float, y: float) -> str:
    """Renders a full-height RectangleMask covering one vertical third.

    Args:
      name: Node name.
      center_x: Mask center X in normalized frame units (0..1).
      y: Flow-view Y position (cosmetic).

    Returns:
      RectangleMask node text. Height 4 (normalized units) overshoots the
      frame so the mask always spans the full height.
    """
    return _node(
        name,
        "RectangleMask",
        [
            _value("UseFrameFormatSettings", "1"),
            _value("Center", f"{{ {center_x:.6f}, 0.5 }}"),
            _value("Width", panel_w),
            _value("Height", "4"),
        ],
        (440, y),
    )

  def masked_panel(name: str, mask: str, y: float) -> str:
    """Cuts one panel out of the (mirrored) active clip.

    Args:
      name: Node name, e.g. `LeftFront`.
      mask: Name of the RectangleMask node used as `EffectMask`.
      y: Flow-view Y position (cosmetic).

    Returns:
      Merge node placing the masked clip over `TransparentCanvas`.
    """
    return _node(
        name,
        "Merge",
        [
            _link("Background", "TransparentCanvas"),
            _link("Foreground", "MirrorIn"),
            _link("EffectMask", mask, "Mask"),
        ],
        (550, y),
    )

  def panel_back(name: str, front: str, y: float) -> str:
    """Builds the plain paper back of a flap.

    Uses an `Atop` merge so `PaperBack` fills only the front panel's alpha.

    Args:
      name: Node name, e.g. `LeftBack`.
      front: Name of the matching masked front panel node.
      y: Flow-view Y position (cosmetic).

    Returns:
      Merge node text.
    """
    return _node(
        name,
        "Merge",
        [
            _value("Operator", 'FuID { "Atop" }'),
            _link("Background", front),
            _link("Foreground", "PaperBack"),
        ],
        (660, y),
    )

  def flap_chain(prefix: str, angle: str, shift: float, y: float) -> list[str]:
    """Builds the four nodes that swing one flap about its hinge.

    `<prefix>Face` shows the front until the flap passes 90 degrees, then
    the paper back. DVE rotates around the frame center, so `HingeIn` shifts
    the flap until its hinge sits at x = 0.5, `Swing` rotates it about Y,
    and `HingeOut` shifts it back.

    Args:
      prefix: `Left` or `Right`; expects `<prefix>Front`/`<prefix>Back`.
      angle: SimpleExpression for the fold angle in degrees (0 = flat).
      shift: Horizontal offset (normalized) that moves the hinge to the
        center; its sign also picks the rotation direction.
      y: Flow-view Y position (cosmetic).

    Returns:
      `[Face, HingeIn, Swing, HingeOut]` node texts in link order.
    """
    face = f"{prefix}Face"
    return [
        _node(
            face,
            "Dissolve",
            [
                _expr("Mix", "0", f"iif(({angle}) > 90, 1, 0)"),
                _link("Background", f"{prefix}Front"),
                _link("Foreground", f"{prefix}Back"),
            ],
            (770, y),
            preamble=_DISSOLVE,
        ),
        # Move the hinge to the frame center, swing, and move it back.
        _node(
            f"{prefix}HingeIn",
            "Transform",
            [
                _value("Center", f"{{ {0.5 + shift:.6f}, 0.5 }}"),
                _link("Input", face),
            ],
            (880, y),
        ),
        _node(
            f"{prefix}Swing",
            "DVE",
            [
                _expr(
                    "YRotation",
                    "0.0",
                    f"{'-' if shift < 0 else ''}({angle})",
                ),
                _link("Input", f"{prefix}HingeIn"),
            ],
            (990, y),
        ),
        _node(
            f"{prefix}HingeOut",
            "Transform",
            [
                _value("Center", f"{{ {0.5 - shift:.6f}, 0.5 }}"),
                _link("Input", f"{prefix}Swing"),
            ],
            (1100, y),
        ),
    ]

  # The shadow falls up to 3.5x farther while the packet is tossed (lifted).
  shadow_reach = f"{m}.ShadowDistance * (1 + 2.5 * ({toss}))"
  nodes = [
      # Pass-through entry points for MainInput1 (outgoing) and MainInput2
      # (incoming); BrightnessContrast with no inputs set is an identity op.
      _node("OutgoingInput", "BrightnessContrast", [], (0, 49.5)),
      _node("IncomingInput", "BrightnessContrast", [], (0, 148.5)),
      _node(
          "ActiveClip",
          "Dissolve",
          [
              _expr("Mix", "0", outgoing_active),
              _link("Background", "IncomingInput"),
              _link("Foreground", "OutgoingInput"),
          ],
          (110, 99),
          preamble=_DISSOLVE,
      ),
      _node(
          "CreaseNoise",
          "FastNoise",
          [
              _value("UseFrameFormatSettings", "1"),
              _value("Detail", "3.0"),
              _value("Contrast", "2.6"),
              _value("Discontinuous", "1"),
              _value("XScale", "3.5"),
              _value("Color1Red", "0.78"),
              _value("Color1Green", "0.76"),
              _value("Color1Blue", "0.72"),
              _value("Color2Red", "1.0"),
              _value("Color2Green", "1.0"),
              _value("Color2Blue", "1.0"),
          ],
          (110, 214.5),
      ),
      _node(
          "ActiveCreased",
          "Merge",
          [
              _value("ApplyMode", 'FuID { "Multiply" }'),
              _value("Operator", 'FuID { "Atop" }'),
              _expr(
                  "Blend",
                  "0.0",
                  f"{m}.CreaseStrength * 0.45 * ({settle})",
              ),
              _link("Background", "ActiveClip"),
              _link("Foreground", "CreaseNoise"),
          ],
          (220, 99),
      ),
      # Mirroring flips the layout while the print keeps its orientation.
      _node(
          "MirrorIn",
          "Transform",
          [
              _expr("FlipHoriz", "0", f"iif({m}.MirrorH > 0.5, 1, 0)"),
              _link("Input", "ActiveCreased"),
          ],
          (330, 99),
      ),
      _node(
          "TransparentCanvas",
          "Background",
          [
              _value("UseFrameFormatSettings", "1"),
              _value("TopLeftAlpha", "0"),
          ],
          (330, 0),
      ),
      _node(
          "PaperBack",
          "Background",
          [
              _value("UseFrameFormatSettings", "1"),
              _expr("TopLeftRed", "0.965", f"{m}.PaperRed * 0.93"),
              _expr("TopLeftGreen", "0.945", f"{m}.PaperGreen * 0.92"),
              _expr("TopLeftBlue", "0.895", f"{m}.PaperBlue * 0.88"),
              _value("TopLeftAlpha", "1"),
          ],
          (550, 280),
      ),
      panel_mask("LeftPanelMask", third / 2, 16.5),
      panel_mask("MiddlePanelMask", 0.5, 82.5),
      panel_mask("RightPanelMask", 1 - third / 2, 148.5),
      masked_panel("LeftFront", "LeftPanelMask", 16.5),
      masked_panel("MiddleFront", "MiddlePanelMask", 82.5),
      masked_panel("RightFront", "RightPanelMask", 148.5),
      panel_back("LeftBack", "LeftFront", 16.5),
      panel_back("RightBack", "RightFront", 148.5),
      *flap_chain("Left", angle_left, third / 2, 16.5),
      *flap_chain("Right", angle_right, -third / 2, 148.5),
      # The left panel was folded last, so it stays on top.
      _node(
          "SheetRight",
          "Merge",
          [
              _link("Background", "MiddleFront"),
              _link("Foreground", "RightHingeOut"),
          ],
          (1210, 115.5),
      ),
      _node(
          "SheetFolded",
          "Merge",
          [
              _link("Background", "SheetRight"),
              _link("Foreground", "LeftHingeOut"),
          ],
          (1320, 82.5),
      ),
      _node(
          "MirrorOut",
          "Transform",
          [
              _expr("FlipHoriz", "0", f"iif({m}.MirrorH > 0.5, 1, 0)"),
              _link("Input", "SheetFolded"),
          ],
          (1430, 82.5),
      ),
      _node(
          "PacketMotion",
          "Transform",
          [
              _expr(
                  "Center",
                  "{ 0.5, 0.5 }",
                  f"Point(0.5 + ({mirror_sign}) * {m}.SlideDistance * ({toss})"
                  f" * cos({m}.SlideAngle * pi / 180), 0.5 + {m}.SlideDistance"
                  f" * ({toss}) * sin({m}.SlideAngle * pi / 180))",
              ),
              _expr("Size", "1.0", f"1 + 0.12 * ({toss})"),
              _expr("Angle", "0.0", f"({mirror_sign}) * 24 * ({toss})"),
              _link("Input", "MirrorOut"),
          ],
          (1540, 82.5),
      ),
      _node(
          "PacketShadow",
          "Shadow",
          [
              _expr(
                  "ShadowOffset",
                  "{ 0.5, 0.5 }",
                  f"Point(0.5 + {shadow_reach} * cos({m}.ShadowAngle * pi /"
                  f" 180), 0.5 + {shadow_reach} * sin({m}.ShadowAngle * pi /"
                  " 180))",
              ),
              _expr(
                  "Softness",
                  "0.02",
                  f"{m}.ShadowSoftness * (1 + 1.5 * ({toss}))",
              ),
              _expr("Alpha", "0.62", f"{m}.ShadowStrength * ({settle})"),
              _link("Input", "PacketMotion"),
          ],
          (1650, 82.5),
      ),
      # Per-step hand wobble: 12.9898 / 78.233 / 43.17 are arbitrary
      # "hash" frequencies (as in the classic GLSL rand) that make each held
      # step land on an unrelated offset; amplitude 0.006 frame, 2.2 degrees.
      _node(
          "HandJitter",
          "Transform",
          [
              _expr(
                  "Center",
                  "{ 0.5, 0.5 }",
                  f"Point(0.5 + {m}.JitterAmount * 0.006 * ({settle}) *"
                  f" sin({step_index} * 12.9898), 0.5 + {m}.JitterAmount *"
                  f" 0.006 * ({settle}) * cos({step_index} * 78.233))",
              ),
              _expr(
                  "Angle",
                  "0.0",
                  f"{m}.JitterAmount * 2.2 * ({settle}) * sin({step_index} *"
                  " 43.17)",
              ),
              _link("Input", "PacketShadow"),
          ],
          (1760, 82.5),
      ),
      # Clip revealed under the moving sheet: the incoming clip when folding
      # out (FoldAction 1), the outgoing clip when unfolding in (0), and
      # nothing (TransparentCanvas) for fold-out-then-in (2).
      _node(
          "Underlay",
          "Dissolve",
          [
              _expr(
                  "Mix",
                  "0",
                  f"iif({m}.FoldAction > 0.5 and {m}.FoldAction < 1.5, 1, 0)",
              ),
              _link("Background", "OutgoingInput"),
              _link("Foreground", "IncomingInput"),
          ],
          (1650, 214.5),
          preamble=_DISSOLVE,
      ),
      _node(
          "UnderlayGate",
          "Dissolve",
          [
              _expr("Mix", "0", f"iif({m}.FoldAction > 1.5, 1, 0)"),
              _link("Background", "Underlay"),
              _link("Foreground", "TransparentCanvas"),
          ],
          (1760, 214.5),
          preamble=_DISSOLVE,
      ),
      _node(
          "CompositeTransition",
          "Merge",
          [
              _link("Background", "UnderlayGate"),
              _link("Foreground", "HandJitter"),
          ],
          (1870, 148.5),
      ),
      # Frame-exact boundaries: the last frame is the clean incoming clip and
      # the first frame the clean outgoing clip, so cuts into and out of the
      # transition never show a partially folded sheet.
      _node(
          "ExactEndSwitch",
          "Dissolve",
          [
              _expr("Mix", "0", "iif(time >= comp.RenderEnd, 1, 0)"),
              _link("Background", "CompositeTransition"),
              _link("Foreground", "IncomingInput"),
          ],
          (1980, 148.5),
          preamble=_DISSOLVE,
      ),
      _node(
          "ExactBoundaryClamp",
          "Dissolve",
          [
              _expr("Mix", "0", "iif(time <= comp.RenderStart, 1, 0)"),
              _link("Background", "ExactEndSwitch"),
              _link("Foreground", "OutgoingInput"),
          ],
          (2090, 148.5),
          preamble=_DISSOLVE,
      ),
  ]
  tools = "\n".join(nodes)

  return f"""{{
\tTools = ordered() {{
\t\t{m} = MacroOperator {{
\t\t\tCtrlWZoom = false,
\t\t\tCustomData = {{
\t\t\t\tHelpPage = "MTTN PaperCraft Studio Paper Fold & Unfold",
\t\t\t}},
\t\t\tInputs = ordered() {{
\t\t\t\tMainInput1 = InstanceInput {{
\t\t\t\t\tSourceOp = "OutgoingInput",
\t\t\t\t\tSource = "Input",
\t\t\t\t}},
\t\t\t\tMainInput2 = InstanceInput {{
\t\t\t\t\tSourceOp = "IncomingInput",
\t\t\t\t\tSource = "Input",
\t\t\t\t}},
{group_defaults}
\t\t\t}},
\t\t\tOutputs = {{
\t\t\t\tMainOutput1 = InstanceOutput {{
\t\t\t\t\tSourceOp = "ExactBoundaryClamp",
\t\t\t\t\tSource = "Output",
\t\t\t\t}},
\t\t\t}},
\t\t\tViewInfo = OperatorInfo {{ Pos = {{ 0, 0 }} }},
\t\t\tTools = ordered() {{
{tools}
\t\t\t}},
{user_controls}
\t\t}},
\t}},
\tActiveTool = "{m}",
}}
"""

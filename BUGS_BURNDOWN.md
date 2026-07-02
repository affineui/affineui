# Decius Game Editor Bugs Burndown

Last manual result: after relinking `decius_game_editor.exe` on 2026-06-19, the listed bugs still showed no visible improvement in the interactive demo.

Current automated result: after relinking and rerunning on 2026-06-23, `affineui_tests.exe --no-colors=true` passes 417/417 test cases with 4089/4089 assertions. The checkboxes below remain open until the rebuilt interactive demo is visually confirmed.

Use this file as the working burndown for `examples/11_decius_game_editor`. Do not check an item off from an automated test alone; check it off after the interactive demo behavior matches the target behavior.

Interactive demo:

```powershell
cd C:\Users\benjcooley\projects\affineui\affineui\build\ninja\examples\decius_game_editor
.\decius_game_editor.exe
```

Interactive synthetic labs:

```powershell
cd C:\Users\benjcooley\projects\affineui\affineui\build\ninja\examples\decius_game_editor
.\decius_game_editor.exe --lab-help
.\decius_game_editor.exe --lab=checkbox
.\decius_game_editor.exe --lab=dropdown
.\decius_game_editor.exe --lab=color
.\decius_game_editor.exe --lab=color-direct
.\decius_game_editor.exe --lab=color-scrolled
.\decius_game_editor.exe --lab=vector
.\decius_game_editor.exe --lab=tree
.\decius_game_editor.exe --lab=tearout
```

Scoped game-editor integration shells:

```powershell
cd C:\Users\benjcooley\projects\affineui\affineui\build\ninja\examples\decius_game_editor
.\decius_game_editor.exe --game-scope=inspector
.\decius_game_editor.exe --game-scope=tree
.\decius_game_editor.exe --game-scope=tearout
```

Automated regression executable:

```powershell
cd C:\Users\benjcooley\projects\affineui\affineui
build\ninja\tests\affineui_tests.exe --no-colors=true
```

## Open Bugs

- [ ] Color picker cannot be manipulated.

  Observed: the color dropdown opens, but clicking anywhere on the picker dismisses it before a new color can be chosen.

  Also observed: the dropdown shell is full width, but the color selector contents do not reliably expand to fill the full widget width.

  Expected: the Decius colorfield picker should follow the web behavior: the popover remains open while interacting with the SV square, hue bar, preview hex, and picker body; dragging/clicking the SV square or hue bar updates the color and emits the widget change.

  Areas to inspect: native transient layer closing, popover hit testing, fixed-position overlay hit testing, `dcs-colorfield` picker geometry, and the C++ colorfield structure versus the unminified `../decius-css` JavaScript/CSS contract.

  Done means: in test 11, Tint opens a full-width picker, SV/hue controls fill the picker body, and mouse interaction changes the color without closing the popover prematurely.

  Automated coverage: `App Decius colorfield picker survives model-backed rebuilds`, `App dispatch invokes Decius colorfield picker callbacks`, and scrolled-panel anchoring tests.

- [ ] Vector editor compression and spacing are still wrong.

  Observed: when compressed, the vector editor draws vertically but the containing foldout/field does not expand, so `X/Y/Z` overlap the following foldout. When horizontal, the `X/Y/Z` value editors still have no visible gap between them.

  Expected: match Decius web behavior. The vector row should switch from horizontal to vertical when the available width is below `n * minWidth + (n - 1) * gap`. When stacked, the `.dcs-field` and enclosing foldout must expand to contain all rows. When horizontal, channels must preserve the Decius gap between widgets.

  Areas to inspect: field/foldout height propagation after `dcs-vec--stacked`, support for the Decius `:has(> .dcs-vec.dcs-vec--stacked)` CSS behavior, gap resolution, Yoga/flex intrinsic height after runtime class mutation, and whether the game editor is loading the expected native CSS.

  Done means: in test 11, narrowing the inspector stacks `X/Y/Z`, the Transform foldout grows, the Display foldout is pushed down, and wide layout shows visible spacing between `X/Y/Z`.

  Automated coverage: `App Decius vector editors keep the default horizontal gutter` and `App Decius vector editors expand their foldout field when stacked`.

- [ ] Tree drag/drop feedback is unreliable or absent.

  Observed: dragging tree elements sometimes shows no drop target feedback. Recent attempts made this visually worse, with no feedback in cases that previously showed some indication.

  Expected: while dragging a draggable tree row, pointer movement should reliably update the drop target. The target should visibly show before/after/into feedback as appropriate, and dropping should reorder consistently.

  Areas to inspect: mouse move delivery during tree drag, event capture/consumption by child widgets, hit testing through tree row children, drop target invalidation, and native painting for all drop zones.

  Done means: in test 11, dragging tree rows continuously shows stable feedback under the pointer and the final drop performs the expected reorder.

  Automated coverage: `UiControls script reorders Decius tree rows by drag/drop`, including the guard that pressing a draggable row does not emit a selection change before drag feedback can start.

- [ ] Single-view tearout title still looks like a tab.

  Observed: a tearout panel with one view still renders a selected-tab style, including tab-like highlight/color-bar flair.

  Expected: single-view floating panels use the Decius "NO Tab" tearout title style: just icon/title space with no selected tab highlight, no color bar, and no tab flair. Dragging the title text area should still initiate dock drag/redocking. Dragging empty titlebar space should still move the floating panel.

  Areas to inspect: `emit_one_floating_panel`, raw dock surgery for title-only floats, canonical `dcs-dockpane__titlebar` / `dcs-panel__title--dock-tab` title-only markup, selected tab pseudo-elements, and CSS specificity/order against the Decius bundle.

  Done means: tearing out Console as a single floating panel shows plain title styling, title text still redocks, and empty titlebar chrome still moves the panel.

  Automated coverage: `UiControls: View panel tearoff uses a default size inside the workspace float host`, plus floating-chrome move and title-tab redock regressions.

- [ ] Cast Shadows checkbox requires two clicks.

  Observed: `Cast shadows` in the Material inspector requires two clicks to change checked state. The `Visible` checkbox below it changes on one click.

  Expected: `Cast shadows` changes on the first click, exactly like `Visible`.

  Important clue: this is probably not the checkbox widget itself. `Cast shadows` is document/model/command backed, while `Visible` is local inspector state. The bug is likely in the inspector handler, command/reload path, or model update timing.

  Areas to inspect: `GameEditor::set_cast_shadows`, `SetProperty`, `Context::run`, command-stack notifications, document dirty/change notifications, and whether a rebuild is replaying stale state over the optimistic DOM toggle.

  Done means: in test 11, the first click on `Cast shadows` immediately toggles the check and remains correct after the inspector rebuilds.

  Automated coverage: `App Decius checkbox survives command-stack rebuilds on first click`.

## Process Notes

- Keep the C++ behavior aligned with the unminified web implementation in `../decius-css` whenever possible.
- Add or strengthen automated tests for each bug, but do not treat those tests as sufficient until the interactive demo is verified.
- Every synthetic regression test must have a launchable interactive twin. The
  lab can be a friendly primary scenario such as `--lab=tree`, but `--lab-help`
  must also list the regression-style alias, such as `--lab=tree-drag-drop`,
  so automated and human verification are paired by name.
- Rebuild/relink the binary before manual verification:

```powershell
cd C:\Users\benjcooley\projects\affineui\affineui
cmd.exe /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && ninja -C build\ninja decius_game_editor'
```

- Use the interactive labs as the first signal check. If a lab passes but its
  scoped game-editor shell fails, keep stripping or isolating that scoped shell
  until it matches the lab. If the scoped shell passes but the full demo fails,
  the remaining issue is integration interference from another panel, overlay,
  or layout participant.

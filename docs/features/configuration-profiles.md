# Developer panel and configuration profiles

The F12 developer panel has **Rendering**, **Lighting**, **Post-processing**,
**Diagnostics**, and **Profiles** tabs. Each tab scrolls independently. The active
profile selector, unsaved indicator, **Save**, and **Revert** stay in the footer.
Drag the panel's inner edge to resize it; **Dock left/right** switches edges.
Panel side, width, and selected tab are remembered separately from rendering
settings. Startup leaves the panel hidden. The host retains ownership of the
F12 shortcut through the existing input and UI-state API.

Controls edit requested settings. The panel also reports the active backend and
feature availability. Device limitations and reconstruction fallbacks do not
change the saved preferences. Changes apply before preparation of the next frame;
backend switches and presentation changes use the existing synchronization paths.

Profiles omitting `rtx.native.pathtrace.translucencyDenoiser` enable surface
denoising by default; `rtx.native.pathtrace.translucencyReuse` is opt-in. These Boolean
settings control [translucent surface filtering and lighting reuse](water-indirect.md).

## Storage and workflow

Profiles live in `<working-directory>/openremix/profiles/`. Set `REMIX_CONFIG_DIR`
to replace the containing `openremix` directory, for example
`REMIX_CONFIG_DIR=/writable/game-settings` uses `/writable/game-settings/profiles/`.
This permits separate configurations for each game instance. The adjacent
`state.conf` contains the last selected profile and panel layout.

With no saved selection, the runtime uses an in-memory **Default** profile.
**Save** writes its complete current settings. **New** asks for a unique name and
starts an unsaved profile from built-in defaults. **Duplicate** saves all current
values, including unsaved edits, into a new named profile and activates it; the
source file stays unchanged. Names may contain spaces and Unicode but cannot
contain path components or characters reserved by Windows file names.

**Load** and the footer selector replace the entire snapshot. Omitted values take
built-in defaults, so settings never leak from the previous profile. **Revert**
restores the snapshot last loaded or saved in this session. Loading, creating, or
reverting while dirty offers **Save / Discard / Cancel**. Closing the panel keeps
edits in memory. Exiting never implicitly saves rendering settings.

**Refresh** lists `.conf` files directly inside the profile folder. Subdirectories
and symlinks are excluded. Files added through a file manager are discovered on
refresh. Loading/saving errors appear in the panel and leave the current settings
or previous file intact. Unknown keys produce warnings and are retained on saves.
Writes use an exclusive temporary file in the same folder followed by atomic
replacement; a failed write leaves the profile dirty.

The last successfully loaded, saved, or duplicated profile is restored before
`Startup` returns. Configuration API calls made after startup override those
values, preserving launcher precedence. Later UI edits or manual loads can replace
those overrides. Missing or invalid startup profiles fall back to built-in
defaults with a nonfatal log and panel diagnostic.

## File format

Files use UTF-8 `key = value` lines, with a mandatory version:

```conf
# Comments occupy their own line; semicolon comments are also accepted.
profile.version = 1
rtx.native.backend = pathtrace
rtx.native.vsync = true
rtx.native.pathtrace.samples = 2
rtx.native.pathtrace.upscaler = fsr31
rtx.native.pathtrace.fsrQuality = quality
rtx.native.pathtrace.overrideLight = 1
rtx.native.pathtrace.lightDirection.x = -0.4
rtx.native.pathtrace.lightDirection.y = -0.8
rtx.native.pathtrace.lightDirection.z = -0.3
rtx.native.pathtrace.lightColor.r = 1
rtx.native.pathtrace.lightColor.g = 1
rtx.native.pathtrace.lightColor.b = 1
rtx.native.pathtrace.lightIntensity = 3.14159265
rtx.native.clearColor.r = 0.025
rtx.native.clearColor.g = 0.035
rtx.native.clearColor.b = 0.055
rtx.native.clearColor.a = 1
```

Saves write every setting using the existing `rtx.native.*` names and accepted API
values. The new daylight keys above and `rtx.native.clearColor.{r,g,b,a}` are also
accepted by `SetConfigVariable`. Direction components range from -1 to 1; color
components range from 0 to 1; daylight intensity ranges from 0 to 20. Other ranges
and enum names retain the existing API semantics. Saved floats use locale-independent
round-trip precision. Duplicate keys, invalid known values, and unsupported versions
reject the whole file before applying any values.

Editable diagnostics are included. Cache-reset actions, runtime counters, the
text-input demonstration field, and panel layout are excluded. A file containing
`rtx.native.pathtrace.sharcReset` emits a warning and never replays or saves that
action. Editing a file on disk does not change a running session until it is loaded.

Resource-changing edits are requested immediately and activate after background
pipeline preparation succeeds. Saving always saves requested values. Further edits
supersede pending requests; a failed replacement leaves the active configuration
working and shows its failure in F12 Diagnostics. Ordinary lighting/display values
that do not change resource contracts retain the current renderer.

The preparation panel reports completed pipeline jobs and jobs discovered so far,
without estimating time remaining. Diagnostics includes device-wide compilation
counts/timing, in-memory reuse, optional driver cache feedback and cache-write
failures. Loading and DLSS preparation keep presentation, input and F12 responsive.
Preparation reuses the last rendered game image behind live controls, including
across resize and VSync changes. Before the first game frame exists, the loading
panel uses the normal background.
Previously unseen raster fixed-function states also prepare before drawing a frame,
so a frame never omits geometry to work around an unavailable pipeline.

Path-traced automatic exposure and bloom now default on when a profile omits their keys.
See [exposure and bloom](exposure-and-bloom.md) for defaults, ranges and preparation behavior.

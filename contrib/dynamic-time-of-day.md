# Dynamic time of day (research)

Goal: a time of day that moves continuously, so day and night never swap in one frame, with weather
laid on top. This page records what is known, what is not, and the probe that finds the rest.

Code: [environment_probe.cpp](../Extension/World/environment_probe.cpp) (console `envprobe`).

## What is known

- **Times of day are world layers.** Each map has seven, `<map>_tod_<n>_<name>` (morning, noon,
  afternoon, evening, night, weatherday, weathernight). `tod` forces one on and the others off
  ([console_commands.cpp](../Extension/World/console_commands.cpp)). Switching is a hard swap:
  everything the layer carries changes at once.
- **Each layer brings visual environments** (`Lighting/VE/TOD/BAM/ve_bam_*` and so on), which
  ReSkate observes ([visual_environment.cpp](../Extension/World/visual_environment.cpp)). Their
  fog, sky and wind components are bound: 214 fields can be written live through each component's
  own property setter, by field hash
  ([atmosphere.h](../Engine/Game/Build/20260929/atmosphere.h),
  [local_atmosphere_controls.cpp](../Extension/World/local_atmosphere_controls.cpp)). Numbers
  written that way can be eased over time, so fog and clouds can already blend.
- **A property id is the djb2-xor hash of the field's name**: `h = 5381; h = h*33 ^ c`. Checked
  against `Enable`, `FogDistanceMultiplier`, `Start` and `End` (a `static_assert` in the probe).
  So any field's id can be computed from its name.
- **Not bound yet: the sun.** Its direction, colour and intensity are expected on the visual
  environment's outdoor-light component, which ReSkate has not mapped. The sky's
  `Light1RotX/RotY` exist, but by default the sky follows the outdoor light
  (`Sky.Light1FollowOutdoorLight`).
- **Unknown: whether two visual environments blend.** If the visual environment carries a
  visibility or priority weight that the engine mixes by, two time-of-day presets can crossfade.
  ReSkate has never written one.

## The probe

Game thread only, from the console (`~`). It only reads, except `poke`.

- `envprobe dump` lists every visual environment of the loaded level and writes
  `environment-probe.txt` in ReSkate's content cache folder (the command prints the path). For
  each visual environment:
  - its data object's type and every reflected field: name, type, offset, the field record's hash
    (flagged if it is not the name hash) and the authored value;
  - every component found beside its fog, sky and wind (in the collection that owns them, or one
    pointer away): type name, vtable, the setter slot (vtable + 0x18) and data type. Components
    ReSkate already binds are listed on one line; any other is listed in full;
  - for each distinctive authored float, `live` lists where the same bits sit in the live
    component or entity. That is where the running game reads it: the cache offset a binding needs.
- `envprobe poke <component type> <field> <numbers...>` writes one field on every matching
  component of the selected map's visual environments, through that component's setter (as the fog
  bindings do), and reports which live bytes changed. `default` instead of the numbers writes the
  authored value back. The type is matched as a substring, ignoring case (`outdoorlight`).

**`poke` is an experiment.** For a component type ReSkate has not verified, the setter slot is
assumed to be the same as on fog, sky and wind. A fault is caught and reported, but a wrong
function could still damage game state. Use it in free skate, not in a session, and restart the
game afterwards.

## The runs to do

1. On BAM, `tod morning`, wait for the level to settle, then `envprobe dump`. Keep the report.
2. `tod evening` (then `tod night`), dump again after each. Comparing the reports shows which
   fields change between presets: those are what a clock has to drive.
3. In the report, find the outdoor-light component (expected: `OutdoorLightComponentData`, with
   fields like `SunRotationX`, `SunRotationY`, `SunColor`, `SkyColor`, `GroundColor`). Then:
   `envprobe poke outdoorlight SunRotationY 45`. Does the sun (shadows, sky) move? Note the
   changed bytes. Then `envprobe poke outdoorlight SunRotationY default`.
4. In the visual environment's own fields, look for `Visibility` or `Priority`, and their `live`
   offsets on the entity.
5. Send the reports, the poke output and what was seen on screen.

## What the answers decide

- **The sun moves through its setter.** Bind the outdoor light the way fog, sky and wind are bound
  (hashes, cache offsets and override flags in a `Build/20260929` header), then add a clock that
  eases sun angle, light colours, sky and fog between the authored presets' values. The world layer
  is still switched at the preset boundaries; with the sun and sky already at the next preset's
  values, the switch is small.
- **Visual environments have a weight the engine blends by.** Load the next preset's layer before
  the boundary and ramp the weights: a real crossfade. Baked parts (reflection captures, emissive
  street lights) may still change at one moment.
- **Neither.** Hide the switch: make it under heavy fog or a short fade.

Weather goes on top either way: fog, cloud and wind offsets from the current preset, blended as
numbers. Cloud textures cannot blend; fade the layer out, swap it, fade it back in.

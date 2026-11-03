# PresetMaker

Makes presets for a VST3 instrument by listening. Give it a target sound (a General MIDI instrument
from Windows' built-in `gm.dls`, or a folder of your own sampled notes) and it searches the plugin's
parameters until the plugin sounds as close to the target as it can, then writes the result as a
`.vstpreset` where your DAW's preset browser will find it.

It is best effort by design: an additive string synth will never be a distortion guitar, but it
will find the nearest thing it *can* do, and tell you how close it got.

<img src="examples/FretPlucked/grand_piano_spectrogram.png" width="720"/>

*gm.dls Acoustic Grand Piano (top) and the fitted FretPlucked preset (bottom) at C3, C4, C5.*

## Quick start

Download `PresetMaker-windows.zip` from Releases (or build, below). Then, from a command prompt:

```
pmcli gm-list
```
lists the 128 GM programs in `C:\Windows\System32\drivers\gm.dls` (0 = Acoustic Grand Piano, 24 =
Nylon Guitar, 40 = Violin ...).

```
pmcli vst-params "C:\Program Files\Common Files\VST3\MySynth.vst3"
```
loads the plugin and lists its parameters, which ones the search will move, and where its presets go.

```
pmcli fit "C:\Program Files\Common Files\VST3\MySynth.vst3" 24 out\nylon --name "Nylon Guitar" --check
```
fits the plugin to GM 24 at C3, C4 and C5, prints progress, then:

- writes `out\nylon\preset.txt` (every parameter's normalised value) and `ref_*.wav` / `fit_*.wav`
  so you can listen to target and result side by side;
- writes `Documents\VST3 Presets\<Vendor>\<Plugin>\Nylon Guitar.vstpreset`, the standard location
  DAWs scan (Cubase, REAPER, Studio One, Bitwig, FL Studio, Live 12 ...), so the preset appears in
  the plugin's preset list next time you open it;
- `--check` plays notes the fit never saw (G2, G3, G4, G5, and C4 soft and loud) and reports how
  well the preset holds up away from the fitted notes.

A fit is 1200 renders by default; with a plugin that renders in real time that is 15–40 minutes.
Several fits run happily in parallel:

```
pmcli fit-batch MySynth.vst3 examples\FretPlucked\gm_strings.txt out --jobs 8
```
takes a text file of `reference<TAB>Preset Name` lines and runs them `--jobs` at a time, each with
its own `fit.log`.

Or use the GUI, `PresetMaker.exe`: pick the plugin, the target, a name, press Fit.

## Reference sounds

`<reference>` is one of:

| | |
|---|---|
| `24` or `gm:24` | GM program 24 from gm.dls (Windows only) |
| `C:\samples\piano\` | a folder of WAVs named `<key>.wav` or `<key>_<velocity>.wav` (MIDI key numbers, e.g. `60_100.wav`); the nearest sample is resampled to pitch like a sampler |
| `note.wav` | one WAV used for every note (resampled) |

The target can be anything you can record: a real instrument, a hardware synth, another plugin.

## Options

| | default | |
|---|---|---|
| `--evals N` | 1200 | renders per fit; the search stops here |
| `--keys 48,60,72` `--vel 100` | | the notes fitted (all keys at once; a preset must serve all of them) |
| `--hold 1.5` `--tail 0.5` | | note length and release tail, seconds |
| `--fix "Volume=0.3;Pan=0.5"` | | parameters held at a normalised value and left out of the search |
| `--skip "LFO;Reverb"` | MIDI CC etc. | parameters whose title contains the text are left out, at their plugin value |
| `--start preset.txt` `--sigma 0.05` | | continue from an earlier result with a small step instead of starting cold |
| `--preroll 0.25` | 0.15 | seconds of silence rendered before each note. Raise it if the plugin builds tables in the background and the result sounds like the *previous* parameters |
| `--name "X"` `--preset-dir D` `--no-vstpreset` | | the `.vstpreset`: its name, somewhere other than the standard folder, or none |
| `--check` | | held-out test after the fit |
| `--delta 0.3` | | weight of the spectral-change term in the score (see below) |

Other commands: `gm-render <ref> <key> <vel> out.wav`, `vst-render <plugin> <key> <vel> out.wav
["Title=0.5" ...]`, `score ref.wav cand.wav`, `check <plugin> <ref> preset.txt`, `export <plugin>
preset.txt "Name"` (a `.vstpreset` from a `preset.txt`).

## Things worth knowing

- **Fix the volume.** A free gain parameter lets the search trade loudness for everything else.
  `vst-params` shows each parameter's current value; `--fix "Volume=0.3"` holds it there.
- **Listen to `fit_60.wav` against `ref_60.wav`**, and check the pitch. The score compares
  loudness in auditory bands, which hears timbre and envelope well but is nearly deaf to
  harmonicity: a plugin whose partials are stretched can score well and sound wrong. If the fit
  keeps pushing a tuning or spacing parameter away from neutral, fix it with `--fix`.
- **Expect a ceiling.** A similarity around 35% on the held-out notes is a good fit for a sound
  the plugin can genuinely make; under 20% means the target is outside what it can do (distorted,
  bowed, or an attack it has no model for), and no budget will change that.
- **Cold then warm.** A first fit samples 10% of its budget at random to find the right region,
  then refines with CMA-ES. To polish, run again with `--start out\x\preset.txt --evals 400`.
- Parameters the plugin marks non-automatable, hidden or read-only are never touched, nor are
  MIDI controller parameters (SynthEdit exposes 178 of them; sending them 0 bends the pitch).

## How the score works

Each render and its target are analysed in 40 ERB-spaced auditory bands at two time resolutions,
converted to loudness with a threshold-in-quiet and a compressive (Zwicker) law so that quiet
detail counts for less than loud detail, and compared on: the fine time-frequency surface, the
tonal (long-window) spectrum, the per-band envelope, and the per-band *change* over 50 ms (which
is what makes the fit care about how partials decay, not just how loud they are). The first 60 ms
after note-on are weighted up. The candidate's gain is fitted out, so level is never the issue.
Similarity = 100·exp(−3·distance). `pmcal` (built alongside) runs the calibration tests the
weights were chosen against: identity (same program, different velocity must rank first of 128),
family retrieval, and monotonic degradation.

The search is separable CMA-ES on the unit cube; stepped parameters are quantised.

## Building

Windows, Visual Studio 2022 or later:

```
cmake -S . -B build -DPM_GUI=ON
cmake --build build --config Release
```

The VST3 SDK is fetched from GitHub (or found in an existing `~/.cpm` cache). `-DPM_GUI=ON` also
builds `PresetMaker.exe`, a small [gmpi_ui](https://github.com/JeffMcClintock/gmpi_ui) desktop
app, fetching gmpi_ui and GMPI; leave it off for the command line alone.

Source layout: `Dls`/`GmRender` (DLS reader and offline note player), `Reference` (target
sources), `Vst3Host` (SDK hosting, offline rendering, `.vstpreset` writing), `Metrics` (the
score), `Cma.h`, `Fit` (the search; shared by `cli.cpp` and `gui/`), `calibrate.cpp` (`pmcal`).

## Example: FretPlucked

`examples/FretPlucked/` holds twenty presets fitted for SynthEdit's FretPlucked additive string
oscillator: the `gm_strings.txt` batch list, each result as `preset.txt`, and the `.vstpreset`
files. `tools/populate_presets.py` is the script that wrote them into the SynthEdit project's
preset slots; `tools/spectro.py` draws comparison spectrograms like the one above.

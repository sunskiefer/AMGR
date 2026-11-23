# AMGR

**A four-layer granular instrument for the Akai Force and MPC OS (VST2, ARMv7).** Vendor: L'Cronx. Current version: 0.2.5.

AMGR is a granular sample mangler in the spirit of the Tasty Chips GR-2 ("Paulstretch"-style time-stretching and smearing),
built as its own clean-room engine aimed at much lower resource consumption so it runs comfortably on the Force's own CPU.
Four independent layers each load their own sample; touching a point on a layer's waveform sets where its grains play.

## What it does

* **Four layers**, each with its own loaded sample (up to 5 minutes), waveform display, and grain scatter drawn as little balls over the wave.
* **Touch-to-play**: tap anywhere on a layer's waveform to move where its grains spawn from.
* **Grain controls per layer** – density, size, spread, jitter (also drives per-grain pitch/speed scatter and reverse probability), drive.
* **An ENVELOPE page** – a 4-stage ADSR per layer, with a layer selector.
* **A SAMPLES browser** per layer – browse any folder or drive (never starts at a bare filesystem root), each box remembers where it last looked.
* **A PRESETS page** – a big factory bank (every preset varies density/size/spread/pitch/level/filter/drive/jitter/reverse/delay/reverb, not just waveform position) plus unlimited user save slots.
* **A CPU-safety governor** – under load it smoothly scales grain density down, steals the nearest-to-finished grain instead of dropping new ones when a layer's grain pool is full, and cuts release-stage voices back first so sustaining/new voices keep priority. GR-2's own hardware ceilings (16 voices, 128 grains/voice, 4000 grains total) are the hard limit either way.
* **Delay and reverb** per layer, with width control.

## Install

Needs MPC OS 3.x on an armv7 Force/MPC. Download `AMGR-<version>-mpc-armv7.zip` from the Releases page, unzip it, and follow
`INSTALL.md` inside (or run `install.sh` on the device). Put your own `.wav`/`.flac` files anywhere reachable from the device
(internal card or an external drive) and load them from each layer's SAMPLES box.

## Status

0.2.5 has been built, packaged (install/uninstall scripts verified, checksums verified, ARMv7 / glibc <= 2.27) and handed to the
maintainer for testing on their own Akai Force. See `docs/CHECKLIST.md` for what has and hasn't been independently re-verified
on hardware since the last round of fixes (text rendering, the SAMPLES browser's start folder, the data wheel's reverse
direction on open lists, and page content "baking" onto other pages).

## Roadmap

A parameter-by-parameter study comparing AMGR's current engine against a real GR-2 preset's full feature set is in
`docs/GR2_parameter_study.md`. The three largest gaps identified there — a full 4-envelope-per-layer modulation system,
a proper modulation matrix (LFOs, envelopes, random, macros), and a much larger FX chain (second delay, second reverb,
chorus/flanger/ringmod/vibrato/saturator/overdrive/bitcrusher/EQ3/multiband resonator) — are the planned next steps, each to
be speced out and built one at a time.

## License

PolyForm Noncommercial 1.0.0, see [LICENSE](LICENSE). The source is public and free to use, change and share for any
noncommercial purpose; selling AMGR or using it commercially is not permitted.

Bundled third-party code and art keep their own original licenses: the Titillium Web font (`Fonts/`) is under the SIL Open
Font License 1.1 (`Fonts/OFL.txt`); knob art is derived from the VARIANT skin's own pixels.

## Support

Free plugin. Tips: ko-fi.com/lcronx

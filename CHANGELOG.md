# Changelog

## 0.2.5
* Factory presets rewritten to vary nearly every sound parameter per preset (pitch, level, size, density, spread, envelope,
  filter, drive, shape, jitter, reverse, delay, reverb, width), deterministically, with safety clamps so nothing is harmfully
  loud or silent (levels 0.45–0.85, drive <= 0.45, delay feedback <= 0.7, delay/reverb mix <= 0.5, master volume fixed).
* Grains now travel in both directions and at varying speeds by default: per-grain pitch/speed scatter tied to JITTER, plus a
  baseline amount of jitter and reverse probability built into every factory preset.
* CPU-safety scheme (replaces raising/lowering the hardware voice and grain ceilings): a load governor that scales grain
  density down smoothly under measured render load and recovers slowly once load drops; grain-stealing (steals the
  nearest-to-finished grain instead of dropping a new one when a layer's grain pool is full); release-stage voices are capped
  to a quarter of the per-voice grain budget under pressure so sustaining/new voices keep priority. GR-2's own hardware
  numbers (16 voices, 128 grains/voice, 4000 total) stay as the hard ceiling.
* Knob art brightened slightly (a soft top-down light gradient plus a small gamma lift) on the VARIANT-sourced knob filmstrip.

## 0.2.1–0.2.4
* Fixed: tiles/buttons rendering as text-less grey blocks (font cache size was too small for this plugin's text).
* Fixed: the SAMPLES browser could start at the filesystem root and had no sane way out of it; it now locates the plugin's
  own install folder and starts there instead of a relative path that could resolve to `/`.
* Fixed: the data wheel only scrolled sample/preset lists one direction (clockwise down, anticlockwise did nothing). A control
  not currently backing the open list is now parked at a mid-range value while the list is open, so a turn in either
  direction always has room to register; it's restored to its real value when the list closes.
* Fixed: SAMPLES/PRESETS tiles and their knobs could stay "baked" onto the screen after switching to another page; the strip
  under the waveform lanes is now cleared every frame instead of only when those two pages are showing.

## 0.2.0
* First build using knobs from the VARIANT skin art (square background removed).
* ADSR per layer (ENVELOPE page, layer selector, four knobs).
* Q-Link mapping, PRESETS/SAMPLES page layout, and panel colours per the maintainer's review of page renders.

## 0.1.0
* First working build: four-layer granular engine, waveform display, touch-to-play grain placement.

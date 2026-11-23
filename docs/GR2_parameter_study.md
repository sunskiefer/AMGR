# GR-2 parameter study — from a real factory preset

Source: `Glaring Contrast.json` (preset 007.A17), factory pack "GR-2 Factory Presets 2026",
cross-checked against the table of contents of `GR-2_Manual_1.8.g.pdf` (Sound Engines, p.35:
Granular, Granular Slice, Sampler, Tape, Spectral). This file is a per-layer preset: the GR-2
has four of these layers, plus a project/song level on top (tempo, multitimbral stacks).

This is for study only — not copied into AMGR, no samples included. Where a name's meaning is
guessed from context rather than confirmed in the manual, it's marked "(guess)".

Column "AMGR" says what AMGR 0.2.x already has for that parameter, or "—" if nothing yet.

## 1. Engine selector
| GR-2 key | What it is | AMGR |
|---|---|---|
| `splmode` | Which of the 5 engines this layer uses: `Granular`, `GranularSlice` (guess), `Sampler`, `Tape`, `Spectral` | — (AMGR is granular-only) |

## 2. Per-stage envelopes (four of them: amp, filter, pitch, aux)
Each has an **ADSR struct** (`a`/`d`/`s`/`r`/`i`/`ac`/`dc`/`rc`) *and* separate flat fields
(`attack_time` etc) that look like duplicates of the same four numbers in a different place —
likely one is the live/UI value and the other the stored curve data, or a legacy vs current field.
`i` is probably "invert" or "initial level"; `ac`/`dc`/`rc` are curve shape (0.5 = linear) for
attack/decay/release, matching the GR-1 manual's curved-segment envelopes.

| GR-2 key | What it is | AMGR |
|---|---|---|
| `amp_adsr.a/d/s/r` | Amplitude envelope stage times/level | AMGR has `atk/dec/sus/rel` per layer — same 4 stages |
| `amp_adsr.ac/dc/rc` | Curve shape per stage (0=log, 0.5=linear, 1=exp, guess) | — (AMGR stages are fixed-curve) |
| `amp_adsr.i` | Unclear — invert or init value (guess) | — |
| `filt_adsr.*` | A **second** envelope, dedicated to the filter cutoff | — (AMGR has no dedicated filter envelope) |
| `pitch_adsr.*` | A **third** envelope, dedicated to pitch | — (AMGR pitch is static per preset) |
| `aux_adsr.*` | A **fourth**, general-purpose envelope for modulation | — |
| `amp_env_inverted` / `filt_env_inverted` / `pitch_env_inverted` / `aux_env_inverted` | Flip each envelope's polarity | — |
| `amp_env_amt` / `pitch_env_amt` / `filt_env_amt` / `aux_env_amt` | How much each envelope affects its destination (0 = off) | — |

**Study point:** the GR-2 has 4 full ADSRs per layer; AMGR has 1. This is the single biggest
sound-design gap.

## 3. Granular engine core
| GR-2 key | What it is | AMGR |
|---|---|---|
| `density_base` | Grain density (grains/sec), 0–1 normalized | `dens1..4` — equivalent |
| `grain_size_base` | Grain length, 0–1 normalized | `size1..4` — equivalent |
| `fwdrev` | Playback direction/speed, -1 (full reverse) .. +1 (full forward) | AMGR has `rev` (reverse probability) + per-grain speed jitter — different shape, same idea |
| `grain_scan` | How far the read position scans while playing (guess: like AMGR's `sprd`) | `sprd1..4` — likely equivalent |
| `spray_base` / `spray_polarity` | Position randomization amount and whether it's one- or two-sided | same role as `sprd1..4`, but GR-2 separates "scan" and "spray" into two controls — AMGR has only one |
| `panspray_base` / `panspray_dist_base` | Per-grain random pan spread, and its distribution shape | — (AMGR pan is static per layer, no per-grain spread) |
| `pitchspray_base` / `pitchspray_polarity` | Per-grain random pitch spread | same role as AMGR's new jitter-driven speed scatter |
| `grain_probability` | Chance each scheduled grain actually fires (0–1) | — |
| `grain_keysync` | Whether grain timing resets on each new note | — |
| `grain_clocksync` / `grain_clock_retrigger` | Lock grain spawn timing to the project tempo | — (AMGR has no tempo sync at all) |
| `spawnmode` | Grain scheduling style, e.g. `Gradual` (guess: vs. bursty) | — |
| `pileup_enabled` | Whether overlapping grains are allowed to stack freely or are thinned (guess) | AMGR's new load governor does something similar for a different reason (CPU safety, not sound design) |
| `grainstealing_threshold` | Loudness level below which a grain may be stolen for a new one | similar purpose to AMGR's new "take over the grain nearest its end" rule, but GR-2 steals by *loudness* and AMGR by *age* |
| `grain_compression` | Unclear — possibly a limiter on grain output, or on the sample data itself (guess) | — |

## 4. Sample / window
| GR-2 key | What it is | AMGR |
|---|---|---|
| `startpos` / `endpos` / `stoppos` | The sample region the grains are allowed to play within | — (AMGR plays the whole file; `pos`/`sprd` move inside it but there's no startpos/endpos clamp) |
| `sample_pos` | Current/last scan position | AMGR's `pos1..4` |
| `startmode` | What happens to position on note-on (`Start` = always from startpos, guess) | — |
| `sample_loop_mode` | `Looping` vs one-shot | — (AMGR always loops within the grain window) |
| `looped_release_enabled` | Whether looping continues into the release stage | — |
| `scan_loop_confinement` | Keeps the scan position inside the loop region | — |
| `win_sides` / `win_am` / `win_tilt` / `win_curve` / `win_type` | The grain envelope (window) shape: symmetry, amplitude mod, tilt, curve, and type (`PowAR` = power attack/release, guess) | AMGR's `shape` knob (one slider, cosine window) — GR-2 exposes far more shape control |
| `kbtune` / `cents` | Base note and fine tune for the sample | AMGR layers are untuned until a preset sets pitch |
| `sized_mode` | `Fixed` — grain size doesn't track something else (guess: vs tracking pitch) | — |

## 5. Sub-oscillator
| GR-2 key | What it is | AMGR |
|---|---|---|
| `subosc_enabled` / `subosc_freq` / `subosc_amp` / `subosc_balance` / `subosc_quant` | A plain sub-bass oscillator layered under the grains, with its own frequency, level, stereo balance, and option to quantize its pitch | — (AMGR has no oscillator, granular only) |

## 6. Keyboard tracking / velocity
| GR-2 key | What it is | AMGR |
|---|---|---|
| `kbposoff0-2` / `kbposscale0-2` | Up to 3 keyboard-tracking "zones" that offset/scale the scan position by note played | — |
| `kbpitch0-2` | Whether each zone also affects pitch | — |
| `kbsplit1` / `kbsplit2` | Split points between the zones | — |
| `velocity_curve` | Velocity response shape (`Sqrt` here) | — (AMGR uses velocity linearly) |
| `velosens` | How much velocity affects level | — |
| `max_polyphony` | Voices for *this layer* (GR-2 allows per-layer up to 16) | AMGR's 16 is global, not per-layer |
| `glide` / `glide_always_enabled` | Portamento time and whether it's always on vs only legato | — |
| `finger_voice_hold` / `mpe_pos_clip_enabled` | MPE-related: holding a voice per finger, clipping position modulation from MPE | — |

## 7. Filter
| GR-2 key | What it is | AMGR |
|---|---|---|
| `cutoff_lo` / `cutoff_hi` | Filter cutoff range/modulation span | `cut` — AMGR has one cutoff, no separate lo/hi range |
| `resonance` / `resonance_hi` | Resonance, and a second resonance for hi-range (guess: filter has a crossfading pair) | `res` |
| `filter_mode` | `Lpf` / presumably also Bpf, Hpf, Notch | `ftype` — AMGR crossfades LP→BP→HP on one knob, GR-2 picks a mode outright |
| `filter_bypassed` | On/off | — |
| `filter_slope_lo` / `filter_slope_hi` | Filter steepness (`Db12` here), independently for lo/hi | — (AMGR's filter is a fixed 12 dB/oct state-variable design) |
| `filter_routing` | Where the filter sits in the signal chain (guess) | — |
| `filter_per_slice_enabled` | Granular Slice engine: each slice can have its own filter | — |
| `slice_cutoffs_lo/hi[128]` / `slice_resonances_lo/hi[128]` | Per-slice filter settings, 128 slices | — |

## 8. Effects (all have `*_dry` / `*_wet` sends, so this layer has its own private FX chain)
| GR-2 key | What it is | AMGR |
|---|---|---|
| `chorus_*` (rate, feedback, center, bandwidth, dry, wet) | Chorus | — |
| `flanger_*` | Flanger | — |
| `ringmod_*` (am_mode, fine_width_mode, freq_coarse, freq_fine, dry, wet) | Ring modulator, with AM or ring mode and coarse/fine frequency | — |
| `vibrato_*` (rate, center, dry, wet) | Pitch vibrato as an FX block rather than a modulation route | — |
| `saturator_*` (drive, post_gain, dry, wet) | Saturation/distortion stage | overlaps with AMGR's `drive` but AMGR's is in the filter stage, not a separate FX |
| `overdrive_*` | A second, different drive stage | — |
| `distortion` | A third distortion amount, separate from the above two (guess: bit/wave distortion) | — |
| `reducer_bit` / `reducer_rate` / `reducer_dry` / `reducer_wet` | Bitcrusher + sample-rate reducer | — |
| `eq3_*` (gain_low/mid/hi, freq_low/hi, slope, low/mid/hi_on) | 3-band EQ with per-band on/off | — |
| `compressor_*` / `comp_*` (threshold_db, knee_db, ratio, gain_db, attack, release, look_ahead) | Full compressor with lookahead | — |
| `mbr_*` (cutoff, resonance, detune, tilt, scale, decay, pan, width, noise, noise_rate, pitch, tone, band_count, stereo, dry, wet) | A multiband resonator effect — essentially a 48-band comb/resonant filter bank (`band_count = FortyEight`) | — (nothing like this in AMGR) |
| `delay_*` (adjust_mode, sync_mode, feedback_mode, rate_preset, frac_time, feedback, dry, wet) | Delay 1, tempo-syncable | AMGR's `dtime/dfb/dtone/dmix` — similar role, no tempo sync, one delay not two |
| `delay2_*` | A **second**, independent delay | — (AMGR has one delay) |
| `reverb_*` (width, damping, room_size, dry, wet) | A small/algorithmic reverb | overlaps with AMGR's `vsize/vdamp/vmix` |
| `large_reverb_*` (pre_lpf/hpf cutoff, time, room_size, dampening, spread, early, tail, pre, loshelf/hishelf cutoff+gain, dry, wet) | A **second**, bigger/more detailed reverb algorithm with early/late split and its own EQ | — |
| `audio_in_fx_send` / `synth_fx_send` | How much of the live audio input vs. the synth's own voice goes into the FX chain | — |
| `fxchain[4]` / `fxfocus` / `single_fx_preset_slot[15]` / `gui_fx_settings[15]` | The FX chain's routing order (which of the many FX blocks are active and in what order) and saved single-FX presets | — |
| `dry_mode` | `Stereo` — how the overall dry signal is handled | — |

**Study point:** GR-2 ships roughly a dozen FX blocks per layer, each independently wet/dry,
plus two delays and two reverbs. AMGR has one filter, one delay, one reverb.

## 9. LFOs and modulation
| GR-2 key | What it is | AMGR |
|---|---|---|
| `lfo_speeds[4]` / `lfo_muls[4]` / `lfo_divs[4]` | Four LFOs, each with a rate (free or as a tempo multiplier/divider) | — |
| `lfo_wavetypes[4]` / `lfo_phases[4]` / `lfo_quants[4]` | Waveform, start phase, and step-quantization per LFO | — |
| `lfo_dests[4]` | What each LFO is routed to | — |
| `lfo_crossmod_enabled` / `lfo12_xmod` | LFO 1 can modulate LFO 2's rate (FM between LFOs) | — |
| `lfo_delay_enabled` | Fade-in delay before an LFO starts after a note | — |
| `cv_dests[2]` / `selected_cv` | Two CV inputs (physical, on the back panel) and what they're routed to | — (AMGR has no CV input) |
| `modulators[28]` | A bank of 28 modulation source slots — likely holds the LFOs, envelopes, CVs, random generators and MPE sources all as one addressable list | — (AMGR has no modulation matrix) |
| `modulation_list[100]` / `mod_list_size` | Up to 100 active source→destination modulation routes (only `mod_list_size` of them active) | — |
| `macro_list[100]` / `macro_list_size` / `macro_levels[20]` / `i_current_macro` | Macro knobs: each macro can drive several destinations at once | — (AMGR has no macros) |
| `mod_source_index` / `mod_source_indices[4]` / `mod_dest_indices[4]` / `selected_mod_source` | UI state for editing the modulation list, not sound data | — |
| `mod_reduction_type` | How conflicting modulation on the same destination combines (`Newest` wins, guess) | — |
| `modwheel` | Current mod wheel position, stored for recall (not really a "parameter" of the sound) | — |
| `modamtcubic` | Whether a modulation amount uses a cubic (vs linear) response curve | — |

**Study point:** this is the part of the manual that calls out "4 envelopes, 4 LFOs, 2 CVs, 4
random generators, MPE, MIDI, macros" as the big 1.8 feature. It's a real modulation matrix, not
hard-wired routes. This is the second-biggest gap after the 4-envelope system.

## 10. Granular Slice engine
| GR-2 key | What it is | AMGR |
|---|---|---|
| `slices[128]` / `num_slices` / `selected_slice` | Up to 128 manually or auto-placed slice markers in the sample | — |
| `autoslice_sensitivity` | Threshold for automatic transient-based slicing | — |
| `slice_slider_op` / `sample_slider_op` / `rec_sample_slider_op` / `slice_scrub_enabled` | What dragging on the waveform display does in each mode (zoom, drag start, scrub) | — |

## 11. Tape engine
| GR-2 key | What it is | AMGR |
|---|---|---|
| `tape_reactivity` | How quickly simulated tape speed responds to changes (guess) | — |
| `rate_mode` / `rate_hz` | Playback rate, free-running or locked | — |
| `bouncewrap_mode` | What happens at the end of the tape: bounce back or wrap around | — |
| `loop_start` / `loop_end` / `play_end` | Loop points, separate from the granular engine's own | — |
| `midside` | Mid/side balance control (stereo width, guess) | overlaps loosely with AMGR's `width` |

## 12. Spectral (phase vocoder) engine
| GR-2 key | What it is | AMGR |
|---|---|---|
| `pv_exp_stretch` | Time-stretch amount | — |
| `pv_amp_thresh` / `pv_amp_smear` | Noise gating and smoothing of the spectral amplitude | — |
| `pv_formant_shift` | Shift formants independently of pitch | — |
| `pv_freeze` | Freeze the spectrum at a moment (classic "spectral freeze") | — |
| `pv_bend` / `pv_pitch_blend` | Pitch bending of the spectral content | — |
| `pv_fold` | Spectral folding/distortion | — |
| `pv_spec_depth` / `pv_spec_contrast` | Spectral shaping | — |
| `fft_len` | FFT window size (2048 here) | — |
| `pvoc_engine` | Algorithm version (`V2`) | — |
| `pvoc_multiband` / `pvoc_phase_lock` / `pvoc_rtpghi` / `pvoc_transient_reset` / `pvoc_formant_preserve` / `pvoc_bend_spectral` | Phase-vocoder quality/mode switches (phase locking, transient handling, formant preservation) | — |
| `display_mode` | `Spectrogram` — what the waveform display shows for this engine | — (AMGR always shows a waveform) |

## 13. Chord / arpeggiator / sequencer
| GR-2 key | What it is | AMGR |
|---|---|---|
| `chord_enabled` / `chord[12]` / `chord_finetunes[12]` / `chord_chances[12]` / `chord_pitches[]` / `chord_index` | One held note triggers a programmable chord, each chord note with its own tune and probability | — |
| `arp_mode` | Arpeggiator direction (`Up`) | — |
| `seq_data.*` | A full per-layer step sequencer: `tempo`, `len` (steps), `step_datas[64]`, `mode` (direction), `plock_dests[16]` (parameter-lock destinations), `multiplier`/`divider`/`rate_preset` (clock), `transpose`, `enabled`, `free_bpm`, `drunkness` (humanize), `mod_slew`, `plocks_enabled` | — |

## 14. Scale / tuning
| GR-2 key | What it is | AMGR |
|---|---|---|
| `western_scale[12]` / `freescale` / `scale` | A 12-note mask defining which notes are in-scale, or free (chromatic) | — |
| `pitch_quant` | Quantize per-voice or per-note (guess) | — |
| `slider_semitones` | Range of a pitch slider control | — |
| `coarse_spl_tuning_base_log` / `spl_tuning_base` | Sample base pitch, log and linear forms | AMGR's `pitch1..4` |

## 15. Recording
| GR-2 key | What it is | AMGR |
|---|---|---|
| `audio_input_type` | `AnalogInput` — where recorded audio comes from | — (AMGR only loads files, no live recording) |
| `audio_destination` | Where a new recording is saved to (`CurrentPreset`) | — |
| `rec_file_name` | Name of the last/pending recording | — |
| `play_follows_rec` / `scrolling_rec` / `rec_scroll_time_s` / `clocked_record` / `armed_midi_rec` / `rec_arm_level_db` | Recording behavior: auto-play after recording, live scrolling view, fixed-length vs tempo-synced recording, MIDI-armed recording, and the input level that triggers an armed recording | — |
| `loopingrec` | Whether recording loops | — |

## 16. UI / session state (not sound parameters — included for completeness, since a clone needs to know what NOT to treat as a sound parameter)
| GR-2 key | What it is |
|---|---|
| `top_enc_page`, `win_btn_page`, `env_btn_page`, `fil_btn_page`, `amp_btn_page`, `fx_btn_page`, `mod_btn_page`, `eq_btn_page`, `mac_btn_page` | Which sub-page of each section's menu was last open |
| `selected_env`, `selected_lfo`, `selected_cv`, `selected_mod_source`, `selected_slice`, `fxfocus` | Which item in each list is currently selected in the UI |
| `zoom_factor`, `zoom_startpoint` | Waveform display zoom state |
| `side_panel_type` / `completed_side_panel_type` | Which panel (e.g. `FourLFO`) is shown on the side |
| `rnd.x` / `rnd.y` / `rnd.z` | A saved xorshift RNG seed/state (same algorithm family AMGR's `rnd01()` uses) — saved so random grain behavior resumes deterministically after a reload |
| `mod_year` | Unclear, possibly a schema/migration version stamp (guess) |

## 17. Top-level / misc
| GR-2 key | What it is | AMGR |
|---|---|---|
| `name` / `sample_name` / `comments` / `version` | Preset metadata | AMGR's preset name/author aren't stored per-preset the same way |
| `volume` | Layer output level | `level1..4` |
| `basenote` | Root MIDI note for the sample | — |
| `filtmode` | A second filter-mode-like field, `None` here — possibly a legacy/alternate filter path (guess) | — |
| `aux_len` / `aux_buf[1024]` | A small auxiliary buffer — possibly a recorded modulation envelope (guess) | — |
| `antialias` / `antialias_type` | Interpolation quality when resampling for pitch (`Linear` here; the manual likely lists better modes) | AMGR uses plain linear interpolation always |
| `pitch_bend_amp` | Pitch bend range in semitones (12 here) | AMGR's `bend` parameter, in steps 1/2/5/7/12/24 — same idea |

---

## Where this leaves AMGR vs. the GR-2

AMGR today covers, in GR-2 terms: the granular engine's density/size/scan/reverse, one filter,
one ADSR, one delay, one reverb, and basic pan/level/pitch per layer. That is a solid core of
engine #1 of 5, using maybe 15 of this preset's ~350 fields.

The three biggest, clearly-scoped gaps for "AMGR, grown toward GR-2" are, in rough order of how
much they'd change the sound:

1. **Four independent envelopes per layer** (amp, filter, pitch, aux) instead of one.
2. **A real modulation matrix**: 4 LFOs + the 4 envelopes + random + MPE, routable to any
   destination, plus macros.
3. **A bigger FX chain**: a second delay, a second (bigger) reverb, chorus/flanger/ring
   mod/vibrato/saturator/overdrive/bitcrusher/EQ3/multiband resonator, each independently
   wet/dry and chain-orderable.

Behind those: the other four engines (Granular Slice, Sampler, Tape, Spectral) are each a
separate playback mode sharing the same modulation/FX system — in effect four more instruments
built on the same chassis.

None of this requires anything from the firmware binary or the factory samples — it's all
buildable from first principles once the target parameter list is known, which is what this
document gives you.

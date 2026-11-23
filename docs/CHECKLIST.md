# AMGR 0.2.5 – pass / fail checklist

PASS = checked in this build (offline, in the build sandbox). NOT RUN = needs the Force (or a person) and has not been done
for this exact build. FAIL = known problem.

## Build and package

| Item | Result |
|---|---|
| ARM cross-build (armv7 hard-float, Zig) of `amgr.so` | PASS |
| Highest glibc symbol 2.27 (limit 2.31) | PASS |
| `params.json` generated (73 params), 14 pages, 48 knobs placed, markers on all 14 pages | PASS |
| `AMGR-0.2.5-mpc-armv7.zip` built by `release.py` (34 files, 0.9 MB) | PASS |
| `sh -n install.sh`, `sh -n uninstall.sh` | PASS |
| `sha256sum -c SHA256SUMS` (all files) | PASS |
| `L'Cronx` apostrophe present in paths/manifest and shell-escaped correctly | PASS |

## Fixes carried in from earlier Force testing (0.1.0 → 0.2.0)

| Item | Result |
|---|---|
| Tiles/buttons no longer render as text-less grey blocks | NOT RUN on this exact build (fixed in 0.2.1, not re-broken since) |
| SAMPLES browser starts in the plugin's own folder, not filesystem root | NOT RUN on this exact build (fixed in 0.2.1, not re-broken since) |
| Data wheel scrolls open lists both directions | NOT RUN on this exact build (fixed in 0.2.1, not re-broken since) |
| PRESETS/SAMPLES tiles and knobs no longer "bake" onto other pages | NOT RUN on this exact build (fixed in 0.2.2, not re-broken since) |

## New in 0.2.5 (not yet tested on hardware)

| Item | Result |
|---|---|
| CPU-safety governor (density scale-back under load, grain-stealing, release-stage grain cut-down) | NOT RUN — needs sustained heavy voice/grain load on the real Force CPU, cannot be measured in this sandbox |
| Factory presets vary all parameters, with safety clamps | NOT RUN — needs listening on hardware |
| Grains travel both directions / varying speed by default | NOT RUN — needs listening on hardware |
| Brightened knob art | NOT RUN — needs eyes on the real screen |

## Not yet verified at all

| Item | Result |
|---|---|
| Any 0.2.x build actually running on the Force without the fixed bugs recurring | NOT RUN since 0.2.2; the maintainer's last confirmed hardware round was on the pre-fix builds that surfaced these bugs |

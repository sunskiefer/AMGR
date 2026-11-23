#!/usr/bin/env bash
# Build AMGR for the Force / 32-bit MPCs: armv7-a hard-float, glibc <= 2.31, with Zig (pip install ziglang). ARM only: no desktop build.
#   ./build.sh   ->  build/arm/amgr.so and build/package/<plugin folder>/ (skin, Fonts, Samples)
# The vendor name carries an apostrophe: every use of NAME / PKG below is double-quoted. Keep it so.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="${MPC_VST:-$PWD/third_party/mpc-vst-plugins}"
ZIG="${ZIG:-python3 -m ziglang}"
NAME="L'Cronx - VST - AMGR"
mkdir -p build/tools build/arm
python3 tools/gen_amgr.py
[ -x build/tools/shadow_art ] || gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -x c -o build/tools/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
rm -rf build/skin build/params.h build/pluginlist-entry.xml
SHADOW_ART="$PWD/build/tools/shadow_art" python3 "$MPC_VST/tools/gen_vst.py" vst.json 2>&1 | grep -v -e "TOUCH" -e "EDGE" -e "MODULE_DIR" -e "QLINK" || true
[ -f build/params.h ] && [ -d "build/skin/$NAME/Plugin Skins" ]
TGT="-target arm-linux-gnueabihf.2.31 -mcpu=generic+v7a+vfp3d16-d32-neon+thumb2 -ffp-contract=off"
COMMON="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -w -std=gnu11"
OBJS=""
for f in src/gr2_engine.c src/gr2_overlay.c src/gr2_screen.c src/gr2_touch.c src/font.c src/wrapper/vst2_wrap.c; do
  o="build/arm/$(echo "$f" | tr / _).o"
  $ZIG cc $TGT $COMMON -Ibuild -Isrc -I"$MPC_VST" -I"$MPC_VST/wrapper" -Ithird_party -Ithird_party/stb -Ithird_party/dr_libs -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
printf '{\n  global: VSTPluginMain;\n  local: *;\n};\n' > build/arm/exports.map
$ZIG cc $TGT -shared -fPIC -Wl,--no-undefined -Wl,--version-script=build/arm/exports.map -Wl,--gc-sections -Wl,-s $OBJS -lm -ldl -lpthread -o build/arm/amgr.so
PKG="build/package/$NAME"
rm -rf build/package && mkdir -p "$PKG"
cp -r "build/skin/$NAME/." "$PKG/"
mkdir -p "$PKG/Fonts" && cp third_party/fonts/TitilliumWeb-SemiBold.ttf third_party/fonts/TitilliumWeb-Regular.ttf third_party/fonts/OFL.txt "$PKG/Fonts/"
mkdir -p "$PKG/Samples" && printf 'Put .wav or .flac files here (up to 5 minutes each). On first start layer 1 takes the first file (by name), layer 2 the second, and so on; a layer without a file plays a built-in test source. On the SAMPLES page tap a layer box to browse any folder or drive; each box remembers where it last looked. Touch a waveform on the screen to set where its grains play.\n' > "$PKG/Samples/README.txt"
echo "-> $PKG, build/arm/amgr.so"

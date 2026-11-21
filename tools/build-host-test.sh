#!/usr/bin/env bash
# Native x86_64 host build of ProjectDural to test the exact platform/arm64/
# SLJIT native recompiler, Core 2 SoundWorker, and 3D export pipeline end-to-end.
#
# Usage:
#   tools/build-host-test.sh [bench_frames]
set -euo pipefail
cd "$(dirname "$0")/.."

BENCH_FRAMES="${1:-60}"
DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-host-arm64"
JOBS="${JOBS:-$(nproc)}"

ZIG_BIN="$DEPS/zig-linux/zig"
if [ ! -x "$ZIG_BIN" ]; then
    echo "Run tools/build-arm64.sh first to fetch Zig and pinned dependencies." >&2
    exit 1
fi
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

ZLIB_DIR=$(ls -d "$DEPS/zlib-src"/zlib-* 2>/dev/null | head -1)
SDL_INC="$PWD/build-arm64/inc"
mkdir -p "$BDIR/rt" "$BDIR/ui" "$BDIR/obj" "$BDIR/zlib"

echo "[1/3] Building native x86_64 ProjectDural runtime..."

ZLIB_SRCS=(
    adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c
    infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c
)
if [ ! -f "$BDIR/libz.a" ]; then
    printf '%s\n' "${ZLIB_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
        src="'"$ZLIB_DIR"'/$1"
        out="$2/$(basename "$1" .c).o"
        [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
        exec "$3" cc -std=c89 -O2 -w -DZ_HAVE_UNISTD_H -c "$src" -o "$out"
    ' _ {} "$BDIR/zlib" "$ZIG_BIN"
    "$ZIG_BIN" ar rcs "$BDIR/libz.a" "$BDIR/zlib"/*.o
fi

"$ZIG_BIN" cc -O2 -w -DSLJIT_CONFIG_AUTO=1 -c third_party/sljit/sljitLir.c -o "$BDIR/sljitLir.o"

CXX_FLAGS="-std=c++20 -O2 -fno-fast-math -ffp-contract=off \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function -Wno-attributes \
  -Isrc/core/shim_include -Isrc -Iplatform/arm64 -I$ZLIB_DIR -I$EXTERN/imgui -I$SDL_INC -I$SDL_INC/SDL2"

RT_SRCS=(
    third_party/mame/i960/i960.cpp third_party/mame/i960/i960dis.cpp
    src/core/mame_shim.cpp src/core/mb86233.cpp src/core/i960_device.cpp
    src/rec/i960_rec.cpp src/rec/i960_analyse.cpp src/rec/i960_native.cpp src/rec/vf2_map.cpp src/rec/gen_none.cpp
    src/machine/model2a.cpp src/machine/random_fighter.cpp src/machine/hud_names.cpp
    src/machine/model2_video.cpp src/machine/model2_smooth.cpp src/machine/scsp.cpp src/machine/scspdsp.cpp
    third_party/mame/m68000/m68000.cpp third_party/mame/m68000/m68000-decode.cpp
    third_party/mame/m68000/m68000-sdf.cpp third_party/mame/m68000/m68000-sdp.cpp
    src/host/romset.cpp src/host/stb_image_impl.cpp
)

printf '%s\n' "${RT_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"; tag=$(echo "$src" | tr "/." "__"); out="$2/${tag}.o"
    extra=""
    case "$src" in third_party/*|src/host/stb_image_impl.cpp) extra="-w" ;; esac
    exec "$3" c++ '"$CXX_FLAGS"' $extra -c "$src" -o "$out"
' _ {} "$BDIR/rt" "$ZIG_BIN"
rm -f "$BDIR/libruntime.a"
"$ZIG_BIN" ar rcs "$BDIR/libruntime.a" "$BDIR/rt"/*.o "$BDIR/sljitLir.o"

for f in extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp; do
    o="$BDIR/ui/$(basename "$f" .cpp).o"
    [ -f "$o" ] && [ "$o" -nt "$f" ] || "$ZIG_BIN" c++ -std=c++20 -O2 -w -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -I"$EXTERN/imgui" -c "$f" -o "$o"
done
rm -f "$BDIR/libimgui.a"
"$ZIG_BIN" ar rcs "$BDIR/libimgui.a" "$BDIR/ui"/*.o

for f in platform/arm64/gpu_gles.cpp platform/arm64/main_arm64.cpp; do
    "$ZIG_BIN" c++ $CXX_FLAGS -c "$f" -o "$BDIR/obj/$(basename "$f" .cpp).o"
done

# Link with system SDL2 if available, else generate link-time stub for headless bench
if pkg-config --exists sdl2 2>/dev/null; then
    SDL_LINK=$(pkg-config --libs sdl2)
else
    python3 - "$BDIR/sdl2_stub.c" "$BDIR/obj"/*.o <<'PYEOF'
import subprocess, sys
syms = set()
for o in sys.argv[2:]:
    for line in subprocess.check_output(["nm", "-u", o], text=True).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U" and parts[-1].startswith("SDL_"):
            syms.add(parts[-1])
with open(sys.argv[1], "w") as f:
    for s in sorted(syms): f.write(f"void {s}(void) {{}}\n")
PYEOF
    "$ZIG_BIN" cc -shared -Wl,-soname,libSDL2-2.0.so.0 -o "$BDIR/libSDL2-2.0.so.0" "$BDIR/sdl2_stub.c"
    SDL_LINK="-L$BDIR -l:libSDL2-2.0.so.0 -Wl,-rpath,$BDIR"
fi

"$ZIG_BIN" c++ -O2 -fno-fast-math -ffp-contract=off \
    -o "$BDIR/vf2.x86_64" \
    "$BDIR/obj"/*.o \
    "$BDIR/libruntime.a" "$BDIR/libimgui.a" "$BDIR/libz.a" \
    $SDL_LINK -lm -ldl -lpthread

echo "[2/3] Built $BDIR/vf2.x86_64"
echo "[3/3] Running ROM-free synthetic verification ($BENCH_FRAMES frames)..."
"$BDIR/vf2.x86_64" --bench-synthetic "$BENCH_FRAMES"

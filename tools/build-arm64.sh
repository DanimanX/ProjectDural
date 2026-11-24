#!/usr/bin/env bash
# Cross-compile ProjectDural (ROM-free distributable runtime) for AArch64 Linux
# handhelds (Anbernic RG34XX / Allwinner H700, RG35XX Plus/H/SP, RG40XX,
# RG CubeXX, Powkiddy RGB30, TrimUI Smart Pro) targeting glibc <= 2.28 +
# OpenGL ES 2.0 + ARM64 SLJIT + NEON, and assemble a PortMaster zip package.
#
# Zero copyrighted ROMs or ROM-derived game code ever enter this package.
#
# Usage:
#   tools/build-arm64.sh                    # default CPU: cortex_a53 (H700)
#   TARGET_CPU=cortex_a55 tools/build-arm64.sh
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -n "${TMPDIR:-}" ] && [ ! -d "$TMPDIR" ]; then
    export TMPDIR="/tmp"
fi

DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-arm64"
OUT="$PWD/dist/ProjectDural-PortMaster-aarch64"
ZIPOUT="$PWD/dist/ProjectDural-PortMaster-aarch64.zip"
TARGET_CPU="${TARGET_CPU:-cortex_a53}"
TARGET_CPU="${TARGET_CPU//-/_}"
TARGET_TRIPLE="aarch64-linux-gnu.2.28"
JOBS="${JOBS:-$(nproc)}"

ZIG_VER="0.17.0"
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64|amd64)
        ZIG_ARCH="x86_64"
        ZIG_SHA="1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"
        ;;
    aarch64|arm64)
        ZIG_ARCH="aarch64"
        ZIG_SHA="9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8"
        ;;
    *)
        echo "ERROR: unsupported host architecture for Zig: $HOST_ARCH" >&2
        exit 1
        ;;
esac
SDL_VER="2.32.10"
SDL_SHA="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
ZLIB_VER="1.3.1"
ZLIB_SHA="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"

IMGUI_URL="https://github.com/ocornut/imgui.git"
IMGUI_REV="f1cc2ae15e53a861a874c3034aae6798fde194ab"

mkdir -p "$DEPS/dl" "$EXTERN" "$BDIR/obj" "$BDIR/rt" "$BDIR/ui" "$BDIR/zlib"

fetch_file() {
    local url="$1" name="$2" sha="$3"
    if [ ! -f "$DEPS/dl/$name" ]; then
        curl -fsSL --retry 3 -o "$DEPS/dl/$name.part" "$url"
        mv "$DEPS/dl/$name.part" "$DEPS/dl/$name"
    fi
    echo "$sha  $DEPS/dl/$name" | sha256sum -c --quiet || {
        echo "ERROR: SHA-256 mismatch for $DEPS/dl/$name" >&2
        exit 1
    }
}

fetch_git() {
    local url="$1" rev="$2" dest="$3" sentinel="$4"
    if [ ! -d "$dest/.git" ]; then
        git clone --quiet --no-checkout "$url" "$dest"
    fi
    if [ ! -f "$dest/$sentinel" ] || [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null || true)" != "$rev" ]; then
        git -C "$dest" fetch --quiet --depth 1 origin "$rev" 2>/dev/null || git -C "$dest" fetch --quiet origin || true
        git -C "$dest" checkout --quiet --force --detach "$rev"
    fi
    [ -f "$dest/$sentinel" ] || { echo "ERROR: missing $dest/$sentinel after checkout of $rev" >&2; exit 1; }
}

# 1. Host Zig compiler (hermetic aarch64-linux-gnu.2.28 C/C++20 cross-compiler)
if [ ! -x "$DEPS/zig-linux/zig" ]; then
    echo "[1/6] Fetching Zig $ZIG_VER ($ZIG_ARCH-linux)..."
    fetch_file "https://ziglang.org/download/$ZIG_VER/zig-$ZIG_ARCH-linux-$ZIG_VER.tar.xz" \
        "zig-$ZIG_ARCH-linux-$ZIG_VER.tar.xz" "$ZIG_SHA"
    tar -C "$DEPS" -xJf "$DEPS/dl/zig-$ZIG_ARCH-linux-$ZIG_VER.tar.xz"
    rm -rf "$DEPS/zig-linux"
    mv "$DEPS/zig-$ZIG_ARCH-linux-$ZIG_VER" "$DEPS/zig-linux"
fi
ZIG_BIN="$DEPS/zig-linux/zig"
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

# 2. Pinned upstream third-party dependencies (Dear ImGui, zlib 1.3.1)
echo "[2/6] Checking pinned dependencies (Dear ImGui, zlib $ZLIB_VER)..."
fetch_git "$IMGUI_URL" "$IMGUI_REV" "$EXTERN/imgui" "imgui.cpp"
if [ ! -f "$DEPS/zlib-src/zlib-$ZLIB_VER/zlib.h" ]; then
    fetch_file "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz" \
        "zlib-$ZLIB_VER.tar.gz" "$ZLIB_SHA"
    rm -rf "$DEPS/zlib-src" && mkdir -p "$DEPS/zlib-src"
    tar -C "$DEPS/zlib-src" -xzf "$DEPS/dl/zlib-$ZLIB_VER.tar.gz"
fi
ZLIB_DIR="$DEPS/zlib-src/zlib-$ZLIB_VER"

# 3. Clean SDL2 2.32.10 headers (target-independent Linux LP64 + GLES 2.0)
if [ ! -d "$DEPS/sdl2-src/SDL2-$SDL_VER/include" ]; then
    echo "[3/6] Fetching SDL2 $SDL_VER headers..."
    fetch_file "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" \
        "SDL2-$SDL_VER.tar.gz" "$SDL_SHA"
    rm -rf "$DEPS/sdl2-src" && mkdir -p "$DEPS/sdl2-src"
    tar -C "$DEPS/sdl2-src" -xzf "$DEPS/dl/SDL2-$SDL_VER.tar.gz"
fi
SDL_INC="$BDIR/inc"
rm -rf "$SDL_INC" && mkdir -p "$SDL_INC/SDL2"
cp "$DEPS/sdl2-src/SDL2-$SDL_VER/include"/*.h "$SDL_INC/SDL2/"
cat > "$SDL_INC/SDL2/SDL_config.h" <<'EOF'
#ifndef SDL_config_h_
#define SDL_config_h_
#include "SDL_platform.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#define SDL_BYTEORDER 1234
#define SIZEOF_VOIDP 8
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_ALLOCA_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_VSNPRINTF 1
#define HAVE_SNPRINTF 1
#define SDL_VIDEO_OPENGL_ES2 1
#define SDL_VIDEO_OPENGL_EGL 1
#endif
EOF

# 4. Cross-compile zlib, SLJIT, MAME cores, Model 2A runtime, Dear ImGui, and ARM64 GLES 2.0 frontend
echo "[4/6] Cross-compiling ProjectDural for $TARGET_TRIPLE ($TARGET_CPU, GLES 2.0, SLJIT ARM64, NEON, -j$JOBS)..."

# 4a. Static zlib
ZLIB_SRCS=(
    adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c
    infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c
)
if [ ! -f "$BDIR/libz.a" ]; then
    printf '%s\n' "${ZLIB_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
        src="$1/'"$ZLIB_DIR"'/$2"
        src="'"$ZLIB_DIR"'/$2"
        out="$3/$(basename "$2" .c).o"
        [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
        exec "$4" cc -target '"$TARGET_TRIPLE"' -mcpu='"$TARGET_CPU"' -O2 -w -DZ_HAVE_UNISTD_H -c "$src" -o "$out"
    ' _ "$PWD" {} "$BDIR/zlib" "$ZIG_BIN"
    "$ZIG_BIN" ar rcs "$BDIR/libz.a" "$BDIR/zlib"/*.o
fi

# 4b. SLJIT (ARM64 native code generator)
"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O3 -w -DSLJIT_CONFIG_AUTO=1 \
    -c third_party/sljit/sljitLir.c -o "$BDIR/sljitLir.o"

CXX_FLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -std=c++20 -O3 -fno-fast-math -fno-math-errno -ffp-contract=off \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function -Wno-attributes \
  -Isrc/core/shim_include -Isrc -Iplatform/arm64 -I$ZLIB_DIR -I$EXTERN/imgui -I$SDL_INC -I$SDL_INC/SDL2"

# 4c. Core + Machine + Host + M68000 (C++20)
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
    src="$1"
    tag=$(echo "$src" | tr "/." "__")
    out="$2/${tag}.o"
    extra=""
    case "$src" in
        third_party/mame/m68000/*|third_party/*|src/host/stb_image_impl.cpp) extra="-w -O3 -fno-math-errno" ;;
        src/machine/model2_video.cpp|src/machine/model2_smooth.cpp) extra="-O3 -fno-math-errno -ffp-contract=fast" ;;
        *) extra="-O3 -fno-math-errno" ;;
    esac
    exec "$3" c++ '"$CXX_FLAGS"' $extra -flto=thin -c "$src" -o "$out"
' _ {} "$BDIR/rt" "$ZIG_BIN"
rm -f "$BDIR/libruntime.a"
"$ZIG_BIN" ar rcs "$BDIR/libruntime.a" "$BDIR/rt"/*.o "$BDIR/sljitLir.o"

# 4d. Dear ImGui
UI_SRCS=(
    extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp
)
printf '%s\n' "${UI_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
    exec "$3" c++ -target '"$TARGET_TRIPLE"' -mcpu='"$TARGET_CPU"' -std=c++20 -O2 -w \
        -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -I'"$EXTERN/imgui"' -c "$src" -o "$out"
' _ {} "$BDIR/ui" "$ZIG_BIN"
rm -f "$BDIR/libimgui.a"
"$ZIG_BIN" ar rcs "$BDIR/libimgui.a" "$BDIR/ui"/*.o

# 4e. ARM64 Frontend
APP_SRCS=(platform/arm64/gpu_gles.cpp platform/arm64/main_arm64.cpp)
printf '%s\n' "${APP_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    exec "$3" c++ '"$CXX_FLAGS"' -O3 -fno-math-errno -ffp-contract=fast -c "$src" -o "$out"
' _ {} "$BDIR/obj" "$ZIG_BIN"

# 5. Link vf2.aarch64 against link-time SDL2 stub + static libz.a + static libimgui.a
echo "[5/6] Generating SDL2 link stub and linking vf2.aarch64..."
python3 - "$BDIR/sdl2_stub.c" "$BDIR/obj"/*.o <<'PYEOF'
import subprocess, sys
out_c = sys.argv[1]
objs = sys.argv[2:]
syms = set()
for o in objs:
    p = subprocess.run(["nm", "-u", o], capture_output=True, text=True, check=True)
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U" and parts[-1].startswith("SDL_"):
            syms.add(parts[-1])
with open(out_c, "w") as f:
    f.write("/* Link-time-only stub for PortMaster system libSDL2-2.0.so.0 */\n")
    for s in sorted(syms):
        f.write(f"void {s}(void) {{}}\n")
print(f"  Generated SDL2 stub with {len(syms)} symbols")
PYEOF

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -shared -Wl,-soname,libSDL2-2.0.so.0 \
    -o "$BDIR/libSDL2-2.0.so.0" "$BDIR/sdl2_stub.c"

"$ZIG_BIN" c++ -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O3 -flto=thin -fno-fast-math -ffp-contract=off \
    -o "$BDIR/vf2.aarch64" \
    "$BDIR/obj"/*.o \
    "$BDIR/libruntime.a" "$BDIR/libimgui.a" "$BDIR/libz.a" \
    -L"$BDIR" -l:libSDL2-2.0.so.0 -lm -ldl -lpthread

# 6. Verify ELF architecture, dynamic deps, glibc <= 2.28
echo "[6/6] Verifying AArch64 binary and assembling ROM-free PortMaster package..."
FILE_INFO=$(file "$BDIR/vf2.aarch64")
echo "$FILE_INFO" | grep -q "ARM aarch64" || { echo "ERROR: not ARM aarch64 ($FILE_INFO)" >&2; exit 1; }
MAX_GLIBC=$(objdump -T "$BDIR/vf2.aarch64" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
if [ -n "$MAX_GLIBC" ] && [ "$(printf '%s\n2.28\n' "$MAX_GLIBC" | sort -V | tail -1)" != "2.28" ]; then
    echo "ERROR: vf2.aarch64 requires GLIBC_$MAX_GLIBC (max allowed 2.28)" >&2
    exit 1
fi
READELF_D=$(readelf -d "$BDIR/vf2.aarch64")
if printf '%s\n' "$READELF_D" | grep -qE "libstdc\+\+|libz\.so"; then
    echo "ERROR: unexpected libstdc++.so or libz.so dependency (must be statically linked)" >&2
    exit 1
fi
echo "  OK: vf2.aarch64 -> ARM aarch64, max GLIBC_${MAX_GLIBC:-none}, zero libstdc++.so/libz.so"
readelf -d "$BDIR/vf2.aarch64" | grep NEEDED

# Assemble ROM-free PortMaster directory & zip
rm -rf "$OUT" "$ZIPOUT"
mkdir -p "$OUT/projectdural/roms" "$OUT/projectdural/saves" \
         "$OUT/projectdural/libs.aarch64" "$OUT/projectdural/licenses" \
         "$OUT/projectdural/assets/backgrounds"

cp portmaster/*.sh "$OUT/"
chmod +x "$OUT/"*.sh
cp portmaster/projectdural/port.json "$OUT/projectdural/"
cp portmaster/projectdural/gameinfo.xml "$OUT/projectdural/"
cp portmaster/projectdural/gamecontrollerdb.txt "$OUT/projectdural/"
cp portmaster/projectdural/PUT-ROMS-HERE.txt "$OUT/projectdural/roms/PUT-ROMS-HERE.txt"
cp assets/backgrounds/select.png "$OUT/projectdural/assets/backgrounds/"
cp "$BDIR/vf2.aarch64" "$OUT/projectdural/vf2.aarch64"
chmod +x "$OUT/projectdural/vf2.aarch64"

cp LICENSE "$OUT/projectdural/licenses/ProjectDural-MIT.txt"
cp third_party/mame/README.md "$OUT/projectdural/licenses/MAME-BSD3-README.md"
cp third_party/sljit/LICENSE "$OUT/projectdural/licenses/SLJIT-BSD2.txt"
cp "$DEPS/sdl2-src/SDL2-$SDL_VER/LICENSE.txt" "$OUT/projectdural/licenses/SDL2-LICENSE.txt"
[ -f "$EXTERN/imgui/LICENSE.txt" ] && cp "$EXTERN/imgui/LICENSE.txt" "$OUT/projectdural/licenses/DearImGui-LICENSE.txt"
[ -f "$ZLIB_DIR/LICENSE" ] && cp "$ZLIB_DIR/LICENSE" "$OUT/projectdural/licenses/zlib-LICENSE.txt"
cp portmaster/projectdural/README.md "$OUT/projectdural/README.md"

if command -v zip >/dev/null 2>&1; then
    (cd "$OUT" && zip -qr9 "$ZIPOUT" *.sh projectdural)
else
    python3 -c "import shutil; shutil.make_archive('${ZIPOUT%.zip}', 'zip', '$OUT')"
fi
cp "$ZIPOUT" "$PWD/dist/projectdural.zip"

# Also stage ready-to-submit PortMaster-New PR directory layout (ports/projectdural/)
PM_PR="$PWD/dist/PortMaster-New-PR/ports/projectdural"
rm -rf "$PM_PR"
mkdir -p "$PM_PR"
cp -r "$OUT/"* "$PM_PR/"
cp portmaster/projectdural/port.json "$PM_PR/port.json"
cp portmaster/projectdural/gameinfo.xml "$PM_PR/gameinfo.xml"
cp portmaster/projectdural/README.md "$PM_PR/README.md"

echo "Packaged: $ZIPOUT ($(du -h "$ZIPOUT" | cut -f1))"
echo "Packaged: $PWD/dist/projectdural.zip"
echo "Staged PortMaster-New PR tree: $PM_PR"

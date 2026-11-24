#!/usr/bin/env bash
# Build ProjectDural Vulkan (ROM-free distributable AArch64 runtime using SDL3
# SDL_GPU Vulkan + SPIR-V compute shaders + Dear ImGui SDL_GPU3) targeting
# aarch64-linux-gnu glibc <= 2.28 with static libc++/zlib/SDL3 (zero libstdc++.so
# dependency; libvulkan.so.1, Wayland, X11, KMSDRM and audio backends are loaded
# dynamically at runtime via dlopen).
#
# Zero copyrighted ROMs or ROM-derived game code ever enter this package.
#
# Usage:
#   tools/build-arm64-vulkan.sh
#   TARGET_CPU=cortex_a720 tools/build-arm64-vulkan.sh
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -n "${TMPDIR:-}" ] && [ ! -d "$TMPDIR" ]; then
    export TMPDIR="/tmp"
fi

DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-arm64-vulkan"
OUT="$PWD/dist/ProjectDural-Vulkan-aarch64"
ZIPOUT="$PWD/dist/ProjectDural-Vulkan-aarch64.zip"
TARGET_CPU="${TARGET_CPU:-generic}"
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

ZLIB_VER="1.3.1"
ZLIB_SHA="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
SDL3_URL="https://github.com/libsdl-org/SDL.git"
SDL3_REV="fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"
IMGUI_URL="https://github.com/ocornut/imgui.git"
IMGUI_REV="f1cc2ae15e53a861a874c3034aae6798fde194ab"

mkdir -p "$DEPS/dl" "$EXTERN" "$BDIR/obj" "$BDIR/rt" "$BDIR/ui" "$BDIR/zlib" "$BDIR/bin"

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

# 1. Host Zig compiler
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

# 2. Pinned upstream third-party dependencies (SDL3, Dear ImGui, zlib 1.3.1)
echo "[2/6] Checking pinned dependencies (SDL3, Dear ImGui, zlib $ZLIB_VER)..."
fetch_git "$SDL3_URL" "$SDL3_REV" "$EXTERN/sdl3" "CMakeLists.txt"
fetch_git "$IMGUI_URL" "$IMGUI_REV" "$EXTERN/imgui" "imgui.cpp"
if [ ! -f "$DEPS/zlib-src/zlib-$ZLIB_VER/zlib.h" ]; then
    fetch_file "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz" \
        "zlib-$ZLIB_VER.tar.gz" "$ZLIB_SHA"
    rm -rf "$DEPS/zlib-src" && mkdir -p "$DEPS/zlib-src"
    tar -C "$DEPS/zlib-src" -xzf "$DEPS/dl/zlib-$ZLIB_VER.tar.gz"
fi
ZLIB_DIR="$DEPS/zlib-src/zlib-$ZLIB_VER"

# 3. Static SDL3 (Vulkan + SDL_GPU + Wayland/X11/KMSDRM + PipeWire/PulseAudio/ALSA shared via dlopen)
echo "[3/6] Building static SDL3 ($TARGET_TRIPLE, -mcpu=$TARGET_CPU, -j$JOBS)..."
cat > "$BDIR/bin/zig-cc" <<EOF
#!/bin/sh
exec "$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" "\$@"
EOF
cat > "$BDIR/bin/zig-cxx" <<EOF
#!/bin/sh
exec "$ZIG_BIN" c++ -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" "\$@"
EOF
cat > "$BDIR/bin/zig-ar" <<EOF
#!/bin/sh
exec "$ZIG_BIN" ar "\$@"
EOF
cat > "$BDIR/bin/zig-ranlib" <<EOF
#!/bin/sh
exec "$ZIG_BIN" ranlib "\$@"
EOF
chmod +x "$BDIR/bin/zig-cc" "$BDIR/bin/zig-cxx" "$BDIR/bin/zig-ar" "$BDIR/bin/zig-ranlib"

CMAKE_GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    CMAKE_GEN_ARGS=(-G Ninja)
fi

if [ ! -f "$BDIR/sdl3-build/libSDL3.a" ]; then
    cmake -S "$EXTERN/sdl3" -B "$BDIR/sdl3-build" "${CMAKE_GEN_ARGS[@]}" \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$BDIR/bin/zig-cc" \
        -DCMAKE_CXX_COMPILER="$BDIR/bin/zig-cxx" \
        -DCMAKE_AR="$BDIR/bin/zig-ar" \
        -DCMAKE_RANLIB="$BDIR/bin/zig-ranlib" \
        -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON \
        -DSDL_DEPS_SHARED=ON \
        -DSDL_VULKAN=ON \
        -DSDL_GPU=ON \
        -DSDL_OPENGL=OFF \
        -DSDL_OPENGLES=OFF \
        -DSDL_WAYLAND=ON \
        -DSDL_WAYLAND_SHARED=ON \
        -DSDL_X11=ON \
        -DSDL_X11_SHARED=ON \
        -DSDL_KMSDRM=ON \
        -DSDL_KMSDRM_SHARED=ON \
        -DSDL_ALSA=ON \
        -DSDL_ALSA_SHARED=ON \
        -DSDL_PULSEAUDIO=ON \
        -DSDL_PULSEAUDIO_SHARED=ON \
        -DSDL_PIPEWIRE=ON \
        -DSDL_PIPEWIRE_SHARED=ON \
        -DSDL_JACK=OFF \
        -DSDL_SNDIO=OFF \
        -DSDL_LIBUDEV=OFF \
        -DSDL_DBUS=OFF \
        -DSDL_IBUS=OFF \
        -DSDL_HIDAPI_LIBUSB=OFF \
        -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF
    cmake --build "$BDIR/sdl3-build" --target SDL3-static --parallel "$JOBS"
fi
SDL3_INC="$EXTERN/sdl3/include"
SDL3_LIB="$BDIR/sdl3-build/libSDL3.a"

# 4. Cross-compile zlib, SLJIT, MAME cores, Model 2A runtime, Dear ImGui (SDL3+SDLGPU3), and Vulkan frontend
echo "[4/6] Cross-compiling ProjectDural Vulkan ($TARGET_TRIPLE, -mcpu=$TARGET_CPU, -j$JOBS)..."

ZLIB_SRCS=(
    adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c
    infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c
)
if [ ! -f "$BDIR/libz.a" ]; then
    printf '%s\n' "${ZLIB_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
        src="'"$ZLIB_DIR"'/$1"
        out="$2/$(basename "$1" .c).o"
        [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
        exec "$3" cc -target '"$TARGET_TRIPLE"' -mcpu='"$TARGET_CPU"' -O2 -w -DZ_HAVE_UNISTD_H -c "$src" -o "$out"
    ' _ {} "$BDIR/zlib" "$ZIG_BIN"
    "$ZIG_BIN" ar rcs "$BDIR/libz.a" "$BDIR/zlib"/*.o
fi

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O3 -w -DSLJIT_CONFIG_AUTO=1 \
    -c third_party/sljit/sljitLir.c -o "$BDIR/sljitLir.o"

CXX_FLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -std=c++20 -O3 -fno-fast-math -fno-math-errno -ffp-contract=off \
  -DVF2_VULKAN=1 \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function -Wno-attributes \
  -Isrc/core/shim_include -Isrc -Iplatform/arm64 -I$ZLIB_DIR -I$EXTERN/imgui -I$EXTERN/imgui/backends -I$SDL3_INC"

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
    [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
    extra=""
    case "$src" in
        third_party/*|src/host/stb_image_impl.cpp) extra="-w -O3 -fno-math-errno" ;;
        src/machine/model2_video.cpp|src/machine/model2_smooth.cpp) extra="-O3 -fno-math-errno -ffp-contract=fast" ;;
        *) extra="-O3 -fno-math-errno" ;;
    esac
    exec "$3" c++ '"$CXX_FLAGS"' $extra -flto=thin -c "$src" -o "$out"
' _ {} "$BDIR/rt" "$ZIG_BIN"
rm -f "$BDIR/libruntime.a"
"$ZIG_BIN" ar rcs "$BDIR/libruntime.a" "$BDIR/rt"/*.o "$BDIR/sljitLir.o"

UI_SRCS=(
    extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp
    extern/imgui/backends/imgui_impl_sdl3.cpp extern/imgui/backends/imgui_impl_sdlgpu3.cpp
)
printf '%s\n' "${UI_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
    exec "$3" c++ -target '"$TARGET_TRIPLE"' -mcpu='"$TARGET_CPU"' -std=c++20 -O2 -w \
        -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -I'"$EXTERN/imgui"' -I'"$EXTERN/imgui/backends"' -I'"$SDL3_INC"' -c "$src" -o "$out"
' _ {} "$BDIR/ui" "$ZIG_BIN"
rm -f "$BDIR/libimgui.a"
"$ZIG_BIN" ar rcs "$BDIR/libimgui.a" "$BDIR/ui"/*.o

APP_SRCS=(src/gpu/gpu3d_sdlgpu.cpp platform/arm64/gpu_vulkan.cpp platform/arm64/main_arm64.cpp)
printf '%s\n' "${APP_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    exec "$3" c++ '"$CXX_FLAGS"' -O3 -fno-math-errno -ffp-contract=fast -c "$src" -o "$out"
' _ {} "$BDIR/obj" "$ZIG_BIN"

# 5. Link vf2-vulkan.aarch64 statically against SDL3, ImGui, runtime, zlib
echo "[5/6] Linking vf2-vulkan.aarch64..."
"$ZIG_BIN" c++ -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O3 -flto=thin -fno-fast-math -ffp-contract=off \
    -o "$BDIR/vf2-vulkan.aarch64" \
    "$BDIR/obj"/*.o \
    "$BDIR/libruntime.a" "$BDIR/libimgui.a" "$SDL3_LIB" "$BDIR/libz.a" \
    -lm -ldl -lpthread

# 6. Verify ELF architecture, dynamic deps, glibc <= 2.28
echo "[6/6] Verifying AArch64 Vulkan binary and assembling ROM-free package..."
FILE_INFO=$(file "$BDIR/vf2-vulkan.aarch64")
echo "$FILE_INFO" | grep -q "ARM aarch64" || { echo "ERROR: not ARM aarch64 ($FILE_INFO)" >&2; exit 1; }
MAX_GLIBC=$(objdump -T "$BDIR/vf2-vulkan.aarch64" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
if [ -n "$MAX_GLIBC" ] && [ "$(printf '%s\n2.28\n' "$MAX_GLIBC" | sort -V | tail -1)" != "2.28" ]; then
    echo "ERROR: vf2-vulkan.aarch64 requires GLIBC_$MAX_GLIBC (max allowed 2.28)" >&2
    exit 1
fi
READELF_D=$(readelf -d "$BDIR/vf2-vulkan.aarch64")
if printf '%s\n' "$READELF_D" | grep -qE "libstdc\+\+|libz\.so|libSDL3"; then
    echo "ERROR: unexpected libstdc++.so, libz.so, or libSDL3.so dependency (must be statically linked)" >&2
    exit 1
fi
echo "  OK: vf2-vulkan.aarch64 -> ARM aarch64, max GLIBC_${MAX_GLIBC:-none}, zero libstdc++.so/libz.so/libSDL3.so"
readelf -d "$BDIR/vf2-vulkan.aarch64" | grep NEEDED

rm -rf "$OUT" "$ZIPOUT"
mkdir -p "$OUT/projectdural-vulkan/roms" "$OUT/projectdural-vulkan/saves" \
         "$OUT/projectdural-vulkan/licenses" "$OUT/projectdural-vulkan/assets/backgrounds"

cp "portmaster/Virtua Fighter 2 Vulkan.sh" "$OUT/"
chmod +x "$OUT/Virtua Fighter 2 Vulkan.sh"
cp portmaster/projectdural/gamecontrollerdb.txt "$OUT/projectdural-vulkan/"
cp portmaster/projectdural/PUT-ROMS-HERE.txt "$OUT/projectdural-vulkan/roms/PUT-ROMS-HERE.txt"
cp assets/backgrounds/select.png "$OUT/projectdural-vulkan/assets/backgrounds/"
cp "$BDIR/vf2-vulkan.aarch64" "$OUT/projectdural-vulkan/vf2-vulkan.aarch64"
chmod +x "$OUT/projectdural-vulkan/vf2-vulkan.aarch64"

cp LICENSE "$OUT/projectdural-vulkan/licenses/ProjectDural-MIT.txt"
cp third_party/mame/README.md "$OUT/projectdural-vulkan/licenses/MAME-BSD3-README.md"
cp third_party/sljit/LICENSE "$OUT/projectdural-vulkan/licenses/SLJIT-BSD2.txt"
[ -f "$EXTERN/sdl3/LICENSE.txt" ] && cp "$EXTERN/sdl3/LICENSE.txt" "$OUT/projectdural-vulkan/licenses/SDL3-LICENSE.txt"
[ -f "$EXTERN/imgui/LICENSE.txt" ] && cp "$EXTERN/imgui/LICENSE.txt" "$OUT/projectdural-vulkan/licenses/DearImGui-LICENSE.txt"
[ -f "$ZLIB_DIR/LICENSE" ] && cp "$ZLIB_DIR/LICENSE" "$OUT/projectdural-vulkan/licenses/zlib-LICENSE.txt"
cp portmaster/projectdural/README.md "$OUT/projectdural-vulkan/README.md"

if command -v zip >/dev/null 2>&1; then
    (cd "$OUT" && zip -qr9 "$ZIPOUT" "Virtua Fighter 2 Vulkan.sh" projectdural-vulkan)
else
    python3 -c "import shutil; shutil.make_archive('${ZIPOUT%.zip}', 'zip', '$OUT')"
fi
(cd dist && sha256sum ProjectDural-Vulkan-aarch64.zip > ProjectDural-Vulkan-aarch64.zip.sha256)
echo "Packaged: $ZIPOUT ($(du -h "$ZIPOUT" | cut -f1))"

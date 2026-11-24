#!/usr/bin/env bash
# Launcher for ProjectDural Vulkan (AArch64 / SDL3 SDL_GPU Vulkan / SPIR-V / SLJIT ARM64)
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$DIR/projectdural-vulkan" ]; then
    GAMEDIR="$DIR/projectdural-vulkan"
else
    GAMEDIR="$DIR"
fi

cd "$GAMEDIR"
exec ./vf2-vulkan.aarch64 --data-dir "$GAMEDIR" "$@"

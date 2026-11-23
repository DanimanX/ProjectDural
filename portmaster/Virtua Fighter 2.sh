#!/bin/bash
# PortMaster launcher for ProjectDural (Virtua Fighter 2 — Sega Model 2A Static Recompilation)
# Targeted at Anbernic RG34XX (Allwinner H700 / Mali-G31) and compatible AArch64
# Linux handhelds running muOS, Knulli, ROCKNIX, ArkOS, AmberELEC or Stock OS.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

GAMEDIR="$(cd "$(dirname "$0")" && pwd)/projectdural"
if [ ! -d "$GAMEDIR" ] && [ -n "$directory" ]; then
  GAMEDIR="/$directory/ports/projectdural"
fi
cd "$GAMEDIR" || exit 1

exec >> "$GAMEDIR/log.txt" 2>&1

if [ -n "$controlfolder" ] && [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  get_controls
fi

echo "=== ProjectDural (Virtua Fighter 2 ARM64 / OpenGL ES 2.0) ==="
echo "Date: $(date)"
echo "Device: ${DEVICE_NAME:-unknown}  CFW: ${CFW_NAME:-unknown}  Arch: $(uname -m)"

# System snapshot for support logs and performance triage: CPU clocks/governors,
# GPU devfreq clocks/governors, free memory and load. Read-only, never fatal.
{
  printf '[sys]'
  for d in /sys/devices/system/cpu/cpu*/cpufreq; do
    [ -d "$d" ] || continue
    f=$(cat "$d/scaling_cur_freq" 2>/dev/null || true)
    [ -n "$f" ] || f=0
    printf ' %s=%sMHz/%s' "$(basename "$(dirname "$d")")" "$((f / 1000))" \
      "$(cat "$d/scaling_governor" 2>/dev/null || echo -)"
  done
  for d in /sys/class/devfreq/*; do
    [ -d "$d" ] || continue
    f=$(cat "$d/cur_freq" 2>/dev/null || true)
    [ -n "$f" ] || f=0
    printf ' devfreq:%s=%sMHz/%s' "$(basename "$d")" "$((f / 1000000))" \
      "$(cat "$d/governor" 2>/dev/null || echo -)"
  done
  printf ' mem=%sMB load=%s' \
    "$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo 2>/dev/null)" \
    "$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null)"
  printf ' cores=%s online=%s\n' "$(nproc 2>/dev/null || echo ?)" \
    "$(cat /sys/devices/system/cpu/online 2>/dev/null || echo ?)"
}

export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$GAMEDIR/libs.aarch64"
if [ -n "$sdl_controllerconfig" ]; then
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
fi
if [ -z "$SDL_GAMECONTROLLERCONFIG" ]; then
  export SDL_GAMECONTROLLERCONFIG="19000000010000000100000000010000,Anbernic RG34XX Controller,platform:Linux,b:b3,a:b4,dpdown:h0.4,leftx:a0,lefty:a1,lefttrigger:b12,dpleft:h0.8,rightshoulder:b8,leftshoulder:b7,righttrigger:b13,dpright:h0.2,back:b9,start:b10,dpup:h0.1,y:b6,x:b5,guide:b11,"
fi

# Lock CPU and Mali GPU devfreq governors to performance while playing if permitted.
# Many CFWs run the port as an unprivileged user (Spruce on the TrimUI, for one), where the
# write simply fails: the [sys] line above then shows the clock the device actually ran at.
ORIG_GOV=""
if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    { echo performance > "$g"; } 2>/dev/null || true
  done
  for d in /sys/class/devfreq/*/governor; do
    [ -w "$d" ] && { echo performance > "$d"; } 2>/dev/null || true
  done
  if [ "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" != performance ]; then
    ORIG_GOV=""
    echo "[sys] CPU governor is read-only for this user; running on the device's own policy."
  fi
elif [ -n "$ESUDO" ] && command -v "$ESUDO" >/dev/null 2>&1 && [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
  $ESUDO sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor /sys/class/devfreq/*/governor; do [ -f "$g" ] && echo performance > "$g" 2>/dev/null || true; done' 2>/dev/null || true
fi

# Remote test hook: create "$GAMEDIR/autobench" holding a number of seconds (8 if empty)
# and the next launch sweeps per-poly scissor and render scale, tagging every [perf] line
# with the step that produced it. Nothing runs unless that file exists.
if [ -f "$GAMEDIR/autobench" ]; then
  DURAL_AUTOBENCH="$(tr -cd '0-9' < "$GAMEDIR/autobench" 2>/dev/null)"
  export DURAL_AUTOBENCH="${DURAL_AUTOBENCH:-8}"
  echo "[bench] autobench requested: ${DURAL_AUTOBENCH}s per step"
fi

pm_platform_helper "$GAMEDIR/vf2.aarch64" >/dev/null 2>&1 || true

./vf2.aarch64 --data-dir "$GAMEDIR" --fullscreen "$@"
RC=$?

if [ -n "$ORIG_GOV" ]; then
  if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      echo "$ORIG_GOV" > "$g" 2>/dev/null || true
    done
  elif [ -n "$ESUDO" ] && command -v "$ESUDO" >/dev/null 2>&1; then
    $ESUDO sh -c "for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do { echo '$ORIG_GOV' > \"\$g\"; } 2>/dev/null || true; done" 2>/dev/null || true
  fi
fi

pm_finish >/dev/null 2>&1 || true
exit $RC

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

# The port runs a 4-thread pipeline pinned to cores 0..3. Some firmwares boot with cores
# hotplugged off to save battery -- Spruce on the TrimUI Smart Pro comes up with
# `online: 0,3`, i.e. cores 1 and 2 down -- and then every thread collapses onto core 0
# and the game runs at roughly half speed (measured: 27 FPS vs 52 FPS). Bring the cores up
# for the session and put back offline the ones we started. The `online` files are usually
# root-owned and may be mode 0444, so they are chmod'ed wide open first and their modes are
# restored on exit. The result is read back rather than assumed, because firmware watchdogs
# can be racing the same writes.
GOV_FILES="/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
ONLINED=""
ONLINE_MODES=""
for c in 0 1 2 3; do
  node="/sys/devices/system/cpu/cpu$c/online"
  [ -f "$node" ] || continue
  chmod a+w "$node" 2>/dev/null || true
  [ "$(cat "$node" 2>/dev/null)" = "0" ] || continue
  for attempt in 1 2 3; do   # retry: a watchdog may be mid-write the first time
    { echo 1 > "$node"; } 2>/dev/null
    [ "$(cat "$node" 2>/dev/null)" = "1" ] && break
    sleep 0.2
  done
  if [ "$(cat "$node" 2>/dev/null)" = "1" ]; then
    ONLINED="$ONLINED $c"
    ONLINE_MODES="$ONLINE_MODES $node"
  fi
done

CORES_NOW=$(cat /sys/devices/system/cpu/online 2>/dev/null || echo "?")
NPROC_NOW=$(nproc 2>/dev/null || echo "?")
if [ "$NPROC_NOW" = "4" ]; then
  [ -n "$ONLINED" ] && echo "[sys] cores started for this session:$ONLINED (online=$CORES_NOW)"
else
  echo "[sys] only $NPROC_NOW core(s) online (online=$CORES_NOW): the 4-thread pipeline will share CPUs and run well below the intended frame rate."
fi

# Lock the CPU governor to performance while playing. `[ -w file ]` is not enough: as root it
# reports true while the write is still refused, because these sysfs files keep their 0444 mode
# and the kernel checks it regardless. Spruce's own cpu_control_functions.sh chmods them wide
# open first and puts the mode back after; the same is done here, and the mode is restored on
# exit along with the governor.
GOV_FILES="/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
GOV_SAVED=""
ORIG_GOV=""
for g in $GOV_FILES; do
  [ -f "$g" ] || continue
  GOV_SAVED="$GOV_SAVED $g:$(stat -c %a "$g" 2>/dev/null || echo 444)"
  chmod a+w "$g" 2>/dev/null || true
done
if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  ORIG_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
  for g in $GOV_FILES; do echo performance > "$g" 2>/dev/null || true; done
  for d in /sys/class/devfreq/*/governor; do
    [ -f "$d" ] || continue
    chmod a+w "$d" 2>/dev/null || true
    echo performance > "$d" 2>/dev/null || true
  done
  if [ "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" = performance ]; then
    echo "[sys] CPU governor locked to performance."
  else
    ORIG_GOV=""
    GOV_SAVED=""
    echo "[sys] CPU governor could not be changed; running on the device's own policy."
  fi
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

# Return to the firmware's boot state what this session changed: the cores it started, and
# each cpufreq file's mode as it was found.
for c in $ONLINED; do
  node="/sys/devices/system/cpu/cpu$c/online"
  { echo 0 > "$node"; } 2>/dev/null || true
done
for f in $ONLINE_MODES; do
  chmod 444 "$f" 2>/dev/null || true
done

if [ -n "$ORIG_GOV" ]; then
  for g in $GOV_FILES; do echo "$ORIG_GOV" > "$g" 2>/dev/null || true; done
fi
# Put back each cpufreq file's mode as it was found (Spruce locks them again).
for entry in $GOV_SAVED; do
  file="${entry%:*}"
  mode="${entry##*:}"
  [ -n "$file" ] && chmod "$mode" "$file" 2>/dev/null || true
done
for d in /sys/class/devfreq/*/governor; do chmod 444 "$d" 2>/dev/null || true; done

pm_finish >/dev/null 2>&1 || true
exit $RC

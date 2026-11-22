## Notes

Thanks to [mr-finster](https://github.com/mr-finster/vf2-recomp) for the original **vf2-recomp** Sega Model 2A static recompilation of *Virtua Fighter 2 (Version 2.1)*, the MAME team for the underlying Model 2A / i960 / MB86233 / SCSP hardware reference cores, and Zoltan Herczeg for SLJIT.

**ProjectDural** is a native ARM64 / OpenGL ES 2.0 PortMaster port engineered for full arcade speed (**57.52 Hz**) on budget quad-core Cortex-A53 handhelds such as the **Anbernic RG34XX** (Allwinner H700 + Mali-G31 MP2):

- **On-device ARM64 SLJIT Recompilation**: Translates the arcade Intel i960KB program directly into native AArch64 machine code in memory at boot (~50 ms). No PC pre-build step is required.
- **Hardware OpenGL ES 2.0 3D Renderer**: Perspective-correct 3D texturing, microtextures, checkering, luma/palette translation, and NEON RGB565 texture decoding on Mali-G31.
- **4-Core Asymmetric Pipeline**:
  - Core 0: OpenGL ES 2.0 3D/2D rendering + page flip
  - Core 1: i960 SLJIT + MB86233 TGP + Geometrizer
  - Core 2: Autonomous 68000 + SCSP 44.1 kHz audio worker
  - Core 3: Parallel 2D System 24 tilemap worker
- **3:2 & 16:9 Widescreen**: Seamless widescreen viewport expansion with painted character-select sides.

## Controls

| Button | Action |
| :--- | :--- |
| D-Pad / Left Stick | Move / Crouch / Jump |
| X | Guard (G) |
| A | Punch (P) |
| B | Kick (K) |
| Y | Guard + Punch (G+P) |
| R1 | Punch + Kick (P+K) |
| L1 | Guard + Kick (G+K) |
| R2 | Punch + Kick + Guard (P+K+G) |
| Start | Start (1P) |
| Select | Insert Coin / Service Combo |
| Start + Select (or Menu) | In-Game Settings, Display, Audio, Secrets & Cheats Menu |
| Select + Y | Cabinet Test Mode |
| Select + X | Service Coin |

## Installation (Bring Your Own ROM)

Zero copyrighted ROMs or ROM-derived binaries are included.

1. Copy your legally dumped MAME *Virtua Fighter 2 (Version 2.1)* ROM archive (`vf2.zip`) into:
   ```
   ports/projectdural/roms/vf2.zip
   ```
2. Launch **Virtua Fighter 2** from PortMaster / EmulationStation.

## Compiling

To cross-compile `vf2.aarch64` (targeting `aarch64-linux-gnu.2.28`, Cortex-A53, ThinLTO, OpenGL ES 2.0, SLJIT ARM64, NEON) and package the PortMaster archives:

```bash
bash tools/build-arm64.sh
```

This produces:
- `dist/projectdural.zip` (PortMaster installable package)
- `dist/PortMaster-New-PR/ports/projectdural/` (ready-to-submit folder for `PortMaster-New/ports/projectdural`)

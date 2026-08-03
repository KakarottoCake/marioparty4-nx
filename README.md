# Mario Party 4 — Nintendo Switch native port (work in progress)

An **experimental, incomplete** effort to turn Mario Party 4 into a **native**
Nintendo Switch homebrew app (`.nro`) — not an emulator.

It is built on top of the
[Mario Party 4 decompilation](https://github.com/mariopartyrd/marioparty4): because
the game is decompiled, the actual *game logic* is real C code compiled directly
for the Switch's ARM CPU. Only the platform layer (graphics, input, file IO, OS
services) is reimplemented on top of libnx.

The long-term goal is a clean port plus quality-of-life extras: native GameCube
controller support on Switch, and dynamic button glyphs that adapt to whichever
controller is in use.

> **Status: the real engine boots, native ANIM sprites work, and the first native
> HSF static-mesh/material-texture path runs in Eden.** The full visible game is
> not finished: HSF animation/TEV materials, audio, saves, UI glyph integration,
> and several overlays still need work.

This repository contains **no game assets** — an existing, legally-obtained copy of
the game is required to supply the data files.

---

## Current status

What works:
- Builds to a working `marioparty4_switch.nro`.
- Boots on Switch hardware (Atmosphère) and emulators (Eden/Ryujinx).
- Sets up an EGL + OpenGL ES 2 rendering context.
- Loads the game's data files from the SD card (virtual DVD/FST layer).
- Converts the big-endian, 32-bit-offset `ANIM` sprite format into native
  64-bit structures instead of mapping it directly over Switch memory.
- Decodes the GameCube tiled 2D texture formats used by sprites, including C4/C8
  palettes and TLUT uploads, and sends textured quads through OpenGL.
- Converts big-endian, 32-bit-offset HSF model tables into native structures and
  draws static triangle/quad meshes through the OpenGL path, including the first
  HSF bitmap/palette and depth-tested texture path.
- Provides real Switch system tick/time values and avoids the zero-byte audio
  staging allocation that corrupted the legacy heap.
- Reads up to four Switch controller slots, including native GameCube and Pro
  styles, with per-player button/stick state and a controller-aware glyph lookup.
- Statically links the `bootDll` overlay (the GameCube uses dynamically linked
  `.rel` overlays; this port links them in and calls their setup directly).
- Runs the engine's real boot sequence: `game_main` → `omMasterInit` →
  `bootDll` `ObjectSetup` → the `BootExec` process and main frame loop, with no
  allocator/free-list errors in the latest controlled Eden run.

What does **not** work yet:
- Complete visible boot artwork and menus still need validation and cleanup on
  hardware. The 2D path is wired, but it is not a finished renderer.
- HSF animation, multi-stage TEV materials, culling, and complete 3D scene
  rendering are not finished; no game board or minigame scene is validated yet.
- No audio or save data yet. Glyph lookup exists, but the original UI has not yet
  been fully wired to replace every on-screen button prompt.
- Only `bootDll` is wired up; every other overlay currently stub-links and does
  nothing.

---

## Building

All port code lives in `src/platform/switch/`.

Requires **devkitPro** with `devkitA64`, `libnx`, and the switch portlibs
(mesa/EGL: `libEGL`, `libGLESv2`, `libglapi`, `libdrm_nouveau`).

```
cd src/platform/switch
make            # runs prepare_headers.py, then compiles + links marioparty4_switch.nro
```

Notes:
- `make` first runs `prepare_headers.py`, which creates junctions under
  `build/include` pointing at the repo's `include/` folders.
- **Windows gotcha:** the devkitA64 toolchain reads the Windows `TMP` env var. If
  you build from a shell where `TMP` points at a non-writable directory you'll see
  `Cannot create temporary file in C:\WINDOWS`. Build from a shell with a writable
  `TMP` (e.g. `set TMP=%LOCALAPPDATA%\Temp` in cmd) before `make`.

---

## Running / testing

The game's data files are **not** embedded in the `.nro` — they are read as loose
files from the SD card. You need the extracted MP4 (USA rev0) `files/` folder
(the `data/`, `dll/`, `mess/`, ... subfolders) from your own copy of the game.

- **Eden / Ryujinx (emulator):** these load `.nro` homebrew directly (no keys, no
  firmware, no NSP needed). Put the game's `files/` folder at the root of the
  emulator's virtual SD card, then load `marioparty4_switch.nro`.
- **Switch hardware:** place the `.nro` and the `files/` folder together on the SD
  card and launch via the homebrew menu.

The asset root is auto-detected at startup (`romfs:/files`, then `sdmc:/files`,
then `files/` relative to the working directory) — see `dvd_switch.c`.

### Logs
Engine `OSReport` output is routed to (1) the system debug log
(`svcOutputDebugString`) — visible in Ryujinx and Eden's log window — and (2) a
file `mp4_log.txt` written to the SD card root. Watch for `Total files found: N`
(N==0 means the `files/` folder isn't where the port is looking).

---

## Architecture / file guide

Everything platform-specific lives in `src/platform/switch/`:

| File | Purpose |
|------|---------|
| `switch_main.c` | Entry point. Mounts romfs, inits controllers/DVD, hands the display from the boot console to GL, then calls `game_main`. |
| `sys_switch.c` | The big "stub layer": minimal/no-op implementations of the GameCube `GX`, `VI`, `OS`, `PAD`, `Hu*` engine calls so the game links. This is where most future work replaces stubs with real behavior. |
| `gfx_switch.c/.h` | EGL/GLES2 setup, framebuffer clear/present, and the 2D shader pipelines (solid + textured). |
| `gx_gl.c` | The `GX → OpenGL` translation layer (compiled with `-DTARGET_PC`): matrix math, immediate-mode vertex capture, GameCube texture decoding, and basic solid 3D draws. |
| `hsf_switch.c` | Safe big-endian/32-bit-offset HSF conversion, static mesh drawing, and the first bitmap/material texture hookup. Animation and full TEV materials are future work. |
| `dvd_switch.c` | Virtual DVD/FST: scans the asset folder and maps GameCube `DVD*` file calls to real files. Also hosts `OSReport` logging. |
| `controller.c/.h` | libnx pad reading for four players, controller-type detection, and the controller-aware button-glyph lookup. |
| `jmp_switch.s` | Small assembly shim. |
| `projection_override/` | Aspect-ratio override hook. |
| `prepare_headers.py` | Sets up `build/include` header junctions before compiling. |

Engine-side files compiled into the port (with Switch/`TARGET_PC` tweaks) include
`src/game/{main,memory,malloc,process,objdll,objmain,data,decode,dvd,ovllist,
hsfman,pad,sprman,sprput}.c` and `src/REL/bootDll/{main,language}.c`. See the
`src/platform/switch/Makefile` `OBJS` list.

---

## Known blockers

These are the main remaining issues between the current boot/2D progress and a
playable port. They come from moving a big-endian, 32-bit GameCube title to a
little-endian, 64-bit ARM device.

### 1. Endianness (big-endian data on a little-endian CPU)
The GameCube is big-endian; the Switch is little-endian. Every multi-byte value
the engine reads directly out of a data file must be decoded explicitly. Fixed so
far in `src/game/data.c`, `src/REL/bootDll/main.c`, and the native `ANIM` loader in
`src/game/sprman.c`. It still recurs in HSF models, message data, and other
formats. The decompression routines in `src/game/decode.c` are already endian-safe.

### 2. 32-bit vs 64-bit pointer layout
GameCube data files store pointers/offsets as **4 bytes**. The Switch is 64-bit, so
pointers in memory are **8 bytes**. Any struct the engine maps directly over file
data that *contains pointer fields* therefore has a **mismatched layout** — fields
after the first pointer land at the wrong offset, producing garbage and reads from
near-null addresses.

The sprite `ANIM` format and the static HSF geometry path are now handled by
dedicated loaders. HSF motion, cluster, shape, and matrix sections still need
the same treatment: parse their 32-bit file offsets, allocate native structures,
and relocate the pointers explicitly.

**You cannot fix this by compiling 32-bit — Switch homebrew (libnx) is 64-bit
only.** It has to be handled in software. Reasonable approaches:
- **Per-format loaders** (most robust): for each on-disk format, read the 32-bit
  big-endian layout explicitly and build a proper 64-bit struct in fresh memory.
- **32-bit offset fields + low heap** (less repetitive, riskier): redefine the
  file-struct pointer members as 32-bit offsets and keep the game heap inside the
  low 4 GB so addresses fit; access via base+offset.

The next major step is completing HSF animation, multi-stage materials, culling,
and the remaining 3D display-list translation layer.

---

## Roadmap (rough)
1. Validate and finish the 2D sprite path on hardware.
2. Finish HSF animation, multi-stage materials, culling, and remaining loaders.
3. Flesh out the remaining `GX → OpenGL` TEV/display-list behavior for complete 3D scenes.
4. Audio and save data.
5. Wire the dynamic glyph lookup through every original UI prompt.
6. Statically wire the remaining overlays.

---

## Credits
- Built on the [Mario Party 4 decompilation](https://github.com/mariopartyrd/marioparty4)
  by the mariopartyrd team — this fork only adds the `src/platform/switch/` layer
  plus the required `#ifdef __SWITCH__` compatibility code in shared engine files.
- Uses [devkitPro / libnx](https://devkitpro.org/).

Contributions welcome. This is early scaffolding, not a finished port.

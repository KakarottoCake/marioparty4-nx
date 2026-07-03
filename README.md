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

> **Status: boots and runs the real engine loop, but does not render game content
> yet (black screen).** It is shared as a starting point for anyone who wants to
> take it further. The [Known blockers](#known-blockers) section is the important
> part.

This repository contains **no game assets** — an existing, legally-obtained copy of
the game is required to supply the data files.

---

## Current status

What works:
- Builds to a working `marioparty4_switch.nro`.
- Boots on Switch hardware (Atmosphère) and emulators (Eden/Ryujinx).
- Sets up an EGL + OpenGL ES 2 rendering context (verified: a test quad rendered
  on screen).
- Loads the game's data files from the SD card (virtual DVD/FST layer).
- Statically links the `bootDll` overlay (the GameCube uses dynamically linked
  `.rel` overlays; this port links them in and calls their setup directly).
- Runs the engine's real boot sequence: `game_main` → `omMasterInit` →
  `bootDll` `ObjectSetup` → the `BootExec` process and main frame loop, with no
  crashes.

What does **not** work yet:
- **Nothing renders (black screen).** The GameCube `GX` graphics calls are mostly
  stubs. A partial `GX → OpenGL` translation layer exists for 2D sprites, but it
  is blocked by the data-layout issue described below.
- No audio, save data, or real controller mapping beyond a minimal stub.
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
| `gx_gl.c` | The `GX → OpenGL` translation layer (compiled with `-DTARGET_PC`): matrix math, immediate-mode vertex capture, and GameCube texture decoding. Used by the 2D sprite path. |
| `dvd_switch.c` | Virtual DVD/FST: scans the asset folder and maps GameCube `DVD*` file calls to real files. Also hosts `OSReport` logging. |
| `controller.c/.h` | Minimal libnx pad reading. (Intended home of the GameCube-controller + button-glyph QoL work.) |
| `jmp_switch.s` | Small assembly shim. |
| `projection_override/` | Aspect-ratio override hook. |
| `prepare_headers.py` | Sets up `build/include` header junctions before compiling. |

Engine-side files compiled into the port (with Switch/`TARGET_PC` tweaks) include
`src/game/{main,memory,malloc,process,objdll,objmain,data,decode,dvd,ovllist,
hsfman,pad,sprman,sprput}.c` and `src/REL/bootDll/{main,language}.c`. See the
`src/platform/switch/Makefile` `OBJS` list.

---

## Known blockers

These are the two systemic issues standing between "boots" and "renders". Both are
inherent to porting a big-endian, 32-bit GameCube title to a little-endian, 64-bit
ARM device.

### 1. Endianness (big-endian data on a little-endian CPU)
The GameCube is big-endian; the Switch is little-endian. Every multi-byte value the
engine reads **directly out of a data file** comes out byte-reversed. Fixed so far
at a few spots (search the tree for `__builtin_bswap`): `src/game/data.c`
`GetFileInfo` and `src/REL/bootDll/main.c` `NintendoDataDecode`. It recurs in every
data format's loader (sprite anim banks, HSF models, message data, etc.). The
decompression routines in `src/game/decode.c` are already endian-safe.

### 2. 32-bit vs 64-bit pointer layout (the current wall)
GameCube data files store pointers/offsets as **4 bytes**. The Switch is 64-bit, so
pointers in memory are **8 bytes**. Any struct the engine maps directly over file
data that *contains pointer fields* therefore has a **mismatched layout** — fields
after the first pointer land at the wrong offset, producing garbage and reads from
near-null addresses.

Concrete example (where sprite/logo rendering currently dies): `ANIMDATA` in
`include/game/animdata.h` is 20 bytes in the file (32-bit pointers) but 32 bytes
when compiled for 64-bit, so `HuSprAnimRead` in `src/game/sprman.c` reads its
fields at the wrong offsets and skips relocation.

**You cannot fix this by compiling 32-bit — Switch homebrew (libnx) is 64-bit
only.** It has to be handled in software. Reasonable approaches:
- **Per-format loaders** (most robust): for each on-disk format, read the 32-bit
  big-endian layout explicitly and build a proper 64-bit struct in fresh memory.
- **32-bit offset fields + low heap** (less repetitive, riskier): redefine the
  file-struct pointer members as 32-bit offsets and keep the game heap inside the
  low 4 GB so addresses fit; access via base+offset.

Getting one 2D format (the sprite/`ANIM` path) through this is the natural next
step and would put the first real game graphics (the boot logos) on screen.

---

## Roadmap (rough)
1. Solve the 32-bit/64-bit data-layout problem for the 2D sprite path → boot logos.
2. Fill in endianness fixes per subsystem as they're hit.
3. Flesh out the `GX → OpenGL` layer (TEV → shaders) and wire the 3D/HSF model path.
4. Audio, save data, real input mapping.
5. QoL: native GameCube controller support + dynamic button glyphs.
6. Statically wire the remaining overlays.

---

## Credits
- Built on the [Mario Party 4 decompilation](https://github.com/mariopartyrd/marioparty4)
  by the mariopartyrd team — this fork only adds the `src/platform/switch/` layer
  and minor `#ifdef __SWITCH__` tweaks to shared engine files.
- Uses [devkitPro / libnx](https://devkitpro.org/).

Contributions welcome. This is early scaffolding, not a finished port.

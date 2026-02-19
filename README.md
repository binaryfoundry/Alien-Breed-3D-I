# Alien Breed 3D PC Port

A **C port** of **Alien Breed 3D** (Amiga), translating the original 68000 assembly into C and rendering with SDL2 on Windows, Linux, and macOS. The game logic, level format, and data layout follow the Amiga version; assets must be extracted from the original game.

---

## Repository layout

- **Root** – PC port (C + SDL2): `src/`, `data/`, `tools/`. Build from here with CMake; game data goes in `data/`.
- **`amiga/`** – Original Amiga 68000 assembly (`.s`), includes, and reference assets (e.g. `amiga/pal` is copied to `data/pal` at configure time).

---

## What this project is

- **Source**: The Amiga game was written in 68000 assembly (`.s`). This repo contains both the original assembly (for reference) in **`amiga/`** and a **PC port in C** at the repository root.
- **Port**: The C code in **`src/`** reimplements the renderer (wall/floor/ceiling texturing, sprites), movement, AI, objects (doors, lifts, switches), and game loop. Data files are the same format as the Amiga (big-endian where applicable).
- **Runtime**: The port uses **SDL2** for video, input, and (stub) audio. No original Amiga binaries are executed.

You need **game data** from the original Alien Breed 3D (e.g. from the game’s ADF disk images). The port does not ship any copyrighted assets.

---

## Extracting game data from ADF files

Game data comes from the **original Alien Breed 3D** release (e.g. Amiga ADF images). You must own or have the right to use the game.

### 1. Getting files out of the ADF

Use **unadf** to extract the contents of the game’s ADF image(s). For example:

```bash
unadf game.adf
```

This extracts the disk contents into the current directory. Copy or rename the extracted files so they match the layout under **`data/`** (at the repository root) in the next section.

### 2. Where to put the data

Place all game data in **`data/`** (at the repository root). Create this structure inside **`data/`**:

| Path | Description |
|------|-------------|
| **`includes/`** | Sprites and gun: `.wad`, `.ptr`; gun also uses `.pal` from `includes/` or `pal/`. |
| **`levels/`**, **`level_a/`**, … | Level data for level 0, 1, … (see below). |
| **`sounds/`** | All sounds. |

**Wall textures** in `includes/walls/` (exact filenames):

`GreenMechanic.wad`, `BlueGreyMetal.wad`, `TechnoDetail.wad`, `BlueStone.wad`, `RedAlert.wad`, `rock.wad`, `scummy.wad`, `stairfronts.wad`, `BIGDOOR.wad`, `RedRock.wad`, `dirt.wad`, `switches.wad`, `shinymetal.wad`, `bluemechanic.wad`.

Each `.wad` can be raw or **=SB= compressed**; the loader decompresses automatically.

**Levels** (per level index 0, 1, … → `level_a`, `level_b`, …):

- **`levels/level_a/twolev.bin`** – level data
- **`levels/level_a/twolev.graph.bin`** – level graphics (walls, floors, roofs)
- **`levels/level_a/twolev.clips`** – clip data

If a level file is missing, the port falls back to a small procedural test level.

**Gun**: `includes/newgunsinhand.wad`, `includes/newgunsinhand.ptr`, and a palette (`pal/newgunsinhand.pal` or `includes/newgunsinhand.pal`). If the palette is missing, a default grayscale one is used.

---

## Build

### Requirements

- **CMake** 3.16+
- **C11** compiler (MSVC, GCC, Clang)
- **SDL2** – fetched automatically by CMake (FetchContent)

### Steps

From the **repository root**:

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

On Windows with Visual Studio you can open the generated solution in `build/` and build from the IDE. The executable is typically `build/Release/ab3d1.exe` (Windows) or `build/ab3d1` (Unix).

---

## Run

Put game data in **`data/`**. Run the executable from the repository root so it can find `data/`:

```bash
cd path/to/Alien-Breed-PC
build/Release/ab3d1.exe    # Windows
./build/ab3d1              # Linux/macOS
```

If data is not found you'll see `[IO] WARNING: Could not locate data/ directory`. Ensure **`data/`** exists and contains at least `includes/floortile` (and the other files you need).

Without real level data, the port still runs and uses the built-in test level so you can test the renderer and controls.

---

## Controls (typical)

- **WASD** – movement (press **N** to activate on the menu if required)
- **Mouse / arrow keys** – look / turn
- **Space** – use (doors, switches)
- **ESC** – exit to shell on the main menu
- **TAB** – level selection on the main menu (level passwords can be saved on exit)

---

## References

- Original Team 17 sources: [alienbreed3dii](https://github.com/videogamepreservation/alienbreed3dii.git)
- RTG version: [ab3d-rtg](https://github.com/mheyer32/ab3d-rtg)
- VS Code Amiga Assembly extension: [prb28.amiga-assembly](https://marketplace.visualstudio.com/items?itemName=prb28.amiga-assembly)

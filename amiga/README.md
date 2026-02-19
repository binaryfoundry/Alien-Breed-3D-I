# Amiga sources and assets

This directory holds the **original Amiga** 68000 assembly (`.s`), includes, and reference assets for Alien Breed 3D I.

- **Assembly** – For reference only; the playable port is the C code at the repository root.
- **`amiga/pal/`** – Palette files used by the PC port. At configure time, CMake copies this into `data/pal` at the repo root so the build can generate sprite palette headers.

For extracting game data from ADFs, building the PC port, running, and controls, see the main [README.md](../README.md) in the repository root.

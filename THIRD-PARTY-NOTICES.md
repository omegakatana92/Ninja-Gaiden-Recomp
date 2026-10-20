# Third-party notices

The Ninja Gaiden recompilation launcher and the game's build files are
distributed under the PolyForm Noncommercial License 1.0.0 (see `LICENSE`).
They contain or use the following third-party work.

---

## NESRecomp - the recompilation framework

* Project: <https://github.com/mstan/nesrecomp>
* Author: Matthew Stanley
* License: PolyForm Noncommercial License 1.0.0
  (<https://polyformproject.org/licenses/noncommercial/1.0.0>)
* What is used here: the cycle-accurate backend - the 6502 core
  (`runner/cyc/cpu6502.*`), the PPU, the APU, the cartridge boards
  (`hw_mapper.c`), the window, audio and input (`cyc_sdl.c`, `cyc_input.c`,
  `cyc_settings.c`) and the build glue (`runner/cyc/cyc.cmake`).
* The release package keeps the framework's own license at `engine/LICENSE`.

The framework's cycle-accurate backend is derived in part from **TriCNES**,
the reference NES implementation used as the oracle the recompiled program is
checked against.

## SDL2

* Project: <https://www.libsdl.org/>
* License: zlib
* What is used here: the game's window, audio device and input, and the
  launcher's controller and key names.
* The notice ships as `engine/runner/external/SDL2/COPYING.txt`.

## The Microsoft Visual C++ runtime

* The release package stages the redistributable DLLs of the Visual C++
  runtime next to the launcher, so it runs on a machine without the
  redistributable installed.  They are covered by the Microsoft Visual Studio
  2015-2022 redistributable license, and are not part of this project's
  license.

---

## What is *not* included

* No part of *Ninja Gaiden* (Tecmo, 1988): no ROM image, no graphics or
  music data, no tiles, palettes or level data, and nothing derived from
  them.  The player supplies their own legally obtained copy, and the
  launcher builds the game from it locally.
* The C source and the executable produced from a ROM are ROM-derived and
  are therefore not part of this repository or of the release package.

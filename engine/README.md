# engine

This directory holds the C11 source files of the engine:

- `src/`: one directory for each module (`core`, `fmt`, `plat`, `vm`, `gfx`, `txt`, `snd`, `ui`,
  `sav`, `app`);
- `third_party/`: the vendored stb files, and the fallback Japanese font (`fonts/`, Kosugi,
  Apache-2.0). The Makefile links the font into the engine (`sayopack cembed`). The engine uses the
  font when the system has no Japanese font;
- `tools/sayopack.c`: the payload pack tool.

The top-level `Makefile` builds all of it (`make help`). The Makefile compiles each `src/*/*.c`
file. Thus a new file needs no change to the build.

The engine reads all game files through the layered data file system in `src/plat/fs.h` (see
[`../docs/PAYLOAD.md`](../docs/PAYLOAD.md)). In the self-contained build, the game files are in the
payload of the executable. Thus, do not open game files with OS paths.

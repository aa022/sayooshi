# Game data layer and the SAYOPAK payload

The engine does not read game files through OS paths. It reads them through a small, read-only file
system with layers. The code is in `engine/src/plat/fs.c`, and the API is in `engine/src/plat/fs.h`.
This file system lets one executable hold all the game data. It also puts the English patch on top
of the Japanese original.

Terms in this document:

- **Data tree.** The canonical directory tree of game data (section 1).
- **Payload.** The SAYOPAK bytes that hold a copy of the data tree (section 4).
- **Archive.** A file that holds a payload: a `.pak` file, or an executable with a payload.
- **Layer.** A directory or an archive that the engine mounts (section 2).

## 1. Canonical data tree

`make data` builds this tree in `build/data/`. The payload holds exactly the same tree.

```
ja/Scene.pck  ja/Gameexe.dat  ja/dat/MODE.CGM  ja/g00/*.g00  ja/bgm/*.nwa  ja/wav/*.nwa  ja/koe/*.ovk
                                       original GameData (SiglusEngine.exe, savedata/ and ADIEU.ico are left out)
ja/font.ttf                            optional: make data FONT=/path/to/japanese.ttf
en/Scene.pck  en/Gameexe.dat           EN patch v1.1 (from Scene.pck.new / Gameexe.dat.new in the NSIS installer)
en/g00/*.g00                           116 replacement images (from $R8/*.g00.new)
en/font/sayooshieng-Regular.ttf        EN patch font
en/routeguide.txt                      EN patch route guide (informational)
manifest.txt                           sources (zip names + sha256), languages, and "sha256 size path" per file
```

The build uses only one file outside the tree: `ADIEU.ico`. The build copies it to
`build/icon/ADIEU.ico`. This file becomes the Windows resource icon and the macOS `.icns`.

## 2. Layers, lookup and languages

- **Layer.** A layer is a directory or a SAYOPAK archive. The engine mounts it at a prefix of the
  canonical tree.
  - `fs_mount_dir(dir, NULL)` finds the prefix automatically. A directory that contains `ja/` is a
    data tree (prefix `""`). The engine treats all other directories as a plain original GameData
    directory (prefix `ja/`).
  - `fs_mount_archive(path, NULL)` mounts a `.pak` file or an executable with a payload.
    `path = NULL` means the executable that runs.
- **Lookup.** Lookup is not case-sensitive, and `'\\'` counts as `'/'`. The layer that the engine
  mounted last wins.
- **Visible tree.** The engine reads paths such as `Scene.pck` or `g00/BG_RD.g00`. The engine finds
  these paths in the visible tree: first in `<lang>/`, then in `ja/`.
  - `fs_set_language("en")` turns on the EN overlay. `"ja"` means no overlay.
  - `fs_has_language(l)` is true when `l/Scene.pck` exists.
  - A path that starts with `/` refers directly to the canonical tree, for example `/en/Scene.pck`.
- **File read.** Use `fs_open` or `fs_read_all`. `fs_open` gives a read-only `SDL_IOStream`.
  `fs_read_all` gives the full file in memory from malloc.
  - The legacy function `fs_path()` gives a virtual path such as `vfs:g00/BG_RD.g00`. It does not
    give an OS path. `fs_read` and `fs_open` accept this virtual path.
  - `fs_list(subdir, ext)` lists the visible directory. It merges `ja/` and the overlay.

## 3. Discovery at startup (`fs_discover`)

The engine looks for the game data in this order:

1. **Explicit source.** This is `--gamedata DIR`, or the old positional `<GameData>` argument. It
   can be a plain GameData directory (= `ja/` only), a data tree with `ja/` and `en/`, or a `.pak`
   file. If the engine cannot mount it, startup fails. The engine does not try other sources.
2. **Payload appended to the executable that runs** (Linux and Windows dist builds). After that,
   `game.pak` in `SDL_GetBasePath()`. In a macOS bundle, that path is `Contents/Resources/`.
3. **`data/`** next to the executable.
4. **Dev fallback.** `<exe dir>/../data`, `<exe dir>/../../build/data`, then `./build/data`. These
   are the trees that `make data` writes. `make run` and the dev binary `build/<platform>/sayooshi`
   use these trees.

The engine uses sources 2–4 only if they contain `ja/Scene.pck`.

- **Language.** `--lang ja|en` selects the language.
  - Without `--lang`, a run with a window shows the start-up language selector. It shows the
    selector only when the data has `en/Scene.pck`. The selector preselects the last choice from
    `config.sav`.
  - Without `--lang`, a headless run uses that saved choice. If there is no saved choice, it uses
    `ja`.
  - The config dialog can change the language at run time. Then main.c loads `Scene.pck`,
    `Gameexe.dat`, `MODE.CGM`, the g00 cache and the font again in the other language. The game
    then starts again at `#START_SCENE`.
- **Diagnostics.** `--data-info` prints the selected source. It also prints which canonical file
  supplies some important paths. `--verify-data` checks each file in the archive against its CRC-32.

## 4. SAYOPAK format (version 1)

All integers are little-endian. "Offset" means an offset from the **payload start**. The payload
start is `file_size - payload_size`. Thus the same bytes work as a stand-alone `game.pak` and as a
payload appended to an executable. The constants are in `engine/src/fmt/sayopak.h`.

```
payload := header  data  table  trailer

header   16 bytes   char magic[8] = "SAYOPAKH"; u32 version = 1; u32 reserved = 0
data                each file stored uncompressed (g00/nwa/ovk are already compressed), starting at a
                    16-byte aligned offset; zero padding between files
table               one entry per file, in the order of the pack list (sorted, LC_ALL=C):
                      u64 offset; u64 size; u32 crc32; u16 path_len; u16 flags = 0; char path[path_len]
                    path: UTF-8, '/'-separated, relative, no empty / "." / ".." components, < 1024 bytes
trailer  48 bytes   u32 version = 1; u32 count; u64 table_offset; u64 table_size; u32 table_crc32;
                    u32 flags = 0; u64 payload_size (header..trailer inclusive); char magic[8] = "SAYOPAK1"
```

- **CRC.** The CRC-32 is the IEEE/zlib CRC-32 (`crc32(0, data, n)`).
- **Mount checks.** The engine reads the last 48 bytes of the file. It checks the trailer magic, the
  version, the sizes and `table_offset + table_size + 48 == payload_size`. Then it checks the header
  magic at the payload start and the table CRC.
- **Entry checks.** Each entry must be in the data area and must have a safe path. If an entry does
  not obey these rules, the mount stops at that entry.
- **File CRCs.** The engine checks these only on request (`--verify-data`, `sayopack verify`). It
  does not check them at each start, because a read of 565 MB at each start is slow.
- **Entry read.** The engine opens the archive file again. It puts the range of the entry in a
  sub-range `SDL_IOStream`. Thus readers that operate at the same time do not share a file position.

### Why the payload is appended

- **Linux (ELF) and Windows (PE).** Both formats ignore data after the last section. PE calls such
  data an "overlay". The loader maps only the sections. Thus the 565 MB payload uses no memory until
  the engine reads a file.
  - Strip the binary *before* you append the payload. `strip` writes the file again and removes
    the overlay.
  - Do Authenticode signing before you append the payload.
- **macOS.** The build never appends the payload to the Mach-O file. Appended data breaks code
  signing, and Apple Silicon requires at least an ad-hoc signature. Instead, `Sayonara.app` keeps
  the payload as `Contents/Resources/game.pak`. The bundle signature covers that file, and discovery
  step 2 finds it.

## 5. Host tool: `engine/tools/sayopack.c`

This tool is plain C11 with stdio and the vendored `stb_image_write`. The Makefile builds it for the
build host as `build/host/sayopack`.

```
sayopack create OUT.pak ROOT LIST     pack the files named in LIST (paths relative to ROOT)
sayopack append EXE PAK OUT           OUT = EXE (minus any old payload) + PAK
sayopack strip  EXE OUT               OUT = EXE without its payload
sayopack list   FILE                  show the table of a .pak / an executable with a payload
sayopack verify FILE                  check all CRC-32s (exit 1 on damage)
sayopack icns   ICO OUT.icns          PNG-based icns (16/32/64/128/256) from the largest .ico image
sayopack png    ICO OUT.png           the largest .ico image as PNG
sayopack cembed FILE OUT.c NAME       FILE as a C byte array NAME, and its size as NAME_size
```

The ICO decoder supports BMP/DIB entries (1/4/8/24/32 bpp, AND mask). For `png` only, it also
accepts PNG-compressed entries. `ADIEU.ico` holds 48, 32 and 256 px images. All of them are 24-bit
DIBs.

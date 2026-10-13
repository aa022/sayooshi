# Save data format (format 2)

This document defines the files that the engine writes for save slots, global data, configuration
and save bundles. It is the reference for `engine/src/sav/ (sav_stream.c, sav_file.c, sav_snap.c, ...)`
and for the `*_save` / `*_load` functions of the modules.

Terms in this document:

- **Save file.** One file that the engine writes in the save directory (a slot, `global.sav`,
  `config.sav`, or one of their versions).
- **Save directory.** The directory that holds all save files (§1.1).
- **Slot.** A save file of kind `SLOT`.
- **Bundle.** One file that holds copies of save files, for export and import (§8.4).
- **Chunk.** A tagged block of bytes with a length and a CRC-32 (§3).
- **Reader** and **writer.** Code that decodes and encodes save files.

The design goals are:

- **Identical bytes on all platforms.** The writer writes each value byte by byte in a defined
  order. No value depends on the endianness, word size, struct layout or float format of the host.
- **The engine always detects damage.** Each chunk has a CRC-32, and the full file also has one.
- **Writes are atomic.** The engine keeps the previous version as a backup.
- **Old and new engines can read the files of the other**, within the rules in §4.
- **Files are small.** A slot is approximately 25 KB. In format 1, a slot was approximately 520 KB.

## 1. Files and locations

All save files are in one directory. The file names are lowercase ASCII. They are the same on all
platforms:

| File | Kind | Contents |
|---|---|---|
| `slot_000.sav` … `slot_099.sav` | `SLOT` | save slots 0–99 (#SAVE.CNT) |
| `quick_0.sav` … `quick_2.sav` | `SLOT` | quick save slots (engine slots 100–102) |
| `global.sav` | `GLOB` | global data: G/Z/M/＊namae banks (endings, `G[98]` …), read flags, CG flags |
| `config.sav` | `CONF` | user settings |
| `*.bak` | same | the previous valid version of that file (§3) |
| `*.tmp` | same | a complete new version left behind if the engine stopped between write and rename (§3) |
| `*.v1` | format 1 | a one-time copy of a format-1 file, made when it was converted (§10) |
| `*.corrupt-<unixtime>` | — | an unusable file moved aside so that nothing overwrites it (§3) |
| `backup_YYYYMMDD_HHMMSS/` | — | the files that `--import-saves` replaced |

### 1.1 Save directories

The engine selects the save directory in this order:

1. `--save-dir DIR`, if you give it.
2. **Portable mode.** If a folder with the name `saves` is next to the executable, the engine uses
   it. If a file with the name `portable.txt` is there, the engine creates `saves/` and uses it.
   With portable mode, the game can run from a USB stick, or from a folder that you copy between
   machines.
3. In all other cases, `SDL_GetPrefPath("sayooshi", "sayooshi")`. That is:
   - `~/.local/share/sayooshi/sayooshi/` on Linux;
   - `%APPDATA%\sayooshi\sayooshi\` on Windows;
   - `~/Library/Application Support/sayooshi/sayooshi/` on macOS.

`sayooshi --print-save-dir` prints the save directory that the engine selected.

### 1.2 Move save files between machines

To move save files to a different machine, copy the save directory, or use a bundle:

- `sayooshi --export-saves FILE` writes one bundle file (§8.4). The bundle holds the best valid
  version of each save file.
- `sayooshi --import-saves FILE` reads a bundle and validates all of it first.
  - If an entry is damaged, the import changes nothing.
  - If all entries are valid, the import copies each file that it will replace into
    `backup_<date>/`. Then it writes the files of the bundle atomically.
  - The import does not change files that are not in the bundle. Thus the import merges at the
    file level.

These three options obey `--save-dir`. They do not need the game data.

## 2. Encoding primitives

| Name | Encoding |
|---|---|
| `u8` | 1 byte |
| `u16`, `u32`, `u64` | unsigned, little-endian, 2/4/8 bytes |
| `i32`, `i64` | two's complement, little-endian, 4/8 bytes |
| `bool` | `u8` 0 or 1. Readers treat any non-zero value as true. Inside module chunks some flags are written as `i32` 0/1. |
| `str16` | `u32 n`, then `n` UTF-16LE code units (game text, scene names) |
| `str8` | `u32 n`, then `n` bytes of UTF-8 (identifiers, keys, versions) |
| `fourcc` | 4 ASCII bytes, in file order (`u32` value `a | b<<8 | c<<16 | d<<24`) |

The format contains no floating-point values, pointers, `size_t`, enum layouts or padding. Each
count is an explicit `u32` or `i32`. Before a reader allocates memory, it checks each count against
the bytes that remain in the chunk.

### 2.3 Zero-run-length i32 arrays (`rle32`)

The format uses this encoding for flag banks and other int lists. Most values in these lists are
zero.

```
u32 n                       number of values
repeat until n values are produced:
    u32 zeros               this many 0 values
    u32 lit                 then this many literal values:
    i32 value[lit]
```

These segments are errors:

- a segment with `zeros = lit = 0`;
- a segment that makes more than `n` values.

The writer of the engine stops a literal run at the first pair of consecutive zeros. Readers must
accept all valid segmentations.

### 2.4 Sparse string lists (`sstr`)

```
u32 n                       list length
u32 m                       number of non-empty entries
m x { u32 index; str16 s }  indices strictly increasing, < n
```

### 2.5 CRC-32

This is the IEEE 802.3 / zlib CRC-32: reflected polynomial `0xEDB88320`, initial value
`0xFFFFFFFF`, final XOR `0xFFFFFFFF`. The CRC of `"123456789"` is `0xCBF43926`.

## 3. File layout and durability

```
offset 0   8 bytes  magic "SAYOSAVE"
offset 8   u16      format major version (2)
offset 10  u16      format minor version (0)
offset 12  fourcc   kind: SLOT, GLOB, CONF, BNDL
offset 16  chunks…
           END chunk (last 20 bytes)
```

A chunk is:

```
fourcc tag
u32    length of body
u32    CRC-32 of body
body   (length bytes)
```

Some chunks are **containers**: their body is also a chunk sequence. The sections for each file
kind (below) tell which tags are containers. Two types of sequence must fill their range exactly,
with no gap and no overlap:

- the sequence in a container;
- the top-level sequence from offset 16 to the END chunk.

The `END` chunk is always the last 20 bytes. It contains `"END "`, length 8, the CRC of its body,
then a body of `u32 offset_of_END` and `u32 crc32(bytes [0, offset_of_END))`. The END chunk proves
that the file is complete and has no damage. A truncated file fails this check, also when the cut is
at a chunk boundary. A file with a changed bit also fails this check.

**Validation on load.** A file is usable only if all of these conditions are true:

- the magic is correct;
- the major version is 2;
- the END chunk and the CRC of the full file match;
- each top-level chunk fills its range correctly and its CRC matches;
- the kind is the kind that the file name expects.

Then each module checks its own values. The engine never trusts a count or index that it did
not check.

**Atomic write.** For each file, the engine does these steps:

1. It writes `name.tmp`, calls `SDL_FlushIO` and closes the file. On Linux, SDL 3.2 calls
   `fdatasync`. On Windows, SDL 3.2 calls `FlushFileBuffers`.
2. It reads `name.tmp` back. It compares the data with the data that it wrote.
3. If `name` exists and is valid, it renames `name` to `name.bak`. If the main file is damaged, or
   if it comes from a newer major version, it renames the file to `name.corrupt-<unixtime>`. The
   engine never deletes this file, and this file never replaces a good backup.
4. It renames `name.tmp` to `name`. `SDL_RenamePath` replaces the target atomically: `rename(2)` on
   POSIX, `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` on Windows. If the rename fails, the engine
   removes the target and tries again.

At all times, at least one complete valid version exists. If a crash occurs between steps 3 and 4,
the new data stays in `.tmp` and the old data stays in `.bak`.

The engine does not sync the directory, because SDL has no function for it. Thus, after a power
loss on some file systems, the rename in step 4 can be lost. In that case, the previous version
stays in place.

**Read.** The engine reads the first valid version of `name`, `name.tmp`, `name.bak`, in that
order. If it uses a version other than `name`, it logs a warning. If `global.sav` exists in some
form but no version of it is usable, the engine renames all versions to `*.corrupt-<time>`. This
prevents the next write from rotating them away. Then the engine starts with empty global data.

## 4. Versioning and compatibility rules

- **Major version** (header): only for changes that are not compatible. A reader refuses all other
  major versions and reports this.
- **Minor version** (header): for compatible additions. Readers ignore it.
- **Unknown chunks**: readers skip them at all levels. In `global.sav`, the engine also keeps
  unknown top-level chunks and writes them back without change (§8.2). Thus an older engine does
  not erase data that a newer engine added.
- **Fields inside a chunk**: new fields are only added at the end.
  - A reader ignores trailing bytes that it does not know (forward compatibility).
  - When a chunk stops before an optional field, the reader uses the default of that field
    (backward compatibility).
- A module chunk gets a new tag when its layout changes in a way that is not compatible (`VM01`
  became `VM02`, `GFX0` became `GFX1`). New engines continue to read the old tags (§10).
- Presence bit masks (§9.2) reserve their unused bits.
  - In the event-field, object-scalar and effect-scalar masks, a reader skips one `i32` for each
    unknown bit. Thus each future field there must be an `i32`.
  - In the object and effect event masks, one event follows an unknown bit. The reader decodes this
    event and discards it.
- `config.sav` is a key/value list (§8.3). Readers ignore unknown keys. Missing keys keep their
  defaults. Readers clamp values to their ranges.

## 5. Scene code hashes and languages

A save records the **language** of the game text that was active when the save was made. This is
`LANG`, from `save_language()`: the active data overlay, `"ja"` or `"en"`.

A save also records a **code hash** for each scene on the saved call stack. The hash of one scene
is FNV-1a 64 (offset `0xcbf29ce484222325`, prime `0x100000001b3`) over this byte stream. All
numbers are `u32` little-endian:

```
"SCN1", code_size, code bytes,
nlabel, label[nlabel] (i32),
1000, zlabel[1000] (i32)         (0 entries if the scene has no z-label table),
nscn_cmd, scn_cmd[] (i32),
nprop, nprop x (form, size),
nread_flag,
2*ncmd_label, cmd_label[] (i32 pairs)
```

The hash does not include the string table and the names. Thus a translated pack with identical
bytecode has the same hash.

The **pack hash** is FNV-1a 64 over this data, in this order:

1. `"PCK1"` and `nscene`;
2. for each scene, its name (`u32` length + UTF-16LE units) and its hash as two `u32` (low, high);
3. `ninc_cmd` and the `(scene, offset)` pairs.

**Load rule.** The engine can load a slot only if each scene on its call stack exists in the current
pack with the same code hash.

- This rule applies for all languages.
- If the languages are different but the hashes match, the engine loads the slot. This is true for
  the 150 scenes whose bytecode is identical in JA and EN.
- If a hash is different, the engine refuses the load and shows a message. An example message is
  "made with the 'en' text; scene seen020 differs in the current 'ja' text".
- The load screen shows such a slot in grey, with that message.
- A refused load does not change the game that runs.

The hashes of the other scenes do not prevent a load. The engine stores their scene props with
their own hash (§9.1). It applies these props only when that hash matches.

**Global data across languages.**

- **Read flags**: the engine stores them for each scene as (name, code hash, flags).
  - A scene whose code is identical in the two languages shares one set of flags, because its lines
    are the same one to one.
  - A scene whose code is different keeps one set for each code version.
  - The engine keeps the set for the code that is not loaded without change, and writes it back.
    Thus, if you change the language many times, no flags are lost.
- **CG flags, endings and all G/Z/M bank values**: the two languages share them, by index.
- The engine merges CG flags on load. A flag that is set stays set.

## 6. SLOT files

Kind `SLOT`. The top-level chunks are in this order:

| Chunk | Content |
|---|---|
| `META` (container) | `GAME` str8 `"sayooshi"` · `ENGV` str8 engine version · `LANG` str8 · `TIME` i64 seconds since 1970 (UTC clock) · `PACK` u64 pack hash + u32 nscene · `TITL` str16 slot title (root 74) · `MESG` str16 message preview · `STAK` call stack: `u32 n`, `n x (str16 scene name, u64 code hash)`, outermost first, current scene last |
| `THMB` | `u32 w, u32 h, u32 encoding` (0 = RGB565), then `w*h` `u16` LE pixels `rrrrrggggggbbbbb`, rows top to bottom. The engine writes 128x72; readers accept 1..512 in each dimension. The colour expands back to 8 bits by bit replication (`r8 = r5<<3 | r5>>2`). |
| `SNAP` (container) | the snapshot, see below |
| `END` | |

The **snapshot** is a chunk sequence. The engine also keeps it in RAM at each
`msg_block`/`savepoint`:

| Chunk | Content |
|---|---|
| `SHDR` | `i64 time`, `str16 title`, `str8 lang`, then the call-stack record (as `STAK`) |
| `VM  ` (container) | `VM02` (§9.1) |
| `PROC` (container) | `PRC1` (§9.4) |
| `SYS ` (container) | `SYS1` (§9.4) |
| `GFX ` (container) | `GFX1` (§9.2) |
| `MWND` (container) | `MWND` (§9.4) |
| `AUD ` (container) | `AUD1` (§9.4) |
| `BLOG` (container) | `BLOG` (§9.4) |
| `BTNG` (container) | `BTNG` (§9.4) |

`VM` and `PROC` are mandatory. If one of them does not load, the engine refuses the load and
restores the previous game state. The other modules are optional. If one of them does not load, the
engine resets that module and continues the load.

Typical sizes, measured for seen101 at TEXT 40: the snapshot is 5.5 KB, the thumbnail is 18.4 KB,
and the full slot file is approximately 24.9 KB.

## 7. Partial failure and global data

The engine loads `global.sav` one chunk at a time:

- The engine does not apply a chunk that it cannot decode. The module decodes into temporary
  storage. It copies the data to the module only when the full chunk is good.
  - The engine also keeps the raw chunk and writes it back without change at the next save.
  - Thus the engine never replaces stored global data with empty data because of a decode error.
  - In that case, the engine does not save progress in that category from that session until
    someone corrects the problem.
- The engine keeps unknown top-level chunks and writes them back in the same way.
- If the file is damaged, the engine uses `.tmp`, then `.bak` (§3). The engine starts with empty
  data only when no version is usable. It keeps the unusable files as `*.corrupt-*`.
- If `global.sav` comes from a newer major version, the engine does not change it. (This can occur
  if you run a newer build and then go back to an older build.) In that session, the engine does
  not load `global.sav` and does not write global data. It logs an error. It never replaces the
  newer data with older data.

## 8. Other file kinds

### 8.1 Common META sub-chunks

`GAME` str8, `ENGV` str8, `LANG` str8, `TIME` i64, `PACK` u64 + u32. All of them are optional for
readers.

### 8.2 GLOB

`META`, `VMGL` (container, `VMG2`), `SYSG` (container, `RFL2`), `CGTG` (container, `CGTF`), then
the preserved chunks (if there are any), then `END`.

- `VMG2`: `rle32 G`, `rle32 Z`, `sstr M`, `sstr ＊namae`.
- `RFL2`: `u32 n`, then `n x { str16 scene name, u64 code hash, u32 nflags, ceil(nflags/8) bytes }`.
  Bit `k` of the flags is `byte[k>>3] >> (k&7) & 1`.
- `CGTF`: `i32 n`, then `n x i32` flag values.

### 8.3 CONF

`META` (`GAME`, `ENGV`, `TIME`), `CFG2`, `END`. `CFG2` is `u32 n`, then `n x (str8 key, i32 value)`.
The table below shows the keys. Booleans are 0/1:

| Key | Range | Key | Range |
|---|---|---|---|
| `volume.all` `.bgm` `.koe` `.pcm` `.se` | 0–255 | `on.all` `.bgm` `.koe` `.pcm` `.se` | bool |
| `bgmfade.on` | bool | `bgmfade.volume` | 0–255 |
| `message.speed` | 0–1000 ms/char | `message.nowait` | bool |
| `auto.moji_wait` | 0–10000 | `auto.min_wait` | 0–60000 |
| `font.shadow` | 0–3 | `font.futoku` | bool |
| `skip.unread` `koe.dont_stop` `wheel.next` | bool | `anime.skip_wipe` `.no_wipe` `.no_mwnd` | bool |
| `message.chrcolor` | bool | `window.mode` | 0 window, 1 fullscreen |
| `window.scale` | 1–8 | `language` | 0 never chosen, 1 `ja`, 2 `en` |

`language` is the last language that the user selected in the start-up selector or in the config
dialog.

- It is the preselected item in the selector.
- It is the language of headless runs without `--lang`.
- `--lang` does not change it.
- Older engines ignore this key.

`config.sav` stores no data that is specific to one platform: no window position, monitor, paths or
key codes.

### 8.4 BNDL (export bundle)

`META` (`GAME`, `ENGV`, `TIME`), then one `FILE` container for each file, then `END`.

A `FILE` holds these chunks:

- `NAME` (str8): one of the file names of §1, or a format-1 slot name;
- `DATA`: the complete file. This file is also a valid save file of the kind that its name
  specifies.

The import refuses the full bundle if an entry is not valid, has an unknown name, or occurs two
times.

## 9. Module chunks

### 9.1 `VM02` (vm.c)

```
str16 scene; u32 pc; i32 line
u32 n; i32 int_stack[n]
u32 n; str16 str_stack[n]
u32 n; i32 marks[n]
u32 depth (>= 1)
depth x frame {
    i32 type; i32 ret_form
    str16 resume_scene; u32 resume_pc; i32 resume_line
    i32 base_i; i32 base_s; i32 base_m
    rle32 L; sstr K
    u32 nloc; nloc x prop
}
7 x rle32            local banks A B C D E F X
sstr S; sstr ％namae
u32 nscene_records   only scenes that have a non-default scene prop
nscene_records x { str16 scene; u64 code hash; u32 nprop; u32 m; m x { u32 prop_index (increasing); prop } }
prop = i32 form; i32 name; i32 int; str16 str; u32 nchain (<= 32); i32 chain[nchain];
       form 11: rle32 list;  form 21: sstr list
```

Load checks:

- Each scene name must exist.
- `pc` and each resume pc must be ≤ the code size of the scene.
- Each count has a limit.
- The engine skips scene props whose hash or prop count is different from the current scene. Those
  props keep their defaults.
- After the load, the engine declares again each prop whose form is different from its declaration.

### 9.2 `GFX1` (gfx.c)

```
u32 n; n x { u32 stage (0 back, 1 front); u32 index (< 256); object }
4 x effect; effect (screen)
```

The writer writes only the objects that have `#OBJECT.nnn.SAVE` set and that are different from a
newly initialised object. In an object, the writer writes a field only when it is different from
the default object:

```
object = u32 scalar_mask; per set bit, ascending: str16 (bit 1) or i32 (all other bits)
         u32 event_mask_lo; u32 event_mask_hi; per set bit, ascending: event
         u32 rep_mask (bits 0..3 = x, y, z, tr rep lists); per set bit: u32 count; count x 11 i32 (full events)
         u32 nchild; nchild x object         (at most 8 levels)
event  = u32 field_mask (bits 0..10 = def, value, time, total, delay, start, cur, end, loop, speed, real);
         per set bit an i32
```

Scalar bits:

| Bit | Field | Bit | Field | Bit | Field |
|---|---|---|---|---|---|
| 0 | type (0/1/2) | 10 | wipe_copy | 20 | mask_no |
| 1 | file (str16) | 11 | wipe_erase | 21 | culling |
| 2 | rect_rgb | 12 | layer | 22 | alpha_test |
| 3 | rect_x | 13 | order | 23 | alpha_blend |
| 4 | rect_y | 14 | world | 24 | is_button |
| 5 | rect_w | 15 | blend | 25 | btn_no |
| 6 | rect_h | 16 | tonecurve | 26 | btn_group |
| 7 | disp | 17 | click_disable | 27 | btn_action |
| 8 | clip_use | 18 | light_no | 28 | btn_se |
| 9 | src_clip_use | 19 | fog_use | 29 | btn_state |

Event bits:

- 0–24: patno, x, y, z, center_x/y/z, center_rep_x/y/z, scale_x/y/z, rotate_x/y/z, clip_l/t/r/b, src_l/t/r/b, tr;
- 25–35: tone[0..10] (mono, reverse, bright, dark, r, g, b, rate, add_r, add_g, add_b).

An effect has two parts:

1. `u32 event_mask` (x, y, z, tone[0..10]), with events as above;
2. `u32 scalar_mask` (begin_order, end_order, begin_layer, end_layer, wipe_copy, wipe_erase). Each
   set field is an `i32`.

### 9.3 `RFL2` (sysstate.c)

See §8.2. The engine keeps the records for scene code that is not loaded, and writes them back (§5).

### 9.4 Module chunks whose layout did not change from format 1

Only the container frame around these chunks changed. This section lists them to make this
document complete:

- `PRC1`: `u32 n (1..4096)`, then `n x { i32 type (< 0x40); i32 nchain (<= 32); i32 chain[nchain]; u8 key_skip;
  u8 no_skip; u8 push_result; i32 end_time; i32 iarg[4] }`. The bottom proc must be the script proc (1).
- `SYS1`: `u8 skip_disabled, readskip_disabled, script_auto, syscom_locked`; `str16 title`; then 16 counters, each
  `{ u8 running, real, frame_mode, frame_loop; i32 frame_start, frame_end, frame_time, value }`.
- `MWND`: `i32 1` (layout version); `i32 current window`; `str16 preview`; then 16 windows, each:
  - `u8 open, in_block, clear_pending`;
  - `i32 waku, name_waku, koe_no`;
  - `u8 koe_linked`;
  - `i32 size` (−1..1000);
  - `str16 name`;
  - `u32 nrf`, then `i32 (scene, flag) pairs[nrf/2]`;
  - `u32 nrp`, then `{ i32 op (0 text, 1 newline, 2 size, 3 name); i32 a; str16 s }[nrp]`.

  On load, the engine makes the page again. It does these operations again in sequence.
- `AUD1`: `u8 bgm_playing; str16 bgm; i32 bgm_volume; i32 koe_volume`; then 16 pcm channels, each
  `{ u8 looping; i32 volume; if looping: str16 name, i32 volume_type }`. The reader clamps volumes
  to 0–255.
- `BLOG`: `u32 n (<= 100)`, then `n x { str16 text, name, disp_name; i32 koe_no, chara, scene, line }`.
- `BTNG`: 3 stages x 16 groups, each `{ u8 flags (bit0 active, bit1 wait, bit2 cancel_ok); i32 cancel_se,
  decided, result, result_no }`.

## 10. Format 1 and migration

Format 1 is the format of the first engine version:

- There is no file header.
- Chunks have 8-byte headers (`fourcc, u32 length`) and no CRC.
- The names of slots are `saveNNN.sav` and `quickN.sav`.
- Snapshots stored approximately 520 KB (dense gfx and banks).

The engine can still read format 1. It migrates format-1 files as follows:

- **`global.sav` and `config.sav`**: the engine converts them at boot.
  1. It copies the original to `global.sav.v1` / `config.sav.v1`, one time only.
  2. It writes the file again in format 2. The old file becomes the `.bak` of that write.

  Read flags in format 1 have no hash. The engine applies them by scene name to the
  current pack. At that time, the current pack was always the Japanese pack.
- **Slots**: the engine converts a slot when it loads it, because the conversion uses the live
  modules as the intermediate form. The load screen lists format-1 slots, with their 240x135
  thumbnail. When the engine loads a format-1 slot, it does these steps:
  1. It restores the slot.
  2. It serialises the state again as a format-2 snapshot.
  3. It writes `slot_NNN.sav`.
  4. It renames the old file to `saveNNN.sav.v1`.

  Format-1 slots have no call-stack hashes. The engine can load them only while the language is
  `"ja"`.
- Bundles can contain format-1 slots that the engine did not convert yet, with their old names.

## 11. Tests

- `sayooshi GameData --selftest save` runs these checks:
  - Writer/Reader, CRC and RLE unit tests;
  - the golden fixture (the test skips this check when the fixture is not available);
  - the save→load→save round trips for each module and for the full snapshot (byte-identical);
  - durability: `.bak` after a write of a new version; fallback from a truncated main file, from a
    main file with a changed bit, and from a single `.tmp`; no rotation of a damaged main file over
    a good `.bak`;
  - preservation of global data (unknown chunk, chunk that fails, and a `global.sav` from a newer
    version that must stay unchanged);
  - the language rule: a snapshot from a different language loads when the call-stack hashes
    match. The engine refuses it (slot in grey, state of the game that runs not changed) when one
    hash is different;
  - export/import, and the rejection of a damaged bundle;
  - format-1 slot conversion;
  - a mutation fuzzer.

  Environment variables:

  | Variable | Meaning |
  |---|---|
  | `SAYO_SAVE_FUZZ_ITERS` | number of fuzz iterations (default 1000) |
  | `SAYO_SAVE_FUZZ_SEED` | fuzzer seed |
  | `SAYO_SAVE_FUZZ_RUN` | VM steps to run after each fuzzed load |
  | `SAYO_SAVE_CORPUS` | `;`-separated extra save files to mutate (any kind, either format) |
  | `SAYO_SAVE_GOLDEN_WRITE` | path to regenerate the golden fixture |

  After mutation, the fuzzer calculates the CRCs again for three quarters of the fuzzed files.
  Thus the corrupted data gets to the module decoders, and the CRC check does not stop it.

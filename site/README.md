# Sayonara o Oshiete builder (static site)

This folder holds a single web page. The page uses two archives that the user supplies:

- the user's own copy of *Sayonara o Oshiete ~Comment te Dire Adieu~* (JA download edition zip);
- the English patch v1.1 zip.

The page makes one self-contained game executable for Linux or Windows from these archives. All
steps run in the browser. No file leaves the user's machine.

This folder holds **no game data**. It holds only these items:

- the engines without a payload;
- the page;
- 7-Zip compiled to WebAssembly.

## How it works

1. The page checks the two archives by size and SHA-256 (`js/build.js`, `KNOWN_INPUTS`). It refuses
   all other files.
2. The page reads the JA zip one entry at a time (`js/zip.js`, the browser's `DecompressionStream`,
   CP932 names).
3. The page unpacks the NSIS installer of the EN patch with 7-Zip/WASM (`vendor/7z-wasm/`).
4. The page puts the files in the same data tree as `make data`. It packs the data tree into a
   SAYOPAK payload (docs/PAYLOAD.md).
5. The page appends the payload to the selected engine from `engines/`. It first verifies the
   engine against `engines/engines.json`.
6. The page checks the finished game against the SHA-256 of the reference build. Then it offers the
   game for download.

The result is byte-identical to `make build` / `make build-windows` on the same archives. Thus the
builds are reproducible. `make site-test` proves this in Node and in headless Chromium.

## Update and deploy

```
make site          # builds Linux + Windows, strips their payloads into site/engines/, writes engines.json
make site-test     # Node + headless-Chromium checks against dist/
make site-serve    # http://localhost:8000
```

Deploy the `site/` folder as it is. On Netlify, use one of these methods:

- Drag and drop the folder.
- Run `netlify deploy --dir site --prod`.
- Link the repository and set the base directory to `site/`.

`netlify.toml` needs no build command.

Serve the folder over HTTP(S). If you open `index.html` from the disk, the page cannot load the
modules and the WebAssembly.

Browser requirements:

- a current Chrome, Edge, Firefox or Safari (ES modules, `DecompressionStream`, WebAssembly);
- approximately 1.5 GB of free memory or disk for the blobs of a build.

## Licenses

- The code of this project (the page, `js/`, the engine): BSD Zero Clause License, 0BSD
  (`licenses/0BSD.txt`).
- Engines: the C engine of this project, statically linked with SDL 3 (zlib license,
  `licenses/SDL3.txt`) and stb_vorbis / stb_truetype / stb_image_write (public domain or MIT,
  `licenses/stb.txt`). The engines also contain the Kosugi font (Apache License 2.0,
  `licenses/Kosugi-Apache-2.0.txt`).
- `vendor/7z-wasm/`: 7-Zip by Igor Pavlov, WebAssembly build from the npm package `7z-wasm` 1.2.0
  (https://github.com/use-strict/7z-wasm), not modified. GNU LGPL with the unRAR restriction
  (`licenses/7-Zip.txt`, `licenses/7-Zip-unRAR.txt`). Source: https://www.7-zip.org and the
  repository above.
- The game and the English patch belong to their owners. This folder never includes them. Users
  supply their own copies.

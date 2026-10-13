"""Stage the payload-free engines into site/engines/ and write site/engines/engines.json.

usage: python3 -I tools/site/stage.py SAYOPACK REPO_ROOT
For each target whose dist/ build exists: engine = `sayopack strip` of it (no game data), plus the SHA-256
of the engine and of the complete self-contained game, so the page can check what it downloads and what it built.
macOS is an app bundle with the payload in Contents/Resources/game.pak: every other file of the bundle is staged,
and the page zips them with the game.pak it builds. The bundle's ad-hoc signature seals game.pak by its hash, and
the page builds the identical game.pak, so the signature stays valid.
A target this host cannot build keeps its previous entry, as long as its staged files are unchanged: build Linux on
a Linux machine, macOS on a Mac, run make site on each, and commit.
Also writes site/favicon.ico: the game's icon (the one the Windows engine embeds), cut down to its small sizes.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys

TARGETS = [
    # id, label, engine file (or folder, for an app) in site/engines, download name, dist build
    ("linux-x86_64", "Linux (x86-64)", "sayooshi-linux-x86_64", "sayooshi", "dist/linux-x86_64/sayooshi"),
    ("windows-x86_64", "Windows (x86-64)", "Sayonara-windows-x86_64.exe", "Sayonara.exe",
     "dist/windows-x86_64/Sayonara.exe"),
    ("macos-universal", "macOS (Apple silicon + Intel)", "macos-universal", "Sayonara.app.zip",
     "dist/macos-universal/Sayonara.app"),
]
APP_PAYLOAD = "Contents/Resources/game.pak"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def favicon(ico_path, out_path):
    """Copies the ICO entries of 48 px and less: the 256 px entry alone is 200 KB."""
    with open(ico_path, "rb") as f:
        data = f.read()
    count = int.from_bytes(data[4:6], "little")
    entries = [data[6 + 16 * i:22 + 16 * i] for i in range(count)]
    entries = [e for e in entries if 0 < e[0] <= 48]
    out = bytearray(data[:4] + len(entries).to_bytes(2, "little"))
    images, offset = b"", 6 + 16 * len(entries)
    for e in entries:
        size = int.from_bytes(e[8:12], "little")
        start = int.from_bytes(e[12:16], "little")
        out += e[:12] + (offset + len(images)).to_bytes(4, "little")
        images += data[start:start + size]
    with open(out_path, "wb") as f:
        f.write(out + images)


def stage_exe(sayopack, root, out_dir, tid, label, engine, output, dist):
    dist_path = os.path.join(root, dist)
    engine_path = os.path.join(out_dir, engine)
    subprocess.run([sayopack, "strip", dist_path, engine_path], check=True, stdout=subprocess.DEVNULL)
    print(f"site: {label}: engine {os.path.getsize(engine_path)} bytes")
    return {
        "id": tid, "label": label, "engine": f"engines/{engine}", "output": output, "dist": dist,
        "size": os.path.getsize(engine_path), "sha256": sha256(engine_path),
        "game_size": os.path.getsize(dist_path), "game_sha256": sha256(dist_path),
    }


def stage_app(root, out_dir, tid, label, engine, output, dist):
    """Copies the bundle without game.pak; the page zips it as <bundle name>/..., game.pak included."""
    app = os.path.join(root, dist)
    name = os.path.basename(dist)
    dest = os.path.join(out_dir, engine)
    shutil.rmtree(dest, ignore_errors=True)
    files = []
    for top, dirs, names in os.walk(app):
        dirs.sort()
        for n in sorted(names):
            src = os.path.join(top, n)
            rel = os.path.relpath(src, app).replace(os.sep, "/")
            if rel == APP_PAYLOAD or n == ".DS_Store":
                continue
            os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
            shutil.copyfile(src, os.path.join(dest, rel))
            files.append({
                "path": f"{name}/{rel}", "engine": f"engines/{engine}/{rel}", "size": os.path.getsize(src),
                "sha256": sha256(src), "mode": 0o755 if os.access(src, os.X_OK) else 0o644,
            })
    payload = os.path.join(app, APP_PAYLOAD)
    print(f"site: {label}: {len(files)} bundle files, {sum(f['size'] for f in files)} bytes")
    return {
        "id": tid, "label": label, "kind": "app", "output": output, "dist": dist, "files": files,
        "payload_path": f"{name}/{APP_PAYLOAD}",
        "payload_size": os.path.getsize(payload), "payload_sha256": sha256(payload),
    }


def still_staged(root, entry):
    """True when every staged file of an entry from the last engines.json is still there, unchanged."""
    files = entry.get("files") or [entry]
    site = os.path.join(root, "site")
    return all(os.path.isfile(os.path.join(site, f["engine"])) and sha256(os.path.join(site, f["engine"])) == f["sha256"]
               for f in files)


def main(sayopack, root):
    out_dir = os.path.join(root, "site", "engines")
    os.makedirs(out_dir, exist_ok=True)
    json_path = os.path.join(out_dir, "engines.json")
    previous = {}
    if os.path.isfile(json_path):
        with open(json_path) as f:
            previous = {t["id"]: t for t in json.load(f)["targets"]}
    targets = []
    for tid, label, engine, output, dist in TARGETS:
        if os.path.exists(os.path.join(root, dist)):
            if dist.endswith(".app"):
                targets.append(stage_app(root, out_dir, tid, label, engine, output, dist))
            else:
                targets.append(stage_exe(sayopack, root, out_dir, tid, label, engine, output, dist))
        elif tid in previous and still_staged(root, previous[tid]):
            print(f"site: {dist} not built here, {label} kept from the last make site")
            targets.append(previous[tid])
        else:
            print(f"site: {dist} not built, {label} left out (make build-{tid.split('-')[0]})")
    ico = os.path.join(root, "build", "icon", "ADIEU.ico")
    if os.path.isfile(ico) and os.path.getsize(ico):  # empty when the game has no icon
        favicon(ico, os.path.join(root, "site", "favicon.ico"))
    if not targets:
        sys.exit("site: no dist builds; run make build / make build-windows first")
    with open(json_path, "w") as f:
        json.dump({"targets": targets}, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

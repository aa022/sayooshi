"""Stage the payload-free engines into site/engines/ and write site/engines/engines.json.

usage: python3 -I tools/site/stage.py SAYOPACK REPO_ROOT
For each target whose dist/ executable exists: engine = `sayopack strip` of it (no game data), plus the SHA-256
of the engine and of the complete self-contained game, so the page can check what it downloads and what it built.
"""
import hashlib
import json
import os
import subprocess
import sys

TARGETS = [
    # id, label, engine file in site/engines, download name, dist executable
    ("linux-x86_64", "Linux (x86-64)", "sayooshi-linux-x86_64", "sayooshi", "dist/linux-x86_64/sayooshi"),
    ("windows-x86_64", "Windows (x86-64)", "Sayonara-windows-x86_64.exe", "Sayonara.exe",
     "dist/windows-x86_64/Sayonara.exe"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(sayopack, root):
    out_dir = os.path.join(root, "site", "engines")
    os.makedirs(out_dir, exist_ok=True)
    targets = []
    for tid, label, engine, output, dist in TARGETS:
        dist_path = os.path.join(root, dist)
        if not os.path.isfile(dist_path):
            print(f"site: {dist} not built, {label} left out (make build-{tid.split('-')[0]})")
            continue
        engine_path = os.path.join(out_dir, engine)
        subprocess.run([sayopack, "strip", dist_path, engine_path], check=True, stdout=subprocess.DEVNULL)
        targets.append({
            "id": tid, "label": label, "engine": f"engines/{engine}", "output": output, "dist": dist,
            "size": os.path.getsize(engine_path), "sha256": sha256(engine_path),
            "game_size": os.path.getsize(dist_path), "game_sha256": sha256(dist_path),
        })
        print(f"site: {label}: engine {os.path.getsize(engine_path)} bytes")
    if not targets:
        sys.exit("site: no dist executables; run make build / make build-windows first")
    with open(os.path.join(out_dir, "engines.json"), "w") as f:
        json.dump({"targets": targets}, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

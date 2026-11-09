#!/usr/bin/env python3
"""Build the PS3 key file the ripper loads (keys/ps3.keys).

Inputs:
  --dat   Redump PS3 DAT (XML), or a .zip containing it
  --keys  directory or .zip of per-game key files, named after the Redump game:
            "<game name>.key"   16 raw bytes (the disc key)
            "<game name>.dkey"  32 hex characters

Output, one line per disc that has both a DAT entry and a key:
  <sha1 of encrypted .iso> <size> <disc key hex> <game name>

The ripper matches a fresh dump by SHA-1, so names never have to line up with
anything on the disc itself.
"""

import argparse
import io
import os
import sys
import xml.etree.ElementTree as ET
import zipfile


def read_dat(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.lower().endswith((".dat", ".xml"))]
            if not names:
                sys.exit(f"{path}: no .dat inside")
            data = z.read(names[0])
    else:
        with open(path, "rb") as f:
            data = f.read()
    root = ET.parse(io.BytesIO(data)).getroot()
    games = {}
    for game in root.iter("game"):
        name = game.get("name")
        for rom in game.iter("rom"):
            if rom.get("name", "").lower().endswith(".iso") and rom.get("sha1"):
                games[name] = (rom.get("sha1").lower(), int(rom.get("size", "0")))
                break
    return games


def parse_key(filename, data):
    if filename.lower().endswith(".key"):
        return data.hex() if len(data) == 16 else None
    text = data.decode("ascii", "ignore").strip()
    if len(text) >= 32:
        text = text[:32]
        try:
            bytes.fromhex(text)
            return text.lower()
        except ValueError:
            return None
    return None


def iter_key_files(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                if n.lower().endswith((".key", ".dkey")):
                    yield os.path.basename(n), z.read(n)
    else:
        for n in sorted(os.listdir(path)):
            if n.lower().endswith((".key", ".dkey")):
                with open(os.path.join(path, n), "rb") as f:
                    yield n, f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dat", required=True)
    ap.add_argument("--keys", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    games = read_dat(args.dat)
    keys = {}
    for filename, data in iter_key_files(args.keys):
        key = parse_key(filename, data)
        if key:
            keys[os.path.splitext(filename)[0]] = key

    written = 0
    missing_key = 0
    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("# sha1 size key name\n")
        for name in sorted(games):
            sha1, size = games[name]
            key = keys.get(name)
            if not key:
                missing_key += 1
                continue
            out.write(f"{sha1} {size} {key} {name}\n")
            written += 1

    print(f"{written} discs written, {missing_key} DAT entries without a key, "
          f"{len(set(keys) - set(games))} keys without a DAT entry")


if __name__ == "__main__":
    main()

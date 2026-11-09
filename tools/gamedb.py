#!/usr/bin/env python3
"""Builds third_party/gamedb/gamedb.tsv: game serials to Redump names.

    tools/gamedb.py [--from DIR]

From libretro-database's metadat/redump DATs (CC-BY-SA-4.0, Redump's names
with the serials the discs carry). One line a serial: "system<TAB>serial<TAB>
name", sorted. The payload embeds it compressed (ps5.mk, gen/gamedb.h) and
names a disc from its serial when the disc doesn't say its own name.

--from reads the .dat files from a folder instead of downloading them.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "third_party/gamedb/gamedb.tsv"
BASE = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/redump/"
SYSTEMS = {
    "ps1": "Sony - PlayStation.dat",
    "ps2": "Sony - PlayStation 2.dat",
    "ps3": "Sony - PlayStation 3.dat",
    "saturn": "Sega - Saturn.dat",
    "segacd": "Sega - Mega-CD - Sega CD.dat",
    "pcecd": "NEC - PC Engine CD - TurboGrafx-CD.dat",
    "3do": "The 3DO Company - 3DO.dat",
    "gamecube": "Nintendo - GameCube.dat",
    "wii": "Nintendo - Wii.dat",
    "xbox": "Microsoft - Xbox.dat",
}
GAME = re.compile(r'^game \($(.*?)^\)$', re.M | re.S)
FIELD = re.compile(r'^\t(name|serial) "(.*)"$', re.M)


def read(name, source):
    if source:
        return (Path(source) / name).read_text(encoding="utf-8", errors="replace")
    url = BASE + urllib.parse.quote(name)
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read().decode("utf-8", errors="replace")


def main():
    source = sys.argv[sys.argv.index("--from") + 1] if "--from" in sys.argv else None
    rows = {}
    for system, dat in SYSTEMS.items():
        text = read(dat, source)
        n = 0
        for block in GAME.findall(text):
            fields = dict(FIELD.findall(block))
            name, serials = fields.get("name"), fields.get("serial")
            if not name or not serials:
                continue
            for serial in re.split(r"[,/]\s*|\s+", serials):
                serial = serial.strip().upper()
                if len(serial) < 4 or "\t" in name:
                    continue
                # The first name a serial has wins: Redump lists the original before revisions.
                if rows.setdefault((system, serial), name) == name:
                    n += 1
        print(f"{system}: {n} serials", file=sys.stderr)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"{s}\t{k}\t{v}\n" for (s, k), v in sorted(rows.items())]
    OUT.write_text("".join(lines), encoding="utf-8")
    print(f"{OUT}: {len(lines)} lines, {OUT.stat().st_size // 1024} KiB", file=sys.stderr)


if __name__ == "__main__":
    main()

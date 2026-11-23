# Developing RipperPS

## How it fits together

```
drive ─► transport ─► drive probe ─► identify ─► offer ─► job ─► sink ─► destination
         CAM (PS5)     what it reads   what disc    formats   read,    ISO, CHD,   console, USB,
         SG_IO (PC)    (caps)          it is        this      hash,    CUE, MKV,   SMB, NFS, FTP,
         image (tests)                              drive can retry    FLAC...     RomM
```

Three layers, each only calling the one below:

- **Core** (`include/ripper/`, `src/core/`): portable C11 with no threads or networking. Sends SCSI/MMC commands, reads file systems, makes images. Builds and tests on a PC.
- **Service** (`src/service/`): the app. Watches drives, runs jobs, keeps settings, delivers rips, serves the JSON API and the web UI.
- **Platform** (`src/platform/ps5/`, `src/platform/linux/`): what differs per system, behind `src/service/platform.h`.

RipperPS (`platform/ps5/app/ripper/`) is a client of the JSON API, like the web UI. It carries the payload and starts it.

## Layout

```
include/ripper/        core headers
src/core/              MMC, drive probe, disc identification, ISO 9660, UDF, CD (TOC, CUE, ECC), CSS, CHD,
                       PS3 crypto, Nintendo/Xbox/Dreamcast reads, DVD titles, remux, audio, game dump
                       packaging (FSELF, UFS2, exFAT), hashes, zip, the rip pipeline, the drive diagnostic
src/service/           drive watcher, jobs, settings, catalog, destinations, names and covers, disc reports,
                       the drive test, HTTP server and JSON API
src/platform/ps5/      CAM transport, game dumps, notifications, focus, keep-awake, the payload's main
src/platform/linux/    SG_IO transport, ripper-cli, ripperd
ui/                    the web UI: one HTML file and its themes, embedded into the payload at build time
platform/ps5/app/      RipperPS
tests/                 fake MMC drive, ISO 9660 and UDF image builders, unit and end-to-end tests
tools/                 build scripts, PS3 key file builder, game name database builder
third_party/           libdvdcss, cJSON, QR code, PS5 SELF pager, fonts, Redump names
docs/                  this guide and the tester guide
```

## Core

- **Transport** (`transport.h`): one SCSI command in, data and sense out. The PS5 uses CAM pass-through (`/dev/passN`), Linux uses `SG_IO`, and tests use `image_drive.c`, a virtual drive that answers MMC from an `.iso` or `.cue`.
- **Drive probe** (`drive.h`): INQUIRY and GET CONFIGURATION become capability bits: CD/DVD/BD read, C2, Sony profiles, and Kreon, OmniDrive and Plextor from their INQUIRY marks.
- **Identify** (`disc.h`): the drive's profile first (the PS5 drive reports PlayStation profiles like `0xFF61` for PS2 DVD), then the disc's own file system and headers.
- **Recipes** (`rip.h`): each disc kind lists the outputs it can go to and the capabilities each needs. That's how a better drive unlocks more without changes elsewhere. The UI shows each output as available, needs another drive, planned or blocked.
- **Rip pipeline**: reads in chunks, retries bad sectors one at a time, hashes while writing through a sink (ISO, CHD, BIN/CUE...), writes `<name>.part` and renames when complete.
- **Diagnostic** (`diag.h`): the drive test. Read commands only, every answer saved raw.

## Service

| File | What it does |
|---|---|
| `service.c` | Startup: settings, log, drive watcher, jobs, web server |
| `drives.c` | One slot per drive, polled every 1.5 s. States: empty, loading, identifying, ready, unsupported, ripping, done, installing. Writes a disc report for every disc |
| `jobs.c` | One job per drive at a time: read into staging, release the drive, deliver. Kept in `jobs.json` |
| `settings.c`, `catalog.c` | `config.json`, and the systems, formats and destination types the UIs build their forms from |
| `deliver*.c`, `remote.c` | Destinations: a folder on the console, a USB drive, SMB, NFS, FTP, RomM (device sign-in, chunked upload, library scan) |
| `details.c`, `gamedb.c`, `musicbrainz.c` | Names and covers: Redump names by serial, MusicBrainz, Wikipedia (or TMDB with a key), the disc's own icons |
| `disc_report.c` | A short report of every disc, in `diag/discs/` |
| `drivetest.c` | The guided drive test: a disc at a time, packed into one zip for the drive report form |
| `web.c`, `http.c` | The HTTP server and JSON API, and the HTTP client |

Data lives in `/data/ripper/` on the console (`RIPPER_DATA` on a PC): `config.json`, `jobs.json`, `stats.json`, `ripper.log`, `crash.log`, `keys/`, `staging/`, `rips/` and `diag/`.

## JSON API

Port 8790. Everything the web UI and RipperPS do goes through it.

| Endpoint | |
|---|---|
| `GET /api/status` | Version, build, IP, uptime, running jobs |
| `GET /api/drives` | Each drive, its state, disc and offer |
| `POST /api/drives/<id>/<action>` | `eject`, `identify`, `lookup`, `benchmark`, `probe`, `drivetest`, `load` (virtual drives) |
| `GET /api/drives/<id>/cover` | The disc's cover |
| `POST /api/rip` | Start a rip from an offer |
| `GET /api/jobs`, `POST /api/jobs/<id>/cancel`, `/retry`, `POST /api/jobs/clear` | The queue |
| `GET /api/games` | Installed PS4/PS5 games |
| `GET`, `POST /api/settings`, `GET /api/catalog` | Settings and what they can be |
| `POST /api/destinations/test`, `GET /api/folders?scope=local\|usb&path=` | Destination check and folder picker |
| `POST /api/romm/pair`, `/api/romm/pair/cancel`, `GET /api/romm/rommps` | RomM sign-in, and RommPS's server if it's installed |
| `GET /api/drivetest`, `POST /api/drivetest/disc`, `/finish`, `/discard`, `GET /api/drivetest/file` | The drive test |
| `GET /api/reports` | Every disc report, drive test and the log, zipped |
| `GET /api/log`, `GET /api/stats`, `POST /api/notify`, `POST /api/keep-awake` | Odds and ends |

## Building

### Host

```sh
cmake -S . -B build/host -G Ninja
cmake --build build/host
(cd build/host && ctest --output-on-failure)
```

Warnings are errors (`RIPPER_WERROR`). An ASan/UBSan build: `cmake -S . -B build/asan -G Ninja -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"`.

The host build has no FFmpeg or libFLAC, so MKV, MP4 and lossy audio show as "not in this build" and audio CDs come out as WAV. SMB and NFS need libsmb2 and libnfs: `tools/host-deps.sh` builds them into `build/deps`.

`ripper-cli` drives the core directly:

```sh
ripper-cli probe --device /dev/sr0
ripper-cli rip --device /dev/sr0 --out out --keys ps3.keys
ripper-cli rip --image dump.iso --profile ps3-bd --keys ps3.keys   # profiles: dvd, bd, ps2-dvd, ps3-dvd, ps3-bd
ripper-cli ps3-decrypt --in encrypted.iso --key <32 hex>            # or --d1 <32 hex>
ripper-cli pack --in GAME_DIR --out game.ffpkg                      # or game.exfat
ripper-cli diag --device /dev/sr0                                   # the drive diagnostic, into a zip
```

`ripperd` runs the whole service on a PC, web UI included. `RIPPER_VIRTUAL=ps5` makes its virtual drive act like the PS5's. Load images from the drive tab, or start with one:

```sh
RIPPER_DATA=data RIPPER_VIRTUAL=ps5 RIPPER_IMAGE=disc.iso build/host/ripperd   # http://localhost:8790
```

### PS5 payload

Built with the PS5 payload SDK in Docker (RommPS's image is used if it's there):

```sh
tools/ps5-build.sh                                  # build/ps5/ripper.elf
tools/ps5-build.sh deploy PS5_HOST=<console ip>     # send it to port 9021
```

After sending, check `http://<ps5-ip>:8790/api/status`: a console in rest mode drops the payload silently. Keep the `ripper.elf` you sent, since crash logs need it to be read.

### RipperPS

Build the payload first, since the app carries it. Then, from the repository's root:

```sh
tools/ps5-app-build.sh bash -c 'cd /repo/platform/ps5/app/ripper && bash ps5/tools/build.sh ripper'   # dist/PPSA76700/
tools/ps5-app-build.sh bash -c 'cd /repo/platform/ps5/app/ripper && bash ps5/tools/run.sh ripper'     # deploy and run
```

It needs mihawk-99's PS5 stack in `../ps5-stack`. More in [its README](../platform/ps5/app/ripper/README.md).

## Conventions

- C sources are formatted with the repository's `.clang-format`. Don't run it over RipperPS's C++, which follows the template's style.
- Drive commands that only read. Vendor commands go only to that vendor's drives.
- The drive test never records key material, and hashes the drive's serial number.
- Comments say why, in plain words.

## Versions and releases

The version is in three places, which must match: `VERSION` in `src/platform/ps5/ps5.mk`, `RIPPER_VERSION` in `src/service/svc.h`, and `contentVersion` in RipperPS's `ps5/sce_sys/param.json` (0.2.0 is `00.002.000`). Each payload build is stamped `YYYYMMDD-HHMMSS`. RipperPS replaces a running payload with an older stamp than its own, and never a newer one.

To release:

1. Bump the version in those three places and write `docs/releases/<tag>.md` (the release notes).
2. Commit. The release is built from a clean commit, since RipperPS's notices name it.
3. `tools/release.sh v0.2.0` builds and checks everything into `build/release/`: `PPSA76700.zip`, `ripper.elf` and `SHA256SUMS`.
4. `tools/release.sh v0.2.0 --publish` does the same and publishes it as a GitHub pre-release.

CI (`.github/workflows/ci.yml`) runs the host tests, the ASan build and the payload build on every push.

The issue forms use these labels: `bug`, `enhancement`, `question` and `test submission`.

## PS3 keys

Build the key file from a Redump PS3 DAT and the matching key files:

```sh
python3 tools/build_ps3_keydb.py --dat "Sony - PlayStation 3.dat" --keys ps3_keys/ --out ps3.keys
```

`--keys` takes a folder or a zip of `<Redump name>.key` (16 raw bytes) or `.dkey` (32 hex characters) files. Each output line is `<sha1> <size> <key> <name>`.

## Not tried on hardware yet

Beyond the "not yet" rows in the README:

- A full PS3 rip decrypted with a key on the console.
- Blu-ray movies: only a badly scratched disc has been tried, and the drive refused it.
- AAC and Opus audio on the console (FLAC and MP3 are done).
- NFS against a real server, and RomM against a real one (tested with a mock).
- A PS5 dump as an exFAT or UFS image in ShadowMountPlus. The folder runs (Deathloop), but the AMPR emulator writes its `ampr_emu.index` into the game's folder on first run, and images are mounted read-only.
- PS4/PS5 dumps with an update or DLC installed.

## Roadmap

Every format a game's targets run, real or emulated, with nothing to convert after (archives like zip and 7z aren't wanted):

1. PS4 and PS5 dumps with the installed update (the system's `-app0-patch0-union` mount).
2. PS3 game folder (`PS3_GAME`, `PS3_DISC.SFB`) for RPCS3 and PS3 loaders, and the ISO split for FAT32 drives (`.iso.0`, `.iso.1`...).
3. DLC dumps, beside the game.
4. PS4 fake PKG (`.pkg`) for GoldHEN and etaHEN, and DLC as packages of their own. The keys come from a file the user supplies, like the PS3 key file. Then the PS5 fake PKG (LibProsperoPkg's format).
5. The formats each system's players commonly use, still missing:
    - PS2: CSO and ZSO (PCSX2, OPL). PS1: PBP (emulators, Adrenaline) and VCD (POPStarter).
    - Wii RVZ that's smaller than the ISO: partitions stored decrypted, which needs the Wii common key (a key file from the user, like the PS3 one).
    - Xbox: CCI (Cerbios). Xbox 360: Games on Demand (RGH and JTAG consoles).
    - Dreamcast: CHD (Flycast, RetroArch), once chdman's GD-ROM layout is understood. GDI and BIN/CUE are done.
6. PS1 and PS2 classics wrappers for the PS4 and PS5 (to research).
7. Results from testers' drives: GameCube, Wii, Xbox, Dreamcast and the rest of the CD systems on real discs.
8. AACS for Blu-ray movies from a KEYDB.cfg, then MKV for Blu-ray.
9. PS4/PS5 discs dumped without installing them, and any game's missing files skipped exactly (from the PS5's `naps_pkg_layout.dat`), not only other regions' languages.
10. Redump DATs to name and verify CD dumps, and the `.ss`/`.pfi`/`.dmi` files Redump keeps for Xbox and Nintendo discs.
11. Faster recovery through wide scratches on CDs.

# ripper

A PS5 payload that rips optical discs into the formats emulators and archivists use. It reads through the console's own Blu-ray drive today, and is built so that USB drives with more capabilities (CD laser, OmniDrive, Kreon, LibreDrive) unlock more disc types later without changing the core.

It needs a jailbroken console (any firmware with a public kernel exploit). Disc access only uses the kernel's standard CAM pass-through, so it does not depend on kstuff or a hypervisor exploit.

## Status

| Disc | PS5 drive | Output | State |
|---|---|---|---|
| PS2 DVD | yes | ISO | working, CHD planned |
| PS3 BD / DVD | yes | decrypted ISO (RPCS3) | working, needs a key file; folder output planned |
| DVD / Blu-ray data, PC games | yes | ISO | working |
| DVD-Video | yes | ISO | unprotected discs only; CSS planned |
| Blu-ray Video | yes | ISO (AACS stays encrypted) | working when the disc has ISO 9660; UDF detection planned |
| PS4 / PS5 | identify only | game folder via system dumper | planned |
| PS1, PS2 CD, audio and other CDs | no CD laser | BIN/CUE, CHD, FLAC | planned, external drive |
| GameCube, Wii, Xbox, Xbox 360 | no | RVZ, XISO, ISO | planned, OmniDrive/Kreon drive |

The host tests cover everything marked working. Nothing has run on a console yet; see [Unverified on hardware](#unverified-on-hardware).

## How it works

```
 drive ──transport──> drive probe ──> disc identify ──> plan (recipes × caps) ──> rip ──> image + manifest
          CAM (PS5)     caps bits       profile + ISO 9660     what is possible       hash, decrypt, rename
          SG_IO (Linux)
          fake (tests)
```

- **Transport** (`include/ripper/transport.h`) sends one SCSI/MMC command. The PS5 uses FreeBSD CAM through `/dev/xpt0` and `/dev/passN`. Linux uses `SG_IO`, so the same code runs against a PC drive while developing.
- **Drive probe** (`drive.h`) turns INQUIRY and GET CONFIGURATION into capability bits: CD/DVD/BD read, C2, PlayStation vendor profiles, and slots for vendor features.
- **Disc identify** (`disc.h`) starts from the drive profile. The PS5 drive reports PlayStation discs with vendor profiles (`0xFF61` PS2 DVD, `0xFF71` PS3 BD, …). A PC drive reports a plain DVD/BD, so the disc's ISO 9660 file system decides: `SYSTEM.CNF`, `PS3_DISC.SFB`, `VIDEO_TS`, `BDMV`.
- **Recipes** (`rip.h`) map a disc kind to an output and the capabilities it needs. The UI shows each one as available, needs another drive, planned, or blocked.
- **Rip** reads in 128 KiB chunks, retries bad sectors one at a time, hashes (CRC-32, MD5, SHA-1) while writing `<name>.iso.part`, and renames the file when it is complete.
- **PS3**: the dump's SHA-1 is looked up in the key file. A match renames the image to its Redump name and decrypts it in place. Without a key the image stays encrypted, and a `<stem>.key` or `<stem>.dkey` dropped next to it can be used later.
- **Manifest** `<name>.txt` records the disc, the drive and the hashes of the image as read. For PS3 these hashes are of the encrypted original, which is what Redump lists.

## Building

### Host (tests and CLI)

```sh
cmake -S . -B build/host -G Ninja
cmake --build build/host
(cd build/host && ctest --output-on-failure)
```

`ripper-cli` drives the same core:

```sh
ripper-cli probe --device /dev/sr0
ripper-cli rip --device /dev/sr0 --out out --keys ps3.keys
ripper-cli rip --image dump.iso --profile ps3-bd --keys ps3.keys
ripper-cli ps3-decrypt --in encrypted.iso --key <32 hex>   # or --d1 <32 hex>
```

### PS5 payload

Install the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk), then:

```sh
$PS5_PAYLOAD_SDK/bin/prospero-cmake -S . -B build/ps5
cmake --build build/ps5
PS5_HOST=<console ip> cmake --build build/ps5 --target deploy   # sends ripper.elf to the ELF loader on port 9021
```

## Running on the console

1. Jailbreak the console and start an ELF loader.
2. Optional: copy `config/ripper.ini.example` to `/data/ripper/ripper.ini`.
3. For PS3, put the key file at `/data/ripper/keys/ps3.keys` (see below).
4. Send `ripper.elf`. It posts a notification when it is ready, then rips every disc you insert.
5. Output goes to `/data/ripper/out` by default. Progress and errors go to klog and on-screen notifications.

## PS3 keys

Keys are not part of this repository. Build the key file from a Redump PS3 DAT and the matching key files:

```sh
python3 tools/build_ps3_keydb.py --dat "Sony - PlayStation 3.dat" --keys ps3_keys/ --out ps3.keys
```

`--keys` takes a directory or a zip of `<Redump name>.key` (16 raw bytes) or `.dkey` (32 hex characters) files. Each output line is `<sha1> <size> <key> <name>`.

## Layout

```
include/ripper/   public headers of the core library
src/core/         portable C11: MMC commands, drive probe, ISO 9660, SFO, PS3 crypto, hashes, rip pipeline
src/platform/ps5/ CAM transport, notifications, the payload's main loop
src/platform/linux/ SG_IO transport and ripper-cli
tests/            fake MMC drive, ISO 9660 image builder, unit and end-to-end tests
tools/            key file builder
config/           example configuration
```

## Unverified on hardware

These are written from documentation and prior work, and need checking on a console:

- The `union ccb` size assertion (`0x4E0`) and the pass-through lookup for `cd0` against the current SDK headers.
- Reading through `/dev/cd0` (the default on the PS5) versus READ(12) through pass-through, especially for PS3 discs.
- Treating `cd1`+ as external USB drives. It is not yet known whether USB optical drives attach as `cd` devices at all.
- `/mnt/disc` as the system mount for PS4/PS5 title IDs.
- Whether `PS3_GAME/PARAM.SFO` is in the unencrypted region on every PS3 disc. If not, the title falls back to the Redump name after hashing.

## Roadmap

1. Run on a console. Confirm the items above and measure read speed.
2. Add a CHD writer for PS2 DVD and later CD images.
3. Add a PS3 folder output for RPCS3. Decrypt while reading when the key can be chosen up front, to save a second pass.
4. Add a menu or web UI to pick a recipe instead of `auto_rip`.
5. External drives: detect them, then add CD (BIN/CUE, subchannel, C2), OmniDrive, Kreon and LibreDrive recipes.
6. PS4/PS5: hand off to a system dumper on firmware where kstuff runs.
7. Video: CSS for DVD, AACS keydb for Blu-ray, and a UDF reader.

Drive access follows the approach shown by [ps-disc-detect](https://github.com/Al-Azif/ps-disc-detect).

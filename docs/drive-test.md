# Ripper drive test

Thanks for testing! This checks what your drive can read, one disc at a time,
and saves the results in a zip for you to send back. Each run takes a minute
or two.

The test only reads. It doesn't write to the disc, change the drive's
settings or touch its firmware.

## What you need

- A jailbroken PS5 with a payload loader on port 9021 (etaHEN or similar).
- Your USB drive, plugged into the PS5.
- `ripper.elf` (sent along with this).

## Start ripper

1. Send `ripper.elf` to the PS5's payload loader, either with your usual
   payload sender or from a PC:

       nc <ps5-ip> 9021 < ripper.elf

   A notification on the PS5 says ripper is running.
2. On a phone or PC on the same network, open `http://<ps5-ip>:8790`.

## Run the test

1. On the **Drives** tab, pick **USB drive**.
2. Insert a disc and wait a few seconds. It's fine if ripper says it can't
   read the disc: the test runs anyway, and how the drive refuses is part of
   what we want to know.
3. Under **Drive test**, press **Test this drive** and wait.
4. Press **Download** when it's done. The zips also stay on the PS5 in
   `/data/ripper/diag/` if you'd rather copy them over FTP.

## Which discs

One run per disc, as many of these as you can:

1. Nothing in the drive.
2. A CD: a music CD and/or a CD game (PS1, Saturn, Sega CD...).
3. A DVD: a movie is fine.
4. A Blu-ray: a movie, or a PS3/PS4 game.
5. Each GameCube and Wii disc you have. These are what we're most curious
   about.
6. Anything else odd you have: Xbox, Dreamcast, PC games with copy
   protection.

## Send back

- Just the test zips. They're small (under 1 MB each) and the file names
  already say which drive and when. No need to rip anything or send any
  disc images: the test only reads a few sectors to see how the drive
  copes.
- Your drive's model and the firmware on it, if you know it (stock, an
  "MK" LibreDrive firmware, OmniDrive...), plus anything you've flashed.

## If something goes wrong

- If a test seems stuck, press **Cancel**. If the page stops answering,
  restart the PS5 and send `ripper.elf` again.
- Send `/data/ripper/ripper.log` from the PS5 along with the zips.

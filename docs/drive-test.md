# RipperPS drive test

Thanks for testing! The drive test finds out what your drive can read. You put
in discs one after another, and RipperPS tests each one as it goes in. At the
end you get one zip to attach to the drive report form.

The test only reads. It doesn't write to the disc, change the drive's settings
or touch its firmware. Nothing is sent anywhere by itself: you choose to send
the zip.

## What you need

- A jailbroken PS5 with a payload loader (etaHEN or similar), and RipperPS installed or `ripper.elf` to send.
- Your drive, plugged into the PS5 over USB. The PS5's own drive can be tested too.
- A phone or PC on the same network, to download the zip and send it.
- A GitHub account, to send the report.

## Run it

1. Open RipperPS, or send `ripper.elf` to the payload loader
   (`nc <ps5-ip> 9021 < ripper.elf`). A notification says it's running.
2. Open the **Drive test** tab: on a phone or PC at `http://<ps5-ip>:8790`, or
   in RipperPS under Settings. Press **Start** next to your drive.
3. If the drive is empty, RipperPS first looks at the drive alone. That takes a
   few seconds.
4. Put in a disc. RipperPS reads it, tests it for a minute or two, then ejects it
   and asks for the next one. A notification tells you when each disc is done.
5. For each disc, pick **what it really is** and add its name if you like.
   That's how we find out where RipperPS got a disc wrong.
6. Put in as many discs as you like, then press **Finish**.
7. Press **Download the zip**, then **Open the drive report form**. Most of the
   form is filled in already. Attach the zip, add what you know about the
   drive (model, firmware, the USB enclosure or adapter), and submit.

It's fine if RipperPS says it can't read a disc: the test runs anyway, and how
the drive refuses a disc is part of what we want to know.

## Which discs

Any disc helps. These help most, most wanted first:

| Disc | What it tells us | Drives most likely to read it |
| --- | --- | --- |
| GameCube | Whether OmniDrive's raw reads work, and Nintendo's scrambling comes off | OmniDrive (MediaTek LG/ASUS Blu-ray drives); old LG/Hitachi DVD drives |
| Wii | The same, on single and dual-layer discs | OmniDrive |
| Xbox 360 | The unlock, the security sector, which XGD it is | Kreon (Lite-On, Samsung, Philips DVD drives), OmniDrive |
| Xbox (original) | The same for XGD1 | Kreon, OmniDrive |
| Wii U | Raw Blu-ray reads with Nintendo's seed | OmniDrive on a Blu-ray drive |
| Dreamcast (GD-ROM) | Whether the high-density area reads | CD drives that read past the lead-out (Plextor, some Lite-On) |
| Saturn, Sega CD, PC Engine CD, 3DO, CD-i, PS1 | Identification, C2, subchannel and offsets for CD games | Any CD drive, Plextor best |
| Xbox One or Series | What a USB Blu-ray drive shows of them | Any Blu-ray drive |
| Music CD | Read offset, C2 and accurate audio | Any CD drive |
| PS4 or PS5 game | Whether a USB drive reads them at all | Any Blu-ray drive |
| 4K Blu-ray movie | Whether the movie comes out without bus encryption, through a normal read or OmniDrive's raw one | OmniDrive or LibreDrive on a 4K-capable drive |
| DVD or Blu-ray movie | Copy protection handling, read speed | Any |

## What's in the zip

- `summary.txt` and `session.json`: the drive, and for each disc what you said
  it is, what RipperPS saw, and what it could make of it.
- For each disc, RipperPS's disc report and the drive test's answers (`raw/`).
  The first sectors of the disc are in there, but no disc image: the test
  reads only a few sectors.
- RipperPS's log from the time of the test, with network addresses taken out.
- The drive's serial number is hashed, never kept as it is.

## If something goes wrong

- If a disc's test seems stuck, press **Finish**: what was found so far is kept.
- If the page stops answering, restart the PS5 and start RipperPS again. The
  test's files are in `/data/ripper/diag/` on the PS5 (reachable over FTP).
- The Log tab's **Download reports** zips everything in that folder, with the
  whole log.

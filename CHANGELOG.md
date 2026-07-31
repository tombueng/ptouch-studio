# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Pictures, QR codes and Code 128 barcodes beside the text, on either side.
  Barcode width follows from the module width, not from the space available:
  below roughly 0.25 mm per bar no scanner reads them, and both codes warn when
  the tape is too narrow for the content.
- Batch printing from a list file, one label per line.
- Templates for recurring labels.
- Translations: English, German, French, Spanish, Italian, Dutch and Polish,
  picked from the system language.
- Several copies now print as one continuous strip with a single cut at the end.
  Each cut costs the tape the distance between print head and cutter, so cutting
  per label could waste more tape than the labels themselves used.
- The setup wizard finds USB printers as well and skips pairing, the RFCOMM
  service and the custom backend for them.
- USB support. The printer is found through its IEEE 1284 device ID, so other
  printers on `/dev/usb/lp*` are left alone; setup needs neither pairing nor the
  RFCOMM service nor the custom backend over USB, and printing is roughly twenty
  times quicker.
- Printable heights now depend on the model: the narrower of tape width and print
  head decides. Two dozen models and the tape widths 21 and 36 mm were added,
  using the figures maintained by ptouch-print (GPL-3.0) — the numbers only, none
  of its code.

### Fixed

- Labels were cut a few millimetres short: the cutter sits behind the print head,
  so the tape has to travel that distance before it can be cut. Anything drawn
  near the end was lost.
- Printable heights were too conservative on 9, 12 and 18 mm tape — 12 mm now
  uses 10.72 instead of 9.88 mm, verified against the device with a frame.
- On tape wider than the print head the label was shifted and clipped along one
  edge: the page handed to CUPS was wider than the head, and the driver drops the
  excess from one side. The page is now never wider than the head, and the
  driver's own tape size check is turned off — the application asks the printer
  for the tape width itself, before every job.
- Status bytes were read one position off: byte 18 is the kind of reply, byte 19
  the phase. An idle printer could look busy, and a blinking one ready.
- Status frames arrive unprompted on phase changes and errors, so a query could
  return a stale frame. Waiting replies are now discarded before asking.
- The command line silently assumed 12 mm when the tape width could not be read
  and printed on whatever was loaded. It stops instead.
- A USB port that moved after replugging left the printer unreachable; the port
  is now looked up again when the configured path is gone.
- Closing the window during a status query could abort the application.
- Switching the automatic cut off cut the tape anyway, and a run of copies came
  out of the printer in two pieces. The driver knows two separate settings: one
  cuts between labels, the other feeds and cuts once the job ends. Only the first
  was ever set, and the second defaults to cutting, so the end of every job was
  severed — including the end of the first of the two jobs the copies were split
  across. Copies now travel in a single job, and switching the cut off turns off
  both.

## [0.2.0] — 2026-07-27

### Added

- Symbols and emoji on labels, with a palette for inserting them. Outline symbols
  print as vectors; colour pictographs are rendered as a greyscale image, because
  colour bitmap glyphs cannot be embedded into a PDF and would otherwise vanish
  between preview and tape.
- Font fallback to symbol families for characters the chosen font lacks.

### Fixed

- The application could abort when the window was closed while a status query was
  still running.

## [0.1.0] — 2026-07-27

First release.

### Added

- Label designer for Brother P-touch tape cassettes with a true-to-scale preview
  that marks the strip the print head cannot reach.
- Tape width read from the printer over Bluetooth: on start, periodically in the
  background, and again immediately before every print. A mismatch between the
  loaded tape and the layout is reported before material is spent.
- Automatic font size that fills the printable tape height, measured from the
  glyph extents rather than the font metrics.
- Any installed font, bold and italic, alignment, automatic or fixed label length,
  side margins, line spacing, frame, mirroring, copies.
- Setup wizard in the interface and as `ptouch-studio setup` on the command line:
  finds the device, pairs it, creates the RFCOMM service, the udev rule, the CUPS
  backend and the print queue.
- Sub-commands `print`, `status`, `scan`, `setup` and `check`.
- Own CUPS backend for RFCOMM ports that waits for the printer to finish, so
  multi-copy jobs come out complete.
- Packages for Debian/Ubuntu, Fedora/openSUSE and Arch, plus an AppImage.

[0.2.0]: https://github.com/tombueng/ptouch-studio/releases/tag/v0.2.0
[0.1.0]: https://github.com/tombueng/ptouch-studio/releases/tag/v0.1.0

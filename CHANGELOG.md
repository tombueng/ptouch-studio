# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

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

<div align="center">

<img src="data/icons/128/io.github.tombueng.PtouchStudio.png" width="96" alt="">

# P-touch Studio

**Labels for Brother P-touch tape cassettes — on Linux, over Bluetooth, without the vendor software.**

[![CI](https://github.com/tombueng/ptouch-studio/actions/workflows/ci.yml/badge.svg)](https://github.com/tombueng/ptouch-studio/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

</div>

<img src="data/screenshots/main.png" alt="P-touch Studio" width="900">

Brother ships no Linux application, and the tools that do exist treat the printer
as a destination only: they have no idea which tape is loaded. P-touch Studio asks
the device directly — before every print.

## What it does

- **Detects the tape instead of guessing.** The inserted cassette is read over
  Bluetooth, in the background and once more right before printing. If the layout
  does not match the loaded tape, you hear about it before material is spent.
- **A preview that tells the truth.** The print head cannot reach the edges of the
  tape — on 12 mm tape only 9.9 mm are printable. The preview shows that strip, and
  preview and print go through the same renderer.
- **Automatic font size** that really fills the printable height, measured from the
  glyphs rather than from the font metrics.
- Any installed font, alignment, fixed or growing length, margins, line spacing,
  frame, mirroring for transparent tape, copies.
- **Pictures, QR codes and barcodes** beside the text, on either side and scaled
  to the printable height. Barcodes are sized by module width rather than by the
  space available — a bar squeezed below a quarter millimetre is unreadable, and
  the application says so before the tape is spent.
- **Batch printing** from a list: one label per line, `|` splits lines within a
  label. Combined with a single cut at the end this is the cheap way to label a
  whole shelf.
- **Templates** for recurring labels — everything except the text is kept.
- **Symbols and emoji**, inserted from a palette. Outline symbols (★ ⚠ ✓ → Ω €)
  print as crisp vectors; pictographs such as 🔧 📦 come from a colour bitmap font
  that no PDF can embed, so those labels are rendered as a greyscale image instead
  — which is exactly what the preview then shows.
- **Guided setup**: find the device, pair it, create the Bluetooth port and the
  print queue — from inside the application.
- **Command line** for batches and scripts.
- **Localised** interface: English, German, French, Spanish, Italian, Dutch and
  Polish, chosen automatically from the system language.

## Installation

### Debian, Ubuntu, Linux Mint

```bash
sudo apt install ./ptouch-studio_0.2.0_amd64.deb
```

### Fedora, openSUSE

```bash
sudo dnf install ./ptouch-studio-0.2.0-1.x86_64.rpm
```

### Arch Linux

```bash
cd packaging/arch && makepkg -si
```

### Everything else (AppImage)

```bash
chmod +x P-touch_Studio-x86_64.AppImage
./P-touch_Studio-x86_64.AppImage
```

The AppImage carries the application. The CUPS backend has to live in the system,
because CUPS starts it as a process of its own:

```bash
sudo install -m 700 ptouch-cups-backend-x86_64 /usr/lib/cups/backend/rfcomm
```

Prebuilt packages are on the [releases page](https://github.com/tombueng/ptouch-studio/releases).

A Flatpak manifest lives in `packaging/flatpak/`, but note what a sandbox cannot
do: it has no `lp`, no `bluetoothctl` and no way to install a CUPS backend, so it
covers designing and exporting labels only. Printing wants one of the packages
above. See [docs/publishing.md](docs/publishing.md).

### Building from source

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

Needs Qt 6.2 or newer, a C++20 compiler and CMake ≥ 3.21. `libqrencode` is
optional — without it everything works except QR codes. At runtime it wants
`bluez`, `cups` and the P-touch driver (`printer-driver-ptouch` on Debian and
Ubuntu, `ptouch-driver` on Fedora and Arch).

## Setup

The wizard opens by itself on first start. It finds the printer, pairs it and
creates the Bluetooth port and the print queue; the system part asks for the
administrator password once.

The same from a terminal:

```bash
ptouch-studio setup      # find, pair and configure
ptouch-studio check      # show what is still missing
```

The printer may stay connected to your phone throughout — that does not interfere
with pairing.

## Usage

```bash
ptouch-studio                              # graphical interface

ptouch-studio print "Workshop"             # tape width comes from the printer
ptouch-studio print "Line 1" "Line 2"      # several lines
ptouch-studio print -w 24 -a left "Shelf A"
ptouch-studio print -n 3 "Three times"
ptouch-studio print --pdf sample.pdf "Test"   # check without spending tape

ptouch-studio print --qr "https://example.org" "Wiki"
ptouch-studio print --barcode "ABC-12345" "Stock"
ptouch-studio print --image logo.png --artwork-right "Workshop"
ptouch-studio print --from-file shelves.txt     # one label per line

ptouch-studio status                       # loaded tape, media, readiness
ptouch-studio status --json                # for scripts
```

`ptouch-studio print --help` lists every layout option.

## Supported devices

Developed and verified with the **PT-P710BT** (P-touch Cube Plus) over Bluetooth.

In principle every P-touch model works that
[ptouch-driver](https://github.com/philpem/printer-driver-ptouch) supports and that
offers a serial Bluetooth connection — PT-P300BT, PT-P700, PT-P750W, PT-E550W,
PT-P900W, PT-P950NW among them. Models whose print head differs from the PT-P700
series may have different printable heights; reports are welcome.

Tape widths: 3.5 / 6 / 9 / 12 / 18 / 24 mm.

## How it works

The path of a label:

```
layout (Qt) → PDF with an exact page size → CUPS → rastertoptch → RFCOMM backend → printer
```

Three parts of that are not obvious:

**The status query.** Asked with `ESC i S`, the printer answers with 32 status
bytes; byte 10 is the tape width in millimetres, and there is media type, error
state and print phase alongside. It costs no tape and takes milliseconds.

**The custom CUPS backend.** None of the backends shipped with CUPS cope with
RFCOMM: `serial` expects modem control lines an RFCOMM device does not carry, and
`file` opens non-blocking, which fails here. This backend opens blocking — that is
what brings the Bluetooth connection up in the first place — and waits for the
printer to report itself finished after the last byte. Without that wait, multiple
copies lose their last label, because the connection drops while the device is
still working.

**Colour emoji.** Qt happily draws them on screen, but a PDF cannot carry colour
bitmap glyphs — they disappear on the way to the printer without any error. Which
characters are affected cannot be decided by code point (⚡ U+26A1 comes from the
colour font, ★ U+2605 does not), so the fonts are asked directly: if no outline
family can draw a character, the whole label goes out as an image.

**Font scaling.** Qt converts point sizes against the device resolution. Scaling
the coordinate system on top of that for a 600 dpi PDF makes text come out 600/72
too large while every other measurement stays correct. Fonts are therefore defined
in device-independent pixel sizes. The [tests](tests/EngineTests.cpp) rasterise the
produced PDF and measure the ink instead of trusting the arithmetic.

More in [docs/architecture.md](docs/architecture.md).

## Troubleshooting

| Symptom | Cause and cure |
|---|---|
| "printer does not answer" | The device powers itself off after a while. Switching it on is enough. |
| "no access to /dev/rfcomm0" | Setup incomplete — `ptouch-studio check` names the missing step. |
| "/dev/rfcomm0 is missing" | `systemctl status ptouch-rfcomm.service` |
| Job stays in the queue | The printer was unreachable. CUPS holds the job for 300 s and prints it once the device is back. |
| Queue stopped | `cupsenable PT-Label` |

More in [docs/troubleshooting.md](docs/troubleshooting.md).

## Translations

The interface follows the system language. English is the source; German is
maintained alongside it. French, Spanish, Italian, Dutch and Polish are provided
as a starting point and would benefit from review by native speakers — the files
live in `translations/` and open in Qt Linguist.

Adding a language means adding it to `TRANSLATION_LANGUAGES` in `CMakeLists.txt`,
running `lupdate`, and filling in the new catalogue.

## Related projects

This project stands on the shoulders of two others, and it is worth knowing both:

**[ptouch-print](https://dominic.familie-radermacher.ch/projekte/ptouch-print/)**
by Dominic Radermacher (GPL-3.0) talks to P-touch printers directly over **USB**,
without CUPS, and takes PNG images or text on the command line. It also maintains
the table of print head widths and printable tape areas that this project's
geometry now builds on — those figures come from there and are verified against
real hardware across two dozen models. None of its code is used here; the two
programs take opposite routes to the same printers.

**[brother-ptouch-label-printer-on-linux](https://github.com/HenrikBengtsson/brother-ptouch-label-printer-on-linux)**
by Henrik Bengtsson collects the practical knowledge around `ptouch-print`:
building it, udev permissions, image formats, model quirks. If your printer is
connected by USB and you prefer the command line, start there.

P-touch Studio covers what those two do not: Bluetooth, a CUPS queue, a graphical
editor, and reading the loaded tape from the printer before every print.

## License

[MIT](LICENSE). Qt 6 is linked dynamically and stays under LGPL-3.0; the AppImage
bundles the Qt libraries in replaceable form.

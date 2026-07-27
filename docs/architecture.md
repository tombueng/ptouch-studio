# Architecture

## Building blocks

| File | Purpose |
|---|---|
| `src/Engine.*` | tape table, text layout, drawing, PDF output |
| `src/Status.*` | status query over RFCOMM, decoding of the 32 status bytes |
| `src/Printer.*` | handing jobs to CUPS and following them to the end |
| `src/Provision.*` | setup: pairing, port, backend, print queue |
| `src/Config.*` | configuration from environment, user and system files |
| `src/MainWindow.*`, `src/PreviewWidget.*`, `src/SetupDialog.*` | user interface |
| `src/Cli.*` | sub-commands without an interface |
| `src/CupsBackend.cpp` | standalone CUPS backend (`rfcomm:`) |

Layout and rendering live in a library with no dependency on the interface, so
preview, PDF export, command line and tests all share exactly the same maths.

## The path of a label

```
Spec ──computeLayout──> Layout ──render──> QPainter
                                             │
                                             ├─► screen (preview)
                                             └─► QPdfWriter ──lp──> CUPS
                                                                     │
                                                          rastertoptch (ptouch-driver)
                                                                     │
                                                        backend rfcomm ──Bluetooth──> printer
```

`Spec` describes the desired label, `Layout` the result of the calculation (font
size, length, baselines). Together they are enough to draw — which output device
receives it makes no difference.

## Geometry

Everything is measured in PostScript points, because that is how CUPS takes page
sizes (`PageSize=Custom.WIDTHxLENGTH`).

The page is portrait: width = tape width, height = label length. Drawing happens
lying down, so `writePdf` rotates the coordinate system by 90°. Inside the drawing
system x runs along the tape, y across it, origin at the top left.

### Printable height

The print head does not reach the edges of the tape. These values come from
Brother's Raster Command Reference for the PT-P700 series (180 dpi):

| Tape | Page width | Print dots | Printable |
|---:|---:|---:|---:|
| 3.5 mm | 10 pt | 24 | 3.4 mm |
| 6 mm | 17 pt | 32 | 4.5 mm |
| 9 mm | 26 pt | 50 | 7.1 mm |
| 12 mm | 34 pt | 70 | 9.9 mm |
| 18 mm | 51 pt | 112 | 15.8 mm |
| 24 mm | 68 pt | 128 | 18.1 mm |

The text block is centred inside that strip, not on the tape as a whole. Measured
on a PT-P710BT with 12 mm tape: 9.80 mm of ink within the 9.88 mm the head can
reach, centred, nothing clipped.

### Font sizes

Qt converts point sizes against the resolution of the output device. If the
coordinate system is scaled as well — by 600/72 for the PDF — the font is enlarged
twice: it comes out 8.33 times too large while margins and label length remain
correct.

`buildFont()` therefore defines fonts through `setPixelSize()` in a unit of 1/10
point. Pixel sizes are device independent; the coordinate system is scaled down
accordingly when the text is drawn. Screen and PDF then cannot disagree.

### Automatic sizing

A bisection search finds the largest font whose glyphs fit the printable height.
What counts is `tightBoundingRect()` — the actual extent of the letters. The font
metrics (`height()`) include room for ascenders and descenders that no real text
uses in full; scaling by those wastes about a third of the tape height.

## Status protocol

```
0x00 × 100    discard buffer
ESC @         initialise
ESC i S       request status
```

Reply: 32 bytes.

| Byte | Meaning |
|---:|---|
| 8, 9 | error bits (no tape, cover open, end of tape …) |
| 10 | tape width in mm |
| 11 | media type (laminated, non-laminated, heat-shrink …) |
| 18 | phase: 0 = waiting for data, 1 = printing |

Opening the RFCOMM port is what establishes the Bluetooth connection. After
closing it, the link needs about a second before it accepts again — hence the
retries in `queryStatusRetry()`.

## CUPS backend

The backends shipped with CUPS fail on RFCOMM:

- `serial` assumes modem control lines that an RFCOMM tty does not carry;
- `file` opens with `O_NONBLOCK`, which fails on RFCOMM.

The `rfcomm:` backend opens blocking — precisely that brings the connection up —
sets `CLOCAL`, writes the job data and **then waits for the printer to finish**: it
polls the status until the device is back in phase 0.

That wait is not cosmetic. After the last byte the printer is still transferring,
feeding tape and cutting. Close the connection before that and it discards the
rest — ask for three copies and two come out. Both behaviours were observed on a
PT-P710BT: two labels without the wait, three with it.

If no connection can be made the backend exits with status 6
(`CUPS_BACKEND_HOLD`): CUPS keeps the job and prints it once the printer is
reachable again, rather than throwing it away.

## Setup

`installSystem()` creates:

| Path | Purpose |
|---|---|
| `/etc/ptouch-studio/rfcomm.conf` | MAC, port index, channel |
| `/etc/udev/rules.d/70-ptouch-rfcomm.rules` | access for `lp` and the logged-in user |
| `/etc/systemd/system/ptouch-rfcomm.service` | binds the port at boot |
| CUPS queue | with the PPD matching the model |

Two traps are accounted for:

- **Membership in group `lp` is not enough**, because it only takes effect after
  the next login. The udev rule additionally sets the owner and `uaccess`.
- The service uses `RemainAfterExit=yes`. `systemctl enable --now` does **nothing**
  when the service already counts as active — the port would stay unbound. So it is
  restarted, not merely enabled.

Driver selection goes through the list from `lpinfo -m`: it looks for the longest
model in that list which the Bluetooth name starts with. The name carries the
serial number (`PT-P710BT3015`), so a direct comparison fails — and taking the
first match unchecked hands you a driver for an entirely different machine.

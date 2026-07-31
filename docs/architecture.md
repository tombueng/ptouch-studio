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

The print head does not reach the edges of the tape, and two limits apply at
once: what the tape allows, and how wide the head is. The narrower one wins.

| Tape | Page width | Dots @180 dpi | Dots @360 dpi |
|---:|---:|---:|---:|
| 3.5 mm | 10 pt | 24 | 48 |
| 6 mm | 17 pt | 32 | 64 |
| 9 mm | 26 pt | 52 | 106 |
| 12 mm | 34 pt | 76 | 150 |
| 18 mm | 51 pt | 120 | 234 |
| 21 mm | 60 pt | 124 | 248 |
| 24 mm | 68 pt | 128 | 320 |
| 36 mm | 102 pt | 192 | 454 |

Head widths per model are in `printerModels()`; nearly every P-touch has 128 dots
at 180 dpi, the exceptions being older 112 dot models and the 360 dpi PT-P900
series with 384 to 560 dots. A 36 mm tape would take 192 dots, but a 128 dot head
reaches only 18.06 mm of it — hence the clamp.

Both tables come from
[ptouch-print](https://dominic.familie-radermacher.ch/projekte/ptouch-print/)
(GPL-3.0), which maintains them against real hardware. Only the figures are used,
not the code. Verified here on a PT-P710BT with 12 mm tape: a frame drawn to the
full 10.72 mm prints complete, top and bottom.

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

## Two links, one protocol

The printer speaks the same language over both connections, only the device node
differs: `/dev/rfcomm0` for Bluetooth, `/dev/usb/lp*` for USB. `candidatePorts()`
lists both, and USB entries are filtered by their IEEE 1284 device ID
(`MFG:Brother;MDL:PT-P710BT;…`) — other printers share `/dev/usb/lp*`, and
writing status commands into a stranger's printer is not harmless.

USB needs markedly less machinery: no pairing, no RFCOMM service, and no custom
CUPS backend, because CUPS' own `usb` backend does the transport. It is also far
quicker — a label that takes 23 seconds over Bluetooth is through in under a
second — and the printer cannot fall asleep mid-job. What both need is a udev
rule, since the kernel hands printer nodes to group `lp` alone.

## The cutter sits behind the print head

Two consequences, both learned the hard way on a PT-P710BT:

**At the end of a label** the tape must travel the distance between head and
cutter before it can be cut, otherwise the last millimetres are cut away
unprinted — the label comes out shorter than calculated. Every job therefore
carries `ExtraMargin=5mm`. With a frame drawn to the edge, the closing line was
missing at 0 mm and complete at 5 mm.

`ExtraMargin` sets what the driver calls the top and bottom margin, so it applies
to both ends. Measured on a PT-P710BT with a 3 mm side margin in the layout: 6 mm
from the cut to the frame at the front, 5 mm at the back. The remaining
millimetre is the accuracy of the cut itself and not worth chasing.

**At the start of a label** the same distance is unavoidable waste: after a cut
the tape begins at the cutter, not at the head, so tape runs through unprinted
before the first dot can be set. Nothing in software prevents that — only cutting
less often does. Printing several labels with `AutoCut=False` pays that toll once
instead of once per label.

The side margin in the layout adds to both ends on top of this. At 3 mm it is the
larger half of the waste on short labels; anyone printing many small labels
should turn it down before touching anything else.

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

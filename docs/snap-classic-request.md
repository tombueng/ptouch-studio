# Snap Store: request for classic confinement

Post this in the Snapcraft forum under **Store requests → classic confinement**:
https://forum.snapcraft.io/c/store-requests/classic-confinement/26

Sign in with the same Ubuntu One account that owns the snap. Reviewers usually
answer within a few days and may ask follow-up questions.

---

**Title:** Request for classic confinement: ptouch-studio

**Body:**

`ptouch-studio` designs and prints labels on Brother P-touch label printers over
Bluetooth. Source: https://github.com/tombueng/ptouch-studio (MIT).

I am requesting classic confinement because the snap needs three kinds of access
that strict confinement cannot express, and each of them is essential rather than
convenience.

**1. Reading an RFCOMM character device**

The distinguishing feature of this application is that it asks the printer which
tape cassette is loaded, instead of letting the user guess. That means opening
`/dev/rfcomm0` (character major 216) and exchanging raw bytes with the printer:
`ESC i S` returns 32 status bytes whose byte 10 holds the tape width in
millimetres. Without it, every label would be laid out blind and material gets
wasted on mismatched tape.

No strict interface covers this device:

- `bluez` grants access to BlueZ over D-Bus, not to an RFCOMM tty;
- `serial-port` is limited to `/dev/tty{S,USB,ACM,AMA}*` and similar patterns
  through udev tagging, and does not match `/dev/rfcomm*`;
- `raw-usb` does not apply — the connection is Bluetooth, not USB.

**2. Installing a CUPS backend on the host**

Printing goes through CUPS, and CUPS starts its backends as separate processes
from `/usr/lib/cups/backend`. This project ships its own backend, because neither
`serial` nor `file` can open an RFCOMM port: `serial` expects modem control lines
that an RFCOMM tty does not carry, and `file` opens non-blocking, which fails
here. A confined snap cannot place an executable in that directory, and a backend
inside the sandbox would never be started by the host's cupsd.

**3. Setting up the port and the print queue**

The application includes a setup wizard that pairs the printer, writes a udev
rule, installs a systemd unit that binds the RFCOMM port, and creates the CUPS
queue with the driver matching the model. It shells out to `bluetoothctl`,
`rfcomm`, `systemctl`, `udevadm` and `lpadmin`, and edits files under `/etc`.
Those are host-level operations by nature.

**What I considered instead**

A strict snap could still design labels and export them as PDF, but it would lose
the tape detection, could not print, and could not set the printer up — which is
the whole point of the application. Splitting it into a confined front end plus a
separately installed system part would leave users with two installations to keep
in sync, and the front end alone offers little over any drawing program.

Deb, RPM and AppImage builds of the same version are published at
https://github.com/tombueng/ptouch-studio/releases for comparison; they install
exactly the same components that the snap needs to reach.

Thanks for considering it.

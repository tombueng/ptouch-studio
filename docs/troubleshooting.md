# Troubleshooting

Always start here:

```bash
ptouch-studio check
```

It lists every setup step on its own and names what is missing.

## The printer does not answer

```
Error: printer does not answer — switched on? (it powers off by itself after a while)
```

P-touch devices switch themselves off after a few minutes of inactivity. You can
verify that without spending tape:

```bash
sudo l2ping -c 2 <MAC>      # "Host is down" means the device is off
```

The Bluetooth address is in `/etc/ptouch-studio/rfcomm.conf`.

## No access to /dev/rfcomm0

```
Error: no access to /dev/rfcomm0 — setup incomplete
```

The udev rule is missing or was not applied:

```bash
ls -l /dev/rfcomm0                            # should show group lp and your user
cat /etc/udev/rules.d/70-ptouch-rfcomm.rules
sudo udevadm control --reload
sudo systemctl restart ptouch-rfcomm.service
```

Membership in group `lp` alone is not enough — it only takes effect at your next
login. That is why the rule also sets the owner.

## /dev/rfcomm0 is missing

```bash
systemctl status ptouch-rfcomm.service
sudo rfcomm show 0
```

The service uses `RemainAfterExit=yes`, so it still counts as active even if the
port was released in the meantime. When in doubt:

```bash
sudo systemctl restart ptouch-rfcomm.service
```

## The printer blinks red and refuses to settle

Ask it what is wrong:

```bash
ptouch-studio status
```

A mismatch between the job and the loaded tape is the usual cause — the printer
rejects the job and stays in that state afterwards.

**It has to be switched off and on again.** That is not a shortcut: the reset
command of the protocol (`ESC @`, available as `ptouch-studio reset`) clears the
data buffer but not the error state, and neither does a subsequent valid job.
Measured on a PT-P710BT: after a deliberate tape mismatch, `ESC @` left
`error 0x01 / status type 2` untouched, a correct label printed fine and the bit
still stood, and the lamp kept blinking until the power was cut.

`ptouch-studio reset` remains useful for a stuck buffer or an aborted transfer.

## The job stays in the queue

If the printer was unreachable when the job was submitted, CUPS holds it for 300
seconds and prints it once the device is back. If you would rather not wait:

```bash
lpstat -W not-completed -o          # what is pending
cancel -a PT-Label                  # discard everything
```

If CUPS stopped the queue:

```bash
cupsenable PT-Label
```

## Fewer labels than requested

That was the behaviour of the earlier, naive backend: it closed the connection
shortly after the last byte, while the printer was still working. The backend
shipped here waits for the finished report instead. Check which one is installed:

```bash
sudo file /usr/lib/cups/backend/rfcomm     # should be an ELF binary
```

## Text is off-centre or clipped

The printable height depends on the model; the values used here come from the
PT-P700 series. On a device with a different print head, a test print with a frame
makes the boundaries visible:

```bash
ptouch-studio print --frame "Test"
```

If that is visibly off, please open an issue with the model designation and the
tape width — the table lives in `src/Engine.cpp`.

## Wrong driver

After setup, check which PPD is in use:

```bash
lpstat -l -p PT-Label | head
grep -i modelname /etc/cups/ppd/PT-Label.ppd
```

If that names a different model, no matching driver was found. Set it up again,
stating the model explicitly:

```bash
sudo ptouch-studio setup-system --mac <MAC> --model PT-P710BT --queue PT-Label --owner "$USER"
```

## When nothing helps

Turn on verbose CUPS logging and repeat the job:

```bash
sudo cupsctl --debug-logging
ptouch-studio print "Test"
sudo tail -50 /var/log/cups/error_log
sudo cupsctl --no-debug-logging
```

Messages from the backend are prefixed with `INFO:` or `ERROR:`.

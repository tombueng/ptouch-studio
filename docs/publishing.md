# Publishing

Where this application can go, where it cannot, and what each channel needs.

## The short version

P-touch Studio is only half an application. The other half lives in the system: a
CUPS backend under `/usr/lib/cups/backend`, an RFCOMM port bound by a systemd
unit, and a udev rule that hands the port to the printing subsystem and to the
logged-in user. Package formats that install into the system carry all of it;
sandboxed formats can only carry the front end.

| Channel | Fit | Status |
|---|---|---|
| GitHub Releases (deb, rpm, AppImage, tarball) | complete | **published**, built by CI on every tag |
| AUR (Arch) | complete | PKGBUILD and `.SRCINFO` ready, upload needs an AUR account |
| Snap Store | complete, but classic confinement | `snapcraft.yaml` ready, needs a store account and a manual review |
| Flathub | front end only | manifest ready and locally verified; see the caveat below |
| Debian, Ubuntu, Fedora proper | complete | needs a sponsor and a maintainer process |

## GitHub Releases — done

Tagging is all it takes:

```bash
git tag -a v0.3.0 -m "P-touch Studio 0.3.0"
git push origin v0.3.0
```

The release workflow builds deb, rpm, tarball and AppImage, attaches the
standalone CUPS backend and a `SHA256SUMS` file, and publishes the release.

## AUR

Everything needed is in `packaging/arch/`. Uploading requires an account at
[aur.archlinux.org](https://aur.archlinux.org) with an SSH key registered in the
account settings, because the AUR is a set of git repositories reachable only
over SSH.

```bash
ssh-keygen -t ed25519 -C "aur"            # if you have no key yet
# add ~/.ssh/id_ed25519.pub in the AUR account settings, then:

git clone ssh://aur@aur.archlinux.org/ptouch-studio.git aur-ptouch-studio
cp packaging/arch/PKGBUILD packaging/arch/.SRCINFO aur-ptouch-studio/
cd aur-ptouch-studio
git add PKGBUILD .SRCINFO
git commit -m "Initial import: ptouch-studio 0.2.0"
git push
```

On an Arch machine, regenerate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`
whenever the PKGBUILD changes; the copy in this repository was written by hand
because it was produced on a Debian system.

## Snap Store

`packaging/snap/snapcraft.yaml` is ready. Two things stand in the way of an
unattended upload:

1. **Classic confinement.** The application opens `/dev/rfcomm0`, drives
   `bluetoothctl` and `rfcomm`, and installs a CUPS backend and a systemd unit.
   No strict-confinement interface covers that. Classic snaps are reviewed by
   hand, so the request has to state that reasoning.
2. **An account.** `snapcraft login` needs Ubuntu One credentials, and the name
   has to be registered first:

```bash
sudo snap install snapcraft --classic
snapcraft login
snapcraft register ptouch-studio
snapcraft                                  # builds the .snap
snapcraft upload --release=stable ptouch-studio_0.2.0_amd64.snap
```

## Flathub — with a caveat

`packaging/flatpak/io.github.tombueng.PtouchStudio.yml` builds and runs; it was
verified locally:

```bash
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/io.github.tombueng.PtouchStudio.yml
flatpak run io.github.tombueng.PtouchStudio
```

What works inside the sandbox: designing labels, the preview, PDF export, and
opening `/dev/rfcomm0` for the tape query (that is what `--device=all` is for).

What does not: the sandbox has no `lp`, no `bluetoothctl` and no `rfcomm`, so the
application can neither print nor set the printer up from inside it. Printing
would mean bundling the CUPS client tools into the Flatpak, and the setup would
still have to happen on the host.

A Flathub submission is therefore only honest if the description says plainly
that the system part has to be installed separately — and reviewers are right to
ask what `--device=all` is doing there. Given that the deb, rpm and AppImage
deliver the whole thing, Flathub buys little here. The manifest is kept for
anyone who wants the front end sandboxed anyway.

Submitting means opening a pull request against
[flathub/flathub](https://github.com/flathub/flathub) that adds the manifest, and
then answering the reviewers — which is a commitment, not a one-off action.

## AppImageHub

The catalogue at [appimage.github.io](https://github.com/AppImage/appimage.github.io)
lists AppImages through a pull request that adds one line pointing at the
release. It is a directory rather than a store, and it requires no account
beyond GitHub.

## Distribution repositories

Debian, Ubuntu, Fedora and openSUSE take packages through their own maintainer
processes — a sponsor for Debian, a review for Fedora, an account on the Open
Build Service for openSUSE. All of them expect a maintainer who stays around for
bug reports. Worth doing once the application has seen a few more printers than
the PT-P710BT it was written on.

# Distro packaging

Status as of v0.1.2: both routes below build and install cleanly (validated
in isolated containers, and rebuilt on every push via
`.github/workflows/packaging.yml`), but neither has actually been submitted
to a distribution yet. This documents the real remaining gap for each.

## Community QA tooling (lintian / namcap)

Beyond "does it build and install", both distros have their own automated
packaging-QA tools that reviewers/sponsors and AUR users actually run.
Neither is installed in the CI containers by default, so this was checked
manually by building the real package in a fresh `debian:bookworm` /
`archlinux:base-devel` container and running the tool against the result:

- **`lintian --pedantic`** on the built `.deb`: clean, zero warnings.
  Two things were found and fixed to get there:
  - No man page (`no-manual-page`, Debian Policy §12.1) - added
    `man/spnav-onshape-bridge.1`, installed by `make install` for all three
    install paths (manual, Debian, Arch).
  - `maintainer-script-calls-systemctl` - a false positive: `postinst` only
    *prints* the `systemctl --user ...` commands the user needs to run
    themselves (there's no system-wide unit for dpkg to enable), but
    lintian's check is a literal grep for the string and can't tell the
    difference. Documented and suppressed in
    `debian/spnav-onshape-bridge.lintian-overrides`.
- **`namcap`** on the built `.pkg.tar.zst` and on `PKGBUILD` itself:
  `PKGBUILD` itself is clean. One real finding on the package: `libx11` was
  listed under `depends`, but the binary never actually links against
  `libX11.so` - `libspnav`'s header only pulls in `X11/Xlib.h` when
  `libspnav` was built with X11 support, guarded by
  `#ifdef SPNAV_USE_X11`, and this project never calls the
  `spnav_x11_*()` functions that need it. Moved to `makedepends`. Three
  remaining namcap messages are benign, well-known noise for this kind of
  package and were left as-is rather than "fixed":
  - `Referenced library 'sh' is an uninstalled dependency` for
    `nss-trust-install.sh`'s `#!/bin/sh` shebang - `/bin/sh` is guaranteed
    on every Arch install, this is namcap treating a shebang like a shared
    library dependency.
  - `Dependency glibc detected and implicitly satisfied` - glibc is always
    present, informational only.
  - The auto-generated `-debug` package's build-id symlink "points to
    non-existing" the main package's binary path - inherent to how
    `makepkg` splits debug info into a separate package; the symlink is
    only meant to resolve once both packages are installed on a real
    system, not to be self-contained within the debug package alone.

Re-run these checks after any change to `debian/`, `packaging/arch/`, or
what gets installed where - see the commands under each section below to
build the package, then `lintian --pedantic *.deb` / `namcap PKGBUILD
*.pkg.tar.zst` in the respective container.

## Debian / Ubuntu (`debian/`)

All build dependencies (`libspnav-dev`, `libssl-dev`, `libx11-dev`) are
already official Debian/Ubuntu packages, and so is the runtime dependency
`spacenavd` - unlike Arch, there's nothing missing upstream.

Build locally:

```sh
sudo apt install build-essential devscripts debhelper libspnav-dev libssl-dev libx11-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../spnav-onshape-bridge_*.deb
```

The package is currently a Debian **native** package (`debian/source/format`
is `3.0 (native)`) - packaging and upstream source live in the same repo
with no separate `.orig.tar.*`. That's the pragmatic choice for a
single-maintainer project at this stage, but actual inclusion in the Debian
archive expects a non-native package (versioned upstream releases packaged
separately from Debian revisions) and goes through a much heavier process:
an ITP (Intent To Package) bug, a sponsor or becoming a Debian
Maintainer/Developer, and `lintian`-clean packaging reviewed by that sponsor.
Nothing here blocks that later if there's interest - it's simply not done.

## Arch Linux (`packaging/arch/`)

`libspnav` is an official Arch package (`extra` repo); `spacenavd` itself is
AUR-only (there is an existing `spacenavd` AUR package this project depends
on at runtime, listed as an `optdepends`, not a hard `depends`, since pacman
can't express "one of these AUR packages").

Build locally:

```sh
cd packaging/arch
makepkg -s
sudo pacman -U spnav-onshape-bridge-*.pkg.tar.zst
```

The `source=` line in `PKGBUILD` points at
`https://github.com/Pace17881/spnav-onshape-bridge` and fetches the tag
matching `pkgver`, so `makepkg -s` needs that tag to already be pushed.

Submitting to the AUR itself is comparatively lightweight once that's done:
create an AUR account, add an SSH key, and `git push` the `PKGBUILD` +
`.SRCINFO` to `ssh://aur@aur.archlinux.org/spnav-onshape-bridge.git` (see
the [AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines)).
Generate `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO` after any
`PKGBUILD` change - it's committed alongside the `PKGBUILD` in an AUR repo,
unlike here.

## What CI actually proves

`.github/workflows/packaging.yml` builds *and installs* both packages from
the commit under test on every push, so a packaging regression (a moved
file, a missing dependency, a broken post-install script) is caught
immediately - not just "the PKGBUILD/debian/rules files exist and look
right", but "a clean container can actually turn them into a working
install". It does not, and cannot, prove archive-acceptance requirements
like `lintian` cleanliness at Debian's standard, since this package hasn't
gone through that review.

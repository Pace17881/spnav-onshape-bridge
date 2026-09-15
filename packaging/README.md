# Distro packaging

Status as of v0.1.0: both routes below build and install cleanly (validated
in isolated containers, and rebuilt on every push via
`.github/workflows/packaging.yml`), but neither has actually been submitted
to a distribution yet. This documents the real remaining gap for each.

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

The `source=` line in `PKGBUILD` currently points at a placeholder URL
(`https://example.invalid/...`) because **this project has no public
repository yet**. Before any real use (including AUR submission), replace
it with the actual repository URL, e.g.:

```
source=("$pkgname::git+https://github.com/<you>/spnav-onshape-bridge.git#tag=v${pkgver}")
```

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

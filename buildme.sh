#!/bin/bash
set -e

source ~/.gnupg-repo.conf
# GNUPGHOME=~/.gnupg-repo
# REPO_GPG_KEY=something

build_deb=0
build_src=0
while getopts "ds" optn; do
case $optn in
    d)
        build_deb=1;
        ;;
    s)
        build_src=1;
        ;;
esac
done
shift $(($OPTIND - 1))

# build everything by default, but limit to a particular module if listed on the command line
modules="base db web gui python"
[ $# -gt 0 ] && modules="$*"

# sudo apt install libgsl-dev qtbase5-dev
# sudo apt install build-essential debhelper devscripts dpkg-dev

# Single source of truth for this machine's vendor SDK paths - exported so
# base/debian/rules' `BASE_FLI_SDK_DIR ?= ...` / `BASE_GXCCD_SDK_DIR ?= ...`
# pick them up too, without needing to edit that file per-site.
#
# These are *different* trees with different layouts: libfli wants the
# built source dir itself (libfli.h + libfli.a at the top level), libgxccd
# and Andor each want an SDK root with include/ and lib/ subdirectories.
# libfli and libgxccd were both pointed at the FLI path once, which
# silently produced empty rts2-drivers-fli/rts2-drivers-gxccd packages -
# CMake just skips the driver when detection fails, and debian/rules skips
# the missing binary.
#
# Exporting alone is not enough for the cmake branch below: these are cache
# PATH variables, and CMake never reads the environment for those, so each
# one also has to be repeated as -D on the cmake line or the driver is
# skipped with nothing but a STATUS line among a hundred others to say so.
# The export is what base/debian/rules picks up.
export BASE_ANDOR_SDK_DIR=/home/mates/andor
export BASE_FLI_SDK_DIR=/home/mates/fliusb/libfli
export BASE_GXCCD_SDK_DIR=/home/mates/libgxccd-0.9.0


if [ $build_deb == 0 ] && [ $build_src == 0 ]; then
    for tree in $modules; do
    #for tree in gui; do
    #for tree in base; do
        # python/ (and any future non-CMake tree) has nothing to
        # configure/build here - it only ever gets touched by the -d/-s
        # (dpkg-buildpackage) branches below, via its own debian/rules.
        if [ ! -f "$tree/CMakeLists.txt" ]; then
            echo "==> Skipping cmake build for $tree (no CMakeLists.txt)"
            continue
        fi
        rm -rf $tree/build
        cmake -S $tree -B $tree/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBASE_ANDOR_SDK_DIR=$BASE_ANDOR_SDK_DIR -DBASE_FLI_SDK_DIR=$BASE_FLI_SDK_DIR -DBASE_GXCCD_SDK_DIR=$BASE_GXCCD_SDK_DIR
        cmake --build $tree/build -j4
    done
fi

if [ $build_deb == 1 ] || [ $build_src == 1 ]; then
    # Output is a flat apt repository per distribution, dist/<os>/<codename>/
    # (e.g. dist/ubuntu/noble/), rsynced as-is to lascaux once deemed final:
    #   deb [trusted=yes] https://lascaux.asu.cas.cz/m/rts2ng/ubuntu/noble ./
    # Each distribution is built on its own host into its own directory, so
    # they never collide. Everything that is not a .deb (.ddeb, .buildinfo,
    # .changes, source packages) goes to dist/meta/<os>/<codename>/ instead -
    # apt-ftparchive recurses into subdirectories and would index the .ddebs.
    . /etc/os-release
    DIST_DIR="$(pwd)/dist/$ID/$VERSION_CODENAME"
    META_DIR="$(pwd)/dist/meta/$ID/$VERSION_CODENAME"
    mkdir -p "$DIST_DIR" "$META_DIR"

    # Package version = <upstream from debian/changelog>-<YYYYMMDD>~<o><ver>,
    # e.g. 0.1.0-20260917~u2404 or 0.1.0-20260917~d13. The date makes each
    # day's build supersede the previous one for apt; a second build on the
    # same day reuses the version and overwrites the files (install such a one
    # by hand with dpkg -i). The suffix makes a dist-upgraded machine pick up
    # the new release's build instead of keeping the old one as "same version".
    # The changelog entry is temporary - every debian/changelog is restored on
    # exit, so the committed changelogs stay untouched.
    BUILD_VERSION_REV="$(date +%Y%m%d)~${ID:0:1}${VERSION_ID//./}"

    stamp_changelog() {
        local cl="$1/debian/changelog" src ver maint
        src=$(dpkg-parsechangelog -l "$cl" -S Source)
        ver=$(dpkg-parsechangelog -l "$cl" -S Version)
        maint=$(dpkg-parsechangelog -l "$cl" -S Maintainer)
        [ -f "$cl.buildme-orig" ] || cp "$cl" "$cl.buildme-orig"
        {
            printf '%s (%s-%s) %s; urgency=medium\n\n' "$src" "${ver%-*}" "$BUILD_VERSION_REV" "$VERSION_CODENAME"
            printf '  * Automated build by buildme.sh.\n\n'
            printf ' -- %s  %s\n\n' "$maint" "$(date -R)"
            cat "$cl.buildme-orig"
        } > "$cl"
    }
    restore_changelogs() {
        local t
        for t in base db web gui python; do
            if [ -f "$t/debian/changelog.buildme-orig" ]; then
                mv -f "$t/debian/changelog.buildme-orig" "$t/debian/changelog"
            fi
        done
    }
    trap restore_changelogs EXIT
fi

if [ $build_deb == 1 ]; then
    echo "==> Building .deb packages ($BUILD_VERSION_REV)"

    for tree in $modules; do
        stamp_changelog $tree
        ( cd $tree && dpkg-buildpackage -us -uc -b )
        ( cd $tree && dh_clean )	# removes debian/.debhelper, obj-*, etc.
    done

    # dpkg-buildpackage drops its output one level up from the source dir
    # (i.e. here, at the repo root) - collect it into the repo directory,
    # replacing any older build of the same package (a flat repo could hold
    # several versions, but only the newest is ever wanted)
    for f in *.deb; do
        [ -e "$f" ] || continue
        rm -f "$DIST_DIR/${f%%_*}"_*.deb
        mv "$f" "$DIST_DIR/"
    done
    mv *.ddeb *.buildinfo *.changes "$META_DIR/" 2>/dev/null || true

    # apt index: Packages (per-.deb SHA256) and Release (hashes of Packages).
    # Signed InRelease only if REPO_GPG_KEY is set; otherwise clients need
    # [trusted=yes] (or sign on lascaux before publishing).
    echo "==> Indexing $DIST_DIR"
    (
        cd "$DIST_DIR"
        apt-ftparchive packages . > Packages
        gzip -9kf Packages
        rm -f Release InRelease
        apt-ftparchive -o APT::FTPArchive::Release::Origin=rts2ng \
                       -o APT::FTPArchive::Release::Codename="$VERSION_CODENAME" \
                       release . > "$META_DIR/Release"	# not in ., or it hashes itself
        mv -f "$META_DIR/Release" Release
        if [ -n "$REPO_GPG_KEY" ]; then
            gpg --batch --yes --local-user "$REPO_GPG_KEY" --clearsign -o InRelease Release
        fi
    )
fi

if [ $build_src == 1 ]; then
    echo "==> Building source packages"

    for tree in $modules; do
        stamp_changelog $tree
        ( cd $tree && dpkg-buildpackage -us -uc -S )
        ( cd $tree && dh_clean )
    done

    # -S output (the .dsc, orig/debian tarballs, and a source-only
    # .changes distinct from -b's binary .changes) lands in the same place
    # as the binary build's output above
    mv *.dsc *.tar.* "$META_DIR/" 2>/dev/null || true
    mv *.changes "$META_DIR/" 2>/dev/null || true
fi

if [ $build_deb == 1 ] || [ $build_src == 1 ]; then
    echo "==> Packages in $DIST_DIR:"
    ls -la "$DIST_DIR"
fi

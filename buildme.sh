#!/bin/bash
set -e

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
test $* && modules="$*"

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
export BASE_FLI_SDK_DIR=/home/torman/fliusb/libfli
export BASE_GXCCD_SDK_DIR=/home/torman/libgxccd-0.9.0


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
    DIST_DIR="$(pwd)/dist"
    rm -rf "$DIST_DIR"
    mkdir -p "$DIST_DIR"
fi

if [ $build_deb == 1 ]; then
    echo "==> Building .deb packages"

    for tree in $modules; do
        ( cd $tree && dpkg-buildpackage -us -uc -b )
        ( cd $tree && dh_clean )	# removes debian/.debhelper, obj-*, etc.
    done

    # dpkg-buildpackage drops its output one level up from the source dir
    # (i.e. here, at the repo root) - collect it into dist/
    mv *.deb *.ddeb *.buildinfo *.changes "$DIST_DIR/" 2>/dev/null || true
fi

if [ $build_src == 1 ]; then
    echo "==> Building source packages"

    for tree in $modules; do
        ( cd $tree && dpkg-buildpackage -us -uc -S )
        ( cd $tree && dh_clean )
    done

    # -S output (the .dsc, orig/debian tarballs, and a source-only
    # .changes distinct from -b's binary .changes) lands in the same place
    # as the binary build's output above
    mv *.dsc *.tar.* *.changes "$DIST_DIR/" 2>/dev/null || true
fi

if [ $build_deb == 1 ] || [ $build_src == 1 ]; then
    echo "==> Packages in $DIST_DIR:"
    ls -la "$DIST_DIR"
fi

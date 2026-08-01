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
modules="base db gui"
test $* && modules="$*"

# sudo apt install libgsl-dev qtbase5-dev
# sudo apt install build-essential debhelper devscripts dpkg-dev

# Single source of truth for this machine's FLI SDK path - exported so
# base/debian/rules' `BASE_FLI_SDK_DIR ?= ...` picks it up too, without
# needing to edit that file per-site.
export BASE_FLI_SDK_DIR=/home/mates/fliusb/libfli


if [ $build_deb == 0 ] && [ $build_src == 0 ]; then
    for tree in $modules; do
    #for tree in gui; do
    #for tree in base; do
        rm -rf $tree/build
        cmake -S $tree -B $tree/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBASE_FLI_SDK_DIR=$BASE_FLI_SDK_DIR
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

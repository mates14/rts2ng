#!/bin/bash
set -e

# sudo apt install libgsl-dev qtbase5-dev
# sudo apt install build-essential debhelper devscripts dpkg-dev

for tree in base db gui; do
#for tree in gui; do
	rm -rf $tree/build
	cmake -S $tree -B $tree/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBASE_FLI_SDK_DIR=/home/mates/fliusb/libfli
	cmake --build $tree/build -j4
done

echo "==> Building .deb packages"

DIST_DIR="$(pwd)/dist"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

for tree in base db gui; do
	( cd $tree && dpkg-buildpackage -us -uc -b )
	( cd $tree && dh_clean )	# removes debian/.debhelper, obj-*, etc.
done

# dpkg-buildpackage drops its output one level up from the source dir
# (i.e. here, at the repo root) - collect it into dist/
mv *.deb *.ddeb *.buildinfo *.changes "$DIST_DIR/" 2>/dev/null || true

echo "==> Packages in $DIST_DIR:"
ls -la "$DIST_DIR"

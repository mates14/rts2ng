# Moravian Instruments libgxccd

Moravian's library for their G/C cameras (USB or the ETH adapter), linked
statically into `rts2-camd-gxccd`. Closed source; bundled so the driver
builds without hunting for an SDK tree, the same way libfli is
(`../fli`).

| File | |
|---|---|
| `include/gxccd.h` | API, documented in the header itself |
| `lib/libgxccd.a` | x86-64 static library (sha256 `02ffccd3…866fdf31b`) |
| `LICENSE`, `README`, `CHANGELOG` | Moravian's, unchanged |

## Provenance

libgxccd **0.12.1**, downloaded on 2026-10-04 from
https://www.gxccd.com/cat?id=156 ("x86-64 Linux", package dated
2026-02-13), unchanged. Left out: `libgxccd.so` and the examples.
The same page offers i686, ARMv8 (AArch64), ARMv7 and ARMv6 builds; for
those, point `BASE_GXCCD_SDK_DIR` at an unpacked SDK, or add the
`libgxccd.a` here and extend the architecture check in
`camd/CMakeLists.txt`.

To update: download the new x86-64 package, replace the five files,
read `CHANGELOG` for API changes, rebuild `rts2-camd-gxccd`.

## Licence

Redistribution in binary form, unmodified, with the copyright notice
reproduced in the documentation (`debian/rules` installs `LICENSE` and
`README` as `/usr/share/doc/rts2-drivers-gxccd/*.libgxccd`). No reverse
engineering. `README` also carries the BSD notice of the inih INI parser
built into the library.

## Linking

`libgxccd.a` needs libusb-1.0 (system, dynamic), libm, libpthread and
librt (`timer_create`; part of libc since glibc 2.34) - see
`camd/gxccd/CMakeLists.txt`.

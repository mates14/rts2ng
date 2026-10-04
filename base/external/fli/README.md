# FLI (Finger Lakes Instrumentation) libfli and fliusb

Vendor code for FLI cameras, focusers and filter wheels, bundled so that
rts2-drivers-fli builds anywhere and the kernel module that makes the
devices reachable at all is versioned next to the drivers that use it.

| Directory | What | Built by |
|---|---|---|
| `libfli/` | FLI SDK 1.104 + RTS2 patches (Jan Štrobl, 2013) | `CMakeLists.txt` here, as the static `base_libfli` |
| `fliusb/` | FLI's Linux USB kernel module 1.5 + fixes for current kernels | DKMS, `dkms.conf` here, packaged as `fliusb-dkms` |

## Provenance

Imported from the RTS2 SVN, `https://svn.code.sf.net/p/rts-2/code/fliusb`
r12114 (libfli last changed r11518, 2013-07-26; fliusb r9864,
2012-01-17), unchanged except for dropped MSVC IDE state files. The
fliusb working version then came from the copy running at FLORES (D50
camera, verified 2026-07-09). `git log -- base/external/fli` separates
the vendor import from everything changed on top of it.

FLI has not released anything newer: its support page
(https://www.flicamera.com/support) still offers SDK 1.104 and modules
1.3.2 / 1.5 ("for kernel 4.18"). Community forks that keep the module
building: https://github.com/LCOGT/fliusb,
https://github.com/SAIL-Labs/FLI-linux.

Licence: BSD-style, FLI's copyright notice at the top of each file
(the module: "Dual BSD/GPL").

## Kernel API changes fixed in fliusb.c

All version-gated except the first:

- 5.19: `usb_maxpacket()` lost its direction argument (not gated - the
  module no longer builds before 5.19)
- 6.2 (old name removed in 6.15): `del_timer_sync()` → `timer_delete_sync()`
- 6.5: `get_user_pages()` lost its `vmas` argument (EDIT3)

and in the Makefile, `EXTRA_CFLAGS` → `ccflags-y`: current kbuild
ignores the former, which silently built the module without `-DSGREAD`
(scatter-gather reads straight into the caller's buffer).

When a new kernel breaks it, `dkms status` / the apt output of the kernel
upgrade says so; build by hand with `make` in `fliusb/`.

## Known limitation

The scatter-gather request (`s_sgreq` in fliusb.c) is a single global,
not per device: two FLI devices doing large reads at the same moment
would collide. Fine for one camera per machine.

## Not here

`~/libfli-1.32` (FLI SDK 1.32, raw usbfs + parallel port, used for the
IMG1024S "Dream Machine") is an older, different API and was left out.

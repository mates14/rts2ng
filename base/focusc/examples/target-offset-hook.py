#!/usr/bin/env python3
#
# rts2-focusc -F hook: plate-solve each completed exposure and show, live
# in a running ds9, how far a chosen sky target actually falls from a
# chosen reference pixel - drawn instead of printed. Two built-in target
# choices:
#
#  --polar   target = the celestial pole (dec=+-90). Point the telescope
#            near the pole (Polaris/sigma Octantis field) and the drawn
#            offset *cannot* be corrected by slewing - only by turning the
#            mount's physical alignment knobs while watching it shrink.
#            This is the original, more thoroughly exercised use of this
#            script (see the git history of this file).
#
#  --radec RA DEC   target = any sky position, in degrees. Unlike --polar,
#            this offset generally *can* be corrected by slewing - the
#            reference pixel is just "where I want this object to sit"
#            (frame center, a fiber head, a slit, a coronagraph mask...),
#            not a fixed mechanical thing. Nothing here closes that loop
#            automatically (no telescope-offset command is issued) - it
#            only draws the offset, same as --polar. Wiring the pixel
#            offset -> an actual slew is a natural next step if wanted:
#            the WCS already gives you the scale and orientation needed
#            to convert it into a real RA/Dec (or Alt/Az) nudge.
#
# ---------------------------------------------------------------------
# Why environment variables and a config file, not command-line flags,
# for the per-image invocation
# ---------------------------------------------------------------------
# rts2-focusc's -F option execs this script directly - no shell, and
# always with exactly one argument (the completed image's path):
#
#   execl(exePath, exePath, img_path, (char *) NULL);   // kernel/src/devclifoc.cpp
#
# So `-F` can never carry embedded arguments, no matter how it's quoted -
# `-F '/path/hook.py --radec 10 41'` would try to execute a file literally
# named "hook.py --radec 10 41", not run hook.py with those two arguments.
# Two ways around that, both supported here:
#
#  1. Set it up once, ahead of the exposure loop, and let the per-image
#     hook read persisted state - the right fit for --polar, since the
#     mount's rotation-center pixel doesn't change during a session:
#
#       target-offset-hook.py --set-center 512.3 498.7
#       target-offset-hook.py --polar
#       rts2-focusc -d C0 -e 5 -F /path/to/target-offset-hook.py
#
#  2. Environment variables, checked first and inherited unchanged
#     through execl() - convenient for a one-off run without a separate
#     setup step:
#
#       RTS2_TARGET_XY="500 500" RTS2_TARGET_MODE=radec \
#       RTS2_TARGET_RADEC="10.6833 41.2692" \
#       rts2-focusc -d C1 -e 1 -F /path/to/target-offset-hook.py
#
# For interactive re-targeting during a session (the expected case for a
# manual-control telescope - see `center` in this same directory), option
# 1 is the one that matters in practice: `center M31` just re-runs
# `target-offset-hook.py --radec ...` under the hood, so the already-
# running rts2-focusc -F loop from option 1 picks up the new target on
# its very next completed exposure, no restart needed.
#
# This is a starting point / sketch, not a shipped/installed tool - not
# built by CMakeLists.txt, not packaged, not covered by any test. See
# ds9-hook.sh in this same directory for the simpler seed this grew from,
# and base/STATUS.md's "hooking rts2-focusc up to ds9" section for the two
# constraints every -F hook has to respect regardless of what it does:
# stay silent on stdout (ConnFocus parses it as protocol data), and hand
# ds9 absolute paths (ds9/XPA resolves relative ones against its own cwd).
#
# Requires ~/pyrt (pyrt-phcat for star detection, pyrt-field-solve for the
# actual astrometry.net plate solve) and a running ds9 reachable via XPA.
#
# Verified in this form: the ds9 region-overlay syntax, the config/env
# round-trip, and the target-pixel/offset math (against a synthetic WCS
# header, both a dec=+-90 target and an arbitrary-radec target). NOT
# verified: an actual astrometry.net solve against a real starfield - no
# real sky imagery was available in the sandbox this was written in.
#
# Copyright (C) 2026 Martin Jelinek
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or (at
# your option) any later version.

import argparse
import json
import math
import os
import subprocess
import sys

import astropy.io.fits as fits
import astropy.wcs
from astropy.wcs.utils import proj_plane_pixel_scales

CONFIG_PATH = os.path.expanduser("~/.config/rts2/target-offset.json")


def load_config():
	"""Return the persisted {"x":, "y":, "mode":, ...} dict, or {} if
	nothing has been configured yet."""
	try:
		with open(CONFIG_PATH) as f:
			return json.load(f)
	except (FileNotFoundError, ValueError, json.JSONDecodeError):
		return {}


def save_config(**updates):
	"""Merge updates into the persisted config and write it back - so
	--set-center and --polar/--radec can each be run independently, in
	either order, without clobbering what the other one set."""
	config = load_config()
	config.update(updates)
	os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
	with open(CONFIG_PATH, "w") as f:
		json.dump(config, f)
		f.write("\n")


def resolve_target():
	"""Figure out (xy, mode, ra_or_none, dec_or_none, dec_sign_or_none) for
	the per-image hook invocation. Precedence: environment variables (a
	one-off override, see the module docstring) first, then the persisted
	config file. Returns None if nothing usable is configured either way."""
	xy_env = os.environ.get("RTS2_TARGET_XY")
	mode_env = os.environ.get("RTS2_TARGET_MODE")
	if xy_env and mode_env:
		x, y = (float(v) for v in xy_env.split())
		if mode_env == "polar":
			dec_sign = os.environ.get("RTS2_TARGET_DEC_SIGN")
			return (x, y), "polar", None, None, (float(dec_sign) if dec_sign else None)
		elif mode_env == "radec":
			radec_env = os.environ.get("RTS2_TARGET_RADEC")
			if not radec_env:
				print("polar-align: RTS2_TARGET_MODE=radec but "
					"RTS2_TARGET_RADEC is not set", file=sys.stderr)
				return None
			ra, dec = (float(v) for v in radec_env.split())
			return (x, y), "radec", ra, dec, None
		else:
			print(f"polar-align: unknown RTS2_TARGET_MODE={mode_env!r}",
				file=sys.stderr)
			return None

	config = load_config()
	if "x" not in config or "y" not in config or "mode" not in config:
		print("polar-align: no target configured yet - run with "
			"--set-center X Y and --polar or --radec RA DEC first "
			"(or set RTS2_TARGET_XY/RTS2_TARGET_MODE/...)", file=sys.stderr)
		return None

	xy = (float(config["x"]), float(config["y"]))
	if config["mode"] == "polar":
		return xy, "polar", None, None, config.get("dec_sign")
	elif config["mode"] == "radec":
		return xy, "radec", config.get("ra"), config.get("dec"), None
	print(f"polar-align: unknown mode {config['mode']!r} in {CONFIG_PATH}",
		file=sys.stderr)
	return None


def solve_field(fits_path, time_limit=15):
	"""Run pyrt-phcat (star detection) then pyrt-field-solve (astrometry.net
	plate solve, which replaces fits_path in place with a real WCS on
	success). Returns True on success, False otherwise - never raises, so a
	failed solve just means this hook quietly does nothing for this image."""
	base, _ = os.path.splitext(fits_path)
	cat_path = base + ".cat"

	try:
		subprocess.run(["pyrt-phcat", fits_path], check=True,
			capture_output=True, timeout=60)
	except Exception as e:
		print(f"polar-align: pyrt-phcat failed (continuing without a "
			f"catalog, field-solve will detect stars itself): {e}",
			file=sys.stderr)
		cat_path = None

	cmd = ["pyrt-field-solve", fits_path, "--time", str(time_limit)]
	if cat_path and os.path.exists(cat_path):
		cmd += ["--cat", cat_path]

	try:
		result = subprocess.run(cmd, capture_output=True,
			timeout=time_limit + 15)
	except subprocess.TimeoutExpired:
		print("polar-align: pyrt-field-solve timed out", file=sys.stderr)
		return False

	if result.returncode != 0:
		print(f"polar-align: field solve failed for {fits_path}",
			file=sys.stderr)
		return False
	return True


def target_pixel(wcs, ra, dec):
	"""Where does sky position (ra, dec) - both in degrees - fall in this
	image's pixel coordinates, according to its WCS?"""
	x, y = wcs.all_world2pix(ra, dec, 0)
	return float(x), float(y)


def pole_radec(wcs, dec_sign):
	"""RA is degenerate at the pole; ask all_world2pix about it at the
	field's own reference RA instead of an arbitrary one - keeps the SIP
	inversion well inside its normal operating range rather than asking it
	to converge exactly at the singularity."""
	return float(wcs.wcs.crval[0]), 90.0 * dec_sign


def guess_dec_sign(wcs):
	"""No dec_sign configured: guess north/south from the solved field's
	own declination. Wrong near the equator, but --polar is only
	meaningful pointed near a pole in the first place."""
	return 1.0 if float(wcs.wcs.crval[1]) >= 0 else -1.0


def push_to_ds9(fits_path, ref_xy, target_xy, offset_arcmin, label):
	"""Show the image, the reference pixel (green circle - the mount axis
	for --polar, or wherever you want the target sitting for --radec), and
	where the WCS says the target actually is right now (red cross), plus
	a connecting line and a text label giving the offset in arcmin.
	Deliberately doesn't try to describe the direction in words (compass/
	knob/slew terms) - for --polar that mapping is instrument-specific
	(camera rotation relative to the knobs) and would need its own
	calibration; for --radec it would need an actual telescope-offset
	command, not attempted here (see the module docstring). The drawn
	line already answers "which way" for whoever is looking at the
	screen."""
	abs_path = os.path.abspath(fits_path)
	subprocess.run(["xpaset", "-p", "ds9", "fits", abs_path],
		stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

	mid_x = (ref_xy[0] + target_xy[0]) / 2.0
	mid_y = (ref_xy[1] + target_xy[1]) / 2.0
	region = (
		"image\n"
		f"circle({ref_xy[0]:.2f},{ref_xy[1]:.2f},15) # color=green width=2\n"
		f"point({target_xy[0]:.2f},{target_xy[1]:.2f}) # point=cross 20 color=red width=2\n"
		f"line({ref_xy[0]:.2f},{ref_xy[1]:.2f},{target_xy[0]:.2f},{target_xy[1]:.2f}) # color=cyan\n"
		f"text({mid_x:.2f},{mid_y:.2f}) # color=yellow text={{{offset_arcmin:.2f}' {label}}}\n"
	)
	subprocess.run(["xpaset", "ds9", "regions"], input=region.encode(),
		stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def process(fits_path, time_limit):
	resolved = resolve_target()
	if resolved is None:
		return
	ref_xy, mode, ra, dec, dec_sign = resolved

	if not solve_field(fits_path, time_limit=time_limit):
		return

	with fits.open(fits_path) as hdul:
		w = astropy.wcs.WCS(hdul[0].header)

	if not w.has_celestial:
		print(f"polar-align: {fits_path} has no celestial WCS after "
			"solving", file=sys.stderr)
		return

	if mode == "polar":
		sign = dec_sign if dec_sign is not None else guess_dec_sign(w)
		ra, dec = pole_radec(w, sign)
		label = "off pole"
	else:
		label = "off target"

	target_xy = target_pixel(w, ra, dec)

	scale_deg = proj_plane_pixel_scales(w)  # degrees/pixel, one per axis
	scale_arcsec = sum(scale_deg) / len(scale_deg) * 3600.0

	dx = target_xy[0] - ref_xy[0]
	dy = target_xy[1] - ref_xy[1]
	offset_px = math.hypot(dx, dy)
	offset_arcmin = offset_px * scale_arcsec / 60.0

	push_to_ds9(fits_path, ref_xy, target_xy, offset_arcmin, label)
	print(f"polar-align: target ({ra:.4f},{dec:.4f}) at pixel "
		f"({target_xy[0]:.1f},{target_xy[1]:.1f}), {offset_arcmin:.2f}' "
		f"from reference pixel ({ref_xy[0]:.1f},{ref_xy[1]:.1f})",
		file=sys.stderr)


def main():
	parser = argparse.ArgumentParser(description=__doc__,
		formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("fits_file", nargs="?",
		help="completed exposure, as passed by rts2-focusc -F")
	parser.add_argument("--set-center", "--xy", dest="set_center",
		nargs=2, type=float, metavar=("X", "Y"),
		help="set the reference pixel (mount rotation-center for --polar, "
			"or wherever you want the target for --radec) and exit")
	parser.add_argument("--polar", action="store_true",
		help="set target mode to the celestial pole and exit")
	parser.add_argument("--dec-sign", type=float, choices=(1.0, -1.0), default=None,
		help="with --polar: +1 north / -1 south; default: guessed per-image "
			"from the solved field's own declination")
	parser.add_argument("--radec", nargs=2, type=float, metavar=("RA", "DEC"),
		help="set target mode to this fixed sky position (degrees) and exit")
	parser.add_argument("--time", type=int, default=15, metavar="SEC",
		help="time limit for the astrometry.net solve (default: 15)")
	args = parser.parse_args()

	if args.set_center:
		save_config(x=args.set_center[0], y=args.set_center[1])
		return
	if args.polar:
		save_config(mode="polar", dec_sign=args.dec_sign)
		return
	if args.radec:
		save_config(mode="radec", ra=args.radec[0], dec=args.radec[1])
		return

	if not args.fits_file:
		parser.error("fits_file is required unless one of --set-center/"
			"--polar/--radec is given")

	try:
		process(args.fits_file, args.time)
	except Exception as e:
		# ConnFocus (the RTS2-side caller) must never see this hook crash
		# out or spew unexpected stdout - report and move on.
		print(f"polar-align: error processing {args.fits_file}: {e}",
			file=sys.stderr)


if __name__ == "__main__":
	main()

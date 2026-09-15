#!/usr/bin/python3
"""
Sky survey for RTS2 – idle-time template exposures.

Position-selection algorithm
─────────────────────────────
1. Draw dec ∝ cos(dec)/HA_window(dec) so that long-term coverage of the
   sphere is uniform.  Equatorial declinations, which have narrow
   observability windows, receive proportionally more visits.

2. Draw |HA| ∝ 1/(|HA|+ε), favouring the meridian while allowing
   east-west room when the Moon blocks the meridian.  ε = HA_EPSILON
   sets the half-width of the Lorentzian preference (default 30 min).

3. For eastern positions (HA < 0) shift the POINTING east by FLIP_MARGIN
   (default 10 min = 2.5°): the 10-min sequence then ends at the drawn
   sky position without ever crossing the meridian – no GEM flip risk.

4. Reject if the drawn sky position is within MIN_MOON_SEP of the Moon
   or MIN_SUN_SEP of the Sun.

Multi-camera
─────────────
C1 (MASTER_CAMERA) drives the mount and picks positions.  C2 follows by
waiting for the telescope to settle at the new position and then exposes
the same number of frames.

Filter strategy
────────────────
Moon above horizon → C1: i,   C2: r
Moon below horizon → C1: clear, C2: g
"""

import sys
import time
import datetime
import numpy as np
import ephem
import rts2.scriptcomm as sc

# ---------------------------------------------------------------------------
# Survey configuration
# ---------------------------------------------------------------------------
FOV_RADIUS   =  1.75   # half of 3.5° FOV (degrees) – sets pointing limits
MIN_ALT      = 30.0    # minimum pointing altitude (degrees)
HA_LIMIT     = 120.0   # maximum |HA| sampled (degrees = 8 h)
HA_EPSILON   =  7.5    # Lorentzian half-width for meridian preference (deg = 30 min)
FLIP_MARGIN  =  2.5    # GEM safety margin east of drawn position (deg = 10 min)
MIN_MOON_SEP = 60.0    # lunar exclusion radius (degrees)
MIN_SUN_SEP  = 50.0    # solar exclusion radius (degrees, safety)
SUN_STOP_ALT = -10.0   # stop when Sun reaches this altitude (degrees)

N_FRAMES     =  4      # exposures per position
FRAME_TIME   = 120.0   # seconds per frame

MASTER_CAMERA  = 'C1'

SETTLE_WAIT    =  2.0  # seconds to wait after telescope settles
MOVE_TIMEOUT   = 180.0 # seconds: telescope settle timeout
FOLLOW_TIMEOUT = 600.0 # seconds: follower waits for master's next slew

# ---------------------------------------------------------------------------

class SurveyScript(sc.Rts2Comm):

	def __init__(self):
		sc.Rts2Comm.__init__(self)
		self.lat     = None   # degrees N
		self.lon     = None   # degrees E
		self.elev    = None   # metres
		self.dec_min = None   # southern pointing limit (degrees)
		self.dec_max = None   # northern pointing limit (degrees)
		self._w_max  = None   # cached dec-weight ceiling for rejection sampling

	# -------------------------------------------------------------------
	# Site
	# -------------------------------------------------------------------

	def get_site_params(self):
		tel = self.getDeviceByType(sc.DEVICE_TELESCOPE)
		self.lon  = self.getValueFloat('LONGITUD', device=tel)
		self.lat  = self.getValueFloat('LATITUDE',  device=tel)
		self.elev = self.getValueFloat('ALTITUDE',  device=tel)
		# Pointing limits: ensure a full FOV clears MIN_ALT on both ends
		self.dec_min = self.lat - (90.0 - MIN_ALT) + FOV_RADIUS
		self.dec_max = 90.0 - FOV_RADIUS

	def make_observer(self):
		obs = ephem.Observer()
		obs.lat       = np.deg2rad(self.lat)
		obs.lon       = np.deg2rad(self.lon)
		obs.elevation = self.elev
		obs.date      = datetime.datetime.utcnow()
		return obs

	# -------------------------------------------------------------------
	# Geometry
	# -------------------------------------------------------------------

	def ha_window(self, dec_deg):
		"""
		Half-width of the HA window (degrees) during which dec_deg is
		above MIN_ALT, clipped to HA_LIMIT.
		"""
		sin_a = np.sin(np.deg2rad(MIN_ALT))
		sin_p = np.sin(np.deg2rad(self.lat))
		cos_p = np.cos(np.deg2rad(self.lat))
		sin_d = np.sin(np.deg2rad(dec_deg))
		cos_d = np.cos(np.deg2rad(dec_deg))
		denom = cos_p * cos_d
		if abs(denom) < 1e-9:
			return 0.0
		cos_ha = (sin_a - sin_p * sin_d) / denom
		if cos_ha >= 1.0:
			return 0.0       # never rises to MIN_ALT
		if cos_ha <= -1.0:
			return HA_LIMIT  # always above MIN_ALT; clip to our search limit
		return min(np.rad2deg(np.arccos(cos_ha)), HA_LIMIT)

	def _dec_weight(self, dec_deg):
		"""
		Sampling weight for declination.
		P(dec) ∝ cos(dec)/ha_window(dec) gives uniform long-term sky coverage:
		each solid-angle element gets visited proportionally regardless of
		how long it spends above the horizon each night.
		"""
		w = self.ha_window(dec_deg)
		if w < 0.5:
			return 0.0
		return np.cos(np.deg2rad(dec_deg)) / w

	def _cache_w_max(self):
		"""Compute and cache the dec-weight ceiling used in rejection sampling."""
		grid = np.linspace(self.dec_min, self.dec_max, 500)
		self._w_max = max(self._dec_weight(d) for d in grid) * 1.05

	# -------------------------------------------------------------------
	# Stop condition
	# -------------------------------------------------------------------

	def is_stop(self):
		"""Return True when the Sun has risen above SUN_STOP_ALT."""
		obs = self.make_observer()
		sun = ephem.Sun()
		sun.compute(obs)
		return float(sun.alt) > np.deg2rad(SUN_STOP_ALT)

	# -------------------------------------------------------------------
	# Position selection
	# -------------------------------------------------------------------

	def pick_position(self):
		"""
		Return (ra_deg, dec_deg) for the telescope pointing, or None.

		The drawn (dec, HA) is the survey sky position the sequence will
		cover.  For eastern draws (HA < 0) the returned RA corresponds to
		a pointing FLIP_MARGIN degrees further east; after 10 min of
		tracking the scope arrives at the drawn sky position without
		crossing the meridian.
		"""
		obs  = self.make_observer()
		moon = ephem.Moon(); moon.compute(obs)
		sun  = ephem.Sun();  sun.compute(obs)
		lst  = np.rad2deg(float(obs.sidereal_time()))

		for _ in range(10000):
			# -- Step 1: draw dec by rejection sampling -----------------
			dec = np.random.uniform(self.dec_min, self.dec_max)
			if np.random.uniform(0.0, self._w_max) > self._dec_weight(dec):
				continue

			ha_win = self.ha_window(dec)   # already clipped to HA_LIMIT

			# -- Step 2: draw |HA| via inverse CDF of 1/(|HA|+ε) -------
			#    CDF^{-1}(u) = ε·[((ha_win+ε)/ε)^u − 1]
			u      = np.random.uniform(0.0, 1.0)
			abs_ha = HA_EPSILON * (((ha_win + HA_EPSILON) / HA_EPSILON) ** u - 1.0)
			ha     = abs_ha * np.random.choice([-1.0, 1.0])

			# -- Step 3: GEM flip margin --------------------------------
			# Eastern draw: point FLIP_MARGIN east of the drawn position.
			# The 10-min sequence ends at the drawn sky position; the mount
			# never needs to flip because it starts even further east.
			ha_point = ha - FLIP_MARGIN if ha < 0.0 else ha

			# -- Step 4: check the drawn sky position -------------------
			ra_sky = (lst - ha) % 360.0

			moon_sep = np.rad2deg(ephem.separation(
				(float(np.deg2rad(ra_sky)), float(np.deg2rad(dec))),
				(float(moon.ra), float(moon.dec))))
			if moon_sep < MIN_MOON_SEP:
				continue

			sun_sep = np.rad2deg(ephem.separation(
				(float(np.deg2rad(ra_sky)), float(np.deg2rad(dec))),
				(float(sun.ra), float(sun.dec))))
			if sun_sep < MIN_SUN_SEP:
				continue

			return (lst - ha_point) % 360.0, dec

		return None

	# -------------------------------------------------------------------
	# Filter selection
	# -------------------------------------------------------------------

	def select_filter(self, camera):
		"""Return the filter for this camera given the current Moon state."""
		obs = self.make_observer()
		moon = ephem.Moon()
		moon.compute(obs)
		moon_up = float(moon.alt) > 0.0
		if camera == MASTER_CAMERA:
			return 'i' if moon_up else 'clear'
		else:
			return 'r' if moon_up else 'g'

	# -------------------------------------------------------------------
	# Telescope / imaging helpers
	# -------------------------------------------------------------------

	def waitIdle2(self):
		"""Wait until the telescope is within 1 arcmin of its target."""
		start = time.time()
		while True:
			td = self.getValueFloat('target_distance', 'T0')
			if td is not None and td < 1.0 / 60.0:
				time.sleep(SETTLE_WAIT)
				break
			if time.time() - start > MOVE_TIMEOUT:
				self.log('W', 'survey: telescope settle timeout')
				break
			time.sleep(0.25)

	def take_sequence(self, mee, flt, abort_check=None):
		"""Expose N_FRAMES frames in filter flt."""
		self.setValue('filter', flt)
		self.setValue('exposure', FRAME_TIME)
		for i in range(N_FRAMES):
			if abort_check is not None and abort_check():
				self.log('I', '%s aborted at frame %d/%d' % (mee, i + 1, N_FRAMES))
				return
			self.log('I', '%s frame %d/%d  filter=%s' % (mee, i + 1, N_FRAMES, flt))
			img = self.exposure()
			if img:
				self.process(img)

	# -------------------------------------------------------------------
	# Master camera loop
	# -------------------------------------------------------------------

	def run_master(self):
		camera = self.getRunDevice()
		mee = 'survey/M'
		self.setValue('SHUTTER', 'LIGHT')
		self.setValue('TRACKING', 1, 'T0')

		while True:
			if self.is_stop():
				self.log('I', '%s stopping at dawn' % mee)
				break

			pos = self.pick_position()
			if pos is None:
				self.log('W', '%s no valid position found, waiting 60 s' % mee)
				time.sleep(60)
				continue

			ra, dec = pos
			flt = self.select_filter(camera)
			self.log('I', '%s -> RA=%.4f°  Dec=%.4f°  filter=%s' % (mee, ra, dec, flt))

			self.radec(ra, dec)
			time.sleep(2)
			self.waitTargetMove()
			self.waitIdle2()

			self.take_sequence(mee, flt)

	# -------------------------------------------------------------------
	# Follower camera loop
	# -------------------------------------------------------------------

	def run_follower(self):
		camera = self.getRunDevice()
		mee = 'survey/F'
		self.setValue('SHUTTER', 'LIGHT')

		while True:
			if self.is_stop():
				self.log('I', '%s stopping at dawn' % mee)
				break

			# Wait for the telescope to be settled (handles both the case
			# where the master is mid-slew and where it is already stable).
			self.waitIdle2()

			this_move_end = self.getValueFloat('move_end', 'T0')
			if this_move_end is None:
				time.sleep(2)
				continue

			flt = self.select_filter(camera)

			def new_slew():
				ms = self.getValueFloat('move_started', 'T0')
				return ms is not None and ms > this_move_end + 1.0

			self.take_sequence(mee, flt, abort_check=new_slew)

			# Wait for the master to start the next slew before looping
			deadline = time.time() + FOLLOW_TIMEOUT
			while True:
				if self.is_stop() or time.time() > deadline:
					break
				if new_slew():
					break
				time.sleep(2)

	# -------------------------------------------------------------------
	# Entry point
	# -------------------------------------------------------------------

	def run(self):
		camera = self.getRunDevice()
		self.get_site_params()
		self._cache_w_max()
		self.log('I', 'survey: camera=%s  lat=%.3f°  lon=%.3f°  dec=[%.2f°, %.2f°]'
			% (camera, self.lat, self.lon, self.dec_min, self.dec_max))
		if camera == MASTER_CAMERA:
			self.run_master()
		else:
			self.run_follower()


if __name__ == '__main__':
	try:
		SurveyScript().run()
	except KeyboardInterrupt:
		sys.exit(0)

# Local Variables:
# tab-width: 4
# python-indent-offset: 4
# indent-tabs-mode: t
# End:

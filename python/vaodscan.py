#!/usr/bin/python

import rts2.scriptcomm
import time
from astropy.coordinates import SkyCoord, EarthLocation, AltAz
from astropy.time import Time
import astropy.units as u

s = rts2.scriptcomm.Rts2Comm()

s.setValue('exposure',10)
s.setValue('filter','B')
s.setValue('IMAGETYP','object')
#sidereal tracking
tel = s.getDeviceByType(rts2.scriptcomm.DEVICE_TELESCOPE)
s.setValue('TRACKING',2,tel)

# Observatory location
observatory = EarthLocation(lat=49.9090806*u.deg, lon=14.7819092*u.deg, height=530*u.m)

def point_altaz(alt,az):
	# Get current time
	obs_time = Time.now()

	# Create AltAz coordinate
	altaz_coord = SkyCoord(alt=alt*u.deg, az=az*u.deg, frame=AltAz(location=observatory, obstime=obs_time))

	# Transform to RA/Dec (ICRS)
	radec_coord = altaz_coord.icrs

	# Point telescope using RA/Dec
	s.radec(radec_coord.ra.deg, radec_coord.dec.deg)
	time.sleep(2)
	#s.waitIdle(tel,180)
	s.waitTargetMove()
	img=s.exposure()
	s.process(img)

alt_r=range(15,76,2)
for alt in alt_r:
	point_altaz(alt,75)
for alt in alt_r:
	point_altaz(alt,90)
for alt in alt_r:
	point_altaz(alt,105)

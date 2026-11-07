#!/usr/bin/python

# D50 C0 sky flats and calibration darks (target 2).
# (C) 2010 Petr Kubanek
#
# Moved here from lascaux50:/etc/rts2/flatC0_nobin.py (2026-11-07) [JS]. That
# one imported the local 2023 copy /etc/rts2/flats.py and ran with
# domeDevice=None, so the morning darks were taken under the open roof. Now
# the packaged rts2.flats closes the dome after the morning flats (the dome
# parks the mount first) and the darks run under a closed roof.
#
# Please see flats.py for details of the parameters.

from rts2.flats import FlatScript, Flat

f = FlatScript(eveningFlats=[Flat('z'),Flat('R'),Flat('i'),Flat('r'),Flat('g'),Flat('N')],maxDarks=15,expTimes=range(1,20))

# Zkusime zvednout optimalFlat na 20000, snad to andor utahne... je kazdopadne treba kvuli tomu zvysit hodnotu VSPEED na tu nejpomalejsi.
# A soucasne menim i shifty.
f.flatLevels(defaultNumberFlats=15,biasLevel=1225,optimalFlat=20000,optimalRange=0.3,allowedOptimalDeviation=0.1,sleepTime=30,shiftRa=-20.0/3600.0,shiftDec=-20.0/3600.0)

f.darkFilter('DF')

# We want define this, as this implies not making mount shift-moves until brigtness range is reached... And also, reading subwindow speeds-up the readout...
f.setSubwindow(waitingSubWindow='400 400 200 200')

# domeDevice defaults to 'DOME': closed after the morning flats, before the darks
f.run()

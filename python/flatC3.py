#!/usr/bin/python

# Example configuation of flats. 
# (C) 2010 Petr Kubanek
#
# Please see flats.py file for details about needed files.
#
# You most probably would like to modify this file to suit your needs.
# Please see comments in flats.py for details of the parameters.

from flats import FlatScript,Flat

# You would at least like to specify filter order, if not binning and other things
#f = FlatScript(eveningFlats=[Flat('B'),Flat('V'),Flat('I'),Flat('R'),Flat('clear')],maxDarks=15,expTimes=range(2,30))
f = FlatScript(eveningFlats=[Flat('z'),Flat('B'),Flat('V'),Flat('I'),Flat('R')],maxDarks=15,expTimes=range(2,30))

# Change deafult number of images
f.flatLevels(defaultNumberFlats=15,biasLevel=250,optimalRange=0.3,allowedOptimalDeviation=0.1,sleepTime=60,shiftRa=0.0,shiftDec=30/3600.0)

#f.darkFilter('DF')
f.darkFilter('z')

# We want define this, as this implies not making mount shift-moves until brigtness range is reached... But we want to readout whole CCD to save it from light...
f.setSubwindow(waitingSubWindow='-1 -1 -1 -1')

# Run it..
# Configure domeDevice,tmpDirectory and mountDevice if your device names differ
f.run()

#f.finish()


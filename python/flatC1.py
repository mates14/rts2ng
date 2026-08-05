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
f = FlatScript(eveningFlats=[Flat('sii'),Flat('halpha'),Flat('i'),Flat('r'),Flat('clear')],maxDarks=15,expTimes=range(2,30))
# blbne filtrovy kolo, takze omezuju docasne pohyb, at se neztraci pozice, nez to opravim (2023-09-06):
#f = FlatScript(eveningFlats=[Flat('i'),Flat('r'),Flat('clear')],maxDarks=15,expTimes=range(2,30))

# Change deafult number of images
f.flatLevels(defaultNumberFlats=10,biasLevel=530,optimalFlat=5000,optimalRange=0.3,allowedOptimalDeviation=0.1)

f.darkFilter('DF')

# We want define this, as this implies not making mount shift-moves until brigtness range is reached... But we want to readout whole CCD to save it from light...
f.setSubwindow(waitingSubWindow='-1 -1 -1 -1')

# Run it..
# Configure domeDevice,tmpDirectory and mountDevice if your device names differ
# domeDevice je tu None schvalne, aby se nezavrela strecha pred porizenim rannich darku (to udela C3, ktere to trva nejdyl)
f.run(domeDevice=None)

# Send some optional emails - configure before uncomenting this line
# f.sendEmail('robot@example.com','Example skyflats')

#f.finish()


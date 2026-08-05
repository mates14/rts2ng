#!/usr/bin/python

import rts2.scriptcomm
import time, os
import math


#shift = 3
#alpha = 290
#alpha_ = 318
#delta = 46
#delta_ = 25

class Cygnus_scan (rts2.scriptcomm.Rts2Comm):


	def __init__(self, exptime = 30):
		rts2.scriptcomm.Rts2Comm.__init__(self)
		self.exptime = 60
		self.numimages = 2
		#self.telescope = 'T0'
		self.telname = self.getDeviceByType(rts2.scriptcomm.DEVICE_TELESCOPE)
		self.index = 0
		shift = 3.15
		RA_center = 305.0
		DEC_center = 40.0

		self.polohy = []
		for RA_index in range(-4, 5):
			#for DEC_index in range(-4, 5):
			#for DEC_index in range(5, 6):
			for DEC_index in range(-4, 6):
				dec = DEC_center - DEC_index*shift
				ra = RA_center + RA_index*shift/math.cos(math.radians(dec))
				temp = [ra, dec]
				self.polohy.append (temp)


	def beforeReadout(self):
		nextIndex = self.index + 1
		if (self.num == self.numimages and nextIndex < len (self.polohy) ):
			self.radec(self.polohy [nextIndex][0], self.polohy [nextIndex][1])
	
	def run(self):
		self.radec(self.polohy [0][0], self.polohy [0][1])	# najedeme na zacatek (dalsi posuny se pak provadeji behem vycitani)
		ret = self.waitIdle(self.telname,300)
		time.sleep(20)	# tohle by tu bylo zbytecny, kdyby ovsem waitIdle opravdu spolehlive fungoval, coz ale zjevne nedela :-(

		while self.index < len (self.polohy):
			self.setValue('filter', 'halpha')
			#self.setValue('exposure', self.exptime)
			self.setValue('SHUTTER', 'LIGHT')
			ret = self.waitIdle(self.telname,300)
			self.setValue('exposure', 10)
			self.num = 0;
			self.exposure(self.beforeReadout, '/home/prudil/cygnus_190831/images/%f')
			self.setValue('exposure', self.exptime)
			#time.sleep(30)	# tohle by tu bylo zbytecny, kdyby ovsem waitIdle opravdu spolehlive fungoval, coz ale zjevne nedela :-(
			# nakonec jsem dal delsi sleep (30, puvodne jsem mel 5) 
			for self.num in range (1, self.numimages+1):
				self.exposure(self.beforeReadout, '/home/prudil/cygnus_190831/images/%f')
			self.index = self.index + 1


labut = Cygnus_scan ()

labut.run() #utikej labut :D


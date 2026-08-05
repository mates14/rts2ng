#!/usr/bin/python
""" Guiding script for RTS2-Executor to be used with a CMOS camera of D50

(C) 2010 Martin Jelinek & Petr Kubanek

Version modified to work with CMOS, which is FAST (30fps) and does not allow
for windowing or binning, requires external shutter operation and dark frame
usage has no cooling = shutter/dark at the beginning of each run

Update: permit also guiding in declination (recently implemented at D50)
"""

import os
from datetime import datetime
import subprocess
import time
import numpy as np
import pyfits
import rts2comm

class GuideScript(rts2comm.Rts2Comm):
    """ Guide the telescope with a CMOS camera on a WF lens."""
    def __init__(self):
        self.detect4g = "/etc/rts2/detect4g"

        # how much of the detected offset to apply (to dump resonance)
        # self.aggresivity = [0.7, 0.7]  # included in factor
        self.exp_time = 0.5             # initial exposure time
        self.exp_snr = 100         # minimum signal to noise ratio of the guide star

        # multiply the position error in pixels by this to get the pulse time
        # n milisekund pojede montaz o 2x rychleji, za 1000ms by tedy dala
        # 15" navic, 1.8" / pixel / (15"/s) je 120ms/pixel, plus nejake tlumeni, deleno cos(delta)
        # zbyle tri smery jsou podobne az stejne
        self.factor = np.array([-300*0.7, 300*0.7])
        self.dark = None
        self.refstars = None
#        self.lasty = 0  # will be removed
#        self.lastx = 0  # will be removed

#       It would be nice to call the parent init(), but I do not happen to find a clean way
#       ... rts2comm.init() is empty anyway
#        super(GuideScript, self).__init__()

    def run_prg_get_array(self, imgfile):
        """ pass the image to sextractor and geta star list """
        self.log('I', 'guide: run_prg_get_array')
        get_stars = subprocess.Popen([self.detect4g, imgfile], stdout=subprocess.PIPE)
        #get_stars.wait()
        input2 = get_stars.stdout.readlines()
        nrecords = len(input2)
        nfields = 4
        ret = np.empty((nrecords, nfields))
        for i in range(nrecords):
            temp = input2[i].split()
            for j in range(nfields):
                ret[i][j] = temp[j]
        return ret

    def dark_sub(self, image):
        """ dark subtract """
        self.log('I', 'guide: dark_sub')
        imfile = pyfits.open(image)
        stars = imfile[0].data - self.dark + 100
        c_time = imfile[0].header['CTIME']
        usec = imfile[0].header['USEC']

        datum = datetime.utcfromtimestamp(c_time)
        datestr = datum.strftime("%Y%m%d%H%M%S")

        obj_m = "/tmp/%s-%03d-d.fits" % (datestr, usec/1000)
        imfile_d = pyfits.open(obj_m, mode='append')
        imfile_d.append(pyfits.PrimaryHDU(data=stars, header=imfile[0].header))
#        self.log('I', 'guide: writing %s' % (obj_m))
        imfile_d.close()
        imfile.close()
        return obj_m

    def getshift(self, stars):
        """Compute the shift of the image center against reference star list"""

        self.log('I', 'guide: get_shift')
        shiftx = np.empty((0, 1))
        shifty = np.empty((0, 1))
        for i in self.refstars:
            if i[2] > self.exp_snr:
                for j in stars:
                    if np.abs(i[0] - j[0]) < 5 and np.abs(i[1] - j[1]) < 5:
                        shiftx = np.vstack([shiftx, np.array([i[0]-j[0]], dtype=np.float32)])
                        shifty = np.vstack([shifty, np.array([i[1]-j[1]], dtype=np.float32)])

        return [np.median(shiftx), np.median(shifty)], len(shiftx)

    def getstars(self):
        """ Acquire image, subtract a dark frame, detect stars and return their list """
        self.log('I', 'guide: getstars')
        image = self.exposure()
        image_d = self.dark_sub(image)
        self.delete(image)

        # run sextractor to get the star center and then trash the image
        stars = self.run_prg_get_array(image_d)
        os.remove(image_d)

        return stars

    def run(self):
        """ Guide the NF using WF """

        # in case we are exposing, do not wait
        # self.sendCommand('stopexpo');

        self.log('I', 'guide: **** GUIDING **** ')

        # don't start guiding if the telescope isn't actually tracking -
        # avoids taking a dark frame and reference image against a mount
        # that's still settling, parked, or was never told to track.
        # Uses raw getState()/mask arithmetic (0x020 = TEL_TRACKING, see
        # status.h) rather than rts2.scriptcomm.Rts2Comm's isTracking()
        # helper: this script imports the separate rts2comm module, not
        # rts2.scriptcomm, and whether the two are interchangeable wasn't
        # something to assume - getState() itself predates that helper
        # and is safe either way.
        if (self.getState('T0') & 0x020) != 0x020:
            self.log('W', 'guide: telescope is not tracking, not starting guiding')
            return

        self.setValue('WFshutter', ' 0', 'SHUTTER1')
        self.setValue('exposure', self.exp_time)
        self.setValue('speed_guide_ra', 10)
        self.setValue('speed_guide_dec', 10)

        # hm... stupid mount is not finished when it says so?
        # and also the shutter moves for certain time
        time.sleep(1)

        # take a dark frame
        image = self.exposure()
        image = pyfits.open(image)
        self.dark = image[0].data
        image.close()
        self.delete(image)

        # now open the shutter
        self.setValue('WFshutter', ' 1', 'SHUTTER1')
        time.sleep(2)  # wait for the shutter to stabilize

        # take a reference image
        self.refstars = self.getstars()

        # here needs to be solved the possible problem of no reference stars

        this_move_end = self.getValueFloat('move_end', 'T0')
        #last_move_start = self.getValueFloat('move_started', 'T0')
        last_move_start = 0.0
        self.log('I', 'guide: times %.3f %.3f'%(last_move_start, this_move_end))

        # the cycle ends when: 1. the mount does a move
        while last_move_start < this_move_end + 1:

            # it also ends when 2. the main camera does not do anything for some time (10s here)
            now = time.time()
            exposure_end = self.getValueFloat('exposure_end', 'C0')
            if (now - exposure_end) > 10:
                self.log('I', 'guide: C0 inactive for %fs - ending'%(now-exposure_end))
                break

            stars = self.getstars()

            # sextractor may fail! we need to recover without losing the daisy :)
            # 4 ... i do not know, what it would actually output when it fails, but a small number
            if len(stars) < 4:
                self.log('I', 'guide: no stars, will try again (len(stars)=%d)' % len(stars))
                # but I have to watch if I am to end the loop!
                last_move_start = self.getValueFloat('move_started', 'T0')
                continue

            change, numcross = self.getshift(stars)

            # fight backlash and induced resonance
            # self.lasty = change[1]/2 + 1*self.lasty/2
            # change[1]=self.lasty
            # tmp = change[0]
            # change[0] = change[0]/2 + self.lastx/2
            # self.lastx = tmp

            county = np.maximum(np.minimum(self.factor*np.array(change), 1000), -1000)

#            scale = 3.85 # arcsec/pixel C1
            # new C1=1.81424"/pix

            # will have to include declination to correct movement towards the pole
            #ch_ra = scale * float(change[0]) * self.aggresivity[0] * 2
            #ch_dec = scale * float(change[1]) * self.dec_aggresivity[1]

            # n milisekund pojede montaz o 10% rychleji, za 1000ms by tedy dala
            # 1.5" navic

            sekundy = np.minimum(np.max(np.abs(county))/1000, 1.0)

            self.log('I', 'guide: x %.2f y %.2f ra %d dec %d numx %.2f time %.4f'\
                % (change[0], change[1], county[0], county[1], numcross, sekundy))

            self.setValue('pulse_guide_ra', ' %d' % (county[0]), 'T0')
            self.setValue('pulse_guide_dec', ' %d' % (county[1]), 'T0')

            time.sleep(sekundy)

            this_move_end = self.getValueFloat('move_end', 'T0')
            last_move_start = self.getValueFloat('move_started', 'T0')

        self.log('I', 'guide: times2 %.3f %.3f'%(last_move_start, this_move_end))
        self.setValue('WFshutter', ' 0', 'SHUTTER1')

RUN = GuideScript()
RUN.run()

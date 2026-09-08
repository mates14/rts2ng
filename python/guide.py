#!/usr/bin/python
""" Guiding script for RTS2-Executor to be used with a CMOS camera of D50

(C) 2010 Martin Jelinek & Petr Kubanek

Version modified to work with CMOS, which is FAST (30fps) and does not allow
for windowing or binning, requires external shutter operation and dark frame
usage has no cooling = shutter/dark at the beginning of each run

Update: permit also guiding in declination (recently implemented at D50)
"""

import sys
import os
import re
import shutil
from datetime import datetime
import subprocess
import time
import threading
import numpy as np
import astropy.io.fits as pyfits
from PIL import Image
import rts2comm

# --- site configuration ----------------------------------------------------
# Everything the guiding telemetry and the kept frames need to know about
# this particular site. All of it is deliberately in one place: the web
# side (d50-monitor.html) reads the values off the bus and the frames out
# of rts2-httpd's images directory, so these have to agree with how that
# daemon is started.

# The camera this script runs on. Its own values (exposure, SHUTTER) are
# set without a device name - the executor routes those to the camera
# running the script - but value_create is handled by ConnExe one level
# up, in the executor itself, so the guiding telemetry has to name the
# camera explicitly to end up on it rather than on EXEC.
GUIDE_CAM = 'C1'

# rts2-httpd's --images-dir, and the subdirectory of it that keeps the
# guiding frames. The path published in guide_last_image is relative to
# the first, which is exactly what /preview/<path> expects.
IMAGES_DIR = os.environ.get('RTS2_IMAGES_DIR', '/data')
GUIDE_SUBDIR = 'guide'

# The web copy of the latest cutout, and the cached stretch used to make
# it.  This goes to the httpd's *static* directory rather than the images
# directory, because rts2-httpd exposes the latter only through /preview/,
# whose scaling collapses anything that is not USHORT or FLOAT to a flat
# grey (getChannelHistogram() has no case for signed short).  The page
# therefore loads a plain JPEG from Apache, exactly as it already does for
# the C0 preview next to it.
WEB_JPEG = '/var/www/info/guide_last.jpg'

# pyrt-f2cj fits log10(counts) to the byte range and prints the three
# coefficients.  It costs ~1.7 s, nearly all of it interpreter start-up,
# against a guiding cadence of ~2 s - so it runs once per observation, off
# the guiding thread, and every frame in between is rendered with the
# coefficients already in hand.  Keeping the last set in /dev/shm means a
# fresh run has something to publish from its very first frame instead of
# showing nothing until the fit lands.
LEVELS_CACHE = '/dev/shm/%s_guide_levels' % GUIDE_CAM
LEVELS_RE = re.compile(r'Fitted parameters: A=(\S+?), B=(\S+?), C=(\S+?) ')

# Kept frames are sorted into per-night directories named the way RTS2
# names nights (%N: the UT date at the start of the night, boundary near
# local noon). The first frame of a new night deletes the older ones -
# they are debugging material, not data. 1 = keep tonight only.
KEEP_NIGHTS = 1

# Longitude, in degrees east, only for that night boundary - the same
# utOffset Configuration::getNight() uses (see base/kernel: night =
# gmtime(t + utOffset*3600 - 43200)). D50/Ondrejov.
LONGITUDE = 14.7823

# A guide frame is not worth keeping whole: at ~1 Hz all night that is
# gigabytes of mostly-empty sky, and the only part anyone looks at is
# the guide star. CUTOUT is the size of the saved box around it, in
# pixels; FWHM_BOX the (smaller) region the FWHM is measured over.
# 48x48 int16 lands at ~9 kB per frame, ~250 MB for a full night.
CUTOUT = 48
FWHM_BOX = 15

# arcsec/pixel, published as guide_scale so the web side can label its
# axes in arcsec without hardcoding a copy of it. This is the value the
# pulse-guide factor below is derived from (947 = 1.42/1.5*1000); the
# "new C1=1.81424"/pix" note further down disagrees with it and one of
# the two is stale - worth settling, since only this one is currently
# doing any work.
PIXEL_SCALE = 1.42


def load_levels():
    """Last stretch pyrt-f2cj fitted, or None when there is not one yet."""
    try:
        with open(LEVELS_CACHE) as f:
            a, b, c = (float(v) for v in f.read().split())
        return a, b, c
    except Exception:
        return None


def save_levels(levels):
    """Best-effort: a stretch that cannot be cached is not worth a failure."""
    try:
        tmp = LEVELS_CACHE + '.tmp'
        with open(tmp, 'w') as f:
            f.write('%.10g %.10g %.10g\n' % levels)
        os.replace(tmp, LEVELS_CACHE)
    except Exception:
        pass


class GuideScript(rts2comm.Rts2Comm):
    """ Guide the telescope with a CMOS camera on a WF lens."""
    def __init__(self):
        super().__init__()

        # Stretch for the web cutout: whatever the last run left behind,
        # refined once this run's own field has been through pyrt-f2cj.
        self.levels = load_levels()
        self.levels_thread = None

        self.detect4g = "/etc/rts2/detect4g"

        # how much of the detected offset to apply (to dump resonance)
        # self.aggresivity = [0.7, 0.7]  # included in factor
        #self.exp_time = 0.5             # initial exposure time
        self.exp_time = 1.0             # initial exposure time
        self.exp_snr = 100         # minimum signal to noise ratio of the guide star

        # multiply the position error in pixels by this to get the pulse time
        # n milisekund pojede montaz o 2x rychleji, za 1000ms by tedy dala
        # 15" navic, 1.8" / pixel / (15"/s) je 120ms/pixel, plus nejake tlumeni, deleno cos(delta)
        # zbyle tri smery jsou podobne az stejne
        #self.factor = np.array([-300*0.7, 300*0.7])
        # JS: s aktualni kamerou mame 1.42"/pixel, pouzivame 10% sidericke rychlosti =>
        #   1000ms odpovida 1.5"
        # takze prevodni koeficient pro prevod na pulse time je 1.42/1.5*1000 ~ 947
        # a k tomu HA musime delit cos(dec)
        telescope_coordinates = self.getValue('TEL', 'T0').split()
        self.log('I', 'guide: TEL DEC: %.4f' % float(telescope_coordinates[1]))
        self.factor = \
            np.array([-947/np.cos(np.radians(float(telescope_coordinates[1])))*0.8, 947*0.8])
        self.dark = None
        self.refstars = None
        # last dark-subtracted frame, as (data, header), handed from
        # dark_sub() to keep_frame()
        self.frame = None
        # detect4g's y convention, resolved once per run - see cutout()
        self.yflip = None
        # frame keeping is best-effort: a full disk must not stop the
        # telescope from guiding, and it must not fill the log either
        self.warned = False
#        self.lasty = 0  # will be removed
#        self.lastx = 0  # will be removed


    def run_prg_get_array(self, imgfile):
        """ pass the image to sextractor and geta star list """
#        self.log('I', 'guide: run_prg_get_array')
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
#        self.log('I', 'guide: dark_sub')
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

        # keep_frame() gets the same data this pass detects stars in, so
        # the saved cutout is the frame the published numbers came from,
        # not a re-read of anything
        self.frame = (stars, imfile[0].header.copy())

        imfile.close()
        return obj_m

    def getshift(self, stars):
        """Compute the shift of the image center against reference star list"""

#        self.log('I', 'guide: get_shift')
        shiftx = np.empty((0, 1))
        shifty = np.empty((0, 1))
        for i in self.refstars:
            if i[2] > self.exp_snr:
                for j in stars:
                    if np.abs(i[0] - j[0]) < 5 and np.abs(i[1] - j[1]) < 5:
                        shiftx = np.vstack([shiftx, np.array([i[0]-j[0]], dtype=np.float32)])
                        shifty = np.vstack([shifty, np.array([i[1]-j[1]], dtype=np.float32)])

        #return [np.median(shiftx), np.median(shifty)], len(shiftx)
        # JS: nekam jsem musel naprat to, ze kamera je ted otocena o 90deg,\
        #    nacpal jsem to sem... 2.6.2021
        return [-np.median(shifty), np.median(shiftx)], len(shiftx)

    def getstars(self):
        """ Acquire image, subtract a dark frame, detect stars and return their list """
#        self.log('I', 'guide: getstars')
        image = self.exposure()
        image_d = self.dark_sub(image)
        # Guide frames used to go toArchive(), which only renames the
        # file - the images row is inserted well before the script ever
        # sees the frame (DevClientCameraImage::processCameraImage saves
        # the image, which for an object frame is an INSERT), so the
        # archive filled up with thousands of junk rows a night. delete
        # is the one image action that also removes that row
        # (ImageSkyDb::deleteImage -> deleteFromDB). What is worth
        # keeping is kept by keep_frame(), outside the database.
        self.delete(image)

        # run sextractor to get the star center and then trash the image
        stars = self.run_prg_get_array(image_d)
        os.remove(image_d)

        return stars

    # --- guiding telemetry and kept frames ---------------------------------

    def create_values(self):
        """ Create the guiding telemetry values on the guide camera.

        Idempotent on the RTS2 side (value_create on an existing value of
        the same type just sets it), so re-running this on every script
        start is fine - and it deliberately resets the errors to NaN, so
        a value left over from the previous observation is never mistaken
        for a live reading. NaN is what both consumers want for "not
        guiding": rts2-recordd skips it instead of recording a flat line,
        and the JSON API sends it as null.
        """
        nan = float('nan')
        for name, desc in [
                ('guide_dx', 'guiding error, X axis [pixels]'),
                ('guide_dy', 'guiding error, Y axis [pixels]'),
                ('guide_pulse_ra', 'applied guiding pulse, RA [ms]'),
                ('guide_pulse_dec', 'applied guiding pulse, DEC [ms]'),
                ('guide_stars', 'stars matched against the reference frame'),
                ('guide_fwhm', 'FWHM of the guiding star [pixels]')]:
            self.valueCreate(name, nan, GUIDE_CAM, desc, 'double', writable=True)
        self.valueCreate('guide_scale', PIXEL_SCALE, GUIDE_CAM,
                         'guide camera plate scale [arcsec/pixel]', 'double')
        self.valueCreate('guide_last_image', '', GUIDE_CAM,
                         'last kept guiding frame, relative to the images directory',
                         'string', writable=True)

    def publish(self, dx, dy, pra, pdec, nstars, fwhm, path=None):
        """ Push one guiding cycle onto the bus, as values of the camera """
        self.setValue('guide_dx', dx, GUIDE_CAM)
        self.setValue('guide_dy', dy, GUIDE_CAM)
        self.setValue('guide_pulse_ra', pra, GUIDE_CAM)
        self.setValue('guide_pulse_dec', pdec, GUIDE_CAM)
        self.setValue('guide_stars', nstars, GUIDE_CAM)
        self.setValue('guide_fwhm', fwhm, GUIDE_CAM)
        if path is not None:
            self.setValue('guide_last_image', path, GUIDE_CAM)

    def publish_idle(self):
        """ Not guiding: say so, rather than leaving the last reading up """
        nan = float('nan')
        self.publish(nan, nan, nan, nan, nan, nan)

    def warn(self, message):
        """ Log a frame-keeping failure once, not once a second """
        if not self.warned:
            self.log('W', message)
            self.warned = True

    def night_dir(self):
        """ Tonight's frame directory, creating (and pruning) as needed.

        The night boundary is RTS2's own - Configuration::getNight() is
        gmtime(t + utOffset*3600 - 43200), i.e. the UT date the night
        started on, rolling over near local noon - so these directory
        names line up with %N everywhere else in the system.
        """
        night = time.strftime('%Y%m%d',
                              time.gmtime(time.time() + LONGITUDE / 15.0 * 3600 - 43200))
        path = os.path.join(IMAGES_DIR, GUIDE_SUBDIR, night)
        if not os.path.isdir(path):
            os.makedirs(path)
            self.purge_nights()
        return path, night

    def purge_nights(self):
        """ Drop guiding frames from previous nights.

        Runs exactly once per night, when that night's directory is first
        created. Only ever touches 8-digit directory names directly under
        the guiding directory - nothing else there can be deleted by a
        mistake here, whatever IMAGES_DIR ends up pointing at.
        """
        base = os.path.join(IMAGES_DIR, GUIDE_SUBDIR)
        nights = sorted([d for d in os.listdir(base)
                         if re.match(r'^\d{8}$', d) and os.path.isdir(os.path.join(base, d))])
        for old in nights[:-KEEP_NIGHTS]:
            shutil.rmtree(os.path.join(base, old))
            self.log('I', 'guide: removed guiding frames of night %s' % old)

    def peak(self, data, row, col):
        """ Brightest pixel near (row, col), or NaN if that is off-frame """
        h, w = data.shape
        r0, r1 = max(0, row - 3), min(h, row + 4)
        c0, c1 = max(0, col - 3), min(w, col + 4)
        if r0 >= r1 or c0 >= c1:
            return float('nan')
        return float(np.max(data[r0:r1, c0:c1]))

    def cutout(self, data, x, y):
        """ Box of CUTOUT pixels around a detected star.

        detect4g is a sextractor wrapper living in /etc/rts2, and nothing
        here documents whether its y counts the way numpy indexes rows or
        the way FITS displays them - the guiding itself never cared,
        since it only ever compares one frame's coordinates against
        another's. A cutout does care, so both readings are tried on the
        first star of a run and the one that actually lands on the star
        is kept for the rest of it.
        """
        h, w = data.shape
        if h < CUTOUT or w < CUTOUT:
            return None, 0, 0
        col = int(round(float(x))) - 1
        rows = [int(round(float(y))) - 1, h - int(round(float(y)))]
        if self.yflip is None:
            peaks = [self.peak(data, r, col) for r in rows]
            self.yflip = 1 if peaks[1] > peaks[0] else 0
            self.log('I', 'guide: star y convention %s (peaks %.0f / %.0f)'
                     % ('flipped' if self.yflip else 'direct', peaks[0], peaks[1]))
        row = rows[self.yflip]
        r0 = max(0, min(h - CUTOUT, row - CUTOUT // 2))
        c0 = max(0, min(w - CUTOUT, col - CUTOUT // 2))
        return data[r0:r0 + CUTOUT, c0:c0 + CUTOUT], c0, r0

    def measure_fwhm(self, box):
        """ FWHM of the star in a cutout, in pixels.

        Area above half maximum rather than a fit: no starting guess to
        get wrong, no iteration to fail to converge, and one bad frame
        costs a NaN instead of an exception. Measured on FWHM_BOX around
        the actual peak, so a second star elsewhere in the cutout cannot
        inflate it. Diagnostic of seeing (and of focus), not photometry.
        """
        peak = np.unravel_index(np.argmax(box), box.shape)
        half = FWHM_BOX // 2
        r0, r1 = max(0, peak[0] - half), min(box.shape[0], peak[0] + half + 1)
        c0, c1 = max(0, peak[1] - half), min(box.shape[1], peak[1] + half + 1)
        sub = box[r0:r1, c0:c1]
        background = float(np.median(box))
        amplitude = float(np.max(sub)) - background
        if amplitude <= 0:
            return float('nan')
        above = np.count_nonzero(sub > background + amplitude / 2.0)
        return 2.0 * np.sqrt(above / np.pi)

    def write_frame(self, box, header, origin, star, dx, dy, pra, pdec, nstars, fwhm):
        """ Save the guide star cutout, named and dated the way RTS2 does """
        directory, night = self.night_dir()
        stamp = datetime.utcfromtimestamp(header['CTIME'])
        name = '%s_%s-%03d.fits' % (GUIDE_CAM, stamp.strftime('%Y%m%d-%H%M%S'),
                                    header['USEC'] / 1000)

        hdu = pyfits.PrimaryHDU(data=np.clip(box, -32768, 32767).astype(np.int16),
                                header=header.copy())

        # FITS headers have no NaN - a card that would carry one is left
        # out instead, which reads the same way: nothing was measured.
        # (The reference frame has no shift to record, and a pass that
        # detected nothing has no FWHM.)
        def card(key, value, comment):
            if not np.isnan(value):
                hdu.header[key] = (value, comment)

        card('GUIDE_DX', dx, 'guiding error, X axis [pixels]')
        card('GUIDE_DY', dy, 'guiding error, Y axis [pixels]')
        card('PULSE_RA', pra, 'applied guiding pulse, RA [ms]')
        card('PULSEDEC', pdec, 'applied guiding pulse, DEC [ms]')
        card('FWHM', fwhm, 'FWHM of the guiding star [pixels]')
        hdu.header['GUIDE_N'] = (nstars, 'stars matched against the reference')
        hdu.header['SCALE'] = (PIXEL_SCALE, 'plate scale [arcsec/pixel]')
        hdu.header['STAR_X'] = (float(star[0]), 'guide star in the full frame')
        hdu.header['STAR_Y'] = (float(star[1]), 'guide star in the full frame')
        hdu.header['CUTOUTX'] = (origin[0], 'cutout origin in the full frame')
        hdu.header['CUTOUTY'] = (origin[1], 'cutout origin in the full frame')
        hdu.writeto(os.path.join(directory, name))

        return '/'.join([GUIDE_SUBDIR, night, name])

    def keep_frame(self, stars, change, county, nstars):
        """ Publish one guiding cycle and keep its guide star.

        Called on every pass, including the ones that measured nothing -
        a failed detection is exactly the moment the web display should
        stop showing the last good number as if it were current.
        """
        nan = float('nan')
        dx, dy = (float(change[0]), float(change[1])) if change is not None else (nan, nan)
        pra, pdec = (float(county[0]), float(county[1])) if county is not None else (nan, nan)

        fwhm = nan
        path = None
        try:
            if stars is not None and len(stars) > 0 and self.frame is not None:
                data, header = self.frame
                star = stars[np.argmax(stars[:, 2])]
                box, c0, r0 = self.cutout(data, star[0], star[1])
                if box is not None:
                    fwhm = self.measure_fwhm(box)
                    path = self.write_frame(box, header, (c0, r0), star,
                                            dx, dy, pra, pdec, nstars, fwhm)
                    self.publish_jpeg(box, path)
        except Exception as ex:
            # Guiding continues without its frames; the telescope does
            # not stop tracking over a full disk or a missing directory.
            self.warn('guide: cannot keep guiding frame: %s' % ex)

        self.publish(dx, dy, pra, pdec, nstars, fwhm, path)
        self.frame = None

    def fit_levels(self, path):
        """Re-derive the stretch for this field, on a thread of its own.

        Runs once per observation.  Guiding must not wait on it, and the
        display must not either - until it lands, the cached coefficients
        from the previous run are close enough to be worth showing.
        """
        try:
            # No -i here: in pyrt-f2cj that is --inverted, not --input.  The
            # input file is positional.
            ret = subprocess.run(['pyrt-f2cj', '-o', '/dev/shm/guide_fit.jpg',
                                  path],
                                 capture_output=True, text=True, timeout=120)
            found = LEVELS_RE.search(ret.stdout)
            if found is None:
                self.warn('guide: pyrt-f2cj gave no levels for %s' % path)
                return
            self.levels = tuple(float(v.rstrip(',')) for v in found.groups())
            save_levels(self.levels)
        except Exception as ex:
            self.warn('guide: cannot fit levels: %s' % ex)

    def publish_jpeg(self, box, path):
        """Write the cutout where the web page can see it.

        Same transformation pyrt-f2cj applies, with the coefficients it
        fitted: 10 ** (A + B * log10(counts - C)), clipped to a byte.  The
        file is replaced atomically so the page never loads a half-written
        frame, and every failure here is silent to the telescope - a
        missing thumbnail is not a reason to stop guiding.
        """
        if self.levels_thread is None:
            self.levels_thread = threading.Thread(
                target=self.fit_levels, args=(os.path.join(IMAGES_DIR, path),),
                daemon=True)
            self.levels_thread.start()

        levels = self.levels
        if levels is None:
            return

        try:
            a, b, c = levels
            counts = np.maximum(np.asarray(box, dtype=float), c + 1e-10)
            scaled = 10.0 ** (a + b * np.log10(counts - c))
            tmp = WEB_JPEG + '.tmp'
            Image.fromarray(np.clip(scaled, 0, 255).astype(np.uint8)).save(
                tmp, 'JPEG', quality=90)
            os.replace(tmp, WEB_JPEG)
        except Exception as ex:
            self.warn('guide: cannot publish guiding frame: %s' % ex)

    def run(self):
        """ Guide the NF using WF """

        # in case we are exposing, do not wait
        # self.sendCommand('stopexpo');

        self.log('I', 'guide: **** GUIDING **** ')
        self.create_values()
        # ok, setting SHUTTER makes no sense for this shutter-less camera, but it should do no harm - and it's needed to enable proper "toDark()" image movement later
        self.setValue('SHUTTER', 'DARK')
        self.setValue('WFshutter', ' 0', 'SHUTTER1')
        self.setValue('exposure', self.exp_time)
        # setValue is (name, value, device) - these two had the name and
        # the device the other way round, so they addressed a device
        # called "speed_guide_ra", which does not exist, and the 10%
        # sidereal rate the pulse factor above is calibrated for was
        # never actually being set.
        self.setValue('speed_guide_ra', 10, 'T0')
        self.setValue('speed_guide_dec', 10, 'T0')

        # hm... stupid mount is not finished when it says so?
        # and also the shutter moves for certain time
        time.sleep(1)

        # take a dark frame
        image = self.exposure()
        imageh = pyfits.open(image)
        self.dark = imageh[0].data
        imageh.close()
        #self.delete(image)
        #self.toArchive(image)
        self.toDark(image)

        # now open the shutter
        self.setValue('SHUTTER', 'LIGHT')
        self.setValue('WFshutter', ' 1', 'SHUTTER1')
        time.sleep(4)  # wait for the shutter to stabilize
        # JS: byly tu 2s, zvedam na 4

        # take a reference image
        self.refstars = self.getstars()
        # no shift to report off the frame everything else is measured
        # against, but its star - and its FWHM - are as real as any other
        self.keep_frame(self.refstars, None, None, len(self.refstars))

        # here needs to be solved the possible problem of no reference stars

        this_move_end = self.getValueFloat('move_end', 'T0')
        #last_move_start = self.getValueFloat('move_started', 'T0')
        last_move_start = 0.0
 #       self.log('I', 'guide: times %.3f %.3f'%(last_move_start, this_move_end))

        # the cycle ends when: 1. the mount does a move
        while last_move_start < this_move_end + 1:

            # it also ends when 2. the main camera does not do anything for some time (10s here)
            now = time.time()
            exposure_end = self.getValueFloat('exposure_end', 'C0')
            if (now - exposure_end) > 10:
                self.log('I', 'guide: C0 inactive for %fs - ending'%(now-exposure_end))
                self.publish_idle()
                sys.exit(0)

            stars = self.getstars()

            # sextractor may fail! we need to recover without losing the daisy :)
            # 4 ... i do not know, what it would actually output when it fails, but a small number
#            self.log('I', 'guide: len(stars) = %d' % len(stars))
            if len(stars) < 4:
                self.log('I', 'guide: no stars, will try again (len(stars)=%d)' % len(stars))
                self.keep_frame(stars, None, None, len(stars))
                # but I have to watch if I am to end the loop!
                last_move_start = self.getValueFloat('move_started', 'T0')
                continue

            change, numcross = self.getshift(stars)

            if numcross<1:
                self.log('I', 'guide: will restart, numcross==0')
                break

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

            self.log('I', 'guide: val %.1f %.2f %.2f %d %d %.2f %.2f'\
                % (now, change[0], change[1], county[0], county[1], numcross, sekundy))

            # same numbers as the log line above, on the bus this time:
            # the web display reads them live, and rts2-recordd samples
            # them into records_double for the night's history
            self.keep_frame(stars, change, county, numcross)

            self.setValue('pulse_guide_ra', ' %d' % (county[0]), 'T0')
# makes problems, lets see if commenting this out helps
            self.setValue('pulse_guide_dec', ' %d' % (county[1]), 'T0')

            time.sleep(sekundy)

            this_move_end = self.getValueFloat('move_end', 'T0')
            last_move_start = self.getValueFloat('move_started', 'T0')

#        self.log('I', 'guide: times2 %.3f %.3f'%(last_move_start, this_move_end))
        self.setValue('WFshutter', ' 0', 'SHUTTER1')
        self.publish_idle()

while 1:
    RUN = GuideScript()
    RUN.run()

#!/usr/bin/python3
"""Perform synchronized observations with two cameras so that the even and odd
cameras exchange the time when they expose and readout"""

import time
from rts2 import scriptcomm

class ojitos(scriptcomm.Rts2Comm3):
    """ Guide the telescope with a CMOS camera on a WF lens."""
    def __init__(self, exptime=12):
        scriptcomm.Rts2Comm3.__init__(self)
        self.exptime = exptime
        self.tick = 2*self.exptime
        self.parity = False

    def waitIdle2(self):
        """ Alternative to parent disfunct waitIdle() for the telescope """
        # dalekohled jede, pokud je posledni start presunu novejsi datum nez posledni konec presunu
       
        this_move_end = 0
        last_move_start = 1

        camera = self.getRunDevice()

        start=time.time()
        #while this_move_end < last_move_start: 
        while True: 
            now = time.time()
            target_distance = self.getValueFloat('target_distance','T0')
            this_move_end = self.getValueFloat('move_end', 'T0')  # may be future!
            last_move_start = self.getValueFloat('move_started', 'T0')
            if target_distance < 1.0/60: # at one arcmin
                self.log('I', 'ojitos: %s td%.3f'%(camera,target_distance))
                time.sleep(1.5)
                break
            if  now > start+120: 
                self.log('I', 'ojitos: %s timeout'%(camera))
                break
            time.sleep(0.25)


    def run(self):
        """ Perform the timed exposures every self.tick """

        # get aware of the environment
        camera = self.getRunDevice()
        serial = self.getValue('CCD_SER')
        self.log('I', '*** OJITOS at %s sn %s *** '%(camera,serial))

        # decide the role
        if camera == "C2":
            self.parity = True
            mee = "ojitos: %s/o"%(camera)
        else:
            mee = "ojitos: %s/x"%(camera)


        # in case the camera is exposing, do not wait 
        # (but this is a delicate op, fix later)
#        self.sendCommand('stopexpo')
        
        self.setValue('SHUTTER','LIGHT')
        self.setValue('exposure', self.exptime)
        self.setValue('filter', 'clear')
        #self.setValue('filter', 'r')

        # Wait for the telescope
        #ret = self.waitIdle("T0", 300)
        ret = self.waitIdle2()
            
        zero = time.time() 

        # hm... the stupid mount is not finished when it says so?
        # and also the shutter moves for certain time
        # a mount must be fixed where it belongs, this can be enabled
        # when/if we measure the delay...
        # time.sleep(1)


        # this_move_end = time.time();
        #if self.parity: pulse=1
        #else: pulse = 0

        time.sleep(1)

        this_move_end = self.getValueFloat('move_end', 'T0')
        last_move_start = self.getValueFloat('move_started', 'T0')

        self.log('I', "%s The move end: %.1fs - %1fs = %1fs"%(mee, zero, this_move_end, zero - this_move_end))
        self.log('I', "%s The move start: %.1fs - %1fs = %1fs"%(mee, zero, last_move_start, zero - last_move_start))

        last_move_start = 0.0

        base = this_move_end
        if self.parity:
            base -= self.exptime

        firstrun = True

        # the cycle ends when the mount does a move
        while last_move_start < this_move_end + 1:

            if self.parity and firstrun:
                firstrun = False
            else:
                self.log('I', "%s Exposing %.1fs"%(mee, self.exptime))
                image = self.exposure()
#                time.sleep(2)
#                self.toArchive(image)
                self.process(image)
#                self.delete(image)
#                pulse += 2

#            this_move_end = self.getValueFloat('move_end', 'T0')
            last_move_start = self.getValueFloat('move_started', 'T0')

            if not last_move_start < this_move_end + 1:
                break

#            if False:
            now = time.time() - base
            next_tick = (int(now/self.tick)+1)*self.tick
#            else:
#                now = time.time()
#                next_tick = self.exptime * pulse + base
#                #pulse += 2

            delay = next_tick - now
            self.log('I', "%s Values %.2f %2f %.2f %.2f %.2f %.2f %.2f"\
                %(mee, now+base, now, delay, next_tick, this_move_end, last_move_start, base))
            if delay < 0:
                delay = 0
            time.sleep(delay)

if __name__ == '__main__':

    while True:
        OJITOS = ojitos()
        OJITOS.run()

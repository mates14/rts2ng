#!/usr/bin/env python3
"""
Simulator of Jan Fuchs' Perek 2m telescoped (ASCOL over XML-RPC), for
testing rts2-teld-perek2m and rts2-dome-perek2m without the telescope.

Listens on 127.0.0.1 only. Models the GLST state machine coarsely: oil
start-up, telescope power, RA/Dec and HA/Dec slews, dome slit, flaps,
dome rotation and focus, each taking a few seconds.

    telescoped_sim.py [port]          (default 19999)
"""
import sys
import threading
import time
from xmlrpc.server import SimpleXMLRPCServer

SLEW_RATE = 3.0       # deg/s, RA/Dec and axes
COVER_TIME = 4.0      # s, slit and flaps
DOME_RATE = 6.0       # deg/s
OIL_STEP = 1.5        # s per oil start-up stage
FOCUS_RATE = 50.0     # units/s


def sexa(value, sec_digits):
    """value -> (sign, big, min, sec) rounded to sec_digits, carries done"""
    sign = "-" if value < 0 else "+"
    scale = 10 ** sec_digits
    t = round(abs(value) * 3600 * scale)
    big, rest = divmod(t, 3600 * scale)
    mm, ss = divmod(rest, 60 * scale)
    return sign, big, mm, ss / scale


def hms(deg):
    _, hh, mm, ss = sexa((deg % 360.0) / 15.0, 2)
    return "%02d%02d%05.2f" % (hh % 24, mm, ss)


def dms(deg):
    sign, dd, mm, ss = sexa(deg, 1)
    return "%s%02d%02d%04.1f" % (sign, dd, mm, ss)


def parse_sexa(s, scale):
    s = s.strip()
    sign = -1 if s.startswith("-") else 1
    s = s.lstrip("+-")
    ip = s.split(".")[0]
    big = int(ip[:-4])
    mm = int(ip[-4:-2])
    ss = float(s[len(ip) - 2:])
    return sign * (big + mm / 60.0 + ss / 3600.0) * scale


def approach(cur, tar, step):
    if abs(tar - cur) <= step:
        return tar, True
    return cur + (step if tar > cur else -step), False


class Telescope:
    def __init__(self):
        self.lock = threading.RLock()
        self.oil = 0            # OFF
        self.oil_t = 0
        self.tel = 1            # OFF
        self.ha_st = 0
        self.da_st = 0
        self.focus_st = 1
        self.dome = 1           # STOP
        self.slit = 4           # CLOSE
        self.tube = 4
        self.mirror = 4
        self.bits = 0x0F0       # aberration, precession, refraction, model
        self.cover_until = {}
        self.cover_target = {}
        self.ra, self.dec, self.side = 150.0, 30.0, 0
        self.tar = (150.0, 30.0, 0)
        self.obj = ""
        self.ha_ax, self.da_ax = 90.0, 45.0
        self.ha_tar, self.da_tar = 90.0, 45.0
        self.dome_az, self.dome_tar = 0.0, 0.0
        self.focus, self.focus_tar = 1000.0, 1000.0
        self.model = 1
        self.last = time.time()
        threading.Thread(target=self._run, daemon=True).start()

    # --- simulation -----------------------------------------------------
    def _run(self):
        while True:
            time.sleep(0.1)
            with self.lock:
                now = time.time()
                dt = now - self.last
                self.last = now
                self._step(now, dt)

    def _step(self, now, dt):
        # oil start-up: START1..START3, then ON; switching off: OFF_DELAY
        if self.oil in (1, 2, 3) and now >= self.oil_t:
            self.oil += 1
            self.oil_t = now + OIL_STEP
        if self.oil == 5 and now >= self.oil_t:
            self.oil = 0
        if self.tel == 2 and now >= self.oil_t:
            self.tel = 1
        # losing oil switches the telescope off
        if self.oil != 4 and self.tel not in (0, 1):
            self.tel = 1

        for name in ("slit", "tube", "mirror"):
            if name in self.cover_until and now >= self.cover_until[name]:
                setattr(self, name, self.cover_target.pop(name))
                del self.cover_until[name]

        if 12 <= self.tel <= 18:            # ST_* slew to RA/Dec
            step = SLEW_RATE * dt
            self.ra, r1 = approach(self.ra, self.tar[0], step)
            self.dec, r2 = approach(self.dec, self.tar[1], step)
            self.tel = 14
            self.ha_st = self.da_st = 9     # MO_FAST
            if r1 and r2:
                self.side = self.tar[2]
                self.tel = 4                # TRACK
                self.ha_st = self.da_st = 1
        elif 6 <= self.tel <= 11:           # SS_* slew to HA/DA
            step = SLEW_RATE * dt
            self.ha_ax, r1 = approach(self.ha_ax, self.ha_tar, step)
            self.da_ax, r2 = approach(self.da_ax, self.da_tar, step)
            self.tel = 7
            self.ha_st = self.da_st = 9
            if r1 and r2:
                self.tel = 3                # STOP
                self.ha_st = self.da_st = 1

        if self.tel == 4:
            # tracking: the axes follow the sky, roughly
            self.ha_ax = (self.ha_ax + dt * 15.0 / 3600.0) % 360.0
            self.da_ax = self.dec if self.side == 0 else 180.0 - self.dec

        if self.dome in (6, 7, 8):          # AUTO: follow the telescope
            self.dome_tar = (self.ra + 10.0) % 360.0 if self.tel >= 4 else self.dome_az
        if self.dome in (4, 5, 6, 7, 8):
            self.dome_az, reached = approach(self.dome_az, self.dome_tar, DOME_RATE * dt)
            if self.dome in (6, 7, 8):
                self.dome = 6 if reached else 7
            elif reached:
                self.dome = 1

        if self.focus_st == 4:
            self.focus, reached = approach(self.focus, self.focus_tar, FOCUS_RATE * dt)
            if reached:
                self.focus_st = 1

    def _cover(self, name, open_):
        cur = getattr(self, name)
        want = 3 if open_ else 4
        if cur == want:
            return
        setattr(self, name, 1 if open_ else 2)
        self.cover_target[name] = want
        self.cover_until[name] = time.time() + COVER_TIME

    # --- XML-RPC API ----------------------------------------------------
    def telescope_info(self):
        with self.lock:
            glst = [self.oil, self.tel, self.ha_st, self.da_st, self.focus_st,
                    self.dome, self.slit, self.tube, self.mirror, self.bits]
            return {
                "ut": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()),
                "glst": " ".join(str(x) for x in glst),
                "trrd": "%s %s %d" % (hms(self.ra), dms(self.dec), self.side),
                "tsra": "%s %s %d" % (hms(self.tar[0]), dms(self.tar[1]), self.tar[2]),
                "trhd": "%.4f %.4f" % (self.ha_ax, self.da_ax),
                "trgv": "0 0",
                "trus": "0 0",
                "dopo": "%.2f" % self.dome_az,
                "fopo": "%.2f" % self.focus,
                "trcs": str(self.model),
                "object": self.obj,
            }

    def telescope_set_coordinates(self, params):
        print("SIM: set_coordinates %s" % params, flush=True)
        with self.lock:
            self.tar = (parse_sexa(params["ra"], 15.0), parse_sexa(params["dec"], 1.0),
                        int(params["position"]))
            self.obj = params.get("object", "")
        return 1

    def telescope_execute(self, cmd):
        print("SIM: %s" % cmd, flush=True)
        p = cmd.split()
        if not p:
            return "0"
        op, a = p[0], p[1:]
        with self.lock:
            ready = self.tel in (3, 4)
            if op == "OION" and a:
                if a[0] == "1" and self.oil in (0, 5):
                    self.oil, self.oil_t = 1, time.time() + OIL_STEP
                elif a[0] == "0" and self.oil != 0:
                    if self.tel not in (0, 1):
                        return "0"          # telescope first
                    self.oil, self.oil_t = 5, time.time() + OIL_STEP
                return "1"
            if op == "TEON" and a:
                if a[0] == "1":
                    if self.oil != 4:
                        return "0"
                    if self.tel == 1:
                        self.tel = 3
                        self.ha_st = self.da_st = 1
                else:
                    if self.tel not in (0, 1):
                        self.tel, self.oil_t = 2, time.time() + 2.0
                        self.ha_st = self.da_st = 0
                return "1"
            if op == "TETR" and a:
                if not ready:
                    return "0"
                self.tel = 4 if a[0] == "1" else 3
                return "1"
            if op == "TSRA" and len(a) >= 3:
                self.tar = (parse_sexa(a[0], 15.0), parse_sexa(a[1], 1.0), int(a[2]))
                return "1"
            if op == "TGRA" and a:
                if a[0] == "1":
                    if not ready:
                        return "0"
                    self.tel = 12
                else:
                    if self.tel >= 6:
                        self.tel = 3
                        self.ha_st = self.da_st = 1
                return "1"
            if op == "TSHA" and len(a) >= 2:
                self.ha_tar, self.da_tar = float(a[0]), float(a[1])
                return "1"
            if op == "TGHA" and a:
                if a[0] == "1":
                    if not ready:
                        return "0"
                    self.tel = 6
                return "1"
            if op in ("TEHC", "TEDC"):
                return "1"
            if op in ("TSCA", "TSCP", "TSCR", "TSCM", "TSGM") and a:
                bit = {"TSCA": 4, "TSCP": 5, "TSCR": 6, "TSCM": 7, "TSGM": 8}[op]
                if a[0] == "1":
                    self.bits |= 1 << bit
                else:
                    self.bits &= ~(1 << bit)
                return "1"
            if op == "TSCS" and a:
                self.model = int(a[0])
                return "1"
            if op == "DOSO" and a:
                self._cover("slit", a[0] == "1")
                return "1"
            if op == "FTOC" and a:
                self._cover("tube", a[0] == "1")
                return "1"
            if op == "FMOC" and a:
                self._cover("mirror", a[0] == "1")
                return "1"
            if op == "DOAM":
                self.dome = 6
                return "1"
            if op == "DOST":
                self.dome = 1
                self.dome_tar = self.dome_az
                return "1"
            if op == "DOSA" and a:
                self.dome_tar = float(a[0]) % 360.0
                return "1"
            if op == "DOGA":
                self.dome = 4
                return "1"
            if op == "FOSA" and a:
                self.focus_tar = float(a[0])
                return "1"
            if op == "FOGA":
                self.focus_st = 4
                return "1"
            if op == "FOST":
                self.focus_st = 1
                self.focus_tar = self.focus
                return "1"
        return "0"


port = int(sys.argv[1]) if len(sys.argv) > 1 else 19999
server = SimpleXMLRPCServer(("127.0.0.1", port), logRequests=False, allow_none=True)
server.register_instance(Telescope())
print("SIM: telescoped simulator on 127.0.0.1:%d" % port, flush=True)
server.serve_forever()

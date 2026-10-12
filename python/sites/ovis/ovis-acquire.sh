#!/bin/bash
#
# OVIS spectroscopic sequence: identification image, slit shot, short and
# full spectrum, neon calibration, each grism/neon setup preceded by a
# flood image that is thrown away. Derived from the karpov-sequencer
# kseq.sh that has been copied from site to site.
#
# Run it like this:
#   rts2-scriptexec -c C0 -s 'exe /path/to/ovis-acquire.sh'
# The full spectrum exposure is EXPTIME below (300 s). A first argument
# overrides it, but the script's `exe` takes only the path, so that needs
# a wrapper.

# How many random frames between
FRAMES_PER_GROUP=2
CALIB_FRAME_SECONDS=10

EXPTIME=300
test $1 && EXPTIME=$1

echo "log I === OVIS spectroscopic sequence with EXPTIME = $EXPTIME ==="
CALTIME=20

do_exp()
{
	echo "log I Exposing exptime = $1 "
	if [ "x$2" == "xdark" ]
	then
		echo "value SHUTTER = 1"
	else
		echo "value SHUTTER = 0"
	fi
	echo "value exposure = $1"
	echo "exposure %b/queue/%N/%c/%f"
	read end_exposure_message
	read x y
	echo "rename $y %b/%N/sk/%f"
	read renamed
}

dump_exp()
{
	echo "log I Exposing exptime = $1 (not to be kept)"
	if [ "x$2" == "xdark" ]
	then
		echo "value SHUTTER = 1"
	else
		echo "value SHUTTER = 0"
	fi
	echo "value exposure = $1"
	echo "exposure %b/queue/%N/%c/%f"
	read end_exposure_message
	read x y
	echo "delete $y"
	read renamed
}

do_fake_exp()
{
	echo "value exposure = $1"
	sleep 2
}

# Setup
#echo V C0 WINDOW = 1536 1536 1024 1024
#echo V WF8 WINDOW = 0 0 1152 1152
#echo V C0 WINDOW = 0 0 4148 4124

#while true
#do
	echo V C1 BINNING = 2x2

	echo "log I Identification image"
		echo V C1 FILTB = open
		#sleep 2
		do_exp 1 

	echo "log I Slit shot"
		echo V C1 FILTB = jogurt
		#sleep 2
		do_exp 1

	echo "log I flood image"
		echo V C1 FILTA = N
		echo V C1 FILTB = open
		echo V W1 NEON = on
		#sleep 2
		dump_exp $CALTIME

	echo "log I Short spectrum"
		echo V W1 NEON = off
		echo V C1 NCLEARS = 20
		echo V C1 FILTB = jogurt
		echo V C1 FILTA = grism
		# sleep 2
		do_exp $CALTIME
		echo V C1 NCLEARS = 0

	echo "log I Full spectrum"
		do_exp $EXPTIME 
		
	echo "log I flood image"
		echo V C1 FILTA = N
		echo V C1 FILTB = open
		echo V W1 NEON = on
		#sleep 2
		dump_exp $CALTIME

	echo "log I calibration lamp"
		echo V C1 NCLEARS = 20
		echo V W1 NEON = on
		# sleep 2
		do_exp $CALTIME 
		echo V C1 NCLEARS = 0

	echo V W1 NEON = off
	echo V C1 FILTA = N
	echo V C1 FILTB = open
	echo "log I finished, reset to full-open"
#done

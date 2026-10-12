#!/bin/bash
#
# Runs the OVIS sequence (ovis-acquire.sh, next to this script) on C1;
# the argument is the full spectrum exposure in seconds (default 300).
# Was ~/bin/sp.

rts2-scriptexec -c C1 -s 'exe "'"$(dirname "$(readlink -f "$0")")"'/ovis-acquire.sh '"$1"'"'

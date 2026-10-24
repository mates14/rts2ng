# RTS2 libraries
# (C) 2009-2012 Petr Kubanek <petr@kubanek.net>
# (C) 2010-2016 Petr Kubanek, Institute of Physics
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
__version__ = '3.0'

# populate namescpace with handy classes
# rts2 note: Rts2Comm & co. exported as classic did - scripts and the image
# pipeline call rts2.Rts2Comm (asarina-imgproc reports fwhm_C<n> so, and
# failed at SBT with "module 'rts2' has no attribute 'Rts2Comm'"; D50 only
# worked through a classic copy in /usr/local shadowing this package).
# The other classic exports stay off: those modules (imgprocess, centering,
# rtsapi, queue, sextractor, ...) are not in this package.
from .scriptcomm import Rts2Comm,Rts2Exception,Rts2NotActive
#from .imgprocess import ImgProcess
#from .centering import Centering
#from .flats import Flat,FlatScript
#from .rtsapi import getProxy,createProxy
#from .queue import Queue,QueueEntry
#from .queues import Queues
#from .sextractor import Sextractor
#from .focusing import Focusing

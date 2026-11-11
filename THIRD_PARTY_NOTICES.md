## libusb 1.0.30 — LGPL-2.1

Copyright (c) 2001 Johannes Erdfelt <johannes@erdfelt.com>
Copyright (c) 2007-2009 Daniel Drake <dsd@gentoo.org>
Copyright (c) 2010-2012 Peter Stuge <peter@stuge.se>
Copyright (c) 2008-2016 Nathan Hjelm <hjelmn@users.sourceforge.net>
Copyright (c) 2009-2013 Pete Batard <pete@akeo.ie>
Copyright (c) 2009-2013 Ludovic Rousseau <ludovic.rousseau@gmail.com>
Copyright (c) 2010-2012 Michael Plante <michael.plante@gmail.com>
Copyright (c) 2011-2013 Hans de Goede <hdegoede@redhat.com>
Copyright (c) 2012-2013 Martin Pieuchot <mpi@openbsd.org>
Copyright (c) 2012-2013 Toby Gray <toby.gray@realvnc.com>
Copyright (c) 2013-2018 Chris Dickens <christopher.a.dickens@gmail.com>
and other contributors (see libusb AUTHORS).

TetherKit links libusb dynamically and redistributes an unmodified copy of
libusb-1.0.0.dylib inside TetherKit.app, so that the app runs without Homebrew
installed. The library is loaded via dyld dynamic linking and the
copy on disk can be replaced by the user.

Source for libusb 1.0.30: https://github.com/libusb/libusb/releases/tag/v1.0.30
Full license text: docs/licenses/libusb-COPYING (also shipped in the app bundle).

## doctest 2.4.12 — MIT

Copyright (c) 2016-2023 Viktor Kirilov. Vendored at third_party/doctest/ for the
test suite only; it is not part of any shipped binary.

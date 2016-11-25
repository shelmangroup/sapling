# statprofext.py - improved statprof integration
#
# Copyright 2013 Facebook, Inc.
#
# This software may be used and distributed according to the terms of the
# GNU General Public License version 2 or any later version.
"""integrates the statprof profiler more tightly with Mercurial

This extension configures the statprof based on Mercurial config knobs.

  statprof.mechanism - configures which profiling mechanism is used
                       (thread, signal)
  statprof.format - configures the output format
                    (hotpath, json)
"""

import contextlib
from mercurial.i18n import _
from mercurial import (
    error,
    extensions,
    profiling,
)

def extsetup(ui):
    extensions.wrapfunction(profiling, 'statprofile', statprofile)

@contextlib.contextmanager
def statprofile(orig, ui, fp):
    try:
        import statprof
    except ImportError:
        raise error.Abort(_(
            'statprof not available - install using "easy_install statprof"'))

    freq = ui.configint('profiling', 'freq', default=1000)
    if freq > 0:
        statprof.reset(freq)
    else:
        ui.warn(_("invalid sampling frequency '%s' - ignoring\n") % freq)

    mechanism = ui.config('statprof', 'mechanism', 'thread')
    statprof.start(mechanism=mechanism)
    try:
        yield
    finally:
        statprof.stop()

        profformat = ui.config('statprof', 'format', 'hotpath')

        if profformat == 'hotpath':
            displayformat = statprof.DisplayFormats.Hotpath
        elif profformat == 'json':
            displayformat = statprof.DisplayFormats.Json
        else:
            ui.warn(_("unknown profiler output format: %s") % profformat)
            displayformat = statprof.DisplayFormats.Hotpath

        statprof.display(fp, format=displayformat)

Drive C: of the TOS container (starttos).

Your own TOS folder, ~/TOS on Unix, made by maketos from /tos/sys:
programs in AUTO run at boot, accessories (*.ACC) load from here, and
the desktop is saved here.  APPS holds the editor Qed.  Drive G: is
the system's games (/usr/games/tos), U: the Unix tree.  tosdrive adds
your own drives.

GEM draws through fVDI (AUTO\FVDI.PRG) at the screen's full size.
Rename it to FVDI.PRX to boot with the ST screen; text-mode
programs need that.  Its source is in /tos/src/fvdi.

TERADESK holds TeraDesk, a desktop to use instead of the built-in one:
select TERADESK\DESKTOP.PRG, choose Options > Install application,
set Boot status to Auto, then Options > Save desktop.  Quitting it
returns to the built-in desktop; set Boot status back to Normal there
to stop using it.  Its source is in /tos/src.

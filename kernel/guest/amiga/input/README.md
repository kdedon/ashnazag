container-input is an AmigaDOS background command for this container.
It feeds session keyboard and mouse events to input.device with IND_WRITEEVENT.
Events carry the system time from timer.device.
Requires AmigaOS 2.04 or newer and the startmig shared input mapping.

Build on the host:
    sh kernel/guest/amiga/input/build.sh images/work/amiga-input

Copy container-input to the guest C: directory, then start it once:
    Run >NIL: C:container-input

Add that command to S:User-Startup after the system input device is available.
Use Status to find its process number and Break <number> C to stop it cleanly.
Only one instance can attach. Run it inside the container, whose fixed shared
mapping must exist; running it on an ordinary Amiga can cause a bus error.

The display child binds /dev/kbd and /dev/mouse to its framebuffer session.
It translates ADB and IKBD physical US positions to Amiga raw key codes;
native Amiga input codes pass through. Guest keymaps choose characters.
Modifiers, keypad qualification, caps latch changes, three mouse buttons,
and relative motion are carried in a 256-event single-producer queue.
Unsupported host keys and absolute pointing devices are ignored.

Focus loss, device errors, and queue overflow request a reset. The guest
releases tracked keys/buttons, drops queued events, then acknowledges the
reset before the host resumes publishing. Events during recovery are dropped.
A held key must be released and pressed again after recovery. The command
polls once per DOS tick and processes at most 64 events before yielding.
This bridge does not provide keyboard.device raw-matrix state or native
custom-chip input. Guest execution and input delivery remain unverified.

Interface references:
https://developer.amigaos3.net/autodocs/input.device/IND_WRITEEVENT.html
https://wiki.amigaos.net/wiki/Input_Device
https://wiki.amigaos.net/wiki/Keyboard_Device

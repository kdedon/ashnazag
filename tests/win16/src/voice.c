/*
 * voice.c -- SOUND.DRV's voice: notes queued, played, waited for.  The
 * results go to RESULT.TXT; the test script checks the samples played
 * (W16_SND): their length and the first note's pitch.
 */
#include <windows.h>
#include <stdio.h>
#include "result.h"

static void
line(char *what, int v)
{
	char buf[80];

	sprintf(buf, "%s %d", what, v);
	report(buf);
}

int PASCAL
WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
	line("open", OpenSound());
	line("open again", OpenSound());
	line("accent", SetVoiceAccent(1, 120, 128, S_NORMAL, 0));
	line("a4 quarter", SetVoiceNote(1, 46, 4, 0));		/* 440 Hz, 500 ms */
	line("rest eighth", SetVoiceNote(1, 0, 8, 0));		/* 250 ms */
	line("c4 dotted eighth", SetVoiceNote(1, 37, 8, 1));	/* 262 Hz, 375 ms */
	line("bad note", SetVoiceNote(1, 99, 4, 0));
	line("queued", CountVoiceNotes(1));
	line("start", StartSound());
	line("wait", WaitSoundState(S_QUEUEEMPTY));
	line("left", CountVoiceNotes(1));
	CloseSound();
	line("open after close", OpenSound());
	CloseSound();
	return 0;
}

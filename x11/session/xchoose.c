/*
 * xchoose -- pick one of a few sessions.
 *
 *	xchoose [-d default] [-t seconds] name=label ...
 *
 * Prints the chosen name.  Click a button, type its number, or press
 * Return for the default (thick border), which -t also picks after the
 * given time.  Escape picks nothing: exit 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <X11/Xaw/Box.h>
#include <X11/Xaw/Label.h>
#include <X11/Xaw/Command.h>

#define MAXE 9

static char *names[MAXE];
static int nent, def, left;
static Widget title;

static void
done(i)
	int i;
{
	if (i < 0)
		exit(1);
	printf("%s\n", names[i]);
	exit(0);
}

static void
picked(w, cl, call)
	Widget w;
	XtPointer cl, call;
{
	done((int)(long)cl);
}

/* key actions: pick(N), pick() for the default, pick(none) */
static void
pick(w, ev, par, np)
	Widget w;
	XEvent *ev;
	String *par;
	Cardinal *np;
{
	int i;

	if (*np == 0)
		done(def);
	if (strcmp(par[0], "none") == 0)
		done(-1);
	i = atoi(par[0]) - 1;
	if (i >= 0 && i < nent)
		done(i);
}

/* without a window manager nobody gives it the focus */
static void
mapped(w, cl, ev, cont)
	Widget w;
	XtPointer cl;
	XEvent *ev;
	Boolean *cont;
{
	if (ev->type == MapNotify)
		XSetInputFocus(XtDisplay(w), XtWindow(w), RevertToPointerRoot, CurrentTime);
}

static void
tick(cl, id)
	XtPointer cl;
	XtIntervalId *id;
{
	char buf[80];
	Arg a[1];

	if (left <= 0)
		done(def);
	sprintf(buf, "Choose a session (%d)", left--);
	XtSetArg(a[0], XtNlabel, buf);
	XtSetValues(title, a, 1);
	XtAppAddTimeOut((XtAppContext)cl, 1000, tick, cl);
}

static XtActionsRec actions[] = { { "pick", pick } };

/* on release: a key still down when the session starts goes to it */
static char keys[] =
	"<KeyUp>Return: pick()\n<KeyUp>KP_Enter: pick()\n<KeyUp>Escape: pick(none)\n"
	"<KeyUp>1: pick(1)\n<KeyUp>2: pick(2)\n<KeyUp>3: pick(3)\n<KeyUp>4: pick(4)\n"
	"<KeyUp>5: pick(5)\n<KeyUp>6: pick(6)\n<KeyUp>7: pick(7)\n<KeyUp>8: pick(8)\n"
	"<KeyUp>9: pick(9)\n";

int
main(argc, argv)
	int argc;
	char **argv;
{
	XtAppContext app;
	Widget top, box, w;
	XtTranslations tr;
	Arg a[4];
	Dimension wd, ht;
	char *dflt = 0, *eq, *prog, lab[80];
	int i;

	/* before Xt, which takes -d and -t for -display and -title */
	prog = argv[0];
	for (; argc > 2 && argv[1][0] == '-'; argc -= 2, argv += 2)
		if (strcmp(argv[1], "-d") == 0)
			dflt = argv[2];
		else if (strcmp(argv[1], "-t") == 0)
			left = atoi(argv[2]);
	argv[0] = prog;
	top = XtAppInitialize(&app, "XChoose", 0, 0, &argc, argv, 0, 0, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: xchoose [-d default] [-t seconds] name=label ...\n");
		return 2;
	}
	for (i = 1; dflt && i < argc; i++)
		if (strncmp(argv[i], dflt, strlen(dflt)) == 0 && argv[i][strlen(dflt)] == '=')
			def = i - 1;
	XtAppAddActions(app, actions, 1);
	tr = XtParseTranslationTable(keys);
	XtSetArg(a[0], XtNorientation, XtorientVertical);
	XtSetArg(a[1], XtNhSpace, 16);
	XtSetArg(a[2], XtNvSpace, 8);
	box = XtCreateManagedWidget("box", boxWidgetClass, top, a, 3);
	XtOverrideTranslations(box, tr);
	XtSetArg(a[0], XtNlabel, "Choose a session");
	XtSetArg(a[1], XtNborderWidth, 0);
	title = XtCreateManagedWidget("title", labelWidgetClass, box, a, 2);
	XtOverrideTranslations(title, tr);
	for (i = 1; i < argc && nent < MAXE; i++) {
		if ((eq = strchr(argv[i], '=')) == 0)
			continue;
		*eq++ = 0;
		names[nent] = argv[i];
		sprintf(lab, "%d  %.60s", nent + 1, eq);
		XtSetArg(a[0], XtNlabel, lab);
		XtSetArg(a[1], XtNborderWidth, def == nent ? 3 : 1);
		w = XtCreateManagedWidget(argv[i], commandWidgetClass, box, a, 2);
		XtAddCallback(w, XtNcallback, picked, (XtPointer)(long)nent);
		XtOverrideTranslations(w, tr);
		nent++;
	}
	if (nent == 0 || def >= nent)
		return 2;
	if (left > 0)
		tick((XtPointer)app, 0);
	XtAddEventHandler(top, StructureNotifyMask, False, mapped, 0);
	XtRealizeWidget(top);
	/* centred; keys go to it wherever the pointer is */
	XtSetArg(a[0], XtNwidth, &wd);
	XtSetArg(a[1], XtNheight, &ht);
	XtGetValues(top, a, 2);
	XtMoveWidget(top, (WidthOfScreen(XtScreen(top)) - wd) / 2,
		(HeightOfScreen(XtScreen(top)) - ht) / 2);
	XtAppMainLoop(app);
	return 0;
}

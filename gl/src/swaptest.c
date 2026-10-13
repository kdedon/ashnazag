/*
 * swaptest.c -- per-frame overhead: clear and swap only.
 *
 *   swaptest [frames [width height]]
 *
 * Prints frames per second.  With SWAPNOCLEAR set, swaps only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <GL/gl.h>
#include <GL/glx.h>

int
main(int argc, char **argv)
{
	static int attr[] = { GLX_RGBA, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1,
	    GLX_BLUE_SIZE, 1, GLX_DEPTH_SIZE, 1, GLX_DOUBLEBUFFER, None };
	Display *dpy;
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	XEvent ev;
	Window win;
	GLXContext ctx;
	struct timeval a, b;
	int frames, w, h, i, clear;
	double s;

	frames = argc > 1 ? atoi(argv[1]) : 500;
	w = argc > 3 ? atoi(argv[2]) : 300;
	h = argc > 3 ? atoi(argv[3]) : 300;
	clear = getenv("SWAPNOCLEAR") == NULL;
	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		printf("swaptest: no display\n");
		return 1;
	}
	if ((vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr)) == NULL) {
		printf("swaptest: no visual\n");
		return 1;
	}
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, w, h, 0, vi->depth,
	    InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XSelectInput(dpy, win, StructureNotifyMask);
	XMapWindow(dpy, win);
	do
		XNextEvent(dpy, &ev);
	while (ev.type != MapNotify);
	ctx = glXCreateContext(dpy, vi, NULL, True);
	glXMakeCurrent(dpy, win, ctx);
	glClearColor(0, 0, 0.5, 1);
	gettimeofday(&a, NULL);
	for (i = 0; i < frames; i++) {
		if (clear)
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glXSwapBuffers(dpy, win);
	}
	glFinish();
	gettimeofday(&b, NULL);
	s = b.tv_sec - a.tv_sec + (b.tv_usec - a.tv_usec) / 1e6;
	printf("swaptest: %s %dx%d %s: %d frames in %.2f s, %.1f fps\n",
	    glGetString(GL_RENDERER), w, h, clear ? "clear+swap" : "swap", frames, s, frames / s);
	return 0;
}

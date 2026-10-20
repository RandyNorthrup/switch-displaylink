/*
 * dl-edgewrap -- make the pointer follow the physical monitor arrangement.
 *
 * The Switch panel has to sit at x=0 of the enlarged X screen (see dl-extend),
 * so monitors that physically stand to its LEFT are placed at the far right
 * end of the X screen instead. This helper joins the two ends: pushing the
 * pointer past the left edge (x=0) moves it to the right end and back.
 * Pointer barriers keep it from crossing where two monitors are neighbours in
 * the X screen but not on the desk, and out of the unused strip below the
 * panel.
 *
 *   dl-edgewrap FB_W FB_H PANEL_W PANEL_H [BARRIER_X]
 *
 * BARRIER_X: x of the seam between the last monitor right of the panel and
 * the first one left of it; 0 = no monitor left of the panel, nothing to join.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>

#define PUSH 24 /* raw units pushed against the edge before wrapping */

/* XNextEvent restarts after a signal, so just leave: the barriers go with the connection. */
static void on_signal(int s) { (void)s; _exit(0); }

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: dl-edgewrap FB_W FB_H PANEL_W PANEL_H [BARRIER_X]\n");
		return 2;
	}
	int fw = atoi(argv[1]), fh = atoi(argv[2]), pw = atoi(argv[3]), ph = atoi(argv[4]);
	int bx = argc > 5 ? atoi(argv[5]) : 0;

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "dl-edgewrap: cannot open X display\n");
		return 1;
	}
	Window root = DefaultRootWindow(dpy);
	int xi_op, ev, err;
	if (!XQueryExtension(dpy, "XInputExtension", &xi_op, &ev, &err)) {
		fprintf(stderr, "dl-edgewrap: no XInput\n");
		return 1;
	}

	if (bx > 0 && bx < fw)
		XFixesCreatePointerBarrier(dpy, root, bx, 0, bx, fh, 0, 0, NULL);
	if (ph < fh) {
		/* the strip below the panel shows on no monitor */
		XFixesCreatePointerBarrier(dpy, root, 0, ph, pw, ph, 0, 0, NULL);
		XFixesCreatePointerBarrier(dpy, root, pw, ph, pw, fh, 0, 0, NULL);
	}

	unsigned char bits[XIMaskLen(XI_LASTEVENT)] = { 0 };
	XIEventMask mask = { .deviceid = XIAllMasterDevices, .mask_len = sizeof(bits), .mask = bits };
	XISetMask(bits, XI_RawMotion);
	XISelectEvents(dpy, root, &mask, 1);
	XFlush(dpy);

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	double push = 0;
	for (;;) {
		XEvent e;
		XNextEvent(dpy, &e);
		if (e.xcookie.type != GenericEvent || e.xcookie.extension != xi_op ||
		    !XGetEventData(dpy, &e.xcookie))
			continue;
		double dx = 0;
		if (e.xcookie.evtype == XI_RawMotion) {
			XIRawEvent *re = e.xcookie.data;
			if (re->valuators.mask_len > 0 && XIMaskIsSet(re->valuators.mask, 0))
				dx = re->raw_values[0];
		}
		XFreeEventData(dpy, &e.xcookie);
		if (dx == 0)
			continue;

		Window r, c;
		int x, y, wx, wy;
		unsigned m;
		if (!XQueryPointer(dpy, root, &r, &c, &x, &y, &wx, &wy, &m))
			continue;
		/* only wrap when some monitor stands left of the panel */
		int at_left = bx > 0 && x <= 0 && dx < 0, at_right = bx > 0 && x >= fw - 1 && dx > 0;
		if (!at_left && !at_right) {
			push = 0;
			continue;
		}
		push += dx < 0 ? -dx : dx;
		if (push < PUSH)
			continue;
		push = 0;
		if (at_right && y >= ph)
			y = ph - 1; /* onto the panel, not the strip below it */
		XWarpPointer(dpy, None, root, 0, 0, 0, 0, at_left ? fw - 2 : 1, y);
		XFlush(dpy);
	}
	XCloseDisplay(dpy);
	return 0;
}

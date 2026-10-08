/*
 * dl-mirror — mirror the X screen onto a DisplayLink (evdi) output via KMS.
 *
 * The NVIDIA Tegra X driver has no RandR PRIME, so X can't drive the evdi
 * card itself. Instead we become DRM master on the evdi card (libevdi in
 * DisplayLinkManager drops master, so it is free), scan out a dumb buffer,
 * and keep it filled by grabbing the root window with XShm. DisplayLinkManager
 * picks the pixels up from evdi and pushes them over USB.
 *
 * Double-buffered: frames are drawn into the hidden buffer and page-flipped
 * in. evdi only completes a flip after DisplayLinkManager has grabbed the new
 * buffer, so the buffer we draw into is never being read (no tearing). With
 * -1 it falls back to one buffer updated in place + drmModeDirtyFB.
 *
 * Only changed rows are copied, and before each flip the back buffer is
 * dirtied with just those rows, so evdi hands DisplayLinkManager a small rect
 * instead of marking the whole screen dirty (a full-screen encode per keystroke
 * made typing lag on the dock). A static desktop costs almost nothing.
 *
 * DL_MIRROR_TRACE=1 logs every push and flip; =2 also blocks on each flip to
 * time the flip event (diagnostic only).
 *
 * With -w it waits for the dock monitor instead of exiting, and goes back to
 * waiting when the dock goes away (used by dl-mirror.service).
 *
 * Build: make -C mirror      Run: dl-mirror [-w] [-d /dev/dri/cardN] [-f fps]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <libgen.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>

/* quiet: set while -w is retrying, so a missing dock doesn't flood the log. */
static int quiet;
#define log(...) do { if (!quiet) { fprintf(stderr, "dl-mirror: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static volatile sig_atomic_t quit;
static void on_signal(int sig) { (void)sig; quit = 1; }

#define NBUF 2

struct kms {
	int fd;
	uint32_t conn_id, crtc_id;
	uint32_t fb_id[NBUF], handle[NBUF];
	uint32_t *map[NBUF];
	int nbuf, front;
	int flip_pending;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	uint32_t pitch; /* in pixels */
	size_t size;
	double t_flip; /* when the pending flip was queued */
};

/* DL_MIRROR_TRACE=1: log every push and how long the flip took to land. */
static int trace;
static double now(void);

/* Find the card whose kernel driver is evdi. */
static int open_evdi_card(const char *want)
{
	if (want)
		return open(want, O_RDWR | O_CLOEXEC);

	for (int i = 0; i < 16; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmVersionPtr v = drmGetVersion(fd);
		int match = v && strcmp(v->name, "evdi") == 0;
		drmFreeVersion(v);
		if (match) {
			log("using %s (evdi)", path);
			return fd;
		}
		close(fd);
	}
	errno = ENODEV;
	return -1;
}

/*
 * Does any evdi card report a connected monitor? Reads sysfs only: opening
 * the card makes evdi log Opened/Closed every time, which -w would spam.
 */
static int evdi_monitor_present(void)
{
	glob_t g;
	int found = 0;
	if (glob("/sys/class/drm/card[0-9]*-*/status", 0, NULL, &g))
		return 0;
	for (size_t i = 0; i < g.gl_pathc && !found; i++) {
		char card[256], drv[256], link[512], st[32] = "";
		/* /sys/class/drm/card0-DVI-I-1/status -> card0 */
		if (sscanf(g.gl_pathv[i], "/sys/class/drm/%255[^-]", card) != 1)
			continue;
		snprintf(link, sizeof(link), "/sys/class/drm/%s/device/driver", card);
		ssize_t n = readlink(link, drv, sizeof(drv) - 1);
		if (n <= 0)
			continue;
		drv[n] = 0;
		if (strcmp(basename(drv), "evdi"))
			continue;
		FILE *f = fopen(g.gl_pathv[i], "r");
		if (f) {
			found = fgets(st, sizeof(st), f) && !strncmp(st, "connected", 9);
			fclose(f);
		}
	}
	globfree(&g);
	return found;
}

/* Prefer a mode matching the X screen exactly (no scaling), else preferred. */
static int pick_mode(drmModeConnector *c, int w, int h, drmModeModeInfo *out)
{
	int best = -1;
	for (int i = 0; i < c->count_modes; i++) {
		if (c->modes[i].hdisplay == w && c->modes[i].vdisplay == h) {
			if (best < 0 || c->modes[i].vrefresh == 60)
				best = i;
		}
	}
	if (best < 0) {
		for (int i = 0; i < c->count_modes; i++)
			if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { best = i; break; }
	}
	if (best < 0 && c->count_modes > 0)
		best = 0;
	if (best < 0)
		return -1;
	*out = c->modes[best];
	return 0;
}

static int kms_setup(struct kms *k, const char *dev, int src_w, int src_h,
		     int force_w, int force_h, int nbuf)
{
	memset(k, 0, sizeof(*k));
	k->nbuf = nbuf;
	k->fd = open_evdi_card(dev);
	if (k->fd < 0) {
		log("no evdi card: %s", strerror(errno));
		return -1;
	}
	if (drmSetMaster(k->fd) && errno != EINVAL)
		log("drmSetMaster: %s (continuing, may already be master)", strerror(errno));

	drmModeRes *res = drmModeGetResources(k->fd);
	if (!res) {
		log("drmModeGetResources: %s", strerror(errno));
		return -1;
	}

	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors && !conn; i++) {
		drmModeConnector *c = drmModeGetConnector(k->fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
			conn = c;
		else
			drmModeFreeConnector(c);
	}
	if (!conn) {
		log("no connected monitor on the dock");
		drmModeFreeResources(res);
		errno = EAGAIN;
		return -1;
	}
	k->conn_id = conn->connector_id;

	if (pick_mode(conn, force_w ? force_w : src_w, force_h ? force_h : src_h,
		      &k->mode)) {
		log("connector has no modes");
		return -1;
	}

	/* CRTC: the encoder's current one, else the first it can drive. */
	drmModeEncoder *enc = conn->encoder_id ?
		drmModeGetEncoder(k->fd, conn->encoder_id) : NULL;
	if (enc && enc->crtc_id)
		k->crtc_id = enc->crtc_id;
	for (int e = 0; e < conn->count_encoders && !k->crtc_id; e++) {
		drmModeEncoder *en = drmModeGetEncoder(k->fd, conn->encoders[e]);
		for (int i = 0; en && i < res->count_crtcs; i++)
			if (en->possible_crtcs & (1u << i)) { k->crtc_id = res->crtcs[i]; break; }
		drmModeFreeEncoder(en);
	}
	drmModeFreeEncoder(enc);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	if (!k->crtc_id) {
		log("no usable CRTC");
		return -1;
	}

	for (int i = 0; i < k->nbuf; i++) {
		struct drm_mode_create_dumb cd = {
			.width = k->mode.hdisplay, .height = k->mode.vdisplay, .bpp = 32,
		};
		if (drmIoctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
			log("create dumb: %s", strerror(errno));
			return -1;
		}
		k->handle[i] = cd.handle;
		k->pitch = cd.pitch / 4;
		k->size = cd.size;

		if (drmModeAddFB(k->fd, cd.width, cd.height, 24, 32, cd.pitch, cd.handle,
				 &k->fb_id[i])) {
			log("addfb: %s", strerror(errno));
			return -1;
		}

		struct drm_mode_map_dumb md = { .handle = cd.handle };
		if (drmIoctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
			log("map dumb: %s", strerror(errno));
			return -1;
		}
		k->map[i] = mmap(NULL, k->size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd,
				 md.offset);
		if (k->map[i] == MAP_FAILED) {
			k->map[i] = NULL;
			log("mmap: %s", strerror(errno));
			return -1;
		}
		memset(k->map[i], 0, k->size);
	}

	k->saved_crtc = drmModeGetCrtc(k->fd, k->crtc_id);
	if (drmModeSetCrtc(k->fd, k->crtc_id, k->fb_id[0], 0, 0, &k->conn_id, 1, &k->mode)) {
		log("setcrtc %dx%d: %s", k->mode.hdisplay, k->mode.vdisplay, strerror(errno));
		return -1;
	}
	log("scanning out %dx%d@%d on crtc %u, %s-buffered", k->mode.hdisplay,
	    k->mode.vdisplay, k->mode.vrefresh, k->crtc_id, k->nbuf > 1 ? "double" : "single");
	return 0;
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
	(void)fd; (void)seq; (void)sec; (void)usec;
	struct kms *k = data;
	if (trace)
		fprintf(stderr, "dl-mirror: trace flip done %.1f ms (t=%.3f)\n", (now() - k->t_flip) * 1e3, now());
	k->flip_pending = 0;
	k->front = !k->front;
}

/*
 * Wait up to timeout seconds for the pending flip to complete.
 * Returns 0 when no flip is pending, -1 on timeout or error.
 */
static int kms_wait_flip(struct kms *k, double timeout)
{
	drmEventContext ev = {
		.version = 2,
		.page_flip_handler = on_flip,
	};
	while (k->flip_pending) {
		struct pollfd pfd = { .fd = k->fd, .events = POLLIN };
		int r = poll(&pfd, 1, (int)(timeout * 1000));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		if (drmHandleEvent(k->fd, &ev))
			return -1;
	}
	return 0;
}

static void kms_teardown(struct kms *k)
{
	if (k->fd < 0)
		return;
	if (k->flip_pending && kms_wait_flip(k, 1.0))
		log("flip still pending at exit");
	/* Blank the output instead of leaving a stale frame up. */
	if (k->crtc_id)
		drmModeSetCrtc(k->fd, k->crtc_id, 0, 0, 0, NULL, 0, NULL);
	drmModeFreeCrtc(k->saved_crtc);
	for (int i = 0; i < NBUF; i++) {
		if (k->map[i])
			munmap(k->map[i], k->size);
		if (k->fb_id[i])
			drmModeRmFB(k->fd, k->fb_id[i]);
		if (k->handle[i]) {
			struct drm_mode_destroy_dumb dd = { .handle = k->handle[i] };
			drmIoctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		}
	}
	drmDropMaster(k->fd);
	close(k->fd);
	k->fd = -1;
}

/*
 * Source → destination geometry. Aspect-preserving fit, centered, with
 * precomputed nearest-neighbour lookup tables.
 */
struct scaler {
	int dx, dy, dw, dh;     /* destination rect inside the fb */
	int *xmap;              /* dw entries: source column */
	int *ymap;              /* dh entries: source row */
	int identity;
};

static void scaler_init(struct scaler *s, int sw, int sh, int fw, int fh)
{
	if ((long)fw * sh <= (long)fh * sw) {
		s->dw = fw;
		s->dh = (int)((long)sh * fw / sw);
	} else {
		s->dh = fh;
		s->dw = (int)((long)sw * fh / sh);
	}
	s->dx = (fw - s->dw) / 2;
	s->dy = (fh - s->dh) / 2;
	s->identity = s->dw == sw && s->dh == sh;
	s->xmap = malloc(sizeof(int) * s->dw);
	s->ymap = malloc(sizeof(int) * s->dh);
	for (int x = 0; x < s->dw; x++)
		s->xmap[x] = (int)((long)x * sw / s->dw);
	for (int y = 0; y < s->dh; y++)
		s->ymap[y] = (int)((long)y * sh / s->dh);
}

/* Overlay the X cursor (XShm grabs don't include it). */
static void draw_cursor(uint32_t *img, int stride, int w, int h, XFixesCursorImage *ci)
{
	int x0 = ci->x - ci->xhot, y0 = ci->y - ci->yhot;
	for (int cy = 0; cy < ci->height; cy++) {
		int y = y0 + cy;
		if (y < 0 || y >= h)
			continue;
		for (int cx = 0; cx < ci->width; cx++) {
			int x = x0 + cx;
			if (x < 0 || x >= w)
				continue;
			uint32_t p = (uint32_t)ci->pixels[cy * ci->width + cx];
			uint32_t a = p >> 24;
			if (!a)
				continue;
			uint32_t *d = &img[y * stride + x];
			if (a == 255) {
				*d = p;
				continue;
			}
			/* Premultiplied ARGB over XRGB. */
			uint32_t dp = *d, ia = 255 - a;
			uint32_t r = ((p >> 16) & 0xff) + (((dp >> 16) & 0xff) * ia) / 255;
			uint32_t g = ((p >> 8) & 0xff) + (((dp >> 8) & 0xff) * ia) / 255;
			uint32_t b = (p & 0xff) + ((dp & 0xff) * ia) / 255;
			*d = (r << 16) | (g << 8) | b;
		}
	}
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: dl-mirror [-w] [-d /dev/dri/cardN] [-f fps] [-m WxH] [-C] [-1]\n"
		"  -w  wait for the dock monitor (and again after it goes away) instead of exiting\n"
		"  -d  DRM device (default: auto-detect the evdi card)\n"
		"  -f  max frames per second (default 30)\n"
		"  -m  force output mode WxH (default: match X screen, else monitor preferred)\n"
		"  -C  don't draw the mouse cursor\n"
		"  -1  single buffer + dirtyfb (may tear) instead of page flipping\n");
}

int main(int argc, char **argv)
{
	const char *dev = NULL;
	int fps = 30, force_w = 0, force_h = 0, want_cursor = 1, nbuf = NBUF, wait = 0, opt;
	trace = getenv("DL_MIRROR_TRACE") ? atoi(getenv("DL_MIRROR_TRACE")) : 0;

	while ((opt = getopt(argc, argv, "wd:f:m:C1h")) != -1) {
		switch (opt) {
		case 'w': wait = 1; break;
		case 'd': dev = optarg; break;
		case 'f': fps = atoi(optarg); if (fps < 1) fps = 1; break;
		case 'm':
			if (sscanf(optarg, "%dx%d", &force_w, &force_h) != 2) { usage(); return 2; }
			break;
		case 'C': want_cursor = 0; break;
		case '1': nbuf = 1; break;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}

	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		log("cannot open X display %s", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
		return 1;
	}
	if (!XShmQueryExtension(dpy)) {
		log("X server lacks MIT-SHM");
		return 1;
	}
	int scr = DefaultScreen(dpy);
	Window root = RootWindow(dpy, scr);
	int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
	if (DefaultDepth(dpy, scr) != 24) {
		log("unsupported X depth %d (need 24)", DefaultDepth(dpy, scr));
		return 1;
	}

	int fx_event, fx_error;
	if (want_cursor && !XFixesQueryExtension(dpy, &fx_event, &fx_error)) {
		log("no XFIXES, cursor will not be shown");
		want_cursor = 0;
	}

	XShmSegmentInfo shm = { 0 };
	XImage *img = XShmCreateImage(dpy, DefaultVisual(dpy, scr), 24, ZPixmap, NULL,
				      &shm, sw, sh);
	if (!img || img->bits_per_pixel != 32) {
		log("XShmCreateImage failed or not 32bpp");
		return 1;
	}
	shm.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * sh, IPC_CREAT | 0600);
	shm.shmaddr = img->data = shmat(shm.shmid, NULL, 0);
	shm.readOnly = False;
	if (shm.shmid < 0 || shm.shmaddr == (void *)-1 || !XShmAttach(dpy, &shm)) {
		log("shm setup failed");
		return 1;
	}
	XSync(dpy, False);
	shmctl(shm.shmid, IPC_RMID, NULL); /* freed once both sides detach */

	int stride = img->bytes_per_line / 4;
	uint32_t *src = (uint32_t *)img->data;
	uint32_t *prev = calloc((size_t)stride * sh, 4);
	uint32_t *frame = malloc((size_t)stride * sh * 4); /* src + cursor */

	struct kms k = { .fd = -1 };
	struct scaler s = { 0 };
	int ret = 0;

again:
	for (;;) {
		if (wait && !dev && !evdi_monitor_present())
			errno = EAGAIN;
		else if (!kms_setup(&k, dev, sw, sh, force_w, force_h, nbuf))
			break;
		int tempfail = errno == EAGAIN || errno == ENODEV;
		kms_teardown(&k);
		if (!wait || quit) {
			ret = tempfail ? 75 /* EX_TEMPFAIL */ : 1;
			goto out;
		}
		if (!quiet)
			log("waiting for the dock monitor");
		quiet = 1;
		sleep(2);
	}
	quiet = 0;

	free(s.xmap);
	free(s.ymap);
	scaler_init(&s, sw, sh, k.mode.hdisplay, k.mode.vdisplay);
	log("X screen %dx%d -> %dx%d at +%d+%d%s, %d fps max", sw, sh, s.dw, s.dh,
	    s.dx, s.dy, s.identity ? " (1:1)" : " (scaled)", fps);

	int force_full = 1;
	const double period = 1.0 / fps;
	unsigned long frames = 0, pushed = 0, skipped = 0;
	int ly0 = 0, ly1 = sh; /* source rows changed by the previous push */
	double t_next = now(), t_stat = t_next, t_full = t_next, t_flip = 0;
	double t_start = t_next, t_full_push = t_next;

	while (!quit) {
		if (!XShmGetImage(dpy, root, img, 0, 0, AllPlanes)) {
			log("XShmGetImage failed");
			ret = 1;
			break;
		}
		memcpy(frame, src, (size_t)stride * sh * 4);
		if (want_cursor) {
			XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
			if (ci) {
				draw_cursor(frame, stride, sw, sh, ci);
				XFree(ci);
			}
		}

		/*
		 * DisplayLinkManager reconnects right after our modeset and drops
		 * what it had, so a one-off full push isn't enough: resend the whole
		 * frame every second for the first few seconds. After that only every
		 * 30 s as a safety net: a full frame costs DLM a whole-screen encode,
		 * and keystrokes queued behind it showed up late on the dock.
		 */
		if (now() - t_full >= 1.0) {
			if (now() - t_start < 5.0 || now() - t_full_push >= 30.0) {
				force_full = 1;
				t_full_push = now();
			}
			t_full = now();

			/* Dock gone or DisplayLinkManager disconnected: flips still
			 * "succeed" on evdi, so check the connector. */
			drmModeConnector *c = drmModeGetConnectorCurrent(k.fd, k.conn_id);
			int up = c && c->connection == DRM_MODE_CONNECTED;
			drmModeFreeConnector(c);
			if (!up) {
				log("dock monitor disconnected");
				ret = 75;
				break;
			}
		}

		/* A flip that completed after the fallback moved scanout: repaint. */
		if (k.nbuf == 1 && k.flip_pending && !kms_wait_flip(&k, 0))
			force_full = 1;

		/* Changed source rows → [y0, y1). */
		int y0 = 0, y1 = sh;
		if (!force_full) {
			size_t rb = (size_t)sw * 4;
			while (y0 < sh && !memcmp(&frame[y0 * stride], &prev[y0 * stride], rb))
				y0++;
			while (y1 > y0 && !memcmp(&frame[(y1 - 1) * stride], &prev[(y1 - 1) * stride], rb))
				y1--;
		}

		/*
		 * Still waiting for DisplayLinkManager to take the last flip: skip
		 * this frame rather than block. prev is left alone, so the changes
		 * get picked up next time.
		 */
		if (y1 > y0 && k.nbuf > 1 && k.flip_pending && kms_wait_flip(&k, 0)) {
			if (now() - t_flip > 2.0) {
				log("flip not completed after 2 s, falling back to single buffer");
				k.nbuf = 1;
				force_full = 1;
				y0 = 0;
				y1 = sh;
			} else {
				skipped++;
				y1 = y0;
			}
		}

		if (y1 > y0) {
			int retry = 0;
			static double t_push;
			if (trace) {
				double t = now();
				log("trace push rows %d-%d%s after %.0f ms idle", y0, y1,
				    force_full ? " (full)" : "", t_push ? (t - t_push) * 1e3 : 0);
				t_push = t;
			}
			memcpy(&prev[y0 * stride], &frame[y0 * stride], (size_t)(y1 - y0) * stride * 4);

			/*
			 * The back buffer holds the frame from two pushes ago, so it
			 * also needs the rows the previous push changed.
			 */
			int back = k.nbuf > 1 ? !k.front : k.front;
			int c0 = y0, c1 = y1;
			if (k.nbuf > 1) {
				if (ly0 < c0) c0 = ly0;
				if (ly1 > c1) c1 = ly1;
			}
			ly0 = y0;
			ly1 = y1;

			/* Destination rows whose source row falls in [c0, c1). */
			int d0 = 0, d1 = s.dh;
			while (d0 < s.dh && s.ymap[d0] < c0) d0++;
			while (d1 > d0 && s.ymap[d1 - 1] >= c1) d1--;

			for (int y = d0; y < d1; y++) {
				uint32_t *dst = &k.map[back][(size_t)(s.dy + y) * k.pitch + s.dx];
				const uint32_t *sr = &frame[(size_t)s.ymap[y] * stride];
				if (s.identity) {
					memcpy(dst, sr, (size_t)sw * 4);
				} else {
					for (int x = 0; x < s.dw; x++)
						dst[x] = sr[s.xmap[x]];
				}
			}

			drmModeClip clip = {
				.x1 = force_full ? 0 : s.dx,
				.y1 = force_full ? 0 : s.dy + d0,
				.x2 = force_full ? k.mode.hdisplay : s.dx + s.dw,
				.y2 = force_full ? k.mode.vdisplay : s.dy + d1,
			};
			if (k.nbuf > 1) {
				/*
				 * evdi marks the whole screen dirty on a flip unless
				 * rects are already pending, and DisplayLinkManager
				 * then re-encodes all of it (100-300 ms per keystroke).
				 * Dirtying the off-screen back buffer first only queues
				 * the rects, so the flip carries just the changed rows.
				 */
				if (!force_full && drmModeDirtyFB(k.fd, k.fb_id[back], &clip, 1) &&
				    errno != ENOSYS && trace)
					log("trace dirtyfb back: %s", strerror(errno));
				double t_call = now();
				int flip_err = drmModePageFlip(k.fd, k.crtc_id, k.fb_id[back],
							       DRM_MODE_PAGE_FLIP_EVENT, &k);
				if (trace)
					log("trace pageflip ioctl %.1f ms (t=%.3f)", (now() - t_call) * 1e3, t_call);
				if (flip_err) {
					log("pageflip: %s", strerror(errno));
					if (errno == ENODEV || errno == ENOENT) { ret = 75; break; }
					retry = 1;
				} else {
					k.flip_pending = 1;
					t_flip = k.t_flip = now();
					if (trace > 1) {
						int r = kms_wait_flip(&k, 0.5);
						log("trace blocking wait: %s after %.1f ms", r ? "TIMEOUT" : "done",
						    (now() - t_flip) * 1e3);
					}
				}
			} else {
				if (drmModeDirtyFB(k.fd, k.fb_id[k.front], &clip, 1) && errno != ENOSYS) {
					log("dirtyfb: %s", strerror(errno));
					if (errno == ENODEV || errno == ENOENT) { ret = 75; break; }
				}
			}
			force_full = retry;
			pushed += !retry;
		}
		frames++;

		double t = now();
		if (t - t_stat >= 60) {
			log("%.1f fps grabbed, %.1f fps pushed, %lu skipped (flip pending)",
			    frames / (t - t_stat), pushed / (t - t_stat), skipped);
			frames = pushed = skipped = 0;
			t_stat = t;
		}
		t_next += period;
		if (t_next > t) {
			struct timespec ts = { 0, (long)((t_next - t) * 1e9) };
			nanosleep(&ts, NULL);
		} else {
			t_next = t; /* fell behind; don't try to catch up */
		}
	}

	kms_teardown(&k);
	if (wait && ret == 75 && !quit) {
		ret = 0;
		goto again;
	}
out:
	log("exiting");
	XShmDetach(dpy, &shm);
	shmdt(shm.shmaddr);
	XCloseDisplay(dpy);
	return ret;
}

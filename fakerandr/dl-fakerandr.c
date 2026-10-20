/*
 * dl-fakerandr.so -- LD_PRELOAD shim that adds virtual RandR outputs for
 * clients using libxcb-randr (KWin, Qt 5).
 *
 * The Tegra NVIDIA X driver has no PRIME and only exposes the internal panel
 * as a RandR CRTC, so the DisplayLink monitors are fed by dl-mirror from
 * regions of an enlarged X screen. `xrandr --setmonitor` describes those
 * regions to RandR 1.5 clients (GTK, Chromium), but KWin and Qt 5 enumerate
 * CRTCs/outputs instead and would treat the regions as off-screen. This shim
 * appends one fake CRTC + output + mode per region to the replies they read.
 *
 * Regions come from $DL_FAKE_RANDR_CONF (default
 * $XDG_RUNTIME_DIR/dl-fake-randr), one per line:
 *     NAME WIDTH HEIGHT X Y MM_WIDTH MM_HEIGHT
 * No file (or an empty one) means pass everything through untouched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#define MAX_FAKE 4
#define FAKE_CRTC   0x7fff0001u
#define FAKE_OUTPUT 0x7fff0101u
#define FAKE_MODE   0x7fff0201u

struct region {
	char name[32];
	int w, h, x, y, mmw, mmh;
};

static struct region regions[MAX_FAKE];
static int nregions;
static struct timespec conf_mtime;
static xcb_randr_crtc_t real_crtc;     /* stand-ins for requests about fakes */
static xcb_randr_output_t real_output;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Requests sent on behalf of a fake object, keyed by cookie sequence. */
enum { K_CRTC = 1, K_OUTPUT };
static struct pend { unsigned seq; xcb_connection_t *c; int kind, idx; } pending[64];

static void *real(const char *name)
{
	return dlsym(RTLD_NEXT, name);
}

static void load_conf(void)
{
	char path[512];
	/* KScreen would try to configure the fake outputs; let it see the truth. */
	if (!strncmp(program_invocation_short_name, "kscreen", 7)) { nregions = 0; return; }
	const char *p = getenv("DL_FAKE_RANDR_CONF");
	if (!p) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (!rt) { nregions = 0; return; }
		snprintf(path, sizeof(path), "%s/dl-fake-randr", rt);
		p = path;
	}
	struct stat st;
	if (stat(p, &st)) { nregions = 0; return; }
	if (st.st_mtim.tv_sec == conf_mtime.tv_sec && st.st_mtim.tv_nsec == conf_mtime.tv_nsec)
		return;
	conf_mtime = st.st_mtim;
	nregions = 0;
	FILE *f = fopen(p, "r");
	if (!f)
		return;
	char line[256];
	while (nregions < MAX_FAKE && fgets(line, sizeof(line), f)) {
		struct region *r = &regions[nregions];
		if (sscanf(line, "%31s %d %d %d %d %d %d", r->name, &r->w, &r->h, &r->x, &r->y,
			   &r->mmw, &r->mmh) == 7 && r->name[0] != '#' && r->w > 0 && r->h > 0)
			nregions++;
	}
	fclose(f);
}

static int fake_idx(uint32_t id, uint32_t base)
{
	int i = (int)(id - base);
	return id >= base && i < nregions ? i : -1;
}

static void remember(xcb_connection_t *c, unsigned seq, int kind, int idx)
{
	static unsigned next;
	pthread_mutex_lock(&lock);
	struct pend *p = &pending[next++ % 64];
	p->seq = seq; p->c = c; p->kind = kind; p->idx = idx;
	pthread_mutex_unlock(&lock);
}

static int recall(xcb_connection_t *c, unsigned seq, int kind)
{
	int idx = -1;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < 64; i++)
		if (pending[i].kind == kind && pending[i].seq == seq && pending[i].c == c) {
			idx = pending[i].idx;
			pending[i].kind = 0;
			break;
		}
	pthread_mutex_unlock(&lock);
	return idx;
}

/* ---- screen resources: append the fake crtcs, outputs and modes ---- */

/* Both resources replies share this layout: header, crtcs, outputs, modes, names. */
static void *patch_resources(void *reply)
{
	xcb_randr_get_screen_resources_reply_t *r = reply;
	if (!r)
		return NULL;
	pthread_mutex_lock(&lock);
	load_conf();
	int n = nregions;
	struct region reg[MAX_FAKE];
	memcpy(reg, regions, sizeof(reg));
	uint32_t *crtcs = (uint32_t *)(r + 1);
	uint32_t *outputs = crtcs + r->num_crtcs;
	if (r->num_crtcs)
		real_crtc = crtcs[0];
	if (r->num_outputs)
		real_output = outputs[0];
	pthread_mutex_unlock(&lock);
	if (!n)
		return r;

	xcb_randr_mode_info_t *modes = (xcb_randr_mode_info_t *)(outputs + r->num_outputs);
	uint8_t *names = (uint8_t *)(modes + r->num_modes);
	char mname[MAX_FAKE][16];
	size_t extra_names = 0;
	for (int i = 0; i < n; i++)
		extra_names += snprintf(mname[i], sizeof(mname[i]), "%dx%d", reg[i].w, reg[i].h);

	size_t names_len = r->names_len + extra_names;
	size_t size = sizeof(*r) + 4 * (r->num_crtcs + n) + 4 * (r->num_outputs + n) +
		      sizeof(*modes) * (r->num_modes + n) + ((names_len + 3) & ~3u);
	xcb_randr_get_screen_resources_reply_t *o = calloc(1, size);
	if (!o)
		return r;
	*o = *r;
	uint32_t *oc = (uint32_t *)(o + 1);
	memcpy(oc, crtcs, 4 * r->num_crtcs);
	uint32_t *oo = oc + r->num_crtcs + n;
	memcpy(oo, outputs, 4 * r->num_outputs);
	xcb_randr_mode_info_t *om = (xcb_randr_mode_info_t *)(oo + r->num_outputs + n);
	memcpy(om, modes, sizeof(*modes) * r->num_modes);
	uint8_t *on = (uint8_t *)(om + r->num_modes + n);
	memcpy(on, names, r->names_len);
	on += r->names_len;
	for (int i = 0; i < n; i++) {
		oc[r->num_crtcs + i] = FAKE_CRTC + i;
		oo[r->num_outputs + i] = FAKE_OUTPUT + i;
		xcb_randr_mode_info_t *m = &om[r->num_modes + i];
		m->id = FAKE_MODE + i;
		m->width = reg[i].w;
		m->height = reg[i].h;
		/* 60 Hz with a nominal 10%/4% blanking */
		m->htotal = reg[i].w + reg[i].w / 10;
		m->vtotal = reg[i].h + reg[i].h / 25;
		m->hsync_start = reg[i].w + 8;
		m->hsync_end = reg[i].w + 16;
		m->vsync_start = reg[i].h + 2;
		m->vsync_end = reg[i].h + 4;
		m->dot_clock = (uint32_t)m->htotal * m->vtotal * 60;
		m->name_len = strlen(mname[i]);
		memcpy(on, mname[i], m->name_len);
		on += m->name_len;
	}
	o->num_crtcs += n;
	o->num_outputs += n;
	o->num_modes += n;
	o->names_len = names_len;
	o->length = (size - 32) / 4;
	free(r);
	return o;
}

xcb_randr_get_screen_resources_reply_t *
xcb_randr_get_screen_resources_reply(xcb_connection_t *c,
				     xcb_randr_get_screen_resources_cookie_t cookie,
				     xcb_generic_error_t **e)
{
	static __typeof__(&xcb_randr_get_screen_resources_reply) fn;
	if (!fn) fn = real("xcb_randr_get_screen_resources_reply");
	return patch_resources(fn(c, cookie, e));
}

xcb_randr_get_screen_resources_current_reply_t *
xcb_randr_get_screen_resources_current_reply(xcb_connection_t *c,
					     xcb_randr_get_screen_resources_current_cookie_t cookie,
					     xcb_generic_error_t **e)
{
	static __typeof__(&xcb_randr_get_screen_resources_current_reply) fn;
	if (!fn) fn = real("xcb_randr_get_screen_resources_current_reply");
	return patch_resources(fn(c, cookie, e));
}

/* ---- crtc info ---- */

#define CRTC_INFO(sym)								\
xcb_randr_get_crtc_info_cookie_t						\
sym(xcb_connection_t *c, xcb_randr_crtc_t crtc, xcb_timestamp_t ts)		\
{										\
	static __typeof__(&sym) fn;						\
	if (!fn) fn = real(#sym);						\
	int i = fake_idx(crtc, FAKE_CRTC);					\
	if (i < 0)								\
		return fn(c, crtc, ts);						\
	xcb_randr_get_crtc_info_cookie_t ck = fn(c, real_crtc, ts);		\
	remember(c, ck.sequence, K_CRTC, i);					\
	return ck;								\
}
CRTC_INFO(xcb_randr_get_crtc_info)
CRTC_INFO(xcb_randr_get_crtc_info_unchecked)

xcb_randr_get_crtc_info_reply_t *
xcb_randr_get_crtc_info_reply(xcb_connection_t *c, xcb_randr_get_crtc_info_cookie_t cookie,
			      xcb_generic_error_t **e)
{
	static __typeof__(&xcb_randr_get_crtc_info_reply) fn;
	if (!fn) fn = real("xcb_randr_get_crtc_info_reply");
	xcb_randr_get_crtc_info_reply_t *r = fn(c, cookie, e);
	int i = recall(c, cookie.sequence, K_CRTC);
	if (i < 0 || !r)
		return r;
	/* header + outputs[1] + possible[1] */
	xcb_randr_get_crtc_info_reply_t *o = calloc(1, sizeof(*o) + 8);
	if (!o)
		return r;
	*o = *r;
	free(r);
	o->status = 0;
	o->x = regions[i].x;
	o->y = regions[i].y;
	o->width = regions[i].w;
	o->height = regions[i].h;
	o->mode = FAKE_MODE + i;
	o->rotation = o->rotations = XCB_RANDR_ROTATION_ROTATE_0;
	o->num_outputs = o->num_possible_outputs = 1;
	o->length = 2;
	uint32_t *t = (uint32_t *)(o + 1);
	t[0] = t[1] = FAKE_OUTPUT + i;
	return o;
}

/* ---- output info ---- */

#define OUTPUT_INFO(sym)							\
xcb_randr_get_output_info_cookie_t						\
sym(xcb_connection_t *c, xcb_randr_output_t output, xcb_timestamp_t ts)	\
{										\
	static __typeof__(&sym) fn;						\
	if (!fn) fn = real(#sym);						\
	int i = fake_idx(output, FAKE_OUTPUT);					\
	if (i < 0)								\
		return fn(c, output, ts);					\
	xcb_randr_get_output_info_cookie_t ck = fn(c, real_output, ts);	\
	remember(c, ck.sequence, K_OUTPUT, i);					\
	return ck;								\
}
OUTPUT_INFO(xcb_randr_get_output_info)
OUTPUT_INFO(xcb_randr_get_output_info_unchecked)

xcb_randr_get_output_info_reply_t *
xcb_randr_get_output_info_reply(xcb_connection_t *c, xcb_randr_get_output_info_cookie_t cookie,
				xcb_generic_error_t **e)
{
	static __typeof__(&xcb_randr_get_output_info_reply) fn;
	if (!fn) fn = real("xcb_randr_get_output_info_reply");
	xcb_randr_get_output_info_reply_t *r = fn(c, cookie, e);
	int i = recall(c, cookie.sequence, K_OUTPUT);
	if (i < 0 || !r)
		return r;
	/* header (36) + crtcs[1] + modes[1] + name */
	size_t nlen = strlen(regions[i].name);
	size_t size = 36 + 8 + ((nlen + 3) & ~3u);
	xcb_randr_get_output_info_reply_t *o = calloc(1, size + 4);
	if (!o)
		return r;
	memcpy(o, r, 36);
	free(r);
	o->status = 0;
	o->crtc = FAKE_CRTC + i;
	o->mm_width = regions[i].mmw;
	o->mm_height = regions[i].mmh;
	o->connection = XCB_RANDR_CONNECTION_CONNECTED;
	o->subpixel_order = 0;
	o->num_crtcs = o->num_modes = o->num_preferred = 1;
	o->num_clones = 0;
	o->name_len = nlen;
	o->length = (size - 32 + 3) / 4;
	uint32_t *t = (uint32_t *)((uint8_t *)o + 36);
	t[0] = FAKE_CRTC + i;
	t[1] = FAKE_MODE + i;
	memcpy(t + 2, regions[i].name, nlen);
	return o;
}

/* ---- properties and gamma: answer for a fake from the real objects ---- */

#define OUTPUT_PROP(sym)							\
xcb_randr_get_output_property_cookie_t						\
sym(xcb_connection_t *c, xcb_randr_output_t output, xcb_atom_t property,	\
    xcb_atom_t type, uint32_t off, uint32_t len, uint8_t del, uint8_t pend)	\
{										\
	static __typeof__(&sym) fn;						\
	if (!fn) fn = real(#sym);						\
	if (fake_idx(output, FAKE_OUTPUT) < 0)					\
		return fn(c, output, property, type, off, len, del, pend);	\
	/* a valid atom no output carries: the reply is "no such property" */	\
	return fn(c, real_output, XCB_ATOM_WM_NAME, type, off, len, 0, pend);	\
}
OUTPUT_PROP(xcb_randr_get_output_property)
OUTPUT_PROP(xcb_randr_get_output_property_unchecked)

#define CRTC_GAMMA(sym)								\
xcb_randr_get_crtc_gamma_cookie_t						\
sym(xcb_connection_t *c, xcb_randr_crtc_t crtc)				\
{										\
	static __typeof__(&sym) fn;						\
	if (!fn) fn = real(#sym);						\
	return fn(c, fake_idx(crtc, FAKE_CRTC) < 0 ? crtc : real_crtc);	\
}
CRTC_GAMMA(xcb_randr_get_crtc_gamma)
CRTC_GAMMA(xcb_randr_get_crtc_gamma_unchecked)

#define CRTC_GAMMA_SIZE(sym)							\
xcb_randr_get_crtc_gamma_size_cookie_t						\
sym(xcb_connection_t *c, xcb_randr_crtc_t crtc)				\
{										\
	static __typeof__(&sym) fn;						\
	if (!fn) fn = real(#sym);						\
	return fn(c, fake_idx(crtc, FAKE_CRTC) < 0 ? crtc : real_crtc);	\
}
CRTC_GAMMA_SIZE(xcb_randr_get_crtc_gamma_size)
CRTC_GAMMA_SIZE(xcb_randr_get_crtc_gamma_size_unchecked)

/* The dock monitors have no gamma of their own; the panel's ramp applies. */
#define SET_GAMMA(sym)								\
xcb_void_cookie_t								\
sym(xcb_connection_t *c, xcb_randr_crtc_t crtc, uint16_t size,			\
    const uint16_t *r, const uint16_t *g, const uint16_t *b)			\
{										\
	static __typeof__(&sym) fn;						\
	if (!fn) fn = real(#sym);						\
	return fn(c, fake_idx(crtc, FAKE_CRTC) < 0 ? crtc : real_crtc, size, r, g, b); \
}
SET_GAMMA(xcb_randr_set_crtc_gamma)
SET_GAMMA(xcb_randr_set_crtc_gamma_checked)

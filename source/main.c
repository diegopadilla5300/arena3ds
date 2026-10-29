/* Are.na for 3DS, after the iPhone app.
 *
 * Bottom screen: what you move through -- the feed, a grid of blocks, a
 * list of channels -- over a tab bar (Feed, Explore, Notifications, you).
 * Top screen: a breadcrumb, and a large look at whatever the cursor is on.
 * Black, hairlines, grey for the quiet parts; channels green when public,
 * red when private, as on the phone. */
#include <3ds.h>
#include <citro2d.h>
#include <curl/curl.h>
#include <dirent.h>
#include <strings.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "add.h"
#include "api.h"
#include "auth.h"
#include "http.h"
#include "img.h"
#include "logo_mask.h"
#include "qrcodegen.h"
#include "store.h"

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

#define C_BLACK  C2D_Color32(0x00, 0x00, 0x00, 0xFF)
#define C_WHITE  C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define C_GREY   C2D_Color32(0x8A, 0x8A, 0x8A, 0xFF)
#define C_DIM    C2D_Color32(0x55, 0x55, 0x55, 0xFF)
#define C_LINE   C2D_Color32(0x2A, 0x2A, 0x2A, 0xFF)
#define C_TILE   C2D_Color32(0x1B, 0x1B, 0x1B, 0xFF)
#define C_SKEL   C2D_Color32(0x16, 0x16, 0x16, 0xFF)
#define C_GREEN  C2D_Color32(0x8C, 0xC4, 0x7C, 0xFF)
#define C_GREEN_BG C2D_Color32(0x16, 0x22, 0x13, 0xFF)
#define C_GREEN_LN C2D_Color32(0x2A, 0x3D, 0x25, 0xFF)
#define C_RED    C2D_Color32(0xD0, 0x67, 0x5F, 0xFF)
#define C_RED_BG C2D_Color32(0x26, 0x12, 0x10, 0xFF)
#define C_RED_LN C2D_Color32(0x45, 0x22, 0x1E, 0xFF)

/* Button glyphs in the system font. */
#define GLYPH_A "\xEE\x80\x80"
#define GLYPH_B "\xEE\x80\x81"
#define GLYPH_X "\xEE\x80\x82"
#define GLYPH_Y "\xEE\x80\x83"
#define GLYPH_L "\xEE\x80\x84"
#define GLYPH_R "\xEE\x80\x85"

#define SC_BODY  0.50f   /* ~15px */
#define SC_SMALL 0.42f
#define SC_TINY  0.36f

static C2D_TextBuf g_tb;       /* drawn this frame */
static C2D_TextBuf g_mb;       /* measuring only */
static C3D_RenderTarget *g_top, *g_bot;
static float g_time;
static float g_z = 0.5f;

static void on_suspend(void);
static void on_resume(void);

/* HOME and the lid: the hardware this app holds open (camera, microphone,
 * the login page's socket) is let go before the system takes over, and
 * picked up again after. The hook runs on the main thread, inside
 * aptMainLoop, between frames. */
static void on_apt(APT_HookType hook, void *param)
{
	(void)param;
	switch (hook) {
	case APTHOOK_ONSUSPEND:
	case APTHOOK_ONSLEEP:
		on_suspend();
		break;
	case APTHOOK_ONRESTORE:
	case APTHOOK_ONWAKEUP:
		on_resume();
		break;
	case APTHOOK_ONEXIT:
		http_abort_all = true;
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------ drawing */

static float text_w(const char *s, float sc)
{
	C2D_Text t;
	if (C2D_TextBufGetNumGlyphs(g_mb) > 3500)
		C2D_TextBufClear(g_mb);
	C2D_TextParse(&t, g_mb, s);
	float w = 0, h = 0;
	C2D_TextGetDimensions(&t, sc, sc, &w, &h);
	return w;
}

/* y is the top of the line. Bold is a second pass a hair to the right:
 * the system font has no bold cut. */
static void text(const char *s, float x, float y, float sc, u32 col, bool bold)
{
	C2D_Text t;
	C2D_TextParse(&t, g_tb, s);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor, x, y, g_z, sc, sc, col);
	if (bold)
		C2D_DrawText(&t, C2D_WithColor, x + 0.5f, y, g_z, sc, sc, col);
}

static void text_right(const char *s, float right, float y, float sc, u32 col, bool bold)
{
	text(s, right - text_w(s, sc), y, sc, col, bold);
}

static void text_center(const char *s, float cx, float y, float sc, u32 col, bool bold)
{
	text(s, cx - text_w(s, sc) / 2, y, sc, col, bold);
}

static void text_wrap(const char *s, float x, float y, float sc, u32 col, float w)
{
	C2D_Text t;
	C2D_TextParse(&t, g_tb, s);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap, x, y, g_z, sc, sc, col, w);
}

static float wrap_h(const char *s, float sc, float w)
{
	C2D_Text t;
	if (C2D_TextBufGetNumGlyphs(g_mb) > 3000)
		C2D_TextBufClear(g_mb);
	C2D_TextParse(&t, g_mb, s);
	float tw = 0, th = 0;
	C2D_TextGetDimensions(&t, sc, sc, &tw, &th);
	if (tw <= w)
		return th;
	/* GetDimensions does not wrap; estimate from the width. */
	float lines = ceilf(tw / (w * 0.92f));
	return th * lines;
}

/* s cut to fit w, with an ellipsis, on a character boundary. */
static void fit(char *out, size_t cap, const char *s, float w, float sc)
{
	snprintf(out, cap, "%s", s);
	if (text_w(out, sc) <= w)
		return;
	size_t len = strlen(out);
	while (len > 0) {
		do
			len--;
		while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "%.*s\xE2\x80\xA6", (int)len, out);
		if (text_w(tmp, sc) <= w || len == 0) {
			snprintf(out, cap, "%s", tmp);
			return;
		}
	}
}

static void text_fit(const char *s, float x, float y, float w, float sc, u32 col, bool bold)
{
	char b[512];
	fit(b, sizeof(b), s, w, sc);
	text(b, x, y, sc, col, bold);
}

static void rect(float x, float y, float w, float h, u32 c)
{
	C2D_DrawRectSolid(x, y, g_z - 0.01f, w, h, c);
}

/* PICA textures are 8x8 tiles, Morton order inside each. */
static inline u32 swizzle_px(int x, int y, int tw)
{
	return ((((y >> 3) * (tw >> 3) + (x >> 3)) << 6) |
	        ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
	         ((x & 4) << 2) | ((y & 4) << 3)));
}

/* A filled circle as a fan of triangles. */
static void disc(float cx, float cy, float r, u32 c)
{
	int n = r < 6 ? 16 : r < 20 ? 28 : 48;
	float px = cx + r, py = cy;
	for (int i = 1; i <= n; i++) {
		float a = i * 2 * (float)M_PI / n;
		float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
		C2D_DrawTriangle(cx, cy, c, px, py, c, x, y, c, g_z - 0.01f);
		px = x;
		py = y;
	}
}

/* A circle's outline, as quads between two radii. */
static void ring(float cx, float cy, float r, float t, u32 c)
{
	const int n = 32;
	float ri = r - t;
	for (int i = 0; i < n; i++) {
		float a0 = i * 2 * (float)M_PI / n, a1 = (i + 1) * 2 * (float)M_PI / n;
		float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
		C2D_DrawTriangle(cx + c0 * r, cy + s0 * r, c, cx + c1 * r, cy + s1 * r, c,
		                 cx + c0 * ri, cy + s0 * ri, c, g_z);
		C2D_DrawTriangle(cx + c1 * r, cy + s1 * r, c, cx + c1 * ri, cy + s1 * ri, c,
		                 cx + c0 * ri, cy + s0 * ri, c, g_z);
	}
}

static void outline(float x, float y, float w, float h, float t, u32 c)
{
	rect(x, y, w, t, c);
	rect(x, y + h - t, w, t, c);
	rect(x, y, t, h, c);
	rect(x + w - t, y, t, h, c);
}

/* Scissor in screen coordinates. The framebuffers are rotated, so the box
 * is given to the GPU as (y, x) from the far corner. A clip inside an
 * outer one (a tile inside the scrolling list) stays within it, and
 * unclip() goes back to the outer one. */
static int   g_clip_w;
static float g_outer[4];      /* x0, y0, x1, y1; x1 == 0 for none */

static void scissor(int scr_w, float fx0, float fy0, float fx1, float fy1)
{
	C2D_Flush();
	int x0 = (int)fx0, y0 = (int)fy0, x1 = (int)fx1, y1 = (int)fy1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > scr_w) x1 = scr_w;
	if (y1 > SCR_H) y1 = SCR_H;
	if (x1 < x0) x1 = x0;
	if (y1 < y0) y1 = y0;
	C3D_SetScissor(GPU_SCISSOR_NORMAL, SCR_H - y1, scr_w - x1, SCR_H - y0, scr_w - x0);
}

static void clip(int scr_w, float x, float y, float w, float h)
{
	float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if (g_outer[2] > 0) {
		x0 = fmaxf(x0, g_outer[0]);
		y0 = fmaxf(y0, g_outer[1]);
		x1 = fminf(x1, g_outer[2]);
		y1 = fminf(y1, g_outer[3]);
	}
	g_clip_w = scr_w;
	scissor(scr_w, x0, y0, x1, y1);
}

static void unclip(void)
{
	if (g_outer[2] > 0) {
		scissor(g_clip_w, g_outer[0], g_outer[1], g_outer[2], g_outer[3]);
		return;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

static void clip_outer(int scr_w, float x, float y, float w, float h)
{
	g_outer[0] = x; g_outer[1] = y; g_outer[2] = x + w; g_outer[3] = y + h;
	g_clip_w = scr_w;
	scissor(scr_w, x, y, x + w, y + h);
}

static void unclip_outer(void)
{
	g_outer[2] = 0;
	unclip();
}

static void draw_image_fit(const C2D_Image *img, int iw, int ih, float x, float y, float w, float h)
{
	float s = fminf(w / iw, h / ih);
	float dw = iw * s, dh = ih * s;
	C2D_DrawImageAt(*img, x + (w - dw) / 2, y + (h - dh) / 2, g_z - 0.02f, NULL, s, s);
}

static void avatar(float cx, float cy, float r, const char *initials, u32 bg, u32 fg)
{
	disc(cx, cy, r, bg);
	float sc = r * 0.042f;
	text_center(initials, cx, cy - r * 0.42f, sc, fg, false);
}

static void ago(char *out, size_t n, time_t t)
{
	if (!t) {
		out[0] = '\0';
		return;
	}
	long d = (long)(http_now_utc() - t);
	if (d < 0) d = 0;
	long m = d / 60, h = m / 60, days = h / 24;
	if (m < 1) snprintf(out, n, "less than a minute ago");
	else if (m < 2) snprintf(out, n, "1 minute ago");
	else if (m < 45) snprintf(out, n, "%ld minutes ago", m);
	else if (m < 90) snprintf(out, n, "about 1 hour ago");
	else if (h < 24) snprintf(out, n, "about %ld hours ago", (m + 30) / 60);
	else if (h < 42) snprintf(out, n, "1 day ago");
	else if (days < 30) snprintf(out, n, "%ld days ago", (h + 12) / 24);
	else if (days < 45) snprintf(out, n, "about 1 month ago");
	else if (days < 365) snprintf(out, n, "%ld months ago", (days + 15) / 30);
	else if (days < 365 + 91) snprintf(out, n, "about 1 year ago");
	else if (days < 365 + 274) snprintf(out, n, "over 1 year ago");
	else {
		long y = days / 365, rem = days % 365;
		snprintf(out, n, "%s %ld years ago", rem < 91 ? "about" : rem < 274 ? "over" : "almost",
		         rem < 274 ? y : y + 1);
	}
}

static void month_year(char *out, size_t n, time_t t)
{
	static const char *MON[] = {"January", "February", "March", "April", "May", "June", "July",
	                            "August", "September", "October", "November", "December"};
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(out, n, "%s %d", MON[tm.tm_mon], tm.tm_year + 1900);
}

static u32 vis_text(visibility v)
{
	return v == VIS_PUBLIC ? C_GREEN : v == VIS_PRIVATE ? C_RED : C_WHITE;
}
static u32 vis_bg(visibility v)
{
	return v == VIS_PUBLIC ? C_GREEN_BG : v == VIS_PRIVATE ? C_RED_BG : C_TILE;
}
static u32 vis_line(visibility v)
{
	return v == VIS_PUBLIC ? C_GREEN_LN : v == VIS_PRIVATE ? C_RED_LN : C_LINE;
}

/* ------------------------------------------------------------ rich text */

/* A sentence of differently styled runs, word-wrapped -- "dani bloop
 * followed Elissa Vinh" with the names bold. Returns its height. */
typedef struct {
	const char *s;
	u32         col;
	bool        bold;
} run_t;

static float rich(const run_t *runs, int nr, float x, float y, float w, float sc,
                  int max_lines, bool draw)
{
	const float lh = 30.0f * sc * 1.12f;
	float cx = 0;
	int line = 0;
	const float space = text_w(" ", sc) + 1.0f;
	/* A space owed to the next word, even when it ends one run and the
	 * word starts the next ("dani bloop" + " " + "followed"). */
	bool gap = false;
	for (int r = 0; r < nr; r++) {
		const char *p = runs[r].s;
		while (*p) {
			if (*p == ' ')
				gap = true;
			while (*p == ' ')
				p++;
			if (!*p)
				break;
			if (gap && cx > 0)
				cx += space;
			gap = false;
			size_t wl = strcspn(p, " ");
			char word[160];
			snprintf(word, sizeof(word), "%.*s", (int)(wl < 159 ? wl : 159), p);
			float ww = text_w(word, sc);
			if (cx > 0 && cx + ww > w) {
				line++;
				cx = 0;
			}
			if (line >= max_lines)
				return max_lines * lh;
			if (draw) {
				if (line == max_lines - 1 && ww > w - cx) {
					text_fit(word, x + cx, y + line * lh, w - cx, sc, runs[r].col, runs[r].bold);
				} else {
					text(word, x + cx, y + line * lh, sc, runs[r].col, runs[r].bold);
				}
			}
			cx += ww;
			p += wl;
		}
	}
	return (line + 1) * lh;
}

static int activity_runs(const entry *e, run_t *r)
{
	int n = 0;
	r[n++] = (run_t){e->owner, C_WHITE, true};
	r[n++] = (run_t){" ", C_GREY, false};
	r[n++] = (run_t){e->verb, C_GREY, false};
	if (e->item.kind != E_HEADER) {
		r[n++] = (run_t){" ", C_GREY, false};
		r[n++] = (run_t){e->item.title, e->item.kind == E_CHANNEL ? vis_text(e->item.vis) : C_WHITE, true};
	}
	if (e->target.kind != E_HEADER) {
		r[n++] = (run_t){" to ", C_GREY, false};
		r[n++] = (run_t){e->target.title, vis_text(e->target.vis), true};
	}
	return n;
}

/* ------------------------------------------------------------ screens */

typedef enum { SC_FEED, SC_NOTIF, SC_EXPLORE, SC_CHANNEL, SC_USER, SC_BLOCK, SC_LOGIN } sc_type;

typedef struct {
	sc_type type;
	view   *v, *v2;         /* SC_USER: channels, blocks */
	int     sub;
	char    parent[96];     /* breadcrumb before the title */
	char    title[160];
	char    slug[80];
	int     id;
	entry   pre;            /* what was known when it was opened */
	bool    has_pre;
	bool    me;             /* your own profile */
} screen;

enum { T_FEED, T_EXPLORE, T_NOTIF, T_PROFILE, TABS };
#define DEPTH 12

static screen g_stack[TABS][DEPTH];
static int    g_depth[TABS];
static int    g_tab = T_EXPLORE;

static screen *cur(void) { return &g_stack[g_tab][g_depth[g_tab] - 1]; }

static view *cur_view(screen *s)
{
	return s->type == SC_USER && s->sub == 1 ? s->v2 : s->v;
}

static bool logged_in(void)
{
	return api_token[0] && !store_account.rejected;
}

static void screen_free(screen *s)
{
	view_free(s->v);
	view_free(s->v2);
	free(s->pre.body);
	free(s->pre.md);
	memset(s, 0, sizeof(*s));
}

static void tab_clear(int t)
{
	for (int i = 0; i < g_depth[t]; i++)
		screen_free(&g_stack[t][i]);
	g_depth[t] = 0;
}

static screen *push(int t)
{
	if (g_depth[t] >= DEPTH) {
		/* Deep enough: forget the oldest step past the root. */
		screen_free(&g_stack[t][1]);
		memmove(&g_stack[t][1], &g_stack[t][2], sizeof(screen) * (DEPTH - 2));
		g_depth[t]--;
	}
	screen *s = &g_stack[t][g_depth[t]++];
	memset(s, 0, sizeof(*s));
	return s;
}

static void pop(void)
{
	if (g_depth[g_tab] <= 1)
		return;
	screen_free(cur());
	g_depth[g_tab]--;
}

static void open_user(int t, const char *slug, const char *name, bool me)
{
	screen *s = push(t);
	s->type = SC_USER;
	s->me = me;
	snprintf(s->slug, sizeof(s->slug), "%s", slug);
	snprintf(s->title, sizeof(s->title), "%s", name && name[0] ? name : slug);
	s->v = view_new(L_USER_CHANNELS, slug, 0, true);
	s->v2 = view_new(L_USER_BLOCKS, slug, 0, false);
}

static void open_channel(int t, const char *slug, const char *title, const char *owner)
{
	screen *s = push(t);
	s->type = SC_CHANNEL;
	snprintf(s->slug, sizeof(s->slug), "%s", slug);
	snprintf(s->title, sizeof(s->title), "%s", title);
	snprintf(s->parent, sizeof(s->parent), "%s", owner ? owner : "");
	s->v = view_new(L_CHANNEL, slug, 0, true);
}

static void open_block(int t, const entry *pre, const char *parent)
{
	screen *s = push(t);
	s->type = SC_BLOCK;
	s->id = pre->id;
	snprintf(s->title, sizeof(s->title), "%s", pre->title);
	snprintf(s->parent, sizeof(s->parent), "%s", parent ? parent : "");
	s->pre = *pre;
	s->pre.body = pre->body ? strdup(pre->body) : NULL;
	s->pre.md = pre->md ? strdup(pre->md) : NULL;
	s->has_pre = true;
	s->v = view_new(L_BLOCK, NULL, pre->id, false);
}

static void open_ref(const ref_t *r, const entry *from, const char *parent)
{
	if (r->kind == E_BLOCK) {
		entry pre = {0};
		pre.kind = E_BLOCK;
		pre.btype = r->btype;
		pre.id = r->id;
		snprintf(pre.title, sizeof(pre.title), "%s", r->title);
		if (from) {
			snprintf(pre.thumb, sizeof(pre.thumb), "%s", from->thumb);
			snprintf(pre.big, sizeof(pre.big), "%s", from->big);
			pre.body = from->body;
			pre.md = NULL;
		}
		open_block(g_tab, &pre, parent);
	} else if (r->kind == E_CHANNEL) {
		open_channel(g_tab, r->slug, r->title, NULL);
	} else if (r->kind == E_USER) {
		open_user(g_tab, r->slug, r->title, false);
	}
}

static void open_entry(const entry *e, const char *parent)
{
	switch (e->kind) {
	case E_BLOCK:
		open_block(g_tab, e, parent);
		break;
	case E_CHANNEL:
		open_channel(g_tab, e->slug, e->title, e->owner);
		break;
	case E_USER:
		open_user(g_tab, e->slug, e->title, false);
		break;
	case E_ACTIVITY:
		open_ref(&e->item, e, NULL);
		break;
	default:
		break;
	}
}

/* The root of each tab follows the account: signed out, Feed and
 * Notifications ask you to log in and You is the login page. */
static void build_roots(void)
{
	static bool built;
	static bool was_in;
	static char was_slug[80];
	bool in = logged_in();
	const char *slug = store_account.known ? store_account.me.slug : "";
	if (built && in == was_in && !strcmp(slug, was_slug))
		return;
	bool arrived = in && (!built || !was_in);
	built = true;
	was_in = in;
	snprintf(was_slug, sizeof(was_slug), "%s", slug);

	/* Signed out there is nothing but the login page: no tabs, no
	 * browsing. Signing in builds every tab and lands on Explore. */
	if (!in) {
		for (int t = 0; t < TABS; t++)
			tab_clear(t);
		screen *p = push(T_PROFILE);
		p->type = SC_LOGIN;
		g_tab = T_PROFILE;
		return;
	}
	if (arrived) {
		for (int t = 0; t < TABS; t++)
			tab_clear(t);
		screen *e = push(T_EXPLORE);
		e->type = SC_EXPLORE;
		e->v = view_new(L_EXPLORE, NULL, 0, false);
		screen *f = push(T_FEED);
		f->type = SC_FEED;
		f->v = view_new(L_FEED, NULL, 0, false);
		screen *nt = push(T_NOTIF);
		nt->type = SC_NOTIF;
		nt->v = view_new(L_NOTIFICATIONS, NULL, 0, false);
		g_tab = T_EXPLORE;
	}
	/* The profile follows the account as it becomes known. */
	tab_clear(T_PROFILE);
	if (slug[0]) {
		open_user(T_PROFILE, slug, store_account.me.name, true);
	} else {
		screen *p = push(T_PROFILE);
		p->type = SC_USER;
		p->me = true;
	}
}

/* ------------------------------------------------------------ layout */

#define TABBAR_H 30.0f
#define CONTENT_H (SCR_H - TABBAR_H)
#define PAD      10.0f

/* Explore has three layouts, as on the phone: one card to a row, two, or
 * three. Other grids are four small squares to a row. */
typedef enum { EX_FEED, EX_SPACIOUS, EX_DENSE, EX_MODES } explore_mode;
static const char *const EX_NAME[EX_MODES] = {"Feed", "Spacious", "Dense"};
static explore_mode g_explore = EX_SPACIOUS;

/* Notifications, as on the phone: Compact rows, or a Feed with the
 * thing itself shown large under each sentence. */
static bool g_notif_feed;
#define NOTIF_BOX_H 150.0f

typedef struct {
	int   cols;
	float w, h, gap, margin;
	float cap;      /* room under a tile for a block's title */
} grid_t;

static grid_t grid_of(const view *v)
{
	grid_t g = {4, 72.0f, 72.0f, 6.0f, 0};
	if (v && v->req.kind == L_EXPLORE) {
		switch (g_explore) {
		/* Still a square, one to a row; the next one peeks in below. */
		case EX_FEED:     g = (grid_t){1, 168.0f, 168.0f, 10.0f, 0}; break;
		case EX_SPACIOUS: g = (grid_t){2, 146.0f, 146.0f, 8.0f, 0}; break;
		default:          g = (grid_t){3, 96.0f, 96.0f, 6.0f, 0}; break;
		}
	}
	g.margin = (BOT_W - g.cols * g.w - (g.cols - 1) * g.gap) / 2;
	g.cap = g.w >= 140 ? 17.0f : 14.0f;
	return g;
}

/* The phone's "Connect ->" and "..." buttons, right-aligned at y. */
#define BTN_H     22.0f
#define CHAN_BTN_Y 60.0f   /* below a channel's four detail lines */
#define BTN_MORE_W 30.0f
#define BTN_CONN_W 84.0f
#define BTN_X_MORE (BOT_W - PAD - BTN_MORE_W)
#define BTN_X_CONN (BTN_X_MORE - 6 - BTN_CONN_W)

static void draw_buttons(float y)
{
	rect(BTN_X_CONN, y, BTN_CONN_W, BTN_H, C_TILE);
	text_center("Connect \xE2\x86\x92", BTN_X_CONN + BTN_CONN_W / 2, y + 4, SC_SMALL, C_WHITE, true);
	rect(BTN_X_MORE, y, BTN_MORE_W, BTN_H, C_TILE);
	for (int k = -1; k <= 1; k++)
		rect(BTN_X_MORE + BTN_MORE_W / 2 + k * 5 - 1, y + BTN_H / 2 - 1, 2, 2, C_WHITE);
}

/* A channel's two: Connect, and Edit when you may edit it. */
#define BTN_EDIT_W 50.0f
#define BTN_X_EDIT (BOT_W - PAD - BTN_EDIT_W)

static float chan_conn_x(bool edit)
{
	return edit ? BTN_X_EDIT - 6 - BTN_CONN_W : BOT_W - PAD - BTN_CONN_W;
}

static void draw_channel_buttons(float y, bool edit)
{
	float cx = chan_conn_x(edit);
	rect(cx, y, BTN_CONN_W, BTN_H, C_TILE);
	text_center("Connect \xE2\x86\x92", cx + BTN_CONN_W / 2, y + 4, SC_SMALL, C_WHITE, true);
	if (edit) {
		rect(BTN_X_EDIT, y, BTN_EDIT_W, BTN_H, C_TILE);
		text_center("Edit", BTN_X_EDIT + BTN_EDIT_W / 2, y + 4, SC_SMALL, C_WHITE, true);
	}
}

/* 1 for Connect, 2 for Edit, 0 for neither. */
static int channel_button_at(float x, float y, float by, bool edit)
{
	if (y < by || y > by + BTN_H)
		return 0;
	float cx = chan_conn_x(edit);
	if (x >= cx && x < cx + BTN_CONN_W)
		return 1;
	if (edit && x >= BTN_X_EDIT && x < BTN_X_EDIT + BTN_EDIT_W)
		return 2;
	return 0;
}

/* 1 for Connect, 2 for More, 0 for neither. */
static int button_at(float x, float y, float by)
{
	if (y < by || y > by + BTN_H)
		return 0;
	if (x >= BTN_X_CONN && x < BTN_X_CONN + BTN_CONN_W)
		return 1;
	if (x >= BTN_X_MORE && x < BTN_X_MORE + BTN_MORE_W)
		return 2;
	return 0;
}

static bool is_grid(const view *v)
{
	return v && (v->req.kind == L_EXPLORE || v->req.kind == L_CHANNEL || v->req.kind == L_USER_BLOCKS);
}

/* Height of what sits above the items. */
static float header_h(const screen *s)
{
	switch (s->type) {
	case SC_CHANNEL: return CHAN_BTN_Y + BTN_H + 8;
	case SC_USER:    return 66.0f;
	default:         return 0.0f;
	}
}

static bool selectable(const view *v, int i)
{
	const entry *e = &v->e[i];
	if (v->req.kind == L_BLOCK)
		return e->kind == E_CHANNEL;
	return e->kind == E_BLOCK || e->kind == E_CHANNEL || e->kind == E_USER || e->kind == E_ACTIVITY;
}

/* What a Feed-style notification shows large: a block's picture or
 * words, or a person. */
static bool notif_has_box(const entry *e)
{
	return e->thumb[0] || (e->item.kind == E_BLOCK && e->item.btype == B_TEXT && e->body) ||
	       e->item.kind == E_USER;
}

static float measure_row(const view *v, int i)
{
	const entry *e = &v->e[i];
	const float w = BOT_W - 2 * PAD;
	switch (e->kind) {
	case E_ACTIVITY: {
		run_t r[8];
		int nr = activity_runs(e, r);
		bool notif = v->req.kind == L_NOTIFICATIONS;
		if (notif && g_notif_feed) {
			float h = rich(r, nr, 0, 0, w, SC_SMALL, 4, false) + 18;
			if (notif_has_box(e))
				h += NOTIF_BOX_H + 26;
			return h + 18;
		}
		float tw = w - (e->thumb[0] ? 52 : 0) - (notif ? 38 : 0);
		float h = rich(r, nr, 0, 0, tw, SC_SMALL, 4, false) + 18;
		return fmaxf(h, e->thumb[0] ? 54 : 0) + 14;
	}
	case E_CHANNEL:
		return 50.0f + 6.0f;
	case E_BLOCK:
		if (v->req.kind == L_BLOCK && i == 0)
			return fminf(wrap_h(e->title, 0.56f, w), 52.0f) + 26.0f + BTN_H + 8;
		return 30.0f;
	case E_USER:
		return 34.0f;
	case E_SECTION:
		return 18.0f + fminf(wrap_h(e->body ? e->body : "", SC_SMALL, w), 90.0f) + 10.0f;
	case E_KV:
		return 20.0f;
	case E_HEADER:
		return 30.0f;
	}
	return 20.0f;
}

/* Lay rows out as they arrive; the grid needs none. */
static void measure(view *v)
{
	if (is_grid(v) || v->rh_n >= v->n)
		return;
	float *rh = realloc(v->rh, sizeof(float) * v->n);
	if (!rh)
		return;
	v->rh = rh;
	for (int i = v->rh_n; i < v->n; i++)
		v->rh[i] = measure_row(v, i);
	v->rh_n = v->n;
}

static float item_y(const screen *s, const view *v, int i)
{
	float y = header_h(s);
	if (is_grid(v))
	{
		grid_t g = grid_of(v);
		return y + g.gap + (i / g.cols) * (g.h + g.cap + g.gap);
	}
	for (int k = 0; k < i && k < v->rh_n; k++)
		y += v->rh[k];
	return y;
}

static float item_h(const view *v, int i)
{
	if (is_grid(v))
	{
		grid_t g = grid_of(v);
		return g.h + g.cap;
	}
	return i < v->rh_n ? v->rh[i] : 20.0f;
}

static float content_h(const screen *s, const view *v)
{
	if (!v || v->n == 0)
		return header_h(s);
	if (is_grid(v))
	{
		grid_t g = grid_of(v);
		return header_h(s) + g.gap + ((v->n + g.cols - 1) / g.cols) * (g.h + g.cap + g.gap) + 24;
	}
	return item_y(s, v, v->n) + 24;
}

static void keep_visible(screen *s, view *v)
{
	if (!v || v->sel < 0 || v->sel >= v->n)
		return;
	float y = item_y(s, v, v->sel), h = item_h(v, v->sel);
	grid_t g = grid_of(v);
	float top = y - (is_grid(v) ? g.gap : 0) - 4;
	if (v->sel < (is_grid(v) ? g.cols : 1))
		top = 0;   /* the header comes back into view at the top */
	if (top < v->scroll_to)
		v->scroll_to = top;
	if (y + h + 8 > v->scroll_to + CONTENT_H)
		v->scroll_to = y + h + 8 - CONTENT_H;
	float max = content_h(s, v) - CONTENT_H;
	if (v->scroll_to > max) v->scroll_to = max;
	if (v->scroll_to < 0) v->scroll_to = 0;
}

static int next_selectable(const view *v, int from, int dir)
{
	for (int i = from + dir; i >= 0 && i < v->n; i += dir)
		if (selectable(v, i))
			return i;
	return -1;
}

static int item_at(const screen *s, const view *v, float x, float y)
{
	if (!v)
		return -1;
	if (is_grid(v)) {
		grid_t g = grid_of(v);
		float yy = y - header_h(s) - g.gap;
		if (yy < 0 || x < g.margin)
			return -1;
		int row = (int)(yy / (g.h + g.cap + g.gap)), col = (int)((x - g.margin) / (g.w + g.gap));
		if (col < 0 || col >= g.cols || yy - row * (g.h + g.cap + g.gap) > g.h + g.cap)
			return -1;
		int i = row * g.cols + col;
		return i < v->n ? i : -1;
	}
	float yy = header_h(s);
	for (int i = 0; i < v->rh_n; i++) {
		if (y >= yy && y < yy + v->rh[i])
			return selectable(v, i) ? i : -1;
		yy += v->rh[i];
	}
	return -1;
}

/* ------------------------------------------------------------ bottom */

static void draw_skeleton_rows(float y0, int rows)
{
	for (int r = 0; r < rows; r++) {
		float y = y0 + r * 56;
		rect(PAD, y + 10, 200, 10, C_SKEL);
		rect(PAD, y + 26, 120, 8, C_SKEL);
		rect(BOT_W - PAD - 44, y + 8, 44, 44, C_SKEL);
	}
}

static void draw_skeleton_grid(const view *v, float y0)
{
	grid_t g = grid_of(v);
	for (int i = 0; i < g.cols * 3; i++)
		rect(g.margin + (i % g.cols) * (g.w + g.gap), y0 + g.gap + (i / g.cols) * (g.h + g.cap + g.gap),
		     g.w, g.h, C_SKEL);
}

static void draw_tile(const entry *e, float x, float y, float w, float h, float cap, bool sel)
{
	if (y > CONTENT_H || y + h + cap < 0)
		return;
	/* Blocks carry their title or filename underneath, as on the phone;
	 * channels say who and how many inside the tile. */
	if (e->kind != E_CHANNEL && cap > 0) {
		const float sc = cap >= 17 ? SC_TINY : 0.3f;
		char t[200];
		fit(t, sizeof(t), e->title, w - 4, sc);
		text_center(t, x + w / 2, y + h + 3, sc, C_GREY, false);
	}
	if (e->kind == E_CHANNEL) {
		/* Bigger tiles say more: title, then who, then size and age. */
		const bool big = w >= 140, mid = w >= 90;
		const bool huge = w >= 180;
		const float sc = huge ? 0.62f : big ? 0.5f : mid ? SC_SMALL : SC_TINY;
		const float meta = huge ? SC_SMALL : big ? SC_TINY + 0.02f : mid ? SC_TINY : 0.3f;
		const float lh = 30.0f * meta * 1.15f;
		rect(x, y, w, h, vis_bg(e->vis));
		outline(x, y, w, h, 1, vis_line(e->vis));
		clip(BOT_W, x + 3, y + 3, w - 6, h - 6);
		char by[96], n[24], a[48], line[96];
		snprintf(by, sizeof(by), "by %s", e->owner);
		snprintf(n, sizeof(n), "%d blocks", e->count);
		ago(a, sizeof(a), e->when);
		int lines = 1 + (mid && e->owner[0]) + big;
		float th = fminf(wrap_h(e->title, sc, w - 16), h * 0.5f);
		float ty = y + (h - th - lines * lh - 2) / 2;
		C2D_Text t;
		C2D_TextParse(&t, g_tb, e->title);
		C2D_TextOptimize(&t);
		C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap | C2D_AlignCenter, x + w / 2, ty, g_z,
		             sc, sc, vis_text(e->vis), w - 16);
		float my = ty + th + 4;
		if (mid && e->owner[0]) {
			fit(line, sizeof(line), by, w - 16, meta);
			text_center(line, x + w / 2, my, meta, C_GREY, false);
			my += lh;
		}
		text_center(n, x + w / 2, my, meta, C_GREY, false);
		my += lh;
		if (big) {
			fit(line, sizeof(line), a, w - 16, meta);
			text_center(line, x + w / 2, my, meta, C_GREY, false);
		}
		unclip();
	} else {
		int iw, ih;
		const C2D_Image *img = e->thumb[0] ? img_get(e->thumb, &iw, &ih) : NULL;
		if (img) {
			draw_image_fit(img, iw, ih, x, y, w, h);
		} else if (e->thumb[0] && !img_failed(e->thumb)) {
			rect(x, y, w, h, C_SKEL);
		} else {
			/* Text, or a link with no picture: the words themselves. */
			rect(x, y, w, h, C_BLACK);
			outline(x, y, w, h, 1, C_LINE);
			clip(BOT_W, x + 4, y + 4, w - 8, h - 8);
			const char *str = e->btype == B_TEXT && e->body ? e->body : e->title;
			text_wrap(str, x + 5, y + 4, 0.3f, e->btype == B_TEXT ? C_WHITE : C_GREY, w - 10);
			unclip();
		}
	}
	if (sel)
		outline(x - 3, y - 3, w + 6, h + 6, 2, C_WHITE);
}

static void draw_activity(const view *v, const entry *e, float y, float h, bool sel)
{
	bool notif = v->req.kind == L_NOTIFICATIONS;
	if (notif && g_notif_feed) {
		/* Feed: the sentence across the full width, then what it is
		 * about in a bordered box with its title underneath. */
		const float w = BOT_W - 2 * PAD;
		run_t r[8];
		int nr = activity_runs(e, r);
		float th = rich(r, nr, PAD, y + 10, w, SC_SMALL, 4, true);
		char a[48];
		ago(a, sizeof(a), e->when);
		text(a, PAD, y + 12 + th, SC_TINY, C_GREY, false);
		if (e->unread)
			disc(BOT_W - 4, y + 16, 2.5f, C_WHITE);
		if (notif_has_box(e)) {
			float by = y + 12 + th + 18;
			outline(PAD, by, w, NOTIF_BOX_H, 1, C_LINE);
			int iw, ih;
			const C2D_Image *img = e->big[0] ? img_get(e->big, &iw, &ih) : NULL;
			if (!img && e->thumb[0])
				img = img_get(e->thumb, &iw, &ih);
			if (img) {
				draw_image_fit(img, iw, ih, PAD + 1, by + 1, w - 2, NOTIF_BOX_H - 2);
			} else if (e->item.btype == B_TEXT && e->body) {
				clip(BOT_W, PAD + 8, by + 6, w - 16, NOTIF_BOX_H - 12);
				text_wrap(e->body, PAD + 10, by + 8, SC_SMALL, C_WHITE, w - 20);
				unclip();
			} else if (e->item.kind == E_USER) {
				avatar(BOT_W / 2.0f, by + NOTIF_BOX_H / 2, 34, e->item.initials, C_TILE, C_WHITE);
			} else {
				rect(PAD + 1, by + 1, w - 2, NOTIF_BOX_H - 2, C_SKEL);
			}
			char t[200];
			fit(t, sizeof(t), e->item.title, w, SC_TINY + 0.02f);
			text_center(t, BOT_W / 2.0f, by + NOTIF_BOX_H + 6, SC_TINY + 0.02f, C_GREY, false);
		}
		if (sel)
			outline(2, y + 2, BOT_W - 4, h - 3, 2, C_WHITE);
		return;
	}
	float x = PAD + (notif ? 38 : 0);
	float w = BOT_W - 2 * PAD - (e->thumb[0] ? 52 : 0) - (notif ? 38 : 0);
	if (notif)
		avatar(PAD + 15, y + 22, 15, e->initials, C_TILE, C_WHITE);
	run_t r[8];
	int nr = activity_runs(e, r);
	float th = rich(r, nr, x, y + 8, w, SC_SMALL, 4, true);
	char a[48];
	ago(a, sizeof(a), e->when);
	text(a, x, y + 10 + th, SC_TINY, C_GREY, false);
	if (e->unread)
		disc(BOT_W - 4, y + 14, 2.5f, C_WHITE);
	if (e->thumb[0]) {
		float tx = BOT_W - PAD - 46, ty = y + 8;
		int iw, ih;
		const C2D_Image *img = img_get(e->thumb, &iw, &ih);
		if (img)
			draw_image_fit(img, iw, ih, tx, ty, 46, 46);
		else
			rect(tx, ty, 46, 46, C_SKEL);
	}
	rect(PAD, y + h - 1, BOT_W - 2 * PAD, 1, C_LINE);
	if (sel)
		outline(2, y + 2, BOT_W - 4, h - 3, 2, C_WHITE);
}

static void draw_channel_row(const entry *e, float y, bool sel)
{
	float x = PAD, w = BOT_W - 2 * PAD, h = 50;
	rect(x, y, w, h, vis_bg(e->vis));
	outline(x, y, w, h, 1, vis_line(e->vis));
	u32 tc = vis_text(e->vis);
	float ow = fminf(text_w(e->owner, SC_SMALL), 110);
	text_fit(e->title, x + 8, y + 7, w - ow - 26, SC_SMALL + 0.04f, tc, false);
	text_fit(e->owner, x + w - 8 - ow, y + 8, ow, SC_SMALL, tc, false);
	char n[24], a[48];
	snprintf(n, sizeof(n), "%d blocks", e->count);
	ago(a, sizeof(a), e->when);
	text(n, x + 8, y + 30, SC_TINY, tc, false);
	text_right(a, x + w - 8, y + 30, SC_TINY, tc, false);
	if (sel)
		outline(x - 3, y - 3, w + 6, h + 6, 2, C_WHITE);
}

static void draw_list_row(const view *v, int i, float y, bool sel)
{
	const entry *e = &v->e[i];
	float h = v->rh[i];
	if (y > CONTENT_H || y + h < 0)
		return;
	const float w = BOT_W - 2 * PAD;
	switch (e->kind) {
	case E_ACTIVITY:
		draw_activity(v, e, y, h, sel);
		break;
	case E_CHANNEL:
		draw_channel_row(e, y + 3, sel);
		break;
	case E_BLOCK:
		if (v->req.kind == L_BLOCK && i == 0) {
			clip(BOT_W, 0, y, BOT_W, h - BTN_H - 12);
			text_wrap(e->title, PAD, y + 8, 0.56f, C_WHITE, w);
			unclip();
			draw_buttons(y + h - BTN_H - 10);
			rect(PAD, y + h - 1, w, 1, C_LINE);
		} else {
			text_fit(e->title, PAD, y + 7, w, SC_SMALL, C_WHITE, false);
			if (sel)
				outline(2, y + 1, BOT_W - 4, h - 2, 2, C_WHITE);
		}
		break;
	case E_USER:
		avatar(PAD + 12, y + 17, 12, e->initials, C_TILE, C_WHITE);
		text_fit(e->title, PAD + 32, y + 9, w - 32, SC_SMALL, C_WHITE, true);
		if (sel)
			outline(2, y + 1, BOT_W - 4, h - 2, 2, C_WHITE);
		break;
	case E_SECTION:
		text(e->title, PAD, y + 4, SC_SMALL, C_WHITE, true);
		clip(BOT_W, 0, y + 20, BOT_W, h - 26);
		text_wrap(e->body ? e->body : "", PAD, y + 21, SC_SMALL, C_GREY, w);
		unclip();
		break;
	case E_KV:
		text(e->title, PAD, y + 3, SC_SMALL, C_GREY, false);
		text_fit(e->body ? e->body : "", PAD + 90, y + 3, w - 90, SC_SMALL, C_WHITE, true);
		rect(PAD, y + h - 1, w, 1, C_LINE);
		break;
	case E_HEADER: {
		int n = 0;
		for (int k = i + 1; k < v->n; k++)
			n += v->e[k].kind == E_CHANNEL;
		text(e->title, PAD, y + 10, SC_SMALL, C_WHITE, true);
		char c[16];
		snprintf(c, sizeof(c), "%d%s", n, v->req.more ? "+" : "");
		float cx = PAD + text_w(e->title, SC_SMALL) + 8;
		rect(cx, y + 9, text_w(c, SC_TINY) + 8, 15, C_TILE);
		text(c, cx + 4, y + 11, SC_TINY, C_GREY, false);
		break;
	}
	}
}

static void draw_channel_header(const view *v, float y)
{
	const detail_t *d = &v->det;
	if (!d->ok) {
		text(v->detail_loading || v->want_detail ? "Loading..." : "", PAD, y + 10, SC_SMALL, C_GREY, false);
		return;
	}
	char a[48], b[48], n[16], f[16];
	ago(a, sizeof(a), d->created);
	ago(b, sizeof(b), d->updated);
	snprintf(n, sizeof(n), "%d", d->length);
	snprintf(f, sizeof(f), "%d", d->followers);
	const char *L[] = {"Started", "Modified", "Length", "Followers"};
	const char *V[] = {a, b, n, f};
	for (int i = 0; i < 4; i++) {
		text(L[i], PAD, y + 6 + i * 13, SC_TINY + 0.02f, C_GREY, false);
		text(V[i], PAD + 70, y + 6 + i * 13, SC_TINY + 0.02f, C_WHITE, false);
	}
	if (logged_in())
		draw_channel_buttons(y + CHAN_BTN_Y, d->can_update);
}

static void draw_user_header(const screen *s, float y)
{
	const view *v = s->v;
	const detail_t *d = &v->det;
	char line[96];
	if (d->ok && d->created) {
		char my[32];
		month_year(my, sizeof(my), d->created);
		snprintf(line, sizeof(line), "Joined %s", my);
	} else {
		snprintf(line, sizeof(line), "%s", d->ok ? "" : "Loading...");
	}
	text(line, PAD, y + 6, SC_SMALL, C_GREY, false);
	if (d->ok) {
		snprintf(line, sizeof(line), "%d followers \xC2\xB7 %d following", d->followers, d->following);
		text_right(line, BOT_W - PAD, y + 6, SC_TINY, C_GREY, false);
	}
	/* Channels | Blocks, underlined like the phone's tabs. */
	const char *names[2] = {"Channels", "Blocks"};
	for (int k = 0; k < 2; k++) {
		float cx = BOT_W / 4.0f + k * BOT_W / 2.0f;
		bool on = s->sub == k;
		char cnt[16] = "";
		const view *kv = k ? s->v2 : s->v;
		if (k == 0 && d->ok)
			snprintf(cnt, sizeof(cnt), "%d", d->channels);
		else if (k == 1 && d->ok && d->blocks)
			snprintf(cnt, sizeof(cnt), "%d", d->blocks);
		else if (kv && kv->n)
			snprintf(cnt, sizeof(cnt), "%d%s", kv->n, kv->req.more ? "+" : "");
		float nw = text_w(names[k], SC_SMALL), cw = cnt[0] ? text_w(cnt, SC_TINY) + 8 : 0;
		float x0 = cx - (nw + (cw ? cw + 6 : 0)) / 2;
		text(names[k], x0, y + 32, SC_SMALL, on ? C_WHITE : C_GREY, on);
		if (cw) {
			rect(x0 + nw + 6, y + 31, cw, 15, C_TILE);
			text(cnt, x0 + nw + 10, y + 33, SC_TINY, on ? C_WHITE : C_GREY, false);
		}
	}
	rect(PAD, y + 54, BOT_W - 2 * PAD, 1, C_LINE);
	rect(s->sub ? BOT_W / 2.0f : PAD, y + 53, BOT_W / 2.0f - PAD, 2, C_WHITE);
}

static void draw_message(const char *msg, const char *hint)
{
	text_center(msg, BOT_W / 2.0f, 80, SC_BODY, C_WHITE, false);
	if (hint)
		text_center(hint, BOT_W / 2.0f, 104, SC_SMALL, C_GREY, false);
}

/* Are.na's logo, an alpha mask in a texture, drawn in any colour. */
static C3D_Tex           g_logo_tex;
static Tex3DS_SubTexture g_logo_sub;
static bool              g_logo_ok;

static void logo_init(void)
{
	/* White with the logo's alpha; the tint gives it its colour. */
	if (!C3D_TexInit(&g_logo_tex, 64, 64, GPU_RGBA8))
		return;
	u32 *dst = g_logo_tex.data;
	memset(dst, 0, 64 * 64 * 4);
	for (int y = 0; y < LOGO_H; y++)
		for (int x = 0; x < LOGO_W; x++)
			dst[swizzle_px(x, y, 64)] = 0xFFFFFF00u | LOGO_ALPHA[y * LOGO_W + x];
	C3D_TexFlush(&g_logo_tex);
	C3D_TexSetFilter(&g_logo_tex, GPU_LINEAR, GPU_LINEAR);
	g_logo_sub = (Tex3DS_SubTexture){LOGO_W, LOGO_H, 0.0f, 1.0f, LOGO_W / 64.0f,
	                                 1.0f - LOGO_H / 64.0f};
	g_logo_ok = true;
}

static void draw_logo(float cx, float cy, float w, u32 c)
{
	if (!g_logo_ok)
		return;
	C2D_Image img = {&g_logo_tex, &g_logo_sub};
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, c, 1.0f);
	float s = w / LOGO_W;
	C2D_DrawImageAt(img, cx - w / 2, cy - LOGO_H * s / 2, g_z, &tint, s, s);
}

static void draw_tabbar(void)
{
	float y = CONTENT_H;
	rect(0, y, BOT_W, TABBAR_H, C_BLACK);
	rect(0, y, BOT_W, 1, C_LINE);
	/* Five slots as on the phone: the + sits in the middle. */
	const float tw = BOT_W / (float)(TABS + 1), cy = y + TABBAR_H / 2;
	{
		float cx = tw * 2 + tw / 2;
		C2D_DrawLine(cx - 7, cy, C_GREY, cx + 7, cy, C_GREY, 1.6f, g_z);
		C2D_DrawLine(cx, cy - 7, C_GREY, cx, cy + 7, C_GREY, 1.6f, g_z);
	}
	for (int t = 0; t < TABS; t++) {
		int slot = t < 2 ? t : t + 1;
		float cx = tw * slot + tw / 2;
		u32 c = t == g_tab ? C_WHITE : C_GREY;
		switch (t) {
		case T_FEED:
			draw_logo(cx, cy, 24, c);
			break;
		case T_EXPLORE:
			ring(cx - 1.5f, cy - 1.5f, 6, 1.6f, c);
			C2D_DrawLine(cx + 2.5f, cy + 2.5f, c, cx + 7, cy + 7, c, 1.8f, g_z);
			break;
		case T_NOTIF: {
			char n[8];
			snprintf(n, sizeof(n), "%d", logged_in() ? store_account.me.unread : 0);
			float w = fmaxf(text_w(n, SC_TINY) + 8, 16);
			rect(cx - w / 2, cy - 8, w, 16, t == g_tab ? C_WHITE : C_TILE);
			text_center(n, cx, cy - 6, SC_TINY, t == g_tab ? C_BLACK : C_GREY, true);
			break;
		}
		case T_PROFILE: {
			const char *ini = logged_in() && store_account.known ? store_account.me.initials : "";
			ring(cx, cy, 9, 1.2f, c);
			if (ini[0]) {
				char one[2] = {ini[0], 0};
				text_center(one, cx, cy - 5.5f, 0.3f, c, false);
			}
			break;
		}
		}
	}
}

static void draw_qr(const char *s, float cx, float cy, float size)
{
	static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	static char last[256];
	static bool ok;
	if (strcmp(last, s)) {
		snprintf(last, sizeof(last), "%s", s);
		ok = qrcodegen_encodeText(s, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);
	}
	if (!ok)
		return;
	int n = qrcodegen_getSize(qr);
	float m = floorf(size / (n + 4));
	float side = m * (n + 4), x0 = floorf(cx - side / 2), y0 = floorf(cy - side / 2);
	rect(x0, y0, side, side, C_WHITE);
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			if (qrcodegen_getModule(qr, x, y))
				C2D_DrawRectSolid(x0 + (x + 2) * m, y0 + (y + 2) * m, g_z, m, m, C_BLACK);
}

static void draw_login_bottom(void)
{
	char msg[320];
	auth_state st = auth_status(msg, sizeof(msg));
	if (st == AUTH_WAITING || st == AUTH_WORKING) {
		draw_qr(auth_page_url(), BOT_W / 2.0f, SCR_H / 2.0f, SCR_H - 24);   /* no tab bar here */
	} else if (st == AUTH_DONE) {
		draw_message("Logged in", NULL);
	} else {
		draw_message(msg[0] ? msg : "Log in to Are.na", GLYPH_A " Try again");
	}
}

static void draw_bottom(void)
{
	screen *s = cur();
	view *v = cur_view(s);
	C2D_TargetClear(g_bot, C_BLACK);
	C2D_SceneBegin(g_bot);

	if (s->type == SC_LOGIN) {
		draw_login_bottom();
	} else if (!v) {
		draw_message("Loading...", NULL);
	} else {
		float sc = floorf(v->scroll);
		clip_outer(BOT_W, 0, 0, BOT_W, CONTENT_H);
		float hy = -sc;
		if (s->type == SC_CHANNEL)
			draw_channel_header(v, hy);
		else if (s->type == SC_USER)
			draw_user_header(s, hy);
		float y0 = header_h(s) - sc;
		if (v->n == 0) {
			if (v->failed)
				draw_message(v->err, GLYPH_Y " Try again");
			else if (v->loading || v->want_more)
				is_grid(v) ? draw_skeleton_grid(v, y0) : draw_skeleton_rows(y0, 4);
			else
				text_center("Nothing here yet", BOT_W / 2.0f, y0 + 30, SC_SMALL, C_GREY, false);
		} else if (is_grid(v)) {
			grid_t g = grid_of(v);
			const float px = g.w + g.gap, py = g.h + g.cap + g.gap;
			int first = (int)((sc - header_h(s)) / py) * g.cols;
			if (first < 0) first = 0;
			int last = first + g.cols * ((int)(CONTENT_H / py) + 2);
			for (int i = first; i < v->n && i < last; i++)
				draw_tile(&v->e[i], g.margin + (i % g.cols) * px, y0 + g.gap + (i / g.cols) * py,
				          g.w, g.h, g.cap, i == v->sel);
		} else {
			float y = y0;
			for (int i = 0; i < v->rh_n; i++) {
				draw_list_row(v, i, y, i == v->sel);
				y += v->rh[i];
			}
		}
		if (v->n > 0 && (v->loading || v->failed)) {
			float end = content_h(s, v) - 24 - sc;
			text_center(v->failed ? v->err : "Loading...", BOT_W / 2.0f, end + 4, SC_TINY, C_GREY, false);
		}
		unclip_outer();
	}
	if (logged_in())
		draw_tabbar();
}

/* ------------------------------------------------------------ top */

#define TOP_Y0 30.0f
#define TOP_Y1 220.0f

static void draw_crumb(const screen *s)
{
	const char *root = "Are.na";
	char title[200];
	const char *t = s->title;
	switch (s->type) {
	case SC_FEED: t = "Feed"; break;
	case SC_EXPLORE: t = "Explore"; break;
	case SC_NOTIF: t = "Notifications"; break;
	case SC_LOGIN: t = "Log in"; break;
	case SC_USER: if (!t[0]) t = "Profile"; break;
	default: break;
	}
	const float sc = 0.56f, sep = text_w(" / ", sc);
	float x = 12;
	text(root, x, 7, sc, C_GREY, true);
	x += text_w(root, sc);
	if (s->parent[0]) {
		text(" / ", x, 7, sc, C_GREY, false);
		x += sep;
		float pw = fminf(text_w(s->parent, sc), 120);
		text_fit(s->parent, x, 7, pw, sc, C_GREY, true);
		x += pw;
	}
	text(" / ", x, 7, sc, C_GREY, false);
	x += sep;
	fit(title, sizeof(title), t, TOP_W - 12 - x, sc);
	text(title, x, 7, sc, C_WHITE, true);
}

static void draw_hints(const char *h)
{
	text(h, 12, 223, SC_TINY, C_DIM, false);
}

/* A block, large: the picture, or its words in a box like the phone's. */
static void draw_block_big(const entry *e)
{
	const float area_h = TOP_Y1 - TOP_Y0 - 18;
	int iw, ih;
	const C2D_Image *img = e->big[0] ? img_get(e->big, &iw, &ih) : NULL;
	if (!img && e->thumb[0])
		img = img_get(e->thumb, &iw, &ih);   /* blurry until the big one lands */
	if (img) {
		draw_image_fit(img, iw, ih, 12, TOP_Y0, TOP_W - 24, area_h);
	} else if ((e->big[0] && !img_failed(e->big)) || (e->thumb[0] && !img_failed(e->thumb))) {
		rect(TOP_W / 2.0f - 90, TOP_Y0, 180, area_h, C_SKEL);
	} else {
		float bw = 300, bx = (TOP_W - bw) / 2;
		rect(bx, TOP_Y0, bw, area_h, C_BLACK);
		outline(bx, TOP_Y0, bw, area_h, 1, C_LINE);
		clip(TOP_W, bx + 10, TOP_Y0 + 8, bw - 20, area_h - 16);
		if (e->btype == B_TEXT && e->body)
			text_wrap(e->body, bx + 14, TOP_Y0 + 12, SC_SMALL + 0.04f, C_WHITE, bw - 28);
		else
			text_wrap(e->title, bx + 14, TOP_Y0 + 12, SC_BODY, C_WHITE, bw - 28);
		unclip();
	}
	char t[200];
	fit(t, sizeof(t), e->title, TOP_W - 40, SC_SMALL);
	text_center(t, TOP_W / 2.0f, TOP_Y1 - 14, SC_SMALL, C_GREY, false);
}

static void draw_channel_big(const entry *e)
{
	u32 tc = vis_text(e->vis);
	C2D_Text t;
	C2D_TextParse(&t, g_tb, e->title);
	C2D_TextOptimize(&t);
	clip(TOP_W, 20, TOP_Y0 + 4, TOP_W - 40, 66);
	C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap | C2D_AlignCenter, TOP_W / 2.0f, TOP_Y0 + 8, g_z,
	             0.72f, 0.72f, tc, TOP_W - 60);
	unclip();
	char line[160], a[48];
	if (e->owner[0]) {
		snprintf(line, sizeof(line), "by %s", e->owner);
		text_center(line, TOP_W / 2.0f, TOP_Y0 + 76, SC_SMALL, C_GREY, false);
	}
	if (e->count >= 0) {
		ago(a, sizeof(a), e->when);
		snprintf(line, sizeof(line), "%d blocks \xC2\xB7 %s", e->count, a);
		text_center(line, TOP_W / 2.0f, TOP_Y0 + 94, SC_SMALL, C_GREY, false);
	}

	const char *urls[PEEK_N];
	int n = store_peek(e->slug, urls);
	const float s = 64, gap = 8, x0 = (TOP_W - (PEEK_N * s + (PEEK_N - 1) * gap)) / 2, y = TOP_Y0 + 118;
	for (int i = 0; i < PEEK_N; i++) {
		float x = x0 + i * (s + gap);
		int iw, ih;
		const C2D_Image *img = n > i ? img_get(urls[i], &iw, &ih) : NULL;
		if (img)
			draw_image_fit(img, iw, ih, x, y, s, s);
		else if (n < 0 || n > i)
			rect(x, y, s, s, C_SKEL);
	}
}

static void draw_user_big(const char *name, const char *initials)
{
	avatar(TOP_W / 2.0f, TOP_Y0 + 70, 44, initials, C_TILE, C_WHITE);
	text_center(name, TOP_W / 2.0f, TOP_Y0 + 128, 0.62f, C_WHITE, true);
}

static void draw_entry_big(const entry *e)
{
	switch (e->kind) {
	case E_BLOCK:
		draw_block_big(e);
		break;
	case E_CHANNEL:
		draw_channel_big(e);
		break;
	case E_USER:
		draw_user_big(e->title, e->initials);
		break;
	case E_ACTIVITY: {
		const ref_t *r = &e->item;
		if (r->kind == E_BLOCK) {
			entry b = {0};
			b.kind = E_BLOCK;
			b.btype = r->btype;
			snprintf(b.title, sizeof(b.title), "%s", r->title);
			snprintf(b.thumb, sizeof(b.thumb), "%s", e->thumb);
			snprintf(b.big, sizeof(b.big), "%s", e->big);
			b.body = e->body;
			draw_block_big(&b);
		} else if (r->kind == E_CHANNEL) {
			entry c = {0};
			c.kind = E_CHANNEL;
			c.vis = r->vis;
			snprintf(c.slug, sizeof(c.slug), "%s", r->slug);
			snprintf(c.title, sizeof(c.title), "%s", r->title);
			c.count = -1;
			draw_channel_big(&c);
		} else if (r->kind == E_USER) {
			draw_user_big(r->title, r->initials);
		}
		break;
	}
	default:
		break;
	}
}

static void draw_login_top(void)
{
	char msg[320];
	auth_state st = auth_status(msg, sizeof(msg));
	float y = TOP_Y0 + 4;
	text("Log in with your phone", 20, y, 0.62f, C_WHITE, true);
	y += 30;
	const char *steps[] = {
		"1  Join the same Wi-Fi as this 3DS.",
		"2  Scan the code below with your phone's camera.",
		auth_has_client() ? "3  Log in with Are.na, then paste the address back."
		                  : "3  Paste a personal access token there.",
	};
	for (int i = 0; i < 3; i++, y += 20)
		text(steps[i], 20, y, SC_SMALL, C_GREY, false);
	y += 10;
	if (st == AUTH_WAITING || st == AUTH_WORKING) {
		text("Or open", 20, y, SC_SMALL, C_GREY, false);
		text(auth_page_url(), 20 + text_w("Or open ", SC_SMALL), y, SC_SMALL, C_WHITE, true);
		y += 26;
		if (msg[0])
			text_wrap(msg, 20, y, SC_SMALL, st == AUTH_WORKING ? C_WHITE : C_RED, TOP_W - 40);
	} else if (st == AUTH_DONE) {
		text("You're logged in.", 20, y, SC_SMALL, C_GREEN, false);
	}
}

static void draw_top(void)
{
	screen *s = cur();
	view *v = cur_view(s);
	/* A channel's Connect and Edit are buttons on the bottom screen; the
	 * hint line leaves them out. */
	const char *chan_hint = "";
	C2D_TargetClear(g_top, C_BLACK);
	C2D_SceneBegin(g_top);
	draw_crumb(s);

	const char *hints = GLYPH_L GLYPH_R " Tabs";
	char hb[160];
	if (s->type == SC_LOGIN) {
		draw_login_top();
		hints = "";
	} else if (s->type == SC_BLOCK) {
		const entry *e = v && v->n > 0 && v->e[0].kind == E_BLOCK ? &v->e[0] : (s->has_pre ? &s->pre : NULL);
		if (e)
			draw_block_big(e);
		snprintf(hb, sizeof(hb), "%s" GLYPH_B " Back   " GLYPH_L GLYPH_R " Tabs",
		         v && v->sel >= 0 && v->sel < v->n ? GLYPH_A " Open channel   " : "");
		hints = hb;
	} else if (v && v->sel >= 0 && v->sel < v->n) {
		const entry *e = &v->e[v->sel];
		draw_entry_big(e);
		bool root = g_depth[g_tab] == 1;
		char xview[32];
		snprintf(xview, sizeof(xview), GLYPH_X " %s   ", EX_NAME[(g_explore + 1) % EX_MODES]);
		snprintf(hb, sizeof(hb), GLYPH_A " Open   %s%s%s" GLYPH_Y " Refresh   " GLYPH_L GLYPH_R " Tabs",
		         root ? "" : GLYPH_B " Back   ",
		         s->type == SC_EXPLORE ? xview
		         : s->type == SC_CHANNEL ? chan_hint
		         : s->type == SC_NOTIF ? (g_notif_feed ? GLYPH_X " Compact   " : GLYPH_X " Feed   ")
		         : e->kind == E_ACTIVITY && e->target.kind == E_CHANNEL ? GLYPH_X " Channel   " : "",
		         s->me && root ? "SELECT Log out   " : "");
		hints = hb;
	} else {
		if (s->type == SC_CHANNEL || s->type == SC_USER) {
			const view *hv = s->v;
			if (hv && hv->det.ok) {
				clip(TOP_W, 20, TOP_Y0, TOP_W - 40, TOP_Y1 - TOP_Y0);
				text_wrap(hv->det.description[0] ? hv->det.description : "", 20, TOP_Y0 + 8,
				          SC_SMALL, C_GREY, TOP_W - 40);
				unclip();
			}
		}
		snprintf(hb, sizeof(hb), "%s%s" GLYPH_Y " Refresh   " GLYPH_L GLYPH_R " Tabs",
		         s->type == SC_CHANNEL ? chan_hint
		         : s->type == SC_NOTIF ? (g_notif_feed ? GLYPH_X " Compact   " : GLYPH_X " Feed   ")
		         : "",
		         g_depth[g_tab] > 1 ? GLYPH_B " Back   " : "");
		hints = hb;
	}
	draw_hints(hints);
}

/* ------------------------------------------------------------ input */

typedef struct {
	bool  dragging, moved;
	float y0, s0, x, y;
	int   item;
} touch_t;

static void select_first(view *v)
{
	if (v && (v->sel < 0 || v->sel >= v->n || !selectable(v, v->sel)))
		v->sel = next_selectable(v, -1, 1);
}

static void move_sel(screen *s, view *v, int dx, int dy)
{
	if (!v || v->n == 0) {
		if (v && dy)
			v->scroll_to += dy * 40;
		return;
	}
	if (is_grid(v)) {
		int sel = v->sel < 0 ? 0 : v->sel;
		const int cols = grid_of(v).cols;
		int i = sel + dx + dy * cols;
		if (i < 0) {
			/* Up from the first row: back to the header. */
			v->scroll_to = 0;
			return;
		}
		if (i >= v->n)
			i = dy > 0 && sel / cols < (v->n - 1) / cols ? v->n - 1 : sel;
		v->sel = i;
	} else if (dy) {
		int i = next_selectable(v, v->sel, dy);
		if (i >= 0)
			v->sel = i;
		else
			v->scroll_to += dy * 40;   /* nothing more to land on: scroll */
		float max = content_h(s, v) - CONTENT_H;
		if (v->scroll_to > max) v->scroll_to = max;
		if (v->scroll_to < 0) v->scroll_to = 0;
		if (i < 0)
			return;
	}
	keep_visible(s, v);
}

static void switch_tab(int t)
{
	if (!logged_in())
		return;      /* only the login page until you are in */
	if (t == g_tab) {
		/* The tab you are on: back to its top. */
		while (g_depth[g_tab] > 1)
			pop();
		return;
	}
	g_tab = t;
}

static void activate(screen *s, view *v)
{
	if (s->type == SC_LOGIN) {
		auth_state st = auth_status(NULL, 0);
		if (st == AUTH_IDLE || st == AUTH_FAILED) {
			char err[96];
			auth_begin(err, sizeof(err));   /* a failure shows in the status */
		}
		return;
	}
	if (!v || v->sel < 0 || v->sel >= v->n || !selectable(v, v->sel))
		return;
	entry e = v->e[v->sel];
	const char *parent = s->type == SC_CHANNEL ? s->title : NULL;
	open_entry(&e, parent);
}

/* ------------------------------------------------------------ adding */

/* The + sheet, as on the phone: Text, Camera, Files, Record, Channel. It
 * takes over both screens until it is done or backed out of. */
typedef enum {
	ADD_NONE, ADD_MENU, ADD_CAMERA, ADD_SHOT, ADD_FILES, ADD_RECORD, ADD_VIS, ADD_PICK, ADD_POST,
	ADD_ACTIONS, ADD_CONFIRM
} add_step;

typedef enum { KB_NONE, KB_TEXT, KB_CHANNEL, KB_EDIT_TITLE, KB_EDIT_BODY, KB_EDIT_CHANNEL } kb_want;

/* The More sheet (X) on a block or a channel. */
typedef enum { A_CONNECT, A_EDIT, A_REMOVE } act_t;

static const char *const ADD_ITEMS[] = {"Text", "Camera", "Files", "Record", "Channel"};
#define ADD_N 5

#define FILES_MAX 512

static struct {
	add_step step;
	int      sel;
	kb_want  kb;
	char     err[128];
	/* what is being added; the draft goes to the poster, these stay */
	post_req draft;
	char     what[160];      /* a line describing it, or the text itself */
	enum { D_NONE, D_TEXT, D_PHOTO, D_FILE, D_SOUND, D_CHANNEL, D_ENTRY } kind;
	char     made_title[128];
	post_op  op;             /* what the poster was last asked to do */
	/* More: what it acts on, and the choices */
	entry    ent;            /* body and md are this sheet's own copies */
	char     parent[160];    /* the channel it was opened from */
	char     parent_slug[80];
	act_t    acts[4];
	char     act_label[4][200];
	int      nacts;
	char     edit_title[160];
	/* camera */
	u16     *shot;           /* the photo taken, CAM_W x CAM_H */
	C3D_Tex  tex;
	bool     tex_ok;
	Tex3DS_SubTexture sub;
	/* files */
	char     dir[384];
	file_ent *files;
	int      nfiles, fsel;
	float    fscroll;
	/* channel picker */
	view    *pick;
	float    pscroll;
} g_add;

static bool adding(void) { return g_add.step != ADD_NONE; }

/* Thin line icons like the phone's menu, drawn about 16px square around
 * (cx, cy): pencil, camera, folder, microphone, plus-in-a-square. */
static void line(float x0, float y0, float x1, float y1, u32 c)
{
	C2D_DrawLine(x0, y0, c, x1, y1, c, 1.4f, g_z);
}

static void add_icon(int i, float cx, float cy, u32 c)
{
	switch (i) {
	case 0: {    /* pencil, leaning right */
		float ax = cx - 6, ay = cy + 6, bx = cx + 4, by = cy - 4;
		line(ax, ay, bx, by, c);
		line(ax + 3, ay + 3, bx + 3, by + 3, c);
		line(bx, by, bx + 2, by - 2, c);
		line(bx + 3, by + 3, bx + 5, by + 1, c);
		line(bx + 2, by - 2, bx + 5, by + 1, c);
		line(ax, ay, ax - 1.5f, ay + 4.5f, c);
		line(ax + 3, ay + 3, ax - 1.5f, ay + 4.5f, c);
		break;
	}
	case 1:      /* camera: body, bump, lens */
		outline(cx - 8, cy - 4, 16, 11, 1.3f, c);
		line(cx - 3, cy - 4, cx - 1.5f, cy - 7, c);
		line(cx - 1.5f, cy - 7, cx + 1.5f, cy - 7, c);
		line(cx + 1.5f, cy - 7, cx + 3, cy - 4, c);
		ring(cx, cy + 1.5f, 3.4f, 1.3f, c);
		break;
	case 2:      /* folder: tab and body */
		line(cx - 8, cy - 5, cx - 3, cy - 5, c);
		line(cx - 3, cy - 5, cx - 1, cy - 3, c);
		line(cx - 1, cy - 3, cx + 8, cy - 3, c);
		line(cx - 8, cy - 5, cx - 8, cy + 6, c);
		line(cx + 8, cy - 3, cx + 8, cy + 6, c);
		line(cx - 8, cy + 6, cx + 8, cy + 6, c);
		break;
	case 3:      /* microphone: capsule, cradle, stand */
		outline(cx - 2.5f, cy - 8, 5, 10, 1.3f, c);
		line(cx - 5.5f, cy - 1, cx - 5.5f, cy + 1, c);
		line(cx + 5.5f, cy - 1, cx + 5.5f, cy + 1, c);
		line(cx - 5.5f, cy + 1, cx - 2.5f, cy + 4.5f, c);
		line(cx + 5.5f, cy + 1, cx + 2.5f, cy + 4.5f, c);
		line(cx - 2.5f, cy + 4.5f, cx + 2.5f, cy + 4.5f, c);
		line(cx, cy + 4.5f, cx, cy + 8, c);
		break;
	case 4:      /* new channel: a square with a plus */
		outline(cx - 7, cy - 7, 14, 14, 1.3f, c);
		line(cx - 3.5f, cy, cx + 3.5f, cy, c);
		line(cx, cy - 3.5f, cx, cy + 3.5f, c);
		break;
	}
}

static void add_tex_init(void)
{
	if (!g_add.tex_ok && C3D_TexInit(&g_add.tex, 512, 256, GPU_RGB565)) {
		g_add.tex_ok = true;
		C3D_TexSetFilter(&g_add.tex, GPU_LINEAR, GPU_LINEAR);
		g_add.sub = (Tex3DS_SubTexture){320, 240, 0.0f, 1.0f, 320.0f / 512, 1.0f - 240.0f / 256};
	}
}

/* A 640x480 camera frame, halved, into the preview texture. */
static void add_tex_from(const u16 *px, bool mirror)
{
	add_tex_init();
	if (!g_add.tex_ok)
		return;
	u16 *dst = g_add.tex.data;
	for (int y = 0; y < 240; y++) {
		const u16 *row = px + (size_t)(y * 2) * CAM_W;
		for (int x = 0; x < 320; x++)
			dst[swizzle_px(x, y, 512)] = row[(mirror ? 319 - x : x) * 2];
	}
	C3D_TexFlush(&g_add.tex);
}

static void add_draw_tex(float x, float y)
{
	if (!g_add.tex_ok)
		return;
	C2D_Image img = {&g_add.tex, &g_add.sub};
	C2D_DrawImageAt(img, x, y, g_z - 0.02f, NULL, 1, 1);
}

static void add_free_draft(void)
{
	free(g_add.draft.text);
	free(g_add.draft.data);
	memset(&g_add.draft, 0, sizeof(g_add.draft));
	g_add.what[0] = '\0';
}

static void add_close(void)
{
	free(g_add.ent.body);
	free(g_add.ent.md);
	g_add.ent.body = g_add.ent.md = NULL;
	cam_stop();
	mic_stop();
	mic_discard();
	add_free_draft();
	free(g_add.shot);
	g_add.shot = NULL;
	free(g_add.files);
	g_add.files = NULL;
	view_free(g_add.pick);
	g_add.pick = NULL;
	if (g_add.tex_ok)
		C3D_TexDelete(&g_add.tex);
	g_add.tex_ok = false;
	post_reset();
	g_add.step = ADD_NONE;
}

static void add_open(void)
{
	if (!logged_in()) {
		g_tab = T_PROFILE;
		return;
	}
	memset(&g_add, 0, sizeof(g_add));
	g_add.step = ADD_MENU;
}

static void add_to_pick(void)
{
	if (!g_add.pick && store_account.known)
		g_add.pick = view_new(L_USER_CHANNELS, store_account.me.slug, 0, false);
	g_add.pscroll = 0;
	g_add.step = ADD_PICK;
}

static void stamp(char *out, size_t n, const char *ext)
{
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(out, n, "3ds-%04d%02d%02d-%02d%02d%02d.%s", tm.tm_year + 1900, tm.tm_mon + 1,
	         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ext);
}

static void files_load(const char *dir)
{
	if (!g_add.files)
		g_add.files = malloc(sizeof(file_ent) * FILES_MAX);
	snprintf(g_add.dir, sizeof(g_add.dir), "%s", dir);
	g_add.nfiles = g_add.files ? list_dir(dir, g_add.files, FILES_MAX) : -1;
	g_add.fsel = 0;
	g_add.fscroll = 0;
}

static void files_up(void)
{
	char *p = g_add.dir + strlen(g_add.dir) - 1;
	if (*p == '/')
		*p = '\0';
	char *slash = strrchr(g_add.dir, '/');
	if (!slash || slash <= g_add.dir + 5) {     /* "sdmc:" */
		files_load("sdmc:/");
		return;
	}
	slash[1] = '\0';
	char d[384];
	snprintf(d, sizeof(d), "%s", g_add.dir);
	files_load(d);
}

static void files_pick(void)
{
	if (g_add.nfiles <= 0)
		return;
	file_ent *f = &g_add.files[g_add.fsel];
	char path[640];
	snprintf(path, sizeof(path), "%s%s%s", g_add.dir,
	         g_add.dir[strlen(g_add.dir) - 1] == '/' ? "" : "/", f->name);
	if (f->dir) {
		strcat(path, "/");
		files_load(path);
		return;
	}
	size_t len = 0;
	unsigned char *b = read_file(path, 25 * 1024 * 1024, &len);
	if (!b) {
		snprintf(g_add.err, sizeof(g_add.err), "Can't read that file (25 MB at most)");
		return;
	}
	add_free_draft();
	g_add.draft.data = b;
	g_add.draft.len = len;
	/* A 3D photo goes up as the JPEG it starts with. */
	const char *ct = content_type_for(f->name);
	snprintf(g_add.draft.filename, sizeof(g_add.draft.filename), "%s", f->name);
	if (!strcasecmp(strrchr(f->name, '.') ? strrchr(f->name, '.') : "", ".mpo"))
		snprintf(strrchr(g_add.draft.filename, '.'), 5, ".jpg");
	snprintf(g_add.draft.ctype, sizeof(g_add.draft.ctype), "%s", ct);
	snprintf(g_add.what, sizeof(g_add.what), "%s", path);
	g_add.kind = D_FILE;
	g_add.err[0] = '\0';
	add_to_pick();
}

/* Keyboard input happens between frames, outside the store lock: the
 * keyboard is a system applet and the app waits while it is up. */
static void add_post(void);

static void add_keyboard(void)
{
	kb_want k = g_add.kb;
	g_add.kb = KB_NONE;
	SwkbdState kb;
	static char buf[4001];
	buf[0] = '\0';
	if (k == KB_TEXT || k == KB_EDIT_BODY) {
		swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 4000);
		swkbdSetFeatures(&kb, SWKBD_MULTILINE | SWKBD_PREDICTIVE_INPUT);
	} else {
		swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 100);
	}
	switch (k) {
	case KB_TEXT: swkbdSetHintText(&kb, "Text, or a web address"); break;
	case KB_CHANNEL: swkbdSetHintText(&kb, "Channel name"); break;
	case KB_EDIT_TITLE: swkbdSetHintText(&kb, "Title"); break;
	case KB_EDIT_BODY:
		swkbdSetHintText(&kb, g_add.ent.btype == B_TEXT ? "Text" : "Description");
		break;
	case KB_EDIT_CHANNEL: swkbdSetHintText(&kb, "Channel name"); break;
	default: break;
	}
	/* Edits start from what is there now. */
	if (k == KB_EDIT_TITLE)
		swkbdSetInitialText(&kb, g_add.ent.has_title ? g_add.ent.title : "");
	else if (k == KB_EDIT_BODY)
		swkbdSetInitialText(&kb, g_add.ent.md ? g_add.ent.md : g_add.ent.body ? g_add.ent.body : "");
	else if (k == KB_EDIT_CHANNEL)
		swkbdSetInitialText(&kb, g_add.ent.title);
	bool may_be_empty = k == KB_EDIT_TITLE || (k == KB_EDIT_BODY && g_add.ent.btype != B_TEXT);
	swkbdSetValidation(&kb, may_be_empty ? SWKBD_ANYTHING : SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	SwkbdButton b = swkbdInputText(&kb, buf, sizeof(buf));
	store_lock();
	/* Backed out of a keyboard opened straight from a button: no sheet
	 * to go back to. */
	if (b != SWKBD_BUTTON_CONFIRM && g_add.step == ADD_ACTIONS && !g_add.nacts) {
		add_close();
		store_unlock();
		return;
	}
	if (b == SWKBD_BUTTON_CONFIRM && k == KB_EDIT_TITLE) {
		snprintf(g_add.edit_title, sizeof(g_add.edit_title), "%s", buf);
		g_add.kb = KB_EDIT_BODY;
	} else if (b == SWKBD_BUTTON_CONFIRM && k == KB_EDIT_BODY) {
		add_free_draft();
		g_add.kind = D_ENTRY;
		g_add.draft.op = OP_EDIT_BLOCK;
		g_add.draft.target_id = g_add.ent.id;
		g_add.draft.target_is_text = g_add.ent.btype == B_TEXT;
		snprintf(g_add.draft.title, sizeof(g_add.draft.title), "%s", g_add.edit_title);
		g_add.draft.text = strdup(buf);
		add_post();
	} else if (b == SWKBD_BUTTON_CONFIRM && k == KB_EDIT_CHANNEL && buf[0]) {
		add_free_draft();
		g_add.kind = D_ENTRY;
		g_add.draft.op = OP_EDIT_CHANNEL;
		g_add.draft.target_id = g_add.ent.id;
		snprintf(g_add.draft.title, sizeof(g_add.draft.title), "%s", buf);
		snprintf(g_add.made_title, sizeof(g_add.made_title), "%s", buf);
		g_add.sel = g_add.ent.vis == VIS_PUBLIC ? 0 : g_add.ent.vis == VIS_PRIVATE ? 2 : 1;
		g_add.step = ADD_VIS;
	} else if (b == SWKBD_BUTTON_CONFIRM && buf[0] && (k == KB_TEXT || k == KB_CHANNEL)) {
		add_free_draft();
		if (k == KB_TEXT) {
			g_add.draft.text = strdup(buf);
			snprintf(g_add.what, sizeof(g_add.what), "%s", buf);
			g_add.kind = D_TEXT;
			add_to_pick();
		} else {
			snprintf(g_add.draft.title, sizeof(g_add.draft.title), "%s", buf);
			g_add.draft.op = OP_NEW_CHANNEL;
			g_add.kind = D_CHANNEL;
			snprintf(g_add.made_title, sizeof(g_add.made_title), "%s", buf);
			g_add.draft.vis = VIS_CLOSED;
			g_add.sel = 1;
			g_add.step = ADD_VIS;
		}
	}
	store_unlock();
}

static void add_choose(int i)
{
	char err[96];
	g_add.err[0] = '\0';
	switch (i) {
	case 0:
		g_add.kb = KB_TEXT;
		break;
	case 1:
		if (cam_start(false, err, sizeof(err)))
			g_add.step = ADD_CAMERA;
		else
			snprintf(g_add.err, sizeof(g_add.err), "%s", err);
		break;
	case 2: {
		DIR *d = opendir("sdmc:/DCIM");
		if (d)
			closedir(d);
		files_load(d ? "sdmc:/DCIM/" : "sdmc:/");
		g_add.step = ADD_FILES;
		break;
	}
	case 3:
		mic_discard();
		g_add.step = ADD_RECORD;
		break;
	case 4:
		g_add.kb = KB_CHANNEL;
		break;
	}
}

static void add_take_photo(void)
{
	const u16 *f = NULL;
	/* The newest frame; if none came since the last draw, the preview
	 * texture's source is gone, so wait a moment for the next. */
	for (int i = 0; i < 20 && !(f = cam_frame()); i++)
		svcSleepThread(20000000LL);
	if (!f)
		return;
	free(g_add.shot);
	g_add.shot = malloc(CAM_W * CAM_H * 2);
	if (!g_add.shot)
		return;
	memcpy(g_add.shot, f, CAM_W * CAM_H * 2);
	cam_shutter();
	add_tex_from(g_add.shot, cam_inner());
	g_add.step = ADD_SHOT;
}

static void add_use_photo(void)
{
	size_t len = 0;
	unsigned char *j = jpeg_encode_565(g_add.shot, CAM_W, CAM_H, 90, &len);
	if (!j) {
		snprintf(g_add.err, sizeof(g_add.err), "Could not save the photo");
		return;
	}
	cam_stop();
	add_free_draft();
	g_add.draft.data = j;
	g_add.draft.len = len;
	stamp(g_add.draft.filename, sizeof(g_add.draft.filename), "jpg");
	snprintf(g_add.draft.ctype, sizeof(g_add.draft.ctype), "image/jpeg");
	snprintf(g_add.what, sizeof(g_add.what), "Photo, %u KB", (unsigned)(len / 1024));
	g_add.kind = D_PHOTO;
	add_to_pick();
}

static void add_use_recording(void)
{
	size_t len = 0;
	unsigned char *w = mic_wav(&len);
	if (!w)
		return;
	add_free_draft();
	g_add.draft.data = w;
	g_add.draft.len = len;
	stamp(g_add.draft.filename, sizeof(g_add.draft.filename), "wav");
	snprintf(g_add.draft.ctype, sizeof(g_add.draft.ctype), "audio/wav");
	snprintf(g_add.what, sizeof(g_add.what), "Recording, %d:%02d", (int)mic_seconds() / 60,
	         (int)mic_seconds() % 60);
	g_add.kind = D_SOUND;
	add_to_pick();
}

static void add_post(void)
{
	g_add.op = g_add.draft.op;
	post_start(&g_add.draft);
	g_add.step = ADD_POST;
}

static void add_post_to(const entry *ch)
{
	g_add.draft.channel_id = ch->id;
	snprintf(g_add.draft.channel, sizeof(g_add.draft.channel), "%s", ch->title);
	add_post();
}

/* X on a block or a channel: what you can do with it. */
static void act_open(screen *s)
{
	view *v = cur_view(s);
	entry e = {0};
	bool is_block = s->type == SC_BLOCK;
	if (is_block) {
		if (v && v->n > 0 && v->e[0].kind == E_BLOCK)
			e = v->e[0];
		else if (s->has_pre)
			e = s->pre;
		else
			return;
		/* The connection is the one it was opened through. Listings leave
		 * out whether you may undo it: you may if the channel is yours to
		 * edit, or the connection was yours. */
		e.conn_id = s->has_pre ? s->pre.conn_id : 0;
		e.can_remove = s->has_pre && s->pre.can_remove;
		if (s->has_pre && store_account.known && !strcmp(s->pre.conn_by, store_account.me.slug))
			e.can_remove = true;
		if (g_depth[g_tab] > 1) {
			screen *ps = &g_stack[g_tab][g_depth[g_tab] - 2];
			if (ps->type == SC_CHANNEL && ps->v && ps->v->det.can_update)
				e.can_remove = true;
		}
	} else if (s->type == SC_CHANNEL && s->v && s->v->det.ok) {
		const detail_t *d = &s->v->det;
		e.kind = E_CHANNEL;
		e.id = d->id;
		e.vis = d->vis;
		e.count = d->length;
		e.when = d->updated;
		e.can_manage = d->can_update;
		snprintf(e.slug, sizeof(e.slug), "%s", s->slug);
		snprintf(e.title, sizeof(e.title), "%s", d->title);
		snprintf(e.owner, sizeof(e.owner), "%s", d->owner);
	} else {
		return;
	}
	memset(&g_add, 0, sizeof(g_add));
	g_add.ent = e;
	g_add.ent.body = e.body ? strdup(e.body) : NULL;
	g_add.ent.md = e.md ? strdup(e.md) : NULL;
	g_add.kind = D_ENTRY;
	snprintf(g_add.parent, sizeof(g_add.parent), "%s", s->parent);
	int n = 0;
	if (logged_in()) {
		g_add.acts[n] = A_CONNECT;
		snprintf(g_add.act_label[n++], 200, "Connect");
		if (e.can_manage) {
			g_add.acts[n] = A_EDIT;
			snprintf(g_add.act_label[n++], 200, "Edit");
		}
		if (is_block && e.conn_id && e.can_remove && s->parent[0]) {
			g_add.acts[n] = A_REMOVE;
			snprintf(g_add.act_label[n++], 200, "Remove from %s", s->parent);
		}
	}
	if (!n)
		return;
	g_add.nacts = n;
	g_add.step = ADD_ACTIONS;
}

static void act_choose(int i);

/* Straight to one action, without the More list (a channel's buttons). */
static void act_start(screen *s, act_t a)
{
	act_open(s);
	if (!adding())
		return;
	for (int i = 0; i < g_add.nacts; i++) {
		if (g_add.acts[i] == a) {
			g_add.nacts = 0;   /* backing out closes, no list to return to */
			act_choose(i);
			return;
		}
	}
	add_close();
}

static void act_choose(int i)
{
	entry *e = &g_add.ent;
	switch (g_add.acts[i]) {
	case A_CONNECT:
		g_add.draft.op = OP_CONNECT;
		g_add.draft.target_id = e->id;
		g_add.draft.target_is_channel = e->kind == E_CHANNEL;
		add_to_pick();
		break;
	case A_EDIT:
		g_add.kb = e->kind == E_CHANNEL ? KB_EDIT_CHANNEL : KB_EDIT_TITLE;
		break;
	case A_REMOVE:
		g_add.step = ADD_CONFIRM;
		break;
	}
}

/* Where B goes from each step. */
static void add_back(void)
{
	switch (g_add.step) {
	case ADD_MENU:
		add_close();
		break;
	case ADD_CAMERA:
		cam_stop();
		g_add.step = ADD_MENU;
		break;
	case ADD_SHOT: {
		char err[96];
		cam_start(cam_inner(), err, sizeof(err));
		g_add.step = ADD_CAMERA;
		break;
	}
	case ADD_FILES:
		if (strcmp(g_add.dir, "sdmc:/"))
			files_up();
		else
			g_add.step = ADD_MENU;
		break;
	case ADD_RECORD:
		mic_stop();
		mic_discard();
		g_add.step = ADD_MENU;
		break;
	case ADD_VIS:
	case ADD_PICK:
	case ADD_CONFIRM:
		add_free_draft();
		if (g_add.nacts)
			g_add.step = ADD_ACTIONS;
		else if (g_add.kind == D_ENTRY)
			add_close();   /* came straight from a button */
		else
			g_add.step = ADD_MENU;
		break;
	case ADD_ACTIONS:
		add_close();
		break;
	case ADD_POST:
		if (post_status(NULL, 0, NULL, 0) >= POST_DONE)
			add_close();
		break;
	default:
		break;
	}
}

#define PICK_H 56.0f
#define FILE_H 24.0f

static void add_input(u32 down, u32 rep, u32 held)
{
	touchPosition tp;
	bool tap = (down & KEY_TOUCH) != 0;
	if (tap)
		hidTouchRead(&tp);
	(void)held;
	bool has_header = g_add.step == ADD_MENU || g_add.step == ADD_FILES || g_add.step == ADD_VIS ||
	                  g_add.step == ADD_PICK || g_add.step == ADD_ACTIONS || g_add.step == ADD_CONFIRM;
	if (down & KEY_B || (tap && has_header && tp.py < 26 && tp.px > BOT_W - 80)) {
		add_back();
		return;
	}
	switch (g_add.step) {
	case ADD_MENU:
		if (rep & KEY_UP && g_add.sel > 0) g_add.sel--;
		if (rep & KEY_DOWN && g_add.sel < ADD_N - 1) g_add.sel++;
		if (tap && tp.py >= 24 && tp.py < 24 + ADD_N * 34 + 10) {
			int i = (tp.py - 24) / 34;
			if (i >= 4) i = tp.py >= 24 + 4 * 34 + 10 ? 4 : 3;
			if (i == g_add.sel)
				add_choose(i);
			g_add.sel = i;
		} else if (down & KEY_A) {
			add_choose(g_add.sel);
		}
		break;
	case ADD_ACTIONS:
		if (rep & KEY_UP && g_add.sel > 0) g_add.sel--;
		if (rep & KEY_DOWN && g_add.sel < g_add.nacts - 1) g_add.sel++;
		if (tap && tp.py >= 30 && tp.py < 30 + g_add.nacts * 36) {
			int i = (tp.py - 30) / 36;
			if (i == g_add.sel)
				down |= KEY_A;
			g_add.sel = i;
		}
		if (down & KEY_A)
			act_choose(g_add.sel);
		break;
	case ADD_CONFIRM:
		if (down & KEY_A || (tap && tp.py >= 120 && tp.py < 170)) {
			add_free_draft();
			g_add.draft.op = OP_REMOVE;
			g_add.draft.conn_id = g_add.ent.conn_id;
			snprintf(g_add.draft.channel, sizeof(g_add.draft.channel), "%s", g_add.parent);
			add_post();
		}
		break;
	case ADD_CAMERA:
		if (down & (KEY_A | KEY_TOUCH)) {
			add_take_photo();
		} else if (down & (KEY_X | KEY_L | KEY_R)) {
			char err[96];
			if (!cam_start(!cam_inner(), err, sizeof(err)))
				snprintf(g_add.err, sizeof(g_add.err), "%s", err);
		}
		break;
	case ADD_SHOT:
		if (down & KEY_A || (tap && tp.py > CONTENT_H / 2))
			add_use_photo();
		else if (tap)
			add_back();
		break;
	case ADD_FILES: {
		/* The pad moves the cursor and the list follows it; the stylus
		 * drags the list, and a tap selects (a second tap opens). */
		static bool  dragging, moved;
		static float y0, s0, ty;
		int n = g_add.nfiles;
		int before = g_add.fsel;
		const float vis = SCR_H - 30 - 26;
		if (rep & KEY_UP && g_add.fsel > 0) g_add.fsel--;
		if (rep & KEY_DOWN && g_add.fsel < n - 1) g_add.fsel++;
		if (rep & KEY_LEFT) g_add.fsel = g_add.fsel > 8 ? g_add.fsel - 8 : 0;
		if (rep & KEY_RIGHT && n > 0) g_add.fsel = g_add.fsel + 8 < n ? g_add.fsel + 8 : n - 1;
		if (tap && tp.py >= 28 && tp.py < SCR_H - 26) {
			dragging = true;
			moved = false;
			y0 = ty = tp.py;
			s0 = g_add.fscroll;
		} else if (dragging && (held & KEY_TOUCH)) {
			touchPosition t;
			hidTouchRead(&t);
			ty = t.py;
			if (fabsf(ty - y0) > 6)
				moved = true;
			if (moved) {
				float max = n * FILE_H - vis;
				float sv = s0 - (ty - y0);
				if (sv > max) sv = max;
				if (sv < 0) sv = 0;
				g_add.fscroll = sv;
			}
		} else if (dragging) {
			dragging = false;
			if (!moved) {
				int i = (int)((y0 - 30 + g_add.fscroll) / FILE_H);
				if (i >= 0 && i < n) {
					if (i == g_add.fsel)
						files_pick();
					else
						g_add.fsel = i;
				}
			}
		} else if (down & KEY_A) {
			files_pick();
		}
		if (g_add.fsel != before && g_add.step == ADD_FILES) {
			float y = g_add.fsel * FILE_H;
			if (y < g_add.fscroll) g_add.fscroll = y;
			if (y + FILE_H > g_add.fscroll + vis) g_add.fscroll = y + FILE_H - vis;
		}
		break;
	}
	case ADD_RECORD: {
		char err[96];
		bool go = (down & KEY_A) || tap;
		if (go && mic_running()) {
			mic_stop();
		} else if (go && mic_seconds() > 0.2f && !(down & KEY_Y)) {
			add_use_recording();
		} else if (go || (down & KEY_Y)) {
			if (!mic_start(err, sizeof(err)))
				snprintf(g_add.err, sizeof(g_add.err), "%s", err);
		}
		break;
	}
	case ADD_VIS:
		if (rep & KEY_UP && g_add.sel > 0) g_add.sel--;
		if (rep & KEY_DOWN && g_add.sel < 2) g_add.sel++;
		if (tap && tp.py >= 60 && tp.py < 60 + 3 * 40) {
			int i = (tp.py - 60) / 40;
			if (i == g_add.sel)
				down |= KEY_A;
			g_add.sel = i;
		}
		if (down & KEY_A) {
			static const visibility V[3] = {VIS_PUBLIC, VIS_CLOSED, VIS_PRIVATE};
			g_add.draft.vis = V[g_add.sel];
			add_post();
		}
		break;
	case ADD_PICK: {
		view *v = g_add.pick;
		if (!v)
			break;
		if (rep & KEY_UP && v->sel > 0) v->sel--;
		if (rep & KEY_DOWN && v->sel < v->n - 1) v->sel++;
		if (tap && tp.py >= 30) {
			int i = (int)((tp.py - 30 + g_add.pscroll) / PICK_H);
			if (i >= 0 && i < v->n) {
				if (i == v->sel)
					down |= KEY_A;
				v->sel = i;
			}
		}
		if (v->sel >= 0 && v->sel < v->n && down & KEY_A)
			add_post_to(&v->e[v->sel]);
		else if (down & KEY_Y)
			view_reload(v);
		if (v->sel < 0 && v->n > 0)
			v->sel = 0;
		float y = v->sel * PICK_H, vis = SCR_H - 30;
		if (y < g_add.pscroll) g_add.pscroll = y;
		if (y + PICK_H > g_add.pscroll + vis) g_add.pscroll = y + PICK_H - vis;
		if (v->sel >= v->n - 4)
			view_more(v);
		break;
	}
	case ADD_POST: {
		char slug[80];
		post_state st = post_status(NULL, 0, slug, sizeof(slug));
		if (st == POST_DONE && (down & KEY_A || tap)) {
			post_op op = g_add.op;
			char title[128];
			snprintf(title, sizeof(title), "%s", g_add.made_title);
			add_close();
			screen *cs = cur();
			view *cv = cur_view(cs);
			if (op == OP_NEW_CHANNEL && slug[0]) {
				open_channel(g_tab, slug, title, store_account.me.name);
			} else if (op == OP_REMOVE && cs->type == SC_BLOCK) {
				/* Gone from the channel: back to it, without the block. */
				pop();
				view *pv = cur_view(cur());
				if (pv)
					view_reload(pv);
			} else {
				if (op == OP_EDIT_CHANNEL) {
					snprintf(cs->title, sizeof(cs->title), "%s", title);
					if (cs->v)
						cs->v->want_detail = true;
				}
				if (op == OP_CONNECT && cs->type == SC_CHANNEL && cs->v)
					cs->v->want_detail = true;
				if (cv && (cs->type == SC_CHANNEL || cs->type == SC_USER || cs->type == SC_BLOCK))
					view_reload(cv);
				/* An edited block shows its new title in the channel too. */
				if (op == OP_EDIT_BLOCK && g_depth[g_tab] > 1) {
					screen *ps = &g_stack[g_tab][g_depth[g_tab] - 2];
					view *pv = cur_view(ps);
					if (pv)
						view_reload(pv);
				}
			}
		} else if (st == POST_FAILED && (down & KEY_A || tap)) {
			char msg[128];
			post_status(msg, sizeof(msg), NULL, 0);
			add_close();
			/* A login from before adding existed can only read: log in
			 * again, which now asks for write access too. */
			if (!strcmp(msg, API_ERR_READONLY)) {
				auth_logout();
				store_account.known = false;
				g_tab = T_PROFILE;
			}
		}
		break;
	}
	default:
		break;
	}
}

/* ---- drawing ---- */

/* Every sheet's header has a Cancel at the right for touch, like B. */
static void add_header(const char *title)
{
	text(title, PAD, 6, SC_BODY, C_WHITE, true);
	text_right("Cancel", BOT_W - PAD, 7, SC_SMALL, C_GREY, false);
	rect(0, 26, BOT_W, 1, C_LINE);
}

static void add_draw_bottom(void)
{
	C2D_TargetClear(g_bot, C_BLACK);
	C2D_SceneBegin(g_bot);
	char line[160];
	switch (g_add.step) {
	case ADD_MENU:
		add_header("Add");
		for (int i = 0; i < ADD_N; i++) {
			float y = 24 + i * 34 + (i == 4 ? 10 : 0);
			if (i == 4)
				rect(PAD, y - 6, BOT_W - 2 * PAD, 1, C_LINE);
			bool on = i == g_add.sel;
			if (on)
				rect(PAD - 4, y + 2, BOT_W - 2 * PAD + 8, 30, C_TILE);
			add_icon(i, PAD + 14, y + 17, on ? C_WHITE : C_GREY);
			text(ADD_ITEMS[i], PAD + 32, y + 9, SC_BODY, on ? C_WHITE : C_GREY, on);
		}
		break;
	case ADD_CAMERA:
	case ADD_SHOT: {
		bool shot = g_add.step == ADD_SHOT;
		/* The shutter: a ring with a dot, tap anywhere for it. */
		float cx = BOT_W / 2.0f, cy = CONTENT_H / 2.0f;
		if (!shot) {
			ring(cx, cy, 34, 3, C_WHITE);
			disc(cx, cy, 27, C_WHITE);
			text_center("Tap or " GLYPH_A " to take a photo", cx, cy + 46, SC_SMALL, C_GREY, false);
		} else {
			rect(PAD, CONTENT_H / 2 + 10, BOT_W - 2 * PAD, 60, C_WHITE);
			text_center("Use photo", cx, CONTENT_H / 2 + 30, SC_BODY, C_BLACK, true);
			text_center("Retake", cx, CONTENT_H / 2 - 40, SC_BODY, C_GREY, false);
		}
		break;
	}
	case ADD_FILES: {
		add_header(g_add.dir + 5);
		clip_outer(BOT_W, 0, 28, BOT_W, SCR_H - 28 - 26);
		if (g_add.nfiles < 0) {
			text_center("Can't open this folder", BOT_W / 2.0f, 80, SC_SMALL, C_GREY, false);
		} else if (g_add.nfiles == 0) {
			text_center("Empty folder", BOT_W / 2.0f, 80, SC_SMALL, C_GREY, false);
		}
		for (int i = 0; i < g_add.nfiles; i++) {
			float y = 30 + i * FILE_H - floorf(g_add.fscroll);
			if (y < 0 || y > SCR_H)
				continue;
			const file_ent *f = &g_add.files[i];
			bool on = i == g_add.fsel;
			if (on)
				rect(4, y, BOT_W - 8, FILE_H - 2, C_TILE);
			if (f->dir) {
				rect(PAD, y + 7, 12, 9, C_GREY);
				rect(PAD, y + 5, 6, 3, C_GREY);
			} else {
				outline(PAD + 1, y + 4, 10, 13, 1, C_GREY);
			}
			text_fit(f->name, PAD + 20, y + 4, 200, SC_SMALL, on ? C_WHITE : C_GREY, on);
			if (!f->dir) {
				snprintf(line, sizeof(line), f->size >= 1048576 ? "%.1f MB" : "%.0f KB",
				         f->size >= 1048576 ? f->size / 1048576.0 : f->size / 1024.0);
				text_right(line, BOT_W - PAD, y + 5, SC_TINY, C_DIM, false);
			}
		}
		unclip_outer();
		rect(0, SCR_H - 26, BOT_W, 26, C_BLACK);
		rect(0, SCR_H - 26, BOT_W, 1, C_LINE);
		/* B climbs a folder, or at the top of the card goes back to Add. */
		text(strcmp(g_add.dir, "sdmc:/") ? GLYPH_A " Open   " GLYPH_B " Up   Left/Right Page"
		                                 : GLYPH_A " Open   " GLYPH_B " Back   Left/Right Page",
		     PAD, SCR_H - 20, SC_TINY, C_GREY, false);
		break;
	}
	case ADD_RECORD: {
		float cx = BOT_W / 2.0f;
		float s = mic_seconds();
		snprintf(line, sizeof(line), "%d:%02d", (int)s / 60, (int)s % 60);
		text_center(line, cx, 40, 1.1f, mic_running() ? C_RED : C_WHITE, true);
		float lw = (BOT_W - 80) * fminf(mic_level() * 2.5f, 1.0f);
		rect(40, 104, BOT_W - 80, 6, C_TILE);
		rect(40, 104, lw, 6, C_WHITE);
		if (mic_running()) {
			disc(cx, 160, 26, C_RED);
			rect(cx - 9, 151, 18, 18, C_WHITE);
			text_center("Tap or " GLYPH_A " to stop", cx, 196, SC_SMALL, C_GREY, false);
		} else if (s > 0.2f) {
			rect(PAD, 140, BOT_W - 2 * PAD, 44, C_WHITE);
			text_center("Use recording", cx, 152, SC_BODY, C_BLACK, true);
			text_center(GLYPH_Y " Record again", cx, 196, SC_SMALL, C_GREY, false);
		} else {
			disc(cx, 160, 26, C_RED);
			text_center("Tap or " GLYPH_A " to record", cx, 196, SC_SMALL, C_GREY, false);
		}
		break;
	}
	case ADD_VIS: {
		add_header("Who can see it?");
		static const char *N[3] = {"Public", "Closed", "Private"};
		static const char *D[3] = {"Anyone can see and add to it", "Anyone can see it, only you add",
		                           "Only you can see it"};
		static const visibility V[3] = {VIS_PUBLIC, VIS_CLOSED, VIS_PRIVATE};
		for (int i = 0; i < 3; i++) {
			float y = 60 + i * 40;
			bool on = i == g_add.sel;
			rect(PAD, y, BOT_W - 2 * PAD, 36, vis_bg(V[i]));
			outline(PAD, y, BOT_W - 2 * PAD, 36, on ? 2 : 1, on ? C_WHITE : vis_line(V[i]));
			text(N[i], PAD + 10, y + 4, SC_SMALL, vis_text(V[i]), true);
			text(D[i], PAD + 10, y + 20, SC_TINY, C_GREY, false);
		}
		break;
	}
	case ADD_ACTIONS:
		add_header("More");
		for (int i = 0; i < g_add.nacts; i++) {
			float y = 30 + i * 36;
			bool on = i == g_add.sel;
			if (on)
				rect(PAD - 4, y, BOT_W - 2 * PAD + 8, 32, C_TILE);
			bool danger = g_add.acts[i] == A_REMOVE;
			char t[200];
			fit(t, sizeof(t), g_add.act_label[i], BOT_W - 2 * PAD - 16, SC_BODY);
			text(t, PAD + 8, y + 8, SC_BODY, danger ? C_RED : on ? C_WHITE : C_GREY, on);
		}
		break;
	case ADD_CONFIRM: {
		add_header("Remove");
		char t[260];
		snprintf(t, sizeof(t), "Take this block out of %s? It stays in your other channels.", g_add.parent);
		text_wrap(t, PAD, 44, SC_SMALL, C_GREY, BOT_W - 2 * PAD);
		rect(PAD, 120, BOT_W - 2 * PAD, 44, C_RED);
		text_center("Remove", BOT_W / 2.0f, 132, SC_BODY, C_WHITE, true);
		text_center(GLYPH_B " Cancel", BOT_W / 2.0f, 180, SC_SMALL, C_GREY, false);
		break;
	}
	case ADD_PICK: {
		add_header(g_add.draft.op == OP_CONNECT ? "Connect to..." : "Add to...");
		view *v = g_add.pick;
		clip_outer(BOT_W, 0, 28, BOT_W, SCR_H - 28);
		if (!v || (v->n == 0 && (v->loading || v->want_more))) {
			text_center("Loading...", BOT_W / 2.0f, 80, SC_SMALL, C_GREY, false);
		} else if (v->n == 0) {
			text_center(v->failed ? v->err : "You have no channels yet", BOT_W / 2.0f, 80, SC_SMALL,
			            C_GREY, false);
		}
		for (int i = 0; v && i < v->n; i++) {
			float y = 33 + i * PICK_H - floorf(g_add.pscroll);
			if (y > SCR_H || y + PICK_H < 0)
				continue;
			draw_channel_row(&v->e[i], y, i == v->sel);
		}
		unclip_outer();
		break;
	}
	case ADD_POST: {
		char msg[128];
		post_state st = post_status(msg, sizeof(msg), NULL, 0);
		text_center(msg, BOT_W / 2.0f, 80, SC_BODY, st == POST_FAILED ? C_RED : C_WHITE, st >= POST_DONE);
		if (st == POST_FAILED && !strcmp(msg, API_ERR_READONLY))
			text_center(GLYPH_A " Log in again to allow adding", BOT_W / 2.0f, 110, SC_SMALL, C_WHITE, false);
		else if (st < POST_DONE) {
			int dots = (int)(g_time * 3) % 4;
			text_center(dots == 0 ? "" : dots == 1 ? "." : dots == 2 ? ". ." : ". . .", BOT_W / 2.0f,
			            104, SC_BODY, C_GREY, false);
		}
		if (st == POST_DONE || (st == POST_FAILED && strcmp(msg, API_ERR_READONLY)))
			text_center(GLYPH_A " Done", BOT_W / 2.0f, 140, SC_SMALL, C_GREY, false);
		break;
	}
	default:
		break;
	}
	if (g_add.err[0])
		text_center(g_add.err, BOT_W / 2.0f, SCR_H - 46, SC_TINY, C_RED, false);
}

/* What is about to go up, on the top screen. */
static void add_draw_draft(float y)
{
	const float h = TOP_Y1 - y - 20;
	switch (g_add.kind) {
	case D_TEXT: {
		float bw = 300, bx = (TOP_W - bw) / 2;
		outline(bx, y, bw, h + 10, 1, C_LINE);
		clip(TOP_W, bx + 10, y + 8, bw - 20, h - 6);
		text_wrap(g_add.what, bx + 14, y + 12, SC_SMALL + 0.04f, C_WHITE, bw - 28);
		unclip();
		break;
	}
	case D_PHOTO:
		if (g_add.tex_ok) {
			C2D_Image img = {&g_add.tex, &g_add.sub};
			draw_image_fit(&img, 320, 240, 12, y, TOP_W - 24, h);
		}
		break;
	case D_FILE: {
		int iw, ih;
		const C2D_Image *img = is_image_name(g_add.what) ? img_get(g_add.what, &iw, &ih) : NULL;
		if (img)
			draw_image_fit(img, iw, ih, 12, y, TOP_W - 24, h);
		char t[200];
		fit(t, sizeof(t), strrchr(g_add.what, '/') ? strrchr(g_add.what, '/') + 1 : g_add.what,
		    TOP_W - 40, SC_SMALL);
		text_center(t, TOP_W / 2.0f, img ? TOP_Y1 - 14 : y + 70, img ? SC_SMALL : SC_BODY,
		            img ? C_GREY : C_WHITE, false);
		break;
	}
	case D_ENTRY:
		draw_entry_big(&g_add.ent);
		break;
	case D_SOUND:
	case D_CHANNEL:
		text_center(g_add.kind == D_SOUND ? g_add.what : g_add.made_title, TOP_W / 2.0f, y + 70,
		            0.62f, C_WHITE, true);
		break;
	default:
		break;
	}
}

static void add_draw_top(void)
{
	C2D_TargetClear(g_top, C_BLACK);
	C2D_SceneBegin(g_top);
	const char *hint = "";
	char crumb[200];
	switch (g_add.step) {
	case ADD_MENU:
		text("Are.na / ", 12, 7, 0.56f, C_GREY, true);
		text("Add", 12 + text_w("Are.na / ", 0.56f), 7, 0.56f, C_WHITE, true);
		/* Just the choice, large, in the middle. */
		C2D_ViewTranslate(TOP_W / 2.0f, 100);
		C2D_ViewScale(3.2f, 3.2f);
		add_icon(g_add.sel, 0, 0, C_WHITE);
		C2D_ViewReset();
		text_center(ADD_ITEMS[g_add.sel], TOP_W / 2.0f, 148, 0.8f, C_WHITE, true);
		hint = GLYPH_A " Choose   " GLYPH_B " Close";
		break;
	case ADD_CAMERA: {
		const u16 *f = cam_frame();
		if (f)
			add_tex_from(f, cam_inner());
		add_draw_tex(40, 0);
		hint = GLYPH_A " Take   " GLYPH_X " Switch camera   " GLYPH_B " Cancel";
		break;
	}
	case ADD_SHOT:
		add_draw_tex(40, 0);
		hint = GLYPH_A " Use photo   " GLYPH_B " Retake";
		break;
	case ADD_FILES:
		text("Are.na / Add / ", 12, 7, 0.56f, C_GREY, true);
		text("Files", 12 + text_w("Are.na / Add / ", 0.56f), 7, 0.56f, C_WHITE, true);
		if (g_add.nfiles > 0) {
			const file_ent *f = &g_add.files[g_add.fsel];
			char path[640];
			snprintf(path, sizeof(path), "%s%s", g_add.dir, f->name);
			int iw, ih;
			const C2D_Image *img = !f->dir && is_image_name(f->name) ? img_get(path, &iw, &ih) : NULL;
			if (img)
				draw_image_fit(img, iw, ih, 12, TOP_Y0, TOP_W - 24, TOP_Y1 - TOP_Y0 - 20);
			else
				text_center(f->dir ? "Folder" : content_type_for(f->name), TOP_W / 2.0f, TOP_Y0 + 70,
				            SC_BODY, C_GREY, false);
			fit(crumb, sizeof(crumb), f->name, TOP_W - 40, SC_SMALL);
			text_center(crumb, TOP_W / 2.0f, TOP_Y1 - 14, SC_SMALL, C_GREY, false);
		}
		hint = strcmp(g_add.dir, "sdmc:/") ? GLYPH_A " Open   " GLYPH_B " Up"
		                                   : GLYPH_A " Open   " GLYPH_B " Back";
		break;
	case ADD_RECORD:
		text("Are.na / Add / ", 12, 7, 0.56f, C_GREY, true);
		text("Record", 12 + text_w("Are.na / Add / ", 0.56f), 7, 0.56f, C_WHITE, true);
		text_wrap("Speak into the microphone, just below the bottom screen. Up to two minutes.",
		          20, TOP_Y0 + 20, SC_BODY, C_GREY, TOP_W - 40);
		hint = mic_running() ? GLYPH_A " Stop" : GLYPH_A " Record   " GLYPH_B " Cancel";
		break;
	case ADD_VIS:
		text("Are.na / Add / ", 12, 7, 0.56f, C_GREY, true);
		text("Channel", 12 + text_w("Are.na / Add / ", 0.56f), 7, 0.56f, C_WHITE, true);
		{
			C2D_Text t;
			C2D_TextParse(&t, g_tb, g_add.draft.title);
			C2D_TextOptimize(&t);
			static const visibility V[3] = {VIS_PUBLIC, VIS_CLOSED, VIS_PRIVATE};
			C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap | C2D_AlignCenter, TOP_W / 2.0f, TOP_Y0 + 50,
			             g_z, 0.8f, 0.8f, vis_text(V[g_add.sel]), TOP_W - 60);
		}
		hint = g_add.draft.op == OP_EDIT_CHANNEL ? GLYPH_A " Save   " GLYPH_B " Cancel"
		                                         : GLYPH_A " Make channel   " GLYPH_B " Cancel";
		break;
	case ADD_ACTIONS:
	case ADD_CONFIRM:
		text("Are.na / ", 12, 7, 0.56f, C_GREY, true);
		fit(crumb, sizeof(crumb), g_add.ent.title, TOP_W - 24 - text_w("Are.na / ", 0.56f), 0.56f);
		text(crumb, 12 + text_w("Are.na / ", 0.56f), 7, 0.56f, C_WHITE, true);
		add_draw_draft(TOP_Y0);
		hint = g_add.step == ADD_CONFIRM ? GLYPH_A " Remove   " GLYPH_B " Cancel"
		                                 : GLYPH_A " Choose   " GLYPH_B " Close";
		break;
	case ADD_PICK:
		text("Are.na / ", 12, 7, 0.56f, C_GREY, true);
		text(g_add.draft.op == OP_CONNECT ? "Connect to" : "Choose a channel",
		     12 + text_w("Are.na / ", 0.56f), 7, 0.56f, C_WHITE, true);
		add_draw_draft(TOP_Y0);
		hint = g_add.draft.op == OP_CONNECT ? GLYPH_A " Connect here   " GLYPH_B " Cancel"
		                                    : GLYPH_A " Add here   " GLYPH_B " Cancel";
		break;
	case ADD_POST:
		text("Are.na / ", 12, 7, 0.56f, C_GREY, true);
		text(g_add.op == OP_ADD || g_add.op == OP_NEW_CHANNEL ? "Adding" : "Saving",
		     12 + text_w("Are.na / ", 0.56f), 7, 0.56f, C_WHITE, true);
		add_draw_draft(TOP_Y0);
		hint = "";
		break;
	default:
		break;
	}
	draw_hints(hint);
}

/* ------------------------------------------------------------ HOME */

static bool g_resume_cam, g_resume_cam_inner, g_resume_login, g_suspended;

static void on_suspend(void)
{
	if (g_suspended)
		return;
	g_suspended = true;
	store_lock();
	g_resume_cam = g_add.step == ADD_CAMERA;
	g_resume_cam_inner = cam_inner();
	/* A recording in progress just stops; what was caught is kept. */
	mic_stop();
	cam_stop();
	g_resume_login = cur()->type == SC_LOGIN && auth_status(NULL, 0) != AUTH_IDLE;
	store_unlock();
	if (g_resume_login)
		auth_cancel();
}

static void on_resume(void)
{
	if (!g_suspended)
		return;
	g_suspended = false;
	char err[96];
	store_lock();
	if (g_resume_cam && g_add.step == ADD_CAMERA && !cam_start(g_resume_cam_inner, err, sizeof(err)))
		snprintf(g_add.err, sizeof(g_add.err), "%s", err);
	store_unlock();
	/* The network may have dropped while away: open the login page anew. */
	if (g_resume_login && cur()->type == SC_LOGIN)
		auth_begin(err, sizeof(err));
	g_resume_cam = g_resume_login = false;
}

/* ------------------------------------------------------------ the app */

int main(void)
{
	romfsInit();
	gfxInitDefault();
	static u32 *soc_buf;
	soc_buf = memalign(0x1000, 0x100000);
	socInit(soc_buf, 0x100000);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	http_init();
	/* Network and decoding share the app core with drawing; let the
	 * system core take what it can. */
	APT_SetAppCpuTimeLimit(30);

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
	C2D_Prepare();
	g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	g_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	g_tb = C2D_TextBufNew(16384);
	g_mb = C2D_TextBufNew(4096);
	logo_init();

	auth_load();
	store_start();
	img_start();
	if (api_token[0])
		store_refresh_me();
	hidSetRepeatParameters(18, 5);

	aptHookCookie apt_cookie;
	aptHook(&apt_cookie, on_apt, NULL);
	touch_t tc = {0};
	u64 last = osGetTime();
	sc_type shown = SC_EXPLORE;
	int shown_tab = -1;

	while (aptMainLoop()) {
		u64 nowms = osGetTime();
		float dt = (nowms - last) / 1000.0f;
		last = nowms;
		g_time += dt;

		if (g_add.kb != KB_NONE)
			add_keyboard();

		hidScanInput();
		u32 down = hidKeysDown(), rep = hidKeysDownRepeat(), held = hidKeysHeld();

		store_lock();

		/* The + sheet takes the screens while it is open. */
		if (adding()) {
			mic_poll();
			add_input(down, rep, held);
			if (adding()) {
				if (g_add.pick)
					measure(g_add.pick);
				img_update();
				C2D_TextBufClear(g_tb);
				C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
				add_draw_top();
				add_draw_bottom();
				store_unlock();
				C3D_FrameEnd(0);
				continue;
			}
		}

		/* Signing in finished: start over with the account. */
		if (auth_status(NULL, 0) == AUTH_DONE) {
			auth_cancel();
			store_account.rejected = false;
			store_account.known = false;
			store_refresh_me();
		}
		/* Signed in but the account is not known yet (just logged in, or the
		 * first try failed): keep asking now and then. */
		static u64 me_asked;
		if (logged_in() && !store_account.known && nowms - me_asked > 5000) {
			me_asked = nowms;
			store_refresh_me();
		}
		build_roots();

		screen *s = cur();
		view *v = cur_view(s);
		if (v)
			measure(v);

		/* The login page runs only while it is on screen. */
		bool on_login = s->type == SC_LOGIN;
		if (on_login && (shown != SC_LOGIN || shown_tab != g_tab) && auth_status(NULL, 0) == AUTH_IDLE) {
			char err[96];
			auth_begin(err, sizeof(err));
		}
		if (!on_login && shown == SC_LOGIN)
			auth_cancel();
		shown = s->type;
		shown_tab = g_tab;

		if (down & KEY_L)
			switch_tab((g_tab + TABS - 1) % TABS);
		else if (down & KEY_R)
			switch_tab((g_tab + 1) % TABS);
		else if (down & KEY_B)
			pop();
		else if (down & KEY_A)
			activate(s, v);
		else if (down & KEY_Y && !v && s->me)
			store_refresh_me();
		else if (down & KEY_Y && v) {
			view_reload(v);
			if (s->v && s->v != v)
				s->v->want_detail = true;
			if (g_tab == T_NOTIF || g_tab == T_FEED)
				store_refresh_me();
		} else if (down & KEY_X) {
			if (s->type == SC_EXPLORE && v) {
				g_explore = (g_explore + 1) % EX_MODES;
				keep_visible(s, v);
			} else if (s->type == SC_NOTIF && v) {
				g_notif_feed = !g_notif_feed;
				v->rh_n = 0;           /* rows change height: measure again */
				measure(v);
				keep_visible(s, v);
			} else if (s->type == SC_BLOCK) {
				act_open(s);
			} else if (s->type == SC_CHANNEL) {
				act_start(s, A_CONNECT);
			} else if (v && v->sel >= 0 && v->sel < v->n && v->e[v->sel].kind == E_ACTIVITY &&
			           v->e[v->sel].target.kind != E_HEADER) {
				ref_t r = v->e[v->sel].target;
				open_ref(&r, NULL, NULL);
			}
		} else if (down & KEY_SELECT && s->type == SC_CHANNEL && s->v && s->v->det.can_update) {
			act_start(s, A_EDIT);
		} else if (down & KEY_SELECT && s->me && g_depth[g_tab] == 1) {
			auth_logout();
			store_account.known = false;
		} else {
			if (rep & KEY_UP) move_sel(s, v, 0, -1);
			if (rep & KEY_DOWN) move_sel(s, v, 0, 1);
			if (rep & KEY_LEFT) move_sel(s, v, -1, 0);
			if (rep & KEY_RIGHT) move_sel(s, v, 1, 0);
		}
		s = cur();
		v = cur_view(s);
		if (v)
			measure(v);
		select_first(v);

		/* Touch: the tab bar switches; a drag scrolls; a tap selects, and
		 * a tap on what is already selected opens it. */
		touchPosition tp;
		if (held & KEY_TOUCH) {
			hidTouchRead(&tp);
			if (down & KEY_TOUCH) {
				tc.dragging = false;
				if (tp.py >= CONTENT_H && logged_in()) {
					int slot = tp.px * (TABS + 1) / BOT_W;
					if (slot == 2)
						add_open();
					else
						switch_tab(slot < 2 ? slot : slot - 1);
				} else if (v && s->type == SC_CHANNEL && v->det.ok && logged_in() &&
				           channel_button_at(tp.px, tp.py, CHAN_BTN_Y - v->scroll, v->det.can_update)) {
					int b = channel_button_at(tp.px, tp.py, CHAN_BTN_Y - v->scroll, v->det.can_update);
					act_start(s, b == 1 ? A_CONNECT : A_EDIT);
				} else if (v && s->type == SC_BLOCK && v->rh_n > 0 &&
				           button_at(tp.px, tp.py, v->rh[0] - BTN_H - 10 - v->scroll)) {
					int b = button_at(tp.px, tp.py, v->rh[0] - BTN_H - 10 - v->scroll);
					act_open(s);
					if (b == 1 && adding() && g_add.acts[0] == A_CONNECT)
						act_choose(0);
				} else if (s->type == SC_USER && v && tp.py + v->scroll >= header_h(s) - 30 &&
				           tp.py + v->scroll < header_h(s)) {
					s->sub = tp.px < BOT_W / 2 ? 0 : 1;
				} else if (s->type == SC_LOGIN) {
					activate(s, v);
				} else if (v) {
					tc.dragging = true;
					tc.moved = false;
					tc.y0 = tp.py;
					tc.s0 = v->scroll;
					tc.item = item_at(s, v, tp.px, tp.py + v->scroll);
				}
			} else if (tc.dragging && v) {
				float dy = tp.py - tc.y0;
				if (fabsf(dy) > 6)
					tc.moved = true;
				if (tc.moved) {
					float max = content_h(s, v) - CONTENT_H;
					float sv = tc.s0 - dy;
					if (sv > max) sv = max;
					if (sv < 0) sv = 0;
					v->scroll = v->scroll_to = sv;
				}
			}
		} else if (tc.dragging) {
			tc.dragging = false;
			if (!tc.moved && v && tc.item >= 0 && tc.item < v->n) {
				if (tc.item == v->sel) {
					activate(s, v);
				} else {
					v->sel = tc.item;
					keep_visible(s, v);
				}
			}
		}

		s = cur();
		v = cur_view(s);
		/* A long list keeps a window of items: once past the limit, let go
		 * of those well above the screen, never what is in view. */
		if (v && v->req.kind != L_BLOCK && v->n > WINDOW_MAX) {
			int first = 0;
			if (is_grid(v)) {
				grid_t g = grid_of(v);
				int row = (int)((v->scroll - header_h(s) - g.gap) / (g.h + g.cap + g.gap));
				first = row > 0 ? row * g.cols : 0;
			} else {
				float y = header_h(s);
				while (first < v->rh_n && y + v->rh[first] < v->scroll) {
					y += v->rh[first];
					first++;
				}
			}
			int spare = first - WINDOW_STEP;               /* keep a little above */
			int over = v->n - WINDOW_MAX;
			int d = spare < over ? spare : over;
			d = (d / WINDOW_STEP) * WINDOW_STEP;
			if (d >= WINDOW_STEP)
				view_trim(v, d);
		}
		if (v && (v->drop_rows_items || v->drop_px)) {
			/* Items above were let go: move up with them so what is on
			 * screen stays put. */
			float shift = v->drop_px;
			if (is_grid(v)) {
				grid_t g = grid_of(v);
				shift = (v->drop_rows_items / g.cols) * (g.h + g.cap + g.gap);
			}
			v->scroll -= shift;
			v->scroll_to -= shift;
			tc.s0 -= shift;
			if (v->scroll < 0) v->scroll = 0;
			if (v->scroll_to < 0) v->scroll_to = 0;
			v->drop_rows_items = 0;
			v->drop_px = 0;
		}
		/* Back at the top of a list whose start was let go: load it again
		 * from the beginning. */
		if (v && v->dropped && v->scroll_to <= 0.5f && v->scroll <= 0.5f && !tc.dragging)
			view_reload(v);
		if (v) {
			measure(v);
			select_first(v);
			if (!tc.dragging)
				v->scroll += (v->scroll_to - v->scroll) * fminf(dt * 14.0f, 1.0f);
			/* Near the end: the next page. */
			int ahead = is_grid(v) ? grid_of(v).cols * 3 : 4;
			if (v->n == 0 || v->sel >= v->n - ahead ||
			    v->scroll + CONTENT_H > content_h(s, v) - 120)
				view_more(v);
		}

		img_update();
		C2D_TextBufClear(g_tb);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		draw_top();
		draw_bottom();
		store_unlock();
		C3D_FrameEnd(0);
	}

	store_lock();
	if (adding())
		add_close();
	store_unlock();
	aptUnhook(&apt_cookie);
	/* Leaving must never hang: cut every transfer short, and if a thread
	 * still has not let go, skip tearing down what it is using -- the
	 * system reclaims it all when the app ends anyway. */
	http_abort_all = true;
	auth_cancel();
	bool clean = store_stop();
	clean = img_stop() && clean;
	C2D_TextBufDelete(g_tb);
	C2D_TextBufDelete(g_mb);
	C2D_Fini();
	C3D_Fini();
	if (clean) {
		http_exit();
		curl_global_cleanup();
		socExit();
	}
	gfxExit();
	romfsExit();
	return 0;
}

#include "img.h"

#include <3ds.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

#define SLOTS    112
#define WORKERS  2
#define URL_MAX  480
#define TEX_MAX  512

typedef enum { S_FREE, S_QUEUED, S_LOADING, S_DECODED, S_READY, S_FAILED } slot_state;

typedef struct {
	char       url[URL_MAX];
	u32        hash;
	slot_state st;
	u32        used;        /* frame it was last asked for */
	u64        failed_at;
	int        w, h, tw, th;
	u16       *pixels;      /* swizzled RGB565, from a worker */
	C3D_Tex    tex;
	Tex3DS_SubTexture sub;
	C2D_Image  img;
} slot;

static slot          s_slot[SLOTS];
static LightLock     s_lock;
static LightEvent    s_work;
static Thread        s_thread[WORKERS];
static volatile bool s_quit;
static u32           s_frame = 1;

static u32 hash_str(const char *s)
{
	u32 h = 2166136261u;
	while (*s)
		h = (h ^ (u8)*s++) * 16777619u;
	return h;
}

static int pow2(int v)
{
	int p = 8;
	while (p < v)
		p <<= 1;
	return p;
}

/* ------------------------------------------------------------ decoding */

typedef struct {
	struct jpeg_error_mgr pub;
	jmp_buf               jb;
} jerr_t;

static void on_jpeg_error(j_common_ptr ci)
{
	longjmp(((jerr_t *)ci->err)->jb, 1);
}

/* PICA textures are laid out in 8x8 tiles, Morton order inside each. */
static inline u32 swizzle(int x, int y, int tw)
{
	return ((((y >> 3) * (tw >> 3) + (x >> 3)) << 6) |
	        ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
	         ((x & 4) << 2) | ((y & 4) << 3)));
}

static u16 *decode_jpeg(const unsigned char *data, size_t len, int *ow, int *oh, int *otw, int *oth)
{
	struct jpeg_decompress_struct ci;
	jerr_t je;
	u16 *out = NULL;
	JSAMPLE *row = NULL;
	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = on_jpeg_error;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		free(out);
		free(row);
		return NULL;
	}
	jpeg_create_decompress(&ci);
	jpeg_mem_src(&ci, (unsigned char *)data, len);
	jpeg_read_header(&ci, TRUE);
	ci.out_color_space = JCS_RGB;
	/* Let the decoder shrink anything too big for a texture. */
	ci.scale_num = 1;
	ci.scale_denom = 1;
	while ((ci.image_width / ci.scale_denom > TEX_MAX || ci.image_height / ci.scale_denom > TEX_MAX) &&
	       ci.scale_denom < 8)
		ci.scale_denom <<= 1;
	jpeg_start_decompress(&ci);
	int w = ci.output_width, h = ci.output_height;
	if (w > TEX_MAX) w = TEX_MAX;
	if (h > TEX_MAX) h = TEX_MAX;
	int tw = pow2(w), th = pow2(h);
	out = calloc((size_t)tw * th, sizeof(u16));
	row = malloc((size_t)ci.output_width * 3);
	if (!out || !row)
		longjmp(je.jb, 1);
	while (ci.output_scanline < ci.output_height) {
		int y = ci.output_scanline;
		JSAMPROW rp = row;
		jpeg_read_scanlines(&ci, &rp, 1);
		if (y >= h)
			continue;
		const JSAMPLE *p = row;
		for (int x = 0; x < w; x++, p += 3)
			out[swizzle(x, y, tw)] = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	free(row);
	*ow = w; *oh = h; *otw = tw; *oth = th;
	return out;
}

/* ------------------------------------------------------------ workers */

static void worker(void *arg)
{
	(void)arg;
	while (!s_quit) {
		/* The image asked for most recently goes first: it is the one on
		 * screen now, not one scrolled past a second ago. */
		LightLock_Lock(&s_lock);
		int best = -1;
		for (int i = 0; i < SLOTS; i++)
			if (s_slot[i].st == S_QUEUED && (best < 0 || s_slot[i].used > s_slot[best].used))
				best = i;
		if (best < 0) {
			LightLock_Unlock(&s_lock);
			LightEvent_WaitTimeout(&s_work, 500000000LL);
			continue;
		}
		slot *s = &s_slot[best];
		s->st = S_LOADING;
		char url[URL_MAX];
		memcpy(url, s->url, sizeof(url));
		LightLock_Unlock(&s_lock);

		char *body = NULL;
		size_t len = 0;
		u16 *px = NULL;
		int w = 0, h = 0, tw = 0, th = 0;
		int got = -1;
		if (!strncmp(url, "sdmc:/", 6)) {
			/* A photo on the SD card: same decoder, no network. */
			FILE *f = fopen(url, "rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				long sz = ftell(f);
				fseek(f, 0, SEEK_SET);
				if (sz > 0 && sz < 8 * 1024 * 1024 && (body = malloc((size_t)sz))) {
					len = fread(body, 1, (size_t)sz, f);
					got = len == (size_t)sz ? 0 : -1;
				}
				fclose(f);
			}
		} else {
			got = http_get(url, &body, &len);
		}
		if (got != 0) {
			free(body);
			body = NULL;
		}
		if (got == 0) {
			px = decode_jpeg((unsigned char *)body, len, &w, &h, &tw, &th);
			free(body);
		}

		LightLock_Lock(&s_lock);
		/* The slot may have been given to another URL meanwhile. */
		if (s->st == S_LOADING && !strcmp(s->url, url)) {
			if (px) {
				s->pixels = px;
				s->w = w; s->h = h; s->tw = tw; s->th = th;
				s->st = S_DECODED;
				px = NULL;
			} else {
				s->st = S_FAILED;
				s->failed_at = osGetTime();
			}
		}
		LightLock_Unlock(&s_lock);
		free(px);
	}
}

void img_start(void)
{
	LightLock_Init(&s_lock);
	LightEvent_Init(&s_work, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	for (int i = 0; i < WORKERS; i++)
		s_thread[i] = threadCreate(worker, NULL, 64 * 1024, prio + 2, -2, false);
}

bool img_stop(void)
{
	s_quit = true;
	bool clean = true;
	for (int i = 0; i < WORKERS; i++) {
		LightEvent_Signal(&s_work);
		if (!s_thread[i])
			continue;
		if (R_FAILED(threadJoin(s_thread[i], 2000000000ULL))) {
			clean = false;
			continue;
		}
		threadFree(s_thread[i]);
		s_thread[i] = NULL;
	}
	if (!clean)
		return false;
	for (int i = 0; i < SLOTS; i++) {
		if (s_slot[i].st == S_READY)
			C3D_TexDelete(&s_slot[i].tex);
		free(s_slot[i].pixels);
	}
	return true;
}

/* ------------------------------------------------------------ cache */

static slot *find(const char *url, u32 h)
{
	for (int i = 0; i < SLOTS; i++)
		if (s_slot[i].st != S_FREE && s_slot[i].hash == h && !strcmp(s_slot[i].url, url))
			return &s_slot[i];
	return NULL;
}

static void release(slot *s)
{
	if (s->st == S_READY)
		C3D_TexDelete(&s->tex);
	free(s->pixels);
	s->pixels = NULL;
	s->st = S_FREE;
}

/* A free slot, or the one least recently asked for -- never one that is
 * on screen this frame or that a worker is filling. */
static slot *take(void)
{
	slot *best = NULL;
	for (int i = 0; i < SLOTS; i++) {
		slot *s = &s_slot[i];
		if (s->st == S_FREE)
			return s;
		if (s->st == S_LOADING || s->used + 1 >= s_frame)
			continue;
		if (!best || s->used < best->used)
			best = s;
	}
	if (best)
		release(best);
	return best;
}

const C2D_Image *img_get(const char *url, int *w, int *h)
{
	if (!url || !url[0] || strlen(url) >= URL_MAX)
		return NULL;
	u32 hs = hash_str(url);
	LightLock_Lock(&s_lock);
	slot *s = find(url, hs);
	if (!s) {
		s = take();
		if (s) {
			strcpy(s->url, url);
			s->hash = hs;
			s->st = S_QUEUED;
			LightEvent_Signal(&s_work);
		}
	} else if (s->st == S_FAILED && osGetTime() - s->failed_at > 20000) {
		s->st = S_QUEUED;     /* try again now and then */
		LightEvent_Signal(&s_work);
	}
	const C2D_Image *img = NULL;
	if (s) {
		s->used = s_frame;
		if (s->st == S_READY) {
			img = &s->img;
			if (w) *w = s->w;
			if (h) *h = s->h;
		}
	}
	LightLock_Unlock(&s_lock);
	return img;
}

bool img_failed(const char *url)
{
	if (!url || !url[0])
		return true;
	LightLock_Lock(&s_lock);
	slot *s = find(url, hash_str(url));
	bool f = s && s->st == S_FAILED;
	LightLock_Unlock(&s_lock);
	return f;
}

void img_update(void)
{
	LightLock_Lock(&s_lock);
	s_frame++;
	int uploads = 0;
	for (int i = 0; i < SLOTS && uploads < 6; i++) {
		slot *s = &s_slot[i];
		if (s->st != S_DECODED)
			continue;
		if (!C3D_TexInit(&s->tex, (u16)s->tw, (u16)s->th, GPU_RGB565)) {
			free(s->pixels);
			s->pixels = NULL;
			s->st = S_FAILED;
			s->failed_at = osGetTime();
			continue;
		}
		memcpy(s->tex.data, s->pixels, (size_t)s->tw * s->th * sizeof(u16));
		C3D_TexFlush(&s->tex);
		C3D_TexSetFilter(&s->tex, GPU_LINEAR, GPU_LINEAR);
		free(s->pixels);
		s->pixels = NULL;
		s->sub = (Tex3DS_SubTexture){
			(u16)s->w, (u16)s->h, 0.0f, 1.0f,
			(float)s->w / s->tw, 1.0f - (float)s->h / s->th,
		};
		s->img = (C2D_Image){&s->tex, &s->sub};
		s->st = S_READY;
		uploads++;
	}
	LightLock_Unlock(&s_lock);
}

#include "add.h"

#include <dirent.h>
#include <stdio.h>
#include <jpeglib.h>
#include <malloc.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ------------------------------------------------------------ camera */

/* A worker thread waits on the camera so the UI never does; it fills one
 * buffer while the UI reads the other. */
static Thread        s_cam_thread;
static volatile bool s_cam_quit;
static bool          s_cam_on, s_cam_inner;
static u16          *s_cam_buf[2];
static volatile int  s_cam_ready = -1;    /* index of the newest full buffer */
static int           s_cam_reading = -1;
static LightLock     s_cam_lock;
static u32           s_cam_unit;

static void cam_worker(void *arg)
{
	(void)arg;
	int fill = 0;
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);
	while (!s_cam_quit) {
		LightLock_Lock(&s_cam_lock);
		if (fill == s_cam_reading)
			fill ^= 1;
		LightLock_Unlock(&s_cam_lock);
		Handle ev = 0;
		if (R_FAILED(CAMU_SetReceiving(&ev, s_cam_buf[fill], PORT_CAM1, CAM_W * CAM_H * 2,
		                               (s16)s_cam_unit))) {
			svcSleepThread(50000000LL);
			continue;
		}
		Result r = svcWaitSynchronization(ev, 300000000LL);
		svcCloseHandle(ev);
		if (R_FAILED(r) || r == (Result)0x09401BFE) {    /* timed out: start again */
			CAMU_StopCapture(PORT_CAM1);
			CAMU_ClearBuffer(PORT_CAM1);
			CAMU_StartCapture(PORT_CAM1);
			continue;
		}
		LightLock_Lock(&s_cam_lock);
		s_cam_ready = fill;
		LightLock_Unlock(&s_cam_lock);
		fill ^= 1;
	}
	CAMU_StopCapture(PORT_CAM1);
}

bool cam_start(bool inner, char *err, size_t errcap)
{
	cam_stop();
	LightLock_Init(&s_cam_lock);
	for (int i = 0; i < 2; i++) {
		s_cam_buf[i] = linearAlloc(CAM_W * CAM_H * 2);
		if (!s_cam_buf[i]) {
			snprintf(err, errcap, "Not enough memory for the camera");
			cam_stop();
			return false;
		}
	}
	if (R_FAILED(camInit())) {
		snprintf(err, errcap, "The camera is not available");
		cam_stop();
		return false;
	}
	s_cam_on = true;
	s_cam_inner = inner;
	u32 sel = inner ? SELECT_IN1 : SELECT_OUT1;
	CAMU_SetSize(sel, SIZE_VGA, CONTEXT_A);
	CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(sel, FRAME_RATE_15);
	CAMU_SetNoiseFilter(sel, true);
	CAMU_SetAutoExposure(sel, true);
	CAMU_SetAutoWhiteBalance(sel, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	CAMU_GetMaxBytes(&s_cam_unit, CAM_W, CAM_H);
	CAMU_SetTransferBytes(PORT_CAM1, s_cam_unit, CAM_W, CAM_H);
	CAMU_Activate(sel);
	s_cam_ready = s_cam_reading = -1;
	s_cam_quit = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_cam_thread = threadCreate(cam_worker, NULL, 16 * 1024, prio - 1, -2, false);
	return true;
}

void cam_stop(void)
{
	if (s_cam_thread) {
		s_cam_quit = true;
		threadJoin(s_cam_thread, 2000000000ULL);
		threadFree(s_cam_thread);
		s_cam_thread = NULL;
	}
	if (s_cam_on) {
		CAMU_Activate(SELECT_NONE);
		camExit();
		s_cam_on = false;
	}
	for (int i = 0; i < 2; i++) {
		if (s_cam_buf[i])
			linearFree(s_cam_buf[i]);
		s_cam_buf[i] = NULL;
	}
}

const u16 *cam_frame(void)
{
	if (!s_cam_on)
		return NULL;
	LightLock_Lock(&s_cam_lock);
	int r = s_cam_ready;
	if (r >= 0) {
		s_cam_reading = r;
		s_cam_ready = -1;
	}
	LightLock_Unlock(&s_cam_lock);
	return r >= 0 ? s_cam_buf[r] : NULL;
}

bool cam_inner(void) { return s_cam_inner; }

void cam_shutter(void)
{
	if (s_cam_on)
		CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);
}

/* ------------------------------------------------------------ JPEG */

typedef struct {
	struct jpeg_error_mgr pub;
	jmp_buf               jb;
} jerr_t;

static void on_jerr(j_common_ptr ci)
{
	longjmp(((jerr_t *)ci->err)->jb, 1);
}

unsigned char *jpeg_encode_565(const u16 *px, int w, int h, int quality, size_t *len)
{
	struct jpeg_compress_struct ci;
	jerr_t je;
	unsigned char *out = NULL;
	unsigned long outlen = 0;
	JSAMPLE *row = malloc((size_t)w * 3);
	if (!row)
		return NULL;
	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = on_jerr;
	if (setjmp(je.jb)) {
		jpeg_destroy_compress(&ci);
		free(row);
		free(out);
		return NULL;
	}
	jpeg_create_compress(&ci);
	jpeg_mem_dest(&ci, &out, &outlen);
	ci.image_width = (JDIMENSION)w;
	ci.image_height = (JDIMENSION)h;
	ci.input_components = 3;
	ci.in_color_space = JCS_RGB;
	jpeg_set_defaults(&ci);
	jpeg_set_quality(&ci, quality, TRUE);
	jpeg_start_compress(&ci, TRUE);
	while (ci.next_scanline < ci.image_height) {
		const u16 *p = px + (size_t)ci.next_scanline * w;
		for (int x = 0; x < w; x++) {
			u16 v = p[x];
			u8 r = (u8)(v >> 11), g = (u8)(v >> 5 & 63), b = (u8)(v & 31);
			row[x * 3] = (u8)(r << 3 | r >> 2);
			row[x * 3 + 1] = (u8)(g << 2 | g >> 4);
			row[x * 3 + 2] = (u8)(b << 3 | b >> 2);
		}
		JSAMPROW rp = row;
		jpeg_write_scanlines(&ci, &rp, 1);
	}
	jpeg_finish_compress(&ci);
	jpeg_destroy_compress(&ci);
	free(row);
	*len = outlen;
	return out;
}

/* ------------------------------------------------------------ microphone */

#define MIC_RING 0x30000

static u8    *s_mic_ring;
static u32    s_mic_size, s_mic_pos;
static bool   s_mic_on;
static s16   *s_rec;
static size_t s_rec_n, s_rec_cap;
static float  s_level;

bool mic_start(char *err, size_t errcap)
{
	mic_stop();
	mic_discard();
	s_mic_ring = memalign(0x1000, MIC_RING);
	if (!s_mic_ring || R_FAILED(micInit(s_mic_ring, MIC_RING))) {
		free(s_mic_ring);
		s_mic_ring = NULL;
		snprintf(err, errcap, "The microphone is not available");
		return false;
	}
	s_mic_size = micGetSampleDataSize();
	s_rec_cap = (size_t)MIC_RATE * MIC_MAX_SECS;
	s_rec = malloc(s_rec_cap * sizeof(s16));
	if (!s_rec) {
		micExit();
		free(s_mic_ring);
		s_mic_ring = NULL;
		snprintf(err, errcap, "Not enough memory to record");
		return false;
	}
	MICU_SetGain(0x40);
	MICU_SetPower(true);
	if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0,
	                                s_mic_size, true))) {
		mic_stop();
		mic_discard();
		snprintf(err, errcap, "Could not start recording");
		return false;
	}
	s_mic_pos = 0;
	s_mic_on = true;
	return true;
}

void mic_poll(void)
{
	if (!s_mic_on)
		return;
	u32 end = micGetLastSampleOffset();
	float peak = 0;
	/* The ring is read in 2-byte samples up to where the hardware is. */
	while (s_mic_pos != end) {
		s16 v = *(s16 *)(s_mic_ring + s_mic_pos);
		if (s_rec_n < s_rec_cap)
			s_rec[s_rec_n++] = v;
		float a = (v < 0 ? -v : v) / 32768.0f;
		if (a > peak)
			peak = a;
		s_mic_pos += 2;
		if (s_mic_pos >= s_mic_size)
			s_mic_pos = 0;
	}
	s_level = peak > s_level ? peak : s_level * 0.9f;
	if (s_rec_n >= s_rec_cap)
		mic_stop();
}

void mic_stop(void)
{
	if (s_mic_on) {
		MICU_StopSampling();
		s_mic_on = false;
	}
	if (s_mic_ring) {
		micExit();
		free(s_mic_ring);
		s_mic_ring = NULL;
	}
	s_level = 0;
}

bool mic_running(void) { return s_mic_on; }
float mic_seconds(void) { return (float)s_rec_n / MIC_RATE; }
float mic_level(void) { return s_level; }

static void put32(unsigned char *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(unsigned char *p, u16 v) { p[0] = v; p[1] = v >> 8; }

unsigned char *mic_wav(size_t *len)
{
	if (!s_rec || !s_rec_n)
		return NULL;
	size_t data = s_rec_n * 2;
	unsigned char *w = malloc(44 + data);
	if (!w)
		return NULL;
	memcpy(w, "RIFF", 4);
	put32(w + 4, (u32)(36 + data));
	memcpy(w + 8, "WAVEfmt ", 8);
	put32(w + 16, 16);
	put16(w + 20, 1);            /* PCM */
	put16(w + 22, 1);            /* mono */
	put32(w + 24, MIC_RATE);
	put32(w + 28, MIC_RATE * 2);
	put16(w + 32, 2);
	put16(w + 34, 16);
	memcpy(w + 36, "data", 4);
	put32(w + 40, (u32)data);
	memcpy(w + 44, s_rec, data);
	*len = 44 + data;
	return w;
}

void mic_discard(void)
{
	free(s_rec);
	s_rec = NULL;
	s_rec_n = s_rec_cap = 0;
}

/* ------------------------------------------------------------ files */

static int by_name(const void *a, const void *b)
{
	const file_ent *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

int list_dir(const char *path, file_ent *out, int cap)
{
	DIR *d = opendir(path);
	if (!d)
		return -1;
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) && n < cap) {
		if (de->d_name[0] == '.')
			continue;
		file_ent *f = &out[n];
		snprintf(f->name, sizeof(f->name), "%s", de->d_name);
		char full[512];
		snprintf(full, sizeof(full), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", de->d_name);
		struct stat st;
		f->dir = de->d_type == DT_DIR;
		f->size = 0;
		if (!f->dir && stat(full, &st) == 0)
			f->size = (long)st.st_size;
		n++;
	}
	closedir(d);
	qsort(out, (size_t)n, sizeof(file_ent), by_name);
	return n;
}

static const char *ext_of(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot ? dot + 1 : "";
}

const char *content_type_for(const char *name)
{
	static const struct { const char *ext, *type; } T[] = {
		{"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"mpo", "image/jpeg"},
		{"png", "image/png"}, {"gif", "image/gif"}, {"bmp", "image/bmp"},
		{"wav", "audio/wav"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"ogg", "audio/ogg"},
		{"mp4", "video/mp4"}, {"pdf", "application/pdf"}, {"txt", "text/plain"},
	};
	const char *e = ext_of(name);
	for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
		if (!strcasecmp(e, T[i].ext))
			return T[i].type;
	return "application/octet-stream";
}

bool is_image_name(const char *name)
{
	const char *e = ext_of(name);
	/* The decoder here reads JPEG; a 3D photo (.MPO) starts with one. */
	return !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "mpo");
}

unsigned char *read_file(const char *path, size_t max, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = NULL;
	if (sz > 0 && (size_t)sz <= max && (b = malloc((size_t)sz))) {
		if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
			free(b);
			b = NULL;
		}
	}
	fclose(f);
	if (b)
		*len = (size_t)sz;
	return b;
}

/* ------------------------------------------------------------ posting */

static Thread              s_post_thread;
static volatile post_state s_post_state;
static char                s_post_msg[128], s_post_slug[80];
static post_req            s_req;
static LightLock           s_post_lock;
static bool                s_post_lock_ok;

static void set_post(post_state st, const char *msg)
{
	LightLock_Lock(&s_post_lock);
	s_post_state = st;
	snprintf(s_post_msg, sizeof(s_post_msg), "%s", msg);
	LightLock_Unlock(&s_post_lock);
}

static void post_worker(void *arg)
{
	(void)arg;
	char err[160] = "", msg[200] = "";
	int rc = 0;
	post_req *r = &s_req;
	switch (r->op) {
	case OP_NEW_CHANNEL: {
		set_post(POST_SAVING, "Making the channel...");
		char slug[80] = "";
		rc = api_create_channel(r->title, r->vis, slug, sizeof(slug), NULL, err, sizeof(err));
		if (rc == 0) {
			LightLock_Lock(&s_post_lock);
			snprintf(s_post_slug, sizeof(s_post_slug), "%s", slug);
			LightLock_Unlock(&s_post_lock);
			snprintf(msg, sizeof(msg), "Made %s", r->title);
		}
		break;
	}
	case OP_ADD: {
		char url[512];
		const char *value = r->text;
		if (r->data) {
			set_post(POST_UPLOADING, "Uploading...");
			rc = api_upload(r->data, r->len, r->filename, r->ctype, url, sizeof(url), err,
			                sizeof(err), NULL);
			value = url;
		}
		if (rc == 0) {
			set_post(POST_SAVING, "Adding the block...");
			rc = api_create_block(value, r->title, r->channel_id, err, sizeof(err));
		}
		snprintf(msg, sizeof(msg), "Added to %s", r->channel);
		break;
	}
	case OP_CONNECT:
		set_post(POST_SAVING, "Connecting...");
		rc = api_connect(r->target_id, r->target_is_channel, r->channel_id, err, sizeof(err));
		snprintf(msg, sizeof(msg), "Connected to %s", r->channel);
		break;
	case OP_REMOVE:
		set_post(POST_SAVING, "Removing...");
		rc = api_disconnect(r->conn_id, err, sizeof(err));
		snprintf(msg, sizeof(msg), "Removed from %s", r->channel);
		break;
	case OP_EDIT_BLOCK:
		set_post(POST_SAVING, "Saving...");
		rc = api_update_block(r->target_id, r->target_is_text, r->title, r->text, err, sizeof(err));
		snprintf(msg, sizeof(msg), "Saved");
		break;
	case OP_EDIT_CHANNEL:
		set_post(POST_SAVING, "Saving...");
		rc = api_update_channel(r->target_id, r->title, r->vis, err, sizeof(err));
		snprintf(msg, sizeof(msg), "Saved");
		break;
	}
	free(r->text);
	free(r->data);
	r->text = NULL;
	r->data = NULL;
	set_post(rc == 0 ? POST_DONE : POST_FAILED, rc == 0 ? msg : err);
}

void post_start(post_req *req)
{
	if (!s_post_lock_ok) {
		LightLock_Init(&s_post_lock);
		s_post_lock_ok = true;
	}
	post_reset();
	s_req = *req;
	memset(req, 0, sizeof(*req));
	s_post_slug[0] = '\0';
	set_post(POST_UPLOADING, "Starting...");
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_post_thread = threadCreate(post_worker, NULL, 64 * 1024, prio + 1, -2, false);
	if (!s_post_thread) {
		free(s_req.text);
		free(s_req.data);
		s_req.text = NULL;
		s_req.data = NULL;
		set_post(POST_FAILED, "Could not start");
	}
}

post_state post_status(char *msg, size_t cap, char *slug, size_t slugcap)
{
	if (!s_post_lock_ok)
		return POST_IDLE;
	LightLock_Lock(&s_post_lock);
	post_state st = s_post_state;
	if (msg)
		snprintf(msg, cap, "%s", s_post_msg);
	if (slug)
		snprintf(slug, slugcap, "%s", s_post_slug);
	LightLock_Unlock(&s_post_lock);
	return st;
}

void post_reset(void)
{
	if (s_post_thread) {
		threadJoin(s_post_thread, U64_MAX);
		threadFree(s_post_thread);
		s_post_thread = NULL;
	}
	if (s_post_lock_ok)
		set_post(POST_IDLE, "");
}

/* Making things: the camera, the microphone, files on the SD card, and a
 * background job that uploads what was made and adds it to a channel. */
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#include "api.h"

/* ---- camera: 640x480, RGB565 ---- */

#define CAM_W 640
#define CAM_H 480

bool cam_start(bool inner, char *err, size_t errcap);
void cam_stop(void);
/* The newest frame, or NULL if none has come since the last call. */
const u16 *cam_frame(void);
bool cam_inner(void);
void cam_shutter(void);

/* RGB565 pixels to a JPEG in a heap buffer. */
unsigned char *jpeg_encode_565(const u16 *px, int w, int h, int quality, size_t *len);

/* ---- microphone: 16-bit mono at ~16.4 kHz ---- */

#define MIC_RATE     16364
#define MIC_MAX_SECS 120

bool mic_start(char *err, size_t errcap);
/* Call every frame while recording: moves new samples out of the ring. */
void mic_poll(void);
void mic_stop(void);
bool mic_running(void);
float mic_seconds(void);
float mic_level(void);       /* 0..1, recent loudness */
/* The recording as a WAV file in a heap buffer (the recording is kept). */
unsigned char *mic_wav(size_t *len);
void mic_discard(void);

/* ---- files ---- */

typedef struct {
	char name[256];
	bool dir;
	long size;
} file_ent;

/* Sorted, folders first; returns the count (at most cap), -1 on error. */
int list_dir(const char *path, file_ent *out, int cap);
const char *content_type_for(const char *name);
bool is_image_name(const char *name);
/* The whole file; NULL if it is missing or larger than max. */
unsigned char *read_file(const char *path, size_t max, size_t *len);

/* ---- posting ---- */

typedef enum { POST_IDLE, POST_UPLOADING, POST_SAVING, POST_DONE, POST_FAILED } post_state;

typedef enum {
	OP_ADD,            /* text or data, as a new block in channel_id */
	OP_NEW_CHANNEL,    /* title, vis */
	OP_CONNECT,        /* target into channel_id */
	OP_REMOVE,         /* connection conn_id, from channel */
	OP_EDIT_BLOCK,     /* target: title, and text as its body */
	OP_EDIT_CHANNEL,   /* target: title, vis */
} post_op;

typedef struct {
	post_op        op;
	int            target_id;
	bool           target_is_channel, target_is_text;
	int            conn_id;
	/* OP_ADD: exactly one of text or data (a file) */
	char          *text;
	unsigned char *data;
	size_t         len;
	char           filename[96];
	char           ctype[48];
	char           title[128];       /* optional block title */
	int            channel_id;
	char           channel[160];     /* its title, for the message */
	visibility     vis;
} post_req;

/* Takes ownership of req's heap buffers. */
void post_start(post_req *req);
post_state post_status(char *msg, size_t cap, char *slug, size_t slugcap);
void post_reset(void);

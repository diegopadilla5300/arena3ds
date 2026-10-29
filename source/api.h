/* The Are.na API: what a screen shows, as a flat list of entries, fetched
 * a page at a time on a background thread. */
#pragma once

#include <stdbool.h>
#include <time.h>

#define THUMB_PX 72          /* grid tiles on the bottom screen */
#define BIG_W    400         /* the top screen, below the breadcrumb */
#define BIG_H    210

typedef enum {
	E_BLOCK,      /* Image, Text, Link, Embed, Attachment */
	E_CHANNEL,
	E_USER,       /* or group */
	E_ACTIVITY,   /* a feed item or notification */
	E_SECTION,    /* label above a paragraph (block info) */
	E_KV,         /* label: value on one line (block info) */
	E_HEADER,     /* a small heading between parts of a list */
} entry_kind;

typedef enum { B_IMAGE, B_TEXT, B_LINK, B_EMBED, B_ATTACH, B_OTHER } block_type;
typedef enum { VIS_CLOSED, VIS_PUBLIC, VIS_PRIVATE } visibility;

/* What an activity is about, enough to draw it and open it. */
typedef struct {
	entry_kind kind;        /* E_BLOCK, E_CHANNEL or E_USER; E_HEADER = none */
	block_type btype;
	visibility vis;
	int        id;
	char       slug[80];
	char       title[128];
	char       initials[4];
} ref_t;

typedef struct {
	entry_kind kind;
	block_type btype;
	visibility vis;
	int        id;
	char       slug[80];
	char       title[160];
	char       owner[64];       /* "by ..." / connected by / the actor */
	char       initials[4];
	int        count;           /* blocks in a channel */
	time_t     when;            /* updated / connected / happened */
	time_t     created;
	char       thumb[480];      /* THUMB_PX square */
	char       big[480];        /* fits BIG_W x BIG_H */
	char      *body;            /* text content, description, value: heap */
	char      *md;              /* the same as markdown, for editing: heap */
	bool       has_title;       /* title is the block's own, not a stand-in */
	bool       can_add;         /* E_CHANNEL: you may add blocks to it */
	bool       can_manage;      /* you may edit it */
	int        conn_id;         /* its connection to the channel it was listed in */
	bool       can_remove;      /* ...and you may undo that connection */
	char       conn_by[80];     /* who made that connection (slug) */
	/* E_ACTIVITY */
	char       verb[40];
	ref_t      item, target;
	bool       unread;
} entry;

typedef enum {
	L_FEED, L_NOTIFICATIONS, L_EXPLORE,
	L_CHANNEL,          /* contents of slug */
	L_USER_CHANNELS,    /* channels of user slug */
	L_USER_BLOCKS,      /* blocks of user slug */
	L_BLOCK,            /* info and connections of block id */
} list_kind;

typedef struct {
	list_kind kind;
	char      slug[80];
	int       id;
	/* paging: page number or an opaque cursor */
	int       page;
	char      cursor[96];
	bool      more;
} list_req;

/* Fetch the next page of req into a new array; req's paging moves on.
 * Returns the number of entries (0 at the end), or -1 on error with a
 * short reason in err. Blocking: call from a worker. */
int api_fetch(list_req *req, entry **out, char *err, size_t errcap);

/* Details of one channel or user, for the header above its list. */
typedef struct {
	bool   ok;
	char   title[160];
	char   owner[64];
	char   initials[4];
	visibility vis;
	int    id;
	int    length, followers, channels, following, blocks;
	bool   can_update, can_add;
	time_t created, updated;
	char   description[600];
} detail_t;

int api_channel(const char *slug, detail_t *out);
int api_user(const char *slug, detail_t *out);

/* Up to n square thumbnails from the start of a channel, for previews. */
int api_channel_peek(const char *slug, char urls[][480], int n);

/* The signed-in account; false if the token was refused. */
typedef struct {
	char slug[80];
	char name[64];
	char initials[4];
	int  unread;
} me_t;
bool api_me(me_t *me, bool *unauthorized);

/* The token every request carries; empty when signed out. */
extern char api_token[128];

void entry_free_all(entry *e, int n);

/* ---- adding: all need a token with the write scope ---- */

/* A short reason when one of these fails; "forbidden" means the login
 * can only read. */
#define API_ERR_READONLY "Your login can only read"

/* Upload bytes; *out_url gets the address a block can be made from. */
int api_upload(const void *data, size_t len, const char *filename, const char *ctype,
               char *out_url, size_t cap, char *err, size_t errcap,
               volatile float *progress);
/* A block from text or a URL, added to a channel. */
int api_create_block(const char *value, const char *title, int channel_id,
                     char *err, size_t errcap);
/* A new channel; slug and id of it come back. */
int api_create_channel(const char *title, visibility vis, char *slug, size_t slugcap,
                       int *id, char *err, size_t errcap);
/* An existing block or channel into one of your channels. */
int api_connect(int id, bool is_channel, int channel_id, char *err, size_t errcap);
/* Take it out of the channel again (the block itself stays). */
int api_disconnect(int connection_id, char *err, size_t errcap);
/* NULL leaves a field as it is. body is the text of a Text block and
 * the description of anything else. */
int api_update_block(int id, bool is_text, const char *title, const char *body,
                     char *err, size_t errcap);
int api_update_channel(int id, const char *title, visibility vis, char *err, size_t errcap);

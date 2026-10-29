/* Screens' data, filled in by a background thread. The main thread holds
 * store_lock() while it reads or changes any view. */
#pragma once

#include <3ds.h>

#include "api.h"

#define PEEK_N 4

typedef struct {
	bool     used;
	u32      gen;           /* bumped when the view is freed or reloaded */
	list_req req;
	entry   *e;
	int      n, cap;
	bool     loading, want_more, failed;
	char     err[80];
	/* channel or user header */
	bool     want_detail, detail_loading;
	detail_t det;
	/* where the cursor is */
	int      sel;
	float    scroll, scroll_to;
	/* row heights, measured by the UI as rows arrive */
	float   *rh;
	int      rh_n;
	/* Long lists keep a window of items: the oldest are let go as new
	 * pages come. dropped counts all let go; the UI shifts its scroll by
	 * drop_rows rows (grids) or drop_px pixels (lists), then clears them. */
	int      dropped, drop_rows_items;
	float    drop_px;
} view;

#define WINDOW_MAX  240
#define WINDOW_STEP 12     /* trims come in multiples of every grid's columns */

void store_start(void);
bool store_stop(void);
void store_lock(void);
void store_unlock(void);

view *view_new(list_kind k, const char *slug, int id, bool detail);
void  view_free(view *v);
/* Drop what is loaded and fetch again from the top. */
void  view_reload(view *v);
/* Ask for the next page if there is one and none is coming. */
void  view_more(view *v);
/* Let the first d items go (the UI picks d from what is off screen). */
void  view_trim(view *v, int d);

/* Thumbnails from the start of a channel, for the preview on the top
 * screen; asks for them the first time. Returns how many are known (-1
 * while loading). Call with the store locked. */
int store_peek(const char *slug, const char **urls);

/* The signed-in account, refreshed in the background. */
typedef struct {
	bool  known;          /* a /me answer has come back */
	bool  rejected;       /* the token was refused */
	me_t  me;
} account_t;
extern account_t store_account;
void store_refresh_me(void);

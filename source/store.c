#include <3ds.h>

#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEWS 32
#define PEEKS 48

static view          s_view[VIEWS];
static LightLock     s_lock;
static LightEvent    s_wake;
static Thread        s_thread;
static volatile bool s_quit;
static volatile bool s_want_me;

account_t store_account;

typedef struct {
	char slug[80];
	int  n;              /* -2 asked, -1 loading, else count */
	char urls[PEEK_N][480];
	u32  used;
} peek_t;
static peek_t s_peek[PEEKS];
static u32    s_peek_clock;

void store_lock(void) { LightLock_Lock(&s_lock); }
void store_unlock(void) { LightLock_Unlock(&s_lock); }

static void wake(void) { LightEvent_Signal(&s_wake); }

/* ------------------------------------------------------------ views */

view *view_new(list_kind k, const char *slug, int id, bool detail)
{
	for (int i = 0; i < VIEWS; i++) {
		view *v = &s_view[i];
		if (v->used)
			continue;
		u32 gen = v->gen + 1;
		memset(v, 0, sizeof(*v));
		v->gen = gen;
		v->used = true;
		v->req.kind = k;
		v->req.id = id;
		snprintf(v->req.slug, sizeof(v->req.slug), "%s", slug ? slug : "");
		v->want_more = true;
		v->want_detail = detail;
		wake();
		return v;
	}
	return NULL;
}

void view_free(view *v)
{
	if (!v)
		return;
	entry_free_all(v->e, v->n);
	free(v->rh);
	v->rh = NULL;
	v->rh_n = 0;
	v->e = NULL;
	v->n = v->cap = 0;
	v->used = false;
	v->gen++;
}

void view_reload(view *v)
{
	entry_free_all(v->e, v->n);
	free(v->rh);
	v->rh = NULL;
	v->rh_n = 0;
	v->e = NULL;
	v->n = v->cap = 0;
	v->gen++;
	v->req.page = 0;
	v->req.cursor[0] = '\0';
	v->req.more = false;
	v->loading = false;
	v->failed = false;
	v->err[0] = '\0';
	v->want_more = true;
	v->want_detail = v->want_detail || v->det.ok;
	v->dropped = v->drop_rows_items = 0;
	v->drop_px = 0;
	v->sel = 0;
	v->scroll = v->scroll_to = 0;
	wake();
}

void view_more(view *v)
{
	if (v->req.more && !v->loading && !v->failed && !v->want_more) {
		v->want_more = true;
		wake();
	}
}

int store_peek(const char *slug, const char **urls)
{
	peek_t *p = NULL, *oldest = NULL;
	for (int i = 0; i < PEEKS; i++) {
		if (!strcmp(s_peek[i].slug, slug)) {
			p = &s_peek[i];
			break;
		}
		if (!oldest || s_peek[i].used < oldest->used)
			oldest = &s_peek[i];
	}
	if (!p) {
		p = oldest;
		snprintf(p->slug, sizeof(p->slug), "%s", slug);
		p->n = -2;
		wake();
	}
	p->used = ++s_peek_clock;
	for (int i = 0; i < PEEK_N; i++)
		urls[i] = p->urls[i];
	return p->n < 0 ? -1 : p->n;
}

void store_refresh_me(void)
{
	s_want_me = true;
	wake();
}

void view_trim(view *v, int d)
{
	if (d <= 0 || d >= v->n)
		return;
	{
		float px = 0;
		for (int i = 0; i < d && i < v->rh_n; i++)
			px += v->rh[i];
		for (int i = 0; i < d; i++) {
			free(v->e[i].body);
			free(v->e[i].md);
		}
		memmove(v->e, v->e + d, sizeof(entry) * (v->n - d));
		v->n -= d;
		if (v->rh_n > d) {
			memmove(v->rh, v->rh + d, sizeof(float) * (v->rh_n - d));
			v->rh_n -= d;
		} else {
			v->rh_n = 0;
		}
		v->sel = v->sel >= d ? v->sel - d : 0;
		v->dropped += d;
		v->drop_rows_items += d;
		v->drop_px += px;
	}
}

/* ------------------------------------------------------------ worker */

static void worker(void *arg)
{
	(void)arg;
	while (!s_quit) {
		bool did = false;

		if (s_want_me) {
			s_want_me = false;
			me_t me;
			bool unauth = false;
			bool ok = api_me(&me, &unauth);
			store_lock();
			if (ok) {
				store_account.me = me;
				store_account.known = true;
				store_account.rejected = false;
			} else if (unauth) {
				store_account.rejected = true;
			}
			store_unlock();
			did = true;
		}

		/* One page for one view per turn, the newest view first: it is
		 * the one on screen. */
		store_lock();
		view *v = NULL;
		bool detail = false;
		for (int i = VIEWS - 1; i >= 0 && !v; i--) {
			view *c = &s_view[i];
			if (!c->used || c->loading || c->detail_loading)
				continue;
			if (c->want_detail) {
				v = c;
				detail = true;
			} else if (c->want_more) {
				v = c;
			}
		}
		if (v) {
			u32 gen = v->gen;
			list_req req = v->req;
			if (detail) {
				v->want_detail = false;
				v->detail_loading = true;
			} else {
				v->want_more = false;
				v->loading = true;
			}
			store_unlock();

			if (detail) {
				detail_t d;
				int rc = req.kind == L_CHANNEL ? api_channel(req.slug, &d) : api_user(req.slug, &d);
				store_lock();
				if (v->gen == gen) {
					if (rc == 0)
						v->det = d;
					v->detail_loading = false;
				}
				store_unlock();
			} else {
				entry *e = NULL;
				char err[80];
				int n = api_fetch(&req, &e, err, sizeof(err));
				store_lock();
				if (v->gen == gen && v->used) {
					v->loading = false;
					if (n < 0) {
						v->failed = true;
						snprintf(v->err, sizeof(v->err), "%s", err);
					} else {
						v->req = req;
						if (n > 0) {
							entry *ne = realloc(v->e, sizeof(entry) * (v->n + n));
							if (ne) {
								memcpy(ne + v->n, e, sizeof(entry) * n);
								v->e = ne;
								v->n += n;
								free(e);
								e = NULL;
								n = 0;
							}

						}
					}
				} else {
					/* Freed or reloaded meanwhile: an old view never
					 * gets new rows. */
					entry *tmp = e;
					e = NULL;
					store_unlock();
					entry_free_all(tmp, n);
					store_lock();
					n = 0;
				}
				store_unlock();
				entry_free_all(e, n);
			}
			did = true;
			continue;
		}

		/* Channel previews, most recently asked for first. */
		peek_t *p = NULL;
		for (int i = 0; i < PEEKS; i++)
			if (s_peek[i].n == -2 && (!p || s_peek[i].used > p->used))
				p = &s_peek[i];
		if (p) {
			char slug[80];
			snprintf(slug, sizeof(slug), "%s", p->slug);
			p->n = -1;
			store_unlock();
			char urls[PEEK_N][480];
			memset(urls, 0, sizeof(urls));
			int n = api_channel_peek(slug, urls, PEEK_N);
			store_lock();
			if (!strcmp(p->slug, slug)) {
				memcpy(p->urls, urls, sizeof(urls));
				p->n = n < 0 ? 0 : n;
			}
			store_unlock();
			did = true;
			continue;
		}
		store_unlock();

		if (!did)
			LightEvent_WaitTimeout(&s_wake, 1000000000LL);
	}
}

void store_start(void)
{
	LightLock_Init(&s_lock);
	LightEvent_Init(&s_wake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	/* TLS handshakes want a deep stack. */
	s_thread = threadCreate(worker, NULL, 128 * 1024, prio + 1, -2, false);
}

bool store_stop(void)
{
	s_quit = true;
	wake();
	if (!s_thread)
		return true;
	if (R_FAILED(threadJoin(s_thread, 2000000000ULL)))
		return false;
	threadFree(s_thread);
	s_thread = NULL;
	return true;
}

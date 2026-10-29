#include "api.h"

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

#define API "https://api.are.na"

char api_token[128];

/* ------------------------------------------------------------ text */

static size_t put_utf8(char *dst, size_t room, unsigned long cp)
{
	char b[4];
	size_t n;
	if (cp < 0x80) {
		b[0] = (char)cp; n = 1;
	} else if (cp < 0x800) {
		b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
	} else if (cp < 0x10000) {
		b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
	} else if (cp < 0x110000) {
		b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 0x3F));
		b[2] = (char)(0x80 | (cp >> 6 & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
	} else {
		return 0;
	}
	if (n > room)
		return 0;
	memcpy(dst, b, n);
	return n;
}

/* Copy src, decoding HTML character references ("&#039;", "&amp;") and
 * never cutting a UTF-8 character in half. */
static void copy_text(char *dst, size_t cap, const char *src)
{
	static const struct { const char *name; unsigned long cp; } NAMED[] = {
		{"amp", '&'}, {"quot", '"'}, {"apos", '\''}, {"lt", '<'}, {"gt", '>'},
		{"nbsp", ' '}, {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D},
		{"ldquo", 0x201C}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
	};
	size_t o = 0;
	if (!src)
		src = "";
	while (*src && o + 1 < cap) {
		if (*src == '&') {
			const char *semi = strchr(src, ';');
			if (semi && semi - src <= 10) {
				unsigned long cp = 0;
				bool ok = false;
				if (src[1] == '#') {
					char *end;
					cp = (src[2] == 'x' || src[2] == 'X') ? strtoul(src + 3, &end, 16)
					                                      : strtoul(src + 2, &end, 10);
					ok = end == semi && cp > 0;
				} else {
					for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
						size_t l = strlen(NAMED[i].name);
						if ((size_t)(semi - src - 1) == l && !strncmp(src + 1, NAMED[i].name, l)) {
							cp = NAMED[i].cp;
							ok = true;
							break;
						}
					}
				}
				if (ok) {
					size_t n = put_utf8(dst + o, cap - 1 - o, cp);
					if (!n)
						break;
					o += n;
					src = semi + 1;
					continue;
				}
			}
		}
		size_t n = 1;
		unsigned char c = (unsigned char)*src;
		if (c >= 0xF0) n = 4;
		else if (c >= 0xE0) n = 3;
		else if (c >= 0xC0) n = 2;
		if (o + n > cap - 1)
			break;
		for (size_t k = 0; k < n && src[k]; k++)
			dst[o++] = src[k];
		src += n;
	}
	dst[o] = '\0';
}

static char *dup_text(const char *src)
{
	if (!src || !src[0])
		return NULL;
	size_t cap = strlen(src) + 1;
	if (cap > 4000)
		cap = 4000;
	char *d = malloc(cap);
	if (d)
		copy_text(d, cap, src);
	return d;
}

static const char *jstr(json_t *o, const char *k)
{
	return json_string_value(json_object_get(o, k));
}

/* object.k.plain, the plain-text rendering of a markdown field */
static const char *jplain(json_t *o, const char *k)
{
	json_t *v = json_object_get(o, k);
	if (json_is_string(v))
		return json_string_value(v);
	return jstr(v, "plain");
}

static int jint(json_t *o, const char *k)
{
	return (int)json_integer_value(json_object_get(o, k));
}

static void first_line(char *dst, size_t cap, const char *src)
{
	char tmp[256];
	copy_text(tmp, sizeof(tmp), src);
	char *p = tmp;
	while (*p == ' ' || *p == '\n')
		p++;
	char *nl = strchr(p, '\n');
	if (nl)
		*nl = '\0';
	snprintf(dst, cap, "%s", p);
}

/* ------------------------------------------------------------ images */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

static size_t b64_decode(char *dst, size_t cap, const char *src)
{
	size_t o = 0;
	int bits = 0, acc = 0;
	for (; *src && *src != '=' && *src != '?'; src++) {
		int v = b64val(*src);
		if (v < 0)
			break;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o + 1 >= cap)
				break;
			dst[o++] = (char)((acc >> bits) & 0xFF);
		}
	}
	dst[o] = '\0';
	return o;
}

/* URL-safe alphabet: the image service decodes either. */
static void b64_encode(char *dst, size_t cap, const char *src, size_t n)
{
	size_t o = 0;
	for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
		uint32_t v = (uint32_t)(unsigned char)src[i] << 16;
		if (i + 1 < n) v |= (uint32_t)(unsigned char)src[i + 1] << 8;
		if (i + 2 < n) v |= (unsigned char)src[i + 2];
		dst[o++] = B64[v >> 18 & 63];
		dst[o++] = B64[v >> 12 & 63];
		dst[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
		dst[o++] = i + 2 < n ? B64[v & 63] : '=';
	}
	dst[o] = '\0';
}

/* The image service takes its instructions as base64 JSON in the path.
 * Ask it for exactly the size and format the 3DS wants: a small baseline
 * JPEG, so a thumbnail is a few kilobytes and decodes in a blink. */
static void image_url(char *dst, size_t cap, json_t *image, int w, int h, bool cover)
{
	dst[0] = '\0';
	const char *src = jstr(json_object_get(image, "small"), "src");
	if (!src)   /* the v2 shape */
		src = jstr(json_object_get(image, "thumb"), "url");
	if (!src)
		return;
	const char *slash = strrchr(src, '/');
	if (!strstr(src, "images.are.na") || !slash) {
		snprintf(dst, cap, "%s", src);
		return;
	}
	char spec[512];
	b64_decode(spec, sizeof(spec), slash + 1);
	json_error_t je;
	json_t *s = json_loads(spec, 0, &je);
	const char *bucket = jstr(s, "bucket"), *key = jstr(s, "key");
	if (!bucket || !key) {
		json_decref(s);
		snprintf(dst, cap, "%s", src);
		return;
	}
	char js[400];
	int n = snprintf(js, sizeof(js),
	                 "{\"bucket\":\"%s\",\"key\":\"%s\",\"edits\":{\"resize\":{\"width\":%d,"
	                 "\"height\":%d,\"fit\":\"%s\"},\"jpeg\":{\"quality\":80},"
	                 "\"toFormat\":\"jpeg\"}}",
	                 bucket, key, w, h, cover ? "cover" : "inside");
	json_decref(s);
	if (n <= 0 || n >= (int)sizeof(js))
		return;
	int pre = snprintf(dst, cap, "https://images.are.na/");
	b64_encode(dst + pre, cap - pre, js, (size_t)n);
}

/* ------------------------------------------------------------ parsing */

static block_type btype_of(const char *t)
{
	if (!t) return B_OTHER;
	if (!strcmp(t, "Image")) return B_IMAGE;
	if (!strcmp(t, "Text")) return B_TEXT;
	if (!strcmp(t, "Link")) return B_LINK;
	if (!strcmp(t, "Embed")) return B_EMBED;
	if (!strcmp(t, "Attachment")) return B_ATTACH;
	return B_OTHER;
}

static visibility vis_of(const char *v)
{
	if (v && !strcmp(v, "public")) return VIS_PUBLIC;
	if (v && !strcmp(v, "private")) return VIS_PRIVATE;
	return VIS_CLOSED;
}

static void initials_of(char *dst, const char *name)
{
	int o = 0;
	bool start = true;
	for (const char *p = name; p && *p && o < 2; p++) {
		if (*p == ' ') {
			start = true;
		} else if (start) {
			char c = *p;
			if (c >= 'a' && c <= 'z')
				c -= 32;
			if ((unsigned char)c < 0x80)
				dst[o++] = c;
			start = false;
		}
	}
	dst[o] = '\0';
}

static void parse_block(json_t *b, entry *e)
{
	e->kind = E_BLOCK;
	e->btype = btype_of(jstr(b, "type"));
	e->id = jint(b, "id");
	snprintf(e->slug, sizeof(e->slug), "%d", e->id);
	json_t *src = json_object_get(b, "source");
	const char *title = jstr(b, "title");
	e->has_title = title && title[0];
	if (title && title[0])
		copy_text(e->title, sizeof(e->title), title);
	else if (e->btype == B_TEXT)
		first_line(e->title, sizeof(e->title), jplain(b, "content"));
	else if (jstr(src, "title"))
		copy_text(e->title, sizeof(e->title), jstr(src, "title"));
	else if (jstr(json_object_get(b, "attachment"), "filename"))
		copy_text(e->title, sizeof(e->title), jstr(json_object_get(b, "attachment"), "filename"));
	if (!e->title[0])
		snprintf(e->title, sizeof(e->title), "Untitled");
	json_t *user = json_object_get(b, "user");
	copy_text(e->owner, sizeof(e->owner), jstr(user, "name"));
	copy_text(e->initials, sizeof(e->initials), jstr(user, "initials"));
	e->created = parse_iso(jstr(b, "created_at"));
	json_t *conn = json_object_get(b, "connection");
	e->when = parse_iso(jstr(conn, "connected_at"));
	e->conn_id = jint(conn, "id");
	e->can_remove = json_is_true(json_object_get(json_object_get(conn, "can"), "remove"));
	copy_text(e->conn_by, sizeof(e->conn_by), jstr(json_object_get(conn, "connected_by"), "slug"));
	e->can_manage = json_is_true(json_object_get(json_object_get(b, "can"), "manage"));
	if (!e->when)
		e->when = parse_iso(jstr(b, "updated_at"));
	e->vis = vis_of(jstr(b, "visibility"));
	json_t *img = json_object_get(b, "image");
	if (json_is_object(img)) {
		image_url(e->thumb, sizeof(e->thumb), img, THUMB_PX, THUMB_PX, true);
		image_url(e->big, sizeof(e->big), img, BIG_W, BIG_H, false);
	}
	const char *k = e->btype == B_TEXT ? "content" : "description";
	e->body = dup_text(jplain(b, k));
	const char *md = jstr(json_object_get(b, k), "markdown");
	e->md = md && md[0] ? strdup(md) : NULL;
}

static void parse_channel(json_t *c, entry *e)
{
	e->kind = E_CHANNEL;
	e->id = jint(c, "id");
	copy_text(e->slug, sizeof(e->slug), jstr(c, "slug"));
	copy_text(e->title, sizeof(e->title), jstr(c, "title"));
	json_t *owner = json_object_get(c, "owner");
	copy_text(e->owner, sizeof(e->owner), jstr(owner, "name"));
	copy_text(e->initials, sizeof(e->initials), jstr(owner, "initials"));
	e->vis = vis_of(jstr(c, "visibility"));
	json_t *counts = json_object_get(c, "counts");
	e->count = jint(counts, "contents");
	if (!e->count)
		e->count = jint(counts, "blocks");
	e->when = parse_iso(jstr(c, "updated_at"));
	e->created = parse_iso(jstr(c, "created_at"));
	e->body = dup_text(jplain(c, "description"));
	e->can_add = json_is_true(json_object_get(json_object_get(c, "can"), "add_to"));
	e->can_manage = json_is_true(json_object_get(json_object_get(c, "can"), "update"));
	json_t *conn = json_object_get(c, "connection");
	e->conn_id = jint(conn, "id");
	e->can_remove = json_is_true(json_object_get(json_object_get(conn, "can"), "remove"));
}

/* The v2 listing behind Explore: recently updated public channels. */
static void parse_channel_v2(json_t *c, entry *e)
{
	e->kind = E_CHANNEL;
	e->id = jint(c, "id");
	copy_text(e->slug, sizeof(e->slug), jstr(c, "slug"));
	copy_text(e->title, sizeof(e->title), jstr(c, "title"));
	json_t *grp = json_object_get(c, "group");
	json_t *user = json_object_get(c, "user");
	const char *name = json_is_object(grp) ? jstr(grp, "name") : jstr(user, "full_name");
	copy_text(e->owner, sizeof(e->owner), name);
	initials_of(e->initials, e->owner);
	const char *st = jstr(c, "status");
	e->vis = st && !strcmp(st, "public") ? VIS_PUBLIC
	       : st && !strcmp(st, "private") ? VIS_PRIVATE : VIS_CLOSED;
	e->count = jint(c, "length");
	e->when = parse_iso(jstr(c, "updated_at"));
}

/* A block in the v2 shape, as the Explore listing gives them. */
static void parse_block_v2(json_t *b, entry *e)
{
	e->kind = E_BLOCK;
	e->btype = btype_of(jstr(b, "class"));
	e->id = jint(b, "id");
	snprintf(e->slug, sizeof(e->slug), "%d", e->id);
	const char *title = jstr(b, "title");
	if (title && title[0])
		copy_text(e->title, sizeof(e->title), title);
	else if (e->btype == B_TEXT)
		first_line(e->title, sizeof(e->title), jstr(b, "content"));
	else
		copy_text(e->title, sizeof(e->title), jstr(b, "generated_title"));
	if (!e->title[0])
		snprintf(e->title, sizeof(e->title), "Untitled");
	copy_text(e->owner, sizeof(e->owner), jstr(json_object_get(b, "user"), "full_name"));
	initials_of(e->initials, e->owner);
	e->when = parse_iso(jstr(b, "updated_at"));
	e->created = parse_iso(jstr(b, "created_at"));
	e->vis = vis_of(jstr(b, "visibility"));
	/* Explore's tiles run larger than a channel's grid: a bigger square
	 * (still one 128px texture) keeps them sharp. */
	json_t *img = json_object_get(b, "image");
	if (json_is_object(img)) {
		image_url(e->thumb, sizeof(e->thumb), img, 128, 128, true);
		image_url(e->big, sizeof(e->big), img, BIG_W, BIG_H, false);
	}
	e->body = dup_text(jstr(b, e->btype == B_TEXT ? "content" : "description"));
}

static int by_newest(const void *a, const void *b)
{
	time_t x = ((const entry *)a)->when, y = ((const entry *)b)->when;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void parse_user(json_t *u, entry *e)
{
	e->kind = E_USER;
	e->id = jint(u, "id");
	copy_text(e->slug, sizeof(e->slug), jstr(u, "slug"));
	copy_text(e->title, sizeof(e->title), jstr(u, "name"));
	copy_text(e->initials, sizeof(e->initials), jstr(u, "initials"));
	if (!e->initials[0])
		initials_of(e->initials, e->title);
}

static void parse_any(json_t *o, entry *e)
{
	const char *t = jstr(o, "type");
	if (t && !strcmp(t, "Channel"))
		parse_channel(o, e);
	else if (t && (!strcmp(t, "User") || !strcmp(t, "Group")))
		parse_user(o, e);
	else
		parse_block(o, e);
}

static void parse_ref(json_t *o, ref_t *r)
{
	memset(r, 0, sizeof(*r));
	r->kind = E_HEADER;
	if (!json_is_object(o))
		return;
	entry e = {0};
	const char *t = jstr(o, "type");
	if (t && !strcmp(t, "Comment"))
		return;
	parse_any(o, &e);
	r->kind = e.kind;
	r->btype = e.btype;
	r->vis = e.vis;
	r->id = e.id;
	snprintf(r->slug, sizeof(r->slug), "%s", e.slug);
	snprintf(r->title, sizeof(r->title), "%s", e.title);
	snprintf(r->initials, sizeof(r->initials), "%s", e.initials);
	free(e.body);
}

static void parse_activity(json_t *a, entry *e)
{
	static const struct { const char *kind, *verb; } VERBS[] = {
		{"followed_user", "followed"},
		{"followed_channel", "followed"},
		{"followed_group", "followed"},
		{"added_block_to_channel", "connected"},
		{"added_channel_to_channel", "connected"},
		{"created_channel", "created"},
		{"collaborating_with_user_on_channel", "added you to"},
		{"collaborating_with_group_on_channel", "added your group to"},
		{"commented_on_block", "commented on"},
		{"mentioned_you", "mentioned you in"},
		{"added_user_to_group", "added you to"},
	};
	e->kind = E_ACTIVITY;
	e->id = jint(a, "id");
	const char *kind = jstr(a, "kind");
	snprintf(e->verb, sizeof(e->verb), "%s", kind ? kind : "");
	for (size_t i = 0; i < sizeof(VERBS) / sizeof(VERBS[0]); i++)
		if (kind && !strcmp(kind, VERBS[i].kind))
			snprintf(e->verb, sizeof(e->verb), "%s", VERBS[i].verb);
	json_t *actor = json_object_get(a, "actor");
	copy_text(e->owner, sizeof(e->owner), jstr(actor, "name"));
	copy_text(e->initials, sizeof(e->initials), jstr(actor, "initials"));
	if (!e->initials[0])
		initials_of(e->initials, e->owner);
	e->when = parse_iso(jstr(a, "created_at"));
	json_t *item = json_object_get(a, "item");
	parse_ref(item, &e->item);
	parse_ref(json_object_get(a, "target"), &e->target);
	/* A comment's block is its parent. */
	if (e->item.kind == E_HEADER)
		parse_ref(json_object_get(a, "parent"), &e->item);
	json_t *img = json_object_get(item, "image");
	if (json_is_object(img)) {
		image_url(e->thumb, sizeof(e->thumb), img, THUMB_PX, THUMB_PX, true);
		image_url(e->big, sizeof(e->big), img, BIG_W, BIG_H, false);
	}
	if (e->item.kind == E_BLOCK && e->item.btype == B_TEXT)
		e->body = dup_text(jplain(item, "content"));
	json_t *rd = json_object_get(a, "is_read");
	e->unread = json_is_boolean(rd) && !json_boolean_value(rd);
}

/* ------------------------------------------------------------ fetching */

static json_t *get_json(const char *url, char *err, size_t errcap, long *status)
{
	char *body = NULL;
	http_opts o = {.bearer = api_token};
	long st = http_fetch(url, &o, &body, NULL);
	if (status)
		*status = st;
	if (st != 200) {
		if (st == 0)
			snprintf(err, errcap, "No connection");
		else if (st == 401)
			snprintf(err, errcap, "Log in to see this");
		else if (st == 403)
			snprintf(err, errcap, "This is private");
		else if (st == 404)
			snprintf(err, errcap, "Not found");
		else if (st == 429)
			snprintf(err, errcap, "Too many requests, try again in a minute");
		else
			snprintf(err, errcap, "Error %ld", st);
		free(body);
		return NULL;
	}
	json_error_t je;
	json_t *root = json_loads(body ? body : "", 0, &je);
	free(body);
	if (!root)
		snprintf(err, errcap, "Bad response");
	return root;
}

static entry *grow(entry *a, int *cap, int need)
{
	if (need <= *cap)
		return a;
	int nc = *cap ? *cap * 2 : 32;
	while (nc < need)
		nc *= 2;
	entry *b = realloc(a, sizeof(entry) * nc);
	if (!b)
		return a;
	memset(b + *cap, 0, sizeof(entry) * (nc - *cap));
	*cap = nc;
	return b;
}

static void page_meta(json_t *root, list_req *r)
{
	json_t *meta = json_object_get(root, "meta");
	json_t *more = json_object_get(meta, "has_more_pages");
	if (!more)
		more = json_object_get(meta, "has_more");
	r->more = json_is_true(more);
	const char *cur = jstr(meta, "next_cursor");
	snprintf(r->cursor, sizeof(r->cursor), "%s", cur ? cur : "");
	r->page++;
}

static entry *add_section(entry *a, int *n, int *cap, entry_kind k, const char *label, const char *val)
{
	if (!val || !val[0])
		return a;
	a = grow(a, cap, *n + 1);
	if (*n >= *cap)
		return a;
	entry *e = &a[(*n)++];
	memset(e, 0, sizeof(*e));
	e->kind = k;
	snprintf(e->title, sizeof(e->title), "%s", label);
	e->body = dup_text(val);
	return a;
}

static void ago(char *out, size_t n, time_t t)
{
	/* An absolute date reads fine here; the list draws relative times. */
	if (!t) {
		out[0] = '\0';
		return;
	}
	static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(out, n, "%s %d, %d", MON[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
}

static int fetch_block(list_req *r, entry **out, char *err, size_t errcap)
{
	char url[256];
	entry *a = NULL;
	int n = 0, cap = 0;
	if (r->page == 0) {
		snprintf(url, sizeof(url), API "/v3/blocks/%d", r->id);
		json_t *b = get_json(url, err, errcap, NULL);
		if (!b)
			return -1;
		a = grow(a, &cap, 16);
		parse_block(b, &a[n++]);
		entry *self = &a[0];
		char tmp[64];
		/* The info sheet, top to bottom as on the phone. */
		if (self->btype != B_TEXT)
			a = add_section(a, &n, &cap, E_SECTION, "Description", jplain(b, "description"));
		json_t *src = json_object_get(b, "source");
		a = add_section(a, &n, &cap, E_SECTION, "Source", jstr(src, "url"));
		ago(tmp, sizeof(tmp), parse_iso(jstr(b, "created_at")));
		a = add_section(a, &n, &cap, E_KV, "Added", tmp);
		ago(tmp, sizeof(tmp), parse_iso(jstr(b, "updated_at")));
		a = add_section(a, &n, &cap, E_KV, "Updated", tmp);
		a = add_section(a, &n, &cap, E_KV, "By", jstr(json_object_get(b, "user"), "name"));
		json_t *img = json_object_get(b, "image");
		if (json_is_object(img)) {
			snprintf(tmp, sizeof(tmp), "%d \xC3\x97 %d", jint(img, "width"), jint(img, "height"));
			a = add_section(a, &n, &cap, E_KV, "Dimensions", tmp);
		}
		int cc = jint(b, "comment_count");
		if (cc) {
			snprintf(tmp, sizeof(tmp), "%d", cc);
			a = add_section(a, &n, &cap, E_KV, "Comments", tmp);
		}
		a = add_section(a, &n, &cap, E_HEADER, "Connections", "-");
		json_decref(b);
		r->page = 1;
		r->more = true;
	}
	snprintf(url, sizeof(url), API "/v3/blocks/%d/connections?per=24&page=%d", r->id, r->page);
	json_t *root = get_json(url, err, errcap, NULL);
	if (!root) {
		if (n) {           /* the block itself still shows */
			r->more = false;
			*out = a;
			return n;
		}
		return -1;
	}
	size_t i;
	json_t *c;
	json_array_foreach(json_object_get(root, "data"), i, c) {
		a = grow(a, &cap, n + 1);
		if (n < cap)
			parse_channel(c, &a[n++]);
	}
	page_meta(root, r);
	json_decref(root);
	*out = a;
	return n;
}

int api_fetch(list_req *r, entry **out, char *err, size_t errcap)
{
	*out = NULL;
	err[0] = '\0';
	if (r->kind == L_BLOCK)
		return fetch_block(r, out, err, errcap);

	char url[384], cur[160];
	url_encode(cur, sizeof(cur), r->cursor);
	int page = r->page + 1;
	switch (r->kind) {
	case L_FEED:
		snprintf(url, sizeof(url), API "/v3/me/feed?limit=20%s%s", cur[0] ? "&next=" : "", cur);
		break;
	case L_NOTIFICATIONS:
		snprintf(url, sizeof(url), API "/v3/me/notifications?limit=20%s%s", cur[0] ? "&next=" : "", cur);
		break;
	case L_EXPLORE:
		/* Everything public, newest first: channels and blocks together. */
		snprintf(url, sizeof(url), API "/v2/search?q=*&per=24&page=%d", page);
		break;
	case L_CHANNEL:
		snprintf(url, sizeof(url), API "/v3/channels/%s/contents?per=24&page=%d&sort=position_desc",
		         r->slug, page);
		break;
	case L_USER_CHANNELS:
		snprintf(url, sizeof(url),
		         API "/v3/users/%s/contents?type=Channel&per=20&page=%d&sort=updated_at_desc",
		         r->slug, page);
		break;
	case L_USER_BLOCKS:
		snprintf(url, sizeof(url),
		         API "/v3/users/%s/contents?type=Block&per=24&page=%d&sort=created_at_desc",
		         r->slug, page);
		break;
	default:
		return -1;
	}
	json_t *root = get_json(url, err, errcap, NULL);
	if (!root)
		return -1;
	entry *a = NULL;
	int n = 0, cap = 0;
	size_t i;
	json_t *o;
	if (r->kind == L_EXPLORE) {
		json_array_foreach(json_object_get(root, "channels"), i, o) {
			a = grow(a, &cap, n + 1);
			if (n < cap)
				parse_channel_v2(o, &a[n++]);
		}
		json_array_foreach(json_object_get(root, "blocks"), i, o) {
			a = grow(a, &cap, n + 1);
			if (n < cap)
				parse_block_v2(o, &a[n++]);
		}
		if (n > 1)
			qsort(a, (size_t)n, sizeof(entry), by_newest);
		r->page = page;
		r->more = jint(root, "total_pages") > page && n > 0;
	} else {
		json_array_foreach(json_object_get(root, "data"), i, o) {
			a = grow(a, &cap, n + 1);
			if (n >= cap)
				break;
			if (r->kind == L_FEED || r->kind == L_NOTIFICATIONS)
				parse_activity(o, &a[n++]);
			else
				parse_any(o, &a[n++]);
		}
		page_meta(root, r);
	}
	json_decref(root);
	*out = a;
	return n;
}

int api_channel(const char *slug, detail_t *d)
{
	char url[256], err[64];
	memset(d, 0, sizeof(*d));
	snprintf(url, sizeof(url), API "/v3/channels/%s", slug);
	json_t *c = get_json(url, err, sizeof(err), NULL);
	if (!c)
		return -1;
	copy_text(d->title, sizeof(d->title), jstr(c, "title"));
	json_t *owner = json_object_get(c, "owner");
	copy_text(d->owner, sizeof(d->owner), jstr(owner, "name"));
	copy_text(d->initials, sizeof(d->initials), jstr(owner, "initials"));
	d->vis = vis_of(jstr(c, "visibility"));
	d->id = jint(c, "id");
	d->can_update = json_is_true(json_object_get(json_object_get(c, "can"), "update"));
	d->can_add = json_is_true(json_object_get(json_object_get(c, "can"), "add_to"));
	json_t *counts = json_object_get(c, "counts");
	d->length = jint(counts, "contents");
	d->followers = jint(c, "follower_count");
	d->created = parse_iso(jstr(c, "created_at"));
	d->updated = parse_iso(jstr(c, "updated_at"));
	copy_text(d->description, sizeof(d->description), jplain(c, "description"));
	json_decref(c);

	/* The follower count comes from the followers list; one item is
	 * enough to read its total. */
	snprintf(url, sizeof(url), API "/v3/channels/%s/followers?per=1", slug);
	json_t *f = get_json(url, err, sizeof(err), NULL);
	if (f) {
		d->followers = jint(json_object_get(f, "meta"), "total_count");
		json_decref(f);
	}
	d->ok = true;
	return 0;
}

int api_user(const char *slug, detail_t *d)
{
	char url[256], err[64];
	memset(d, 0, sizeof(*d));
	snprintf(url, sizeof(url), API "/v3/users/%s", slug);
	json_t *u = get_json(url, err, sizeof(err), NULL);
	if (!u) {
		/* Groups own channels too. */
		snprintf(url, sizeof(url), API "/v3/groups/%s", slug);
		u = get_json(url, err, sizeof(err), NULL);
	}
	if (!u)
		return -1;
	copy_text(d->title, sizeof(d->title), jstr(u, "name"));
	copy_text(d->initials, sizeof(d->initials), jstr(u, "initials"));
	d->created = parse_iso(jstr(u, "created_at"));
	json_t *counts = json_object_get(u, "counts");
	d->channels = jint(counts, "channels");
	d->followers = jint(counts, "followers");
	d->following = jint(counts, "following");
	copy_text(d->description, sizeof(d->description), jplain(u, "bio"));
	/* Users carry no block count; the list's total is it. */
	char burl[256];
	snprintf(burl, sizeof(burl), API "/v3/users/%s/contents?type=Block&per=1", slug);
	json_t *bl = get_json(burl, err, sizeof(err), NULL);
	if (bl) {
		d->blocks = jint(json_object_get(bl, "meta"), "total_count");
		json_decref(bl);
	}
	if (!d->description[0])
		copy_text(d->description, sizeof(d->description), jplain(u, "description"));
	json_decref(u);
	d->ok = true;
	return 0;
}

int api_channel_peek(const char *slug, char urls[][480], int n)
{
	char url[256], err[64];
	snprintf(url, sizeof(url), API "/v3/channels/%s/contents?per=%d&sort=position_desc", slug, n * 2);
	json_t *root = get_json(url, err, sizeof(err), NULL);
	if (!root)
		return -1;
	int k = 0;
	size_t i;
	json_t *b;
	json_array_foreach(json_object_get(root, "data"), i, b) {
		json_t *img = json_object_get(b, "image");
		if (k < n && json_is_object(img))
			image_url(urls[k++], 480, img, THUMB_PX, THUMB_PX, true);
	}
	json_decref(root);
	return k;
}

bool api_me(me_t *me, bool *unauthorized)
{
	char err[64];
	long st = 0;
	memset(me, 0, sizeof(*me));
	json_t *u = get_json(API "/v3/me", err, sizeof(err), &st);
	if (unauthorized)
		*unauthorized = st == 401;
	if (!u)
		return false;
	copy_text(me->slug, sizeof(me->slug), jstr(u, "slug"));
	copy_text(me->name, sizeof(me->name), jstr(u, "name"));
	copy_text(me->initials, sizeof(me->initials), jstr(u, "initials"));
	if (!me->initials[0])
		initials_of(me->initials, me->name);
	me->unread = jint(json_object_get(u, "counts"), "notifications");
	json_decref(u);
	return me->slug[0] != '\0';
}

void entry_free_all(entry *e, int n)
{
	if (!e)
		return;
	for (int i = 0; i < n; i++) {
		free(e[i].body);
		free(e[i].md);
	}
	free(e);
}

/* ------------------------------------------------------------ adding */

static void write_err(long st, const char *body, char *err, size_t errcap)
{
	json_error_t je;
	json_t *j = body ? json_loads(body, 0, &je) : NULL;
	const char *m = json_string_value(json_object_get(json_object_get(j, "details"), "message"));
	if (!m)
		m = json_string_value(json_object_get(j, "error"));
	if (st == 0)
		snprintf(err, errcap, "No connection");
	else if (st == 401 || st == 403)
		snprintf(err, errcap, API_ERR_READONLY);
	else if (st == 429)
		snprintf(err, errcap, "Too many requests, try again in a minute");
	else if (m)
		snprintf(err, errcap, "%s", m);
	else
		snprintf(err, errcap, "Error %ld", st);
	json_decref(j);
}

static long post_json(const char *url, json_t *payload, char **resp)
{
	char *body = json_dumps(payload, JSON_COMPACT);
	http_opts o = {.method = "POST", .bearer = api_token, .body = body,
	               .content_type = "application/json"};
	long st = http_fetch(url, &o, resp, NULL);
	free(body);
	return st;
}

int api_upload(const void *data, size_t len, const char *filename, const char *ctype,
               char *out_url, size_t cap, char *err, size_t errcap,
               volatile float *progress)
{
	(void)progress;
	json_t *req = json_pack("{s:[{s:s,s:s}]}", "files", "filename", filename, "content_type", ctype);
	char *resp = NULL;
	long st = post_json(API "/v3/uploads/presign", req, &resp);
	json_decref(req);
	if (st != 200 && st != 201) {
		write_err(st, resp, err, errcap);
		free(resp);
		return -1;
	}
	json_error_t je;
	json_t *j = json_loads(resp ? resp : "", 0, &je);
	free(resp);
	json_t *f = json_array_get(json_object_get(j, "files"), 0);
	const char *put = jstr(f, "upload_url"), *key = jstr(f, "key"), *ct = jstr(f, "content_type");
	if (!put || !key) {
		snprintf(err, errcap, "Are.na did not give an upload address");
		json_decref(j);
		return -1;
	}
	http_opts o = {.method = "PUT", .body = data, .body_len = len,
	               .content_type = ct ? ct : ctype, .no_auth_hdr = true};
	char *r2 = NULL;
	st = http_fetch(put, &o, &r2, NULL);
	free(r2);
	if (st != 200 && st != 204) {
		snprintf(err, errcap, st ? "Upload refused (%ld)" : "Upload failed: no connection", st);
		json_decref(j);
		return -1;
	}
	snprintf(out_url, cap, "https://s3.amazonaws.com/arena_images-temp/%s", key);
	json_decref(j);
	return 0;
}

int api_create_block(const char *value, const char *title, int channel_id, char *err, size_t errcap)
{
	json_t *req = json_pack("{s:s,s:[i]}", "value", value, "channel_ids", channel_id);
	if (title && title[0])
		json_object_set_new(req, "title", json_string(title));
	char *resp = NULL;
	long st = post_json(API "/v3/blocks", req, &resp);
	json_decref(req);
	if (st != 200 && st != 201) {
		write_err(st, resp, err, errcap);
		free(resp);
		return -1;
	}
	free(resp);
	return 0;
}

int api_create_channel(const char *title, visibility vis, char *slug, size_t slugcap, int *id,
                       char *err, size_t errcap)
{
	const char *v = vis == VIS_PUBLIC ? "public" : vis == VIS_PRIVATE ? "private" : "closed";
	json_t *req = json_pack("{s:s,s:s}", "title", title, "visibility", v);
	char *resp = NULL;
	long st = post_json(API "/v3/channels", req, &resp);
	json_decref(req);
	if (st != 200 && st != 201) {
		write_err(st, resp, err, errcap);
		free(resp);
		return -1;
	}
	json_error_t je;
	json_t *j = json_loads(resp ? resp : "", 0, &je);
	free(resp);
	snprintf(slug, slugcap, "%s", jstr(j, "slug") ? jstr(j, "slug") : "");
	if (id)
		*id = jint(j, "id");
	json_decref(j);
	return 0;
}

static long send_json(const char *method, const char *url, json_t *payload, char **resp)
{
	char *body = payload ? json_dumps(payload, JSON_COMPACT) : NULL;
	http_opts o = {.method = method, .bearer = api_token, .body = body,
	               .content_type = "application/json"};
	long st = http_fetch(url, &o, resp, NULL);
	free(body);
	return st;
}

static int finish(long st, char *resp, char *err, size_t errcap)
{
	int rc = st >= 200 && st < 300 ? 0 : -1;
	if (rc)
		write_err(st, resp, err, errcap);
	free(resp);
	return rc;
}

int api_connect(int id, bool is_channel, int channel_id, char *err, size_t errcap)
{
	json_t *req = json_pack("{s:i,s:s,s:[i]}", "connectable_id", id, "connectable_type",
	                        is_channel ? "Channel" : "Block", "channel_ids", channel_id);
	char *resp = NULL;
	long st = send_json("POST", API "/v3/connections", req, &resp);
	json_decref(req);
	return finish(st, resp, err, errcap);
}

int api_disconnect(int connection_id, char *err, size_t errcap)
{
	char url[128], *resp = NULL;
	snprintf(url, sizeof(url), API "/v3/connections/%d", connection_id);
	long st = send_json("DELETE", url, NULL, &resp);
	return finish(st, resp, err, errcap);
}

int api_update_block(int id, bool is_text, const char *title, const char *body, char *err,
                     size_t errcap)
{
	char url[128], *resp = NULL;
	snprintf(url, sizeof(url), API "/v3/blocks/%d", id);
	json_t *req = json_object();
	if (title)
		json_object_set_new(req, "title", json_string(title));
	if (body)
		json_object_set_new(req, is_text ? "content" : "description", json_string(body));
	long st = send_json("PUT", url, req, &resp);
	json_decref(req);
	return finish(st, resp, err, errcap);
}

int api_update_channel(int id, const char *title, visibility vis, char *err, size_t errcap)
{
	char url[128], *resp = NULL;
	snprintf(url, sizeof(url), API "/v3/channels/%d", id);
	const char *v = vis == VIS_PUBLIC ? "public" : vis == VIS_PRIVATE ? "private" : "closed";
	json_t *req = json_pack("{s:s,s:s}", "title", title, "visibility", v);
	long st = send_json("PUT", url, req, &resp);
	json_decref(req);
	return finish(st, resp, err, errcap);
}

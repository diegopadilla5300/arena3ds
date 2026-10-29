#include "http.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

volatile bool http_abort_all;
volatile long http_clock_offset;

static CURLSH   *s_share;
static LightLock s_share_lock[CURL_LOCK_DATA_LAST];

static void share_lock(CURL *c, curl_lock_data d, curl_lock_access a, void *ud)
{
	(void)c; (void)a; (void)ud;
	LightLock_Lock(&s_share_lock[d]);
}

static void share_unlock(CURL *c, curl_lock_data d, void *ud)
{
	(void)c; (void)ud;
	LightLock_Unlock(&s_share_lock[d]);
}

void http_init(void)
{
	for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
		LightLock_Init(&s_share_lock[i]);
	s_share = curl_share_init();
	if (!s_share)
		return;
	curl_share_setopt(s_share, CURLSHOPT_LOCKFUNC, share_lock);
	curl_share_setopt(s_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
	curl_share_setopt(s_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	curl_share_setopt(s_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
	curl_share_setopt(s_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
}

void http_exit(void)
{
	if (s_share)
		curl_share_cleanup(s_share);
	s_share = NULL;
}

typedef struct {
	char  *data;
	size_t len, cap;
} membuf;

static size_t on_body(char *p, size_t sz, size_t n, void *ud)
{
	membuf *m = ud;
	size_t add = sz * n;
	if (m->len + add + 1 > m->cap) {
		size_t cap = m->cap ? m->cap * 2 : 16384;
		while (cap < m->len + add + 1)
			cap *= 2;
		char *d = realloc(m->data, cap);
		if (!d)
			return 0;
		m->data = d;
		m->cap = cap;
	}
	memcpy(m->data + m->len, p, add);
	m->len += add;
	m->data[m->len] = '\0';
	return add;
}

time_t http_now_utc(void)
{
	return time(NULL) - http_clock_offset;
}

/* "Date: Wed, 28 Oct 2026 10:05:06 GMT" from api.are.na, which does not
 * cache: close enough to now to set the clock offset by. */
static size_t on_header(char *p, size_t sz, size_t n, void *ud)
{
	size_t len = sz * n;
	if (!ud || len < 10 || strncasecmp(p, "date:", 5) != 0)
		return len;
	static const char *MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char line[64];
	size_t l = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
	memcpy(line, p, l);
	line[l] = '\0';
	int d, y, h, mi, s;
	char mon[4];
	if (sscanf(line, "%*[^,], %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &s) == 6) {
		const char *m = strstr(MON, mon);
		if (m) {
			long off = (long)(time(NULL) - utc_to_epoch(y, (int)(m - MON) / 3 + 1, d, h, mi, s));
			/* Zones are whole quarter hours; round off the network delay. */
			http_clock_offset = ((off + (off >= 0 ? 450 : -450)) / 900) * 900;
		}
	}
	return len;
}

static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)ud; (void)a; (void)b; (void)c; (void)d;
	return http_abort_all;
}

long http_fetch(const char *url, const http_opts *o, char **body, size_t *len)
{
	static const http_opts none;
	if (!o)
		o = &none;
	*body = NULL;
	if (len)
		*len = 0;
	if (http_abort_all)
		return 0;
	CURL *c = curl_easy_init();
	if (!c)
		return 0;
	membuf m = {0};
	struct curl_slist *hdr = NULL;
	char auth[192];
	curl_easy_setopt(c, CURLOPT_URL, url);
	if (s_share)
		curl_easy_setopt(c, CURLOPT_SHARE, s_share);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 6L);
	curl_easy_setopt(c, CURLOPT_CAINFO, HTTP_CA_BUNDLE);
	curl_easy_setopt(c, CURLOPT_USERAGENT, HTTP_USER_AGENT);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, o->timeout ? o->timeout : 40L);
	if (o->body_len > 512 * 1024)   /* big uploads over the 3DS's Wi-Fi */
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, strstr(url, "api.are.na") ? (void *)1 : NULL);
	if (!o->no_auth_hdr)
		hdr = curl_slist_append(hdr, "Accept: application/json, image/jpeg");
	if (o->bearer && o->bearer[0]) {
		snprintf(auth, sizeof(auth), "Authorization: Bearer %s", o->bearer);
		hdr = curl_slist_append(hdr, auth);
	}
	if (o->body) {
		char ct[96];
		snprintf(ct, sizeof(ct), "Content-Type: %s",
		         o->content_type ? o->content_type : "application/x-www-form-urlencoded");
		hdr = curl_slist_append(hdr, ct);
		/* S3 rejects a chunked or 100-continue upload it did not sign. */
		hdr = curl_slist_append(hdr, "Expect:");
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, o->body);
		if (o->body_len)
			curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)o->body_len);
	}
	if (o->method)
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, o->method);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);

	CURLcode rc = curl_easy_perform(c);
	long status = 0;
	if (rc == CURLE_OK)
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(c);
	curl_slist_free_all(hdr);
	*body = m.data;
	if (len)
		*len = m.len;
	return status;
}

int http_get(const char *url, char **body, size_t *len)
{
	long st = http_fetch(url, NULL, body, len);
	if (st != 200 || !*body) {
		free(*body);
		*body = NULL;
		return -1;
	}
	return 0;
}

time_t utc_to_epoch(int y, int mo, int d, int h, int mi, int s)
{
	/* days_from_civil, Howard Hinnant's algorithm. */
	y -= mo <= 2;
	const int era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);
	const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	const long days = (long)era * 146097 + (long)doe - 719468;
	return (time_t)days * 86400 + h * 3600 + mi * 60 + s;
}

time_t parse_iso(const char *s)
{
	int y, mo, d, h, mi, sec;
	if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
		return 0;
	return utc_to_epoch(y, mo, d, h, mi, sec);
}

void url_encode(char *dst, size_t cap, const char *src)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *src && o + 4 < cap; src++) {
		unsigned char c = (unsigned char)*src;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~') {
			dst[o++] = (char)c;
		} else {
			dst[o++] = '%';
			dst[o++] = HEX[c >> 4];
			dst[o++] = HEX[c & 15];
		}
	}
	dst[o] = '\0';
}

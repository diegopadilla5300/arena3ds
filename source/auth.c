#define _GNU_SOURCE
#include "auth.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <mbedtls/sha256.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "config.h"
#include "http.h"

#ifndef ARENA_CLIENT_ID
#define ARENA_CLIENT_ID ""
#endif
#ifndef ARENA_REDIRECT_URI
#define ARENA_REDIRECT_URI ""
#endif

#define DIR_PATH   "sdmc:/3ds/arena"
#define TOKEN_PATH DIR_PATH "/token.txt"
#define CONF_PATH  DIR_PATH "/oauth.txt"
#define PORT       8080

static char s_client_id[96], s_client_secret[96], s_redirect[160];
static char s_verifier[72], s_challenge[48], s_state[24];
static char s_page_url[48];
static volatile auth_state s_state_now;
static char s_msg[320];
static LightLock s_lock;
static Thread s_thread;
static volatile bool s_stop;
static int s_listen = -1;

/* ------------------------------------------------------------ config */

static void load_conf(void)
{
	snprintf(s_client_id, sizeof(s_client_id), "%s", ARENA_CLIENT_ID);
	snprintf(s_redirect, sizeof(s_redirect), "%s", ARENA_REDIRECT_URI);
	FILE *f = fopen(CONF_PATH, "r");
	if (!f)
		return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "client_id"))
			snprintf(s_client_id, sizeof(s_client_id), "%s", eq);
		else if (!strcmp(line, "client_secret"))
			snprintf(s_client_secret, sizeof(s_client_secret), "%s", eq);
		else if (!strcmp(line, "redirect_uri"))
			snprintf(s_redirect, sizeof(s_redirect), "%s", eq);
	}
	fclose(f);
}

bool auth_has_client(void)
{
	return s_client_id[0] && s_redirect[0];
}

void auth_load(void)
{
	LightLock_Init(&s_lock);
	load_conf();
	FILE *f = fopen(TOKEN_PATH, "r");
	if (!f)
		return;
	char t[128] = {0};
	if (fgets(t, sizeof(t), f)) {
		t[strcspn(t, "\r\n ")] = '\0';
		snprintf(api_token, sizeof(api_token), "%s", t);
	}
	fclose(f);
}

static void save_token(const char *t)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(DIR_PATH, 0777);
	FILE *f = fopen(TOKEN_PATH, "w");
	if (f) {
		fprintf(f, "%s\n", t);
		fclose(f);
	}
	snprintf(api_token, sizeof(api_token), "%s", t);
}

void auth_logout(void)
{
	remove(TOKEN_PATH);
	api_token[0] = '\0';
}

static void set_state(auth_state st, const char *msg)
{
	LightLock_Lock(&s_lock);
	s_state_now = st;
	snprintf(s_msg, sizeof(s_msg), "%s", msg ? msg : "");
	LightLock_Unlock(&s_lock);
}

auth_state auth_status(char *msg, size_t cap)
{
	LightLock_Lock(&s_lock);
	auth_state st = s_state_now;
	if (msg)
		snprintf(msg, cap, "%s", s_msg);
	LightLock_Unlock(&s_lock);
	return st;
}

const char *auth_page_url(void)
{
	return s_page_url;
}

/* ------------------------------------------------------------ PKCE */

static void random_bytes(u8 *out, size_t n)
{
	bool ok = false;
	if (R_SUCCEEDED(psInit())) {
		ok = R_SUCCEEDED(PS_GenerateRandomBytes(out, n));
		psExit();
	}
	if (!ok) {
		srand((unsigned)osGetTime() ^ (unsigned)svcGetSystemTick());
		for (size_t i = 0; i < n; i++)
			out[i] = (u8)rand();
	}
}

static void b64url(char *dst, const u8 *src, size_t n)
{
	static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		u32 v = (u32)src[i] << 16;
		if (i + 1 < n) v |= (u32)src[i + 1] << 8;
		if (i + 2 < n) v |= src[i + 2];
		dst[o++] = A[v >> 18 & 63];
		dst[o++] = A[v >> 12 & 63];
		if (i + 1 < n) dst[o++] = A[v >> 6 & 63];
		if (i + 2 < n) dst[o++] = A[v & 63];
	}
	dst[o] = '\0';
}

static void make_pkce(void)
{
	u8 r[48];
	random_bytes(r, sizeof(r));
	b64url(s_verifier, r, 48);           /* 64 characters */
	u8 h[32];
	mbedtls_sha256_ret((const u8 *)s_verifier, strlen(s_verifier), h, 0);
	b64url(s_challenge, h, 32);
	random_bytes(r, 12);
	b64url(s_state, r, 12);
}

/* The redirect may name the console itself: "{ip}" stands for its address. */
static void redirect_uri(char *out, size_t cap)
{
	const char *p = strstr(s_redirect, "{ip}");
	if (!p) {
		snprintf(out, cap, "%s", s_redirect);
		return;
	}
	struct in_addr a = {.s_addr = (in_addr_t)gethostid()};
	snprintf(out, cap, "%.*s%s%s", (int)(p - s_redirect), s_redirect, inet_ntoa(a), p + 4);
}

static void authorize_url(char *out, size_t cap)
{
	char ru[200], ru_e[400];
	redirect_uri(ru, sizeof(ru));
	url_encode(ru_e, sizeof(ru_e), ru);
	snprintf(out, cap,
	         "https://www.are.na/oauth/authorize?client_id=%s&redirect_uri=%s"
	         "&response_type=code&scope=read%%20write&state=%s&code_challenge=%s"
	         "&code_challenge_method=S256",
	         s_client_id, ru_e, s_state, s_challenge);
}

/* ------------------------------------------------------------ signing in */

static bool try_token(const char *tok)
{
	char saved[128];
	snprintf(saved, sizeof(saved), "%s", api_token);
	snprintf(api_token, sizeof(api_token), "%s", tok);
	me_t me;
	bool unauth = false;
	if (api_me(&me, &unauth)) {
		save_token(tok);
		return true;
	}
	snprintf(api_token, sizeof(api_token), "%s", saved);
	return false;
}

static bool exchange(const char *code, char *why, size_t whycap)
{
	if (!auth_has_client()) {
		snprintf(why, whycap, "No Are.na app is set up for codes");
		return false;
	}
	char ru[200], ru_e[400], code_e[300], body[1200];
	redirect_uri(ru, sizeof(ru));
	url_encode(ru_e, sizeof(ru_e), ru);
	url_encode(code_e, sizeof(code_e), code);
	int n = snprintf(body, sizeof(body),
	                 "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s"
	                 "&code_verifier=%s",
	                 code_e, ru_e, s_client_id, s_verifier);
	if (s_client_secret[0])
		snprintf(body + n, sizeof(body) - n, "&client_secret=%s", s_client_secret);
	http_opts o = {.body = body};
	char *resp = NULL;
	long st = http_fetch("https://api.are.na/v3/oauth/token", &o, &resp, NULL);
	json_error_t je;
	json_t *j = resp ? json_loads(resp, 0, &je) : NULL;
	free(resp);
	const char *tok = json_string_value(json_object_get(j, "access_token"));
	bool ok = false;
	if (st == 200 && tok) {
		ok = try_token(tok);
		if (!ok)
			snprintf(why, whycap, "Are.na gave a token that does not work");
	} else {
		const char *e = json_string_value(json_object_get(j, "error_description"));
		if (!e)
			e = json_string_value(json_object_get(j, "error"));
		if (st == 0)
			snprintf(why, whycap, "No connection to Are.na");
		else
			snprintf(why, whycap, "Are.na said: %s", e ? e : "no");
	}
	json_decref(j);
	return ok;
}

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void url_decode(char *s)
{
	char *o = s;
	for (; *s; s++) {
		if (*s == '+') {
			*o++ = ' ';
		} else if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
			*o++ = (char)(hexv(s[1]) * 16 + hexv(s[2]));
			s += 2;
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
}

/* name=value out of a query string or form body, decoded. */
static bool param(const char *qs, const char *name, char *out, size_t cap)
{
	size_t nl = strlen(name);
	const char *p = qs;
	while (p && *p) {
		if (!strncmp(p, name, nl) && p[nl] == '=') {
			p += nl + 1;
			size_t l = strcspn(p, "&# \r\n");
			if (l >= cap)
				l = cap - 1;
			memcpy(out, p, l);
			out[l] = '\0';
			url_decode(out);
			return true;
		}
		p = strpbrk(p, "&?");
		if (p)
			p++;
	}
	return false;
}

/* Whatever was pasted: the address the phone ended up on (with code=),
 * a bare code, or a personal access token. */
static bool use_pasted(char *v, char *why, size_t whycap)
{
	char *s = v;
	while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')
		s++;
	s[strcspn(s, " \r\n\t")] = '\0';
	if (!*s) {
		snprintf(why, whycap, "Nothing was pasted");
		return false;
	}
	char code[256], state[64];
	const char *q = strchr(s, '?');
	if (param(q ? q + 1 : s, "code", code, sizeof(code))) {
		if (param(q ? q + 1 : s, "state", state, sizeof(state)) && strcmp(state, s_state)) {
			snprintf(why, whycap, "That login was for an older try; log in again");
			return false;
		}
		return exchange(code, why, whycap);
	}
	if (param(q ? q + 1 : s, "error", state, sizeof(state))) {
		snprintf(why, whycap, "Are.na said: %s", state);
		return false;
	}
	if (strlen(s) >= sizeof(code)) {
		snprintf(why, whycap, "That is too long to be a code or token");
		return false;
	}
	snprintf(code, sizeof(code), "%s", s);
	if (try_token(code))
		return true;
	if (auth_has_client() && exchange(code, why, whycap))
		return true;
	snprintf(why, whycap, "That code or token did not work");
	return false;
}

/* ------------------------------------------------------------ the page */

static const char PAGE_HEAD[] =
	"<!doctype html><html><head><meta charset=utf-8>"
	"<meta name=viewport content='width=device-width,initial-scale=1'>"
	"<title>Are.na on 3DS</title><style>"
	"body{background:#000;color:#fff;font:16px/1.4 -apple-system,Helvetica,Arial,sans-serif;"
	"margin:0 auto;padding:28px 20px;max-width:460px}"
	"h1{font-size:20px;margin:0 0 24px}h1 span{color:#8c8c8c}"
	"p{color:#8c8c8c;margin:14px 0}"
	".b,button{display:block;box-sizing:border-box;width:100%;background:#fff;color:#000;"
	"text-align:center;padding:14px;border:0;border-radius:4px;font:600 16px -apple-system,"
	"Helvetica,sans-serif;text-decoration:none;margin:16px 0}"
	"textarea{box-sizing:border-box;width:100%;height:96px;background:#1a1a1a;color:#fff;"
	"border:1px solid #333;border-radius:4px;padding:10px;font:13px ui-monospace,Menlo,monospace}"
	".ok{color:#7dbf6a}.err{color:#d0675f}"
	"</style></head><body><h1><span>Are.na /</span> 3DS</h1>";

static void send_all(int fd, const char *s, size_t n)
{
	while (n > 0) {
		ssize_t w = send(fd, s, n, 0);
		if (w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}

static void respond(int fd, const char *body)
{
	char hdr[160];
	size_t bl = strlen(PAGE_HEAD) + strlen(body) + strlen("</body></html>");
	int hn = snprintf(hdr, sizeof(hdr),
	                  "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
	                  "Content-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
	                  (unsigned)bl);
	send_all(fd, hdr, (size_t)hn);
	send_all(fd, PAGE_HEAD, strlen(PAGE_HEAD));
	send_all(fd, body, strlen(body));
	send_all(fd, "</body></html>", 14);
}

static void html_escape(char *dst, size_t cap, const char *src)
{
	size_t o = 0;
	for (; *src && o + 7 < cap; src++) {
		const char *r = NULL;
		switch (*src) {
		case '&': r = "&amp;"; break;
		case '<': r = "&lt;"; break;
		case '>': r = "&gt;"; break;
		case '"': r = "&quot;"; break;
		case '\'': r = "&#39;"; break;
		}
		if (r) {
			size_t l = strlen(r);
			memcpy(dst + o, r, l);
			o += l;
		} else {
			dst[o++] = *src;
		}
	}
	dst[o] = '\0';
}

static void page_home(int fd, const char *note, bool note_ok)
{
	char body[3000], au[900], au_h[1100], nt[640];
	size_t o = 0;
	body[0] = '\0';
	if (note) {
		html_escape(nt, sizeof(nt), note);
		o += snprintf(body + o, sizeof(body) - o, "<p class=%s>%s</p>", note_ok ? "ok" : "err", nt);
	}
	if (auth_has_client()) {
		authorize_url(au, sizeof(au));
		html_escape(au_h, sizeof(au_h), au);
		/* A new tab, so this page is still here to paste into. */
		o += snprintf(body + o, sizeof(body) - o,
		              "<a class=b target=_blank href=\"%s\">Log in with Are.na</a>%s",
		              au_h,
		              !strncmp(s_redirect, "urn:", 4)
		                  ? "<p>After you approve, Are.na shows a code. Copy it, come back to "
		                    "this tab and paste it below.</p>"
		                  : "");
	} else {
		o += snprintf(body + o, sizeof(body) - o,
		              "<p>Make a personal access token at "
		              "<a style='color:#fff' href='https://www.are.na/settings/personal-access-tokens'>"
		              "are.na/settings/personal-access-tokens</a> and paste it below.</p>");
	}
	snprintf(body + o, sizeof(body) - o,
	         "<form method=post action=/paste><textarea name=v autocapitalize=off "
	         "autocorrect=off spellcheck=false placeholder='Paste the %s'></textarea>"
	         "<button>Send to 3DS</button></form>",
	         !auth_has_client() ? "token here"
	         : !strncmp(s_redirect, "urn:", 4) ? "code here" : "address here");
	respond(fd, body);
}

static void page_done(int fd)
{
	respond(fd, "<p class=ok>You're logged in. Have a look at your 3DS.</p>"
	            "<p>You can close this page.</p>");
}

/* Read one request: the head, and a form body if there is one. */
static int read_request(int fd, char *buf, size_t cap)
{
	size_t len = 0;
	u64 start = osGetTime();
	while (len + 1 < cap && osGetTime() - start < 5000) {
		ssize_t r = recv(fd, buf + len, cap - 1 - len, 0);
		if (r > 0) {
			len += (size_t)r;
			buf[len] = '\0';
			char *end = strstr(buf, "\r\n\r\n");
			if (end) {
				const char *cl = strcasestr(buf, "\r\ncontent-length:");
				size_t want = cl ? (size_t)strtoul(cl + 17, NULL, 10) : 0;
				if (len >= (size_t)(end + 4 - buf) + want)
					return (int)len;
			}
		} else if (r == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			svcSleepThread(10000000LL);
		} else {
			break;
		}
	}
	buf[len] = '\0';
	return (int)len;
}

static void handle(int fd)
{
	static char req[8192];
	if (read_request(fd, req, sizeof(req)) <= 0)
		return;
	char method[8] = {0}, path[2048] = {0};
	sscanf(req, "%7s %2047s", method, path);
	char why[320] = "";

	if (!strcmp(method, "GET") && !strncmp(path, "/callback", 9)) {
		set_state(AUTH_WORKING, "Signing in...");
		if (use_pasted(path, why, sizeof(why))) {
			set_state(AUTH_DONE, "Logged in");
			page_done(fd);
		} else {
			set_state(AUTH_WAITING, why);
			page_home(fd, why, false);
		}
	} else if (!strcmp(method, "POST") && !strcmp(path, "/paste")) {
		char *body = strstr(req, "\r\n\r\n");
		static char v[2048];
		v[0] = '\0';
		if (body)
			param(body + 4, "v", v, sizeof(v));
		set_state(AUTH_WORKING, "Signing in...");
		if (use_pasted(v, why, sizeof(why))) {
			set_state(AUTH_DONE, "Logged in");
			page_done(fd);
		} else {
			set_state(AUTH_WAITING, why);
			page_home(fd, why, false);
		}
	} else if (!strcmp(method, "GET") && (!strcmp(path, "/") || path[1] == '?')) {
		set_state(AUTH_WAITING, "Page opened on your phone");
		page_home(fd, NULL, false);
	} else {
		const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		send_all(fd, nf, sizeof(nf) - 1);
	}
}

static void server(void *arg)
{
	(void)arg;
	while (!s_stop) {
		struct sockaddr_in ca;
		socklen_t cl = sizeof(ca);
		int fd = accept(s_listen, (struct sockaddr *)&ca, &cl);
		if (fd < 0) {
			svcSleepThread(50000000LL);
			continue;
		}
		/* The listening socket is non-blocking; the client need not be,
		 * but a stalled phone must not hold the console forever. */
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		handle(fd);
		shutdown(fd, SHUT_RDWR);
		close(fd);
		if (s_state_now == AUTH_DONE)
			break;
	}
}

static bool begin(char *err, size_t errcap);

bool auth_begin(char *err, size_t errcap)
{
	auth_cancel();
	if (begin(err, errcap))
		return true;
	set_state(AUTH_FAILED, err);
	return false;
}

static bool begin(char *err, size_t errcap)
{
	load_conf();
	u32 ip = (u32)gethostid();
	if (!ip || ip == 0x7F000001 || ip == 0x0100007F) {
		snprintf(err, errcap, "Connect to Wi-Fi first");
		return false;
	}
	make_pkce();
	s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s_listen < 0) {
		snprintf(err, errcap, "Could not open a network socket");
		return false;
	}
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons(PORT);
	a.sin_addr.s_addr = INADDR_ANY;
	if (bind(s_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s_listen, 4) < 0) {
		close(s_listen);
		s_listen = -1;
		snprintf(err, errcap, "Could not start the login page");
		return false;
	}
	fcntl(s_listen, F_SETFL, fcntl(s_listen, F_GETFL, 0) | O_NONBLOCK);
	struct in_addr ia = {.s_addr = (in_addr_t)ip};
	snprintf(s_page_url, sizeof(s_page_url), "http://%s:%d/", inet_ntoa(ia), PORT);
	s_stop = false;
	set_state(AUTH_WAITING, "");
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_thread = threadCreate(server, NULL, 96 * 1024, prio + 1, -2, false);
	return true;
}

void auth_cancel(void)
{
	s_stop = true;
	if (s_thread) {
		threadJoin(s_thread, 8000000000ULL);
		threadFree(s_thread);
		s_thread = NULL;
	}
	if (s_listen >= 0) {
		close(s_listen);
		s_listen = -1;
	}
	set_state(AUTH_IDLE, "");
}

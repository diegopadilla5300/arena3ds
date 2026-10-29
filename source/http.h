/* Small libcurl helpers shared by the API client and the image loader. */
#pragma once

#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define HTTP_CA_BUNDLE  "romfs:/cacert.pem"
#define HTTP_USER_AGENT "Arena-3DS/1.0"

/* Set when the app is closing: every transfer in flight gives up at once
 * instead of running out its timeout. */
extern volatile bool http_abort_all;

/* Once, after curl_global_init: lets every thread reuse connections and
 * TLS sessions, so a page of thumbnails does not pay a handshake each. */
void http_init(void);
void http_exit(void);

typedef struct {
	const char *method;       /* NULL for GET */
	const char *bearer;       /* Authorization: Bearer ..., or NULL */
	const char *body;         /* request body for POST/PUT */
	size_t      body_len;     /* for a binary body; 0 means a C string */
	const char *content_type; /* of the body */
	bool        no_auth_hdr;  /* uploads to S3 carry no Accept header */
	long        timeout;      /* seconds; 0 for the default */
} http_opts;

/* Fetch url into a NUL-terminated heap buffer. Returns the HTTP status
 * (0 when the transfer itself failed); *body is set whenever any came back,
 * error pages included, and the caller frees it. */
long http_fetch(const char *url, const http_opts *o, char **body, size_t *len);

/* GET that only counts a 200 as success. */
int http_get(const char *url, char **body, size_t *len);

/* Console clock minus UTC, learned from the API's Date header: the
 * console keeps local time and knows no timezone. */
extern volatile long http_clock_offset;
time_t http_now_utc(void);

/* Seconds since 1970 for a UTC calendar time, without relying on a
 * timezone database the console does not have. */
time_t utc_to_epoch(int y, int mo, int d, int h, int mi, int s);

/* "2026-10-28T21:38:40Z" or with fractional seconds; 0 if unparseable. */
time_t parse_iso(const char *s);

/* Percent-encode for a query string or form body. */
void url_encode(char *dst, size_t cap, const char *src);

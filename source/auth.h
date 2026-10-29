/* Signing in without typing on the 3DS: the console serves a small page
 * on the local network, the phone opens it from a QR code, logs in to
 * Are.na there (OAuth with PKCE), and the code comes back to the console --
 * straight to it, or pasted into that page. A personal access token pasted
 * into the same box works too. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	AUTH_IDLE,
	AUTH_WAITING,     /* the page is up, waiting for the phone */
	AUTH_WORKING,     /* exchanging the code */
	AUTH_DONE,
	AUTH_FAILED,
} auth_state;

/* Read a saved token into api_token. */
void auth_load(void);
/* Start the page; false (with a reason) if there is no network. */
bool auth_begin(char *err, size_t errcap);
void auth_cancel(void);
auth_state auth_status(char *msg, size_t cap);
/* "http://192.168.1.20:8080/" while waiting. */
const char *auth_page_url(void);
/* Whether an OAuth app is configured (otherwise: token paste only). */
bool auth_has_client(void);
void auth_logout(void);

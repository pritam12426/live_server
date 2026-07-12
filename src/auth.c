/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * auth.c — HTTP Basic Authentication
 *
 * Decodes the Base64-encoded "Authorization: Basic …" header
 * and compares against expected user/password.
 */

#include "auth.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "response.h"
#include "transport.h"

// Map a single Base64 character to its 6-bit value, or -1 on error
static int b64_char(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+')             return 62;
	if (c == '/')             return 63;
	return -1;
}

// Decode a Base64 string into raw bytes (null-terminated)
// Returns the decoded length, or -1 on invalid input
static int b64_decode(const char *src, char *dst, int dst_len)
{
	int out = 0;

	while (*src) {
		int      v[4] = {0, 0, 0, 0};
		int      pad  = 0;
		int      i;

		// Read up to 4 Base64 characters
		for (i = 0; i < 4; i++) {
			char c = *src;
			if (c == '\0') {
				if (i == 0) goto done;
				return -1;
			}
			if (c == '=') {
				pad++;
				src++;
				// Skip remaining padding chars
				for (i++; i < 4; i++) {
					if (*src == '=') { src++; pad++; }
				}
				break;
			}
			int n = b64_char(c);
			if (n < 0) return -1;
			v[i] = n;
			src++;
		}

		// Combine 4×6 bits into 3×8 bits
		uint32_t bits = ((uint32_t)v[0] << 18)
		                | ((uint32_t)v[1] << 12)
		                | ((uint32_t)v[2] <<  6)
		                |  (uint32_t)v[3];

		if (out < dst_len - 1) dst[out++] = (char)((bits >> 16) & 0xff);
		if (pad < 2 && out < dst_len - 1) dst[out++] = (char)((bits >>  8) & 0xff);
		if (pad < 1 && out < dst_len - 1) dst[out++] = (char)( bits        & 0xff);
	}

done:
	dst[out] = '\0';
	return out;
}

// Check the Authorization header against expected credentials
// Returns 1 if authorised, 0 if denied
int auth_check(const HttpRequest *req,
               const char        *expected_user,
               const char        *expected_pass)
{
	int user_set = expected_user && *expected_user;
	int pass_set = expected_pass && *expected_pass;

	LOG_DEBUG("auth_check: expected_user='%s', expected_pass='%s', user_set=%d, pass_set=%d",
              expected_user ? expected_user : "(null)",
              expected_pass ? expected_pass : "(null)",
              user_set, pass_set);

	// If neither user nor pass is configured, auth is disabled
	if (!user_set && !pass_set) {
		LOG_DEBUG("Auth disabled — no credentials configured, granting access");
		return 1;
	}

	const char *hdr = req->auth;
	if (!hdr) {
		LOG_DEBUG("No Authorization header present");
		return 0;
	}
	LOG_DEBUG("Authorization header: %s", hdr);

	// Must be "Basic …"
	if (strncasecmp(hdr, "Basic ", 6) != 0) {
		LOG_DEBUG("Authorization scheme is not Basic (got: %.20s)", hdr);
		return 0;
	}

	// Extract and decode the Base64 payload
	const char *b64 = hdr + 6;
	while (*b64 == ' ') b64++;
	LOG_DEBUG("Base64 credentials: %s", b64);

	char decoded[512];
	int  dec_len = b64_decode(b64, decoded, (int)sizeof decoded);
	if (dec_len < 0) {
		LOG_DEBUG("Base64 decode failed");
		return 0;
	}
	LOG_DEBUG("Base64 decoded (%d bytes)", dec_len);

	// Split on ":" to get user:pass
	char *colon = strchr(decoded, ':');
	if (!colon) {
		LOG_DEBUG("Decoded credentials missing ':' separator");
		return 0;
	}
	*colon = '\0';

	const char *got_user  = decoded;
	const char *got_pass  = colon + 1;
	const char *want_user = expected_user ? expected_user : "";
	const char *want_pass = expected_pass ? expected_pass : "";

	int ok = (strcmp(got_user, want_user) == 0 &&
			  strcmp(got_pass, want_pass) == 0);
	LOG_DEBUG("Auth result: %s", ok ? "ACCEPT" : "REJECT");
	return ok ? 1 : 0;
}

// Send a 401 response with a WWW-Authenticate challenge header
// This triggers the browser's login dialog
void auth_send_challenge(Transport *t)
{
	const char *extra =
	    "WWW-Authenticate: Basic realm=\"live-server\"\r\n"
	    "Cache-Control: no-store\r\n";
	const char *body = "<h1>401 Unauthorized</h1>";
	response_send(t, 401, "Unauthorized",
	                "text/html; charset=utf-8",
	                extra,
	                body, strlen(body), 0, 1);
}

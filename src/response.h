/*
 * response.h — HTTP response builder API
 */

#ifndef _RESPONSE_H_
#define _RESPONSE_H_

#include <stddef.h>

typedef struct Transport Transport;

// Send a full HTTP response with headers + optional body
// If send_body is 0, Content-Length is still set correctly but the body is not
// written to the wire (required for HEAD responses per RFC 9110 §9.3.4).
void response_send(Transport   *t,
                   int         status,
                   const char *status_text,
                   const char *mime,
                   const char *extra_hdrs,
                   const char *body,
                   size_t      body_len,
                   int         keep_alive,
                   int         send_body);

// Send a styled HTML error page
void response_error(Transport *t, int status, const char *status_text, const char *detail);

// Send a 301 redirect
void response_redirect(Transport *t, const char *location);

// (SSE helpers live in livereload.h — not here)

#endif  // _RESPONSE_H_

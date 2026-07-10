/*
 * auth.h — HTTP Basic Authentication
 */

#ifndef _AUTH_H_
#define _AUTH_H_


#include "http.h"

typedef struct Transport Transport;

// Verify the Authorization header against expected credentials
// Returns 1 on success, 0 on failure
int auth_check(const HttpRequest *req,
               const char        *expected_user,
               const char        *expected_pass);

// Send a 401 with WWW-Authenticate challenge
void auth_send_challenge(Transport *t);


#endif  // _AUTH_H_

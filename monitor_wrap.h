/*	$OpenBSD: monitor_wrap.h,v 1.5 2002/05/12 23:53:45 djm Exp $	*/

/*
 * Copyright 2002 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _MM_WRAP_H_
#define _MM_WRAP_H_
#include "key.h"
#include "buffer.h"

#include <soaap.h>

// For our SOAAP analysis we want use_privsep to be always true:
// extern int use_privsep;
#define use_privsep 1

// always call the mm_functions:
// #define PRIVSEP(x)	(use_privsep ? mm_##x : x)
#define PRIVSEP(x)	mm_##x

enum mm_keytype {MM_NOKEY, MM_HOSTKEY, MM_USERKEY, MM_RSAHOSTKEY, MM_RSAUSERKEY};

struct monitor;
struct mm_master;
struct passwd;
struct Authctxt;

__soaap_sandboxed("preauth", "postauth") DH *mm_choose_dh(int, int, int);
__soaap_sandboxed("preauth", "postauth") int mm_key_sign(Key *, u_char **, u_int *, u_char *, u_int);
__soaap_sandboxed("preauth") void mm_inform_authserv(char *, char *);
__soaap_sandboxed("preauth") struct passwd *mm_getpwnamallow(const char *);
__soaap_sandboxed("preauth") char* mm_auth2_read_banner(void);
__soaap_sandboxed("preauth") int mm_auth_password(struct Authctxt *, char *);
__soaap_sandboxed("preauth") int mm_key_allowed(enum mm_keytype, char *, char *, Key *);
__soaap_sandboxed("preauth") int mm_user_key_allowed(struct passwd *, Key *);
__soaap_sandboxed("preauth") int mm_hostbased_key_allowed(struct passwd *, char *, char *, Key *);
__soaap_sandboxed("preauth") int mm_auth_rhosts_rsa_key_allowed(struct passwd *, char *, char *, Key *);
__soaap_sandboxed("preauth") int mm_key_verify(Key *, u_char *, u_int, u_char *, u_int);
__soaap_sandboxed("preauth") int mm_auth_rsa_key_allowed(struct passwd *, BIGNUM *, Key **);
__soaap_sandboxed("preauth") int mm_auth_rsa_verify_response(Key *, BIGNUM *, u_char *);
__soaap_sandboxed("preauth") BIGNUM *mm_auth_rsa_generate_challenge(Key *);

#ifdef USE_PAM
__soaap_sandboxed("preauth") void mm_start_pam(char *);
#endif

__soaap_sandboxed("preauth", "postauth") void mm_terminate(void);
__soaap_sandboxed("postauth") int mm_pty_allocate(int *, int *, char *, int);
__soaap_sandboxed("postauth") void mm_session_pty_cleanup2(void *);

/* SSHv1 interfaces */
__soaap_sandboxed("preauth") void mm_ssh1_session_id(u_char *);
__soaap_sandboxed("preauth") int mm_ssh1_session_key(BIGNUM *);

/* Key export functions */
__soaap_privileged struct Newkeys *mm_newkeys_from_blob(u_char *, int);
__soaap_sandboxed("preauth") int mm_newkeys_to_blob(int, u_char **, u_int *);

__soaap_privileged void monitor_apply_keystate(struct monitor *);
__soaap_privileged void mm_get_keystate(struct monitor *);
__soaap_sandboxed("preauth") void mm_send_keystate(struct monitor*);

/* bsdauth */
__soaap_sandboxed("preauth") int mm_bsdauth_query(void *, char **, char **, u_int *, char ***, u_int **);
__soaap_sandboxed("preauth") int mm_bsdauth_respond(void *, u_int, char **);

/* skey */
__soaap_sandboxed("preauth") int mm_skey_query(void *, char **, char **, u_int *, char ***, u_int **);
__soaap_sandboxed("preauth") int mm_skey_respond(void *, u_int, char **);

/* zlib allocation hooks */

__soaap_sandboxed("preauth") void *mm_zalloc(struct mm_master *, u_int, u_int);
__soaap_sandboxed("preauth") void mm_zfree(struct mm_master *, void *);
__soaap_privileged void mm_init_compression(struct mm_master *);

#endif /* _MM_H_ */

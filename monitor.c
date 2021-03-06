/*
 * Copyright 2002 Niels Provos <provos@citi.umich.edu>
 * Copyright 2002 Markus Friedl <markus@openbsd.org>
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

#include "includes.h"
RCSID("$OpenBSD: monitor.c,v 1.11 2002/05/15 15:47:49 mouring Exp $");

#include <openssl/dh.h>

#ifdef SKEY
#include <skey.h>
#endif

#include "ssh.h"
#include "auth.h"
#include "kex.h"
#include "dh.h"
#include "zlib.h"
#include "packet.h"
#include "auth-options.h"
#include "sshpty.h"
#include "channels.h"
#include "session.h"
#include "sshlogin.h"
#include "canohost.h"
#include "log.h"
#include "servconf.h"
#include "monitor.h"
#include "monitor_mm.h"
#include "monitor_wrap.h"
#include "monitor_fdpass.h"
#include "xmalloc.h"
#include "misc.h"
#include "buffer.h"
#include "bufaux.h"
#include "compat.h"
#include "ssh2.h"
#include "mpaux.h"

/* Imports */
extern ServerOptions options;
extern u_int utmp_len;
extern Newkeys *current_keys[];
extern z_stream incoming_stream;
extern z_stream outgoing_stream;
extern u_char session_id[];
extern Buffer input, output;
extern Buffer auth_debug;
extern int auth_debug_init;

/* State exported from the child */

struct {
	z_stream incoming;
	z_stream outgoing;
	u_char *keyin;
	u_int keyinlen;
	u_char *keyout;
	u_int keyoutlen;
	u_char *ivin;
	u_int ivinlen;
	u_char *ivout;
	u_int ivoutlen;
	int ssh1cipher;
	int ssh1protoflags;
	u_char *input;
	u_int ilen;
	u_char *output;
	u_int olen;
} child_state;

/* Functions on the montior that answer unprivileged requests */

int mm_answer_moduli(int, Buffer *);
int mm_answer_sign(int, Buffer *);
int mm_answer_pwnamallow(int, Buffer *);
int mm_answer_auth2_read_banner(int, Buffer *);
int mm_answer_authserv(int, Buffer *);
int mm_answer_authpassword(int, Buffer *);
int mm_answer_bsdauthquery(int, Buffer *);
int mm_answer_bsdauthrespond(int, Buffer *);
int mm_answer_skeyquery(int, Buffer *);
int mm_answer_skeyrespond(int, Buffer *);
int mm_answer_keyallowed(int, Buffer *);
int mm_answer_keyverify(int, Buffer *);
int mm_answer_pty(int, Buffer *);
int mm_answer_pty_cleanup(int, Buffer *);
int mm_answer_term(int, Buffer *);
int mm_answer_rsa_keyallowed(int, Buffer *);
int mm_answer_rsa_challenge(int, Buffer *);
int mm_answer_rsa_response(int, Buffer *);
int mm_answer_sesskey(int, Buffer *);
int mm_answer_sessid(int, Buffer *);

#ifdef USE_PAM
int mm_answer_pam_start(int, Buffer *);
#endif

static Authctxt *authctxt;
static BIGNUM *ssh1_challenge = NULL;	/* used for ssh1 rsa auth */

/* local state for key verify */
static u_char *key_blob = NULL;
static u_int key_bloblen = 0;
static int key_blobtype = MM_NOKEY;
static u_char *hostbased_cuser = NULL;
static u_char *hostbased_chost = NULL;
static char *auth_method = "unknown";

struct mon_table {
	enum monitor_reqtype type;
	int flags;
	int (*f)(int, Buffer *);
};

#define MON_ISAUTH	0x0004	/* Required for Authentication */
#define MON_AUTHDECIDE	0x0008	/* Decides Authentication */
#define MON_ONCE	0x0010	/* Disable after calling */

#define MON_AUTH	(MON_ISAUTH|MON_AUTHDECIDE)

#define MON_PERMIT	0x1000	/* Request is permitted */

struct mon_table mon_dispatch_proto20[] = {
    {MONITOR_REQ_MODULI, MON_ONCE, mm_answer_moduli},
    {MONITOR_REQ_SIGN, MON_ONCE, mm_answer_sign},
    {MONITOR_REQ_PWNAM, MON_ONCE, mm_answer_pwnamallow},
    {MONITOR_REQ_AUTHSERV, MON_ONCE, mm_answer_authserv},
    {MONITOR_REQ_AUTH2_READ_BANNER, MON_ONCE, mm_answer_auth2_read_banner},
    {MONITOR_REQ_AUTHPASSWORD, MON_AUTH, mm_answer_authpassword},
#ifdef USE_PAM
    {MONITOR_REQ_PAM_START, MON_ONCE, mm_answer_pam_start},
#endif
#ifdef BSD_AUTH
    {MONITOR_REQ_BSDAUTHQUERY, MON_ISAUTH, mm_answer_bsdauthquery},
    {MONITOR_REQ_BSDAUTHRESPOND, MON_AUTH,mm_answer_bsdauthrespond},
#endif
#ifdef SKEY
    {MONITOR_REQ_SKEYQUERY, MON_ISAUTH, mm_answer_skeyquery},
    {MONITOR_REQ_SKEYRESPOND, MON_AUTH, mm_answer_skeyrespond},
#endif
    {MONITOR_REQ_KEYALLOWED, MON_ISAUTH, mm_answer_keyallowed},
    {MONITOR_REQ_KEYVERIFY, MON_AUTH, mm_answer_keyverify},
    {0, 0, NULL}
};

struct mon_table mon_dispatch_postauth20[] = {
    {MONITOR_REQ_MODULI, 0, mm_answer_moduli},
    {MONITOR_REQ_SIGN, 0, mm_answer_sign},
    {MONITOR_REQ_PTY, 0, mm_answer_pty},
    {MONITOR_REQ_PTYCLEANUP, 0, mm_answer_pty_cleanup},
    {MONITOR_REQ_TERM, 0, mm_answer_term},
    {0, 0, NULL}
};

struct mon_table mon_dispatch_proto15[] = {
    {MONITOR_REQ_PWNAM, MON_ONCE, mm_answer_pwnamallow},
    {MONITOR_REQ_SESSKEY, MON_ONCE, mm_answer_sesskey},
    {MONITOR_REQ_SESSID, MON_ONCE, mm_answer_sessid},
    {MONITOR_REQ_AUTHPASSWORD, MON_AUTH, mm_answer_authpassword},
    {MONITOR_REQ_RSAKEYALLOWED, MON_ISAUTH, mm_answer_rsa_keyallowed},
    {MONITOR_REQ_KEYALLOWED, MON_ISAUTH, mm_answer_keyallowed},
    {MONITOR_REQ_RSACHALLENGE, MON_ONCE, mm_answer_rsa_challenge},
    {MONITOR_REQ_RSARESPONSE, MON_ONCE|MON_AUTHDECIDE, mm_answer_rsa_response},
#ifdef USE_PAM
    {MONITOR_REQ_PAM_START, MON_ONCE, mm_answer_pam_start},
#endif
#ifdef BSD_AUTH
    {MONITOR_REQ_BSDAUTHQUERY, MON_ISAUTH, mm_answer_bsdauthquery},
    {MONITOR_REQ_BSDAUTHRESPOND, MON_AUTH,mm_answer_bsdauthrespond},
#endif
#ifdef SKEY
    {MONITOR_REQ_SKEYQUERY, MON_ISAUTH, mm_answer_skeyquery},
    {MONITOR_REQ_SKEYRESPOND, MON_AUTH, mm_answer_skeyrespond},
#endif
#ifdef USE_PAM
    {MONITOR_REQ_PAM_START, MON_ONCE, mm_answer_pam_start}, // WTF: this is listed twice
#endif
    {0, 0, NULL}
};

struct mon_table mon_dispatch_postauth15[] = {
    {MONITOR_REQ_PTY, MON_ONCE, mm_answer_pty},
    {MONITOR_REQ_PTYCLEANUP, MON_ONCE, mm_answer_pty_cleanup},
    {MONITOR_REQ_TERM, 0, mm_answer_term},
    {0, 0, NULL}
};

struct mon_table *mon_dispatch;

/* Specifies if a certain message is allowed at the moment */

static void
monitor_permit(struct mon_table *ent, enum monitor_reqtype type, int permit)
{
	while (ent->f != NULL) {
		if (ent->type == type) {
			ent->flags &= ~MON_PERMIT;
			ent->flags |= permit ? MON_PERMIT : 0;
			return;
		}
		ent++;
	}
}

static void
monitor_permit_authentications(int permit)
{
	struct mon_table *ent = mon_dispatch;

	while (ent->f != NULL) {
		if (ent->flags & MON_AUTH) {
			ent->flags &= ~MON_PERMIT;
			ent->flags |= permit ? MON_PERMIT : 0;
		}
		ent++;
	}
}

Authctxt *
monitor_child_preauth(struct monitor *pmonitor)
{
	struct mon_table *ent;
	int authenticated = 0;

	debug3("preauth child monitor started");

	if (compat20) {
		mon_dispatch = mon_dispatch_proto20;

		/* Permit requests for moduli and signatures */
		monitor_permit(mon_dispatch, MONITOR_REQ_MODULI, 1);
		monitor_permit(mon_dispatch, MONITOR_REQ_SIGN, 1);
	} else {
		mon_dispatch = mon_dispatch_proto15;

		monitor_permit(mon_dispatch, MONITOR_REQ_SESSKEY, 1);
	}

	authctxt = authctxt_new();

	/* The first few requests do not require asynchronous access */
	while (!authenticated) {
		authenticated = monitor_read(pmonitor, mon_dispatch, &ent);

		// here we get all the RPC messages
		// obviously some of them are only allowed in certain states, but for now listing all possibilities here seems to be a good idea
		__soaap_rpc_recv("preauth", MONITOR_REQ_MODULI, mm_answer_moduli);
		__soaap_rpc_recv("preauth", MONITOR_REQ_SIGN, mm_answer_sign);
		__soaap_rpc_recv("preauth", MONITOR_REQ_PWNAM, mm_answer_pwnamallow);
		__soaap_rpc_recv("preauth", MONITOR_REQ_AUTHSERV, mm_answer_authserv);
		__soaap_rpc_recv("preauth", MONITOR_REQ_AUTH2_READ_BANNER, mm_answer_auth2_read_banner);
		__soaap_rpc_recv("preauth", MONITOR_REQ_AUTHPASSWORD, mm_answer_authpassword);
#ifdef USE_PAM
		__soaap_rpc_recv("preauth", MONITOR_REQ_PAM_START, mm_answer_pam_start);
#endif
#ifdef BSD_AUTH
		__soaap_rpc_recv("preauth", MONITOR_REQ_BSDAUTHQUERY, mm_answer_bsdauthquery);
		__soaap_rpc_recv("preauth", MONITOR_REQ_BSDAUTHRESPOND, mm_answer_bsdauthrespond);
#endif
#ifdef SKEY
		__soaap_rpc_recv("preauth", MONITOR_REQ_SKEYQUERY, mm_answer_skeyquery);
		__soaap_rpc_recv("preauth", MONITOR_REQ_SKEYRESPOND, mm_answer_skeyrespond);
#endif

		__soaap_rpc_recv("preauth", MONITOR_REQ_KEYALLOWED, mm_answer_keyallowed);
		__soaap_rpc_recv("preauth", MONITOR_REQ_KEYVERIFY, mm_answer_keyverify);

		// now the ones from the proto15 table that weren't already listed:
		__soaap_rpc_recv("preauth", MONITOR_REQ_SESSKEY, mm_answer_sesskey);
		__soaap_rpc_recv("preauth", MONITOR_REQ_SESSID, mm_answer_sessid);
		__soaap_rpc_recv("preauth", MONITOR_REQ_RSAKEYALLOWED, mm_answer_rsa_keyallowed);
		__soaap_rpc_recv("preauth", MONITOR_REQ_RSACHALLENGE, mm_answer_rsa_challenge);
		__soaap_rpc_recv("preauth", MONITOR_REQ_RSARESPONSE, mm_answer_rsa_response);

		if (authenticated) {
			if (!(ent->flags & MON_AUTHDECIDE))
				fatal("%s: unexpected authentication from %d",
				    __FUNCTION__, ent->type);
			if (authctxt->pw->pw_uid == 0 &&
			    !auth_root_allowed(auth_method))
				authenticated = 0;
#ifdef USE_PAM
			if (!do_pam_account(authctxt->pw->pw_name, NULL))
				authenticated = 0;
#endif
		}

		if (ent->flags & MON_AUTHDECIDE) {
			auth_log(authctxt, authenticated, auth_method,
			    compat20 ? " ssh2" : "");
			if (!authenticated)
				authctxt->failures++;
		}
	}

	if (!authctxt->valid)
		fatal("%s: authenticated invalid user", __FUNCTION__);

	debug("%s: %s has been authenticated by privileged process",
	    __FUNCTION__, authctxt->user);

	mm_get_keystate(pmonitor);

	return (authctxt);
}

void
monitor_child_postauth(struct monitor *pmonitor)
{
	if (compat20) {
		mon_dispatch = mon_dispatch_postauth20;

		/* Permit requests for moduli and signatures */
		monitor_permit(mon_dispatch, MONITOR_REQ_MODULI, 1);
		monitor_permit(mon_dispatch, MONITOR_REQ_SIGN, 1);
		monitor_permit(mon_dispatch, MONITOR_REQ_TERM, 1);

	} else {
		mon_dispatch = mon_dispatch_postauth15;
		monitor_permit(mon_dispatch, MONITOR_REQ_TERM, 1);
	}
	if (!no_pty_flag) {
		monitor_permit(mon_dispatch, MONITOR_REQ_PTY, 1);
		monitor_permit(mon_dispatch, MONITOR_REQ_PTYCLEANUP, 1);
	}

	for (;;) {
		monitor_read(pmonitor, mon_dispatch, NULL);

	__soaap_rpc_recv("postauth", MONITOR_REQ_MODULI, mm_answer_moduli); // compat20 only
	__soaap_rpc_recv("postauth", MONITOR_REQ_SIGN, mm_answer_sign); // compat20 only
	// common to both:
	__soaap_rpc_recv("postauth", MONITOR_REQ_PTY, mm_answer_pty);
	__soaap_rpc_recv("postauth", MONITOR_REQ_PTYCLEANUP, mm_answer_pty_cleanup);
	// this RPC call causes the monitor process to exit (and hopefully the child as well)
	__soaap_rpc_recv("postauth", MONITOR_REQ_TERM, mm_answer_term);
	}
}

void
monitor_sync(struct monitor *pmonitor)
{
	/* The member allocation is not visible, so sync it */
	mm_share_sync(&pmonitor->m_zlib, &pmonitor->m_zback);
}

int
monitor_read(struct monitor *pmonitor, struct mon_table *ent,
    struct mon_table **pent)
{
	Buffer m;
	int ret;
	u_char type;

	buffer_init(&m);

	mm_request_receive(pmonitor->m_sendfd, &m);
	type = buffer_get_char(&m);

	debug3("%s: checking request %d", __FUNCTION__, type);

	while (ent->f != NULL) {
		if (ent->type == type)
			break;
		ent++;
	}

	if (ent->f != NULL) {
		if (!(ent->flags & MON_PERMIT))
			fatal("%s: unpermitted request %d", __FUNCTION__,
			    type);
		ret = (*ent->f)(pmonitor->m_sendfd, &m);
		buffer_free(&m);

		/* The child may use this request only once, disable it */
		if (ent->flags & MON_ONCE) {
			debug2("%s: %d used once, disabling now", __FUNCTION__,
			    type);
			ent->flags &= ~MON_PERMIT;
		}

		if (pent != NULL)
			*pent = ent;

		return ret;
	}

	fatal("%s: unsupported request: %d", __FUNCTION__, type);

	/* NOTREACHED */
	return (-1);
}

/* allowed key state */
static int
monitor_allowed_key(u_char *blob, u_int bloblen)
{
	/* make sure key is allowed */
	if (key_blob == NULL || key_bloblen != bloblen ||
	    memcmp(key_blob, blob, key_bloblen))
		return (0);
	return (1);
}

static void
monitor_reset_key_state(void)
{
	/* reset state */
	if (key_blob != NULL)
		xfree(key_blob);
	if (hostbased_cuser != NULL)
		xfree(hostbased_cuser);
	if (hostbased_chost != NULL)
		xfree(hostbased_chost);
	key_blob = NULL;
	key_bloblen = 0;
	key_blobtype = MM_NOKEY;
	hostbased_cuser = NULL;
	hostbased_chost = NULL;
}

int
mm_answer_moduli(int socket, Buffer *m)
{
	DH *dh;
	int min, want, max;

	min = buffer_get_int(m);
	want = buffer_get_int(m);
	max = buffer_get_int(m);

	debug3("%s: got parameters: %d %d %d",
	    __FUNCTION__, min, want, max);
	/* We need to check here, too, in case the child got corrupted */
	if (max < min || want < min || max < want)
		fatal("%s: bad parameters: %d %d %d",
		    __FUNCTION__, min, want, max);

	buffer_clear(m);

	dh = choose_dh(min, want, max);
	if (dh == NULL) {
		buffer_put_char(m, 0);
		return (0);
	} else {
		/* Send first bignum */
		buffer_put_char(m, 1);
		buffer_put_bignum2(m, dh->p);
		buffer_put_bignum2(m, dh->g);

		DH_free(dh);
	}
	mm_request_send("preauth", socket, MONITOR_ANS_MODULI, m);
	__soaap_rpc_send_with_params("postauth", MONITOR_ANS_MODULI, m);
	return (0);
}

void noop_fp_target() {}


int
mm_answer_sign(int socket, Buffer *m)
{
	Key *key;
	u_char *p;
	u_char *signature;
	u_int siglen, datlen;
	int keyid;

	debug3("%s", __FUNCTION__);

	keyid = buffer_get_int(m);
	p = buffer_get_string(m, &datlen);

	if (datlen != 20)
		fatal("%s: data length incorrect: %d", __FUNCTION__, datlen);

	if ((key = get_hostkey_by_index(keyid)) == NULL)
		fatal("%s: no hostkey from index %d", __FUNCTION__, keyid);
	if (key_sign(key, &signature, &siglen, p, datlen) < 0)
		fatal("%s: key_sign failed", __FUNCTION__);

	debug3("%s: signature %p(%d)", __FUNCTION__, signature, siglen);

	buffer_clear(m);
	buffer_put_string(m, signature, siglen);

	xfree(p);
	xfree(signature);

	mm_request_send("preauth", socket, MONITOR_ANS_SIGN, m);
	__soaap_rpc_send_with_params("postauth", MONITOR_ANS_SIGN, m);

	/* Turn on permissions for getpwnam */
	monitor_permit(mon_dispatch, MONITOR_REQ_PWNAM, 1);

	return (0);
}

/* Retrieves the password entry and also checks if the user is permitted */

int
mm_answer_pwnamallow(int socket, Buffer *m)
{
	char *login;
	struct passwd *pwent;
	int allowed = 0;

	debug3("%s", __FUNCTION__);

	if (authctxt->attempt++ != 0)
		fatal("%s: multiple attempts for getpwnam", __FUNCTION__);

	login = buffer_get_string(m, NULL);

	pwent = getpwnamallow(login);

	authctxt->user = xstrdup(login);
	setproctitle("%s [priv]", pwent ? login : "unknown");
	xfree(login);

	buffer_clear(m);

	if (pwent == NULL) {
		buffer_put_char(m, 0);
		goto out;
	}

	allowed = 1;
	authctxt->pw = pwent;
	authctxt->valid = 1;

	buffer_put_char(m, 1);
	buffer_put_string(m, pwent, sizeof(struct passwd));
	buffer_put_cstring(m, pwent->pw_name);
	buffer_put_cstring(m, "*");
	buffer_put_cstring(m, pwent->pw_gecos);
#ifdef HAVE_PW_CLASS_IN_PASSWD
	buffer_put_cstring(m, pwent->pw_class);
#endif
	buffer_put_cstring(m, pwent->pw_dir);
	buffer_put_cstring(m, pwent->pw_shell);

 out:
	debug3("%s: sending MONITOR_ANS_PWNAM: %d", __FUNCTION__, allowed);
	mm_request_send("preauth", socket, MONITOR_ANS_PWNAM, m);

	/* For SSHv1 allow authentication now */
	if (!compat20)
		monitor_permit_authentications(1);
	else {
		/* Allow service/style information on the auth context */
		monitor_permit(mon_dispatch, MONITOR_REQ_AUTHSERV, 1);
		monitor_permit(mon_dispatch, MONITOR_REQ_AUTH2_READ_BANNER, 1);
	}

#ifdef USE_PAM
	monitor_permit(mon_dispatch, MONITOR_REQ_PAM_START, 1);
#endif

	return (0);
}

int mm_answer_auth2_read_banner(int socket, Buffer *m)
{
	char *banner;

	buffer_clear(m);
	banner = auth2_read_banner();
	buffer_put_cstring(m, banner != NULL ? banner : "");
	mm_request_send("preauth", socket, MONITOR_ANS_AUTH2_READ_BANNER, m);

	if (banner != NULL)
		free(banner);

	return (0);
}

int
mm_answer_authserv(int socket, Buffer *m)
{
	monitor_permit_authentications(1);

	authctxt->service = buffer_get_string(m, NULL);
	authctxt->style = buffer_get_string(m, NULL);
	debug3("%s: service=%s, style=%s",
	    __FUNCTION__, authctxt->service, authctxt->style);

	if (strlen(authctxt->style) == 0) {
		xfree(authctxt->style);
		authctxt->style = NULL;
	}

	return (0);
}

int
mm_answer_authpassword(int socket, Buffer *m)
{
	static int call_count;
	char *passwd;
	int authenticated, plen;

	passwd = buffer_get_string(m, &plen);
	/* Only authenticate if the context is valid */
	authenticated = authctxt->valid && auth_password(authctxt, passwd);
	memset(passwd, 0, strlen(passwd));
	xfree(passwd);

	buffer_clear(m);
	buffer_put_int(m, authenticated);

	debug3("%s: sending result %d", __FUNCTION__, authenticated);
	mm_request_send("preauth", socket, MONITOR_ANS_AUTHPASSWORD, m);

	call_count++;
	if (plen == 0 && call_count == 1)
		auth_method = "none";
	else
		auth_method = "password";

	/* Causes monitor loop to terminate if authenticated */
	return (authenticated);
}

#ifdef BSD_AUTH
int
mm_answer_bsdauthquery(int socket, Buffer *m)
{
	char *name, *infotxt;
	u_int numprompts;
	u_int *echo_on;
	char **prompts;
	int res;

	res = bsdauth_query(authctxt, &name, &infotxt, &numprompts,
	    &prompts, &echo_on);

	buffer_clear(m);
	buffer_put_int(m, res);
	if (res != -1)
		buffer_put_cstring(m, prompts[0]);

	debug3("%s: sending challenge res: %d", __FUNCTION__, res);
	mm_request_send(socket, MONITOR_ANS_BSDAUTHQUERY, m);

	if (res != -1) {
		xfree(name);
		xfree(infotxt);
		xfree(prompts);
		xfree(echo_on);
	}

	return (0);
}

int
mm_answer_bsdauthrespond(int socket, Buffer *m)
{
	char *response;
	int authok;

	if (authctxt->as == 0)
		fatal("%s: no bsd auth session", __FUNCTION__);

	response = buffer_get_string(m, NULL);
	authok = auth_userresponse(authctxt->as, response, 0);
	authctxt->as = NULL;
	debug3("%s: <%s> = <%d>", __FUNCTION__, response, authok);
	xfree(response);

	buffer_clear(m);
	buffer_put_int(m, authok);

	debug3("%s: sending authenticated: %d", __FUNCTION__, authok);
	mm_request_send("preauth", socket, MONITOR_ANS_BSDAUTHRESPOND, m);

	auth_method = "bsdauth";

	return (authok != 0);
}
#endif

#ifdef SKEY
int
mm_answer_skeyquery(int socket, Buffer *m)
{
	struct skey skey;
	char challenge[1024];
	int res;

	res = skeychallenge(&skey, authctxt->user, challenge);

	buffer_clear(m);
	buffer_put_int(m, res);
	if (res != -1)
		buffer_put_cstring(m, challenge);

	debug3("%s: sending challenge res: %d", __FUNCTION__, res);
	mm_request_send("preauth", socket, MONITOR_ANS_SKEYQUERY, m);

	return (0);
}

int
mm_answer_skeyrespond(int socket, Buffer *m)
{
	char *response;
	int authok;

	response = buffer_get_string(m, NULL);

	authok = (authctxt->valid &&
	    skey_haskey(authctxt->pw->pw_name) == 0 &&
	    skey_passcheck(authctxt->pw->pw_name, response) != -1);

	xfree(response);

	buffer_clear(m);
	buffer_put_int(m, authok);

	debug3("%s: sending authenticated: %d", __FUNCTION__, authok);
	mm_request_send("preauth", socket, MONITOR_ANS_SKEYRESPOND, m);

	auth_method = "skey";

	return (authok != 0);
}
#endif

#ifdef USE_PAM
int
mm_answer_pam_start(int socket, Buffer *m)
{
	char *user;
	
	user = buffer_get_string(m, NULL);

	start_pam(user);

	xfree(user);

	return (0);
}
#endif

static void
mm_append_debug(Buffer *m)
{
	if (auth_debug_init && buffer_len(&auth_debug)) {
		debug3("%s: Appending debug messages for child", __FUNCTION__);
		buffer_append(m, buffer_ptr(&auth_debug),
		    buffer_len(&auth_debug));
		buffer_clear(&auth_debug);
	}
}

int
mm_answer_keyallowed(int socket, Buffer *m)
{
	Key *key;
	u_char *cuser, *chost, *blob;
	u_int bloblen;
	enum mm_keytype type = 0;
	int allowed = 0;

	debug3("%s entering", __FUNCTION__);

	type = buffer_get_int(m);
	cuser = buffer_get_string(m, NULL);
	chost = buffer_get_string(m, NULL);
	blob = buffer_get_string(m, &bloblen);

	key = key_from_blob(blob, bloblen);

	if ((compat20 && type == MM_RSAHOSTKEY) ||
	    (!compat20 && type != MM_RSAHOSTKEY))
		fatal("%s: key type and protocol mismatch", __FUNCTION__);

	debug3("%s: key_from_blob: %p", __FUNCTION__, key);

	if (key != NULL && authctxt->pw != NULL) {
		switch(type) {
		case MM_USERKEY:
			allowed = user_key_allowed(authctxt->pw, key);
			break;
		case MM_HOSTKEY:
			allowed = hostbased_key_allowed(authctxt->pw,
			    cuser, chost, key);
			break;
		case MM_RSAHOSTKEY:
			key->type = KEY_RSA1; /* XXX */
			allowed = auth_rhosts_rsa_key_allowed(authctxt->pw,
			    cuser, chost, key);
			break;
		default:
			fatal("%s: unknown key type %d", __FUNCTION__, type);
			break;
		}
		key_free(key);
	}

	/* clear temporarily storage (used by verify) */
	monitor_reset_key_state();

	if (allowed) {
		/* Save temporarily for comparison in verify */
		key_blob = blob;
		key_bloblen = bloblen;
		key_blobtype = type;
		hostbased_cuser = cuser;
		hostbased_chost = chost;
	}

	debug3("%s: key %p is %s",
	    __FUNCTION__, key, allowed ? "allowed" : "disallowed");

	buffer_clear(m);
	buffer_put_int(m, allowed);

	mm_append_debug(m);

	mm_request_send("preauth", socket, MONITOR_ANS_KEYALLOWED, m);

	if (type == MM_RSAHOSTKEY)
		monitor_permit(mon_dispatch, MONITOR_REQ_RSACHALLENGE, allowed);

	return (0);
}

static int
monitor_valid_userblob(u_char *data, u_int datalen)
{
	Buffer b;
	u_char *p;
	u_int len;
	int fail = 0;
	int session_id2_len = 20 /*XXX should get from [net] */;

	buffer_init(&b);
	buffer_append(&b, data, datalen);

	if (datafellows & SSH_OLD_SESSIONID) {
		buffer_consume(&b, session_id2_len);
	} else {
		xfree(buffer_get_string(&b, &len));
		if (len != session_id2_len)
			fail++;
	}
	if (buffer_get_char(&b) != SSH2_MSG_USERAUTH_REQUEST)
		fail++;
	p = buffer_get_string(&b, NULL);
	if (strcmp(authctxt->user, p) != 0) {
		log("wrong user name passed to monitor: expected %s != %.100s",
		    authctxt->user, p);
		fail++;
	}
	xfree(p);
	buffer_skip_string(&b);
	if (datafellows & SSH_BUG_PKAUTH) {
		if (!buffer_get_char(&b))
			fail++;
	} else {
		p = buffer_get_string(&b, NULL);
		if (strcmp("publickey", p) != 0)
			fail++;
		xfree(p);
		if (!buffer_get_char(&b))
			fail++;
		buffer_skip_string(&b);
	}
	buffer_skip_string(&b);
	if (buffer_len(&b) != 0)
		fail++;
	buffer_free(&b);
	return (fail == 0);
}

static int
monitor_valid_hostbasedblob(u_char *data, u_int datalen, u_char *cuser,
    u_char *chost)
{
	Buffer b;
	u_char *p;
	u_int len;
	int fail = 0;
	int session_id2_len = 20 /*XXX should get from [net] */;

	buffer_init(&b);
	buffer_append(&b, data, datalen);

	xfree(buffer_get_string(&b, &len));
	if (len != session_id2_len)
		fail++;
	if (buffer_get_char(&b) != SSH2_MSG_USERAUTH_REQUEST)
		fail++;
	p = buffer_get_string(&b, NULL);
	if (strcmp(authctxt->user, p) != 0) {
		log("wrong user name passed to monitor: expected %s != %.100s",
		    authctxt->user, p);
		fail++;
	}
	xfree(p);
	buffer_skip_string(&b);	/* service */
	p = buffer_get_string(&b, NULL);
	if (strcmp(p, "hostbased") != 0)
		fail++;
	xfree(p);
	buffer_skip_string(&b);	/* pkalg */
	buffer_skip_string(&b);	/* pkblob */

	/* verify client host, strip trailing dot if necessary */
	p = buffer_get_string(&b, NULL);
	if (((len = strlen(p)) > 0) && p[len - 1] == '.')
		p[len - 1] = '\0';
	if (strcmp(p, chost) != 0)
		fail++;
	xfree(p);

	/* verify client user */
	p = buffer_get_string(&b, NULL);
	if (strcmp(p, cuser) != 0)
		fail++;
	xfree(p);

	if (buffer_len(&b) != 0)
		fail++;
	buffer_free(&b);
	return (fail == 0);
}

int
mm_answer_keyverify(int socket, Buffer *m)
{
	Key *key;
	u_char *signature, *data, *blob;
	u_int signaturelen, datalen, bloblen;
	int verified = 0;
	int valid_data = 0;

	blob = buffer_get_string(m, &bloblen);
	signature = buffer_get_string(m, &signaturelen);
	data = buffer_get_string(m, &datalen);

	if (hostbased_cuser == NULL || hostbased_chost == NULL ||
	  !monitor_allowed_key(blob, bloblen))
		fatal("%s: bad key, not previously allowed", __FUNCTION__);

	key = key_from_blob(blob, bloblen);
	if (key == NULL)
		fatal("%s: bad public key blob", __FUNCTION__);

	switch (key_blobtype) {
	case MM_USERKEY:
		valid_data = monitor_valid_userblob(data, datalen);
		break;
	case MM_HOSTKEY:
		valid_data = monitor_valid_hostbasedblob(data, datalen,
		    hostbased_cuser, hostbased_chost);
		break;
	default:
		valid_data = 0;
		break;
	}
	if (!valid_data)
		fatal("%s: bad signature data blob", __FUNCTION__);

	verified = key_verify(key, signature, signaturelen, data, datalen);
	debug3("%s: key %p signature %s",
	    __FUNCTION__, key, verified ? "verified" : "unverified");

	key_free(key);
	xfree(blob);
	xfree(signature);
	xfree(data);

	monitor_reset_key_state();

	buffer_clear(m);
	buffer_put_int(m, verified);
	mm_request_send("preauth", socket, MONITOR_ANS_KEYVERIFY, m);

	auth_method = "publickey";

	return (verified);
}

static void
mm_record_login(Session *s, struct passwd *pw)
{
	socklen_t fromlen;
	struct sockaddr_storage from;

	/*
	 * Get IP address of client. If the connection is not a socket, let
	 * the address be 0.0.0.0.
	 */
	memset(&from, 0, sizeof(from));
	if (packet_connection_is_on_socket()) {
		fromlen = sizeof(from);
		if (getpeername(packet_get_connection_in(),
			(struct sockaddr *) & from, &fromlen) < 0) {
			debug("getpeername: %.100s", strerror(errno));
			fatal_cleanup();
		}
	}
	/* Record that there was a login on that tty from the remote host. */
	record_login(s->pid, s->tty, pw->pw_name, pw->pw_uid,
	    get_remote_name_or_ip(utmp_len, options.verify_reverse_mapping),
	    (struct sockaddr *)&from);
}

static void
mm_session_close(Session *s)
{
	debug3("%s: session %d pid %d", __FUNCTION__, s->self, s->pid);
	if (s->ttyfd != -1) {
		debug3("%s: tty %s ptyfd %d",  __FUNCTION__, s->tty, s->ptyfd);
		fatal_remove_cleanup(session_pty_cleanup2, (void *)s);
		session_pty_cleanup2(s);
	}
	s->used = 0;
}

int
mm_answer_pty(int socket, Buffer *m)
{
	extern struct monitor *pmonitor;
	Session *s;
	int res, fd0;

	debug3("%s entering", __FUNCTION__);

	buffer_clear(m);
	s = session_new();
	if (s == NULL)
		goto error;
	s->authctxt = authctxt;
	s->pw = authctxt->pw;
	s->pid = pmonitor->m_pid;
	res = pty_allocate(&s->ptyfd, &s->ttyfd, s->tty, sizeof(s->tty));
	if (res == 0)
		goto error;
	fatal_add_cleanup(session_pty_cleanup2, (void *)s);
	pty_setowner(authctxt->pw, s->tty);

	buffer_put_int(m, 1);
	buffer_put_cstring(m, s->tty);
	mm_request_send("postauth", socket, MONITOR_ANS_PTY, m);

#warning FD is being passed, can SOAAP model this?
	mm_send_fd(socket, s->ptyfd);
	mm_send_fd(socket, s->ttyfd);

	/* We need to trick ttyslot */
	if (dup2(s->ttyfd, 0) == -1)
		fatal("%s: dup2", __FUNCTION__);

	mm_record_login(s, authctxt->pw);

	/* Now we can close the file descriptor again */
	close(0);

	/* make sure nothing uses fd 0 */
	if ((fd0 = open(_PATH_DEVNULL, O_RDONLY)) < 0)
		fatal("%s: open(/dev/null): %s", __FUNCTION__, strerror(errno));
	if (fd0 != 0)
		error("%s: fd0 %d != 0", __FUNCTION__, fd0);

	/* slave is not needed */
	close(s->ttyfd);
	s->ttyfd = s->ptyfd;
	/* no need to dup() because nobody closes ptyfd */
	s->ptymaster = s->ptyfd;

	debug3("%s: tty %s ptyfd %d",  __FUNCTION__, s->tty, s->ttyfd);

	return (0);

 error:
	if (s != NULL)
		mm_session_close(s);
	buffer_put_int(m, 0);
	mm_request_send("postauth", socket, MONITOR_ANS_PTY, m);
	return (0);
}

int
mm_answer_pty_cleanup(int socket, Buffer *m)
{
	Session *s;
	char *tty;

	debug3("%s entering", __FUNCTION__);

	tty = buffer_get_string(m, NULL);
	if ((s = session_by_tty(tty)) != NULL)
		mm_session_close(s);
	buffer_clear(m);
	xfree(tty);
	return (0);
}

int
mm_answer_sesskey(int socket, Buffer *m)
{
	BIGNUM *p;
	int rsafail;

	/* Turn off permissions */
	monitor_permit(mon_dispatch, MONITOR_REQ_SESSKEY, 1);

	if ((p = BN_new()) == NULL)
		fatal("%s: BN_new", __FUNCTION__);

	buffer_get_bignum2(m, p);

	rsafail = ssh1_session_key(p);

	buffer_clear(m);
	buffer_put_int(m, rsafail);
	buffer_put_bignum2(m, p);

	BN_clear_free(p);

	mm_request_send("preauth", socket, MONITOR_ANS_SESSKEY, m);

	/* Turn on permissions for sessid passing */
	monitor_permit(mon_dispatch, MONITOR_REQ_SESSID, 1);

	return (0);
}

int
mm_answer_sessid(int socket, Buffer *m)
{
	int i;

	debug3("%s entering", __FUNCTION__);

	if (buffer_len(m) != 16)
		fatal("%s: bad ssh1 session id", __FUNCTION__);
	for (i = 0; i < 16; i++)
		session_id[i] = buffer_get_char(m);

	/* Turn on permissions for getpwnam */
	monitor_permit(mon_dispatch, MONITOR_REQ_PWNAM, 1);

	return (0);
}

int
mm_answer_rsa_keyallowed(int socket, Buffer *m)
{
	BIGNUM *client_n;
	Key *key = NULL;
	u_char *blob = NULL;
	u_int blen = 0;
	int allowed = 0;

	debug3("%s entering", __FUNCTION__);

	if (authctxt->valid) {
		if ((client_n = BN_new()) == NULL)
			fatal("%s: BN_new", __FUNCTION__);
		buffer_get_bignum2(m, client_n);
		allowed = auth_rsa_key_allowed(authctxt->pw, client_n, &key);
		BN_clear_free(client_n);
	}
	buffer_clear(m);
	buffer_put_int(m, allowed);

	/* clear temporarily storage (used by generate challenge) */
	monitor_reset_key_state();

	if (allowed && key != NULL) {
		key->type = KEY_RSA;	/* cheat for key_to_blob */
		if (key_to_blob(key, &blob, &blen) == 0)
			fatal("%s: key_to_blob failed", __FUNCTION__);
		buffer_put_string(m, blob, blen);

		/* Save temporarily for comparison in verify */
		key_blob = blob;
		key_bloblen = blen;
		key_blobtype = MM_RSAUSERKEY;
		key_free(key);
	}

	mm_append_debug(m);

	mm_request_send("preauth", socket, MONITOR_ANS_RSAKEYALLOWED, m);

	monitor_permit(mon_dispatch, MONITOR_REQ_RSACHALLENGE, allowed);
	monitor_permit(mon_dispatch, MONITOR_REQ_RSARESPONSE, 0);
	return (0);
}

int
mm_answer_rsa_challenge(int socket, Buffer *m)
{
	Key *key = NULL;
	u_char *blob;
	u_int blen;

	debug3("%s entering", __FUNCTION__);

	if (!authctxt->valid)
		fatal("%s: authctxt not valid", __FUNCTION__);
	blob = buffer_get_string(m, &blen);
	if (!monitor_allowed_key(blob, blen))
		fatal("%s: bad key, not previously allowed", __FUNCTION__);
	if (key_blobtype != MM_RSAUSERKEY && key_blobtype != MM_RSAHOSTKEY)
		fatal("%s: key type mismatch", __FUNCTION__);
	if ((key = key_from_blob(blob, blen)) == NULL)
		fatal("%s: received bad key", __FUNCTION__);

	if (ssh1_challenge)
		BN_clear_free(ssh1_challenge);
	ssh1_challenge = auth_rsa_generate_challenge(key);

	buffer_clear(m);
	buffer_put_bignum2(m, ssh1_challenge);

	debug3("%s sending reply", __FUNCTION__);
	mm_request_send("preauth", socket, MONITOR_ANS_RSACHALLENGE, m);

	monitor_permit(mon_dispatch, MONITOR_REQ_RSARESPONSE, 1);
	return (0);
}

int
mm_answer_rsa_response(int socket, Buffer *m)
{
	Key *key = NULL;
	u_char *blob, *response;
	u_int blen, len;
	int success;

	debug3("%s entering", __FUNCTION__);

	if (!authctxt->valid)
		fatal("%s: authctxt not valid", __FUNCTION__);
	if (ssh1_challenge == NULL)
		fatal("%s: no ssh1_challenge", __FUNCTION__);

	blob = buffer_get_string(m, &blen);
	if (!monitor_allowed_key(blob, blen))
		fatal("%s: bad key, not previously allowed", __FUNCTION__);
	if (key_blobtype != MM_RSAUSERKEY && key_blobtype != MM_RSAHOSTKEY)
		fatal("%s: key type mismatch: %d", __FUNCTION__, key_blobtype);
	if ((key = key_from_blob(blob, blen)) == NULL)
		fatal("%s: received bad key", __FUNCTION__);
	response = buffer_get_string(m, &len);
	if (len != 16)
		fatal("%s: received bad response to challenge", __FUNCTION__);
	success = auth_rsa_verify_response(key, ssh1_challenge, response);

	key_free(key);
	xfree(response);

	auth_method = key_blobtype == MM_RSAUSERKEY ? "rsa" : "rhosts-rsa";

	/* reset state */
	BN_clear_free(ssh1_challenge);
	ssh1_challenge = NULL;
	monitor_reset_key_state();

	buffer_clear(m);
	buffer_put_int(m, success);
	mm_request_send("preauth", socket, MONITOR_ANS_RSARESPONSE, m);

	return (success);
}

int
mm_answer_term(int socket, Buffer *req)
{
	extern struct monitor *pmonitor;
	int res, status;

	debug3("%s: tearing down sessions", __FUNCTION__);

	/* The child is terminating */
	session_destroy_all(&mm_session_close);

	while (waitpid(pmonitor->m_pid, &status, 0) == -1)
		if (errno != EINTR)
			exit(1);

	res = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

	/* Terminate process */
	exit (res);
}

void
monitor_apply_keystate(struct monitor *pmonitor)
{
	if (compat20) {
		set_newkeys(MODE_IN);
		set_newkeys(MODE_OUT);
	} else {
		u_char key[SSH_SESSION_KEY_LENGTH];

		memset(key, 'a', sizeof(key));
		packet_set_protocol_flags(child_state.ssh1protoflags);
		packet_set_encryption_key(key, SSH_SESSION_KEY_LENGTH,
		    child_state.ssh1cipher);
	}

	packet_set_keycontext(MODE_OUT, child_state.keyout);
	xfree(child_state.keyout);
	packet_set_keycontext(MODE_IN, child_state.keyin);
	xfree(child_state.keyin);

	if (!compat20) {
		packet_set_iv(MODE_OUT, child_state.ivout);
		xfree(child_state.ivout);
		packet_set_iv(MODE_IN, child_state.ivin);
		xfree(child_state.ivin);
	}

	memcpy(&incoming_stream, &child_state.incoming,
	    sizeof(incoming_stream));
	memcpy(&outgoing_stream, &child_state.outgoing,
	    sizeof(outgoing_stream));

	/* Update with new address */
	mm_init_compression(pmonitor->m_zlib);

	/* Network I/O buffers */
	/* XXX inefficient for large buffers, need: buffer_init_from_string */
	buffer_clear(&input);
	buffer_append(&input, child_state.input, child_state.ilen);
	memset(child_state.input, 0, child_state.ilen);
	xfree(child_state.input);

	buffer_clear(&output);
	buffer_append(&output, child_state.output, child_state.olen);
	memset(child_state.output, 0, child_state.olen);
	xfree(child_state.output);
}

static Kex *
mm_get_kex(Buffer *m)
{
	Kex *kex;
	void *blob;
	u_int bloblen;

	kex = xmalloc(sizeof(*kex));
	memset(kex, 0, sizeof(*kex));
	kex->session_id = buffer_get_string(m, &kex->session_id_len);
	kex->we_need = buffer_get_int(m);
	kex->server = 1;
	kex->hostkey_type = buffer_get_int(m);
	kex->kex_type = buffer_get_int(m);
	blob = buffer_get_string(m, &bloblen);
	buffer_init(&kex->my);
	buffer_append(&kex->my, blob, bloblen);
	xfree(blob);
	blob = buffer_get_string(m, &bloblen);
	buffer_init(&kex->peer);
	buffer_append(&kex->peer, blob, bloblen);
	xfree(blob);
	kex->done = 1;
	kex->flags = buffer_get_int(m);
	kex->client_version_string = buffer_get_string(m, NULL);
	kex->server_version_string = buffer_get_string(m, NULL);
	kex->load_host_key=&get_hostkey_by_type;
	kex->host_key_index=&get_hostkey_index;

	return (kex);
}

/* This function requries careful sanity checking */

void
mm_get_keystate(struct monitor *pmonitor)
{
	Buffer m;
	u_char *blob, *p;
	u_int bloblen, plen;

	debug3("%s: Waiting for new keys", __FUNCTION__);

	buffer_init(&m);
	mm_request_receive_expect("preauth", pmonitor->m_sendfd, MONITOR_REQ_KEYEXPORT, &m);
	if (!compat20) {
		child_state.ssh1protoflags = buffer_get_int(&m);
		child_state.ssh1cipher = buffer_get_int(&m);
		child_state.ivout = buffer_get_string(&m,
		    &child_state.ivoutlen);
		child_state.ivin = buffer_get_string(&m, &child_state.ivinlen);
		goto skip;
	} else {
		/* Get the Kex for rekeying */
		*pmonitor->m_pkex = mm_get_kex(&m);
	}

	blob = buffer_get_string(&m, &bloblen);
	current_keys[MODE_OUT] = mm_newkeys_from_blob(blob, bloblen);
	xfree(blob);

	debug3("%s: Waiting for second key", __FUNCTION__);
	blob = buffer_get_string(&m, &bloblen);
	current_keys[MODE_IN] = mm_newkeys_from_blob(blob, bloblen);
	xfree(blob);

	/* Now get sequence numbers for the packets */
	packet_set_seqnr(MODE_OUT, buffer_get_int(&m));
	packet_set_seqnr(MODE_IN, buffer_get_int(&m));

 skip:
	/* Get the key context */
	child_state.keyout = buffer_get_string(&m, &child_state.keyoutlen);
	child_state.keyin  = buffer_get_string(&m, &child_state.keyinlen);

	debug3("%s: Getting compression state", __FUNCTION__);
	/* Get compression state */
	p = buffer_get_string(&m, &plen);
	if (plen != sizeof(child_state.outgoing))
		fatal("%s: bad request size", __FUNCTION__);
	memcpy(&child_state.outgoing, p, sizeof(child_state.outgoing));
	xfree(p);

	p = buffer_get_string(&m, &plen);
	if (plen != sizeof(child_state.incoming))
		fatal("%s: bad request size", __FUNCTION__);
	memcpy(&child_state.incoming, p, sizeof(child_state.incoming));
	xfree(p);

	/* Network I/O buffers */
	debug3("%s: Getting Network I/O buffers", __FUNCTION__);
	child_state.input = buffer_get_string(&m, &child_state.ilen);
	child_state.output = buffer_get_string(&m, &child_state.olen);

	buffer_free(&m);
}


/* Allocation functions for zlib */
void *
mm_zalloc(struct mm_master *mm, u_int ncount, u_int size)
{
	void *address;

	address = mm_malloc(mm, size * ncount);

	return (address);
}

void
mm_zfree(struct mm_master *mm, void *address)
{
	mm_free(mm, address);
}

void
mm_init_compression(struct mm_master *mm)
{
	outgoing_stream.zalloc = (alloc_func)mm_zalloc;
	outgoing_stream.zfree = (free_func)mm_zfree;
	outgoing_stream.opaque = mm;

	incoming_stream.zalloc = (alloc_func)mm_zalloc;
	incoming_stream.zfree = (free_func)mm_zfree;
	incoming_stream.opaque = mm;
}

/* XXX */

#define FD_CLOSEONEXEC(x) do { \
	if (fcntl(x, F_SETFD, 1) == -1) \
		fatal("fcntl(%d, F_SETFD)", x); \
} while (0)

static void
monitor_socketpair(int *pair)
{
#ifdef HAVE_SOCKETPAIR
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1)
		fatal("%s: socketpair", __FUNCTION__);
#else
	fatal("%s: UsePrivilegeSeparation=yes not supported",
	    __FUNCTION__);
#endif
	FD_CLOSEONEXEC(pair[0]);
	FD_CLOSEONEXEC(pair[1]);
}

#define MM_MEMSIZE	65536

struct monitor *
monitor_init(void)
{
	struct monitor *mon;
	int pair[2];

	mon = xmalloc(sizeof(*mon));

	monitor_socketpair(pair);

	mon->m_recvfd = pair[0];
	mon->m_sendfd = pair[1];

	/* Used to share zlib space across processes */
	mon->m_zback = mm_create(NULL, MM_MEMSIZE);
	mon->m_zlib = mm_create(mon->m_zback, 20 * MM_MEMSIZE);

	/* Compression needs to share state across borders */
	mm_init_compression(mon->m_zlib);

	return mon;
}

void
monitor_reinit(struct monitor *mon)
{
	int pair[2];

	monitor_socketpair(pair);

	mon->m_recvfd = pair[0];
	mon->m_sendfd = pair[1];
}

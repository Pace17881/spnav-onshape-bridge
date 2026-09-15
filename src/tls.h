#ifndef TLS_H_
#define TLS_H_

#include <openssl/ssl.h>

/* Ensures a per-user, self-signed certificate for `bind_ip` exists under
 * state_dir (created 0700 if missing), generating a fresh one (never
 * shipped/shared - unlike spacenav-ws's single hardcoded key committed to
 * its public git repo) if not already present. Returns 0 on success.
 *
 * Deliberately a single self-signed leaf (issuer == subject), not a locally
 * generated CA that issues a leaf: browsers trust the *CA* form as a much
 * more powerful grant (that CA could mint certs for any hostname), and in
 * testing that measurably made both Chromium and Firefox kill the
 * WebSocket connection after ~1s (close code 1006) even with every
 * Local/Private-Network-Access flag disabled - switching to a single
 * directly-trusted leaf, matching how "add a security exception" normally
 * works, fixed it. See the comment on make_self_signed_cert() in tls.c.
 *
 * cert_path_out, if non-NULL, is filled with the path to the certificate
 * (PEM, public - safe to import into browser trust stores), for the caller
 * to print/hand to the NSS import helper script.
 */
int tls_ensure_certs(const char *state_dir, const char *bind_ip,
		char *cert_path_out, size_t cert_path_sz);

/* Builds a server-mode SSL_CTX using the cert/key produced by
 * tls_ensure_certs(). Returns NULL on failure. */
SSL_CTX *tls_make_server_ctx(const char *state_dir);

#endif

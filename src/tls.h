/*
spnav-onshape-bridge - bridges spacenavd 3D-mouse events to Onshape's
browser 3Dconnexion API on Linux.
Copyright (C) 2026 Sebastian Polster <polsterseb@protonmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef TLS_H_
#define TLS_H_

#include <openssl/ssl.h>

/* Ensures a per-user CA + leaf certificate for `bind_ip` exist under
 * state_dir (created 0700 if missing), generating fresh ones (never
 * shipped/shared - unlike spacenav-ws's single hardcoded key committed to
 * its public git repo) if not already present. Returns 0 on success.
 *
 * A local CA that issues the leaf, not a single self-signed leaf trusted
 * directly: verified empirically (headless Firefox against both variants)
 * that Firefox's certificate verifier does not reliably accept a
 * peer-trusted self-signed leaf without a manual per-site exception, while
 * a CA-trusted leaf validates with no user interaction in both Firefox and
 * Chromium. See the comment on tls_ensure_certs() in tls.c for the full
 * story, including an earlier, incorrect attempt to fix an unrelated bug by
 * switching away from this design.
 *
 * ca_cert_path_out, if non-NULL, is filled with the path to the CA
 * certificate (PEM, public - safe to import into browser trust stores), for
 * the caller to print/hand to the NSS import helper script.
 */
int tls_ensure_certs(const char *state_dir, const char *bind_ip,
		char *ca_cert_path_out, size_t ca_cert_path_sz);

/* Builds a server-mode SSL_CTX using the leaf cert/key produced by
 * tls_ensure_certs(). Returns NULL on failure. */
SSL_CTX *tls_make_server_ctx(const char *state_dir);

#endif

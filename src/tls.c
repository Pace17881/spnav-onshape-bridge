#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include "tls.h"

#define CERT_VALID_DAYS 3650

static int ensure_dir(const char *path, mode_t mode)
{
	if(mkdir(path, mode) == 0 || errno == EEXIST) {
		return 0;
	}
	return -1;
}

static int write_pem_key(const char *path, EVP_PKEY *key)
{
	FILE *fp = fopen(path, "wb");
	int ok;
	if(!fp) return -1;
	ok = PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL);
	fclose(fp);
	if(ok && chmod(path, S_IRUSR | S_IWUSR) != 0) {
		return -1;
	}
	return ok ? 0 : -1;
}

static int write_pem_cert(const char *path, X509 *cert)
{
	FILE *fp = fopen(path, "wb");
	int ok;
	if(!fp) return -1;
	ok = PEM_write_X509(fp, cert);
	fclose(fp);
	return ok ? 0 : -1;
}

static EVP_PKEY *gen_ec_key(void)
{
	return EVP_EC_gen("P-256");
}

static void add_ext(X509 *cert, int nid, const char *value)
{
	X509V3_CTX ctx;
	X509_EXTENSION *ext;

	/* self-signed leaf: issuer and subject are both `cert` itself */
	X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
	if(ext) {
		X509_add_ext(cert, ext, -1);
		X509_EXTENSION_free(ext);
	}
}

static void set_random_serial(X509 *cert)
{
	unsigned char buf[8];
	int64_t serial;

	RAND_bytes(buf, sizeof buf);
	memcpy(&serial, buf, sizeof serial);
	serial &= 0x7fffffffffffffffLL; /* keep positive for DER INTEGER encoding */
	if(serial == 0) serial = 1;
	ASN1_INTEGER_set_int64(X509_get_serialNumber(cert), serial);
}

/* A single, directly self-signed leaf certificate (issuer == subject == the
 * bind IP) - the same trust model spacenav-ws's shipped certificate uses
 * (see certs/ip.crt there), and the one "add a security exception"/"import
 * as a trusted certificate" flows in browsers are built around: the browser
 * trusts this *one* certificate directly (NSS "peer trust", not CA trust).
 *
 * Direct peer trust scopes the trust grant to this certificate instead of
 * installing a CA that could issue certificates for other hostnames.
 * The earlier claim that this change fixed browser disconnects was incorrect:
 * see POSTMORTEM.md. Trust model changes did not resolve that failure.
 */
static X509 *make_self_signed_cert(EVP_PKEY *key, const char *cn, const char *san_ip)
{
	X509 *cert = X509_new();
	X509_NAME *name;
	char san[128];

	X509_set_version(cert, 2); /* v3 */
	set_random_serial(cert);

	X509_gmtime_adj(X509_getm_notBefore(cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(cert), (long)60 * 60 * 24 * CERT_VALID_DAYS);

	X509_set_pubkey(cert, key);

	name = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)cn, -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
			(const unsigned char*)"spnav-onshape-bridge (local, per-install)", -1, -1, 0);
	X509_set_issuer_name(cert, name); /* self-signed: issuer == subject */

	snprintf(san, sizeof san, "IP:%s", san_ip);
	add_ext(cert, NID_basic_constraints, "critical,CA:FALSE");
	add_ext(cert, NID_key_usage, "critical,digitalSignature,keyEncipherment");
	add_ext(cert, NID_ext_key_usage, "serverAuth");
	add_ext(cert, NID_subject_alt_name, san);
	add_ext(cert, NID_subject_key_identifier, "hash");

	X509_sign(cert, key, EVP_sha256());
	return cert;
}

static void path_join(char *out, size_t outsz, const char *dir, const char *file)
{
	snprintf(out, outsz, "%s/%s", dir, file);
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

int tls_ensure_certs(const char *state_dir, const char *bind_ip,
		char *cert_path_out, size_t cert_path_sz)
{
	char key_path[512], crt_path[512];

	if(ensure_dir(state_dir, S_IRWXU) != 0) {
		fprintf(stderr, "failed to create state dir %s: %s\n", state_dir, strerror(errno));
		return -1;
	}

	path_join(key_path, sizeof key_path, state_dir, "server.key.pem");
	path_join(crt_path, sizeof crt_path, state_dir, "server.crt.pem");

	if(cert_path_out) {
		snprintf(cert_path_out, cert_path_sz, "%s", crt_path);
	}

	if(file_exists(key_path) && file_exists(crt_path)) {
		return 0; /* already provisioned for this user */
	}

	fprintf(stderr, "no local certificate found in %s, generating a fresh, "
			"unique-to-this-install self-signed certificate...\n", state_dir);

	EVP_PKEY *key = gen_ec_key();
	if(!key) {
		fprintf(stderr, "key generation failed: %s\n", ERR_error_string(ERR_get_error(), NULL));
		return -1;
	}

	X509 *cert = make_self_signed_cert(key, bind_ip, bind_ip);

	if(write_pem_key(key_path, key) || write_pem_cert(crt_path, cert)) {
		fprintf(stderr, "failed writing certificate/key files under %s\n", state_dir);
		return -1;
	}

	EVP_PKEY_free(key);
	X509_free(cert);

	fprintf(stderr, "generated certificate: %s\n"
			"import it into your browsers' trust stores with "
			"contrib/nss-trust-install.sh (one-time step)\n", crt_path);
	return 0;
}

SSL_CTX *tls_make_server_ctx(const char *state_dir)
{
	char key_path[512], crt_path[512];
	SSL_CTX *ctx;

	path_join(key_path, sizeof key_path, state_dir, "server.key.pem");
	path_join(crt_path, sizeof crt_path, state_dir, "server.crt.pem");

	ctx = SSL_CTX_new(TLS_server_method());
	if(!ctx) {
		return NULL;
	}
	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

	if(SSL_CTX_use_certificate_file(ctx, crt_path, SSL_FILETYPE_PEM) <= 0 ||
			SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
		fprintf(stderr, "failed to load certificate/key from %s: %s\n",
				state_dir, ERR_error_string(ERR_get_error(), NULL));
		SSL_CTX_free(ctx);
		return NULL;
	}

	return ctx;
}

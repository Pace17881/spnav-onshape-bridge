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

#define CA_VALID_DAYS   3650
#define LEAF_VALID_DAYS 730

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

static void add_ext(X509 *cert, X509 *issuer, X509 *subject, int nid, const char *value)
{
	X509V3_CTX ctx;
	X509_EXTENSION *ext;

	X509V3_set_ctx(&ctx, issuer, subject, NULL, NULL, 0);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
	if(ext) {
		X509_add_ext(cert, ext, -1);
		X509_EXTENSION_free(ext);
	}
}

/* A random serial, not time(NULL): the CA and the leaf it issues share the
 * same issuer DN (the leaf's issuer is the CA's subject) and are generated
 * a fraction of a second apart, so a time()-based serial reliably collides
 * between them - NSS then refuses the chain with
 * SEC_ERROR_REUSED_ISSUER_AND_SERIAL, since (issuer, serial) is supposed to
 * uniquely identify one certificate (this actually happened during
 * development; see POSTMORTEM.md). */
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

static X509 *make_cert(EVP_PKEY *subj_key, EVP_PKEY *issuer_key, X509 *issuer_cert,
		const char *cn, int is_ca, int valid_days, const char *san_ip)
{
	X509 *cert = X509_new();
	X509_NAME *name;

	X509_set_version(cert, 2); /* v3 */
	set_random_serial(cert);

	X509_gmtime_adj(X509_getm_notBefore(cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(cert), (long)60 * 60 * 24 * valid_days);

	X509_set_pubkey(cert, subj_key);

	name = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)cn, -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
			(const unsigned char*)"spnav-onshape-bridge (local, per-install)", -1, -1, 0);

	X509_set_issuer_name(cert, issuer_cert ? X509_get_subject_name(issuer_cert) : name);

	if(is_ca) {
		add_ext(cert, cert, cert, NID_basic_constraints, "critical,CA:TRUE");
		add_ext(cert, cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign");
		add_ext(cert, cert, cert, NID_subject_key_identifier, "hash");
	} else {
		char san[128];
		X509 *iss = issuer_cert ? issuer_cert : cert;
		snprintf(san, sizeof san, "IP:%s", san_ip);
		add_ext(cert, iss, cert, NID_basic_constraints, "critical,CA:FALSE");
		add_ext(cert, iss, cert, NID_key_usage, "critical,digitalSignature,keyEncipherment");
		add_ext(cert, iss, cert, NID_ext_key_usage, "serverAuth");
		add_ext(cert, iss, cert, NID_subject_alt_name, san);
		add_ext(cert, iss, cert, NID_subject_key_identifier, "hash");
		add_ext(cert, iss, cert, NID_authority_key_identifier, "keyid:always");
	}

	X509_sign(cert, issuer_key, EVP_sha256());
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

/* A local CA that issues a leaf certificate for `bind_ip`, imported into
 * browsers with real CA trust (contrib/nss-trust-install.sh uses
 * `certutil -t C,,`), not a bare self-signed leaf trusted "directly" (NSS
 * peer trust, `-t P,,`).
 *
 * An earlier version of this code used a directly-trusted self-signed leaf,
 * on the (wrong) theory that CA trust had caused an unrelated Chromium
 * connection-drop bug - it hadn't (see POSTMORTEM.md; that bug was an
 * unrelated message-size limit). Peer-trusting a self-signed leaf then
 * turned out to be the thing actually causing trouble, just in Firefox
 * rather than Chromium: Firefox's certificate verifier (mozilla::pkix)
 * does not reliably accept a peer-trusted self-signed leaf the way NSS's
 * classic model implies it should, requiring a manual per-site security
 * exception instead of a one-time, scriptable trust-store import. Verified
 * empirically with a headless Firefox profile: a peer-trusted self-signed
 * leaf shows the certificate warning interstitial, a CA-trusted leaf issued
 * by a locally generated CA loads directly with no interstitial - in both
 * Firefox and Chromium.
 *
 * The trade-off this reintroduces: a trusted CA can mint valid certificates
 * for *any* hostname, not just this one IP, which is why it gets a fresh,
 * unique-per-install key/certificate (never shipped or shared - unlike
 * spacenav-ws's single hardcoded key committed to its public git repo) and
 * is documented as a real CA in every place a user might import it.
 */
int tls_ensure_certs(const char *state_dir, const char *bind_ip,
		char *ca_cert_path_out, size_t ca_cert_path_sz)
{
	char ca_key[512], ca_crt[512], leaf_key[512], leaf_crt[512];

	if(ensure_dir(state_dir, S_IRWXU) != 0) {
		fprintf(stderr, "failed to create state dir %s: %s\n", state_dir, strerror(errno));
		return -1;
	}

	path_join(ca_key, sizeof ca_key, state_dir, "ca.key.pem");
	path_join(ca_crt, sizeof ca_crt, state_dir, "ca.crt.pem");
	path_join(leaf_key, sizeof leaf_key, state_dir, "leaf.key.pem");
	path_join(leaf_crt, sizeof leaf_crt, state_dir, "leaf.crt.pem");

	if(ca_cert_path_out) {
		snprintf(ca_cert_path_out, ca_cert_path_sz, "%s", ca_crt);
	}

	if(file_exists(ca_key) && file_exists(ca_crt) &&
			file_exists(leaf_key) && file_exists(leaf_crt)) {
		return 0; /* already provisioned for this user */
	}

	fprintf(stderr, "no local CA/certificate found in %s, generating a fresh, "
			"unique-to-this-install pair...\n", state_dir);

	EVP_PKEY *cakey = gen_ec_key();
	EVP_PKEY *leafkey = gen_ec_key();
	if(!cakey || !leafkey) {
		fprintf(stderr, "key generation failed: %s\n", ERR_error_string(ERR_get_error(), NULL));
		return -1;
	}

	X509 *cacert = make_cert(cakey, cakey, NULL,
			"spnav-onshape-bridge local CA", 1, CA_VALID_DAYS, NULL);
	X509 *leafcert = make_cert(leafkey, cakey, cacert,
			bind_ip, 0, LEAF_VALID_DAYS, bind_ip);

	if(write_pem_key(ca_key, cakey) || write_pem_cert(ca_crt, cacert) ||
			write_pem_key(leaf_key, leafkey) || write_pem_cert(leaf_crt, leafcert)) {
		fprintf(stderr, "failed writing certificate/key files under %s\n", state_dir);
		return -1;
	}

	EVP_PKEY_free(cakey);
	EVP_PKEY_free(leafkey);
	X509_free(cacert);
	X509_free(leafcert);

	fprintf(stderr, "generated CA certificate: %s\n"
			"import it into your browsers' trust stores with "
			"contrib/nss-trust-install.sh (one-time step)\n", ca_crt);
	return 0;
}

SSL_CTX *tls_make_server_ctx(const char *state_dir)
{
	char leaf_key[512], leaf_crt[512];
	SSL_CTX *ctx;

	path_join(leaf_key, sizeof leaf_key, state_dir, "leaf.key.pem");
	path_join(leaf_crt, sizeof leaf_crt, state_dir, "leaf.crt.pem");

	ctx = SSL_CTX_new(TLS_server_method());
	if(!ctx) {
		return NULL;
	}
	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

	if(SSL_CTX_use_certificate_file(ctx, leaf_crt, SSL_FILETYPE_PEM) <= 0 ||
			SSL_CTX_use_PrivateKey_file(ctx, leaf_key, SSL_FILETYPE_PEM) <= 0) {
		fprintf(stderr, "failed to load certificate/key from %s: %s\n",
				state_dir, ERR_error_string(ERR_get_error(), NULL));
		SSL_CTX_free(ctx);
		return NULL;
	}

	return ctx;
}

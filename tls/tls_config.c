/* $OpenBSD: tls_config.c,v 1.27 2016/08/13 13:15:53 jsing Exp $ */
/*
 * Copyright (c) 2014 Joel Sing <jsing@openbsd.org>
 * Copyright (c) 2016 Tai Chi Minh Ralph Eastwood <tcmreastwood@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <tls.h>
#include "tls_internal.h"

#ifdef  LIBRESSL_VERSION_NUMBER
/* This is not defined in libressl */
void explicit_bzero(void *buf, size_t len);
#endif


static int
set_string(const char **dest, const char *src)
{
	free((char *)*dest);
	*dest = NULL;
	if (src != NULL)
		if ((*dest = strdup(src)) == NULL)
			return -1;
	return 0;
}

static void *
memdup(const void *in, size_t len)
{
	void *out;

	if ((out = malloc(len)) == NULL)
		return NULL;
	memcpy(out, in, len);
	return out;
}

static int
set_mem(char **dest, size_t *destlen, const void *src, size_t srclen)
{
	free(*dest);
	*dest = NULL;
	*destlen = 0;
	if (src != NULL)
		if ((*dest = memdup(src, srclen)) == NULL)
			return -1;
	*destlen = srclen;
	return 0;
}

static struct tls_keypair *
tls_keypair_new()
{
	return calloc(1, sizeof(struct tls_keypair));
}

static int
tls_keypair_set_cert_file(struct tls_keypair *keypair, struct tls_error *error,
    const char *cert_file)
{
	return tls_config_load_file(error, "certificate", cert_file,
	    &keypair->cert_mem, &keypair->cert_len);
}

static int
tls_keypair_set_cert_mem(struct tls_keypair *keypair, const uint8_t *cert,
    size_t len)
{
	return set_mem(&keypair->cert_mem, &keypair->cert_len, cert, len);
}

static int
tls_keypair_set_key_file(struct tls_keypair *keypair, struct tls_error *error,
    const char *key_file)
{
	if (keypair->key_mem != NULL)
		explicit_bzero(keypair->key_mem, keypair->key_len);
	return tls_config_load_file(error, "key", key_file,
	    &keypair->key_mem, &keypair->key_len);
}

static int
tls_keypair_set_key_mem(struct tls_keypair *keypair, const uint8_t *key,
    size_t len)
{
	if (keypair->key_mem != NULL)
		explicit_bzero(keypair->key_mem, keypair->key_len);
	return set_mem(&keypair->key_mem, &keypair->key_len, key, len);
}

static void
tls_keypair_clear(struct tls_keypair *keypair)
{
	tls_keypair_set_cert_mem(keypair, NULL, 0);
	tls_keypair_set_key_mem(keypair, NULL, 0);
}

static void
tls_keypair_free(struct tls_keypair *keypair)
{
	if (keypair == NULL)
		return;

	tls_keypair_clear(keypair);

	free(keypair->cert_mem);
	free(keypair->key_mem);

	free(keypair);
}

int
tls_config_load_file(struct tls_error *error, const char *filetype,
    const char *filename, char **buf, size_t *len)
{
	struct stat st;
	int fd = -1;

	free(*buf);
	*buf = NULL;
	*len = 0;

	if ((fd = open(filename, O_RDONLY)) == -1) {
		tls_error_set(error, "failed to open %s file '%s'",
		    filetype, filename);
		goto fail;
	}
	if (fstat(fd, &st) != 0) {
		tls_error_set(error, "failed to stat %s file '%s'",
		    filetype, filename);
		goto fail;
	}
	*len = (size_t)st.st_size;
	if ((*buf = malloc(*len)) == NULL) {
		tls_error_set(error, "failed to allocate buffer for "
		    "%s file", filetype);
		goto fail;
	}
	if (read(fd, *buf, *len) != *len) {
		tls_error_set(error, "failed to read %s file '%s'",
		    filetype, filename);
		goto fail;
	}
	close(fd);
	return 0;

 fail:
	if (fd != -1)
		close(fd);
	if (*buf != NULL)
		explicit_bzero(*buf, *len);
	free(*buf);
	*buf = NULL;
	*len = 0;

	return -1;
}

struct tls_config *
tls_config_new(void)
{
	struct tls_config *config;

	if ((config = calloc(1, sizeof(*config))) == NULL)
		return (NULL);

	if ((config->keypair = tls_keypair_new()) == NULL)
		goto err;

	/*
	 * Default configuration.
	 */
	if (tls_config_set_dheparams(config, "none") != 0)
		goto err;
	if (tls_config_set_ecdhecurve(config, "auto") != 0)
		goto err;
	if (tls_config_set_ciphers(config, "secure") != 0)
		goto err;

	tls_config_set_protocols(config, TLS_PROTOCOLS_DEFAULT);
	tls_config_set_verify_depth(config, 6);

	tls_config_prefer_ciphers_server(config);

	tls_config_verify(config);

	return (config);

 err:
	tls_config_free(config);
	return (NULL);
}

void
tls_config_free(struct tls_config *config)
{
	struct tls_keypair *kp, *nkp;

	if (config == NULL)
		return;

	for (kp = config->keypair; kp != NULL; kp = nkp) {
		nkp = kp->next;
		tls_keypair_free(kp);
	}

	free(config->error.msg);

	free(config->alpn);
	free((char *)config->ca_mem);
	free((char *)config->ca_path);
	free((char *)config->ciphers);

	free(config);
}

static void
tls_config_keypair_add(struct tls_config *config, struct tls_keypair *keypair)
{
	struct tls_keypair *kp;

	kp = config->keypair;
	while (kp->next != NULL)
		kp = kp->next;

	kp->next = keypair;
}

const char *
tls_config_error(struct tls_config *config)
{
	return config->error.msg;
}

void
tls_config_clear_keys(struct tls_config *config)
{
	struct tls_keypair *kp;

	for (kp = config->keypair; kp != NULL; kp = kp->next)
		tls_keypair_clear(kp);

	tls_config_set_ca_mem(config, NULL, 0);
}

int
tls_config_parse_protocols(uint32_t *protocols, const char *protostr)
{
	uint32_t proto, protos = 0;
	char *s, *p, *q;
	int negate;

	if ((s = strdup(protostr)) == NULL)
		return (-1);

	q = s;
	while ((p = strsep(&q, ",:")) != NULL) {
		while (*p == ' ' || *p == '\t')
			p++;

		negate = 0;
		if (*p == '!') {
			negate = 1;
			p++;
		}

		if (negate && protos == 0)
			protos = TLS_PROTOCOLS_ALL;

		proto = 0;
		if (strcasecmp(p, "all") == 0 ||
		    strcasecmp(p, "legacy") == 0)
			proto = TLS_PROTOCOLS_ALL;
		else if (strcasecmp(p, "default") == 0 ||
		    strcasecmp(p, "secure") == 0)
			proto = TLS_PROTOCOLS_DEFAULT;
		if (strcasecmp(p, "tlsv1") == 0)
			proto = TLS_PROTOCOL_TLSv1;
		else if (strcasecmp(p, "tlsv1.0") == 0)
			proto = TLS_PROTOCOL_TLSv1_0;
		else if (strcasecmp(p, "tlsv1.1") == 0)
			proto = TLS_PROTOCOL_TLSv1_1;
		else if (strcasecmp(p, "tlsv1.2") == 0)
			proto = TLS_PROTOCOL_TLSv1_2;

		if (proto == 0) {
			free(s);
			return (-1);
		}

		if (negate)
			protos &= ~proto;
		else
			protos |= proto;
	}

	*protocols = protos;

	free(s);

	return (0);
}

static int
tls_config_parse_alpn(struct tls_config *config, const char *alpn,
    char **alpn_data, size_t *alpn_len)
{
	size_t buf_len, i, len;
	char *buf = NULL;
	char *s = NULL;
	char *p, *q;

	free(*alpn_data);
	*alpn_data = NULL;
	*alpn_len = 0;

	if ((buf_len = strlen(alpn) + 1) > 65535) {
		tls_config_set_errorx(config, "alpn too large");
		goto err;
	}

	if ((buf = malloc(buf_len)) == NULL) {
		tls_config_set_errorx(config, "out of memory");
		goto err;
	}

	if ((s = strdup(alpn)) == NULL) {
		tls_config_set_errorx(config, "out of memory");
		goto err;
	}

	i = 0;
	q = s;
	while ((p = strsep(&q, ",")) != NULL) {
		if ((len = strlen(p)) == 0) {
			tls_config_set_errorx(config,
			    "alpn protocol with zero length");
			goto err;
		}
		if (len > 255) {
			tls_config_set_errorx(config,
			    "alpn protocol too long");
			goto err;
		}
		buf[i++] = len & 0xff;
		memcpy(&buf[i], p, len);
		i += len;
	}

	free(s);

	*alpn_data = buf;
	*alpn_len = buf_len;

	return (0);

 err:
	free(buf);
	free(s);

	return (-1);
}

int
tls_config_set_alpn(struct tls_config *config, const char *alpn)
{
	return tls_config_parse_alpn(config, alpn, &config->alpn,
	    &config->alpn_len);
}

int
tls_config_add_keypair_file(struct tls_config *config,
    const char *cert_file, const char *key_file)
{
	struct tls_keypair *keypair;

	if ((keypair = tls_keypair_new()) == NULL)
		return (-1);
	if (tls_keypair_set_cert_file(keypair, &config->error, cert_file) != 0)
		goto err;
	if (tls_keypair_set_key_file(keypair, &config->error, key_file) != 0)
		goto err;

	tls_config_keypair_add(config, keypair);

	return (0);

 err:
	tls_keypair_free(keypair);
	return (-1);
}

int
tls_config_add_keypair_mem(struct tls_config *config, const uint8_t *cert,
    size_t cert_len, const uint8_t *key, size_t key_len)
{
	struct tls_keypair *keypair;

	if ((keypair = tls_keypair_new()) == NULL)
		return (-1);
	if (tls_keypair_set_cert_mem(keypair, cert, cert_len) != 0)
		goto err;
	if (tls_keypair_set_key_mem(keypair, key, key_len) != 0)
		goto err;

	tls_config_keypair_add(config, keypair);

	return (0);

 err:
	tls_keypair_free(keypair);
	return (-1);
}

int
tls_config_set_ca_file(struct tls_config *config, const char *ca_file)
{
	return tls_config_load_file(&config->error, "CA", ca_file,
	    &config->ca_mem, &config->ca_len);
}

int
tls_config_set_ca_path(struct tls_config *config, const char *ca_path)
{
	return set_string(&config->ca_path, ca_path);
}

int
tls_config_set_ca_mem(struct tls_config *config, const uint8_t *ca, size_t len)
{
	return set_mem(&config->ca_mem, &config->ca_len, ca, len);
}

int
tls_config_set_cert_file(struct tls_config *config, const char *cert_file)
{
	return tls_keypair_set_cert_file(config->keypair, &config->error,
	    cert_file);
}

int
tls_config_set_cert_mem(struct tls_config *config, const uint8_t *cert,
    size_t len)
{
	return tls_keypair_set_cert_mem(config->keypair, cert, len);
}

int
tls_config_set_ciphers(struct tls_config *config, const char *ciphers)
{
	SSL_CTX *ssl_ctx = NULL;

	if (ciphers == NULL ||
	    strcasecmp(ciphers, "default") == 0 ||
	    strcasecmp(ciphers, "secure") == 0)
		ciphers = TLS_CIPHERS_DEFAULT;
	else if (strcasecmp(ciphers, "compat") == 0)
		ciphers = TLS_CIPHERS_COMPAT;
	else if (strcasecmp(ciphers, "legacy") == 0)
		ciphers = TLS_CIPHERS_LEGACY;
	else if (strcasecmp(ciphers, "all") == 0 ||
	    strcasecmp(ciphers, "insecure") == 0)
		ciphers = TLS_CIPHERS_ALL;

	if ((ssl_ctx = SSL_CTX_new(SSLv23_method())) == NULL) {
		tls_config_set_errorx(config, "out of memory");
		goto fail;
	}
	if (SSL_CTX_set_cipher_list(ssl_ctx, ciphers) != 1) {
		tls_config_set_errorx(config, "no ciphers for '%s'", ciphers);
		goto fail;
	}

	SSL_CTX_free(ssl_ctx);
	return set_string(&config->ciphers, ciphers);

 fail:
	SSL_CTX_free(ssl_ctx);
	return -1;
}

int
tls_config_set_dheparams(struct tls_config *config, const char *params)
{
	int keylen;

	if (params == NULL || strcasecmp(params, "none") == 0)
		keylen = 0;
#if defined(SSL_CTX_set_dh_auto)
	else if (strcasecmp(params, "auto") == 0)
		keylen = -1;
	else if (strcasecmp(params, "legacy") == 0)
		keylen = 1024;
#endif
	else {
		tls_config_set_errorx(config, "invalid dhe param '%s'", params);
		return (-1);
	}

	config->dheparams = keylen;

	return (0);
}

int
tls_config_set_ecdhecurve(struct tls_config *config, const char *name)
{
	int nid;

	if (name == NULL || strcasecmp(name, "none") == 0)
		nid = NID_undef;
	else if (strcasecmp(name, "auto") == 0)
		nid = -1;
	else if ((nid = OBJ_txt2nid(name)) == NID_undef) {
		tls_config_set_errorx(config, "invalid ecdhe curve '%s'", name);
		return (-1);
	}

	config->ecdhecurve = nid;

	return (0);
}

int
tls_config_set_key_file(struct tls_config *config, const char *key_file)
{
	return tls_keypair_set_key_file(config->keypair, &config->error,
	    key_file);
}

int
tls_config_set_key_mem(struct tls_config *config, const uint8_t *key,
    size_t len)
{
	return tls_keypair_set_key_mem(config->keypair, key, len);
}

int
tls_config_set_keypair_file(struct tls_config *config,
    const char *cert_file, const char *key_file)
{
	if (tls_config_set_cert_file(config, cert_file) != 0)
		return (-1);
	if (tls_config_set_key_file(config, key_file) != 0)
		return (-1);

	return (0);
}

int
tls_config_set_keypair_mem(struct tls_config *config, const uint8_t *cert,
    size_t cert_len, const uint8_t *key, size_t key_len)
{
	if (tls_config_set_cert_mem(config, cert, cert_len) != 0)
		return (-1);
	if (tls_config_set_key_mem(config, key, key_len) != 0)
		return (-1);

	return (0);
}

int
tls_config_set_protocols(struct tls_config *config, uint32_t protocols)
{
	config->protocols = protocols;
}

int
tls_config_set_verify_depth(struct tls_config *config, int verify_depth)
{
	config->verify_depth = verify_depth;
}

void
tls_config_prefer_ciphers_client(struct tls_config *config)
{
	config->ciphers_server = 0;
}

void
tls_config_prefer_ciphers_server(struct tls_config *config)
{
	config->ciphers_server = 1;
}

void
tls_config_insecure_noverifycert(struct tls_config *config)
{
	config->verify_cert = 0;
}

void
tls_config_insecure_noverifyname(struct tls_config *config)
{
	config->verify_name = 0;
}

void
tls_config_insecure_noverifytime(struct tls_config *config)
{
	config->verify_time = 0;
}

void
tls_config_verify(struct tls_config *config)
{
	config->verify_cert = 1;
	config->verify_name = 1;
	config->verify_time = 1;
}

void
tls_config_verify_client(struct tls_config *config)
{
	config->verify_client = 1;
}

void
tls_config_verify_client_optional(struct tls_config *config)
{
	config->verify_client = 2;
}

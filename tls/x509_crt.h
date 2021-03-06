/**
 *		Tempesta TLS
 *
 * X.509 certificate parsing and writing
 *
 * Based on mbed TLS, https://tls.mbed.org.
 *
 * Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 * Copyright (C) 2015-2020 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifndef TTLS_X509_CRT_H
#define TTLS_X509_CRT_H

#include "x509.h"
#include "x509_crl.h"

#define TTLS_CERT_LEN_LEN			3

/**
 * Container for an X.509 certificate. The certificate may be chained.
 */
typedef struct ttls_x509_crt
{
	ttls_x509_buf raw;			   /**< The raw certificate data (DER). */
	ttls_x509_buf tbs;			   /**< The raw certificate body (DER). The part that is To Be Signed. */

	int version;				/**< The X.509 version. (1=v1, 2=v2, 3=v3) */
	ttls_x509_buf serial;			/**< Unique id for certificate issued by a specific CA. */
	ttls_x509_buf sig_oid;		   /**< Signature algorithm, e.g. sha1RSA */

	ttls_x509_buf issuer_raw;		/**< The raw issuer data (DER). Used for quick comparison. */
	ttls_x509_buf subject_raw;	   /**< The raw subject data (DER). Used for quick comparison. */

	ttls_x509_name issuer;		   /**< The parsed issuer data (named information object). */
	ttls_x509_name subject;		  /**< The parsed subject data (named information object). */

	ttls_x509_time valid_from;	   /**< Start time of certificate validity. */
	ttls_x509_time valid_to;		 /**< End time of certificate validity. */

	TlsPkCtx pk;			  /**< Container for the public key context. */

	ttls_x509_buf issuer_id;		 /**< Optional X.509 v2/v3 issuer unique identifier. */
	ttls_x509_buf subject_id;		/**< Optional X.509 v2/v3 subject unique identifier. */
	ttls_x509_buf v3_ext;			/**< Optional X.509 v3 extensions.  */
	ttls_x509_sequence subject_alt_names;	/**< Optional list of Subject Alternative Names (Only dNSName supported). */

	int ext_types;			  /**< Bit string containing detected and parsed extensions */
	int ca_istrue;			  /**< Optional Basic Constraint extension value: 1 if this certificate belongs to a CA, 0 otherwise. */
	int max_pathlen;			/**< Optional Basic Constraint extension value: The maximum path length to the root certificate. Path length is 1 higher than RFC 5280 'meaning', so 1+ */

	unsigned int key_usage;	 /**< Optional key usage extension value: See the values in x509.h */

	ttls_x509_sequence ext_key_usage; /**< Optional list of extended key usage OIDs. */

	unsigned char ns_cert_type; /**< Optional Netscape certificate type extension value: See the values in x509.h */

	ttls_x509_buf sig;			   /**< Signature: hash of the tbs part signed with the private key. */
	ttls_md_type_t sig_md;		   /**< Internal representation of the MD algorithm of the signature algorithm, e.g. TTLS_MD_SHA256 */
	ttls_pk_type_t sig_pk;		   /**< Internal representation of the Public Key algorithm of the signature algorithm, e.g. TTLS_PK_RSA */
	void *sig_opts;			 /**< Signature options to be passed to ttls_pk_verify_ext(), e.g. for RSASSA-PSS */

	struct ttls_x509_crt *next;	 /**< Next certificate in the CA-chain. */
}
ttls_x509_crt;

/**
 * Build flag from an algorithm/curve identifier (pk, md, ecp)
 * Since 0 is always XXX_NONE, ignore it.
 */
#define TTLS_X509_ID_FLAG(id)   (1 << (id - 1))

/**
 * Security profile for certificate verification.
 *
 * All lists are bitfields, built by ORing flags from TTLS_X509_ID_FLAG().
 */
typedef struct
{
	uint32_t allowed_mds;	   /**< MDs for signatures		 */
	uint32_t allowed_pks;	   /**< PK algs for signatures	 */
	uint32_t allowed_curves;	/**< Elliptic curves for ECDSA  */
	uint32_t rsa_min_bitlen;	/**< Minimum size for RSA keys  */
}
ttls_x509_crt_profile;

#define TTLS_X509_CRT_VERSION_1			  0
#define TTLS_X509_CRT_VERSION_2			  1
#define TTLS_X509_CRT_VERSION_3			  2

#define TTLS_X509_RFC5280_MAX_SERIAL_LEN 32
#define TTLS_X509_RFC5280_UTC_TIME_LEN   15

/**
 * Container for writing a certificate (CRT)
 */
typedef struct ttls_x509write_cert
{
	int version;
	TlsMpi serial;
	TlsPkCtx *subject_key;
	TlsPkCtx *issuer_key;
	ttls_asn1_named_data *subject;
	ttls_asn1_named_data *issuer;
	ttls_md_type_t md_alg;
	char not_before[TTLS_X509_RFC5280_UTC_TIME_LEN + 1];
	char not_after[TTLS_X509_RFC5280_UTC_TIME_LEN + 1];
	ttls_asn1_named_data *extensions;
}
ttls_x509write_cert;

/**
 * Default security profile. Should provide a good balance between security
 * and compatibility with current deployments.
 */
extern const ttls_x509_crt_profile ttls_x509_crt_profile_default;

/**
 * Expected next default profile. Recommended for new deployments.
 * Currently targets a 128-bit security level, except for RSA-2048.
 */
extern const ttls_x509_crt_profile ttls_x509_crt_profile_next;

/**
 * NSA Suite B profile.
 */
extern const ttls_x509_crt_profile ttls_x509_crt_profile_suiteb;

int ttls_x509_crt_parse_der(ttls_x509_crt *chain, unsigned char *buf,
			    size_t buflen);

/**
 * Parse one or more certificates and add them to the chained list. Parses
 * permissively. If some certificates can be parsed, the result is the number
 * of failed certificates it encountered. If none complete correctly, the first
 * error is returned.
 */
int ttls_x509_crt_parse(ttls_x509_crt *chain, unsigned char *buf, size_t buflen);

/**
 * \brief	  Verify the certificate signature
 *
 *			 The verify callback is a user-supplied callback that
 *			 can clear / modify / add flags for a certificate. If set,
 *			 the verification callback is called for each
 *			 certificate in the chain (from the trust-ca down to the
 *			 presented crt). The parameters for the callback are:
 *			 (void *parameter, ttls_x509_crt *crt, int certificate_depth,
 *			 int *flags). With the flags representing current flags for
 *			 that specific certificate and the certificate depth from
 *			 the bottom (Peer cert depth = 0).
 *
 *			 All flags left after returning from the callback
 *			 are also returned to the application. The function should
 *			 return 0 for anything (including invalid certificates)
 *			 other than fatal error, as a non-zero return code
 *			 immediately aborts the verification process. For fatal
 *			 errors, a specific error code should be used (different
 *			 from TTLS_ERR_X509_CERT_VERIFY_FAILED which should not
 *			 be returned at this point), or TTLS_ERR_X509_FATAL_ERROR
 *			 can be used if no better code is available.
 *
 * \note	   Same as \c ttls_x509_crt_verify_with_profile() with the
 *			 default security profile.
 *
 * \note	   It is your responsibility to provide up-to-date CRLs for
 *			 all trusted CAs. If no CRL is provided for the CA that was
 *			 used to sign the certificate, CRL verification is skipped
 *			 silently, that is *without* setting any flag.
 *
 * \param crt	  a certificate (chain) to be verified
 * \param trust_ca the list of trusted CAs
 * \param ca_crl   the list of CRLs for trusted CAs (see note above)
 * \param cn	   expected Common Name (can be set to
 *				 NULL if the CN must not be verified)
 * \param flags	result of the verification
 *
 * \return		 0 (and flags set to 0) if the chain was verified and valid,
 *			 TTLS_ERR_X509_CERT_VERIFY_FAILED if the chain was verified
 *			 but found to be invalid, in which case *flags will have one
 *			 or more TTLS_X509_BADCERT_XXX or TTLS_X509_BADCRL_XXX
 *			 flags set, or another error (and flags set to 0xffffffff)
 *			 in case of a fatal error encountered during the
 *			 verification process.
 */
int ttls_x509_crt_verify(ttls_x509_crt *crt, ttls_x509_crt *trust_ca,
			 ttls_x509_crl *ca_crl, const char *cn,
			 uint32_t *flags);

/**
 * \brief		  Verify the certificate signature according to profile
 *
 * \note		   Same as \c ttls_x509_crt_verify(), but with explicit
 *				 security profile.
 *
 * \note		   The restrictions on keys (RSA minimum size, allowed curves
 *				 for ECDSA) apply to all certificates: trusted root,
 *				 intermediate CAs if any, and end entity certificate.
 *
 * \param crt	  a certificate (chain) to be verified
 * \param trust_ca the list of trusted CAs
 * \param ca_crl   the list of CRLs for trusted CAs
 * \param profile  security profile for verification
 * \param cn	   expected Common Name (can be set to
 *				 NULL if the CN must not be verified)
 * \param flags	result of the verification
 *
 * \return		 0 if successful or TTLS_ERR_X509_CERT_VERIFY_FAILED
 *				 in which case *flags will have one or more
 *				 TTLS_X509_BADCERT_XXX or TTLS_X509_BADCRL_XXX flags
 *				 set,
 *				 or another error in case of a fatal error encountered
 *				 during the verification process.
 */
int ttls_x509_crt_verify_with_profile(ttls_x509_crt *crt,
		 ttls_x509_crt *trust_ca,
		 ttls_x509_crl *ca_crl,
		 const ttls_x509_crt_profile *profile,
		 const char *cn, uint32_t *flags);

/**
 * \brief		  Check usage of certificate against keyUsage extension.
 *
 * \param crt	  Leaf certificate used.
 * \param usage	Intended usage(s) (eg TTLS_X509_KU_KEY_ENCIPHERMENT
 *				 before using the certificate to perform an RSA key
 *				 exchange).
 *
 * \note		   Except for decipherOnly and encipherOnly, a bit set in the
 *				 usage argument means this bit MUST be set in the
 *				 certificate. For decipherOnly and encipherOnly, it means
 *				 that bit MAY be set.
 *
 * \return		 0 is these uses of the certificate are allowed,
 *				 TTLS_ERR_X509_BAD_INPUT_DATA if the keyUsage extension
 *				 is present but does not match the usage argument.
 *
 * \note		   You should only call this function on leaf certificates, on
 *				 (intermediate) CAs the keyUsage extension is automatically
 *				 checked by \c ttls_x509_crt_verify().
 */
int ttls_x509_crt_check_key_usage(const ttls_x509_crt *crt,
			  unsigned int usage);

/**
 * \brief		   Check usage of certificate against extendedKeyUsage.
 *
 * \param crt	   Leaf certificate used.
 * \param usage_oid Intended usage (eg TTLS_OID_SERVER_AUTH or
 *				  TTLS_OID_CLIENT_AUTH).
 * \param usage_len Length of usage_oid (eg given by TTLS_OID_SIZE()).
 *
 * \return		  0 if this use of the certificate is allowed,
 *				  TTLS_ERR_X509_BAD_INPUT_DATA if not.
 *
 * \note			Usually only makes sense on leaf certificates.
 */
int ttls_x509_crt_check_extended_key_usage(const ttls_x509_crt *crt,
		   const char *usage_oid,
		   size_t usage_len);

/**
 * \brief		  Verify the certificate revocation status
 *
 * \param crt	  a certificate to be verified
 * \param crl	  the CRL to verify against
 *
 * \return		 1 if the certificate is revoked, 0 otherwise
 *
 */
int ttls_x509_crt_is_revoked(const ttls_x509_crt *crt, const ttls_x509_crl *crl);

void ttls_x509_crt_init(ttls_x509_crt *crt);
void ttls_x509_crt_free(ttls_x509_crt *crt);

/**
 * Writes certificate length in exactly TTLS_CERT_LEN_LEN bytes of @buf.
 */
static inline void
ttls_x509_write_cert_len(const ttls_x509_crt *crt, unsigned char *buf)
{
	size_t n = crt->raw.len;

	buf[0] = (unsigned char)(n >> 16);
	buf[1] = (unsigned char)(n >> 8);
	buf[2] = (unsigned char)n;
}

static inline void *
ttls_x509_crt_page(const ttls_x509_crt *crt)
{
	unsigned long addr = (unsigned long)crt->raw.p - TTLS_CERT_LEN_LEN;

	BUG_ON(addr & ~PAGE_MASK);

	return (void *)addr;
}

#endif /* TTLS_X509_CRT_H */

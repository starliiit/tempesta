/*
 *		Tempesta TLS
 *
 * Elliptic curves over GF(p): generic functions.
 *
 * References:
 *
 * 1. SEC1 http://www.secg.org/index.php?action=secg,docs_secg
 *
 * 2. GECC = Guide to Elliptic Curve Cryptography - Hankerson, Menezes, Vanstone
 *
 * 3. FIPS 186-3 http://csrc.nist.gov/publications/fips/fips186-3/fips_186-3.pdf
 *
 * 4. RFC 8422 for the related TLS structures and constants
 *
 * 5. [Curve25519] http://cr.yp.to/ecdh/curve25519-20060209.pdf
 *
 * 6. CORON, Jean-S'ebastien. Resistance against differential power analysis
 *    for elliptic curve cryptosystems. In : Cryptographic Hardware and
 *    Embedded Systems. Springer Berlin Heidelberg, 1999. p. 292-302.
 *    <http://link.springer.com/chapter/10.1007/3-540-48059-5_25>
 *
 * 7. HEDABOU, Mustapha, PINEL, Pierre, et B'EN'ETEAU, Lucien. A comb method to
 *    render ECC resistant against Side Channel Attacks. IACR Cryptology
 *    ePrint Archive, 2004, vol. 2004, p. 342.
 *    <http://eprint.iacr.org/2004/342.pdf>
 *
 * 8. Jacobian coordinates for short Weierstrass curves,
 *    http://www.hyperelliptic.org/EFD/g1p/auto-shortw-jacobian.html
 *
 * 9. S.Gueron, V.KRasnov, "Fast prime field elliptic-curve cryptography with
 *    256-bit primes", 2014.
 *
 * 10. NIST: Mathematical routines for the NIST prime elliptic curves, 2010.
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
#include <linux/types.h>

#include "tls_internal.h"
#include "bignum_asm.h"
#include "ecp.h"
#include "mpool.h"

typedef enum {
	ECP_TYPE_SHORT_WEIERSTRASS,	/* y^2 = x^3 + a x + b */
	ECP_TYPE_MONTGOMERY,		/* y^2 = x^3 + a x^2 + x */
} ecp_curve_type;

/*
 * List of supported curves (RFC 8422):
 *  - internal ID
 *  - TLS NamedCurve ID (RFC 8422 5.1.1, RFC 7071 sec. 2)
 *  - size in bits
 *  - readable name
 *
 * Secp256r1 is at the first postion as the most used one.
 *
 * TODO #1031 add Curve25519 and Curve448.
 *
 * Reminder: update profiles in x509_crt.c when adding a new curves!
 */
static const TlsEcpCurveInfo ecp_supported_curves[] = {
	{ TTLS_ECP_DP_SECP256R1,	23,	 256,	"secp256r1"},
	{ TTLS_ECP_DP_SECP384R1,	24,	 384,	"secp384r1"},
	{ TTLS_ECP_DP_NONE,		0,	 0,	NULL},
};

#define ECP_NB_CURVES   sizeof(ecp_supported_curves) /	\
			sizeof(ecp_supported_curves[0])

ttls_ecp_group_id ttls_preset_curves[] = {
	TTLS_ECP_DP_SECP256R1,
	TTLS_ECP_DP_SECP384R1,
	TTLS_ECP_DP_NONE
};

/**
 * Get the curve info for the internal identifier.
 */
const TlsEcpCurveInfo *
ttls_ecp_curve_info_from_grp_id(ttls_ecp_group_id grp_id)
{
	const TlsEcpCurveInfo *curve_info;

	for (curve_info = ecp_supported_curves;
	     curve_info->grp_id != TTLS_ECP_DP_NONE;
	     curve_info++)
	{
		if (curve_info->grp_id == grp_id)
			return curve_info;
	}

	return NULL;
}

/**
 * Get the curve info from the TLS identifier.
 */
const TlsEcpCurveInfo *
ttls_ecp_curve_info_from_tls_id(uint16_t tls_id)
{
	const TlsEcpCurveInfo *curve_info;

	T_DBG3("try curve id %#x from the client\n", tls_id);
	for (curve_info = ecp_supported_curves;
	     curve_info->grp_id != TTLS_ECP_DP_NONE;
	     curve_info++)
	{
		if (curve_info->tls_id == tls_id)
			return curve_info;
	}
	return NULL;
}

static inline ecp_curve_type
ecp_get_type(const TlsEcpGrp *grp)
{
	return ttls_mpi_empty(&grp->G.Y)
		? ECP_TYPE_MONTGOMERY
		: ECP_TYPE_SHORT_WEIERSTRASS;
}

void
ttls_ecp_point_init(TlsEcpPoint *pt)
{
	ttls_mpi_init_next(&pt->X, 0);
	ttls_mpi_init_next(&pt->Y, 0);
	ttls_mpi_init_next(&pt->Z, 0);
}

#define ttls_ecp_point_tmp_alloc_init(pt, xn, yn, zn)			\
do {									\
	pt = __builtin_alloca(sizeof(TlsEcpPoint) + CIL * (xn + yn + zn)); \
	BUG_ON(!pt);							\
	pt->X.s = 1;							\
	pt->X.used = 0;							\
	pt->X.limbs = xn;						\
	pt->X._off = xn ? sizeof(*pt) : 0;				\
	pt->Y.s = 1;							\
	pt->Y.used = 0;							\
	pt->Y.limbs = yn;						\
	pt->Y._off = yn ? sizeof(*pt) - sizeof(pt->X) + xn * CIL : 0;	\
	pt->Z.s = 1;							\
	pt->Z.used = 0;							\
	pt->Z.limbs = zn;						\
	pt->Z._off = zn ? sizeof(pt->Z) + (xn + yn) * CIL : 0;		\
} while (0)

/**
 * Called after ttls_mpi_pool_create() using __GFP_ZERO, so all the @key
 * members are zero here.
 */
void
ttls_ecp_keypair_init(TlsEcpKeypair *key)
{
	ttls_mpi_init_next(&key->d, 0);
	ttls_ecp_point_init(&key->Q);
}

void
ttls_ecp_keypair_free(TlsEcpKeypair *key)
{
	if (WARN_ON_ONCE(!key))
		return;
	ttls_mpi_pool_free(key);
}

void
ttls_ecp_copy(TlsEcpPoint *P, const TlsEcpPoint *Q)
{
	ttls_mpi_copy(&P->X, &Q->X);
	ttls_mpi_copy(&P->Y, &Q->Y);
	ttls_mpi_copy(&P->Z, &Q->Z);
}

static void
ttls_ecp_set_zero(TlsEcpPoint *pt)
{
	ttls_mpi_lset(&pt->X , 1);
	ttls_mpi_lset(&pt->Y , 1);
	ttls_mpi_lset(&pt->Z , 0);
}

int
ttls_ecp_is_zero(TlsEcpPoint *pt)
{
	return !ttls_mpi_cmp_int(&pt->Z, 0);
}

/*
 * Export a point into unsigned binary data (SEC1 2.3.3).
 * Uncompressed is the only point format supported by RFC 8422.
 *
 * @grp		- Group to which the point should belong;
 * @p		- Point to export;
 * @olen	- Length of the actual output;
 * @buf		- Output buffer;
 * @buflen	- Length of the output buffer.
 */
static int
ttls_ecp_point_write_binary(const TlsEcpGrp *grp, const TlsEcpPoint *P,
			    size_t *olen, unsigned char *buf, size_t buflen)
{
	size_t plen;

	/* Common case: P == 0 . */
	if (!ttls_mpi_cmp_int(&P->Z, 0)) {
		if (buflen < 1)
			return -ENOSPC;

		buf[0] = 0x00;
		*olen = 1;

		return 0;
	}

	plen = ttls_mpi_size(&grp->P);

	*olen = 2 * plen + 1;

	if (buflen < *olen)
		return -ENOSPC;

	buf[0] = 0x04;
	if (ttls_mpi_write_binary(&P->X, buf + 1, plen)
	    || ttls_mpi_write_binary(&P->Y, buf + 1 + plen, plen))
		return -ENOSPC;

	return 0;
}

/**
 * Import a point from unsigned binary data (SEC1 2.3.4).
 */
int
ttls_ecp_point_read_binary(const TlsEcpGrp *grp, TlsEcpPoint *pt,
			   const unsigned char *buf, size_t ilen)
{
	size_t plen;

	if (ilen < 1)
		return TTLS_ERR_ECP_BAD_INPUT_DATA;

	if (buf[0] == 0x00) {
		if (ilen == 1) {
			ttls_ecp_set_zero(pt);
			return 0;
		}
		return TTLS_ERR_ECP_BAD_INPUT_DATA;
	}

	plen = ttls_mpi_size(&grp->P);

	if (buf[0] != 0x04)
		return TTLS_ERR_ECP_FEATURE_UNAVAILABLE;

	if (ilen != 2 * plen + 1)
		return TTLS_ERR_ECP_BAD_INPUT_DATA;

	ttls_mpi_read_binary(&pt->X, buf + 1, plen);
	ttls_mpi_read_binary(&pt->Y, buf + 1 + plen, plen);
	ttls_mpi_lset(&pt->Z, 1);

	return 0;
}

/**
 * Import a point from a TLS ECPoint record (RFC 8443 5.4)
 *	struct {
 *		opaque point <1..2^8-1>;
 *	} ECPoint;
 */
int
ttls_ecp_tls_read_point(const TlsEcpGrp *grp, TlsEcpPoint *pt,
			const unsigned char **buf, size_t buf_len)
{
	unsigned char data_len;
	const unsigned char *buf_start;

	/*
	 * We must have at least two bytes (1 for length,
	 * at least one for data).
	 */
	if (buf_len < 2)
		return TTLS_ERR_ECP_BAD_INPUT_DATA;

	data_len = *(*buf)++;
	if (data_len < 1 || data_len > buf_len - 1)
		return TTLS_ERR_ECP_BAD_INPUT_DATA;

	/* Save buffer start for read_binary and update buf. */
	buf_start = *buf;
	*buf += data_len;

	return ttls_ecp_point_read_binary(grp, pt, buf_start, data_len);
}

/**
 * Export a point as a TLS ECPoint record (RFC 8422 5.4)
 *	struct {
 *		opaque point <1..2^8-1>;
 *	} ECPoint;
 */
int
ttls_ecp_tls_write_point(const TlsEcpGrp *grp, const TlsEcpPoint *pt,
			 size_t *olen, unsigned char *buf, size_t blen)
{
	int r;

	/* Buffer length must be at least one, for our length byte. */
	if (blen < 1)
		return -EINVAL;

	r = ttls_ecp_point_write_binary(grp, pt, olen, buf + 1, blen - 1);
	if (r)
		return r;

	/* Write length to the first byte and update total length. */
	buf[0] = (unsigned char) *olen;
	++*olen;

	return 0;
}

/**
 * Set a group from an ECParameters record (RFC 8422 5.4).
 * TODO #769 used in client mode only - fix the ECP group destination address.
 */
int
ttls_ecp_tls_read_group(TlsEcpGrp *grp, const unsigned char **buf, size_t len)
{
	uint16_t tls_id;
	const TlsEcpCurveInfo *curve_info;

	/* We expect at least three bytes (see below). */
	if (len < 3)
		return TTLS_ERR_ECP_BAD_INPUT_DATA;

	/* First byte is curve_type; only named_curve is handled. */
	if (*(*buf)++ != TTLS_ECP_TLS_NAMED_CURVE)
		return TTLS_ERR_ECP_BAD_INPUT_DATA;

	/* Next two bytes are the namedcurve value. */
	tls_id = *(*buf)++;
	tls_id <<= 8;
	tls_id |= *(*buf)++;

	if (!(curve_info = ttls_ecp_curve_info_from_tls_id(tls_id)))
		return TTLS_ERR_ECP_FEATURE_UNAVAILABLE;

	return ttls_ecp_group_load(grp, curve_info->grp_id);
}

/**
 * Write the ECParameters record corresponding to a group (RFC 8422 5.4).
 */
int
ttls_ecp_tls_write_group(const TlsEcpGrp *grp, size_t *olen,
			 unsigned char *buf, size_t blen)
{
	const TlsEcpCurveInfo *curve_info;

	if (!(curve_info = ttls_ecp_curve_info_from_grp_id(grp->id)))
		return -EINVAL;

	/* We are going to write 3 bytes (see below). */
	*olen = 3;
	if (blen < *olen)
		return -ENOSPC;

	/* First byte is curve_type, always named_curve. */
	*buf++ = TTLS_ECP_TLS_NAMED_CURVE;

	/* Next two bytes are the namedcurve value. */
	buf[0] = curve_info->tls_id >> 8;
	buf[1] = curve_info->tls_id & 0xFF;

	return 0;
}

/**
 * Wrapper around fast quasi-modp functions.
 */
static void
ecp_modp(TlsMpi *N, const TlsEcpGrp *grp)
{
	BUG_ON(N->limbs < grp->bits * 2 / BIL);
	BUG_ON(N->s < 0);

	if (N->used > grp->bits / BIL)
		/*
		 * P modulo is very close to the maximum value of 4-limbs MPI,
		 * so only one addition or subtraction will be enough to
		 * get the modulo and we don't need to execute the exepnsive
		 * reduction operation.
		 */
		grp->modp(N);

	while (N->s < 0 && ttls_mpi_cmp_int(N, 0))
		ttls_mpi_add_mpi(N, N, &grp->P);

	while (ttls_mpi_cmp_mpi(N, &grp->P) >= 0)
		/* We known P, N and the result are positive. */
		ttls_mpi_sub_abs(N, N, &grp->P);
}

/*
 * Fast mod-p functions expect their argument to be in the 0..p^2 range.
 *
 * In order to guarantee that, we need to ensure that operands of
 * ttls_mpi_mul_mpi are in the 0..p range. So, after each operation we will
 * bring the result back to this range.
 *
 * The following macros are shortcuts for doing that.
 */

/*
 * Reduce a TlsMpi mod p in-place, general case, to use after ttls_mpi_mul_mpi
 */
#define MOD_MUL(N)	ecp_modp(N, grp)

/*
 * Reduce a TlsMpi mod p in-place, to use after ttls_mpi_sub_mpi
 * N->s < 0 is a very fast test, which fails only if N is 0
 */
#define MOD_SUB(N)							\
	while ((N)->s < 0 && ttls_mpi_cmp_int(N, 0))			\
		ttls_mpi_add_mpi(N, N, &grp->P)

/*
 * Reduce a TlsMpi mod p in-place, to use after ttls_mpi_add_mpi().
 * We known P, N and the result are positive, so sub_abs is correct, and
 * a bit faster.
 */
#define MOD_ADD(N)							\
	while (ttls_mpi_cmp_mpi(N, &grp->P) >= 0)			\
		ttls_mpi_sub_abs(N, N, &grp->P)

/**
 * Wrapper around fast quasi-mod for SECP 256.
 */
static void
ecp_mod_p256(TlsMpi *N)
{
	BUG_ON(N->limbs < 4);
	BUG_ON(N->s < 0);

	ecp_mod_p256_x86_64(MPI_P(N), N->used);
	mpi_fixup_used(N, 4);
}

static void
ecp_mul_mod(const TlsEcpGrp *grp, TlsMpi *X, const TlsMpi *A, const TlsMpi *B)
{
	BUG_ON(X->limbs < grp->bits / BIL);

	if (likely(grp->bits == 256)) {
		BUG_ON(A->used != 4 || B->used != 4);
		mpi_mul_x86_64_4(MPI_P(X), MPI_P(A), MPI_P(B));

		mpi_fixup_used(X, 8);
		X->s = A->s * B->s;

		ecp_mod_p256(X);
	} else {
		/* TODO #1335: also optimize for Secp384. */
		ttls_mpi_mul_mpi(X, A, B);
		ecp_modp(X, grp);
	}
}

static void
ecp_sqr_mod(const TlsEcpGrp *grp, TlsMpi *X, const TlsMpi *A)
{
	BUG_ON(X->limbs < grp->bits / BIL);

	if (likely(grp->bits == 256)) {
		BUG_ON(A->used != 4);
		mpi_sqr_x86_64_4(MPI_P(X), MPI_P(A));

		mpi_fixup_used(X, 8);
		X->s = 1;

		ecp_mod_p256(X);
	} else {
		/* TODO #1064: also optimize for Secp384 and call mpi sqr. */
		ttls_mpi_mul_mpi(X, A, A);
		ecp_modp(X, grp);
	}
}

/*
 * For curves in short Weierstrass form, we do all the internal operations in
 * Jacobian coordinates.
 *
 * For multiplication, we'll use a comb method with coutermeasueres against
 * SPA, hence timing attacks.
 */

/*
 * Normalize jacobian coordinates so that Z == 0 || Z == 1  (GECC 3.2.1)
 * Cost: 1N := 1I + 3M + 1S
 */
static int
ecp_normalize_jac(const TlsEcpGrp *grp, TlsEcpPoint *pt)
{
	TlsMpi *Zi, *ZZi;

	if (!ttls_mpi_cmp_int(&pt->Z, 0))
		return 0;

	Zi = ttls_mpi_alloc_stack_init(grp->bits / BIL);
	ZZi = ttls_mpi_alloc_stack_init(grp->bits * 2 / BIL);

	/* X = X / Z^2  mod p */
	MPI_CHK(ttls_mpi_inv_mod(Zi, &pt->Z, &grp->P));
	ecp_sqr_mod(grp, ZZi, Zi);
	ecp_mul_mod(grp, &pt->X, &pt->X, ZZi);

	/* Y = Y / Z^3  mod p */
	ecp_mul_mod(grp, &pt->Y, &pt->Y, ZZi);
	ecp_mul_mod(grp, &pt->Y, &pt->Y, Zi);

	/* Z = 1 */
	ttls_mpi_lset(&pt->Z, 1);

	return 0;
}

/*
 * Normalize jacobian coordinates of an array of (pointers to) points,
 * using Montgomery's trick to perform only one inversion mod P.
 * (See for example Cohen's "A Course in Computational Algebraic Number
 * Theory", Algorithm 10.3.4.)
 *
 * Warning: fails (returning an error) if one of the points is zero!
 * This should never happen, see choice of w in ecp_mul_comb().
 *
 * Cost: 1N(t) := 1I + (6t - 3)M + 1S
 */
static int
ecp_normalize_jac_many(const TlsEcpGrp *grp, TlsEcpPoint *T[], size_t t_len)
{
#define __INIT_C(i)							\
do {									\
	c[i].s = 1;							\
	c[i].used = 0;							\
	c[i].limbs = n_limbs;						\
	c[i]._off = (unsigned long)p_limbs - (unsigned long)(c + i);	\
	p_limbs += n_limbs;						\
} while (0)

	int i, ret = 0;
	unsigned long *p_limbs, n_limbs = grp->bits * 2 / BIL;
	TlsMpi *u, *Zi, *ZZi, *c;

	WARN_ON_ONCE(t_len < 2);
	BUG_ON(t_len > TTLS_ECP_WINDOW_SIZE);

	c = ttls_mpool_alloc_stack((sizeof(TlsMpi) + n_limbs * CIL) * t_len);
	u = ttls_mpi_alloc_stack_init(n_limbs);
	Zi = ttls_mpi_alloc_stack_init(n_limbs);
	ZZi = ttls_mpi_alloc_stack_init(n_limbs);
	bzero_fast(c, sizeof(TlsMpi) * t_len);
	p_limbs = (unsigned long *)&c[t_len];

	/* c[i] = Z_0 * ... * Z_i */
	__INIT_C(0);
	ttls_mpi_copy_alloc(&c[0], &T[0]->Z, false);
	for (i = 1; i < t_len; i++) {
		__INIT_C(i);
		ecp_mul_mod(grp, &c[i], &c[i - 1], &T[i]->Z);
	}

	/* u = 1 / (Z_0 * ... * Z_n) mod P */
	TTLS_MPI_CHK(ttls_mpi_inv_mod(u, &c[t_len - 1], &grp->P));

	for (i = t_len - 1; i >= 0; i--) {
		/*
		 * Zi = 1 / Z_i mod p
		 * u = 1 / (Z_0 * ... * Z_i) mod P
		 */
		if (!i) {
			ttls_mpi_copy(Zi, u);
		} else {
			ecp_mul_mod(grp, Zi, u, &c[i - 1]);
			ecp_mul_mod(grp, u, u, &T[i]->Z);
		}

		/* proceed as in normalize(). */
		ecp_sqr_mod(grp, ZZi, Zi);
		ecp_mul_mod(grp, &T[i]->X, &T[i]->X, ZZi);
		ecp_mul_mod(grp, &T[i]->Y, &T[i]->Y, ZZi);
		ecp_mul_mod(grp, &T[i]->Y, &T[i]->Y, Zi);
		/*
		 * At the moment Z coordinate stores a garbage, so free it now
		 * and treat as 1 on subsequent processing.
		 */
		ttls_mpi_reset(&T[i]->Z);
	}

cleanup:
	ttls_mpi_pool_cleanup_ctx((unsigned long)c, false);
	return ret;
#undef __INIT_C
}

/**
 * Conditional point inversion: Q -> -Q = (Q.X, -Q.Y, Q.Z) without leak.
 * "inv" must be 0 (don't invert) or 1 (invert) or the result will be invalid.
 */
static void
ecp_safe_invert_jac(const TlsEcpGrp *grp, TlsEcpPoint *Q, unsigned char inv)
{
	unsigned char nonzero;
	TlsMpi *mQY = ttls_mpi_alloc_stack_init(grp->P.used);

	/* Use the fact that -Q.Y mod P = P - Q.Y unless Q.Y == 0 */
	ttls_mpi_sub_mpi(mQY, &grp->P, &Q->Y);
	nonzero = !!ttls_mpi_cmp_int(&Q->Y, 0);

	ttls_mpi_safe_cond_assign(&Q->Y, mQY, inv & nonzero);
}

/**
 * Point doubling R = 2 P, Jacobian coordinates [8, "dbl-1998-cmo-2"].
 *
 * We follow the variable naming fairly closely. The formula variations that
 * trade a MUL for a SQR (plus a few ADDs) aren't useful as our bignum
 * implementation doesn't distinguish squaring.
 *
 * Standard optimizations are applied when curve parameter A is one of {0, -3}.
 *
 * Cost: 1D := 3M + 4S	(A ==  0)
 *	 4M + 4S	(A == -3)
 *	 3M + 6S + 1a	otherwise
 */
static int
ecp_double_jac(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsEcpPoint *P)
{
	TlsMpi M, S, T, U;

	ttls_mpi_alloca_init(&M, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&S, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&T, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&U, grp->bits * 2 / BIL);

	if (ttls_mpi_empty(&grp->A)) {
		/*
		 * NIST curves case: A = -3
		 * M = 3(X + Z^2)(X - Z^2)
		 */
		if (likely(ttls_mpi_cmp_int(&P->Z, 1)))
			ecp_sqr_mod(grp, &S, &P->Z);
		else
			ttls_mpi_lset(&S, 1);
		ttls_mpi_add_mpi(&T, &P->X, &S);
		MOD_ADD(&T);
		ttls_mpi_sub_mpi(&U, &P->X, &S);
		MOD_SUB(&U);
		ecp_mul_mod(grp, &S, &T, &U);
		ttls_mpi_copy_alloc(&M, &S, false);
		ttls_mpi_shift_l(&M, 1);
		ttls_mpi_add_mpi(&M, &M, &S);
		MOD_ADD(&M);
	} else {
		/* M = 3 * X^2 */
		ecp_sqr_mod(grp, &S, &P->X);
		ttls_mpi_copy_alloc(&M, &S, false);
		ttls_mpi_shift_l(&M, 1);
		ttls_mpi_add_mpi(&M, &M, &S);
		MOD_ADD(&M);
	}

	/* S = 4 * X * Y^2 */
	ecp_sqr_mod(grp, &T, &P->Y);
	ttls_mpi_shift_l(&T, 1);
	MOD_ADD(&T);
	ecp_mul_mod(grp, &S, &P->X, &T);
	ttls_mpi_shift_l(&S, 1);
	MOD_ADD(&S);

	/* U = 8.Y^4 */
	ecp_sqr_mod(grp, &U, &T);
	ttls_mpi_shift_l(&U, 1);
	MOD_ADD(&U);

	/* T = M^2 - 2 * S */
	ecp_sqr_mod(grp, &T, &M);
	ttls_mpi_sub_mpi(&T, &T, &S);
	MOD_SUB(&T);
	ttls_mpi_sub_mpi(&T, &T, &S);
	MOD_SUB(&T);

	/* S = M(S - T) - U */
	ttls_mpi_sub_mpi(&S, &S, &T);
	MOD_SUB(&S);
	ecp_mul_mod(grp, &S, &S, &M);
	ttls_mpi_sub_mpi(&S, &S, &U);
	MOD_SUB(&S);

	/* U = 2 * Y * Z */
	if (likely(ttls_mpi_cmp_int(&P->Z, 1)))
		ecp_mul_mod(grp, &U, &P->Y, &P->Z);
	else
		ttls_mpi_copy(&U, &P->Y);
	ttls_mpi_shift_l(&U, 1);
	MOD_ADD(&U);

	ttls_mpi_copy(&R->X, &T);
	ttls_mpi_copy(&R->Y, &S);
	ttls_mpi_copy(&R->Z, &U);

	return 0;
}

/*
 * Addition: R = P + Q, mixed affine-Jacobian coordinates (GECC 3.22)
 *
 * #TODO #1064: the implementation uses formula [8, "madd-2008-g"] and I'm not
 * sure if it's the most efficient one - [9] refernces another formula.
 *
 * The coordinates of Q must be normalized (= affine),
 * but those of P don't need to. R is not normalized.
 *
 * Special cases: (1) P or Q is zero, (2) R is zero, (3) P == Q.
 * None of these cases can happen as intermediate step in ecp_mul_comb():
 * - at each step, P, Q and R are multiples of the base point, the factor
 *   being less than its order, so none of them is zero;
 * - Q is an odd multiple of the base point, P an even multiple,
 *   due to the choice of precomputed points in the modified comb method.
 * So branches for these cases do not leak secret information.
 *
 * We accept Q->Z being unset (saving memory in tables) as meaning 1.
 *
 * Cost: 1A := 8M + 3S
 */
static int
ecp_add_mixed(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsEcpPoint *P,
	      const TlsEcpPoint *Q)
{
	TlsMpi T1, T2, T3, T4, X, Y, Z;

	/* Trivial cases: P == 0 or Q == 0 (case 1). */
	if (!ttls_mpi_cmp_int(&P->Z, 0)) {
		ttls_ecp_copy(R, Q);
		return 0;
	}
	if (!ttls_mpi_empty(&Q->Z)) {
		if (!ttls_mpi_cmp_int(&Q->Z, 0)) {
			ttls_ecp_copy(R, P);
			return 0;
		}
		/* Make sure Q coordinates are normalized. */
		if (ttls_mpi_cmp_int(&Q->Z, 1))
			return -EINVAL;
	}

	ttls_mpi_alloca_init(&T1, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&T2, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&T3, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&T4, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&X, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&Y, grp->bits * 2 / BIL);
	ttls_mpi_alloca_init(&Z, grp->bits * 2 / BIL);

	if (unlikely(!ttls_mpi_cmp_int(&P->Z, 1))) {
		/* Relatively rare case, ~1/60. */
		ttls_mpi_sub_mpi(&T1, &Q->X, &P->X);
		MOD_SUB(&T1);
		ttls_mpi_sub_mpi(&T2, &Q->Y, &P->Y);
		MOD_SUB(&T2);
	} else {
		ecp_sqr_mod(grp, &T1, &P->Z);
		ecp_mul_mod(grp, &T2, &T1, &P->Z);
		ecp_mul_mod(grp, &T1, &T1, &Q->X);
		ecp_mul_mod(grp, &T2, &T2, &Q->Y);
		ttls_mpi_sub_mpi(&T1, &T1, &P->X);
		MOD_SUB(&T1);
		ttls_mpi_sub_mpi(&T2, &T2, &P->Y);
		MOD_SUB(&T2);
	}

	/* Special cases (2) and (3) */
	if (!ttls_mpi_cmp_int(&T1, 0)) {
		if (!ttls_mpi_cmp_int(&T2, 0)) {
			return ecp_double_jac(grp, R, P);
		} else {
			ttls_ecp_set_zero(R);
			return 0;
		}
	}

	if (unlikely(!ttls_mpi_cmp_int(&P->Z, 1)))
		ttls_mpi_copy_alloc(&Z, &T1, false);
	else
		ecp_mul_mod(grp, &Z, &P->Z, &T1);
	ecp_sqr_mod(grp, &T3, &T1);
	ecp_mul_mod(grp, &T4, &T3, &T1);
	ecp_mul_mod(grp, &T3, &T3, &P->X);
	ttls_mpi_copy_alloc(&T1, &T3, false);
	ttls_mpi_shift_l(&T1, 1);
	MOD_ADD(&T1);
	ecp_sqr_mod(grp, &X, &T2);
	ttls_mpi_sub_mpi(&X, &X, &T1);
	MOD_SUB(&X);
	ttls_mpi_sub_mpi(&X, &X, &T4);
	MOD_SUB(&X);
	ttls_mpi_sub_mpi(&T3, &T3, &X);
	MOD_SUB(&T3);
	ecp_mul_mod(grp, &T3, &T3, &T2);
	ecp_mul_mod(grp, &T4, &T4, &P->Y);
	ttls_mpi_sub_mpi(&Y, &T3, &T4);
	MOD_SUB(&Y);

	/* Resulting coorinates are twice smaller than the temporary MPIs. */
	ttls_mpi_copy(&R->X, &X);
	ttls_mpi_copy(&R->Y, &Y);
	ttls_mpi_copy(&R->Z, &Z);

	return 0;
}

/*
 * Randomize jacobian coordinates:
 * (X, Y, Z) -> (l^2 X, l^3 Y, l Z) for random l
 * This is sort of the reverse operation of ecp_normalize_jac().
 *
 * This countermeasure was first suggested in [2]. See also the recommendation
 * for SPA and DPA attacks prevention in J.Coron, "Resistance against
 * Differential Power Analysis for Elliptic Curve Cryptosystems".
 */
static int
ecp_randomize_jac(const TlsEcpGrp *grp, TlsEcpPoint *pt)
{
	TlsMpi l, ll;
	size_t p_size = (grp->bits + 7) / 8;
	int count = 0;

	ttls_mpi_alloca_init(&l, p_size);
	ttls_mpi_alloca_init(&ll, p_size * 2);

	/* Generate l such that 1 < l < p */
	do {
		ttls_mpi_fill_random(&l, p_size);

		while (ttls_mpi_cmp_mpi(&l, &grp->P) >= 0)
			ttls_mpi_shift_r(&l, 1);

		if (count++ > 10)
			return TTLS_ERR_ECP_RANDOM_FAILED;
	} while (ttls_mpi_cmp_int(&l, 1) <= 0);

	/* Z = l * Z */
	if (likely(ttls_mpi_cmp_int(&pt->Z, 1)))
		ecp_mul_mod(grp, &pt->Z, &pt->Z, &l);
	else
		ttls_mpi_copy_alloc(&pt->Z, &l, false);

	/* X = l^2 * X */
	ecp_sqr_mod(grp, &ll, &l);
	ecp_mul_mod(grp, &pt->X, &pt->X, &ll);

	/* Y = l^3 * Y */
	ecp_mul_mod(grp, &ll, &ll, &l);
	ecp_mul_mod(grp, &pt->Y, &pt->Y, &ll);

	return 0;
}

/* d = ceil(n / w) */
#define COMB_MAX_D	  (TTLS_ECP_MAX_BITS + 1) / 2

/*
 * Compute the representation of m that will be used with our comb method.
 *
 * The basic comb method is described in GECC 3.44 for example. We use a
 * modified version that provides resistance to SPA by avoiding zero
 * digits in the representation as in [3]. We modify the method further by
 * requiring that all K_i be odd, which has the small cost that our
 * representation uses one more K_i, due to carries.
 *
 * Also, for the sake of compactness, only the seven low-order bits of x[i]
 * are used to represent K_i, and the msb of x[i] encodes the sign (s_i in
 * the paper): it is set if and only if s_i == -1;
 *
 * Calling conventions:
 * - x is an array of size d + 1
 * - w is the size, ie number of teeth, of the comb, and must be between
 *   2 and 7 (in practice, between 2 and TTLS_ECP_WINDOW_ORDER)
 * - m is the MPI, expected to be odd and such that bitlength(m) <= w * d
 *   (the result will be incorrect if these assumptions are not satisfied)
 */
static void
ecp_comb_fixed(unsigned char x[], size_t d, unsigned char w, const TlsMpi *m)
{
	size_t i, j;
	unsigned char c, cc, adjust;

	bzero_fast(x, d + 1);

	/* First get the classical comb values (except for x_d = 0) */
	for (i = 0; i < d; i++)
		for (j = 0; j < w; j++)
			x[i] |= ttls_mpi_get_bit(m, i + d * j) << j;

	/* Now make sure x_1 .. x_d are odd */
	for (c = 0, i = 1; i <= d; i++) {
		/* Add carry and update it */
		cc = x[i] & c;
		x[i] = x[i] ^ c;
		c = cc;

		/* Adjust if needed, avoiding branches */
		adjust = 1 - (x[i] & 0x01);
		c |= x[i] & (x[i-1] * adjust);
		x[i] = x[i] ^ (x[i-1] * adjust);
		x[i-1] |= adjust << 7;
	}
}

/*
 * Precompute points for the comb method
 *
 * If i = i_{w-1} ... i_1 is the binary representation of i, then
 * T[i] = i_{w-1} 2^{(w-1)d} P + ... + i_1 2^d P + P
 *
 * T must be able to hold 2^{w - 1} elements
 *
 * Cost: d(w-1) D + (2^{w-1} - 1) A + 1 N(w-1) + 1 N(2^{w-1} - 1)
 */
int
ecp_precompute_comb(const TlsEcpGrp *grp, TlsEcpPoint T[], const TlsEcpPoint *P,
		    unsigned char w, size_t d)
{
	int i, j, k;
	TlsEcpPoint *cur, *TT[TTLS_ECP_WINDOW_SIZE];

	/*
	 * Set T[0] = P and T[2^{i-1}] = 2^{di} P for i = 1 .. w-1
	 * (this is not the final value).
	 */
	ttls_ecp_copy(&T[0], P);

	k = 0;
	for (i = 1; i < (1U << (w - 1)); i <<= 1) {
		cur = T + i;
		ttls_ecp_copy(cur, T + (i >> 1));
		for (j = 0; j < d; j++)
			/* TODO #1064 use repeated doubling optimization. */
			MPI_CHK(ecp_double_jac(grp, cur, cur));

		TT[k++] = cur;
	}
	BUG_ON(!k || k >= TTLS_ECP_WINDOW_ORDER);

	MPI_CHK(ecp_normalize_jac_many(grp, TT, k));

	/*
	 * Compute the remaining ones using the minimal number of additions
	 * Be careful to update T[2^l] only after using it!
	 */
	k = 0;
	for (i = 1; i < (1U << (w - 1)); i <<= 1) {
		j = i;
		while (j--) {
			MPI_CHK(ecp_add_mixed(grp, &T[i + j], &T[j], &T[i]));
			TT[k++] = &T[i + j];
		}
	}

	MPI_CHK(ecp_normalize_jac_many(grp, TT, k));

	return 0;
}

/*
 * Select precomputed point: R = sign(i) * T[ abs(i) / 2 ]
 */
static void
ecp_select_comb(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsEcpPoint T[],
		unsigned char t_len, unsigned char i)
{
	unsigned char ii, j;

	/* Ignore the "sign" bit and scale down */
	ii =  (i & 0x7Fu) >> 1;

	/* Read the whole table to thwart cache-based timing attacks */
	for (j = 0; j < t_len; j++) {
		/* TODO #1064 do specialization to avoid conditions. */
		ttls_mpi_safe_cond_assign(&R->X, &T[j].X, j == ii);
		ttls_mpi_safe_cond_assign(&R->Y, &T[j].Y, j == ii);
	}

	/* Safely invert result if i is "negative" */
	ecp_safe_invert_jac(grp, R, i >> 7);
}

/*
 * Core multiplication algorithm for the (modified) comb method.
 * This part is actually common with the basic comb method (GECC 3.44)
 *
 * Cost: d A + d D + 1 R
 */
static int
ecp_mul_comb_core(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsEcpPoint T[],
		  unsigned char t_len, const unsigned char x[], size_t d,
		  bool rnd)
{
	TlsEcpPoint *Txi;
	size_t i;

	ttls_ecp_point_tmp_alloc_init(Txi, T->X.used, T->Y.used, 0);
	ttls_mpi_alloc(&R->X, grp->bits * 2 / BIL);
	ttls_mpi_alloc(&R->Y, grp->bits * 2 / BIL);
	ttls_mpi_alloc(&R->Z, grp->bits / BIL + 1);

	/* Start with a non-zero point and randomize its coordinates */
	i = d;
	ecp_select_comb(grp, R, T, t_len, x[i]);
	ttls_mpi_lset(&R->Z, 1);
	if (rnd)
		MPI_CHK(ecp_randomize_jac(grp, R));

	while (i-- != 0) {
		/* TODO #1064 use repeated doubling optimization. */
		MPI_CHK(ecp_double_jac(grp, R, R));
		ecp_select_comb(grp, Txi, T, t_len, x[i]);
		MPI_CHK(ecp_add_mixed(grp, R, R, Txi));
	}

	return 0;
}

/*
 * Multiplication using the comb method, for curves in short Weierstrass form.
 *
 * May allocate @R point on the stack, so while the function uses plenty of
 * memory we can't call ttls_mpi_pool_cleanup_ctx() here.
 *
 * TODO #1064: why wNAF isn't used?
 */
static int
ecp_mul_comb(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsMpi *m,
	     const TlsEcpPoint *P, bool rnd)
{
	int ret = -EINVAL;
	unsigned char w, m_is_odd, p_eq_g, pre_len;
	size_t d = max(m->used, grp->N.used);
	TlsEcpPoint *T;
	TlsMpi *M, *mm;
	unsigned char k[COMB_MAX_D + 1];

	/* We need N to be odd to transform m in an odd number, check now. */
	if (WARN_ON_ONCE(ttls_mpi_get_bit(&grp->N, 0) != 1))
		return -EINVAL;

	M = ttls_mpi_alloc_stack_init(d);
	mm = ttls_mpi_alloc_stack_init(d);

	/*
	 * Minimize the number of multiplications, that is minimize
	 * 10 * d * w + 18 * 2^(w-1) + 11 * d + 7 * w, with d = ceil(bits / w)
	 * (see costs of the various parts, with 1S = 1M).
	 * TODO #1064 make sure that w size is the best one.
	 */
	BUG_ON(grp->bits > 384);
	w = grp->bits == 384 ? 5 : 4;

	/*
	 * If P == G, pre-compute a bit more, since this may be re-used later.
	 * Just adding one avoids upping the cost of the first mul too much,
	 * and the memory cost too.
	 */
	p_eq_g = !ttls_mpi_cmp_mpi(&P->Y, &grp->G.Y)
		 && !ttls_mpi_cmp_mpi(&P->X, &grp->G.X);
	if (p_eq_g) {
		w++;
		T = (TlsEcpPoint *)grp->T; /* we won't change it */
		MPI_CHK(ttls_mpi_empty(&T->X) | ttls_mpi_empty(&T->Y));
	} else {
		if (!(T = ttls_mpool_ecp_create_tmp_T(1 << (w - 1), P)))
			return -ENOMEM;
	}
	WARN_ON_ONCE(w > TTLS_ECP_WINDOW_ORDER);

	/* Other sizes that depend on w */
	pre_len = 1U << (w - 1);
	if (WARN_ON_ONCE(pre_len > TTLS_ECP_WINDOW_SIZE))
		goto cleanup;
	d = (grp->bits + w - 1) / w;
	BUG_ON(d > COMB_MAX_D);

	/*
	 * Compute T if it wasn't precomputed for the case.
	 * ecp_precompute_comb() is good with uninitialized T.
	 */
	if (!p_eq_g)
		TTLS_MPI_CHK(ecp_precompute_comb(grp, T, P, w, d));

	/*
	 * Make sure M is odd (M = m or M = N - m, since N is odd)
	 * using the fact that m * P = - (N - m) * P
	 */
	m_is_odd = (ttls_mpi_get_bit(m, 0) == 1);
	ttls_mpi_copy(M, m);
	ttls_mpi_sub_mpi(mm, &grp->N, m);
	ttls_mpi_safe_cond_assign(M, mm, !m_is_odd);

	/* Go for comb multiplication, R = M * P */
	ecp_comb_fixed(k, d, w, M);
	TTLS_MPI_CHK(ecp_mul_comb_core(grp, R, T, pre_len, k, d, rnd));

	/* Now get m * P from M * P and normalize it. */
	ecp_safe_invert_jac(grp, R, !m_is_odd);
	TTLS_MPI_CHK(ecp_normalize_jac(grp, R));

cleanup:
	if (!p_eq_g)
		ttls_mpool_shrink_tailtmp(ttls_mpool(T), false);

	return ret;
}

/*
 * For Montgomery curves, we do all the internal arithmetic in projective
 * coordinates. Import/export of points uses only the x coordinates, which is
 * internaly represented as X / Z.
 *
 * For scalar multiplication, we'll use a Montgomery ladder.
 */

/*
 * Normalize Montgomery x/z coordinates: X = X/Z, Z = 1
 * Cost: 1M + 1I
 */
static int
ecp_normalize_mxz(const TlsEcpGrp *grp, TlsEcpPoint *P)
{
	MPI_CHK(ttls_mpi_inv_mod(&P->Z, &P->Z, &grp->P));
	ecp_mul_mod(grp, &P->X, &P->X, &P->Z);
	ttls_mpi_lset(&P->Z, 1);

	return 0;
}

/*
 * Randomize projective x/z coordinates: (X, Z) -> (l X, l Z) for random l.
 * This is sort of the reverse operation of ecp_normalize_mxz().
 *
 * This countermeasure was first suggested in [2].
 * Cost: 2M
 */
static int
ecp_randomize_mxz(const TlsEcpGrp *grp, TlsEcpPoint *P)
{
	TlsMpi *l = ttls_mpi_alloc_stack_init(0);
	size_t p_size;
	int count = 0;

	p_size = (grp->bits + 7) / 8;

	/* Generate l such that 1 < l < p */
	do {
		ttls_mpi_fill_random(l, p_size);

		while (ttls_mpi_cmp_mpi(l, &grp->P) >= 0)
			ttls_mpi_shift_r(l, 1);

		if (count++ > 10)
			return TTLS_ERR_ECP_RANDOM_FAILED;
	} while (ttls_mpi_cmp_int(l, 1) <= 0);

	ecp_mul_mod(grp, &P->X, &P->X, l);
	ecp_mul_mod(grp, &P->Z, &P->Z, l);

	return 0;
}

/**
 * Double-and-add: R = 2P, S = P + Q, with d = X(P - Q), for Montgomery curves
 * in x/z coordinates.
 *
 * http://www.hyperelliptic.org/EFD/g1p/auto-code/montgom/xz/ladder/mladd-1987-m.op3
 * with
 * d =  X1
 * P = (X2, Z2)
 * Q = (X3, Z3)
 * R = (X4, Z4)
 * S = (X5, Z5)
 * and eliminating temporary variables tO, ..., t4.
 *
 * Cost: 5M + 4S
 */
static int
ecp_double_add_mxz(const TlsEcpGrp *grp, TlsEcpPoint *R, TlsEcpPoint *S,
		   const TlsEcpPoint *P, const TlsEcpPoint *Q, const TlsMpi *d)
{
	TlsMpi *A, *AA, *B, *BB, *E, *C, *D, *DA, *CB;
	size_t n;

	n = sizeof(TlsMpi) * 9 + CIL * ((max(P->X.used, P->Z.used) + 1) * 9
					+ (max(Q->X.used, Q->Z.used) + 1) * 4);
	A = ttls_mpool_alloc_stack(n);
	AA = ttls_mpi_init_next(A, max(P->X.used, P->Z.used) + 1);
	B = ttls_mpi_init_next(AA, A->limbs * 2);
	BB = ttls_mpi_init_next(B, max(P->X.used, P->Z.used));
	E = ttls_mpi_init_next(BB, B->limbs * 2);
	C = ttls_mpi_init_next(E, max(P->X.used, P->Z.used) * 2);
	D = ttls_mpi_init_next(C, max(Q->X.used, Q->Z.used) + 1);
	DA = ttls_mpi_init_next(D, max(Q->X.used, Q->Z.used));
	CB = ttls_mpi_init_next(DA, D->limbs + A->limbs);
	ttls_mpi_init_next(CB, C->limbs + B->limbs);

	ttls_mpi_add_mpi(A, &P->X, &P->Z);
	MOD_ADD(A);
	ecp_sqr_mod(grp, AA, A);
	ttls_mpi_sub_mpi(B, &P->X, &P->Z);
	MOD_SUB(B);
	ecp_sqr_mod(grp, BB, B);
	ttls_mpi_sub_mpi(E, AA, BB);
	MOD_SUB(E);
	ttls_mpi_add_mpi(C, &Q->X, &Q->Z);
	MOD_ADD(C);
	ttls_mpi_sub_mpi(D, &Q->X, &Q->Z);
	MOD_SUB(D);
	ecp_mul_mod(grp, DA, D, A);
	ecp_mul_mod(grp, CB, C, B);
	ttls_mpi_add_mpi(&S->X, DA, CB);
	MOD_MUL(&S->X);
	ecp_sqr_mod(grp, &S->X, &S->X);
	ttls_mpi_sub_mpi(&S->Z, DA, CB);
	MOD_SUB(&S->Z);
	ecp_sqr_mod(grp, &S->Z, &S->Z);
	ecp_mul_mod(grp, &S->Z, &S->Z, d);
	ecp_mul_mod(grp, &R->X, AA, BB);
	ecp_mul_mod(grp, &R->Z, &grp->A, E);
	ttls_mpi_add_mpi(&R->Z, BB, &R->Z);
	MOD_ADD(&R->Z);
	ecp_mul_mod(grp, &R->Z, &R->Z, E);

	return 0;
}

/**
 * Multiplication with Montgomery ladder in x/z coordinates,
 * for curves in Montgomery form.
 */
static int
ecp_mul_mxz(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsMpi *m,
	    const TlsEcpPoint *P, bool rng)
{
	size_t i;
	unsigned char b;
	TlsEcpPoint *RP;
	TlsMpi *PX;

	PX = ttls_mpi_alloc_stack_init(0);
	RP = ttls_mpool_alloc_stack(sizeof(*RP));
	ttls_ecp_point_init(RP);

	/* Save PX and read from P before writing to R, in case P == R */
	ttls_mpi_copy(PX, &P->X);
	ttls_ecp_copy(RP, P);

	/* Set R to zero in modified x/z coordinates */
	ttls_mpi_lset(&R->X, 1);
	ttls_mpi_lset(&R->Z, 0);
	ttls_mpi_reset(&R->Y);

	/* RP.X might be sligtly larger than P, so reduce it */
	MOD_ADD(&RP->X);

	/* Randomize coordinates of the starting point */
	if (rng)
		MPI_CHK(ecp_randomize_mxz(grp, RP));

	/*
	 * Loop invariant: R = result so far, RP = R + P.
	 * One past the (zero-based) most significant bit.
	 */
	i = ttls_mpi_bitlen(m);
	while (i-- > 0) {
		b = ttls_mpi_get_bit(m, i);
		/*
		 *  if (b) R = 2R + P else R = 2R,
		 * which is:
		 *  if (b) double_add(RP, R, RP, R)
		 *  else   double_add(R, RP, R, RP)
		 * but using safe conditional swaps to avoid leaks
		 */
		MPI_CHK(ttls_mpi_safe_cond_swap(&R->X, &RP->X, b));
		MPI_CHK(ttls_mpi_safe_cond_swap(&R->Z, &RP->Z, b));
		MPI_CHK(ecp_double_add_mxz(grp, R, RP, R, RP, PX));
		MPI_CHK(ttls_mpi_safe_cond_swap(&R->X, &RP->X, b));
		MPI_CHK(ttls_mpi_safe_cond_swap(&R->Z, &RP->Z, b));
	}

	return ecp_normalize_mxz(grp, R);
}

/**
 * Multiplication R = m * P.
 *
 * In order to prevent timing attacks, this function executes the exact same
 * sequence of (base field) operations for any valid m. It avoids any if-branch
 * or array index depending on the value of m.
 *
 * If @rng is true, the functions randomizes intermediate results in order to
 * prevent potential timing attacks targeting these results.
 */
int
ttls_ecp_mul(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsMpi *m,
	     const TlsEcpPoint *P, bool rnd)
{
	switch (ecp_get_type(grp)) {
	case ECP_TYPE_MONTGOMERY:
		return ecp_mul_mxz(grp, R, m, P, rnd);
	case ECP_TYPE_SHORT_WEIERSTRASS:
		return ecp_mul_comb(grp, R, m, P, rnd);
	}
	BUG();
}

/**
 * TODO #1064: Specialization for R = m * G.
 */
int
ttls_ecp_mul_g(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsMpi *m,				     bool rnd)
{
	return ttls_ecp_mul(grp, R, m, &grp->G, rnd);
}

/**
 * R = m * P with shortcuts for m == 1 and m == -1.
 * NOT constant-time - ONLY for short Weierstrass!
 */
static int
ttls_ecp_mul_shortcuts(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsMpi *m,
		       const TlsEcpPoint *P)
{
	if (!ttls_mpi_cmp_int(m, 1)) {
		ttls_ecp_copy(R, P);
	}
	else if (!ttls_mpi_cmp_int(m, -1)) {
		ttls_ecp_copy(R, P);
		if (ttls_mpi_cmp_int(&R->Y, 0))
			ttls_mpi_sub_mpi(&R->Y, &grp->P, &R->Y);
	}
	else {
		MPI_CHK(ttls_ecp_mul(grp, R, m, P, false));
	}

	return 0;
}

/*
 * Multiplication and addition of two points by integers: R = m * grp->G + n * Q
 * In contrast to ttls_ecp_mul(), this function does not guarantee a constant
 * execution flow and timing.
 */
int
ttls_ecp_muladd(const TlsEcpGrp *grp, TlsEcpPoint *R, const TlsMpi *m,
		const TlsMpi *n, const TlsEcpPoint *Q)
{
	TlsEcpPoint *mP;

	if (WARN_ON_ONCE(ecp_get_type(grp) != ECP_TYPE_SHORT_WEIERSTRASS))
		return -EINVAL;

	mP = ttls_mpool_alloc_stack(sizeof(TlsEcpPoint));
	ttls_ecp_point_init(mP);

	MPI_CHK(ttls_ecp_mul_shortcuts(grp, mP, m, &grp->G));
	MPI_CHK(ttls_ecp_mul_shortcuts(grp, R, n, Q));
	MPI_CHK(ecp_add_mixed(grp, R, mP, R));
	MPI_CHK(ecp_normalize_jac(grp, R));

	return 0;
}

/**
 * Check that a point is valid as a public key.
 *
 * This function only checks the point is non-zero, has valid coordinates and
 * lies on the curve, but not that it is indeed a multiple of G. This is
 * additional check is more expensive, isn't required by standards, and
 * shouldn't be necessary if the group used has a small cofactor. In particular,
 * it is useless for the NIST groups which all have a cofactor of 1.
 *
 * Uses bare components rather than an TlsEcpKeypair structure in order to ease
 * use with other structures such as TlsECDHCtx of TlsEcpKeypair.
 */
int
ttls_ecp_check_pubkey(const TlsEcpGrp *grp, const TlsEcpPoint *pt)
{
#ifdef DEBUG
	TlsMpi *YY, *RHS;

	/* Must use affine coordinates */
	if (WARN_ON_ONCE(ttls_mpi_cmp_int(&pt->Z, 1)))
		return -EINVAL;

	if (ecp_get_type(grp) == ECP_TYPE_MONTGOMERY) {
		/*
		 * Check validity of a public key for Montgomery curves with
		 * x-only schemes. [Curve25519 p. 5] Just check X is the correct
		 * number of bytes.
		 */
		if (WARN_ON_ONCE(ttls_mpi_size(&pt->X) > (grp->bits + 7) / 8))
			return -EINVAL;
		return 0;
	}

	/*
	 * Check that an affine point is valid as a public key,
	 * short Weierstrass curves (SEC1 3.2.3.1).
	 *
	 * pt coordinates must be normalized for our checks.
	 */
	if (ttls_mpi_cmp_mpi(&pt->X, &grp->P) >= 0
	    || ttls_mpi_cmp_mpi(&pt->Y, &grp->P) >= 0)
	{
		T_DBG_MPI3("ECP invalid weierstrass public key",
			   &pt->X, &pt->Y, &grp->P);
		return -EINVAL;
	}

	YY = ttls_mpi_alloc_stack_init(grp->bits * 2 / BIL);
	RHS = ttls_mpi_alloc_stack_init(grp->bits * 2 / BIL);

	/*
	 * YY = Y^2
	 * RHS = X (X^2 + A) + B = X^3 + A X + B
	 */
	ecp_sqr_mod(grp, YY, &pt->Y);
	ecp_sqr_mod(grp, RHS, &pt->X);

	/* Special case for A = -3 */
	if (ttls_mpi_empty(&grp->A)) {
		ttls_mpi_sub_int(RHS, RHS, 3);
		MOD_SUB(RHS);
	} else {
		ttls_mpi_add_mpi(RHS, RHS, &grp->A);
		MOD_ADD(RHS);
	}

	ecp_mul_mod(grp, RHS, RHS, &pt->X);
	ttls_mpi_add_mpi(RHS, RHS, &grp->B);
	MOD_ADD(RHS);

	if (ttls_mpi_cmp_mpi(YY, RHS)) {
		T_DBG_MPI2("ECP invalid weierstrass public key", YY, RHS);
		return -EINVAL;
	}
#endif
	return 0;
}

/**
 * Check that an TlsMpi is valid as a private key.
 *
 * Uses bare components rather than an TlsEcpKeypair structure in order to ease
 * use with other structures such as TlsECDHCtx of TlsEcpKeypair.
 */
int
ttls_ecp_check_privkey(const TlsEcpGrp *grp, const TlsMpi *d)
{
	switch (ecp_get_type(grp)) {
	case ECP_TYPE_MONTGOMERY:
		/* see [Curve25519] page 5 */
		if (ttls_mpi_get_bit(d, 0)
		    || ttls_mpi_get_bit(d, 1)
		    || ttls_mpi_get_bit(d, 2)
		    /* ttls_mpi_bitlen is one-based! */
		    || ttls_mpi_bitlen(d) - 1 != grp->bits)
		{
			T_DBG_MPI1("ECP bad montgomery priv key", d);
			return -EINVAL;
		}
		return 0;
	case ECP_TYPE_SHORT_WEIERSTRASS:
		/* see SEC1 3.2 */
		if (ttls_mpi_cmp_int(d, 1) < 0
		    || ttls_mpi_cmp_mpi(d, &grp->N) >= 0)
		{
			T_DBG_MPI2("ECP bad weierstrass priv key", d, &grp->N);
			return -EINVAL;
		}
		return 0;
	}
	BUG();
}

/**
 * Generate a keypair with configurable base point.
 */
int
ttls_ecp_gen_keypair(const TlsEcpGrp *grp, TlsMpi *d, TlsEcpPoint *Q)
{
	size_t n_size = (grp->bits + 7) / 8;

	if (ecp_get_type(grp) == ECP_TYPE_MONTGOMERY) {
		/* [M225] page 5 */
		size_t b;

		do {
			ttls_mpi_fill_random(d, n_size);
		} while (!ttls_mpi_bitlen(d));

		/* Make sure the most significant bit is bits */
		b = ttls_mpi_bitlen(d) - 1; /* ttls_mpi_bitlen is one-based */
		if (b > grp->bits)
			ttls_mpi_shift_r(d, b - grp->bits);
		else
			ttls_mpi_set_bit(d, grp->bits, 1);

		/* Make sure the last three bits are unset */
		ttls_mpi_set_bit(d, 0, 0);
		ttls_mpi_set_bit(d, 1, 0);
		ttls_mpi_set_bit(d, 2, 0);
	} else {
		/* SEC1 3.2.1: Generate d such that 1 <= n < N */
		int count = 0;

		/*
		 * Match the procedure given in RFC 6979 (deterministic ECDSA):
		 * - use the same byte ordering;
		 * - keep the leftmost bits bits of the generated octet string;
		 * - try until result is in the desired range.
		 * This also avoids any biais, which is especially important
		 * for ECDSA.
		 */
		do {
			ttls_mpi_fill_random(d, n_size);
			ttls_mpi_shift_r(d, 8 * n_size - grp->bits);

			/*
			 * Each try has at worst a probability 1/2 of failing
			 * (the msb has a probability 1/2 of being 0, and then
			 * the result will be < N), so after 30 tries failure
			 * probability is a most 2**(-30).
			 *
			 * For most curves, 1 try is enough with overwhelming
			 * probability, since N starts with a lot of 1s in
			 * binary, but some curves such as secp224k1 are
			 * actually very close to the worst case.
			 */
			if (WARN_ON_ONCE(++count > 10))
				return TTLS_ERR_ECP_RANDOM_FAILED;
		}
		while (!ttls_mpi_cmp_int(d, 0)
		       || ttls_mpi_cmp_mpi(d, &grp->N) >= 0)
			;
	}

	MPI_CHK(ttls_ecp_mul_g(grp, Q, d, true));

	return ttls_ecp_check_pubkey(grp, Q);
}

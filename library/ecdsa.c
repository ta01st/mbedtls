/*
 *  Elliptic curve DSA
 *
 *  Copyright (C) 2006-2014, ARM Limited, All Rights Reserved
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * References:
 *
 * SEC1 http://www.secg.org/index.php?action=secg,docs_secg
 */

#if !defined(POLARSSL_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include POLARSSL_CONFIG_FILE
#endif

#if defined(POLARSSL_ECDSA_C)

#include "mbedtls/ecdsa.h"
#include "mbedtls/asn1write.h"

#include <string.h>

#if defined(POLARSSL_ECDSA_DETERMINISTIC)
#include "mbedtls/hmac_drbg.h"
#endif

/*
 * Derive a suitable integer for group grp from a buffer of length len
 * SEC1 4.1.3 step 5 aka SEC1 4.1.4 step 3
 */
static int derive_mpi( const ecp_group *grp, mpi *x,
                       const unsigned char *buf, size_t blen )
{
    int ret;
    size_t n_size = ( grp->nbits + 7 ) / 8;
    size_t use_size = blen > n_size ? n_size : blen;

    MPI_CHK( mpi_read_binary( x, buf, use_size ) );
    if( use_size * 8 > grp->nbits )
        MPI_CHK( mpi_shift_r( x, use_size * 8 - grp->nbits ) );

    /* While at it, reduce modulo N */
    if( mpi_cmp_mpi( x, &grp->N ) >= 0 )
        MPI_CHK( mpi_sub_mpi( x, x, &grp->N ) );

cleanup:
    return( ret );
}

/*
 * Compute ECDSA signature of a hashed message (SEC1 4.1.3)
 * Obviously, compared to SEC1 4.1.3, we skip step 4 (hash message)
 */
int ecdsa_sign( ecp_group *grp, mpi *r, mpi *s,
                const mpi *d, const unsigned char *buf, size_t blen,
                int (*f_rng)(void *, unsigned char *, size_t), void *p_rng )
{
    int ret, key_tries, sign_tries, blind_tries;
    ecp_point R;
    mpi k, e, t;

    /* Fail cleanly on curves such as Curve25519 that can't be used for ECDSA */
    if( grp->N.p == NULL )
        return( POLARSSL_ERR_ECP_BAD_INPUT_DATA );

    ecp_point_init( &R );
    mpi_init( &k ); mpi_init( &e ); mpi_init( &t );

    sign_tries = 0;
    do
    {
        /*
         * Steps 1-3: generate a suitable ephemeral keypair
         * and set r = xR mod n
         */
        key_tries = 0;
        do
        {
            MPI_CHK( ecp_gen_keypair( grp, &k, &R, f_rng, p_rng ) );
            MPI_CHK( mpi_mod_mpi( r, &R.X, &grp->N ) );

            if( key_tries++ > 10 )
            {
                ret = POLARSSL_ERR_ECP_RANDOM_FAILED;
                goto cleanup;
            }
        }
        while( mpi_cmp_int( r, 0 ) == 0 );

        /*
         * Step 5: derive MPI from hashed message
         */
        MPI_CHK( derive_mpi( grp, &e, buf, blen ) );

        /*
         * Generate a random value to blind inv_mod in next step,
         * avoiding a potential timing leak.
         */
        blind_tries = 0;
        do
        {
            size_t n_size = ( grp->nbits + 7 ) / 8;
            MPI_CHK( mpi_fill_random( &t, n_size, f_rng, p_rng ) );
            MPI_CHK( mpi_shift_r( &t, 8 * n_size - grp->nbits ) );

            /* See ecp_gen_keypair() */
            if( ++blind_tries > 30 )
                return( POLARSSL_ERR_ECP_RANDOM_FAILED );
        }
        while( mpi_cmp_int( &t, 1 ) < 0 ||
               mpi_cmp_mpi( &t, &grp->N ) >= 0 );

        /*
         * Step 6: compute s = (e + r * d) / k = t (e + rd) / (kt) mod n
         */
        MPI_CHK( mpi_mul_mpi( s, r, d ) );
        MPI_CHK( mpi_add_mpi( &e, &e, s ) );
        MPI_CHK( mpi_mul_mpi( &e, &e, &t ) );
        MPI_CHK( mpi_mul_mpi( &k, &k, &t ) );
        MPI_CHK( mpi_inv_mod( s, &k, &grp->N ) );
        MPI_CHK( mpi_mul_mpi( s, s, &e ) );
        MPI_CHK( mpi_mod_mpi( s, s, &grp->N ) );

        if( sign_tries++ > 10 )
        {
            ret = POLARSSL_ERR_ECP_RANDOM_FAILED;
            goto cleanup;
        }
    }
    while( mpi_cmp_int( s, 0 ) == 0 );

cleanup:
    ecp_point_free( &R );
    mpi_free( &k ); mpi_free( &e ); mpi_free( &t );

    return( ret );
}

#if defined(POLARSSL_ECDSA_DETERMINISTIC)
/*
 * Deterministic signature wrapper
 */
int ecdsa_sign_det( ecp_group *grp, mpi *r, mpi *s,
                    const mpi *d, const unsigned char *buf, size_t blen,
                    md_type_t md_alg )
{
    int ret;
    hmac_drbg_context rng_ctx;
    unsigned char data[2 * POLARSSL_ECP_MAX_BYTES];
    size_t grp_len = ( grp->nbits + 7 ) / 8;
    const md_info_t *md_info;
    mpi h;

    if( ( md_info = md_info_from_type( md_alg ) ) == NULL )
        return( POLARSSL_ERR_ECP_BAD_INPUT_DATA );

    mpi_init( &h );
    memset( &rng_ctx, 0, sizeof( hmac_drbg_context ) );

    /* Use private key and message hash (reduced) to initialize HMAC_DRBG */
    MPI_CHK( mpi_write_binary( d, data, grp_len ) );
    MPI_CHK( derive_mpi( grp, &h, buf, blen ) );
    MPI_CHK( mpi_write_binary( &h, data + grp_len, grp_len ) );
    hmac_drbg_init_buf( &rng_ctx, md_info, data, 2 * grp_len );

    ret = ecdsa_sign( grp, r, s, d, buf, blen,
                      hmac_drbg_random, &rng_ctx );

cleanup:
    hmac_drbg_free( &rng_ctx );
    mpi_free( &h );

    return( ret );
}
#endif /* POLARSSL_ECDSA_DETERMINISTIC */

/*
 * Verify ECDSA signature of hashed message (SEC1 4.1.4)
 * Obviously, compared to SEC1 4.1.3, we skip step 2 (hash message)
 */
int ecdsa_verify( ecp_group *grp,
                  const unsigned char *buf, size_t blen,
                  const ecp_point *Q, const mpi *r, const mpi *s)
{
    int ret;
    mpi e, s_inv, u1, u2;
    ecp_point R, P;

    ecp_point_init( &R ); ecp_point_init( &P );
    mpi_init( &e ); mpi_init( &s_inv ); mpi_init( &u1 ); mpi_init( &u2 );

    /* Fail cleanly on curves such as Curve25519 that can't be used for ECDSA */
    if( grp->N.p == NULL )
        return( POLARSSL_ERR_ECP_BAD_INPUT_DATA );

    /*
     * Step 1: make sure r and s are in range 1..n-1
     */
    if( mpi_cmp_int( r, 1 ) < 0 || mpi_cmp_mpi( r, &grp->N ) >= 0 ||
        mpi_cmp_int( s, 1 ) < 0 || mpi_cmp_mpi( s, &grp->N ) >= 0 )
    {
        ret = POLARSSL_ERR_ECP_VERIFY_FAILED;
        goto cleanup;
    }

    /*
     * Additional precaution: make sure Q is valid
     */
    MPI_CHK( ecp_check_pubkey( grp, Q ) );

    /*
     * Step 3: derive MPI from hashed message
     */
    MPI_CHK( derive_mpi( grp, &e, buf, blen ) );

    /*
     * Step 4: u1 = e / s mod n, u2 = r / s mod n
     */
    MPI_CHK( mpi_inv_mod( &s_inv, s, &grp->N ) );

    MPI_CHK( mpi_mul_mpi( &u1, &e, &s_inv ) );
    MPI_CHK( mpi_mod_mpi( &u1, &u1, &grp->N ) );

    MPI_CHK( mpi_mul_mpi( &u2, r, &s_inv ) );
    MPI_CHK( mpi_mod_mpi( &u2, &u2, &grp->N ) );

    /*
     * Step 5: R = u1 G + u2 Q
     *
     * Since we're not using any secret data, no need to pass a RNG to
     * ecp_mul() for countermesures.
     */
    MPI_CHK( ecp_mul( grp, &R, &u1, &grp->G, NULL, NULL ) );
    MPI_CHK( ecp_mul( grp, &P, &u2, Q, NULL, NULL ) );
    MPI_CHK( ecp_add( grp, &R, &R, &P ) );

    if( ecp_is_zero( &R ) )
    {
        ret = POLARSSL_ERR_ECP_VERIFY_FAILED;
        goto cleanup;
    }

    /*
     * Step 6: convert xR to an integer (no-op)
     * Step 7: reduce xR mod n (gives v)
     */
    MPI_CHK( mpi_mod_mpi( &R.X, &R.X, &grp->N ) );

    /*
     * Step 8: check if v (that is, R.X) is equal to r
     */
    if( mpi_cmp_mpi( &R.X, r ) != 0 )
    {
        ret = POLARSSL_ERR_ECP_VERIFY_FAILED;
        goto cleanup;
    }

cleanup:
    ecp_point_free( &R ); ecp_point_free( &P );
    mpi_free( &e ); mpi_free( &s_inv ); mpi_free( &u1 ); mpi_free( &u2 );

    return( ret );
}

/*
 * Convert a signature (given by context) to ASN.1
 */
static int ecdsa_signature_to_asn1( const mpi *r, const mpi *s,
                                    unsigned char *sig, size_t *slen )
{
    int ret;
    unsigned char buf[POLARSSL_ECDSA_MAX_LEN];
    unsigned char *p = buf + sizeof( buf );
    size_t len = 0;

    ASN1_CHK_ADD( len, asn1_write_mpi( &p, buf, s ) );
    ASN1_CHK_ADD( len, asn1_write_mpi( &p, buf, r ) );

    ASN1_CHK_ADD( len, asn1_write_len( &p, buf, len ) );
    ASN1_CHK_ADD( len, asn1_write_tag( &p, buf,
                                       ASN1_CONSTRUCTED | ASN1_SEQUENCE ) );

    memcpy( sig, p, len );
    *slen = len;

    return( 0 );
}

/*
 * Compute and write signature
 */
int ecdsa_write_signature( ecdsa_context *ctx, md_type_t md_alg,
                           const unsigned char *hash, size_t hlen,
                           unsigned char *sig, size_t *slen,
                           int (*f_rng)(void *, unsigned char *, size_t),
                           void *p_rng )
{
    int ret;
    mpi r, s;

    mpi_init( &r );
    mpi_init( &s );

#if defined(POLARSSL_ECDSA_DETERMINISTIC)
    (void) f_rng;
    (void) p_rng;

    MPI_CHK( ecdsa_sign_det( &ctx->grp, &r, &s, &ctx->d,
                             hash, hlen, md_alg ) );
#else
    (void) md_alg;

    MPI_CHK( ecdsa_sign( &ctx->grp, &r, &s, &ctx->d,
                         hash, hlen, f_rng, p_rng ) );
#endif

    MPI_CHK( ecdsa_signature_to_asn1( &r, &s, sig, slen ) );

cleanup:
    mpi_free( &r );
    mpi_free( &s );

    return( ret );
}

#if ! defined(POLARSSL_DEPRECATED_REMOVED)
int ecdsa_write_signature_det( ecdsa_context *ctx,
                               const unsigned char *hash, size_t hlen,
                               unsigned char *sig, size_t *slen,
                               md_type_t md_alg )
{
    return( ecdsa_write_signature( ctx, md_alg, hash, hlen, sig, slen,
                                   NULL, NULL ) );
}
#endif

/*
 * Read and check signature
 */
int ecdsa_read_signature( ecdsa_context *ctx,
                          const unsigned char *hash, size_t hlen,
                          const unsigned char *sig, size_t slen )
{
    int ret;
    unsigned char *p = (unsigned char *) sig;
    const unsigned char *end = sig + slen;
    size_t len;
    mpi r, s;

    mpi_init( &r );
    mpi_init( &s );

    if( ( ret = asn1_get_tag( &p, end, &len,
                    ASN1_CONSTRUCTED | ASN1_SEQUENCE ) ) != 0 )
    {
        ret += POLARSSL_ERR_ECP_BAD_INPUT_DATA;
        goto cleanup;
    }

    if( p + len != end )
    {
        ret = POLARSSL_ERR_ECP_BAD_INPUT_DATA +
              POLARSSL_ERR_ASN1_LENGTH_MISMATCH;
        goto cleanup;
    }

    if( ( ret = asn1_get_mpi( &p, end, &r ) ) != 0 ||
        ( ret = asn1_get_mpi( &p, end, &s ) ) != 0 )
    {
        ret += POLARSSL_ERR_ECP_BAD_INPUT_DATA;
        goto cleanup;
    }

    if( ( ret = ecdsa_verify( &ctx->grp, hash, hlen,
                              &ctx->Q, &r, &s ) ) != 0 )
        goto cleanup;

    if( p != end )
        ret = POLARSSL_ERR_ECP_SIG_LEN_MISMATCH;

cleanup:
    mpi_free( &r );
    mpi_free( &s );

    return( ret );
}

/*
 * Generate key pair
 */
int ecdsa_genkey( ecdsa_context *ctx, ecp_group_id gid,
                  int (*f_rng)(void *, unsigned char *, size_t), void *p_rng )
{
    return( ecp_use_known_dp( &ctx->grp, gid ) ||
            ecp_gen_keypair( &ctx->grp, &ctx->d, &ctx->Q, f_rng, p_rng ) );
}

/*
 * Set context from an ecp_keypair
 */
int ecdsa_from_keypair( ecdsa_context *ctx, const ecp_keypair *key )
{
    int ret;

    if( ( ret = ecp_group_copy( &ctx->grp, &key->grp ) ) != 0 ||
        ( ret = mpi_copy( &ctx->d, &key->d ) ) != 0 ||
        ( ret = ecp_copy( &ctx->Q, &key->Q ) ) != 0 )
    {
        ecdsa_free( ctx );
    }

    return( ret );
}

/*
 * Initialize context
 */
void ecdsa_init( ecdsa_context *ctx )
{
    ecp_keypair_init( ctx );
}

/*
 * Free context
 */
void ecdsa_free( ecdsa_context *ctx )
{
    ecp_keypair_free( ctx );
}

#endif /* POLARSSL_ECDSA_C */

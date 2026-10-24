/*
 * Rufus: The Reliable USB Formatting Utility
 * Message-Digest algorithms (md5sum, sha1sum, sha256sum, sha512sum)
 * Copyright © 1998-2001 Free Software Foundation, Inc.
 * Copyright © 2004-2019 Tom St Denis
 * Copyright © 2004 g10 Code GmbH
 * Copyright © 2002-2015 Wei Dai & Igor Pavlov
 * Copyright © 2015-2025 Pete Batard <pete@akeo.ie>
 * Copyright © 2022 Jeffrey Walton <noloader@gmail.com>
 * Copyright © 2016 Alexander Graf
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * SHA-1 code taken from GnuPG, as per copyrights above.
 *
 * SHA-256 taken from 7-zip's Sha256.c, itself based on Crypto++ - Public Domain
 *
 * SHA-512 modified from LibTomCrypt - Public Domain
 *
 * PE256 modified from u-boot's efi_image_loader.c - GPLv2.0+
 *
 * CPU accelerated SHA code taken from SHA-Intrinsics - Public Domain
 *
 * MD5 code from various public domain sources sharing the following
 * copyright declaration:
 *
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */

/* Shared algorithm body. The host supplies intrinsics, byte swaps, prefetch,
 * BOOL/TRUE/FALSE and the two acceleration flags. Keep the algorithms in the
 * original Rufus style; Windows and Linux compile this same implementation. */

#if (defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__i386) || \
     defined(_X86_) || defined(__I86__) || defined(__x86_64__))
#define CPU_X86_SHA1_ACCELERATION       1
#define CPU_X86_SHA256_ACCELERATION     1
#endif

#if defined(_MSC_VER)
#define RUFUS_ENABLE_GCC_ARCH(arch)
#else
#define RUFUS_ENABLE_GCC_ARCH(arch) __attribute__ ((target (arch)))
#endif

#undef BIG_ENDIAN_HOST

/*
 * Detect if the processor supports SHA-1 acceleration. We only check for
 * the three ISAs we need - SSSE3, SSE4.1 and SHA. We don't check for OS
 * support or XSAVE because that's been enabled since Windows 2000.
 */
BOOL DetectSHA1Acceleration(void)
{
#if defined(CPU_X86_SHA1_ACCELERATION)
#if defined(_MSC_VER)
	uint32_t regs0[4] = { 0,0,0,0 }, regs1[4] = { 0,0,0,0 }, regs7[4] = { 0,0,0,0 };
	const uint32_t SSSE3_BIT = 1u << 9; /* Function 1, Bit  9 of ECX */
	const uint32_t SSE41_BIT = 1u << 19; /* Function 1, Bit 19 of ECX */
	const uint32_t SHA_BIT = 1u << 29; /* Function 7, Bit 29 of EBX */

	__cpuid(regs0, 0);
	const uint32_t highest = regs0[0]; /*EAX*/

	if (highest >= 0x01) {
		__cpuidex(regs1, 1, 0);
	}
	if (highest >= 0x07) {
		__cpuidex(regs7, 7, 0);
	}

	return (regs1[2] /*ECX*/ & SSSE3_BIT) && (regs1[2] /*ECX*/ & SSE41_BIT) && (regs7[1] /*EBX*/ & SHA_BIT) ? TRUE : FALSE;
#elif defined(__GNUC__) || defined(__clang__)
	/* __builtin_cpu_supports available in GCC 4.8.1 and above */
	return __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("sha") ? TRUE : FALSE;
#else
	return FALSE;
#endif
#else
	return FALSE;
#endif
}

/*
 * Detect if the processor supports SHA-256 acceleration. We only check for
 * the three ISAs we need - SSSE3, SSE4.1 and SHA. We don't check for OS
 * support or XSAVE because that's been enabled since Windows 2000.
 */
BOOL DetectSHA256Acceleration(void)
{
#if defined(CPU_X86_SHA256_ACCELERATION)
#if defined(_MSC_VER)
	uint32_t regs0[4] = { 0,0,0,0 }, regs1[4] = { 0,0,0,0 }, regs7[4] = { 0,0,0,0 };
	const uint32_t SSSE3_BIT = 1u << 9; /* Function 1, Bit  9 of ECX */
	const uint32_t SSE41_BIT = 1u << 19; /* Function 1, Bit 19 of ECX */
	const uint32_t SHA_BIT = 1u << 29; /* Function 7, Bit 29 of EBX */

	__cpuid(regs0, 0);
	const uint32_t highest = regs0[0]; /*EAX*/

	if (highest >= 0x01) {
		__cpuidex(regs1, 1, 0);
	}
	if (highest >= 0x07) {
		__cpuidex(regs7, 7, 0);
	}

	return (regs1[2] /*ECX*/ & SSSE3_BIT) && (regs1[2] /*ECX*/ & SSE41_BIT) && (regs7[1] /*EBX*/ & SHA_BIT) ? TRUE : FALSE;
#elif defined(__GNUC__) || defined(__clang__)
	/* __builtin_cpu_supports available in GCC 4.8.1 and above */
	return __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("sha") ? TRUE : FALSE;
#else
	return FALSE;
#endif
#else
	return FALSE;
#endif
}

/*
 * Rotate 32 or 64 bit integers by n bytes.
 * Don't bother trying to hand-optimize those, as the
 * compiler usually does a pretty good job at that.
 */
#define ROL32(a,b) (((a) << (b)) | ((a) >> (32-(b))))
#define ROR32(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define ROL64(a,b) (((a) << (b)) | ((a) >> (64-(b))))
#define ROR64(a,b) (((a) >> (b)) | ((a) << (64-(b))))

/*
 * SHA-256, SHA-512 common macros (use Wikipedia SHA-2 names for clarity)
 */
#define Ch(x,y,z) ((z) ^ ((x) & ((y) ^ (z))))
#define Ma(x,y,z) (((x) & (y)) | ((z) & ((x) | (y))))

/* SHA-256 constants */
static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* SHA-512 constants */
static const uint64_t K512[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void md5_init(HASH_CONTEXT *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
}

static void sha1_init(HASH_CONTEXT *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xc3d2e1f0;
}

static void sha256_init(HASH_CONTEXT *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
}

static void sha512_init(HASH_CONTEXT* ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state[0] = 0x6a09e667f3bcc908ULL;
	ctx->state[1] = 0xbb67ae8584caa73bULL;
	ctx->state[2] = 0x3c6ef372fe94f82bULL;
	ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
	ctx->state[4] = 0x510e527fade682d1ULL;
	ctx->state[5] = 0x9b05688c2b3e6c1fULL;
	ctx->state[6] = 0x1f83d9abfb41bd6bULL;
	ctx->state[7] = 0x5be0cd19137e2179ULL;
}

/* Transform the message X which consists of 16 32-bit-words (SHA-1) */
static void sha1_transform_cc(HASH_CONTEXT *ctx, const uint8_t *data)
{
	uint32_t a, b, c, d, e, tm, x[16];

	/* get values from the chaining vars */
	a = (uint32_t)ctx->state[0];
	b = (uint32_t)ctx->state[1];
	c = (uint32_t)ctx->state[2];
	d = (uint32_t)ctx->state[3];
	e = (uint32_t)ctx->state[4];

#ifdef BIG_ENDIAN_HOST
	memcpy(x, data, sizeof(x));
#else
	{
		unsigned k;
		for (k = 0; k < 16; k += 4) {
			const uint8_t *p2 = data + k * 4;
			x[k] = read_swap32(p2);
			x[k + 1] = read_swap32(p2 + 4);
			x[k + 2] = read_swap32(p2 + 8);
			x[k + 3] = read_swap32(p2 + 12);
		}
	}
#endif

#define K1  0x5a827999L
#define K2  0x6ed9eba1L
#define K3  0x8f1bbcdcL
#define K4  0xca62c1d6L
#define F1(x,y,z)   ( z ^ ( x & ( y ^ z ) ) )
#define F2(x,y,z)   ( x ^ y ^ z )
#define F3(x,y,z)   ( ( x & y ) | ( z & ( x | y ) ) )
#define F4(x,y,z)   ( x ^ y ^ z )

#define M(i) ( tm = x[i&0x0f] ^ x[(i-14)&0x0f] ^ x[(i-8)&0x0f] ^ x[(i-3)&0x0f], (x[i&0x0f] = ROL32(tm,1)) )

#define SHA1STEP(a, b, c, d, e, f, k, m) do { e += ROL32(a, 5) + f(b, c, d) + k + m; \
                                              b = ROL32(b, 30); } while(0)
	SHA1STEP(a, b, c, d, e, F1, K1, x[0]);
	SHA1STEP(e, a, b, c, d, F1, K1, x[1]);
	SHA1STEP(d, e, a, b, c, F1, K1, x[2]);
	SHA1STEP(c, d, e, a, b, F1, K1, x[3]);
	SHA1STEP(b, c, d, e, a, F1, K1, x[4]);
	SHA1STEP(a, b, c, d, e, F1, K1, x[5]);
	SHA1STEP(e, a, b, c, d, F1, K1, x[6]);
	SHA1STEP(d, e, a, b, c, F1, K1, x[7]);
	SHA1STEP(c, d, e, a, b, F1, K1, x[8]);
	SHA1STEP(b, c, d, e, a, F1, K1, x[9]);
	SHA1STEP(a, b, c, d, e, F1, K1, x[10]);
	SHA1STEP(e, a, b, c, d, F1, K1, x[11]);
	SHA1STEP(d, e, a, b, c, F1, K1, x[12]);
	SHA1STEP(c, d, e, a, b, F1, K1, x[13]);
	SHA1STEP(b, c, d, e, a, F1, K1, x[14]);
	SHA1STEP(a, b, c, d, e, F1, K1, x[15]);
	SHA1STEP(e, a, b, c, d, F1, K1, M(16));
	SHA1STEP(d, e, a, b, c, F1, K1, M(17));
	SHA1STEP(c, d, e, a, b, F1, K1, M(18));
	SHA1STEP(b, c, d, e, a, F1, K1, M(19));
	SHA1STEP(a, b, c, d, e, F2, K2, M(20));
	SHA1STEP(e, a, b, c, d, F2, K2, M(21));
	SHA1STEP(d, e, a, b, c, F2, K2, M(22));
	SHA1STEP(c, d, e, a, b, F2, K2, M(23));
	SHA1STEP(b, c, d, e, a, F2, K2, M(24));
	SHA1STEP(a, b, c, d, e, F2, K2, M(25));
	SHA1STEP(e, a, b, c, d, F2, K2, M(26));
	SHA1STEP(d, e, a, b, c, F2, K2, M(27));
	SHA1STEP(c, d, e, a, b, F2, K2, M(28));
	SHA1STEP(b, c, d, e, a, F2, K2, M(29));
	SHA1STEP(a, b, c, d, e, F2, K2, M(30));
	SHA1STEP(e, a, b, c, d, F2, K2, M(31));
	SHA1STEP(d, e, a, b, c, F2, K2, M(32));
	SHA1STEP(c, d, e, a, b, F2, K2, M(33));
	SHA1STEP(b, c, d, e, a, F2, K2, M(34));
	SHA1STEP(a, b, c, d, e, F2, K2, M(35));
	SHA1STEP(e, a, b, c, d, F2, K2, M(36));
	SHA1STEP(d, e, a, b, c, F2, K2, M(37));
	SHA1STEP(c, d, e, a, b, F2, K2, M(38));
	SHA1STEP(b, c, d, e, a, F2, K2, M(39));
	SHA1STEP(a, b, c, d, e, F3, K3, M(40));
	SHA1STEP(e, a, b, c, d, F3, K3, M(41));
	SHA1STEP(d, e, a, b, c, F3, K3, M(42));
	SHA1STEP(c, d, e, a, b, F3, K3, M(43));
	SHA1STEP(b, c, d, e, a, F3, K3, M(44));
	SHA1STEP(a, b, c, d, e, F3, K3, M(45));
	SHA1STEP(e, a, b, c, d, F3, K3, M(46));
	SHA1STEP(d, e, a, b, c, F3, K3, M(47));
	SHA1STEP(c, d, e, a, b, F3, K3, M(48));
	SHA1STEP(b, c, d, e, a, F3, K3, M(49));
	SHA1STEP(a, b, c, d, e, F3, K3, M(50));
	SHA1STEP(e, a, b, c, d, F3, K3, M(51));
	SHA1STEP(d, e, a, b, c, F3, K3, M(52));
	SHA1STEP(c, d, e, a, b, F3, K3, M(53));
	SHA1STEP(b, c, d, e, a, F3, K3, M(54));
	SHA1STEP(a, b, c, d, e, F3, K3, M(55));
	SHA1STEP(e, a, b, c, d, F3, K3, M(56));
	SHA1STEP(d, e, a, b, c, F3, K3, M(57));
	SHA1STEP(c, d, e, a, b, F3, K3, M(58));
	SHA1STEP(b, c, d, e, a, F3, K3, M(59));
	SHA1STEP(a, b, c, d, e, F4, K4, M(60));
	SHA1STEP(e, a, b, c, d, F4, K4, M(61));
	SHA1STEP(d, e, a, b, c, F4, K4, M(62));
	SHA1STEP(c, d, e, a, b, F4, K4, M(63));
	SHA1STEP(b, c, d, e, a, F4, K4, M(64));
	SHA1STEP(a, b, c, d, e, F4, K4, M(65));
	SHA1STEP(e, a, b, c, d, F4, K4, M(66));
	SHA1STEP(d, e, a, b, c, F4, K4, M(67));
	SHA1STEP(c, d, e, a, b, F4, K4, M(68));
	SHA1STEP(b, c, d, e, a, F4, K4, M(69));
	SHA1STEP(a, b, c, d, e, F4, K4, M(70));
	SHA1STEP(e, a, b, c, d, F4, K4, M(71));
	SHA1STEP(d, e, a, b, c, F4, K4, M(72));
	SHA1STEP(c, d, e, a, b, F4, K4, M(73));
	SHA1STEP(b, c, d, e, a, F4, K4, M(74));
	SHA1STEP(a, b, c, d, e, F4, K4, M(75));
	SHA1STEP(e, a, b, c, d, F4, K4, M(76));
	SHA1STEP(d, e, a, b, c, F4, K4, M(77));
	SHA1STEP(c, d, e, a, b, F4, K4, M(78));
	SHA1STEP(b, c, d, e, a, F4, K4, M(79));

#undef F1
#undef F2
#undef F3
#undef F4

	/* Update chaining vars */
	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
}

#ifdef CPU_X86_SHA1_ACCELERATION
/*
 * Transform the message X which consists of 16 32-bit-words (SHA-1)
 * The code is public domain taken from https://github.com/noloader/SHA-Intrinsics.
 */
RUFUS_ENABLE_GCC_ARCH("ssse3,sse4.1,sha")
static void sha1_transform_x86(uint64_t state64[5], const uint8_t *data, size_t length)
{
	__m128i ABCD, E0, E1;
	__m128i MSG0, MSG1, MSG2, MSG3;
	const __m128i MASK = _mm_set_epi64x(0x0001020304050607ULL, 0x08090a0b0c0d0e0fULL);

	/* Rufus uses uint64_t for the state array. Pack it into uint32_t. */
	uint32_t state[5] = {
		(uint32_t)state64[0],
		(uint32_t)state64[1],
		(uint32_t)state64[2],
		(uint32_t)state64[3],
		(uint32_t)state64[4]
	};

	/* Load initial values */
	ABCD = _mm_loadu_si128((const __m128i*) state);
	E0 = _mm_set_epi32(state[4], 0, 0, 0);
	ABCD = _mm_shuffle_epi32(ABCD, 0x1B);

	while (length >= SHA1_BLOCKSIZE)
	{
		/* Save current state  */
		const __m128i ABCD_SAVE = ABCD;
		const __m128i E0_SAVE = E0;

		/* Rounds 0-3 */
		MSG0 = _mm_loadu_si128((const __m128i*)(data + 0));
		MSG0 = _mm_shuffle_epi8(MSG0, MASK);
		E0 = _mm_add_epi32(E0, MSG0);
		E1 = ABCD;
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 0);

		/* Rounds 4-7 */
		MSG1 = _mm_loadu_si128((const __m128i*)(data + 16));
		MSG1 = _mm_shuffle_epi8(MSG1, MASK);
		E1 = _mm_sha1nexte_epu32(E1, MSG1);
		E0 = ABCD;
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 0);
		MSG0 = _mm_sha1msg1_epu32(MSG0, MSG1);

		/* Rounds 8-11 */
		MSG2 = _mm_loadu_si128((const __m128i*)(data + 32));
		MSG2 = _mm_shuffle_epi8(MSG2, MASK);
		E0 = _mm_sha1nexte_epu32(E0, MSG2);
		E1 = ABCD;
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 0);
		MSG1 = _mm_sha1msg1_epu32(MSG1, MSG2);
		MSG0 = _mm_xor_si128(MSG0, MSG2);

		/* Rounds 12-15 */
		MSG3 = _mm_loadu_si128((const __m128i*)(data + 48));
		MSG3 = _mm_shuffle_epi8(MSG3, MASK);
		E1 = _mm_sha1nexte_epu32(E1, MSG3);
		E0 = ABCD;
		MSG0 = _mm_sha1msg2_epu32(MSG0, MSG3);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 0);
		MSG2 = _mm_sha1msg1_epu32(MSG2, MSG3);
		MSG1 = _mm_xor_si128(MSG1, MSG3);

		/* Rounds 16-19 */
		E0 = _mm_sha1nexte_epu32(E0, MSG0);
		E1 = ABCD;
		MSG1 = _mm_sha1msg2_epu32(MSG1, MSG0);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 0);
		MSG3 = _mm_sha1msg1_epu32(MSG3, MSG0);
		MSG2 = _mm_xor_si128(MSG2, MSG0);

		/* Rounds 20-23 */
		E1 = _mm_sha1nexte_epu32(E1, MSG1);
		E0 = ABCD;
		MSG2 = _mm_sha1msg2_epu32(MSG2, MSG1);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 1);
		MSG0 = _mm_sha1msg1_epu32(MSG0, MSG1);
		MSG3 = _mm_xor_si128(MSG3, MSG1);

		/* Rounds 24-27 */
		E0 = _mm_sha1nexte_epu32(E0, MSG2);
		E1 = ABCD;
		MSG3 = _mm_sha1msg2_epu32(MSG3, MSG2);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 1);
		MSG1 = _mm_sha1msg1_epu32(MSG1, MSG2);
		MSG0 = _mm_xor_si128(MSG0, MSG2);

		/* Rounds 28-31 */
		E1 = _mm_sha1nexte_epu32(E1, MSG3);
		E0 = ABCD;
		MSG0 = _mm_sha1msg2_epu32(MSG0, MSG3);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 1);
		MSG2 = _mm_sha1msg1_epu32(MSG2, MSG3);
		MSG1 = _mm_xor_si128(MSG1, MSG3);

		/* Rounds 32-35 */
		E0 = _mm_sha1nexte_epu32(E0, MSG0);
		E1 = ABCD;
		MSG1 = _mm_sha1msg2_epu32(MSG1, MSG0);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 1);
		MSG3 = _mm_sha1msg1_epu32(MSG3, MSG0);
		MSG2 = _mm_xor_si128(MSG2, MSG0);

		/* Rounds 36-39 */
		E1 = _mm_sha1nexte_epu32(E1, MSG1);
		E0 = ABCD;
		MSG2 = _mm_sha1msg2_epu32(MSG2, MSG1);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 1);
		MSG0 = _mm_sha1msg1_epu32(MSG0, MSG1);
		MSG3 = _mm_xor_si128(MSG3, MSG1);

		/* Rounds 40-43 */
		E0 = _mm_sha1nexte_epu32(E0, MSG2);
		E1 = ABCD;
		MSG3 = _mm_sha1msg2_epu32(MSG3, MSG2);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 2);
		MSG1 = _mm_sha1msg1_epu32(MSG1, MSG2);
		MSG0 = _mm_xor_si128(MSG0, MSG2);

		/* Rounds 44-47 */
		E1 = _mm_sha1nexte_epu32(E1, MSG3);
		E0 = ABCD;
		MSG0 = _mm_sha1msg2_epu32(MSG0, MSG3);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 2);
		MSG2 = _mm_sha1msg1_epu32(MSG2, MSG3);
		MSG1 = _mm_xor_si128(MSG1, MSG3);

		/* Rounds 48-51 */
		E0 = _mm_sha1nexte_epu32(E0, MSG0);
		E1 = ABCD;
		MSG1 = _mm_sha1msg2_epu32(MSG1, MSG0);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 2);
		MSG3 = _mm_sha1msg1_epu32(MSG3, MSG0);
		MSG2 = _mm_xor_si128(MSG2, MSG0);

		/* Rounds 52-55 */
		E1 = _mm_sha1nexte_epu32(E1, MSG1);
		E0 = ABCD;
		MSG2 = _mm_sha1msg2_epu32(MSG2, MSG1);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 2);
		MSG0 = _mm_sha1msg1_epu32(MSG0, MSG1);
		MSG3 = _mm_xor_si128(MSG3, MSG1);

		/* Rounds 56-59 */
		E0 = _mm_sha1nexte_epu32(E0, MSG2);
		E1 = ABCD;
		MSG3 = _mm_sha1msg2_epu32(MSG3, MSG2);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 2);
		MSG1 = _mm_sha1msg1_epu32(MSG1, MSG2);
		MSG0 = _mm_xor_si128(MSG0, MSG2);

		/* Rounds 60-63 */
		E1 = _mm_sha1nexte_epu32(E1, MSG3);
		E0 = ABCD;
		MSG0 = _mm_sha1msg2_epu32(MSG0, MSG3);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 3);
		MSG2 = _mm_sha1msg1_epu32(MSG2, MSG3);
		MSG1 = _mm_xor_si128(MSG1, MSG3);

		/* Rounds 64-67 */
		E0 = _mm_sha1nexte_epu32(E0, MSG0);
		E1 = ABCD;
		MSG1 = _mm_sha1msg2_epu32(MSG1, MSG0);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 3);
		MSG3 = _mm_sha1msg1_epu32(MSG3, MSG0);
		MSG2 = _mm_xor_si128(MSG2, MSG0);

		/* Rounds 68-71 */
		E1 = _mm_sha1nexte_epu32(E1, MSG1);
		E0 = ABCD;
		MSG2 = _mm_sha1msg2_epu32(MSG2, MSG1);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 3);
		MSG3 = _mm_xor_si128(MSG3, MSG1);

		/* Rounds 72-75 */
		E0 = _mm_sha1nexte_epu32(E0, MSG2);
		E1 = ABCD;
		MSG3 = _mm_sha1msg2_epu32(MSG3, MSG2);
		ABCD = _mm_sha1rnds4_epu32(ABCD, E0, 3);

		/* Rounds 76-79 */
		E1 = _mm_sha1nexte_epu32(E1, MSG3);
		E0 = ABCD;
		ABCD = _mm_sha1rnds4_epu32(ABCD, E1, 3);

		/* Combine state */
		E0 = _mm_sha1nexte_epu32(E0, E0_SAVE);
		ABCD = _mm_add_epi32(ABCD, ABCD_SAVE);

		data += 64;
		length -= 64;
	}

	/* Save state */
	ABCD = _mm_shuffle_epi32(ABCD, 0x1B);
	_mm_storeu_si128((__m128i*) state, ABCD);
	state[4] = _mm_extract_epi32(E0, 3);

	/* Repack into uint64_t. */
	state64[0] = state[0];
	state64[1] = state[1];
	state64[2] = state[2];
	state64[3] = state[3];
	state64[4] = state[4];
}
#endif /* CPU_X86_SHA1_ACCELERATION */

/* Transform the message X which consists of 16 32-bit-words (SHA-1) */
static void sha1_transform(HASH_CONTEXT *ctx, const uint8_t *data)
{
#ifdef CPU_X86_SHA1_ACCELERATION
	if (cpu_has_sha1_accel)
	{
		/* SHA-1 acceleration using intrinsics */
		sha1_transform_x86(ctx->state, data, SHA1_BLOCKSIZE);
	}
	else
#endif
	{
		/* Portable C/C++ implementation */
		sha1_transform_cc(ctx, data);
	}
}

/* Transform the message X which consists of 16 32-bit-words (SHA-256) */
static __inline void sha256_transform_cc(HASH_CONTEXT *ctx, const uint8_t *data)
{
	uint32_t a, b, c, d, e, f, g, h, j, x[16];

	a = (uint32_t)ctx->state[0];
	b = (uint32_t)ctx->state[1];
	c = (uint32_t)ctx->state[2];
	d = (uint32_t)ctx->state[3];
	e = (uint32_t)ctx->state[4];
	f = (uint32_t)ctx->state[5];
	g = (uint32_t)ctx->state[6];
	h = (uint32_t)ctx->state[7];

// Nesting the ROR allows for single register compiler optimizations
#define S0(x) (ROR32(ROR32(ROR32(x,9)^(x),11)^(x),2))	// Σ0 (Sigma 0)
#define S1(x) (ROR32(ROR32(ROR32(x,14)^(x),5)^(x),6))	// Σ1 (Sigma 1)
#define s0(x) (ROR32(ROR32(x,11)^(x),7)^((x)>>3))		// σ0 (sigma 0)
#define s1(x) (ROR32(ROR32(x,2)^(x),17)^((x)>>10))		// σ1 (sigma 1)
#define BLK0(i) (x[i])
#define BLK2(i) (x[i] += s1(x[((i)-2)&15]) + x[((i)-7)&15] + s0(x[((i)-15)&15]))
#define R(a, b, c, d, e, f, g, h, i) \
	h += S1(e) + Ch(e,f,g) + K256[(i)+(j)] + (j ? BLK2(i) : BLK0(i)); \
	d += h; \
	h += S0(a) + Ma(a, b, c)
#define RX_8(i) \
	R(a, b, c, d, e, f, g, h, i);   \
	R(h, a, b, c, d, e, f, g, i+1); \
	R(g, h, a, b, c, d, e, f, i+2); \
	R(f, g, h, a, b, c, d, e, i+3); \
	R(e, f, g, h, a, b, c, d, i+4); \
	R(d, e, f, g, h, a, b, c, i+5); \
	R(c, d, e, f, g, h, a, b, i+6); \
	R(b, c, d, e, f, g, h, a, i+7)

#ifdef BIG_ENDIAN_HOST
	memcpy(x, data, sizeof(x));
#else
	{
		uint32_t k;
		for (k = 0; k < 16; k += 4) {
			const uint8_t* p2 = data + k * 4;
			x[k] = read_swap32(p2);
			x[k + 1] = read_swap32(p2 + 4);
			x[k + 2] = read_swap32(p2 + 8);
			x[k + 3] = read_swap32(p2 + 12);
		}
	}
#endif

	for (j = 0; j < 64; j += 16) {
		RX_8(0);
		RX_8(8);
	}

#undef S0
#undef S1
#undef s0
#undef s1
#undef BLK0
#undef BLK2
#undef R
#undef RX_8

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

#ifdef CPU_X86_SHA256_ACCELERATION
/*
 * Transform the message X which consists of 16 32-bit-words (SHA-256)
 * The code is public domain taken from https://github.com/noloader/SHA-Intrinsics.
 */
RUFUS_ENABLE_GCC_ARCH("ssse3,sse4.1,sha")
static __inline void sha256_transform_x86(uint64_t state64[8], const uint8_t *data, size_t length)
{
	__m128i STATE0, STATE1;
	__m128i MSG, TMP;
	__m128i MSG0, MSG1, MSG2, MSG3;
	const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

	/* Rufus uses uint64_t for the state array. Pack it into uint32_t. */
	uint32_t state[8] = {
		(uint32_t)state64[0],
		(uint32_t)state64[1],
		(uint32_t)state64[2],
		(uint32_t)state64[3],
		(uint32_t)state64[4],
		(uint32_t)state64[5],
		(uint32_t)state64[6],
		(uint32_t)state64[7]
	};

	/* Load initial values */
	TMP = _mm_loadu_si128((const __m128i*) (state+0));
	STATE1 = _mm_loadu_si128((const __m128i*) (state+4));

	TMP = _mm_shuffle_epi32(TMP, 0xB1);          /* CDAB */
	STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);    /* EFGH */
	STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);    /* ABEF */
	STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0); /* CDGH */

	while (length >= SHA256_BLOCKSIZE)
	{
		/* Save current state */
		const __m128i ABEF_SAVE = STATE0;
		const __m128i CDGH_SAVE = STATE1;

		/* Rounds 0-3 */
		MSG = _mm_loadu_si128((const __m128i*) (data+0));
		MSG0 = _mm_shuffle_epi8(MSG, MASK);
		MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0xE9B5DBA5B5C0FBCFULL, 0x71374491428A2F98ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

		/* Rounds 4-7 */
		MSG1 = _mm_loadu_si128((const __m128i*) (data+16));
		MSG1 = _mm_shuffle_epi8(MSG1, MASK);
		MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0xAB1C5ED5923F82A4ULL, 0x59F111F13956C25BULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

		/* Rounds 8-11 */
		MSG2 = _mm_loadu_si128((const __m128i*) (data+32));
		MSG2 = _mm_shuffle_epi8(MSG2, MASK);
		MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0x550C7DC3243185BEULL, 0x12835B01D807AA98ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

		/* Rounds 12-15 */
		MSG3 = _mm_loadu_si128((const __m128i*) (data+48));
		MSG3 = _mm_shuffle_epi8(MSG3, MASK);
		MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0xC19BF1749BDC06A7ULL, 0x80DEB1FE72BE5D74ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
		MSG0 = _mm_add_epi32(MSG0, TMP);
		MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

		/* Rounds 16-19 */
		MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0x240CA1CC0FC19DC6ULL, 0xEFBE4786E49B69C1ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
		MSG1 = _mm_add_epi32(MSG1, TMP);
		MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

		/* Rounds 20-23 */
		MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0x76F988DA5CB0A9DCULL, 0x4A7484AA2DE92C6FULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
		MSG2 = _mm_add_epi32(MSG2, TMP);
		MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

		/* Rounds 24-27 */
		MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0xBF597FC7B00327C8ULL, 0xA831C66D983E5152ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
		MSG3 = _mm_add_epi32(MSG3, TMP);
		MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

		/* Rounds 28-31 */
		MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0x1429296706CA6351ULL,  0xD5A79147C6E00BF3ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
		MSG0 = _mm_add_epi32(MSG0, TMP);
		MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

		/* Rounds 32-35 */
		MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0x53380D134D2C6DFCULL, 0x2E1B213827B70A85ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
		MSG1 = _mm_add_epi32(MSG1, TMP);
		MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

		/* Rounds 36-39 */
		MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0x92722C8581C2C92EULL, 0x766A0ABB650A7354ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
		MSG2 = _mm_add_epi32(MSG2, TMP);
		MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

		/* Rounds 40-43 */
		MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0xC76C51A3C24B8B70ULL, 0xA81A664BA2BFE8A1ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
		MSG3 = _mm_add_epi32(MSG3, TMP);
		MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

		/* Rounds 44-47 */
		MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0x106AA070F40E3585ULL, 0xD6990624D192E819ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
		MSG0 = _mm_add_epi32(MSG0, TMP);
		MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

		/* Rounds 48-51 */
		MSG = _mm_add_epi32(MSG0, _mm_set_epi64x(0x34B0BCB52748774CULL, 0x1E376C0819A4C116ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
		MSG1 = _mm_add_epi32(MSG1, TMP);
		MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
		MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

		/* Rounds 52-55 */
		MSG = _mm_add_epi32(MSG1, _mm_set_epi64x(0x682E6FF35B9CCA4FULL, 0x4ED8AA4A391C0CB3ULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
		MSG2 = _mm_add_epi32(MSG2, TMP);
		MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

		/* Rounds 56-59 */
		MSG = _mm_add_epi32(MSG2, _mm_set_epi64x(0x8CC7020884C87814ULL, 0x78A5636F748F82EEULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
		MSG3 = _mm_add_epi32(MSG3, TMP);
		MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

		/* Rounds 60-63 */
		MSG = _mm_add_epi32(MSG3, _mm_set_epi64x(0xC67178F2BEF9A3F7ULL, 0xA4506CEB90BEFFFAULL));
		STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
		MSG = _mm_shuffle_epi32(MSG, 0x0E);
		STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

		/* Combine state  */
		STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
		STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);

		data += 64;
		length -= 64;
	}

	TMP = _mm_shuffle_epi32(STATE0, 0x1B);       /* FEBA */
	STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);    /* DCHG */
	STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0); /* DCBA */
	STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);    /* ABEF */

	/* Save state */
	_mm_storeu_si128((__m128i*) (state+0), STATE0);
	_mm_storeu_si128((__m128i*) (state+4), STATE1);

	/* Repack into uint64_t. */
	state64[0] = state[0];
	state64[1] = state[1];
	state64[2] = state[2];
	state64[3] = state[3];
	state64[4] = state[4];
	state64[5] = state[5];
	state64[6] = state[6];
	state64[7] = state[7];
}
#endif /* CPU_X86_SHA256_ACCELERATION */

static __inline void sha256_transform(HASH_CONTEXT *ctx, const uint8_t *data)
{
#ifdef CPU_X86_SHA256_ACCELERATION
	if (cpu_has_sha256_accel)
	{
		/* SHA-256 acceleration using intrinsics */
		sha256_transform_x86(ctx->state, data, SHA256_BLOCKSIZE);
	}
	else
#endif
	{
		/* Portable C/C++ implementation */
		sha256_transform_cc(ctx, data);
	}
}

/*
 * Transform the message X which consists of 16 64-bit-words (SHA-512)
 * This is an algorithm that *REALLY* benefits from being executed as 64-bit
 * code rather than 32-bit, as it's more than twice as fast then...
 */
static __inline void sha512_transform(HASH_CONTEXT* ctx, const uint8_t* data)
{
	uint64_t a, b, c, d, e, f, g, h, W[80];
	uint32_t i;

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

// Nesting the ROR allows for single register compiler optimizations
#define S0(x) (ROR64(ROR64(ROR64(x,5)^(x),6)^(x),28))	// Σ0 (Sigma 0)
#define S1(x) (ROR64(ROR64(ROR64(x,23)^(x),4)^(x),14))	// Σ1 (Sigma 1)
#define s0(x) (ROR64(ROR64(x,7)^(x),1)^((x)>>7))		// σ0 (sigma 0)
#define s1(x) (ROR64(ROR64(x,42)^(x),19)^((x)>>6))		// σ1 (sigma 1)
#define R(a, b, c, d, e, f, g, h, i) \
	h += S1(e) + Ch(e, f, g) + K512[i] + W[i]; \
	d += h; \
	h += S0(a) + Ma(a, b, c)

	for (i = 0; i < 80; i++) {
		if (i < 16)
#ifdef BIG_ENDIAN_HOST
			W[i] = *((uint64_t*)&data[8 * i]));
#else
			W[i] = read_swap64(&data[8 * i]);
#endif
		else
			W[i] = s1(W[i - 2]) + W[i - 7] + s0(W[i - 15]) + W[i - 16];
	}

	for (i = 0; i < 80; i += 8) {
		R(a, b, c, d, e, f, g, h, i);
		R(h, a, b, c, d, e, f, g, i+1);
		R(g, h, a, b, c, d, e, f, i+2);
		R(f, g, h, a, b, c, d, e, i+3);
		R(e, f, g, h, a, b, c, d, i+4);
		R(d, e, f, g, h, a, b, c, i+5);
		R(c, d, e, f, g, h, a, b, i+6);
		R(b, c, d, e, f, g, h, a, i+7);
	}

#undef S0
#undef S1
#undef s0
#undef s1
#undef R

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

/* Transform the message X which consists of 16 32-bit-words (MD5) */
static void md5_transform(HASH_CONTEXT *ctx, const uint8_t *data)
{
	uint32_t a, b, c, d, x[16];

	a = (uint32_t)ctx->state[0];
	b = (uint32_t)ctx->state[1];
	c = (uint32_t)ctx->state[2];
	d = (uint32_t)ctx->state[3];

#ifdef BIG_ENDIAN_HOST
	{
		uint32_t k;
		for (k = 0; k < 16; k += 4) {
			const uint8_t *p2 = data + k * 4;
			x[k] = read_swap32(p2);
			x[k + 1] = read_swap32(p2 + 4);
			x[k + 2] = read_swap32(p2 + 8);
			x[k + 3] = read_swap32(p2 + 12);
		}
	}
#else
	memcpy(x, data, sizeof(x));
#endif

#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

#define MD5STEP(f, w, x, y, z, data, s) do { \
	( w += f(x, y, z) + data,  w = w<<s | w>>(32-s),  w += x ); } while(0)

	MD5STEP(F1, a, b, c, d, x[0] + 0xd76aa478, 7);
	MD5STEP(F1, d, a, b, c, x[1] + 0xe8c7b756, 12);
	MD5STEP(F1, c, d, a, b, x[2] + 0x242070db, 17);
	MD5STEP(F1, b, c, d, a, x[3] + 0xc1bdceee, 22);
	MD5STEP(F1, a, b, c, d, x[4] + 0xf57c0faf, 7);
	MD5STEP(F1, d, a, b, c, x[5] + 0x4787c62a, 12);
	MD5STEP(F1, c, d, a, b, x[6] + 0xa8304613, 17);
	MD5STEP(F1, b, c, d, a, x[7] + 0xfd469501, 22);
	MD5STEP(F1, a, b, c, d, x[8] + 0x698098d8, 7);
	MD5STEP(F1, d, a, b, c, x[9] + 0x8b44f7af, 12);
	MD5STEP(F1, c, d, a, b, x[10] + 0xffff5bb1, 17);
	MD5STEP(F1, b, c, d, a, x[11] + 0x895cd7be, 22);
	MD5STEP(F1, a, b, c, d, x[12] + 0x6b901122, 7);
	MD5STEP(F1, d, a, b, c, x[13] + 0xfd987193, 12);
	MD5STEP(F1, c, d, a, b, x[14] + 0xa679438e, 17);
	MD5STEP(F1, b, c, d, a, x[15] + 0x49b40821, 22);

	MD5STEP(F2, a, b, c, d, x[1] + 0xf61e2562, 5);
	MD5STEP(F2, d, a, b, c, x[6] + 0xc040b340, 9);
	MD5STEP(F2, c, d, a, b, x[11] + 0x265e5a51, 14);
	MD5STEP(F2, b, c, d, a, x[0] + 0xe9b6c7aa, 20);
	MD5STEP(F2, a, b, c, d, x[5] + 0xd62f105d, 5);
	MD5STEP(F2, d, a, b, c, x[10] + 0x02441453, 9);
	MD5STEP(F2, c, d, a, b, x[15] + 0xd8a1e681, 14);
	MD5STEP(F2, b, c, d, a, x[4] + 0xe7d3fbc8, 20);
	MD5STEP(F2, a, b, c, d, x[9] + 0x21e1cde6, 5);
	MD5STEP(F2, d, a, b, c, x[14] + 0xc33707d6, 9);
	MD5STEP(F2, c, d, a, b, x[3] + 0xf4d50d87, 14);
	MD5STEP(F2, b, c, d, a, x[8] + 0x455a14ed, 20);
	MD5STEP(F2, a, b, c, d, x[13] + 0xa9e3e905, 5);
	MD5STEP(F2, d, a, b, c, x[2] + 0xfcefa3f8, 9);
	MD5STEP(F2, c, d, a, b, x[7] + 0x676f02d9, 14);
	MD5STEP(F2, b, c, d, a, x[12] + 0x8d2a4c8a, 20);

	MD5STEP(F3, a, b, c, d, x[5] + 0xfffa3942, 4);
	MD5STEP(F3, d, a, b, c, x[8] + 0x8771f681, 11);
	MD5STEP(F3, c, d, a, b, x[11] + 0x6d9d6122, 16);
	MD5STEP(F3, b, c, d, a, x[14] + 0xfde5380c, 23);
	MD5STEP(F3, a, b, c, d, x[1] + 0xa4beea44, 4);
	MD5STEP(F3, d, a, b, c, x[4] + 0x4bdecfa9, 11);
	MD5STEP(F3, c, d, a, b, x[7] + 0xf6bb4b60, 16);
	MD5STEP(F3, b, c, d, a, x[10] + 0xbebfbc70, 23);
	MD5STEP(F3, a, b, c, d, x[13] + 0x289b7ec6, 4);
	MD5STEP(F3, d, a, b, c, x[0] + 0xeaa127fa, 11);
	MD5STEP(F3, c, d, a, b, x[3] + 0xd4ef3085, 16);
	MD5STEP(F3, b, c, d, a, x[6] + 0x04881d05, 23);
	MD5STEP(F3, a, b, c, d, x[9] + 0xd9d4d039, 4);
	MD5STEP(F3, d, a, b, c, x[12] + 0xe6db99e5, 11);
	MD5STEP(F3, c, d, a, b, x[15] + 0x1fa27cf8, 16);
	MD5STEP(F3, b, c, d, a, x[2] + 0xc4ac5665, 23);

	MD5STEP(F4, a, b, c, d, x[0] + 0xf4292244, 6);
	MD5STEP(F4, d, a, b, c, x[7] + 0x432aff97, 10);
	MD5STEP(F4, c, d, a, b, x[14] + 0xab9423a7, 15);
	MD5STEP(F4, b, c, d, a, x[5] + 0xfc93a039, 21);
	MD5STEP(F4, a, b, c, d, x[12] + 0x655b59c3, 6);
	MD5STEP(F4, d, a, b, c, x[3] + 0x8f0ccc92, 10);
	MD5STEP(F4, c, d, a, b, x[10] + 0xffeff47d, 15);
	MD5STEP(F4, b, c, d, a, x[1] + 0x85845dd1, 21);
	MD5STEP(F4, a, b, c, d, x[8] + 0x6fa87e4f, 6);
	MD5STEP(F4, d, a, b, c, x[15] + 0xfe2ce6e0, 10);
	MD5STEP(F4, c, d, a, b, x[6] + 0xa3014314, 15);
	MD5STEP(F4, b, c, d, a, x[13] + 0x4e0811a1, 21);
	MD5STEP(F4, a, b, c, d, x[4] + 0xf7537e82, 6);
	MD5STEP(F4, d, a, b, c, x[11] + 0xbd3af235, 10);
	MD5STEP(F4, c, d, a, b, x[2] + 0x2ad7d2bb, 15);
	MD5STEP(F4, b, c, d, a, x[9] + 0xeb86d391, 21);

#undef F1
#undef F2
#undef F3
#undef F4

	/* Update chaining vars */
	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
}

/* Update the message digest with the contents of the buffer (SHA-1) */
static void sha1_write(HASH_CONTEXT *ctx, const uint8_t *buf, size_t len)
{
	size_t num = ctx->bytecount & (SHA1_BLOCKSIZE - 1);

	/* Update bytecount */
	ctx->bytecount += len;

	/* Handle any leading odd-sized chunks */
	if (num) {
		uint8_t *p = ctx->buf + num;

		num = SHA1_BLOCKSIZE - num;
		if (len < num) {
			memcpy(p, buf, len);
			return;
		}
		memcpy(p, buf, num);
		sha1_transform(ctx, ctx->buf);
		buf += num;
		len -= num;
	}

#ifdef CPU_X86_SHA1_ACCELERATION
	if (cpu_has_sha1_accel)
	{
		/* Process all full blocks at once */
		if (len >= SHA1_BLOCKSIZE) {
			/* Calculate full blocks, in bytes */
			num = (len / SHA1_BLOCKSIZE) * SHA1_BLOCKSIZE;
			/* SHA-1 acceleration using intrinsics */
			sha1_transform_x86(ctx->state, buf, num);
			buf += num;
			len -= num;
		}
	}
	else
#endif
	{
		/* Process data in blocksize chunks */
		while (len >= SHA1_BLOCKSIZE) {
			PREFETCH64(buf + SHA1_BLOCKSIZE);
			sha1_transform(ctx, buf);
			buf += SHA1_BLOCKSIZE;
			len -= SHA1_BLOCKSIZE;
		}
	}

	/* Handle any remaining bytes of data. */
	memcpy(ctx->buf, buf, len);
}

/* Update the message digest with the contents of the buffer (SHA-256) */
static void sha256_write(HASH_CONTEXT *ctx, const uint8_t *buf, size_t len)
{
	size_t num = ctx->bytecount & (SHA256_BLOCKSIZE - 1);

	/* Update bytecount */
	ctx->bytecount += len;

	/* Handle any leading odd-sized chunks */
	if (num) {
		uint8_t *p = ctx->buf + num;

		num = SHA256_BLOCKSIZE - num;
		if (len < num) {
			memcpy(p, buf, len);
			return;
		}
		memcpy(p, buf, num);
		sha256_transform(ctx, ctx->buf);
		buf += num;
		len -= num;
	}

#ifdef CPU_X86_SHA256_ACCELERATION
	if (cpu_has_sha256_accel)
	{
		/* Process all full blocks at once */
		if (len >= SHA256_BLOCKSIZE) {
			/* Calculate full blocks, in bytes */
			num = (len / SHA256_BLOCKSIZE) * SHA256_BLOCKSIZE;
			/* SHA-256 acceleration using intrinsics */
			sha256_transform_x86(ctx->state, buf, num);
			buf += num;
			len -= num;
		}
	}
	else
#endif
	{
		/* Process data in blocksize chunks */
		while (len >= SHA256_BLOCKSIZE) {
			PREFETCH64(buf + SHA256_BLOCKSIZE);
			sha256_transform(ctx, buf);
			buf += SHA256_BLOCKSIZE;
			len -= SHA256_BLOCKSIZE;
		}
	}

	/* Handle any remaining bytes of data. */
	memcpy(ctx->buf, buf, len);
}

/* Update the message digest with the contents of the buffer (SHA-512) */
static void sha512_write(HASH_CONTEXT* ctx, const uint8_t* buf, size_t len)
{
	size_t num = ctx->bytecount & (SHA512_BLOCKSIZE - 1);

	/* Update bytecount */
	ctx->bytecount += len;

	/* Handle any leading odd-sized chunks */
	if (num) {
		uint8_t* p = ctx->buf + num;

		num = SHA512_BLOCKSIZE - num;
		if (len < num) {
			memcpy(p, buf, len);
			return;
		}
		memcpy(p, buf, num);
		sha512_transform(ctx, ctx->buf);
		buf += num;
		len -= num;
	}

	/* Process data in blocksize chunks */
	while (len >= SHA512_BLOCKSIZE) {
		PREFETCH64(buf + SHA512_BLOCKSIZE);
		sha512_transform(ctx, buf);
		buf += SHA512_BLOCKSIZE;
		len -= SHA512_BLOCKSIZE;
	}

	/* Handle any remaining bytes of data. */
	memcpy(ctx->buf, buf, len);
}

/* Update the message digest with the contents of the buffer (MD5) */
static void md5_write(HASH_CONTEXT *ctx, const uint8_t *buf, size_t len)
{
	size_t num = ctx->bytecount & (MD5_BLOCKSIZE - 1);

	/* Update bytecount */
	ctx->bytecount += len;

	/* Handle any leading odd-sized chunks */
	if (num) {
		uint8_t *p = ctx->buf + num;

		num = MD5_BLOCKSIZE - num;
		if (len < num) {
			memcpy(p, buf, num);
			return;
		}
		memcpy(p, buf, num);
		md5_transform(ctx, ctx->buf);
		buf += num;
		len -= num;
	}

	/* Process data in blocksize chunks */
	while (len >= MD5_BLOCKSIZE) {
		PREFETCH64(buf + MD5_BLOCKSIZE);
		md5_transform(ctx, buf);
		buf += MD5_BLOCKSIZE;
		len -= MD5_BLOCKSIZE;
	}

	/* Handle any remaining bytes of data. */
	memcpy(ctx->buf, buf, len);
}

/* Finalize the computation and write the digest in ctx->state[] (SHA-1) */
static void sha1_final(HASH_CONTEXT *ctx)
{
	size_t pos = ((size_t)ctx->bytecount) & (SHA1_BLOCKSIZE - 1);
	uint64_t bitcount = ctx->bytecount << 3;
	uint8_t *p;

	ctx->buf[pos++] = 0x80;

	/* Pad whatever data is left in the buffer */
	while (pos != (SHA1_BLOCKSIZE - sizeof(uint64_t))) {
		pos &= (SHA1_BLOCKSIZE - 1);
		if (pos == 0)
			sha1_transform(ctx, ctx->buf);
		ctx->buf[pos++] = 0;
	}

	/* Append to the padding the total message's length in bits and transform */
	ctx->buf[SHA1_BLOCKSIZE - 1] = (uint8_t) bitcount;
	ctx->buf[SHA1_BLOCKSIZE - 2] = (uint8_t) (bitcount >> 8);
	ctx->buf[SHA1_BLOCKSIZE - 3] = (uint8_t) (bitcount >> 16);
	ctx->buf[SHA1_BLOCKSIZE - 4] = (uint8_t) (bitcount >> 24);
	ctx->buf[SHA1_BLOCKSIZE - 5] = (uint8_t) (bitcount >> 32);
	ctx->buf[SHA1_BLOCKSIZE - 6] = (uint8_t) (bitcount >> 40);
	ctx->buf[SHA1_BLOCKSIZE - 7] = (uint8_t) (bitcount >> 48);
	ctx->buf[SHA1_BLOCKSIZE - 8] = (uint8_t) (bitcount >> 56);

	sha1_transform(ctx, ctx->buf);

	p = ctx->buf;
#ifdef BIG_ENDIAN_HOST
#define X(a) do { *(uint32_t*)p = (uint32_t)ctx->state[a]; p += 4; } while(0)
#else
#define X(a) do { write_swap32(p, (uint32_t)ctx->state[a]); p += 4; } while(0);
#endif
	X(0);
	X(1);
	X(2);
	X(3);
	X(4);
#undef X
}

/* Finalize the computation and write the digest in ctx->state[] (SHA-256) */
static void sha256_final(HASH_CONTEXT *ctx)
{
	size_t pos = ((size_t)ctx->bytecount) & (SHA256_BLOCKSIZE - 1);
	uint64_t bitcount = ctx->bytecount << 3;
	uint8_t *p;

	ctx->buf[pos++] = 0x80;

	/* Pad whatever data is left in the buffer */
	while (pos != (SHA256_BLOCKSIZE - sizeof(uint64_t))) {
		pos &= (SHA256_BLOCKSIZE - 1);
		if (pos == 0)
			sha256_transform(ctx, ctx->buf);
		ctx->buf[pos++] = 0;
	}

	/* Append to the padding the total message's length in bits and transform */
	ctx->buf[SHA256_BLOCKSIZE - 1] = (uint8_t) bitcount;
	ctx->buf[SHA256_BLOCKSIZE - 2] = (uint8_t) (bitcount >> 8);
	ctx->buf[SHA256_BLOCKSIZE - 3] = (uint8_t) (bitcount >> 16);
	ctx->buf[SHA256_BLOCKSIZE - 4] = (uint8_t) (bitcount >> 24);
	ctx->buf[SHA256_BLOCKSIZE - 5] = (uint8_t) (bitcount >> 32);
	ctx->buf[SHA256_BLOCKSIZE - 6] = (uint8_t) (bitcount >> 40);
	ctx->buf[SHA256_BLOCKSIZE - 7] = (uint8_t) (bitcount >> 48);
	ctx->buf[SHA256_BLOCKSIZE - 8] = (uint8_t) (bitcount >> 56);

	sha256_transform(ctx, ctx->buf);

	p = ctx->buf;
#ifdef BIG_ENDIAN_HOST
#define X(a) do { *(uint32_t*)p = (uint32_t)ctx->state[a]; p += 4; } while(0)
#else
#define X(a) do { write_swap32(p, (uint32_t)ctx->state[a]); p += 4; } while(0);
#endif
	X(0);
	X(1);
	X(2);
	X(3);
	X(4);
	X(5);
	X(6);
	X(7);
#undef X
}

/* Finalize the computation and write the digest in ctx->state[] (SHA-256) */
static void sha512_final(HASH_CONTEXT* ctx)
{
	size_t pos = ((size_t)ctx->bytecount) & (SHA512_BLOCKSIZE - 1);
	/* 16 EB ought to be enough for everybody... */
	uint64_t bitcount_lo = ctx->bytecount << 3;
	uint64_t bitcount_hi = ctx->bytecount >> (64 - 3);
	uint8_t* p;

	ctx->buf[pos++] = 0x80;

	/* Pad whatever data is left in the buffer */
	while (pos != (SHA512_BLOCKSIZE - (2 * sizeof(uint64_t)))) {
		pos &= (SHA512_BLOCKSIZE - 1);
		if (pos == 0)
			sha512_transform(ctx, ctx->buf);
		ctx->buf[pos++] = 0;
	}

	/* Append to the padding the total message's length in bits and transform */
	ctx->buf[SHA512_BLOCKSIZE - 1] = (uint8_t)bitcount_lo;
	ctx->buf[SHA512_BLOCKSIZE - 2] = (uint8_t)(bitcount_lo >> 8);
	ctx->buf[SHA512_BLOCKSIZE - 3] = (uint8_t)(bitcount_lo >> 16);
	ctx->buf[SHA512_BLOCKSIZE - 4] = (uint8_t)(bitcount_lo >> 24);
	ctx->buf[SHA512_BLOCKSIZE - 5] = (uint8_t)(bitcount_lo >> 32);
	ctx->buf[SHA512_BLOCKSIZE - 6] = (uint8_t)(bitcount_lo >> 40);
	ctx->buf[SHA512_BLOCKSIZE - 7] = (uint8_t)(bitcount_lo >> 48);
	ctx->buf[SHA512_BLOCKSIZE - 8] = (uint8_t)(bitcount_lo >> 56);
	ctx->buf[SHA512_BLOCKSIZE - 9] = (uint8_t)bitcount_hi;
	/* For clarity since, with a 64-bit bytecount, the following are always 0 */
	ctx->buf[SHA512_BLOCKSIZE - 10] = (uint8_t)(bitcount_hi >> 8);
	ctx->buf[SHA512_BLOCKSIZE - 11] = (uint8_t)(bitcount_hi >> 16);
	ctx->buf[SHA512_BLOCKSIZE - 12] = (uint8_t)(bitcount_hi >> 24);
	ctx->buf[SHA512_BLOCKSIZE - 13] = (uint8_t)(bitcount_hi >> 32);
	ctx->buf[SHA512_BLOCKSIZE - 14] = (uint8_t)(bitcount_hi >> 40);
	ctx->buf[SHA512_BLOCKSIZE - 15] = (uint8_t)(bitcount_hi >> 48);
	ctx->buf[SHA512_BLOCKSIZE - 16] = (uint8_t)(bitcount_hi >> 56);

	sha512_transform(ctx, ctx->buf);

	p = ctx->buf;
#ifdef BIG_ENDIAN_HOST
#define X(a) do { *p = ctx->state[a]; p += 8; } while(0)
#else
#define X(a) do { write_swap64(p, ctx->state[a]); p += 8; } while(0);
#endif
	X(0);
	X(1);
	X(2);
	X(3);
	X(4);
	X(5);
	X(6);
	X(7);
#undef X
}

/* Finalize the computation and write the digest in ctx->state[] (MD5) */
static void md5_final(HASH_CONTEXT *ctx)
{
	size_t count = ((size_t)ctx->bytecount) & (MD5_BLOCKSIZE - 1);
	uint64_t bitcount = ctx->bytecount << 3;
	uint8_t *p;

	/* Set the first char of padding to 0x80.
	 * This is safe since there is always at least one byte free
	 */
	p = ctx->buf + count;
	*p++ = 0x80;

	/* Bytes of padding needed to make blocksize */
	count = (MD5_BLOCKSIZE - 1) - count;

	/* Pad out to 56 mod 64 */
	if (count < 8) {
		/* Two lots of padding: Pad the first block to blocksize */
		memset(p, 0, count);
		md5_transform(ctx, ctx->buf);

		/* Now fill the next block */
		memset(ctx->buf, 0, MD5_BLOCKSIZE - 8);
	} else {
		/* Pad block to blocksize */
		memset(p, 0, count - 8);
	}

	/* append the 64 bit count (little endian) */
	ctx->buf[MD5_BLOCKSIZE - 8] = (uint8_t) bitcount;
	ctx->buf[MD5_BLOCKSIZE - 7] = (uint8_t) (bitcount >> 8);
	ctx->buf[MD5_BLOCKSIZE - 6] = (uint8_t) (bitcount >> 16);
	ctx->buf[MD5_BLOCKSIZE - 5] = (uint8_t) (bitcount >> 24);
	ctx->buf[MD5_BLOCKSIZE - 4] = (uint8_t) (bitcount >> 32);
	ctx->buf[MD5_BLOCKSIZE - 3] = (uint8_t) (bitcount >> 40);
	ctx->buf[MD5_BLOCKSIZE - 2] = (uint8_t) (bitcount >> 48);
	ctx->buf[MD5_BLOCKSIZE - 1] = (uint8_t) (bitcount >> 56);

	md5_transform(ctx, ctx->buf);

	p = ctx->buf;
#ifdef BIG_ENDIAN_HOST
#define X(a) do { write_swap32(p, (uint32_t)ctx->state[a]); p += 4; } while(0);
#else
#define X(a) do { *(uint32_t*)p = (uint32_t)ctx->state[a]; p += 4; } while(0)
#endif
	X(0);
	X(1);
	X(2);
	X(3);
#undef X
}

//#define NULL_TEST
#ifdef NULL_TEST
// These 'null' calls are useful for testing load balancing and individual algorithm speed
static void null_init(HASH_CONTEXT *ctx) { memset(ctx, 0, sizeof(*ctx)); }
static void null_write(HASH_CONTEXT *ctx, const uint8_t *buf, size_t len) { }
static void null_final(HASH_CONTEXT *ctx) { }
#endif

hash_init_t *hash_init[HASH_MAX] = { md5_init, sha1_init , sha256_init, sha512_init };
hash_write_t *hash_write[HASH_MAX] = { md5_write, sha1_write , sha256_write, sha512_write };
hash_final_t *hash_final[HASH_MAX] = { md5_final, sha1_final , sha256_final, sha512_final };


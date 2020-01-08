/*
 * Copyright 2017-2018, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

[[gnu::always_inline]] static __m128i inline
m256_get16b(__m256i ymm)
{
    return _mm256_extractf128_si256(ymm, 0);
}

[[gnu::always_inline]] static uint64_t inline
m256_get8b(__m256i ymm)
{
    return (uint64_t)_mm256_extract_epi64(ymm, 0);
}

[[gnu::always_inline]] static uint32_t inline
m256_get4b(__m256i ymm)
{
    return (uint32_t)_mm256_extract_epi32(ymm, 0);
}

[[gnu::always_inline]] static uint16_t inline
m256_get2b(__m256i ymm)
{
    return (uint16_t)_mm256_extract_epi16(ymm, 0);
}

[[gnu::always_inline]] static void inline
memset_movnt8x64b(char *dest, __m256i ymm)
{
    _mm256_stream_si256((__m256i *)dest + 0, ymm);
    _mm256_stream_si256((__m256i *)dest + 1, ymm);
    _mm256_stream_si256((__m256i *)dest + 2, ymm);
    _mm256_stream_si256((__m256i *)dest + 3, ymm);
    _mm256_stream_si256((__m256i *)dest + 4, ymm);
    _mm256_stream_si256((__m256i *)dest + 5, ymm);
    _mm256_stream_si256((__m256i *)dest + 6, ymm);
    _mm256_stream_si256((__m256i *)dest + 7, ymm);
    _mm256_stream_si256((__m256i *)dest + 8, ymm);
    _mm256_stream_si256((__m256i *)dest + 9, ymm);
    _mm256_stream_si256((__m256i *)dest + 10, ymm);
    _mm256_stream_si256((__m256i *)dest + 11, ymm);
    _mm256_stream_si256((__m256i *)dest + 12, ymm);
    _mm256_stream_si256((__m256i *)dest + 13, ymm);
    _mm256_stream_si256((__m256i *)dest + 14, ymm);
    _mm256_stream_si256((__m256i *)dest + 15, ymm);
}

[[gnu::always_inline]] static void inline
memset_movnt4x64b(char *dest, __m256i ymm)
{
    _mm256_stream_si256((__m256i *)dest + 0, ymm);
    _mm256_stream_si256((__m256i *)dest + 1, ymm);
    _mm256_stream_si256((__m256i *)dest + 2, ymm);
    _mm256_stream_si256((__m256i *)dest + 3, ymm);
    _mm256_stream_si256((__m256i *)dest + 4, ymm);
    _mm256_stream_si256((__m256i *)dest + 5, ymm);
    _mm256_stream_si256((__m256i *)dest + 6, ymm);
    _mm256_stream_si256((__m256i *)dest + 7, ymm);
}

[[gnu::always_inline]] static void inline
memset_movnt2x64b(char *dest, __m256i ymm)
{
    _mm256_stream_si256((__m256i *)dest + 0, ymm);
    _mm256_stream_si256((__m256i *)dest + 1, ymm);
    _mm256_stream_si256((__m256i *)dest + 2, ymm);
    _mm256_stream_si256((__m256i *)dest + 3, ymm);
}

[[gnu::always_inline]] static void inline
memset_movnt1x64b(char *dest, __m256i ymm)
{
    _mm256_stream_si256((__m256i *)dest + 0, ymm);
    _mm256_stream_si256((__m256i *)dest + 1, ymm);
}

[[gnu::always_inline]] static void inline
memset_movnt1x32b(char *dest, __m256i ymm)
{
    _mm256_stream_si256((__m256i *)dest, ymm);
}

[[gnu::always_inline]] static void inline
memset_movnt1x16b(char *dest, __m256i ymm)
{
    __m128i xmm0 = m256_get16b(ymm);

    _mm_stream_si128((__m128i *)dest, xmm0);
}

[[gnu::always_inline]] static void inline
memset_movnt1x8b(char *dest, __m256i ymm)
{
    uint64_t x = m256_get8b(ymm);

    _mm_stream_si64((long long *)dest, (long long)x);
}

[[gnu::always_inline]] static void inline
memset_movnt1x4b(char *dest, __m256i ymm)
{
    uint32_t x = m256_get4b(ymm);

    _mm_stream_si32((int *)dest, (int)x);
}

[[gnu::always_inline]] static void inline
memset_small_avx(char *dest, __m256i ymm, size_t len)
{
	assert(len <= 64);

    uint64_t d2, d8;

	if (len <= 8)
		goto le8;
	if (len <= 32)
		goto le32;

	/* 33..64 */
	_mm256_storeu_si256((__m256i *)dest, ymm);
	_mm256_storeu_si256((__m256i *)(dest + len - 32), ymm);
	return;

le32:
	if (len > 16) {
		/* 17..32 */
		__m128i xmm = m256_get16b(ymm);

		_mm_storeu_si128((__m128i *)dest, xmm);
		_mm_storeu_si128((__m128i *)(dest + len - 16), xmm);
		return;
	}

	/* 9..16 */
	d8 = m256_get8b(ymm);

	*(uint64_t *)dest = d8;
	*(uint64_t *)(dest + len - 8) = d8;
	return;

le8:
	if (len <= 2)
		goto le2;

	if (len > 4) {
		/* 5..8 */
		uint32_t d = m256_get4b(ymm);

		*(uint32_t *)dest = d;
		*(uint32_t *)(dest + len - 4) = d;
		return;
	}

	/* 3..4 */
	d2 = m256_get2b(ymm);

	*(uint16_t *)dest = d2;
	*(uint16_t *)(dest + len - 2) = d2;
	return;

le2:
	if (len == 2) {
		uint16_t d2 = m256_get2b(ymm);

		*(uint16_t *)dest = d2;
		return;
	}

	*(uint8_t *)dest = (uint8_t)m256_get2b(ymm);
}

[[gnu::always_inline]] static int inline
util_is_pow2(uint64_t v)
{
	return v && !(v & (v - 1));
}

[[gnu::always_inline]] static void inline
avx_zeroupper(void)
{
	_mm256_zeroupper();
}

void
memset_movnt_avx(char *dest, int c, size_t len)
{
    __m256i ymm = _mm256_set1_epi8((char)c);

    size_t cnt = (uint64_t)dest & 63;
    if (cnt > 0)
    {
        cnt = 64 - cnt;

        if (cnt > len)
            cnt = len;

        memset_small_avx(dest, ymm, cnt);

        dest += cnt;
        len -= cnt;
    }

    while (len >= 8 * 64)
    {
        memset_movnt8x64b(dest, ymm);
        dest += 8 * 64;
        len -= 8 * 64;
    }

    if (len >= 4 * 64)
    {
        memset_movnt4x64b(dest, ymm);
        dest += 4 * 64;
        len -= 4 * 64;
    }

    if (len >= 2 * 64)
    {
        memset_movnt2x64b(dest, ymm);
        dest += 2 * 64;
        len -= 2 * 64;
    }

    if (len >= 1 * 64)
    {
        memset_movnt1x64b(dest, ymm);

        dest += 1 * 64;
        len -= 1 * 64;
    }

    if (len == 0)
        goto end;

    /* There's no point in using more than 1 nt store for 1 cache line. */
    if (util_is_pow2(len))
    {
        if (len == 32)
            memset_movnt1x32b(dest, ymm);
        else if (len == 16)
            memset_movnt1x16b(dest, ymm);
        else if (len == 8)
            memset_movnt1x8b(dest, ymm);
        else if (len == 4)
            memset_movnt1x4b(dest, ymm);
        else
            goto nonnt;

        goto end;
    }

nonnt:
    memset_small_avx(dest, ymm, len);
end:
    avx_zeroupper();
}


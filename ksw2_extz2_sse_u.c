#include <string.h>
#include "ksw2.h"

#ifdef __SSE2__
#include <emmintrin.h>

#ifdef __SSE4_1__
#include <smmintrin.h>
#endif

void ksw_extz2_sse_u(void *km, int qlen, const uint8_t *query, int tlen, const uint8_t *target, int8_t m, const int8_t *mat, int8_t q, int8_t e, int w, int zdrop, int flag, ksw_extz_t *ez)
{
#define __dp_code_block1 \
	z = _mm_add_epi8(_mm_loadu_si128((__m128i*)&s[t]), qe2_); \
	xt1 = _mm_loadu_si128((__m128i*)&x[t]);          /* xt1 <- x[r-1][t..t+15] */ \
	tmp = _mm_srli_si128(xt1, 15);                   /* tmp <- x[r-1][t+15] */ \
	xt1 = _mm_or_si128(_mm_slli_si128(xt1, 1), x1_); /* xt1 <- x[r-1][t-1..t+14] */ \
	x1_ = tmp; \
	vt1 = _mm_loadu_si128((__m128i*)&v[t]);          /* vt1 <- v[r-1][t..t+15] */ \
	tmp = _mm_srli_si128(vt1, 15);                   /* tmp <- v[r-1][t+15] */ \
	vt1 = _mm_or_si128(_mm_slli_si128(vt1, 1), v1_); /* vt1 <- v[r-1][t-1..t+14] */ \
	v1_ = tmp; \
	a = _mm_add_epi8(xt1, vt1);                      /* a <- x[r-1][t-1..t+14] + v[r-1][t-1..t+14] */ \
	ut = _mm_loadu_si128((__m128i*)&u[t]);           /* ut <- u[t..t+15] */ \
	b = _mm_add_epi8(_mm_loadu_si128((__m128i*)&y[t]), ut); /* b <- y[r-1][t..t+15] + u[r-1][t..t+15] */

#define __dp_code_block2 \
	z = _mm_max_epu8(z, b);                                  /* z = max(z, b); this works because both are non-negative */ \
	_mm_storeu_si128((__m128i*)&u[t], _mm_sub_epi8(z, vt1)); /* u[r][t..t+15] <- z - v[r-1][t-1..t+14] */ \
	_mm_storeu_si128((__m128i*)&v[t], _mm_sub_epi8(z, ut));  /* v[r][t..t+15] <- z - u[r-1][t..t+15] */ \
	z = _mm_sub_epi8(z, q_); \
	a = _mm_sub_epi8(a, z); \
	b = _mm_sub_epi8(b, z);

	int r, t, qe = q + e, n_col, *off = 0, tlen16, qlen16, with_cigar = !(flag&KSW_EZ_SCORE_ONLY);
	int8_t *u, *v, *x, *y, *s;
	int32_t *H;
	uint8_t *p = 0, *qr, *sf, *mem;
	__m128i q_, qe2_, zero_, flag1_, flag2_, flag4_, flag32_, sc_mch_, sc_mis_, m1_;

	zero_   = _mm_set1_epi8(0);
	q_      = _mm_set1_epi8(q);
	qe2_    = _mm_set1_epi8((q + e) * 2);
	flag1_  = _mm_set1_epi8(1<<0);
	flag2_  = _mm_set1_epi8(2<<0);
	flag4_  = _mm_set1_epi8(1<<2);
	flag32_ = _mm_set1_epi8(2<<4);
	sc_mch_ = _mm_set1_epi8(mat[0]);
	sc_mis_ = _mm_set1_epi8(mat[1]);
	m1_     = _mm_set1_epi8(m - 1); // wildcard

	ez->max_q = ez->max_t = ez->mqe_t = ez->mte_q = -1;
	ez->max = 0, ez->mqe = ez->mte = KSW_NEG_INF;
	ez->n_cigar = 0;

	w = (w + 1 + 15) / 16 * 16 - 1;
	tlen16 = (tlen + 15) / 16 * 16;
	n_col = w + 1 < tlen16? w + 1 : tlen16; // number of columns in the backtrack matrix
	n_col += 16, tlen16 += 16; // leave enough space at the end
	qlen16 = (qlen + 15) / 16 * 16 + 16;

	mem = (uint8_t*)kcalloc(km, tlen16 * 6 + qlen16 + 15, 1);
	u = (int8_t*)(((size_t)mem + 15) >> 4 << 4); // 16-byte aligned (though not necessary)
	v = u + tlen16, x = v + tlen16, y = x + tlen16, s = y + tlen16, sf = (uint8_t*)s + tlen16, qr = sf + tlen16;
	H = (int32_t*)kcalloc(km, (tlen + 3) / 4 * 4 + 4, 4);
	if (with_cigar) {
		p = (uint8_t*)kcalloc(km, (qlen + tlen) * n_col, 1);
		off = (int*)kmalloc(km, (qlen + tlen) * sizeof(int));
	}

	for (t = 0; t < qlen; ++t) qr[t] = query[qlen - 1 - t];
	memcpy(sf, target, tlen);

	for (r = 0; r < qlen + tlen - 1; ++r) {
		int st = 0, en = tlen - 1, max_H, max_t;
		int8_t x1, v1;
		uint8_t *qrr = qr + (qlen - 1 - r);
		__m128i x1_, v1_;
		// find the boundaries
		if (st < r - qlen + 1) st = r - qlen + 1;
		if (en > r) en = r;
		if (st < (r-w+1)>>1) st = (r-w+1)>>1; // take the ceil
		if (en > (r+w)>>1) en = (r+w)>>1; // take the floor
		// set boundary conditions
		if (st != 0) {
			if (r > st + st + w - 1) x1 = v1 = 0;
			else x1 = x[st-1], v1 = v[st-1]; // (r-1, st-1) in the band
		} else x1 = 0, v1 = r? q : 0;
		if (en != r) {
			if (r < en + en - w - 1) y[en] = u[en] = 0; // (r-1,en) out of the band; TODO: is this line necessary?
		} else y[r] = 0, u[r] = r? q : 0;
		// loop fission: set scores first

		if (flag & KSW_EZ_SIMPLE_SC) {
			for (t = st; t <= en; t += 16) {
				__m128i sq, st, tmp, mask;
				sq = _mm_loadu_si128((__m128i*)&sf[t]);
				st = _mm_loadu_si128((__m128i*)&qrr[t]);
				mask = _mm_or_si128(_mm_cmpeq_epi8(sq, m1_), _mm_cmpeq_epi8(st, m1_));
				tmp = _mm_cmpeq_epi8(sq, st);
#ifdef __SSE4_1__
				tmp = _mm_blendv_epi8(sc_mis_, sc_mch_, tmp);
#else
				tmp = _mm_or_si128(_mm_andnot_si128(tmp, sc_mis_), _mm_and_si128(tmp, sc_mch_));
#endif
				tmp = _mm_andnot_si128(mask, tmp);
				_mm_storeu_si128((__m128i*)&s[t], tmp);
			}
		} else {
			for (t = st; t <= en; ++t)
				s[t] = mat[sf[t] * m + qrr[t]];
		}
		// core loop
		x1_ = _mm_cvtsi32_si128(x1);
		v1_ = _mm_cvtsi32_si128(v1);
		if (!with_cigar) { // score only
			for (t = st; t <= en; t += 16) {
				__m128i z, a, b, xt1, vt1, ut, tmp;
				__dp_code_block1;
#ifdef __SSE4_1__
				z = _mm_max_epi8(z, a);                          // z = z > a? z : a (signed)
#else // we need to emulate SSE4.1 intrinsics _mm_max_epi8()
				z = _mm_and_si128(z, _mm_cmpgt_epi8(z, zero_));  // z = z > 0? z : 0;
				z = _mm_max_epu8(z, a);                          // z = max(z, a); this works because both are non-negative
#endif
				__dp_code_block2;
#ifdef __SSE4_1__
				_mm_storeu_si128((__m128i*)&x[t], _mm_max_epi8(a, zero_));
				_mm_storeu_si128((__m128i*)&y[t], _mm_max_epi8(b, zero_));
#else
				tmp = _mm_cmpgt_epi8(a, zero_);
				_mm_storeu_si128((__m128i*)&x[t], _mm_and_si128(a, tmp));
				tmp = _mm_cmpgt_epi8(b, zero_);
				_mm_storeu_si128((__m128i*)&y[t], _mm_and_si128(b, tmp));
#endif
			}
		} else if (!(flag&KSW_EZ_RIGHT)) { // gap left-alignment
			uint8_t *pr = p + r * n_col;
			off[r] = st;
			for (t = st; t <= en; t += 16) {
				__m128i d, z, a, b, xt1, vt1, ut, tmp;
				__dp_code_block1;
				d = _mm_and_si128(_mm_cmpgt_epi8(a, z), flag1_); // d = a > z? 1 : 0
#ifdef __SSE4_1__
				z = _mm_max_epi8(z, a);                          // z = z > a? z : a (signed)
				tmp = _mm_cmpgt_epi8(b, z);
				d = _mm_blendv_epi8(d, flag2_, tmp);             // d = b > z? 2 : d
#else // we need to emulate SSE4.1 intrinsics _mm_max_epi8() and _mm_blendv_epi8()
				z = _mm_and_si128(z, _mm_cmpgt_epi8(z, zero_));  // z = z > 0? z : 0;
				z = _mm_max_epu8(z, a);                          // z = max(z, a); this works because both are non-negative
				tmp = _mm_cmpgt_epi8(b, z);
				d = _mm_or_si128(_mm_andnot_si128(tmp, d), _mm_and_si128(tmp, flag2_)); // d = b > z? 2 : d; emulating blendv
#endif
				__dp_code_block2;
				tmp = _mm_cmpgt_epi8(a, zero_);
				d = _mm_or_si128(d, _mm_and_si128(tmp, flag4_));  // d = a > 0? 1<<2 : 0
				_mm_storeu_si128((__m128i*)&x[t], _mm_and_si128(tmp, a));
				tmp = _mm_cmpgt_epi8(b, zero_);
				d = _mm_or_si128(d, _mm_and_si128(tmp, flag32_)); // d = b > 0? 2<<4 : 0
				_mm_storeu_si128((__m128i*)&y[t], _mm_and_si128(tmp, b));
				_mm_storeu_si128((__m128i*)&pr[t - st], d);
			}
		} else { // gap right-alignment
			uint8_t *pr = p + r * n_col;
			off[r] = st;
			for (t = st; t <= en; t += 16) {
				__m128i d, z, a, b, xt1, vt1, ut, tmp;
				__dp_code_block1;
				d = _mm_andnot_si128(_mm_cmpgt_epi8(z, a), flag1_); // d = z > a? 0 : 1
#ifdef __SSE4_1__
				z = _mm_max_epi8(z, a);                          // z = z > a? z : a (signed)
				tmp = _mm_cmpgt_epi8(z, b);
				d = _mm_blendv_epi8(flag2_, d, tmp);             // d = z > b? d : 2
#else // we need to emulate SSE4.1 intrinsics _mm_max_epi8() and _mm_blendv_epi8()
				z = _mm_and_si128(z, _mm_cmpgt_epi8(z, zero_));  // z = z > 0? z : 0;
				z = _mm_max_epu8(z, a);                          // z = max(z, a); this works because both are non-negative
				tmp = _mm_cmpgt_epi8(z, b);
				d = _mm_or_si128(_mm_andnot_si128(tmp, flag2_), _mm_and_si128(tmp, d)); // d = z > b? d : 2; emulating blendv
#endif
				__dp_code_block2;
				tmp = _mm_cmpgt_epi8(zero_, a);
				d = _mm_or_si128(d, _mm_andnot_si128(tmp, flag4_));  // d = 0 > a? 0 : 1<<2
				_mm_storeu_si128((__m128i*)&x[t], _mm_andnot_si128(tmp, a));
				tmp = _mm_cmpgt_epi8(zero_, b);
				d = _mm_or_si128(d, _mm_andnot_si128(tmp, flag32_)); // d = 0 > b? 0 : 2<<4
				_mm_storeu_si128((__m128i*)&y[t], _mm_andnot_si128(tmp, b));
				_mm_storeu_si128((__m128i*)&pr[t - st], d);
			}
		}
		// compute H[]
		if (r > 0) {
			int32_t HH[4], tt[4], en1 = st + (en - st) / 4 * 4, i;
			__m128i max_H_, max_t_, qe_;
			max_H = H[en] = H[en-1] + u[en] - qe, max_t = en; // special casing last H
			max_H_ = _mm_set1_epi32(max_H);
			max_t_ = _mm_set1_epi32(max_t);
			qe_    = _mm_set1_epi32(q + e);
			for (t = st; t < en1; t += 4) {
				__m128i H1, tmp, t_;
				H1 = _mm_loadu_si128((__m128i*)&H[t]);
				t_ = _mm_setr_epi32(v[t], v[t+1], v[t+2], v[t+3]);
				H1 = _mm_add_epi32(H1, t_);
				H1 = _mm_sub_epi32(H1, qe_);
				_mm_storeu_si128((__m128i*)&H[t], H1);
				t_ = _mm_set1_epi32(t);
				tmp = _mm_cmpgt_epi32(H1, max_H_);
#ifdef __SSE4_1__
				max_H_ = _mm_blendv_epi8(max_H_, H1, tmp); // _mm_blendv_epi8 also works for 32-bit integers here
				max_t_ = _mm_blendv_epi8(max_t_, t_, tmp);
#else
				max_H_ = _mm_or_si128(_mm_and_si128(tmp, H1), _mm_andnot_si128(tmp, max_H_));
				max_t_ = _mm_or_si128(_mm_and_si128(tmp, t_), _mm_andnot_si128(tmp, max_t_));
#endif
			}
			_mm_storeu_si128((__m128i*)HH, max_H_);
			_mm_storeu_si128((__m128i*)tt, max_t_);
			for (i = 0; i < 4; ++i)
				if (max_H < HH[i]) max_H = HH[i], max_t = tt[i] + i;
			for (; t < en; ++t) {
				H[t] += (int32_t)v[t] - qe;
				if (H[t] > max_H)
					max_H = H[t], max_t = t;
			}
		} else H[0] = v[0] - qe - qe, max_H = H[0], max_t = 0; // special casing r==0
		// update ez
		if (en == tlen - 1 && H[en] > ez->mte)
			ez->mte = H[en], ez->mte_q = r - en;
		if (r - st == qlen - 1 && H[st] > ez->mqe)
			ez->mqe = H[st], ez->mqe_t = st;
		if (max_H > ez->max) {
			ez->max = max_H, ez->max_t = max_t, ez->max_q = r - max_t;
		} else if (r - max_t > ez->max_q) {
			int tl = max_t - ez->max_t, ql = (r - max_t) - ez->max_q, l;
			l = tl > ql? tl - ql : ql - tl;
			if (ez->max - max_H > zdrop + l * e)
				break;
		}
		if (r == qlen + tlen - 2 && en == tlen - 1)
			ez->score = H[tlen - 1];
		//for (t = st; t <= en; ++t) printf("(%d,%d)\t(%d,%d,%d,%d)\t%d\t%x\n", r, t, u[t], v[t], x[t], y[t], H[t], pr[t-st]); // for debugging
	}
	kfree(km, mem); kfree(km, H);
	if (with_cigar) { // backtrack
		if (ez->score > KSW_NEG_INF) ksw_backtrack(km, 1, p, off, n_col, tlen-1, qlen-1, &ez->m_cigar, &ez->n_cigar, &ez->cigar);
		else ksw_backtrack(km, 1, p, off, n_col, ez->max_t, ez->max_q, &ez->m_cigar, &ez->n_cigar, &ez->cigar);
		kfree(km, p); kfree(km, off);
	}
}
#endif // __SSE2__

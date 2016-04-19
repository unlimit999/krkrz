

#define _USE_MATH_DEFINES

#include "tjsCommHead.h"
#include "LayerBitmapIntf.h"
#include "LayerBitmapImpl.h"
#include "ThreadIntf.h"

#include <float.h>
#include <math.h>
#include <cmath>
#include <vector>

#include <xmmintrin.h> // SSE
#include <emmintrin.h> // SSE2
#include <immintrin.h> // AVX/AVX2

#include "x86simdutil.h"
#include "aligned_allocator.h"

#include "WeightFunctorAVX.h"
#include "ResampleImageInternal.h"

static const __m256 M256_PS_STEP( _mm256_set_ps(7.0f,6.0f,5.0f,4.0f,3.0f,2.0f,1.0f,0.0f) );
static const __m256 M256_PS_8_0( _mm256_set1_ps( 8.0f ) );
static const __m256 M256_PS_FIXED15( _mm256_set1_ps( (float)(1<<15) ) );
static const __m256i M256_U32_FIXED_ROUND( (_mm256_set1_epi32(0x00200020)) );
static const __m256i M256_U32_FIXED_COLOR_MASK( (_mm256_set1_epi32(0x00ff00ff)) );
static const __m256i M256_U32_FIXED_COLOR_MASK8( (_mm256_set1_epi32(0x000000ff)) );
static const __m256i M256_U32_TOP_MASK( _mm256_set_epi32(0, 0, 0, 0, 0, 0, 0, 0xffffffff) );
static const __m256 M256_EPSILON( _mm256_set1_ps( FLT_EPSILON ) );
static const __m256 M256_ABS_MASK( _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff)) );

void TJS_USERENTRY ResamplerAVX2FixFunc( void* p );
void TJS_USERENTRY ResamplerAVX2Func( void* p );

template<typename TWeight>
struct AxisParamAVX2 {
	std::vector<int> start_;	// �J�n�C���f�b�N�X
	std::vector<int> length_;	// �e�v�f����
	std::vector<int> length_min_;	// �e�v�f����, �A���C�����g������Ă��Ȃ��ŏ�����
	std::vector<TWeight,aligned_allocator<TWeight,32> > weight_;

	/**
	 * �͂ݏo���Ă��镔�����J�b�g����
	 */
	static inline void calculateEdge( float* weight, int& len, int leftedge, int rightedge ) {
		// ���[or�E�[�̎��A�͂ݏo�����̃E�F�C�g��[�ɉ��Z����
		if( leftedge ) {
			// ���[����͂ݏo���������Z
			int i = 1;
			for( ; i <= leftedge; i++ ) {
				weight[0] += weight[i];
			}
			// ���Z���������ړ�
			for( int j = 1; i < len; i++, j++ ) {
				weight[j] = weight[i];
			}
			// �͂ݏo�������̒������J�b�g
			len -= leftedge;
		}
		if( rightedge ) {
			// �E�[����͂ݏo���������Z
			int i = len - rightedge;
			int r = i - 1;
			for( ; i < len; i++ ) {
				weight[r] += weight[i];
			}
			// �͂ݏo�������̒������J�b�g
			len -= rightedge;
		}
	}
	// ���v�l�����߂�
	static inline __m256 sumWeight( float* weight, int len8 ) {
		float* w = weight;
		__m256 sum = _mm256_setzero_ps();
		for( int i = 0; i < len8; i+=8 ) {
			__m256 weight8 = _mm256_load_ps( w );
			sum = _mm256_add_ps( sum, weight8 );
			w += 8;
		}
		return m256_hsum_avx_ps(sum);
	}
	static inline void normalizeAndFixed( float* weight, tjs_uint32*& output, int& len, int len8, bool strip ) {
		// ���v�l�����߂�
		__m256 sum = sumWeight( weight, len8 );

		// EPSILON ��菬�����ꍇ�� 0 ��ݒ�
		const __m256 one = M256_PS_FIXED15; // �����t�Ȃ̂ŁB���Ɛ��K������Ă��邩��A�ő�l��1�ɂȂ�
		__m256 onemask = _mm256_cmp_ps( sum, M256_EPSILON, _CMP_GT_OS ); // sum > FLT_EPSILON ? 0xffffffff : 0; _mm_cmpgt_ps
		__m256 rcp = m256_rcp_22bit_ps( sum );
		rcp = _mm256_mul_ps( rcp, one );	// ��ɃV�t�g�����|���Ă���
		rcp = _mm256_and_ps( rcp, onemask );
		float* w = weight;
		// ���K���ƌŒ菬���_��
		for( int i = 0; i < len8; i+=8 ) {
			__m256 weight8 = _mm256_load_ps( w ); w += 8;
			weight8 = _mm256_mul_ps( weight8, rcp );

			// �Œ菬���_��
			__m256i fix = _mm256_cvtps_epi32( weight8 );
			fix = _mm256_packs_epi32( fix, fix );		// 16bit�� [01 02 03 04 01 02 03 04]
			fix = _mm256_unpacklo_epi16( fix, fix );	// 01 01 02 02 03 03 04 04
			//_mm256_store_si256( (__m256i*)output, fix );	// tjs_uint32 �� short*2 �œ����l���i�[����
			_mm256_storeu_si256( (__m256i*)output, fix );	// tjs_uint32 �� short*2 �œ����l���i�[����
			output += 8;
		}
		if( strip ) {
			output -= len8-len;
		}
	}
	static inline void calculateWeight( float* weight, tjs_uint32*& output, int& len, int leftedge, int rightedge, bool strip=false ) {
		// len �ɂ͂͂ݏo���������܂܂�Ă���̂ŁA�܂��͂��̕������J�b�g����
		calculateEdge( weight, len, leftedge, rightedge );

		// 8 �̔{����
		int len8 = ((len+7)>>3)<<3;

		// �_�~�[������0�ɐݒ�
		for( int i = len; i < len8; i++ ) weight[i] = 0.0f;

		// ���K���ƌŒ菬���_��
		normalizeAndFixed( weight, output, len, len8, strip );
	}
	static inline void normalize( float* weight, float*& output, int& len, int len8, bool strip ) {
		// ���v�l�����߂�
		__m256 sum = sumWeight( weight, len8 );

		// EPSILON ��菬�����ꍇ�� 0 ��ݒ�
		__m256 onemask = _mm256_cmp_ps( sum, M256_EPSILON, _CMP_GT_OS ); // sum > FLT_EPSILON ? 0xffffffff : 0; _mm_cmpgt_ps
		__m256 rcp = m256_rcp_22bit_ps( sum );
		rcp = _mm256_and_ps( rcp, onemask );
		float* w = weight;
		// ���K��
		for( int i = 0; i < len8; i+=8 ) {
			__m256 weight8 = _mm256_load_ps( w ); w += 8;
			weight8 = _mm256_mul_ps( weight8, rcp );
			_mm256_storeu_ps( (float*)output, weight8 );
			output += 8;
		}
		if( strip ) {
			output -= len8-len;
		}
	}
	static inline void calculateWeight( float* weight, float*& output, int& len, int leftedge, int rightedge, bool strip=false ) {
		// len �ɂ͂͂ݏo���������܂܂�Ă���̂ŁA�܂��͂��̕������J�b�g����
		calculateEdge( weight, len, leftedge, rightedge );

		// 8 �̔{����
		int len8 = ((len+7)>>3)<<3;

		// �_�~�[������0�ɐݒ�
		for( int i = len; i < len8; i++ ) weight[i] = 0.0f;

		// ���K���ƌŒ菬���_��
		normalize( weight, output, len, len8, strip );
	}

	template<typename TWeightFunc>
	void calculateAxis( int srcstart, int srcend, int srclength, int dstlength, float tap, bool strip, TWeightFunc& func) {
		start_.clear();
		start_.reserve( dstlength );
		length_.clear();
		length_.reserve( dstlength );
		length_min_.clear();
		length_min_.reserve( dstlength );
		// �܂��͋������v�Z
		// left/right������O�ɏo���Ə������������Ȃ�Ƃ͎v�������x���Ȃ����Ainline������Ȃ��̂�������Ȃ�
		if( srclength <= dstlength ) { // �g��
			float rangex = tap;
			int maxrange = ((((int)rangex*2+2)+7)>>3)<<3;
			std::vector<float,aligned_allocator<float,32> > work( maxrange, 0.0f );
			float* weight = &work[0];
			int length = (((dstlength * maxrange + dstlength)+7)>>3)<<3;
#ifdef _DEBUG
			weight_.resize( length );
#else
			weight_.reserve( length );
#endif
			const __m256 delta8 = M256_PS_8_0;
			const __m256 deltafirst = M256_PS_STEP;//_mm256_set_ps( 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f );
			const __m256 absmask = M256_ABS_MASK;
			TWeight* output = &weight_[0];
			for( int x = 0; x < dstlength; x++ ) {
				float cx = (x+0.5f)*(float)srclength/(float)dstlength + srcstart;
				int left = (int)std::floor(cx-rangex);
				int right = (int)std::floor(cx+rangex);
				int start = left;
				int leftedge = 0;
				if( left < srcstart ) {
					leftedge = srcstart - left;
					start = srcstart;
				}
				int rightedge = 0;
				if( right >= srcend ) {
					rightedge = right - srcend;
				}
				start_.push_back( start );
				int len = right - left;
				__m256 dist8 = _mm256_set1_ps((float)left+0.5f-cx);
				int len8 = ((len+7)>>3)<<3;	// 8 �̔{����
				float* w = weight;
				// �܂��͍ŏ��̗v�f�̂ݏ�������
				dist8 = _mm256_add_ps( dist8, deltafirst );
				_mm256_store_ps( w, func( _mm256_and_ps( dist8, absmask ) ) );	// ��Βl+weight�v�Z
				w += 8;
				for( int sx = 8; sx < len8; sx+=8 ) {
					dist8 = _mm256_add_ps( dist8, delta8 );	// 8���X���C�h
					_mm256_store_ps( w, func( _mm256_and_ps( dist8, absmask ) ) );	// ��Βl+weight�v�Z
					w += 8;
				}
				calculateWeight( weight, output, len, leftedge, rightedge, strip );
				len8 = ((len+7)>>3)<<3;
				if( strip ) {
					length_.push_back( len );
					length_min_.push_back( len );
				} else {
					length_.push_back( len8 );
					length_min_.push_back( len );
				}
			}
		} else { // �k��
			float rangex = tap*(float)srclength/(float)dstlength;
			int maxrange = ((((int)rangex*2+2)+7)>>3)<<3;
			std::vector<float,aligned_allocator<float,32> > work( maxrange, 0.0f );
			float* weight = &work[0];
			int length = (((srclength * maxrange + srclength)+7)>>3)<<3;
#ifdef _DEBUG
			weight_.resize( length );
#else
			weight_.reserve( length );
#endif
			TWeight* output = &weight_[0];
			const float delta = (float)dstlength/(float)srclength; // �]������W�ł̈ʒu����

			__m256 delta8 = _mm256_set1_ps(delta);
			__m256 deltafirst = M256_PS_STEP;//_mm256_set_ps( 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f );
			const __m256 absmask = M256_ABS_MASK;
			deltafirst = _mm256_mul_ps( deltafirst, delta8 );	// 0 1 2 3 �Ə��ɉ��Z�����悤�ɂ���
			// 8�{����
			delta8 = _mm256_add_ps( delta8, delta8 );
			delta8 = _mm256_add_ps( delta8, delta8 );
			delta8 = _mm256_add_ps( delta8, delta8 );
			for( int x = 0; x < dstlength; x++ ) {
				float cx = (x+0.5f)*(float)srclength/(float)dstlength + srcstart;
				int left = (int)std::floor(cx-rangex);
				int right = (int)std::floor(cx+rangex);
				int start = left;
				int leftedge = 0;
				if( left < srcstart ) {
					leftedge = srcstart - left;
					start = srcstart;
				}
				int rightedge = 0;
				if( right >= srcend ) {
					rightedge = right - srcend;
				}
				start_.push_back( start );
				// �]������W�ł̈ʒu
				int len = right-left;
				float dx = (left+0.5f) * delta -(x+0.5f);
				__m256 dist8 = _mm256_set1_ps(dx);
				int len8 = ((len+7)>>3)<<3;	// 8 �̔{����
				float* w = weight;
				// �܂��͍ŏ��̗v�f�̂ݏ�������
				dist8 = _mm256_add_ps( dist8, deltafirst );
				_mm256_store_ps( w, func( _mm256_and_ps( dist8, absmask ) ) );	// ��Βl+weight�v�Z
				w += 8;
				for( int sx = 8; sx < len8; sx+=8 ) {
					dist8 = _mm256_add_ps( dist8, delta8 );	// 8���X���C�h
					_mm256_store_ps( w, func( _mm256_and_ps( dist8, absmask ) ) );	// ��Βl+weight�v�Z
					w += 8;
				}
				calculateWeight( weight, output, len, leftedge, rightedge, strip );
				len8 = ((len+7)>>3)<<3;
				if( strip ) {
					length_.push_back( len );
					length_min_.push_back( len );
				} else {
					length_.push_back( len8 );
					length_min_.push_back( len );
				}
			}
		}
	}
	// ���v�l�����߂�
	static inline __m256 sumWeightUnalign( float* weight, int len8 ) {
		float* w = weight;
		__m256 sum = _mm256_setzero_ps();
		for( int i = 0; i < len8; i+=8 ) {
			__m256 weight8 = _mm256_loadu_ps( w );
			sum = _mm256_add_ps( sum, weight8 );
			w += 8;
		}
		return m256_hsum_avx_ps(sum);
	}
	// ���K��
	void normalizeAreaAvg( float* wstart, float* dweight, tjs_uint size, bool strip ) {
		const int count = (const int)length_.size();
		int dwindex = 0;
		const __m256 epsilon = M256_EPSILON;
		for( int i = 0; i < count; i++ ) {
			float* dw = dweight;
			int len = length_[i];
			float* w = wstart;
			int len8 = ((len+7)>>3)<<3;	// 8 �̔{����
			int idx = 0;
			for( ; idx < len; idx++ ) {
				*dw = *w;
				dw++;
				w++;
			}
			wstart = w;
			w = dweight;
			// �A���C�����g
			for( ; idx < len8; idx++ ) {
				*dw = 0.0f;
				dw++;
			}
			dweight = dw;

			// ���v�l�����߂�
			__m256 sum;
			if( strip ) {
				sum = sumWeightUnalign( w, len8 );
			} else {
				sum = sumWeight( w, len8 );
			}

			// EPSILON ��菬�����ꍇ�� 0 ��ݒ�
			__m256 onemask = _mm256_cmp_ps( sum, epsilon, _CMP_GT_OS ); // sum > FLT_EPSILON ? 0xffffffff : 0; _mm_cmpgt_ps
			__m256 rcp = m256_rcp_22bit_ps( sum );
			rcp = _mm256_and_ps( rcp, onemask );
			// ���K��
			for( int j = 0; j < len8; j += 8 ) {
				__m256 weight8 = _mm256_loadu_ps( w );
				weight8 = _mm256_mul_ps( weight8, rcp );
				_mm256_storeu_ps( (float*)w, weight8 );
				w += 8;
			}
			if( strip ) {
				dweight -= len8-len;
				length_min_.push_back( len );
			} else {
				length_[i] = len8;
				length_min_.push_back( len );
			}
		}
	}
	void normalizeAreaAvg( float* wstart, tjs_uint32* dweight, tjs_uint size,  bool strip ) {
		const int count = (const int)length_.size();
#ifdef _DEBUG
		std::vector<float,aligned_allocator<float,32> > work(size);
#else
		std::vector<float,aligned_allocator<float,32> > work;
		work.reserve( size );
#endif
		int dwindex = 0;
		const __m256 one = M256_PS_FIXED15; // �����t�Ȃ̂ŁB���Ɛ��K������Ă��邩��A�ő�l��1�ɂȂ�
		const __m256 epsilon = M256_EPSILON;
		for( int i = 0; i < count; i++ ) {
			float* dw = &work[0];
			int len = length_[i];
			float* w = wstart;
			int len8 = ((len+7)>>3)<<3;	// 8 �̔{����
			int idx = 0;
			for( ; idx < len; idx++ ) {
				*dw = *w;
				dw++;
				w++;
			}
			wstart = w;
			w = &work[0];
			// �A���C�����g
			for( ; idx < len8; idx++ ) {
				*dw = 0.0f;
				dw++;
			}

			// ���v�l�����߂�
			__m256 sum = sumWeight( w, len8 );

			// EPSILON ��菬�����ꍇ�� 0 ��ݒ�
			__m256 onemask = _mm256_cmp_ps( sum, epsilon, _CMP_GT_OS ); // sum > FLT_EPSILON ? 0xffffffff : 0; _mm_cmpgt_ps
			__m256 rcp = m256_rcp_22bit_ps( sum );
			rcp = _mm256_mul_ps( rcp, one );	// ��ɃV�t�g�����|���Ă���
			rcp = _mm256_and_ps( rcp, onemask );
			// ���K��
			for( int j = 0; j < len8; j += 8 ) {
				__m256 weight8 = _mm256_load_ps( w ); w += 8;
				weight8 = _mm256_mul_ps( weight8, rcp );
				// �Œ菬���_��
				__m256i fix = _mm256_cvtps_epi32( weight8 );
				fix = _mm256_packs_epi32( fix, fix );		// 16bit�� [01 02 03 04 01 02 03 04]
				fix = _mm256_unpacklo_epi16( fix, fix );	// 01 01 02 02 03 03 04 04
				_mm256_storeu_si256( (__m256i*)dweight, fix );	// tjs_uint32 �� short*2 �œ����l���i�[����
				dweight += 8;
			}
			if( strip ) {
				dweight -= len8-len;
				length_min_.push_back( len );
			} else {
				length_[i] = len8;
				length_min_.push_back( len );
			}
		}
	}
	void calculateAxisAreaAvg( int srcstart, int srcend, int srclength, int dstlength, bool strip ) {
		if( dstlength < srclength ) { // �k���̂�
			std::vector<float> weight;
			TVPCalculateAxisAreaAvg( srcstart, srcend, srclength, dstlength, start_, length_, weight );
			// ���ۂ̃T�C�Y�����߂�
			int maxsize = 0;
			if( strip == false ) {
				int count = (int)length_.size();
				for( int i = 0; i < count; i++ ) {
					int len = length_[i];
					maxsize += ((len+7)>>3)<<3;	// 8 �̔{����
				}
			} else {
				maxsize = (int)weight.size();
			}
#ifdef _DEBUG
			weight_.resize( maxsize+7 );
#else
			weight_.reserve( maxsize+7 );
#endif
			normalizeAreaAvg( &weight[0], &weight_[0], maxsize+7, strip );
		}
	}
	
};

class ResamplerAVX2Fix {
	AxisParamAVX2<tjs_uint32> paramx_;
	AxisParamAVX2<tjs_uint32> paramy_;

public:
	/**
	 * �}���`�X���b�h���p
	 */
	struct ThreadParameterHV {
		ResamplerAVX2Fix* sampler_;
		int start_;
		int end_;
		int alingnwidth_;
		
		const tjs_uint32* wstarty_;
		const tTVPBaseBitmap* src_;
		const tTVPRect* srcrect_;
		tTVPBaseBitmap* dest_;
		const tTVPRect* destrect_;

		const tTVPResampleClipping* clip_;
		const tTVPImageCopyFuncBase* blendfunc_;
	};
	/**
	 * �������̏�������ɂ����ꍇ�̎���
	 */
	inline void samplingHorizontal( tjs_uint32* dstbits, const int offsetx, const int dstwidth, const tjs_uint32* srcbits ) {
		const tjs_uint32* weightx = &paramx_.weight_[0];
		// �܂�offset�����X�L�b�v
		for( int x = 0; x < offsetx; x++ ) {
			weightx += paramx_.length_[x];
		}
		const tjs_uint32* src = srcbits;
		const __m256i cmask = M256_U32_FIXED_COLOR_MASK;
		const __m256i storemask = M256_U32_TOP_MASK;
		const __m256i fixround = M256_U32_FIXED_ROUND;
		for( int x = offsetx; x < dstwidth; x++ ) {
			const int left = paramx_.start_[x];
			int right = left + paramx_.length_[x];
			__m256i color_lo = _mm256_setzero_si256();
			__m256i color_hi = _mm256_setzero_si256();
			// 8�s�N�Z������������
			for( int sx = left; sx < right; sx+=8 ) {
				__m256i col8 = _mm256_loadu_si256( (const __m256i*)&src[sx] ); // 8�s�N�Z���ǂݍ���
				__m256i weight8 = _mm256_loadu_si256( (const __m256i*)weightx ); // �E�F�C�g(�Œ菭��)8��(16bit��16)�ǂݍ��� 0 1 2 3 �A���C�����g�ς�
				weightx += 8;

				__m256i col = _mm256_and_si256( col8, cmask );	// 00 RR 00 BB & 0x00ff00ff
				col = _mm256_slli_epi16( col, 7 );	// << 7
				col = _mm256_mulhi_epi16( col, weight8 );
				color_lo = _mm256_adds_epi16( color_lo, col );

				col = _mm256_srli_epi16( col8, 8 );	// 00 AA 00 GG
				col = _mm256_slli_epi16( col, 7 );	// << 7
				col = _mm256_mulhi_epi16( col, weight8 );
				color_hi = _mm256_adds_epi16( color_hi, col );
			}
			{	// AVX - �������Z
				__m256i sumlo = color_lo;
				color_lo = _mm256_shuffle_epi32( color_lo, _MM_SHUFFLE(1,0,3,2) ); // 0 1 2 3 + 1 0 3 2
				sumlo = _mm256_adds_epi16( sumlo, color_lo );
				color_lo = _mm256_shuffle_epi32( sumlo, _MM_SHUFFLE(2,3,0,1) ); // 3 2 1 0
				sumlo = _mm256_adds_epi16( sumlo, color_lo );
				color_lo = _mm256_permute2x128_si256( sumlo, sumlo, 0 << 4 | 1 ); // 128bit �őO�㔽�]
				sumlo = _mm256_adds_epi16( sumlo, color_lo );
				sumlo = _mm256_adds_epi16( sumlo, fixround );
				sumlo = _mm256_srai_epi16( sumlo, 6 ); // �Œ菬���_���琮���� - << 15, << 7, >> 16 = 6

				__m256i sumhi = color_hi;
				color_hi = _mm256_shuffle_epi32( color_hi, _MM_SHUFFLE(1,0,3,2) ); // 0 1 2 3 + 1 0 3 2
				sumhi = _mm256_adds_epi16( sumhi, color_hi );
				color_hi = _mm256_shuffle_epi32( sumhi, _MM_SHUFFLE(2,3,0,1) ); // 3 2 1 0
				sumhi = _mm256_adds_epi16( sumhi, color_hi );
				color_hi = _mm256_permute2x128_si256( sumhi, sumhi, 0 << 4 | 1 ); // 128bit �őO�㔽�]
				sumhi = _mm256_adds_epi16( sumhi, color_hi );
				sumhi = _mm256_adds_epi16( sumhi, fixround );
				sumhi = _mm256_srai_epi16( sumhi, 6 ); // �Œ菬���_���琮����

				sumlo = _mm256_unpacklo_epi16( sumlo, sumhi );
				sumlo = _mm256_packus_epi16( sumlo, sumlo );
				_mm256_maskstore_epi32( (int*)dstbits, storemask, sumlo );
			}
			dstbits++;
		}
	}
	/**
	 * 8���C���ł́A�c������Ă��牡�����Ƃ����̂���
	 * ��������A�����ǂݍ��݂��o���邩
	 * @dstheight : �k����̏c�̒���
	 */
	inline void samplingVertical( int y, tjs_uint32* dstbits, int dstheight, int srcwidth, const tTVPBaseBitmap *src, const tTVPRect &srcrect, const tjs_uint32*& wstarty ) {
		const int top = paramy_.start_[y];
		const int len = paramy_.length_min_[y];
		const int bottom = top + len;
		const tjs_uint32* weighty = wstarty;
		const __m256i cmask = M256_U32_FIXED_COLOR_MASK;
		const __m256i fixround = M256_U32_FIXED_ROUND;
		const tjs_uint32* srctop = (const tjs_uint32*)src->GetScanLine(top+srcrect.top) + srcrect.left;
		tjs_int stride = src->GetPitchBytes()/(int)sizeof(tjs_uint32);
		for( int x = 0; x < srcwidth; x+=8 ) {
			weighty = wstarty;
			__m256i color_lo = _mm256_setzero_si256();
			__m256i color_hi = _mm256_setzero_si256();
			const tjs_uint32* srcbits = &srctop[x];
			for( int sy = top; sy < bottom; sy++ ) {
				__m256i col8 = _mm256_loadu_si256( (const __m256i*)srcbits ); // 8��ǂݍ���
				srcbits += stride;
				__m256i weight8 = _mm256_set1_epi32( (int)*weighty ); // weight �́A�����l��ݒ�
				weighty++;

				__m256i col = _mm256_and_si256( col8, cmask );	// 00 RR 00 BB
				col = _mm256_slli_epi16( col, 7 );	// << 7
				col = _mm256_mulhi_epi16( col, weight8 );
				color_lo = _mm256_adds_epi16( color_lo, col );

				col = _mm256_srli_epi16( col8, 8 );	// 00 AA 00 GG
				col = _mm256_slli_epi16( col, 7 );	// << 7
				col = _mm256_mulhi_epi16( col, weight8 );
				color_hi = _mm256_adds_epi16( color_hi, col );
			}
			{
				color_lo = _mm256_adds_epi16( color_lo, fixround );
				color_hi = _mm256_adds_epi16( color_hi, fixround );
				color_lo = _mm256_srai_epi16( color_lo, 6 ); // �Œ菬���_���琮���� - << 15, << 7, >> 16 = 6
				color_hi = _mm256_srai_epi16( color_hi, 6 ); // �Œ菬���_���琮����
				__m256i lo = _mm256_unpacklo_epi16( color_lo, color_hi );
				__m256i hi = _mm256_unpackhi_epi16( color_lo, color_hi );
				color_lo = _mm256_packus_epi16( lo, hi );
				//_mm256_storeu_si256( (__m256i *)&dstbits[x], color_lo );
				_mm256_store_si256( (__m256i *)&dstbits[x], color_lo );
			}
		}
		wstarty = weighty;
	}

	void ResampleImage( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
		const int srcwidth = srcrect.get_width();
		const int dstheight = destrect.get_height();
		const int alingnwidth = (((srcwidth+7)>>3)<<3) + 7;
#ifdef _DEBUG
		std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work( alingnwidth );
#else
		std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work;
		work.reserve( alingnwidth );
#endif
		const tjs_uint32* wstarty = &paramy_.weight_[0];
		// �N���b�s���O�����X�L�b�v
		for( int y = 0; y < clip.offsety_; y++ ) {
			wstarty += paramy_.length_[y];
		}
		tjs_uint32* workbits = &work[0];
		tjs_int dststride = dest->GetPitchBytes()/(int)sizeof(tjs_uint32);
		tjs_uint32* dstbits = (tjs_uint32*)dest->GetScanLineForWrite(clip.dst_top_) + clip.dst_left_;
		if( blendfunc == NULL ) {
			for( int y = clip.offsety_; y < clip.height_; y++ ) {
				samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
				samplingHorizontal( dstbits, clip.offsetx_, clip.width_, workbits );
				dstbits += dststride;
			}
		} else {	// �P���R�s�[�ȊO�́A��x�e���|�����ɏ����o���Ă��獇������
#ifdef _DEBUG
			std::vector<tjs_uint32> dstwork(clip.getDestWidth()+7);
#else
			std::vector<tjs_uint32> dstwork;
			dstwork.reserve( clip.getDestWidth()+7 );
#endif
			tjs_uint32* midbits = &dstwork[0];	// �r�������p�o�b�t�@
			for( int y = clip.offsety_; y < clip.height_; y++ ) {
				samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
				samplingHorizontal( midbits, clip.offsetx_, clip.width_, workbits ); // �ꎞ�o�b�t�@�ɂ܂��R�s�[, �͈͊O�͏������Ȃ�
				(*blendfunc)( dstbits, midbits, clip.getDestWidth() );
				dstbits += dststride;
			}
		}
	}
	void ResampleImageMT( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, tjs_int threadNum ) {
		const int srcwidth = srcrect.get_width();
		const int alingnwidth = ((srcwidth+7)>>3)<<3;
		const tjs_uint32* wstarty = &paramy_.weight_[0];
		// �N���b�s���O�����X�L�b�v
		for( int y = 0; y < clip.offsety_; y++ ) {
			wstarty += paramy_.length_[y];
		}
		int offset = clip.offsety_;
		const int height = clip.getDestHeight();

		TVPBeginThreadTask(threadNum);
		std::vector<ThreadParameterHV> params(threadNum);
		for( int i = 0; i < threadNum; i++ ) {
			ThreadParameterHV* param = &params[i];
			param->sampler_ = this;
			param->start_ = height * i / threadNum + offset;
			param->end_ = height * (i + 1) / threadNum + offset;
			param->alingnwidth_ = alingnwidth;
			param->wstarty_ = wstarty;
			param->src_ = src;
			param->srcrect_ = &srcrect;
			param->dest_ = dest;
			param->destrect_ = &destrect;
			param->clip_ = &clip;
			param->blendfunc_ = blendfunc;
			int top = param->start_;
			int bottom = param->end_;
			TVPExecThreadTask(&ResamplerAVX2FixFunc, TVP_THREAD_PARAM(param));
			if( i < (threadNum-1) ) {
				for( int y = top; y < bottom; y++ ) {
					int len = paramy_.length_[y];
					wstarty += len;
				}
			}
		}
		TVPEndThreadTask();
	}
public:
	template<typename TWeightFunc>
	void Resample( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, float tap, TWeightFunc& func ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();

		paramx_.calculateAxis( srcrect.left, srcrect.right, srcwidth, dstwidth, tap, false, func );
		paramy_.calculateAxis( srcrect.top, srcrect.bottom, srcheight, dstheight, tap, true, func );
		ResampleImage( clip, blendfunc, dest, destrect, src, srcrect );
	}
	template<typename TWeightFunc>
	void ResampleMT( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, float tap, TWeightFunc& func ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();
		int maxwidth = srcwidth > dstwidth ? srcwidth : dstwidth;
		int maxheight = srcheight > dstheight ? srcheight : dstheight;
		int threadNum = 1;
		int pixelNum = maxwidth*(int)tap*maxheight + maxheight*(int)tap*maxwidth;
		if( pixelNum >= 50 * 500 ) {
			threadNum = TVPGetThreadNum();
		}
		if( threadNum == 1 ) { // �ʐς����Ȃ��X���b�h��1�̎��͂��̂܂܎��s
			Resample( clip, blendfunc, dest, destrect, src, srcrect, tap, func );
			return;
		}

		paramx_.calculateAxis( srcrect.left, srcrect.right, srcwidth, dstwidth, tap, false, func );
		paramy_.calculateAxis( srcrect.top, srcrect.bottom, srcheight, dstheight, tap, true, func );
		ResampleImageMT( clip, blendfunc, dest, destrect, src, srcrect, threadNum );
	}
	void ResampleAreaAvg( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();
		if( dstwidth > srcwidth || dstheight > srcheight ) return;

		paramx_.calculateAxisAreaAvg( srcrect.left, srcrect.right, srcwidth, dstwidth, false );
		paramy_.calculateAxisAreaAvg( srcrect.top, srcrect.bottom, srcheight, dstheight, true );
		ResampleImage( clip, blendfunc, dest, destrect, src, srcrect );
	}
	void ResampleAreaAvgMT( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();
		if( dstwidth > srcwidth || dstheight > srcheight ) return;

		int maxwidth = srcwidth > dstwidth ? srcwidth : dstwidth;
		int maxheight = srcheight > dstheight ? srcheight : dstheight;
		int threadNum = 1;
		int pixelNum = maxwidth*maxheight;
		if( pixelNum >= 50 * 500 ) {
			threadNum = TVPGetThreadNum();
		}
		if( threadNum == 1 ) { // �ʐς����Ȃ��X���b�h��1�̎��͂��̂܂܎��s
			ResampleAreaAvg( clip, blendfunc, dest, destrect, src, srcrect );
			return;
		}

		paramx_.calculateAxisAreaAvg( srcrect.left, srcrect.right, srcwidth, dstwidth, false );
		paramy_.calculateAxisAreaAvg( srcrect.top, srcrect.bottom, srcheight, dstheight, true );
		ResampleImageMT( clip, blendfunc, dest, destrect, src, srcrect, threadNum );
	}
};

void TJS_USERENTRY ResamplerAVX2FixFunc( void* p ) {
	ResamplerAVX2Fix::ThreadParameterHV* param = (ResamplerAVX2Fix::ThreadParameterHV*)p;
	const int alingnwidth = param->alingnwidth_;
#ifdef _DEBUG
	std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work(alingnwidth);
#else
	std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work;
	work.reserve( alingnwidth );
#endif

	tTVPBaseBitmap* dest = param->dest_;
	const tTVPRect& destrect = *param->destrect_;
	const tTVPBaseBitmap* src = param->src_;
	const tTVPRect& srcrect = *param->srcrect_;

	const int srcwidth = srcrect.get_width();
	const int dstwidth = destrect.get_width();
	const int dstheight = destrect.get_height();
	const tjs_uint32* wstarty = param->wstarty_;
	tjs_uint32* workbits = &work[0];
	tjs_int dststride = dest->GetPitchBytes()/(int)sizeof(tjs_uint32);
	tjs_uint32* dstbits = (tjs_uint32*)dest->GetScanLineForWrite(param->start_+destrect.top) + param->clip_->dst_left_;
	if( param->blendfunc_ == NULL ) {
		for( int y = param->start_; y < param->end_; y++ ) {
			param->sampler_->samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
			param->sampler_->samplingHorizontal( dstbits, param->clip_->offsetx_, param->clip_->width_, workbits );
			dstbits += dststride;
		}
	} else {	// �P���R�s�[�ȊO
#ifdef _DEBUG
		std::vector<tjs_uint32> dstwork(param->clip_->getDestWidth()+7);
#else
		std::vector<tjs_uint32> dstwork;
		dstwork.reserve( param->clip_->getDestWidth()+7 );
#endif
		tjs_uint32* midbits = &dstwork[0];	// �r�������p�o�b�t�@
		for( int y = param->start_; y < param->end_; y++ ) {
			param->sampler_->samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
			param->sampler_->samplingHorizontal( midbits, param->clip_->offsetx_, param->clip_->width_, workbits ); // �ꎞ�o�b�t�@�ɂ܂��R�s�[, �͈͊O�͏������Ȃ�
			(*param->blendfunc_)( dstbits, midbits, param->clip_->getDestWidth() );
			dstbits += dststride;
		}
	}
}

class ResamplerAVX2 {
	AxisParamAVX2<float> paramx_;
	AxisParamAVX2<float> paramy_;

public:
	/** �}���`�X���b�h���p */
	struct ThreadParameterHV {
		ResamplerAVX2* sampler_;
		int start_;
		int end_;
		int alingnwidth_;
		
		const float* wstarty_;
		const tTVPBaseBitmap* src_;
		const tTVPRect* srcrect_;
		tTVPBaseBitmap* dest_;
		const tTVPRect* destrect_;

		const tTVPResampleClipping* clip_;
		const tTVPImageCopyFuncBase* blendfunc_;
	};
	/**
	 * �������̏��� (��ɏ���)
	 */
	inline void samplingHorizontal( tjs_uint32* dstbits, const int offsetx, const int dstwidth, const tjs_uint32* srcbits ) {
		const __m256i cmask = M256_U32_FIXED_COLOR_MASK8;	// 8bit�����邽�߂̃}�X�N
		const float* weightx = &paramx_.weight_[0];
		// �܂�offset�����X�L�b�v
		for( int x = 0; x < offsetx; x++ ) {
			weightx += paramx_.length_[x];
		}
		const tjs_uint32* src = srcbits;
		const __m256i storemask = M256_U32_TOP_MASK;
		const __m256i zero = _mm256_setzero_si256();
		for( int x = offsetx; x < dstwidth; x++ ) {
			const int left = paramx_.start_[x];
			int right = left + paramx_.length_[x];
			__m256 color_elm = _mm256_setzero_ps();
			// 8�s�N�Z������������
			for( int sx = left; sx < right; sx+=8 ) {
				__m256i col8 = _mm256_loadu_si256( (const __m256i*)&src[sx] ); // 8�s�N�Z���ǂݍ���
				__m256 weight8 = _mm256_loadu_ps( (const float*)weightx ); // �E�F�C�g8��
				weightx += 8;

				// a r g b | a r g b �� 2���������邩��Aweight �����̌`�ɃC���^�[���[�u
				__m256i collo = _mm256_unpacklo_epi8( col8, zero );		// 00 01 00 02 00 03 0 04 00 05 00 06...
				__m256i col = _mm256_unpacklo_epi16( collo, zero );		// 00 00 00 01 00 00 00 02...
				__m256 colf = _mm256_cvtepi32_ps( col );
				__m256 wlo = _mm256_unpacklo_ps( weight8, weight8 );
				__m256 w = _mm256_unpacklo_ps( wlo, wlo );	// 00 00 00 00 04 04 04 04
				colf = _mm256_mul_ps( colf, w );
				color_elm = _mm256_add_ps( color_elm, colf );
				
				col = _mm256_unpackhi_epi16( collo, zero );		// 00 00 00 01 00 00 00 02...
				colf = _mm256_cvtepi32_ps( col );				// int to float
				w = _mm256_unpackhi_ps( wlo, wlo );
				colf = _mm256_mul_ps( colf, w );
				color_elm = _mm256_add_ps( color_elm, colf );
				
				__m256i colhi = _mm256_unpackhi_epi8( col8, zero );	// 00 01 00 02 00 03 0 04 00 05 00 06...
				col = _mm256_unpacklo_epi16( colhi, zero );			// 00 00 00 01 00 00 00 02...
				colf = _mm256_cvtepi32_ps( col );					// int to float
				__m256 whi = _mm256_unpackhi_ps( weight8, weight8 );
				w = _mm256_unpacklo_ps( whi, whi );
				colf = _mm256_mul_ps( colf, w );
				color_elm = _mm256_add_ps( color_elm, colf );
				
				col = _mm256_unpackhi_epi16( colhi, zero );		// 00 00 00 01 00 00 00 02...
				colf = _mm256_cvtepi32_ps( col );				// int to float
				w = _mm256_unpackhi_ps( whi, whi );
				colf = _mm256_mul_ps( colf, w );
				color_elm = _mm256_add_ps( color_elm, colf );
			}
			{	// AVX - �O��128bit���Z
				__m256 color_rev = _mm256_permute2f128_ps(color_elm, color_elm, 0 << 4 | 1 );	// �O����ꂩ��
				color_elm = _mm256_add_ps( color_elm, color_rev );
				__m256i color = _mm256_cvtps_epi32( color_elm );
				color = _mm256_packus_epi32( color, color );
				color = _mm256_packus_epi16( color, color );
				_mm256_maskstore_epi32( (int*)dstbits, storemask, color );
			}
			dstbits++;
		}
	}
	/**
	 * �c��������
	 */
	inline void samplingVertical( int y, tjs_uint32* dstbits, int dstheight, int srcwidth, const tTVPBaseBitmap *src, const tTVPRect &srcrect, const float*& wstarty ) {
		const int top = paramy_.start_[y];
		const int len = paramy_.length_min_[y];
		const int bottom = top + len;
		const float* weighty = wstarty;
		const __m256i cmask = M256_U32_FIXED_COLOR_MASK8;
		const tjs_uint32* srctop = (const tjs_uint32*)src->GetScanLine(top+srcrect.top) + srcrect.left;
		tjs_int stride = src->GetPitchBytes()/(int)sizeof(tjs_uint32);
		for( int x = 0; x < srcwidth; x+=8 ) {
			weighty = wstarty;
			__m256 color_a = _mm256_setzero_ps();
			__m256 color_r = _mm256_setzero_ps();
			__m256 color_g = _mm256_setzero_ps();
			__m256 color_b = _mm256_setzero_ps();
			const tjs_uint32* srcbits = &srctop[x];
			for( int sy = top; sy < bottom; sy++ ) {
				__m256i col8 = _mm256_loadu_si256( (const __m256i*)srcbits ); // 8��ǂݍ���
				srcbits += stride;
				__m256 weight8 = _mm256_set1_ps( *weighty ); // weight �́A�����l��ݒ�
				weighty++;
				
				__m256i c = _mm256_srli_epi32( col8, 24 );
				__m256 cf = _mm256_cvtepi32_ps(c);
				cf = _mm256_mul_ps( cf, weight8 );
				color_a = _mm256_add_ps( color_a, cf );

				c = _mm256_srli_epi32( col8, 16 );
				c = _mm256_and_si256( c, cmask );
				cf = _mm256_cvtepi32_ps(c);
				cf = _mm256_mul_ps( cf, weight8 );
				color_r = _mm256_add_ps( color_r, cf );
				
				c = _mm256_srli_epi32( col8, 8 );
				c = _mm256_and_si256( c, cmask );
				cf = _mm256_cvtepi32_ps(c);
				cf = _mm256_mul_ps( cf, weight8 );
				color_g = _mm256_add_ps( color_g, cf );

				c = _mm256_and_si256( col8, cmask );
				cf = _mm256_cvtepi32_ps(c);
				cf = _mm256_mul_ps( cf, weight8 );
				color_b = _mm256_add_ps( color_b, cf );
			}
			{
				__m256i a = _mm256_cvtps_epi32( color_a );
				__m256i r = _mm256_cvtps_epi32( color_r );
				__m256i g = _mm256_cvtps_epi32( color_g );
				__m256i b = _mm256_cvtps_epi32( color_b );
				// �C���^�[���[�u
				__m256i arl = _mm256_unpacklo_epi32( r, a );
				__m256i arh = _mm256_unpackhi_epi32( r, a );
				arl = _mm256_packs_epi32( arl, arh );	// a r a r a r ar
				__m256i gbl = _mm256_unpacklo_epi32( b, g );
				__m256i gbh = _mm256_unpackhi_epi32( b, g );
				gbl = _mm256_packs_epi32( gbl, gbh );	// g b g b g b g b
				__m256i l = _mm256_unpacklo_epi32( gbl, arl );
				__m256i h = _mm256_unpackhi_epi32( gbl, arl );
				l = _mm256_packus_epi16( l, h );
				_mm256_store_si256( (__m256i *)&dstbits[x], l );
			}
		}
		wstarty = weighty;
	}
	
	void ResampleImage( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
		const int srcwidth = srcrect.get_width();
		const int dstheight = destrect.get_height();
		const int alingnwidth = (((srcwidth+7)>>3)<<3) + 7;
#ifdef _DEBUG
		std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work( alingnwidth );
#else
		std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work;
		work.reserve( alingnwidth );
#endif
		const float* wstarty = &paramy_.weight_[0];
		// �N���b�s���O�����X�L�b�v
		for( int y = 0; y < clip.offsety_; y++ ) {
			wstarty += paramy_.length_[y];
		}
		tjs_uint32* workbits = &work[0];
		tjs_int dststride = dest->GetPitchBytes()/(int)sizeof(tjs_uint32);
		tjs_uint32* dstbits = (tjs_uint32*)dest->GetScanLineForWrite(clip.dst_top_) + clip.dst_left_;
		if( blendfunc == NULL ) {
			for( int y = clip.offsety_; y < clip.height_; y++ ) {
				samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
				samplingHorizontal( dstbits, clip.offsetx_, clip.width_, workbits );
				dstbits += dststride;
			}
		} else {	// �P���R�s�[�ȊO�́A��x�e���|�����ɏ����o���Ă��獇������
#ifdef _DEBUG
			std::vector<tjs_uint32> dstwork(clip.getDestWidth()+7);
#else
			std::vector<tjs_uint32> dstwork;
			dstwork.reserve( clip.getDestWidth()+7 );
#endif
			tjs_uint32* midbits = &dstwork[0];	// �r�������p�o�b�t�@
			for( int y = clip.offsety_; y < clip.height_; y++ ) {
				samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
				samplingHorizontal( midbits, clip.offsetx_, clip.width_, workbits ); // �ꎞ�o�b�t�@�ɂ܂��R�s�[, �͈͊O�͏������Ȃ�
				(*blendfunc)( dstbits, midbits, clip.getDestWidth() );
				dstbits += dststride;
			}
		}
	}
	void ResampleImageMT( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, tjs_int threadNum ) {
		const int srcwidth = srcrect.get_width();
		const int alingnwidth = ((srcwidth+7)>>3)<<3;
		const float* wstarty = &paramy_.weight_[0];
		// �N���b�s���O�����X�L�b�v
		for( int y = 0; y < clip.offsety_; y++ ) {
			wstarty += paramy_.length_[y];
		}
		int offset = clip.offsety_;
		const int height = clip.getDestHeight();

		TVPBeginThreadTask(threadNum);
		std::vector<ThreadParameterHV> params(threadNum);
		for( int i = 0; i < threadNum; i++ ) {
			ThreadParameterHV* param = &params[i];
			param->sampler_ = this;
			param->start_ = height * i / threadNum + offset;
			param->end_ = height * (i + 1) / threadNum + offset;
			param->alingnwidth_ = alingnwidth;
			param->wstarty_ = wstarty;
			param->src_ = src;
			param->srcrect_ = &srcrect;
			param->dest_ = dest;
			param->destrect_ = &destrect;
			param->clip_ = &clip;
			param->blendfunc_ = blendfunc;
			int top = param->start_;
			int bottom = param->end_;
			TVPExecThreadTask(&ResamplerAVX2Func, TVP_THREAD_PARAM(param));
			if( i < (threadNum-1) ) {
				for( int y = top; y < bottom; y++ ) {
					int len = paramy_.length_[y];
					wstarty += len;
				}
			}
		}
		TVPEndThreadTask();
	}
public:
	/** 8���C������������ */
	template<typename TWeightFunc>
	void Resample( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, float tap, TWeightFunc& func ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();
		paramx_.calculateAxis( srcrect.left, srcrect.right, srcwidth, dstwidth, tap, false, func );
		paramy_.calculateAxis( srcrect.top, srcrect.bottom, srcheight, dstheight, tap, true, func );
		ResampleImage( clip, blendfunc, dest, destrect, src, srcrect );
	}
	template<typename TWeightFunc>
	void ResampleMT( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, float tap, TWeightFunc& func ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();
		int maxwidth = srcwidth > dstwidth ? srcwidth : dstwidth;
		int maxheight = srcheight > dstheight ? srcheight : dstheight;
		int threadNum = 1;
		int pixelNum = maxwidth*(int)tap*maxheight + maxheight*(int)tap*maxwidth;
		if( pixelNum >= 50 * 500 ) {
			threadNum = TVPGetThreadNum();
		}
		if( threadNum == 1 ) { // �ʐς����Ȃ��X���b�h��1�̎��͂��̂܂܎��s
			Resample( clip, blendfunc, dest, destrect, src, srcrect, tap, func );
			return;
		}
		paramx_.calculateAxis( srcrect.left, srcrect.right, srcwidth, dstwidth, tap, false, func );
		paramy_.calculateAxis( srcrect.top, srcrect.bottom, srcheight, dstheight, tap, true, func );
		ResampleImageMT( clip, blendfunc, dest, destrect, src, srcrect, threadNum );
	}
	
	void ResampleAreaAvg( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();
		if( dstwidth > srcwidth || dstheight > srcheight ) return;
		paramx_.calculateAxisAreaAvg( srcrect.left, srcrect.right, srcwidth, dstwidth, false );
		paramy_.calculateAxisAreaAvg( srcrect.top, srcrect.bottom, srcheight, dstheight, true );
		ResampleImage( clip, blendfunc, dest, destrect, src, srcrect );
	}
	void ResampleAreaAvgMT( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
		const int srcwidth = srcrect.get_width();
		const int srcheight = srcrect.get_height();
		const int dstwidth = destrect.get_width();
		const int dstheight = destrect.get_height();
		if( dstwidth > srcwidth || dstheight > srcheight ) return;

		int maxwidth = srcwidth > dstwidth ? srcwidth : dstwidth;
		int maxheight = srcheight > dstheight ? srcheight : dstheight;
		int threadNum = 1;
		int pixelNum = maxwidth*maxheight;
		if( pixelNum >= 50 * 500 ) {
			threadNum = TVPGetThreadNum();
		}
		if( threadNum == 1 ) { // �ʐς����Ȃ��X���b�h��1�̎��͂��̂܂܎��s
			ResampleAreaAvg( clip, blendfunc, dest, destrect, src, srcrect );
			return;
		}
		paramx_.calculateAxisAreaAvg( srcrect.left, srcrect.right, srcwidth, dstwidth, false );
		paramy_.calculateAxisAreaAvg( srcrect.top, srcrect.bottom, srcheight, dstheight, true );
		ResampleImageMT( clip, blendfunc, dest, destrect, src, srcrect, threadNum );
	}
};

void TJS_USERENTRY ResamplerAVX2Func( void* p ) {
	ResamplerAVX2::ThreadParameterHV* param = (ResamplerAVX2::ThreadParameterHV*)p;
	const int alingnwidth = param->alingnwidth_;
#ifdef _DEBUG
	std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work(alingnwidth);
#else
	std::vector<tjs_uint32,aligned_allocator<tjs_uint32,32> > work;
	work.reserve( alingnwidth );
#endif

	tTVPBaseBitmap* dest = param->dest_;
	const tTVPRect& destrect = *param->destrect_;
	const tTVPBaseBitmap* src = param->src_;
	const tTVPRect& srcrect = *param->srcrect_;

	const int srcwidth = srcrect.get_width();
	const int dstwidth = destrect.get_width();
	const int dstheight = destrect.get_height();
	const float* wstarty = param->wstarty_;
	tjs_uint32* workbits = &work[0];
	tjs_int dststride = dest->GetPitchBytes()/(int)sizeof(tjs_uint32);
	tjs_uint32* dstbits = (tjs_uint32*)dest->GetScanLineForWrite(param->start_+destrect.top) + param->clip_->dst_left_;
	if( param->blendfunc_ == NULL ) {
		for( int y = param->start_; y < param->end_; y++ ) {
			param->sampler_->samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
			param->sampler_->samplingHorizontal( dstbits, param->clip_->offsetx_, param->clip_->width_, workbits );
			dstbits += dststride;
		}
	} else {	// �P���R�s�[�ȊO
#ifdef _DEBUG
		std::vector<tjs_uint32> dstwork(param->clip_->getDestWidth()+7);
#else
		std::vector<tjs_uint32> dstwork;
		dstwork.reserve( param->clip_->getDestWidth()+7 );
#endif
		tjs_uint32* midbits = &dstwork[0];	// �r�������p�o�b�t�@
		for( int y = param->start_; y < param->end_; y++ ) {
			param->sampler_->samplingVertical( y, workbits, dstheight, srcwidth, src, srcrect, wstarty );
			param->sampler_->samplingHorizontal( midbits, param->clip_->offsetx_, param->clip_->width_, workbits ); // �ꎞ�o�b�t�@�ɂ܂��R�s�[, �͈͊O�͏������Ȃ�
			(*param->blendfunc_)( dstbits, midbits, param->clip_->getDestWidth() );
			dstbits += dststride;
		}
	}
}
void TVPBicubicResampleAVX2Fix( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, float sharpness ) {
	BicubicWeightAVX weightfunc(sharpness);
	ResamplerAVX2Fix sampler;
	sampler.ResampleMT( clip, blendfunc, dest, destrect, src, srcrect, BicubicWeightAVX::RANGE, weightfunc );
	_mm256_zeroupper();
}
void TVPBicubicResampleAVX2( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect, float sharpness ) {
	BicubicWeightAVX weightfunc(sharpness);
	ResamplerAVX2 sampler;
	sampler.ResampleMT( clip, blendfunc, dest, destrect, src, srcrect, BicubicWeightAVX::RANGE, weightfunc );
	_mm256_zeroupper();
}

void TVPAreaAvgResampleAVX2Fix( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
	ResamplerAVX2Fix sampler;
	sampler.ResampleAreaAvgMT( clip, blendfunc, dest, destrect, src, srcrect );
	_mm256_zeroupper();
}
void TVPAreaAvgResampleAVX2( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
	ResamplerAVX2 sampler;
	sampler.ResampleAreaAvgMT( clip, blendfunc, dest, destrect, src, srcrect );
	_mm256_zeroupper();
}

template<typename TWeightFunc>
void TVPWeightResampleAVX2Fix( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
	TWeightFunc weightfunc;
	ResamplerAVX2Fix sampler;
	sampler.ResampleMT( clip, blendfunc, dest, destrect, src, srcrect, TWeightFunc::RANGE, weightfunc );
	_mm256_zeroupper();
}

template<typename TWeightFunc>
void TVPWeightResampleAVX2( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc, tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect ) {
	TWeightFunc weightfunc;
	ResamplerAVX2 sampler;
	sampler.ResampleMT( clip, blendfunc, dest, destrect, src, srcrect, TWeightFunc::RANGE, weightfunc );
	_mm256_zeroupper();
}
/**
 * �g��k������ AVX2 ��
 * @param dest : �������ݐ�摜
 * @param destrect : �������ݐ��`
 * @param src : �ǂݍ��݌��摜
 * @param srcrect : �ǂݍ��݌���`
 * @param type : �g��k���t�B���^�^�C�v
 * @param typeopt : �g��k���t�B���^�^�C�v�I�v�V����
 * @param method : �u�����h���@
 * @param opa : �s�����x
 * @param hda : �������ݐ�A���t�@�ێ�
 */
void TVPResampleImageAVX2( const tTVPResampleClipping &clip, const tTVPImageCopyFuncBase* blendfunc,
	tTVPBaseBitmap *dest, const tTVPRect &destrect, const tTVPBaseBitmap *src, const tTVPRect &srcrect,
	tTVPBBStretchType type, tjs_real typeopt ) {
	switch( type ) {
	case stLinear:
		TVPWeightResampleAVX2<BilinearWeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stCubic:
		TVPBicubicResampleAVX2(clip, blendfunc, dest, destrect, src, srcrect, (float)typeopt );
		break;
	case stSemiFastLinear:
		TVPWeightResampleAVX2Fix<BilinearWeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastCubic:
		TVPBicubicResampleAVX2Fix(clip, blendfunc, dest, destrect, src, srcrect, (float)typeopt );
		break;
	case stLanczos2:
		TVPWeightResampleAVX2<LanczosWeightAVX<2> >(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastLanczos2:
		TVPWeightResampleAVX2Fix<LanczosWeightAVX<2> >(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stLanczos3:
		TVPWeightResampleAVX2<LanczosWeightAVX<3> >(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastLanczos3:
		TVPWeightResampleAVX2Fix<LanczosWeightAVX<3> >(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stSpline16:
		TVPWeightResampleAVX2<Spline16WeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastSpline16:
		TVPWeightResampleAVX2Fix<Spline16WeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stSpline36:
		TVPWeightResampleAVX2<Spline36WeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastSpline36:
		TVPWeightResampleAVX2Fix<Spline36WeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stAreaAvg:
		TVPAreaAvgResampleAVX2(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastAreaAvg:
		TVPAreaAvgResampleAVX2Fix(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stGaussian:
		TVPWeightResampleAVX2<GaussianWeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastGaussian:
		TVPWeightResampleAVX2Fix<GaussianWeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stBlackmanSinc:
		TVPWeightResampleAVX2<BlackmanSincWeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	case stFastBlackmanSinc:
		TVPWeightResampleAVX2Fix<BlackmanSincWeightAVX>(clip, blendfunc, dest, destrect, src, srcrect );
		break;
	default:
		throw L"Not supported yet.";
		break;
	}
}
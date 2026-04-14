#pragma once

#include <vector>

#define BIT(n) (1U << (n))
#define BIT64(n) (1ULL << (n))

#define SetBits(bit_vector, bits) ((bit_vector) |= (bits))
#define ClearBits(bit_vector, bits) ((bit_vector) &= ~(bits))
#define FBitSet(bit_vector, bits) ((bit_vector) & (bits))

#ifndef M_PI
#define M_PI (double)3.14159265358979323846
#endif

#define M_PI2 ((double)(M_PI * 2))
#define M_PI_F ((float)(M_PI))
#define M_PI2_F ((float)(M_PI2))

#define VectorNormalize2(v, dest)                      \
	{                                                  \
		float ilength = (float)sqrt(DotProduct(v, v)); \
		if (ilength)                                   \
			ilength = 1.0f / ilength;                  \
		dest[0] = v[0] * ilength;                      \
		dest[1] = v[1] * ilength;                      \
		dest[2] = v[2] * ilength;                      \
	}

#define VectorNormalizeFast(v)                            \
	{                                                     \
		float ilength = (float)Q_rsqrt(DotProduct(v, v)); \
		v[0] *= ilength;                                  \
		v[1] *= ilength;                                  \
		v[2] *= ilength;                                  \
	}

#define VectorSet(v, x, y, z) ((v)[0] = (x), (v)[1] = (y), (v)[2] = (z))
#define VectorMAMAM(scale1, b1, scale2, b2, scale3, b3, c) ((c)[0] = (scale1) * (b1)[0] + (scale2) * (b2)[0] + (scale3) * (b3)[0], (c)[1] = (scale1) * (b1)[1] + (scale2) * (b2)[1] + (scale3) * (b3)[1], (c)[2] = (scale1) * (b1)[2] + (scale2) * (b2)[2] + (scale3) * (b3)[2])
#define bound(min, num, max) ((num) >= (min) ? ((num) < (max) ? (num) : (max)) : (min))

typedef union
{
	float fl;
	uint32_t u;
	int32_t i;
} float_bits_t;

#define Swap32(x) (((uint32_t)(((x) & 255) << 24)) + ((uint32_t)((((x) >> 8) & 255) << 16)) + ((uint32_t)(((x) >> 16) & 255) << 8) + (((x) >> 24) & 255))

static inline uint32_t FloatAsUint(float v)
{
	float_bits_t bits = {v};
	return bits.u;
}

static inline int32_t FloatAsInt(float v)
{
	float_bits_t bits = {v};
	return bits.i;
}

static inline float IntAsFloat(int32_t i)
{
	float_bits_t bits;
	bits.i = i;
	return bits.fl;
}

static inline float UintAsFloat(uint32_t u)
{
	float_bits_t bits;
	bits.u = u;
	return bits.fl;
}

static inline float SwapFloat(float bf)
{
	uint32_t bi = FloatAsUint(bf);
	uint32_t li = Swap32(bi);
	return UintAsFloat(li);
}

inline float Q_rsqrt(float number)
{
	int i;
	float x, y;

	if (number == 0.0f)
		return 0.0f;

	x = number * 0.5f;
	i = FloatAsInt(number);	   // evil floating point bit level hacking
	i = 0x5f3759df - (i >> 1); // what the fuck?
	y = IntAsFloat(i);
	y = y * (1.5f - (x * y * y)); // first iteration

	return y;
}

static inline void SinCos(float radians, float* sine, float* cosine)
{
	*sine = sin(radians);
	*cosine = cos(radians);
}

extern cl_entity_t* R_BeamGetEntity(int index);
extern bool R_BeamCull(const Vector &start, const Vector &end, bool pvsOnly);
extern void R_BeamSetup(CLIENTBEAM* pbeam, Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed);
extern void R_BeamSetAttributes(CLIENTBEAM* pbeam, float r, float g, float b, float framerate, int startFrame);
extern void R_FreeDeadParticles(particle_t** ppparticles);
extern void CL_DrawBeams(bool fTrans);
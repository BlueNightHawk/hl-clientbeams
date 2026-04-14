/*
gl_beams.c - beams rendering
Copyright (C) 2009 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "PlatformHeaders.h"
#include "SDL2/SDL_opengl.h"

#include "hud.h"
#include "cl_beams.h"
#include "Exports.h"
#include "cl_util.h"
#include "xash_defs.h"
#include "com_model.h"
#include "r_efx.h"
#include "event_flags.h"
#include "entity_types.h"
#include "triangleapi.h"
#include "customentity.h"
#include "pm_defs.h"
#include "studio.h"

#define NOISE_DIVISIONS 64 // don't touch - many tripmines cause the crash when it equal 128

extern std::vector<CLIENTBEAM*> g_pClientBeams;
extern ref_params_s refdef;

typedef struct
{
	Vector pos;
	float texcoord; // Y texture coordinate
	float width;
} beamseg_t;

/*
==============================================================

FRACTAL NOISE

==============================================================
*/
static float rgNoise[NOISE_DIVISIONS + 1]; // global noise array

// freq2 += step * 0.1;
// Fractal noise generator, power of 2 wavelength
static void FracNoise(float* noise, int divs)
{
	int div2;

	div2 = divs >> 1;
	if (divs < 2)
		return;

	// noise is normalized to +/- scale
	noise[div2] = (noise[0] + noise[divs]) * 0.5f + divs * gEngfuncs.pfnRandomFloat(-0.125f, 0.125f);

	if (div2 > 1)
	{
		FracNoise(&noise[div2], div2);
		FracNoise(noise, div2);
	}
}

static void SineNoise(float* noise, int divs)
{
	float freq = 0;
	float step = M_PI_F / (float)divs;
	int i;

	for (i = 0; i < divs; i++)
	{
		noise[i] = sin(freq);
		freq += step;
	}
}


/*
==============================================================

CLIENTBEAM MATHLIB

==============================================================
*/
static void R_BeamComputePerpendicular(const Vector &vecBeamDelta, Vector &pPerp)
{
	// direction in worldspace of the center of the beam
	Vector vecBeamCenter;

	VectorNormalize2(vecBeamDelta, vecBeamCenter);
	CrossProduct(refdef.forward, vecBeamCenter, pPerp);
	VectorNormalize(pPerp);
}

static void R_BeamComputeNormal(const Vector &vStartPos, const Vector &vNextPos, Vector &pNormal)
{
	// vTangentY = line vector for beam
	Vector vTangentY, vDirToBeam;

	VectorSubtract(vStartPos, vNextPos, vTangentY);

	// vDirToBeam = vector from viewer origin to beam
	VectorSubtract(vStartPos, refdef.vieworg, vDirToBeam);

	// get a vector that is perpendicular to us and perpendicular to the beam.
	// this is used to fatten the beam.
	CrossProduct(vTangentY, vDirToBeam, pNormal);
	VectorNormalizeFast(pNormal);
}


/*
==============
R_BeamCull

Cull the beam by bbox
==============
*/
bool R_BeamCull(const Vector &start, const Vector &end, bool pvsOnly)
{
	Vector mins, maxs;
	int i;

	for (i = 0; i < 3; i++)
	{
		if (start[i] < end[i])
		{
			mins[i] = start[i];
			maxs[i] = end[i];
		}
		else
		{
			mins[i] = end[i];
			maxs[i] = start[i];
		}

		// don't let it be zero sized
		if (mins[i] == maxs[i])
			maxs[i] += 1.0f;
	}

	// beam is culled
	return !gEngfuncs.pTriAPI->BoxInPVS(mins, maxs);
}


/*
==============================================================

CLIENTBEAM DRAW METHODS

==============================================================
*/
/*
================
R_DrawSegs

general code for drawing beams
================
*/
static void R_DrawSegs(Vector source, Vector control, Vector delta, Vector target, float width, float scale, float freq, float speed, int segments, int flags, int clflags)
{
	int noiseIndex, noiseStep;
	int i, total_segs, segs_drawn;
	float div, length, fraction, factor;
	float flMaxWidth, vLast, vStep, brightness;
	Vector perp1, vLastNormal;
	beamseg_t curSeg;

	float t = 0, dt = 0;

	if (segments < 2)
		return;

	length = Length(delta);
	flMaxWidth = width * 0.5f;
	div = 1.0f / (segments - 1);

	if (length * div < flMaxWidth * 1.414f)
	{
		// here, we have too many segments; we could get overlap... so lets have less segments
		segments = (int)(length / (flMaxWidth * 1.414f)) + 1.0f;
		if (segments < 2)
			segments = 2;
	}

	if (segments > NOISE_DIVISIONS)
		segments = NOISE_DIVISIONS;

	div = 1.0f / (segments - 1);
	length *= 0.01f;
	vStep = length * div; // Texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	vLast = fmod(freq * speed, 1);

	if (flags & FBEAM_SINENOISE)
	{
		if (segments < 16)
		{
			segments = 16;
			div = 1.0f / (segments - 1);
		}
		scale *= 100.0f;
		length = segments * 0.1f;
	}
	else
	{
		scale *= length * 2.0f;
	}

	// Iterator to resample noise waveform (it needs to be generated in powers of 2)
	noiseStep = (int)((float)(NOISE_DIVISIONS - 1) * div * 65536.0f);
	brightness = 1.0f;
	noiseIndex = 0;

	if (FBitSet(flags, FBEAM_SHADEIN))
		brightness = 0;

	// Choose two vectors that are perpendicular to the beam
	R_BeamComputePerpendicular(delta, perp1);

	total_segs = segments;
	segs_drawn = 0;

	if (FBitSet(clflags, FBEAM_QUADRATIC))
	{
		dt = 1.0 / (float)segments;
	}

	// specify all the segments.
	for (i = 0; i < segments; i++, t += dt)
	{
		beamseg_t nextSeg;
		Vector vPoint1, vPoint2;

		assert(noiseIndex < (NOISE_DIVISIONS << 16));

		fraction = i * div;

		if (FBitSet(clflags, FBEAM_QUADRATIC))
		{
			float omt = (1 - t);
			float p0 = omt * omt;
			float p1 = 2 * t * omt;
			float p2 = t * t;

			nextSeg.pos = p0 * source + p1 * control + p2 * target;
		}
		else
			VectorMA(source, fraction, delta, nextSeg.pos);

		// distort using noise
		if (scale != 0)
		{
			factor = rgNoise[noiseIndex >> 16] * scale;

			if (FBitSet(flags, FBEAM_SINENOISE))
			{
				float s, c;

				SinCos(fraction * M_PI_F * length + freq, &s, &c);
				VectorMA(nextSeg.pos, (factor * s), refdef.up, nextSeg.pos);

				// rotate the noise along the perpendicluar axis a bit to keep the bolt from looking diagonal
				VectorMA(nextSeg.pos, (factor * c), refdef.right, nextSeg.pos);
			}
			else
			{
				VectorMA(nextSeg.pos, factor, perp1, nextSeg.pos);
			}
		}

		// specify the next segment.
		nextSeg.width = width * 2.0f;
		nextSeg.texcoord = vLast;

		if (segs_drawn > 0)
		{
			// Get a vector that is perpendicular to us and perpendicular to the beam.
			// This is used to fatten the beam.
			Vector vNormal, vAveNormal;

			R_BeamComputeNormal(curSeg.pos, nextSeg.pos, vNormal);

			if (segs_drawn > 1)
			{
				// Average this with the previous normal
				VectorAdd(vNormal, vLastNormal, vAveNormal);
				VectorScale(vAveNormal, 0.5f, vAveNormal);
				VectorNormalizeFast(vAveNormal);
			}
			else
			{
				VectorCopy(vNormal, vAveNormal);
			}

			VectorCopy(vNormal, vLastNormal);

			// draw regular segment
			VectorMA(curSeg.pos, (curSeg.width * 0.5f), vAveNormal, vPoint1);
			VectorMA(curSeg.pos, (-curSeg.width * 0.5f), vAveNormal, vPoint2);

			glTexCoord2f(0.0f, curSeg.texcoord);
			gEngfuncs.pTriAPI->Brightness(brightness);
			glNormal3fv(vAveNormal);
			glVertex3fv(vPoint1);

			glTexCoord2f(1.0f, curSeg.texcoord);
			gEngfuncs.pTriAPI->Brightness(brightness);
			glNormal3fv(vAveNormal);
			glVertex3fv(vPoint2);
		}

		curSeg = nextSeg;
		segs_drawn++;

		if (FBitSet(flags, FBEAM_SHADEIN) && FBitSet(flags, FBEAM_SHADEOUT))
		{
			if (fraction < 0.5f)
				brightness = fraction;
			else
				brightness = (1.0f - fraction);
		}
		else if (FBitSet(flags, FBEAM_SHADEIN))
		{
			brightness = fraction;
		}
		else if (FBitSet(flags, FBEAM_SHADEOUT))
		{
			brightness = 1.0f - fraction;
		}

		if (segs_drawn == total_segs)
		{
			// draw the last segment
			VectorMA(curSeg.pos, (curSeg.width * 0.5f), vLastNormal, vPoint1);
			VectorMA(curSeg.pos, (-curSeg.width * 0.5f), vLastNormal, vPoint2);

			// specify the points.
			glTexCoord2f(0.0f, curSeg.texcoord);
			gEngfuncs.pTriAPI->Brightness(brightness);
			glNormal3fv(vLastNormal);
			glVertex3fv(vPoint1);

			glTexCoord2f(1.0f, curSeg.texcoord);
			gEngfuncs.pTriAPI->Brightness(brightness);
			glNormal3fv(vLastNormal);
			glVertex3fv(vPoint2);
		}

		vLast += vStep; // Advance texture scroll (v axis only)
		noiseIndex += noiseStep;
	}
}

/*
================
R_DrawTorus

Draw beamtours
================
*/
static void R_DrawTorus(Vector source, Vector delta, float width, float scale, float freq, float speed, int segments)
{
	int i, noiseIndex, noiseStep;
	float div, length, fraction, factor, vLast, vStep;
	Vector last1, last2, point, screen, screenLast, tmp, normal;

	if (segments < 2)
		return;

	if (segments > NOISE_DIVISIONS)
		segments = NOISE_DIVISIONS;

	length = Length(delta) * 0.01f;
	if (length < 0.5f)
		length = 0.5f; // don't lose all of the noise/texture on short beams

	div = 1.0f / (segments - 1);

	vStep = length * div; // Texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	vLast = fmod(freq * speed, 1);
	scale = scale * length;

	// Iterator to resample noise waveform (it needs to be generated in powers of 2)
	noiseStep = (int)((float)(NOISE_DIVISIONS - 1) * div * 65536.0f);
	noiseIndex = 0;

	for (i = 0; i < segments; i++)
	{
		float s, c;

		fraction = i * div;
		SinCos(fraction * M_PI2_F, &s, &c);

		point[0] = s * freq * delta[2] + source[0];
		point[1] = c * freq * delta[2] + source[1];
		point[2] = source[2];

		// distort using noise
		if (scale != 0)
		{
			if ((noiseIndex >> 16) < NOISE_DIVISIONS)
			{
				factor = rgNoise[noiseIndex >> 16] * scale;
				VectorMA(point, factor, refdef.up, point);

				// rotate the noise along the perpendicluar axis a bit to keep the bolt from looking diagonal
				factor = rgNoise[noiseIndex >> 16] * scale * cos(fraction * M_PI_F * 3 + freq);
				VectorMA(point, factor, refdef.right, point);
			}
		}

		// Transform point into screen space
		gEngfuncs.pTriAPI->WorldToScreen(point, screen);

		if (i != 0)
		{
			// build world-space normal to screen-space direction vector
			VectorSubtract(screen, screenLast, tmp);

			// we don't need Z, we're in screen space
			tmp[2] = 0;
			VectorNormalize(tmp);
			VectorScale(refdef.up, -tmp[0], normal); // Build point along noraml line (normal is -y, x)
			VectorMA(normal, tmp[1], refdef.right, normal);

			// Make a wide line
			VectorMA(point, width, normal, last1);
			VectorMA(point, -width, normal, last2);

			vLast += vStep; // advance texture scroll (v axis only)
			gEngfuncs.pTriAPI->TexCoord2f(1, vLast);
			gEngfuncs.pTriAPI->Vertex3fv(last2);
			gEngfuncs.pTriAPI->TexCoord2f(0, vLast);
			gEngfuncs.pTriAPI->Vertex3fv(last1);
		}

		VectorCopy(screen, screenLast);
		noiseIndex += noiseStep;
	}
}

/*
================
R_DrawDisk

Draw beamdisk
================
*/
static void R_DrawDisk(Vector source, Vector delta, float width, float scale, float freq, float speed, int segments)
{
	float div, length, fraction;
	float w, vLast, vStep;
	Vector point;
	int i;

	if (segments < 2)
		return;

	if (segments > NOISE_DIVISIONS) // UNDONE: Allow more segments?
		segments = NOISE_DIVISIONS;

	length = Length(delta) * 0.01f;
	if (length < 0.5f)
		length = 0.5f; // don't lose all of the noise/texture on short beams

	div = 1.0f / (segments - 1);
	vStep = length * div; // Texture length texels per space pixel

	// scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	vLast = fmod(freq * speed, 1);
	scale = scale * length;

	// clamp the beam width
	w = fmod(freq, width * 0.1f) * delta[2];

	// NOTE: we must force the degenerate triangles to be on the edge
	for (i = 0; i < segments; i++)
	{
		float s, c;

		fraction = i * div;
		VectorCopy(source, point);

		gEngfuncs.pTriAPI->Brightness(1.0f);
		gEngfuncs.pTriAPI->TexCoord2f(1.0f, vLast);
		gEngfuncs.pTriAPI->Vertex3fv(point);

		SinCos(fraction * M_PI2_F, &s, &c);
		point[0] = s * w + source[0];
		point[1] = c * w + source[1];
		point[2] = source[2];

		gEngfuncs.pTriAPI->Brightness(1.0f);
		gEngfuncs.pTriAPI->TexCoord2f(0.0f, vLast);
		gEngfuncs.pTriAPI->Vertex3fv(point);

		vLast += vStep; // advance texture scroll (v axis only)
	}
}

/*
================
R_DrawCylinder

Draw beam cylinder
================
*/
static void R_DrawCylinder(Vector source, Vector delta, float width, float scale, float freq, float speed, int segments)
{
	float div, length, fraction;
	float vLast, vStep;
	Vector point;
	int i;

	if (segments < 2)
		return;

	if (segments > NOISE_DIVISIONS)
		segments = NOISE_DIVISIONS;

	length = Length(delta) * 0.01f;
	if (length < 0.5f)
		length = 0.5f; // don't lose all of the noise/texture on short beams

	div = 1.0f / (segments - 1);
	vStep = length * div; // texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	vLast = fmod(freq * speed, 1);
	scale = scale * length;

	for (i = 0; i < segments; i++)
	{
		float s, c;

		fraction = i * div;
		SinCos(fraction * M_PI2_F, &s, &c);

		point[0] = s * freq * delta[2] + source[0];
		point[1] = c * freq * delta[2] + source[1];
		point[2] = source[2] + width;

		gEngfuncs.pTriAPI->Brightness(0);
		gEngfuncs.pTriAPI->TexCoord2f(1, vLast);
		gEngfuncs.pTriAPI->Vertex3fv(point);

		point[0] = s * freq * (delta[2] + width) + source[0];
		point[1] = c * freq * (delta[2] + width) + source[1];
		point[2] = source[2] - width;

		gEngfuncs.pTriAPI->Brightness(1);
		gEngfuncs.pTriAPI->TexCoord2f(0, vLast);
		gEngfuncs.pTriAPI->Vertex3fv(point);

		vLast += vStep; // Advance texture scroll (v axis only)
	}
}

/*
==============
R_FreeDeadParticles

Free particles that time has expired
==============
*/
void R_FreeDeadParticles(particle_t** ppparticles)
{
	if (!ppparticles || !*ppparticles)
		return;

	particle_t *p, *kill;

	// kill all the ones hanging direcly off the base pointer
	while (1)
	{
		kill = *ppparticles;
		if (kill && kill->die < gEngfuncs.GetClientTime())
		{
			if (kill->on_die)
				kill->on_die(kill);
			kill->on_die = NULL;
			*ppparticles = kill->next;
			kill->next = gEngfuncs.pEfxAPI->R_AllocParticle(nullptr);
			continue;
		}
		break;
	}

	// kill off all the others
	for (p = *ppparticles; p; p = p->next)
	{
		while (1)
		{
			kill = p->next;
			if (kill && kill->die < gEngfuncs.GetClientTime())
			{
				if (kill->on_die)
					kill->on_die(kill);
				kill->on_die = NULL;
				p->next = kill->next;
				kill->next = gEngfuncs.pEfxAPI->R_AllocParticle(nullptr);
				continue;
			}
			break;
		}
	}
}


/*
==============
R_DrawBeamFollow

drawi followed beam
==============
*/
static void R_DrawBeamFollow(CLIENTBEAM* pbeam, float frametime)
{
	particle_t *pnew, *particles;
	float fraction, div, vLast, vStep;
	Vector last1, last2, tmp, screen;
	Vector delta, screenLast, normal;

	R_FreeDeadParticles(&pbeam->particles);
	particles = pbeam->particles;
	pnew = NULL;

	div = 0;
	if (FBitSet(pbeam->flags, FBEAM_STARTENTITY))
	{
		if (particles)
		{
			VectorSubtract(particles->org, pbeam->source, delta);
			div = Length(delta);

			if (div >= 32)
			{
				pnew = gEngfuncs.pEfxAPI->R_AllocParticle(nullptr);
			}
		}
		else
		{
			pnew = gEngfuncs.pEfxAPI->R_AllocParticle(nullptr);
		}
	}

	if (pnew)
	{
		VectorCopy(pbeam->source, pnew->org);
		pnew->die = gEngfuncs.GetClientTime() + pbeam->amplitude;
		VectorClear(pnew->vel);

		pnew->next = particles;
		pbeam->particles = pnew;
		particles = pnew;
	}

	// nothing to draw
	if (!particles)
		return;

	if (!pnew && div != 0)
	{
		VectorCopy(pbeam->source, delta);
		gEngfuncs.pTriAPI->WorldToScreen(pbeam->source, screenLast);
		gEngfuncs.pTriAPI->WorldToScreen(particles->org, screen);
	}
	else if (particles && particles->next)
	{
		VectorCopy(particles->org, delta);
		gEngfuncs.pTriAPI->WorldToScreen(particles->org, screenLast);
		gEngfuncs.pTriAPI->WorldToScreen(particles->next->org, screen);
		particles = particles->next;
	}
	else
	{
		return;
	}

	// UNDONE: This won't work, screen and screenLast must be extrapolated here to fix the
	// first beam segment for this trail

	// build world-space normal to screen-space direction vector
	VectorSubtract(screen, screenLast, tmp);
	// we don't need Z, we're in screen space
	tmp[2] = 0;
	VectorNormalize(tmp);

	// Build point along noraml line (normal is -y, x)
	VectorScale(refdef.up, tmp[0], normal); // Build point along normal line (normal is -y, x)
	VectorMA(normal, tmp[1], refdef.right, normal);

	// Make a wide line
	VectorMA(delta, pbeam->width, normal, last1);
	VectorMA(delta, -pbeam->width, normal, last2);

	div = 1.0f / pbeam->amplitude;
	fraction = (pbeam->die - gEngfuncs.GetClientTime()) * div;

	vLast = 0.0f;
	vStep = 1.0f;

	while (particles)
	{
		gEngfuncs.pTriAPI->Brightness(fraction);
		gEngfuncs.pTriAPI->TexCoord2f(1, 1);
		gEngfuncs.pTriAPI->Vertex3fv(last2);
		gEngfuncs.pTriAPI->Brightness(fraction);
		gEngfuncs.pTriAPI->TexCoord2f(0, 1);
		gEngfuncs.pTriAPI->Vertex3fv(last1);

		// Transform point into screen space
		gEngfuncs.pTriAPI->WorldToScreen(particles->org, screen);
		// Build world-space normal to screen-space direction vector
		VectorSubtract(screen, screenLast, tmp);

		// we don't need Z, we're in screen space
		tmp[2] = 0;
		VectorNormalize(tmp);
		VectorScale(refdef.up, tmp[0], normal); // Build point along noraml line (normal is -y, x)
		VectorMA(normal, tmp[1], refdef.right, normal);

		// Make a wide line
		VectorMA(particles->org, pbeam->width, normal, last1);
		VectorMA(particles->org, -pbeam->width, normal, last2);

		vLast += vStep; // Advance texture scroll (v axis only)

		if (particles->next != NULL)
		{
			fraction = (particles->die - gEngfuncs.GetClientTime()) * div;
		}
		else
		{
			fraction = 0.0;
		}

		gEngfuncs.pTriAPI->Brightness(fraction);
		gEngfuncs.pTriAPI->TexCoord2f(0, 0);
		gEngfuncs.pTriAPI->Vertex3fv(last1);
		gEngfuncs.pTriAPI->Brightness(fraction);
		gEngfuncs.pTriAPI->TexCoord2f(1, 0);
		gEngfuncs.pTriAPI->Vertex3fv(last2);

		VectorCopy(screen, screenLast);

		particles = particles->next;
	}

	// drift popcorn trail if there is a velocity
	particles = pbeam->particles;

	while (particles)
	{
		VectorMA(particles->org, frametime, particles->vel, particles->org);
		particles = particles->next;
	}
}

/*
================
R_DrawRing

Draw beamring
================
*/
static void R_DrawRing(Vector source, Vector delta, float width, float amplitude, float freq, float speed, int segments)
{
	int i, j, noiseIndex, noiseStep;
	float div, length, fraction, factor, vLast, vStep;
	Vector last1, last2, point, screen, screenLast;
	Vector tmp, normal, center, xaxis, yaxis;
	float radius, x, y, scale;

	if (segments < 2)
		return;

	VectorClear(screenLast);
	segments = segments * M_PI_F;

	if (segments > NOISE_DIVISIONS * 8)
		segments = NOISE_DIVISIONS * 8;

	length = Length(delta) * 0.01f * M_PI_F;
	if (length < 0.5f)
		length = 0.5f; // Don't lose all of the noise/texture on short beams

	div = 1.0f / (segments - 1);

	vStep = length * div / 8.0f; // texture length texels per space pixel

	// Scroll speed 3.5 -- initial texture position, scrolls 3.5/sec (1.0 is entire texture)
	vLast = fmod(freq * speed, 1.0f);
	scale = amplitude * length / 8.0f;

	// Iterator to resample noise waveform (it needs to be generated in powers of 2)
	noiseStep = (int)((float)(NOISE_DIVISIONS - 1) * div * 65536.0f) * 8;
	noiseIndex = 0;

	VectorScale(delta, 0.5f, delta);
	VectorAdd(source, delta, center);

	VectorCopy(delta, xaxis);
	radius = Length(xaxis);

	// cull beamring
	// --------------------------------
	// Compute box center +/- radius
	VectorSet(last1, radius, radius, scale);
	VectorAdd(center, last1, tmp);		   // maxs
	VectorSubtract(center, last1, screen); // mins

	if (!gEngfuncs.hudGetModelByIndex(1))
		return;

	// is that box in PVS && frustum?
	if (!gEngfuncs.pTriAPI->BoxInPVS(screen, tmp))
	{
		return;
	}

	VectorSet(yaxis, xaxis[1], -xaxis[0], 0.0f);
	VectorNormalize(yaxis);
	VectorScale(yaxis, radius, yaxis);

	j = segments / 8;

	for (i = 0; i < segments + 1; i++)
	{
		fraction = i * div;
		SinCos(fraction * M_PI2_F, &x, &y);

		VectorMAMAM(x, xaxis, y, yaxis, 1.0f, center, point);

		// distort using noise
		factor = rgNoise[(noiseIndex >> 16) & (NOISE_DIVISIONS - 1)] * scale;
		VectorMA(point, factor, refdef.up, point);

		// Rotate the noise along the perpendicluar axis a bit to keep the bolt from looking diagonal
		factor = rgNoise[(noiseIndex >> 16) & (NOISE_DIVISIONS - 1)] * scale;
		factor *= cos(fraction * M_PI_F * 24 + freq);
		VectorMA(point, factor, refdef.right, point);

		// Transform point into screen space
		gEngfuncs.pTriAPI->WorldToScreen(point, screen);

		if (i != 0)
		{
			// build world-space normal to screen-space direction vector
			VectorSubtract(screen, screenLast, tmp);

			// we don't need Z, we're in screen space
			tmp[2] = 0;
			VectorNormalize(tmp);

			// Build point along normal line (normal is -y, x)
			VectorScale(refdef.up, tmp[0], normal);
			VectorMA(normal, tmp[1], refdef.right, normal);

			// Make a wide line
			VectorMA(point, width, normal, last1);
			VectorMA(point, -width, normal, last2);

			vLast += vStep; // Advance texture scroll (v axis only)
			gEngfuncs.pTriAPI->TexCoord2f(1.0f, vLast);
			gEngfuncs.pTriAPI->Vertex3fv(last2);
			gEngfuncs.pTriAPI->TexCoord2f(0.0f, vLast);
			gEngfuncs.pTriAPI->Vertex3fv(last1);
		}

		VectorCopy(screen, screenLast);
		noiseIndex += noiseStep;
		j--;

		if (j == 0 && amplitude != 0)
		{
			j = segments / 8;
			FracNoise(rgNoise, NOISE_DIVISIONS);
		}
	}
}

/*
==============
R_BeamGetEntity

extract entity number from index
handle user entities
==============
*/
cl_entity_t* R_BeamGetEntity(int index)
{
	if (index < 0)
		return HUD_GetUserEntity(BEAMENT_ENTITY(-index));
	return gEngfuncs.GetEntityByIndex(BEAMENT_ENTITY(index));
}

/*
===============
CL_FxBlend
===============
*/
int CL_FxBlend(cl_entity_t* e)
{
	int blend = 0;
	float offset, dist;
	Vector tmp;

	offset = ((int)e->index) * 363.0f; // Use ent index to de-sync these fx

	switch (e->curstate.renderfx)
	{
	case kRenderFxPulseSlowWide:
		blend = e->curstate.renderamt + 0x40 * sin(gEngfuncs.GetClientTime() * 2 + offset);
		break;
	case kRenderFxPulseFastWide:
		blend = e->curstate.renderamt + 0x40 * sin(gEngfuncs.GetClientTime() * 8 + offset);
		break;
	case kRenderFxPulseSlow:
		blend = e->curstate.renderamt + 0x10 * sin(gEngfuncs.GetClientTime() * 2 + offset);
		break;
	case kRenderFxPulseFast:
		blend = e->curstate.renderamt + 0x10 * sin(gEngfuncs.GetClientTime() * 8 + offset);
		break;
	case kRenderFxFadeSlow:
		if (e->curstate.renderamt > 0)
			e->curstate.renderamt -= 1;
		else
			e->curstate.renderamt = 0;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxFadeFast:
		if (e->curstate.renderamt > 3)
			e->curstate.renderamt -= 4;
		else
			e->curstate.renderamt = 0;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidSlow:
		if (e->curstate.renderamt < 255)
			e->curstate.renderamt += 1;
		else
			e->curstate.renderamt = 255;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidFast:
		if (e->curstate.renderamt < 252)
			e->curstate.renderamt += 4;
		else
			e->curstate.renderamt = 255;
		blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeSlow:
		blend = 20 * sin(gEngfuncs.GetClientTime() * 4 + offset);
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFast:
		blend = 20 * sin(gEngfuncs.GetClientTime() * 16 + offset);
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFaster:
		blend = 20 * sin(gEngfuncs.GetClientTime() * 36 + offset);
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerSlow:
		blend = 20 * (sin(gEngfuncs.GetClientTime() * 2) + sin(gEngfuncs.GetClientTime() * 17 + offset));
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerFast:
		blend = 20 * (sin(gEngfuncs.GetClientTime() * 16) + sin(gEngfuncs.GetClientTime() * 23 + offset));
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxHologram:
	case kRenderFxDistort:
		VectorCopy(e->origin, tmp);
		VectorSubtract(tmp, refdef.vieworg, tmp);
		dist = DotProduct(tmp, refdef.forward);

		// turn off distance fade
		if (e->curstate.renderfx == kRenderFxDistort)
			dist = 1;

		if (dist <= 0)
		{
			blend = 0;
		}
		else
		{
			e->curstate.renderamt = 180;
			if (dist <= 100)
				blend = e->curstate.renderamt;
			else
				blend = (int)((1.0f - (dist - 100) * (1.0f / 400.0f)) * e->curstate.renderamt);
			blend += gEngfuncs.pfnRandomLong(-32, 31);
		}
		break;
	default:
		blend = e->curstate.renderamt;
		break;
	}

	blend = bound(0, blend, 255);

	return blend;
}

/*
==============
R_BeamComputePoint

compute attachment point for beam
==============
*/
static bool R_BeamComputePoint(int beamEnt, Vector &pt)
{
	cl_entity_t* ent;
	int attach;

	ent = R_BeamGetEntity(beamEnt);

	if (beamEnt < 0)
		attach = BEAMENT_ATTACHMENT(-beamEnt);
	else
		attach = BEAMENT_ATTACHMENT(beamEnt);

	if (!ent)
	{
		gEngfuncs.Con_DPrintf("%s: invalid entity %i\n", __func__, BEAMENT_ENTITY(beamEnt));
		VectorClear(pt);
		return false;
	}

	// get attachment
	if (attach > 0)
	{
		VectorCopy(ent->attachment[attach - 1], pt);
	}
	else if (ent->index == gEngfuncs.GetLocalPlayer()->index)
	{
		VectorCopy(refdef.simorg, pt);
	}
	else
	{
		VectorCopy(ent->origin, pt);
	}

	return true;
}

/*
==============
R_BeamRecomputeEndpoints

Recomputes beam endpoints..
==============
*/
static bool R_BeamRecomputeEndpoints(CLIENTBEAM* pbeam)
{
	if (FBitSet(pbeam->flags, FBEAM_STARTENTITY))
	{
		cl_entity_t* start = R_BeamGetEntity(pbeam->startEntity);

		if (R_BeamComputePoint(pbeam->startEntity, pbeam->source))
		{
			if (!pbeam->pFollowModel)
				pbeam->pFollowModel = start->model;
			SetBits(pbeam->flags, FBEAM_STARTVISIBLE);
		}
		else if (!FBitSet(pbeam->flags, FBEAM_FOREVER))
		{
			ClearBits(pbeam->flags, FBEAM_STARTENTITY);
		}
	}

	if (FBitSet(pbeam->flags, FBEAM_ENDENTITY))
	{
		cl_entity_t* end = R_BeamGetEntity(pbeam->endEntity);

		if (R_BeamComputePoint(pbeam->endEntity, pbeam->target))
		{
			if (!pbeam->pFollowModel)
				pbeam->pFollowModel = end->model;
			SetBits(pbeam->flags, FBEAM_ENDVISIBLE);
		}
		else if (!FBitSet(pbeam->flags, FBEAM_FOREVER))
		{
			ClearBits(pbeam->flags, FBEAM_ENDENTITY);
			pbeam->die = gEngfuncs.GetClientTime();
			return false;
		}
		else
		{
			return false;
		}
	}

	if (FBitSet(pbeam->flags, FBEAM_STARTENTITY) && !FBitSet(pbeam->flags, FBEAM_STARTVISIBLE))
		return false;
	return true;
}


/*
==============
R_BeamDraw

Update beam vars and draw it
==============
*/
static void R_BeamDraw(CLIENTBEAM* pbeam, float frametime)
{
	model_t* model;
	Vector delta;

	model = gEngfuncs.hudGetModelByIndex(pbeam->modelIndex);
	SetBits(pbeam->flags, FBEAM_ISACTIVE);

	if (!model || model->type != mod_sprite)
	{
		pbeam->flags &= ~FBEAM_ISACTIVE; // force to ignore
		pbeam->die = gEngfuncs.GetClientTime();
		return;
	}

	// update frequency
	pbeam->freq = pbeam->speed * gEngfuncs.GetClientTime();

	// generate fractal noise
	if (frametime != 0.0f)
	{
		rgNoise[0] = 0;
		rgNoise[NOISE_DIVISIONS] = 0;
	}

	if (pbeam->amplitude != 0 && frametime != 0.0f)
	{
		if (FBitSet(pbeam->flags, FBEAM_SINENOISE))
			SineNoise(rgNoise, NOISE_DIVISIONS);
		else
			FracNoise(rgNoise, NOISE_DIVISIONS);
	}

	// update end points
	if (FBitSet(pbeam->flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY))
	{
		// makes sure attachment[0] + attachment[1] are valid
		if (!R_BeamRecomputeEndpoints(pbeam))
		{
			ClearBits(pbeam->flags, FBEAM_ISACTIVE); // force to ignore
			return;
		}

		// compute segments from the new endpoints
		VectorSubtract(pbeam->target, pbeam->source, delta);
		VectorClear(pbeam->delta);

		if (Length(delta) > 0.0000001f)
			VectorCopy(delta, pbeam->delta);

		if (pbeam->amplitude >= 0.50f)
			pbeam->segments = Length(pbeam->delta) * 0.25f + 3.0f; // one per 4 pixels
		else
			pbeam->segments = Length(pbeam->delta) * 0.075f + 3.0f; // one per 16 pixels
	}

	if (pbeam->type == TE_BEAMPOINTS && R_BeamCull(pbeam->source, pbeam->target, false))
	{
		ClearBits(pbeam->flags, FBEAM_ISACTIVE);
		return;
	}

	// don't draw really short or inactive beams
	if (!FBitSet(pbeam->flags, FBEAM_ISACTIVE) || Length(pbeam->delta) < 0.1f)
	{
		return;
	}

	if (pbeam->flags & (FBEAM_FADEIN | FBEAM_FADEOUT))
	{
		// update life cycle
		pbeam->t = pbeam->freq + (pbeam->die - gEngfuncs.GetClientTime());
		if (pbeam->t != 0.0f)
			pbeam->t = 1.0f - pbeam->freq / pbeam->t;
	}

	if (pbeam->type == TE_BEAMHOSE)
	{
		float flDot;

		VectorSubtract(pbeam->target, pbeam->source, delta);
		VectorNormalize(delta);

		flDot = DotProduct(delta, refdef.forward);

		// abort if the player's looking along it away from the source
		if (flDot > 0)
		{
			return;
		}
		else
		{
			float flFade = pow(flDot, 10);
			Vector localDir, vecProjection, tmp;
			float flDistance;

			// fade the beam if the player's not looking at the source
			VectorSubtract(refdef.vieworg, pbeam->source, localDir);
			flDot = DotProduct(delta, localDir);
			VectorScale(delta, flDot, vecProjection);
			VectorSubtract(localDir, vecProjection, tmp);
			flDistance = Length(tmp);

			if (flDistance > 30)
			{
				flDistance = 1.0f - ((flDistance - 30.0f) / 64.0f);
				if (flDistance <= 0)
					flFade = 0;
				else
					flFade *= pow(flDistance, 3);
			}

			if (flFade < (1.0f / 255.0f))
				return;

			// FIXME: needs to be testing
			pbeam->brightness *= flFade;
		}
	}

	gEngfuncs.pTriAPI->RenderMode(FBitSet(pbeam->flags, FBEAM_SOLID) ? kRenderNormal : kRenderTransAdd);

	if (!gEngfuncs.pTriAPI->SpriteTexture(model, (int)(pbeam->frame + pbeam->frameRate * gEngfuncs.GetClientTime()) % pbeam->frameCount))
	{
		ClearBits(pbeam->flags, FBEAM_ISACTIVE);
		return;
	}

	if (pbeam->type == TE_BEAMFOLLOW)
	{
		cl_entity_t* pStart;

		// XASH SPECIFIC: get brightness from head entity
		pStart = R_BeamGetEntity(pbeam->startEntity);
		if (pStart && pStart->curstate.rendermode != kRenderNormal)
			pbeam->brightness = CL_FxBlend(pStart) / 255.0f;
	}

	if (FBitSet(pbeam->flags, FBEAM_FADEIN))
		gEngfuncs.pTriAPI->Color4f(pbeam->r, pbeam->g, pbeam->b, pbeam->t * pbeam->brightness);
	else if (FBitSet(pbeam->flags, FBEAM_FADEOUT))
		gEngfuncs.pTriAPI->Color4f(pbeam->r, pbeam->g, pbeam->b, (1.0f - pbeam->t) * pbeam->brightness);
	else
		gEngfuncs.pTriAPI->Color4f(pbeam->r, pbeam->g, pbeam->b, pbeam->brightness);

	switch (pbeam->type)
	{
	case TE_BEAMTORUS:
		gEngfuncs.pTriAPI->CullFace(TRI_NONE);
		gEngfuncs.pTriAPI->Begin(TRI_TRIANGLE_STRIP);
		R_DrawTorus(pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments);
		gEngfuncs.pTriAPI->End();
		break;
	case TE_BEAMDISK:
		gEngfuncs.pTriAPI->CullFace(TRI_NONE);
		gEngfuncs.pTriAPI->Begin(TRI_TRIANGLE_STRIP);
		R_DrawDisk(pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments);
		gEngfuncs.pTriAPI->End();
		break;
	case TE_BEAMCYLINDER:
		gEngfuncs.pTriAPI->CullFace(TRI_NONE);
		gEngfuncs.pTriAPI->Begin(TRI_TRIANGLE_STRIP);
		R_DrawCylinder(pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments);
		gEngfuncs.pTriAPI->End();
		break;
	case TE_BEAMPOINTS:
	case TE_BEAMHOSE:
		gEngfuncs.pTriAPI->Begin(TRI_TRIANGLE_STRIP);
		R_DrawSegs(pbeam->source, pbeam->control, pbeam->delta, pbeam->target, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments, pbeam->flags, pbeam->clflags);
		gEngfuncs.pTriAPI->End();
		break;
	case TE_BEAMFOLLOW:
		gEngfuncs.pTriAPI->Begin(TRI_QUADS);
		R_DrawBeamFollow(pbeam, frametime);
		gEngfuncs.pTriAPI->End();
		break;
	case TE_BEAMRING:
		gEngfuncs.pTriAPI->CullFace(TRI_NONE);
		gEngfuncs.pTriAPI->Begin(TRI_TRIANGLE_STRIP);
		R_DrawRing(pbeam->source, pbeam->delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, pbeam->segments);
		gEngfuncs.pTriAPI->End();
		break;
	}

	gEngfuncs.pTriAPI->CullFace(TRI_FRONT);
}

/*
==============
R_BeamSetup

generic function. all beams must be
passed through this
==============
*/
void R_BeamSetup(CLIENTBEAM* pbeam, Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed)
{
	model_t* sprite = gEngfuncs.hudGetModelByIndex(modelIndex);

	if (!sprite)
		return;
	
	speed *= 0.1f;

	pbeam->type = BEAM_POINTS;
	pbeam->modelIndex = modelIndex;
	pbeam->frame = 0;
	pbeam->frameRate = 0;
	pbeam->frameCount = sprite->numframes;

	VectorCopy(start, pbeam->source);
	VectorCopy(end, pbeam->target);
	VectorSubtract(end, start, pbeam->delta);

	pbeam->freq = speed * gEngfuncs.GetClientTime();
	pbeam->die = life + gEngfuncs.GetClientTime();
	pbeam->amplitude = amplitude;
	pbeam->brightness = brightness;
	pbeam->width = width;
	pbeam->speed = speed;

	if (amplitude >= 0.50f)
		pbeam->segments = Length(pbeam->delta) * 0.25f + 3.0f; // one per 4 pixels
	else
		pbeam->segments = Length(pbeam->delta) * 0.075f + 3.0f; // one per 16 pixels

	pbeam->pFollowModel = NULL;
	pbeam->flags = 0;
}

/*
==============
R_BeamSetAttributes

set beam attributes
==============
*/
void R_BeamSetAttributes(CLIENTBEAM* pbeam, float r, float g, float b, float framerate, int startFrame)
{
	pbeam->frame = (float)startFrame;
	pbeam->frameRate = framerate;
	pbeam->r = r;
	pbeam->g = g;
	pbeam->b = b;
}

/*
==============
R_BeamDrawCustomEntity

initialize beam from server entity
==============
*/
static void R_BeamDrawCustomEntity(cl_entity_t* ent)
{
	CLIENTBEAM beam;
	float amp = ent->curstate.body / 100.0f;
	float blend = CL_FxBlend(ent) / 255.0f;
	float r, g, b;
	int beamFlags;

	r = ent->curstate.rendercolor.r / 255.0f;
	g = ent->curstate.rendercolor.g / 255.0f;
	b = ent->curstate.rendercolor.b / 255.0f;

	R_BeamSetup(&beam, ent->origin, ent->curstate.angles, ent->curstate.modelindex, 0, ent->curstate.scale, amp, blend, ent->curstate.animtime);
	R_BeamSetAttributes(&beam, r, g, b, ent->curstate.framerate, ent->curstate.frame);
	beam.pFollowModel = NULL;

	switch (ent->curstate.rendermode & 0x0F)
	{
	case BEAM_ENTPOINT:
		beam.type = TE_BEAMPOINTS;
		if (ent->curstate.sequence)
		{
			SetBits(beam.flags, FBEAM_STARTENTITY);
			beam.startEntity = ent->curstate.sequence;
		}
		if (ent->curstate.skin)
		{
			SetBits(beam.flags, FBEAM_ENDENTITY);
			beam.endEntity = ent->curstate.skin;
		}
		break;
	case BEAM_ENTS:
		beam.type = TE_BEAMPOINTS;
		SetBits(beam.flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY);
		beam.startEntity = ent->curstate.sequence;
		beam.endEntity = ent->curstate.skin;
		break;
	case BEAM_HOSE:
		beam.type = TE_BEAMHOSE;
		break;
	case BEAM_POINTS:
		// already set up
		break;
	}

	beamFlags = (ent->curstate.rendermode & 0xF0);

	if (FBitSet(beamFlags, BEAM_FSINE))
		SetBits(beam.flags, FBEAM_SINENOISE);

	if (FBitSet(beamFlags, BEAM_FSOLID))
		SetBits(beam.flags, FBEAM_SOLID);

	if (FBitSet(beamFlags, BEAM_FSHADEIN))
		SetBits(beam.flags, FBEAM_SHADEIN);

	if (FBitSet(beamFlags, BEAM_FSHADEOUT))
		SetBits(beam.flags, FBEAM_SHADEOUT);

	// draw it
	R_BeamDraw(&beam, refdef.frametime);
}


/*
==============
CL_DrawBeams

draw beam loop
==============
*/
void CL_DrawBeams(bool fTrans)
{
	glShadeModel(GL_SMOOTH);
	glDepthMask(fTrans ? GL_FALSE : GL_TRUE);

	// draw temporary entity beams
	for (auto& pBeam : g_pClientBeams)
	{
		if (fTrans && FBitSet(pBeam->flags, FBEAM_SOLID))
			continue;

		if (!fTrans && !FBitSet(pBeam->flags, FBEAM_SOLID))
			continue;

		R_BeamDraw(pBeam, refdef.frametime);
	}

	glShadeModel(GL_FLAT);
	glDepthMask(GL_TRUE);
}

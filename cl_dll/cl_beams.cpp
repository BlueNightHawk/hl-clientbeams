#include <algorithm>

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

// Beams
CLIENTBEAM* R_BeamLightning(Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed);
CLIENTBEAM* R_BeamEnts(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
CLIENTBEAM* R_BeamPoints(Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
CLIENTBEAM* R_BeamCirclePoints(int type, Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
CLIENTBEAM* R_BeamEntPoint(int startEnt, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
CLIENTBEAM* R_BeamRing(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
CLIENTBEAM* R_BeamFollow(int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness);
void R_BeamKill(int deadEntity);

// init beam api
beam_api_s cl_beam_api =
{
	R_BeamLightning, 
	R_BeamEnts, 
	R_BeamPoints, 
	R_BeamCirclePoints, 
	R_BeamEntPoint, 
	R_BeamRing, 
	R_BeamFollow, 
	R_BeamKill
};

beam_api_s* gBeamAPI = &cl_beam_api;

std::vector<CLIENTBEAM*> g_pClientBeams;

/*
==============
R_BeamAlloc

==============
*/
static CLIENTBEAM* R_BeamAlloc(void)
{
	CLIENTBEAM* pBeam = new CLIENTBEAM;

	if (!pBeam)
		return nullptr;

	*pBeam = {};
	pBeam->die = gEngfuncs.GetClientTime();

	g_pClientBeams.push_back(pBeam);

	return pBeam;
}

/*
==============
R_BeamKill

Remove beam attached to specified entity
and all particle trails (if this is a beamfollow)
==============
*/
void R_BeamKill(int deadEntity)
{
	for (auto& beam : g_pClientBeams)
	{
		if (FBitSet(beam->flags, FBEAM_STARTENTITY) && beam->startEntity == deadEntity)
		{
			if (beam->type != TE_BEAMFOLLOW)
				beam->die = gEngfuncs.GetClientTime();

			ClearBits(beam->flags, FBEAM_STARTENTITY);
		}

		if (FBitSet(beam->flags, FBEAM_ENDENTITY) && beam->endEntity == deadEntity)
		{
			beam->die = gEngfuncs.GetClientTime();
			ClearBits(beam->flags, FBEAM_ENDENTITY);
		}
	}
}

/*
==============
R_BeamEnts

Create beam between two ents
==============
*/
CLIENTBEAM* R_BeamEnts(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude, float brightness,
	float speed, int startFrame, float framerate, float r, float g, float b)
{
	cl_entity_t *start, *end;
	CLIENTBEAM* pbeam;
	model_t* mod;

	mod = gEngfuncs.hudGetModelByIndex(modelIndex);

	// need a valid model.
	if (!mod || mod->type != mod_sprite)
		return NULL;

	start = R_BeamGetEntity(startEnt);
	end = R_BeamGetEntity(endEnt);

	if (!start || !end)
		return NULL;

	// don't start temporary beams out of the PVS
	if (life != 0 && (!start->model || !end->model))
		return NULL;

	pbeam = R_BeamLightning(vec3_origin, vec3_origin, modelIndex, life, width, amplitude, brightness, speed);
	if (!pbeam)
		return NULL;

	pbeam->type = TE_BEAMPOINTS;
	SetBits(pbeam->flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY);
	if (life == 0)
		SetBits(pbeam->flags, FBEAM_FOREVER);

	pbeam->startEntity = startEnt;
	pbeam->endEntity = endEnt;

	R_BeamSetAttributes(pbeam, r, g, b, framerate, startFrame);

	return pbeam;
}

/*
==============
R_BeamPoints

Create beam between two points
==============
*/
CLIENTBEAM* R_BeamPoints(Vector start, Vector end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	CLIENTBEAM* pbeam;

	if (life != 0 && R_BeamCull(start, end, true))
		return NULL;

	pbeam = R_BeamAlloc();
	if (!pbeam)
		return NULL;

	pbeam->die = gEngfuncs.GetClientTime();

	if (modelIndex < 0)
		return NULL;

	R_BeamSetup(pbeam, start, end, modelIndex, life, width, amplitude, brightness, speed);
	if (life == 0)
		SetBits(pbeam->flags, FBEAM_FOREVER);

	R_BeamSetAttributes(pbeam, r, g, b, framerate, startFrame);

	return pbeam;
}

/*
==============
R_BeamCirclePoints

Create beam cicrle
==============
*/
CLIENTBEAM* R_BeamCirclePoints(int type, Vector start, Vector end, int modelIndex, float life, float width,
	float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	CLIENTBEAM* pbeam = R_BeamLightning(start, end, modelIndex, life, width, amplitude, brightness, speed);

	if (!pbeam)
		return NULL;
	pbeam->type = type;
	if (life == 0)
		SetBits(pbeam->flags, FBEAM_FOREVER);
	R_BeamSetAttributes(pbeam, r, g, b, framerate, startFrame);

	return pbeam;
}


/*
==============
R_BeamEntPoint

Create beam between entity and point
==============
*/
CLIENTBEAM* R_BeamEntPoint(int startEnt, Vector end, int modelIndex, float life, float width, float amplitude,
	float brightness, float speed, int startFrame, float framerate, float r, float g, float b)
{
	CLIENTBEAM* pbeam;
	cl_entity_t* start;

	start = R_BeamGetEntity(startEnt);

	if (!start)
		return NULL;

	if (life == 0 && !start->model)
		return NULL;

	pbeam = R_BeamAlloc();
	if (!pbeam)
		return NULL;

	pbeam->die = gEngfuncs.GetClientTime();
	if (modelIndex < 0)
		return NULL;

	R_BeamSetup(pbeam, vec3_origin, end, modelIndex, life, width, amplitude, brightness, speed);

	pbeam->type = TE_BEAMPOINTS;
	SetBits(pbeam->flags, FBEAM_STARTENTITY);
	if (life == 0)
		SetBits(pbeam->flags, FBEAM_FOREVER);
	pbeam->startEntity = startEnt;
	pbeam->endEntity = 0;

	R_BeamSetAttributes(pbeam, r, g, b, framerate, startFrame);

	return pbeam;
}

/*
==============
R_BeamRing

Create beam between two ents
==============
*/
CLIENTBEAM* R_BeamRing(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude, float brightness,
	float speed, int startFrame, float framerate, float r, float g, float b)
{
	CLIENTBEAM* pbeam;
	cl_entity_t *start, *end;

	start = R_BeamGetEntity(startEnt);
	end = R_BeamGetEntity(endEnt);

	if (!start || !end)
		return NULL;

	if (life != 0 && (!start->model || !end->model))
		return NULL;

	pbeam = R_BeamLightning(vec3_origin, vec3_origin, modelIndex, life, width, amplitude, brightness, speed);
	if (!pbeam)
		return NULL;

	pbeam->type = TE_BEAMRING;
	SetBits(pbeam->flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY);
	if (life == 0)
		SetBits(pbeam->flags, FBEAM_FOREVER);
	pbeam->startEntity = startEnt;
	pbeam->endEntity = endEnt;

	R_BeamSetAttributes(pbeam, r, g, b, framerate, startFrame);

	return pbeam;
}

/*
==============
R_BeamFollow

Create beam following with entity
==============
*/
CLIENTBEAM* R_BeamFollow(int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness)
{
	CLIENTBEAM* pbeam = R_BeamAlloc();

	if (!pbeam)
		return NULL;
	pbeam->die = gEngfuncs.GetClientTime();

	if (modelIndex < 0)
		return NULL;

	R_BeamSetup(pbeam, vec3_origin, vec3_origin, modelIndex, life, width, life, brightness, 1.0f);

	pbeam->type = TE_BEAMFOLLOW;
	SetBits(pbeam->flags, FBEAM_STARTENTITY);
	pbeam->startEntity = startEnt;

	R_BeamSetAttributes(pbeam, r, g, b, 1.0f, 0);

	return pbeam;
}


/*
==============
R_BeamLightning

template for new beams
==============
*/
CLIENTBEAM* R_BeamLightning(Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed)
{
	CLIENTBEAM* pbeam = R_BeamAlloc();

	if (!pbeam)
		return NULL;
	pbeam->die = gEngfuncs.GetClientTime();

	if (modelIndex < 0)
		return NULL;

	R_BeamSetup(pbeam, start, end, modelIndex, life, width, amplitude, brightness, speed);

	return pbeam;
}

/*
==============
CL_BeamAttemptToDie

Check for expired beams
==============
*/
static bool CL_BeamAttemptToDie(CLIENTBEAM* pBeam)
{
	assert(pBeam != NULL);

	// premanent beams never die automatically
	if (FBitSet(pBeam->flags, FBEAM_FOREVER))
		return false;

	if (pBeam->type == TE_BEAMFOLLOW && pBeam->particles)
	{
		// wait for all trails are dead
		return false;
	}

	// other beams
	if (pBeam->die > gEngfuncs.GetClientTime())
		return false;

	return true;
}

/*
==============
CL_FreeDeadBeams


==============
*/
void CL_FreeDeadBeams(void)
{
	g_pClientBeams.erase(std::remove_if(g_pClientBeams.begin(), g_pClientBeams.end(),
							 [](CLIENTBEAM* pBeam)
							 { 		// retire old beams
								 if (CL_BeamAttemptToDie(pBeam))
								 {
									 R_FreeDeadParticles(&pBeam->particles);
									 delete pBeam;
									 return true;
								 }
								 return false;
							 }),
		g_pClientBeams.end());
}

/*
==============
R_DrawBeams


==============
*/
void R_DrawBeams(bool trans)
{
	if (!trans)
		CL_FreeDeadBeams();

	CL_DrawBeams(trans);
}

/*
==============
R_KillBeams


==============
*/
void R_KillBeams()
{
	for (auto& pBeam : g_pClientBeams)
	{
		R_FreeDeadParticles(&pBeam->particles);
		delete pBeam;
	}
	g_pClientBeams.clear();
}
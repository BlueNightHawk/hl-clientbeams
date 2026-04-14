#pragma once

#define FBEAM_QUADRATIC 0x00000001

typedef struct cl_beam_s CLIENTBEAM;
struct cl_beam_s
{
	int type;
	int flags;
	int clflags;
	Vector source;
	Vector target;
	Vector delta;
	Vector control;
	float t; // 0 .. 1 over lifetime of beam
	float freq;
	float die;
	float width;
	float amplitude;
	float r, g, b;
	float brightness;
	float speed;
	float frameRate;
	float frame;
	int segments;
	int startEntity;
	int endEntity;
	int modelIndex;
	int frameCount;
	struct model_s* pFollowModel;
	struct particle_s* particles;
};

struct beam_api_s
{
	CLIENTBEAM* (*R_BeamLightning)(Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed);
	CLIENTBEAM* (*R_BeamEnts)(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	CLIENTBEAM* (*R_BeamPoints)(Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	CLIENTBEAM* (*R_BeamCirclePoints)(int type, Vector start, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	CLIENTBEAM* (*R_BeamEntPoint)(int startEnt, Vector end, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	CLIENTBEAM* (*R_BeamRing)(int startEnt, int endEnt, int modelIndex, float life, float width, float amplitude, float brightness, float speed, int startFrame, float framerate, float r, float g, float b);
	CLIENTBEAM* (*R_BeamFollow)(int startEnt, int modelIndex, float life, float width, float r, float g, float b, float brightness);
	void (*R_BeamKill)(int deadEntity);
};

void R_DrawBeams(bool trans);
void R_KillBeams();

extern beam_api_s *gBeamAPI;
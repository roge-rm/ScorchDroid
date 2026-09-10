// M6: a real 3D GLES3 renderer - terrain mesh (built from the real
// heightmap, with per-vertex normals and basic directional lighting) plus
// an orbit/free-fly camera, replacing the M2/M5-era flat top-down 2D view
// (a deliberate, explicitly-scoped-down stand-in used to validate the
// NDK-reuse architecture early - see the porting plan's M6 entry for why
// a real 3D presentation matters, especially once mod content (M7) needs
// to actually be visible). No upstream rendering code (GLW/GLEXT) is
// reused here either, same as the file it replaces - this is new code,
// per the architecture decision to rewrite the client/rendering layer
// entirely. Reads live state directly from whichever ScorchedContext this
// process is driving - ScorchedServer if hosting, or a ClientContext if
// joined as a client (see EngineState.hpp/engine_jni.cpp's EngineMode).
//
// Camera: this slice ships the orbit/free-fly camera only (the default per
// the user's camera-style decision - see the porting plan). Third-person-
// follow and a host-enforced camera-mode option are follow-up work, not
// done here. Touch-to-fire-at-a-point (the old handleTap() aiming path)
// doesn't carry over as-is - a screen tap no longer has an unambiguous
// landscape-point meaning under a perspective camera without ray-casting
// against the terrain mesh (not done in this slice) - so the battlefield
// touch gesture now drives the camera (drag to orbit, pinch to zoom)
// instead. Firing is reliably handled by the angle/elevation/power sliders
// + Fire button (see GameHud.kt/MainActivity.kt), which don't depend on
// screen-to-world mapping at all.
#include <jni.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <vector>
#include <sstream>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>

#include <engine/ScorchedContext.hpp>
#include <engine/Simulator.hpp>
#include <engine/Wind.hpp>
#include <target/TargetContainer.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <landscapemap/RoofMaps.hpp>
#include <landscapedef/LandscapeDefinitionCache.hpp>
#include <landscapedef/LandscapeDefinition.hpp>
#include <landscapedef/LandscapeDefn.hpp>
#include <common/OptionsTransient.hpp>
#include <landscapedef/LandscapeTex.hpp>
#include <image/ImageFactory.hpp>
#include <target/TargetLife.hpp>
#include <tank/Tank.hpp>
#include <tank/TankState.hpp>
#include <target/TargetShield.hpp>
#include <weapons/Shield.hpp>
#include <weapons/ShieldRound.hpp>
#include <weapons/ShieldSquare.hpp>
#include <target/TargetState.hpp>
#include <actions/TargetFalling.hpp>
#include <lang/LangString.hpp>
#include <engine/ActionController.hpp>
#include <common/FixedVector.hpp>
#include <common/FixedVector4.hpp>
#include <EngineState.hpp>
#include <RenderState.hpp>
#include <Mat4.hpp>
#include <LandscapeTextureBuilder.hpp>
#include <MovementStore.h>
#include <TargetModelStore.h>
#include <SkyDescription.hpp>
#include <OceanWaves.h>
#include <ShoreBreakers.h>
#include <ParticleTextures.h>
#include <SoundEventQueue.h>
#include <landscapedef/LandscapeTex.hpp>
#include <target/TargetLife.hpp>
#include <tank/TankState.hpp>
#include <InstanceBuffer.hpp>
#include <TreeGeometry.hpp>
#include <3dsparse/TreeModelFactory.hpp>
#include <DeformEventQueue.h>
#include <EffectEventQueue.h>
#include <TracerStore.h>
// M6: real .ase tank/projectile models. The whole 3dsparse parser plus
// ModelStore are in src/common, so the actual model data is reusable -
// only the rendering of it is ours to write.
#include <3dsparse/ModelStore.hpp>
#include <3dsparse/Model.hpp>
#include <3dsparse/Mesh.hpp>
#include <3dsparse/Face.hpp>
#include <3dsparse/Vertex.hpp>
#include <common/ModelID.hpp>
#include <tank/TankModelContainer.hpp>
#include <tank/TankModel.hpp>
#include <tanket/Tanket.hpp>
#include <weapons/AccessoryStore.hpp>
#include <weapons/Accessory.hpp>
#include <weapons/WeaponProjectile.hpp>
#include <target/Target.hpp>
#include <tanket/TanketShotInfo.hpp>
#include <map>
#include <set>
#include <sys/system_properties.h>
#include <string>
#include <cstring>

#define LOG_TAG "ScorchDroidRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Guards access to ScorchedServer state shared between the simulation
// thread (tickEngine, see engine_jni.cpp) and the GL render thread.
extern std::mutex g_engineMutex;

namespace
{
	GLuint terrainProgram = 0, pointProgram = 0;
	// M6 performance: instanced scenery - see InstanceBuffer.hpp.
	GLuint instancedMeshProgram = 0;
	GLint  instancedViewProjLoc = -1, instancedLightDirLoc = -1;
	GLint  instancedFogColorLoc = -1, instancedFogDensityLoc = -1;
	// M6: upstream's trees. One geometry buffer and one atlas per tree
	// type - see porting/TreeGeometry.hpp.
	GLuint treeProgram = 0;
	GLint  treeViewProjLoc = -1, treeLightDirLoc = -1;
	GLint  treeFogColorLoc = -1, treeFogDensityLoc = -1, treeAtlasLoc = -1;
	struct TreeKind {
		GLuint vao = 0, vbo = 0, instanceVbo = 0;
		int    vertexCount = 0;
		GLuint atlas = 0;
		std::vector<float> uploaded;
	};
	std::map<int, TreeKind> g_treeKinds;   // keyed by TreeModelFactory::TreeType
	GLuint g_treeAtlases[ScorchDroidTrees::eAtlasCount] = { 0 };
	// One entry per distinct mesh drawn instanced, keyed by the source
	// geometry's VBO. Its own VAO, so adding the instance attributes never
	// disturbs the non-instanced VAO the same geometry may also be drawn
	// through. Kept across frames: the packed bytes are compared before
	// uploading, so a landscape whose scenery is not moving uploads nothing
	// at all after the first frame.
	struct InstancedDraw {
		GLuint vao = 0;
		GLuint instanceVbo = 0;
		int    instanceCount = 0;
		std::vector<float> uploaded;
	};
	std::map<GLuint, InstancedDraw> g_instancedDraws;
	GLint  terrainMvpLoc = -1, terrainMinHeightLoc = -1, terrainHeightRangeLoc = -1, terrainLightDirLoc = -1;
	GLint  terrainGroundTexLoc = -1, terrainHasTextureLoc = -1, terrainLightBakedLoc = -1;
	GLint  terrainFogColorLoc = -1, terrainFogDensityLoc = -1, terrainHalfLambertLoc = -1;
	GLint  meshFogColorLoc = -1, meshFogDensityLoc = -1;
	GLint  waterFogColorLoc = -1, waterFogDensityLoc = -1;
	GLint  waterDebugModeLoc = -1;
	GLuint groundTexture = 0;
	bool   groundTextureBuilt = false;
	// Whether the ground texture already carries the sun's lighting, in
	// which case the terrain shader must not apply its own on top.
	bool   groundLightBaked = false;
	// M6 scorch marks: the CPU-side copy of the ground texture is kept, not
	// discarded after upload, because each blast blends into the *result of*
	// every earlier one - marks accumulate over a round the way upstream's
	// do. Re-reading it back off the GPU each time would be far worse.
	LandscapeTextureBuilder::Texture groundTextureData;
	// G1: the ground texture is built on a worker thread. The GL thread
	// captures the builder's inputs under the engine lock, hands them to
	// the worker, and adopts the result when it is done - so the game does
	// not stall for the second a 1024-square build takes, and the terrain
	// draws with its height-ramp fallback until then. A job is tagged with
	// the generation it was started for, so a landscape or setting change
	// mid-build simply discards it.
	struct GroundJob {
		unsigned int generation = 0;
		int size = 0;
		bool bake = false;
		LandscapeTextureBuilder::Inputs inputs;
		LandscapeTextureBuilder::Texture ground;
		LandscapeTextureBuilder::Texture detail;
		bool lightBaked = false;
		std::string error;
		bool done = false;
	};
	std::thread groundThread;
	std::mutex groundMutex;
	GroundJob groundJob;              // guarded by groundMutex once the thread runs
	unsigned int groundGeneration = 0;
	bool   groundJobRunning = false;
	// G2: the landscape's <detail> image, tiled every 16 units over the
	// ground, the surround and the cavern roof - see the terrain shader.
	GLuint detailTexture = 0;
	GLint  terrainDetailTexLoc = -1, terrainHasDetailLoc = -1;
	// M6 tank movement: the version of ScorchDroidMovement's mask currently
	// painted over the ground, and whether anything is painted at all. The
	// mask itself is recomputed on the simulation side (see engine_jni's
	// refreshMovementMask); this side only notices the version change and
	// re-uploads.
	// M6 trees. TreeModelFactory::createModel returns an *empty* Model - a
	// bounding box and nothing else - because upstream generates tree
	// geometry procedurally in its own client renderer (ModelRendererTree,
	// ~880 lines of triangle fans) rather than loading a mesh. So every
	// tree target arrived here with no vertices to draw, which is why a
	// landscape's scenery was invisible: on one map, 1027 of 1029 targets
	// were trees.
	//
	// Built once, in the same conical shape upstream draws (stacked cones
	// for the canopy over a trunk), and drawn per instance with its own
	// colour and scale.
	GLuint spriteProgram = 0, spriteVao = 0, spriteVbo = 0;
	GLint  spriteMvpLoc = -1, spriteSamplerLoc = -1, spriteTintLoc = -1;
	GLuint sunTexture = 0;
	// Stars share the cloud layer's geometry and shader - same plane, same
	// tiling - but never scroll, which is what "skytexturestatic" means.
	GLuint starTexture = 0;

	GLuint cloudProgram = 0, cloudVao = 0, cloudVbo = 0, cloudTexture = 0;
	GLint  cloudMvpLoc = -1, cloudScrollLoc = -1, cloudTexScaleLoc = -1;
	GLint  cloudSamplerLoc = -1, cloudTintLoc = -1, cloudOpacityLoc = -1;
	GLint  cloudFogColorLoc = -1, cloudFogDensityLoc = -1, cloudEyePosLoc = -1;
	GLint  skyFogColorLoc = -1, skyFogDensityLoc = -1, skyEyeHeightLoc = -1;
	bool   cloudsBuilt = false, cloudsVisible = false;
	float  cloudScrollX = 0.0f, cloudScrollY = 0.0f;


	GLuint shadowProgram = 0, shadowVao = 0, shadowVbo = 0;
	GLint  shadowMvpLoc = -1, shadowStrengthLoc = -1;

	// M6 sky.
	GLuint skyProgram = 0, skyVao = 0, skyVbo = 0;
	GLint  skyGradientLoc = -1, skySunDirLoc = -1, skySunColorLoc = -1, skyGlowLoc = -1;
	GLint  skyFlashLoc = -1, skySunDiscLoc = -1;
	bool   skyBuilt = false;
	ScorchDroidSky::Description skyDescription;

	// The water surface: upstream's sea, see the notes at oceanTexture and
	// waterGridEbo below.
	GLuint waterProgram = 0;
	GLint  waterMvpLoc = -1, waterUpwellTopLoc = -1, waterUpwellBotLoc = -1;
	GLint  waterHeightLoc = -1, waterSunDiffuseLoc = -1;
	GLint  waterNoise0Loc = -1, waterNoise1Loc = -1;
	GLint  waterShadowTexLoc = -1, waterShadowMatrixLoc = -1, waterShadowEnabledLoc = -1;
	GLint  waterAlphaLoc = -1, waterTimeLoc = -1;
	GLint  waterSkyHorizonLoc = -1, waterSkyZenithLoc = -1, waterEyePosLoc = -1;
	bool   waterBuilt = false;    // one attempt per landscape, success or not
	bool   waterVisible = false;  // this landscape actually has water
	float  waterHeight = 0.0f;
	// Upstream's upwelling pair - the colour the sea's own body shows,
	// bottom in a trough and top on a crest.
	float  waterUpwellBot[3] = { 0.29f, 0.56f, 0.91f };
	float  waterUpwellTop[3] = { 0.49f, 0.83f, 0.94f };
	float  waterAlpha = 1.0f;
	// W2: the displaced part of the surface. The flat skirt above still
	// covers out to the far plane; this grid is the near water that moves.
	GLuint waterGridVao = 0, waterGridVbo = 0;
	// W4: the ocean tile, and the worker that keeps it up to date. The
	// generator is a couple of milliseconds of FFT per update, which is
	// fine off the GL thread and not fine on it.
	GLint terrainClipEnabledLoc = -1, terrainClipBelowLoc = -1;
	GLint terrainShadowMatrixLoc = -1, terrainShadowTexLoc = -1, terrainShadowEnabledLoc = -1;
	GLint terrainAmbienceLoc = -1, terrainDiffuseLoc = -1, terrainSunPosLoc = -1;
	// W3: the reflection this port draws the water with when upstream's own
	// reflection is asked for - the scene mirrored in the water plane,
	// rendered into a half-size target and sampled by the water shader.
	GLuint reflectionFbo = 0, reflectionTexture = 0, reflectionDepth = 0;
	int    reflectionWidth = 0, reflectionHeight = 0;
	// 0 = this port's sky colour, 1 = the sky and the land, 2 = everything
	// solid in the scene, which is what upstream reflects.
	std::atomic<int> g_reflectionStyle{0};
	GLint  waterReflectTexLoc = -1, waterUseReflectLoc = -1;
	// Upstream's own reflection texture coordinate: the water vertex pushed
	// out along its normal onto a "virtual plane" and then projected through
	// the *real* camera (water.vshader, and the texture matrix
	// VisibilityPatchGrid sets for unit 1 - bias * proj * modelview). That is
	// what makes the distortion a displacement in the world rather than a
	// nudge in screen space, so a wave bends the reflection of the thing it
	// is actually in front of.
	GLint  waterReflectMatrixLoc = -1;

	// The sun's shadow map. Upstream renders the scene's depth from the sun
	// into a 2048-square depth texture (Landscape::drawShadows) and samples it
	// with shadow2DProj in both its land and its water shader, which is what
	// puts an island's shadow on the sea beside it.
	//
	// It also decides the terrain's lighting: upstream bakes its light map
	// into the ground texture *only* when the hardware has no shadows
	// (Landscape.cpp: `if (!GLStateExtension::hasHardwareShadows())`), and
	// otherwise lights the terrain per fragment against this map. Both paths
	// are here, chosen by the same setting, so the terrain is never lit twice.
	GLuint shadowFbo = 0, shadowTexture = 0;
	int    shadowSize = 0;
	bool   shadowValid = false;
	Mat4   shadowMatrix = Mat4::identity();
	// 0 = off (bake the light map instead, as upstream does without shadows),
	// 1 = 1024, 2 = upstream's own 2048.
	std::atomic<int> g_shadowLevel{2};
	// What the ground texture currently in hand was built for, so a changed
	// setting rebuilds it rather than leaving the terrain lit twice or not
	// at all.
	bool   groundBakedForShadows = false;

	// W4/W6/W7: the ocean tile as two textures. oceanTexture is RGB16F -
	// height, x displacement, z displacement, world units - which the
	// vertex shader displaces the grid by. oceanNormalTexture is RGBA8 -
	// the displaced surface's normal, encoded n * 127 + 128 exactly as
	// upstream's Water2Patches::generateNormalMap does, with the whitecap
	// foam amount in alpha (W10a). The vertex shader reads it for the
	// vertex normal; the fragment shader reads it *again* at 1/8 and 1/32
	// scale for the fine detail, which is what upstream's "noise" layers
	// really are - its own sea, small. Mipmapped for that reason: the
	// 16-unit layer is a few texels per pixel at distance.
	GLuint oceanTexture = 0, oceanNormalTexture = 0;
	GLint  waterWaveTexLoc = -1, waterWaveNormalTexLoc = -1, waterWaveTileLoc = -1;
	// W10a: the landscape's own <foam> bitmap, which upstream tiles 25
	// times across the map and reads the blue channel of to break the
	// foam amount up into flecks (water.fshader's tex_foamamount.z).
	GLuint waterFoamMaskTexture = 0;
	GLint  waterFoamMaskLoc = -1;
	// W10c: upstream's breakers (WaterWaves.cpp) - sprite quads along
	// every shoreline, three phases of each of two images on a six-second
	// cycle, additively blended, riding the wave height. The segments
	// come from porting/ShoreBreakers; here they are one VBO (set 0's
	// quads then set 1's), a shader that slides each quad seaward and
	// back by the phase, and the two images.
	GLuint breakerProgram = 0, breakerVao = 0, breakerVbo = 0;
	GLuint breakerTexture[2] = { 0, 0 };
	int    breakerVertexCount[2] = { 0, 0 };
	GLint  breakerMvpLoc = -1, breakerFrontLoc = -1, breakerEndLoc = -1, breakerAlphaLoc = -1;
	GLint  breakerWaterHeightLoc = -1, breakerTileLoc = -1, breakerWindLoc = -1;
	GLint  breakerWaveTexLoc = -1, breakerTextureLoc = -1, breakerMapHeightLoc = -1;
	float  breakerTime = 0.0f;         // upstream's totalTime_, 0..6
	bool   breakersDirty = false;      // a crater moved the shoreline
	double breakersRebuiltAt = 0.0;
	// Water detail, the one water setting: 2 = Full (upstream's 2-unit grid,
	// 24 tile updates a second - its own phase rate), 1 = Half (4 units,
	// 12/s), 0 = Quarter (8 units, 6/s). Low is the low number, as with
	// every setting slider.
	std::atomic<int> g_waterDetail{2};
	std::thread oceanThread;
	std::atomic<bool> oceanRunning{false};
	std::mutex oceanMutex;
	ScorchDroidOcean::Tile oceanReady;    // guarded by oceanMutex
	bool oceanHasNew = false;             // guarded by oceanMutex
	float oceanSeededSpeed = -1.0f, oceanSeededDirection = 0.0f;
	std::vector<float> oceanUpload;       // reused, so no per-update allocation
	std::vector<unsigned char> oceanNormalUpload;
	// Upstream's second ripple layer runs on its own wind: the round's
	// speed plus a random offset in [-1, 1], the direction jittered by up
	// to 0.2 on each axis (Water2Renderer::generate). Rolled once per
	// landscape so it holds for a round.
	float  waterWind2SpeedOffset = 0.0f, waterWind2JitterX = 0.0f, waterWind2JitterZ = 0.0f;
	int oceanUploadLogsLeft = 3;
	// W11: the surface is one indexed mesh in two parts. The inner grid
	// covers the map and 64 units around it at the Water detail cell size
	// (2 units at Full - upstream's own vertex spacing - 4 at Half, 8 at
	// Quarter). The outer ring runs from there to the far plane at 16-unit
	// cells. Both are displaced by the tile; the seam between them is
	// closed with stitch fans (each 16-unit outer edge fanned to the inner
	// vertices along it) so that every triangle edge is shared exactly and
	// a wave cannot open a crack. No amplitude fade anywhere - upstream
	// never fades - and the far ring samples a coarser mip of the tile so
	// that its 16-unit vertices see a smoothed sea rather than every
	// eighth texel of a rough one.
	GLuint waterGridEbo = 0;
	int    waterInnerIndexCount = 0, waterOuterIndexCount = 0;
	int    waterGridBuiltForDetail = -1;
	float  waterInnerLod = 0.0f;
	GLint  waterWaveAmpLoc = -1, waterWaveLodLoc = -1;
	GLint  waterSunDirLoc = -1;
	GLint  waterMapSizeLoc = -1;

	// M6: SkyFlash. Seconds of flash left; the sky pass lifts its colour
	// by whatever remains, so a nuke whites out the whole view briefly.
	constexpr float kSkyFlashSeconds = 0.45f;
	float  skyFlashRemaining = 0.0f;

	// M6 perf readout. Draw calls are counted rather than estimated because
	// the number that matters (one per landscape target) is exactly the one
	// that is easy to be wrong about by an order of magnitude.
	int   frameDrawCalls = 0;
	int   lastTargetsDrawn = 0;
	int   lastFrameDrawCalls = 0;
	float smoothedFps = 0.0f;
	std::mutex g_statsMutex;

	unsigned int paintedMovementVersion = 0;
	bool movementOverlayPainted = false;
	GLint  pointMvpLoc = -1, pointColorLoc = -1, pointSizeLoc = -1;
	GLuint meshProgram = 0;
	GLint  meshMvpLoc = -1, meshLightDirLoc = -1, meshColorLoc = -1;
	// V2/V3: upstream's fixed-function model light and its textures.
	GLint  meshModelLoc = -1, meshViewLoc = -1, meshSunPosLoc = -1;
	GLint  meshSkyAmbientLoc = -1, meshSkyDiffuseLoc = -1;
	GLint  meshMatAmbientLoc = -1, meshMatDiffuseLoc = -1, meshMatEmissiveLoc = -1;
	GLint  meshLightModeLoc = -1, meshHasTextureLoc = -1, meshSphereMapLoc = -1, meshTextureLoc = -1;
	GLint  instancedViewLoc = -1, instancedSunPosLoc = -1;
	GLint  instancedSkyAmbientLoc = -1, instancedSkyDiffuseLoc = -1;
	GLint  instancedMatAmbientLoc = -1, instancedMatDiffuseLoc = -1, instancedMatEmissiveLoc = -1;
	GLint  instancedHasTextureLoc = -1, instancedSphereMapLoc = -1, instancedTextureLoc = -1;
	GLint  treeSunPosLoc = -1, treeSkyAmbientLoc = -1, treeSkyDiffuseLoc = -1;
	GLuint sightProgram = 0, sightVao = 0, sightVbo = 0;
	// M22: the "original" sight, upstream's own (TargetRendererImplTank::
	// drawSight). Three pieces, because each lives in a different frame:
	// the protractor ring lies flat under the tank, the bearing marker and
	// the elevation arc turn with the turret, and the barrel blade
	// additionally lifts with the gun.
	GLuint sightRingVao = 0, sightRingVbo = 0;
	GLuint sightBearingVao = 0, sightBearingVbo = 0;
	GLuint sightBarrelVao = 0, sightBarrelVbo = 0;
	int sightRingVertexCount = 0, sightBearingVertexCount = 0, sightBarrelVertexCount = 0;
	// 0 = this port's own blade, 1 = upstream's arrangement.
	std::atomic<int> g_sightStyle{0};
	GLint  sightMvpLoc = -1, sightFogColorLoc = -1, sightFogDensityLoc = -1;

	GLuint particleProgram = 0, particleVao = 0, particleVbo = 0;
	GLint  particleMvpLoc = -1, particleFogColorLoc = -1, particleFogDensityLoc = -1;
	GLuint beamVao = 0, beamVbo = 0;

	// M6 effects: live particles and beams spawned from the engine's own
	// weapon-effect events (see EffectEventQueue.h). Everything here is
	// presentation only - the simulation already happened, this just shows
	// it - so these are plain CPU-side lists rebuilt into a vertex buffer
	// each frame rather than anything the engine can see.
	struct Particle {
		float x, y, z;     // render space (y up)
		float vx, vy, vz;
		float r, g, b;
		float worldSize;   // radius in world units, converted to pixels at draw time
		float age, life;   // seconds
		float drag;        // per-second velocity retention, 1 = none
		// The four below exist for smoke, which behaves unlike every other
		// particle here: it rises rather than falls, spreads much further,
		// is only partly opaque, and darkens what is behind it instead of
		// lighting it. The defaults are the old hard-coded behaviour, so a
		// spark or a debris fleck is unaffected.
		float gravityScale = 1.0f;   // <0 rises
		float growth = 0.8f;         // fraction of its own size gained over its life
		float peakAlpha = 1.0f;      // opacity at birth
		bool  alphaBlend = false;    // false = additive
		// X1: whether the wind pushes it. Upstream's emitters set this per
		// effect: missile flame and smoke, landscape smoke, splash spray,
		// rain and snow are blown; explosions, lasers, teleports are not.
		bool  windAffect = false;
		// Upstream integrates position += velocity * mass * time, so a
		// heavier particle is moved less by the same velocity - including
		// the wind's. The velocities here already have the mass folded in;
		// this is kept so the wind term can fold it in too.
		float mass = 1.0f;
		// 0 = a round sprite; 1 = rain, drawn as a short vertical streak
		// (upstream's ParticleRendererRain, a 0.1-wide quad about a unit
		// tall); 2 = snow, a small sprite. Both fade with distance from
		// the camera rather than with age, and die below the ground plane.
		int   kind = 0;   // 3 = a mushroom-cloud puff, positioned from its path
		// V1: which layer of the sprite array this particle draws with
		// (0 is the soft disc), how many frames its set has from that
		// layer, whether the frames run by age (framesPerSecond > 0, the
		// napalm) or by life (the animated explosions), a random start
		// frame, and one of upstream's four texture orientations.
		int   layer = 0;
		int   frames = 1;
		float framesPerSecond = 0.0f;
		int   frameOffset = 0;
		int   orient = 0;
		// Upstream's alpha runs linearly from its start value to an end
		// value over the life; the disc particles keep the old late fade.
		float endAlpha = 0.0f;
		bool  linearAlpha = false;
		// Mushroom puffs: where the cloud started, the puff's own spread
		// direction and the blast size, for ExplosionNukeRendererEntry's
		// path.
		float startX = 0.0f, startY = 0.0f, startZ = 0.0f;
		float spreadX = 0.0f, spreadZ = 0.0f;
		float mushroomSize = 0.0f;
	};

	// V1: upstream's nuke cloud (ExplosionNukeRenderer): from 1.25 s to
	// 2.25 s after the blast, every 0.08 s, a handful of puffs are raised
	// at the cloud's base and then carried along a fixed rise-then-spread
	// path over 16/3 s. One of these per cloud, stepped each frame.
	struct MushroomEmitter {
		float x, y, z;       // render space, the base (blast lowered by size, clamped to ground)
		float size;
		int   layer, frames; // the explosion's set
		float time = 0.0f, accumulator = 0.0f;
	};

	// X4a: upstream's damaged-tank smoke (TargetRendererImplTank::simulate):
	// a tank below full life puffs from its turret every
	// (rand * life * 10 + 250) / 3000 seconds - the healthier, the rarer.
	struct TankSmoke { float time = 0.0f, waitFor = 0.0f; };
	std::map<unsigned int, TankSmoke> tankSmoke;

	// X4c: precipitation, from the landscape's <precipitation>: 0 none,
	// 1 rain, 2 snow, and how many particles per tenth of a second
	// (upstream's TargetCamera::simulate emits `particles` every 0.1 s
	// within 200 units of the camera at a height of 180).
	int   precipitationKind = 0;
	int   precipitationCount = 0;
	float precipitationAccumulator = 0.0f;

	struct Beam {
		float x1, y1, z1, x2, y2, z2;  // render space
		float r, g, b;
		float age, life;
	};

	std::vector<Particle> particles;
	std::vector<Beam> beams;
	std::vector<MushroomEmitter> mushroomEmitters;
	// V1: the sprite array (see ParticleTextures.h), one 128-square layer
	// per texture, and the CPU copy it is rebuilt from after a context loss.
	GLuint spriteArrayTexture = 0;
	GLint  particleSpritesLoc = -1;
	ScorchDroidParticleTextures::Atlas spriteAtlas;
	double lastFrameSeconds = 0.0;
	// When the camera last updated, for the occlusion ease-out's timestep.
	double lastCameraSeconds = 0.0;

	// A hard stop so a napalm field can't grow the buffer without limit.
	//
	// Upstream's own budget, and upstream's own setting: ScorchedClient sizes
	// its particle engine from OptionsDisplay's effects detail - 100 at low,
	// 6000 at normal, 10000 at high - and once the pool is full its emitters
	// simply get nothing back, which is exactly what addParticle does here.
	// So capping the pool is upstream's throttle, not an approximation of it.
	// This was a fixed 4000, below upstream's ordinary setting, which is one
	// of the reasons a big fire thinned out.
	std::atomic<int> g_maxParticles{6000};
	const size_t kMaxBeams = 512;
	int    sightVertexCount = 0;

	GLuint terrainVao = 0, terrainVbo = 0, terrainIbo = 0;
	GLuint pointVao = 0, pointVbo = 0;

	bool   terrainBuilt = false;
	// M6: which landscape the current mesh/texture were built for. The
	// landscape changes every round, so a one-shot "build once" would leave
	// round 2 onwards rendering round 1's terrain. getDefinitionNumber() is
	// a stable per-landscape id (0 = none chosen yet, which is the blank
	// landscape the very first round uses).
	unsigned int builtDefinitionNumber = 0xffffffffu;
	int    terrainIndexCount = 0;
	float  terrainMinHeight = 0.0f, terrainMaxHeight = 1.0f;
	float  mapWidthUnits = 1.0f, mapHeightUnits = 1.0f;

	// Landscape (x, y, height) -> render (x, height, z), the single place
	// that mapping is defined.
	//
	// World Z runs *opposite* to landscape y. Mapping y straight onto Z
	// would swap two axes without negating either, which is a reflection
	// (determinant -1): the whole scene would be drawn as a mirror image of
	// the world the engine is simulating, every rotation about "up" would
	// come out backwards, and triangle winding would invert. It was built
	// that way at first, and the mirror was paid for twice - a negated
	// turret heading and a negated gun elevation, each "fixed" by
	// measurement without the cause being understood.
	//
	// Subtracting from the map height rather than plain negation keeps the
	// world in the same 0..mapSize box it was always in, so the camera
	// target, its pan clamps and the pick bounds all keep working
	// unchanged. Landscape y = 0 is at the far side of the world (world Z =
	// mapHeight) and landscape y = mapHeight is nearest the origin.
	inline float worldZFromEngineY(float engineY) { return mapHeightUnits - engineY; }
	inline float engineYFromWorldZ(float worldZ) { return mapHeightUnits - worldZ; }

	// M6 terrain destruction: the sampled grid is kept around after the
	// initial build so a crater can re-sample and re-upload just the
	// vertices it touched (see applyTerrainDeformations) instead of
	// rebuilding the whole mesh. terrainGrid^2 quads => (terrainGrid+1)^2 verts.
	// M23: the resolution the landscape is drawn at, and a setting rather
	// than a number chosen once.
	//
	// This was a fixed 96, sampled out of a heightmap that is 256 across -
	// so six of every seven heights upstream generated never reached the
	// GPU, and small features between grid points simply did not exist in
	// the drawn surface. Upstream draws the whole map (with level of detail,
	// which this port has no equivalent of), so the default here is the
	// whole map too, and the slider is for the device that cannot afford it.
	constexpr int kTerrainGridMax = 256;
	constexpr int kTerrainGridMin = 32;
	// What the settings screen has asked for; read on the next build.
	std::atomic<int> g_requestedTerrainGrid{kTerrainGridMax};
	// What the mesh currently on the GPU was actually built with - clamped
	// to the map's own resolution, since sampling finer than the heightmap
	// only duplicates vertices.
	int terrainGrid = kTerrainGridMax;
	int terrainVerts1D = kTerrainGridMax + 1;

	// The grid to build with: what the player asked for, but never finer
	// than the heightmap itself - past that the extra vertices carry no new
	// information.
	int requestedGridFor(ScorchedContext &ctx)
	{
		HeightMap &heightMap = ctx.getLandscapeMaps().getGroundMaps().getHeightMap();
		const int source = std::max(heightMap.getMapWidth(), heightMap.getMapHeight());
		int wanted = g_requestedTerrainGrid.load();
		if (source > 0) wanted = std::min(wanted, source);
		return std::min(std::max(wanted, kTerrainGridMin), kTerrainGridMax);
	}

	// Which heightmap cell a mesh grid vertex samples. The rows run
	// backwards for the same reason worldZFromEngineY subtracts: grid row 0
	// sits at world Z = 0, which is landscape y = mapHeight.
	inline int heightMapRowForGridZ(int gz, int mapH) {
		return std::min(std::max(mapH - gz * mapH / terrainGrid, 0), mapH - 1);
	}
	inline int heightMapColForGridX(int gx, int mapW) {
		return std::min(std::max(gx * mapW / terrainGrid, 0), mapW - 1);
	}
	// ...and back, for turning a deformed heightmap region into the grid
	// rows that need re-sampling.
	inline int gridZForHeightMapRow(int row, int mapH) {
		return (mapH > 0) ? ((mapH - row) * terrainGrid / mapH) : 0;
	}
	inline int gridXForHeightMapCol(int col, int mapW) {
		return (mapW > 0) ? (col * terrainGrid / mapW) : 0;
	}
	// M6 S6: the cavern roof. A landscape's <roof> is either type="sky",
	// which is LandscapeDefnTypeNone and means no roof at all (33 of the 36
	// shipped landscapes), or type="cavern", which generates a second real
	// heightmap hanging over the map - RoofMaps::generateRMap flips it so
	// its heights are absolute world heights below the cavern's <height>.
	// Only defncavern, defncavern2 and defnicebergs2 use one.
	//
	// It is drawn through the terrain program: same vertex layout, same
	// fog, just the other heightmap, downward normals and reversed winding.
	GLuint roofVao = 0, roofVbo = 0, roofIbo = 0, roofTexture = 0;
	int    roofIndexCount = 0;
	bool   roofBuilt = false, roofVisible = false;
	float  roofMinHeight = 0.0f, roofMaxHeight = 1.0f;
	// How often the roof image tiles across the map, so the skirt beyond the
	// map edge can carry on from the same texture coordinates.
	float  roofUScale = 1.0f, roofVScale = 1.0f;
	// The wall that closes the cavern off beyond the map edge - see
	// buildRoofSkirt. Non-indexed, and drawn double-sided.
	GLuint roofSkirtVao = 0, roofSkirtVbo = 0;
	int    roofSkirtVertexCount = 0;

	// The land surround: upstream's LandSurround, a flat apron of ground at
	// height 0 filling the world beyond the map edge. Water maps hide the
	// edge behind the sea and caverns behind the roof skirt, but a landscape
	// with neither just stopped.
	GLuint surroundVao = 0, surroundVbo = 0, surroundTexture = 0;
	int    surroundVertexCount = 0;
	bool   surroundBuilt = false, surroundVisible = false;

	// One-shot confirmations for the two effects that are hard to catch on
	// screen - see where each is set. Cleared with the landscape.
	bool loggedShieldHit = false, loggedParachute = false;

	constexpr int kTerrainFloatsPerVertex = 8;  // pos(3) + normal(3) + uv(2)
	std::vector<float> terrainHeights;          // terrainVerts1D^2, row-major by gz
	// G4: the heightmap's own normals (HeightMap::getNormal, upstream's
	// average of cross products at distances 1 and 3), in render axes,
	// three per vertex. The mesh used to take a central difference of its
	// sampled heights, which shaded slightly sharper than upstream and
	// from different normals than the ground texture was built from.
	std::vector<float> terrainNormals;
	std::vector<float> terrainWorldX, terrainWorldZ;
	int    terrainSrcWidth = 0, terrainSrcHeight = 0;
	// Set when a new landscape is built, cleared once the free-fly camera
	// has been pointed at "my tank" for that round. One-shot, so it never
	// fights the player's own panning afterwards.
	// M6 name plates / health bars. Upstream draws these in world space with
	// its own GL font atlas (TargetRendererImplTank::drawNames/drawLife);
	// this port has no font renderer and its whole UI layer is Compose, so
	// the renderer publishes projected screen positions instead and Kotlin
	// draws the text. Guarded by its own mutex: written on the GL thread,
	// read from the UI thread's poll.
	struct TankOverlay {
		float screenX = 0.0f, screenY = 0.0f;
		bool  onScreen = false;
		bool  alive = false;
		bool  mine = false;
		float life = 0.0f;    // 0..1
		float shield = 0.0f;  // 0..1, 0 when no shield is up
		float r = 1.0f, g = 1.0f, b = 1.0f;
		std::string name;
	};
	std::mutex g_overlayMutex;
	std::vector<TankOverlay> g_tankOverlays;

	// M6: short-lived labels pinned to a world position - the floating
	// damage numbers upstream draws over a hurt target, and the speech
	// bubble over a tank that just spoke. Both are *text*, which this
	// renderer cannot draw: it has no font. So they are projected here and
	// handed to Compose, the same route the tank name plates already take.
	struct FloatingLabel {
		float x, y, z;      // world, render space
		std::string text;
		float age = 0.0f, life = 1.0f;
		float r = 1.0f, g = 1.0f, b = 1.0f;
		// Filled per frame by the projection below.
		float screenX = 0.0f, screenY = 0.0f;
		bool  onScreen = false;
	};
	std::vector<FloatingLabel> floatingLabels;
	std::vector<FloatingLabel> g_labelOverlays;   // published snapshot

	// M11: renderer options from the settings screen. Atomic because the UI
	// thread writes them while the GL thread reads them every frame; plain
	// bools would be a data race for no gain.
	std::atomic<bool> g_showTrees{true};
	std::atomic<bool> g_showFog{true};
	std::mutex g_labelMutex;
	// Bounded: a multi-target blast raises one number per target hurt, and
	// nothing downstream depends on seeing every one.
	const size_t kMaxFloatingLabels = 48;

	// M6: upstream's thrown rocks. Opaque tumbling meshes flung alongside
	// the fireball - being solid and dark they read against bright ground,
	// where the additive fireball sprites simply wash out.
	//
	// Drawn through the ordinary mesh program one at a time rather than
	// instanced, because each tumbles about its own random axis and the
	// instance format carries only a rotation about world up. They are
	// short-lived and capped, so this is a bounded handful of draws rather
	// than the unbounded per-target cost instancing was added to remove.
	struct DebrisChunk {
		float x, y, z;
		float vx, vy, vz;
		float axisX, axisY, axisZ;   // unit, the tumble axis
		float angle, spin;           // radians, radians/second
		float scale;
		int   mesh;                  // 0 = rock1, 1 = rock2
		float age = 0.0f, life = 1.0f;
	};
	std::vector<DebrisChunk> debrisChunks;
	const size_t kMaxDebris = 120;

	// M6: upstream's arena wall flash. A shot striking the boundary lights
	// the *whole* of that side - a full-height translucent panel in the wall
	// type's colour, fading out - rather than splashing at the impact point.
	// Walls are on in most rounds (WallType defaults to WallRandom), so this
	// is ordinary play.
	//
	// One fade timer per side, exactly as upstream keeps (Wall::fadeTime_).
	float wallFade[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	// Upstream's own per-type colours (OptionsTransient::getWallColor):
	// wrap-around olive, bouncy blue, concrete grey.
	float wallColor[3] = { 0.5f, 0.5f, 0.5f };

	// M6 terrain picking. Rather than invert the MVP, the camera basis is
	// published each frame and the pick ray is rebuilt from it. The basis
	// is read out of the view matrix that was actually drawn with (its rows
	// are the camera axes), so there is no second derivation to keep in
	// step and no matrix inversion to get subtly wrong - see the publish
	// site in nativeOnDrawFrame for what a second derivation cost.
	struct PickCamera {
		bool  valid = false;
		float eyeX = 0, eyeY = 0, eyeZ = 0;
		float fwdX = 0, fwdY = 0, fwdZ = 1;
		float rightX = 1, rightY = 0, rightZ = 0;
		float upX = 0, upY = 1, upZ = 0;
		float tanHalfFov = 1.0f;
		float aspect = 1.0f;
	};
	std::mutex g_pickMutex;
	PickCamera g_pickCamera;

	bool   recentreOnMyTank = false;
	int    terrainDeformLogsLeft = 0;
	int    terrainScorchLogsLeft = 0;
	int    effectLogsLeft = 0;

	int    surfaceWidth = 1, surfaceHeight = 1;

	// Orbit/free-fly camera - the default per the user's camera-style
	// decision (see the porting plan's M6 entry). Target defaults to the
	// map center once a landscape exists; distance/pitch defaults give a
	// reasonable overview on first frame. Touched by both the GL thread
	// (read every frame) and the UI thread (nativeCameraDrag/Zoom below,
	// called from touch handling in Kotlin) - guarded by its own mutex
	// rather than g_engineMutex, since camera state has nothing to do with
	// simulation state and shouldn't contend with the sim tick.
	std::mutex g_cameraMutex;
	struct OrbitCamera {
		// Map-center target, used in free-fly mode only - follow mode
		// retargets to "my tank"'s live position every frame instead (see
		// nativeOnDrawFrame), so it doesn't need its own stored target.
		float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;
		float yaw = 0.7f;    // radians - shared by both modes
		float pitch = 0.7f;  // radians above the horizontal plane - shared by both modes
		// Separate remembered distances per mode: free-fly defaults to a
		// wide view of the whole map (set once the map size is known - see
		// buildTerrainIfNeeded), which would be a useless too-far-away
		// default for following a single tank up close, and vice versa.
		float orbitDistance = 120.0f;
		// Far enough back to frame the tank *and* the ground it is shooting
		// over - at ~2.2 units per tank this shows roughly a fifth of the
		// map, so craters, the aim sight and nearby terrain are all visible
		// without pinching out first. It was 14, which filled the screen
		// with the tank and whatever hillside it happened to be standing
		// against; pinch-zoom still goes closer for anyone who wants that.
		float followDistance = 45.0f;
		bool followMode = false;

		// M6 parity: upstream's camera presets (TargetCamera::CamType).
		// Free and Follow are this port's own two and stay exactly as they
		// were - the camera button still toggles between them, since that
		// is the one control reached mid-aim. The rest are upstream's, each
		// a fixed (yaw, elevation, distance) framing of the current tank.
		//
		// Upstream measures its elevation from the *vertical* and this
		// port's pitch from the horizontal, so its numbers arrive here as
		// (pi/2 - elevation) - which is why "Top" is a large pitch and
		// "Tank" a near-zero one, the opposite way round to how upstream
		// writes them.
		enum Preset
		{
			pFree = 0,
			pFollow,
			pTop,       // movePosition(rot, 0.174, 50)
			pBehind,    // movePosition(rot, 1.0, 60) - upstream's AboveTank
			pTank,      // movePosition(rot, 1.48, 15)
			pAction,    // movePosition(rot, 0.7, 80)
			pShot,      // follows the projectile; falls back to Behind
		};
		Preset preset = pFree;
		// The bearing the shot camera watches from. Held steady while a
		// shot is in the air - see the preset block in nativeOnDrawFrame.
		float shotYaw = 0.0f;
		// How much of the requested distance the terrain currently allows -
		// see the occlusion pull-in in nativeOnDrawFrame. 1.0 is "nothing in
		// the way". Kept between frames so the camera can ease back out
		// rather than snapping when a ridge stops blocking the view.
		float occlusionFraction = 1.0f;
	} g_camera;

	constexpr float kMinPitch = 0.15f;
	constexpr float kMaxPitch = 1.45f;
	constexpr float kMinDistance = 5.0f;
	// How far the camera stays above the ground beneath it - comfortably
	// more than the near plane, so nothing clips even on a steep slope.
	constexpr float kCameraGroundClearance = 4.0f;
	// Never pull the camera closer than this when a ridge is in the way.
	// Below about this the tank fills the frame and the shot can't be read,
	// which is worse than seeing part of the hillside.
	constexpr float kMinOcclusionDistance = 14.0f;
	// Roughly a tank's height. The camera's job is to see the tank, so the
	// sightline is drawn to its body rather than to the ground under it.
	constexpr float kTankSightHeight = 2.5f;
	// How fast the camera eases back out once the view clears, as a fraction
	// of the gap per second. Pulling in is instant (the alternative is
	// looking through a hill), backing out is smoothed, which is the usual
	// asymmetry for this kind of camera - a snap outward is far more
	// noticeable than a snap inward.
	constexpr float kOcclusionReleaseRate = 3.0f;
	constexpr float kMaxFollowDistance = 150.0f;
	constexpr float kDragSensitivity = 0.006f;  // radians per pixel
	// Vertical field of view. Shared by the projection matrix, the
	// particle world-size-to-pixels conversion and the pan scale, all of
	// which are wrong in different ways if they disagree.
	constexpr float kFovYRadians = 1.0472f;  // 60 degrees

	GLuint compileShader(GLenum type, const char *src)
	{
		GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &src, nullptr);
		glCompileShader(shader);
		GLint status = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
		if (!status) {
			char log[1024];
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			LOGE("shader compile failed: %s", log);
		}
		return shader;
	}

	// Every fragment shader here declares highp. GLSL ES 3.00 guarantees
	// highp in fragment shaders, and on a Mali GPU mediump is a real
	// 16-bit float (range 65504, ten bits of mantissa) where Adreno and
	// the emulator quietly run it at 32 bits - so mediump shaders that
	// looked right everywhere else drew a white sea and navy blocks of
	// broken water on a Moto G Stylus 2024 (Mali-G52). dan A/B'd it on
	// the phone: highp fixed both, mediump brought both back.
	//
	// `adb shell setprop debug.scorchdroid.highp 0` (then restart the app)
	// puts mediump back, for comparing the two on a device.
	bool forceMediumpFragments()
	{
		static int cached = -1;
		if (cached < 0) {
			char value[PROP_VALUE_MAX] = { 0 };
			cached = (__system_property_get("debug.scorchdroid.highp", value) > 0 && value[0] == '0') ? 1 : 0;
			if (cached) LOGI("Shaders: fragment precision forced back to mediump (debug.scorchdroid.highp=0)");
		}
		return cached == 1;
	}

	GLuint linkProgram(const char *vs, const char *fs)
	{
		std::string fsSource = fs;
		if (forceMediumpFragments()) {
			const std::string from = "precision highp float;";
			size_t at = fsSource.find(from);
			if (at != std::string::npos) fsSource.replace(at, from.size(), "precision mediump float;");
		}
		GLuint v = compileShader(GL_VERTEX_SHADER, vs);
		GLuint f = compileShader(GL_FRAGMENT_SHADER, fsSource.c_str());
		GLuint p = glCreateProgram();
		glAttachShader(p, v);
		glAttachShader(p, f);
		glLinkProgram(p);
		GLint status = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &status);
		if (!status) {
			char log[1024];
			glGetProgramInfoLog(p, sizeof(log), nullptr, log);
			LOGE("program link failed: %s", log);
		}
		glDeleteShader(v);
		glDeleteShader(f);
		return p;
	}

	const char *kTerrainVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec3 aNormal;
		layout(location = 2) in vec2 aTexCoord;
		uniform mat4 uMVP;
		uniform float uMinHeight;
		uniform float uHeightRange;
		// The sun's own view-projection, with the [-1,1] to [0,1] bias
		// already folded in - upstream's shadow texture matrix.
		uniform mat4 uShadowMatrix;
		out vec3 vNormal;
		out float vHeight01;
		out vec2 vTexCoord;
		out float vViewDepth;
		out vec4 vShadowCoord;
		out vec2 vDetailCoord;
		// G5: upstream lights the land from GL_LIGHT1 *at the sun's
		// position*, a point 900 units out from the map centre, so the
		// light direction is worked out per vertex (land.vshader:
		// lightDir = normalize(lightpos - vertex)) and varies a little
		// across the map. A single directional vector was up to ~8 degrees
		// off at the map's edges.
		uniform vec3 uSunPos;
		out vec3 vLightDir;
		// W3: for the reflection pass, which has to drop everything below
		// the waterline - GLES3 has no clip planes, so the fragment shader
		// does it.
		out float vWorldY;
		void main() {
			vLightDir = uSunPos - aPosition;
			vNormal = aNormal;
			vTexCoord = aTexCoord;
			vHeight01 = clamp((aPosition.y - uMinHeight) / uHeightRange, 0.0, 1.0);
			vShadowCoord = uShadowMatrix * vec4(aPosition, 1.0);
			// G2: one detail tile per 16 landscape units, upstream's own
			// tiling (GraphicalLandscapeMap::reset builds its second
			// texture coordinate as x/width * width/16, i.e. x/16; the
			// surround's works out the same). From the world position so
			// the one line serves the terrain, the surround and the roof.
			vDetailCoord = aPosition.xz / 16.0;
			// Order matters: gl_Position has to be written before its w can
			// be read. The other way round this reads an undefined value and
			// the terrain silently stops fogging.
			gl_Position = uMVP * vec4(aPosition, 1.0);
			vViewDepth = gl_Position.w;
			vWorldY = aPosition.y;
		}
	)";

	// M6: the ground is now the real generated landscape texture (see
	// LandscapeTextureBuilder - grass/rock/sand blended by height and
	// slope, the same way upstream builds it), lit by the same directional
	// light as before. uHasTexture falls back to the old flat height-ramp
	// colouring if the landscape definition doesn't use generated textures
	// or the images failed to load, so the ground is never invisible.
	const char *kTerrainFragmentShader = R"(#version 300 es
		precision highp float;
		in vec3 vNormal;
		in float vHeight01;
		in vec2 vTexCoord;
		in float vViewDepth;
		in vec4 vShadowCoord;
		in vec2 vDetailCoord;
		in float vWorldY;
		out vec4 fragColor;
		// G2: the landscape's own <detail> image. Upstream blends it into
		// the ground (land.fshader) and the cavern roof (SkyRoof::draw,
		// texture unit 2); without it a surface close up is the generated
		// texture magnified with nothing at high frequency in it.
		uniform sampler2D uDetailTexture;
		uniform int uHasDetail;
		// Upstream's sampler2DShadow, sampled with textureProj - the hardware
		// does the depth comparison and, with a linear filter, gives back a
		// bilinear average of four comparisons rather than a hard bit.
		uniform highp sampler2DShadow uShadowTex;
		uniform int uShadowEnabled;
		// <skyambience> and <skydiffuse>. Upstream's land shader combines them
		// as `diffuse * NdotL * shadow + ambient` and multiplies the ground
		// texture by the result.
		uniform vec3 uAmbience;
		uniform vec3 uDiffuse;
		in vec3 vLightDir;

		// 1.0 in full light, 0.0 in full shade. Outside the sun's frustum
		// there is nothing to test against, so everything there is lit -
		// clamping to the edge instead would smear the border texel across
		// the whole map.
		float sunShadow() {
			if (uShadowEnabled == 0) return 1.0;
			if (vShadowCoord.w <= 0.0) return 1.0;
			vec3 p = vShadowCoord.xyz / vShadowCoord.w;
			if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) return 1.0;
			return textureProj(uShadowTex, vShadowCoord);
		}
		// W3: when this pass is a reflection, anything under the water is
		// not in it.
		uniform int uClipEnabled;
		uniform float uClipBelowY;
		uniform vec3 uLightDir;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		uniform sampler2D uGroundTexture;
		uniform int uHasTexture;
		uniform int uLightBaked;
		// The cavern roof is the same mesh upside down, and every one of its
		// normals points away from the sun - clamped lambert would give it a
		// single flat tone and hide all its relief. Upstream shades its roof
		// with a half-lambert instead (SkyRoof::makeNormal: `dot/2 + 0.5`,
		// never zero), so this switches to the same curve for it.
		uniform int uHalfLambert;
		void main() {
			if (uClipEnabled == 1 && vWorldY < uClipBelowY) discard;
			vec3 baseColor;
			if (uHasTexture == 1) {
				baseColor = texture(uGroundTexture, vTexCoord).rgb;
			} else {
				baseColor = mix(vec3(0.22, 0.34, 0.13), vec3(0.58, 0.52, 0.42), vHeight01);
			}
			// Upstream's own blend, from land.fshader:
			//   ((groundColor * 3.5) + detailColor) / 4
			// before the lighting, so ground, surround and roof all get it.
			if (uHasDetail == 1) {
				vec3 detail = texture(uDetailTexture, vDetailCoord).rgb;
				baseColor = (baseColor * 3.5 + detail) / 4.0;
			}
			vec3 lit;
			if (uLightBaked == 1) {
				// The texture already carries the sun, the ambience and the
				// shadows hills cast on each other - lighting it again here
				// would apply the sun twice and wash the shadows out. This is
				// upstream's no-shadow path, and with shadows on it is not
				// taken: the light map is not baked at all then, exactly as
				// upstream skips it when the hardware can shadow.
				lit = baseColor;
			} else if (uShadowEnabled == 1 && uHalfLambert == 0) {
				// Upstream's own land shader, verbatim in structure:
				// lightcolor = diffuse * (N.L * shadow) + ambient, times the
				// ground texture. The sun's real direction, not the fixed
				// light the unshadowed path uses, because the shadow map is
				// cast from the sun and the two have to agree or a slope will
				// be lit from one side and shadowed from the other.
				vec3 n = normalize(vNormal);
				float ndotl = max(dot(n, normalize(vLightDir)), 0.0) * sunShadow();
				lit = baseColor * (uDiffuse * ndotl + uAmbience);
			} else {
				vec3 n = normalize(vNormal);
				float raw = dot(n, uLightDir);
				float diffuse = (uHalfLambert == 1) ? (raw * 0.5 + 0.5) : max(raw, 0.0);
				lit = baseColor * (0.55 + diffuse * 0.6);
			}
			// Upstream's exponential distance fog, exactly as its land and
			// water shaders have it: gl_FogFragCoord = max(z - 350, 0) in
			// the vertex shader, then exp2(-density * coord * 3 * 1.442695)
			// - which is exp(-3 * density * coord). Nothing is fogged inside
			// 350 units, and beyond that it thickens three times as fast as
			// the landscape's <fogdensity> alone would say. The old
			// exp(-density * depth) from zero was 30% fog at 350 units on a
			// typical map, where upstream has none, and a soft horizon
			// where upstream's sea meets the fog colour hard.
			//
			// gl_Position.w is the view distance for a standard projection,
			// so no eye position needs passing in - which is what lets
			// every shader here fog the same way with one line each.
			float fog = clamp(exp(-3.0 * uFogDensity * max(vViewDepth - 350.0, 0.0)), 0.0, 1.0);
			fragColor = vec4(mix(uFogColor, lit, fog), 1.0);
		}
	)";

	// M6: real .ase models (tanks, and later projectiles). Flat-lit with the
	// same directional light as the terrain, plus a per-instance colour so
	// "my tank" stays visually distinct from opponents the way the old
	// point sprites were.
	// V3: upstream lights every model with the fixed-function pipeline:
	// GL_LIGHT1 at the sun's position with the landscape's <skyambience>
	// and <skydiffuse> (Sun::setLightPosition), the default global ambient
	// of 0.2, no specular (GL_LIGHT1's default), and the mesh's own
	// material (ModelRendererMesh::drawMesh). That is evaluated per vertex
	// and interpolated, exactly as GL did it - Gouraud, not per fragment -
	// so a low-polygon tank shades the way it does on the PC.
	//
	// uLightMode 0 keeps the port's old flat shade for the geometry that is
	// not an upstream model at all: shields, the parachute, the tank
	// stand-ins. uSphereMap is GL_SPHERE_MAP's texgen (the chrome on the
	// semi and the MLRS): the eye-space reflection vector mapped to a disc.
	const char *kMeshVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec3 aNormal;
		layout(location = 2) in vec2 aTexCoord;
		uniform mat4 uMVP;
		uniform mat4 uModel;
		uniform mat4 uView;
		uniform vec3 uSunPos;
		uniform vec3 uSkyAmbient;
		uniform vec3 uSkyDiffuse;
		uniform vec3 uMatAmbient;
		uniform vec3 uMatDiffuse;
		uniform vec3 uMatEmissive;
		uniform int uLightMode;
		uniform int uSphereMap;
		uniform vec3 uLightDir;
		uniform vec4 uColor;
		out vec3 vLit;
		out vec2 vTexCoord;
		out float vViewDepth;
		void main() {
			vec4 world = uModel * vec4(aPosition, 1.0);
			vec3 n = normalize(mat3(uModel) * aNormal);
			if (uLightMode == 1) {
				vec3 L = normalize(uSunPos - world.xyz);
				float nl = max(dot(n, L), 0.0);
				vLit = min(uMatAmbient * (0.2 + uSkyAmbient)
						   + uMatDiffuse * uSkyDiffuse * nl
						   + uMatEmissive, vec3(1.0)) * uColor.rgb;
			} else {
				float d = max(dot(n, uLightDir), 0.0);
				vLit = uColor.rgb * (0.45 + d * 0.75);
			}
			if (uSphereMap == 1) {
				vec3 eyePos = (uView * world).xyz;
				vec3 eyeN = normalize(mat3(uView) * n);
				vec3 r = reflect(normalize(eyePos), eyeN);
				float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
				vTexCoord = vec2(r.x / m + 0.5, r.y / m + 0.5);
			} else {
				vTexCoord = aTexCoord;
			}
			gl_Position = uMVP * vec4(aPosition, 1.0);
			vViewDepth = gl_Position.w;
		}
	)";

	// M6 performance: the same mesh shading, drawn once for many copies.
	//
	// A separate program rather than a branch in the mesh one above: that
	// one is shared with tanks, shots, shields and parachutes and should
	// stay simple, and an instanced draw needs different *attributes*, not
	// just a different uniform.
	//
	// The transform is rebuilt here from eight floats instead of being sent
	// as a matrix - see InstanceBuffer.hpp for why that is enough. The
	// rotation must match Mat4::rotateY exactly or every instance turns the
	// wrong way: that matrix is x' = cx + sz, z' = -sx + cz.
	const char *kInstancedMeshVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec3 aNormal;
		layout(location = 2) in vec2 aTexCoord;
		layout(location = 3) in vec4 aInstancePosScale;
		layout(location = 4) in vec4 aInstanceRotColor;
		uniform mat4 uViewProj;
		uniform mat4 uView;
		uniform vec3 uSunPos;
		uniform vec3 uSkyAmbient;
		uniform vec3 uSkyDiffuse;
		uniform vec3 uMatAmbient;
		uniform vec3 uMatDiffuse;
		uniform vec3 uMatEmissive;
		uniform int uSphereMap;
		out vec3 vLit;
		out vec2 vTexCoord;
		out float vViewDepth;
		void main() {
			float rot = aInstanceRotColor.x;
			float s = sin(rot), c = cos(rot);

			vec3 p = aPosition * aInstancePosScale.w;
			vec3 rotated = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
			vec3 world = rotated + aInstancePosScale.xyz;

			// The scale is uniform, so the normal needs the rotation only -
			// no inverse transpose.
			vec3 n = normalize(vec3(c * aNormal.x + s * aNormal.z,
									aNormal.y,
									-s * aNormal.x + c * aNormal.z));
			// Upstream's fixed-function light, per vertex - see the mesh
			// vertex shader. The instance colour (upstream's glColor for a
			// target) is deliberately unused: with GL lighting on and no
			// GL_COLOR_MATERIAL it never reached the screen there either.
			vec3 L = normalize(uSunPos - world);
			float nl = max(dot(n, L), 0.0);
			vLit = min(uMatAmbient * (0.2 + uSkyAmbient)
					   + uMatDiffuse * uSkyDiffuse * nl
					   + uMatEmissive, vec3(1.0));
			if (uSphereMap == 1) {
				vec3 eyePos = (uView * vec4(world, 1.0)).xyz;
				vec3 eyeN = normalize(mat3(uView) * n);
				vec3 r = reflect(normalize(eyePos), eyeN);
				float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
				vTexCoord = vec2(r.x / m + 0.5, r.y / m + 0.5);
			} else {
				vTexCoord = aTexCoord;
			}

			gl_Position = uViewProj * vec4(world, 1.0);
			vViewDepth = gl_Position.w;
		}
	)";

	// Identical to the mesh fragment shader except that the colour arrives
	// per instance rather than as a uniform.
	const char *kInstancedMeshFragmentShader = R"(#version 300 es
		precision highp float;
		in vec3 vLit;
		in vec2 vTexCoord;
		in float vViewDepth;
		out vec4 fragColor;
		uniform sampler2D uTexture;
		uniform int uHasTexture;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		void main() {
			vec4 texel = (uHasTexture == 1) ? texture(uTexture, vTexCoord) : vec4(1.0);
			vec3 lit = texel.rgb * vLit;
			// Fixed-function GL_EXP2 fog, as upstream's models get.
			float fog = clamp(exp(-(uFogDensity * vViewDepth) * (uFogDensity * vViewDepth)), 0.0, 1.0);
			fragColor = vec4(mix(uFogColor, lit, fog), texel.a);
		}
	)";

	// M6: upstream's real trees - instanced, textured and alpha-cut.
	//
	// Same instancing as the scenery program above, plus a texture
	// coordinate and an alpha test. The alpha mask is the whole point: it
	// cuts a ragged needle silhouette out of each branch layer, which is
	// what stops a tree reading as the smooth cone it is built from.
	//
	// A discard rather than blending, deliberately. Blended foliage would
	// need back-to-front sorting of ~1,000 instances every frame, which
	// would undo the instancing entirely; alpha *testing* needs no sorting
	// and writes depth normally.
	const char *kTreeVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec3 aNormal;
		layout(location = 2) in vec2 aTexCoord;
		layout(location = 3) in vec4 aInstancePosScale;
		layout(location = 4) in vec4 aInstanceRotColor;
		uniform mat4 uViewProj;
		uniform vec3 uSunPos;
		uniform vec3 uSkyAmbient;
		uniform vec3 uSkyDiffuse;
		out vec3 vLit;
		out vec2 vTexCoord;
		out float vViewDepth;
		void main() {
			float rot = aInstanceRotColor.x;
			float s = sin(rot), c = cos(rot);

			vec3 p = aPosition * aInstancePosScale.w;
			vec3 rotated = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
			vec3 world = rotated + aInstancePosScale.xyz;

			vec3 n = normalize(vec3(c * aNormal.x + s * aNormal.z,
									aNormal.y,
									-s * aNormal.x + c * aNormal.z));
			// V3: upstream's fixed-function light with ModelRendererTree's
			// own material (ambient 0.4, diffuse 1, no specular), per
			// vertex. Two-sided: foliage is drawn from both faces, and a
			// leaf lit from behind should not be black.
			vec3 L = normalize(uSunPos - world);
			float nl = abs(dot(n, L));
			vLit = min(vec3(0.4) * (0.2 + uSkyAmbient) + uSkyDiffuse * nl, vec3(1.0))
				   * aInstanceRotColor.yzw;
			vTexCoord = aTexCoord;

			gl_Position = uViewProj * vec4(world, 1.0);
			vViewDepth = gl_Position.w;
		}
	)";

	const char *kTreeFragmentShader = R"(#version 300 es
		precision highp float;
		in vec3 vLit;
		in vec2 vTexCoord;
		in float vViewDepth;
		out vec4 fragColor;
		uniform sampler2D uAtlas;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		void main() {
			vec4 texel = texture(uAtlas, vTexCoord);
			// Upstream's masks are hard-edged, so the threshold only has to
			// separate needle from gap.
			if (texel.a < 0.5) discard;

			vec3 lit = texel.rgb * vLit;

			// Fixed-function GL_EXP2 fog, as upstream's trees get.
			float fog = clamp(exp(-(uFogDensity * vViewDepth) * (uFogDensity * vViewDepth)), 0.0, 1.0);
			fragColor = vec4(mix(uFogColor, lit, fog), 1.0);
		}
	)";

	const char *kMeshFragmentShader = R"(#version 300 es
		precision highp float;
		in vec3 vLit;
		in vec2 vTexCoord;
		in float vViewDepth;
		out vec4 fragColor;
		uniform sampler2D uTexture;
		uniform int uHasTexture;
		uniform vec4 uColor;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		void main() {
			// GL_MODULATE: the texel times the lit vertex colour.
			vec4 texel = (uHasTexture == 1) ? texture(uTexture, vTexCoord) : vec4(1.0);
			vec3 lit = texel.rgb * vLit;
			// Fixed-function GL_EXP2 fog, as upstream's models get. Only the
			// land and water shaders use the 350-unit, three-times curve.
			float fog = clamp(exp(-(uFogDensity * vViewDepth) * (uFogDensity * vViewDepth)), 0.0, 1.0);
			fragColor = vec4(mix(uFogColor, lit, fog), texel.a * uColor.a);
		}
	)";

	// M6: the aim sight - upstream's "old sight" (TargetRendererImplTank::
	// drawOldSight): a fan-shaped blade projecting from the gun, brightest
	// along the exact aim line and fading out to either side. Per-vertex
	// colour carries that fade, and it's drawn unlit so it reads as a UI
	// overlay rather than part of the scene.
	const char *kSightVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec3 aColor;
		uniform mat4 uMVP;
		out vec3 vColor;
		out float vViewDepth;
		void main() {
			vColor = aColor;
			gl_Position = uMVP * vec4(aPosition, 1.0);
			vViewDepth = gl_Position.w;
		}
	)";

	// Also draws the beams (lasers, lightning), which upstream draws
	// fixed-function and so fogged with GL_EXP2; the sight itself is at
	// the tank and never far enough to notice.
	const char *kSightFragmentShader = R"(#version 300 es
		precision highp float;
		in vec3 vColor;
		in float vViewDepth;
		out vec4 fragColor;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		void main() {
			float z = uFogDensity * vViewDepth;
			fragColor = vec4(mix(uFogColor, vColor, exp(-z * z)), 1.0);
		}
	)";

	// M6 object shadows. The baked light map above shadows the *terrain*
	// against itself, but nothing anchors a tank or a tree to the ground it
	// stands on. Upstream solves that separately too, with a soft circle
	// painted under each target (Landscape's shadow map, fed by
	// getShadowMap().addCircle in the tank and target renderers), so this
	// is the same idea drawn directly: a small ground-hugging quad with a
	// radial falloff.
	const char *kShadowVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec2 aLocal;
		uniform mat4 uMVP;
		out vec2 vLocal;
		void main() {
			vLocal = aLocal;
			gl_Position = uMVP * vec4(aPosition, 1.0);
		}
	)";

	const char *kShadowFragmentShader = R"(#version 300 es
		precision highp float;
		in vec2 vLocal;
		out vec4 fragColor;
		uniform float uStrength;
		void main() {
			// Soft-edged rather than a hard disc: a crisp ellipse on a
			// bumpy hillside reads as a decal, a soft one as shade.
			float d = length(vLocal);
			if (d > 1.0) discard;
			float a = (1.0 - d);
			fragColor = vec4(0.0, 0.0, 0.0, a * a * uStrength);
		}
	)";

	// M6 sky. A full-screen pass rather than a dome: the colour only ever
	// depends on the direction the pixel looks in, so the geometry a dome
	// would add is a way of interpolating that direction, and interpolating
	// the four corner rays across two triangles does the same for six
	// vertices. It also cannot be clipped, fall through at the horizon, or
	// need the camera translating into it.
	const char *kSkyVertexShader = R"(#version 300 es
		layout(location = 0) in vec2 aClip;
		layout(location = 1) in vec3 aRay;
		out vec3 vRay;
		void main() {
			vRay = aRay;
			// z = 1 puts it on the far plane; it is drawn first with depth
			// writes off, so everything else lands in front of it.
			gl_Position = vec4(aClip, 1.0, 1.0);
		}
	)";

	const char *kSkyFragmentShader = R"(#version 300 es
		precision highp float;
		in vec3 vRay;
		out vec4 fragColor;
		uniform vec3 uGradient[16];
		uniform vec3 uSunDir;
		uniform vec3 uSunColor;
		uniform float uHorizonGlow;
		uniform float uFlash;
		uniform float uSunDisc;
		// The fog, and the camera's height above upstream's dome centre.
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		uniform float uEyeHeight;
		void main() {
			vec3 d = normalize(vRay);

			// Upstream's gradient is indexed by the *angle* above the
			// horizon (Hemisphere::drawColored: colour row = slice / slices
			// * 15, where a slice is an equal step of elevation), not by
			// the sine of it. Below the horizon there is nothing to show
			// but the horizon colour - the ground is drawn over that
			// anyway, and clamping avoids a hard band when the camera dips.
			float t = asin(clamp(d.y, 0.0, 1.0)) / 1.5707963 * 15.0;
			int lo = int(floor(t));
			int hi = min(lo + 1, 15);
			vec3 sky = mix(uGradient[lo], uGradient[hi], fract(t));

			// Upstream's horizon glow, per vertex of the dome: every colour
			// channel lifted by (dot(direction, sun) + 1) / 4 - a quarter
			// everywhere, half towards the sun - and capped at 1.
			if (uHorizonGlow > 0.5) {
				sky = min(sky + vec3((dot(d, uSunDir) + 1.0) / 4.0), vec3(1.0));
			}

			// Upstream draws its sky dome with fixed-function fog *on*: a
			// flattened ellipsoid 2000 units across and 225 tall, centred
			// 15 units below sea level under the camera, so the horizon is
			// 2000 units away and, with GL_EXP2 (exp(-(density * z)^2)),
			// almost entirely the fog colour; even the zenith at 225 units
			// carries a little. That is why upstream's night horizon is
			// dark and this port's was a bright band the sea then mirrored.
			// The ray's distance to that dome is solved here per fragment.
			// In thousands of units: the raw coefficients are around 1e-7,
			// which a phone's mediump float flushes to zero.
			//
			// The eye is kept inside the dome. Upstream's camera never
			// rises above it, but this port's can (the buying-phase view
			// and the free camera both go past 210 units), and from above
			// the dome every ray hits it within a few units - no fog at
			// all, and the raw horizon colour across the whole lower sky.
			// Pinned just under the top, the horizon stays 2000 units away
			// and the zenith close, whatever the camera does.
			{
				float R = 2.0, r = 0.225;
				float h = min((uEyeHeight + 15.0) / 1000.0, r * 0.9);
				float a = (d.x * d.x + d.z * d.z) / (R * R) + (d.y * d.y) / (r * r);
				float b = 2.0 * h * d.y / (r * r);
				float c = (h * h) / (r * r) - 1.0;
				float disc = b * b - 4.0 * a * c;
				float dist = (disc > 0.0 && a > 0.0) ? (-b + sqrt(disc)) / (2.0 * a) : R;
				float z = uFogDensity * 1000.0 * dist;
				sky = mix(uFogColor, sky, exp(-z * z));
			}

			// The sun itself, only when the landscape has no sun texture of
			// its own; otherwise the sprite is the sun and this would show
			// through it as a second, harder one. Not fogged here: the
			// sprite path fogs the real billboard unless <nosunfog>.
			float toSun = max(dot(d, uSunDir), 0.0);
			sky += uSunColor * pow(toSun, 256.0) * 2.0 * uSunDisc;

			// SkyFlash: lift the whole sky towards white.
			fragColor = vec4(mix(sky, vec3(1.0), uFlash), 1.0);
		}
	)";

	// M6: a camera-facing textured quad, for the sun and moon. The corners
	// are built on the CPU from the view matrix's own right/up rows, which
	// is fewer moving parts than passing the basis in and rebuilding it
	// per vertex for a single quad.
	const char *kSpriteVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec2 aUv;
		uniform mat4 uMVP;
		out vec2 vUv;
		void main() {
			vUv = aUv;
			gl_Position = uMVP * vec4(aPosition, 1.0);
		}
	)";

	const char *kSpriteFragmentShader = R"(#version 300 es
		precision highp float;
		in vec2 vUv;
		out vec4 fragColor;
		uniform sampler2D uTexture;
		uniform vec4 uTint;
		void main() {
			vec4 c = texture(uTexture, vUv);
			fragColor = vec4(c.rgb * uTint.rgb, c.a * uTint.a);
		}
	)";

	// M6 clouds. Upstream hangs its cloud texture on the sky dome and
	// scrolls it with the live wind (SkyDome::simulate). Here it is a plane
	// high above the scene, which gives the same thing with the perspective
	// convergence that makes height read - and, unlike the sky pass, it has
	// to be real geometry, because a cloud layer has a position and a
	// backdrop does not.
	const char *kCloudVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		uniform mat4 uMVP;
		uniform vec2 uScroll;
		uniform float uTexScale;
		uniform vec3 uEyePos;
		out vec2 vUv;
		out float vEyeDistance;
		void main() {
			vUv = aPosition.xz * uTexScale + uScroll;
			gl_Position = uMVP * vec4(aPosition, 1.0);
			vEyeDistance = distance(aPosition, uEyePos);
		}
	)";

	const char *kCloudFragmentShader = R"(#version 300 es
		precision highp float;
		in vec2 vUv;
		in float vEyeDistance;
		out vec4 fragColor;
		uniform sampler2D uClouds;
		uniform vec3 uTint;
		uniform float uOpacity;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		void main() {
			vec4 c = texture(uClouds, vUv);
			// Upstream's cloud layers sit on a 1980-unit dome drawn with
			// fixed-function GL_EXP2 fog, so a cloud near the horizon is
			// nearly the fog colour. Same curve here, by distance from the
			// eye - which also does the job the old distance fade did, of
			// keeping the layer from aliasing out at the horizon. The stars
			// are drawn with fog off (density 0), as upstream draws them.
			float z = uFogDensity * vEyeDistance;
			vec3 colour = mix(uFogColor, c.rgb * uTint, exp(-z * z));
			fragColor = vec4(colour, c.a * uOpacity);
		}
	)";

	// M6 water. Upstream's own water is a whole subsystem in the excluded
	// client layer (Water/WaterMap/WaterWaves, a reflection cubemap, a foam
	// pass and a wave shader), and none of it is reusable - but the
	// landscape definition that drives it is ordinary src/common data
	// (LandscapeTexBorderWater: a height, five wave colours, a
	// transparency). So the surface is ours to draw, from upstream's own
	// numbers.
	//
	// The surface is upstream's own sea: the Tessendorf tile from
	// OceanWaves.h, generated on a worker thread and uploaded as the two
	// textures described above. The vertex shader displaces a flat grid by
	// it - up by the height and sideways by the choppy displacement - and
	// the fragment shader shades it with a transcription of water.fshader.
	const char *kWaterVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		uniform mat4 uMVP;
		// Explicitly highp, and matched in the fragment shader below. A
		// uniform of the same name must have the same precision in both
		// stages or the program will not link - and a vertex shader's
		// default for float is highp while a fragment shader's is whatever
		// its `precision` line says. Time also genuinely needs the range:
		// it runs to 3600, where mediump's ~10-bit mantissa steps in units
		// of about 4 and the waves would stop moving.
		uniform highp float uTime;
		uniform float uWaveAmplitude;
		// Which mip of the tile this draw's vertices read: 0 for a 2-unit
		// grid, one more for each doubling of the cell, so a coarse grid
		// sees a sea smoothed to what it can carry.
		uniform float uWaveLod;
		out vec2 vWorld;
		out vec3 vWorldPos;
		out float vViewDepth;
		out vec3 vNormal;
		out vec3 vUpwell;
		out vec4 vReflectCoord;
		out vec4 vShadowCoord;

		// Upstream's upwelling colours, the two the landscape's own
		// <wavetop*>/<wavebottom*> resolve to, and the water plane they are
		// measured against.
		uniform vec3 uUpwellTop;
		uniform vec3 uUpwellBot;
		uniform float uWaterHeight;
		// bias * proj * view of the *real* camera, for the reflection, and
		// the sun's for the shadow map.
		uniform mat4 uReflectMatrix;
		uniform mat4 uShadowMatrix;

		// Scorched3D's own ocean, as a tile the CPU regenerates from a
		// Tessendorf spectrum (see OceanWaves.h). Height in R and the
		// horizontal displacement in G (x) and B (z), all world units,
		// repeated across the sea; the displaced surface's normal in the
		// second texture, encoded n * 127 + 128 as upstream stores it.
		uniform sampler2D uWaveTex;
		uniform sampler2D uWaveNormalTex;
		uniform float uWaveTileLength;

		void main() {
			vWorld = aPosition.xz;
			float amp = uWaveAmplitude;

			vec3 world = aPosition;
			// textureLod, not texture: a vertex shader has no derivatives
			// to pick a mip level from, and ES3 requires the level to be
			// given explicitly here.
			vec2 tile = vWorld / uWaveTileLength;
			vec3 wave = textureLod(uWaveTex, tile, uWaveLod).rgb;
			// Up by the height and sideways by the choppy displacement -
			// upstream's Water2Patch places each vertex at exactly this
			// sum, which is what makes crests sharp and troughs broad.
			world.y += wave.r * amp;
			world.xz += wave.gb * amp;
			// The normal of the displaced surface, built on the CPU from
			// the displaced neighbours as Water2Patch builds it; leaned
			// back towards straight up by the same fade the height takes.
			vec3 n0 = textureLod(uWaveNormalTex, tile, uWaveLod).rgb * 2.0 - 1.0;
			vNormal = normalize(vec3(n0.x * amp, n0.y, n0.z * amp));

			vWorldPos = world;

			// Upstream's upwelling colour, from water.vshader:
			//   upwelltopbot * clamp((vertex.z + viewpos.z)/9 + N.z - 7/15, 0, 1)
			//     + upwellbot
			// with viewpos.z set to -waterHeight, so the first term is the
			// wave's own displacement. This is the water's *body* colour, and
			// it is slope dependent - a crest turns towards the top colour and
			// a trough towards the bottom one, which is most of why upstream's
			// sea reads as water rather than as a tinted mirror.
			vec3 n = normalize(vNormal);
			vUpwell = (uUpwellTop - uUpwellBot) *
				clamp((world.y - uWaterHeight) * 0.1111111 + n.y - 0.4666667, 0.0, 1.0)
				+ uUpwellBot;

			// Upstream's virtual-plane reflection coordinate (water.vshader):
			//   texc = vertex + N * (12 * N.z); texc.z -= 12;
			// then through the real camera's bias*proj*view. Its Z-up maps to
			// our Y-up, so its N.z is our N.y. The displacement is in world
			// units on a plane 12 above the surface, which is why the
			// distortion follows the wave rather than sliding across the
			// screen the way a texture-space nudge does.
			vec3 texc = world + n * (12.0 * n.y);
			texc.y -= 12.0;
			vReflectCoord = uReflectMatrix * vec4(texc, 1.0);
			vShadowCoord = uShadowMatrix * vec4(world, 1.0);

			gl_Position = uMVP * vec4(world, 1.0);
			vViewDepth = gl_Position.w;
		}
	)";

	const char *kWaterFragmentShader = R"(#version 300 es
		precision highp float;
		in vec2 vWorld;
		in vec3 vWorldPos;
		in float vViewDepth;
		in vec3 vNormal;
		in vec3 vUpwell;
		in vec4 vReflectCoord;
		in vec4 vShadowCoord;
		out vec4 fragColor;
		// The sun's position (upstream's GL_LIGHT0 for the water is the sun
		// as a point light, so the direction to it is per fragment).
		uniform vec3 uSunPos;
		uniform vec3 uSkyHorizon;
		uniform vec3 uSkyZenith;
		uniform vec3 uEyePos;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		uniform vec3 uSunDiffuse;
		uniform float uAlpha;
		uniform highp float uTime;  // must match the vertex shader's, see above
		uniform vec2 uMapSize;
		// W3: the mirrored scene, when it is being drawn.
		uniform sampler2D uReflectionTex;
		uniform float uUseReflection;
		uniform highp sampler2DShadow uShadowTex;
		uniform int uShadowEnabled;
		// Upstream's two "noise" layers: xy is the scroll offset, z the
		// scale (its noise_xform_0 / noise_xform_1). What they sample is
		// not noise but the sea's own normal map at 1/8 and 1/32 scale.
		uniform vec3 uNoise0;
		uniform vec3 uNoise1;
		uniform sampler2D uWaveNormalTex;
		// highp to match the vertex shader's declaration, see uTime above.
		uniform highp float uWaveTileLength;
		// W10a: the landscape's foam bitmap, tiled across the map.
		uniform sampler2D uFoamMask;
		uniform int uDebugMode;

		// Upstream's water shininess, from water.fshader.
		const float kWaterShininess = 120.0;

		float sunShadow() {
			if (uShadowEnabled == 0) return 1.0;
			if (vShadowCoord.w <= 0.0) return 1.0;
			vec3 p = vShadowCoord.xyz / vShadowCoord.w;
			if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) return 1.0;
			return textureProj(uShadowTex, vShadowCoord);
		}
		void main() {
			float fogFactor = clamp(exp(-3.0 * uFogDensity * max(vViewDepth - 350.0, 0.0)), 0.0, 1.0);

			// Upstream's two "noise" layers, from water.vshader/fshader:
			//   noise_texc = vertex / 2 * noise_xform.z + noise_xform.xy
			//   N0 = texture(tex_normal, noise_texc) * 2 - 1, times fog
			//   N = normalize(normal + N0 + N1)
			// tex_normal is the sea's own normal map, so the fine detail is
			// the same wind-aligned, moving sea at 1/8 and 1/32 scale
			// (repeating every 64 and 16 units, with the halving). N0 and
			// N1 are decoded as *full* unit normals, y near 1, so the sum
			// flattens every slope by about three - that is upstream's
			// look, not a mistake to correct. Without these the far water
			// shares one normal and the specular term becomes a solid lobe.
			vec2 t0 = vWorld / 2.0 * uNoise0.z + uNoise0.xy;
			vec2 t1 = vWorld / 2.0 * uNoise1.z + uNoise1.xy;
			vec3 N0 = (texture(uWaveNormalTex, t0).rgb * 2.0 - 1.0) * fogFactor;
			vec3 N1 = (texture(uWaveNormalTex, t1).rgb * 2.0 - 1.0) * fogFactor;
			vec3 n = normalize(vNormal + N0 + N1);
			// E, the direction *to* the viewer, and L, the direction to the
			// sun - both as upstream's shader has them.
			vec3 E = normalize(uEyePos - vWorldPos);
			vec3 L = normalize(uSunPos - vWorldPos);
			vec3 R = reflect(-L, n);
			float s0 = sunShadow();

			// Upstream's Fresnel, which is not Schlick: an approximation of
			// 1/((dot+1)^8), capped at 0.8 so the sea is never a full mirror.
			// Its own comment says that cap "greatly increases the realism of
			// the appearance", and it does - the previous Schlick term with a
			// 2% base went to nearly 1.0 at a grazing angle and turned the
			// whole distance into a mirror, which is most of why the water
			// read as the wrong colour.
			float fresnel = clamp(dot(E, n), 0.0, 1.0) + 1.0;
			fresnel = pow(fresnel, -8.0) * 0.8;

			// Phong specular off the wave slopes, at upstream's shininess.
			vec3 specular = uSunDiffuse * pow(clamp(dot(R, E), 0.0, 1.0), kWaterShininess);

			// The refracted (upwelling) colour: the body colour computed per
			// vertex, lit by the sun and dimmed - but not extinguished - in
			// shadow, upstream's `min(1, s0 + 0.9)`.
			float dl = max(dot(L, n), 0.0);
			vec3 refraction = vUpwell * dl * min(1.0, s0 + 0.9);

			// What the surface reflects. With the reflection buffer off this
			// is the sky evaluated from its own two colours, which is this
			// port's own fallback and all the old shader ever had.
			vec3 reflected;
			if (uUseReflection > 0.5) {
				// Upstream's projective lookup. The mirrored pass shares this
				// pass's projection, so a point on the water plane projects to
				// the same place in both and its reflection is read there;
				// the virtual-plane displacement in the vertex shader is what
				// bends that lookup along the wave.
				reflected = textureProj(uReflectionTex, vReflectCoord).rgb;
			} else {
				vec3 up = reflect(-E, n);
				reflected = mix(uSkyHorizon, uSkyZenith, sqrt(clamp(up.y, 0.0, 1.0)));
			}

			// Upstream's mix, shadow-weighted the same way.
			vec3 water = mix(refraction, reflected, fresnel * min(1.0, s0 + 0.8))
				+ specular * s0;

			// Whitecaps, from water.fshader: the foam amount rides in the
			// normal texture's alpha (upstream's tex_foamamount.x), faded
			// by fog and taken back in shadow, then broken into flecks by
			// the landscape's foam bitmap sampled 25 times across the map
			// (its aofland coordinate, which is the map-relative position;
			// landscape y runs the other way to world z). Mixed towards the
			// sun's own diffuse colour, not white.
			float aof = texture(uWaveNormalTex, vWorld / uWaveTileLength).a * fogFactor;
			vec2 mapPer = vec2(vWorld.x, uMapSize.y - vWorld.y) / uMapSize;
			float foam = max(min(aof, 1.0) - (1.0 - s0) * 0.5, 0.0)
				* texture(uFoamMask, mapPer * 25.0).b;
			water = mix(water, uSunDiffuse, foam);

			// Remote diagnosis: `adb shell setprop debug.scorchdroid.water N`
			// shows one term of the sum instead of the sum.
			if (uDebugMode != 0) {
				vec3 d = vec3(1.0, 0.0, 1.0);
				if (uDebugMode == 1) d = vec3(s0);
				else if (uDebugMode == 2) d = vec3(foam);
				else if (uDebugMode == 3) d = specular;
				else if (uDebugMode == 4) d = reflected;
				else if (uDebugMode == 5) d = vec3(fresnel);
				else if (uDebugMode == 6) d = refraction;
				else if (uDebugMode == 7) d = n * 0.5 + 0.5;
				else if (uDebugMode == 8) d = vec3(aof);
				else if (uDebugMode == 9) d = vec3(fogFactor);
				else if (uDebugMode == 10) d = vUpwell;
				fragColor = vec4(d, 1.0);
				return;
			}
			fragColor = vec4(mix(uFogColor, water, fogFactor), uAlpha);
		}
	)";

	// W10c: the breakers. Each quad is four corners of a segment's home
	// position; the phase slides it seaward along the segment's
	// perpendicular by upstream's frontlen (the two shore-side corners)
	// and endlen (the seaward two), and it rides the sea's height at
	// wherever it lands. Segments whose perpendicular runs with the wind
	// are upstream's skip (their sea faces away), done here by clipping.
	const char *kBreakerVertexShader = R"(#version 300 es
		layout(location = 0) in vec2 aBase;   // world x, z of this corner's home
		layout(location = 1) in vec2 aPerp;   // seaward, world x, z
		layout(location = 2) in vec2 aUv;
		uniform mat4 uMVP;
		uniform float uFront;
		uniform float uEnd;
		uniform float uWaterHeight;
		uniform float uWaveTileLength;
		uniform vec2 uWind;
		uniform sampler2D uWaveTex;
		out vec2 vUv;
		void main() {
			vUv = aUv;
			if (dot(aPerp, uWind) > 0.0) {
				gl_Position = vec4(0.0, 0.0, 2.0, 1.0);   // clipped away
				return;
			}
			vec2 p = aBase - aPerp * (aUv.y > 0.5 ? uFront : uEnd);
			float h = uWaterHeight
				+ textureLod(uWaveTex, p / uWaveTileLength, 0.0).r + 0.05;
			gl_Position = uMVP * vec4(p.x, h, p.y, 1.0);
		}
	)";

	const char *kBreakerFragmentShader = R"(#version 300 es
		precision highp float;
		in vec2 vUv;
		out vec4 fragColor;
		uniform sampler2D uTexture;
		uniform float uAlpha;
		void main() {
			vec4 c = texture(uTexture, vUv);
			// White at alpha * 0.3, upstream's glColor4f, times the image.
			fragColor = vec4(c.rgb, c.a * uAlpha * 0.3);
		}
	)";

	// M6 effects: one additive, soft-edged round sprite per particle.
	// Size and colour are per-vertex because a single explosion mixes both
	// (a bright small core with dimmer larger debris), which a uniform
	// could not express without a draw call each.
	const char *kParticleVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec2 aUv;
		layout(location = 2) in float aLayer;
		layout(location = 3) in vec4 aColor;
		uniform mat4 uMVP;
		out vec2 vUv;
		out float vLayer;
		out vec4 vColor;
		out float vViewDepth;
		void main() {
			vUv = aUv;
			vLayer = aLayer;
			vColor = aColor;
			gl_Position = uMVP * vec4(aPosition, 1.0);
			vViewDepth = gl_Position.w;
		}
	)";

	const char *kParticleFragmentShader = R"(#version 300 es
		precision highp float;
		in vec2 vUv;
		in float vLayer;
		in vec4 vColor;
		in float vViewDepth;
		out vec4 fragColor;
		uniform mediump sampler2DArray uSprites;
		uniform vec3 uFogColor;
		uniform float uFogDensity;
		void main() {
			// Upstream's particle: a camera-facing quad with one of its
			// textures, GL_MODULATE by the particle colour and alpha.
			vec4 t = texture(uSprites, vec3(vUv, vLayer));
			// Fixed-function GL_EXP2 fog, as upstream's particles get: the
			// colour goes to the fog colour with distance, the alpha stays.
			float z = uFogDensity * vViewDepth;
			vec3 colour = mix(uFogColor, t.rgb * vColor.rgb, exp(-z * z));
			fragColor = vec4(colour, t.a * vColor.a);
		}
	)";

	const char *kPointVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		uniform mat4 uMVP;
		uniform float uPointSize;
		void main() {
			gl_Position = uMVP * vec4(aPosition, 1.0);
			gl_PointSize = uPointSize;
		}
	)";

	const char *kPointFragmentShader = R"(#version 300 es
		precision highp float;
		out vec4 fragColor;
		uniform vec4 uColor;
		void main() {
			fragColor = uColor;
		}
	)";

	// One terrain vertex: position, normal, UV. Normals via central
	// differences on neighboring grid heights (clamped at the edges) - a
	// standard heightmap-normal approximation, not anything upstream
	// provides (its normal computation, if any, lives in the excluded
	// client rendering code). Split out of the full build so a partial
	// rebuild after a crater produces byte-identical vertices to a full
	// one - the two paths can't drift apart.
	// Engine normal (x, y, up) -> render (x, up, z), with the engine's y
	// running the other way to render z (worldZFromEngineY).
	inline void renderNormalFromEngine(FixedVector &n, float out[3])
	{
		out[0] = n[0].asFloat();
		out[1] = n[2].asFloat();
		out[2] = -n[1].asFloat();
	}

	void writeTerrainVertex(int gx, int gz, float *out)
	{
		const float py = terrainHeights[gz * terrainVerts1D + gx];
		const float *n = &terrainNormals[(size_t) (gz * terrainVerts1D + gx) * 3];
		const float nx = n[0], ny = n[1], nz = n[2];
		out[0] = terrainWorldX[gx];
		out[1] = py;
		out[2] = terrainWorldZ[gz];
		out[3] = nx;
		out[4] = ny;
		out[5] = nz;
		// UV spans the whole landscape once - the ground texture is
		// generated per-landscape at map resolution, not tiled here
		// (LandscapeTextureBuilder already tiles its sources).
		out[6] = (float) gx / (float) terrainGrid;
		// V runs backwards for the same reason the heightmap rows do: the
		// ground texture is generated in landscape orientation (row index =
		// landscape y - see LandscapeTextureBuilder, and the scorch marks
		// painted into it), while world Z runs the other way.
		out[7] = 1.0f - (float) gz / (float) terrainGrid;
	}

	// Builds the real terrain mesh - a regular grid sampled from the real
	// heightmap, downsampled to a fixed resolution (same reasoning as the
	// old 2D renderer's texture downsample: the source heightmap can be
	// far denser than a mobile GPU needs for a good-looking mesh).
	// Vertices are in real landscape-space world units (x, height, z) -
	// same coordinate space the tank/shot positions below use, so
	// everything shares one world without extra scale-factor bookkeeping.
	void buildTerrainIfNeeded(ScorchedContext &ctx)
	{
		// Rebuild whenever the landscape changes (a new round), not just
		// once per process.
		unsigned int defnNumber = ctx.getLandscapeMaps().getDefinitions().getDefinition().getDefinitionNumber();
		// M23: a changed detail setting rebuilds the mesh too - the grid is
		// baked into every vertex, index and texture coordinate.
		const bool gridChanged = (terrainGrid != requestedGridFor(ctx));
		// Turning shadows on or off changes whether the ground texture is
		// baked with the sun in it, so the texture has to be built again -
		// otherwise the terrain ends up lit twice or not at all.
		const bool shadowModeChanged = (groundBakedForShadows != (g_shadowLevel.load() > 0));
		if (terrainBuilt && defnNumber == builtDefinitionNumber &&
			!gridChanged && !shadowModeChanged) return;
		if (terrainBuilt && (defnNumber != builtDefinitionNumber || gridChanged || shadowModeChanged)) {
			// Drop the old landscape's GL objects before rebuilding.
			if (terrainVao) glDeleteVertexArrays(1, &terrainVao);
			if (terrainVbo) glDeleteBuffers(1, &terrainVbo);
			if (terrainIbo) glDeleteBuffers(1, &terrainIbo);
			if (groundTexture) glDeleteTextures(1, &groundTexture);
			terrainVao = terrainVbo = terrainIbo = groundTexture = 0;
			groundTextureData = LandscapeTextureBuilder::Texture();
			terrainBuilt = false;
			groundTextureBuilt = false;
			groundLightBaked = false;
			if (detailTexture) glDeleteTextures(1, &detailTexture);
			detailTexture = 0;
			groundGeneration++;   // any build in flight is for the old one
			// The new landscape gets a freshly built texture, so whatever
			// was painted over the old one is gone with it - and the
			// version has to be forced to re-sync, or a mask published
			// before this rebuild would never be painted.
			movementOverlayPainted = false;
			paintedMovementVersion = 0;
			// A camera pulled in against the old landscape's terrain has no
			// business constraining the new one's.
			g_camera.occlusionFraction = 1.0f;
			// Water, roof and sky are per-landscape too.
			waterBuilt = false;
			waterVisible = false;
			if (roofVao) glDeleteVertexArrays(1, &roofVao);
			if (roofVbo) glDeleteBuffers(1, &roofVbo);
			if (roofIbo) glDeleteBuffers(1, &roofIbo);
			if (roofTexture) glDeleteTextures(1, &roofTexture);
			if (roofSkirtVao) glDeleteVertexArrays(1, &roofSkirtVao);
			if (roofSkirtVbo) glDeleteBuffers(1, &roofSkirtVbo);
			roofVao = roofVbo = roofIbo = roofTexture = 0;
			roofSkirtVao = roofSkirtVbo = 0;
			roofIndexCount = 0;
			roofSkirtVertexCount = 0;
			if (surroundVao) glDeleteVertexArrays(1, &surroundVao);
			if (surroundVbo) glDeleteBuffers(1, &surroundVbo);
			if (surroundTexture) glDeleteTextures(1, &surroundTexture);
			surroundVao = surroundVbo = surroundTexture = 0;
			surroundVertexCount = 0;
			surroundBuilt = false;
			surroundVisible = false;
			loggedShieldHit = false;
			loggedParachute = false;
			roofBuilt = false;
			roofVisible = false;
			skyBuilt = false;
			cloudsBuilt = false;
			cloudsVisible = false;
			if (cloudTexture) glDeleteTextures(1, &cloudTexture);
			if (starTexture) glDeleteTextures(1, &starTexture);
			if (sunTexture) glDeleteTextures(1, &sunTexture);
			cloudTexture = starTexture = sunTexture = 0;
			// Any pending crater belongs to the landscape being thrown
			// away - applying it to the new one would corrupt unrelated
			// vertices.
			ScorchDroidLandscape::clearDirtyRegion();
			// Ranging tracers belong to the round that made them
			// (upstream clears them in RenderTracer::newGame).
			ScorchDroidTracer::clearAll();
			LOGI("Landscape changed (definition %u) - rebuilding terrain", defnNumber);
		}

		HeightMap &heightMap = ctx.getLandscapeMaps().getGroundMaps().getHeightMap();
		int w = heightMap.getMapWidth();
		int h = heightMap.getMapHeight();
		if (w <= 0 || h <= 0) return;

		// Adopt the requested detail before anything is sized by it.
		terrainGrid = requestedGridFor(ctx);
		terrainVerts1D = terrainGrid + 1;
		const int verts1D = terrainVerts1D;
		terrainSrcWidth = w;
		terrainSrcHeight = h;
		// Set before anything below converts a landscape coordinate -
		// worldZFromEngineY reads mapHeightUnits.
		mapWidthUnits = (float) w;
		mapHeightUnits = (float) h;
		terrainHeights.assign(verts1D * verts1D, 0.0f);
		terrainNormals.assign((size_t) verts1D * verts1D * 3, 0.0f);
		terrainWorldX.assign(verts1D, 0.0f);
		terrainWorldZ.assign(verts1D, 0.0f);
		terrainMinHeight = 1e9f;
		terrainMaxHeight = -1e9f;
		for (int i = 0; i < verts1D; i++) {
			terrainWorldX[i] = (float) i / (float) terrainGrid * (float) w;
			terrainWorldZ[i] = (float) i / (float) terrainGrid * (float) h;
		}
		for (int gz = 0; gz < verts1D; gz++) {
			int sy = heightMapRowForGridZ(gz, h);
			for (int gx = 0; gx < verts1D; gx++) {
				int sx = heightMapColForGridX(gx, w);
				float height = heightMap.getHeight(sx, sy).asFloat();
				terrainHeights[gz * verts1D + gx] = height;
				renderNormalFromEngine(heightMap.getNormal(sx, sy),
									   &terrainNormals[(size_t) (gz * verts1D + gx) * 3]);
				terrainMinHeight = std::min(terrainMinHeight, height);
				terrainMaxHeight = std::max(terrainMaxHeight, height);
			}
		}
		if (terrainMaxHeight - terrainMinHeight < 0.001f) terrainMaxHeight = terrainMinHeight + 1.0f;

		std::vector<float> vertexData(verts1D * verts1D * kTerrainFloatsPerVertex);
		for (int gz = 0; gz < verts1D; gz++) {
			for (int gx = 0; gx < verts1D; gx++) {
				writeTerrainVertex(gx, gz,
					&vertexData[(gz * verts1D + gx) * kTerrainFloatsPerVertex]);
			}
		}

		std::vector<unsigned int> indices;
		indices.reserve(terrainGrid * terrainGrid * 6);
		for (int gz = 0; gz < terrainGrid; gz++) {
			for (int gx = 0; gx < terrainGrid; gx++) {
				unsigned int i00 = gz * verts1D + gx;
				unsigned int i10 = i00 + 1;
				unsigned int i01 = i00 + verts1D;
				unsigned int i11 = i01 + 1;
				indices.push_back(i00); indices.push_back(i01); indices.push_back(i10);
				indices.push_back(i10); indices.push_back(i01); indices.push_back(i11);
			}
		}
		terrainIndexCount = (int) indices.size();

		glGenVertexArrays(1, &terrainVao);
		glBindVertexArray(terrainVao);
		glGenBuffers(1, &terrainVbo);
		glBindBuffer(GL_ARRAY_BUFFER, terrainVbo);
		glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(float), vertexData.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) (3 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) (6 * sizeof(float)));
		glGenBuffers(1, &terrainIbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, terrainIbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
		glBindVertexArray(0);

		{
			std::lock_guard<std::mutex> camLock(g_cameraMutex);
			g_camera.targetX = mapWidthUnits / 2.0f;
			g_camera.targetZ = mapHeightUnits / 2.0f;
			g_camera.targetY = (terrainMinHeight + terrainMaxHeight) / 2.0f;
			g_camera.orbitDistance = std::max(mapWidthUnits, mapHeightUnits) * 0.9f;
		}
		// The map centre above is only a placeholder until the tanks are
		// placed: a new round should open looking at your own tank, not at
		// whatever happens to be in the middle of the map. Positions aren't
		// known yet here (placement runs after the landscape is generated),
		// so this asks the draw loop to do it on the first frame that has
		// one - see recentreOnMyTank.
		recentreOnMyTank = true;

		terrainBuilt = true;
		builtDefinitionNumber = defnNumber;
		terrainDeformLogsLeft = 5;
		terrainScorchLogsLeft = 5;
		effectLogsLeft = 8;
		LOGI("Terrain mesh built: %dx%d source -> %dx%d grid, height range [%.1f, %.1f]",
			 w, h, verts1D, verts1D, terrainMinHeight, terrainMaxHeight);
	}

	// G6: anisotropic filtering, where the device has it (essentially every
	// GLES3 one). Upstream draws its main map with plain linear filtering
	// and no mipmaps, which on a phone screen at oblique angles shimmers;
	// keeping the mipmaps and adding anisotropy keeps upstream's sharpness
	// on oblique ground without the shimmer. A deliberate deviation.
	void applyAnisotropy()
	{
		static int checked = 0;   // 0 unknown, 1 yes, -1 no
		if (checked == 0) {
			const char *ext = (const char *) glGetString(GL_EXTENSIONS);
			checked = (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic")) ? 1 : -1;
			LOGI("Anisotropic filtering: %s", checked == 1 ? "available, 4x" : "not available");
		}
		if (checked != 1) return;
		const GLenum kMaxAnisotropy = 0x84FE;   // GL_TEXTURE_MAX_ANISOTROPY_EXT
		glTexParameterf(GL_TEXTURE_2D, kMaxAnisotropy, 4.0f);
	}

	// G1: upstream's texture size follows its TexureSize option - 256 at
	// Low, 1024 at the default, 2048 at High. Here it follows the Landscape
	// detail slider, which already sets the mesh: the full 256 grid gets
	// upstream's default 1024, half of it 512, anything less 256.
	int groundTextureSizeFor(int grid)
	{
		if (grid >= kTerrainGridMax) return 1024;
		if (grid >= kTerrainGridMax / 2) return 512;
		return 256;
	}

	// M6/G1: generates and uploads the real ground texture once a landscape
	// exists (see LandscapeTextureBuilder). The build runs on a worker
	// (see GroundJob); this is called every frame and does a little each
	// time: start the job, then adopt it when it is done. Failure is
	// non-fatal - the terrain shader falls back to its old height-ramp
	// colouring rather than drawing nothing, so a landscape definition we
	// can't texture still renders, and so does one still being built.
	void buildGroundTextureIfNeeded(ScorchedContext &ctx)
	{
		if (groundTextureBuilt) return;

		if (!groundJobRunning) {
			// Sun lighting and terrain self-shadowing are baked in - before
			// the upload, exactly where upstream does it (Landscape.cpp,
			// right after generating the texture) - but only when there
			// are no shadows to light it with. Upstream's own condition,
			// verbatim: `if (!GLStateExtension::hasHardwareShadows())` it
			// bakes, and otherwise leaves the texture unlit and lights the
			// terrain per fragment in the land shader against the shadow
			// map. Doing both would light the ground twice.
			groundBakedForShadows = (g_shadowLevel.load() > 0);
			if (groundThread.joinable()) groundThread.join();
			groundJob = GroundJob();
			groundJob.generation = ++groundGeneration;
			groundJob.size = groundTextureSizeFor(terrainGrid);
			groundJob.bake = !groundBakedForShadows;
			// Under the engine lock, which this frame holds: the only
			// engine access the build makes.
			groundJob.inputs = LandscapeTextureBuilder::capture(ctx);
			groundJobRunning = true;
			LOGI("Ground texture: building %dx%d on a worker%s", groundJob.size, groundJob.size,
				 groundJob.bake ? " with the light map" : "");
			groundThread = std::thread([]() {
				// Everything below reads only the job's own copies.
				LandscapeTextureBuilder::Inputs inputs;
				int size; bool bake;
				{
					std::lock_guard<std::mutex> lock(groundMutex);
					inputs = groundJob.inputs;
					size = groundJob.size;
					bake = groundJob.bake;
				}
				std::string error;
				LandscapeTextureBuilder::Texture ground = LandscapeTextureBuilder::build(inputs, size, &error);
				bool lightBaked = false;
				if (ground.valid() && bake) lightBaked = LandscapeTextureBuilder::applyLightMap(inputs, ground);
				LandscapeTextureBuilder::Texture detail = LandscapeTextureBuilder::loadDetail(inputs);
				std::lock_guard<std::mutex> lock(groundMutex);
				groundJob.ground = std::move(ground);
				groundJob.detail = std::move(detail);
				groundJob.lightBaked = lightBaked;
				groundJob.error = error;
				groundJob.done = true;
			});
			return;
		}

		// Adopt a finished job, if it is still the one we want.
		{
			std::lock_guard<std::mutex> lock(groundMutex);
			if (!groundJob.done) return;
		}
		groundThread.join();
		groundJobRunning = false;
		if (groundJob.generation != groundGeneration) {
			LOGI("Ground texture: discarding a build for a previous landscape");
			return;   // the next frame starts the right one
		}
		groundTextureBuilt = true;  // one attempt per landscape, success or not
		groundTextureData = std::move(groundJob.ground);
		groundLightBaked = groundJob.lightBaked;
		if (groundLightBaked) LOGI("Ground light map baked (sun lighting + terrain shadows)");
		else if (!groundBakedForShadows) LOGI("Ground light map not baked - the builder had no landscape");
		else LOGI("Ground light map not baked - the shadow map lights the terrain");

		LandscapeTextureBuilder::Texture &ground = groundTextureData;
		if (!ground.valid()) {
			LOGE("ground texture generation failed (%s) - falling back to flat height colours", groundJob.error.c_str());
			return;
		}

		glGenTextures(1, &groundTexture);
		glBindTexture(GL_TEXTURE_2D, groundTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, ground.width, ground.height, 0,
					 GL_RGB, GL_UNSIGNED_BYTE, ground.rgb.data());
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		applyAnisotropy();
		LOGI("Ground texture built: %dx%d", ground.width, ground.height);

		// G2: the detail image, mipmapped and repeating (upstream's
		// detailTexture_.replace(bitmapDetail, true)).
		LandscapeTextureBuilder::Texture &detail = groundJob.detail;
		if (detail.valid()) {
			if (detailTexture == 0) glGenTextures(1, &detailTexture);
			glBindTexture(GL_TEXTURE_2D, detailTexture);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, detail.width, detail.height, 0,
						 GL_RGB, GL_UNSIGNED_BYTE, detail.rgb.data());
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glGenerateMipmap(GL_TEXTURE_2D);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			applyAnisotropy();
			LOGI("Detail texture: %dx%d, one tile per 16 units", detail.width, detail.height);
		} else {
			LOGI("Detail texture: none (%s)", groundJob.inputs.detail.c_str());
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		groundJob.ground = LandscapeTextureBuilder::Texture();
		groundJob.detail = LandscapeTextureBuilder::Texture();
		groundJob.inputs = LandscapeTextureBuilder::Inputs();
	}


	// What distance fades towards: the landscape's own <fog> colour, as
	// upstream has it (Landscape::generate sets GL_FOG_COLOR from it and
	// every shader mixes towards gl_Fog.color). This port used to fog to
	// the sky gradient's horizon row instead, because the shipped daytime
	// maps set a flat grey <fog> that showed as a band against a blue
	// horizon when the fog started at zero distance. With upstream's fog -
	// nothing inside 350 units, then three times as dense - that grey reads
	// as distance haze, as it does on the PC; and on a dark map the old
	// choice turned the whole far sea the colour of the moonlit horizon,
	// a white band with a hard edge that upstream never shows.
	void currentFogColor(float out[3])
	{
		for (int i = 0; i < 3; i++) out[i] = skyDescription.fog[i];
	}

	// Loads a landscape image into a GL texture, with its mask as alpha.
	// Returns 0 if the definition names nothing or the file won't load -
	// every caller treats that as "this landscape has no such layer".
	// Named for the sky because that is where it started, but nothing about
	// it is sky-specific and the cavern roof uses it too; outW/outH are for
	// callers that need to know how often the image tiles across the map.
	GLuint loadSkyTexture(const std::string &file, const std::string &mask, bool repeat,
						  int *outW = nullptr, int *outH = nullptr)
	{
		if (file.empty()) return 0;
		// toRGB: the roof and sky images of the storm set are greyscale
		// JPEGs, which arrive with one channel; uploaded as GL_RGB they
		// were rainbow noise (the cavern's ceiling on every phone).
		Image image = LandscapeTextureBuilder::toRGB(
			ImageFactory::loadImage(S3D::eModLocation, file, mask, false));
		if (!image.getBits() || image.getWidth() <= 0) return 0;
		if (outW) *outW = image.getWidth();
		if (outH) *outH = image.getHeight();

		GLuint texture = 0;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		const GLenum format = (image.getComponents() == 4) ? GL_RGBA : GL_RGB;
		glTexImage2D(GL_TEXTURE_2D, 0, (GLint) format,
					 image.getWidth(), image.getHeight(), 0,
					 format, GL_UNSIGNED_BYTE, image.getBits());
		glGenerateMipmap(GL_TEXTURE_2D);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
		glBindTexture(GL_TEXTURE_2D, 0);
		return texture;
	}

	// M6: builds (once) the geometry and atlas for one of upstream's tree
	// types. Replaces the procedural three-cone stand-in that stood here
	// while the real thing was unavailable - see porting/TreeGeometry.hpp
	// for why upstream's trees are generated rather than loaded, and what
	// makes them look like trees.
	TreeKind *treeKindFor(TreeModelFactory::TreeType type)
	{
		std::map<int, TreeKind>::iterator existing = g_treeKinds.find((int) type);
		if (existing != g_treeKinds.end()) {
			return existing->second.vertexCount > 0 ? &existing->second : nullptr;
		}

		TreeKind &kind = g_treeKinds[(int) type];

		std::vector<float> verts;
		kind.vertexCount = ScorchDroidTrees::build(type, verts);
		if (kind.vertexCount == 0) return nullptr;

		// One atlas per image, shared by every type that samples it.
		const ScorchDroidTrees::Atlas atlas = ScorchDroidTrees::atlasFor(type);
		if (g_treeAtlases[atlas] == 0) {
			g_treeAtlases[atlas] = loadSkyTexture(
				ScorchDroidTrees::atlasImage(atlas),
				ScorchDroidTrees::atlasMask(atlas), false);
			LOGI("Tree atlas %d: %s + %s -> %s", (int) atlas,
				 ScorchDroidTrees::atlasImage(atlas),
				 ScorchDroidTrees::atlasMask(atlas),
				 g_treeAtlases[atlas] ? "loaded" : "FAILED");
		}
		kind.atlas = g_treeAtlases[atlas];

		glGenVertexArrays(1, &kind.vao);
		glBindVertexArray(kind.vao);
		glGenBuffers(1, &kind.vbo);
		glBindBuffer(GL_ARRAY_BUFFER, kind.vbo);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		const GLsizei stride = ScorchDroidTrees::kFloatsPerVertex * sizeof(float);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void *) (3 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *) (6 * sizeof(float)));

		glGenBuffers(1, &kind.instanceVbo);
		glBindBuffer(GL_ARRAY_BUFFER, kind.instanceVbo);
		const GLsizei instanceStride = ScorchDroidInstances::kFloatsPerInstance * sizeof(float);
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, instanceStride, (void *) 0);
		glVertexAttribDivisor(3, 1);
		glEnableVertexAttribArray(4);
		glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, instanceStride, (void *) (4 * sizeof(float)));
		glVertexAttribDivisor(4, 1);
		glBindVertexArray(0);

		LOGI("Tree type %d built: %d vertices (%d triangles)",
			 (int) type, kind.vertexCount, kind.vertexCount / 3);
		return &kind;
	}

	// M6 sun/moon: upstream draws a 60-unit billboard at the sun's position
	// (Sun::draw), additively unless the landscape says otherwise.
	void drawSunSprite(const Mat4 &mvp, const Mat4 &view)
	{
		if (sunTexture == 0 || spriteProgram == 0) return;

		// The sun's absolute position, as upstream places it: map centre
		// plus 900 units along its bearing. Being a real point rather than
		// fixed to the eye, it shifts very slightly as the camera moves,
		// which is upstream's behaviour too.
		const float cx = skyDescription.sunPosition[0];
		const float cy = skyDescription.sunPosition[2];
		const float cz = worldZFromEngineY(skyDescription.sunPosition[1]);

		const float rx = view.m[0], ry = view.m[4], rz = view.m[8];
		const float ux = view.m[1], uy = view.m[5], uz = view.m[9];
		const float half = 30.0f;  // upstream's 60x60

		float verts[6 * 5];
		const float corner[6][2] = {
			{ -1, -1 }, { 1, -1 }, { -1, 1 },
			{ -1,  1 }, { 1, -1 }, {  1, 1 },
		};
		for (int i = 0; i < 6; i++) {
			const float sx = corner[i][0] * half, sy = corner[i][1] * half;
			verts[i * 5 + 0] = cx + rx * sx + ux * sy;
			verts[i * 5 + 1] = cy + ry * sx + uy * sy;
			verts[i * 5 + 2] = cz + rz * sx + uz * sy;
			verts[i * 5 + 3] = corner[i][0] * 0.5f + 0.5f;
			verts[i * 5 + 4] = corner[i][1] * 0.5f + 0.5f;
		}

		glUseProgram(spriteProgram);
		glUniformMatrix4fv(spriteMvpLoc, 1, GL_FALSE, mvp.m);
		// Upstream draws the billboard with fixed-function fog unless the
		// landscape says <nosunfog>: GL_EXP2 by its distance from the eye.
		float tint[3] = { skyDescription.sunColor[0], skyDescription.sunColor[1], skyDescription.sunColor[2] };
		if (skyDescription.sunFog && g_showFog) {
			const float dx = cx - g_pickCamera.eyeX, dy = cy - g_pickCamera.eyeY, dz = cz - g_pickCamera.eyeZ;
			const float z = skyDescription.fogDensity * sqrtf(dx * dx + dy * dy + dz * dz);
			const float f = expf(-z * z);
			float fog[3];
			currentFogColor(fog);
			for (int i = 0; i < 3; i++) tint[i] = fog[i] + (tint[i] - fog[i]) * f;
		}
		glUniform4f(spriteTintLoc, tint[0], tint[1], tint[2], 1.0f);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, sunTexture);
		glUniform1i(spriteSamplerLoc, 0);

		glBindVertexArray(spriteVao);
		glBindBuffer(GL_ARRAY_BUFFER, spriteVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA,
					skyDescription.sunBlendAdditive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
		glDisable(GL_CULL_FACE);
		frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, 6);
		glEnable(GL_CULL_FACE);
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glBindVertexArray(0);
	}

	// M6 clouds: the landscape's own cloud texture on a high plane.
	void buildCloudsIfNeeded(ScorchedContext &ctx)
	{
		if (cloudsBuilt) return;
		cloudsBuilt = true;
		cloudsVisible = false;

		LandscapeTex *tex = ctx.getLandscapeMaps().getDefinitions().getTex();
		if (!tex || tex->skytexture.empty()) return;

		// The mask becomes the alpha channel - that is what makes the gaps
		// between clouds transparent rather than black. Tiled, since the
		// plane is far wider than the texture.
		cloudTexture = loadSkyTexture(tex->skytexture, tex->skytexturemask, true);
		if (cloudTexture == 0) return;

		// Stars ride the same plane. Only night landscapes define them, and
		// upstream draws them with no mask of their own.
		starTexture = loadSkyTexture(tex->skytexturestatic, tex->skytexturestatic, true);

		// Sun or moon. Clamped, not tiled - it is one sprite.
		sunTexture = loadSkyTexture(tex->suntexture, tex->suntexturemask, false);

		// High enough to sit well above the tallest terrain and read as sky
		// rather than as a ceiling, and wide enough that its edge is past
		// the distance fade in the shader.
		const float height = 220.0f;
		const float reach = 1400.0f;
		const float cx = mapWidthUnits * 0.5f, cz = mapHeightUnits * 0.5f;
		const float quad[] = {
			cx - reach, height, cz - reach,
			cx - reach, height, cz + reach,
			cx + reach, height, cz - reach,
			cx + reach, height, cz - reach,
			cx - reach, height, cz + reach,
			cx + reach, height, cz + reach,
		};

		if (cloudVao == 0) glGenVertexArrays(1, &cloudVao);
		if (cloudVbo == 0) glGenBuffers(1, &cloudVbo);
		glBindVertexArray(cloudVao);
		glBindBuffer(GL_ARRAY_BUFFER, cloudVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
		glBindVertexArray(0);

		cloudsVisible = true;
		LOGI("Sky layers: clouds from %s, stars %s, sun sprite %s",
			 tex->skytexture.c_str(),
			 starTexture ? tex->skytexturestatic.c_str() : "none",
			 sunTexture ? tex->suntexture.c_str() : "none");
	}

	// Advances the cloud scroll by the real wind, the way upstream does in
	// SkyDome::simulate: the stronger the wind the shorter the period, and
	// the layer runs *against* the wind's starting direction.
	void advanceClouds(ScorchedContext &ctx, float deltaSeconds)
	{
		if (!cloudsVisible) return;

		Wind &wind = ctx.getSimulator().getWind();
		const float speed = wind.getWindSpeed().asFloat();
		// Upstream's own curve, as a period in seconds per texture width.
		const float period = ((5.0f - speed) / 5.0f) * (500.0f - 100.0f) + 100.0f;
		if (period <= 0.0f) return;

		FixedVector direction = wind.getWindStartingDirection();
		float dx = -direction[0].asFloat();
		float dy = -direction[1].asFloat();
		const float len = sqrtf(dx * dx + dy * dy);
		if (len < 0.0001f) { dx = 0.8f; dy = 0.8f; }
		else { dx /= len; dy /= len; }

		cloudScrollX += dx * deltaSeconds / period;
		cloudScrollY += dy * deltaSeconds / period;
		cloudScrollX = fmodf(cloudScrollX, 1.0f);
		cloudScrollY = fmodf(cloudScrollY, 1.0f);
	}

	// M6 sky: the landscape's own sky colours, read once per landscape.
	void buildSkyIfNeeded(ScorchedContext &ctx)
	{
		if (skyBuilt) return;
		skyBuilt = true;
		skyDescription = ScorchDroidSky::describe(ctx);

		// X4c: the landscape's weather. Three shipped landscapes ask for
		// it; the rest have none.
		precipitationKind = 0;
		precipitationCount = 0;
		precipitationAccumulator = 0.0f;
		{
			LandscapeTex *tex = ctx.getLandscapeMaps().getDefinitions().getTex();
			if (tex && tex->precipitation) {
				const LandscapeTexType::TexType type = tex->precipitation->getType();
				if (type == LandscapeTexType::ePrecipitationRain ||
					type == LandscapeTexType::ePrecipitationSnow) {
					precipitationKind = (type == LandscapeTexType::ePrecipitationRain) ? 1 : 2;
					precipitationCount = ((LandscapeTexPrecipitation *) tex->precipitation)->particles;
					LOGI("Precipitation: %s, %d particles per 0.1 s",
						 precipitationKind == 1 ? "rain" : "snow", precipitationCount);
				}
			}
		}
		if (skyDescription.valid) {
			LOGI("Sky: gradient loaded, sun towards (%.2f, %.2f, %.2f), glow %d",
				 skyDescription.sunDirection[0], skyDescription.sunDirection[1],
				 skyDescription.sunDirection[2], skyDescription.horizonGlow ? 1 : 0);
			LOGI("Sky: horizon (%.2f, %.2f, %.2f), zenith (%.2f, %.2f, %.2f), fog density %.4f",
				 skyDescription.gradient[0][0], skyDescription.gradient[0][1],
				 skyDescription.gradient[0][2],
				 skyDescription.gradient[15][0], skyDescription.gradient[15][1],
				 skyDescription.gradient[15][2], skyDescription.fogDensity);
		} else {
			LOGI("Sky: no usable colour map for this landscape - keeping the flat fallback");
		}
	}

	// Draws the sky behind everything. The four corner view rays are built
	// from the same camera basis the terrain pick uses, so the horizon sits
	// where the world says it does at any pitch.
	void drawSky(float eyeFwdX, float eyeFwdY, float eyeFwdZ,
				 float rightX, float rightY, float rightZ,
				 float upX, float upY, float upZ,
				 float tanHalfFov, float aspect, float eyeHeight)
	{
		if (!skyDescription.valid || skyProgram == 0) return;

		const float sx = aspect * tanHalfFov;
		const float sy = tanHalfFov;
		float verts[6 * 5];
		const float corners[6][2] = {
			{ -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f },
			{ -1.0f,  1.0f }, { 1.0f, -1.0f }, {  1.0f, 1.0f },
		};
		for (int i = 0; i < 6; i++) {
			const float cx = corners[i][0], cy = corners[i][1];
			verts[i * 5 + 0] = cx;
			verts[i * 5 + 1] = cy;
			verts[i * 5 + 2] = eyeFwdX + rightX * cx * sx + upX * cy * sy;
			verts[i * 5 + 3] = eyeFwdY + rightY * cx * sx + upY * cy * sy;
			verts[i * 5 + 4] = eyeFwdZ + rightZ * cx * sx + upZ * cy * sy;
		}

		glUseProgram(skyProgram);
		glUniform3fv(skyGradientLoc, ScorchDroidSky::kGradientSteps,
					 &skyDescription.gradient[0][0]);
		// Landscape (x, y, height) -> render (x, height, z), like every
		// other direction that crosses this boundary.
		glUniform3f(skySunDirLoc,
					skyDescription.sunDirection[0],
					skyDescription.sunDirection[2],
					-skyDescription.sunDirection[1]);
		glUniform3f(skySunColorLoc, skyDescription.sunColor[0],
					skyDescription.sunColor[1], skyDescription.sunColor[2]);
		glUniform1f(skyGlowLoc, skyDescription.horizonGlow ? 1.0f : 0.0f);
		glUniform1f(skyFlashLoc,
					std::min(1.0f, skyFlashRemaining / kSkyFlashSeconds));
		glUniform1f(skySunDiscLoc, (sunTexture != 0) ? 0.0f : 1.0f);
		{
			float fog[3];
			currentFogColor(fog);
			glUniform3f(skyFogColorLoc, fog[0], fog[1], fog[2]);
			glUniform1f(skyFogDensityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);
			glUniform1f(skyEyeHeightLoc, eyeHeight);
		}

		glBindVertexArray(skyVao);
		glBindBuffer(GL_ARRAY_BUFFER, skyVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
		// No depth writes: this is a backdrop, and everything drawn after
		// it must win the depth test whatever its distance.
		// The backdrop sits on the far plane, where the GL_LESS depth test
		// rejects it against the cleared depth; it is drawn first, so the
		// test is simply off for it.
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glDisable(GL_CULL_FACE);
		frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, 6);
		glEnable(GL_CULL_FACE);
		glDepthMask(GL_TRUE);
		glEnable(GL_DEPTH_TEST);
		glBindVertexArray(0);
	}

	// Upstream's LandSurround, which fills the world beyond the map edge with
	// a flat apron of ground at height 0. Without it the terrain patch simply
	// stops in mid-air: the water plane hides that on water maps and the roof
	// skirt does on caverns, but a landscape with neither had nothing there
	// at all, only fog.
	//
	// The geometry is upstream's own - sixteen points forming a ring of eight
	// quads around the map, taken from LandSurround::generateVerts and its
	// dataOfs table, so the apron reaches as far out as upstream's does
	// rather than as far as looked right here.
	void buildSurroundIfNeeded(ScorchedContext &ctx)
	{
		if (surroundBuilt) return;
		surroundBuilt = true;
		surroundVisible = false;

		LandscapeTex *tex = ctx.getLandscapeMaps().getDefinitions().getTex();
		if (!tex || !tex->texture) return;

		// Upstream's own choice of image for this (Landscape::generate sets
		// groundTexture_, which is what LandSurround samples): the first
		// ground layer for a generated texture, or the explicit
		// <surroundtexture> - falling back to the main image - for a
		// file-based one.
		std::string file;
		if (tex->texture->getType() == LandscapeTexType::eTextureGenerate) {
			file = ((LandscapeTexTextureGenerate *) tex->texture)->texture0;
		} else if (tex->texture->getType() == LandscapeTexType::eTextureFile) {
			LandscapeTexTextureFile *fileTex = (LandscapeTexTextureFile *) tex->texture;
			file = fileTex->surroundTexture.empty() ? fileTex->texture : fileTex->surroundTexture;
		}
		surroundTexture = loadSkyTexture(file, "", true);
		if (surroundTexture == 0) {
			LOGI("Land surround: no texture (%s) - not drawn", file.empty() ? "none named" : file.c_str());
			return;
		}

		// LandSurround::generateVerts, in landscape coordinates. The inner
		// box is exactly the map; the outer one is upstream's 1536 + three
		// map widths from the centre.
		const float cx = mapWidthUnits * 0.5f, cy = mapHeightUnits * 0.5f;
		const float o2x = mapWidthUnits * 0.5f, o2y = mapHeightUnits * 0.5f;
		// Upstream uses getMapWidth() for *both* components here, which is a
		// slip that only shows on a non-square map; the map's own height is
		// used for the second one.
		const float o3x = 1536.0f + mapWidthUnits * 3.0f;
		const float o3y = 1536.0f + mapHeightUnits * 3.0f;
		const float box[16][2] = {
			{ cx - o2x, cy - o2y }, { cx - o2x, cy + o2y },
			{ cx + o2x, cy + o2y }, { cx + o2x, cy - o2y },
			{ cx - o3x, cy - o3y }, { cx - o3x, cy + o3y },
			{ cx + o3x, cy + o3y }, { cx + o3x, cy - o3y },
			{ cx - o2x, cy - o3y }, { cx - o2x, cy + o3y },
			{ cx + o2x, cy + o3y }, { cx + o2x, cy - o3y },
			{ cx - o3x, cy - o2y }, { cx - o3x, cy + o2y },
			{ cx + o3x, cy + o2y }, { cx + o3x, cy - o2y },
		};
		const int quads[8][4] = {
			{ 8, 11, 3, 0 }, { 1, 2, 10, 9 }, { 4, 8, 0, 12 }, { 11, 7, 15, 3 },
			{ 3, 15, 14, 2 }, { 2, 14, 6, 10 }, { 13, 1, 9, 5 }, { 12, 0, 1, 13 },
		};

		std::vector<float> verts;
		verts.reserve(8 * 6 * kTerrainFloatsPerVertex);
		auto emit = [&](int index) {
			const float lx = box[index][0], ly = box[index][1];
			verts.push_back(lx);
			verts.push_back(0.0f);                    // upstream's own height
			verts.push_back(worldZFromEngineY(ly));
			verts.push_back(0.0f); verts.push_back(1.0f); verts.push_back(0.0f);
			// One tile per 64 units, which is what upstream's texture
			// coordinate reduces to: (x / texWidth) * (texWidth / 16) / 4.
			// In landscape coordinates, so it lines up with the ground
			// texture's own orientation rather than mirroring against it.
			verts.push_back(lx / 64.0f);
			verts.push_back(ly / 64.0f);
		};
		for (int q = 0; q < 8; q++) {
			// Two triangles per quad, each wound so its front face points up
			// - worked out from the geometry rather than assumed, since
			// upstream's table is ordered for GL_QUADS and the ring's four
			// sides do not all run the same way round.
			const int tri[2][3] = {
				{ quads[q][0], quads[q][1], quads[q][2] },
				{ quads[q][0], quads[q][2], quads[q][3] },
			};
			for (int t = 0; t < 2; t++) {
				const float ax = box[tri[t][0]][0], az = box[tri[t][0]][1];
				const float bx = box[tri[t][1]][0], bz = box[tri[t][1]][1];
				const float cxx = box[tri[t][2]][0], cz = box[tri[t][2]][1];
				// Signed area in the landscape plane. World Z is flipped
				// against landscape y, so a positive area here is a
				// clockwise (back-facing) triangle in world space.
				const float area = (bx - ax) * (cz - az) - (cxx - ax) * (bz - az);
				if (area < 0.0f) {
					emit(tri[t][0]); emit(tri[t][1]); emit(tri[t][2]);
				} else {
					emit(tri[t][0]); emit(tri[t][2]); emit(tri[t][1]);
				}
			}
		}
		surroundVertexCount = (int) (verts.size() / kTerrainFloatsPerVertex);

		if (surroundVao == 0) glGenVertexArrays(1, &surroundVao);
		glBindVertexArray(surroundVao);
		if (surroundVbo == 0) glGenBuffers(1, &surroundVbo);
		glBindBuffer(GL_ARRAY_BUFFER, surroundVbo);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		const GLsizei stride = kTerrainFloatsPerVertex * sizeof(float);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void *) (3 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *) (6 * sizeof(float)));
		glBindVertexArray(0);

		surroundVisible = true;
		LOGI("Land surround: %d verts from %s, out to %.0fx%.0f units",
			 surroundVertexCount, file.c_str(), o3x * 2.0f, o3y * 2.0f);
	}

	// S6, the skirt that closes the cavern off. The roof mesh covers the map
	// and no more, so without this the ceiling simply stops at the map edge
	// and open sky shows through the gap - which reads as a bug rather than
	// as a cave.
	//
	// Upstream's SkyRoof::drawSegment, followed closely: from each vertex on
	// the map boundary it marches outward, away from the map centre, in
	// steps of (cavern width - distance from centre) / 5, and drops the
	// height along a quarter-cosine of the step number. It takes steps + 4
	// of those steps, so the last three carry on past cos(90 degrees) and
	// the wall curves down below the horizon, closing the view off. The
	// numbers here - the 5 steps, the 1.57, the steps + 3 loop bound - are
	// upstream's own, not tuned.
	//
	// Single-sided, facing inwards: the camera can get outside the wall or
	// above the part of it that has curved below the horizon, and a
	// two-sided skirt then fills the screen with a solid slab. Each
	// triangle's winding is derived from its own geometry below rather than
	// fixed once for the whole shell, because the four edges march outward
	// in four different directions.
	void buildRoofSkirt(ScorchedContext &ctx, const std::vector<float> &heights)
	{
		roofSkirtVertexCount = 0;

		LandscapeDefn *defn = ctx.getLandscapeMaps().getDefinitions().getDefn();
		if (!defn || !defn->roof ||
			defn->roof->getType() != LandscapeDefnType::eRoofCavern) return;
		LandscapeDefnRoofCavern *cavern = (LandscapeDefnRoofCavern *) defn->roof;
		const float radius = cavern->width.asFloat();

		const int verts1D = terrainVerts1D;
		const float centreX = mapWidthUnits * 0.5f;
		const float centreZ = mapHeightUnits * 0.5f;
		const int steps = 5;

		// A boundary point, as world position plus the roof height there.
		auto edgePoint = [&](int gx, int gz, float out[3]) {
			out[0] = terrainWorldX[gx];
			out[1] = heights[gz * verts1D + gx];
			out[2] = terrainWorldZ[gz];
		};

		std::vector<float> verts;
		auto uvFor = [&](float x, float z, float &u, float &v) {
			u = (x / mapWidthUnits) * roofUScale;
			v = (1.0f - z / mapHeightUnits) * roofVScale;
		};

		// One outward-marching strip between two adjacent boundary points.
		auto addSegment = [&](float a[3], float b[3]) {
			const float heightA = a[1], heightB = b[1];

			// Upstream's outward step: normalised away from the map centre,
			// scaled so `steps` of them reach the cavern radius.
			float da[3] = { a[0] - centreX, 0.0f, a[2] - centreZ };
			float db[3] = { b[0] - centreX, 0.0f, b[2] - centreZ };
			const float distA = sqrtf(da[0] * da[0] + da[2] * da[2]);
			const float distB = sqrtf(db[0] * db[0] + db[2] * db[2]);
			if (distA < 1e-4f || distB < 1e-4f) return;
			for (int i = 0; i < 3; i++) {
				da[i] = da[i] / distA * (radius - distA) / (float) steps;
				db[i] = db[i] / distB * (radius - distB) / (float) steps;
			}

			float cur[3] = { a[0], a[1], a[2] };
			float curB[3] = { b[0], b[1], b[2] };
			// Upstream emits steps + 4 vertex pairs, so steps + 3 quads. Its
			// height for the pair *after* step i is cos(1.57 * i / steps),
			// which is why the first step out keeps the edge height exactly
			// (cos 0) and the wall only starts dropping after it.
			for (int i = 0; i < steps + 3; i++) {
				float nextA[3] = { cur[0] + da[0], 0.0f, cur[2] + da[2] };
				float nextB[3] = { curB[0] + db[0], 0.0f, curB[2] + db[2] };
				const float drop = cosf(1.57f * (float) i / (float) steps);
				nextA[1] = heightA * drop;
				nextB[1] = heightB * drop;

				// Face normal, flipped to face the inside of the cavern -
				// the only side anyone stands on.
				const float e1[3] = { curB[0] - cur[0], curB[1] - cur[1], curB[2] - cur[2] };
				const float e2[3] = { nextA[0] - cur[0], nextA[1] - cur[1], nextA[2] - cur[2] };
				float n[3] = {
					e1[1] * e2[2] - e1[2] * e2[1],
					e1[2] * e2[0] - e1[0] * e2[2],
					e1[0] * e2[1] - e1[1] * e2[0],
				};
				const float nLen = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (nLen < 1e-6f) { n[0] = 0; n[1] = -1; n[2] = 0; }
				else { for (int k = 0; k < 3; k++) n[k] /= nLen; }
				const float toCentre[3] = { centreX - cur[0], 0.0f, centreZ - cur[2] };
				if (n[0] * toCentre[0] + n[2] * toCentre[2] < 0.0f) {
					for (int k = 0; k < 3; k++) n[k] = -n[k];
				}

				// Emit each triangle wound so that its front face is the one
				// the interior normal points along. Derived per triangle
				// from the geometry rather than reasoned out once for the
				// whole strip: the four edges march outward in four
				// different directions, so a single fixed order is right
				// for some of them and inside-out for the others.
				const float *tris[2][3] = {
					{ cur, curB, nextA },
					{ nextA, curB, nextB },
				};
				for (int t = 0; t < 2; t++) {
					const float *v0 = tris[t][0];
					const float *v1 = tris[t][1];
					const float *v2 = tris[t][2];
					const float a1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
					const float a2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
					const float face[3] = {
						a1[1] * a2[2] - a1[2] * a2[1],
						a1[2] * a2[0] - a1[0] * a2[2],
						a1[0] * a2[1] - a1[1] * a2[0],
					};
					const bool frontFacesInward =
						(face[0] * n[0] + face[1] * n[1] + face[2] * n[2]) > 0.0f;
					const float *ordered[3] = {
						v0,
						frontFacesInward ? v1 : v2,
						frontFacesInward ? v2 : v1,
					};
					for (int k = 0; k < 3; k++) {
						float u, v;
						uvFor(ordered[k][0], ordered[k][2], u, v);
						verts.push_back(ordered[k][0]);
						verts.push_back(ordered[k][1]);
						verts.push_back(ordered[k][2]);
						verts.push_back(n[0]); verts.push_back(n[1]); verts.push_back(n[2]);
						verts.push_back(u); verts.push_back(v);
					}
				}

				for (int k = 0; k < 3; k++) { cur[k] = nextA[k]; curB[k] = nextB[k]; }
			}
		};

		// All four map edges. Each pair of adjacent boundary vertices gets a
		// strip; the corners are covered because the two edges meeting there
		// each start from the same vertex.
		float a[3], b[3];
		for (int gx = 0; gx < terrainGrid; gx++) {
			edgePoint(gx, 0, a);          edgePoint(gx + 1, 0, b);          addSegment(a, b);
			edgePoint(gx + 1, terrainGrid, a);  edgePoint(gx, terrainGrid, b);          addSegment(a, b);
		}
		for (int gz = 0; gz < terrainGrid; gz++) {
			edgePoint(0, gz + 1, a);      edgePoint(0, gz, b);              addSegment(a, b);
			edgePoint(terrainGrid, gz, a);      edgePoint(terrainGrid, gz + 1, b);      addSegment(a, b);
		}

		roofSkirtVertexCount = (int) (verts.size() / kTerrainFloatsPerVertex);
		if (roofSkirtVertexCount == 0) return;

		if (roofSkirtVao == 0) glGenVertexArrays(1, &roofSkirtVao);
		glBindVertexArray(roofSkirtVao);
		if (roofSkirtVbo == 0) glGenBuffers(1, &roofSkirtVbo);
		glBindBuffer(GL_ARRAY_BUFFER, roofSkirtVbo);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		const GLsizei stride = kTerrainFloatsPerVertex * sizeof(float);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void *) (3 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *) (6 * sizeof(float)));
		glBindVertexArray(0);
	}

	// M6 S6: the cavern roof. Structurally this is buildTerrainIfNeeded with
	// a different heightmap - RoofMaps::getRoofMap() is a real HeightMap at
	// the same dimensions as the ground's (generateRMap creates it from
	// getLandscapeWidth/Height), already flipped so its heights are absolute
	// world heights hanging below the cavern's <height>.
	//
	// Upstream also extends a hemisphere skirt outwards from the map edge to
	// the cavern's <width> (SkyRoof::drawSegment marches five steps out and
	// curves the height down by cos). That is doing the same job the water
	// skirt does here - hiding the edge of the world - and the arena is
	// already surrounded by markers and, on these maps, by sea. So the roof
	// is the roof; the skirt is left for later if the edge reads badly.
	void buildRoofIfNeeded(ScorchedContext &ctx)
	{
		if (roofBuilt) return;
		roofBuilt = true;
		roofVisible = false;

		RoofMaps &roofMaps = ctx.getLandscapeMaps().getRoofMaps();
		if (!roofMaps.getRoofOn()) {
			LOGI("Landscape has no roof (not a cavern)");
			return;
		}

		HeightMap &rmap = roofMaps.getRoofMap();
		const int w = rmap.getMapWidth();
		const int h = rmap.getMapHeight();
		if (w <= 0 || h <= 0) return;

		// Sampled onto the same grid as the terrain, so the two meet at the
		// map edges and the sampling helpers can be shared verbatim.
		const int verts1D = terrainVerts1D;
		std::vector<float> heights(verts1D * verts1D, 0.0f);
		roofMinHeight = 1e9f;
		roofMaxHeight = -1e9f;
		for (int gz = 0; gz < verts1D; gz++) {
			const int sy = heightMapRowForGridZ(gz, h);
			for (int gx = 0; gx < verts1D; gx++) {
				const int sx = heightMapColForGridX(gx, w);
				const float height = rmap.getHeight(sx, sy).asFloat();
				heights[gz * verts1D + gx] = height;
				roofMinHeight = std::min(roofMinHeight, height);
				roofMaxHeight = std::max(roofMaxHeight, height);
			}
		}
		if (roofMaxHeight - roofMinHeight < 0.001f) roofMaxHeight = roofMinHeight + 1.0f;

		// The roof image out of the landscape's tex definition. Tiled at the
		// same density the ground texture builder uses for its own sources -
		// one source texel per landscape unit - so the rock on the ceiling
		// reads at the same scale as the rock on the ground.
		int texW = 0, texH = 0;
		LandscapeTex *tex = ctx.getLandscapeMaps().getDefinitions().getTex();
		if (tex && tex->texture &&
			tex->texture->getType() == LandscapeTexType::eTextureGenerate) {
			LandscapeTexTextureGenerate *generate =
				(LandscapeTexTextureGenerate *) tex->texture;
			roofTexture = loadSkyTexture(generate->roof, "", true, &texW, &texH);
		}
		// G8: upstream's SkyRoof puts the <roof> image once across the map
		// (its texture coordinate is position / map size), whatever the
		// image's pixel size. The port used to tile it by pixel size, which
		// only agreed on a 256 map with a 256 image.
		const float uScale = 1.0f;
		const float vScale = 1.0f;
		roofUScale = uScale;
		roofVScale = vScale;

		std::vector<float> vertexData(verts1D * verts1D * kTerrainFloatsPerVertex);
		const int last = verts1D - 1;
		for (int gz = 0; gz < verts1D; gz++) {
			for (int gx = 0; gx < verts1D; gx++) {
				float *out = &vertexData[(gz * verts1D + gx) * kTerrainFloatsPerVertex];

				// G4: the roof map's own normals. RoofMaps creates its
				// HeightMap with invertedNormals, so getNormal already
				// points down at the cavern floor - the face that matters.
				float n[3];
				renderNormalFromEngine(rmap.getNormal(heightMapColForGridX(gx, w),
													  heightMapRowForGridZ(gz, h)), n);
				const float nx = -n[0], ny = -n[1], nz = -n[2];

				out[0] = terrainWorldX[gx];
				out[1] = heights[gz * verts1D + gx];
				out[2] = terrainWorldZ[gz];
				out[3] = -nx;
				out[4] = -ny;
				out[5] = -nz;
				out[6] = (float) gx / (float) terrainGrid * uScale;
				out[7] = (1.0f - (float) gz / (float) terrainGrid) * vScale;
			}
		}

		// Wound the opposite way to the terrain's, so back-face culling
		// keeps the underside - the only side anyone can see - and discards
		// the top.
		std::vector<unsigned int> indices;
		indices.reserve(terrainGrid * terrainGrid * 6);
		for (int gz = 0; gz < terrainGrid; gz++) {
			for (int gx = 0; gx < terrainGrid; gx++) {
				const unsigned int i00 = gz * verts1D + gx;
				const unsigned int i10 = i00 + 1;
				const unsigned int i01 = i00 + verts1D;
				const unsigned int i11 = i01 + 1;
				indices.push_back(i00); indices.push_back(i10); indices.push_back(i01);
				indices.push_back(i10); indices.push_back(i11); indices.push_back(i01);
			}
		}
		roofIndexCount = (int) indices.size();

		if (roofVao == 0) glGenVertexArrays(1, &roofVao);
		glBindVertexArray(roofVao);
		if (roofVbo == 0) glGenBuffers(1, &roofVbo);
		glBindBuffer(GL_ARRAY_BUFFER, roofVbo);
		glBufferData(GL_ARRAY_BUFFER, vertexData.size() * sizeof(float),
					 vertexData.data(), GL_STATIC_DRAW);
		if (roofIbo == 0) glGenBuffers(1, &roofIbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, roofIbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int),
					 indices.data(), GL_STATIC_DRAW);
		const GLsizei stride = kTerrainFloatsPerVertex * sizeof(float);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void *) (3 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *) (6 * sizeof(float)));
		glBindVertexArray(0);

		buildRoofSkirt(ctx, heights);

		roofVisible = true;
		LOGI("Cavern roof: %d indices + %d skirt verts, heights %.1f-%.1f, "
			 "texture %s (%dx%d, %.1fx%.1f tiles)",
			 roofIndexCount, roofSkirtVertexCount, roofMinHeight, roofMaxHeight,
			 roofTexture ? "loaded" : "none", texW, texH, uScale, vScale);
	}

	// W4: keeps the ocean tile current.
	//
	// The spectrum is fixed by the wind, so it is only rebuilt when the wind
	// changes; what runs continuously is the far cheaper phase rotation and
	// the two inverse FFTs behind it. That runs on its own thread, and the
	// GL thread only ever uploads whatever is finished - so a slow frame
	// makes the sea move less smoothly rather than making the frame slower.
	void stopOceanWorker()
	{
		if (!oceanRunning.exchange(false)) return;
		if (oceanThread.joinable()) oceanThread.join();
		std::lock_guard<std::mutex> lock(oceanMutex);
		oceanHasNew = false;
	}

	void startOceanWorker()
	{
		if (oceanRunning.load()) return;
		oceanRunning.store(true);
		oceanThread = std::thread([]() {
			// Its own clock: the sea's phase should not depend on how often
			// the GL thread happens to ask for it.
			auto started = std::chrono::steady_clock::now();
			ScorchDroidOcean::Tile tile;
			while (oceanRunning.load()) {
				const float seconds = std::chrono::duration<float>(
						std::chrono::steady_clock::now() - started).count();
				ScorchDroidOcean::generate(seconds, tile);
				{
					std::lock_guard<std::mutex> lock(oceanMutex);
					oceanReady = tile;
					oceanHasNew = true;
				}
				// Upstream steps its baked phases 24 times a second; Half
				// and Quarter detail halve and quarter that, which is the
				// CPU side of the one water setting.
				const int detail = g_waterDetail.load();
				const int millis = detail >= 2 ? 41 : (detail == 1 ? 83 : 166);
				std::this_thread::sleep_for(std::chrono::milliseconds(millis));
			}
		});
	}

	void updateOceanIfNeeded(ScorchedContext &ctx)
	{
		// Upstream seeds its generator from the game's own wind, and falls
		// back to a diagonal breeze when the round is dead calm - a flat sea
		// is not a sea.
		// The tile's y axis lies along world z, and world z runs the
		// opposite way to the engine's y (worldZFromEngineY), so the
		// engine's wind is mirrored in y before it seeds the spectrum -
		// the same flip the noise scroll below makes. Without it the
		// swell ran mirrored against both the wind and the ripples.
		Wind &wind = ctx.getSimulator().getWind();
		FixedVector direction = wind.getWindDirection();
		float bearing = atan2f(direction[0].asFloat(), -direction[1].asFloat());
		if (direction[0] == fixed(0) && direction[1] == fixed(0)) {
			// Upstream's own fallback for a dead calm round: a diagonal
			// breeze, because a flat sea is not a sea.
			bearing = atan2f(0.8f, -0.8f);
		}
		// Upstream's own mapping, verbatim (Water2::generate): the game's
		// 0-5 wind becomes 3-13 for the spectrum. Worth taking exactly
		// rather than inventing a scale - it decides the wavelength, and a
		// wind twice as strong gives waves four times as long.
		const float generatorSpeed = wind.getWindSpeed().asFloat() * 2.0f + 3.0f;
		if (fabsf(generatorSpeed - oceanSeededSpeed) > 0.01f ||
			fabsf(bearing - oceanSeededDirection) > 0.01f) {
			ScorchDroidOcean::reseed(generatorSpeed, bearing, 0x5cd0u);
			oceanSeededSpeed = generatorSpeed;
			oceanSeededDirection = bearing;
		}

		startOceanWorker();

		// Upload whatever the worker has finished, if anything.
		bool haveNew = false;
		{
			std::lock_guard<std::mutex> lock(oceanMutex);
			if (oceanHasNew && !oceanReady.height.empty()) {
				const int n = ScorchDroidOcean::kResolution;
				oceanUpload.resize((size_t) n * n * 3);
				oceanNormalUpload.resize((size_t) n * n * 4);
				for (size_t i = 0; i < (size_t) n * n; i++) {
					oceanUpload[i * 3 + 0] = oceanReady.height[i];
					oceanUpload[i * 3 + 1] = oceanReady.dispX[i];
					oceanUpload[i * 3 + 2] = oceanReady.dispZ[i];
					// Upstream's own encoding (Water2Patches::generateNormalMap).
					oceanNormalUpload[i * 4 + 0] = (unsigned char) (oceanReady.normalX[i] * 127.0f + 128.0f);
					oceanNormalUpload[i * 4 + 1] = (unsigned char) (oceanReady.normalY[i] * 127.0f + 128.0f);
					oceanNormalUpload[i * 4 + 2] = (unsigned char) (oceanReady.normalZ[i] * 127.0f + 128.0f);
					oceanNormalUpload[i * 4 + 3] = (unsigned char) (oceanReady.foam[i] * 255.0f);
				}
				oceanHasNew = false;
				haveNew = true;
			}
		}
		if (!haveNew) return;

		const int n = ScorchDroidOcean::kResolution;
		if (oceanTexture == 0) {
			glGenTextures(1, &oceanTexture);
			glBindTexture(GL_TEXTURE_2D, oceanTexture);
			// Repeated across the sea, and filtered - RGB16F is
			// texture-filterable in ES3 without an extension, which the
			// 32-bit float formats are not.
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			// No mip chain: this texture is only ever read at level 0. A
			// CPU-built chain read at level 3 by the far ring came out as
			// a white sea on a phone (the emulator's software GL accepted
			// it), so the ring reads level 0 like the inner grid.
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, n, n, 0, GL_RGB, GL_FLOAT, nullptr);
		}
		if (oceanNormalTexture == 0) {
			glGenTextures(1, &oceanNormalTexture);
			glBindTexture(GL_TEXTURE_2D, oceanNormalTexture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			// Trilinear: the fragment shader reads this at 1/32 scale,
			// where a far pixel covers many texels and would shimmer
			// without the mip chain (upstream asks for hardware mipmaps
			// on exactly this texture).
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		}
		glBindTexture(GL_TEXTURE_2D, oceanTexture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGB, GL_FLOAT, oceanUpload.data());
		glBindTexture(GL_TEXTURE_2D, oceanNormalTexture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, oceanNormalUpload.data());
		glGenerateMipmap(GL_TEXTURE_2D);
		if (oceanUploadLogsLeft > 0) {
			oceanUploadLogsLeft--;
			float peak = 0.0f, peakDisp = 0.0f, peakTilt = 0.0f, peakFoam = 0.0f;
			for (size_t i = 0; i < (size_t) n * n; i++) {
				peak = std::max(peak, fabsf(oceanUpload[i * 3]));
				peakDisp = std::max(peakDisp, std::max(fabsf(oceanUpload[i * 3 + 1]), fabsf(oceanUpload[i * 3 + 2])));
				peakTilt = std::max(peakTilt, fabsf(oceanReady.normalX[i]));
				peakFoam = std::max(peakFoam, oceanReady.foam[i]);
			}
			LOGI("Ocean tile uploaded: wind %.1f bearing %.2f, peak height %.2f, peak displacement %.2f, peak normal tilt %.3f, peak foam %.2f",
				 oceanSeededSpeed, oceanSeededDirection, peak, peakDisp, peakTilt, peakFoam);
		}
	}

	// W10c: (re)builds the breaker quads from the current heightmap. Called
	// with the water, and again after a crater has moved the shoreline.
	// Engine landscape coordinates become world ones here: world z runs
	// the other way to engine y, and so does the perpendicular's y.
	void buildBreakers(ScorchedContext &ctx)
	{
		static unsigned int seed = 0x9e3779b9u;
		std::vector<ScorchDroidBreakers::Segment> segments =
			ScorchDroidBreakers::build(ctx, waterHeight, seed);

		// Two triangles per segment, corners A(1,1) B(0,1) C(0,0) D(1,0)
		// as upstream's glTexCoord/glVertex order has them; A and D share
		// the "current" end, B and C the "point" end. The vertex shader
		// picks the corner's home from its uv: y > 0.5 is a shore-side
		// corner (A, B), which upstream slides by frontlen, and the
		// seaward pair (C, D) by endlen - so the home stored for A is D's
		// position and for B is C's, exactly as drawBoxes computes them.
		std::vector<float> verts;
		verts.reserve(segments.size() * 6 * 6);
		auto push = [&](float ex, float ey, float px, float py, float u, float v) {
			verts.push_back(ex);
			verts.push_back(worldZFromEngineY(ey));
			verts.push_back(px);
			verts.push_back(-py);
			verts.push_back(u);
			verts.push_back(v);
		};
		for (int set = 0; set < 2; set++) {
			const size_t start = verts.size();
			for (const ScorchDroidBreakers::Segment &sg : segments) {
				if (sg.set != set) continue;
				// A = D - perp*front, B = C - perp*front, C' = C - perp*end, D' = D - perp*end.
				push(sg.dx, sg.dy, sg.perpX, sg.perpY, 1.0f, 1.0f);   // A
				push(sg.cx, sg.cy, sg.perpX, sg.perpY, 0.0f, 1.0f);   // B
				push(sg.cx, sg.cy, sg.perpX, sg.perpY, 0.0f, 0.0f);   // C
				push(sg.dx, sg.dy, sg.perpX, sg.perpY, 1.0f, 1.0f);   // A
				push(sg.cx, sg.cy, sg.perpX, sg.perpY, 0.0f, 0.0f);   // C
				push(sg.dx, sg.dy, sg.perpX, sg.perpY, 1.0f, 0.0f);   // D
			}
			breakerVertexCount[set] = (int) ((verts.size() - start) / 6);
		}

		if (breakerVao == 0) glGenVertexArrays(1, &breakerVao);
		if (breakerVbo == 0) glGenBuffers(1, &breakerVbo);
		glBindVertexArray(breakerVao);
		glBindBuffer(GL_ARRAY_BUFFER, breakerVbo);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (verts.size() * sizeof(float)),
					 verts.empty() ? nullptr : verts.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) (2 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) (4 * sizeof(float)));
		glBindVertexArray(0);

		// Upstream's two images, loaded as alpha images: the bitmap's own
		// colour, opaque wherever it is not black (ImageBitmapFactory).
		if (breakerTexture[0] == 0) {
			const char *files[2] = { "data/textures/waves.bmp", "data/textures/waves2.bmp" };
			for (int i = 0; i < 2; i++) {
				Image image = ImageFactory::loadAlphaImage(S3D::eModLocation, files[i]);
				if (!image.getBits() || image.getComponents() != 4) {
					LOGI("Breakers: could not load %s", files[i]);
					continue;
				}
				glGenTextures(1, &breakerTexture[i]);
				glBindTexture(GL_TEXTURE_2D, breakerTexture[i]);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.getWidth(), image.getHeight(), 0,
							 GL_RGBA, GL_UNSIGNED_BYTE, image.getBits());
				glGenerateMipmap(GL_TEXTURE_2D);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glBindTexture(GL_TEXTURE_2D, 0);
			}
		}
		breakersDirty = false;
		breakersRebuiltAt = lastFrameSeconds;
		LOGI("Breakers: %d segments (%d + %d quads)", (int) segments.size(),
			 breakerVertexCount[0] / 6, breakerVertexCount[1] / 6);
	}

	// W11: the surface mesh, at the given Water detail. See the note at
	// waterGridEbo for the shape of it.
	void buildWaterGrid(int detail)
	{
		const float cell = (detail >= 2) ? 2.0f : (detail == 1 ? 4.0f : 8.0f);
		waterInnerLod = (detail >= 2) ? 0.0f : (detail == 1 ? 1.0f : 2.0f);
		const float outerCell = 16.0f;

		// The inner rectangle: the map and 64 units around it, on the
		// 16-unit lattice so the outer ring's vertices land on its edge.
		const float ix0 = -64.0f, iz0 = -64.0f;
		const float ix1 = ceilf((mapWidthUnits + 64.0f) / outerCell) * outerCell;
		const float iz1 = ceilf((mapHeightUnits + 64.0f) / outerCell) * outerCell;
		const int inx = (int) ((ix1 - ix0) / cell), inz = (int) ((iz1 - iz0) / cell);
		const int perOuter = (int) (outerCell / cell);   // inner vertices per outer edge

		// The outer extent: out to the far clip plane on every side, as
		// before - the horizon is where the far plane clips the sea, so
		// the surface must reach it.
		const float margin = std::max(mapWidthUnits, mapHeightUnits) * 3.0f + 200.0f;
		const int outLeft = (int) ceilf((ix0 + margin) / outerCell);
		const int outRight = (int) ceilf((margin + mapWidthUnits - ix1) / outerCell);
		const int outNear = (int) ceilf((iz0 + margin) / outerCell);
		const int outFar = (int) ceilf((margin + mapHeightUnits - iz1) / outerCell);
		const float ox0 = ix0 - outLeft * outerCell, oz0 = iz0 - outNear * outerCell;
		const int onx = outLeft + (int) ((ix1 - ix0) / outerCell) + outRight;
		const int onz = outNear + (int) ((iz1 - iz0) / outerCell) + outFar;

		std::vector<float> verts;
		verts.reserve((size_t) (inx + 1) * (inz + 1) * 3 + (size_t) (onx + 1) * (onz + 1) * 3);
		auto addVertex = [&](float x, float z) {
			verts.push_back(x);
			verts.push_back(waterHeight);
			verts.push_back(z);
			return (unsigned int) (verts.size() / 3 - 1);
		};

		// Inner vertices, row-major.
		for (int gz = 0; gz <= inz; gz++) {
			for (int gx = 0; gx <= inx; gx++) addVertex(ix0 + gx * cell, iz0 + gz * cell);
		}
		auto innerIndex = [&](int gx, int gz) { return (unsigned int) (gz * (inx + 1) + gx); };

		// Outer lattice vertices: allocated on demand, and a lattice point
		// on the inner rectangle's edge *is* the inner vertex there.
		std::vector<int> outerTable((size_t) (onx + 1) * (onz + 1), -1);
		auto outerIndex = [&](int ox, int oz) -> unsigned int {
			const float x = ox0 + ox * outerCell, z = oz0 + oz * outerCell;
			if (x >= ix0 && x <= ix1 && z >= iz0 && z <= iz1) {
				return innerIndex((int) ((x - ix0) / cell), (int) ((z - iz0) / cell));
			}
			int &slot = outerTable[(size_t) oz * (onx + 1) + ox];
			if (slot < 0) slot = (int) addVertex(x, z);
			return (unsigned int) slot;
		};

		std::vector<unsigned int> indices;
		indices.reserve((size_t) inx * inz * 6 + (size_t) onx * onz * 6);
		for (int gz = 0; gz < inz; gz++) {
			for (int gx = 0; gx < inx; gx++) {
				const unsigned int a = innerIndex(gx, gz), b = innerIndex(gx + 1, gz);
				const unsigned int c = innerIndex(gx, gz + 1), d = innerIndex(gx + 1, gz + 1);
				indices.insert(indices.end(), { a, c, b, b, c, d });
			}
		}
		waterInnerIndexCount = (int) indices.size();

		// The outer ring. A cell strictly inside the inner rectangle is
		// skipped; one with an edge on its boundary is a stitch fan; the
		// rest are plain quads.
		auto stitch = [&](unsigned int f0, unsigned int f1, int sx, int sz, int dx, int dz) {
			// Seam vertices s_0..s_k run from inner (sx, sz) in steps of
			// (dx, dz); f0 sits beside s_0 and f1 beside s_k.
			const int k = perOuter;
			std::vector<unsigned int> seam;
			for (int j = 0; j <= k; j++) seam.push_back(innerIndex(sx + j * dx, sz + j * dz));
			for (int j = 0; j < k; j++) {
				const unsigned int far = (j < k / 2) ? f0 : f1;
				indices.insert(indices.end(), { seam[j], seam[j + 1], far });
			}
			indices.insert(indices.end(), { f0, seam[k / 2], f1 });
		};
		for (int oz = 0; oz < onz; oz++) {
			for (int ox = 0; ox < onx; ox++) {
				const float x0 = ox0 + ox * outerCell, x1 = x0 + outerCell;
				const float z0 = oz0 + oz * outerCell, z1 = z0 + outerCell;
				const bool inside = x0 >= ix0 && x1 <= ix1 && z0 >= iz0 && z1 <= iz1;
				if (inside) continue;
				const unsigned int a = outerIndex(ox, oz), b = outerIndex(ox + 1, oz);
				const unsigned int c = outerIndex(ox, oz + 1), d = outerIndex(ox + 1, oz + 1);
				// An edge on the seam: both of its ends on the inner
				// boundary and the cell outside it.
				const bool onLeft = (x1 == ix0) && z0 >= iz0 && z1 <= iz1;    // cell's right edge is the seam
				const bool onRight = (x0 == ix1) && z0 >= iz0 && z1 <= iz1;   // left edge
				const bool onNear = (z1 == iz0) && x0 >= ix0 && x1 <= ix1;    // far edge (z1)
				const bool onFar = (z0 == iz1) && x0 >= ix0 && x1 <= ix1;     // near edge (z0)
				if (onLeft) {
					stitch(a, c, (int) ((x1 - ix0) / cell), (int) ((z0 - iz0) / cell), 0, 1);
				} else if (onRight) {
					stitch(b, d, (int) ((x0 - ix0) / cell), (int) ((z0 - iz0) / cell), 0, 1);
				} else if (onNear) {
					stitch(a, b, (int) ((x0 - ix0) / cell), (int) ((z1 - iz0) / cell), 1, 0);
				} else if (onFar) {
					stitch(c, d, (int) ((x0 - ix0) / cell), (int) ((z0 - iz0) / cell), 1, 0);
				} else {
					indices.insert(indices.end(), { a, c, b, b, c, d });
				}
			}
		}
		waterOuterIndexCount = (int) indices.size() - waterInnerIndexCount;

		if (waterGridVao == 0) glGenVertexArrays(1, &waterGridVao);
		if (waterGridVbo == 0) glGenBuffers(1, &waterGridVbo);
		if (waterGridEbo == 0) glGenBuffers(1, &waterGridEbo);
		glBindVertexArray(waterGridVao);
		glBindBuffer(GL_ARRAY_BUFFER, waterGridVbo);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, waterGridEbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (indices.size() * sizeof(unsigned int)),
					 indices.data(), GL_STATIC_DRAW);
		glBindVertexArray(0);
		waterGridBuiltForDetail = detail;
		LOGI("Water grid: detail %d, cell %.0f, %d vertices, %d + %d triangles",
			 detail, cell, (int) (verts.size() / 3), waterInnerIndexCount / 3, waterOuterIndexCount / 3);
	}

	// M6 water: reads the landscape's own water definition and builds the
	// surface quad. Called each frame; does nothing after the first attempt
	// for a given landscape (waterBuilt is cleared when one is thrown away,
	// alongside the terrain and ground texture).
	void buildWaterIfNeeded(ScorchedContext &ctx)
	{
		if (waterBuilt) return;
		waterBuilt = true;
		waterVisible = false;

		LandscapeTex *tex = ctx.getLandscapeMaps().getDefinitions().getTex();
		if (!tex || !tex->border) return;
		if (tex->border->getType() != LandscapeTexType::eWater) {
			LOGI("Landscape has no water border");
			return;
		}

		LandscapeTexBorderWater *water = (LandscapeTexBorderWater *) tex->border;
		waterHeight = water->height.asFloat();

		// Upstream's own two upwelling colours, resolved its own way
		// (Water2Renderer::generate): the "a" and "b" pair of each are the
		// ends of a gradient and <wavelight> is the position along it, so
		//   wavetop    = lerp(wavetopa,    wavetopb,    wavelight)
		//   wavebottom = lerp(wavebottoma, wavebottomb, wavelight)
		// The "a" pair is black in every shipped landscape and wavelight is
		// white in most, which is why taking "b" straight looked right - but
		// not on the ones that do set it. Hell's <wavelight> is (1, 0.3, 0.3)
		// and turns its sea red, and that only happens if the lerp is real.
		//
		// The previous darkening of the deep colour by 0.55 is gone with the
		// gradient it existed to serve: these are now upstream's top and
		// bottom, and the shader picks between them by wave slope exactly as
		// upstream's vertex shader does.
		for (int i = 0; i < 3; i++) {
			const float light = water->wavelight[i];
			waterUpwellTop[i] = water->wavetopa[i] + (water->wavetopb[i] - water->wavetopa[i]) * light;
			waterUpwellBot[i] = water->wavebottoma[i] + (water->wavebottomb[i] - water->wavebottoma[i]) * light;
		}

		// Upstream's water is opaque. Its shader has a depth-transparency
		// branch, but the uniform that gates it (landfoam) is set from a
		// default Vector - all zero - so the branch never runs and the alpha
		// is the landscape's own <watertransparency>, which is 1.0 in every
		// shipped landscape, unless the hide-water key is held. The 0.82 this
		// port drew at showed the drowned terrain through the whole sea.
		waterAlpha = std::min(1.0f, std::max(0.0f, water->waterTransparency));

		buildWaterGrid(g_waterDetail.load());

		// The landscape's foam bitmap (texdefault.xml: data/textures/foam.png,
		// 128 square - upstream insists on exactly that size, since it also
		// writes its foam amounts into the same image). Repeated, as the
		// shader tiles it.
		if (waterFoamMaskTexture != 0) glDeleteTextures(1, &waterFoamMaskTexture);
		waterFoamMaskTexture = loadSkyTexture(water->foam, "", true);
		if (waterFoamMaskTexture == 0) LOGI("Water: no foam bitmap (%s)", water->foam.c_str());

		// Upstream's second ripple wind (Water2Renderer::generate):
		//   windSpeed2 = max(0, RAND * 2 - 1 + windSpeed1)
		//   windDir2   = normalize(windDir1 + (RAND * 0.4 - 0.2, RAND * 0.4 - 0.2))
		// Rolled here, once per landscape, with a small generator of its
		// own so it cannot disturb anything else's randomness.
		{
			static unsigned int roll = 0x2545f491u;
			auto next = [&]() {
				roll = roll * 1664525u + 1013904223u;
				return (float) ((roll >> 8) & 0xffffffu) / (float) 0x1000000u;
			};
			waterWind2SpeedOffset = next() * 2.0f - 1.0f;
			waterWind2JitterX = next() * 0.4f - 0.2f;
			waterWind2JitterZ = next() * 0.4f - 0.2f;
		}

		waterVisible = true;
		LOGI("Water surface at height %.1f, alpha %.2f", waterHeight, waterAlpha);
		buildBreakers(ctx);
		LOGI("Water upwelling: bottom (%.2f, %.2f, %.2f), top (%.2f, %.2f, %.2f)",
			 waterUpwellBot[0], waterUpwellBot[1], waterUpwellBot[2],
			 waterUpwellTop[0], waterUpwellTop[1], waterUpwellTop[2]);
	}

	// M6 terrain destruction: craters are carved into the real heightmap by
	// DeformLandscape (which runs under S3D_SERVER - the simulation half was
	// never client-only), but the mesh was built once per round, so the
	// ground silently changed shape under a static picture. The patched
	// engine now reports each deformed region (see DeformEventQueue.h); this
	// re-samples just those grid vertices and re-uploads them.
	//
	// Only the affected rows are touched, one glBufferSubData per row - a
	// crater is a handful of grid cells across, so this is a few hundred
	// bytes per blast rather than the ~300KB a full re-upload would cost.
	void applyTerrainDeformations(ScorchedContext &ctx)
	{
		if (!terrainBuilt) return;

		int minX, minY, maxX, maxY;
		if (!ScorchDroidLandscape::takeDirtyRegion(minX, minY, maxX, maxY)) return;
		// A crater at the waterline moves the shore, and the breakers with
		// it (W10c). Rebuilt lazily by the water draw, at most twice a
		// second, since a sustained weapon deforms every step.
		breakersDirty = true;

		HeightMap &heightMap = ctx.getLandscapeMaps().getGroundMaps().getHeightMap();
		const int w = heightMap.getMapWidth();
		const int h = heightMap.getMapHeight();
		if (w != terrainSrcWidth || h != terrainSrcHeight) return;  // mid-rebuild

		// Heightmap cells -> grid vertices. Pad by 2: a vertex's normal is a
		// central difference over its neighbours, so vertices just outside
		// the crater still change, and integer division either way loses up
		// to a cell.
		//
		// The row range flips end for end on the way across: grid rows run
		// opposite to landscape y (see worldZFromEngineY), so the dirty
		// region's *max* y is its lowest grid row.
		const int last = terrainVerts1D - 1;
		int gx0 = std::max(gridXForHeightMapCol(minX, w) - 2, 0);
		int gx1 = std::min(gridXForHeightMapCol(maxX, w) + 2, last);
		int gz0 = std::max(gridZForHeightMapRow(maxY, h) - 2, 0);
		int gz1 = std::min(gridZForHeightMapRow(minY, h) + 2, last);
		if (gx0 > gx1 || gz0 > gz1) return;  // entirely off-map

		// Re-sample heights first (over a further 1-vertex margin, since
		// writeTerrainVertex reads its neighbours' heights), then rebuild
		// vertices - doing both in one pass would use stale neighbours.
		for (int gz = std::max(gz0 - 1, 0); gz <= std::min(gz1 + 1, last); gz++) {
			int sy = heightMapRowForGridZ(gz, h);
			for (int gx = std::max(gx0 - 1, 0); gx <= std::min(gx1 + 1, last); gx++) {
				int sx = heightMapColForGridX(gx, w);
				float height = heightMap.getHeight(sx, sy).asFloat();
				terrainHeights[gz * terrainVerts1D + gx] = height;
				// setHeight zeroed the normals around the crater; getNormal
				// recomputes them here, on this thread, under the lock.
				renderNormalFromEngine(heightMap.getNormal(sx, sy),
									   &terrainNormals[(size_t) (gz * terrainVerts1D + gx) * 3]);
				// The shader colours by height ratio, so let the range grow
				// with a crater rather than clamping new extremes flat.
				terrainMinHeight = std::min(terrainMinHeight, height);
				terrainMaxHeight = std::max(terrainMaxHeight, height);
			}
		}

		const int rowVerts = gx1 - gx0 + 1;
		std::vector<float> row(rowVerts * kTerrainFloatsPerVertex);
		glBindBuffer(GL_ARRAY_BUFFER, terrainVbo);
		for (int gz = gz0; gz <= gz1; gz++) {
			for (int i = 0; i < rowVerts; i++) {
				writeTerrainVertex(gx0 + i, gz, &row[i * kTerrainFloatsPerVertex]);
			}
			GLintptr offset = (GLintptr) (gz * terrainVerts1D + gx0)
				* kTerrainFloatsPerVertex * sizeof(float);
			glBufferSubData(GL_ARRAY_BUFFER, offset,
							row.size() * sizeof(float), row.data());
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		// Only the first few per landscape: sustained weapons (napalm above
		// all) deform every simulation step, so an unconditional log here
		// would be one line per rendered frame.
		if (terrainDeformLogsLeft > 0) {
			terrainDeformLogsLeft--;
			LOGI("Terrain deformed: map [%d,%d]-[%d,%d] -> grid [%d,%d]-[%d,%d], %d verts",
				 minX, minY, maxX, maxY, gx0, gz0, gx1, gz1, rowVerts * (gz1 - gz0 + 1));
		}
	}

	// M6 shield bubbles and parachutes. Upstream builds these from GLU
	// quadrics and display lists (TargetRendererImpl::drawShield /
	// drawParachute), neither of which exists in GLES3, so the shapes are
	// generated once here as plain vertex buffers instead. Positions and
	// normals only - both are drawn with the existing mesh shader.
	GLuint sphereVao = 0, sphereVbo = 0;   int sphereVertexCount = 0;
	GLuint hemiVao = 0, hemiVbo = 0;       int hemiVertexCount = 0;
	GLuint cubeVao = 0, cubeVbo = 0;       int cubeVertexCount = 0;
	GLuint chuteVao = 0, chuteVbo = 0;     int chuteVertexCount = 0;
	GLuint chuteCordVao = 0, chuteCordVbo = 0; int chuteCordVertexCount = 0;

	void uploadPosNormal(GLuint &vao, GLuint &vbo, const std::vector<float> &data)
	{
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) (3 * sizeof(float)));
		glBindVertexArray(0);
	}

	// A unit sphere as triangles. `startRow` at half the stacks yields the
	// upper hemisphere, which is upstream's half-shield.
	std::vector<float> buildSphere(int stacks, int slices, int startRow)
	{
		std::vector<float> data;
		auto point = [&](int stack, int slice) {
			const float phi = (float) M_PI * (float) stack / (float) stacks;
			const float theta = 2.0f * (float) M_PI * (float) slice / (float) slices;
			const float y = cosf(phi), r = sinf(phi);
			const float x = r * cosf(theta), z = r * sinf(theta);
			// Unit sphere, so the position doubles as the normal.
			for (float v : { x, y, z, x, y, z }) data.push_back(v);
		};
		for (int stack = startRow; stack < stacks; stack++) {
			for (int slice = 0; slice < slices; slice++) {
				point(stack, slice);     point(stack + 1, slice); point(stack, slice + 1);
				point(stack, slice + 1); point(stack + 1, slice); point(stack + 1, slice + 1);
			}
		}
		return data;
	}

	std::vector<float> buildCube()
	{
		std::vector<float> data;
		const float f[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
		for (const auto &n : f) {
			// Two in-plane axes for this face.
			float a[3] = { n[1], n[2], n[0] };
			float b[3] = { n[2], n[0], n[1] };
			auto corner = [&](float sa, float sb) {
				for (int i = 0; i < 3; i++) data.push_back(n[i] + a[i] * sa + b[i] * sb);
				for (int i = 0; i < 3; i++) data.push_back(n[i]);
			};
			corner(-1,-1); corner(1,-1); corner(1,1);
			corner(-1,-1); corner(1,1);  corner(-1,1);
		}
		return data;
	}

	// The canopy: a cone from an apex at y=3 down to a ring of radius 2 at
	// y=2, matching upstream's triangle fan.
	std::vector<float> buildParachuteCanopy(int slices)
	{
		std::vector<float> data;
		for (int i = 0; i < slices; i++) {
			const float t0 = 2.0f * (float) M_PI * (float) i / (float) slices;
			const float t1 = 2.0f * (float) M_PI * (float) (i + 1) / (float) slices;
			const float x0 = sinf(t0) * 2.0f, z0 = cosf(t0) * 2.0f;
			const float x1 = sinf(t1) * 2.0f, z1 = cosf(t1) * 2.0f;
			const float verts[3][3] = {{0.0f, 3.0f, 0.0f}, {x0, 2.0f, z0}, {x1, 2.0f, z1}};
			for (const auto &v : verts) {
				data.push_back(v[0]); data.push_back(v[1]); data.push_back(v[2]);
				// Outward-ish normal; the canopy is unlit enough that a
				// per-face approximation is plenty.
				data.push_back(v[0] * 0.4f); data.push_back(0.8f); data.push_back(v[2] * 0.4f);
			}
		}
		return data;
	}

	// Eight cords from the tank up to the canopy rim, as upstream draws.
	std::vector<float> buildParachuteCords(int count)
	{
		std::vector<float> data;
		for (int i = 0; i < count; i++) {
			const float t = 2.0f * (float) M_PI * (float) i / (float) count;
			const float pts[2][3] = {{0.0f, 0.0f, 0.0f}, {sinf(t) * 2.0f, 2.0f, cosf(t) * 2.0f}};
			for (const auto &v : pts) {
				data.push_back(v[0]); data.push_back(v[1]); data.push_back(v[2]);
				data.push_back(0.0f); data.push_back(1.0f); data.push_back(0.0f);
			}
		}
		return data;
	}

	// M6 effects. The engine raises one event per explosion / napalm flame /
	// laser / lightning arc / shield impact (see EffectEventQueue.h, fed by
	// patch 0011); this turns each into particles or beams. Upstream's own
	// versions are ParticleEmitter bursts, textured quads and gluQuadrics in
	// the excluded client layer, so the look here is a reinterpretation
	// rather than a port - but the *timing, position, size and colour* are
	// the engine's own, so it fires when and where upstream fires.
	//
	// Note the coordinate swizzle: events arrive in engine space
	// (x, y, height) and the renderer works in a y-up world, so height
	// becomes y and engine y becomes z.
	float randomUnit()
	{
		return (float) rand() / (float) RAND_MAX;
	}

	float randomSigned()
	{
		return randomUnit() * 2.0f - 1.0f;
	}

	void addParticle(const Particle &particle)
	{
		if (particles.size() >= (size_t) g_maxParticles.load()) return;
		particles.push_back(particle);
	}

	// V1: the sprite array texture, from the loaded sets. Built on first
	// use so it comes after the surface-created reset, and rebuilt from
	// the CPU copy after a context loss.
	void ensureSpriteTexture()
	{
		if (spriteArrayTexture != 0) return;
		if (!spriteAtlas.valid()) {
			spriteAtlas = ScorchDroidParticleTextures::load();
			LOGI("Particle textures: %d layers in %d sets", spriteAtlas.layers, (int) spriteAtlas.sets.size());
		}
		if (!spriteAtlas.valid()) return;
		const int n = ScorchDroidParticleTextures::kSize;
		glGenTextures(1, &spriteArrayTexture);
		glBindTexture(GL_TEXTURE_2D_ARRAY, spriteArrayTexture);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, n, n, spriteAtlas.layers, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, spriteAtlas.rgba.data());
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	}

	// Gives a particle upstream's texture set: every frame by life when
	// animated, else one frame at random (ParticleEmitter's two branches).
	// An unknown or empty name leaves the soft disc.
	void setSprite(Particle &particle, const std::string &setName, bool animate)
	{
		if (!spriteAtlas.valid()) spriteAtlas = ScorchDroidParticleTextures::load();
		const ScorchDroidParticleTextures::Set *set = spriteAtlas.find(setName);
		if (!set || set->count <= 0) return;
		if (animate && set->count > 1) {
			particle.layer = set->firstLayer;
			particle.frames = set->count;
		} else {
			const int index = std::min((int) (randomUnit() * (float) (set->count - 1)), set->count - 1);
			particle.layer = set->firstLayer + index;
			particle.frames = 1;
		}
		particle.orient = (int) (randomUnit() * 4.0f) & 3;
		particle.linearAlpha = true;
	}

	// The sampled terrain's height under a render-space point, from the
	// grid the mesh was built from; 0 off the map.
	float sampledGroundHeight(float x, float z)
	{
		if (terrainHeights.empty() || mapWidthUnits <= 0.0f || mapHeightUnits <= 0.0f) return 0.0f;
		const int gx = std::min(std::max((int) (x / mapWidthUnits * (float) terrainGrid), 0), terrainVerts1D - 1);
		const int gz = std::min(std::max((int) (z / mapHeightUnits * (float) terrainGrid), 0), terrainVerts1D - 1);
		return terrainHeights[(size_t) gz * terrainVerts1D + gx];
	}

	// ExplosionNukeRenderer's path table: 100 steps over 16/3 s, rising
	// two units a step for the first 30% and spreading half a unit a step
	// after. Scaled per cloud by its size at use.
	const int kMushroomSteps = 100;
	void mushroomPath(int step, float &width, float &height)
	{
		float w = 0.0f, h = 0.0f;
		for (int i = 0; i < step; i++) {
			if (i > (int) (kMushroomSteps * 0.3f)) w += 0.5f; else h += 2.0f;
		}
		width = w; height = h;
	}

	// One puff of a nuke cloud, ParticleEmitter::emitMushroom with
	// ExplosionNukeRenderer's attributes: life 5, white, alpha 0.3-0.2 to
	// 0, 2-3 units growing to 4-6, alpha-blended, its own random spread
	// direction of length 1-1.5 (ExplosionNukeRendererEntry).
	void spawnMushroomPuff(const MushroomEmitter &cloud)
	{
		Particle puff = {};
		puff.kind = 3;
		puff.startX = cloud.x; puff.startY = cloud.y; puff.startZ = cloud.z;
		puff.x = cloud.x; puff.y = cloud.y; puff.z = cloud.z;
		const float rot = randomUnit() * 6.2831853f;
		const float width = 1.0f + randomUnit() * 0.5f;
		puff.spreadX = sinf(rot) * width;
		puff.spreadZ = cosf(rot) * width;
		puff.mushroomSize = cloud.size;
		puff.r = puff.g = puff.b = 1.0f;
		puff.life = 5.0f;
		const float born = 2.0f + randomUnit();
		const float dies = 4.0f + randomUnit() * 2.0f;
		puff.worldSize = born;
		puff.growth = dies / born - 1.0f;
		puff.peakAlpha = 0.2f + randomUnit() * 0.1f;
		puff.endAlpha = 0.0f;
		puff.linearAlpha = true;
		puff.alphaBlend = true;
		puff.drag = 1.0f;
		puff.gravityScale = 0.0f;
		if (cloud.frames > 0) {
			puff.layer = cloud.layer + std::min((int) (randomUnit() * (float) (cloud.frames - 1)), cloud.frames - 1);
			puff.frames = 1;
		}
		puff.orient = (int) (randomUnit() * 4.0f) & 3;
		addParticle(puff);
	}

	// Steps the clouds: ExplosionNukeRenderer::simulate, 0.08 s between
	// bursts, from 1.25 s to 2.25 s, 8 / 14 / 18 puffs a burst by upstream's
	// effects detail (which this port's particle budget stands in for).
	void stepMushroomEmitters(float deltaSeconds)
	{
		const int budget = g_maxParticles.load();
		const int perBurst = (budget <= 100) ? 8 : (budget >= 10000 ? 18 : 14);
		size_t live = 0;
		for (size_t i = 0; i < mushroomEmitters.size(); i++) {
			MushroomEmitter &cloud = mushroomEmitters[i];
			cloud.time += deltaSeconds;
			cloud.accumulator += deltaSeconds;
			while (cloud.accumulator > 0.08f) {
				cloud.accumulator -= 0.08f;
				if (cloud.time > 1.25f && cloud.time < 2.25f) {
					for (int n = 0; n < perBurst; n++) spawnMushroomPuff(cloud);
				}
			}
			if (cloud.time < 2.25f) mushroomEmitters[live++] = cloud;
		}
		mushroomEmitters.resize(live);
	}

	// One lingering puff, with upstream's own Smoke emitter numbers
	// (src/client/landscape/Smoke.cpp): 2-4 seconds, grey, 0.6-0.8 opaque
	// falling to nothing, growing from about 0.35 to about 1.35 units, and
	// rising - its emitter gravity is +Z, unlike every other emitter in the
	// game, which is what makes smoke drift up off a fire. Wind-affected,
	// with upstream's mass of 0.2-0.5, so it streams downwind. Raised by
	// the eSmoke event (muzzle, napalm, driving) and by a damaged tank.
	void spawnSmokePuff(float x, float y, float z)
	{
		Particle puff = {};
		puff.x = x; puff.y = y; puff.z = z;
		puff.vx = randomSigned() * 0.3f;
		puff.vy = 1.2f + randomUnit() * 0.8f;
		puff.vz = randomSigned() * 0.3f;
		puff.r = 0.8f; puff.g = 0.8f; puff.b = 0.8f;
		puff.mass = 0.2f + randomUnit() * 0.3f;
		const float born = 0.2f + randomUnit() * 0.3f;
		const float dies = 1.2f + randomUnit() * 0.3f;
		puff.worldSize = born;
		puff.growth = dies / born - 1.0f;
		puff.life = 2.0f + randomUnit() * 2.0f;
		// Upstream's friction 0.01-0.02 a second, and its +400 gravity on
		// the time-squared integrator: 400/60 a second squared at 60 fps,
		// times the mass, upward - against this port's 3.15 per unit of
		// gravityScale.
		puff.drag = 0.985f;
		puff.gravityScale = -(400.0f / 60.0f * puff.mass) / 3.15f;
		puff.peakAlpha = 0.6f + randomUnit() * 0.2f;
		puff.endAlpha = 0.0f;
		puff.alphaBlend = true;
		puff.windAffect = true;
		setSprite(puff, "smoke", false);
		addParticle(puff);
	}

	// X4a: upstream's damaged-tank smoke, TargetRendererImplTank::simulate
	// step for step: a tank in its normal state with less than full life
	// puffs from its turret, then waits (rand * life * 10 + 250) / 3000
	// seconds - 0.08 s at death's door, 0.4 s barely scratched.
	void emitTankSmoke(ScorchedContext &ctx, float deltaSeconds)
	{
		std::map<unsigned int, Tank *> &tanks = ctx.getTargetContainer().getTanks();
		for (std::map<unsigned int, Tank *>::iterator it = tanks.begin(); it != tanks.end(); ++it) {
			Tank *tank = it->second;
			if (!tank || tank->getState().getState() != TankState::sNormal) continue;
			const float life = tank->getLife().getLife().asFloat();
			if (life >= tank->getLife().getMaxLife().asFloat()) continue;
			TankSmoke &smoke = tankSmoke[it->first];
			smoke.time += deltaSeconds;
			if (smoke.time < smoke.waitFor) continue;
			FixedVector &turret = tank->getLife().getTankTurretPosition();
			const float randX = randomUnit() - 0.5f, randY = randomUnit() - 0.5f;
			spawnSmokePuff(turret[0].asFloat() + randX,
						   turret[2].asFloat(),
						   worldZFromEngineY(turret[1].asFloat() + randY));
			smoke.waitFor = (randomUnit() * life * 10.0f + 250.0f) / 3000.0f;
			smoke.time = 0.0f;
			static bool logged = false;
			if (!logged) {
				logged = true;
				LOGI("Tank smoke: first puff, tank %u at life %.0f", it->first, life);
			}
		}
	}

	// X4b: upstream's splash spray (Water::explosion, its emitSpray): for a
	// blast under the water, 6 + 2 * width white flecks scattered within
	// `width` of the point, thrown up at 15-40 units a second and out a
	// little, 3-4 units across, half opaque fading to nothing over 3-4
	// seconds, wind-affected with a mass of 0.5-1. Its gravity of -800 on
	// upstream's time-squared integrator is about 13 units per second
	// squared per unit of mass at 60 fps; this port's integrator applies
	// 3.15 * gravityScale, hence the factor.
	void spawnSplash(float x, float y, float z, float width)
	{
		const int count = 6 + (int) std::max(width, 0.0f) * 2;
		for (int i = 0; i < count; i++) {
			const float rotation = randomUnit() * 6.2831853f;
			const float sx = sinf(rotation), sy = cosf(rotation);
			const float mass = 0.5f + randomUnit() * 0.5f;
			Particle drop = {};
			drop.x = x + sx * width * randomUnit();
			drop.z = z - sy * width * randomUnit();
			drop.y = y;
			drop.vx = sx * randomUnit() / 10.0f * mass;
			drop.vz = -sy * randomUnit() / 10.0f * mass;
			drop.vy = (25.0f * randomUnit() + 15.0f) * mass;
			const float shade = 0.9f + randomUnit() * 0.1f;
			drop.r = shade; drop.g = shade; drop.b = shade;
			drop.worldSize = 3.0f + randomUnit();
			drop.growth = 0.0f;
			drop.life = 3.0f + randomUnit();
			drop.drag = 0.985f;
			drop.gravityScale = 4.2f * mass;
			drop.peakAlpha = 0.5f + randomUnit() * 0.2f;
			drop.alphaBlend = true;
			drop.windAffect = true;
			drop.mass = mass;
			addParticle(drop);
		}
	}

	// X4c: upstream's rain and snow (TargetCamera's two emitters, emitted
	// through ParticleEmitter::emitPrecipitation): every tenth of a second
	// the landscape's `particles` count, placed within 200 units of the
	// camera at a height of 180, falling from there. Rain lives 4 s under
	// a gravity of -1600, snow 16 s under -600 with a sideways drift of up
	// to 10 a second; both have a mass of 0.5 and are wind-affected. The
	// gravity factors are the same 60 fps reading as the splash's.
	void emitPrecipitation(float deltaSeconds)
	{
		if (precipitationKind == 0 || precipitationCount <= 0) return;
		precipitationAccumulator += deltaSeconds;
		int bursts = (int) (precipitationAccumulator / 0.1f);
		if (bursts <= 0) return;
		precipitationAccumulator -= (float) bursts * 0.1f;
		bursts = std::min(bursts, 3);   // a long stall does not dump a cloudburst
		const bool rain = precipitationKind == 1;
		for (int b = 0; b < bursts; b++) {
			for (int i = 0; i < precipitationCount; i++) {
				const float mass = 0.5f;
				Particle drop = {};
				drop.x = g_pickCamera.eyeX + randomUnit() * 400.0f - 200.0f;
				drop.z = g_pickCamera.eyeZ + randomUnit() * 400.0f - 200.0f;
				drop.y = 180.0f;
				if (rain) {
					drop.life = 4.0f;
					drop.gravityScale = 8.5f * mass;   // -1600
					drop.kind = 1;
				} else {
					drop.life = 16.0f;
					drop.gravityScale = 3.2f * mass;   // -600
					drop.vx = (randomUnit() * 20.0f - 10.0f) * mass;
					drop.vz = (randomUnit() * 20.0f - 10.0f) * mass;
					drop.kind = 2;
				}
				drop.r = drop.g = drop.b = 1.0f;
				drop.worldSize = 0.2f;
				drop.growth = 0.0f;
				drop.drag = 0.985f;
				drop.peakAlpha = 0.7f;
				drop.alphaBlend = true;
				drop.windAffect = true;
				drop.mass = mass;
				addParticle(drop);
			}
		}
	}

	// Trail behind an in-flight projectile: upstream's MissileActionRenderer
	// hangs a flame emitter and a smoke emitter off each shot, both enabled
	// by default (WeaponProjectile's createFlame_/createSmoke_ start true and
	// are only turned off by an explicit <nocreateflame>/<nocreatesmoke>).
	//
	// The rates below are upstream's, and they are much higher than what this
	// used to emit. MissileActionRenderer::simulate runs from the simulator,
	// which steps at a fixed 1/50s whatever the frame rate (Simulator.cpp's
	// StepSize), and every step it calls emitLinear(2, ...) on the flame -
	// 100 flame particles a second, unthrottled - while the smoke is gated by
	// its own 0.05s counter and emits 3 at a time, so 60 a second. This port
	// emitted one of each every 40ms: 25 a second, a quarter of the flame.
	//
	// Size mattered even more than rate. Upstream's flame is born between
	// half and all of flamestartsize and grows to between half and all of
	// flameendsize, whose defaults are 0.5 and *3.0* - each puff expands
	// sixfold over its life, so seventy-odd overlapping ones merge into a
	// continuous jet. This port used the start size and never grew it, so it
	// drew twenty-five small dots in a line.
	const float kTrailStepSeconds = 1.0f / 50.0f;   // upstream's StepSize
	const float kSmokeIntervalSeconds = 0.05f;      // its smoke counter
	// A stall must not spend the whole particle budget catching up: eight
	// steps is 160ms of trail, well past anything a running frame misses.
	const int   kMaxTrailStepsPerFrame = 8;
	float trailStepAccumulator = 0.0f;
	float smokeAccumulator = 0.0f;
	int   trailStepsThisFrame = 0;
	int   smokeEmitsThisFrame = 0;

	// [vx, vy, vz] is the shot's world-space velocity. Upstream does not need
	// it: its emitter runs inside the fixed-rate simulation, so each step's
	// particles land where the shot actually was. This runs on the render
	// thread instead, so a frame owes several steps at once - and dropping
	// them all at the shot's current position would bunch the trail into
	// blobs. Walking back along the velocity puts each step's puff where the
	// shot was at that step, which is the same trail at any frame rate.
	void emitProjectileTrail(WeaponProjectile *weapon, float x, float y, float z,
							 float vx, float vy, float vz)
	{
		if (!weapon) return;

		if (weapon->getCreateFlame() && trailStepsThisFrame > 0) {
			// Upstream's own ranges, from the emitter it builds in
			// MissileActionRenderer::simulate.
			const float startSize = std::max(weapon->getFlameStartSize(), 0.05f);
			const float endSize = std::max(weapon->getFlameEndSize(), startSize);
			const float life = std::max(weapon->getFlameLife(), 0.1f);
			Vector &c1 = weapon->getFlameStartColor1();
			Vector &c2 = weapon->getFlameStartColor2();

			for (int step = 0; step < trailStepsThisFrame; step++) {
				const float back = (float) step * kTrailStepSeconds;
				for (int n = 0; n < 2; n++) {   // upstream's emitLinear(2, ...)
					// Upstream randomises the position inside a 0.5-unit box
					// about the shot, which is what gives the jet width.
					Particle flame = {};
					flame.x = x - vx * back + randomSigned() * 0.25f;
					flame.y = y - vy * back + randomSigned() * 0.25f;
					flame.z = z - vz * back + randomSigned() * 0.25f;
					// Upstream's velocity range, mapped out of its Z-up world,
					// and multiplied by the particle's own mass the way its
					// integrator does (position += velocity * mass * time).
					const float mass = 0.5f + randomUnit() * 0.5f;
					flame.vx = randomSigned() * 0.05f * mass;
					flame.vy = (0.3f + randomUnit() * 0.6f) * mass;
					flame.vz = randomSigned() * 0.1f * mass;
					const float mix = randomUnit();
					flame.r = c1[0] + (c2[0] - c1[0]) * mix;
					flame.g = c1[1] + (c2[1] - c1[1]) * mix;
					flame.b = c1[2] + (c2[2] - c1[2]) * mix;
					// Half to full, as upstream's start/end size pairs read.
					const float born = startSize * (0.5f + randomUnit() * 0.5f);
					const float dies = endSize * (0.5f + randomUnit() * 0.5f);
					flame.worldSize = born;
					flame.growth = std::max(dies / born - 1.0f, 0.0f);
					flame.life = life * (0.5f + randomUnit() * 0.5f);
					// Upstream's gravity is applied as `gravity * time * time`,
					// which at a 1/50s step is four ten-thousandths of it - so
					// a flame effectively coasts on the velocity it was born
					// with. Its friction (0.01-0.02) is just as slight.
					flame.gravityScale = 0.0f;
					flame.drag = 0.98f;
					flame.peakAlpha = 0.9f + randomUnit() * 0.1f;
					flame.endAlpha = randomUnit() * 0.1f;
					// Both of MissileActionRenderer's emitters are wind-affected.
					flame.windAffect = true;
					flame.mass = mass;
					// The weapon's <flametexture> set, animated if it says so.
					setSprite(flame, weapon->getFlameTexture(), weapon->getAnimateFlameTexture());
					addParticle(flame);
				}
			}
		}

		if (weapon->getCreateSmoke() && smokeEmitsThisFrame > 0) {
			const float startSize = std::max(weapon->getSmokeStartSize(), 0.05f);
			const float endSize = std::max(weapon->getSmokeEndSize(), startSize);
			const float life = std::max(weapon->getSmokeLife(), 0.1f);

			for (int emit = 0; emit < smokeEmitsThisFrame; emit++) {
				const float back = (float) emit * kSmokeIntervalSeconds;
				for (int n = 0; n < 3; n++) {   // upstream's emitLinear(3, ...)
					Particle smoke = {};
					// Upstream drops the smoke a fifth of a second's travel
					// behind the shot, so it leaves the tail rather than the nose.
					smoke.x = x - vx * (back + 0.2f) + randomSigned() * 0.25f;
					smoke.y = y - vy * (back + 0.2f) + randomSigned() * 0.25f;
					smoke.z = z - vz * (back + 0.2f) + randomSigned() * 0.25f;
					// And gives it 28-40% of the shot's speed *backwards*,
					// which is what makes a rocket's smoke stream away from it.
					const float mass = 0.2f + randomUnit() * 0.3f;
					const float kick = -(0.28f + randomUnit() * 0.12f) * mass;
					smoke.vx = vx * kick;
					smoke.vy = vy * kick;
					smoke.vz = vz * kick;
					// Upstream's smoke: 0.7 grey, alpha-blended, on the
					// weapon's <smoketexture> set.
					smoke.r = 0.7f; smoke.g = 0.7f; smoke.b = 0.7f;
					const float born = startSize * (0.5f + randomUnit() * 0.5f);
					const float dies = endSize * (0.5f + randomUnit() * 0.5f);
					smoke.worldSize = born;
					smoke.growth = std::max(dies / born - 1.0f, 0.0f);
					smoke.life = life * (0.5f + randomUnit() * 0.5f);
					smoke.gravityScale = 0.0f;
					smoke.drag = 0.9f;
					smoke.peakAlpha = 0.3f;
					smoke.endAlpha = randomUnit() * 0.1f;
					smoke.alphaBlend = true;
					// Upstream's +100 gravity on its time-squared integrator:
					// 100/60 a second squared at 60 fps, times the mass, up.
					smoke.gravityScale = -(100.0f / 60.0f * mass) / 3.15f;
					smoke.windAffect = true;
					smoke.mass = mass;
					setSprite(smoke, weapon->getSmokeTexture(), weapon->getAnimateSmokeTexture());
					addParticle(smoke);
				}
			}
		}
	}

	void spawnEffects()
	{
		std::vector<ScorchDroidEffects::EffectEvent> events = ScorchDroidEffects::drain();
		if (!events.empty() && effectLogsLeft > 0) {
			effectLogsLeft--;
			// A count per type, not just the first one's: a batch mixes
			// explosion, smoke, debris and the rest, and "first type" says
			// nothing about whether the others arrived. Chasing a missing
			// effect without this is guesswork.
			int histogram[16] = { 0 };
			for (const ScorchDroidEffects::EffectEvent &e : events) {
				if (e.type >= 0 && e.type < 16) histogram[e.type]++;
			}
			std::ostringstream types;
			for (int t = 0; t < 16; t++) {
				if (histogram[t]) types << " t" << t << "=" << histogram[t];
			}
			LOGI("Effects by type:%s", types.str().c_str());
			LOGI("Effects: %zu event(s), first type=%d at (%.1f, %.1f, %.1f) size %.1f",
				 events.size(), (int) events[0].type,
				 events[0].x, events[0].y, events[0].z, events[0].size);
		}
		for (size_t i = 0; i < events.size(); i++) {
			const ScorchDroidEffects::EffectEvent &event = events[i];
			// engine (x, y, height) -> render (x, height, z)
			const float x = event.x, y = event.z, z = worldZFromEngineY(event.y);
			const float endX = event.endX, endY = event.endZ,
						endZ = worldZFromEngineY(event.endY);

			switch (event.type) {
			case ScorchDroidEffects::eExplosion: {
				// X4b: a splashing weapon that went off under the water.
				// Water::explosion: the spray, its width the blast size
				// less two, and the splash sound. The flag rides in value
				// (patch 0019); the under-water test is this side's, as it
				// is upstream's.
				if (event.value > 0.5f && waterVisible && event.z < waterHeight) {
					spawnSplash(x, y, z, event.size - 2.0f);
					ScorchDroidAudio::pushSoundEvent(S3D::getModFile("data/wav/misc/splash.wav"));
					static bool logged = false;
					if (!logged) {
						logged = true;
						LOGI("Splash: first spray, blast size %.1f at %.1f under water %.1f",
							 event.size, event.z, waterHeight);
					}
				}
				// A bright core plus an outward burst. Count scales with the
				// blast so a small weapon doesn't look like a big one.
				// Upstream's client body, with the weapon's own texture set.
				// A normal explosion (ParticleEmitter::emitExplosion): 4 per
				// unit of size, thrown in random directions at 2.5 x size,
				// the weapon's <explosioncolour>, alpha 0.8-0.9 fading to
				// 0-0.1, 0.2-0.5 units growing to up to 2 x size + 2, over
				// the weapon's life range, mass 0.2-0.5 (upstream integrates
				// position += velocity * mass), additive if <luminance>.
				// A ring (ExplosionRing, ExplosionRingDirectional): 400
				// particles around an axis at 4 x size, blue-white, 0.2-0.5
				// growing to 1.5-3, mass 0.5, no friction.
				const float size = std::max(event.size, 0.5f);
				const float life1 = event.life1 > 0.0f ? event.life1 : 0.5f;
				const float life2 = event.life2 > life1 ? event.life2 : life1 + 0.5f;
				if (event.explosionType == 0) {
					const int count = (int) event.size * 4;
					for (int p = 0; p < count; p++) {
						const float rotXY = randomUnit() * 6.2831853f;
						const float rotXZ = randomUnit() * 6.2831853f;
						const float mass = 0.2f + randomUnit() * 0.3f;
						Particle particle = {};
						particle.x = x; particle.y = y; particle.z = z;
						// Engine (x, y, up) -> render (x, up, -y).
						particle.vx = sinf(rotXY) * cosf(rotXZ) * size * 2.5f * mass;
						particle.vz = -cosf(rotXY) * cosf(rotXZ) * size * 2.5f * mass;
						particle.vy = sinf(rotXZ) * size * 2.5f * mass;
						particle.r = event.r; particle.g = event.g; particle.b = event.b;
						const float born = 0.2f + randomUnit() * 0.3f;
						const float dies = randomUnit() * (size * 2.0f + 2.0f);
						particle.worldSize = born;
						particle.growth = std::max(dies / born - 1.0f, 0.0f);
						particle.life = life1 + randomUnit() * (life2 - life1);
						particle.peakAlpha = 0.8f + randomUnit() * 0.1f;
						particle.endAlpha = randomUnit() * 0.1f;
						particle.drag = 0.985f;
						particle.gravityScale = 0.0f;
						particle.alphaBlend = !event.additive;
						particle.windAffect = event.windAffected;
						particle.mass = mass;
						setSprite(particle, event.texture, event.animate);
						addParticle(particle);
					}
				} else {
					float ax = 0.0f, ay = 1.0f, az = 0.0f;
					if (event.explosionType == 2) {
						ax = event.endX; ay = event.endZ; az = -event.endY;
						const float len = sqrtf(ax * ax + ay * ay + az * az);
						if (len > 0.0001f) { ax /= len; ay /= len; az /= len; } else { ax = 0; ay = 1; az = 0; }
					}
					// A perpendicular to the axis, as emitExplosionRing picks it.
					float ox = 0.0f, oy = 1.0f, oz = 0.0f;
					if (fabsf(ay) > 0.7f) { ox = 1.0f; oy = 0.0f; }
					float px = ay * oz - az * oy, py = az * ox - ax * oz, pz = ax * oy - ay * ox;
					const float plen = sqrtf(px * px + py * py + pz * pz);
					if (plen > 0.0001f) { px /= plen; py /= plen; pz /= plen; }
					for (int p = 0; p < 400; p++) {
						const float ang = randomUnit() * 6.2831853f;
						// p rotated about the axis by ang (Rodrigues).
						const float c = cosf(ang), sn = sinf(ang);
						const float dotAP = ax * px + ay * py + az * pz;
						const float rx2 = px * c + (ay * pz - az * py) * sn + ax * dotAP * (1.0f - c);
						const float ry2 = py * c + (az * px - ax * pz) * sn + ay * dotAP * (1.0f - c);
						const float rz2 = pz * c + (ax * py - ay * px) * sn + az * dotAP * (1.0f - c);
						const float speed = size * 4.0f * 0.5f;   // mass 0.5
						Particle particle = {};
						particle.x = x; particle.y = y; particle.z = z;
						particle.vx = rx2 * speed; particle.vy = ry2 * speed; particle.vz = rz2 * speed;
						const float mix = randomUnit();
						particle.r = 0.0f + 0.2f * mix; particle.g = 0.0f + 0.2f * mix; particle.b = 0.8f + 0.1f * mix;
						const float born = 0.2f + randomUnit() * 0.3f;
						const float dies = 1.5f + randomUnit() * 1.5f;
						particle.worldSize = born;
						particle.growth = std::max(dies / born - 1.0f, 0.0f);
						particle.life = life1 + randomUnit() * (life2 - life1);
						particle.peakAlpha = 0.9f + randomUnit() * 0.1f;
						particle.endAlpha = randomUnit() * 0.1f;
						particle.drag = 1.0f;
						particle.gravityScale = 0.0f;
						particle.alphaBlend = !event.additive;
						particle.windAffect = event.windAffected;
						particle.mass = 0.5f;
						setSprite(particle, event.texture, event.animate);
						addParticle(particle);
					}
				}
				break;
			}
			case ScorchDroidEffects::eNapalm: {
				// The per-tick flame upstream raises at one burning point:
				// the same flames particle, for a second or so.
				Particle flame = {};
				flame.x = x + randomSigned() * 0.5f;
				flame.y = y + event.size * 2.0f;
				flame.z = z + randomSigned() * 0.5f;
				flame.r = flame.g = flame.b = 1.0f;
				flame.worldSize = event.size;
				flame.life = 0.9f + randomUnit() * 0.6f;
				flame.peakAlpha = 0.6f + randomUnit() * 0.3f;
				flame.endAlpha = 0.0f;
				flame.drag = 1.0f;
				flame.gravityScale = 0.0f;
				flame.alphaBlend = !event.additive;
				setSprite(flame, event.texture, true);
				flame.framesPerSecond = 6.0f;
				flame.frameOffset = (int) (randomUnit() * (float) std::max(flame.frames, 1));
				flame.orient = 0;
				addParticle(flame);
				break;
			}
			case ScorchDroidEffects::eNapalmFire: {
				// Napalm's emitter: white, alpha 0.9-0.6 fading to 0-0.1, a
				// flat 1.5 units, for the whole napalm time, three units
				// above the ground (NapalmRenderer: ground + size * 2), and
				// the weapon's flames set stepped by time - NapalmRenderer
				// advances a tenth of a frame per simulate, six frames a
				// second at 60 fps - from a random start frame.
				Particle fire = {};
				fire.x = x;
				fire.y = y + event.size * 2.0f;
				fire.z = z;
				fire.r = fire.g = fire.b = 1.0f;
				fire.worldSize = event.size;
				fire.growth = 0.0f;
				fire.gravityScale = 0.0f;
				fire.drag = 1.0f;
				fire.peakAlpha = 0.6f + randomUnit() * 0.3f;
				fire.endAlpha = randomUnit() * 0.1f;
				fire.life = std::max(event.value, 0.5f);
				fire.alphaBlend = !event.additive;
				setSprite(fire, event.texture, true);
				fire.framesPerSecond = 6.0f;
				fire.frameOffset = (int) (randomUnit() * (float) std::max(fire.frames, 1));
				fire.orient = 0;
				addParticle(fire);
				break;
			}
			case ScorchDroidEffects::eSkyFlash: {
				// Upstream flashes the whole sky white for a moment
				// (Sky::flashSky, used by the big warheads). Nothing is
				// added to the scene - the sky pass reads this and lifts
				// its own colour, so the flash lights the whole view the
				// way a nuke should rather than appearing as an object.
				skyFlashRemaining = kSkyFlashSeconds;
				break;
			}
			case ScorchDroidEffects::eWallHit: {
				const int side = (int) event.value;
				if (side >= 0 && side < 4) wallFade[side] = 1.0f;
				break;
			}
			case ScorchDroidEffects::eDebris: {
				// One rock per event; the pushing site decides how many,
				// using upstream's own count.
				if (debrisChunks.size() >= kMaxDebris) break;
				const float size = std::max(event.size, 1.0f);
				DebrisChunk chunk = {};
				chunk.x = x; chunk.y = y; chunk.z = z;
				float dx = randomSigned(), dy = randomSigned(), dz = randomSigned();
				const float len = sqrtf(dx * dx + dy * dy + dz * dz);
				if (len < 0.001f) break;
				dx /= len; dy /= len; dz /= len;
				const float speed = size * (1.2f + randomUnit() * 1.6f);
				chunk.vx = dx * speed;
				chunk.vy = fabsf(dy) * speed + size * 1.2f;   // always thrown up
				chunk.vz = dz * speed;
				// A second random direction for the tumble, so the spin is
				// unrelated to the direction of travel.
				float ax = randomSigned(), ay = randomSigned(), az = randomSigned();
				const float alen = sqrtf(ax * ax + ay * ay + az * az);
				if (alen < 0.001f) { ax = 0.0f; ay = 1.0f; az = 0.0f; }
				else { ax /= alen; ay /= alen; az /= alen; }
				chunk.axisX = ax; chunk.axisY = ay; chunk.axisZ = az;
				chunk.angle = randomUnit() * 6.2831853f;
				chunk.spin = (randomUnit() * 8.0f + 4.0f) * (randomUnit() < 0.5f ? -1.0f : 1.0f);
				// A fraction of a normalised model rather than an absolute
				// size: the rock meshes are ~32 units across natively, so
				// this rides on uploadModel's own 2.2-units normalisation
				// (applied at the draw) and a chunk ends up a believable
				// size next to a tank whatever the source mesh measures.
				chunk.scale = 0.30f + randomUnit() * 0.45f;
				chunk.mesh = (randomUnit() < 0.5f) ? 0 : 1;
				chunk.life = 1.6f + randomUnit() * 1.2f;
				debrisChunks.push_back(chunk);
				break;
			}
			case ScorchDroidEffects::eDamage: {
				// Upstream's floating damage number - red, over the target,
				// already scattered by the pushing site so several from one
				// blast do not stack.
				if (floatingLabels.size() < kMaxFloatingLabels) {
					FloatingLabel label;
					label.x = x; label.y = y; label.z = z;
					char text[32];
					snprintf(text, sizeof(text), "%.0f", event.value);
					label.text = text;
					label.life = 1.6f;
					label.r = 0.85f; label.g = 0.1f; label.b = 0.1f;
					floatingLabels.push_back(label);
				}
				break;
			}
			case ScorchDroidEffects::eTalk: {
				// TalkRenderer: the talk.bmp bubble, 2 units, 8-8.5 s, fading
				// to nothing, alpha-blended, over the tank.
				Particle bubble = {};
				bubble.x = x; bubble.y = y + 2.0f; bubble.z = z;
				bubble.r = bubble.g = bubble.b = 1.0f;
				bubble.worldSize = 2.0f;
				bubble.life = 8.0f + randomUnit() * 0.5f;
				bubble.peakAlpha = 1.0f; bubble.endAlpha = 0.0f;
				bubble.drag = 1.0f; bubble.gravityScale = 0.0f;
				bubble.alphaBlend = true;
				setSprite(bubble, "talk", false);
				bubble.orient = 0;
				addParticle(bubble);
				break;
			}
			case ScorchDroidEffects::eMushroom: {
				// ExplosionNukeRenderer: the cloud's base is the blast lowered
				// by its size and clamped to the ground; the puffs come from
				// stepMushroomEmitters over the next two seconds.
				MushroomEmitter cloud;
				cloud.size = std::max(event.size, 1.0f);
				cloud.x = x; cloud.z = z;
				cloud.y = std::max(y - cloud.size, sampledGroundHeight(x, z));
				cloud.layer = 0; cloud.frames = 0;
				if (!spriteAtlas.valid()) spriteAtlas = ScorchDroidParticleTextures::load();
				if (const ScorchDroidParticleTextures::Set *set = spriteAtlas.find(event.texture)) {
					cloud.layer = set->firstLayer; cloud.frames = set->count;
				}
				mushroomEmitters.push_back(cloud);
				break;
			}
			case ScorchDroidEffects::eSmoke:
				spawnSmokePuff(x, y, z);
				break;
			case ScorchDroidEffects::eTeleport: {
				// TeleportRenderer: one particle of the animated "trans" set,
				// 0.7-1 units, 2-2.5 s, white fading to nothing, alpha-blended.
				Particle warp = {};
				warp.x = x; warp.y = y; warp.z = z;
				warp.r = warp.g = warp.b = 1.0f;
				warp.worldSize = 0.7f + randomUnit() * 0.3f;
				warp.life = 2.0f + randomUnit() * 0.5f;
				warp.peakAlpha = 1.0f; warp.endAlpha = 0.0f;
				warp.drag = 1.0f; warp.gravityScale = 0.0f;
				warp.alphaBlend = true;
				setSprite(warp, "trans", true);
				warp.orient = 0;
				addParticle(warp);
				break;
			}
			case ScorchDroidEffects::eShieldHit: {
				// Logged once per landscape. This effect is the hardest one
				// here to see on purpose - upstream exempts your own shots
				// from your own shield (PhysicsParticleObject::
				// shieldCollision returns false when the shot's owner is the
				// target), so it needs someone else to hit a shield you have
				// up - and "did that ever actually fire?" is otherwise
				// unanswerable from a screenshot.
				if (!loggedShieldHit) {
					loggedShieldHit = true;
					LOGI("Shield hit effect raised at (%.1f, %.1f, %.1f) radius %.1f",
						 event.x, event.y, event.z, event.size);
				}
				// A ring of sparks on the shield surface, facing outward.
				const float radius = std::max(event.size, 1.0f);
				for (int p = 0; p < 18; p++) {
					float dx = randomSigned(), dy = randomSigned(), dz = randomSigned();
					float len = sqrtf(dx * dx + dy * dy + dz * dz);
					if (len < 0.001f) { p--; continue; }
					dx /= len; dy /= len; dz /= len;

					Particle spark = {};
					spark.x = x + dx * radius * 0.6f;
					spark.y = y + dy * radius * 0.6f;
					spark.z = z + dz * radius * 0.6f;
					spark.vx = dx * radius; spark.vy = dy * radius; spark.vz = dz * radius;
					spark.r = event.r; spark.g = event.g; spark.b = event.b;
					spark.worldSize = radius * 0.3f;
					spark.life = 0.35f + randomUnit() * 0.2f;
					spark.drag = 0.1f;
					addParticle(spark);
				}
				break;
			}
			case ScorchDroidEffects::eLaser:
			case ScorchDroidEffects::eLightning: {
				if (beams.size() >= kMaxBeams) break;
				Beam beam = {};
				beam.x1 = x; beam.y1 = y; beam.z1 = z;
				beam.x2 = endX; beam.y2 = endY; beam.z2 = endZ;
				beam.r = event.r; beam.g = event.g; beam.b = event.b;
				// Upstream's laser lives for its weapon's totalTime and its
				// lightning for the bolt's; neither is carried on the event,
				// so both use a short constant - long enough to read, short
				// enough not to linger over the next shot.
				beam.life = (event.type == ScorchDroidEffects::eLaser) ? 0.5f : 0.6f;
				beams.push_back(beam);
				break;
			}
			}
		}
	}

	void updateEffects(ScorchedContext &ctx, float deltaSeconds)
	{
		const float kGravity = 9.0f;  // not the sim's gravity: this is smoke, not ballistics

		// X1: the wind, as upstream's ParticleEngine::simulate applies it
		// to every wind-affected particle:
		//   velocity += windDir * windSpeed * 80 * time * time
		// That is a frame-rate-dependent acceleration - twice as strong at
		// 30 fps as at 60 - and copying it verbatim would make the same
		// round drift differently on different phones. So it is taken as
		// what it evaluates to at upstream's usual 60 fps, a fixed
		// acceleration of dir * speed * 80 / 60 per second squared, and
		// applied per second. Engine (x, y) -> render (x, 0, -y).
		float windAx = 0.0f, windAz = 0.0f;
		{
			Wind &wind = ctx.getSimulator().getWind();
			const float speed = wind.getWindSpeed().asFloat();
			if (speed > 0.0f) {
				FixedVector &dir = wind.getWindDirection();
				const float strength = speed * 80.0f / 60.0f;
				windAx = dir[0].asFloat() * strength;
				windAz = -dir[1].asFloat() * strength;
			}
		}

		stepMushroomEmitters(deltaSeconds);

		size_t live = 0;
		for (size_t i = 0; i < particles.size(); i++) {
			Particle &particle = particles[i];
			particle.age += deltaSeconds;
			if (particle.age >= particle.life) continue;
			// Rain and snow end at the ground plane, as upstream's
			// renderers end them (position z < 0 -> life 0).
			if ((particle.kind == 1 || particle.kind == 2) && particle.y < 0.0f) continue;
			if (particle.kind == 3) {
				// ExplosionNukeRendererEntry::simulate: the puff's position
				// is the path, scaled by the blast, not its velocity.
				const int step = std::min((int) (particle.age / (16.0f / 3.0f) * (float) kMushroomSteps), kMushroomSteps - 1);
				float pathW, pathH;
				mushroomPath(step, pathW, pathH);
				const float h = pathH * (10.0f + particle.mushroomSize) / 30.0f;
				const float w = pathW + particle.mushroomSize / 2.0f;
				particle.x = particle.startX + particle.spreadX * w;
				particle.z = particle.startZ + particle.spreadZ * w;
				particle.y = particle.startY + h;
				particles[live++] = particle;
				continue;
			}

			if (particle.windAffect) {
				particle.vx += windAx * particle.mass * deltaSeconds;
				particle.vz += windAz * particle.mass * deltaSeconds;
			}
			particle.vy -= kGravity * deltaSeconds * 0.35f * particle.gravityScale;
			const float retain = powf(particle.drag, deltaSeconds);
			particle.vx *= retain; particle.vy *= retain; particle.vz *= retain;
			particle.x += particle.vx * deltaSeconds;
			particle.y += particle.vy * deltaSeconds;
			particle.z += particle.vz * deltaSeconds;

			particles[live++] = particle;
		}
		particles.resize(live);

		for (int i = 0; i < 4; i++) {
			if (wallFade[i] > 0.0f) wallFade[i] = std::max(0.0f, wallFade[i] - deltaSeconds);
		}

		live = 0;
		for (size_t i = 0; i < debrisChunks.size(); i++) {
			DebrisChunk &chunk = debrisChunks[i];
			chunk.age += deltaSeconds;
			if (chunk.age >= chunk.life) continue;
			// Real ballistics rather than the particles' softened fall -
			// these are rocks, and they should arc and land like rocks.
			chunk.vy -= kGravity * deltaSeconds;
			chunk.x += chunk.vx * deltaSeconds;
			chunk.y += chunk.vy * deltaSeconds;
			chunk.z += chunk.vz * deltaSeconds;
			chunk.angle += chunk.spin * deltaSeconds;
			debrisChunks[live++] = chunk;
		}
		debrisChunks.resize(live);

		live = 0;
		for (size_t i = 0; i < floatingLabels.size(); i++) {
			floatingLabels[i].age += deltaSeconds;
			if (floatingLabels[i].age >= floatingLabels[i].life) continue;
			// Drift upward as they fade, so a number reads as leaving
			// rather than simply vanishing.
			floatingLabels[i].y += deltaSeconds * 2.5f;
			floatingLabels[live++] = floatingLabels[i];
		}
		floatingLabels.resize(live);

		live = 0;
		for (size_t i = 0; i < beams.size(); i++) {
			beams[i].age += deltaSeconds;
			if (beams[i].age >= beams[i].life) continue;
			beams[live++] = beams[i];
		}
		beams.resize(live);
	}

	// Particles are drawn as point sprites, so their size has to be given in
	// pixels - this is the standard perspective conversion, using the same
	// vertical field of view the projection matrix was built with.
	float worldSizeToPixels(float worldSize, float distance, float fovYRadians)
	{
		if (distance < 0.01f) distance = 0.01f;
		const float halfScreen = (float) surfaceHeight * 0.5f;
		return worldSize * halfScreen / (distance * tanf(fovYRadians * 0.5f));
	}

	// W3: [pixelScale] scales the point sizes for a target that is not the
	// screen - worldSizeToPixels measures against surfaceHeight, so a
	// half-resolution reflection buffer needs half-size sprites or every
	// puff of smoke reflects twice as large as it is.
	//
	// [clipBelowY] drops anything under the water plane, which is upstream's
	// rule for what goes in the reflection (RenderTargets::draw skips targets
	// below the waterline). Upstream does not apply it to its own particles -
	// its reflection pass just draws the lot - but a mirrored particle from
	// below the surface surfaces *above* it in the reflection, so the rule it
	// already uses for targets is the right one here too.
	// The fixed-function fog upstream applies to particles and beams:
	// GL_EXP2 by view depth, in the landscape's fog colour, off with the
	// Distance fog setting.
	void setFixedFunctionFog(GLint colorLoc, GLint densityLoc)
	{
		float fog[3];
		currentFogColor(fog);
		glUniform3f(colorLoc, fog[0], fog[1], fog[2]);
		glUniform1f(densityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);
	}

	void drawEffects(const Mat4 &viewProjection, const Mat4 &view, float eyeX, float eyeY, float eyeZ,
					 float fovYRadians, float pixelScale = 1.0f,
					 float clipBelowY = -1.0e9f)
	{
		if (particles.empty() && beams.empty()) return;

		// Additive, depth-tested but not depth-writing: effects light up
		// whatever is behind them and never occlude each other.
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glDepthMask(GL_FALSE);

		if (!particles.empty()) {
			ensureSpriteTexture();
			// Upstream's particles are camera-facing quads (GLCameraFrustum::
			// drawBilboard): corners at position +- right*size +- up*size,
			// the texture in one of four orientations, colour and alpha as
			// GL_MODULATE. Two passes, because blend mode is per draw and
			// not per vertex: additive first, then the alpha-blended smoke
			// over the top, which lets smoke obscure a flame in front of it.
			const float rx = view.m[0], ry = view.m[4], rz = view.m[8];
			const float ux = view.m[1], uy = view.m[5], uz = view.m[9];
			// Texture corners for the four orientations, in the order
			// (+r+u), (-r+u), (-r-u), (+r-u) as upstream lays them out.
			static const float kUv[4][4][2] = {
				{ { 1, 1 }, { 0, 1 }, { 0, 0 }, { 1, 0 } },
				{ { 0, 1 }, { 0, 0 }, { 1, 0 }, { 1, 1 } },
				{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } },
				{ { 1, 0 }, { 1, 1 }, { 0, 1 }, { 0, 0 } },
			};
			std::vector<float> additive, blended;
			additive.reserve(particles.size() * 60);
			for (size_t i = 0; i < particles.size(); i++) {
				const Particle &particle = particles[i];
				if (particle.y < clipBelowY) continue;
				if (particle.kind == 1) continue;   // rain is drawn as streaks below
				const float percent = std::min(std::max(particle.age / particle.life, 0.0f), 1.0f);
				const float remaining = 1.0f - percent;

				// Half-extent in world units, growing over the life.
				const float half = particle.worldSize * (1.0f + percent * particle.growth);

				float alpha = particle.linearAlpha
					? particle.peakAlpha + (particle.endAlpha - particle.peakAlpha) * percent
					: particle.peakAlpha * remaining * remaining;
				if (particle.kind == 2) {
					// Snow fades by distance alone, upstream's 0.7 at the
					// camera to nothing 200 units away.
					const float dx = particle.x - eyeX, dy = particle.y - eyeY, dz = particle.z - eyeZ;
					alpha = std::max(0.0f, 0.7f * (1.0f - (dx * dx + dy * dy + dz * dz) / 40000.0f));
				}
				if (alpha <= 0.0f) continue;

				int layer = particle.layer;
				if (particle.frames > 1) {
					const int frame = (particle.framesPerSecond > 0.0f)
						? ((int) (particle.age * particle.framesPerSecond) + particle.frameOffset) % particle.frames
						: std::min((int) ((float) (particle.frames - 1) * percent), particle.frames - 1);
					layer += frame;
				}

				const float cx[4] = {
					particle.x + rx * half + ux * half, particle.x - rx * half + ux * half,
					particle.x - rx * half - ux * half, particle.x + rx * half - ux * half };
				const float cy[4] = {
					particle.y + ry * half + uy * half, particle.y - ry * half + uy * half,
					particle.y - ry * half - uy * half, particle.y + ry * half - uy * half };
				const float cz[4] = {
					particle.z + rz * half + uz * half, particle.z - rz * half + uz * half,
					particle.z - rz * half - uz * half, particle.z + rz * half - uz * half };
				const float (*uv)[2] = kUv[particle.orient & 3];
				std::vector<float> &into = particle.alphaBlend ? blended : additive;
				static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
				for (int t = 0; t < 6; t++) {
					const int c = tri[t];
					into.push_back(cx[c]); into.push_back(cy[c]); into.push_back(cz[c]);
					into.push_back(uv[c][0]); into.push_back(uv[c][1]);
					into.push_back((float) layer);
					into.push_back(particle.r); into.push_back(particle.g); into.push_back(particle.b);
					into.push_back(alpha);
				}
			}

			glUseProgram(particleProgram);
			setFixedFunctionFog(particleFogColorLoc, particleFogDensityLoc);
			glUniformMatrix4fv(particleMvpLoc, 1, GL_FALSE, viewProjection.m);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D_ARRAY, spriteArrayTexture);
			glUniform1i(particleSpritesLoc, 0);
			glBindVertexArray(particleVao);
			glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
			glDisable(GL_CULL_FACE);

			if (!additive.empty()) {
				glBufferData(GL_ARRAY_BUFFER, additive.size() * sizeof(float),
							 additive.data(), GL_DYNAMIC_DRAW);
				frameDrawCalls++;
				glDrawArrays(GL_TRIANGLES, 0, (GLsizei) (additive.size() / 10));
			}
			if (!blended.empty()) {
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glBufferData(GL_ARRAY_BUFFER, blended.size() * sizeof(float),
							 blended.data(), GL_DYNAMIC_DRAW);
				frameDrawCalls++;
				glDrawArrays(GL_TRIANGLES, 0, (GLsizei) (blended.size() / 10));
			}
			glEnable(GL_CULL_FACE);
			glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // the beams below expect additive
		}

		{
			std::vector<float> data;
			data.reserve(beams.size() * 12);
			// X4c: rain, as upstream's ParticleRendererRain draws it - a
			// streak 0.1 wide and (1 - |camera pitch| + 0.1) tall, so it
			// lengthens as the view levels out, at 0.7 alpha fading to
			// nothing 200 units away. Additive lines carry no alpha, so the
			// fade dims the colour instead.
			{
				float streak = 0.0f;
				bool streakKnown = false;
				for (size_t i = 0; i < particles.size(); i++) {
					const Particle &drop = particles[i];
					if (drop.kind != 1 || drop.y < clipBelowY) continue;
					if (!streakKnown) {
						// The camera's forward is unit length already.
						streak = 1.0f - fabsf(g_pickCamera.fwdY) + 0.1f;
						streakKnown = true;
					}
					const float dx = drop.x - eyeX, dy = drop.y - eyeY, dz = drop.z - eyeZ;
					const float d2 = dx * dx + dy * dy + dz * dz;
					const float fade = std::max(0.0f, 0.7f * (1.0f - d2 / 40000.0f));
					if (fade <= 0.0f) continue;
					data.push_back(drop.x); data.push_back(drop.y); data.push_back(drop.z);
					data.push_back(fade); data.push_back(fade); data.push_back(fade);
					data.push_back(drop.x); data.push_back(drop.y - streak); data.push_back(drop.z);
					data.push_back(fade); data.push_back(fade); data.push_back(fade);
				}
			}
			for (size_t i = 0; i < beams.size(); i++) {
				const Beam &beam = beams[i];
				// Same waterline rule as the particles above: a beam wholly
				// under the surface has no reflection.
				if (beam.y1 < clipBelowY && beam.y2 < clipBelowY) continue;
				// The beam shader carries no alpha, so fade by dimming the
				// colour - which is the same thing under additive blending.
				const float fade = 1.0f - beam.age / beam.life;
				const float r = beam.r * fade, g = beam.g * fade, b = beam.b * fade;
				data.push_back(beam.x1); data.push_back(beam.y1); data.push_back(beam.z1);
				data.push_back(r); data.push_back(g); data.push_back(b);
				data.push_back(beam.x2); data.push_back(beam.y2); data.push_back(beam.z2);
				data.push_back(r); data.push_back(g); data.push_back(b);
			}

			if (!data.empty()) {
				glUseProgram(sightProgram);
		setFixedFunctionFog(sightFogColorLoc, sightFogDensityLoc);
				glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, viewProjection.m);
				glBindVertexArray(beamVao);
				glBindBuffer(GL_ARRAY_BUFFER, beamVbo);
				glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_DYNAMIC_DRAW);
				glLineWidth(3.0f);
				// Counted from the data rather than from beams.size(): the
				// waterline cull above can drop some, and the old count would
				// then have read past what was uploaded.
				frameDrawCalls++; glDrawArrays(GL_LINES, 0, (GLsizei) (data.size() / 6));
			}
		}

		glBindVertexArray(0);
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
	}

	// Ground height at an arbitrary world (x, z), clamped to the map -
	// outside the landscape the edge height is returned rather than
	// nothing, so the camera clearance check below still has a sane floor
	// when the view swings off the map.
	float terrainHeightAt(ScorchedContext &ctx, float worldX, float worldZ)
	{
		HeightMap &heightMap = ctx.getLandscapeMaps().getGroundMaps().getHeightMap();
		const int w = heightMap.getMapWidth();
		const int h = heightMap.getMapHeight();
		if (w <= 0 || h <= 0) return 0.0f;

		int sx = std::min(std::max((int) worldX, 0), w - 1);
		int sy = std::min(std::max((int) engineYFromWorldZ(worldZ), 0), h - 1);
		return heightMap.getHeight(sx, sy).asFloat();
	}

	// Replaces the whole ground texture on the GPU from a CPU-side image.
	// 512*3 bytes per row is 4-byte aligned, so this needs no unpack-
	// alignment fiddling of its own (unlike the scorch patches below).
	void uploadGroundTexture(const LandscapeTextureBuilder::Texture &texture)
	{
		if (groundTexture == 0 || !texture.valid()) return;
		glBindTexture(GL_TEXTURE_2D, groundTexture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture.width, texture.height,
						GL_RGB, GL_UNSIGNED_BYTE, texture.rgb.data());
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	// M6 tank movement: paints where the tank may move onto the ground, the
	// way upstream does when a position-selecting weapon becomes current
	// (TankWeaponSwitcher::switchWeapon -> MovementMap::movementTexture).
	//
	// The tint is never written into groundTextureData: that copy has to
	// survive to be put back when the weapon is switched away, and it keeps
	// collecting scorch marks underneath in the meantime - exactly what
	// upstream's restoreLandscapeTexture() relies on too.
	void syncMovementOverlay()
	{
		if (!groundTextureBuilt || groundTexture == 0 || !groundTextureData.valid()) return;

		const unsigned int version = ScorchDroidMovement::version();
		if (version == paintedMovementVersion) return;
		paintedMovementVersion = version;

		int width = 0, height = 0;
		std::vector<unsigned char> mask;
		if (!ScorchDroidMovement::get(width, height, mask)) {
			if (movementOverlayPainted) {
				uploadGroundTexture(groundTextureData);
				movementOverlayPainted = false;
			}
			return;
		}

		LandscapeTextureBuilder::Texture tinted =
			LandscapeTextureBuilder::applyMovementMask(
				groundTextureData, mask.data(), width, height);
		if (!tinted.valid()) return;
		uploadGroundTexture(tinted);
		movementOverlayPainted = true;
	}

	// M6 scorch marks: the burnt patch a blast leaves on the ground.
	// Upstream paints these straight into the landscape texture
	// (DeformTextures::deformLandscape, client-only), and so do we - into
	// the CPU-side copy, so marks accumulate on top of each other across a
	// round, then re-upload only the affected rectangles.
	void applyScorchMarks(ScorchedContext &ctx)
	{
		if (!groundTextureBuilt || groundTexture == 0 || !groundTextureData.valid()) return;

		std::vector<ScorchDroidLandscape::ScorchEvent> events =
			ScorchDroidLandscape::drainScorchEvents();
		if (events.empty()) return;

		glBindTexture(GL_TEXTURE_2D, groundTexture);

		// The patch rows below are tightly packed, but GL defaults to
		// expecting each row padded to a 4-byte boundary. An RGB rectangle
		// only satisfies that when its width happens to be a multiple of 4,
		// so an arbitrary crater (33px wide = 99 bytes) gets read with every
		// row shifted a byte or two - which renders as diagonal rainbow
		// banding, since the shift also rotates the colour channels. The
		// full-texture upload never hit this only because 512*3 is divisible
		// by 4. Restored afterwards so nothing else inherits the change.
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		int painted = 0;
		for (size_t i = 0; i < events.size(); i++) {
			const ScorchDroidLandscape::ScorchEvent &event = events[i];
			LandscapeTextureBuilder::Rect rect = LandscapeTextureBuilder::applyScorch(
				ctx, groundTextureData, event.centreX, event.centreY, event.radius, event.texture);
			if (!rect.valid()) continue;

			// GLES3 has no GL_UNPACK_ROW_LENGTH-free way to upload a
			// sub-rectangle of a wider buffer, so copy the rows out
			// contiguously first. (GLES3 does have UNPACK_ROW_LENGTH, but
			// setting and restoring it per mark is no cheaper than this for
			// rectangles this small, and this way the pixel-store state is
			// left exactly as the rest of the renderer expects it.)
			std::vector<unsigned char> patch(size_t(rect.width) * rect.height * 3);
			for (int row = 0; row < rect.height; row++) {
				const unsigned char *src = &groundTextureData.rgb[
					(size_t(rect.y + row) * groundTextureData.width + rect.x) * 3];
				memcpy(&patch[size_t(row) * rect.width * 3], src, size_t(rect.width) * 3);
			}
			glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.width, rect.height,
							GL_RGB, GL_UNSIGNED_BYTE, patch.data());
			painted++;
		}

		// The texture is sampled with mipmapping (see buildGroundTextureIfNeeded),
		// so the lower levels are stale until regenerated - a scorch would
		// otherwise vanish as the camera pulls back. Done once per frame
		// rather than per mark.
		if (painted > 0) glGenerateMipmap(GL_TEXTURE_2D);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glBindTexture(GL_TEXTURE_2D, 0);

		// Those patches came from the untinted copy, so with the movement
		// overlay up they punch un-darkened holes in it. Rare (it needs a
		// blast while a move is being chosen) and cheap to put right: ask
		// for a full repaint on the next frame.
		if (painted > 0 && movementOverlayPainted) paintedMovementVersion--;

		if (painted > 0 && terrainScorchLogsLeft > 0) {
			terrainScorchLogsLeft--;
			LOGI("Scorch marks painted: %d of %zu queued", painted, events.size());
		}
	}

	// The aim sight blade. Upstream sweeps ~36-45deg of arc either side
	// (126deg -> 90deg and 90deg -> 135deg in 9deg steps, fading out over
	// 45deg), which reads as an uncomfortably wide wedge on a phone-sized
	// screen - so the span is narrowed here, with the fade tied to it so
	// the blade still fades to nothing exactly at its edge. Geometry is
	// otherwise upstream's: a quad strip from radius 2 to 10, mirrored
	// about the aim line, brightest along it.
	constexpr float kSightSpanDegrees = 16.0f;  // arc either side of the aim line
	constexpr int   kSightSteps = 4;

	// M22: upstream's sight, as geometry rather than as its four textures.
	//
	// aimrotation.png is a ring of twenty-four radial ticks, aimbot.png a
	// blue tapered blade and aimtop.png a red one - shapes, not artwork, so
	// they are built here instead of decoding PNGs and adding a textured
	// pass for four of them. The colours are the textures' own.
	//
	// Triangles rather than a strip: the ring and the arc are rows of
	// separate ticks, and stitching those into one strip means degenerate
	// vertices between every pair. Two triangles per quad costs a few dozen
	// vertices on a mesh built once.
	//
	// Not reproduced: aimside.png, a one-unit strip standing on edge beside
	// the barrel blade. It reads as a thin line even in upstream and adds
	// nothing on a phone.
	struct SightVertex { float x, y, z, r, g, b; };

	void appendQuad(std::vector<float> &verts,
		const SightVertex &a, const SightVertex &b,
		const SightVertex &c, const SightVertex &d)
	{
		const SightVertex order[6] = { a, b, c, a, c, d };
		for (const SightVertex &v : order) {
			verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
			verts.push_back(v.r); verts.push_back(v.g); verts.push_back(v.b);
		}
	}

	// A spike: a point at `near` widening to `halfWidth` at `far`, in columns
	// so the colour can fade to the edges the way the textures' alpha does.
	// `flat` lays it on the ground rather than along the barrel.
	void appendTaperedBlade(std::vector<float> &verts,
		float nearRadius, float farRadius, float halfWidth,
		float r, float g, float b, bool flat)
	{
		const int columns = 4;
		// A little clear of the ground rather than flat on it: this port's
		// terrain mesh is coarser than upstream's, so a ring lying at exactly
		// the tank's base height disappears into the interpolated surface on
		// any slope.
		const float lift = flat ? 0.4f : 0.0f;
		auto vertex = [&](float lateral, float radius) {
			const float fade = 1.0f - fabsf(lateral);
			const float taper = (radius - nearRadius) / (farRadius - nearRadius);
			SightVertex v;
			v.x = lateral * halfWidth * taper;
			v.y = lift;
			v.z = -radius;
			v.r = r * fade; v.g = g * fade; v.b = b * fade;
			return v;
		};
		for (int i = -columns; i < columns; i++) {
			const float left = (float) i / (float) columns;
			const float right = (float) (i + 1) / (float) columns;
			appendQuad(verts,
				vertex(left, nearRadius), vertex(left, farRadius),
				vertex(right, farRadius), vertex(right, nearRadius));
		}
	}

	// One arc of ticks: upstream's ring is twenty-four of them over a full
	// turn, and its elevation quadrant is a quarter of the same.
	void appendTickArc(std::vector<float> &verts, int ticks, float sweep,
		float inner, float outer, bool flat, float r, float g, float b)
	{
		const float halfTick = 0.11f;   // radians; upstream's ticks are ~6deg
		for (int i = 0; i < ticks; i++) {
			const float centre = sweep * ((float) i / (float) ticks);
			auto vertex = [&](float angle, float radius) {
				SightVertex v;
				if (flat) {
					// Flat on the ground: the ring lies in the x/z plane.
					v.x = sinf(angle) * radius;
					v.y = 0.4f;
					v.z = -cosf(angle) * radius;
				} else {
					// Standing up in the plane the barrel swings through.
					v.x = 0.0f;
					v.y = sinf(angle) * radius;
					v.z = -cosf(angle) * radius;
				}
				v.r = r; v.g = g; v.b = b;
				return v;
			};
			appendQuad(verts,
				vertex(centre - halfTick, inner), vertex(centre - halfTick, outer),
				vertex(centre + halfTick, outer), vertex(centre + halfTick, inner));
		}
	}

	void uploadSightPiece(const std::vector<float> &verts, GLuint &vao, GLuint &vbo, int &count)
	{
		count = (int) (verts.size() / 6);
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) (3 * sizeof(float)));
		glBindVertexArray(0);
	}

	void buildOriginalSightGeometry()
	{
		if (sightRingVertexCount > 0) return;

		const float kInner = 9.5f, kOuter = 13.5f;
		const float kRingR = 0.68f, kRingG = 0.68f, kRingB = 1.0f;

		// Flat under the tank: the protractor ring.
		{
			std::vector<float> verts;
			appendTickArc(verts, 24, 2.0f * (float) M_PI, kInner, kOuter, true,
				kRingR, kRingG, kRingB);
			uploadSightPiece(verts, sightRingVao, sightRingVbo, sightRingVertexCount);
		}

		// Turning with the turret: the blue bearing marker lying on the
		// ground, and the arc the barrel's elevation is read against.
		{
			std::vector<float> verts;
			appendTaperedBlade(verts, 3.0f, 15.0f, 1.0f, 0.35f, 0.35f, 1.0f, true);
			appendTickArc(verts, 7, (float) M_PI_2, kInner, kOuter, false,
				kRingR, kRingG, kRingB);
			uploadSightPiece(verts, sightBearingVao, sightBearingVbo, sightBearingVertexCount);
		}

		// Lifting with the gun: the red blade along the barrel, upstream's
		// aimtop.png - two units across at fifteen out, a point at three.
		{
			std::vector<float> verts;
			appendTaperedBlade(verts, 3.0f, 15.0f, 1.0f, 1.0f, 0.25f, 0.25f, false);
			uploadSightPiece(verts, sightBarrelVao, sightBarrelVbo, sightBarrelVertexCount);
		}
	}

	void buildSightGeometry()
	{
		if (sightVertexCount > 0) return;

		std::vector<float> verts;
		auto emit = [&](float angleDeg, float side) {
			float dx = angleDeg * (float) M_PI / 180.0f;
			float color = 1.0f - fabsf(90.0f - angleDeg) / kSightSpanDegrees;
			if (color < 0.0f) color = 0.0f;
			for (float radius : { 2.0f, 10.0f }) {
				verts.push_back(side * 0.03f * color);
				verts.push_back(radius * cosf(dx));    // upstream z -> our y
				verts.push_back(-radius * sinf(dx));   // upstream y -> our -z
				verts.push_back(1.0f * color);
				verts.push_back(0.5f * color);
				verts.push_back(0.5f * color);
			}
		};
		const float step = kSightSpanDegrees / (float) kSightSteps;
		for (int i = kSightSteps; i >= 0; i--) emit(90.0f + i * step, +1.0f);
		for (int i = 0; i <= kSightSteps; i++) emit(90.0f + i * step, -1.0f);

		sightVertexCount = (int) (verts.size() / 6);
		glGenVertexArrays(1, &sightVao);
		glBindVertexArray(sightVao);
		glGenBuffers(1, &sightVbo);
		glBindBuffer(GL_ARRAY_BUFFER, sightVbo);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) (3 * sizeof(float)));
		glBindVertexArray(0);
	}

	// M6: a model uploaded to the GPU, plus the placement info needed to
	// draw it at the right size. Cached per Model* - several tanks usually
	// share one model, and reparsing/re-uploading per frame would be
	// pointless.
	//
	// Tank models are articulated: upstream splits the meshes into hull,
	// turret and gun by *name* and rotates each group separately, so the
	// turret swings to the firing bearing and the barrel lifts to the
	// elevation (ModelRendererTank::setupModelRendererTank/draw). We do the
	// same, with each group's vertices pre-translated onto its own pivot at
	// upload time - exactly as upstream does with setVertexTranslation -
	// so drawing is just three transforms rather than any per-vertex work.
	// V2/V3: one upstream Mesh's share of a group - its triangles, its
	// texture and its material, the way ModelRendererMesh::drawMesh sets
	// them before each mesh. Consecutive meshes that agree on all of it are
	// merged into one range.
	struct MeshRange {
		int first = 0, count = 0;
		GLuint texture = 0;
		bool sphereMap = false;
		float ambient[3] = { 0.0f, 0.0f, 0.0f };
		float diffuse[3] = { 0.0f, 0.0f, 0.0f };
		float emissive[3] = { 0.0f, 0.0f, 0.0f };
	};
	struct MeshGroup {
		GLuint vao = 0, vbo = 0;
		int vertexCount = 0;
		std::vector<MeshRange> ranges;
	};
	// Floats per model vertex: position, normal, texture coordinate.
	const int kMeshFloats = 8;
	struct GpuModel {
		MeshGroup hull, turret, gun;
		float scale = 1.0f;         // upstream's "don't let the model be huge" rule
		float rawSize = 0.0f;       // the model's bounding diagonal before any scale
		float groundOffset = 0.0f;  // lifts the model so its base sits on the ground
		// The same lift in raw model units. Non-tank targets carry their
		// own scale from the landscape definition rather than the tank
		// sizing rule above, so they scale this themselves.
		// Gun pivot relative to the turret pivot, already in our Y-up space.
		float baseOffset = 0.0f;
		float gunOffsetX = 0.0f, gunOffsetY = 0.0f, gunOffsetZ = 0.0f;
	};
	std::map<Model *, GpuModel> g_modelCache;
	// `adb shell setprop debug.scorchdroid.water N` picks a debug view of
	// the water shader (see uDebugMode there); polled once a second so a
	// phone that cannot be attached to a debugger can still be diagnosed.
	int waterDebugMode()
	{
		static int cached = 0;
		static double checkedAt = -10.0;
		if (lastFrameSeconds - checkedAt < 1.0) return cached;
		checkedAt = lastFrameSeconds;
		char value[PROP_VALUE_MAX] = { 0 };
		if (__system_property_get("debug.scorchdroid.water", value) > 0) cached = atoi(value);
		else cached = 0;
		return cached;
	}

	// The view matrix of the pass being drawn, for the sphere-mapped
	// meshes (GL_SPHERE_MAP works in eye space). Set before each pass.
	Mat4 g_passView = Mat4::identity();

	// V2: model textures by file name, as upstream's GLTextureReference
	// shares them. A MilkShape mesh names its texture (and, rarely, a
	// separate alpha image) with an absolute path built by MSModelFactory;
	// an .ase tank gets its <skin> on every mesh (ModelStore::getModel).
	// Missing files are remembered as 0 so they are not retried per frame.
	std::map<std::string, GLuint> g_modelTextures;
	GLuint modelTexture(const char *name, const char *alphaName)
	{
		if (!name || !name[0]) return 0;
		std::string key = std::string(name) + "|" + (alphaName ? alphaName : "");
		auto it = g_modelTextures.find(key);
		if (it != g_modelTextures.end()) return it->second;

		GLuint texture = 0;
		Image image = LandscapeTextureBuilder::toRGB(ImageFactory::loadImage(
			S3D::eAbsLocation, name, alphaName ? alphaName : "", false));
		if (image.getBits() && image.getWidth() > 0) {
			glGenTextures(1, &texture);
			glBindTexture(GL_TEXTURE_2D, texture);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			const GLenum format = (image.getComponents() == 4) ? GL_RGBA : GL_RGB;
			glTexImage2D(GL_TEXTURE_2D, 0, (GLint) format,
						 image.getWidth(), image.getHeight(), 0,
						 format, GL_UNSIGNED_BYTE, image.getBits());
			// GLTexture::create: mipmapped, trilinear, repeating.
			glGenerateMipmap(GL_TEXTURE_2D);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			applyAnisotropy();
			glBindTexture(GL_TEXTURE_2D, 0);
			LOGI("Model texture loaded: %s (%dx%d, %d ch)", name,
				 image.getWidth(), image.getHeight(), image.getComponents());
		} else {
			LOGI("Model texture FAILED: %s", name);
		}
		g_modelTextures[key] = texture;
		return texture;
	}

	// ModelStore::getModel() calls DIALOG_ASSERT (i.e. abort, in this port -
	// see the porting plan's dialogAssert note) when handed a ModelID it
	// can't resolve, rather than returning null. Not every tank defines a
	// projectile model, so an unguarded loadModel() on an empty id takes
	// the whole process down - hence checking modelValid() first.
	Model *loadModelSafely(ModelID &id)
	{
		if (!id.modelValid()) return nullptr;
		Model *model = ModelStore::instance()->loadModel(id);
		static std::set<std::string> logged;
		if (logged.insert(id.getStringHash()).second) {
			LOGI("Model loaded: %s mesh %s skin %s -> %p", id.getType(), id.getMeshName(),
				 id.getSkinName(), (void *) model);
		}
		return model;
	}

	// The model every shot falls back to, straight out of upstream's own
	// Accessory::getWeaponMesh - a V2 rocket. Almost every weapon reaches
	// it, because declaring a <projectilemodel> is the exception, not the
	// rule: the Baby Missile, the Missile and most of the arsenal say
	// nothing about what they look like in flight and rely on this.
	//
	// Resolved through the mod path like any other data file, so a mod that
	// ships its own v2missile gets its own rocket.
	ModelID &defaultProjectileModelId()
	{
		static ModelID id;
		if (!id.modelValid()) {
			id.initFromString("MilkShape", "data/accessories/v2missile/v2missile.txt", "");
		}
		return id;
	}

	// Models are Z-up with +y forward (upstream's world convention, the same
	// one the landscape uses) while our renderer is Y-up, so vertices are
	// remapped (x, y, z) -> (x, z, -y) once here rather than fought with a
	// transform at every draw. The negation is the same one
	// worldZFromEngineY carries and for the same reason: without it the
	// remap is a reflection, and every model would be drawn mirrored, wound
	// backwards for face culling, and turned the wrong way by rotateY.
	//
	// The pivot offsets are subtracted in *model* space, before the remap,
	// so they stay in the model's own axes - hence the parameter names.
	//
	// V2/V3: each vertex also carries the face's texture coordinate, and
	// each mesh its texture and material, chosen the way upstream's
	// drawModel does - a model that uses textures anywhere takes every
	// mesh's textured material (an .ase's 0.6 grey ambient and 0.8 grey
	// diffuse), one that uses none takes the "no texture" colours (the
	// .ase's own colour times the same two).
	void uploadMeshGroup(MeshGroup &group, const std::vector<Mesh *> &meshes,
						 float offModelX, float offModelZ, float offModelY,
						 bool textured)
	{
		std::vector<float> verts;
		for (Mesh *mesh : meshes) {
			MeshRange range;
			range.first = (int) (verts.size() / kMeshFloats);
			range.texture = textured ? modelTexture(mesh->getTextureName(), mesh->getATextureName()) : 0;
			range.sphereMap = textured && mesh->getSphereMap();
			FixedVector4 &amb = textured ? mesh->getAmbientColor() : mesh->getAmbientNoTexColor();
			FixedVector4 &dif = textured ? mesh->getDiffuseColor() : mesh->getDiffuseNoTexColor();
			FixedVector4 &emi = textured ? mesh->getEmissiveColor() : mesh->getEmissiveNoTexColor();
			for (int i = 0; i < 3; i++) {
				range.ambient[i] = amb[i].asFloat();
				range.diffuse[i] = dif[i].asFloat();
				range.emissive[i] = emi[i].asFloat();
			}
			for (Face *face : mesh->getFaces()) {
				// drawVerts skips degenerate faces.
				if (face->v[0] == face->v[1] || face->v[1] == face->v[2] || face->v[0] == face->v[2]) continue;
				for (int i = 0; i < 3; i++) {
					Vertex *v = mesh->getVertexes()[face->v[i]];
					verts.push_back(v->position[0].asFloat() - offModelX);
					verts.push_back(v->position[2].asFloat() - offModelZ);
					verts.push_back(-(v->position[1].asFloat() - offModelY));
					verts.push_back(face->normal[i][0].asFloat());
					verts.push_back(face->normal[i][2].asFloat());
					verts.push_back(-face->normal[i][1].asFloat());
					verts.push_back(face->tcoord[i][0].asFloat());
					verts.push_back(face->tcoord[i][1].asFloat());
				}
			}
			range.count = (int) (verts.size() / kMeshFloats) - range.first;
			if (range.count == 0) continue;
			if (!group.ranges.empty()) {
				MeshRange &last = group.ranges.back();
				if (last.texture == range.texture && last.sphereMap == range.sphereMap &&
					memcmp(last.ambient, range.ambient, sizeof(range.ambient)) == 0 &&
					memcmp(last.diffuse, range.diffuse, sizeof(range.diffuse)) == 0 &&
					memcmp(last.emissive, range.emissive, sizeof(range.emissive)) == 0) {
					last.count += range.count;
					continue;
				}
			}
			group.ranges.push_back(range);
		}
		group.vertexCount = (int) (verts.size() / kMeshFloats);
		if (verts.empty()) return;

		glGenVertexArrays(1, &group.vao);
		glBindVertexArray(group.vao);
		glGenBuffers(1, &group.vbo);
		glBindBuffer(GL_ARRAY_BUFFER, group.vbo);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kMeshFloats * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kMeshFloats * sizeof(float), (void *) (3 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kMeshFloats * sizeof(float), (void *) (6 * sizeof(float)));
		glBindVertexArray(0);
	}

	GpuModel *uploadModel(Model *model)
	{
		if (!model) return nullptr;
		auto existing = g_modelCache.find(model);
		if (existing != g_modelCache.end()) return &existing->second;

		// Classify meshes exactly the way upstream does. Note the leading
		// quote: .ase mesh names arrive quoted, so the prefix really is
		// "\"Turret" / "\"Gun".
		std::vector<Mesh *> hullMeshes, turretMeshes, gunMeshes;
		Mesh *turretPivot = nullptr, *gunPivot = nullptr;
		FixedVector turretCenter;
		int turretCount = 0;
		for (Mesh *mesh : model->getMeshes()) {
			const char *name = mesh->getName();
			bool isPivot = (strstr(name, "pivot") || strstr(name, "Pivot"));
			if (strstr(name, "\"Turret") == name || strstr(name, "\"turret") == name) {
				if (isPivot) {
					turretPivot = mesh;
				} else {
					turretCount++;
					turretCenter += (mesh->getMax() + mesh->getMin()) / fixed(2);
				}
				turretMeshes.push_back(mesh);
			} else if (strstr(name, "\"Gun") == name || strstr(name, "\"gun") == name) {
				if (isPivot) gunPivot = mesh;
				gunMeshes.push_back(mesh);
			} else {
				hullMeshes.push_back(mesh);
			}
		}

		if (turretPivot) {
			turretCenter = (turretPivot->getMax() + turretPivot->getMin()) / fixed(2);
		} else if (turretCount > 0) {
			turretCenter /= fixed(turretCount);
		}
		FixedVector gunCenter = turretCenter;
		turretCenter[2] = fixed(0);  // turret spins about the model's up axis
		if (gunPivot) gunCenter = (gunPivot->getMax() + gunPivot->getMin()) / fixed(2);
		FixedVector gunOffset = gunCenter - turretCenter;

		GpuModel gpu;

		// Same sizing rule upstream uses (ModelRendererTank::setup): keep
		// models from dwarfing the battlefield.
		FixedVector minV = model->getMin(), maxV = model->getMax();
		float dx = (maxV[0] - minV[0]).asFloat();
		float dy = (maxV[1] - minV[1]).asFloat();
		float dz = (maxV[2] - minV[2]).asFloat();
		float size = sqrtf(dx * dx + dy * dy + dz * dz);
		const float kMaxSize = 3.0f;
		if (size > kMaxSize) gpu.scale = 2.2f / size;
		gpu.rawSize = size;

		// Hull and turret sit on the turret pivot; the gun additionally
		// sits on its own pivot so it elevates about the right point.
		// Pivots stay in model axes for uploadMeshGroup, which subtracts
		// them before the remap; the gun offset is *also* used as a world
		// translate at draw time, so that copy carries the remap's negated
		// forward axis.
		float tcx = turretCenter[0].asFloat();
		float tcModelZ = turretCenter[2].asFloat();
		float tcModelY = turretCenter[1].asFloat();
		float gunModelX = gunOffset[0].asFloat();
		float gunModelZ = gunOffset[2].asFloat();
		float gunModelY = gunOffset[1].asFloat();
		gpu.gunOffsetX = gunModelX;
		gpu.gunOffsetY = gunModelZ;
		gpu.gunOffsetZ = -gunModelY;

		const bool textured = model->getTexturesUsed();
		uploadMeshGroup(gpu.hull, hullMeshes, tcx, tcModelZ, tcModelY, textured);
		uploadMeshGroup(gpu.turret, turretMeshes, tcx, tcModelZ, tcModelY, textured);
		uploadMeshGroup(gpu.gun, gunMeshes,
						tcx + gunModelX, tcModelZ + gunModelZ, tcModelY + gunModelY, textured);

		if (gpu.hull.vertexCount == 0 && gpu.turret.vertexCount == 0 && gpu.gun.vertexCount == 0) {
			return nullptr;
		}

		// Vertices are now relative to the turret pivot, so "sit on the
		// ground" is measured from there too.
		gpu.baseOffset = tcModelZ - minV[2].asFloat();
		gpu.groundOffset = gpu.baseOffset * gpu.scale;

		LOGI("Model uploaded: hull %d, turret %d, gun %d tris, scale %.3f, %s, %d ranges",
			 gpu.hull.vertexCount / 3, gpu.turret.vertexCount / 3, gpu.gun.vertexCount / 3, gpu.scale,
			 textured ? "textured" : "untextured",
			 (int) (gpu.hull.ranges.size() + gpu.turret.ranges.size() + gpu.gun.ranges.size()));
		g_modelCache[model] = gpu;
		return &g_modelCache[model];
	}

	// Binds one range's material and texture for whichever of the two
	// model programs is current.
	void setMeshRangeState(const MeshRange &range, GLint matAmbientLoc, GLint matDiffuseLoc,
						   GLint matEmissiveLoc, GLint hasTextureLoc, GLint sphereMapLoc)
	{
		glUniform3fv(matAmbientLoc, 1, range.ambient);
		glUniform3fv(matDiffuseLoc, 1, range.diffuse);
		glUniform3fv(matEmissiveLoc, 1, range.emissive);
		glUniform1i(hasTextureLoc, range.texture != 0 ? 1 : 0);
		glUniform1i(sphereMapLoc, range.sphereMap ? 1 : 0);
		if (range.texture != 0) {
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, range.texture);
		}
	}

	// The per-pass state of the mesh program for upstream models: the
	// sun as GL_LIGHT1, the sky's two light colours, this pass's view,
	// and no tint - upstream draws its models with no glColor at all.
	void beginUpstreamModels()
	{
		glUniform1i(meshLightModeLoc, 1);
		glUniform4f(meshColorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
		glUniformMatrix4fv(meshViewLoc, 1, GL_FALSE, g_passView.m);
		glUniform3f(meshSunPosLoc,
					skyDescription.sunPosition[0],
					skyDescription.sunPosition[2],
					worldZFromEngineY(skyDescription.sunPosition[1]));
		glUniform3fv(meshSkyAmbientLoc, 1, skyDescription.ambience);
		glUniform3fv(meshSkyDiffuseLoc, 1, skyDescription.diffuse);
	}

	// The mesh program drawing something that is not an upstream model
	// (shields, parachutes): the port's own flat shade, untextured.
	void beginFlatMeshes()
	{
		glUniform1i(meshLightModeLoc, 0);
		glUniform1i(meshHasTextureLoc, 0);
		glUniform1i(meshSphereMapLoc, 0);
		glUniformMatrix4fv(meshModelLoc, 1, GL_FALSE, Mat4::identity().m);
		glUniform3f(meshLightDirLoc, 0.4f, 0.82f, 0.35f);
	}

	void drawMeshGroup(const MeshGroup &group, const Mat4 &vp, const Mat4 &model)
	{
		if (group.vertexCount == 0) return;
		glUniformMatrix4fv(meshMvpLoc, 1, GL_FALSE, Mat4::multiply(vp, model).m);
		glUniformMatrix4fv(meshModelLoc, 1, GL_FALSE, model.m);
		glBindVertexArray(group.vao);
		for (const MeshRange &range : group.ranges) {
			setMeshRangeState(range, meshMatAmbientLoc, meshMatDiffuseLoc, meshMatEmissiveLoc,
							  meshHasTextureLoc, meshSphereMapLoc);
			frameDrawCalls++; glDrawArrays(GL_TRIANGLES, range.first, range.count);
		}
	}

	// Draws a set of real world-space positions (already x,y,z in the same
	// units as the terrain mesh) as colored point sprites - tanks, shots,
	// and explosions. Reused for all three, same as the old 2D renderer's
	// drawPoints() - see ActionController::getShotAndExplosionPositions()
	// for why shots/explosions need this at all (nothing was visibly
	// happening on a fired shot before that hookup existed).
	void drawPoints(const Mat4 &mvp, const std::vector<float> &worldPositions, float size, float r, float g, float b)
	{
		if (worldPositions.empty()) return;

		glUniformMatrix4fv(pointMvpLoc, 1, GL_FALSE, mvp.m);
		glUniform1f(pointSizeLoc, size);
		glUniform4f(pointColorLoc, r, g, b, 1.0f);
		glBindVertexArray(pointVao);
		glBindBuffer(GL_ARRAY_BUFFER, pointVbo);
		glBufferData(GL_ARRAY_BUFFER, worldPositions.size() * sizeof(float), worldPositions.data(), GL_DYNAMIC_DRAW);
		frameDrawCalls++; glDrawArrays(GL_POINTS, 0, (GLsizei) (worldPositions.size() / 3));
	}

	// Real height at an arbitrary landscape-space (x,z), nearest-sample
	// (no interpolation - fine for placing a tank/shot marker a little
	// above the ground, not for anything precision-sensitive).
	float heightAt(HeightMap &heightMap, int mapW, int mapH, float x, float z)
	{
		int sx = std::min(std::max((int) x, 0), mapW - 1);
		int sz = std::min(std::max((int) z, 0), mapH - 1);
		return heightMap.getHeight(sx, sz).asFloat();
	}
}  // namespace

// See RenderState.hpp. Read off the same per-frame camera snapshot the pick
// ray uses, rather than re-deriving the eye position - one derivation, one
// mutex, and no second copy to fall out of step.
//
// The renderer's world axes are not the engine's: world Y is height where
// the engine's is Z, and world Z runs the opposite way to landscape Y (see
// worldZFromEngineY). Converted here so no caller has to remember that.
bool renderListenerEnginePosition(float &x, float &y, float &z) {
	std::lock_guard<std::mutex> lock(g_pickMutex);
	if (!g_pickCamera.valid) return false;
	x = g_pickCamera.eyeX;
	y = engineYFromWorldZ(g_pickCamera.eyeZ);
	z = g_pickCamera.eyeY;
	return true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeOnSurfaceCreated(JNIEnv *, jobject) {
	terrainProgram = linkProgram(kTerrainVertexShader, kTerrainFragmentShader);
	terrainMvpLoc = glGetUniformLocation(terrainProgram, "uMVP");
	terrainShadowMatrixLoc = glGetUniformLocation(terrainProgram, "uShadowMatrix");
	terrainShadowTexLoc = glGetUniformLocation(terrainProgram, "uShadowTex");
	terrainShadowEnabledLoc = glGetUniformLocation(terrainProgram, "uShadowEnabled");
	terrainAmbienceLoc = glGetUniformLocation(terrainProgram, "uAmbience");
	terrainDiffuseLoc = glGetUniformLocation(terrainProgram, "uDiffuse");
	terrainSunPosLoc = glGetUniformLocation(terrainProgram, "uSunPos");
	terrainDetailTexLoc = glGetUniformLocation(terrainProgram, "uDetailTexture");
	terrainHasDetailLoc = glGetUniformLocation(terrainProgram, "uHasDetail");
	terrainMinHeightLoc = glGetUniformLocation(terrainProgram, "uMinHeight");
	terrainHeightRangeLoc = glGetUniformLocation(terrainProgram, "uHeightRange");
	terrainLightDirLoc = glGetUniformLocation(terrainProgram, "uLightDir");
	terrainGroundTexLoc = glGetUniformLocation(terrainProgram, "uGroundTexture");
	terrainHasTextureLoc = glGetUniformLocation(terrainProgram, "uHasTexture");
	terrainLightBakedLoc = glGetUniformLocation(terrainProgram, "uLightBaked");
	terrainHalfLambertLoc = glGetUniformLocation(terrainProgram, "uHalfLambert");
	terrainClipEnabledLoc = glGetUniformLocation(terrainProgram, "uClipEnabled");
	terrainClipBelowLoc = glGetUniformLocation(terrainProgram, "uClipBelowY");
	terrainFogColorLoc = glGetUniformLocation(terrainProgram, "uFogColor");
	terrainFogDensityLoc = glGetUniformLocation(terrainProgram, "uFogDensity");

	shadowProgram = linkProgram(kShadowVertexShader, kShadowFragmentShader);
	shadowMvpLoc = glGetUniformLocation(shadowProgram, "uMVP");
	shadowStrengthLoc = glGetUniformLocation(shadowProgram, "uStrength");
	glGenVertexArrays(1, &shadowVao);
	glGenBuffers(1, &shadowVbo);
	glBindVertexArray(shadowVao);
	glBindBuffer(GL_ARRAY_BUFFER, shadowVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
	glBindVertexArray(0);

	spriteProgram = linkProgram(kSpriteVertexShader, kSpriteFragmentShader);
	spriteMvpLoc = glGetUniformLocation(spriteProgram, "uMVP");
	spriteSamplerLoc = glGetUniformLocation(spriteProgram, "uTexture");
	spriteTintLoc = glGetUniformLocation(spriteProgram, "uTint");
	glGenVertexArrays(1, &spriteVao);
	glGenBuffers(1, &spriteVbo);
	glBindVertexArray(spriteVao);
	glBindBuffer(GL_ARRAY_BUFFER, spriteVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
	glBindVertexArray(0);

	cloudProgram = linkProgram(kCloudVertexShader, kCloudFragmentShader);
	cloudMvpLoc = glGetUniformLocation(cloudProgram, "uMVP");
	cloudScrollLoc = glGetUniformLocation(cloudProgram, "uScroll");
	cloudFogColorLoc = glGetUniformLocation(cloudProgram, "uFogColor");
	cloudFogDensityLoc = glGetUniformLocation(cloudProgram, "uFogDensity");
	cloudEyePosLoc = glGetUniformLocation(cloudProgram, "uEyePos");
	cloudTexScaleLoc = glGetUniformLocation(cloudProgram, "uTexScale");
	cloudSamplerLoc = glGetUniformLocation(cloudProgram, "uClouds");
	cloudTintLoc = glGetUniformLocation(cloudProgram, "uTint");
	cloudOpacityLoc = glGetUniformLocation(cloudProgram, "uOpacity");

	skyProgram = linkProgram(kSkyVertexShader, kSkyFragmentShader);
	skyGradientLoc = glGetUniformLocation(skyProgram, "uGradient");
	skySunDirLoc = glGetUniformLocation(skyProgram, "uSunDir");
	skySunColorLoc = glGetUniformLocation(skyProgram, "uSunColor");
	skyGlowLoc = glGetUniformLocation(skyProgram, "uHorizonGlow");
	skyFlashLoc = glGetUniformLocation(skyProgram, "uFlash");
	skySunDiscLoc = glGetUniformLocation(skyProgram, "uSunDisc");
	skyFogColorLoc = glGetUniformLocation(skyProgram, "uFogColor");
	skyFogDensityLoc = glGetUniformLocation(skyProgram, "uFogDensity");
	skyEyeHeightLoc = glGetUniformLocation(skyProgram, "uEyeHeight");
	glGenVertexArrays(1, &skyVao);
	glGenBuffers(1, &skyVbo);
	glBindVertexArray(skyVao);
	glBindBuffer(GL_ARRAY_BUFFER, skyVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (2 * sizeof(float)));
	glBindVertexArray(0);

	breakerProgram = linkProgram(kBreakerVertexShader, kBreakerFragmentShader);
	breakerMvpLoc = glGetUniformLocation(breakerProgram, "uMVP");
	breakerFrontLoc = glGetUniformLocation(breakerProgram, "uFront");
	breakerEndLoc = glGetUniformLocation(breakerProgram, "uEnd");
	breakerAlphaLoc = glGetUniformLocation(breakerProgram, "uAlpha");
	breakerWaterHeightLoc = glGetUniformLocation(breakerProgram, "uWaterHeight");
	breakerTileLoc = glGetUniformLocation(breakerProgram, "uWaveTileLength");
	breakerWindLoc = glGetUniformLocation(breakerProgram, "uWind");
	breakerWaveTexLoc = glGetUniformLocation(breakerProgram, "uWaveTex");
	breakerTextureLoc = glGetUniformLocation(breakerProgram, "uTexture");

	waterProgram = linkProgram(kWaterVertexShader, kWaterFragmentShader);
	waterMvpLoc = glGetUniformLocation(waterProgram, "uMVP");
	waterUpwellTopLoc = glGetUniformLocation(waterProgram, "uUpwellTop");
	waterUpwellBotLoc = glGetUniformLocation(waterProgram, "uUpwellBot");
	waterHeightLoc = glGetUniformLocation(waterProgram, "uWaterHeight");
	waterSunDiffuseLoc = glGetUniformLocation(waterProgram, "uSunDiffuse");
	waterNoise0Loc = glGetUniformLocation(waterProgram, "uNoise0");
	waterNoise1Loc = glGetUniformLocation(waterProgram, "uNoise1");
	waterReflectMatrixLoc = glGetUniformLocation(waterProgram, "uReflectMatrix");
	waterShadowTexLoc = glGetUniformLocation(waterProgram, "uShadowTex");
	waterShadowMatrixLoc = glGetUniformLocation(waterProgram, "uShadowMatrix");
	waterShadowEnabledLoc = glGetUniformLocation(waterProgram, "uShadowEnabled");
	waterAlphaLoc = glGetUniformLocation(waterProgram, "uAlpha");
	waterTimeLoc = glGetUniformLocation(waterProgram, "uTime");
	waterSkyHorizonLoc = glGetUniformLocation(waterProgram, "uSkyHorizon");
	waterSkyZenithLoc = glGetUniformLocation(waterProgram, "uSkyZenith");
	waterWaveTexLoc = glGetUniformLocation(waterProgram, "uWaveTex");
	waterWaveNormalTexLoc = glGetUniformLocation(waterProgram, "uWaveNormalTex");
	waterFoamMaskLoc = glGetUniformLocation(waterProgram, "uFoamMask");
	waterWaveTileLoc = glGetUniformLocation(waterProgram, "uWaveTileLength");
	waterReflectTexLoc = glGetUniformLocation(waterProgram, "uReflectionTex");
	waterUseReflectLoc = glGetUniformLocation(waterProgram, "uUseReflection");
	waterEyePosLoc = glGetUniformLocation(waterProgram, "uEyePos");
	waterFogColorLoc = glGetUniformLocation(waterProgram, "uFogColor");
	waterFogDensityLoc = glGetUniformLocation(waterProgram, "uFogDensity");
	waterDebugModeLoc = glGetUniformLocation(waterProgram, "uDebugMode");
	waterWaveAmpLoc = glGetUniformLocation(waterProgram, "uWaveAmplitude");
	waterWaveLodLoc = glGetUniformLocation(waterProgram, "uWaveLod");
	waterSunDirLoc = glGetUniformLocation(waterProgram, "uSunPos");
	waterMapSizeLoc = glGetUniformLocation(waterProgram, "uMapSize");

	sightProgram = linkProgram(kSightVertexShader, kSightFragmentShader);
	sightMvpLoc = glGetUniformLocation(sightProgram, "uMVP");
	sightFogColorLoc = glGetUniformLocation(sightProgram, "uFogColor");
	sightFogDensityLoc = glGetUniformLocation(sightProgram, "uFogDensity");
	sightVertexCount = 0;
	sightRingVertexCount = 0;
	sightBearingVertexCount = 0;
	sightBarrelVertexCount = 0;

	meshProgram = linkProgram(kMeshVertexShader, kMeshFragmentShader);
	meshMvpLoc = glGetUniformLocation(meshProgram, "uMVP");
	meshLightDirLoc = glGetUniformLocation(meshProgram, "uLightDir");
	meshColorLoc = glGetUniformLocation(meshProgram, "uColor");
	meshFogColorLoc = glGetUniformLocation(meshProgram, "uFogColor");
	meshFogDensityLoc = glGetUniformLocation(meshProgram, "uFogDensity");
	meshModelLoc = glGetUniformLocation(meshProgram, "uModel");
	meshViewLoc = glGetUniformLocation(meshProgram, "uView");
	meshSunPosLoc = glGetUniformLocation(meshProgram, "uSunPos");
	meshSkyAmbientLoc = glGetUniformLocation(meshProgram, "uSkyAmbient");
	meshSkyDiffuseLoc = glGetUniformLocation(meshProgram, "uSkyDiffuse");
	meshMatAmbientLoc = glGetUniformLocation(meshProgram, "uMatAmbient");
	meshMatDiffuseLoc = glGetUniformLocation(meshProgram, "uMatDiffuse");
	meshMatEmissiveLoc = glGetUniformLocation(meshProgram, "uMatEmissive");
	meshLightModeLoc = glGetUniformLocation(meshProgram, "uLightMode");
	meshHasTextureLoc = glGetUniformLocation(meshProgram, "uHasTexture");
	meshSphereMapLoc = glGetUniformLocation(meshProgram, "uSphereMap");
	meshTextureLoc = glGetUniformLocation(meshProgram, "uTexture");
	glUseProgram(meshProgram);
	glUniform1i(meshTextureLoc, 0);

	instancedMeshProgram = linkProgram(kInstancedMeshVertexShader, kInstancedMeshFragmentShader);
	treeProgram = linkProgram(kTreeVertexShader, kTreeFragmentShader);
	treeViewProjLoc = glGetUniformLocation(treeProgram, "uViewProj");
	treeLightDirLoc = glGetUniformLocation(treeProgram, "uLightDir");
	treeFogColorLoc = glGetUniformLocation(treeProgram, "uFogColor");
	treeFogDensityLoc = glGetUniformLocation(treeProgram, "uFogDensity");
	treeAtlasLoc = glGetUniformLocation(treeProgram, "uAtlas");
	instancedViewProjLoc = glGetUniformLocation(instancedMeshProgram, "uViewProj");
	instancedLightDirLoc = glGetUniformLocation(instancedMeshProgram, "uLightDir");
	instancedFogColorLoc = glGetUniformLocation(instancedMeshProgram, "uFogColor");
	instancedFogDensityLoc = glGetUniformLocation(instancedMeshProgram, "uFogDensity");
	instancedViewLoc = glGetUniformLocation(instancedMeshProgram, "uView");
	instancedSunPosLoc = glGetUniformLocation(instancedMeshProgram, "uSunPos");
	instancedSkyAmbientLoc = glGetUniformLocation(instancedMeshProgram, "uSkyAmbient");
	instancedSkyDiffuseLoc = glGetUniformLocation(instancedMeshProgram, "uSkyDiffuse");
	instancedMatAmbientLoc = glGetUniformLocation(instancedMeshProgram, "uMatAmbient");
	instancedMatDiffuseLoc = glGetUniformLocation(instancedMeshProgram, "uMatDiffuse");
	instancedMatEmissiveLoc = glGetUniformLocation(instancedMeshProgram, "uMatEmissive");
	instancedHasTextureLoc = glGetUniformLocation(instancedMeshProgram, "uHasTexture");
	instancedSphereMapLoc = glGetUniformLocation(instancedMeshProgram, "uSphereMap");
	instancedTextureLoc = glGetUniformLocation(instancedMeshProgram, "uTexture");
	glUseProgram(instancedMeshProgram);
	glUniform1i(instancedTextureLoc, 0);
	treeSunPosLoc = glGetUniformLocation(treeProgram, "uSunPos");
	treeSkyAmbientLoc = glGetUniformLocation(treeProgram, "uSkyAmbient");
	treeSkyDiffuseLoc = glGetUniformLocation(treeProgram, "uSkyDiffuse");

	pointProgram = linkProgram(kPointVertexShader, kPointFragmentShader);
	pointMvpLoc = glGetUniformLocation(pointProgram, "uMVP");
	pointColorLoc = glGetUniformLocation(pointProgram, "uColor");
	pointSizeLoc = glGetUniformLocation(pointProgram, "uPointSize");

	// M6 effects. Both buffers are refilled every frame from the live
	// particle/beam lists, hence GL_DYNAMIC_DRAW and no initial allocation.
	particleProgram = linkProgram(kParticleVertexShader, kParticleFragmentShader);
	particleMvpLoc = glGetUniformLocation(particleProgram, "uMVP");
	particleFogColorLoc = glGetUniformLocation(particleProgram, "uFogColor");
	particleFogDensityLoc = glGetUniformLocation(particleProgram, "uFogDensity");
	particleSpritesLoc = glGetUniformLocation(particleProgram, "uSprites");
	glGenVertexArrays(1, &particleVao);
	glBindVertexArray(particleVao);
	glGenBuffers(1, &particleVbo);
	glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void *) (3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void *) (5 * sizeof(float)));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void *) (6 * sizeof(float)));

	// Beams reuse the sight program (position + rgb), so the layout must
	// match what that shader declares.
	glGenVertexArrays(1, &beamVao);
	glBindVertexArray(beamVao);
	glGenBuffers(1, &beamVbo);
	glBindBuffer(GL_ARRAY_BUFFER, beamVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) (3 * sizeof(float)));
	glBindVertexArray(0);

	// Shield bubbles and parachutes - static shapes, uploaded once.
	uploadPosNormal(sphereVao, sphereVbo, buildSphere(12, 16, 0));
	sphereVertexCount = 12 * 16 * 6;
	uploadPosNormal(hemiVao, hemiVbo, buildSphere(12, 16, 6));
	hemiVertexCount = 6 * 16 * 6;
	uploadPosNormal(cubeVao, cubeVbo, buildCube());
	cubeVertexCount = 36;
	uploadPosNormal(chuteVao, chuteVbo, buildParachuteCanopy(12));
	chuteVertexCount = 12 * 3;
	uploadPosNormal(chuteCordVao, chuteCordVbo, buildParachuteCords(8));
	chuteCordVertexCount = 8 * 2;

	particles.clear();
	beams.clear();
	lastFrameSeconds = 0.0;

	// Everything built once and cached must be forgotten here, because this
	// function runs again whenever the EGL context is recreated - which
	// happens every time the app is minimised and resumed, not only at
	// startup. Every VAO, VBO and texture named below belonged to the
	// context that has just gone away.
	//
	// This was a real bug: only terrain, the ground texture and the model
	// cache were reset, so the tree geometry kept its stale handles and drew
	// garbage polygons across the battlefield after every resume - light
	// grey on a snow map, dark green on a tropical one, which is what
	// identified them as the trees. They survived a new round too, because
	// nothing but a context recreate rebuilds them.
	//
	// The handles are *zeroed, never deleted*. A fresh context reissues
	// names from 1, so calling glDeleteBuffers on a stale name here would
	// destroy an unrelated object that happens to have been given the same
	// number - a far nastier bug than the one being fixed.
	terrainBuilt = false;
	terrainVao = terrainVbo = terrainIbo = 0;
	groundTextureBuilt = false;
	detailTexture = 0;
	spriteArrayTexture = 0;
	groundGeneration++;
	groundTexture = 0;
	groundLightBaked = false;
	movementOverlayPainted = false;
	paintedMovementVersion = 0;

	g_treeKinds.clear();
	for (int i = 0; i < ScorchDroidTrees::eAtlasCount; i++) g_treeAtlases[i] = 0;

	waterBuilt = false;
	waterVisible = false;
	waterGridVao = waterGridVbo = waterGridEbo = waterFoamMaskTexture = 0;
	waterGridBuiltForDetail = -1;
	breakerVao = breakerVbo = breakerTexture[0] = breakerTexture[1] = 0;
	breakerVertexCount[0] = breakerVertexCount[1] = 0;

	skyBuilt = false;
	// Not skyVao/skyVbo: unlike everything else in this block they are
	// created unconditionally above in this same function, and zeroing
	// them here threw the fresh handles away - every sky draw then bound
	// buffer 0, failed, and the sky was the clear colour.

	cloudsBuilt = false;
	cloudsVisible = false;
	cloudVao = cloudVbo = cloudTexture = starTexture = sunTexture = 0;

	roofBuilt = false;
	roofVisible = false;
	roofVao = roofVbo = roofIbo = roofTexture = roofSkirtVao = roofSkirtVbo = 0;

	surroundBuilt = false;
	surroundVisible = false;
	surroundVao = surroundVbo = surroundTexture = 0;

	g_modelCache.clear();
	g_modelTextures.clear();
	// Same reason as the model cache above: these name GL objects belonging
	// to the context that has just gone away.
	g_instancedDraws.clear();

	glGenVertexArrays(1, &pointVao);
	glBindVertexArray(pointVao);
	glGenBuffers(1, &pointVbo);
	glBindBuffer(GL_ARRAY_BUFFER, pointVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
	glBindVertexArray(0);

	// Every draw is guarded by `program != 0`, so a program that fails to
	// link doesn't crash - the thing it drew just quietly stops appearing,
	// and looks like a shading bug rather than a missing draw. That cost a
	// while with the water (a uTime precision mismatch across the two
	// stages), so say so once, plainly, at the point it happens.
	const struct { const char *name; GLuint program; } programs[] = {
		{ "terrain", terrainProgram }, { "shadow", shadowProgram },
		{ "sprite", spriteProgram },   { "cloud", cloudProgram },
		{ "sky", skyProgram },         { "water", waterProgram },
		{ "sight", sightProgram },     { "mesh", meshProgram },
		{ "point", pointProgram },     { "particle", particleProgram },
		{ "instanced-mesh", instancedMeshProgram },
		{ "tree", treeProgram },
	};
	for (const auto &p : programs) {
		if (p.program == 0) LOGE("Shader program '%s' FAILED to link - it will draw nothing", p.name);
	}

	glClearColor(0.5f, 0.65f, 0.85f, 1.0f);  // sky
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeOnSurfaceChanged(JNIEnv *, jobject, jint width, jint height) {
	surfaceWidth = width;
	surfaceHeight = height;
	glViewport(0, 0, width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeOnDrawFrame(JNIEnv *, jobject) {
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	{
		// Publish last frame's total before starting this one's count, so
		// the HUD always shows a complete frame rather than a partial one.
		std::lock_guard<std::mutex> lock(g_statsMutex);
		lastFrameDrawCalls = frameDrawCalls;
	}
	frameDrawCalls = 0;

	std::lock_guard<std::mutex> lock(g_engineMutex);
	ScorchedContext *ctx = engineActiveContext();
	if (!ctx) return;

	buildTerrainIfNeeded(*ctx);
	if (!terrainBuilt) return;
	buildGroundTextureIfNeeded(*ctx);
	// After the terrain, which is where the map size it spans comes from.
	buildWaterIfNeeded(*ctx);
	// After the terrain too - the roof is sampled onto the same grid and
	// reuses the world-coordinate tables the terrain build fills in.
	buildRoofIfNeeded(*ctx);
	// Also after the terrain, for the map size it spans.
	buildSurroundIfNeeded(*ctx);
	buildSkyIfNeeded(*ctx);
	buildCloudsIfNeeded(*ctx);
	applyTerrainDeformations(*ctx);
	applyScorchMarks(*ctx);
	syncMovementOverlay();

	// M6 effects. Real elapsed time rather than a fixed step, so particles
	// age correctly whatever the frame rate; clamped so that a stall (a
	// landscape rebuild, the app resuming) doesn't teleport every live
	// particle to the end of its life in one frame.
	{
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		double nowSeconds = (double) now.tv_sec + (double) now.tv_nsec / 1e9;
		float delta = (lastFrameSeconds > 0.0) ? (float) (nowSeconds - lastFrameSeconds) : 0.0f;
		lastFrameSeconds = nowSeconds;
		if (delta < 0.0f) delta = 0.0f;
		if (delta > 0.1f) delta = 0.1f;
		advanceClouds(*ctx, delta);
		// WaterWaves::simulate: half speed, wrapping at 6 - a 12 s cycle.
		breakerTime += delta / 2.0f;
		if (breakerTime > 6.0f) breakerTime = 0.0f;
		if (skyFlashRemaining > 0.0f) {
			skyFlashRemaining = std::max(0.0f, skyFlashRemaining - delta);
		}

		// Frame rate, smoothed. An instantaneous 1/delta jitters far too
		// much to read on a phone; this settles in about a second while
		// still reacting to a real stall.
		if (delta > 0.0001f) {
			const float instant = 1.0f / delta;
			std::lock_guard<std::mutex> lock(g_statsMutex);
			smoothedFps = (smoothedFps <= 0.0f)
				? instant
				: (smoothedFps * 0.9f + instant * 0.1f);
		}

		// How many of upstream's fixed simulation steps this frame is worth,
		// so the trail has its density rather than the frame rate's (see
		// emitProjectileTrail). Whole steps only; the remainder carries.
		trailStepAccumulator += delta;
		trailStepsThisFrame = (int) (trailStepAccumulator / kTrailStepSeconds);
		trailStepAccumulator -= (float) trailStepsThisFrame * kTrailStepSeconds;
		trailStepsThisFrame = std::min(trailStepsThisFrame, kMaxTrailStepsPerFrame);

		smokeAccumulator += delta;
		smokeEmitsThisFrame = (int) (smokeAccumulator / kSmokeIntervalSeconds);
		smokeAccumulator -= (float) smokeEmitsThisFrame * kSmokeIntervalSeconds;
		smokeEmitsThisFrame = std::min(smokeEmitsThisFrame, kMaxTrailStepsPerFrame);

		spawnEffects();
		emitTankSmoke(*ctx, delta);
		emitPrecipitation(delta);
		updateEffects(*ctx, delta);
	}

	HeightMap &heightMap = ctx->getLandscapeMaps().getGroundMaps().getHeightMap();
	int mapW = heightMap.getMapWidth();
	int mapH = heightMap.getMapHeight();
	if (mapW <= 0) mapW = 1;
	if (mapH <= 0) mapH = 1;

	// Gather tank positions first (needed both for drawing and, in follow
	// mode, as the camera's target) - split by ownership so "my tank" is
	// visually distinct from everyone else's, same as the M5-era 2D view.
	unsigned int myDestinationId = engineMyDestinationId();
	std::map<unsigned int, Tank *> &tanks = ctx->getTargetContainer().getTanks();
	// M6: tanks are real .ase models now (see uploadModel) rather than
	// coloured point sprites. Each one still gets a tint so "my tank"
	// stays instantly identifiable, which the point sprites were good at
	// and a uniformly-coloured model would lose.
	struct TankInstance {
		float x, y, z;
		float headingRadians;
		float elevationRadians;
		// Which way the hull faces. Upstream turns it as the tank drives
		// (TanketMovement sets the target's rotation to the bearing of each
		// step it takes) and it is quite separate from headingRadians,
		// which is where the *gun* points - a tank can drive east while
		// aiming north.
		float hullYawRadians;
		bool mine;
		bool alive;
		// Tank::getVisible() - alive, or shopping in the buying phase.
		// Upstream's TargetRendererImplTank::drawParticle bails on this
		// before it draws anything at all, so a destroyed tank gets no
		// name plate either; a tank that is visible but not sNormal (i.e.
		// buying) gets its name but no life bar. Kept separate from
		// `alive` because the two differ exactly during the buying phase.
		bool visible;
		// Rotation that stands the hull on the slope beneath it, from the
		// landscape normal. See where it is built for why only the hull
		// gets it.
		Mat4 groundTilt;
		Model *model;
		// Name-plate data, filled here and projected to screen space once
		// the MVP exists further down.
		std::string name;
		float life;
		float shield;
		float colorR, colorG, colorB;
		// Shield bubble (upstream's TargetRendererImpl::drawShield) and
		// parachute (drawParachute). shieldRound distinguishes a sphere
		// from a box; shieldHalf is upstream's half-shield hemisphere.
		bool  hasShield;
		bool  shieldRound;
		bool  shieldHalf;
		float shieldRadius;                        // round shields
		float shieldX, shieldY, shieldZ;           // square shields (half-extents)
		float shieldR, shieldG, shieldB;
		bool  parachuteOpen;
	};
	std::vector<TankInstance> tankInstances;

	// M6: everything on the landscape that isn't a tank - trees, buildings,
	// ships. These are ordinary Targets in the same container; the model to
	// draw each one with comes from the hook in patch 0013 (see
	// TargetModelStore.h), because upstream computes it and hands it
	// straight to a client renderer this build doesn't have.
	struct TargetInstance {
		float x, y, z;
		float rotationRadians;
		float scale;
		std::string meshName;  // for the oversize diagnostic in the scenery pass
		float brightness;
		float shadowRadius;
		// Trees have no mesh (see buildTreeGeometryIfNeeded); they draw the
		// shared procedural one in these colours instead.
		bool  isTree;
		// Which of upstream's 28 tree types, when isTree.
		TreeModelFactory::TreeType treeType;
		float treeR, treeG, treeB;
		Model *model;
	};
	std::vector<TargetInstance> targetInstances;
	{
		int skipped = 0, nonTank = 0;
		std::map<unsigned int, Target *> &allTargets = ctx->getTargetContainer().getTargets();
		for (auto &entry : allTargets) {
			Target *target = entry.second;
			// Tanks are drawn below, with their turrets and name plates.
			if (target->getType() == Target::TypeTank) continue;
			nonTank++;
			// Same guard upstream's drawParticle opens with: a destroyed
			// target is gone, not drawn dark.
			if (!target->getVisible()) continue;

			ScorchDroidTargets::Info info;
			if (!ScorchDroidTargets::get(target->getPlayerId(), info)) { skipped++; continue; }

			// ModelID packs a tree as type "Tree", meshName "<B|N>:<kind>"
			// and skinName "S" for snow (see ModelID::initFromNode). The
			// mesh itself is empty, so these are drawn procedurally.
			const bool isTree = (0 == strcmp(info.model.getType(), "Tree"));
			Model *model = nullptr;
			if (!isTree) {
				model = loadModelSafely(info.model);
				if (!model) { skipped++; continue; }
			}

			FixedVector &pos = target->getLife().getTargetPosition();
			TargetInstance inst;
			inst.x = pos[0].asFloat();
			inst.z = worldZFromEngineY(pos[1].asFloat());
			// Targets carry a real height (they can be dropped, and they
			// fall when the ground under them goes), so use it rather than
			// re-sampling the heightmap the way tanks do.
			inst.y = pos[2].asFloat();
			// The *live* rotation, not the one the definition was created
			// with. Targets that move - the boid flocks of jets, the ships
			// on their splines - are turned every step by
			// setTargetPositionAndRotation() to face the way they are
			// going, and reading the creation value left them flying along
			// their path at whatever heading they happened to spawn with,
			// which for half a circuit looks exactly like flying backwards.
			//
			// TargetLife keeps it only as a quaternion (getFloatRotMatrix()
			// is another of the mirrors it maintains only when
			// !serverMode_, so it is identity here). The quaternion is
			// always a yaw about the engine's up axis, laid out
			// (w, x, y, z), so the angle comes straight back out of the w
			// and z components.
			FixedVector4 &quat = target->getLife().getQuaternion();
			inst.rotationRadians =
				2.0f * atan2f(quat[3].asFloat(), quat[0].asFloat());
			inst.scale = info.scale;
			inst.meshName = info.model.getMeshName();
			// Upstream multiplies the model by this grey ("color_", used as
			// glColor3f(c,c,c)), randomising it when the definition asks by
			// setting -1. But TargetDefinition's constructor never
			// initialises modelbrightness_, so a definition that doesn't
			// name one gets 0 rather than the -1 its own randomise check
			// looks for - which would draw the model pure black. Anything
			// non-positive is treated as "no tint" here; drawing scenery
			// black is never what the data meant.
			inst.brightness = (info.brightness > 0.0f) ? info.brightness : 1.0f;
			inst.isTree = isTree;
			inst.treeType = TreeModelFactory::eNone;
			if (isTree) {
				// Upstream's own decoding of the ModelID: the mesh name is
				// "<B|N>:<kind>" (B for the burnt version) and the skin name
				// starts with S for the snow-laden one, and
				// TreeModelFactory::getTypes - which is in src/common, so we
				// have it - maps that pair to one of its 28 types. This
				// replaces three guessed tint colours with the actual
				// species, which is what lets the right atlas cell be
				// sampled.
				const bool burnt = (info.model.getMeshName()[0] == 'B');
				const bool snow = (info.model.getSkinName()[0] == 'S');
				TreeModelFactory::TreeType normalType = TreeModelFactory::eNone;
				TreeModelFactory::TreeType burntType = TreeModelFactory::eNone;
				TreeModelFactory::getTypes(&info.model.getMeshName()[2], snow,
										   normalType, burntType);
				inst.treeType = burnt ? burntType : normalType;
				// Upstream tints only the burnt types, with a flat grey; the
				// rest take their colour from the atlas.
				const float tint = ScorchDroidTrees::isBurnt(inst.treeType) ? 0.3f : 1.0f;
				inst.treeR = inst.treeG = inst.treeB = tint;
			} else {
				inst.treeR = inst.treeG = inst.treeB = 1.0f;
			}
			// Upstream sizes a target's shadow from its own bounding size
			// (TargetRendererImplTarget::render: size.Max() + 2).
			inst.shadowRadius =
				target->getLife().getSize().Max().asFloat() * 0.5f + 0.5f;
			inst.model = model;
			targetInstances.push_back(inst);
		}

		// One line whenever the mix changes, which in practice is once per
		// landscape. "skipped" means the hook never recorded a model for a
		// live target, which would be a patch problem, not a data one.
		lastTargetsDrawn = (int) targetInstances.size();
		static size_t lastDrawn = (size_t) -1;
		static int lastSkipped = -1;
		if (targetInstances.size() != lastDrawn || skipped != lastSkipped) {
			lastDrawn = targetInstances.size();
			lastSkipped = skipped;
			LOGI("Landscape targets: %zu drawn, %d skipped (no model), %d non-tank total",
				 targetInstances.size(), skipped, nonTank);
		}
	}

	std::vector<float> myTankPositions, enemyTankPositions;  // fallback markers
	// Real in-flight shot/explosion positions, straight from the running
	// simulation's ActionController. Shots also report which tank fired
	// them, so each one can use that tank's own projectile model (see the
	// shotPlayerIds addition in patch 0009).
	//
	// Read here rather than at the draw site further down because the shot
	// camera needs them: it frames the projectile, so it has to know where
	// the projectile is before the view matrix is built.
	std::vector<FixedVector> shotPositionsRaw, explosionPositionsRaw;
	std::vector<unsigned int> shotPlayerIds;
	std::vector<FixedVector> shotVelocities;
	std::vector<WeaponProjectile *> shotWeapons;
	ctx->getActionController().getShotAndExplosionPositions(
		shotPositionsRaw, explosionPositionsRaw, &shotPlayerIds, &shotVelocities, &shotWeapons);

	bool haveMyTank = false;
	bool myTankAlive = false;
	unsigned int myPlayerId = 0;
	float myColorR = 1.0f, myColorG = 1.0f, myColorB = 1.0f;
	float myTankX = 0.0f, myTankY = 0.0f, myTankZ = 0.0f;
	float myTankYaw = 0.0f;
	for (auto &entry : tanks) {
		Tank *tank = entry.second;
		FixedVector &pos = tank->getLife().getTargetPosition();
		// heightAt samples the heightmap, so it wants landscape y; the
		// instance carries world Z.
		float x = pos[0].asFloat(), engineY = pos[1].asFloat();
		float z = worldZFromEngineY(engineY);
		float groundY = heightAt(heightMap, mapW, mapH, x, engineY);
		bool mine = (tank->getDestinationId() == myDestinationId);

		// Sit the tank on the slope rather than axis-aligned on top of it.
		// Upstream doesn't do this - TargetLife's rotation is a yaw about
		// the world up axis and nothing else, so its tanks stay upright on
		// any hillside - but the landscape normal it maintains for lighting
		// is right there, and a tank bedded into the hill reads far better
		// on a small screen than one apparently hovering at one corner.
		//
		// Deliberately the hull only (see the draw loop): the turret and
		// gun carry the firing bearing and elevation, which are world-space
		// angles that the ground has no say in. Tilting those would put the
		// drawn barrel back out of step with where the shot actually goes,
		// which is a bug this port has already paid for more than once.
		Mat4 groundTilt = Mat4::identity();
		{
			FixedVector &normal = ctx->getLandscapeMaps().getGroundMaps().getNormal(
				std::min(std::max((int) x, 0), mapW - 1),
				std::min(std::max((int) engineY, 0), mapH - 1));
			// Landscape normal (x, y, up) -> world (x, up, -y), the same
			// mapping positions take.
			const float nx = normal[0].asFloat();
			const float ny = normal[2].asFloat();
			const float nz = -normal[1].asFloat();
			const float len = sqrtf(nx * nx + ny * ny + nz * nz);
			if (len > 1e-5f) {
				const float ux = nx / len, uy = ny / len, uz = nz / len;
				// Axis = worldUp x normal, angle = the one between them.
				const float axisX = uz, axisZ = -ux;
				if (sqrtf(axisX * axisX + axisZ * axisZ) > 1e-5f) {
					groundTilt = Mat4::rotateAxis(axisX, 0.0f, axisZ,
						acosf(std::min(1.0f, std::max(-1.0f, uy))));
				}
			}
		}

		Model *model = nullptr;
		TankModel *tankModel = tank->getModelContainer().getTankModel();
		if (tankModel) model = loadModelSafely(tankModel->getTankModelID());

		// The turret angle is the engine's own bearing, and it goes into
		// rotateY as-is. TankLib::getVelocityVector fires along
		// (-sin(xy), cos(xy)) in landscape axes; the model's forward axis
		// is landscape +y, which the upload maps to world -Z, and
		// rotateY(xy) turns that to (-sin(xy), 0, -cos(xy)) - the same
		// direction once world Z is read back through engineYFromWorldZ.
		//
		// This was negated for a long time, along with the gun's elevation
		// below, because the landscape-to-world map was a reflection and
		// reversed both. Both negations went when the map was corrected;
		// re-adding one here would put the barrel back out of step with the
		// shot.
		float heading = tank->getShotInfo().getRotationGunXY().asFloat() * (float) M_PI / 180.0f;
		float elevation = tank->getShotInfo().getRotationGunYZ().asFloat() * (float) M_PI / 180.0f;
		// The hull's own bearing, from the same place a moving target's
		// comes from: TargetLife keeps it only as a quaternion, a yaw about
		// the engine's up axis laid out (w, x, y, z). Zeroed when the tank
		// stops (TanketMovement setRotation(0)), so a parked tank faces the
		// way upstream parks it.
		FixedVector4 &tankQuat = tank->getLife().getQuaternion();
		float hullYaw = 2.0f * atan2f(tankQuat[3].asFloat(), tankQuat[0].asFloat());
		bool alive = (tank->getState().getState() == TankState::sNormal);
		bool visible = tank->getVisible();

		// Name plate / health bar inputs. Life is a straight fraction of
		// max; shield is the fraction of the raised shield's own power, or
		// zero when none is up - matching what upstream's drawLife shows as
		// its second bar.
		TargetLife &life = tank->getLife();
		float lifeFraction = (life.getMaxLife() > fixed(0))
			? (life.getLife() / life.getMaxLife()).asFloat() : 0.0f;
		float shieldFraction = 0.0f;
		if (Accessory *currentShield = tank->getShield().getCurrentShield()) {
			Shield *shieldAction = (Shield *) currentShield->getAction();
			if (shieldAction && shieldAction->getPower() > fixed(0)) {
				shieldFraction = (tank->getShield().getShieldPower() /
								  shieldAction->getPower()).asFloat();
			}
		}
		Vector &tankColor = tank->getColor();

		// Shield bubble geometry, straight off the raised shield's own
		// accessory - upstream reads exactly these (getRound /
		// getActualRadius / getHalfShield / getSize / getColor).
		bool hasShield = false, shieldRound = true, shieldHalf = false;
		float shieldRadius = 0.0f, shieldX = 0.0f, shieldY = 0.0f, shieldZ = 0.0f;
		float shieldR = 1.0f, shieldG = 1.0f, shieldB = 1.0f;
		if (Accessory *shieldAcc = tank->getShield().getCurrentShield()) {
			if (Shield *shieldAction = (Shield *) shieldAcc->getAction()) {
				hasShield = true;
				Vector &sc = shieldAction->getColor();
				shieldR = sc[0]; shieldG = sc[1]; shieldB = sc[2];
				shieldRound = shieldAction->getRound();
				if (shieldRound) {
					ShieldRound *roundShield = (ShieldRound *) shieldAction;
					shieldRadius = roundShield->getActualRadius().asFloat();
					shieldHalf = roundShield->getHalfShield();
				} else {
					FixedVector &size = ((ShieldSquare *) shieldAction)->getSize();
					// Half-extents, not a position: the axes swap the same
					// way but the sign the position map carries doesn't
					// matter for a symmetric box.
					shieldX = size[0].asFloat();
					shieldY = size[2].asFloat();
					shieldZ = size[1].asFloat();
				}
			}
		}

		// Upstream shows a parachute only while actually falling *with* one
		// deployed - not merely for owning them.
		bool parachuteOpen = false;
		if (TargetFalling *falling = tank->getTargetState().getFalling()) {
			parachuteOpen = (falling->getParachute() != nullptr);
			// Same reasoning as the shield-hit log above: a parachute is
			// only drawn while a tank is actually falling with one bought,
			// which is a second or two per round at most.
			if (parachuteOpen && !loggedParachute) {
				loggedParachute = true;
				LOGI("Parachute drawn for a falling tank");
			}
		}

		tankInstances.push_back({
			x, groundY, z, heading, elevation, hullYaw, mine, alive, visible, groundTilt, model,
			LangStringUtil::convertFromLang(tank->getTargetName()),
			std::min(std::max(lifeFraction, 0.0f), 1.0f),
			std::min(std::max(shieldFraction, 0.0f), 1.0f),
			tankColor[0], tankColor[1], tankColor[2],
			hasShield, shieldRound, shieldHalf,
			shieldRadius, shieldX, shieldY, shieldZ,
			shieldR, shieldG, shieldB,
			parachuteOpen,
		});

		float markerY = groundY + 1.5f;
		if (mine) {
			myTankPositions.push_back(x); myTankPositions.push_back(markerY); myTankPositions.push_back(z);
			haveMyTank = true;
			myTankAlive = alive;
			myPlayerId = tank->getPlayerId();
			myColorR = tankColor[0]; myColorG = tankColor[1]; myColorB = tankColor[2];
			myTankX = x; myTankY = markerY; myTankZ = z;
			// The turret bearing, for the camera presets that frame the
			// tank from behind/above it (see the preset block below).
			myTankYaw = heading;
		} else {
			enemyTankPositions.push_back(x); enemyTankPositions.push_back(markerY); enemyTankPositions.push_back(z);
		}
	}

	// M20: the shot camera's framing, upstream's own (TargetCamera::CamShot
	// with TankViewPoints::getValues).
	//
	// Upstream builds this out of view points: each projectile registers one
	// carrying its position and a "look from" vector, and the camera averages
	// the positions into what it looks at and sums the vectors into where it
	// looks from. The arithmetic is reproduced here rather than the plumbing,
	// because this port already reads every live shot off the action
	// controller each frame - the view-point machinery exists to carry that
	// information from the simulation to a camera that cannot see it, and
	// this one can.
	//
	// The look-from vector is the shot's own velocity reversed with its
	// vertical component forced to a constant +10, which is what puts the
	// camera behind and above the shell looking along its flight - and what
	// stops it flipping over the top of the arc, where the real vertical
	// velocity changes sign.
	bool haveShot = false;
	float shotCamTargetX = 0.0f, shotCamTargetY = 0.0f, shotCamTargetZ = 0.0f;
	float shotCamOffsetX = 0.0f, shotCamOffsetY = 1.0f, shotCamOffsetZ = 0.0f;
	// Upstream watches *your* shots and only borrows someone else's when you
	// have none in the air (TargetCamera::CamShot's fallback through
	// TankViewPointsTanks). Without that a bot firing at the same moment -
	// which in a simultaneous game is every moment - drags the camera off
	// your own shell.
	for (int pass = 0; pass < 2 && !haveShot; pass++) {
		const bool mineOnly = (pass == 0);
		if (mineOnly && !haveMyTank) continue;
		float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
		float fromX = 0.0f, fromY = 0.0f, fromZ = 0.0f;
		float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
		float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
		int tracked = 0;
		for (size_t i = 0; i < shotPositionsRaw.size(); i++) {
			// Upstream's own opt-out: a weapon with <nocameratrack> registers
			// no view point, so the camera never follows it.
			if (i < shotWeapons.size() && shotWeapons[i] &&
				shotWeapons[i]->getNoCameraTrack()) {
				continue;
			}
			if (mineOnly &&
				(i >= shotPlayerIds.size() || shotPlayerIds[i] != myPlayerId)) {
				continue;
			}
			FixedVector &p = shotPositionsRaw[i];
			const float wx = p[0].asFloat();
			const float wy = p[2].asFloat();
			const float wz = worldZFromEngineY(p[1].asFloat());
			sumX += wx; sumY += wy; sumZ += wz;
			if (tracked == 0) {
				minX = maxX = wx; minY = maxY = wy; minZ = maxZ = wz;
			} else {
				minX = std::min(minX, wx); maxX = std::max(maxX, wx);
				minY = std::min(minY, wy); maxY = std::max(maxY, wy);
				minZ = std::min(minZ, wz); maxZ = std::max(maxZ, wz);
			}
			if (i < shotVelocities.size()) {
				FixedVector &v = shotVelocities[i];
				// Engine (x, y, up) to world (x, up, -y), reversed
				// horizontally, with the vertical pinned as upstream pins it.
				fromX += -v[0].asFloat();
				fromY += 10.0f;
				fromZ += v[1].asFloat();
			}
			tracked++;
		}

		if (tracked > 0) {
			haveShot = true;
			shotCamTargetX = sumX / (float) tracked;
			shotCamTargetY = sumY / (float) tracked;
			shotCamTargetZ = sumZ / (float) tracked;

			// Upstream's distance: far enough back to hold everything it is
			// watching, which for a cluster weapon spreads with the cluster.
			// The radius of one is 1, so the box is inflated by that first.
			const float spanX = (maxX - minX) + 2.0f;
			const float spanY = (maxY - minY) + 2.0f;
			const float spanZ = (maxZ - minZ) + 2.0f;
			const float span = sqrtf(spanX * spanX + spanY * spanY + spanZ * spanZ);
			const float distance = span > 0.0f ? span + 25.0f : 25.0f;

			const float length = sqrtf(fromX * fromX + fromY * fromY + fromZ * fromZ);
			if (length > 0.0001f) {
				shotCamOffsetX = fromX / length * distance;
				shotCamOffsetY = fromY / length * distance;
				shotCamOffsetZ = fromZ / length * distance;
			} else {
				shotCamOffsetX = 0.0f;
				shotCamOffsetY = distance;
				shotCamOffsetZ = 0.0f;
			}
		} else if (!mineOnly && !explosionPositionsRaw.empty()) {
			// Nothing in the air, but something is going off: upstream keeps
			// the camera on it, because the explosion registers a view point
			// of its own and the action outlives the shell that caused it.
			// This is what stops the view cutting away at the exact moment
			// the shot arrives.
			FixedVector &p = explosionPositionsRaw[0];
			haveShot = true;
			shotCamTargetX = p[0].asFloat();
			shotCamTargetY = p[2].asFloat();
			shotCamTargetZ = worldZFromEngineY(p[1].asFloat());
			// No velocity to look along, so keep the bearing the shot left
			// behind and simply stand off it.
			shotCamOffsetX = sinf(g_camera.shotYaw) * 30.0f;
			shotCamOffsetY = 20.0f;
			shotCamOffsetZ = cosf(g_camera.shotYaw) * 30.0f;
		}
	}

	// Camera: free-fly orbits the map center; follow mode retargets to "my
	// tank"'s live position every frame instead (falling back to free-fly
	// framing if there's no tank yet - e.g. still spectating/loading) - see
	// the porting plan's M6 entry for why both modes exist. Yaw/pitch are
	// shared between modes so switching modes mid-look doesn't jar the view
	// around; only the target and remembered distance differ.
	float eyeX, eyeY, eyeZ, targetX, targetY, targetZ;
	{
		std::lock_guard<std::mutex> camLock(g_cameraMutex);

		// First frame of a round that has a tank to look at: point the
		// free-fly target at it. Done even while in follow mode, so that
		// switching to free-fly later starts from your tank rather than
		// from wherever the previous round left the target.
		// Gated on the tank being *alive*, not merely present. At the
		// instant a new landscape appears the tank still carries its
		// previous round's position - placement runs later, in
		// ServerTankNewGameState, after buying. Consuming the flag on mere
		// presence pointed the camera at where the tank used to be last
		// round and then cleared itself, which is why this appeared to work
		// on the very first landscape and never again.
		if (recentreOnMyTank && haveMyTank && myTankAlive) {
			recentreOnMyTank = false;
			g_camera.targetX = myTankX;
			g_camera.targetY = myTankY;
			g_camera.targetZ = myTankZ;
			// Pull in from the whole-map framing at the same time. Centred
			// on your tank but still zoomed out far enough to see the whole
			// map, the tank is a couple of pixels - centred on something
			// invisible. A third of the map across shows it, the ground it
			// is on, and plenty of the battlefield; pinch still goes wider.
			g_camera.orbitDistance = std::max(mapWidthUnits, mapHeightUnits) * 0.35f;
		}

		bool useFollow = g_camera.followMode && haveMyTank;
		targetX = useFollow ? myTankX : g_camera.targetX;
		targetY = useFollow ? myTankY : g_camera.targetY;
		targetZ = useFollow ? myTankZ : g_camera.targetZ;
		float distance = g_camera.followMode ? g_camera.followDistance : g_camera.orbitDistance;
		float pitch = g_camera.pitch;

		// M6 parity: the fixed presets. Each overrides the framing for this
		// frame only - the drag-controlled yaw/pitch/distance are left
		// untouched underneath, so switching back to Free or Follow restores
		// exactly the view the player had set up.
		if (g_camera.preset >= OrbitCamera::pTop && haveMyTank) {
			targetX = myTankX;
			targetY = myTankY;
			targetZ = myTankZ;

			// Upstream frames these relative to the tank's own turret
			// bearing, not the free camera's yaw, which is what makes
			// "behind the tank" mean behind *it* rather than behind where
			// you happened to be looking.
			float presetYaw = myTankYaw;
			switch (g_camera.preset) {
				case OrbitCamera::pTop:    pitch = 1.397f; distance = 50.0f; break;
				case OrbitCamera::pBehind: pitch = 0.571f; distance = 60.0f; break;
				case OrbitCamera::pTank:   pitch = 0.150f; distance = 15.0f; break;
				case OrbitCamera::pAction: pitch = 0.871f; distance = 80.0f; break;
				case OrbitCamera::pShot:
					// Upstream's CamShot watches the shot itself and drops
					// back to the behind-the-tank view when there is none.
					if (haveShot) {
						targetX = shotCamTargetX;
						targetY = shotCamTargetY;
						targetZ = shotCamTargetZ;
						// The framing is an offset from the shell, not an
						// orbit around it, so it is converted into the
						// yaw/pitch/distance the eye is built from below.
						// Chasing along the flight is the whole point: an
						// orbit at a fixed bearing shows the shell from the
						// side and tells you nothing about where it is going.
						distance = sqrtf(
							shotCamOffsetX * shotCamOffsetX +
							shotCamOffsetY * shotCamOffsetY +
							shotCamOffsetZ * shotCamOffsetZ);
						if (distance > 0.0001f) {
							pitch = asinf(std::min(1.0f, std::max(-1.0f,
								shotCamOffsetY / distance)));
							presetYaw = atan2f(shotCamOffsetX, shotCamOffsetZ);
						}
						g_camera.shotYaw = presetYaw;
					} else {
						pitch = 0.571f;
						distance = 60.0f;
						g_camera.shotYaw = presetYaw;
					}
					break;
				default: break;
			}
			g_camera.yaw = presetYaw;
		}

		eyeX = targetX + distance * cosf(pitch) * sinf(g_camera.yaw);
		eyeY = targetY + distance * sinf(pitch);
		eyeZ = targetZ + distance * cosf(pitch) * cosf(g_camera.yaw);
	}

	// S6: in a cavern, keep the eye inside the room. Both clamps are a
	// no-op on the 33 landscapes with no roof - getRoofOn() is false there
	// and nothing below runs. This has to come before the ground clearance
	// below, because moving the eye horizontally can put it inside a hill.
	RoofMaps &roofMaps = ctx->getLandscapeMaps().getRoofMaps();
	const bool inCavern = roofMaps.getRoofOn();
	if (inCavern) {
		// Horizontally: past the map edge there is no roof mesh, only the
		// skirt hanging down from the boundary, so an eye out there is
		// behind the wall looking at the back of the cave. Keeping it over
		// the map keeps it in the room. (This is not what causes the
		// camera to end up buried in one of these maps' steep bowls - that
		// is the orbit sitting inside a terrain *wall*, which predates the
		// roof and which lifting the eye vertically cannot fix.)
		const float inset = kCameraGroundClearance;
		eyeX = std::min(std::max(eyeX, inset), mapWidthUnits - inset);
		eyeZ = std::min(std::max(eyeZ, inset), mapHeightUnits - inset);
	}

	// Pull the eye in until the target is actually visible. The clamps below
	// keep the eye out of the ground *at its own position*, which is a
	// different problem from a ridge standing between it and the tank - and
	// that is what actually goes wrong: measured on a cavern map, the eye
	// sat 6.4 units clear of the ground beneath it while the terrain 70% of
	// the way along the sightline was 25 units above the line, so the view
	// was a wall of magnified hillside with the tank somewhere behind it.
	// Lifting the eye cannot fix that; only shortening the sightline can.
	//
	// This is the standard third-person camera collision: march out from the
	// target along the ray to the desired eye, and stop at the first place
	// the ground rises through it.
	//
	// Not gated on caverns - a hill between the camera and the tank is just
	// as possible on an ordinary landscape, and this only ever engages when
	// the view is genuinely blocked.
	{
		// The sightline is aimed at the tank's body, not at the ground it
		// stands on. That matters more than it sounds: at a low pitch the
		// ray climbs only ~13 units over ~90 of distance, so a line from
		// ground level is within a couple of units of the ground for a long
		// way, and testing it against the eye's ground *clearance* marked it
		// blocked immediately and collapsed the camera onto the tank every
		// time. The clearance keeps the eye out of the dirt; it has no
		// business in the visibility test.
		const float fromY = targetY + kTankSightHeight;
		const float dx = eyeX - targetX, dy = eyeY - fromY, dz = eyeZ - targetZ;
		const float fullDistance = sqrtf(dx * dx + dy * dy + dz * dz);
		float allowed = 1.0f;
		// Only when the target is something that could be seen in the first
		// place. Free-fly aims at the middle of the map at the *mid* terrain
		// height, which on a mountainous map is inside a hill - the first
		// sample would then read as blocked and pin the camera at its
		// minimum distance for the whole round. Follow mode's target is a
		// tank sitting on the ground, so this passes.
		const bool targetVisible = fromY > terrainHeightAt(*ctx, targetX, targetZ);
		if (targetVisible && fullDistance > kMinOcclusionDistance) {
			const float startT = kMinOcclusionDistance / fullDistance;
			const int steps = 32;
			for (int i = 0; i <= steps; i++) {
				const float t = startT + (1.0f - startT) * ((float) i / (float) steps);
				const float sx = targetX + dx * t;
				const float sy = fromY + dy * t;
				const float sz = targetZ + dz * t;
				if (terrainHeightAt(*ctx, sx, sz) > sy) {
					// Blocked here, so the last clear sample is as far as
					// the camera can go.
					allowed = std::max(startT, t - (1.0f - startT) / (float) steps);
					break;
				}
			}
		}

		// Snap inward, ease outward. This cannot oscillate: `allowed` is
		// measured along the ray to the *desired* eye position, which is
		// recomputed from the orbit every frame and never from the pulled-in
		// one, so there is no feedback from the result back into the test.
		if (allowed < g_camera.occlusionFraction) {
			g_camera.occlusionFraction = allowed;
		} else {
			const float dt = std::min((float) (lastFrameSeconds - lastCameraSeconds), 0.1f);
			const float k = std::min(kOcclusionReleaseRate * std::max(dt, 0.0f), 1.0f);
			g_camera.occlusionFraction += (allowed - g_camera.occlusionFraction) * k;
		}
		lastCameraSeconds = lastFrameSeconds;

		eyeX = targetX + dx * g_camera.occlusionFraction;
		eyeY = fromY + dy * g_camera.occlusionFraction;
		eyeZ = targetZ + dz * g_camera.occlusionFraction;
	}


	// Keep the eye above ground. The pull-in above stops the *sightline*
	// crossing terrain; this is the backstop for the eye's own position,
	// which the pull-in can still leave close to a slope, and for the case
	// where the eye is already within kMinOcclusionDistance so the pull-in
	// declines to act at all.
	{
		float groundAtEye = terrainHeightAt(*ctx, eyeX, eyeZ);
		float minEyeY = groundAtEye + kCameraGroundClearance;
		if (eyeY < minEyeY) eyeY = minEyeY;
	}

	// ...and below the roof: the orbit rises as the pitch increases and
	// would otherwise climb through the ceiling, which is back-facing from
	// above and culled, so the view becomes "looking down into the cave
	// through an invisible lid".
	if (inCavern) {
		// Cell lookup rather than the interpolated one, for the same reason
		// terrainHeightAt uses it: a cell is a couple of world units and the
		// camera only needs to know it is under the roof.
		const int rx = (int) eyeX;
		const int ry = (int) engineYFromWorldZ(eyeZ);
		const float roofAtEye = roofMaps.getRoofHeight(rx, ry).asFloat();
		const float maxEyeY = roofAtEye - kCameraGroundClearance;
		// Never below the ground lift above: where the cavern pinches to
		// less than the clearance the two clamps cross over, and preferring
		// the ceiling there would bury the camera in the floor - much worse
		// than clipping the ceiling.
		const float floorEyeY = terrainHeightAt(*ctx, eyeX, eyeZ) + kCameraGroundClearance;
		if (eyeY > maxEyeY) eyeY = std::max(maxEyeY, floorEyeY);
	}

	float aspect = (float) surfaceWidth / (float) surfaceHeight;
	float farPlane = std::max(mapWidthUnits, mapHeightUnits) * 3.0f + 200.0f;
	Mat4 proj = Mat4::perspective(kFovYRadians, aspect, 1.0f, farPlane);
	Mat4 view = Mat4::lookAt(eyeX, eyeY, eyeZ, targetX, targetY, targetZ, 0.0f, 1.0f, 0.0f);
	Mat4 mvp = Mat4::multiply(proj, view);

	{
		// Publish the pick basis by reading it straight out of the view
		// matrix that was just drawn with, rather than deriving the same
		// vectors a second time. A view matrix's upper 3x3 is the world ->
		// eye rotation, so its rows are exactly the camera axes: row 0 is
		// right, row 1 is up, row 2 is -forward (Mat4 is column-major, so
		// row r of column c is m[c * 4 + r]).
		//
		// The second derivation this replaces had right and up both
		// negated - a 180 degree rotation of the screen about its centre,
		// so every tap picked the landscape point diametrically opposite
		// the one under the finger. With the tank near the middle of the
		// screen that reads as "the turret aims away from the tap", and it
		// is what made upstream's own autoAim expression look like it
		// needed a half-turn added (see aimAtPoint in engine_jni.cpp).
		// Two derivations of one basis can drift apart; one cannot.
		std::lock_guard<std::mutex> lock(g_pickMutex);
		g_pickCamera.valid = true;
		g_pickCamera.eyeX = eyeX; g_pickCamera.eyeY = eyeY; g_pickCamera.eyeZ = eyeZ;
		g_pickCamera.rightX = view.m[0]; g_pickCamera.rightY = view.m[4]; g_pickCamera.rightZ = view.m[8];
		g_pickCamera.upX    = view.m[1]; g_pickCamera.upY    = view.m[5]; g_pickCamera.upZ    = view.m[9];
		g_pickCamera.fwdX  = -view.m[2]; g_pickCamera.fwdY  = -view.m[6]; g_pickCamera.fwdZ  = -view.m[10];
		g_pickCamera.tanHalfFov = tanf(kFovYRadians * 0.5f);
		g_pickCamera.aspect = aspect;
	}

	// Shared with the passes after the land, which fog to the same colour.
	float fogColor[3];
	currentFogColor(fogColor);

	// W3: the sky's own layers - stars, the sun, and the clouds over both -
	// as one callable pass. Upstream's reflection draws these too: its
	// Landscape::drawWater calls sky_->drawBackdrop(true) *and*
	// sky_->drawLayers(), and leaving the second one out is why a reflected
	// sky here was a bare gradient under a clouded one.
	auto drawSkyLayersPass = [&](const Mat4 &vp, const Mat4 &viewMatrix) {
		// Stars first, then the sun, then the clouds over both. Stars share
		// the cloud plane and shader but never scroll - that is what
		// upstream's "skytexturestatic" means - and are drawn at upstream's
		// own 0.7 alpha.
		if (cloudsVisible && starTexture != 0 && cloudProgram != 0) {
			glUseProgram(cloudProgram);
			glUniformMatrix4fv(cloudMvpLoc, 1, GL_FALSE, vp.m);
			glUniform2f(cloudScrollLoc, 0.0f, 0.0f);
			glUniform1f(cloudTexScaleLoc, 1.0f / 700.0f);
			glUniform3f(cloudTintLoc, 1.0f, 1.0f, 1.0f);
			glUniform1f(cloudOpacityLoc, 0.7f);
			glUniform3f(cloudFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
			glUniform1f(cloudFogDensityLoc, 0.0f);   // upstream draws the stars with fog off
			glUniform3f(cloudEyePosLoc, g_pickCamera.eyeX, g_pickCamera.eyeY, g_pickCamera.eyeZ);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, starTexture);
			glUniform1i(cloudSamplerLoc, 0);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);
			glDisable(GL_CULL_FACE);
			glBindVertexArray(cloudVao);
			frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, 6);
			glEnable(GL_CULL_FACE);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
			glBindVertexArray(0);
		}

		// Billboarded against the basis it is given, so the mirrored pass
		// gets a sun square-on to the mirrored camera rather than one edge-on.
		drawSunSprite(vp, viewMatrix);

		// Clouds sit between the sky and everything solid. Blended, and with
		// no depth writes, so terrain drawn afterwards always wins - the layer
		// is above the world but is not something you can hide behind.
		if (cloudsVisible && cloudProgram != 0) {
			glUseProgram(cloudProgram);
			glUniformMatrix4fv(cloudMvpLoc, 1, GL_FALSE, vp.m);
			glUniform2f(cloudScrollLoc, cloudScrollX, cloudScrollY);
			// One tile per 400 units - big enough that the repeat isn't the
			// first thing the eye finds.
			glUniform1f(cloudTexScaleLoc, 1.0f / 400.0f);
			// Tinted by the sun so a night map's clouds aren't daylit.
			glUniform3f(cloudTintLoc,
						0.4f + skyDescription.sunColor[0] * 0.6f,
						0.4f + skyDescription.sunColor[1] * 0.6f,
						0.4f + skyDescription.sunColor[2] * 0.6f);
			glUniform1f(cloudOpacityLoc, 0.75f);
			glUniform3f(cloudFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
			glUniform1f(cloudFogDensityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);
			glUniform3f(cloudEyePosLoc, g_pickCamera.eyeX, g_pickCamera.eyeY, g_pickCamera.eyeZ);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, cloudTexture);
			glUniform1i(cloudSamplerLoc, 0);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);
			glDisable(GL_CULL_FACE);
			glBindVertexArray(cloudVao);
			frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, 6);
			glEnable(GL_CULL_FACE);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
			glBindVertexArray(0);
		}
	};

	auto drawLandPass = [&](const Mat4 &vp, bool clipUnderwater) {
		glUseProgram(terrainProgram);
		// Set after the program is bound, not before - a uniform written
		// against whatever happened to be current goes nowhere.
		glUniform1i(terrainClipEnabledLoc, clipUnderwater ? 1 : 0);
		glUniform1f(terrainClipBelowLoc, waterHeight);
		glUniformMatrix4fv(terrainMvpLoc, 1, GL_FALSE, vp.m);
		glUniform1f(terrainMinHeightLoc, terrainMinHeight);
		glUniform1f(terrainHeightRangeLoc, terrainMaxHeight - terrainMinHeight);
		glUniform3f(terrainLightDirLoc, 0.4f, 0.82f, 0.35f);
		glUniform1i(terrainHasTextureLoc, groundTexture != 0 ? 1 : 0);
		glUniform1i(terrainLightBakedLoc, groundLightBaked ? 1 : 0);
		glUniform1i(terrainHalfLambertLoc, 0);
		glUniform3f(terrainFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
		glUniform1f(terrainFogDensityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);
		// The sun's shadow map. False while the map is itself being drawn,
		// which is what keeps that pass from sampling the texture it writes.
		glUniform1i(terrainShadowEnabledLoc, shadowValid ? 1 : 0);
		glUniformMatrix4fv(terrainShadowMatrixLoc, 1, GL_FALSE, shadowMatrix.m);
		glUniform1i(terrainShadowTexLoc, 5);
		// G2: unit 8, bound once for the whole pass since the ground, the
		// surround and the roof all blend the same image.
		glUniform1i(terrainHasDetailLoc, detailTexture != 0 ? 1 : 0);
		if (detailTexture != 0) {
			glActiveTexture(GL_TEXTURE8);
			glBindTexture(GL_TEXTURE_2D, detailTexture);
			glUniform1i(terrainDetailTexLoc, 8);
			glActiveTexture(GL_TEXTURE0);
		}
		glUniform3f(terrainAmbienceLoc, skyDescription.ambience[0],
					skyDescription.ambience[1], skyDescription.ambience[2]);
		glUniform3f(terrainDiffuseLoc, skyDescription.diffuse[0],
					skyDescription.diffuse[1], skyDescription.diffuse[2]);
		// The sun's position, in render axes (Sun::setPosition's point 900
		// units out, which is also where the shadow map is cast from).
		glUniform3f(terrainSunPosLoc,
					skyDescription.sunPosition[0],
					skyDescription.sunPosition[2],
					worldZFromEngineY(skyDescription.sunPosition[1]));
		if (groundTexture != 0) {
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, groundTexture);
			glUniform1i(terrainGroundTexLoc, 0);
		}
		glBindVertexArray(terrainVao);
		frameDrawCalls++; glDrawElements(GL_TRIANGLES, terrainIndexCount, GL_UNSIGNED_INT, (void *) 0);

		// The land surround, through the same program while it is still bound.
		// Upstream lights it with the same half-lambert against the real sun it
		// uses for the roof (LandSurround::generateList), and its own light map
		// is never baked - it is flat ground at height 0, so there is nothing
		// for hills to shadow.
		if (surroundVisible) {
			glUniform1f(terrainMinHeightLoc, 0.0f);
			glUniform1f(terrainHeightRangeLoc, 1.0f);
			glUniform1i(terrainHasTextureLoc, 1);
			glUniform1i(terrainLightBakedLoc, 0);
			glUniform1i(terrainHalfLambertLoc, 1);
			glUniform3f(terrainLightDirLoc,
						skyDescription.sunDirection[0],
						skyDescription.sunDirection[2],
						-skyDescription.sunDirection[1]);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, surroundTexture);
			glUniform1i(terrainGroundTexLoc, 0);
			glBindVertexArray(surroundVao);
			frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, surroundVertexCount);
		}

		// S6: the cavern roof, through the same program while it is still bound.
		// Its light map is never baked (the ground's is generated per landscape;
		// there is no roof equivalent), so it takes the lit path - with the
		// half-lambert, since every normal on a ceiling points away from the sun.
		if (roofVisible) {
			glUniform1f(terrainMinHeightLoc, roofMinHeight);
			glUniform1f(terrainHeightRangeLoc, roofMaxHeight - roofMinHeight);
			glUniform1i(terrainHasTextureLoc, roofTexture != 0 ? 1 : 0);
			glUniform1i(terrainLightBakedLoc, 0);
			glUniform1i(terrainHalfLambertLoc, 1);
			// Upstream shades its roof against the real sun (SkyRoof::makeNormal
			// takes the direction to Sun::getPosition), not the fixed light the
			// terrain uses.
			glUniform3f(terrainLightDirLoc,
						skyDescription.sunDirection[0],
						skyDescription.sunDirection[2],
						-skyDescription.sunDirection[1]);
			if (roofTexture != 0) {
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, roofTexture);
				glUniform1i(terrainGroundTexLoc, 0);
			}
			glBindVertexArray(roofVao);
			frameDrawCalls++; glDrawElements(GL_TRIANGLES, roofIndexCount, GL_UNSIGNED_INT, (void *) 0);

			// The skirt that closes the cavern beyond the map edge. Culled, not
			// double-sided: the camera can orbit out past the wall or above it
			// where it has curved below the horizon, and a two-sided skirt then
			// fills the screen with a solid grey slab. Single-sided, straying
			// outside just makes it disappear, which degrades to the view
			// without a skirt at all rather than to an opaque wall.
			if (roofSkirtVertexCount > 0) {
				glBindVertexArray(roofSkirtVao);
				frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, roofSkirtVertexCount);
			}
		}
	};

	// Filled by the tank pass below: the markers for tanks with no model,
	// and where the aim sight goes once the tanks are drawn.
	std::vector<float> unmodelledMine, unmodelledOther;
	std::vector<float> unmodelledShots, explosionPositions;
	bool haveSight = false;
	Mat4 sightTransform = Mat4::identity();
	Mat4 sightBaseTransform = Mat4::identity();
	Mat4 sightBearingTransform = Mat4::identity();

	// W3: the scenery, as one callable pass, so the reflection can draw
	// it too. The instance buckets are rebuilt on each call rather than
	// hoisted and shared: it is a few hundred microseconds of vector
	// filling, it only happens twice when reflections are at their
	// fullest, and hoisting would have meant moving the whole block
	// above the water and changing the order the frame has always been
	// drawn in.
	//
	// [cullBelowY] is upstream's own reflection rule, from
	// RenderTargets::drawTargets: a target whose position is under the
	// waterline is skipped entirely, so a half-drowned tree does not appear
	// standing on the reflected surface.
	auto drawSceneryPass = [&](const Mat4 &vp, float cullBelowY = -1.0e9f) {
	glUseProgram(meshProgram);
		beginUpstreamModels();
		glUniform3f(meshFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
		glUniform1f(meshFogDensityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);

		// Landscape targets first: they are scenery, so they should be behind
		// everything that matters, and drawing them before the tanks keeps the
		// per-target colour uniform out of the tank loop's way.
		{
			// M6 performance: scenery is drawn instanced - one call per distinct
			// mesh rather than one per target. A landscape scatters up to ~2,000
			// of these and, measured, uses a single model between them, so this
			// is the one pass in the renderer where instancing was worth having
			// (see InstanceBuffer.hpp).
			//
			// Bucketed by the source geometry's VBO rather than by Model*,
			// because the procedural trees have no Model at all and share this
			// path - they are simply another bucket.
			struct Bucket {
				int vertexCount = 0;
				const MeshGroup *group = nullptr;
				std::vector<ScorchDroidInstances::Instance> instances;
			};
			std::map<GLuint, Bucket> buckets;
			// Trees are bucketed by type rather than by VBO, because each type
			// has its own geometry *and* its own atlas cell.
			std::map<int, std::vector<ScorchDroidInstances::Instance> > treeBuckets;

			for (TargetInstance &inst : targetInstances) {
				if (inst.y < cullBelowY) continue;
				GLuint sourceVbo = 0;
				int vertexCount = 0;
				ScorchDroidInstances::Instance packed;
				packed.x = inst.x;
				packed.z = inst.z;
				packed.scale = inst.scale;
				packed.rotationRadians = inst.rotationRadians;

				if (inst.isTree) {
					// Trees have their own pass below - textured and alpha-cut,
					// which the flat scenery program cannot do.
					treeBuckets[(int) inst.treeType].push_back(packed);
					std::vector<ScorchDroidInstances::Instance> &treeBucket =
						treeBuckets[(int) inst.treeType];
					treeBucket.back().y = inst.y;
					treeBucket.back().r = inst.treeR * inst.brightness;
					treeBucket.back().g = inst.treeG * inst.brightness;
					treeBucket.back().b = inst.treeB * inst.brightness;
					continue;
				} else {
					GpuModel *gpu = uploadModel(inst.model);
					if (!gpu || gpu->hull.vertexCount == 0 || gpu->hull.vbo == 0) continue;
					// Diagnostic for the oversized object dan photographed:
					// anything that would draw more than 40 units across is
					// named once, with the numbers that made it that size.
					if (gpu->rawSize * inst.scale > 40.0f) {
						static std::set<std::string> reported;
						if (reported.insert(inst.meshName).second) {
							LOGI("Oversize target: %s raw %.1f x scale %.4f = %.1f units at (%.0f, %.0f, %.0f)",
								 inst.meshName.c_str(), gpu->rawSize, inst.scale,
								 gpu->rawSize * inst.scale, inst.x, inst.y, inst.z);
						}
					}
					sourceVbo = gpu->hull.vbo;
					vertexCount = gpu->hull.vertexCount;
					// The model's base lift, scaled by the definition's own
					// scale - folded into the instance here because the shader
					// has no idea which model it is drawing.
					packed.y = inst.y + gpu->baseOffset * inst.scale;
					// Upstream's "color" for a target is a grey multiplier,
					// randomised per target when the definition doesn't fix one,
					// so a stand of identical objects doesn't look stamped out.
					packed.r = packed.g = packed.b = inst.brightness;
				}

				Bucket &bucket = buckets[sourceVbo];
				bucket.vertexCount = vertexCount;
				bucket.group = &uploadModel(inst.model)->hull;
				bucket.instances.push_back(packed);
			}

			if (!buckets.empty() && instancedMeshProgram != 0) {
				glUseProgram(instancedMeshProgram);
				glUniformMatrix4fv(instancedViewProjLoc, 1, GL_FALSE, vp.m);
				glUniformMatrix4fv(instancedViewLoc, 1, GL_FALSE, g_passView.m);
				glUniform3f(instancedSunPosLoc,
							skyDescription.sunPosition[0],
							skyDescription.sunPosition[2],
							worldZFromEngineY(skyDescription.sunPosition[1]));
				glUniform3fv(instancedSkyAmbientLoc, 1, skyDescription.ambience);
				glUniform3fv(instancedSkyDiffuseLoc, 1, skyDescription.diffuse);
				glUniform3f(instancedFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
				glUniform1f(instancedFogDensityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);

				for (auto &entry : buckets) {
					const GLuint sourceVbo = entry.first;
					Bucket &bucket = entry.second;

					InstancedDraw &draw = g_instancedDraws[sourceVbo];
					if (draw.vao == 0) {
						glGenVertexArrays(1, &draw.vao);
						glGenBuffers(1, &draw.instanceVbo);
						glBindVertexArray(draw.vao);
						// The mesh's own vertices, same layout uploadMeshGroup
						// wrote them in.
						glBindBuffer(GL_ARRAY_BUFFER, sourceVbo);
						glEnableVertexAttribArray(0);
						glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kMeshFloats * sizeof(float), (void *) 0);
						glEnableVertexAttribArray(1);
						glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kMeshFloats * sizeof(float), (void *) (3 * sizeof(float)));
						glEnableVertexAttribArray(2);
						glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kMeshFloats * sizeof(float), (void *) (6 * sizeof(float)));
						// ...then the per-instance attributes, advancing once
						// per instance rather than once per vertex.
						const GLsizei stride = ScorchDroidInstances::kFloatsPerInstance * sizeof(float);
						glBindBuffer(GL_ARRAY_BUFFER, draw.instanceVbo);
						glEnableVertexAttribArray(3);
						glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void *) 0);
						glVertexAttribDivisor(3, 1);
						glEnableVertexAttribArray(4);
						glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void *) (4 * sizeof(float)));
						glVertexAttribDivisor(4, 1);
						glBindVertexArray(0);
					}

					std::vector<float> packed;
					ScorchDroidInstances::pack(bucket.instances, packed);

					// Upload only when something actually changed. Scenery is
					// static until a target is destroyed or the ground under it
					// gives way and it falls, so in the steady state this
					// uploads nothing - and comparing 30KB is far cheaper than
					// re-sending it every frame.
					if (packed != draw.uploaded) {
						glBindBuffer(GL_ARRAY_BUFFER, draw.instanceVbo);
						glBufferData(GL_ARRAY_BUFFER, packed.size() * sizeof(float),
									 packed.data(), GL_DYNAMIC_DRAW);
						draw.uploaded = packed;
					}
					draw.instanceCount = (int) bucket.instances.size();

					glBindVertexArray(draw.vao);
					for (const MeshRange &range : bucket.group->ranges) {
						setMeshRangeState(range, instancedMatAmbientLoc, instancedMatDiffuseLoc,
										  instancedMatEmissiveLoc, instancedHasTextureLoc,
										  instancedSphereMapLoc);
						frameDrawCalls++;
						glDrawArraysInstanced(GL_TRIANGLES, range.first, range.count, draw.instanceCount);
					}
				}
				glBindVertexArray(0);
			}

			// M6: the trees, in their own textured alpha-cut pass - one draw per
			// species present, which is a handful.
			//
			// M11: skipping the pass is the whole of "trees off". The geometry
			// stays built, so turning them back on costs nothing and does not
			// wait for a new round - and a landscape scatters up to two thousand
			// of them, so this is the setting most likely to buy a weak device
			// its frame rate back.
			if (g_showTrees && !treeBuckets.empty() && treeProgram != 0) {
				glUseProgram(treeProgram);
				// The pass's own matrix, not the frame's: this read `mvp`
				// when the block was lifted into a lambda, which drew the
				// reflection's trees from the real camera - two thousand of
				// them in the wrong place, on top of the reflected land.
				glUniformMatrix4fv(treeViewProjLoc, 1, GL_FALSE, vp.m);
				glUniform3f(treeSunPosLoc,
							skyDescription.sunPosition[0],
							skyDescription.sunPosition[2],
							worldZFromEngineY(skyDescription.sunPosition[1]));
				glUniform3fv(treeSkyAmbientLoc, 1, skyDescription.ambience);
				glUniform3fv(treeSkyDiffuseLoc, 1, skyDescription.diffuse);
				glUniform3f(treeFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
				glUniform1f(treeFogDensityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);
				glUniform1i(treeAtlasLoc, 0);
				glActiveTexture(GL_TEXTURE0);
				// Foliage is a one-sided shell built from fans; seen from the
				// other side a branch layer would simply vanish, so both faces
				// are drawn. The alpha test, not the winding, is what shapes it.
				glDisable(GL_CULL_FACE);

				for (auto &entry : treeBuckets) {
					TreeKind *kind = treeKindFor((TreeModelFactory::TreeType) entry.first);
					if (!kind || kind->atlas == 0) continue;

					std::vector<float> packed;
					ScorchDroidInstances::pack(entry.second, packed);
					if (packed != kind->uploaded) {
						glBindBuffer(GL_ARRAY_BUFFER, kind->instanceVbo);
						glBufferData(GL_ARRAY_BUFFER, packed.size() * sizeof(float),
									 packed.data(), GL_DYNAMIC_DRAW);
						kind->uploaded = packed;
					}

					glBindTexture(GL_TEXTURE_2D, kind->atlas);
					glBindVertexArray(kind->vao);
					frameDrawCalls++;
					glDrawArraysInstanced(GL_TRIANGLES, 0, kind->vertexCount,
										  (GLsizei) entry.second.size());
				}
				glEnable(GL_CULL_FACE);
				glBindVertexArray(0);
			}

			// The tank loop below expects the ordinary mesh program bound.
			glUseProgram(meshProgram);
		}
	};


	// W3: the tanks, as one callable pass. The reflection draws them too
	// at its fullest setting - a tank at the water's edge with nothing
	// under it is the thing that gives a fake reflection away.
	//
	// [collect] marks the real view. The pass leaves two things behind for
	// the draws that follow it - where the aim blade goes, and the markers
	// for tanks whose model would not load - and the mirrored pass must
	// contribute to neither: it would overwrite the sight frames with
	// mirrored ones and push a second copy of every marker into a list that
	// is drawn once, unmirrored.
	//
	// [cullBelowY] is upstream's waterline rule again (see the scenery pass).
	auto drawTanksPass = [&](const Mat4 &vp, bool collect, float cullBelowY = -1.0e9f) {
	for (TankInstance &inst : tankInstances) {
			// Upstream's rule, verbatim: TargetRendererImplTank::render() opens
			// with `if (tank_->getState().getState() != TankState::sNormal)
			// return;`, so a tank is drawn only while it is alive and playing -
			// not while dead, loading, spectating or buying. Its drawParticle()
			// does the same, falling through to just an off-screen arrow and a
			// name plate for a non-normal tank (neither of which exists here
			// yet), so nothing else is drawn for it either.
			//
			// The instance is still built for a dead tank, deliberately: the
			// follow camera reads its position, and losing that mid-round would
			// snap the view away the moment you died.
			if (!inst.alive) continue;
			if (inst.y < cullBelowY) continue;

			GpuModel *gpu = uploadModel(inst.model);
			if (gpu && gpu->rawSize * gpu->scale > 10.0f) {
				static std::set<Model *> reported;
				if (reported.insert(inst.model).second) {
					LOGI("Oversize tank: %s raw %.1f x scale %.4f = %.1f units",
						 inst.name.c_str(), gpu->rawSize, gpu->scale, gpu->rawSize * gpu->scale);
				}
			}
			if (!gpu) {
				if (!collect) continue;
				auto &bucket = inst.mine ? unmodelledMine : unmodelledOther;
				bucket.push_back(inst.x); bucket.push_back(inst.y + 1.5f); bucket.push_back(inst.z);
				continue;
			}

			// No tint. Upstream draws the tank model as it is - its skin or
			// its material colours under the sun - and shows the player's
			// colour only on the name plate (TargetRendererImplTank::render
			// sets no glColor before ModelRendererTank::draw). This port
			// used to multiply the model by the tank colour, which is why
			// every tank was a flat team-coloured silhouette.

			// The hull faces the way the tank last drove; the turret swings to
			// the firing bearing and the gun additionally lifts to the
			// elevation, each about its own pivot (see uploadModel) - the same
			// articulation upstream does. The turret's bearing is a world
			// angle, not one relative to the hull, so it is deliberately built
			// from `base` rather than off the hull's transform.
			Mat4 base = Mat4::multiply(
				Mat4::translate(inst.x, inst.y + gpu->groundOffset, inst.z),
				Mat4::scale(gpu->scale));
			// The hull, and only the hull, leans onto the ground - the turret
			// and gun below stay in world axes so the barrel keeps agreeing
			// with the shot (see where groundTilt is built).
			// Tilt first, then yaw inside it, so a tank driving across a slope
			// turns about its own up axis rather than the world's - the same
			// order the ground tilt was added under.
			Mat4 hullModel = Mat4::multiply(
				Mat4::multiply(
					Mat4::translate(inst.x, inst.y + gpu->groundOffset, inst.z),
					Mat4::multiply(inst.groundTilt, Mat4::rotateY(inst.hullYawRadians))),
				Mat4::scale(gpu->scale));
			drawMeshGroup(gpu->hull, vp, hullModel);

			Mat4 turret = Mat4::multiply(base, Mat4::rotateY(inst.headingRadians));
			drawMeshGroup(gpu->turret, vp, turret);

			Mat4 gun = Mat4::multiply(
				turret,
				Mat4::multiply(
					Mat4::translate(gpu->gunOffsetX, gpu->gunOffsetY, gpu->gunOffsetZ),
					// Not negated: the barrel points along world -Z after the
					// upload's remap, and rotateX(+e) lifts -Z towards +Y.
					Mat4::rotateX(inst.elevationRadians)));
			drawMeshGroup(gpu->gun, vp, gun);

			// Upstream draws the sight on the player's own tank while it's
			// playing (TargetRendererImplTank::drawParticle: currentTank &&
			// StatePlaying, and it bails entirely unless the tank is sNormal).
			// Our nearest equivalent is "my tank, alive" - this config has no
			// strict turn order, so every live moment is your turn.
			if (collect && inst.mine && inst.alive) {
				haveSight = true;
				// M22: upstream's sight needs three frames, not one - see
				// buildOriginalSightGeometry.
				sightBaseTransform = Mat4::translate(inst.x, inst.y + gpu->groundOffset, inst.z);
				sightBearingTransform = Mat4::multiply(
					sightBaseTransform, Mat4::rotateY(inst.headingRadians));
				// The blade lives in the gun's own frame, so it inherits the
				// bearing and elevation for free - but not the model scale,
				// since its radii are already in world units.
				sightTransform = Mat4::multiply(
					Mat4::translate(inst.x, inst.y + gpu->groundOffset, inst.z),
					Mat4::multiply(
						Mat4::rotateY(inst.headingRadians),
						Mat4::rotateX(inst.elevationRadians)));
			}
		}
	};

	// Real in-flight shot/explosion positions, straight from the running
	// simulation's ActionController. Shots also report which tank fired
	// them, so each one can use that tank's own projectile model (see the
	// shotPlayerIds addition in patch 0009) rather than one shared mesh.
	//
	// W3: one callable pass, so the reflection can draw it too.
	//
	// Upstream does *not* reflect these: it draws in-flight shots from its
	// ActionController's own 3D game state, which the reflection pass never
	// runs - so a Scorched3D missile over water reflects its smoke trail and
	// nothing else. That reads as an oversight rather than a choice, so this
	// pass goes in the reflection and the omission is not copied.
	//
	// [primary] marks the real view: the trails are emitted here, once per
	// frame, and emitting them again from the mirrored pass would double
	// every rocket's smoke.
	auto drawShotsPass = [&](const Mat4 &vp, bool primary, float cullBelowY = -1.0e9f) {
		glUseProgram(meshProgram);
		beginUpstreamModels();
		glUniform3f(meshFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
		glUniform1f(meshFogDensityLoc, skyDescription.fogDensity);
		for (size_t i = 0; i < shotPositionsRaw.size(); i++) {
			FixedVector &p = shotPositionsRaw[i];
			float wx = p[0].asFloat(), wy = p[2].asFloat(), wz = worldZFromEngineY(p[1].asFloat());
			if (wy < cullBelowY) continue;

			// Upstream's precedence, all three steps of it (see
			// Accessory::getWeaponMesh): the weapon's own <projectilemodel>,
			// then the firing tank's, then a default missile for everything
			// else. That last step is the one that matters - almost no weapon
			// and almost no tank declares a projectile model, so without it the
			// Baby Missile and most of the arsenal flew as a bare dot. It is
			// also why a Gorilla throws bananas and Bender throws bottles:
			// those tanks declare one and their shots inherit it.
			//
			// The first step was reading the accessory's own <model>, which is
			// its inventory model, not the projectile's - a different field
			// that these weapons do not set either. The weapon now comes from
			// the shot itself rather than from its accessory (see patch 0009):
			// for a weapon built out of other weapons those are different
			// objects, and only the shot's own is a WeaponProjectile.
			Model *projectileModel = nullptr;
			float projectileScale = 1.0f;
			if (i < shotWeapons.size() && shotWeapons[i]) {
				{
					WeaponProjectile *projectile = shotWeapons[i];
					projectileModel = loadModelSafely(projectile->getModelID());
					// <projectilescale>, which upstream applies on top of the
					// mesh's own size normalisation. The Baby Missile is half
					// size by it, and looks it beside a real Missile.
					projectileScale = projectile->getScale(*ctx).asFloat();
					// Flame and smoke trail. Upstream emits these from particle
					// emitters attached to the shot (MissileActionRenderer), and
					// both default to *on* for every projectile - so this is what
					// makes an ordinary missile read as a missile rather than a
					// travelling dot. Per-weapon colours, sizes and lifetimes are
					// the weapon's own.
					//
					// Only from the real view: this raises particles rather
					// than drawing them, and a second call from the mirrored
					// pass would emit every rocket's trail twice a frame.
					//
					// The velocity goes with it so the trail can be laid along
					// the path the shot took since the last frame rather than
					// bunched at where it is now. Engine (x, y, height) maps to
					// world (x, height, -y), the same remap the positions take.
					if (primary) {
						float wvx = 0.0f, wvy = 0.0f, wvz = 0.0f;
						if (i < shotVelocities.size()) {
							FixedVector &v = shotVelocities[i];
							wvx = v[0].asFloat();
							wvy = v[2].asFloat();
							wvz = -v[1].asFloat();
						}
						emitProjectileTrail(projectile, wx, wy, wz, wvx, wvy, wvz);
					}
				}
			}
			if (!projectileModel && i < shotPlayerIds.size()) {
				Tanket *firer = ctx->getTargetContainer().getTanketById(shotPlayerIds[i]);
				Tank *firerTank = (firer && firer->getType() == Target::TypeTank) ? (Tank *) firer : nullptr;
				if (firerTank) {
					TankModel *tankModel = firerTank->getModelContainer().getTankModel();
					if (tankModel) {
						projectileModel = loadModelSafely(tankModel->getProjectileModelID());
					}
				}
			}
			if (!projectileModel) projectileModel = loadModelSafely(defaultProjectileModelId());

			GpuModel *gpu = uploadModel(projectileModel);
			if (!gpu) {
				// Collected for the real view only - the fallback dots are
				// drawn once, from the unmirrored camera.
				if (primary) {
					unmodelledShots.push_back(wx);
					unmodelledShots.push_back(wy);
					unmodelledShots.push_back(wz);
				}
				continue;
			}
			// Point the mesh along its actual flight path: a bearing about the
			// up axis, then a pitch about X, like upstream's MissileMesh::draw.
			// Velocity is an engine-space (x, y, height) direction.
			//
			// A projectile model rests pointing *up*, not forward. That is the
			// whole of what was wrong here: this used the tank convention,
			// where the mesh faces along landscape +y, and a missile visibly
			// flew at ninety degrees to its own path.
			//
			// Upstream is unambiguous about it. MissileMesh::draw takes
			// angYZ = acos(dir[2]) - zero rotation when the shot is going
			// straight up - so the nose is along landscape +z at rest, which
			// the upload's (x, y, z) -> (x, z, -y) remap turns into world +Y.
			//
			// rotateY(a) * rotateX(b) sends (0, 1, 0) to
			// (sin b * sin a, cos b, sin b * cos a). Matching that to the
			// world velocity (vx, vz, -vy) gives b = acos(vz) and
			// a = atan2(vx, -vy) - the same pair of angles upstream computes,
			// its own angXY being pi - atan2(vx, vy) about a landscape axis
			// that runs the other way to ours.
			Mat4 orientation = Mat4::identity();
			if (i < shotVelocities.size()) {
				FixedVector &vel = shotVelocities[i];
				float vx = vel[0].asFloat(), vy = vel[1].asFloat(), vz = vel[2].asFloat();
				float len = sqrtf(vx * vx + vy * vy + vz * vz);
				if (len > 0.0001f) {
					vx /= len; vy /= len; vz /= len;
					float angXY = atan2f(vx, -vy);
					float angYZ = acosf(std::min(1.0f, std::max(-1.0f, vz)));
					orientation = Mat4::multiply(Mat4::rotateY(angXY), Mat4::rotateX(angYZ));
				}
			}
			Mat4 model = Mat4::multiply(
				Mat4::translate(wx, wy, wz),
				Mat4::multiply(orientation, Mat4::scale(gpu->scale * projectileScale)));
			drawMeshGroup(gpu->hull, vp, model);
			drawMeshGroup(gpu->turret, vp, model);
			drawMeshGroup(gpu->gun, vp, model);
		}
	};



	// The sun's shadow map, drawn before anything that samples it.
	//
	// Upstream's Landscape::drawShadows, followed closely: a depth-only pass
	// from the sun's own position, aimed at the middle of the map, with the
	// land, the scenery and the tanks in it. Front faces are culled and the
	// depth is offset, both to keep a surface from shadowing itself.
	{
		const int level = g_shadowLevel.load();
		const int wanted = (level <= 0) ? 0 : (level == 1 ? 1024 : 2048);
		if (wanted != shadowSize) {
			if (shadowTexture) glDeleteTextures(1, &shadowTexture);
			if (shadowFbo) glDeleteFramebuffers(1, &shadowFbo);
			shadowTexture = 0; shadowFbo = 0; shadowSize = 0; shadowValid = false;
			if (wanted > 0) {
				glGenTextures(1, &shadowTexture);
				glBindTexture(GL_TEXTURE_2D, shadowTexture);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, wanted, wanted, 0,
							 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
				// COMPARE_REF_TO_TEXTURE is what makes this a sampler2DShadow:
				// the hardware compares rather than returning a depth, and with
				// a linear filter it averages four comparisons, which softens
				// the edge for free. Upstream gets the same from shadow2DProj.
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glGenFramebuffers(1, &shadowFbo);
				glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
									   GL_TEXTURE_2D, shadowTexture, 0);
				// Depth only: no colour attachment at all, which ES3 needs
				// told explicitly on both ends.
				GLenum none = GL_NONE;
				glDrawBuffers(1, &none);
				glReadBuffer(GL_NONE);
				const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				shadowSize = wanted;
				LOGI("Shadow map: %dx%d, status 0x%x", wanted, wanted, status);
			}
		}

		shadowValid = false;
		if (shadowSize > 0 && shadowFbo != 0) {
			// Upstream's own framing, from Landscape::drawShadows. The sun is
			// a position rather than a direction (its light map bakes against
			// a point source 900 units out), pulled in or pushed out with the
			// map's size, and the frustum is a 60-degree cone reaching a map
			// and a half either side of it.
			const float landWidth = mapWidthUnits * 0.5f;
			const float landHeight = mapHeightUnits * 0.5f;
			const float maxWidth = std::max(landWidth, landHeight);
			const float spread = 0.5f + (maxWidth - 128.0f) / 256.0f;
			const float sunX = skyDescription.sunPosition[0] * spread;
			const float sunY = skyDescription.sunPosition[1] * spread;
			const float sunH = skyDescription.sunPosition[2] * spread;
			const float relX = sunX - landWidth, relY = sunY - landHeight;
			const float magnitude = sqrtf(relX * relX + relY * relY + sunH * sunH);
			const float nearZ = std::max(magnitude - maxWidth * 1.5f, 1.0f);
			const float farZ = magnitude + maxWidth * 1.5f;

			// Landscape (x, y, height) into this renderer's (x, height, z).
			const float eyeWX = sunX, eyeWY = sunH, eyeWZ = worldZFromEngineY(sunY);
			const float tgtWX = landWidth, tgtWY = 0.0f,
						tgtWZ = worldZFromEngineY(landHeight);
			// Upstream's up is its height axis, which is ours; a sun directly
			// overhead would make that parallel to the view, so it falls back
			// to a horizontal one in that case rather than producing a
			// degenerate matrix.
			const float dx = tgtWX - eyeWX, dz = tgtWZ - eyeWZ;
			const bool overhead = (dx * dx + dz * dz) < 1.0f;
			Mat4 lightView = Mat4::lookAt(eyeWX, eyeWY, eyeWZ, tgtWX, tgtWY, tgtWZ,
										  0.0f, overhead ? 0.0f : 1.0f, overhead ? 1.0f : 0.0f);
			Mat4 lightProj = Mat4::perspective(60.0f * 3.14159265f / 180.0f, 1.0f, nearZ, farZ);
			Mat4 lightVp = Mat4::multiply(lightProj, lightView);
			// The [-1,1] to [0,1] remap upstream folds into its texture matrix.
			const Mat4 bias = Mat4::multiply(Mat4::translate(0.5f, 0.5f, 0.5f),
											 Mat4::scale(0.5f, 0.5f, 0.5f));
			shadowMatrix = Mat4::multiply(bias, lightVp);

			glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
			glViewport(0, 0, shadowSize, shadowSize);
			glClear(GL_DEPTH_BUFFER_BIT);
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(10.0f, 10.0f);
			// Upstream culls front faces here, which is the usual trick for
			// closed geometry: the back of a solid is far enough behind its
			// front to hide the depth-comparison error. This terrain is not
			// closed - it is an open heightmap with a top and no bottom, and
			// its top *is* the front face from the sun. Culling that leaves
			// nothing to write depth at all, so the map comes back empty and
			// nothing is ever shadowed. Both sides are drawn instead, and the
			// offset above does the work upstream's cull was doing.
			glDisable(GL_CULL_FACE);

			g_passView = lightView;
			drawLandPass(lightVp, false);
			drawSceneryPass(lightVp);
			drawTanksPass(lightVp, false);

			glEnable(GL_CULL_FACE);
			glDisable(GL_POLYGON_OFFSET_FILL);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, surfaceWidth, surfaceHeight);
			shadowValid = true;
		}

		// Bound once for the whole frame; both the terrain and the water read
		// it, and nothing else uses unit 5.
		if (shadowValid) {
			glActiveTexture(GL_TEXTURE5);
			glBindTexture(GL_TEXTURE_2D, shadowTexture);
			glActiveTexture(GL_TEXTURE0);
		}
	}

	// W3: the reflection, when upstream's own is the one asked for.
	//
	// Upstream renders the scene a second time into a texture with the
	// camera mirrored in the water plane (Water2Renderer), and samples that
	// where this port samples a sky gradient. Same idea here, at half the
	// screen's resolution.
	//
	// At the top setting this draws what upstream's own reflection pass
	// draws, in upstream's own order - Landscape::drawWater is the list:
	//
	//     sky_->drawBackdrop(true);   // sky colour, stars, the sun
	//     sky_->drawLayers();         // the clouds over them
	//     actualDrawLandReflection();
	//     RenderTargets::instance()->draw(true);          // tanks, scenery
	//     ScorchedClient::instance()->getParticleEngine().draw(0);
	//
	// plus the shots in flight, which upstream leaves out for a structural
	// reason rather than a visual one (see drawShotsPass). What upstream
	// leaves out and so does this: the water's own points, which its own
	// comment calls "bad reflections in large wind".
	const int reflectionLevel = g_reflectionStyle.load();
	const bool wantReflection =
		reflectionLevel > 0 && waterVisible && surfaceWidth > 0 && surfaceHeight > 0;
	if (wantReflection) {
		const int rw = std::max(surfaceWidth / 2, 1), rh = std::max(surfaceHeight / 2, 1);
		if (reflectionFbo == 0 || rw != reflectionWidth || rh != reflectionHeight) {
			if (reflectionTexture) glDeleteTextures(1, &reflectionTexture);
			if (reflectionDepth) glDeleteRenderbuffers(1, &reflectionDepth);
            if (reflectionFbo) glDeleteFramebuffers(1, &reflectionFbo);
			glGenTextures(1, &reflectionTexture);
			glBindTexture(GL_TEXTURE_2D, reflectionTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, rw, rh, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			// Clamped: a wave can push the sample off the edge, and wrapping
			// there would fold the far shore into the near one.
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glGenRenderbuffers(1, &reflectionDepth);
			glBindRenderbuffer(GL_RENDERBUFFER, reflectionDepth);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, rw, rh);
			glGenFramebuffers(1, &reflectionFbo);
			glBindFramebuffer(GL_FRAMEBUFFER, reflectionFbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
								   GL_TEXTURE_2D, reflectionTexture, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
									  GL_RENDERBUFFER, reflectionDepth);
			reflectionWidth = rw;
			reflectionHeight = rh;
			LOGI("Reflection target: %dx%d, status 0x%x", rw, rh,
				 glCheckFramebufferStatus(GL_FRAMEBUFFER));
		}

		// The camera, mirrored in the plane y = waterHeight - and this is the
		// whole of why the reflection used to sit in the wrong place.
		//
		// It was built by feeding lookAt the mirrored eye and the mirrored
		// target with the world's own up vector, which sounds like the same
		// thing and is not. The true mirrored view is the real view composed
		// with the reflection itself, `view * mirror`, and lookAt cannot
		// produce that: given the mirrored eye and target it derives an up of
		// its own that comes out *negated* against the real one. The basis is
		// orthonormal either way, so nothing looked broken, but every point
		// landed at -y in the reflection buffer's clip space.
		//
		// That matters because the sampling relies on the two agreeing: a
		// point on the water plane is unmoved by the mirror, so it must
		// project to the same place in both views, and then the reflection is
		// read at the fragment's own screen position. With the vertical
		// flipped it was read from the mirror image of that position instead -
		// which is why a reflection belonging under the shoreline appeared up
		// near the horizon.
		//
		// Upstream composes it exactly this way (Landscape::drawWater:
		// glTranslatef(0, 0, waterHeight*2) then glScalef(1, 1, -1), in its
		// Z-up world), and flips the winding afterwards because the result is
		// a reflection rather than a rotation - which ours now genuinely is.
		const float reflEyeY = 2.0f * waterHeight - eyeY;
		const Mat4 mirror = Mat4::multiply(
			Mat4::translate(0.0f, 2.0f * waterHeight, 0.0f),
			Mat4::scale(1.0f, -1.0f, 1.0f));
		Mat4 reflView = Mat4::multiply(view, mirror);
		Mat4 reflMvp = Mat4::multiply(proj, reflView);

		glBindFramebuffer(GL_FRAMEBUFFER, reflectionFbo);
		glViewport(0, 0, rw, rh);
		// Upstream's own clear colour for this buffer, from
		// Landscape::drawWater: a near-black blue-green. It was the fog colour
		// here, which is a pale haze on most landscapes - and since the
		// mirrored view has nothing to draw below its own horizon, that pale
		// clear showed through as a hard diagonal band of lighter sea wherever
		// the reflection was sampled from that part of the buffer.
		glClearColor(0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		// Mirroring turns every triangle inside out, so what was front-facing
		// is now back-facing. Winding is flipped rather than culling turned
		// off, to keep the same triangles visible as in the real view.
		glFrontFace(GL_CW);
		drawSky(-reflView.m[2], -reflView.m[6], -reflView.m[10],
				reflView.m[0], reflView.m[4], reflView.m[8],
				reflView.m[1], reflView.m[5], reflView.m[9],
				tanf(kFovYRadians * 0.5f), aspect, reflEyeY);
		// The clouds, stars and sun, which upstream's drawLayers() puts in
		// its reflection as well - a mirrored gradient under a clouded sky
		// was the giveaway that this was only half a reflection.
		drawSkyLayersPass(reflMvp, reflView);
		drawLandPass(reflMvp, true);
		if (reflectionLevel >= 2) {
			// Everything else, with upstream's waterline rule applied to
			// each: anything whose position is under the surface is skipped,
			// because mirroring would raise it back above one.
			g_passView = reflView;
			drawSceneryPass(reflMvp, waterHeight);
			drawTanksPass(reflMvp, false, waterHeight);
			drawShotsPass(reflMvp, false, waterHeight);
			// The explosions, smoke and trails. Point sizes are measured
			// against the screen's height, so the half-resolution buffer
			// needs them scaled to match - otherwise every puff reflects at
			// twice its size.
			drawEffects(reflMvp, reflView, eyeX, reflEyeY, eyeZ, kFovYRadians,
						(float) rh / (float) std::max(surfaceHeight, 1), waterHeight);
		}
		glFrontFace(GL_CCW);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, surfaceWidth, surfaceHeight);
	}

	// Sky first: it is the backdrop everything else is drawn in front of.
	drawSky(-view.m[2], -view.m[6], -view.m[10],
			view.m[0], view.m[4], view.m[8],
			view.m[1], view.m[5], view.m[9],
			tanf(kFovYRadians * 0.5f), aspect, eyeY);

	drawSkyLayersPass(mvp, view);

	// W3: the land, as one callable pass - the reflection draws it a
	// second time from the mirrored camera, and duplicating seventy
	// lines of uniform setting to do that would guarantee the two
	// drift apart.
	drawLandPass(mvp, false);

	// Ground shadows, between the terrain and the water: they belong on the
	// land, and a shadow showing through the sea would be worse than none.
	{
		std::vector<float> quads;
		auto addShadow = [&](float x, float y, float z, float radius) {
			if (radius <= 0.0f) return;
			// Lifted clear of the ground so it doesn't z-fight the slope it
			// lies on. Depth is still tested, so a shadow behind a hill is
			// correctly hidden.
			const float h = y + 0.25f;
			const float corners[6][2] = {
				{ -1, -1 }, { 1, -1 }, { -1, 1 },
				{ -1,  1 }, { 1, -1 }, {  1, 1 },
			};
			for (int i = 0; i < 6; i++) {
				quads.push_back(x + corners[i][0] * radius);
				quads.push_back(h);
				quads.push_back(z + corners[i][1] * radius);
				quads.push_back(corners[i][0]);
				quads.push_back(corners[i][1]);
			}
		};

		for (TargetInstance &inst : targetInstances) {
			addShadow(inst.x, inst.y, inst.z, inst.shadowRadius);
		}
		for (TankInstance &inst : tankInstances) {
			if (inst.alive) addShadow(inst.x, inst.y, inst.z, 1.6f);
		}

		if (!quads.empty() && shadowProgram != 0) {
			glUseProgram(shadowProgram);
			glUniformMatrix4fv(shadowMvpLoc, 1, GL_FALSE, mvp.m);
			glUniform1f(shadowStrengthLoc, 0.5f);
			glBindVertexArray(shadowVao);
			glBindBuffer(GL_ARRAY_BUFFER, shadowVbo);
			glBufferData(GL_ARRAY_BUFFER, quads.size() * sizeof(float),
						 quads.data(), GL_DYNAMIC_DRAW);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);
			glDisable(GL_CULL_FACE);
			frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, (GLsizei) (quads.size() / 5));
			glEnable(GL_CULL_FACE);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
			glBindVertexArray(0);
		}
	}

	// M6: the arena wall flash. Drawn after the ground and before the water,
	// with the other translucent geometry. Each side is one quad standing on
	// the arena boundary, matching upstream's own corners and its 256-unit
	// height - the panel is meant to look like the whole wall lighting up,
	// not a splash where the shot landed.
	if ((wallFade[0] > 0.0f || wallFade[1] > 0.0f ||
		 wallFade[2] > 0.0f || wallFade[3] > 0.0f) && sightProgram != 0) {
		// Upstream's own per-type colours (OptionsTransient::getWallColor),
		// read from the engine rather than fixed here - the type is picked
		// per round, since WallType defaults to WallRandom.
		switch (ctx->getOptionsTransient().getWallType()) {
			case OptionsTransient::wallWrapAround:
				wallColor[0] = 0.5f; wallColor[1] = 0.5f; wallColor[2] = 0.0f; break;
			case OptionsTransient::wallBouncy:
				wallColor[0] = 0.0f; wallColor[1] = 0.0f; wallColor[2] = 0.5f; break;
			case OptionsTransient::wallConcrete:
				wallColor[0] = 0.5f; wallColor[1] = 0.5f; wallColor[2] = 0.5f; break;
			default:
				wallColor[0] = wallColor[1] = wallColor[2] = 0.0f; break;
		}

		GroundMaps &ground = ctx->getLandscapeMaps().getGroundMaps();
		const float ax = (float) ground.getArenaX();
		const float ay = (float) ground.getArenaY();
		const float aw = (float) ground.getArenaWidth();
		const float ah = (float) ground.getArenaHeight();
		const float top = 256.0f;   // upstream's own wall height

		// Upstream's corner order, converted to our world axes. Its
		// LeftSide/RightSide/TopSide/BotSide index the arena in landscape
		// terms, so the pairs are taken from its own drawWall calls rather
		// than re-derived.
		const float sides[4][4] = {
			// x0, y0(engine), x1, y1(engine)
			{ ax,      ay + ah, ax,      ay      },  // LeftSide
			{ ax + aw, ay,      ax + aw, ay + ah },  // RightSide
			{ ax,      ay,      ax + aw, ay      },  // TopSide
			{ ax + aw, ay + ah, ax,      ay + ah },  // BotSide
		};

		std::vector<float> quads;
		for (int side = 0; side < 4; side++) {
			const float fade = wallFade[side];
			if (fade <= 0.0f) continue;
			const float x0 = sides[side][0], z0 = worldZFromEngineY(sides[side][1]);
			const float x1 = sides[side][2], z1 = worldZFromEngineY(sides[side][3]);
			const float r = wallColor[0] * fade;
			const float g = wallColor[1] * fade;
			const float b = wallColor[2] * fade;
			const float corner[6][3] = {
				{ x0, 0.0f, z0 }, { x1, 0.0f, z1 }, { x1, top, z1 },
				{ x0, 0.0f, z0 }, { x1, top,  z1 }, { x0, top, z0 },
			};
			for (int c = 0; c < 6; c++) {
				quads.push_back(corner[c][0]);
				quads.push_back(corner[c][1]);
				quads.push_back(corner[c][2]);
				quads.push_back(r); quads.push_back(g); quads.push_back(b);
			}
		}

		if (!quads.empty()) {
			glUseProgram(sightProgram);
		setFixedFunctionFog(sightFogColorLoc, sightFogDensityLoc);
			glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, mvp.m);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			glDepthMask(GL_FALSE);
			glDisable(GL_CULL_FACE);
			glBindVertexArray(beamVao);
			glBindBuffer(GL_ARRAY_BUFFER, beamVbo);
			glBufferData(GL_ARRAY_BUFFER, quads.size() * sizeof(float), quads.data(), GL_DYNAMIC_DRAW);
			frameDrawCalls++;
			glDrawArrays(GL_TRIANGLES, 0, (GLsizei) (quads.size() / 6));
			glEnable(GL_CULL_FACE);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
			glBindVertexArray(0);
		}
	}

	// Water goes on immediately after the ground and before anything that
	// stands on it. It blends over the terrain already drawn (so a shoreline
	// shows the bottom shelving away) but still writes depth, so a tank or a
	// tree below the waterline is properly submerged rather than floating
	// on top of the surface.
	if (waterVisible && waterProgram != 0) {
		glUseProgram(waterProgram);
		// The ocean tile: refreshed from the worker if it has a new one.
		updateOceanIfNeeded(*ctx);
		glUniform1f(waterWaveTileLoc, ScorchDroidOcean::kTileLength);
		const bool useReflection = wantReflection && reflectionTexture != 0;
		glUniform1f(waterUseReflectLoc, useReflection ? 1.0f : 0.0f);
		if (useReflection) {
			glActiveTexture(GL_TEXTURE4);
			glBindTexture(GL_TEXTURE_2D, reflectionTexture);
			glUniform1i(waterReflectTexLoc, 4);
		}
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, oceanTexture);
		glUniform1i(waterWaveTexLoc, 3);
		glActiveTexture(GL_TEXTURE6);
		glBindTexture(GL_TEXTURE_2D, oceanNormalTexture);
		glUniform1i(waterWaveNormalTexLoc, 6);
		glActiveTexture(GL_TEXTURE7);
		glBindTexture(GL_TEXTURE_2D, waterFoamMaskTexture);
		glUniform1i(waterFoamMaskLoc, 7);
		glUniformMatrix4fv(waterMvpLoc, 1, GL_FALSE, mvp.m);
		// Upstream's reflection texture matrix: bias * proj * view of the
		// *real* camera. The mirrored pass drew into the buffer with a view
		// that agrees with this one on the water plane, so this projects a
		// surface point to the place its own reflection was rendered.
		{
			const Mat4 bias = Mat4::multiply(Mat4::translate(0.5f, 0.5f, 0.5f),
											 Mat4::scale(0.5f, 0.5f, 0.5f));
			const Mat4 reflectMatrix = Mat4::multiply(bias, mvp);
			glUniformMatrix4fv(waterReflectMatrixLoc, 1, GL_FALSE, reflectMatrix.m);
		}
		glUniform1i(waterShadowEnabledLoc, shadowValid ? 1 : 0);
		glUniformMatrix4fv(waterShadowMatrixLoc, 1, GL_FALSE, shadowMatrix.m);
		glUniform1i(waterShadowTexLoc, 5);
		glUniform3f(waterUpwellTopLoc, waterUpwellTop[0], waterUpwellTop[1], waterUpwellTop[2]);
		glUniform3f(waterUpwellBotLoc, waterUpwellBot[0], waterUpwellBot[1], waterUpwellBot[2]);
		glUniform1f(waterHeightLoc, waterHeight);
		glUniform3f(waterSunDiffuseLoc, skyDescription.diffuse[0],
					skyDescription.diffuse[1], skyDescription.diffuse[2]);

		// Upstream's own noise scroll, from Water2Renderer::drawWaterShaders:
		//   D_0 = windDir1 * (windSpeed1 / (-64 * 6))
		//   D_1 = windDir2 * (windSpeed2 / (-16 * 6))
		//   noise_n_pos = D_n * (totalTime / 24), z = 8/256 and 32/256
		// with windSpeed1 the game's own wind mapped its way (speed*2 + 3),
		// and the second layer on upstream's second wind: speed1 plus a
		// random offset in [-1, 1], floored at 0, the direction jittered by
		// up to 0.2 on each axis and renormalised. The jitter is rolled per
		// landscape (buildWaterIfNeeded) as upstream rolls it per generate.
		{
			Wind &waterWind = ctx->getSimulator().getWind();
			FixedVector dir = waterWind.getWindDirection();
			float wdx = dir[0].asFloat(), wdz = -dir[1].asFloat();
			const float wlen = sqrtf(wdx * wdx + wdz * wdz);
			if (wlen < 0.01f) { wdx = 0.707f; wdz = 0.707f; }   // a dead calm still drifts
			else { wdx /= wlen; wdz /= wlen; }
			const float speed1 = waterWind.getWindSpeed().asFloat() * 2.0f + 3.0f;
			const float speed2 = std::max(0.0f, speed1 + waterWind2SpeedOffset);
			float w2x = wdx + waterWind2JitterX, w2z = wdz + waterWind2JitterZ;
			const float w2len = sqrtf(w2x * w2x + w2z * w2z);
			if (w2len > 0.001f) { w2x /= w2len; w2z /= w2len; }
			const float t = (float) fmod(lastFrameSeconds, 3600.0);
			const float d0 = speed1 / (-64.0f * 6.0f) * t;
			const float d1 = speed2 / (-16.0f * 6.0f) * t;
			glUniform3f(waterNoise0Loc, wdx * d0, wdz * d0, 8.0f / 256.0f);
			glUniform3f(waterNoise1Loc, w2x * d1, w2z * d1, 32.0f / 256.0f);
		}
		glUniform1f(waterAlphaLoc, waterAlpha);
		glUniform1f(waterTimeLoc, (float) fmod(lastFrameSeconds, 3600.0));
		// The two ends of the sky gradient *as the sky pass draws them*:
		// fogged by upstream's dome distances, 2000 units at the horizon
		// and 225 at the zenith, so the sea's own reflection fallback
		// agrees with the sky it stands in for. Unfogged, the far sea
		// mirrored a bright horizon the sky pass never showed.
		{
			float skyFog[3];
			currentFogColor(skyFog);
			const float density = g_showFog ? skyDescription.fogDensity : 0.0f;
			const float fh = expf(-(density * 2000.0f) * (density * 2000.0f));
			const float fz = expf(-(density * 225.0f) * (density * 225.0f));
			const float *top = skyDescription.gradient[ScorchDroidSky::kGradientSteps - 1];
			const float *bottom = skyDescription.gradient[0];
			glUniform3f(waterSkyHorizonLoc,
						skyFog[0] + (bottom[0] - skyFog[0]) * fh,
						skyFog[1] + (bottom[1] - skyFog[1]) * fh,
						skyFog[2] + (bottom[2] - skyFog[2]) * fh);
			glUniform3f(waterSkyZenithLoc,
						skyFog[0] + (top[0] - skyFog[0]) * fz,
						skyFog[1] + (top[1] - skyFog[1]) * fz,
						skyFog[2] + (top[2] - skyFog[2]) * fz);
		}
		glUniform3f(waterEyePosLoc, eyeX, eyeY, eyeZ);
		float waterFog[3];
		currentFogColor(waterFog);
		glUniform3f(waterFogColorLoc, waterFog[0], waterFog[1], waterFog[2]);
		glUniform1f(waterFogDensityLoc, g_showFog ? skyDescription.fogDensity : 0.0f);
		glUniform2f(waterMapSizeLoc, mapWidthUnits, mapHeightUnits);
		glUniform1i(waterDebugModeLoc, waterDebugMode());
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		// The surface extends far past the map on every side, so with the
		// camera under it the back faces are what you see - and being able
		// to look up through it from a valley floor is worth more than the
		// culling.
		glDisable(GL_CULL_FACE);
		glUniform3f(waterSunDirLoc,
					skyDescription.sunPosition[0],
					skyDescription.sunPosition[2],
					worldZFromEngineY(skyDescription.sunPosition[1]));

		// The mesh, rebuilt if the Water detail has changed since. The
		// ocean tile is already in world units at upstream's own scale
		// (OceanWaves.h), so it is drawn at 1. Two draws from one index
		// buffer: the inner grid at its own mip, the outer ring at the
		// 16-unit one.
		if (waterGridBuiltForDetail != g_waterDetail.load()) buildWaterGrid(g_waterDetail.load());
		if (waterInnerIndexCount > 0 && oceanTexture != 0) {
			glUniform1f(waterWaveAmpLoc, 1.0f);
			glBindVertexArray(waterGridVao);
			glUniform1f(waterWaveLodLoc, waterInnerLod);
			frameDrawCalls++;
			glDrawElements(GL_TRIANGLES, waterInnerIndexCount, GL_UNSIGNED_INT, (void *) 0);
			// Level 0 for the ring as well - see the height texture's
			// creation for why its mip chain is gone.
			glUniform1f(waterWaveLodLoc, 0.0f);
			frameDrawCalls++;
			glDrawElements(GL_TRIANGLES, waterOuterIndexCount, GL_UNSIGNED_INT,
						   (void *) (waterInnerIndexCount * sizeof(unsigned int)));
			glBindVertexArray(0);
		}

		glDisable(GL_BLEND);
		glEnable(GL_CULL_FACE);

		// W10c: the breakers, upstream's WaterWaves::draw. Additive, no
		// depth write, three phases of each image two seconds apart.
		if (breakersDirty && lastFrameSeconds - breakersRebuiltAt > 0.5) buildBreakers(*ctx);
		if (breakerProgram != 0 && oceanTexture != 0 &&
			(breakerVertexCount[0] > 0 || breakerVertexCount[1] > 0)) {
			glUseProgram(breakerProgram);
			glUniformMatrix4fv(breakerMvpLoc, 1, GL_FALSE, mvp.m);
			glUniform1f(breakerWaterHeightLoc, waterHeight);
			glUniform1f(breakerTileLoc, ScorchDroidOcean::kTileLength);
			{
				// Upstream filters against the round's *starting* wind
				// direction, normalised; mirrored in y into the world frame
				// as the perpendiculars were.
				FixedVector w = ctx->getSimulator().getWind().getWindStartingDirection();
				float wx = w[0].asFloat(), wz = -w[1].asFloat();
				const float len = sqrtf(wx * wx + wz * wz);
				if (len > 0.001f) { wx /= len; wz /= len; }
				glUniform2f(breakerWindLoc, wx, wz);
			}
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_2D, oceanTexture);
			glUniform1i(breakerWaveTexLoc, 3);
			glActiveTexture(GL_TEXTURE0);
			glUniform1i(breakerTextureLoc, 0);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			glDepthMask(GL_FALSE);
			glDisable(GL_CULL_FACE);
			glBindVertexArray(breakerVao);

			// WaterWaves::drawBoxes, its own "magic to try to get it look
			// kind of ok": the front slides out for four seconds and back
			// for two, fading in over the first second and out over the
			// last two.
			auto drawPhase = [&](int set, float time) {
				if (breakerVertexCount[set] == 0 || breakerTexture[set] == 0) return;
				float t = time;
				if (t > 6.0f) t -= 6.0f;
				float alpha = 1.0f;
				float front = t + 0.2f;
				float end = t / 2.0f;
				if (t < 1.0f) alpha = t;
				if (t > 4.0f) {
					front = 4.2f - (t - 4.0f) / 3.0f;
					end = 2.0f - (t - 4.0f) / 3.0f;
					alpha = (2.0f - (t - 4.0f)) / 2.0f;
				}
				front *= 2.0f;
				end *= 2.0f;
				glUniform1f(breakerFrontLoc, front);
				glUniform1f(breakerEndLoc, end);
				glUniform1f(breakerAlphaLoc, alpha);
				glBindTexture(GL_TEXTURE_2D, breakerTexture[set]);
				frameDrawCalls++;
				glDrawArrays(GL_TRIANGLES, set == 0 ? 0 : breakerVertexCount[0], breakerVertexCount[set]);
			};
			drawPhase(0, breakerTime + 0.0f);
			drawPhase(0, breakerTime + 2.0f);
			drawPhase(0, breakerTime + 4.0f);
			drawPhase(1, breakerTime + 1.0f);
			drawPhase(1, breakerTime + 3.0f);
			drawPhase(1, breakerTime + 5.0f);

			glBindVertexArray(0);
			glEnable(GL_CULL_FACE);
			glDepthMask(GL_TRUE);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDisable(GL_BLEND);
		}
	}

	glUseProgram(pointProgram);
	glDisable(GL_CULL_FACE);  // point sprites have no winding

	// Real tank models where we have one; a point sprite is kept as the
	// fallback for any tank whose model wouldn't load, so a tank is never
	// simply invisible.

	g_passView = view;
	drawSceneryPass(mvp);

	// M6: the thrown rocks. Upstream picks between rock1 and rock2 per chunk
	// and draws them with no skin, so they take the .ase's own "no
	// texture" material colours under the sun.
	if (!debrisChunks.empty()) {
		ModelID rockIds[2];
		rockIds[0].initFromString("ase", "data/meshes/rock1.ase", "none");
		rockIds[1].initFromString("ase", "data/meshes/rock2.ase", "none");
		for (const DebrisChunk &chunk : debrisChunks) {
			Model *model = loadModelSafely(rockIds[chunk.mesh & 1]);
			if (!model) continue;
			GpuModel *gpu = uploadModel(model);
			if (!gpu) continue;
			Mat4 rockModel = Mat4::multiply(
				Mat4::translate(chunk.x, chunk.y, chunk.z),
				Mat4::multiply(
					Mat4::rotateAxis(chunk.axisX, chunk.axisY, chunk.axisZ, chunk.angle),
					Mat4::scale(gpu->scale * chunk.scale)));
			drawMeshGroup(gpu->hull, mvp, rockModel);
		}
	}

	// G7: the arena markers, upstream's LandscapePoints and WaterMapPoints.
	// Every 32 units around the arena a small model of the wall type -
	// wrap.ase, bounce.ase or concrete.ase, scaled 0.15 - stands on the
	// ground, and a second set rides the water at the same points, 0.6
	// above the wave. The arena is usually the whole map but a landscape
	// can set it smaller, and a shot that crosses the edge is gone, so
	// the ring matters. (This replaces a ring of sprites, which was also
	// spaced by the arena's width over 32 rather than every 32 units.)
	{
		const OptionsTransient::WallType wall = ctx->getOptionsTransient().getWallType();
		const char *file = nullptr;
		switch (wall) {
			case OptionsTransient::wallWrapAround: file = "data/meshes/wrap.ase"; break;
			case OptionsTransient::wallBouncy:     file = "data/meshes/bounce.ase"; break;
			case OptionsTransient::wallConcrete:   file = "data/meshes/concrete.ase"; break;
			default: break;
		}
		if (file) {
			ModelID markerId;
			markerId.initFromString("ase", file, "none");
			Model *model = loadModelSafely(markerId);
			GpuModel *gpu = model ? uploadModel(model) : nullptr;
			if (gpu) {
				GroundMaps &ground = ctx->getLandscapeMaps().getGroundMaps();
				HeightMap &markerMap = ground.getHeightMap();
				const int arenaX = ground.getArenaX(), arenaY = ground.getArenaY();
				const int arenaW = ground.getArenaWidth(), arenaH = ground.getArenaHeight();
				const int pointsX = arenaW / 32, pointsY = arenaH / 32;
				std::vector<std::pair<int, int>> points;
				for (int i = 0; i <= pointsX; i++) {
					points.push_back({ arenaX + 32 * i, arenaY });
					points.push_back({ arenaX + 32 * i, arenaY + arenaH });
				}
				for (int i = 1; i <= pointsY - 1; i++) {
					points.push_back({ arenaX, arenaY + 32 * i });
					points.push_back({ arenaX + arenaW, arenaY + 32 * i });
				}
				static bool markersLogged = false;
				if (!markersLogged) {
					markersLogged = true;
					LOGI("Arena markers: %s, %d points, water %s", file, (int) points.size(),
						 waterVisible ? "yes" : "no");
				}
				const Mat4 markerScale = Mat4::scale(0.15f);
				for (const std::pair<int, int> &pt : points) {
					const float x = (float) pt.first;
					const float z = worldZFromEngineY((float) pt.second);
					// On the ground (LandscapePoints::draw).
					const float gy = heightAt(markerMap, mapWidthUnits, mapHeightUnits, x, (float) pt.second);
					drawMeshGroup(gpu->hull, mvp, Mat4::multiply(Mat4::translate(x, gy, z), markerScale));
					// And on the water (WaterMapPoints::draw), riding the
					// ocean tile 0.6 above the surface, where there is one.
					if (waterVisible && !oceanUpload.empty()) {
						const int n = ScorchDroidOcean::kResolution;
						const float tile = ScorchDroidOcean::kTileLength;
						const int tx = ((int) floorf(x / tile * n) % n + n) % n;
						const int tz = ((int) floorf(z / tile * n) % n + n) % n;
						const float wave = oceanUpload[((size_t) tz * n + tx) * 3];
						drawMeshGroup(gpu->hull, mvp, Mat4::multiply(
							Mat4::translate(x, waterHeight + wave + 0.6f, z), markerScale));
					}
				}
			}
		}
	}


	drawTanksPass(mvp, true);

	if (haveSight && g_sightStyle.load() == 1) {
		// M22: upstream's own arrangement - a protractor ring flat under the
		// tank, a bearing marker on the ground, an arc for the elevation and
		// the red blade along the barrel.
		buildOriginalSightGeometry();
		glUseProgram(sightProgram);
		setFixedFunctionFog(sightFogColorLoc, sightFogDensityLoc);
		glDisable(GL_CULL_FACE);
		struct Piece { GLuint vao; int count; const Mat4 *frame; };
		const Piece pieces[] = {
			{ sightRingVao,    sightRingVertexCount,    &sightBaseTransform },
			{ sightBearingVao, sightBearingVertexCount, &sightBearingTransform },
			{ sightBarrelVao,  sightBarrelVertexCount,  &sightTransform },
		};
		for (const Piece &piece : pieces) {
			if (piece.count == 0) continue;
			Mat4 pieceMvp = Mat4::multiply(mvp, *piece.frame);
			glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, pieceMvp.m);
			glBindVertexArray(piece.vao);
			frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, piece.count);
		}
		glEnable(GL_CULL_FACE);
	} else if (haveSight) {
		buildSightGeometry();
		glUseProgram(sightProgram);
		setFixedFunctionFog(sightFogColorLoc, sightFogDensityLoc);
		Mat4 sightMvp = Mat4::multiply(mvp, sightTransform);
		glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, sightMvp.m);
		glBindVertexArray(sightVao);
		// Two-sided: it is a flat blade you are meant to see from wherever
		// the camera happens to be, and one winding always faces away. This
		// came in with a slimmer blade that vanished without it; the wide
		// fan here survived culling by luck, and keeping the two-sided draw
		// means it no longer depends on that luck.
		glDisable(GL_CULL_FACE);
		frameDrawCalls++; glDrawArrays(GL_TRIANGLE_STRIP, 0, sightVertexCount);
		glEnable(GL_CULL_FACE);
	}

	glUseProgram(pointProgram);
	drawPoints(mvp, unmodelledOther, 26.0f, 0.95f, 0.25f, 0.2f);
	drawPoints(mvp, unmodelledMine, 26.0f, 0.2f, 0.9f, 0.95f);

	drawShotsPass(mvp, true);

	for (FixedVector &p : explosionPositionsRaw) {
		explosionPositions.push_back(p[0].asFloat());
		explosionPositions.push_back(p[2].asFloat());
		explosionPositions.push_back(worldZFromEngineY(p[1].asFloat()));
	}
	glUseProgram(pointProgram);
	drawPoints(mvp, unmodelledShots, 14.0f, 1.0f, 1.0f, 0.2f);
	// The explosion marker dot is deliberately kept: it tracks the Explosion
	// action for as long as it lives, whereas the particle burst below is
	// raised once at detonation and then flies on its own.
	drawPoints(mvp, explosionPositions, 20.0f, 1.0f, 0.5f, 0.0f);

	// Shield bubbles and parachutes, after the solid tanks so they blend
	// over them. Translucent and depth-write-off, so a bubble never hides
	// the tank inside it or another bubble behind it.
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
		glUseProgram(meshProgram);
		beginFlatMeshes();
		glUniform3f(meshFogColorLoc, fogColor[0], fogColor[1], fogColor[2]);
		glUniform1f(meshFogDensityLoc, skyDescription.fogDensity);

		for (TankInstance &inst : tankInstances) {
			if (!inst.alive) continue;

			if (inst.hasShield) {
				// Upstream centres the bubble on the tank's own position.
				Mat4 scale = inst.shieldRound
					? Mat4::scale(inst.shieldRadius)
					: Mat4::scale(inst.shieldX, inst.shieldY, inst.shieldZ);
				Mat4 model = Mat4::multiply(Mat4::translate(inst.x, inst.y, inst.z), scale);
				glUniformMatrix4fv(meshMvpLoc, 1, GL_FALSE, Mat4::multiply(mvp, model).m);
				glUniform4f(meshColorLoc, inst.shieldR, inst.shieldG, inst.shieldB, 0.35f);

				if (!inst.shieldRound) {
					glBindVertexArray(cubeVao);
					frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, cubeVertexCount);
				} else if (inst.shieldHalf) {
					glBindVertexArray(hemiVao);
					frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, hemiVertexCount);
				} else {
					glBindVertexArray(sphereVao);
					frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, sphereVertexCount);
				}
			}

			if (inst.parachuteOpen) {
				Mat4 model = Mat4::translate(inst.x, inst.y, inst.z);
				glUniformMatrix4fv(meshMvpLoc, 1, GL_FALSE, Mat4::multiply(mvp, model).m);
				glUniform4f(meshColorLoc, 0.85f, 0.85f, 0.9f, 0.9f);
				glBindVertexArray(chuteVao);
				frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, chuteVertexCount);
				glUniform4f(meshColorLoc, 1.0f, 1.0f, 1.0f, 0.9f);
				glBindVertexArray(chuteCordVao);
				frameDrawCalls++; glDrawArrays(GL_LINES, 0, chuteCordVertexCount);
			}
		}

		// Ranging tracers - the marks left by "Tracer" and "Smoke Tracer"
		// shots. Upstream draws only the *current* tank's, in that tank's
		// colour (RenderTracer::draw looks up getCurrentTank), because they
		// are a private aid for ranging your own next shot rather than a
		// shared map marker. Same here.
		if (haveMyTank && myPlayerId != 0) {
			std::vector<ScorchDroidTracer::Point> endPoints;
			std::vector<std::vector<ScorchDroidTracer::Point> > paths;
			ScorchDroidTracer::getFor(myPlayerId, endPoints, paths);

			if (!endPoints.empty()) {
				glUniform4f(meshColorLoc, myColorR, myColorG, myColorB, 0.85f);
				glBindVertexArray(sphereVao);
				for (size_t i = 0; i < endPoints.size(); i++) {
					// Upstream's marker is a 0.5-radius sphere.
					Mat4 model = Mat4::multiply(
						Mat4::translate(endPoints[i].x, endPoints[i].z,
										worldZFromEngineY(endPoints[i].y)),
						Mat4::scale(0.5f));
					glUniformMatrix4fv(meshMvpLoc, 1, GL_FALSE, Mat4::multiply(mvp, model).m);
					frameDrawCalls++; glDrawArrays(GL_TRIANGLES, 0, sphereVertexCount);
				}
			}

			if (!paths.empty()) {
				// Upstream draws the path as a textured ribbon; a line strip
				// reads the same at these widths and needs no texture.
				std::vector<float> line;
				for (size_t p = 0; p < paths.size(); p++) {
					const std::vector<ScorchDroidTracer::Point> &path = paths[p];
					for (size_t i = 0; i + 1 < path.size(); i++) {
						const ScorchDroidTracer::Point &a = path[i];
						const ScorchDroidTracer::Point &b = path[i + 1];
						line.push_back(a.x); line.push_back(a.z);
						line.push_back(worldZFromEngineY(a.y));
						line.push_back(myColorR); line.push_back(myColorG); line.push_back(myColorB);
						line.push_back(b.x); line.push_back(b.z);
						line.push_back(worldZFromEngineY(b.y));
						line.push_back(myColorR); line.push_back(myColorG); line.push_back(myColorB);
					}
				}
				if (!line.empty()) {
					glUseProgram(sightProgram);
		setFixedFunctionFog(sightFogColorLoc, sightFogDensityLoc);
					glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, mvp.m);
					glBindVertexArray(beamVao);
					glBindBuffer(GL_ARRAY_BUFFER, beamVbo);
					glBufferData(GL_ARRAY_BUFFER, line.size() * sizeof(float), line.data(), GL_DYNAMIC_DRAW);
					glLineWidth(2.0f);
					frameDrawCalls++; glDrawArrays(GL_LINES, 0, (GLsizei) (line.size() / 6));
				}
			}
		}

		glBindVertexArray(0);
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
	}

	// Effects last, so they blend additively over the finished scene.
	drawEffects(mvp, view, eyeX, eyeY, eyeZ, kFovYRadians);

	// Project each tank to screen space for the Compose name plates. Done
	// here rather than in Kotlin because this is the only place that has
	// the finished MVP, and repeating the camera maths on the UI thread
	// would be a second implementation to keep in step.
	{
		std::vector<TankOverlay> overlays;
		overlays.reserve(tankInstances.size());
		for (TankInstance &inst : tankInstances) {
			// Nothing at all for a tank upstream wouldn't draw. Its
			// drawParticle starts with `if (!getVisible()) return;`, and
			// Tank::getVisible() is `getAlive() || state == sBuying` - so a
			// destroyed tank has no name, no life bar and no off-screen
			// arrow, and the `!= sNormal` branch that draws a bare name is
			// reached only by a tank that is alive but shopping.
			//
			// This port previously kept a dimmed plate over a destroyed
			// tank, on the reading that upstream falls through to
			// drawNames() for any non-normal tank. That reading missed the
			// getVisible() guard above it, and on screen it left a player's
			// name hanging over an empty crater.
			if (!inst.visible) continue;

			// Anchor above the tank, like upstream's own name billboard
			// (drawNames puts it at height + 8).
			const float wx = inst.x, wy = inst.y + 4.0f, wz = inst.z;
			const float cx = mvp.m[0] * wx + mvp.m[4] * wy + mvp.m[8]  * wz + mvp.m[12];
			const float cy = mvp.m[1] * wx + mvp.m[5] * wy + mvp.m[9]  * wz + mvp.m[13];
			const float cw = mvp.m[3] * wx + mvp.m[7] * wy + mvp.m[11] * wz + mvp.m[15];

			TankOverlay overlay;
			overlay.alive = inst.alive;
			overlay.mine = inst.mine;
			overlay.life = inst.life;
			overlay.shield = inst.shield;
			overlay.r = inst.colorR; overlay.g = inst.colorG; overlay.b = inst.colorB;
			overlay.name = inst.name;

			// w <= 0 is behind the eye, where the perspective divide flips
			// the result and would place the plate on the opposite side of
			// the screen.
			if (cw > 0.0001f) {
				const float ndcX = cx / cw, ndcY = cy / cw;
				overlay.screenX = (ndcX * 0.5f + 0.5f) * (float) surfaceWidth;
				overlay.screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * (float) surfaceHeight;
				overlay.onScreen =
					overlay.screenX >= 0.0f && overlay.screenX <= (float) surfaceWidth &&
					overlay.screenY >= 0.0f && overlay.screenY <= (float) surfaceHeight;
			}
			overlays.push_back(overlay);
		}
		std::lock_guard<std::mutex> lock(g_overlayMutex);
		g_tankOverlays.swap(overlays);
	}

	// The floating labels take the same projection as the plates above.
	{
		std::vector<FloatingLabel> labels;
		labels.reserve(floatingLabels.size());
		for (FloatingLabel label : floatingLabels) {
			const float cx = mvp.m[0] * label.x + mvp.m[4] * label.y + mvp.m[8]  * label.z + mvp.m[12];
			const float cy = mvp.m[1] * label.x + mvp.m[5] * label.y + mvp.m[9]  * label.z + mvp.m[13];
			const float cw = mvp.m[3] * label.x + mvp.m[7] * label.y + mvp.m[11] * label.z + mvp.m[15];
			if (cw > 0.0001f) {
				const float ndcX = cx / cw, ndcY = cy / cw;
				label.screenX = (ndcX * 0.5f + 0.5f) * (float) surfaceWidth;
				label.screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * (float) surfaceHeight;
				label.onScreen =
					label.screenX >= 0.0f && label.screenX <= (float) surfaceWidth &&
					label.screenY >= 0.0f && label.screenY <= (float) surfaceHeight;
			}
			labels.push_back(label);
		}
		std::lock_guard<std::mutex> lock(g_labelMutex);
		g_labelOverlays.swap(labels);
	}
}

// M6: battlefield touch now drives the orbit camera (see the file-level
// comment above for why tap-to-fire-at-a-point doesn't carry over as-is) -
// called from MainActivity's touch handling on the GLSurfaceView.
// M6 terrain picking: turns a screen tap into a landscape (x, y), or "" if
// the ray misses the ground. Upstream does the same thing with
// GroundMaps::getIntersect against a Line from the camera
// (TankKeyboardControlUtil::autoAim); this marches the heightmap directly,
// which needs no client camera class.
//
// Returned in landscape coordinates because that is what every consumer
// wants - aiming, and eventually tank movement, both talk to the engine.
// M6 perf readout: "fps|drawCalls|targets" for the HUD. A development aid
// rather than a player-facing feature - gate it before any release.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeGetFrameStats(JNIEnv *env, jobject) {
	float fps;
	int calls, targets;
	{
		std::lock_guard<std::mutex> lock(g_statsMutex);
		fps = smoothedFps;
		calls = lastFrameDrawCalls;
		targets = lastTargetsDrawn;
	}
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.0f|%d|%d", fps, calls, targets);
	return env->NewStringUTF(buffer);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativePickTerrain(JNIEnv *env, jobject, jfloat screenX, jfloat screenY) {
	PickCamera camera;
	{
		std::lock_guard<std::mutex> lock(g_pickMutex);
		camera = g_pickCamera;
	}
	if (!camera.valid || surfaceWidth <= 0 || surfaceHeight <= 0) return env->NewStringUTF("");

	// Screen pixel -> normalised device coords -> a ray through that pixel.
	const float ndcX = (screenX / (float) surfaceWidth) * 2.0f - 1.0f;
	const float ndcY = 1.0f - (screenY / (float) surfaceHeight) * 2.0f;
	const float sx = ndcX * camera.aspect * camera.tanHalfFov;
	const float sy = ndcY * camera.tanHalfFov;

	float dx = camera.fwdX + camera.rightX * sx + camera.upX * sy;
	float dy = camera.fwdY + camera.rightY * sx + camera.upY * sy;
	float dz = camera.fwdZ + camera.rightZ * sx + camera.upZ * sy;
	const float dlen = sqrtf(dx * dx + dy * dy + dz * dz);
	if (dlen < 0.0001f) return env->NewStringUTF("");
	dx /= dlen; dy /= dlen; dz /= dlen;

	std::lock_guard<std::mutex> lock(g_engineMutex);
	ScorchedContext *ctx = engineActiveContext();
	if (!ctx || !terrainBuilt) return env->NewStringUTF("");

	// March until the ray passes below the ground, then bisect. A fixed
	// step is fine at this scale and avoids the cliff-skipping a coarse
	// adaptive march can suffer; the bisect gives back the precision.
	const float maxDistance = std::max(mapWidthUnits, mapHeightUnits) * 3.0f;
	const float step = 0.5f;
	float previous = 0.0f;
	bool wasAbove = (camera.eyeY >= terrainHeightAt(*ctx, camera.eyeX, camera.eyeZ));

	for (float travelled = step; travelled < maxDistance; travelled += step) {
		const float px = camera.eyeX + dx * travelled;
		const float py = camera.eyeY + dy * travelled;
		const float pz = camera.eyeZ + dz * travelled;
		const bool above = (py >= terrainHeightAt(*ctx, px, pz));

		if (wasAbove && !above) {
			// Bisect between the last two samples for a stable hit point.
			float lo = previous, hi = travelled;
			for (int i = 0; i < 24; i++) {
				const float mid = (lo + hi) * 0.5f;
				const float mx = camera.eyeX + dx * mid;
				const float my = camera.eyeY + dy * mid;
				const float mz = camera.eyeZ + dz * mid;
				if (my >= terrainHeightAt(*ctx, mx, mz)) lo = mid; else hi = mid;
			}
			const float hitX = camera.eyeX + dx * lo;
			const float hitZ = camera.eyeZ + dz * lo;
			if (hitX < 0.0f || hitX > mapWidthUnits || hitZ < 0.0f || hitZ > mapHeightUnits) {
				return env->NewStringUTF("");
			}
			char buffer[64];
			snprintf(buffer, sizeof(buffer), "%.2f|%.2f",
					 hitX, engineYFromWorldZ(hitZ));
			return env->NewStringUTF(buffer);
		}
		wasAbove = above;
		previous = travelled;
	}
	return env->NewStringUTF("");
}

// M6 name plates: the last frame's projected tank positions, one row each:
// "screenX|screenY|onScreen|alive|mine|life|shield|r|g|b|name". Kotlin draws
// the plates (see GameHud) - this port has no GL font renderer and its UI is
// Compose, so text stays on that side.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeGetTankOverlays(JNIEnv *env, jobject) {
	std::vector<TankOverlay> snapshot;
	{
		std::lock_guard<std::mutex> lock(g_overlayMutex);
		snapshot = g_tankOverlays;
	}

	jclass stringClass = env->FindClass("java/lang/String");
	jobjectArray result = env->NewObjectArray((jsize) snapshot.size(), stringClass, nullptr);
	for (size_t i = 0; i < snapshot.size(); i++) {
		const TankOverlay &o = snapshot[i];
		char buffer[512];
		snprintf(buffer, sizeof(buffer), "%.1f|%.1f|%d|%d|%d|%.3f|%.3f|%.3f|%.3f|%.3f|%s",
				 o.screenX, o.screenY, o.onScreen ? 1 : 0, o.alive ? 1 : 0, o.mine ? 1 : 0,
				 o.life, o.shield, o.r, o.g, o.b, o.name.c_str());
		jstring row = env->NewStringUTF(buffer);
		env->SetObjectArrayElement(result, (jsize) i, row);
		env->DeleteLocalRef(row);
	}
	return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeCameraDrag(JNIEnv *, jobject, jfloat dx, jfloat dy) {
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	// A drag means the player wants to look somewhere themselves, so it
	// drops out of any fixed preset rather than fighting it - the preset
	// recomputes the framing every frame, so a drag inside one would be
	// silently discarded and read as a dead control.
	if (g_camera.preset >= OrbitCamera::pTop) {
		g_camera.preset = g_camera.followMode ? OrbitCamera::pFollow : OrbitCamera::pFree;
	}
	// Both axes turn the view the way the ground would go if you had hold
	// of it - the same sense as the two-finger pan, which moves the target
	// against the finger so the world follows it.
	//
	// Horizontal was `+= dx` and was reported backwards: the eye orbits
	// target + d*(cos p * sin yaw, sin p, cos p * cos yaw), so screen-right
	// is (cos yaw, 0, -sin yaw) and a point on the near side of the target
	// has d(screenX)/d(yaw) < 0 - increasing yaw as the finger moves right
	// pushes the world left. Subtracting makes the world follow the finger,
	// and matches the vertical axis, which was flipped for the same reason
	// (drag down and the far side of the ground tips up towards you; the
	// opposite was tried first and also reported backwards).
	g_camera.yaw -= dx * kDragSensitivity;
	g_camera.pitch = std::min(std::max(g_camera.pitch + dy * kDragSensitivity, kMinPitch), kMaxPitch);
}

// M6: two-finger drag slides the free-fly camera's look-at point across the
// ground. Without this "free-fly" was really "orbit the map centre" - the
// target was set once from the map size and never moved, so you could
// circle the middle of the map and zoom, but never go and look somewhere
// else. Follow mode ignores this deliberately: it retargets to the tank
// every frame, so any pan would be overwritten on the very next one.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeCameraPan(JNIEnv *, jobject, jfloat dx, jfloat dy) {
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	if (g_camera.followMode) return;

	// Screen-relative, not world-relative: dragging right has to move the
	// view right whichever way the camera is currently facing, so the drag
	// is rotated into the camera's own yaw. Screen-up maps to *away* from
	// the camera along the ground, hence the second axis being the forward
	// one rather than a second right vector.
	const float sinYaw = sinf(g_camera.yaw), cosYaw = cosf(g_camera.yaw);
	const float rightX = cosYaw,  rightZ = -sinYaw;
	const float fwdX   = -sinYaw, fwdZ   = -cosYaw;

	// Move the world under the finger at 1:1 rather than at some tuned
	// rate: the visible world height at the target's distance is
	// 2*d*tan(fov/2), so dividing by the viewport height gives world units
	// per pixel exactly. That makes panning feel like dragging the map
	// itself, and stays right at any zoom or screen size, with nothing to
	// re-tune. (Exact only for ground parallel to the view plane - the
	// ground is pitched away, so it drifts slightly at shallow angles,
	// which is not noticeable in practice.)
	const float visibleHeight = 2.0f * g_camera.orbitDistance * tanf(kFovYRadians * 0.5f);
	const float scale = visibleHeight / (float) std::max(surfaceHeight, 1);

	// Vertical drag is inverted relative to the horizontal one, matching
	// the pitch direction in nativeCameraDrag - the first version moved the
	// view the other way and was reported backwards. Negated once here
	// rather than by flipping a sign in each axis below, so the two lines
	// stay symmetrical and can't drift apart.
	const float dragUp = -dy;
	g_camera.targetX -= (rightX * dx + fwdX * dragUp) * scale;
	g_camera.targetZ -= (rightZ * dx + fwdZ * dragUp) * scale;

	// Keep the target near the map. Panning off into empty space is never
	// useful and is easy to do by accident, leaving nothing on screen and
	// no obvious way back.
	const float marginX = mapWidthUnits * 0.25f;
	const float marginZ = mapHeightUnits * 0.25f;
	g_camera.targetX = std::min(std::max(g_camera.targetX, -marginX), mapWidthUnits + marginX);
	g_camera.targetZ = std::min(std::max(g_camera.targetZ, -marginZ), mapHeightUnits + marginZ);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeCameraZoom(JNIEnv *, jobject, jfloat scaleFactor) {
	if (scaleFactor <= 0.0f) return;
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	if (g_camera.followMode) {
		g_camera.followDistance = std::min(std::max(g_camera.followDistance / scaleFactor, kMinDistance), kMaxFollowDistance);
	} else {
		float maxDistance = std::max(mapWidthUnits, mapHeightUnits) * 2.5f + 50.0f;
		g_camera.orbitDistance = std::min(std::max(g_camera.orbitDistance / scaleFactor, kMinDistance), maxDistance);
	}
}

// M6: toggles between free-fly (orbit the map) and third-person-follow
// (orbit "my tank", retargeted every frame - see nativeOnDrawFrame) camera
// modes - the per-user toggle from the porting plan's camera-style
// decision. Returns the new mode (true = follow) so the Kotlin caller can
// update the HUD label without a separate query call. A host-enforced
// "force everyone into follow mode" option is deliberately not done here -
// see the porting plan's M6 notes for why that's a separate, bigger piece
// of work (a real game-option + network sync + UI, not just a rendering
// change).
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeToggleCameraMode(JNIEnv *, jobject) {
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	g_camera.followMode = !g_camera.followMode;
	// The button toggles between this port's own two modes, so it also
	// leaves any fixed preset - otherwise the toggle would appear to do
	// nothing while a preset kept overriding the framing.
	g_camera.preset = g_camera.followMode ? OrbitCamera::pFollow : OrbitCamera::pFree;
	return g_camera.followMode ? JNI_TRUE : JNI_FALSE;
}

// M6 parity: pick one of upstream's camera presets (TargetCamera::CamType).
// 0/1 are this port's own Free and Follow - the camera button's two - and
// setting either just restores that mode; 2 and up are the fixed framings.
// A drag drops back out of a fixed one (see nativeCameraDrag).
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeSetCameraPreset(JNIEnv *, jobject, jint preset) {
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	if (preset < 0 || preset > OrbitCamera::pShot) return;
	g_camera.preset = (OrbitCamera::Preset) preset;
	// Free and Follow are the existing toggle's two states, so selecting
	// one has to move the toggle with it or the camera button would show
	// the wrong icon.
	if (g_camera.preset == OrbitCamera::pFree) g_camera.followMode = false;
	if (g_camera.preset == OrbitCamera::pFollow) g_camera.followMode = true;
}

// M6: the short-lived world-anchored labels - floating damage numbers and
// speech bubbles. Rows are "screenX|screenY|onScreen|fade|r|g|b|text"; text
// is last so it may contain pipes. Drawn in Compose because this renderer
// has no font, exactly as the tank name plates are.
// M11: renderer options from the settings screen. Takes effect on the next
// frame - nothing here is baked into a buffer.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_setRenderOptions(
        JNIEnv *env, jobject, jboolean showTrees, jboolean showFog) {
    g_showTrees = (showTrees == JNI_TRUE);
    g_showFog = (showFog == JNI_TRUE);
}

// M23: how finely the landscape is drawn, as a grid resolution. Takes
// effect on the next landscape build, which the renderer forces as soon as
// it sees the value change.
// W3: how much the water reflects - 0 for the sky's own colours, 1 for the
// sky and the land, 2 for everything upstream reflects.
//
// Clamped to the range rather than tested against a single value: this was
// `style == 1 ? 1 : 0` when it grew a third setting, which quietly mapped
// "Everything" back onto "Sky" - the top of the slider turned reflections
// off altogether, and looked exactly like a setting that had no effect.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_setReflectionStyle(JNIEnv *env, jobject, jint style) {
    const int level = std::min(std::max((int) style, 0), 2);
    g_reflectionStyle.store(level);
    // Logged because the failure above was invisible: the setting moved, the
    // renderer did nothing, and there was no way to tell which end was wrong.
    LOGI("Water reflections: level %d (asked for %d)", level, (int) style);
}

// The sun's shadow map: 0 off, 1 for a 1024 map, 2 for upstream's own 2048.
//
// Off is not "no lighting": it is upstream's own no-shadow path, where the
// sun and the shadows hills cast on each other are baked into the ground
// texture instead. So the terrain looks lit either way; what the map adds is
// everything a baked texture cannot hold - an island's shadow falling on the
// sea beside it, a tank's on the ground, and shadows that move with the sun.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_setShadowDetail(JNIEnv *env, jobject, jint level) {
    const int clamped = std::min(std::max((int) level, 0), 2);
    g_shadowLevel.store(clamped);
    LOGI("Shadow detail: level %d (%s)", clamped,
         clamped == 0 ? "off, light map baked" : (clamped == 1 ? "1024" : "2048"));
}

// How many particles may be alight at once, as upstream's effects detail
// setting - its own three sizes, from ScorchedClient's particle engine.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_setEffectsDetail(JNIEnv *env, jobject, jint level) {
    const int budget = (level <= 0) ? 100 : (level == 1 ? 6000 : 10000);
    g_maxParticles.store(budget);
    LOGI("Effects detail: level %d, %d particles", (int) level, budget);
}

// Water detail: 2 Full (upstream's own grid and phase rate), 1 Half,
// 0 Quarter. See g_waterDetail.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_setWaterDetail(JNIEnv *env, jobject, jint level) {
    g_waterDetail.store(level < 0 ? 0 : (level > 2 ? 2 : level));
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_setTerrainDetail(JNIEnv *env, jobject, jint grid) {
    g_requestedTerrainGrid.store(grid);
}

// The range the setting may ask for, as "min|max", so the slider does not
// have to repeat numbers this file owns.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getTerrainDetailRange(JNIEnv *env, jobject) {
    std::ostringstream out;
    out << kTerrainGridMin << "|" << kTerrainGridMax;
    return env->NewStringUTF(out.str().c_str());
}

// M22: which aim sight to draw - 0 for this port's own blade, 1 for
// upstream's arrangement of a protractor ring, a bearing marker on the
// ground and a blade along the barrel.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_setSightStyle(JNIEnv *env, jobject, jint style) {
    g_sightStyle.store(style == 1 ? 1 : 0);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeGetFloatingLabels(JNIEnv *env, jobject) {
    std::vector<FloatingLabel> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_labelMutex);
        snapshot = g_labelOverlays;
    }

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray((jsize) snapshot.size(), stringClass, nullptr);
    for (size_t i = 0; i < snapshot.size(); i++) {
        const FloatingLabel &label = snapshot[i];
        // Fade over the last part of the life, so a number leaves rather
        // than blinking out.
        const float remaining = 1.0f - (label.age / label.life);
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%.1f|%.1f|%d|%.3f|%.3f|%.3f|%.3f|%s",
                 label.screenX, label.screenY, label.onScreen ? 1 : 0,
                 std::min(remaining * 2.0f, 1.0f),
                 label.r, label.g, label.b, label.text.c_str());
        jstring row = env->NewStringUTF(buffer);
        env->SetObjectArrayElement(result, (jsize) i, row);
        env->DeleteLocalRef(row);
    }
    return result;
}

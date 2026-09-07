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
#include <mutex>
#include <algorithm>
#include <cmath>

#include <engine/ScorchedContext.hpp>
#include <target/TargetContainer.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <landscapedef/LandscapeDefinitionCache.hpp>
#include <landscapedef/LandscapeDefinition.hpp>
#include <landscapedef/LandscapeTex.hpp>
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
#include <EngineState.hpp>
#include <Mat4.hpp>
#include <LandscapeTextureBuilder.hpp>
#include <MovementStore.h>
#include <TargetModelStore.h>
#include <SkyDescription.hpp>
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
	GLint  terrainMvpLoc = -1, terrainMinHeightLoc = -1, terrainHeightRangeLoc = -1, terrainLightDirLoc = -1;
	GLint  terrainGroundTexLoc = -1, terrainHasTextureLoc = -1;
	GLuint groundTexture = 0;
	bool   groundTextureBuilt = false;
	// M6 scorch marks: the CPU-side copy of the ground texture is kept, not
	// discarded after upload, because each blast blends into the *result of*
	// every earlier one - marks accumulate over a round the way upstream's
	// do. Re-reading it back off the GPU each time would be far worse.
	LandscapeTextureBuilder::Texture groundTextureData;
	// M6 tank movement: the version of ScorchDroidMovement's mask currently
	// painted over the ground, and whether anything is painted at all. The
	// mask itself is recomputed on the simulation side (see engine_jni's
	// refreshMovementMask); this side only notices the version change and
	// re-uploads.
	// M6 sky.
	GLuint skyProgram = 0, skyVao = 0, skyVbo = 0;
	GLint  skyGradientLoc = -1, skySunDirLoc = -1, skySunColorLoc = -1, skyGlowLoc = -1;
	bool   skyBuilt = false;
	ScorchDroidSky::Description skyDescription;

	// M6 water surface: one quad at the landscape's own water height.
	GLuint waterProgram = 0, waterVao = 0, waterVbo = 0;
	GLint  waterMvpLoc = -1, waterDeepLoc = -1, waterShallowLoc = -1;
	GLint  waterAlphaLoc = -1, waterTimeLoc = -1;
	bool   waterBuilt = false;    // one attempt per landscape, success or not
	bool   waterVisible = false;  // this landscape actually has water
	float  waterHeight = 0.0f;
	float  waterDeep[3] = { 0.11f, 0.26f, 0.45f };
	float  waterShallow[3] = { 0.29f, 0.56f, 0.91f };
	float  waterAlpha = 0.8f;

	unsigned int paintedMovementVersion = 0;
	bool movementOverlayPainted = false;
	GLint  pointMvpLoc = -1, pointColorLoc = -1, pointSizeLoc = -1;
	GLuint meshProgram = 0;
	GLint  meshMvpLoc = -1, meshLightDirLoc = -1, meshColorLoc = -1;
	GLuint sightProgram = 0, sightVao = 0, sightVbo = 0;
	GLint  sightMvpLoc = -1;

	GLuint particleProgram = 0, particleVao = 0, particleVbo = 0;
	GLint  particleMvpLoc = -1;
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
	};

	struct Beam {
		float x1, y1, z1, x2, y2, z2;  // render space
		float r, g, b;
		float age, life;
	};

	std::vector<Particle> particles;
	std::vector<Beam> beams;
	double lastFrameSeconds = 0.0;

	// Plenty for several simultaneous blasts, and a hard stop so a napalm
	// field can't grow the buffer without limit on a slow device.
	const size_t kMaxParticles = 4000;
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
	// rebuilding the whole mesh. kGrid x kGrid quads => (kGrid+1)^2 verts.
	constexpr int kGrid = 96;
	constexpr int kTerrainVerts1D = kGrid + 1;

	// Which heightmap cell a mesh grid vertex samples. The rows run
	// backwards for the same reason worldZFromEngineY subtracts: grid row 0
	// sits at world Z = 0, which is landscape y = mapHeight.
	inline int heightMapRowForGridZ(int gz, int mapH) {
		return std::min(std::max(mapH - gz * mapH / kGrid, 0), mapH - 1);
	}
	inline int heightMapColForGridX(int gx, int mapW) {
		return std::min(std::max(gx * mapW / kGrid, 0), mapW - 1);
	}
	// ...and back, for turning a deformed heightmap region into the grid
	// rows that need re-sampling.
	inline int gridZForHeightMapRow(int row, int mapH) {
		return (mapH > 0) ? ((mapH - row) * kGrid / mapH) : 0;
	}
	inline int gridXForHeightMapCol(int col, int mapW) {
		return (mapW > 0) ? (col * kGrid / mapW) : 0;
	}
	constexpr int kTerrainFloatsPerVertex = 8;  // pos(3) + normal(3) + uv(2)
	std::vector<float> terrainHeights;          // kTerrainVerts1D^2, row-major by gz
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
	} g_camera;

	constexpr float kMinPitch = 0.15f;
	constexpr float kMaxPitch = 1.45f;
	constexpr float kMinDistance = 5.0f;
	// How far the camera stays above the ground beneath it - comfortably
	// more than the near plane, so nothing clips even on a steep slope.
	constexpr float kCameraGroundClearance = 4.0f;
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

	GLuint linkProgram(const char *vs, const char *fs)
	{
		GLuint v = compileShader(GL_VERTEX_SHADER, vs);
		GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
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
		out vec3 vNormal;
		out float vHeight01;
		out vec2 vTexCoord;
		void main() {
			vNormal = aNormal;
			vTexCoord = aTexCoord;
			vHeight01 = clamp((aPosition.y - uMinHeight) / uHeightRange, 0.0, 1.0);
			gl_Position = uMVP * vec4(aPosition, 1.0);
		}
	)";

	// M6: the ground is now the real generated landscape texture (see
	// LandscapeTextureBuilder - grass/rock/sand blended by height and
	// slope, the same way upstream builds it), lit by the same directional
	// light as before. uHasTexture falls back to the old flat height-ramp
	// colouring if the landscape definition doesn't use generated textures
	// or the images failed to load, so the ground is never invisible.
	const char *kTerrainFragmentShader = R"(#version 300 es
		precision mediump float;
		in vec3 vNormal;
		in float vHeight01;
		in vec2 vTexCoord;
		out vec4 fragColor;
		uniform vec3 uLightDir;
		uniform sampler2D uGroundTexture;
		uniform int uHasTexture;
		void main() {
			vec3 n = normalize(vNormal);
			float diffuse = max(dot(n, uLightDir), 0.0);
			vec3 baseColor;
			if (uHasTexture == 1) {
				baseColor = texture(uGroundTexture, vTexCoord).rgb;
			} else {
				baseColor = mix(vec3(0.22, 0.34, 0.13), vec3(0.58, 0.52, 0.42), vHeight01);
			}
			vec3 lit = baseColor * (0.55 + diffuse * 0.6);
			fragColor = vec4(lit, 1.0);
		}
	)";

	// M6: real .ase models (tanks, and later projectiles). Flat-lit with the
	// same directional light as the terrain, plus a per-instance colour so
	// "my tank" stays visually distinct from opponents the way the old
	// point sprites were.
	const char *kMeshVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec3 aNormal;
		uniform mat4 uMVP;
		out vec3 vNormal;
		void main() {
			vNormal = aNormal;
			gl_Position = uMVP * vec4(aPosition, 1.0);
		}
	)";

	const char *kMeshFragmentShader = R"(#version 300 es
		precision mediump float;
		in vec3 vNormal;
		out vec4 fragColor;
		uniform vec3 uLightDir;
		uniform vec4 uColor;
		void main() {
			vec3 n = normalize(vNormal);
			float diffuse = max(dot(n, uLightDir), 0.0);
			fragColor = vec4(uColor.rgb * (0.45 + diffuse * 0.75), uColor.a);
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
		void main() {
			vColor = aColor;
			gl_Position = uMVP * vec4(aPosition, 1.0);
		}
	)";

	const char *kSightFragmentShader = R"(#version 300 es
		precision mediump float;
		in vec3 vColor;
		out vec4 fragColor;
		void main() { fragColor = vec4(vColor, 1.0); }
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
		precision mediump float;
		in vec3 vRay;
		out vec4 fragColor;
		uniform vec3 uGradient[16];
		uniform vec3 uSunDir;
		uniform vec3 uSunColor;
		uniform float uHorizonGlow;
		void main() {
			vec3 d = normalize(vRay);

			// Upstream's gradient is indexed by height above the horizon.
			// Below it there is nothing to show but the horizon colour -
			// the ground is drawn over that anyway, and clamping avoids a
			// hard band when the camera dips.
			float t = clamp(d.y, 0.0, 1.0) * 15.0;
			int lo = int(floor(t));
			int hi = min(lo + 1, 15);
			vec3 sky = mix(uGradient[lo], uGradient[hi], fract(t));

			// The sun itself, then its halo. Two powers rather than one so
			// the disc stays tight while the glow spreads.
			float toSun = max(dot(d, uSunDir), 0.0);
			sky += uSunColor * pow(toSun, 256.0) * 2.0;
			sky += uSunColor * pow(toSun, 12.0) * 0.35 * uHorizonGlow;

			fragColor = vec4(sky, 1.0);
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
	// Deliberately not a texture: what sells water at a glance is that it
	// moves and that it is flat where the land is not. Two of the
	// definition's own colours crossfaded by a pair of slow, non-commensurate
	// sine waves gives that for a few instructions, with no image to load
	// and nothing to keep in step with the ground texture.
	const char *kWaterVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		uniform mat4 uMVP;
		out vec2 vWorld;
		void main() {
			vWorld = aPosition.xz;
			gl_Position = uMVP * vec4(aPosition, 1.0);
		}
	)";

	const char *kWaterFragmentShader = R"(#version 300 es
		precision mediump float;
		in vec2 vWorld;
		out vec4 fragColor;
		uniform vec3 uDeepColor;
		uniform vec3 uShallowColor;
		uniform float uAlpha;
		uniform float uTime;
		void main() {
			// Two waves at different angles and speeds, deliberately not
			// harmonics of each other - a single sine reads as corduroy.
			float a = sin(vWorld.x * 0.09 + uTime * 0.7);
			float b = sin((vWorld.x * 0.4 + vWorld.y * 0.9) * 0.05 - uTime * 0.5);
			float crest = clamp(0.5 + 0.25 * a + 0.25 * b, 0.0, 1.0);
			fragColor = vec4(mix(uDeepColor, uShallowColor, crest), uAlpha);
		}
	)";

	// M6 effects: one additive, soft-edged round sprite per particle.
	// Size and colour are per-vertex because a single explosion mixes both
	// (a bright small core with dimmer larger debris), which a uniform
	// could not express without a draw call each.
	const char *kParticleVertexShader = R"(#version 300 es
		layout(location = 0) in vec3 aPosition;
		layout(location = 1) in vec4 aColor;
		layout(location = 2) in float aSize;
		uniform mat4 uMVP;
		out vec4 vColor;
		void main() {
			vColor = aColor;
			gl_Position = uMVP * vec4(aPosition, 1.0);
			gl_PointSize = aSize;
		}
	)";

	const char *kParticleFragmentShader = R"(#version 300 es
		precision mediump float;
		in vec4 vColor;
		out vec4 fragColor;
		void main() {
			// Round the square point sprite off and fade towards its edge,
			// so particles read as soft puffs rather than tiles.
			vec2 offset = gl_PointCoord - vec2(0.5);
			float r = length(offset) * 2.0;
			if (r > 1.0) discard;
			float falloff = 1.0 - r * r;
			fragColor = vec4(vColor.rgb, vColor.a * falloff);
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
		precision mediump float;
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
	void writeTerrainVertex(int gx, int gz, float *out)
	{
		const int last = kTerrainVerts1D - 1;
		float py = terrainHeights[gz * kTerrainVerts1D + gx];
		float pxL = terrainWorldX[std::max(gx - 1, 0)];
		float pxR = terrainWorldX[std::min(gx + 1, last)];
		float hL = terrainHeights[gz * kTerrainVerts1D + std::max(gx - 1, 0)];
		float hR = terrainHeights[gz * kTerrainVerts1D + std::min(gx + 1, last)];
		float pzT = terrainWorldZ[std::max(gz - 1, 0)];
		float pzB = terrainWorldZ[std::min(gz + 1, last)];
		float hT = terrainHeights[std::max(gz - 1, 0) * kTerrainVerts1D + gx];
		float hB = terrainHeights[std::min(gz + 1, last) * kTerrainVerts1D + gx];

		// Tangent along +X and along +Z, then normal = normalize(tZ x tX)
		// (chosen order/signs give an outward/up-facing normal for a
		// heightmap in this y-up, right-handed world).
		float tXx = pxR - pxL, tXy = hR - hL, tXz = 0.0f;
		float tZx = 0.0f, tZy = hB - hT, tZz = pzB - pzT;
		float nx = tZy * tXz - tZz * tXy;
		float ny = tZz * tXx - tZx * tXz;
		float nz = tZx * tXy - tZy * tXx;
		float nLen = sqrtf(nx * nx + ny * ny + nz * nz);
		if (nLen < 1e-6f) { nx = 0; ny = 1; nz = 0; } else { nx /= nLen; ny /= nLen; nz /= nLen; }

		out[0] = terrainWorldX[gx];
		out[1] = py;
		out[2] = terrainWorldZ[gz];
		out[3] = nx;
		out[4] = ny;
		out[5] = nz;
		// UV spans the whole landscape once - the ground texture is
		// generated per-landscape at map resolution, not tiled here
		// (LandscapeTextureBuilder already tiles its sources).
		out[6] = (float) gx / (float) kGrid;
		// V runs backwards for the same reason the heightmap rows do: the
		// ground texture is generated in landscape orientation (row index =
		// landscape y - see LandscapeTextureBuilder, and the scorch marks
		// painted into it), while world Z runs the other way.
		out[7] = 1.0f - (float) gz / (float) kGrid;
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
		if (terrainBuilt && defnNumber == builtDefinitionNumber) return;
		if (terrainBuilt && defnNumber != builtDefinitionNumber) {
			// Drop the old landscape's GL objects before rebuilding.
			if (terrainVao) glDeleteVertexArrays(1, &terrainVao);
			if (terrainVbo) glDeleteBuffers(1, &terrainVbo);
			if (terrainIbo) glDeleteBuffers(1, &terrainIbo);
			if (groundTexture) glDeleteTextures(1, &groundTexture);
			terrainVao = terrainVbo = terrainIbo = groundTexture = 0;
			groundTextureData = LandscapeTextureBuilder::Texture();
			terrainBuilt = false;
			groundTextureBuilt = false;
			// The new landscape gets a freshly built texture, so whatever
			// was painted over the old one is gone with it - and the
			// version has to be forced to re-sync, or a mask published
			// before this rebuild would never be painted.
			movementOverlayPainted = false;
			paintedMovementVersion = 0;
			// Water and sky are per-landscape too.
			waterBuilt = false;
			waterVisible = false;
			skyBuilt = false;
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

		const int verts1D = kTerrainVerts1D;
		terrainSrcWidth = w;
		terrainSrcHeight = h;
		// Set before anything below converts a landscape coordinate -
		// worldZFromEngineY reads mapHeightUnits.
		mapWidthUnits = (float) w;
		mapHeightUnits = (float) h;
		terrainHeights.assign(verts1D * verts1D, 0.0f);
		terrainWorldX.assign(verts1D, 0.0f);
		terrainWorldZ.assign(verts1D, 0.0f);
		terrainMinHeight = 1e9f;
		terrainMaxHeight = -1e9f;
		for (int i = 0; i < verts1D; i++) {
			terrainWorldX[i] = (float) i / (float) kGrid * (float) w;
			terrainWorldZ[i] = (float) i / (float) kGrid * (float) h;
		}
		for (int gz = 0; gz < verts1D; gz++) {
			int sy = heightMapRowForGridZ(gz, h);
			for (int gx = 0; gx < verts1D; gx++) {
				int sx = heightMapColForGridX(gx, w);
				float height = heightMap.getHeight(sx, sy).asFloat();
				terrainHeights[gz * verts1D + gx] = height;
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
		indices.reserve(kGrid * kGrid * 6);
		for (int gz = 0; gz < kGrid; gz++) {
			for (int gx = 0; gx < kGrid; gx++) {
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

	// M6: generates and uploads the real ground texture once a landscape
	// exists (see LandscapeTextureBuilder). Failure is non-fatal - the
	// terrain shader falls back to its old height-ramp colouring rather
	// than drawing nothing, so a landscape definition we can't texture
	// still renders.
	void buildGroundTextureIfNeeded(ScorchedContext &ctx)
	{
		if (groundTextureBuilt) return;
		groundTextureBuilt = true;  // one attempt per landscape, success or not

		std::string groundError;
		groundTextureData = LandscapeTextureBuilder::build(ctx, 512, &groundError);
		LandscapeTextureBuilder::Texture &ground = groundTextureData;
		if (!ground.valid()) {
			LOGE("ground texture generation failed (%s) - falling back to flat height colours", groundError.c_str());
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
		LOGI("Ground texture built: %dx%d", ground.width, ground.height);
	}

	// M6 sky: the landscape's own sky colours, read once per landscape.
	void buildSkyIfNeeded(ScorchedContext &ctx)
	{
		if (skyBuilt) return;
		skyBuilt = true;
		skyDescription = ScorchDroidSky::describe(ctx);
		if (skyDescription.valid) {
			LOGI("Sky: gradient loaded, sun towards (%.2f, %.2f, %.2f), glow %d",
				 skyDescription.sunDirection[0], skyDescription.sunDirection[1],
				 skyDescription.sunDirection[2], skyDescription.horizonGlow ? 1 : 0);
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
				 float tanHalfFov, float aspect)
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

		glBindVertexArray(skyVao);
		glBindBuffer(GL_ARRAY_BUFFER, skyVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
		// No depth writes: this is a backdrop, and everything drawn after
		// it must win the depth test whatever its distance.
		glDepthMask(GL_FALSE);
		glDisable(GL_CULL_FACE);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glEnable(GL_CULL_FACE);
		glDepthMask(GL_TRUE);
		glBindVertexArray(0);
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

		// The definition gives five wave colours. The "b" pair are the lit
		// ones (the "a" pair are black in every landscape upstream ships,
		// being the far end of a shader gradient we aren't reproducing), so
		// those drive the crossfade, with the deep tone darkened from the
		// bottom colour so there is somewhere for the crests to stand out
		// against.
		waterShallow[0] = water->wavetopb[0];
		waterShallow[1] = water->wavetopb[1];
		waterShallow[2] = water->wavetopb[2];
		for (int i = 0; i < 3; i++) waterDeep[i] = water->wavebottomb[i] * 0.55f;

		// waterTransparency defaults to 1.0 and no shipped landscape sets
		// it, so it can only make the surface *more* see-through than the
		// value chosen here - opaque enough to read as a surface, open
		// enough that a shoreline shows the ground shelving away under it.
		waterAlpha = std::min(1.0f, std::max(0.0f, 0.82f * water->waterTransparency));

		// The surface runs well past the landscape on every side. Upstream
		// does the same (its water plane is far larger than the map), and
		// it doubles as the fix for the terrain patch's visible edge at low
		// camera angles - past the shore there is now sea rather than a
		// cliff into nothing.
		// Out as far as the far clip plane (see nativeOnDrawFrame). Two map
		// widths was not enough: the surface simply stopped mid-view and
		// read as "the sea ends there". Taken out to the far plane the edge
		// is clipped rather than seen, which is what a sea horizon looks
		// like.
		const float margin = std::max(mapWidthUnits, mapHeightUnits) * 3.0f + 200.0f;
		const float x0 = -margin, x1 = mapWidthUnits + margin;
		const float z0 = -margin, z1 = mapHeightUnits + margin;
		const float quad[] = {
			x0, waterHeight, z0,
			x0, waterHeight, z1,
			x1, waterHeight, z0,
			x1, waterHeight, z0,
			x0, waterHeight, z1,
			x1, waterHeight, z1,
		};

		if (waterVao == 0) glGenVertexArrays(1, &waterVao);
		if (waterVbo == 0) glGenBuffers(1, &waterVbo);
		glBindVertexArray(waterVao);
		glBindBuffer(GL_ARRAY_BUFFER, waterVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
		glBindVertexArray(0);

		waterVisible = true;
		LOGI("Water surface at height %.1f, alpha %.2f", waterHeight, waterAlpha);
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
		const int last = kTerrainVerts1D - 1;
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
				terrainHeights[gz * kTerrainVerts1D + gx] = height;
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
			GLintptr offset = (GLintptr) (gz * kTerrainVerts1D + gx0)
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
		if (particles.size() >= kMaxParticles) return;
		particles.push_back(particle);
	}

	// Trail behind an in-flight projectile: upstream's MissileActionRenderer
	// hangs a flame emitter and a smoke emitter off each shot, both enabled
	// by default (WeaponProjectile's createFlame_/createSmoke_ start true and
	// are only turned off by an explicit <nocreateflame>/<nocreatesmoke>).
	// Emission is rate-limited by elapsed time rather than by frame, so the
	// trail has the same density whatever the frame rate.
	double lastTrailSeconds = 0.0;
	bool   trailDueThisFrame = false;

	void emitProjectileTrail(WeaponProjectile *weapon, float x, float y, float z)
	{
		if (!weapon || !trailDueThisFrame) return;

		if (weapon->getCreateFlame()) {
			// Upstream randomises between two start colours and two end
			// colours; one sample per puff gives the same mottled look.
			Vector &c1 = weapon->getFlameStartColor1();
			Vector &c2 = weapon->getFlameStartColor2();
			const float mix = randomUnit();
			Particle flame = {};
			flame.x = x; flame.y = y; flame.z = z;
			flame.vy = 0.6f;
			flame.r = c1[0] + (c2[0] - c1[0]) * mix;
			flame.g = c1[1] + (c2[1] - c1[1]) * mix;
			flame.b = c1[2] + (c2[2] - c1[2]) * mix;
			flame.worldSize = std::max(weapon->getFlameStartSize(), 0.05f);
			flame.life = std::max(weapon->getFlameLife(), 0.1f);
			flame.drag = 0.2f;
			addParticle(flame);
		}

		if (weapon->getCreateSmoke()) {
			Particle smoke = {};
			smoke.x = x + randomSigned() * 0.15f;
			smoke.y = y + randomSigned() * 0.15f;
			smoke.z = z + randomSigned() * 0.15f;
			smoke.vy = 0.9f;
			// Upstream's smoke is a grey particle texture; additive blending
			// makes pure grey glow, so it is kept dim and cool rather than
			// bright white.
			const float grey = 0.25f + randomUnit() * 0.15f;
			smoke.r = grey; smoke.g = grey; smoke.b = grey * 1.1f;
			smoke.worldSize = std::max(weapon->getSmokeStartSize(), 0.05f);
			smoke.life = std::max(weapon->getSmokeLife(), 0.1f);
			smoke.drag = 0.5f;
			addParticle(smoke);
		}
	}

	void spawnEffects()
	{
		std::vector<ScorchDroidEffects::EffectEvent> events = ScorchDroidEffects::drain();
		if (!events.empty() && effectLogsLeft > 0) {
			effectLogsLeft--;
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
				// A bright core plus an outward burst. Count scales with the
				// blast so a small weapon doesn't look like a big one.
				const float size = std::max(event.size, 0.5f);
				Particle core = {};
				core.x = x; core.y = y; core.z = z;
				core.r = 1.0f; core.g = 0.95f; core.b = 0.8f;
				core.worldSize = size * 1.6f;
				core.life = 0.45f;
				core.drag = 1.0f;
				addParticle(core);

				const int count = std::min(12 + (int) (size * 5.0f), 90);
				for (int p = 0; p < count; p++) {
					// Normalising a cube sample would clump towards the
					// corners; rejecting long ones keeps the burst round.
					float dx = randomSigned(), dy = randomSigned(), dz = randomSigned();
					float len = sqrtf(dx * dx + dy * dy + dz * dz);
					if (len < 0.001f || len > 1.0f) { p--; continue; }
					dx /= len; dy /= len; dz /= len;

					float speed = size * (0.8f + randomUnit() * 1.4f);
					Particle particle = {};
					particle.x = x; particle.y = y; particle.z = z;
					particle.vx = dx * speed;
					particle.vy = dy * speed * 0.8f + size * 0.4f;  // biased upward
					particle.vz = dz * speed;
					// Fade from the weapon's own colour towards smoke.
					float heat = randomUnit();
					particle.r = event.r * (0.6f + heat * 0.4f);
					particle.g = event.g * (0.4f + heat * 0.6f);
					particle.b = event.b * (0.3f + heat * 0.5f);
					particle.worldSize = size * (0.35f + randomUnit() * 0.5f);
					particle.life = 0.5f + randomUnit() * 0.7f;
					particle.drag = 0.25f;
					addParticle(particle);
				}
				break;
			}
			case ScorchDroidEffects::eNapalm: {
				Particle flame = {};
				flame.x = x + randomSigned() * 0.5f;
				flame.y = y + 0.3f;
				flame.z = z + randomSigned() * 0.5f;
				flame.vy = 1.5f + randomUnit();
				flame.r = event.r; flame.g = event.g; flame.b = event.b;
				flame.worldSize = event.size * (0.7f + randomUnit() * 0.6f);
				flame.life = 0.9f + randomUnit() * 0.6f;
				flame.drag = 0.6f;
				addParticle(flame);
				break;
			}
			case ScorchDroidEffects::eShieldHit: {
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

	void updateEffects(float deltaSeconds)
	{
		const float kGravity = 9.0f;  // not the sim's gravity: this is smoke, not ballistics

		size_t live = 0;
		for (size_t i = 0; i < particles.size(); i++) {
			Particle &particle = particles[i];
			particle.age += deltaSeconds;
			if (particle.age >= particle.life) continue;

			particle.vy -= kGravity * deltaSeconds * 0.35f;
			const float retain = powf(particle.drag, deltaSeconds);
			particle.vx *= retain; particle.vy *= retain; particle.vz *= retain;
			particle.x += particle.vx * deltaSeconds;
			particle.y += particle.vy * deltaSeconds;
			particle.z += particle.vz * deltaSeconds;

			particles[live++] = particle;
		}
		particles.resize(live);

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

	void drawEffects(const Mat4 &viewProjection, float eyeX, float eyeY, float eyeZ, float fovYRadians)
	{
		if (particles.empty() && beams.empty()) return;

		// Additive, depth-tested but not depth-writing: effects light up
		// whatever is behind them and never occlude each other.
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glDepthMask(GL_FALSE);

		if (!particles.empty()) {
			std::vector<float> data;
			data.reserve(particles.size() * 8);
			for (size_t i = 0; i < particles.size(); i++) {
				const Particle &particle = particles[i];
				const float remaining = 1.0f - particle.age / particle.life;

				const float dx = particle.x - eyeX, dy = particle.y - eyeY, dz = particle.z - eyeZ;
				const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
				float pixels = worldSizeToPixels(particle.worldSize, distance, fovYRadians);
				// Grow slightly as they age, the way a real puff spreads.
				pixels *= 1.0f + (1.0f - remaining) * 0.8f;
				pixels = std::min(std::max(pixels, 1.0f), 256.0f);

				data.push_back(particle.x);
				data.push_back(particle.y);
				data.push_back(particle.z);
				data.push_back(particle.r);
				data.push_back(particle.g);
				data.push_back(particle.b);
				data.push_back(remaining * remaining);  // fade out, weighted late
				data.push_back(pixels);
			}

			glUseProgram(particleProgram);
			glUniformMatrix4fv(particleMvpLoc, 1, GL_FALSE, viewProjection.m);
			glBindVertexArray(particleVao);
			glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
			glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_DYNAMIC_DRAW);
			glDrawArrays(GL_POINTS, 0, (GLsizei) particles.size());
		}

		if (!beams.empty()) {
			std::vector<float> data;
			data.reserve(beams.size() * 12);
			for (size_t i = 0; i < beams.size(); i++) {
				const Beam &beam = beams[i];
				// The beam shader carries no alpha, so fade by dimming the
				// colour - which is the same thing under additive blending.
				const float fade = 1.0f - beam.age / beam.life;
				const float r = beam.r * fade, g = beam.g * fade, b = beam.b * fade;
				data.push_back(beam.x1); data.push_back(beam.y1); data.push_back(beam.z1);
				data.push_back(r); data.push_back(g); data.push_back(b);
				data.push_back(beam.x2); data.push_back(beam.y2); data.push_back(beam.z2);
				data.push_back(r); data.push_back(g); data.push_back(b);
			}

			glUseProgram(sightProgram);
			glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, viewProjection.m);
			glBindVertexArray(beamVao);
			glBindBuffer(GL_ARRAY_BUFFER, beamVbo);
			glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_DYNAMIC_DRAW);
			glLineWidth(3.0f);
			glDrawArrays(GL_LINES, 0, (GLsizei) beams.size() * 2);
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
	struct MeshGroup {
		GLuint vao = 0, vbo = 0;
		int vertexCount = 0;
	};
	struct GpuModel {
		MeshGroup hull, turret, gun;
		float scale = 1.0f;         // upstream's "don't let the model be huge" rule
		float groundOffset = 0.0f;  // lifts the model so its base sits on the ground
		// The same lift in raw model units. Non-tank targets carry their
		// own scale from the landscape definition rather than the tank
		// sizing rule above, so they scale this themselves.
		// Gun pivot relative to the turret pivot, already in our Y-up space.
		float baseOffset = 0.0f;
		float gunOffsetX = 0.0f, gunOffsetY = 0.0f, gunOffsetZ = 0.0f;
	};
	std::map<Model *, GpuModel> g_modelCache;

	// ModelStore::getModel() calls DIALOG_ASSERT (i.e. abort, in this port -
	// see the porting plan's dialogAssert note) when handed a ModelID it
	// can't resolve, rather than returning null. Not every tank defines a
	// projectile model, so an unguarded loadModel() on an empty id takes
	// the whole process down - hence checking modelValid() first.
	Model *loadModelSafely(ModelID &id)
	{
		if (!id.modelValid()) return nullptr;
		return ModelStore::instance()->loadModel(id);
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
	void uploadMeshGroup(MeshGroup &group, const std::vector<Mesh *> &meshes,
						 float offModelX, float offModelZ, float offModelY)
	{
		std::vector<float> verts;
		for (Mesh *mesh : meshes) {
			for (Face *face : mesh->getFaces()) {
				for (int i = 0; i < 3; i++) {
					Vertex *v = mesh->getVertexes()[face->v[i]];
					verts.push_back(v->position[0].asFloat() - offModelX);
					verts.push_back(v->position[2].asFloat() - offModelZ);
					verts.push_back(-(v->position[1].asFloat() - offModelY));
					verts.push_back(face->normal[i][0].asFloat());
					verts.push_back(face->normal[i][2].asFloat());
					verts.push_back(-face->normal[i][1].asFloat());
				}
			}
		}
		group.vertexCount = (int) (verts.size() / 6);
		if (verts.empty()) return;

		glGenVertexArrays(1, &group.vao);
		glBindVertexArray(group.vao);
		glGenBuffers(1, &group.vbo);
		glBindBuffer(GL_ARRAY_BUFFER, group.vbo);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *) (3 * sizeof(float)));
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

		uploadMeshGroup(gpu.hull, hullMeshes, tcx, tcModelZ, tcModelY);
		uploadMeshGroup(gpu.turret, turretMeshes, tcx, tcModelZ, tcModelY);
		uploadMeshGroup(gpu.gun, gunMeshes,
						tcx + gunModelX, tcModelZ + gunModelZ, tcModelY + gunModelY);

		if (gpu.hull.vertexCount == 0 && gpu.turret.vertexCount == 0 && gpu.gun.vertexCount == 0) {
			return nullptr;
		}

		// Vertices are now relative to the turret pivot, so "sit on the
		// ground" is measured from there too.
		gpu.baseOffset = tcModelZ - minV[2].asFloat();
		gpu.groundOffset = gpu.baseOffset * gpu.scale;

		LOGI("Model uploaded: hull %d, turret %d, gun %d tris, scale %.3f",
			 gpu.hull.vertexCount / 3, gpu.turret.vertexCount / 3, gpu.gun.vertexCount / 3, gpu.scale);
		g_modelCache[model] = gpu;
		return &g_modelCache[model];
	}

	void drawMeshGroup(const MeshGroup &group, GLint mvpLoc, const Mat4 &mvp)
	{
		if (group.vertexCount == 0) return;
		glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp.m);
		glBindVertexArray(group.vao);
		glDrawArrays(GL_TRIANGLES, 0, group.vertexCount);
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
		glDrawArrays(GL_POINTS, 0, (GLsizei) (worldPositions.size() / 3));
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

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeOnSurfaceCreated(JNIEnv *, jobject) {
	terrainProgram = linkProgram(kTerrainVertexShader, kTerrainFragmentShader);
	terrainMvpLoc = glGetUniformLocation(terrainProgram, "uMVP");
	terrainMinHeightLoc = glGetUniformLocation(terrainProgram, "uMinHeight");
	terrainHeightRangeLoc = glGetUniformLocation(terrainProgram, "uHeightRange");
	terrainLightDirLoc = glGetUniformLocation(terrainProgram, "uLightDir");
	terrainGroundTexLoc = glGetUniformLocation(terrainProgram, "uGroundTexture");
	terrainHasTextureLoc = glGetUniformLocation(terrainProgram, "uHasTexture");

	skyProgram = linkProgram(kSkyVertexShader, kSkyFragmentShader);
	skyGradientLoc = glGetUniformLocation(skyProgram, "uGradient");
	skySunDirLoc = glGetUniformLocation(skyProgram, "uSunDir");
	skySunColorLoc = glGetUniformLocation(skyProgram, "uSunColor");
	skyGlowLoc = glGetUniformLocation(skyProgram, "uHorizonGlow");
	glGenVertexArrays(1, &skyVao);
	glGenBuffers(1, &skyVbo);
	glBindVertexArray(skyVao);
	glBindBuffer(GL_ARRAY_BUFFER, skyVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (2 * sizeof(float)));
	glBindVertexArray(0);

	waterProgram = linkProgram(kWaterVertexShader, kWaterFragmentShader);
	waterMvpLoc = glGetUniformLocation(waterProgram, "uMVP");
	waterDeepLoc = glGetUniformLocation(waterProgram, "uDeepColor");
	waterShallowLoc = glGetUniformLocation(waterProgram, "uShallowColor");
	waterAlphaLoc = glGetUniformLocation(waterProgram, "uAlpha");
	waterTimeLoc = glGetUniformLocation(waterProgram, "uTime");

	sightProgram = linkProgram(kSightVertexShader, kSightFragmentShader);
	sightMvpLoc = glGetUniformLocation(sightProgram, "uMVP");
	sightVertexCount = 0;

	meshProgram = linkProgram(kMeshVertexShader, kMeshFragmentShader);
	meshMvpLoc = glGetUniformLocation(meshProgram, "uMVP");
	meshLightDirLoc = glGetUniformLocation(meshProgram, "uLightDir");
	meshColorLoc = glGetUniformLocation(meshProgram, "uColor");

	pointProgram = linkProgram(kPointVertexShader, kPointFragmentShader);
	pointMvpLoc = glGetUniformLocation(pointProgram, "uMVP");
	pointColorLoc = glGetUniformLocation(pointProgram, "uColor");
	pointSizeLoc = glGetUniformLocation(pointProgram, "uPointSize");

	// M6 effects. Both buffers are refilled every frame from the live
	// particle/beam lists, hence GL_DYNAMIC_DRAW and no initial allocation.
	particleProgram = linkProgram(kParticleVertexShader, kParticleFragmentShader);
	particleMvpLoc = glGetUniformLocation(particleProgram, "uMVP");
	glGenVertexArrays(1, &particleVao);
	glBindVertexArray(particleVao);
	glGenBuffers(1, &particleVbo);
	glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) (3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) (7 * sizeof(float)));

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

	terrainBuilt = false;
	groundTextureBuilt = false;
	g_modelCache.clear();

	glGenVertexArrays(1, &pointVao);
	glBindVertexArray(pointVao);
	glGenBuffers(1, &pointVbo);
	glBindBuffer(GL_ARRAY_BUFFER, pointVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
	glBindVertexArray(0);

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

	std::lock_guard<std::mutex> lock(g_engineMutex);
	ScorchedContext *ctx = engineActiveContext();
	if (!ctx) return;

	buildTerrainIfNeeded(*ctx);
	if (!terrainBuilt) return;
	buildGroundTextureIfNeeded(*ctx);
	// After the terrain, which is where the map size it spans comes from.
	buildWaterIfNeeded(*ctx);
	buildSkyIfNeeded(*ctx);
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

		// One puff per shot per ~40ms rather than per frame, so a fast
		// device doesn't lay down a denser trail than a slow one.
		trailDueThisFrame = (nowSeconds - lastTrailSeconds) >= 0.04;
		if (trailDueThisFrame) lastTrailSeconds = nowSeconds;

		spawnEffects();
		updateEffects(delta);
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
		float brightness;
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

			Model *model = loadModelSafely(info.model);
			if (!model) { skipped++; continue; }

			FixedVector &pos = target->getLife().getTargetPosition();
			TargetInstance inst;
			inst.x = pos[0].asFloat();
			inst.z = worldZFromEngineY(pos[1].asFloat());
			// Targets carry a real height (they can be dropped, and they
			// fall when the ground under them goes), so use it rather than
			// re-sampling the heightmap the way tanks do.
			inst.y = pos[2].asFloat();
			inst.rotationRadians = info.rotationDegrees * (float) M_PI / 180.0f;
			inst.scale = info.scale;
			// Upstream multiplies the model by this grey ("color_", used as
			// glColor3f(c,c,c)), randomising it when the definition asks by
			// setting -1. But TargetDefinition's constructor never
			// initialises modelbrightness_, so a definition that doesn't
			// name one gets 0 rather than the -1 its own randomise check
			// looks for - which would draw the model pure black. Anything
			// non-positive is treated as "no tint" here; drawing scenery
			// black is never what the data meant.
			inst.brightness = (info.brightness > 0.0f) ? info.brightness : 1.0f;
			inst.model = model;
			targetInstances.push_back(inst);
		}

		// One line whenever the mix changes, which in practice is once per
		// landscape. "skipped" means the hook never recorded a model for a
		// live target, which would be a patch problem, not a data one.
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
	bool haveMyTank = false;
	bool myTankAlive = false;
	unsigned int myPlayerId = 0;
	float myColorR = 1.0f, myColorG = 1.0f, myColorB = 1.0f;
	float myTankX = 0.0f, myTankY = 0.0f, myTankZ = 0.0f;
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
		}

		tankInstances.push_back({
			x, groundY, z, heading, elevation, mine, alive, visible, groundTilt, model,
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
		} else {
			enemyTankPositions.push_back(x); enemyTankPositions.push_back(markerY); enemyTankPositions.push_back(z);
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
		eyeX = targetX + distance * cosf(g_camera.pitch) * sinf(g_camera.yaw);
		eyeY = targetY + distance * sinf(g_camera.pitch);
		eyeZ = targetZ + distance * cosf(g_camera.pitch) * cosf(g_camera.yaw);
	}

	// Keep the eye above ground. Orbiting swings the camera to wherever the
	// yaw points, which is regularly inside a hill - the near plane then
	// slices through it and the view fills with a smear of magnified
	// terrain, which reads as a rendering bug rather than as "you are
	// standing in a mountain". Lifting the eye is the cheap fix and behaves
	// sensibly: the shot stays framed, the camera just rides over the ridge.
	{
		float groundAtEye = terrainHeightAt(*ctx, eyeX, eyeZ);
		float minEyeY = groundAtEye + kCameraGroundClearance;
		if (eyeY < minEyeY) eyeY = minEyeY;
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

	// Sky first: it is the backdrop everything else is drawn in front of.
	drawSky(-view.m[2], -view.m[6], -view.m[10],
			view.m[0], view.m[4], view.m[8],
			view.m[1], view.m[5], view.m[9],
			tanf(kFovYRadians * 0.5f), aspect);

	glUseProgram(terrainProgram);
	glUniformMatrix4fv(terrainMvpLoc, 1, GL_FALSE, mvp.m);
	glUniform1f(terrainMinHeightLoc, terrainMinHeight);
	glUniform1f(terrainHeightRangeLoc, terrainMaxHeight - terrainMinHeight);
	glUniform3f(terrainLightDirLoc, 0.4f, 0.82f, 0.35f);
	glUniform1i(terrainHasTextureLoc, groundTexture != 0 ? 1 : 0);
	if (groundTexture != 0) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, groundTexture);
		glUniform1i(terrainGroundTexLoc, 0);
	}
	glBindVertexArray(terrainVao);
	glDrawElements(GL_TRIANGLES, terrainIndexCount, GL_UNSIGNED_INT, (void *) 0);

	// Water goes on immediately after the ground and before anything that
	// stands on it. It blends over the terrain already drawn (so a shoreline
	// shows the bottom shelving away) but still writes depth, so a tank or a
	// tree below the waterline is properly submerged rather than floating
	// on top of the surface.
	if (waterVisible && waterProgram != 0) {
		glUseProgram(waterProgram);
		glUniformMatrix4fv(waterMvpLoc, 1, GL_FALSE, mvp.m);
		glUniform3f(waterDeepLoc, waterDeep[0], waterDeep[1], waterDeep[2]);
		glUniform3f(waterShallowLoc, waterShallow[0], waterShallow[1], waterShallow[2]);
		glUniform1f(waterAlphaLoc, waterAlpha);
		glUniform1f(waterTimeLoc, (float) fmod(lastFrameSeconds, 3600.0));
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		// The surface extends far past the map on every side, so with the
		// camera under it the back faces are what you see - and being able
		// to look up through it from a valley floor is worth more than the
		// culling.
		glDisable(GL_CULL_FACE);
		glBindVertexArray(waterVao);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glDisable(GL_BLEND);
		glEnable(GL_CULL_FACE);
	}

	glUseProgram(pointProgram);
	glDisable(GL_CULL_FACE);  // point sprites have no winding

	// Real tank models where we have one; a point sprite is kept as the
	// fallback for any tank whose model wouldn't load, so a tank is never
	// simply invisible.
	glUseProgram(meshProgram);
	glUniform3f(meshLightDirLoc, 0.4f, 0.82f, 0.35f);

	// Landscape targets first: they are scenery, so they should be behind
	// everything that matters, and drawing them before the tanks keeps the
	// per-target colour uniform out of the tank loop's way.
	for (TargetInstance &inst : targetInstances) {
		GpuModel *gpu = uploadModel(inst.model);
		if (!gpu) continue;

		// Upstream's "color" for a target is a grey multiplier, randomised
		// per target when the definition doesn't fix one, so a stand of
		// identical trees doesn't look stamped out.
		glUniform4f(meshColorLoc, inst.brightness, inst.brightness, inst.brightness, 1.0f);

		// The definition's own scale, not the tank sizing rule - a building
		// is meant to dwarf a tank - so the base lift is scaled here too.
		Mat4 model = Mat4::multiply(
			Mat4::translate(inst.x, inst.y + gpu->baseOffset * inst.scale, inst.z),
			Mat4::multiply(
				Mat4::rotateY(inst.rotationRadians),
				Mat4::scale(inst.scale)));
		Mat4 targetMvp = Mat4::multiply(mvp, model);
		// A target is one undivided model: uploadModel only splits meshes
		// named turret/gun, which nothing but a tank has, so everything
		// lands in the hull group.
		drawMeshGroup(gpu->hull, meshMvpLoc, targetMvp);
	}

	std::vector<float> unmodelledMine, unmodelledOther;
	bool haveSight = false;
	Mat4 sightTransform = Mat4::identity();
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

		GpuModel *gpu = uploadModel(inst.model);
		if (!gpu) {
			auto &bucket = inst.mine ? unmodelledMine : unmodelledOther;
			bucket.push_back(inst.x); bucket.push_back(inst.y + 1.5f); bucket.push_back(inst.z);
			continue;
		}

		// The engine's own per-tank colour, as upstream tints tanks and
		// draws their names with. Was a hardcoded cyan/red "mine vs theirs"
		// split, which contradicted the name plates the moment those
		// started showing the real colour - a red "Player" label over a
		// cyan tank. Your own tank is identifiable by the aim sight and the
		// follow camera; it doesn't need to lie about its colour too.
		glUniform4f(meshColorLoc, inst.colorR, inst.colorG, inst.colorB, 1.0f);

		// Hull sits still; the turret swings to the firing bearing and the
		// gun additionally lifts to the elevation, each about its own pivot
		// (see uploadModel) - the same articulation upstream does.
		Mat4 base = Mat4::multiply(
			Mat4::translate(inst.x, inst.y + gpu->groundOffset, inst.z),
			Mat4::scale(gpu->scale));
		// The hull, and only the hull, leans onto the ground - the turret
		// and gun below stay in world axes so the barrel keeps agreeing
		// with the shot (see where groundTilt is built).
		Mat4 hullMvp = Mat4::multiply(mvp, Mat4::multiply(
			Mat4::multiply(
				Mat4::translate(inst.x, inst.y + gpu->groundOffset, inst.z),
				inst.groundTilt),
			Mat4::scale(gpu->scale)));
		drawMeshGroup(gpu->hull, meshMvpLoc, hullMvp);

		Mat4 turret = Mat4::multiply(base, Mat4::rotateY(inst.headingRadians));
		drawMeshGroup(gpu->turret, meshMvpLoc, Mat4::multiply(mvp, turret));

		Mat4 gun = Mat4::multiply(
			turret,
			Mat4::multiply(
				Mat4::translate(gpu->gunOffsetX, gpu->gunOffsetY, gpu->gunOffsetZ),
				// Not negated: the barrel points along world -Z after the
				// upload's remap, and rotateX(+e) lifts -Z towards +Y.
				Mat4::rotateX(inst.elevationRadians)));
		drawMeshGroup(gpu->gun, meshMvpLoc, Mat4::multiply(mvp, gun));

		// Upstream draws the sight on the player's own tank while it's
		// playing (TargetRendererImplTank::drawParticle: currentTank &&
		// StatePlaying, and it bails entirely unless the tank is sNormal).
		// Our nearest equivalent is "my tank, alive" - this config has no
		// strict turn order, so every live moment is your turn.
		if (inst.mine && inst.alive) {
			haveSight = true;
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

	if (haveSight) {
		buildSightGeometry();
		glUseProgram(sightProgram);
		Mat4 sightMvp = Mat4::multiply(mvp, sightTransform);
		glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, sightMvp.m);
		glBindVertexArray(sightVao);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, sightVertexCount);
	}

	glUseProgram(pointProgram);
	drawPoints(mvp, unmodelledOther, 26.0f, 0.95f, 0.25f, 0.2f);
	drawPoints(mvp, unmodelledMine, 26.0f, 0.2f, 0.9f, 0.95f);

	// Real in-flight shot/explosion positions, straight from the running
	// simulation's ActionController. Shots also report which tank fired
	// them, so each one can use that tank's own projectile model (see the
	// shotPlayerIds addition in patch 0009) rather than one shared mesh.
	std::vector<FixedVector> shotPositionsRaw, explosionPositionsRaw;
	std::vector<unsigned int> shotPlayerIds;
	std::vector<FixedVector> shotVelocities;
	std::vector<unsigned int> shotWeaponIds;
	ctx->getActionController().getShotAndExplosionPositions(
		shotPositionsRaw, explosionPositionsRaw, &shotPlayerIds, &shotVelocities, &shotWeaponIds);

	std::vector<float> unmodelledShots, explosionPositions;
	glUseProgram(meshProgram);
	glUniform3f(meshLightDirLoc, 0.4f, 0.82f, 0.35f);
	glUniform4f(meshColorLoc, 0.95f, 0.9f, 0.4f, 1.0f);
	for (size_t i = 0; i < shotPositionsRaw.size(); i++) {
		FixedVector &p = shotPositionsRaw[i];
		float wx = p[0].asFloat(), wy = p[2].asFloat(), wz = worldZFromEngineY(p[1].asFloat());

		// Upstream's precedence: the weapon's own model first, falling back
		// to the firing tank's projectilemodel (see Accessory::getWeaponMesh).
		// Most tanks define no projectilemodel, so consulting only the
		// fallback - as this did at first - means almost no shot ever gets
		// a mesh.
		Model *projectileModel = nullptr;
		if (i < shotWeaponIds.size() && shotWeaponIds[i] != 0) {
			Accessory *weapon = ctx->getAccessoryStore().findByAccessoryId(shotWeaponIds[i]);
			if (weapon) {
				projectileModel = loadModelSafely(weapon->getModel());
				// Flame and smoke trail. Upstream emits these from particle
				// emitters attached to the shot (MissileActionRenderer), and
				// both default to *on* for every projectile - so this is what
				// makes an ordinary missile read as a missile rather than a
				// travelling dot. Per-weapon colours, sizes and lifetimes are
				// the weapon's own.
				emitProjectileTrail((WeaponProjectile *) weapon->getAction(), wx, wy, wz);
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

		GpuModel *gpu = uploadModel(projectileModel);
		if (!gpu) {
			unmodelledShots.push_back(wx); unmodelledShots.push_back(wy); unmodelledShots.push_back(wz);
			continue;
		}
		// Point the mesh along its actual flight path: a bearing about the
		// up axis, then a pitch about X, like upstream's MissileMesh::draw.
		// Velocity is an engine-space (x, y, height) direction.
		//
		// Re-derived for the corrected landscape-to-world map rather than
		// carried over: the mesh's forward axis is world -Z after the
		// upload's remap, so rotateY(a) * rotateX(b) sends it to
		// (-cos b * sin a, sin b, -cos b * cos a). Matching that to the
		// world velocity (vx, vz, -vy) gives b = asin(vz) and
		// a = atan2(-vx, vy).
		Mat4 orientation = Mat4::identity();
		if (i < shotVelocities.size()) {
			FixedVector &vel = shotVelocities[i];
			float vx = vel[0].asFloat(), vy = vel[1].asFloat(), vz = vel[2].asFloat();
			float len = sqrtf(vx * vx + vy * vy + vz * vz);
			if (len > 0.0001f) {
				vx /= len; vy /= len; vz /= len;
				float angXY = atan2f(-vx, vy);
				float angYZ = asinf(std::min(1.0f, std::max(-1.0f, vz)));
				orientation = Mat4::multiply(Mat4::rotateY(angXY), Mat4::rotateX(angYZ));
			}
		}
		Mat4 model = Mat4::multiply(
			Mat4::translate(wx, wy, wz),
			Mat4::multiply(orientation, Mat4::scale(gpu->scale)));
		Mat4 shotMvp = Mat4::multiply(mvp, model);
		drawMeshGroup(gpu->hull, meshMvpLoc, shotMvp);
		drawMeshGroup(gpu->turret, meshMvpLoc, shotMvp);
		drawMeshGroup(gpu->gun, meshMvpLoc, shotMvp);
	}

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
		glUniform3f(meshLightDirLoc, 0.4f, 0.82f, 0.35f);

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
					glDrawArrays(GL_TRIANGLES, 0, cubeVertexCount);
				} else if (inst.shieldHalf) {
					glBindVertexArray(hemiVao);
					glDrawArrays(GL_TRIANGLES, 0, hemiVertexCount);
				} else {
					glBindVertexArray(sphereVao);
					glDrawArrays(GL_TRIANGLES, 0, sphereVertexCount);
				}
			}

			if (inst.parachuteOpen) {
				Mat4 model = Mat4::translate(inst.x, inst.y, inst.z);
				glUniformMatrix4fv(meshMvpLoc, 1, GL_FALSE, Mat4::multiply(mvp, model).m);
				glUniform4f(meshColorLoc, 0.85f, 0.85f, 0.9f, 0.9f);
				glBindVertexArray(chuteVao);
				glDrawArrays(GL_TRIANGLES, 0, chuteVertexCount);
				glUniform4f(meshColorLoc, 1.0f, 1.0f, 1.0f, 0.9f);
				glBindVertexArray(chuteCordVao);
				glDrawArrays(GL_LINES, 0, chuteCordVertexCount);
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
					glDrawArrays(GL_TRIANGLES, 0, sphereVertexCount);
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
					glUniformMatrix4fv(sightMvpLoc, 1, GL_FALSE, mvp.m);
					glBindVertexArray(beamVao);
					glBindBuffer(GL_ARRAY_BUFFER, beamVbo);
					glBufferData(GL_ARRAY_BUFFER, line.size() * sizeof(float), line.data(), GL_DYNAMIC_DRAW);
					glLineWidth(2.0f);
					glDrawArrays(GL_LINES, 0, (GLsizei) (line.size() / 6));
				}
			}
		}

		glBindVertexArray(0);
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
	}

	// Effects last, so they blend additively over the finished scene.
	drawEffects(mvp, eyeX, eyeY, eyeZ, kFovYRadians);

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
	return g_camera.followMode ? JNI_TRUE : JNI_FALSE;
}

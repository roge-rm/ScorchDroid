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
#include <target/TargetLife.hpp>
#include <tank/Tank.hpp>
#include <tank/TankState.hpp>
#include <engine/ActionController.hpp>
#include <common/FixedVector.hpp>
#include <EngineState.hpp>
#include <Mat4.hpp>
#include <LandscapeTextureBuilder.hpp>
#include <DeformEventQueue.h>
#include <EffectEventQueue.h>
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

	// M6 terrain destruction: the sampled grid is kept around after the
	// initial build so a crater can re-sample and re-upload just the
	// vertices it touched (see applyTerrainDeformations) instead of
	// rebuilding the whole mesh. kGrid x kGrid quads => (kGrid+1)^2 verts.
	constexpr int kGrid = 96;
	constexpr int kTerrainVerts1D = kGrid + 1;
	constexpr int kTerrainFloatsPerVertex = 8;  // pos(3) + normal(3) + uv(2)
	std::vector<float> terrainHeights;          // kTerrainVerts1D^2, row-major by gz
	std::vector<float> terrainWorldX, terrainWorldZ;
	int    terrainSrcWidth = 0, terrainSrcHeight = 0;
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
		out[7] = (float) gz / (float) kGrid;
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
			// Any pending crater belongs to the landscape being thrown
			// away - applying it to the new one would corrupt unrelated
			// vertices.
			ScorchDroidLandscape::clearDirtyRegion();
			LOGI("Landscape changed (definition %u) - rebuilding terrain", defnNumber);
		}

		HeightMap &heightMap = ctx.getLandscapeMaps().getGroundMaps().getHeightMap();
		int w = heightMap.getMapWidth();
		int h = heightMap.getMapHeight();
		if (w <= 0 || h <= 0) return;

		const int verts1D = kTerrainVerts1D;
		terrainSrcWidth = w;
		terrainSrcHeight = h;
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
			int sy = std::min(gz * h / kGrid, h - 1);
			for (int gx = 0; gx < verts1D; gx++) {
				int sx = std::min(gx * w / kGrid, w - 1);
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

		mapWidthUnits = (float) w;
		mapHeightUnits = (float) h;
		{
			std::lock_guard<std::mutex> camLock(g_cameraMutex);
			g_camera.targetX = mapWidthUnits / 2.0f;
			g_camera.targetZ = mapHeightUnits / 2.0f;
			g_camera.targetY = (terrainMinHeight + terrainMaxHeight) / 2.0f;
			g_camera.orbitDistance = std::max(mapWidthUnits, mapHeightUnits) * 0.9f;
		}

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
		const int last = kTerrainVerts1D - 1;
		int gx0 = std::max((minX * kGrid) / w - 2, 0);
		int gx1 = std::min((maxX * kGrid) / w + 2, last);
		int gz0 = std::max((minY * kGrid) / h - 2, 0);
		int gz1 = std::min((maxY * kGrid) / h + 2, last);
		if (gx0 > gx1 || gz0 > gz1) return;  // entirely off-map

		// Re-sample heights first (over a further 1-vertex margin, since
		// writeTerrainVertex reads its neighbours' heights), then rebuild
		// vertices - doing both in one pass would use stale neighbours.
		for (int gz = std::max(gz0 - 1, 0); gz <= std::min(gz1 + 1, last); gz++) {
			int sy = std::min(gz * h / kGrid, h - 1);
			for (int gx = std::max(gx0 - 1, 0); gx <= std::min(gx1 + 1, last); gx++) {
				int sx = std::min(gx * w / kGrid, w - 1);
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
			const float x = event.x, y = event.z, z = event.y;  // engine -> render
			const float endX = event.endX, endY = event.endZ, endZ = event.endY;

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
		int sy = std::min(std::max((int) worldZ, 0), h - 1);
		return heightMap.getHeight(sx, sy).asFloat();
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
				verts.push_back(radius * cosf(dx));   // upstream z -> our y
				verts.push_back(radius * sinf(dx));   // upstream y -> our z
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
		// Gun pivot relative to the turret pivot, already in our Y-up space.
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

	// Models are Z-up (upstream's world convention) while our renderer is
	// Y-up, so vertices are remapped (x, y, z) -> (x, z, y) once here rather
	// than fought with a transform at every draw.
	void uploadMeshGroup(MeshGroup &group, const std::vector<Mesh *> &meshes,
						 float offX, float offY, float offZ)
	{
		std::vector<float> verts;
		for (Mesh *mesh : meshes) {
			for (Face *face : mesh->getFaces()) {
				for (int i = 0; i < 3; i++) {
					Vertex *v = mesh->getVertexes()[face->v[i]];
					verts.push_back(v->position[0].asFloat() - offX);
					verts.push_back(v->position[2].asFloat() - offY);
					verts.push_back(v->position[1].asFloat() - offZ);
					verts.push_back(face->normal[i][0].asFloat());
					verts.push_back(face->normal[i][2].asFloat());
					verts.push_back(face->normal[i][1].asFloat());
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
		float tcx = turretCenter[0].asFloat();
		float tcy = turretCenter[2].asFloat();  // model Z -> our Y
		float tcz = turretCenter[1].asFloat();
		gpu.gunOffsetX = gunOffset[0].asFloat();
		gpu.gunOffsetY = gunOffset[2].asFloat();
		gpu.gunOffsetZ = gunOffset[1].asFloat();

		uploadMeshGroup(gpu.hull, hullMeshes, tcx, tcy, tcz);
		uploadMeshGroup(gpu.turret, turretMeshes, tcx, tcy, tcz);
		uploadMeshGroup(gpu.gun, gunMeshes,
						tcx + gpu.gunOffsetX, tcy + gpu.gunOffsetY, tcz + gpu.gunOffsetZ);

		if (gpu.hull.vertexCount == 0 && gpu.turret.vertexCount == 0 && gpu.gun.vertexCount == 0) {
			return nullptr;
		}

		// Vertices are now relative to the turret pivot, so "sit on the
		// ground" is measured from there too.
		gpu.groundOffset = (tcy - minV[2].asFloat()) * gpu.scale;

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
	applyTerrainDeformations(*ctx);
	applyScorchMarks(*ctx);

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
		Model *model;
	};
	std::vector<TankInstance> tankInstances;
	std::vector<float> myTankPositions, enemyTankPositions;  // fallback markers
	bool haveMyTank = false;
	float myTankX = 0.0f, myTankY = 0.0f, myTankZ = 0.0f;
	for (auto &entry : tanks) {
		Tank *tank = entry.second;
		FixedVector &pos = tank->getLife().getTargetPosition();
		float x = pos[0].asFloat(), z = pos[1].asFloat();
		float groundY = heightAt(heightMap, mapW, mapH, x, z);
		bool mine = (tank->getDestinationId() == myDestinationId);

		Model *model = nullptr;
		TankModel *tankModel = tank->getModelContainer().getTankModel();
		if (tankModel) model = loadModelSafely(tankModel->getTankModelID());

		// The turret angle is the engine's own bearing (counterclockwise
		// from world +Y - see engine_jni.cpp's handleTap comment), and our
		// world maps landscape (x,y) onto (x,z), so it converts straight
		// into a rotation about the world up axis.
		float heading = tank->getShotInfo().getRotationGunXY().asFloat() * (float) M_PI / 180.0f;
		float elevation = tank->getShotInfo().getRotationGunYZ().asFloat() * (float) M_PI / 180.0f;
		bool alive = (tank->getState().getState() == TankState::sNormal);
		tankInstances.push_back({ x, groundY, z, heading, elevation, mine, alive, model });

		float markerY = groundY + 1.5f;
		if (mine) {
			myTankPositions.push_back(x); myTankPositions.push_back(markerY); myTankPositions.push_back(z);
			haveMyTank = true;
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

	glUseProgram(pointProgram);
	glDisable(GL_CULL_FACE);  // point sprites have no winding

	// Real tank models where we have one; a point sprite is kept as the
	// fallback for any tank whose model wouldn't load, so a tank is never
	// simply invisible.
	glUseProgram(meshProgram);
	glUniform3f(meshLightDirLoc, 0.4f, 0.82f, 0.35f);
	std::vector<float> unmodelledMine, unmodelledOther;
	bool haveSight = false;
	Mat4 sightTransform = Mat4::identity();
	for (TankInstance &inst : tankInstances) {
		GpuModel *gpu = uploadModel(inst.model);
		if (!gpu) {
			auto &bucket = inst.mine ? unmodelledMine : unmodelledOther;
			bucket.push_back(inst.x); bucket.push_back(inst.y + 1.5f); bucket.push_back(inst.z);
			continue;
		}

		if (inst.mine) {
			glUniform4f(meshColorLoc, 0.45f, 0.85f, 0.95f, 1.0f);  // cyan-ish - mine
		} else {
			glUniform4f(meshColorLoc, 0.9f, 0.45f, 0.4f, 1.0f);    // red-ish - opponents
		}

		// Hull sits still; the turret swings to the firing bearing and the
		// gun additionally lifts to the elevation, each about its own pivot
		// (see uploadModel) - the same articulation upstream does.
		Mat4 base = Mat4::multiply(
			Mat4::translate(inst.x, inst.y + gpu->groundOffset, inst.z),
			Mat4::scale(gpu->scale));
		Mat4 hullMvp = Mat4::multiply(mvp, base);
		drawMeshGroup(gpu->hull, meshMvpLoc, hullMvp);

		Mat4 turret = Mat4::multiply(base, Mat4::rotateY(inst.headingRadians));
		drawMeshGroup(gpu->turret, meshMvpLoc, Mat4::multiply(mvp, turret));

		Mat4 gun = Mat4::multiply(
			turret,
			Mat4::multiply(
				Mat4::translate(gpu->gunOffsetX, gpu->gunOffsetY, gpu->gunOffsetZ),
				// Negated: vertices are remapped Z-up -> Y-up at upload, so
				// "forward" and "up" swap axes and a positive rotation about
				// X tips the barrel *down* rather than elevating it.
				Mat4::rotateX(-inst.elevationRadians)));
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
					Mat4::rotateX(-inst.elevationRadians)));
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
		float wx = p[0].asFloat(), wy = p[2].asFloat(), wz = p[1].asFloat();

		// Upstream's precedence: the weapon's own model first, falling back
		// to the firing tank's projectilemodel (see Accessory::getWeaponMesh).
		// Most tanks define no projectilemodel, so consulting only the
		// fallback - as this did at first - means almost no shot ever gets
		// a mesh.
		Model *projectileModel = nullptr;
		if (i < shotWeaponIds.size() && shotWeaponIds[i] != 0) {
			Accessory *weapon = ctx->getAccessoryStore().findByAccessoryId(shotWeaponIds[i]);
			if (weapon) projectileModel = loadModelSafely(weapon->getModel());
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
		// Point the mesh along its actual flight path, using upstream's own
		// direction-to-angles math (MissileMesh::draw): a bearing about the
		// up axis, then a pitch about X. Velocity is in the engine's Z-up
		// space, so the components are read in that order here.
		Mat4 orientation = Mat4::identity();
		if (i < shotVelocities.size()) {
			FixedVector &vel = shotVelocities[i];
			float vx = vel[0].asFloat(), vy = vel[1].asFloat(), vz = vel[2].asFloat();
			float len = sqrtf(vx * vx + vy * vy + vz * vz);
			if (len > 0.0001f) {
				vx /= len; vy /= len; vz /= len;
				float angXY = (float) M_PI - atan2f(vx, vy);
				float angYZ = acosf(std::min(1.0f, std::max(-1.0f, vz)));
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
		explosionPositions.push_back(p[1].asFloat());
	}
	glUseProgram(pointProgram);
	drawPoints(mvp, unmodelledShots, 14.0f, 1.0f, 1.0f, 0.2f);
	// The explosion marker dot is deliberately kept: it tracks the Explosion
	// action for as long as it lives, whereas the particle burst below is
	// raised once at detonation and then flies on its own.
	drawPoints(mvp, explosionPositions, 20.0f, 1.0f, 0.5f, 0.0f);

	// Effects last, so they blend additively over the finished scene.
	drawEffects(mvp, eyeX, eyeY, eyeZ, kFovYRadians);
}

// M6: battlefield touch now drives the orbit camera (see the file-level
// comment above for why tap-to-fire-at-a-point doesn't carry over as-is) -
// called from MainActivity's touch handling on the GLSurfaceView.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeCameraDrag(JNIEnv *, jobject, jfloat dx, jfloat dy) {
	std::lock_guard<std::mutex> lock(g_cameraMutex);
	g_camera.yaw += dx * kDragSensitivity;
	g_camera.pitch = std::min(std::max(g_camera.pitch - dy * kDragSensitivity, kMinPitch), kMaxPitch);
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
	g_camera.targetX -= (rightX * dx + fwdX * dy) * scale;
	g_camera.targetZ -= (rightZ * dx + fwdZ * dy) * scale;

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

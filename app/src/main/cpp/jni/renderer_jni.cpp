// M2 vertical slice: a from-scratch, minimal GLES3 renderer (top-down 2D
// view, not a full 3D camera - out of scope for M2, see the porting plan)
// that reads live state directly from the running ScorchedServer instance:
// the real heightmap (as a luminance texture) and real tank positions, both
// produced by completely unmodified upstream game logic. No upstream
// rendering code (GLW/GLEXT) is reused - this is new code, per the
// architecture decision to rewrite the client/rendering layer entirely.
#include <jni.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <vector>
#include <mutex>

#include <server/ScorchedServer.hpp>
#include <target/TargetContainer.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <target/TargetLife.hpp>
#include <tank/Tank.hpp>

#define LOG_TAG "ScorchDroidRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Guards access to ScorchedServer state shared between the simulation
// thread (tickEngine, see engine_jni.cpp) and the GL render thread.
extern std::mutex g_engineMutex;

namespace
{
	GLuint program = 0;
	GLuint landscapeVao = 0, landscapeVbo = 0;
	GLuint tankVao = 0, tankVbo = 0;
	GLint  positionLoc = -1, colorLoc = -1, useColorLoc = -1, texCoordLoc = -1;
	GLuint landscapeTexture = 0;
	bool   landscapeBuilt = false;
	int    surfaceWidth = 1, surfaceHeight = 1;

	const char *kVertexShader = R"(#version 300 es
		layout(location = 0) in vec2 aPosition;
		layout(location = 1) in vec2 aTexCoord;
		out vec2 vTexCoord;
		void main() {
			vTexCoord = aTexCoord;
			gl_Position = vec4(aPosition, 0.0, 1.0);
			gl_PointSize = 24.0;
		}
	)";

	const char *kFragmentShader = R"(#version 300 es
		precision mediump float;
		in vec2 vTexCoord;
		out vec4 fragColor;
		uniform sampler2D uTexture;
		uniform vec4 uColor;
		uniform int uUseTexture;
		void main() {
			if (uUseTexture == 1) {
				float h = texture(uTexture, vTexCoord).r;
				fragColor = vec4(0.2 + h * 0.3, 0.35 + h * 0.4, 0.15 + h * 0.15, 1.0);
			} else {
				fragColor = uColor;
			}
		}
	)";

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

	// Builds a single-channel luminance texture from the real heightmap and
	// a fullscreen-quad VBO to display it, once, the first time a landscape
	// actually exists.
	void buildLandscapeIfNeeded()
	{
		if (landscapeBuilt) return;
		if (!ScorchedServer::serverStarted()) return;

		HeightMap &heightMap = ScorchedServer::instance()->getLandscapeMaps().getGroundMaps().getHeightMap();
		int w = heightMap.getMapWidth();
		int h = heightMap.getMapHeight();
		if (w <= 0 || h <= 0) return;

		// Downsample to a manageable texture size.
		const int texSize = 128;
		std::vector<unsigned char> pixels(texSize * texSize);
		float minH = 1e9f, maxH = -1e9f;
		std::vector<float> raw(texSize * texSize);
		for (int ty = 0; ty < texSize; ty++) {
			for (int tx = 0; tx < texSize; tx++) {
				int sx = tx * w / texSize;
				int sy = ty * h / texSize;
				float height = heightMap.getHeight(sx, sy).asFloat();
				raw[ty * texSize + tx] = height;
				if (height < minH) minH = height;
				if (height > maxH) maxH = height;
			}
		}
		float range = (maxH - minH) > 0.001f ? (maxH - minH) : 1.0f;
		for (int i = 0; i < texSize * texSize; i++) {
			pixels[i] = (unsigned char) (255.0f * (raw[i] - minH) / range);
		}

		glGenTextures(1, &landscapeTexture);
		glBindTexture(GL_TEXTURE_2D, landscapeTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, texSize, texSize, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		float quad[] = {
			// x,    y,    u,   v
			-0.9f, -0.9f, 0.0f, 1.0f,
			 0.9f, -0.9f, 1.0f, 1.0f,
			-0.9f,  0.9f, 0.0f, 0.0f,
			 0.9f,  0.9f, 1.0f, 0.0f,
		};
		glGenVertexArrays(1, &landscapeVao);
		glBindVertexArray(landscapeVao);
		glGenBuffers(1, &landscapeVbo);
		glBindBuffer(GL_ARRAY_BUFFER, landscapeVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *) 0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *) (2 * sizeof(float)));
		glBindVertexArray(0);

		landscapeBuilt = true;
		LOGI("Landscape texture built: %dx%d source -> %dx%d texture", w, h, texSize, texSize);
	}
}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeOnSurfaceCreated(JNIEnv *, jobject) {
	program = linkProgram(kVertexShader, kFragmentShader);
	positionLoc = 0;
	texCoordLoc = 1;
	colorLoc = glGetUniformLocation(program, "uColor");
	useColorLoc = glGetUniformLocation(program, "uUseTexture");
	landscapeBuilt = false;

	glGenVertexArrays(1, &tankVao);
	glBindVertexArray(tankVao);
	glGenBuffers(1, &tankVbo);
	glBindBuffer(GL_ARRAY_BUFFER, tankVbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *) 0);
	glBindVertexArray(0);

	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeOnSurfaceChanged(JNIEnv *, jobject, jint width, jint height) {
	surfaceWidth = width;
	surfaceHeight = height;
	glViewport(0, 0, width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_GameRenderer_nativeOnDrawFrame(JNIEnv *, jobject) {
	glClear(GL_COLOR_BUFFER_BIT);

	std::lock_guard<std::mutex> lock(g_engineMutex);
	if (!ScorchedServer::serverStarted()) return;

	buildLandscapeIfNeeded();
	glUseProgram(program);

	if (landscapeBuilt) {
		glUniform1i(useColorLoc, 1);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, landscapeTexture);
		glBindVertexArray(landscapeVao);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

	// Real tank positions, straight from the running simulation.
	HeightMap &heightMap = ScorchedServer::instance()->getLandscapeMaps().getGroundMaps().getHeightMap();
	int mapW = heightMap.getMapWidth();
	int mapH = heightMap.getMapHeight();
	if (mapW <= 0) mapW = 1;
	if (mapH <= 0) mapH = 1;

	std::map<unsigned int, Tank *> &tanks = ScorchedServer::instance()->getTargetContainer().getTanks();
	std::vector<float> tankVerts;
	tankVerts.reserve(tanks.size() * 2);
	for (auto &entry : tanks) {
		Tank *tank = entry.second;
		FixedVector &pos = tank->getLife().getTargetPosition();
		float nx = pos[0].asFloat() / (float) mapW;
		float ny = pos[1].asFloat() / (float) mapH;
		// Map landscape-space [0,1] into the same -0.9..0.9 quad used above.
		tankVerts.push_back(-0.9f + nx * 1.8f);
		tankVerts.push_back(-0.9f + ny * 1.8f);
	}

	if (!tankVerts.empty()) {
		glUniform1i(useColorLoc, 0);
		glUniform4f(colorLoc, 0.95f, 0.25f, 0.2f, 1.0f);
		glBindVertexArray(tankVao);
		glBindBuffer(GL_ARRAY_BUFFER, tankVbo);
		glBufferData(GL_ARRAY_BUFFER, tankVerts.size() * sizeof(float), tankVerts.data(), GL_DYNAMIC_DRAW);
		glDrawArrays(GL_POINTS, 0, (GLsizei) (tankVerts.size() / 2));
	}
}

#include <OceanWaves.h>

#include <cmath>
#include <complex>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace
{
	using Complex = std::complex<float>;

	const int N = ScorchDroidOcean::kResolution;
	const float kL = ScorchDroidOcean::kTileLength;

	// Upstream's own constants (ocean_wave_generator.hpp, Water2.cpp):
	// gravity is 10, not 9.81, and the surface repeats every 10.24s.
	const float kGravity = 10.0f;
	const float kCycleSeconds = 10.24f;
	// Upstream's wave height scalar, "a" in the Phillips spectrum
	// (Water2::generate passes wave_resolution * 1e-8). It only means
	// anything against an unnormalised inverse transform, which is what
	// both FFTW's c2r and the one below are - so with it in and nothing
	// rescaled afterwards the heights come out in world units, and they
	// grow with the wind exactly as upstream's do: a peak near 0.2 units
	// at generator wind 3 (a dead calm round) and near 4 at wind 13.
	const float kHeightScalar = (float) ScorchDroidOcean::kResolution * 1e-8f;

	std::mutex g_mutex;
	// h0(k), the fixed part of the spectrum. (N+1)^2 rather than N^2 because
	// h_tilde needs h0 at both k and -k, and upstream indexes the mirrored
	// entry as (N-y, N-x) - which needs the far edge to exist.
	std::vector<Complex> g_h0;
	bool g_seeded = false;

	// W10a: the foam's memory between calls, and the clock it decays by.
	// Upstream steps 256 phases across its 10.24 s cycle at 24 a second;
	// its spawn and decay rates are per phase, so elapsed time is turned
	// into phases here and both scaled by it.
	std::vector<float> g_aof;
	float g_aofRandom[37];
	float g_lastSeconds = -1.0f;
	const float kPhasesPerSecond = 24.0f;
	const float kPhases = 256.0f;

	// Box-Muller, as upstream's gaussrand: a complex Gaussian per wave.
	Complex gaussianPair(unsigned int &state)
	{
		auto next = [&state]() {
			// A small deterministic generator rather than rand(), so a seed
			// really does reproduce a sea - rand() would tie this to whatever
			// else in the process happened to call it.
			state = state * 1664525u + 1013904223u;
			return (float) ((state >> 8) & 0xffffffu) / (float) 0x1000000u;
		};
		float x1, x2, w;
		do {
			x1 = 2.0f * next() - 1.0f;
			x2 = 2.0f * next() - 1.0f;
			w = x1 * x1 + x2 * x2;
		} while (w >= 1.0f || w < 1e-8f);
		w = sqrtf(-2.0f * logf(w) / w);
		return Complex(x1 * w, x2 * w);
	}

	// The Phillips spectrum, transcribed from upstream's phillips():
	// how much energy the wind puts into a wave of vector K.
	float phillips(float kx, float ky, float windX, float windY, float windSpeed)
	{
		const float k2 = kx * kx + ky * ky;
		if (k2 <= 0.0f) return 0.0f;
		const float v2 = windSpeed * windSpeed;
		const float v4 = v2 * v2;
		const float k4 = k2 * k2;
		const float g2 = kGravity * kGravity;

		const float kDotW = kx * windX + ky * windY;
		// Upstream keeps K unnormalised here and divides by k2 afterwards,
		// which is the same cos^2 with one less square root.
		const float kDotWhat = kDotW * kDotW / k2;
		const float eterm = expf(-g2 / (k2 * v4)) / k4;
		// Damping of very small waves, upstream's 1/100 factor.
		const float damp = 1.0f / 100.0f;
		const float l2 = v4 / g2 * damp * damp;

		float result = kHeightScalar * eterm * kDotWhat * expf(-k2 * l2);
		// Waves running against the wind are suppressed, not removed.
		if (kDotW < 0.0f) result *= 0.25f;
		return result;
	}

	// In-place iterative radix-2 FFT. `sign` is +1 for the inverse transform
	// (the direction this file only ever needs) and -1 for the forward one.
	//
	// Written here rather than linked: upstream uses FFTW, and a 128-point
	// power-of-two transform is a page of code against a library to build,
	// ship and license on Android.
	void fft1D(std::vector<Complex> &data, int stride, int offset, int n, float sign)
	{
		// Bit-reversal permutation.
		for (int i = 1, j = 0; i < n; i++) {
			int bit = n >> 1;
			for (; j & bit; bit >>= 1) j ^= bit;
			j ^= bit;
			if (i < j) std::swap(data[offset + i * stride], data[offset + j * stride]);
		}
		for (int len = 2; len <= n; len <<= 1) {
			const float angle = sign * 2.0f * (float) M_PI / (float) len;
			const Complex step(cosf(angle), sinf(angle));
			for (int i = 0; i < n; i += len) {
				Complex w(1.0f, 0.0f);
				for (int j = 0; j < len / 2; j++) {
					Complex &u = data[offset + (i + j) * stride];
					Complex &v = data[offset + (i + j + len / 2) * stride];
					const Complex t = v * w;
					v = u - t;
					u = u + t;
					w *= step;
				}
			}
		}
	}

	// Rows then columns: a separable 2D transform.
	void fft2D(std::vector<Complex> &grid, float sign)
	{
		for (int y = 0; y < N; y++) fft1D(grid, 1, y * N, N, sign);
		for (int x = 0; x < N; x++) fft1D(grid, N, x, N, sign);
	}
}

namespace ScorchDroidOcean
{
	void reseed(float windSpeed, float windDirectionRadians, unsigned int seed)
	{
		// Upstream's own guard: a dead calm still has a sea, so the
		// generator is given a direction and a floor rather than nothing.
		if (windSpeed < 1.0f) windSpeed = 1.0f;
		const float windX = sinf(windDirectionRadians);
		const float windY = cosf(windDirectionRadians);

		std::vector<Complex> h0((size_t) (N + 1) * (N + 1));
		unsigned int state = seed ? seed : 1u;
		const float twoPi = 2.0f * (float) M_PI;
		for (int y = 0; y <= N; y++) {
			const float ky = twoPi * (float) (y - N / 2) / kL;
			for (int x = 0; x <= N; x++) {
				const float kx = twoPi * (float) (x - N / 2) / kL;
				const Complex g = gaussianPair(state);
				// h0 = gaussian * sqrt(P(k)/2), upstream's h0_tilde.
				h0[(size_t) y * (N + 1) + x] =
					g * sqrtf(0.5f * phillips(kx, ky, windX, windY, windSpeed));
			}
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		g_h0.swap(h0);
		g_seeded = true;
		// A new sea starts with no foam. Upstream's rndtab: 37 random
		// values that stagger the decay so it does not fade uniformly.
		g_aof.assign((size_t) N * N, 0.0f);
		for (int i = 0; i < 37; i++) {
			state = state * 1664525u + 1013904223u;
			g_aofRandom[i] = (float) ((state >> 8) & 0xffffffu) / (float) 0x1000000u;
		}
		g_lastSeconds = -1.0f;
	}

	void generate(float seconds, Tile &out)
	{
		const size_t count = (size_t) N * N;
		out.height.assign(count, 0.0f);
		out.dispX.assign(count, 0.0f);
		out.dispZ.assign(count, 0.0f);
		out.normalX.assign(count, 0.0f);
		out.normalY.assign(count, 1.0f);
		out.normalZ.assign(count, 0.0f);
		out.foam.assign(count, 0.0f);

		std::vector<Complex> h0;
		std::vector<float> aof;
		float aofRandom[37];
		float phases;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_seeded) return;
			h0 = g_h0;
			aof.swap(g_aof);
			for (int i = 0; i < 37; i++) aofRandom[i] = g_aofRandom[i];
			// Phases elapsed since the last call: one on the first, and
			// never more than a cycle's worth, so a long pause does not
			// run the decay to absurd values.
			phases = (g_lastSeconds < 0.0f) ? 1.0f
				: std::min(std::max((seconds - g_lastSeconds) * kPhasesPerSecond, 0.0f), kPhases);
			g_lastSeconds = seconds;
		}
		if (aof.size() != count) aof.assign(count, 0.0f);

		// Every wave's frequency is rounded down to a multiple of the base
		// frequency, exactly as upstream does, so the whole surface repeats
		// on the cycle rather than drifting out of phase with itself.
		const float w0 = 2.0f * (float) M_PI / kCycleSeconds;
		const float twoPi = 2.0f * (float) M_PI;

		std::vector<Complex> heightSpectrum(count);
		std::vector<Complex> dispSpectrum(count);
		for (int y = 0; y < N; y++) {
			const float ky = twoPi * (float) (y - N / 2) / kL;
			for (int x = 0; x < N; x++) {
				const float kx = twoPi * (float) (x - N / 2) / kL;

				const Complex a = h0[(size_t) y * (N + 1) + x];
				const Complex b = std::conj(h0[(size_t) (N - y) * (N + 1) + (N - x)]);
				const float k = sqrtf(kx * kx + ky * ky);
				const float wK = floorf(sqrtf(kGravity * k) / w0) * w0;
				const float phase = wK * seconds;
				const Complex rot(cosf(phase), sinf(phase));
				const Complex h = a * rot + b * std::conj(rot);

				heightSpectrum[(size_t) y * N + x] = h;

				// Upstream's compute_displacements: the spectrum of the
				// horizontal displacement is -i * h * k/|k|, one for each
				// axis. Both at once: x in the real part and z in the
				// imaginary one, which a single transform separates
				// because each is itself a real (Hermitian) field.
				Complex unitK(0.0f, 0.0f);
				if (k > 0.0f) unitK = Complex(kx / k, ky / k);
				dispSpectrum[(size_t) y * N + x] = Complex(0.0f, -1.0f) * h * unitK;
			}
		}

		fft2D(heightSpectrum, 1.0f);
		fft2D(dispSpectrum, 1.0f);

		// The spectrum was laid out with k = 0 in the middle, so the result
		// comes back with the tile's origin in the corners; the (-1)^(x+y)
		// factor shifts it back, which is the usual fftshift done in the
		// spatial domain because it is free here.
		//
		// The displacement's scale is upstream's own: it passes -2.0 as the
		// scale factor, Tessendorf's lambda, so a point moves sideways by
		// about twice what it rises.
		const float kChoppiness = -2.0f;
		for (int y = 0; y < N; y++) {
			for (int x = 0; x < N; x++) {
				const size_t i = (size_t) y * N + x;
				const float sign = ((x + y) & 1) ? -1.0f : 1.0f;
				out.height[i] = heightSpectrum[i].real() * sign;
				out.dispX[i] = dispSpectrum[i].real() * sign * kChoppiness;
				out.dispZ[i] = dispSpectrum[i].imag() * sign * kChoppiness;
			}
		}

		// Normals of the displaced surface, as Water2Patch::generate builds
		// them: the point's displaced position against its four neighbours
		// (wrapping, since the tile repeats) at upstream's two units per
		// point, normal = normalize(cross(+x, +y) + cross(-x, -y)) in the
		// tile's Z-up frame. Mapped to Y-up on the way out.
		const float spacing = kL / (float) N;
		auto position = [&](int x, int y, float p[3]) {
			const int wx = (x + N) % N, wy = (y + N) % N;
			const size_t i = (size_t) wy * N + wx;
			p[0] = out.dispX[i] + (float) x * spacing;
			p[1] = out.dispZ[i] + (float) y * spacing;
			p[2] = out.height[i];
		};
		auto cross = [](const float a[3], const float b[3], float r[3]) {
			r[0] = a[1] * b[2] - a[2] * b[1];
			r[1] = a[2] * b[0] - a[0] * b[2];
			r[2] = a[0] * b[1] - a[1] * b[0];
		};
		for (int y = 0; y < N; y++) {
			for (int x = 0; x < N; x++) {
				float c[3], px[3], py[3], mx[3], my[3];
				position(x, y, c);
				position(x + 1, y, px);
				position(x, y + 1, py);
				position(x - 1, y, mx);
				position(x, y - 1, my);
				float d1[3], d2[3], d3[3], d4[3];
				for (int j = 0; j < 3; j++) {
					d1[j] = px[j] - c[j]; d2[j] = py[j] - c[j];
					d3[j] = mx[j] - c[j]; d4[j] = my[j] - c[j];
				}
				float n1[3], n2[3];
				cross(d1, d2, n1);
				cross(d3, d4, n2);
				float n[3] = { n1[0] + n2[0], n1[1] + n2[1], n1[2] + n2[2] };
				const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				const size_t i = (size_t) y * N + x;
				if (len > 1e-12f) {
					// Tile (x, y, up) -> world (x, up, z).
					out.normalX[i] = n[0] / len;
					out.normalY[i] = n[2] / len;
					out.normalZ[i] = n[1] / len;
				}
			}
		}

		// Whitecaps, transcribed from Water2::generateAOF. The Jacobian of
		// the horizontal displacement says how much a cell has been
		// squeezed: below zero the surface has folded over itself, which
		// is a breaking crest, and that much foam is spawned there and
		// half as much on its four neighbours. Then everything decays.
		// Upstream's numbers throughout; its derivative factor is
		// wave_resolution / wavetile_length, and it takes the difference
		// across the two neighbours as it stands, so that is kept too.
		{
			const float derivFac = (float) N / kL;
			const float lambda = 1.0f;   // already in the displacements
			const float decay = 4.0f / kPhases;
			const float decayRnd = 0.25f / kPhases;
			const float spawnFac = 0.25f;
			for (int y = 0; y < N; y++) {
				const int ym1 = (y + N - 1) & (N - 1), yp1 = (y + 1) & (N - 1);
				for (int x = 0; x < N; x++) {
					const int xm1 = (x + N - 1) & (N - 1), xp1 = (x + 1) & (N - 1);
					const size_t iXp = (size_t) y * N + xp1, iXm = (size_t) y * N + xm1;
					const size_t iYp = (size_t) yp1 * N + x, iYm = (size_t) ym1 * N + x;
					const float dxdx = (out.dispX[iXp] - out.dispX[iXm]) * derivFac;
					const float dxdy = (out.dispX[iYp] - out.dispX[iYm]) * derivFac;
					const float dydx = (out.dispZ[iXp] - out.dispZ[iXm]) * derivFac;
					const float dydy = (out.dispZ[iYp] - out.dispZ[iYm]) * derivFac;
					const float jxx = 1.0f + lambda * dxdx;
					const float jyy = 1.0f + lambda * dydy;
					const float jxy = lambda * dydx;
					const float jyx = lambda * dxdy;
					const float jac = jxx * jyy - jxy * jyx;
					const float foamAdd = (jac < 0.0f) ? ((jac < -1.0f) ? 1.0f : -jac) : 0.0f;
					if (foamAdd <= 0.0f) continue;
					const float spawn = foamAdd * spawnFac * phases;
					aof[(size_t) y * N + x] += spawn;
					aof[iYm] += spawn * 0.5f;
					aof[iYp] += spawn * 0.5f;
					aof[iXm] += spawn * 0.5f;
					aof[iXp] += spawn * 0.5f;
				}
			}
			for (int y = 0; y < N; y++) {
				for (int x = 0; x < N; x++) {
					const size_t i = (size_t) y * N + x;
					const float fade = (decay + decayRnd * aofRandom[(3 * x + 5 * y) % 37]) * phases;
					aof[i] = std::max(std::min(aof[i], 1.0f) - fade, 0.0f);
					out.foam[i] = aof[i];
				}
			}
		}

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			// Hand the history back - unless a reseed happened meanwhile,
			// which leaves a fresh, empty-of-foam vector in place; a new
			// sea keeps its clean start.
			if (g_aof.empty()) g_aof.swap(aof);
		}
	}
}

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
	}

	void generate(float seconds, Tile &out)
	{
		out.height.assign((size_t) N * N, 0.0f);
		out.slopeX.assign((size_t) N * N, 0.0f);
		out.slopeZ.assign((size_t) N * N, 0.0f);

		std::vector<Complex> h0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_seeded) return;
			h0 = g_h0;
		}

		// Every wave's frequency is rounded down to a multiple of the base
		// frequency, exactly as upstream does, so the whole surface repeats
		// on the cycle rather than drifting out of phase with itself.
		const float w0 = 2.0f * (float) M_PI / kCycleSeconds;
		const float twoPi = 2.0f * (float) M_PI;

		std::vector<Complex> heightSpectrum((size_t) N * N);
		std::vector<Complex> slopeSpectrum((size_t) N * N);
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
				// Both slopes at once: multiplying by i*k differentiates, and
				// packing dx into the real part and dz into the imaginary one
				// gets them from a single transform.
				slopeSpectrum[(size_t) y * N + x] = Complex(0.0f, 1.0f) * h * Complex(kx, ky);
			}
		}

		fft2D(heightSpectrum, 1.0f);
		fft2D(slopeSpectrum, 1.0f);

		// The spectrum was laid out with k = 0 in the middle, so the result
		// comes back with the tile's origin in the corners; the (-1)^(x+y)
		// factor shifts it back, which is the usual fftshift done in the
		// spatial domain because it is free here.
		for (int y = 0; y < N; y++) {
			for (int x = 0; x < N; x++) {
				const size_t i = (size_t) y * N + x;
				const float sign = ((x + y) & 1) ? -1.0f : 1.0f;
				out.height[i] = heightSpectrum[i].real() * sign;
				out.slopeX[i] = slopeSpectrum[i].real() * sign;
				out.slopeZ[i] = slopeSpectrum[i].imag() * sign;
			}
		}

		// Not normalised: the heights are world units, see kHeightScalar.
	}
}

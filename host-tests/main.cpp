// Fast, non-Android verification of the game-logic code shared with the
// Android build (see the porting plan and CMakeLists.txt in this
// directory). Deliberately a plain assert-based runner rather than a test
// framework dependency - the point is a fast host compile+run loop, not
// test-reporting features, and the suite is small enough that a framework
// would be more machinery than the job needs.

#include <common/Defines.hpp>
#include <common/fixed.hpp>
#include <server/ScorchedServer.hpp>
#include <server/ScorchedServerSettings.hpp>
#include <weapons/AccessoryStore.hpp>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace
{
	int failures = 0;

	void check(bool condition, const char *description)
	{
		if (condition)
		{
			printf("  [PASS] %s\n", description);
		}
		else
		{
			printf("  [FAIL] %s\n", description);
			failures++;
		}
	}

	// fixed's arithmetic underpins every gameplay calculation this port
	// wants to keep byte-for-byte compatible with a PC build (see the
	// porting plan's PC cross-play notes) - deterministic fixed-point math,
	// not floats, is exactly what makes that plausible, so it's worth its
	// own direct sanity check independent of anything data-driven.
	void testFixedPointMath()
	{
		printf("fixed-point math:\n");
		check(fixed(1) + fixed(1) == fixed(2), "1 + 1 == 2");
		check(fixed(10) / fixed(4) == fixed(true, fixed::FIXED_RESOLUTION * 5 / 2), "10 / 4 == 2.5");
		check(fixed(3) * fixed(3) == fixed(9), "3 * 3 == 9");
		check(fixed(5) > fixed(4), "5 > 4");
		check(fixed(-1).abs() == fixed(1), "abs(-1) == 1");

		fixed a(true, 12345);
		fixed b = a;
		b += fixed(1);
		check(a.getInternalData() == 12345, "fixed copy is independent of the original");
		check(b.getInternalData() == 12345 + fixed::FIXED_RESOLUTION, "+= advances by exactly one FIXED_RESOLUTION unit");
	}

	// Boots the real, unmodified ScorchedServer bootstrap path (the same
	// one NativeBridge.startLocalGame() drives on Android) against
	// upstream's actual bundled data - not a mock or a subset - and checks
	// that a specific, hand-verified accessory parsed correctly. This is
	// the regression check: if a future porting patch subtly changes
	// parsing/loading behavior, this fails fast on a laptop instead of only
	// showing up as a confusing runtime difference on-device.
	void testRealAccessoryDataLoads()
	{
		printf("real accessory data (data/globalmods/none/data/accessories.xml):\n");

		ScorchedServerSettingsOptions settings("data/server.xml", false, false);
		bool started = ScorchedServer::startServer(settings, true, nullptr);
		check(started, "ScorchedServer::startServer() succeeds against real upstream data");
		if (!started) return;

		AccessoryStore &store = ScorchedServer::instance()->getAccessoryStore();
		Accessory *babyMissile = store.findByPrimaryAccessoryName("Baby Missile");
		check(babyMissile != nullptr, "\"Baby Missile\" accessory was parsed");
		if (babyMissile)
		{
			check(babyMissile->getArmsLevel() == 10, "\"Baby Missile\" armslevel == 10 (data/globalmods/none/data/accessories.xml)");
		}
	}
}

int main()
{
	// Point cwd/$HOME/settings dir at the real submodule checkout, exactly
	// like Android's initEngine() JNI function does at the extracted asset
	// root - see the porting plan. No asset-extraction step is needed here
	// since this is a plain Linux process with normal filesystem access.
	if (chdir(SCORCHED_SUBMODULE_ROOT) != 0)
	{
		fprintf(stderr, "chdir(%s) failed\n", SCORCHED_SUBMODULE_ROOT);
		return 1;
	}
	setenv("HOME", "/tmp/scorchdroid-host-tests-home", 1);
	S3D::setSettingsDir("scorchdroid-host-tests");

	testFixedPointMath();
	testRealAccessoryDataLoads();

	printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
		failures, failures == 1 ? "" : "s");
	return failures == 0 ? 0 : 1;
}

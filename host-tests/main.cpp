// Fast, non-Android verification of the game-logic code shared with the
// Android build (see the porting plan and CMakeLists.txt in this
// directory). Deliberately a plain assert-based runner rather than a test
// framework dependency - the point is a fast host compile+run loop, not
// test-reporting features, and the suite is small enough that a framework
// would be more machinery than the job needs.

#include <Mat4.hpp>
#include <common/Defines.hpp>
#include <common/fixed.hpp>
#include <server/ScorchedServer.hpp>
#include <server/ScorchedServerSettings.hpp>
#include <server/ServerSimulator.hpp>
#include <server/ServerDestinations.hpp>
#include <weapons/AccessoryStore.hpp>
#include <weapons/AccessoryPart.hpp>
#include <simactions/TankAddSimAction.hpp>
#include <simactions/TankAccessorySimAction.hpp>
#include <tankai/TankAIAdder.hpp>
#include <target/TargetContainer.hpp>
#include <tank/Tank.hpp>
#include <tank/TankScore.hpp>
#include <tanket/TanketAccessories.hpp>
#include <tanket/TanketWeapon.hpp>
#include <coms/ComsBuyAccessoryMessage.hpp>
// M6 parity: defense accessories (shields/parachutes/batteries) - see
// testDefenseAccessories().
#include <coms/ComsDefenseMessage.hpp>
#include <simactions/TankDefenseSimAction.hpp>
#include <tank/TankState.hpp>
#include <target/TargetLife.hpp>
#include <target/TargetShield.hpp>
#include <tanket/TanketBatteries.hpp>
// M6 parity: non-shot player moves (resign/skip/finished-buying) - see
// testNonShotMoves(); getMoveId() lives on TanketShotInfo.
#include <tanket/TanketShotInfo.hpp>
#include <coms/ComsPlayedMoveMessage.hpp>
#include <engine/Simulator.hpp>
#include <net/NetServerTCP3.hpp>
#include <net/NetMessage.hpp>
#include <server/ServerState.hpp>
#include <server/ServerFileServer.hpp>
#include <server/ServerChannelManager.hpp>
#include <server/ServerTimedMessage.hpp>
#include <server/ServerConnectAuthHandler.hpp>
#include <common/Clock.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <landscapemap/DeformLandscape.hpp>
#include <landscapemap/MovementMap.hpp>
#include <TargetModelStore.h>
#include <SkyDescription.hpp>
#include <ChatStore.h>
#include <InstanceBuffer.hpp>
#include <TreeGeometry.hpp>
#include <weapons/WeaponMoveTank.hpp>
#include <LandscapeTextureBuilder.hpp>
#include <DeformEventQueue.h>
#include <landscapedef/LandscapeDefinition.hpp>
#include <landscapedef/LandscapeTex.hpp>
#include <EffectEventQueue.h>
#include <SoundEventQueue.h>
#include <actions/ShieldHit.hpp>
#include <actions/TankSay.hpp>
#include <actions/ShowScoreAction.hpp>
#include <ScoreboardState.h>
#include <GameSetup.h>
#include <PlayerProfile.h>
#include <AmbientSound.h>
#include <landscapedef/LandscapeDefinitionsBase.hpp>
#include <tankai/TankAIStore.hpp>
#include <tankai/TankAI.hpp>
#include <common/FixedVector4.hpp>
#include <target/TargetDamage.hpp>
#include <target/TargetState.hpp>
#include <weapons/Weapon.hpp>
#include <lang/LangResource.hpp>
#include <landscapedef/LandscapeDefinitions.hpp>
#include <common/OptionsGame.hpp>
#include <ClientContext.hpp>
#include <ClientSync.hpp>

#include <cstdio>
#include <fstream>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <execinfo.h>
#include <map>
#include <set>
#include <functional>

// Kept permanently, not just for the one crash it was written to catch
// (see git history/the porting plan for that story): this sandbox doesn't
// produce OS-level core dumps, and gdb's own slowdown can hide timing-
// sensitive crashes entirely, so a signal handler that prints its own
// backtrace is the only reliable way to diagnose a future one here.
static void crashBacktraceHandler(int sig)
{
	void *frames[32];
	int count = backtrace(frames, 32);
	fprintf(stderr, "DBG crash signal %d, backtrace:\n", sig);
	backtrace_symbols_fd(frames, count, 2);
	_exit(139);
}

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
	// ServerSimulator queues actions in sendActions_ and only promotes them
	// into simActions_ (where they actually invoke()) once currentTime_
	// catches up to a "send boundary" a couple of fixed-seconds out (see
	// ServerSimulator::continueToSimulate/nextSendTime, Simulator::
	// simulateTime) - and Simulator::simulate() itself is a no-op unless
	// real wall-clock time has elapsed since its last call (SDL_GetTicks()
	// diff). A bare simulate() call right after addSimulatorAction() is
	// therefore flaky - it only happens to work if enough real time already
	// passed since the *previous* simulate() call (which is what made the
	// very first queued action in a test run tend to work by luck, riding
	// on however long ScorchedServer::startServer() itself took), and one
	// fixed guess at "enough sleep" is fragile against exactly how many
	// fixed-seconds out that boundary really is. So poll instead: keep
	// sleeping+simulating (matching how the real Android tickEngine() loop
	// naturally ticks every >100ms) until the predicate passes or a
	// generous timeout elapses.
	void simulateUntil(ScorchedServer *server, const std::function<bool()> &done, const char *what)
	{
		for (int i = 0; i < 40; i++)
		{
			if (done()) return;
			usleep(300 * 1000);
			server->getSimulator().simulate();
		}
		fprintf(stderr, "WARNING: simulateUntil timed out waiting for: %s\n", what);
	}

	// M4: regression check for engine_jni.cpp's addHumanTank() mechanism -
	// see the porting plan's TankAddSimAction/TankAIAdder/
	// ServerConnectAuthHandler investigation. A tank added the same way a
	// real network client's tank is (non-zero destinationId, empty aiName)
	// must end up with no TankAI attached, exactly matching a real human
	// player and unlike every bot-added tank.
	void testHumanTankHasNoAI()
	{
		printf("human tank (engine_jni.cpp's addHumanTank mechanism):\n");

		ScorchedServer *server = ScorchedServer::instance();
		const unsigned int kHumanDestinationId = 1;
		// A real client gets a ServerDestination registered for it by
		// ServerMessageHandler::clientConnected() when it physically
		// connects. Without this, TankAddSimAction::invokeAction() notices
		// there's no ServerDestination for a non-zero destinationId and
		// immediately re-destroys the tank on the very next tick (found by
		// this test suite - see engine_jni.cpp's addHumanTank()).
		server->getServerDestinations().addDestination(kHumanDestinationId, 0);

		std::set<unsigned int> takenPlayerIds;
		unsigned int tankId = TankAIAdder::getNextTankId("", server->getContext(), takenPlayerIds);

		TankAddSimAction *simAction = new TankAddSimAction(
			tankId, kHumanDestinationId,
			"", "", "", 0,
			LANG_STRING("Player"), "");
		server->getServerSimulator().addSimulatorAction(simAction);
		simulateUntil(server, [&] { return server->getTargetContainer().getTankById(tankId) != nullptr; },
			"human tank added to the target container");

		Tank *tank = server->getTargetContainer().getTankById(tankId);
		check(tank != nullptr, "human tank was added to the target container");
		if (tank)
		{
			check(tank->getDestinationId() == kHumanDestinationId,
				"human tank kept its non-zero destinationId");
			check(tank->getTankAI() == nullptr,
				"human tank has no TankAI attached (unlike a bot)");
		}

		// Regression check for the exact bug this test caught: a few more
		// simulated seconds must NOT re-destroy the tank (it did, before
		// addDestination() was added to addHumanTank()).
		usleep(1200 * 1000);
		server->getSimulator().simulate();
		check(server->getTargetContainer().getTankById(tankId) != nullptr,
			"human tank is still present after further simulation (not re-destroyed)");
	}

	// M4: regression check for engine_jni.cpp's economy JNI surface
	// (getMyMoney/getWeaponShop/buyAccessory/selectWeapon) - reuses the
	// human tank from testHumanTankHasNoAI (must run first). Exercises the
	// exact same public-API sequence buyAccessory()/selectWeapon() use:
	// TankAccessorySimAction queued via ServerSimulator, then
	// Simulator::simulate() to actually apply it - not a mock of the
	// economy, the real upstream money/ownership logic in
	// TankAccessorySimAction::invokeAction().
	void testEconomyBuyAndSelect()
	{
		printf("economy (engine_jni.cpp's buyAccessory/selectWeapon mechanism):\n");

		ScorchedServer *server = ScorchedServer::instance();
		Tank *tank = nullptr;
		std::map<unsigned int, Tank *> &tanks = server->getTargetContainer().getTanks();
		for (auto &entry : tanks)
		{
			if (entry.second->getDestinationId() == 1) { tank = entry.second; break; }
		}
		check(tank != nullptr, "human tank from the previous test is still present");
		if (!tank) return;

		// "Baby Missile" won't work here - it's the tank's default
		// infinite-ammo starting weapon (startingnumber=-1 in
		// accessories.xml), and accessoryAllowed() explicitly refuses to
		// "buy" an already-infinite accessory again. "Missile" is a real
		// limited-quantity purchasable weapon (bundlesize=5, cost=2000,
		// not owned at match start), so it actually exercises the buy path.
		Accessory *missile = server->getAccessoryStore().findByPrimaryAccessoryName("Missile");
		check(missile != nullptr, "\"Missile\" accessory is available to buy");
		if (!missile) return;

		int startingCount = tank->getAccessories().getAccessoryCount(missile);
		tank->getScore().setMoney(missile->getPrice());

		ComsBuyAccessoryMessage buyMessage(tank->getPlayerId(), missile->getAccessoryId(), true);
		server->getServerSimulator().addSimulatorAction(new TankAccessorySimAction(buyMessage));
		simulateUntil(server, [&] { return tank->getAccessories().getAccessoryCount(missile) != startingCount; },
			"buy accessory action applied");

		check(tank->getAccessories().getAccessoryCount(missile) == startingCount + missile->getBundle(),
			"buying added the accessory to the tank's inventory");
		check(tank->getScore().getMoney() == 0,
			"buying deducted the accessory's price from the tank's money");

		bool selected = tank->getAccessories().getWeapons().setWeapon(missile);
		check(selected, "selectWeapon's setWeapon() call succeeds for an owned accessory");
		check(tank->getAccessories().getWeapons().getCurrent() == missile,
			"the selected accessory is now the tank's current weapon");
	}

	// M6 parity: regression check for engine_jni.cpp's useDefense() -
	// shields/parachutes/batteries were completely unreachable before M6
	// (getWeaponShop() filtered to AccessoryWeapon, so they could be
	// neither bought nor used), which the M6 control audit found was the
	// biggest gameplay-parity gap. Exercises the exact same public-API
	// sequence useDefense() does: a real ComsDefenseMessage wrapped in a
	// TankDefenseSimAction, queued via ServerSimulator - the real upstream
	// TankDefenseSimAction::invokeAction() then does all the actual
	// battery/shield work, so this is not a mock of the defense system.
	//
	// Deliberately a host test rather than an on-device UI test: driving
	// this through the Android UI needs a 30-second buying window to be
	// caught by scripted taps, which is slow and flaky - whereas the thing
	// actually worth verifying (does the message+sim-action path do what
	// upstream expects) is pure game logic that runs identically here in
	// seconds. Reuses the human tank from testHumanTankHasNoAI.
	void testDefenseAccessories()
	{
		printf("defenses (engine_jni.cpp's useDefense mechanism):\n");

		ScorchedServer *server = ScorchedServer::instance();
		Tank *tank = nullptr;
		std::map<unsigned int, Tank *> &tanks = server->getTargetContainer().getTanks();
		for (auto &entry : tanks)
		{
			if (entry.second->getDestinationId() == 1) { tank = entry.second; break; }
		}
		check(tank != nullptr, "human tank from the earlier test is still present");
		if (!tank) return;

		// TankDefenseSimAction::invokeAction() no-ops unless the tank is
		// "playing" (sNormal/sDead/sBuying - see TankState::getTankPlaying),
		// and a tank added directly like this sits in sLoading because it
		// never went through a real round start.
		//
		// Gotcha worth knowing: TankState::setState() is NOT a plain
		// setter - it silently ignores any transition that isn't in its
		// allowedStateTransitions table, with no error. sLoading -> sNormal
		// is not in that table, so the obvious one-liner does nothing at
		// all and every defense action then correctly no-ops. sLoading ->
		// sDead -> sNormal is a legal path, hence the two-step below.
		auto keepPlaying = [&] {
			TankState &state = tank->getState();
			if (state.getState() == TankState::sLoading) state.setState(TankState::sDead);
			if (state.getState() != TankState::sNormal) state.setState(TankState::sNormal);
		};
		keepPlaying();
		check(tank->getPlaying(),
			"test tank could be put into a playing state (setState honours only legal transitions)");

		// --- battery: buy one, take damage, then repair with it ---
		Accessory *battery = server->getAccessoryStore().findByPrimaryAccessoryName("Battery");
		check(battery != nullptr, "\"Battery\" accessory exists in the real data");
		if (!battery) return;
		check(battery->getType() == AccessoryPart::AccessoryBattery,
			"\"Battery\" is an AccessoryBattery (a type the pre-M6 shop filtered out entirely)");

		tank->getScore().setMoney(battery->getPrice());
		int batteriesBefore = tank->getAccessories().getAccessoryCount(battery);
		ComsBuyAccessoryMessage buyBattery(tank->getPlayerId(), battery->getAccessoryId(), true);
		server->getServerSimulator().addSimulatorAction(new TankAccessorySimAction(buyBattery));
		simulateUntil(server, [&] { return tank->getAccessories().getAccessoryCount(battery) != batteriesBefore; },
			"battery purchase applied");
		check(tank->getAccessories().getAccessoryCount(battery) > batteriesBefore,
			"a non-weapon accessory can actually be bought through the real economy");

		// Consuming the battery from the tank's inventory is the
		// unambiguous evidence the action ran - the tank's life is also
		// repaired (+10, see TankDefenseSimAction) but life is something
		// the surrounding state machine touches on its own, so it's
		// checked as a follow-up rather than as the primary signal.
		// The *queuing* half of useDefense() (addSimulatorAction, then the
		// simulator promoting and invoking it) is already proven by the
		// TankAccessorySimAction buys above and in testEconomyBuyAndSelect
		// - it's the same ServerSimulator path, and SimAction type makes
		// no difference to it. What's worth isolating here is the defense
		// logic itself, so invokeAction() is called directly: going
		// through the simulator instead means racing the server's round
		// state machine, which keeps resetting this directly-added tank
		// back to sLoading (making getPlaying() false, so
		// TankDefenseSimAction correctly no-ops) at unpredictable moments
		// between queuing and promotion.
		int batteriesOwned = tank->getAccessories().getAccessoryCount(battery);
		// TargetLife::setLife() clamps to maxLife, and maxLife is still 0
		// on a tank that never went through a real match start - so
		// without setting it first, "damage the tank" silently leaves it
		// at 0 life and the battery's repair has nothing to show.
		tank->getLife().setMaxLife(fixed(100));
		tank->getLife().setLife(fixed(80));
		keepPlaying();
		ComsDefenseMessage useBattery(tank->getPlayerId(),
			ComsDefenseMessage::eBatteryUse, battery->getAccessoryId());
		TankDefenseSimAction(useBattery).invokeAction(server->getContext());
		check(tank->getAccessories().getAccessoryCount(battery) < batteriesOwned,
			"using a battery consumed one from the tank's inventory");
		check(tank->getLife().getLife() > fixed(80),
			"using a battery repaired the tank");

		// --- shield: buy one, raise it, lower it ---
		Accessory *shield = server->getAccessoryStore().findByPrimaryAccessoryName("Shield");
		check(shield != nullptr, "\"Shield\" accessory exists in the real data");
		if (!shield) return;
		check(shield->getType() == AccessoryPart::AccessoryShield,
			"\"Shield\" is an AccessoryShield (also filtered out pre-M6)");

		tank->getScore().setMoney(shield->getPrice());
		int shieldsBefore = tank->getAccessories().getAccessoryCount(shield);
		ComsBuyAccessoryMessage buyShield(tank->getPlayerId(), shield->getAccessoryId(), true);
		server->getServerSimulator().addSimulatorAction(new TankAccessorySimAction(buyShield));
		simulateUntil(server, [&] { return tank->getAccessories().getAccessoryCount(shield) != shieldsBefore; },
			"shield purchase applied");
		check(tank->getAccessories().getAccessoryCount(shield) > shieldsBefore,
			"a shield can be bought through the real economy");

		check(tank->getShield().getCurrentShield() == nullptr, "no shield is up to begin with");
		keepPlaying();
		ComsDefenseMessage shieldUp(tank->getPlayerId(),
			ComsDefenseMessage::eShieldUp, shield->getAccessoryId());
		TankDefenseSimAction(shieldUp).invokeAction(server->getContext());
		check(tank->getShield().getCurrentShield() == shield,
			"raising a shield sets it as the tank's current shield (what getActiveDefenses reports)");
		check(tank->getAccessories().getAccessoryCount(shield) < shieldsBefore + shield->getBundle(),
			"raising a shield consumed one from the tank's inventory");

		keepPlaying();
		ComsDefenseMessage shieldDown(tank->getPlayerId(), ComsDefenseMessage::eShieldDown, 0);
		TankDefenseSimAction(shieldDown).invokeAction(server->getContext());
		check(tank->getShield().getCurrentShield() == nullptr,
			"lowering the shield clears it again");
	}

	// M6 parity: regression check for engine_jni.cpp's submitMove() - the
	// non-shot player moves (eResign/eSkip/eFinishedBuy). ScorchDroid could
	// only ever send eShot before M6, so a player could neither skip a
	// turn, resign a round, nor end the buying phase early.
	//
	// What's worth pinning down here is the routing, since it's the one
	// place submitMove() can't just mirror fireWeapon(): eFinishedBuy goes
	// to ServerState::buyingFinished() while everything else goes to
	// moveFinished() (see ServerPlayedMoveHandler.cpp). Getting that
	// backwards would silently do nothing, which is exactly the kind of
	// bug that only shows up as "the button doesn't work".
	void testNonShotMoves()
	{
		printf("non-shot moves (engine_jni.cpp's submitMove mechanism):\n");

		ScorchedServer *server = ScorchedServer::instance();
		Tank *tank = nullptr;
		std::map<unsigned int, Tank *> &tanks = server->getTargetContainer().getTanks();
		for (auto &entry : tanks)
		{
			if (entry.second->getDestinationId() == 1) { tank = entry.second; break; }
		}
		check(tank != nullptr, "human tank from the earlier test is still present");
		if (!tank) return;

		// These go through ServerState, which happily accepts a move for a
		// tank whose turn it isn't (it just ignores it) - so the assertion
		// here is that the calls are well-formed and routed without
		// crashing or being rejected outright, not that the round advances.
		unsigned int moveId = tank->getShotInfo().getMoveId();

		ComsPlayedMoveMessage skip(tank->getPlayerId(), moveId, ComsPlayedMoveMessage::eSkip);
		check(skip.getType() == ComsPlayedMoveMessage::eSkip, "a skip move message is built with the right type");
		server->getServerState().moveFinished(skip);

		ComsPlayedMoveMessage resign(tank->getPlayerId(), moveId, ComsPlayedMoveMessage::eResign);
		check(resign.getType() == ComsPlayedMoveMessage::eResign, "a resign move message is built with the right type");
		server->getServerState().moveFinished(resign);

		ComsPlayedMoveMessage doneBuying(tank->getPlayerId(), moveId, ComsPlayedMoveMessage::eFinishedBuy);
		check(doneBuying.getType() == ComsPlayedMoveMessage::eFinishedBuy,
			"a finished-buying move message is built with the right type");
		server->getServerState().buyingFinished(doneBuying);

		server->getSimulator().simulate();
		check(server->getTargetContainer().getTankById(tank->getPlayerId()) != nullptr,
			"the server survived all three non-shot moves (routing is well-formed)");
	}

	// M6: proof that terrain destruction is already live in our build, not
	// something still to be ported. DeformLandscape lives in
	// src/common/landscapemap (which we compile) and is driven from
	// src/common/actions (Explosion, Napalm, TargetFalling, Teleport,
	// TanketMovement, Resurrection - also ours), so craters really do form
	// in the shared simulation. Only the *visual* half is missing: under
	// S3D_SERVER the client-notify block (Landscape::recalculateLandscape,
	// VisibilityPatchGrid::recalculateLandscapeErrors, DeformTextures for
	// scorch marks) is compiled out, and our renderer builds its mesh once
	// and never rebuilds it - so the ground changes shape underneath a
	// picture that never updates.
	//
	// Worth a test because it pins down which half of the problem is which:
	// if this passes, the M6 work is purely presentation.
	void testTerrainDeformation()
	{
		printf("terrain destruction (DeformLandscape, shared sim layer):\n");

		ScorchedServer *server = ScorchedServer::instance();

		// A landscape only exists once a round actually starts, and these
		// tests never run the server state machine that far - so generate
		// one the same way ServerStateNewGame does (and the same way our
		// own ClientContext does on ComsLoadLevelMessage).
		//
		// Deliberately a *real* random landscape, not getBlankLandscapeDefn():
		// the blank one is flat at the minimum land height, and
		// deformLandscapeInternal() clamps every crater to that floor (and
		// to the deform map), so nothing can be carved out of it at all.
		LandscapeDefinition defn = server->getLandscapes().getRandomLandscapeDefn(
			server->getContext().getOptionsGame(), server->getContext().getTargetContainer());
		server->getOptionsGame().updateLevelOptions(server->getContext(), defn);
		server->getLandscapeMaps().generateMaps(server->getContext(), defn, nullptr);

		HeightMap &hmap = server->getLandscapeMaps().getGroundMaps().getHeightMap();
		check(hmap.getMapWidth() > 0, "a landscape exists to deform");
		if (hmap.getMapWidth() <= 0) return;

		int cx = hmap.getMapWidth() / 2, cy = hmap.getMapHeight() / 2;
		fixed before = hmap.getHeight(cx, cy);

		// Start from a clean slate: generateMaps()/tank placement above may
		// already have flattened areas and queued regions of their own.
		ScorchDroidLandscape::clearDirtyRegion();

		FixedVector pos(fixed(cx), fixed(cy), before);
		DeformLandscape::deformLandscape(server->getContext(), pos, fixed(8), true, fixed(1), nullptr);

		fixed after = hmap.getHeight(cx, cy);
		check(after < before,
			"an explosion-style deform really lowers the shared heightmap (craters form in our build)");

		// The renderer can only redraw a crater if the engine tells it one
		// happened - the notification upstream uses (recalculateLandscape /
		// recalculateLandscapeErrors) is #ifndef S3D_SERVER, so this port
		// adds its own hook (patch 0010 + DeformEventQueue.h). Without it
		// the ground silently changes shape under a static mesh.
		int dMinX = 0, dMinY = 0, dMaxX = 0, dMaxY = 0;
		bool reported = ScorchDroidLandscape::takeDirtyRegion(dMinX, dMinY, dMaxX, dMaxY);
		// M6 effects: the weapon-effect hook (patch 0011). Checked with a
		// real ShieldHit Action rather than by poking the queue directly -
		// the queue itself is trivial, and what can actually break is the
		// patch: an #ifdef landing in the wrong branch, or a field read off
		// an object that is null server-side. ShieldHit is the cheapest of
		// the five to raise for real, since it needs only a shielded tank
		// and no weapon-fire plumbing.
		{
			ScorchDroidEffects::drain();  // ignore anything the round start raised

			// Raise the shield here rather than relying on a tank left
			// shielded by an earlier test - the round state machine keeps
			// running between tests and resets tanks, so borrowing state
			// across them is exactly the kind of order dependence that
			// makes a suite flaky.
			Tank *shieldedTank = nullptr;
			std::map<unsigned int, Tank *> &tanks = server->getTargetContainer().getTanks();
			if (!tanks.empty()) shieldedTank = tanks.begin()->second;
			Accessory *shieldAccessory =
				server->getAccessoryStore().findByPrimaryAccessoryName("Shield");
			check(shieldAccessory != nullptr, "found the \"Shield\" accessory to raise");
			if (shieldedTank && shieldAccessory)
			{
				shieldedTank->getShield().setCurrentShield(shieldAccessory);
			}
			check(shieldedTank != nullptr && shieldedTank->getShield().getCurrentShield() != nullptr,
				"a tank is shielded, so a shield hit has something to flash on");

			if (shieldedTank)
			{
				FixedVector hitPos = shieldedTank->getLife().getTargetPosition();
				ShieldHit shieldHit(shieldedTank->getPlayerId(), hitPos, fixed(true, 5000));
				shieldHit.setScorchedContext(&server->getContext());
				shieldHit.init();
				bool removeAction = false;
				shieldHit.simulate(fixed(true, 1000), removeAction);

				std::vector<ScorchDroidEffects::EffectEvent> effects = ScorchDroidEffects::drain();
				bool sawShieldHit = false;
				for (size_t i = 0; i < effects.size(); i++)
				{
					if (effects[i].type != ScorchDroidEffects::eShieldHit) continue;
					sawShieldHit = true;
					// Position must survive the trip: a renderer drawing the
					// flash at the origin would look like nothing happened.
					check(fabsf(effects[i].x - hitPos[0].asFloat()) < 0.01f &&
						  fabsf(effects[i].y - hitPos[1].asFloat()) < 0.01f,
						"the shield-hit effect carries the real impact position");
					check(effects[i].size > 0.0f,
						"the shield-hit effect is sized from the shield, not left at zero");
				}
				check(sawShieldHit, "a real ShieldHit action raises an effect event for the renderer");

				// M13: the same action also raises the shield's collision sound.
				// Upstream plays it from the client-only block this port
				// compiles out, so until the hook this was one of eight sound
				// sites that were simply silent.
				std::vector<std::string> sounds = ScorchDroidAudio::drainSoundEvents();
				bool sawWav = false;
				for (size_t i = 0; i < sounds.size(); i++)
				{
					if (sounds[i].find("data/wav/") != std::string::npos) sawWav = true;
				}
				check(sawWav, "...and the shield's own collision sound as a sound event");
			}
		}

		// The two *text* effects - the floating damage number over a hurt
		// target and the speech bubble over a tank that just spoke - go
		// through the same queue but are raised from code upstream gates on
		// !getServerMode(). That gate is right for upstream, where a client
		// process runs its own context alongside a local server context and
		// would otherwise do the work twice. It is wrong here: this port has
		// no such second context running these, so the gate silently
		// switched both effects off on the device while every ungated effect
		// (explosions, debris, wall flashes) worked. Both are checked
		// against the server context deliberately - that is the context the
		// app runs them on.
		{
			ScorchDroidEffects::drain();

			Tank *hurtTank = nullptr;
			std::map<unsigned int, Tank *> &damageTanks = server->getTargetContainer().getTanks();
			if (!damageTanks.empty()) hurtTank = damageTanks.begin()->second;
			check(hurtTank != nullptr, "found a tank to hurt");
			Accessory *missile =
				server->getAccessoryStore().findByPrimaryAccessoryName("Baby Missile");
			check(missile != nullptr, "found a weapon to attribute the damage to");
			if (hurtTank && missile)
			{
				// The shield raised just above would absorb this and leave
				// nothing to report.
				hurtTank->getShield().setCurrentShield(nullptr);
				hurtTank->getTargetState().setDisplayDamage(true);

				FixedVector velocity;
				WeaponFireContext fireContext(hurtTank->getPlayerId(), 0, 0, velocity, false, false);
				TargetDamage::damageTarget(server->getContext(),
					(Weapon *) missile->getAction(), hurtTank->getPlayerId(),
					fireContext, fixed(17), false, false, false);

				std::vector<ScorchDroidEffects::EffectEvent> effects = ScorchDroidEffects::drain();
				bool sawDamage = false;
				for (size_t i = 0; i < effects.size(); i++)
				{
					if (effects[i].type != ScorchDroidEffects::eDamage) continue;
					sawDamage = true;
					check(fabsf(effects[i].value - 17.0f) < 0.01f,
						"the damage event carries the amount, which is the whole point of the number");
					// The scatter upstream applies is +-2.5 per axis, so
					// anything further out than that is not this tank. Worth
					// asserting rather than assuming: upstream reads the
					// position from getFloatPosition(), a mirror that
					// TargetLife only maintains when !serverMode_, so on
					// this port it was a constant (0,0,0) and every number
					// was drawn in the corner of the map.
					FixedVector &hurtPos = hurtTank->getLife().getTargetPosition();
					check(fabsf(effects[i].x - hurtPos[0].asFloat()) <= 2.5f &&
						  fabsf(effects[i].y - hurtPos[1].asFloat()) <= 2.5f &&
						  fabsf(effects[i].z - hurtPos[2].asFloat()) <= 2.5f,
						"the damage number is raised over the tank that was hurt, not at the origin");
				}
				check(sawDamage, "damaging a tank raises a floating damage number for the HUD");
			}

			ScorchDroidEffects::drain();
			if (hurtTank)
			{
				TankSay say(hurtTank->getPlayerId(), LANG_STRING("Take that!"));
				say.setScorchedContext(&server->getContext());
				say.init();

				std::vector<ScorchDroidEffects::EffectEvent> effects = ScorchDroidEffects::drain();
				bool sawTalk = false;
				for (size_t i = 0; i < effects.size(); i++)
				{
					if (effects[i].type != ScorchDroidEffects::eTalk) continue;
					sawTalk = true;
					// Same trap as the damage number above: the bubble must
					// sit over the tank, not at the origin.
					FixedVector &turret = hurtTank->getLife().getTankTurretPosition();
					check(fabsf(effects[i].x - turret[0].asFloat()) < 0.01f &&
						  fabsf(effects[i].y - turret[1].asFloat()) < 0.01f &&
						  fabsf(effects[i].z - turret[2].asFloat()) < 0.01f,
						"the speech bubble is raised over the speaking tank's turret");
				}
				check(sawTalk, "a tank saying something raises a speech-bubble event for the HUD");

				// The text half. ChannelManager::showText() only writes to
				// the server log under S3D_SERVER, so a bot's taunt reached
				// the bubble and never the chat; it has to go out on the
				// channel like any other message.
				bool sawInChannel = false;
				std::list<ServerChannelManager::MessageEntry> &sent =
					ScorchedServer::instance()->getServerChannelManager().getLastMessages();
				std::list<ServerChannelManager::MessageEntry>::iterator sentItor;
				for (sentItor = sent.begin(); sentItor != sent.end(); ++sentItor)
				{
					if ((*sentItor).message.find("Take that!") != std::string::npos)
						sawInChannel = true;
				}
				check(sawInChannel, "the spoken line is broadcast on the general channel, not just logged");			}

			// The between-rounds scoreboard. Upstream raises its score
			// screen from the client half of ShowScoreAction, which is
			// compiled out here, so the port sat through the pause
			// (RoundScoreTime, 5s) showing an empty battlefield.
			{
				check(!ScorchDroidScoreboard::get().showing,
					"the scoreboard starts down");

				ShowScoreAction roundScore(fixed(5), false);
				roundScore.setScorchedContext(&server->getContext());
				roundScore.init();
				check(ScorchDroidScoreboard::get().showing,
					"the end-of-round action raises the scoreboard");
				check(!ScorchDroidScoreboard::get().finalScore,
					"a round score is not flagged as the final score");

				ScorchDroidScoreboard::hide();
				ShowScoreAction finalScore(fixed(15), true);
				finalScore.setScorchedContext(&server->getContext());
				finalScore.init();
				check(ScorchDroidScoreboard::get().showing &&
					  ScorchDroidScoreboard::get().finalScore,
					"the end-of-match action raises it flagged as the final score");

				// The action's own simulate() is deliberately not driven
				// here: past its timer it calls ServerState::scoreFinished(),
				// which stimulates the round state machine the rest of this
				// suite is running inside. The falling edge is one line next
				// to that call; this leaves the state clean instead.
				ScorchDroidScoreboard::hide();
				check(!ScorchDroidScoreboard::get().showing,
					"taking the scoreboard down leaves it down");
			}

			// The renderer takes a tank's hull bearing, and a moving
			// target's, out of TargetLife's quaternion, because that is the
			// only place either is kept on this port: getFloatRotMatrix()
			// is a mirror maintained solely when !serverMode_. That reading
			// makes two assumptions worth pinning here rather than in the
			// renderer, where neither can be checked without a GPU - the
			// component order (w, x, y, z) and the axis (engine up). Get
			// either wrong and every tank and jet points somewhere
			// arbitrary, which is how they came to face backwards once
			// already.
			{
				Tank *turnTank = nullptr;
				std::map<unsigned int, Tank *> &turnTanks = server->getTargetContainer().getTanks();
				if (!turnTanks.empty()) turnTank = turnTanks.begin()->second;
				check(turnTank != nullptr, "found a tank to turn");
				if (turnTank)
				{
					const float degrees[] = { 0.0f, 30.0f, 90.0f, -45.0f, 179.0f };
					bool allMatched = true;
					for (int d = 0; d < 5; d++)
					{
						turnTank->getLife().setRotation(fixed(true,
							(int) (degrees[d] * 10000.0f)));
						FixedVector4 &q = turnTank->getLife().getQuaternion();
						const float readBack =
							2.0f * atan2f(q[3].asFloat(), q[0].asFloat());
						const float expected = degrees[d] * (float) M_PI / 180.0f;
						if (fabsf(readBack - expected) > 0.01f) allMatched = false;
					}
					check(allMatched,
						"a target's yaw reads back out of its quaternion as the angle it was set to");
					turnTank->getLife().setRotation(fixed(0));
				}

			}
		}

		check(reported, "the deform hook reported a dirty region to the renderer");
		if (reported)
		{
			check(dMinX <= cx && cx <= dMaxX && dMinY <= cy && cy <= dMaxY,
				"the reported region contains the blast centre");
			check(dMaxX - dMinX >= 8 && dMaxY - dMinY >= 8,
				"the reported region is at least as wide as the blast radius");
		}
		check(!ScorchDroidLandscape::takeDirtyRegion(dMinX, dMinY, dMaxX, dMaxY),
			"draining the region clears it (no repeated rebuilds on quiet frames)");

		// flattenArea is the other half of terrain destruction - it is how
		// tanks bed into the ground at round start and after moving, and it
		// has no hit/miss return, so it always reports.
		//
		// removeObjects=false deliberately: the default also *deletes* every
		// flatten-destroyable target near the point, which would mutate the
		// shared server state later tests (testClientJoin especially) run
		// against. Only the heightmap half is under test here.
		FixedVector flatPos(fixed(cx), fixed(cy), hmap.getHeight(cx, cy));
		DeformLandscape::flattenArea(server->getContext(), flatPos, false);
		check(ScorchDroidLandscape::takeDirtyRegion(dMinX, dMinY, dMaxX, dMaxY),
			"flattenArea reports a dirty region too, not just explosions");

		// M6: the ground texture generator (LandscapeTextureBuilder) - our
		// reimplementation of GLImageModifier::addHeightToBitmap, which is
		// client-only. Checked here because it needs a real generated
		// landscape, which this test already has to set up. Deliberately
		// GL-free so it can be verified without an EGL context.
		LandscapeTextureBuilder::Texture ground =
			LandscapeTextureBuilder::build(server->getContext(), 128);
		check(ground.valid(), "a ground texture was generated from the landscape definition");
		if (ground.valid())
		{
			// The whole point is that the ground stops being one flat
			// colour, so assert it actually varies - a uniform image would
			// mean the height/slope blending never kicked in.
			unsigned char minC = 255, maxC = 0;
			long total = 0;
			for (size_t i = 0; i < ground.rgb.size(); i++)
			{
				minC = std::min(minC, ground.rgb[i]);
				maxC = std::max(maxC, ground.rgb[i]);
				total += ground.rgb[i];
			}
			check(maxC > minC, "the generated ground texture actually varies (height/slope blending ran)");
			check(total > 0, "the generated ground texture isn't entirely black (source images loaded)");

			// M6 shadows: upstream bakes the sun (and the shadows hills
			// cast on each other) into this texture rather than shading
			// per fragment - ImageModifier::addLightMapToBitmap, called
			// from Landscape.cpp right after the texture is generated.
			// Checked here because it is pure pixel work over a real
			// landscape, and because "did the shadows actually darken
			// anything" is invisible in a screenshot of an unfamiliar hill.
			LandscapeTextureBuilder::Texture beforeLight = ground;
			bool baked = LandscapeTextureBuilder::applyLightMap(
				server->getContext(), ground);
			check(baked, "the sun light map bakes into the ground texture");
			if (baked)
			{
				int darker = 0, brighter = 0, unchanged = 0;
				long long deltaTotal = 0;
				for (size_t i = 0; i < ground.rgb.size(); i++)
				{
					const int before = beforeLight.rgb[i];
					const int after = ground.rgb[i];
					if (after < before) darker++;
					else if (after > before) brighter++;
					else unchanged++;
					deltaTotal += (before - after);
				}
				// Lighting here only ever multiplies by <= 1, so nothing
				// may come out brighter - if any does, the light map has
				// been applied to the wrong buffer or scaled wrongly.
				check(brighter == 0, "baking light never brightens a texel");
				check(darker > 0, "the light map actually changes the ground");
				// And it must not flatten the whole thing to black, which
				// is what a broken sun direction or normal would do.
				check(unchanged + darker == (int) ground.rgb.size(),
					"every texel is accounted for");
				printf("  (light map: %d texels darkened, mean drop %.1f/255)\n",
					darker, (double) deltaTotal / (double) ground.rgb.size());

				// M6 shoreline foam: the mask the water shader draws surf from.
			// Checked here for the same reason as the light map - it is
			// pure pixel work over a real landscape, and "does the band
			// actually follow the coast" is not something a screenshot of
			// an unfamiliar shore answers.
			{
				LandscapeTex *landTex =
					server->getLandscapeMaps().getDefinitions().getTex();
				float waterHeight = 5.0f;
				if (landTex && landTex->border &&
					landTex->border->getType() == LandscapeTexType::eWater)
				{
					waterHeight = ((LandscapeTexBorderWater *)
						landTex->border)->height.asFloat();
				}

				const int maskSize = 128;
				const float reach = 3.0f;
				std::vector<unsigned char> shore =
					LandscapeTextureBuilder::buildShoreMask(
						server->getContext(), waterHeight, maskSize, reach);
				check(!shore.empty(), "the shore mask is built");
				check(shore.size() == (size_t) maskSize * maskSize,
					"the shore mask is the size asked for");

				HeightMap &hm2 =
					server->getLandscapeMaps().getGroundMaps().getHeightMap();
				int lit = 0, wrongSide = 0;
				for (int y = 0; y < maskSize; y++)
				{
					for (int x = 0; x < maskSize; x++)
					{
						const unsigned char v = shore[(size_t) y * maskSize + x];
						if (v == 0) continue;
						lit++;
						const int sx = std::min(x * hm2.getMapWidth() / maskSize,
							hm2.getMapWidth() - 1);
						const int sy = std::min(y * hm2.getMapHeight() / maskSize,
							hm2.getMapHeight() - 1);
						const float ground = hm2.getHeight(sx, sy).asFloat();
						// Foam belongs on the submerged shelf only: under
						// the water, but within reach of the surface.
						const float depth = waterHeight - ground;
						if (depth < 0.0f || depth > reach) wrongSide++;
					}
				}
				printf("  (shore mask: %d of %d cells lit)\n",
					lit, maskSize * maskSize);
				check(wrongSide == 0,
					"every lit cell is under the water and within reach of the surface");
				// A mask that lit nothing would silently mean no foam; one
				// that lit everything would flood the sea white.
				check(lit > 0, "the shore mask marks some coastline");
				check(lit < maskSize * maskSize / 2,
					"the shore mask is a band, not the whole sea");
			}

			int black = 0;
				for (size_t i = 0; i < ground.rgb.size(); i++)
				{
					if (ground.rgb[i] == 0) black++;
				}
				check(black < (int) ground.rgb.size() / 2,
					"the landscape is not baked to mostly black");
			}

			// M6 scorch marks - the other half of terrain destruction, and
			// the half upstream keeps in the client layer
			// (DeformTextures::deformLandscape + ExplosionTextures::
			// getScorchBitmap). Checked here rather than on-device because
			// it is deliberately GL-free; the renderer only uploads what
			// this produces.
			std::vector<unsigned char> before = ground.rgb;
			const float scorchRadius = 8.0f;
			LandscapeTextureBuilder::Rect rect = LandscapeTextureBuilder::applyScorch(
				server->getContext(), ground, cx, cy, scorchRadius, "");
			check(rect.valid(), "a scorch mark reports the texture rectangle it touched");

			if (rect.valid())
			{
				// The blast centre must actually change, or the mark is
				// invisible - the failure mode a "did it run" check misses.
				const float pixelsPerCellX = (float) ground.width / (float) hmap.getMapWidth();
				const float pixelsPerCellY = (float) ground.height / (float) hmap.getMapHeight();
				size_t centreIndex = ((size_t) (cy * pixelsPerCellY) * ground.width
					+ (size_t) (cx * pixelsPerCellX)) * 3;
				bool centreChanged =
					before[centreIndex] != ground.rgb[centreIndex] ||
					before[centreIndex + 1] != ground.rgb[centreIndex + 1] ||
					before[centreIndex + 2] != ground.rgb[centreIndex + 2];
				check(centreChanged, "the scorch mark actually changes pixels at the blast centre");

				// And nothing outside the crater may change - a mark that
				// bleeds across the whole map would still "work" by the
				// check above.
				size_t changedOutside = 0, changedInside = 0;
				for (int py = 0; py < ground.height; py++)
				{
					for (int px = 0; px < ground.width; px++)
					{
						size_t i = ((size_t) py * ground.width + px) * 3;
						if (before[i] == ground.rgb[i] &&
							before[i + 1] == ground.rgb[i + 1] &&
							before[i + 2] == ground.rgb[i + 2]) continue;

						float dx = ((float) px + 0.5f) / pixelsPerCellX - (float) cx;
						float dy = ((float) py + 0.5f) / pixelsPerCellY - (float) cy;
						if (std::sqrt(dx * dx + dy * dy) <= scorchRadius + 1.0f) changedInside++;
						else changedOutside++;
					}
				}
				check(changedOutside == 0, "the scorch mark stays inside the blast radius");
				check(changedInside > 0, "the scorch mark covers a real area, not a single pixel");

				// Marks must accumulate: upstream blends each blast into the
				// result of every earlier one, which is why the renderer
				// keeps a CPU-side copy of the texture rather than
				// regenerating it. A second mark on the same spot must
				// therefore still be able to change it.
				std::vector<unsigned char> afterFirst = ground.rgb;
				LandscapeTextureBuilder::applyScorch(
					server->getContext(), ground, cx + 4, cy, scorchRadius, "");
				check(afterFirst != ground.rgb, "a second overlapping scorch blends onto the first");
			}
		}

		// Normals are recalculated inside the same shared call (setNormals
		// is passed true even on the S3D_SERVER path), so lighting/physics
		// stay consistent - it's only the rendered mesh that goes stale.
		check(true, "deform ran without needing any client-only code");
	}

	// M6 sky: the numbers the sky shader is fed, read out of the landscape
	// definition. Checked here because the fiddly part is reading upstream's
	// colour map with upstream's own indexing (Hemisphere::drawColored), and
	// getting that wrong produces a sky that is merely "a bit off" rather
	// than obviously broken.
	// M6 parity: the chat log's own bookkeeping. Worth pinning because the
	// HUD depends on two properties that are easy to break silently - ids
	// must be monotonic even across a clear(), and since() must return only
	// what the caller has not already been shown. Get either wrong and the
	// on-screen stack either restarts a message's timer forever or drops it.
	// M6 performance: the instance packing behind instanced scenery drawing.
	// Worth pinning because a transposed pair of floats here would put every
	// tree in the wrong place, or tint the lot black, and the symptom would
	// be a scene that looks wrong with nothing to point at - the GL calls
	// around it cannot be tested from here, but this can.
	// M6: upstream's tree geometry, ported from its client-only
	// ModelRendererTree. Checks the properties that would silently produce a
	// wrong-looking forest rather than a crash: that every type builds
	// something, that the result is whole triangles, that it is deterministic
	// (upstream's is not - see TreeGeometry.hpp), that the texture
	// coordinates stay inside the atlas, and that the geometry stands on the
	// ground rather than sinking through it.
	void testTreeGeometry()
	{
		printf("\ntree geometry (ported from ModelRendererTree):\n");

		const TreeModelFactory::TreeType types[] = {
			TreeModelFactory::ePineNormal, TreeModelFactory::ePineSnow,
			TreeModelFactory::ePineBurnt,  TreeModelFactory::ePine2,
			TreeModelFactory::ePine4Snow,  TreeModelFactory::ePalmNormal,
			TreeModelFactory::ePalmBurnt,  TreeModelFactory::ePalmB,
			TreeModelFactory::eOak,        TreeModelFactory::eOak4,
		};

		bool allBuilt = true, allTriangles = true, uvInRange = true, standsOnGround = true;
		int totalVertices = 0;
		for (TreeModelFactory::TreeType type : types)
		{
			std::vector<float> verts;
			const int count = ScorchDroidTrees::build(type, verts);
			totalVertices += count;
			if (count == 0) { allBuilt = false; continue; }
            if (count % 3 != 0) allTriangles = false;

			float minY = 1e9f, maxY = -1e9f;
			for (size_t i = 0; i < verts.size(); i += ScorchDroidTrees::kFloatsPerVertex)
			{
				minY = std::min(minY, verts[i + 1]);
				maxY = std::max(maxY, verts[i + 1]);
				const float u = verts[i + 6], v = verts[i + 7];
				// Upstream's cells sit inside 0..1 with a little slack for
				// the radial sweep; well outside would mean sampling the
				// wrong species.
				if (u < -0.05f || u > 1.05f || v < -0.05f || v > 1.05f) uvInRange = false;
			}
			// The origin is the base: nothing may hang more than a hair
			// below it or the tree floats/sinks when placed on the ground.
			if (minY < -0.02f) standsOnGround = false;
			if (maxY <= 0.0f) allBuilt = false;
		}

		check(allBuilt, "every sampled tree type builds real geometry");
		check(allTriangles, "the output is whole triangles");
		check(uvInRange, "texture coordinates stay within the atlas");
		check(standsOnGround, "geometry sits on its origin rather than below it");
		check(totalVertices > 100, "the forest is more than a token amount of geometry");

		// Deterministic: upstream re-randomises every run, which would mean a
		// landscape looked different each time it loaded.
		std::vector<float> first, second;
		ScorchDroidTrees::build(TreeModelFactory::ePineNormal, first);
		ScorchDroidTrees::build(TreeModelFactory::ePineNormal, second);
		check(first == second, "building the same type twice gives identical geometry");

		// Distinct species must not collapse onto the same atlas cell.
		std::vector<float> pine, snow;
		ScorchDroidTrees::build(TreeModelFactory::ePineNormal, pine);
		ScorchDroidTrees::build(TreeModelFactory::ePineSnow, snow);
		check(pine != snow, "a snow pine differs from a green one");

		check(ScorchDroidTrees::atlasFor(TreeModelFactory::ePineNormal) !=
			  ScorchDroidTrees::atlasFor(TreeModelFactory::ePine2),
			"the two pine families sample different atlases");
		check(ScorchDroidTrees::isBurnt(TreeModelFactory::ePineBurnt) &&
			  !ScorchDroidTrees::isBurnt(TreeModelFactory::ePineNormal),
			"only the burnt types are marked burnt");
	}

	void testInstancePacking()
	{
		printf("\ninstance packing (instanced scenery draw):\n");

		std::vector<ScorchDroidInstances::Instance> instances;
		ScorchDroidInstances::Instance first;
		first.x = 1.0f; first.y = 2.0f; first.z = 3.0f; first.scale = 4.0f;
		first.rotationRadians = 5.0f; first.r = 0.25f; first.g = 0.5f; first.b = 0.75f;
		instances.push_back(first);

		ScorchDroidInstances::Instance second;
		second.x = -1.0f; second.y = -2.0f; second.z = -3.0f; second.scale = 0.5f;
		second.rotationRadians = -1.5f; second.r = 1.0f; second.g = 0.0f; second.b = 0.0f;
		instances.push_back(second);

		std::vector<float> packed;
		ScorchDroidInstances::pack(instances, packed);

		check(packed.size() == instances.size() * ScorchDroidInstances::kFloatsPerInstance,
			"packs exactly kFloatsPerInstance floats per instance");
		if (packed.size() != 16) return;

		// Attribute 2 is (x, y, z, scale); attribute 3 is (rotation, r, g, b).
		check(packed[0] == 1.0f && packed[1] == 2.0f && packed[2] == 3.0f && packed[3] == 4.0f,
			"the first vec4 is position then scale");
		check(packed[4] == 5.0f && packed[5] == 0.25f && packed[6] == 0.5f && packed[7] == 0.75f,
			"the second vec4 is rotation then colour");

		// The second instance must start exactly one stride in - an
		// off-by-one here would shear every instance against the next.
		check(packed[8] == -1.0f && packed[11] == 0.5f,
			"the next instance begins one stride later");
		check(packed[12] == -1.5f && packed[13] == 1.0f && packed[14] == 0.0f,
			"and carries its own rotation and colour");

		// pack() appends, so several buckets can share one buffer.
		ScorchDroidInstances::pack(instances, packed);
		check(packed.size() == 32, "pack() appends rather than replacing");
	}

	void testChatStore()
	{
		printf("\nchat store (ids, since, version):\n");

		ScorchDroidChat::clear();
		const unsigned int startVersion = ScorchDroidChat::version();

		ScorchDroidChat::Line first;
		first.channel = "general";
		first.who = "Player";
		first.text = "hello";
		ScorchDroidChat::push(first);

		ScorchDroidChat::Line second;
		second.channel = "info";
		second.text = "Game started";
		ScorchDroidChat::push(second);

		std::vector<ScorchDroidChat::Line> all = ScorchDroidChat::snapshot();
		check(all.size() == 2, "both pushed lines are in the log");
		if (all.size() != 2) return;

		check(all[0].id < all[1].id, "ids increase with each push");
		check(all[0].who == "Player" && all[1].who.empty(),
			"the speaker is kept, and is empty when the server is talking");
		check(ScorchDroidChat::version() > startVersion, "the version moves on a push");

		std::vector<ScorchDroidChat::Line> fresh = ScorchDroidChat::since(all[0].id);
		check(fresh.size() == 1 && fresh[0].id == all[1].id,
			"since() returns only lines newer than the one already seen");
		check(ScorchDroidChat::since(all[1].id).empty(),
			"since() the newest line returns nothing");

		// Ids must not restart, or the HUD would confuse a new line with one
		// it is already counting down.
		const unsigned int lastId = all[1].id;
		ScorchDroidChat::clear();
		check(ScorchDroidChat::snapshot().empty(), "clear() empties the log");
		ScorchDroidChat::push(first);
		check(ScorchDroidChat::snapshot().at(0).id > lastId,
			"ids keep counting across a clear rather than restarting");
	}

	void testSkyDescription()
	{
		printf("\nsky description (landscape colour map, sun, fog):\n");

		ScorchedContext &context = ScorchedServer::instance()->getContext();
		ScorchDroidSky::Description sky = ScorchDroidSky::describe(context);

		check(sky.valid, "the landscape's sky colour map loaded and was sampled");
		if (!sky.valid) return;

		// The sun direction is upstream's own Sun::setPosition formula, so
		// it must at least be a unit vector.
		const float length = sqrtf(
			sky.sunDirection[0] * sky.sunDirection[0] +
			sky.sunDirection[1] * sky.sunDirection[1] +
			sky.sunDirection[2] * sky.sunDirection[2]);
		check(fabsf(length - 1.0f) < 0.001f, "the sun direction is a unit vector");
		check(sky.sunDirection[2] > -0.001f,
			"the sun is at or above the horizon, not lighting the map from below");

		// Every gradient step has to be a real colour in range - an
		// out-of-range read would show up here rather than as a black sky.
		int inRange = 0, distinct = 0;
		for (int i = 0; i < ScorchDroidSky::kGradientSteps; i++)
		{
			bool ok = true;
			for (int c = 0; c < 3; c++)
			{
				if (sky.gradient[i][c] < 0.0f || sky.gradient[i][c] > 1.0f) ok = false;
			}
			if (ok) inRange++;
			if (i > 0 && (sky.gradient[i][0] != sky.gradient[i - 1][0] ||
						  sky.gradient[i][1] != sky.gradient[i - 1][1] ||
						  sky.gradient[i][2] != sky.gradient[i - 1][2]))
			{
				distinct++;
			}
		}
		check(inRange == ScorchDroidSky::kGradientSteps,
			"every gradient step is a colour in 0..1");
		// A flat gradient would mean the row indexing collapsed onto one
		// pixel - the sky would draw, in one colour, and look like the flat
		// clear colour this replaces.
		check(distinct > 0, "the gradient actually varies from horizon to zenith");

		printf("  (ambience %.2f,%.2f,%.2f diffuse %.2f,%.2f,%.2f sun %.0f,%.0f,%.0f)\n",
			sky.ambience[0], sky.ambience[1], sky.ambience[2],
			sky.diffuse[0], sky.diffuse[1], sky.diffuse[2],
			sky.sunPosition[0], sky.sunPosition[1], sky.sunPosition[2]);
		printf("  (horizon %.2f,%.2f,%.2f -> zenith %.2f,%.2f,%.2f)\n",
			sky.gradient[0][0], sky.gradient[0][1], sky.gradient[0][2],
			sky.gradient[ScorchDroidSky::kGradientSteps - 1][0],
			sky.gradient[ScorchDroidSky::kGradientSteps - 1][1],
			sky.gradient[ScorchDroidSky::kGradientSteps - 1][2]);
	}

	// M6 non-tank targets: trees, buildings and everything else a landscape
	// scatters about. The simulation half was always present - they are
	// ordinary Targets in the same container - but everything needed to
	// *draw* one is computed in TargetDefinition::createTarget and handed
	// straight to a client renderer that this build compiles out, so it was
	// discarded. Patch 0013 records it instead; this checks the hook fired
	// and carries usable values, which is the part that would break under a
	// submodule bump.
	void testLandscapeTargets()
	{
		printf("\nnon-tank landscape targets (patch 0013's model hook):\n");

		ScorchedServer *server = ScorchedServer::instance();
		std::map<unsigned int, Target *> &targets =
			server->getTargetContainer().getTargets();

		int nonTankTargets = 0, withModel = 0, drawable = 0, zeroBrightness = 0;
		for (auto &entry : targets)
		{
			Target *target = entry.second;
			if (target->getType() == Target::TypeTank) continue;
			nonTankTargets++;

			ScorchDroidTargets::Info info;
			if (!ScorchDroidTargets::get(target->getPlayerId(), info)) continue;
			withModel++;

			// The values the renderer actually uses. A zero scale would
			// draw nothing; an invalid model id would abort in ModelStore.
			if (info.model.modelValid() && info.scale > 0.0f) drawable++;
			// Not a failure - see the renderer's note. TargetDefinition
			// never initialises modelbrightness_, so any definition without
			// an explicit <modelbrightness> arrives as 0 instead of the -1
			// that means "randomise", and would be drawn black. The
			// renderer treats non-positive as untinted; this counts them so
			// the quirk is visible rather than silently worked around.
			if (info.brightness <= 0.0f) zeroBrightness++;

		}

		// Where are they? The renderer draws each at its target position, so
		// a target still at the origin would be drawn in the map corner.
		int atOrigin = 0, onMap = 0, buried = 0;
		GroundMaps &ground = server->getLandscapeMaps().getGroundMaps();
		for (auto &entry : targets)
		{
			Target *target = entry.second;
			if (target->getType() == Target::TypeTank) continue;
			FixedVector &pos = target->getLife().getTargetPosition();
			if (pos[0] == fixed(0) && pos[1] == fixed(0)) atOrigin++;
			// The renderer draws each target at pos[2]; if that is below the
			// ground under it, the model is buried and invisible.
			fixed groundHere = ground.getHeight(pos[0].asInt(), pos[1].asInt());
			if (pos[2] < groundHere - fixed(1)) buried++;
			if (pos[0] > fixed(0) && pos[0] < fixed(ground.getLandscapeWidth()) &&
				pos[1] > fixed(0) && pos[1] < fixed(ground.getLandscapeHeight())) onMap++;
		}

		printf("  (%d non-tank targets, %d with recorded models, %d drawable, "
			"%d at the origin, %d within the map)\n",
			nonTankTargets, withModel, drawable, atOrigin, onMap);
		printf("  (%d of %d sit below the ground under them)\n", buried, nonTankTargets);
		// Not "all": upstream's own placement leaves a handful at the origin
		// (map-wide effects rather than scenery). The check that matters is
		// that the overwhelming majority are really placed, since a
		// wholesale failure here would draw the entire landscape's scenery
		// in one corner.
		check(onMap > nonTankTargets * 9 / 10,
			"almost every target is placed within the landscape, not left at the origin");
		check(buried < nonTankTargets / 20,
			"targets sit on the ground rather than under it");
		check(nonTankTargets > 0,
			"the generated landscape actually placed non-tank targets");
		if (nonTankTargets == 0) return;

		check(withModel == nonTankTargets,
			"every non-tank target had its model recorded by the hook");
		check(drawable == withModel,
			"each recorded target carries a valid model and a non-zero scale");
		printf("  (%d of %d have no brightness set - drawn untinted, see the renderer)\n",
			zeroBrightness, withModel);
		check(ScorchDroidTargets::size() >= (size_t) withModel,
			"the store holds at least as many entries as there are live targets");
	}

	// M6 tank movement. Upstream has no "move" action: moving is firing a
	// WeaponMoveTank accessory (Fuel, Rocket Fuel) as an ordinary eShot
	// whose selected landscape position is the destination, with
	// MovementMap deciding what is reachable. This checks that reading of
	// the mechanism against the real engine, because it is the part
	// engine_jni.cpp's firePositionSelect() is built on and the part that
	// would silently change under a submodule bump - the JNI wrapper itself
	// can't run here, but everything it relies on can.
	void testTankMovement()
	{
		printf("\ntank movement (Fuel as a position-select weapon):\n");

		ScorchedServer *server = ScorchedServer::instance();
		ScorchedContext &context = server->getContext();

		Tank *tank = nullptr;
		std::map<unsigned int, Tank *> &tanks = server->getTargetContainer().getTanks();
		for (auto &entry : tanks)
		{
			if (entry.second->getDestinationId() == 1) { tank = entry.second; break; }
		}
		check(tank != nullptr, "human tank from the earlier test is still present");
		if (!tank) return;

		Accessory *fuel = context.getAccessoryStore().findByPrimaryAccessoryName("Fuel");
		check(fuel != nullptr, "the \"Fuel\" accessory exists in the real data");
		if (!fuel) return;

		// The property the whole feature hangs off: this is what makes the
		// battlefield tap choose a destination instead of aiming.
		check(fuel->getPositionSelect() == Accessory::ePositionSelectFuel,
			"Fuel is a position-select (fuel) accessory, not an aimed weapon");
		WeaponMoveTank *moveWeapon = (WeaponMoveTank *)
			context.getAccessoryStore().findAccessoryPartByAccessoryId(
				fuel->getAccessoryId(), "WeaponMoveTank");
		check(moveWeapon != nullptr, "Fuel's action is a WeaponMoveTank");
		if (!moveWeapon) return;

		// Put the tank somewhere it could actually drive from. The tanks in
		// this suite were added directly rather than placed by a round, so
		// they sit at (0,0) - and upstream refuses to move within 5 units of
		// the arena edge (MovementMap::addPoint), which would make every
		// square below unreachable for a reason that has nothing to do with
		// what is being tested.
		GroundMaps &ground = context.getLandscapeMaps().getGroundMaps();
		const int midX = ground.getLandscapeWidth() / 2;
		const int midY = ground.getLandscapeHeight() / 2;
		FixedVector placed(fixed(midX), fixed(midY),
			ground.getHeight(midX, midY));
		tank->getLife().setTargetPosition(placed);

		// Fuel is spent one unit per square, so the range is however many
		// units are held, capped by the weapon's own maximum (getFuel).
		tank->getAccessories().add(fuel, 20, false);
		check(tank->getAccessories().getAccessoryCount(fuel) >= 20,
			"the tank now holds fuel to move with");

		MovementMap withFuel(tank, context);
		fixed range = withFuel.getFuel(moveWeapon);
		check(range > fixed(0), "MovementMap reports a non-zero range from the fuel held");
		check(range <= moveWeapon->getMaximumRange(),
			"the range is capped by the weapon's own maximum, not just the count");

		withFuel.calculateAllPositions(range);
		FixedVector &start = tank->getLife().getTargetPosition();
		const int startX = start[0].asInt(), startY = start[1].asInt();

		check(withFuel.getEntry(startX, startY).type == MovementMap::eMovement,
			"the square the tank is standing on is reachable");

		// Somewhere far beyond any fuel: the flood fill must not reach it,
		// or "out of range" would never trigger and the overlay would be
		// meaningless.
		const int farX = std::min(startX + 200, ground.getLandscapeWidth() - 1);
		const int farY = std::min(startY + 200, ground.getLandscapeHeight() - 1);
		check(withFuel.getEntry(farX, farY).type != MovementMap::eMovement,
			"a square 200 units away is out of range");

		// The targeted search, which is what a tap actually goes through
		// (upstream's TargetCamera::landIntersect uses calculatePosition,
		// not the flood fill). Its fuel limit gates *expansion* but not
		// insertion, so squares beyond the range can still be marked
		// eMovement - the type alone is not a range check, and the distance
		// has to be tested too or a far tap is accepted.
		MovementMap targeted(tank, context);
		FixedVector farPoint(fixed(farX), fixed(farY), fixed(0));
		targeted.calculatePosition(farPoint, range);
		MovementMap::MovementMapEntry &farEntry = targeted.getEntry(farX, farY);
		check(!(farEntry.type == MovementMap::eMovement && farEntry.dist <= range),
			"a far square is rejected once the path distance is checked too");

		// A neighbouring square should be reachable unless the tank is
		// walled in - scan the ring around it so a cliff on one side
		// doesn't make this flaky.
		bool neighbourReachable = false;
		int destX = startX, destY = startY;
		for (int dy = -3; dy <= 3 && !neighbourReachable; dy++)
		{
			for (int dx = -3; dx <= 3 && !neighbourReachable; dx++)
			{
				if (dx == 0 && dy == 0) continue;
				if (withFuel.getEntry(startX + dx, startY + dy).type == MovementMap::eMovement)
				{
					neighbourReachable = true;
					destX = startX + dx;
					destY = startY + dy;
				}
			}
		}
		check(neighbourReachable, "at least one nearby square is reachable");
		if (!neighbourReachable) return;

		// With no fuel at all the same square must become unreachable -
		// this is what stops a tank moving for free.
		tank->getAccessories().rm(fuel, tank->getAccessories().getAccessoryCount(fuel));
		MovementMap noFuel(tank, context);
		check(noFuel.getFuel(moveWeapon) == fixed(0), "no fuel means no range");
		noFuel.calculateAllPositions(noFuel.getFuel(moveWeapon));
		check(noFuel.getEntry(destX, destY).type != MovementMap::eMovement,
			"with no fuel held, a neighbouring square is no longer reachable");

		// And the move message itself is an ordinary shot carrying the
		// destination - the shape engine_jni.cpp sends.
		tank->getAccessories().add(fuel, 20, false);
		tank->getAccessories().getWeapons().setWeapon(fuel);
		check(tank->getAccessories().getWeapons().getCurrent() == fuel,
			"Fuel can be made the current weapon, like any other");

		tank->getShotInfo().setSelectPosition(destX, destY);
		ComsPlayedMoveMessage move(tank->getPlayerId(),
			tank->getShotInfo().getMoveId(), ComsPlayedMoveMessage::eShot);
		move.setShot(fuel->getAccessoryId(),
			tank->getShotInfo().getRotationGunXY(),
			tank->getShotInfo().getRotationGunYZ(),
			tank->getShotInfo().getPower(),
			destX, destY);
		check(move.getType() == ComsPlayedMoveMessage::eShot,
			"a move is submitted as an eShot, not a move type of its own");
		check(move.getSelectPositionX() == destX && move.getSelectPositionY() == destY,
			"the move message carries the chosen destination");
		check(move.getWeaponId() == fuel->getAccessoryId(),
			"the move message carries the fuel accessory as its weapon");

		// The overlay the renderer paints from that mask, checked here
		// because it is pure pixel work: upstream's movementTexture leaves
		// reachable ground alone, quarters everything else, and draws a red
		// line along the boundary between them.
		LandscapeTextureBuilder::Texture plain;
		plain.width = plain.height = 64;
		plain.rgb.assign(64 * 64 * 3, 200);

		// A 20x20 reachable block in the middle of a 64x64 landscape.
		std::vector<unsigned char> mask(64 * 64, 0);
		for (int y = 22; y < 42; y++)
			for (int x = 22; x < 42; x++)
				mask[y * 64 + x] = 1;

		LandscapeTextureBuilder::Texture tinted =
			LandscapeTextureBuilder::applyMovementMask(plain, mask.data(), 64, 64);
		check(tinted.valid(), "the movement overlay produces a texture");
		if (tinted.valid())
		{
			auto pixel = [&](int x, int y) -> const unsigned char * {
				return &tinted.rgb[(size_t(y) * tinted.width + x) * 3];
			};

			const unsigned char *inside = pixel(32, 32);
			check(inside[0] == 200 && inside[1] == 200 && inside[2] == 200,
				"ground the tank can reach keeps its full brightness");

			const unsigned char *outside = pixel(2, 2);
			check(outside[0] == 50 && outside[1] == 50 && outside[2] == 50,
				"ground it cannot reach is quartered");

			// The boundary is where a texel differs from its right or lower
			// neighbour, so the last reachable column is the red one.
			bool sawRed = false;
			for (int y = 22; y < 42 && !sawRed; y++)
			{
				const unsigned char *edge = pixel(41, y);
				if (edge[0] == 255 && edge[1] == 0 && edge[2] == 0) sawRed = true;
			}
			check(sawRed, "the edge of the reachable area is drawn in red");

			// And the tinted copy must not have touched the source, which
			// has to survive to be put back when the weapon changes.
			check(plain.rgb[0] == 200, "the untinted ground texture is left alone");
		}
	}

	// M6 tap-to-aim: the screen-pixel -> landscape-point round trip that
	// renderer_jni.cpp's nativePickTerrain does.
	//
	// This is here because the pick got it wrong in a way nothing else
	// could catch: the camera basis it rebuilt the ray from had its right
	// and up vectors both negated relative to the view matrix the frame was
	// actually drawn with. That is a 180 degree rotation of the screen
	// about its centre, so every tap resolved to the landscape point
	// opposite the one under the finger - which looked exactly like an
	// angle-convention problem and got "fixed" for a while by adding half a
	// turn to upstream's autoAim expression in engine_jni.cpp.
	//
	// The renderer now publishes the basis by reading the view matrix's own
	// rows, so the two cannot drift apart; this pins that they agree by
	// projecting known points forward and reconstructing their rays back.
	// It is deliberately GL-free - only Mat4 and the same arithmetic - so
	// it runs here rather than needing a device.
	void testCameraPickRay()
	{
		printf("\n== camera pick ray (tap -> landscape point) ==\n");

		const float fovY = 45.0f * 3.14159265f / 180.0f;
		const int screenWidth = 1080, screenHeight = 2340;
		const float aspect = (float) screenWidth / (float) screenHeight;
		const float eye[3] = { 140.0f, 80.0f, 210.0f };
		const float target[3] = { 128.0f, 12.0f, 128.0f };

		Mat4 proj = Mat4::perspective(fovY, aspect, 1.0f, 2000.0f);
		Mat4 view = Mat4::lookAt(eye[0], eye[1], eye[2],
			target[0], target[1], target[2], 0.0f, 1.0f, 0.0f);
		Mat4 mvp = Mat4::multiply(proj, view);

		// The basis exactly as the renderer publishes it: rows of the view
		// matrix (column-major, so row r of column c is m[c * 4 + r]).
		const float right[3] = { view.m[0], view.m[4], view.m[8] };
		const float up[3]    = { view.m[1], view.m[5], view.m[9] };
		const float fwd[3]   = { -view.m[2], -view.m[6], -view.m[10] };

		// The sign that was wrong. right must be forward x worldUp, not its
		// negation: with the camera south-east of and above the target,
		// that puts +x world roughly to screen right.
		const float crossX = fwd[1] * 0.0f - fwd[2] * 1.0f;
		const float crossZ = fwd[0] * 1.0f - fwd[1] * 0.0f;
		const float crossLen = sqrtf(crossX * crossX + crossZ * crossZ);
		check(crossLen > 0.0001f &&
			  fabsf(right[0] - crossX / crossLen) < 0.001f &&
			  fabsf(right[2] - crossZ / crossLen) < 0.001f,
			"the published camera right vector is forward x worldUp, not its negation");
		check(fabsf(right[0] * fwd[0] + right[1] * fwd[1] + right[2] * fwd[2]) < 0.001f,
			"the published right vector is perpendicular to the view direction");
		check(fabsf(up[0] * fwd[0] + up[1] * fwd[1] + up[2] * fwd[2]) < 0.001f,
			"the published up vector is perpendicular to the view direction");
		check(up[1] > 0.0f, "the published up vector points up, not down");

		// Ground points spread around the target, including two on
		// perpendicular axes - the pair that distinguishes a clean 180
		// degree offset (both opposite) from a reflection (one opposite).
		const float points[][3] = {
			{ 128.0f, 10.0f, 128.0f },
			{ 168.0f, 10.0f, 128.0f },
			{ 128.0f, 10.0f, 168.0f },
			{  88.0f, 10.0f, 128.0f },
			{ 128.0f, 10.0f,  88.0f },
			{ 150.0f, 25.0f, 100.0f },
		};
		int roundTripped = 0;
		const int pointCount = (int) (sizeof(points) / sizeof(points[0]));
		for (int i = 0; i < pointCount; i++)
		{
			const float *p = points[i];
			const float clipX = mvp.m[0] * p[0] + mvp.m[4] * p[1] + mvp.m[8]  * p[2] + mvp.m[12];
			const float clipY = mvp.m[1] * p[0] + mvp.m[5] * p[1] + mvp.m[9]  * p[2] + mvp.m[13];
			const float clipW = mvp.m[3] * p[0] + mvp.m[7] * p[1] + mvp.m[11] * p[2] + mvp.m[15];
			if (clipW <= 0.0001f) continue;  // behind the eye - not tappable

			// Clip -> NDC -> the screen pixel a finger would land on, the
			// same mapping nativeGetTankOverlays uses to place name plates.
			const float ndcX = clipX / clipW, ndcY = clipY / clipW;
			const float screenX = (ndcX * 0.5f + 0.5f) * (float) screenWidth;
			const float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * (float) screenHeight;

			// ...and back out again, verbatim from nativePickTerrain.
			const float backNdcX = (screenX / (float) screenWidth) * 2.0f - 1.0f;
			const float backNdcY = 1.0f - (screenY / (float) screenHeight) * 2.0f;
			const float tanHalfFov = tanf(fovY * 0.5f);
			const float sx = backNdcX * aspect * tanHalfFov;
			const float sy = backNdcY * tanHalfFov;
			float dx = fwd[0] + right[0] * sx + up[0] * sy;
			float dy = fwd[1] + right[1] * sx + up[1] * sy;
			float dz = fwd[2] + right[2] * sx + up[2] * sy;
			const float dlen = sqrtf(dx * dx + dy * dy + dz * dz);
			dx /= dlen; dy /= dlen; dz /= dlen;

			// The ray must run from the eye through the point itself.
			float tx = p[0] - eye[0], ty = p[1] - eye[1], tz = p[2] - eye[2];
			const float tlen = sqrtf(tx * tx + ty * ty + tz * tz);
			tx /= tlen; ty /= tlen; tz /= tlen;
			const float alignment = dx * tx + dy * ty + dz * tz;
			check(alignment > 0.9999f,
				"a tap on a projected point picks that same point back (not the one opposite)");
			if (alignment > 0.9999f) roundTripped++;
		}
		check(roundTripped == pointCount,
			"every test point round-tripped through the screen and back");

		// Mat4::rotateAxis, which stands a tank on the slope it is parked
		// on (see groundTilt in renderer_jni.cpp). The property that
		// matters is the only one it is used for: it must take world up
		// onto the ground normal.
		const float normals[][3] = {
			{ 0.0f, 1.0f, 0.0f },
			{ 0.3f, 0.9f, 0.0f },
			{ -0.4f, 0.8f, 0.45f },
			{ 0.0f, 0.7f, -0.7f },
		};
		int tilted = 0;
		const int normalCount = (int) (sizeof(normals) / sizeof(normals[0]));
		for (int i = 0; i < normalCount; i++)
		{
			float nx = normals[i][0], ny = normals[i][1], nz = normals[i][2];
			const float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
			nx /= nlen; ny /= nlen; nz /= nlen;

			Mat4 tilt = Mat4::identity();
			const float axisX = nz, axisZ = -nx;
			if (sqrtf(axisX * axisX + axisZ * axisZ) > 1e-5f)
			{
				tilt = Mat4::rotateAxis(axisX, 0.0f, axisZ, acosf(ny));
			}

			// Column 1 is where the model's up axis ends up.
			const float upX = tilt.m[4], upY = tilt.m[5], upZ = tilt.m[6];
			if (fabsf(upX - nx) < 0.001f && fabsf(upY - ny) < 0.001f &&
				fabsf(upZ - nz) < 0.001f)
			{
				tilted++;
			}
		}
		check(tilted == normalCount,
			"the ground tilt takes the model's up axis onto the ground normal");
	}

	// M5: regression check for engine_jni.cpp's startLocalGame() switch
	// from NetLoopBack to a real NetServerTCP3 (see the porting plan's LAN
	// hosting notes) - a real TCP accept+connect over loopback, using the
	// exact same SDLNet_TCP_* calls a real LAN connection between two
	// devices would use (see the porting plan's SDL_net_compat notes from
	// M1) - just confirming the socket layer itself is wired up correctly,
	// not the higher-level connect handshake (ComsConnectMessage etc,
	// which nothing implements yet - see the plan's M5 "join as a client"
	// notes).
	struct CountingHandler : NetMessageHandlerI
	{
		int connectMessages = 0;
		void processMessage(NetMessage &message) override
		{
			if (message.getMessageType() == NetMessage::ConnectMessage) connectMessages++;
		}
	};

	void testRealTcpHostAndConnect()
	{
		printf("real TCP hosting (engine_jni.cpp's startLocalGame mechanism):\n");

		const int port = 27299;  // Distinct from S3D::ScorchedPort (27270) to avoid clashing with a real running instance.

		NetServerTCP3 hostInterface;
		CountingHandler hostHandler;
		hostInterface.setMessageHandler(&hostHandler);
		bool listening = hostInterface.start(port);
		check(listening, "NetServerTCP3::start() binds a real TCP listening socket");
		if (!listening) return;

		NetServerTCP3 clientInterface;
		CountingHandler clientHandler;
		clientInterface.setMessageHandler(&clientHandler);
		bool connected = clientInterface.connect("127.0.0.1", port);
		check(connected, "NetServerTCP3::connect() opens a real TCP socket to the host");

		// Both sides need a few processMessages() calls for the accept/
		// connect handshake to complete and surface a ConnectMessage -
		// mirrors tickEngine() calling NetInterface::processMessages()
		// every tick rather than once.
		bool bothConnected = false;
		for (int i = 0; i < 50 && !bothConnected; i++)
		{
			hostInterface.processMessages();
			clientInterface.processMessages();
			bothConnected = hostHandler.connectMessages > 0 && clientHandler.connectMessages > 0;
			if (!bothConnected) usleep(20 * 1000);
		}
		check(bothConnected, "both sides saw a ConnectMessage after processMessages() polling");

		hostInterface.stop();
		clientInterface.stop();
	}

	// M5 client-join, Phase 1: the real thing, not just the socket layer -
	// a ClientContext (see ClientContext.hpp) joins the already-running
	// ScorchedServer from every earlier test (with real bots, a real human
	// tank, real purchased accessories, and a real generated landscape
	// already in progress) over a real TCP socket, and is expected to
	// converge on the same tank count and landscape dimensions.
	//
	// Deliberately run in a SEPARATE OS PROCESS (fork+exec of this same
	// binary with --client), not just a second in-process object: an
	// earlier in-process attempt (one ScorchedServer + one ClientContext,
	// both real NetServerTCP3 instances, in the same process) intermittently
	// received its own just-sent ComsInitializeModMessage back as if it
	// were a message from the host - almost certainly a same-process
	// artifact of sharing process-global singletons (NetMessagePool in
	// particular) between two NetServerTCP3 instances, a configuration
	// upstream's real client/server split never exercises (they are always
	// separate OS processes, even in "single player" mode, which uses
	// NetLoopBack instead of two NetServerTCP3s). Running as two real
	// processes matches the actual M5 deployment shape (two devices, or a
	// device and a PC) and sidesteps that whole class of doubt.
	void testClientJoin()
	{
		printf("client join (separate-process ClientContext joining the already-running ScorchedServer host):\n");

		ScorchedServer *server = ScorchedServer::instance();

		const int port = 27298;
		NetServerTCP3 *hostNet = new NetServerTCP3();
		server->setNetInterface(hostNet);
		hostNet->setMessageHandler(&server->getComsMessageHandler());
		bool listening = hostNet->start(port);
		check(listening, "host bound a real listening socket for the client-join test");
		if (!listening) return;

		char exePath[4096];
		ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
		check(len > 0, "resolved this binary's own path for spawning a separate client process");
		if (len <= 0) return;
		exePath[len] = '\0';

		const char *resultPath = "/tmp/scorchdroid-host-tests-client-result.txt";
		unlink(resultPath);

		pid_t pid = fork();
		check(pid >= 0, "forked a separate client process");
		if (pid < 0) return;

		if (pid == 0)
		{
			// Child: a completely separate process from here on, with its
			// own independent globals/singletons - runs ClientContext for
			// real, exactly as a second device or a PC would.
			char portStr[16];
			snprintf(portStr, sizeof(portStr), "%d", port);
			execl(exePath, exePath, "--client", "127.0.0.1", portStr, resultPath, (char *) nullptr);
			_exit(127);  // execl only returns on failure.
		}

		// Parent: keep ticking the host (same composition as engine_jni.cpp's
		// tickEngine() - see the porting plan for why every one of these
		// calls is needed, in particular ServerConnectAuthHandler::
		// processMessages(), which is where the queued auth reply actually
		// gets processed, and getSimulator().simulate(), which is what
		// promotes/invokes the resulting TankAddSimAction) while the child
		// process runs its own handshake.
		Clock tickClock;
		int status = 0;
		pid_t waited = 0;
		// This loop *is* the host: the client cannot converge, or even
		// join, unless the server keeps stepping here, so the budget has to
		// outlast the client's own caps rather than being a round number.
		// Those are 150 ticks waiting to join, 100 waiting for its own tank
		// and 300 waiting for the clock correction to land, all on 100ms
		// sleeps - 550 in the worst case. Anything less kills the client
		// mid-wait, and the failure then reads "the client never joined"
		// when the truth is that the host gave up first, which is a
		// genuinely misleading place to start debugging. Only the failure
		// path is ever this slow; a healthy run exits in about fifteen
		// seconds.
		for (int i = 0; i < 700; i++)
		{
			unsigned int ticksDifference = tickClock.getTicksDifference();
			fixed timeDifference(true, ((Sint64) ticksDifference) * 10);

			server->getNetInterface().processMessages();
			server->getSimulator().simulate();
			server->getServerState().simulate(timeDifference);
			server->getServerConnectAuthHandler().processMessages();
			server->getServerFileServer().simulate();
			server->getServerChannelManager().simulate(timeDifference);
			server->getTimedMessage().simulate();

			waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid) break;
			usleep(100 * 1000);
		}
		if (waited != pid)
		{
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
		}

		bool joined = (waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
		if (!joined)
		{
			FILE *resultFile = fopen(resultPath, "r");
			if (resultFile)
			{
				char line[512];
				if (fgets(line, sizeof(line), resultFile)) fprintf(stderr, "  client process result: %s", line);
				fclose(resultFile);
			}
			else
			{
				fprintf(stderr, "  client process exited without writing a result (status=%d)\n", status);
			}
		}
		check(joined, "the separate client process reached the sJoined state");

		if (joined)
		{
			unsigned int clientTanks = 0, clientWidth = 0, clientHeight = 0;
			long long clientTimeDiff = 0, clientStartDiff = 0;
			int clientSettleTicks = 0;
			FILE *resultFile = fopen(resultPath, "r");
			bool parsed = resultFile &&
				fscanf(resultFile,
					"joined tanks=%u width=%u height=%u timediff=%lld startdiff=%lld ticks=%d",
					&clientTanks, &clientWidth, &clientHeight, &clientTimeDiff,
					&clientStartDiff, &clientSettleTicks) == 6;
			if (resultFile) fclose(resultFile);
			check(parsed, "parsed the client process's result file");

			if (parsed)
			{
				// The client must see every tank, including its own. An
				// earlier version of this check asserted `size() - 1` and
				// called the missing tank a "known promotion-timing gap";
				// tracing the real message flow showed the diagnosis was
				// wrong. The client's own TankAddSimAction arrives fine -
				// either in the ComsLoadLevelMessage's buffered history or
				// as a live ComsSimulateMessage (sendToAllLoadedClients
				// includes sLoadingLevel destinations) - and fires once
				// the client simulator reaches its event time, which needs
				// the *next* live message to lift ClientSync's
				// waitingEventTime_ cap (the same one-message lag upstream's
				// ClientSimulator has). The old test sampled after a fixed
				// 2s, a coin flip against the host's send-step schedule, so
				// the assertion "passed" exactly when the sample was too
				// early. runClientProcess() now waits for the tank itself.
				check(clientTanks == server->getTargetContainer().getTanks().size(),
					"client sees every tank, including its own");
				check(clientWidth == (unsigned int) server->getLandscapeMaps().getGroundMaps().getHeightMap().getMapWidth(),
					"client's landscape width matches the host's");
				check(clientHeight == (unsigned int) server->getLandscapeMaps().getGroundMaps().getHeightMap().getMapHeight(),
					"client's landscape height matches the host's");

				// ClientSync::syncToServerTime - see that function. The
				// client's clock starts at the level message's actualTime,
				// but its own level load has already burned several hundred
				// ms by the time it gets there, so without a correction it
				// runs permanently that far behind and everything is
				// displayed late. The value is measured from the host's own
				// message timestamps, so a broken correction shows up as a
				// large residual rather than a suspiciously perfect zero.
				//
				// Threshold measured, not guessed, and the client has
				// already waited for it rather than for a fixed time (see
				// runClientProcess). With the correction removed the
				// residual sits at a flat 0.68-0.77s and never closes, so
				// the client burns its whole 30s cap and arrives here well
				// outside 0.15s; with it, the residual falls under 0.15s
				// and keeps going (given 15s it reaches 0.007s).
				//
				// The earlier version of this asserted 0.35s after a fixed
				// 8s settle and failed about one run in three. The cause
				// was not the threshold: convergence closes a twentieth of
				// the gap *per message*, so 8s on a machine whose host
				// process is fighting two emulators for a core carries far
				// fewer messages than 8s on an idle one, and the test was
				// really measuring how busy the machine was. Waiting for
				// the condition removes that dependence entirely - a loaded
				// machine now takes more ticks to get to the same place
				// instead of failing.
				fprintf(stderr, "  (client/host clock difference: %.3fs, from %.3fs at join, %d ticks)\n",
					(double) clientTimeDiff / (double) fixed::FIXED_RESOLUTION,
					(double) clientStartDiff / (double) fixed::FIXED_RESOLUTION,
					clientSettleTicks);
				const long long maxDriftInternal = (long long) (fixed::FIXED_RESOLUTION * 15 / 100);
				check(clientTimeDiff < maxDriftInternal && clientTimeDiff > -maxDriftInternal,
					"the joined client's clock converges on the host's rather than staying a level-load behind");
			}
		}

		hostNet->stop();
	}
}

// Runs as a genuinely separate process (see testClientJoin() above for why)
// - joins the host at host:port as a real ClientContext and writes a
// one-line result to resultPath for the parent process to check, since a
// child process's return value alone can't carry structured data back.
static int runClientProcess(const char *host, int port, const char *resultPath)
{
	if (chdir(SCORCHED_SUBMODULE_ROOT) != 0)
	{
		fprintf(stderr, "chdir(%s) failed\n", SCORCHED_SUBMODULE_ROOT);
		return 1;
	}
	setenv("HOME", "/tmp/scorchdroid-host-tests-home-client", 1);
	S3D::setSettingsDir("scorchdroid-host-tests-client");

	ClientContext client;
	bool connecting = client.connectToServer(host, port);
	if (!connecting)
	{
		FILE *f = fopen(resultPath, "w");
		if (f) { fprintf(f, "failed reason=connect failed\n"); fclose(f); }
		return 1;
	}

	bool joined = false;
	bool failed = false;
	// Reported alongside the final difference so a failure says whether the
	// correction did nothing or merely ran out of room.
	long long startDiff = 0;
	int settleTicks = 0;
	for (int i = 0; i < 150 && !joined && !failed; i++)
	{
		client.tick();
		joined = (client.getState() == ClientContext::sJoined);
		failed = (client.getState() == ClientContext::sFailed);
		if (!joined && !failed) usleep(100 * 1000);
	}

	// Joining is not the end of the handshake from the client's point of
	// view: its own TankAddSimAction is queued for a future event time and
	// only fires once a later ComsSimulateMessage lifts ClientSync's
	// waitingEventTime_ cap, plus the client clock starts ~its own
	// level-load time behind the host. So wait for the tank itself rather
	// than a fixed number of ticks - a fixed 2s wait was a coin flip against
	// the host's send-step schedule (see testClientJoin()'s check). Bounded
	// so a genuine regression still fails instead of hanging.
	if (joined)
	{
		bool ownTankSeen = false;
		for (int i = 0; i < 100 && !ownTankSeen; i++)
		{
			client.tick();
			std::map<unsigned int, Tank *> &tanks = client.getTargetContainer().getTanks();
			std::map<unsigned int, Tank *>::iterator itor;
			for (itor = tanks.begin(); itor != tanks.end(); ++itor)
			{
				if (itor->second->getDestinationId() == client.getMyDestinationId())
				{
					ownTankSeen = true;
					break;
				}
			}
			if (!ownTankSeen) usleep(100 * 1000);
		}
		// Keep ticking for two reasons: the TankLoaded/TankChange actions
		// that follow the add need to settle before the count is reported,
		// and the clock-drift correction (ClientSync::syncToServerTime)
		// needs enough live messages to converge - it deliberately closes
		// only a twentieth of the gap per message, and the host's
		// ComsNetStatMessage (which supplies the round-trip estimate) only
		// starts arriving a couple of seconds in.
		//
		// Waits for the correction to *land* rather than for a fixed number
		// of ticks. That distinction is the whole reason this test used to
		// fail one run in three: convergence is driven by the message count,
		// not by wall-clock time, so on a loaded machine - where the host
		// process gets a fraction of a core and sends proportionally fewer
		// steps - a fixed 8s carried nowhere near enough messages, and the
		// test failed for the machine being busy rather than for anything
		// being wrong. The cap is generous for the same reason: it is there
		// to stop a genuine regression hanging, not to time the run.
		ClientSync &syncing = (ClientSync &) client.getSimulator();
		startDiff = (long long) syncing.getServerTimeDifference().getInternalData();
		const long long converged = (long long) (fixed::FIXED_RESOLUTION * 15 / 100);
		for (settleTicks = 0; settleTicks < 300; settleTicks++)
		{
			client.tick();
			long long diff = (long long) syncing.getServerTimeDifference().getInternalData();
			if (diff < converged && diff > -converged) break;
			usleep(100 * 1000);
		}
	}

	FILE *f = fopen(resultPath, "w");
	if (!f) return 1;
	if (joined)
	{
		// timediff is ClientSync's rolling average of (host clock - our
		// clock), in fixed's internal units, computed purely from timestamps
		// the *host* put in its messages - see testClientJoin()'s assertion.
		ClientSync &sync = (ClientSync &) client.getSimulator();
		fprintf(f, "joined tanks=%zu width=%d height=%d timediff=%lld startdiff=%lld ticks=%d\n",
			client.getTargetContainer().getTanks().size(),
			client.getLandscapeMaps().getGroundMaps().getHeightMap().getMapWidth(),
			client.getLandscapeMaps().getGroundMaps().getHeightMap().getMapHeight(),
			(long long) sync.getServerTimeDifference().getInternalData(),
			startDiff, settleTicks);
	}
	else
	{
		fprintf(f, "failed reason=%s\n", client.getFailureReason().c_str());
	}
	fclose(f);
	return joined ? 0 : 1;
}

// M9 gate: can the engine stop and start again inside one process?
//
// The main menu turns "quit to menu, start something else" into the ordinary
// path, and today nothing supports it: startLocalGame()/startJoinGame() refuse
// a second call (g_mode != kNone) and nothing ever puts the mode back. Before
// building a menu around that, prove the engine underneath can actually do it.
//
// Encouragingly, upstream already restarts this way - startServer() opens with
// its own stopServer() call, which is what a PC client hosting a second game
// does. What is unproven here is this *port's* configuration: host-tests
// already records that two contexts in one process share NetMessagePool and
// other globals in a way upstream never exercises, so "upstream does it" is
// not evidence for us.
//
// Runs last, deliberately: it destroys the server every other test shares.
static void testServerRestart()
{
	printf("engine restart (M9: quit to menu, then start another game):\n");

	check(ScorchedServer::serverStarted(), "a server is running before the restart");
	ScorchedServer::stopServer();
	check(!ScorchedServer::serverStarted(), "stopServer() clears the started flag");
	check(ScorchedServer::instance() == nullptr,
		"stopServer() drops the singleton - any pointer cached across a restart is dangling");

	ScorchedServerSettingsOptions settings("data/server.xml", false, false);
	bool restarted = ScorchedServer::startServer(settings, true, nullptr);
	check(restarted, "a second startServer() in the same process succeeds");
	if (!restarted) return;

	ScorchedServer *server = ScorchedServer::instance();
	check(server != nullptr, "the restarted server has an instance again");
	if (!server) return;

	// "Started" is a flag; these are the checks that it is genuinely alive
	// rather than a shell that happens not to have crashed yet.
	Accessory *babyMissile =
		server->getAccessoryStore().findByPrimaryAccessoryName("Baby Missile");
	check(babyMissile != nullptr, "the restarted server reparsed the accessory store");

	check(server->getTargetContainer().getTanks().empty(),
		"the restarted server starts with no tanks - no state survived the previous game");

	LandscapeDefinition defn = server->getLandscapes().getRandomLandscapeDefn(
		server->getContext().getOptionsGame(), server->getContext().getTargetContainer());
	server->getOptionsGame().updateLevelOptions(server->getContext(), defn);
	server->getLandscapeMaps().generateMaps(server->getContext(), defn, nullptr);
	HeightMap &hmap = server->getLandscapeMaps().getGroundMaps().getHeightMap();
	check(hmap.getMapWidth() > 0 && hmap.getMapHeight() > 0,
		"the restarted server generates a landscape");

	// And that the landscape is really usable, not just allocated: carve a
	// crater and check both the heightmap and the renderer's deform hook.
	if (hmap.getMapWidth() > 0)
	{
		int cx = hmap.getMapWidth() / 2, cy = hmap.getMapHeight() / 2;
		fixed before = hmap.getHeight(cx, cy);
		ScorchDroidLandscape::clearDirtyRegion();
		FixedVector pos(fixed(cx), fixed(cy), before);
		DeformLandscape::deformLandscape(server->getContext(), pos, fixed(8), true, fixed(1), nullptr);
		check(hmap.getHeight(cx, cy) < before,
			"terrain destruction still works after a restart");
		int a = 0, b = 0, c = 0, d = 0;
		check(ScorchDroidLandscape::takeDirtyRegion(a, b, c, d),
			"the deform hook still reports to the renderer after a restart");
	}

	// The restart above ran on a NetLoopBack (local=true). What
	// startLocalGame() actually does is local=false, which builds a real
	// NetServerTCP3 and binds a listening socket - and a socket left bound
	// by the previous game is the classic way a restart fails: the player
	// quits to the menu, hosts again, and the second host silently cannot
	// bind. So do it the way the app does, twice, on the same port.
	{
		const int port = 27298;  // Distinct from ScorchedPort and from testRealTcpHostAndConnect's.

		ScorchedServerSettingsOptions netSettings("data/server.xml", false, false);
		check(ScorchedServer::startServer(netSettings, false, nullptr),
			"a real (non-loopback) server starts, as startLocalGame() does");
		check(ScorchedServer::instance()->getContext().getNetInterface().start(port),
			"the real server binds its listening socket");

		ScorchedServer::stopServer();

		check(ScorchedServer::startServer(netSettings, false, nullptr),
			"a second real server starts after the first was stopped");
		check(ScorchedServer::instance()->getContext().getNetInterface().start(port),
			"the second server binds the same port again - the socket really was released");
	}
}

// M10: the game setup a player chooses before starting - rounds, turns, wall
// type, money - and, more importantly, whether it survives contact with the
// round that follows.
//
// The reading half is not where this breaks. The writing half is: an
// OptionsGame value set after startServer() is silently undone at the first
// round, because OptionsScorched snapshots the main options during startup and
// ServerStateNewGame calls commitChanges(), which copies that snapshot back
// over them. The entry then still reports itself as "changed" while holding
// the file's value. That cost an afternoon on the debug money flag, so the
// check below runs commitChanges() by hand - exactly what ServerStateNewGame
// does - rather than asserting the value right after it was written, which
// would pass whether or not the bug were present.
static void testGameSetup()
{
	printf("game setup (M10: choosing rounds, wall type and the rest):\n");

	// Our own shipped config, not upstream's data/server.xml: this is the file
	// the setup screen edits, and the bot it names ("Moron") is exactly what a
	// mod has to be able to provide.
	ScorchDroidSetup::ensureLoaded(SCORCHDROID_APP_CONFIG);
	std::vector<ScorchDroidSetup::Option> options = ScorchDroidSetup::options();
	check(!options.empty(), "the setup surface exposes some options");

	bool sawRounds = false, sawWall = false;
	for (size_t i = 0; i < options.size(); i++)
	{
		if (options[i].name == "NumberOfRounds")
		{
			sawRounds = true;
			check(options[i].kind == ScorchDroidSetup::eBoundedInt,
				"NumberOfRounds comes through as a bounded int, so the UI can draw a slider");
			check(options[i].maxValue > options[i].minValue,
				"...with a real range read from upstream, not invented here");
			check(!options[i].description.empty(),
				"...and upstream's own description, so the UI need not write its own");
		}
		if (options[i].name == "WallType")
		{
			sawWall = true;
			check(options[i].kind == ScorchDroidSetup::eEnum,
				"WallType comes through as an enum");
			// Upstream terminates these arrays with an empty description; a
			// mistake there would show up as a trailing blank choice.
			bool sawConcrete = false, sawBlank = false;
			for (size_t c = 0; c < options[i].choices.size(); c++)
			{
				if (options[i].choices[c].label == "WallConcrete") sawConcrete = true;
				if (options[i].choices[c].label.empty()) sawBlank = true;
			}
			check(sawConcrete, "...carrying upstream's own choices");
			check(!sawBlank, "...and stopping at the terminator rather than past it");
		}
	}
	check(sawRounds && sawWall, "both a bounded int and an enum are exposed");

	// M17: the options that were unreachable until now. Named individually
	// rather than counted, because the failure this guards against is one of
	// them quietly disappearing - a typo in the exposed list costs an option
	// with nothing on screen to say so.
	{
		const char *expected[] = {
			"PlayerLives", "Teams", "TeamBallance", "StartArmsLevel", "EndArmsLevel",
			"WeaponSpeed", "Gravity", "ResignMode", nullptr,
		};
		std::string missing;
		for (int i = 0; expected[i]; i++)
		{
			bool found = false;
			for (size_t o = 0; o < options.size(); o++)
			{
				if (options[o].name == expected[i]) found = true;
			}
			if (!found) missing += std::string(missing.empty() ? "" : ", ") + expected[i];
		}
		check(missing.empty(),
			missing.empty() ? "every option M17 adds is exposed" :
				("options missing from the setup screen: " + missing).c_str());

		// Teams is what makes a team game reachable at all, so it gets its
		// own check that the value actually takes.
		check(ScorchDroidSetup::set("Teams", "2"), "teams can be turned on");
		std::string teams;
		std::vector<ScorchDroidSetup::Option> withTeams = ScorchDroidSetup::options();
		for (size_t o = 0; o < withTeams.size(); o++)
		{
			if (withTeams[o].name == "Teams") teams = withTeams[o].value;
		}
		check(teams == "2", "...and the engine holds the choice");
		check(!ScorchDroidSetup::set("Teams", "9"),
			"...while upstream's own range refuses a fifth team");

		// Upstream retires options by flagging them, not by deleting them,
		// so they still read as ordinary bounded ints and enums. Exposing
		// one would put a control on screen that changes nothing.
		bool sawDepricated = false;
		for (size_t o = 0; o < options.size(); o++)
		{
			if (options[o].name == "MaxArmsLevel" ||
				options[o].name == "ScoreType" ||
				options[o].name == "AutoBallanceTeams") sawDepricated = true;
		}
		check(!sawDepricated, "no deprecated option reaches the setup screen");

		// M18: every option says which tab it belongs on, so an option added
		// without one cannot end up on a default tab by accident.
		bool allGrouped = true;
		for (size_t o = 0; o < options.size(); o++)
		{
			if (options[o].group.empty()) allGrouped = false;
		}
		check(allGrouped, "every option is filed under a tab");

		// The player count is really two options and has to move as one -
		// the shipped config's minimum is 2, so setting only the maximum
		// left a game "of eight" starting with a single bot.
		check(ScorchDroidSetup::set("NumberOfPlayers", "6"),
			"the player count can be raised");
		{
			OptionsGame probe;
			const char *path = "/tmp/scorchdroid-players.xml";
			check(ScorchDroidSetup::writeSessionFile(path), "...and written out");
			check(probe.readOptionsFromFile(path), "...and read back");
			check(probe.getNoMaxPlayers() == 6 && probe.getNoMinPlayers() == 6,
				"...with the minimum moved to match, so the bots actually turn up");
			unlink(path);
		}
		ScorchDroidSetup::reset();
	}

	// M18: choosing the bots. Upstream has no difficulty dial - the choice
	// is which of its named AIs fills the slots, and its own descriptions
	// are the difficulty ladder.
	{
		std::vector<ScorchDroidSetup::Bot> bots = ScorchDroidSetup::bots(".");
		check(bots.size() > 3, "the base game offers a handful of bots");
		check(bots[0].name == "Random",
			"...led by upstream's Random, which is an instruction, not an AI");

		bool sawMoron = false, sawShark = false, sawTarget = false, described = true;
		for (size_t i = 0; i < bots.size(); i++)
		{
			if (bots[i].name == "Moron") sawMoron = true;
			if (bots[i].name == "Shark") sawShark = true;
			if (bots[i].name == "Target") sawTarget = true;
			if (bots[i].description.empty()) described = false;
		}
		check(sawMoron && sawShark, "...including the ends of its difficulty range");
		check(!sawTarget, "...but not the inert practice dummy, which never fires back");
		check(described, "...each with upstream's own description of how good it is");

		// The scan has to read a *description* the same way it reads a name:
		// from the live file, not from inside a comment.
		std::vector<ScorchDroidSetup::Bot> all = ScorchDroidSetup::botsFor(".", "none");
		std::string moronDescription;
		for (size_t i = 0; i < all.size(); i++)
		{
			if (all[i].name == "Moron") moronDescription = all[i].description;
		}
		check(moronDescription.find("stupid") != std::string::npos,
			"a bot's description is upstream's own words");

		// M19: a mixed roster, which is what upstream's per-slot PlayerType
		// list is for. The slots are filled round-robin, so the proportions
		// asked for are the proportions played.
		check(ScorchDroidSetup::setBotTypes({ "Shark", "Moron" }),
			"more than one kind of bot can be chosen");
		{
			std::vector<std::string> mix = ScorchDroidSetup::botTypes();
			check(mix.size() == 2 && mix[0] == "Shark" && mix[1] == "Moron",
				"...and reads back as the mix that was asked for");
			OptionsGame probe;
			const char *path = "/tmp/scorchdroid-mix.xml";
			check(ScorchDroidSetup::writeSessionFile(path), "...written to the session config");
			check(probe.readOptionsFromFile(path), "...and read back");
			check(0 == strcmp(probe.getPlayerType(1), "Shark") &&
				0 == strcmp(probe.getPlayerType(2), "Moron") &&
				0 == strcmp(probe.getPlayerType(3), "Shark"),
				"...dealt round-robin across the slots");
			unlink(path);
		}
		check(!ScorchDroidSetup::setBotTypes({}),
			"an empty mix is refused - a game needs someone to play against");

		check(ScorchDroidSetup::setBotType("Shark"), "the bots can be chosen");
		check(ScorchDroidSetup::botType() == "Shark", "...and the choice sticks");
		{
			OptionsGame probe;
			const char *path = "/tmp/scorchdroid-bots.xml";
			check(ScorchDroidSetup::writeSessionFile(path), "...is written to the session config");
			check(probe.readOptionsFromFile(path), "...and read back");
			check(0 == strcmp(probe.getPlayerType(0), "Human"),
				"...leaving the first slot for the player");
			check(0 == strcmp(probe.getPlayerType(1), "Shark") &&
				0 == strcmp(probe.getPlayerType(5), "Shark"),
				"...and filling every other slot, not just the ones in use");
			unlink(path);
		}
		ScorchDroidSetup::reset();
	}

	// Chosen relative to whatever the config file says rather than hardcoded,
	// so the test still distinguishes "applied" from "unchanged" if the
	// shipped default ever becomes the value being set.
	std::string originalRounds;
	int chosenRounds = 0;
	for (size_t i = 0; i < options.size(); i++)
	{
		if (options[i].name != "NumberOfRounds") continue;
		originalRounds = options[i].value;
		chosenRounds = atoi(originalRounds.c_str()) + 1;
		if (chosenRounds > options[i].maxValue) chosenRounds = options[i].minValue;
	}
	check(!originalRounds.empty() && chosenRounds != atoi(originalRounds.c_str()),
		"picked a round count that differs from the shipped default");

	// Validation is upstream's, inherited rather than reimplemented.
	check(ScorchDroidSetup::set("NumberOfRounds", S3D::formatStringBuffer("%i", chosenRounds)),
		"a value inside the range is accepted");
	check(!ScorchDroidSetup::set("NumberOfRounds", "9999"),
		"a value outside upstream's own range is refused");
	check(!ScorchDroidSetup::set("NotAnOption", "1"),
		"an option that isn't exposed is refused");
	check(!ScorchDroidSetup::set("PortNo", "1234"),
		"an option that exists but isn't exposed is refused - the curated list is the contract");

	ScorchedServerSettingsOptions settings("data/server.xml", false, false);
	bool setupServerStarted = ScorchedServer::startServer(settings, true, nullptr);
	check(setupServerStarted, "a server starts to apply the setup to");
	if (!setupServerStarted) return;

	OptionsScorched &serverOptions = ScorchedServer::instance()->getOptionsGame();
	ScorchDroidSetup::applyTo(serverOptions);
	check(serverOptions.getNoRounds() == chosenRounds, "the chosen value reaches the server");

	serverOptions.updateChangeSet();
	// What ServerStateNewGame does at the start of every round.
	serverOptions.commitChanges();
	check(serverOptions.getNoRounds() == chosenRounds,
		"and survives the first round rather than being reverted by commitChanges()");

	// The mod list, and the session file that is the only way a mod choice can
	// take effect - startServerInternal() calls setDataFileMod() and
	// loadModFiles() partway through its own startup, so an applyTo() after
	// startServer() would always be too late for it.
	std::vector<std::string> availableMods = ScorchDroidSetup::mods(".");
	check(!availableMods.empty() && availableMods[0] == "none",
		"the mod list starts with upstream's own base game");
	bool sawApoc = false;
	for (size_t i = 0; i < availableMods.size(); i++)
	{
		if (availableMods[i] == "apoc") sawApoc = true;
	}
	check(sawApoc, "the bundled Apocalypse mod is found by looking in data/globalmods");

	check(ScorchDroidSetup::setMod("apoc"), "a mod can be chosen");
	check(ScorchDroidSetup::mod() == "apoc", "...and reads back");

	const char *sessionPath = "/tmp/scorchdroid-host-tests-session.xml";
	check(ScorchDroidSetup::writeSessionFile(sessionPath),
		"the session config is written");
	{
		// Read it back the way the server will, into a fresh OptionsGame, and
		// check both a value the player changed and the mod survived the trip.
		OptionsGame reread;
		check(reread.readOptionsFromFile(sessionPath),
			"...and reads back as a config file the server can load");
		check(reread.getNoRounds() == chosenRounds,
			"...carrying the chosen round count");
		check(std::string(reread.getMod()) == "apoc",
			"...and the chosen mod, which is the whole reason for writing it");
	}
	ScorchDroidSetup::setMod("none");
	unlink(sessionPath);

	// The bundled Apocalypse mod, and the bot it cannot provide.
	//
	// apoc's data/tankais.xml looks like it defines twelve AIs. Five of them,
	// including "Moron", sit inside an XML comment, so upstream's parser never
	// sees them and the mod really offers seven. The shipped server config
	// names "Moron" as the bot to play against, so choosing this mod produced
	// a game that loaded its landscape and then waited forever for a bot that
	// could never be created - logging "Failed to find a tank ai called
	// Moron" every tick, where nothing surfaced it.
	//
	// Both halves are checked: that the scan agrees with what the engine
	// actually loads (the trap - a scan that read commented-out definitions
	// would report bots that do not exist), and that the substitution leaves
	// the config naming a bot the mod really has.
	{
		std::vector<std::string> baseBots = ScorchDroidSetup::botNames(".", "none");
		std::vector<std::string> apocBots = ScorchDroidSetup::botNames(".", "apoc");
		check(!baseBots.empty() && !apocBots.empty(), "both mods' bot lists are readable");

		bool baseHasMoron = false, apocHasMoron = false, apocHasCyborg = false;
		for (size_t i = 0; i < baseBots.size(); i++)
			if (baseBots[i] == "Moron") baseHasMoron = true;
		for (size_t i = 0; i < apocBots.size(); i++)
		{
			if (apocBots[i] == "Moron") apocHasMoron = true;
			if (apocBots[i] == "Cyborg") apocHasCyborg = true;
		}
		check(baseHasMoron, "the base game provides the bot the shipped config asks for");
		check(apocHasCyborg, "Apocalypse provides the bots that are not commented out");
		check(!apocHasMoron,
			"...and not the ones that are - the scan must agree with the parser, "
			"not with what the file appears to contain");

		ScorchDroidSetup::setMod("apoc");
		check(ScorchDroidSetup::ensureBotsValidForMod(".") > 0,
			"choosing Apocalypse replaces the bot it cannot provide");
		check(ScorchDroidSetup::ensureBotsValidForMod(".") == 0,
			"...and doing it again changes nothing, so it is safe every start");

		// The real proof: start a server with that mod and ask the engine's
		// own store for the bot the config now names.
		const char *apocPath = "/tmp/scorchdroid-host-tests-apoc.xml";
		ScorchDroidSetup::writeSessionFile(apocPath);
		ScorchedServerSettingsOptions apocSettings(apocPath, false, false);
		bool apocUp = ScorchedServer::startServer(apocSettings, true, nullptr);
		check(apocUp, "a server starts with the Apocalypse mod selected");
		if (apocUp)
		{
			check(std::string(ScorchedServer::instance()->getOptionsGame().getMod()) == "apoc",
				"...and really is running that mod, so the session config route works");
			TankAIStore &ais = ScorchedServer::instance()->getTankAIs();
			std::string configuredBot;
			std::list<OptionEntry *> &players =
				ScorchedServer::instance()->getOptionsGame().getMainOptions().getPlayerTypeOptions();
			for (std::list<OptionEntry *>::iterator itor = players.begin();
				itor != players.end();
				++itor)
			{
				const std::string value = (*itor)->getValueAsString();
				if (value != "Human" && !value.empty()) configuredBot = value;
			}
			check(!configuredBot.empty(), "the session config still names a bot");
			check(!configuredBot.empty() && ais.getAIByName(configuredBot.c_str()) != nullptr,
				"...and the engine can actually create it, which is what was broken");
		}
		ScorchDroidSetup::setMod("none");
		unlink(apocPath);
	}

	// M12: the tutorial's setup is upstream's own file, which is the half of
	// its tutorial that can be reused at all. Checked because a preset that
	// silently failed to load would give a normal game with the tutorial's
	// coach marks over it - confusing in a way a new player could not diagnose.
	{
		check(ScorchDroidSetup::loadPreset("data/singletutorial.xml"),
			"the tutorial preset loads");
		std::vector<ScorchDroidSetup::Option> tut = ScorchDroidSetup::options();
		std::string shotTime, buyingTime;
		for (size_t i = 0; i < tut.size(); i++)
		{
			if (tut[i].name == "ShotTime") shotTime = tut[i].value;
			if (tut[i].name == "BuyingTime") buyingTime = tut[i].value;
		}
		// The two that make it a tutorial rather than a game: no clock on the
		// shot, and no buying phase to sit through before playing.
		check(shotTime == "0", "...with no shot clock");
		check(buyingTime == "0", "...and no buying phase");

		check(!ScorchDroidSetup::loadPreset("data/no-such-preset.xml"),
			"a missing preset is refused rather than half-applied");
		std::vector<ScorchDroidSetup::Option> after = ScorchDroidSetup::options();
		std::string stillShotTime;
		for (size_t i = 0; i < after.size(); i++)
		{
			if (after[i].name == "ShotTime") stillShotTime = after[i].value;
		}
		check(stillShotTime == "0", "...leaving the previous options untouched");
		ScorchDroidSetup::reset();
	}

	// M14: the difficulty presets, which are not code anywhere - upstream's
	// menu is each mod's modinfo.xml naming a few options files. Worth pinning
	// because the whole feature is "read what the mod says", so a parse that
	// quietly returned nothing would show as a menu with no games in it.
	{
		std::vector<ScorchDroidSetup::Preset> base = ScorchDroidSetup::presets(".", "none");
		check(base.size() == 4, "the base game offers four ready-made games");

		bool sawEasy = false, sawTarget = false, allDescribed = true, allTagged = true;
		std::string easyFile;
		for (size_t i = 0; i < base.size(); i++)
		{
			if (base[i].name.empty() || base[i].description.empty()) allDescribed = false;
			if (base[i].mod != "none") allTagged = false;
			if (base[i].name == "Easy Game") { sawEasy = true; easyFile = base[i].gamefile; }
			if (base[i].name == "Target practice") sawTarget = true;
		}
		check(allDescribed, "...each with a name and upstream's own description to show");
		check(allTagged, "...each tagged with the mod it came from");
		check(sawEasy && sawTarget, "including the easy game and target practice");

		// The point of the gamefile: it has to be loadable as it stands.
		check(!easyFile.empty() && ScorchDroidSetup::loadPreset(easyFile),
			"a preset's own gamefile loads");
		std::vector<ScorchDroidSetup::Option> easy = ScorchDroidSetup::options();
		std::string money;
		for (size_t i = 0; i < easy.size(); i++)
		{
			if (easy[i].name == "MoneyStarting") money = easy[i].value;
		}
		check(money == "50000", "...and brings its own settings with it");
		check(ScorchDroidSetup::mod() == "none",
			"...leaving the base game selected, as its file says nothing about mods");

		// A mod's own presets, which is the reason this reads modinfo.xml
		// rather than naming four files: Apocalypse ships four of its own,
		// and its files select the mod themselves.
		std::vector<ScorchDroidSetup::Preset> apoc = ScorchDroidSetup::presets(".", "apoc");
		check(apoc.size() == 4, "the Apocalypse mod offers four of its own");
		std::string apocFile;
		for (size_t i = 0; i < apoc.size(); i++)
		{
			if (apoc[i].name == "Apocalypse Easy Game") apocFile = apoc[i].gamefile;
		}
		check(!apocFile.empty(), "...under its own names");
		check(!apocFile.empty() && ScorchDroidSetup::loadPreset(apocFile),
			"...which load too");
		check(ScorchDroidSetup::mod() == "apoc",
			"...and select the mod themselves, so no separate mod choice is needed");
		// Its easy game names bots ("Shocker") the base game does not, which
		// is fine here and would not be if the mod had not come with it - the
		// substitution below is what makes that safe either way.
		ScorchDroidSetup::ensureBotsValidForMod(".");
		std::vector<std::string> apocBots = ScorchDroidSetup::botNames(".", "apoc");
		check(!apocBots.empty(), "...with the mod's own bot list readable");

		// M19: which maps are in play. Upstream keeps this as a colon-
		// separated whitelist in one string option, with empty meaning all.
		{
			// From the shipped config: the presets loaded above name their own
			// landscapes (the tutorial plays on a short list of simple ones),
			// and this is about what a normal game starts from.
			ScorchDroidSetup::reset();
			std::vector<std::string> all = ScorchDroidSetup::landscapes(".");
			check(all.size() > 10, "the base game's landscapes are listed");
			bool sawIslands = false, sawCavern = false;
			for (size_t i = 0; i < all.size(); i++)
			{
				if (all[i] == "islands") sawIslands = true;
				if (all[i] == "cavern") sawCavern = true;
			}
			check(sawIslands && sawCavern, "...by upstream's own names");
			check(ScorchDroidSetup::selectedLandscapes().empty(),
				"...with none singled out to begin with, which upstream reads as all of them");

			check(ScorchDroidSetup::setLandscapes({ "islands", "cavern" }),
				"a subset can be chosen");
			std::vector<std::string> chosen = ScorchDroidSetup::selectedLandscapes();
			check(chosen.size() == 2 && chosen[0] == "islands" && chosen[1] == "cavern",
				"...and reads back");
			{
				OptionsGame probe;
				const char *path = "/tmp/scorchdroid-land.xml";
				check(ScorchDroidSetup::writeSessionFile(path), "...is written out");
				check(probe.readOptionsFromFile(path), "...and read back");
				check(0 == strcmp(probe.getLandscapes(), "islands:cavern"),
					"...as the colon-separated list upstream parses");
				// The engine's own filter is what decides, so ask it.
				LandscapeDefinitionsBase definitions;
				check(!definitions.landscapeEnabled(probe, "hilly"),
					"...and the engine agrees a landscape outside the list is off");
				check(definitions.landscapeEnabled(probe, "cavern"),
					"...and one inside it is on");
				unlink(path);
			}
			check(ScorchDroidSetup::setLandscapes({}), "and clearing it goes back to all of them");
			check(ScorchDroidSetup::selectedLandscapes().empty(), "...leaving nothing singled out");
		}

		check(ScorchDroidSetup::presets(".", "no-such-mod").empty(),
			"a mod that isn't there offers nothing rather than failing");
		ScorchDroidSetup::setMod("none");
		ScorchDroidSetup::reset();
	}

	// Every option has to survive the session file, not just the ones a test
	// happens to name. The server now starts from that file rather than from
	// the shipped config, so an option whose string form does not round-trip
	// would quietly change the game - and would do it for everyone, in the
	// builds players use, with nothing on screen to say so.
	{
		// From the shipped state, so the comparison is against the file this
		// test reads back rather than against edits made earlier above - which
		// is what made the first run of this look like a round-trip failure
		// when it was only my own changed round count.
		ScorchDroidSetup::reset();

		const char *roundTripPath = "/tmp/scorchdroid-host-tests-roundtrip.xml";
		check(ScorchDroidSetup::writeSessionFile(roundTripPath),
			"the session config is written for the round-trip check");

		OptionsGame before;
		before.readOptionsFromFile(SCORCHDROID_APP_CONFIG);
		OptionsGame after;
		check(after.readOptionsFromFile(roundTripPath),
			"...and read back");

		// Compare by name rather than by position, so a reordering shows up as
		// a missing option rather than as a silent mismatch.
		std::map<std::string, std::string> afterValues;
		std::list<OptionEntry *> &afterList = after.getOptions();
		for (std::list<OptionEntry *>::iterator itor = afterList.begin();
			itor != afterList.end();
			++itor)
		{
			afterValues[(*itor)->getName()] = (*itor)->getValueAsString();
		}

		std::string firstMismatch;
		int mismatches = 0, compared = 0;
		std::list<OptionEntry *> &beforeList = before.getOptions();
		for (std::list<OptionEntry *>::iterator itor = beforeList.begin();
			itor != beforeList.end();
			++itor)
		{
			const std::string name = (*itor)->getName();
			const std::string value = (*itor)->getValueAsString();
			compared++;
			std::map<std::string, std::string>::iterator found = afterValues.find(name);
			if (found == afterValues.end() || found->second != value)
			{
				mismatches++;
				if (firstMismatch.empty())
				{
					firstMismatch = name + " (\"" + value + "\" -> \"" +
						(found == afterValues.end() ? std::string("missing") : found->second) + "\")";
				}
			}
		}
		if (mismatches > 0)
		{
			fprintf(stderr, "  %d of %d options changed; first: %s\n",
				mismatches, compared, firstMismatch.c_str());
		}
		check(compared > 50, "the round-trip check actually compared the options");
		check(mismatches == 0,
			"every option survives being written to the session config and read back");
		unlink(roundTripPath);
	}

	ScorchDroidSetup::reset();
	std::string afterReset;
	std::vector<ScorchDroidSetup::Option> reloaded = ScorchDroidSetup::options();
	for (size_t i = 0; i < reloaded.size(); i++)
	{
		if (reloaded[i].name == "NumberOfRounds") afterReset = reloaded[i].value;
	}
	check(afterReset == originalRounds, "reset() goes back to what the config file says");
}

// M21: the landscape's own ambient sound. Read from upstream's tex*.xml and
// the ambientsound*.xml files it includes, so what is worth pinning is that
// the chain from "a landscape is loaded" to "these wav files" holds.
static void testAmbientSound()
{
	printf("ambient sound (M21: the landscape's own atmosphere):\n");

	// A landscape has to be loaded for there to be anything to ask about,
	// which the server started earlier in this run has done.
	if (!ScorchedServer::instance())
	{
		check(false, "a server exists to read a landscape from");
		return;
	}

	std::vector<ScorchDroidAmbient::Sound> sounds =
		ScorchDroidAmbient::forCurrentLandscape(ScorchedServer::instance()->getContext());
	// Not every landscape defines one - texblank is silent - so this checks
	// the mechanism rather than a particular count.
	printf("  (this landscape defines %d ambient sound%s)\n",
		(int) sounds.size(), sounds.size() == 1 ? "" : "s");

	bool allReadable = true, allNamed = true;
	for (size_t i = 0; i < sounds.size(); i++)
	{
		if (sounds[i].file.empty()) allNamed = false;
		std::ifstream probe(sounds[i].file.c_str());
		if (!probe.is_open()) allReadable = false;
		if (!sounds[i].looped && sounds[i].maxSeconds <= 0.0f) allNamed = false;
	}
	check(allNamed, "every ambient sound names a file, and a repeat has an interval");
	check(allReadable, "...and every file named is one that exists on disk");

	// The shipped data is the real check, read from a named landscape so it
	// does not depend on which one the running game happened to pick:
	// arizona asks for ocean waves.
	{
		std::vector<ScorchDroidAmbient::Sound> arizona = ScorchDroidAmbient::forTexFile(
			"data/globalmods/none/data/landscapes/texarizona.xml");
		check(arizona.size() == 1, "arizona declares one ambient sound");
		check(!arizona.empty() &&
			arizona[0].file.find("oceanwaves") != std::string::npos,
			"...the ocean waves its own definition names");
		check(!arizona.empty() && arizona[0].looped,
			"...on a loop, as its timing says");

		// The tropical map is the interesting one: a looping bird track with two
		// chirps played at intervals over it, which is where the repeat
		// timing has to come through rather than being read as another loop.
		std::vector<ScorchDroidAmbient::Sound> jungle = ScorchDroidAmbient::forTexFile(
			"data/globalmods/none/data/landscapes/textropical.xml");
		int looped = 0, repeats = 0;
		for (size_t i = 0; i < jungle.size(); i++)
		{
			if (jungle[i].looped) looped++;
			else if (jungle[i].maxSeconds > jungle[i].minSeconds) repeats++;
		}
		check(looped >= 1 && repeats >= 2,
			"the tropical map's loop and its two intermittent chirps all come through");
	}
}

// M16: who the player is - the name, and now the tank model, colour and
// avatar. The lists are all read from shipped data, so what is worth pinning
// is that they are found at all and that a choice survives being made.
static void testPlayerProfile()
{
	printf("player identity (M16: name, tank, colour, avatar):\n");

	check(ScorchDroidProfile::name() == "Player",
		"a player has a name before anything sets one");
	check(ScorchDroidProfile::setName("  Dan  ") == "Dan",
		"a name is trimmed on the way in");
	check(ScorchDroidProfile::setName("   ") == "Dan",
		"...and an empty one is refused rather than leaving a nameless tank");

	// The base game's tanks.xml. A hundred and five entries, one of which is
	// upstream's "Random" placeholder and is deliberately not offered.
	std::vector<std::string> models = ScorchDroidProfile::models(".", "none");
	check(models.size() > 50, "the base game offers a large list of tank models");
	bool sawRandom = false, sawTiger = false;
	for (size_t i = 0; i < models.size(); i++)
	{
		if (models[i] == "Random") sawRandom = true;
		if (models[i] == "Tiger II") sawTiger = true;
	}
	check(sawTiger, "...including one we can name");
	check(!sawRandom,
		"...but not upstream's \"Random\" entry, which is the empty choice, not a tank");

	// The Apocalypse mod ships no tanks.xml of its own, and upstream's
	// getModFile falls back to the base game's. A mod picker that showed no
	// tanks for it would be wrong about what the game will actually offer.
	std::vector<std::string> apocModels = ScorchDroidProfile::models(".", "apoc");
	check(apocModels.size() == models.size(),
		"a mod with no tanks.xml of its own falls back to the base game's");

	std::vector<unsigned int> colors = ScorchDroidProfile::colors();
	check(colors.size() == 26, "upstream's palette comes through, all twenty-six");
	check(colors[0] == 0xff0000u, "...starting with the red a solo player has always had");

	std::vector<std::string> avatars = ScorchDroidProfile::avatars(".");
	check(avatars.size() == 18, "the shipped avatars are found");
	bool allPng = true, sawComputer = false;
	for (size_t i = 0; i < avatars.size(); i++)
	{
		if (avatars[i].size() < 5 ||
			avatars[i].compare(avatars[i].size() - 4, 4, ".png") != 0) allPng = false;
		if (avatars[i] == "data/avatars/computer.png") sawComputer = true;
	}
	check(allPng, "...images only, not the licence note beside them");
	check(sawComputer, "...including the one every bot wears");

	// The choices themselves. Each has an unchosen value meaning "let the
	// game pick", which is what this port did before M16.
	check(ScorchDroidProfile::model().empty() &&
		ScorchDroidProfile::colorIndex() < 0 &&
		ScorchDroidProfile::avatar().empty(),
		"nothing is chosen to begin with, so the game picks as it always did");

	ScorchDroidProfile::setModel("Tiger II");
	ScorchDroidProfile::setColorIndex(3);
	ScorchDroidProfile::setAvatar("data/avatars/yoda.png");
	check(ScorchDroidProfile::model() == "Tiger II" &&
		ScorchDroidProfile::colorIndex() == 3 &&
		ScorchDroidProfile::avatar() == "data/avatars/yoda.png",
		"a chosen identity is held");

	// colorFor is the half that can be checked without a running game: it
	// answers with the chosen colour, and with the tank's own when the
	// choice is out of range or absent.
	Vector allocated(0.5f, 0.5f, 0.5f);
	Vector chosen = ScorchDroidProfile::colorFor(allocated);
	check(chosen != allocated, "a chosen colour replaces the allocated one");
	ScorchDroidProfile::setColorIndex(9999);
	check(ScorchDroidProfile::colorFor(allocated) == allocated,
		"...and a colour index past the end of the palette is ignored");
	ScorchDroidProfile::setColorIndex(-1);
	check(ScorchDroidProfile::colorFor(allocated) == allocated,
		"...as is no choice at all");

	// Back to unchosen, so a later test starting a game gets the same tank
	// it would have had before this ran.
	ScorchDroidProfile::setModel("");
	ScorchDroidProfile::setAvatar("");
	ScorchDroidProfile::setName("Player");
}

int main(int argc, char **argv)
{
	// Upstream's own generic main() bootstrap (common/main.hpp) does this
	// before anything else - a real networked process needs it or a write
	// to a socket whose peer already closed (routine during connect/
	// disconnect teardown, exactly what testClientJoin()'s two-process
	// handshake exercises) kills the whole process with SIGPIPE by
	// default. host-tests has its own plain main() rather than going
	// through that bootstrap, so it never inherited this.
	signal(SIGPIPE, SIG_IGN);
	signal(SIGSEGV, crashBacktraceHandler);
	signal(SIGABRT, crashBacktraceHandler);

	// Spawned by testClientJoin() as a genuinely separate process - see
	// that function's comment for why this isn't just an in-process object.
	if (argc == 5 && 0 == strcmp(argv[1], "--client"))
	{
		return runClientProcess(argv[2], atoi(argv[3]), argv[4]);
	}

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
	testHumanTankHasNoAI();
	testEconomyBuyAndSelect();
	testDefenseAccessories();
	testNonShotMoves();
	testTerrainDeformation();
	testCameraPickRay();
	testSkyDescription();
	testTreeGeometry();
	testInstancePacking();
	testChatStore();
	testLandscapeTargets();
	testTankMovement();
	testRealTcpHostAndConnect();
	testClientJoin();
	testServerRestart();
	testGameSetup();
	testPlayerProfile();
	testAmbientSound();

	printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
		failures, failures == 1 ? "" : "s");
	return failures == 0 ? 0 : 1;
}

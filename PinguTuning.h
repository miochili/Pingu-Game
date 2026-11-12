// Pingu - the abilities' tuning numbers in one place. The gameplay reads them (PenguinAbilities.cpp,
// PenguinProjectile.cpp), the PINGUPEDIA shows them (GetAbilityStats), and Traits mode scales some of them per use (the
// use's strength: UPenguinAbilityComponent::GetUseStrength, PinguTraitTuning::GetReachScale / GetDurationScale).
//
// Distances in cm, speeds in cm/s, times in s. "x size": the user's size scales it (big penguins reach further).

#pragma once

#include "CoreMinimal.h"
#include "PenguinTypes.h"
#include "PinguTraitTable.h"

class UPenguinMovementComponent;

namespace PinguTuning
{
	// ------------------------------------------------------------------ snowball
	constexpr float SnowballSpeed = 2100.f;
	/** The hit: along the flight this fast, plus SnowballLift up (x the snowball's size; a frozen one 1.3x). */
	constexpr float SnowballPush = 1300.f;
	constexpr float SnowballLift = 480.f;
	constexpr float SnowballStun = 0.4f;
	constexpr float SnowballRecoil = 420.f;      // air only; aiming down turns it into a hop
	/** The combo: a hit gives the next snowball back at once (UPenguinAbilityComponent::RefundCooldown), and every hit in that
	 *  chain pushes harder - the 1st hit x1, the 2nd x(1 + SnowballComboStep), the 3rd x(1 + 2 x SnowballComboStep) ... never past
	 *  SnowballComboMax (the lift goes with the push). A chain goes on while each hit comes within SnowballComboWindow of the one
	 *  before and no snowball missed in between (a miss has to cool down: its throw is no refunded one). It is the thrower's, not the
	 *  victim's: a hit on somebody else goes on with it. */
	constexpr float SnowballComboStep = 0.35f;   // +35 % push per hit in the chain
	constexpr float SnowballComboMax = 3.f;      // the push stops growing at x3 (the 7th hit)
	constexpr float SnowballComboWindow = 1.5f;  // s from one hit to the next
	/** The push scale of a combo's Hit-th hit (1 = the first). */
	inline float GetSnowballComboScale(int32 Hit)
	{
		return FMath::Min(1.f + SnowballComboStep * static_cast<float>(FMath::Max(Hit - 1, 0)), SnowballComboMax);
	}
	/** A snowball against a frozen penguin's ice cube: it bursts on the ice (the ice and the penguin inside are not hurt) and
	 *  nudges the cube along, this share of its push (the wind's push on the cube is 0.4: APenguinCharacter IceCubePushScale). */
	constexpr float SnowballIceNudge = 0.2f;

	// ------------------------------------------------------------------ icicle
	constexpr float IcicleSpeed = 2600.f;        // a full charge's throw: perfectly straight (no drop, no lift) and quick
	/** The throw: the longer its key is held, the faster it goes (IcicleFullChargeTime is the perfectly straight icicle at
	 *  IcicleSpeed). The speed rises smoothly from IcicleTapSpeed (a tap) to IcicleSpeed. A tap is a weak, heavy lob: it
	 *  comes down after about 2 m (aimed level; IcicleTapReach). The drop and the lift above the aim shrink with the hold
	 *  and are gone at full charge. */
	constexpr float IcicleTapSpeed = 600.f;      // cm/s with no charge ...
	constexpr float IcicleTapGravity = 2800.f;   // ... and its gravity (cm/s^2)
	constexpr float IcicleGravityFalloff = 1.5f; // gravity x (1 - charge)^this: the drop shrinks with the charge, none at full
	constexpr float IcicleTapLiftDeg = 6.f;      // the lob points this much above the aim with no charge (none at full)
	constexpr float IcicleTapReach = 230.f;      // cm from the penguin a tap lands, aimed level (PINGUPEDIA; look_IcicleCharge measures it: 232)
	/** The pull-out (s): the icicle comes out from behind the back and the arm cocks over the head. A key let go sooner still
	 *  throws - at the end of the pull-out. */
	constexpr float IciclePullOutTime = 0.1f;
	/** Held this long (from the press) is a full charge. Holding longer stays at full. (1.0 s before 2026-11-05; Linus: "make the
	 *  icicle have a longer charge up time": 1.5x.) */
	constexpr float IcicleFullChargeTime = 1.5f;
	/** Held this long at full charge (IcicleMeltAfterFull s after IcicleFullChargeTime) the icicle melts away in the flipper:
	 *  nothing is thrown and the cooldown starts (Linus: "after 5s max charge it just despans so u cant hold on to it for ever"). */
	constexpr float IcicleMeltAfterFull = 5.0f;
	/** A bot lets go this long before the melt, at the latest. */
	constexpr float IcicleBotMeltMargin = 0.6f;
	/** The forward swing (s): from the release (or the end of the pull-out) to the icicle leaving the flipper. */
	constexpr float IcicleSwingTime = 0.05f;
	/** The arm's follow-through (s) after the icicle has left. */
	constexpr float IcicleFollowTime = 0.1f;
	/** A stuck icicle (a wall peg or a ground pad) lasts this long. */
	constexpr float PegLifetime = 10.f;

	// ------------------------------------------------------------------ belly stomp
	/** The slam (knocks out): the other's capsule within SlamReach sideways (x size; it is a normal penguin's centre
	 *  distance) and SlamHeight up or down of the feet. */
	constexpr float StompSlamReach = 150.f;
	constexpr float StompSlamHeight = 110.f;
	constexpr float StompSlamPush = 700.f;       // the knock-out's launch (never scaled)
	constexpr float StompSlamLift = 1000.f;
	/** The shockwave around it (x size): pushes, stuns a moment. */
	constexpr float StompShockRadius = 360.f;
	constexpr float StompShockPush = 1100.f;
	constexpr float StompShockLift = 650.f;
	constexpr float StompShockStun = 0.45f;
	/** The crater it smashes into the island (x size). */
	constexpr float StompCraterRadius = 110.f;
	constexpr float StompBounceHeld = 1350.f;    // the bounce off the slam with jump held ...
	constexpr float StompBounceTap = 620.f;      // ... and without

	// ------------------------------------------------------------------ iceberg drop
	/** Where it can be dropped: this far left / right of the dropper, this far below and above it. */
	constexpr float IcebergReachX = 1000.f;
	constexpr float IcebergReachDown = 900.f;
	constexpr float IcebergReachUp = 700.f;
	constexpr int32 IcebergBlocks = 2;           // a normal iceberg is this many blocks wide and high

	// ------------------------------------------------------------------ ice that grows with its maker (iceberg drop, ice wall)
	/** A grown penguin's ice is bigger (the maker's size, GetSizeScale, when it is made). From IceGrowSize the iceberg is one
	 *  block wider and higher (3x3) and the wall two blocks thick; from IceBigSize both are made of big blocks - each as big as
	 *  IceBigBlockCells x IceBigBlockCells cells of the island, on its lattice (ABlockIsland's big blocks): the iceberg 2x2 of
	 *  them (4x4 cells), the wall a line of them (2 cells thick). Smaller than IceGrowSize (Tiny, shrunk, a Giant not grown
	 *  further: 1.68) it is the normal ice. */
	constexpr float IceGrowSize = 2.f;
	constexpr float IceBigSize = 4.f;
	constexpr int32 IceBigBlockCells = 2;
	/** Sizes this close under a step count as there (a size a hair under it while growing toward it). */
	constexpr float IceSizeSlack = 0.01f;
	struct FPinguIceScale
	{
		/** Cells per side of each block: 1, or IceBigBlockCells (big blocks). */
		int32 Span = 1;
		/** The wall's thickness in cells (across it): 1, or 2 from IceGrowSize. */
		int32 WallCellsAcross = 1;
		/** The iceberg's width and height in cells. */
		int32 IcebergCells = IcebergBlocks;
	};
	inline FPinguIceScale GetIceScale(float MakerSize)
	{
		FPinguIceScale Scale;
		if (MakerSize >= IceBigSize - IceSizeSlack)
		{
			Scale.Span = IceBigBlockCells;
			Scale.WallCellsAcross = IceBigBlockCells;
			Scale.IcebergCells = IcebergBlocks * IceBigBlockCells;
		}
		else if (MakerSize >= IceGrowSize - IceSizeSlack)
		{
			Scale.WallCellsAcross = 2;
			Scale.IcebergCells = IcebergBlocks + 1;
		}
		return Scale;
	}

	// ------------------------------------------------------------------ flipper slap
	/** Hits whoever's capsule is within SlapHitRadius of a point SlapReachAhead along the aim (x size). Shots within
	 *  SlapBatRadius of that point are batted back. */
	constexpr float SlapReachAhead = 85.f;
	constexpr float SlapHitRadius = 100.f;
	constexpr float SlapBatRadius = 125.f;
	/** The slap stuns (no moving, no abilities: 2 s, it was 1) and knocks back only a little (it was 1750 / 620). Another
	 *  slap during that stun or within a second after it only makes it flinch (APenguinCharacter::ApplyPenguinHit). */
	constexpr float SlapStun = 2.f;
	constexpr float SlapPush = 650.f;
	constexpr float SlapLift = 300.f;
	constexpr float SlapLunge = 720.f;           // along the aim, full strength once per air time
	constexpr float SlapLungeUp = 560.f;
	constexpr float SlapIceRadius = 75.f;        // the ice block in front of the flipper breaks (x size)

	// ------------------------------------------------------------------ frost breath
	constexpr float FrostBreathTime = 0.4f;      // it blows this long
	constexpr float FrostRange = 440.f;          // to the capsule's surface (x size)
	constexpr float FrostConeDeg = 32.f;         // half the cone's angle
	constexpr float FrostFreezeTime = 2.3f;      // a penguin it reaches is frozen this long
	constexpr float FrostPush = 250.f;           // ... and nudged along the breath
	constexpr float FrostShotFreeze = 2.5f;      // shots in the cone freeze this long (APenguinProjectile::FreezeInArea)
	constexpr float FrostJetSpeed = 900.f;       // top speed of the frost jet (away from the cursor)
	constexpr float FrostJetAccel = 9000.f;

	// ------------------------------------------------------------------ ice wall
	constexpr int32 IceWallBlocks = 3;           // a column this high (in the air: a ledge this wide); x the size's block (big blocks)
	constexpr float IceWallRange = 650.f;        // from the user, at most
	/** The wall is solid the moment it is cast (whoever is there is moved out of it, shots there break on it); its look rises
	 *  into place this long out of its floor while the marker rings show, then the build burst (puffs, shards, sound). */
	constexpr float IceWallDelay = 0.35f;

	// ------------------------------------------------------------------ snow warp
	constexpr float WarpRange = 700.f;
	constexpr float WarpKeepSpeed = 0.35f;       // a warp keeps a third of the speed
	/** Aim at one of your own shots (this close to it, this far from you at most) to swap places with it. */
	constexpr float WarpSwapAimRadius = 150.f;   // (generous: on a LAN client the shot is drawn a little behind the server's)
	constexpr float WarpSwapMaxRange = 1400.f;

	// ------------------------------------------------------------------ fish hook
	/** The hook flies this fast and its line reaches this far. */
	constexpr float HookSpeed = 6000.f;
	constexpr float HookReach = 3000.f;
	/** A hooked penguin lands this far behind the thrower (x the thrower's size, 0.75 .. 1.25). */
	constexpr float HookThrowBehind = 400.f;

	// ------------------------------------------------------------------ ice shell
	constexpr float ShellDuration = 1.1f;
	constexpr float ShellPop = 330.f;            // used in the air: a little pop up
	/** Wind (gust, noot) can't hurt a penguin in an ice shell, but it blows the bubble away. */
	constexpr float ShellWindShare = 0.35f;
	constexpr float ShellWindLift = 380.f;       // enough to lift a shelled penguin off the ground: the bubble drifts
	/** Two ice shells that touch bounce apart: away from each other along X, a little up - a touching pair at most this often. */
	constexpr float ShellBounceSide = 700.f;
	constexpr float ShellBounceUp = 250.f;
	constexpr double ShellBounceInterval = 0.4;

	// ------------------------------------------------------------------ tornado (the Gust slot, EPenguinAbility::Gust)
	/** A short tornado round its user (APinguTornado): for TornadoTime the user hangs perfectly still in its eye, in the air
	 *  or on the ground (no other ability meanwhile; a knockback, a freeze or a knock-out ends that). For TornadoSuckTime it
	 *  sucks everything within TornadoRadius in - other penguins, shots, ability bubbles, falling icebergs and the island's
	 *  ice blocks, which it rips out - then throws it all out at once, the nearer the middle the harder; then it winds down.
	 *  Radius x the user's size (Traits mode: x sqrt(the use's strength); the push x the strength: FPenguinHit::Strength). */
	constexpr float TornadoRadius = 600.f;
	constexpr float TornadoTime = 1.f;
	constexpr float TornadoSuckTime = 0.5f;
	/** The throw (a penguin): NootPointBlankPush x TornadoPushCenterShare at the middle down to x TornadoPushEdgeShare at the
	 *  radius (linear in between), away from the middle - at least a little upward (TornadoMinUp) - plus the lift. */
	constexpr float TornadoPushCenterShare = 0.5f;
	constexpr float TornadoPushEdgeShare = 0.15f;
	constexpr float TornadoMinUp = 0.3f;
	constexpr float TornadoLiftCenter = 650.f;
	constexpr float TornadoLiftEdge = 300.f;
	constexpr float TornadoStunCenter = 0.55f;
	constexpr float TornadoStunEdge = 0.35f;
	constexpr float TornadoFrozenBonus = 1.3f;   // a frozen penguin's ice cube is thrown 1.3x as hard (it slides off as ice)
	/** The suck (a penguin): dragged toward the middle - no steering meanwhile - this fast at the edge, up to TornadoPullNearSpeed
	 *  near the middle; it stops at the eye (TornadoEyeRadius x the user's size: round the user, never into it). */
	constexpr float TornadoPullEdgeSpeed = 350.f;
	constexpr float TornadoPullNearSpeed = 850.f;
	constexpr float TornadoPullResponse = 8.f;   // 1/s: how fast its own speed turns into the pull
	constexpr float TornadoPullLift = 150.f;     // cm/s up on top of the pull (off its feet: it is sucked up into the funnel)
	constexpr float TornadoPullGravity = 0.15f;  // gravity x this while it is pulled
	constexpr float TornadoEyeRadius = 95.f;
	/** Shots and ability bubbles circle the eye at TornadoOrbitRadius (x size) while it sucks, pulled there at up to
	 *  TornadoShotPull and swept round at TornadoSpinSpeed; then they fly out from TornadoShotThrowCenter (at the middle)
	 *  down to TornadoShotThrowEdge (at the radius) - a shot as the user's own (it never hits the user while it sucks). */
	constexpr float TornadoOrbitRadius = 150.f;
	constexpr float TornadoShotPull = 1400.f;
	constexpr float TornadoSpinSpeed = 900.f;
	constexpr float TornadoShotThrowCenter = 2600.f;
	constexpr float TornadoShotThrowEdge = 1300.f;
	/** The island's ice: the blocks it reaches (one layer: the face toward the middle open and nothing solid on the way, never
	 *  the floor under the user's feet), at most TornadoIceBlocks nearest first, are ripped out (not broken), circle the eye at
	 *  TornadoIceOrbitRadius and are thrown out with the rest - heavy, they come down as falling ice that crushes whoever it
	 *  lands on (the user's knock-out, reason Crushed) and is the island's ice again where it stops. */
	constexpr int32 TornadoIceBlocks = 10;
	constexpr float TornadoIceOrbitRadius = 210.f;
	constexpr float TornadoIceThrowSpeed = 1800.f;
	constexpr float TornadoIceThrowLift = 1200.f;
	/** A falling iceberg in reach is drawn in a little at the start and blown away at the throw (AIcebergDrop::Blow). */
	constexpr float TornadoIcebergPull = 0.4f;
	constexpr float TornadoIcebergStrength = 1.f;
	/** Two tornadoes whose reaches overlap (their eyes closer than their two radii added up) suck each other in and become one
	 *  giant tornado (APinguTornado::BeginMerge / Absorb). For TornadoMergeDriftTime both eyes drift together - their users,
	 *  still hanging in them, too - to the point between them (weighted by their radii), then they are one: its reach the two
	 *  reaches added up (6 + 6 = 12 m; at most TornadoMergeMaxRadius), its push the two pushes added up at every distance (the
	 *  same falloff over the new reach; at most TornadoMergeMaxStrength x one plain tornado's), its funnel as wide as both. It
	 *  sucks for TornadoMergeSuckTime more (one that had thrown already sucks again), then throws, and lasts until the later
	 *  of the two would have ended plus TornadoMergeExtraTime (but winds down at least TornadoMergeMinWindDown after its throw).
	 *  Its users hang side by side in its eye until then (TornadoMergeEyeGap cm between their capsules) and it never touches
	 *  them. It rips out more of the island's ice in its bigger reach, until it holds TornadoMergeIceBlocks. A tornado with less
	 *  than TornadoMergeMinLeft to go merges no more. A third one that reaches a giant merges in the same way. */
	constexpr float TornadoMergeDriftTime = 0.3f;
	constexpr float TornadoMergeSuckTime = 0.5f;
	constexpr float TornadoMergeExtraTime = 0.5f;
	constexpr float TornadoMergeMinWindDown = 0.4f;
	constexpr float TornadoMergeMinLeft = 0.1f;
	constexpr float TornadoMergeMaxRadius = 2000.f;
	constexpr float TornadoMergeMaxStrength = 3.f;
	constexpr int32 TornadoMergeIceBlocks = 20;
	constexpr float TornadoMergeEyeGap = 40.f;

	// ------------------------------------------------------------------ snow bomb
	constexpr float BombSpeed = 1350.f;          // the lob ...
	constexpr float BombGravity = 1500.f;        // ... and its gravity
	constexpr float BombFuse = 1.3f;
	/** The blast (x the bomb's size): knocked out inside the kill radius, pushed away up to the push radius; the crater
	 *  takes the blocks within the crater radius. Its blast zone shows the first two while the fuse burns. Nobody is spared:
	 *  the thrower's own bomb knocks it out and pushes it like anybody (no kill for anyone: a self-knock-out). */
	constexpr float BombKillRadius = 115.f;
	constexpr float BombPushRadius = 330.f;
	constexpr float BombCraterRadius = 200.f;
	/** The push (half of it at the edge of the push radius), plus BombLift up. */
	constexpr float BombPush = 1350.f;
	constexpr float BombLift = 520.f;
	constexpr float BombStun = 0.5f;

	// ------------------------------------------------------------------ noot
	/** A shotgun honk - 5700 cm/s at point blank (inside the other penguin; 3x the old gust's push), falling off steeply with
	 *  distance ((1 - d/range)^2) to a little at the edge of its range. Shots and bubbles in the cone get the same falloff. */
	constexpr float NootRange = 480.f;
	constexpr float NootConeDeg = 40.f;
	constexpr float NootPointBlankPush = 5700.f;
	constexpr float NootEdgeShare = 0.06f;       // what is left of the push at the edge of the range
	constexpr float NootLiftPointBlank = 820.f;  // straight up on top of the push (a bigger pop up close)
	constexpr float NootLiftEdge = 360.f;
	constexpr float NootStunPointBlank = 0.55f;
	constexpr float NootStunEdge = 0.35f;
	constexpr float NootShotSpeed = 3300.f;      // shots blown away at point blank fly off this fast (3x a batted shot)
	constexpr float NootShotMinSpeed = 250.f;    // ... and at least this fast at the edge
	constexpr float NootIceReach = 0.6f;         // breaks ice in the nearer part of its cone (a share of the range)
	constexpr int32 NootIceBlocks = 3;
	constexpr float NootRecoil = 480.f;
	constexpr float NootIcebergStrength = 1.25f; // a falling iceberg in the cone flies off a bit faster than from a gust

	// ------------------------------------------------------------------ ice gun, size rays
	constexpr float IceBoltSpeed = 2600.f;
	constexpr float IceGunSlipTime = 3.f;        // seconds of slippery feet from an ice bolt (x the bolt's size, 0.5 .. 2)
	constexpr float IceGunRecoil = 300.f;
	constexpr float RaySpeed = 2300.f;
	/** A size ray's change wears off after this long. */
	constexpr float SizeRayDuration = 12.f;

	// ------------------------------------------------------------------ giant snowball (APinguGiantSnowball, 2026-10-29)
	/** Hold the key this long to roll it up (the penguin can't move meanwhile); let go sooner and it crumbles - the cooldown runs
	 *  all the same. At the end it rolls off by itself. Sizes are its diameter (a tile = a block = 100 cm). */
	constexpr float GiantBuildTime = 1.5f;
	constexpr float GiantStartSize = 100.f;      // 1 tile when it starts rolling
	constexpr float GiantBuildStartSize = 18.f;  // the little ball the roll-up starts from
	/** Rolling makes it bigger by this much per cm rolled (10 cm per metre: 1 -> 3 tiles in 20 m) - never past GiantRollMaxSize
	 *  (3 tiles: a normal penguin's double jump, 290 + 230 cm, clears it; its single jump clears up to about 2.5 tiles). */
	constexpr float GiantRollGrowPerCm = 0.1f;
	constexpr float GiantRollMaxSize = 300.f;
	/** Snowballs and the grow ray make it bigger past that (Linus: "snowballs can push it past the limit"): a normal snowball
	 *  adds GiantSnowballGrow cm, x the snowball's size (a giant's big ones a lot, a shrunk penguin's small ones a little), x the
	 *  thrower's snowball combo push scale (GetSnowballComboScale) and x the throw's strength. Only a technical cap stops it
	 *  (GiantSafetyMaxSize, 8 tiles: it never fills the island or the camera). The shrink ray makes it smaller, never below
	 *  GiantMinSize (half a tile: still a ball that swallows; whoever is inside stays inside). */
	constexpr float GiantSnowballGrow = 30.f;
	constexpr float GiantSafetyMaxSize = 800.f;
	constexpr float GiantMinSize = 50.f;
	/** Its shown size follows a change at this rate (1/s, as a penguin's size: APenguinCharacter::UpdateSize's 9). */
	constexpr float GiantSizeSpeed = 9.f;
	/** Rolling speed (cm/s; a penguin runs 560): fairly slow. Frozen (ice gun, frost breath, an Ice Touch penguin) it slides
	 *  this much faster and swallows nothing. Falling off a ledge: this gravity. */
	constexpr float GiantRollSpeed = 260.f;
	constexpr float GiantFrozenSpeed = 480.f;
	constexpr float GiantGravity = 2400.f;
	/** Steps (2026-11-05, Linus: "it can go up 1 block when roling but when it went up 1 it will bounce off the next 1block higher
	 *  tile"): it rolls up a step one block high (up its face at GiantClimbSpeed cm/s, then over the corner), but only one in a
	 *  row: right after a climb the next step up is a wall, until it has rolled down a step again. Steps of 2+ blocks are walls.
	 *  The GiantWallHits-th wall hit breaks it. Wider than GiantGapBridgeSize (cm) it rolls over gaps one block wide; smaller,
	 *  it drops into them. */
	constexpr float GiantClimbSpeed = 260.f;
	constexpr float GiantGapBridgeSize = 105.f;
	constexpr int32 GiantWallHits = 3;
	/** It breaks by itself after this long at the latest (s: stuck somewhere). */
	constexpr float GiantMaxLife = 40.f;
	/** A frozen one shoves the penguins it meets instead (cm/s along its way, up, stun; each penguin again after GiantShoveEvery). */
	constexpr float GiantShovePush = 750.f;
	constexpr float GiantShoveLift = 450.f;
	constexpr float GiantShoveStun = 0.35f;
	constexpr float GiantShoveEvery = 0.6f;
	/** Broken: who was inside pops out where it broke (up, and apart sideways), what it swallowed flies out. */
	constexpr float GiantPopUp = 650.f;
	constexpr float GiantPopSide = 260.f;
	constexpr float GiantItemSpeed = 850.f;
	/** What breaks it: a bomb going off with the ball's surface inside its kill radius (a bomb thrown straight into it is
	 *  swallowed), the noot within this share of its range, a slap (its hit circle), a belly stomp coming down on it, a falling
	 *  iceberg, a Hot Feet penguin touching it, and a penguin more than GiantCrushHeight times as tall as it is wide. */
	constexpr float GiantNootShare = 0.5f;
	constexpr float GiantCrushHeight = 2.f;

	// ------------------------------------------------------------------ flap (UPenguinMovementComponent::StartFlap, 2026-11-04)
	/** Linus: "slows ur fall if u are falling and gives u a strong boost up ... it should be 75% of the height of the icicle jump when
	 *  u are falling and form standing it will boost u eaven higher ... like 40% of the speed of an icicle jump". The icicle jump is
	 *  the icicle ground pad (UPenguinMovementComponent::JumpPadSpeed 2970 cm/s at the jump's gravity 4475 cm/s^2: 986 cm). A
	 *  flap from the air climbs FlapFallHeightShare of that, from the ground FlapGroundHeightShare, at FlapSpeedShare of the pad's
	 *  speed (a slow, flappy climb: 1188 cm/s, 0.62 s / 0.83 s); the jump scale (size, trait) scales the speed, its square the
	 *  height, as for a jump. */
	constexpr float FlapFallHeightShare = 0.75f;
	constexpr float FlapGroundHeightShare = 1.f;
	constexpr float FlapSpeedShare = 0.4f;
	/** The defaults' numbers for the PINGUPEDIA (the movement works them out from its own tuning). */
	constexpr float FlapPadSpeed = 2970.f;
	constexpr float FlapJumpGravity = 2.f * 290.f / (0.36f * 0.36f);
	constexpr float FlapPadHeight = FlapPadSpeed * FlapPadSpeed / (2.f * FlapJumpGravity);
	/** Seconds a flap may take beyond its climb (a ceiling in the way): then it ends anyway. */
	constexpr float FlapExtraTime = 0.35f;

	// ------------------------------------------------------------------ snowman (APinguSnowman, 2026-11-03)
	/** Linus: "make it last for ever and just die from like everything just like the ice blocks. Hes balls also have the combo but
	 *  dont get instantly back and he cant move". It is built SnowmanBuildTime after the cast, SnowmanAhead cm in front of its
	 *  builder's feet, and stands until something breaks it (or the round ends). Since 2026-11-05 (card build-limit-snowman;
	 *  Linus: "make the snowman so u can place unlimited times ... it can only spawn on a tile and there can be just one snowman per
	 *  tile"; "gets placed next to the one already placed with max reach of 1 snowman further and nothing happens in the air"): no
	 *  limit per penguin; it stands on the tile (grid block) in front of its builder, one per tile; a taken tile: the next one on,
	 *  never further (APinguSnowman::FindSpot). SnowmanAhead is only the old spot's distance (no longer used for the spot). */
	constexpr float SnowmanBuildTime = 0.45f;
	constexpr float SnowmanAhead = 80.f;
	/** It throws a snowball (its builder's: the combo goes on, but no snowball comes back for a hit) at the nearest enemy within
	 *  SnowmanRange it can see, every SnowmanReload s (the first SnowmanFirstThrow s after it stands), led for the ball's flight.
	 *  Its reload is shorter than the combo's window: hit after hit it builds the combo up on its own. */
	constexpr float SnowmanRange = 900.f;
	constexpr float SnowmanReload = 1.4f; // (under SnowballComboWindow 1.5 s: its hits in a row go on with the combo)
	constexpr float SnowmanFirstThrow = 0.6f;
	/** After one of its snowballs hit a penguin its next throw comes SnowmanHitReload s after the last one instead (Linus: "it also
	 *  shots a bit faster when it hit someone"; 2026-11-05); a miss: SnowmanReload again. */
	constexpr float SnowmanHitReload = 0.9f;
	/** The ground it may stand on: at most this many blocks below its builder's feet (lower: "in the air", nothing is built), or
	 *  one block up (a step). */
	constexpr int32 SnowmanMaxDropBlocks = 2;
	/** Its body (size 1): three balls, SnowmanHeight tall, SnowmanBodyRadius wide at its widest (what touches it). */
	constexpr float SnowmanHeight = 150.f;
	constexpr float SnowmanBodyRadius = 36.f;
	/** The rays resize it like a penguin (x PinguGrowRayFactor / PinguShrinkRayFactor): its balls with it. Past SnowmanMinSize it
	 *  melts away; never bigger than SnowmanMaxSize. */
	constexpr float SnowmanMinSize = 0.5f;
	constexpr float SnowmanMaxSize = 2.f;

	// ------------------------------------------------------------------ everything
	/** Push abilities knock ability bubbles around too. */
	constexpr float PickupPush = 900.f;
	constexpr float NootPickupPush = 3.f * PickupPush;
	/** Knockback by relative size (APenguinCharacter::ApplyPenguinHit): (attacker size / victim size)^SizeKnockbackPower,
	 *  within these bounds. */
	constexpr float SizeKnockbackPower = 1.25f;
	constexpr float SizeKnockbackMin = 0.25f;
	constexpr float SizeKnockbackMax = 4.f;
	/** A penguin's size changes its running speed by size^SizeSpeedPower and its jump speed by size^SizeJumpPower
	 *  (UPenguinMovementComponent::SetSizeScale: 2x size runs 1.23x, jumps 1.19x as fast). */
	constexpr float SizeSpeedPower = 0.3f;
	constexpr float SizeJumpPower = 0.25f;

	/**
	 * The PINGUPEDIA's stat lines of an ability: its cooldown first, then what it does (the numbers above; the stomp's fall
	 * speed and the icicle pad's throw come from Move, BP_Penguin's numbers - null: the class defaults).
	 */
	PENGUIN_API void GetAbilityStats(EPenguinAbility Ability, const UPenguinMovementComponent* Move, TArray<FPinguStatLine>& Out);
}

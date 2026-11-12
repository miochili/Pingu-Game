// Pingu - the playable penguin.

#include "PenguinCharacter.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinAnimInstance.h"
#include "PenguinBot.h"
#include "PenguinGameMode.h"
#include "PenguinInputConfig.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguTuning.h"
#include "PinguAntiCheat.h"
#include "PinguFX.h"
#include "PinguLobby.h"
#include "PinguPickup.h"
#include "PinguSettings.h"
#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "InputActionValue.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace PenguinCharacterPrivate
{
	// Pingu.blend is ~2 units from feet to pom-pom; 0.55 makes him ~111 cm (about one block).
	constexpr float MeshScale = 0.55f;
	constexpr float CapsuleRadius = 30.f;
	constexpr float CapsuleHalfHeight = 56.f;
	// In every ground clip the feet rest 0.16 Blender units above the rig origin.
	constexpr float FeetHeight = 0.16f * 100.f * MeshScale;

	// The ice gun's ice chunks on the feet (FitIceFeet), from Pingu.blend in its rest pose, Blender units: x across the
	// penguin, -y forward, z up. Rig bone heads - they tie the imported mesh's axes and units to Blender's:
	const FVector BlendFootLHead(-0.234f, 0.f, 0.4f);
	const FVector BlendFootRHead(0.234f, 0.f, 0.4f);
	const FVector BlendBodyHead(0.f, 0.f, 0.45f);
	const FVector BlendEyeTargetHead(0.f, -2.5f, 1.35f);
	// The noot horn (SM_NootHorn, Tools/ArtGen/make_noot_horn.py), modelled in Pingu.blend's units along +X (the FBX:
	// 1 unit = 100): its throat (the model's origin) sits at the beak, inside the face - Pingu.blend rest pose.
	const FVector BlendHornPivot(0.f, -0.45f, 1.21f);
	constexpr float HornDuration = 0.5f;
	/** Honking, the body turns up / down toward the aim this far at most (the horn sticks straight out of the beak). */
	constexpr float HonkMaxUp = 70.f;
	constexpr float HonkMaxDown = 60.f;
	/** The first honks of a session go to the log (on a LAN client too: the tests check the horn reaches it). */
	int32 NootHornLogsLeft = 30;

	// A frozen penguin's ice: wind and a belly slide don't break it, they push the ice cube - it slides far on its own, so
	// a gentler push than a hit's.
	constexpr float IceCubePushScale = 0.4f;
	// The attack whose hit the ice took does nothing more for this long (a belly stomp squashes, then slams).
	constexpr double IceGuardTime = 0.4;
	// The fish hook pulls a frozen penguin along in its ice cube (BeginIceThrow): the ice shatters at the first impact -
	// on the ground, or against a wall (flying sideways at least IceThrowWallSpeed, it kept less than IceThrowWallKeep of
	// that speed, with a block right there). Checked from IceThrowArmTime on (the throw has left the ground by then);
	// IceThrowExtraTime past the throw's flight it is no hook throw any more (it fell off the island).
	constexpr float IceThrowArmTime = 0.06f;
	constexpr float IceThrowWallSpeed = 150.f;
	constexpr float IceThrowWallKeep = 0.35f;
	constexpr float IceThrowWallProbe = 14.f;
	constexpr float IceThrowExtraTime = 1.5f;
	// The slap's stun (2 s: UPenguinAbilityComponent's SlapStun): a slap while it lasts, and for this long after it, only
	// knocks (with this little flinch, never longer than the stun left) - no stun-locking.
	constexpr double SlapStunGrace = 1.0;
	constexpr float SlapGraceStun = 0.25f;
	// Knockback by relative size (ApplyPenguinHit): (attacker size / victim size)^SizeKnockbackPower, within these bounds.
	// A normal attacker sends a 0.5x penguin 2.38x as far, a 2x one 0.42x; an 8x attacker a normal one 4x (the bound).
	constexpr float SizeKnockbackPower = PinguTuning::SizeKnockbackPower;
	constexpr float SizeKnockbackMin = PinguTuning::SizeKnockbackMin;
	constexpr float SizeKnockbackMax = PinguTuning::SizeKnockbackMax;
	// Dizzy stars: shown while a stun this long or longer lasts (the slap's 2 s; short hit stuns don't), three of them round
	// the head (this far out, this high above the capsule's middle, this size, turns a second).
	constexpr float DizzyMinStun = 0.6f;
	constexpr float DizzyRadius = 27.f;
	constexpr float DizzyHeight = 64.f;
	constexpr float DizzyStarScale = 0.13f;
	constexpr float DizzyTurns = 1.5f;

	/** How far the honk is buzzing at T (0..1): through both noots (noot.wav: 0.02-0.14 s and 0.20-0.37 s). */
	static float NootHornBuzz(float T)
	{
		return FMath::SmoothStep(0.02f, 0.06f, T) * (1.f - FMath::SmoothStep(0.34f, 0.40f, T));
	}

	/** The horn's length and width (x its full size) T seconds into the honk: it shoots out of the beak, the bell pops
	 *  open, breathes in between the two noots, pops wider for the louder second one, buzzes, and slips back in. */
	static void NootHornShape(float T, float& OutLength, float& OutWidth)
	{
		struct FKey
		{
			float Time;
			float Length;
			float Width;
		};
		static const FKey Keys[] = {
			{ 0.00f, 0.16f, 0.20f },
			{ 0.06f, 1.12f, 0.90f },
			{ 0.11f, 1.00f, 1.10f },
			{ 0.17f, 0.97f, 0.94f },
			{ 0.23f, 1.06f, 1.16f },
			{ 0.38f, 1.00f, 1.04f },
			{ HornDuration, 0.14f, 0.18f },
		};
		OutLength = Keys[UE_ARRAY_COUNT(Keys) - 1].Length;
		OutWidth = Keys[UE_ARRAY_COUNT(Keys) - 1].Width;
		for (int32 Index = 0; Index + 1 < UE_ARRAY_COUNT(Keys); ++Index)
		{
			const FKey& A = Keys[Index];
			const FKey& B = Keys[Index + 1];
			if (T <= B.Time)
			{
				const float Alpha = FMath::SmoothStep(0.f, 1.f, (T - A.Time) / FMath::Max(B.Time - A.Time, 0.001f));
				OutLength = FMath::Lerp(A.Length, B.Length, Alpha);
				OutWidth = FMath::Lerp(A.Width, B.Width, Alpha);
				break;
			}
		}
		const float Buzz = NootHornBuzz(T);
		OutWidth *= 1.f + 0.035f * Buzz * FMath::Sin(T * 2.f * UE_PI * 26.f);
		OutLength *= 1.f + 0.02f * Buzz * FMath::Sin(T * 2.f * UE_PI * 26.f + 1.3f);
	}

	// Each foot (Penguin_Foot_L / _R) spans x -0.51..0.04 (left; the right one mirrored), y -0.52..0.12 (left) and
	// -0.56..0.08 (right), z 0.16..0.49 (its top is inside the body). A chunk reaches 4 cm (Blender) past the foot's side
	// and toes, 2 cm past its heel, down to the ground and up to the body - and stops at the middle, where the two feet
	// meet (no doubled ice there).
	const FVector BlendIceFootLMin(-0.553f, -0.564f, 0.15f);
	const FVector BlendIceFootLMax(0.f, 0.136f, 0.5f);
	const FVector BlendIceFootRMin(0.f, -0.596f, 0.15f);
	const FVector BlendIceFootRMax(0.553f, 0.104f, 0.5f);
	/** SM_Block, the chunks' mesh, is a 100 cm cube. */
	constexpr float IceChunkMeshSize = 100.f;

	// LAN: four penguins on a local network - send them often (they are always relevant).
	constexpr float NetUpdateRate = 100.f;
	constexpr float MinNetUpdateRate = 30.f;
	// The owner sends its aim (for the others' eyes) at most this often, and only when it moved this far (cm).
	constexpr float AimSendInterval = 0.05f;
	constexpr float AimSendTolerance = 2.f;
	// The server's replicated aim follows the aim point to within this (cm).
	constexpr float RepAimTolerance = 1.5f;
	// The owner confirms a round reset once this long and this many frames at the new place have gone by: by then the
	// moves it made before the reset are all on their way to the server.
	constexpr float ResetAckDelay = 0.1f;
	constexpr int32 ResetAckFrames = 3;
	// The server stops waiting for that confirmation after this long.
	constexpr float ResetAckTimeout = 2.f;
	// Waiting for it, a position further than this from the reset place is a move from before the reset.
	constexpr float StaleMoveDistance = 400.f;

	// Belly slide hits (server). The slider lies on its belly: for its contacts it is a low capsule along the floor
	// (radius, half the straight part, and how far its middle sits ahead of the standing capsule's; all x size) - about
	// 120 cm long and 50 cm high, so a penguin can jump over it. Shapes this close count as touching.
	constexpr float SlideLyingRadius = 26.f;
	constexpr float SlideLyingHalfLength = 34.f;
	constexpr float SlideLyingAhead = 10.f;
	constexpr float SlideContactMargin = 6.f;
	// The slide hit's knockback follows the slide's speed (BellySlideHitSpeed = 1x) within these bounds.
	constexpr float SlideHitMinScale = 0.6f;
	constexpr float SlideHitMaxScale = 1.5f;
	constexpr float SlideHitStun = 0.45f;
	// Belly slide effects: snow spray interval (s, with full effects).
	constexpr float SlideSprayInterval = 0.04f;
	// Only without the slide clips: the penguin is tipped onto its belly by hand (per second in, then lifted and pulled
	// back by these cm so the lying body rests on the floor over its capsule).
	constexpr float BellyTiltSpeed = 12.f;
	constexpr float BellyTiltLift = 22.f;
	constexpr float BellyTiltBack = 50.f;

	// A push clashing on a braced penguin (one that has just pushed itself): a ring and sparks around it (x its size), a
	// high slap, and the pusher bounces off (cm/s; standing, the small upward part only scoots it along the ground).
	constexpr float ClashRecoilSide = 500.f;
	constexpr float ClashRecoilUp = 150.f;
	constexpr float ClashRingRadius = 70.f;
	constexpr float ClashRingTime = 0.25f;
	constexpr int32 ClashSparkles = 10;
	constexpr float ClashSparkleRadius = 50.f;
	constexpr float ClashVolume = 0.9f;
	constexpr float ClashPitch = 1.4f;
	static const FLinearColor ClashColor(1.f, 0.9f, 0.55f);

	// The death look (UpdateDeathLook): cartoon knock-outs, all in code (mesh scale and turn, the ice block, an overlay,
	// the effects' particles and afterimages). Everything fits in the 0.85 s a knocked-out penguin stays visible.
	// A pancake (the classic cartoon squash): turned to face the camera (belly, face and dizzy eyes toward it: mesh yaw
	// PancakeYaw, reached within the squash), this flat and this wide across the screen (x the mesh; toward the camera it
	// spreads by this share of the widening), squashed in this long, then jiggling (this much, this fast in rad/s, dying
	// away at this rate). Its flippers and feet flail out to the sides (GetAnimInputs: the stunned flail, dizzy eyes).
	constexpr float PancakeFlat = 0.32f;
	constexpr float PancakeWide = 1.75f;
	constexpr float PancakeDepthShare = 0.4f;
	constexpr float PancakeYaw = 0.f;
	constexpr float PancakeSquashTime = 0.07f;
	constexpr float PancakeJiggle = 0.35f;
	constexpr float PancakeJiggleRate = 38.f;
	constexpr float PancakeJiggleDamp = 5.f;
	// A stomp's pancake: dizzy stars circling this high over the pancake's top (at rest), this far out, this much bigger
	// than the dizzy ones, this many turns a second.
	constexpr float PancakeStarHeight = 14.f;
	constexpr float PancakeStarRadius = 50.f;
	constexpr float PancakeStarScale = 1.3f;
	constexpr float PancakeStarTurns = 1.2f;
	// An iceberg's (or crushed) pancake squirts out from under the ice flat on the floor, facing the camera like a stomp's:
	// stretched up to this much more along the way it slides (at this speed, cm/s), wobbling like jelly in the screen plane
	// (degrees, rad/s, dying away at this rate), leaning its front edge up this far while fast, kicking up snow (a puff
	// every this long with full effects), and it twitches once (from this time, for this long).
	constexpr float PancakeSquirtSpeed = 900.f;
	constexpr float PancakeSquirtStretch = 0.3f;
	constexpr float PancakeWobble = 14.f;
	constexpr float PancakeWobbleRate = 20.f;
	constexpr float PancakeWobbleDamp = 2.5f;
	constexpr float PancakeSkidLean = 8.f;
	constexpr float PancakeSnowInterval = 0.05f;
	constexpr float PancakeTwitchTime = 0.45f;
	constexpr float PancakeTwitchLength = 0.15f;
	// An icicle: frozen solid, the ice block tumbles (degrees a second) and shatters this long after the hit (a burst of
	// ice chunks in this blue); the penguin tumbles on faster once it is out, still stiff for this long, then flailing.
	constexpr float IceDeathTumbleRate = 260.f;
	constexpr float IceDeathShatterTime = 0.5f;
	constexpr float IceDeathFreeTumbleRate = 720.f;
	constexpr float IceDeathThawTime = 0.2f;
	static const FLinearColor IceShatterColor(0.42f, 0.8f, 1.f);
	/** The ice block's place on the capsule (as the constructor sets it). */
	const FVector IceBlockOffset(0.f, 0.f, 4.f);
	// A snow bomb (the cartoon gag): charred on the spot. The soot (M_Ghost, the afterimages' see-through unlit material, as
	// the overlay over the whole penguin: every LOD, every setting, beanie and all) creeps in from the outline over CharTime,
	// a hot glow going to soot black on the way, until the whole penguin is solid black (SootOpacity: past the material's
	// full opacity over the middle of every surface; the rim stays a shade lighter, so the silhouette still reads). Stiff
	// (the frozen pose), facing the camera, puffed up by the blast; only its eyes stay white (the ash pile's eyes on the
	// effects actor, laid over the penguin's own: APinguFX::SetAshPile). It stands like that for CharHoldTime, smoking,
	// embers glinting on it, blinking once and coughing a puff of smoke; then it crumbles into a heap of black ash and the
	// eyes drop onto the heap. They blink and look around until a gust blows the pile away; the eyes go last, then the
	// sooty ghost floats up.
	static const FLinearColor SootColor(0.012f, 0.011f, 0.01f);
	/** The soot's colour as it starts creeping in: a hot orange glow (M_Ghost is unlit: the colour is what shows). */
	static const FLinearColor CharHotColor(1.4f, 0.42f, 0.08f);
	constexpr float SootOpacity = 4.4f;
	/** M_Ghost's opacity is its Opacity parameter x (0.25 + 0.75 x rim) (Tools/ue_setup_project.py, make_translucent_fresnel):
	 *  over the middle of a surface this share of the parameter, at the outline all of it. */
	constexpr float SootMiddleShare = 0.25f;
	/** The soot's parameter goes up as (time / CharTime) to this power (the outline is black early, the middle last). */
	constexpr float SootRampPower = 1.15f;
	static const TCHAR* SootMaterialPath = TEXT("/Game/FX/M_Ghost.M_Ghost");
	constexpr float CharTime = 0.25f;
	constexpr float CharHoldTime = 0.5f;
	/** Puffed up by the blast this much wider (and a third of it squatter) within the char-up; a faint quiver after. */
	constexpr float CharPuff = 0.07f;
	constexpr float CharQuiver = 0.006f;
	/** Smoke rising off it (a puff every this long with full effects) and embers glinting on it (one every this long). */
	constexpr float SmokeInterval = 0.06f;
	constexpr float CharEmberInterval = 0.07f;
	/** Standing there charred, it blinks once and coughs a puff of smoke. */
	constexpr float CharBlinkAt = CharTime + 0.17f;
	constexpr float CoughTime = CharTime + 0.33f;
	/** The penguin turns to face the camera within this long. */
	constexpr float SootTurnTime = 0.07f;
	// Its eyes stay white: the white eyes laid over its eyeballs (Penguin_Eye_L / _R in Pingu.blend: spheres of 0.17 Blender
	// units round the Eye_L / Eye_R bones = 17 mesh units) are this much bigger, so they hide the soot on the penguin's own.
	static const TCHAR* CharEyeBones[2] = { TEXT("Eye_L"), TEXT("Eye_R") };
	constexpr float CharEyeballRadius = 17.f;
	constexpr float CharEyeCover = 1.06f;
	// The ash pile's timeline. It crumbles once the char-up and the hold are over (or later, once it is on the floor: a
	// penguin blown up in the air drops first; one that never lands before it vanishes gets the poof and the ghost
	// instead); the rest counts from the crumble (its "pile time"). The pile sits there, blinking and looking round,
	// AshRestTime longer than the gag needs before the gust blows it away (about 7.5 s after the knock-out it is all gone).
	// A respawn (practice, the lobby: APenguinGameMode's BombRespawnDelay, 2.3 s of real time after the knock-out) or the
	// next round's reset ends it early with a puff of ash (EndDeathLook); the floor broken away under it brings the gust at
	// once.
	constexpr float AshCrumbleAt = CharTime + CharHoldTime;
	/** How much longer than the bare gag the pile waits for the gust (s). */
	constexpr float AshRestTime = 5.f;
	/** The charred penguin comes down into the pile over this long (slowly at first, then all at once; the pile grows
	 *  meanwhile, from the floor up). */
	constexpr float AshCrumbleTime = 0.36f;
	/** The pile has piled up from this share of the crumble on. */
	constexpr float AshGrowFrom = 0.1f;
	/** On the floor: its feet this close above the ground (cm, x its size). */
	constexpr float AshFloorReach = 12.f;
	// The eyes ride down on the sinking face until it has come down to the pile's top (about 0.25 s into the crumble), settle
	// there (within AshEyesSettleTime) with a little squash, hop once, blink, glance one way and the other, then the gust:
	// the eyes widen and look into the wind while it blows the pile away from the upwind side; the eyes left hanging in the
	// air blink once more and are whisked off downwind (shrinking), the ghost rising there.
	constexpr float AshEyesSettleTime = 0.06f;
	constexpr float AshEyesHopTime = 0.16f;
	constexpr float AshEyesHop = 7.f;
	constexpr float AshEyesSquash = 0.35f;
	constexpr float AshEyesSquashTime = 0.08f;
	constexpr float AshBlinkAt = 0.62f;
	constexpr float AshBlinkTime = 0.08f;
	constexpr float AshGlanceAt = 0.76f;
	constexpr float AshGlanceTime = 0.18f;
	constexpr float AshGustAt = 0.96f + AshRestTime;
	constexpr float AshBlowTime = 0.46f;
	constexpr float AshWideEyes = 0.15f;
	constexpr float AshLastBlinkAt = 1.48f + AshRestTime;
	constexpr float AshEyesOffAt = 1.6f + AshRestTime;
	constexpr float AshEyesOffTime = 0.1f;
	/** The embers on the fresh pile die down over this long (pile time, from the crumble's end), flickering. */
	constexpr float AshEmberTime = 2.5f;
	// While the pile waits for the gust: a blink every this long (from AshBlinkAt), and after the first glance a slower look
	// round every this long (this long each, one way then the other), this many of them - the last well before the gust.
	constexpr float AshIdleBlinkEvery = 1.6f;
	constexpr float AshIdleGlanceEvery = 1.25f;
	constexpr float AshIdleGlanceTime = 0.5f;
	constexpr int32 AshIdleGlances = 3;
	/** The floor under the waiting pile is checked this often (s). */
	constexpr float AshFloorCheckInterval = 0.2f;
	/** DeathAshStart before the penguin has crumbled. (Not just any negative time: the floor broken away under the pile moves
	 *  its start back so the gust comes at once - with the pile's long rest that start lies before the look began.) */
	constexpr float AshNotCrumbled = -1.e6f;
	/** Whisked off this far downwind (cm, x its size) and a little up. */
	constexpr float AshEyesOffDrift = 70.f;
	/** The ghost rises from the pile this far into the eyes' going. */
	constexpr float AshSpiritShare = 0.5f;
	// Ash streaming off the pile while the gust blows it away: a puff every this long with full effects (density-scaled:
	// fewer on lower settings), flying downwind this fast and up a little; every third one a dark speck.
	constexpr float AshStreamInterval = 0.022f;
	constexpr float AshStreamMinSpeed = 260.f;
	constexpr float AshStreamMaxSpeed = 480.f;
	// (The puffs are lit: the sun warms them, so the darks lean a little blue to come out grey, not brown.)
	static const FLinearColor AshPuffColor(0.013f, 0.014f, 0.017f);
	static const FLinearColor AshSpeckColor(0.005f, 0.005f, 0.006f);
	/** Smoke wisps off the pile until the gust (a wisp every this long with full effects). */
	constexpr float AshWispInterval = 0.09f;
	/** The charred penguin's and its pile's smoke: dark soot grey (lit, it leans a little blue so the sun does not turn it brown). */
	static const FLinearColor SmokeColor(0.022f, 0.025f, 0.031f);
	/** The snow bomb blast's own cloud of smoke round the knock-out (lighter, over the head: the face stays in sight while it
	 *  chars), and the sooty ghost's tint. */
	static const FLinearColor BlastSmokeColor(0.14f, 0.13f, 0.13f);
	static const FLinearColor EmberColor(1.f, 0.45f, 0.1f);
	static const FLinearColor FeatherWhite(0.95f, 0.95f, 0.97f);
	static const FLinearColor FeatherBlack(0.03f, 0.035f, 0.05f);
	static const FLinearColor IceDeathColor(0.7f, 0.92f, 1.f);
	// The sea: bubbles (one every this long, for this long), a sinking copy of the penguin, and the ghost coming out of the
	// water this long after the splash.
	constexpr float BubbleInterval = 0.07f;
	constexpr float BubbleTime = 1.f;
	static const FLinearColor BubbleColor(0.75f, 0.9f, 1.f);
	static const FLinearColor SinkColor(0.3f, 0.55f, 0.85f);
	constexpr float SinkSpeed = -320.f;
	constexpr float SinkLife = 0.8f;
	constexpr float SeaSpiritTime = 0.35f;
	// The ghost: this share of the penguin's size, rising this fast (cm/s), swaying this far, living this long, this opaque
	// (M_Ghost's Opacity: its middle stays see-through, its outline and face lines show), turned this far from facing
	// the camera toward where the penguin faced. It rises slowly: a fast see-through mesh smears into stacked copies under
	// the temporal anti-aliasing. The sea's sinking copy is fainter.
	constexpr float SpiritSize = 0.75f;
	constexpr float SpiritRise = 85.f;
	constexpr float SpiritSway = 9.f;
	constexpr float SpiritLife = 1.7f;
	constexpr float SpiritOpacity = 1.f;
	constexpr float SinkOpacity = 0.6f;
	constexpr float SpiritTurn = 12.f;
	static const FLinearColor SpiritColor(0.78f, 0.9f, 1.f);
	/** A knock-out that was over before this machine saw the penguin (it joined late): its look counts as this old. */
	constexpr float DeathLookLate = 30.f;

	// Shrunk past the smallest size: it shrinks to nothing on the spot (spinning a little, wobbling) within this long, then
	// vanishes with a tiny pop (a puff, sparkles in the shrink ray's violet, a high squeak) - no ghost.
	constexpr float ShrunkVanishTime = 0.3f;
	constexpr float ShrunkSpinRate = 540.f;
	constexpr float ShrunkWobble = 0.12f;
	constexpr float ShrunkWobbleRate = 55.f;
	/** The mesh never quite reaches 0 (a zero scale breaks its bounds). */
	constexpr float ShrunkMinScale = 0.02f;
	static const FLinearColor ShrinkColor(0.85f, 0.4f, 1.f);
	static const FLinearColor GrowColor(0.45f, 1.f, 0.35f);
	// Grown past the biggest size: it puffs up (this much more at the end), wobbling faster and harder (from / to this much
	// and these rates in rad/s) and trembling, steam puffing off it, for this long - then it pops (OvergrownBlast).
	constexpr float OvergrownPopTime = 0.55f;
	constexpr float OvergrownSwell = 0.3f;
	constexpr float OvergrownWobbleFrom = 0.03f;
	constexpr float OvergrownWobbleTo = 0.12f;
	constexpr float OvergrownWobbleRateFrom = 18.f;
	constexpr float OvergrownWobbleRateTo = 60.f;
	constexpr float OvergrownTremble = 1.5f;   // cm (x size) to the sides
	constexpr float OvergrownSteamInterval = 0.06f;
	// The pop: a snow bomb's blast, bigger - its push reaches this far past the penguin's capsule (from its middle: its scaled
	// half height plus this), pushing like a bomb's outer zone, harder (cm/s, half of it at the edge; plus this much up), a
	// short stun; nobody is knocked out by it and it makes no crater (it was the ray's victim that blew up).
	constexpr float OvergrownBlastReach = 450.f;
	constexpr float OvergrownBlastPush = 1750.f;
	constexpr float OvergrownBlastLift = 600.f;
	constexpr float OvergrownBlastStun = 0.5f;
	constexpr float OvergrownPickupPush = 1400.f;
	static const FLinearColor ConfettiColors[] = { FLinearColor(1.f, 0.35f, 0.4f), FLinearColor(1.f, 0.85f, 0.25f), FLinearColor(0.4f, 0.8f, 1.f), GrowColor };

	/** The death looks that shape the mesh themselves (the others leave it to the squash & stretch, as before). */
	static bool DeathLookShapesMesh(EPenguinKOReason Reason)
	{
		return Reason == EPenguinKOReason::Stomp || Reason == EPenguinKOReason::Iceberg || Reason == EPenguinKOReason::Crushed
			|| Reason == EPenguinKOReason::Icicle || Reason == EPenguinKOReason::Bomb || Reason == EPenguinKOReason::Shrunk
			|| Reason == EPenguinKOReason::Overgrown;
	}

	/** The size knock-outs (a ray took the penguin past a size limit). */
	static bool IsSizeDeath(EPenguinKOReason Reason)
	{
		return Reason == EPenguinKOReason::Shrunk || Reason == EPenguinKOReason::Overgrown;
	}

	/** How long a knocked-out penguin stays visible before the server hides it (FinishKnockOut): a snow bomb's until it has
	 *  crumbled into its pile (charred, standing, crumbling: the pile stays on the effects actor). */
	static float KOHideTime(EPenguinKOReason Reason)
	{
		switch (Reason)
		{
		case EPenguinKOReason::Shrunk: return ShrunkVanishTime;
		case EPenguinKOReason::Overgrown: return OvergrownPopTime;
		case EPenguinKOReason::Bomb: return AshCrumbleAt + AshCrumbleTime + 0.05f;
		default: return 0.85f;
		}
	}

	/** The death looks that squash the penguin into a pancake facing the camera. */
	static bool IsPancakeLook(EPenguinKOReason Reason)
	{
		return Reason == EPenguinKOReason::Stomp || Reason == EPenguinKOReason::Iceberg || Reason == EPenguinKOReason::Crushed;
	}

	/** A pancake Wide wide across the screen spreads this much toward the camera (the mesh's forward axis). */
	static float PancakeDepth(float Wide)
	{
		return 1.f + (Wide - 1.f) * PancakeDepthShare;
	}

	/** A pancake T seconds after the squash: flattened at once (a bit past its rest shape), then jiggling to rest. */
	static void PancakeShape(float T, float& OutFlat, float& OutWide)
	{
		const float EndFlat = PancakeFlat * (1.f - PancakeJiggle);
		const float EndWide = PancakeWide * (1.f + 0.45f * PancakeJiggle);
		if (T < PancakeSquashTime)
		{
			const float Alpha = FMath::InterpEaseOut(0.f, 1.f, FMath::Clamp(T / PancakeSquashTime, 0.f, 1.f), 2.f);
			OutFlat = FMath::Lerp(1.f, EndFlat, Alpha);
			OutWide = FMath::Lerp(1.f, EndWide, Alpha);
			return;
		}
		const float U = T - PancakeSquashTime;
		const float Wobble = -PancakeJiggle * FMath::Exp(-PancakeJiggleDamp * U) * FMath::Cos(PancakeJiggleRate * U);
		OutFlat = PancakeFlat * (1.f + Wobble);
		OutWide = PancakeWide * (1.f - 0.45f * Wobble);
	}

	/** 0..1 over [From, From + Length] (0 before, 1 after). */
	static float Phase01(float T, float From, float Length)
	{
		return FMath::Clamp((T - From) / FMath::Max(Length, 1.e-3f), 0.f, 1.f);
	}

	/** How open the ash pile's eyes are in a blink that starts at From (1 open, shut halfway through it). */
	static float AshBlinkOpen(float T, float From)
	{
		const float U = (T - From) / AshBlinkTime;
		return U > 0.f && U < 1.f ? 1.f - FMath::Sin(U * UE_PI) : 1.f;
	}

	/** The ash pile P seconds after the penguin began crumbling into it (Out.Base, the face eyes and the embers are the
	 *  caller's): how far it has piled up and been blown away, and what its eyes do. LandP: the pile time the eyes came
	 *  down onto the pile's top (-1: still riding down on the face); Wind: the way the gust blows (+1 / -1); Size: the
	 *  penguin's size. */
	static void AshPileAt(float P, float LandP, float Wind, float Size, FPinguAshPile& Out)
	{
		Out.Size = Size;
		Out.Wind = Wind;
		Out.Grown = 1.f - FMath::Square(1.f - Phase01(P, AshGrowFrom * AshCrumbleTime, (1.f - AshGrowFrom) * AshCrumbleTime));
		Out.Blown = 1.f - FMath::Square(1.f - Phase01(P, AshGustAt, AshBlowTime)); // most of it goes at once, the rest trails
		// The eyes stay on the sinking face until it has come down to the pile's top, settle there with a little squash and
		// hop once; wide open when the gust comes; at the end whisked off downwind, shrinking.
		const float Landed = LandP >= 0.f ? P - LandP : -1.f;
		Out.EyeOnPile = Landed >= 0.f ? FMath::SmoothStep(0.f, AshEyesSettleTime, Landed) : 0.f;
		const float Hop = Landed >= 0.f ? Phase01(Landed, 0.f, AshEyesHopTime) : 0.f;
		const float HopUp = Hop > 0.f && Hop < 1.f ? FMath::Sin(Hop * UE_PI) : 0.f;
		const float Landing = Landed >= 0.f ? Phase01(Landed, 0.f, AshEyesSquashTime) : 0.f;
		const float LandSquash = Landing > 0.f && Landing < 1.f ? 1.f - AshEyesSquash * FMath::Sin(Landing * UE_PI) : 1.f;
		float Eyes = 1.f + AshWideEyes * FMath::SmoothStep(AshGustAt, AshGustAt + 0.06f, P);
		const float Off = Phase01(P, AshEyesOffAt, AshEyesOffTime);
		Eyes *= 1.f - Off;
		Out.Eyes = Eyes;
		Out.EyeShift = FVector(Wind * AshEyesOffDrift * Off * Off, 0.f, 12.f * Off + AshEyesHop * HopUp) * Size;
		// A blink from AshBlinkAt on, every AshIdleBlinkEvery while the pile waits for the gust (none in the gust), and the
		// last one before the eyes are whisked off.
		const float BlinkFrom = AshBlinkAt + FMath::FloorToFloat(FMath::Max(P - AshBlinkAt, 0.f) / AshIdleBlinkEvery) * AshIdleBlinkEvery;
		const float IdleBlinkOpen = BlinkFrom + AshBlinkTime < AshGustAt ? AshBlinkOpen(P, BlinkFrom) : 1.f;
		Out.EyeOpen = FMath::Min3(IdleBlinkOpen, AshBlinkOpen(P, AshLastBlinkAt), LandSquash);
		// A glance one way and the other; while it waits, now and then a slower look round (one way, then the other way
		// the next time); into the wind once the gust comes (where it comes from); back at the camera once the pile is gone.
		const float Glance = Phase01(P, AshGlanceAt, AshGlanceTime);
		float Around = Glance > 0.f && Glance < 1.f ? FMath::Sin(Glance * 2.f * UE_PI) : 0.f;
		const float Waiting = P - (AshGlanceAt + AshGlanceTime);
		if (Waiting > 0.f)
		{
			const int32 GlanceRound = FMath::FloorToInt(Waiting / AshIdleGlanceEvery);
			const float Into = Waiting - GlanceRound * AshIdleGlanceEvery - (AshIdleGlanceEvery - AshIdleGlanceTime); // each round ends with its look
			if (GlanceRound < AshIdleGlances && Into > 0.f)
			{
				Around = FMath::Sin(Into / AshIdleGlanceTime * UE_PI) * ((GlanceRound % 2) == 0 ? 0.8f : -0.8f);
			}
		}
		const float BlowEnd = AshGustAt + AshBlowTime;
		const float IntoWind = FMath::SmoothStep(AshGustAt - 0.02f, AshGustAt + 0.04f, P) * (1.f - FMath::SmoothStep(BlowEnd, BlowEnd + 0.06f, P));
		Out.Look = FMath::Lerp(Around, -Wind, IntoWind);
	}

	/** One bit of ash streaming off the pile downwind (a soft dark puff drifting up, or a speck tumbling), this machine only. */
	static void SpawnAshBit(APinguFX* FX, const FVector& From, float Wind, float Size, bool bSpeck)
	{
		FPinguParticleDesc Bit;
		Bit.Location = From;
		Bit.Velocity = FVector(Wind * FMath::FRandRange(AshStreamMinSpeed, AshStreamMaxSpeed), FMath::FRandRange(-40.f, 40.f), FMath::FRandRange(40.f, 190.f));
		Bit.Rotation = FRotator(FMath::FRandRange(0.f, 360.f), FMath::FRandRange(0.f, 360.f), 0.f);
		Bit.bFadeOut = true;
		if (bSpeck)
		{
			Bit.Mesh = EPinguFXMesh::Flake;
			Bit.Color = AshSpeckColor;
			Bit.Size = FMath::FRandRange(5.f, 8.f) * Size;
			Bit.EndSize = Bit.Size * 0.5f;
			Bit.Curve = EPinguSizeCurve::Linear;
			Bit.Life = FMath::FRandRange(0.6f, 0.9f);
			Bit.Drag = 0.8f;
			Bit.Gravity = 120.f;
			Bit.Sway = 60.f;
			Bit.Spin = FVector(FMath::FRandRange(-400.f, 400.f), FMath::FRandRange(-400.f, 400.f), 0.f);
		}
		else
		{
			Bit.Mesh = EPinguFXMesh::Puff;
			Bit.Color = AshPuffColor * FMath::FRandRange(0.85f, 1.15f);
			Bit.Size = FMath::FRandRange(12.f, 20.f) * Size;
			Bit.EndSize = 0.f;
			Bit.Curve = EPinguSizeCurve::Pop;
			Bit.Life = FMath::FRandRange(0.55f, 0.9f);
			Bit.Drag = 1.3f;
			Bit.Gravity = -70.f;
		}
		FX->Spawn(Bit);
	}

	static bool IsSizeRay(EPenguinAbility Ability)
	{
		return Ability == EPenguinAbility::ShrinkRay || Ability == EPenguinAbility::GrowRay;
	}

	struct FClipName { EPenguinClip Clip; const TCHAR* Name; };
	static const FClipName ClipNames[] = {
		{ EPenguinClip::Idle, TEXT("Idle") },
		{ EPenguinClip::WalkSlow, TEXT("Walk_Slow") },
		{ EPenguinClip::Walk, TEXT("Walk") },
		{ EPenguinClip::WalkFast, TEXT("Walk_Fast") },
		{ EPenguinClip::Run, TEXT("Run") },
		{ EPenguinClip::Sprint, TEXT("Sprint") },
		{ EPenguinClip::WalkIce, TEXT("Walk_Ice") },
		{ EPenguinClip::DashStart, TEXT("Dash_Start") },
		{ EPenguinClip::DashLoop, TEXT("Dash_Loop") },
		{ EPenguinClip::DashEnd, TEXT("Dash_End") },
		{ EPenguinClip::DashToFall, TEXT("Dash_To_Fall") },
		{ EPenguinClip::JumpStart, TEXT("Jump_Start") },
		{ EPenguinClip::JumpStartRun, TEXT("Jump_Start_Run") },
		{ EPenguinClip::JumpLoop, TEXT("Jump_Loop") },
		{ EPenguinClip::JumpApex, TEXT("Jump_Apex") },
		{ EPenguinClip::DoubleJump, TEXT("DoubleJump") },
		{ EPenguinClip::FallLoop, TEXT("Fall_Loop") },
		{ EPenguinClip::Land, TEXT("Land") },
		{ EPenguinClip::LandHard, TEXT("Land_Hard") },
		{ EPenguinClip::SlideStart, TEXT("Slide_Start") },
		{ EPenguinClip::Slide, TEXT("Slide") },
		{ EPenguinClip::SlideEnd, TEXT("Slide_End") },
		{ EPenguinClip::Slip, TEXT("Slip") },
		{ EPenguinClip::SlapQuick, TEXT("Slap_Quick") },
		{ EPenguinClip::SlapHeavy, TEXT("Slap_Heavy") },
		{ EPenguinClip::SlapSpin, TEXT("Slap_Spin") },
		{ EPenguinClip::ThrowAim, TEXT("Throw_Aim") },
		{ EPenguinClip::Throw, TEXT("Throw") },
		{ EPenguinClip::ThrowToss, TEXT("Throw_Toss") },
		{ EPenguinClip::Waddle, TEXT("Waddle") },
		{ EPenguinClip::Jog, TEXT("Jog") },
		{ EPenguinClip::RunFast, TEXT("RunFast") },
		{ EPenguinClip::Skid, TEXT("Skid") },
		{ EPenguinClip::Stop, TEXT("Stop") },
		{ EPenguinClip::IdleLook, TEXT("Idle_Look") },
		{ EPenguinClip::IdleFlap, TEXT("Idle_Flap") },
		{ EPenguinClip::IdleTap, TEXT("Idle_Tap") },
		{ EPenguinClip::FallFlail, TEXT("Fall_Flail") },
		{ EPenguinClip::Hit, TEXT("Hit") },
		{ EPenguinClip::Tumble, TEXT("Tumble") },
		{ EPenguinClip::KO, TEXT("KO") },
		{ EPenguinClip::Frozen, TEXT("Frozen") },
		{ EPenguinClip::Victory, TEXT("Victory") },
		{ EPenguinClip::Cast, TEXT("Cast") },
		{ EPenguinClip::Shield, TEXT("Shield") },
		{ EPenguinClip::GroundPound, TEXT("GroundPound") },
		{ EPenguinClip::ThrowUp, TEXT("Throw_Up") },
		{ EPenguinClip::ThrowUpL, TEXT("Throw_Up_L") },
		{ EPenguinClip::Toss, TEXT("Toss") },
		{ EPenguinClip::TossL, TEXT("Toss_L") },
		{ EPenguinClip::Swat, TEXT("Swat") },
		{ EPenguinClip::SwatL, TEXT("Swat_L") },
		{ EPenguinClip::Spin, TEXT("Spin") },
		{ EPenguinClip::Summon, TEXT("Summon") },
		{ EPenguinClip::Taunt, TEXT("Taunt") },
		{ EPenguinClip::Noot, TEXT("Noot") },
		{ EPenguinClip::Climb, TEXT("Climb") },
	};
	static_assert(UE_ARRAY_COUNT(ClipNames) == static_cast<int32>(EPenguinClip::Count), "clip table out of sync");

	// Beanie felt / cuff colours per colour index (linear): the player slots 0..9 (a penguin keeps its own in a team match).
	// Red, blue, green, yellow, purple, orange, pink, cyan, white, brown.
	static const FLinearColor FeltColors[] = {
		FLinearColor(0.32f, 0.012f, 0.014f), FLinearColor(0.012f, 0.07f, 0.42f), FLinearColor(0.02f, 0.25f, 0.03f), FLinearColor(0.62f, 0.36f, 0.01f),
		FLinearColor(0.16f, 0.041f, 0.42f), FLinearColor(0.52f, 0.117f, 0.006f), FLinearColor(0.51f, 0.086f, 0.25f), FLinearColor(0.007f, 0.28f, 0.34f),
		FLinearColor(0.62f, 0.66f, 0.7f), FLinearColor(0.152f, 0.061f, 0.021f) };
	static const FLinearColor CuffColors[] = {
		FLinearColor(0.45f, 0.012f, 0.014f), FLinearColor(0.02f, 0.11f, 0.6f), FLinearColor(0.03f, 0.38f, 0.045f), FLinearColor(0.8f, 0.5f, 0.015f),
		FLinearColor(0.23f, 0.058f, 0.59f), FLinearColor(0.74f, 0.165f, 0.009f), FLinearColor(0.73f, 0.123f, 0.36f), FLinearColor(0.009f, 0.39f, 0.48f),
		FLinearColor(0.75f, 0.79f, 0.84f), FLinearColor(0.216f, 0.087f, 0.029f) };
	// Bright UI colours (names, rings, the feed).
	static const FLinearColor UIColors[] = {
		FLinearColor(0.95f, 0.22f, 0.2f), FLinearColor(0.25f, 0.55f, 1.f), FLinearColor(0.3f, 0.85f, 0.3f), FLinearColor(1.f, 0.8f, 0.15f),
		FLinearColor(0.62f, 0.35f, 1.f), FLinearColor(1.f, 0.48f, 0.1f), FLinearColor(1.f, 0.4f, 0.7f), FLinearColor(0.15f, 0.85f, 0.95f),
		FLinearColor(0.88f, 0.92f, 0.97f), FLinearColor(0.72f, 0.47f, 0.28f) };
	// The portraits' beanie layer (white felt shading) times this: the beanie colour, vivid (sRGB D93A33, 2264D8, 2FAE3A,
	// F2B515, 8E4AD9, F07A1A, EE6AAE, 1BB5C6, E6ECF3, 8A5A34).
	static const FLinearColor PortraitTints[] = {
		FLinearColor(0.694f, 0.042f, 0.033f), FLinearColor(0.016f, 0.127f, 0.687f), FLinearColor(0.028f, 0.423f, 0.042f), FLinearColor(0.888f, 0.462f, 0.007f),
		FLinearColor(0.27f, 0.068f, 0.694f), FLinearColor(0.871f, 0.195f, 0.01f), FLinearColor(0.855f, 0.144f, 0.423f), FLinearColor(0.011f, 0.462f, 0.565f),
		FLinearColor(0.791f, 0.839f, 0.896f), FLinearColor(0.254f, 0.102f, 0.034f) };
	static_assert(UE_ARRAY_COUNT(FeltColors) == PinguMaxPenguins && UE_ARRAY_COUNT(CuffColors) == PinguMaxPenguins
		&& UE_ARRAY_COUNT(UIColors) == PinguMaxPenguins && UE_ARRAY_COUNT(PortraitTints) == PinguMaxPenguins, "a colour per slot");

	static int32 WrapSlot(int32 Slot)
	{
		return ((Slot % PinguMaxPenguins) + PinguMaxPenguins) % PinguMaxPenguins;
	}
}

APenguinCharacter::APenguinCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UPenguinMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	using namespace PenguinCharacterPrivate;

	PrimaryActorTick.bCanEverTick = true;
	PenguinMovement = Cast<UPenguinMovementComponent>(GetCharacterMovement());

	// LAN: every machine always has every penguin (a knocked-out, hidden one too: it is back next round), with the
	// standard character movement replication for the penguins the server simulates.
	bReplicates = true;
	SetReplicatingMovement(true);
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(NetUpdateRate);
	SetMinNetUpdateFrequency(MinNetUpdateRate);

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	UCapsuleComponent* Capsule = GetCapsuleComponent();
	Capsule->InitCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
	// Penguins pass through each other; hits come from abilities.
	Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	USkeletalMeshComponent* SkelMesh = GetMesh();
	PenguinMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/Penguin/Character/SK_Penguin.SK_Penguin")));
	SkelMesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	SkelMesh->SetAnimInstanceClass(UPenguinAnimInstance::StaticClass());
	SkelMesh->SetRelativeLocation(FVector(0.f, 0.f, -CapsuleHalfHeight - FeetHeight));
	SkelMesh->SetRelativeRotation(FRotator(0.f, -90.f + CameraPeekYaw, 0.f));
	SkelMesh->SetRelativeScale3D(FVector(MeshScale));
	SkelMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	SkelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	AbilityComponent = CreateDefaultSubobject<UPenguinAbilityComponent>(TEXT("Abilities"));
	BotComponent = CreateDefaultSubobject<UPenguinBotComponent>(TEXT("Bot"));
	TraitComponent = CreateDefaultSubobject<UPenguinTraitComponent>(TEXT("Traits"));

	ShieldBubble = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ShieldBubble"));
	ShieldBubble->SetupAttachment(Capsule);
	ShieldBubble->SetRelativeLocation(FVector(0.f, 0.f, 18.f));
	ShieldBubble->SetRelativeScale3D(FVector(UPenguinAbilityComponent::ShieldRadius * 2.f / 100.f));
	ShieldBubble->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ShieldBubble->SetCastShadow(false);
	ShieldBubble->SetHiddenInGame(true);

	IceBlock = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("IceBlock"));
	IceBlock->SetupAttachment(Capsule);
	IceBlock->SetRelativeLocation(FVector(0.f, 0.f, 4.f));
	IceBlock->SetRelativeScale3D(FVector(0.95f, 0.95f, 1.28f));
	IceBlock->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	IceBlock->SetCastShadow(false);
	IceBlock->SetHiddenInGame(true);

	IceFootL = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("IceFootL"));
	IceFootR = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("IceFootR"));
	for (UStaticMeshComponent* Foot : { IceFootL.Get(), IceFootR.Get() })
	{
		Foot->SetupAttachment(SkelMesh); // moved onto the Foot_L / Foot_R bones once the penguin mesh is loaded
		Foot->SetUsingAbsoluteScale(true);
		Foot->SetWorldScale3D(FVector(0.36f, 0.38f, 0.17f));
		Foot->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Foot->SetCastShadow(false);
		Foot->SetHiddenInGame(true);
	}

	// The noot horn sticks straight out of the beak: on the Body bone (moved there once the penguin mesh is loaded), its
	// size its own (the bones carry the FBX's unit scale).
	NootHorn = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("NootHorn"));
	NootHorn->SetupAttachment(SkelMesh);
	NootHorn->SetUsingAbsoluteScale(true);
	NootHorn->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NootHorn->SetGenerateOverlapEvents(false);
	NootHorn->SetHiddenInGame(true);

	DizzyStars = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("DizzyStars"));
	DizzyStars->SetupAttachment(Capsule);
	DizzyStars->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DizzyStars->SetGenerateOverlapEvents(false);
	DizzyStars->SetCanEverAffectNavigation(false);
	DizzyStars->SetCastShadow(false);
	DizzyStars->NumCustomDataFloats = 5; // like the effect particles (M_FX): R, G, B, emissive, opacity
	DizzyStars->SetHiddenInGame(true);

	for (const FClipName& Entry : ClipNames)
	{
		const FString Path = FString::Printf(TEXT("/Game/Penguin/Anims/A_%s.A_%s"), Entry.Name, Entry.Name);
		AnimClips.Add(Entry.Clip, TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(Path)));
	}
}

void APenguinCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Penguins stand on the island's collision boxes, which every machine builds for itself at runtime: they cannot be
	// referenced over the network. With the engine's "based movement" replicated, every change of the box under a
	// penguin's feet logged a NetGUID warning and LAN clients stopped simulating that penguin (its base never resolved).
	// The positions replicate as absolute positions instead (ReplicatedMovement), which is all a static island needs.
	DISABLE_REPLICATED_PRIVATE_PROPERTY(ACharacter, ReplicatedBasedMovement);

	DOREPLIFETIME(APenguinCharacter, PlayerSlot);
	DOREPLIFETIME(APenguinCharacter, DisplayName);
	DOREPLIFETIME(APenguinCharacter, Team);
	// The owner of a remote player's penguin gets these through the Client RPCs, in order with its own simulation.
	DOREPLIFETIME_CONDITION(APenguinCharacter, bKnockedOut, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APenguinCharacter, bKOHidden, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APenguinCharacter, KOReason, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APenguinCharacter, bVictory, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APenguinCharacter, SizeScale, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APenguinCharacter, SizeTarget, COND_SkipOwner);
	DOREPLIFETIME(APenguinCharacter, SizeTimeLeft);
	DOREPLIFETIME(APenguinCharacter, BaseSize);
	DOREPLIFETIME(APenguinCharacter, RepActionCount);
	DOREPLIFETIME(APenguinCharacter, RepActionClip);
	DOREPLIFETIME(APenguinCharacter, RepActionRate);
	DOREPLIFETIME(APenguinCharacter, RepActionAim);
	// The owner aims by itself.
	DOREPLIFETIME_CONDITION(APenguinCharacter, RepAimPoint, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APenguinCharacter, bAimValid, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(APenguinCharacter, bAimFromStick, COND_SkipOwner);
	// The snowball combo's pop above its victim (everybody sees it, the victim's own player too).
	DOREPLIFETIME(APenguinCharacter, ComboPopCount);
	DOREPLIFETIME(APenguinCharacter, ComboPopSerial);
}

void APenguinCharacter::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	LoadPenguinAssets(false); // shows the penguin in the editor and in the Blueprint viewport
}

void APenguinCharacter::PostInitializeComponents()
{
	LoadPenguinAssets(true);
	Super::PostInitializeComponents();
}

void APenguinCharacter::LoadPenguinAssets(bool bLoadClips)
{
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (SkelMesh && !SkelMesh->GetSkeletalMeshAsset() && !PenguinMesh.IsNull())
	{
		if (USkeletalMesh* MeshAsset = PenguinMesh.LoadSynchronous())
		{
			SkelMesh->SetSkeletalMeshAsset(MeshAsset);
		}
	}

	if (bLoadClips)
	{
		LoadedClips.Reset();
		LoadedClips.SetNum(static_cast<int32>(EPenguinClip::Count));
		for (const TPair<EPenguinClip, TSoftObjectPtr<UAnimSequence>>& Pair : AnimClips)
		{
			const int32 Index = static_cast<int32>(Pair.Key);
			if (LoadedClips.IsValidIndex(Index) && !Pair.Value.IsNull())
			{
				LoadedClips[Index] = Pair.Value.LoadSynchronous();
			}
		}

		// Status visuals (loaded by path so nothing is hard-referenced by the class default object).
		if (ShieldBubble && !ShieldBubble->GetStaticMesh())
		{
			ShieldBubble->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Bubble.SM_Bubble"), nullptr, LOAD_Quiet | LOAD_NoWarn));
			// The shell's own bubble (M_ShieldBubble: shaded, a bright rim, more solid); the plain M_Bubble if it is missing.
			UMaterialInterface* Bubble = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_ShieldBubble.M_ShieldBubble"), nullptr, LOAD_Quiet | LOAD_NoWarn);
			Bubble = Bubble ? Bubble : LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Bubble.M_Bubble"), nullptr, LOAD_Quiet | LOAD_NoWarn);
			if (Bubble)
			{
				ShieldBubble->SetMaterial(0, Bubble);
			}
		}
		if (IceBlock && !IceBlock->GetStaticMesh())
		{
			IceBlock->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Environment/SM_Block.SM_Block"), nullptr, LOAD_Quiet | LOAD_NoWarn));
			if (UMaterialInterface* Ice = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_IceBlock.M_IceBlock"), nullptr, LOAD_Quiet | LOAD_NoWarn))
			{
				IceBlock->SetMaterial(0, Ice);
			}
		}
		// Ice chunks on the feet: a deeper blue and more opaque than the frozen block so they read on white snow.
		UMaterialInstanceDynamic* FeetIce = nullptr;
		if (UMaterialInterface* Ice = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_IceBlock.M_IceBlock"), nullptr, LOAD_Quiet | LOAD_NoWarn))
		{
			FeetIce = UMaterialInstanceDynamic::Create(Ice, this);
			FeetIce->SetScalarParameterValue(TEXT("Opacity"), 2.2f);
			FeetIce->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.25f, 0.65f, 1.f));
		}
		for (UStaticMeshComponent* Foot : { IceFootL.Get(), IceFootR.Get() })
		{
			if (!Foot)
			{
				continue;
			}
			if (!Foot->GetStaticMesh())
			{
				Foot->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Environment/SM_Block.SM_Block"), nullptr, LOAD_Quiet | LOAD_NoWarn));
				if (FeetIce)
				{
					Foot->SetMaterial(0, FeetIce);
				}
			}
			// Onto the foot bones now that the mesh exists (attaching earlier only warns that there is no skeletal mesh yet).
			const FName Bone = Foot == IceFootL.Get() ? FName(TEXT("Foot_L")) : FName(TEXT("Foot_R"));
			if (SkelMesh && SkelMesh->GetSkeletalMeshAsset() && Foot->GetAttachSocketName() != Bone)
			{
				Foot->AttachToComponent(SkelMesh, FAttachmentTransformRules::KeepRelativeTransform, Bone);
			}
		}
		FitIceFeet(SkelMesh);

		if (NootHorn)
		{
			if (!NootHorn->GetStaticMesh())
			{
				NootHorn->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Penguin/Horn/SM_NootHorn.SM_NootHorn"), nullptr, LOAD_Quiet | LOAD_NoWarn));
			}
			const FName Bone(TEXT("Body"));
			if (SkelMesh && SkelMesh->GetSkeletalMeshAsset() && SkelMesh->GetBoneIndex(Bone) != INDEX_NONE && NootHorn->GetAttachSocketName() != Bone)
			{
				NootHorn->AttachToComponent(SkelMesh, FAttachmentTransformRules::KeepRelativeTransform, Bone);
			}
			FitNootHorn(SkelMesh);
		}

		// Dizzy stars: the effects' star with its material, gold.
		if (DizzyStars && !DizzyStars->GetStaticMesh())
		{
			DizzyStars->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Star.SM_Star"), nullptr, LOAD_Quiet | LOAD_NoWarn));
			if (UMaterialInterface* FX = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_FX.M_FX"), nullptr, LOAD_Quiet | LOAD_NoWarn))
			{
				DizzyStars->SetMaterial(0, FX);
			}
			DizzyStars->SetNumCustomDataFloats(5);
			DizzyStars->ClearInstances();
			const float Gold[5] = { 1.f, 0.78f, 0.12f, 1.2f, 1.f };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const int32 Instance = DizzyStars->AddInstance(FTransform(FRotator::ZeroRotator, FVector(0.f, 0.f, PenguinCharacterPrivate::DizzyHeight),
					FVector(PenguinCharacterPrivate::DizzyStarScale)), false);
				DizzyStars->SetCustomData(Instance, MakeArrayView(Gold, 5), false);
			}
		}
	}
}

void APenguinCharacter::FitIceFeet(USkeletalMeshComponent* SkelMesh)
{
	using namespace PenguinCharacterPrivate;
	const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
	if (!Asset || !IceFootL || !IceFootR)
	{
		return;
	}
	for (const TCHAR* Bone : { TEXT("Root"), TEXT("Body"), TEXT("Foot_L"), TEXT("Foot_R"), TEXT("Eye_Target") })
	{
		if (SkelMesh->GetBoneIndex(FName(Bone)) == INDEX_NONE)
		{
			return; // not the penguin rig: the chunks stay on the bones as they are
		}
	}
	const auto RefLocation = [Asset](const TCHAR* Bone) { return Asset->GetComposedRefPoseMatrix(FName(Bone)).GetOrigin(); };
	// Blender's armature space -> the mesh's (component space, reference pose): the bone heads give its axes and units.
	const FVector Origin = RefLocation(TEXT("Root"));
	const FVector MapX = (RefLocation(TEXT("Foot_R")) - RefLocation(TEXT("Foot_L"))) / static_cast<float>(BlendFootRHead.X - BlendFootLHead.X);
	const FVector MapZ = (RefLocation(TEXT("Body")) - Origin) / static_cast<float>(BlendBodyHead.Z);
	const FVector MapY = (RefLocation(TEXT("Eye_Target")) - RefLocation(TEXT("Body")) - MapZ * static_cast<float>(BlendEyeTargetHead.Z - BlendBodyHead.Z))
		/ static_cast<float>(BlendEyeTargetHead.Y - BlendBodyHead.Y);
	const auto ToMesh = [&](const FVector& Blend) { return Origin + MapX * Blend.X + MapY * Blend.Y + MapZ * Blend.Z; };
	// The chunks line up with the mesh's axes in the reference pose (the feet are modelled that way), then move with the
	// foot bones: across the foot, along it, up.
	const FVector AxisX = MapX.GetSafeNormal();
	const FVector AxisY = (MapZ.GetSafeNormal() ^ AxisX).GetSafeNormal();
	const FQuat ChunkRotation = FMatrix(AxisX, AxisY, AxisX ^ AxisY, FVector::ZeroVector).ToQuat();
	const struct
	{
		UStaticMeshComponent* Chunk;
		const TCHAR* Bone;
		FVector Min;
		FVector Max;
	} Feet[2] = { { IceFootL.Get(), TEXT("Foot_L"), BlendIceFootLMin, BlendIceFootLMax }, { IceFootR.Get(), TEXT("Foot_R"), BlendIceFootRMin, BlendIceFootRMax } };
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const FTransform BoneRef(Asset->GetComposedRefPoseMatrix(FName(Feet[Side].Bone)));
		const FVector Center = ToMesh((Feet[Side].Min + Feet[Side].Max) * 0.5f);
		Feet[Side].Chunk->SetRelativeLocationAndRotation(BoneRef.InverseTransformPosition(Center), BoneRef.GetRotation().Inverse() * ChunkRotation);
		const FVector Size = Feet[Side].Max - Feet[Side].Min;
		IceFootSize[Side] = FVector(MapX.Size() * Size.X, MapY.Size() * Size.Y, MapZ.Size() * Size.Z);
	}
	static bool bLogged = false;
	if (!bLogged)
	{
		bLogged = true;
		UE_LOG(LogPingu, Display, TEXT("[ICEFEET] Blender -> mesh: x %s, y %s, z %s, origin %s; left chunk %s in its bone, size %s"),
			*MapX.ToString(), *MapY.ToString(), *MapZ.ToString(), *Origin.ToString(), *IceFootL->GetRelativeLocation().ToString(), *IceFootSize[0].ToString());
	}
}

void APenguinCharacter::FitNootHorn(USkeletalMeshComponent* SkelMesh)
{
	using namespace PenguinCharacterPrivate;
	bHornFitted = false;
	const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
	if (!Asset || !NootHorn || !NootHorn->GetStaticMesh())
	{
		return; // no horn: the noot honks without it
	}
	for (const TCHAR* Bone : { TEXT("Root"), TEXT("Body"), TEXT("Eye_Target") })
	{
		if (SkelMesh->GetBoneIndex(FName(Bone)) == INDEX_NONE)
		{
			return; // not the penguin rig
		}
	}
	// Blender's armature space -> the mesh's (as in FitIceFeet; the throat is on the middle line, x = 0).
	const auto RefLocation = [Asset](const TCHAR* Bone) { return Asset->GetComposedRefPoseMatrix(FName(Bone)).GetOrigin(); };
	const FVector Origin = RefLocation(TEXT("Root"));
	const FVector MapZ = (RefLocation(TEXT("Body")) - Origin) / static_cast<float>(BlendBodyHead.Z);
	const FVector MapY = (RefLocation(TEXT("Eye_Target")) - RefLocation(TEXT("Body")) - MapZ * static_cast<float>(BlendEyeTargetHead.Z - BlendBodyHead.Z))
		/ static_cast<float>(BlendEyeTargetHead.Y - BlendBodyHead.Y);
	const FVector Pivot = Origin + MapY * static_cast<float>(BlendHornPivot.Y) + MapZ * static_cast<float>(BlendHornPivot.Z);
	const FTransform BoneRef(Asset->GetComposedRefPoseMatrix(FName(TEXT("Body"))));
	HornPivot = BoneRef.InverseTransformPosition(Pivot);
	HornForward = (-MapY).GetSafeNormal(); // Pingu.blend looks along -y
	HornUp = MapZ.GetSafeNormal();
	HornUnit = static_cast<float>(MapZ.Size()) / 100.f;
	bHornFitted = !HornForward.IsNearlyZero() && !HornUp.IsNearlyZero() && HornUnit > 0.f && NootHorn->GetAttachSocketName() == FName(TEXT("Body"));
	// Straight out of the beak, at a right angle to the body: the horn's +X along the beak, +Z up the body (rest pose).
	const FVector Side = (HornUp ^ HornForward).GetSafeNormal();
	HornRotation = BoneRef.GetRotation().Inverse() * FMatrix(HornForward, Side, HornForward ^ Side, FVector::ZeroVector).ToQuat();
	NootHorn->SetRelativeLocationAndRotation(HornPivot, HornRotation);
	static bool bLogged = false;
	if (!bLogged)
	{
		bLogged = true;
		UE_LOG(LogPingu, Display, TEXT("[NOOTHORN] throat %s in the Body bone (mesh %s), forward %s, up %s, unit %.3f, mesh extent %s"),
			*HornPivot.ToString(), *Pivot.ToString(), *HornForward.ToString(), *HornUp.ToString(), HornUnit,
			*NootHorn->GetStaticMesh()->GetBounds().BoxExtent.ToString());
	}
}

void APenguinCharacter::UpdateNootHorn(float Dt)
{
	using namespace PenguinCharacterPrivate;
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (!NootHorn || !SkelMesh)
	{
		return;
	}
	if (HornTime >= 0.f)
	{
		HornTime += Dt;
		const bool bFrozen = PenguinMovement && PenguinMovement->IsFrozen();
		if (HornTime >= HornDuration || bKnockedOut || IsHidden() || bFrozen || !bHornFitted)
		{
			HornTime = -1.f;
		}
	}
	const bool bShow = HornTime >= 0.f;
	if (bShow != bHornShown)
	{
		bHornShown = bShow;
		NootHorn->SetHiddenInGame(!bShow);
	}
	if (!bShow)
	{
		return;
	}
	float Length = 1.f;
	float Width = 1.f;
	NootHornShape(HornTime, Length, Width);
	// It sticks straight out of the beak (the body turns to aim it, see GetHonkPitch), buzzing round its own axis a
	// little; sized with the penguin.
	const float Wobble = FMath::DegreesToRadians(3.f) * NootHornBuzz(HornTime) * FMath::Sin(HornTime * 2.f * UE_PI * 13.f);
	NootHorn->SetRelativeRotation(HornRotation * FQuat(FVector(1.f, 0.f, 0.f), Wobble));
	NootHorn->SetWorldScale3D(FVector(Length, Width, Width) * HornUnit * static_cast<float>(SkelMesh->GetComponentScale().Z));
}

float APenguinCharacter::GetHonkWeight() const
{
	// The body turns into the noot as the horn shoots out and back as it slips in.
	return HornTime < 0.f ? 0.f : FMath::SmoothStep(0.f, 0.07f, HornTime) * (1.f - FMath::SmoothStep(0.40f, PenguinCharacterPrivate::HornDuration, HornTime));
}

float APenguinCharacter::GetHonkPitch() const
{
	using namespace PenguinCharacterPrivate;
	// Up / down from the side the penguin turned to (aimed straight up or down it keeps its side).
	const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(HornAim.Y), FMath::Abs(static_cast<float>(HornAim.X))));
	return FMath::Clamp(Pitch, -HonkMaxDown, HonkMaxUp);
}

void APenguinCharacter::UpdateDizzyStars(float Dt)
{
	using namespace PenguinCharacterPrivate;
	if (!DizzyStars || !PenguinMovement)
	{
		return;
	}
	// The stun is known on every machine (the movement state); a long one (the slap's) makes the penguin dizzy until it ends.
	const float StunLeft = PenguinMovement->GetStunRemaining();
	const bool bCanBe = !bKnockedOut && !IsHidden() && !PenguinMovement->IsFrozen() && StunLeft > 0.05f;
	const bool bNow = bCanBe && (bDizzy || StunLeft > DizzyMinStun);
	if (bNow != bDizzy)
	{
		bDizzy = bNow;
		DizzyTime = 0.f;
		DizzyStars->SetHiddenInGame(!bDizzy);
	}
	if (!bDizzy || DizzyStars->GetInstanceCount() < 3)
	{
		return;
	}
	DizzyTime += Dt;
	// Round the head (on the capsule: they grow with the penguin), bobbing a little, each one spinning.
	TArray<FTransform, TInlineAllocator<3>> Transforms;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const float Angle = DizzyTime * DizzyTurns * 2.f * UE_PI + Index * 2.f * UE_PI / 3.f;
		const FVector Where(FMath::Cos(Angle) * DizzyRadius, FMath::Sin(Angle) * DizzyRadius, DizzyHeight + 5.f * FMath::Sin(Angle * 2.f));
		Transforms.Add(FTransform(FRotator(0.f, DizzyTime * 420.f + Index * 60.f, 0.f), Where, FVector(DizzyStarScale)));
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		DizzyStars->UpdateInstanceTransform(Index, Transforms[Index], false, false);
	}
	APinguFX::MarkInstancesChanged(DizzyStars);
}

float APenguinCharacter::GetVisualFacing() const
{
	// Honking, the penguin turns to the side the noot goes (its movement keeps its own facing).
	if (HornTime >= 0.f && FMath::Abs(HornAim.X) > 0.2f)
	{
		return FMath::Sign(static_cast<float>(HornAim.X));
	}
	// Charging or throwing an icicle, it turns to the side it aims at (so the icicle leaves from the front, not out of the
	// back), even running the other way; the movement's own facing comes back when the follow-through is over. Every machine
	// works this out from the shown throw and the aim it knows (the owner's own, the others' replicated).
	if (AbilityComponent)
	{
		const float IcicleSide = AbilityComponent->GetIcicleFacing();
		if (IcicleSide != 0.f)
		{
			return IcicleSide;
		}
	}
	return GetFacing();
}

int32 APenguinCharacter::GetNumLoadedClips() const
{
	int32 Count = 0;
	for (const TObjectPtr<UAnimSequence>& Clip : LoadedClips)
	{
		Count += Clip ? 1 : 0;
	}
	return Count;
}

void APenguinCharacter::BeginPlay()
{
	Super::BeginPlay();
	// Play In Editor spawns the player at the viewport camera, turned like the camera: undo that.
	KeepActorUnrotated();
	if (SpawnLocation.IsZero())
	{
		SpawnLocation = GetActorLocation();
	}
	BaseMeshScale = GetMesh()->GetRelativeScale3D();
	MeshYaw = GetMesh()->GetRelativeRotation().Yaw;
	SyncSeenCounters();
	if (GetNumLoadedClips() == 0)
	{
		UE_LOG(LogPingu, Warning, TEXT("%s has no animation clips. Run Tools/ue_setup_project.py to import the penguin."), *GetName());
	}
	ApplyBeanieColor();
	ApplyOutlineSetting(); // hides the ink outline (every penguin: respawns, late joiners, the menu's demo)
}

void APenguinCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	KeepActorUnrotated();
}

void APenguinCharacter::FaceRotation(FRotator NewControlRotation, float DeltaTime)
{
	// Never follow the control rotation: the capsule stays unrotated, the mesh yaw shows the facing.
}

void APenguinCharacter::KeepActorUnrotated()
{
	// A Blueprint child may have switched these on; with them the controller would turn the whole penguin.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	SetActorRotation(FRotator::ZeroRotator, ETeleportType::TeleportPhysics);
}

void APenguinCharacter::SyncSeenCounters()
{
	if (!PenguinMovement)
	{
		return;
	}
	SeenJump = PenguinMovement->JumpEvents;
	SeenAirJump = PenguinMovement->AirJumpEvents;
	SeenLand = PenguinMovement->LandEvents;
	SeenDash = PenguinMovement->DashEvents;
	SeenStompLand = PenguinMovement->StompLandEvents;
	SeenHit = PenguinMovement->HitEvents;
	SeenClimb = PenguinMovement->ClimbEvents;
	SeenPadBounce = PenguinMovement->PadBounceEvents;
	SeenSlide = PenguinMovement->SlideEvents;
}

// ---------------------------------------------------------------------------- network roles

bool APenguinCharacter::IsLocallySimulated() const
{
	if (GetLocalRole() == ROLE_AutonomousProxy)
	{
		return true; // a LAN client's own penguin
	}
	if (!HasAuthority())
	{
		return false; // somebody else's penguin on a LAN client
	}
	return !IsRemotePlayerPenguin();
}

bool APenguinCharacter::IsRemotePlayerPenguin() const
{
	if (!HasAuthority())
	{
		return false;
	}
	const AController* PawnController = GetController();
	return PawnController && PawnController->IsPlayerController() && !PawnController->IsLocalController();
}

// ---------------------------------------------------------------------------- identity

FLinearColor APenguinCharacter::GetSlotColor(int32 Slot)
{
	return PenguinCharacterPrivate::UIColors[PenguinCharacterPrivate::WrapSlot(Slot)];
}

FLinearColor APenguinCharacter::GetBeanieTint(int32 ColorIndex)
{
	return PenguinCharacterPrivate::PortraitTints[PenguinCharacterPrivate::WrapSlot(ColorIndex)];
}

FText APenguinCharacter::GetTeamName(int32 InTeam)
{
	switch (InTeam)
	{
	case 0: return NSLOCTEXT("Pingu", "TeamRed", "RED");
	case 1: return NSLOCTEXT("Pingu", "TeamBlue", "BLUE");
	case 2: return NSLOCTEXT("Pingu", "TeamGreen", "GREEN");
	case 3: return NSLOCTEXT("Pingu", "TeamYellow", "YELLOW");
	case 4: return NSLOCTEXT("Pingu", "TeamPurple", "PURPLE");
	case 5: return NSLOCTEXT("Pingu", "TeamOrange", "ORANGE");
	case 6: return NSLOCTEXT("Pingu", "TeamPink", "PINK");
	case 7: return NSLOCTEXT("Pingu", "TeamCyan", "CYAN");
	case 8: return NSLOCTEXT("Pingu", "TeamWhite", "WHITE");
	case 9: return NSLOCTEXT("Pingu", "TeamBrown", "BROWN");
	default: return NSLOCTEXT("Pingu", "TeamNone", "NO TEAM");
	}
}

void APenguinCharacter::SetPlayerSlot(int32 Slot, const FString& Name)
{
	if (!HasAuthority())
	{
		return; // the server's slot and name replicate (OnRep_PlayerSlot colours the beanie)
	}
	PlayerSlot = PenguinCharacterPrivate::WrapSlot(Slot);
	DisplayName = Name;
	ApplyBeanieColor();
}

void APenguinCharacter::SetTeam(int32 InTeam)
{
	if (!HasAuthority())
	{
		return; // replicates (OnRep_PlayerSlot colours the beanie)
	}
	const int32 NewTeam = InTeam >= 0 && InTeam < PinguMaxTeams ? InTeam : -1;
	if (Team != NewTeam)
	{
		Team = NewTeam; // (the beanie keeps the penguin's own colour)
		ForceNetUpdate();
	}
}

bool APenguinCharacter::IsTeammate(const APenguinCharacter* Other) const
{
	return Other && Other != this && Team >= 0 && Other->Team == Team;
}

void APenguinCharacter::SetHatColorIndex(int32 ColorIndex)
{
	const int32 Wanted = ColorIndex >= 0 ? ColorIndex : -1;
	if (HatColorIndex != Wanted)
	{
		HatColorIndex = Wanted;
		ApplyBeanieColor();
	}
}

void APenguinCharacter::OnRep_PlayerSlot()
{
	ApplyBeanieColor();
}

void APenguinCharacter::ApplyBeanieColor()
{
	using namespace PenguinCharacterPrivate;
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (!SkelMesh || !SkelMesh->GetSkeletalMeshAsset())
	{
		return;
	}
	const int32 ColorIndex = WrapSlot(GetHatColorIndex());
	BeanieMaterials.Reset();
	const TArray<FName> SlotNames = SkelMesh->GetMaterialSlotNames();
	for (int32 Index = 0; Index < SlotNames.Num(); ++Index)
	{
		const FString Slot = SlotNames[Index].ToString().ToLower();
		const bool bFelt = Slot.Contains(TEXT("felt"));
		const bool bCuff = Slot.Contains(TEXT("wool"));
		if (!bFelt && !bCuff)
		{
			continue;
		}
		if (UMaterialInstanceDynamic* Material = SkelMesh->CreateDynamicMaterialInstance(Index))
		{
			Material->SetVectorParameterValue(TEXT("Color"), bFelt ? FeltColors[ColorIndex] : CuffColors[ColorIndex]);
			BeanieMaterials.Add(Material);
		}
	}
}

// ---------------------------------------------------------------------------- no outline (always hidden)

namespace PenguinCharacterPrivate
{
	/** Indices of the slots called "Outline" (the ink hull; the horn has one too). */
	static void FindOutlineSlots(const TArray<FName>& SlotNames, TArray<int32>& Out)
	{
		for (int32 Index = 0; Index < SlotNames.Num(); ++Index)
		{
			if (SlotNames[Index].ToString().Contains(TEXT("outline"), ESearchCase::IgnoreCase))
			{
				Out.Add(Index);
			}
		}
	}

	/** The material slot a section of one LOD draws (the LOD's own material map first, as the renderer does). */
	static int32 SectionMaterialIndex(const USkeletalMesh* Asset, int32 LOD, int32 Section, int32 SectionMaterial)
	{
		const FSkeletalMeshLODInfo* LODInfo = Asset->GetLODInfo(LOD);
		if (LODInfo && LODInfo->LODMaterialMap.IsValidIndex(Section) && LODInfo->LODMaterialMap[Section] != INDEX_NONE)
		{
			return LODInfo->LODMaterialMap[Section];
		}
		return SectionMaterial;
	}
}

void APenguinCharacter::ApplyOutlineSetting()
{
	using namespace PenguinCharacterPrivate;
	const bool bShow = false; // the ink outline is gone for good: its sections are always hidden

	// ---- the penguin: the outline's section on every LOD (the hidden state is kept per LOD and material slot by the mesh
	// component, apart from the trait looks' own hidden sections - the beanie's - and it survives a render state rebuild)
	USkeletalMeshComponent* SkelMesh = GetMesh();
	const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
	const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
	int32 Changed = 0;
	if (RenderData)
	{
		TArray<int32> Slots;
		FindOutlineSlots(SkelMesh->GetMaterialSlotNames(), Slots);
		for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
		{
			const TArray<FSkelMeshRenderSection>& Sections = RenderData->LODRenderData[LOD].RenderSections;
			for (int32 Section = 0; Section < Sections.Num(); ++Section)
			{
				const int32 MaterialIndex = SectionMaterialIndex(Asset, LOD, Section, Sections[Section].MaterialIndex);
				if (Slots.Contains(MaterialIndex) && SkelMesh->IsMaterialSectionShown(MaterialIndex, LOD) != bShow)
				{
					SkelMesh->ShowMaterialSection(MaterialIndex, Section, bShow, LOD);
					++Changed;
				}
			}
		}
	}

	// ---- the noot horn (a static mesh: no hiding of one section, so its outline slot gets a material that draws nothing)
	if (NootHorn && NootHorn->GetStaticMesh())
	{
		TArray<int32> HornSlots;
		FindOutlineSlots(NootHorn->GetMaterialSlotNames(), HornSlots);
		if (HornSlots.Num() > 0 && !bShow && !HornOutlineHider)
		{
			if (UMaterialInterface* Ghost = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Ghost.M_Ghost"), nullptr, LOAD_Quiet | LOAD_NoWarn))
			{
				HornOutlineHider = UMaterialInstanceDynamic::Create(Ghost, this);
				HornOutlineHider->SetScalarParameterValue(TEXT("Opacity"), 0.f);
			}
		}
		const bool bHide = !bShow && HornOutlineHider != nullptr;
		if (HornSlots.Num() > 0 && bHide != bHornOutlineHidden)
		{
			for (const int32 Slot : HornSlots)
			{
				NootHorn->SetMaterial(Slot, bHide ? HornOutlineHider.Get() : nullptr); // nullptr: the mesh's own
			}
			bHornOutlineHidden = bHide;
			++Changed;
		}
	}
	if (Changed > 0)
	{
		UE_LOG(LogPingu, Verbose, TEXT("[OUTLINE] %s: outline hidden (%d changed)"), *GetName(), Changed);
	}
}

void APenguinCharacter::GetOutlineInfo(TArray<FPenguinOutlineLODInfo>& OutLODs) const
{
	using namespace PenguinCharacterPrivate;
	OutLODs.Reset();
	USkeletalMeshComponent* SkelMesh = GetMesh(); // (IsMaterialSectionShown is not const)
	const USkeletalMesh* Asset = SkelMesh ? SkelMesh->GetSkeletalMeshAsset() : nullptr;
	const FSkeletalMeshRenderData* RenderData = Asset ? Asset->GetResourceForRendering() : nullptr;
	if (!RenderData)
	{
		return;
	}
	TArray<int32> Slots;
	FindOutlineSlots(SkelMesh->GetMaterialSlotNames(), Slots);
	for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
	{
		FPenguinOutlineLODInfo& Info = OutLODs.AddDefaulted_GetRef();
		const TArray<FSkelMeshRenderSection>& Sections = RenderData->LODRenderData[LOD].RenderSections;
		for (int32 Section = 0; Section < Sections.Num(); ++Section)
		{
			const int32 Triangles = static_cast<int32>(Sections[Section].NumTriangles);
			const int32 MaterialIndex = SectionMaterialIndex(Asset, LOD, Section, Sections[Section].MaterialIndex);
			const bool bOutline = Slots.Contains(MaterialIndex);
			const bool bSectionHidden = bOutline && !SkelMesh->IsMaterialSectionShown(MaterialIndex, LOD);
			Info.Triangles += Triangles;
			Info.Sections += bOutline ? 1 : 0;
			Info.Hidden += bSectionHidden ? 1 : 0;
			Info.OutlineTriangles += bOutline ? Triangles : 0;
			Info.DrawnTriangles += bSectionHidden ? 0 : Triangles;
		}
	}
}

bool APenguinCharacter::IsHornOutlineShown(bool* bOutHasOutline) const
{
	TArray<int32> HornSlots;
	if (NootHorn && NootHorn->GetStaticMesh())
	{
		PenguinCharacterPrivate::FindOutlineSlots(NootHorn->GetMaterialSlotNames(), HornSlots);
	}
	if (bOutHasOutline)
	{
		*bOutHasOutline = HornSlots.Num() > 0;
	}
	for (const int32 Slot : HornSlots)
	{
		if (NootHorn->GetMaterial(Slot) == HornOutlineHider.Get() && HornOutlineHider)
		{
			return false;
		}
	}
	return HornSlots.Num() > 0;
}

// ---------------------------------------------------------------------------- input

void APenguinCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogPingu, Error, TEXT("Enhanced Input is not the default input component class."));
		return;
	}
	const UPenguinInputConfig* Config = UPenguinInputConfig::Get();
	Input->BindAction(Config->Move, ETriggerEvent::Triggered, this, &APenguinCharacter::Input_Move);
	Input->BindAction(Config->Move, ETriggerEvent::Completed, this, &APenguinCharacter::Input_MoveReleased);
	Input->BindAction(Config->Move, ETriggerEvent::Canceled, this, &APenguinCharacter::Input_MoveReleased);
	// Jumping is read from the jump keys by APenguinPlayerController (an action fires "started" only when the first of its
	// keys goes down, so Space did nothing while W was still held). The old way stays for the input test's comparison.
	if (UPenguinInputConfig::UseOldJumpInput())
	{
		Input->BindAction(Config->Jump, ETriggerEvent::Started, this, &APenguinCharacter::Input_JumpPressed);
		Input->BindAction(Config->Jump, ETriggerEvent::Completed, this, &APenguinCharacter::Input_JumpReleased);
		Input->BindAction(Config->Jump, ETriggerEvent::Canceled, this, &APenguinCharacter::Input_JumpReleased);
	}
	Input->BindAction(Config->Dash, ETriggerEvent::Started, this, &APenguinCharacter::Input_Dash);
	Input->BindAction(Config->BellySlide, ETriggerEvent::Started, this, &APenguinCharacter::Input_BellySlide);
	Input->BindAction(Config->Respawn, ETriggerEvent::Started, this, &APenguinCharacter::Input_Respawn);
	// Ability keys: down and up (a size ray fires on the release, or on yourself when held: SetAbilityKeyDown).
	Input->BindAction(Config->Ability1, ETriggerEvent::Started, this, &APenguinCharacter::Input_Ability1);
	Input->BindAction(Config->Ability2, ETriggerEvent::Started, this, &APenguinCharacter::Input_Ability2);
	Input->BindAction(Config->Ability3, ETriggerEvent::Started, this, &APenguinCharacter::Input_Ability3);
	Input->BindAction(Config->Ability1, ETriggerEvent::Completed, this, &APenguinCharacter::Input_Ability1Released);
	Input->BindAction(Config->Ability2, ETriggerEvent::Completed, this, &APenguinCharacter::Input_Ability2Released);
	Input->BindAction(Config->Ability3, ETriggerEvent::Completed, this, &APenguinCharacter::Input_Ability3Released);
	Input->BindAction(Config->Ability1, ETriggerEvent::Canceled, this, &APenguinCharacter::Input_Ability1Released);
	Input->BindAction(Config->Ability2, ETriggerEvent::Canceled, this, &APenguinCharacter::Input_Ability2Released);
	Input->BindAction(Config->Ability3, ETriggerEvent::Canceled, this, &APenguinCharacter::Input_Ability3Released);
	Input->BindAction(Config->Aim, ETriggerEvent::Triggered, this, &APenguinCharacter::Input_Aim);
	// Back inside the dead zone: stop aiming with the stick (aim where you face).
	Input->BindAction(Config->Aim, ETriggerEvent::Completed, this, &APenguinCharacter::Input_AimReleased);
	Input->BindAction(Config->Aim, ETriggerEvent::Canceled, this, &APenguinCharacter::Input_AimReleased);
}

void APenguinCharacter::ApplyVirtualMoveInput(const FVector2D& Input)
{
	if (PenguinMovement)
	{
		PenguinMovement->SetMoveInput(Input);
	}
	if (!FMath::IsNearlyZero(Input.X))
	{
		AddMovementInput(FVector(1.f, 0.f, 0.f), Input.X, true);
	}
}

void APenguinCharacter::PressJump()
{
	if (PenguinMovement)
	{
		PenguinMovement->JumpPressed();
	}
}

void APenguinCharacter::ReleaseJump()
{
	if (PenguinMovement)
	{
		PenguinMovement->JumpReleased();
	}
}

void APenguinCharacter::PressDash()
{
	if (PenguinMovement)
	{
		PenguinMovement->DashPressed();
	}
}

void APenguinCharacter::BellySlide()
{
	if (PenguinMovement)
	{
		PenguinMovement->BellySlidePressed(); // the movement runs it (on a LAN client: our own machine, the server sees it)
	}
}

bool APenguinCharacter::ActivateAbility(int32 Slot)
{
	if (!AbilityComponent || bKnockedOut)
	{
		return false;
	}
	const FVector Target = bAimValid ? AimPoint : GetAimOrigin() + FVector(GetFacing() * 400.f, 0.f, 0.f);
	if (HasAuthority())
	{
		return AbilityComponent->UseAbility(Slot, Target);
	}
	// LAN client: abilities are server gameplay. Only the owner can ask, and the server has the last word (cooldown, control).
	if (GetLocalRole() != ROLE_AutonomousProxy || !AbilityComponent->GetAbilities().IsValidIndex(Slot))
	{
		return false;
	}
	Rq02(Slot, FVector_NetQuantize10(Target), bAimFromStick);
	return true;
}

void APenguinCharacter::Rq02_Implementation(int32 Slot, const FVector_NetQuantize10& InAimPoint, bool bFromStick)
{
	// The host's own checks follow (its loadout, its cooldowns, the penguin's state: UseAbility); a joiner's machine is never
	// trusted with more than which key it pressed and where it aimed. Illegal requests go nowhere (PinguAntiCheat).
	if (!AbilityComponent || !PinguAntiCheat::AllowRequest(this, PinguAntiCheat::EGate::Ability))
	{
		return;
	}
	if (InAimPoint.ContainsNaN() || Slot < 0 || Slot >= NumAbilityKeys)
	{
		PinguAntiCheat::NoteIgnored(this, TEXT("an ability request"), FString::Printf(TEXT("slot %d, aim %s"), Slot, *InAimPoint.ToString()));
		return;
	}
	if (bKnockedOut)
	{
		return; // (a slot the host's loadout does not have - a press as a new round begins - UseAbility turns down silently)
	}
	// Aim where the owner aimed at the press (a size ray reads the stick flag for a self-cast).
	SetAimPoint(InAimPoint, bFromStick);
	AbilityComponent->UseAbility(Slot, AimPoint);
}

bool APenguinCharacter::ActivateAbilityOnSelf(int32 Slot)
{
	if (!AbilityComponent || bKnockedOut)
	{
		return false;
	}
	if (HasAuthority())
	{
		// Aimed right at our own chest: the size ray's self-cast (UPenguinAbilityComponent::DoSizeRay), a normal use.
		return AbilityComponent->UseAbilityOnSelf(Slot);
	}
	if (GetLocalRole() != ROLE_AutonomousProxy || !AbilityComponent->GetAbilities().IsValidIndex(Slot))
	{
		return false;
	}
	// LAN client: the server runs it on its copy of us - whatever our aim and however far that copy is behind ours.
	Rq01(Slot);
	return true;
}

void APenguinCharacter::DevCheatRequestAbility(int32 Slot)
{
#if !UE_BUILD_SHIPPING
	if (GetLocalRole() == ROLE_AutonomousProxy)
	{
		Rq02(Slot, FVector_NetQuantize10(GetAimOrigin() + FVector(GetFacing() * 400.f, 0.f, 0.f)), false);
	}
#endif
}

void APenguinCharacter::Rq01_Implementation(int32 Slot)
{
	if (AbilityComponent && !bKnockedOut && PinguAntiCheat::AllowRequest(this, PinguAntiCheat::EGate::Ability))
	{
		AbilityComponent->UseAbilityOnSelf(Slot); // checks the slot holds a size ray, the cooldown and control
	}
}

void APenguinCharacter::SetAbilityKeyDown(int32 Slot, bool bDown)
{
	using namespace PenguinCharacterPrivate;
	if (Slot < 0 || Slot >= NumAbilityKeys)
	{
		return;
	}
	FAbilityKeyState& Key = AbilityKeys[Slot];
	if (bDown)
	{
		// Enhanced Input starts an action only when all its keys were up: a press while this one looks held means its
		// release never got here (a lost focus, say) - it is a new press all the same, never a stuck key.
		Key.bDown = true;
		Key.Ability = AbilityComponent ? AbilityComponent->GetAbility(Slot) : EPenguinAbility::None;
		Key.bPending = IsSizeRay(Key.Ability) && !bKnockedOut;
		if (Key.bPending)
		{
			// A size ray waits: the release shoots it, holding on uses it on yourself (UpdateAbilityKeys).
			const UWorld* World = GetWorld();
			Key.PressTime = World ? World->GetRealTimeSeconds() : 0.0;
			return;
		}
		if (UPenguinAbilityComponent::IsChargedAbility(Key.Ability) && AbilityComponent && !bKnockedOut)
		{
			// The icicle charges while its key is held; the release throws it, further the longer it was held.
			AbilityComponent->BeginCharge(Slot);
			return;
		}
		ActivateAbility(Slot); // everything else fires on the press, as always
		return;
	}
	if (!Key.bDown)
	{
		return;
	}
	Key.bDown = false;
	if (UPenguinAbilityComponent::IsChargedAbility(Key.Ability) && AbilityComponent)
	{
		AbilityComponent->ReleaseCharge(Slot); // (nothing if no charge runs: cancelled, or it never started)
		return;
	}
	if (Key.Ability == EPenguinAbility::GiantSnowball && AbilityComponent)
	{
		// The giant snowball is rolled up while its key is held: let go too soon and it crumbles (the cooldown runs all the
		// same). Nothing if it has rolled off already.
		AbilityComponent->ReleaseRollUp(Slot);
		return;
	}
	if (Key.bPending)
	{
		Key.bPending = false;
		// A tap: the ray is shot toward the aim - if the slot still holds it (a picked-up bubble may have swapped it).
		if (AbilityComponent && AbilityComponent->GetAbility(Slot) == Key.Ability)
		{
			ActivateAbility(Slot);
		}
	}
}

void APenguinCharacter::CancelAbilityKeys()
{
	bool bRollUpHeld = false;
	for (FAbilityKeyState& Key : AbilityKeys)
	{
		bRollUpHeld |= Key.bDown && Key.Ability == EPenguinAbility::GiantSnowball;
		Key = FAbilityKeyState();
	}
	if (bRollUpHeld && AbilityComponent)
	{
		AbilityComponent->ReleaseRollUp(INDEX_NONE); // a forgotten key: the roll-up crumbles (it counts as used)
	}
	if (AbilityComponent)
	{
		AbilityComponent->DropCharge(); // an icicle charging on a forgotten key throws nothing
	}
}

void APenguinCharacter::UpdateAbilityKeys()
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (bKnockedOut)
	{
		CancelAbilityKeys();
		return;
	}
	const double Now = World->GetRealTimeSeconds();
	for (int32 Slot = 0; Slot < NumAbilityKeys; ++Slot)
	{
		FAbilityKeyState& Key = AbilityKeys[Slot];
		if (!Key.bDown || !Key.bPending || Now - Key.PressTime < SelfCastHoldTime)
		{
			continue;
		}
		// Held long enough: the ray on yourself, once, right now (the release does nothing more).
		Key.bPending = false;
		if (AbilityComponent && AbilityComponent->GetAbility(Slot) == Key.Ability)
		{
			ActivateAbilityOnSelf(Slot);
		}
	}
}

void APenguinCharacter::SetAimPoint(const FVector& WorldPoint, bool bFromStick)
{
	AimPoint = FVector(WorldPoint.X, 0.f, WorldPoint.Z);
	bAimValid = true;
	bAimFromStick = bFromStick;
	if (HasAuthority())
	{
		UpdateReplicatedAim();
	}
}

void APenguinCharacter::UpdateReplicatedAim()
{
	const FVector2f OnPlane(static_cast<float>(AimPoint.X), static_cast<float>(AimPoint.Z));
	if (!RepAimPoint.Equals(OnPlane, PenguinCharacterPrivate::RepAimTolerance))
	{
		RepAimPoint = OnPlane;
	}
}

void APenguinCharacter::Rq03_Implementation(const FVector_NetQuantize10& InAimPoint, bool bFromStick)
{
	if (!InAimPoint.ContainsNaN() && PinguAntiCheat::AllowRequest(this, PinguAntiCheat::EGate::Aim))
	{
		SetAimPoint(InAimPoint, bFromStick);
	}
}

void APenguinCharacter::OnRep_Aim()
{
	AimPoint = FVector(RepAimPoint.X, 0.f, RepAimPoint.Y);
}

bool APenguinCharacter::IsInputBlockedByLobby() const
{
	// The lobby island (2026-10-24, Linus: "make it so u cant walk around in the lobby"): a person's penguin stands where it
	// puffed in and does its idles; only the bots stroll (APinguLobby walks them itself, not through these handlers).
	const APinguLobby* Lobby = APinguLobby::Get(GetWorld());
	return Lobby && Lobby->IsShowing();
}

void APenguinCharacter::Input_Move(const FInputActionValue& Value)
{
	if (IsInputBlockedByLobby())
	{
		ApplyVirtualMoveInput(FVector2D::ZeroVector);
		return;
	}
	ApplyVirtualMoveInput(Value.Get<FVector2D>());
}
void APenguinCharacter::Input_MoveReleased(const FInputActionValue& Value)
{
	if (PenguinMovement)
	{
		PenguinMovement->SetMoveInput(FVector2D::ZeroVector);
	}
}
void APenguinCharacter::Input_JumpPressed(const FInputActionValue& Value) { if (!IsInputBlockedByLobby()) { PressJump(); } }
void APenguinCharacter::Input_JumpReleased(const FInputActionValue& Value) { ReleaseJump(); }
void APenguinCharacter::Input_Dash(const FInputActionValue& Value) { if (!IsInputBlockedByLobby()) { PressDash(); } }
void APenguinCharacter::Input_BellySlide(const FInputActionValue& Value) { if (!IsInputBlockedByLobby()) { BellySlide(); } }
void APenguinCharacter::Input_Ability1(const FInputActionValue& Value) { SetAbilityKeyDown(0, true); }
void APenguinCharacter::Input_Ability2(const FInputActionValue& Value) { SetAbilityKeyDown(1, true); }
void APenguinCharacter::Input_Ability3(const FInputActionValue& Value) { SetAbilityKeyDown(2, true); }
void APenguinCharacter::Input_Ability1Released(const FInputActionValue& Value) { SetAbilityKeyDown(0, false); }
void APenguinCharacter::Input_Ability2Released(const FInputActionValue& Value) { SetAbilityKeyDown(1, false); }
void APenguinCharacter::Input_Ability3Released(const FInputActionValue& Value) { SetAbilityKeyDown(2, false); }

void APenguinCharacter::Input_Aim(const FInputActionValue& Value)
{
	StickAim = Value.Get<FVector2D>();
	if (StickAim.SizeSquared() > 0.09f && GetWorld())
	{
		LastStickAimTime = GetWorld()->GetRealTimeSeconds();
	}
}

void APenguinCharacter::Input_AimReleased(const FInputActionValue& Value)
{
	StickAim = FVector2D::ZeroVector;
}

void APenguinCharacter::Input_Respawn(const FInputActionValue& Value)
{
	// Respawning at will is for practice and the LAN lobby; the server owns the round flow and checks the phase.
	if (!HasAuthority())
	{
		const UWorld* World = GetWorld();
		const APenguinGameState* PenguinState = World ? World->GetGameState<APenguinGameState>() : nullptr;
		if (PenguinState && PenguinState->AllowsManualRespawn())
		{
			Rq05();
		}
		return;
	}
	const APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
	if (!GameMode || GameMode->AllowsManualRespawn())
	{
		Respawn();
	}
}

void APenguinCharacter::Rq05_Implementation()
{
	if (!PinguAntiCheat::AllowRequest(this, PinguAntiCheat::EGate::Lobby))
	{
		return;
	}
	const APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr;
	if (GameMode && GameMode->AllowsManualRespawn())
	{
		Respawn();
	}
}

// ---------------------------------------------------------------------------- round flow

void APenguinCharacter::Respawn()
{
	if (!HasAuthority())
	{
		return;
	}
	// Back in as far from everybody else as the island allows (the LAN lobby; practice is alone: our own start).
	if (const APenguinGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr)
	{
		FVector Far;
		if (GameMode->FindFarthestStart(this, Far))
		{
			SpawnLocation = Far;
		}
	}
	// Never respawn into a hole a bomb blew into the (practice) island: use the nearest column that has ground.
	FVector Where = SpawnLocation;
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), SpawnLocation);
	float GroundZ = 0.f;
	if (Island && !Island->FindGroundBelow(SpawnLocation, 3000.f, GroundZ))
	{
		const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : 60.f;
		bool bFound = false;
		for (int32 Step = 1; Step <= 30 && !bFound; ++Step)
		{
			for (const float Side : { 1.f, -1.f })
			{
				const FVector Probe = SpawnLocation + FVector(Side * Step * Island->CellSize, 0.f, 800.f);
				if (Island->FindGroundBelow(Probe, 4000.f, GroundZ))
				{
					Where = FVector(Probe.X, 0.f, GroundZ + HalfHeight + 60.f);
					bFound = true;
					break;
				}
			}
		}
	}
	ResetForRound(Where);
}

void APenguinCharacter::ResetForRound(const FVector& Location)
{
	if (!HasAuthority())
	{
		return; // the server's call reaches the owner (Rq06) and everybody else by replication
	}
	bKnockedOut = false;
	bKOHidden = false;
	KOTime = 0.f;
	KOReason = EPenguinKOReason::None;
	bVictory = false;
	LastAttacker.Reset();
	LastAttackTime = -100.f;
	bIceThrown = false;
	SetActorHiddenInGame(false);
	SpawnLocation = Location;
	ApplyRoundReset(Location, true);
	SizeTimeLeft = 0.f;
	BaseSize = 1.f; // (a trait of the new round sets its own at its transform)
	if (IsRemotePlayerPenguin())
	{
		// The owner's simulation owns the position: it resets itself. Until it confirms, the moves it sends may still be
		// from before (in the sea, say): see UpdateNetSync.
		++ResetSerial;
		bAwaitingResetAck = true;
		ResetLocation = Location;
		const UWorld* World = GetWorld();
		ResetAckDeadline = (World ? static_cast<float>(World->GetTimeSeconds()) : 0.f) + PenguinCharacterPrivate::ResetAckTimeout;
		Rq06(FVector_NetQuantize100(Location), ResetSerial);
	}
}

void APenguinCharacter::ApplyRoundReset(const FVector& Location, bool bOnServer)
{
	SetActorEnableCollision(true);
	if (PenguinMovement)
	{
		PenguinMovement->SetKnockedOut(false);
		PenguinMovement->ResetMovementState();
		PenguinMovement->StopMovementImmediately();
		PenguinMovement->SetMovementMode(MOVE_Falling);
		PenguinMovement->FaceDirection(Location.X > 0.f ? -1.f : 1.f); // face the middle
	}
	if (bOnServer && PenguinMovement && IsRemotePlayerPenguin())
	{
		// The movement component's teleport also tells the owner (and copes with the moves already on their way).
		PenguinMovement->NetTeleportPenguin(Location, FVector::ZeroVector, true);
	}
	else
	{
		SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	}
	KeepActorUnrotated();
	if (bOnServer && AbilityComponent)
	{
		AbilityComponent->ResetAll(); // replicated by the ability component
	}
	ShowIceBlock(false);
	ShowIceFeet(false);
	OnShieldChanged(false);
	SizeScale = SizeTarget = 1.f;
	SetActorScale3D(FVector::OneVector);
	if (PenguinMovement)
	{
		PenguinMovement->SetSizeScale(1.f);
	}
	Squash = SquashVelocity = Stretch = StretchVelocity = 0.f;
	MeshYaw = (Location.X > 0.f) ? (90.f - CameraPeekYaw) : (-90.f + CameraPeekYaw);
	BellyTilt = 0.f;
	bSlideFxActive = false;
	CancelAbilityKeys(); // a ray key held into the new round does nothing
	SlideStartOverlaps.Reset();
	SyncSeenCounters();
	EndDeathLook(); // normal again: mesh scale and place, no ice, no soot, no stars (the other machines: UpdateDeathLook)
}

void APenguinCharacter::Rq06_Implementation(const FVector_NetQuantize100& Location, uint8 Serial)
{
	if (HasAuthority())
	{
		return;
	}
	// Our own copies of the state the server replicates to everybody else (skip owner).
	bKnockedOut = false;
	bKOHidden = false;
	KOTime = 0.f;
	KOReason = EPenguinKOReason::None;
	bVictory = false;
	ApplyRoundReset(Location, false);
	PendingResetAck = Serial;
	PendingResetAckFrames = 0;
	const UWorld* World = GetWorld();
	PendingResetAckTime = (World ? static_cast<float>(World->GetTimeSeconds()) : 0.f) + PenguinCharacterPrivate::ResetAckDelay;
}

void APenguinCharacter::Rq04_Implementation(uint8 Serial)
{
	if (Serial == ResetSerial)
	{
		bAwaitingResetAck = false;
	}
}

void APenguinCharacter::SetInputLocked(bool bLocked)
{
	if (PenguinMovement)
	{
		PenguinMovement->SetInputLocked(bLocked); // reaches a remote owner by itself (movement component)
		if (bLocked)
		{
			PenguinMovement->SetMoveInput(FVector2D::ZeroVector);
			PenguinMovement->JumpReleased();
		}
	}
}

void APenguinCharacter::SetVictory(bool bInVictory)
{
	if (!HasAuthority())
	{
		return;
	}
	bVictory = bInVictory;
	if (IsRemotePlayerPenguin())
	{
		Rq09(bInVictory);
	}
}

void APenguinCharacter::Rq09_Implementation(bool bInVictory)
{
	if (!HasAuthority())
	{
		bVictory = bInVictory;
	}
}

// ---------------------------------------------------------------------------- network upkeep

void APenguinCharacter::UpdateNetSync(float Dt)
{
	using namespace PenguinCharacterPrivate;
	UWorld* World = GetWorld();
	if (!World || GetNetMode() == NM_Standalone)
	{
		return;
	}
	const float Now = static_cast<float>(World->GetTimeSeconds());
	if (HasAuthority())
	{
		if (bAwaitingResetAck)
		{
			if (Now > ResetAckDeadline || !IsRemotePlayerPenguin())
			{
				if (Now > ResetAckDeadline)
				{
					UE_LOG(LogPingu, Warning, TEXT("[NET] %s: the owner did not confirm round reset %d within %.0f s, trusting its positions again"),
						*DisplayName, static_cast<int32>(ResetSerial), ResetAckTimeout);
				}
				bAwaitingResetAck = false;
			}
			else if (FVector::DistSquared(GetActorLocation(), ResetLocation) > FMath::Square(StaleMoveDistance))
			{
				// A move the owner made before it got the reset: keep the penguin at the new place until fresh ones come.
				SetActorLocation(ResetLocation, false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
		return;
	}
	if (GetLocalRole() != ROLE_AutonomousProxy)
	{
		return;
	}
	// Our own penguin on a LAN client: confirm the last round reset once the moves from the new place are on their way.
	if (PendingResetAck >= 0 && ++PendingResetAckFrames >= ResetAckFrames && Now >= PendingResetAckTime)
	{
		Rq04(static_cast<uint8>(PendingResetAck));
		PendingResetAck = -1;
	}
	// Where we aim, for the others to see (an ability press sends its own aim).
	AimSendTimer -= Dt;
	if (bAimValid && AimSendTimer <= 0.f && (!AimPoint.Equals(LastSentAim, AimSendTolerance) || bAimFromStick != bLastSentAimFromStick))
	{
		AimSendTimer = AimSendInterval;
		LastSentAim = AimPoint;
		bLastSentAimFromStick = bAimFromStick;
		Rq03(FVector_NetQuantize10(AimPoint), bAimFromStick);
	}
}

// ---------------------------------------------------------------------------- helpers

FVector APenguinCharacter::GetFeetLocation() const
{
	return GetActorLocation() - FVector(0.f, 0.f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
}

float APenguinCharacter::GetFacing() const
{
	return PenguinMovement ? PenguinMovement->GetFacing() : 1.f;
}

void APenguinCharacter::FaceDirection(float DirectionX)
{
	if (PenguinMovement)
	{
		PenguinMovement->FaceDirection(DirectionX);
	}
	if (IsRemotePlayerPenguin())
	{
		Rq11(DirectionX); // the owner's facing is its own (the server copy's is overwritten by it)
	}
}

void APenguinCharacter::Rq11_Implementation(float DirectionX)
{
	if (!HasAuthority() && PenguinMovement)
	{
		PenguinMovement->FaceDirection(DirectionX);
	}
}

void APenguinCharacter::NetTeleport(const FVector& Location, const FVector& NewVelocity, bool bForceFalling, bool bKeepSlide)
{
	if (PenguinMovement)
	{
		PenguinMovement->NetTeleportPenguin(Location, NewVelocity, bForceFalling, bKeepSlide);
	}
	if (bAwaitingResetAck)
	{
		ResetLocation = Location; // the stale-move guard keeps the penguin at the newest place
	}
}

float APenguinCharacter::DistanceTo(const FVector& Point) const
{
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const FVector Center = GetActorLocation();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float Half = Capsule->GetScaledCapsuleHalfHeight() - Radius;
	const float Z = FMath::Clamp(static_cast<float>(Point.Z), static_cast<float>(Center.Z) - Half, static_cast<float>(Center.Z) + Half);
	const float Dx = Point.X - Center.X;
	const float Dz = Point.Z - Z;
	return FMath::Max(0.f, FMath::Sqrt(Dx * Dx + Dz * Dz) - Radius);
}

bool APenguinCharacter::IsShielded() const
{
	return AbilityComponent && AbilityComponent->IsShieldActive();
}

bool APenguinCharacter::IsBraced() const
{
	return !bKnockedOut && AbilityComponent && AbilityComponent->IsBraced();
}

void APenguinCharacter::PlayAction(EPenguinClip Clip, float Rate)
{
	if (HasAuthority())
	{
		// Every client plays it once through OnRep_Action - the owner too, nothing plays it ahead of the server.
		++RepActionCount;
		RepActionClip = Clip;
		RepActionRate = Rate;
	}
	PlayLocalAction(Clip, Rate);
}

void APenguinCharacter::PlayAimedAction(EPenguinClip Clip, float Rate, const FVector& AimDirection)
{
	FVector2D Aim = FVector2D(AimDirection.X, AimDirection.Z).GetSafeNormal();
	if (Aim.IsNearlyZero())
	{
		Aim = FVector2D(GetFacing(), 0.f);
	}
	ActionAim = Aim;
	if (HasAuthority())
	{
		const float Angle = FMath::Atan2(static_cast<float>(Aim.Y), static_cast<float>(Aim.X)); // -pi .. pi
		RepActionAim = static_cast<uint8>(FMath::RoundToInt((Angle + UE_PI) / (2.f * UE_PI) * 255.f) & 0xFF);
	}
	if (Clip == EPenguinClip::Noot && PenguinCharacterPrivate::NootHornLogsLeft > 0)
	{
		--PenguinCharacterPrivate::NootHornLogsLeft;
		UE_LOG(LogPingu, Display, TEXT("[NOOTHORN] %s honks: aim %.2f %.2f (code %d), horn fitted %d"), *GetDisplayName(),
			Aim.X, Aim.Y, static_cast<int32>(RepActionAim), bHornFitted ? 1 : 0);
	}
	PlayAction(Clip, Rate);
}

void APenguinCharacter::PlayLocalAction(EPenguinClip Clip, float Rate)
{
	++ActionCount;
	ActionClip = Clip;
	ActionRate = Rate;
	if (Clip == EPenguinClip::Noot)
	{
		// The beak stretches out into the horn, pointing where the noot goes.
		HornAim = ActionAim;
		HornTime = 0.f;
	}
}

void APenguinCharacter::OnRep_Action()
{
	// Not the action a newly replicated penguin arrives with: that one is long over.
	if (HasActorBegunPlay())
	{
		const float Angle = static_cast<float>(RepActionAim) / 255.f * 2.f * UE_PI - UE_PI;
		ActionAim = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle));
		if (RepActionClip == EPenguinClip::Noot && PenguinCharacterPrivate::NootHornLogsLeft > 0)
		{
			--PenguinCharacterPrivate::NootHornLogsLeft;
			UE_LOG(LogPingu, Display, TEXT("[NOOTHORN] %s honks (replicated): aim %.2f %.2f (code %d), horn fitted %d"), *GetDisplayName(),
				ActionAim.X, ActionAim.Y, static_cast<int32>(RepActionAim), bHornFitted ? 1 : 0);
		}
		PlayLocalAction(RepActionClip, RepActionRate);
	}
}

void APenguinCharacter::TryTaunt()
{
	if (PenguinMovement && !bKnockedOut && PenguinMovement->IsMovingOnGround() && !PenguinMovement->HasNoControl()
		&& !PenguinMovement->IsDashing() && FMath::Abs(PenguinMovement->Velocity.X) < 200.f)
	{
		PlayAction(EPenguinClip::Taunt, 1.1f);
	}
}

EPenguinClip APenguinCharacter::PickNearFlipper(EPenguinClip RightFlipperClip, EPenguinClip LeftFlipperClip) const
{
	// Facing right the camera sees Flipper_L, facing left it sees Flipper_R.
	return GetFacing() > 0.f ? LeftFlipperClip : RightFlipperClip;
}

void APenguinCharacter::OnShieldChanged(bool bActive)
{
	bShieldShown = bActive;
	if (ShieldBubble)
	{
		ShieldBubble->SetHiddenInGame(!bActive);
	}
}

float APenguinCharacter::GetShieldRadius() const
{
	return UPenguinAbilityComponent::ShieldRadius * SizeScale;
}

void APenguinCharacter::ChangeSize(float Factor, float Duration, APenguinCharacter* By)
{
	if (!HasAuthority() || bKnockedOut)
	{
		return;
	}
	const float Old = SizeTarget;
	const float Wanted = SizeTarget * Factor;
	// Past a size limit (judged on the size it is heading for, as the rays stack on it): too big, it pops; too small, it
	// vanishes. By (the ray's shooter) gets the knock-out; our own ray on ourselves is nobody's.
	if (Wanted > PinguMaxSize * (1.f + PinguSizeLimitSlack) || Wanted < PinguMinSize * (1.f - PinguSizeLimitSlack))
	{
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s's size %.2f x %.3f = %.2f is past the limit (%.2f .. %.2f)"), *DisplayName, SizeTarget, Factor, Wanted,
			PinguMinSize, PinguMaxSize);
		KnockOut(Wanted > 1.f ? EPenguinKOReason::Overgrown : EPenguinKOReason::Shrunk, By ? By : this);
		return;
	}
	SizeTarget = FMath::Clamp(Wanted, PinguMinSize, PinguMaxSize);
	SizeTimeLeft = FMath::Max(SizeTimeLeft, Duration);
	if (IsRemotePlayerPenguin())
	{
		Rq10(SizeTarget); // the owner grows / shrinks its own penguin
	}
	const bool bGrow = Factor > 1.f;
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sound(this, bGrow ? EPinguSound::Grow : EPinguSound::Shrink);
	APinguFX::Sparkles(this, GetActorLocation(), 12, 60.f * SizeScale, bGrow ? FLinearColor(0.45f, 1.f, 0.35f) : FLinearColor(0.85f, 0.4f, 1.f));
	APinguFX::Ring(this, GetActorLocation(), 70.f * FMath::Max(SizeTarget, Old), bGrow ? FLinearColor(0.45f, 1.f, 0.35f) : FLinearColor(0.85f, 0.4f, 1.f), 0.3f);
}

void APenguinCharacter::Rq10_Implementation(float NewTarget)
{
	if (!HasAuthority())
	{
		SizeTarget = NewTarget;
	}
}

void APenguinCharacter::SetBaseSize(float Size)
{
	if (!HasAuthority() || bKnockedOut)
	{
		return;
	}
	const float NewBase = FMath::Clamp(Size, PinguMinSize, PinguMaxSize);
	if (FMath::IsNearlyEqual(NewBase, BaseSize, 1.e-4f))
	{
		return;
	}
	// A size ray that is still on keeps its steps: the target moves with the base (the size animates there as it does
	// for a ray, and the owner of a remote player's penguin follows).
	SizeTarget = FMath::Clamp(SizeTarget / FMath::Max(BaseSize, 0.01f) * NewBase, PinguMinSize, PinguMaxSize);
	BaseSize = NewBase;
	if (IsRemotePlayerPenguin())
	{
		Rq10(SizeTarget);
	}
}

void APenguinCharacter::OnRep_Size()
{
	if (IsLocallySimulated())
	{
		return; // (never sent to the owner: it sizes its own penguin)
	}
	// Somebody else's penguin: the server's size, capsule (the root) and mesh with it.
	SetActorScale3D(FVector(SizeScale));
	if (PenguinMovement)
	{
		PenguinMovement->SetSizeScale(SizeScale);
	}
}

void APenguinCharacter::UpdateSize(float Dt)
{
	const bool bAuthority = HasAuthority();
	if (bAuthority && SizeTimeLeft > 0.f)
	{
		SizeTimeLeft -= Dt;
		if (SizeTimeLeft <= 0.f && !FMath::IsNearlyEqual(SizeTarget, BaseSize))
		{
			SizeTarget = BaseSize; // wore off (back to the size it rests at: 1, or its trait's)
			if (IsRemotePlayerPenguin())
			{
				Rq10(SizeTarget);
			}
			FPinguFXBroadcastScope Broadcast(this);
			APinguFX::Sound(this, SizeScale > BaseSize ? EPinguSound::Shrink : EPinguSound::Grow, 0.6f);
		}
	}
	// The machine that simulates the penguin grows it with room checks; the server copy of a remote player's penguin
	// follows along for gameplay (its position is the owner's); everybody else gets the server's size (OnRep_Size).
	const bool bSimulated = IsLocallySimulated();
	if (!bAuthority && !bSimulated)
	{
		return;
	}
	if (FMath::IsNearlyEqual(SizeScale, SizeTarget, 0.002f))
	{
		return;
	}
	float Next = FMath::FInterpTo(SizeScale, SizeTarget, Dt, 9.f);
	if (FMath::Abs(Next - SizeTarget) < 0.003f)
	{
		Next = SizeTarget;
	}
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	UWorld* World = GetWorld();
	if (Next > SizeScale && Capsule && World)
	{
		// Growing: keep the feet on the ground. Touching a wall on one side? Slide away from it as we widen.
		// Only refuse (and try again next frame) when there is no room either way, or a ceiling. The server copy of a
		// remote player's penguin makes the same test (so it does not grow through a ceiling its owner is stuck under),
		// but never moves: its position is the owner's.
		const float UnscaledRadius = Capsule->GetUnscaledCapsuleRadius();
		const float OldHalf = Capsule->GetUnscaledCapsuleHalfHeight() * SizeScale;
		const float NewHalf = Capsule->GetUnscaledCapsuleHalfHeight() * Next;
		const FVector GrowCenter = GetActorLocation() + FVector(0.f, 0.f, NewHalf - OldHalf);
		const float Shift = UnscaledRadius * (Next - SizeScale) + 1.f;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguGrow), false, this);
		const FCollisionShape Shape = FCollisionShape::MakeCapsule(UnscaledRadius * Next, NewHalf);
		const FVector Candidates[3] = { GrowCenter, GrowCenter + FVector(Shift, 0.f, 0.f), GrowCenter - FVector(Shift, 0.f, 0.f) };
		int32 FreeIndex = INDEX_NONE;
		for (int32 Index = 0; Index < 3 && FreeIndex == INDEX_NONE; ++Index)
		{
			if (!World->OverlapBlockingTestByChannel(Candidates[Index], FQuat::Identity, ECC_Pawn, Shape, Params))
			{
				FreeIndex = Index;
			}
		}
		if (FreeIndex == INDEX_NONE)
		{
			return;
		}
		if (bSimulated)
		{
			SetActorLocation(Candidates[FreeIndex], false, nullptr, ETeleportType::TeleportPhysics);
		}
	}
	SizeScale = Next;
	SetActorScale3D(FVector(SizeScale));
	if (PenguinMovement)
	{
		// On the server copy of a remote player's penguin too: the movement component replicates its size to everybody.
		PenguinMovement->SetSizeScale(SizeScale);
	}
}

void APenguinCharacter::SetSlippery(float Seconds)
{
	if (!HasAuthority() || !PenguinMovement || bKnockedOut)
	{
		return;
	}
	if (TraitComponent && TraitComponent->IsFreezeImmune())
	{
		// Hot Feet: no frozen feet (an ice bolt, Ice Touch) - the ice hisses away.
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Puffs(this, GetFeetLocation(), 4, 20.f * SizeScale, 140.f, FVector::UpVector, 0.6f, FLinearColor(0.9f, 0.9f, 0.93f));
		APinguFX::Sound(this, EPinguSound::Melt, 0.5f);
		return;
	}
	PenguinMovement->SetSlippery(Seconds); // reaches a remote owner by itself (movement component)
	ShowIceFeet(true);
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sound(this, EPinguSound::Slippery);
	APinguFX::Sparkles(this, GetFeetLocation(), 8, 35.f, FLinearColor(0.7f, 0.95f, 1.f));
}

void APenguinCharacter::ShowIceFeet(bool bShow)
{
	bIceFeetShown = bShow;
	for (UStaticMeshComponent* Foot : { IceFootL.Get(), IceFootR.Get() })
	{
		if (Foot)
		{
			Foot->SetHiddenInGame(!bShow);
		}
	}
}

void APenguinCharacter::ShowIceBlock(bool bShow)
{
	bIceShown = bShow;
	if (IceBlock)
	{
		IceBlock->SetHiddenInGame(!bShow);
	}
}

void APenguinCharacter::NudgeVelocityX(float DeltaX)
{
	// The movement component also forwards the nudge to the owner of a remote player's penguin.
	if (PenguinMovement)
	{
		PenguinMovement->NudgeVelocity(FVector2D(DeltaX, 0.f));
	}
}

// ---------------------------------------------------------------------------- combat

bool APenguinCharacter::ApplyPenguinHit(const FPenguinHit& Hit)
{
	using namespace PenguinCharacterPrivate;
	// Hits are match events: only the server has them. The round winner celebrates in peace (a bomb left over from the
	// fight can't knock it out any more).
	if (!HasAuthority() || bKnockedOut || IsHidden() || !PenguinMovement || bVictory)
	{
		return false;
	}
	// (Team matches have friendly fire: a teammate's hit lands like anybody's - the game mode only keeps it out of the
	// statistics: OnPenguinHit.)
	// The ice just took a hit from this attack: the rest of it (a stomp's slam right after its squash) does nothing.
	if (Hit.Source != EPenguinAbility::None && Hit.Source == IceGuardSource && Hit.Instigator == IceGuardBy && GetWorld()
		&& GetWorld()->GetTimeSeconds() < IceGuardUntil)
	{
		return false;
	}
	// A push meeting a penguin that has just pushed itself clashes: nothing of the hit happens to us (we keep our own
	// momentum), the pusher bounces off. Before the shell: braced in our shell, we clash all the same.
	if (UPenguinAbilityComponent::IsBracingPush(Hit.Source) && IsBraced())
	{
		ClashWith(Hit.Instigator.Get());
		return false;
	}
	if (IsShielded())
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::ShieldBlock);
		APinguFX::Sparkles(this, Hit.Location, 6, 30.f, FLinearColor(0.6f, 0.95f, 1.f));
		APinguFX::Ring(this, GetAimOrigin(), UPenguinAbilityComponent::ShieldRadius, FLinearColor(0.6f, 0.95f, 1.f), 0.2f);
		return false;
	}
	// Frozen: the ice block is a shield. A snowball does not break it: it bursts on the cube and nudges it along a little (still
	// frozen, still a shield); the hit does not land - no statistics, no combo (false: the thrower gets no snowball back).
	if (PenguinMovement->IsFrozen() && UPenguinAbilityComponent::BouncesOffIce(Hit.Source))
	{
		NudgeVelocityX(Hit.Knockback.X * PinguTuning::SnowballIceNudge * FMath::Max(Hit.Strength, 0.f));
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::IcicleHit, 0.7f, 0.05f, 0.9f);
		APinguFX::Sparkles(this, Hit.Location, 4, 30.f * SizeScale, FLinearColor(0.7f, 0.95f, 1.f));
		APinguFX::Shake(this, 0.08f);
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s's ice shrugged off the %s"), *DisplayName, UPenguinAbilityComponent::GetInfo(Hit.Source).Name);
		return false;
	}
	// A hit that smashes ice (slap, belly stomp, icicle, snow bomb) shatters it
	// and is used up on it - nothing happens to the penguin inside, not even a knock-out. Wind and a belly slide blow the
	// ice cube away instead (below); frost freezes it again; an ice bolt or a ray leaves the ice alone; the fish hook pulls
	// the whole ice cube along with the penguin inside (it shatters where it hits something); an iceberg still crushes it.
	if (PenguinMovement->IsFrozen() && UPenguinAbilityComponent::IsIceSmashing(Hit.Source))
	{
		PenguinMovement->SetFrozen(0.f); // reaches a remote owner by itself
		ShowIceBlock(false);
		IceGuardSource = Hit.Source;
		IceGuardBy = Hit.Instigator;
		IceGuardUntil = GetWorld() ? GetWorld()->GetTimeSeconds() + IceGuardTime : -1.0;
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Shards(this, GetActorLocation(), 16, 900.f, FLinearColor(0.7f, 0.92f, 1.f));
		APinguFX::Ring(this, GetAimOrigin(), 75.f * SizeScale, FLinearColor(0.7f, 0.95f, 1.f), 0.25f);
		APinguFX::Sound(this, EPinguSound::Shatter);
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s's ice took the %s"), *DisplayName, UPenguinAbilityComponent::GetInfo(Hit.Source).Name);
		return false;
	}
	// Hot Feet can't be frozen: the frost hisses off it. It counts as reached (the breath does not try again every frame),
	// but nothing happens - no hit for the statistics, no push.
	if (Hit.Freeze > 0.f && TraitComponent && TraitComponent->IsFreezeImmune())
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Puffs(this, GetAimOrigin(), 6, 24.f * SizeScale, 180.f, FVector::UpVector, 0.7f, FLinearColor(0.9f, 0.9f, 0.93f));
		APinguFX::Sparkles(this, GetFeetLocation(), 4, 30.f * SizeScale, FLinearColor(1.f, 0.45f, 0.1f));
		APinguFX::Sound(this, EPinguSound::Melt, 0.7f);
		UE_LOG(LogPingu, Log, TEXT("[TRAITS] %s (Hot Feet) can't be frozen"), *DisplayName);
		return true;
	}

	APenguinCharacter* By = Hit.Instigator.Get();
	RememberAttacker(By);
	// The hit lands (every way on from here does something to us): the match statistics count it for By.
	if (APenguinGameMode* GameMode = By && By != this && GetWorld() ? GetWorld()->GetAuthGameMode<APenguinGameMode>() : nullptr)
	{
		GameMode->OnPenguinHit(By, this);
	}
	// The attacker's trait hears of it (Leech).
	if (UPenguinTraitComponent* ByTraits = By && By != this ? By->GetTraits() : nullptr)
	{
		ByTraits->OnHitLanded(this);
	}

	if (Hit.bDeadly)
	{
		KnockOut(Hit.Reason, By, Hit.Knockback); // a knock-out's launch is not a knockback: never scaled
		return true;
	}

	// Size matters, a lot - relative to the attacker's: a penguin smaller than whoever hits it flies much further, a bigger
	// one is heavy. (Attacker size / our size)^1.25, within 0.25 .. 4: a normal attacker sends a 0.5x penguin 2.38x as far,
	// a 2x one 0.42x; a 2x attacker a normal one 2.38x; an 8x attacker anyone normal 4x. A hit without an attacker (falling
	// ice, a blast of nobody's, our own) goes by our size alone, as if a normal-size penguin hit us (Hit.SourceSize can set
	// the size it comes from).
	// And the longer the fight, the further every knock goes (1 + 2 % a second of fight: the number on everybody's HUD).
	// Every attack's push comes through here - this is the one place it is scaled (movement of your own is not).
	const APenguinGameState* FlowState = APenguinGameState::Get(this);
	const float RoundKnockback = FlowState ? FlowState->GetKnockbackScale() : 1.f;
	const float SourceSize = Hit.SourceSize > 0.f ? Hit.SourceSize : (By && By != this ? By->GetSizeScale() : 1.f);
	const float SizeRatio = FMath::Max(SourceSize, 0.1f) / FMath::Max(SizeScale, 0.1f);
	const float SizeKnockback = FMath::Clamp(FMath::Pow(SizeRatio, SizeKnockbackPower), SizeKnockbackMin, SizeKnockbackMax) * RoundKnockback;
	// Traits mode, the one rule: the knock goes x the strength of the use it comes from x the attacker's KnockbackDealt x
	// our KnockbackTaken (all 1 without traits). A knock-out's launch (above), a status hit's nudge and the fish hook's
	// throw (it flies its own arc) are not scaled.
	const UPenguinTraitComponent* AttackerTraits = By && By != this ? By->GetTraits() : nullptr;
	// (Damage %: x 1 + the damage we took this round.)
	const float TraitKnockback = FMath::Max(Hit.Strength, 0.f) * (AttackerTraits ? AttackerTraits->GetKnockbackDealt() : 1.f)
		* (TraitComponent ? TraitComponent->GetKnockbackTaken() * TraitComponent->GetDamageKnockback() : 1.f);

	// Ice gun and size rays: status changes, usually without a real knock.
	const bool bStatusHit = Hit.Slip > 0.f || !FMath::IsNearlyEqual(Hit.SizeFactor, 1.f);
	if (Hit.Slip > 0.f)
	{
		SetSlippery(Hit.Slip);
	}
	if (!FMath::IsNearlyEqual(Hit.SizeFactor, 1.f))
	{
		// A size ray's hit is one size step whatever the shot says (PinguGrowRayFactor / PinguShrinkRayFactor: eight grow
		// hits are 8x, two shrink hits 0.5x); any other size change as it comes. Past a limit it knocks us out, By's.
		float Factor = Hit.SizeFactor;
		if (IsSizeRay(Hit.Source))
		{
			Factor = Hit.SizeFactor > 1.f ? PinguGrowRayFactor : PinguShrinkRayFactor;
		}
		ChangeSize(Factor, PinguTuning::SizeRayDuration * PinguTraitTuning::GetDurationScale(Hit.Strength), By);
		if (bKnockedOut)
		{
			return true;
		}
	}
	if (bStatusHit && Hit.Knockback.SizeSquared() < 1.f)
	{
		return true;
	}

	if (Hit.Freeze > 0.f)
	{
		PenguinMovement->SetFrozen(Hit.Freeze); // reaches a remote owner by itself (movement component)
		NudgeVelocityX(Hit.Knockback.X * SizeKnockback);
		ShowIceBlock(true);
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::Freeze);
		APinguFX::Flakes(this, GetActorLocation(), 8, 350.f, FLinearColor(0.75f, 0.95f, 1.f));
		APinguFX::Sparkles(this, GetActorLocation(), 6, 50.f, FLinearColor(0.7f, 0.95f, 1.f));
		return true;
	}

	float Scale = 1.f;
	if (PenguinMovement->IsFrozen() && !bStatusHit && (UPenguinAbilityComponent::IsWind(Hit.Source) || Hit.bBellySlide))
	{
		// Wind and a belly slide don't break the ice: the ice cube slides off (no stun, no turning round - it is a block of
		// ice), still frozen and still a shield. It slides far on its slippery ice anyway: a gentler push.
		NudgeVelocityX(Hit.Knockback.X * IceCubePushScale * SizeKnockback * TraitKnockback);
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::IcicleHit, 0.8f, 0.05f, 0.75f);
		APinguFX::Sparkles(this, Hit.Location, 6, 40.f * SizeScale, FLinearColor(0.7f, 0.95f, 1.f));
		APinguFX::Shake(this, 0.12f);
		return true;
	}
	if (PenguinMovement->IsFrozen() && !bStatusHit && Hit.Source == EPenguinAbility::FishHook)
	{
		// The fish hook doesn't break the ice: it yanks the whole ice cube, heavy, with the penguin inside (the hook sets
		// the arc - about halfway to its thrower - and BeginIceThrow: the ice shatters where it hits something).
		PenguinMovement->ApplyKnockback(Hit.Knockback * SizeKnockback, Hit.Stun); // reaches a remote owner by itself
		SeenHit = PenguinMovement->HitEvents;
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::IcicleHit, 0.9f, 0.05f, 0.6f);
		APinguFX::Sparkles(this, Hit.Location, 8, 40.f * SizeScale, FLinearColor(0.7f, 0.95f, 1.f));
		APinguFX::Shake(this, 0.15f);
		return true;
	}
	if (PenguinMovement->IsFrozen() && !bStatusHit)
	{
		// Anything else that gets here breaks the ice and sends it flying further (no attack does now: every one is
		// handled above - an ice bolt or a ray leaves the ice alone).
		Scale = 1.35f;
		PenguinMovement->SetFrozen(0.f);
		ShowIceBlock(false);
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Shards(this, GetActorLocation(), 12, 800.f, FLinearColor(0.7f, 0.92f, 1.f));
		APinguFX::Sound(this, EPinguSound::Shatter);
	}
	Scale *= SizeKnockback;
	if (bStatusHit)
	{
		NudgeVelocityX(Hit.Knockback.X * Scale); // a nudge, not a stun
		return true;
	}
	Scale *= TraitKnockback;
	float Stun = Hit.Stun;
	bool bSlapDizzy = false;
	if (Hit.Source == EPenguinAbility::FlipperSlap && GetWorld())
	{
		// The slap stuns (two seconds); another one during that stun or right after it only knocks - no stun-locking.
		const double Now = GetWorld()->GetTimeSeconds();
		if (Now < SlapStunFreeUntil)
		{
			Stun = FMath::Min(Stun, SlapGraceStun);
		}
		else
		{
			SlapStunFreeUntil = Now + Stun + SlapStunGrace;
			bSlapDizzy = Stun > 0.f;
		}
	}
	PenguinMovement->ApplyKnockback(Hit.Knockback * Scale, Stun); // reaches a remote owner by itself
	if (TraitComponent)
	{
		TraitComponent->OnKnockbackTaken(By, Hit.Knockback * Scale); // Damage % grows, Recoil pushes the attacker back
	}
	if (bSlapDizzy)
	{
		// Dizzy: no ability for that stun either (the hit stun of every other attack leaves abilities alone). The flinch
		// of a slap within the grace is no dizzy stun.
		PenguinMovement->SetSlapDizzy(Stun);
	}
	SeenHit = PenguinMovement->HitEvents;

	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sound(this, EPinguSound::Hit);
	if (FMath::FRand() < 0.6f)
	{
		APinguFX::Sound(this, EPinguSound::Squawk, 0.7f, 0.12f);
	}
	APinguFX::Stars(this, GetActorLocation(), 3, 300.f);
	APinguFX::Puffs(this, Hit.Location, 5, 20.f, 260.f, FVector(Hit.Knockback.X, 0.f, Hit.Knockback.Y).GetSafeNormal(), 1.f);
	APinguFX::HitStop(this, 0.045f);
	APinguFX::Shake(this, 0.22f);
	return true;
}

void APenguinCharacter::ShowComboHit(int32 Count)
{
	if (!HasAuthority())
	{
		return;
	}
	// Every machine's overhead widget pops the number when the serial changes (never 0 on the wire: 0 is "never popped").
	ComboPopCount = static_cast<uint8>(FMath::Clamp(Count, 1, 255));
	ComboPopSerial = ComboPopSerial == 255 ? 1 : static_cast<uint8>(ComboPopSerial + 1);
}

void APenguinCharacter::RememberAttacker(APenguinCharacter* By)
{
	// (A teammate too: friendly fire. Its knock-out never counts for its statistics: APenguinGameMode::OnPenguinKnockedOut.)
	if (HasAuthority() && By && By != this && GetWorld())
	{
		LastAttacker = By;
		LastAttackTime = GetWorld()->GetTimeSeconds();
	}
}

APenguinCharacter* APenguinCharacter::GetLastAttacker(float MaxAge) const
{
	const UWorld* World = GetWorld();
	return World && LastAttacker.IsValid() && World->GetTimeSeconds() - LastAttackTime <= MaxAge ? LastAttacker.Get() : nullptr;
}

float APenguinCharacter::GetDeathShowTime(EPenguinKOReason Reason)
{
	return PenguinCharacterPrivate::KOHideTime(Reason);
}

void APenguinCharacter::BeginIceThrow(float FlightTime)
{
	using namespace PenguinCharacterPrivate;
	if (!HasAuthority() || bKnockedOut || !PenguinMovement || !PenguinMovement->IsFrozen())
	{
		return;
	}
	bIceThrown = true;
	IceThrowAge = 0.f;
	IceThrowLimit = FMath::Max(FlightTime, 0.f) + IceThrowExtraTime;
	IceThrowLastVelocity = PenguinMovement->Velocity;
}

void APenguinCharacter::UpdateIceThrow(float Dt)
{
	using namespace PenguinCharacterPrivate;
	if (!bIceThrown)
	{
		return;
	}
	if (!PenguinMovement || !PenguinMovement->IsFrozen() || bKnockedOut || IsHidden())
	{
		bIceThrown = false; // thawed on the way (another hit took the ice), or out
		return;
	}
	// The server's copy of a LAN player's penguin has the owner's movement mode and velocity (it flies on the owner's
	// machine): the same checks work for every penguin.
	IceThrowAge += Dt;
	const FVector Velocity = PenguinMovement->Velocity;
	const FVector Before = IceThrowLastVelocity;
	IceThrowLastVelocity = Velocity;
	if (IceThrowAge < IceThrowArmTime)
	{
		return;
	}
	if (PenguinMovement->IsMovingOnGround())
	{
		ShatterIceOnImpact(TEXT("the ground"));
		return;
	}
	if (FMath::Abs(Before.X) > IceThrowWallSpeed && FMath::Abs(Velocity.X) < IceThrowWallKeep * FMath::Abs(Before.X))
	{
		// Stopped sideways: a wall if there is a block right beside us that way (a gust blowing the cube back is not one).
		const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
		const UCapsuleComponent* Capsule = GetCapsuleComponent();
		const float Side = FMath::Sign(static_cast<float>(Before.X));
		const FVector Probe = GetActorLocation() + FVector(Side * (Capsule->GetScaledCapsuleRadius() + IceThrowWallProbe), 0.f, 0.f);
		const float Half = Capsule->GetScaledCapsuleHalfHeight() * 0.6f;
		if (Island && (Island->IsSolidAt(Probe) || Island->IsSolidAt(Probe + FVector(0.f, 0.f, Half)) || Island->IsSolidAt(Probe - FVector(0.f, 0.f, Half))))
		{
			ShatterIceOnImpact(TEXT("a wall"));
			return;
		}
	}
	if (IceThrowAge > IceThrowLimit)
	{
		bIceThrown = false; // still flying long after the throw (off the island): the ice thaws in its time
	}
}

void APenguinCharacter::ShatterIceOnImpact(const TCHAR* What)
{
	bIceThrown = false;
	if (!PenguinMovement || !PenguinMovement->IsFrozen())
	{
		return;
	}
	PenguinMovement->SetFrozen(0.f); // reaches a remote owner by itself
	ShowIceBlock(false);
	FPinguFXBroadcastScope Broadcast(this);
	const FVector Center = GetActorLocation();
	APinguFX::Shards(this, Center, 18, 950.f, FLinearColor(0.7f, 0.92f, 1.f));
	APinguFX::Puffs(this, GetFeetLocation(), 5, 30.f * SizeScale, 300.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.95f, 1.f));
	APinguFX::Ring(this, Center, 85.f * SizeScale, FLinearColor(0.7f, 0.95f, 1.f), 0.25f);
	APinguFX::Sound(this, EPinguSound::Shatter);
	APinguFX::Shake(this, 0.25f);
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's ice shattered on %s"), *DisplayName, What);
}

void APenguinCharacter::ClashWith(APenguinCharacter* Pusher)
{
	using namespace PenguinCharacterPrivate;
	FPinguFXBroadcastScope Broadcast(this);
	const FVector Chest = GetAimOrigin();
	APinguFX::Ring(this, Chest, ClashRingRadius * SizeScale, ClashColor, ClashRingTime);
	APinguFX::Sparkles(this, Chest, ClashSparkles, ClashSparkleRadius * SizeScale, ClashColor);
	APinguFX::Sound(this, EPinguSound::Slap, ClashVolume, 0.05f, ClashPitch);
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's push clashed on braced %s"), Pusher ? *Pusher->GetDisplayName() : TEXT("nobody"), *DisplayName);
	UPenguinMovementComponent* PusherMove = Pusher && Pusher != this ? Pusher->GetPenguinMovement() : nullptr;
	if (!PusherMove)
	{
		return;
	}
	// The pusher bounces off away from us along X (right above or below us: back against its facing), a recoil like the
	// abilities' own: it reaches a LAN player's own machine.
	const float Dx = static_cast<float>(Pusher->GetActorLocation().X - GetActorLocation().X);
	const float Side = FMath::Abs(Dx) > 1.f ? FMath::Sign(Dx) : -Pusher->GetFacing();
	PusherMove->AddAbilityImpulse(FVector2D(Side * ClashRecoilSide, ClashRecoilUp));
}

void APenguinCharacter::KnockOut(EPenguinKOReason Reason, APenguinCharacter* By, const FVector2D& Launch)
{
	// A knock-out is a match event: only the server has them (the clients get the replicated state, the owner its RPCs).
	if (!HasAuthority() || bKnockedOut)
	{
		return;
	}
	UWorld* World = GetWorld();
	bKnockedOut = true;
	bKOHidden = false;
	KOTime = 0.f;
	KOReason = Reason;
	bIceThrown = false;

	// Pushed into the sea? The last penguin that hit you gets the point.
	if (!By && LastAttacker.IsValid() && World && World->GetTimeSeconds() - LastAttackTime < 5.f)
	{
		By = LastAttacker.Get();
	}
	// Our own doing (our own snow bomb): a self-knock-out. It counts for nobody - the point does not go to the last penguin
	// that hit us either (that fallback above only runs when the knock-out has no maker at all).
	bKOSelfInflicted = By == this;
	if (By == this)
	{
		By = nullptr;
	}
	KOBy = By;

	if (PenguinMovement)
	{
		PenguinMovement->SetKnockedOut(true);
	}
	ShowIceBlock(false);
	OnShieldChanged(false);
	// Freeze the size while knocked out (ResetForRound brings it back to 1): no rescaling or 'wore off' sound while hidden.
	SizeTimeLeft = 0.f;
	SizeTarget = SizeScale;

	FVector LaunchVelocity = FVector::ZeroVector;
	if ((Reason == EPenguinKOReason::Stomp || Reason == EPenguinKOReason::Bomb || PenguinCharacterPrivate::IsSizeDeath(Reason)) && PenguinMovement)
	{
		// Squashed flat on the spot (the death look's pancake), blackened on the spot to crumble into a pile of ash there
		// (the snow bomb's), shrinking to nothing or puffing up until it pops (a size limit): no launch - it only drops on
		// down if it was in the air.
		LaunchVelocity = FVector(0.f, 0.f, FMath::Min(static_cast<float>(PenguinMovement->Velocity.Z), 0.f));
		PenguinMovement->Velocity = LaunchVelocity;
		PenguinMovement->SetMovementMode(MOVE_Falling);
	}
	else if ((Reason == EPenguinKOReason::Iceberg || Reason == EPenguinKOReason::Crushed) && PenguinMovement)
	{
		// Pressed flat under the ice: the pancake squirts out sideways along the ground (not up through the block).
		const float Side = FMath::Abs(Launch.X) > 10.f ? FMath::Sign(Launch.X) : -GetFacing();
		LaunchVelocity = FVector(Side * 900.f, 0.f, FMath::Min(static_cast<float>(PenguinMovement->Velocity.Z), 0.f));
		PenguinMovement->Velocity = LaunchVelocity;
		PenguinMovement->SetMovementMode(MOVE_Falling);
	}
	else if (Reason != EPenguinKOReason::Sea && PenguinMovement)
	{
		const float Side = FMath::Abs(Launch.X) > 10.f ? FMath::Sign(Launch.X) : -GetFacing();
		LaunchVelocity = FVector(Side * FMath::Max(FMath::Abs(Launch.X), 500.f), 0.f, FMath::Max(static_cast<float>(Launch.Y), 950.f));
		PenguinMovement->Velocity = LaunchVelocity;
		PenguinMovement->SetMovementMode(MOVE_Falling);
	}
	if (IsRemotePlayerPenguin())
	{
		Rq07(Reason, FVector_NetQuantize10(LaunchVelocity)); // before Rq08 (reliable, in order)
	}
	// The cartoon death on this machine (every other machine starts its own from the replicated state: UpdateDeathLook).
	StartDeathLook();

	if (Reason == EPenguinKOReason::Sea)
	{
		float Sea = GetActorLocation().Z;
		if (const ABlockIsland* Island = ABlockIsland::FindNear(World, GetActorLocation()))
		{
			Sea = Island->SeaLevel;
		}
		{
			// A big splash: the splash and a tall spout of water (the bubbles and the ghost are the death look's).
			FPinguFXBroadcastScope Broadcast(this);
			const FVector Water(GetActorLocation().X, 0.f, Sea + 50.f);
			APinguFX::Splash(this, Water);
			APinguFX::Puffs(this, Water, 6, 38.f, 700.f, FVector::UpVector, 0.35f, FLinearColor(0.55f, 0.8f, 1.f));
			APinguFX::Sound(this, EPinguSound::Splash);
		}
		FinishKnockOut();
	}
	else
	{
		using namespace PenguinCharacterPrivate;
		FPinguFXBroadcastScope Broadcast(this);
		const FVector Here = GetActorLocation();
		switch (Reason)
		{
		case EPenguinKOReason::Stomp:
			// Splat: a ring and snow shooting out flat along the ground, a few stars, a squeak.
			APinguFX::Ring(this, GetFeetLocation() + FVector(0.f, 0.f, 6.f), 110.f * SizeScale, FLinearColor::White, 0.3f);
			APinguFX::GroundPuffs(this, GetFeetLocation(), 1.6f);
			APinguFX::Stars(this, Here, 6, 420.f);
			APinguFX::Sound(this, EPinguSound::Squawk, 0.9f, 0.08f, 1.7f);
			break;
		case EPenguinKOReason::Iceberg:
		case EPenguinKOReason::Crushed:
			// Pressed flat: feathers burst out (white and black), a puff of snow, a squeak.
			APinguFX::Flakes(this, Here, 8, 520.f, FeatherWhite);
			APinguFX::Flakes(this, Here, 5, 460.f, FeatherBlack);
			APinguFX::Puffs(this, Here, 6, 30.f, 380.f, FVector::UpVector, 1.f);
			APinguFX::Sound(this, EPinguSound::Squawk, 0.9f, 0.08f, 1.6f);
			break;
		case EPenguinKOReason::Icicle:
			// Frozen solid on the spot (the squawk never gets out): the freeze, frost and glints.
			APinguFX::Sound(this, EPinguSound::Freeze);
			APinguFX::Flakes(this, Here, 8, 350.f, IceDeathColor);
			APinguFX::Sparkles(this, Here, 6, 50.f, IceDeathColor);
			break;
		case EPenguinKOReason::Bomb:
			// Blown up: feathers everywhere, a cloud of smoke going up over its head (its face stays in sight: it chars),
			// embers, a croak.
			APinguFX::Flakes(this, Here, 8, 700.f, FeatherWhite);
			APinguFX::Flakes(this, Here, 5, 650.f, FeatherBlack);
			APinguFX::Puffs(this, Here + FVector(0.f, -10.f, 55.f) * SizeScale, 6, 30.f, 300.f, FVector::UpVector, 0.6f, BlastSmokeColor);
			APinguFX::Sparkles(this, Here, 4, 45.f, EmberColor);
			APinguFX::Sound(this, EPinguSound::Squawk, 0.9f, 0.1f, 0.8f);
			break;
		case EPenguinKOReason::Shrunk:
			// Too small: a startled high squeak and violet glints as it starts shrinking away (the pop is FinishKnockOut's).
			APinguFX::Sparkles(this, Here, 6, 30.f * SizeScale, ShrinkColor);
			APinguFX::Sound(this, EPinguSound::Shrink, 0.8f, 0.05f, 1.4f);
			APinguFX::Sound(this, EPinguSound::Squawk, 0.7f, 0.08f, 2.1f);
			break;
		case EPenguinKOReason::Overgrown:
			// Too big: it creaks and strains as it swells (a low, stretched grow and a strangled squawk), green glints; the
			// blast comes when it pops (OvergrownBlast).
			APinguFX::Sparkles(this, Here, 10, 40.f * SizeScale, GrowColor);
			APinguFX::Sound(this, EPinguSound::Grow, 1.f, 0.03f, 0.6f);
			APinguFX::Sound(this, EPinguSound::Squawk, 0.8f, 0.05f, 0.55f);
			break;
		default:
			APinguFX::Stars(this, Here, 8, 520.f);
			APinguFX::Puffs(this, Here, 10, 30.f, 380.f, FVector::UpVector, 1.f);
			APinguFX::Sound(this, EPinguSound::Squawk, 0.9f, 0.1f, 1.15f);
			break;
		}
		APinguFX::Sound(this, EPinguSound::KO);
		if (Reason == EPenguinKOReason::Overgrown)
		{
			APinguFX::Shake(this, 0.2f); // the big hit-stop and shake come with the blast
		}
		else
		{
			APinguFX::HitStop(this, 0.12f, 0.02f);
			APinguFX::Shake(this, Reason == EPenguinKOReason::Shrunk ? 0.25f : 0.5f);
		}
	}

	UE_LOG(LogPingu, Display, TEXT("%s knocked out (%s) by %s"), *DisplayName, *UEnum::GetValueAsString(Reason), By ? *By->GetDisplayName() : TEXT("nobody"));
	if (APenguinGameMode* GameMode = World ? World->GetAuthGameMode<APenguinGameMode>() : nullptr)
	{
		GameMode->OnPenguinKnockedOut(this, By, Reason);
	}
}

void APenguinCharacter::Rq07_Implementation(EPenguinKOReason Reason, const FVector_NetQuantize10& LaunchVelocity)
{
	if (HasAuthority())
	{
		return;
	}
	// Our own penguin is out: no more control, it flies off (the server plays the effects for everybody).
	bKnockedOut = true;
	bKOHidden = false;
	KOTime = 0.f;
	KOReason = Reason;
	ShowIceBlock(false);
	OnShieldChanged(false);
	SizeTarget = SizeScale;
	if (PenguinMovement)
	{
		PenguinMovement->SetKnockedOut(true);
		if (Reason != EPenguinKOReason::Sea)
		{
			PenguinMovement->Velocity = LaunchVelocity;
			PenguinMovement->SetMovementMode(MOVE_Falling);
		}
	}
}

void APenguinCharacter::FinishKnockOut()
{
	if (!HasAuthority() || bKOHidden)
	{
		return;
	}
	bKOHidden = true;
	if (KOReason == EPenguinKOReason::Shrunk)
	{
		// Shrunk to nothing: a tiny pop - a little puff, violet glints and a squeak, where it was.
		using namespace PenguinCharacterPrivate;
		FPinguFXBroadcastScope Broadcast(this);
		const FVector Pop = GetActorLocation() + GetDeathFloorCentre(ShrunkMinScale) * SizeScale;
		APinguFX::Puffs(this, Pop, 5, 9.f * SizeScale, 140.f, FVector::UpVector, 1.f);
		APinguFX::Sparkles(this, Pop, 6, 18.f * SizeScale, ShrinkColor);
		APinguFX::Sound(this, EPinguSound::Squawk, 0.6f, 0.05f, 2.6f);
		APinguFX::Sound(this, EPinguSound::UIClick, 0.8f, 0.05f, 1.6f);
	}
	else if (KOReason != EPenguinKOReason::Sea && KOReason != EPenguinKOReason::Bomb && KOReason != EPenguinKOReason::Overgrown)
	{
		// Poof (where the body is: a stomp's pancake lies on the floor). The ghost floating up is every machine's own. (A
		// snow bomb's penguin is an ash pile by now, which goes its own way on every machine: UpdateDeathLook. An overgrown
		// one has just popped: OvergrownBlast.)
		FVector Poof = GetActorLocation();
		if (KOReason == EPenguinKOReason::Stomp && GetCapsuleComponent())
		{
			Poof.Z -= GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.7f;
		}
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Puffs(this, Poof, 12, 36.f, 300.f, FVector::UpVector, 1.f);
		APinguFX::Sparkles(this, Poof, 8, 60.f, FLinearColor(1.f, 0.9f, 0.5f));
	}
	SetActorHiddenInGame(true); // replicates (the penguin stays relevant: bAlwaysRelevant)
	SetActorEnableCollision(false);
	if (PenguinMovement)
	{
		PenguinMovement->StopMovementImmediately();
		PenguinMovement->DisableMovement();
	}
	if (IsRemotePlayerPenguin())
	{
		Rq08();
	}
}

void APenguinCharacter::OvergrownBlast()
{
	using namespace PenguinCharacterPrivate;
	UWorld* World = GetWorld();
	if (!HasAuthority() || !World)
	{
		return;
	}
	// From the body's middle: its reach is past the capsule (an 8x penguin is about 9 m tall), and it pushes like a snow
	// bomb's outer zone, harder - nobody is knocked out by it. The knock-out's maker (who grew it) gets the pushes;
	// they go by the victims' size alone (a blast has no size of its own: SourceSize 1).
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const FVector Center = GetActorLocation();
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : CapsuleHalfHeight * SizeScale;
	const float Reach = HalfHeight + OvergrownBlastReach;
	APenguinCharacter* Maker = KOBy.Get();
	FPinguFXBroadcastScope Broadcast(this);
	bool bHitSomeone = false;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Other = *It;
		if (Other == this || !IsValid(Other) || !Other->IsAlive() || Other->IsHidden())
		{
			continue;
		}
		const float Dist = Other->DistanceTo(Center);
		if (Dist > Reach)
		{
			continue;
		}
		FVector Dir = Other->GetActorLocation() - Center;
		Dir.Y = 0.f;
		Dir = Dir.GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			Dir = FVector::UpVector;
		}
		const float Falloff = 1.f - 0.5f * FMath::Clamp(Dist / Reach, 0.f, 1.f);
		FPenguinHit Hit;
		Hit.Instigator = Maker;
		Hit.Source = EPenguinAbility::SnowBomb; // a blast like a snow bomb's: it smashes a frozen penguin's ice, a shell holds
		Hit.SourceSize = 1.f;
		Hit.Knockback = FVector2D(static_cast<float>(Dir.X) * OvergrownBlastPush * Falloff,
			FMath::Max(static_cast<float>(Dir.Z) * OvergrownBlastPush * Falloff, 0.f) + OvergrownBlastLift);
		Hit.Stun = OvergrownBlastStun;
		Hit.Location = Other->GetActorLocation();
		bHitSomeone |= Other->ApplyPenguinHit(Hit);
	}
	APinguPickup::PushInArea(World, Center, Reach * 1.2f, OvergrownPickupPush);

	// The look: the snow bomb's explosion at the blast's size, feathers and confetti everywhere, a ring, a big bang and a
	// last squawk, a big shake.
	APinguFX::Explosion(this, Center, Reach);
	APinguFX::Ring(this, Center, Reach * 0.7f, GrowColor, 0.35f);
	APinguFX::Flakes(this, Center, 14, 1100.f, FeatherWhite);
	APinguFX::Flakes(this, Center, 8, 1000.f, FeatherBlack);
	for (const FLinearColor& Confetti : ConfettiColors)
	{
		APinguFX::Flakes(this, Center, 5, 950.f, Confetti);
	}
	APinguFX::Stars(this, Center, 8, 800.f);
	APinguFX::Sound(this, EPinguSound::Explosion, 1.f, 0.03f, 0.8f);
	APinguFX::Sound(this, EPinguSound::Squawk, 0.9f, 0.05f, 1.5f);
	APinguFX::Shake(this, 0.9f);
	APinguFX::HitStop(this, bHitSomeone ? 0.1f : 0.06f);
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s popped at size %.2f: blast reach %.0f cm, hit somebody %d"), *DisplayName, SizeScale, Reach, bHitSomeone ? 1 : 0);
}

bool APenguinCharacter::HasAshFloor() const
{
	const float Reach = PenguinCharacterPrivate::AshFloorReach * SizeScale;
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), DeathAshBase);
	float GroundZ = 0.f;
	return Island && Island->FindGroundBelow(DeathAshBase + FVector(0.f, 0.f, Reach), 2.f * Reach, GroundZ)
		&& FMath::Abs(static_cast<float>(DeathAshBase.Z) - GroundZ) <= Reach;
}

void APenguinCharacter::Rq08_Implementation()
{
	if (HasAuthority() || bKOHidden)
	{
		return;
	}
	bKOHidden = true;
	SetActorEnableCollision(false); // (hidden comes with the replicated actor state)
	if (PenguinMovement)
	{
		PenguinMovement->StopMovementImmediately();
		PenguinMovement->DisableMovement();
	}
}

void APenguinCharacter::OnRep_KnockOut()
{
	// Somebody else's penguin on a LAN client.
	if (bKnockedOut)
	{
		ShowIceBlock(false);
		OnShieldChanged(false);
		if (bDeathIceShown && IceBlock)
		{
			IceBlock->SetHiddenInGame(false); // (the death look's own ice: an icicle knock-out)
		}
	}
	else if (bDeathLookActive)
	{
		EndDeathLook(); // back in (a new round): it looks normal at once
	}
	SetActorEnableCollision(!bKOHidden); // collision does not replicate: off while knocked out and gone, as on the server
}

// ---------------------------------------------------------------------------- the death look (every machine)

void APenguinCharacter::StartDeathLook()
{
	using namespace PenguinCharacterPrivate;
	bDeathLookActive = true;
	DeathLook = KOReason;
	DeathLookTime = 0.f;
	DeathSpin = 0.f;
	DeathFxTimer = 0.f;
	DeathHitCount = PenguinMovement ? PenguinMovement->HitEvents : 0;
	bDeathSpiritDone = false;
	bDeathSinkDone = false;
	bDeathCoughDone = false;
	if (bDeathAshShown)
	{
		bDeathAshShown = false;
		APinguFX::HideAshPile(this); // (a pile from before: never two at once)
	}
	DeathAshStart = AshNotCrumbled;
	DeathAshBlown = 0.f;
	bDeathAshDone = false;
	bDeathAshSettled = false;
	bDeathGustDone = false;
	bDeathAshOnIsland = false;
	DeathAshFloorTimer = 0.f;
	DeathEmberTimer = 0.f;
	DeathCharDone = 0.f;
	DeathEyesLandP = -1.f;
	bDeathFaceEyesKnown = false;
	DeathFaceEyeRadius = 0.f;
	DeathLastLocation = GetActorLocation();
	const float Vx = static_cast<float>(GetVelocity().X);
	DeathSide = FMath::Abs(Vx) > 100.f ? FMath::Sign(Vx) : -GetFacing();
	DeathSeaZ = static_cast<float>(DeathLastLocation.Z) - 60.f;
	if (const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), DeathLastLocation))
	{
		DeathSeaZ = Island->SeaLevel;
	}
	if (IsHidden())
	{
		return; // gone already (the sea, or a knock-out from before this machine saw it): only effects, no costume
	}
	if (DeathLook == EPenguinKOReason::Icicle && IceBlock)
	{
		// Frozen solid: the ice block (as a freeze shows it) around the penguin, until it shatters.
		bDeathIceShown = true;
		IceBlock->SetHiddenInGame(false);
	}
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (DeathLook == EPenguinKOReason::Bomb && SkelMesh)
	{
		// Charred: the afterimages' see-through material over the whole penguin (every LOD, every setting), from nothing at
		// first - UpdateDeathLook lets it creep in until it is solid black.
		if (!SootMaterial)
		{
			if (UMaterialInterface* SootBase = LoadObject<UMaterialInterface>(nullptr, SootMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn))
			{
				SootMaterial = UMaterialInstanceDynamic::Create(SootBase, this);
			}
		}
		if (SootMaterial)
		{
			SootMaterial->SetVectorParameterValue(TEXT("Color"), CharHotColor);
			SootMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.f);
			DeathCharDone = 0.f;
			OverlayBeforeSoot = SkelMesh->GetOverlayMaterial();
			SkelMesh->SetOverlayMaterial(SootMaterial);
			bDeathSootShown = true;
		}
	}
}

void APenguinCharacter::EndDeathLook()
{
	using namespace PenguinCharacterPrivate;
	const bool bWasShaping = bDeathLookActive && DeathLookShapesMesh(DeathLook);
	bDeathLookActive = false;
	bDeathMeshPlaced = false;
	DeathLook = EPenguinKOReason::None;
	DeathLookTime = 0.f;
	DeathSpin = 0.f;
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (SkelMesh)
	{
		// The knocked-out mesh stopped animating while hidden (UpdateDeathLook): animate every frame again.
		SkelMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	}
	if (bWasShaping && SkelMesh)
	{
		// The mesh as the squash & stretch leaves it at rest (UpdateJuice takes over again from the next frame).
		SkelMesh->SetRelativeScale3D(GetBaseMeshScale());
		SkelMesh->SetRelativeLocationAndRotation(GetBaseTranslationOffset(), FRotator(0.f, MeshYaw, 0.f));
	}
	if (bDeathIceShown)
	{
		bDeathIceShown = false;
		if (IceBlock)
		{
			IceBlock->SetHiddenInGame(!bIceShown);
			IceBlock->SetRelativeLocationAndRotation(IceBlockOffset, FRotator::ZeroRotator);
		}
	}
	if (bDeathSootShown)
	{
		bDeathSootShown = false;
		if (SkelMesh)
		{
			SkelMesh->SetOverlayMaterial(OverlayBeforeSoot);
		}
		OverlayBeforeSoot = nullptr;
	}
	if (bDeathStarsShown)
	{
		bDeathStarsShown = false;
		if (DizzyStars)
		{
			DizzyStars->SetHiddenInGame(!bDizzy);
		}
	}
	if (bDeathAshShown)
	{
		// The ash pile still there (back in before the wind had taken it all - a practice or lobby respawn): gone in a
		// puff of ash. (Not crumbled yet, only the eyes on the charred penguin's face showed: they just go.)
		bDeathAshShown = false;
		if (DeathAshStart > AshNotCrumbled)
		{
			APinguFX::Puffs(this, DeathAshBase + FVector(0.f, 0.f, 15.f * SizeScale), 6, 20.f * SizeScale, 160.f, FVector::UpVector, 1.f,
				AshPuffColor);
		}
		APinguFX::HideAshPile(this);
	}
	DeathAshStart = PenguinCharacterPrivate::AshNotCrumbled;
	DeathAshBlown = 0.f;
	bDeathAshDone = false;
}

FVector APenguinCharacter::GetDeathFloorCentre(float ScaleZ) const
{
	// Upright, the body's middle is the actor's middle; squashed ScaleZ high with the feet still on the floor, it sinks by
	// the share of the half height it lost.
	const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : PenguinCharacterPrivate::CapsuleHalfHeight;
	return FVector(0.f, 0.f, -HalfHeight * (1.f - ScaleZ));
}

bool APenguinCharacter::FindDeathFloor(FVector& OutBase) const
{
	// The island's ground right under the feet (every machine knows the island, whoever simulates the penguin), else the
	// movement's own floor (an iceberg or an ice wall is no island ground).
	const FVector Feet = GetFeetLocation();
	const float Reach = PenguinCharacterPrivate::AshFloorReach * SizeScale;
	OutBase = FVector(Feet.X, 0.f, Feet.Z);
	if (const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Feet))
	{
		float GroundZ = 0.f;
		if (Island->FindGroundBelow(Feet + FVector(0.f, 0.f, Reach), 2.f * Reach, GroundZ) && Feet.Z - GroundZ <= Reach)
		{
			OutBase.Z = GroundZ;
			return true;
		}
	}
	return PenguinMovement && PenguinMovement->IsMovingOnGround();
}

bool APenguinCharacter::IsDeathAshPileShown() const
{
	// What the effects actor really shows (not only what the look thinks): the tests check it is gone after a reset.
	return APinguFX::IsAshPileShown(this);
}

float APenguinCharacter::GetDeathSootCoverage() const
{
	// What the soot material itself says (the tests check it really went black): the share of a surface's middle it hides.
	UMaterialInstanceDynamic* Soot = SootMaterial.Get();
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (!bDeathSootShown || !Soot || !SkelMesh || SkelMesh->GetOverlayMaterial() != Soot)
	{
		return 0.f;
	}
	return FMath::Clamp(Soot->K2_GetScalarParameterValue(TEXT("Opacity")) * PenguinCharacterPrivate::SootMiddleShare, 0.f, 1.f);
}

float APenguinCharacter::GetDeathCrumbleTime() const
{
	return bDeathLookActive && DeathAshStart > PenguinCharacterPrivate::AshNotCrumbled ? DeathAshStart : -1.f;
}

bool APenguinCharacter::UpdateDeathFaceEyes()
{
	using namespace PenguinCharacterPrivate;
	const USkeletalMeshComponent* SkelMesh = GetMesh();
	if (!SkelMesh || !SkelMesh->GetSkeletalMeshAsset())
	{
		return false;
	}
	FVector Eyes[2];
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const FName Bone(CharEyeBones[Side]);
		if (SkelMesh->GetBoneIndex(Bone) == INDEX_NONE)
		{
			return false; // not the penguin rig: no white eyes on the face (the pile's come later)
		}
		Eyes[Side] = SkelMesh->GetSocketLocation(Bone);
	}
	// The one further left first (the pile's eyes are in that order), as big as the mesh scales the eyeballs now.
	const bool bSwap = Eyes[0].X > Eyes[1].X;
	DeathFaceEyes[0] = bSwap ? Eyes[1] : Eyes[0];
	DeathFaceEyes[1] = bSwap ? Eyes[0] : Eyes[1];
	const FVector Scale3 = SkelMesh->GetComponentScale();
	DeathFaceEyeRadius = CharEyeballRadius * CharEyeCover * static_cast<float>(FMath::Max(Scale3.X, Scale3.Z));
	bDeathFaceEyesKnown = true;
	return true;
}

void APenguinCharacter::SetDeathMeshTransform(const FVector& Scale, float SpinDegrees, const FVector& Centre, float TurnYaw)
{
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (!SkelMesh)
	{
		return;
	}
	// The mesh origin sits below the body's middle (its feet): scaled with the body and turned with it, so the middle
	// stays at Centre. The turn is about the camera axis (world Y: a tumble seen side on), after the yaw.
	const FVector Middle(0.f, 0.f, -GetBaseTranslationOffset().Z * Scale.Z);
	const FQuat Turn = FQuat(FVector::YAxisVector, FMath::DegreesToRadians(SpinDegrees)) * FRotator(0.f, TurnYaw, 0.f).Quaternion();
	SkelMesh->SetRelativeScale3D(GetBaseMeshScale() * Scale);
	DeathMeshLocation = Centre - Turn.RotateVector(Middle);
	bDeathMeshPlaced = true;
	SkelMesh->SetRelativeLocationAndRotation(DeathMeshLocation, Turn);
}

void APenguinCharacter::ReapplyDeathMeshLocation()
{
	// Network smoothing (a penguin this machine does not simulate) sets the mesh back to the feet-origin place after the
	// character's tick: put the death look's place back, the rotation it leaves alone.
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (bDeathMeshPlaced && bDeathLookActive && SkelMesh && PenguinCharacterPrivate::DeathLookShapesMesh(DeathLook))
	{
		SkelMesh->SetRelativeLocation(DeathMeshLocation);
	}
}

void APenguinCharacter::UpdateDeathStars(float Height, float Radius)
{
	using namespace PenguinCharacterPrivate;
	if (!DizzyStars || DizzyStars->GetInstanceCount() < 3)
	{
		return;
	}
	if (!bDeathStarsShown)
	{
		bDeathStarsShown = true;
		DizzyStars->SetHiddenInGame(false);
	}
	// The dizzy stars (the slap's), bigger, round the squashed penguin.
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const float Angle = DeathLookTime * PancakeStarTurns * 2.f * UE_PI + Index * 2.f * UE_PI / 3.f;
		const FVector Where(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, Height + 5.f * FMath::Sin(Angle * 2.f));
		DizzyStars->UpdateInstanceTransform(Index, FTransform(FRotator(0.f, DeathLookTime * 420.f + Index * 60.f, 0.f), Where,
			FVector(DizzyStarScale * PancakeStarScale)), false, false);
	}
	APinguFX::MarkInstancesChanged(DizzyStars);
}

void APenguinCharacter::PlayDeathSpirit(const FVector& Where, float RiseSpeed, const FLinearColor& Color, float SizeShare, float Life)
{
	using namespace PenguinCharacterPrivate;
	USkeletalMeshComponent* SkelMesh = GetMesh();
	if (!SkelMesh)
	{
		return;
	}
	// Upright and SizeShare as big as the penguin: its feet (the mesh origin) below Where (its middle) as the penguin's are.
	const float Size = static_cast<float>(GetActorScale3D().Z) * SizeShare;
	const FVector Feet = Where + FVector(0.f, 0.f, static_cast<float>(GetBaseTranslationOffset().Z) * Size);
	const float GhostYaw = -GetFacing() * SpiritTurn;
	// The ghost rising stands upright in the rest pose (one clean little penguin shape: the knock-out pose it would copy
	// lies on its back, its head toward the camera, a heap of see-through rings); the sea's sinking copy keeps the pose.
	const bool bGhost = RiseSpeed > 0.f;
	APinguFX::Spirit(this, SkelMesh, Feet, GhostYaw, static_cast<float>(GetBaseMeshScale().Z) * Size, RiseSpeed * Size / SizeShare,
		SpiritSway * Size, Color, Life, bGhost ? SpiritOpacity : SinkOpacity, bGhost);
}

void APenguinCharacter::UpdateDeathLook(float Dt)
{
	using namespace PenguinCharacterPrivate;
	if (!bKnockedOut)
	{
		if (bDeathLookActive)
		{
			EndDeathLook();
		}
		return;
	}
	if (!bDeathLookActive)
	{
		StartDeathLook(); // a LAN client: the replicated knock-out (or our own penguin's Rq07) came in
	}
	DeathLookTime += Dt;
	const float T = DeathLookTime;
	const bool bShown = !IsHidden();
	if (bShown)
	{
		DeathLastLocation = GetActorLocation();
	}
	const float Vx = static_cast<float>(GetVelocity().X);
	if (FMath::Abs(Vx) > 100.f)
	{
		DeathSide = FMath::Sign(Vx); // spins the way it flies
	}
	// The ghost's colour: pale, a hint of the penguin's own.
	FLinearColor GhostColor = SpiritColor * 0.8f + GetPlayerColor() * 0.2f;
	// A pancake turns from the way the penguin faced to face the camera while it is squashed.
	const float PancakeTurnYaw = FMath::Lerp(MeshYaw, PancakeYaw, FMath::Clamp(T / PancakeSquashTime, 0.f, 1.f));

	switch (DeathLook)
	{
	case EPenguinKOReason::Stomp:
		if (bShown)
		{
			// Squashed flat on the spot into a jiggling pancake facing the camera (wide across the screen, its face and
			// dizzy eyes toward us, flippers and feet flailing out to the sides), stars circling over it.
			float Flat = 1.f;
			float Wide = 1.f;
			PancakeShape(T, Flat, Wide);
			SetDeathMeshTransform(FVector(Wide, PancakeDepth(Wide), Flat), 0.f, GetDeathFloorCentre(Flat), PancakeTurnYaw);
			const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : CapsuleHalfHeight;
			UpdateDeathStars(-HalfHeight + 2.f * HalfHeight * PancakeFlat + PancakeStarHeight, PancakeStarRadius);
		}
		break;
	case EPenguinKOReason::Iceberg:
	case EPenguinKOReason::Crushed:
		if (bShown)
		{
			// Pressed into a pancake under the ice, it squirts out sideways along the ground: a pancake facing the camera
			// like a stomp's, stretched along the way it slides (across the screen), wobbling like jelly in the screen plane
			// (about the camera axis only) with its front edge up while fast, kicking up snow, and it twitches once.
			float Flat = 1.f;
			float Wide = 1.f;
			PancakeShape(T, Flat, Wide);
			const float TwitchT = (T - PancakeTwitchTime) / PancakeTwitchLength;
			if (TwitchT > 0.f && TwitchT < 1.f)
			{
				Flat *= 1.f + 0.6f * FMath::Sin(TwitchT * UE_PI) * FMath::Abs(FMath::Sin(T * 70.f));
			}
			const float SpeedShare = FMath::Clamp(FMath::Abs(Vx) / PancakeSquirtSpeed, 0.f, 1.f);
			const float Squirt = 1.f + PancakeSquirtStretch * SpeedShare;
			const float Flutter = 1.f + 0.08f * FMath::Sin(T * 31.f);
			// (A turn about the camera axis by A lifts the end at +x by -sin(A): the front edge up is -DeathSide.)
			const float Tilt = PancakeWobble * FMath::Exp(-PancakeWobbleDamp * T) * FMath::Sin(PancakeWobbleRate * T)
				- DeathSide * PancakeSkidLean * SpeedShare;
			// Mesh axes, facing the camera: X = side (along the slide, across the screen), Y = forward (toward the camera),
			// Z = up; the flutter trades width for height like jelly. Tipped, an end would dip into the floor: lifted by
			// about as much.
			const float Radius = GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleRadius() : CapsuleRadius;
			const float Across = Wide * Squirt * Flutter;
			const float Tall = Flat / Flutter;
			const float Lift = FMath::Abs(FMath::Sin(FMath::DegreesToRadians(Tilt))) * Radius * Across * 0.8f;
			SetDeathMeshTransform(FVector(Across, PancakeDepth(Wide), Tall), Tilt, GetDeathFloorCentre(Tall) + FVector(0.f, 0.f, Lift), PancakeTurnYaw);
			if (SpeedShare > 0.3f)
			{
				DeathFxTimer -= Dt;
				if (DeathFxTimer <= 0.f)
				{
					// Snow sprayed up ahead of it (fewer puffs on lower effects settings; Trail thins them out again).
					DeathFxTimer = PancakeSnowInterval / FMath::Max(UPinguSettings::GetFXDensity(), 0.5f);
					APinguFX::Trail(this, GetFeetLocation() + FVector(DeathSide * Radius * Wide * SizeScale, 0.f, 8.f * SizeScale), 22.f * SizeScale,
						FLinearColor::White, FVector(Vx * 0.3f, 0.f, 110.f));
				}
			}
		}
		break;
	case EPenguinKOReason::Icicle:
		if (bDeathIceShown && (T >= IceDeathShatterTime || !bShown))
		{
			// The ice shatters (at the latest as the penguin vanishes): the block bursts into chunks and shards flying out
			// all round (in front of the penguin), a flash, frost, a ring, the crash. This machine's own, like the whole look.
			bDeathIceShown = false;
			if (IceBlock)
			{
				IceBlock->SetHiddenInGame(!bIceShown);
				IceBlock->SetRelativeLocationAndRotation(IceBlockOffset, FRotator::ZeroRotator);
			}
			const FVector Middle = DeathLastLocation + IceBlockOffset * SizeScale;
			APinguFX::IceBurst(this, Middle, SizeScale, IceShatterColor);
			APinguFX::Sound(this, EPinguSound::Shatter);
			APinguFX::Sound(this, EPinguSound::IcicleHit, 0.5f, 0.05f, 0.8f);
		}
		DeathSpin += Dt * (bDeathIceShown ? IceDeathTumbleRate : IceDeathFreeTumbleRate) * DeathSide;
		if (bShown)
		{
			// Frozen stiff (the animation holds the frozen pose: GetAnimInputs) and tumbling away in the ice block; out of
			// it, it tumbles on faster, still stiff a moment, then flailing (the KO clip).
			SetDeathMeshTransform(FVector::OneVector, DeathSpin, FVector::ZeroVector, MeshYaw);
			if (bDeathIceShown && IceBlock)
			{
				IceBlock->SetRelativeLocationAndRotation(IceBlockOffset, FQuat(FVector::YAxisVector, FMath::DegreesToRadians(DeathSpin)));
			}
		}
		GhostColor = SpiritColor * 0.6f + IceDeathColor * 0.4f;
		break;
	case EPenguinKOReason::Bomb:
	{
		// The cartoon gag: charred black on the spot (the soot creeping in from the outline over CharTime), stiff (the frozen
		// pose: GetAnimInputs), facing the camera, only its eyes still white; it stands like that for CharHoldTime, smoking,
		// then crumbles into a heap of black ash and its eyes drop onto the heap (pile and eyes on the effects actor: they
		// stay when the penguin hides), where they blink and look around for a good while (AshRestTime); then a gust blows
		// the pile away, the eyes go last, then the ghost floats up.
		GhostColor = SpiritColor * 0.55f + BlastSmokeColor * 0.45f; // a sooty little ghost
		const float Density = FMath::Max(UPinguSettings::GetFXDensity(), 0.3f);
		const float S = SizeScale;
		const float Char = Phase01(T, 0.f, CharTime);
		if (bDeathSootShown && SootMaterial && Char > DeathCharDone)
		{
			// The soot creeps in: black at the outline first (M_Ghost is more opaque there), the middle last, a hot glow
			// going to soot black on the way; from the end of the char-up on it is solid black (and left alone).
			DeathCharDone = Char;
			SootMaterial->SetScalarParameterValue(TEXT("Opacity"), SootOpacity * FMath::Pow(Char, SootRampPower));
			SootMaterial->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(CharHotColor, SootColor, FMath::SmoothStep(0.f, 0.75f, Char)));
		}
		if (DeathAshStart <= AshNotCrumbled && bShown && T >= AshCrumbleAt && FindDeathFloor(DeathAshBase))
		{
			// On the floor: it crumbles from here (a penguin blown up in the air drops first; one that never lands before
			// it vanishes gets the poof and the ghost below instead). The wind blows the way it would have flown. It gives
			// way with a crunch: black dust rolling out along the floor, bits of charcoal tumbling off it.
			DeathAshStart = T;
			DeathAshWind = DeathSide;
			DeathFxTimer = 0.f;
			bDeathAshOnIsland = HasAshFloor();
			DeathAshFloorTimer = AshFloorCheckInterval;
			APinguFX::Sound(this, EPinguSound::Crumble, 0.9f, 0.05f, 0.85f);
			APinguFX::Puffs(this, DeathAshBase + FVector(0.f, 0.f, 45.f * S), 5, 24.f * S, 150.f, FVector::UpVector, 1.f, AshPuffColor);
			for (const float Outward : { -1.f, 1.f })
			{
				APinguFX::Puffs(this, DeathAshBase + FVector(Outward * 30.f * S, 10.f * S, 10.f * S), 3, 20.f * S, 260.f, FVector(Outward, 0.f, 0.2f), 0.35f,
					AshPuffColor);
			}
			APinguFX::Shards(this, DeathAshBase + FVector(0.f, 10.f * S, 60.f * S), 6, 220.f, AshSpeckColor);
		}
		float P = DeathAshStart > AshNotCrumbled ? T - DeathAshStart : -1.f;
		if (bDeathAshOnIsland && P >= AshCrumbleTime && P < AshGustAt && !bDeathAshDone)
		{
			// Waiting for the gust on the island's ground: the floor broken away under it (a crater, ice melted or blown
			// up), the gust comes at once - no pile hanging in the air.
			DeathAshFloorTimer -= Dt;
			if (DeathAshFloorTimer <= 0.f)
			{
				DeathAshFloorTimer = AshFloorCheckInterval;
				if (!HasAshFloor())
				{
					DeathAshStart -= AshGustAt - P;
					P = AshGustAt;
				}
			}
		}
		if (bShown)
		{
			// Stiff, facing the camera, puffed up by the blast within the char-up (wider, a little squatter), then a faint
			// quiver; crumbling, it gives way slowly, then all at once - squashing down into the heap, spreading a moment and
			// shaking, down to a speck hidden inside it. Its feet stay on the floor.
			const float Puff = CharPuff * (1.f - FMath::Cube(1.f - Char));
			const float Quiver = T > CharTime ? CharQuiver * FMath::Sin(T * 47.f) : 0.f;
			float Wide = 1.f + Puff + Quiver;
			float Tall = 1.f - 0.3f * Puff - 0.5f * Quiver;
			FVector Shake = FVector::ZeroVector;
			if (P >= 0.f)
			{
				const float Crumble = Phase01(P, 0.f, AshCrumbleTime);
				const float Fall = Crumble * Crumble;
				Tall = FMath::Lerp(Tall, 0.03f, Fall);
				Wide = FMath::Lerp(Wide, 0.45f, Fall) + 0.3f * FMath::Sin(Crumble * UE_PI);
				Shake.X = 1.5f * FMath::Sin(P * 90.f) * (1.f - Crumble);
			}
			const float TurnYaw = FMath::Lerp(MeshYaw, PancakeYaw, FMath::Clamp(T / SootTurnTime, 0.f, 1.f));
			SetDeathMeshTransform(FVector(Wide, Wide, Tall), 0.f, GetDeathFloorCentre(Tall) + Shake, TurnYaw);
			UpdateDeathFaceEyes(); // where its own eyes are now: the white eyes lie over them
			const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : CapsuleHalfHeight * S;
			if (P < 0.5f * AshCrumbleTime)
			{
				DeathFxTimer -= Dt;
				if (DeathFxTimer <= 0.f)
				{
					// Smoke curling up off its head (fewer puffs on lower effects settings; Trail thins them out again by itself).
					DeathFxTimer = SmokeInterval / FMath::Max(Density, 0.5f);
					APinguFX::Trail(this, GetActorLocation() + FVector(FMath::FRandRange(-12.f, 12.f) * S, 0.f, HalfHeight * (2.f * Tall - 1.f) * 0.9f),
						24.f * S, SmokeColor, FVector(FMath::FRandRange(-15.f, 15.f), 0.f, 85.f));
				}
				DeathEmberTimer -= Dt;
				APinguFX* FX = DeathEmberTimer <= 0.f && T > 0.06f ? APinguFX::Get(this) : nullptr;
				if (FX)
				{
					// Now and then an ember glowing on it: a tiny orange spark that drifts up and goes out.
					DeathEmberTimer = CharEmberInterval / Density;
					FPinguParticleDesc Ember;
					Ember.Mesh = EPinguFXMesh::Puff;
					Ember.Location = GetActorLocation() + FVector(FMath::FRandRange(-24.f, 24.f) * S, 24.f * S, FMath::FRandRange(-0.75f, 0.6f) * HalfHeight * Tall);
					Ember.Velocity = FVector(FMath::FRandRange(-15.f, 15.f), 0.f, FMath::FRandRange(25.f, 70.f));
					Ember.Color = EmberColor;
					Ember.Emissive = 6.f;
					Ember.Size = FMath::FRandRange(2.5f, 4.f) * S;
					Ember.EndSize = 0.f;
					Ember.Curve = EPinguSizeCurve::Linear;
					Ember.Life = FMath::FRandRange(0.3f, 0.55f);
					Ember.bFadeOut = true;
					FX->Spawn(Ember);
				}
			}
			if (!bDeathCoughDone && T >= CoughTime && P < 0.f)
			{
				// A cough: a puff of smoke out of the beak (under the eyes), toward the camera.
				bDeathCoughDone = true;
				const FVector Beak = bDeathFaceEyesKnown ? (DeathFaceEyes[0] + DeathFaceEyes[1]) * 0.5f + FVector(0.f, 14.f * S, -15.f * S)
					: GetActorLocation() + FVector(0.f, 30.f * S, 15.f * S);
				APinguFX::Puffs(this, Beak, 4, 13.f * S, 170.f, FVector(0.f, 1.f, 0.6f), 0.5f, SmokeColor);
				APinguFX::Sound(this, EPinguSound::Squawk, 0.45f, 0.05f, 0.6f);
			}
		}
		if (!bDeathAshDone && (P >= 0.f || (bShown && bDeathFaceEyesKnown)))
		{
			FPinguAshPile Pile;
			if (P >= 0.f)
			{
				Pile.Base = DeathAshBase;
				if (DeathEyesLandP < 0.f)
				{
					// The eyes land once the sinking face has come down to the pile's top (or the penguin is gone, or the
					// crumble is over).
					FPinguAshPile OnTop;
					OnTop.Base = DeathAshBase;
					OnTop.Size = S;
					const float PileEyeZ = static_cast<float>(APinguFX::GetAshPileEyes(OnTop).Z);
					const float FaceEyeZ = bDeathFaceEyesKnown ? static_cast<float>(DeathFaceEyes[0].Z + DeathFaceEyes[1].Z) * 0.5f : PileEyeZ;
					if (FaceEyeZ <= PileEyeZ || !bShown || P >= AshCrumbleTime)
					{
						DeathEyesLandP = P;
					}
				}
				AshPileAt(P, DeathEyesLandP, DeathAshWind, S, Pile);
				// The embers on the fresh pile die down, flickering.
				Pile.Embers = (1.f - Phase01(P - AshCrumbleTime, 0.f, AshEmberTime)) * (0.8f + 0.2f * FMath::Sin(P * 23.f));
			}
			else
			{
				// Charred and standing: only its eyes show, on its face (one blink while it stands there).
				Pile.Base = GetFeetLocation();
				Pile.Size = S;
				Pile.Grown = 0.f;
				Pile.Eyes = 1.f;
				Pile.EyeOnPile = 0.f;
				Pile.EyeOpen = AshBlinkOpen(T, CharBlinkAt);
			}
			if (bDeathFaceEyesKnown)
			{
				// (After the penguin has hidden, where they were last.)
				Pile.FaceEyes[0] = DeathFaceEyes[0];
				Pile.FaceEyes[1] = DeathFaceEyes[1];
				Pile.FaceEyeRadius = DeathFaceEyeRadius;
			}
			DeathAshBlown = Pile.Blown;
			const FVector PileBase = DeathAshBase;
			if (P >= 0.f && !bDeathAshSettled && P >= AshCrumbleTime)
			{
				// Down: the heap settles with a puff of ash out to both sides along the floor.
				bDeathAshSettled = true;
				DeathFxTimer = 0.f;
				for (const float Outward : { -1.f, 1.f })
				{
					APinguFX::Puffs(this, PileBase + FVector(Outward * 35.f * S, 8.f * S, 6.f * S), 3, 18.f * S, 220.f, FVector(Outward, 0.f, 0.15f), 0.3f, AshPuffColor);
				}
			}
			if (bDeathAshSettled && P < AshGustAt)
			{
				DeathFxTimer -= Dt;
				if (DeathFxTimer <= 0.f)
				{
					// A thin wisp of smoke off its top now and then.
					DeathFxTimer = AshWispInterval / Density;
					APinguFX::Trail(this, PileBase + FVector(FMath::FRandRange(-10.f, 10.f) * S, -4.f * S, 50.f * S), 14.f * S, SmokeColor, FVector(0.f, 0.f, 70.f));
				}
			}
			if (!bDeathGustDone && P >= AshGustAt)
			{
				// The gust: a whoosh and a few wind lines streaking over the pile (one on low effects settings).
				bDeathGustDone = true;
				DeathFxTimer = 0.f;
				APinguFX::Sound(this, EPinguSound::Gust, 0.6f, 0.05f, 1.15f);
				const int32 Streaks = Density >= 0.5f ? 3 : 1;
				for (int32 Index = 0; Index < Streaks; ++Index)
				{
					const float Z = static_cast<float>(PileBase.Z) + (14.f + 18.f * Index) * S;
					const FVector From(PileBase.X - DeathAshWind * (170.f + 25.f * Index) * S, 38.f * S, Z);
					const FVector To(PileBase.X + DeathAshWind * (40.f + 20.f * Index) * S, 38.f * S, Z + 6.f * S);
					APinguFX::GlowTrail(this, From, To, 5.f * S, FLinearColor(0.95f, 0.97f, 1.f), 0.35f, 0.22f);
				}
			}
			if (bDeathGustDone && Pile.Blown < 1.f)
			{
				DeathFxTimer -= Dt;
				if (APinguFX* FX = DeathFxTimer <= 0.f ? APinguFX::Get(this) : nullptr)
				{
					// Ash streaming off what is left of the pile, downwind and up (fewer bits on lower effects settings).
					DeathFxTimer = AshStreamInterval / Density;
					const float Remaining = 1.f - Pile.Blown;
					const FVector From(PileBase.X + (FMath::FRandRange(-40.f, 40.f) * (0.4f + 0.6f * Remaining) + DeathAshWind * Pile.Blown * 12.f) * S,
						FMath::FRandRange(-10.f, 20.f) * S, PileBase.Z + FMath::FRandRange(4.f, 6.f + 40.f * Remaining) * S);
					SpawnAshBit(FX, From, DeathAshWind, S, (++DeathAshBits % 3) == 0);
				}
			}
			const float EyesGone = AshEyesOffAt + AshEyesOffTime;
			if (!bDeathSpiritDone && P >= AshEyesOffAt + AshEyesOffTime * AshSpiritShare)
			{
				// The eyes whisked off: a last little puff of ash, and the sooty ghost rises from the floor there.
				bDeathSpiritDone = true;
				const FVector Eyes = APinguFX::GetAshPileEyes(Pile);
				APinguFX::Puffs(this, Eyes, 3, 12.f * S, 120.f, FVector::UpVector, 1.f, AshPuffColor);
				const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : CapsuleHalfHeight;
				const FVector GhostMiddle(Eyes.X, 0.f, PileBase.Z + HalfHeight * static_cast<float>(GetActorScale3D().Z) * SpiritSize);
				PlayDeathSpirit(GhostMiddle, SpiritRise, GhostColor, SpiritSize, SpiritLife);
			}
			if (P >= EyesGone)
			{
				bDeathAshDone = true;
				bDeathAshShown = false;
				APinguFX::HideAshPile(this);
			}
			else
			{
				bDeathAshShown = true;
				APinguFX::SetAshPile(this, Pile);
			}
		}
		break;
	}
	case EPenguinKOReason::Sea:
		// Gone under at once (the server hid it in KnockOut): a copy of it sinks rocking below the surface, bubbles come
		// up, then the ghost rises out of the water.
		if (!bDeathSinkDone)
		{
			bDeathSinkDone = true;
			PlayDeathSpirit(DeathLastLocation, SinkSpeed, SinkColor, 1.f, SinkLife);
		}
		if (T < BubbleTime)
		{
			DeathFxTimer -= Dt;
			if (DeathFxTimer <= 0.f)
			{
				// One bubble at a time (PlayPuffs keeps a single one only now and then on lower effects settings).
				DeathFxTimer = BubbleInterval;
				const FVector Bubble(DeathLastLocation.X + FMath::FRandRange(-35.f, 35.f) * SizeScale, 0.f, DeathSeaZ + 15.f);
				APinguFX::Puffs(this, Bubble, 1, FMath::FRandRange(9.f, 14.f) * SizeScale, 80.f, FVector::UpVector, 0.3f, BubbleColor);
			}
		}
		if (!bDeathSpiritDone && T >= SeaSpiritTime)
		{
			bDeathSpiritDone = true;
			const FVector OutOfWater(DeathLastLocation.X, 0.f, DeathSeaZ + 40.f * SizeScale);
			PlayDeathSpirit(OutOfWater, SpiritRise, GhostColor, SpiritSize, SpiritLife);
		}
		break;
	case EPenguinKOReason::Shrunk:
		// It just vanishes: no ghost.
		bDeathSpiritDone = true;
		if (bShown)
		{
			// Shrinking to nothing on the spot, faster and faster, twirling round and wobbling, its feet on the floor. The
			// tiny pop as it vanishes is the server's (FinishKnockOut), for everybody.
			const float Gone = Phase01(T, 0.f, ShrunkVanishTime);
			const float Wobble = ShrunkWobble * (1.f - Gone) * FMath::Sin(T * ShrunkWobbleRate);
			const float Left = FMath::Max(1.f - Gone * Gone, ShrunkMinScale);
			const float Wide = FMath::Max(Left * (1.f + Wobble), ShrunkMinScale);
			const float Tall = FMath::Max(Left * (1.f - Wobble), ShrunkMinScale);
			DeathSpin += Dt * ShrunkSpinRate * DeathSide;
			SetDeathMeshTransform(FVector(Wide, Wide, Tall), 0.f, GetDeathFloorCentre(Tall), MeshYaw + DeathSpin);
		}
		break;
	case EPenguinKOReason::Overgrown:
		// A pale green ghost (normal size: below) once it has popped.
		GhostColor = SpiritColor * 0.7f + GrowColor * 0.3f;
		if (bShown)
		{
			// Too big: it puffs up more, wobbling harder and faster and trembling, steam puffing off its head, facing the
			// camera, its feet on the floor - until it pops (the blast is the server's: OvergrownBlast, for everybody).
			const float Swell = Phase01(T, 0.f, OvergrownPopTime);
			const float Wobble = FMath::Lerp(OvergrownWobbleFrom, OvergrownWobbleTo, Swell)
				* FMath::Sin(T * FMath::Lerp(OvergrownWobbleRateFrom, OvergrownWobbleRateTo, Swell));
			const float Puff = 1.f + OvergrownSwell * Swell * Swell;
			const float Wide = Puff * (1.f + Wobble);
			const float Tall = Puff * (1.f - 0.6f * Wobble);
			const float Tremble = OvergrownTremble * Swell * FMath::Sin(T * 97.f);
			const float TurnYaw = FMath::Lerp(MeshYaw, PancakeYaw, FMath::Clamp(T / SootTurnTime, 0.f, 1.f));
			SetDeathMeshTransform(FVector(Wide, Wide, Tall), 0.f, GetDeathFloorCentre(Tall) + FVector(Tremble, 0.f, 0.f), TurnYaw);
			DeathFxTimer -= Dt;
			if (DeathFxTimer <= 0.f)
			{
				// Steam off the top of its head (fewer puffs on lower effects settings).
				DeathFxTimer = OvergrownSteamInterval / FMath::Max(UPinguSettings::GetFXDensity(), 0.5f);
				const float HalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : CapsuleHalfHeight * SizeScale;
				const FVector Head = GetActorLocation() + FVector(FMath::FRandRange(-0.3f, 0.3f) * HalfHeight, 0.f, HalfHeight * (2.f * Tall - 1.f) * 0.95f);
				APinguFX::Puffs(this, Head, 2, 0.15f * HalfHeight, 260.f, FVector::UpVector, 0.5f, FLinearColor(0.95f, 0.97f, 1.f));
			}
		}
		break;
	default:
		break; // the tumble: the KO clip flying off, as before
	}

	// Vanished: the little ghost floats up from where it was last seen (a snow bomb's ash pile has its own, once the wind
	// has taken it all).
	if (!bShown && !bDeathSpiritDone && DeathLook != EPenguinKOReason::Sea && !(DeathLook == EPenguinKOReason::Bomb && DeathAshStart > AshNotCrumbled))
	{
		bDeathSpiritDone = true;
		if (DeathLook == EPenguinKOReason::Bomb)
		{
			// Never on the floor before it vanished (blown up over the sea, say): the poof the other knock-outs get from the
			// server as they vanish (FinishKnockOut leaves a snow bomb's out), here - its white eyes go with it.
			APinguFX::Puffs(this, DeathLastLocation, 12, 36.f, 300.f, FVector::UpVector, 1.f);
			APinguFX::Sparkles(this, DeathLastLocation, 8, 60.f, FLinearColor(1.f, 0.9f, 0.5f));
			if (bDeathAshShown)
			{
				bDeathAshShown = false;
				APinguFX::HideAshPile(this);
			}
		}
		// (Out of a popped giant comes a ghost of a normal penguin's size, rising at a normal ghost's speed.)
		const float Giant = DeathLook == EPenguinKOReason::Overgrown ? FMath::Max(static_cast<float>(GetActorScale3D().Z), 1.f) : 1.f;
		PlayDeathSpirit(DeathLastLocation, SpiritRise / Giant, GhostColor, SpiritSize / Giant, SpiritLife);
	}

	// Gone and the ghost has its copy of the pose: nobody sees the mesh, so stop evaluating its animation and bones until
	// the engine renders it again or EndDeathLook puts the every-frame update back.
	if (!bShown && bDeathSpiritDone)
	{
		USkeletalMeshComponent* GoneMesh = GetMesh();
		if (GoneMesh && GoneMesh->VisibilityBasedAnimTickOption != EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered)
		{
			GoneMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
		}
	}
}

// ---------------------------------------------------------------------------- belly slide hits (server)

bool APenguinCharacter::IsTouchingForSlide(const APenguinCharacter* Other, float ShiftBack, float SweepBack) const
{
	using namespace PenguinCharacterPrivate;
	const UCapsuleComponent* Theirs = Other ? Other->GetCapsuleComponent() : nullptr;
	if (!Theirs || !GetCapsuleComponent())
	{
		return false;
	}
	// Us: lying along the floor, the way we slide. Them: their standing capsule (on the play plane).
	const float SlideWay = PenguinMovement ? PenguinMovement->GetBellySlideDirection() : GetFacing();
	const float LyingRadius = SlideLyingRadius * SizeScale;
	const FVector Feet = GetFeetLocation();
	const FVector Middle(GetActorLocation().X + SlideWay * (SlideLyingAhead * SizeScale - FMath::Max(ShiftBack, 0.f)), 0.f, Feet.Z + LyingRadius);
	const FVector Along(SlideWay * SlideLyingHalfLength * SizeScale, 0.f, 0.f);
	const FVector Swept(SlideWay * FMath::Max(SweepBack, 0.f), 0.f, 0.f);
	const float TheirRadius = Theirs->GetScaledCapsuleRadius();
	const float TheirHalf = FMath::Max(Theirs->GetScaledCapsuleHalfHeight() - TheirRadius, 0.f);
	const FVector There = Other->GetActorLocation();
	FVector OnMine;
	FVector OnTheirs;
	FMath::SegmentDistToSegmentSafe(Middle - Along - Swept, Middle + Along, FVector(There.X, 0.f, There.Z - TheirHalf),
		FVector(There.X, 0.f, There.Z + TheirHalf), OnMine, OnTheirs);
	return FVector::Dist(OnMine, OnTheirs) <= LyingRadius + TheirRadius + SlideContactMargin * SizeScale;
}

void APenguinCharacter::UpdateBellySlideContacts()
{
	using namespace PenguinCharacterPrivate;
	UPenguinMovementComponent* Move = PenguinMovement;
	UWorld* World = GetWorld();
	// The slide state is right here on every penguin: our own simulation, or - a LAN player's - the slides the host counted of
	// what its owner sends (its own cooldown, its own measure of the way: UPenguinMovementComponent::ApplyOwnerRequests).
	if (!HasAuthority() || !Move || !World || bKnockedOut || IsHidden() || !Move->IsBellySliding())
	{
		return;
	}
	const float Travel = Move->GetBellySlideDistance();
	if (Move->SlideEvents != SlideContactSerial)
	{
		// A new slide: one contact again. Whoever we stood inside where it began (penguins pass through each other) is let
		// through until we have come apart - starting a slide next to somebody never stops it at once. (A LAN client's slide
		// may only show up here some way in: it began Travel back, and the first check sweeps all the way from there.)
		SlideContactSerial = Move->SlideEvents;
		bSlideContactDone = false;
		SlideContactTravel = 0.f;
		SlideStartOverlaps.Reset();
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			APenguinCharacter* Other = *It;
			if (Other != this && Other->IsAlive() && !Other->IsHidden() && IsTouchingForSlide(Other, Travel))
			{
				SlideStartOverlaps.Add(Other);
			}
		}
	}
	const float SlideWay = Move->GetBellySlideDirection();
	const FVector Here = GetActorLocation();
	// The way slid since the last check: whoever is anywhere along it was run into.
	const float Sweep = FMath::Clamp(Travel - SlideContactTravel, 0.f, Travel);
	SlideContactTravel = Travel;

	// (A snow bomb in the way is kicked along by the bomb itself: APenguinProjectile::KickBySlide.)

	if (bSlideContactDone)
	{
		return;
	}
	APenguinCharacter* Victim = nullptr;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Other = *It;
		// (A teammate too: friendly fire.)
		if (Other == this || !Other->IsAlive() || Other->IsHidden())
		{
			continue;
		}
		const int32 StartIndex = SlideStartOverlaps.IndexOfByPredicate([Other](const TWeakObjectPtr<APenguinCharacter>& Ptr) { return Ptr.Get() == Other; });
		if (StartIndex != INDEX_NONE)
		{
			if (!IsTouchingForSlide(Other))
			{
				SlideStartOverlaps.RemoveAtSwap(StartIndex); // apart now: from here on it counts
			}
			continue;
		}
		// Only ahead of where we were at the last check: a penguin the slide had passed already stays behind.
		if (IsTouchingForSlide(Other, 0.f, Sweep) && (Other->GetActorLocation().X - (Here.X - SlideWay * Sweep)) * SlideWay >= 0.f)
		{
			Victim = Other;
			break;
		}
	}
	if (!Victim)
	{
		return;
	}

	// One contact per slide. Far enough along, it knocks the penguin away (along the slide, up a little, harder the
	// faster it goes; ApplyPenguinHit adds the sizes (ours to the victim's) and the fight's knockback growth, an ice shell blocks it).
	// Before that it is only a bump. Either way the slide stops here and on its owner's machine.
	bSlideContactDone = true;
	const FVector Contact = (Here + Victim->GetActorLocation()) * 0.5f;
	bool bHit = false;
	FPinguFXBroadcastScope Broadcast(this);
	if (Move->GetBellySlideDistance() >= Move->BellySlideHitDistance)
	{
		// (A LAN player's slide: at most as fast as the host measured it - GetSlideHitSpeed.)
		const float Speed = Move->GetSlideHitSpeed();
		const float SpeedFactor = FMath::Clamp(Speed / FMath::Max(Move->BellySlideHitSpeed, 1.f), SlideHitMinScale, SlideHitMaxScale);
		FPenguinHit Hit;
		Hit.Instigator = this;
		Hit.Knockback = FVector2D(SlideWay * Move->BellySlideKnockback.X, Move->BellySlideKnockback.Y) * SpeedFactor;
		Hit.Stun = SlideHitStun;
		Hit.Location = Contact;
		Hit.bBellySlide = true;
		bHit = Victim->ApplyPenguinHit(Hit);
		if (bHit)
		{
			APinguFX::Sound(this, EPinguSound::Slap, 0.8f);
			APinguFX::Puffs(this, Contact, 6, 22.f * SizeScale, 320.f, FVector(SlideWay, 0.f, 0.5f).GetSafeNormal(), 0.7f);
		}
	}
	if (!bHit)
	{
		// A bump (or the shell held): a soft thud of snow (the get-up skid plays on every machine by itself).
		APinguFX::Sound(this, EPinguSound::Land, 0.6f);
		APinguFX::Puffs(this, Contact, 3, 16.f * SizeScale, 140.f, FVector::UpVector, 1.f);
	}
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s's belly slide ran into %s after %.0f cm: %s"), *DisplayName, *Victim->GetDisplayName(),
		Move->GetBellySlideDistance(), bHit ? TEXT("knocked away") : TEXT("bump"));
	Move->EndBellySlideOnContact(bHit);
}

// ---------------------------------------------------------------------------- tick

void APenguinCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	// Whatever turned the actor, turn it back before the mesh yaw is applied (the facing is only the mesh yaw).
	if (!GetActorQuat().IsIdentity(1.e-4f))
	{
		KeepActorUnrotated();
	}
	const bool bSimulated = IsLocallySimulated();
	if (!bSeenCountersSynced)
	{
		bSeenCountersSynced = true;
		if (!bSimulated)
		{
			SyncSeenCounters(); // replicated counters can come in after BeginPlay: no juice for events from before we saw the penguin
		}
		if (bKnockedOut && IsHidden())
		{
			// Out and gone before this machine saw it (it joined late): no death look, no ghost for that old knock-out.
			StartDeathLook();
			DeathLookTime = PenguinCharacterPrivate::DeathLookLate;
			bDeathSpiritDone = true;
			bDeathSinkDone = true;
			bDeathCoughDone = true;
		}
	}
	UpdateAbilityKeys(); // a size ray key held long enough: the ray on ourselves
	UpdateNetSync(Dt);
	UpdateJuice(Dt);
	UpdateFlapWind(Dt);
	UpdateNootHorn(Dt); // after the mesh has turned (UpdateJuice)
	UpdateStatusVisuals(Dt);
	UpdateDizzyStars(Dt);
	UpdateSize(Dt);
	UpdateDeathLook(Dt); // after the squash & stretch, the status visuals and the stars: it overrides them while knocked out
	if (!bSimulated && PenguinMovement)
	{
		RemoteAirTime = PenguinMovement->IsMovingOnGround() ? 0.f : RemoteAirTime + Dt;
	}

	if (bKnockedOut)
	{
		if (HasAuthority())
		{
			KOTime += Dt;
			if (!bKOHidden && KOTime > PenguinCharacterPrivate::KOHideTime(KOReason))
			{
				if (KOReason == EPenguinKOReason::Overgrown)
				{
					OvergrownBlast(); // it pops
				}
				FinishKnockOut();
			}
		}
		return;
	}

	// Into the sea: a match event, the server decides on its own copy's position (a LAN player's only where the host took its
	// moves: one that hangs in the air or ignores a knockback is moved by the host itself - PinguHostAuthority.h).
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), GetActorLocation());
	const float SeaLevel = Island ? Island->SeaLevel : -1650.f;
	if (HasAuthority() && !bAwaitingResetAck && GetActorLocation().Z < SeaLevel + 120.f)
	{
		if (bVictory)
		{
			// The winner slipped off during the celebration: back onto the island instead of into the sea.
			Respawn();
			SetVictory(true);
			SetInputLocked(true);
			return;
		}
		// A trait may save it once (Second Chance: a fish throws it back up).
		if (TraitComponent && TraitComponent->TrySecondChance())
		{
			return;
		}
		KnockOut(EPenguinKOReason::Sea, nullptr);
		return;
	}

	// Yanked by the fish hook in our ice cube: it shatters where it hits something (server).
	if (HasAuthority())
	{
		UpdateIceThrow(Dt);
	}

	// Nothing below us? Flail (the animation reads this).
	GroundCheckTimer -= Dt;
	if (GroundCheckTimer <= 0.f)
	{
		GroundCheckTimer = 0.1f;
		float GroundZ = 0.f;
		bFallingToSea = PenguinMovement && PenguinMovement->IsFalling() && Island && !Island->FindGroundBelow(GetFeetLocation(), 3000.f, GroundZ);
	}

	// Belly stomp impact (the slam is server gameplay; the counter is right on every machine).
	if (PenguinMovement && PenguinMovement->StompLandEvents != SeenStompLand)
	{
		SeenStompLand = PenguinMovement->StompLandEvents;
		if (AbilityComponent && HasAuthority())
		{
			AbilityComponent->OnStompLanded();
		}
	}

	// A belly slide running into somebody (server gameplay, on the slide state the owner sends).
	UpdateBellySlideContacts();

	// Ledge grab: flippers up and over, a puff of snow off the edge (every machine plays its own, from the counter).
	if (PenguinMovement && PenguinMovement->ClimbEvents != SeenClimb)
	{
		SeenClimb = PenguinMovement->ClimbEvents;
		PlayLocalAction(EPenguinClip::Climb, 1.f);
		const FVector Edge = GetActorLocation() + FVector(GetFacing() * 45.f * SizeScale, 0.f, 50.f * SizeScale);
		APinguFX::Puffs(this, Edge, 4, 18.f * SizeScale, 160.f, FVector::UpVector, 0.8f);
		APinguFX::Sound(this, EPinguSound::Jump, 0.6f, 0.06f, 0.85f);
	}
}

void APenguinCharacter::UpdateStatusVisuals(float Dt)
{
	StatusTime += Dt;
	const bool bAuthority = HasAuthority();
	if (!bAuthority && PenguinMovement)
	{
		// LAN client: show what the movement state says (the server plays the freeze / thaw / slip effects for everybody).
		const bool bFrozenNow = PenguinMovement->IsFrozen() && !bKnockedOut;
		if (bFrozenNow != bIceShown)
		{
			ShowIceBlock(bFrozenNow);
		}
		const bool bSlipperyNow = PenguinMovement->IsSlippery() && !bKnockedOut;
		if (bSlipperyNow != bIceFeetShown)
		{
			ShowIceFeet(bSlipperyNow);
		}
	}
	if (bAuthority && PenguinMovement && bIceShown && !PenguinMovement->IsFrozen() && !bKnockedOut)
	{
		// Thawed.
		ShowIceBlock(false);
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Shards(this, GetActorLocation(), 10, 600.f, FLinearColor(0.7f, 0.92f, 1.f));
		APinguFX::Sound(this, EPinguSound::Shatter, 0.8f);
	}
	if (bShieldShown && ShieldBubble)
	{
		const float Wobble = 1.f + 0.04f * FMath::Sin(StatusTime * 18.f);
		const float Base = UPenguinAbilityComponent::ShieldRadius * 2.f / 100.f;
		ShieldBubble->SetRelativeScale3D(FVector(Base * Wobble, Base * Wobble, Base / Wobble));
	}
	if (bIceShown && IceBlock)
	{
		const float Shiver = PenguinMovement && PenguinMovement->GetFrozenRemaining() < 0.4f ? FMath::Sin(StatusTime * 70.f) * 2.f : 0.f;
		IceBlock->SetRelativeLocation(FVector(Shiver, 0.f, 4.f));
	}
	if (bIceFeetShown)
	{
		if (bAuthority && (!PenguinMovement || !PenguinMovement->IsSlippery() || bKnockedOut))
		{
			ShowIceFeet(false);
			FPinguFXBroadcastScope Broadcast(this);
			APinguFX::Shards(this, GetFeetLocation(), 4, 300.f, FLinearColor(0.7f, 0.92f, 1.f));
		}
		else
		{
			// The chunks grow and shrink with the penguin (FitIceFeet sized them in the mesh's own units).
			const float MeshWorldScale = GetMesh() ? static_cast<float>(GetMesh()->GetComponentScale().X) : PenguinCharacterPrivate::MeshScale * SizeScale;
			IceFootL->SetWorldScale3D(IceFootSize[0] * (MeshWorldScale / PenguinCharacterPrivate::IceChunkMeshSize));
			IceFootR->SetWorldScale3D(IceFootSize[1] * (MeshWorldScale / PenguinCharacterPrivate::IceChunkMeshSize));
			// A few ice glints at the feet while they slip (every machine its own).
			IceSparkleTimer -= Dt;
			if (IceSparkleTimer <= 0.f)
			{
				IceSparkleTimer = 0.16f;
				APinguFX::Sparkles(this, GetFeetLocation() + FVector(FMath::FRandRange(-18.f, 18.f) * SizeScale, 0.f, 6.f), 1, 10.f * SizeScale, FLinearColor(0.6f, 0.92f, 1.f));
			}
		}
	}
}

void APenguinCharacter::UpdateFlapWind(float Dt)
{
	if (!PenguinMovement || !PenguinMovement->IsFlapping() || IsHidden() || bKnockedOut)
	{
		FlapWindClock = 0.f;
		FlapWindBeat = 0;
		return;
	}
	// The anim's beat is 7 a second and its down-stroke starts each beat (PenguinAnimInstance ApplyFlap): the wind starts there too.
	constexpr float BeatsPerSecond = 7.f;
	const int32 Beat = FMath::FloorToInt(FlapWindClock * BeatsPerSecond) + 1;
	FlapWindClock += Dt;
	if (Beat <= FlapWindBeat)
	{
		return;
	}
	FlapWindBeat = Beat;
	const FVector Feet = GetFeetLocation();
	float GroundDistance = -1.f;
	FVector GroundPoint = Feet;
	if (const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Feet))
	{
		float GroundZ = 0.f;
		if (Island->FindGroundBelow(Feet + FVector(0.f, 0.f, 20.f), 400.f, GroundZ))
		{
			GroundDistance = FMath::Max(0.f, static_cast<float>(Feet.Z) - GroundZ);
			GroundPoint = FVector(Feet.X, 0.f, GroundZ);
		}
	}
	APinguFX::FlapWind(this, GetActorLocation() + FVector(0.f, 0.f, 10.f * SizeScale), SizeScale, GroundDistance, GroundPoint, Beat);
}

void APenguinCharacter::UpdateJuice(float Dt)
{
	if (!PenguinMovement || Dt <= 0.f)
	{
		return;
	}
	USkeletalMeshComponent* SkelMesh = GetMesh();
	const UPenguinMovementComponent* Move = PenguinMovement;

	// Impulses from movement events.
	if (Move->JumpEvents != SeenJump)
	{
		SquashVelocity += JumpStretch;
	}
	if (Move->AirJumpEvents != SeenAirJump)
	{
		SquashVelocity += JumpStretch * 0.7f;
	}
	if (Move->LandEvents != SeenLand)
	{
		SquashVelocity -= LandSquash * FMath::GetMappedRangeValueClamped(FVector2f(300.f, 2600.f), FVector2f(0.25f, 1.f), Move->LastLandSpeed);
	}
	if (Move->DashEvents != SeenDash)
	{
		StretchVelocity += DashStretch;
	}
	if (Move->PadBounceEvents != SeenPadBounce)
	{
		SquashVelocity += JumpStretch * 1.6f; // boing: a big stretch up
	}
	if (Move->SlideEvents != SeenSlide)
	{
		StretchVelocity += DashStretch; // thrown forward onto the belly
	}
	if (bMovementEffects)
	{
		UpdateMovementEffects(Dt);
	}
	SeenJump = Move->JumpEvents;
	SeenAirJump = Move->AirJumpEvents;
	SeenLand = Move->LandEvents;
	SeenDash = Move->DashEvents;
	SeenPadBounce = Move->PadBounceEvents;
	SeenSlide = Move->SlideEvents;

	// Damped springs back to neutral.
	SquashVelocity += (-SquashStiffness * Squash - SquashDamping * SquashVelocity) * Dt;
	Squash += SquashVelocity * Dt;
	StretchVelocity += (-SquashStiffness * Stretch - SquashDamping * StretchVelocity) * Dt;
	Stretch += StretchVelocity * Dt;
	// Settled springs rest at exactly neutral: the mesh scale then stops changing (no transform update every frame).
	if (FMath::Abs(Squash) < 1.e-4f && FMath::Abs(SquashVelocity) < 1.e-3f)
	{
		Squash = SquashVelocity = 0.f;
	}
	if (FMath::Abs(Stretch) < 1.e-4f && FMath::Abs(StretchVelocity) < 1.e-3f)
	{
		Stretch = StretchVelocity = 0.f;
	}
	const float S = FMath::Clamp(Squash, -0.4f, 0.4f);
	const float D = FMath::Clamp(Stretch, -0.3f, 0.45f);

	// Turn toward the facing direction, peeking at the camera (+Y) while facing left/right.
	const float TargetYaw = GetVisualFacing() > 0.f ? (-90.f + CameraPeekYaw) : (90.f - CameraPeekYaw);
	MeshYaw = FMath::FInterpTo(MeshYaw, TargetYaw, Dt, TurnSpeed);
	if (bDeathLookActive && PenguinCharacterPrivate::DeathLookShapesMesh(DeathLook))
	{
		return; // knocked out: the death look shapes the mesh (UpdateDeathLook)
	}

	// Mesh local axes: X = side, Y = forward (the penguin faces +Y in the mesh), Z = up.
	const FVector Scale(1.f - 0.5f * S - 0.15f * D, 1.f - 0.5f * S + D, 1.f + S - 0.35f * D);
	SkelMesh->SetRelativeScale3D(GetBaseMeshScale() * Scale);

	// The slide clips lay the penguin on its belly. Without them (an import that lacks them) it is tipped over by hand,
	// so it never slides along upright: rolled about its side axis (its up turns to where it faces, the belly to the
	// floor), lifted out of the floor and pulled back over its capsule.
	using namespace PenguinCharacterPrivate;
	const bool bTipOver = Move->IsBellySliding() && !GetAnimClip(EPenguinClip::Slide);
	BellyTilt = FMath::FInterpConstantTo(BellyTilt, bTipOver ? 1.f : 0.f, Dt, BellyTiltSpeed);
	FVector MeshOffset = GetBaseTranslationOffset();
	FRotator MeshRotation(0.f, MeshYaw, 0.f);
	if (BellyTilt > 0.f)
	{
		MeshRotation.Roll = 90.f * BellyTilt;
		MeshOffset += FVector(-Move->GetFacing() * BellyTiltBack, 0.f, BellyTiltLift) * BellyTilt;
	}
	SkelMesh->SetRelativeLocationAndRotation(MeshOffset, MeshRotation);
}

void APenguinCharacter::UpdateMovementEffects(float Dt)
{
	// Movement effects are local on every machine: the movement event counters are right everywhere (no broadcast).
	const UPenguinMovementComponent* Move = PenguinMovement;
	if (bKnockedOut || IsHidden())
	{
		return;
	}
	const FVector Feet = GetFeetLocation();
	const float Speed = FMath::Abs(static_cast<float>(Move->Velocity.X));
	const bool bGrounded = Move->IsMovingOnGround();
	const FLinearColor Snow(0.96f, 0.98f, 1.f);

	if (Move->JumpEvents != SeenJump)
	{
		APinguFX::GroundPuffs(this, Feet, 0.55f);
		APinguFX::Sound(this, EPinguSound::Jump, 0.8f);
	}
	if (Move->AirJumpEvents != SeenAirJump)
	{
		APinguFX::Ring(this, Feet + FVector(0.f, 0.f, 10.f), 60.f, FLinearColor::White, 0.25f);
		APinguFX::Puffs(this, Feet, 5, 20.f, 260.f, FVector(0.f, 0.f, -1.f), 1.f);
		APinguFX::Sound(this, EPinguSound::DoubleJump, 0.8f);
	}
	if (Move->PadBounceEvents != SeenPadBounce)
	{
		// Off an icicle jump pad: an icy ring and sparkles at the feet, a high springy whoosh.
		const FLinearColor Ice(0.6f, 0.92f, 1.f);
		APinguFX::Ring(this, Feet + FVector(0.f, 0.f, 6.f), 75.f, Ice, 0.3f);
		APinguFX::Sparkles(this, Feet, 6, 40.f, Ice);
		APinguFX::Sound(this, EPinguSound::DoubleJump, 0.9f, 0.04f, 1.35f);
		APinguFX::Sound(this, EPinguSound::IcicleHit, 0.35f, 0.05f, 1.5f);
	}
	if (Move->LandEvents != SeenLand && !Move->IsStomping())
	{
		const float Strength = FMath::GetMappedRangeValueClamped(FVector2f(300.f, 2600.f), FVector2f(0.35f, 1.7f), Move->LastLandSpeed);
		APinguFX::GroundPuffs(this, Feet, Strength);
		if (Move->LastLandSpeed > 1700.f)
		{
			APinguFX::Sound(this, EPinguSound::LandHard, 0.8f);
			APinguFX::Shake(this, 0.12f);
		}
		else
		{
			APinguFX::Sound(this, EPinguSound::Land, 0.6f);
		}
	}
	if (Move->DashEvents != SeenDash)
	{
		APinguFX::Puffs(this, GetActorLocation(), 5, 24.f, 220.f, -Move->Velocity.GetSafeNormal(), 0.8f);
		APinguFX::Sound(this, EPinguSound::Dash, 0.8f);
		GhostTimer = 0.f;
	}

	// Belly slide: a whoosh onto the belly, snow spraying off it while it slides (ice glints: it goes much further there),
	// a skid when it gets up again.
	const float SlideWay = Move->GetBellySlideDirection();
	if (Move->SlideEvents != SeenSlide)
	{
		APinguFX::Puffs(this, Feet + FVector(0.f, 0.f, 10.f), 5, 22.f * SizeScale, 240.f, FVector(-SlideWay, 0.f, 0.5f).GetSafeNormal(), 0.8f);
		APinguFX::Sound(this, EPinguSound::SlideStart, 0.8f);
		SlideFxTimer = 0.f;
	}
	if (Move->IsBellySliding())
	{
		bSlideFxActive = true;
		SlideFxTimer -= Dt;
		if (bGrounded && SlideFxTimer <= 0.f)
		{
			using namespace PenguinCharacterPrivate;
			SlideFxTimer = SlideSprayInterval / FMath::Max(UPinguSettings::GetFXDensity(), 0.3f);
			APinguFX::Trail(this, Feet + FVector(0.f, 0.f, 8.f), 18.f * SizeScale, Snow, FVector(-Move->Velocity.X * 0.25f, 0.f, 70.f));
			if (Move->IsOnIce())
			{
				APinguFX::Sparkles(this, Feet + FVector(0.f, 0.f, 6.f), 1, 14.f * SizeScale, FLinearColor(0.6f, 0.92f, 1.f));
			}
		}
	}
	else if (bSlideFxActive)
	{
		bSlideFxActive = false;
		// Up on its feet (a jump out of the slide has its own puff and sound).
		if (bGrounded && Move->JumpEvents == SeenJump)
		{
			APinguFX::Sound(this, EPinguSound::Skid, 0.45f);
			APinguFX::GroundPuffs(this, Feet, 0.35f);
		}
	}

	// Dash afterimages.
	if (Move->IsDashing())
	{
		GhostTimer -= Dt;
		if (GhostTimer <= 0.f)
		{
			// Fewer afterimages on lower effects settings (each one is a whole see-through penguin to draw).
			GhostTimer = 0.032f / FMath::Max(UPinguSettings::GetFXDensity(), 0.3f);
			APinguFX::Ghost(this, GetMesh(), GetPlayerColor() * 0.5f + FLinearColor(0.4f, 0.55f, 0.75f), 0.2f, 0.45f);
			APinguFX::Trail(this, GetActorLocation(), 22.f, Snow);
		}
	}

	// Skid: turning around at speed kicks up snow (and squeaks on ice).
	const float InputX = Move->GetMoveInput().X;
	const bool bSkid = bGrounded && Speed > 280.f && InputX * Move->Velocity.X < -0.3f * Speed;
	if (bSkid)
	{
		if (!bSkidding)
		{
			APinguFX::Sound(this, Move->IsOnIce() ? EPinguSound::IceSlip : EPinguSound::Skid, 0.7f);
		}
		SkidTimer -= Dt;
		if (SkidTimer <= 0.f)
		{
			SkidTimer = 0.035f;
			APinguFX::Trail(this, Feet + FVector(0.f, 0.f, 6.f), 20.f, Snow, FVector(Move->Velocity.X * 0.4f, 0.f, 90.f));
		}
	}
	bSkidding = bSkid;

	// Little kicks of snow from running feet.
	StepTimer -= Dt;
	if (bGrounded && Speed > 420.f && StepTimer <= 0.f && !Move->IsOnIce())
	{
		StepTimer = 0.18f;
		APinguFX::Trail(this, Feet + FVector(0.f, 0.f, 4.f), 11.f, Snow, FVector(-Move->Velocity.X * 0.15f, 0.f, 50.f));
	}
}

// ---------------------------------------------------------------------------- animation inputs

void APenguinCharacter::GetAnimInputs(FPenguinAnimInputs& Out) const
{
	const UPenguinMovementComponent* Move = PenguinMovement;
	if (!Move)
	{
		return;
	}
	Out.bValid = true;
	Out.bGrounded = Move->IsMovingOnGround();
	Out.bDashing = Move->IsDashing();
	Out.bSliding = Move->IsBellySliding();
	Out.bOnIce = Move->IsOnIce();
	Out.SpeedX = FMath::Abs(Move->Velocity.X);
	Out.VelocityX = Move->Velocity.X;
	Out.VelocityZ = Move->Velocity.Z;
	Out.InputX = Move->HasNoControl() ? 0.f : Move->GetMoveInput().X;
	Out.Facing = Move->GetFacing();
	Out.RunSpeed = Move->GetEffectiveRunSpeed();
	Out.SizeScale = SizeScale;
	// The movement component times the air time while it simulates; for the others we watch the replicated state.
	Out.AirTime = IsLocallySimulated() ? Move->GetAirTime() : RemoteAirTime;
	Out.bFallingToSea = bFallingToSea;
	Out.JumpCount = Move->JumpEvents;
	Out.AirJumpCount = Move->AirJumpEvents;
	Out.DashCount = Move->DashEvents;
	Out.SlideCount = Move->SlideEvents;
	Out.LandCount = Move->LandEvents;
	Out.LastLandSpeed = Move->LastLandSpeed;
	Out.bLastJumpFromRun = Move->bLastJumpFromRun;
	Out.bLastSlideWasSlip = Move->bLastSlideWasSlip;

	Out.bStunned = Move->IsStunned();
	Out.bFrozen = Move->IsFrozen();
	Out.bKO = bKnockedOut;
	if (bDeathIceShown || (bDeathLookActive && DeathLook == EPenguinKOReason::Icicle
		&& DeathLookTime < PenguinCharacterPrivate::IceDeathShatterTime + PenguinCharacterPrivate::IceDeathThawTime))
	{
		// An icicle's knock-out: frozen stiff in its ice block, and a moment longer once it shattered (the burst reads
		// around the same tumbling pose), the KO clip from then on.
		Out.bKO = false;
		Out.bFrozen = true;
	}
	if (bDeathLookActive && DeathLook == EPenguinKOReason::Bomb)
	{
		// A snow bomb's knock-out: charred stiff on the spot (the frozen pose, standing - not the KO clip's flop onto the
		// back), until it has crumbled into its ash pile. No hit reaction on top (below).
		Out.bKO = false;
		Out.bFrozen = true;
	}
	Out.bShielding = IsShielded();
	Out.bStomping = Move->IsStomping();
	Out.bFlapping = Move->IsFlapping();
	Out.bVictory = bVictory;
	Out.bInputLocked = Move->IsInputLocked();
	Out.HitCount = Move->HitEvents;
	Out.LastHitSpeed = Move->LastHitSpeed;
	if (bDeathLookActive && DeathLook == EPenguinKOReason::Bomb)
	{
		// (The snow bomb's charred penguin stands stiff: no hit reaction on top - after the hit counter is read above.)
		Out.LastHitSpeed = 0.f;
		Out.HitCount = DeathHitCount;
	}

	Out.ActionCount = ActionCount;
	Out.ActionClip = ActionClip;
	Out.ActionRate = ActionRate;
	Out.bDizzy = bDizzy;
	Out.HonkWeight = GetHonkWeight();
	Out.HonkPitch = GetHonkPitch();
	Out.VisualFacing = GetVisualFacing();

	if (bDeathLookActive && (PenguinCharacterPrivate::IsPancakeLook(DeathLook) || PenguinCharacterPrivate::IsSizeDeath(DeathLook)))
	{
		// Squashed into a pancake facing the camera: not the KO clip (it spins and flops onto the back, the head toward
		// the camera, so the squash showed the beanie's top like a frisbee). The stunned flail instead (upright, flippers
		// flapping straight out to the sides, feet kicking: as an in-the-air stun with a soft hit, never the somersault),
		// the eyes rolling dizzy, and no hit reaction on top - as it shrinks to nothing or swells up to pop, too. Every
		// machine derives this from the replicated knock-out.
		Out.bKO = false;
		Out.bFrozen = false;
		Out.bStomping = false;
		Out.bFlapping = false;
		Out.bDashing = false;
		Out.bSliding = false;
		Out.bVictory = false;
		Out.bStunned = true;
		Out.bGrounded = false;
		Out.bDizzy = true;
		Out.LastHitSpeed = 0.f;
		Out.HitCount = DeathHitCount;
	}

	Out.bHasLookTarget = true;
	Out.LookTarget = bAimValid ? AimPoint : GetActorLocation() + FVector(GetFacing() * 400.f, 0.f, 60.f);
	Out.ComponentToWorld = GetMesh()->GetComponentTransform();
}

const UAnimSequence* APenguinCharacter::GetAnimClip(EPenguinClip Clip) const
{
	const int32 Index = static_cast<int32>(Clip);
	return LoadedClips.IsValidIndex(Index) ? LoadedClips[Index].Get() : nullptr;
}

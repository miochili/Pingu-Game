// Pingu - the three random abilities every penguin brings into a round.
//
// LAN: UseAbility, the Do* functions, the stomp / frost / shell / ice wall ticks and the pickups given run on the server
// only. Effects they cause play everywhere through FPinguFXBroadcastScope; what lasts (the frost cloud, the shell
// bubble, the gust's ring of puffs) every machine makes by itself from the replicated state.
//
// The icicle charges while its key is held (BeginCharge / ReleaseCharge): the server runs the charge and throws it; every
// machine shows the pull-out, the cocked arm and the throw from IcicleRep (the arm: UPenguinAnimInstance, the icicle in the
// flipper: here). A LAN player's own machine shows its charge at once and sends the press and the release (with the hold
// time it measured) to the server.

#include "PenguinAbilities.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguAntiCheat.h"
#include "PinguBuildLimit.h"
#include "PinguFX.h"
#include "PinguGiantSnowball.h"
#include "PinguSnowman.h"
#include "PinguPickup.h"
#include "PinguPortal.h"
#include "PinguSettings.h"
#include "PinguTornado.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "Templates/TypeCompatibleBytes.h"

namespace PenguinAbilitiesPrivate
{
	static const FPenguinAbilityInfo Infos[] = {
		{ EPenguinAbility::None, TEXT("None"), TEXT(""), TEXT(""), 1.f, false, FLinearColor::White, 0.f, false },
		{ EPenguinAbility::Snowball, TEXT("Snowball"), TEXT("Knocks penguins back. Every hit gives it right back, and every hit in a row pushes harder - keep the combo going! In the air: recoil, aim down to hop. Frozen penguins' ice just shrugs it off."), TEXT("Snowball"),
			0.9f, false, FLinearColor(0.85f, 0.93f, 1.f), 1000.f, true },
		{ EPenguinAbility::Icicle, TEXT("Icicle"), TEXT("Hold to throw faster and straighter - a quick tap is only a weak lob. Held 5 s at full charge it melts away. One hit knocks out! Stuck in a wall it is a jump pad that throws everyone up high."), TEXT("Icicle"),
			3.f, true, FLinearColor(0.4f, 0.85f, 1.f), 1200.f, true },
		{ EPenguinAbility::BellyStomp, TEXT("Belly Stomp"), TEXT("Slam down belly first and squash anyone below. Hold jump to bounce high off the slam."), TEXT("BellyStomp"),
			3.5f, true, FLinearColor(1.f, 0.6f, 0.2f), 220.f, false },
		{ EPenguinAbility::IcebergDrop, TEXT("Iceberg Drop"), TEXT("Drop an iceberg on the cursor: it crushes anyone under it and stays as ice that piles up. Over the sea: a floe."), TEXT("IcebergDrop"),
			6.f, true, FLinearColor(0.55f, 0.8f, 1.f), 1000.f, false },
		{ EPenguinAbility::FlipperSlap, TEXT("Flipper Slap"), TEXT("Stuns penguins for two seconds (they can't move or do anything) and knocks them back a little. Bats shots back, breaks ice, lunges you toward the cursor."), TEXT("FlipperSlap"),
			0.8f, false, FLinearColor(1.f, 0.85f, 0.3f), 160.f, false },
		{ EPenguinAbility::FrostBreath, TEXT("Frost Breath"), TEXT("Freeze the penguins in front of you. The blast pushes you the other way - aim down to fly up."), TEXT("FrostBreath"),
			4.5f, false, FLinearColor(0.6f, 0.95f, 1.f), 380.f, true },
		{ EPenguinAbility::IceWall, TEXT("Ice Wall"), TEXT("Raise a lasting wall of ice that lifts whoever stands there, or a ledge in mid-air. Hits break it block by block."), TEXT("IceWall"),
			3.5f, false, FLinearColor(0.5f, 0.8f, 1.f), 600.f, false },
		{ EPenguinAbility::SnowWarp, TEXT("Snow Warp"), TEXT("Teleport toward the cursor and leave a portal: anything that goes into one end comes out of the other. Warping into ice shatters it. Aim at your own shot to swap."), TEXT("SnowWarp"),
			8.f, false, FLinearColor(0.7f, 0.5f, 1.f), 700.f, false },
		{ EPenguinAbility::FishHook, TEXT("Fish Hook"), TEXT("Hook a penguin to throw it over your head! Reels in shots and bubbles, yanks out ice and pegs, or flies you there."), TEXT("FishHook"),
			3.f, false, FLinearColor(0.4f, 0.7f, 1.f), 1800.f, true },
		{ EPenguinAbility::IceShell, TEXT("Ice Shell"), TEXT("A bubble that blocks everything for a second and lets you float down gently - but wind blows it away."), TEXT("IceShell"),
			4.5f, false, FLinearColor(0.45f, 0.95f, 1.f), 0.f, false },
		{ EPenguinAbility::Gust, TEXT("Tornado"), TEXT("A short tornado round you: you hang still in its eye while it sucks penguins, shots and ice blocks in - then it flings it all out. The nearer the middle, the harder."), TEXT("Gust"),
			3.5f, false, FLinearColor(0.85f, 0.95f, 1.f), 450.f, false },
		{ EPenguinAbility::SnowBomb, TEXT("Snow Bomb"), TEXT("Lob a bomb that knocks out up close and blows up blocks. Careful: your own bomb knocks you out too! Slide in to kick it."), TEXT("SnowBomb"),
			5.5f, true, FLinearColor(0.35f, 0.55f, 1.f), 850.f, false },
		{ EPenguinAbility::Noot, TEXT("Noot"), TEXT("A shotgun honk: point blank it blasts penguins away, at range it barely pushes. Breaks ice, kicks you back."), TEXT("Noot"),
			2.5f, false, FLinearColor(1.f, 0.82f, 0.3f), 220.f, false },
		{ EPenguinAbility::IceGun, TEXT("Ice Gun"), TEXT("Ice bolt: whoever it hits slips on everything for 6 s. Snow it hits turns to ice; a peg it hits refreezes."), TEXT("IceGun"),
			2.f, false, FLinearColor(0.5f, 0.9f, 1.f), 950.f, true },
		{ EPenguinAbility::ShrinkRay, TEXT("Shrink Ray"), TEXT("Shrinks what it hits - penguins get tiny and light, shots too, ice melts away. A third hit and a penguin vanishes! Hold the key to shrink yourself."), TEXT("ShrinkRay"),
			3.5f, false, FLinearColor(0.85f, 0.45f, 1.f), 850.f, true },
		{ EPenguinAbility::GrowRay, TEXT("Grow Ray"), TEXT("Grows what it hits - big penguins are heavy, hit hard, are fast and jump high; ice grows a block. Past 8x size a penguin pops! Hold the key to grow yourself."), TEXT("GrowRay"),
			3.5f, false, FLinearColor(0.5f, 1.f, 0.4f), 850.f, true },
		{ EPenguinAbility::GiantSnowball, TEXT("Giant Snowball"), TEXT("Hold to roll up a huge snowball - you can't move meanwhile. It rolls on, grows and swallows everyone it touches, you too! Its third wall breaks it and frees them; off the edge, they all sink."), TEXT("GiantSnowball"),
			8.f, false, FLinearColor(0.88f, 0.95f, 1.f), 1300.f, false },
		{ EPenguinAbility::Snowman, TEXT("Snowman"), TEXT("Build a snowman that stays until something breaks it - and almost anything does. It throws snowballs at your enemies; its hits count for your snowball combo. Build as many as you like, one per block; on a taken block it goes one block further."), TEXT("Snowman"),
			6.175f, false, FLinearColor(1.f, 0.62f, 0.25f), 900.f, false },
		{ EPenguinAbility::Flap, TEXT("Flap"), TEXT("Flap your flippers like mad: a fall is caught at once and you flutter up - slowly, but high. From the ground you get even higher."), TEXT("Flap"),
			6.f, false, FLinearColor(1.f, 0.93f, 0.7f), 0.f, false },
	};
	static_assert(UE_ARRAY_COUNT(Infos) == static_cast<int32>(EPenguinAbility::Count), "ability table out of sync");

	// The gameplay numbers live in PinguTuning (PinguTuning.h): the PINGUPEDIA shows them, Traits mode scales them.
	using namespace PinguTuning;

	// What the throw looks like (every machine): the little icicle in the near flipper (SM_Icicle: 100 cm long, tip +X, its
	// pivot in the middle).
	constexpr float HandIcicleAppear = 0.03f;      // s into the pull-out it comes out from behind the back ...
	constexpr float HandIcicleGrown = 0.08f;       // ... full size here
	constexpr float HandIcicleLength = 0.62f;      // x 100 cm
	constexpr float HandIcicleThickness = 1.05f;
	constexpr float HandIcicleGrip = 16.f;         // cm of its thick end behind the flipper's hand
	constexpr float HandIcicleGlow = 0.7f;         // emissive: brighter as it charges ...
	constexpr float HandIcicleChargeGlow = 3.4f;   // ... this much more at full (a hot blue-white)
	constexpr float HandIcicleGlint = 3.f;         // the full charge glints this much brighter ...
	constexpr float HandIcicleGlintRate = 3.f;     // ... this many times a second
	// The charge builds: the icicle trembles more and more (fast, small), glows brighter and whiter, wears a cold rim and sheds
	// frost sparkles; all from the key's time (the same on every machine). Strongest at full.
	constexpr float HandShakeCurve = 1.3f;         // the trembling grows with the charge to this power (a slow start)
	constexpr float HandShakeMove = 3.4f;          // cm the icicle is thrown about at full (x size) ...
	constexpr float HandShakeTiltDeg = 5.5f;       // ... and the degrees it wobbles
	constexpr float HandHaloFull = 0.55f;          // the cold glow laid along it (APinguFX::GlowTrail, as the flying icicle's trail): its opacity at full charge ...
	constexpr float HandHaloCurve = 1.2f;          // ... growing with the charge to this power
	constexpr float HandHaloWidthStart = 12.f;     // its width (cm, x size) just pulled out ...
	constexpr float HandHaloWidthFull = 30.f;      // ... and at full charge
	constexpr float HandHaloLife = 0.05f;          // each piece of glow fades in this long (a trembling icicle smears it)
	constexpr float HandSparkleSlow = 0.55f;       // s between two frost sparkles just after the pull-out ...
	constexpr float HandSparkleFast = 0.06f;       // ... and at full charge (at the full effects density)
	constexpr float IcicleCockTiltDeg = 30.f;      // cocked, it points this far above the aim (a spear held up to throw)
	constexpr float ChargePredictTimeout = 0.6f;   // a LAN player's charge that the server never started is put away then
	// Facing the aim while the icicle shows (the character turns its mesh there): the side only changes once the aim is this far
	// (as the share of its length along the play plane's X) to the other side of straight up / down - no flicker on a vertical aim.
	constexpr float IcicleFacingDeadzone = 0.2f;
	// The aim crossing over (the penguin turns round to it): the icicle in the flipper shrinks away, is put in the other flipper at its
	// smallest and grows back, over this long (s) - so it never jumps across the body in the picture. (The arms swap roles over the
	// same time: PenguinAnimInstance's ThrowSwapTime.)
	constexpr float HandSwapTime = 0.1f;
	constexpr float HandSwapMinScale = 0.02f;

	// The slap's look (it was easy to miss): a bright swoosh along the swing on every slap, a gold burst on a hit.
	constexpr int32 SlapSwooshPieces = 7;
	constexpr float SlapSwooshRadius = 88.f;     // around the aim origin (x size)
	constexpr float SlapSwooshSweepDeg = 150.f;  // centred on the aim, from above it to below it
	constexpr float SlapSwooshWidth = 34.f;      // at its leading end (x size); the tail is thinner
	constexpr float SlapSwooshLife = 0.22f;      // the leading end's; the tail fades sooner, so the arc seems to sweep
	constexpr float SlapSwooshOpacity = 0.62f;

	// Walls block the area abilities (HasClearLine): the lines run from the user's chest (its aim origin) or head to the
	// other's middle or head; a head point is this share of the capsule's half height above the middle (the top of the
	// head: a one-block bump between two penguins on the same floor does not shield them, a two-block wall does).
	constexpr float LineUpperBody = 0.95f;

	// A LAN player's stomp starts on its own machine: the server's copy of the penguin shows it this much later at most
	// (until then "not stomping and on the ground" is the state from before the stomp, not an interruption).
	constexpr float StompRemoteGrace = 0.4f;

	/** The server's game clock: on a LAN client the replicated server time, else the world's own. */
	static double ServerNow(const UWorld* World)
	{
		if (!World)
		{
			return 0.0;
		}
		if (World->GetNetMode() == NM_Client)
		{
			if (const AGameStateBase* GameState = World->GetGameState())
			{
				return GameState->GetServerWorldTimeSeconds();
			}
		}
		return World->GetTimeSeconds();
	}

	// Area hits test the victim's size-scaled capsule, not its centre: big penguins are easier to hit, small ones
	// harder. The ranges were tuned on centre distances, so they subtract the capsule of a normal penguin.
	constexpr float NormalPenguinRadius = 30.f;
	constexpr float NormalPenguinHalfHeight = 56.f;

	template <typename FuncType>
	static void ForEachOtherPenguin(UWorld* World, const APenguinCharacter* Self, FuncType&& Func)
	{
		if (!World)
		{
			return;
		}
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			APenguinCharacter* Other = *It;
			if (Other != Self && IsValid(Other) && Other->IsAlive() && !Other->IsHidden())
			{
				Func(Other);
			}
		}
	}

	/** Everybody an attack of Self's reaches: every other penguin, its teammates too (team matches have friendly fire). */
	template <typename FuncType>
	static void ForEachOpponent(UWorld* World, const APenguinCharacter* Self, FuncType&& Func)
	{
		ForEachOtherPenguin(World, Self, Func);
	}

	/**
	 * Does the penguin's capsule reach into a cone (apex Origin, unit direction Dir, CosHalfAngle)? Range is measured
	 * from the apex to the capsule's surface. The cone is widened for penguins bigger than normal, narrowed for smaller ones.
	 */
	static bool IsInCone(const APenguinCharacter* Other, const FVector& Origin, const FVector& Dir, float Range, float CosHalfAngle)
	{
		const float Surface = Other->DistanceTo(Origin);
		if (Surface > Range)
		{
			return false;
		}
		if (Surface <= 40.f - NormalPenguinRadius)
		{
			return true; // right at the mouth of the cone
		}
		const UCapsuleComponent* Capsule = Other->GetCapsuleComponent();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		// The point of the capsule's axis closest to the cone's axis: a tall penguin sticks into a low cone. The axis is
		// shortened by a normal penguin's, so a normal one is tested at its centre like before.
		const float Half = FMath::Max(Capsule->GetScaledCapsuleHalfHeight() - Radius - (NormalPenguinHalfHeight - NormalPenguinRadius), 0.f);
		const FVector Center = Other->GetActorLocation();
		FVector OnCapsule;
		FVector OnAxis;
		FMath::SegmentDistToSegmentSafe(Center - FVector(0.f, 0.f, Half), Center + FVector(0.f, 0.f, Half), Origin, Origin + Dir * (Range + Radius),
			OnCapsule, OnAxis);
		FVector ToPoint = OnCapsule - Origin;
		ToPoint.Y = 0.f;
		const float Dist = FMath::Max(static_cast<float>(ToPoint.Size()), 1.f);
		const float Angle = FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(ToPoint / Dist, Dir)), -1.f, 1.f));
		const float Widen = FMath::Asin(FMath::Clamp((Radius - NormalPenguinRadius) / Dist, -1.f, 1.f));
		return Angle <= FMath::Acos(FMath::Clamp(CosHalfAngle, -1.f, 1.f)) + Widen;
	}

	/** How much of the noot's point-blank push is left at Distance: (1 - d/range)^2, never less than NootEdgeShare. */
	static float NootFalloff(float Distance, float Range)
	{
		const float Near = 1.f - FMath::Clamp(Distance / FMath::Max(Range, 1.f), 0.f, 1.f);
		return NootEdgeShare + (1.f - NootEdgeShare) * Near * Near;
	}

	/**
	 * Area abilities break the ice blocks they sweep over: up to MaxBlocks within Range of Origin (in a cone if
	 * CosHalfAngle > -1), nearest first, whose face toward Origin is open (walls, pillars, icebergs - not the inside of
	 * an ice floor) and never the floor right around the user's feet (no blasting the ground away under yourself, nor
	 * with a downward noot hop). Shards fly away from Origin (along the cone). Returns how many broke.
	 */
	static int32 BreakIceInArea(UWorld* World, const APenguinCharacter* User, const FVector& Origin, float Range, int32 MaxBlocks,
		const FVector& ConeDirection = FVector::ZeroVector, float CosHalfAngle = -2.f)
	{
		ABlockIsland* Island = ABlockIsland::FindNear(World, Origin);
		if (!Island || !User || MaxBlocks <= 0)
		{
			return 0;
		}
		const FVector Feet = User->GetFeetLocation();
		const float UserRadius = User->GetCapsuleComponent()->GetScaledCapsuleRadius();
		const FIntPoint SupportLeft = Island->WorldToCell(Feet + FVector(-UserRadius, 0.f, -10.f)) - FIntPoint(1, 0);
		const FIntPoint SupportRight = Island->WorldToCell(Feet + FVector(UserRadius, 0.f, -10.f)) + FIntPoint(1, 0);
		const bool bCone = CosHalfAngle > -1.f && !ConeDirection.IsNearlyZero();
		const FIntPoint Middle = Island->WorldToCell(Origin);
		const int32 Reach = FMath::CeilToInt(Range / Island->CellSize) + 1;
		TArray<TPair<float, FIntPoint>> Found;
		for (int32 X = Middle.X - Reach; X <= Middle.X + Reach; ++X)
		{
			for (int32 Z = Middle.Y - Reach; Z <= Middle.Y + Reach; ++Z)
			{
				if (Island->GetBlock(X, Z) != EBlockKind::Ice || (Z == SupportLeft.Y && X >= SupportLeft.X && X <= SupportRight.X))
				{
					continue;
				}
				FVector D = Island->CellToWorld(FIntPoint(X, Z)) - Origin;
				D.Y = 0.f;
				const float Dist = static_cast<float>(D.Size());
				if (Dist > Range || (bCone && Dist > 30.f && FVector::DotProduct(D / Dist, ConeDirection) < CosHalfAngle))
				{
					continue;
				}
				// The blast hits the face toward it: that one must be open.
				const bool bAlongX = FMath::Abs(D.X) >= FMath::Abs(D.Z);
				const FIntPoint Toward = bAlongX ? FIntPoint(D.X > 0.f ? X - 1 : X + 1, Z) : FIntPoint(X, D.Z > 0.f ? Z - 1 : Z + 1);
				if (!Island->IsSolidCell(Toward))
				{
					Found.Add(TPair<float, FIntPoint>(Dist, FIntPoint(X, Z)));
				}
			}
		}
		Found.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
		int32 Broken = 0;
		for (const TPair<float, FIntPoint>& Entry : Found)
		{
			if (Broken >= MaxBlocks)
			{
				break;
			}
			const FVector Center = Island->CellToWorld(Entry.Value);
			FVector Away = Center - Origin;
			Away.Y = 0.f;
			const FVector Push = bCone ? ConeDirection : Away.GetSafeNormal();
			Broken += Island->BreakIceAt(Center, Push) ? 1 : 0;
		}
		return Broken;
	}

	/** Wind can't hurt a penguin in an ice shell (the shell blocks the hit), but it blows the bubble along - no stun, so it
	 *  can steer back. Push is what the wind of By would have knocked it with. */
	static void BlowShell(const UObject* Ctx, APenguinCharacter* By, APenguinCharacter* Other, const FVector2D& Push)
	{
		UPenguinMovementComponent* Move = Other->GetPenguinMovement();
		if (!Move)
		{
			return;
		}
		Move->AddAbilityImpulse(Push * ShellWindShare + FVector2D(0.f, ShellWindLift)); // reaches a LAN player's own machine
		// Blown into the sea, it was By's knock-out (as after a hit).
		Other->RememberAttacker(By);
		APinguFX::Sparkles(Ctx, Other->GetAimOrigin(), 6, 60.f, FLinearColor(0.6f, 0.95f, 1.f));
		APinguFX::Sound(Ctx, EPinguSound::ShieldBlock, 0.5f, 0.06f, 1.3f);
	}

	/** Does a standing capsule (centre, radius, half height) reach into the square cell around CellCenter (play plane)? */
	static bool CapsuleTouchesCell(const FVector& Center, float Radius, float HalfHeight, const FVector& CellCenter, float HalfCell)
	{
		// Distance from the capsule's axis segment to the square; the capsule touches it when that is under its radius
		// (minus a hair: a penguin standing on a block floats a couple of centimetres above it).
		const float SegBottom = static_cast<float>(Center.Z) - (HalfHeight - Radius);
		const float SegTop = static_cast<float>(Center.Z) + (HalfHeight - Radius);
		const float Dx = FMath::Max3(static_cast<float>(CellCenter.X - HalfCell - Center.X), 0.f, static_cast<float>(Center.X - (CellCenter.X + HalfCell)));
		const float Dz = FMath::Max3(static_cast<float>(CellCenter.Z - HalfCell) - SegTop, 0.f, SegBottom - static_cast<float>(CellCenter.Z + HalfCell));
		return Dx * Dx + Dz * Dz < FMath::Square(Radius - 1.f);
	}

	/** A standing capsule (centre, radius, half height) at Location reaches into the ice shell bubble of a penguin other
	 *  than Self (a sphere of its shield radius around its chest). */
	static bool TouchesOtherShell(UWorld* World, const APenguinCharacter* Self, const FVector& Location, float Radius, float HalfHeight)
	{
		const FVector Half(0.f, 0.f, FMath::Max(HalfHeight - Radius, 0.f));
		bool bTouches = false;
		ForEachOtherPenguin(World, Self, [&](const APenguinCharacter* Other)
		{
			if (!bTouches && Other->IsShielded())
			{
				const FVector Chest = Other->GetAimOrigin();
				bTouches = FMath::PointDistToSegment(FVector(Chest.X, Location.Y, Chest.Z), Location - Half, Location + Half) < Radius + Other->GetShieldRadius();
			}
		});
		return bTouches;
	}

	/**
	 * Room for the penguin's capsule at Location: true if it is free, or if only ice blocks are in the way (they are
	 * listed in OutIce: warping into ice shatters it). Pegs, another penguin's ice shell bubble and anything else block it.
	 */
	static bool WarpRoomAt(UWorld* World, const APenguinCharacter* Penguin, ABlockIsland* Island, const FVector& Location, TArray<FIntPoint>& OutIce)
	{
		OutIce.Reset();
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		// Another penguin's ice shell blocks everything - a warp into the bubble too: it stops short of it, as at a peg.
		if (TouchesOtherShell(World, Penguin, Location, Radius, HalfHeight))
		{
			return false;
		}
		const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius, HalfHeight);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguWarp), false, Penguin);
		if (!World->OverlapBlockingTestByChannel(Location, FQuat::Identity, ECC_Pawn, Shape, Params))
		{
			return true;
		}
		if (!Island)
		{
			return false;
		}
		// Blocked. Only by the island (no peg or anything else) - and only by its ice blocks?
		Params.AddIgnoredActor(Island);
		if (World->OverlapBlockingTestByChannel(Location, FQuat::Identity, ECC_Pawn, Shape, Params))
		{
			return false;
		}
		const float HalfCell = Island->CellSize * 0.5f;
		const FIntPoint Min = Island->WorldToCell(Location - FVector(Radius, 0.f, HalfHeight));
		const FIntPoint Max = Island->WorldToCell(Location + FVector(Radius, 0.f, HalfHeight));
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Z = Min.Y; Z <= Max.Y; ++Z)
			{
				const EBlockKind Block = Island->GetBlock(X, Z);
				if (Block == EBlockKind::None || !CapsuleTouchesCell(Location, Radius, HalfHeight, Island->CellToWorld(FIntPoint(X, Z)), HalfCell))
				{
					continue;
				}
				if (Block != EBlockKind::Ice)
				{
					OutIce.Reset();
					return false;
				}
				OutIce.Add(FIntPoint(X, Z));
			}
		}
		return OutIce.Num() > 0;
	}
}

UPenguinAbilityComponent::UPenguinAbilityComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	// Loadout, cooldowns, counters, shell and frost go to every LAN machine (the penguin replicates, so do we).
	SetIsReplicatedByDefault(true);
	Abilities = { EPenguinAbility::Snowball, EPenguinAbility::FlipperSlap, EPenguinAbility::Icicle };
	CooldownEnds.Init(0.0, Abilities.Num());
	CooldownLengths.Init(0.f, Abilities.Num());
	UseCounts.Init(0, Abilities.Num());
	SwapCounts.Init(0, Abilities.Num());
	SeenUseCounts = UseCounts;
}

void UPenguinAbilityComponent::BeginPlay()
{
	Super::BeginPlay();
	// Clients: whatever arrived with the penguin is old news (no gust ring for uses from before we saw it), and the
	// shell bubble is shown afresh by the next tick if it is up.
	SeenUseCounts = UseCounts;
	RekeyShadow(); // (server / solo: the hidden shadow of the loadout the penguin starts with)
	bShieldVisualOn = false;
	FrostFxTimer = 0.f;
	// An icicle throw that came with the penguin is long over too.
	IcicleShow = FIcicleShowState();
	IcicleShow.Count = IcicleRep.Count;
	CreateHandIcicle();
}

void UPenguinAbilityComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPenguinAbilityComponent, Vq); // Abilities (the reflected names are meaningless on purpose: the header)
	DOREPLIFETIME(UPenguinAbilityComponent, Tz); // CooldownEnds
	DOREPLIFETIME(UPenguinAbilityComponent, Lw); // CooldownLengths
	DOREPLIFETIME(UPenguinAbilityComponent, Nc); // UseCounts
	DOREPLIFETIME(UPenguinAbilityComponent, Sk); // SwapCounts
	DOREPLIFETIME(UPenguinAbilityComponent, ShieldEndTime);
	DOREPLIFETIME(UPenguinAbilityComponent, FrostEndTime);
	DOREPLIFETIME(UPenguinAbilityComponent, FrostDirection);
	DOREPLIFETIME(UPenguinAbilityComponent, IcicleRep);
}

bool UPenguinAbilityComponent::HasGameplayAuthority() const
{
	const AActor* OwnerActor = GetOwner();
	return OwnerActor && OwnerActor->HasAuthority();
}

bool UPenguinAbilityComponent::IsMovementSimulatedHere() const
{
	const APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin)
	{
		return false;
	}
	if (Penguin->HasAuthority())
	{
		// A LAN player's penguin is moved by that player's machine; the host's own penguin, bots and solo ones by us.
		const APlayerController* OwningPC = Cast<APlayerController>(Penguin->GetController());
		return !OwningPC || OwningPC->IsLocalController();
	}
	return Penguin->GetLocalRole() == ROLE_AutonomousProxy;
}

double UPenguinAbilityComponent::GetServerNow() const
{
	return PenguinAbilitiesPrivate::ServerNow(GetWorld());
}

const FPenguinAbilityInfo& UPenguinAbilityComponent::GetInfo(EPenguinAbility Ability)
{
	const int32 Index = static_cast<int32>(Ability);
	return PenguinAbilitiesPrivate::Infos[FMath::Clamp(Index, 0, static_cast<int32>(EPenguinAbility::Count) - 1)];
}

bool UPenguinAbilityComponent::IsKnockback(EPenguinAbility Ability)
{
	return Ability == EPenguinAbility::Snowball || Ability == EPenguinAbility::FlipperSlap || Ability == EPenguinAbility::Gust
		|| Ability == EPenguinAbility::FishHook || Ability == EPenguinAbility::Noot;
}

bool UPenguinAbilityComponent::HasClearLine(const ABlockIsland* Island, const APenguinCharacter* User, const APenguinCharacter* Other)
{
	if (!Island || !User || !Other)
	{
		return true;
	}
	// Two points each, on the capsule's axis (never inside a block): the user's chest and head, the other's middle and
	// upper body - a wall that hides only part of a penguin does not shield it.
	using namespace PenguinAbilitiesPrivate;
	const FVector UserHead = User->GetActorLocation() + FVector(0.f, 0.f, LineUpperBody * User->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const FVector OtherMiddle = Other->GetActorLocation();
	const FVector OtherUpper = OtherMiddle + FVector(0.f, 0.f, LineUpperBody * Other->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	for (const FVector& From : { User->GetAimOrigin(), UserHead })
	{
		if (Island->IsLineClear(From, OtherMiddle) || Island->IsLineClear(From, OtherUpper))
		{
			return true;
		}
	}
	return false;
}

TArray<EPenguinAbility> UPenguinAbilityComponent::RollAbilities(FRandomStream& Rand, int32 Count)
{
	// Truly random (Linus 2026-11-05): Count different abilities out of all of them, no guaranteed deadly or knockback one.
	TArray<EPenguinAbility> Bag;
	for (int32 Index = 1; Index < static_cast<int32>(EPenguinAbility::Count); ++Index)
	{
		Bag.Add(static_cast<EPenguinAbility>(Index));
	}
	Count = FMath::Clamp(Count, 1, Bag.Num());
	TArray<EPenguinAbility> Result;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 Pick = Rand.RandRange(0, Bag.Num() - 1);
		Result.Add(Bag[Pick]);
		Bag.RemoveAt(Pick);
	}
	return Result;
}

TArray<EPenguinAbility> UPenguinAbilityComponent::RollLoadout(FRandomStream& Rand, int32 NumAbilities)
{
	if (NumAbilities >= 3)
	{
		return RollAbilities(Rand, 3);
	}
	// Two abilities in the outer slots: the middle one (where pickups go) stays empty.
	const TArray<EPenguinAbility> Two = RollAbilities(Rand, FMath::Max(NumAbilities, 1));
	TArray<EPenguinAbility> Loadout = { Two[0], EPenguinAbility::None, EPenguinAbility::None };
	if (Two.Num() > 1)
	{
		Loadout[2] = Two[1];
	}
	return Loadout;
}

APenguinCharacter* UPenguinAbilityComponent::GetPenguin() const
{
	return Cast<APenguinCharacter>(GetOwner());
}

UPenguinTraitComponent* UPenguinAbilityComponent::GetTraits() const
{
	const APenguinCharacter* Penguin = GetPenguin();
	return Penguin ? Penguin->GetTraits() : nullptr;
}

void UPenguinAbilityComponent::SizeSlotArrays()
{
	while (CooldownEnds.Num() < Abilities.Num())
	{
		CooldownEnds.Add(0.0);
	}
	while (CooldownLengths.Num() < Abilities.Num())
	{
		CooldownLengths.Add(0.f);
	}
	while (UseCounts.Num() < Abilities.Num())
	{
		UseCounts.Add(0);
	}
	while (SwapCounts.Num() < Abilities.Num())
	{
		SwapCounts.Add(0);
	}
	SyncShadow(ShadowGrowOnly);
}

// ---------------------------------------------------------------------------- the hidden shadow (2026-11-05, host-cheat-fix)

namespace PenguinAbilitiesPrivate
{
	uint8 ShadowByte(uint64 Key, int32 Slot)
	{
		return static_cast<uint8>((Key >> ((Slot % 8) * 8)) ^ (0x5Bu + 31u * static_cast<uint32>(Slot)));
	}
	uint64 ShadowWord(uint64 Key, int32 Slot)
	{
		return Key * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull * static_cast<uint64>(Slot + 1);
	}
}

void UPenguinAbilityComponent::RekeyShadow()
{
	// Any per-loadout value nobody can guess from outside: the cycle counter, the time and our address.
	Shadow.Key = (static_cast<uint64>(FPlatformTime::Cycles64()) * 0x2545F4914F6CDD1Dull) ^ (reinterpret_cast<UPTRINT>(this) << 7)
		^ static_cast<uint64>(FMath::Rand()) << 32;
	SyncShadow();
}

void UPenguinAbilityComponent::SyncShadow(int32 OnlySlot)
{
	using namespace PenguinAbilitiesPrivate;
	if (!HasGameplayAuthority())
	{
		return;
	}
	if (!Shadow.bValid)
	{
		OnlySlot = INDEX_NONE; // (the first one copies everything)
	}
	const int32 OldAbilities = Shadow.Ability.Num();
	const int32 OldEnds = Shadow.End.Num();
	Shadow.Ability.SetNum(Abilities.Num());
	Shadow.End.SetNum(CooldownEnds.Num());
	// Only what was just written (or added): a slot an outside write changed since the last GuardShadow is never copied.
	const auto Wanted = [OnlySlot](int32 Slot, int32 Old) { return OnlySlot == INDEX_NONE || Slot == OnlySlot || Slot >= Old; };
	for (int32 Slot = 0; Slot < Abilities.Num(); ++Slot)
	{
		if (Wanted(Slot, OldAbilities))
		{
			Shadow.Ability[Slot] = static_cast<uint8>(Abilities[Slot]) ^ ShadowByte(Shadow.Key, Slot);
		}
	}
	for (int32 Slot = 0; Slot < CooldownEnds.Num(); ++Slot)
	{
		if (Wanted(Slot, OldEnds))
		{
			Shadow.End[Slot] = BitCast<uint64>(CooldownEnds[Slot]) ^ ShadowWord(Shadow.Key, Slot);
		}
	}
	Shadow.bValid = true;
}

double UPenguinAbilityComponent::GetShadowEnd(int32 Slot) const
{
	if (!Shadow.bValid || !Shadow.End.IsValidIndex(Slot))
	{
		return CooldownEnds.IsValidIndex(Slot) ? CooldownEnds[Slot] : 0.0;
	}
	return BitCast<double>(Shadow.End[Slot] ^ PenguinAbilitiesPrivate::ShadowWord(Shadow.Key, Slot));
}

bool UPenguinAbilityComponent::GuardShadow()
{
	using namespace PenguinAbilitiesPrivate;
	if (!Shadow.bValid || !HasGameplayAuthority() || PinguAntiCheat::IsDevNoGuard())
	{
		return false;
	}
	bool bChanged = Abilities.Num() != Shadow.Ability.Num() || CooldownEnds.Num() != Shadow.End.Num();
	for (int32 Slot = 0; !bChanged && Slot < Abilities.Num(); ++Slot)
	{
		bChanged = static_cast<uint8>(Abilities[Slot]) != (Shadow.Ability[Slot] ^ ShadowByte(Shadow.Key, Slot));
	}
	for (int32 Slot = 0; !bChanged && Slot < CooldownEnds.Num(); ++Slot)
	{
		bChanged = BitCast<uint64>(CooldownEnds[Slot]) != (Shadow.End[Slot] ^ ShadowWord(Shadow.Key, Slot));
	}
	if (!bChanged)
	{
		return false;
	}
	// Something outside the game wrote them (the game itself always syncs the shadow): the real values go back.
	Abilities.SetNum(Shadow.Ability.Num());
	for (int32 Slot = 0; Slot < Abilities.Num(); ++Slot)
	{
		const uint8 Value = Shadow.Ability[Slot] ^ ShadowByte(Shadow.Key, Slot);
		Abilities[Slot] = Value < static_cast<uint8>(EPenguinAbility::Count) ? static_cast<EPenguinAbility>(Value) : EPenguinAbility::None;
	}
	CooldownEnds.SetNum(Shadow.End.Num());
	for (int32 Slot = 0; Slot < CooldownEnds.Num(); ++Slot)
	{
		CooldownEnds[Slot] = GetShadowEnd(Slot);
	}
	++ShadowRestores;
	const double Now = FPlatformTime::Seconds();
	if (Now - ShadowLastLog > 5.0)
	{
		ShadowLastLog = Now;
		UE_LOG(LogPingu, Warning, TEXT("[ANTICHEAT] %s: the loadout or a cooldown was changed from outside the game - put back (%d time(s))"),
			*GetNameSafe(GetOwner()), ShadowRestores);
	}
	return true;
}

void UPenguinAbilityComponent::PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker)
{
	Super::PreReplication(ChangedPropertyTracker);
	GuardShadow();
}

#if !UE_BUILD_SHIPPING
void UPenguinAbilityComponent::DevTrainerWrite(int32 Slot, EPenguinAbility Ability, bool bZeroCooldown)
{
	// What PinguTrainer.exe does from outside (2026-11-05): raw writes into the replicated arrays, never through the game.
	if (Abilities.IsValidIndex(Slot) && Ability != EPenguinAbility::None)
	{
		Abilities[Slot] = Ability;
	}
	if (bZeroCooldown)
	{
		for (double& End : CooldownEnds)
		{
			End = 0.0;
		}
	}
}
#endif

void UPenguinAbilityComponent::SetAbilities(const TArray<EPenguinAbility>& InAbilities)
{
	if (!HasGameplayAuthority())
	{
		return; // the loadout comes from the server
	}
	Abilities = InAbilities;
	CooldownEnds.Init(0.0, Abilities.Num());
	CooldownLengths.Init(0.f, Abilities.Num());
	UseCounts.Init(0, Abilities.Num());
	SwapCounts.Init(0, Abilities.Num());
	RekeyShadow(); // a new key for every loadout, and the shadow of this one
	ResetAll();
}

int32 UPenguinAbilityComponent::GiveAbility(EPenguinAbility Ability)
{
	if (!HasGameplayAuthority() || Ability == EPenguinAbility::None || Ability >= EPenguinAbility::Count)
	{
		return INDEX_NONE;
	}
	GuardShadow(); // (an outside write first goes back: the bubble works on the real loadout)
	// Already have it: recharged. Anything else replaces the pickup slot (its running effects just finish).
	int32 Slot = FindSlot(Ability);
	if (Slot == INDEX_NONE)
	{
		Slot = PickupSlot;
		// A loadout of two (Second Chance) never fills its empty pickup slot: the bubble replaces one of the two - the one
		// in the pickup slot, and with that one empty, the first.
		const UPenguinTraitComponent* Traits = GetTraits();
		if (Traits && Traits->GetNumAbilities() < 3 && GetAbility(PickupSlot) == EPenguinAbility::None && Abilities.Num() > 0)
		{
			Slot = 0;
		}
		while (Abilities.Num() <= Slot)
		{
			Abilities.Add(EPenguinAbility::None); // a loadout shorter than the pickup slot grows
		}
		Abilities[Slot] = Ability;
	}
	// Cooldowns and counters cover every slot.
	SizeSlotArrays();
	CooldownEnds[Slot] = 0.0;
	SyncShadow(Slot);
	++SwapCounts[Slot];
	return Slot;
}

void UPenguinAbilityComponent::ResetAll()
{
	if (!HasGameplayAuthority())
	{
		return; // the server's reset arrives by replication (a client's ResetForRound may call this too)
	}
	GuardShadow();
	for (double& End : CooldownEnds)
	{
		End = 0.0;
	}
	SyncShadow();
	for (float& Length : CooldownLengths)
	{
		Length = 0.f;
	}
	UseStrength = 1.f;
	StompStrength = 1.f;
	FrostStrength = 1.f;
	ShieldLeft = 0.f;
	ShieldEndTime = 0.0;
	FrostLeft = 0.f;
	FrostEndTime = 0.0;
	BraceEndTime = 0.0;
	SnowballComboHits = 0; // (a new round / loadout starts a new snowball combo)
	SnowballComboLastHit = -1000.0;
	bSnowballFromRefund = false;
	bStompArmed = false;
	bAirLungeUsed = false;
	FrostVictims.Reset();
	FrostShellBlocks.Reset();
	ShellBounces.Reset();
	PendingWallLeft = 0.f;
	PendingWallCells.Reset();
	PendingWallIsland.Reset();
	// An icicle being charged just goes (a new loadout, a new round: nothing to cancel).
	KeyChargeSlot = INDEX_NONE;
	KeyChargeAutoRelease = -1.f;
	if (Charge.Slot != INDEX_NONE || IcicleShow.Stage != EPinguIcicleShow::None)
	{
		Charge = FIcicleCharge();
		IcicleRep.Stage = 0;
		ShowChargeEnd(false);
	}
	if (APenguinCharacter* Penguin = GetPenguin())
	{
		Penguin->OnShieldChanged(false);
	}
}

float UPenguinAbilityComponent::GetCooldownRemaining(int32 Slot) const
{
	if (!CooldownEnds.IsValidIndex(Slot) || CooldownEnds[Slot] <= 0.0)
	{
		return 0.f;
	}
	// End times are on the server clock, so every machine counts the same cooldown down by itself.
	return FMath::Max(0.f, static_cast<float>(CooldownEnds[Slot] - GetServerNow()));
}

float UPenguinAbilityComponent::GetCooldownFraction(int32 Slot) const
{
	if (!CooldownEnds.IsValidIndex(Slot) || !Abilities.IsValidIndex(Slot))
	{
		return 0.f;
	}
	return FMath::Clamp(GetCooldownRemaining(Slot) / FMath::Max(GetCooldownLength(Slot), 0.01f), 0.f, 1.f);
}

float UPenguinAbilityComponent::GetCooldownLength(int32 Slot) const
{
	if (CooldownLengths.IsValidIndex(Slot) && CooldownLengths[Slot] > 0.f)
	{
		return CooldownLengths[Slot];
	}
	return GetInfo(GetAbility(Slot)).Cooldown;
}

void UPenguinAbilityComponent::ReduceCooldowns(float Seconds)
{
	if (!HasGameplayAuthority() || Seconds <= 0.f)
	{
		return;
	}
	GuardShadow();
	const double Now = GetServerNow();
	for (double& End : CooldownEnds)
	{
		if (End > Now)
		{
			End -= Seconds;
			if (End <= Now)
			{
				End = 0.0; // ready (the icon pops as when a cooldown runs out)
			}
		}
	}
	SyncShadow();
}

void UPenguinAbilityComponent::ReplaceAbility(int32 Slot, EPenguinAbility NewAbility)
{
	if (!HasGameplayAuthority() || !Abilities.IsValidIndex(Slot) || NewAbility >= EPenguinAbility::Count)
	{
		return;
	}
	GuardShadow();
	Abilities[Slot] = NewAbility;
	SizeSlotArrays();
	SyncShadow(Slot);
	++SwapCounts[Slot]; // (the cooldown's end and length stay: it carries over)
}

bool UPenguinAbilityComponent::IsReady(int32 Slot) const
{
	// An icicle being charged is not ready again either (its cooldown starts with the throw). The server goes by its hidden
	// shadow of the end (a memory trainer's 0 in the replicated array does not make it ready: 2026-11-05).
	if (!CooldownEnds.IsValidIndex(Slot) || Charge.Slot == Slot || KeyChargeSlot == Slot)
	{
		return false;
	}
	if (HasGameplayAuthority() && Shadow.bValid && !PinguAntiCheat::IsDevNoGuard())
	{
		const double End = GetShadowEnd(Slot);
		return End <= 0.0 || End <= GetServerNow();
	}
	return GetCooldownRemaining(Slot) <= 0.f;
}

bool UPenguinAbilityComponent::RefundCooldown(EPenguinAbility Ability)
{
	if (!HasGameplayAuthority())
	{
		return false; // cooldowns are the server's (they replicate to everybody)
	}
	GuardShadow();
	bool bRefunded = false;
	const double Now = GetServerNow();
	for (int32 Slot = 0; Slot < Abilities.Num(); ++Slot)
	{
		if (Abilities[Slot] == Ability && CooldownEnds.IsValidIndex(Slot) && CooldownEnds[Slot] > Now)
		{
			CooldownEnds[Slot] = 0.0; // ready: the HUD icon pops as it does when a cooldown runs out
			bRefunded = true;
		}
	}
	SyncShadow();
	return bRefunded;
}

int32 UPenguinAbilityComponent::GetNextSnowballComboHit() const
{
	// The chain goes on while the last hit is no older than the window; otherwise this hit is the first.
	if (SnowballComboHits > 0 && GetServerNow() - SnowballComboLastHit <= PinguTuning::SnowballComboWindow)
	{
		return SnowballComboHits + 1;
	}
	return 1;
}

int32 UPenguinAbilityComponent::NoteSnowballHit()
{
	if (!HasGameplayAuthority())
	{
		return 1;
	}
	SnowballComboHits = GetNextSnowballComboHit();
	SnowballComboLastHit = GetServerNow();
	bSnowballFromRefund = true; // (the caller gives the throw back right now: the next one goes on with the chain)
	return SnowballComboHits;
}

bool UPenguinAbilityComponent::IsShieldActive() const
{
	// The server counts its own shell down (gameplay); clients go by the replicated end time.
	return HasGameplayAuthority() ? ShieldLeft > 0.f : ShieldEndTime > GetServerNow();
}

float UPenguinAbilityComponent::GetShieldRemaining() const
{
	return HasGameplayAuthority() ? ShieldLeft : FMath::Max(0.f, static_cast<float>(ShieldEndTime - GetServerNow()));
}

bool UPenguinAbilityComponent::IsFrostActive() const
{
	return HasGameplayAuthority() ? FrostLeft > 0.f : FrostEndTime > GetServerNow();
}

bool UPenguinAbilityComponent::IsBraced() const
{
	return HasGameplayAuthority() && BraceEndTime > GetServerNow();
}

FVector UPenguinAbilityComponent::AimDirection(const FVector& AimPoint) const
{
	const APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin)
	{
		return FVector(1.f, 0.f, 0.f);
	}
	FVector Dir = AimPoint - Penguin->GetAimOrigin();
	Dir.Y = 0.f;
	if (Dir.SizeSquared() < 25.f)
	{
		Dir = FVector(Penguin->GetFacing(), 0.f, 0.f);
	}
	return Dir.GetSafeNormal();
}

bool UPenguinAbilityComponent::UseAbility(int32 Slot, const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	// Server only: a LAN client's character sends the press to the server (Rq02), never runs it here.
	// (-PinguCheat=cooldown, dev builds only: the anti-cheat test's cheating host ignores its own cooldowns.)
	const bool bCooldownCheat = PinguAntiCheat::IsHostCooldownCheat(Penguin);
	GuardShadow(); // the loadout and the cooldowns as the game set them, not as a memory trainer wrote them
	if (!HasGameplayAuthority() || !Penguin || !Abilities.IsValidIndex(Slot) || Abilities[Slot] == EPenguinAbility::None
		|| (!IsReady(Slot) && !bCooldownCheat) || !Penguin->IsAlive())
	{
		return false;
	}
	// Abilities work in the middle of anything - knocked back (the hit stun of an ordinary hit), dashing, sliding, climbing,
	// in the air. Only frozen, dizzy from a slap, knocked out, input locked or in our own stomp they don't (CanUseAbilities).
	const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
	if (!Move || !Move->CanUseAbilities())
	{
		return false;
	}
	// The icicle in one go (bots' and tests' ActivateAbility): a full charge, thrown as soon as it is pulled out. Its
	// cooldown starts with the throw (TickChargeOnServer).
	if (IsChargedAbility(Abilities[Slot]))
	{
		if (!BeginChargeOnServer(Slot))
		{
			return false;
		}
		ReleaseChargeOnServer(Slot, AimPoint, IcicleFullChargeTime);
		return true;
	}

	// The build limit (PinguBuildLimit, 2026-11-05): a placement past it - or a snowman without a free tile - is refused before
	// anything is spent (no cooldown, no use, no Gambler's roll), with a small "no" puff every machine sees.
	if (IsPlacingAbility(Abilities[Slot]))
	{
		FVector RefusedAt = AimPoint;
		FString Why;
		if (!CheckPlacement(Abilities[Slot], AimPoint, 1.f, &RefusedAt, &Why))
		{
			FPinguFXBroadcastScope Broadcast(this);
			PinguBuildLimit::ShowRefused(this, ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation()), RefusedAt,
				*FString::Printf(TEXT("%s's %s (%s)"), *Penguin->GetDisplayName(), GetInfo(Abilities[Slot]).Name, *Why));
			return false;
		}
	}

	// The snowball's combo goes on only with a throw the last hit gave back; any other throw (the last one missed or fizzled,
	// or the ability was waiting) starts a new chain.
	if (Abilities[Slot] == EPenguinAbility::Snowball)
	{
		if (!bSnowballFromRefund)
		{
			SnowballComboHits = 0;
		}
		bSnowballFromRefund = false;
	}

	// The strength of this use (Traits mode; 1 otherwise): what it launches and hits with.
	UPenguinTraitComponent* Traits = GetTraits();
	const EPenguinAbility UsedAbility = Abilities[Slot];
	UseStrength = Traits ? Traits->RollUseStrength(UsedAbility) : 1.f;
	if (UseStrength <= 0.f)
	{
		// A fizzle (Gambler): the press is spent, nothing happens.
		FizzleUse(Slot, UsedAbility, AimPoint);
		if (Traits)
		{
			Traits->OnAbilityUsed(Slot, UsedAbility, 0.f);
		}
		UseStrength = 1.f;
		return true;
	}
	bool bUsed = false;
	{
		// Whatever the ability does to the world is seen (and heard) on every LAN machine.
		FPinguFXBroadcastScope Broadcast(this);
		switch (Abilities[Slot])
		{
		case EPenguinAbility::Snowball: bUsed = DoSnowball(AimPoint); break;
		case EPenguinAbility::BellyStomp: bUsed = DoBellyStomp(); break;
		case EPenguinAbility::IcebergDrop: bUsed = DoIcebergDrop(AimPoint); break;
		case EPenguinAbility::FlipperSlap: bUsed = DoFlipperSlap(AimPoint); break;
		case EPenguinAbility::FrostBreath: bUsed = DoFrostBreath(AimPoint); break;
		case EPenguinAbility::IceWall: bUsed = DoIceWall(AimPoint); break;
		case EPenguinAbility::SnowWarp: bUsed = DoSnowWarp(AimPoint); break;
		case EPenguinAbility::FishHook: bUsed = DoFishHook(AimPoint); break;
		case EPenguinAbility::IceShell: bUsed = DoIceShell(); break;
		case EPenguinAbility::Gust: bUsed = DoGust(AimPoint); break;
		case EPenguinAbility::SnowBomb: bUsed = DoSnowBomb(AimPoint); break;
		case EPenguinAbility::Noot: bUsed = DoNoot(AimPoint); break;
		case EPenguinAbility::IceGun: bUsed = DoIceGun(AimPoint); break;
		case EPenguinAbility::ShrinkRay: bUsed = DoSizeRay(AimPoint, false); break;
		case EPenguinAbility::GrowRay: bUsed = DoSizeRay(AimPoint, true); break;
		case EPenguinAbility::GiantSnowball: bUsed = DoGiantSnowball(AimPoint); break;
		case EPenguinAbility::Snowman: bUsed = DoSnowman(AimPoint); break;
		case EPenguinAbility::Flap: bUsed = DoFlap(); break;
		default: break;
		}
	}
	if (bUsed && Abilities.IsValidIndex(Slot))
	{
		const float CooldownLength = GetInfo(Abilities[Slot]).Cooldown * (Traits ? Traits->GetCooldownScale() : 1.f);
		SizeSlotArrays();
		if (!bCooldownCheat)
		{
			CooldownEnds[Slot] = GetServerNow() + CooldownLength;
			if (Abilities[Slot] == EPenguinAbility::GiantSnowball)
			{
				// Its cooldown runs from the launch (a roll-up let go sooner starts it again then: RestartCooldown).
				CooldownEnds[Slot] += PinguTuning::GiantBuildTime;
			}
			CooldownLengths[Slot] = CooldownLength;
			SyncShadow(Slot);
		}
		if (UseCounts.IsValidIndex(Slot))
		{
			++UseCounts[Slot];
		}
		// A push braces its user for a moment: another push that reaches it now clashes instead of moving it.
		if (IsBracingPush(Abilities[Slot]))
		{
			BraceEndTime = GetServerNow() + BraceTime;
		}
		// Clients make these when the new use count arrives (OnRep_Nc).
		PlayLocalUseVisuals(Abilities[Slot]);
		if (Traits)
		{
			Traits->OnAbilityUsed(Slot, UsedAbility, UseStrength);
		}
	}
	UseStrength = 1.f; // (only valid during a use)
	// Bubbles the snow warp went through: taken now that this use is booked (one may replace or recharge this slot).
	for (const TWeakObjectPtr<APinguPickup>& Grab : WarpGrabs)
	{
		if (APinguPickup* Pickup = Grab.Get())
		{
			Pickup->GiveTo(Penguin);
		}
	}
	WarpGrabs.Reset();
	return bUsed;
}

bool UPenguinAbilityComponent::UseAbilityOnSelf(int32 Slot)
{
	const APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin || !CanUseOnSelf(GetAbility(Slot)))
	{
		return false;
	}
	// Aimed right at our own chest: DoSizeRay casts it on us (whatever the aim the owner's machine last sent).
	return UseAbility(Slot, Penguin->GetAimOrigin());
}

namespace
{
	/** Gambler's fizzle: how an ability looks for a moment before it turns to dust - the shape, whether it forms at the body
	 *  (the stomp, the shell, the gust) or at the flipper / beak, and its size against a normal one. */
	struct FFizzleLook
	{
		EPinguDustShape Shape = EPinguDustShape::Ball;
		bool bAtBody = false;
		float Size = 1.f;
	};

	FFizzleLook GetFizzleLook(EPenguinAbility Ability)
	{
		switch (Ability)
		{
		case EPenguinAbility::Snowball: return { EPinguDustShape::Ball, false, 1.f };
		case EPenguinAbility::Icicle: return { EPinguDustShape::Spike, false, 1.f };
		case EPenguinAbility::BellyStomp: return { EPinguDustShape::Block, true, 1.1f };
		case EPenguinAbility::IcebergDrop: return { EPinguDustShape::Block, false, 1.5f };
		case EPenguinAbility::FlipperSlap: return { EPinguDustShape::Cone, false, 0.9f };
		case EPenguinAbility::FrostBreath: return { EPinguDustShape::Cone, false, 1.2f };
		case EPenguinAbility::IceWall: return { EPinguDustShape::Block, false, 1.3f };
		case EPenguinAbility::SnowWarp: return { EPinguDustShape::Ring, false, 0.9f };
		case EPenguinAbility::FishHook: return { EPinguDustShape::Ball, false, 0.8f };
		case EPenguinAbility::IceShell: return { EPinguDustShape::Ring, true, 1.25f };
		case EPenguinAbility::Gust: return { EPinguDustShape::Ring, true, 1.1f };
		case EPenguinAbility::SnowBomb: return { EPinguDustShape::Ball, false, 1.2f };
		case EPenguinAbility::Noot: return { EPinguDustShape::Cone, false, 1.2f };
		case EPenguinAbility::IceGun: return { EPinguDustShape::Spike, false, 0.8f };
		case EPenguinAbility::ShrinkRay:
		case EPenguinAbility::GrowRay: return { EPinguDustShape::Spike, false, 1.f };
		case EPenguinAbility::GiantSnowball: return { EPinguDustShape::Ball, false, 1.6f };
		case EPenguinAbility::Snowman: return { EPinguDustShape::Block, false, 1.3f };
		case EPenguinAbility::Flap: return { EPinguDustShape::Ring, true, 0.9f };
		default: return FFizzleLook();
		}
	}
}

void UPenguinAbilityComponent::FizzleUse(int32 Slot, EPenguinAbility Ability, const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	if (!HasGameplayAuthority() || !Penguin || Slot == INDEX_NONE)
	{
		return;
	}
	// Half the cooldown (the trait's scale on it as ever). No use is counted: no card flash, no gust ring on the clients.
	const UPenguinTraitComponent* Traits = GetTraits();
	const float CooldownLength = GetInfo(Ability).Cooldown * (Traits ? Traits->GetCooldownScale() : 1.f) * PinguTraitTuning::GambleFizzleCooldown;
	SizeSlotArrays();
	if (CooldownEnds.IsValidIndex(Slot))
	{
		CooldownEnds[Slot] = GetServerNow() + CooldownLength;
		CooldownLengths[Slot] = CooldownLength;
		SyncShadow(Slot);
	}
	// The ability turns into sparkling dust where it would have come out (on every machine), with a muffled throw, a fizzling
	// sizzle that slides down and a little falling tinkle. The icon's own dust is the overhead widget's (it reads the
	// trait's events).
	FPinguFXBroadcastScope Broadcast(this);
	const float Size = Penguin->GetSizeScale();
	const FFizzleLook Look = GetFizzleLook(Ability);
	const FVector AimTo = AimPoint.IsNearlyZero()
		? (Penguin->HasAimPoint() ? Penguin->GetAimPoint() : Penguin->GetAimOrigin() + FVector(Penguin->GetFacing() * 400.f, 0.f, 0.f))
		: AimPoint;
	const FVector Dir = AimDirection(AimTo);
	const FVector Where = Look.bAtBody ? Penguin->GetAimOrigin() : Penguin->GetAimOrigin() + Dir * 45.f * Size;
	APinguFX::FizzleDust(this, Where, Dir, GetInfo(Ability).Color, Look.Size * Size, Look.Shape);
	APinguFX::Sound(this, EPinguSound::Throw, 0.3f, 0.05f, 0.6f);
	APinguFX::Sound(this, EPinguSound::Melt, 0.5f, 0.05f, 1.2f);
	APinguFX::Sound(this, EPinguSound::Drop, 0.3f, 0.05f, 1.35f);
}

// ---------------------------------------------------------------------------- projectiles

bool UPenguinAbilityComponent::DoSnowball(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	const FVector Dir = AimDirection(AimPoint);
	Penguin->PlayAction(Penguin->PickNearFlipper(EPenguinClip::Toss, EPenguinClip::TossL), 1.8f);
	APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Penguin, Penguin->GetAimOrigin() + Dir * 45.f,
		Dir * PenguinAbilitiesPrivate::SnowballSpeed + FVector(0.f, 0.f, 120.f));
	APinguFX::Sound(this, EPinguSound::Throw);
	// Movement: the throw's recoil nudges you in the air (throw down for a little hop, up to drop faster).
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		const bool bAir = !Move->IsGroundedForAbilities();
		if (bAir || Dir.Z < -0.6f)
		{
			const float Side = bAir ? static_cast<float>(-Dir.X) * PenguinAbilitiesPrivate::SnowballRecoil : 0.f;
			const float Up = static_cast<float>(-Dir.Z) * PenguinAbilitiesPrivate::SnowballRecoil * 1.25f;
			Move->AddAbilityImpulse(FVector2D(Side, Up));
		}
	}
	return true;
}

bool UPenguinAbilityComponent::DoIcicle(const FVector& AimPoint, float Power)
{
	using namespace PenguinAbilitiesPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	float Speed = IcicleSpeed;
	float Gravity = 0.f;
	float LiftDegrees = 0.f;
	GetIcicleThrow(Power, Speed, Gravity, LiftDegrees);
	FVector Dir = AimDirection(AimPoint);
	if (LiftDegrees > 0.f)
	{
		// A weak throw points a little above the aim (never past straight up), on whichever side it goes; a full one none.
		const float Side = FMath::Abs(Dir.X) > 1.e-3 ? FMath::Sign(static_cast<float>(Dir.X)) : Penguin->GetFacing();
		const float Angle = FMath::Min(FMath::Atan2(static_cast<float>(Dir.Z), FMath::Abs(static_cast<float>(Dir.X))) + FMath::DegreesToRadians(LiftDegrees), UE_HALF_PI);
		Dir = FVector(Side * FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
	}
	// (The arm's throw is the code-driven icicle throw every machine shows: UPenguinAnimInstance.)
	const FVector Start = Penguin->GetAimOrigin() + Dir * 50.f;
	if (APenguinProjectile* Icicle = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Icicle, Penguin, Start, Dir * Speed))
	{
		if (Gravity > 0.f)
		{
			Icicle->SetGravity(Gravity); // set before it first replicates: every machine flies it in the same arc
		}
	}
	// A full throw whooshes, a tap is a lighter toss.
	APinguFX::Sound(this, EPinguSound::IcicleThrow, FMath::Lerp(0.6f, 1.f, Power), 0.06f, FMath::Lerp(1.18f, 1.f, Power));
	APinguFX::Sparkles(this, Start, FMath::RoundToInt(FMath::Lerp(3.f, 6.f, Power)), 20.f, FLinearColor(0.6f, 0.9f, 1.f));
	return true;
}

// ---------------------------------------------------------------------------- the icicle charge

float UPenguinAbilityComponent::GetIciclePower(float HoldSeconds)
{
	return FMath::Clamp(HoldSeconds / IcicleFullChargeTime, 0.f, 1.f);
}

void UPenguinAbilityComponent::GetIcicleThrow(float Power, float& OutSpeed, float& OutGravity, float& OutLiftDegrees)
{
	using namespace PenguinAbilitiesPrivate;
	const float P = FMath::Clamp(Power, 0.f, 1.f);
	OutSpeed = FMath::Lerp(IcicleTapSpeed, IcicleSpeed, P);
	OutGravity = P >= 1.f ? 0.f : IcicleTapGravity * FMath::Pow(1.f - P, IcicleGravityFalloff);
	OutLiftDegrees = IcicleTapLiftDeg * (1.f - P);
}

float UPenguinAbilityComponent::GetChargeShakeBuild(float ChargeSeconds)
{
	using namespace PenguinAbilitiesPrivate;
	return FMath::Pow(FMath::Clamp(ChargeSeconds / IcicleFullChargeTime, 0.f, 1.f), HandShakeCurve);
}

bool UPenguinAbilityComponent::GetHandIcicleState(FVector& OutCenter, FVector& OutTip, float& OutScale) const
{
	if (!bHandIcicleShown || !HandIcicle)
	{
		return false;
	}
	OutCenter = HandIcicleCenter;
	OutTip = HandIcicleTip;
	OutScale = HandIcicleDrawScale;
	return true;
}

FVector UPenguinAbilityComponent::GetCurrentAim() const
{
	const APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin)
	{
		return FVector::ZeroVector;
	}
	return Penguin->HasAimPoint() ? Penguin->GetAimPoint() : Penguin->GetAimOrigin() + FVector(Penguin->GetFacing() * 400.f, 0.f, 0.f);
}

float UPenguinAbilityComponent::PickIcicleSide(float Current) const
{
	using namespace PenguinAbilitiesPrivate;
	const APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin)
	{
		return Current;
	}
	FVector ToAim = GetCurrentAim() - Penguin->GetAimOrigin();
	ToAim.Y = 0.f;
	// (On top of the penguin, or straight up / down: no side to turn to - it keeps the one it has, or faces where it moves.)
	const float Along = ToAim.SizeSquared() < 25.f ? 0.f : static_cast<float>(ToAim.GetSafeNormal().X);
	if (Current == 0.f)
	{
		return FMath::Abs(Along) > IcicleFacingDeadzone ? FMath::Sign(Along) : Penguin->GetFacing();
	}
	return FMath::Abs(Along) > IcicleFacingDeadzone ? FMath::Sign(Along) : Current;
}

bool UPenguinAbilityComponent::IsChargeBroken(int32 HitsAtStart) const
{
	const APenguinCharacter* Penguin = GetPenguin();
	const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	// Frozen, dizzy from a slap, knocked out, locked (the round is over) or stomping - or a hit that stuns since the press
	// (a knockback flight the press was made in goes on: that is no new hit).
	if (!Penguin || !Move || !Penguin->IsAlive() || !Move->CanUseAbilities() || Penguin->IsDizzy())
	{
		return true;
	}
	return Move->HitEvents != HitsAtStart && Move->IsStunned();
}

bool UPenguinAbilityComponent::BeginChargeOnServer(int32 Slot)
{
	APenguinCharacter* Penguin = GetPenguin();
	if (!HasGameplayAuthority() || !Penguin || !Abilities.IsValidIndex(Slot) || !IsChargedAbility(Abilities[Slot]) || Charge.Slot != INDEX_NONE
		|| !IsReady(Slot) || IsChargeBroken(Penguin->GetPenguinMovement() ? Penguin->GetPenguinMovement()->HitEvents : 0))
	{
		return false;
	}
	Charge = FIcicleCharge();
	Charge.Slot = Slot;
	Charge.StartTime = GetServerNow();
	Charge.HitsAtStart = Penguin->GetPenguinMovement()->HitEvents;
	// Every machine shows the pull-out from here (a LAN owner already does).
	++IcicleRep.Count;
	IcicleRep.Stage = 1;
	ShowChargeBegin(IcicleRep.Count);
	return true;
}

void UPenguinAbilityComponent::ReleaseChargeOnServer(int32 Slot, const FVector& AimPoint, float HoldSeconds)
{
	if (!HasGameplayAuthority() || Charge.Slot != Slot || Slot == INDEX_NONE || Charge.bReleased)
	{
		return; // (cancelled, or never started: nothing to throw)
	}
	using namespace PenguinAbilitiesPrivate;
	Charge.bReleased = true;
	Charge.ReleaseTime = GetServerNow();
	Charge.HoldSeconds = FMath::Clamp(HoldSeconds, 0.f, 60.f);
	Charge.Aim = AimPoint;
	// Let go before the icicle was out: it is thrown as soon as it is.
	Charge.SwingAt = FMath::Max(GetServerNow(), Charge.StartTime + IciclePullOutTime);
	IcicleRep.Stage = 2;
	ShowChargeRelease();
}

void UPenguinAbilityComponent::EndChargeOnServer(bool bCancel, bool bMelt)
{
	if (!HasGameplayAuthority() || Charge.Slot == INDEX_NONE)
	{
		return;
	}
	const int32 Slot = Charge.Slot;
	Charge = FIcicleCharge();
	if (KeyChargeSlot == Slot)
	{
		// The key is still down on this machine (a penguin the server controls): its release throws nothing now.
		KeyChargeSlot = INDEX_NONE;
		KeyChargeAutoRelease = -1.f;
	}
	if (bCancel)
	{
		// The icicle breaks in the flipper and the cooldown is used all the same (no free cancel).
		if (CooldownEnds.IsValidIndex(Slot) && GetAbility(Slot) == EPenguinAbility::Icicle)
		{
			const UPenguinTraitComponent* Traits = GetTraits();
			const float CooldownLength = GetInfo(EPenguinAbility::Icicle).Cooldown * (Traits ? Traits->GetCooldownScale() : 1.f);
			CooldownEnds[Slot] = GetServerNow() + CooldownLength;
			SizeSlotArrays();
			CooldownLengths[Slot] = CooldownLength;
			SyncShadow(Slot);
		}
		if (bMelt)
		{
			++IciclesMelted;
		}
		else
		{
			++IciclesCancelled;
		}
		UE_LOG(LogPingu, Log, TEXT("[ICICLE] %s: the charge %s (cooldown used)"), GetPenguin() ? *GetPenguin()->GetDisplayName() : TEXT("?"),
			bMelt ? TEXT("melted away") : TEXT("was cancelled"));
	}
	IcicleRep.Stage = bMelt ? 4 : bCancel ? 3 : 0;
	ShowChargeEnd(bCancel, bMelt);
}

void UPenguinAbilityComponent::TickChargeOnServer()
{
	if (Charge.Slot == INDEX_NONE)
	{
		return;
	}
	using namespace PenguinAbilitiesPrivate;
	// A bubble swapped the icicle out of its slot: the charge just goes. Stunned, frozen, knocked out, the round over: cancelled.
	if (GetAbility(Charge.Slot) != EPenguinAbility::Icicle)
	{
		EndChargeOnServer(false);
		return;
	}
	if (IsChargeBroken(Charge.HitsAtStart))
	{
		EndChargeOnServer(true);
		return;
	}
	const double Now = GetServerNow();
	if (!Charge.bReleased && Now - Charge.StartTime >= IcicleFullChargeTime + IcicleMeltAfterFull)
	{
		// Held at full charge for too long: it melts in the flipper, nothing is thrown, the cooldown starts (a late release
		// finds no charge). The host decides: a LAN owner's own hold time is never asked.
		EndChargeOnServer(true, true);
		return;
	}
	if (!Charge.bReleased || Now < Charge.SwingAt + IcicleSwingTime - 0.001)
	{
		return;
	}
	// The swing is through: the icicle leaves the flipper, as far as the hold made it, where the release aimed.
	const int32 Slot = Charge.Slot;
	const float Hold = Charge.HoldSeconds;
	const FVector Aim = Charge.Aim;
	LastIciclePressTime = Charge.StartTime;
	LastIcicleReleaseTime = Charge.ReleaseTime;
	LastIcicleThrowTime = Now;
	Charge = FIcicleCharge();
	const float Power = GetIciclePower(Hold);
	// The strength of this use (Traits mode; 1 otherwise), rolled as it leaves the flipper.
	UPenguinTraitComponent* Traits = GetTraits();
	UseStrength = Traits ? Traits->RollUseStrength(EPenguinAbility::Icicle) : 1.f;
	if (UseStrength <= 0.f)
	{
		// A fizzle (Gambler): the arm swung, but no icicle leaves the flipper.
		FizzleUse(Slot, EPenguinAbility::Icicle);
		if (Traits)
		{
			Traits->OnAbilityUsed(Slot, EPenguinAbility::Icicle, 0.f);
		}
		UseStrength = 1.f;
		return;
	}
	{
		FPinguFXBroadcastScope Broadcast(this);
		DoIcicle(Aim, Power);
	}
	const float CooldownLength = GetInfo(EPenguinAbility::Icicle).Cooldown * (Traits ? Traits->GetCooldownScale() : 1.f);
	CooldownEnds[Slot] = Now + CooldownLength; // from the throw: a long hold does not eat it
	SizeSlotArrays();
	CooldownLengths[Slot] = CooldownLength;
	SyncShadow(Slot);
	if (UseCounts.IsValidIndex(Slot))
	{
		++UseCounts[Slot];
	}
	if (Traits)
	{
		Traits->OnAbilityUsed(Slot, EPenguinAbility::Icicle, UseStrength);
	}
	UseStrength = 1.f;
	LastIciclePower = Power;
	LastIcicleHold = Hold;
	++IciclesThrown;
	UE_LOG(LogPingu, Log, TEXT("[ICICLE] %s threw: held %.2f s, charge %.2f"), GetPenguin() ? *GetPenguin()->GetDisplayName() : TEXT("?"), Hold, Power);
}

bool UPenguinAbilityComponent::BeginChargeOnOwner(int32 Slot)
{
	APenguinCharacter* Penguin = GetPenguin();
	const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	// A LAN player's own penguin: shown at once (the server's charge shows up a moment later and is the same one).
	if (!Penguin || !Move || Penguin->GetLocalRole() != ROLE_AutonomousProxy || !IsReady(Slot) || IsChargeBroken(Move->HitEvents))
	{
		return false;
	}
	bChargePredicted = true;
	ChargePredictAge = 0.f;
	ShowChargeBegin(static_cast<uint8>(IcicleRep.Count + 1));
	Rq12(Slot);
	return true;
}

bool UPenguinAbilityComponent::BeginCharge(int32 Slot)
{
	APenguinCharacter* Penguin = GetPenguin();
	const UWorld* World = GetWorld();
	if (!Penguin || !World || !IsChargedAbility(GetAbility(Slot)) || KeyChargeSlot != INDEX_NONE)
	{
		return false; // (a press while one charges already: that one goes on, its release throws)
	}
	const bool bStarted = HasGameplayAuthority() ? BeginChargeOnServer(Slot) : BeginChargeOnOwner(Slot);
	if (!bStarted)
	{
		return false;
	}
	KeyChargeSlot = Slot;
	KeyChargeStart = World->GetTimeSeconds();
	KeyChargeHits = Penguin->GetPenguinMovement() ? Penguin->GetPenguinMovement()->HitEvents : 0;
	KeyChargeAutoRelease = -1.f;
	return true;
}

void UPenguinAbilityComponent::ReleaseCharge(int32 Slot)
{
	APenguinCharacter* Penguin = GetPenguin();
	const UWorld* World = GetWorld();
	if (Slot == INDEX_NONE || KeyChargeSlot != Slot || !Penguin || !World)
	{
		return;
	}
	// The hold as this machine measured it: from the press to now (game time).
	const float Hold = static_cast<float>(World->GetTimeSeconds() - KeyChargeStart);
	KeyChargeSlot = INDEX_NONE;
	KeyChargeAutoRelease = -1.f;
	const FVector Aim = GetCurrentAim();
	if (HasGameplayAuthority())
	{
		ReleaseChargeOnServer(Slot, Aim, Hold);
		return;
	}
	// A LAN player: the swing shows now, the server throws it with our aim and hold (a charge it cancelled stays cancelled).
	ShowChargeRelease();
	Rq13(Slot, FVector_NetQuantize10(Aim), Penguin->IsStickAim(), Hold);
}

void UPenguinAbilityComponent::DropCharge()
{
	const bool bKeyCharge = KeyChargeSlot != INDEX_NONE;
	KeyChargeSlot = INDEX_NONE;
	KeyChargeAutoRelease = -1.f;
	if (HasGameplayAuthority())
	{
		// Thrown already (the swing runs): that one still goes.
		if (Charge.Slot != INDEX_NONE && !Charge.bReleased)
		{
			EndChargeOnServer(IsChargeBroken(Charge.HitsAtStart));
		}
		return;
	}
	if (bKeyCharge)
	{
		if (IcicleShow.Stage == EPinguIcicleShow::Charging && !IcicleShow.bReleasePending)
		{
			ShowChargeEnd(false);
		}
		Rq14();
	}
}

bool UPenguinAbilityComponent::ChargeAndThrow(int32 Slot, float HoldSeconds)
{
	if (!BeginCharge(Slot))
	{
		return false;
	}
	// Never held into the melt (IcicleMeltAfterFull): a hold asked for that long is let go IcicleBotMeltMargin s before it.
	KeyChargeAutoRelease = FMath::Clamp(HoldSeconds, 0.f, IcicleFullChargeTime + IcicleMeltAfterFull - PinguTuning::IcicleBotMeltMargin);
	return true;
}

void UPenguinAbilityComponent::TickChargeRelease()
{
	const UWorld* World = GetWorld();
	if (KeyChargeSlot == INDEX_NONE || KeyChargeAutoRelease < 0.f || !World)
	{
		return;
	}
	if (World->GetTimeSeconds() - KeyChargeStart >= KeyChargeAutoRelease - 0.001)
	{
		ReleaseCharge(KeyChargeSlot);
	}
}

void UPenguinAbilityComponent::Rq12_Implementation(int32 Slot)
{
	if (PinguAntiCheat::AllowRequest(GetOwner(), PinguAntiCheat::EGate::Ability))
	{
		BeginChargeOnServer(Slot); // (the host's loadout, cooldown and state checks)
	}
}

void UPenguinAbilityComponent::Rq13_Implementation(int32 Slot, const FVector_NetQuantize10& InAimPoint, bool bFromStick, float HoldSeconds)
{
	APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin || !PinguAntiCheat::AllowRequest(GetOwner(), PinguAntiCheat::EGate::Ability))
	{
		return;
	}
	if (InAimPoint.ContainsNaN() || !FMath::IsFinite(HoldSeconds))
	{
		PinguAntiCheat::NoteIgnored(GetOwner(), TEXT("an icicle release"), TEXT("not a number"));
		return;
	}
	// Aimed where the owner aimed at the release (the others see it too), as far as the owner held it - but never longer than
	// the host saw the charge run (the press and the release travel the same way: a little slack for the jitter).
	const float Hold = Charge.Slot == Slot && Charge.Slot != INDEX_NONE
		? PinguAntiCheat::ClampHold(GetOwner(), HoldSeconds, GetServerNow() - Charge.StartTime) : HoldSeconds;
	Penguin->SetAimPoint(InAimPoint, bFromStick);
	ReleaseChargeOnServer(Slot, Penguin->GetAimPoint(), Hold);
}

void UPenguinAbilityComponent::Rq14_Implementation()
{
	if (Charge.Slot != INDEX_NONE && !Charge.bReleased && PinguAntiCheat::AllowRequest(GetOwner(), PinguAntiCheat::EGate::Ability))
	{
		EndChargeOnServer(IsChargeBroken(Charge.HitsAtStart));
	}
}

void UPenguinAbilityComponent::RestartCooldown(int32 Slot)
{
	if (!HasGameplayAuthority() || !Abilities.IsValidIndex(Slot))
	{
		return;
	}
	SizeSlotArrays();
	const float Length = CooldownLengths[Slot] > 0.f ? CooldownLengths[Slot] : GetInfo(Abilities[Slot]).Cooldown;
	CooldownEnds[Slot] = GetServerNow() + Length;
	CooldownLengths[Slot] = Length;
	SyncShadow(Slot);
}

void UPenguinAbilityComponent::ReleaseRollUp(int32 Slot)
{
	APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin)
	{
		return;
	}
	if (!HasGameplayAuthority())
	{
		if (Penguin->GetLocalRole() == ROLE_AutonomousProxy)
		{
			Rq15(Slot); // (the server's roll-up: it crumbles there, everybody sees it)
		}
		return;
	}
	TArray<APinguGiantSnowball*> All;
	APinguGiantSnowball::GetAll(GetWorld(), All);
	for (APinguGiantSnowball* Giant : All)
	{
		if (Giant->IsBuilding() && Giant->GetRoller() == Penguin && (Slot == INDEX_NONE || Giant->GetSlot() == Slot))
		{
			Giant->CancelRollUp(TEXT("let go"));
		}
	}
}

void UPenguinAbilityComponent::Rq15_Implementation(int32 Slot)
{
	if (PinguAntiCheat::AllowRequest(GetOwner(), PinguAntiCheat::EGate::Ability))
	{
		ReleaseRollUp(Slot);
	}
}

bool UPenguinAbilityComponent::DoGiantSnowball(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	// Rolled up from the snow under its feet: only standing on the ground (in the air the press does nothing).
	if (!Move || !Move->IsMovingOnGround())
	{
		return false;
	}
	const int32 Slot = FindSlot(EPenguinAbility::GiantSnowball);
	const float Dx = static_cast<float>(AimPoint.X - Penguin->GetActorLocation().X);
	const float Side = FMath::Abs(Dx) > 20.f ? FMath::Sign(Dx) : Penguin->GetFacing();
	return APinguGiantSnowball::BeginRollUp(Penguin, Slot, Side) != nullptr;
}

bool UPenguinAbilityComponent::DoSnowman(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	// On the tile in front of it toward the aim (or the facing); taken: the next one on; in the air, no tile, the next one taken
	// too or past the build limit: nothing happens, no cooldown, a "no" puff (APinguSnowman::FindSpot). As many as you like.
	const float Dx = static_cast<float>(AimPoint.X - Penguin->GetActorLocation().X);
	const float Side = FMath::Abs(Dx) > 20.f ? FMath::Sign(Dx) : Penguin->GetFacing();
	FVector Feet;
	FVector RefusedAt;
	FString Why;
	if (!APinguSnowman::FindSpot(Penguin, Side, Feet, &RefusedAt, &Why))
	{
		PinguBuildLimit::ShowRefused(this, ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation()), RefusedAt,
			*FString::Printf(TEXT("%s's Snowman (%s)"), *Penguin->GetDisplayName(), *Why));
		return false;
	}
	return APinguSnowman::Build(Penguin, Feet, Side) != nullptr;
}

// ---------------------------------------------------------------------------- the build limit (PinguBuildLimit, 2026-11-05)

bool UPenguinAbilityComponent::IsPlacingAbility(EPenguinAbility Ability)
{
	// What leaves something solid or standing in the world. (Thrown things don't count: the icicle's pad, a hooked ice block.
	// The grow ray's new ice block is checked where it grows: APenguinProjectile.)
	return Ability == EPenguinAbility::IceWall || Ability == EPenguinAbility::IcebergDrop || Ability == EPenguinAbility::Snowman;
}

bool UPenguinAbilityComponent::IsPlacementAllowed(EPenguinAbility Ability, const FVector& AimPoint) const
{
	return CheckPlacement(Ability, AimPoint, 1.f, nullptr, nullptr);
}

FVector UPenguinAbilityComponent::GetIcebergTarget(const FVector& AimPoint, float Strength) const
{
	const APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin)
	{
		return AimPoint;
	}
	// It reaches further (or less far) with the use's strength (Traits mode; 1 normally).
	const float Reach = FMath::Clamp(Strength, PinguTraitTuning::ReachScaleMin, PinguTraitTuning::ReachScaleMax);
	const float SelfX = Penguin->GetActorLocation().X;
	const float SelfZ = Penguin->GetActorLocation().Z;
	return FVector(FMath::Clamp(static_cast<float>(AimPoint.X), SelfX - PinguTuning::IcebergReachX * Reach, SelfX + PinguTuning::IcebergReachX * Reach), 0.f,
		FMath::Clamp(static_cast<float>(AimPoint.Z), SelfZ - PinguTuning::IcebergReachDown * Reach, SelfZ + PinguTuning::IcebergReachUp * Reach));
}

bool UPenguinAbilityComponent::CheckPlacement(EPenguinAbility Ability, const FVector& AimPoint, float Strength, FVector* OutRefusedAt, FString* OutWhy) const
{
	const APenguinCharacter* Penguin = GetPenguin();
	if (!Penguin || !IsPlacingAbility(Ability))
	{
		return true;
	}
	const auto Refuse = [OutRefusedAt, OutWhy](const FVector& At, const TCHAR* Why)
	{
		if (OutRefusedAt)
		{
			*OutRefusedAt = At;
		}
		if (OutWhy)
		{
			*OutWhy = Why;
		}
		return false;
	};
	switch (Ability)
	{
	case EPenguinAbility::IceWall:
	{
		ABlockIsland* Island = nullptr;
		TArray<FIntPoint> Cells;
		TArray<FIntPoint> BigOrigins;
		FVector Target;
		if (PlanIceWall(AimPoint, Strength, Island, Cells, BigOrigins, Target) && !PinguBuildLimit::AllowsCells(Island, Cells))
		{
			return Refuse(Target, TEXT("past the build limit"));
		}
		return true;
	}
	case EPenguinAbility::IcebergDrop:
	{
		const FVector Target = GetIcebergTarget(AimPoint, Strength);
		FVector LandedAt = Target;
		if (!AIcebergDrop::IsDropAllowed(GetWorld(), Penguin, static_cast<float>(Target.X), static_cast<float>(Target.Z), 0.f, &LandedAt))
		{
			return Refuse(LandedAt, TEXT("past the build limit"));
		}
		return true;
	}
	case EPenguinAbility::Snowman:
	{
		const float Dx = static_cast<float>(AimPoint.X - Penguin->GetActorLocation().X);
		const float Side = FMath::Abs(Dx) > 20.f ? FMath::Sign(Dx) : Penguin->GetFacing();
		FVector Feet;
		FVector RefusedAt = AimPoint;
		FString Why;
		if (!APinguSnowman::FindSpot(Penguin, Side, Feet, &RefusedAt, &Why))
		{
			if (OutWhy)
			{
				*OutWhy = Why;
			}
			if (OutRefusedAt)
			{
				*OutRefusedAt = RefusedAt;
			}
			return false;
		}
		return true;
	}
	default:
		return true;
	}
}

bool UPenguinAbilityComponent::DoFishHook(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	const FVector Dir = AimDirection(AimPoint);
	Penguin->PlayAction(Penguin->PickNearFlipper(EPenguinClip::Toss, EPenguinClip::TossL), 1.6f);
	APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::FishHook, Penguin, Penguin->GetAimOrigin() + Dir * 45.f,
		Dir * APenguinProjectile::HookSpeed);
	APinguFX::Sound(this, EPinguSound::HookThrow);
	return true;
}

void UPenguinAbilityComponent::GetBombThrow(const FVector& AimOrigin, const FVector& AimPoint, float Facing, FVector& OutSpawn, FVector& OutVelocity, bool& bOutDropAtFeet)
{
	using namespace PenguinAbilitiesPrivate;
	const FVector Origin = AimOrigin + FVector(0.f, 0.f, 25.f);
	const float Dx = AimPoint.X - Origin.X;
	const float Dz = AimPoint.Z - Origin.Z;
	const float Dist = FMath::Abs(Dx);
	// Aimed straight down at your own feet: the bomb is dropped right there on a short fuse (and your own bomb knocks you out).
	bOutDropAtFeet = Dist < 40.f && Dz < 0.f;
	const float V = BombSpeed;
	const float G = BombGravity;
	float Angle = FMath::DegreesToRadians(45.f);
	const float Disc = V * V * V * V - G * (G * Dx * Dx + 2.f * Dz * V * V);
	if (Dist < 40.f)
	{
		Angle = FMath::DegreesToRadians(80.f);
	}
	else if (Disc >= 0.f)
	{
		Angle = FMath::Atan((V * V - FMath::Sqrt(Disc)) / (G * Dist)); // the low arc
	}
	const float Side = Dist < 40.f ? Facing : FMath::Sign(Dx);
	OutVelocity = FVector(Side * FMath::Cos(Angle) * V, 0.f, FMath::Sin(Angle) * V);
	OutSpawn = Origin + FVector(Side * 30.f, 0.f, 0.f);
	if (bOutDropAtFeet)
	{
		// It falls through its thrower (a bomb flies through its own thrower) and lands under the feet.
		OutVelocity = FVector(0.f, 0.f, -200.f);
		OutSpawn = AimOrigin;
	}
}

bool UPenguinAbilityComponent::DoSnowBomb(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	FVector SpawnAt;
	FVector Velocity;
	bool bDropAtFeet = false;
	GetBombThrow(Penguin->GetAimOrigin(), AimPoint, Penguin->GetFacing(), SpawnAt, Velocity, bDropAtFeet);
	Penguin->PlayAction(Penguin->PickNearFlipper(EPenguinClip::ThrowUp, EPenguinClip::ThrowUpL), 1.6f);
	APenguinProjectile* Bomb = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, Penguin, SpawnAt, Velocity);
	if (Bomb && bDropAtFeet)
	{
		Bomb->Detonate(0.6f);
	}
	APinguFX::Sound(this, EPinguSound::BombThrow);
	return true;
}

// ---------------------------------------------------------------------------- stomp

bool UPenguinAbilityComponent::DoFlap()
{
	// The movement does the climb (UPenguinMovementComponent::StartFlap: the penguin's own machine flies it; a LAN host's
	// move judge knows of it). Here: the sound and a puff of snow off the flippers.
	APenguinCharacter* Penguin = GetPenguin();
	UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	if (!Move || !Move->StartFlap())
	{
		return false;
	}
	APinguFX::Sound(this, EPinguSound::Flap);
	// (the wing beats' own wind replaces the old puff of snow: APinguFX::FlapWind, see APenguinCharacter::UpdateFlapWind)
	return true;
}

bool UPenguinAbilityComponent::DoBellyStomp()
{
	APenguinCharacter* Penguin = GetPenguin();
	UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
	if (!Move->StartStomp())
	{
		return false;
	}
	bStompArmed = true;
	StompStrength = UseStrength; // the slam comes later: it keeps this use's strength
	StompArmedTime = GetServerNow();
	StompLandsAtArm = Move->StompLandEvents;
	bStompRemote = !IsMovementSimulatedHere();
	APinguFX::Sound(this, EPinguSound::StompWhoosh);
	APinguFX::Puffs(this, Penguin->GetActorLocation(), 5, 22.f, 180.f, FVector::UpVector, 1.f);
	// Wind-up: it hangs for a moment first - mark where it will come down.
	if (const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation()))
	{
		float GroundZ = 0.f;
		if (Island->FindGroundBelow(Penguin->GetFeetLocation(), 2500.f, GroundZ))
		{
			APinguFX::Ring(this, FVector(Penguin->GetActorLocation().X, 0.f, GroundZ + 20.f), 70.f * Penguin->GetSizeScale(), FLinearColor(1.f, 0.55f, 0.2f), 0.45f);
		}
	}
	return true;
}

void UPenguinAbilityComponent::TickStomp()
{
	APenguinCharacter* Penguin = GetPenguin();
	const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	if (!bStompArmed || !Move)
	{
		return;
	}
	if (Move->StompLandEvents != StompLandsAtArm)
	{
		// It landed and the character has not told us yet (a LAN player's landing can show up here first): slam now.
		OnStompLanded();
		return;
	}
	if (!Move->IsStomping())
	{
		// A LAN player's own machine starts the stomp: until its state comes back, this copy still shows the penguin
		// from before it (standing, not stomping) - that is no interruption.
		const bool bStillOnItsWay = bStompRemote && GetServerNow() - StompArmedTime < PenguinAbilitiesPrivate::StompRemoteGrace;
		if (!bStillOnItsWay && (Move->IsMovingOnGround() || Move->IsStunned() || Move->IsFrozen() || Move->IsKnockedOut()))
		{
			bStompArmed = false; // interrupted
		}
		return;
	}
	// Squash anyone right below on the way down: our capsule against theirs (bigger ones are easier to land on).
	if (Move->Velocity.Z < -500.f)
	{
		const FVector Self = Penguin->GetActorLocation();
		const UCapsuleComponent* SelfCapsule = Penguin->GetCapsuleComponent();
		const float SelfRadius = SelfCapsule->GetScaledCapsuleRadius();
		const float SelfHalfHeight = SelfCapsule->GetScaledCapsuleHalfHeight();
		PenguinAbilitiesPrivate::ForEachOpponent(GetWorld(), Penguin, [&](APenguinCharacter* Other)
		{
			const UCapsuleComponent* OtherCapsule = Other->GetCapsuleComponent();
			const FVector D = Other->GetActorLocation() - Self;
			// Two normal penguins: |D.X| < 62, D.Z in (-125, 0).
			if (FMath::Abs(D.X) < SelfRadius + OtherCapsule->GetScaledCapsuleRadius() + 2.f
				&& D.Z < 0.f && D.Z > -(SelfHalfHeight + OtherCapsule->GetScaledCapsuleHalfHeight() + 13.f))
			{
				FPenguinHit Hit;
				Hit.Instigator = Penguin;
				Hit.Source = EPenguinAbility::BellyStomp;
				Hit.bDeadly = true;
				Hit.Reason = EPenguinKOReason::Stomp;
				Hit.Knockback = FVector2D(FMath::Sign(D.X + 0.01f) * 600.f, 900.f);
				Hit.Location = Other->GetActorLocation();
				FPinguFXBroadcastScope Broadcast(this);
				Other->ApplyPenguinHit(Hit);
			}
		});
	}
}

void UPenguinAbilityComponent::OnStompLanded()
{
	APenguinCharacter* Penguin = GetPenguin();
	// The character calls this on every machine when its stomp lands; the slam is server gameplay.
	if (!HasGameplayAuthority() || !Penguin || !bStompArmed)
	{
		return;
	}
	bStompArmed = false;
	FPinguFXBroadcastScope Broadcast(this);
	const FVector Feet = Penguin->GetFeetLocation();
	const float Size = Penguin->GetSizeScale();
	// The slam's reach and the shockwave's radius grow with the stomp's strength (Traits mode; 1 normally), its push too.
	const float Reach = Size * PinguTraitTuning::GetReachScale(StompStrength);
	bool bHitSomeone = false;
	PenguinAbilitiesPrivate::ForEachOpponent(GetWorld(), Penguin, [&](APenguinCharacter* Other)
	{
		using namespace PenguinAbilitiesPrivate;
		const FVector D = Other->GetFeetLocation() - Feet;
		const float OtherRadius = Other->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FPenguinHit Hit;
		Hit.Instigator = Penguin;
		Hit.Source = EPenguinAbility::BellyStomp;
		Hit.Location = Other->GetActorLocation();
		// The slam: on about the same floor, the capsule within reach. The shockwave: the capsule in the ring.
		if (FMath::Abs(D.X) < StompSlamReach * Reach - NormalPenguinRadius + OtherRadius && FMath::Abs(D.Z) < StompSlamHeight * Size)
		{
			Hit.bDeadly = true;
			Hit.Reason = EPenguinKOReason::Stomp;
			Hit.Knockback = FVector2D(FMath::Sign(D.X + 0.01f) * StompSlamPush, StompSlamLift);
		}
		else if (Other->DistanceTo(Feet) < StompShockRadius * Reach - NormalPenguinRadius)
		{
			Hit.Knockback = FVector2D(FMath::Sign(D.X + 0.01f) * StompShockPush, StompShockLift);
			Hit.Stun = StompShockStun;
			Hit.Strength = StompStrength;
		}
		else
		{
			return;
		}
		bHitSomeone |= Other->ApplyPenguinHit(Hit);
	});
	APenguinProjectile::PushInArea(GetWorld(), Penguin, Feet, PinguTuning::StompShockRadius * Reach, FVector::UpVector, -2.f);
	// The slam's shockwave knocks a snowman over.
	APinguSnowman::BreakInArea(GetWorld(), Feet, PinguTuning::StompShockRadius * Reach, Penguin, TEXT("belly stomp"));
	APinguPickup::PushInArea(GetWorld(), Feet, PinguTuning::StompShockRadius * Reach, PenguinAbilitiesPrivate::PickupPush);
	// The slam smashes a crater into the island (the block below and the ones beside it).
	if (ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Feet))
	{
		TArray<FVector> Removed;
		Island->RemoveBlocksInRadius(Feet - FVector(0.f, 0.f, Island->CellSize * 0.5f), PinguTuning::StompCraterRadius * Size, &Removed);
		for (const FVector& Where : Removed)
		{
			APinguFX::Shards(this, Where, 3, 600.f, FLinearColor(0.8f, 0.88f, 0.98f));
		}
		if (Removed.Num() > 0)
		{
			APinguFX::Sound(this, EPinguSound::Crumble, 0.8f);
		}
	}
	APinguFX::GroundPuffs(this, Feet, 2.f * Size);
	APinguFX::Ring(this, Feet + FVector(0.f, 0.f, 30.f), 300.f * Size, FLinearColor::White, 0.35f);
	APinguFX::Shards(this, Feet, 6, 700.f, FLinearColor(0.85f, 0.93f, 1.f));
	APinguFX::Shake(this, 0.55f);
	APinguFX::Sound(this, EPinguSound::StompImpact);
	if (bHitSomeone)
	{
		APinguFX::HitStop(this, 0.06f);
	}
	// Movement: bounce off the slam (high while jump is held) with the double jump and air dash back.
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		const float Bounce = Move->IsJumpHeld() ? PenguinAbilitiesPrivate::StompBounceHeld : PenguinAbilitiesPrivate::StompBounceTap;
		Move->AddAbilityImpulse(FVector2D(0.f, Bounce));
		Move->RefreshAirMoves();
		APinguFX::Puffs(this, Feet, 6, 26.f, 260.f, FVector::UpVector, 1.f);
	}
}

// ---------------------------------------------------------------------------- iceberg

bool UPenguinAbilityComponent::DoIcebergDrop(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	const FVector Target = GetIcebergTarget(AimPoint, UseStrength);
	const float TargetX = static_cast<float>(Target.X);
	const float TargetZ = static_cast<float>(Target.Z);
	// Its ice past the build limit (a use stronger than the pre-check's: Traits mode): refused, nothing spent.
	FVector LandedAt;
	if (!AIcebergDrop::IsDropAllowed(GetWorld(), Penguin, TargetX, TargetZ, 0.f, &LandedAt))
	{
		PinguBuildLimit::ShowRefused(this, ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation()), LandedAt,
			*FString::Printf(TEXT("%s's Iceberg Drop (past the build limit)"), *Penguin->GetDisplayName()));
		return false;
	}
	if (!AIcebergDrop::Drop(GetWorld(), Penguin, TargetX, TargetZ))
	{
		return false;
	}
	Penguin->PlayAction(EPenguinClip::Summon, 1.5f);
	APinguFX::Sound(this, EPinguSound::IcebergSummon);
	APinguFX::Sparkles(this, Penguin->GetActorLocation() + FVector(0.f, 0.f, 90.f), 8, 50.f, FLinearColor(0.6f, 0.9f, 1.f));
	return true;
}

// ---------------------------------------------------------------------------- slap

namespace PenguinAbilitiesPrivate
{
	/** The flipper's swing: a glowing arc in front of the body (toward the camera), from above the aim down past it. */
	static void SlapSwoosh(const UObject* Ctx, const FVector& Origin, const FVector& Dir, float Size, const FLinearColor& Color)
	{
		const float Aim = FMath::Atan2(static_cast<float>(Dir.Z), static_cast<float>(Dir.X));
		const float Half = FMath::DegreesToRadians(SlapSwooshSweepDeg) * 0.5f;
		// Top to bottom on either side (facing left the angles run the other way round).
		const float Turn = Dir.X >= 0.0 ? 1.f : -1.f;
		const FVector Front(0.f, 45.f, 0.f);
		const float Radius = SlapSwooshRadius * Size;
		const auto Point = [&](float T)
		{
			const float Angle = Aim + Turn * Half - Turn * 2.f * Half * T;
			return Origin + Front + FVector(FMath::Cos(Angle), 0.f, FMath::Sin(Angle)) * Radius;
		};
		for (int32 Piece = 0; Piece < SlapSwooshPieces; ++Piece)
		{
			const float Lead = static_cast<float>(Piece + 1) / static_cast<float>(SlapSwooshPieces);
			APinguFX::GlowTrail(Ctx, Point(static_cast<float>(Piece) / static_cast<float>(SlapSwooshPieces)), Point(Lead),
				SlapSwooshWidth * Size * (0.4f + 0.6f * Lead), Color, SlapSwooshOpacity, SlapSwooshLife * (0.5f + 0.5f * Lead));
		}
	}

	/** A hit: a gold ring bursts out of the one slapped, a big star flashes where the flipper met it, stars and puffs fly. */
	static void SlapImpact(const UObject* Ctx, const APenguinCharacter* Other, const FVector& Dir, float Size)
	{
		const FVector Body = Other->GetActorLocation();
		const FVector Contact = Body - Dir * 30.f * Size;
		APinguFX::Ring(Ctx, Other->GetAimOrigin(), 95.f * Other->GetSizeScale(), FLinearColor(1.f, 0.82f, 0.3f), 0.3f);
		APinguFX::Stars(Ctx, Body, 8, 560.f);
		APinguFX::Puffs(Ctx, Contact, 6, 26.f, 380.f, Dir, 0.5f);
		if (APinguFX* FX = APinguFX::Get(Ctx))
		{
			FPinguParticleDesc Flash;
			Flash.Mesh = EPinguFXMesh::Star;
			Flash.Location = Contact + FVector(0.f, 60.f, 0.f);
			Flash.Size = 96.f * Size;
			Flash.EndSize = 0.f;
			Flash.Curve = EPinguSizeCurve::Pop;
			Flash.Life = 0.2f;
			Flash.Emissive = 3.f;
			Flash.Color = FLinearColor(1.f, 0.9f, 0.45f);
			Flash.Spin = FVector(0.f, 0.f, 260.f);
			FX->Spawn(Flash);
		}
	}
}

bool UPenguinAbilityComponent::DoFlipperSlap(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	const FVector Dir = AimDirection(AimPoint);
	Penguin->PlayAction(Penguin->PickNearFlipper(EPenguinClip::Swat, EPenguinClip::SwatL), 2.f);
	const float Size = Penguin->GetSizeScale();
	// The flipper reaches further (or less far) with the use's strength (Traits mode; 1 normally); its knock: Hit.Strength.
	const float Reach = Size * PinguTraitTuning::GetReachScale(UseStrength);
	const FVector Center = Penguin->GetAimOrigin() + Dir * PinguTuning::SlapReachAhead * Reach;

	// Not through a wall (the ice block in front of the flipper breaks below, after the penguins).
	const ABlockIsland* Walls = ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation());
	bool bHit = false;
	PenguinAbilitiesPrivate::ForEachOpponent(GetWorld(), Penguin, [&](APenguinCharacter* Other)
	{
		if (Other->DistanceTo(Center) > PinguTuning::SlapHitRadius * Reach || !HasClearLine(Walls, Penguin, Other))
		{
			return;
		}
		FPenguinHit Hit;
		Hit.Instigator = Penguin;
		Hit.Source = EPenguinAbility::FlipperSlap;
		Hit.Strength = UseStrength;
		Hit.Knockback = FVector2D(Dir.X * PenguinAbilitiesPrivate::SlapPush, FMath::Max(Dir.Z * PenguinAbilitiesPrivate::SlapPush, 0.f)
			+ PenguinAbilitiesPrivate::SlapLift);
		Hit.Stun = PenguinAbilitiesPrivate::SlapStun;
		Hit.Location = Other->GetActorLocation();
		if (Other->ApplyPenguinHit(Hit))
		{
			bHit = true;
			PenguinAbilitiesPrivate::SlapImpact(this, Other, Dir, Size);
		}
	});

	TArray<APenguinProjectile*> Projectiles;
	APenguinProjectile::GetAll(GetWorld(), Projectiles);
	for (APenguinProjectile* Projectile : Projectiles)
	{
		if (Projectile->GetShooter() != Penguin && FVector::Dist(Projectile->GetActorLocation(), Center) < PinguTuning::SlapBatRadius * Reach)
		{
			if (Projectile->IsFlying())
			{
				bHit |= Projectile->Reflect(Penguin, Dir);
			}
			else
			{
				Projectile->Push(Penguin, Dir); // knocks icicle pegs out of the wall
			}
		}
	}

	// A giant snowball the flipper reaches breaks (it can't be pushed: the slap smashes it).
	if (APinguGiantSnowball::BurstInArea(GetWorld(), Center, PinguTuning::SlapHitRadius * Reach, Penguin, TEXT("flipper slap")) > 0)
	{
		bHit = true;
	}
	// A snowman the flipper reaches crumbles (as an ice block does).
	if (APinguSnowman::BreakInArea(GetWorld(), Center, PinguTuning::SlapHitRadius * Reach, Penguin, TEXT("flipper slap")) > 0)
	{
		bHit = true;
	}

	// Ability bubbles in front of the flipper get batted along too.
	APinguPickup::PushInArea(GetWorld(), Penguin->GetAimOrigin(), (PinguTuning::SlapReachAhead + PinguTuning::SlapBatRadius) * Reach,
		PenguinAbilitiesPrivate::PickupPush, Dir,
		FMath::Cos(FMath::DegreesToRadians(60.f)));

	// ... and the ice block in front of it breaks (aim at the floor and it is the floor).
	if (ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Center))
	{
		bHit |= Island->BreakIceInRadius(Center, PenguinAbilitiesPrivate::SlapIceRadius * Size, 1, Dir) > 0;
	}

	// Every slap shows: the swoosh of the swing (warm on a hit), a puff burst and a small ring where the flipper went.
	PenguinAbilitiesPrivate::SlapSwoosh(this, Penguin->GetAimOrigin(), Dir, Size, bHit ? FLinearColor(1.9f, 1.7f, 1.2f) : FLinearColor(1.5f, 1.65f, 1.9f));
	APinguFX::Puffs(this, Center, 6, 24.f, 320.f, Dir, 0.6f);
	if (!bHit)
	{
		APinguFX::Ring(this, Center, 55.f * Size, FLinearColor(0.8f, 0.92f, 1.f), 0.22f);
	}

	// Movement: the slap lunges you along the aim (full strength once per air time, always on the ground).
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		using namespace PenguinAbilitiesPrivate;
		const bool bAir = !Move->IsGroundedForAbilities();
		const float Strength = bAir && bAirLungeUsed ? 0.35f : 1.f;
		const float DirZ = static_cast<float>(Dir.Z);
		const float Up = DirZ > 0.f ? DirZ * SlapLungeUp : (bAir ? DirZ * SlapLungeUp * 0.5f : 0.f);
		const float Side = static_cast<float>(Dir.X) * SlapLunge;
		Move->AddAbilityImpulse(FVector2D(Side, Up) * Strength);
		if (bAir)
		{
			bAirLungeUsed = true;
		}
		APinguFX::Trail(this, Penguin->GetActorLocation(), 18.f);
	}

	if (bHit)
	{
		APinguFX::Sound(this, EPinguSound::Slap);
		APinguFX::HitStop(this, 0.06f);
		APinguFX::Shake(this, 0.25f);
	}
	else
	{
		APinguFX::Sound(this, EPinguSound::Throw, 0.6f, 0.1f, 1.3f);
	}
	return true;
}

// ---------------------------------------------------------------------------- frost

bool UPenguinAbilityComponent::DoFrostBreath(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	FrostDirection = AimDirection(AimPoint);
	Penguin->PlayAction(EPenguinClip::Cast, 1.5f);
	FrostLeft = PinguTuning::FrostBreathTime;
	FrostEndTime = GetServerNow() + FrostLeft; // every machine blows its own frost cloud until then
	FrostStrength = UseStrength; // the breath blows on after this use: it keeps its strength (range, freeze time)
	FrostFxTimer = 0.f;
	FrostVictims.Reset();
	FrostShellBlocks.Reset();
	APinguFX::Sound(this, EPinguSound::Frost);
	// Everything interacts: the snow in front of you freezes into slippery ice.
	if (ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation()))
	{
		const float Size = Penguin->GetSizeScale() * PinguTraitTuning::GetReachScale(FrostStrength);
		int32 FrozenTotal = 0;
		for (const float Along : { 130.f, 260.f, 390.f })
		{
			const FVector Point = Penguin->GetAimOrigin() + FrostDirection * Along * Size;
			float GroundZ = 0.f;
			if (Island->FindGroundBelow(Point + FVector(0.f, 0.f, 40.f), 260.f * Size, GroundZ))
			{
				FrozenTotal += Island->FreezeBlocksInRadius(FVector(Point.X, 0.f, GroundZ - Island->CellSize * 0.5f), 80.f * Size, false);
			}
		}
		if (FrozenTotal > 0)
		{
			Island->RebuildAfterEdits(); // once for the whole breath, not once per point
		}
	}
	return true;
}

void UPenguinAbilityComponent::TickFrost(float DeltaTime)
{
	if (FrostLeft <= 0.f)
	{
		return;
	}
	APenguinCharacter* Penguin = GetPenguin();
	const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	// Knocked out, frozen, dizzy from a slap or locked in place: the breath ends there. An ordinary hit doesn't end it - it
	// blows on through the knockback flight (as it can be started in one).
	if (!Penguin || !Penguin->IsAlive() || !Move || Move->IsFrozen() || Move->IsSlapDizzy() || Move->IsInputLocked())
	{
		FrostLeft = 0.f;
		FrostEndTime = 0.0; // clients stop their frost cloud too
		return;
	}
	FrostLeft -= DeltaTime;

	// Movement: the frost jet. A LAN player's own machine flies it (it moves that penguin, every frame, from the
	// replicated breath); the server does it for its own penguins.
	if (IsMovementSimulatedHere())
	{
		TickFrostJet(DeltaTime);
	}

	const FVector Origin = Penguin->GetAimOrigin() + FrostDirection * 35.f;
	const float Range = PinguTuning::FrostRange * Penguin->GetSizeScale() * PinguTraitTuning::GetReachScale(FrostStrength);
	const float CosHalfAngle = FMath::Cos(FMath::DegreesToRadians(PinguTuning::FrostConeDeg));
	{
		FPinguFXBroadcastScope FreezeBroadcast(this);
		APenguinProjectile::FreezeInArea(GetWorld(), Penguin, Origin, Range, FrostDirection, CosHalfAngle, PinguTuning::FrostShotFreeze);
		// A giant snowball in the frost freezes for good (it swallows nothing more and slides faster).
		APinguGiantSnowball::FreezeInArea(GetWorld(), Origin, Range, FrostDirection, CosHalfAngle);
		// A snowman in the frost cracks and crumbles (frost breaks it as the island's ice: "die from like everything").
		APinguSnowman::BreakInArea(GetWorld(), Origin, Range, Penguin, TEXT("frost breath"), FrostDirection, CosHalfAngle);
	}

	// The cloud is ours to see only: every LAN machine blows its own (no stream of effects over the network).
	TickFrostCloud(DeltaTime);

	const ABlockIsland* Walls = ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation());
	FPinguFXBroadcastScope HitBroadcast(this);
	PenguinAbilitiesPrivate::ForEachOpponent(GetWorld(), Penguin, [&](APenguinCharacter* Other)
	{
		// Range reaches the capsule's surface (a normal penguin's centre may be 30 cm further away). A wall shields it (it
		// may still come into view later in the breath).
		if (FrostVictims.Contains(Other) || !PenguinAbilitiesPrivate::IsInCone(Other, Origin, FrostDirection, Range, CosHalfAngle)
			|| !HasClearLine(Walls, Penguin, Other))
		{
			return;
		}
		const bool bShelled = Other->IsShielded();
		if (bShelled && FrostShellBlocks.Contains(Other))
		{
			return; // its shell held already: no block effects every frame (it is frozen if the shell pops in time)
		}
		FPenguinHit Hit;
		Hit.Instigator = Penguin;
		Hit.Source = EPenguinAbility::FrostBreath;
		Hit.Freeze = PinguTuning::FrostFreezeTime * PinguTraitTuning::GetDurationScale(FrostStrength);
		Hit.Stun = 0.f;
		Hit.Knockback = FVector2D(FrostDirection.X * PinguTuning::FrostPush, 0.f);
		Hit.Location = Other->GetActorLocation();
		// Only a penguin that was really frozen is done with for this breath.
		if (Other->ApplyPenguinHit(Hit))
		{
			FrostVictims.Add(Other);
		}
		else if (bShelled)
		{
			FrostShellBlocks.Add(Other);
		}
	});
}

void UPenguinAbilityComponent::TickFrostJet(float DeltaTime)
{
	// The frost blast pushes you away from the cursor (aim down to fly up, sideways to glide).
	APenguinCharacter* Penguin = GetPenguin();
	UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	// It pushes in a knockback flight too (the ability rule: CanUseAbilities), not frozen, knocked out, locked or stomping.
	if (!Move || !Move->CanUseAbilities())
	{
		return;
	}
	using namespace PenguinAbilitiesPrivate;
	const FVector Push = -FrostDirection;
	const float Along = static_cast<float>(FVector::DotProduct(Move->Velocity, Push));
	if (Along < FrostJetSpeed)
	{
		const float Add = FMath::Min(FrostJetAccel * DeltaTime, FrostJetSpeed - Along);
		const bool bLifting = Push.Z > 0.3f;
		if (bLifting || !Move->IsGroundedForAbilities())
		{
			// Low gravity that outlasts even a long frame, so the jet height does not depend on the frame rate.
			Move->AddAbilityImpulse(FVector2D(Push.X, Push.Z) * Add, FMath::Max(0.06f, DeltaTime * 1.5f + 0.02f), false);
		}
	}
}

void UPenguinAbilityComponent::TickFrostCloud(float DeltaTime)
{
	FrostFxTimer -= DeltaTime;
	if (FrostFxTimer > 0.f)
	{
		return;
	}
	FrostFxTimer = 0.025f;
	const APenguinCharacter* Penguin = GetPenguin();
	APinguFX* FX = Penguin ? APinguFX::Get(this) : nullptr;
	if (!FX)
	{
		return;
	}
	const FVector Origin = Penguin->GetAimOrigin() + FrostDirection * 35.f;
	// Three bits of frost every 25 ms with full effects, fewer on low settings (a flake first, then puffs).
	const int32 Count = FMath::Max(1, FMath::RoundToInt(3.f * UPinguSettings::GetFXDensity()));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Spread = FMath::FRandRange(-0.45f, 0.45f);
		const FVector Dir(FrostDirection.X * FMath::Cos(Spread) - FrostDirection.Z * FMath::Sin(Spread), FMath::FRandRange(-0.2f, 0.2f),
			FrostDirection.X * FMath::Sin(Spread) + FrostDirection.Z * FMath::Cos(Spread));
		FPinguParticleDesc D;
		D.Mesh = Index == 0 ? EPinguFXMesh::Flake : EPinguFXMesh::Puff;
		D.Location = Origin;
		D.Velocity = Dir * FMath::FRandRange(700.f, 1000.f);
		D.Size = Index == 0 ? FMath::FRandRange(12.f, 20.f) : FMath::FRandRange(18.f, 34.f);
		D.Curve = EPinguSizeCurve::Pop;
		D.Life = FMath::FRandRange(0.35f, 0.5f);
		D.Drag = 3.2f;
		D.Emissive = 0.5f;
		D.Color = FLinearColor(0.7f, 0.93f, 1.f);
		D.Spin = FVector(0.f, 0.f, FMath::FRandRange(-360.f, 360.f));
		FX->Spawn(D);
	}
}

// ---------------------------------------------------------------------------- ice wall

bool UPenguinAbilityComponent::PlanIceWall(const FVector& AimPoint, float Strength, ABlockIsland*& OutIsland, TArray<FIntPoint>& OutCells,
	TArray<FIntPoint>& OutBigOrigins, FVector& OutTarget) const
{
	const APenguinCharacter* Penguin = GetPenguin();
	ABlockIsland* Island = Penguin ? ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation()) : nullptr;
	OutIsland = Island;
	OutCells.Reset();
	OutBigOrigins.Reset();
	if (!Island)
	{
		return false;
	}
	const FVector Self = Penguin->GetActorLocation();
	FVector Target = AimPoint;
	Target.Y = 0.f;
	FVector Offset = Target - Self;
	if (Offset.Size() > PinguTuning::IceWallRange)
	{
		Target = Self + Offset.GetSafeNormal() * PinguTuning::IceWallRange;
	}
	const FIntPoint Cell = Island->WorldToCell(Target);

	// How many blocks: more (or fewer) with the use's strength (Traits mode: 2 to 5; 3 normally). A grown penguin's blocks
	// are bigger (PinguTuning::GetIceScale): from 2x the wall is two cells thick, from 4x it is made of big blocks (2x2 cells
	// each) - as many of them as it would have blocks.
	const int32 NumBlocks = FMath::Clamp(FMath::RoundToInt(PinguTuning::IceWallBlocks * Strength), 2, 5);
	const PinguTuning::FPinguIceScale IceScale = PinguTuning::GetIceScale(Penguin->GetSizeScale());
	const int32 Span = FMath::Max(IceScale.Span, 1);
	const int32 Across = FMath::Max(IceScale.WallCellsAcross, 1);
	const int32 Along = NumBlocks * Span;
	const bool bGrown = Span > 1 || Across > 1;
	// Grown: the cells nearest the aim across the wall (two columns of a wall: the pair whose border is nearest it).
	const FVector IslandOrigin = Island->GetActorLocation();
	const float AimCellX = static_cast<float>(Target.X - IslandOrigin.X) / Island->CellSize;
	const float AimCellZ = static_cast<float>(Target.Z - IslandOrigin.Z) / Island->CellSize;
	const auto Nearest = [](float AimAt, int32 NumCells) { return FMath::FloorToInt(AimAt - NumCells * 0.5f + 0.5f); };

	// The ground under the wall: the first block at or under the aim within 6 rows (in every column it covers).
	const int32 FirstColumn = bGrown ? Nearest(AimCellX, Across) : Cell.X;
	int32 Base = TNumericLimits<int32>::Lowest();
	for (int32 Column = FirstColumn; Column < FirstColumn + Across; ++Column)
	{
		for (int32 Down = 0; Down <= 6; ++Down)
		{
			if (Island->IsSolidCell(FIntPoint(Column, Cell.Y - Down)))
			{
				// Up from there to the first free cell: the wall stands on top of what is there (the highest of its columns).
				const int32 GroundZ = Cell.Y - Down;
				int32 Z = GroundZ + 1;
				while (Island->IsSolidCell(FIntPoint(Column, Z)) && Z < GroundZ + 12)
				{
					++Z;
				}
				Base = FMath::Max(Base, Z);
				break;
			}
		}
	}
	if (Base != TNumericLimits<int32>::Lowest())
	{
		// A column (Across cells wide) Along cells high on the ground.
		for (int32 Row = 0; Row < Along; ++Row)
		{
			for (int32 Dx = 0; Dx < Across; ++Dx)
			{
				OutCells.Add(FIntPoint(FirstColumn + Dx, Base + Row));
			}
		}
		for (int32 Row = 0; Span > 1 && Row + Span <= Along; Row += Span)
		{
			OutBigOrigins.Add(FIntPoint(FirstColumn, Base + Row));
		}
	}
	else
	{
		// Nothing below: a little ledge in the air (handy to get back to the island), as wide as the wall would be high (and
		// as thick).
		const int32 Left = bGrown ? Nearest(AimCellX, Along) : Cell.X - (NumBlocks - 1) / 2;
		const int32 Bottom = bGrown ? Nearest(AimCellZ, Across) : Cell.Y;
		for (int32 Dz = 0; Dz < Across; ++Dz)
		{
			for (int32 Dx = 0; Dx < Along; ++Dx)
			{
				OutCells.Add(FIntPoint(Left + Dx, Bottom + Dz));
			}
		}
		for (int32 Dx = 0; Span > 1 && Dx + Span <= Along; Dx += Span)
		{
			OutBigOrigins.Add(FIntPoint(Left + Dx, Bottom));
		}
	}

	OutTarget = Target;
	return OutCells.Num() > 0;
}

bool UPenguinAbilityComponent::DoIceWall(const FVector& AimPoint)
{
	APenguinCharacter* Penguin = GetPenguin();
	ABlockIsland* Island = nullptr;
	TArray<FIntPoint> NewCells;
	TArray<FIntPoint> BigOrigins;
	FVector Target;
	if (PendingWallLeft > 0.f || !PlanIceWall(AimPoint, UseStrength, Island, NewCells, BigOrigins, Target))
	{
		return false;
	}
	// Past the build limit (a use stronger than the pre-check's: Traits mode): refused, nothing spent.
	if (!PinguBuildLimit::AllowsCells(Island, NewCells))
	{
		PinguBuildLimit::ShowRefused(this, Island, Target, *FString::Printf(TEXT("%s's Ice Wall (past the build limit)"), *Penguin->GetDisplayName()));
		return false;
	}

	// Lasting ice (the island is regenerated every round): it stays until something breaks it, block by block. Solid at once:
	// its collision is there this very frame (the boxes are rebuilt now, not at the end of the frame), whoever stands there is
	// moved out of it (onto it, or aside), shots in it break on it; its look rises into place while the spot's rings show.
	const TArray<FIntPoint> Added = Island->AddBlocks(NewCells, EBlockKind::Ice, 0.f, &BigOrigins);
	if (Added.Num() == 0)
	{
		return false;
	}
	Island->RiseBlocks(Added, PenguinAbilitiesPrivate::IceWallDelay);
	Island->FlushEdits();
	Island->PushPenguinsOutOfBlocks(Added);
	Island->SmashProjectilesInCells(Added);

	// The build burst comes when the look has risen (RaiseIceWall).
	PendingWallCells = Added;
	PendingWallIsland = Island;
	PendingWallLeft = PenguinAbilitiesPrivate::IceWallDelay;
	for (const FIntPoint& C : Added)
	{
		const FVector Center = Island->CellToWorld(C);
		APinguFX::Sparkles(this, Center, 6, 45.f, FLinearColor(0.6f, 0.9f, 1.f));
		APinguFX::Ring(this, Center, 48.f, FLinearColor(0.55f, 0.85f, 1.f), PenguinAbilitiesPrivate::IceWallDelay);
	}
	Penguin->PlayAction(EPenguinClip::Cast, 1.8f);
	APinguFX::Sound(this, EPinguSound::Frost, 0.35f);
	return true;
}

void UPenguinAbilityComponent::RaiseIceWall()
{
	ABlockIsland* Island = PendingWallIsland.Get();
	const TArray<FIntPoint> WallCells = PendingWallCells;
	PendingWallCells.Reset();
	PendingWallIsland.Reset();
	if (!HasGameplayAuthority() || !Island || WallCells.Num() == 0 || !GetWorld())
	{
		return;
	}
	// The wall has risen into place (it was solid since the cast): the build burst where its blocks still stand.
	FPinguFXBroadcastScope Broadcast(this);
	int32 Standing = 0;
	for (const FIntPoint& C : WallCells)
	{
		if (Island->GetBlock(C.X, C.Y) != EBlockKind::Ice)
		{
			continue;
		}
		++Standing;
		const FVector Center = Island->CellToWorld(C);
		APinguFX::Puffs(this, Center, 3, 35.f, 220.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.95f, 1.f));
		APinguFX::Shards(this, Center, 2, 500.f, FLinearColor(0.7f, 0.9f, 1.f));
	}
	if (Standing > 0)
	{
		APinguFX::Sound(this, EPinguSound::WallBuild);
		APinguFX::Shake(this, 0.15f);
	}
}

// ---------------------------------------------------------------------------- warp

bool UPenguinAbilityComponent::DoSnowWarp(const FVector& AimPoint)
{
	using namespace PenguinAbilitiesPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	UWorld* World = GetWorld();
	const FVector Start = Penguin->GetActorLocation();
	ABlockIsland* Island = ABlockIsland::FindNear(World, Start);
	TArray<FIntPoint> IceInWay;
	FVector Destination = Start;
	bool bFound = false;

	// Aimed at one of our own shots: swap places with it (it flies on from where we were).
	APenguinProjectile* SwapShot = nullptr;
	{
		TArray<APenguinProjectile*> Projectiles;
		APenguinProjectile::GetAll(World, Projectiles);
		float BestDist = WarpSwapAimRadius * FMath::Max(Penguin->GetSizeScale(), 1.f);
		for (APenguinProjectile* Shot : Projectiles)
		{
			const FVector ShotAt = Shot->GetActorLocation();
			const float AimDist = static_cast<float>(FVector::Dist(FVector(ShotAt.X, 0.f, ShotAt.Z), FVector(AimPoint.X, 0.f, AimPoint.Z)));
			if (Shot->GetShooter() != Penguin || !Shot->IsFlying() || Shot->GetKind() == EPenguinProjectileKind::FishHook
				|| AimDist > BestDist || FVector::Dist(ShotAt, Start) > WarpSwapMaxRange || !APinguPortal::IsInWarpView(Penguin, ShotAt))
			{
				continue;
			}
			// Room for us there (a shot rolling on the ground: a little higher up)?
			const float HalfHeight = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			for (const float Lift : { 0.f, 0.5f, 1.f })
			{
				const FVector Candidate(ShotAt.X, 0.f, ShotAt.Z + Lift * HalfHeight);
				if (WarpRoomAt(World, Penguin, Island, Candidate, IceInWay))
				{
					SwapShot = Shot;
					Destination = Candidate;
					BestDist = AimDist;
					break;
				}
			}
		}
		bFound = SwapShot != nullptr;
		if (bFound)
		{
			WarpRoomAt(World, Penguin, Island, Destination, IceInWay); // the ice in the way of the one we picked
		}
	}

	FVector Delta = Destination - Start;
	if (!bFound)
	{
		Delta = AimPoint - Start;
		Delta.Y = 0.f;
		// Further (or less far) with the use's strength (Traits mode; 1 normally).
		const float Range = WarpRange * PinguTraitTuning::GetReachScale(UseStrength);
		if (Delta.Size() > Range)
		{
			Delta = Delta.GetSafeNormal() * Range;
		}
		if (Delta.Size() < 60.f)
		{
			Delta = FVector(Penguin->GetFacing() * Range * 0.5f, 0.f, 0.f);
		}
		Delta = APinguPortal::ClampWarpMove(Penguin, Start, Delta); // the exit stays on the warper's screen (16:9 at most)
		// The furthest spot toward the cursor where we fit - warping into ice shatters it (other blocks stop us short).
		// Aimed at the ground (or into it, or at a low step): the same spot a little higher up, standing on it. A free spot
		// there beats breaking the ice we would arrive in.
		const float HalfHeight = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		TArray<FIntPoint> IceSpotCells;
		for (float T = 1.f; T >= 0.15f && !bFound; T -= 0.085f)
		{
			bool bIceSpot = false;
			FVector IceSpot = FVector::ZeroVector;
			for (const float Lift : { 0.f, 0.5f, 1.f, 1.5f })
			{
				const FVector Candidate = Start + Delta * T + FVector(0.f, 0.f, Lift * HalfHeight);
				if (!WarpRoomAt(World, Penguin, Island, Candidate, IceInWay))
				{
					continue;
				}
				if (IceInWay.Num() == 0)
				{
					Destination = Candidate;
					bFound = true;
					break;
				}
				if (!bIceSpot)
				{
					bIceSpot = true;
					IceSpot = Candidate;
					IceSpotCells = IceInWay;
				}
			}
			if (!bFound && bIceSpot)
			{
				Destination = IceSpot;
				IceInWay = IceSpotCells;
				bFound = true;
			}
		}
	}
	if (!bFound)
	{
		return false;
	}
	if (IceInWay.Num() > 0 && Island)
	{
		// Warped into ice: it shatters around us. Its collision goes at once (we move in there this frame); a LAN
		// player's own island gets the change with the island's own replication.
		for (const FIntPoint& Cell : IceInWay)
		{
			const FVector CellCenter = Island->CellToWorld(Cell);
			FVector Out = CellCenter - Destination;
			Out.Y = 0.f;
			Island->BreakIceAt(CellCenter, Out.GetSafeNormal());
		}
		Island->FlushEdits();
	}

	APinguFX::Ghost(this, Penguin->GetMesh(), FLinearColor(0.7f, 0.5f, 1.f), 0.35f, 0.6f);
	APinguFX::Puffs(this, Start, 8, 30.f, 260.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.8f, 1.f));
	// Keeps a third of the speed; in the air it stays falling. NetTeleport also moves a LAN player's penguin on its own
	// machine (which owns its position).
	UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
	const FVector WarpVelocity = Move ? FVector(Move->Velocity * WarpKeepSpeed) : FVector::ZeroVector;
	Penguin->NetTeleport(Destination, WarpVelocity, Move && (!Move->IsMovingOnGround() || SwapShot != nullptr));
	if (Move)
	{
		Move->RefreshAirMoves();
	}
	if (SwapShot)
	{
		SwapShot->WarpTo(Start);
		APinguFX::Sparkles(this, Start, 8, 40.f, FLinearColor(0.75f, 0.55f, 1.f));
	}
	// A portal from where we were to where we are now (our old one closes): whatever goes into one end comes out of the
	// other - penguins, shots, bombs, ability bubbles.
	APinguPortal::Open(World, Penguin, Start, Destination);
	// Ability bubbles on the way are grabbed in passing (UseAbility hands them over once this use is booked: a bubble
	// may change the very slot the warp was in).
	TArray<APinguPickup*> Pickups;
	APinguPickup::GetAll(World, Pickups);
	const float Reach = Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
	for (APinguPickup* Pickup : Pickups)
	{
		const FVector Bubble = Pickup->GetIconLocation();
		if (Pickup->CanBeTaken() && FVector::Dist(FMath::ClosestPointOnSegment(Bubble, Start, Destination), Bubble) < Reach + Pickup->GetRadius())
		{
			WarpGrabs.Add(Pickup);
		}
	}
	APinguFX::Puffs(this, Destination, 10, 32.f, 300.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.8f, 1.f));
	APinguFX::Sparkles(this, Destination, 8, 45.f, FLinearColor(0.75f, 0.55f, 1.f));
	APinguFX::Sound(this, EPinguSound::Warp);
	Penguin->FaceDirection(Delta.X);
	Penguin->PlayAction(EPenguinClip::Spin, 2.2f);
	return true;
}

// ---------------------------------------------------------------------------- shell & gust

bool UPenguinAbilityComponent::DoIceShell()
{
	APenguinCharacter* Penguin = GetPenguin();
	ShieldLeft = PinguTuning::ShellDuration * PinguTraitTuning::GetDurationScale(UseStrength); // (Traits mode: longer or shorter)
	ShieldEndTime = GetServerNow() + ShieldLeft; // clients show the bubble until then
	Penguin->OnShieldChanged(true);
	// Movement: the bubble floats you down gently (and pops you up a little when used in the air).
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		Move->SetFloat(ShieldLeft);
		if (!Move->IsGroundedForAbilities())
		{
			Move->AddAbilityImpulse(FVector2D(0.f, PenguinAbilitiesPrivate::ShellPop));
		}
	}
	APinguFX::Sound(this, EPinguSound::ShieldOn);
	APinguFX::Ring(this, Penguin->GetAimOrigin(), ShieldRadius, FLinearColor(0.5f, 0.95f, 1.f), 0.3f);
	APinguFX::Sparkles(this, Penguin->GetAimOrigin(), 6, 70.f, FLinearColor(0.6f, 0.95f, 1.f));
	return true;
}

void UPenguinAbilityComponent::TickShield(float DeltaTime)
{
	if (ShieldLeft <= 0.f)
	{
		return;
	}
	ShieldLeft -= DeltaTime;
	APenguinCharacter* Penguin = GetPenguin();
	if (ShieldLeft <= 0.f || !Penguin || !Penguin->IsAlive())
	{
		ShieldLeft = 0.f;
		ShieldEndTime = 0.0; // also takes it down on the clients when it ends early (knocked out)
		if (Penguin)
		{
			Penguin->OnShieldChanged(false);
			if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
			{
				Move->SetFloat(0.f);
			}
		}
		return;
	}
	BounceOffShells(Penguin);
}

void UPenguinAbilityComponent::BounceOffShells(APenguinCharacter* Penguin)
{
	using namespace PenguinAbilitiesPrivate;
	const double Now = GetServerNow();
	ShellBounces.RemoveAll([Now](const FShellBounce& Bounce) { return !Bounce.Other.IsValid() || Now - Bounce.Time >= ShellBounceInterval; });
	ForEachOtherPenguin(GetWorld(), Penguin, [&](APenguinCharacter* Other)
	{
		// Both shells tick: the penguin with the lower id handles the pair (and remembers when it bounced last).
		if (!Other->IsShielded() || Other->GetUniqueID() < Penguin->GetUniqueID()
			|| ShellBounces.ContainsByPredicate([Other](const FShellBounce& Bounce) { return Bounce.Other.Get() == Other; }))
		{
			return;
		}
		const FVector Chest = Penguin->GetAimOrigin();
		FVector Gap = Other->GetAimOrigin() - Chest;
		Gap.Y = 0.f;
		const float Reach = Penguin->GetShieldRadius() + Other->GetShieldRadius();
		if (Gap.SizeSquared() >= FMath::Square(Reach))
		{
			return;
		}
		ShellBounces.Add({ Other, Now });
		// Apart along X (straight above one another: we back off against our facing) and up a little. The impulses reach a
		// LAN player's own machine.
		const float Side = FMath::Abs(Gap.X) > 1.f ? -FMath::Sign(static_cast<float>(Gap.X)) : -Penguin->GetFacing();
		if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
		{
			Move->AddAbilityImpulse(FVector2D(Side * ShellBounceSide, ShellBounceUp));
		}
		if (UPenguinMovementComponent* OtherMove = Other->GetPenguinMovement())
		{
			OtherMove->AddAbilityImpulse(FVector2D(-Side * ShellBounceSide, ShellBounceUp));
		}
		FPinguFXBroadcastScope Broadcast(this);
		const FLinearColor ShellColor(0.6f, 0.95f, 1.f);
		APinguFX::Sound(this, EPinguSound::ShieldBlock);
		APinguFX::Sparkles(this, Chest + Gap * (Penguin->GetShieldRadius() / FMath::Max(Reach, 1.f)), 8, 40.f, ShellColor);
		APinguFX::Ring(this, Chest, Penguin->GetShieldRadius(), ShellColor, 0.2f);
		APinguFX::Ring(this, Other->GetAimOrigin(), Other->GetShieldRadius(), ShellColor, 0.2f);
	});
}

bool UPenguinAbilityComponent::DoGust(const FVector& /*AimPoint*/)
{
	// The tornado (APinguTornado: its suck, its throw, its look): round our chest, as wide as our size and the use's strength
	// make it (Traits mode: reach x sqrt(strength), its push x strength). We hang perfectly still in its eye while it lasts -
	// in the air or on the ground, no other ability meanwhile (a knockback, a freeze or a knock-out ends that) - and get our
	// double jump and air dash back for when it is over.
	using namespace PenguinAbilitiesPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	if (!APinguTornado::Spawn(Penguin, UseStrength, PinguTraitTuning::GetReachScale(UseStrength)))
	{
		return false;
	}
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		Move->SetHover(TornadoTime);
		Move->RefreshAirMoves();
	}
	return true;
}

// ---------------------------------------------------------------------------- noot, ice gun, size rays

bool UPenguinAbilityComponent::DoNoot(const FVector& AimPoint)
{
	using namespace PenguinAbilitiesPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	UWorld* World = GetWorld();
	const FVector Dir = AimDirection(AimPoint);
	const float Size = Penguin->GetSizeScale();
	const FVector Origin = Penguin->GetAimOrigin() + Dir * 20.f * Size;
	const float Range = NootRange * Size * PinguTraitTuning::GetReachScale(UseStrength); // (further with the use's strength: Traits mode)
	// Ranges reach the capsule's surface: a normal penguin's centre may be 30 cm further away.
	const float SurfaceRange = Range - NormalPenguinRadius;
	const float CosHalf = FMath::Cos(FMath::DegreesToRadians(NootConeDeg));
	Penguin->PlayAimedAction(EPenguinClip::Noot, 1.4f, Dir); // the beak stretches into the horn, pointing along Dir

	// A wall shields the penguins behind it (its ice may break below, after the penguins).
	const ABlockIsland* Walls = ABlockIsland::FindNear(World, Penguin->GetActorLocation());
	bool bHit = false;
	ForEachOpponent(World, Penguin, [&](APenguinCharacter* Other)
	{
		if (!IsInCone(Other, Origin, Dir, SurfaceRange, CosHalf) || !HasClearLine(Walls, Penguin, Other))
		{
			return;
		}
		// A shotgun: full blast standing inside them (the honk's mouth in their capsule), little at the edge.
		const float Falloff = NootFalloff(Other->DistanceTo(Origin), SurfaceRange);
		const float Shape = (Falloff - NootEdgeShare) / (1.f - NootEdgeShare); // 1 at point blank, 0 at the edge
		const float PushSpeed = NootPointBlankPush * Falloff;
		const FVector2D Push(static_cast<float>(Dir.X) * PushSpeed, FMath::Max(static_cast<float>(Dir.Z) * PushSpeed, 0.f));
		// The shell blocks the hit, but the blast blows the bubble along - unless it just pushed: then the pushes clash
		// (ApplyPenguinHit), shell or not.
		if (Other->IsShielded() && !Other->IsBraced())
		{
			BlowShell(this, Penguin, Other, Push * UseStrength); // (the wind's strength: Traits mode)
			return;
		}
		FPenguinHit Hit;
		Hit.Instigator = Penguin;
		Hit.Source = EPenguinAbility::Noot;
		Hit.Strength = UseStrength;
		Hit.Knockback = Push + FVector2D(0.f, FMath::Lerp(NootLiftEdge, NootLiftPointBlank, Shape));
		Hit.Stun = FMath::Lerp(NootStunEdge, NootStunPointBlank, Shape);
		Hit.Location = Other->GetActorLocation();
		bHit |= Other->ApplyPenguinHit(Hit);
	});

	// Everything interacts: shots in the cone get blown away (the same falloff: back fast up close, nudged far away),
	// pegs break, ability bubbles fly, the nearest ice blocks in the cone break.
	TArray<APenguinProjectile*> Projectiles;
	APenguinProjectile::GetAll(World, Projectiles);
	for (APenguinProjectile* Shot : Projectiles)
	{
		if (Shot->GetShooter() == Penguin)
		{
			continue; // your own shots and pegs are safe
		}
		FVector D = Shot->GetActorLocation() - Origin;
		D.Y = 0.f;
		const float Dist = static_cast<float>(D.Size());
		if (Dist > Range + Shot->GetRadius() || (Dist > 30.f && FVector::DotProduct(D / Dist, Dir) < CosHalf))
		{
			continue;
		}
		const float Falloff = NootFalloff(Dist - Shot->GetRadius(), Range);
		Shot->Push(Penguin, Dir, FMath::Max(NootShotSpeed * Falloff, NootShotMinSpeed));
	}
	TArray<APinguPickup*> Pickups;
	APinguPickup::GetAll(World, Pickups);
	for (APinguPickup* Pickup : Pickups)
	{
		FVector D = Pickup->GetIconLocation() - Origin;
		D.Y = 0.f;
		const float Dist = static_cast<float>(D.Size());
		if (Dist > Range + Pickup->GetRadius() || (Dist > 30.f && FVector::DotProduct(D / Dist, Dir) < CosHalf))
		{
			continue;
		}
		Pickup->Push(Dir * NootPickupPush * NootFalloff(Dist - Pickup->GetRadius(), Range));
	}
	// A falling iceberg in the cone (not our own) is blown away the way we honk.
	AIcebergDrop::BlowInArea(World, Penguin, Origin, Range, Dir, CosHalf, NootIcebergStrength);
	BreakIceInArea(World, Penguin, Origin, Range * NootIceReach, NootIceBlocks, Dir, CosHalf);
	// A giant snowball in the nearer half of the cone is blasted apart (nothing pushes it: the noot breaks it).
	if (APinguGiantSnowball::BurstInArea(World, Origin, Range * PinguTuning::GiantNootShare, Penguin, TEXT("noot"), Dir, CosHalf) > 0)
	{
		bHit = true;
	}
	// A snowman where the noot breaks ice (the nearer part of its cone) is blown apart.
	if (APinguSnowman::BreakInArea(World, Origin, Range * NootIceReach, Penguin, TEXT("noot"), Dir, CosHalf) > 0)
	{
		bHit = true;
	}

	for (int32 Index = 0; Index < 3; ++Index)
	{
		APinguFX::Ring(this, Origin + Dir * (70.f + Index * 110.f) * Size, (45.f + Index * 35.f) * Size, FLinearColor(1.f, 0.92f, 0.6f), 0.18f + Index * 0.06f);
	}
	APinguFX::Puffs(this, Origin + Dir * 40.f * Size, 8, 26.f * Size, 700.f, Dir, 0.5f, FLinearColor(1.f, 0.97f, 0.9f));
	APinguFX::Sound(this, EPinguSound::Noot);
	APinguFX::Shake(this, bHit ? 0.35f : 0.2f);

	// Movement: the blast kicks you back (aim down for a hop, sideways to scoot away).
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		const bool bAir = !Move->IsGroundedForAbilities();
		float Up = FMath::Max(0.f, static_cast<float>(-Dir.Z)) * NootRecoil * 1.3f;
		if (!bAir && Up < 300.f)
		{
			Up = 0.f; // a near-level honk on the ground scoots you back instead of a 1 cm hop that uses up the jump
		}
		const float Side = static_cast<float>(-Dir.X) * NootRecoil;
		Move->AddAbilityImpulse(FVector2D(Side, Up));
	}
	return true;
}

bool UPenguinAbilityComponent::DoIceGun(const FVector& AimPoint)
{
	using namespace PenguinAbilitiesPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	const FVector Dir = AimDirection(AimPoint);
	Penguin->PlayAction(Penguin->PickNearFlipper(EPenguinClip::Toss, EPenguinClip::TossL), 2.2f);
	const FVector Muzzle = Penguin->GetAimOrigin() + Dir * 45.f;
	APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::IceBolt, Penguin, Muzzle, Dir * IceBoltSpeed);
	APinguFX::Sound(this, EPinguSound::IceGun);
	APinguFX::Sparkles(this, Muzzle, 5, 20.f, FLinearColor(0.6f, 0.92f, 1.f));
	// Movement: a little recoil in the air (shoot down for a hop). Shooting the snow makes ice to slip on.
	if (UPenguinMovementComponent* Move = Penguin->GetPenguinMovement())
	{
		if (!Move->IsGroundedForAbilities())
		{
			Move->AddAbilityImpulse(FVector2D(static_cast<float>(-Dir.X) * IceGunRecoil, static_cast<float>(-Dir.Z) * IceGunRecoil * 1.2f));
		}
	}
	return true;
}

bool UPenguinAbilityComponent::DoSizeRay(const FVector& AimPoint, bool bGrow)
{
	using namespace PenguinAbilitiesPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	// One size step, as a shot ray's hit (eight grow steps from normal are 8x, two shrink steps 0.5x).
	const float Factor = bGrow ? PinguGrowRayFactor : PinguShrinkRayFactor;
	FVector ToAim = AimPoint - Penguin->GetAimOrigin();
	ToAim.Y = 0.f;
	Penguin->PlayAction(EPenguinClip::Cast, 1.8f);
	// Aim at your own penguin to use it on yourself: grow to run faster and jump higher, shrink to be a tiny target.
	// On a gamepad: push the right stick straight down.
	const float AimLen = static_cast<float>(ToAim.Size());
	const bool bSelf = AimLen < SelfCastRadius * Penguin->GetSizeScale() || (Penguin->IsStickAim() && ToAim.Z < -0.9f * AimLen);
	if (bSelf)
	{
		// The size limits hold for ourselves too: one step past 8x and we pop, past 0.5x and we vanish (nobody's knock-out).
		Penguin->ChangeSize(Factor, PinguTuning::SizeRayDuration * PinguTraitTuning::GetDurationScale(UseStrength), Penguin);
		return true;
	}
	const FVector Dir = ToAim.GetSafeNormal();
	APenguinProjectile::Launch(GetWorld(), bGrow ? EPenguinProjectileKind::GrowRay : EPenguinProjectileKind::ShrinkRay, Penguin,
		Penguin->GetAimOrigin() + Dir * 45.f, Dir * RaySpeed);
	APinguFX::Sound(this, bGrow ? EPinguSound::Grow : EPinguSound::Shrink, 0.7f);
	return true;
}

// ---------------------------------------------------------------------------- tick

void UPenguinAbilityComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	// Server / solo: whatever wrote the loadout or a cooldown from outside the game (a memory trainer) is undone every frame.
	GuardShadow();
	// Where the penguin is controlled: a ChargeAndThrow (bots) lets go of the icicle when its hold is up.
	TickChargeRelease();
	if (!HasGameplayAuthority())
	{
		// LAN client: only what shows (cooldowns count down by themselves from their end times).
		UpdateShieldVisual();
		if (IsFrostActive())
		{
			if (IsMovementSimulatedHere())
			{
				TickFrostJet(DeltaTime); // our own penguin: we move it, so we fly its frost jet
			}
			TickFrostCloud(DeltaTime);
		}
		// Our own icicle charge, shown ahead of the server: broken here (a stun, frozen, out) - the server cancels it too -
		// or never started there (it said no): put away.
		if (KeyChargeSlot != INDEX_NONE && IcicleShow.Stage == EPinguIcicleShow::Charging && IsChargeBroken(KeyChargeHits))
		{
			ShowChargeEnd(true);
		}
		if (bChargePredicted)
		{
			ChargePredictAge += DeltaTime;
			if (ChargePredictAge > PenguinAbilitiesPrivate::ChargePredictTimeout)
			{
				bChargePredicted = false;
				ShowChargeEnd(false);
			}
		}
		TickIcicleShow(DeltaTime);
		UpdateHandIcicle(DeltaTime);
		return;
	}
	if (bAirLungeUsed)
	{
		const APenguinCharacter* Penguin = GetPenguin();
		const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
		if (!Move || Move->IsGroundedForAbilities())
		{
			bAirLungeUsed = false;
		}
	}
	TickStomp();
	TickFrost(DeltaTime);
	TickShield(DeltaTime);
	if (PendingWallLeft > 0.f)
	{
		PendingWallLeft -= DeltaTime;
		if (PendingWallLeft <= 0.f)
		{
			RaiseIceWall();
		}
	}
	TickChargeOnServer();
	TickIcicleShow(DeltaTime);
	UpdateHandIcicle(DeltaTime);
}

// ---------------------------------------------------------------------------- what every machine shows

void UPenguinAbilityComponent::PlayLocalUseVisuals(EPenguinAbility Ability)
{
	if (Ability != EPenguinAbility::Gust)
	{
		return;
	}
	const APenguinCharacter* Penguin = GetPenguin();
	APinguFX* FX = Penguin ? APinguFX::Get(this) : nullptr;
	if (!FX)
	{
		return;
	}
	// The tornado gathers: a ring of puffs swirls in round the penguin (16 with full effects; the tornado's own funnel and
	// wind are APinguTornado's, every machine draws them from the replicated tornado).
	const FVector Center = Penguin->GetAimOrigin();
	const int32 Count = FMath::Max(6, FMath::RoundToInt(16.f * UPinguSettings::GetFXDensity()));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Angle = Index / static_cast<float>(Count) * 2.f * UE_PI;
		const FVector Out(FMath::Cos(Angle), 0.f, FMath::Sin(Angle));
		const FVector Round(-FMath::Sin(Angle), FMath::FRandRange(-0.2f, 0.2f), FMath::Cos(Angle));
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Puff;
		D.Location = Center + Out * 140.f;
		D.Velocity = Round * 700.f - Out * 260.f;
		D.Size = FMath::FRandRange(22.f, 36.f);
		D.Life = 0.35f;
		D.Drag = 2.f;
		D.Color = FLinearColor(0.9f, 0.96f, 1.f);
		FX->Spawn(D);
	}
}

void UPenguinAbilityComponent::UpdateShieldVisual()
{
	// Clients: the character shows the bubble while the replicated shell lasts (the server tells it itself).
	const bool bActive = IsShieldActive();
	if (bActive == bShieldVisualOn)
	{
		return;
	}
	bShieldVisualOn = bActive;
	if (APenguinCharacter* Penguin = GetPenguin())
	{
		Penguin->OnShieldChanged(bActive);
	}
}

void UPenguinAbilityComponent::OnRep_Nc()
{
	// New uses of this penguin's abilities: their local visuals (not for the counts that come with the penguin itself).
	const bool bPlaying = HasBegunPlay();
	for (int32 Slot = 0; Slot < UseCounts.Num(); ++Slot)
	{
		const int32 Before = SeenUseCounts.IsValidIndex(Slot) ? SeenUseCounts[Slot] : 0;
		if (bPlaying && UseCounts[Slot] > Before)
		{
			PlayLocalUseVisuals(GetAbility(Slot));
		}
	}
	SeenUseCounts = UseCounts;
}

void UPenguinAbilityComponent::OnRep_Shield()
{
	UpdateShieldVisual();
}

void UPenguinAbilityComponent::OnRep_IcicleThrow()
{
	if (!HasBegunPlay())
	{
		IcicleShow.Count = IcicleRep.Count; // what came with the penguin is long over
		return;
	}
	const APenguinCharacter* Penguin = GetPenguin();
	if (bChargePredicted && Penguin && Penguin->GetLocalRole() == ROLE_AutonomousProxy)
	{
		// Our own charge, shown here since the press: the server's is the same one (nothing to start again).
		if (IcicleRep.Count == IcicleShow.Count)
		{
			bChargePredicted = false;
			if (IcicleRep.Stage == 3 || IcicleRep.Stage == 4 || IcicleRep.Stage == 0)
			{
				ShowChargeEnd(IcicleRep.Stage >= 3, IcicleRep.Stage == 4);
			}
			return;
		}
		if (static_cast<uint8>(IcicleRep.Count + 1) == IcicleShow.Count)
		{
			return; // older news: ours is still on its way to the server
		}
		bChargePredicted = false; // something else: show what the server says
	}
	if (IcicleRep.Count != IcicleShow.Count)
	{
		// A charge we have not shown yet (it may be thrown already: then it is thrown as soon as it is pulled out here).
		if (IcicleRep.Stage == 1 || IcicleRep.Stage == 2)
		{
			ShowChargeBegin(IcicleRep.Count);
			if (IcicleRep.Stage == 2)
			{
				ShowChargeRelease();
			}
		}
		else
		{
			IcicleShow.Count = IcicleRep.Count; // over before it got here: nothing to show
		}
		return;
	}
	switch (IcicleRep.Stage)
	{
	case 2:
		if (IcicleShow.Stage == EPinguIcicleShow::Charging && !IcicleShow.bReleasePending)
		{
			ShowChargeRelease();
		}
		break;
	case 3:
		ShowChargeEnd(true);
		break;
	case 4:
		ShowChargeEnd(true, true);
		break;
	case 0:
		ShowChargeEnd(false);
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------- the icicle throw as every machine shows it

void UPenguinAbilityComponent::ShowChargeBegin(uint8 Count)
{
	IcicleShow = FIcicleShowState();
	IcicleShow.Count = Count;
	IcicleShow.Stage = EPinguIcicleShow::Charging;
	IcicleShow.bFresh = true; // its time starts with the next frame
	IcicleShow.AimSide = PickIcicleSide(0.f); // the penguin starts turning to its aim at once (APenguinCharacter::GetVisualFacing)
	// A soft icy shimmer as the icicle comes out (every machine plays its own).
	APinguFX::Sound(this, EPinguSound::Frost, 0.22f, 0.05f, 1.7f);
}

void UPenguinAbilityComponent::ShowChargeRelease()
{
	if (IcicleShow.Stage != EPinguIcicleShow::Charging)
	{
		return;
	}
	if (IcicleShow.ChargeTime >= IciclePullOutTime - 0.001f)
	{
		IcicleShow.Stage = EPinguIcicleShow::Throwing;
		IcicleShow.StageTime = 0.f;
		IcicleShow.bReleasePending = false;
	}
	else
	{
		IcicleShow.bReleasePending = true; // thrown as soon as it is out
	}
}

void UPenguinAbilityComponent::ShowChargeEnd(bool bBreak, bool bMelt)
{
	if (IcicleShow.Stage == EPinguIcicleShow::None)
	{
		return;
	}
	if (bMelt && bHandIcicleShown && HandIcicle)
	{
		// Held too long: the icicle melts away in the flipper - a soft white poof, a few drops, no shards (this machine's own effect).
		const FVector At = HandIcicle->GetComponentLocation();
		APinguFX::Puffs(this, At, 5, 18.f, 90.f, FVector::UpVector, 0.8f, FLinearColor(0.85f, 0.93f, 1.f));
		APinguFX::Sparkles(this, At, 6, 22.f, FLinearColor(0.6f, 0.9f, 1.f));
		APinguFX::Sound(this, EPinguSound::Frost, 0.3f, 0.05f, 0.8f);
	}
	else if (bBreak && !bMelt && bHandIcicleShown && HandIcicle)
	{
		// The icicle breaks in the flipper (this machine's own effect).
		const FVector At = HandIcicle->GetComponentLocation();
		APinguFX::Shards(this, At, 4, 380.f, FLinearColor(0.72f, 0.92f, 1.f));
		APinguFX::Sparkles(this, At, 4, 25.f, FLinearColor(0.6f, 0.9f, 1.f));
		APinguFX::Sound(this, EPinguSound::IcicleHit, 0.4f, 0.05f, 1.5f);
	}
	IcicleShow.Stage = EPinguIcicleShow::None;
	IcicleShow.StageTime = 0.f;
	IcicleShow.bReleasePending = false;
}

void UPenguinAbilityComponent::TickIcicleShow(float DeltaTime)
{
	FIcicleShowState& Show = IcicleShow;
	if (Show.Stage == EPinguIcicleShow::None)
	{
		return;
	}
	if (Show.bFresh)
	{
		Show.bFresh = false;
		return;
	}
	Show.StageTime += DeltaTime;
	// The penguin faces its aim: followed all through the charge (it turns round if the aim crosses over) and until the icicle
	// has left the flipper, then held through the follow-through - whatever the aim does after the throw.
	if (Show.Stage == EPinguIcicleShow::Charging || Show.StageTime < IcicleSwingTime)
	{
		Show.AimSide = PickIcicleSide(Show.AimSide);
	}
	if (Show.Stage == EPinguIcicleShow::Charging)
	{
		Show.ChargeTime += DeltaTime;
		if (Show.bReleasePending && Show.ChargeTime >= IciclePullOutTime - 0.001f)
		{
			Show.Stage = EPinguIcicleShow::Throwing;
			Show.StageTime = 0.f;
			Show.bReleasePending = false;
		}
		else if (!Show.bReleasePending && !Show.bFullChargeShown && Show.ChargeTime >= IcicleFullChargeTime)
		{
			// Fully charged: a little icy "ting" (the icicle glints and the arm trembles: UpdateHandIcicle, the animation).
			Show.bFullChargeShown = true;
			HandGlintTimer = 0.f;
			APinguFX::Sound(this, EPinguSound::IcicleHit, 0.22f, 0.f, 1.9f);
		}
	}
	else if (Show.Stage == EPinguIcicleShow::Throwing && Show.StageTime >= IcicleSwingTime + IcicleFollowTime)
	{
		Show.Stage = EPinguIcicleShow::None;
		Show.StageTime = 0.f;
	}
}

void UPenguinAbilityComponent::CreateHandIcicle()
{
	APenguinCharacter* Penguin = GetPenguin();
	USkeletalMeshComponent* SkelMesh = Penguin ? Penguin->GetMesh() : nullptr;
	if (HandIcicle || !SkelMesh || !SkelMesh->GetSkeletalMeshAsset() || SkelMesh->GetBoneIndex(FName(TEXT("Hand_R"))) == INDEX_NONE
		|| SkelMesh->GetBoneIndex(FName(TEXT("Hand_L"))) == INDEX_NONE)
	{
		return; // (not the penguin rig: no icicle in the flipper, the throw still works)
	}
	UStaticMesh* IcicleMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Icicle.SM_Icicle"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	if (!IcicleMesh)
	{
		return;
	}
	HandIcicleBone = FName(TEXT("Hand_R"));
	HandIcicle = NewObject<UStaticMeshComponent>(Penguin, TEXT("HandIcicle"));
	HandIcicle->SetStaticMesh(IcicleMesh);
	HandIcicle->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HandIcicle->SetGenerateOverlapEvents(false);
	HandIcicle->SetCanEverAffectNavigation(false);
	HandIcicle->SetCastShadow(false);
	HandIcicle->SetUsingAbsoluteScale(true); // its own size (the bones carry the FBX's unit scale)
	HandIcicle->SetHiddenInGame(true);
	// The thrown icicle's material and glow.
	if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Prop.M_Prop"), nullptr, LOAD_Quiet | LOAD_NoWarn))
	{
		HandIcicleMaterial = UMaterialInstanceDynamic::Create(Material, this);
		HandIcicleMaterial->SetScalarParameterValue(TEXT("Emissive"), PenguinAbilitiesPrivate::HandIcicleGlow);
		HandIcicle->SetMaterial(0, HandIcicleMaterial);
	}
	HandIcicle->SetupAttachment(SkelMesh, HandIcicleBone);
	HandIcicle->RegisterComponent();
}

void UPenguinAbilityComponent::UpdateHandIcicle(float DeltaTime)
{
	using namespace PenguinAbilitiesPrivate;
	APenguinCharacter* Penguin = GetPenguin();
	const FIcicleShowState& Show = IcicleShow;
	const bool bCharging = Show.Stage == EPinguIcicleShow::Charging;
	const bool bWanted = Penguin && Penguin->IsAlive() && !Penguin->IsHidden()
		&& ((bCharging && Show.ChargeTime >= HandIcicleAppear) || (Show.Stage == EPinguIcicleShow::Throwing && Show.StageTime < IcicleSwingTime));
	if (bWanted && !HandIcicle)
	{
		CreateHandIcicle();
	}
	if (!HandIcicle)
	{
		bHandIcicleShown = false;
		return;
	}
	if (bWanted != bHandIcicleShown)
	{
		bHandIcicleShown = bWanted;
		HandIcicle->SetHiddenInGame(!bWanted);
		bHandSideFresh = bWanted; // it comes out in the flipper of the side the penguin faces (no swap)
		if (!bWanted)
		{
			HandShakeAmplitude = HandShakeOffset = HandGlowNow = HandHaloNow = HandSparkleRate = 0.f;
			bHandHaloShown = false;
			HandSparkleTimer = 0.f;
		}
	}
	if (!bWanted)
	{
		return;
	}
	USkeletalMeshComponent* SkelMesh = Penguin->GetMesh();
	// In the flipper the camera sees (turned right that is Flipper_L) - the one the animation throws with. The penguin faces its
	// aim while it charges (APenguinCharacter::GetVisualFacing), and may turn round under the icicle when the aim crosses over:
	// then the icicle shrinks away, is put in the other flipper at its smallest and grows back (HandSwapTime), so it never
	// jumps across the body. A throw that starts, or the swing itself, takes the right flipper at once.
	const bool bWantLeft = Penguin->GetVisualFacing() > 0.f;
	// The side it is held up on: the aim's (a nearly vertical aim keeps the side it has), changed only together with the flipper.
	const float WantSide = Show.AimSide != 0.f ? Show.AimSide : (bWantLeft ? 1.f : -1.f);
	float TurnScale = 1.f;
	if (bHandSideFresh || !bCharging)
	{
		bHandSideFresh = false;
		bHandLeft = bWantLeft;
		HandSide = WantSide;
		HandSwapAge = -1.f;
	}
	else
	{
		if (HandSwapAge < 0.f && (bHandLeft != bWantLeft || HandSide != WantSide))
		{
			HandSwapAge = 0.f;
		}
		if (HandSwapAge >= 0.f)
		{
			HandSwapAge += DeltaTime;
			const float Half = HandSwapTime * 0.5f;
			if (HandSwapAge >= Half && (bHandLeft != bWantLeft || HandSide != WantSide))
			{
				// (at its smallest: the frame it moves over it has next to no size, whatever the frame rate)
				bHandLeft = bWantLeft;
				HandSide = WantSide;
				TurnScale = HandSwapMinScale;
			}
			else if (HandSwapAge >= HandSwapTime)
			{
				HandSwapAge = -1.f;
			}
			else
			{
				TurnScale = FMath::Max(HandSwapMinScale, HandSwapAge < Half ? 1.f - FMath::SmoothStep(0.f, Half, HandSwapAge) : FMath::SmoothStep(Half, HandSwapTime, HandSwapAge));
			}
		}
	}
	const bool bLeft = bHandLeft;
	const FName Hand(bLeft ? TEXT("Hand_L") : TEXT("Hand_R"));
	const FName Shoulder(bLeft ? TEXT("Flipper_L") : TEXT("Flipper_R"));
	if (HandIcicleBone != Hand || HandIcicle->GetAttachSocketName() != Hand)
	{
		HandIcicleBone = Hand;
		HandIcicle->AttachToComponent(SkelMesh, FAttachmentTransformRules::KeepWorldTransform, Hand);
	}
	const FVector HandAt = SkelMesh->GetSocketLocation(Hand);
	FVector Arm = (HandAt - SkelMesh->GetSocketLocation(Shoulder)).GetSafeNormal();
	if (Arm.IsNearlyZero())
	{
		Arm = FVector(0.f, 0.f, -1.f);
	}
	// It comes out along the arm (pulled from behind the back), then is held up like a spear pointing a little above the aim;
	// the swing brings it down onto the aim.
	FVector Aim = GetCurrentAim() - Penguin->GetAimOrigin();
	Aim.Y = 0.f;
	Aim = Aim.GetSafeNormal();
	if (Aim.IsNearlyZero())
	{
		Aim = FVector(Penguin->GetFacing(), 0.f, 0.f);
	}
	const float Side = HandSide;
	const float CockAngle = FMath::Min(FMath::Atan2(static_cast<float>(Aim.Z), FMath::Abs(static_cast<float>(Aim.X))) + FMath::DegreesToRadians(IcicleCockTiltDeg), UE_HALF_PI);
	const FVector Cocked(Side * FMath::Cos(CockAngle), 0.f, FMath::Sin(CockAngle));
	FVector Tip;
	if (bCharging)
	{
		const float Out = FMath::SmoothStep(HandIcicleGrown, IciclePullOutTime, Show.ChargeTime);
		Tip = (Arm * (1.f - Out) + Cocked * Out).GetSafeNormal();
	}
	else
	{
		const float Swing = FMath::Clamp(Show.StageTime / IcicleSwingTime, 0.f, 1.f);
		Tip = (Cocked * (1.f - Swing) + Aim * Swing).GetSafeNormal();
	}
	if (Tip.IsNearlyZero())
	{
		Tip = Cocked;
	}
	const float Size = Penguin->GetSizeScale();
	const float Grown = (bCharging ? FMath::Lerp(0.3f, 1.f, FMath::SmoothStep(HandIcicleAppear, HandIcicleGrown, Show.ChargeTime)) : 1.f) * TurnScale;
	const float Length = 100.f * HandIcicleLength * Grown * Size;

	// The charge builds (0 = just pulled out .. 1 = full): the icicle trembles more and more, glows brighter and whiter, wears a
	// cold rim and sheds frost sparkles. All of it from the key's time, so every machine shows the same.
	const float Charged = FMath::Clamp(Show.ChargeTime / IcicleFullChargeTime, 0.f, 1.f);
	const float Build = bCharging ? GetChargeShakeBuild(Show.ChargeTime) : 0.f;
	// Small, fast jitter: a few unrelated sines (none a multiple of the frame rate), the icicle thrown about a little and
	// wobbling in the play plane about its grip.
	const float Clock = Show.ChargeTime * 2.f * UE_PI;
	const float ShakeX = FMath::Sin(Clock * 41.3f) * 0.6f + FMath::Sin(Clock * 27.9f + 1.3f) * 0.4f;
	const float ShakeZ = FMath::Sin(Clock * 37.1f + 0.7f) * 0.6f + FMath::Sin(Clock * 23.7f + 2.1f) * 0.4f;
	const float ShakeTilt = FMath::Sin(Clock * 33.7f + 2.9f) * 0.7f + FMath::Sin(Clock * 47.9f + 0.4f) * 0.3f;
	const FVector Jitter = FVector(ShakeX, 0.f, ShakeZ) * (HandShakeMove * Build * Size);
	const float Tilt = FMath::DegreesToRadians(HandShakeTiltDeg) * Build * ShakeTilt;
	const float TiltCos = FMath::Cos(Tilt);
	const float TiltSin = FMath::Sin(Tilt);
	const FVector ShakyTip = FVector(Tip.X * TiltCos - Tip.Z * TiltSin, Tip.Y, Tip.X * TiltSin + Tip.Z * TiltCos).GetSafeNormal();
	const FVector Center = HandAt + ShakyTip * (Length * 0.5f - HandIcicleGrip * Grown * Size) + Jitter;
	HandIcicle->SetWorldLocationAndRotation(Center, ShakyTip.Rotation());
	HandIcicle->SetWorldScale3D(FVector(HandIcicleLength, HandIcicleThickness, HandIcicleThickness) * Grown * Size);
	HandIcicleCenter = Center;
	HandIcicleTip = ShakyTip;
	HandIcicleDrawScale = TurnScale;
	HandShakeAmplitude = HandShakeMove * Build * Size;
	HandShakeOffset = static_cast<float>(Jitter.Size());

	// Brighter and whiter the more it is charged; fully charged it glints, now and then with a sparkle at its tip.
	float Glow = HandIcicleGlow + HandIcicleChargeGlow * Charged;
	float HaloPulse = 0.f;
	const bool bFull = bCharging && Charged >= 1.f && !Show.bReleasePending;
	if (bFull)
	{
		const float Wave = FMath::Max(0.f, FMath::Sin((Show.ChargeTime - IcicleFullChargeTime) * 2.f * UE_PI * HandIcicleGlintRate));
		const float Wave4 = Wave * Wave * Wave * Wave;
		HaloPulse = Wave4;
		HandGlowNow = Glow; // (the steady glow; the glint is on top of it)
		Glow += HandIcicleGlint * Wave4;
		HandGlintTimer -= DeltaTime;
		if (HandGlintTimer <= 0.f)
		{
			HandGlintTimer = 1.f / HandIcicleGlintRate;
			APinguFX::Sparkles(this, Center + ShakyTip * Length * 0.45f, 2, 12.f * Size, FLinearColor(0.75f, 0.95f, 1.f));
		}
	}
	else
	{
		HandGlowNow = Glow;
	}
	if (HandIcicleMaterial)
	{
		HandIcicleMaterial->SetScalarParameterValue(TEXT("Emissive"), Glow);
		// Cold, not white-hot: the tint (it multiplies the emissive) leans from white to a saturated cyan with the charge, so the
		// icicle glows ice blue instead of burning out to white.
		HandIcicleMaterial->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(FLinearColor(1.f, 1.f, 1.f), FLinearColor(0.4f, 1.f, 1.45f), Charged));
	}

	// A cold glow laid along it (the same soft glow the flying icicle trails, wider and brighter the more it is charged): the
	// glow you can see on every preset. A piece per frame that fades in a moment, so a trembling icicle smears it.
	HandHaloNow = HandHaloFull * (FMath::Pow(Charged, HandHaloCurve) + 0.3f * HaloPulse); // (it stays through the swing)
	bHandHaloShown = HandHaloNow > 0.02f;
	if (bHandHaloShown)
	{
		const FVector Base = Center - ShakyTip * (Length * 0.5f);
		const FVector TipAt = Center + ShakyTip * (Length * 0.5f);
		const float Width = FMath::Lerp(HandHaloWidthStart, HandHaloWidthFull, Charged) * Grown * Size;
		APinguFX::GlowTrail(this, Base, TipAt, Width, FMath::Lerp(FLinearColor(0.35f, 0.8f, 1.f), FLinearColor(0.3f, 0.85f, 1.f), Charged), FMath::Min(HandHaloNow, 1.f), HandHaloLife);
	}

	// Frost sparkles come off it, faster the more it is charged (the effects setting thins them out: Low sheds few).
	HandSparkleRate = 0.f;
	if (bCharging && !Show.bReleasePending && Charged > 0.12f)
	{
		const float Interval = FMath::Lerp(HandSparkleSlow, HandSparkleFast, Charged);
		const int32 Count = Charged > 0.7f ? 2 : 1;
		HandSparkleRate = static_cast<float>(Count) / Interval;
		HandSparkleTimer -= DeltaTime;
		if (HandSparkleTimer <= 0.f)
		{
			HandSparkleTimer = Interval * FMath::FRandRange(0.7f, 1.3f);
			APinguFX::Sparkles(this, Center + ShakyTip * Length * FMath::FRandRange(-0.4f, 0.5f), Count, (6.f + 8.f * Charged) * Size, FLinearColor(0.75f, 0.95f, 1.f));
		}
	}
}

void UPenguinAbilityComponent::OnRep_Frost()
{
	FrostFxTimer = 0.f; // a new breath puffs at once
}

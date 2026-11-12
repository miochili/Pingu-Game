// Pingu - the island's size for 2 to 10 penguins, the stage camera's furthest view and how it follows, Snow Warp kept on
// the warper's screen: the autotest's steps (APenguinAutoTest::AddIslandSizeSteps) and the lobby test's check that bots
// make the lobby island bigger (APinguLobbyTest::UpdateLobbyIslandSizeCheck).

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PinguPickup.h"
#include "PinguPortal.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace PenguinAutoTestIslandPrivate
{
	/** The sizes measured (penguins), each on a seed of its own (SizeSeedBase + penguins). */
	constexpr int32 Sizes[] = { 2, 4, 6, 10 };
	constexpr int32 SizeSeedBase = 4300;
	constexpr float SizeStepTime = 1.5f;
	/** The camera is measured (and the picture taken) this long into a size step (s): it snapped, then settled. */
	constexpr float MeasureAt = 1.1f;
	/** Sky drops tried on each size: every one must come down over the island. */
	constexpr int32 SkyDrops = 6;
	/** A start is this high (cm) above the ground under it (ABlockIsland's starting places). */
	constexpr float StartAboveGround = 62.f;
	constexpr float StartTolerance = 5.f;
	/** "At the furthest view": within this share of MaxFrameDistance. */
	constexpr float CapTolerance = 0.01f;

	// ---- following on a ten-penguin island
	constexpr int32 FollowPenguins = 10;
	constexpr int32 FollowSeed = 4310;
	constexpr float FollowStepTime = 8.f;
	/** Our penguin is moved along the ground this fast (cm/s, about twice a run) ... */
	constexpr float FollowSpeed = 1200.f;
	/** ... from its start at the left end (the camera settles first), stopping this share of the way to the right end's
	 *  start, for this long, then on to the right end. */
	constexpr float FollowStartAt = 0.8f;
	constexpr float FollowStopShare = 0.6f;
	constexpr float FollowStopTime = 1.4f;
	/** One second after the stop the view's middle is this close to the followed point (cm). */
	constexpr float SettleAfter = 1.f;
	constexpr float SettleWant = 20.f;
	/** While running, the view trails at least this far (cm): it lags. */
	constexpr float MinTrail = 100.f;
	/** Measuring tolerances: the lag box (cm), the edge keep (share of the half screen). */
	constexpr float LagTolerance = 2.f;
	constexpr float EdgeTolerance = 0.03f;
	/** At the island's right end: no more sea past it than the frame's margin and this (cm). */
	constexpr float EndSeaSlack = 100.f;
	constexpr float EndMeasureAfter = 1.f;

	// ---- Snow Warp on screen (the same ten-penguin island)
	constexpr float WarpStepTime = 4.f;
	constexpr float WarpFireAt = 1.f;
	constexpr float WarpResetAt = 2.f;
	constexpr float WarpWideFireAt = 2.8f;
	/** Aimed this far to the right (and up): far outside the screen. */
	constexpr float WarpAimFar = 3000.f;
	constexpr float WarpAimUp = 300.f;
	/** On screen after the warp: checked this long after it (s). */
	constexpr float WarpSeenAfterA = 0.1f;
	constexpr float WarpSeenAfterB = 0.4f;
	/** The ultrawide screen simulated: 21:9. */
	constexpr float WideAspect = 21.f / 9.f;
	/** The warp counts as kept short when it ended at least this far (cm) before where it would have gone. */
	constexpr float WarpShortened = 50.f;

	/** The share of the half screen between Body's nearest corner and the nearest left / right edge (X) and top / bottom edge
	 *  (Y); negative when it is off the screen. False if it could not be projected. */
	static bool EdgeShares(const APlayerController* Viewer, const FBox& Body, FVector2D& OutShare)
	{
		int32 SizeX = 0;
		int32 SizeY = 0;
		Viewer->GetViewportSize(SizeX, SizeY);
		if (SizeX < 2 || SizeY < 2)
		{
			return false;
		}
		OutShare = FVector2D(1.0, 1.0);
		for (const double X : { Body.Min.X, Body.Max.X })
		{
			for (const double Z : { Body.Min.Z, Body.Max.Z })
			{
				FVector2D Screen;
				if (!Viewer->ProjectWorldLocationToScreen(FVector(X, 0.0, Z), Screen, false))
				{
					return false;
				}
				OutShare.X = FMath::Min(OutShare.X, FMath::Min(Screen.X, SizeX - Screen.X) / (0.5 * SizeX));
				OutShare.Y = FMath::Min(OutShare.Y, FMath::Min(Screen.Y, SizeY - Screen.Y) / (0.5 * SizeY));
			}
		}
		return true;
	}

	/** All of Penguin, head to feet (play plane). */
	static FBox BodyOf(const APenguinCharacter* Penguin)
	{
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const FVector Location(Penguin->GetActorLocation().X, 0.0, Penguin->GetActorLocation().Z);
		const FVector Half(Capsule->GetScaledCapsuleRadius(), 0.0, Capsule->GetScaledCapsuleHalfHeight());
		return FBox(Location - Half, Location + Half);
	}
}

/** The island steps' state (from step to step). */
struct FPinguIslandTestState
{
	// the sizes
	TMap<int32, float> Distance;
	FString Line;
	bool bLineOk = false;
	bool bMeasured = false;
	// following
	float LeftX = 0.f;
	float StopX = 0.f;
	float RightX = 0.f;
	float X = 0.f;
	float GroundZ = 0.f;
	int32 Phase = 0;
	float StopAt = -1.f;
	float EndAt = -1.f;
	int32 Frames = 0;
	int32 FramesAtCap = 0;
	int32 FramesFollowing = 0;
	int32 FramesInLag = 0;
	float WorstLagShareX = 0.f;
	float WorstLagShareZ = 0.f;
	FVector2D LeastEdge = FVector2D(1.0, 1.0);
	float MaxTrail = 0.f;
	float SettleError = -1.f;
	float SeaPastEnd = 1.e9f;
	float SeaWant = 0.f;
	bool bShotA = false;
	bool bShotB = false;
	// Snow Warp
	int32 WarpPhase = 0;
	float WarpAt = -1.f;
	FVector WarpSpot = FVector::ZeroVector;
	FString WarpLine;
	bool bWarpOkA = false;
	bool bWarpOkB = false;
	int32 WarpSeen = 0;
	int32 WarpSeenOk = 0;
};

void APenguinAutoTest::AddIslandSizeSteps()
{
	using namespace PenguinAutoTestIslandPrivate;
	IslandTest = MakeShared<FPinguIslandTestState>();

	// ---- The arena's size for 2, 4, 6 and 10 penguins: its width within the table's range, every start on the ground with
	// room to stand, sky drops over the island, and how far out the camera goes (4 penguins: the old fixed island).
	for (const int32 Size : Sizes)
	{
		AddStep(*FString::Printf(TEXT("look_IslandSize%d"), Size), SizeStepTime,
			[this, Size]
			{
				FPinguIslandTestState& State = *IslandTest;
				State.bMeasured = false;
				State.bLineOk = false;
				SpawnTarget();
				ClearAbilityLeftovers();
				APinguPickup::DestroyAll(GetWorld());
				ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				if (!Island)
				{
					State.Line = TEXT("no island");
					return;
				}
				const int32 IslandSeed = SizeSeedBase + Size;
				FString Same;
				bool bSame = true;
				if (Size == 4)
				{
					// Four penguins: the old fixed island (RandomWidth 26), block for block.
					Island->ArenaPenguins = 0;
					Island->RegenerateArena(IslandSeed);
					const uint32 OldSum = Island->GetCellChecksum();
					const int32 OldBlocks = Island->GetNumBlocks();
					Island->ArenaPenguins = Size;
					Island->RegenerateArena(IslandSeed);
					bSame = OldSum == Island->GetCellChecksum() && OldBlocks == Island->GetNumBlocks();
					Same = FString::Printf(TEXT(", the old fixed island's blocks %d (%d blocks, %08x / %08x)"), bSame ? 1 : 0, OldBlocks, OldSum, Island->GetCellChecksum());
				}
				else
				{
					Island->ArenaPenguins = Size;
					Island->RegenerateArena(IslandSeed);
				}
				const FPinguArenaSize Arena = ABlockIsland::GetArenaSize(Size);
				const FBox Bounds = Island->GetBounds();
				const int32 Width = Bounds.IsValid ? FMath::RoundToInt(static_cast<float>(Bounds.Max.X - Bounds.Min.X) / Island->CellSize) : 0;
				const bool bWidth = Width >= Arena.GetNarrowest() && Width <= Arena.GetWidest() && Island->GetArenaPenguins() == Size;

				// Every start on the ground, with room for a penguin there; the closest two.
				const TArray<FVector> Starts = Island->GetSpreadSpawnLocations(Size);
				const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
				const float Radius = Capsule->GetUnscaledCapsuleRadius();
				const float HalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
				int32 OnGround = 0;
				float Closest = TNumericLimits<float>::Max();
				for (int32 Index = 0; Index < Starts.Num(); ++Index)
				{
					float GroundZ = 0.f;
					if (Island->FindGroundBelow(Starts[Index], 200.f, GroundZ) && FMath::Abs(static_cast<float>(Starts[Index].Z) - GroundZ - StartAboveGround) < StartTolerance
						&& Island->CanFitCapsule(FVector(Starts[Index].X, 0.f, GroundZ + HalfHeight + 1.f), Radius, HalfHeight))
					{
						++OnGround;
					}
					if (Index > 0)
					{
						Closest = FMath::Min(Closest, static_cast<float>(Starts[Index].X - Starts[Index - 1].X));
					}
				}

				// Sky drops: each must come down over the island's ground.
				FRandomStream Drops(IslandSeed * 7);
				int32 Landed = 0;
				for (int32 Drop = 0; Drop < SkyDrops; ++Drop)
				{
					const APinguPickup* Pickup = APinguPickup::SpawnFromSky(GetWorld(), Drops);
					float GroundZ = 0.f;
					if (Pickup && Island->FindGroundBelow(FVector(Pickup->GetActorLocation().X, 0.f, Bounds.Max.Z + 50.0), static_cast<float>(Bounds.Max.Z - Bounds.Min.Z) + 200.f, GroundZ))
					{
						++Landed;
					}
				}
				APinguPickup::DestroyAll(GetWorld());

				// Us at the first start, the dummy at the last; the camera snaps to the new island.
				if (Starts.Num() > 0)
				{
					Teleport(Starts[0]);
					ResetTarget(Starts.Last());
				}
				Penguin->ResetForRound(Penguin->GetActorLocation());
				Penguin->SetInputLocked(false);
				VirtualInput = FVector2D::ZeroVector;
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(true);
					Stage->SnapToTargets(0.3f);
				}
				State.bLineOk = bWidth && bSame && Starts.Num() == Size && OnGround == Size && Landed == SkyDrops;
				State.Line = FString::Printf(TEXT("%d blocks wide (want %d..%d), starts %d of %d on the ground with room (closest two %.1f m apart), sky drops %d of %d over the island%s"),
					Width, Arena.GetNarrowest(), Arena.GetWidest(), OnGround, Size, Starts.Num() > 1 ? Closest / 100.f : 0.f, Landed, SkyDrops, *Same);
			},
			[this, Size](float T)
			{
				FPinguIslandTestState& State = *IslandTest;
				const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
				if (!State.bMeasured && T > MeasureAt && Stage)
				{
					State.bMeasured = true;
					State.Distance.Add(Size, Stage->GetFrameDistance());
					Shot(*FString::Printf(TEXT("58_island_size_%d"), Size));
				}
			},
			[this, Size]
			{
				const FPinguIslandTestState& State = *IslandTest;
				const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
				const float* Measured = State.Distance.Find(Size);
				Report(FString::Printf(TEXT("RESULT island size %d: %s, camera %.0f cm out%s: %s"), Size, *State.Line, Measured ? *Measured : -1.f,
					Stage && Stage->IsFollowing() ? TEXT(" (following)") : TEXT(""), State.bLineOk && Measured ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- The camera: closer for a small island, never further out than MaxFrameDistance (six and ten penguins: right there).
	AddStep(TEXT("look_IslandSizeCamera"), 0.1f, [] {}, [](float) {},
		[this]
		{
			const FPinguIslandTestState& State = *IslandTest;
			const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
			const float Cap = Stage ? Stage->MaxFrameDistance : 0.f;
			const auto Get = [&State](int32 Size) { const float* Found = State.Distance.Find(Size); return Found ? *Found : -1.f; };
			const float Two = Get(2);
			const float Four = Get(4);
			const float Six = Get(6);
			const float Ten = Get(10);
			const bool bOk = Two > 0.f && Two < Four && Four <= Cap * (1.f + CapTolerance) && FMath::Abs(Six - Cap) <= Cap * CapTolerance
				&& FMath::Abs(Ten - Cap) <= Cap * CapTolerance;
			Report(FString::Printf(TEXT("RESULT island size camera: 2 penguins %.0f cm, 4: %.0f, 6: %.0f, 10: %.0f (furthest %.0f; want 2 < 4 <= furthest, 6 and 10 at it): %s"),
				Two, Four, Six, Ten, Cap, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	// ---- Following on a ten-penguin island (too big for the furthest view): our penguin from the left end to 60 % of the way,
	// a stop, then on to the right end. Every frame: the camera at its furthest, following us, never further behind the
	// followed point than the lag box, all of us at least the edge keep inside the screen. It trails while we move, settles
	// on us within a second of the stop, and at the right end shows no more sea than the island's frame.
	AddStep(TEXT("look_IslandFollow"), FollowStepTime,
		[this]
		{
			FPinguIslandTestState& State = *IslandTest;
			const TMap<int32, float> SizeDistances = State.Distance;
			State = FPinguIslandTestState();
			State.Distance = SizeDistances;
			SpawnTarget();
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			if (!Island)
			{
				return;
			}
			Island->ArenaPenguins = FollowPenguins;
			Island->RegenerateArena(FollowSeed);
			const TArray<FVector> Starts = Island->GetSpreadSpawnLocations(FollowPenguins);
			if (Starts.Num() < 2)
			{
				return;
			}
			State.LeftX = static_cast<float>(Starts[0].X);
			State.RightX = static_cast<float>(Starts.Last().X);
			State.StopX = FMath::Lerp(State.LeftX, State.RightX, FollowStopShare);
			State.X = State.LeftX;
			State.GroundZ = static_cast<float>(Starts[0].Z);
			Teleport(Starts[0]);
			Penguin->ResetForRound(Starts[0]);
			Penguin->SetInputLocked(true); // it only goes where the step puts it
			ResetTarget(Starts[Starts.Num() / 2]);
			VirtualInput = FVector2D::ZeroVector;
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets(0.3f);
			}
		},
		[this](float T)
		{
			FPinguIslandTestState& State = *IslandTest;
			const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
			const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			const APlayerController* Viewer = GetWorld()->GetFirstPlayerController();
			if (!Stage || !Island || !Viewer || State.RightX <= State.LeftX)
			{
				return;
			}
			// Measured first: the camera and our penguin both as of the last frame.
			const FVector Here = Penguin->GetActorLocation();
			if (T > FollowStartAt)
			{
				++State.Frames;
				const float Cap = Stage->MaxFrameDistance;
				State.FramesAtCap += FMath::Abs(Stage->GetFrameDistance() - Cap) <= Cap * CapTolerance ? 1 : 0;
				State.FramesFollowing += Stage->IsFollowing() && Stage->GetFollowPenguin() == Penguin.Get() ? 1 : 0;
				const FVector2D Focus = Stage->GetFocusPoint();
				const FVector2D Point = Stage->GetFollowPoint();
				const float HalfW = Stage->GetFrameDistance() * FMath::Tan(FMath::DegreesToRadians(Stage->FieldOfView * 0.5f));
				int32 SizeX = 0;
				int32 SizeY = 0;
				Viewer->GetViewportSize(SizeX, SizeY);
				const float HalfH = HalfW * static_cast<float>(SizeY) / static_cast<float>(FMath::Max(SizeX, 1));
				const float LagX = FMath::Abs(static_cast<float>(Focus.X - Point.X));
				const float LagZ = FMath::Abs(static_cast<float>(Focus.Y - Point.Y));
				State.WorstLagShareX = FMath::Max(State.WorstLagShareX, LagX / HalfW);
				State.WorstLagShareZ = FMath::Max(State.WorstLagShareZ, LagZ / HalfH);
				State.FramesInLag += LagX <= Stage->FollowMaxLag.X * HalfW + LagTolerance && LagZ <= Stage->FollowMaxLag.Y * HalfH + LagTolerance ? 1 : 0;
				FVector2D Edge;
				if (PenguinAutoTestIslandPrivate::EdgeShares(Viewer, PenguinAutoTestIslandPrivate::BodyOf(Penguin.Get()), Edge))
				{
					State.LeastEdge.X = FMath::Min(State.LeastEdge.X, Edge.X);
					State.LeastEdge.Y = FMath::Min(State.LeastEdge.Y, Edge.Y);
				}
				if ((State.Phase == 1 || State.Phase == 3) && FMath::Abs(Point.X - Here.X) < 1.0)
				{
					// (Where the followed point is our penguin - not held at the island's end by the pan limits.)
					State.MaxTrail = FMath::Max(State.MaxTrail, static_cast<float>(Point.X - Focus.X));
				}
				if (State.Phase == 2 && State.SettleError < 0.f && T - State.StopAt >= SettleAfter)
				{
					State.SettleError = static_cast<float>(FMath::Max(FMath::Abs(Focus.X - Here.X), (Focus - Point).Size()));
				}
				if (State.Phase == 4 && !State.bShotB && T - State.EndAt >= EndMeasureAfter)
				{
					State.bShotB = true;
					FBox2D View;
					const FBox Bounds = Island->GetBounds();
					if (Stage->GetViewRect(View) && Bounds.IsValid)
					{
						State.SeaPastEnd = static_cast<float>(View.Max.X - Bounds.Max.X);
						State.SeaWant = Stage->IslandMargin.X + EndSeaSlack;
					}
					Shot(TEXT("58_island_follow_b"));
				}
			}

			// Then our penguin moves on (along the ground: the top of the column under it, pits skipped at the height it had).
			const float Dt = GetWorld()->GetDeltaSeconds();
			if (State.Phase == 0 && T > FollowStartAt)
			{
				State.Phase = 1;
			}
			if (State.Phase == 1 || State.Phase == 3)
			{
				const float Goal = State.Phase == 1 ? State.StopX : State.RightX;
				State.X = FMath::Min(State.X + FollowSpeed * Dt, Goal);
				if (State.Phase == 1 && !State.bShotA && State.X > 0.5f * (State.LeftX + State.StopX))
				{
					State.bShotA = true;
					Shot(TEXT("58_island_follow_a"));
				}
				if (State.X >= Goal)
				{
					State.Phase += 1;
					(State.Phase == 2 ? State.StopAt : State.EndAt) = T;
				}
			}
			else if (State.Phase == 2 && T - State.StopAt >= FollowStopTime)
			{
				State.Phase = 3;
			}
			const FBox Bounds = Island->GetBounds();
			float GroundZ = 0.f;
			if (Island->FindGroundBelow(FVector(State.X, 0.f, Bounds.Max.Z + 100.0), static_cast<float>(Bounds.Max.Z - Bounds.Min.Z) + 300.f, GroundZ))
			{
				State.GroundZ = GroundZ + StartAboveGround;
			}
			Teleport(FVector(State.X, 0.f, State.GroundZ));
		},
		[this]
		{
			const FPinguIslandTestState& State = *IslandTest;
			const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
			const FVector2D EdgeWant = Stage ? Stage->FollowEdgeKeep : FVector2D(0.25, 0.2);
			const bool bOk = State.Frames > 0 && State.FramesAtCap == State.Frames && State.FramesFollowing == State.Frames && State.FramesInLag == State.Frames
				&& State.LeastEdge.X >= EdgeWant.X - EdgeTolerance && State.LeastEdge.Y >= EdgeWant.Y - EdgeTolerance && State.MaxTrail > MinTrail
				&& State.SettleError >= 0.f && State.SettleError < SettleWant && State.SeaPastEnd <= State.SeaWant && State.Phase == 4;
			Report(FString::Printf(TEXT("RESULT island follow (10 penguins): of %d frames at the furthest view %d, following us %d, within the lag box %d (worst %.2f / %.2f of the half screen); "
				"us at least %.2f / %.2f of the half screen inside its edges (want %.2f / %.2f); trailed up to %.0f cm while moving (want over %.0f); %.0f cm off 1 s after the stop "
				"(want under %.0f); at the right end %.0f cm of sea past the island (want at most %.0f): %s"),
				State.Frames, State.FramesAtCap, State.FramesFollowing, State.FramesInLag, State.WorstLagShareX, State.WorstLagShareZ, State.LeastEdge.X, State.LeastEdge.Y,
				EdgeWant.X, EdgeWant.Y, State.MaxTrail, MinTrail, State.SettleError, SettleWant, State.SeaPastEnd, State.SeaWant, bOk ? TEXT("ok") : TEXT("FAIL")));
			Penguin->SetInputLocked(false);
		});

	// ---- Snow Warp stays on the warper's screen: at the island's right end (the camera at the end of its pan), a warp aimed
	// far out to the right ends inside the screen (its margin in, room for all of us) and we are on screen after it; then the
	// same with a 21:9 screen simulated - it ends inside the 16:9 screen of the same height (not further out).
	AddStep(TEXT("look_WarpInView"), WarpStepTime,
		[this]
		{
			FPinguIslandTestState& State = *IslandTest;
			State.WarpPhase = 0;
			State.WarpLine.Reset();
			State.bWarpOkA = State.bWarpOkB = false;
			State.WarpSeen = State.WarpSeenOk = 0;
			ClearAbilityLeftovers();
			APinguPickup::DestroyAll(GetWorld());
			ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			if (!Island)
			{
				return;
			}
			if (Island->GetArenaPenguins() != FollowPenguins)
			{
				Island->ArenaPenguins = FollowPenguins;
				Island->RegenerateArena(FollowSeed);
			}
			const TArray<FVector> Starts = Island->GetSpreadSpawnLocations(FollowPenguins);
			if (Starts.Num() < 2)
			{
				return;
			}
			State.WarpSpot = Starts.Last();
			Teleport(State.WarpSpot);
			Penguin->ResetForRound(State.WarpSpot);
			Penguin->SetInputLocked(false);
			Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::SnowWarp, EPenguinAbility::SnowWarp, EPenguinAbility::Snowball });
			ResetTarget(Starts[0]);
			VirtualInput = FVector2D::ZeroVector;
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SetViewRectOverride(FBox2D(ForceInit));
				Stage->SnapToTargets(0.3f);
			}
		},
		[this](float T)
		{
			FPinguIslandTestState& State = *IslandTest;
			APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
			const APlayerController* Viewer = GetWorld()->GetFirstPlayerController();
			if (!Stage || !Viewer)
			{
				return;
			}
			const auto Fire = [this, &State](int32 Slot, const FBox2D& Allowed, const TCHAR* Label, float WouldReachX) -> bool
			{
				const FVector From = Penguin->GetActorLocation();
				Penguin->SetAimPoint(From + FVector(WarpAimFar, 0.f, WarpAimUp));
				const bool bUsed = Penguin->ActivateAbility(Slot);
				const APinguPortal* Portal = APinguPortal::FindOwnedBy(GetWorld(), Penguin.Get());
				const FVector Exit = Portal ? Portal->GetEnd(1) : Penguin->GetActorLocation();
				const FVector Free = From + FVector(WarpAimFar, 0.f, WarpAimUp).GetSafeNormal() * PinguTuning::WarpRange;
				const bool bInside = Allowed.bIsValid && Exit.X >= Allowed.Min.X - 1.0 && Exit.X <= Allowed.Max.X + 1.0 && Exit.Z >= Allowed.Min.Y - 1.0 && Exit.Z <= Allowed.Max.Y + 1.0;
				const bool bShort = Exit.X < FMath::Min(static_cast<float>(Free.X), WouldReachX) - WarpShortened;
				State.WarpLine += FString::Printf(TEXT("%s%s: used %d, from x %.0f to %.0f (free: %.0f%s), the allowed x %.0f..%.0f z %.0f..%.0f"),
					State.WarpLine.IsEmpty() ? TEXT("") : TEXT("; "), Label, bUsed ? 1 : 0, From.X, Exit.X, Free.X,
					WouldReachX < Free.X ? *FString::Printf(TEXT(", on the wide screen it could go to %.0f"), WouldReachX) : TEXT(""),
					Allowed.Min.X, Allowed.Max.X, Allowed.Min.Y, Allowed.Max.Y);
				State.WarpAt = GetWorld()->GetTimeSeconds();
				return bUsed && Portal && bInside && bShort;
			};
			// On screen after the warp (all of us).
			if (State.WarpAt >= 0.f && (State.WarpPhase == 1 || State.WarpPhase == 3))
			{
				const float Since = GetWorld()->GetTimeSeconds() - State.WarpAt;
				const bool bFirstLook = Since >= WarpSeenAfterA && State.WarpSeen % 2 == 0;
				const bool bSecondLook = Since >= WarpSeenAfterB && State.WarpSeen % 2 == 1;
				if (bFirstLook || bSecondLook)
				{
					FVector2D Edge;
					++State.WarpSeen;
					State.WarpSeenOk += PenguinAutoTestIslandPrivate::EdgeShares(Viewer, PenguinAutoTestIslandPrivate::BodyOf(Penguin.Get()), Edge) && Edge.X >= 0.0 && Edge.Y >= 0.0 ? 1 : 0;
					if (State.WarpPhase == 1 && bFirstLook)
					{
						Shot(TEXT("58_warp_in_view"));
					}
					if (bSecondLook)
					{
						State.WarpPhase += 1;
					}
				}
			}
			if (State.WarpPhase == 0 && T > WarpFireAt)
			{
				// The real screen (16:9 in the tests).
				FBox2D Allowed;
				const bool bView = APinguPortal::GetWarpView(Penguin.Get(), Allowed);
				State.bWarpOkA = bView && Fire(0, Allowed, TEXT("16:9 screen"), TNumericLimits<float>::Max());
				State.WarpPhase = 1;
			}
			if (State.WarpPhase == 2 && T > WarpResetAt)
			{
				// Back at the right end for the wide screen.
				Teleport(State.WarpSpot);
				Penguin->ResetForRound(State.WarpSpot);
				Penguin->SetInputLocked(false);
				Stage->SnapToTargets(0.3f);
				State.WarpPhase = 10;
			}
			if (State.WarpPhase == 10 && T > WarpWideFireAt)
			{
				// A 21:9 screen of the same height and middle as the real one: the exit stays inside the 16:9 one.
				FBox2D Real;
				if (Stage->GetViewRect(Real))
				{
					const double MiddleX = 0.5 * (Real.Min.X + Real.Max.X);
					const double HalfWide = 0.5 * (Real.Max.Y - Real.Min.Y) * WideAspect;
					const FBox2D Wide(FVector2D(MiddleX - HalfWide, Real.Min.Y), FVector2D(MiddleX + HalfWide, Real.Max.Y));
					const FBox2D Reference = APinguPortal::ToReferenceView(Wide);
					const FVector2D Margin = Reference.GetExtent() * APinguPortal::WarpViewMargin;
					const FBox2D Allowed(Reference.Min + Margin, Reference.Max - Margin);
					// Where it could have gone on the wide screen alone (its own margin in, room for us).
					const float WouldReachX = static_cast<float>(Wide.Max.X - Wide.GetExtent().X * APinguPortal::WarpViewMargin
						- Penguin->GetCapsuleComponent()->GetScaledCapsuleRadius());
					Stage->SetViewRectOverride(Wide);
					State.bWarpOkB = Fire(1, Allowed, TEXT("21:9 screen"), WouldReachX);
					Stage->SetViewRectOverride(FBox2D(ForceInit));
				}
				State.WarpPhase = 3;
			}
		},
		[this]
		{
			FPinguIslandTestState& State = *IslandTest;
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetViewRectOverride(FBox2D(ForceInit));
			}
			const bool bOk = State.bWarpOkA && State.bWarpOkB && State.WarpSeen == 4 && State.WarpSeenOk == 4;
			Report(FString::Printf(TEXT("RESULT warp in view: %s; on screen after the warps %d of %d looks: %s"), *State.WarpLine, State.WarpSeenOk, State.WarpSeen,
				bOk ? TEXT("ok") : TEXT("FAIL")));
			// Back to the old fixed size for whatever comes next.
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
			{
				Island->ArenaPenguins = 0;
			}
		});
}

// ---------------------------------------------------------------------------- the lobby test

bool APinguLobbyTest::UpdateLobbyIslandSizeCheck(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	// Nine bots (ten penguins): a bigger lobby island, the camera at its furthest and on our penguin; then one bot (two
	// penguins): a small island and the camera closer. Each a moment after the cards changed (the lobby waits for more clicks).
	constexpr float IslandWait = 1.4f;
	constexpr int32 BigBots = 9;
	constexpr int32 SmallBots = 1;
	constexpr float CapTolerance = 0.01f;
	constexpr float SmallShare = 0.8f; // two penguins: the camera at most this share of the furthest view
	constexpr float ShotWait = 0.4f;   // the screenshot is taken before the cards change again
	constexpr int32 Done = 99;
	if (IslandStep == Done)
	{
		return false;
	}
	const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	const APenguinCamera* StageCamera = APenguinCamera::FindOrSpawn(GetWorld());
	if (!Island || !StageCamera)
	{
		IslandStep = Done;
		Check(false, TEXT("the lobby island grows with the bots (no island or camera)"));
		Check(false, TEXT("the lobby island shrinks without them (no island or camera)"));
		return false;
	}
	const FBox Bounds = Island->GetBounds();
	const float Width = Bounds.IsValid ? static_cast<float>(Bounds.Max.X - Bounds.Min.X) / 100.f : 0.f;
	const float Cap = StageCamera->MaxFrameDistance;
	const APlayerController* Viewer = GetWorld()->GetFirstPlayerController();
	const APenguinCharacter* You = Viewer ? Cast<APenguinCharacter>(Viewer->GetPawn()) : nullptr;
	switch (IslandStep)
	{
	case 0:
		IslandStartPenguins = Island->GetArenaPenguins();
		IslandStartWidth = Width;
		for (int32 Guard = 0; Guard < 12 && LobbyHUD->GetBotCount() < BigBots; ++Guard)
		{
			LobbyHUD->Action_AddBot();
		}
		IslandStepAt = TotalTime;
		IslandStep = 1;
		return true;

	case 1:
		if (TotalTime - IslandStepAt < IslandWait)
		{
			return true;
		}
		{
			const FVector2D Focus = StageCamera->GetFocusPoint();
			const FVector2D Point = StageCamera->GetFollowPoint();
			const float HalfW = StageCamera->GetFrameDistance() * FMath::Tan(FMath::DegreesToRadians(StageCamera->FieldOfView * 0.5f));
			const bool bOnYou = You && StageCamera->IsFollowing() && StageCamera->GetFollowPenguin() == You && FMath::Abs(Focus.X - Point.X) <= StageCamera->FollowMaxLag.X * HalfW + 2.f;
			IslandBigWidth = Width;
			IslandBigDistance = StageCamera->GetFrameDistance();
			const bool bOk = Island->GetArenaPenguins() == BigBots + 1 && Width > IslandStartWidth && FMath::Abs(IslandBigDistance - Cap) <= Cap * CapTolerance && bOnYou;
			Shot(TEXT("lobbytest_island_big"));
			Check(bOk, FString::Printf(TEXT("the lobby island grows with the bots: %d bots, an island for %d penguins %.0f m wide (was for %d, %.0f m), the camera %.0f cm out (furthest %.0f) on your penguin %d"),
				LobbyHUD->GetBotCount(), Island->GetArenaPenguins(), Width, IslandStartPenguins, IslandStartWidth, IslandBigDistance, Cap, bOnYou ? 1 : 0));
		}
		IslandStepAt = TotalTime;
		IslandStep = 10;
		return true;

	case 10:
		if (TotalTime - IslandStepAt < ShotWait)
		{
			return true;
		}
		for (int32 Guard = 0; Guard < 12 && LobbyHUD->GetBotCount() > SmallBots; ++Guard)
		{
			LobbyHUD->Action_RemoveBot(LobbyHUD->GetBotCount() - 1);
		}
		IslandStepAt = TotalTime;
		IslandStep = 2;
		return true;

	case 2:
		if (TotalTime - IslandStepAt < IslandWait)
		{
			return true;
		}
		{
			const float Small = StageCamera->GetFrameDistance();
			const bool bOk = Island->GetArenaPenguins() == SmallBots + 1 && Width < IslandBigWidth && Width < IslandStartWidth && Small < Cap * SmallShare;
			Shot(TEXT("lobbytest_island_small"));
			Check(bOk, FString::Printf(TEXT("the lobby island shrinks without them: %d bot, an island for %d penguins %.0f m wide (was %.0f m), the camera %.0f cm out (want under %.0f)"),
				LobbyHUD->GetBotCount(), Island->GetArenaPenguins(), Width, IslandBigWidth, Small, Cap * SmallShare));
		}
		IslandStepAt = TotalTime;
		IslandStep = 20;
		return true;

	case 20:
		if (TotalTime - IslandStepAt < ShotWait)
		{
			return true;
		}
		IslandStep = Done;
		return false;

	default:
		return false;
	}
}

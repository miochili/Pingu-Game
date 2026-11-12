// Pingu - the round hazards (APinguRoundHazards, 2026-11-07, card round-hazards). The autotest's steps
// (APenguinAutoTest::AddRoundHazardSteps; filter hazard_,look_Hazard + spawn_target). They make the main island a four-penguin
// arena (seed 4711) where they need one, so they come after the build limit's steps, last in the script.
//
//  hazard_Setup        the game mode's hook: a penguin with the Wind trait -> SetupForRound spawns the hazard (not running before a
//                      fight); outside a round (practice) it removes itself.
//  hazard_RisingSea    started 20 s into a fight: the island's SeaLevel = its own + the hazard's rise every frame, never down, never
//                      over the top (260 cm under the lowest start ground); the dummy dropped beside the island is knocked out by
//                      the risen sea far above the old one; stopped, the sea stands; removed, the sea is back exactly
//                      (98_hazard_sea_risen).
//  hazard_Crumble      the first crumble: the outermost column at each end shakes (nothing in the middle), then is gone; the island
//                      two columns narrower (98_hazard_crumble_cracking).
//  hazard_CrumbleMin   started 500 s into a fight: crumbles until the island would be narrower than 6 columns, never past that.
//  hazard_CrumbleBot   a bot standing on the outermost column walks off the shaking blocks before they fall.
//  hazard_Wind         on the flat test island: no push in the 1 s warning (the arrow shows), then the gust moves a penguin that
//                      stands still about WindSpeed x (WindGust - WindRamp) = 660 cm its way.
//  hazard_WindAgainst  walking against the gust still gets you somewhere (> 300 cm against it).
//  hazard_Stop         stopped in the middle of a gust: no push from then on.
//  hazard_Rain         a drop forced onto the dummy and the rain of 7 s: every drop's snowball is nobody's, lands within 90 cm of its
//                      ring, the forced one knocks the dummy; rings show; at least 4 drops (98_hazard_rain_markers).
//  look_HazardWind     the wind's warning on the arena with the HUD (98_hazard_wind_warning, 98_hazard_wind_gust).

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinBot.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguRoundHazards.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PenguinAutoTestHazardsPrivate
{
	constexpr int32 ArenaSeed = 4711;
	constexpr int32 ArenaPenguins = 4;

	static const TCHAR* Ok(bool b) { return b ? TEXT("ok") : TEXT("FAIL"); }

	static ABlockIsland* MakeArena(UWorld* World)
	{
		ABlockIsland* Island = ABlockIsland::Get(World);
		if (Island)
		{
			Island->ArenaPenguins = ArenaPenguins;
			Island->RegenerateArena(ArenaSeed);
			Island->FlushEdits();
		}
		return Island;
	}

	/** Columns (cell X) with at least one block, from the island's cells. */
	static int32 CountColumns(const ABlockIsland* Island, int32* OutMin = nullptr, int32* OutMax = nullptr)
	{
		int32 Count = 0;
		int32 Min = 1000;
		int32 Max = -1000;
		for (int32 X = -60; X <= 60; ++X)
		{
			for (int32 Z = -20; Z <= 25; ++Z)
			{
				if (Island->IsSolidCell(FIntPoint(X, Z)))
				{
					++Count;
					Min = FMath::Min(Min, X);
					Max = FMath::Max(Max, X);
					break;
				}
			}
		}
		if (OutMin)
		{
			*OutMin = Min;
		}
		if (OutMax)
		{
			*OutMax = Max;
		}
		return Count;
	}

	/** Blocks whose middle is left of / right of these world X. */
	static int32 CountBlocksOutside(const ABlockIsland* Island, float MinX, float MaxX)
	{
		int32 Count = 0;
		for (int32 X = -60; X <= 60; ++X)
		{
			for (int32 Z = -20; Z <= 25; ++Z)
			{
				const FIntPoint Cell(X, Z);
				if (Island->IsSolidCell(Cell))
				{
					const float CellX = static_cast<float>(Island->CellToWorld(Cell).X);
					Count += CellX < MinX || CellX > MaxX ? 1 : 0;
				}
			}
		}
		return Count;
	}

	/** The stage camera looking at the whole island from the front (its tick off). */
	static APenguinCamera* FrameIsland(UWorld* World, const ABlockIsland* Island, float DropZ = 0.f)
	{
		APenguinCamera* Stage = APenguinCamera::FindOrSpawn(World);
		if (Stage && Island)
		{
			Stage->SetActorTickEnabled(false);
			const FBox Bounds = Island->GetBounds();
			const FVector Focus = Bounds.GetCenter() - FVector(0.f, 0.f, DropZ);
			const float Distance = static_cast<float>(Bounds.Max.X - Bounds.Min.X) * 1.6f + 900.f;
			const FVector From = Focus + FVector(0.f, Distance, Distance * 0.08f);
			Stage->SetActorLocationAndRotation(From, (Focus - From).Rotation());
		}
		return Stage;
	}

	/** A start place on the island (Index of four, left to right), on its ground. */
	static FVector StartPlace(ABlockIsland* Island, int32 Index)
	{
		TArray<FVector> Starts = Island->GetSpreadSpawnLocations(ArenaPenguins);
		Starts.Sort([](const FVector& A, const FVector& B) { return A.X < B.X; });
		return Starts.IsValidIndex(Index) ? Starts[Index] : Island->GetBounds().GetCenter();
	}

	struct FSeaRun
	{
		float Base = 0.f;
		float Top = 0.f;
		float LastRise = -1.f;
		int32 Samples = 0;
		int32 Bad = 0;
		int32 Down = 0;
		int32 OverTop = 0;
		float RiseAtStop = -1.f;
		float RiseAfterStop = -1.f;
		bool bDropped = false;
		bool bKnocked = false;
		float KnockZ = 0.f;
		float GroundMinZ = 0.f;
		bool bShot = false;
	};

	struct FCrumbleRun
	{
		int32 Columns0 = 0;
		int32 Min0 = 0;
		int32 Max0 = 0;
		float Left0 = 0.f;
		float Right0 = 0.f;
		bool bWarnSeen = false;
		bool bMiddleShaking = false;
		bool bShot = false;
		int32 Columns1 = 0;
		int32 Outside1 = -1;
		int32 Crumbles = 0;
		bool bBotOut = false;
		float BotX = 0.f;
	};

	struct FWindRun
	{
		float X0 = 0.f;
		float XWarnEnd = 0.f;
		float WarnDrift = 0.f;
		float XEnd = 0.f;
		int32 Dir = 0;
		float ArrowMax = 0.f;
		int32 Moved = 0;
		bool bStopped = false;
		float XAtStop = 0.f;
		float DriftAfterStop = 0.f;
		bool bShotWarn = false;
		bool bShotGust = false;
	};

	struct FRainRun
	{
		TMap<TWeakObjectPtr<APenguinProjectile>, FVector> Balls;
		int32 Balls0 = 0;
		int32 Landed = 0;
		int32 Near = 0;
		int32 NotNobodys = 0;
		int32 MaxRings = 0;
		float ForcedX = 0.f;
		float ForcedLandAt = -1.f;
		FVector TargetStart = FVector::ZeroVector;
		float TargetMoved = 0.f;
		bool bShot = false;
	};
}

void APenguinAutoTest::AddRoundHazardSteps()
{
	using namespace PenguinAutoTestHazardsPrivate;

	const auto RestoreCamera = [this]()
	{
		if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
		{
			Stage->SetActorTickEnabled(true);
			Stage->SnapToTargets();
		}
		LookCamera.Reset();
	};

	// ---- hazard_Setup: the game mode's hook.
	{
		const TSharedRef<int32> State = MakeShared<int32>(0);
		AddStep(TEXT("hazard_Setup"), 1.f,
			[this, State]
			{
				*State = 0;
				SpawnTarget();
				APinguRoundHazards::ClearAll(GetWorld());
				if (UPenguinTraitComponent* Traits = Penguin.IsValid() ? Penguin->GetTraits() : nullptr)
				{
					Traits->SetTrait(EPinguTrait::Wind, 0.f);
				}
				APinguRoundHazards::SetupForRound(GetWorld());
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				const bool bSpawned = H && H->GetHazard() == EPinguTrait::Wind && !H->IsRunning();
				Report(FString::Printf(TEXT("RESULT hazard setup: a Wind penguin -> hazard %s, running %d (want Wind, 0) %s"),
					H ? *PinguTraits::GetLogName(H->GetHazard()) : TEXT("none"), H && H->IsRunning() ? 1 : 0, Ok(bSpawned)));
			},
			[](float) {},
			[this]
			{
				// Outside a round (the autotest's practice) it goes by itself.
				const bool bGone = APinguRoundHazards::Get(GetWorld()) == nullptr;
				Report(FString::Printf(TEXT("RESULT hazard setup: outside a round it removed itself: %s"), Ok(bGone)));
				APinguRoundHazards::ClearAll(GetWorld());
				if (UPenguinTraitComponent* Traits = Penguin.IsValid() ? Penguin->GetTraits() : nullptr)
				{
					Traits->ClearTrait();
				}
			});
	}

	// ---- hazard_RisingSea
	{
		const TSharedRef<FSeaRun> Run = MakeShared<FSeaRun>();
		AddStep(TEXT("hazard_RisingSea"), 6.5f,
			[this, Run]
			{
				*Run = FSeaRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				ABlockIsland* Island = MakeArena(GetWorld());
				if (!Island)
				{
					return;
				}
				FBox Base;
				float GroundMaxZ = 0.f;
				Island->GetBaseFrame(Base, Run->GroundMinZ, GroundMaxZ);
				Run->Base = Island->SeaLevel;
				Teleport(StartPlace(Island, 1));
				ResetTarget(StartPlace(Island, 2));
				LookCamera = FrameIsland(GetWorld(), Island, 250.f);
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::RisingSea, 20.f);
			},
			[this, Run](float T)
			{
				FSeaRun& R = *Run;
				ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				if (!Island || !H)
				{
					return;
				}
				R.Top = H->GetSeaTop();
				const float Rise = Island->GetSeaRise();
				if (T > 0.05f)
				{
					++R.Samples;
					R.Bad += FMath::Abs(Island->SeaLevel - (R.Base + Rise)) > 0.5f || FMath::Abs(Rise - H->GetSeaRiseNow()) > 0.5f ? 1 : 0;
					R.Down += R.LastRise >= 0.f && Rise < R.LastRise - 0.01f ? 1 : 0;
					R.OverTop += Rise > R.Top + 0.5f ? 1 : 0;
					R.LastRise = Rise;
				}
				if (!R.bShot && T >= 1.5f)
				{
					R.bShot = true;
					Shot(TEXT("98_hazard_sea_risen"));
				}
				if (!R.bDropped && T >= 2.f && Target.IsValid())
				{
					// Beside the island, 2 m over the risen sea: it falls in and is out far above the old sea.
					R.bDropped = true;
					const FBox Bounds = Island->GetBounds();
					ResetTarget(FVector(Bounds.Max.X + 400.f, 0.f, Island->SeaLevel + 200.f));
				}
				if (R.bDropped && !R.bKnocked && Target.IsValid() && Target->IsKnockedOut())
				{
					R.bKnocked = true;
					R.KnockZ = static_cast<float>(Target->GetActorLocation().Z);
				}
				if (R.RiseAtStop < 0.f && T >= 4.f)
				{
					H->StopNow();
					R.RiseAtStop = H->GetSeaRiseNow();
				}
				if (T >= 6.f)
				{
					R.RiseAfterStop = Island->GetSeaRise();
				}
			},
			[this, Run, RestoreCamera]
			{
				FSeaRun& R = *Run;
				ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				APinguRoundHazards::ClearAll(GetWorld());
				const float SeaAfter = Island ? Island->SeaLevel : 0.f;
				const float RiseAfter = Island ? Island->GetSeaRise() : -1.f;
				RestoreCamera();
				const float TopZ = R.Base + R.Top;
				const bool bTop = FMath::Abs(TopZ - (R.GroundMinZ - PinguHazardTuning::SeaTopBelowGround)) < 1.f && R.Top > 500.f;
				Report(FString::Printf(TEXT("RESULT hazard rising sea: top %.0f cm up (%.0f, want %.0f = lowest start ground - %.0f) %s"), R.Top, TopZ,
					R.GroundMinZ - PinguHazardTuning::SeaTopBelowGround, PinguHazardTuning::SeaTopBelowGround, Ok(bTop)));
				Report(FString::Printf(TEXT("RESULT hazard rising sea: %d samples, %d off the island's sea level, %d going down, %d over the top, rise at the end %.0f (want 0, 0, 0) %s"),
					R.Samples, R.Bad, R.Down, R.OverTop, R.LastRise, Ok(R.Samples > 100 && R.Bad == 0 && R.Down == 0 && R.OverTop == 0 && R.LastRise > 0.8f * R.Top)));
				Report(FString::Printf(TEXT("RESULT hazard rising sea: the dummy dropped beside the island knocked out %d at Z %.0f (the old sea's line %.0f) %s"),
					R.bKnocked ? 1 : 0, R.KnockZ, R.Base + 120.f, Ok(R.bKnocked && R.KnockZ > R.Base + 400.f)));
				Report(FString::Printf(TEXT("RESULT hazard rising sea: stopped at %.1f, 2 s later %.1f (stands) %s"), R.RiseAtStop, R.RiseAfterStop,
					Ok(R.RiseAtStop > 0.f && FMath::Abs(R.RiseAfterStop - R.RiseAtStop) < 0.5f)));
				Report(FString::Printf(TEXT("RESULT hazard rising sea: removed, sea %.1f (want %.1f), rise %.1f %s"), SeaAfter, R.Base, RiseAfter,
					Ok(FMath::Abs(SeaAfter - R.Base) < 0.01f && RiseAfter == 0.f)));
				ResetTarget(FVector(FlatIslandX + 400.f, 0.f, 62.f));
			});
	}

	// ---- hazard_Crumble: the first crumble on the arena.
	{
		const TSharedRef<FCrumbleRun> Run = MakeShared<FCrumbleRun>();
		AddStep(TEXT("hazard_Crumble"), 3.5f,
			[this, Run]
			{
				*Run = FCrumbleRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				ABlockIsland* Island = MakeArena(GetWorld());
				if (!Island)
				{
					return;
				}
				Run->Columns0 = CountColumns(Island, &Run->Min0, &Run->Max0);
				Teleport(StartPlace(Island, 1));
				ResetTarget(StartPlace(Island, 2));
				LookCamera = FrameIsland(GetWorld(), Island);
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::CrumblingIsland, 9.5f);
				if (const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld()))
				{
					Run->Left0 = H->GetCrumbleLeftX();
					Run->Right0 = H->GetCrumbleRightX();
				}
			},
			[this, Run](float T)
			{
				FCrumbleRun& R = *Run;
				ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				if (!Island)
				{
					return;
				}
				const float Cell = Island->CellSize;
				if (T > 0.6f && T < 1.9f)
				{
					R.bWarnSeen |= Island->IsCrumblingOver(R.Left0 - 10.f, R.Left0 + 10.f) && Island->IsCrumblingOver(R.Right0 - 10.f, R.Right0 + 10.f);
					R.bMiddleShaking |= Island->IsCrumblingOver(R.Left0 + 1.5f * Cell, R.Right0 - 1.5f * Cell);
				}
				if (!R.bShot && T >= 1.3f)
				{
					R.bShot = true;
					Shot(TEXT("98_hazard_crumble_cracking"));
				}
			},
			[this, Run, RestoreCamera]
			{
				FCrumbleRun& R = *Run;
				ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				R.Crumbles = H ? H->GetCrumbles() : 0;
				if (Island)
				{
					R.Columns1 = CountColumns(Island);
					R.Outside1 = CountBlocksOutside(Island, R.Left0 + 0.5f * Island->CellSize, R.Right0 - 0.5f * Island->CellSize);
				}
				APinguRoundHazards::ClearAll(GetWorld());
				RestoreCamera();
				Report(FString::Printf(TEXT("RESULT hazard crumble: the outermost columns shook %d, the middle shook %d (want 1, 0) %s"), R.bWarnSeen ? 1 : 0,
					R.bMiddleShaking ? 1 : 0, Ok(R.bWarnSeen && !R.bMiddleShaking)));
				Report(FString::Printf(TEXT("RESULT hazard crumble: %d crumble, columns %d -> %d (want -2), blocks left out there %d %s"), R.Crumbles, R.Columns0,
					R.Columns1, R.Outside1, Ok(R.Crumbles == 1 && R.Columns1 == R.Columns0 - 2 && R.Outside1 == 0)));
			});
	}

	// ---- hazard_CrumbleMin: never narrower than CrumbleMinColumns.
	{
		const TSharedRef<FCrumbleRun> Run = MakeShared<FCrumbleRun>();
		AddStep(TEXT("hazard_CrumbleMin"), 3.f,
			[this, Run]
			{
				*Run = FCrumbleRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				if (ABlockIsland* Island = MakeArena(GetWorld()))
				{
					Run->Columns0 = CountColumns(Island);
					Teleport(StartPlace(Island, 1) + FVector(0.f, 0.f, 0.f));
					ResetTarget(FVector(FlatIslandX + 400.f, 0.f, 62.f));
				}
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::CrumblingIsland, 500.f);
			},
			[](float) {},
			[this, Run]
			{
				FCrumbleRun& R = *Run;
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				R.Crumbles = H ? H->GetCrumbles() : 0;
				const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				R.Columns1 = Island ? CountColumns(Island) : 0;
				APinguRoundHazards::ClearAll(GetWorld());
				const int32 Min = PinguHazardTuning::CrumbleMinColumns;
				Report(FString::Printf(TEXT("RESULT hazard crumble min: %d crumbles, columns %d -> %d (want %d or %d) %s"), R.Crumbles, R.Columns0, R.Columns1, Min, Min + 1,
					Ok(R.Crumbles > 0 && R.Columns1 >= Min && R.Columns1 <= Min + 1 && R.Columns1 == R.Columns0 - 2 * R.Crumbles)));
				Teleport(FVector(FlatIslandX - 300.f, 0.f, 62.f));
			});
	}

	// ---- hazard_CrumbleBot: a bot steps off the shaking blocks.
	{
		const TSharedRef<FCrumbleRun> Run = MakeShared<FCrumbleRun>();
		AddStep(TEXT("hazard_CrumbleBot"), 3.5f,
			[this, Run]
			{
				*Run = FCrumbleRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				ABlockIsland* Island = MakeArena(GetWorld());
				if (!Island || !Target.IsValid() || !Target->GetBot())
				{
					return;
				}
				// We wait far off on the flat test island; the bot stands on the arena's outermost left column.
				Teleport(FVector(FlatIslandX - 300.f, 0.f, 62.f));
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::CrumblingIsland, 9.6f);
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				Run->Left0 = H ? H->GetCrumbleLeftX() : 0.f;
				float GroundZ = 0.f;
				Island->FindGroundBelow(FVector(Run->Left0, 0.f, 3000.f), 6000.f, GroundZ);
				ResetTarget(FVector(Run->Left0, 0.f, GroundZ + 70.f));
				Target->GetBot()->SetBotEnabled(true, 2);
			},
			[](float) {},
			[this, Run]
			{
				FCrumbleRun& R = *Run;
				const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				const bool bAlive = Target.IsValid() && !Target->IsKnockedOut();
				const float X = Target.IsValid() ? static_cast<float>(Target->GetActorLocation().X) : 0.f;
				const float Cell = Island ? Island->CellSize : 100.f;
				float GroundZ = -1.f;
				const bool bOnGround = Island && Target.IsValid() && Island->FindGroundBelow(Target->GetActorLocation(), 400.f, GroundZ);
				if (Target.IsValid() && Target->GetBot())
				{
					Target->GetBot()->SetBotEnabled(false);
				}
				APinguRoundHazards::ClearAll(GetWorld());
				Report(FString::Printf(TEXT("RESULT hazard crumble bot: alive %d, at %.0f (the crumbled column %.0f), ground under it %d %s"), bAlive ? 1 : 0, X, R.Left0,
					bOnGround ? 1 : 0, Ok(bAlive && bOnGround && X > R.Left0 + 0.5f * Cell)));
				ResetTarget(FVector(FlatIslandX + 400.f, 0.f, 62.f));
			});
	}

	// ---- hazard_Wind: on the flat test island, a penguin standing still.
	{
		const TSharedRef<FWindRun> Run = MakeShared<FWindRun>();
		AddStep(TEXT("hazard_Wind"), 4.6f,
			[this, Run]
			{
				*Run = FWindRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				VirtualInput = FVector2D::ZeroVector;
				Teleport(FVector(FlatIslandX + 600.f, 0.f, 62.f));
				ResetTarget(FVector(FlatIslandX + 2400.f, 0.f, 62.f));
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::Wind, 2.5f); // the warning at 0.5 s, the gust 1.5 - 4.0 s
			},
			[this, Run](float T)
			{
				FWindRun& R = *Run;
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				if (!H || !Penguin.IsValid())
				{
					return;
				}
				const float X = static_cast<float>(Penguin->GetActorLocation().X);
				if (T < 0.45f)
				{
					R.X0 = X;
				}
				else if (T < 1.45f)
				{
					R.WarnDrift = FMath::Max(R.WarnDrift, FMath::Abs(X - R.X0));
					R.XWarnEnd = X;
				}
				R.ArrowMax = FMath::Max(R.ArrowMax, H->IsWindWarning() ? H->GetArrowOpacity() : 0.f);
				R.Dir = H->GetGustDir();
				R.Moved = FMath::Max(R.Moved, H->GetWindMoved());
				R.XEnd = X;
			},
			[this, Run]
			{
				FWindRun& R = *Run;
				APinguRoundHazards::ClearAll(GetWorld());
				const float Moved = (R.XEnd - R.XWarnEnd) * R.Dir;
				const float Want = PinguHazardTuning::WindSpeed * (PinguHazardTuning::WindGust - PinguHazardTuning::WindRamp);
				Report(FString::Printf(TEXT("RESULT hazard wind: warning drift %.1f cm (want 0), arrow %.2f (want 1) %s"), R.WarnDrift, R.ArrowMax,
					Ok(R.WarnDrift < 3.f && R.ArrowMax > 0.95f)));
				Report(FString::Printf(TEXT("RESULT hazard wind: the gust (%s) moved a penguin standing still %.0f cm its way (want about %.0f), %d penguins moved %s"),
					R.Dir > 0 ? TEXT("right") : TEXT("left"), Moved, Want, R.Moved, Ok(Moved > 0.8f * Want && Moved < 1.2f * Want && R.Moved >= 1)));
			});
	}

	// ---- hazard_WindAgainst: walking against the gust.
	{
		const TSharedRef<FWindRun> Run = MakeShared<FWindRun>();
		AddStep(TEXT("hazard_WindAgainst"), 4.4f,
			[this, Run]
			{
				*Run = FWindRun();
				SpawnTarget();
				VirtualInput = FVector2D::ZeroVector;
				Teleport(FVector(FlatIslandX + 600.f, 0.f, 62.f));
				ResetTarget(FVector(FlatIslandX + 2400.f, 0.f, 62.f));
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::Wind, 2.5f);
			},
			[this, Run](float T)
			{
				FWindRun& R = *Run;
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				if (!H || !Penguin.IsValid())
				{
					return;
				}
				const float X = static_cast<float>(Penguin->GetActorLocation().X);
				R.Dir = H->GetGustDir();
				// Walk against it from the warning on (a player sees the arrow and leans in).
				VirtualInput = T >= 0.6f && R.Dir != 0 ? FVector2D(-static_cast<float>(R.Dir), 0.f) : FVector2D::ZeroVector;
				if (T < 1.5f)
				{
					R.XWarnEnd = X;
				}
				R.XEnd = X;
			},
			[this, Run]
			{
				FWindRun& R = *Run;
				VirtualInput = FVector2D::ZeroVector;
				APinguRoundHazards::ClearAll(GetWorld());
				const float Against = -(R.XEnd - R.XWarnEnd) * R.Dir;
				Report(FString::Printf(TEXT("RESULT hazard wind against: walking into the gust got %.0f cm against it (want > 300) %s"), Against, Ok(Against > 300.f)));
			});
	}

	// ---- hazard_Stop: stopped in the middle of a gust, no push from then on.
	{
		const TSharedRef<FWindRun> Run = MakeShared<FWindRun>();
		AddStep(TEXT("hazard_Stop"), 3.2f,
			[this, Run]
			{
				*Run = FWindRun();
				SpawnTarget();
				VirtualInput = FVector2D::ZeroVector;
				Teleport(FVector(FlatIslandX + 600.f, 0.f, 62.f));
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::Wind, 4.f); // blowing from 0 s
			},
			[this, Run](float T)
			{
				FWindRun& R = *Run;
				APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				if (!H || !Penguin.IsValid())
				{
					return;
				}
				const float X = static_cast<float>(Penguin->GetActorLocation().X);
				if (!R.bStopped && T >= 1.6f)
				{
					R.bStopped = true;
					R.Moved = H->IsGustBlowing() ? 1 : 0; // (it was blowing)
					H->StopNow();
				}
				if (R.bStopped && T >= 1.7f && R.XAtStop == 0.f)
				{
					R.XAtStop = X; // (the penguin's own sliding after the push: none, it never had a velocity)
				}
				if (R.bStopped && T >= 1.7f)
				{
					R.DriftAfterStop = FMath::Max(R.DriftAfterStop, FMath::Abs(X - R.XAtStop));
					R.ArrowMax = FMath::Max(R.ArrowMax, H->GetArrowOpacity() + FMath::Abs(H->GetWindNow()));
				}
			},
			[this, Run]
			{
				FWindRun& R = *Run;
				APinguRoundHazards::ClearAll(GetWorld());
				Report(FString::Printf(TEXT("RESULT hazard stop: blowing at the stop %d, drift after it %.1f cm, wind / arrow after it %.2f (want 1, 0, 0) %s"), R.Moved,
					R.DriftAfterStop, R.ArrowMax, Ok(R.Moved == 1 && R.DriftAfterStop < 1.f && R.ArrowMax < 0.01f)));
			});
	}

	// ---- hazard_Rain: the rain on the arena, one drop forced onto the dummy.
	{
		const TSharedRef<FRainRun> Run = MakeShared<FRainRun>();
		AddStep(TEXT("hazard_Rain"), 7.5f,
			[this, Run]
			{
				Run->Balls.Reset();
				*Run = FRainRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				ABlockIsland* Island = MakeArena(GetWorld());
				if (!Island)
				{
					return;
				}
				// The dummy on a start place under open sky (a floating platform over it would catch the snowball).
				FVector Dummy = StartPlace(Island, 2);
				for (const int32 Index : { 2, 3, 1 })
				{
					const FVector Place = StartPlace(Island, Index);
					float SkyGroundZ = 0.f;
					if (Island->FindGroundBelow(FVector(Place.X, 0.f, Place.Z + 3000.f), 3500.f, SkyGroundZ) && SkyGroundZ < Place.Z)
					{
						Dummy = Place;
						break;
					}
				}
				Teleport(StartPlace(Island, 0));
				ResetTarget(Dummy);
				Run->ForcedX = static_cast<float>(Dummy.X);
				LookCamera = FrameIsland(GetWorld(), Island);
				if (APinguRoundHazards* H = APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::SnowballRain, 0.f))
				{
					H->ForceNextDrop(Run->ForcedX);
				}
			},
			[this, Run](float T)
			{
				FRainRun& R = *Run;
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				if (!H)
				{
					return;
				}
				if (Target.IsValid() && T < 0.1f)
				{
					R.TargetStart = Target->GetActorLocation();
				}
				// Every snowball in the air: whose it is and where it was last.
				TSet<TWeakObjectPtr<APenguinProjectile>> Seen;
				for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
				{
					if (It->GetKind() != EPenguinProjectileKind::Snowball || It->IsActorBeingDestroyed())
					{
						continue;
					}
					const TWeakObjectPtr<APenguinProjectile> Ball(*It);
					if (!R.Balls.Contains(Ball))
					{
						++R.Balls0;
						R.NotNobodys += It->GetShooter() ? 1 : 0;
					}
					R.Balls.Add(Ball, It->GetActorLocation());
					Seen.Add(Ball);
				}
				for (auto Entry = R.Balls.CreateIterator(); Entry; ++Entry)
				{
					if (!Seen.Contains(Entry.Key()))
					{
						// Gone: it landed (or hit somebody) here - near a drop's ring?
						++R.Landed;
						const FVector Last = Entry.Value();
						for (const FPinguRainDrop& Drop : H->GetRainDrops())
						{
							if (FMath::Abs(Last.X - Drop.X) < 90.f && FMath::Abs(Last.Z - Drop.Z) < 220.f)
							{
								++R.Near;
								break;
							}
						}
						Entry.RemoveCurrent();
					}
				}
				for (const FPinguRainDrop& Drop : H->GetRainDrops())
				{
					if (Drop.Serial == 1)
					{
						R.ForcedLandAt = Drop.LandAt - H->GetNow() + T; // (step time)
					}
				}
				if (Target.IsValid() && R.ForcedLandAt > 0.f && T > R.ForcedLandAt && T < R.ForcedLandAt + 0.6f)
				{
					R.TargetMoved = FMath::Max(R.TargetMoved, static_cast<float>(FVector::Dist(Target->GetActorLocation(), R.TargetStart)));
				}
				R.MaxRings = FMath::Max(R.MaxRings, H->GetShownRings());
				if (!R.bShot && T > 2.2f && H->GetShownRings() >= 1)
				{
					R.bShot = true;
					Shot(TEXT("98_hazard_rain_markers"));
				}
			},
			[this, Run, RestoreCamera]
			{
				FRainRun& R = *Run;
				const APinguRoundHazards* H = APinguRoundHazards::Get(GetWorld());
				const int32 Drops = H ? H->GetRainCount() : 0;
				APinguRoundHazards::ClearAll(GetWorld());
				RestoreCamera();
				ClearAbilityLeftovers();
				Report(FString::Printf(TEXT("RESULT hazard rain: %d drops in 7.5 s (want >= 4), %d snowballs, %d of them somebody's (want 0), rings at most %d at once %s"),
					Drops, R.Balls0, R.NotNobodys, R.MaxRings, Ok(Drops >= 4 && R.Balls0 >= 3 && R.NotNobodys == 0 && R.MaxRings >= 1)));
				Report(FString::Printf(TEXT("RESULT hazard rain: %d of %d landed snowballs came down within 90 cm of their ring %s"), R.Near, R.Landed,
					Ok(R.Landed >= 3 && R.Near >= R.Landed - 1)));
				Report(FString::Printf(TEXT("RESULT hazard rain: the drop forced onto the dummy moved it %.0f cm (want > 60) %s"), R.TargetMoved, Ok(R.TargetMoved > 60.f)));
				ResetTarget(FVector(FlatIslandX + 400.f, 0.f, 62.f));
			});
	}

	// ---- look_HazardWind: the warning arrow and the streaks on the arena, with the HUD.
	{
		const TSharedRef<FWindRun> Run = MakeShared<FWindRun>();
		AddStep(TEXT("look_HazardWind"), 3.f,
			[this, Run]
			{
				*Run = FWindRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				ABlockIsland* Island = MakeArena(GetWorld());
				if (!Island)
				{
					return;
				}
				Teleport(StartPlace(Island, 1));
				ResetTarget(StartPlace(Island, 2));
				LookCamera = FrameIsland(GetWorld(), Island);
				APinguRoundHazards::StartForTest(GetWorld(), EPinguTrait::Wind, 2.5f);
			},
			[this, Run](float T)
			{
				FWindRun& R = *Run;
				if (!R.bShotWarn && T >= 1.f)
				{
					R.bShotWarn = true;
					Shot(TEXT("98_hazard_wind_warning"), true);
				}
				if (!R.bShotGust && T >= 2.3f)
				{
					R.bShotGust = true;
					Shot(TEXT("98_hazard_wind_gust"), true);
				}
			},
			[this, Run, RestoreCamera]
			{
				APinguRoundHazards::ClearAll(GetWorld());
				RestoreCamera();
				Report(FString::Printf(TEXT("RESULT hazard wind look: 2 pictures (98_hazard_wind_warning / _gust): %s"), Ok(Run->bShotWarn && Run->bShotGust)));
				ResetTarget(FVector(FlatIslandX + 400.f, 0.f, 62.f));
				Teleport(FVector(FlatIslandX - 300.f, 0.f, 62.f));
			});
	}
}

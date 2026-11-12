// Pingu - the build limit (PinguBuildLimit, 2026-11-05, card build-limit-snowman). The autotest's steps (APenguinAutoTest::
// AddBuildLimitSteps; filter buildlimit_ + spawn_target, the picture look_BuildLimitPuff). They turn the main island into a
// four-penguin arena (only arenas and the lobby island have a limit), so they come last in the script.
//
//  buildlimit_Place    every placer against the limit, the uses straight on the host's ability component: an ice ledge
//                      reaching over the top line refused, one under it built; 7 columns out at either side refused, 5 out
//                      built; far below the island built; an Iceberg Drop's floe 7 out refused, 3 out built; a snowman on a
//                      tower whose top would poke over the line refused. Every refusal: nothing placed, no cooldown, one "no"
//                      puff.
//  look_BuildLimitPuff the "no" puff of an ice wall aimed over the top line (96_buildlimit_no_puff_*).
//  buildlimit_Line     the top line against the stage camera's real view of the fresh arena (96_buildlimit_line).
//  buildlimit_Bots     a bot with Ice Wall, Iceberg Drop and Flap dropped in above the top line beside the island, then fighting
//                      us for 10 s: no block of it above the line or past the sides ever, and no refused try (it asks first);
//                      the top line against the stage camera's real view (96_buildlimit_bots).

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinBot.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguBuildLimit.h"
#include "PinguSnowman.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"

namespace PenguinAutoTestBuildLimitPrivate
{
	/** The arena: four penguins, this seed. */
	constexpr int32 ArenaSeed = 4711;
	constexpr int32 ArenaPenguins = 4;

	static const TCHAR* Ok(bool b) { return b ? TEXT("ok") : TEXT("FAIL"); }

	/** One use against the limit. */
	struct FCase
	{
		const TCHAR* Name;
		EPenguinAbility Ability;
		bool bWantPlaced;
	};

	struct FPlaceRun
	{
		int32 Done = 0;
		int32 Passed = 0;
		TArray<FString> Lines;
		FPinguBuildLimit Limit;
		bool bLimit = false;
	};

	struct FBotRun
	{
		FPinguBuildLimit Limit;
		bool bLimit = false;
		int32 RefusalsAtStart = 0;
		int32 AboveTop = 0;
		int32 PastSides = 0;
		int32 BotIce = 0;
		int32 Samples = 0;
		int32 Shots = 0;
		float ViewTop = 0.f;
		float Aspect = 0.f;
		bool bFollowing = false;
		bool bView = false;
		int32 BlocksAtStart = 0;
	};

	/** The main island as a fresh four-penguin arena; its limit. */
	static ABlockIsland* MakeArena(UWorld* World, FPinguBuildLimit& OutLimit, bool& bOutLimit)
	{
		ABlockIsland* Island = ABlockIsland::Get(World);
		if (Island)
		{
			Island->ArenaPenguins = ArenaPenguins;
			Island->RegenerateArena(ArenaSeed);
			bOutLimit = PinguBuildLimit::Get(Island, OutLimit);
		}
		return Island;
	}

	/** The middle of the island's column (world X). */
	static float ColumnX(const ABlockIsland* Island, int32 Column)
	{
		return static_cast<float>(Island->GetActorLocation().X) + (Column + 0.5f) * Island->CellSize;
	}

	/** Blocks above the line, and blocks past the sides (in the island's rows), in the columns around the island. */
	static void CountPast(const ABlockIsland* Island, const FPinguBuildLimit& Limit, int32& OutAbove, int32& OutSides)
	{
		OutAbove = 0;
		OutSides = 0;
		const float OriginZ = static_cast<float>(Island->GetActorLocation().Z);
		const int32 TopRow = FMath::FloorToInt((Limit.TopZ - OriginZ) / Island->CellSize); // (its top edge is over the line)
		for (int32 Column = Limit.MinColumn - 12; Column <= Limit.MaxColumn + 12; ++Column)
		{
			for (int32 Row = TopRow - 40; Row <= TopRow + 25; ++Row)
			{
				if (!Island->IsSolidCell(FIntPoint(Column, Row)))
				{
					continue;
				}
				OutAbove += Row >= TopRow ? 1 : 0;
				OutSides += Column < Limit.MinColumn || Column > Limit.MaxColumn ? 1 : 0;
			}
		}
	}
}

void APenguinAutoTest::AddBuildLimitSteps()
{
	using namespace PenguinAutoTestBuildLimitPrivate;

	const auto Restore = [this]()
	{
		VirtualInput = FVector2D::ZeroVector;
		ClearAbilityLeftovers();
		if (Target.IsValid())
		{
			if (Target->GetBot())
			{
				Target->GetBot()->SetBotEnabled(false);
			}
			if (SavedTargetAbilities.Num() > 0)
			{
				Target->GetAbilities()->SetAbilities(SavedTargetAbilities);
				SavedTargetAbilities.Reset();
			}
		}
		if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
		{
			Stage->SetActorTickEnabled(true);
			Stage->SnapToTargets();
		}
		LookCamera.Reset();
		if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
		{
			Mode->SetActorTickEnabled(true);
		}
	};

	// ---- buildlimit_Place: each case at its own time (an ice wall needs 0.4 s before the next: 0.5 s apart), on a fresh arena.
	{
		static const FCase Cases[] = {
			{ TEXT("ice ledge over the top line"), EPenguinAbility::IceWall, false },
			{ TEXT("ice ledge under the top line"), EPenguinAbility::IceWall, true },
			{ TEXT("ice ledge 7 columns out right"), EPenguinAbility::IceWall, false },
			{ TEXT("ice ledge 5 columns out right"), EPenguinAbility::IceWall, true },
			{ TEXT("ice ledge 7 columns out left"), EPenguinAbility::IceWall, false },
			{ TEXT("ice ledge 5 columns out left"), EPenguinAbility::IceWall, true },
			{ TEXT("ice ledge far below the island"), EPenguinAbility::IceWall, true },
			{ TEXT("iceberg floe 7 columns out"), EPenguinAbility::IcebergDrop, false },
			{ TEXT("iceberg floe 3 columns out"), EPenguinAbility::IcebergDrop, true },
			{ TEXT("snowman on a tower up to the line"), EPenguinAbility::Snowman, false },
		};
		constexpr int32 NumCases = UE_ARRAY_COUNT(Cases);
		const TSharedRef<FPlaceRun> Run = MakeShared<FPlaceRun>();
		AddStep(TEXT("buildlimit_Place"), 0.3f + 0.5f * NumCases,
			[this, Run]
			{
				*Run = FPlaceRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
				{
					Mode->SetActorTickEnabled(false); // (no practice respawn, no sky drops)
				}
				MakeArena(GetWorld(), Run->Limit, Run->bLimit);
			},
			[this, Run](float T)
			{
				FPlaceRun& R = *Run;
				if (R.Done >= NumCases || T < 0.3f + 0.5f * R.Done)
				{
					return;
				}
				const FCase& Case = Cases[R.Done];
				++R.Done;
				FPinguBuildLimit& L = R.Limit;
				ABlockIsland* Island = MakeArena(GetWorld(), L, R.bLimit); // (fresh for every case)
				if (!Island || !R.bLimit)
				{
					R.Lines.Add(FString::Printf(TEXT("%s: no island or no limit FAIL"), Case.Name));
					return;
				}
				FBox Bounds;
				float GroundMinZ = 0.f;
				float GroundMaxZ = 0.f;
				Island->GetBaseFrame(Bounds, GroundMinZ, GroundMaxZ);
				const int32 FirstColumn = L.MinColumn + PinguBuildLimit::SideCells;
				const int32 LastColumn = L.MaxColumn - PinguBuildLimit::SideCells;
				const float OriginZ = static_cast<float>(Island->GetActorLocation().Z);
				const float Cell = Island->CellSize;
				const float MidZ = static_cast<float>(Bounds.Max.Z) - 250.f; // (beside the island, in the picture)
				FVector At = FVector::ZeroVector;
				FVector Aim = FVector::ZeroVector;
				switch (R.Done - 1)
				{
				case 0: At = FVector(ColumnX(Island, LastColumn + 2), 0.f, L.TopZ - 40.f); Aim = At + FVector(0.f, 0.f, 100.f); break;
				case 1: At = FVector(ColumnX(Island, LastColumn + 2), 0.f, L.TopZ - 260.f); Aim = At + FVector(0.f, 0.f, 100.f); break;
				case 2: At = FVector(ColumnX(Island, LastColumn + 7), 0.f, MidZ); Aim = At; break;
				case 3: At = FVector(ColumnX(Island, LastColumn + 5), 0.f, MidZ); Aim = At; break;
				case 4: At = FVector(ColumnX(Island, FirstColumn - 7), 0.f, MidZ); Aim = At; break;
				case 5: At = FVector(ColumnX(Island, FirstColumn - 5), 0.f, MidZ); Aim = At; break;
				case 6:
					At = FVector(ColumnX(Island, LastColumn + 3), 0.f, FMath::Max(static_cast<float>(Bounds.Min.Z) - 300.f, Island->SeaLevel + 500.f));
					Aim = At;
					break;
				case 7: At = FVector(ColumnX(Island, LastColumn + 7) + Cell * 0.5f, 0.f, MidZ); Aim = At; break;
				case 8: At = FVector(ColumnX(Island, LastColumn + 3) + Cell * 0.5f, 0.f, MidZ); Aim = At; break;
				default:
				{
					// A two-column tower on the island's middle ground, its top 40 - 140 cm under the line: a snowman (150 cm) on
					// it would poke over.
					const TArray<FVector> Starts = Island->GetSpreadSpawnLocations(ArenaPenguins);
					const FVector Start = Starts.Num() > 1 ? Starts[1] : Bounds.GetCenter();
					float GroundZ = static_cast<float>(Start.Z) - 62.f;
					Island->FindGroundBelow(Start, 400.f, GroundZ);
					const FIntPoint Ground = Island->WorldToCell(FVector(Start.X, 0.f, GroundZ - Cell * 0.5f));
					const int32 TopRow = FMath::FloorToInt((L.TopZ - 40.f - OriginZ) / Cell) - 1;
					TArray<FIntPoint> Tower;
					for (int32 Row = Ground.Y + 1; Row <= TopRow; ++Row)
					{
						Tower.Add(FIntPoint(Ground.X, Row));
						Tower.Add(FIntPoint(Ground.X + 1, Row));
					}
					Island->AddBlocks(Tower, EBlockKind::Snow, 0.f);
					Island->FlushEdits();
					At = FVector(ColumnX(Island, Ground.X), 0.f, OriginZ + (TopRow + 1) * Cell + 62.f);
					Aim = At + FVector(400.f, 0.f, 0.f);
					break;
				}
				}
				Teleport(At);
				Penguin->ResetForRound(At); // (alive again: the case before may have dropped it into the sea)
				Penguin->SetInputLocked(false);
				UPenguinAbilityComponent* Abilities = Penguin->GetAbilities();
				Abilities->SetAbilities({ Case.Ability });
				const int32 Blocks = Island->GetNumBlocks();
				TArray<APinguSnowman*> Snowmen;
				APinguSnowman::GetAll(GetWorld(), Snowmen);
				const int32 SnowmenBefore = Snowmen.Num();
				int32 Bergs = 0;
				for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
				{
					++Bergs;
				}
				const int32 RefusalsBefore = PinguBuildLimit::GetRefusals();
				Penguin->SetAimPoint(Aim);
				const bool bUsed = Abilities->UseAbility(0, Aim);
				const int32 Puffs = PinguBuildLimit::GetRefusals() - RefusalsBefore;
				APinguSnowman::GetAll(GetWorld(), Snowmen);
				int32 BergsAfter = 0;
				for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
				{
					++BergsAfter;
				}
				const int32 Placed = (Island->GetNumBlocks() - Blocks) + (Snowmen.Num() - SnowmenBefore) + (BergsAfter - Bergs);
				const float Cooldown = Abilities->GetCooldownRemaining(0);
				const bool bOk = Case.bWantPlaced ? (bUsed && Placed > 0 && Puffs == 0 && Cooldown > 0.f) : (!bUsed && Placed == 0 && Puffs == 1 && Cooldown <= 0.f);
				R.Passed += bOk ? 1 : 0;
				R.Lines.Add(FString::Printf(TEXT("%s at (%.0f, %.0f): used %d, placed %d, \"no\" puffs %d, cooldown %.1f s (want %s) %s"), Case.Name, At.X, At.Z,
					bUsed ? 1 : 0, Placed, Puffs, Cooldown, Case.bWantPlaced ? TEXT("placed") : TEXT("refused, nothing spent"), Ok(bOk)));
				ClearAbilityLeftovers();
			},
			[this, Run, Restore]
			{
				const FPlaceRun& R = *Run;
				for (const FString& Line : R.Lines)
				{
					Report(FString::Printf(TEXT("  build limit: %s"), *Line));
				}
				Report(FString::Printf(TEXT("RESULT build limit placing: the limit columns %d .. %d (the island %d .. %d), top line z %.0f; %d of %d cases right: %s"),
					R.Limit.MinColumn, R.Limit.MaxColumn, R.Limit.MinColumn + PinguBuildLimit::SideCells, R.Limit.MaxColumn - PinguBuildLimit::SideCells, R.Limit.TopZ,
					R.Passed, NumCases, Ok(R.bLimit && R.Passed == NumCases && R.Done == NumCases)));
				Restore();
			});
	}

	// ---- look_BuildLimitPuff: an ice wall aimed over the top line beside the island: refused, the puff just under the line.
	{
		struct FPuff
		{
			float At = -1.f;
			int32 Shots = 0;
			FVector Puff = FVector::ZeroVector;
			bool bRefused = false;
		};
		const TSharedRef<FPuff> Puff = MakeShared<FPuff>();
		AddStep(TEXT("look_BuildLimitPuff"), 1.f,
			[this, Puff]
			{
				*Puff = FPuff();
				SpawnTarget();
				ClearAbilityLeftovers();
				if (AGameModeBase* Mode = GetWorld()->GetAuthGameMode())
				{
					Mode->SetActorTickEnabled(false);
				}
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(false);
					LookCamera = Stage;
				}
			},
			[this, Puff](float T)
			{
				FPuff& P = *Puff;
				if (P.At < 0.f && T > 0.2f)
				{
					P.At = T;
					FPinguBuildLimit L;
					bool bLimit = false;
					ABlockIsland* Island = MakeArena(GetWorld(), L, bLimit);
					if (!Island || !bLimit)
					{
						return;
					}
					const FVector At(ColumnX(Island, L.MaxColumn - PinguBuildLimit::SideCells + 2), 0.f, L.TopZ - 40.f);
					Teleport(At);
					Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::IceWall });
					const int32 Before = PinguBuildLimit::GetRefusals();
					P.bRefused = !Penguin->GetAbilities()->UseAbility(0, At + FVector(80.f, 0.f, 120.f)) && PinguBuildLimit::GetRefusals() == Before + 1;
					P.Puff = PinguBuildLimit::GetLastRefusedAt();
				}
				if (AActor* Camera = LookCamera.Get(); Camera && P.At > 0.f)
				{
					const FVector From = P.Puff + FVector(0.f, 520.f, 20.f);
					Camera->SetActorLocationAndRotation(From, (P.Puff - From).Rotation());
				}
				static const float ShotAt[] = { 0.04f, 0.12f };
				if (P.At > 0.f && P.Shots < 2 && T > P.At + ShotAt[P.Shots])
				{
					Shot(*FString::Printf(TEXT("96_buildlimit_no_puff_%d"), P.Shots + 1));
					++P.Shots;
				}
			},
			[this, Puff, Restore]
			{
				const FPuff& P = *Puff;
				Report(FString::Printf(TEXT("RESULT build limit puff: refused with a puff %d at (%.0f, %.0f), %d of 2 pictures: %s"), P.bRefused ? 1 : 0, P.Puff.X, P.Puff.Z, P.Shots,
					Ok(P.bRefused && P.Shots == 2)));
				Restore();
			});
	}

	// ---- buildlimit_Line: the top line against the stage camera's real view of a fresh arena (both penguins standing on it, nothing
	// built: the camera frames the island alone). A measurement on a 16:9 screen: within 60 cm.
	{
		struct FLine
		{
			FPinguBuildLimit Limit;
			bool bLimit = false;
			bool bMeasured = false;
			bool bView = false;
			bool bFollowing = false;
			float ViewTop = 0.f;
			float Aspect = 0.f;
			float Distance = 0.f;
		};
		const TSharedRef<FLine> Line = MakeShared<FLine>();
		AddStep(TEXT("buildlimit_Line"), 1.8f,
			[this, Line]
			{
				FLine& L = *Line;
				L = FLine();
				SpawnTarget();
				ClearAbilityLeftovers();
				ABlockIsland* Island = MakeArena(GetWorld(), L.Limit, L.bLimit);
				if (!Island)
				{
					return;
				}
				const TArray<FVector> Starts = Island->GetSpreadSpawnLocations(ArenaPenguins);
				if (Starts.Num() >= 3)
				{
					Teleport(Starts[1]);
					Penguin->ResetForRound(Starts[1]);
					ResetTarget(Starts[2]);
				}
				Penguin->SetInputLocked(false);
				VirtualInput = FVector2D::ZeroVector;
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(true);
					Stage->SnapToTargets(0.3f);
				}
			},
			[this, Line](float T)
			{
				FLine& L = *Line;
				const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld());
				if (L.bMeasured || T < 1.5f || !Stage)
				{
					return;
				}
				L.bMeasured = true;
				FBox2D View;
				L.bView = Stage->GetViewRect(View);
				L.ViewTop = L.bView ? static_cast<float>(View.Max.Y) : 0.f;
				L.bFollowing = Stage->IsFollowing();
				L.Distance = Stage->GetFrameDistance();
				if (GEngine && GEngine->GameViewport)
				{
					FVector2D Size;
					GEngine->GameViewport->GetViewportSize(Size);
					L.Aspect = Size.Y > 1.f ? static_cast<float>(Size.X / Size.Y) : 0.f;
				}
				Shot(TEXT("96_buildlimit_line"));
			},
			[this, Line, Restore]
			{
				const FLine& L = *Line;
				const bool bCompare = L.bView && FMath::Abs(L.Aspect - 16.f / 9.f) < 0.03f;
				const bool bOk = L.bLimit && L.bMeasured && (!bCompare || FMath::Abs(L.ViewTop - L.Limit.TopZ) < 60.f);
				Report(FString::Printf(TEXT("RESULT build limit line: the top line z %.0f, the camera's view top %.0f (%.0f cm out, following %d, screen %.2f, compared %d; want within 60 cm on 16:9): %s"),
					L.Limit.TopZ, L.ViewTop, L.Distance, L.bFollowing ? 1 : 0, L.Aspect, bCompare ? 1 : 0, Ok(bOk)));
				Restore();
			});
	}

	// ---- buildlimit_Bots: a bot (Ice Wall, Iceberg Drop, Flap) dropped in 3 m above the top line two columns beside the island
	// (its way back would be ice under its feet: above the line, so not tried), then fighting us on the arena. Every 0.25 s: blocks
	// above the line or past the sides (want none), refused tries (want none). At the end the stage camera's real view against the
	// line, and a picture of the round.
	{
		const TSharedRef<FBotRun> Run = MakeShared<FBotRun>();
		AddStep(TEXT("buildlimit_Bots"), 11.f,
			[this, Run]
			{
				FBotRun& R = *Run;
				R = FBotRun();
				SpawnTarget();
				ClearAbilityLeftovers();
				ABlockIsland* Island = MakeArena(GetWorld(), R.Limit, R.bLimit);
				if (!Island || !R.bLimit || !Target.IsValid() || !Target->GetBot())
				{
					return;
				}
				R.BlocksAtStart = Island->GetNumBlocks();
				const TArray<FVector> Starts = Island->GetSpreadSpawnLocations(ArenaPenguins);
				if (Starts.Num() > 1)
				{
					Teleport(Starts[1]);
				}
				Penguin->ResetForRound(Penguin->GetActorLocation());
				Penguin->SetInputLocked(false);
				Penguin->SetTeam(-1);
				Penguin->GetAbilities()->SetAbilities({ EPenguinAbility::Snowball });
				SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
				Target->SetTeam(-1);
				Target->ResetForRound(FVector(ColumnX(Island, R.Limit.MaxColumn - PinguBuildLimit::SideCells + 2), 0.f, R.Limit.TopZ + 300.f));
				Target->GetAbilities()->SetAbilities({ EPenguinAbility::IceWall, EPenguinAbility::IcebergDrop, EPenguinAbility::Flap });
				Target->GetBot()->SetBotEnabled(true, 2);
				R.RefusalsAtStart = PinguBuildLimit::GetRefusals();
				if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
				{
					Stage->SetActorTickEnabled(true);
					Stage->SnapToTargets(0.3f);
				}
			},
			[this, Run](float T)
			{
				FBotRun& R = *Run;
				const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
				if (!Island || !R.bLimit)
				{
					return;
				}
				VirtualInput = FVector2D::ZeroVector; // (we stand still: the bot comes to us)
				if (static_cast<int32>(T * 4.f) > R.Samples)
				{
					R.Samples = static_cast<int32>(T * 4.f);
					int32 Above = 0;
					int32 Sides = 0;
					CountPast(Island, R.Limit, Above, Sides);
					R.AboveTop = FMath::Max(R.AboveTop, Above);
					R.PastSides = FMath::Max(R.PastSides, Sides);
					R.BotIce = FMath::Max(R.BotIce, Island->GetNumBlocks() - R.BlocksAtStart);
				}
				if (R.Shots == 0 && T > 10.5f)
				{
					++R.Shots;
					Shot(TEXT("96_buildlimit_bots"));
					// The real view (this machine's viewport) against the line.
					if (const APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
					{
						FBox2D View;
						R.bView = Stage->GetViewRect(View);
						R.ViewTop = R.bView ? static_cast<float>(View.Max.Y) : 0.f;
						R.bFollowing = Stage->IsFollowing();
						if (GEngine && GEngine->GameViewport)
						{
							FVector2D Size;
							GEngine->GameViewport->GetViewportSize(Size);
							R.Aspect = Size.Y > 1.f ? static_cast<float>(Size.X / Size.Y) : 0.f;
						}
					}
				}
			},
			[this, Run, Restore]
			{
				const FBotRun& R = *Run;
				const int32 Refused = PinguBuildLimit::GetRefusals() - R.RefusalsAtStart;
				// (The line is worked out for a 16:9 screen; another window shape shows more or less: then only reported.)
				const bool bCompare = R.bView && !R.bFollowing && FMath::Abs(R.Aspect - 16.f / 9.f) < 0.03f;
				const bool bLineOk = !bCompare || FMath::Abs(R.ViewTop - R.Limit.TopZ) < 60.f;
				const bool bOk = R.bLimit && R.AboveTop == 0 && R.PastSides == 0 && Refused == 0 && R.Shots == 1 && bLineOk;
				Report(FString::Printf(TEXT("RESULT build limit bots: blocks above the line at most %d, past the sides %d (want 0, 0), refused tries %d (want 0: it asks first), the bot's new blocks %d; the top line z %.0f, the camera's view top %.0f (screen %.2f, following %d, compared %d, want within 60 cm): %s"),
					R.AboveTop, R.PastSides, Refused, R.BotIce, R.Limit.TopZ, R.ViewTop, R.Aspect, R.bFollowing ? 1 : 0, bCompare ? 1 : 0, Ok(bOk)));
				Restore();
			});
	}
}

// Pingu - the arena's ends step down to the sea (card 2026-11-05_map-edges). The autotest's steps
// (APenguinAutoTest::AddMapEdgeSteps; filter mapedges_ + spawn_target, look_MapEdges for the pictures):
//
//  mapedges_Stats   50 seeds, each made twice: the old arenas (ABlockIsland::EndStepChanceOverride 0) and the game's own. Counts the ends
//                   (two per island) that step down - from the outermost column in, four columns each at most one block higher than
//                   the one before it, the fourth at least one higher than the outermost - and the islands with at least one such
//                   end. Wants at least 55 % of the ends and 80 % of the islands with the new arenas, clearly more than before. Also:
//                   every island has its four spawn points on both settings, and the width (the build limit's span) is the same.
//  look_MapEdges    97_mapedges_<seed>_old / _new: four seeds, the same seed before and after.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "Engine/World.h"

namespace PenguinAutoTestMapEdgesPrivate
{
	constexpr int32 NumSeeds = 50;
	constexpr int32 SeedBase = 500;
	constexpr int32 Look = 4;
	constexpr int32 LookSeeds[Look] = { 503, 507, 512, 526 };

	/** The ground of a column: from its lowest block up while solid (floating platforms are not ground). -100 when empty. */
	static int32 GroundOf(const ABlockIsland* Island, int32 Column)
	{
		int32 Lowest = 100;
		for (int32 Z = -14; Z <= 24; ++Z)
		{
			if (Island->IsSolidCell(FIntPoint(Column, Z)))
			{
				Lowest = Z;
				break;
			}
		}
		if (Lowest == 100)
		{
			return -100;
		}
		int32 Top = Lowest;
		while (Island->IsSolidCell(FIntPoint(Column, Top + 1)))
		{
			++Top;
		}
		return Top;
	}

	/** The outermost columns of the island (MinX, MaxX), false when it is empty. */
	static bool Extent(const ABlockIsland* Island, int32& MinX, int32& MaxX)
	{
		MinX = 1000;
		MaxX = -1000;
		for (int32 X = -45; X <= 45; ++X)
		{
			if (GroundOf(Island, X) > -100)
			{
				MinX = FMath::Min(MinX, X);
				MaxX = FMath::Max(MaxX, X);
			}
		}
		return MaxX >= MinX;
	}

	/** Does the end (Side 0: left, 1: right) step down: the ground of the outermost four columns, each at most one block lower
	 *  than the next one in (never higher), and the outermost at least one lower than the fourth. */
	static bool StepsDown(const ABlockIsland* Island, int32 MinX, int32 MaxX, int32 Side)
	{
		int32 G[4];
		for (int32 Depth = 0; Depth < 4; ++Depth)
		{
			G[Depth] = GroundOf(Island, Side == 0 ? MinX + Depth : MaxX - Depth);
			if (G[Depth] <= -100)
			{
				return false;
			}
		}
		for (int32 Depth = 0; Depth < 3; ++Depth)
		{
			const int32 Rise = G[Depth + 1] - G[Depth];
			if (Rise < 0 || Rise > 1)
			{
				return false;
			}
		}
		return G[3] - G[0] >= 1;
	}

	struct FEdgeStats
	{
		bool bSaved = false;
		EIslandLayout Layout = EIslandLayout::MovementTest;
		int32 Seed = 0;
		int32 Penguins = 0;
		int32 Done = 0;
		int32 Ends[2] = { 0, 0 };    // stepping ends: old, new
		int32 Islands[2] = { 0, 0 }; // islands with at least one stepping end
		int32 SpawnBad = 0;
		int32 WidthBad = 0;
	};

	struct FEdgeLook
	{
		bool bSaved = false;
		EIslandLayout Layout = EIslandLayout::MovementTest;
		int32 Seed = 0;
		int32 Penguins = 0;
		int32 Next = 0;
	};
}

void APenguinAutoTest::AddMapEdgeSteps()
{
	using namespace PenguinAutoTestMapEdgesPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	const TSharedRef<FEdgeStats> Stats = MakeShared<FEdgeStats>();
	const TSharedRef<FEdgeLook> LookRun = MakeShared<FEdgeLook>();

	AddStep(TEXT("mapedges_Stats"), 2.5f,
		[this, Stats, Base]
		{
			*Stats = FEdgeStats();
			SpawnTarget();
			Teleport(Base);
			ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
			{
				Stats->bSaved = true;
				Stats->Layout = Island->Layout;
				Stats->Seed = Island->Seed;
				Stats->Penguins = Island->ArenaPenguins;
			}
		},
		[this, Stats](float)
		{
			ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			if (!Island || !Stats->bSaved || Stats->Done >= NumSeeds)
			{
				return;
			}
			const int32 Seed = SeedBase + Stats->Done;
			Island->ArenaPenguins = 2 + Stats->Done % 9; // every size from two to ten penguins
			int32 Width[2] = { 0, 0 };
			int32 Spawns[2] = { 0, 0 };
			for (int32 Mode = 0; Mode < 2; ++Mode)
			{
				ABlockIsland::EndStepChanceOverride = Mode == 0 ? 0.f : -1.f;
				Island->RegenerateArena(Seed);
				int32 MinX = 0;
				int32 MaxX = 0;
				if (!Extent(Island, MinX, MaxX))
				{
					++Stats->SpawnBad;
					continue;
				}
				Width[Mode] = MaxX - MinX;
				Spawns[Mode] = Island->GetSpawnLocations().Num();
				const bool bLeft = StepsDown(Island, MinX, MaxX, 0);
				const bool bRight = StepsDown(Island, MinX, MaxX, 1);
				Stats->Ends[Mode] += (bLeft ? 1 : 0) + (bRight ? 1 : 0);
				Stats->Islands[Mode] += (bLeft || bRight) ? 1 : 0;
			}
			ABlockIsland::EndStepChanceOverride = -1.f;
			if (Spawns[1] < 4 || Spawns[0] != Spawns[1])
			{
				++Stats->SpawnBad;
			}
			if (Width[0] != Width[1])
			{
				++Stats->WidthBad;
			}
			++Stats->Done;
		},
		[this, Stats, Base]
		{
			ABlockIsland::EndStepChanceOverride = -1.f;
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()); Island && Stats->bSaved)
			{
				Island->Layout = Stats->Layout;
				Island->Seed = Stats->Seed;
				Island->ArenaPenguins = Stats->Penguins;
				Island->Rebuild();
			}
			ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			const FEdgeStats& S = *Stats;
			const float EndsOld = 100.f * S.Ends[0] / (2.f * NumSeeds);
			const float EndsNew = 100.f * S.Ends[1] / (2.f * NumSeeds);
			const float IslOld = 100.f * S.Islands[0] / NumSeeds;
			const float IslNew = 100.f * S.Islands[1] / NumSeeds;
			const bool bOk = S.bSaved && S.Done == NumSeeds && EndsNew >= 55.f && IslNew >= 80.f && S.Ends[1] > S.Ends[0] + 10 && S.SpawnBad == 0 && S.WidthBad == 0;
			Report(FString::Printf(TEXT("RESULT map edges over %d seeds: ends stepping down old %d of %d (%.0f %%), new %d (%.0f %%, want 55 or more); islands with a stepping end old %.0f %%, new %.0f %% (want 80 or more); "
				"islands with fewer than four spawn points or other spawns than before %d, with another width %d (want 0): %s"),
				S.Done, S.Ends[0], 2 * NumSeeds, EndsOld, S.Ends[1], EndsNew, IslOld, IslNew, S.SpawnBad, S.WidthBad, bOk ? TEXT("ok") : TEXT("FAIL")));
		});

	AddStep(TEXT("look_MapEdges"), 6.f,
		[this, LookRun, Base]
		{
			*LookRun = FEdgeLook();
			SpawnTarget();
			Teleport(Base);
			ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
			{
				LookRun->bSaved = true;
				LookRun->Layout = Island->Layout;
				LookRun->Seed = Island->Seed;
				LookRun->Penguins = Island->ArenaPenguins;
				Island->ArenaPenguins = 4;
			}
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		},
		[this, LookRun](float T)
		{
			ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			if (!Island || !LookRun->bSaved)
			{
				return;
			}
			// Every 0.35 s one action per island: made old, shot old, made new, shot new.
			const int32 Tick = FMath::FloorToInt(T / 0.35f);
			if (Tick < LookRun->Next || LookRun->Next >= Look * 4)
			{
				return;
			}
			const int32 Index = LookRun->Next / 4;
			const int32 Part = LookRun->Next % 4;
			++LookRun->Next;
			if (Part == 0 || Part == 2)
			{
				ABlockIsland::EndStepChanceOverride = Part == 0 ? 0.f : -1.f;
				Island->RegenerateArena(LookSeeds[Index]);
				ABlockIsland::EndStepChanceOverride = -1.f;
				if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
				{
					const FBox Bounds = Island->GetBounds();
					const FVector Focus = Bounds.GetCenter();
					const float Distance = static_cast<float>(Bounds.Max.X - Bounds.Min.X) * 2.3f + 600.f;
					const FVector From = Focus + FVector(0.f, Distance, Distance * 0.05f);
					Stage->SetActorLocationAndRotation(From, (Focus - From).Rotation());
				}
			}
			else
			{
				Shot(*FString::Printf(TEXT("97_mapedges_%d_%s"), LookSeeds[Index], Part == 1 ? TEXT("old") : TEXT("new")));
			}
		},
		[this, LookRun, Base]
		{
			ABlockIsland::EndStepChanceOverride = -1.f;
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()); Island && LookRun->bSaved)
			{
				Island->Layout = LookRun->Layout;
				Island->Seed = LookRun->Seed;
				Island->ArenaPenguins = LookRun->Penguins;
				Island->Rebuild();
			}
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			Report(FString::Printf(TEXT("RESULT map edges look: %d of %d pictures taken (97_mapedges_*): %s"),
				LookRun->Next, Look * 4, LookRun->Next >= Look * 4 ? TEXT("ok") : TEXT("FAIL")));
		});
}

// Pingu - the island's blocks keep their look when the island changes: the autotest's step (APenguinAutoTest::AddIslandLooksSteps).
//
// island_StableLooks: a ten-penguin arena. Every frame the look of every drawn block instance (its component, the random number its
// material reads for the texture tile and tint, and where it sits) is read with ABlockIsland::GetLookSignatures and compared with the
// frame before: a block that is still there in the same component must look exactly the same. It runs through sudden death (columns
// crumbling away one after the other), an ice block broken out of the middle and a sand block of ice falling and landing. Before,
// every block after a removed one in the component's list got another random number (the engine draws them by instance index), so the
// textures changed each time a column went. Pictures: 72_island_stable_before / 72_island_stable_after (the same view; the blocks in
// the middle must be the same ones).

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "Engine/World.h"

namespace PenguinAutoTestIslandLooksPrivate
{
	constexpr int32 LooksSeed = 7071;
	/** The components whose blocks move on purpose (crumbling shakes, temporary ice fades): left out of the comparison. */
	constexpr int32 SlotTemp = 4;
	constexpr int32 SlotCrumble = 5;

	struct FLooksRun
	{
		bool bSaved = false;
		EIslandLayout Layout = EIslandLayout::MovementTest;
		int32 Seed = 0;
		int32 Penguins = 0;
		TMap<FIntVector, uint32> Last;
		bool bHaveLast = false;
		int32 Stage = 0;
		float StageAt = 0.f;
		int32 Frames = 0;
		int32 Compared = 0;       // block instances compared with the frame before, summed over all frames
		int32 Changed = 0;        // of those, how many looked different
		int32 NotBuilt = 0;       // frames the visuals were not ready
		int32 BlocksStart = 0;
		int32 BlocksCrumbled = 0; // blocks gone by sudden death
		int32 Phase[3] = { 0, 0, 0 };        // changed per part: crumbling, a block broken out, sand landing
		int32 PhaseRemoved[3] = { 0, 0, 0 }; // block instances that left during that part
		int32 SandBefore = 0;
		bool bSandLanded = false;
		bool bBroken = false;
		FIntPoint SandTarget = FIntPoint::ZeroValue;
		FIntPoint BreakCell = FIntPoint::ZeroValue;
		FString FirstChange;
	};

	/** The highest solid row of a column (-3 when it is empty). */
	static int32 TopSolidRow(const ABlockIsland* Island, int32 Column)
	{
		for (int32 Z = 14; Z >= -2; --Z)
		{
			if (Island->IsSolidCell(FIntPoint(Column, Z)))
			{
				return Z;
			}
		}
		return -3;
	}

	static bool IsSteady(const FIntVector& Key)
	{
		const int32 Slot = Key.Z / 100;
		return Slot != SlotTemp && Slot != SlotCrumble;
	}
}

void APenguinAutoTest::AddIslandLooksSteps()
{
	using namespace PenguinAutoTestIslandLooksPrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	const TSharedRef<FLooksRun> Run = MakeShared<FLooksRun>();

	AddStep(TEXT("island_StableLooks"), 9.f,
		[this, Run, Base]
		{
			*Run = FLooksRun();
			SpawnTarget();
			Teleport(Base);
			ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			if (!Island)
			{
				return;
			}
			Run->bSaved = true;
			Run->Layout = Island->Layout;
			Run->Seed = Island->Seed;
			Run->Penguins = Island->ArenaPenguins;
			Island->ArenaPenguins = PinguMaxPenguins;
			Island->RegenerateArena(LooksSeed);
			Run->BlocksStart = Island->GetNumBlocks();
			// The stage camera held on the island's middle for the two pictures.
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
				const FBox Bounds = Island->GetBounds();
				const FVector Focus = Bounds.GetCenter();
				const float Distance = static_cast<float>(Bounds.Max.X - Bounds.Min.X) * 1.7f;
				const FVector From = Focus + FVector(0.f, Distance, Distance * 0.1f);
				Stage->SetActorLocationAndRotation(From, (Focus - From).Rotation());
			}
		},
		[this, Run](float T)
		{
			FLooksRun& R = *Run;
			ABlockIsland* Island = ABlockIsland::Get(GetWorld());
			if (!R.bSaved || !Island)
			{
				return;
			}
			++R.Frames;
			TMap<FIntVector, uint32> Now;
			if (!Island->GetLookSignatures(Now))
			{
				++R.NotBuilt;
				return;
			}
			if (R.bHaveLast)
			{
				const int32 Part = R.Stage <= 2 ? 0 : (R.Stage <= 3 ? 1 : 2);
				for (const TPair<FIntVector, uint32>& Pair : R.Last)
				{
					if (!IsSteady(Pair.Key))
					{
						continue;
					}
					const uint32* Same = Now.Find(Pair.Key);
					if (!Same)
					{
						++R.PhaseRemoved[Part];
						continue;
					}
					++R.Compared;
					if (*Same != Pair.Value)
					{
						++R.Changed;
						++R.Phase[Part];
						if (R.FirstChange.IsEmpty())
						{
							R.FirstChange = FString::Printf(TEXT("block (%d, %d) in component %d row %d, at %.2f s"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z / 100, Pair.Key.Z % 100, T);
						}
					}
				}
			}
			R.Last = MoveTemp(Now);
			R.bHaveLast = true;

			switch (R.Stage)
			{
			case 0: // a first look, then sudden death: a column every 0.3 s, shaking 0.4 s before it falls
				if (T > 0.5f)
				{
					Shot(TEXT("72_island_stable_before"));
					Island->StartCollapse(0.3f, 0.4f);
					R.Stage = 1;
					R.StageAt = T;
				}
				break;
			case 1: // crumbling
				if (T > R.StageAt + 2.4f)
				{
					Island->StopCollapse();
					R.BlocksCrumbled = R.BlocksStart - Island->GetNumBlocks();
					R.Stage = 2;
					R.StageAt = T;
				}
				break;
			case 2: // let the last shaking blocks settle, then break a block out of the middle of what is left
				if (T > R.StageAt + 0.8f)
				{
					const FBox Bounds = Island->GetBounds();
					const FIntPoint Middle = Island->WorldToCell(Bounds.GetCenter());
					for (int32 Offset = 0; Offset < 90 && !R.bBroken; ++Offset)
					{
						const int32 Column = Middle.X + (Offset % 2 == 0 ? Offset / 2 : -(Offset + 1) / 2);
						const int32 Top = TopSolidRow(Island, Column);
						if (Top > -3)
						{
							R.BreakCell = FIntPoint(Column, Top);
							R.bBroken = Island->BreakBlockAt(Island->CellToWorld(R.BreakCell));
						}
					}
					R.Stage = 3;
					R.StageAt = T;
				}
				break;
			case 3:
				if (T > R.StageAt + 1.f)
				{
					// a block of ice falls out of the air onto the island a few columns on: its landing
					const FBox Bounds = Island->GetBounds();
					const FIntPoint Middle = Island->WorldToCell(Bounds.GetCenter());
					int32 Column = Middle.X + 4;
					int32 TopZ = TopSolidRow(Island, Column);
					for (int32 Step = 1; Step < 60 && TopZ <= -3; ++Step)
					{
						Column = Middle.X + 4 + Step;
						TopZ = TopSolidRow(Island, Column);
					}
					TopZ = FMath::Max(TopZ, -1);
					R.SandTarget = FIntPoint(Column, TopZ + 4);
					R.SandBefore = Island->GetNumBlocks();
					Island->AddSandBlocks({ R.SandTarget }, EBlockKind::Ice);
					R.Stage = 4;
					R.StageAt = T;
				}
				break;
			case 4:
				if (T > R.StageAt + 2.f)
				{
					R.bSandLanded = Island->GetNumBlocks() >= R.SandBefore;
					R.Stage = 5;
					R.StageAt = T;
				}
				break;
			case 5:
				if (T > R.StageAt + 0.3f)
				{
					Shot(TEXT("72_island_stable_after"));
					R.Stage = 6;
				}
				break;
			default:
				break;
			}
		},
		[this, Run, Base]
		{
			const FLooksRun& R = *Run;
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()); Island && R.bSaved)
			{
				Island->StopCollapse();
				Island->ClearTemporaryBlocks();
				Island->Layout = R.Layout;
				Island->Seed = R.Seed;
				Island->ArenaPenguins = R.Penguins;
				Island->Rebuild();
			}
			if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
			{
				Stage->SetActorTickEnabled(true);
				Stage->SnapToTargets();
			}
			LookCamera.Reset();
			ResetTarget(Base + FVector(2400.f, 0.f, 0.f));
			const bool bOk = R.bSaved && R.Stage >= 6 && R.Compared > 5000 && R.Changed == 0 && R.BlocksCrumbled >= 20 && R.bBroken && R.PhaseRemoved[0] > 0 && R.PhaseRemoved[1] > 0;
			Report(FString::Printf(TEXT("RESULT island stable looks: %d frames (%d without built visuals), %d block looks compared with the frame before, %d changed "
				"(crumbling %d, a block broken out %d, sand landing %d; want 0); %d blocks went by sudden death, instances that left the list: crumbling %d, broken %d, sand %d; "
				"broke %d, sand landed %d%s%s: %s"),
				R.Frames, R.NotBuilt, R.Compared, R.Changed, R.Phase[0], R.Phase[1], R.Phase[2], R.BlocksCrumbled, R.PhaseRemoved[0], R.PhaseRemoved[1], R.PhaseRemoved[2],
				R.bBroken ? 1 : 0, R.bSandLanded ? 1 : 0, R.FirstChange.IsEmpty() ? TEXT("") : TEXT("; first change: "), *R.FirstChange, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

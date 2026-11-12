// Pingu - ice that grows with its maker, and the ice wall that is solid at once: the autotest's steps
// (APenguinAutoTest::AddIceScaleSteps).
//
//  ice_WallInstant        the ice wall is solid in the very frame it is cast: its cells are blocks, a trace through it hits the
//                         island's collision, a snowball lying in it breaks on it; the dummy standing in its column is lifted
//                         onto it (alive, never inside a block); its look still rises into place, in 0.35 s (a shot mid-rise:
//                         71_icewall_instant).
//  ice_WallInstantSide    the dummy only reaching into the wall's column from its side is set aside (not lifted), alive.
//  ice_WallSize1/2/4      our penguin at size 1, 2 and 4 raises a wall: 3 / 6 / 12 cells (1 x 3, 2 x 3, 2 x 6), big blocks
//                         0 / 0 / 3 (71_icewall_size1/2/4).
//  ice_IcebergSize1/2/4   its iceberg at those sizes: 2x2 / 3x3 / 2x2 big blocks while it falls (71_iceberg_size*_fall); it
//                         lands as 4 / 9 / 16 cells of sand ice with 0 / 0 / 4 big blocks (71_iceberg_size1/2/4); the dummy
//                         under it is knocked out (reason Iceberg) only once it is pressed onto the floor.
//  ice_BigIcebergOverhang a 4x iceberg landing with its left half on a ledge: the left big blocks stay there, the right ones
//                         fall on as big blocks (one piece each) and land on the floor (16 cells, 4 big blocks).
//  ice_BigBlockSand       a big block of sand falls and lands as one; either of its columns holds it up; with both gone it
//                         falls as one again (and lands a row lower). An ordinary sand block falling into a pit one block deep
//                         lands in it (row -1: it used to fall through the island).
//  ice_BigBlockBreak      breaking one cell of a big block breaks all four (a snowball, a blast, BreakIceInRadius - one block
//                         of the max); the hook or a tornado taking one leaves the other three as ordinary ice.
//  ice_BigBlockTornado    a tornado next to a big block rips its cells out one by one (the big block falls apart), nothing
//                         breaks.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PenguinAutoTestIceScalePrivate
{
	/** The ice on Island in the columns [FromColumn, ToColumn] and rows [FromRow, ToRow]: how many cells, how many big blocks
	 *  (by their bottom-left cell), how many of the cells are sand, and the columns / rows they span. */
	struct FIceCount
	{
		int32 Cells = 0;
		int32 Big = 0;
		int32 Sand = 0;
		int32 MinColumn = TNumericLimits<int32>::Max();
		int32 MaxColumn = TNumericLimits<int32>::Lowest();
		int32 MinRow = TNumericLimits<int32>::Max();
		int32 MaxRow = TNumericLimits<int32>::Lowest();

		int32 Width() const { return Cells > 0 ? MaxColumn - MinColumn + 1 : 0; }
		int32 Height() const { return Cells > 0 ? MaxRow - MinRow + 1 : 0; }
	};

	static FIceCount CountIceCells(const ABlockIsland* Island, int32 FromColumn, int32 ToColumn, int32 FromRow, int32 ToRow)
	{
		FIceCount Count;
		if (!Island)
		{
			return Count;
		}
		for (int32 X = FromColumn; X <= ToColumn; ++X)
		{
			for (int32 Z = FromRow; Z <= ToRow; ++Z)
			{
				const FIntPoint Cell(X, Z);
				if (Island->GetBlock(X, Z) != EBlockKind::Ice)
				{
					continue;
				}
				++Count.Cells;
				Count.Sand += Island->IsSandCell(Cell) ? 1 : 0;
				FIntPoint Origin;
				Count.Big += Island->IsBigBlockCell(Cell, &Origin) && Origin == Cell ? 1 : 0;
				Count.MinColumn = FMath::Min(Count.MinColumn, X);
				Count.MaxColumn = FMath::Max(Count.MaxColumn, X);
				Count.MinRow = FMath::Min(Count.MinRow, Z);
				Count.MaxRow = FMath::Max(Count.MaxRow, Z);
			}
		}
		return Count;
	}

	/** Who is inside a block (its capsule a hair smaller than it is: touching is fine). */
	static bool IsInsideBlock(const ABlockIsland* Island, const APenguinCharacter* Who)
	{
		const UCapsuleComponent* Capsule = Who ? Who->GetCapsuleComponent() : nullptr;
		return Island && Capsule && !Island->CanFitCapsule(Who->GetActorLocation(), Capsule->GetScaledCapsuleRadius() - 3.f, Capsule->GetScaledCapsuleHalfHeight() - 3.f);
	}

	/** A step's own numbers. */
	struct FIceRun
	{
		bool bCast = false;
		bool bUsed = false;
		float CastT = -1.f;
		float SizeAtCast = 0.f;
		int32 Column = 0;
		// ice_WallInstant
		int32 SolidAtCast = 0;
		float ProbeX = -999.f;
		bool bProbeIsland = false;
		bool bSnowballBroken = false;
		float AlphaAtCast = -1.f;
		float AlphaMid = -1.f;
		float RiseDoneAt = -1.f;
		int32 Inside = 0;
		float DummyX0 = 0.f;
		// icebergs
		TWeakObjectPtr<AIcebergDrop> Berg;
		int32 BergCells = 0;
		int32 BergSpan = 0;
		int32 BergLeft = 0;
		float BergBottom = 0.f;
		float LandT = -1.f;
		float KOT = -1.f;
		EPenguinKOReason KOReason = EPenguinKOReason::None;
		float KOFeet = -999.f;
		float KOBergBottom = -999.f;
		int32 FallingBigMost = 0;
		bool bFallShot = false;
		bool bShot = false;
		// big blocks
		FIntPoint Origins[5];
		int32 Checks[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
		int32 BrokenAtStart = 0;
		int32 Stage = 0;
		float StageT = -1.f;
		int32 RippedSeen = 0;
	};
}

void APenguinAutoTest::AddIceScaleSteps()
{
	using namespace PenguinAutoTestIceScalePrivate;
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);

	// The stage camera held still for the pictures (from the camera's side, a little above), and given back after.
	const auto Frame = [this](const FVector& Focus, float Distance)
	{
		if (!LookCamera.IsValid())
		{
			if (APenguinCamera* Stage = APenguinCamera::FindOrSpawn(GetWorld()))
			{
				Stage->SetActorTickEnabled(false);
				LookCamera = Stage;
			}
		}
		if (AActor* Camera = LookCamera.Get())
		{
			const FVector From = Focus + FVector(0.f, Distance, Distance * 0.12f);
			Camera->SetActorLocationAndRotation(From, (Focus - From).Rotation());
		}
	};
	const auto Unframe = [this]
	{
		if (APenguinCamera* Stage = Cast<APenguinCamera>(LookCamera.Get()))
		{
			Stage->SetActorTickEnabled(true);
			Stage->SnapToTargets();
		}
		LookCamera.Reset();
	};
	// Our penguin grown to Size (from its size now; it takes about half a second to get there).
	const auto GrowTo = [this](float Size)
	{
		if (Size > 1.01f && Penguin.IsValid())
		{
			Penguin->ChangeSize(Size / FMath::Max(Penguin->GetSizeTarget(), 0.1f), 30.f);
		}
	};

	// ---- the ice wall is solid the moment it is cast; the dummy standing in its column is lifted onto it.
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(TEXT("ice_WallInstant"), 2.f,
			[this, Run, Base, Frame]
			{
				SpawnTarget();
				PrepareBrawl(250.f, { EPenguinAbility::IceWall });
				*Run = FIceRun();
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (Flat && Target.IsValid())
				{
					Run->Column = Flat->WorldToCell(Target->GetActorLocation()).X;
					// The dummy right in the middle of that column.
					ResetTarget(FVector(Flat->CellToWorld(FIntPoint(Run->Column, 0)).X, 0.f, Base.Z));
				}
				Frame(Base + FVector(250.f, 0.f, 120.f), 1600.f); // (the stage camera sees 35 degrees across: 0.63 x its distance)
			},
			[this, Run, Base](float T)
			{
				FIceRun& R = *Run;
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				APenguinCharacter* Dummy = Target.Get();
				if (!Flat || !Dummy)
				{
					return;
				}
				if (!R.bCast && T > 0.3f)
				{
					R.bCast = true;
					R.CastT = T;
					// A snowball lying in the wall's middle cell (the dummy's: it never hits its own thrower).
					TestBombA = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, Dummy, Flat->CellToWorld(FIntPoint(R.Column, 1)), FVector(0.f, 0.f, -10.f));
					Penguin->SetAimPoint(FVector(Flat->CellToWorld(FIntPoint(R.Column, 0)).X, 0.f, 50.f));
					R.bUsed = Penguin->ActivateAbility(0);
					// The very same frame: three blocks, the island's collision there (a trace from the left hits the wall's face).
					for (int32 Row = 0; Row < 3; ++Row)
					{
						R.SolidAtCast += Flat->IsSolidCell(FIntPoint(R.Column, Row)) ? 1 : 0;
					}
					const float ColumnLeft = static_cast<float>(Flat->CellToWorld(FIntPoint(R.Column, 0)).X) - Flat->CellSize * 0.5f;
					FCollisionQueryParams Params(SCENE_QUERY_STAT(IceWallProbe), false);
					Params.AddIgnoredActor(Penguin.Get());
					Params.AddIgnoredActor(Dummy);
					Params.AddIgnoredActor(TestBombA.Get());
					FHitResult Hit;
					if (GetWorld()->LineTraceSingleByChannel(Hit, FVector(ColumnLeft - 120.f, 0.f, 150.f), FVector(ColumnLeft + 220.f, 0.f, 150.f), ECC_WorldStatic, Params))
					{
						R.ProbeX = static_cast<float>(Hit.ImpactPoint.X) - ColumnLeft;
						R.bProbeIsland = Hit.GetActor() == Flat;
					}
					const APenguinProjectile* Snowball = TestBombA.Get();
					R.bSnowballBroken = !Snowball || Snowball->IsActorBeingDestroyed() || !Snowball->IsFlying();
					R.AlphaAtCast = Flat->GetRiseAlpha(FIntPoint(R.Column, 2));
					return;
				}
				if (!R.bCast)
				{
					return;
				}
				const float Since = T - R.CastT;
				if (Dummy->IsAlive())
				{
					R.Inside += IsInsideBlock(Flat, Dummy) ? 1 : 0;
				}
				if (!R.bShot && Since > 0.12f)
				{
					R.bShot = true;
					R.AlphaMid = Flat->GetRiseAlpha(FIntPoint(R.Column, 2));
					Shot(TEXT("71_icewall_instant"));
				}
				if (R.RiseDoneAt < 0.f && !Flat->IsAnyBlockRising())
				{
					R.RiseDoneAt = Since;
				}
			},
			[this, Run, Unframe]
			{
				const FIceRun& R = *Run;
				const APenguinCharacter* Dummy = Target.Get();
				const float Feet = Dummy ? static_cast<float>(Dummy->GetFeetLocation().Z) : -999.f;
				const bool bAlive = Dummy && Dummy->IsAlive();
				const bool bLifted = Feet > 290.f && Feet < 320.f;
				const bool bOk = R.bUsed && R.SolidAtCast == 3 && FMath::Abs(R.ProbeX) < 3.f && R.bProbeIsland && R.bSnowballBroken && bAlive && bLifted && R.Inside == 0
					&& R.AlphaAtCast < 0.05f && R.AlphaMid > 0.15f && R.AlphaMid < 0.85f && R.RiseDoneAt > 0.3f && R.RiseDoneAt < 0.45f;
				Report(FString::Printf(TEXT("RESULT ice wall instant: used %d, solid in the cast's frame %d of 3 cells, a trace through it hit %s %.1f cm from the wall's face (want 0), the snowball lying in it broken %d; "
					"the dummy in its column alive %d, lifted onto it (feet at %.0f cm, want about 300), frames inside a block %d (want 0); its look rose %.2f at the cast, %.2f in the shot (want 0.15 .. 0.85), all risen after %.2f s (want 0.35): %s"),
					R.bUsed ? 1 : 0, R.SolidAtCast, R.bProbeIsland ? TEXT("the island") : TEXT("NOT the island"), R.ProbeX, R.bSnowballBroken ? 1 : 0, bAlive ? 1 : 0, Feet, R.Inside,
					R.AlphaAtCast, R.AlphaMid, R.RiseDoneAt, bOk ? TEXT("ok") : TEXT("FAIL")));
				Unframe();
			});
	}

	// ---- the dummy only reaching into the wall's column from the side (its middle 15 cm out): set aside, not lifted.
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(TEXT("ice_WallInstantSide"), 1.4f,
			[this, Run, Base]
			{
				SpawnTarget();
				PrepareBrawl(250.f, { EPenguinAbility::IceWall });
				*Run = FIceRun();
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (Flat)
				{
					Run->Column = Flat->WorldToCell(Base + FVector(250.f, 0.f, 0.f)).X;
					const float ColumnLeft = static_cast<float>(Flat->CellToWorld(FIntPoint(Run->Column, 0)).X) - Flat->CellSize * 0.5f;
					ResetTarget(FVector(ColumnLeft - 15.f, 0.f, Base.Z));
					Run->DummyX0 = ColumnLeft - 15.f;
				}
			},
			[this, Run, Base](float T)
			{
				FIceRun& R = *Run;
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				APenguinCharacter* Dummy = Target.Get();
				if (!Flat || !Dummy)
				{
					return;
				}
				if (!R.bCast && T > 0.3f)
				{
					R.bCast = true;
					R.CastT = T;
					Penguin->SetAimPoint(FVector(Flat->CellToWorld(FIntPoint(R.Column, 0)).X, 0.f, 50.f));
					R.bUsed = Penguin->ActivateAbility(0);
					R.SolidAtCast = Flat->IsSolidCell(FIntPoint(R.Column, 0)) ? 1 : 0;
					R.ProbeX = static_cast<float>(Dummy->GetActorLocation().X) - R.DummyX0; // where the push put it (at once)
					return;
				}
				if (R.bCast && Dummy->IsAlive())
				{
					R.Inside += IsInsideBlock(Flat, Dummy) ? 1 : 0;
				}
			},
			[this, Run]
			{
				const FIceRun& R = *Run;
				const APenguinCharacter* Dummy = Target.Get();
				const float Feet = Dummy ? static_cast<float>(Dummy->GetFeetLocation().Z) : -999.f;
				const bool bAlive = Dummy && Dummy->IsAlive();
				const bool bOk = R.bUsed && R.SolidAtCast == 1 && R.ProbeX < -10.f && bAlive && Feet < 40.f && R.Inside == 0;
				Report(FString::Printf(TEXT("RESULT ice wall instant side: used %d, solid at once %d, the dummy (its middle 15 cm beside the column) set %.0f cm aside at once (want 10 or more, to the left), "
					"alive %d, its feet at %.0f cm (want on the floor: not lifted), frames inside a block %d (want 0): %s"),
					R.bUsed ? 1 : 0, R.SolidAtCast, -R.ProbeX, bAlive ? 1 : 0, Feet, R.Inside, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- the wall at our sizes 1, 2 and 4: 1 x 3 cells, 2 x 3 cells, 2 x 6 cells (three big blocks).
	struct FSizeCase
	{
		int32 Size;
		int32 WantCells;
		int32 WantWidth;
		int32 WantHeight;
		int32 WantBig;
	};
	static const FSizeCase WallCases[] = { { 1, 3, 1, 3, 0 }, { 2, 6, 2, 3, 0 }, { 4, 12, 2, 6, 3 } };
	for (const FSizeCase& Case : WallCases)
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(*FString::Printf(TEXT("ice_WallSize%d"), Case.Size), 2.2f,
			[this, Run, Base, Frame, GrowTo, &Case]
			{
				PrepareBrawl(2400.f, { EPenguinAbility::IceWall });
				*Run = FIceRun();
				GrowTo(static_cast<float>(Case.Size));
				Frame(Base + FVector(330.f, 0.f, 60.f + 60.f * Case.Size), 1200.f + 250.f * Case.Size);
			},
			[this, Run, Base, &Case](float T)
			{
				FIceRun& R = *Run;
				if (!R.bCast && T > 1.f)
				{
					R.bCast = true;
					R.CastT = T;
					R.SizeAtCast = Penguin->GetSizeScale();
					Penguin->SetAimPoint(Base + FVector(450.f, 0.f, -12.f)); // the row right on the floor
					R.bUsed = Penguin->ActivateAbility(0);
				}
				if (R.bCast && !R.bShot && T > R.CastT + 0.6f)
				{
					R.bShot = true;
					Shot(*FString::Printf(TEXT("71_icewall_size%d"), Case.Size));
				}
			},
			[this, Run, Base, Unframe, &Case]
			{
				const FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				const int32 Middle = Flat ? Flat->WorldToCell(Base + FVector(450.f, 0.f, 0.f)).X : 0;
				const FIceCount Ice = CountIceCells(Flat, Middle - 4, Middle + 4, -1, 14);
				const bool bOk = R.bUsed && Ice.Cells == Case.WantCells && Ice.Width() == Case.WantWidth && Ice.Height() == Case.WantHeight && Ice.Big == Case.WantBig
					&& Ice.MinRow == 0;
				Report(FString::Printf(TEXT("RESULT ice wall size %d: our size %.2f at the cast, used %d: %d ice cells (want %d), %d wide x %d high (want %d x %d) from row %d (want 0: on the floor), big blocks %d (want %d): %s"),
					Case.Size, R.SizeAtCast, R.bUsed ? 1 : 0, Ice.Cells, Case.WantCells, Ice.Width(), Ice.Height(), Case.WantWidth, Case.WantHeight, Ice.Cells > 0 ? Ice.MinRow : -99, Ice.Big,
					Case.WantBig, bOk ? TEXT("ok") : TEXT("FAIL")));
				Unframe();
				Penguin->ResetForRound(Penguin->GetActorLocation()); // back to size 1
			});
	}

	// ---- the iceberg at our sizes 1, 2 and 4: 2x2, 3x3, 2x2 big blocks; it lands as that much sand ice and crushes the dummy
	// under it only once it presses it onto the floor.
	static const FSizeCase BergCases[] = { { 1, 4, 2, 2, 0 }, { 2, 9, 3, 3, 0 }, { 4, 16, 4, 4, 4 } };
	for (const FSizeCase& Case : BergCases)
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(*FString::Printf(TEXT("ice_IcebergSize%d"), Case.Size), 2.8f,
			[this, Run, Base, Frame, GrowTo, &Case]
			{
				SpawnTarget();
				PrepareBrawl(600.f, { EPenguinAbility::IcebergDrop });
				*Run = FIceRun();
				GrowTo(static_cast<float>(Case.Size));
				Frame(Base + FVector(400.f, 0.f, 190.f + 50.f * Case.Size), 1500.f + 300.f * Case.Size);
			},
			[this, Run, Base, &Case](float T)
			{
				FIceRun& R = *Run;
				APenguinCharacter* Dummy = Target.Get();
				if (!Dummy)
				{
					return;
				}
				if (!R.bCast && T > 1.f)
				{
					R.bCast = true;
					R.CastT = T;
					R.SizeAtCast = Penguin->GetSizeScale();
					Penguin->SetAimPoint(Dummy->GetFeetLocation() + FVector(0.f, 0.f, 50.f));
					R.bUsed = Penguin->ActivateAbility(0);
					for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
					{
						if (It->GetDropOwner() == Penguin.Get())
						{
							R.Berg = *It;
						}
					}
					if (const AIcebergDrop* Berg = R.Berg.Get())
					{
						R.BergCells = Berg->GetSizeCells();
						R.BergSpan = Berg->GetBlockSpan();
						const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
						R.BergLeft = Flat ? FMath::RoundToInt(static_cast<float>(Berg->GetLeftX() - Flat->GetActorLocation().X) / Flat->CellSize) : 0;
					}
					return;
				}
				if (!R.bCast)
				{
					return;
				}
				const AIcebergDrop* Berg = R.Berg.Get();
				if (Berg)
				{
					R.BergBottom = static_cast<float>(Berg->GetActorLocation().Z);
					if (!R.bFallShot && Berg->IsFalling() && R.BergBottom < 600.f)
					{
						R.bFallShot = true;
						Shot(*FString::Printf(TEXT("71_iceberg_size%d_fall"), Case.Size));
					}
				}
				else if (R.LandT < 0.f)
				{
					R.LandT = T; // landed: its ice is the island's now
				}
				if (!Dummy->IsAlive() && R.KOT < 0.f)
				{
					R.KOT = T;
					R.KOReason = Dummy->GetKOReason();
					R.KOFeet = static_cast<float>(Dummy->GetFeetLocation().Z);
					R.KOBergBottom = Berg ? R.BergBottom : -1.f;
				}
				if (R.LandT >= 0.f && !R.bShot && T > R.LandT + 0.5f)
				{
					R.bShot = true;
					Shot(*FString::Printf(TEXT("71_iceberg_size%d"), Case.Size));
				}
			},
			[this, Run, Base, Unframe, &Case]
			{
				const FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				const FIceCount Ice = CountIceCells(Flat, R.BergLeft - 1, R.BergLeft + Case.WantWidth, -1, Case.WantHeight + 2);
				const int32 WantSpan = Case.Size >= 4 ? 2 : 1;
				const bool bShape = R.BergCells == Case.WantWidth && R.BergSpan == WantSpan;
				const bool bLanded = R.LandT >= 0.f && Ice.Cells == Case.WantCells && Ice.Sand == Case.WantCells && Ice.Big == Case.WantBig && Ice.MinRow == 0
					&& Ice.Width() == Case.WantWidth && Ice.Height() == Case.WantHeight && Flat && !Flat->IsSandFalling();
				const bool bCrushed = R.KOT >= 0.f && R.KOReason == EPenguinKOReason::Iceberg && R.KOFeet < 15.f;
				const bool bOk = R.bUsed && bShape && bLanded && bCrushed;
				Report(FString::Printf(TEXT("RESULT iceberg size %d: our size %.2f, used %d, falling as %d x %d cells of %d-cell blocks (want %d x %d of %d); landed %d as %d ice cells (want %d), %d of them sand, %d wide x %d high from row %d, big blocks %d (want %d), "
					"sand still falling %d; the dummy under it knocked out %d, reason %s (want Iceberg), its feet %.0f cm above the floor then (want under 15: crushed on contact): %s"),
					Case.Size, R.SizeAtCast, R.bUsed ? 1 : 0, R.BergCells, R.BergCells, R.BergSpan, Case.WantWidth, Case.WantWidth, WantSpan, R.LandT >= 0.f ? 1 : 0, Ice.Cells, Case.WantCells,
					Ice.Sand, Ice.Width(), Ice.Height(), Ice.Cells > 0 ? Ice.MinRow : -99, Ice.Big, Case.WantBig, Flat && Flat->IsSandFalling() ? 1 : 0, R.KOT >= 0.f ? 1 : 0,
					*UEnum::GetValueAsString(R.KOReason), R.KOFeet, bOk ? TEXT("ok") : TEXT("FAIL")));
				Unframe();
				Penguin->ResetForRound(Penguin->GetActorLocation());
			});
	}

	// ---- a 4x iceberg with its left half over a ledge (rock, 3 high): the left big blocks stay on the ledge, the right ones
	// fall on as big blocks to the floor.
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(TEXT("ice_BigIcebergOverhang"), 2.6f,
			[this, Run, Base, Frame]
			{
				SpawnTarget();
				PrepareBrawl(2400.f, {});
				*Run = FIceRun();
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!Flat)
				{
					return;
				}
				const FIntPoint Ledge = Flat->WorldToCell(Base + FVector(500.f, 0.f, -12.f));
				Run->Column = Ledge.X;
				TArray<FIntPoint> Rock;
				for (int32 Dx = 0; Dx < 2; ++Dx)
				{
					for (int32 Row = 0; Row < 3; ++Row)
					{
						Rock.Add(FIntPoint(Ledge.X + Dx, Ledge.Y + Row));
					}
				}
				Flat->AddBlocks(Rock, EBlockKind::Rock, 0.f);
				// Its four columns from the ledge's left one: its middle two cells right of it.
				const float TargetX = static_cast<float>(Flat->CellToWorld(FIntPoint(Ledge.X, 0)).X) - Flat->CellSize * 0.5f + 2.f * Flat->CellSize;
				Run->Berg = AIcebergDrop::Drop(GetWorld(), Penguin.Get(), TargetX, 0.f, 4.f);
				Frame(FVector(TargetX - 100.f, 0.f, 330.f), 2600.f);
			},
			[this, Run, Base](float T)
			{
				FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!Flat)
				{
					return;
				}
				R.FallingBigMost = FMath::Max(R.FallingBigMost, Flat->GetNumFallingBigBlocks());
				if (!R.Berg.IsValid() && R.LandT < 0.f)
				{
					R.LandT = T;
				}
				if (R.LandT >= 0.f && !R.bShot && T > R.LandT + 0.7f)
				{
					R.bShot = true;
					Shot(TEXT("71_iceberg_overhang_big"));
				}
			},
			[this, Run, Base, Unframe]
			{
				const FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				const int32 L = R.Column;
				const FIceCount Ice = CountIceCells(Flat, L, L + 3, 0, 9);
				FIntPoint Origin;
				const bool bLeftUp = Flat && Flat->IsBigBlockCell(FIntPoint(L, 3), &Origin) && Origin == FIntPoint(L, 3) && Flat->IsBigBlockCell(FIntPoint(L, 5), &Origin) && Origin == FIntPoint(L, 5);
				const bool bRightDown = Flat && Flat->IsBigBlockCell(FIntPoint(L + 2, 0), &Origin) && Origin == FIntPoint(L + 2, 0) && Flat->IsBigBlockCell(FIntPoint(L + 2, 2), &Origin)
					&& Origin == FIntPoint(L + 2, 2);
				const bool bOk = R.LandT >= 0.f && Ice.Cells == 16 && Ice.Big == 4 && bLeftUp && bRightDown && R.FallingBigMost >= 2 && Flat && !Flat->IsSandFalling();
				Report(FString::Printf(TEXT("RESULT big iceberg overhang: landed %d, ice cells %d (want 16), big blocks %d (want 4): the left two on the ledge %d, the right two fell on as big blocks (at most %d falling at once, want 2) and lie on the floor %d, sand still falling %d: %s"),
					R.LandT >= 0.f ? 1 : 0, Ice.Cells, Ice.Big, bLeftUp ? 1 : 0, R.FallingBigMost, bRightDown ? 1 : 0, Flat && Flat->IsSandFalling() ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
				Unframe();
			});
	}

	// ---- a big block of sand: falls and lands as one, held up by either of its columns, falls as one when both are gone.
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(TEXT("ice_BigBlockSand"), 2.4f,
			[this, Run, Base]
			{
				PrepareBrawl(2400.f, {});
				*Run = FIceRun();
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!Flat)
				{
					return;
				}
				const FIntPoint Cell = Flat->WorldToCell(Base + FVector(400.f, 0.f, -12.f));
				Run->Column = Cell.X;
				const TArray<FIntPoint> Big = { FIntPoint(Cell.X, 4) };
				Flat->AddSandBlocks({ FIntPoint(Cell.X, 4), FIntPoint(Cell.X + 1, 4), FIntPoint(Cell.X, 5), FIntPoint(Cell.X + 1, 5) }, EBlockKind::Ice, nullptr, false, &Big);
				Run->FallingBigMost = Flat->GetNumFallingBigBlocks();
				// (And an ordinary sand block into a pit one block deep, 5 columns on: it lands in it, on row -1 - such a block used to
				// fall right through the island into the sea.)
				Flat->BreakBlockAt(Flat->CellToWorld(FIntPoint(Cell.X + 5, -1)));
				Flat->AddSandBlocks({ FIntPoint(Cell.X + 5, 3) }, EBlockKind::Ice);
			},
			[this, Run, Base](float T)
			{
				FIceRun& R = *Run;
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!Flat)
				{
					return;
				}
				const int32 X = R.Column;
				FIntPoint Origin;
				const int32 Falling = Flat->GetNumFallingBigBlocks();
				switch (R.Stage)
				{
				case 0: // falling as one, landing on the floor as a big block
					R.FallingBigMost = FMath::Max(R.FallingBigMost, Falling);
					if (Falling == 0 && Flat->IsBigBlockCell(FIntPoint(X, 0), &Origin) && Origin == FIntPoint(X, 0))
					{
						R.Checks[0] = 1;
						R.Stage = 1;
					}
					break;
				case 1: // its left column's floor goes: held up by the right one
					if (T > 1.f)
					{
						Flat->BreakBlockAt(Flat->CellToWorld(FIntPoint(X, -1)));
						R.Stage = 2;
						R.StageT = T;
					}
					break;
				case 2:
					R.Checks[1] = FMath::Max(R.Checks[1], Falling); // must stay 0
					if (T > R.StageT + 0.4f)
					{
						R.Checks[2] = Flat->IsBigBlockCell(FIntPoint(X, 0), &Origin) && Origin == FIntPoint(X, 0) ? 1 : 0;
						Flat->BreakBlockAt(Flat->CellToWorld(FIntPoint(X + 1, -1))); // and the right one's: it falls as one
						R.Stage = 3;
						R.StageT = T;
					}
					break;
				case 3:
					R.Checks[3] = FMath::Max(R.Checks[3], Falling);
					if (T > R.StageT + 0.7f)
					{
						R.Checks[4] = Flat->IsBigBlockCell(FIntPoint(X, -1), &Origin) && Origin == FIntPoint(X, -1) ? 1 : 0;
						R.Stage = 4;
					}
					break;
				default:
					break;
				}
			},
			[this, Run, Base]
			{
				const FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				const FIceCount Ice = CountIceCells(Flat, R.Column - 1, R.Column + 2, -3, 6);
				const bool bInPit = Flat && Flat->GetBlock(R.Column + 5, -1) == EBlockKind::Ice;
				const bool bOk = R.FallingBigMost == 1 && R.Checks[0] == 1 && R.Checks[1] == 0 && R.Checks[2] == 1 && R.Checks[3] == 1 && R.Checks[4] == 1 && Ice.Cells == 4
					&& Ice.Sand == 4 && Ice.Big == 1 && bInPit;
				Report(FString::Printf(TEXT("RESULT big block sand: fell as one big block %d (want 1), landed on the floor as one %d; one column's floor gone: falling %d (want 0), still a big block %d; "
					"both gone: fell as one %d (want 1), landed a row lower as one %d; ice cells %d (want 4), sand %d, big blocks %d (want 1); a sand block in a pit one block deep landed in it %d: %s"),
					R.FallingBigMost, R.Checks[0], R.Checks[1], R.Checks[2], R.Checks[3], R.Checks[4], Ice.Cells, Ice.Sand, Ice.Big, bInPit ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- breaking a big block: one hit breaks all four cells; taking one cell out (the hook, a tornado) leaves three ordinary
	// ice blocks. A: a snowball, B: taken, C: a blast (its edge), D: BreakIceInRadius with one block to break (E beside it
	// stays).
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(TEXT("ice_BigBlockBreak"), 1.6f,
			[this, Run, Base]
			{
				PrepareBrawl(2400.f, { EPenguinAbility::Snowball });
				*Run = FIceRun();
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!Flat)
				{
					return;
				}
				const int32 First = Flat->WorldToCell(Base + FVector(400.f, 0.f, -12.f)).X;
				const int32 Columns[5] = { First, First + 3, First + 6, First + 9, First + 11 };
				TArray<FIntPoint> Cells;
				TArray<FIntPoint> Big;
				for (int32 Index = 0; Index < 5; ++Index)
				{
					Run->Origins[Index] = FIntPoint(Columns[Index], 0);
					Big.Add(Run->Origins[Index]);
					for (int32 Dx = 0; Dx < 2; ++Dx)
					{
						for (int32 Dz = 0; Dz < 2; ++Dz)
						{
							Cells.Add(FIntPoint(Columns[Index] + Dx, Dz));
						}
					}
				}
				Flat->AddBlocks(Cells, EBlockKind::Ice, 0.f, &Big);
				Run->Checks[0] = Flat->GetNumBigBlocks(); // 5
				Run->BrokenAtStart = Flat->GetBrokenBlocks();
				// B: the hook / a tornado takes one cell.
				FIntPoint Taken;
				const FIntPoint B = Run->Origins[1];
				const bool bTaken = Flat->TakeIceBlockAt(Flat->CellToWorld(B + FIntPoint(1, 1)), Taken);
				Run->Checks[1] = bTaken ? CountIceCells(Flat, B.X, B.X + 1, 0, 1).Cells : -1; // 3
				Run->Checks[2] = Flat->IsBigBlockCell(B) ? 1 : 0;                        // 0
				// C: a blast reaching only its bottom-left cell.
				const FIntPoint C = Run->Origins[2];
				Run->Checks[3] = Flat->RemoveBlocksInRadius(Flat->CellToWorld(C) - FVector(30.f, 0.f, 0.f), 40.f); // 4
				// D: BreakIceInRadius, one block: all of D, none of E (its cells are in the circle too).
				const FIntPoint D = Run->Origins[3];
				Run->Checks[4] = Flat->BreakIceInRadius(Flat->CellToWorld(D + FIntPoint(1, 0)), 120.f, 1); // 4
				Run->Checks[5] = CountIceCells(Flat, Run->Origins[4].X, Run->Origins[4].X + 1, 0, 1).Big;   // 1
			},
			[this, Run, Base](float T)
			{
				FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!R.bCast && T > 0.2f && Flat)
				{
					// A: our snowball at its upper-left cell.
					R.bCast = true;
					Penguin->SetAimPoint(Flat->CellToWorld(R.Origins[0] + FIntPoint(0, 1)));
					R.bUsed = Penguin->ActivateAbility(0);
				}
			},
			[this, Run, Base]
			{
				const FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				const FIntPoint A = R.Origins[0];
				const int32 ALeft = CountIceCells(Flat, A.X, A.X + 1, 0, 1).Cells;
				const int32 Broken = Flat ? Flat->GetBrokenBlocks() - R.BrokenAtStart : -1;
				const bool bOk = R.Checks[0] == 5 && R.Checks[1] == 3 && R.Checks[2] == 0 && R.Checks[3] == 4 && R.Checks[4] == 4 && R.Checks[5] == 1 && R.bUsed && ALeft == 0
					&& Broken == 12 && Flat && Flat->GetNumBigBlocks() == 1;
				Report(FString::Printf(TEXT("RESULT big block break: big blocks made %d (want 5); one cell taken out: %d cells left (want 3), still big %d (want 0); a blast reaching one cell took %d (want 4); "
					"BreakIceInRadius (1 block) broke %d cells (want 4), the big block beside it still whole %d; a snowball at one cell: %d cells left (want 0); broken blocks counted %d (want 12), big blocks left %d (want 1): %s"),
					R.Checks[0], R.Checks[1], R.Checks[2], R.Checks[3], R.Checks[4], R.Checks[5], ALeft, Broken, Flat ? Flat->GetNumBigBlocks() : -1, bOk ? TEXT("ok") : TEXT("FAIL")));
			});
	}

	// ---- a tornado next to a big block: it rips cells out of it (the big block falls apart into ordinary ice), none breaks.
	{
		const TSharedRef<FIceRun> Run = MakeShared<FIceRun>();
		AddStep(TEXT("ice_BigBlockTornado"), 2.4f,
			[this, Run, Base]
			{
				PrepareBrawl(2400.f, { EPenguinAbility::Gust });
				*Run = FIceRun();
				ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				if (!Flat)
				{
					return;
				}
				const FIntPoint Origin(Flat->WorldToCell(Base + FVector(220.f, 0.f, -12.f)).X, 0);
				Run->Origins[0] = Origin;
				const TArray<FIntPoint> Big = { Origin };
				Flat->AddBlocks({ Origin, Origin + FIntPoint(1, 0), Origin + FIntPoint(0, 1), Origin + FIntPoint(1, 1) }, EBlockKind::Ice, 0.f, &Big);
				Run->Checks[0] = Flat->GetNumBigBlocks();
				Run->BrokenAtStart = Flat->GetBrokenBlocks();
			},
			[this, Run](float T)
			{
				FIceRun& R = *Run;
				if (!R.bCast && T > 0.2f)
				{
					R.bCast = true;
					Penguin->SetAimPoint(Penguin->GetAimOrigin() + FVector(200.f, 0.f, 0.f));
					R.bUsed = Penguin->ActivateAbility(0);
				}
				int32 Ripped = 0;
				for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
				{
					Ripped += It->IsFromTornado() ? 1 : 0;
				}
				R.RippedSeen = FMath::Max(R.RippedSeen, Ripped);
			},
			[this, Run, Base]
			{
				const FIceRun& R = *Run;
				const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
				const int32 Broken = Flat ? Flat->GetBrokenBlocks() - R.BrokenAtStart : -1;
				const int32 BigLeft = Flat ? Flat->GetNumBigBlocks() : -1;
				const bool bOk = R.Checks[0] == 1 && R.bUsed && R.RippedSeen >= 1 && BigLeft == 0 && Broken == 0;
				Report(FString::Printf(TEXT("RESULT big block tornado: big block made %d, tornado used %d, ice blocks ripped out at once %d (want 1 or more), big blocks left %d (want 0: it fell apart), blocks broken %d (want 0): %s"),
					R.Checks[0], R.bUsed ? 1 : 0, R.RippedSeen, BigLeft, Broken, bOk ? TEXT("ok") : TEXT("FAIL")));
				ClearAbilityLeftovers();
			});
	}
}

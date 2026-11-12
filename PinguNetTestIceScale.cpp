// Pingu - the LAN test's ice checks (APinguNetTest::UpdateIceScaleCheck, JudgeIceScaleCheck, UpdateIceScaleWatch,
// AddIceScaleChecks).
//
// Round 2, the last part of the hold (after the giant): the host builds a platform high above everything with the two players'
// penguins on it 3 m apart, and its penguin raises an ice wall right where the client's penguin stands. The wall is solid at
// once: on both machines the client's penguin (on its own machine client-authoritative) goes up onto the wall - never inside a
// block, never under the platform's top (no falling through), up there within a second, and no snapping back down afterwards.
// Then the host drops a 4x iceberg: both machines see it fall 4 cells wide in big blocks (its replicated shape) and find its 4
// big blocks on the island after it landed (the island's state, a client's from the host).
// -PinguNetLate: the host keeps a big block (2x2 cells, made of four rock cells at the bottom of the island's middle) in every
// round's island; the client finds it in the very first island it gets when it joins in the middle of the match.

#include "PinguNetTest.h"
#include "PinguNetTestGrant.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguNetTestIceScalePrivate
{
	/** Times into the check's part of the hold (s; APinguNetTest::IceScaleCheckLength in all). */
	constexpr float SetupAfter = 0.1f;
	constexpr float WatchAfter = 0.7f;
	constexpr float WallAfter = 1.f;
	constexpr float BergAfter = 2.4f;
	constexpr float JudgeAfter = 4.4f;
	/** The platform: this many rows under the island's lowest block, this many columns; the host's penguin on this column, the
	 *  client's ClientGap to its right; the iceberg's middle on this column. */
	constexpr int32 PlatformRowsDown = 8; // (2026-11-06: was 24 rows UP, past the v0.7 build limit, which refused the wall; below there is no limit)
	constexpr float PlatformOverSea = 400.f;
	constexpr int32 PlatformColumns = 30;
	constexpr int32 HostColumn = 6;
	constexpr float ClientGap = 300.f;
	constexpr int32 BergColumn = 22;
	/** Judged: the client's penguin is up on the wall (its feet this far over the platform's top, cm: the wall is 3 blocks) within
	 *  this long after the wall showed here; inside a block at most this many frames (the wall and the lift reach a client in
	 *  the same moment); afterwards never lower than this (no snapping back) for this long. */
	constexpr float UpFeet = 285.f;
	constexpr float UpWithin = 1.f;
	constexpr int32 MaxInsideFrames = 2;
	constexpr float StayFeet = 270.f;
	constexpr float StayFor = 0.8f;
	/** -PinguNetLate: the big block the host keeps in the island (its count). */
	constexpr int32 LateBigBlocks = 1;

	bool IsLateRun()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("PinguNetLate"));
	}

	/** One check per process (the client logs its results in a solo world after the game). */
	struct FIceScaleProgress
	{
		bool bJudged = false;
		bool bOk = false;
		FString Detail;
		bool bBergJudged = false;
		bool bBergOk = false;
		FString BergDetail;
		// this round's check
		bool bWatching = false;
		bool bSetUp = false;
		bool bWallUsed = false;
		bool bBergDropped = false;
		float PlatformTop = 0.f;          // world Z of the platform's top (both: from where the client's penguin stands)
		FIntPoint WallCell = FIntPoint(0, 0);
		bool bWallCellKnown = false;
		float WallSeenAt = -1.f;          // WorldTime the wall's block showed here
		float UpAt = -1.f;                // ... and the client's penguin was up on it
		float UpFeetSeen = 0.f;           // its feet over the platform's top then
		int32 Inside = 0;                 // frames the client's penguin was inside a block (from the wall on)
		float LowestFeet = 1.e9f;         // the lowest its feet got over the platform top (from the wall on)
		float LowestAfterUp = 1.e9f;      // ... and after it was up (for StayFor)
		bool bAlive = true;
		int32 BergCellsSeen = 0;          // the iceberg's shape as this machine has it
		int32 BergSpanSeen = 0;
		float BergGoneAt = -1.f;
		int32 BigBefore = -1;
		int32 BigAfter = -1;
		// -PinguNetLate
		int32 LateMadeGeneration = -1;
		int32 LateMade = 0;
		bool bLateJoinSeen = false;
		int32 LateBigAtJoin = -1;
		bool bLateSameAtJoin = false;
		int32 LateBigMost = 0;
	};

	FIceScaleProgress& IceScaleProgress()
	{
		static FIceScaleProgress Instance;
		return Instance;
	}
}

void APinguNetTest::ResetIceScaleCheck()
{
	using namespace PinguNetTestIceScalePrivate;
	FIceScaleProgress& P = IceScaleProgress();
	FIceScaleProgress Kept = P;
	P = FIceScaleProgress();
	P.bJudged = Kept.bJudged;
	P.bOk = Kept.bOk;
	P.Detail = Kept.Detail;
	P.bBergJudged = Kept.bBergJudged;
	P.bBergOk = Kept.bBergOk;
	P.BergDetail = Kept.BergDetail;
	P.LateMadeGeneration = Kept.LateMadeGeneration;
	P.LateMade = Kept.LateMade;
	P.bLateJoinSeen = Kept.bLateJoinSeen;
	P.LateBigAtJoin = Kept.LateBigAtJoin;
	P.bLateSameAtJoin = Kept.bLateSameAtJoin;
	P.LateBigMost = Kept.LateBigMost;
}

void APinguNetTest::UpdateIceScaleCheck(float T)
{
	using namespace PinguNetTestIceScalePrivate;
	FIceScaleProgress& P = IceScaleProgress();
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!HostPenguin || !ClientPenguin || !Island || P.bJudged)
	{
		return;
	}
	if (!P.bWatching)
	{
		P.bWatching = true;
		P.BigBefore = Island->GetNumBigBlocks();
		Report(TEXT("ice check: the host raises an ice wall where the client's penguin stands, then drops a 4x iceberg"));
	}

	// Host: the platform high above everything, the two penguins on it 3 m apart; the host's penguin gets the ice wall.
	if (bHostRole && !P.bSetUp && T >= SetupAfter)
	{
		P.bSetUp = true;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float BottomZ = Bounds.IsValid ? static_cast<float>(Bounds.Min.Z) : 0.f;
		const float PlatformZ = FMath::Max(BottomZ - (PlatformRowsDown + 0.5f) * Island->CellSize, Island->SeaLevel + PlatformOverSea);
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - PlatformColumns * 0.5f * Island->CellSize, 0.f, PlatformZ));
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < PlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		// (Lasting: the host clears everything it made here when it judges.)
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, 0.f).Num();
		Island->FlushEdits();
		const float FloorZ = static_cast<float>(Island->CellToWorld(Left).Z) + Island->CellSize * 0.5f;
		const float HostX = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + HostColumn, Left.Y)).X);
		const auto Place = [FloorZ](APenguinCharacter* Penguin, float X)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Penguin->NetTeleport(FVector(X, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
		};
		Place(HostPenguin, HostX);
		Place(ClientPenguin, HostX + ClientGap);
		if (UPenguinAbilityComponent* HostAbilities = HostPenguin->GetAbilities())
		{
			PinguNetTestGrant::Give(HostAbilities, EPenguinAbility::IceWall); // ready
		}
		Report(FString::Printf(TEXT("ice check: platform of %d blocks at z %.0f, the two penguins %.0f cm apart"), Added, FloorZ, ClientGap));
	}

	// Both: where the client's penguin stands (the cell the wall will fill under its middle) once it is up there.
	if (!P.bWallCellKnown && T >= WatchAfter)
	{
		const UCapsuleComponent* Capsule = ClientPenguin->GetCapsuleComponent();
		const FVector Loc = ClientPenguin->GetActorLocation();
		P.bWallCellKnown = true;
		P.WallCell = Island->WorldToCell(Loc);
		P.PlatformTop = static_cast<float>(Loc.Z) - (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.f);
		Report(FString::Printf(TEXT("ice check: the client's penguin stands at %s here, its feet at z %.0f, cell %d %d"), *Loc.ToString(), P.PlatformTop, P.WallCell.X, P.WallCell.Y));
	}

	// Host: the wall, aimed right at the client's penguin.
	if (bHostRole && P.bSetUp && !P.bWallUsed && T >= WallAfter)
	{
		P.bWallUsed = true;
		UPenguinAbilityComponent* HostAbilities = HostPenguin->GetAbilities();
		const int32 Slot = HostAbilities ? HostAbilities->FindSlot(EPenguinAbility::IceWall) : INDEX_NONE;
		const bool bUsed = Slot != INDEX_NONE && HostAbilities->UseAbility(Slot, ClientPenguin->GetFeetLocation() + FVector(0.f, 0.f, 50.f));
		Report(FString::Printf(TEXT("ice check: the host's ice wall at the client's penguin (%d)"), bUsed ? 1 : 0));
	}

	// Both: the wall showing here, the client's penguin from then on.
	if (P.bWallCellKnown)
	{
		if (P.WallSeenAt < 0.f && Island->IsSolidCell(P.WallCell))
		{
			P.WallSeenAt = WorldTime;
			Report(TEXT("ice check: the wall shows here"));
		}
		if (P.WallSeenAt >= 0.f && T < BergAfter)
		{
			const UCapsuleComponent* Capsule = ClientPenguin->GetCapsuleComponent();
			const float Feet = static_cast<float>(ClientPenguin->GetFeetLocation().Z) - P.PlatformTop;
			P.bAlive = P.bAlive && ClientPenguin->IsAlive();
			if (Capsule && !Island->CanFitCapsule(ClientPenguin->GetActorLocation(), Capsule->GetScaledCapsuleRadius() - 3.f, Capsule->GetScaledCapsuleHalfHeight() - 3.f))
			{
				++P.Inside;
			}
			P.LowestFeet = FMath::Min(P.LowestFeet, Feet);
			if (P.UpAt < 0.f && Feet >= UpFeet)
			{
				P.UpAt = WorldTime;
				P.UpFeetSeen = Feet;
			}
			if (P.UpAt >= 0.f && WorldTime - P.UpAt <= StayFor)
			{
				P.LowestAfterUp = FMath::Min(P.LowestAfterUp, Feet);
			}
		}
	}

	// Host: the 4x iceberg on the platform's other end.
	if (bHostRole && P.bSetUp && !P.bBergDropped && T >= BergAfter)
	{
		P.bBergDropped = true;
		const FIntPoint Column = Island->WorldToCell(HostPenguin->GetActorLocation() + FVector((BergColumn - HostColumn) * Island->CellSize, 0.f, 0.f));
		const float BergX = static_cast<float>(Island->CellToWorld(Column).X) - Island->CellSize * 0.5f;
		const AIcebergDrop* Berg = AIcebergDrop::Drop(GetWorld(), HostPenguin, BergX, HostPenguin->GetActorLocation().Z, 4.f);
		Report(FString::Printf(TEXT("ice check: the host drops a 4x iceberg (%d)"), Berg ? 1 : 0));
	}
	// Both: its shape as it falls here, its big blocks on the island once it is down.
	if (T >= BergAfter)
	{
		bool bFalling = false;
		for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
		{
			bFalling = true;
			P.BergCellsSeen = FMath::Max(P.BergCellsSeen, It->GetSizeCells());
			P.BergSpanSeen = FMath::Max(P.BergSpanSeen, It->GetBlockSpan());
		}
		if (!bFalling && P.BergCellsSeen > 0 && P.BergGoneAt < 0.f)
		{
			P.BergGoneAt = WorldTime;
		}
		if (P.BergGoneAt >= 0.f)
		{
			P.BigAfter = FMath::Max(P.BigAfter, Island->GetNumBigBlocks());
		}
	}
	if (T >= JudgeAfter)
	{
		JudgeIceScaleCheck(TEXT("judged after the iceberg"));
		if (bHostRole)
		{
			// Everything made up there goes (the platform, the wall, the iceberg's ice): the round plays on as before.
			Island->ClearTemporaryBlocks();
		}
	}
}

void APinguNetTest::JudgeIceScaleCheck(const TCHAR* Why)
{
	using namespace PinguNetTestIceScalePrivate;
	FIceScaleProgress& P = IceScaleProgress();
	if (P.bJudged || !P.bWatching)
	{
		return; // (it never started: logged as not run)
	}
	P.bJudged = true;
	const float UpAfter = P.UpAt >= 0.f && P.WallSeenAt >= 0.f ? P.UpAt - P.WallSeenAt : -1.f;
	P.bOk = P.WallSeenAt >= 0.f && UpAfter >= 0.f && UpAfter <= UpWithin && P.Inside <= MaxInsideFrames && P.LowestFeet > -5.f && P.LowestAfterUp >= StayFeet && P.bAlive;
	P.Detail = FString::Printf(TEXT("%s; the wall showed here %d, the client's penguin up on it %.2f s later (want 0 to %.1f; feet %.0f cm over the platform then, want %.0f), frames inside a block %d (want %d at most), "
		"its feet at least %.0f cm over the platform (want over -5: no falling through), afterwards at least %.0f for %.1f s (want %.0f: no snapping back), alive %d"),
		Why, P.WallSeenAt >= 0.f ? 1 : 0, UpAfter, UpWithin, P.UpFeetSeen, UpFeet, P.Inside, MaxInsideFrames, P.LowestFeet < 1.e8f ? P.LowestFeet : -1.f,
		P.LowestAfterUp < 1.e8f ? P.LowestAfterUp : -1.f, StayFor, StayFeet, P.bAlive ? 1 : 0);
	Report(FString::Printf(TEXT("ice wall check: %s (%s)"), P.bOk ? TEXT("ok") : TEXT("FAIL"), *P.Detail));

	P.bBergJudged = true;
	const int32 NewBig = P.BigAfter >= 0 && P.BigBefore >= 0 ? P.BigAfter - P.BigBefore : -1;
	P.bBergOk = P.BergCellsSeen == 4 && P.BergSpanSeen == 2 && P.BergGoneAt >= 0.f && NewBig >= 4;
	P.BergDetail = FString::Printf(TEXT("%s; the iceberg fell here %d cells wide in %d-cell blocks (want 4 and 2), landed %d, big blocks on the island here %d -> %d (want 4 more)"),
		Why, P.BergCellsSeen, P.BergSpanSeen, P.BergGoneAt >= 0.f ? 1 : 0, P.BigBefore, P.BigAfter);
	Report(FString::Printf(TEXT("big iceberg check: %s (%s)"), P.bBergOk ? TEXT("ok") : TEXT("FAIL"), *P.BergDetail));
}

void APinguNetTest::UpdateIceScaleWatch()
{
	using namespace PinguNetTestIceScalePrivate;
	if (!IsLateRun() || IsTraitsRun())
	{
		return;
	}
	FIceScaleProgress& P = IceScaleProgress();
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!Island || !GameState)
	{
		return;
	}
	const EPinguPhase Phase = GameState->GetPhase();
	if (bHostRole)
	{
		// Host: a big block in every round's island, from its first frame on (made of the four bottom cells of its middle two
		// columns: the island's look and size stay as they are).
		const bool bRound = Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight;
		if (!bRound || P.LateMadeGeneration == Island->GetGeneration() || Island->GetNumBigBlocks() >= LateBigBlocks)
		{
			return;
		}
		P.LateMadeGeneration = Island->GetGeneration();
		const FBox Bounds = Island->GetBounds();
		if (!Bounds.IsValid)
		{
			return;
		}
		const FIntPoint Middle = Island->WorldToCell(FVector((Bounds.Min.X + Bounds.Max.X) * 0.5, 0.0, Bounds.Min.Z + Island->CellSize * 0.5));
		TArray<FIntPoint> Cells;
		for (int32 Dx = 0; Dx < ABlockIsland::BigBlockCells; ++Dx)
		{
			// The lowest block of each column and the one above it.
			int32 Bottom = Middle.Y;
			while (!Island->IsSolidCell(FIntPoint(Middle.X + Dx, Bottom)) && Bottom < Middle.Y + 20)
			{
				++Bottom;
			}
			for (int32 Dz = 0; Dz < ABlockIsland::BigBlockCells; ++Dz)
			{
				Cells.Add(FIntPoint(Middle.X + Dx, Bottom + Dz));
			}
		}
		const FIntPoint Origin(Middle.X, FMath::Max(Cells[0].Y, Cells[ABlockIsland::BigBlockCells].Y));
		TArray<FIntPoint> Block;
		for (int32 Dx = 0; Dx < ABlockIsland::BigBlockCells; ++Dx)
		{
			for (int32 Dz = 0; Dz < ABlockIsland::BigBlockCells; ++Dz)
			{
				const FIntPoint Cell(Origin.X + Dx, Origin.Y + Dz);
				Block.Add(Cell);
				if (Island->IsSolidCell(Cell))
				{
					Island->BreakBlockAt(Island->CellToWorld(Cell));
				}
			}
		}
		const TArray<FIntPoint> Origins = { Origin };
		Island->AddBlocks(Block, EBlockKind::Ice, 0.f, &Origins);
		P.LateMade += Island->GetNumBigBlocks() >= LateBigBlocks ? 1 : 0;
		Report(FString::Printf(TEXT("late ice: a big block in round %d's island (generation %d) at cell %d %d: %d"), GameState->GetRoundNumber(), Island->GetGeneration(),
			Origin.X, Origin.Y, Island->GetNumBigBlocks()));
		return;
	}
	// Client: the very first island it gets from the host (in the middle of the match) has it.
	if (!P.bLateJoinSeen && GetNetMode() == NM_Client && Island->IsNetSynced())
	{
		P.bLateJoinSeen = true;
		P.LateBigAtJoin = Island->GetNumBigBlocks();
		P.bLateSameAtJoin = Island->MatchesHostIsland();
		Report(FString::Printf(TEXT("late ice: our first island from the host (round %d, phase %d) has %d big blocks, the same as the host's %d"), GameState->GetRoundNumber(),
			static_cast<int32>(Phase), P.LateBigAtJoin, P.bLateSameAtJoin ? 1 : 0));
	}
	P.LateBigMost = FMath::Max(P.LateBigMost, Island->GetNumBigBlocks());
}

void APinguNetTest::AddIceScaleChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestIceScalePrivate;
	const FIceScaleProgress& P = IceScaleProgress();
	if (IsLateRun())
	{
		if (IsTraitsRun())
		{
			return;
		}
		if (bHostRole)
		{
			Check(TEXT("a big block in every round's island (for the late joiner)"), P.LateMade >= 1, FString::Printf(TEXT("made in %d rounds"), P.LateMade));
		}
		else
		{
			Check(TEXT("big blocks came with the island we got when we joined"), P.bLateJoinSeen && P.LateBigAtJoin >= LateBigBlocks && P.bLateSameAtJoin,
				FString::Printf(TEXT("first island here: %d big blocks (want %d), the same as the host's %d; the most seen %d"), P.LateBigAtJoin, LateBigBlocks, P.bLateSameAtJoin ? 1 : 0,
					P.LateBigMost));
		}
		return;
	}
	Check(TEXT("an ice wall is solid at once: the client's penguin goes up onto it (never inside, no falling through, no snapping back)"), P.bOk,
		P.bJudged ? P.Detail : FString(TEXT("not run")));
	Check(TEXT("a 4x iceberg falls in big blocks and lands as big blocks on both machines"), P.bBergOk, P.bBergJudged ? P.BergDetail : FString(TEXT("not run")));
}

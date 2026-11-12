// Pingu - the LAN test's tornado check (APinguNetTest::UpdateTornadoCheck, JudgeTornadoCheck, AddTornadoChecks): round 1,
// last part of the hold (after the icicle throws). The host builds a platform high above everything with two ice blocks on
// it, puts its own penguin and the client's on it 2.5 m apart, and starts a tornado (the Gust slot). Both machines watch:
// the tornado shows, the client's penguin is sucked in toward the host's (its own machine moves it: the pull is a forwarded
// effect), then thrown away from it; the host's penguin hangs still in its eye the whole second; the two ice blocks leave
// the island on both machines (the island's own replication), and the client's island is the host's again once the ice has
// come down.
//
// Then the merge (UpdateTornadoMergeCheck, JudgeTornadoMergeCheck): on a new platform the host's penguin and the client's, 8 m
// apart, each start a tornado on their own machine (the client asks the server, as a player does). Their reaches overlap: both
// machines see them become one giant tornado (reach 12 m) with both players' penguins in it - side by side in its eye: the
// client's own penguin drifted there on its own machine (the drift is a forwarded effect) - and the bot, 11 m from their middle
// (out of a plain tornado's reach), pulled in and thrown away from it (into a wall: it stays in the round). The host also
// checks that the bot's hit is the client's (its own tornado started nearer).

#include "PinguNetTest.h"
#include "PinguNetTestGrant.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguLobby.h"
#include "PinguTornado.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerState.h"

namespace PinguNetTestTornadoPrivate
{
	/** Times into the check's part of the hold (s; APinguNetTest::TornadoCheckLength in all). */
	constexpr float SetupAfter = 0.1f;
	constexpr float CastAfter = 0.8f;
	constexpr float JudgeAfter = 2.8f;
	/** The platform: this many rows over the island's highest block, this many columns, gone after this long. */
	constexpr int32 PlatformRowsUp = 20;
	constexpr int32 PlatformColumns = 26;
	constexpr float PlatformLife = 3.5f;
	constexpr int32 HostColumn = 9;
	constexpr float ClientGap = 250.f;       // from the host's penguin to the client's (cm, to the right)
	constexpr int32 IceColumn = HostColumn - 3; // two ice blocks stacked on the platform, 3 m left of the host's penguin
	/** Judged: sucked in at least this much (cm), thrown at least this fast (cm/s), the host's penguin moved at most this
	 *  much while it hung in its eye (cm; a client sees another machine's penguin smoothed: a little more), the hit seen this
	 *  long after the tornado showed here (s). */
	constexpr float MinPulled = 40.f;
	constexpr float MinThrowSpeed = 1000.f;
	constexpr float MaxHostDriftHere = 2.f;
	constexpr float MaxHostDriftThere = 15.f;
	constexpr float HitFrom = 0.3f;
	constexpr float HitTo = 1.f;
	/** The ice must be gone from the island this long after the tornado showed here (s). */
	constexpr float IceGoneAfter = 0.35f;

	/** One check per process (the client logs its results in a solo world after the game). */
	struct FTornadoProgress
	{
		bool bJudged = false;
		bool bOk = false;
		FString Detail;
		// this round's check
		bool bSetUp = false;
		bool bWatching = false;
		bool bCast = false;
		bool bUsed = false;
		TWeakObjectPtr<APinguTornado> Tornado;
		float SeenAt = -1.f;              // WorldTime the tornado showed here
		FVector HostAt = FVector::ZeroVector;
		float HostDrift = 0.f;
		float StartDist = -1.f;
		float DistBeforeHit = -1.f;
		int32 ClientHitsAtStart = 0;
		float HitAt = -1.f;               // s after it showed here
		float HitSpeed = 0.f;
		bool bHitAway = false;
		TArray<FIntPoint> IceCells;       // where the host put the ice (the client finds them as the only ice up there)
		bool bIceSeenThere = false;
		bool bIceGoneChecked = false;
		int32 IceLeft = -1;
		int32 Ripped = -1;
	};

	FTornadoProgress& TornadoProgress()
	{
		static FTornadoProgress Instance;
		return Instance;
	}

	float DistXZ(const FVector& A, const FVector& B)
	{
		return static_cast<float>(FVector::Dist(FVector(A.X, 0.f, A.Z), FVector(B.X, 0.f, B.Z)));
	}

	// ---- the merge part: times into the tornado's part of the hold (the first part's platform is gone at 3.6 s)
	constexpr float MergeSetupAfter = 3.7f;
	constexpr float MergeCastAfter = 4.3f;
	constexpr float MergeJudgeAfter = 6.3f;
	/** Its own platform: this many rows over the island's highest block (between the first part's and the self bomb's), gone
	 *  after this long; the two players' penguins 8 m apart on it, the bot 11 m from their middle (7 m from the client's: out
	 *  of a plain tornado's reach) with a rock wall 2 m behind it (it is thrown into that, not into the sea: the round goes on). */
	constexpr int32 MergePlatformRowsUp = 22;
	constexpr float MergePlatformLife = 2.8f;
	constexpr int32 MergeHostColumn = 4;
	constexpr int32 MergeClientColumn = 12;
	constexpr int32 MergeBotColumn = 19;
	constexpr int32 MergeWallColumn = 21;
	constexpr int32 MergeWallRows = 6;
	/** Judged: the giant seen here this long after the casts at most (s), its reach at least this (cm), the two penguins this
	 *  close to their places in its eye this long after it showed here (cm, s), the bot thrown this long after it showed here. */
	constexpr float GiantSeenWithin = 1.f;
	constexpr float MinGiantReach = 1150.f;
	constexpr float GapTolerance = 30.f;
	constexpr float GapAfter = 0.35f;
	constexpr float BotHitFrom = 0.25f;
	constexpr float BotHitTo = 1.1f;

	struct FMergeProgress
	{
		bool bJudged = false;
		bool bOk = false;
		FString Detail;
		// this round's check
		bool bSetUp = false;
		bool bWatching = false;
		bool bCast = false;
		bool bOwnUsed = false;
		float CastAt = -1.f;              // WorldTime of our own cast here
		TWeakObjectPtr<APinguTornado> Giant;
		float GiantSeenAt = -1.f;         // WorldTime the giant showed here
		float GiantReach = 0.f;
		bool bBothUsers = false;
		int32 TornadoesAfter = -1;
		float StartGap = -1.f;            // the two players' penguins along X at the cast, as this machine shows them
		float Gap = -1.f;
		float GapWant = 0.f;
		int32 OwnHoverFrames = 0;
		int32 OwnNotHovering = 0;
		FVector Eye = FVector::ZeroVector;
		int32 BotHitsAtStart = 0;
		float BotStartDist = -1.f;
		float BotDistBeforeHit = -1.f;
		float BotHitAt = -1.f;            // s after the giant showed here
		float BotXAtHit = 0.f;
		float BotAwayAfter = 0.f;         // how much further from the eye it got in the 0.3 s after its hit (cm)
		bool bBotByClient = false;        // (the host: the giant's record says it was the client's hit)
		bool bBotAlive = false;
		int32 LobbyFrames = 0;            // frames on which the lobby still had a say on the bot (strolls, idles, pop, its hat)
		int32 Frames = 0;
	};

	FMergeProgress& MergeProgress()
	{
		static FMergeProgress Instance;
		return Instance;
	}

	/** The bot: the only penguin no player has. */
	APenguinCharacter* FindBot(UWorld* World, const APenguinCharacter* HostPenguin, const APenguinCharacter* ClientPenguin)
	{
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			const APlayerState* PlayerState = It->GetPlayerState();
			if (*It != HostPenguin && *It != ClientPenguin && (!PlayerState || PlayerState->IsABot()))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

void APinguNetTest::ResetTornadoCheck()
{
	using namespace PinguNetTestTornadoPrivate;
	FTornadoProgress& P = TornadoProgress();
	const bool bJudged = P.bJudged;
	const bool bOk = P.bOk;
	const FString Detail = P.Detail;
	P = FTornadoProgress();
	P.bJudged = bJudged;
	P.bOk = bOk;
	P.Detail = Detail;
	FMergeProgress& M = MergeProgress();
	const bool bMergeJudged = M.bJudged;
	const bool bMergeOk = M.bOk;
	const FString MergeDetail = M.Detail;
	M = FMergeProgress();
	M.bJudged = bMergeJudged;
	M.bOk = bMergeOk;
	M.Detail = MergeDetail;
}

void APinguNetTest::UpdateTornadoCheck(float T)
{
	using namespace PinguNetTestTornadoPrivate;
	if (T >= MergeSetupAfter)
	{
		UpdateTornadoMergeCheck(T); // (the first part was judged at its JudgeAfter)
		return;
	}
	FTornadoProgress& P = TornadoProgress();
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
		Report(TEXT("tornado check: the host's penguin starts a tornado next to the client's on a platform with two ice blocks"));
	}

	// Host: the platform high above everything, the two penguins on it 2.5 m apart, two ice blocks left of the host's.
	if (bHostRole && !P.bSetUp && T >= SetupAfter)
	{
		P.bSetUp = true;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float TopZ = Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) : 0.f;
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - PlatformColumns * 0.5f * Island->CellSize, 0.f, TopZ + (PlatformRowsUp + 0.5f) * Island->CellSize));
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < PlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, PlatformLife).Num();
		P.IceCells = { FIntPoint(Left.X + IceColumn, Left.Y + 1), FIntPoint(Left.X + IceColumn, Left.Y + 2) };
		const int32 IceAdded = Island->AddBlocks(P.IceCells, EBlockKind::Ice, 0.f).Num();
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
			PinguNetTestGrant::Give(HostAbilities, EPenguinAbility::Gust); // ready
		}
		Report(FString::Printf(TEXT("tornado check: platform of %d blocks at z %.0f, %d ice blocks on it, the two penguins %.0f cm apart"), Added, FloorZ, IceAdded, ClientGap));
	}

	// Host: the tornado.
	if (bHostRole && P.bSetUp && !P.bCast && T >= CastAfter)
	{
		P.bCast = true;
		UPenguinAbilityComponent* HostAbilities = HostPenguin->GetAbilities();
		const int32 Slot = HostAbilities ? HostAbilities->FindSlot(EPenguinAbility::Gust) : INDEX_NONE;
		P.bUsed = Slot != INDEX_NONE && HostAbilities->UseAbility(Slot, ClientPenguin->GetAimOrigin());
		Report(FString::Printf(TEXT("tornado check: the host's penguin starts its tornado (%d)"), P.bUsed ? 1 : 0));
	}

	// Both: the tornado as this machine has it.
	if (!P.Tornado.IsValid() && P.SeenAt < 0.f)
	{
		for (TActorIterator<APinguTornado> It(GetWorld()); It; ++It)
		{
			if (It->GetUser() == HostPenguin)
			{
				P.Tornado = *It;
				P.SeenAt = WorldTime;
				P.HostAt = HostPenguin->GetActorLocation();
				P.StartDist = DistXZ(ClientPenguin->GetActorLocation(), It->GetCenter());
				const UPenguinMovementComponent* ClientMove = ClientPenguin->GetPenguinMovement();
				P.ClientHitsAtStart = ClientMove ? ClientMove->HitEvents : 0;
				Report(FString::Printf(TEXT("tornado check: the tornado is here, the client's penguin %.0f cm from its eye"), P.StartDist));
				break;
			}
		}
		// The client finds the ice the host put up there: the only ice that high.
		if (!bHostRole && P.IceCells.Num() < 2)
		{
			P.IceCells.Reset();
			const FIntPoint HostCell = Island->WorldToCell(HostPenguin->GetFeetLocation() - FVector(0.f, 0.f, 10.f));
			for (int32 Column = HostCell.X - 6; Column <= HostCell.X; ++Column)
			{
				for (int32 Row = HostCell.Y + 1; Row <= HostCell.Y + 3; ++Row)
				{
					if (Island->GetBlock(Column, Row) == EBlockKind::Ice)
					{
						P.IceCells.Add(FIntPoint(Column, Row));
					}
				}
			}
			P.bIceSeenThere = P.IceCells.Num() == 2;
		}
	}
	if (bHostRole && P.IceCells.Num() > 0 && !P.bIceSeenThere && !P.bCast)
	{
		P.bIceSeenThere = Island->GetBlock(P.IceCells[0].X, P.IceCells[0].Y) == EBlockKind::Ice && Island->GetBlock(P.IceCells[1].X, P.IceCells[1].Y) == EBlockKind::Ice;
	}
	if (P.SeenAt < 0.f)
	{
		if (T >= JudgeAfter)
		{
			JudgeTornadoCheck(TEXT("no tornado showed up"));
		}
		return;
	}
	const float Since = WorldTime - P.SeenAt;
	APinguTornado* Tornado = P.Tornado.Get();
	const FVector Eye = Tornado ? Tornado->GetCenter() : HostPenguin->GetAimOrigin();
	if (Since <= PinguTuning::TornadoTime - 0.1f)
	{
		P.HostDrift = FMath::Max(P.HostDrift, DistXZ(HostPenguin->GetActorLocation(), P.HostAt));
	}
	if (Tornado)
	{
		P.Ripped = bHostRole ? Tornado->GetIceRipped() : P.Ripped;
	}
	const UPenguinMovementComponent* ClientMove = ClientPenguin->GetPenguinMovement();
	if (ClientMove && P.HitAt < 0.f)
	{
		if (ClientMove->HitEvents != P.ClientHitsAtStart)
		{
			P.HitAt = Since;
			P.HitSpeed = static_cast<float>(ClientMove->Velocity.Size());
			P.bHitAway = (ClientPenguin->GetActorLocation().X - Eye.X) * ClientMove->Velocity.X > 0.f;
			Report(FString::Printf(TEXT("tornado check: the client's penguin thrown %.2f s after the tornado showed here, %.0f cm/s, away from the host's %d"), P.HitAt, P.HitSpeed,
				P.bHitAway ? 1 : 0));
		}
		else
		{
			P.DistBeforeHit = DistXZ(ClientPenguin->GetActorLocation(), Eye);
		}
	}
	if (!P.bIceGoneChecked && Since >= IceGoneAfter && P.IceCells.Num() > 0)
	{
		P.bIceGoneChecked = true;
		P.IceLeft = 0;
		for (const FIntPoint& Cell : P.IceCells)
		{
			P.IceLeft += Island->GetBlock(Cell.X, Cell.Y) != EBlockKind::None ? 1 : 0;
		}
	}
	if (T >= JudgeAfter)
	{
		JudgeTornadoCheck(TEXT("judged after the throw"));
	}
}

void APinguNetTest::JudgeTornadoCheck(const TCHAR* Why)
{
	using namespace PinguNetTestTornadoPrivate;
	JudgeTornadoMergeCheck(Why); // (only once its part has begun)
	FTornadoProgress& P = TornadoProgress();
	if (P.bJudged || !P.bWatching)
	{
		return; // (it never started: logged as not run)
	}
	P.bJudged = true;
	const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	const bool bSameIsland = Island && Island->MatchesHostIsland();
	const float Pulled = P.StartDist - P.DistBeforeHit;
	const bool bHitOnTime = P.HitAt >= HitFrom && P.HitAt <= HitTo;
	const float MaxDrift = bHostRole ? MaxHostDriftHere : MaxHostDriftThere;
	P.bOk = P.SeenAt >= 0.f && Pulled >= MinPulled && bHitOnTime && P.bHitAway && P.HitSpeed >= MinThrowSpeed && P.HostDrift <= MaxDrift && P.bIceSeenThere
		&& P.IceLeft == 0 && bSameIsland && (!bHostRole || (P.bUsed && P.Ripped == 2));
	P.Detail = FString::Printf(TEXT("%s; the tornado here %d, the client's penguin %.0f -> %.0f cm from its eye (pulled %.0f, want %.0f or more), thrown %.2f s after it showed here "
		"(want %.1f to %.1f) at %.0f cm/s (want %.0f or more) away from the host's %d, the host's penguin moved %.1f cm in its eye (want %.0f at most), the two ice blocks there %d and "
		"gone after %.2f s: %d left (want 0)%s, this island the host's %d"),
		Why, P.SeenAt >= 0.f ? 1 : 0, P.StartDist, P.DistBeforeHit, Pulled, MinPulled, P.HitAt, HitFrom, HitTo, P.HitSpeed, MinThrowSpeed, P.bHitAway ? 1 : 0, P.HostDrift, MaxDrift,
		P.bIceSeenThere ? 1 : 0, IceGoneAfter, P.IceLeft, bHostRole ? *FString::Printf(TEXT(", started %d, ripped out %d (want 2)"), P.bUsed ? 1 : 0, P.Ripped) : TEXT(""),
		bSameIsland ? 1 : 0);
	Report(FString::Printf(TEXT("tornado check: %s (%s)"), P.bOk ? TEXT("ok") : TEXT("FAIL"), *P.Detail));
}

void APinguNetTest::AddTornadoChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestTornadoPrivate;
	const FTornadoProgress& P = TornadoProgress();
	Check(TEXT("tornado: sucks the client's penguin in and throws it, the host's hangs still, its ice gone on both machines"), P.bOk,
		P.bJudged ? P.Detail : FString(TEXT("not run")));
	const FMergeProgress& M = MergeProgress();
	Check(TEXT("tornado merge: the host's and the client's tornadoes become one giant with both in its eye, it throws the bot from 11 m"), M.bOk,
		M.bJudged ? M.Detail : FString(TEXT("not run")));
}

void APinguNetTest::UpdateTornadoMergeCheck(float T)
{
	using namespace PinguNetTestTornadoPrivate;
	FMergeProgress& M = MergeProgress();
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	APenguinCharacter* Bot = FindBot(GetWorld(), HostPenguin, ClientPenguin);
	if (!HostPenguin || !ClientPenguin || !Island || !Bot || M.bJudged)
	{
		return;
	}
	if (!M.bWatching)
	{
		M.bWatching = true;
		Report(TEXT("tornado merge check: the host's and the client's penguins, 8 m apart, start a tornado each; the bot stands 11 m from their middle"));
	}

	// Host: a new platform high above, the two players' penguins on it 8 m apart, the bot further right with a wall behind it.
	if (bHostRole && !M.bSetUp)
	{
		M.bSetUp = true;
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			It->Destroy();
		}
		const FBox Bounds = Island->GetBounds();
		const float MidX = Bounds.IsValid ? static_cast<float>(Bounds.Min.X + Bounds.Max.X) * 0.5f : 0.f;
		const float TopZ = Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) : 0.f;
		const FIntPoint Left = Island->WorldToCell(FVector(MidX - PlatformColumns * 0.5f * Island->CellSize, 0.f, TopZ + (MergePlatformRowsUp + 0.5f) * Island->CellSize));
		TArray<FIntPoint> Cells;
		for (int32 Column = 0; Column < PlatformColumns; ++Column)
		{
			Cells.Add(FIntPoint(Left.X + Column, Left.Y));
		}
		for (int32 Row = 1; Row <= MergeWallRows; ++Row)
		{
			Cells.Add(FIntPoint(Left.X + MergeWallColumn, Left.Y + Row));
		}
		const int32 Added = Island->AddBlocks(Cells, EBlockKind::Rock, MergePlatformLife).Num();
		Island->FlushEdits();
		const float FloorZ = static_cast<float>(Island->CellToWorld(Left).Z) + Island->CellSize * 0.5f;
		const auto Place = [FloorZ, Island, &Left](APenguinCharacter* Penguin, int32 Column)
		{
			const float Half = Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const float X = static_cast<float>(Island->CellToWorld(FIntPoint(Left.X + Column, Left.Y)).X);
			Penguin->NetTeleport(FVector(X, 0.f, FloorZ + Half + 2.f), FVector::ZeroVector, true);
		};
		Place(HostPenguin, MergeHostColumn);
		Place(ClientPenguin, MergeClientColumn);
		Place(Bot, MergeBotColumn);
		for (APenguinCharacter* Caster : { HostPenguin, ClientPenguin })
		{
			if (UPenguinAbilityComponent* Abilities = Caster->GetAbilities())
			{
				PinguNetTestGrant::Give(Abilities, EPenguinAbility::Gust); // ready
			}
		}
		Report(FString::Printf(TEXT("tornado merge check: platform of %d blocks (with the wall) at z %.0f, the players' penguins %d m apart, the bot %d m right of the client's"), Added, FloorZ,
			MergeClientColumn - MergeHostColumn, MergeBotColumn - MergeClientColumn));
	}

	// Both: our own penguin starts its tornado (a LAN client asks the server: the ability is server gameplay).
	if (!M.bCast && T >= MergeCastAfter)
	{
		M.bCast = true;
		const UPenguinAbilityComponent* Abilities = Own->GetAbilities();
		const int32 Slot = Abilities ? Abilities->FindSlot(EPenguinAbility::Gust) : INDEX_NONE;
		M.bOwnUsed = Slot != INDEX_NONE && Own->ActivateAbility(Slot);
		M.CastAt = WorldTime;
		M.StartGap = FMath::Abs(static_cast<float>(ClientPenguin->GetActorLocation().X - HostPenguin->GetActorLocation().X));
		M.GapWant = HostPenguin->GetCapsuleComponent()->GetScaledCapsuleRadius() + ClientPenguin->GetCapsuleComponent()->GetScaledCapsuleRadius() + PinguTuning::TornadoMergeEyeGap;
		M.BotHitsAtStart = Bot->GetPenguinMovement() ? Bot->GetPenguinMovement()->HitEvents : 0;
		Report(FString::Printf(TEXT("tornado merge check: our penguin starts its tornado (%d), the two %.0f cm apart here"), M.bOwnUsed ? 1 : 0, M.StartGap));
	}
	if (!M.bCast)
	{
		return;
	}
	// (A trace of the bot as this machine has it, every frame of the check: where it is, how it moves, who runs it.)
	// Nothing of the lobby island acts on a penguin in the match (its strolls, idles, pokes, pop scale, the hat's team colour).
	{
		const APinguLobby* Lobby = APinguLobby::Get(GetWorld());
		const bool bLobby = (Lobby && Lobby->IsShowing()) || PinguLobby::GetAnimState(Bot).bLobby || !FMath::IsNearlyEqual(Bot->GetLobbyPopScale(), 1.f)
			|| Bot->GetHatColorIndex() != Bot->GetColorIndex();
		++M.Frames;
		M.LobbyFrames += bLobby ? 1 : 0;
	}
	if (const UPenguinMovementComponent* TraceMove = Bot->GetPenguinMovement())
	{
		TArray<APinguTornado*> TraceTornadoes;
		APinguTornado::GetAll(GetWorld(), TraceTornadoes);
		const FRepMovement& Rep = Bot->GetReplicatedMovement();
		UE_LOG(LogPingu, Log, TEXT("[NETTEST] merge trace %.3f: %s role %d/%d owner %s at %.0f, %.0f vel %.0f, %.0f mode %d pulled %d hover %d hits %d rep %.0f, %.0f v %.0f, %.0f tornadoes %d"),
			WorldTime - M.CastAt, *Bot->GetName(), static_cast<int32>(Bot->GetLocalRole()), static_cast<int32>(Bot->GetRemoteRole()),
			Bot->GetOwner() ? *Bot->GetOwner()->GetName() : TEXT("-"), Bot->GetActorLocation().X, Bot->GetActorLocation().Z,
			TraceMove->Velocity.X, TraceMove->Velocity.Z, static_cast<int32>(TraceMove->MovementMode.GetValue()), TraceMove->IsTornadoPulled() ? 1 : 0,
			TraceMove->IsHovering() ? 1 : 0, TraceMove->HitEvents, Rep.Location.X, Rep.Location.Z, Rep.LinearVelocity.X, Rep.LinearVelocity.Z,
			TraceTornadoes.Num());
	}

	// Both: the giant as this machine has it (one tornado that took another in, with both players' penguins in it).
	TArray<APinguTornado*> All;
	APinguTornado::GetAll(GetWorld(), All);
	if (!M.Giant.IsValid() && M.GiantSeenAt < 0.f)
	{
		for (APinguTornado* Each : All)
		{
			if (Each->GetMerges() > 0)
			{
				M.Giant = Each;
				M.GiantSeenAt = WorldTime;
				M.GiantReach = Each->GetRadius();
				M.bBothUsers = Each->IsUser(HostPenguin) && Each->IsUser(ClientPenguin);
				M.Eye = Each->GetCenter();
				M.BotStartDist = DistXZ(Bot->GetActorLocation(), M.Eye);
				Report(FString::Printf(TEXT("tornado merge check: the giant is here %.2f s after our cast, reach %.0f cm, both players in it %d, the bot %.0f cm from its eye"),
					WorldTime - M.CastAt, M.GiantReach, M.bBothUsers ? 1 : 0, M.BotStartDist));
				break;
			}
		}
	}
	if (M.GiantSeenAt < 0.f)
	{
		if (T >= MergeJudgeAfter)
		{
			JudgeTornadoMergeCheck(TEXT("no giant tornado showed up"));
		}
		return;
	}
	const float Since = WorldTime - M.GiantSeenAt;
	if (M.TornadoesAfter < 0 && Since > 0.25f)
	{
		M.TornadoesAfter = All.Num();
	}
	// Both players' penguins side by side in its eye: the client's own drifted there on its own machine (a forwarded effect).
	if (M.Gap < 0.f && Since > GapAfter)
	{
		M.Gap = FMath::Abs(static_cast<float>(ClientPenguin->GetActorLocation().X - HostPenguin->GetActorLocation().X));
	}
	// Our own penguin hangs in its eye (its hover reaches us as a forwarded effect on a client).
	if (Since > 0.05f && Since < 0.9f)
	{
		++M.OwnHoverFrames;
		M.OwnNotHovering += Own->GetPenguinMovement() && Own->GetPenguinMovement()->IsHovering() ? 0 : 1;
	}
	// The bot: pulled in, then thrown away from the eye.
	const UPenguinMovementComponent* BotMove = Bot->GetPenguinMovement();
	if (BotMove && M.BotHitAt < 0.f)
	{
		if (BotMove->HitEvents != M.BotHitsAtStart)
		{
			M.BotHitAt = Since;
			M.BotXAtHit = static_cast<float>(Bot->GetActorLocation().X);
			Report(FString::Printf(TEXT("tornado merge check: the bot thrown %.2f s after the giant showed here"), M.BotHitAt));
		}
		else
		{
			M.BotDistBeforeHit = DistXZ(Bot->GetActorLocation(), M.Eye);
		}
	}
	if (M.BotHitAt >= 0.f && Since < M.BotHitAt + 0.3f)
	{
		M.BotAwayAfter = FMath::Max(M.BotAwayAfter, (static_cast<float>(Bot->GetActorLocation().X) - M.BotXAtHit) * FMath::Sign(M.BotXAtHit - static_cast<float>(M.Eye.X)));
	}
	if (bHostRole && !M.bBotByClient)
	{
		if (const APinguTornado* Giant = M.Giant.Get())
		{
			for (const APinguTornado::FThrowRecord& Record : Giant->GetThrows())
			{
				M.bBotByClient |= Record.Penguin.Get() == Bot && Record.By.Get() == ClientPenguin;
			}
		}
	}
	M.bBotAlive = Bot->IsAlive();
	if (T >= MergeJudgeAfter)
	{
		JudgeTornadoMergeCheck(TEXT("judged after the giant's throw"));
	}
}

void APinguNetTest::JudgeTornadoMergeCheck(const TCHAR* Why)
{
	using namespace PinguNetTestTornadoPrivate;
	FMergeProgress& M = MergeProgress();
	if (M.bJudged || !M.bWatching)
	{
		return; // (its part never began: logged as not run)
	}
	M.bJudged = true;
	const bool bSeen = M.GiantSeenAt >= 0.f && M.CastAt >= 0.f && M.GiantSeenAt - M.CastAt <= GiantSeenWithin;
	const bool bGap = M.Gap >= 0.f && FMath::Abs(M.Gap - M.GapWant) <= GapTolerance && M.StartGap > 600.f;
	const float Pulled = M.BotStartDist - M.BotDistBeforeHit;
	const bool bBot = M.BotHitAt >= BotHitFrom && M.BotHitAt <= BotHitTo && Pulled > 20.f && M.BotAwayAfter > 50.f;
	M.bOk = M.bOwnUsed && bSeen && M.GiantReach >= MinGiantReach && M.bBothUsers && M.TornadoesAfter == 1 && bGap && M.OwnHoverFrames > 10 && M.OwnNotHovering == 0 && bBot
		&& M.LobbyFrames == 0 && (!bHostRole || M.bBotByClient);
	M.Detail = FString::Printf(TEXT("%s; our cast %d, the giant here %.2f s after it (want %.1f at most), reach %.0f cm (want %.0f or more), both players in it %d, tornadoes here after it %d (want 1); "
		"the two penguins %.0f cm apart at the cast, %.0f cm %.2f s after the giant showed (want %.0f within %.0f); our own penguin hung in its eye on %d of %d frames (want all); the bot %.0f -> %.0f cm "
		"from its eye (pulled %.0f, want over 20), thrown %.2f s after it showed here (want %.2f to %.2f), %.0f cm further away 0.3 s later (want over 50), alive after %d, the lobby on it %d of %d frames (want 0)%s"),
		Why, M.bOwnUsed ? 1 : 0, M.GiantSeenAt >= 0.f ? M.GiantSeenAt - M.CastAt : -1.f, GiantSeenWithin, M.GiantReach, MinGiantReach, M.bBothUsers ? 1 : 0, M.TornadoesAfter,
		M.StartGap, M.Gap, GapAfter, M.GapWant, GapTolerance, M.OwnHoverFrames - M.OwnNotHovering, M.OwnHoverFrames, M.BotStartDist, M.BotDistBeforeHit, Pulled, M.BotHitAt, BotHitFrom,
		BotHitTo, M.BotAwayAfter, M.bBotAlive ? 1 : 0, M.LobbyFrames, M.Frames, bHostRole ? *FString::Printf(TEXT(", the client's hit by the giant's record %d (want 1)"), M.bBotByClient ? 1 : 0) : TEXT(""));
	Report(FString::Printf(TEXT("tornado merge check: %s (%s)"), M.bOk ? TEXT("ok") : TEXT("FAIL"), *M.Detail));
}

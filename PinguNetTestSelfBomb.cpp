// Pingu - the LAN test's self-bomb check (APinguNetTest::UpdateSelfBombCheck, JudgeSelfBombCheck, AddSelfBombChecks): round 1,
// the very last part of the hold (after the tornado). The host builds a platform high above everything, puts its own penguin
// and the client's on it 6 m apart (outside each other's blasts), and its penguin drops a snow bomb at its own feet: the bomb
// knocks its thrower out (reason Bomb) - both machines see the host's penguin knocked out about 0.6 s after they saw the
// bomb, the feed says it blew itself up, and nobody gets the point (the knock-out counts for nobody: no kill in the score rows,
// one fall for the host). Then a bomb of the client's lies 2 m from the client's penguin (the host makes it for the client:
// "thrown" by it) and goes off: the client's own penguin is pushed like anybody - the push reaches its owner's machine, both
// machines see the hit - and survives (the push radius, not the kill radius).

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

namespace PinguNetTestSelfBombPrivate
{
	/** Times into the check's part of the hold (s; APinguNetTest::SelfBombCheckLength in all). */
	constexpr float SetupAfter = 0.1f;
	constexpr float ThrowAfter = 0.8f;
	constexpr float ClientBombAfter = 1.7f;
	constexpr float JudgeAfter = 3.3f;
	/** The platform: this many rows over the island's highest block, this many columns, gone after this long. */
	constexpr int32 PlatformRowsUp = 24;
	constexpr int32 PlatformColumns = 26;
	constexpr float PlatformLife = 4.6f;
	constexpr int32 HostColumn = 5;
	constexpr float ClientGap = 600.f;           // from the host's penguin to the client's (cm, to the right)
	constexpr float ClientBombGap = 200.f;       // from the client's capsule to its bomb (cm, to its right): inside the push radius, outside the kill radius
	constexpr float BombFuse = 0.6f;             // the bomb dropped at the host's feet (APenguinAbilities' short fuse) and the client's
	/** Judged: a bomb's knock-out comes this long (s) after the bomb showed here; the client's hit the same; the client flies at
	 *  least this fast (cm/s); a client sees the host's feed line within this long after the knock-out. */
	constexpr float KOFrom = 0.3f;
	constexpr float KOTo = 1.5f;
	constexpr float HitFrom = 0.25f;
	constexpr float HitTo = 1.3f;
	constexpr float MinPushSpeed = 400.f;

	/** One check per process (the client logs its results in a solo world after the game). */
	struct FSelfBombProgress
	{
		bool bJudged = false;
		bool bOk = false;
		FString Detail;
		// this round's check
		bool bSetUp = false;
		bool bWatching = false;
		bool bThrown = false;
		bool bUsed = false;
		bool bClientBombMade = false;
		float BombSeenAt = -1.f;          // WorldTime the host's bomb showed here
		float KOSeenAt = -1.f;            // WorldTime the host's penguin was seen knocked out here
		uint8 KOReason = 0;
		bool bKOSelf = false;             // (the host only: the flag is the server's)
		FString FeedLine;                 // what the feed said about it
		int32 KOsBefore = -1;             // knock-outs (kills) counted in all the score rows, and the host's falls, when the check began
		int32 FallsBefore = -1;
		int32 KOsAfter = -1;
		int32 FallsAfter = -1;
		float ClientBombSeenAt = -1.f;
		float ClientHitAt = -1.f;         // s after the client's bomb showed here
		float ClientHitSpeed = 0.f;
		bool bClientHitAway = false;
		bool bClientAlive = true;
		int32 ClientHitsAtStart = 0;
		FVector ClientBombAt = FVector::ZeroVector;
	};

	FSelfBombProgress& SelfBombProgress()
	{
		static FSelfBombProgress Instance;
		return Instance;
	}

	int32 TotalKills(const APenguinGameState* GameState)
	{
		int32 Total = 0;
		for (const FPinguScore& Score : GameState->GetScores())
		{
			Total += Score.KOs;
		}
		return Total;
	}

	int32 FallsOf(const APenguinGameState* GameState, const APenguinCharacter* Penguin)
	{
		const FPinguScore* Score = Penguin ? GameState->FindScore(Penguin->GetPlayerSlot()) : nullptr;
		return Score ? Score->Falls : -1;
	}
}

void APinguNetTest::ResetSelfBombCheck()
{
	using namespace PinguNetTestSelfBombPrivate;
	FSelfBombProgress& P = SelfBombProgress();
	const bool bJudged = P.bJudged;
	const bool bOk = P.bOk;
	const FString Detail = P.Detail;
	P = FSelfBombProgress();
	P.bJudged = bJudged;
	P.bOk = bOk;
	P.Detail = Detail;
}

void APinguNetTest::UpdateSelfBombCheck(float T)
{
	using namespace PinguNetTestSelfBombPrivate;
	FSelfBombProgress& P = SelfBombProgress();
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!HostPenguin || !ClientPenguin || !Island || !GameState || P.bJudged)
	{
		return;
	}
	UPenguinMovementComponent* ClientMove = ClientPenguin->GetPenguinMovement();
	if (!P.bWatching)
	{
		P.bWatching = true;
		P.KOsBefore = TotalKills(GameState);
		P.FallsBefore = FallsOf(GameState, HostPenguin);
		P.ClientHitsAtStart = ClientMove ? ClientMove->HitEvents : 0;
		Report(FString::Printf(TEXT("self bomb check: the host's penguin drops a bomb at its own feet, then a bomb of the client's goes off 2 m from the client's penguin (kills %d, host falls %d)"),
			P.KOsBefore, P.FallsBefore));
	}

	// Host: the platform high above everything, the two penguins on it 6 m apart.
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
			PinguNetTestGrant::Give(HostAbilities, EPenguinAbility::SnowBomb); // ready
		}
		const float Radius = ClientPenguin->GetCapsuleComponent()->GetScaledCapsuleRadius();
		P.ClientBombAt = FVector(HostX + ClientGap + Radius + ClientBombGap, 0.f, FloorZ + 34.f);
		Report(FString::Printf(TEXT("self bomb check: platform of %d blocks at z %.0f, the two penguins %.0f cm apart"), Added, FloorZ, ClientGap));
	}

	// Host: its own bomb, aimed straight down at its feet (dropped there on a short fuse).
	if (bHostRole && P.bSetUp && !P.bThrown && T >= ThrowAfter)
	{
		P.bThrown = true;
		UPenguinAbilityComponent* HostAbilities = HostPenguin->GetAbilities();
		const int32 Slot = HostAbilities ? HostAbilities->FindSlot(EPenguinAbility::SnowBomb) : INDEX_NONE;
		P.bUsed = Slot != INDEX_NONE && HostAbilities->UseAbility(Slot, HostPenguin->GetAimOrigin() + FVector(0.f, 0.f, -100.f));
		Report(FString::Printf(TEXT("self bomb check: the host's penguin drops a bomb at its feet (%d)"), P.bUsed ? 1 : 0));
	}

	// Host: the client's bomb, "thrown" by the client's penguin, on the platform to its right.
	if (bHostRole && P.bThrown && !P.bClientBombMade && T >= ClientBombAfter)
	{
		P.bClientBombMade = true;
		APenguinProjectile* Bomb = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Bomb, ClientPenguin, P.ClientBombAt, FVector::ZeroVector);
		if (Bomb)
		{
			Bomb->Detonate(BombFuse);
		}
		Report(FString::Printf(TEXT("self bomb check: the client's bomb lies %.0f cm from its penguin (%d)"), ClientBombGap, Bomb ? 1 : 0));
	}

	// Both: what this machine sees - the bombs, the host's knock-out, the feed line, the client's hit.
	for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
	{
		if (It->GetKind() != EPenguinProjectileKind::Bomb || !It->IsFlying())
		{
			continue;
		}
		if (P.BombSeenAt < 0.f && It->GetShooter() == HostPenguin)
		{
			P.BombSeenAt = WorldTime;
		}
		else if (P.ClientBombSeenAt < 0.f && It->GetShooter() == ClientPenguin)
		{
			P.ClientBombSeenAt = WorldTime;
			P.ClientHitsAtStart = ClientMove ? ClientMove->HitEvents : P.ClientHitsAtStart;
		}
	}
	if (P.KOSeenAt < 0.f && P.BombSeenAt >= 0.f && HostPenguin->IsKnockedOut())
	{
		P.KOSeenAt = WorldTime;
		P.KOReason = static_cast<uint8>(HostPenguin->GetKOReason());
		P.bKOSelf = HostPenguin->WasKOSelfInflicted();
		Report(FString::Printf(TEXT("self bomb check: the host's penguin is knocked out, %.2f s after its bomb showed here (reason %d)"), P.KOSeenAt - P.BombSeenAt, P.KOReason));
	}
	if (P.FeedLine.IsEmpty())
	{
		for (const FPinguFeedEntry& Entry : GameState->GetFeed())
		{
			if (Entry.Text.Contains(TEXT("blew themselves up")) && Entry.Text.Contains(HostPenguin->GetDisplayName()))
			{
				P.FeedLine = Entry.Text;
				break;
			}
		}
	}
	if (P.ClientBombSeenAt >= 0.f && P.ClientHitAt < 0.f && ClientMove && ClientMove->HitEvents != P.ClientHitsAtStart)
	{
		P.ClientHitAt = WorldTime - P.ClientBombSeenAt;
		P.ClientHitSpeed = static_cast<float>(ClientMove->Velocity.Size());
		P.bClientHitAway = ClientMove->Velocity.X < 0.f; // the bomb lay to its right
		Report(FString::Printf(TEXT("self bomb check: the client's penguin is hit %.2f s after its bomb showed here, %.0f cm/s, away from the bomb %d"), P.ClientHitAt, P.ClientHitSpeed,
			P.bClientHitAway ? 1 : 0));
	}
	if (P.ClientHitAt >= 0.f)
	{
		P.bClientAlive = P.bClientAlive && ClientPenguin->IsAlive();
	}
	if (T >= JudgeAfter)
	{
		P.KOsAfter = TotalKills(GameState);
		P.FallsAfter = FallsOf(GameState, HostPenguin);
		JudgeSelfBombCheck(TEXT("judged after the blasts"));
	}
}

void APinguNetTest::JudgeSelfBombCheck(const TCHAR* Why)
{
	using namespace PinguNetTestSelfBombPrivate;
	FSelfBombProgress& P = SelfBombProgress();
	if (P.bJudged || !P.bWatching)
	{
		return; // (it never started: logged as not run)
	}
	P.bJudged = true;
	const float KODelay = P.KOSeenAt >= 0.f && P.BombSeenAt >= 0.f ? P.KOSeenAt - P.BombSeenAt : -1.f;
	const bool bKO = KODelay >= KOFrom && KODelay <= KOTo && P.KOReason == static_cast<uint8>(EPenguinKOReason::Bomb);
	const bool bFeed = !P.FeedLine.IsEmpty();
	const bool bNoKill = P.KOsAfter >= 0 && P.KOsAfter == P.KOsBefore;
	const bool bFall = P.FallsBefore >= 0 && P.FallsAfter == P.FallsBefore + 1;
	const bool bHit = P.ClientHitAt >= HitFrom && P.ClientHitAt <= HitTo && P.bClientHitAway && P.ClientHitSpeed >= MinPushSpeed && P.bClientAlive;
	P.bOk = bKO && bFeed && bNoKill && bFall && bHit && (!bHostRole || (P.bUsed && P.bKOSelf));
	P.Detail = FString::Printf(TEXT("%s; the host's own bomb: its penguin knocked out %.2f s after the bomb showed here (want %.1f to %.1f) with reason %d (want %d, Bomb)%s, the feed said \"%s\", "
		"kills in the score rows %d -> %d (want no change), the host's falls %d -> %d (want +1); the client's bomb: its penguin hit %.2f s after it showed here (want %.2f to %.1f) at %.0f cm/s "
		"(want %.0f or more) away from the bomb %d, still alive %d"),
		Why, KODelay, KOFrom, KOTo, P.KOReason, static_cast<int32>(EPenguinKOReason::Bomb),
		bHostRole ? *FString::Printf(TEXT(", self-inflicted %d, used %d"), P.bKOSelf ? 1 : 0, P.bUsed ? 1 : 0) : TEXT(""), *P.FeedLine, P.KOsBefore, P.KOsAfter, P.FallsBefore, P.FallsAfter,
		P.ClientHitAt, HitFrom, HitTo, P.ClientHitSpeed, MinPushSpeed, P.bClientHitAway ? 1 : 0, P.bClientAlive ? 1 : 0);
	Report(FString::Printf(TEXT("self bomb check: %s (%s)"), P.bOk ? TEXT("ok") : TEXT("FAIL"), *P.Detail));
}

void APinguNetTest::AddSelfBombChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestSelfBombPrivate;
	const FSelfBombProgress& P = SelfBombProgress();
	Check(TEXT("a bomb knocks out its own thrower (seen on both machines, the feed says so, no kill for anyone) and pushes the client's penguin"), P.bOk,
		P.bJudged ? P.Detail : FString(TEXT("not run")));
}

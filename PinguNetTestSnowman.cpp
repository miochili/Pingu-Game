// Pingu - the LAN test's snowman check (APinguNetTest::UpdateSnowmanCheck, JudgeSnowmanCheck, AddSnowmanChecks): round 2, the very
// last part of the hold (after the giant snowball), SnowmanCheckLength seconds. The host builds a platform under the island
// (2026-11-05: it was high above everything - past the build limit now; below is always allowed), puts its own penguin and the
// client's on it 5 m apart and builds a snowman with the real ability: both machines see it stand, see it throw a snowball and
// see the client's penguin hit by it (the hit reaches the client's own machine). Then the host breaks it and both see it gone.
//
// Then the build limit (2026-11-05, card build-limit-snowman; PinguBuildLimit): the host puts the client's penguin on a short
// platform beside the island just under the top line and gives it the Ice Wall; the CLIENT presses it, aimed up over the line.
// The host refuses it (a "no" puff, nothing built, no cooldown): both machines see no block over the line and the client's ice
// wall still ready - a joiner can't place past the limit.

#include "PinguNetTest.h"
#include "PinguNetTestGrant.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguBuildLimit.h"
#include "PinguSnowman.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PinguNetTestSnowmanPrivate
{
	/** Times into the check's part of the hold (s; APinguNetTest::SnowmanCheckLength in all). */
	constexpr float SetupAfter = 0.1f;
	constexpr float UseAfter = 0.4f;
	constexpr float BreakAfter = 3.2f;
	constexpr float JudgeAfter = 4.2f;
	/** The platform: this many rows under the island's lowest block (never closer to the sea than PlatformOverSea cm), columns,
	 *  life. */
	constexpr int32 PlatformRowsDown = 3;
	constexpr float PlatformOverSea = 400.f;
	constexpr int32 PlatformColumns = 20;
	constexpr float PlatformLife = 4.6f;
	constexpr int32 HostColumn = 5;
	constexpr float ClientGap = 500.f;
	/** The build limit part (after JudgeAfter): set up, the client's press, judged; the platform's columns beside the island, its
	 *  top this far under the line, its life. */
	constexpr float LimitSetupAfter = 4.4f;
	constexpr float LimitPressAfter = 4.9f;
	constexpr float LimitJudgeAfter = 6.1f;
	constexpr int32 LimitPlatformColumns = 4;
	constexpr float LimitPlatformUnderTop = 200.f;
	constexpr float LimitPlatformLife = 2.2f;

	struct FLimitProgress
	{
		bool bJudged = false;
		bool bOk = false;
		FString Detail;
		bool bSetUp = false;
		bool bPressed = false;
		bool bPressSent = false;
		int32 RefusalsAtStart = -1;
		int32 MostAbove = 0;
		int32 AboveAtStart = 0;
		float MostCooldown = 0.f;
		bool bLimit = false;
		float TopZ = 0.f;
	};

	FLimitProgress& LimitProgress()
	{
		static FLimitProgress Instance;
		return Instance;
	}

	/** Blocks of Island whose top is over the line, in the columns around it. */
	static int32 CountAboveLine(const ABlockIsland* Island, const FPinguBuildLimit& Limit)
	{
		const float OriginZ = static_cast<float>(Island->GetActorLocation().Z);
		const int32 TopRow = FMath::FloorToInt((Limit.TopZ - OriginZ) / Island->CellSize);
		int32 Count = 0;
		for (int32 Column = Limit.MinColumn - 8; Column <= Limit.MaxColumn + 8; ++Column)
		{
			for (int32 Row = TopRow; Row <= TopRow + 12; ++Row)
			{
				Count += Island->IsSolidCell(FIntPoint(Column, Row)) ? 1 : 0;
			}
		}
		return Count;
	}
	struct FSnowmanProgress
	{
		bool bJudged = false;
		bool bOk = false;
		FString Detail;
		bool bWatching = false;
		bool bSetUp = false;
		bool bUsed = false;
		bool bBroken = false;
		bool bStandingSeen = false;
		int32 PartsSeen = 0;
		float SeenAt = -1.f;
		bool bThrowSeen = false;
		int32 ClientHitsAtStart = -1;
		bool bClientHit = false;
		bool bGoneSeen = false;
	};

	FSnowmanProgress& SnowmanProgress()
	{
		static FSnowmanProgress Instance;
		return Instance;
	}
}

void APinguNetTest::ResetSnowmanCheck()
{
	using namespace PinguNetTestSnowmanPrivate;
	FSnowmanProgress& P = SnowmanProgress();
	const bool bJudged = P.bJudged;
	const bool bOk = P.bOk;
	const FString Detail = P.Detail;
	P = FSnowmanProgress();
	P.bJudged = bJudged;
	P.bOk = bOk;
	P.Detail = Detail;
	FLimitProgress& L = LimitProgress();
	const bool bLimitJudged = L.bJudged;
	const bool bLimitOk = L.bOk;
	const FString LimitDetail = L.Detail;
	L = FLimitProgress();
	L.bJudged = bLimitJudged;
	L.bOk = bLimitOk;
	L.Detail = LimitDetail;
}

void APinguNetTest::UpdateSnowmanCheck(float T)
{
	using namespace PinguNetTestSnowmanPrivate;
	FSnowmanProgress& P = SnowmanProgress();
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!HostPenguin || !ClientPenguin || !Island || (P.bJudged && LimitProgress().bJudged))
	{
		return;
	}
	const UPenguinMovementComponent* ClientMove = ClientPenguin->GetPenguinMovement();
	if (!P.bWatching)
	{
		P.bWatching = true;
		P.ClientHitsAtStart = ClientMove ? ClientMove->HitEvents : 0;
		Report(TEXT("snowman check: the host's penguin builds a snowman that throws at the client's penguin, then it breaks"));
	}
	if (bHostRole && !P.bSetUp && T >= SetupAfter)
	{
		P.bSetUp = true;
		APinguSnowman::DestroyAll(GetWorld());
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
		Island->AddBlocks(Cells, EBlockKind::Rock, PlatformLife);
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
			PinguNetTestGrant::Give(HostAbilities, EPenguinAbility::Snowman); // ready (with a bubble that goes, as a pickup)
		}
	}
	if (bHostRole && P.bSetUp && !P.bUsed && T >= UseAfter)
	{
		UPenguinAbilityComponent* HostAbilities = HostPenguin->GetAbilities();
		const int32 Slot = HostAbilities ? HostAbilities->FindSlot(EPenguinAbility::Snowman) : INDEX_NONE;
		P.bUsed = Slot != INDEX_NONE && HostAbilities->UseAbility(Slot, HostPenguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
		Report(FString::Printf(TEXT("snowman check: the host's penguin builds a snowman (%d)"), P.bUsed ? 1 : 0));
	}

	// Both: what this machine sees.
	APinguSnowman* Snowman = APinguSnowman::FindOf(GetWorld(), HostPenguin);
	if (Snowman)
	{
		if (P.SeenAt < 0.f)
		{
			P.SeenAt = T;
		}
		P.bStandingSeen |= Snowman->IsStanding();
		P.PartsSeen = FMath::Max(P.PartsSeen, Snowman->GetShownParts());
		for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
		{
			if (It->GetKind() == EPenguinProjectileKind::Snowball && It->GetShooter() == HostPenguin && It->IsFlying())
			{
				P.bThrowSeen = true;
			}
		}
	}
	else if (P.SeenAt >= 0.f && T > BreakAfter)
	{
		P.bGoneSeen = true;
	}
	if (ClientMove && P.ClientHitsAtStart >= 0 && ClientMove->HitEvents > P.ClientHitsAtStart)
	{
		P.bClientHit = true;
	}
	if (bHostRole && Snowman && !P.bBroken && T >= BreakAfter)
	{
		P.bBroken = true;
		Snowman->Break(nullptr, TEXT("net test"));
	}
	if (T >= JudgeAfter)
	{
		JudgeSnowmanCheck(TEXT("judged after the break"));
	}

	// ---- The build limit: the client's ice wall aimed over the top line is refused by the host.
	FLimitProgress& L = LimitProgress();
	if (L.bJudged || T < LimitSetupAfter)
	{
		return;
	}
	FPinguBuildLimit Limit;
	L.bLimit = PinguBuildLimit::Get(Island, Limit); // (every machine works it out from the same island)
	L.TopZ = Limit.TopZ;
	if (!L.bLimit)
	{
		if (T >= LimitJudgeAfter)
		{
			L.bJudged = true;
			L.Detail = TEXT("this island has no build limit");
			Report(FString::Printf(TEXT("build limit check: FAIL (%s)"), *L.Detail));
		}
		return;
	}
	UPenguinAbilityComponent* ClientAbilities = ClientPenguin->GetAbilities();
	const int32 WallSlot = ClientAbilities ? ClientAbilities->FindSlot(EPenguinAbility::IceWall) : INDEX_NONE;
	if (!L.bSetUp)
	{
		L.bSetUp = true;
		L.RefusalsAtStart = PinguBuildLimit::GetRefusals();
		L.AboveAtStart = CountAboveLine(Island, Limit); // (the tests' own platforms up there, if any still stand)
		Report(FString::Printf(TEXT("build limit check: the client's ice wall aimed over the top line (z %.0f) beside the island"), Limit.TopZ));
		if (bHostRole)
		{
			// A short platform beside the island (columns 2 - 5 out), its top LimitPlatformUnderTop under the line; the client's
			// penguin on it with the Ice Wall.
			const float OriginZ = static_cast<float>(Island->GetActorLocation().Z);
			const int32 Row = FMath::FloorToInt((Limit.TopZ - LimitPlatformUnderTop - OriginZ) / Island->CellSize) - 1;
			const int32 FirstColumn = Limit.MaxColumn - PinguBuildLimit::SideCells + 2;
			TArray<FIntPoint> Cells;
			for (int32 Column = 0; Column < LimitPlatformColumns; ++Column)
			{
				Cells.Add(FIntPoint(FirstColumn + Column, Row));
			}
			Island->AddBlocks(Cells, EBlockKind::Rock, LimitPlatformLife);
			Island->FlushEdits();
			const FVector Top = Island->CellToWorld(FIntPoint(FirstColumn + 1, Row)) + FVector(0.f, 0.f, Island->CellSize * 0.5f);
			const float Half = ClientPenguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			ClientPenguin->NetTeleport(Top + FVector(0.f, 0.f, Half + 2.f), FVector::ZeroVector, true);
			// The host's penguin too, on the platform's far column: the snowman's platform is gone by now, and its fall into the sea
			// would end the round before this check is judged (2026-11-06).
			HostPenguin->NetTeleport(Top + FVector(2.f * Island->CellSize, 0.f, Half + 2.f), FVector::ZeroVector, true);
			PinguNetTestGrant::Give(ClientAbilities, EPenguinAbility::IceWall);
		}
	}
	// The client presses it (its own machine sends the press to the host), aimed 3 m over its head: over the line.
	if (!bHostRole && !L.bPressSent && T >= LimitPressAfter && WallSlot != INDEX_NONE)
	{
		L.bPressSent = true;
		ClientPenguin->SetAimPoint(ClientPenguin->GetAimOrigin() + FVector(60.f, 0.f, 300.f));
		L.bPressed = ClientPenguin->ActivateAbility(WallSlot);
		Report(FString::Printf(TEXT("build limit check: the client presses its ice wall (%d)"), L.bPressed ? 1 : 0));
	}
	if (T >= LimitPressAfter)
	{
		L.MostAbove = FMath::Max(L.MostAbove, CountAboveLine(Island, Limit) - L.AboveAtStart);
		if (WallSlot != INDEX_NONE)
		{
			L.MostCooldown = FMath::Max(L.MostCooldown, ClientAbilities->GetCooldownRemaining(WallSlot));
		}
	}
	if (T >= LimitJudgeAfter)
	{
		L.bJudged = true;
		const int32 Refused = PinguBuildLimit::GetRefusals() - L.RefusalsAtStart;
		// The host: one refusal (the "no" puff is made there). Both: no block over the line, the client's ice wall never cooling
		// down (nothing spent), the ability in its slot.
		L.bOk = WallSlot != INDEX_NONE && L.MostAbove == 0 && L.MostCooldown <= 0.f && (bHostRole ? Refused == 1 : L.bPressed);
		L.Detail = FString::Printf(TEXT("the client's ice wall in slot %d, pressed %d, refused on the host %d (want 1 there), blocks over the line %d (want 0), its cooldown at most %.2f s (want 0)"),
			WallSlot, L.bPressed ? 1 : 0, bHostRole ? Refused : -1, L.MostAbove, L.MostCooldown);
		Report(FString::Printf(TEXT("build limit check: %s (%s)"), L.bOk ? TEXT("ok") : TEXT("FAIL"), *L.Detail));
	}
}

void APinguNetTest::JudgeSnowmanCheck(const TCHAR* Why)
{
	using namespace PinguNetTestSnowmanPrivate;
	FSnowmanProgress& P = SnowmanProgress();
	if (P.bJudged || !P.bWatching)
	{
		return;
	}
	P.bJudged = true;
	P.bOk = P.SeenAt >= 0.f && P.bStandingSeen && P.PartsSeen >= 9 && P.bThrowSeen && P.bClientHit && P.bGoneSeen && (!bHostRole || P.bUsed);
	P.Detail = FString::Printf(TEXT("%s; the snowman seen here %d (standing %d, %d parts drawn), a snowball of it seen %d, the client's penguin hit %d, gone after the break %d"),
		Why, P.SeenAt >= 0.f ? 1 : 0, P.bStandingSeen ? 1 : 0, P.PartsSeen, P.bThrowSeen ? 1 : 0, P.bClientHit ? 1 : 0, P.bGoneSeen ? 1 : 0);
	Report(FString::Printf(TEXT("snowman check: %s (%s)"), P.bOk ? TEXT("ok") : TEXT("FAIL"), *P.Detail));
}

void APinguNetTest::AddSnowmanChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestSnowmanPrivate;
	const FSnowmanProgress& P = SnowmanProgress();
	Check(TEXT("a snowman: built and seen standing, it throws at the client's penguin and hits it, gone when broken"), P.bOk,
		P.bJudged ? P.Detail : FString(TEXT("not run")));
	const FLimitProgress& L = LimitProgress();
	Check(TEXT("the build limit: the client's ice wall over the top line refused by the host, nothing built, nothing spent"), L.bOk,
		L.bJudged ? L.Detail : FString(TEXT("not run")));
}

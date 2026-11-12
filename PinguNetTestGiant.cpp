// Pingu - the LAN test's giant snowball check (APinguNetTest::UpdateGiantCheck, JudgeGiantCheck, AddGiantChecks): round 2, the
// last part of the hold (after the ice wall and the big iceberg), GiantCheckLength seconds. The host builds a platform high above
// everything, puts its own penguin and the client's on it 6 m apart and rolls a giant snowball up with the real ability (the
// 1.5 s roll-up, its penguin hanging still), which rolls at the client's penguin and swallows it. Both machines see the roll-up
// and the roll, the client's penguin hidden in the ball (no collision there, hanging still on its own machine) and drawn
// tumbling inside it; then the host breaks the ball and both see the client's penguin out again where it broke: shown,
// colliding, free to move, alive. (Not run by the pipeline now: Linus runs every LAN test in one batch.)

#include "PinguNetTest.h"
#include "PinguNetTestGrant.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PinguGiantSnowball.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PinguNetTestGiantPrivate
{
	/** Times into the check's part of the hold (s; APinguNetTest::GiantCheckLength in all). */
	constexpr float SetupAfter = 0.1f;
	constexpr float UseAfter = 0.5f;
	constexpr float BurstAfter = 4.6f;
	constexpr float JudgeAfter = 5.6f;
	/** The platform: this many rows over the island's highest block (above the ice scale check's, whose ice wall and iceberg are
	 *  still there), this many columns, gone after this long. */
	constexpr int32 PlatformRowsUp = 32;
	constexpr int32 PlatformColumns = 26;
	constexpr float PlatformLife = 6.2f;
	constexpr int32 HostColumn = 5;
	constexpr float ClientGap = 600.f; // from the host's penguin to the client's (cm, to the right)

	struct FGiantProgress
	{
		bool bJudged = false;
		bool bOk = false;
		FString Detail;
		// this round's check
		bool bWatching = false;
		bool bSetUp = false;
		bool bUsed = false;
		bool bBurst = false;
		float HostX0 = 0.f;
		bool bBuildSeen = false;
		bool bHostHoverSeen = false;
		float HostDrift = 0.f;
		bool bRollSeen = false;
		bool bSwallowSeen = false;     // the client's penguin in the ball and hidden here
		bool bNoCollisionSeen = false; // ... without collision here
		bool bOwnHoverSeen = false;    // (the client: its own penguin hangs still on its own machine)
		int32 ShownInside = 0;
		float SizeAtSwallow = -1.f;
		FVector LastBallAt = FVector::ZeroVector;
		float LastBallRadius = 0.f;
		float GoneAt = -1.f;
		bool bOutChecked = false;
		bool bOutShown = false;
		bool bOutColliding = false;
		bool bOutFree = false;
		bool bOutAlive = false;
		float OutDistance = -1.f;
	};

	FGiantProgress& GiantProgress()
	{
		static FGiantProgress Instance;
		return Instance;
	}
}

void APinguNetTest::ResetGiantCheck()
{
	using namespace PinguNetTestGiantPrivate;
	FGiantProgress& P = GiantProgress();
	const bool bJudged = P.bJudged;
	const bool bOk = P.bOk;
	const FString Detail = P.Detail;
	P = FGiantProgress();
	P.bJudged = bJudged;
	P.bOk = bOk;
	P.Detail = Detail;
}

void APinguNetTest::UpdateGiantCheck(float T)
{
	using namespace PinguNetTestGiantPrivate;
	FGiantProgress& P = GiantProgress();
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
		Report(TEXT("giant check: the host's penguin rolls a giant snowball at the client's, which is swallowed, then the ball breaks"));
	}

	// Host: the platform, the two penguins on it, the ability ready; then the real use (the roll-up, then it rolls right).
	if (bHostRole && !P.bSetUp && T >= SetupAfter)
	{
		P.bSetUp = true;
		APinguGiantSnowball::DestroyAll(GetWorld());
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
			PinguNetTestGrant::Give(HostAbilities, EPenguinAbility::GiantSnowball); // ready
		}
		Report(FString::Printf(TEXT("giant check: platform of %d blocks at z %.0f, the two penguins %.0f cm apart"), Added, FloorZ, ClientGap));
	}
	if (bHostRole && P.bSetUp && !P.bUsed && T >= UseAfter)
	{
		UPenguinAbilityComponent* HostAbilities = HostPenguin->GetAbilities();
		const int32 Slot = HostAbilities ? HostAbilities->FindSlot(EPenguinAbility::GiantSnowball) : INDEX_NONE;
		P.bUsed = Slot != INDEX_NONE && HostAbilities->UseAbility(Slot, HostPenguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f));
		Report(FString::Printf(TEXT("giant check: the host's penguin rolls up a giant snowball (%d)"), P.bUsed ? 1 : 0));
	}

	// Both: what this machine sees.
	TArray<APinguGiantSnowball*> Balls;
	APinguGiantSnowball::GetAll(GetWorld(), Balls);
	APinguGiantSnowball* Ball = nullptr;
	for (APinguGiantSnowball* Each : Balls)
	{
		if (Each->GetRoller() == HostPenguin)
		{
			Ball = Each;
		}
	}
	const UPenguinMovementComponent* HostMove = HostPenguin->GetPenguinMovement();
	const UPenguinMovementComponent* ClientMove = ClientPenguin->GetPenguinMovement();
	if (Ball)
	{
		if (Ball->IsBuilding())
		{
			if (!P.bBuildSeen)
			{
				P.bBuildSeen = true;
				P.HostX0 = static_cast<float>(HostPenguin->GetActorLocation().X);
			}
			P.HostDrift = FMath::Max(P.HostDrift, FMath::Abs(static_cast<float>(HostPenguin->GetActorLocation().X) - P.HostX0));
			P.bHostHoverSeen |= HostMove && HostMove->IsHovering();
		}
		P.bRollSeen |= Ball->IsRolling();
		if (Ball->HoldsPenguin(ClientPenguin) && ClientPenguin->IsHidden())
		{
			if (!P.bSwallowSeen)
			{
				P.bSwallowSeen = true;
				P.SizeAtSwallow = Ball->GetDiameter();
				Report(FString::Printf(TEXT("giant check: the client's penguin is in the ball here (%.0f cm)"), P.SizeAtSwallow));
			}
			P.bNoCollisionSeen |= !ClientPenguin->GetActorEnableCollision();
			P.bOwnHoverSeen |= bHostRole || (ClientMove && ClientMove->IsHovering());
			P.ShownInside = FMath::Max(P.ShownInside, Ball->GetShownInside());
		}
		P.LastBallAt = Ball->GetActorLocation();
		P.LastBallRadius = Ball->GetRadius();
	}
	else if (P.bRollSeen && P.GoneAt < 0.f)
	{
		P.GoneAt = T;
	}

	// Host: it breaks the ball (the client's penguin comes out where it broke).
	if (bHostRole && Ball && !P.bBurst && T >= BurstAfter)
	{
		P.bBurst = true;
		Ball->Burst(nullptr, TEXT("net test"));
	}
	// Both: half a second after it went, the client's penguin is out.
	if (P.GoneAt >= 0.f && !P.bOutChecked && T >= P.GoneAt + 0.5f)
	{
		P.bOutChecked = true;
		P.bOutShown = !ClientPenguin->IsHidden();
		P.bOutColliding = ClientPenguin->GetActorEnableCollision();
		P.bOutFree = ClientMove && !ClientMove->IsHovering();
		P.bOutAlive = ClientPenguin->IsAlive();
		P.OutDistance = FMath::Abs(static_cast<float>(ClientPenguin->GetActorLocation().X - P.LastBallAt.X));
	}
	if (T >= JudgeAfter)
	{
		JudgeGiantCheck(TEXT("judged after the burst"));
	}
}

void APinguNetTest::JudgeGiantCheck(const TCHAR* Why)
{
	using namespace PinguNetTestGiantPrivate;
	FGiantProgress& P = GiantProgress();
	if (P.bJudged || !P.bWatching)
	{
		return;
	}
	P.bJudged = true;
	const bool bBuild = P.bBuildSeen && P.bHostHoverSeen && P.HostDrift < 10.f && P.bRollSeen;
	const bool bInside = P.bSwallowSeen && P.bNoCollisionSeen && P.bOwnHoverSeen && P.ShownInside >= 1 && P.SizeAtSwallow > 100.f
		&& P.SizeAtSwallow < PinguTuning::GiantRollMaxSize;
	const bool bOut = P.bOutChecked && P.bOutShown && P.bOutColliding && P.bOutFree && P.bOutAlive && P.OutDistance < P.LastBallRadius + 200.f;
	P.bOk = bBuild && bInside && bOut && (!bHostRole || P.bUsed);
	P.Detail = FString::Printf(TEXT("%s; the roll-up seen %d (the host's penguin hanging still %d, drifted %.0f cm), rolling seen %d; the client's penguin swallowed here %d "
		"(no collision %d, its own hanging still %d, drawn inside %d, the ball %.0f cm then); out again %d (shown %d, colliding %d, free %d, alive %d, %.0f cm from where it broke)"),
		Why, P.bBuildSeen ? 1 : 0, P.bHostHoverSeen ? 1 : 0, P.HostDrift, P.bRollSeen ? 1 : 0, P.bSwallowSeen ? 1 : 0, P.bNoCollisionSeen ? 1 : 0, P.bOwnHoverSeen ? 1 : 0,
		P.ShownInside, P.SizeAtSwallow, P.bOutChecked ? 1 : 0, P.bOutShown ? 1 : 0, P.bOutColliding ? 1 : 0, P.bOutFree ? 1 : 0, P.bOutAlive ? 1 : 0, P.OutDistance);
	Report(FString::Printf(TEXT("giant check: %s (%s)"), P.bOk ? TEXT("ok") : TEXT("FAIL"), *P.Detail));
}

void APinguNetTest::AddGiantChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestGiantPrivate;
	const FGiantProgress& P = GiantProgress();
	Check(TEXT("a giant snowball: the roll-up and the roll seen, the client's penguin swallowed (hidden, no collision, drawn inside) and out again where it broke"),
		P.bOk, P.bJudged ? P.Detail : FString(TEXT("not run")));
}

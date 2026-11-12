// Pingu - the LAN test's island checks (APinguNetTest::UpdateIslandWatch, AddIslandChecks): every fight the client's island
// is the host's and made for the fight's penguins, and the camera follows this machine's own penguin; in the lobby a warp of
// the client's penguin ends on the client's own screen (it sends the host a narrow test screen, the host fires the warp far
// past it); the late join: the island grows from two penguins to three when the late player comes in.

#include "PinguNetTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinPlayerController.h"
#include "PinguPortal.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguNetTestIslandPrivate
{
	/** A fight is judged this long after it started (s): the island and the camera have settled. */
	constexpr float FightJudgeAfter = 0.5f;
	/** Client: the test's screen for the warp, this far around its penguin (cm: sideways, below, above) - much smaller than
	 *  a real one, so the warp must end short; centred again when the penguin gets this close to its side (share). */
	constexpr float WarpViewHalfWidth = 450.f;
	constexpr float WarpViewBelow = 300.f;
	constexpr float WarpViewAbove = 350.f;
	constexpr float WarpRecentreShare = 0.6f;
	/** Client: the screens of this many seconds back count (the host's copy is a little old). */
	constexpr float WarpViewHistory = 0.6f;
	/** Client: the exit may be this far outside such a screen (cm). */
	constexpr float WarpTolerance = 10.f;
	/** Host: the client's screen is the test's once it is narrower than this (cm) ... */
	constexpr double WarpViewNarrow = 1200.0;
	/** ... and this long after the client's penguin was first seen in the lobby (s: the lobby has its new island by then). */
	constexpr float WarpFireAfter = 1.2f;
	constexpr int32 WarpMaxTries = 120;
	/** Host: aimed this far to the right and up from the client's penguin (far off its test screen). */
	constexpr float WarpAimFar = 3000.f;
	constexpr float WarpAimUp = 500.f;

	struct FPastView
	{
		float Time = 0.f;
		FBox2D View = FBox2D(ForceInit);
	};

	/** What the island checks know across the map loads (one test per process; the client logs its results in a solo world). */
	struct FIslandProgress
	{
		int32 LastFightRound = -1;
		int32 Fights = 0;
		int32 FightsSized = 0;
		int32 FightsFollowing = 0;
		FString SizeDetail;
		FString FollowDetail;
		int32 FirstSize = -1;
		int32 LastSize = -1;
		// the warp, client
		bool bWarpWatching = false;
		bool bWarpJudged = false;
		bool bWarpOk = false;
		FString WarpDetail;
		TArray<FPastView> Views;
		// the warp, host
		float ClientSeenAt = -1.f;
		bool bWarpFired = false;
		float WarpFiredAt = -1.f;
		int32 WarpTries = 0;
	};

	FIslandProgress& IslandProgress()
	{
		static FIslandProgress Instance;
		return Instance;
	}

	bool IsLateTest()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("PinguNetLate"));
	}
}

void APinguNetTest::UpdateIslandWatch()
{
	using namespace PinguNetTestIslandPrivate;
	FIslandProgress& Island = IslandProgress();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	ABlockIsland* Arena = ABlockIsland::Get(GetWorld());
	APenguinCamera* StageCamera = APenguinCamera::FindOrSpawn(GetWorld()); // (a client: null until the host's arrived)
	if (!GameState || !Arena)
	{
		return;
	}
	const EPinguPhase Phase = GameState->GetPhase();

	// ---- Every fight: the island made for the fight's penguins, a client's the host's; the camera on our own penguin.
	if (Phase == EPinguPhase::Fight && GameState->GetRoundNumber() != Island.LastFightRound && GameState->GetPhaseTime() >= FightJudgeAfter)
	{
		Island.LastFightRound = GameState->GetRoundNumber();
		++Island.Fights;
		const int32 Here = CountPenguins();
		const int32 Sized = Arena->GetArenaPenguins();
		const bool bHostBlocks = Arena->MatchesHostIsland();
		const FBox Bounds = Arena->GetBounds();
		const float Width = Bounds.IsValid ? static_cast<float>(Bounds.Max.X - Bounds.Min.X) / 100.f : 0.f;
		Island.FightsSized += Sized == Here && bHostBlocks ? 1 : 0;
		Island.SizeDetail += FString::Printf(TEXT("%sround %d: for %d (%d penguins here), %.0f m, the host's blocks %d"), Island.SizeDetail.IsEmpty() ? TEXT("") : TEXT("; "),
			Island.LastFightRound, Sized, Here, Width, bHostBlocks ? 1 : 0);
		if (Island.FirstSize < 0)
		{
			Island.FirstSize = Sized;
		}
		Island.LastSize = Sized;
		const APenguinCharacter* Own = GetOwnPenguin();
		const bool bOwnIn = Own && Own->IsAlive();
		const bool bFollows = !bOwnIn || (StageCamera && StageCamera->GetFollowPenguin() == Own);
		Island.FightsFollowing += bFollows ? 1 : 0;
		Island.FollowDetail += FString::Printf(TEXT("%sround %d: %s"), Island.FollowDetail.IsEmpty() ? TEXT("") : TEXT("; "), Island.LastFightRound,
			!bOwnIn ? TEXT("no penguin of ours") : (bFollows ? TEXT("ours") : TEXT("not ours")));
		Report(FString::Printf(TEXT("island round %d: made for %d penguins (%d here), %.0f m wide, the host's blocks %d; the camera %.0f cm out, following %d, on our penguin %d"),
			Island.LastFightRound, Sized, Here, Width, bHostBlocks ? 1 : 0, StageCamera ? StageCamera->GetFrameDistance() : -1.f,
			StageCamera && StageCamera->IsFollowing() ? 1 : 0, bOwnIn && bFollows ? 1 : 0));
	}

	if (IsLateTest())
	{
		return;
	}

	// ---- Snow Warp on the client's own screen, in the lobby.
	if (!bHostRole)
	{
		// Client: sends a narrow test screen around its penguin (APenguinPlayerController sends what the camera says it
		// sees), then judges where its warp ends.
		APenguinCharacter* Own = GetOwnPenguin();
		if (Island.bWarpJudged || !StageCamera)
		{
			return;
		}
		if (Phase != EPinguPhase::Lobby)
		{
			if (Island.bWarpWatching)
			{
				Island.bWarpJudged = true;
				Island.WarpDetail = TEXT("the match started before our penguin warped");
				StageCamera->SetViewRectOverride(FBox2D(ForceInit));
			}
			return;
		}
		if (!Own || !Own->IsAlive())
		{
			return;
		}
		Island.bWarpWatching = true;
		if (const APinguPortal* Portal = APinguPortal::FindOwnedBy(GetWorld(), Own))
		{
			// Warped: the exit must be in a screen we sent lately (the host had one of them).
			const FVector From = Portal->GetEnd(0);
			const FVector Exit = Portal->GetEnd(1);
			bool bInside = false;
			for (int32 Index = 0; Index < Island.Views.Num(); ++Index)
			{
				// The one in use, or one replaced only a moment ago.
				const bool bRecent = Index == Island.Views.Num() - 1 || WorldTime - Island.Views[Index + 1].Time <= WarpViewHistory;
				const FBox2D& Past = Island.Views[Index].View;
				const FBox2D Grown(Past.Min - FVector2D(WarpTolerance, WarpTolerance), Past.Max + FVector2D(WarpTolerance, WarpTolerance));
				bInside |= bRecent && Grown.IsInside(FVector2D(Exit.X, Exit.Z));
			}
			const FBox2D Shown = Island.Views.Num() > 0 ? Island.Views.Last().View : FBox2D(ForceInit);
			Island.bWarpJudged = true;
			Island.bWarpOk = bInside;
			Island.WarpDetail = FString::Printf(TEXT("warped from %.0f, %.0f to %.0f, %.0f (%.0f cm); our screen x %.0f..%.0f z %.0f..%.0f"), From.X, From.Z, Exit.X, Exit.Z,
				FVector::Dist(From, Exit), Shown.Min.X, Shown.Max.X, Shown.Min.Y, Shown.Max.Y);
			Report(TEXT("own Snow Warp: ") + Island.WarpDetail + (bInside ? TEXT(", inside it") : TEXT(", OUTSIDE it")));
			StageCamera->SetViewRectOverride(FBox2D(ForceInit));
			return;
		}
		// The test screen around our penguin (centred again when it walks close to a side).
		const FVector At = Own->GetActorLocation();
		const FBox2D* Current = Island.Views.Num() > 0 ? &Island.Views.Last().View : nullptr;
		if (!Current || FMath::Abs(At.X - Current->GetCenter().X) > WarpRecentreShare * WarpViewHalfWidth
			|| At.Z < Current->Min.Y + (1.f - WarpRecentreShare) * WarpViewBelow || At.Z > Current->Max.Y - (1.f - WarpRecentreShare) * WarpViewAbove)
		{
			FPastView& Next = Island.Views.AddDefaulted_GetRef();
			Next.Time = WorldTime;
			Next.View = FBox2D(FVector2D(At.X - WarpViewHalfWidth, At.Z - WarpViewBelow), FVector2D(At.X + WarpViewHalfWidth, At.Z + WarpViewAbove));
			StageCamera->SetViewRectOverride(Next.View);
			if (Island.Views.Num() > 8)
			{
				Island.Views.RemoveAt(0);
			}
		}
		return;
	}

	// Host: once the client's (narrow) screen is here and its penguin inside it, the client's penguin gets Snow Warp and warps
	// far past it, to the right and up.
	if (Phase != EPinguPhase::Lobby || Island.bWarpFired)
	{
		return;
	}
	APenguinCharacter* Client = GetOtherPlayerPenguin();
	if (!Client || !Client->IsAlive())
	{
		return;
	}
	if (Island.ClientSeenAt < 0.f)
	{
		Island.ClientSeenAt = WorldTime;
	}
	const APenguinPlayerController* ClientController = Cast<APenguinPlayerController>(Client->GetController());
	FBox2D ClientView(ForceInit);
	const FVector From = Client->GetActorLocation();
	if (WorldTime - Island.ClientSeenAt < WarpFireAfter || !ClientController || !ClientController->GetWarpView(ClientView)
		|| ClientView.GetSize().X > WarpViewNarrow || !ClientView.IsInside(FVector2D(From.X, From.Z)))
	{
		return;
	}
	UPenguinAbilityComponent* Abilities = Client->GetAbilities();
	if (!Abilities)
	{
		return;
	}
	if (Abilities->GetAbility(0) != EPenguinAbility::SnowWarp)
	{
		TArray<EPenguinAbility> Loadout = Abilities->GetAbilities();
		if (Loadout.Num() == 0)
		{
			Loadout.Add(EPenguinAbility::None);
		}
		Loadout[0] = EPenguinAbility::SnowWarp;
		Abilities->SetAbilities(Loadout);
	}
	const bool bUsed = Abilities->UseAbility(0, From + FVector(WarpAimFar, 0.f, WarpAimUp));
	++Island.WarpTries;
	if (bUsed || Island.WarpTries >= WarpMaxTries)
	{
		Island.bWarpFired = true;
		Island.WarpFiredAt = WorldTime;
		Report(FString::Printf(TEXT("client's Snow Warp fired: %s (its screen x %.0f..%.0f z %.0f..%.0f, from %.0f, %.0f)"), bUsed ? TEXT("warped") : TEXT("refused"),
			ClientView.Min.X, ClientView.Max.X, ClientView.Min.Y, ClientView.Max.Y, From.X, From.Z));
	}
}

bool APinguNetTest::IsLobbyWarpSettled() const
{
	using namespace PinguNetTestIslandPrivate;
	if (IsLateTest())
	{
		return true;
	}
	const FIslandProgress& Island = IslandProgress();
	// A second after the fire is enough for the portal to reach the client, which judges its warp the moment it shows there.
	return Island.bWarpFired && WorldTime - Island.WarpFiredAt >= 1.f;
}

void APinguNetTest::AddIslandChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestIslandPrivate;
	const FIslandProgress& Island = IslandProgress();
	const bool bLate = IsLateTest();
	if (!bHostRole)
	{
		Check(TEXT("the island is the host's, made for each fight's penguins"), Island.Fights > 0 && Island.FightsSized == Island.Fights,
			FString::Printf(TEXT("%d of %d fights: %s"), Island.FightsSized, Island.Fights, *Island.SizeDetail));
		if (!bLate)
		{
			Check(TEXT("the camera follows our own penguin"), Island.Fights > 0 && Island.FightsFollowing == Island.Fights,
				FString::Printf(TEXT("%d of %d fights: %s"), Island.FightsFollowing, Island.Fights, *Island.FollowDetail));
			Check(TEXT("our Snow Warp ends on our own screen"), Island.bWarpJudged && Island.bWarpOk,
				Island.WarpDetail.IsEmpty() ? FString(TEXT("no warp of ours seen in the lobby")) : Island.WarpDetail);
		}
	}
	else if (bLate)
	{
		Check(TEXT("the island grows for the late player (2 penguins, then 3)"), Island.FirstSize == 2 && Island.LastSize == 3,
			FString::Printf(TEXT("%s"), *Island.SizeDetail));
	}
}

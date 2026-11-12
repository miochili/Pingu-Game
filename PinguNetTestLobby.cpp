// Pingu - the LAN test's lobby island checks (APinguNetTest::UpdateLobbyIslandWatch, AddLobbyIslandChecks), -PinguNetTest without
// -PinguNetLate / -PinguNetTraits: in the lobby both machines have the host's small lobby island; the host sees the client's
// penguin appear on it with a puff (it pops in from small, the magic dust counted there); the client sees the host's penguin; on
// both machines every person's name tag says that person's name (the host's, the client's).

#include "PinguNetTest.h"
#include "Penguin.h"
#include "BlockIsland.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PinguLobby.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace PinguNetTestLobbyPrivate
{
	struct FLobbyProgress
	{
		bool bLobbySeen = false;
		bool bIslandOk = false;
		FString IslandDetail;
		// host: the client's penguin arriving
		bool bOtherSeen = false;
		float OtherMinPop = 2.f;
		/** How long ago the other's penguin had puffed in when it was first seen here (s). */
		float OtherPuffAge = -1.f;
		int32 PuffInsBefore = -1;
		int32 PuffInsAfter = 0;
		// both: the tags
		bool bTagsOk = false;
		bool bTagsJudged = false;
		FString TagDetail;
		int32 TagFrames = 0;
		// the spots and the gags (2026-10-27): host - the client's penguin sits on a spot of its own, the host starts a gag on it;
		// client - both penguins on their spots (posed, apart), the host's gag on our own penguin seen here with its horn
		bool bSpotJudged = false;
		float SpotJudgedAt = -1.f;
		bool bSpotOk = false;
		FString SpotDetail;
		bool bGagSent = false;
		float GagSentAt = -1.f;
		bool bGagSeen = false;
		float GagSeenAt = -1.f;
		float GagHonk = 0.f;
		FString GagDetail;
		// the card lobby (the default since 2026-10-29): both machines' cards - one per person, the host's marked HOST, ours YOU,
		// every name the player state's
		bool bCardsJudged = false;
		bool bCardsOk = false;
		int32 CardFrames = 0;
		FString CardDetail;
	};

	FLobbyProgress& LobbyProgress()
	{
		static FLobbyProgress Instance;
		return Instance;
	}

	/** The lobby island checks (-PinguIslandLobby); the card lobby has its own checks below. */
	bool IsLobbyRun()
	{
		return !PinguUseOldMenus() && PinguMenu3D::UseIslandLobby() && !FParse::Param(FCommandLine::Get(), TEXT("PinguNetLate")) && !FParse::Param(FCommandLine::Get(), TEXT("PinguNetTraits"));
	}

	bool IsCardLobbyRun()
	{
		return !PinguUseOldMenus() && !PinguMenu3D::UseIslandLobby() && !FParse::Param(FCommandLine::Get(), TEXT("PinguNetLate")) && !FParse::Param(FCommandLine::Get(), TEXT("PinguNetTraits"));
	}

	/** What the name tag should say: the person's name (its player state's), and what it says here. */
	bool TagRight(const APinguLobby* Lobby, const APenguinCharacter* Penguin, FString& OutLine)
	{
		const APlayerState* Player = Penguin ? Penguin->GetPlayerState() : nullptr;
		const FString Want = Player ? Player->GetPlayerName() : FString();
		const FString Shown = Lobby && Penguin ? Lobby->GetNameTagText(Penguin) : FString();
		OutLine = FString::Printf(TEXT("\"%s\" (want \"%s\")"), *Shown, *Want);
		return !Want.IsEmpty() && Shown == Want;
	}
}

void APinguNetTest::UpdateLobbyIslandWatch()
{
	using namespace PinguNetTestLobbyPrivate;
	if (IsCardLobbyRun())
	{
		// The card lobby: once both people are in the lobby, their cards (APenguinHUD::GetLobbyCards) say the right names - the host's
		// card HOST, ours YOU - judged over a few frames (a card is made a moment after the person joins).
		FLobbyProgress& C = LobbyProgress();
		const APenguinGameState* State = APenguinGameState::Get(this);
		const APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		const APenguinHUD* HUD = Controller ? Cast<APenguinHUD>(Controller->GetHUD()) : nullptr;
		APenguinCharacter* Own = GetOwnPenguin();
		APenguinCharacter* Other = GetOtherPlayerPenguin();
		if (C.bCardsJudged || !State || State->GetPhase() != EPinguPhase::Lobby || !HUD || !Own || !Other)
		{
			return;
		}
		const APlayerState* OwnState = Own->GetPlayerState();
		const APlayerState* OtherState = Other->GetPlayerState();
		if (!OwnState || !OtherState || OwnState->GetPlayerName().IsEmpty() || OtherState->GetPlayerName().IsEmpty())
		{
			return;
		}
		const TArray<FPinguLobbyPlayer> Cards = HUD->GetLobbyCards();
		int32 Humans = 0;
		bool bOwnCard = false;
		bool bOtherCard = false;
		for (const FPinguLobbyPlayer& Card : Cards)
		{
			if (Card.bBot)
			{
				continue;
			}
			++Humans;
			bOwnCard |= Card.bYou && Card.Name == OwnState->GetPlayerName();
			bOtherCard |= !Card.bYou && Card.Name == OtherState->GetPlayerName();
		}
		// every human card must have the host flag right: ours is the host's exactly when we are the host
		bool bHostFlagsRight = true;
		for (const FPinguLobbyPlayer& Card : Cards)
		{
			if (!Card.bBot)
			{
				bHostFlagsRight &= Card.bHost == (Card.bYou ? bHostRole : !bHostRole);
			}
		}
		++C.CardFrames;
		C.CardDetail = FString::Printf(TEXT("%d people cards, ours \"%s\" %d, the other's \"%s\" %d, the HOST mark on the right card %d"), Humans,
			*OwnState->GetPlayerName(), bOwnCard ? 1 : 0, *OtherState->GetPlayerName(), bOtherCard ? 1 : 0, bHostFlagsRight ? 1 : 0);
		if ((Humans == 2 && bOwnCard && bOtherCard && bHostFlagsRight) || C.CardFrames > 240)
		{
			C.bCardsJudged = true;
			C.bCardsOk = Humans == 2 && bOwnCard && bOtherCard && bHostFlagsRight;
			Report(TEXT("lobby cards: ") + C.CardDetail);
		}
		return;
	}
	if (!IsLobbyRun())
	{
		return;
	}
	FLobbyProgress& L = LobbyProgress();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	const APinguLobby* Lobby = APinguLobby::Get(GetWorld());
	const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!GameState || GameState->GetPhase() != EPinguPhase::Lobby || !Lobby || !Island)
	{
		return;
	}
	if (!L.bLobbySeen && Lobby->IsShowing())
	{
		L.bLobbySeen = true;
		L.PuffInsBefore = Lobby->GetPuffInsSeen();
	}
	if (!L.bLobbySeen)
	{
		return;
	}
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	// The lobby island here is the host's (a client's blocks: its checksum matches).
	const FBox Bounds = Island->GetBounds();
	const float Width = Bounds.IsValid ? static_cast<float>(Bounds.Max.X - Bounds.Min.X) / Island->CellSize : 0.f;
	L.bIslandOk = Island->Layout == EIslandLayout::Lobby && Island->MatchesHostIsland() && Width > 11.5f && Width < 14.5f;
	L.IslandDetail = FString::Printf(TEXT("layout lobby %d, the host's blocks %d, %.1f blocks wide"), Island->Layout == EIslandLayout::Lobby ? 1 : 0,
		Island->MatchesHostIsland() ? 1 : 0, Width);
	if (Other)
	{
		if (!L.bOtherSeen)
		{
			L.bOtherSeen = true;
			Report(FString::Printf(TEXT("lobby: the other player's penguin %s is here at %.0f, %.0f"), *Other->GetDisplayName(), Other->GetActorLocation().X,
				Other->GetActorLocation().Z));
		}
		L.OtherMinPop = FMath::Min(L.OtherMinPop, Lobby->GetPopScale(Other));
		if (L.OtherPuffAge < 0.f)
		{
			L.OtherPuffAge = Lobby->GetPuffInAge(Other); // (-1 until the lobby has its entry)
		}
		L.PuffInsAfter = Lobby->GetPuffInsSeen();
	}
	// The spots: once both are in, each penguin on a spot of its own (host: the server's spot; both: posed, apart).
	// (The client judges once its own penguin has been in for 3 s: a new player's first reset can come before it owns its
	// penguin - the server then puts it on its spot again a moment later.)
	if (Own && Other && !L.bSpotJudged && Lobby->GetPopScale(Other) > 0.99f && Lobby->GetPuffInAge(Other) > 0.8f
		&& (bHostRole || Lobby->GetPuffInAge(Own) > 3.f))
	{
		L.bSpotJudged = true;
		L.SpotJudgedAt = static_cast<float>(GetWorld()->GetTimeSeconds());
		const FVector OwnBody = Lobby->GetBodyCentre(Own);
		const FVector OtherBody = Lobby->GetBodyCentre(Other);
		const float Apart = static_cast<float>(FVector::Dist(OwnBody, OtherBody));
		// (Both in the lobby's entries; since 2026-10-27 everybody stands - pose 0.)
		const bool bPosed = Lobby->GetPuffInAge(Own) >= 0.f && Lobby->GetPuffInAge(Other) >= 0.f;
		bool bOnSpot = true;
		if (bHostRole)
		{
			const int32 Spot = Lobby->GetSpotOf(Other);
			bOnSpot = Lobby->GetSpots().IsValidIndex(Spot) && Spot != Lobby->GetSpotOf(Own)
				&& FVector::Dist(Other->GetActorLocation(), Lobby->GetSpots()[Spot].Location) < 25.f;
		}
		L.bSpotOk = bPosed && bOnSpot && Apart >= 100.f;
		L.SpotDetail = FString::Printf(TEXT("poses %d / %d (ours / the other's; 0 stand), %.0f cm apart (want 100 or more), on its spot %d"),
			static_cast<int32>(Lobby->GetPose(Own)), static_cast<int32>(Lobby->GetPose(Other)), Apart, bOnSpot ? 1 : 0);
		Report(TEXT("lobby spots: ") + L.SpotDetail);
	}
	// Host: a noot at the camera on the client's penguin (the server picks the gags), a moment later (the client's own lobby
	// view may come up a second or two after the host's); the client watches for it.
	if (bHostRole && Other && L.bSpotJudged && !L.bGagSent && GetWorld()->GetTimeSeconds() - L.SpotJudgedAt > 2.f)
	{
		if (APinguLobby* HostLobby = APinguLobby::Get(GetWorld()))
		{
			L.bGagSent = HostLobby->StartGag(Other, PinguLobby::EGag::NootCamera);
			L.GagSentAt = static_cast<float>(GetWorld()->GetTimeSeconds());
			Report(FString::Printf(TEXT("lobby gag: a noot at the camera on %s started %d"), *Other->GetDisplayName(), L.bGagSent ? 1 : 0));
		}
	}
	if (!bHostRole && Own)
	{
		float Age = -1.f;
		const float WorldNow = static_cast<float>(GetWorld()->GetTimeSeconds());
		if (Lobby->GetGag(Own, Age) == PinguLobby::EGag::NootCamera && !L.bGagSeen)
		{
			L.bGagSeen = true;
			L.GagSeenAt = WorldNow;
			Report(FString::Printf(TEXT("lobby gag: the host's noot at the camera on our penguin seen here, %.2f s into it"), Age));
		}
		// Its horn (the noot's own, replicated as an action) - a while longer: a busy host fires its blasts late.
		if (L.bGagSeen && WorldNow - L.GagSeenAt < 5.f)
		{
			L.GagHonk = FMath::Max(L.GagHonk, Own->GetHonkWeight());
		}
		L.GagDetail = FString::Printf(TEXT("gag seen %d, the horn out %.2f (a measurement: a busy host may start the match before its blasts), %d gags seen here"), L.bGagSeen ? 1 : 0, L.GagHonk, Lobby->GetGagsSeen());
	}
	// Both names on both machines, judged over a few frames once both are in (a tag needs a frame to be built).
	if (Own && Other && !L.bTagsJudged && Lobby->GetPopScale(Other) > 0.99f)
	{
		FString OwnLine;
		FString OtherLine;
		const bool bOwn = TagRight(Lobby, Own, OwnLine);
		const bool bOther = TagRight(Lobby, Other, OtherLine);
		++L.TagFrames;
		L.TagDetail = FString::Printf(TEXT("ours %s, the other's %s"), *OwnLine, *OtherLine);
		if ((bOwn && bOther) || L.TagFrames > 120)
		{
			L.bTagsJudged = true;
			L.bTagsOk = bOwn && bOther;
			Report(TEXT("lobby tags: ") + L.TagDetail);
		}
	}
}

void APinguNetTest::AddLobbyIslandChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestLobbyPrivate;
	if (IsCardLobbyRun())
	{
		const FLobbyProgress& C = LobbyProgress();
		Check(TEXT("lobby cards: both people on a card with their names, the host marked"), C.bCardsOk, C.bCardsJudged ? C.CardDetail : FString(TEXT("never both in the lobby")));
		return;
	}
	if (!IsLobbyRun())
	{
		return;
	}
	const FLobbyProgress& L = LobbyProgress();
	if (bHostRole)
	{
		const int32 Puffs = L.PuffInsBefore >= 0 ? L.PuffInsAfter - L.PuffInsBefore : 0;
		// (Its pop lasts 0.42 s; the test may find the client's penguin only once it has its player state: the puff's age then.)
		const bool bPuffed = L.OtherMinPop < 0.5f || (L.OtherPuffAge >= 0.f && L.OtherPuffAge < 3.f);
		Check(TEXT("lobby island: the client's penguin appears with a puff"), L.bOtherSeen && bPuffed && Puffs >= 1,
			L.bOtherSeen ? FString::Printf(TEXT("seen %.2f s after its puff, smallest seen %.2f, %d puffs in here; %s"), L.OtherPuffAge, L.OtherMinPop, Puffs, *L.IslandDetail)
				: FString(TEXT("never seen in the lobby")));
		Check(TEXT("lobby island: the client's penguin sits on a spot of its own, a gag started on it"), L.bSpotOk && L.bGagSent,
			L.bSpotJudged ? FString::Printf(TEXT("%s; gag started %d"), *L.SpotDetail, L.bGagSent ? 1 : 0) : FString(TEXT("never both in the lobby")));
	}
	else
	{
		Check(TEXT("lobby island: the host's island, the host's penguin on it"), L.bIslandOk && L.bOtherSeen,
			FString::Printf(TEXT("%s, the host's penguin seen %d"), *L.IslandDetail, L.bOtherSeen ? 1 : 0));
		Check(TEXT("lobby island: both penguins on their spots, the host's gag seen here"), L.bSpotOk && L.bGagSeen,
			L.bSpotJudged ? FString::Printf(TEXT("%s; %s"), *L.SpotDetail, *L.GagDetail) : FString(TEXT("never both in the lobby")));
	}
	Check(TEXT("lobby island: the names on the tags"), L.bTagsOk, L.bTagsJudged ? L.TagDetail : FString(TEXT("never both in the lobby")));
}

// Pingu - the LAN test in Traits mode (-PinguNetTest=host / client -PinguNetTraits; pipeline steps nettraits and
// nettraitslate): APinguNetTest's traits part (see PinguNetTest.h).
//
// Host: turns TRAITS on in the lobby (the HUD's toggle, as a player does), starts the match with it and forces the traits
//       by slot - round 1 host Giant, client Second Chance, bot Glass Cannon; round 2 host Heavyweight, client
//       Featherweight, bot Gambler (-PinguNetLate: host Giant, everybody else Glass Cannon, every round). Round 1 it throws
//       the client's penguin into the sea twice (a fish throws it back the first time, the second is a knock-out) while
//       everybody holds still; round 2 everybody holds still while the client's penguin does its triple jump, and the host
//       makes the bot's Gambler fizzle (its ability turns into sparkling dust on both machines, its overhead icon crumbles).
// Both: log the traits each round in the same words ("round N traits: ...", diffed in nettest_compare.txt), judge what they
//       see: the reveal's cards on the client, the looks (the host's Giant at 1.68, the bot's glass), the client's two
//       abilities, the rescue; the late client the looks it finds when it joins (quietly: no reveal for them) and its own
//       trait and reveal in its first round.
// What it knows across the map loads (the client logs its RESULT lines back in a solo world) lives in FNetTraits.

#include "PinguNetTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinBot.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "PinguFX.h"
#include "PinguTraitTable.h"
#include "PinguUI.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguNetTestTraitsPrivate
{
	// Round 1: the client's Second Chance (fight phase times; WorldTime for what follows a moment seen here).
	constexpr float DropAt = 1.f;              // host: the client's penguin over the sea, falling
	constexpr float DropOut = 700.f;           // cm past the island's end ...
	constexpr float DropFarOut = 2500.f;       // ... and the second time, too far to come back
	constexpr float DropAboveSea = 400.f;      // cm above the sea level (the knock-out line is 120 above it)
	constexpr float AliveAfter = 1.5f;         // s after the rescue it must still be in
	constexpr float SecondDropAfter = 2.2f;    // s after the rescue: the second fall
	constexpr float SecondDropLatest = 7.f;    // no rescue seen by then: the second fall anyway
	constexpr float HoldOneTo = 11.f;          // round 1: everybody holds still at most this long
	constexpr float HoldAfterKO = 0.5f;        // ... or this long after the second fall's knock-out
	// Round 2: the client's triple jump (fight phase times).
	constexpr float HoldTwoTo = 2.8f;
	constexpr float JumpBaseAt = 0.3f;         // the jump counters are taken here
	constexpr float JumpPressAt[4] = { 0.45f, 0.8f, 1.15f, 1.5f }; // the ground jump and three presses in the air
	constexpr float JumpHold = 0.15f;
	constexpr float JumpJudgeAt = 2.6f;
	constexpr float FizzleAt = 1.f;            // host: the bot's Gambler fizzles (during the hold; its roll is forced)
	// The jump needs open sky: a round's spawn is "the lowest surface with two free cells above", so it can sit under a
	// floating platform, where a Featherweight's jump hits the ceiling after 0.07 s and every later press lands on the ground.
	constexpr float JumpRoomWanted = 600.f;    // cm of free room over the head (at least) the three presses need (a full jump rises about 290)
	// The looks.
	constexpr float LookAt = 0.5f;             // fight phase time: the host's size is sampled
	constexpr float JoinWatch = 2.f;           // late client: s from its first fight frame it watches the others' looks
	constexpr float GiantSize = 1.682f;

	struct FNetTraits
	{
		// the lobby
		bool bToggleSent = false;
		bool bToggleChecked = false;
		bool bToggledOn = false;
		bool bLobbyOnSeen = false;
		bool bMatchTraitsOn = false;
		// every round (index 0, 1: rounds 1 and 2)
		int32 LastFightRound = -1;
		float IntroLongest[2] = { 0.f, 0.f };
		bool bRoundSeen[2] = { false, false };
		bool bRoundOk[2] = { false, false };
		FString RoundLine[2];
		// round 1's reveal on this machine
		int32 MostCards = 0;
		int32 MostFlying = 0;
		// round 1's looks
		bool bLooksSampled = false;
		float HostSize = 0.f;
		bool bGlassOverlay = false;
		int32 ClientAbilities = -1;
		bool bClientSlotOneEmpty = false;
		FString ClientLoadout;
		// round 1: Second Chance (and everybody's hold)
		bool bHoldOneOver = false;
		bool bFirstDrop = false;
		float RescuedAt = -1.f;
		float FastestUp = 0.f;
		float HighestFeet = -1.e9f;
		bool bAliveSampled = false;
		bool bAliveAfter = false;
		bool bSecondDrop = false;
		float KOAt = -1.f;
		uint8 KOReason = 0;
		bool bKOBeforeRescue = false;
		int32 Rescues = -1;
		/** The fish's throw passes through the island's blocks (the flag on this machine: the client's own movement, the host's
		 *  copy of it), over the island's highest block (its top, when the throw was seen). */
		bool bPassSeen = false;
		float IslandTop = -1.e9f;
		// round 2: the triple jump
		bool bHoldTwoOver = false;
		bool bJumpBase = false;
		int32 JumpsAtBase = 0;
		int32 AirJumpsAtBase = 0;
		int32 JumpStage = 0;
		bool bJumpJudged = false;
		bool bRoomChecked = false;
		float RoomAtStart = -1.f;     // cm over the head when the countdown began (before any move)
		// round 2: the bot's Gambler fizzles (the host forces it); what each machine saw of it
		bool bFizzleFired = false;
		int32 FizzleDustEvents = 0;   // APinguFX fizzle dust played here
		int32 IconStartsBase = -1;    // the overhead widget's dust starts before round 2
		int32 IconDust = 0;           // icons that began to crumble here since
		bool bRoofBuilt = false;      // host, PINGU_NETTRAITS_ROOF: the low roof over the client's penguin
		bool bMovedForRoom = false;
		float RoomAtJump = -1.f;      // cm over the head when the jump counters were taken
		int32 GroundJumps = -1;
		int32 AirJumps = -1;
		int32 MaxAirJumps = -1;
		// -PinguNetLate
		float JoinFightAt = -1.f;
		bool bJoinJudged = false;
		float JoinHostSize = 0.f;
		bool bJoinOverlay = false;
		int32 JoinCards = 0;
		bool bJoinTransformed = false;
		int32 OwnFirstRound = -1;
		int32 OwnRoundCards = 0;
		bool bOwnRoundJudged = false;
		EPinguTrait OwnFirstTrait = EPinguTrait::None;
		int32 ClientFirstRound = -1;
		EPinguTrait ClientFirstTrait = EPinguTrait::None;
		// round 3 (2026-11-07, shared-traits): no forcing - the host's list (two ticked) deals ONE trait for everybody
		bool bListSet = false;
		bool bRoundThreeSeen = false;
		EPinguTrait RoundThreeTrait = EPinguTrait::None;
		bool bRoundThreeShared = false;
		FString RoundThreeLine;
		bool bBannerThree = false;
		FString BannerThreeName;
		/** The top-left corner icon (2026-11-07 hud-cleanup): frames of round 1's fight with this machine's own trait changed, and
		 *  the ones where the corner showed exactly that trait. */
		int32 CornerFightFrames = 0;
		int32 CornerShownFrames = 0;
	};

	/** Round 3's list: only these two ticked (both safe for the free fight the joiner judges the host's motion in). */
	constexpr EPinguTrait RoundThreeTicked[] = { EPinguTrait::LowGravity, EPinguTrait::Turbo };

	FNetTraits& State()
	{
		static FNetTraits Instance;
		return Instance;
	}

	bool IsLateRun()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("PinguNetLate"));
	}

	EPinguTrait TraitOf(const APenguinCharacter* Penguin)
	{
		const UPenguinTraitComponent* Traits = Penguin ? Penguin->GetTraits() : nullptr;
		return Traits ? Traits->GetTrait() : EPinguTrait::None;
	}

	bool HasOverlay(const APenguinCharacter* Penguin)
	{
		const USkeletalMeshComponent* SkelMesh = Penguin ? Penguin->GetMesh() : nullptr;
		return SkelMesh && SkelMesh->GetOverlayMaterial() != nullptr;
	}

	/** The traits the host forces (round: 1 or 2; the late run's every round). */
	void Expected(int32 Round, EPinguTrait& OutHost, EPinguTrait& OutClient, EPinguTrait& OutBot)
	{
		if (IsLateRun())
		{
			OutHost = EPinguTrait::Giant;
			OutClient = EPinguTrait::GlassCannon;
			OutBot = EPinguTrait::GlassCannon;
		}
		else if (Round <= 1)
		{
			OutHost = EPinguTrait::Giant;
			OutClient = EPinguTrait::SecondChance;
			OutBot = EPinguTrait::GlassCannon;
		}
		else
		{
			OutHost = EPinguTrait::Heavyweight;
			OutClient = EPinguTrait::Featherweight;
			OutBot = EPinguTrait::Gambler;
		}
	}

	/** Host: these traits by slot for the next deal (the two players' slots, Others for every other slot). */
	void ForceBySlot(APenguinGameMode* GameMode, int32 HostSlot, int32 ClientSlot, EPinguTrait Host, EPinguTrait Client, EPinguTrait Others)
	{
		if (!GameMode)
		{
			return;
		}
		TArray<EPinguTrait> BySlot;
		BySlot.Init(Others, PinguMaxPenguins);
		if (BySlot.IsValidIndex(HostSlot))
		{
			BySlot[HostSlot] = Host;
		}
		if (BySlot.IsValidIndex(ClientSlot))
		{
			BySlot[ClientSlot] = Client;
		}
		GameMode->SetForcedTraits(BySlot);
	}

	int32 CountReal(const UPenguinAbilityComponent* Abilities)
	{
		int32 Count = 0;
		for (int32 Slot = 0; Abilities && Slot < Abilities->GetAbilities().Num(); ++Slot)
		{
			Count += Abilities->GetAbility(Slot) != EPenguinAbility::None ? 1 : 0;
		}
		return Count;
	}

	/** Cm of free room above a capsule's top (over its middle and both sides), up to Max. */
	float RoomAbove(const ABlockIsland* Island, const FVector& Center, float CapsuleRadius, float CapsuleHalfHeight, float Max)
	{
		const float Step = Island->CellSize * 0.25f;
		for (float Up = 0.f; Up < Max; Up += Step)
		{
			for (const float Side : { -CapsuleRadius * 0.8f, 0.f, CapsuleRadius * 0.8f })
			{
				if (Island->IsSolidAt(FVector(Center.X + Side, 0.f, Center.Z + CapsuleHalfHeight + Up + 1.f)))
				{
					return Up;
				}
			}
		}
		return Max;
	}

	/**
	 * The standing spot (capsule centre) of the open-sky column nearest to NearX: a column whose highest block has nothing
	 * above it, preferably with the same height on both sides (a flat top, not a ledge), with room for the capsule and
	 * JumpRoomWanted over its head. False when the island has none.
	 */
	bool FindOpenSkySpot(const ABlockIsland* Island, float NearX, float CapsuleRadius, float CapsuleHalfHeight, FVector& OutSpot)
	{
		const FBox Bounds = Island->GetBounds();
		if (!Bounds.IsValid)
		{
			return false;
		}
		const FIntPoint Low = Island->WorldToCell(Bounds.Min);
		const FIntPoint High = Island->WorldToCell(Bounds.Max);
		const int32 NoRow = TNumericLimits<int32>::Min();
		const auto TopRow = [Island, Low, High, NoRow](int32 Column) -> int32
		{
			for (int32 Row = High.Y; Row >= Low.Y; --Row)
			{
				if (Island->IsSolidCell(FIntPoint(Column, Row)))
				{
					return Row;
				}
			}
			return NoRow;
		};
		float BestScore = TNumericLimits<float>::Max();
		bool bFound = false;
		for (int32 Column = Low.X + 1; Column < High.X; ++Column)
		{
			const int32 Top = TopRow(Column);
			const int32 LeftTop = TopRow(Column - 1);
			const int32 RightTop = TopRow(Column + 1);
			if (Top == NoRow || LeftTop < Top || RightTop < Top)
			{
				continue;
			}
			const FVector Floor = Island->CellToWorld(FIntPoint(Column, Top));
			const FVector Spot(Floor.X, 0.f, Floor.Z + Island->CellSize * 0.5f + CapsuleHalfHeight + 2.f);
			if (!Island->CanFitCapsule(Spot, CapsuleRadius, CapsuleHalfHeight)
				|| RoomAbove(Island, Spot, CapsuleRadius, CapsuleHalfHeight, JumpRoomWanted) < JumpRoomWanted)
			{
				continue;
			}
			const bool bFlat = LeftTop == Top && RightTop == Top;
			const float Score = FMath::Abs(static_cast<float>(Spot.X) - NearX) + (bFlat ? 0.f : 100000.f);
			if (Score < BestScore)
			{
				BestScore = Score;
				OutSpot = Spot;
				bFound = true;
			}
		}
		return bFound;
	}

	/**
	 * The client, round 2's countdown: makes sure its penguin has open sky over its head for the triple jump. A spawn under a
	 * floating platform is moved to the nearest column top with open sky (a client's own moves carry absolute positions).
	 * Returns the line to log (empty: the island is not there yet, asked again next frame).
	 */
	FString MakeRoomForJump(APenguinCharacter* Penguin, FNetTraits& N, bool bMove)
	{
		const UWorld* PenguinWorld = Penguin ? Penguin->GetWorld() : nullptr;
		const ABlockIsland* Island = PenguinWorld ? ABlockIsland::Get(PenguinWorld) : nullptr;
		const UCapsuleComponent* Capsule = Penguin ? Penguin->GetCapsuleComponent() : nullptr;
		if (!Island || !Capsule || !Island->MatchesHostIsland())
		{
			return FString();
		}
		N.bRoomChecked = true;
		const float CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		const float CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		const FVector From = Penguin->GetActorLocation();
		N.RoomAtStart = RoomAbove(Island, From, CapsuleRadius, CapsuleHalfHeight, JumpRoomWanted);
		if (N.RoomAtStart >= JumpRoomWanted)
		{
			return FString::Printf(TEXT("traits: round 2 jump room: %.0f cm of free room over the own head at x %.0f, z %.0f (want %.0f): stays"), N.RoomAtStart,
				From.X, From.Z, JumpRoomWanted);
		}
		FVector Spot = From;
		if (FindOpenSkySpot(Island, static_cast<float>(From.X), CapsuleRadius, CapsuleHalfHeight, Spot))
		{
			if (!bMove)
			{
				// (The host moves it: a joiner that moved its own penguin would be refused by the host - PinguHostAuthority.h.)
				N.bMovedForRoom = true;
				return FString::Printf(TEXT("traits: round 2 jump room: only %.0f cm over the own head at x %.0f, z %.0f (want %.0f): the host moves it to the open sky at x %.0f, z %.0f"),
					N.RoomAtStart, From.X, From.Z, JumpRoomWanted, Spot.X, Spot.Z);
			}
			Penguin->NetTeleport(Spot, FVector::ZeroVector, false);
			N.bMovedForRoom = true;
			return FString::Printf(TEXT("traits: round 2 jump room: only %.0f cm over the own head at x %.0f, z %.0f (want %.0f): moved to the open sky at x %.0f, z %.0f"),
				N.RoomAtStart, From.X, From.Z, JumpRoomWanted, Spot.X, Spot.Z);
		}
		return FString::Printf(TEXT("traits: round 2 jump room: only %.0f cm over the own head at x %.0f, z %.0f (want %.0f) and no open-sky column to move to"),
			N.RoomAtStart, From.X, From.Z, JumpRoomWanted);
	}
}

bool APinguNetTest::IsTraitsRun()
{
	return FParse::Param(FCommandLine::Get(), TEXT("PinguNetTraits"));
}

void APinguNetTest::PrepareTraitsMatch(APenguinGameMode* GameMode, FPinguMatchSettings& Settings)
{
	using namespace PinguNetTestTraitsPrivate;
	if (!IsTraitsRun() || !GameMode)
	{
		return;
	}
	FNetTraits& N = State();
	// Traits mode as the lobby's toggle left it (StartMatch takes these settings).
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	Settings.bTraits = GameState && GameState->AreTraitsOn();
	Settings.TraitsOff = GameState ? GameState->GetSettings().TraitsOff : 0; // (the host's TRAITS list, as the lobby's START keeps it)
	N.bMatchTraitsOn = Settings.bTraits;
	const APenguinCharacter* Own = GetOwnPenguin();
	const APenguinCharacter* Other = GetOtherPlayerPenguin();
	EPinguTrait Host = EPinguTrait::None;
	EPinguTrait Client = EPinguTrait::None;
	EPinguTrait Bot = EPinguTrait::None;
	Expected(1, Host, Client, Bot);
	ForceBySlot(GameMode, Own ? Own->GetPlayerSlot() : 0, Other ? Other->GetPlayerSlot() : INDEX_NONE, Host, Client, Bot);
	Report(FString::Printf(TEXT("traits: the match with TRAITS %s, forced: host %s, client %s, the rest %s"), Settings.bTraits ? TEXT("on") : TEXT("OFF"),
		*PinguTraits::GetLogName(Host), *PinguTraits::GetLogName(Client), *PinguTraits::GetLogName(Bot)));
}

bool APinguNetTest::UpdateTraitsCheck()
{
	using namespace PinguNetTestTraitsPrivate;
	UWorld* World = GetWorld();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!IsTraitsRun() || !World || !GameState || GetNetMode() == NM_Standalone)
	{
		return false;
	}
	FNetTraits& N = State();
	const bool bLate = IsLateRun();
	const EPinguPhase Phase = GameState->GetPhase();
	const int32 Round = GameState->GetRoundNumber();
	const float T = GameState->GetPhaseTime();
	APenguinCharacter* Own = GetOwnPenguin();
	APenguinCharacter* Other = GetOtherPlayerPenguin();
	APenguinCharacter* HostPenguin = bHostRole ? Own : Other;
	APenguinCharacter* ClientPenguin = bHostRole ? Other : Own;
	TArray<APenguinCharacter*> Bots;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		const APlayerState* PlayerState = It->GetPlayerState();
		if (*It != HostPenguin && *It != ClientPenguin && (!PlayerState || PlayerState->IsABot()))
		{
			Bots.Add(*It);
		}
	}

	// ---- The lobby: the host turns TRAITS on (its HUD's toggle, as a player does); the client sees it.
	if (Phase == EPinguPhase::Lobby)
	{
		const APlayerController* LocalController = World->GetFirstPlayerController();
		APenguinHUD* LocalHUD = LocalController ? Cast<APenguinHUD>(LocalController->GetHUD()) : nullptr;
		if (bHostRole && !N.bToggleSent && WorldTime > 1.f && LocalHUD)
		{
			N.bToggleSent = true;
			LocalHUD->Action_ToggleTraits();
			Report(TEXT("traits: TRAITS toggled in the lobby"));
		}
		if (bHostRole && N.bToggleSent && !N.bToggleChecked && WorldTime > 1.5f)
		{
			N.bToggleChecked = true;
			N.bToggledOn = GameState->AreTraitsOn();
			Report(FString::Printf(TEXT("traits: TRAITS %s after the toggle"), N.bToggledOn ? TEXT("on") : TEXT("STILL OFF")));
		}
		// The host's TRAITS list: only two ticked (round 3 deals one of them to everybody).
		if (bHostRole && !bLate && N.bToggleChecked && !N.bListSet && WorldTime > 1.7f)
		{
			N.bListSet = true;
			uint64 Off = ~uint64(0);
			for (const EPinguTrait Trait : RoundThreeTicked)
			{
				Off &= ~(uint64(1) << static_cast<int32>(Trait));
			}
			if (APenguinGameMode* ListMode = World->GetAuthGameMode<APenguinGameMode>())
			{
				ListMode->SetTraitsOff(Off);
			}
			Report(TEXT("traits: the host's list: Low Gravity and Turbo ticked"));
		}
		if (!bHostRole && !N.bLobbyOnSeen && GameState->AreTraitsOn())
		{
			N.bLobbyOnSeen = true;
			Report(TEXT("traits: TRAITS ON seen in the lobby"));
		}
		return false;
	}
	if (Phase != EPinguPhase::Intro && Phase != EPinguPhase::Countdown && Phase != EPinguPhase::Fight && Phase != EPinguPhase::RoundOver
		&& Phase != EPinguPhase::MatchOver)
	{
		return false;
	}
	N.bMatchTraitsOn |= GameState->AreTraitsOn();

	// ---- Round 2's bot is a Gambler that fizzles once the host forces it: the dust of its ability (an APinguFX event the host
	// broadcasts) and the crumbling of its overhead icon (read from the replicated trait events) show on this machine too.
	if (!bLate && Round >= 2)
	{
		if (const APinguFX* FX = APinguFX::Get(this))
		{
			N.FizzleDustEvents = FMath::Max(N.FizzleDustEvents, FX->GetFizzleDustEvents());
		}
		const int32 Starts = SPinguOverheadAbilities::GetLastPaintStats().DustStarts;
		N.IconStartsBase = N.IconStartsBase < 0 ? Starts : N.IconStartsBase;
		N.IconDust = FMath::Max(N.IconDust, Starts - N.IconStartsBase);
	}

	// ---- The intro: how long it lasts, the reveal's cards here.
	if (Phase == EPinguPhase::Intro && Round >= 1 && Round <= 2)
	{
		N.IntroLongest[Round - 1] = FMath::Max(N.IntroLongest[Round - 1], T);
	}
	if (Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown)
	{
		const PinguUI::FTraitRevealStats& Stats = PinguUI::GetTraitRevealStats();
		if (Round == 1 && !bLate)
		{
			N.MostCards = FMath::Max(N.MostCards, Stats.Cards);
			N.MostFlying = FMath::Max(N.MostFlying, Stats.Flying);
		}
		// Round 3: the shared trait's one banner here.
		if (Round == 3 && !bLate && Stats.bBanner)
		{
			N.bBannerThree = true;
			N.BannerThreeName = Stats.BannerName;
		}
		// Late client: its first round's reveal (its own card among them).
		if (bLate && !bHostRole && Own && (N.OwnFirstRound < 0 || N.OwnFirstRound == Round))
		{
			N.OwnFirstRound = Round;
			N.OwnRoundCards = FMath::Max(N.OwnRoundCards, Stats.Cards);
		}
	}

	// ---- The corner icon: in round 1's fight (the forced traits) it shows this machine's own trait, on host and client alike.
	if (Phase == EPinguPhase::Fight && Round == 1 && !bLate && Own && PinguTraits::IsReal(TraitOf(Own)) && Own->GetTraits()->IsTransformed())
	{
		const PinguUI::FTraitCornerStats& Corner = PinguUI::GetTraitCornerStats();
		++N.CornerFightFrames;
		N.CornerShownFrames += Corner.bShown && Corner.Trait == TraitOf(Own) ? 1 : 0;
	}

	// ---- Every fight: the traits as this machine has them, in the same words on both (nettest_compare.txt).
	if (Phase == EPinguPhase::Fight && Round != N.LastFightRound)
	{
		N.LastFightRound = Round;
		EPinguTrait WantHost = EPinguTrait::None;
		EPinguTrait WantClient = EPinguTrait::None;
		EPinguTrait WantBot = EPinguTrait::None;
		Expected(Round, WantHost, WantClient, WantBot);
		struct FEntry
		{
			int32 Slot = 0;
			FString Text;
		};
		TArray<FEntry> Entries;
		bool bAsForced = HostPenguin != nullptr;
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			const APenguinCharacter* Penguin = *It;
			const UPenguinTraitComponent* Traits = Penguin->GetTraits();
			const EPinguTrait Trait = TraitOf(Penguin);
			FEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Slot = Penguin->GetPlayerSlot();
			Entry.Text = FString::Printf(TEXT("slot %d %s%s"), Entry.Slot, *PinguTraits::GetLogName(Trait), Traits && Traits->GetNumAbilities() < 3 ? TEXT(" (2 abilities)") : TEXT(""));
			const EPinguTrait Want = Penguin == HostPenguin ? WantHost : (Penguin == ClientPenguin ? WantClient : WantBot);
			bAsForced &= Trait == Want;
		}
		Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Slot < B.Slot; });
		FString Line;
		for (const FEntry& Entry : Entries)
		{
			Line += (Line.IsEmpty() ? TEXT("") : TEXT(", ")) + Entry.Text;
		}
		Report(FString::Printf(TEXT("round %d traits: %s"), Round, *Line));
		if (Round >= 1 && Round <= 2)
		{
			N.bRoundSeen[Round - 1] = true;
			N.bRoundOk[Round - 1] = bAsForced && Entries.Num() >= 3;
			N.RoundLine[Round - 1] = Line;
		}
		// Round 3 (no forcing): everybody the same trait, the round trait here, one of the host's ticked two.
		if (!bLate && Round == 3)
		{
			N.bRoundThreeSeen = true;
			N.RoundThreeTrait = GameState->GetRoundTrait();
			bool bSame = PinguTraits::IsReal(N.RoundThreeTrait) && Entries.Num() >= 3;
			for (TActorIterator<APenguinCharacter> It(World); It; ++It)
			{
				bSame &= TraitOf(*It) == N.RoundThreeTrait;
			}
			const bool bTicked = N.RoundThreeTrait == RoundThreeTicked[0] || N.RoundThreeTrait == RoundThreeTicked[1];
			N.bRoundThreeShared = bSame && bTicked && GameState->GetSettings().IsTraitAllowed(N.RoundThreeTrait);
			N.RoundThreeLine = Line;
		}
		// Host: round 3 is dealt from the bag (the host's list) - the forcing ends once round 2's deal is done.
		if (bHostRole && !bLate && Round == 2)
		{
			if (APenguinGameMode* FreeMode = World->GetAuthGameMode<APenguinGameMode>())
			{
				FreeMode->SetForcedTraits(TArray<EPinguTrait>());
			}
		}
		// Host: round 2's traits are forced now (round 1's deal is done).
		if (bHostRole && !bLate && Round == 1)
		{
			EPinguTrait Host = EPinguTrait::None;
			EPinguTrait Client = EPinguTrait::None;
			EPinguTrait Bot = EPinguTrait::None;
			Expected(2, Host, Client, Bot);
			ForceBySlot(World->GetAuthGameMode<APenguinGameMode>(), HostPenguin ? HostPenguin->GetPlayerSlot() : 0,
				ClientPenguin ? ClientPenguin->GetPlayerSlot() : INDEX_NONE, Host, Client, Bot);
		}
		// Late: the client's first round with a penguin - its trait (and here on the client its reveal, counted above).
		if (bLate && ClientPenguin && N.ClientFirstRound < 0)
		{
			N.ClientFirstRound = Round;
			N.ClientFirstTrait = TraitOf(ClientPenguin);
			if (!bHostRole)
			{
				N.OwnFirstTrait = N.ClientFirstTrait;
			}
		}
	}

	// ---- Late client: the others' looks as it finds them when it comes in (a fight already on: no reveal for them).
	if (bLate && !bHostRole && !N.bJoinJudged && Phase == EPinguPhase::Fight)
	{
		if (N.JoinFightAt < 0.f)
		{
			N.JoinFightAt = WorldTime;
		}
		const PinguUI::FTraitRevealStats& Stats = PinguUI::GetTraitRevealStats();
		N.JoinCards = FMath::Max(N.JoinCards, Stats.Cards);
		if (HostPenguin)
		{
			N.JoinHostSize = FMath::Max(N.JoinHostSize, HostPenguin->GetSizeScale());
			const UPenguinTraitComponent* HostTraits = HostPenguin->GetTraits();
			N.bJoinTransformed |= HostTraits && HostTraits->HasAnyTrait() && HostTraits->IsTransformed();
		}
		for (const APenguinCharacter* Bot : Bots)
		{
			N.bJoinOverlay |= TraitOf(Bot) == EPinguTrait::GlassCannon && HasOverlay(Bot);
		}
		if (WorldTime - N.JoinFightAt > JoinWatch)
		{
			N.bJoinJudged = true;
			Report(FString::Printf(TEXT("traits: on joining the host at %.2f, the bot's glass %d, the host transformed %d, cards %d"), N.JoinHostSize,
				N.bJoinOverlay ? 1 : 0, N.bJoinTransformed ? 1 : 0, N.JoinCards));
		}
	}

	// Everybody holds still: our own penguin (its brain off, the stick at rest), on the host the bots too.
	const auto Hold = [this, Own]()
	{
		if (Own)
		{
			if (UPenguinBotComponent* OwnBot = Own->GetBot())
			{
				if (OwnBot->IsBotEnabled())
				{
					OwnBot->SetBotEnabled(false);
				}
			}
			Own->ApplyVirtualMoveInput(FVector2D::ZeroVector);
		}
		if (bHostRole)
		{
			for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
			{
				UPenguinBotComponent* Brain = It->GetBot();
				if (It->IsBot() && Brain && Brain->IsBotEnabled())
				{
					Brain->SetBotEnabled(false);
					HeldBots.AddUnique(*It);
				}
			}
		}
	};

	// Test of the test (PINGU_NETTRAITS_ROOF=1 in the environment): the host builds a low roof over the client's penguin in
	// round 2's intro, so the client has to find open sky by itself (its "jump room" line then says "moved").
	if (bHostRole && !bLate && Round == 2 && Phase == EPinguPhase::Intro && T > 1.f && !N.bRoofBuilt && ClientPenguin
		&& !FPlatformMisc::GetEnvironmentVariable(TEXT("PINGU_NETTRAITS_ROOF")).IsEmpty())
	{
		ABlockIsland* RoofIsland = ABlockIsland::Get(World);
		const UCapsuleComponent* ClientCapsule = ClientPenguin->GetCapsuleComponent();
		if (RoofIsland && ClientCapsule)
		{
			N.bRoofBuilt = true;
			const FVector At = ClientPenguin->GetActorLocation();
			const float HeadTop = static_cast<float>(At.Z) + ClientCapsule->GetScaledCapsuleHalfHeight();
			const FIntPoint Over = RoofIsland->WorldToCell(FVector(At.X, 0.f, HeadTop + RoofIsland->CellSize));
			TArray<FIntPoint> RoofCells;
			for (int32 Column = -2; Column <= 2; ++Column)
			{
				RoofCells.Add(FIntPoint(Over.X + Column, Over.Y));
			}
			const int32 Added = RoofIsland->AddBlocks(RoofCells, EBlockKind::Rock, 0.f).Num();
			RoofIsland->FlushEdits();
			Report(FString::Printf(TEXT("traits: round 2 roof (PINGU_NETTRAITS_ROOF): %d blocks over the client's penguin at x %.0f, row %d (its head top at z %.0f)"), Added,
				At.X, Over.Y, HeadTop));
		}
	}

	// Rounds 1 and 2: the hold starts in the countdown already. A bot whose brain was still on in the fight's first frame
	// could use an ability right at GO (2026-10-20: the bot's iceberg, dropped at GO, knocked the client's penguin out
	// before its Second Chance check began).
	if (!bLate && Phase == EPinguPhase::Countdown && ((Round == 1 && !N.bHoldOneOver) || (Round == 2 && !N.bHoldTwoOver)))
	{
		Hold();
		// Round 2: open sky over the client's head for its triple jump (see MakeRoomForJump). The host moves the client's penguin
		// (a NetTeleport the host's move judge knows of); the client only measures.
		if (Round == 2 && !N.bRoomChecked)
		{
			APenguinCharacter* Jumper = bHostRole ? GetOtherPlayerPenguin() : Own;
			const FString RoomLine = Jumper ? MakeRoomForJump(Jumper, N, bHostRole) : FString();
			if (!RoomLine.IsEmpty())
			{
				Report(bHostRole ? TEXT("host: ") + RoomLine : RoomLine);
			}
		}
		return true;
	}

	if (bLate || Phase != EPinguPhase::Fight)
	{
		// (The holds are rounds 1 and 2's fights: anything else ends them.)
		if (!bLate && Phase != EPinguPhase::Intro && Phase != EPinguPhase::Countdown)
		{
			if (Round >= 1 && !N.bHoldOneOver)
			{
				N.bHoldOneOver = true;
				ReleaseHold();
			}
			if (Round >= 2 && !N.bHoldTwoOver)
			{
				N.bHoldTwoOver = true;
				ReleaseHold();
			}
		}
		return false;
	}

	// ---- Round 1's looks (a moment into the fight: the transform is long done).
	if (Round == 1)
	{
		if (!N.bLooksSampled && T > LookAt)
		{
			N.bLooksSampled = true;
			N.HostSize = HostPenguin ? HostPenguin->GetSizeScale() : 0.f;
			const UPenguinAbilityComponent* ClientAbilities = ClientPenguin ? ClientPenguin->GetAbilities() : nullptr;
			N.ClientAbilities = CountReal(ClientAbilities);
			N.bClientSlotOneEmpty = ClientAbilities && ClientAbilities->GetAbilities().Num() == 3 && ClientAbilities->GetAbility(1) == EPenguinAbility::None;
			N.ClientLoadout.Reset();
			for (int32 Slot = 0; ClientAbilities && Slot < ClientAbilities->GetAbilities().Num(); ++Slot)
			{
				N.ClientLoadout += FString::Printf(TEXT("%s%s"), Slot > 0 ? TEXT(" / ") : TEXT(""), UPenguinAbilityComponent::GetInfo(ClientAbilities->GetAbility(Slot)).Name);
			}
			Report(FString::Printf(TEXT("traits: round 1 looks here: the host at %.2f, the client's abilities %s"), N.HostSize, *N.ClientLoadout));
		}
		for (const APenguinCharacter* Bot : Bots)
		{
			N.bGlassOverlay |= Bot->IsAlive() && TraitOf(Bot) == EPinguTrait::GlassCannon && HasOverlay(Bot);
		}
	}

	// ---- Round 1: the client's Second Chance - into the sea (a fish throws it back), still in a moment later, into the sea
	// again (a knock-out).
	if (Round == 1 && !N.bHoldOneOver)
	{
		const ABlockIsland* Island = ABlockIsland::Get(World);
		const UPenguinMovementComponent* ClientMove = ClientPenguin ? ClientPenguin->GetPenguinMovement() : nullptr;
		const UPenguinTraitComponent* ClientTraits = ClientPenguin ? ClientPenguin->GetTraits() : nullptr;
		const auto DropIntoSea = [this, Island, ClientPenguin](float Out)
		{
			const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
			const float Edge = Bounds.IsValid ? static_cast<float>(Bounds.Max.X) : 2000.f;
			const float SeaZ = Island ? Island->SeaLevel : -1650.f;
			ClientPenguin->NetTeleport(FVector(Edge + Out, 0.f, SeaZ + DropAboveSea), FVector(0.f, 0.f, -300.f), true);
			Report(FString::Printf(TEXT("traits: the client's penguin dropped into the sea %.0f cm past the island's end"), Out));
		};
		if (bHostRole && ClientPenguin && !N.bFirstDrop && T >= DropAt)
		{
			N.bFirstDrop = true;
			DropIntoSea(DropOut);
		}
		if (ClientTraits && N.RescuedAt < 0.f && ClientTraits->GetEvents().SecondChanceAt >= 0.f)
		{
			N.RescuedAt = WorldTime;
			const FBox IslandBounds = Island ? Island->GetBounds() : FBox(ForceInit);
			N.IslandTop = IslandBounds.IsValid ? static_cast<float>(IslandBounds.Max.Z) : -1.e9f;
			Report(TEXT("traits: the client's penguin thrown back out of the sea (Second Chance)"));
		}
		if (ClientPenguin && N.RescuedAt >= 0.f && !N.bSecondDrop && ClientMove)
		{
			N.FastestUp = FMath::Max(N.FastestUp, static_cast<float>(ClientMove->Velocity.Z));
			N.HighestFeet = FMath::Max(N.HighestFeet, static_cast<float>(ClientPenguin->GetFeetLocation().Z));
			N.bPassSeen |= ClientMove->IsPassingThroughIsland();
			if (!N.bAliveSampled && WorldTime > N.RescuedAt + AliveAfter)
			{
				N.bAliveSampled = true;
				N.bAliveAfter = ClientPenguin->IsAlive();
			}
		}
		if (ClientPenguin && ClientPenguin->IsKnockedOut() && N.KOAt < 0.f)
		{
			N.KOAt = WorldTime;
			N.KOReason = static_cast<uint8>(ClientPenguin->GetKOReason());
			N.bKOBeforeRescue = N.RescuedAt < 0.f;
			if (bHostRole && ClientTraits)
			{
				N.Rescues = ClientTraits->GetCounters().SecondChances;
			}
		}
		if (ClientPenguin && N.KOAt >= 0.f && N.KOReason == 0 && ClientPenguin->GetKOReason() != EPenguinKOReason::None)
		{
			N.KOReason = static_cast<uint8>(ClientPenguin->GetKOReason()); // (the reason can replicate a frame later)
		}
		if (bHostRole && ClientPenguin && N.bFirstDrop && !N.bSecondDrop && ClientPenguin->IsAlive()
			&& ((N.RescuedAt >= 0.f && WorldTime > N.RescuedAt + SecondDropAfter) || T > SecondDropLatest))
		{
			N.bSecondDrop = true;
			DropIntoSea(DropFarOut);
		}
		if (!bHostRole && N.RescuedAt >= 0.f && WorldTime > N.RescuedAt + SecondDropAfter)
		{
			N.bSecondDrop = true; // (the client: from here on the host drops it again)
		}
		if ((N.KOAt >= 0.f && WorldTime > N.KOAt + HoldAfterKO) || T > HoldOneTo)
		{
			N.bHoldOneOver = true;
			ReleaseHold();
			Report(FString::Printf(TEXT("traits: round 1's hold over (%.1f s into the fight)"), T));
			return false;
		}
		Hold();
		return true;
	}

	// ---- Round 2: the client's penguin (Featherweight) jumps and presses three times in the air: two air jumps come out.
	if (Round == 2 && !N.bHoldTwoOver)
	{
		if (T > HoldTwoTo)
		{
			N.bHoldTwoOver = true;
			ReleaseHold();
			return false;
		}
		Hold();
		if (bHostRole && !N.bFizzleFired && T > FizzleAt)
		{
			// The bot's Gambler (every roll a fizzle from now on) uses its first ability, aimed at the client.
			for (APenguinCharacter* Bot : Bots)
			{
				UPenguinTraitComponent* BotTraits = Bot->GetTraits();
				if (BotTraits && BotTraits->HasTrait(EPinguTrait::Gambler) && Bot->IsAlive() && Bot->GetAbilities())
				{
					N.bFizzleFired = true;
					BotTraits->SetForcedGamble(0);
					Bot->SetAimPoint(ClientPenguin ? ClientPenguin->GetAimOrigin() : Bot->GetAimOrigin() + FVector(300.f, 0.f, 0.f));
					const bool bUsed = Bot->ActivateAbility(0);
					Report(FString::Printf(TEXT("traits: round 2 the bot's Gambler (slot %d) uses %s: %s"), Bot->GetPlayerSlot(),
						UPenguinAbilityComponent::GetInfo(Bot->GetAbilities()->GetAbility(0)).Name, bUsed ? TEXT("pressed") : TEXT("refused")));
					break;
				}
			}
		}
		if (!bHostRole && Own)
		{
			const UPenguinMovementComponent* OwnMove = Own->GetPenguinMovement();
			if (OwnMove && !N.bJumpBase && T > JumpBaseAt)
			{
				N.bJumpBase = true;
				N.JumpsAtBase = OwnMove->JumpEvents;
				N.AirJumpsAtBase = OwnMove->AirJumpEvents;
				const ABlockIsland* RoomIsland = ABlockIsland::Get(World);
				const UCapsuleComponent* OwnCapsule = Own->GetCapsuleComponent();
				if (RoomIsland && OwnCapsule)
				{
					N.RoomAtJump = RoomAbove(RoomIsland, Own->GetActorLocation(), OwnCapsule->GetScaledCapsuleRadius(), OwnCapsule->GetScaledCapsuleHalfHeight(), 1000.f);
				}
			}
			if (N.bJumpBase && N.JumpStage < 8)
			{
				const int32 Press = N.JumpStage / 2;
				const bool bDown = N.JumpStage % 2 == 0;
				if (T > JumpPressAt[Press] + (bDown ? 0.f : JumpHold))
				{
					if (bDown)
					{
						Own->PressJump();
					}
					else
					{
						Own->ReleaseJump();
					}
					++N.JumpStage;
				}
			}
			if (OwnMove && N.bJumpBase && !N.bJumpJudged && T > JumpJudgeAt)
			{
				N.bJumpJudged = true;
				N.GroundJumps = OwnMove->JumpEvents - N.JumpsAtBase;
				N.AirJumps = OwnMove->AirJumpEvents - N.AirJumpsAtBase;
				N.MaxAirJumps = OwnMove->GetMaxAirJumpsNow();
				Report(FString::Printf(TEXT("traits: round 2 triple jump: jumps %d, air jumps %d of 3 presses (%d allowed), %.0f cm of room over the head at the start (spawn had %.0f%s)"),
						N.GroundJumps, N.AirJumps, N.MaxAirJumps, N.RoomAtJump, N.RoomAtStart, N.bMovedForRoom ? TEXT(", moved") : TEXT("")));
			}
		}
		return true;
	}
	return false;
}

void APinguNetTest::AddTraitsChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check)
{
	using namespace PinguNetTestTraitsPrivate;
	const FNetTraits& N = State();
	const auto RoundCheck = [&Check, &N](int32 Index, const TCHAR* Label)
	{
		Check(Label, N.bRoundSeen[Index] && N.bRoundOk[Index], N.bRoundSeen[Index] ? N.RoundLine[Index] : FString(TEXT("round not seen")));
	};
	if (IsLateRun())
	{
		if (bHostRole)
		{
			Check(TEXT("traits: the late client dealt its trait in its first round"), N.bMatchTraitsOn && N.ClientFirstRound >= 1 && N.ClientFirstTrait == EPinguTrait::GlassCannon,
				FString::Printf(TEXT("traits on %d, its first round %d, its trait %s (want Glass Cannon)"), N.bMatchTraitsOn ? 1 : 0, N.ClientFirstRound,
					*PinguTraits::GetLogName(N.ClientFirstTrait)));
		}
		else
		{
			Check(TEXT("traits: the others' looks when joining, quietly (no reveal for them)"), N.bJoinJudged && N.JoinHostSize > GiantSize - 0.05f && N.bJoinOverlay
				&& N.bJoinTransformed && N.JoinCards == 0,
				FString::Printf(TEXT("the host at %.2f (want 1.68), the bot's glass %d, the host's trait shown at once %d, cards %d (want 0)"), N.JoinHostSize,
					N.bJoinOverlay ? 1 : 0, N.bJoinTransformed ? 1 : 0, N.JoinCards));
			Check(TEXT("traits: own trait and its reveal in its first round"), N.OwnFirstRound >= 1 && N.OwnFirstTrait == EPinguTrait::GlassCannon && N.OwnRoundCards >= 3,
				FString::Printf(TEXT("round %d, own trait %s (want Glass Cannon), cards %d (want 3)"), N.OwnFirstRound, *PinguTraits::GetLogName(N.OwnFirstTrait), N.OwnRoundCards));
		}
		return;
	}
	const float ShortestIntro = FMath::Min(N.IntroLongest[0], N.IntroLongest[1]);
	Check(TEXT("traits: the top-left corner icon shows this machine's own trait in the fight"), N.CornerFightFrames >= 30 && N.CornerShownFrames * 10 >= N.CornerFightFrames * 8,
		FString::Printf(TEXT("shown in %d of %d frames (want 80 %% or more of at least 30)"), N.CornerShownFrames, N.CornerFightFrames));
	if (bHostRole)
	{
		Check(TEXT("traits: TRAITS on from the lobby's toggle"), N.bToggledOn && N.bMatchTraitsOn,
			FString::Printf(TEXT("on after the toggle %d, the match with traits %d"), N.bToggledOn ? 1 : 0, N.bMatchTraitsOn ? 1 : 0));
		RoundCheck(0, TEXT("traits: round 1 as forced (host Giant, client Second Chance, bot Glass Cannon)"));
		RoundCheck(1, TEXT("traits: round 2 as forced (host Heavyweight, client Featherweight, bot Gambler)"));
		Check(TEXT("traits: the intro waits for the reveal (4.8 s or more)"), ShortestIntro >= 4.8f,
			FString::Printf(TEXT("round 1 %.2f s, round 2 %.2f s"), N.IntroLongest[0], N.IntroLongest[1]));
		Check(TEXT("traits: round 2 the bot's Gambler fizzles: the dust here and its icon crumbling"), N.bFizzleFired && N.FizzleDustEvents >= 1 && N.IconDust >= 1,
			FString::Printf(TEXT("forced %d, dust bursts %d, icons crumbled %d (want 1 or more each)"), N.bFizzleFired ? 1 : 0, N.FizzleDustEvents, N.IconDust));
		Check(TEXT("traits: the host's Giant at 1.68"), FMath::Abs(N.HostSize - GiantSize) < 0.05f, FString::Printf(TEXT("%.2f"), N.HostSize));
		Check(TEXT("traits: the client's Second Chance - one rescue, then a knock-out"), N.RescuedAt >= 0.f && N.bAliveAfter && N.bSecondDrop
			&& N.KOReason == static_cast<uint8>(EPenguinKOReason::Sea) && !N.bKOBeforeRescue && N.Rescues == 1,
			FString::Printf(TEXT("thrown back %d, up at %.0f cm/s, feet up to z %.0f (the island's top %.0f), through the island's blocks seen here %d, still in %.1f s later %d, "
				"dropped again %d, knocked out %s, rescues counted %d"),
				N.RescuedAt >= 0.f ? 1 : 0, N.FastestUp, N.HighestFeet, N.IslandTop, N.bPassSeen ? 1 : 0, AliveAfter, N.bAliveAfter ? 1 : 0, N.bSecondDrop ? 1 : 0,
				*UEnum::GetValueAsString(static_cast<EPenguinKOReason>(N.KOReason)), N.Rescues));
		Check(TEXT("traits: the client's Second Chance penguin has two abilities"), N.ClientAbilities == 2 && N.bClientSlotOneEmpty,
			FString::Printf(TEXT("%s"), N.ClientLoadout.IsEmpty() ? TEXT("not seen") : *N.ClientLoadout));
		Check(TEXT("traits: round 3 one trait for everybody, from the host's list"), N.bRoundThreeSeen && N.bRoundThreeShared,
			FString::Printf(TEXT("round trait %s (want Low Gravity or Turbo, on every penguin): %s"), *PinguTraits::GetLogName(N.RoundThreeTrait),
				N.bRoundThreeSeen ? *N.RoundThreeLine : TEXT("round 3 not seen")));
	}
	else
	{
		Check(TEXT("traits: TRAITS ON seen in the lobby"), N.bLobbyOnSeen, N.bLobbyOnSeen ? FString(TEXT("yes")) : FString(TEXT("never")));
		Check(TEXT("traits: round 1's reveal - 3 cards, 3 flying in"), N.MostCards >= 3 && N.MostFlying >= 3,
			FString::Printf(TEXT("cards up to %d, flying up to %d"), N.MostCards, N.MostFlying));
		RoundCheck(0, TEXT("traits: round 1 as forced (host Giant, own Second Chance, bot Glass Cannon)"));
		RoundCheck(1, TEXT("traits: round 2 as forced (host Heavyweight, own Featherweight, bot Gambler)"));
		Check(TEXT("traits: the host's Giant at 1.68 here"), FMath::Abs(N.HostSize - GiantSize) < 0.05f, FString::Printf(TEXT("%.2f"), N.HostSize));
		Check(TEXT("traits: the Glass Cannon bot's glass look here"), N.bGlassOverlay, N.bGlassOverlay ? FString(TEXT("seen")) : FString(TEXT("no overlay seen")));
		// (Its own movement runs the throw: through the island's blocks on the way up, its feet over the highest one.)
		Check(TEXT("traits: own Second Chance seen - thrown back up through the island and over it, still in, then out"), N.RescuedAt >= 0.f && N.FastestUp > 2000.f
			&& N.bPassSeen && N.HighestFeet > N.IslandTop && N.bAliveAfter && N.KOReason == static_cast<uint8>(EPenguinKOReason::Sea) && !N.bKOBeforeRescue,
			FString::Printf(TEXT("thrown back %d, up at %.0f cm/s, through the island's blocks %d, feet up to z %.0f (want over the island's top %.0f), still in %.1f s later %d, knocked out %s"),
				N.RescuedAt >= 0.f ? 1 : 0, N.FastestUp, N.bPassSeen ? 1 : 0, N.HighestFeet, N.IslandTop, AliveAfter, N.bAliveAfter ? 1 : 0,
				*UEnum::GetValueAsString(static_cast<EPenguinKOReason>(N.KOReason))));
		Check(TEXT("traits: own two abilities (slot 1 empty)"), N.ClientAbilities == 2 && N.bClientSlotOneEmpty,
			FString::Printf(TEXT("%s"), N.ClientLoadout.IsEmpty() ? TEXT("not seen") : *N.ClientLoadout));
		Check(TEXT("traits: round 2 the bot's Gambler fizzles: the dust here and its icon crumbling"), N.FizzleDustEvents >= 1 && N.IconDust >= 1,
			FString::Printf(TEXT("dust bursts %d, icons crumbled %d (want 1 or more each)"), N.FizzleDustEvents, N.IconDust));
		Check(TEXT("traits: round 2 own triple jump (Featherweight)"), N.GroundJumps >= 1 && N.AirJumps == 2 && N.MaxAirJumps == 2,
			FString::Printf(TEXT("jumps %d, air jumps %d of 3 presses (want 2), allowed %d, room over the head %.0f cm (spawn %.0f%s)"), N.GroundJumps, N.AirJumps,
					N.MaxAirJumps, N.RoomAtJump, N.RoomAtStart, N.bMovedForRoom ? TEXT(", moved") : TEXT("")));
		Check(TEXT("traits: round 3 one trait for everybody here, from the host's list"), N.bRoundThreeSeen && N.bRoundThreeShared,
			FString::Printf(TEXT("round trait %s (want Low Gravity or Turbo, on every penguin): %s"), *PinguTraits::GetLogName(N.RoundThreeTrait),
				N.bRoundThreeSeen ? *N.RoundThreeLine : TEXT("round 3 not seen")));
		Check(TEXT("traits: round 3's one banner here"), N.bBannerThree && N.BannerThreeName == PinguTraits::GetLogName(N.RoundThreeTrait),
			FString::Printf(TEXT("banner %d (%s), want the round trait's"), N.bBannerThree ? 1 : 0, *N.BannerThreeName));
	}
}

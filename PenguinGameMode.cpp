// Pingu - game flow: menu demo, matches of rounds on random islands, practice; the LAN lobby and LAN matches.
//
// Server only (a LAN client has no game mode). What every machine shows lives in APenguinGameState (phase, rounds,
// scores, feed, winners, settings), which also plays the flow's music, sounds and camera moments on every machine.
// Solo games run exactly as before; a listen server (hosting) starts in the lobby and never shows the bots' demo.

#include "PenguinGameMode.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinAutoTest.h"
#include "PenguinBot.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PenguinPlayerController.h"
#include "PenguinPlayerState.h"
#include "PinguAntiCheat.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguAbilityPicks.h"
#include "PinguFX.h"
#include "PinguLobby.h"
#include "PinguNames.h"
#include "PinguNetSubsystem.h"
#include "PinguPerfTest.h"
#include "PinguPickup.h"
#include "PinguPortal.h"
#include "PinguRoundHazards.h"
#include "PinguTornado.h"
#include "PinguGiantSnowball.h"
#include "PinguSnowman.h"
#include "PinguSettings.h"
#include "EngineUtils.h"
#include "Engine/NetConnection.h"
#include "GameFramework/GameSession.h"
#include "TimerManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "PinguKillCam.h"

namespace PenguinGameModePrivate
{
	/** Knock-outs this close together (real seconds) count as "at the same moment": the only way a round ends in a draw
	 *  (besides time running out). Whoever outlasts the others by more than this wins, even if it falls into the sea or
	 *  gets blown up right after. (Real time: what the players see. The slow motion that starts with the decisive
	 *  knock-out stretches game time, so a game-time window would call splashes everybody saw one after the other a draw.) */
	constexpr double SimultaneousKOWindow = 0.05;
	constexpr int32 RoundTestRounds = 3;     // -PinguRoundTest: one round per case (see UpdateRoundTest)
	/** More names than bots (up to 9): every bot of a match has its own. */
	static const TCHAR* BotNames[] = { TEXT("Waddles"), TEXT("Frosty"), TEXT("Pebble"), TEXT("Chilly"), TEXT("Slushy"), TEXT("Flipper"), TEXT("Nibbles"),
		TEXT("Sprinkles"), TEXT("Puddles"), TEXT("Mochi"), TEXT("Blizzard"), TEXT("Sardine") };
	static_assert(UE_ARRAY_COUNT(BotNames) >= PinguMaxPenguins, "a name for every bot");
	constexpr float RoundOverTime = 5.2f;   // the winner banner, then the leaderboard (APenguinGameState::LeaderboardDelay)
	constexpr int32 MaxPickups = 4;          // sky drops wait while this many ability bubbles are around
	constexpr int32 EndlessTestRounds = 3;   // -PinguMatchTest -PinguEndless: quit after this many rounds
	constexpr float MenuRoundRestart = 2.5f;
	constexpr float RoundTimeLimit = 150.f;
	/** Practice and lobby: a knocked-out penguin is back on the island after this long. */
	constexpr float RespawnDelay = 1.1f;
	/** ... a snow bomb's after this long: it stands charred for a moment and crumbles into its ash pile first (about 1.1 s,
	 *  APenguinCharacter's death look), and the pile with its eyes shows for a moment before it is back. */
	constexpr float BombRespawnDelay = 2.3f;
	/** Penguins in a match (humans + bots); also the most people in a LAN game. */
	constexpr int32 MaxPenguins = PinguMaxPenguins;
	/** The title menu's demo: this many penguins play (the player's own included). */
	constexpr int32 MenuDemoPenguins = 4;
	/** The island is made for the penguins of the round or the lobby (ABlockIsland::GetArenaSize: 2 to 10). Lobby: this long
	 *  after the count changed (somebody joined or left, a bot came or went) there is an island of the new size - a few
	 *  clicks on the cards make one new island, not one each. */
	constexpr int32 FewestIslandPenguins = 2;
	constexpr float LobbyResizeDelay = 0.5f;
	/** A round's starting places: at least this many (both ends), so somebody arriving later still finds a far one. */
	constexpr int32 MinRoundStarts = 2;
	/** FindFarthestStart: places spread over the island to choose from. */
	constexpr int32 FarthestStartCandidates = 13;
	/** The engine cuts player names to 20 characters too. */
	constexpr int32 MaxNameLength = 20;
	/** Lobby: a fresh island once this share of its blocks was blown away. */
	constexpr float LobbyWornOut = 0.4f;

	/** A match is running (a LAN player joining now plays from the next round). */
	bool IsMatchPhase(EPinguPhase Phase)
	{
		return Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight
			|| Phase == EPinguPhase::RoundOver || Phase == EPinguPhase::MatchOver;
	}

	FString CleanName(const FString& Name)
	{
		// (No control or zero-width characters, one kind of space, cut to the length cap: PinguNames.h.)
		return PinguNames::Sanitize(Name);
	}

	/** Out = the valid penguins of In, in slot order (TArray sorts arrays of pointers through the pointers). */
	void SetSortedBySlot(TArray<TObjectPtr<APenguinCharacter>>& Out, TArray<APenguinCharacter*> In)
	{
		In.RemoveAll([](const APenguinCharacter* Penguin) { return !IsValid(Penguin); });
		In.StableSort([](const APenguinCharacter& A, const APenguinCharacter& B) { return A.GetPlayerSlot() < B.GetPlayerSlot(); });
		Out.Reset();
		for (APenguinCharacter* Penguin : In)
		{
			Out.Add(Penguin);
		}
	}

	int32 LowestFreeSlot(uint32 UsedSlots)
	{
		for (int32 Slot = 0; Slot < MaxPenguins; ++Slot)
		{
			if ((UsedSlots & (1u << Slot)) == 0)
			{
				return Slot;
			}
		}
		return INDEX_NONE;
	}
}

APenguinGameMode::APenguinGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	DefaultPawnClass = APenguinCharacter::StaticClass();
	PlayerControllerClass = APenguinPlayerController::StaticClass();
	HUDClass = APenguinHUD::StaticClass();
	GameStateClass = APenguinGameState::StaticClass();
	PlayerStateClass = APenguinPlayerState::StaticClass();
	Rand.GenerateNewSeed();
}

void APenguinGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	// Prefer the Blueprint child so the movement can be tuned in the editor without recompiling.
	if (DefaultPawnClass == APenguinCharacter::StaticClass() && FPackageName::DoesPackageExist(TEXT("/Game/Penguin/BP_Penguin")))
	{
		const FSoftClassPath BlueprintPath(TEXT("/Game/Penguin/BP_Penguin.BP_Penguin_C"));
		if (UClass* BlueprintClass = BlueprintPath.TryLoadClass<APenguinCharacter>())
		{
			DefaultPawnClass = BlueprintClass;
		}
	}
	PenguinClass = DefaultPawnClass.Get() && DefaultPawnClass->IsChildOf(APenguinCharacter::StaticClass())
		? TSubclassOf<APenguinCharacter>(DefaultPawnClass.Get()) : TSubclassOf<APenguinCharacter>(APenguinCharacter::StaticClass());
	// -PinguCollapseAt=<seconds> moves the sudden death, -PinguPickupEvery=<seconds> the sky drops (tests).
	FParse::Value(FCommandLine::Get(), TEXT("PinguCollapseAt="), SuddenDeathAfter);
	FParse::Value(FCommandLine::Get(), TEXT("PinguPickupEvery="), PickupInterval);
	PickupInterval = FMath::Max(PickupInterval, 1.f);
}

void APenguinGameMode::StartPlay()
{
	Super::StartPlay();
	APenguinCamera::FindOrSpawn(GetWorld());
	APinguFX::Get(this);
	bFlowStarted = true;
	FString NetTestRole;
	bNetTest = FParse::Param(FCommandLine::Get(), TEXT("PinguNetTest")) || FParse::Value(FCommandLine::Get(), TEXT("PinguNetTest="), NetTestRole);

	if (IsNetworkGame())
	{
		// Hosting (the map came up as a listen server, from HostGame's ?listen?Lobby or the editor's net mode): everybody
		// meets in the lobby. None of the solo hooks below run in a LAN game (-PinguNetTest drives it from outside).
		UE_LOG(LogPingu, Display, TEXT("[GAME] LAN game (%s): lobby"), UGameplayStatics::HasOption(OptionsString, TEXT("Lobby")) ? TEXT("hosting") : TEXT("listen server"));
		EnterLobby();
		return;
	}

	const TCHAR* CommandLine = FCommandLine::Get();
	// -PinguTraits: the test matches below play in Traits mode. -PinguTrait=<Name>[,<Name>...]: those traits, by slot (one
	// name: everybody) - Traits mode too.
	FString TraitList;
	if (FParse::Value(CommandLine, TEXT("PinguTrait="), TraitList, false))
	{
		SetForcedTraits(PinguTraits::ParseList(TraitList));
	}
	const bool bTraitSwitch = FParse::Param(CommandLine, TEXT("PinguTraits")) || ForcedTraits.Num() > 0;
	if (FParse::Param(CommandLine, TEXT("PinguAutoTest")))
	{
		StartPractice();
		GetWorld()->SpawnActor<APenguinAutoTest>();
		return;
	}
	if (FParse::Param(CommandLine, TEXT("PinguMatchTest")))
	{
		bMatchTest = true;
		if (APenguinGameState* PenguinState = GetPenguinState())
		{
			PenguinState->SetMatchTest(true);
		}
#if !UE_BUILD_SHIPPING
		APinguAntiCheat::StartSelfJudge(GetWorld()); // the joiners' judge watches this honest bots' match: it must find nothing
#endif
		Rand.Initialize(4242);
		FPinguMatchSettings TestSettings;
		TestSettings.NumBots = 3;
		TestSettings.Difficulty = 2;
		TestSettings.RoundsToWin = 2;
		TestSettings.bEndless = FParse::Param(CommandLine, TEXT("PinguEndless"));
		TestSettings.bTraits = bTraitSwitch;
		// -PinguBots=<1..9> -PinguTeams=<2..10>: a bigger match, or a team match (the team test: everybody evened out over
		// that many teams).
		FParse::Value(CommandLine, TEXT("PinguBots="), TestSettings.NumBots);
		FParse::Value(CommandLine, TEXT("PinguTeams="), TestSettings.NumTeams);
		if (!FParse::Value(CommandLine, TEXT("PinguPickupEvery="), PickupInterval))
		{
			PickupInterval = 6.f; // more ability bubbles to see in the test's screenshots
		}
		StartMatch(TestSettings);
		return;
	}
	if (FParse::Param(CommandLine, TEXT("PinguRoundTest")))
	{
		// Scripted knock-outs (UpdateRoundTest) check who wins a round; the bots stand still for it.
		bRoundTest = true;
		Rand.Initialize(4243);
		FPinguMatchSettings TestSettings;
		TestSettings.NumBots = 3;
		TestSettings.RoundsToWin = 9;
		StartMatch(TestSettings);
		return;
	}
	if (FParse::Param(CommandLine, TEXT("PinguPerfTest")))
	{
		// A bots-only match without end (every penguin plays by itself) while APinguPerfTest measures each graphics
		// preset. If the test starts a match of its own, its penguins play by themselves too.
		bPerfTest = true;
		FPinguMatchSettings PerfSettings;
		PerfSettings.NumBots = 3;
		PerfSettings.Difficulty = 2;
		PerfSettings.RoundsToWin = 9;
		PerfSettings.bEndless = true;
		PerfSettings.bTraits = bTraitSwitch;
		FParse::Value(CommandLine, TEXT("PinguBots="), PerfSettings.NumBots);  // -PinguBots=9: ten penguins
		StartMatch(PerfSettings);
		GetWorld()->SpawnActor<APinguPerfTest>();
		return;
	}
	if (FParse::Param(CommandLine, TEXT("PinguPractice")))
	{
		StartPractice();
		return;
	}
	bMenuShot = FParse::Param(CommandLine, TEXT("PinguMenuShot"));
	if (FParse::Param(CommandLine, TEXT("PinguInputTest")))
	{
		// Real keyboard input from the title menu on: jumps while walking (see APenguinInputTest).
		GetWorld()->SpawnActor<APenguinInputTest>();
	}
	if (FParse::Param(CommandLine, TEXT("PinguLobbyTest")))
	{
		// From the title like a player: PLAY, the lobby's cards, a team match, the leaderboard's key (APinguLobbyTest).
		Rand.Initialize(4244);
		GetWorld()->SpawnActor<APinguLobbyTest>();
	}
	EnterMenu();
}

void APenguinGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bShuttingDown = true;
	Super::EndPlay(EndPlayReason);
}

AActor* APenguinGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (!IslandStarts.Contains(*It))
		{
			return Super::ChoosePlayerStart_Implementation(Player);
		}
	}
	if (IslandStarts.Num() == 0)
	{
		if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
		{
			for (const FVector& Location : Island->GetSpawnLocations())
			{
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				if (APlayerStart* Start = GetWorld()->SpawnActor<APlayerStart>(Location, FRotator::ZeroRotator, Params))
				{
					IslandStarts.Add(Start);
				}
			}
		}
	}
	if (IslandStarts.Num() > 0)
	{
		return IslandStarts[NextStart++ % IslandStarts.Num()];
	}
	return Super::ChoosePlayerStart_Implementation(Player);
}

void APenguinGameMode::RestartPlayerAtPlayerStart(AController* NewPlayer, AActor* StartSpot)
{
	Super::RestartPlayerAtPlayerStart(NewPlayer, StartSpot);
	if (APenguinCharacter* Penguin = NewPlayer ? Cast<APenguinCharacter>(NewPlayer->GetPawn()) : nullptr)
	{
		Penguin->SetSpawnLocation(Penguin->GetActorLocation());
	}
	if (NewPlayer)
	{
		NewPlayer->SetControlRotation(FRotator::ZeroRotator);
	}
}

void APenguinGameMode::RestartPlayerAtTransform(AController* NewPlayer, const FTransform& SpawnTransform)
{
	Super::RestartPlayerAtTransform(NewPlayer, SpawnTransform);
	if (APenguinCharacter* Penguin = NewPlayer ? Cast<APenguinCharacter>(NewPlayer->GetPawn()) : nullptr)
	{
		Penguin->SetSpawnLocation(Penguin->GetActorLocation());
	}
	if (NewPlayer)
	{
		NewPlayer->SetControlRotation(FRotator::ZeroRotator);
	}
}

APawn* APenguinGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	// The editor's Play button spawns the player at the viewport camera, turned like the camera. A turned actor makes the
	// mesh (whose left/right facing is a relative yaw) look the wrong way, so penguins always spawn unrotated.
	FTransform Upright = SpawnTransform;
	Upright.SetRotation(FQuat::Identity);
	return Super::SpawnDefaultPawnAtTransform_Implementation(NewPlayer, Upright);
}

// ---------------------------------------------------------------------------- state

APenguinGameState* APenguinGameMode::GetPenguinState() const
{
	return GetGameState<APenguinGameState>();
}

bool APenguinGameMode::IsNetworkGame() const
{
	return GetNetMode() != NM_Standalone;
}

EPinguPhase APenguinGameMode::GetPhase() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetPhase() : EPinguPhase::Menu;
}

float APenguinGameMode::GetPhaseTime() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetPhaseTime() : 0.f;
}

int32 APenguinGameMode::GetRoundNumber() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetRoundNumber() : 0;
}

const TArray<FPinguScore>& APenguinGameMode::GetScores() const
{
	static const TArray<FPinguScore> NoScores;
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetScores() : NoScores;
}

const TArray<FPinguFeedEntry>& APenguinGameMode::GetFeed() const
{
	static const TArray<FPinguFeedEntry> NoFeed;
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetFeed() : NoFeed;
}

int32 APenguinGameMode::GetRoundWinnerSlot() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetRoundWinnerSlot() : -1;
}

int32 APenguinGameMode::GetMatchWinnerSlot() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetMatchWinnerSlot() : -1;
}

const FPinguMatchSettings& APenguinGameMode::GetSettings() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetSettings() : FallbackSettings;
}

FPinguMatchSettings& APenguinGameMode::GetMutableSettings()
{
	APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetMutableSettings() : FallbackSettings;
}

bool APenguinGameMode::IsEndless() const
{
	return GetSettings().bEndless;
}

TArray<int32> APenguinGameMode::GetLeaderboard() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetLeaderboard() : TArray<int32>();
}

int32 APenguinGameMode::GetCountdownNumber() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState ? PenguinState->GetCountdownNumber() : -1;
}

bool APenguinGameMode::AllowsManualRespawn() const
{
	const EPinguPhase Phase = GetPhase();
	return Phase == EPinguPhase::Practice || Phase == EPinguPhase::Lobby;
}

void APenguinGameMode::SetPhase(EPinguPhase NewPhase)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	PenguinState->SetPhase(NewPhase);
	UE_LOG(LogPingu, Display, TEXT("[GAME] phase %s (round %d)"), *UEnum::GetValueAsString(NewPhase), PenguinState->GetRoundNumber());
	UpdateAdvertisedInfo();
}

void APenguinGameMode::AddFeed(const FString& Text, const FLinearColor& Color)
{
	if (APenguinGameState* PenguinState = GetPenguinState())
	{
		PenguinState->AddFeed(Text, Color);
	}
}

void APenguinGameMode::SetSlowMotion(float Dilation)
{
	if (!IsNetworkGame())
	{
		APinguFX::SetSlowMotion(this, Dilation);
	}
}

// ---------------------------------------------------------------------------- players

TArray<APenguinPlayerState*> APenguinGameMode::GetHumanPlayers(const APlayerState* Ignore) const
{
	TArray<APenguinPlayerState*> Humans;
	if (const APenguinGameState* PenguinState = GetPenguinState())
	{
		for (APenguinPlayerState* Player : PenguinState->GetPenguinPlayers())
		{
			// A player whose controller is going away (leaving right now) no longer counts.
			const AController* OwningController = Player->GetOwningController();
			if (Player != Ignore && (!OwningController || !OwningController->IsPendingKillPending()))
			{
				Humans.Add(Player);
			}
		}
	}
	return Humans;
}

int32 APenguinGameMode::CountHumans(const APlayerState* Ignore) const
{
	return GetHumanPlayers(Ignore).Num();
}

int32 APenguinGameMode::FindFreeHumanSlot(const APenguinPlayerState* For) const
{
	uint32 UsedSlots = 0;
	for (const APenguinPlayerState* Player : GetHumanPlayers())
	{
		const int32 Slot = Player->GetPenguinSlot();
		if (Player != For && Slot >= 0 && Slot < PenguinGameModePrivate::MaxPenguins)
		{
			UsedSlots |= 1u << Slot;
		}
	}
	return PenguinGameModePrivate::LowestFreeSlot(UsedSlots);
}

FString APenguinGameMode::GetJoinRefusal() const
{
	// Plain sentences: the joining player's game shows them as they are. A running match is no reason (they play from its
	// next round) unless the host turned "Join running matches" off in the settings; the lobby always works.
	if (CountHumans() >= PenguinGameModePrivate::MaxPenguins)
	{
		return TEXT("The game is full.");
	}
	const APenguinGameState* PenguinState = GetPenguinState();
	if (bFlowStarted && PenguinState && PenguinGameModePrivate::IsMatchPhase(PenguinState->GetPhase()))
	{
		const UPinguSettings* PinguSettings = UPinguSettings::Get();
		if (PinguSettings && !PinguSettings->GetJoinRunningMatches())
		{
			return TEXT("The match is running - join again when it ends (the host turned off joining running matches).");
		}
	}
	return FString();
}

FString APenguinGameMode::GetHumanName(const APlayerState* Player) const
{
	if (!IsNetworkGame())
	{
		return TEXT("You");
	}
	const FString Name = Player ? Player->GetPlayerName() : FString();
	return Name.IsEmpty() ? FString(TEXT("Pingu")) : Name;
}

void APenguinGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	const FString Refusal = GetJoinRefusal();
	if (!Refusal.IsEmpty())
	{
		ErrorMessage = Refusal;
		RefusedRunningJoins += Refusal.StartsWith(TEXT("The match is running")) ? 1 : 0; // (the LAN test counts them)
	}
	else if (ErrorMessage == TEXT("Server full."))
	{
		ErrorMessage = TEXT("The game is full.");
	}
	if (!ErrorMessage.IsEmpty())
	{
		UE_LOG(LogPingu, Display, TEXT("[GAME] a player from %s cannot join: %s"), *Address, *ErrorMessage);
	}
}

APlayerController* APenguinGameMode::Login(UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	// A player from the LAN (the host's own player logs in locally): PreLogin let them in, but somebody else may have taken
	// the last place while they were loading the map.
	if (InRemoteRole == ROLE_AutonomousProxy && bFlowStarted)
	{
		const FString Refusal = GetJoinRefusal();
		if (!Refusal.IsEmpty())
		{
			ErrorMessage = Refusal;
			UE_LOG(LogPingu, Display, TEXT("[GAME] a joining player is turned away: %s"), *ErrorMessage);
			return nullptr;
		}
	}
	return Super::Login(NewPlayer, InRemoteRole, Portal, Options, UniqueId, ErrorMessage);
}

FString APenguinGameMode::InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal)
{
	const FString Error = Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
	if (!NewPlayerController)
	{
		return Error;
	}
	FString Name;
	if (NewPlayerController->IsLocalController())
	{
		// The host (or the solo player): the name from the settings.
		if (const UPinguSettings* PinguSettings = UPinguSettings::Get())
		{
			Name = PinguSettings->GetPlayerName();
		}
	}
	else
	{
		// ?PinguName arrives untouched (the engine may have replaced ?Name with an online nickname).
		Name = UGameplayStatics::ParseOption(Options, TEXT("PinguName"));
	}
	Name = PenguinGameModePrivate::CleanName(Name);
	if (Name.IsEmpty() && !NewPlayerController->IsLocalController())
	{
		Name = TEXT("Pingu"); // (a joiner whose name was nothing but invisible characters)
	}
	if (!Name.IsEmpty())
	{
		ChangeName(NewPlayerController, Name, false);
	}
	return Error;
}

void APenguinGameMode::ChangeName(AController* Controller, const FString& NewName, bool bNameChange)
{
	using namespace PenguinGameModePrivate;
	FString Name = CleanName(NewName);
	if (!Controller || Name.IsEmpty())
	{
		return;
	}
	if (IsNetworkGame())
	{
		// Two players with one name, or names that look alike (case, 0 / O, l / I, Cyrillic letters ...), would be confusing in the
		// kill feed and let somebody pose as the host: the later one gets a number (PinguNames::MakeUnique).
		TArray<FString> Taken;
		for (const APenguinPlayerState* Other : GetHumanPlayers(Controller->PlayerState))
		{
			Taken.Add(Other->GetPlayerName());
		}
		Name = PinguNames::MakeUnique(Name, Taken);
	}
	Super::ChangeName(Controller, Name, bNameChange);

	// The scoreboard and the penguin (feed lines, name tags) follow.
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || !Controller->PlayerState)
	{
		return;
	}
	for (FPinguScore& Row : PenguinState->GetMutableScores())
	{
		if (Row.PlayerState == Controller->PlayerState)
		{
			Row.Name = GetHumanName(Row.PlayerState);
			if (IsValid(Row.Penguin))
			{
				Row.Penguin->SetPlayerSlot(Row.Slot, Row.Name);
			}
		}
	}
}

void APenguinGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	if (bFlowStarted)
	{
		const APenguinPlayerState* NewPlayerState = NewPlayer ? NewPlayer->GetPlayerState<APenguinPlayerState>() : nullptr;
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s joined (slot %d, %d players)"), NewPlayerState ? *NewPlayerState->GetPlayerName() : TEXT("?"),
			NewPlayerState ? NewPlayerState->GetPenguinSlot() : -1, CountHumans());
		UpdateAdvertisedInfo();
	}
}

void APenguinGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	APenguinPlayerState* NewPlayerState = NewPlayer ? NewPlayer->GetPlayerState<APenguinPlayerState>() : nullptr;
	const EPinguPhase Phase = GetPhase();
	const bool bMidMatch = bFlowStarted && IsNetworkGame() && PenguinGameModePrivate::IsMatchPhase(Phase);
	if (NewPlayerState)
	{
		NewPlayerState->SetIsHost(NewPlayer->IsLocalController());
		if (bMidMatch)
		{
			// The slot comes with the penguin at the next round (one no penguin of the match has then).
			NewPlayerState->SetPenguinSlot(-1);
			NewPlayerState->SetWaitingForRound(true);
		}
		else if (NewPlayerState->GetPenguinSlot() < 0)
		{
			NewPlayerState->SetPenguinSlot(FindFreeHumanSlot(NewPlayerState));
		}
	}
	if (bFlowStarted && Phase == EPinguPhase::Lobby)
	{
		// Straight onto the lobby island, at the slot's starting place (somebody standing right there: a player start).
		if (NewPlayerState && NewPlayerState->GetPenguinSlot() >= 0 && !bStartPlayersAsSpectators && !MustSpectate(NewPlayer) && PlayerCanRestart(NewPlayer))
		{
			RestartPlayerAtTransform(NewPlayer, FTransform(FRotator::ZeroRotator, GetStartLocation(NewPlayerState->GetPenguinSlot())));
			if (!NewPlayer->GetPawn())
			{
				RestartPlayer(NewPlayer);
			}
		}
		AddLobbyPlayer(NewPlayer);
		return;
	}
	if (bMidMatch)
	{
		// A match is running: they watch (the shared camera) and play from the next round (AdmitWaitingPlayers; the next
		// match, a rematch or the lobby brings their penguin too).
		const FString Name = GetHumanName(NewPlayerState);
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s joined during the match: plays from the next round"), *Name);
		AddFeed(FString::Printf(TEXT("%s joined - plays from the next round."), *Name), FLinearColor::White);
		return;
	}
	// Solo, and the host's own player while the map loads: the penguin spawns now, StartPlay sets up the rest.
	Super::HandleStartingNewPlayer_Implementation(NewPlayer);
}

void APenguinGameMode::AddLobbyPlayer(APlayerController* NewPlayer)
{
	APenguinPlayerState* NewPlayerState = NewPlayer ? NewPlayer->GetPlayerState<APenguinPlayerState>() : nullptr;
	APenguinCharacter* Penguin = NewPlayer ? Cast<APenguinCharacter>(NewPlayer->GetPawn()) : nullptr;
	APenguinGameState* PenguinState = GetPenguinState();
	if (!NewPlayerState || !Penguin || !PenguinState)
	{
		return;
	}
	const FString Name = GetHumanName(NewPlayerState);
	Penguin->SetPlayerSlot(NewPlayerState->GetPenguinSlot(), Name);
	if (UPenguinBotComponent* Bot = Penguin->GetBot())
	{
		if (Bot->IsBotEnabled())
		{
			Bot->SetBotEnabled(false);
		}
	}
	if (UPenguinAbilityComponent* Abilities = Penguin->GetAbilities())
	{
		// The lobby island: no abilities (everybody just hangs around); the old lobby: three to try.
		Abilities->SetAbilities(bLobbyIsland ? TArray<EPenguinAbility>() : UPenguinAbilityComponent::RollAbilities(Rand, 3));
	}
	Penguin->SetInputLocked(false);

	TArray<APenguinCharacter*> Sorted;
	for (APenguinCharacter* Existing : Penguins)
	{
		if (IsValid(Existing) && Existing != Penguin)
		{
			Sorted.Add(Existing);
		}
	}
	Sorted.Add(Penguin);
	PenguinGameModePrivate::SetSortedBySlot(Penguins, Sorted);
	RebuildScores();
	// The host's bots must still fit next to the people (the last ones make room; on the lobby island it puffs away).
	PenguinState->SetSettings(PrepareSettings(PenguinState->GetSettings()));
	SyncLobbyBots();
	AddFeed(FString::Printf(TEXT("%s joined the game."), *Name), Penguin->GetPlayerColor());
}

void APenguinGameMode::Logout(AController* Exiting)
{
	APenguinPlayerState* Leaving = Exiting ? Exiting->GetPlayerState<APenguinPlayerState>() : nullptr;
	APawn* LeavingPawn = Exiting ? Exiting->GetPawn() : nullptr;
	const int32 LeavingId = Leaving ? Leaving->GetPlayerId() : INDEX_NONE;
	Super::Logout(Exiting);
	const UWorld* World = GetWorld();
	if (!Leaving || !bFlowStarted || bShuttingDown || !World || World->bIsTearingDown)
	{
		return;  // the world is going away (the host left, or a new map loads): nothing to tidy up
	}
	if (LeavingId != INDEX_NONE)
	{
		// Their seed value leaves the next reveal (a joiner who comes later would take it for a value under "no joiner").
		for (TActorIterator<APinguAntiCheat> It(const_cast<UWorld*>(World)); It; ++It)
		{
			It->HostForgetPlayer(LeavingId);
		}
	}
	RemovePlayer(Leaving, LeavingPawn);
	UpdateAdvertisedInfo(Leaving);
}

void APenguinGameMode::RemovePlayer(APenguinPlayerState* Leaving, APawn* LeavingPawn)
{
	using namespace PenguinGameModePrivate;
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || !Leaving)
	{
		return;
	}
	// Their penguin: the controller has usually let go of it already, the row still knows it.
	APenguinCharacter* Penguin = Cast<APenguinCharacter>(LeavingPawn);
	FString Name = GetHumanName(Leaving);
	TArray<FPinguScore>& Scores = PenguinState->GetMutableScores();
	const int32 Row = Scores.IndexOfByPredicate([Leaving](const FPinguScore& Entry) { return Entry.PlayerState == Leaving; });
	if (Row != INDEX_NONE)
	{
		if (!Penguin)
		{
			Penguin = Scores[Row].Penguin;
		}
		Name = Scores[Row].Name;
		Scores.RemoveAt(Row);
	}
	if (!Penguin)
	{
		Penguin = Leaving->GetPenguin();
	}
	const FLinearColor Color = Penguin ? Penguin->GetPlayerColor() : FLinearColor::White;
	const int32 LeaverTeam = Penguin ? Penguin->GetTeam() : -1;
	if (Penguin)
	{
		Penguins.Remove(Penguin);
		PendingRespawns.RemoveAll([Penguin](const FPendingRespawn& Entry) { return Entry.Penguin.Get() == Penguin; });
		if (IsValid(Penguin))
		{
			Penguin->Destroy();
		}
	}
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s left (%d players)"), *Name, CountHumans(Leaving));
	if (IsNetworkGame())
	{
		AddFeed(FString::Printf(TEXT("%s left the game."), *Name), Color);
	}

	const EPinguPhase Phase = PenguinState->GetPhase();
	if (Phase == EPinguPhase::Lobby)
	{
		CompactLobbySlots();
		TArray<APenguinCharacter*> Remaining;
		for (APenguinCharacter* Other : Penguins)
		{
			Remaining.Add(Other);
		}
		SetSortedBySlot(Penguins, Remaining);
		RebuildScores();
		return;
	}
	if (IsNetworkGame() && IsMatchPhase(Phase) && Penguins.Num() < 2)
	{
		// Nobody left to play against.
		EnterLobby();
		AddFeed(TEXT("Everybody else left: back to the lobby."), FLinearColor::White);
		return;
	}
	if (IsMatchPhase(Phase) && IsTeamMatch())
	{
		// A team match needs two sides: a whole team left - a bot of the other one changes sides, else back to the lobby.
		uint32 TeamsWithPenguins = 0;
		int32 Remaining = -1;
		for (const APenguinCharacter* Other : Penguins)
		{
			if (IsValid(Other) && Other->GetTeam() >= 0)
			{
				TeamsWithPenguins |= 1u << Other->GetTeam();
				Remaining = Other->GetTeam();
			}
		}
		if (FMath::CountBits(TeamsWithPenguins) < 2)
		{
			APenguinCharacter* Switcher = nullptr;
			for (APenguinCharacter* Other : Penguins)
			{
				if (IsValid(Other) && !Other->GetPlayerState())
				{
					Switcher = Other;  // the last bot (highest slot)
				}
			}
			const int32 NewTeam = LeaverTeam >= 0 && LeaverTeam != Remaining ? LeaverTeam : (Remaining == 0 ? 1 : 0);
			if (!Switcher)
			{
				EnterLobby();
				AddFeed(TEXT("Only one side is left: back to the lobby."), FLinearColor::White);
				return;
			}
			Switcher->SetTeam(NewTeam);
			if (FPinguScore* SwitcherRow = PenguinState->GetMutableScores().FindByPredicate([Switcher](const FPinguScore& Entry) { return Entry.Penguin == Switcher; }))
			{
				SwitcherRow->Team = NewTeam;
			}
			if (NewTeam != LeaverTeam)
			{
				// A team nobody had: the switcher's own (no wins a team of an earlier round may have left there).
				LoneTeams |= 1u << NewTeam;
				TArray<int32>& TeamWins = PenguinState->GetMutableTeamWins();
				if (TeamWins.IsValidIndex(NewTeam))
				{
					TeamWins[NewTeam] = 0;
				}
			}
			// It is alone in that team now: on its own, as the screens show it (no team name).
			AddFeed(FString::Printf(TEXT("%s changes sides."), *Switcher->GetDisplayName()), Switcher->GetPlayerColor());
			UE_LOG(LogPingu, Display, TEXT("[GAME] a whole team left: %s changes sides (team %d, %s)"), *Switcher->GetDisplayName(), NewTeam,
				PenguinState->GetShownTeam(NewTeam) >= 0 ? *APenguinCharacter::GetTeamName(NewTeam).ToString() : TEXT("on its own"));
		}
	}
	if (Phase == EPinguPhase::Fight)
	{
		CheckRoundEnd(nullptr);  // they may have been the last one the others were fighting
	}
}

void APenguinGameMode::CompactLobbySlots()
{
	// The humans by slot (the host first): 0, 1, 2, ... A colour may change here, never in a match.
	int32 NextSlot = 0;
	for (APenguinPlayerState* Player : GetHumanPlayers())
	{
		if (NextSlot >= PenguinGameModePrivate::MaxPenguins)
		{
			break;
		}
		const int32 Slot = NextSlot++;
		if (Player->GetPenguinSlot() != Slot)
		{
			Player->SetPenguinSlot(Slot);
			if (APenguinCharacter* Penguin = Player->GetPenguin())
			{
				Penguin->SetPlayerSlot(Slot, GetHumanName(Player));
			}
		}
	}
}

void APenguinGameMode::RebuildScores()
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	TArray<FPinguScore>& Scores = PenguinState->GetMutableScores();
	Scores.Reset();
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (!IsValid(Penguin))
		{
			continue;
		}
		FPinguScore& Row = Scores.AddDefaulted_GetRef();
		Row.Penguin = Penguin;
		Row.PlayerState = Penguin->GetPlayerState();
		Row.bHuman = Row.PlayerState != nullptr;
		Row.Name = Penguin->GetDisplayName();
		Row.Slot = Penguin->GetPlayerSlot();
	}
	Scores.StableSort([](const FPinguScore& A, const FPinguScore& B) { return A.Slot < B.Slot; });
}

FPinguMatchSettings APenguinGameMode::ClampSettings(const FPinguMatchSettings& InSettings) const
{
	using namespace PenguinGameModePrivate;
	FPinguMatchSettings Clamped = InSettings;
	const int32 Humans = IsNetworkGame() ? CountHumans() : 1;
	if (IsNetworkGame())
	{
		Clamped.NumBots = FMath::Clamp(Clamped.NumBots, 0, FMath::Max(MaxPenguins - Humans, 0));
	}
	else
	{
		Clamped.NumBots = FMath::Clamp(Clamped.NumBots, 1, MaxPenguins - 1);
	}
	Clamped.Difficulty = FMath::Clamp(Clamped.Difficulty, 0, 2);
	Clamped.RoundsToWin = FMath::Clamp(Clamped.RoundsToWin, 1, PinguMaxRoundsToWin);
	// (Tests) evened-out teams: none, or 2..10 - never more than there are penguins (a match has at least two).
	const int32 MaxTeams = FMath::Min(PinguMaxTeams, FMath::Max(Humans + Clamped.NumBots, 2));
	Clamped.NumTeams = Clamped.NumTeams < 2 ? 0 : FMath::Min(Clamped.NumTeams, MaxTeams);
	// A setup per bot: the extra ones go from the end (somebody joined), the missing ones (a test asked for bots by number)
	// get the settings' difficulty and no team.
	const int32 HadSetups = Clamped.Bots.Num();
	Clamped.Bots.SetNum(Clamped.NumBots);
	for (int32 Index = 0; Index < Clamped.Bots.Num(); ++Index)
	{
		FPinguBotSetup& Bot = Clamped.Bots[Index];
		if (Index >= HadSetups)
		{
			Bot = FPinguBotSetup();
			Bot.Skill = Clamped.Difficulty;
		}
		Bot.Skill = FMath::Clamp(Bot.Skill, 0, 2);
		Bot.Team = Bot.Team >= 0 && Bot.Team < PinguMaxTeams ? Bot.Team : -1;
		Bot.Name = CleanName(Bot.Name);
	}
	return Clamped;
}

FPinguMatchSettings APenguinGameMode::PrepareSettings(const FPinguMatchSettings& InSettings)
{
	using namespace PenguinGameModePrivate;
	FPinguMatchSettings Prepared = ClampSettings(InSettings);
	// Every bot its own name (none of the people's either): a new bot gets a random free one.
	TSet<FString> Taken;
	for (const APenguinPlayerState* Player : GetHumanPlayers())
	{
		Taken.Add(Player->GetPlayerName().ToLower());
	}
	for (FPinguBotSetup& Bot : Prepared.Bots)
	{
		const FString Key = Bot.Name.ToLower();
		if (!Bot.Name.IsEmpty() && !Taken.Contains(Key))
		{
			Taken.Add(Key);
			continue;
		}
		TArray<const TCHAR*> Free;
		for (const TCHAR* Candidate : BotNames)
		{
			if (!Taken.Contains(FString(Candidate).ToLower()))
			{
				Free.Add(Candidate);
			}
		}
		Bot.Name = Free.Num() > 0 ? FString(Free[Rand.RandRange(0, Free.Num() - 1)]) : FString::Printf(TEXT("Bot %d"), Taken.Num() + 1);
		Taken.Add(Bot.Name.ToLower());
	}
	return Prepared;
}

const FPinguBotSetup* APenguinGameMode::FindBotSetup(const APenguinCharacter* Bot) const
{
	if (!Bot || Bot->GetPlayerState())
	{
		return nullptr;
	}
	const FString& Name = Bot->GetDisplayName();
	return GetSettings().Bots.FindByPredicate([&Name](const FPinguBotSetup& Setup) { return Setup.Name == Name; });
}

void APenguinGameMode::UpdateAdvertisedInfo(const APlayerState* Leaving) const
{
	if (!IsNetworkGame() || !bFlowStarted)
	{
		return;
	}
	if (UPinguNetSubsystem* Net = UPinguNetSubsystem::Get(this))
	{
		Net->SetAdvertisedInfo(CountHumans(Leaving), GetPhase() != EPinguPhase::Lobby);
	}
}

// ---------------------------------------------------------------------------- penguins

APenguinCharacter* APenguinGameMode::GetHumanPenguin() const
{
	const APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return PlayerController ? Cast<APenguinCharacter>(PlayerController->GetPawn()) : nullptr;
}

APenguinCharacter* APenguinGameMode::SpawnBotPenguin(int32 Slot)
{
	UWorld* World = GetWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Location(Slot * 300.f - 450.f, 0.f, 400.f);
	APenguinCharacter* Penguin = World->SpawnActor<APenguinCharacter>(PenguinClass ? PenguinClass.Get() : APenguinCharacter::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (!Penguin)
	{
		return nullptr;
	}
	if (APenguinBotController* BotController = World->SpawnActor<APenguinBotController>(Params))
	{
		BotController->Possess(Penguin);
		BotControllers.Add(BotController);
	}
	return Penguin;
}

void APenguinGameMode::DestroyBot(APenguinCharacter* Bot)
{
	if (!Bot)
	{
		return;
	}
	if (AController* BotController = Bot->GetController())
	{
		BotControllers.Remove(Cast<APenguinBotController>(BotController));
		BotController->Destroy();
	}
	Penguins.Remove(Bot);
	PendingRespawns.RemoveAll([Bot](const FPendingRespawn& Entry) { return Entry.Penguin.Get() == Bot; });
	Bot->Destroy();
}

void APenguinGameMode::EnsurePenguins(int32 NumBots)
{
	using namespace PenguinGameModePrivate;
	// Humans keep their penguins and slots. A player still without a penguin (they arrived while a match ran) gets one.
	TArray<APenguinCharacter*> Kept;
	uint32 UsedSlots = 0;
	for (APenguinPlayerState* Player : GetHumanPlayers())
	{
		int32 Slot = Player->GetPenguinSlot();
		if (Slot < 0 || Slot >= MaxPenguins || (UsedSlots & (1u << Slot)) != 0)
		{
			Slot = LowestFreeSlot(UsedSlots);
			Player->SetPenguinSlot(Slot);
		}
		if (Slot == INDEX_NONE)
		{
			continue;  // more people than penguins (a LAN game never lets that many in)
		}
		UsedSlots |= 1u << Slot;  // a human's slot, even while their penguin is missing: no bot takes it
		APlayerController* PlayerController = Player->GetPlayerController();
		if (PlayerController && !PlayerController->GetPawn() && PlayerCanRestart(PlayerController))
		{
			RestartPlayer(PlayerController);
		}
		APenguinCharacter* Penguin = Player->GetPenguin();
		if (!IsValid(Penguin))
		{
			continue;
		}
		Penguin->SetPlayerSlot(Slot, GetHumanName(Player));
		Player->SetWaitingForRound(false);  // (arrived during the last match: in from now on)
		Kept.Add(Penguin);
	}
	const int32 HumanCount = Kept.Num();
	NumBots = FMath::Clamp(NumBots, 0, MaxPenguins - HumanCount);

	// Bots: keep the ones there are (in order), drop the extra ones, then spawn the missing ones.
	TArray<APenguinCharacter*> Bots;
	TArray<APenguinCharacter*> Previous;
	for (APenguinCharacter* Penguin : Penguins)
	{
		Previous.Add(Penguin);
	}
	if (bKeepBotLooks)
	{
		// The lobby island's bots go on as themselves (into the match and back): each its setup's place, so it keeps its name.
		const TArray<FPinguBotSetup>& Order = GetSettings().Bots;
		const auto PlaceOf = [&Order](const APenguinCharacter* Penguin)
		{
			const int32 Place = IsValid(Penguin) ? Order.IndexOfByPredicate([Penguin](const FPinguBotSetup& Setup) { return Setup.Name == Penguin->GetDisplayName(); }) : INDEX_NONE;
			return Place == INDEX_NONE ? 1000 : Place;
		};
		Previous.StableSort([&PlaceOf](const APenguinCharacter& A, const APenguinCharacter& B) { return PlaceOf(&A) < PlaceOf(&B); });
	}
	for (APenguinCharacter* Penguin : Previous)
	{
		if (!IsValid(Penguin) || Kept.Contains(Penguin) || Penguin->GetPlayerState())
		{
			continue;
		}
		if (Bots.Num() < NumBots)
		{
			Bots.Add(Penguin);
		}
		else
		{
			DestroyBot(Penguin);
		}
	}
	// A bot brain without a penguin in the list (should not happen) goes too.
	const TArray<TObjectPtr<APenguinBotController>> Controllers = BotControllers;
	for (APenguinBotController* BotController : Controllers)
	{
		APenguinCharacter* Pawn = BotController ? Cast<APenguinCharacter>(BotController->GetPawn()) : nullptr;
		if (!Pawn || !Bots.Contains(Pawn))
		{
			if (Pawn)
			{
				DestroyBot(Pawn);
			}
			else if (BotController)
			{
				BotControllers.Remove(BotController);
				BotController->Destroy();
			}
		}
	}
	// Bots take the free slots in order (never a human's). The lobby island's bots keep the slot (the colour) they have.
	TArray<int32> BotSlots;
	BotSlots.Init(INDEX_NONE, NumBots);
	uint32 TakenSlots = UsedSlots;
	if (bKeepBotLooks)
	{
		for (int32 Index = 0; Index < Bots.Num() && Index < NumBots; ++Index)
		{
			const int32 Own = Bots[Index]->GetPlayerSlot();
			if (Own >= 0 && Own < MaxPenguins && (TakenSlots & (1u << Own)) == 0)
			{
				TakenSlots |= 1u << Own;
				BotSlots[Index] = Own;
			}
		}
	}
	for (int32 Index = 0; Index < NumBots; ++Index)
	{
		if (BotSlots[Index] != INDEX_NONE)
		{
			continue;
		}
		const int32 Slot = LowestFreeSlot(TakenSlots);
		if (Slot == INDEX_NONE)
		{
			BotSlots.SetNum(Index);
			break;
		}
		TakenSlots |= 1u << Slot;
		BotSlots[Index] = Slot;
	}
	while (Bots.Num() < BotSlots.Num())
	{
		APenguinCharacter* Bot = SpawnBotPenguin(BotSlots[Bots.Num()]);
		if (!Bot)
		{
			break;
		}
		Bots.Add(Bot);
	}

	// Names and colours: the lobby's bots by their setups (in order), any more (the menu's demo) a free name each.
	const TArray<FPinguBotSetup>& Setups = GetSettings().Bots;
	TSet<FString> Taken;
	for (const APenguinCharacter* Person : Kept)
	{
		Taken.Add(Person->GetDisplayName().ToLower());
	}
	for (const FPinguBotSetup& Setup : Setups)
	{
		Taken.Add(Setup.Name.ToLower());
	}
	TArray<int32> NameOrder;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(BotNames); ++Index)
	{
		NameOrder.Insert(Index, Rand.RandRange(0, NameOrder.Num()));
	}
	int32 NextName = 0;
	for (int32 Index = 0; Index < Bots.Num() && Index < BotSlots.Num(); ++Index)
	{
		FString Name = Setups.IsValidIndex(Index) ? Setups[Index].Name : FString();
		while (Name.IsEmpty() && NextName < NameOrder.Num())
		{
			const FString Candidate(BotNames[NameOrder[NextName++]]);
			if (!Taken.Contains(Candidate.ToLower()))
			{
				Name = Candidate;
				Taken.Add(Candidate.ToLower());
			}
		}
		Bots[Index]->SetPlayerSlot(BotSlots[Index], Name.IsEmpty() ? FString::Printf(TEXT("Bot %d"), Index + 1) : Name);
		Kept.Add(Bots[Index]);
	}
	SetSortedBySlot(Penguins, Kept);
	RebuildScores();
}

void APenguinGameMode::DisableLobbyBrains()
{
	for (APenguinCharacter* Penguin : Penguins)
	{
		UPenguinBotComponent* Bot = IsValid(Penguin) ? Penguin->GetBot() : nullptr;
		if (Bot && Bot->IsBotEnabled())
		{
			Bot->SetBotEnabled(false); // the lobby island: APinguLobby walks the bots around
		}
	}
}

void APenguinGameMode::SyncLobbyBots()
{
	using namespace PenguinGameModePrivate;
	if (!bLobbyIsland || GetPhase() != EPinguPhase::Lobby)
	{
		return;
	}
	const TArray<FPinguBotSetup>& Setups = GetSettings().Bots;
	APinguLobby* Lobby = APinguLobby::FindOrSpawn(GetWorld());
	// Bots the settings no longer have puff away (an X, or a joining player took the place).
	const TArray<TObjectPtr<APenguinCharacter>> Previous = Penguins;
	for (APenguinCharacter* Penguin : Previous)
	{
		if (!IsValid(Penguin) || Penguin->GetPlayerState())
		{
			continue;
		}
		const FString Name = Penguin->GetDisplayName();
		if (Setups.ContainsByPredicate([&Name](const FPinguBotSetup& Setup) { return Setup.Name == Name; }))
		{
			continue;
		}
		if (AController* BotController = Penguin->GetController())
		{
			BotControllers.Remove(Cast<APenguinBotController>(BotController));
			BotController->UnPossess();
			BotController->Destroy();
		}
		Penguins.Remove(Penguin);
		PendingRespawns.RemoveAll([Penguin](const FPendingRespawn& Entry) { return Entry.Penguin.Get() == Penguin; });
		UE_LOG(LogPingu, Display, TEXT("[LOBBY] %s puffs away"), *Name);
		if (Lobby)
		{
			Lobby->PuffAway(Penguin); // it shrinks into the dust, then goes
		}
		else
		{
			Penguin->Destroy();
		}
	}
	// The people's colours first: a bot wearing a colour a person just got (somebody joined) changes to a free one.
	uint32 UsedSlots = 0;
	for (const APenguinPlayerState* Player : GetHumanPlayers())
	{
		if (Player->GetPenguinSlot() >= 0 && Player->GetPenguinSlot() < MaxPenguins)
		{
			UsedSlots |= 1u << Player->GetPenguinSlot();
		}
	}
	bool bChanged = Previous.Num() != Penguins.Num();
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (!IsValid(Penguin) || Penguin->GetPlayerState())
		{
			continue;
		}
		int32 Slot = Penguin->GetPlayerSlot();
		if (Slot < 0 || Slot >= MaxPenguins || (UsedSlots & (1u << Slot)) != 0)
		{
			Slot = LowestFreeSlot(UsedSlots);
			if (Slot != INDEX_NONE)
			{
				Penguin->SetPlayerSlot(Slot, Penguin->GetDisplayName());
				bChanged = true;
			}
		}
		if (Slot != INDEX_NONE)
		{
			UsedSlots |= 1u << Slot;
		}
	}
	// New ones puff in where there is room (each in a free colour).
	for (const FPinguBotSetup& Setup : Setups)
	{
		const bool bThere = Penguins.ContainsByPredicate([&Setup](const APenguinCharacter* Penguin)
		{
			return IsValid(Penguin) && !Penguin->GetPlayerState() && Penguin->GetDisplayName() == Setup.Name;
		});
		const int32 Slot = bThere ? INDEX_NONE : LowestFreeSlot(UsedSlots);
		if (Slot == INDEX_NONE)
		{
			continue;
		}
		APenguinCharacter* Bot = SpawnBotPenguin(Slot);
		if (!Bot)
		{
			continue;
		}
		UsedSlots |= 1u << Slot;
		Bot->SetPlayerSlot(Slot, Setup.Name);
		if (UPenguinBotComponent* Brain = Bot->GetBot())
		{
			Brain->SetBotEnabled(false);
		}
		FVector Where = GetStartLocation(Slot);
		FindFarthestStart(Bot, Where);
		FVector Step;
		ABlockIsland* LobbyIsland = ABlockIsland::Get(GetWorld());
		if (LobbyIsland && LobbyIsland->GetLobbyStepTop(Step) && Where.X > Step.X - 2.f * LobbyIsland->CellSize - 40.f)
		{
			// Not on the "+"'s step (the "+" floats over it): the farthest place left of it.
			float Best = -1.f;
			for (const FVector& Candidate : LobbyIsland->GetSpreadSpawnLocations(13))
			{
				if (Candidate.X > Step.X - 2.f * LobbyIsland->CellSize - 40.f)
				{
					continue;
				}
				float Nearest = TNumericLimits<float>::Max();
				for (const APenguinCharacter* Other : Penguins)
				{
					if (IsValid(Other) && Other != Bot)
					{
						Nearest = FMath::Min(Nearest, static_cast<float>(FVector::Dist(Candidate, Other->GetActorLocation())));
					}
				}
				if (Nearest > Best)
				{
					Best = Nearest;
					Where = Candidate;
				}
			}
		}
		Bot->ResetForRound(Where);
		if (UPenguinAbilityComponent* Abilities = Bot->GetAbilities())
		{
			Abilities->SetAbilities(TArray<EPenguinAbility>());
		}
		Bot->SetInputLocked(false);
		Penguins.Add(Bot);
		bChanged = true;
		UE_LOG(LogPingu, Display, TEXT("[LOBBY] %s puffs in (slot %d)"), *Setup.Name, Slot);
	}
	if (bChanged)
	{
		TArray<APenguinCharacter*> Sorted;
		for (APenguinCharacter* Penguin : Penguins)
		{
			Sorted.Add(Penguin);
		}
		SetSortedBySlot(Penguins, Sorted);
		RebuildScores();
	}
}

void APenguinGameMode::ApplyBotBrains()
{
	// The solo tests let the player's penguin play by itself too. Otherwise a human's penguin never gets a brain (in a
	// LAN game that would be a remote player's penguin).
	const bool bAllBots = !IsNetworkGame() && (bMatchTest || bPerfTest);
	for (APenguinCharacter* Penguin : Penguins)
	{
		UPenguinBotComponent* Bot = IsValid(Penguin) ? Penguin->GetBot() : nullptr;
		if (!Bot)
		{
			continue;
		}
		if (bRoundTest)
		{
			if (Bot->IsBotEnabled())
			{
				Bot->SetBotEnabled(false); // -PinguRoundTest: everybody stands still for the scripted knock-outs
			}
			continue;
		}
		if (!Penguin->GetPlayerState() || bAllBots)
		{
			// Each bot at the skill its lobby card says (a bot the settings don't know: their difficulty).
			const FPinguBotSetup* Setup = FindBotSetup(Penguin);
			const int32 Skill = Setup ? Setup->Skill : GetSettings().Difficulty;
			Bot->SetBotEnabled(true, bAllBots ? 2 : Skill);
		}
		else if (Bot->IsBotEnabled())
		{
			Bot->SetBotEnabled(false);
		}
	}
}

APenguinCharacter* APenguinGameMode::GetLastSurvivor() const
{
	APenguinCharacter* Survivor = nullptr;
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin) && Penguin->IsAlive())
		{
			if (Survivor)
			{
				return nullptr;
			}
			Survivor = Penguin;
		}
	}
	return Survivor;
}

bool APenguinGameMode::IsOverGround(const APenguinCharacter* Penguin) const
{
	const UPenguinMovementComponent* Move = Penguin ? Penguin->GetPenguinMovement() : nullptr;
	if (!Move || Move->IsMovingOnGround())
	{
		return true;
	}
	const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Penguin->GetActorLocation());
	float GroundZ = 0.f;
	return !Island || Island->FindGroundBelow(Penguin->GetFeetLocation(), 3000.f, GroundZ);
}

int32 APenguinGameMode::CountAlive() const
{
	int32 Alive = 0;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		Alive += (IsValid(Penguin) && Penguin->IsAlive()) ? 1 : 0;
	}
	return Alive;
}

bool APenguinGameMode::IsTeamMatch() const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PenguinState && PenguinState->IsTeamMatch();
}

int32 APenguinGameMode::CountAliveSides() const
{
	// Everybody for themselves: every penguin standing is a side. A team match: every team with a penguin standing.
	const bool bTeams = IsTeamMatch();
	int32 Sides = 0;
	uint32 TeamsSeen = 0;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		if (!IsValid(Penguin) || !Penguin->IsAlive())
		{
			continue;
		}
		const int32 Team = Penguin->GetTeam();
		if (bTeams && Team >= 0)
		{
			if ((TeamsSeen & (1u << Team)) == 0)
			{
				TeamsSeen |= 1u << Team;
				++Sides;
			}
		}
		else
		{
			++Sides;
		}
	}
	return Sides;
}

int32 APenguinGameMode::GetAliveTeam() const
{
	int32 Found = -1;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		if (!IsValid(Penguin) || !Penguin->IsAlive())
		{
			continue;
		}
		if (Penguin->GetTeam() < 0 || (Found >= 0 && Penguin->GetTeam() != Found))
		{
			return -1;
		}
		Found = Penguin->GetTeam();
	}
	return Found;
}

bool APenguinGameMode::IsFinalVictim(const APenguinCharacter* Penguin) const
{
	if (!Penguin || !Penguin->IsAlive() || bRoundEnding || GetPhase() != EPinguPhase::Fight || !Penguins.ContainsByPredicate([Penguin](const TObjectPtr<APenguinCharacter>& Each) { return Each.Get() == Penguin; }) || CountAliveSides() != 2)
	{
		return false;
	}
	const int32 Team = Penguin->GetTeam();
	return !IsTeamMatch() || Team < 0 || CountAliveInTeam(Team) == 1;
}

int32 APenguinGameMode::CountAliveInTeam(int32 Team) const
{
	int32 Alive = 0;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		Alive += (IsValid(Penguin) && Penguin->IsAlive() && Team >= 0 && Penguin->GetTeam() == Team) ? 1 : 0;
	}
	return Alive;
}

bool APenguinGameMode::IsWinnerFalling() const
{
	if (DecidedTeam >= 0)
	{
		// The winning team: falling only when none of its penguins still standing is over the island.
		bool bAny = false;
		for (const APenguinCharacter* Penguin : Penguins)
		{
			if (IsValid(Penguin) && Penguin->IsAlive() && Penguin->GetTeam() == DecidedTeam)
			{
				if (IsOverGround(Penguin))
				{
					return false;
				}
				bAny = true;
			}
		}
		return bAny;
	}
	const APenguinCharacter* Survivor = GetLastSurvivor();
	return Survivor && !IsOverGround(Survivor);
}

// ---------------------------------------------------------------------------- teams, statistics, late arrivals

void APenguinGameMode::OnPenguinHit(APenguinCharacter* By, APenguinCharacter* Victim)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!By || !Victim || By == Victim || !PenguinState || GetPhase() != EPinguPhase::Fight || bRoundEnding)
	{
		return;
	}
	if (By->IsTeammate(Victim))
	{
		// Friendly fire: it lands, but a hit on your own team is no hit for the statistics.
		++FriendlyHits;
		return;
	}
	const int32 Row = PenguinState->FindScoreIndex(By->GetPlayerSlot());
	if (Row != INDEX_NONE && PenguinState->GetScores()[Row].Penguin == By)
	{
		++PenguinState->GetMutableScores()[Row].Hits;
	}
}

void APenguinGameMode::LogBombStats() const
{
	// A bomb knocks out its own thrower too now: the bots keep out of their own blasts (PenguinBot: AvoidBlast, the throw's
	// landing check), so this should stay rare.
	UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] RESULT bombs: %d went off, %d of the %d knock-outs were by a bomb, %d of those the thrower's own (self-knock-outs, credited to nobody)"),
		BombsExploded, BombKnockOuts, FightKnockOuts, SelfBombKnockOuts);
}

void APenguinGameMode::SetTeamChoice(APenguinPlayerState* Player, int32 Team)
{
	if (!Player)
	{
		return;
	}
	const int32 Choice = Team >= 0 && Team < PinguMaxTeams ? Team : -1;
	if (Player->GetTeamChoice() != Choice)
	{
		Player->SetTeamChoice(Choice);
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s picks %s"), *GetHumanName(Player),
			Choice >= 0 ? *FString::Printf(TEXT("the %s team"), *APenguinCharacter::GetTeamName(Choice).ToString()) : TEXT("no team"));
	}
}

void APenguinGameMode::AssignTeams(bool bMatch)
{
	using namespace PenguinGameModePrivate;
	APenguinGameState* PenguinState = GetPenguinState();
	TMap<const APenguinCharacter*, int32> TeamOf;
	bool bTeamMatch = false;
	LoneTeams = 0;
	if (bMatch)
	{
		const FPinguMatchSettings& MatchSettings = GetSettings();
		const int32 EvenTeams = MatchSettings.NumTeams >= 2 ? FMath::Min(MatchSettings.NumTeams, PinguMaxTeams) : 0;
		int32 Sizes[PinguMaxTeams] = {};
		const auto LowestEmpty = [&Sizes]()
		{
			for (int32 Team = 0; Team < PinguMaxTeams; ++Team)
			{
				if (Sizes[Team] == 0)
				{
					return Team;
				}
			}
			return PinguMaxTeams - 1; // (never: there are never more penguins than teams)
		};
		const auto SmallestEven = [&Sizes, EvenTeams]()
		{
			int32 Best = 0;
			for (int32 Team = 1; Team < EvenTeams; ++Team)
			{
				Best = Sizes[Team] < Sizes[Best] ? Team : Best;
			}
			return Best;
		};
		// The picks first: a person's card, a bot's setup.
		TArray<APenguinCharacter*> People;
		TArray<APenguinCharacter*> Bots;
		TArray<APenguinCharacter*> Undecided;
		for (APenguinCharacter* Penguin : Penguins)
		{
			if (!IsValid(Penguin))
			{
				continue;
			}
			const APenguinPlayerState* Player = Cast<APenguinPlayerState>(Penguin->GetPlayerState());
			(Player ? People : Bots).Add(Penguin);
			const FPinguBotSetup* Setup = Player ? nullptr : FindBotSetup(Penguin);
			const int32 Want = Player ? Player->GetTeamChoice() : (Setup ? Setup->Team : -1);
			if (Want >= 0 && Want < PinguMaxTeams)
			{
				TeamOf.Add(Penguin, Want);
				++Sizes[Want];
			}
		}
		// Then the rest, people before bots: evened out over the test's teams, or each on its own.
		for (APenguinCharacter* Penguin : People)
		{
			if (!TeamOf.Contains(Penguin))
			{
				Undecided.Add(Penguin);
			}
		}
		for (APenguinCharacter* Penguin : Bots)
		{
			if (!TeamOf.Contains(Penguin))
			{
				Undecided.Add(Penguin);
			}
		}
		for (APenguinCharacter* Penguin : Undecided)
		{
			const int32 Team = EvenTeams >= 2 ? SmallestEven() : LowestEmpty();
			TeamOf.Add(Penguin, Team);
			++Sizes[Team];
			if (EvenTeams < 2)
			{
				LoneTeams |= 1u << Team;  // on its own: a team of one that never shows as a team, and no late joiner gets into it
			}
		}
		// Everybody in one team: the last bot of it (else the last person) plays on its own - a match needs two sides.
		int32 Sides = 0;
		for (const int32 Size : Sizes)
		{
			Sides += Size > 0 ? 1 : 0;
		}
		if (Sides < 2 && People.Num() + Bots.Num() >= 2)
		{
			APenguinCharacter* Moved = Bots.Num() > 0 ? Bots.Last() : People.Last();
			const int32 From = TeamOf.FindRef(Moved);
			--Sizes[From];
			const int32 To = LowestEmpty();
			TeamOf.Add(Moved, To);
			++Sizes[To];
			LoneTeams |= 1u << To;
			UE_LOG(LogPingu, Display, TEXT("[GAME] teams: everybody picked the same team - %s plays on its own (team %d)"), *Moved->GetDisplayName(), To);
		}
		// A team match when two (or more) are in one team (or the test asked for teams): otherwise nobody has a team.
		for (const int32 Size : Sizes)
		{
			bTeamMatch |= Size >= 2;
		}
		bTeamMatch |= EvenTeams >= 2;
		if (bTeamMatch)
		{
			FString Line;
			for (int32 Team = 0; Team < PinguMaxTeams; ++Team)
			{
				if (Sizes[Team] == 0)
				{
					continue;
				}
				FString Members;
				for (const TPair<const APenguinCharacter*, int32>& Pair : TeamOf)
				{
					if (Pair.Value == Team)
					{
						Members += FString::Printf(TEXT("%s%s"), Members.IsEmpty() ? TEXT("") : TEXT(", "), *Pair.Key->GetDisplayName());
					}
				}
				if ((LoneTeams & (1u << Team)) != 0)
				{
					Line += FString::Printf(TEXT("%s%s on its own (team %d)"), Line.IsEmpty() ? TEXT("") : TEXT("; "), *Members, Team);
				}
				else
				{
					Line += FString::Printf(TEXT("%s%s %d (%s)"), Line.IsEmpty() ? TEXT("") : TEXT("; "), *APenguinCharacter::GetTeamName(Team).ToString(), Sizes[Team], *Members);
				}
			}
			UE_LOG(LogPingu, Display, TEXT("[GAME] teams: %s"), *Line);
		}
		else
		{
			TeamOf.Reset();
			LoneTeams = 0;
		}
	}
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			const int32* Team = TeamOf.Find(Penguin);
			Penguin->SetTeam(Team ? *Team : -1);
		}
	}
	if (PenguinState)
	{
		PenguinState->SetTeamMatch(bTeamMatch);
		for (FPinguScore& Row : PenguinState->GetMutableScores())
		{
			Row.Team = IsValid(Row.Penguin) ? Row.Penguin->GetTeam() : -1;
		}
		if (bTeamMatch)
		{
			PenguinState->GetMutableTeamWins().Init(0, PinguMaxTeams);
		}
		else
		{
			PenguinState->GetMutableTeamWins().Reset();
		}
	}
}

int32 APenguinGameMode::PickTeamFor(int32 Choice, const APenguinCharacter* Joining) const
{
	if (!IsTeamMatch())
	{
		return -1;
	}
	if (Choice >= 0 && Choice < PinguMaxTeams)
	{
		return Choice;
	}
	int32 Sizes[PinguMaxTeams] = {};
	for (const APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin) && Penguin != Joining && Penguin->GetTeam() >= 0 && Penguin->GetTeam() < PinguMaxTeams)
		{
			++Sizes[Penguin->GetTeam()];
		}
	}
	// The test's evened-out teams: the smallest. Otherwise on its own (a team nobody has).
	const int32 EvenTeams = GetSettings().NumTeams >= 2 ? FMath::Min(GetSettings().NumTeams, PinguMaxTeams) : 0;
	if (EvenTeams >= 2)
	{
		int32 Best = 0;
		for (int32 Team = 1; Team < EvenTeams; ++Team)
		{
			Best = Sizes[Team] < Sizes[Best] ? Team : Best;
		}
		return Best;
	}
	for (int32 Team = 0; Team < PinguMaxTeams; ++Team)
	{
		if (Sizes[Team] == 0)
		{
			return Team;
		}
	}
	return -1;
}

void APenguinGameMode::MoveLoneTeamAside(int32 Team, const APenguinCharacter* Joining)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || Team < 0 || Team >= PinguMaxTeams || (LoneTeams & (1u << Team)) == 0)
	{
		return;
	}
	LoneTeams &= ~(1u << Team);
	uint32 Used = 1u << Team;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin) && Penguin != Joining && Penguin->GetTeam() >= 0 && Penguin->GetTeam() < PinguMaxTeams)
		{
			Used |= 1u << Penguin->GetTeam();
		}
	}
	TArray<int32>& TeamWins = PenguinState->GetMutableTeamWins();
	if (TeamWins.Num() < PinguMaxTeams)
	{
		TeamWins.SetNumZeroed(PinguMaxTeams);
	}
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (!IsValid(Penguin) || Penguin == Joining || Penguin->GetTeam() != Team)
		{
			continue;
		}
		// (There is always a free team: never more penguins than teams.)
		int32 To = INDEX_NONE;
		for (int32 Free = 0; Free < PinguMaxTeams && To == INDEX_NONE; ++Free)
		{
			To = (Used & (1u << Free)) == 0 ? Free : INDEX_NONE;
		}
		if (To == INDEX_NONE)
		{
			break;
		}
		Used |= 1u << To;
		LoneTeams |= 1u << To;
		Penguin->SetTeam(To);
		TeamWins[To] = TeamWins[Team];
		if (FPinguScore* Row = PenguinState->GetMutableScores().FindByPredicate([Penguin](const FPinguScore& Entry) { return Entry.Penguin == Penguin; }))
		{
			Row->Team = To;
		}
		DecidedTeam = DecidedTeam == Team ? To : DecidedTeam;
		const int32 RoundTeam = PenguinState->GetRoundWinnerTeam();
		const int32 MatchTeam = PenguinState->GetMatchWinnerTeam();
		PenguinState->SetTeamWinners(RoundTeam == Team ? To : RoundTeam, MatchTeam == Team ? To : MatchTeam);
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s (on its own) moves from team %d to team %d: somebody joining picked the %s team"), *Penguin->GetDisplayName(),
			Team, To, *APenguinCharacter::GetTeamName(Team).ToString());
	}
	// The team the newcomer picked starts afresh (those round wins were the one on its own's).
	TeamWins[Team] = 0;
}

FString APenguinGameMode::DescribeSide(int32 Team) const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	if (Team < 0 || !PenguinState)
	{
		return FString(TEXT("nobody"));
	}
	if (PenguinState->CountTeamMembers(Team) == 1)
	{
		const FPinguScore* Row = PenguinState->GetScores().FindByPredicate([Team](const FPinguScore& Entry) { return Entry.Team == Team; });
		return Row ? Row->Name : FString(TEXT("nobody"));
	}
	return FString::Printf(TEXT("the %s team"), *APenguinCharacter::GetTeamName(Team).ToString());
}

APenguinCharacter* APenguinGameMode::PickBotToMakeRoom(int32 PreferTeam) const
{
	const APenguinGameState* PenguinState = GetPenguinState();
	// Team match: from the team with the most penguins (the newcomer then goes to the smallest one).
	TMap<int32, int32> TeamSizes;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin) && Penguin->GetTeam() >= 0)
		{
			++TeamSizes.FindOrAdd(Penguin->GetTeam());
		}
	}
	APenguinCharacter* Best = nullptr;
	int32 BestSize = -1;
	int32 BestWins = 0;
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (!IsValid(Penguin) || Penguin->GetPlayerState())
		{
			continue;  // people never leave for somebody else
		}
		const int32* TeamSize = TeamSizes.Find(Penguin->GetTeam());
		// (A bot of the team the newcomer chose goes first: the teams keep their sizes.)
		const int32 Size = (TeamSize ? *TeamSize : 0) + (PreferTeam >= 0 && Penguin->GetTeam() == PreferTeam ? 1000 : 0);
		const FPinguScore* Row = PenguinState ? PenguinState->FindScore(Penguin->GetPlayerSlot()) : nullptr;
		const int32 Wins = Row ? Row->Wins : 0;
		// The biggest team, then the fewest wins, then the highest slot.
		const bool bBetter = !Best || Size > BestSize || (Size == BestSize && (Wins < BestWins || (Wins == BestWins && Penguin->GetPlayerSlot() > Best->GetPlayerSlot())));
		if (bBetter)
		{
			Best = Penguin;
			BestSize = Size;
			BestWins = Wins;
		}
	}
	return Best;
}

void APenguinGameMode::AdmitWaitingPlayers()
{
	using namespace PenguinGameModePrivate;
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || !IsNetworkGame())
	{
		return;
	}
	for (APenguinPlayerState* Player : GetHumanPlayers())
	{
		APlayerController* PlayerController = Player->GetPlayerController();
		if ((!Player->IsWaitingForRound() && IsValid(Player->GetPenguin())) || !PlayerController || !PlayerCanRestart(PlayerController))
		{
			continue;  // playing already, or still loading the map (then the round after this one)
		}
		const FString Name = GetHumanName(Player);
		// Room for one more: at most MaxPenguins penguins - a bot makes way for the person.
		Penguins.RemoveAll([](const APenguinCharacter* Penguin) { return !IsValid(Penguin); });
		// The team they get: their pick, else (the test's even teams) the smallest, else one of their own (decided before a bot
		// leaves: a bot of their chosen team makes room, so the teams stay as even as they were).
		const int32 Choice = IsTeamMatch() && Player->GetTeamChoice() >= 0 && Player->GetTeamChoice() < PinguMaxTeams ? Player->GetTeamChoice() : -1;
		if (Penguins.Num() >= MaxPenguins)
		{
			APenguinCharacter* Leaving = PickBotToMakeRoom(Choice);
			if (!Leaving)
			{
				continue;
			}
			const FString BotName = Leaving->GetDisplayName();
			const FLinearColor BotColor = Leaving->GetPlayerColor();
			PenguinState->GetMutableScores().RemoveAll([Leaving](const FPinguScore& Row) { return Row.Penguin == Leaving; });
			DestroyBot(Leaving);
			// Its lobby card goes too.
			FPinguMatchSettings& MatchSettings = PenguinState->GetMutableSettings();
			MatchSettings.Bots.RemoveAll([&BotName](const FPinguBotSetup& Setup) { return Setup.Name == BotName; });
			MatchSettings.NumBots = MatchSettings.Bots.Num();
			AddFeed(FString::Printf(TEXT("%s makes room for %s."), *BotName, *Name), BotColor);
			UE_LOG(LogPingu, Display, TEXT("[GAME] %s leaves to make room for %s"), *BotName, *Name);
		}
		// A slot no penguin (and no other person) has: the colour and portrait nobody wears.
		uint32 UsedSlots = 0;
		for (const APenguinCharacter* Penguin : Penguins)
		{
			UsedSlots |= 1u << FMath::Clamp(Penguin->GetPlayerSlot(), 0, 31);
		}
		for (const APenguinPlayerState* Other : GetHumanPlayers())
		{
			if (Other != Player && Other->GetPenguinSlot() >= 0 && Other->GetPenguinSlot() < MaxPenguins)
			{
				UsedSlots |= 1u << Other->GetPenguinSlot();
			}
		}
		const int32 Slot = LowestFreeSlot(UsedSlots);
		if (Slot == INDEX_NONE)
		{
			continue;
		}
		Player->SetPenguinSlot(Slot);
		if (!PlayerController->GetPawn())
		{
			RestartPlayer(PlayerController);
		}
		APenguinCharacter* Penguin = Cast<APenguinCharacter>(PlayerController->GetPawn());
		if (!Penguin)
		{
			Player->SetPenguinSlot(-1);
			continue;  // no pawn (yet): the next round
		}
		Player->SetWaitingForRound(false);
		Penguin->SetPlayerSlot(Slot, Name);
		if (UPenguinBotComponent* Bot = Penguin->GetBot())
		{
			if (Bot->IsBotEnabled())
			{
				Bot->SetBotEnabled(false);
			}
		}
		// The team they picked may be the team of one of somebody who picked none: that one moves aside first (it never
		// gets a teammate). No pick (and no test teams): a team of their own - with no wins of whoever had it before.
		MoveLoneTeamAside(Choice, Penguin);
		const int32 JoinTeam = PickTeamFor(Choice, Penguin);
		if (JoinTeam >= 0 && Choice < 0 && GetSettings().NumTeams < 2)
		{
			LoneTeams |= 1u << JoinTeam;
			TArray<int32>& TeamWins = PenguinState->GetMutableTeamWins();
			if (TeamWins.IsValidIndex(JoinTeam))
			{
				TeamWins[JoinTeam] = 0;
			}
		}
		Penguin->SetTeam(JoinTeam);
		// In the line-up and on the scoreboard (everybody else keeps their scores).
		TArray<APenguinCharacter*> Sorted;
		for (APenguinCharacter* Existing : Penguins)
		{
			if (IsValid(Existing) && Existing != Penguin)
			{
				Sorted.Add(Existing);
			}
		}
		Sorted.Add(Penguin);
		SetSortedBySlot(Penguins, Sorted);
		TArray<FPinguScore>& Scores = PenguinState->GetMutableScores();
		FPinguScore& Row = Scores.AddDefaulted_GetRef();
		Row.Penguin = Penguin;
		Row.PlayerState = Player;
		Row.bHuman = true;
		Row.Name = Name;
		Row.Slot = Slot;
		Row.Team = Penguin->GetTeam();
		Scores.StableSort([](const FPinguScore& A, const FPinguScore& B) { return A.Slot < B.Slot; });
		AddFeed(FString::Printf(TEXT("%s joins the match!"), *Name), Penguin->GetPlayerColor());
		UE_LOG(LogPingu, Display, TEXT("[GAME] %s joins the match at round %d (slot %d%s)"), *Name, PenguinState->GetRoundNumber() + 1, Slot,
			Penguin->GetTeam() < 0 ? TEXT("") : (PenguinState->GetShownTeam(Penguin->GetTeam()) >= 0
				? *FString::Printf(TEXT(", %s team"), *APenguinCharacter::GetTeamName(Penguin->GetTeam()).ToString()) : TEXT(", on its own")));
	}
	UpdateAdvertisedInfo();
}

void APenguinGameMode::ApplyIslandSize(ABlockIsland* Island, int32 NumPenguins)
{
	using namespace PenguinGameModePrivate;
	if (Island)
	{
		// The next RegenerateArena makes it this big (the LAN clients get the size with the island).
		Island->ArenaPenguins = FMath::Clamp(NumPenguins, FewestIslandPenguins, MaxPenguins);
	}
}

int32 APenguinGameMode::CountLobbyPenguins() const
{
	using namespace PenguinGameModePrivate;
	// The bots wait on their cards in the lobby, but the island is made for the match they will play: people + bots.
	return FMath::Clamp(CountHumans() + GetSettings().NumBots, FewestIslandPenguins, MaxPenguins);
}

void APenguinGameMode::UpdateLobbyIslandSize(float Dt)
{
	using namespace PenguinGameModePrivate;
	const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	const int32 Want = CountLobbyPenguins();
	if (!Island || Island->GetArenaPenguins() == Want)
	{
		LobbyResizeWait = 0.f;
		return;
	}
	LobbyResizeWait += Dt;
	if (LobbyResizeWait < LobbyResizeDelay)
	{
		return;
	}
	LobbyResizeWait = 0.f;
	UE_LOG(LogPingu, Display, TEXT("[GAME] lobby: a new island for %d penguins (it was for %d)"), Want, Island->GetArenaPenguins());
	// Like a fresh lobby island (RefreshLobbyIsland): everybody back on it, as far apart as it allows.
	SetupRound(false);
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			Penguin->SetInputLocked(false);
		}
	}
}

void APenguinGameMode::ClearProjectiles()
{
	for (TActorIterator<APenguinProjectile> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}
	for (TActorIterator<AIcebergDrop> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}
	for (TActorIterator<AHookedIceBlock> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}
	APinguPickup::DestroyAll(GetWorld());
	APinguPortal::CloseAll(GetWorld());
	APinguTornado::DestroyAll(GetWorld());
	APinguGiantSnowball::DestroyAll(GetWorld());
	APinguSnowman::DestroyAll(GetWorld());
}

void APenguinGameMode::UpdateSkyDrops(float Dt)
{
	using namespace PenguinGameModePrivate;
	PickupTimer += Dt;
	if (PickupTimer < PickupInterval)
	{
		return;
	}
	PickupTimer = 0.f;
	if (APinguPickup::CountAll(GetWorld()) < MaxPickups)
	{
		APinguPickup::SpawnFromSky(GetWorld(), Rand);
	}
}

void APenguinGameMode::DropAbility(const APenguinCharacter* Victim, EPenguinKOReason Reason)
{
	const UPenguinAbilityComponent* Abilities = Victim ? Victim->GetAbilities() : nullptr;
	if (!Abilities)
	{
		return;
	}
	TArray<EPenguinAbility> Owned;
	for (const EPenguinAbility Ability : Abilities->GetAbilities())
	{
		if (Ability != EPenguinAbility::None)
		{
			Owned.Add(Ability);
		}
	}
	if (Owned.Num() == 0)
	{
		return;
	}
	// Knocked out over the sea (or into it): the bubble floats down over the island instead.
	const FVector Where = Victim->GetActorLocation();
	bool bOverSea = Reason == EPenguinKOReason::Sea;
	if (!bOverSea)
	{
		const ABlockIsland* Island = ABlockIsland::FindNear(GetWorld(), Where);
		float GroundZ = 0.f;
		bOverSea = !Island || !Island->FindGroundBelow(Where, 3000.f, GroundZ);
	}
	APinguPickup::SpawnDrop(GetWorld(), Owned[Rand.RandRange(0, Owned.Num() - 1)], Where, bOverSea);
}

void APenguinGameMode::ScheduleRespawn(APenguinCharacter* Penguin)
{
	if (!Penguin)
	{
		return;
	}
	const float Delay = Penguin->GetKOReason() == EPenguinKOReason::Bomb ? PenguinGameModePrivate::BombRespawnDelay : PenguinGameModePrivate::RespawnDelay;
	for (FPendingRespawn& Entry : PendingRespawns)
	{
		if (Entry.Penguin.Get() == Penguin)
		{
			Entry.TimeLeft = Delay;
			return;
		}
	}
	FPendingRespawn& Entry = PendingRespawns.AddDefaulted_GetRef();
	Entry.Penguin = Penguin;
	Entry.TimeLeft = Delay;
}

void APenguinGameMode::UpdateRespawns(float Dt)
{
	for (int32 Index = PendingRespawns.Num() - 1; Index >= 0; --Index)
	{
		PendingRespawns[Index].TimeLeft -= Dt;
		if (PendingRespawns[Index].TimeLeft > 0.f)
		{
			continue;
		}
		APenguinCharacter* Penguin = PendingRespawns[Index].Penguin.Get();
		PendingRespawns.RemoveAt(Index);
		if (IsValid(Penguin) && Penguin->IsKnockedOut())
		{
			Penguin->Respawn();
			Penguin->SetInputLocked(false);
		}
	}
}

FVector APenguinGameMode::GetStartLocation(int32 Slot) const
{
	if (const FVector* Assigned = RoundStartBySlot.Find(Slot))
	{
		return *Assigned;
	}
	// Not in the round's line-up (they arrived later): as far from everybody as the island allows.
	FVector Far;
	if (FindFarthestStart(nullptr, Far))
	{
		return Far;
	}
	const int32 Index = FMath::Max(Slot, 0);
	if (RoundStarts.Num() == 0)
	{
		return FVector(Index * 300.f - 450.f, 0.f, 400.f);
	}
	return RoundStarts[Index % RoundStarts.Num()] + FVector((Index / RoundStarts.Num()) * 60.f, 0.f, 0.f);
}

bool APenguinGameMode::FindFarthestStart(const APenguinCharacter* Placing, FVector& OutLocation) const
{
	using namespace PenguinGameModePrivate;
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!Island)
	{
		return false;
	}
	TArray<FVector> Others;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin) && Penguin != Placing && Penguin->IsAlive())
		{
			Others.Add(Penguin->GetActorLocation());
		}
	}
	if (Others.Num() == 0)
	{
		return false;
	}
	// Places spread over the whole island; the one whose nearest penguin is farthest away.
	float BestNearest = -1.f;
	for (const FVector& Candidate : Island->GetSpreadSpawnLocations(FarthestStartCandidates))
	{
		float Nearest = TNumericLimits<float>::Max();
		for (const FVector& Other : Others)
		{
			Nearest = FMath::Min(Nearest, static_cast<float>(FVector::Dist(Candidate, Other)));
		}
		if (Nearest > BestNearest)
		{
			BestNearest = Nearest;
			OutLocation = Candidate;
		}
	}
	return BestNearest >= 0.f;
}

void APenguinGameMode::LogRoundStarts(const ABlockIsland* Island)
{
	// Where everybody starts, and how far apart the closest two are against an even spread over the island's ground.
	TArray<float> Xs;
	for (const TPair<int32, FVector>& Pair : RoundStartBySlot)
	{
		Xs.Add(static_cast<float>(Pair.Value.X));
	}
	Xs.Sort();
	float Closest = 0.f;
	for (int32 Index = 1; Index < Xs.Num(); ++Index)
	{
		Closest = Index == 1 ? Xs[1] - Xs[0] : FMath::Min(Closest, Xs[Index] - Xs[Index - 1]);
	}
	const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
	const float IslandWidth = Bounds.IsValid ? static_cast<float>(Bounds.Max.X - Bounds.Min.X) : 0.f;
	FString Places;
	for (const float X : Xs)
	{
		Places += FString::Printf(TEXT(" %.0f"), X / 100.f);
	}
	UE_LOG(LogPingu, Display, TEXT("[GAME] round starts (m):%s - closest two %.1f m apart, island %.1f m wide"), *Places, Closest / 100.f, IslandWidth / 100.f);
	if (bRoundTest && Xs.Num() > 1 && IslandWidth > 0.f)
	{
		// Even from end to end (four blocks for the edges' margins, a block for the columns' width): what far apart means.
		const float Even = (IslandWidth - 500.f) / static_cast<float>(Xs.Num() - 1);
		UE_LOG(LogPingu, Display, TEXT("[ROUNDTEST] RESULT starts spread out: %s (%d penguins, the closest two %.1f m apart; an even spread over the island: %.1f m)"),
			Closest >= Even * 0.8f ? TEXT("ok") : TEXT("FAIL"), Xs.Num(), Closest / 100.f, Even / 100.f);
	}
}

// ---------------------------------------------------------------------------- flow

void APenguinGameMode::EnterMenu()
{
	if (IsNetworkGame())
	{
		// The title menu and its bots' demo are for solo games; a LAN game goes back to its lobby.
		EnterLobby();
		return;
	}
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	SetPaused(false);
	bLobbyIsland = false;
	bMatchTest = false;
	PenguinState->SetMatchTest(false);
	PenguinState->SetWinners(-1, -1);
	PenguinState->SetTeamWinners(-1, -1);
	PenguinState->SetRoundNumber(0);
	PenguinState->ClearFeed();
	EnsurePenguins(PenguinGameModePrivate::MenuDemoPenguins - CountHumans());
	AssignTeams(false);
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (UPenguinBotComponent* Bot = IsValid(Penguin) ? Penguin->GetBot() : nullptr)
		{
			Bot->SetBotEnabled(true, 1); // the demo: everybody plays by themselves
		}
	}
	SetupRound(true);
	SetPhase(EPinguPhase::Menu); // the demo skips the intro and the countdown
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			Penguin->SetInputLocked(false);
		}
	}
}

void APenguinGameMode::EnterLobby()
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	bMatchTest = false;
	PenguinState->SetMatchTest(false);
	PenguinState->SetPaused(false);
	PenguinState->SetWinners(-1, -1);
	PenguinState->SetTeamWinners(-1, -1);
	PenguinState->SetRoundNumber(0);
	PenguinState->ClearFeed();
	SetSlowMotion(1.f);
	SetPaused(false);
	// ABILITIES RANDOM / CHOOSE: the host's saved choice (2026-11-07; tests: RANDOM unless -PinguChoose).
	PenguinState->GetMutableSettings().bChooseAbilities = PinguPicks::LoadChooseSetting();
	// (The wooden card lobby since 2026-10-29: the island only with -PinguIslandLobby.)
	bLobbyIsland = PinguMenu3D::UseIslandLobby();
	CompactLobbySlots();
	if (bLobbyIsland)
	{
		// The lobby island (2026-10-23): the people (slots 0, 1, 2, ...) and the bots of the settings stand around on a small
		// island; the bots wander (APinguLobby), no brains, no abilities. Bots back from a match stay who they are.
		PenguinState->SetSettings(PrepareSettings(PenguinState->GetSettings()));
		bKeepBotLooks = true;
		EnsurePenguins(PenguinState->GetSettings().NumBots);
		bKeepBotLooks = false;
		AssignTeams(false);  // nothing counts in the lobby: the teams picked show on the hats (APinguLobby), for the next match
		ApplyBotBrains();
		DisableLobbyBrains();
	}
	else
	{
		// The people only, in slots 0, 1, 2, ... (the bots of the last match go - they wait on their cards; whoever waited for
		// a round gets a penguin). Solo: just you, on an island of your own.
		EnsurePenguins(0);
		AssignTeams(false);  // nothing counts in the lobby: no teams either (a card's team is for the next match)
		ApplyBotBrains();
		PenguinState->SetSettings(PrepareSettings(PenguinState->GetSettings()));
	}
	PenguinState->SetWaitingForContinue(false);
	SetupRound(false);
	SetPhase(EPinguPhase::Lobby);
	if (bLobbyIsland)
	{
		if (APinguLobby* Lobby = APinguLobby::FindOrSpawn(GetWorld()))
		{
			Lobby->OnLobbyStarted();
		}
	}
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			Penguin->SetInputLocked(false);
		}
	}
	UE_LOG(LogPingu, Display, TEXT("[GAME] lobby%s: %d players, %d bots"), IsNetworkGame() ? TEXT("") : TEXT(" (solo)"), CountHumans(),
		PenguinState->GetSettings().NumBots);
}

void APenguinGameMode::ReturnToLobby()
{
	EnterLobby();
}

void APenguinGameMode::RefreshLobbyIsland()
{
	SetupRound(false);
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			Penguin->SetInputLocked(false);
		}
	}
	AddFeed(TEXT("Fresh snow: a new island!"), FLinearColor::White);
}

void APenguinGameMode::SetMatchSettings(const FPinguMatchSettings& InSettings)
{
	if (APenguinGameState* PenguinState = GetPenguinState())
	{
		const FPinguMatchSettings Requested = InSettings; // may be the game state's own settings
		PenguinState->SetSettings(PrepareSettings(Requested));
		SyncLobbyBots(); // the lobby island: a bot added puffs in, a bot removed puffs out
	}
}

void APenguinGameMode::StartMatch(const FPinguMatchSettings& InSettings)
{
	using namespace PenguinGameModePrivate;
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	// A copy first: Rematch passes the game state's own settings.
	const FPinguMatchSettings Requested = InSettings;
	SetPaused(false);
	FPinguMatchSettings NewSettings = ClampSettings(Requested);
	const int32 Humans = CountHumans();
	if (IsNetworkGame())
	{
		// Never a match of one: alone with no bots, a bot joins.
		NewSettings.NumBots = FMath::Clamp(NewSettings.NumBots, FMath::Max(0, 2 - Humans), FMath::Max(0, MaxPenguins - Humans));
	}
	NewSettings = PrepareSettings(NewSettings);
	PenguinState->SetSettings(NewSettings);
	PenguinState->SetRoundNumber(0);
	PenguinState->SetWinners(-1, -1);
	PenguinState->SetTeamWinners(-1, -1);
	PenguinState->SetWaitingForContinue(false);
	PenguinState->ClearFeed();
	BombsExploded = 0;
	FightKnockOuts = 0;
	BombKnockOuts = 0;
	SelfBombKnockOuts = 0;
	bKeepBotLooks = bLobbyIsland && GetPhase() == EPinguPhase::Lobby;
	bLobbyIsland = false;
	EnsurePenguins(NewSettings.NumBots);
	bKeepBotLooks = false;
	ApplyBotBrains();
	// Teams once everybody is there (the test's even teams: never more than penguins).
	int32 NumPenguins = 0;
	for (const APenguinCharacter* Penguin : Penguins)
	{
		NumPenguins += IsValid(Penguin) ? 1 : 0;
	}
	NewSettings.NumTeams = NewSettings.NumTeams >= 2 && NumPenguins >= 2 ? FMath::Min(NewSettings.NumTeams, NumPenguins) : 0;
	PenguinState->SetSettings(NewSettings);
	AssignTeams(true);
	// Traits mode: a fresh bag of traits for the match (its own shuffle: the island and the abilities roll as ever).
	TraitBag.Reset();
	TraitRand.Initialize(static_cast<int32>(Rand.GetCurrentSeed() ^ 0x5EED7A17));
	const TCHAR* Who = IsNetworkGame() ? TEXT("LAN, ") : TEXT("");
	FString Bots;
	for (const FPinguBotSetup& Bot : NewSettings.Bots)
	{
		static const TCHAR* SkillNames[] = { TEXT("easy"), TEXT("normal"), TEXT("hard") };
		Bots += FString::Printf(TEXT("%s%s %s%s"), Bots.IsEmpty() ? TEXT(" (") : TEXT(", "), *Bot.Name, SkillNames[FMath::Clamp(Bot.Skill, 0, 2)],
			Bot.Team >= 0 ? *FString::Printf(TEXT(" %s"), *APenguinCharacter::GetTeamName(Bot.Team).ToString().ToLower()) : TEXT(""));
	}
	Bots += Bots.IsEmpty() ? TEXT("") : TEXT(")");
	const FString Teams = PenguinState->IsTeamMatchSetUp() ? TEXT(", team match") : FString();
	if (NewSettings.bEndless)
	{
		UE_LOG(LogPingu, Display, TEXT("[GAME] match start: %s%d players, %d bots%s, endless%s%s"), Who, Humans, NewSettings.NumBots, *Bots, *Teams,
			NewSettings.bTraits ? TEXT(", traits") : TEXT(""));
	}
	else
	{
		UE_LOG(LogPingu, Display, TEXT("[GAME] match start: %s%d players, %d bots%s, first to %d%s%s"), Who, Humans, NewSettings.NumBots, *Bots,
			NewSettings.RoundsToWin, *Teams, NewSettings.bTraits ? TEXT(", traits") : TEXT(""));
	}
	BeginRound();
}

void APenguinGameMode::StartPractice()
{
	if (IsNetworkGame())
	{
		UE_LOG(LogPingu, Warning, TEXT("[GAME] practice is for solo games"));
		return;
	}
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	SetPaused(false);
	bLobbyIsland = false;
	PenguinState->ClearFeed();
	PenguinState->SetRoundNumber(0);
	PenguinState->SetTeamWinners(-1, -1);
	PendingRespawns.Reset();
	EnsurePenguins(0);
	AssignTeams(false);
	PenguinState->SetRoundTrait(EPinguTrait::None, FVector2D::ZeroVector);
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (UPenguinTraitComponent* Traits = IsValid(Penguin) ? Penguin->GetTraits() : nullptr)
		{
			Traits->ClearTrait(); // practice has no traits
		}
	}
	if (APenguinCharacter* Human = GetHumanPenguin())
	{
		if (UPenguinBotComponent* Bot = Human->GetBot())
		{
			Bot->SetBotEnabled(false);
		}
	}
	if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
	{
		Island->Layout = EIslandLayout::MovementTest;
		Island->Rebuild();
		const TArray<FVector> Spawns = Island->GetSpawnLocations();
		if (APenguinCharacter* Human = GetHumanPenguin())
		{
			Human->ResetForRound(Spawns.Num() > 0 ? Spawns[0] : FVector(0.f, 0.f, 200.f));
			Human->SetInputLocked(false);
			// A random set of abilities to try out.
			if (UPenguinAbilityComponent* Abilities = Human->GetAbilities())
			{
				Abilities->SetAbilities(UPenguinAbilityComponent::RollAbilities(Rand, 3));
			}
		}
	}
	ClearProjectiles();
	if (APenguinCamera* Camera = APenguinCamera::FindOrSpawn(GetWorld()))
	{
		Camera->SnapToTargets();
	}
	SetPhase(EPinguPhase::Practice);
}

void APenguinGameMode::SetupRound(bool bNewRound)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	if (bNewRound)
	{
		PenguinState->SetRoundNumber(PenguinState->GetRoundNumber() + 1);
	}
	PenguinState->SetWinners(-1, PenguinState->GetMatchWinnerSlot());
	PenguinState->SetTeamWinners(-1, PenguinState->GetMatchWinnerTeam());
	PenguinState->SetCollapseStartTime(-1.f);
	bRoundEnding = false;
	bTimeUp = false;
	bCollapseStarted = false;
	RoundEndTimer = 0.f;
	DecidedWinner.Reset();
	DecidedTeam = -1;
	WinnerDecidedAt = -1.0;
	WinnerOutAt = -1.0;
	RoundTestStep = 0;
	RoundTestWant = -1;
	PickupTimer = 0.f;
	PendingRespawns.Reset();
	SetSlowMotion(1.f);
	ClearProjectiles();

	// Everybody starts as far from everybody else as the island allows (nobody gets jumped right away): one place each,
	// evenly from one end to the other - at least two, so somebody arriving later (the lobby) still finds a far one.
	TArray<APenguinCharacter*> Starters;
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			Starters.Add(Penguin);
		}
	}
	ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	TArray<FVector> Spawns;
	if (Island)
	{
		if (!bNewRound && bLobbyIsland)
		{
			Island->RegenerateLobby(Rand.RandRange(1, 999999)); // the lobby's small island, whoever is there
		}
		else
		{
			ApplyIslandSize(Island, bNewRound ? Starters.Num() : CountLobbyPenguins()); // (bNewRound false: the lobby)
			Island->RegenerateArena(Rand.RandRange(1, 999999));
		}
		Spawns = Island->GetSpreadSpawnLocations(FMath::Max(Starters.Num(), PenguinGameModePrivate::MinRoundStarts), true);
	}
	if (Spawns.Num() < Starters.Num())
	{
		Spawns = { FVector(-900.f, 0.f, 300.f), FVector(900.f, 0.f, 300.f), FVector(-300.f, 0.f, 300.f), FVector(300.f, 0.f, 300.f) };
	}
	RoundStarts = Spawns;
	// Who starts where: shuffled. A team match: each team's penguins on neighbouring places (the places in a row along the
	// island, the teams in a random order, teammates side by side in a random order).
	TArray<APenguinCharacter*> Order = Starters;
	for (int32 Index = Order.Num() - 1; Index > 0; --Index)
	{
		Order.Swap(Index, Rand.RandRange(0, Index));
	}
	const bool bTeams = Starters.ContainsByPredicate([](const APenguinCharacter* Penguin) { return Penguin->GetTeam() >= 0; });
	if (bTeams)
	{
		Spawns.Sort([](const FVector& A, const FVector& B) { return A.X < B.X; });
		TArray<int32> TeamOrder;
		for (const APenguinCharacter* Penguin : Order)
		{
			TeamOrder.AddUnique(Penguin->GetTeam()); // (Order is shuffled: so is this)
		}
		TArray<APenguinCharacter*> Grouped;
		for (const int32 Team : TeamOrder)
		{
			for (APenguinCharacter* Penguin : Order)
			{
				if (Penguin->GetTeam() == Team)
				{
					Grouped.Add(Penguin);
				}
			}
		}
		Order = MoveTemp(Grouped);
	}
	else
	{
		for (int32 Index = Spawns.Num() - 1; Index > 0; --Index)
		{
			Spawns.Swap(Index, Rand.RandRange(0, Index));
		}
	}
	RoundStartBySlot.Reset();
	for (int32 Index = 0; Index < Order.Num(); ++Index)
	{
		RoundStartBySlot.Add(Order[Index]->GetPlayerSlot(), Spawns[Index % Spawns.Num()]);
	}
	LogRoundStarts(Island);
	// Traits mode: everybody's trait for this round (BeginRound's only: the lobby, the menu demo and a fresh lobby island
	// have none). Its reveal starts once the round's banner has shown a moment.
	const bool bDeal = bDealTraits && PenguinState->AreTraitsOn();
	bDealTraits = false;
	// Abilities CHOOSE (2026-11-07, PinguAbilityPicks.h): a match's rounds deal the people's picks (BeginRound's only).
	const bool bPicksRound = bDealPicks;
	bDealPicks = false;
	// A LAN game: the round's abilities and traits come from a seed every player adds to (commit-reveal, PinguAntiCheat.h),
	// so every joiner can recompute the deal - the host's own penguin's too. Solo: the very same rolls as ever.
	FPinguFairDeal Fair;
	bool bFair = false;
	if (bNewRound && IsNetworkGame())
	{
		TArray<uint8> Slots;
		for (const APenguinCharacter* Penguin : Starters)
		{
			Slots.AddUnique(static_cast<uint8>(FMath::Clamp(Penguin->GetPlayerSlot(), 0, 255)));
		}
		bFair = APinguAntiCheat::HostDeal(GetWorld(), PenguinState->GetRoundNumber(), Slots, bDeal, ForcedTraits, PenguinState->GetSettings().TraitsOff,
			TraitBag, Fair, bPicksRound);
	}
	TArray<EPinguTrait> Dealt;
	if (bDeal && bFair)
	{
		for (const APenguinCharacter* Penguin : Starters)
		{
			const int32 FairIndex = Fair.IndexOf(Penguin->GetPlayerSlot());
			Dealt.Add(Fair.Traits.IsValidIndex(FairIndex) ? Fair.Traits[FairIndex] : EPinguTrait::None);
		}
	}
	else if (bDeal)
	{
		Dealt = DealTraits(Starters);
	}
	// The round's one trait (everybody's: it shows as one banner, and the round-wide traits read it on every machine). None when
	// there is no deal, or a test dealt different traits by slot.
	{
		EPinguTrait RoundTrait = Dealt.Num() > 0 ? Dealt[0] : EPinguTrait::None;
		for (const EPinguTrait Trait : Dealt)
		{
			RoundTrait = Trait == RoundTrait ? RoundTrait : EPinguTrait::None;
		}
		// Deadly Edges: the island's sides as they are at the round's start (blocks built or broken later don't move them).
		FVector2D Edges = FVector2D::ZeroVector;
		if (RoundTrait == EPinguTrait::DeadlyEdges && Island)
		{
			const FBox Bounds = Island->GetBounds();
			Edges = FVector2D(Bounds.Min.X - PinguTraitTuning::DeadlyEdgeMargin, Bounds.Max.X + PinguTraitTuning::DeadlyEdgeMargin);
		}
		PenguinState->SetRoundTrait(PinguTraits::IsReal(RoundTrait) ? RoundTrait : EPinguTrait::None, Edges);
	}
	const float RevealStart = static_cast<float>(PenguinState->GetFlowTime()) + APenguinGameState::TraitRevealDelay;
	// Same Abilities (solo / no fair deal): one roll of three for everybody (none rolled otherwise: the same rolls as ever).
	const TArray<EPenguinAbility> SameLoadout = PinguPicks::IsSameAbilitiesRound(Dealt) ? UPenguinAbilityComponent::RollAbilities(Rand, PinguPicks::NumSlots)
		: TArray<EPenguinAbility>();
	for (int32 Index = 0; Index < Starters.Num(); ++Index)
	{
		APenguinCharacter* Penguin = Starters[Index];
		Penguin->ResetForRound(GetStartLocation(Penguin->GetPlayerSlot()));
		Penguin->SetInputLocked(true);
		UPenguinTraitComponent* Traits = Penguin->GetTraits();
		if (Traits)
		{
			Traits->SetTrait(Dealt.IsValidIndex(Index) ? Dealt[Index] : EPinguTrait::None, RevealStart);
		}
		if (UPenguinAbilityComponent* Abilities = Penguin->GetAbilities())
		{
			// Three abilities (a trait may allow only two: Second Chance) - without traits the very same rolls as ever. The lobby
			// island has none (everybody just hangs around).
			const int32 FairIndex = bFair ? Fair.IndexOf(Penguin->GetPlayerSlot()) : INDEX_NONE;
			if (Fair.Loadouts.IsValidIndex(FairIndex))
			{
				Abilities->SetAbilities(Fair.Loadouts[FairIndex]);
			}
			else
			{
				// (Picks: a person's, CHOOSE on, a match round - PinguPicks::MakeLoadout rolls as RollLoadout without any.)
				const int32 NumAbilities = Traits ? Traits->GetNumAbilities() : 3;
				Abilities->SetAbilities(!bNewRound && bLobbyIsland ? TArray<EPenguinAbility>()
					: SameLoadout.Num() > 0 ? PinguPicks::FitShared(SameLoadout, NumAbilities)
					: PinguPicks::MakeLoadout(Rand, NumAbilities, bPicksRound ? PinguPicks::GetPicksFor(Penguin) : TArray<uint8>()));
			}
#if !UE_BUILD_SHIPPING
			if (bFair && PinguAntiCheat::IsHostPickCheat() && Penguin->IsLocallyControlled())
			{
				// -PinguCheat=picks (dev builds only): the anti-cheat test's cheating host picks its own abilities.
				Abilities->SetAbilities({ EPenguinAbility::SnowBomb, EPenguinAbility::IceGun, EPenguinAbility::Noot });
				UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] dev cheat: the host picked its own abilities"));
			}
#endif
		}
	}
	if (bDeal)
	{
		FString Deal;
		TArray<EPinguTrait> Different;
		const EPinguTrait RoundTrait = PenguinState->GetRoundTrait();
		for (int32 Index = 0; Index < Starters.Num() && Index < Dealt.Num(); ++Index)
		{
			const UPenguinTraitComponent* Traits = Starters[Index]->GetTraits();
			Deal += FString::Printf(TEXT("%s%s %s%s"), Deal.IsEmpty() ? TEXT("") : TEXT(", "), *Starters[Index]->GetDisplayName(), *PinguTraits::GetLogName(Dealt[Index]),
				Traits && Traits->GetNumAbilities() < 3 ? *FString::Printf(TEXT(" (%d abilities)"), Traits->GetNumAbilities()) : TEXT(""));
			Different.AddUnique(Dealt[Index]);
		}
		UE_LOG(LogPingu, Display, TEXT("[GAME] traits, round %d: %s (round trait %s)"), PenguinState->GetRoundNumber(), *Deal, *PinguTraits::GetLogName(RoundTrait));
		if (bMatchTest)
		{
			// Everybody has the round's one trait (2026-11-07, shared-traits), a real one, ticked in the host's list (unless a test
			// forced traits by slot).
			const bool bShared = ForcedTraits.Num() > 1 || (Different.Num() == 1 && PinguTraits::IsReal(RoundTrait)
				&& PenguinState->GetSettings().IsTraitAllowed(RoundTrait));
			UE_LOG(LogPingu, Display, TEXT("[TRAITTEST] round %d deal: %s"), PenguinState->GetRoundNumber(), *Deal);
			UE_LOG(LogPingu, Display, TEXT("[TRAITTEST] RESULT round %d: %d traits, %d different, round trait %s %s"), PenguinState->GetRoundNumber(), Dealt.Num(),
				Different.Num(), *PinguTraits::GetLogName(RoundTrait), bShared && Dealt.Num() == Starters.Num() ? TEXT("ok") : TEXT("FAIL"));
		}
	}
	// The round hazards (Rising Sea, Crumbling Island, Wind, Snowball Rain): the last round's goes, this round's trait may bring one.
	APinguRoundHazards::SetupForRound(GetWorld());
	if (APenguinCamera* Camera = APenguinCamera::FindOrSpawn(GetWorld()))
	{
		Camera->SnapToTargets();
	}
}

void APenguinGameMode::BeginRound()
{
	bDealTraits = true; // (Traits mode only)
	bDealPicks = true;  // (abilities CHOOSE only)
	SetupRound(true);
	SetPhase(EPinguPhase::Intro);
}

// ---------------------------------------------------------------------------- Traits mode

bool APenguinGameMode::SetChooseAbilities(bool bOn)
{
	// (The lobby only: a match keeps the setting it started with - the joiners' judge holds the deal to it.)
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || PenguinState->GetPhase() != EPinguPhase::Lobby)
	{
		return false;
	}
	PinguPicks::SaveChooseSetting(bOn);
	if (PenguinState->GetSettings().bChooseAbilities != bOn)
	{
		FPinguMatchSettings NewSettings = PenguinState->GetSettings();
		NewSettings.bChooseAbilities = bOn;
		SetMatchSettings(NewSettings);
		UE_LOG(LogPingu, Display, TEXT("[GAME] abilities %s"), bOn ? TEXT("CHOOSE") : TEXT("RANDOM"));
	}
	return true;
}

void APenguinGameMode::SetTraitsEnabled(bool bOn)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || PenguinState->AreTraitsOn() == bOn)
	{
		return;
	}
	FPinguMatchSettings NewSettings = PenguinState->GetSettings();
	NewSettings.bTraits = bOn;
	SetMatchSettings(NewSettings);
	UE_LOG(LogPingu, Display, TEXT("[GAME] traits %s"), bOn ? TEXT("on") : TEXT("off"));
}

void APenguinGameMode::SetTraitsOff(uint64 OffMask)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || GetPhase() != EPinguPhase::Lobby)
	{
		return;
	}
	// Only real traits' bits, and never all of them off (an empty list would deal every trait anyway: DrawShared).
	uint64 Clean = 0;
	int32 Ticked = 0;
	for (const EPinguTrait Trait : PinguTraits::GetAll())
	{
		const bool bOff = PinguTraits::IsOffIn(OffMask, Trait);
		Clean |= bOff ? (uint64(1) << static_cast<int32>(Trait)) : 0;
		Ticked += bOff ? 0 : 1;
	}
	if (Ticked == 0 || Clean == PenguinState->GetSettings().TraitsOff)
	{
		return;
	}
	FPinguMatchSettings NewSettings = PenguinState->GetSettings();
	NewSettings.TraitsOff = Clean;
	SetMatchSettings(NewSettings);
	UE_LOG(LogPingu, Display, TEXT("[GAME] traits list: %d of %d ticked"), Ticked, PinguNumTraits);
}

void APenguinGameMode::SetForcedTraits(const TArray<EPinguTrait>& BySlot)
{
	ForcedTraits = BySlot;
	if (ForcedTraits.Num() > 0)
	{
		FString Names;
		for (const EPinguTrait Trait : ForcedTraits)
		{
			Names += FString::Printf(TEXT("%s%s"), Names.IsEmpty() ? TEXT("") : TEXT(", "), *PinguTraits::GetLogName(Trait));
		}
		UE_LOG(LogPingu, Display, TEXT("[GAME] forced traits by slot: %s%s"), *Names, ForcedTraits.Num() == 1 ? TEXT(" (everybody)") : TEXT(""));
	}
}

TArray<EPinguTrait> APenguinGameMode::DealTraits(const TArray<APenguinCharacter*>& Starters)
{
	// Everybody the same (Linus 2026-11-07: "make it so traits are for all the same"): one trait for the round - a test's one
	// forced trait, else the next from the bag. PinguFair::Deal (the LAN deal every joiner checks) does exactly this too.
	const EPinguTrait Shared = ForcedTraits.Num() == 1 ? ForcedTraits[0] : DrawTrait();
	TArray<EPinguTrait> Dealt;
	Dealt.Init(Shared, Starters.Num());
	if (ForcedTraits.Num() > 1)
	{
		// A test's traits by slot go over it (the rest keep the round's trait).
		for (int32 Index = 0; Index < Starters.Num(); ++Index)
		{
			const int32 Slot = Starters[Index] ? Starters[Index]->GetPlayerSlot() : Index;
			if (ForcedTraits.IsValidIndex(Slot))
			{
				Dealt[Index] = ForcedTraits[Slot];
			}
		}
	}
	return Dealt;
}

EPinguTrait APenguinGameMode::DrawTrait()
{
	const APenguinGameState* PenguinState = GetPenguinState();
	return PinguTraits::DrawShared(TraitBag, TraitRand, PenguinState ? PenguinState->GetSettings().TraitsOff : 0);
}

void APenguinGameMode::CheckRoundEnd(const APenguinCharacter* LastVictim)
{
	const EPinguPhase Phase = GetPhase();
	if (bRoundEnding || (Phase != EPinguPhase::Fight && Phase != EPinguPhase::Menu))
	{
		return;
	}
	if (CountAliveSides() <= 1)
	{
		// The winner is whoever is still standing now (nobody: they went out together) - in a team match the team whose
		// penguins are left. It keeps the win if it falls into the sea afterwards; a draw only if it goes out at the same
		// moment (EndRound).
		const bool bTeams = IsTeamMatch();
		DecidedWinner = bTeams ? nullptr : GetLastSurvivor();
		DecidedTeam = bTeams ? GetAliveTeam() : -1;
		WinnerDecidedAt = GetWorld()->GetRealTimeSeconds();
		WinnerOutAt = -1.0;
		bRoundEnding = true;
		RoundEndGraceLeft = 3.f;
		// The winner must not fall into the sea during the victory pose.
		if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
		{
			Island->StopCollapse();
		}
		RoundEndTimer = Phase == EPinguPhase::Menu ? PenguinGameModePrivate::MenuRoundRestart : 0.75f;
		if (Phase == EPinguPhase::Fight)
		{
			// The final knock-out: the kill cam (PinguKillCam.h) plays its death animation in slow motion that ramps back to
			// normal speed as it ends, every machine zoomed in on it; the winner banner waits for it.
			if (APinguKillCam* KillCam = LastVictim ? APinguKillCam::Get(this) : nullptr)
			{
				RoundEndTimer = FMath::Max(RoundEndTimer, KillCam->OnFinalKnockOut(const_cast<APenguinCharacter*>(LastVictim)));
			}
		}
	}
}

void APenguinGameMode::EndRound()
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	using namespace PenguinGameModePrivate;
	APenguinCharacter* Winner = nullptr;
	if (!bTimeUp)
	{
		if (WinnerDecidedAt >= 0.0)
		{
			// The last one standing won, even if it fell into the sea after the others; a draw only when it went out at the
			// same moment as the one before it (or it left the game).
			Winner = DecidedWinner.Get();
			if (Winner && WinnerOutAt >= 0.0 && WinnerOutAt - WinnerDecidedAt < SimultaneousKOWindow)
			{
				Winner = nullptr;
			}
		}
		else
		{
			Winner = GetLastSurvivor();
		}
	}
	if (Winner && !IsValid(Winner))
	{
		Winner = nullptr;
	}
	// Team match: the team left standing wins (a draw like above: its last penguin went out at the same moment).
	const bool bTeams = IsTeamMatch();
	int32 RoundWinnerTeam = -1;
	if (bTeams)
	{
		Winner = nullptr;
		if (!bTimeUp)
		{
			if (WinnerDecidedAt >= 0.0)
			{
				RoundWinnerTeam = DecidedTeam;
				if (RoundWinnerTeam >= 0 && WinnerOutAt >= 0.0 && WinnerOutAt - WinnerDecidedAt < SimultaneousKOWindow)
				{
					RoundWinnerTeam = -1;
				}
				// Its last people left the game since: a draw (as for a winner who left).
				const bool bTeamStillHere = Penguins.ContainsByPredicate([RoundWinnerTeam](const APenguinCharacter* Penguin)
				{
					return IsValid(Penguin) && Penguin->GetTeam() == RoundWinnerTeam;
				});
				if (!bTeamStillHere)
				{
					RoundWinnerTeam = -1;
				}
			}
			else
			{
				RoundWinnerTeam = GetAliveTeam();
			}
		}
	}
	const int32 RoundWinnerSlot = Winner ? Winner->GetPlayerSlot() : -1;
	int32 MatchWinnerSlot = PenguinState->GetMatchWinnerSlot();
	int32 MatchWinnerTeam = PenguinState->GetMatchWinnerTeam();
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			Penguin->SetInputLocked(true);
		}
	}
	if (RoundWinnerTeam >= 0)
	{
		// Every winner still standing celebrates; the camera swoops to them.
		FVector Sum = FVector::ZeroVector;
		int32 Standing = 0;
		for (APenguinCharacter* Penguin : Penguins)
		{
			if (IsValid(Penguin) && Penguin->GetTeam() == RoundWinnerTeam && Penguin->IsAlive())
			{
				Penguin->SetVictory(true);
				Sum += Penguin->GetActorLocation();
				++Standing;
			}
		}
		if (Standing > 0)
		{
			PenguinState->FocusCamera(Sum / Standing, 2.4f, Standing > 1 ? 0.95f : 0.8f);
		}
		TArray<int32>& TeamWins = PenguinState->GetMutableTeamWins();
		if (TeamWins.Num() <= RoundWinnerTeam)
		{
			TeamWins.SetNumZeroed(RoundWinnerTeam + 1);
		}
		++TeamWins[RoundWinnerTeam];
		for (FPinguScore& Row : PenguinState->GetMutableScores())
		{
			Row.Wins += Row.Team == RoundWinnerTeam ? 1 : 0;
		}
		const FPinguMatchSettings& MatchSettings = PenguinState->GetSettings();
		if (!MatchSettings.bEndless && TeamWins[RoundWinnerTeam] >= MatchSettings.RoundsToWin)
		{
			MatchWinnerTeam = RoundWinnerTeam;
		}
	}
	if (Winner)
	{
		// (A winner that went out after the others has no victory pose: the camera stays where the round ended.)
		if (Winner->IsAlive())
		{
			Winner->SetVictory(true);
			PenguinState->FocusCamera(Winner->GetActorLocation(), 2.4f, 0.8f);
		}
		const int32 Row = PenguinState->FindScoreIndex(RoundWinnerSlot);
		if (Row != INDEX_NONE)
		{
			FPinguScore& WinnerScore = PenguinState->GetMutableScores()[Row];
			++WinnerScore.Wins;
			const FPinguMatchSettings& MatchSettings = PenguinState->GetSettings();
			if (!MatchSettings.bEndless && WinnerScore.Wins >= MatchSettings.RoundsToWin)
			{
				MatchWinnerSlot = RoundWinnerSlot;
			}
		}
	}
	// The fanfare (or the gong) plays from the game state's phase change, on every machine.
	PenguinState->SetWinners(RoundWinnerSlot, MatchWinnerSlot);
	PenguinState->SetTeamWinners(RoundWinnerTeam, MatchWinnerTeam);
	// (Still the fight phase here: its time and the knockback it grew to.)
	const FString WinnerText = bTeams ? (RoundWinnerTeam >= 0 ? DescribeSide(RoundWinnerTeam) : FString(TEXT("nobody (draw)")))
		: (Winner ? Winner->GetDisplayName() : FString(TEXT("nobody (draw)")));
	UE_LOG(LogPingu, Display, TEXT("[GAME] round %d over after %.1f s of fight (knockback +%.0f%%): winner %s"), PenguinState->GetRoundNumber(),
		PenguinState->GetPhase() == EPinguPhase::Fight ? PenguinState->GetPhaseTime() : 0.f, (PenguinState->GetKnockbackScale() - 1.f) * 100.f, *WinnerText);
	if (bTeams)
	{
		FString TeamBoard;
		for (const int32 Team : PenguinState->GetTeamBoard())
		{
			// (Somebody on their own by name, as the board shows them.)
			const int32 Members = PenguinState->CountTeamMembers(Team);
			TeamBoard += FString::Printf(TEXT("%s%s %d (%s)"), TeamBoard.IsEmpty() ? TEXT("") : TEXT(", "),
				Members == 1 ? *DescribeSide(Team) : *APenguinCharacter::GetTeamName(Team).ToString(), PenguinState->GetTeamWinCount(Team),
				Members == 1 ? TEXT("on its own") : *FString::Printf(TEXT("%d penguins"), Members));
		}
		UE_LOG(LogPingu, Display, TEXT("[GAME] team wins: %s"), *TeamBoard);
	}
	if (bMatchTest)
	{
		int32 Slides = 0;
		for (const APenguinCharacter* Penguin : Penguins)
		{
			const UPenguinMovementComponent* PenguinMove = IsValid(Penguin) ? Penguin->GetPenguinMovement() : nullptr;
			Slides += PenguinMove ? PenguinMove->SlideEvents : 0;
		}
		UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] belly slides so far: %d"), Slides);
		UPenguinBotComponent::LogSlideStats(*FString::Printf(TEXT("round %d"), PenguinState->GetRoundNumber()));
	}
	if (bRoundTest)
	{
		const bool bRight = RoundWinnerSlot == RoundTestWant;
		RoundTestRight += bRight ? 1 : 0;
		++RoundTestChecked;
		UE_LOG(LogPingu, Display, TEXT("[ROUNDTEST] round %d: winner slot %d (want %d), winner still standing %d: %s"),
			PenguinState->GetRoundNumber(), RoundWinnerSlot, RoundTestWant, (Winner && Winner->IsAlive()) ? 1 : 0, bRight ? TEXT("ok") : TEXT("WRONG"));
	}
	FString Board;
	int32 Rank = 0;
	const TArray<FPinguScore>& Scores = PenguinState->GetScores();
	for (const int32 Index : PenguinState->GetLeaderboard())
	{
		const FPinguScore& Score = Scores[Index];
		Board += FString::Printf(TEXT("%s%d. %s %d wins %d KOs %d falls %d hits"), Board.IsEmpty() ? TEXT("") : TEXT(", "), ++Rank, *Score.Name, Score.Wins,
			Score.KOs, Score.Falls, Score.Hits);
	}
	UE_LOG(LogPingu, Display, TEXT("[GAME] leaderboard: %s"), *Board);
	// Diagnostics in the log: what became of the player's jump presses this round.
	if (APenguinPlayerController* Controller = Cast<APenguinPlayerController>(GetWorld()->GetFirstPlayerController()))
	{
		Controller->LogJumpStats(FString::Printf(TEXT("round %d"), PenguinState->GetRoundNumber()));
	}
	// The leaderboard stays until somebody presses a key - unless the match is decided (the results come by themselves)
	// or a test runs.
	bContinueRequested = false;
	PenguinState->SetWaitingForContinue(MatchWinnerSlot < 0 && MatchWinnerTeam < 0 && !ContinuesByItself());
	SetPhase(EPinguPhase::RoundOver);
}

bool APenguinGameMode::ContinuesByItself() const
{
	if (bMatchTest || bRoundTest || bPerfTest || bNetTest)
	{
		return true;
	}
	// Nobody plays (only bots are left in the match): nobody would press a key.
	return !Penguins.ContainsByPredicate([](const APenguinCharacter* Penguin) { return IsValid(Penguin) && Penguin->GetPlayerState(); });
}

bool APenguinGameMode::KickPlayer(APenguinPlayerState* Player)
{
	APlayerController* Controller = Player ? Cast<APlayerController>(Player->GetOwningController()) : nullptr;
	UNetConnection* Connection = Controller ? Controller->GetNetConnection() : nullptr;
	// Only the host may (this is the game mode: clients have none), and only a joiner: not the host's own player.
	if (!Controller || Controller->IsLocalController() || !Connection || !IsNetworkGame())
	{
		return false;
	}
	// A plain kick: nothing is remembered (no ban list, no address, no install id), they can join again right away.
	UE_LOG(LogPingu, Display, TEXT("[GAME] the host kicks %s (they can join again)"), *GetHumanName(Player));
	// The joiner leaves by itself with the message; the engine's kick follows a moment later in case it does not.
	if (APenguinPlayerController* PenguinController = Cast<APenguinPlayerController>(Controller))
	{
		PenguinController->Rq22();
	}
	TWeakObjectPtr<APlayerController> WeakController = Controller;
	FTimerHandle KickTimer; // (one per kick; the timer manager keeps it)
	GetWorldTimerManager().SetTimer(KickTimer, FTimerDelegate::CreateWeakLambda(this, [this, WeakController]
	{
		if (APlayerController* Target = WeakController.Get(); Target && GameSession)
		{
			GameSession->KickPlayer(Target, FText::FromString(TEXT("The host kicked you.")));
		}
	}), 1.0f, false);
	return true;
}

void APenguinGameMode::RequestContinueRound(const APlayerController* Who)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || PenguinState->GetPhase() != EPinguPhase::RoundOver || !PenguinState->IsWaitingForContinue() || bContinueRequested)
	{
		return;
	}
	// (A little slack: a LAN client's clock may run a moment ahead of this one.)
	if (PenguinState->GetPhaseTime() < APenguinGameState::ContinueAfter - 0.3f)
	{
		return;
	}
	bContinueRequested = true;
	UE_LOG(LogPingu, Display, TEXT("[GAME] %s pressed a key after %.1f s of leaderboard: round %d next"), *GetHumanName(Who ? Who->PlayerState.Get() : nullptr),
		PenguinState->GetPhaseTime(), PenguinState->GetRoundNumber() + 1);
}

bool APenguinGameMode::EndMatchNow()
{
	using namespace PenguinGameModePrivate;
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState || !IsMatchPhase(PenguinState->GetPhase()) || PenguinState->GetPhase() == EPinguPhase::MatchOver)
	{
		return false;
	}
	// The winner: the most round wins, alone at the top (a tie, or nobody won a round yet: no winner).
	int32 WinnerSlot = -1;
	int32 WinnerTeam = -1;
	int32 BestWins = 0;
	bool bTie = false;
	if (IsTeamMatch())
	{
		for (const int32 Team : PenguinState->GetTeamBoard())
		{
			const int32 Wins = PenguinState->GetTeamWinCount(Team);
			if (Wins > BestWins)
			{
				BestWins = Wins;
				WinnerTeam = Team;
				bTie = false;
			}
			else if (Wins == BestWins && Wins > 0)
			{
				bTie = true;
			}
		}
		WinnerTeam = bTie ? -1 : WinnerTeam;
	}
	else
	{
		for (const FPinguScore& Score : PenguinState->GetScores())
		{
			if (Score.Wins > BestWins)
			{
				BestWins = Score.Wins;
				WinnerSlot = Score.Slot;
				bTie = false;
			}
			else if (Score.Wins == BestWins && Score.Wins > 0)
			{
				bTie = true;
			}
		}
		WinnerSlot = bTie ? -1 : WinnerSlot;
	}

	// The round stops where it is: nobody moves, nothing flies, normal time, no pause.
	SetPaused(false);
	SetSlowMotion(1.f);
	bRoundEnding = false;
	bContinueRequested = false;
	PendingRespawns.Reset();
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin))
		{
			Penguin->SetInputLocked(true);
		}
	}
	ClearProjectiles();
	// The crumbling of a sudden-death island must not go on under the results screen.
	if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
	{
		Island->StopCollapse();
	}
	// A round still being fought (or about to start) was never finished: the results count only the finished ones.
	if (PenguinState->GetPhase() != EPinguPhase::RoundOver)
	{
		PenguinState->SetRoundNumber(FMath::Max(PenguinState->GetRoundNumber() - 1, 0));
	}
	PenguinState->SetWaitingForContinue(false);
	PenguinState->SetWinners(-1, WinnerSlot);
	PenguinState->SetTeamWinners(-1, WinnerTeam);

	const FPinguScore* WinnerScore = PenguinState->FindScore(WinnerSlot);
	const FString WinnerName = WinnerTeam >= 0 ? DescribeSide(WinnerTeam) : (WinnerScore ? WinnerScore->Name : FString(TEXT("nobody (tie)")));
	UE_LOG(LogPingu, Display, TEXT("[GAME] match ended from the menu after %d rounds: %s wins (%d round wins)"), PenguinState->GetRoundNumber(), *WinnerName, BestWins);
	for (const FPinguScore& Score : PenguinState->GetScores())
	{
		const FString TeamText = Score.Team < 0 ? FString() : (PenguinState->GetShownTeam(Score.Team) >= 0
			? FString::Printf(TEXT(" (%s)"), *APenguinCharacter::GetTeamName(Score.Team).ToString()) : FString(TEXT(" (on its own)")));
		UE_LOG(LogPingu, Display, TEXT("[GAME]   %s%s: %d wins, %d KOs, %d falls, %d hits"), *Score.Name, *TeamText, Score.Wins, Score.KOs, Score.Falls, Score.Hits);
	}
	SetPhase(EPinguPhase::MatchOver);
	return true;
}

void APenguinGameMode::OnPenguinKnockedOut(APenguinCharacter* Victim, APenguinCharacter* By, EPenguinKOReason Reason)
{
	if (!Victim || !HasAuthority())
	{
		return;
	}
	if (bRoundEnding && WinnerOutAt < 0.0 && (Victim == DecidedWinner.Get() || (DecidedTeam >= 0 && Victim->GetTeam() == DecidedTeam && CountAliveInTeam(DecidedTeam) == 0)))
	{
		// The winner (the winning team's last penguin) went out after all: still the winner, unless it was at once.
		WinnerOutAt = GetWorld()->GetRealTimeSeconds();
	}
	const EPinguPhase Phase = GetPhase();
	const FString V = Victim->GetDisplayName();
	const FString B = By ? By->GetDisplayName() : FString();
	// Its own doing (its own snow bomb, By is nobody then): a self-knock-out - a fall for it, no knock-out for anyone.
	const bool bSelf = Victim->WasKOSelfInflicted();
	if (Phase == EPinguPhase::Fight)
	{
		++FightKnockOuts;
		BombKnockOuts += Reason == EPenguinKOReason::Bomb ? 1 : 0;
		SelfBombKnockOuts += Reason == EPenguinKOReason::Bomb && bSelf ? 1 : 0;
	}
	FString Text;
	switch (Reason)
	{
	case EPenguinKOReason::Sea: Text = By ? FString::Printf(TEXT("%s pushed %s into the sea!"), *B, *V) : FString::Printf(TEXT("%s fell into the sea!"), *V); break;
	case EPenguinKOReason::Icicle: Text = By ? FString::Printf(TEXT("%s iced %s!"), *B, *V) : FString::Printf(TEXT("%s got iced!"), *V); break;
	case EPenguinKOReason::Stomp: Text = By ? FString::Printf(TEXT("%s squashed %s!"), *B, *V) : FString::Printf(TEXT("%s got squashed!"), *V); break;
	case EPenguinKOReason::Iceberg: Text = By ? FString::Printf(TEXT("%s dropped an iceberg on %s!"), *B, *V) : FString::Printf(TEXT("%s got flattened!"), *V); break;
	case EPenguinKOReason::Crushed: Text = By ? FString::Printf(TEXT("%s buried %s under falling ice!"), *B, *V) : FString::Printf(TEXT("%s got squashed by falling ice!"), *V); break;
	case EPenguinKOReason::Bomb:
		Text = By ? FString::Printf(TEXT("%s blew up %s!"), *B, *V) : (bSelf ? FString::Printf(TEXT("%s blew themselves up!"), *V) : FString::Printf(TEXT("%s blew up!"), *V));
		break;
	case EPenguinKOReason::Shrunk: Text = By ? FString::Printf(TEXT("%s shrank %s into nothing!"), *B, *V) : FString::Printf(TEXT("%s shrank into nothing!"), *V); break;
	case EPenguinKOReason::Overgrown: Text = By ? FString::Printf(TEXT("%s grew %s until they popped!"), *B, *V) : FString::Printf(TEXT("%s grew too big and popped!"), *V); break;
	case EPenguinKOReason::Edge: Text = By ? FString::Printf(TEXT("%s knocked %s over the edge!"), *B, *V) : FString::Printf(TEXT("%s went over the edge!"), *V); break;
	default: Text = FString::Printf(TEXT("%s is out!"), *V); break;
	}
	if (Phase != EPinguPhase::Practice)
	{
		AddFeed(Text, By ? By->GetPlayerColor() : Victim->GetPlayerColor());
	}
	// Nothing counts in the lobby, nor a teammate's knock-out (friendly fire knocks out, but it is no KO for the statistics).
	APenguinGameState* PenguinState = GetPenguinState();
	const int32 KillerRow = (By && PenguinState && Phase != EPinguPhase::Lobby && Phase != EPinguPhase::MatchOver && !By->IsTeammate(Victim)) ? PenguinState->FindScoreIndex(By->GetPlayerSlot()) : INDEX_NONE;
	if (KillerRow != INDEX_NONE && PenguinState->GetScores()[KillerRow].Penguin == By)
	{
		++PenguinState->GetMutableScores()[KillerRow].KOs;
	}
	// The match statistics: a fall for the victim (in the fight, not after the round was decided).
	const bool bSameMoment = bRoundEnding && WinnerDecidedAt >= 0.0 && GetWorld()->GetRealTimeSeconds() - WinnerDecidedAt < PenguinGameModePrivate::SimultaneousKOWindow;
	const int32 VictimRow = PenguinState && Phase == EPinguPhase::Fight && (!bRoundEnding || bSameMoment) ? PenguinState->FindScoreIndex(Victim->GetPlayerSlot()) : INDEX_NONE;
	if (VictimRow != INDEX_NONE && PenguinState->GetScores()[VictimRow].Penguin == Victim)
	{
		++PenguinState->GetMutableScores()[VictimRow].Falls;
	}
	if (By && By != Victim && By->IsBot() && !By->IsTeammate(Victim))
	{
		By->TryTaunt(); // (not over a teammate it knocked out by mistake)
	}
	// Knocked-out penguins drop one of their abilities for the others to grab.
	if ((Phase == EPinguPhase::Fight || Phase == EPinguPhase::Menu) && !bRoundEnding)
	{
		DropAbility(Victim, Reason);
	}

	if (Phase == EPinguPhase::Practice || Phase == EPinguPhase::Lobby)
	{
		ScheduleRespawn(Victim);
		return;
	}
	CheckRoundEnd(Victim);
}

void APenguinGameMode::SetPaused(bool bPause)
{
	APenguinGameState* PenguinState = GetPenguinState();
	if (IsNetworkGame())
	{
		// The pause menu of a LAN game is a local overlay: the match goes on.
		if (PenguinState)
		{
			PenguinState->SetPaused(false);
		}
		return;
	}
	if (APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		const EPinguPhase Phase = GetPhase();
		const bool bAllowed = Phase != EPinguPhase::Menu && Phase != EPinguPhase::MatchOver;
		PlayerController->SetPause(bPause && bAllowed);
	}
	if (PenguinState)
	{
		PenguinState->SetPaused(IsGamePaused());
	}
}

bool APenguinGameMode::IsGamePaused() const
{
	return GetWorld() && GetWorld()->IsPaused();
}

bool APenguinGameMode::AllowPausing(APlayerController* PC)
{
	// Also keeps a client's "pause" console command from stopping the host's world.
	return !IsNetworkGame() && Super::AllowPausing(PC);
}

// ---------------------------------------------------------------------------- -PinguRoundTest

void APenguinGameMode::UpdateRoundTest(float PhaseTime)
{
	// One case per round, the penguins standing still (lowest slot first):
	//  1. three go into the sea one after another, then the last one too, a moment later: it still wins the round.
	//  2. two go into the sea, then the last two in one blast (the same frame): a draw.
	//  3. three go into the sea, the last one stays on the island: it wins.
	static constexpr float StepTimes[] = { 0.6f, 0.8f, 1.0f, 1.4f };
	if (RoundTestStep >= static_cast<int32>(UE_ARRAY_COUNT(StepTimes)) || PhaseTime < StepTimes[RoundTestStep])
	{
		return;
	}
	const APenguinGameState* PenguinState = GetPenguinState();
	const int32 Round = PenguinState ? PenguinState->GetRoundNumber() : 0;
	const int32 Step = RoundTestStep++;
	if (Round == 1 && Step == 0 && PenguinState)
	{
		// Knockback grows with the fight: +2 % a second, the same for everybody.
		UE_LOG(LogPingu, Display, TEXT("[ROUNDTEST] RESULT knockback growth: x%.3f after %.2f s of fight (want x%.3f)"), PenguinState->GetKnockbackScale(),
			PenguinState->GetPhaseTime(), 1.f + APenguinGameState::KnockbackGrowthPerSecond * PenguinState->GetPhaseTime());
	}
	TArray<APenguinCharacter*> Alive;
	for (APenguinCharacter* Penguin : Penguins)
	{
		if (IsValid(Penguin) && Penguin->IsAlive())
		{
			Alive.Add(Penguin);
		}
	}
	if (Alive.Num() == 0 || (Step == 3 && Round != 1))
	{
		return; // (only round 1's winner goes into the sea afterwards)
	}
	if (Round == 2 && Step == 2)
	{
		RoundTestWant = -1;
		UE_LOG(LogPingu, Display, TEXT("[ROUNDTEST] round %d: the last %d go out in one blast"), Round, Alive.Num());
		for (APenguinCharacter* Penguin : Alive)
		{
			Penguin->KnockOut(EPenguinKOReason::Bomb, nullptr);
		}
		return;
	}
	if (Alive.Num() == 2)
	{
		RoundTestWant = Alive[1]->GetPlayerSlot(); // the last one standing after this knock-out
	}
	UE_LOG(LogPingu, Display, TEXT("[ROUNDTEST] round %d: slot %d goes into the sea (%d still in)"), Round, Alive[0]->GetPlayerSlot(), Alive.Num() - 1);
	Alive[0]->KnockOut(EPenguinKOReason::Sea, nullptr);
}

// ---------------------------------------------------------------------------- tick

void APenguinGameMode::Tick(float DeltaSeconds)
{
	using namespace PenguinGameModePrivate;
	Super::Tick(DeltaSeconds);
	APenguinGameState* PenguinState = GetPenguinState();
	if (!PenguinState)
	{
		return;
	}
	// Real time: countdowns and banners keep their pace through slow motion.
	const float Dt = FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f);
	PenguinState->AdvanceSoloClock(Dt);
	const float PhaseTime = PenguinState->GetPhaseTime();
	const EPinguPhase Phase = PenguinState->GetPhase();
	const int32 RoundNumber = PenguinState->GetRoundNumber();
	if (!IsNetworkGame() && PenguinState->IsGamePaused() != IsGamePaused())
	{
		PenguinState->SetPaused(IsGamePaused()); // unpaused from somewhere else
	}

	if (SlowMotionLeft > 0.f)
	{
		SlowMotionLeft -= Dt;
		if (SlowMotionLeft <= 0.f)
		{
			SetSlowMotion(1.f);
		}
	}

	// Automated match test: screenshots every few seconds, and a safety time limit.
	if (bMatchTest)
	{
		MatchTestTime += Dt;
		MatchTestShotTimer -= Dt;
		if (MatchTestShotTimer <= 0.f && (Phase == EPinguPhase::Fight || Phase == EPinguPhase::Intro || Phase == EPinguPhase::RoundOver))
		{
			MatchTestShotTimer = Phase == EPinguPhase::Fight ? 2.5f : 1.5f;
			// (The team test and the ten-penguin test keep their own pictures.)
			const FString Folder = PenguinState->AreTraitsOn() ? TEXT("ClaudeLogs/Traits") : PenguinState->IsEndless() ? TEXT("ClaudeLogs/Endless")
				: PenguinState->GetSettings().NumTeams >= 2 ? TEXT("ClaudeLogs/Team")
				: Penguins.Num() > PenguinGameModePrivate::MenuDemoPenguins ? TEXT("ClaudeLogs/Big") : TEXT("ClaudeLogs/Match");
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / Folder / FString::Printf(TEXT("match_%02d.png"), MatchTestShots++));
			FScreenshotRequest::RequestScreenshot(Path, true, false);
			UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] screenshot %s phase %s round %d alive %d"), *Path, *UEnum::GetValueAsString(Phase), RoundNumber, CountAlive());
		}
		if (MatchTestTime > 420.f)
		{
			UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] RESULT timeout after %d rounds"), RoundNumber);
			PinguEndTestRun(this, TEXT("PinguMatchTest"));
			bMatchTest = false;
		}
	}
	if (bRoundTest)
	{
		RoundTestTime += Dt;
		const bool bDone = Phase == EPinguPhase::RoundOver && RoundNumber >= RoundTestRounds && PhaseTime >= 1.f;
		if (bDone || RoundTestTime > 150.f)
		{
			UE_LOG(LogPingu, Display, TEXT("[ROUNDTEST] RESULT %s%d of %d rounds right"), bDone ? TEXT("") : TEXT("timeout: "), RoundTestRight, RoundTestRounds);
			PinguEndTestRun(this, TEXT("PinguRoundTest"));
			bRoundTest = false;
		}
	}

	switch (Phase)
	{
	case EPinguPhase::Intro:
		// (Longer in Traits mode: the trait reveal.)
		if (PhaseTime >= PenguinState->GetIntroTime())
		{
			if (bMatchTest && PenguinState->AreTraitsOn())
			{
				const bool bIntroOk = PhaseTime >= APenguinGameState::TraitIntroTime - 0.1f && PhaseTime <= APenguinGameState::TraitIntroTime + 0.2f;
				UE_LOG(LogPingu, Display, TEXT("[TRAITTEST] RESULT round %d intro: %.2f s before the countdown (want %.1f), %.2f s before GO %s"), RoundNumber,
					PhaseTime, APenguinGameState::TraitIntroTime, PhaseTime + APenguinGameState::CountStep * 3.f, bIntroOk ? TEXT("ok") : TEXT("FAIL"));
			}
			SetPhase(EPinguPhase::Countdown); // the game state beeps 3, 2, 1 and GO on every machine
		}
		break;

	case EPinguPhase::Countdown:
		if (PhaseTime >= APenguinGameState::CountStep * 3.f)
		{
			SetPhase(EPinguPhase::Fight);
			for (APenguinCharacter* Penguin : Penguins)
			{
				if (IsValid(Penguin))
				{
					Penguin->SetInputLocked(false);
				}
			}
			if (APenguinPlayerController* Controller = Cast<APenguinPlayerController>(GetWorld()->GetFirstPlayerController()))
			{
				Controller->ResetJumpStats();
			}
		}
		break;

	case EPinguPhase::Fight:
		if (bRoundTest)
		{
			UpdateRoundTest(PhaseTime);
		}
		if (bRoundEnding)
		{
			RoundEndTimer -= Dt;
			if (RoundEndTimer <= 0.f)
			{
				// The last one standing (the winning team) is still falling toward the sea: give it a moment to make it back first.
				if (!bTimeUp && RoundEndGraceLeft > 0.f && IsWinnerFalling())
				{
					RoundEndGraceLeft -= Dt;
				}
				else
				{
					EndRound();
				}
			}
		}
		else if (PhaseTime > RoundTimeLimit)
		{
			// Nobody managed it: call it a draw and move on.
			bTimeUp = true;
			bRoundEnding = true;
			RoundEndTimer = 0.f;
			AddFeed(TEXT("Time's up!"), FLinearColor::White);
		}
		else if (!bCollapseStarted && PhaseTime > (bMatchTest ? FMath::Min(SuddenDeathAfter, 20.f) : SuddenDeathAfter))
		{
			// Sudden death: the island crumbles from the outside in.
			bCollapseStarted = true;
			if (ABlockIsland* Island = ABlockIsland::Get(GetWorld()))
			{
				Island->StartCollapse(0.8f, 1.2f);
				PenguinState->SetCollapseStartTime(static_cast<float>(PenguinState->GetFlowTime()));
				UE_LOG(LogPingu, Display, TEXT("[GAME] sudden death: the island crumbles (round %d)"), RoundNumber);
			}
		}
		if (!bRoundEnding)
		{
			UpdateSkyDrops(Dt);
		}
		break;

	case EPinguPhase::RoundOver:
		// The results come by themselves (and the tests' rounds); the next round waits for somebody's key.
		if (PenguinState->IsWaitingForContinue() ? (bContinueRequested && PhaseTime >= APenguinGameState::ContinueAfter - 0.3f) : PhaseTime >= RoundOverTime)
		{
			const int32 MatchWinnerSlot = PenguinState->GetMatchWinnerSlot();
			const int32 MatchWinnerTeam = PenguinState->GetMatchWinnerTeam();
			if (bMatchTest && PenguinState->IsEndless() && RoundNumber >= EndlessTestRounds)
			{
				// Endless never ends by itself: END MATCH (the pause menu's) ends it, the most round wins win.
				UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] RESULT endless: %d rounds played, match winner %d (want -1), still going"), RoundNumber, MatchWinnerSlot);
				int32 MostWins = 0;
				int32 AtMost = 0;
				for (const FPinguScore& Score : PenguinState->GetScores())
				{
					AtMost = Score.Wins > MostWins ? 1 : AtMost + (Score.Wins == MostWins ? 1 : 0);
					MostWins = FMath::Max(MostWins, Score.Wins);
				}
				const bool bEnded = EndMatchNow();
				LogBombStats();
				const FPinguScore* Winner = PenguinState->FindScore(PenguinState->GetMatchWinnerSlot());
				const bool bRightWinner = (MostWins > 0 && AtMost == 1) ? (Winner && Winner->Wins == MostWins) : PenguinState->GetMatchWinnerSlot() < 0;
				UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] RESULT end match: ended %d, phase %s, winner %s (%d wins, most %d shared by %d) %s"), bEnded ? 1 : 0,
					*UEnum::GetValueAsString(PenguinState->GetPhase()), Winner ? *Winner->Name : TEXT("none"), Winner ? Winner->Wins : 0, MostWins, AtMost,
					bEnded && PenguinState->GetPhase() == EPinguPhase::MatchOver && bRightWinner ? TEXT("ok") : TEXT("FAIL"));
				PinguEndTestRun(this, TEXT("PinguMatchTest"));
				bMatchTest = false;
			}
			else if (MatchWinnerSlot >= 0 || MatchWinnerTeam >= 0)
			{
				// The fanfare and the menu music come from the game state's phase change, on every machine.
				const FPinguScore* WinnerScore = PenguinState->FindScore(MatchWinnerSlot);
				const FString WinnerName = MatchWinnerTeam >= 0 ? DescribeSide(MatchWinnerTeam) : (WinnerScore ? WinnerScore->Name : FString(TEXT("?")));
				UE_LOG(LogPingu, Display, TEXT("[GAME] match over: %s wins"), *WinnerName);
				for (const FPinguScore& Score : PenguinState->GetScores())
				{
					const FString TeamText = Score.Team < 0 ? FString() : (PenguinState->GetShownTeam(Score.Team) >= 0
						? FString::Printf(TEXT(" (%s)"), *APenguinCharacter::GetTeamName(Score.Team).ToString()) : FString(TEXT(" (on its own)")));
					UE_LOG(LogPingu, Display, TEXT("[GAME]   %s%s: %d wins, %d KOs, %d falls, %d hits"), *Score.Name, *TeamText, Score.Wins, Score.KOs, Score.Falls, Score.Hits);
				}
				SetPhase(EPinguPhase::MatchOver);
				if (bMatchTest)
				{
					UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] RESULT match finished after %d rounds"), RoundNumber);
					LogBombStats();
					int32 InMatch = 0;
					for (const APenguinCharacter* Penguin : Penguins)
					{
						InMatch += IsValid(Penguin) ? 1 : 0;
					}
					UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] RESULT penguins %d, teams %d, match winner %s, teammates' hits (friendly fire) %d"), InMatch,
						PenguinState->GetSettings().NumTeams, *WinnerName, FriendlyHits);
					PinguEndTestRun(this, TEXT("PinguMatchTest"));
				}
			}
			else
			{
				PenguinState->SetWaitingForContinue(false);
				AdmitWaitingPlayers();  // LAN: whoever joined during the match plays from this round on
				BeginRound();
			}
		}
		break;

	case EPinguPhase::Menu:
		if (bMenuShot)
		{
			// -PinguMenuShot: the title screen as the player sees it, then quit.
			if (!bMenuShotTaken && PhaseTime > 3.f)
			{
				bMenuShotTaken = true;
				const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ClaudeLogs/Menu/menu.png"));
				FScreenshotRequest::RequestScreenshot(Path, true, false);
				UE_LOG(LogPingu, Display, TEXT("[MENUSHOT] screenshot %s"), *Path);
			}
			else if (bMenuShotTaken && PhaseTime > 4.5f)
			{
				bMenuShot = false;
				PinguEndTestRun(this, TEXT("PinguMenuShot"));
			}
		}
		if (bRoundEnding)
		{
			RoundEndTimer -= Dt;
			if (RoundEndTimer <= 0.f)
			{
				SetupRound(true);
				SetPhase(EPinguPhase::Menu);
				for (APenguinCharacter* Penguin : Penguins)
				{
					if (IsValid(Penguin))
					{
						Penguin->SetInputLocked(false);
					}
				}
			}
		}
		else if (PhaseTime > 60.f)
		{
			bRoundEnding = true; // a fresh island every minute
			RoundEndTimer = 0.f;
		}
		else
		{
			UpdateSkyDrops(Dt);
		}
		break;

	case EPinguPhase::Lobby:
	{
		UpdateRespawns(Dt);
		if (bLobbyIsland)
		{
			break; // the lobby island: no sky drops, always its own small size, nothing wears it down (no abilities)
		}
		UpdateSkyDrops(Dt);
		UpdateLobbyIslandSize(Dt); // somebody joined or left, a bot came or went: an island of that size
		// Bombs and crumbling ice wear the lobby island down while people wait: a fresh one before it is gone.
		const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
		const int32 Broken = Island ? Island->GetBrokenBlocks() : 0;
		const int32 Total = Island ? Island->GetNumBlocks() + Broken : 0;
		if (Total > 0 && Broken > LobbyWornOut * Total)
		{
			RefreshLobbyIsland();
		}
		break;
	}

	case EPinguPhase::Practice:
		UpdateRespawns(Dt);
		break;

	default:
		break;
	}
}

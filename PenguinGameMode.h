// Pingu - game flow: menu demo, matches of rounds on random islands, practice; LAN lobby and matches (server only).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "PenguinTypes.h"
#include "PenguinGameState.h"
#include "PinguTraitTable.h"
#include "PenguinGameMode.generated.h"

class ABlockIsland;
class APenguinBotController;
class APenguinCharacter;
class APenguinPlayerState;
class APlayerStart;

/**
 * The round flow. It exists only on the server (or in a solo game): every machine's UI reads APenguinGameState, which
 * this writes. Solo: menu demo with bots; PLAY opens a lobby of your own (you and the bots you add). LAN (listen server,
 * the host plays too): a lobby where everybody runs around, then matches of humans + bots, back to the lobby or a rematch.
 * In both lobbies everybody picks a team on their card (the host for the bots) - or none, and plays on their own.
 */
UCLASS()
class PENGUIN_API APenguinGameMode : public AGameModeBase
{
	GENERATED_BODY()

	// The autotest's trait_deal runs the real deal (DealTraits / DrawTrait, private).
	friend class APenguinAutoTest;

public:
	APenguinGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual void RestartPlayerAtPlayerStart(AController* NewPlayer, AActor* StartSpot) override;
	virtual void RestartPlayerAtTransform(AController* NewPlayer, const FTransform& SpawnTransform) override;
	/** Penguins never spawn turned (the editor's Play spawns at the viewport camera, rotation included). */
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform) override;

	// ------------------------------------------------------------------ LAN players
	/** Joining is possible while fewer than PinguMaxPenguins (10) people play - also during a match: the newcomer watches
	 *  until the next round, which brings a penguin for them (a bot makes room if the match is full). */
	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	/** The same check again: somebody else may have taken the last place while the new player was loading the map. */
	virtual APlayerController* Login(UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	/** A slot, a penguin and a scoreboard row for the new player (a penguin right away in the lobby; during a match they
	 *  wait for the next round: AdmitWaitingPlayers). */
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	/** Names are at most 20 characters; the scoreboard and the penguin follow a change. */
	virtual void ChangeName(AController* Controller, const FString& NewName, bool bNameChange) override;
	/** Solo games pause, LAN games never do (the pause menu is a local overlay there). */
	virtual bool AllowPausing(APlayerController* PC = nullptr) override;

	// ------------------------------------------------------------------ flow (called by the UI; LAN: the host's UI)
	void StartMatch(const FPinguMatchSettings& InSettings);
	void Rematch() { StartMatch(GetSettings()); }
	/** CONTRACT (v2): back from the results to the lobby (LAN: everybody stays connected; solo: your own lobby). */
	void ReturnToLobby();
	/** The lobby: LAN games start there; solo, PLAY opens it (a lobby only you are in, with your bots). */
	void EnterLobby();
	/** The leaderboard between rounds stays until somebody presses a key: Who did (any player - a LAN client's press comes
	 *  through its controller). The next round starts once the board has been up a moment (APenguinGameState::ContinueAfter). */
	void RequestContinueRound(const APlayerController* Who);

	/** The host's x on a joiner's card in the lobby: that joiner is sent away ("The host kicked you." on its screen); nothing
	 *  is remembered, they can join again at once (2026-11-07: no ban). False for the host's own player, bots and solo. */
	bool KickPlayer(APenguinPlayerState* Player);
	/** How many joiners PreLogin turned away because a match was running and the host's "Join running matches" is off (tests). */
	int32 GetRefusedRunningJoins() const { return RefusedRunningJoins; }
	/** END MATCH in the pause menu (solo player / LAN host), any match, endless too: the results now. The winner is whoever
	 *  has the most round wins (a team match: the team); a tie or no wins at all: no winner ("MATCH OVER"). False outside
	 *  a match. */
	bool EndMatchNow();
	/** CONTRACT (v2): the host / solo player changed the match settings (the bots and their skills and teams, rounds,
	 *  endless) in the lobby; they replicate to the clients' lobby screens through the game state. */
	void SetMatchSettings(const FPinguMatchSettings& InSettings);
	/** Host / solo: the lobby's TRAITS ON / OFF (the match settings: the next match deals traits). A LAN client's toggle
	 *  comes through its controller (APenguinPlayerController::RequestTraits): taken in the lobby only. */
	void SetTraitsEnabled(bool bOn);
	/** The host's TRAITS list (2026-11-07): the traits left out of the round bag (FPinguMatchSettings::TraitsOff). Taken in the lobby
	 *  only; at least one trait stays ticked. */
	void SetTraitsOff(uint64 OffMask);
	/** Host / solo, the lobby only: ABILITIES RANDOM / CHOOSE (saved; PinguAbilityPicks.h). False if refused. */
	bool SetChooseAbilities(bool bOn);
	/** Tests: the traits the deal gives by slot (one entry: everybody gets it; slots past the list are dealt from the bag
	 *  as usual). Empty: the bag for everybody. Kept until it is changed; used while Traits mode is on (-PinguTrait=
	 *  sets it and turns Traits mode on in the test matches). */
	void SetForcedTraits(const TArray<EPinguTrait>& BySlot);
	const TArray<EPinguTrait>& GetForcedTraits() const { return ForcedTraits; }
	/** Solo only. */
	void StartPractice();
	/** Solo: the title menu with the bots' demo. A LAN game goes back to the lobby instead (it never shows the demo). */
	void EnterMenu();
	/** Solo only: LAN games never pause. */
	void SetPaused(bool bPause);
	bool IsGamePaused() const;

	/** Every knock-out ends up here (sea, icicle, stomp, iceberg, bomb, crushed, shrunk, overgrown). Server only. */
	void OnPenguinKnockedOut(APenguinCharacter* Victim, APenguinCharacter* By, EPenguinKOReason Reason);
	/** A hit of By landed on Victim (APenguinCharacter::ApplyPenguinHit): the match statistics (a teammate's hit - friendly
	 *  fire - is only counted for the team test's log). Server only. */
	void OnPenguinHit(APenguinCharacter* By, APenguinCharacter* Victim);
	/** A snow bomb went off (APenguinProjectile::Explode). Server only; the bot matches count them. */
	void NoteBombExploded() { if (GetPhase() == EPinguPhase::Fight) { ++BombsExploded; } }
	/** The fights' knock-outs so far this match: all of them, those by a bomb, and those a penguin did to itself with its own
	 *  bomb (a self-knock-out: no kill for anyone) - the bot matches' log (LogBombStats) and the tests read them. */
	int32 GetFightKnockOuts() const { return FightKnockOuts; }
	int32 GetBombKnockOuts() const { return BombKnockOuts; }
	int32 GetSelfBombKnockOuts() const { return SelfBombKnockOuts; }
	int32 GetBombsExploded() const { return BombsExploded; }
	/** The team a player picked on their lobby card (-1: none, on their own). Used from the next match on. Server only. */
	void SetTeamChoice(APenguinPlayerState* Player, int32 Team);
	/** Practice and the LAN lobby: the Respawn key puts you back on the island. */
	bool AllowsManualRespawn() const;
	/** The place on the current island farthest from every other penguin still in (Placing aside): for a respawn or a
	 *  late arrival. False when nobody else is in. */
	bool FindFarthestStart(const APenguinCharacter* Placing, FVector& OutLocation) const;

	// ------------------------------------------------------------------ state (thin wrappers of APenguinGameState)
	EPinguPhase GetPhase() const;
	float GetPhaseTime() const;
	int32 GetRoundNumber() const;
	const TArray<FPinguScore>& GetScores() const;
	const TArray<FPinguFeedEntry>& GetFeed() const;
	int32 GetRoundWinnerSlot() const;
	int32 GetMatchWinnerSlot() const;
	const FPinguMatchSettings& GetSettings() const;
	/** Written in place (the solo menu cycles bots / difficulty / rounds); replicates through the game state. */
	FPinguMatchSettings& GetMutableSettings();
	/** Endless mode: the rounds never stop. */
	bool IsEndless() const;
	/** Indices into GetScores(), best first: most round wins, then most knock-outs, then slot order. */
	TArray<int32> GetLeaderboard() const;
	/** RoundOver: the winner banner shows first, the leaderboard after this many seconds. */
	static constexpr float LeaderboardDelay = APenguinGameState::LeaderboardDelay;
	/** 3, 2, 1 during the countdown, 0 for "GO!" (shown briefly), -1 otherwise. */
	int32 GetCountdownNumber() const;
	/** The penguin of this machine's player (the host's in a LAN game). */
	APenguinCharacter* GetHumanPenguin() const;
	/** Every penguin of the current round / lobby / demo, in slot order. */
	const TArray<TObjectPtr<APenguinCharacter>>& GetPenguins() const { return Penguins; }
	/** Automated match test (-PinguMatchTest): bots only, logs results, quits. */
	bool IsMatchTest() const { return bMatchTest; }
	/** A listen server (LAN game), not solo. */
	bool IsNetworkGame() const;
	/** The lobby is the lobby island (2026-10-23: the people and the bots on a small island, no abilities; not with
	 *  -PinguOldMenus) - true from EnterLobby until a match, the menu or practice starts. */
	bool IsLobbyIsland() const { return bLobbyIsland; }
	/** The round's end is decided (the last knock-out happened; the winner banner comes after the kill cam). */
	bool IsRoundEnding() const { return bRoundEnding; }
	/** In a fight: Penguin's knock-out would end the round (it is the last one standing of its side, against one other side). */
	bool IsFinalVictim(const APenguinCharacter* Penguin) const;

protected:
	/** Player names: the host's from the settings, a LAN player's from ?PinguName (the engine may swap ?Name for an
	 *  online nickname). */
	virtual FString InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal = TEXT("")) override;

private:
	struct FPendingRespawn
	{
		TWeakObjectPtr<APenguinCharacter> Penguin;
		float TimeLeft = 0.f;
	};

	APenguinGameState* GetPenguinState() const;
	void SetPhase(EPinguPhase NewPhase);
	/** Humans keep their penguins (and slots), NumBots bots join them (in the free slots, in order; the names of the
	 *  settings' bot setups). */
	void EnsurePenguins(int32 NumBots);
	/** bMatch: the teams of a match - everybody plays for the team picked on their card (a bot: its setup's); whoever has
	 *  none plays on their own (inside the game a team of their own: the lowest one nobody picked - LoneTeams; it never
	 *  shows as a team: no ring, no team name), or - tests, NumTeams >= 2 - is evened
	 *  out over that many teams. A team match when two penguins or more share a team (or NumTeams asked for one);
	 *  otherwise nobody has a team. All in one team: one of them (the last bot) plays on its own - a match needs two
	 *  sides. !bMatch (lobby, menu, practice): no teams. Also the score rows and the team wins (all 0). */
	void AssignTeams(bool bMatch);
	/** The team a penguin joining the match now goes to: the player's choice, else (NumTeams) the smallest team, else a
	 *  team of its own. -1 when it is not a team match. */
	int32 PickTeamFor(int32 Choice, const APenguinCharacter* Joining) const;
	/** A late joiner picked Team, and Team is somebody's team of their own (they picked none: LoneTeams): they move to a
	 *  team nobody has first (their round wins go with them), so whoever picked no team never gets a teammate. The team
	 *  the joiner picked starts with no wins. */
	void MoveLoneTeamAside(int32 Team, const APenguinCharacter* Joining);
	/** For the log and the feed: "the RED team", or - a team of one: somebody on their own - that penguin's name (as the
	 *  screens show it: APenguinGameState::GetShownTeam). */
	FString DescribeSide(int32 Team) const;
	/** The setup of a bot of the match (by its name), null for a person or a bot the settings don't know. */
	const FPinguBotSetup* FindBotSetup(const APenguinCharacter* Bot) const;
	/** Settings as the game mode keeps them: clamped (ClampSettings), and every bot named (each its own name). */
	FPinguMatchSettings PrepareSettings(const FPinguMatchSettings& InSettings);
	/** RoundOver: the next round comes by itself, nobody needs to press a key (the automated tests; nobody plays). */
	bool ContinuesByItself() const;
	/** LAN: players who joined while a match ran get their penguin now (a new round is about to start): a free slot, a
	 *  team, a score row - a bot makes room when the match is full. */
	void AdmitWaitingPlayers();
	/** The bot that leaves to make room for a joining player (PreferTeam's if it has one, else the largest team's; the
	 *  fewest wins; the highest slot). */
	APenguinCharacter* PickBotToMakeRoom(int32 PreferTeam) const;
	/** The island's size follows the penguins (a round's, or the lobby's people + bots): small for two, big for ten. */
	void ApplyIslandSize(ABlockIsland* Island, int32 NumPenguins);
	/** Lobby: the penguins the island is made for - the people and the bots on the cards (at least 2). */
	int32 CountLobbyPenguins() const;
	/** Lobby: somebody joined or left, or a bot came or went - an island of the new size a moment later (LobbyResizeDelay),
	 *  everybody back on it. */
	void UpdateLobbyIslandSize(float Dt);
	APenguinCharacter* SpawnBotPenguin(int32 Slot);
	void DestroyBot(APenguinCharacter* Bot);
	/** Bot brains: bots on (each at its setup's skill), humans off (solo tests: every penguin plays by itself). */
	void ApplyBotBrains();
	/** The lobby island: every bot brain off (APinguLobby walks the bots around). */
	void DisableLobbyBrains();
	/** The lobby island: the bots on the island follow the settings' bots (by name) - one that is no longer there puffs away,
	 *  a new one puffs in at a free place with a free colour. Nothing outside the lobby island. */
	void SyncLobbyBots();
	/** Island, starting places, abilities, everybody locked; bNewRound counts the round. BeginRound's deals the traits
	 *  (Traits mode: bDealTraits); every other one (lobby, menu demo, a fresh lobby island) takes them away. */
	void SetupRound(bool bNewRound);
	/** Traits mode: a trait for each of Starters (in that order). Everybody gets the same one (2026-11-07, shared-traits): a
	 *  test's one forced trait, else the next from the bag (DrawTrait); a test's traits forced by slot go over it. */
	TArray<EPinguTrait> DealTraits(const TArray<APenguinCharacter*>& Starters);
	/** The round's trait from the bag: every ticked trait (the host's list, FPinguMatchSettings::TraitsOff) once before any
	 *  comes again; the bag lasts the whole match (PinguTraits::DrawShared). */
	EPinguTrait DrawTrait();
	void BeginRound();
	void EndRound();
	void CheckRoundEnd(const APenguinCharacter* LastVictim);
	void ClearProjectiles();
	int32 CountAlive() const;
	/** Who is still in the round: penguins (everybody for themselves) or teams (a team match) with a penguin standing. */
	int32 CountAliveSides() const;
	/** Team match: the one team still standing (-1: none, or more than one). */
	int32 GetAliveTeam() const;
	int32 CountAliveInTeam(int32 Team) const;
	/** The round's decided winner (the last penguin, or the last team's penguins) is falling toward the sea, none of it
	 *  over the island: the round end waits a moment for a victory pose. */
	bool IsWinnerFalling() const;
	/** A team match is running (the settings' teams, a match phase). */
	bool IsTeamMatch() const;
	/** The one penguin still in the round (null unless exactly one is alive). */
	APenguinCharacter* GetLastSurvivor() const;
	/** On (or above) the island, not falling toward the sea. */
	bool IsOverGround(const APenguinCharacter* Penguin) const;
	void AddFeed(const FString& Text, const FLinearColor& Color);
	/** Every PickupInterval seconds of fighting a random ability floats down from the sky. */
	void UpdateSkyDrops(float Dt);
	/** A knocked-out penguin drops one of its abilities. */
	void DropAbility(const APenguinCharacter* Victim, EPenguinKOReason Reason);
	/** Practice and lobby: knocked-out penguins come back after a moment. */
	void ScheduleRespawn(APenguinCharacter* Penguin);
	void UpdateRespawns(float Dt);
	/** Round-end slow motion (solo only: a LAN game never changes the time). */
	void SetSlowMotion(float Dilation);
	/** -PinguRoundTest: the scripted knock-outs of this round (see the .cpp). */
	void UpdateRoundTest(float PhaseTime);

	// players
	/** Human players (not leaving), by slot. */
	TArray<APenguinPlayerState*> GetHumanPlayers(const APlayerState* Ignore = nullptr) const;
	int32 CountHumans(const APlayerState* Ignore = nullptr) const;
	/** The lowest slot no other human has. */
	int32 FindFreeHumanSlot(const APenguinPlayerState* For) const;
	/** Why a LAN player cannot join right now (empty: welcome). */
	FString GetJoinRefusal() const;
	/** What the scoreboard calls a human: "You" in solo games, the player name in LAN games. */
	FString GetHumanName(const APlayerState* Player) const;
	/** A LAN player arrived in the lobby: penguin placed, abilities to try, a row. */
	void AddLobbyPlayer(APlayerController* NewPlayer);
	/** A player left: penguin and row go; the round goes on without them. */
	void RemovePlayer(APenguinPlayerState* Leaving, APawn* LeavingPawn);
	/** The lobby closes gaps in the slots (0, 1, 2, ... in join order), so the next match has no holes. */
	void CompactLobbySlots();
	/** Rebuilds the score rows from the penguins (all scores 0). */
	void RebuildScores();
	/** Bots are clamped so humans + bots stay at most PinguMaxPenguins (solo 1..9 bots), with a setup each (the extra
	 *  ones go from the end; new ones get Difficulty and no team); NumTeams 0 or 2..PinguMaxTeams. */
	FPinguMatchSettings ClampSettings(const FPinguMatchSettings& InSettings) const;
	/** Host: tells the LAN beacon how many play and whether a match runs. */
	void UpdateAdvertisedInfo(const APlayerState* Leaving = nullptr) const;
	/** Where a penguin of a slot starts on the current island (the lobby). */
	FVector GetStartLocation(int32 Slot) const;
	/** The round's starting places in the log (the round test: a RESULT on how spread out they are). */
	void LogRoundStarts(const ABlockIsland* Island);
	/** Lobby: the island got blown to bits: a fresh one, everybody back on it. */
	void RefreshLobbyIsland();

	UPROPERTY(Transient)
	TArray<TObjectPtr<APlayerStart>> IslandStarts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<APenguinCharacter>> Penguins;

	UPROPERTY(Transient)
	TArray<TObjectPtr<APenguinBotController>> BotControllers;

	UPROPERTY(Transient)
	TSubclassOf<APenguinCharacter> PenguinClass;

	bool bRoundEnding = false;
	bool bTimeUp = false;
	/** Sudden death: the island crumbles from the edges once a round has lasted this long. */
	float SuddenDeathAfter = 30.f;
	bool bCollapseStarted = false;
	/** Seconds between the random abilities that float down from the sky. */
	float PickupInterval = 15.f;
	float PickupTimer = 0.f;
	float RoundEndTimer = 0.f;
	/** The last survivor is falling toward the sea when the round would end: wait up to this long to see if it makes it
	 *  back (for its victory pose - it has won either way, see DecidedWinner). */
	float RoundEndGraceLeft = 0.f;
	/** The round's winner, decided the moment only one penguin is left standing: it stays the winner if it falls into the
	 *  sea afterwards. A draw only when it goes out at the same moment as the one before it (same explosion, same splash). */
	TWeakObjectPtr<APenguinCharacter> DecidedWinner;
	/** Team match: the team that was left standing (-1: none / not a team match). Like DecidedWinner: it keeps the win if
	 *  its last penguin falls into the sea afterwards, a draw only when that happens at the same moment. */
	int32 DecidedTeam = -1;
	/** Team match: the teams that are somebody's team of their own (they picked no team; AssignTeams, a late joiner or a
	 *  bot that changes sides got one nobody had), a bit per team. Server only - the screens show anybody alone in a team
	 *  as on their own anyway (APenguinGameState::GetShownTeam); this keeps a late joiner out of such a team. */
	uint32 LoneTeams = 0;
	/** Real time when DecidedWinner was decided - the second-to-last knock-out - and when it went out itself (-1: still
	 *  in). Real time is what the players see: the slow motion after the decision stretches game time. */
	double WinnerDecidedAt = -1.0;
	double WinnerOutAt = -1.0;
	float SlowMotionLeft = 0.f;
	bool bMatchTest = false;
	/** -PinguPerfTest: every penguin plays by itself (APinguPerfTest measures the graphics presets meanwhile). */
	bool bPerfTest = false;
	/** -PinguMenuShot: a screenshot of the main menu, then quit. */
	bool bMenuShot = false;
	bool bMenuShotTaken = false;
	int32 MatchTestShots = 0;
	float MatchTestShotTimer = 0.f;
	float MatchTestTime = 0.f;
	/** -PinguRoundTest: scripted knock-outs check who wins a round (the last one out wins, a draw only for knock-outs at
	 *  the same moment), logs results, quits. */
	bool bRoundTest = false;
	int32 RoundTestStep = 0;
	/** The slot the current round must go to (-1: a draw), and how many rounds went right / were checked. */
	int32 RoundTestWant = -1;
	int32 RoundTestRight = 0;
	int32 RoundTestChecked = 0;
	float RoundTestTime = 0.f;
	/** StartPlay ran (players logging in before that are set up by it). */
	bool bFlowStarted = false;
	/** The world is going away (the host left): leaving players change nothing any more. */
	bool bShuttingDown = false;
	TArray<FPendingRespawn> PendingRespawns;
	/** The current island's starting places (as far apart as it allows) and which slot got which. */
	TArray<FVector> RoundStarts;
	TMap<int32, FVector> RoundStartBySlot;
	/** Only without a game state (never in practice): GetMutableSettings needs something to return. */
	FPinguMatchSettings FallbackSettings;
	FRandomStream Rand;
	int32 NextStart = 0;
	/** Lobby: how long the island has not been the size for the lobby's penguins (UpdateLobbyIslandSize). */
	float LobbyResizeWait = 0.f;
	/** IsLobbyIsland. */
	bool bLobbyIsland = false;
	/** EnsurePenguins keeps the bots it has as themselves (each its setup's name, its slot if free): from and into the lobby
	 *  island, so a bot keeps its colour. */
	bool bKeepBotLooks = false;
	/** Teammates' hits that landed so far (friendly fire: OnPenguinHit), for the team test's log. */
	int32 FriendlyHits = 0;
	/** This match's fights: bombs gone off, knock-outs in all / by a bomb / by the knocked-out penguin's own bomb. */
	int32 BombsExploded = 0;
	int32 FightKnockOuts = 0;
	int32 BombKnockOuts = 0;
	int32 SelfBombKnockOuts = 0;
	/** The bot test matches: how many bombs went off and how many penguins they knocked out, their own thrower among them. */
	void LogBombStats() const;
	/** RoundOver: somebody pressed a key for the next round (RequestContinueRound). */
	bool bContinueRequested = false;
	/** GetRefusedRunningJoins. */
	int32 RefusedRunningJoins = 0;
	/** -PinguNetTest: the LAN test drives the match (rounds go on by themselves). */
	bool bNetTest = false;

	// Traits mode
	/** BeginRound's SetupRound deals the traits (the next SetupRound only). */
	bool bDealTraits = false;
	/** BeginRound's SetupRound deals the people's ability picks (abilities CHOOSE; the next SetupRound only). */
	bool bDealPicks = false;
	/** The traits not dealt yet in this match's bag (StartMatch empties it), and its shuffle. */
	TArray<EPinguTrait> TraitBag;
	FRandomStream TraitRand;
	/** SetForcedTraits. */
	TArray<EPinguTrait> ForcedTraits;
};

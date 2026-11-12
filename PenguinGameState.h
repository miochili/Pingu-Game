// Pingu - the replicated match state: everything the UI shows, the same on the host and on every LAN client.
//
// CONTRACT (v2): the getters below are used by the UI (PinguUI / PenguinHUD), the player controller and the camera.
// Keep their names and meaning. APenguinGameMode (server only) is the only writer.
//
// Clients have no game mode, so the round flow's sounds, music and camera moments come from here on every machine:
// the server's setters play them locally, the clients' replication notifies play them there (each exactly once).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "PenguinTypes.h"
#include "PinguTraitTable.h"
#include "PenguinGameState.generated.h"

class APenguinCharacter;
class APenguinPlayerState;
class APlayerState;

UENUM(BlueprintType)
enum class EPinguPhase : uint8
{
	Menu,       // solo title screen, bots play a demo in the background (never in a LAN game)
	Lobby,      // LAN game: players gather on the island, the host picks the settings and starts
	Intro,      // "ROUND 2" banner, penguins placed
	Countdown,  // 3, 2, 1
	Fight,
	RoundOver,  // winner celebrates, then the leaderboard
	MatchOver,  // results screen
	Practice,   // free movement on the test course (solo only)
};

/** A bot of the lobby (its card): its name, how good it is and the team it plays for. */
USTRUCT(BlueprintType)
struct FPinguBotSetup
{
	GENERATED_BODY()

	/** Picked when the bot is added (every bot of a match has its own); empty: the game mode picks one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match")
	FString Name;

	/** 0 easy, 1 normal, 2 hard. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match", meta = (ClampMin = "0", ClampMax = "2"))
	int32 Skill = 1;

	/** The team it plays for: 0..PinguMaxTeams-1, or -1 for none (on its own). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match", meta = (ClampMin = "-1", ClampMax = "9"))
	int32 Team = -1;
};

USTRUCT(BlueprintType)
struct FPinguMatchSettings
{
	GENERATED_BODY()

	/** Bots added to the match (always Bots.Num() once the game mode has the settings). Solo: 1..9. LAN: 0..9, and humans
	 *  + bots never exceed PinguMaxPenguins (10) penguins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match", meta = (ClampMin = "0", ClampMax = "9"))
	int32 NumBots = 3;

	/** The skill of a bot that has no setup of its own (tests ask for bots by number): 0 easy, 1 normal, 2 hard. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match", meta = (ClampMin = "0", ClampMax = "2"))
	int32 Difficulty = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match", meta = (ClampMin = "1", ClampMax = "9"))
	int32 RoundsToWin = 3;

	/** Endless: rounds go on forever (nobody wins the match), the leaderboard keeps counting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match")
	bool bEndless = false;

	/** Traits mode (the lobby's TRAITS ON / OFF): every penguin gets a random trait each round (UPenguinTraitComponent),
	 *  revealed during a longer intro (APenguinGameState::GetIntroTime). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match")
	bool bTraits = false;

	/** Traits mode: the traits the host unticked in the lobby's TRAITS list (bit = 1 << EPinguTrait value; 0 = every trait in
	 *  the bag, the default). The round's one trait comes from the ticked ones (PinguTraits::DrawShared). */
	UPROPERTY()
	uint64 TraitsOff = 0;

	/** The trait is ticked in the host's list (in the bag). */
	bool IsTraitAllowed(EPinguTrait Trait) const { return !PinguTraits::IsOffIn(TraitsOff, Trait); }

	/** The lobby's ABILITIES: RANDOM (false, the default) / CHOOSE: every person picks their own (APenguinPlayerState::GetPicks;
	 *  PinguAbilityPicks.h). The host's, changed only in the lobby, saved (UPinguSettings::GetChooseAbilities). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match")
	bool bChooseAbilities = false;

	/** Tests only (the lobby has no such setting: everybody picks a team on their card): 2..PinguMaxTeams evens the
	 *  penguins without a team of their own out over this many teams (-PinguTeams=2, the LAN late-join test). 0: off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match", meta = (ClampMin = "0", ClampMax = "10"))
	int32 NumTeams = 0;

	/** The lobby's bots, in order (their slots follow the people's: the colours their cards show). A team match is one
	 *  where two penguins or more (people, bots) are in one team; whoever has no team plays on their own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Match")
	TArray<FPinguBotSetup> Bots;
};

/** One penguin of the match on the scoreboard / leaderboard. */
USTRUCT(BlueprintType)
struct FPinguScore
{
	GENERATED_BODY()

	/** The penguin (can be null for a moment on a client until the actor has replicated). */
	UPROPERTY()
	TObjectPtr<APenguinCharacter> Penguin = nullptr;

	/** The human player's state (null for bots). The UI marks "YOU" where this is the local player's state. */
	UPROPERTY()
	TObjectPtr<APlayerState> PlayerState = nullptr;

	/** Solo games call their player "You"; LAN games show the player names. */
	UPROPERTY()
	FString Name;

	/** Colour / portrait slot 0..9. Rows are sorted by slot, but look them up with FindScore: a player who leaves in
	 *  the middle of a match leaves a gap (the others keep their colours). */
	UPROPERTY()
	int32 Slot = 0;

	/** Rounds won (in a team match: rounds the penguin's team won). */
	UPROPERTY()
	int32 Wins = 0;

	/** Penguins it knocked out. */
	UPROPERTY()
	int32 KOs = 0;

	UPROPERTY()
	bool bHuman = false;

	/** Team match: 0..PinguMaxTeams-1; -1 everybody for themselves. */
	UPROPERTY()
	int32 Team = -1;

	/** Times it was knocked out itself (the match statistics). */
	UPROPERTY()
	int32 Falls = 0;

	/** Hits it landed on others (pushes, freezes, rays, knock-outs; blocked ones don't count). */
	UPROPERTY()
	int32 Hits = 0;
};

/** A kill-feed line. */
USTRUCT(BlueprintType)
struct FPinguFeedEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString Text;

	UPROPERTY()
	FLinearColor Color = FLinearColor::White;

	/** Flow time it happened (APenguinGameState::GetFlowTime: the server time in LAN games). Use GetFeedAge. */
	UPROPERTY()
	float Time = 0.f;
};

/** A short camera swoop of the round flow (the last knock-out, the winner), shown on every machine. */
USTRUCT()
struct FPinguCameraMoment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	float Duration = 0.f;

	UPROPERTY()
	float Zoom = 1.f;

	/** Flow time it happened: a moment that arrives late (or was over before this machine joined) is skipped. */
	UPROPERTY()
	float Time = -100.f;

	/** Counts up with every moment, so a new one replicates even at the same place. */
	UPROPERTY()
	int32 Serial = 0;
};

UCLASS()
class PENGUIN_API APenguinGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	APenguinGameState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PostNetInit() override;

	/** The game state of WorldContext's world (null before it exists). */
	static APenguinGameState* Get(const UObject* WorldContext);

	// ------------------------------------------------------------------ read on every machine
	EPinguPhase GetPhase() const { return Phase; }
	/** Seconds since the phase began, from the replicated server time (the same on every machine). Solo games count
	 *  real seconds instead, so banners and countdowns keep their pace through the round-end slow motion. */
	float GetPhaseTime() const;
	int32 GetRoundNumber() const { return RoundNumber; }
	const TArray<FPinguScore>& GetScores() const { return Scores; }
	/** The score row of a slot (null if nobody has that slot, or a player in it left). */
	const FPinguScore* FindScore(int32 Slot) const;
	/** Index into GetScores() of a slot's row, INDEX_NONE if there is none. */
	int32 FindScoreIndex(int32 Slot) const;
	/** Newest last, at most a handful of lines. */
	const TArray<FPinguFeedEntry>& GetFeed() const { return Feed; }
	/** Seconds since that feed line happened. */
	float GetFeedAge(const FPinguFeedEntry& Entry) const;
	int32 GetRoundWinnerSlot() const { return RoundWinnerSlot; }
	int32 GetMatchWinnerSlot() const { return MatchWinnerSlot; }
	const FPinguMatchSettings& GetSettings() const { return Settings; }
	bool IsEndless() const { return Settings.bEndless; }
	/** Indices into GetScores(), best first: most round wins, then most knock-outs, then fewest falls, then slot order (the
	 *  results screen, with every statistic). */
	TArray<int32> GetLeaderboard() const;
	/** Indices into GetScores() for the board between rounds, which shows only the wins: most wins, then slot order. */
	TArray<int32> GetWinsBoard() const;
	/** A team match is going on (two penguins or more of the match share a team, and a match runs: not in the lobby, the
	 *  menu or practice). Every penguin of a team match has a team - whoever chose none is a team of its own (shown as on its
	 *  own, never as a team: GetShownTeam). */
	bool IsTeamMatch() const;
	/** Team match: rounds each team won (index = team). */
	const TArray<int32>& GetTeamWins() const { return TeamWins; }
	int32 GetTeamWinCount(int32 Team) const { return TeamWins.IsValidIndex(Team) ? TeamWins[Team] : 0; }
	/** Team match: the teams that have penguins, best first (most rounds won, then the lower team). */
	TArray<int32> GetTeamBoard() const;
	/** Team match: score rows of a team's penguins, in slot order. */
	TArray<int32> GetTeamMembers(int32 Team) const;
	/** Team match: how many penguins of the match play for a team (its score rows). */
	int32 CountTeamMembers(int32 Team) const;
	/** The team a penguin of the match shows as - a ring in the team's colour, the team's name, a team's row on the boards:
	 *  its team in a team match when two penguins or more play for it, else -1. Whoever picked no team plays on its own: the
	 *  game mode gives it a team of one (its own side for the round's winner, nobody's teammate), which never shows as a
	 *  team. From the replicated score rows, so every machine decides it the same way. */
	int32 GetShownTeam(int32 Team) const;
	/** Team match: the team that won the round / the match (-1: none yet, a draw, or not a team match). */
	int32 GetRoundWinnerTeam() const { return RoundWinnerTeam; }
	int32 GetMatchWinnerTeam() const { return MatchWinnerTeam; }
	/** This machine's player's team in a team match (-1 otherwise, or while it waits for its first round). */
	int32 GetLocalTeam() const;
	/** RoundOver: the winner banner shows first, the leaderboard after this many seconds (1.8 since 2026-11-07, was 1.1: the
	 *  kill cam's view of the winner and the kill shows a second before the board covers it). */
	static constexpr float LeaderboardDelay = 1.8f;
	/** RoundOver: the leaderboard stays until somebody presses a key (any player) - from this many seconds on (presses
	 *  before that, still part of the fight, don't count). 2.5 since 2026-11-07 (was 1.8: the board comes 0.7 s later). */
	static constexpr float ContinueAfter = 2.5f;
	/** RoundOver: the next round waits for a key (false: it comes by itself - the match is decided, or a test runs). */
	bool IsWaitingForContinue() const { return bWaitingForContinue; }
	/** The "ROUND 2" banner shows this long before the countdown. */
	static constexpr float IntroTime = 1.2f;
	/** Traits mode: the intro is longer (7.3 s before GO instead of 3.6): the "ROUND 2" banner for TraitBannerTime, then
	 *  from TraitRevealDelay on the trait reveal (UPenguinTraitComponent: the cards for 3 s, their flight, the penguins
	 *  changing), then a moment to settle. */
	static constexpr float TraitIntroTime = 4.9f;
	static constexpr float TraitBannerTime = 0.9f;
	static constexpr float TraitRevealDelay = 0.9f;
	/** Seconds of the Intro phase before the countdown: IntroTime, or TraitIntroTime in Traits mode. */
	float GetIntroTime() const { return Settings.bTraits ? TraitIntroTime : IntroTime; }
	/** Traits mode is on (the match settings: the lobby's toggle). */
	bool AreTraitsOn() const { return Settings.bTraits; }
	/** Traits mode: the round's one trait, everybody's (2026-11-07; None outside a match round - lobby, menu demo, practice - and
	 *  when a test dealt different traits by slot). Replicated: the same on every machine. */
	EPinguTrait GetRoundTrait() const { return RoundTrait; }
	/** Deadly Edges: a penguin whose middle is left of X or right of Y is knocked out (the island's sides at the round's start
	 *  plus PinguTraitTuning::DeadlyEdgeMargin). Only while the round's trait is Deadly Edges; the UI draws the line there. */
	FVector2D GetDeadlyEdges() const { return DeadlyEdges; }
	bool HasDeadlyEdges() const { return RoundTrait == EPinguTrait::DeadlyEdges && DeadlyEdges.Y > DeadlyEdges.X; }
	/** Seconds per countdown number (3, 2, 1). */
	static constexpr float CountStep = 0.8f;
	/** "GO!" stays up this long into the fight. */
	static constexpr float GoTime = 0.75f;
	/** 3, 2, 1 during the countdown, 0 for "GO!" (shown briefly at the start of the fight), -1 otherwise. */
	int32 GetCountdownNumber() const;
	/** Automated match test (-PinguMatchTest). */
	bool IsMatchTest() const { return bMatchTest; }
	/** Solo games pause (the world stops). A LAN game never pauses: the pause menu is only a local menu there. */
	bool IsGamePaused() const { return bPaused; }
	/** Hosting or joined a LAN game (not solo). */
	bool IsNetworkGame() const;
	/** The human players' states by slot (the host first). */
	TArray<APenguinPlayerState*> GetPenguinPlayers() const;
	/** This machine's player (the first local player), null until its player state has replicated. */
	APenguinPlayerState* GetLocalPenguinPlayer() const;
	/** Sudden death: flow time (see GetFlowTime) the island started crumbling, or -1. */
	float GetCollapseStartTime() const { return CollapseStartTime; }
	/** Practice and the LAN lobby: the player may put the penguin back on the island (the Respawn key). */
	bool AllowsManualRespawn() const { return Phase == EPinguPhase::Practice || Phase == EPinguPhase::Lobby; }
	/** The clock of the round flow (phase times, the feed, sudden death): the server world time in LAN games, real
	 *  seconds that stop while paused in solo games. */
	double GetFlowTime() const;
	/**
	 * Every knockback of the round is multiplied by this (APenguinCharacter::ApplyPenguinHit): 1 + 0.02 per second since
	 * the fight began (+60 % after 30 s, +120 % after a minute), at most MaxKnockbackScale. 1 outside the Fight phase
	 * (menu demo, lobby, practice, between rounds). From the replicated phase and phase time: the same on every machine.
	 */
	float GetKnockbackScale() const;
	/** GetKnockbackScale's rule after FightTime seconds of fight with the round trait Trait (Pinball, Fast Growth change it). */
	static float GetKnockbackScaleAt(EPinguTrait Trait, float FightTime);
	/** GetKnockbackScale grows by this much per second of fight... */
	static constexpr float KnockbackGrowthPerSecond = 0.02f;
	/** ...up to this (reached after 150 s). */
	static constexpr float MaxKnockbackScale = 4.f;

	// ------------------------------------------------------------------ written by APenguinGameMode (server only)
	void SetPhase(EPinguPhase NewPhase);
	void SetRoundNumber(int32 InRound) { RoundNumber = InRound; }
	TArray<FPinguScore>& GetMutableScores() { return Scores; }
	void AddFeed(const FString& Text, const FLinearColor& Color);
	void ClearFeed() { Feed.Reset(); }
	void SetWinners(int32 InRoundWinnerSlot, int32 InMatchWinnerSlot);
	/** Team match: the round's / match's winning team (-1 none). */
	void SetTeamWinners(int32 InRoundWinnerTeam, int32 InMatchWinnerTeam);
	/** Team match: the rounds won per team (sized to the number of teams at the start of the match). */
	TArray<int32>& GetMutableTeamWins() { return TeamWins; }
	void SetSettings(const FPinguMatchSettings& InSettings) { Settings = InSettings; }
	/** The settings in place (the solo menu cycles bots / difficulty / rounds through this); they replicate. */
	FPinguMatchSettings& GetMutableSettings() { return Settings; }
	void SetMatchTest(bool bInMatchTest) { bMatchTest = bInMatchTest; }
	/** A team match starts (true) or ends (false); see IsTeamMatch. */
	void SetTeamMatch(bool bInTeamMatch) { bTeamMatch = bInTeamMatch; }
	/** The teams of the match are set up (whatever the phase: the first round is set up while the lobby still shows). */
	bool IsTeamMatchSetUp() const { return bTeamMatch; }
	void SetWaitingForContinue(bool bInWaiting) { bWaitingForContinue = bInWaiting; }
	void SetPaused(bool bInPaused) { bPaused = bInPaused; }
	void SetCollapseStartTime(float ServerTime) { CollapseStartTime = ServerTime; }
	void SetRoundTrait(EPinguTrait InTrait, const FVector2D& InDeadlyEdges) { RoundTrait = InTrait; DeadlyEdges = InDeadlyEdges; }
	/** A camera swoop toward Location (APenguinCamera::FocusOn) here and on every client. */
	void FocusCamera(const FVector& Location, float Duration, float ZoomScale);
	/** Solo: the game mode's tick moves the flow clock on by real seconds (LAN games use the server time instead). */
	void AdvanceSoloClock(float RealSeconds);

protected:
	/** LAN client: the engine averages the server clock over up to 250 samples, so after a frame longer than 0.4 s on
	 *  either machine (MaxUndilatedFrameTime: a shader compile, dragging the window) it took 10-25 s to catch up - frost
	 *  breath pushed for over a second (or not at all), ice shells hid early, cooldowns read wrong. Two samples in a row
	 *  more than 0.3 s off: start the average over from the newest one. (No UFUNCTION: the engine's is the rep notify.) */
	virtual void OnRep_ReplicatedWorldTimeSecondsDouble() override;

	UFUNCTION()
	void OnRep_Phase();

	UFUNCTION()
	void OnRep_CameraMoment();

	UPROPERTY(Replicated)
	EPinguPhase Phase = EPinguPhase::Menu;

	/** Flow time the phase began (GetFlowTime: the server world time in LAN games). */
	UPROPERTY(Replicated)
	float PhaseStartTime = 0.f;

	/** Counts every SetPhase (also a phase that starts again, like a rematch), so every machine sees each one. */
	UPROPERTY(ReplicatedUsing = OnRep_Phase)
	int32 PhaseSerial = 0;

	UPROPERTY(Replicated)
	int32 RoundNumber = 0;

	UPROPERTY(Replicated)
	TArray<FPinguScore> Scores;

	UPROPERTY(Replicated)
	TArray<FPinguFeedEntry> Feed;

	UPROPERTY(Replicated)
	int32 RoundWinnerSlot = -1;

	UPROPERTY(Replicated)
	int32 MatchWinnerSlot = -1;

	UPROPERTY(Replicated)
	TArray<int32> TeamWins;

	UPROPERTY(Replicated)
	int32 RoundWinnerTeam = -1;

	UPROPERTY(Replicated)
	int32 MatchWinnerTeam = -1;

	UPROPERTY(Replicated)
	FPinguMatchSettings Settings;

	UPROPERTY(Replicated)
	bool bMatchTest = false;

	UPROPERTY(Replicated)
	bool bTeamMatch = false;

	UPROPERTY(Replicated)
	bool bWaitingForContinue = false;

	UPROPERTY(Replicated)
	bool bPaused = false;

	UPROPERTY(Replicated)
	float CollapseStartTime = -1.f;

	UPROPERTY(Replicated)
	EPinguTrait RoundTrait = EPinguTrait::None;

	UPROPERTY(Replicated)
	FVector2D DeadlyEdges = FVector2D::ZeroVector;

	UPROPERTY(ReplicatedUsing = OnRep_CameraMoment)
	FPinguCameraMoment CameraMoment;

private:
	/** Server clock samples in a row that were off (see OnRep_ReplicatedWorldTimeSecondsDouble). */
	int32 ClockOffSamples = 0;
	/** Music, sounds and the camera for a phase that just began. bQuiet: only the state (music, camera framing), no
	 *  one-shot sounds (a client joining in the middle of something). */
	void HandlePhaseChanged(bool bQuiet);
	/** The countdown beeps and "GO!" whenever the countdown number changes (on every machine). */
	void UpdateCountdownCue(bool bQuiet);
	void ApplyCameraMoment() const;
	/** Server: feed lines older than a few seconds go. */
	void PruneFeed();

	/** Solo games: real seconds of the flow (advanced by the game mode's tick, so it stops while paused). */
	double SoloClock = 0.0;
	/** The PhaseSerial whose music / sounds were played on this machine. */
	int32 HandledPhaseSerial = -1;
	/** The countdown number last beeped on this machine. */
	int32 CuedCountdownNumber = -1;
	/** Client: the first replication (a snapshot of whatever was going on before we joined) is over. */
	bool bInitialReplicationDone = false;
};

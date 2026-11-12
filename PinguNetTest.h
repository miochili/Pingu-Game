// Pingu - automated LAN test: two -game processes on one PC (-PinguNetTest=host / -PinguNetTest=client
// -PinguNetAddr=127.0.0.1). The host hosts, the client joins, both play a short match with their bot brains driving
// their own penguins, and both log [NETTEST] lines and RESULT summaries, then quit.
//
// CONTRACT (v2): UPinguNetSubsystem spawns one in every game world that begins play when the command line has
// -PinguNetTest (a client's world has no game mode, so the game mode cannot do it; the test continues after hosting /
// joining reloads the map). Written by net_session.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguNetTest.generated.h"

class APenguinCharacter;
class APenguinGameState;

UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguNetTest : public AActor
{
	GENERATED_BODY()

public:
	APinguNetTest();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** -PinguNetTest, -PinguNetTest=host or -PinguNetTest=client is on the command line. */
	static bool IsRequested();

private:
	/** What happens next depends on the world we are in: solo (host / join), listen server or client. */
	void TickSolo();
	void TickHost();
	void TickClient();
	/** Logs what both machines should see the same way: phases, penguins, the feed, scores, island blocks, our penguin. */
	void Observe();
	void OnPhase(const APenguinGameState* GameState);
	/** Our own penguin plays by its bot brain (switched on again whenever something switched it off). */
	void DriveOwnPenguin();
	/**
	 * The belly slide over LAN, in rounds 1 and 2 while everybody holds still (bots too): the host builds a short-lived
	 * platform in the air and puts the two players' penguins on it; round 1 the client's penguin slides into the host's,
	 * round 2 the host's into the client's. Both machines judge what they saw (RESULT lines). Drives our own penguin while
	 * it runs; true while everybody holds still.
	 */
	bool UpdateSlideCheck();
	/** Records the slide check of round Index + 1 (once). */
	void JudgeSlideCheck(int32 Index, const TCHAR* Why);
	/**
	 * After the slide check, in the same hold: round 1 the host's penguin slaps the client's (a second's stun: the client's
	 * penguin can't move or use anything, it is dizzy, then it walks again); round 2 the host freezes the client's penguin and
	 * hits it with a snowball (the ice takes it: no knock), freezes it again and blows a gust at it (the ice cube slides off,
	 * still frozen). Round 1 ends with an ordinary hit (no slap) that sends the client's penguin flying: its player throws a
	 * snowball in the flight, stunned, and it comes out (abilities work in a knockback). Both machines judge what they saw
	 * (the knock with the slap, in JudgeEffectCheck). Returns the stick our own penguin holds.
	 */
	FVector2D UpdateEffectCheck(int32 Index, float T);
	void JudgeEffectCheck(int32 Index, const TCHAR* Why);
	/** Then, still in the hold: round 1 the host hooks an ice block of the platform (pulled out whole, it comes about
	 *  halfway and is a block again where it lands), round 2 it hooks the client's penguin frozen (the whole ice cube comes
	 *  about halfway, falling like a block, and shatters where it lands). Both machines watch (UpdateEffectCheck calls it). */
	void UpdateHookCheck(int32 Index, float T, APenguinCharacter* HostPenguin, APenguinCharacter* ClientPenguin);
	void JudgeHookCheck(int32 Index, const TCHAR* Why);
	/** Round 1, after the hook, still in the hold: the host sticks an icicle into a fresh platform next to the client's
	 *  penguin (a ground pad), the client walks into it and is thrown straight up. Both machines watch. Returns the stick
	 *  our own penguin holds. */
	FVector2D UpdatePadCheck(float T);
	void JudgePadCheck(const TCHAR* Why);
	/** Round 1, after the pad, still in the hold: the icicle's charged throw on a fresh platform high above. The host's
	 *  penguin charges a full second (the client sees it charging: the icicle in its flipper) and throws - fast and perfectly
	 *  straight - then taps one - a weak lob (about 2 m); the client's penguin charges half a second on its own machine (shown there at once,
	 *  sent to the server) - in between. Both machines watch every icicle and how far it gets. */
	void UpdateThrowCheck(float T);
	void JudgeThrowCheck(const TCHAR* Why);
	/** Round 2, last part of the hold: the bot grown to 8x pops (host), sampled on both machines. */
	FVector2D UpdateSizeCheck(float T);
	/** Every tick (both): once the giant was sampled, watches it get knocked out and gone, then judges the size check. */
	void UpdateSizeWatch();
	/** Everybody moves again after the checks (our own brain: DriveOwnPenguin, the bots' brains back on). */
	void ReleaseHold();
	/** Client: every knock-out of another machine's penguin (the host's, the bot) plays its death look here too, from the
	 *  replicated knock-out: the look is on, with the knock-out's reason, and shows it (flat, frozen, sooty, gone). */
	void WatchDeathLooks();
	/** The other human player's penguin (on the host the client's, on the client the host's). */
	APenguinCharacter* GetOtherPlayerPenguin() const;
	APenguinCharacter* GetOwnPenguin() const;
	int32 CountPenguins() const;
	/** The RESULT lines (once per process: the host logs them before it leaves, the client when it is back in a solo world). */
	void LogResults(const FString& Why);
	/** Every tick, both machines (PinguNetTestIsland.cpp): every fight the island made for its penguins (a client's the
	 *  host's) and the camera on our own penguin; in the lobby the client's Snow Warp kept on its own screen (a narrow test
	 *  screen; the host fires the warp); the late join's island growing from two penguins to three. */
	void UpdateIslandWatch();
	/** Host: the client's lobby Snow Warp is fired and had a second to show up on the client (always true in -PinguNetLate,
	 *  which has no lobby warp). The match waits for it: the client's first frames in the new world can take 1 to 5 s, so a
	 *  fixed start delay sometimes started the match before the client had a lobby to judge its warp in. */
	bool IsLobbyWarpSettled() const;
	/** Their RESULT lines (LogResults calls it). */
	void AddIslandChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);
	/** Every tick, both machines (PinguNetTestLobby.cpp; not -PinguNetLate / -PinguNetTraits): in the lobby, the host's lobby island
	 *  here, the other player's penguin on it (the host: it pops in with a puff), the name tags say the people's names. */
	void UpdateLobbyIslandWatch();
	/** Its RESULT lines (host 2, client 2; LogResults calls it). */
	void AddLobbyIslandChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);

	// ---- The tornado (PinguNetTestTornado.cpp): round 1, the last part of the hold (after the icicle throws), TornadoCheckLength
	// seconds. The host builds a platform high above with two ice blocks on it, puts the two players' penguins on it 2.5 m
	// apart and starts a tornado with its own: both machines watch the client's penguin sucked in and thrown, the host's hang
	// still, the ice leave the island the same way on both. Then two tornadoes merge: on a new platform the two players'
	// penguins, 8 m apart, start one each (each on its own machine); both machines see one giant tornado with both in its eye
	// (the client's own penguin drifts there on its own machine), and the bot 11 m from its middle thrown.
	static constexpr float TornadoCheckLength = 6.5f;
	/** Both machines, T seconds into its part of the hold. */
	void UpdateTornadoCheck(float T);
	/** Judges both parts (the merge only once its part has begun). */
	void JudgeTornadoCheck(const TCHAR* Why);
	/** The merge part (from its own start time within the tornado's part). */
	void UpdateTornadoMergeCheck(float T);
	void JudgeTornadoMergeCheck(const TCHAR* Why);
	/** A new round: its per-round state goes (a result stays). */
	void ResetTornadoCheck();
	/** Its RESULT lines (LogResults calls it). */
	void AddTornadoChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);

	// ---- The self bomb (PinguNetTestSelfBomb.cpp): round 1, the very last part of the hold (after the tornado),
	// SelfBombCheckLength seconds. The host builds a platform high above with the two players' penguins on it 6 m apart and drops
	// a snow bomb at its own feet: both machines see its penguin knocked out (reason Bomb) and the feed say it blew itself up,
	// with no kill for anyone in the score rows (one fall for the host). Then a bomb of the client's goes off 2 m from the
	// client's penguin: pushed like anybody (the hit reaches its owner), it survives.
	static constexpr float SelfBombCheckLength = 3.6f;
	/** Both machines, T seconds into its part of the hold. */
	void UpdateSelfBombCheck(float T);
	void JudgeSelfBombCheck(const TCHAR* Why);
	/** A new round: its per-round state goes (a result stays). */
	void ResetSelfBombCheck();
	/** Its RESULT line (LogResults calls it). */
	void AddSelfBombChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);

	// ---- Ice that is solid at once and grows with its maker (PinguNetTestIceScale.cpp): round 2, the last part of the hold (after
	// the giant), IceScaleCheckLength seconds. The host builds a platform high above, its penguin raises an ice wall right where
	// the client's penguin stands: on both machines (the client's own: client-authoritative) the client's penguin goes up onto the
	// wall - never inside it, never falling through, no snapping back. Then the host drops a 4x iceberg (2x2 big blocks): both
	// machines see its shape fall and its big blocks on the island. -PinguNetLate: the host keeps a big block in every round's
	// island; the client finds it in the island it gets when it joins.
	static constexpr float IceScaleCheckLength = 4.6f;
	/** Both machines, T seconds into its part of the hold. */
	void UpdateIceScaleCheck(float T);
	void JudgeIceScaleCheck(const TCHAR* Why);
	/** A new round: its per-round state goes (a result stays). */
	void ResetIceScaleCheck();
	/** Every tick, both machines (Observe): -PinguNetLate's big block (the host makes it, the client finds it when it joins). */
	void UpdateIceScaleWatch();
	/** Its RESULT lines (LogResults calls it). */
	void AddIceScaleChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);

	// ---- The giant snowball (PinguNetTestGiant.cpp): round 2, the last part of the hold (after the ice scale), GiantCheckLength
	// seconds. The host's penguin rolls one up (the real ability) on a platform high above; it swallows the client's penguin 6 m
	// away (both machines: hidden, no collision, drawn inside, the client's own hanging still), then the host breaks it and both
	// see the client's penguin out again where it broke.
	static constexpr float GiantCheckLength = 6.f;
	// ---- The snowman (PinguNetTestSnowman.cpp): round 2, the very last part of the hold (after the giant), SnowmanCheckLength s
	// (and the build limit after it, 2026-11-05: 4.4 -> 6.4 s).
	static constexpr float SnowmanCheckLength = 6.4f;
	void UpdateSnowmanCheck(float T);
	void JudgeSnowmanCheck(const TCHAR* Why);
	void ResetSnowmanCheck();
	void AddSnowmanChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);
	void UpdateGiantCheck(float T);
	void JudgeGiantCheck(const TCHAR* Why);
	void ResetGiantCheck();
	void AddGiantChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);

	// ---- Traits mode (-PinguNetTraits, PinguNetTestTraits.cpp): the host turns TRAITS on in the lobby and forces the
	// traits (round 1: host Giant, client Second Chance, bot Glass Cannon; round 2: Heavyweight, Featherweight, Gambler;
	// -PinguNetLate: host Giant, everybody else Glass Cannon); no slide / stun / hook / pad / throw / size checks then.
	/** -PinguNetTraits is on the command line. */
	static bool IsTraitsRun();
	/** Every tick, both machines: TRAITS in the lobby, the traits each round as this machine has them, the reveal, the looks;
	 *  round 1 the client's Second Chance (thrown into the sea twice by the host: one rescue, then a knock-out), round 2
	 *  the client's triple jump - everybody holds still for those. True while it holds our own penguin. */
	bool UpdateTraitsCheck();
	/** Host, right before StartMatch: Traits mode as the lobby's toggle has it, and round 1's forced traits. */
	void PrepareTraitsMatch(class APenguinGameMode* GameMode, struct FPinguMatchSettings& Settings);
	/** The traits run's RESULT lines (LogResults calls it instead of the usual checks). */
	void AddTraitsChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);

	// ---- The anti-cheat (PinguNetTestAntiCheat.cpp, PinguAntiCheat.h). Every run: the join check passed both ways, the client
	// recomputed and found every deal, nothing of the honest client's ignored, no false alarm. -PinguNetCheat=host (the host also
	// has -PinguCheat=picks,cooldown): the client leaves the match with "Your games don't match."; -PinguNetCheat=client (the
	// client has -PinguCheat=client): the host ignores its spam and teleports, the client plays on. Cheat runs skip the holds.
	/** -PinguNetCheat=host or =client is on the command line. */
	static bool IsCheatRun();
	/** The client's join loop after it landed in a solo world again: true once in a -PinguNetCheat=kick run when the error is
	 *  "The host kicked you." (it then joins again at once: a kick bans nobody; 2026-11-07). */
	static bool RejoinAfterKick(const FString& Error);
	/** The client's join loop: a join attempt failed with this error (the nohotjoin run checks the host's message). */
	static void NoteJoinFailure(const FString& Error);
	/** The client's join loop: the connection came up (counted by the kick / nohotjoin runs). */
	static void NoteConnected();
	/** Host, every tick: -PinguNetCheat=nohotjoin turns "Join running matches" off (memory only) until two joiners were turned
	 *  away, then back on, so the same client's next try gets in. */
	void UpdateHotJoinSwitch();
	/** -PinguNetCheat=choose (+ -PinguChoose, PinguNetTestPicks.cpp; 2026-11-07): the client picks Snowball, random, Flap in the
	 *  lobby, the host's match is ABILITIES: CHOOSE; both watch the joiner's loadout each round (UpdatePicksRun, every tick) and
	 *  log a check (AddPicksChecks); the client's judge must find every deal (the usual anti-cheat checks). */
	void PreparePicksMatch(struct FPinguMatchSettings& Settings);
	void UpdatePicksRun();
	void AddPicksChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);
	/** Its RESULT lines (LogResults calls it; in a cheat run with only the basic checks). */
	void AddAntiCheatChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);

	// ---- The host's say over the client's own penguin (PinguNetTestHostAuthority.cpp, PinguHostAuthority.h). Every honest run:
	// the host's knockback / freeze handed back on its path, nothing pulled back, the corrections a minute. -PinguNetCheat=move /
	// =combat: the client cheats with its movement (-PinguCheat=fly,speed,fakey,noack / noknock,slide,stomp,giant); the host
	// refuses it, moves the penguin itself (into the sea), counts one slide a cooldown and no made-up landing.
	/** The host, every frame: watches the client's penguin; the combat run knocks it toward the sea every 3 s of a fight. */
	void UpdateHostAuthority(float DeltaSeconds);
	void AddHostAuthorityChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);
	/** Logs the RESULT lines (if not yet) and quits the game. */
	void Finish(const FString& Why);
	void Report(const FString& Message) const;

	bool bHostRole = true;
	/** Real seconds in this world (the actor is new in every world). */
	float WorldTime = 0.f;
	bool bWorldLogged = false;
	bool bPhaseSeen = false;
	/** EPinguPhase of the last OnPhase. */
	uint8 LastPhase = 0;
	int32 LastRound = -1;
	int32 LastFightRound = -1;
	float ClientJoinedTime = -1.f;
	float MatchOverTime = -1.f;
	/** Host: END MATCH was picked (the match is endless; it ends after MinRounds rounds). */
	bool bEndMatchSent = false;
	float LeaveTime = -1.f;
	bool bQuitRequested = false;
	/** Client: the LAN search runs in this solo world; a join is under way from it; the next attempt waits until then. */
	bool bSearchStarted = false;
	bool bJoinUnderWay = false;
	float JoinWaitUntil = 0.f;
	int32 BotSwitchOns = 0;

	/** The last "penguins N: [slot ...]" line (logged again whenever it changes). */
	FString LastPenguinsText;
	/** The kill feed's texts last frame (new lines are logged once). */
	TArray<FString> LastFeed;
	TArray<TWeakObjectPtr<AActor>> SeenProjectiles;
	TArray<TWeakObjectPtr<AActor>> SeenPickups;

	TWeakObjectPtr<APenguinCharacter> OwnPenguin;
	FVector LastOwnLocation = FVector::ZeroVector;
	TArray<int32> LastUseCounts;

	// the slide check of this round (UpdateSlideCheck)
	int32 SlideRound = -1;
	bool bSlideSetUp = false;
	bool bSlidePressed = false;
	bool bSlideJudged = false;
	bool bSlideWatching = false;
	/** Phase time until which the slider's stick still points the slide's way. */
	float SlideInputUntil = -1.f;
	float SlideWay = 1.f;
	int32 SlideTargetHitsAtStart = 0;
	int32 SlideOwnSlidesAtStart = 0;
	/** How far the slide had gone when the target was hit (-1: not hit), the target's velocity then. */
	float SlideHitDistance = -1.f;
	FVector SlideTargetVelocity = FVector::ZeroVector;
	/** Our own slide: it was seen sliding, and how far it had gone when it stopped (-1: still going / never). */
	bool bOwnSlideSeen = false;
	float OwnSlideStop = -1.f;
	/** Host: the bots held still for the check (their brains go back on afterwards). */
	TArray<TWeakObjectPtr<APenguinCharacter>> HeldBots;

	// the stun / ice check of this round (UpdateEffectCheck)
	bool bEffectSetUp = false;
	bool bEffectJudged = false;
	bool bEffectWatching = false;
	/** Host: the hits done so far. */
	int32 EffectStep = 0;
	/** From the host's penguin to the client's (+1 right). */
	float EffectWay = 1.f;
	int32 EffectHitsAtStart = 0;
	int32 EffectUsesAtStart = 0;
	// round 1: the slap
	float EffectHitT = -1.f;
	float EffectStunAtHit = 0.f;
	float EffectStunEndT = -1.f;
	float EffectMaxToward = 0.f;
	float EffectMovedAgainT = -1.f;
	bool bEffectAbilityTried = false;
	bool bEffectDizzy = false;
	/** The client's ability uses from the start of the check to the knock below (the slap's part; -1: not got that far). */
	int32 EffectUsesBeforeKnock = -1;
	// round 1, then: an ordinary hit sends the client's penguin flying - it throws a snowball in the flight
	bool bKnockWatching = false;
	int32 KnockUsesAtStart = 0;
	int32 KnockHitsAtStart = 0;
	float KnockT = -1.f;
	/** Client: it pressed in the flight (stunned, in the air), with the snowball in a slot and ready. */
	bool bKnockPressed = false;
	bool bKnockSlotReady = false;
	/** When the use showed here, and whether the client's penguin was stunned / in the air then (as this machine has it). */
	float KnockUseT = -1.f;
	bool bKnockUseStunned = false;
	bool bKnockUseInAir = false;
	// round 2: the ice
	bool bEffectFrozenA = false;
	float EffectThawT = -1.f;
	float EffectSnowballT = -1.f;
	float EffectMaxSpeedOnIce = 0.f;
	bool bEffectFrozenB = false;
	float EffectGustT = -1.f;
	FVector EffectGustFrom = FVector::ZeroVector;
	bool bEffectStillFrozen = false;
	bool bEffectStillSampled = false;
	float EffectSlid = 0.f;
	int32 EffectHitsAfterIce = 0;
	bool bEffectAlive = true;
	// round 1, then: the host's fish hook pulls an ice block out of the platform (it comes about halfway, a block again)
	FIntPoint EffectIceCell = FIntPoint(0, 0);
	bool bHookFired = false;
	bool bClientParked = false;
	bool bHookWatching = false;
	bool bFlyerSeen = false;
	FIntPoint FlyerFromCell = FIntPoint(0, 0);
	int32 BrokenAtHook = 0;
	// round 2, then: frozen once more and hooked - the whole ice cube comes about halfway and shatters where it lands
	int32 YankHitsAtStart = 0;
	bool bYankWatching = false;
	float YankT = -1.f;
	float YankFromX = 0.f;
	float YankHostX = 0.f;
	float YankLandT = -1.f;
	float YankLandX = 0.f;
	float YankThawT = -1.f;
	bool bYankFrozenAt = false;
	bool bYankFrozenMid = false;
	bool bYankMidSampled = false;
	bool bYankAlive = true;
	float YankSampleT[2] = { -1.f, -1.f };
	float YankSampleVz[2] = { 0.f, 0.f };
	/** Every frame of the flight: (game time, vertical speed) - the gravity is fitted to them. */
	TArray<FVector2D> YankVzSamples;
	/** How far the ice cube stayed on its own side of the host (it is no over-the-head throw: above 0). */
	float YankNearest = 1.e9f;
	// round 1, last: the ground pad (UpdatePadCheck)
	bool bHoldOver = false;
	bool bPadSetUp = false;
	bool bPadWatching = false;
	bool bPadJudged = false;
	bool bPadSeen = false;
	int32 PadBouncesAtStart = 0;
	float PadStartFeetZ = 0.f;
	float PadPeakFeetZ = 0.f;
	float PadBounceT = -1.f;
	// round 1, after the pad: the icicle throws (UpdateThrowCheck)
	bool bThrowSetUp = false;
	bool bThrowWatching = false;
	bool bThrowJudged = false;
	/** What the test did so far (host: long press, long release, tap press, tap release, moved aside; client: press, release). */
	int32 ThrowStep = 0;
	/** The other player's penguin seen charging (its icicle in the flipper) - and our own charge showing in the frame of the
	 *  press (the client: before the server knew). */
	bool bThrowOtherChargeSeen = false;
	bool bThrowOwnShownAtOnce = false;
	/** The host aims left for the first 0.6 s of its charge and right after that (its own movement facing stays right), the client
	 *  aims left for its whole hold: each charging penguin must face its aim as this machine shows it. Per penguin (0 the host's,
	 *  1 the client's) and window (0 aim left, 1 aim right): the frames sampled and the frames the mesh looked the wrong way. */
	bool bThrowFaceFlipped = false;
	int32 ThrowFaceFrames[2][2] = { { 0, 0 }, { 0, 0 } };
	int32 ThrowFaceWrong[2][2] = { { 0, 0 }, { 0, 0 } };
	/** Every icicle of the two players seen here: whose, from where, how far it got. */
	struct FThrowWatch
	{
		TWeakObjectPtr<AActor> Icicle;
		bool bHost = true;
		float FromX = 0.f;
		float Far = 0.f;
	};
	TArray<FThrowWatch> ThrowWatches;
	// round 2, last: the giant (UpdateSizeCheck / UpdateSizeWatch; one-shot, not reset with the round)
	bool bSizeSetUp = false;
	bool bSizeSampled = false;
	bool bSizePopped = false;
	TWeakObjectPtr<APenguinCharacter> SizeGiant;
	float SizeSeen = 0.f;
	float SizeSampledAt = -1.f;
	int32 SizeClientHitsAtStart = 0;
	float SizeKOSeenAt = -1.f;
	uint8 SizeKOReason = 0;
	bool bSizeHiddenSeen = false;
	float SizeMaxTall = 0.f;

	// the death looks of the others' knock-outs (client, WatchDeathLooks): one entry per knock-out while it is watched
	struct FDeathWatch
	{
		TWeakObjectPtr<APenguinCharacter> Penguin;
		/** WorldTime it was first seen knocked out. */
		float SeenAt = 0.f;
		/** EPenguinKOReason as replicated (0: none yet). */
		uint8 Reason = 0;
		bool bLookOn = false;
		bool bReasonSeen = false;
		/** It showed what its reason looks like (flat, frozen, sooty; the sea: gone) while it was still there. */
		bool bShapeSeen = false;
		bool bWasShown = false;
		/** Counted already (kept until the penguin is back in). */
		bool bJudged = false;
	};
	TArray<FDeathWatch> DeathWatches;

	// this round (logged when it ends)
	int32 RoundProjectiles = 0;
	int32 RoundPickups = 0;
	int32 RoundAbilities = 0;
	float RoundDistance = 0.f;
};

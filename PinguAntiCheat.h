// Pingu - anti-cheat for LAN games (2026-10-29; Linus: "a firend of mine used ai to reverse engenire the .exe and make a cheat so
// he can chose his items no cooldown usw can you make an counter against that"; caught cheats are blocked silently).
//
// Three parts:
//  1. The host checks every request of a joiner (PenguinCharacter.cpp, PenguinAbilities.cpp, PenguinMovementComponent.cpp call
//     the gates here): rate limits, an icicle hold no longer than the host saw it, no teleports and no impossible ground speed.
//     The abilities themselves were always the host's (its loadout, its cooldowns, its state checks). Illegal requests are
//     ignored with a log line ([ANTICHEAT] host ignored ...), nothing else happens.
//  2. Fair picks (commit-reveal): the host commits to a secret before each round (only its hash is replicated), every joiner
//     adds a random value of its own, the round's seed is the hash of all of them. The host reveals its secret with the deal,
//     so every joiner recomputes everyone's abilities and traits (the host's too) and checks them (PinguFair::Deal).
//  3. Every joiner watches what the host replicates (FPinguCheatJudge): loadouts as dealt, cooldowns as the rules say, abilities
//     only from the deal or a bubble, no teleports or impossible speed of the host's own penguin and the bots. Clear, repeated
//     violations: the joiner leaves quietly with "Your games don't match." and logs what it saw.
//  Plus the join handshake: both sides compare a build id and a challenge-response hash of the game's files (the exe and the
//  paks; in an editor run the game module), so a plainly modified exe or pak can't play with an unmodified one.
//
// Dev-only cheat switches for the tests (compiled out of Shipping): -PinguCheat=picks,cooldown (a cheating host),
// -PinguCheat=client (a cheating joiner: spammed and illegal ability requests, teleports).
// What a determined cheater can still do: docs/FEATURES.md, section 7.1.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "PenguinTypes.h"
#include "PinguTraitTable.h"
#include "PinguAntiCheat.generated.h"

class APenguinCharacter;
class APlayerController;

/** One joiner's random value for a round's seed (its PlayerId, 32 hex digits). */
USTRUCT()
struct FPinguFairContribution
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Player = 0;

	UPROPERTY()
	FString Value;
};

/** The host's promise for the next deal: SHA-1 of its secret and the serial. */
USTRUCT()
struct FPinguFairCommit
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Serial = 0;

	UPROPERTY()
	FString Hash;
};

/** Everything a joiner needs to recompute a round's deal. */
USTRUCT()
struct FPinguFairReveal
{
	GENERATED_BODY()

	/** The commit it belongs to (0: none yet). */
	UPROPERTY()
	int32 Serial = 0;

	/** The round it dealt. */
	UPROPERTY()
	int32 Round = 0;

	/** The host's secret (its commit's hash must match). */
	UPROPERTY()
	FString Secret;

	/** The joiners' values that went into the seed (every one that came for this commit). */
	UPROPERTY()
	TArray<FPinguFairContribution> Contributions;

	/** The penguins dealt to, by slot, ascending. */
	UPROPERTY()
	TArray<uint8> Slots;

	/** Traits mode: traits were dealt this round. */
	UPROPERTY()
	bool bTraits = false;

	/** The match's trait bag before this deal (EPinguTrait values; empty: a fresh bag). */
	UPROPERTY()
	TArray<uint8> TraitBag;

	/** A test's forced traits (one for everybody, or by slot). Never honoured by a Shipping joiner. */
	UPROPERTY()
	TArray<uint8> ForcedTraits;

	/** The host's unticked traits (FPinguMatchSettings::TraitsOff): a fresh bag holds only the ticked ones (2026-11-07). */
	UPROPERTY()
	uint64 TraitsOff = 0;

	/** Abilities CHOOSE (2026-11-07, card ability-picks): the match's setting, and every slot's picks - PinguPicks::NumSlots
	 *  entries per entry of Slots, in Slots' order (EPenguinAbility, 0 = random); empty when nobody picked. */
	UPROPERTY()
	bool bChoose = false;

	UPROPERTY()
	TArray<uint8> Picks;
};

/** A round's deal as PinguFair::Deal computes it: per entry of Slots its trait and its abilities. */
struct FPinguFairDeal
{
	TArray<uint8> Slots;
	TArray<EPinguTrait> Traits;
	TArray<TArray<EPenguinAbility>> Loadouts;
	TArray<EPinguTrait> BagAfter;

	int32 IndexOf(int32 Slot) const { return Slots.IndexOfByKey(static_cast<uint8>(Slot)); }
};

namespace PinguFair
{
	/** 32 random hex digits (two GUIDs from the OS's random source). */
	PENGUIN_API FString MakeSecret();
	/** The commit of a secret: SHA-1 hex of "<serial>|<secret>". */
	PENGUIN_API FString CommitOf(const FString& Secret, int32 Serial);
	/** True for 1..64 hex digits (what a joiner may send). */
	PENGUIN_API bool IsValidValue(const FString& Value);
	/** The round's seed: SHA-1 of the serial, the round, the secret and every contribution in slot order. */
	PENGUIN_API int32 SeedOf(const FPinguFairReveal& Reveal);
	/**
	 * The deal (every machine computes the same): traits first (Traits mode; a test's forced ones, else ONE draw from the
	 * match's bag for everybody - PinguTraits::DrawShared, as the solo deal; 2026-11-07, shared-traits), then each
	 * slot's loadout (UPenguinAbilityComponent::RollLoadout with its trait's number of abilities), from two streams of the seed.
	 */
	PENGUIN_API void Deal(const FPinguFairReveal& Reveal, FPinguFairDeal& Out);
	/** A slot's picks in a reveal (empty: none, or the reveal's picks are malformed). */
	PENGUIN_API TArray<uint8> PicksOf(const FPinguFairReveal& Reveal, int32 Slot);
}

/** What a joiner knows when a reveal arrives (PinguFair::CheckReveal; 2026-11-05, card host-cheat-fix). */
struct FPinguRevealCheck
{
	/** The commit we saw for the reveal's serial (empty: none - it came before we did). */
	FString SeenHash;
	/** Our PlayerId and the value we sent for this serial (empty: none), how long before the reveal it went out. */
	int32 OwnPlayer = INDEX_NONE;
	FString OwnValue;
	double OwnSentAgo = -1.0;
	/** Every joiner's PlayerId we have seen in this game (people, not bots; the host's own not among them). */
	TSet<int32> KnownJoiners;
	/** The ones among them who have left since (2026-11-07, kick / leave): the host drops a leaver's value from the reveal, so
	 *  "received and then left out" does not count for them. */
	TSet<int32> LeftJoiners;
	/** What the replicated Received list showed for this serial (PlayerId -> value). */
	TMap<int32, FString> SeenReceived;
	/** The match so far: rounds already dealt, and the round before this one if it was dealt (its traits and bag after). */
	TSet<int32> RoundsDealt;
	int32 PrevRound = -1;
	bool bPrevTraits = false;
	TArray<uint8> PrevBagAfter;
	/** Ability picks (2026-11-07): our own slot and every set of picks we asked the host for; the setting as seen lately
	 *  (bit 0: RANDOM, bit 1: CHOOSE; 0: unknown); the bots' slots; the match so far (CHOOSE of its first reveal, -1 none yet;
	 *  every slot's picks). */
	int32 OwnSlot = -1;
	TArray<TArray<uint8>> OwnPicksSent;
	uint8 ChooseSeen = 0;
	TSet<int32> BotSlots;
	int32 MatchChoose = -1;
	TMap<int32, TArray<uint8>> MatchPicks;
};

namespace PinguFair
{
	/**
	 * Every rule break in a reveal, with its points (FPinguCheatJudge::LeaveAt: leave at once): a secret that does not match the
	 * commit, our value changed, a value under a PlayerId that is no joiner (made up, or the host's own: with one the host could
	 * grind the seed until it deals what it wants), a joiner's value other than the one Received showed, or left out, the same
	 * round dealt twice in a match (a re-roll), a trait bag that is not the last round's. Our own value left out though sent
	 * ContributionLead s before: 1 point (it may have crossed the deal).
	 */
	PENGUIN_API TArray<TPair<int32, FString>> CheckReveal(const FPinguFairReveal& Reveal, const FPinguRevealCheck& Check);
	/** A new commit is fine when it is the first we see, or the next serial once the one before was revealed (a commit dropped
	 *  unrevealed would let the host peek at a deal and roll again). */
	PENGUIN_API bool IsCommitStepFine(int32 LastSeenSerial, int32 NewSerial, int32 RevealSerialNow);
}

/** What a joiner sees of one penguin in one frame (APinguAntiCheat fills it from the replicated state; tests make it up). */
struct FPinguSeenSlot
{
	EPenguinAbility Ability = EPenguinAbility::None;
	int32 Uses = 0;
	int32 Swaps = 0;
	/** Server clock. */
	double End = 0.0;
	float Length = 0.f;
};

struct FPinguSeenPenguin
{
	int32 Slot = -1;
	FString Name;
	EPinguTrait Trait = EPinguTrait::None;
	TArray<FPinguSeenSlot> Slots;
	/** Judge its movement (the host's own penguin and the bots: the host simulates them). */
	bool bJudgeMotion = false;
	/** A new position sample (MotionTime: the host's clock at that position). */
	bool bNewSample = false;
	double MotionTime = 0.0;
	FVector Location = FVector::ZeroVector;
	bool bWalking = false;
	/** Out, hidden, inside a giant snowball, hanging in a tornado, frozen, stunned, a bounce / throw of a trait: no motion
	 *  judgement now (and for a while). */
	bool bMotionExempt = false;
	/** Hits it took (every penguin: a hit can give a snowball back or cut a Leech's cooldowns). */
	int32 HitEvents = 0;
	/** Where it is now (every penguin: a bubble must go away near it to give it an ability) and its size. */
	FVector Position = FVector::ZeroVector;
	float Size = 1.f;
	/** Jester's swaps so far (a Jester's new ability needs one); Leech's cooldown cuts so far (each allows LeechCooldownCut). */
	int32 JesterSwaps = 0;
	int32 LeechCuts = 0;
	bool bKnockedOut = false;
};

/**
 * A joiner's judge of the host (pure logic, no world: the anti-cheat test feeds it made-up streams). Strikes have points; at
 * LeaveAt points the joiner leaves. The same kind on the same penguin counts once per StrikeSpacing seconds.
 */
class PENGUIN_API FPinguCheatJudge
{
public:
	static constexpr int32 LeaveAt = 4;
	static constexpr double StrikeSpacing = 3.0;
	/** Host-clock slack for the cooldown checks (rounding only: every time compared is the host's own). */
	static constexpr double CooldownSlack = 0.15;
	/** A slot's ability may change only through a bubble that went away within this many seconds (either side). */
	static constexpr double PickupWindow = 2.5;
	/** Fastest move the rules allow outside a hit (cm/s; a fast fall 3000, a stomp 3300, a ground pad 2970) + a fixed slack. */
	static constexpr float JumpSpeed = 9000.f;
	static constexpr float JumpSlack = 600.f;
	/** Average ground speed over GroundWindow (cm/s): a dash is 1900, a Speedster's 2850. */
	static constexpr float GroundSpeed = 4200.f;
	static constexpr double GroundWindow = 0.5;
	/** After a hit (or anything exempt) motion is not judged for this long (a 4x knock flies far). */
	static constexpr double MotionQuiet = 4.0;
	/** The deal must be on every penguin this long after the fight began. */
	static constexpr float LoadoutGrace = 1.5f;
	/** Uses of one ability by one penguin, timed by their arrival: k more uses need k cooldowns minus this (lag, frames). */
	static constexpr double RateSlack = 1.25;
	/** A bubble counts as taken by a penguin it went away this near to (cm, + TakeRangePerSize x its size). */
	static constexpr float TakeRange = 350.f;
	static constexpr float TakeRangePerSize = 150.f;
	/** A shot (a new projectile, snowman, giant snowball) and the counted use that made it may arrive this far apart (s). */
	static constexpr double ShotWindow = 1.5;
	/** A new round's reveal must have come this long into its fight. */
	static constexpr float RevealGrace = 3.f;

	/** A new round (or the fight is over): the per-round tracking starts again. */
	void SetRound(int32 Round, bool bFight, float FightAge, double ClientNow);
	/** The verified deal of a round (only rounds whose commit this joiner saw). */
	void ExpectDeal(int32 Round, const FPinguFairDeal& Deal);
	/** One penguin, this frame. ServerNow: the host's clock as this joiner has it. */
	void Observe(double ClientNow, double ServerNow, const FPinguSeenPenguin& Seen);
	/** A bubble with this ability went away at Where (taken by whoever was near, swallowed, popped or timed out). */
	void NotePickupGone(EPenguinAbility Ability, double ClientNow, const FVector& Where);
	/** Something an ability makes appeared (a projectile, a snowman, a giant snowball), made by Slot's penguin: it must hold
	 *  the ability, and the host must have counted the use. */
	void NoteShot(double ClientNow, double ServerNow, int32 Slot, EPenguinAbility Ability);
	/** A new round began while we watched, with a commit seen: its reveal must come (RevealGrace into the fight). */
	void ExpectRevealFor(int32 InRound);
	/** A new match (the lobby or the results): round numbers start again. */
	void NewMatch();
	/** A reveal came for this round that we can't check (its commit came before we did): not missing. */
	void NoteRevealed(int32 InRound) { RevealedRounds.Add(InRound); }
	/** Dev builds: the net test's own script hands out abilities and throws things (no loadout / shot judgement meanwhile). */
	void SetDevScript(bool bOn) { bDevScript = bOn; }
	/** Settles what waited for a bubble. Call once per frame after the Observes. */
	void Tick(double ClientNow);
	void Strike(int32 Points, int32 Slot, const TCHAR* Kind, const FString& Detail, double ClientNow);

	int32 GetPoints() const { return Points; }
	bool ShouldLeave() const { return Points >= LeaveAt; }
	const TArray<FString>& GetStrikes() const { return Strikes; }
	/** Penguins whose deal was found as computed. */
	int32 GetDealsVerified() const { return DealsVerified; }
	int32 GetUsesJudged() const { return UsesJudged; }
	int32 GetSamplesJudged() const { return SamplesJudged; }

private:
	struct FTrack
	{
		bool bValid = false;
		TArray<FPinguSeenSlot> Slots;
		double ObservedAt = 0.0; // the host's clock (as we had it) at the last observation
		bool bLoadoutChecked = false;
		// motion
		bool bHaveSample = false;
		double SampleTime = 0.0;
		FVector SampleAt = FVector::ZeroVector;
		double QuietUntil = -1.0; // host clock
		int32 HitEvents = 0;
		double WindowStart = -1.0;
		FVector WindowFrom = FVector::ZeroVector;
		// the round's loadout (2026-11-05): what it may hold, what it holds without a reason yet
		bool bEarnedSet = false;
		TSet<EPenguinAbility> Earned;
		TMap<EPenguinAbility, double> Unearned; // client clock since
		TArray<bool> Armed;
		bool bTraitChecked = false;
		bool bNotDealtStruck = false;
		bool bVerified = false;
		TMap<EPenguinAbility, double> Held; // client clock it last held each ability
		TArray<TPair<double, FVector>> Positions; // client clock
		float Size = 1.f;
		int32 Jester = -1;
		TArray<double> JesterBumps; // client clock
		double LastJester = -100.0;
		TMap<EPenguinAbility, double> Arrived; // client clock an ability last came into a slot
		int32 Hits = -1;
		bool bOut = false;
		TMap<EPenguinAbility, TArray<double>> UseTimes; // server clock
		int32 LeechCuts = -1;
		TArray<double> LeechTimes; // server clock
		TMap<EPenguinAbility, TArray<double>> Uncounted; // shots with no counted use (client clock)
		TMap<EPenguinAbility, TArray<double>> Unheld; // shots of an ability it never had this round (client clock)
		TArray<TPair<EPenguinAbility, double>> Counted; // counted uses not matched with a shot yet (client clock)
	};
	struct FPendingSwap
	{
		int32 Slot = -1;
		EPenguinAbility Ability = EPenguinAbility::None;
		double Since = 0.0;
		FString Name;
	};
	struct FGone
	{
		EPenguinAbility Ability = EPenguinAbility::None;
		double At = 0.0;
		FVector Where = FVector::ZeroVector;
		bool bUsed = false;
	};
	/** Something that waits a moment for its excuse (a bubble, a hit, the host's count of the use) before it is a strike. */
	enum class EPending : uint8 { Cut, Refund, Shot };
	struct FPending
	{
		EPending Kind = EPending::Cut;
		int32 Slot = -1;
		EPenguinAbility Ability = EPenguinAbility::None;
		double Since = 0.0; // client clock
		double ServerAt = 0.0;
		FString Name;
		FString Detail;
		/** A hit about then excuses it (a snowball given back); a Leech's own cut about then (bLeech). */
		bool bHitOk = false;
		bool bLeech = false;
	};
	void JudgeSlots(FTrack& Track, double ServerNow, double ClientNow, const FPinguSeenPenguin& Seen);
	void JudgeLoadout(FTrack& Track, double ClientNow, const FPinguSeenPenguin& Seen);
	void JudgeMotion(FTrack& Track, double ClientNow, const FPinguSeenPenguin& Seen);
	/** A bubble of Ability went away near the penguin within the pickup window of At (bConsume: a bubble gives only once). */
	bool TakenNear(FTrack& Track, EPenguinAbility Ability, double At, bool bConsume);
	bool HitNear(double ServerAt, double Window) const;
	int32 CountHits(double From, double To) const;
	/** A use at ServerAt: checks the uses of that ability against its cooldown (RateSlack). */
	void AddUse(FTrack& Track, int32 Slot, const FString& Name, EPinguTrait Trait, EPenguinAbility Ability, double ServerAt, double ClientNow);

	TMap<int32, FTrack> Tracks;
	TArray<FPendingSwap> PendingSwaps;
	TArray<FGone> GonePickups;
	TArray<FPending> Pendings;
	/** Hits on anybody (server clock, as they arrived). */
	TArray<double> HitTimes;
	/** Rounds whose reveal came in this match; the round whose reveal we wait for. */
	TSet<int32> RevealedRounds;
	int32 RevealWanted = -1;
	bool bDevScript = false;
	TMap<FString, double> LastStrike;
	int32 Round = -1;
	bool bFight = false;
	float FightAge = 0.f;
	int32 ExpectedRound = -1;
	FPinguFairDeal Expected;
	int32 Points = 0;
	TArray<FString> Strikes;
	int32 DealsVerified = 0;
	int32 UsesJudged = 0;
	int32 SamplesJudged = 0;
};

namespace PinguAntiCheat
{
	// ------------------------------------------------------------------ dev cheat switches (always false in Shipping)
	/** -PinguCheat=picks: the host gives its own penguin a loadout of its choice after the fair deal. */
	PENGUIN_API bool IsHostPickCheat();
	/** -PinguCheat=cooldown: the host's own penguin ignores its cooldowns. */
	PENGUIN_API bool IsHostCooldownCheat(const APenguinCharacter* Penguin);
	/** -PinguCheat=client: this joiner spams illegal ability requests and teleports its penguin in fights. */
	PENGUIN_API bool IsClientCheat();
	/** Any -PinguCheat at all (the net test's cheat runs). */
	PENGUIN_API bool IsAnyCheat();
	/** -PinguCheat=trainer: the host does what PinguTrainer.exe did (PinguMemoryGuard::DevTrainerWrite every 50 ms). */
	PENGUIN_API bool IsDevTrainer();
	/** -PinguCheat=noguard: the host's guards against memory writes are off (the joiners' judge must catch the trainer). */
	PENGUIN_API bool IsDevNoGuard();

	// ------------------------------------------------------------------ host: the joiners' requests
	enum class EGate : uint8
	{
		Ability,  // presses, releases, charges, roll-ups
		Aim,      // the unreliable aim updates
		Lobby,    // team, traits, continue, respawn
		Fair,     // seed contributions
		Count
	};
	/** Host: a request of a joiner's (Who: its penguin or controller) within its rate; false = ignore it (logged). */
	PENGUIN_API bool AllowRequest(const UObject* Who, EGate Gate);
	/** Host: logs an ignored request (throttled per joiner) and counts it. */
	PENGUIN_API void NoteIgnored(const UObject* Who, const TCHAR* What, const FString& Detail);
	/** Host: requests ignored as illegal since the start (the net test's numbers): all of them, and teleports / speed. */
	PENGUIN_API int32 GetIgnoredCount();
	PENGUIN_API int32 GetIgnoredMoves();
	/** Host: every ignored request so far, "<who>: <what> (<why>)" (the first 20; unthrottled - the net test shows them). */
	PENGUIN_API const TArray<FString>& GetIgnoredLines();
	/** Host: an icicle hold a joiner claims, cut to what the host saw (with lag slack). */
	PENGUIN_API float ClampHold(const UObject* Who, float ClaimedSeconds, double ServerSeenSeconds);
	/**
	 * Host: a joiner's move from ServerLoc to ClientLoc (bWalking: the joiner says it is on the ground). bMovedByHost: the host
	 * has just moved it (an effect on its way to the owner, a guard, a knock-out): not judged. bPushed: the host pushed it in the
	 * last seconds (a knockback flight may be fast): only a jump three times as far counts. False = keep the host's position.
	 * Sets bOutPullBack when the joiner has been refused long enough that the host should move it back (NetTeleport).
	 */
	PENGUIN_API bool AcceptMove(const UObject* Who, const FVector& ServerLoc, const FVector& ClientLoc, bool bWalking, bool bMovedByHost,
		bool bPushed, double Now, bool& bOutPullBack);

	// ------------------------------------------------------------------ the join handshake
	/** "<engine version>|<config>|<anti-cheat version>": a joiner with another build id doesn't play. */
	PENGUIN_API FString GetBuildId();
	/** Starts hashing the game's files in the background (once). */
	PENGUIN_API void StartFileDigest();
	/** A stretch of one hashed file the answer to a nonce must cover (File = index in the files sorted by name). */
	struct FFileRange
	{
		int32 File = 0;
		int64 Offset = 0;
		int64 Length = 0;
	};
	/** The ranges a nonce asks for: a few random 64-256 KB stretches, picked (weighted by size) from files of these sizes.
	 *  Both machines derive the same ones from the nonce alone (FRandomStream seeded by its hash), so the challenger picks them
	 *  by picking the nonce. */
	PENGUIN_API TArray<FFileRange> DeriveRanges(const FString& Nonce, const TArray<int64>& FileSizes);
	/** The answer to a nonce: SHA-1 hex of nonce, build id, the files' digest and the hashes of the ranges of the files the
	 *  nonce asks for (read from disk for each new nonce). False while the digest is not ready. */
	PENGUIN_API bool AnswerNonce(const FString& Nonce, FString& OutAnswer);
	/** What was hashed (for the log). */
	PENGUIN_API FString DescribeDigest();

	/** Host, from the controller's RPC: a joiner's answer to our hello. */
	PENGUIN_API void HostOnAnswer(APlayerController* Joiner, const FString& Answer, const FString& JoinerNonce, const FString& JoinerBuildId);
	/** Joiner, from the controller's RPCs: the host's hello / its answer / its "no". */
	PENGUIN_API void ClientOnHello(APlayerController* Own, const FString& HostNonce, const FString& HostBuildId);
	PENGUIN_API void ClientOnHostAnswer(APlayerController* Own, const FString& Answer);
	PENGUIN_API void ClientOnRefused(APlayerController* Own);
	/** Joiner, every frame (APenguinPlayerController::PlayerTick): answers a hello once the digest is ready, gives up on a host
	 *  that never says hello. */
	PENGUIN_API void TickClientHandshake(APlayerController* Own, float DeltaTime);
	/** Joiner: leaves the game with the neutral message ("Your games don't match."), logging why. */
	PENGUIN_API void LeaveQuietly(const UObject* WorldContext, const FString& Why);

	// ------------------------------------------------------------------ the net test's numbers (this process)
	struct FClientReport
	{
		bool bHandshakeOk = false;
		bool bLeft = false;
		FString LeftWhy;
		int32 Points = 0;
		int32 DealsVerified = 0;
		int32 UsesJudged = 0;
		int32 SamplesJudged = 0;
		int32 Contributions = 0;
		TArray<FString> Strikes;
	};
	PENGUIN_API const FClientReport& GetClientReport();
	/** Host: joiners whose handshake passed / failed. */
	PENGUIN_API int32 GetHandshakesOk();
	PENGUIN_API int32 GetHandshakesFailed();
}

/**
 * The anti-cheat's actor in a LAN game (the host spawns it: UPinguNetSubsystem when a listen server's world begins play; it
 * replicates to every joiner). Host: the commits and reveals, the joiners' contributions, the hellos. Joiner: contributes,
 * checks every reveal and deal, runs the judge, leaves on a clear repeated violation.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguAntiCheat : public AInfo
{
	GENERATED_BODY()

public:
	APinguAntiCheat();

	static APinguAntiCheat* Get(const UWorld* World);
	/** Host: the one of this world (spawned if needed). Null in a solo game. */
	static APinguAntiCheat* GetOrSpawn(UWorld* World);
	/** A joiner left or was kicked: its seed value goes out of the reveal (a joiner arriving later never saw it and would take
	 *  it for a value under "no joiner": 2026-11-07, found by the kick-and-rejoin LAN test). */
	void HostForgetPlayer(int32 Player);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void Tick(float DeltaSeconds) override;

	/**
	 * Host, a new round's setup in a LAN game: the deal from the current commit and the contributions that came for it (only
	 * of anybody), the reveal published, the next commit made. InOutBag: the game mode's trait bag (updated).
	 * Forced: the game mode's forced traits.
	 */
	static bool HostDeal(UWorld* World, int32 Round, const TArray<uint8>& Slots, bool bTraits, const TArray<EPinguTrait>& Forced,
		uint64 TraitsOff, TArray<EPinguTrait>& InOutBag, FPinguFairDeal& OutDeal, bool bPicks = false);
	/** Host: a joiner's value for the commit Serial (from its controller's RPC). */
	void HostReceive(APlayerController* From, int32 Serial, const FString& Value);
	/** Host, the net test only: its own script moves penguins about (the holds): the joiners don't judge movement for Seconds.
	 *  A Shipping joiner never honours it. */
	static void DevQuietMotion(UWorld* World, float Seconds);
	/** Host, the net test only: its own script hands out abilities or throws things (PinguNetTestGrant): the joiners don't judge
	 *  loadouts and shots for Seconds. A Shipping joiner never honours it. */
	static void DevQuietScript(UWorld* World, float Seconds);
#if !UE_BUILD_SHIPPING
	/** Tests (-PinguMatchTest, the anti-cheat steps): this world's own penguins are judged by a joiner's judge, fed from the
	 *  server's state, so honest bot matches show the judge finds nothing (and the trainer test that it does). */
	static APinguAntiCheat* StartSelfJudge(UWorld* World);
	static APinguAntiCheat* GetSelfJudge(const UWorld* World);
	/** The self judge: the expected loadouts of the running round are the ones the penguins had when it began. */
	bool IsSelfJudge() const { return bSelfJudge; }
#endif

	const FPinguFairCommit& GetCommit() const { return Commit; }
	const FPinguFairReveal& GetReveal() const { return Reveal; }
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** The joiner's judge (tests and the net test read it). */
	const FPinguCheatJudge& GetJudge() const { return Judge; }

private:
	void HostMakeCommit(int32 Serial);
	void HostTick();
	void ClientTick(float DeltaSeconds);
	/** Joiner: a new reveal arrived: check it and expect its deal. */
	void ClientCheckReveal();
	void ClientWatchPenguins(double ClientNow, double ServerNow);
	void ClientDevCheat(float DeltaSeconds);
	/** Joiner: the people in the game (for the reveal's values) and the match's rounds (for re-rolls and missing reveals). */
	void ClientWatchGame(double Now);
	/** Feeds the shots (new projectiles, snowmen, giant snowballs) to the judge. */
	void ClientWatchShots(double ClientNow, double ServerNow);
	void SelfJudgeTick();

	// The reflected names are meaningless on purpose (2026-11-05, card host-cheat-fix; the readable names are references).
	UPROPERTY(Replicated)
	FPinguFairCommit Pc;
	FPinguFairCommit& Commit = Pc;

	UPROPERTY(Replicated)
	FPinguFairReveal Pr;
	FPinguFairReveal& Reveal = Pr;

	/** The contributions for the current commit so far (every joiner sees its own arrived). */
	UPROPERTY(Replicated)
	TArray<FPinguFairContribution> Pv;
	TArray<FPinguFairContribution>& Received = Pv;

	/** The host's own penguin's slot (-1: none): joiners judge its movement. */
	UPROPERTY(Replicated)
	int32 HostSlot = -1;

	/** The net test's holds (DevQuietMotion): no movement judged before this server time. Ignored in Shipping. */
	UPROPERTY(Replicated)
	float DevQuietUntil = -1.f;

	/** The net test's own grants and throws (DevQuietScript): no loadout / shot judged before this server time. Ignored in
	 *  Shipping. */
	UPROPERTY(Replicated)
	float DevScriptUntil = -1.f;

	// host
	FString Secret;

	// joiner
	struct FSeenCommit
	{
		FString Hash;
		double SeenAt = 0.0;
	};
	TMap<int32, FSeenCommit> SeenCommits;
	TMap<int32, TPair<FString, double>> SentValues; // serial -> our value, when it went out
	TSet<int32> AckedSerials;
	int32 LastCommitSerial = 0;
	int32 LastRevealSerial = 0;
	FPinguCheatJudge Judge;
	struct FSeenPickup
	{
		EPenguinAbility Ability = EPenguinAbility::None;
		FVector Where = FVector::ZeroVector;
	};
	TMap<TWeakObjectPtr<AActor>, FSeenPickup> SeenPickups;
	/** Shots (projectiles, snowmen, giant snowballs) seen so far; false until the first look (what was there then is old). */
	TSet<TWeakObjectPtr<AActor>> SeenShots;
	bool bShotsPrimed = false;
	/** Giant snowballs (Slot -1) and snowmen (their builder's slot) of the last moment: what they throw is no use. */
	struct FRecentSource
	{
		FVector Where = FVector::ZeroVector;
		float Radius = 0.f;
		int32 Slot = -1;
		double At = 0.0;
	};
	TArray<FRecentSource> RecentSources;
	/** Joiners' PlayerIds seen in this game; Received as seen per commit serial. */
	TSet<int32> KnownJoiners;
	TSet<int32> PresentJoiners;
	TMap<int32, TMap<int32, FString>> SeenReceived;
	/** The match as this joiner saw it: rounds dealt, the last reveal's round and bag after, the round number and phase. */
	TSet<int32> RoundsDealt;
	int32 PrevDealtRound = -1;
	bool bPrevDealtTraits = false;
	TArray<uint8> PrevBagAfter;
	/** Ability picks (2026-11-07): the match's CHOOSE and picks by slot from its reveals; when the setting was last seen
	 *  RANDOM / CHOOSE (client clock). */
	int32 MatchChoose = -1;
	TMap<int32, TArray<uint8>> MatchPicks;
	double ChooseOffSeenAt = -100.0;
	double ChooseOnSeenAt = -100.0;
	int32 LastRoundSeen = -1;
	uint8 LastPhaseSeen = 255;
	/** A new commit waits for the reveal of the one before (IsCommitStepFine), at most a moment. */
	int32 CommitStepFrom = 0;
	int32 CommitStepTo = 0;
	double CommitStepSince = -1.0;
	bool bSelfJudge = false;
	/** Self judge: the round whose loadouts it took as the deal; the guards' restores seen. */
	int32 SelfRound = -1;
	int32 SelfRestores = 0;
	/** A watched penguin's last motion sample: the host's transform time stamp, the replicated position, whether the stamp was
	 *  ever seen to move (else samples are timed by their arrival on the host's clock). */
	struct FSeenMotion
	{
		float Stamp = 0.f;
		FVector Location = FVector::ZeroVector;
		bool bStampMoves = false;
		bool bLogged = false;
	};
	TMap<TWeakObjectPtr<AActor>, FSeenMotion> SeenStamps;
	float FightStartedAt = -1.f;
	int32 WatchedRound = -1;
	bool bLeaving = false;
	float DevCheatClock = 0.f;
	float DevTeleportClock = 0.f;
};

// Pingu - the host's say over a LAN joiner's own penguin (card 2026-10-30_host-authority; docs/FEATURES.md 7.1).
//
// The LAN model lets every player's machine move its own penguin (no lag for the player). The host still decides:
//  - knockback, freeze, stun, a tornado's pull or hover and the belly stomp: while one lasts the host moves the penguin itself
//    (UPenguinMovementComponent::HostSimulate) and takes the joiner's position again only when it is on the host's path;
//  - every other move of the joiner's is judged here first (FPinguMoveJudge): no rising higher than its jumps can take it, no
//    hanging in the air, no running faster than it can; Y is always 0 and the host decides walking or falling;
//  - belly slides (FPinguSlideGate: the host's own cooldown) and landings (FPinguLandGate: only after the host saw it in the air)
//    are requests; the stomp's landing is the host's own (its simulation of the stomp).
// A refused move: the host keeps its own copy and simulates it (it falls); after a while the joiner is put back where the host
// has it. Pure logic here (no world): the movement component feeds it, the autotest feeds it made-up moves.

#pragma once

#include "CoreMinimal.h"

/** The joiner's penguin's own limits right now (its tuning, size and trait: UPenguinMovementComponent::GetHostMoveLimits). */
struct PENGUIN_API FPinguMoveLimits
{
	/** cm/s^2 of gravity rising out of a jump, and its share at the top of a held jump (slower than ApexSpeed cm/s). */
	float Gravity = 4475.f;
	float ApexGravityScale = 0.5f;
	float ApexSpeed = 220.f;
	/** cm/s: the host's idea of the penguin never sinks faster than this (slower than any real fall: lenient). */
	float SlowestFall = 650.f;
	/** cm one push of its own lifts it at most (a jump, an air jump, an air dash, a ledge climb), with room. */
	float CreditHeight = 400.f;
	/** How many of those it has in one time in the air (air jumps + air dashes + the ground jump + a ledge climb). */
	int32 Credits = 4;
	/** cm/s a jump pad throws it up. */
	float PadSpeed = 2970.f;
	/** Rubber: the share of the height fallen that a landing bounces back up (0 without the trait). */
	float BounceShare = 0.f;
	/** cm/s: the fastest it can go sideways by itself (run, dash, slide), with room. */
	float SideSpeed = 2500.f;
};

/** One move of the joiner's as the host sees it. */
struct PENGUIN_API FPinguMoveSample
{
	/** The host's clock (s). */
	double Now = 0.0;
	/** The joiner's position (Y 0). */
	FVector Location = FVector::ZeroVector;
	/** The host found walkable ground right under it (or a wall beside Sticky). */
	bool bSupported = false;
	/** The host knows of something that lifts or floats it right now (low gravity, an ice shell's float, Second Chance's
	 *  pass through the island): its height is not judged. */
	bool bFreeVertical = false;
	/** A jump pad (a stuck icicle) is right there. */
	bool bNearPad = false;
};

/**
 * The host's ballistic idea of a joiner's penguin: a "ceiling" that rises like a jump and falls like gravity. Standing on the
 * ground resets it; each of the penguin's own lifts (a jump, an air jump, an air dash, a climb: Credits per time in the air,
 * a jump pad any time) starts a new arc from where the ceiling was; the host's pushes lift it too. A joiner above the ceiling
 * (a jump higher than its jumps, hanging in the air, gravity 0) is refused. Sideways: its average speed over the last
 * SideWindow seconds, at most its own fastest (SideSpeed) or what the host's pushes gave it (a budget that wears off).
 */
class PENGUIN_API FPinguMoveJudge
{
public:
	/** s: moves arrive late (the network), so the ceiling of this long ago counts too. */
	static constexpr double LagSlack = 0.15;
	/** cm above the ceiling that still pass. */
	static constexpr float RiseSlack = 60.f;
	/** Sideways: the speed is judged over this long (at least SideMinSpan), with SideSlack cm of room. */
	static constexpr double SideWindow = 0.5;
	static constexpr double SideMinSpan = 0.25;
	static constexpr float SideSlack = 200.f;
	/** cm/s^2: a push's speed budget wears off this fast (slower than any real slow-down). */
	static constexpr float BudgetDecay = 250.f;
	/** s: lifts this close together count as one combo (from the first one's place, ComboHeightScale x the height). */
	static constexpr double ComboTime = 0.25;
	static constexpr float ComboHeightScale = 1.5f;

	void Reset();
	/** The host has the penguin (it moved or simulated it): the judge goes on from the host's state. */
	void FollowHost(double Now, const FVector& Location, const FVector& Velocity, bool bSupported, const FPinguMoveLimits& Limits);
	/**
	 * A push the host gave it: Velocity is the new velocity (bReplace: a knockback, a launch, a teleport) or what was added
	 * (an ability's push). Lifts the ceiling, widens the sideways budget; bRefresh gives the air moves back (a knockback does).
	 */
	void NoteHostPush(double Now, const FVector& Location, const FVector& Velocity, bool bReplace, bool bRefresh, const FPinguMoveLimits& Limits);
	/** The sideways window starts again (a teleport: the jump is no speed). */
	void ForgetPath();
	/** A joiner's move: true = take it, else OutWhy says why not. Only taken moves change the judge's idea of the penguin. */
	bool Judge(const FPinguMoveSample& Move, const FPinguMoveLimits& Limits, FString& OutWhy);

	/** The highest Z it may have right now (with the slack): tests and the log. */
	float GetAllowedZ() const;
	int32 GetCreditsLeft() const { return Credits; }
	float GetSideBudget() const { return SideBudget; }
	bool HasState() const { return bHave; }

private:
	struct FStamp
	{
		double Time = 0.0;
		float Value = 0.f;
	};
	/** Runs the ceiling on to Now (gravity, the slowest fall) and wears off the push budget. */
	void Advance(double Now, const FPinguMoveLimits& Limits);
	/** Starts a new arc from Level upward at Speed (as it is LagSlack seconds later: the joiner's move is that late). */
	void StartArc(double Now, float Level, float Speed, const FPinguMoveLimits& Limits);
	void Stamp(double Now);
	static float CreditSpeed(const FPinguMoveLimits& Limits);

	bool bHave = false;
	double LastTime = 0.0;
	/** The ceiling (cm) and its upward speed (cm/s). */
	float CeilZ = 0.f;
	float CeilV = 0.f;
	int32 Credits = 0;
	/** The highest it got since it last stood (Rubber's bounce off the next landing). */
	float TopZ = 0.f;
	/** Rubber: the speed of the bounce of its last landing, good until BounceUntil. */
	float BounceSpeed = 0.f;
	double BounceUntil = -1.0;
	/** When and from where the last lift's arc began: lifts within ComboTime of it (a jump and an air dash at once) start from
	 *  that same place, a bit faster - chained lifts never stack their lag slack. */
	double LastArcTime = -100.0;
	float LastArcLevel = 0.f;
	/** cm/s sideways the host's pushes allow on top of its own. */
	float SideBudget = 0.f;
	TArray<FStamp, TInlineAllocator<32>> CeilingHistory;
	TArray<FStamp, TInlineAllocator<64>> SidePath;
};

/** The host's own belly-slide cooldown: a joiner's new slide counts only if the last one began long enough ago. */
struct PENGUIN_API FPinguSlideGate
{
	/** s of network jitter the cooldown forgives. */
	static constexpr double Slack = 0.2;

	/** A joiner says it began a slide. True = it counts (a new slide on the host: it may hit once). */
	bool Request(double Now, bool bCanSlide, float Cooldown);
	void Reset() { LastStart = -100.0; }

	double LastStart = -100.0;
	int32 Accepted = 0;
	int32 Refused = 0;
};

/** A joiner's landing counts (the Giant's ice breaking reads it) only after the host saw the penguin in the air. */
struct PENGUIN_API FPinguLandGate
{
	/** Every position the host takes: in the air (not supported) arms the next landing. */
	void Observe(float Z, bool bSupported);
	/** A joiner says it landed at ClaimedSpeed cm/s, now at Z. True = it counts, with OutSpeed (at most what the fall gives). */
	bool Request(float Z, float ClaimedSpeed, const FPinguMoveLimits& Limits, float FastFallGravityScale, float FastFallStartSpeed, float& OutSpeed);
	/** The host's own simulation landed it. */
	void NoteLanded() { bSawAir = false; }

	bool bSawAir = false;
	float TopZ = 0.f;
	int32 Refused = 0;
};

namespace PinguHostAuthority
{
	/** What the host did about its joiners' movement since the start (the net test's numbers, the log). */
	struct FStats
	{
		/** Moves refused by the judge (FPinguMoveJudge) or the old gate (PinguAntiCheat::AcceptMove). */
		int32 Refused = 0;
		/** Moves of a penguin the host had taken over (refused moves) that were still off the host's path: ignored too. */
		int32 IgnoredWhileTaken = 0;
		/** Times the host took a joiner's penguin over after refused moves, and put it back where the host had it. */
		int32 Penalties = 0;
		int32 PullBacks = 0;
		/** Host-owned stretches (a knockback, a freeze ...) handed back, and those where the joiner was not on the host's path. */
		int32 Handovers = 0;
		int32 HandoverMisses = 0;
		/** Joiners that went quiet (no moves) and were moved by the host meanwhile. */
		int32 Silences = 0;
		/** Moves whose Y was not 0. */
		int32 YClamped = 0;
		/** Belly slides, landings and stomp landings a joiner claimed that the host did not count. */
		int32 SlidesRefused = 0;
		int32 LandsRefused = 0;
		int32 StompClaimsIgnored = 0;
		int32 SlidesAccepted = 0;
		/** Real time (FPlatformTime) of the first joiner move judged: corrections per minute. */
		double FirstMoveTime = -1.0;
	};
	PENGUIN_API FStats& Stats();
	/** Host corrections a player would feel (refused moves, pull-backs, missed handovers) per minute of joiner moves. */
	PENGUIN_API float CorrectionsPerMinute();
	/** One line for the log / the net test. */
	PENGUIN_API FString DescribeStats();

	/**
	 * Dev builds only (always false in Shipping): -PinguCheat=<list> switches that make this joiner cheat with its movement,
	 * for the net test: fly, speed, noknock, slide, stomp, giant, noack, fakey (see docs/FEATURES.md 7.1).
	 */
	PENGUIN_API bool HasDevCheat(const TCHAR* Name);
}

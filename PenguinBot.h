// Pingu - computer-controlled penguins.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Controller.h"
#include "PenguinTypes.h"
#include "PenguinBot.generated.h"

class ABlockIsland;
class APenguinCharacter;
class APenguinProjectile;
class APinguPickup;
class APinguPortal;

/** Possesses a bot penguin (so it is not player controlled). The brain is UPenguinBotComponent. */
UCLASS(NotBlueprintable)
class PENGUIN_API APenguinBotController : public AController
{
	GENERATED_BODY()

public:
	APenguinBotController();
	virtual void OnPossess(APawn* InPawn) override;
};

/**
 * The bot brain. Lives on every penguin, switched on for bots (and for the player's penguin in the
 * menu demo). It plays with the same inputs a player has: move, jump, dash, aim, abilities.
 *
 *  - Picks the nearest opponent, keeps a distance that suits its three abilities.
 *  - Goes for ability bubbles (pickups) that land close by, unless they are over the sea or on crumbling ground.
 *  - Reads the block grid: jumps walls and gaps, won't walk off edges, drops down to chase.
 *  - Belly-slides through gaps one block high (standing up it doesn't fit) and crawls on until it is out; a dead end it
 *    jumps over or turns away from.
 *  - Portals (Snow Warp): it knows where each end leads. Goes through one that takes it closer to where it is going (or,
 *    knocked off the island, back onto it), jumps over the others on the ground and steers clear of them in the air -
 *    never one whose far end is over the sea, at a deadly edge, or straight above its entry (it would fall back in for
 *    ever). Two trips through the same portal within PortalLoopWindow s: that portal is left alone for a while.
 *  - Never stands around (2026-11-07): AggroIdleTime s without landing a hit, being hit or using an attacking ability
 *    and it goes in - the nearest reachable enemy (the leader preferred), close (melee, short range, or the belly slide's
 *    distance), every ability more eagerly - until it lands a hit or a few seconds pass. Every safety check stays on.
 *    Host-side only (a LAN client's brain - the net test - plays as before).
 *  - Recovers when knocked off the island (double jump, air dash, warp / hook / ice ledge).
 *  - The snow bomb knocks out its own thrower too: it throws one only where the arc ends outside its own blast, and runs
 *    out of the blast of every live bomb (its own at once, others' after its reaction time), through it when the island
 *    ends the other way, with a dash when the run is too slow and the ice shell when there is no way out.
 *  - Uses abilities with lead and some aim error, dodges projectiles and icebergs (everybody's but its own: friendly fire
 *    is on, so a teammate's shots are dodged too; teammates are never targets).
 *  - Sizes (shrink / grow rays, 0.5x .. 8x): heights are judged from where a normal penguin's middle would be, melee
 *    ranges and walls it jumps grow with it; it never rays itself past a size limit, and grows an enemy only when that
 *    pops it (at 8x already).
 *  - Traits mode: plays its trait (UPenguinTraitComponent) - never an empty slot (Second Chance), its grip when it judges
 *    how far it needs to stop (Speedster also goes slower toward edges), gaps and walls by its jumps (Heavyweight and Giant
 *    leave long gaps alone), no slide (Sticky; it sticks to the island's wall and wall-jumps instead, never hangs on a
 *    wall for long: a jump when its way is into the wall, else it lets go), no ledge grab and
 *    no jump presses while bouncing (Rubber), off a melting block in time (Hot Feet), closer (Ice Touch), at range and
 *    dodging more (Glass Cannon), dodging shots less while its Mirror is ready, a calmer recovery while its fish is still
 *    to come (Second Chance). Without a trait it plays exactly as before.
 * Difficulty 0 = easy, 1 = normal, 2 = hard (reaction time, aim, aggression, dodging).
 *
 * LAN: the brain only drives a penguin this machine simulates (APenguinCharacter::IsLocallySimulated): the server's bots
 * (and the host's own penguin), the solo menu demo, and a LAN client's own penguin when switched on there (the net test).
 * On a client it plays through the owner's normal paths: movement input into its own simulation, abilities through
 * ActivateAbility (the server runs them). It never runs for another machine's penguin.
 */
UCLASS(ClassGroup = (Penguin))
class PENGUIN_API UPenguinBotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPenguinBotComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Switches the brain on or off on this machine (it thinks only while the penguin is simulated here). */
	void SetBotEnabled(bool bEnabled, int32 InDifficulty = 1);
	/** 0 easy, 1 normal, 2 hard (as last enabled). */
	int32 GetDifficulty() const { return Difficulty; }
	bool IsBotEnabled() const { return bBotEnabled; }

	/** Seconds without a fight (no hit landed, none taken, no attacking ability used) after which the bot goes in. */
	static constexpr float AggroIdleTime = 15.f;
	/** Going in right now (tests, diagnostics). */
	bool IsAggressive() const { return bAggressive; }
	/** Seconds since its last fight (0 while it goes in). */
	float GetIdleTime() const { return IdleTime; }
	/** How often it went in since its brain was switched on. */
	int32 GetAggroCount() const { return AggroCount; }
	/** Trips through portals since its brain was switched on. */
	int32 GetPortalTrips() const { return PortalTrips; }

	/** Diagnostics (every bot on this machine): how the looks for a belly slide attack went - a target, standing ready,
	 *  in range, on our level, the dice, open ground (= slid). Logged and cleared. */
	static void LogSlideStats(const TCHAR* Label);

	/** A snow bomb thrown at Aim by Self (the same arc the ability throws) goes off far enough from it: outside its own push
	 *  radius (what its trait makes of it, and some margin for the roll), and it is not dropped at its feet. The bots ask it
	 *  before every bomb; the tests ask it too (it needs no brain switched on). */
	bool BombThrowIsSafe(const APenguinCharacter* Self, const ABlockIsland* Island, const FVector& Aim) const;

private:
	struct FSkill
	{
		float Reaction = 0.28f;
		float AimError = 9.f;
		float Aggression = 0.6f;
		float Dodge = 0.45f;
		float Recovery = 0.95f;
		bool bLead = true;
	};

	/** A gap one block high ahead of us (FindLowGapAhead). */
	struct FLowGap
	{
		/** cm from the front of our capsule to the ceiling's near face (0 or less: we touch it). */
		float Distance = 0.f;
		/** It leads out to room to stand (no dead end, and the slide never goes on into the sea): a belly slide gets us through. */
		bool bPassable = false;
		/** Columns under the ceiling up to where there is room to stand again. */
		int32 Length = 0;
		/** Jumping over it instead: rows from our feet row to the top of the ceiling's stack (like a wall that high). */
		int32 OverHeight = 0;
	};

	APenguinCharacter* GetPenguin() const;
	void Think(APenguinCharacter* Self, ABlockIsland* Island, float Dt);
	void ChooseTarget(APenguinCharacter* Self);
	bool IsOffStage(const APenguinCharacter* Self, const ABlockIsland* Island) const;
	void Recover(APenguinCharacter* Self, ABlockIsland* Island);
	void Navigate(APenguinCharacter* Self, ABlockIsland* Island, float Dt);
	bool UseAbilities(APenguinCharacter* Self, ABlockIsland* Island);
	bool UseRecoveryAbility(APenguinCharacter* Self, const ABlockIsland* Island, const FVector& Goal);
	bool Dodge(APenguinCharacter* Self, ABlockIsland* Island);
	/** Ice walls stay until broken: an ice block in our way (too high to jump, or we are stuck against it) gets broken
	 *  with whatever ability can. True if one was used. */
	bool BreakIceAhead(APenguinCharacter* Self, const ABlockIsland* Island, float Direction);
	/** Fire an ability that breaks ice at an ice block (centre IceSpot). True if one was used. */
	bool FireAtIce(APenguinCharacter* Self, const FVector& IceSpot);
	/** Falling sand ice (an iceberg's column dropping) right above us: step out from under it. */
	bool AvoidFallingIce(APenguinCharacter* Self, const ABlockIsland* Island);
	/** A giant snowball rolling at us (APinguGiantSnowball): jump over it (twice for a big one) toward its far side; one rolling
	 *  away from us in front: keep back. True if it acted. */
	bool AvoidGiantSnowball(APenguinCharacter* Self, const ABlockIsland* Island);
	/** Using our giant snowball is worth it now: on the ground, the target on about our level in front of us, no wall right
	 *  in the way. */
	bool GiantSnowballWorthIt(const APenguinCharacter* Self, const APenguinCharacter* Target, const ABlockIsland* Island) const;

	// ---- the snow bomb (its blast knocks out and pushes its own thrower too)
	/** Where a bomb that flies on from From with Velocity (cm/s, gravity Gravity, radius Radius) goes off: where it first touches
	 *  the island or a penguin (never Ignore: its thrower), or - Fuse seconds from now - wherever it is by then (or the sea
	 *  under it). A bomb at rest stays where it is. The bounces and the roll after the first touch are not followed. */
	FVector PredictBombEnd(const ABlockIsland* Island, const APenguinCharacter* Ignore, const FVector& From, const FVector& Velocity,
		float Gravity, float Radius, float Fuse) const;
	/** A live bomb (anybody's, ours too) is about to go off with us in its blast: Desired is the way out of it (away from it, or
	 *  through it and out the far side when the island ends that way), a dash when the run is too slow, the ice shell when
	 *  there is no way out. True if it set Desired or acted. Sets bFleeingBlast. */
	bool AvoidBlast(APenguinCharacter* Self, const ABlockIsland* Island, float& Desired);
	/** How far (cm, up to MaxDistance) we can run Direction (+1 / -1) before a wall too high to jump or a gap too wide to cross. */
	float FreeRunDistance(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction, float MaxDistance) const;
	/** The target stands on our level the right distance away (the slide has gone its three blocks when it gets there)
	 *  over open ground: a belly slide into it. True if we slid. */
	bool TrySlideAttack(APenguinCharacter* Self, const ABlockIsland* Island);
	bool HasLineOfSight(const FVector& From, const FVector& To) const;
	/** The line of sight is blocked by an ice block (we can shoot it away): its centre. */
	bool IceBlocksSight(const ABlockIsland* Island, const FVector& From, const FVector& To, FVector& OutIceSpot) const;
	FVector Lead(const APenguinCharacter* Target, const FVector& From, float ProjectileSpeed, float Gravity) const;
	FVector ApplyAimError(const FVector& From, const FVector& To) const;
	/**
	 * Is there an edge within Lookahead cm (or already under our feet)? OutGapCells: >0 jumpable gap to a floor at our
	 * height (in cells), -1 a lower floor to drop onto, 0 nothing. OutLanding: the floor cell across the gap.
	 */
	bool IsEdgeAhead(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction, float& OutGapCells,
		float Lookahead = 45.f, FIntPoint* OutLanding = nullptr) const;
	/** How far ahead we must look to stop (or turn) before an edge at our current speed. */
	float StopLookahead(const APenguinCharacter* Self, float Direction) const;
	int32 WallHeightAhead(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction) const;
	float PreferredRange(const APenguinCharacter* Self) const;
	/** Which kinds of attack we have ready right now (close: slap, frost breath, tornado, stomp, noot; from range: the shots,
	 *  bomb, hook, iceberg, rays). */
	void ReadyAttacks(const APenguinCharacter* Self, bool& bOutMelee, bool& bOutRanged) const;

	// ---- going in after standing around (AggroIdleTime)
	/** Every think: notices fights (a hit landed or taken), counts the idle time, starts and ends going in. Server only. */
	void UpdateAggro(APenguinCharacter* Self, const ABlockIsland* Island);
	/** A fight happened: the idle time starts over; a hit we landed also ends going in. */
	void NoteFight(bool bLandedHit);
	/** Going in: the nearest enemy we can reach (alive, over ground, not at a deadly edge), the one leading the match
	 *  preferred. Null if there is none. */
	APenguinCharacter* ChooseAggroTarget(const APenguinCharacter* Self, const ABlockIsland* Island) const;
	/** Going in: the distance we close to (melee, short range, or the belly slide's), and whether only the slide attacks. */
	float AggroRange(const APenguinCharacter* Self, bool& bOutSlideOnly) const;
	/** The chance to use an ability / slide when we look (higher while we go in; it stays below 1 on easy). */
	float CurrentAggression() const;
	/** The ability bubble worth a detour right now (close, reachable, over ground, not crumbling), or null. */
	const APinguPickup* ChoosePickup(const APenguinCharacter* Self, const ABlockIsland* Island);
	void Jump(float HoldTime);
	/**
	 * Uses an ability at Aim. The icicle charges first (UPenguinAbilityComponent::ChargeAndThrow) and is let go after
	 * ChargeHold seconds (< 0: a full charge): aimed at Aim - or, with bLeadTarget, at the target as it moves (Think keeps
	 * the aim on it, led for that charge's flight).
	 */
	bool FireAbility(APenguinCharacter* Self, EPenguinAbility Ability, const FVector& Aim, float ChargeHold = -1.f, bool bLeadTarget = false);
	/** Where to aim an icicle of this charge (0..1) at T: led for its speed and drop, and lower by its lob's lift. */
	FVector IcicleAim(const APenguinCharacter* Self, const APenguinCharacter* T, float Power) const;
	/** The same for a spot that does not move (a cliff face): higher by the drop of the flight, lower by the lift. */
	FVector IcicleAimAt(const APenguinCharacter* Self, const FVector& Where, float Power) const;
	/** How much to charge an icicle for a target this far away: a short hold at point blank, a real hold from there, a full
	 *  charge (the fastest, straight one) from far enough (a tap is only a weak 2 m lob). */
	float IcicleCharge(float Distance);

	/** A gap one block high (the floor ahead, a ceiling right above our feet row) within a few blocks the way we go. False
	 *  if there is none (or a wall or an edge comes first). Standing up we don't fit in; on the belly we do. A grown penguin
	 *  (taller than two blocks) finds any ceiling lower than itself, and for it that is never passable (over it or away). */
	bool FindLowGapAhead(const APenguinCharacter* Self, const ABlockIsland* Island, float Direction, FLowGap& OutGap) const;
	/** Belly slide into the gap ahead (or commit to the slide we are in) and keep going that way until we are out. False if
	 *  we can't slide right now (cooldown, in the air, no control). */
	bool StartLowPassage(APenguinCharacter* Self, const ABlockIsland* Island, float Direction, const FLowGap& Gap);
	/** Under (or into) a ceiling too low to stand up: owns the stick until we are out (true while it does). */
	bool UpdateLowPassage(APenguinCharacter* Self, const ABlockIsland* Island, float Dt);
	/** Lying under a low ceiling: the way out (+1 / -1), the Preferred way if that one leads out; 0 if neither does.
	 *  OutCells: columns to the room to stand (as tall as we are). */
	float CrawlDirection(const APenguinCharacter* Self, const ABlockIsland* Island, float Preferred, int32& OutCells) const;

	/** Notices when we went through a portal (our place jumped from one end into the other since the last think): drops what
	 *  we were committed to, and two trips close together (a loop) keep us out of every portal for a while. */
	void TrackPortalTrips(const APenguinCharacter* Self, float Now);
	/** Portal ends on our way: Desired turns toward one whose other end is much closer to Goal (null: no goal, we only
	 *  wander or keep our distance), we walk on into one that helps, and we jump over the others. */
	void HandlePortals(APenguinCharacter* Self, const ABlockIsland* Island, float& Desired, const FVector* Goal);
	/** Going into Portal's end End is worth it: its far end lands safely (not over the sea, not crumbling, not at a deadly
	 *  edge), not straight above End (we would fall back in: a loop), the portal is not one we just looped through, and it
	 *  takes us much closer to Goal (bOnTheWay: the end lies right on our way - any real gain will do, no detour) - or
	 *  (bRecovering, off the island) anywhere safe at all. */
	bool WantsPortal(const APenguinCharacter* Self, const ABlockIsland* Island, const APinguPortal* Portal, int32 End,
		const FVector* Goal, bool bRecovering, bool bOnTheWay = false) const;
	/** We looped through Portal (two trips within PortalLoopWindow): we leave it alone until its time runs out. */
	bool IsPortalAvoided(const APinguPortal* Portal) const;
	/** In the air: our flight for the next moment goes into an end we don't want - the stick (and, falling onto it, a
	 *  jump) takes us past it. After every think. */
	void SteerClearOfPortals(APenguinCharacter* Self, const ABlockIsland* Island);
	/** A Snow Warp aimed at Aim ends over ground (not over the sea or a deep pit, not where the island crumbles). */
	bool WarpLandsSafely(const APenguinCharacter* Self, const ABlockIsland* Island, const FVector& Aim) const;

	// ---- Traits mode (UPenguinTraitComponent): what our trait changes in how we play. Without a trait nothing.
	/** The widest gap (cells) we jump at: 4, less for Heavyweight / Giant, up to 5 for a far jumper (Featherweight). */
	int32 MaxGapCells(const APenguinCharacter* Self) const;
	/** Hot Feet: the block we stand on melts after a second - stepped off it after a moment. True if it set Desired. */
	bool AvoidMeltingBlock(const APenguinCharacter* Self, const ABlockIsland* Island, float Dt, float& Desired);
	/** Mirror: it can send the next shot back right now. */
	bool IsMirrorReady(const APenguinCharacter* Self) const;
	/** Hot Feet: the block under us and how long we have stood on it. */
	FIntPoint HotFeetCell = FIntPoint(0, 0);
	float HotFeetTime = 0.f;
	/** The way we step off it (kept until we are off: no dithering between the edge checks). */
	float HotFeetWay = 0.f;
	/** Sticky: a wall holds us until we press away from it or jump. After a moment on one: a jump (up over the ledge in reach,
	 *  else a wall jump) when the way we want to go is into the wall, else the stick away from it (we let go). Every frame,
	 *  after the think (it may change Move). */
	void HandleStickyWall(APenguinCharacter* Self, float Dt);
	/** Sticky: seconds we have hung on the wall we are stuck to; letting go of one: seconds left pressing away from it, and
	 *  which way (+1 right). */
	float StickyHangTime = 0.f;
	float StickyLetGoLeft = 0.f;
	float StickyLetGoDir = 0.f;
	/** Rubber: a landing bounced us back up - no chained jump presses while it does (Jump ignores them). */
	bool bRubberBouncing = false;
	int32 LastLandEvents = 0;
	float LastLandTime = -100.f;

	bool bBotEnabled = false;
	int32 Difficulty = 1;
	FSkill Skill;
	FRandomStream Rand;

	TWeakObjectPtr<APenguinCharacter> Target;
	float TargetTimer = 0.f;
	float ThinkTimer = 0.f;
	float AbilityTimer = 0.f;
	/** The icicle we are charging (FireAbility): its charge, whether it follows the target (else IcicleFixedAim), and the
	 *  aim error picked for it (degrees). */
	float IcicleChargePower = 1.f;
	bool bIcicleLeadsTarget = false;
	FVector IcicleFixedAim = FVector::ZeroVector;
	float IcicleAimErrorDeg = 0.f;
	float ReactionTimer = 0.f;
	float JumpHold = 0.f;
	bool bJumpDown = false;
	float StuckTimer = 0.f;
	float DodgeCooldown = 0.f;
	/** Time until the next look for a belly slide attack. */
	float SlideTimer = 0.f;
	float WanderTimer = 0.f;
	float WanderDir = 1.f;
	FVector LastLocation = FVector::ZeroVector;
	FVector2D Move = FVector2D::ZeroVector;
	/** The ability bubble we are going for (kept for a moment so we do not dither between two). */
	TWeakObjectPtr<const APinguPickup> PickupGoal;

	/** A committed gap jump: keep going for the far side instead of "recovering" back halfway. */
	float GapDeadline = -1.f;
	float GapLandingX = 0.f;
	float GapLandingTop = 0.f;

	/** A committed pass through a gap one block high (or a crawl out from under a low ceiling): the stick stays TunnelDir
	 *  until we stand again or TunnelDeadline passes (then we think again). */
	float TunnelDeadline = -1.f;
	float TunnelStartTime = 0.f;
	float TunnelDir = 1.f;
	/** Crawling without getting anywhere (a dead end): seconds. */
	float CrawlStuckTimer = 0.f;

	/** Running from a live bomb's blast (AvoidBlast): the bomb, the way we run and whether we run right now (no slide attack,
	 *  and a gap is jumped on the way out). BombsSeen: when we first saw each bomb (the reaction time starts then). */
	TWeakObjectPtr<const APenguinProjectile> FleeBomb;
	float FleeDir = 0.f;
	bool bFleeingBlast = false;
	TMap<TWeakObjectPtr<const APenguinProjectile>, float> BombsSeen;

	/** Where we were at the last think (a jump from one portal end into the other: we went through). */
	FVector ThinkLocation = FVector::ZeroVector;
	bool bHasThinkLocation = false;
	/** Per portal we went through: when the last trip was, and until when we leave it alone (a loop). */
	struct FPortalMemory
	{
		TWeakObjectPtr<const APinguPortal> Portal;
		float LastTrip = -100.f;
		float AvoidUntil = -1.f;
	};
	TArray<FPortalMemory> PortalMemory;
	int32 PortalTrips = 0;
	/** Too close to an end to jump it: we back off this way until then (a run-up), no turning back and forth. */
	float PortalRunUpUntil = -1.f;
	float PortalRunUpDir = 0.f;
	/** Where Navigate last headed (portals in the air are judged by it). */
	FVector LastGoal = FVector::ZeroVector;
	bool bHasLastGoal = false;

	/** Going in (UpdateAggro): on, seconds without a fight, seconds left, how often, the hit counter we last saw, and
	 *  UseAbilities' last use was no attack (our own grow ray, a flap). */
	bool bAggressive = false;
	float IdleTime = 0.f;
	float AggroLeft = 0.f;
	/** Going in: seconds so far and attacks used so far (none after AggroStallTime: the belly slide instead). */
	float AggroElapsed = 0.f;
	int32 AggroAttacks = 0;
	int32 AggroCount = 0;
	int32 SeenHitEvents = -1;
	/** Game time of the last UpdateAggro (-1: none yet). */
	float LastAggroUpdate = -1.f;
	bool bLastUseNoAttack = false;
};

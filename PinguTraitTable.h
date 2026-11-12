// Pingu - the traits (Traits mode): with TRAITS ON every penguin gets one random trait each round, an upside with a
// downside. This is the table: names, colours, body shapes, the numbers the gameplay reads (FPinguTraitNumbers), the
// trait-specific tuning (PinguTraitTuning) and the stat lines the PINGUPEDIA shows.
//
// Data only. UPenguinTraitComponent (on every penguin) holds the trait a penguin has this round; the movement, the
// abilities and the hits read its numbers. EPinguTrait::None is the neutral entry: none of its numbers changes anything.

#pragma once

#include "CoreMinimal.h"
#include "PinguTraitTable.generated.h"

/** The traits (17 to begin with; more appended since). Names are TCHAR data in the table (PinguTraits::GetInfo), like the abilities' Infos. */
UENUM()
enum class EPinguTrait : uint8
{
	None,
	Heavyweight,    // hard to knock away; no double jump, slower
	QuickDraw,      // cooldowns x0.5; hits x0.7
	SlowBurn,       // hits x1.75; cooldowns x2
	IceTouch,       // the snow under you turns to ice, shots freeze on you, touched penguins get frozen feet; you slip
	Featherweight,  // triple jump, slow fall; knocked 1.5x as far
	Tiny,           // one shrink step smaller (fits one-block gaps), runs faster; the size's downsides
	Giant,          // two grow steps bigger, landings break ice; jumps lower, falls faster
	GlassCannon,    // pushes 1.5x as hard; is pushed 1.5x as far
	Rubber,         // bounces off walls and the ground in a knockback flight; bouncy landings, no ledge grab
	SecondChance,   // a fish throws it back out of the sea once a round; only two abilities
	Leech,          // every hit landed takes 2 s off all cooldowns; cooldowns x1.5
	Gambler,        // 60 % of uses hit twice as hard, 40 % fizzle (the ability turns into sparkling dust)
	Speedster,      // runs and dashes 1.5x as fast; slippery feet
	HotFeet,        // can't be frozen, melts ice; the block it stands on melts
	Mirror,         // the first shot every 6 s goes back to its thrower; knocked 1.25x as far
	Sticky,         // wall cling, wall jump, long ledge reach; no belly slide, less air control
	Jester,         // hits x1.3; every used ability turns into a random other one
	SameAbilities,  // everybody gets the same three random abilities this round (over any picks); 2026-11-07, card ability-picks
	// ---- 2026-11-07, card shared-traits (every penguin has the round's one trait: these are made for that)
	Pinball,        // knockback x2 from the round's start (the growth on top)
	FastGrowth,     // knockback grows +6 % a second (normally +2 %)
	DamagePercent,  // every hit taken adds +12 % to how far the next ones send you (shown under the penguin)
	Recoil,         // every hit you land pushes you back with half its force
	HeavyHitters,   // hits x1.5; cooldowns x1.5
	LowGravity,     // gravity x0.4
	HighGravity,    // gravity x1.6
	IceRink,        // the whole island turns to ice: everybody slides
	Turbo,          // run x1.5; cooldowns x0.5
	DeadlyEdges,    // past the island's left / right side is an instant knock-out
	Bouncy,         // knocked flying, you bounce off walls (Rubber's 85 %)
	// ---- 2026-11-07, card round-hazards: the whole island gets a hazard (APinguRoundHazards, PinguRoundHazards.h); no numbers
	RisingSea,      // the sea rises through the round
	CrumblingIsland, // every 10 s the outermost column at each end shakes and falls into the sea
	Wind,           // gusts push everybody left or right (a warning arrow first)
	SnowballRain,   // snowballs fall from the sky (a ring marks where)
	Count UMETA(Hidden)
};

/** The traits a penguin can get (None not counted). */
constexpr int32 PinguNumTraits = static_cast<int32>(EPinguTrait::Count) - 1;

/**
 * What a trait changes, as numbers. The defaults change nothing (EPinguTrait::None has exactly these). Scales multiply the
 * normal value (BP_Penguin's tuning, an ability's cooldown or strength); the rest is said per field.
 */
struct FPinguTraitNumbers
{
	/** Every ability's cooldown x this (UPenguinAbilityComponent::UseAbility). */
	float CooldownScale = 1.f;
	/** The strength of every ability use (UPenguinAbilityComponent::GetUseStrength; Gambler rolls its own). */
	float Strength = 1.f;
	/** Knockback this penguin takes / deals, x this (APenguinCharacter::ApplyPenguinHit). */
	float KnockbackTaken = 1.f;
	float KnockbackDealt = 1.f;
	/** Movement (UPenguinMovementComponent's effective getters): running speed, dash speed, jump speed (ground, double jump
	 *  and jump pads), the falling gravity multiplier. */
	float RunScale = 1.f;
	float DashScale = 1.f;
	float JumpScale = 1.f;
	float FallGravityScale = 1.f;
	/** The fastest fall (cm/s); 0 keeps the penguin's own (BP_Penguin's MaxFallSpeed). A fast fall is not limited by it. */
	float MaxFallSpeed = 0.f;
	/** All of the gravity, rising and falling, x this (Low / High Gravity; UPenguinMovementComponent::GetGravityZ). The jump
	 *  speeds stay: a jump goes 1 / GravityScale as high. */
	float GravityScale = 1.f;
	/** Knocked flying into a wall, it bounces off keeping this share of its speed (Bouncy; 0 = no bounce. Rubber has its own:
	 *  PinguTraitTuning::RubberWallKeep). */
	float FlightWallBounce = 0.f;
	/** Air acceleration, turning and braking x this. */
	float AirControlScale = 1.f;
	/** Grip on the ground: speeding up and turning (GripAccelScale) and braking without input (GripBrakeScale) x this, on
	 *  top of the ice's own (it stacks). */
	float GripAccelScale = 1.f;
	float GripBrakeScale = 1.f;
	/** The size the penguin rests at (APenguinCharacter::SetBaseSize; size rays stack on it, a ray wears off back to it). */
	float BaseSize = 1.f;
	/** Ledge grab reach up / forward x this. */
	float ClimbUpScale = 1.f;
	float ClimbForwardScale = 1.f;
	/** Air jumps after the ground jump: -1 keeps the penguin's own (BP_Penguin's MaxAirJumps, 1). */
	int32 AirJumps = -1;
	/** Abilities in the loadout: 3, or 2 (slots 0 and 2; slot 1 None - Second Chance). */
	int32 NumAbilities = 3;
	bool bCanBellySlide = true;
	bool bCanGrabLedge = true;
	/** Never frozen (frost, ice bolts' frozen feet, Ice Touch). */
	bool bFreezeImmune = false;
	/** Every floor grips like ice (Ice Rink: rock too - the island's snow turns to ice as well). */
	bool bAllFloorsIce = false;
};

/** One trait: what it is called and looks like, and its numbers. */
struct FPinguTraitInfo
{
	EPinguTrait Trait = EPinguTrait::None;
	/** "Glass Cannon". */
	const TCHAR* Name = TEXT("");
	/** /Game/UI/Traits/T_Trait_<IconName> ("GlassCannon": the name without spaces). */
	const TCHAR* IconName = TEXT("");
	/** One line each for the PINGUPEDIA. */
	const TCHAR* Upside = TEXT("");
	const TCHAR* Downside = TEXT("");
	/** The trait's colour (the reveal card's ring, the PINGUPEDIA). */
	FLinearColor Color = FLinearColor::White;
	/** The body's shape (side, forward, up) on top of the mesh scale (APenguinCharacter::SetTraitBodyScale). */
	FVector BodyScale = FVector::OneVector;
	FPinguTraitNumbers N;
};

/** What a stat line of the PINGUPEDIA says (its label; the unit: PinguTraits::GetStatUnit). Abilities use the first
 *  block (PinguTuning::GetAbilityStats), traits the second (PinguTraits::GetStatLines). */
enum class EPinguStatKind : uint8
{
	// ---- abilities
	Cooldown,       // s
	Knockback,      // cm/s: how hard it pushes
	Lift,           // cm/s: straight up on top of the push (a jump pad: how fast it throws you up)
	Speed,          // cm/s: how fast the shot flies (the stomp: how fast it falls)
	Range,          // cm
	Radius,         // cm
	KillRadius,     // cm: knocks out inside this
	Cone,           // degrees: half the cone's angle (the PINGUPEDIA shows the whole cone's width: twice this)
	Duration,       // s
	Freeze,         // s
	Stun,           // s
	Slip,           // s
	Blocks,         // count
	Fuse,           // s
	SizeStep,       // x: one hit's size change
	// ---- traits
	CooldownScale,  // x
	Strength,       // x
	KnockbackTaken, // x
	KnockbackDealt, // x
	RunSpeed,       // x
	DashSpeed,      // x
	Jump,           // x: jump speed
	AirJumps,       // count
	FallGravity,    // x
	FallSpeed,      // cm/s: the fastest fall
	Size,           // x
	Grip,           // x: speeding up and turning on the ground
	Braking,        // x
	AirControl,     // x
	ClimbReach,     // x
	LedgeGrab,      // yes / no
	BellySlide,     // yes / no
	FreezeImmune,   // yes / no
	Abilities,      // count
	Rescues,        // count (Second Chance: per round)
	WallBounce,     // share of the speed kept
	FloorBounce,    // share of the height fallen it bounces back up (Rubber)
	Chance,         // share (Gambler: a double)
	FizzleChance,   // share
	CooldownCut,    // s off every cooldown (Leech: per hit)
	Recharge,       // s (Mirror)
	Cling,          // yes / no: it sticks to walls, as long as it likes (Sticky)
	MeltTime,       // s a block takes to melt (Hot Feet)
	SwapDelay,      // s (Jester)
	ClimbReachForward, // x: how far from a wall a ledge can be grabbed (Sticky; ClimbReach is the reach up)
	ClingFallSpeed, // cm/s: the slide down a wall while clinging with down held (Sticky)
	WallJump,       // cm/s: a wall jump's push away from the wall (Sticky)
	ReturnSpeed,    // x: a shot sent back is this much faster (Mirror)
	RescueThrow,    // yes / no: the fish throws it high over the island and back onto it (Second Chance)
	MeltReach,      // cm: ice blocks, frozen shots and icicle pegs this close to it melt (Hot Feet)
	// ---- abilities again (added later)
	ComboStep,      // share: how much harder each snowball hit in a row pushes (+35 %)
	ComboMax,       // x: the snowball combo's push never grows past this
	// ---- traits again (2026-11-07, shared-traits)
	Gravity,        // x: all of the gravity (Low / High Gravity)
	KnockbackGrowth, // share per second: how fast the fight's knockback grows (Fast Growth)
	DamageStep,     // share: how much further every hit taken makes the next ones send you (Damage %)
	Recoil,         // share: how hard your own hit pushes you back (Recoil)
	EdgeKnockOut,   // yes / no: past the island's sides is a knock-out (Deadly Edges)
	Count
};

/** How a stat's value reads: the PINGUPEDIA's LOCTEXT PingupediaUnit<Unit>. */
enum class EPinguStatUnit : uint8
{
	Seconds,     // "2.3 s"
	Centimetres, // "440 cm" (or blocks: 100 cm each)
	Speed,       // "1300 cm/s"
	Multiplier,  // "x0.35"
	Count,       // "3"
	Share,       // 0..1, shown as "55 %"
	Degrees,     // "32°"
	Flag,        // 1 yes, 0 no
};

/** One line of a stat grid: what, how much, and (traits) whether it is part of the upside. */
struct FPinguStatLine
{
	EPinguStatKind Kind = EPinguStatKind::Cooldown;
	float Value = 0.f;
	/** Traits: an upside (green) or a downside (red). Abilities: always true. */
	bool bUpside = true;
};

namespace PinguTraits
{
	/** The table entry of a trait (None for anything out of range: neutral numbers). */
	PENGUIN_API const FPinguTraitInfo& GetInfo(EPinguTrait Trait);
	/** Numbers that change nothing (EPinguTrait::None's). */
	PENGUIN_API const FPinguTraitNumbers& GetNeutralNumbers();
	/** A real trait (not None, not Count). */
	PENGUIN_API bool IsReal(EPinguTrait Trait);
	/** "Glass Cannon", "GlassCannon", "glasscannon" -> GlassCannon (case and spaces don't matter); None if unknown. */
	PENGUIN_API EPinguTrait FindByName(const FString& Name);
	/** "Giant,Tiny" (the -PinguTrait= switch) -> { Giant, Tiny }; unknown names are skipped (and logged). */
	PENGUIN_API TArray<EPinguTrait> ParseList(const FString& List);
	/** The trait's name for logs ("Glass Cannon", "none"). */
	PENGUIN_API FString GetLogName(EPinguTrait Trait);
	/** Every real trait, in table order. */
	PENGUIN_API TArray<EPinguTrait> GetAll();
	/** The PINGUPEDIA's stat lines of a trait: its upsides first, then its downsides. */
	PENGUIN_API void GetStatLines(EPinguTrait Trait, TArray<FPinguStatLine>& Out);
	/** How a kind of stat reads. */
	PENGUIN_API EPinguStatUnit GetStatUnit(EPinguStatKind Kind);
	/** The kind's name ("KnockbackTaken"): logs, and the PINGUPEDIA's LOCTEXT keys PingupediaStat<Name>. */
	PENGUIN_API const TCHAR* GetStatKindName(EPinguStatKind Kind);
}

/** Trait-specific numbers (the gameplay of the traits reads them: PenguinTraitEffects.cpp and its hooks; the PINGUPEDIA
 *  shows some). */
namespace PinguTraitTuning
{
	// ---- the strength of one ability use (S: UPenguinAbilityComponent::GetUseStrength)
	/** Reach (ranges, radii, hitboxes) scales with R = sqrt(S), within these ... */
	constexpr float ReachScaleMin = 0.7f;
	constexpr float ReachScaleMax = 1.5f;
	/** ... durations (freeze, slip, shell, size rays) with D = S, within these. */
	constexpr float DurationScaleMin = 0.5f;
	constexpr float DurationScaleMax = 2.f;
	inline float GetReachScale(float Strength) { return FMath::Clamp(FMath::Sqrt(FMath::Max(Strength, 0.f)), ReachScaleMin, ReachScaleMax); }
	inline float GetDurationScale(float Strength) { return FMath::Clamp(Strength, DurationScaleMin, DurationScaleMax); }

	// ---- Ice Touch
	/** The snow under its feet freezes in this radius (cm), this often (s) while it stands on the ground. */
	constexpr float IceTouchFloorRadius = 60.f;
	constexpr float IceTouchFloorInterval = 0.25f;
	/** A shot that touches it freezes for this long and drops: its velocity becomes (ShotDropKeepX x vx, ShotDropZ), and it
	 *  ignores the penguin for ShotIgnoreTime. */
	constexpr float IceTouchShotFreeze = 3.f;
	constexpr float IceTouchShotDropKeepX = 0.15f;
	constexpr float IceTouchShotDropZ = -200.f;
	constexpr float IceTouchShotIgnoreTime = 0.6f;
	/** A penguin it touches gets frozen feet for this long, each one at most once per Repeat seconds. */
	constexpr float IceTouchFeetTime = 3.f; // (2.5 until 2026-11-05: ice feet are a slide now - no walking for this long)
	constexpr float IceTouchFeetRepeat = 1.5f;
	/** Its own ice: the grip an ice floor gives (UPenguinMovementComponent::IceAccelerationScale / IceBrakingScale). */
	constexpr float IceTouchGrip = 0.28f;
	constexpr float IceTouchBraking = 0.1f;

	// ---- Giant
	/** A landing at least this fast (cm/s) breaks up to this many ice blocks under its feet. */
	constexpr float GiantBreakLandSpeed = 700.f;
	constexpr int32 GiantBreakBlocks = 3;

	// ---- Rubber
	/** Every landing bounces it back up to BounceHeight of the height it fell from (the top of the fall to the landing,
	 *  whatever the falling gravity and the top falling speed did on the way down: UPenguinMovementComponent::
	 *  TryRubberLandingBounce) - 5 m, 3.5 m, 2.45 m, ... - as long as that bounce leaves the ground at MinSpeed (cm/s) or
	 *  more; a smaller one settles. Not a stomp's slam, a fish hook's throw, a belly slide, an ice cube; a jump pressed for
	 *  the landing jumps instead. In a knockback flight (stunned, or faster sideways than FlightSpeed) it also bounces off
	 *  walls keeping WallKeep of its speed into them. */
	constexpr float RubberWallKeep = 0.85f;
	constexpr float RubberBounceHeight = 0.7f;
	constexpr float RubberMinSpeed = 500.f;
	constexpr float RubberFlightSpeed = 900.f;

	// ---- Second Chance
	/** The first fall into the sea each round: a fish throws it high over the island - its feet ApexAbove (cm) over the
	 *  island's highest block at the top of the throw - toward the middle of what still stands of the island, at most
	 *  MaxAngle (degrees) off straight up, as fast sideways as it takes to come down there (PenguinTraitEffects.cpp:
	 *  TrySecondChance). On the way up it passes through the island's blocks until its feet are PassClear over the highest
	 *  one (UPenguinMovementComponent::SetPassThroughIsland); it is held like a hit until the top of the throw. Too far out
	 *  for MaxAngle: thrown at MaxAngle anyway - short of the island it comes down in the sea. Its air moves come back, and
	 *  the sea leaves it alone for Grace seconds. */
	constexpr float SecondChanceApexAbove = 200.f;
	constexpr float SecondChanceMaxAngle = 45.f;
	constexpr float SecondChancePassClear = 10.f;
	constexpr float SecondChanceGrace = 0.6f;
	constexpr int32 SecondChanceRescues = 1;

	// ---- Leech
	/** Every hit it lands takes this much off all its cooldowns (s). */
	constexpr float LeechCooldownCut = 2.f;

	// ---- Gambler
	/** This share of its uses hits DoubleStrength times as hard; the rest fizzle (nothing happens, FizzleCooldown of the
	 *  cooldown). */
	constexpr float GambleDoubleChance = 0.6f;
	constexpr float GambleDoubleStrength = 2.f;
	constexpr float GambleFizzleCooldown = 0.5f;

	// ---- Hot Feet
	/** A block it stands on melts after this long (s; Snow, Rock or Ice - never DeepRock). */
	constexpr float HotFeetMeltTime = 1.f;
	/** Ice blocks it touches (capsule + Reach cm) melt, one per Interval seconds. */
	constexpr float HotFeetTouchReach = 30.f;
	constexpr float HotFeetTouchInterval = 0.2f;

	// ---- Mirror
	/** The first shot that hits it every Recharge seconds goes back to its shooter, Speed times as fast (never a hook). */
	constexpr float MirrorRecharge = 6.f;
	constexpr float MirrorSpeed = 1.2f;

	// ---- Sticky
	/** Any wall it touches in the air on the way down (a jump rises on past it and sticks at its top) holds it: no slide, no
	 *  time limit, until the stick presses away from the wall (it lets go), a jump (a ledge in reach: up over it; else the
	 *  wall jump: WallJumpSide sideways plus a full jump, the double jump back), or anything that takes the control (a hit, a
	 *  freeze). Down held: it slides down the wall at ClingSlideSpeed (cm/s); up does nothing. */
	constexpr float StickyClingSlideSpeed = 250.f;
	constexpr float StickyWallJumpSide = 600.f;

	// ---- Jester
	/** This long after a use the slot becomes a random other ability (not one of its other two); the cooldown carries over. */
	constexpr float JesterSwapDelay = 0.4f;

	// ---- the shared traits (2026-11-07: everybody has the round's one trait)
	/** Pinball: every knockback x this from the fight's first second (APenguinGameState::GetKnockbackScale; the growth on top). */
	constexpr float PinballKnockback = 2.f;
	/** Fast Growth: the fight's knockback grows this much a second (APenguinGameState::KnockbackGrowthPerSecond is 0.02). */
	constexpr float FastGrowthPerSecond = 0.06f;
	/** Damage %: every hit a penguin takes adds this to how far the next ones send it (x 1 + its damage). Reset each round. */
	constexpr float DamageStep = 0.12f;
	/** ... never more than this much damage (x5 at the most). */
	constexpr float DamageMax = 4.f;
	/** Recoil: a hit you land pushes you back with this share of the push it gave (sideways), and a little up. */
	constexpr float RecoilShare = 0.5f;
	constexpr float RecoilLift = 150.f;
	/** Recoil: the attacker's push back is at most this fast (cm/s) and only comes from hits at least this fast. */
	constexpr float RecoilMax = 2200.f;
	constexpr float RecoilMinPush = 200.f;
	/** Heavy Hitters: every push x KnockbackDealt; cooldowns x this. */
	constexpr float HeavyHittersKnockback = 1.5f;
	constexpr float HeavyHittersCooldown = 1.5f;
	/** Low / High Gravity: all of the gravity x this. */
	constexpr float LowGravityScale = 0.4f;
	constexpr float HighGravityScale = 1.6f;
	/** Turbo: running (and dashing) x RunScale, cooldowns x CooldownScale. */
	constexpr float TurboRunScale = 1.5f;
	constexpr float TurboCooldown = 0.5f;
	/** Deadly Edges: a penguin whose middle is this far (cm) past the island's outermost block at the round's start (left or
	 *  right) is knocked out at once, however high it is (APenguinGameState::GetDeadlyEdges). */
	constexpr float DeadlyEdgeMargin = 150.f;
	/** Bouncy: knocked flying into a wall it keeps this share of its speed (Rubber's wall bounce). */
	constexpr float BouncyWallKeep = 0.85f;
}

namespace PinguTraits
{
	/** True when the bit of Trait is set in a "traits off" mask (FPinguMatchSettings::TraitsOff: bit = 1 << trait value). */
	inline bool IsOffIn(uint64 OffMask, EPinguTrait Trait)
	{
		const int32 Bit = static_cast<int32>(Trait);
		return Bit >= 0 && Bit < 64 && (OffMask & (uint64(1) << Bit)) != 0;
	}
	/**
	 * The round's one trait for everybody (Traits mode, 2026-11-07): the first one in Bag; a Bag that runs dry is refilled with
	 * every trait not in OffMask (the host's checklist), shuffled with Rand - so none comes twice before all the ticked ones came.
	 * A trait in Bag that is off now (the list changed) is skipped. Nothing ticked: every trait. APenguinGameMode::DealTraits and
	 * PinguFair::Deal (the joiners' check) both call this: they must agree.
	 */
	PENGUIN_API EPinguTrait DrawShared(TArray<EPinguTrait>& Bag, FRandomStream& Rand, uint64 OffMask);
}

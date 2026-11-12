// Pingu - the round hazards (2026-11-07, card round-hazards; Linus: "...low gravity or edges kill and stuff like that", he
// picked all four from a list: "Rising Sea - the sea slowly rises during the round", "Crumbling Island - the outer blocks break
// off every 10 s", "Wind - gusts push everyone left or right every few seconds, with a warning arrow first", "Snowball Rain -
// snowballs fall from the sky").
//
// Four traits of the trait table (EPinguTrait::RisingSea, CrumblingIsland, Wind, SnowballRain) change nothing on a penguin: when
// the round's trait is one of them, the whole island gets that hazard. APenguinGameMode::SetupRound calls SetupForRound once the
// traits are dealt; this actor waits for the fight (the GO), runs the hazard through it, stops when the round is over and goes
// with the next round (or the lobby, the menu).
//
//  - Rising Sea:       the sea rises from where it is toward SeaTopBelowGround under the island's lowest start ground, fast at
//                      first, slower near the top (an ease-out over SeaRiseTime). ABlockIsland::SetSeaRise moves SeaLevel - every
//                      sea check (knock-outs, pickups, sand) follows it - and the sea's look.
//  - Crumbling Island: every CrumbleEvery s the outermost block column at each end (and anything built beyond it) shakes for
//                      CrumbleWarn s and falls into the sea (ABlockIsland::CrumbleOutside: sudden death's crumbling); never
//                      narrower than CrumbleMinColumns columns. Sudden death takes over when it comes.
//  - Wind:             every WindEvery s a WindWarn s warning (a small arrow at the top of the screen, a few streaks), then a
//                      WindGust s gust pushing everybody WindSpeed cm/s left or right (moved by it like a conveyor belt: walking
//                      against it still gets you somewhere).
//  - Snowball Rain:    every RainEvery s (+- RainJitter) a snowball falls from above the screen at a slant onto a random spot of
//                      the island; a ring (the bomb's blast-zone look, its clock filling) marks the spot from RainLead s before it
//                      is thrown. Its thrower is nobody: the hit is a normal snowball's, its knock-out counts for the last
//                      penguin that hit the victim (if any) or nobody ("X fell into the sea!").
//
// LAN: the host decides everything and replicates it: the fight's start / stop times, the sea's top, the gusts (time and
// direction), the drops (where and when); the snowballs are ordinary replicated projectiles and the crumbling is the island's own
// replicated state. Every machine moves its own sea and draws its own arrow, streaks and rings from that; the wind moves the
// penguins each machine simulates (its own player, the host's bots).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguTraitTable.h"
#include "PinguRoundHazards.generated.h"

class ABlockIsland;
class APenguinCharacter;
class SWidget;
class UGameViewportClient;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** The hazards' numbers (s, cm, cm/s). */
namespace PinguHazardTuning
{
	// ---- Rising Sea
	/** Seconds of fight the sea takes to reach its top (eases out: half of it after ~10 s, 89 % at 30 s - sudden death). */
	constexpr float SeaRiseTime = 35.f;
	/** Its top: this far under the lowest ground of the island's start places (a penguin there still has ~1.4 m to the sea). */
	constexpr float SeaTopBelowGround = 260.f;

	// ---- Crumbling Island
	constexpr float CrumbleEvery = 10.f;
	constexpr float CrumbleWarn = 1.5f;
	constexpr int32 CrumbleMinColumns = 6;

	// ---- Wind
	/** The first warning this long after GO, then one every WindEvery s (warning + gust + calm). */
	constexpr float WindFirstAfter = 3.f;
	constexpr float WindEvery = 8.f;
	constexpr float WindWarn = 1.f;
	constexpr float WindGust = 2.5f;
	/** The push (cm/s; a penguin runs 560): eased in and out over WindRamp s. */
	constexpr float WindSpeed = 300.f;
	constexpr float WindRamp = 0.3f;

	// ---- Snowball Rain
	constexpr float RainFirstAfter = 1.5f;
	constexpr float RainEvery = 1.5f;
	constexpr float RainJitter = 0.4f;
	/** The ring shows this long before the snowball is thrown; it then falls for RainFallTime s. */
	constexpr float RainLead = 0.6f;
	constexpr float RainFallTime = 1.1f;
	/** Thrown from this far over the build limit's top line (off the screen), this fast sideways (the slant: its hit pushes
	 *  sideways like a thrown snowball's, a bit less), as heavy as a snowball. */
	constexpr float RainAboveTop = 250.f;
	constexpr float RainSideSpeed = 1000.f;
	constexpr float RainGravity = 700.f;
	/** The ring's radius (cm). */
	constexpr float RainRingRadius = 70.f;
}

namespace PinguRoundHazards
{
	/** One of the four hazard traits. */
	PENGUIN_API bool IsHazard(EPinguTrait Trait);
}

/** One snowball of the rain: where it lands and when (host flow times: the server world time in LAN games). */
USTRUCT()
struct FPinguRainDrop
{
	GENERATED_BODY()

	/** Where it comes down (world X, the ground's Z). */
	UPROPERTY()
	float X = 0.f;
	UPROPERTY()
	float Z = 0.f;
	/** The ring shows from ShowAt, the snowball is thrown at LandAt - RainFallTime and lands at LandAt. */
	UPROPERTY()
	float ShowAt = 0.f;
	UPROPERTY()
	float LandAt = 0.f;
	/** -1 from the right, 1 from the left, 0 straight down. */
	UPROPERTY()
	int8 Side = 0;
	UPROPERTY()
	int32 Serial = 0;
	/** Host: its snowball is out. */
	UPROPERTY(NotReplicated)
	bool bThrown = false;
};

UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguRoundHazards : public AActor
{
	GENERATED_BODY()

public:
	APinguRoundHazards();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- the game mode's hook (solo / LAN host)
	/** A new round (or the lobby): the last round's hazard goes; the round's trait (the first hazard trait among the penguins) gets
	 *  its hazard. */
	static void SetupForRound(UWorld* World);
	/** The hazard of World (null without one). */
	static APinguRoundHazards* Get(const UWorld* World);
	/** Removes every hazard (the sea goes back). */
	static void ClearAll(UWorld* World);

	// ---- bots (host): steps off crumbling blocks, away from a ring about to be hit, walks against the wind near the edge it
	/** blows toward. True when it changed Desired (-1..1, the stick). */
	static bool SteerBot(const APenguinCharacter* Bot, const ABlockIsland* Island, float& Desired);

	// ---- every machine (tests, bots, the looks)
	EPinguTrait GetHazard() const { return Hazard; }
	/** The fight began and the hazard has not stopped. */
	bool IsRunning() const { return HasStarted() && !HasStopped(); }
	bool HasStarted() const { return StartTime > NotYet; }
	bool HasStopped() const { return StopTime > NotYet; }
	/** Seconds of the hazard's fight so far (stands still once stopped; 0 before the GO). */
	float GetHazardTime() const;
	/** The clock the times are on: the server world time in LAN games, the world time in solo. */
	float GetNow() const;
	/** Rising Sea: how far up the sea is now (cm) and its top. */
	float GetSeaRiseNow() const;
	float GetSeaTop() const { return SeaTop; }
	/** Wind: the push now (cm/s, signed: + to the right), whether the warning shows, the gust direction (+1 / -1), gusts so far. */
	float GetWindNow() const;
	bool IsWindWarning() const;
	bool IsGustBlowing() const;
	int32 GetGustDir() const { return GustDir; }
	int32 GetGusts() const { return GustSerial; }
	/** Crumbling Island: crumbles so far, and the outermost standing columns' middles (world X). */
	int32 GetCrumbles() const { return Crumbles; }
	float GetCrumbleLeftX() const { return CrumbleLeftX; }
	float GetCrumbleRightX() const { return CrumbleRightX; }
	/** Snowball Rain: the drops still to come or just landed, and how many there were. */
	const TArray<FPinguRainDrop>& GetRainDrops() const { return RainDrops; }
	int32 GetRainCount() const { return RainSerial; }

	// ---- tests (solo)
	/** Tests: every round gets this hazard whatever its trait (automated runs only): -PinguHazard=<Name>[,<Name>...] or the
	 *  environment variable PINGU_HAZARD (the LAN tests' processes inherit it) - round N takes the list's (N-1)th, round by round. */
	static EPinguTrait GetForcedHazard(int32 Round);
	/** Starts Trait's hazard on the arena now, as if its fight had begun Ago seconds back (it runs whatever the phase). */
	static APinguRoundHazards* StartForTest(UWorld* World, EPinguTrait Trait, float Ago);
	/** The round is over: it stops here. */
	void StopNow();
	/** The next drop lands at X (its ring shows at once). */
	void ForceNextDrop(float X);
	/** This machine's looks: the wind arrow on the screen (its opacity, 0 hidden) and the rain rings showing. */
	float GetArrowOpacity() const { return ArrowOpacity; }
	int32 GetShownRings() const { return ShownRings; }
	/** Penguins the wind moved in the last frame here. */
	int32 GetWindMoved() const { return WindMoved; }

private:
	void ServerTick(float Now);
	void StartHazard(float Now);
	void TickCrumble(float Now);
	void TickWind(float Now);
	void TickRain(float Now);
	bool PlanDrop(float Now, float ForcedX);
	void ThrowDrop(FPinguRainDrop& Drop);
	void ApplyWind(float DeltaSeconds);
	void ApplySea();
	void UpdateRings();
	void UpdateStreaks(float DeltaSeconds);
	void UpdateArrow();
	void RemoveArrow();
	/** LAN tests (-PinguNetTest): both machines log what they see of the hazard in the same words ("round N hazard ...", diffed
	 *  in the pipeline's compare file) and judge their own looks of it when it stops. */
	void UpdateNetTestLog();
	ABlockIsland* GetIsland() const;

	UPROPERTY(Replicated)
	EPinguTrait Hazard = EPinguTrait::None;
	/** Clock times (GetNow) the fight began / the hazard stopped; NotYet before (a LAN host's clock can be young: no sign tricks). */
	static constexpr float NotYet = -1.e9f;
	UPROPERTY(Replicated)
	float StartTime = NotYet;
	UPROPERTY(Replicated)
	float StopTime = NotYet;
	/** Rising Sea: the top (cm above the island's own sea). */
	UPROPERTY(Replicated)
	float SeaTop = 0.f;
	/** Wind: the latest gust's warning began at GustAt, blowing GustDir (+1 right, -1 left); GustSerial counts them. */
	UPROPERTY(Replicated)
	float GustAt = -100.f;
	UPROPERTY(Replicated)
	int8 GustDir = 0;
	UPROPERTY(Replicated)
	int32 GustSerial = 0;
	/** Crumbling Island. */
	UPROPERTY(Replicated)
	int32 Crumbles = 0;
	UPROPERTY(Replicated)
	float CrumbleLeftX = 0.f;
	UPROPERTY(Replicated)
	float CrumbleRightX = 0.f;
	/** Snowball Rain. */
	UPROPERTY(Replicated)
	TArray<FPinguRainDrop> RainDrops;
	UPROPERTY(Replicated)
	int32 RainSerial = 0;

	// ---- host
	FRandomStream Rand;
	bool bTestRun = false;
	float NextCrumbleAt = 0.f;
	float NextGustAt = 0.f;
	float NextRainAt = 0.f;
	float CellSize = 100.f;
	bool bForcedDrop = false;
	float ForcedDropX = 0.f;

	// ---- this machine's looks
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Rings;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> RingMaterials;
	bool bNoRings = false;
	int32 ShownRings = 0;
	TSharedPtr<SWidget> ArrowWidget;
	TWeakObjectPtr<UGameViewportClient> ArrowViewport;
	float ArrowOpacity = 0.f;
	int32 SeenGustSerial = 0;
	float StreakCarry = 0.f;
	int32 WindMoved = 0;
	bool bSeaApplied = false;

	// ---- LAN test log (this machine)
	bool bNetTestLog = false;
	int32 LoggedGusts = 0;
	int32 LoggedCrumbles = 0;
	int32 LoggedDrops = 0;
	bool bLoggedStop = false;
	float SeenArrowMax = 0.f;
	int32 SeenWindMoved = 0;
	int32 SeenRingsMax = 0;
	bool bSeenShake = false;
};

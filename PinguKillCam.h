// Pingu - the kill cam (card 2026-11-07_kill-cam; docs/FEATURES.md 1 "The kill cam").
//
// The knock-out that wins a round plays in slow motion: right before a hit that can't be escaped any more the game almost
// freezes (0.05x) and the camera zooms in on the victim (and the hitter, if both fit); then the speed ramps back up, faster and
// faster, and is exactly 1x when the death animation ends. The winner banner comes after it (higher up on the screen).
//
// Who decides: the server (solo or the LAN host). Every frame of a fight it looks at the penguins whose knock-out would end the
// round (APenguinGameMode::IsFinalVictim) and asks whether one can still be saved:
//  - a deadly shot about to hit (an icicle, a snow bomb going off, a belly stomp coming down) within ShotWindow seconds;
//  - its flight (after a knockback, or a plain fall) ending in the sea with every way back it has (air jumps, the air dash, Flap,
//    the other abilities that can save it, Second Chance) - PinguKillCam::PredictFlight. Triggered at most MaxLead s of game
//    time before the splash.
// No prediction (an iceberg, a ray, a hazard): the slow motion starts at the knock-out itself. A prediction that does not
// come true eases back to normal speed and logs "[KILLCAM] false trigger".
//
// The curve: one segment at a time, d(t) = From + (1 - From) * (t / Duration)^Exponent (real seconds, the paused world stops
// it), chosen so the game time it covers is exactly what is left until the death animation ends; at the knock-out it is
// planned again from there (the death animation's length is known then: APenguinCharacter::GetDeathShowTime).
//
// LAN: the actor replicates its plan (FPinguKillCamPlan); every machine runs the same curve on its own clock from when the
// plan arrived and sets its own time dilation (a client after the engine's replication, so the host's replicated value never
// makes it stutter) and its own camera zoom. The host's judge of a joiner's moves runs on PacedSeconds (real time without the
// slowed part) so slowed moves are never refused.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"
#include "PenguinTypes.h"
#include "PinguKillCam.generated.h"

class APenguinCharacter;
class ABlockIsland;
class UPenguinMovementComponent;
class APenguinCamera;

UENUM()
enum class EPinguKillCamStage : uint8
{
	Off,
	/** Predicted: the hit / the splash is still coming. */
	Approach,
	/** The knock-out happened: the death animation plays out. */
	Showcase,
	/** A prediction that did not come true: back to normal speed. */
	Release,
};

/** One segment of the slow motion curve (replicated: every machine runs it on its own clock). */
USTRUCT()
struct FPinguKillCamPlan
{
	GENERATED_BODY()

	/** A new segment: every machine starts it again from when it arrived. */
	UPROPERTY()
	uint8 Serial = 0;

	UPROPERTY()
	EPinguKillCamStage Stage = EPinguKillCamStage::Off;

	/** The dilation the segment starts at, its ease-in power, its length (real seconds). */
	UPROPERTY()
	float From = 1.f;

	UPROPERTY()
	float Exponent = 2.f;

	UPROPERTY()
	float Duration = 0.f;

	UPROPERTY()
	TObjectPtr<APenguinCharacter> Victim = nullptr;

	UPROPERTY()
	TObjectPtr<APenguinCharacter> Hitter = nullptr;
};

namespace PinguKillCam
{
	/** Slowest point of the curve. */
	constexpr float StartDilation = 0.05f;
	/** A shot that hits within this many seconds of game time can't be escaped any more (no control: LongShotWindow). */
	constexpr float ShotWindow = 0.15f;
	constexpr float LongShotWindow = 0.25f;
	/** A doomed flight starts the slow motion at most this long (game time) before the splash. */
	constexpr float MaxLead = 0.75f;
	/** ... and trying everything it can't make the fall last more than this much longer (game s). */
	constexpr float MaxStretch = 1.f;
	/** The whole slow motion: about this long in real time (RealMin .. RealMax). */
	constexpr float RealMin = 2.2f;
	constexpr float RealMax = 3.4f;
	constexpr float RealPerGame = 2.5f;
	/** A false trigger eases back to 1x in this long (real s). */
	constexpr float ReleaseTime = 0.4f;
	/** The predicted knock-out is this late (game s) without happening: it was a false trigger. */
	constexpr float LateSlack = 0.5f;
	/** The prediction says "saved" this many frames in a row after the trigger: a false trigger. */
	constexpr int32 SavedFramesToRelease = 3;
	/** The death animation after a fall into the sea (the splash and its spout), game s. */
	constexpr float SeaDeathShowTime = 0.9f;

	/** d(T) of a segment (T real seconds into it). */
	float CurveAt(float From, float Exponent, float Duration, float T);
	/** The game time a segment covers (its integral). */
	float GameTimeOf(float From, float Exponent, float Duration);
	/** A plan from StartDilation that covers GameSeconds in about RealPerGame x of it, OutDuration in RealMin..RealMax (the
	 *  power from 1 to 4; more game time than fits: an even ramp, a little longer). */
	void PlanCurve(float GameSeconds, float& OutFrom, float& OutExponent, float& OutDuration);
	/** From the dilation it is at now, exactly GameSeconds more with the same power: OutDuration. */
	float PlanRest(float From, float Exponent, float GameSeconds);

	/** What PredictFlight found. */
	struct FFlight
	{
		/** It can't be saved: every way back it has ends in the sea. */
		bool bDoomed = false;
		/** Not predictable right now (on the ground, hanging in a tornado, frozen, ...): never doomed. */
		bool bUnknown = false;
		/** Game seconds until it reaches the sea: the latest of the ways tried, and doing nothing (the earliest). */
		float SplashIn = 0.f;
		float EarliestSplashIn = 0.f;
		/** Where it reaches the sea. */
		FVector SplashAt = FVector::ZeroVector;
		/** Why it is saved (logs, tests): "land", "reach", "trait", "time", ... */
		const TCHAR* Saved = TEXT("");
	};
	/** Server: can Victim, flying or falling as it is now, still get back onto the island? */
	FFlight PredictFlight(const APenguinCharacter* Victim);

	/** Server: a deadly shot / stomp reaching Victim within its window: game seconds until it hits (OutBy: whose), or < 0. */
	float PredictDeadlyHit(const APenguinCharacter* Victim, APenguinCharacter*& OutBy);

	/** Real time without the slowed part of the kill cam (FPlatformTime::Seconds() minus what the slow motion took away on this
	 *  world): the host's judge of a joiner's moves runs on it. */
	double PacedSeconds(const UWorld* World);
}

/**
 * The kill cam: one per game world (the server spawns it, clients get it by replication). See the file comment.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguKillCam : public AActor
{
	GENERATED_BODY()

public:
	APinguKillCam();

	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** This world's kill cam (null on a client before it arrived). */
	static APinguKillCam* Get(const UObject* WorldContext);

	/**
	 * Server, APenguinGameMode::CheckRoundEnd: Victim's knock-out ended the round. The slow motion plays its death animation
	 * (planned again if it was predicted, else it starts now). Returns the real seconds until it is over (the winner banner
	 * waits for it); 0 when there is none.
	 */
	float OnFinalKnockOut(APenguinCharacter* Victim);

	EPinguKillCamStage GetStage() const { return Plan.Stage; }
	/** The dilation the curve wants on this machine right now (1 when off). */
	float GetDilation() const { return CurrentDilation; }
	/** Real seconds left of the current segment on this machine. */
	float GetRemaining() const;
	APenguinCharacter* GetVictim() const { return Plan.Victim; }
	APenguinCharacter* GetHitter() const { return Plan.Hitter; }
	/** The camera's zoom toward the victim on this machine (0 .. 1). */
	float GetZoomBlend() const { return ZoomBlend; }

	// ---- tests (server)
	/** Treat Penguin as the round's last victim (practice: no round to end). Null: the game's rule again. */
	void SetTestFinalVictim(APenguinCharacter* Penguin);
	/** What happened since the last reset: triggers by kind, false triggers, the last plan's numbers. */
	struct FTestStats
	{
		int32 Predicted = 0;
		int32 PredictedShot = 0;
		int32 AtKnockOut = 0;
		int32 FalseTriggers = 0;
		int32 Showcases = 0;
		/** Game seconds from the knock-out until the curve reached 1 (want the death animation's length). */
		float KOToNormalGame = -1.f;
		/** The death animation's length the last showcase was planned for. */
		float ShowTime = -1.f;
		/** Real seconds from the first trigger to 1x. */
		float TotalReal = -1.f;
		/** Lowest dilation seen, the dilation at the end. */
		float Lowest = 1.f;
		float AtEnd = 1.f;
		/** A flight: game seconds before the splash it became doomed (the point of no return), and when it triggered. */
		float NoReturnLead = -1.f;
		float TriggerLead = -1.f;
	};
	const FTestStats& GetTestStats() const { return Stats; }
	void ResetTestStats() { Stats = FTestStats(); }

private:
	UFUNCTION()
	void OnRep_Plan();

	/** Server: a new segment (replicated). */
	void StartSegment(EPinguKillCamStage Stage, float From, float Exponent, float Duration, APenguinCharacter* Victim, APenguinCharacter* Hitter);
	/** This machine starts running Plan now. */
	void BeginLocalSegment(double ClockOffset);
	/** Server: look for a knock-out that can't be escaped any more (Off), watch the one predicted (Approach). */
	void TickServer(float GameDt);
	void Release(const TCHAR* Why);
	bool IsFinal(const APenguinCharacter* Penguin) const;
	/** The curve's value now on this machine; Off at its end. */
	void UpdateCurve();
	void UpdateCamera(float RealDt);
	void ApplyDilation();
	/** A LAN client: after the engine's replication of the frame (which may bring the host's own time dilation), ours again. */
	void OnPostTickDispatch();
	double Clock() const;

	UPROPERTY(ReplicatedUsing = OnRep_Plan)
	FPinguKillCamPlan Plan;

	/** This machine's clock when it started Plan. */
	double SegmentStart = 0.0;
	float CurrentDilation = 1.f;
	bool bSegmentRunning = false;
	/** The plan's serial this machine has started. */
	uint8 LocalSerialSeen = 0;
	/** The stage as this machine runs it (its own segment's end: Off again). */
	EPinguKillCamStage LocalStage = EPinguKillCamStage::Off;

	// server
	double TriggerWorldTime = -1.0;
	float PredictedGameIn = 0.f;
	/** The latest the predicted knock-out may come (trying everything) before it counts as a false trigger. */
	float PredictedLatestIn = 0.f;
	int32 SavedFrames = 0;
	/** The trigger was a deadly shot (else a flight). */
	bool bShotTrigger = false;
	double KOWorldTime = -1.0;
	TWeakObjectPtr<APenguinCharacter> TestFinalVictim;
	bool bTestKOHandled = false;
	/** The candidate found doomed first (its splash then: how early the point of no return was). */
	TWeakObjectPtr<APenguinCharacter> DoomedVictim;
	float DoomedSplashIn = -1.f;

	// camera (every machine)
	float ZoomBlend = 0.f;
	FVector LastVictimSpot = FVector::ZeroVector;
	bool bVictimSpotKnown = false;
	TWeakObjectPtr<APenguinCharacter> ZoomVictim;
	TWeakObjectPtr<APenguinCharacter> ZoomHitter;
	float ZoomHoldLeft = 0.f;
	/** A kill was shown this round (the banner's wider view follows it). */
	bool bKillShown = false;
	/** A new kill began: the camera snaps to it on its first frame. */
	bool bSnapPending = false;
	TWeakObjectPtr<APenguinCamera> CameraWithPrerequisite;

	FDelegateHandle PostDispatchHandle;
	FTestStats Stats;
	double FirstTriggerClock = -1.0;
	double KOGameTime = -1.0;
};

/** Spawns the kill cam on the server / in solo and keeps the paced clock of the world. */
UCLASS()
class PENGUIN_API UPinguKillCamWorld : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	/** Real seconds the slow motion took away from this world so far. */
	double LostSeconds = 0.0;
	TWeakObjectPtr<APinguKillCam> KillCam;
};

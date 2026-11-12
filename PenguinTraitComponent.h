// Pingu - a penguin's trait (Traits mode): which one it has this round, its reveal, and what the trait did.
//
// LAN: the server sets the trait (APenguinGameMode deals one to every penguin each round) and runs every trait effect.
// FPinguTraitState and FPinguTraitEvents replicate to every machine (the penguin is always relevant): the owner's movement
// simulation reads the numbers from there (UPenguinMovementComponent::GetTraitNumbers), and every machine builds the
// trait's look and the reveal from them by itself, timed on the flow clock (APenguinGameState::GetFlowTime) - no
// multicasts. A machine that joins later just gets the state: the look shows quietly.
//
// One component, three files:
//   PenguinTraitComponent.cpp  the state, its replication, the reveal clock, the numbers (core)
//   PenguinTraitEffects.cpp    each trait's gameplay: TickEffects and the hooks the abilities, hits and the sea call
//   PenguinTraitLooks.cpp      each trait's look on every machine: TickLooks, OnLookStateChanged
// Each part keeps its own state in its own struct (FPinguTraitEffectsState / FPinguTraitLooksState, defined in its .cpp)
// and the objects it makes (components, materials) in TraitObjects, so it never needs this header changed.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PenguinTypes.h"
#include "PinguTraitTable.h"
#include "PenguinTraitComponent.generated.h"

class APenguinCharacter;
class APenguinProjectile;
struct FPinguTraitEffectsState;
struct FPinguTraitLooksState;

/** The trait as every machine has it. */
USTRUCT()
struct FPinguTraitState
{
	GENERATED_BODY()

	UPROPERTY()
	EPinguTrait Trait = EPinguTrait::None;

	/** Bumped by every SetTrait (a new round's trait, even the same one): every machine builds the look again. */
	UPROPERTY()
	uint8 Serial = 0;

	/** Flow time (APenguinGameState::GetFlowTime) the reveal begins; -1: no reveal, the look shows at once. */
	UPROPERTY()
	float RevealStart = -1.f;
};

/** What the trait did, for every machine to show (the gameplay writes it on the server: PenguinTraitEffects.cpp). */
USTRUCT()
struct FPinguTraitEvents
{
	GENERATED_BODY()

	/** Mirror: flow time it can reflect a shot again. */
	UPROPERTY()
	float MirrorReadyAt = 0.f;

	/** Second Chance: flow time the fish threw it back up (-1: not this round). */
	UPROPERTY()
	float SecondChanceAt = -1.f;

	/** Gambler: bumped by every roll that was not a plain use; bGambleDouble says which (a double, else a fizzle). */
	UPROPERTY()
	uint8 GambleSerial = 0;

	UPROPERTY()
	bool bGambleDouble = false;

	/** Gambler: the slot (0..2) and ability of the roll above, so every machine's overhead icon can crumble to dust (a fizzle)
	 *  or shine (a double). 255: no slot known (a roll whose ability never came out, a new trait). */
	UPROPERTY()
	uint8 GambleSlot = 255;

	UPROPERTY()
	uint8 GambleAbility = 0;

	/** Jester: bumped by every swap. */
	UPROPERTY()
	uint8 JesterSwaps = 0;

	/** Leech: bumped by every hit that cut its cooldowns (the joiners' judge allows the cut: 2026-11-05, host-cheat-fix). */
	UPROPERTY()
	uint8 LeechCuts = 0;

	/** Damage %: the damage it took this round, in percent (+12 a hit; every machine shows it under the penguin). */
	UPROPERTY()
	uint16 DamagePercent = 0;

	/** Recoil: bumped by every push back it got from a hit it landed. */
	UPROPERTY()
	uint8 Recoils = 0;
};

/** Server only: what the traits did, for the tests' logs. */
struct FPinguTraitCounters
{
	int32 Uses = 0;
	int32 Doubles = 0;
	int32 Fizzles = 0;
	int32 Swaps = 0;
	int32 Reflections = 0;
	int32 SecondChances = 0;
	int32 IceCells = 0;
	/** Damage %: hits that made it grow; Recoil: push backs; Deadly Edges: knock-outs past a side; Ice Rink: blocks it froze. */
	int32 DamageHits = 0;
	int32 RecoilPushes = 0;
	float LastRecoilSpeed = 0.f;
	int32 EdgeKnockOuts = 0;
	int32 RinkCells = 0;
	int32 MeltedBlocks = 0;
	float LeechSeconds = 0.f;
	/** Second Chance's last throw (TrySecondChance): where it started, its velocity (X right, Z up), the middle of the island it
	 *  aimed at and the ground nearest it, the island's highest block top; bRescueAimed: there was an island to aim at;
	 *  bRescueTooFar: even the steepest throw allowed falls short of the island. */
	FVector RescueStart = FVector::ZeroVector;
	FVector RescueThrow = FVector::ZeroVector;
	float RescueMidX = 0.f;
	float RescueLandX = 0.f;
	float RescueHighestTop = 0.f;
	bool bRescueAimed = false;
	bool bRescueTooFar = false;

	FString ToString() const;
};

/**
 * A penguin's trait: the state (replicated), the reveal's clock and the trait's numbers, plus the hooks through which
 * the gameplay (abilities, hits, projectiles, the sea) asks the trait. Every hook is neutral while the penguin has no
 * trait (and while Traits mode is off nobody has one): the game plays exactly as without traits.
 */
UCLASS(ClassGroup = (Penguin), meta = (BlueprintSpawnableComponent))
class PENGUIN_API UPenguinTraitComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPenguinTraitComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ------------------------------------------------------------------ the reveal (seconds after RevealStart)
	/** The cards show this long (the reveal widget: PinguUITraits.cpp) ... */
	static constexpr float CardsTime = 3.f;
	/** ... then fly into their penguins ... */
	static constexpr float FlyTime = 0.45f;
	/** ... and the penguin changes (its look, the body's shape, the base size): from TransformAt for TransformTime. */
	static constexpr float TransformTime = 0.35f;
	static constexpr float TransformAt = CardsTime + FlyTime;
	static constexpr float RevealEndAt = TransformAt + TransformTime;

	// ------------------------------------------------------------------ every machine
	EPinguTrait GetTrait() const { return State.Trait; }
	bool HasTrait(EPinguTrait InTrait) const { return State.Trait == InTrait; }
	/** A real trait (Traits mode, during a match). */
	bool HasAnyTrait() const { return PinguTraits::IsReal(State.Trait); }
	const FPinguTraitInfo& GetInfo() const { return PinguTraits::GetInfo(State.Trait); }
	/** The trait's numbers (neutral without a trait). The movement reads them every move: on the owner too. */
	const FPinguTraitNumbers& GetNumbers() const { return GetInfo().N; }
	const FPinguTraitState& GetState() const { return State; }
	const FPinguTraitEvents& GetEvents() const { return Events; }
	/** Seconds since this trait's reveal began on the flow clock: negative before it starts (the round's banner), a large
	 *  number when it has none (the look shows at once). */
	float GetRevealAge() const;
	/** The trait has changed the penguin (from TransformAt of its reveal on, or at once without one). Every machine. */
	bool IsTransformed() const { return GetRevealAge() >= TransformAt; }
	float GetCooldownScale() const { return GetNumbers().CooldownScale; }
	float GetKnockbackTaken() const { return GetNumbers().KnockbackTaken; }
	float GetKnockbackDealt() const { return GetNumbers().KnockbackDealt; }
	/** Damage %: how much further hits send it now (1 + its damage; 1 with any other trait). */
	float GetDamageKnockback() const { return State.Trait == EPinguTrait::DamagePercent ? 1.f + Events.DamagePercent * 0.01f : 1.f; }
	/** Damage %: its damage in percent (0 with any other trait). */
	int32 GetDamagePercent() const { return State.Trait == EPinguTrait::DamagePercent ? Events.DamagePercent : 0; }
	/** 3, or 2 (Second Chance: slots 0 and 2, slot 1 stays empty). */
	int32 GetNumAbilities() const { return GetNumbers().NumAbilities; }
	bool IsFreezeImmune() const { return GetNumbers().bFreezeImmune; }
	APenguinCharacter* GetPenguin() const;
	/** The server or a solo game: trait gameplay runs here. */
	bool IsServer() const;

	// ------------------------------------------------------------------ server
	/** Server: this round's trait. InRevealStart: the flow time its reveal begins (< 0: none - the look and the base size
	 *  come at once). Every call is a new Serial (also the same trait again). None takes the trait away. */
	void SetTrait(EPinguTrait NewTrait, float InRevealStart);
	void ClearTrait() { SetTrait(EPinguTrait::None, -1.f); }
	/** Server: the events to change (the replication sends them on). */
	FPinguTraitEvents& GetMutableEvents() { return Events; }
	const FPinguTraitCounters& GetCounters() const { return Counters; }
	FPinguTraitCounters& GetMutableCounters() { return Counters; }

	// ------------------------------------------------------------------ gameplay hooks (PenguinTraitEffects.cpp)
	/** Server, UPenguinAbilityComponent::UseAbility before the ability runs: the strength of this use (1 = normal; 0 = a
	 *  fizzle). Without a trait: 1. */
	float RollUseStrength(EPenguinAbility Ability);
	/** Server: the ability in Slot was used (with that strength). */
	void OnAbilityUsed(int32 Slot, EPenguinAbility Ability, float Strength);
	/** Tests, server: Gambler's next rolls come out as Mode says - 0 every roll a fizzle, 1 every roll a double, -1 (the
	 *  default) the random roll. Kept until it is set again. */
	void SetForcedGamble(int32 Mode);
	/** Server, APenguinCharacter::ApplyPenguinHit: a hit of ours landed on Victim. */
	void OnHitLanded(APenguinCharacter* Victim);
	/** Server, APenguinCharacter::ApplyPenguinHit: we were knocked (Push, cm/s, X right, Y up) by By (may be null or us). Damage %
	 *  grows ours; Recoil (By's trait) pushes By back with half of it. */
	void OnKnockbackTaken(APenguinCharacter* By, const FVector2D& Push);
	/** Server, APenguinProjectile: Shot is about to hit us at Where. True: the trait took it (the shot does not hit). */
	bool InterceptShot(APenguinProjectile* Shot, const FVector& Where);
	/** Server, APenguinCharacter::Tick: we are about to fall into the sea. True: the trait saved us (no knock-out). */
	bool TrySecondChance();
	/** Server: the transform of the reveal (or at once without one): the trait's base size and whatever changes then. */
	void OnTransformed();

	// ------------------------------------------------------------------ looks hook (PenguinTraitLooks.cpp)
	/** Every machine: a new trait state or new events arrived (or were set here): the look follows. */
	void OnLookStateChanged();

	/** Objects the effects and the looks make (components, materials): kept from garbage collection here. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> TraitObjects;

private:
	// effects (PenguinTraitEffects.cpp)
	/** Server, every frame while the penguin has a trait. */
	void TickEffects(float DeltaTime);
	/** Server: a new trait (or none) - whatever the last one was doing stops. */
	void ResetEffects();

	// looks (PenguinTraitLooks.cpp)
	/** Every machine, every frame. */
	void TickLooks(float DeltaTime);
	/** Every machine: the component goes (the penguin left): take the look down. */
	void EndLooks();

	/** Server: runs OnTransformed once per trait, when its reveal reaches TransformAt. */
	void UpdateTransform();

	UFUNCTION()
	void OnRep_State();

	UFUNCTION()
	void OnRep_Events();

	UPROPERTY(Transient, ReplicatedUsing = OnRep_State)
	FPinguTraitState State;

	UPROPERTY(Transient, ReplicatedUsing = OnRep_Events)
	FPinguTraitEvents Events;

	FPinguTraitCounters Counters;
	/** Server: OnTransformed ran for the current Serial. */
	bool bTransformDone = true;

	/** The effects' and the looks' own state (made by their files, on first use). */
	TSharedPtr<FPinguTraitEffectsState> EffectsState;
	TSharedPtr<FPinguTraitLooksState> LooksState;
};

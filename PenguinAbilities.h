// Pingu - the three random abilities every penguin brings into a round.
//
// LAN (v2): abilities are server gameplay. The server (or a solo game) runs UseAbility and everything it causes; the
// component replicates what every machine needs to show it: the loadout, the cooldowns (as end times on the server
// clock, so each machine counts them down by itself), the use / swap counters and the ice shell / frost breath state.
// Clients never run ability gameplay; their copies of the server-only functions do nothing.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "PenguinTypes.h"
#include "PinguTuning.h"
#include "PenguinAbilities.generated.h"

class APenguinCharacter;
class UPenguinTraitComponent;
class APenguinProjectile;
class APinguPickup;
class ABlockIsland;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** Where an icicle throw is, as this machine shows it (the arm, the icicle in the flipper). */
enum class EPinguIcicleShow : uint8
{
	None,
	/** The key is down: the icicle is pulled out of the back, the arm cocks over the head and draws back as it charges. */
	Charging,
	/** Let go: the arm swings forward (the icicle leaves the flipper after IcicleSwingTime) and follows through. */
	Throwing,
};

/** The icicle throw as the server has it, for every machine to show (UPenguinAbilityComponent::IcicleRep). */
USTRUCT()
struct FPinguIcicleThrowRep
{
	GENERATED_BODY()

	/** Bumped by every new charge (its key went down). */
	UPROPERTY()
	uint8 Count = 0;

	/** 0 nothing (dropped quietly), 1 charging, 2 thrown (the swing started), 3 cancelled (the icicle broke in the flipper), 4 melted (held too long at full charge). */
	UPROPERTY()
	uint8 Stage = 0;
};

/** Static data for one ability (UI + bots). */
struct FPenguinAbilityInfo
{
	EPenguinAbility Ability = EPenguinAbility::None;
	const TCHAR* Name = TEXT("");
	const TCHAR* Description = TEXT("");
	/** Texture /Game/UI/Icons/T_Icon_<IconName>. */
	const TCHAR* IconName = TEXT("");
	float Cooldown = 1.f;
	bool bDeadly = false;
	FLinearColor Color = FLinearColor::White;
	/** Bots: use it on a target closer than this (cm). */
	float BotRange = 800.f;
	/** Bots: needs a clear line to the target. */
	bool bBotNeedsLineOfSight = true;
};

/**
 * Holds a penguin's three abilities, their cooldowns and the code for every ability.
 * Players and bots both call UseAbility(Slot, AimPoint) - on the server: a LAN client's character sends its presses
 * there (Rq02). The getters work on every machine.
 */
UCLASS(ClassGroup = (Penguin), meta = (BlueprintSpawnableComponent))
class PENGUIN_API UPenguinAbilityComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPenguinAbilityComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Server: the hidden shadow's values go out, never an outside write's (GuardShadow right before replication). */
	virtual void PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker) override;

	static const FPenguinAbilityInfo& GetInfo(EPenguinAbility Ability);
	/** Count different random abilities, drawn evenly from all of them (no guaranteed deadly or knockback one since 2026-11-05). */
	static TArray<EPenguinAbility> RollAbilities(FRandomStream& Rand, int32 Count = 3);
	/** A round's loadout of NumAbilities (a trait's: UPenguinTraitComponent::GetNumAbilities): 3 is RollAbilities' three (the
	 *  same rolls); 2 is { A, None, B } - slot 1 (the pickup slot) stays empty (Second Chance). */
	static TArray<EPenguinAbility> RollLoadout(FRandomStream& Rand, int32 NumAbilities);
	/** Knocks out in one hit (icicle, belly stomp, iceberg, snow bomb). */
	static bool IsDeadly(EPenguinAbility Ability) { return GetInfo(Ability).bDeadly; }
	/** Knocks (or yanks) penguins around hard enough to ring them out (snowball, slap, gust, fish hook, noot). */
	static bool IsKnockback(EPenguinAbility Ability);
	/** The pushes (slap, gust, noot): using one braces you for BraceTime, and one that hits a braced penguin clashes on it
	 *  (APenguinCharacter::IsBraced). */
	static bool IsBracingPush(EPenguinAbility Ability)
	{
		return Ability == EPenguinAbility::FlipperSlap || Ability == EPenguinAbility::Gust || Ability == EPenguinAbility::Noot;
	}
	/** Hits that smash ice: a frozen penguin's ice block takes the first one and shatters - nothing happens to the penguin
	 *  inside, not even a knock-out (APenguinCharacter::ApplyPenguinHit). (Not the snowball: BouncesOffIce.) */
	static bool IsIceSmashing(EPenguinAbility Ability)
	{
		return Ability == EPenguinAbility::FlipperSlap || Ability == EPenguinAbility::BellyStomp
			|| Ability == EPenguinAbility::Icicle || Ability == EPenguinAbility::SnowBomb;
	}
	/** The snowball does not break a frozen penguin's ice: it bursts on the cube, which it only nudges along (still frozen,
	 *  still a shield; the hit is not one that landed: no combo, no refund). A bot never wastes one on a frozen target. */
	static bool BouncesOffIce(EPenguinAbility Ability) { return Ability == EPenguinAbility::Snowball; }
	/** Wind never breaks a frozen penguin's ice: it blows the ice cube away (still frozen, its ice still a shield). */
	static bool IsWind(EPenguinAbility Ability) { return Ability == EPenguinAbility::Gust || Ability == EPenguinAbility::Noot; }
	/** Walls block the area abilities (slap, gust, noot, frost breath): they reach Other only if a line from User's chest or
	 *  head to Other's middle or upper body is clear of Island's blocks (no island: no walls). Bots check it too. */
	static bool HasClearLine(const ABlockIsland* Island, const APenguinCharacter* User, const APenguinCharacter* Other);

	/** Picked-up abilities always go into this slot (the middle icon, middle mouse button). */
	static constexpr int32 PickupSlot = 1;

	/** Server only (does nothing on a LAN client): a new loadout, everything ready. */
	void SetAbilities(const TArray<EPenguinAbility>& InAbilities);
	const TArray<EPenguinAbility>& GetAbilities() const { return Abilities; }
	EPenguinAbility GetAbility(int32 Slot) const { return Abilities.IsValidIndex(Slot) ? Abilities[Slot] : EPenguinAbility::None; }
	int32 FindSlot(EPenguinAbility Ability) const { return Abilities.IndexOfByKey(Ability); }
	/** Seconds until the slot is ready again (every machine: from the replicated end time and the server clock). */
	float GetCooldownRemaining(int32 Slot) const;
	/** 1 = just used, 0 = ready. */
	float GetCooldownFraction(int32 Slot) const;
	/** The full length of the slot's current cooldown (s, every machine): the ability's cooldown x the trait's scale when it
	 *  was used; the ability's own cooldown when none is recorded. GetCooldownFraction divides by it. */
	float GetCooldownLength(int32 Slot) const;
	/** Server: every running cooldown ends Seconds sooner (Leech; one that gets to the end is ready at once). */
	void ReduceCooldowns(float Seconds);
	/** Server: Slot holds NewAbility from now on (Jester). Its cooldown carries over (end time and length); the overhead icon
	 *  pops as for a pickup (the swap count). A charging icicle swapped away just goes. */
	void ReplaceAbility(int32 Slot, EPenguinAbility NewAbility);
	/** Server, during UseAbility (and the icicle's throw): the strength of the use that runs right now (1 normally; Traits
	 *  mode: UPenguinTraitComponent::RollUseStrength). 1 outside a use. Projectiles take it when they are launched
	 *  (APenguinProjectile::GetStrength); an ability that acts later (a stomp's slam, the frost breath) keeps its own copy. */
	float GetUseStrength() const { return UseStrength; }
	/** Off cooldown - and not charging an icicle right now (as the machine controlling the penguin has it). */
	bool IsReady(int32 Slot) const;
	/** Server: every slot with this ability is ready again at once (the snowball combo: a hit gives it back). True if one
	 *  was cooling down. */
	bool RefundCooldown(EPenguinAbility Ability);
	/** The abilities that leave something solid or standing in the world (the build limit covers them: PinguBuildLimit): Ice
	 *  Wall, Iceberg Drop, Snowman. */
	static bool IsPlacingAbility(EPenguinAbility Ability);
	/** Server: would Ability aimed at AimPoint place its thing now (inside the build limit; the snowman: a free tile in front)?
	 *  Bots ask first, so they never try a refused one. True for every other ability. */
	bool IsPlacementAllowed(EPenguinAbility Ability, const FVector& AimPoint) const;
	/** Server, the snowball combo (PinguTuning::SnowballCombo*) of this penguin as the thrower: the number the next snowball hit
	 *  that lands will have in its chain (1 = the first: no chain, or it ran out of time - SnowballComboWindow since the last hit)
	 *  ... */
	int32 GetNextSnowballComboHit() const;
	/** ... books a snowball hit that landed (the caller refunds the throw) and returns its number in the chain. A throw that is
	 *  not a refunded one (the last one missed, was fizzled, or none came for a while) starts a new chain at the throw
	 *  (UseAbility). A new round / loadout (ResetAll) ends it too. */
	int32 NoteSnowballHit();
	/** The hits in the chain so far (0 = none; tests). */
	int32 GetSnowballComboHits() const { return SnowballComboHits; }
	/** Increments every time the slot is used (the HUD flashes the card). */
	int32 GetUseCount(int32 Slot) const { return UseCounts.IsValidIndex(Slot) ? UseCounts[Slot] : 0; }
	/** Increments every time a pickup changes or recharges the slot (the HUD pops the icon). */
	int32 GetSwapCount(int32 Slot) const { return SwapCounts.IsValidIndex(Slot) ? SwapCounts[Slot] : 0; }
	/** The slot's cooldown end on the server clock as replicated (0: ready) and its recorded length (0: none yet): the LAN
	 *  anti-cheat checks them against the rules (PinguAntiCheat.h). */
	double GetCooldownEnd(int32 Slot) const { return CooldownEnds.IsValidIndex(Slot) ? CooldownEnds[Slot] : 0.0; }
	float GetRecordedCooldownLength(int32 Slot) const { return CooldownLengths.IsValidIndex(Slot) ? CooldownLengths[Slot] : 0.f; }
	/** Server / solo: puts the loadout and the cooldown ends back from the hidden shadow when something outside the game
	 *  changed them (a memory trainer); true if it had to. Off with the dev switch -PinguCheat=noguard. */
	bool GuardShadow();
	/** How often GuardShadow had to put values back (tests). */
	int32 GetShadowRestores() const { return ShadowRestores; }
#if !UE_BUILD_SHIPPING
	/** Tests only: writes the replicated arrays directly, past the shadow, as a memory trainer would. */
	void DevTrainerWrite(int32 Slot, EPenguinAbility Ability, bool bZeroCooldown);
#endif

	/** Server only. A picked-up ability: an ability we already have is recharged, anything else replaces the PickupSlot
	 *  one (ready to use at once). Returns the slot that changed (INDEX_NONE on a LAN client). */
	int32 GiveAbility(EPenguinAbility Ability);

	/** Server only. Fire the ability in a slot toward a point on the play plane. False when it could not be used: on
	 *  cooldown, or the penguin can't use abilities now (UPenguinMovementComponent::CanUseAbilities: frozen, slap-dizzy,
	 *  knocked out, input locked, stomping - an ordinary hit stun, a dash or a slide don't stop it); and always on a LAN
	 *  client (the character sends the press to the server instead). A charged ability (the icicle) used this way is a full
	 *  charge, thrown at AimPoint as soon as it is pulled out (IciclePullOutTime + IcicleSwingTime later); the key's way is
	 *  BeginCharge / ReleaseCharge. */
	bool UseAbility(int32 Slot, const FVector& AimPoint);
	/** The abilities a penguin can use on itself (holding the key): the shrink and grow rays. */
	static bool CanUseOnSelf(EPenguinAbility Ability) { return Ability == EPenguinAbility::ShrinkRay || Ability == EPenguinAbility::GrowRay; }
	/** Server only. Uses the ability in Slot on the penguin itself (a held size ray key): aimed at its own chest as the
	 *  server has it, so a LAN player moving fast still hits itself. False if that ability can't (CanUseOnSelf) or it
	 *  could not be used. */
	bool UseAbilityOnSelf(int32 Slot);
	/** Server only: every cooldown ready, running effects stopped. */
	void ResetAll();

	// ------------------------------------------------------------------ the giant snowball: hold the key to roll it up
	/** Server: Slot's cooldown runs its whole length again from now (a giant snowball's roll-up let go too soon: it counts as
	 *  used). */
	void RestartCooldown(int32 Slot);
	/** The key of a giant snowball went up (or was forgotten): a roll-up still running in Slot (INDEX_NONE: any) crumbles - its
	 *  cooldown runs all the same. Server: at once; a LAN player's own machine sends it to the server. */
	void ReleaseRollUp(int32 Slot);

	// ------------------------------------------------------------------ the icicle: hold to throw faster and straighter, a tap is a weak lob
	/** Abilities whose key charges while it is held and that throw on the release (the icicle). */
	static bool IsChargedAbility(EPenguinAbility Ability) { return Ability == EPenguinAbility::Icicle; }
	/** The pull-out (s): the icicle comes out from behind the back and the arm cocks over the head. A key let go sooner still
	 *  throws - at the end of the pull-out. */
	static constexpr float IciclePullOutTime = PinguTuning::IciclePullOutTime;
	/** Held this long (from the press) is a full charge: the fastest, perfectly straight icicle. Holding longer stays at full. */
	static constexpr float IcicleFullChargeTime = PinguTuning::IcicleFullChargeTime;
	/** Held this long at full charge (from IcicleFullChargeTime on) the icicle melts: no throw, the cooldown starts. */
	static constexpr float IcicleMeltAfterFull = PinguTuning::IcicleMeltAfterFull;
	/** The forward swing (s): from the release (or the end of the pull-out) to the icicle leaving the flipper. */
	static constexpr float IcicleSwingTime = PinguTuning::IcicleSwingTime;
	/** The arm's follow-through (s) after the icicle has left. */
	static constexpr float IcicleFollowTime = PinguTuning::IcicleFollowTime;
	/** The charge (0 = a tap .. 1 = full) a key held this long gives. */
	static float GetIciclePower(float HoldSeconds);
	/** The throw a charge makes: speed (cm/s), gravity (cm/s^2) and how far the lob points above the aim (degrees). A full
	 *  charge: 2600, 0, 0 (perfectly straight); a tap: 600, 2800, 6 (a weak, heavy lob that drops after about 2 m). The speed
	 *  rises smoothly between them; the gravity and the lift fall to 0. */
	static void GetIcicleThrow(float Power, float& OutSpeed, float& OutGravity, float& OutLiftDegrees);
	/** The snow bomb's throw for a penguin whose chest (GetAimOrigin) is at AimOrigin, facing Facing (+1 / -1; only used when the
	 *  aim is almost straight above or below the chest), aiming at AimPoint: where the bomb leaves, its velocity (the low arc
	 *  through the aim, 45 degrees when the aim is out of reach) and whether it is dropped right at the thrower's feet on a short
	 *  fuse (aimed straight down at yourself). DoSnowBomb throws it; the bots use it to see where a bomb would land. */
	static void GetBombThrow(const FVector& AimOrigin, const FVector& AimPoint, float Facing, FVector& OutSpawn, FVector& OutVelocity, bool& bOutDropAtFeet);
	/** How built up the charge looks (0 .. 1) after the key was down ChargeSeconds: the hand icicle's shake and the arm's
	 *  tremble grow with it (the glow and the halo follow the plain charge). Every machine shows the same from the replicated
	 *  charge stage and its time. */
	static float GetChargeShakeBuild(float ChargeSeconds);

	/**
	 * The key of a charged ability (the icicle) went down: the icicle is pulled out and charges while the key is held (no
	 * cooldown yet). Server: the real charge. A LAN player's own machine shows it at once and sends it to the server. False if
	 * it can't be used now (cooldown, frozen, dizzy, knocked out, locked, already charging).
	 */
	bool BeginCharge(int32 Slot);
	/** The key went up: the icicle is thrown where the penguin aims now, as far as the hold made it (after the pull-out if
	 *  it is not done yet). The cooldown starts with the throw. A LAN player's machine sends the hold time it measured. */
	void ReleaseCharge(int32 Slot);
	/** The held key was forgotten (a menu opened, a new round): the charge ends without a throw (and without a cooldown -
	 *  unless the penguin can't use abilities any more: then it is a cancel). */
	void DropCharge();
	/** A charge that is let go by itself once it was held HoldSeconds (bots), aimed where the penguin aims then. Works where
	 *  the penguin is controlled (the server, or a LAN player's own machine). */
	bool ChargeAndThrow(int32 Slot, float HoldSeconds);

	/** What this machine shows of the icicle throw (stage, seconds in it, seconds the key was held - kept after the release). */
	EPinguIcicleShow GetIcicleShow() const { return IcicleShow.Stage; }
	float GetIcicleShowTime() const { return IcicleShow.StageTime; }
	float GetIcicleChargeTime() const { return IcicleShow.ChargeTime; }
	/** The side the penguin turns to while the throw shows (+1 right, -1 left; 0 when none shows): the side it aims at - it
	 *  follows the aim while the key is held and until the icicle has left the flipper, then holds through the follow-through,
	 *  however the penguin moves - so the icicle comes out of the front. The character turns its mesh there
	 *  (APenguinCharacter::GetVisualFacing); the movement's own facing is left alone. Every machine works it out itself. */
	float GetIcicleFacing() const { return IcicleShow.Stage == EPinguIcicleShow::None ? 0.f : IcicleShow.AimSide; }
	/** The icicle shows in the flipper (pulled out, not thrown yet). */
	bool IsIcicleInHand() const { return bHandIcicleShown; }
	/** Tests: where the icicle in the flipper is (its middle), where its tip points (unit vector) and how big it is drawn now (1:
	 *  full size; it shrinks away while the aim crosses over and the penguin turns round, is put in the other flipper at its
	 *  smallest and grows back, so it never jumps across the body in the picture). False while it does not show. */
	bool GetHandIcicleState(FVector& OutCenter, FVector& OutTip, float& OutScale) const;
	/** What the icicle in the flipper looks like right now (0 while it does not show), for the tests: how far the trembling
	 *  can throw it (cm, the envelope that grows with the hold) and how far it did throw it this frame (cm), its glow (the
	 *  emissive it is drawn with), its aura (the opacity of the cold glow laid along it) and the sparkles it sheds each second. */
	float GetHandShakeAmplitude() const { return HandShakeAmplitude; }
	float GetHandShakeOffset() const { return HandShakeOffset; }
	float GetHandGlow() const { return HandGlowNow; }
	float GetHandHalo() const { return HandHaloNow; }
	float GetHandSparkleRate() const { return HandSparkleRate; }
	/** The aura is drawn this frame (the cold glow along the icicle). */
	bool IsHandHaloShown() const { return bHandHaloShown; }
	/** Server: a charge runs (pulled out or held, not thrown yet). */
	bool IsChargingIcicle() const { return Charge.Slot != INDEX_NONE; }
	/** Where the penguin is controlled: a charge begun here (its key, or ChargeAndThrow) waits for its release. */
	bool IsChargeKeyHeld() const { return KeyChargeSlot != INDEX_NONE; }
	/** Server: the last icicle thrown - its charge (0..1), how long its key was held, and how many were thrown so far. */
	float GetLastIciclePower() const { return LastIciclePower; }
	float GetLastIcicleHold() const { return LastIcicleHold; }
	/** Server: the last icicle thrown - when its key went down and up and when it left the flipper (server time). */
	void GetLastIcicleTimes(double& OutPress, double& OutRelease, double& OutThrow) const
	{
		OutPress = LastIciclePressTime;
		OutRelease = LastIcicleReleaseTime;
		OutThrow = LastIcicleThrowTime;
	}
	int32 GetIciclesThrown() const { return IciclesThrown; }
	/** Server: charges cancelled so far (a stun, freeze, knock-out or the round's end while charging: no icicle). */
	int32 GetIciclesCancelled() const { return IciclesCancelled; }
	/** Charges that melted away (held IcicleMeltAfterFull s at full charge). */
	int32 GetIciclesMelted() const { return IciclesMelted; }

	bool IsShieldActive() const;
	float GetShieldRemaining() const;
	bool IsFrostActive() const;
	/** Server: a push (IsBracingPush) was used less than BraceTime ago, so pushes clash on this penguin (APenguinCharacter::
	 *  IsBraced). Hits are server gameplay: false on a LAN client. */
	bool IsBraced() const;

	/** The belly stomp hit the ground (called by the character; only the server's call does anything). */
	void OnStompLanded();

	/** Radius of the ice shell bubble. */
	static constexpr float ShieldRadius = 95.f;

	/** How long a push braces its user (s, server time). */
	static constexpr float BraceTime = 0.3f;

	/** Aim this close to your own chest (x your size) to use a size ray on yourself. */
	static constexpr float SelfCastRadius = 75.f;

private:
	APenguinCharacter* GetPenguin() const;
	/** The penguin's trait component (null only without a penguin). */
	UPenguinTraitComponent* GetTraits() const;
	/** Every per-slot array (cooldown ends and lengths, use and swap counts) as long as the loadout. */
	void SizeSlotArrays();
	FVector AimDirection(const FVector& AimPoint) const;
	/** The server or a solo game: ability gameplay runs here. */
	bool HasGameplayAuthority() const;
	/** This machine runs the penguin's movement: the server for its own penguins (host, bots, solo), the owning client
	 *  for a LAN player's penguin. */
	bool IsMovementSimulatedHere() const;
	/** The server's game clock, the same on every machine (the end times below are on it). */
	double GetServerNow() const;

	bool DoSnowball(const FVector& AimPoint);
	/** The icicle leaves the flipper with this charge (0..1): spawned the way the throw at that charge flies. */
	bool DoIcicle(const FVector& AimPoint, float Power);
	bool DoBellyStomp();
	bool DoIcebergDrop(const FVector& AimPoint);
	bool DoFlipperSlap(const FVector& AimPoint);
	bool DoFrostBreath(const FVector& AimPoint);
	bool DoIceWall(const FVector& AimPoint);
	/** The ice wall's cells for a use of Strength aimed at AimPoint (a column on the ground there, or a ledge in the air). False:
	 *  no island. */
	bool PlanIceWall(const FVector& AimPoint, float Strength, ABlockIsland*& OutIsland, TArray<FIntPoint>& OutCells, TArray<FIntPoint>& OutBigOrigins,
		FVector& OutTarget) const;
	/** IsPlacementAllowed for a use of Strength; refused: where (for the "no" puff) and why (the log). */
	bool CheckPlacement(EPenguinAbility Ability, const FVector& AimPoint, float Strength, FVector* OutRefusedAt, FString* OutWhy) const;
	/** The Iceberg Drop's target for a use of Strength aimed at AimPoint (its reach clamps it). */
	FVector GetIcebergTarget(const FVector& AimPoint, float Strength) const;
	bool DoSnowWarp(const FVector& AimPoint);
	bool DoFishHook(const FVector& AimPoint);
	bool DoIceShell();
	bool DoGust(const FVector& AimPoint);
	bool DoSnowBomb(const FVector& AimPoint);
	bool DoNoot(const FVector& AimPoint);
	bool DoIceGun(const FVector& AimPoint);
	bool DoSizeRay(const FVector& AimPoint, bool bGrow);
	/** The giant snowball: the roll-up starts (APinguGiantSnowball::BeginRollUp) toward the aim's side (or the facing). Only
	 *  standing on the ground. */
	bool DoGiantSnowball(const FVector& AimPoint);
	/** The snowman: built on the tile in front of the penguin toward the aim's side (APinguSnowman::FindSpot); none free: refused. */
	bool DoSnowman(const FVector& AimPoint);
	/** Flap: the penguin flutters up (UPenguinMovementComponent::StartFlap). */
	bool DoFlap();
	/** Server: a use whose strength came out 0 (Gambler's fizzle): nothing happens - no effect, no use counted - but half
	 *  the cooldown runs. The ability turns into sparkling dust where it would have come out (APinguFX::FizzleDust, on every
	 *  machine) with a sad fizzle sound; AimPoint (zero: the penguin's own aim) says which way. The overhead icon's dust
	 *  comes from the trait's events on every machine (SPinguOverheadAbilities). */
	void FizzleUse(int32 Slot, EPenguinAbility Ability, const FVector& AimPoint = FVector::ZeroVector);

	void TickStomp();
	void TickFrost(float DeltaTime);
	void TickShield(float DeltaTime);
	/** Our ice shell touching another penguin's: the two bubbles bounce apart (a touching pair at most every 0.4 s). */
	void BounceOffShells(APenguinCharacter* Penguin);
	/** The ice wall has risen into place (it is solid from the cast, DoIceWall): its build burst. */
	void RaiseIceWall();

	/** The frost blast pushes its breather away from the cursor (runs where the penguin's movement is simulated). */
	void TickFrostJet(float DeltaTime);
	/** The frost cloud: purely visual, every machine makes its own from the replicated breath. */
	void TickFrostCloud(float DeltaTime);
	/** Visual-only parts of an ability use that every machine makes by itself (the gust's ring of puffs). */
	void PlayLocalUseVisuals(EPenguinAbility Ability);
	/** Clients: the ice shell bubble follows the replicated shield. */
	void UpdateShieldVisual();

	// ---- the icicle charge
	/** Server: starts the charge in Slot (the checks as UseAbility's). */
	bool BeginChargeOnServer(int32 Slot);
	/** Server: the key of the charge in Slot was let go after HoldSeconds, aiming at AimPoint. */
	void ReleaseChargeOnServer(int32 Slot, const FVector& AimPoint, float HoldSeconds);
	/** Server: the charge ends without an icicle - bCancel (stunned, frozen, knocked out, the round over): the cooldown is
	 *  used and the icicle breaks in the flipper; else it just goes (a menu, a new loadout). */
	void EndChargeOnServer(bool bCancel, bool bMelt = false);
	/** Server: cancels, releases and throws the running charge when it is time. */
	void TickChargeOnServer();
	/** Where the penguin is controlled (server, or a LAN player's machine): a ChargeAndThrow's release when it is due. */
	void TickChargeRelease();
	/** The penguin's aim now (or straight ahead when it has none), as ActivateAbility takes it. */
	FVector GetCurrentAim() const;
	/** The side (+1 / -1) the throw shown here faces, given the one it faces now (0: none yet): the aim's side, kept (not
	 *  flipped) while the aim is nearly straight up or down. */
	float PickIcicleSide(float Current) const;
	/** A LAN player's own machine: our charge began here (shown at once), the server is told. */
	bool BeginChargeOnOwner(int32 Slot);
	/** The penguin can't use abilities right now as this machine knows it (stunned by a hit since FromHits, frozen, dizzy,
	 *  knocked out, locked, stomping). */
	bool IsChargeBroken(int32 HitsAtStart) const;

	UFUNCTION(Server, Reliable)
	void Rq12(int32 Slot);

	UFUNCTION(Server, Reliable)
	void Rq13(int32 Slot, const FVector_NetQuantize10& InAimPoint, bool bFromStick, float HoldSeconds);

	UFUNCTION(Server, Reliable)
	void Rq14();

	UFUNCTION(Server, Reliable)
	void Rq15(int32 Slot);

	// ---- what every machine shows of the throw (the arm: UPenguinAnimInstance; the icicle in the flipper: here)
	/** A new charge to show (Count: the replicated one it belongs to). */
	void ShowChargeBegin(uint8 Count);
	/** The key was let go: the swing starts now, or when the pull-out is done. */
	void ShowChargeRelease();
	/** No throw: the arm drops, the icicle breaks in the flipper (bBreak) or just goes. */
	void ShowChargeEnd(bool bBreak, bool bMelt = false);
	void TickIcicleShow(float DeltaTime);
	/** The little icicle in the near flipper: made on every machine (BeginPlay), placed every frame while it shows. */
	void CreateHandIcicle();
	void UpdateHandIcicle(float DeltaTime);

	UFUNCTION()
	void OnRep_IcicleThrow();

	UFUNCTION()
	void OnRep_Nc();

	UFUNCTION()
	void OnRep_Shield();

	UFUNCTION()
	void OnRep_Frost();

	// ---- replicated (written on the server only). The reflected names are meaningless on purpose (2026-11-05, card
	// host-cheat-fix): a memory trainer found "Abilities" and "CooldownEnds" by name in the running exe and overwrote them.
	// The code keeps its readable names through the references below (not reflected, so not in the exe's name table).
	UPROPERTY(Transient, Replicated)
	TArray<EPenguinAbility> Vq;
	TArray<EPenguinAbility>& Abilities = Vq;

	/** Server world time each slot is ready again (0 = ready). */
	UPROPERTY(Transient, Replicated)
	TArray<double> Tz;
	TArray<double>& CooldownEnds = Tz;

	/** The full length of each slot's last cooldown (s; 0: none recorded - the ability's own). */
	UPROPERTY(Transient, Replicated)
	TArray<float> Lw;
	TArray<float>& CooldownLengths = Lw;

	UPROPERTY(Transient, ReplicatedUsing = OnRep_Nc)
	TArray<int32> Nc;
	TArray<int32>& UseCounts = Nc;

	UPROPERTY(Transient, Replicated)
	TArray<int32> Sk;
	TArray<int32>& SwapCounts = Sk;

	// ---- the host's hidden copy of the loadout and the cooldown ends (2026-11-05, card host-cheat-fix). Not reflected and
	// masked with a key per loadout, so a memory trainer can neither find it by name nor by its values. Written only where the
	// game itself changes them (SyncShadow after every legit write); GuardShadow puts the real values back when the replicated
	// arrays differ from it (an outside write), IsReady goes by it. Server / solo only.
	struct FPinguSlotShadow
	{
		TArray<uint8> Ability;
		TArray<uint64> End;
		uint64 Key = 0;
		bool bValid = false;
	};
	FPinguSlotShadow Shadow;
	int32 ShadowRestores = 0;
	double ShadowLastLog = -100.0;
	/** Copies the loadout and cooldown ends into the shadow (authority only): every slot (INDEX_NONE; only right after
	 *  GuardShadow in the same function), one slot (the one just written), or ShadowGrowOnly (slots added since). */
	void SyncShadow(int32 OnlySlot = INDEX_NONE);
	static constexpr int32 ShadowGrowOnly = -2;
	/** A new key (a new loadout). */
	void RekeyShadow();
	/** The shadow's cooldown end of a slot (0: ready; the replicated one without a shadow). */
	double GetShadowEnd(int32 Slot) const;

	/** Server world time the ice shell pops (0 = no shell). */
	UPROPERTY(Transient, ReplicatedUsing = OnRep_Shield)
	double ShieldEndTime = 0.0;

	/** Server world time the frost breath ends (0 = none) and where it blows. */
	UPROPERTY(Transient, ReplicatedUsing = OnRep_Frost)
	double FrostEndTime = 0.0;

	UPROPERTY(Transient, Replicated)
	FVector FrostDirection = FVector(1.f, 0.f, 0.f);

	/** The icicle throw (every machine shows the charge, the throw or the break from it; its owner shows its own at once). */
	UPROPERTY(Transient, ReplicatedUsing = OnRep_IcicleThrow)
	FPinguIcicleThrowRep IcicleRep;

	// ---- server gameplay state
	/** The strength of the use running now (GetUseStrength). */
	float UseStrength = 1.f;
	/** The strength of the stomp armed now and of the frost breath blowing now (they act after their use). */
	float StompStrength = 1.f;
	float FrostStrength = 1.f;
	float ShieldLeft = 0.f;
	float FrostLeft = 0.f;
	/** Frozen by this breath (each penguin once). */
	TArray<TWeakObjectPtr<APenguinCharacter>> FrostVictims;
	/** Their ice shell held this breath off (no new block effects every frame; still frozen if the shell pops in time). */
	TArray<TWeakObjectPtr<APenguinCharacter>> FrostShellBlocks;
	/** Server world time the brace of our last push ends (IsBraced). */
	double BraceEndTime = 0.0;
	/** The snowball combo: hits in the chain so far, the server time of the last one, and whether the snowball slot is ready
	 *  because a hit refunded it (a throw made like that goes on with the chain; any other throw starts a new one). */
	int32 SnowballComboHits = 0;
	double SnowballComboLastHit = -1000.0;
	bool bSnowballFromRefund = false;
	/** A shell that bounced off ours, and when (this penguin handles the pair: BounceOffShells). */
	struct FShellBounce
	{
		TWeakObjectPtr<APenguinCharacter> Other;
		double Time = 0.0;
	};
	TArray<FShellBounce> ShellBounces;
	bool bStompArmed = false;
	/** When the stomp was armed, and whether the penguin's movement runs on another machine (its stomp shows up here a
	 *  moment later, so it is not "interrupted" while that state is still on its way). */
	double StompArmedTime = 0.0;
	bool bStompRemote = false;
	/** The movement's stomp landing counter when it was armed (it moves on when this stomp lands). */
	int32 StompLandsAtArm = 0;
	/** Ice wall: its cells (solid since the cast) while their look rises into place; the build burst when the timer runs out. */
	TArray<FIntPoint> PendingWallCells;
	TWeakObjectPtr<ABlockIsland> PendingWallIsland;
	float PendingWallLeft = 0.f;
	/** The flipper slap's full-strength lunge was already used in this jump. */
	bool bAirLungeUsed = false;
	/** Ability bubbles the snow warp passed through, handed over at the end of UseAbility. */
	TArray<TWeakObjectPtr<APinguPickup>> WarpGrabs;
	/** The icicle charge (server gameplay). */
	struct FIcicleCharge
	{
		/** The slot charging (INDEX_NONE: none). */
		int32 Slot = INDEX_NONE;
		/** Server time of the press. */
		double StartTime = 0.0;
		/** The movement's hit counter at the press: a hit that stuns since cancels it. */
		int32 HitsAtStart = 0;
		bool bReleased = false;
		/** Server time of the release, the hold it had (the key's machine measured it), where it aimed. */
		double ReleaseTime = 0.0;
		float HoldSeconds = 0.f;
		FVector Aim = FVector::ZeroVector;
		/** Server time the forward swing starts (the release, or the end of the pull-out); the icicle leaves IcicleSwingTime later. */
		double SwingAt = 0.0;
	};
	FIcicleCharge Charge;
	float LastIciclePower = 0.f;
	float LastIcicleHold = 0.f;
	double LastIciclePressTime = 0.0;
	double LastIcicleReleaseTime = 0.0;
	double LastIcicleThrowTime = 0.0;
	int32 IciclesThrown = 0;
	int32 IciclesCancelled = 0;
	int32 IciclesMelted = 0;

	// ---- the machine that controls the penguin (the server, or a LAN player's own machine): its key's charge
	/** The slot whose key charges here (INDEX_NONE: none), the world time of the press, the hit counter then. */
	int32 KeyChargeSlot = INDEX_NONE;
	double KeyChargeStart = 0.0;
	int32 KeyChargeHits = 0;
	/** ChargeAndThrow: let go by itself after this many seconds (< 0: by the key). */
	float KeyChargeAutoRelease = -1.f;
	/** LAN player's machine: the charge shown here before the server had it (its count), and since when (s). */
	bool bChargePredicted = false;
	float ChargePredictAge = 0.f;

	// ---- local visuals (every machine)
	float FrostFxTimer = 0.f;
	/** Clients: what the character was last told about the shell (OnShieldChanged). */
	bool bShieldVisualOn = false;
	/** Clients: use counts already seen (new uses play their local visuals). */
	TArray<int32> SeenUseCounts;

	/** The icicle throw this machine shows. */
	struct FIcicleShowState
	{
		/** The replicated charge it belongs to (IcicleRep.Count; a LAN owner's own: the one it will be). */
		uint8 Count = 0;
		EPinguIcicleShow Stage = EPinguIcicleShow::None;
		float StageTime = 0.f;
		/** Seconds since the press (frozen at the release). */
		float ChargeTime = 0.f;
		/** The side the penguin turns to (+1 right, -1 left, 0 not set): where it aims, followed until the icicle leaves. */
		float AimSide = 0.f;
		/** Let go before the pull-out was done: the swing starts when it is. */
		bool bReleasePending = false;
		bool bFullChargeShown = false;
		/** Begun this frame: its time starts with the next one (as the server's charge does). */
		bool bFresh = false;
	};
	FIcicleShowState IcicleShow;
	/** The little icicle in the near flipper and its glow. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> HandIcicle;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> HandIcicleMaterial;
	bool bHandIcicleShown = false;
	/** The flipper bone it is on (Hand_L / Hand_R). */
	FName HandIcicleBone;
	/** Where it is (its middle), where its tip points and how big it is drawn now (the tests' GetHandIcicleState). */
	FVector HandIcicleCenter = FVector::ZeroVector;
	FVector HandIcicleTip = FVector::ForwardVector;
	float HandIcicleDrawScale = 0.f;
	/** Which flipper it is in (true: Hand_L, the one seen turned right), the swap to the other one when the aim crosses over
	 *  (its age in s; < 0: none), and a freshly shown icicle takes the flipper of the side the penguin faces. */
	bool bHandLeft = true;
	/** The side (+1 right, -1 left) it is held up on: changes with the flipper. */
	float HandSide = 1.f;
	float HandSwapAge = -1.f;
	bool bHandSideFresh = false;
	float HandGlintTimer = 0.f;
	/** Frost sparkles it sheds while charging: the time to the next ones. */
	float HandSparkleTimer = 0.f;
	/** What it looks like now (the getters above). */
	float HandShakeAmplitude = 0.f;
	float HandShakeOffset = 0.f;
	float HandGlowNow = 0.f;
	float HandHaloNow = 0.f;
	bool bHandHaloShown = false;
	float HandSparkleRate = 0.f;
};

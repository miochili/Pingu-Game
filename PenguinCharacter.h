// Pingu - the playable penguin.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "GameFramework/Character.h"
#include "PenguinTypes.h"
#include "PenguinCharacter.generated.h"

class AController;
class UAnimSequence;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPenguinAbilityComponent;
class UPenguinBotComponent;
class UPenguinMovementComponent;
class UPenguinTraitComponent;
class USkeletalMesh;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
struct FInputActionValue;

/** One LOD of the penguin's mesh with its outline hidden (APenguinCharacter::GetOutlineInfo, for tests). */
struct FPenguinOutlineLODInfo
{
	/** Sections that draw the Outline slot (the ink hull), and how many of them are hidden right now. */
	int32 Sections = 0;
	int32 Hidden = 0;
	/** Triangles of the whole LOD, of its outline sections, and what is drawn (the hidden outline sections left out). */
	int32 Triangles = 0;
	int32 OutlineTriangles = 0;
	int32 DrawnTriangles = 0;
};

/**
 * The penguin. Movement lives in UPenguinMovementComponent, animation in UPenguinAnimInstance,
 * abilities in UPenguinAbilityComponent. This class wires input (players and bots use the same
 * functions), handles hits and knock-outs, and adds the "juice": quick turn with a peek at the
 * camera, squash & stretch, snow puffs, afterimages and sounds.
 *
 * The actor (capsule) never rotates: facing left/right is only the mesh's relative yaw. Anything that
 * turns the actor (a rotated spawn point, the controller) is undone, or the mesh would face the wrong way.
 *
 * LAN (v2): the penguin replicates to everybody, always (a knocked-out, hidden one too). What changes the match - hits,
 * knock-outs (also the sea), size, slippery feet, respawns, the stomp landing, a belly slide running into somebody (on
 * the slide state its owner sends) - happens only on the server; on a client
 * those functions do nothing. The server replicates the character's own state (slot + name, knock-out, victory, size,
 * the ability action clip, the aim) and forwards its high-level operations (round reset, knock-out, victory, size) with
 * Client RPCs to the owner of a remote player's penguin, whose own simulation owns the position. The owner's copy of
 * that state is written only by those (ordered) RPCs, everybody else gets it by replication. Juice, movement effects and
 * status visuals run on every machine from the movement component's state (made right everywhere by it) and the
 * replicated state; hit / knock-out / size effects are played by the server for everybody (FPinguFXBroadcastScope).
 * The cartoon death that follows a knock-out (squashed, frozen, sooty, sunk, the ghost floating up: UpdateDeathLook) every
 * machine plays by itself from the replicated knock-out state (bKnockedOut, KOReason, hidden).
 */
UCLASS(Config = Game)
class PENGUIN_API APenguinCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	APenguinCharacter(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PostInitializeComponents() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Does nothing: the controller never turns the penguin (facing is the mesh yaw). */
	virtual void FaceRotation(FRotator NewControlRotation, float DeltaTime = 0.f) override;

	UFUNCTION(BlueprintPure, Category = "Penguin")
	UPenguinMovementComponent* GetPenguinMovement() const { return PenguinMovement; }

	UFUNCTION(BlueprintPure, Category = "Penguin")
	UPenguinAbilityComponent* GetAbilities() const { return AbilityComponent; }

	/** The AI brain (enabled for bots and for the menu demo). */
	UFUNCTION(BlueprintPure, Category = "Penguin")
	UPenguinBotComponent* GetBot() const { return BotComponent; }

	/** The penguin's trait this round (Traits mode; none otherwise). Never null on a spawned penguin. */
	UPenguinTraitComponent* GetTraits() const { return TraitComponent; }

	// ------------------------------------------------------------------ network roles
	/**
	 * This machine runs the penguin's simulation (movement, input, bot brain): solo always; on the server its bots, the
	 * host's own penguin and penguins without a controller; on a LAN client only its own penguin. Everywhere else the
	 * penguin only shows what was replicated.
	 */
	bool IsLocallySimulated() const;
	/** Server: the penguin of a player on another machine (its owner simulates it; round resets, knock-outs, victory and
	 *  size changes are forwarded to it). False on clients and in solo games. */
	bool IsRemotePlayerPenguin() const;

	// ------------------------------------------------------------------ round flow
	/** Back to the spawn point (practice, the LAN lobby). Server only. (No UFUNCTION since 2026-11-05: a hook calling it by its
	 *  name mid-fight would revive the host's penguin and reset its cooldowns. Card host-cheat-fix.) */
	void Respawn();

	void SetSpawnLocation(const FVector& Location) { SpawnLocation = Location; }

	/** Alive, visible, full reset, at Location. Server only (the owner of a remote player's penguin follows). */
	void ResetForRound(const FVector& Location);
	void SetInputLocked(bool bLocked);
	/** The round winner's celebration (no more hits, victory pose). Server only. */
	void SetVictory(bool bInVictory);
	bool IsVictorious() const { return bVictory; }

	// ------------------------------------------------------------------ identity
	/** Server only: the slot and name replicate (the beanie colour follows on every machine). */
	void SetPlayerSlot(int32 Slot, const FString& Name);
	int32 GetPlayerSlot() const { return PlayerSlot; }
	const FString& GetDisplayName() const { return DisplayName; }
	/** Server only: the team of a team match (0..PinguMaxTeams-1), -1 when everybody plays for themselves. It replicates.
	 *  The beanie keeps the penguin's own colour: the team shows as a ring in the team's colour around its icons. */
	void SetTeam(int32 InTeam);
	int32 GetTeam() const { return Team; }
	/** Other is on our team (a team match). Never ourselves. Friendly fire: hits between teammates land like anyone's -
	 *  the team only decides the round's winner, the spawns, the rings, the statistics and whom the bots aim at. */
	bool IsTeammate(const APenguinCharacter* Other) const;
	/** The colour index the penguin wears: always its own slot's (also in a team match - everybody has their own colour). */
	int32 GetColorIndex() const { return PlayerSlot; }
	/** Bright UI colour of a colour index: a player slot 0..9 (red, blue, green, yellow, purple, orange, pink, cyan, white,
	 *  brown) - the ten teams use the same ten colours. */
	static FLinearColor GetSlotColor(int32 Slot);
	/** A team's colour and name ("RED"...): ten teams, so everybody can play in a team of their own. */
	static FLinearColor GetTeamColor(int32 InTeam) { return GetSlotColor(InTeam); }
	static FText GetTeamName(int32 InTeam);
	/** Linear tint for a portrait's beanie layer (T_PortraitFelt_N) of a colour index: the beanie's colour, vivid. */
	static FLinearColor GetBeanieTint(int32 ColorIndex);
	/** What the UI shows for this penguin: its own colour. */
	FLinearColor GetPlayerColor() const { return GetSlotColor(GetColorIndex()); }
	bool IsBot() const { return !IsPlayerControlled(); }
	/** On the lobby island a person's penguin stands still: its move, jump, dash and slide input does nothing (the bots stroll
	 *  by APinguLobby, the idles and the poke play). */
	bool IsInputBlockedByLobby() const;

	// ------------------------------------------------------------------ input (players and bots)
	/** Drive the penguin without a player (bots and the automated test). */
	void ApplyVirtualMoveInput(const FVector2D& Input);
	void PressJump();
	void ReleaseJump();
	void PressDash();
	/** The slide key: a belly slide on the ground, a belly-flop dive in the air (UPenguinMovementComponent::TryBellySlide;
	 *  a press it can't do right now waits a moment, as the dash does). */
	void BellySlide();
	/** Uses the ability in Slot toward the aim point. On a LAN client the server runs it (true = the request was sent;
	 *  the server decides: cooldown and UPenguinMovementComponent::CanUseAbilities - a knockback, dash or slide is fine). */
	bool ActivateAbility(int32 Slot);
	/** Dev builds, the anti-cheat test's cheating joiner (-PinguCheat=client): sends a use request for any slot number as it is,
	 *  no check here (the host must ignore what is illegal). Does nothing in Shipping. */
	void DevCheatRequestAbility(int32 Slot);
	/**
	 * A player's ability key went down / up (the real input calls this; tests may too). Most abilities fire on the press.
	 * A slot holding a shrink or grow ray fires on the release instead - unless the key is held for SelfCastHoldTime:
	 * then the ray is used on yourself at that moment (one size step, through the ray's own self-aim) and the release
	 * does nothing. Bots call ActivateAbility directly.
	 */
	void SetAbilityKeyDown(int32 Slot, bool bDown);
	/** Forgets held ability keys without firing anything (a menu opened, knocked out, a new round). */
	void CancelAbilityKeys();
	/** How long a size ray's key is held to use the ray on yourself (s). */
	static constexpr float SelfCastHoldTime = 0.45f;

	/** Where the penguin aims (mouse on the play plane, the stick, or a bot's target). */
	void SetAimPoint(const FVector& WorldPoint, bool bFromStick = false);
	FVector GetAimPoint() const { return AimPoint; }
	bool HasAimPoint() const { return bAimValid; }
	/** The aim comes from a pushed gamepad stick (pad players cast the size rays on themselves by pushing it straight down). */
	bool IsStickAim() const { return bAimFromStick; }
	/** Chest height: projectiles start here. */
	FVector GetAimOrigin() const { return GetActorLocation() + FVector(0.f, 0.f, 18.f); }
	FVector GetFeetLocation() const;
	float GetFacing() const;
	void FaceDirection(float DirectionX);
	FVector2D GetStickAim() const { return StickAim; }
	float GetLastStickAimTime() const { return LastStickAimTime; }

	// ------------------------------------------------------------------ combat
	/**
	 * CONTRACT (v2, LAN): gameplay code that moves a penguin directly (snow warp, an ice wall / iceberg pushing it out of
	 * the blocks) calls this instead of SetActorLocation / writing the velocity. On the server it also reaches the
	 * owning client of a remote player (whose own simulation is authoritative for its position).
	 * NewVelocity: X right, Z up. bForceFalling: leave the ground (the push lifts it out). bKeepSlide: a belly slide goes
	 * on from there (a portal); otherwise it ends.
	 */
	void NetTeleport(const FVector& Location, const FVector& NewVelocity, bool bForceFalling, bool bKeepSlide = false);

	/** An ability hit this penguin. False if it was blocked (ice shell, or a push clashing on a braced penguin) or the
	 *  penguin is already out. Server only. */
	bool ApplyPenguinHit(const FPenguinHit& Hit);
	/** Server: By moved us without a hit (wind blowing our ice shell along): pushed into the sea soon after, the knock-out
	 *  is By's, as after a hit. */
	void RememberAttacker(APenguinCharacter* By);
	/** Server: the fish hook yanks this frozen penguin along in its ice cube (APenguinProjectile::ThrowPenguinOverhead): the
	 *  ice holds for the flight (FlightTime: how long the throw's arc flies) and shatters at the first impact - on the
	 *  ground or against a wall (UpdateIceThrow). */
	void BeginIceThrow(float FlightTime);
	/** Flying in its ice cube, yanked by the fish hook (BeginIceThrow). Server. */
	bool IsIceThrown() const { return bIceThrown; }
	/** Server only. */
	void KnockOut(EPenguinKOReason Reason, APenguinCharacter* By, const FVector2D& Launch = FVector2D(0.f, 1000.f));
	bool IsKnockedOut() const { return bKnockedOut; }
	bool IsAlive() const { return !bKnockedOut; }
	/** How the penguin was knocked out (None while it is in; replicated, the owner's copy through Rq07). */
	EPenguinKOReason GetKOReason() const { return KOReason; }
	/** Server: the knock-out was the penguin's own doing (its own snow bomb: KnockOut's By was the penguin itself) - a
	 *  self-knock-out, a fall for it and no knock-out for anybody (the feed says it blew itself up). */
	bool WasKOSelfInflicted() const { return bKOSelfInflicted; }
	/** Server: whoever got the knock-out (null for the sea without a push, a self-knock-out ...). */
	APenguinCharacter* GetKOBy() const { return KOBy.Get(); }
	/** Server: whoever hit us last, if within the last MaxAge seconds of game time (the kill cam's hitter). */
	APenguinCharacter* GetLastAttacker(float MaxAge) const;
	/** Game seconds a knock-out's death animation plays before the penguin is hidden (the kill cam ends its slow motion then). */
	static float GetDeathShowTime(EPenguinKOReason Reason);
	/** The snowball combo's pop above this penguin (every machine reads it from the replicated state, SPinguOverheadAbilities):
	 *  the number of the combo hit that just landed on it (2 and up pop a "x2" ...) and a serial that goes up with every one. */
	uint8 GetComboPopSerial() const { return ComboPopSerial; }
	int32 GetComboPopCount() const { return ComboPopCount; }
	/** Server: a snowball hit that was the Count-th of its thrower's combo landed on this penguin. */
	void ShowComboHit(int32 Count);
	/**
	 * The knock-out's cartoon death this machine is playing (UpdateDeathLook), on every machine from the replicated
	 * knock-out state: squashed flat (a stomp), pressed into a spinning pancake (an iceberg / crushed), frozen in an ice
	 * block that shatters (an icicle), charred black on the spot (only the eyes white), standing half a second, then
	 * crumbling into a heap of black ash with the eyes on top that the wind blows away (a snow bomb), sunk with bubbles (the sea), shrinking to nothing (shrunk past the smallest size: no ghost), puffing
	 * up and wobbling until it pops (grown past the biggest size), today's tumble otherwise - and a little ghost floating up
	 * when the penguin vanishes (the snow bomb's once its pile is gone). ResetForRound ends it.
	 */
	bool IsDeathLookActive() const { return bDeathLookActive; }
	/**
	 * Puts the death look's mesh place back. The movement component calls it after its tick on a penguin this machine does
	 * not simulate (a client's view of anyone, the host's view of a remote player): the engine's network smoothing moves the
	 * mesh back to the feet-origin place there, and the spin would turn about the feet instead of the body's middle.
	 */
	void ReapplyDeathMeshLocation();
	/** Seconds since this machine's death look began (0 when none). */
	float GetDeathLookTime() const { return bDeathLookActive ? DeathLookTime : 0.f; }
	/** The death look's soot (a dark overlay over the whole penguin, a snow bomb's: on until the look ends) is on. */
	bool IsDeathSootShown() const { return bDeathSootShown; }
	/** 0..1: how much of the penguin's surface (the middle of it; the outline goes black first) the snow bomb's soot hides
	 *  now, read from the soot material on the mesh (0 without it). 1 = solid black. */
	float GetDeathSootCoverage() const;
	/** The death look time the snow bomb's charred penguin began crumbling into its pile (-1: not yet / no such look). */
	float GetDeathCrumbleTime() const;
	/** A snow bomb's ash pile (heap or eyes, on the effects actor) shows on this machine now. */
	bool IsDeathAshPileShown() const;
	/** 0..1: how much of the snow bomb's ash pile the wind has blown away (0 before the gust and without a pile). */
	float GetDeathAshBlown() const { return DeathAshBlown; }
	/** The death look's ice block (an icicle knock-out) shows. */
	bool IsDeathIceShown() const { return bDeathIceShown; }
	/** The mesh's normal relative scale (MeshScale, x the trait's body shape; size changes scale the actor): the juice and
	 *  the death look squash it. */
	FVector GetBaseMeshScale() const { return BaseMeshScale * TraitBodyScale * LobbyPopScale; }
	/** The trait's body shape (side, forward, up) on top of the mesh scale (1, 1, 1: none). Every machine sets its own (the
	 *  trait's look: UPenguinTraitComponent), local only. */
	void SetTraitBodyScale(const FVector& Scale) { TraitBodyScale = Scale; }
	FVector GetTraitBodyScale() const { return TraitBodyScale; }
	/** The lobby island (APinguLobby, every machine, local look): the beanie in this colour index - the team picked for the next
	 *  match - instead of the penguin's own (-1: its own again). */
	void SetHatColorIndex(int32 ColorIndex);
	/** The colour index the beanie shows (the hat's, else the penguin's own). */
	int32 GetHatColorIndex() const { return HatColorIndex >= 0 ? HatColorIndex : GetColorIndex(); }
	/** The lobby island's puff in / out (APinguLobby, every machine, local look): the whole mesh this big (1: normal). */
	void SetLobbyPopScale(float Scale) { LobbyPopScale = FMath::Max(Scale, 0.01f); }
	float GetLobbyPopScale() const { return LobbyPopScale; }
	bool IsShielded() const;
	/** Just used a push (Flipper Slap, Gust or Noot, UPenguinAbilityComponent::BraceTime ago at most): other pushes clash on
	 *  it instead of moving it. Server (hits are server gameplay): false on a LAN client. */
	bool IsBraced() const;
	/** Distance from a point to the penguin's capsule on the play plane (0 inside). */
	float DistanceTo(const FVector& Point) const;
	/** Plays an ability clip on top of the movement. Called on the server, it plays on every machine. */
	void PlayAction(EPenguinClip Clip, float Rate = 1.f);
	/** PlayAction with a direction on the play plane (X right, Z up), e.g. where the noot's horn points - on every machine. */
	void PlayAimedAction(EPenguinClip Clip, float Rate, const FVector& AimDirection);
	/** Dizzy from a long stun (a slap): stars circle its head, its eyes roll. */
	bool IsDizzy() const { return bDizzy; }
	/** The frozen penguin's ice cube is showing on this machine (tests). */
	bool IsIceBlockShown() const { return bIceShown; }
	/** The noot horn (the beak stretched out into a wide trumpet) is out. */
	bool IsHonking() const { return HornTime >= 0.f; }
	UStaticMeshComponent* GetNootHorn() const { return NootHorn; }
	/** Hides the penguin's black ink outline for good - the "Outline" material slot's section on every LOD of the mesh - and
	 *  the noot horn's. Called when the penguin spawns (BeginPlay); a penguin that already matches does nothing. The slot's
	 *  material is left alone (a trait's clipped outline stays). */
	void ApplyOutlineSetting();
	/** Tests: what ApplyOutlineSetting did on the mesh, per LOD (index = LOD). */
	void GetOutlineInfo(TArray<FPenguinOutlineLODInfo>& OutLODs) const;
	/** Tests: the noot horn's outline is drawn (bOutHasOutline: the horn mesh has an outline slot at all). */
	bool IsHornOutlineShown(bool* bOutHasOutline = nullptr) const;
	/** The way the penguin looks (its mesh turns there): where it faces, or while it honks the side the noot goes to, or while
	 *  it charges / throws an icicle the side it aims at (movement keeps its own facing; it turns back afterwards). */
	float GetVisualFacing() const;
	/** Honking, the body turns (about its base) so the beak and the horn point along the noot: how far (0..1) and the
	 *  beak's pitch above the facing direction then (degrees). The animation applies it. */
	float GetHonkWeight() const;
	float GetHonkPitch() const;
	/** A cheeky little dance (bots after a knock-out), only when standing around. */
	void TryTaunt();
	/** The right- or left-flipper version of a clip, whichever flipper faces the camera. */
	EPenguinClip PickNearFlipper(EPenguinClip RightFlipperClip, EPenguinClip LeftFlipperClip) const;
	/** Called by the ability component when the ice shell goes up or down (on every machine). */
	void OnShieldChanged(bool bActive);

	// ------------------------------------------------------------------ size and status
	/** Shrink / grow ray: multiplies the size (PinguMinSize .. PinguMaxSize) for Duration seconds, then it wears off. A
	 *  change past a limit knocks the penguin out instead (Shrunk / Overgrown; By gets the knock-out - nullptr or the
	 *  penguin itself: nobody). Server only. */
	void ChangeSize(float Factor, float Duration, APenguinCharacter* By = nullptr);
	float GetSizeScale() const { return SizeScale; }
	/** The size we are growing / shrinking toward. */
	float GetSizeTarget() const { return SizeTarget; }
	/**
	 * Server: the size the penguin rests at (a trait's: Tiny, Giant; 1 normally - a round reset puts it back to 1). Size
	 * rays stack on it (their steps are kept when it changes) and wear off back to it; the size limits stay the same
	 * absolute sizes. The penguin grows / shrinks to it like to a ray's size (a remote owner follows).
	 */
	void SetBaseSize(float Size);
	/** Every machine (it replicates). */
	float GetBaseSize() const { return BaseSize; }
	/** Seconds until a size change wears off (0: none). */
	float GetSizeTimeLeft() const { return SizeTimeLeft; }
	/** Radius of the ice shell bubble (grows with the penguin). */
	float GetShieldRadius() const;
	/** Ice gun: frozen feet slip on everything for Seconds. Server only. */
	void SetSlippery(float Seconds);

	// ------------------------------------------------------------------ animation
	void GetAnimInputs(FPenguinAnimInputs& Out) const;
	const UAnimSequence* GetAnimClip(EPenguinClip Clip) const;
	/** Number of clips that are loaded and ready. */
	int32 GetNumLoadedClips() const;

	/** The penguin mesh (soft reference: nothing is loaded when the game module starts). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Penguin|Animation")
	TSoftObjectPtr<USkeletalMesh> PenguinMesh;

	/** Animation clips (A_<Name> assets imported from Pingu.blend). Soft references, loaded on spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Penguin|Animation")
	TMap<EPenguinClip, TSoftObjectPtr<UAnimSequence>> AnimClips;

	/** Yaw (deg) the penguin turns toward the camera when facing left/right, so you see his face. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	float CameraPeekYaw = 34.f;

	/** How fast the penguin turns around (higher = snappier). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	float TurnSpeed = 24.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	float SquashStiffness = 380.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	float SquashDamping = 16.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	float JumpStretch = 6.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	float LandSquash = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	float DashStretch = 7.f;

	/** Snow puffs, afterimages and sounds for movement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Penguin|Visuals")
	bool bMovementEffects = true;

protected:
	UPROPERTY(Transient)
	TObjectPtr<UPenguinMovementComponent> PenguinMovement;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UPenguinAbilityComponent> AbilityComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UPenguinBotComponent> BotComponent;

	/** The trait (Traits mode): replicated state, the trait's gameplay hooks and its look. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UPenguinTraitComponent> TraitComponent;

	/** Ice shell bubble. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UStaticMeshComponent> ShieldBubble;

	/** Block of ice around a frozen penguin. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UStaticMeshComponent> IceBlock;

	/** Ice chunks on the feet while the ice gun makes them slippery. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UStaticMeshComponent> IceFootL;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UStaticMeshComponent> IceFootR;

	/** The noot: the beak stretched out into a wide trumpet (SM_NootHorn), shown while honking. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UStaticMeshComponent> NootHorn;

	/** Dizzy: little stars circling the head while a long stun lasts (a slap's). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Penguin")
	TObjectPtr<UInstancedStaticMeshComponent> DizzyStars;

	/** Hard references to the loaded clips, indexed by EPenguinClip. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> LoadedClips;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> BeanieMaterials;

	/** A snow bomb's soot: /Game/FX/M_Ghost (the afterimages' see-through unlit material), laid over the whole penguin as
	 *  its overlay material while the death look lasts; the char-up takes it from nothing (a hot glow) to solid soot black.
	 *  Made the first time it is needed, then kept. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SootMaterial;

private:
	// ------------------------------------------------------------------ RPCs (LAN)
	/** Owner -> server: use an ability; the server runs it with the aim the owner had at the press. */
	UFUNCTION(Server, Reliable)
	void Rq02(int32 Slot, const FVector_NetQuantize10& InAimPoint, bool bFromStick);

	/** Owner -> server: use the ability in Slot on yourself (a held size ray key); the server checks it can be. */
	UFUNCTION(Server, Reliable)
	void Rq01(int32 Slot);

	/** Owner -> server: where the owner aims, for the others to see (eyes). Throttled; presses carry their own aim. */
	UFUNCTION(Server, Unreliable)
	void Rq03(const FVector_NetQuantize10& InAimPoint, bool bFromStick);

	/** Owner -> server: round reset Serial is applied and the moves from the new place are on their way. */
	UFUNCTION(Server, Reliable)
	void Rq04(uint8 Serial);

	/** Owner -> server: respawn at will (lobby / practice only; the server checks the phase). */
	UFUNCTION(Server, Reliable)
	void Rq05();

	/** Server -> owner of a remote player's penguin: the high-level operations, so its own simulation follows. */
	UFUNCTION(Client, Reliable)
	void Rq06(const FVector_NetQuantize100& Location, uint8 Serial);

	UFUNCTION(Client, Reliable)
	void Rq07(EPenguinKOReason Reason, const FVector_NetQuantize10& LaunchVelocity);

	UFUNCTION(Client, Reliable)
	void Rq08();

	UFUNCTION(Client, Reliable)
	void Rq09(bool bInVictory);

	UFUNCTION(Client, Reliable)
	void Rq10(float NewTarget);

	UFUNCTION(Client, Reliable)
	void Rq11(float DirectionX);

	UFUNCTION()
	void OnRep_PlayerSlot();

	UFUNCTION()
	void OnRep_KnockOut();

	UFUNCTION()
	void OnRep_Size();

	UFUNCTION()
	void OnRep_Action();

	UFUNCTION()
	void OnRep_Aim();

	void Input_Move(const FInputActionValue& Value);
	void Input_MoveReleased(const FInputActionValue& Value);
	void Input_JumpPressed(const FInputActionValue& Value);
	void Input_JumpReleased(const FInputActionValue& Value);
	void Input_Dash(const FInputActionValue& Value);
	void Input_BellySlide(const FInputActionValue& Value);
	void Input_Respawn(const FInputActionValue& Value);
	void Input_Ability1(const FInputActionValue& Value);
	void Input_Ability2(const FInputActionValue& Value);
	void Input_Ability3(const FInputActionValue& Value);
	void Input_Ability1Released(const FInputActionValue& Value);
	void Input_Ability2Released(const FInputActionValue& Value);
	void Input_Ability3Released(const FInputActionValue& Value);
	void Input_Aim(const FInputActionValue& Value);
	void Input_AimReleased(const FInputActionValue& Value);

	/** A size ray key held down long enough: the ray on yourself (SetAbilityKeyDown). */
	void UpdateAbilityKeys();
	/** The ability in Slot aimed at our own penguin (the size rays' self-cast), the normal way: the server runs it. */
	bool ActivateAbilityOnSelf(int32 Slot);
	/** Server: a belly slide that runs into a penguin knocks it away (once it has gone BellySlideHitDistance) or stops
	 *  against it (a lying snow bomb in its way kicks itself along: APenguinProjectile::KickBySlide). */
	void UpdateBellySlideContacts();
	/** We, lying on our belly (a low capsule along the floor, the way we slide), touch the other's capsule - ShiftBack cm
	 *  back along the slide (where it began), or anywhere over the last SweepBack cm we slid (a LAN client's slide arrives
	 *  in steps: whoever it passed between two of them was touched too). */
	bool IsTouchingForSlide(const APenguinCharacter* Other, float ShiftBack = 0.f, float SweepBack = 0.f) const;
	/** Server: Pusher's push met us braced (ApplyPenguinHit): the clash effects here, and Pusher bounces off us. */
	void ClashWith(APenguinCharacter* Pusher);

	void UpdateJuice(float DeltaSeconds);
	/** Flap: the wind of each wing beat (a local cosmetic from the replicated flap state, APinguFX::FlapWind). */
	void UpdateFlapWind(float DeltaSeconds);
	float FlapWindClock = 0.f;
	int32 FlapWindBeat = 0;
	void UpdateMovementEffects(float DeltaSeconds);
	void UpdateStatusVisuals(float DeltaSeconds);
	void LoadPenguinAssets(bool bLoadClips);
	void ApplyBeanieColor();
	void ShowIceBlock(bool bShow);
	void ShowIceFeet(bool bShow);
	/** Fits the ice chunks onto the feet (IceFootL / IceFootR on the foot bones): around each foot as Pingu.blend has it,
	 *  through the mesh's reference pose (IceFootSize). */
	void FitIceFeet(USkeletalMeshComponent* SkelMesh);
	/** Finds where the noot horn's throat sits (on the beak, in the Body bone) and the beak's axes, through the mesh's
	 *  reference pose. */
	void FitNootHorn(USkeletalMeshComponent* SkelMesh);
	/** Out of the beak, shaped by the clock (NootHornShape); put away when it is over. */
	void UpdateNootHorn(float Dt);
	/** Stars round the head (and rolling eyes, the animation) while a long stun lasts - on every machine. */
	void UpdateDizzyStars(float Dt);
	/** Server: the ice cube yanked by the fish hook (BeginIceThrow) shatters at its first impact. */
	void UpdateIceThrow(float Dt);
	/** Server: the ice breaks against What (a crash of shards, a ring, the shatter sound) - the penguin inside is free. */
	void ShatterIceOnImpact(const TCHAR* What);
	void UpdateSize(float DeltaSeconds);
	void FinishKnockOut();
	/** Server: an overgrown penguin pops (Overgrown knock-out, OvergrownPopTime after it): a big blast like a snow bomb's
	 *  that pushes everybody around it away (no knock-out, no crater), played for everybody. */
	void OvergrownBlast();
	/** The island's ground is still right under the snow bomb's ash pile (DeathAshBase). */
	bool HasAshFloor() const;
	/** Every machine: plays the death look while the penguin is knocked out (starting it when this machine learns of the
	 *  knock-out: KnockOut, Rq07 or the replicated bKnockedOut) and ends it once it is back in. Local effects
	 *  only (each machine makes its own): no broadcast. */
	void UpdateDeathLook(float Dt);
	void StartDeathLook();
	/** Everything the death look changed back to normal: the mesh's scale and place, the ice block, the soot, the stars,
	 *  the ash pile (gone in a puff if the wind had not taken it all yet). */
	void EndDeathLook();
	/** On the floor (the feet on the island's ground, or the movement's own floor)? OutBase: the floor under the feet. */
	bool FindDeathFloor(FVector& OutBase) const;
	/** The snow bomb's charred penguin: where its eyeballs are now (DeathFaceEyes, DeathFaceEyeRadius: the white eyes lie
	 *  over them). False without the eye bones. */
	bool UpdateDeathFaceEyes();
	/** The mesh Scale (x BaseMeshScale, along the mesh's own axes), facing TurnYaw (the mesh's relative yaw: MeshYaw keeps
	 *  the way it faces, 0 faces the camera), then turned SpinDegrees about the camera axis (world Y), its body's middle at
	 *  Centre (actor space). */
	void SetDeathMeshTransform(const FVector& Scale, float SpinDegrees, const FVector& Centre, float TurnYaw);
	/** Actor space: where the body's middle is when the mesh, scaled ScaleZ high, stands with its feet on the capsule's floor. */
	FVector GetDeathFloorCentre(float ScaleZ) const;
	/** The little ghost floating up from Where (the body's middle), on this machine only. RiseSpeed < 0 sinks it. */
	void PlayDeathSpirit(const FVector& Where, float RiseSpeed, const FLinearColor& Color, float SizeShare, float Life);
	/** The dizzy stars circling the squashed penguin (the DizzyStars component, at Height over the actor's middle). */
	void UpdateDeathStars(float Height, float Radius);
	/** Zero actor rotation and no controller rotation (a spawn point or the editor camera may have turned us). */
	void KeepActorUnrotated();
	/** The part of a round reset that the simulating machine does: movement, place, status visuals, size, juice. */
	void ApplyRoundReset(const FVector& Location, bool bOnServer);
	/** The movement event counters as seen now (no juice or effects for events from before). */
	void SyncSeenCounters();
	/** Starts an action clip on this machine only (the replicated ones come through OnRep_Action). */
	void PlayLocalAction(EPenguinClip Clip, float Rate);
	/** Adds to the horizontal velocity, on the owner too. */
	void NudgeVelocityX(float DeltaX);
	/** Server: keeps the replicated aim close to the aim point (others' eyes). */
	void UpdateReplicatedAim();
	/** Owner -> server aim (throttled), round reset acknowledgement, the server's stale-position guard. */
	void UpdateNetSync(float DeltaSeconds);

	FVector SpawnLocation = FVector::ZeroVector;
	FVector BaseMeshScale = FVector::OneVector;
	/** The trait's body shape (SetTraitBodyScale). */
	FVector TraitBodyScale = FVector::OneVector;
	/** SetHatColorIndex / SetLobbyPopScale. */
	int32 HatColorIndex = -1;
	float LobbyPopScale = 1.f;
	FVector AimPoint = FVector::ZeroVector;
	FVector2D StickAim = FVector2D::ZeroVector;
	float LastStickAimTime = -100.f;

	float MeshYaw = -56.f;
	float Squash = 0.f;
	float SquashVelocity = 0.f;
	float Stretch = 0.f;
	float StretchVelocity = 0.f;
	int32 SeenJump = 0;
	int32 SeenAirJump = 0;
	int32 SeenLand = 0;
	int32 SeenDash = 0;
	int32 SeenStompLand = 0;
	int32 SeenClimb = 0;
	int32 SeenPadBounce = 0;
	int32 SeenSlide = 0;
	int32 SeenHit = 0;
	/** The counters were synced again on the first tick (replicated ones may arrive after BeginPlay). */
	bool bSeenCountersSynced = false;

	// effects timers
	float GhostTimer = 0.f;
	float StepTimer = 0.f;
	float SkidTimer = 0.f;
	bool bSkidding = false;
	float GroundCheckTimer = 0.f;
	bool bFallingToSea = false;
	/** Seconds since this penguin last touched the ground, for penguins not simulated here (animation). */
	float RemoteAirTime = 0.f;
	/** Belly slide effects: the snow spray timer, and whether this machine saw a slide going (its end: the get-up skid). */
	float SlideFxTimer = 0.f;
	bool bSlideFxActive = false;
	/** 0 -> 1: the penguin tipped onto its belly by hand, only when the slide clips are missing (see UpdateJuice). */
	float BellyTilt = 0.f;

	// ability keys of a player (SetAbilityKeyDown)
	struct FAbilityKeyState
	{
		bool bDown = false;
		/** A size ray press waiting for the release (a shot) or the hold time (on yourself). */
		bool bPending = false;
		/** The ability in the slot at the press (a pickup that swaps it in the meantime cancels the press). */
		EPenguinAbility Ability = EPenguinAbility::None;
		/** World real time of the press. */
		double PressTime = 0.0;
	};
	static constexpr int32 NumAbilityKeys = 3;
	FAbilityKeyState AbilityKeys[NumAbilityKeys];

	// belly slide hits (server)
	/** The slide (movement SlideEvents) these belong to. */
	int32 SlideContactSerial = -1;
	/** This slide ran into a penguin already: one hit (or bump) per slide. */
	bool bSlideContactDone = false;
	/** Penguins touched when the slide started (we stood inside them): they count only once they have been apart. */
	TArray<TWeakObjectPtr<APenguinCharacter>> SlideStartOverlaps;
	/** How far the slide had gone at the last contact check (the next check sweeps from there). */
	float SlideContactTravel = 0.f;

	// combat
	float KOTime = 0.f;
	/** Server: who got the last knock-out (KnockOut's By): an overgrown penguin's blast pushes others in their name. */
	TWeakObjectPtr<APenguinCharacter> KOBy;
	/** Server: the knock-out was its own doing (WasKOSelfInflicted). */
	bool bKOSelfInflicted = false;
	TWeakObjectPtr<APenguinCharacter> LastAttacker;
	float LastAttackTime = -100.f;
	/** Server: a slap stuns this penguin again only after this time (no stun-locking: the next slap only knocks). */
	double SlapStunFreeUntil = -1.0;
	/** Server: the attack whose hit the ice just took (a stomp's squash and its slam right after are one attack) does
	 *  nothing more until IceGuardUntil. */
	EPenguinAbility IceGuardSource = EPenguinAbility::None;
	TWeakObjectPtr<APenguinCharacter> IceGuardBy;
	double IceGuardUntil = -1.0;
	/** Server: yanked by the fish hook in the ice cube (BeginIceThrow), for IceThrowAge of at most IceThrowLimit seconds. */
	bool bIceThrown = false;
	float IceThrowAge = 0.f;
	float IceThrowLimit = 0.f;
	/** The velocity at the last check (a sudden stop against a block is a wall). */
	FVector IceThrowLastVelocity = FVector::ZeroVector;
	bool bDizzy = false;
	float DizzyTime = 0.f;
	bool bIceShown = false;
	bool bIceFeetShown = false;
	/** The ice chunks' size (left, right) in the penguin mesh's own units (FitIceFeet): x across the foot, y along it, z up. */
	FVector IceFootSize[2] = { FVector(65.f, 69.f, 31.f), FVector(65.f, 69.f, 31.f) };
	bool bShieldShown = false;
	float StatusTime = 0.f;
	float IceSparkleTimer = 0.f;

	// the death look (UpdateDeathLook): this machine's own, never replicated
	bool bDeathLookActive = false;
	/** The knock-out reason it plays (KOReason when it began). */
	EPenguinKOReason DeathLook = EPenguinKOReason::None;
	float DeathLookTime = 0.f;
	/** The spin so far (degrees about the camera axis) and which way it turns (+1 / -1: the way the penguin flies). */
	float DeathSpin = 0.f;
	float DeathSide = 1.f;
	float DeathFxTimer = 0.f;
	/** The movement's hit counter as the death look started: a pancake's animation sees no hit reaction after it. */
	int32 DeathHitCount = 0;
	/** Where the penguin's middle was last seen (the ghost rises from there when it vanishes). */
	FVector DeathLastLocation = FVector::ZeroVector;
	/** The sea's surface under a penguin that fell in (bubbles, the ghost coming out of the water). */
	float DeathSeaZ = 0.f;
	bool bDeathSpiritDone = false;
	/** The mesh place the death look last set (SetDeathMeshTransform), for ReapplyDeathMeshLocation. */
	FVector DeathMeshLocation = FVector::ZeroVector;
	bool bDeathMeshPlaced = false;
	bool bDeathSinkDone = false;
	bool bDeathCoughDone = false;
	bool bDeathIceShown = false;
	bool bDeathSootShown = false;
	bool bDeathStarsShown = false;
	/** A snow bomb's ash pile (APinguFX::SetAshPile, every frame while it shows): the look time it began crumbling at
	 *  (-1e6, AshNotCrumbled: not yet; a broken floor can move it below 0), the floor under its middle, the way the wind blows
	 *  it (+1 / -1), how much is blown away. */
	float DeathAshStart = -1.e6f;
	FVector DeathAshBase = FVector::ZeroVector;
	float DeathAshWind = 1.f;
	float DeathAshBlown = 0.f;
	/** Ash bits streamed off it so far (every third one is a speck). */
	int32 DeathAshBits = 0;
	/** It shows on the effects actor now (set this frame or before). */
	bool bDeathAshShown = false;
	/** Over: blown away, eyes and all (the ghost is out). */
	bool bDeathAshDone = false;
	bool bDeathAshSettled = false;
	bool bDeathGustDone = false;
	/** The pile lies on the island's ground (checked now and then while it waits for the gust: the floor broken away under
	 *  it, the gust comes at once) and the time to the next check. */
	bool bDeathAshOnIsland = false;
	float DeathAshFloorTimer = 0.f;
	/** The snow bomb's char-up: how far the soot material has been taken (0..1, 1 = solid black: left alone from then on),
	 *  the ember glints' timer, and the charred penguin's eyeballs (world middles, the one further left first) and the
	 *  radius of the white eyes over them (UpdateDeathFaceEyes; kept where they were last once the penguin hides). */
	float DeathCharDone = 0.f;
	float DeathEmberTimer = 0.f;
	FVector DeathFaceEyes[2] = { FVector::ZeroVector, FVector::ZeroVector };
	float DeathFaceEyeRadius = 0.f;
	bool bDeathFaceEyesKnown = false;
	/** The pile time the eyes came down onto the pile's top (-1: still on the sinking face). */
	float DeathEyesLandP = -1.f;
	/** The mesh's overlay material before the soot went on (put back after). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> OverlayBeforeSoot;

	// ability animation: what this machine plays (server actions arrive through RepAction*, the climb is local)
	int32 ActionCount = 0;
	EPenguinClip ActionClip = EPenguinClip::Count;
	float ActionRate = 1.f;
	/** The direction of the last aimed action (X right, Y up). */
	FVector2D ActionAim = FVector2D(1.f, 0.f);

	// the noot horn (FitNootHorn / UpdateNootHorn)
	/** Seconds since the honk started; < 0: put away. */
	float HornTime = -1.f;
	/** Where it points (X right, Y up). */
	FVector2D HornAim = FVector2D(1.f, 0.f);
	/** Its throat and turn in the Body bone, and the beak's forward / up in the mesh (reference pose). */
	FVector HornPivot = FVector::ZeroVector;
	FQuat HornRotation = FQuat::Identity;
	FVector HornForward = FVector(0.f, 1.f, 0.f);
	FVector HornUp = FVector(0.f, 0.f, 1.f);
	/** Mesh units per unit of the horn's model (it is modelled in Pingu.blend's units). */
	float HornUnit = 1.f;
	bool bHornFitted = false;
	bool bHornShown = false;
	/** The horn's outline (always hidden): a static mesh component cannot hide one section, so a material that draws
	 *  nothing (M_Ghost at opacity 0) stands in for the slot's own (the mesh's, back when the option is on again). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> HornOutlineHider;
	bool bHornOutlineHidden = false;

	// ------------------------------------------------------------------ replicated state (written on the server)
	UPROPERTY(ReplicatedUsing = OnRep_PlayerSlot)
	int32 PlayerSlot = 0;

	UPROPERTY(ReplicatedUsing = OnRep_PlayerSlot)
	FString DisplayName = TEXT("Pingu");

	/** Team match: 0..PinguMaxTeams-1; -1 everybody for themselves. */
	UPROPERTY(ReplicatedUsing = OnRep_PlayerSlot)
	int32 Team = -1;

	// Skip the owner: a remote player's own copy of these is written by the Client RPCs (in order with its simulation).
	UPROPERTY(ReplicatedUsing = OnRep_KnockOut)
	bool bKnockedOut = false;

	UPROPERTY(ReplicatedUsing = OnRep_KnockOut)
	bool bKOHidden = false;

	UPROPERTY(Replicated)
	EPenguinKOReason KOReason = EPenguinKOReason::None;

	UPROPERTY(Replicated)
	bool bVictory = false;

	/** The current size. Skips the owner: the simulating machine grows / shrinks its own penguin toward SizeTarget. */
	UPROPERTY(ReplicatedUsing = OnRep_Size)
	float SizeScale = 1.f;

	UPROPERTY(ReplicatedUsing = OnRep_Size)
	float SizeTarget = 1.f;

	/** Counted down on the server only. */
	UPROPERTY(Replicated)
	float SizeTimeLeft = 0.f;

	/** The size it rests at (SetBaseSize): a size ray wears off back to it. */
	UPROPERTY(Replicated)
	float BaseSize = 1.f;

	/** The server's ability actions (PlayAction): every change plays RepActionClip once on every client. */
	UPROPERTY(ReplicatedUsing = OnRep_Action)
	uint8 RepActionCount = 0;

	UPROPERTY(Replicated)
	EPenguinClip RepActionClip = EPenguinClip::Count;

	UPROPERTY(Replicated)
	float RepActionRate = 1.f;

	/** PlayAimedAction's direction: its angle on the play plane in 256 steps. */
	UPROPERTY(Replicated)
	uint8 RepActionAim = 0;

	/** The aim on the play plane (X, Z) for the machines that do not control the penguin (skips the owner). */
	UPROPERTY(ReplicatedUsing = OnRep_Aim)
	FVector2f RepAimPoint = FVector2f(0.f, 0.f);

	UPROPERTY(ReplicatedUsing = OnRep_Aim)
	bool bAimValid = false;

	UPROPERTY(ReplicatedUsing = OnRep_Aim)
	bool bAimFromStick = false;

	/** The snowball combo's pop (ShowComboHit): the combo hit's number and a serial every machine sees change. */
	UPROPERTY(Replicated)
	uint8 ComboPopCount = 0;

	UPROPERTY(Replicated)
	uint8 ComboPopSerial = 0;

	// ------------------------------------------------------------------ LAN bookkeeping (not replicated)
	/** Server: round resets sent to a remote owner. Until the owner acknowledges the last one, the positions it sends may
	 *  still be from before the reset (stale moves): no sea check, and the server copy is kept near the new place. */
	uint8 ResetSerial = 0;
	bool bAwaitingResetAck = false;
	float ResetAckDeadline = 0.f;
	FVector ResetLocation = FVector::ZeroVector;
	/** Owner: the reset to acknowledge once moves from the new place have gone out (-1: none). */
	int32 PendingResetAck = -1;
	int32 PendingResetAckFrames = 0;
	float PendingResetAckTime = 0.f;
	/** Owner: the aim last sent to the server. */
	FVector LastSentAim = FVector::ZeroVector;
	bool bLastSentAimFromStick = false;
	float AimSendTimer = 0.f;
};

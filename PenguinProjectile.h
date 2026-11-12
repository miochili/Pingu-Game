// Pingu - thrown things (snowballs, icicles, snow bombs, the fish hook) and the falling iceberg.
//
// LAN: only the server spawns and simulates them (flight, hits, interactions, pegs, island edits); they replicate,
// always relevant. Clients draw them: the stock movement replication brings position + velocity, every client keeps
// flying its copy with that velocity (gravity, walls and bomb bounces included) and blends corrections out, and it
// rebuilds icicle pegs from replicated numbers, because each client's own penguin stands on its local copy.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguTuning.h"
#include "PenguinProjectile.generated.h"

class ABlockIsland;
class AHookedIceBlock;
class APenguinCharacter;
class APinguPickup;
class UBoxComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

UENUM()
enum class EPenguinProjectileKind : uint8
{
	Snowball,
	Icicle,
	Bomb,
	FishHook,
	IceBolt,    // ice gun: the victim slips on everything, snow turns to ice
	ShrinkRay,  // shrinks penguins, projectiles and pegs
	GrowRay,    // grows penguins, projectiles and pegs
};

/**
 * A projectile flying on the play plane. Moved by hand every frame: a sphere sweep against the
 * island (world static) and a capsule test against the penguins, so fast icicles never tunnel.
 * Shots meet each other (your own ones too, once they are out of your flipper; bombs bounce off
 * each other) and knock ability bubbles along as they fly through them. Shots that hit an ice block break it
 * (and are used up); the ice bolt makes ice instead, the grow ray grows it.
 *
 * The fish hook grabs whatever it meets: a penguin is thrown over its thrower's head (the hook rides along on it),
 * snowballs and bombs are flung over the head too, icicles come straight back, ability bubbles are reeled in to the
 * thrower, icicle pegs and ice blocks are yanked out, any other ground pulls the thrower there.
 *
 * Gameplay runs only where HasAuthority() (server or solo); on a LAN client every gameplay function is a no-op.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APenguinProjectile : public AActor
{
	GENERATED_BODY()

public:
	APenguinProjectile();

	virtual void Tick(float DeltaSeconds) override;
	virtual FVector GetVelocity() const override { return Velocity; }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Clients: a replicated position / velocity is blended in instead of snapped to (no stepping at the net rate). */
	virtual void PostNetReceiveVelocity(const FVector& NewVelocity) override;
	virtual void PostNetReceiveLocationAndRotation() override;

	/** Server or solo only (clients get projectiles by replication): returns null on a LAN client. */
	static APenguinProjectile* Launch(UWorld* World, EPenguinProjectileKind InKind, APenguinCharacter* InShooter, const FVector& Location, const FVector& InVelocity);

	/** The fish hook flies this fast (cm/s) and its line reaches this far (cm). */
	static constexpr float HookSpeed = PinguTuning::HookSpeed;
	static constexpr float HookReach = PinguTuning::HookReach;

	/** Server: the strength of the use that threw it (its thrower's UPenguinAbilityComponent::GetUseStrength at the launch;
	 *  1 normally - Traits mode scales it). Its hits carry it (FPenguinHit::Strength); a batted shot keeps its own. */
	float GetStrength() const { return ShotStrength; }
	void SetStrength(float NewStrength) { ShotStrength = FMath::Max(NewStrength, 0.f); }
	/** Server: a snowball a snowman threw (APinguSnowman): its hit goes on with its thrower's combo but gives no snowball back. */
	void SetNoRefund(bool bInNoRefund) { bNoRefund = bInNoRefund; }
	bool IsNoRefund() const { return bNoRefund; }

	/** Server: the shot flies through Penguin for Seconds (a shot that Ice Touch froze drops past it). */
	void IgnorePenguin(APenguinCharacter* Penguin, float Seconds);
	/** Server: it flies on with this velocity (cm/s, X right, Z up) - every machine flies its copy from there. */
	void SetFlightVelocity(const FVector& NewVelocity);
	/** A bomb's blast reach on top of its size (Traits mode: the throw's strength, PinguTraitTuning::GetReachScale; 1
	 *  normally). Every machine draws the blast zone with it. */
	float GetBlastScale() const { return BlastScale; }
	/** A bomb's blast as it is now (x its size and blast scale, cm): pushes everybody up to the push radius - the thrower too -
	 *  and knocks out everybody inside the kill radius. Bots keep out of the first, tests read both. */
	float GetBlastPushRadius() const { return PinguTuning::BombPushRadius * SizeScale * BlastScale; }
	float GetBlastKillRadius() const { return PinguTuning::BombKillRadius * SizeScale * BlastScale; }

	/** Batted back (slap, gust, ice shell): new owner, new direction. NewSpeed > 0 sets the new speed (a noot: fast at
	 *  point blank, a nudge at the edge), else it flies back a little faster than it came. */
	bool Reflect(APenguinCharacter* NewOwner, const FVector& Direction, float NewSpeed = 0.f);

	// ---- everything interacts with everything (server or solo; no-ops on a LAN client)
	/** A push (gust, noot, slap, stomp): flying things are batted along Direction (NewSpeed: see Reflect), pegs break. */
	void Push(APenguinCharacter* By, const FVector& Direction, float NewSpeed = 0.f);
	/** The gust's wind: an icicle peg or ground pad is blown out of its wall (the ground) and flies off along Direction as
	 *  By's icicle - heavy, it drops, and sticking into a wall again it is a new peg (its way out of the surface is never
	 *  into it). Anything else is pushed (Push). */
	void BlowOut(APenguinCharacter* By, const FVector& Direction);
	/** Frost / ice gun: bombs stop ticking, snowballs turn into heavy ice balls, hooks let go, a flying icicle slows down
	 *  and drops (once). (An ice bolt meeting an icicle leaves it alone: ResolveCollision.) */
	void Freeze(float Seconds);
	/** Shrink / grow ray: size, reach and power scale with it (also pegs and bomb blasts). A peg never grows into a
	 *  penguin: it keeps its size then. */
	void Rescale(float Factor);
	/** Breaks it (explosions, crushing icebergs, hits on pegs). Bombs go off. */
	void Shatter();
	/** Bombs: go off after at most Delay seconds (chain reactions). */
	void Detonate(float Delay);
	/** Snow warp: the shot is put somewhere else at once and flies on from there (every machine snaps it there). */
	void WarpTo(const FVector& NewLocation);
	/** Moving, resting or about to explode - not stuck in a wall, not finished, not a hook on its way back or riding on
	 *  the penguin (or the ice block) it pulls. */
	bool IsFlying() const { return !bStuck && !bDone && !bRetracting && !bAnchored && !HookedPenguin && !HookedIce; }
	float GetSizeScale() const { return SizeScale; }
	/** Frozen by frost or an ice bolt (bombs stop ticking, snowballs are heavy ice balls, a frosted icicle drops). */
	bool IsFrozen() const { return HasAuthority() ? FrozenLeft > 0.f : bFrozenLook; }
	/** A fish hook throwing this penguin over its thrower's head (it rides on it until the throw ends). */
	APenguinCharacter* GetHookedPenguin() const { return HookedPenguin.Get(); }

	/** Every projectile and peg in the area (a cone if CosHalfAngle > -1) gets pushed - not By's own. bBlowPegsOut (the
	 *  gust): pegs and ground pads are blown out (BlowOut) instead of breaking where they are. Returns how many. */
	static int32 PushInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle,
		bool bBlowPegsOut = false);
	/** Frost breath: every flying projectile in the area (a cone if CosHalfAngle > -1) gets frozen (not By's own), every
	 *  icicle peg in it is refrozen (anybody's: it lasts its full time again), every falling iceberg it reaches (not By's
	 *  own) hangs in the air a moment (AIcebergDrop::Hold). Returns how many. */
	static int32 FreezeInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle, float Seconds);

	EPenguinProjectileKind GetKind() const { return Kind; }
	/** Whoever threw it last (null once that penguin is gone). */
	APenguinCharacter* GetShooter() const;
	float GetRadius() const { return Radius; }
	float GetGravity() const { return Gravity; }
	/** Server, right after Launch (before it first replicates): how heavy it flies (cm/s^2) - a lobbed icicle (a short-charged
	 *  throw, UPenguinAbilityComponent::GetIcicleThrow). Every machine flies it with it. */
	void SetGravity(float NewGravity) { Gravity = FMath::Max(NewGravity, 0.f); }
	/** An icicle stuck in a wall that penguins can stand on. */
	bool HasPeg() const { return Peg != nullptr; }
	/** World position of the middle of the peg's top face (only valid with HasPeg()). */
	FVector GetPegTop() const;
	/** An icicle stuck in the ground (or a ceiling, anything but a wall): walking into it throws you up like a jump pad. */
	bool IsGroundPad() const { return bGroundPad && bStuck && !bDone; }
	/** The ground pad's icicle as a line (thick end, tip) and how thick it is around that line. */
	void GetGroundPadSegment(FVector& OutA, FVector& OutB, float& OutRadius) const;

	/** Still flying and able to hurt (bots dodge these). */
	bool IsThreat() const { return !bStuck && !bDone && !bRetracting && !bResting && !bAnchored && !HookedPenguin && !HookedIce; }
	/** A tornado (APinguTornado) can suck it in: flying or lying on the ground - not stuck in a wall (a peg is blown out at the
	 *  throw instead), not a fish hook (its line stays in its thrower's flipper), not finished. Every machine. */
	bool CanBeSucked() const
	{
		return !bStuck && !bDone && !bRetracting && !bAnchored && !HookedPenguin && !HookedIce && Kind != EPenguinProjectileKind::FishHook;
	}

	/** Every live projectile in the world. */
	static void GetAll(const UWorld* World, TArray<APenguinProjectile*>& Out);

	/** Seconds this projectile or peg still has before it goes away by itself (a bomb: until it goes off, a frozen fuse
	 *  waiting first). Server or solo (a client's copy keeps no time). */
	float GetLifeLeft() const;

	/** A bomb's blast zone clock, as this machine shows it right now: how far round the outer ring the fuse has burnt (solid
	 *  from 12 o'clock clockwise, dotted after that) - 0 just lit .. 1 going off. It stands still while the fuse is frozen.
	 *  The value the ring's material gets (Progress). */
	float GetBlastZoneProgress() const { return ZoneProgress; }
	/** The blast zone is drawn (its material was found). */
	bool HasBlastZone() const { return BlastZone != nullptr; }

protected:
	virtual void BeginPlay() override;

private:
	void Setup(EPenguinProjectileKind InKind, APenguinCharacter* InShooter, const FVector& InVelocity);
	/** Mesh, material, size and spin of the kind (the server at spawn, clients when it arrives). */
	void SetupLook();
	void ApplyFrozenLook(bool bFrozen);
	/** Server: not frozen any more (back to its own look). */
	void Thaw();
	/** Server: one frame of flight and everything it meets. Effects in here reach the LAN clients. */
	void TickGameplay(float Dt);
	/** Client: keeps flying the copy with the replicated velocity and blends the server's corrections out. */
	void TickRemote(float Dt);
	/** Every machine, never broadcast: spin, glow trail, fuse sparks, trail puffs, peg blink, the hook line, a bomb's blast
	 *  zone. */
	void TickVisuals(float Dt);
	/** Bombs, every machine and effects setting: the blast zone around it - the push radius with the fuse burning round
	 *  its edge like a clock (a solid line sweeps clockwise from 12 o'clock over the dotted ring; it goes off when the ring
	 *  is all solid), the knock-out zone inside, pulsing faster as the fuse runs down (/Game/FX/M_BlastZone on the engine
	 *  plane, facing the camera, drawn over everything). A LAN client's clock runs from FuseTicks / FuseTotalTicks. */
	void UpdateBlastZone(float Dt);
	/** Client: shows what the replicated state says (size, frozen tint, peg, finished). Safe to call again. */
	void ApplyState();
	UFUNCTION()
	void OnRep_State();
	/** The sphere sweep against the island and the pegs (your own snowballs and icicles pass your own pegs). */
	bool SweepWorld(const FVector& Start, const FVector& End, FHitResult& OutHit) const;
	void HitPenguin(APenguinCharacter* Victim, const FVector& Where);
	void HitWorld(const FHitResult& Hit);
	/** Icicle stuck in a wall: a peg penguins can stand on. */
	void MakePeg(const FHitResult& Hit, float OutX);
	/** The peg box from PegWallX / PegOutX / PegCenterZ / SizeScale: the same on every machine. */
	void CreatePegBox(bool bCollide);
	void ResizePegBox();
	FVector PegCenter() const;
	FVector PegIcicleLocation() const;
	FRotator PegIcicleRotation() const;
	/** A live penguin (only ones this machine moves itself if bOnlyLocal) would be inside a peg box there. */
	bool PegWouldTrap(const FVector& Center, const FVector& Extent, bool bOnlyLocal) const;
	/** Growing the peg to NewScale would reach into a live penguin its box does not touch now. */
	bool PegGrowthWouldTrap(float NewScale) const;
	/** Server: an ice bolt or frost on an icicle peg - it lasts its full time again and stops blinking. bQuietIfFresh: no
	 *  effects if it was refrozen a moment ago (frost refreezes it every frame of a breath). */
	void RefreezePeg(bool bQuietIfFresh);
	void Explode();
	/** Two projectiles met in the air. */
	static void ResolveCollision(APenguinProjectile* A, APenguinProjectile* B);
	/** Two bombs bump into each other: they bounce apart. */
	static void BounceBombs(APenguinProjectile* A, APenguinProjectile* B);
	/** Knocks along every ability bubble this frame's path goes through (each bubble once per flight). */
	void PushPickupsOnPath(const FVector& Start, const FVector& End);
	/** The fish hook: the first ability bubble on this frame's path (and where along it, 0..1), or null. */
	APinguPickup* FindPickupOnPath(const FVector& Start, const FVector& End, float& OutT) const;

	// ---- the fish hook grabbing things (server)
	/** A hooked penguin: thrown in an arc over the thrower's head to land a few metres behind it; the hook rides along.
	 *  A frozen one goes YankIceCube's way instead. */
	void ThrowPenguinOverhead(APenguinCharacter* Victim, const FVector& Where);
	/** A hooked frozen penguin: the ice holds - the whole ice cube, heavy, comes about halfway to the thrower on a low lob,
	 *  falling like a block of the island, and shatters where it hits something (APenguinCharacter::BeginIceThrow). */
	void YankIceCube(APenguinCharacter* Victim, const FVector& Where);
	/** A shot the hook met: snowballs and bombs fly over the hook thrower's head, the rest straight back past it. */
	void FlingOverhead(APenguinCharacter* NewOwner);
	/** The hook pulls an icicle peg out of the wall: it flies at the thrower (and past, it is the thrower's now). */
	void YankPeg(APenguinProjectile* PegOwner);
	/** Hook at the hand: the reeled-in bubble goes to the thrower (or floats on if the thrower can't take it). */
	void HandOverPickup();
	/** A bomb a belly-sliding penguin runs into gets kicked along (the slider's bomb now). */
	void KickBySlide();
	/** Shots hitting an ice block (ice walls, icebergs, frozen snow) break it; the grow ray grows one next to it. True if
	 *  the shot is used up by it. */
	bool HitIce(const FHitResult& Hit);
	FLinearColor RayColor() const;
	void UpdateLine();
	void Finish();
	/** Soft glowing path behind the shot: one segment from where it was last drawn to where it is now. */
	void EmitTrail();
	FVector HandLocation() const;

	UPROPERTY(VisibleAnywhere, Category = "Projectile")
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** Fishing line (a thin stretched cylinder). */
	UPROPERTY(VisibleAnywhere, Category = "Projectile")
	TObjectPtr<UStaticMeshComponent> Line;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MeshMaterial;

	/** A bomb's blast zone (made on its first visual tick on every machine; hidden with the bomb). */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BlastZone;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BlastZoneMaterial;

	/** Collision of an icicle peg (only created when an icicle sticks into a wall; every machine builds its own). */
	UPROPERTY(Transient)
	TObjectPtr<UBoxComponent> Peg;

	// ---- replicated: the server writes these, clients draw from them
	UPROPERTY(Replicated)
	EPenguinProjectileKind Kind = EPenguinProjectileKind::Snowball;

	/** Whoever threw it last (a batted or hooked shot changes hands). */
	UPROPERTY(Replicated)
	TObjectPtr<APenguinCharacter> Shooter;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	float SizeScale = 1.f;

	/** Clients fly their copy with it (a frozen snowball gets heavy). */
	UPROPERTY(Replicated)
	float Gravity = 0.f;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool bStuck = false;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool bDone = false;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool bRetracting = false;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool bAnchored = false;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool bResting = false;

	/** Wears the frozen tint. */
	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool bFrozenLook = false;

	/** Sticks out of a wall as a peg (built from the Peg* numbers below, which arrive with it). */
	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool bPegged = false;

	/** The peg (or ground pad) is about to break (it blinks). */
	UPROPERTY(Replicated)
	bool bPegExpiring = false;

	/** Stuck in the ground / a ceiling, not a wall: a jump pad you walk into (IsGroundPad). */
	UPROPERTY(Replicated)
	bool bGroundPad = false;

	/** Which way the peg sticks out of the wall (+1 right, -1 left). */
	UPROPERTY(Replicated)
	float PegOutX = 1.f;

	/** X of the wall face the peg sticks out of. */
	UPROPERTY(Replicated)
	double PegWallX = 0.0;

	/** Height of the middle of the peg box (it never moves; resizing keeps it). */
	UPROPERTY(Replicated)
	double PegCenterZ = 0.0;

	/** The fish hook rides on this penguin while it throws it over the thrower's head (every machine draws it there). */
	UPROPERTY(ReplicatedUsing = OnRep_State)
	TObjectPtr<APenguinCharacter> HookedPenguin;

	/** The fish hook rides on the ice block it pulled out of the island until that lands (every machine draws it there). */
	UPROPERTY(ReplicatedUsing = OnRep_State)
	TObjectPtr<AHookedIceBlock> HookedIce;

	/** Bumped when the server puts the shot somewhere else at once (snow warp): clients snap there instead of blending. */
	UPROPERTY(Replicated)
	uint8 TeleportCount = 0;

	/** A bomb's fuse left in 1/40 s (rounded up): every machine's blast zone burns from it. */
	UPROPERTY(Replicated)
	uint8 FuseTicks = 0;

	/** The blast zone clock's full length in 1/40 s (FuseTotal): every machine's ring is full when FuseTicks reaches 0. */
	UPROPERTY(Replicated)
	uint8 FuseTotalTicks = 0;

	/** GetBlastScale (set at the launch, before it first replicates). */
	UPROPERTY(Replicated)
	float BlastScale = 1.f;

	// ---- server gameplay
	/** Ability bubbles this shot already knocked along (so a bubble is not pushed again every frame it overlaps). */
	TArray<TWeakObjectPtr<APinguPickup>> PushedPickups;
	/** The fish hook: the ability bubble it reels in to its thrower. */
	TWeakObjectPtr<APinguPickup> CaughtPickup;
	/** The fish hook: seconds left of the throw it rides along on, and how long it has been riding. */
	float HaulLeft = 0.f;
	float HaulAge = 0.f;
	/** A bomb kicked by a belly slide is not kicked again at once. */
	float KickCooldown = 0.f;
	FVector Velocity = FVector::ZeroVector;
	/** GetStrength. */
	float ShotStrength = 1.f;
	/** IgnorePenguin: it flies through this penguin while IgnoreLeft lasts. */
	TWeakObjectPtr<APenguinCharacter> IgnoredPenguin;
	/** A snowman's snowball: no refund for its hit (SetNoRefund). */
	bool bNoRefund = false;
	float IgnoreLeft = 0.f;
	float Radius = 18.f;
	float Age = 0.f;
	float Life = 2.f;
	float FuseLeft = 0.f;
	/** The longest fuse this bomb had: the blast zone's clock goes once round in it. A bomb batted back onto a short fuse
	 *  goes back round to match the time it has left; a hooked bomb's longer fuse starts the ring again from there. */
	float FuseTotal = 0.f;
	float StuckLeft = 0.f;
	float AnchorLeft = 0.f;
	FVector Anchor = FVector::ZeroVector;
	float FrozenLeft = 0.f;
	/** An icicle frost has slowed already (it slows only once). */
	bool bFrostSlowed = false;

	// ---- visuals (every machine)
	float TrailTimer = 0.f;
	/** Lower effect settings draw the glow trail in fewer, longer segments. */
	float GlowWait = 0.f;
	FVector LastTrailPoint = FVector::ZeroVector;
	bool bTrailStarted = false;
	bool bFinalTrailDone = false;
	float Spin = 0.f;
	/** Flew freely this frame (spin, trail puffs). */
	bool bMovedThisFrame = false;
	/** The peg's blink is showing (it stops when an ice bolt refreezes the peg). */
	bool bPegBlinkShown = false;
	/** The blast zone: its clock (0 lit .. 1 going off, GetBlastZoneProgress), how long it has shown, its pulse phase. */
	float ZoneProgress = 0.f;
	/** A LAN client's fuse for the clock: the replicated FuseTicks, counted down between updates (never below the value
	 *  the server sent, and not while frozen) so the ring turns smoothly. Below 0: none yet. */
	float ClientFuse = -1.f;
	float ZoneAge = 0.f;
	float ZonePhase = 0.f;
	/** Its material or mesh is missing: no blast zone (not looked for again). */
	bool bNoBlastZone = false;

	// ---- client: smoothing and what is already shown
	/** Where the copy flies (the replicated position, carried on with the velocity). */
	FVector NetLocation = FVector::ZeroVector;
	/** Drawn offset from NetLocation that fades out (keeps corrections from the server smooth). */
	FVector NetError = FVector::ZeroVector;
	/** Ran into a wall here: waits for the server to say what became of it. */
	bool bNetBlocked = false;
	/** The last update kept our heading and speed (its correction along the path is timing: it fades slowly). */
	bool bNetSteady = false;
	float ShownSizeScale = 1.f;
	bool bShownFrozen = false;
	bool bShownDone = false;
	uint8 ShownTeleportCount = 0;
	/** The peg turns solid once our own penguin is out of its way. */
	bool bPegCollisionPending = false;
};

/**
 * Iceberg Drop: a 2x2 block of ice that appears high above the target, blinks a warning, then
 * falls, fast. A penguin under it (its middle within its columns) is pushed down with it and crushed once it is pressed
 * against the ground (over the sea it is pushed down into the sea); one only touching its side is shoved out of the way,
 * not hurt. It becomes lasting ice blocks of the island: on ground they behave like sand (each column piles up on
 * whatever is under it; landing on one column, the ice of the other falls on down and crushes only a penguin it really
 * comes down on), over the sea they stay put as a floe. Like every ice block they break block by block (shots,
 * pushes, the hook ...). Frost breath holds it in the air for a moment (frosty, every machine); wind (gust, noot) blows
 * it away sideways - it lands wherever it comes down then, or splashes into the sea.
 * It has no collision itself: the blocks it becomes are island blocks (those reach every client by themselves).
 * A grown dropper's iceberg is bigger (PinguTuning::GetIceScale): from 2x size 3x3 blocks, from 4x 2x2 big blocks (each as
 * big as 2x2 cells of the island, on its lattice: 4x4 cells); it falls, carries, crushes and lands the same, as big ice.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API AIcebergDrop : public AActor
{
	GENERATED_BODY()

public:
	AIcebergDrop();

	virtual void Tick(float DeltaSeconds) override;
	/** Sideways too once wind blew it: the stock movement replication sends it to the clients. */
	virtual FVector GetVelocity() const override { return FVector(VelocityX, 0.f, VelocityZ); }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Clients: the fall (and a sideways flight) is simulated locally and the replicated position only corrects it
	 *  (smoothly). */
	virtual void PostNetReceiveVelocity(const FVector& NewVelocity) override;
	virtual void PostNetReceiveLocationAndRotation() override;

	/** Over solid ground it falls onto the ground; over the sea it stops as a floe with its top near TargetZ. As big as its
	 *  owner's size makes it (MakerSize > 0: as if the owner were that big). Server or solo only: returns null on a LAN client. */
	static AIcebergDrop* Drop(UWorld* World, APenguinCharacter* InOwner, float TargetX, float TargetZ, float MakerSize = 0.f);
	/** Where Drop would put its ice (its columns' outer edges, world X, and its top when it has landed or stopped, world Z) inside
	 *  the build limit (PinguBuildLimit; 2026-11-05). True without a limit or an island. OutLandedAt: the middle of its top. */
	static bool IsDropAllowed(const UWorld* World, const APenguinCharacter* InOwner, float TargetX, float TargetZ, float MakerSize = 0.f, FVector* OutLandedAt = nullptr);

	/** Width and height in cells (2 normally; PinguTuning::GetIceScale), and how many cells per side each of its blocks covers
	 *  (1, or 2: big blocks). Every machine. */
	int32 GetSizeCells() const { return SizeCells; }
	int32 GetBlockSpan() const { return BlockSpan; }
	float GetWidth() const { return SizeCells * CellSize; }

	/** The strip of the island it will land on (bots run away from it). */
	float GetLeftX() const { return GetActorLocation().X; }
	float GetRightX() const { return GetActorLocation().X + GetWidth(); }
	bool IsFalling() const { return WarnLeft <= 0.f; }
	/** Whoever dropped it (it never crushes its owner). Server only. */
	APenguinCharacter* GetDropOwner() const { return DropOwner.Get(); }
	/** Its box touches a circle on the play plane (explosions). */
	bool Overlaps(const FVector& Center, float InRadius) const;
	/** Blown up in mid-air (server or solo). */
	void Shatter();
	/** Frost Breath: a falling iceberg hangs in the air for Seconds (at least), then falls on from rest. Server or solo.
	 *  Does nothing during the warning. */
	void Hold(float Seconds);
	/** Hanging in the air right now (Hold). Every machine. */
	bool IsHeld() const;
	/** Its box touches the area on the play plane (a cone of it if CosHalfAngle > -1): frost breath reaching it. */
	bool IsInArea(const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle) const;
	/** Wind: a falling iceberg flies off sideways (DirectionX: its sign is the way) and a little up, Strength times the
	 *  usual speed, and falls on from there; a frost hold ends. Server or solo. Does nothing during the warning. */
	void Blow(float DirectionX, float Strength);
	/** Gust and noot: every falling iceberg the wind reaches (its box in the area, a cone of it if CosHalfAngle > -1; not
	 *  By's own, like its own shots) is blown away - along the cone, or away from the area's middle. Returns how many. */
	static int32 BlowInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, const FVector& ConeDirection, float CosHalfAngle, float Strength);

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void OnRep_Held();
	/** The ice's look: clear, or frosty while held (every machine, from bHeld). */
	void ApplyIceLook();
	/** Server: it stopped (bOnIsland: on ground or as a floe; else in the sea). On ground its ice becomes the island's sand
	 *  ice: a column with a lower floor falls on down from where it stopped and crushes only a penguin it really comes
	 *  down on (ABlockIsland::AddSandBlocks with bIcebergIce), never one that got out from under it in time. */
	void Land(bool bOnIsland);
	/** The ice blocks and the warning ring (the server after Drop, clients when it arrives). */
	void BuildLook();
	/** The highest top of a block under its columns (with its left side at LeftX) that its bottom passed going from
	 *  OldBottom down to NewBottom (a block built there since the drop), or the lowest float if none. */
	float BlockTopBetween(double LeftX, float OldBottom, float NewBottom) const;
	/** The highest ground top under every column its box (left side at LeftX) reaches into, searched from FromBottom down
	 *  MaxDrop, or the lowest float if none. */
	float GroundTopUnder(const ABlockIsland* Island, double LeftX, float FromBottom, float MaxDrop) const;
	/** Its box (left side at LeftX, bottom at Bottom, a hair inside its edges) is inside a block of the island. */
	bool BoxHitsBlocks(const ABlockIsland* Island, double LeftX, float Bottom) const;
	/** Blown sideways (every machine, on its own island): InOutX goes on with VelocityX; a block in the way stops it
	 *  flush with the column grid (VelocityX 0). */
	void StepSideways(const ABlockIsland* Island, float Dt, double& InOutX, float Bottom);
	/** Where it lands from LeftX / Bottom on (it moved sideways): the ground under its columns; with none, a floe drop
	 *  keeps its floe height and any other one splashes into the sea. Every machine. */
	void UpdateLanding(const ABlockIsland* Island, double LeftX, float Bottom);
	/** The warning ring under its middle at the landing height (it follows a blown iceberg). Every machine. */
	void UpdateMarker();
	/** Server, every frame it falls (bottom OldBottom -> NewBottom, MovedX sideways): a penguin under it is pushed down
	 *  with it and crushed once pressed against the ground; one at its side is shoved out of the way. */
	void PressPenguins(const ABlockIsland* Island, float OldBottom, float NewBottom, float MovedX);
	/** Client: the same warning and fall as on the server, corrected by the replicated position. */
	void TickRemote(float Dt);

	UPROPERTY(VisibleAnywhere, Category = "Iceberg")
	TObjectPtr<UInstancedStaticMeshComponent> Blocks;

	UPROPERTY(VisibleAnywhere, Category = "Iceberg")
	TObjectPtr<UStaticMeshComponent> Marker;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> IceMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MarkerMaterial;

	TWeakObjectPtr<APenguinCharacter> DropOwner;
	int32 LeftColumn = 0;

	UPROPERTY(Replicated)
	float CellSize = 100.f;

	/** Width and height in cells, and cells per side of each block (1, or 2 for big blocks). Set before its first
	 *  replication, never changed: every client draws the same shape. */
	UPROPERTY(Replicated)
	int32 SizeCells = 2;

	UPROPERTY(Replicated)
	int32 BlockSpan = 1;

	/** Where its bottom stops (ground top, or the floe height). Every machine follows it on by itself (UpdateLanding,
	 *  BlockTopBetween) from the value it arrived with. */
	UPROPERTY(Replicated)
	float LandTopZ = 0.f;

	/** Dropped over the sea: the height its bottom stops at as a floe (-1e9, far below everything, for one dropped over
	 *  ground). */
	UPROPERTY(Replicated)
	float FloeTopZ = -1.e9f;

	/** Frost holds it in the air (Hold): every machine shows it frosty and hanging (clients don't carry the fall on). */
	UPROPERTY(ReplicatedUsing = OnRep_Held)
	bool bHeld = false;

	bool bHasGround = false;
	/** No ground below: stops in the air as a floating floe. */
	bool bFloe = false;
	float WarnLeft = 0.45f;
	float VelocityZ = 0.f;
	/** Blown by wind (cm/s, + right); 0 until then. */
	float VelocityX = 0.f;
	float Age = 0.f;
	/** Server: seconds it still hangs in the air (Hold). */
	float HoldLeft = 0.f;
	/** Every machine: the next frosty sparkle while it hangs. */
	float HeldSparkleTimer = 0.f;
	/** Every machine: the left side its landing was last looked up for (UpdateLanding again once it moved). */
	double LandingX = 0.0;
	/** Every machine: where the warning ring is now. */
	FVector ShownMarkerLocation = FVector::ZeroVector;
	/** Server: penguins it could not knock out (the ice took the hit ...): it passes through them from then on. */
	TArray<TWeakObjectPtr<APenguinCharacter>> Spared;

	// ---- client smoothing
	/** Bottom height of the locally simulated fall. */
	double NetZ = 0.0;
	/** Drawn offset from NetZ that fades out. */
	float NetErrorZ = 0.f;
	/** Left side of the locally simulated (sideways) flight, and the drawn offset from it that fades out. */
	double NetX = 0.0;
	float NetErrorX = 0.f;
	/** The last update kept our fall speed (its correction is timing: it fades slowly). */
	bool bNetSteady = false;
};

/**
 * An ice block the fish hook pulled out of the island: heavy, it comes on a low lob about halfway to the thrower (a hooked
 * penguin flies over the thrower's head), falling like a block of the island (ABlockIsland::FallingBlockGravity, the same
 * on the way up and down), and is a block of the island again where it lands - sand ice, piling up on whatever is there
 * (whoever stands where it settles is moved out of it). A wall in its way stops it (it drops from there); a penguin in its
 * way is knocked aside (never its thrower). Spawned by the server only; every LAN client flies its own copy against its
 * own island, corrected by the server's position.
 *
 * The gust's wind blows ice blocks out of the island the same way (Blow, BlowInArea): each flies off away from the wind's
 * middle and lands the same (a block again, or a splash in the sea); its "thrower" is the blower.
 */
UCLASS(NotPlaceable)
class PENGUIN_API AHookedIceBlock : public AActor
{
	GENERATED_BODY()

public:
	AHookedIceBlock();

	virtual void Tick(float DeltaSeconds) override;
	virtual FVector GetVelocity() const override { return FVector(Velocity2D.X, 0.f, Velocity2D.Y); }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Clients: the flight is simulated locally and the replicated position only corrects it (smoothly). */
	virtual void PostNetReceiveVelocity(const FVector& NewVelocity) override;
	virtual void PostNetReceiveLocationAndRotation() override;

	/** Server or solo: takes the ice block at InsidePoint out of the island (not broken) and sends it toward Thrower,
	 *  about halfway. Null if there is no ice there or no thrower, and on a LAN client. */
	static AHookedIceBlock* Pull(ABlockIsland* Island, const FVector& InsidePoint, APenguinCharacter* Thrower);
	/** Server or solo: wind takes the ice block at InsidePoint out of the island (not broken) and it flies off with
	 *  InVelocity (cm/s, X right, Y up), falling like a block of the island. Blower is never hit by it and gets the
	 *  knock-outs of what it does. Null if there is no ice there, and on a LAN client. */
	static AHookedIceBlock* Blow(ABlockIsland* Island, const FVector& InsidePoint, APenguinCharacter* Blower, const FVector2D& InVelocity);
	/** The gust: blows away the ice blocks around Origin (any ice: walls, icebergs, sand ice, frozen snow) - each one whose
	 *  middle is within AreaRadius, whose face toward Origin is open and with no block between (one layer per gust), never
	 *  the floor right under By's feet; nearest first, at most MaxBlocks. They fly off away from Origin (harder near it).
	 *  Server or solo (0 on a LAN client). Returns how many. */
	static int32 BlowInArea(UWorld* World, APenguinCharacter* By, const FVector& Origin, float AreaRadius, int32 MaxBlocks);

	/** The block's middle (where the actor is). */
	FVector GetCenter() const { return GetActorLocation(); }
	/** How long its lob is meant to fly (a wall or a higher block may end it sooner). */
	float GetFlightTime() const { return FlightTime; }
	/** Server: it landed (a block of the island again, or lost in the sea) - it goes in a moment. */
	bool HasLanded() const { return bLanded; }
	APenguinCharacter* GetThrower() const { return Thrower.Get(); }
	/** Server: how many penguins it knocked on the way. */
	int32 GetPenguinsHit() const { return HitAlready.Num(); }
	/** Blown out of the island by wind (Blow, a tornado's RipOut), not pulled by a hook. Every machine. */
	bool IsBlown() const { return bBlown; }

	// ---- the tornado (APinguTornado)
	/** Server or solo: a tornado rips the ice block at InsidePoint out of the island (not broken) into its wind: the block
	 *  hangs in it - no gravity, it hits nobody - and goes where Swirl puts it until ThrowOut. Ripper is the tornado's user.
	 *  OutCell: the cell it came out of. Null if there is no ice there, and on a LAN client. */
	static AHookedIceBlock* RipOut(ABlockIsland* Island, const FVector& InsidePoint, APenguinCharacter* Ripper, FIntPoint* OutCell = nullptr);
	/** Server, every frame of the swirl: it is at NewCenter (X, Z) moving at NewVelocity (cm/s); every machine draws it there. */
	void Swirl(const FVector2D& NewCenter, const FVector2D& NewVelocity);
	/** Server: thrown out of the tornado with InVelocity (cm/s, X right, Y up). Heavy, it flies on and comes down as falling
	 *  ice: a penguin it comes down on is pushed down with it and crushed once that presses it against what is below (the
	 *  ripper's knock-out, reason Crushed: ABlockIsland::PressUnderFallingIce); one it meets from the side is knocked aside -
	 *  never one in Spare (the tornado has thrown those itself). Where it stops it is the island's ice again. */
	void ThrowOut(const FVector2D& InVelocity, const TArray<TWeakObjectPtr<APenguinCharacter>>& Spare);
	/** In a tornado's wind (RipOut until ThrowOut). Every machine. */
	bool IsSwirling() const { return bSwirling; }
	/** A tornado ripped it out (it comes down as crushing falling ice). Every machine. */
	bool IsFromTornado() const { return bTornado; }
	/** Server: penguins it crushed on its way down. */
	int32 GetPenguinsCrushed() const { return PenguinsCrushed; }

protected:
	virtual void BeginPlay() override;

private:
	/** The block's look: the island's ability-ice mesh and material, DepthRows deep like the island's blocks. */
	void BuildLook();
	/** Flies Dt on against the island's blocks (every machine): sideways into a block it stops sideways on the middle of
	 *  the column before it (a wall: it drops from there), up into one it stops rising, down onto one it lands - true. */
	bool Step(float Dt);
	/** The block's box (Center, a hair smaller than a cell) touches a block of the island. */
	bool TouchesBlocks(const FVector2D& Center) const;
	/** Server: knocks aside the penguins its box meets (each once; never its thrower). */
	void HitPenguins();
	/** Server: a block of the island again where it is (sand ice), or (bInSea) a splash. */
	void Land(bool bInSea);

	UPROPERTY(VisibleAnywhere, Category = "Ice")
	TObjectPtr<UInstancedStaticMeshComponent> Blocks;

	/** Set before its first replication, never changed. */
	UPROPERTY(Replicated)
	float CellSize = 100.f;

	/** Blown out by wind (Blow), not pulled by a hook. Set before its first replication, never changed. */
	UPROPERTY(Replicated)
	bool bBlown = false;

	/** Ripped out by a tornado (RipOut). Set before its first replication, never changed. */
	UPROPERTY(Replicated)
	bool bTornado = false;

	/** In the tornado's wind: it goes where the server puts it (no gravity, no hits) until it is thrown out. */
	UPROPERTY(Replicated)
	bool bSwirling = false;

	/** Server: seconds in the wind (a block whose tornado is gone falls after a while), penguins crushed. */
	float SwirlAge = 0.f;
	int32 PenguinsCrushed = 0;

	TWeakObjectPtr<ABlockIsland> Island;
	TWeakObjectPtr<APenguinCharacter> Thrower;
	TArray<TWeakObjectPtr<APenguinCharacter>> HitAlready;
	/** X right, Y up (cm/s). */
	FVector2D Velocity2D = FVector2D::ZeroVector;
	/** The middle of the block as this machine flies it (X, Z); a client draws it NetError further on (fading). */
	FVector2D Position = FVector2D::ZeroVector;
	FVector2D NetError = FVector2D::ZeroVector;
	float FlightTime = 0.f;
	float Age = 0.f;
	/** Server: landed, goes after LingerLeft (so the island's new block reaches the clients before this one goes). */
	bool bLanded = false;
	float LingerLeft = 0.f;
	/** Client: down on a block, waiting there for the server's landing. */
	bool bResting = false;
	/** Client: the cell it came out of (our island may still have that block for a moment: not one to hit). */
	FIntPoint FromCell = FIntPoint(0, 0);
	bool bIgnoreFromCell = false;
};

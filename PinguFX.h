// Pingu - particles, afterimages, screen shake, hit-stop, sounds and music.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PinguFX.generated.h"

class APinguFXRelay;
class UAudioComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UNiagaraComponent;
class UNiagaraSystem;
class UPoseableMeshComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class USoundBase;
class UStaticMesh;
class UStaticMeshComponent;
class UWorld;

/** Meshes the particle system can draw (each is one instanced mesh, 100 cm across). */
UENUM()
enum class EPinguFXMesh : uint8
{
	Puff,
	Shard,
	Star,
	Ring,
	Flake,
	Glow,     // soft translucent segment (projectile glow trails): engine cylinder, M_Trail
	Count UMETA(Hidden)
};

/** Every sound (asset /Game/Audio/S_<file name>). Keep in sync with PinguFX.cpp. */
UENUM()
enum class EPinguSound : uint8
{
	UIHover, UIClick, UIBack, UIStart,
	CountBeep, CountGo,
	Jump, DoubleJump, Land, LandHard, Dash, SlideStart, Skid, IceSlip,
	Throw, SnowballHit, IcicleThrow, IcicleHit, StompWhoosh, StompImpact, IcebergSummon, IcebergCrash,
	Slap, Frost, Freeze, Shatter, WallBuild, Warp, HookThrow, HookHit, ShieldOn, ShieldBlock, Gust,
	BombThrow, BombFuse, Explosion, Hit, KO, Splash, Squawk, Melt,
	RoundWin, MatchWin, MatchLose,
	Noot, IceGun, Slippery, Shrink, Grow,
	Crumble,
	Pickup, Drop,
	TornadoMerge,
	GiantRollUp, GiantRoll, GiantGulp, GiantBounce, GiantBurst,
	SnowmanBuild, SnowmanThrow, SnowmanCrumble,
	Flap,
	Count UMETA(Hidden)
};

/** How a particle's size changes over its life. */
UENUM()
enum class EPinguSizeCurve : uint8
{
	Linear,   // Size -> EndSize
	Pop,      // grows fast to Size, then shrinks to EndSize (cartoon puff)
	Grow,     // 0 -> Size quickly, then holds
};

/** One particle (APinguFX::Spawn). A USTRUCT so the LAN host can send it to the clients. */
USTRUCT()
struct FPinguParticleDesc
{
	GENERATED_BODY()

	UPROPERTY()
	EPinguFXMesh Mesh = EPinguFXMesh::Puff;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY()
	FLinearColor Color = FLinearColor::White;

	UPROPERTY()
	float Emissive = 0.f;

	/** cm */
	UPROPERTY()
	float Size = 30.f;

	/** cm */
	UPROPERTY()
	float EndSize = 0.f;

	UPROPERTY()
	EPinguSizeCurve Curve = EPinguSizeCurve::Pop;

	/** s */
	UPROPERTY()
	float Life = 0.5f;

	/** cm/s^2, positive pulls down */
	UPROPERTY()
	float Gravity = 0.f;

	/** 1/s */
	UPROPERTY()
	float Drag = 0.f;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	/** deg/s (pitch, yaw, roll) */
	UPROPERTY()
	FVector Spin = FVector::ZeroVector;

	UPROPERTY()
	float Opacity = 1.f;

	UPROPERTY()
	bool bFadeOut = false;

	/** Sideways wobble (snowflakes), cm/s. */
	UPROPERTY()
	float Sway = 0.f;

	/** > 0: a stretched segment this long (cm). Scale is (Size, Size, Length) / 100, so Size is the width and
	 *  Rotation must turn the mesh's +Z axis along the segment. The size curve only changes the width. */
	UPROPERTY()
	float Length = 0.f;
};

/** Which APinguFX preset a broadcast effect is. */
UENUM()
enum class EPinguFXEventKind : uint8
{
	Puffs,
	GroundPuffs,
	Trail,
	Shards,
	Stars,
	Sparkles,
	Ring,
	Flakes,
	Splash,
	Explosion,
	Ghost,
	GlowTrail,
	Shake,
	Sound,
	Spirit,
	IceBurst,
	FizzleDust,
};

/** What an ability that fizzled looks like for the first moment, before it crumbles to dust (APinguFX::FizzleDust). */
enum class EPinguDustShape : uint8
{
	Ball,   // a snowball-sized ball (snowball, bomb, hook)
	Spike,  // an icicle-shaped flash along the aim (icicle, ice gun, the rays)
	Ring,   // a ring (shell, gust, warp)
	Cone,   // a breath along the aim (frost, noot, slap)
	Block,  // a lump of ice (wall, iceberg, stomp)
};

/** One APinguFX preset call, sent by the LAN host to its clients (see FPinguFXBroadcastScope). */
USTRUCT()
struct FPinguFXEvent
{
	GENERATED_BODY()

	UPROPERTY()
	EPinguFXEventKind Kind = EPinguFXEventKind::Puffs;

	/** Particle count (each client scales it by its own effects density), the EPinguSound, or 1 = a spirit in its rest pose. */
	UPROPERTY()
	uint8 Count = 0;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** Direction (puffs), velocity (trail), the end point (glow trail) or (yaw, scale, rise speed) of a spirit. */
	UPROPERTY()
	FVector Direction = FVector::ZeroVector;

	UPROPERTY()
	FLinearColor Color = FLinearColor::White;

	/** Size, strength, speed, radius, width, life, trauma or volume (see APinguFX::PlayEvent). */
	UPROPERTY()
	float A = 0.f;

	/** Speed, duration, opacity or pitch variance. */
	UPROPERTY()
	float B = 0.f;

	/** Spread, life or pitch. */
	UPROPERTY()
	float C = 0.f;

	/** Afterimages and spirits: the penguin's mesh (a replicated penguin, so the clients find it). */
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> Mesh = nullptr;
};

/**
 * One moment of a snow bomb knock-out's ash pile (APenguinCharacter's death look sets it every frame: APinguFX::SetAshPile):
 * a lumpy heap of black ash and charcoal on the floor with two big white cartoon eyes on top, a few embers glowing on it,
 * the wind blowing it away from one side. Before the charred penguin has crumbled the eyes sit on its face (FaceEyes: the
 * white eyes laid over the penguin's own, which the soot blackens) and drop onto the pile as it crumbles.
 * Sizes are a normal penguin's pile (x Size). This machine only, never sent.
 */
struct FPinguAshPile
{
	/** The floor under the pile's middle (world; it stands on the play plane, its eyes toward the camera). */
	FVector Base = FVector::ZeroVector;
	/** x a normal penguin's pile (the penguin's size scale). */
	float Size = 1.f;
	/** 0..1: how far it has piled up (the penguin crumbling into it): the spilled bits and the foot first, the top last. */
	float Grown = 1.f;
	/** 0..1: how much of it the wind has blown away (the upwind side and the top go first). */
	float Blown = 0.f;
	/** +1 / -1: the way the wind blows (along X). */
	float Wind = 1.f;
	/** The eyes' size: 0 none, 1 normal (a bit more: wide open). */
	float Eyes = 0.f;
	/** 1 open .. 0 shut (a blink). */
	float EyeOpen = 1.f;
	/** -1 .. 1: the pupils look left .. right. */
	float Look = 0.f;
	/** The eyes moved this far from their place on the pile (cm, world: a hop on landing, blown off at the end). */
	FVector EyeShift = FVector::ZeroVector;
	/** The charred penguin's eyeballs (world middles, the one further left first) and the white eyes' radius over them (cm,
	 *  world). Used while EyeOnPile < 1 and FaceEyeRadius > 0. */
	FVector FaceEyes[2] = { FVector::ZeroVector, FVector::ZeroVector };
	float FaceEyeRadius = 0.f;
	/** 0 the eyes on the penguin's face .. 1 in their place on the pile (they fall from one to the other). */
	float EyeOnPile = 1.f;
	/** 0..1: the embers glowing on the fresh pile (they die down; 0: none). */
	float Embers = 0.f;
};

/** The host's effects of one moment, in one unreliable multicast. */
USTRUCT()
struct FPinguFXBatch
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FPinguFXEvent> Events;

	/** APinguFX::Spawn calls. */
	UPROPERTY()
	TArray<FPinguParticleDesc> Particles;
};

/**
 * CONTRACT (v2, LAN): an APinguFX call plays only on the machine that makes it. Server gameplay code (abilities,
 * projectiles, pickups, the island, hits, knock-outs, round events) wraps its effects in a FPinguFXBroadcastScope so
 * they also play on every LAN client. Effects every machine triggers by itself (movement events from replicated
 * counters, UI sounds, local camera feel) stay without a scope.
 *
 * Inside the scope on the LAN host every preset call (and Spawn) plays here and is sent once to the clients
 * (APinguFXRelay); the clients play it and never send it on. Music, volumes, hit-stop and slow motion are never
 * sent (hit-stop and slow motion do nothing at all in a LAN game). Solo: the scope does nothing.
 * Game thread only; the nesting is counted per world (PIE runs the host and its clients in one process).
 */
struct PENGUIN_API FPinguFXBroadcastScope
{
	/** Inside this scope (server, LAN game) every APinguFX call is also sent to all clients. Nests. */
	explicit FPinguFXBroadcastScope(const UObject* WorldContext);
	~FPinguFXBroadcastScope();

	FPinguFXBroadcastScope(const FPinguFXBroadcastScope&) = delete;
	FPinguFXBroadcastScope& operator=(const FPinguFXBroadcastScope&) = delete;

	/** True while a scope is open for that world (only ever on a LAN host). */
	static bool IsBroadcasting(const UWorld* InWorld);

private:
	bool bActive = false;
	const UWorld* World = nullptr;
};

/**
 * One per world, spawned on demand. Particles are pooled instances of a few little meshes
 * (snow puffs, ice shards, stars, rings, snowflakes) simulated on the CPU: cheap, crisp and
 * fully under our control. Also owns the dash afterimages, the screen shake / hit-stop / slow
 * motion and the sounds + music.
 *
 * Every pool is one instanced mesh whose live particles use its first instances; once per frame each pool gets one
 * batched transform update and only the changed instance data goes to the GPU (no render state rebuild).
 * Particle counts scale with UPinguSettings::GetFXDensity() (Low ~0.3).
 *
 * Effects quality Ultra (UPinguSettings::UseUltraEffects) adds Niagara layers on top (/Game/FX/Niagara, built by
 * Tools/ue_niagara_fx.py from Tools/niagara_recipe.json): the snow bomb's blast (light flash, shockwave, sparks, powder,
 * ice chunks, glints, mist), snow and ice bursts, twinkling glints, shockwave rings and a heavier snowfall around the
 * camera (instead of the instanced flakes). Each machine decides for itself (a LAN client plays the host's effects with
 * its own setting). A missing system just leaves its layer out.
 * Every explosion also flashes its blast zone (the real push radius, /Game/FX/M_BlastZone) on every setting.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguFX : public AActor
{
	GENERATED_BODY()

public:
	APinguFX();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	static APinguFX* Get(const UObject* WorldContext);

	void Spawn(const FPinguParticleDesc& Desc);

	// ---- presets
	static void Puffs(const UObject* Ctx, const FVector& Location, int32 Count, float Size, float Speed,
		const FVector& Direction = FVector::UpVector, float Spread = 1.f, const FLinearColor& Color = FLinearColor::White);
	/** Landing / jumping: puffs rolling out to both sides along the ground. */
	static void GroundPuffs(const UObject* Ctx, const FVector& FeetLocation, float Strength);
	/**
	 * One Flap wing beat's wind (a local cosmetic, never broadcast: every machine calls it from the replicated flap state):
	 * streaks and puffs pushed down and out from the flippers at BodyLocation (SizeScale = the penguin's size), and when
	 * the ground is within GroundDistance cm below the feet a ring of snow dust blown out along it (GroundDistance < 0: none).
	 * Beat = the beat's number (lower Effects settings only show every 2nd / 4th beat; Off none).
	 */
	static void FlapWind(const UObject* Ctx, const FVector& BodyLocation, float SizeScale, float GroundDistance, const FVector& GroundPoint, int32 Beat);
	/** One soft lingering puff (dash / slide / run trails). */
	static void Trail(const UObject* Ctx, const FVector& Location, float Size, const FLinearColor& Color = FLinearColor::White, const FVector& Velocity = FVector::ZeroVector);
	static void Shards(const UObject* Ctx, const FVector& Location, int32 Count, float Speed, const FLinearColor& Color);
	static void Stars(const UObject* Ctx, const FVector& Location, int32 Count, float Speed);
	static void Sparkles(const UObject* Ctx, const FVector& Location, int32 Count, float Radius, const FLinearColor& Color);
	static void Ring(const UObject* Ctx, const FVector& Location, float Radius, const FLinearColor& Color, float Duration = 0.35f);
	static void Flakes(const UObject* Ctx, const FVector& Location, int32 Count, float Speed, const FLinearColor& Color);
	/**
	 * An ice block bursting apart (an icicle knock-out's block shattering): a flash, big chunks starting all over where
	 * the block was (in front of what was inside it) and flying out on the play plane, small shards, frost and a ring.
	 * Size 1 = a penguin's block. Fewer pieces on lower effects settings (never fewer than a handful of chunks).
	 */
	static void IceBurst(const UObject* Ctx, const FVector& Location, float Size, const FLinearColor& Color);
	/**
	 * Gambler's fizzle: the ability that did not come out turns into sparkling dust. At Location (where it would have come out,
	 * Direction = the aim) a small burst in Color forms for a moment in the ability's shape (Shape; Size 1 = a normal penguin),
	 * then crumbles into glittering dust and chips that drift down and fade over about a second, with a pinch of grey smoke.
	 * Fewer particles on lower effects settings (never fewer than a handful). The sounds are the caller's (Sound).
	 */
	static void FizzleDust(const UObject* Ctx, const FVector& Location, const FVector& Direction, const FLinearColor& Color, float Size, EPinguDustShape Shape);
	/** Tests: the fizzle dust played on this machine so far, and the particles it made (what density left of them). */
	int32 GetFizzleDustEvents() const { return FizzleDustEvents; }
	int32 GetFizzleDustParticles() const { return FizzleDustParticles; }
	static void Splash(const UObject* Ctx, const FVector& Location);
	static void Explosion(const UObject* Ctx, const FVector& Location, float Radius);
	/** Translucent copy of the penguin's current pose that fades out (dash afterimage). */
	static void Ghost(const UObject* Ctx, USkeletalMeshComponent* Source, const FLinearColor& Color, float Life = 0.22f, float Opacity = 0.5f);
	/**
	 * A knocked-out penguin's little ghost: a translucent copy of Source's pose, upright, Scale = its world scale, its feet
	 * (the mesh origin) starting at Origin and turned Yaw degrees (0 = facing the camera). It pops in, drifts RiseSpeed cm/s
	 * up (negative: it sinks), swaying Sway cm from side to side and leaning into it, and fades out over Life seconds.
	 * An afterimage like Ghost's (the same capped pool: fewer at once on lower effects settings).
	 * bRestPose: the mesh's rest pose (standing upright) instead of a copy of Source's pose - a knocked-out penguin's pose
	 * (flopped on its back, seen end on) turns into a heap of see-through rings that reads as no penguin at all.
	 */
	static void Spirit(const UObject* Ctx, USkeletalMeshComponent* Source, const FVector& Origin, float Yaw, float Scale, float RiseSpeed,
		float Sway, const FLinearColor& Color, float Life = 1.2f, float Opacity = 0.6f, bool bRestPose = false);
	/**
	 * One piece of a soft glowing path (call it every frame with the segment a projectile just flew).
	 * Width in cm (the soft edge makes the visible core about 70% of it; it thins to half over its life),
	 * Color is the emitted colour (linear, > 1 glows brighter), fading from Opacity to 0 over Life seconds.
	 * Segments shorter than 1 cm are skipped.
	 */
	static void GlowTrail(const UObject* Ctx, const FVector& From, const FVector& To, float Width, const FLinearColor& Color, float Opacity = 0.3f, float Life = 0.3f);

	// ---- a snow bomb knock-out's ash pile (this machine only: every machine's death look drives its own)
	/**
	 * Shows PileOwner's ash pile as Pile says (call it every frame while it shows): engine shapes with /Game/FX/M_Prop, kept here
	 * so the pile stays when the knocked-out penguin hides. One per owner, made the first time and reused (never more than
	 * there are penguins); a part that has shrunk to nothing is hidden. It goes away with HideAshPile, or by itself when
	 * its owner is gone.
	 */
	static void SetAshPile(const UObject* PileOwner, const FPinguAshPile& Pile);
	static void HideAshPile(const UObject* PileOwner);
	/** Some of PileOwner's ash pile (heap or eyes) shows here now. */
	static bool IsAshPileShown(const UObject* PileOwner);
	/** How many parts of PileOwner's heap (OutHeap) and how many of its eye whites (OutEyes) show here now. */
	static void CountAshPileParts(const UObject* PileOwner, int32& OutHeap, int32& OutEyes);
	/** Where PileOwner's two eye whites are (world middles, the one further left first) and their radius; false unless both show. */
	static bool GetAshPileEyeSpots(const UObject* PileOwner, FVector& OutLeft, FVector& OutRight, float& OutRadius);
	/** The point between the pile's eyes (world). */
	static FVector GetAshPileEyes(const FPinguAshPile& Pile);

	// ---- feel
	static void Shake(const UObject* Ctx, float Trauma);
	/** Freezes the action for a moment (real time), then resumes. Solo only (does nothing in a LAN game). */
	static void HitStop(const UObject* Ctx, float RealSeconds, float Dilation = 0.03f);
	/** Base time dilation (round-end slow motion). 1 = normal. Solo only (does nothing in a LAN game). */
	static void SetSlowMotion(const UObject* Ctx, float Dilation);
	/** The kill cam's slow motion (APinguKillCam, every machine runs its own curve): LAN games too. New sounds play
	 *  deeper with it. 1 = normal. */
	static void SetKillCamDilation(const UObject* Ctx, float Dilation);

	// ---- audio
	static void Sound(const UObject* Ctx, EPinguSound Which, float Volume = 1.f, float PitchVariance = 0.06f, float Pitch = 1.f);
	/** 0 = silence, 1 = menu, 2 = battle. Always local. */
	static void Music(const UObject* Ctx, int32 Track);
	static void SetVolumes(const UObject* Ctx, float InSfxVolume, float InMusicVolume);

	// ---- LAN / engine helpers
	/** Plays one effect here (the host's effects arriving on a LAN client go through this). */
	void PlayEvent(const FPinguFXEvent& Event);
	/** 0.1 .. 1: the effects density of this machine (from the graphics settings). */
	float GetDensity() const { return Density; }
	/** The Ultra (Niagara) layers play here now. */
	bool UsesUltraEffects() const { return bUltra; }
	/** Tests: -1 the Effects setting decides whether the Ultra layers play (the default), 0 never, 1 always. */
	static void SetUltraOverride(int32 Mode);
	/** Tests: the Ultra snowfall's flakes in the air right now (0 when it is not running); OutDetail per emitter. */
	int32 CountUltraSnowflakes(FString& OutDetail) const;
	/** Sends an instanced mesh's changed instances to the GPU without rebuilding its render state (the island uses it
	 *  too). The command line switch -PinguFullRenderUpdate falls back to a full render state rebuild. */
	static void MarkInstancesChanged(UInstancedStaticMeshComponent* Instances);

	/** Gentle snowfall around the camera. */
	UPROPERTY(EditAnywhere, Category = "FX")
	bool bSnowfall = true;

	UPROPERTY(EditAnywhere, Category = "FX")
	float SfxVolume = 0.8f;

	UPROPERTY(EditAnywhere, Category = "FX")
	float MusicVolume = 0.45f;

private:
	struct FParticle
	{
		FPinguParticleDesc Desc;
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float Age = 0.f;
		float Phase = 0.f;
		bool bSnow = false;
		/** Its colour / opacity still has to be written to its instance (new, or moved to another instance). */
		bool bDataDirty = true;
	};

	/** The particles of one mesh: Live[i] is drawn by instance i, the instances after the live ones are parked. */
	struct FPool
	{
		TArray<FParticle> Live;
		/** Instances in the component. */
		int32 Size = 0;
		/** Instances [0, Shown) are not parked (they showed a particle at the last update). */
		int32 Shown = 0;
	};

	struct FGhost
	{
		TObjectPtr<UPoseableMeshComponent> Mesh;
		TObjectPtr<UMaterialInstanceDynamic> Material;
		float Age = 0.f;
		float Life = 0.f;
		float Opacity = 0.f;
		bool bActive = false;
		/** A spirit (PlaySpirit): it drifts from Origin (its feet) and sways, instead of staying where it was copied. */
		bool bDrift = false;
		FVector Origin = FVector::ZeroVector;
		float Yaw = 0.f;
		float Scale = 1.f;
		float RiseSpeed = 0.f;
		float Sway = 0.f;
		float Phase = 0.f;
	};
	/** Starts an afterimage of Source's pose (null when there is nothing to show); the caller places it. */
	FGhost* StartGhost(USkeletalMeshComponent* Source, const FLinearColor& Color, float Life, float Opacity, bool bForSpirit = false);
	/** One afterimage T (0..1) through its life: its fade, and a spirit's drift. */
	void UpdateGhost(FGhost& Entry, float T);

	/** Plays here and, inside a FPinguFXBroadcastScope on the LAN host, queues it for the clients. */
	static void Emit(const UObject* Ctx, const FPinguFXEvent& Event);
	/** LAN host: the relay that carries the broadcast effects to the clients (spawned on first use). */
	APinguFXRelay* GetRelay();

	void PlayPuffs(const FVector& Location, int32 Count, float Size, float Speed, const FVector& Direction, float Spread, const FLinearColor& Color);
	void PlayGroundPuffs(const FVector& FeetLocation, float Strength);
	void PlayFlapWind(const FVector& BodyLocation, float SizeScale, float GroundDistance, const FVector& GroundPoint, int32 Beat);
	void PlayTrail(const FVector& Location, float Size, const FLinearColor& Color, const FVector& Velocity);
	void PlayShards(const FVector& Location, int32 Count, float Speed, const FLinearColor& Color);
	void PlayStars(const FVector& Location, int32 Count, float Speed);
	void PlaySparkles(const FVector& Location, int32 Count, float Radius, const FLinearColor& Color);
	void PlayRing(const FVector& Location, float Radius, const FLinearColor& Color, float Duration);
	void PlayFlakes(const FVector& Location, int32 Count, float Speed, const FLinearColor& Color);
	void PlayIceBurst(const FVector& Location, float Size, const FLinearColor& Color);
	void PlayFizzleDust(const FVector& Location, const FVector& Direction, const FLinearColor& Color, float Size, EPinguDustShape Shape);
	void PlaySplash(const FVector& Location);
	void PlayExplosion(const FVector& Location, float Radius);
	void PlayGhost(USkeletalMeshComponent* Source, const FLinearColor& Color, float Life, float Opacity);
	void PlaySpirit(USkeletalMeshComponent* Source, const FVector& Origin, float Yaw, float Scale, float RiseSpeed, float Sway,
		const FLinearColor& Color, float Life, float Opacity, bool bRestPose);
	void PlayGlowTrail(const FVector& From, const FVector& To, float Width, const FLinearColor& Color, float Opacity, float Life);
	void PlayShake(float Trauma);
	void PlaySound(EPinguSound Which, float Volume, float PitchVariance, float Pitch);
	/** Every setting: the blast zone of an explosion (the real push radius) flashes and fades. */
	void PlayZoneFlash(const FVector& Location, float Radius);
	void UpdateZones(float Dt);

	// ---- Ultra (Niagara) layers; nothing without bUltra, the system or inside a preset that brings its own
	/** A pooled one-shot Niagara system at Location, not active yet (set its user parameters, then Activate). */
	UNiagaraComponent* StartNiagara(int32 Layer, const FVector& Location);
	/** Snow and ice: Dust fine bits, Chunks ice chunks, Glints twinkles, Mist soft puffs, flying out along Direction
	 *  (Spread 0) up to all round (Spread 1) at about Speed; Size scales the bits and puffs. */
	void UltraBurst(const FVector& Location, const FLinearColor& Color, int32 Dust, int32 Chunks, int32 Glints, int32 Mist,
		float Speed, float Size, const FVector& Direction, float Spread);
	void UltraGlints(const FVector& Location, const FLinearColor& Color, int32 Count, float Radius);
	void UltraRing(const FVector& Location, const FLinearColor& Color, float Radius, float Duration);
	void UltraBlast(const FVector& Location, float Radius);
	/** Ultra: the Niagara snowfall around the camera (on), or none (off). */
	void UpdateUltraSnowfall(float Dt);
	/** Loads the Ultra layers' systems (once, the first time they are needed). */
	void EnsureNiagara();
	/** Count * density, rounded at random so the average stays right (density 1: exactly Count). */
	int32 ScaleCount(int32 Count);

	void EnsureAssets();
	void SimulateParticles(float Dt);
	void UpdateSnowfall(float Dt);
	void UpdateGhosts(float Dt);
	void UpdateTimeDilation(float RealDt);
	void SpawnInternal(const FPinguParticleDesc& Desc, bool bSnow);
	/** A free (or recycled) ghost slot. Only a spirit (bForSpirit) may take over a drifting spirit's slot; an afterimage never does. */
	FGhost* AcquireGhost(int32 MaxGhosts, bool bForSpirit = false);
	USoundBase* GetSound(EPinguSound Which);

	/** PileOwner's ash pile slot (bCreate: a free one, its parts made the first time), INDEX_NONE when there is none. */
	int32 FindAshPile(const UObject* PileOwner, bool bCreate);
	/** The shapes and their materials for the ash piles (loaded once); false without them. */
	bool EnsureAshPileAssets();
	void PlaceAshPile(int32 Slot, const FPinguAshPile& Pile);
	/** Hides the slot's parts and frees it. */
	void ReleaseAshPile(int32 Slot);
	/** Frees the piles whose owner is gone (a penguin that left the game mid-way). */
	void UpdateAshPiles();

	UPROPERTY(VisibleAnywhere, Category = "FX")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "FX")
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Pools;

	UPROPERTY(VisibleAnywhere, Category = "FX")
	TObjectPtr<UAudioComponent> MusicComponent;

	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundBase>> Sounds;

	/** The Ultra layers' Niagara systems (PinguFXPrivate::NiagaraPaths), null where missing. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraSystem>> NiagaraSystems;

	/** Ultra: the snowfall around the camera. */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> SnowfallComponent;

	/** Blast-zone flashes (the engine plane with /Game/FX/M_BlastZone), reused; Zones has their state. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> ZoneMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> ZoneMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ZoneMaterial;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPoseableMeshComponent>> GhostMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> GhostMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> GhostMaterial;

	/** The one-slot afterimage penguin (/Game/Penguin/Ghost/SK_Penguin_Ghost: 1 draw, ~2.7k triangles, same bones);
	 *  without it the afterimage copies the source penguin's mesh. */
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMesh> GhostMesh;

	/** The ash piles' parts (PinguFXPrivate::NumAshParts per slot, in slot order), made once per slot and reused. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> AshPileParts;

	/** The ash piles' looks (/Game/FX/M_Prop, PinguFXPrivate::EAshLook): the ash, the charcoal, the eyes' white, the pupils,
	 *  the embers. Shared by every pile. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> AshPileMaterials;

	/** The heap's soot (/Game/FX/M_Ghost, unlit, opaque over the middle: the charred penguin's own look) laid over its parts
	 *  as their overlay material - the ash's and a slightly lighter one for the charcoal. Shared by every pile. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> AshSootMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> AshConeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> AshSphereMesh;

	struct FAshPileSlot
	{
		TWeakObjectPtr<const UObject> PileOwner;
		bool bInUse = false;
	};
	TArray<FAshPileSlot> AshPiles;
	/** The shapes' own bounds (the parts are fitted to boxes with them, whatever the meshes' pivots). */
	FBoxSphereBounds AshConeBounds = FBoxSphereBounds(ForceInit);
	FBoxSphereBounds AshSphereBounds = FBoxSphereBounds(ForceInit);
	bool bAshAssetsTried = false;

	struct FZoneFlash
	{
		FVector Location = FVector::ZeroVector;
		float Radius = 0.f;
		float Age = 0.f;
		float Life = 0.f;
		bool bActive = false;
	};

	TWeakObjectPtr<APinguFXRelay> Relay;
	TArray<FPool> PoolStates;
	/** One per ZoneMeshes entry. */
	TArray<FZoneFlash> Zones;
	/** One pool's transforms of this frame (kept: no allocations per frame). */
	TArray<FTransform> TransformScratch;
	TArray<FGhost> Ghosts;
	bool bAssetsReady = false;
	float Density = 1.f;
	float SnowAccumulator = 0.f;
	int32 SnowCount = 0;
	float HitStopLeft = 0.f;
	float HitStopDilation = 1.f;
	float SlowMotion = 1.f;
	float KillCamDilation = 1.f;
	float AppliedDilation = 1.f;
	int32 MusicTrack = 0;
	/** Ultra layers play (the setting, or a test's override). */
	bool bUltra = false;
	bool bNiagaraLoaded = false;
	/** The Ultra snowfall was started and not stopped since (a stopped one stays "active" while its flakes finish
	 *  falling, so IsActive() cannot tell). The instanced snowfall runs whenever this is off. */
	bool bUltraSnowfallOn = false;
	/** Inside a preset that brings its own Ultra layer (an explosion's shards and ring): the nested presets add none. */
	int32 UltraNesting = 0;
	/** Fizzle dust played here, and the particles it spawned (GetFizzleDustEvents / GetFizzleDustParticles). */
	int32 FizzleDustEvents = 0;
	int32 FizzleDustParticles = 0;
	FRandomStream Rand;
};

/**
 * LAN: carries the host's broadcast effects to the clients. Spawned by the host (APinguFX), always relevant; it
 * collects the effects of a moment and sends them in one unreliable multicast (at most ~60 per second, so the
 * engine's limit of two queued multicasts per net update is never hit). The clients play them locally.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class PENGUIN_API APinguFXRelay : public AActor
{
	GENERATED_BODY()

public:
	APinguFXRelay();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Host: sends the effect with the next batch. */
	void Queue(const FPinguFXEvent& Event);
	void QueueParticle(const FPinguParticleDesc& Desc);

	/** Host -> clients: effects to play (the host played them already). */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayFX(const FPinguFXBatch& Batch);

private:
	void Flush();

	/** Effects waiting for the next send (a UPROPERTY so the afterimage meshes it points at stay known to the GC). */
	UPROPERTY(Transient)
	FPinguFXBatch Pending;

	double LastFlushTime = -1.0;
};

// Pingu - particles, afterimages, screen shake, hit-stop, sounds and music.

#include "PinguFX.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PinguSettings.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"
#include "ReferenceSkeleton.h"
#include "Sound/SoundBase.h"

namespace PinguFXPrivate
{
	// Per EPinguFXMesh (every table below has one entry per mesh).
	static const TCHAR* MeshPaths[] = {
		TEXT("/Game/FX/SM_Puff.SM_Puff"),
		TEXT("/Game/FX/SM_Shard.SM_Shard"),
		TEXT("/Game/FX/SM_Star.SM_Star"),
		TEXT("/Game/FX/SM_Ring.SM_Ring"),
		TEXT("/Game/FX/SM_Flake.SM_Flake"),
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), // Glow: 100 cm tall along Z, 100 cm wide, pivot in the middle
	};
	static const TCHAR* FallbackMeshPaths[] = {
		TEXT("/Engine/BasicShapes/Sphere.Sphere"),
		TEXT("/Engine/BasicShapes/Cone.Cone"),
		TEXT("/Engine/BasicShapes/Cube.Cube"),
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
		TEXT("/Engine/BasicShapes/Cube.Cube"),
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
	};
	static const int32 PoolSizes[] = { 380, 140, 90, 24, 160, 500 };
	static const TCHAR* PoolNames[] = { TEXT("Puffs"), TEXT("Shards"), TEXT("Stars"), TEXT("Rings"), TEXT("Flakes"), TEXT("Glows") };
	constexpr int32 NumMeshes = static_cast<int32>(EPinguFXMesh::Count);
	static_assert(UE_ARRAY_COUNT(MeshPaths) == NumMeshes, "FX mesh table out of sync with EPinguFXMesh");
	static_assert(UE_ARRAY_COUNT(FallbackMeshPaths) == NumMeshes, "FX fallback mesh table out of sync with EPinguFXMesh");
	static_assert(UE_ARRAY_COUNT(PoolSizes) == NumMeshes, "FX pool size table out of sync with EPinguFXMesh");
	static_assert(UE_ARRAY_COUNT(PoolNames) == NumMeshes, "FX pool name table out of sync with EPinguFXMesh");
	static const TCHAR* FXMaterialPath = TEXT("/Game/FX/M_FX.M_FX");
	/** Glow pool: translucent unlit with a soft edge (built by Tools/ue_setup_project.py); falls back to M_FX. */
	static const TCHAR* TrailMaterialPath = TEXT("/Game/FX/M_Trail.M_Trail");
	/** Glow trails: emissive custom data. M_Trail emits RGB * (0.5 + this), so 0.5 emits exactly the colour. */
	constexpr float TrailGlow = 0.5f;
	constexpr int32 NumCustomData = 5; // R, G, B, emissive, opacity
	constexpr int32 MaxSnow = 150;
	constexpr int32 MaxGhosts = 8;
	// A knocked-out penguin's spirit (PlaySpirit): it pops in from this share of its size within this share of its life,
	// sways this many times a second (reaching its full sway after SpiritSwayInTime s, leaning this many degrees into it),
	// slows its drift by this share toward its end, fades in over the first share and out from the second. Slow and
	// gentle on purpose: a see-through mesh has no motion vectors, so a fast one smears into stacked copies under the
	// temporal anti-aliasing; a slow one stays one clear little ghost, fully there for most of its life.
	constexpr float SpiritPopFrom = 0.55f;
	constexpr float SpiritPopTime = 0.1f;
	constexpr float SpiritSwayRate = 0.8f;
	constexpr float SpiritSwayInTime = 0.3f;
	constexpr float SpiritLean = 7.f;
	constexpr float SpiritSlowDown = 0.45f;
	constexpr float SpiritFadeIn = 0.08f;
	constexpr float SpiritFadeOutFrom = 0.7f;
	// An ice block bursting apart (PlayIceBurst): this many big chunks (scaled by the effects density, never fewer than
	// the minimum) and small shards; the chunks start this far out from the middle (cm, x the block size; in front of the
	// penguin, toward the camera between the two depths) and fly out this fast, a little up, braked by this drag (1/s) and
	// pulled down by this gravity (cm/s^2), so they stay round the penguin a moment before they tumble away and fall. The
	// Ultra layer's own burst keeps its speed.
	constexpr int32 IceBurstChunks = 12;
	constexpr int32 IceBurstMinChunks = 5;
	constexpr int32 IceBurstShards = 14;
	constexpr float IceBurstHalfWidth = 35.f;
	constexpr float IceBurstHalfHeight = 40.f;
	constexpr float IceBurstNearDepth = 40.f;
	constexpr float IceBurstFarDepth = 70.f;
	constexpr float IceBurstMinSpeed = 240.f;
	constexpr float IceBurstMaxSpeed = 520.f;
	constexpr float IceBurstDrag = 1.4f;
	constexpr float IceBurstGravity = 1250.f;
	constexpr float IceBurstUltraSpeed = 1000.f;
	constexpr float IceBurstLift = 0.45f;
	// A chunk is the shard mesh (a slim six-sided crystal, 100 cm long along its X, about 22 cm thick) this long (cm, x the
	// block size), thickened along its Z to this many times its length (a fat wedge of ice instead of a needle), shrinking
	// to this share by the end of its life.
	constexpr float IceChunkMinSize = 42.f;
	constexpr float IceChunkMaxSize = 72.f;
	constexpr float IceChunkThick = 2.2f;
	constexpr float IceChunkEndShare = 0.7f;
	// Free instances wait below the (opaque) sea: far enough to stay unseen, close enough that the pools' bounds stay
	// small (bounds-based systems - culling, virtual shadow maps, Lumen - treat a pool spanning a kilometre as huge).
	static const FVector HiddenLocation(0.f, 0.f, -6000.f);
	/** LAN: broadcast batches at most this often (s). The engine drops a third unreliable multicast queued for one
	 *  net update, and a busy listen server updates each client about 60+ times a second. */
	constexpr double MinFlushInterval = 1.0 / 60.0 - 0.001;
	/** Effects per batch at most (they are cosmetic: the rest of an extreme moment is dropped). */
	constexpr int32 MaxEventsPerBatch = 96;
	constexpr int32 MaxParticlesPerBatch = 48;

	static const TCHAR* SoundNames[] = {
		TEXT("ui_hover"), TEXT("ui_click"), TEXT("ui_back"), TEXT("ui_start"),
		TEXT("count_beep"), TEXT("count_go"),
		TEXT("jump"), TEXT("double_jump"), TEXT("land"), TEXT("land_hard"), TEXT("dash"), TEXT("slide_start"), TEXT("skid"), TEXT("ice_slip"),
		TEXT("throw"), TEXT("snowball_hit"), TEXT("icicle_throw"), TEXT("icicle_hit"), TEXT("stomp_whoosh"), TEXT("stomp_impact"), TEXT("iceberg_summon"), TEXT("iceberg_crash"),
		TEXT("slap"), TEXT("frost"), TEXT("freeze"), TEXT("shatter"), TEXT("wall_build"), TEXT("warp"), TEXT("hook_throw"), TEXT("hook_hit"), TEXT("shield_on"), TEXT("shield_block"), TEXT("gust"),
		TEXT("bomb_throw"), TEXT("bomb_fuse"), TEXT("explosion"), TEXT("hit"), TEXT("ko"), TEXT("splash"), TEXT("squawk"), TEXT("melt"),
		TEXT("round_win"), TEXT("match_win"), TEXT("match_lose"),
		TEXT("noot"), TEXT("ice_gun"), TEXT("slippery"), TEXT("shrink"), TEXT("grow"),
		TEXT("crumble"),
		TEXT("pickup"), TEXT("drop"),
		TEXT("tornado_merge"),
		TEXT("giant_rollup"), TEXT("giant_roll"), TEXT("giant_gulp"), TEXT("giant_bounce"), TEXT("giant_burst"),
		TEXT("snowman_build"), TEXT("snowman_throw"), TEXT("snowman_crumble"),
		TEXT("flap"),
	};
	static_assert(UE_ARRAY_COUNT(SoundNames) == static_cast<int32>(EPinguSound::Count), "sound table out of sync");

	/** Open FPinguFXBroadcastScopes of one world (game thread only). */
	struct FBroadcastDepth
	{
		const UWorld* World = nullptr;
		int32 Depth = 0;
	};
	static TArray<FBroadcastDepth> GBroadcastDepths;

	/** Each world's effects actor (Get runs for every single effect, so no actor search each time). */
	struct FWorldFX
	{
		const UWorld* World = nullptr;
		TWeakObjectPtr<APinguFX> FX;
	};
	static TArray<FWorldFX> GWorldFX;

	static bool IsHostWorld(const UWorld* World)
	{
		const ENetMode Mode = World ? World->GetNetMode() : NM_Standalone;
		return Mode == NM_ListenServer || Mode == NM_DedicatedServer;
	}

	/** Hosting or joined a LAN game (hit-stop and slow motion are solo only). */
	static bool IsNetworkWorld(const UWorld* World)
	{
		return World && World->GetNetMode() != NM_Standalone;
	}

	static float ReadDensity()
	{
		return FMath::Clamp(UPinguSettings::GetFXDensity(), 0.1f, 1.f);
	}

	static uint8 ClampCount(int32 Count)
	{
		return static_cast<uint8>(FMath::Clamp(Count, 0, 255));
	}

	static bool UseFullRenderUpdates()
	{
		static const bool bFull = FParse::Param(FCommandLine::Get(), TEXT("PinguFullRenderUpdate"));
		return bFull;
	}

	/** Ultra layers (APinguFX::StartNiagara's Layer) and their systems (Tools/niagara_recipe.json). */
	enum ENiagaraLayer : int32 { LayerBlast, LayerBurst, LayerGlints, LayerRing, LayerSnowfall, NumLayers };
	static const TCHAR* NiagaraPaths[] = {
		TEXT("/Game/FX/Niagara/NS_PinguBlast.NS_PinguBlast"),
		TEXT("/Game/FX/Niagara/NS_PinguBurst.NS_PinguBurst"),
		TEXT("/Game/FX/Niagara/NS_PinguGlints.NS_PinguGlints"),
		TEXT("/Game/FX/Niagara/NS_PinguRing.NS_PinguRing"),
		TEXT("/Game/FX/Niagara/NS_PinguSnowfall.NS_PinguSnowfall"),
	};
	static_assert(UE_ARRAY_COUNT(NiagaraPaths) == NumLayers, "Niagara layer table out of sync with ENiagaraLayer");

	/** Tests' override of the Ultra layers: -1 the Effects setting decides, 0 off, 1 on. */
	static int32 GUltraOverride = -1;

	/** The Ultra layers play: the setting (Effects Ultra), -PinguUltraFX / -PinguNoUltraFX, or a test's override. */
	static bool WantUltra()
	{
		if (GUltraOverride >= 0)
		{
			return GUltraOverride == 1;
		}
		static const bool bForceOn = FParse::Param(FCommandLine::Get(), TEXT("PinguUltraFX"));
		static const bool bForceOff = FParse::Param(FCommandLine::Get(), TEXT("PinguNoUltraFX"));
		return !bForceOff && (bForceOn || UPinguSettings::UseUltraEffects());
	}

	/** An explosion's blast-zone flash: how long it shows, how many can show at once, and the knock-out zone's share of
	 *  the radius it draws (the snow bomb's 115 of 330 cm). */
	constexpr float ZoneFlashLife = 0.6f;
	constexpr int32 MaxZones = 4;
	constexpr float ZoneInner = 115.f / 330.f;
	static const TCHAR* ZoneMaterialPath = TEXT("/Game/FX/M_BlastZone.M_BlastZone");
	/** The explosion look was made for a 330 cm blast (sizes and speeds scale with Radius / this). */
	constexpr float ExplosionLookRadius = 330.f;

	// A snow bomb knock-out's ash pile (APinguFX::SetAshPile): engine shapes with the props' material (/Game/FX/M_Prop:
	// Color x vertex colour, Roughness, Emissive), one set of parts per penguin that needs one (never more than penguins).
	constexpr int32 MaxAshPiles = 10;
	static const TCHAR* AshMaterialPath = TEXT("/Game/FX/M_Prop.M_Prop");
	static const TCHAR* AshFallbackMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	static const TCHAR* AshConePath = TEXT("/Engine/BasicShapes/Cone.Cone");
	static const TCHAR* AshSpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	// The looks: soot-black ash (matt; so dark the warm sunlight cannot turn it brown), charcoal (as black, with a soft
	// sheen, so the lumps read apart), the eyes' white (glowing a little, so they read on the black heap from far), glossy
	// black pupils, glowing orange embers.
	enum EAshLook : int32 { AshLookAsh, AshLookCharcoal, AshLookWhite, AshLookPupil, AshLookEmber, NumAshLooks };
	static const FLinearColor AshLookColors[NumAshLooks] = { FLinearColor(0.0016f, 0.0015f, 0.0015f), FLinearColor(0.003f, 0.003f, 0.0032f),
		FLinearColor(1.f, 1.f, 1.f), FLinearColor(0.004f, 0.004f, 0.005f), FLinearColor(1.f, 0.36f, 0.06f) };
	static const float AshLookGlows[NumAshLooks] = { 0.f, 0.f, 0.3f, 0.f, 9.f };
	static const float AshLookRoughness[NumAshLooks] = { 0.95f, 0.7f, 0.5f, 0.25f, 0.6f };
	// Lit, even that black turns brown in the sun (the specular): the heap wears the charred penguin's soot over it too -
	// M_Ghost (unlit) as each part's overlay material, opaque over the middle of every surface (Opacity x 0.25 past 1), its
	// outline a shade lighter so the lumps read apart; the charcoal's a little lighter still. It keeps its shadow.
	static const TCHAR* AshSootPath = TEXT("/Game/FX/M_Ghost.M_Ghost");
	static const FLinearColor AshSootColors[2] = { FLinearColor(0.012f, 0.011f, 0.01f), FLinearColor(0.024f, 0.023f, 0.022f) };
	constexpr float AshSootOpacity = 4.4f;
	/** One part of the heap (cm, size 1, from the floor point under its middle: X along the play plane, Y toward the
	 *  camera, Z up): the middle of its box, its half size, a cone or a sphere, its look. */
	struct FAshShape
	{
		FVector Centre;
		FVector Half;
		bool bCone;
		EAshLook Look;
	};
	// A penguin's worth of ash: about 95 cm across and 50 high - a cone with a round top, lumps on its shoulders, at its
	// foot, in front and behind, and charcoal bits spilled round it.
	static const FAshShape AshShapes[] = {
		{ FVector(0.f, 0.f, 19.f), FVector(44.f, 34.f, 21.f), true, AshLookAsh },          // the body, its foot a little in the floor
		{ FVector(0.f, 2.f, 36.f), FVector(19.f, 16.f, 12.f), false, AshLookAsh },         // a round top over the cone's tip
		{ FVector(-21.f, 6.f, 20.f), FVector(19.f, 17.f, 14.f), false, AshLookCharcoal },  // shoulders
		{ FVector(22.f, -2.f, 17.f), FVector(18.f, 16.f, 13.f), false, AshLookAsh },
		{ FVector(-4.f, 20.f, 11.f), FVector(17.f, 10.f, 10.f), false, AshLookCharcoal },  // in front, low (under the eyes)
		{ FVector(-38.f, 8.f, 5.f), FVector(15.f, 13.f, 9.f), false, AshLookAsh },         // at its foot
		{ FVector(40.f, 4.f, 4.f), FVector(14.f, 12.f, 8.f), false, AshLookCharcoal },
		{ FVector(4.f, -20.f, 12.f), FVector(22.f, 13.f, 12.f), false, AshLookAsh },       // behind
		{ FVector(-55.f, 16.f, 3.f), FVector(6.f, 5.f, 4.5f), false, AshLookCharcoal },    // spilled bits
		{ FVector(57.f, 20.f, 2.5f), FVector(5.f, 4.5f, 3.5f), false, AshLookAsh },
		{ FVector(28.f, 30.f, 2.f), FVector(4.5f, 4.f, 3.f), false, AshLookCharcoal },
		{ FVector(-30.f, 32.f, 2.5f), FVector(5.f, 4.f, 3.5f), false, AshLookAsh },
		{ FVector(-12.f, -30.f, 2.f), FVector(4.f, 4.f, 3.f), false, AshLookCharcoal },
	};
	constexpr int32 NumAshHeapParts = UE_ARRAY_COUNT(AshShapes);
	/** The heap parts that cast a shadow (the body, its top and shoulders: it stands on the floor); the small ones do not. */
	constexpr int32 NumAshShadowParts = 4;
	/** The heap piles up from the floor: a part this high (cm, size 1) and higher comes last, this share of the pile-up later. */
	constexpr float AshGrowTop = 40.f;
	constexpr float AshGrowStagger = 0.45f;
	/** Embers glowing on the fresh heap (a middle on its surface, cm at size 1, and a radius). */
	struct FAshEmber
	{
		FVector Centre;
		float Radius;
	};
	static const FAshEmber AshEmbers[] = {
		{ FVector(-14.f, 26.f, 16.f), 1.8f },
		{ FVector(17.f, 19.f, 26.f), 1.5f },
		{ FVector(-31.f, 17.f, 20.f), 1.6f },
		{ FVector(31.f, 12.f, 14.f), 1.4f },
	};
	constexpr int32 NumAshEmbers = UE_ARRAY_COUNT(AshEmbers);
	// The parts of one pile, in this order: the heap, the embers, the eyes' whites, the pupils.
	constexpr int32 AshFirstEmber = NumAshHeapParts;
	constexpr int32 AshEyeLeft = AshFirstEmber + NumAshEmbers;
	constexpr int32 AshEyeRight = AshEyeLeft + 1;
	constexpr int32 AshPupilLeft = AshEyeLeft + 2;
	constexpr int32 AshPupilRight = AshEyeLeft + 3;
	constexpr int32 NumAshParts = AshEyeLeft + 4;
	// The eyes on the pile: each one's middle this far to the side of the pile's middle, this high, this far toward the
	// camera (on its top, half sunk into it); round, the size of the penguin's own eyes (its eyeballs, 9.35 cm at size 1,
	// and a little: on the charred penguin they lie over them). The pupils: round black caps on the front of the whites
	// (their middle this far out of the white's middle, x its radius, this half size), looking this far to the side at most
	// and a little down.
	constexpr float AshEyeApart = 11.2f;
	constexpr float AshEyeHeight = 41.f;
	constexpr float AshEyeDepth = 10.f;
	constexpr float AshEyeRadius = 9.9f;
	constexpr float AshPupilOut = 0.93f;
	const FVector AshPupilHalf(0.4f, 0.15f, 0.44f);
	constexpr float AshPupilSide = 0.55f;
	constexpr float AshPupilDown = 0.12f;
	/** A blink never squeezes the eyes thinner than this (a line, not nothing). */
	constexpr float AshShutShare = 0.08f;
	// Blown away: each heap part goes over this share of the blow, the later the further downwind and the lower it is;
	// on its way it slides this far downwind, and the whole heap leans downwind up to this many degrees and shears (its
	// higher parts slide further: this share of their height).
	constexpr float AshPartBlowShare = 0.55f;
	constexpr float AshPartDrift = 18.f;
	constexpr float AshLean = 22.f;
	constexpr float AshShear = 0.35f;
	/** A part smaller than this share of its size is hidden instead (no near-zero scales). */
	constexpr float AshMinShare = 0.03f;

	/** The transform that fits a mesh with these bounds into a box: its middle Centre, half size Half, turned by Turn. */
	static FTransform FitBox(const FBoxSphereBounds& Bounds, const FVector& Centre, const FVector& Half, const FQuat& Turn)
	{
		const FVector Extent = Bounds.BoxExtent.ComponentMax(FVector(0.01));
		const FVector Scale = Half / Extent;
		return FTransform(Turn, Centre - Turn.RotateVector(Bounds.Origin * Scale), Scale);
	}

	static FVector RandomInCone(FRandomStream& Rand, const FVector& Direction, float Spread)
	{
		// Direction on the play plane (X/Z) plus some depth so puffs are not paper-flat.
		const FVector Dir = Direction.GetSafeNormal();
		const float Angle = Rand.FRandRange(-UE_PI, UE_PI) * FMath::Clamp(Spread, 0.f, 1.f) * 0.5f;
		const float C = FMath::Cos(Angle), S = FMath::Sin(Angle);
		FVector Out(Dir.X * C - Dir.Z * S, 0.f, Dir.X * S + Dir.Z * C);
		Out.Y = Rand.FRandRange(-0.35f, 0.35f) * Spread;
		return Out.GetSafeNormal();
	}
}

// ---------------------------------------------------------------------------- broadcast scope

FPinguFXBroadcastScope::FPinguFXBroadcastScope(const UObject* WorldContext)
{
	using namespace PinguFXPrivate;
	const UWorld* ContextWorld = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!ContextWorld || !ContextWorld->IsGameWorld() || !IsHostWorld(ContextWorld))
	{
		return; // solo, or a LAN client: effects stay on this machine
	}
	World = ContextWorld;
	bActive = true;
	for (FBroadcastDepth& Entry : GBroadcastDepths)
	{
		if (Entry.World == World)
		{
			++Entry.Depth;
			return;
		}
	}
	FBroadcastDepth& Entry = GBroadcastDepths.AddDefaulted_GetRef();
	Entry.World = World;
	Entry.Depth = 1;
}

FPinguFXBroadcastScope::~FPinguFXBroadcastScope()
{
	using namespace PinguFXPrivate;
	if (!bActive)
	{
		return;
	}
	for (int32 Index = 0; Index < GBroadcastDepths.Num(); ++Index)
	{
		if (GBroadcastDepths[Index].World == World)
		{
			if (--GBroadcastDepths[Index].Depth <= 0)
			{
				GBroadcastDepths.RemoveAtSwap(Index);
			}
			return;
		}
	}
}

bool FPinguFXBroadcastScope::IsBroadcasting(const UWorld* InWorld)
{
	if (!InWorld)
	{
		return false;
	}
	for (const PinguFXPrivate::FBroadcastDepth& Entry : PinguFXPrivate::GBroadcastDepths)
	{
		if (Entry.World == InWorld)
		{
			return Entry.Depth > 0;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------- actor

APinguFX::APinguFX()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork; // after the penguins and the camera moved
	PrimaryActorTick.bTickEvenWhenPaused = true;
	SetActorEnableCollision(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	for (int32 Index = 0; Index < PinguFXPrivate::NumMeshes; ++Index)
	{
		UInstancedStaticMeshComponent* Pool = CreateDefaultSubobject<UInstancedStaticMeshComponent>(PinguFXPrivate::PoolNames[Index]);
		Pool->SetupAttachment(Root);
		Pool->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pool->SetGenerateOverlapEvents(false);
		Pool->SetCanEverAffectNavigation(false);
		Pool->SetCastShadow(false);
		// Particles light nothing: keep them out of Lumen, distance fields, ray tracing and the captures (High / Ultra cost).
		Pool->bAffectDistanceFieldLighting = false;
		Pool->bAffectDynamicIndirectLighting = false;
		Pool->bVisibleInRayTracing = false;
		Pool->bVisibleInReflectionCaptures = false;
		Pool->bVisibleInRealTimeSkyCaptures = false;
		Pool->NumCustomDataFloats = PinguFXPrivate::NumCustomData;
		if (Index == static_cast<int32>(EPinguFXMesh::Glow))
		{
			// Translucent: draw the trails before the other see-through things (ghosts, shields, ice), which sit in
			// front of them. The pool's bounds include the parked instances far below, so distance sorting is useless.
			Pool->SetTranslucentSortPriority(-1);
		}
		Pools.Add(Pool);
	}

	MusicComponent = CreateDefaultSubobject<UAudioComponent>(TEXT("Music"));
	MusicComponent->SetupAttachment(Root);
	MusicComponent->bAutoActivate = false;
	MusicComponent->bIsUISound = true;
	MusicComponent->bAllowSpatialization = false;

	Rand.Initialize(0x51A7);
}

APinguFX* APinguFX::Get(const UObject* WorldContext)
{
	using namespace PinguFXPrivate;
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld())
	{
		return nullptr;
	}
	for (int32 Index = GWorldFX.Num() - 1; Index >= 0; --Index)
	{
		APinguFX* Cached = GWorldFX[Index].FX.Get();
		if (!Cached || Cached->IsActorBeingDestroyed())
		{
			GWorldFX.RemoveAtSwap(Index);
			continue;
		}
		if (GWorldFX[Index].World == World && Cached->GetWorld() == World)
		{
			return Cached;
		}
	}
	APinguFX* Found = nullptr;
	for (TActorIterator<APinguFX> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			Found = *It;
			break;
		}
	}
	if (!Found)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Found = World->SpawnActor<APinguFX>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	if (Found)
	{
		FWorldFX& Entry = GWorldFX.AddDefaulted_GetRef();
		Entry.World = World;
		Entry.FX = Found;
	}
	return Found;
}

void APinguFX::BeginPlay()
{
	Super::BeginPlay();
	EnsureAssets();
	// LAN host: the relay exists before the first broadcast effect (a client gets it as soon as it joins).
	if (PinguFXPrivate::IsHostWorld(GetWorld()))
	{
		GetRelay();
	}
}

APinguFXRelay* APinguFX::GetRelay()
{
	if (APinguFXRelay* Existing = Relay.Get())
	{
		return Existing;
	}
	UWorld* World = GetWorld();
	if (!World || !PinguFXPrivate::IsHostWorld(World))
	{
		return nullptr;
	}
	for (TActorIterator<APinguFXRelay> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			Relay = *It;
			return *It;
		}
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguFXRelay* Spawned = World->SpawnActor<APinguFXRelay>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	Relay = Spawned;
	return Spawned;
}

void APinguFX::MarkInstancesChanged(UInstancedStaticMeshComponent* Instances)
{
	if (!Instances)
	{
		return;
	}
	if (PinguFXPrivate::UseFullRenderUpdates())
	{
		Instances->MarkRenderStateDirty();
	}
	else
	{
		Instances->MarkRenderInstancesDirty();
	}
}

void APinguFX::EnsureAssets()
{
	using namespace PinguFXPrivate;
	if (bAssetsReady)
	{
		return;
	}
	bAssetsReady = true;
	Density = ReadDensity();
	bUltra = WantUltra();

	UMaterialInterface* FXMaterial = LoadObject<UMaterialInterface>(nullptr, FXMaterialPath);
	UMaterialInterface* TrailMaterial = LoadObject<UMaterialInterface>(nullptr, TrailMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
	if (!TrailMaterial)
	{
		UE_LOG(LogPingu, Warning, TEXT("FX material %s missing (re-run the fx setup phase), glow trails use M_FX"), TrailMaterialPath);
	}
	PoolStates.SetNum(NumMeshes);
	int32 LargestPool = 0;
	for (int32 Index = 0; Index < Pools.Num() && Index < NumMeshes; ++Index)
	{
		UInstancedStaticMeshComponent* Pool = Pools[Index];
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, MeshPaths[Index]);
		if (!Mesh)
		{
			UE_LOG(LogPingu, Warning, TEXT("FX mesh %s missing, using a basic shape"), MeshPaths[Index]);
			Mesh = LoadObject<UStaticMesh>(nullptr, FallbackMeshPaths[Index]);
		}
		Pool->SetStaticMesh(Mesh);
		UMaterialInterface* PoolMaterial = (Index == static_cast<int32>(EPinguFXMesh::Glow) && TrailMaterial) ? TrailMaterial : FXMaterial;
		if (PoolMaterial)
		{
			Pool->SetMaterial(0, PoolMaterial);
		}
		Pool->SetNumCustomDataFloats(NumCustomData);
		Pool->ClearInstances();
		FPool& State = PoolStates[Index];
		State.Size = PoolSizes[Index];
		State.Shown = 0;
		State.Live.Reset();
		State.Live.Reserve(State.Size); // never grows: a full pool recycles its oldest particle
		LargestPool = FMath::Max(LargestPool, State.Size);
		TArray<FTransform> Hidden;
		Hidden.Init(FTransform(FRotator::ZeroRotator, HiddenLocation, FVector(0.001f)), State.Size);
		Pool->AddInstances(Hidden, false, true);
	}
	TransformScratch.Reserve(LargestPool);

	GhostMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Ghost.M_Ghost"));
	GhostMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Penguin/Ghost/SK_Penguin_Ghost.SK_Penguin_Ghost"), nullptr, LOAD_Quiet | LOAD_NoWarn);

	Sounds.SetNum(static_cast<int32>(EPinguSound::Count));
	for (int32 Index = 0; Index < Sounds.Num(); ++Index)
	{
		const FString Path = FString::Printf(TEXT("/Game/Audio/S_%s.S_%s"), SoundNames[Index], SoundNames[Index]);
		Sounds[Index] = LoadObject<USoundBase>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
	}
}

void APinguFX::SetUltraOverride(int32 Mode)
{
	PinguFXPrivate::GUltraOverride = FMath::Clamp(Mode, -1, 1);
}

void APinguFX::EnsureNiagara()
{
	using namespace PinguFXPrivate;
	if (bNiagaraLoaded)
	{
		return;
	}
	bNiagaraLoaded = true;
	NiagaraSystems.SetNum(NumLayers);
	int32 Missing = 0;
	for (int32 Index = 0; Index < NumLayers; ++Index)
	{
		NiagaraSystems[Index] = LoadObject<UNiagaraSystem>(nullptr, NiagaraPaths[Index], nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!NiagaraSystems[Index])
		{
			++Missing;
			UE_LOG(LogPingu, Warning, TEXT("[FX] Ultra effects: %s missing (run the niagara setup phase)"), NiagaraPaths[Index]);
		}
	}
	UE_LOG(LogPingu, Display, TEXT("[FX] Ultra effects: %d of %d Niagara layers"), NumLayers - Missing, NumLayers);
}

// ---------------------------------------------------------------------------- Ultra (Niagara) layers

UNiagaraComponent* APinguFX::StartNiagara(int32 Layer, const FVector& Location)
{
	if (!bUltra || UltraNesting > 0)
	{
		return nullptr;
	}
	EnsureNiagara();
	UNiagaraSystem* System = NiagaraSystems.IsValidIndex(Layer) ? NiagaraSystems[Layer].Get() : nullptr;
	if (!System)
	{
		return nullptr;
	}
	// Pooled, not active yet: the caller sets the user parameters first. Off-screen ones are not spawned at all.
	return UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, System, Location, FRotator::ZeroRotator, FVector(1.f), true, false,
		ENCPoolMethod::AutoRelease, true);
}

void APinguFX::UltraBurst(const FVector& Location, const FLinearColor& Color, int32 Dust, int32 Chunks, int32 Glints, int32 Mist,
	float Speed, float Size, const FVector& Direction, float Spread)
{
	UNiagaraComponent* Layer = StartNiagara(PinguFXPrivate::LayerBurst, Location);
	if (!Layer)
	{
		return;
	}
	const FVector Dir = Direction.IsNearlyZero() ? FVector::UpVector : Direction.GetSafeNormal();
	Layer->SetVariableLinearColor(TEXT("Color"), Color);
	Layer->SetVariableInt(TEXT("Dust"), FMath::Clamp(Dust, 0, 200));
	Layer->SetVariableInt(TEXT("Chunks"), FMath::Clamp(Chunks, 0, 60));
	Layer->SetVariableInt(TEXT("Glints"), FMath::Clamp(Glints, 0, 60));
	Layer->SetVariableInt(TEXT("Mist"), FMath::Clamp(Mist, 0, 30));
	Layer->SetVariableFloat(TEXT("Speed"), FMath::Max(Speed, 0.f));
	Layer->SetVariableFloat(TEXT("Size"), FMath::Max(Size, 1.f));
	Layer->SetVariableVec3(TEXT("Direction"), Dir);
	Layer->SetVariableFloat(TEXT("Spread"), FMath::Clamp(Spread, 0.f, 1.f));
	Layer->Activate(true);
}

void APinguFX::UltraGlints(const FVector& Location, const FLinearColor& Color, int32 Count, float Radius)
{
	UNiagaraComponent* Layer = StartNiagara(PinguFXPrivate::LayerGlints, Location);
	if (!Layer)
	{
		return;
	}
	Layer->SetVariableLinearColor(TEXT("Color"), Color);
	Layer->SetVariableInt(TEXT("Count"), FMath::Clamp(Count, 0, 80));
	Layer->SetVariableFloat(TEXT("Radius"), FMath::Max(Radius, 1.f));
	Layer->Activate(true);
}

void APinguFX::UltraRing(const FVector& Location, const FLinearColor& Color, float Radius, float Duration)
{
	UNiagaraComponent* Layer = StartNiagara(PinguFXPrivate::LayerRing, Location);
	if (!Layer)
	{
		return;
	}
	Layer->SetVariableLinearColor(TEXT("Color"), Color);
	Layer->SetVariableFloat(TEXT("Radius"), FMath::Max(Radius, 1.f));
	Layer->SetVariableFloat(TEXT("Duration"), FMath::Clamp(Duration, 0.1f, 2.f));
	Layer->Activate(true);
}

void APinguFX::UltraBlast(const FVector& Location, float Radius)
{
	UNiagaraComponent* Layer = StartNiagara(PinguFXPrivate::LayerBlast, Location);
	if (!Layer)
	{
		return;
	}
	Layer->SetVariableFloat(TEXT("Radius"), Radius);
	Layer->SetVariableFloat(TEXT("Scale"), Radius / PinguFXPrivate::ExplosionLookRadius);
	Layer->Activate(true);
}

void APinguFX::UpdateUltraSnowfall(float Dt)
{
	using namespace PinguFXPrivate;
	if (!bUltra || !bSnowfall)
	{
		if (SnowfallComponent && bUltraSnowfallOn)
		{
			SnowfallComponent->Deactivate(); // the flakes in the air still fall; the instanced snow takes over
		}
		bUltraSnowfallOn = false;
		return;
	}
	EnsureNiagara();
	UNiagaraSystem* System = NiagaraSystems.IsValidIndex(LayerSnowfall) ? NiagaraSystems[LayerSnowfall].Get() : nullptr;
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (!System || !CameraManager)
	{
		return;
	}
	const FVector Camera = CameraManager->GetCameraLocation();
	const float Distance = FMath::Max(1500.f, static_cast<float>(Camera.Y)); // the camera sits at +Y, looking at y = 0
	if (!SnowfallComponent)
	{
		SnowfallComponent = NewObject<UNiagaraComponent>(this, TEXT("UltraSnowfall"), RF_Transient);
		SnowfallComponent->SetupAttachment(Root);
		SnowfallComponent->SetUsingAbsoluteLocation(true);
		SnowfallComponent->SetUsingAbsoluteRotation(true);
		SnowfallComponent->SetAutoActivate(false);
		SnowfallComponent->SetCastShadow(false);
		SnowfallComponent->SetAsset(System);
		SnowfallComponent->RegisterComponent();
	}
	// Around what the camera sees (the flakes themselves stay where they are in the world when it moves).
	SnowfallComponent->SetWorldLocation(FVector(Camera.X, 100.f, Camera.Z));
	SnowfallComponent->SetVariableFloat(TEXT("HalfWidth"), Distance * 0.42f * 1.3f);
	SnowfallComponent->SetVariableFloat(TEXT("HalfHeight"), Distance * 0.26f + 250.f);
	SnowfallComponent->SetVariableFloat(TEXT("Depth"), Distance);
	if (!bUltraSnowfallOn)
	{
		// A fresh start (also over one still finishing): the system warms itself up, so the air is full of snow at once.
		bUltraSnowfallOn = true;
		SnowfallComponent->Activate(true);
		UE_LOG(LogPingu, Display, TEXT("[FX] Ultra snowfall on (view %.0f x %.0f cm)"), Distance * 0.42f * 2.6f, (Distance * 0.26f + 250.f) * 2.f);
	}
}

int32 APinguFX::CountUltraSnowflakes(FString& OutDetail) const
{
	OutDetail.Reset();
	if (!SnowfallComponent)
	{
		OutDetail = TEXT("not created");
		return 0;
	}
	OutDetail = FString::Printf(TEXT("on %d, active %d, state %d, at %s"), bUltraSnowfallOn ? 1 : 0, SnowfallComponent->IsActive() ? 1 : 0,
		static_cast<int32>(SnowfallComponent->GetExecutionState()), *SnowfallComponent->GetComponentLocation().ToCompactString());
	int32 Total = 0;
	const FNiagaraSystemInstanceControllerConstPtr Controller = SnowfallComponent->GetSystemInstanceController();
	if (const FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr)
	{
		for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
		{
			const int32 Count = Emitter->GetNumParticles();
			Total += Count;
			OutDetail += FString::Printf(TEXT(", %s %d"), *Emitter->GetEmitterHandle().GetName().ToString(), Count);
		}
	}
	else
	{
		OutDetail += TEXT(", no system instance");
	}
	return Total;
}

// ---------------------------------------------------------------------------- blast-zone flashes (every setting)

void APinguFX::PlayZoneFlash(const FVector& Location, float Radius)
{
	using namespace PinguFXPrivate;
	if (Radius <= 1.f)
	{
		return;
	}
	if (!ZoneMaterial)
	{
		ZoneMaterial = LoadObject<UMaterialInterface>(nullptr, ZoneMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!ZoneMaterial)
		{
			return; // built by the niagara / fx setup phase
		}
	}
	int32 Slot = INDEX_NONE;
	for (int32 Index = 0; Index < Zones.Num(); ++Index)
	{
		if (!Zones[Index].bActive)
		{
			Slot = Index;
			break;
		}
	}
	if (Slot == INDEX_NONE && Zones.Num() < MaxZones)
	{
		UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		if (!Plane)
		{
			return;
		}
		UStaticMeshComponent* Disc = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		Disc->SetupAttachment(Root);
		Disc->SetUsingAbsoluteLocation(true);
		Disc->SetUsingAbsoluteRotation(true);
		Disc->SetUsingAbsoluteScale(true);
		Disc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Disc->SetGenerateOverlapEvents(false);
		Disc->SetCanEverAffectNavigation(false);
		Disc->SetCastShadow(false);
		Disc->bAffectDistanceFieldLighting = false;
		Disc->bAffectDynamicIndirectLighting = false;
		Disc->bVisibleInRayTracing = false;
		Disc->bVisibleInReflectionCaptures = false;
		Disc->bVisibleInRealTimeSkyCaptures = false;
		Disc->SetStaticMesh(Plane);
		UMaterialInstanceDynamic* DiscMaterial = UMaterialInstanceDynamic::Create(ZoneMaterial, this);
		Disc->SetMaterial(0, DiscMaterial);
		Disc->SetTranslucentSortPriority(3);
		Disc->SetVisibility(false);
		Disc->RegisterComponent();
		ZoneMeshes.Add(Disc);
		ZoneMaterials.Add(DiscMaterial);
		Zones.AddDefaulted();
		Slot = Zones.Num() - 1;
	}
	if (Slot == INDEX_NONE)
	{
		// All showing: the oldest makes room.
		Slot = 0;
		for (int32 Index = 1; Index < Zones.Num(); ++Index)
		{
			if (Zones[Index].Age > Zones[Slot].Age)
			{
				Slot = Index;
			}
		}
	}
	if (!ZoneMeshes.IsValidIndex(Slot) || !ZoneMeshes[Slot] || !ZoneMaterials.IsValidIndex(Slot) || !ZoneMaterials[Slot])
	{
		return;
	}
	FZoneFlash& Zone = Zones[Slot];
	Zone.Location = FVector(Location.X, 0.f, Location.Z);
	Zone.Radius = Radius;
	Zone.Age = 0.f;
	Zone.Life = ZoneFlashLife;
	Zone.bActive = true;
	UMaterialInstanceDynamic* DiscMaterial = ZoneMaterials[Slot];
	DiscMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.8f, 0.45f));
	DiscMaterial->SetVectorParameterValue(TEXT("CoreColor"), FLinearColor(1.f, 0.4f, 0.18f));
	DiscMaterial->SetScalarParameterValue(TEXT("Progress"), 1.f); // the whole ring burnt
	DiscMaterial->SetScalarParameterValue(TEXT("Pulse"), 1.f);
	DiscMaterial->SetScalarParameterValue(TEXT("Inner"), ZoneInner);
	DiscMaterial->SetScalarParameterValue(TEXT("RingWidth"), FMath::Clamp(12.f / Radius, 0.01f, 0.1f));
	ZoneMeshes[Slot]->SetVisibility(true);
	UpdateZones(0.f);
}

void APinguFX::UpdateZones(float Dt)
{
	for (int32 Index = 0; Index < Zones.Num(); ++Index)
	{
		FZoneFlash& Zone = Zones[Index];
		if (!Zone.bActive || !ZoneMeshes.IsValidIndex(Index) || !ZoneMeshes[Index])
		{
			continue;
		}
		Zone.Age += Dt;
		const float T = Zone.Age / FMath::Max(Zone.Life, 0.01f);
		if (T >= 1.f)
		{
			Zone.bActive = false;
			ZoneMeshes[Index]->SetVisibility(false);
			continue;
		}
		// Snaps out to the radius within a few hundredths of a second, then fades where it is.
		const float Grow = 0.88f + 0.12f * FMath::Min(1.f, Zone.Age / 0.06f);
		const float Diameter = 2.f * Zone.Radius * Grow / 100.f; // the engine plane is 100 cm across
		ZoneMeshes[Index]->SetWorldLocationAndRotation(Zone.Location, FRotator(0.f, 0.f, 90.f)); // facing the camera (+Y)
		ZoneMeshes[Index]->SetWorldScale3D(FVector(Diameter, Diameter, 1.f));
		if (ZoneMaterials.IsValidIndex(Index) && ZoneMaterials[Index])
		{
			ZoneMaterials[Index]->SetScalarParameterValue(TEXT("Opacity"), 1.3f * FMath::Square(1.f - T));
		}
	}
}

// ---------------------------------------------------------------------------- the snow bomb's ash pile

void APinguFX::SetAshPile(const UObject* PileOwner, const FPinguAshPile& Pile)
{
	APinguFX* FX = PileOwner ? Get(PileOwner) : nullptr;
	if (!FX)
	{
		return;
	}
	const int32 Slot = FX->FindAshPile(PileOwner, true);
	if (Slot != INDEX_NONE)
	{
		FX->PlaceAshPile(Slot, Pile);
	}
}

void APinguFX::HideAshPile(const UObject* PileOwner)
{
	APinguFX* FX = PileOwner ? Get(PileOwner) : nullptr;
	if (!FX)
	{
		return;
	}
	const int32 Slot = FX->FindAshPile(PileOwner, false);
	if (Slot != INDEX_NONE)
	{
		FX->ReleaseAshPile(Slot);
	}
}

bool APinguFX::IsAshPileShown(const UObject* PileOwner)
{
	using namespace PinguFXPrivate;
	APinguFX* FX = PileOwner ? Get(PileOwner) : nullptr;
	const int32 Slot = FX ? FX->FindAshPile(PileOwner, false) : INDEX_NONE;
	if (Slot == INDEX_NONE)
	{
		return false;
	}
	for (int32 Part = 0; Part < NumAshParts; ++Part)
	{
		const int32 Index = Slot * NumAshParts + Part;
		if (FX->AshPileParts.IsValidIndex(Index) && FX->AshPileParts[Index] && FX->AshPileParts[Index]->IsVisible())
		{
			return true;
		}
	}
	return false;
}

void APinguFX::CountAshPileParts(const UObject* PileOwner, int32& OutHeap, int32& OutEyes)
{
	using namespace PinguFXPrivate;
	OutHeap = 0;
	OutEyes = 0;
	APinguFX* FX = PileOwner ? Get(PileOwner) : nullptr;
	const int32 Slot = FX ? FX->FindAshPile(PileOwner, false) : INDEX_NONE;
	if (Slot == INDEX_NONE)
	{
		return;
	}
	for (int32 Part = 0; Part < NumAshParts; ++Part)
	{
		const int32 Index = Slot * NumAshParts + Part;
		if (FX->AshPileParts.IsValidIndex(Index) && FX->AshPileParts[Index] && FX->AshPileParts[Index]->IsVisible())
		{
			OutHeap += Part < NumAshHeapParts ? 1 : 0;
			OutEyes += Part == AshEyeLeft || Part == AshEyeRight ? 1 : 0;
		}
	}
}

bool APinguFX::GetAshPileEyeSpots(const UObject* PileOwner, FVector& OutLeft, FVector& OutRight, float& OutRadius)
{
	using namespace PinguFXPrivate;
	APinguFX* FX = PileOwner ? Get(PileOwner) : nullptr;
	const int32 Slot = FX ? FX->FindAshPile(PileOwner, false) : INDEX_NONE;
	if (Slot == INDEX_NONE)
	{
		return false;
	}
	const int32 Left = Slot * NumAshParts + AshEyeLeft;
	const int32 Right = Slot * NumAshParts + AshEyeRight;
	if (!FX->AshPileParts.IsValidIndex(Right) || !FX->AshPileParts[Left] || !FX->AshPileParts[Right]
		|| !FX->AshPileParts[Left]->IsVisible() || !FX->AshPileParts[Right]->IsVisible())
	{
		return false;
	}
	// The parts are fitted to boxes (FitBox): a white's middle is its bounds' middle, its radius across the screen its half width.
	OutLeft = FX->AshPileParts[Left]->Bounds.Origin;
	OutRight = FX->AshPileParts[Right]->Bounds.Origin;
	OutRadius = static_cast<float>(FX->AshSphereBounds.BoxExtent.X * FX->AshPileParts[Left]->GetComponentScale().X);
	return true;
}

FVector APinguFX::GetAshPileEyes(const FPinguAshPile& Pile)
{
	using namespace PinguFXPrivate;
	const FVector OnPile = Pile.Base + FVector(0.f, AshEyeDepth, AshEyeHeight) * Pile.Size + Pile.EyeShift;
	if (Pile.EyeOnPile < 1.f && Pile.FaceEyeRadius > 0.f)
	{
		return FMath::Lerp((Pile.FaceEyes[0] + Pile.FaceEyes[1]) * 0.5f, OnPile, FMath::Clamp(Pile.EyeOnPile, 0.f, 1.f));
	}
	return OnPile;
}

bool APinguFX::EnsureAshPileAssets()
{
	using namespace PinguFXPrivate;
	if (!bAshAssetsTried)
	{
		bAshAssetsTried = true;
		AshSphereMesh = LoadObject<UStaticMesh>(nullptr, AshSpherePath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		AshConeMesh = LoadObject<UStaticMesh>(nullptr, AshConePath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!AshConeMesh)
		{
			AshConeMesh = AshSphereMesh; // a rounder heap, still a heap
		}
		if (AshSphereMesh)
		{
			AshSphereBounds = AshSphereMesh->GetBounds();
		}
		if (AshConeMesh)
		{
			AshConeBounds = AshConeMesh->GetBounds();
		}
		UMaterialInterface* Prop = LoadObject<UMaterialInterface>(nullptr, AshMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Prop)
		{
			UE_LOG(LogPingu, Warning, TEXT("FX material %s missing (re-run the fx setup phase), the ash pile uses a basic one"), AshMaterialPath);
			Prop = LoadObject<UMaterialInterface>(nullptr, AshFallbackMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		}
		if (Prop)
		{
			for (int32 Look = 0; Look < NumAshLooks; ++Look)
			{
				UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Prop, this);
				Material->SetVectorParameterValue(TEXT("Color"), AshLookColors[Look]);
				Material->SetScalarParameterValue(TEXT("Emissive"), AshLookGlows[Look]);
				Material->SetScalarParameterValue(TEXT("Roughness"), AshLookRoughness[Look]);
				AshPileMaterials.Add(Material);
			}
		}
		// The soot over the heap (without M_Ghost the heap is only the lit near-black).
		if (UMaterialInterface* Soot = LoadObject<UMaterialInterface>(nullptr, AshSootPath, nullptr, LOAD_Quiet | LOAD_NoWarn))
		{
			for (const FLinearColor& SootTint : AshSootColors)
			{
				UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Soot, this);
				Material->SetVectorParameterValue(TEXT("Color"), SootTint);
				Material->SetScalarParameterValue(TEXT("Opacity"), AshSootOpacity);
				AshSootMaterials.Add(Material);
			}
		}
	}
	return AshSphereMesh && AshConeMesh && AshPileMaterials.Num() == NumAshLooks;
}

int32 APinguFX::FindAshPile(const UObject* PileOwner, bool bCreate)
{
	using namespace PinguFXPrivate;
	if (!PileOwner)
	{
		return INDEX_NONE;
	}
	int32 Free = INDEX_NONE;
	for (int32 Slot = 0; Slot < AshPiles.Num(); ++Slot)
	{
		if (AshPiles[Slot].bInUse && AshPiles[Slot].PileOwner.Get() == PileOwner)
		{
			return Slot;
		}
		if (!AshPiles[Slot].bInUse && Free == INDEX_NONE)
		{
			Free = Slot;
		}
	}
	if (!bCreate)
	{
		return INDEX_NONE;
	}
	if (Free == INDEX_NONE)
	{
		if (AshPiles.Num() >= MaxAshPiles || !EnsureAshPileAssets())
		{
			return INDEX_NONE;
		}
		// A new slot: its parts, made once and reused by every pile that gets this slot later. Props that light nothing:
		// out of Lumen, distance fields, ray tracing and the captures; only the heap's big parts cast a shadow (it stands
		// on the floor), the small lumps, the embers and the eyes do not.
		for (int32 Part = 0; Part < NumAshParts; ++Part)
		{
			const bool bCone = Part < NumAshHeapParts && AshShapes[Part].bCone;
			int32 Look = AshLookPupil;
			if (Part < NumAshHeapParts)
			{
				Look = AshShapes[Part].Look;
			}
			else if (Part < AshEyeLeft)
			{
				Look = AshLookEmber;
			}
			else if (Part < AshPupilLeft)
			{
				Look = AshLookWhite;
			}
			UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			Mesh->SetupAttachment(Root);
			Mesh->SetUsingAbsoluteLocation(true);
			Mesh->SetUsingAbsoluteRotation(true);
			Mesh->SetUsingAbsoluteScale(true);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetGenerateOverlapEvents(false);
			Mesh->SetCanEverAffectNavigation(false);
			Mesh->SetCastShadow(Part < NumAshShadowParts);
			Mesh->bAffectDistanceFieldLighting = false;
			Mesh->bAffectDynamicIndirectLighting = false;
			Mesh->bVisibleInRayTracing = false;
			Mesh->bVisibleInReflectionCaptures = false;
			Mesh->bVisibleInRealTimeSkyCaptures = false;
			Mesh->SetStaticMesh(bCone ? AshConeMesh.Get() : AshSphereMesh.Get());
			Mesh->SetMaterial(0, AshPileMaterials[Look]);
			if (Part < NumAshHeapParts && AshSootMaterials.Num() == 2)
			{
				Mesh->SetOverlayMaterial(AshSootMaterials[Look == AshLookCharcoal ? 1 : 0]);
			}
			Mesh->SetVisibility(false);
			Mesh->RegisterComponent();
			AshPileParts.Add(Mesh);
		}
		AshPiles.AddDefaulted();
		Free = AshPiles.Num() - 1;
	}
	AshPiles[Free].PileOwner = PileOwner;
	AshPiles[Free].bInUse = true;
	return Free;
}

void APinguFX::PlaceAshPile(int32 Slot, const FPinguAshPile& Pile)
{
	using namespace PinguFXPrivate;
	if (!AshPiles.IsValidIndex(Slot) || !AshPileParts.IsValidIndex(Slot * NumAshParts + NumAshParts - 1))
	{
		return;
	}
	const int32 First = Slot * NumAshParts;
	const float Size = FMath::Max(Pile.Size, 0.05f);
	const float Wind = Pile.Wind >= 0.f ? 1.f : -1.f;
	const float Grown = FMath::Clamp(Pile.Grown, 0.f, 1.f);
	const float Blown = FMath::Clamp(Pile.Blown, 0.f, 1.f);
	auto Show = [](UStaticMeshComponent* Mesh, bool bShow, const FTransform& Where)
	{
		if (!Mesh)
		{
			return;
		}
		if (bShow)
		{
			Mesh->SetWorldTransform(Where);
		}
		if (Mesh->IsVisible() != bShow)
		{
			Mesh->SetVisibility(bShow);
		}
	};

	// The heap: piling up from the floor (the spilled bits and the low lumps first, the top last; each part low and
	// narrow first), then blown away part by part - the upwind side and the top first, each part shrinking toward the
	// floor and sliding downwind while the heap leans and shears after the wind.
	const FQuat Lean(FVector::YAxisVector, FMath::DegreesToRadians(Wind * AshLean * Blown));
	for (int32 Part = 0; Part < NumAshHeapParts; ++Part)
	{
		const FAshShape& Shape = AshShapes[Part];
		const float Upwind = FMath::Clamp(-Wind * static_cast<float>(Shape.Centre.X) / 30.f, -1.f, 1.f);
		const float High = FMath::Clamp(static_cast<float>(Shape.Centre.Z) / AshGrowTop, 0.f, 1.f);
		const float GrowFrom = AshGrowStagger * High;
		const float PartGrown = FMath::SmoothStep(GrowFrom, GrowFrom + 1.f - AshGrowStagger, Grown);
		const float Spread = 0.5f + 0.5f * PartGrown;
		const float Start = FMath::Clamp(0.22f - 0.15f * Upwind - 0.2f * High, 0.f, 1.f - AshPartBlowShare);
		const float Gone = FMath::SmoothStep(Start, Start + AshPartBlowShare, Blown);
		const float Keep = PartGrown * (1.f - Gone);
		FVector Centre = Shape.Centre;
		Centre.Z *= PartGrown * (0.4f + 0.6f * (1.f - Gone));
		Centre.X += Wind * (Gone * AshPartDrift + Blown * AshShear * (static_cast<float>(Shape.Centre.Z) + 6.f));
		const FVector Half = Shape.Half * FVector(Spread * (1.f - Gone), Spread * (1.f - Gone), Keep);
		const bool bShow = Keep > AshMinShare;
		const FBoxSphereBounds& Bounds = Shape.bCone ? AshConeBounds : AshSphereBounds;
		Show(AshPileParts[First + Part], bShow, bShow ? FitBox(Bounds, Pile.Base + Centre * Size, Half * Size, Lean) : FTransform::Identity);
	}

	// The embers: little glowing bits on the fresh heap, dying down (gone with the first of the wind).
	const float Glow = FMath::Clamp(Pile.Embers, 0.f, 1.f) * FMath::SmoothStep(0.7f, 1.f, Grown) * (1.f - FMath::SmoothStep(0.f, 0.2f, Blown));
	for (int32 Ember = 0; Ember < NumAshEmbers; ++Ember)
	{
		const float EmberRadius = AshEmbers[Ember].Radius * Size * Glow;
		const bool bShow = Glow > 0.15f;
		Show(AshPileParts[First + AshFirstEmber + Ember], bShow,
			bShow ? FitBox(AshSphereBounds, Pile.Base + AshEmbers[Ember].Centre * Size, FVector(EmberRadius), FQuat::Identity) : FTransform::Identity);
	}

	// The eyes: round whites with black pupils on their fronts, looking about; a blink squeezes them to a line. On the
	// charred penguin they lie over its own eyes (FaceEyes, a little bigger: the soot blackens the penguin's own), then they
	// fall onto the pile.
	const float OnPile = FMath::Clamp(Pile.EyeOnPile, 0.f, 1.f);
	const bool bFromFace = OnPile < 1.f && Pile.FaceEyeRadius > 0.f;
	const float EyeRadius = (bFromFace ? FMath::Lerp(Pile.FaceEyeRadius, AshEyeRadius * Size, OnPile) : AshEyeRadius * Size) * FMath::Max(Pile.Eyes, 0.f);
	const bool bEyes = Pile.Eyes > AshMinShare;
	const float Open = FMath::Clamp(Pile.EyeOpen, AshShutShare, 1.f);
	const float Look = FMath::Clamp(Pile.Look, -1.f, 1.f);
	// Toward the camera (+Y), to the side as they look, a little down.
	const FVector Gaze = FVector(Look * AshPupilSide, 1.f, -AshPupilDown).GetSafeNormal();
	const FQuat GazeTurn = FQuat::FindBetweenNormals(FVector::YAxisVector, Gaze);
	const FVector Squeeze(1.f, 1.f, Open);
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float Sign = Side == 0 ? -1.f : 1.f;
		FVector Eye = Pile.Base + FVector(Sign * AshEyeApart, AshEyeDepth, AshEyeHeight) * Size + Pile.EyeShift;
		if (bFromFace)
		{
			Eye = FMath::Lerp(Pile.FaceEyes[Side], Eye, OnPile);
		}
		const FVector Pupil = Eye + Gaze * (AshPupilOut * EyeRadius) * Squeeze;
		Show(AshPileParts[First + AshEyeLeft + Side], bEyes, bEyes ? FitBox(AshSphereBounds, Eye, FVector(EyeRadius) * Squeeze, FQuat::Identity) : FTransform::Identity);
		Show(AshPileParts[First + AshPupilLeft + Side], bEyes, bEyes ? FitBox(AshSphereBounds, Pupil, AshPupilHalf * EyeRadius * Squeeze, GazeTurn) : FTransform::Identity);
	}
}

void APinguFX::ReleaseAshPile(int32 Slot)
{
	using namespace PinguFXPrivate;
	if (!AshPiles.IsValidIndex(Slot))
	{
		return;
	}
	AshPiles[Slot].bInUse = false;
	AshPiles[Slot].PileOwner.Reset();
	for (int32 Part = 0; Part < NumAshParts; ++Part)
	{
		const int32 Index = Slot * NumAshParts + Part;
		if (AshPileParts.IsValidIndex(Index) && AshPileParts[Index] && AshPileParts[Index]->IsVisible())
		{
			AshPileParts[Index]->SetVisibility(false);
		}
	}
}

void APinguFX::UpdateAshPiles()
{
	for (int32 Slot = 0; Slot < AshPiles.Num(); ++Slot)
	{
		if (!AshPiles[Slot].bInUse)
		{
			continue;
		}
		const UObject* PileOwner = AshPiles[Slot].PileOwner.Get();
		const AActor* OwnerActor = Cast<AActor>(PileOwner);
		if (!PileOwner || (OwnerActor && OwnerActor->IsActorBeingDestroyed()))
		{
			ReleaseAshPile(Slot);
		}
	}
}

// ---------------------------------------------------------------------------- particles

void APinguFX::Spawn(const FPinguParticleDesc& Desc)
{
	if (FPinguFXBroadcastScope::IsBroadcasting(GetWorld()))
	{
		if (APinguFXRelay* ToClients = GetRelay())
		{
			ToClients->QueueParticle(Desc);
		}
	}
	SpawnInternal(Desc, false);
}

void APinguFX::SpawnInternal(const FPinguParticleDesc& Desc, bool bSnow)
{
	EnsureAssets();
	const int32 PoolIndex = static_cast<int32>(Desc.Mesh);
	if (!PoolStates.IsValidIndex(PoolIndex) || PoolStates[PoolIndex].Size <= 0)
	{
		return;
	}
	FPool& State = PoolStates[PoolIndex];
	int32 Slot = INDEX_NONE;
	if (State.Live.Num() >= State.Size)
	{
		// Pool is full: recycle the oldest particle of this mesh (in place, so it keeps its instance).
		float OldestAge = -1.f;
		for (int32 Index = 0; Index < State.Live.Num(); ++Index)
		{
			const float Age = State.Live[Index].Age / FMath::Max(State.Live[Index].Desc.Life, 0.01f);
			if (Age > OldestAge)
			{
				OldestAge = Age;
				Slot = Index;
			}
		}
		if (Slot == INDEX_NONE)
		{
			return;
		}
		if (State.Live[Slot].bSnow)
		{
			--SnowCount;
		}
		State.Live[Slot] = FParticle();
	}
	else
	{
		Slot = State.Live.AddDefaulted();
	}
	FParticle& P = State.Live[Slot];
	P.Desc = Desc;
	P.Location = Desc.Location;
	P.Velocity = Desc.Velocity;
	P.Rotation = Desc.Rotation;
	P.Phase = Rand.FRandRange(0.f, 2.f * UE_PI);
	P.bSnow = bSnow;
	P.bDataDirty = true;
	if (bSnow)
	{
		++SnowCount;
	}
}

void APinguFX::SimulateParticles(float Dt)
{
	using namespace PinguFXPrivate;
	float Data[NumCustomData];
	const FTransform Parked(FRotator::ZeroRotator, HiddenLocation, FVector(0.001f));

	for (int32 PoolIndex = 0; PoolIndex < PoolStates.Num() && PoolIndex < Pools.Num(); ++PoolIndex)
	{
		FPool& State = PoolStates[PoolIndex];
		UInstancedStaticMeshComponent* Pool = Pools[PoolIndex];
		if (!Pool || (State.Live.Num() == 0 && State.Shown == 0))
		{
			continue; // nothing to draw and nothing to hide: no work, no GPU update
		}

		// Retire the dead: the last particle moves into the gap, so the live ones always use the first instances.
		for (int32 Index = State.Live.Num() - 1; Index >= 0; --Index)
		{
			FParticle& P = State.Live[Index];
			P.Age += Dt;
			if (P.Age >= P.Desc.Life)
			{
				if (P.bSnow)
				{
					--SnowCount;
				}
				State.Live.RemoveAtSwap(Index, EAllowShrinking::No);
				if (Index < State.Live.Num())
				{
					State.Live[Index].bDataDirty = true; // drawn by another instance now
				}
			}
		}

		const int32 LiveCount = State.Live.Num();
		const int32 NumToUpdate = FMath::Min(FMath::Max(LiveCount, State.Shown), State.Size);
		TransformScratch.SetNum(NumToUpdate, EAllowShrinking::No);
		for (int32 Index = 0; Index < LiveCount; ++Index)
		{
			FParticle& P = State.Live[Index];
			const float T = FMath::Clamp(P.Age / FMath::Max(P.Desc.Life, 0.01f), 0.f, 1.f);

			P.Velocity.Z -= P.Desc.Gravity * Dt;
			if (P.Desc.Drag > 0.f)
			{
				P.Velocity *= FMath::Exp(-P.Desc.Drag * Dt);
			}
			FVector Move = P.Velocity * Dt;
			if (P.Desc.Sway > 0.f)
			{
				Move.X += FMath::Sin(P.Age * 1.7f + P.Phase) * P.Desc.Sway * Dt;
			}
			P.Location += Move;
			P.Rotation += FRotator(P.Desc.Spin.X, P.Desc.Spin.Y, P.Desc.Spin.Z) * Dt;

			float Size = P.Desc.Size;
			switch (P.Desc.Curve)
			{
			case EPinguSizeCurve::Linear:
				Size = FMath::Lerp(P.Desc.Size, P.Desc.EndSize, T);
				break;
			case EPinguSizeCurve::Pop:
				Size = T < 0.18f
					? FMath::Lerp(P.Desc.Size * 0.35f, P.Desc.Size, FMath::InterpEaseOut(0.f, 1.f, T / 0.18f, 2.f))
					: FMath::Lerp(P.Desc.Size, P.Desc.EndSize, FMath::InterpEaseIn(0.f, 1.f, (T - 0.18f) / 0.82f, 2.f));
				break;
			case EPinguSizeCurve::Grow:
				Size = P.Desc.Size * FMath::InterpEaseOut(0.f, 1.f, FMath::Min(T / 0.25f, 1.f), 2.5f);
				break;
			}
			const float Scale = FMath::Max(Size, 0.01f) / 100.f;
			// Stretched segments (glow trails): the curve thins the width, the length stays so the pieces keep touching.
			const FVector Scale3D = P.Desc.Length > 0.f ? FVector(Scale, Scale, P.Desc.Length / 100.f) : FVector(Scale);
			TransformScratch[Index] = FTransform(P.Rotation, P.Location, Scale3D);

			// Colour and emissive never change; only fading particles need their custom data every frame.
			if (P.bDataDirty || P.Desc.bFadeOut)
			{
				P.bDataDirty = false;
				Data[0] = P.Desc.Color.R;
				Data[1] = P.Desc.Color.G;
				Data[2] = P.Desc.Color.B;
				Data[3] = P.Desc.Emissive;
				Data[4] = P.Desc.bFadeOut ? P.Desc.Opacity * (1.f - T * T) : P.Desc.Opacity;
				Pool->SetCustomData(Index, MakeArrayView(Data, NumCustomData), false);
			}
		}
		// Instances that showed a particle last frame and have none now are parked far below.
		for (int32 Index = LiveCount; Index < NumToUpdate; ++Index)
		{
			TransformScratch[Index] = Parked;
		}
		State.Shown = LiveCount;
		if (NumToUpdate == 0)
		{
			continue;
		}
		// The effects actor sits at the origin, so world space is the pool's space (checked, in case it ever moves).
		const FTransform& PoolToWorld = Pool->GetComponentTransform();
		if (!PoolToWorld.Equals(FTransform::Identity))
		{
			for (int32 Index = 0; Index < NumToUpdate; ++Index)
			{
				TransformScratch[Index] = TransformScratch[Index].GetRelativeTransform(PoolToWorld);
			}
		}
		// One batched update per pool; only the changed instances go to the GPU (no render state rebuild).
		Pool->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		MarkInstancesChanged(Pool);
	}
}

void APinguFX::UpdateSnowfall(float Dt)
{
	if (!bSnowfall || bUltraSnowfallOn)
	{
		return; // Ultra: the Niagara snowfall instead
	}
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (!CameraManager)
	{
		return;
	}
	const FVector Camera = CameraManager->GetCameraLocation();
	const float Distance = FMath::Max(1500.f, static_cast<float>(Camera.Y)); // the camera sits at +Y, looking at the plane y = 0
	const float HalfWidth = Distance * 0.42f;
	const float HalfHeight = Distance * 0.26f;
	// Fewer flakes on lower effects settings (Low: about a third).
	const int32 MaxFlakes = Density >= 0.999f ? PinguFXPrivate::MaxSnow : FMath::Max(8, FMath::RoundToInt(PinguFXPrivate::MaxSnow * Density));

	SnowAccumulator += Dt * 28.f * Density;
	while (SnowAccumulator >= 1.f && SnowCount < MaxFlakes)
	{
		SnowAccumulator -= 1.f;
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Puff;
		const float Depth = Rand.FRandRange(-900.f, 1100.f);
		D.Location = FVector(Camera.X + Rand.FRandRange(-HalfWidth, HalfWidth) * 1.2f, Depth, Camera.Z + HalfHeight + Rand.FRandRange(0.f, 300.f));
		D.Velocity = FVector(Rand.FRandRange(-25.f, 25.f), 0.f, -Rand.FRandRange(70.f, 130.f));
		D.Size = Rand.FRandRange(4.5f, 8.5f);
		D.EndSize = D.Size;
		D.Curve = EPinguSizeCurve::Grow;
		D.Life = (2.f * HalfHeight + 600.f) / 100.f;
		D.Sway = Rand.FRandRange(25.f, 60.f);
		D.Emissive = 0.25f;
		D.Color = FLinearColor(0.95f, 0.97f, 1.f);
		SpawnInternal(D, true);
	}
	SnowAccumulator = FMath::Min(SnowAccumulator, 4.f);
}

int32 APinguFX::ScaleCount(int32 Count)
{
	if (Count <= 0)
	{
		return 0;
	}
	if (Density >= 0.999f)
	{
		return Count;
	}
	const float Wanted = Count * Density;
	int32 Result = FMath::FloorToInt(Wanted);
	if (Rand.FRand() < Wanted - Result)
	{
		++Result; // so even a single sparkle still shows up now and then
	}
	return Result;
}

// ---------------------------------------------------------------------------- presets (public: play here, maybe send)

void APinguFX::Emit(const UObject* Ctx, const FPinguFXEvent& Event)
{
	APinguFX* FX = Get(Ctx);
	if (!FX)
	{
		return;
	}
	if (FPinguFXBroadcastScope::IsBroadcasting(FX->GetWorld()))
	{
		if (APinguFXRelay* ToClients = FX->GetRelay())
		{
			ToClients->Queue(Event);
		}
	}
	FX->PlayEvent(Event);
}

void APinguFX::PlayEvent(const FPinguFXEvent& Event)
{
	switch (Event.Kind)
	{
	case EPinguFXEventKind::Puffs: PlayPuffs(Event.Location, Event.Count, Event.A, Event.B, Event.Direction, Event.C, Event.Color); break;
	case EPinguFXEventKind::GroundPuffs: PlayGroundPuffs(Event.Location, Event.A); break;
	case EPinguFXEventKind::Trail: PlayTrail(Event.Location, Event.A, Event.Color, Event.Direction); break;
	case EPinguFXEventKind::Shards: PlayShards(Event.Location, Event.Count, Event.A, Event.Color); break;
	case EPinguFXEventKind::Stars: PlayStars(Event.Location, Event.Count, Event.A); break;
	case EPinguFXEventKind::Sparkles: PlaySparkles(Event.Location, Event.Count, Event.A, Event.Color); break;
	case EPinguFXEventKind::Ring: PlayRing(Event.Location, Event.A, Event.Color, Event.B); break;
	case EPinguFXEventKind::Flakes: PlayFlakes(Event.Location, Event.Count, Event.A, Event.Color); break;
	case EPinguFXEventKind::Splash: PlaySplash(Event.Location); break;
	case EPinguFXEventKind::Explosion: PlayExplosion(Event.Location, Event.A); break;
	case EPinguFXEventKind::Ghost: PlayGhost(Event.Mesh, Event.Color, Event.A, Event.B); break;
	case EPinguFXEventKind::GlowTrail: PlayGlowTrail(Event.Location, Event.Direction, Event.A, Event.Color, Event.B, Event.C); break;
	case EPinguFXEventKind::Shake: PlayShake(Event.A); break;
	case EPinguFXEventKind::Sound: PlaySound(static_cast<EPinguSound>(Event.Count), Event.A, Event.B, Event.C); break;
	case EPinguFXEventKind::Spirit:
		PlaySpirit(Event.Mesh, Event.Location, Event.Direction.X, Event.Direction.Y, Event.Direction.Z, Event.C, Event.Color, Event.A, Event.B,
			Event.Count != 0);
		break;
	case EPinguFXEventKind::IceBurst: PlayIceBurst(Event.Location, Event.A, Event.Color); break;
	case EPinguFXEventKind::FizzleDust:
		PlayFizzleDust(Event.Location, Event.Direction, Event.Color, Event.A, static_cast<EPinguDustShape>(Event.Count));
		break;
	}
}

void APinguFX::Puffs(const UObject* Ctx, const FVector& Location, int32 Count, float Size, float Speed, const FVector& Direction, float Spread, const FLinearColor& Color)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Puffs;
	Event.Location = Location;
	Event.Count = PinguFXPrivate::ClampCount(Count);
	Event.A = Size;
	Event.B = Speed;
	Event.C = Spread;
	Event.Direction = Direction;
	Event.Color = Color;
	Emit(Ctx, Event);
}

void APinguFX::GroundPuffs(const UObject* Ctx, const FVector& FeetLocation, float Strength)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::GroundPuffs;
	Event.Location = FeetLocation;
	Event.A = Strength;
	Emit(Ctx, Event);
}

void APinguFX::Trail(const UObject* Ctx, const FVector& Location, float Size, const FLinearColor& Color, const FVector& Velocity)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Trail;
	Event.Location = Location;
	Event.A = Size;
	Event.Color = Color;
	Event.Direction = Velocity;
	Emit(Ctx, Event);
}

void APinguFX::Shards(const UObject* Ctx, const FVector& Location, int32 Count, float Speed, const FLinearColor& Color)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Shards;
	Event.Location = Location;
	Event.Count = PinguFXPrivate::ClampCount(Count);
	Event.A = Speed;
	Event.Color = Color;
	Emit(Ctx, Event);
}

void APinguFX::Stars(const UObject* Ctx, const FVector& Location, int32 Count, float Speed)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Stars;
	Event.Location = Location;
	Event.Count = PinguFXPrivate::ClampCount(Count);
	Event.A = Speed;
	Emit(Ctx, Event);
}

void APinguFX::Sparkles(const UObject* Ctx, const FVector& Location, int32 Count, float Radius, const FLinearColor& Color)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Sparkles;
	Event.Location = Location;
	Event.Count = PinguFXPrivate::ClampCount(Count);
	Event.A = Radius;
	Event.Color = Color;
	Emit(Ctx, Event);
}

void APinguFX::Ring(const UObject* Ctx, const FVector& Location, float Radius, const FLinearColor& Color, float Duration)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Ring;
	Event.Location = Location;
	Event.A = Radius;
	Event.B = Duration;
	Event.Color = Color;
	Emit(Ctx, Event);
}

void APinguFX::Flakes(const UObject* Ctx, const FVector& Location, int32 Count, float Speed, const FLinearColor& Color)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Flakes;
	Event.Location = Location;
	Event.Count = PinguFXPrivate::ClampCount(Count);
	Event.A = Speed;
	Event.Color = Color;
	Emit(Ctx, Event);
}

void APinguFX::IceBurst(const UObject* Ctx, const FVector& Location, float Size, const FLinearColor& Color)
{
	if (Size <= 0.f)
	{
		return;
	}
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::IceBurst;
	Event.Location = Location;
	Event.A = Size;
	Event.Color = Color;
	Emit(Ctx, Event);
}

void APinguFX::FizzleDust(const UObject* Ctx, const FVector& Location, const FVector& Direction, const FLinearColor& Color, float Size, EPinguDustShape Shape)
{
	if (Size <= 0.f)
	{
		return;
	}
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::FizzleDust;
	Event.Location = Location;
	Event.Direction = Direction;
	Event.Color = Color;
	Event.A = Size;
	Event.Count = static_cast<uint8>(Shape);
	Emit(Ctx, Event);
}

void APinguFX::Splash(const UObject* Ctx, const FVector& Location)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Splash;
	Event.Location = Location;
	Emit(Ctx, Event);
}

void APinguFX::Explosion(const UObject* Ctx, const FVector& Location, float Radius)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Explosion;
	Event.Location = Location;
	Event.A = Radius;
	Emit(Ctx, Event);
}

void APinguFX::Ghost(const UObject* Ctx, USkeletalMeshComponent* Source, const FLinearColor& Color, float Life, float Opacity)
{
	if (!Source)
	{
		return;
	}
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Ghost;
	Event.Mesh = Source;
	Event.Color = Color;
	Event.A = Life;
	Event.B = Opacity;
	Emit(Ctx, Event);
}

void APinguFX::Spirit(const UObject* Ctx, USkeletalMeshComponent* Source, const FVector& Origin, float Yaw, float Scale, float RiseSpeed,
	float Sway, const FLinearColor& Color, float Life, float Opacity, bool bRestPose)
{
	if (!Source || Scale <= 0.f || Life <= 0.f || Opacity <= 0.f)
	{
		return;
	}
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Spirit;
	Event.Mesh = Source;
	Event.Location = Origin;
	Event.Direction = FVector(Yaw, Scale, RiseSpeed); // (packed: a spirit has no direction of its own)
	Event.Color = Color;
	Event.A = Life;
	Event.B = Opacity;
	Event.C = Sway;
	Event.Count = bRestPose ? 1 : 0;
	Emit(Ctx, Event);
}

void APinguFX::GlowTrail(const UObject* Ctx, const FVector& From, const FVector& To, float Width, const FLinearColor& Color, float Opacity, float Life)
{
	if ((To - From).SizeSquared() < 1.f || Width <= 0.f || Opacity <= 0.f || Life <= 0.f)
	{
		return; // too short to see: not worth playing or sending
	}
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::GlowTrail;
	Event.Location = From;
	Event.Direction = To;
	Event.A = Width;
	Event.B = Opacity;
	Event.C = Life;
	Event.Color = Color;
	Emit(Ctx, Event);
}

void APinguFX::Shake(const UObject* Ctx, float Trauma)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Shake;
	Event.A = Trauma;
	Emit(Ctx, Event);
}

void APinguFX::Sound(const UObject* Ctx, EPinguSound Which, float Volume, float PitchVariance, float Pitch)
{
	FPinguFXEvent Event;
	Event.Kind = EPinguFXEventKind::Sound;
	Event.Count = static_cast<uint8>(Which);
	Event.A = Volume;
	Event.B = PitchVariance;
	Event.C = Pitch;
	Emit(Ctx, Event);
}

// ---------------------------------------------------------------------------- presets (local play)

void APinguFX::PlayPuffs(const FVector& Location, int32 Count, float Size, float Speed, const FVector& Direction, float Spread, const FLinearColor& Color)
{
	if (Count >= 3)
	{
		UltraBurst(Location, Color, Count * 3, 0, Count / 3, Count / 2, Speed * 1.2f, Size * 0.5f, Direction, Spread);
	}
	const int32 Num = ScaleCount(Count);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Puff;
		D.Location = Location + Rand.VRand() * Size * 0.3f;
		D.Velocity = PinguFXPrivate::RandomInCone(Rand, Direction, Spread) * Speed * Rand.FRandRange(0.5f, 1.1f);
		D.Size = Size * Rand.FRandRange(0.7f, 1.25f);
		D.Life = Rand.FRandRange(0.32f, 0.55f);
		D.Drag = 4.5f;
		D.Gravity = -40.f;
		D.Color = Color;
		D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), 0.f);
		SpawnInternal(D, false);
	}
}

void APinguFX::PlayGroundPuffs(const FVector& FeetLocation, float Strength)
{
	Strength = FMath::Clamp(Strength, 0.1f, 2.f);
	if (Strength >= 0.5f)
	{
		UltraBurst(FeetLocation + FVector(0.f, 0.f, 6.f), FLinearColor(0.95f, 0.97f, 1.f), FMath::RoundToInt(8.f + 12.f * Strength), 0, 0,
			FMath::RoundToInt(2.f * Strength), 320.f * Strength, 14.f, FVector::UpVector, 1.f);
	}
	const int32 PerSide = 2 + FMath::RoundToInt(3.f * Strength);
	for (const float Side : { -1.f, 1.f })
	{
		const int32 Num = ScaleCount(PerSide);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			FPinguParticleDesc D;
			D.Mesh = EPinguFXMesh::Puff;
			D.Location = FeetLocation + FVector(Side * Rand.FRandRange(5.f, 25.f), Rand.FRandRange(-25.f, 25.f), 6.f);
			D.Velocity = FVector(Side * Rand.FRandRange(180.f, 420.f) * Strength, Rand.FRandRange(-60.f, 60.f), Rand.FRandRange(20.f, 110.f) * Strength);
			D.Size = Rand.FRandRange(16.f, 30.f) * (0.65f + 0.5f * Strength);
			D.Life = Rand.FRandRange(0.3f, 0.5f);
			D.Drag = 6.f;
			D.Gravity = -30.f;
			D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), 0.f);
			SpawnInternal(D, false);
		}
	}
}

void APinguFX::FlapWind(const UObject* Ctx, const FVector& BodyLocation, float SizeScale, float GroundDistance, const FVector& GroundPoint, int32 Beat)
{
	if (APinguFX* FX = Get(Ctx))
	{
		FX->PlayFlapWind(BodyLocation, SizeScale, GroundDistance, GroundPoint, Beat);
	}
}

void APinguFX::PlayFlapWind(const FVector& BodyLocation, float SizeScale, float GroundDistance, const FVector& GroundPoint, int32 Beat)
{
	// Effects setting: Off none, Low every 4th beat (small), Medium every 2nd (light), High every beat, Ultra the same plus Niagara.
	const EPinguQualityLevel Level = UPinguSettings::Get()->GetGraphics().Effects;
	if (Level == EPinguQualityLevel::Off || (Level == EPinguQualityLevel::Low && ((Beat - 1) % 4) != 0) || (Level == EPinguQualityLevel::Medium && ((Beat - 1) % 2) != 0))
	{
		return;
	}
	const float Amount = Level == EPinguQualityLevel::Low ? 0.4f : Level == EPinguQualityLevel::Medium ? 0.7f : 1.f;
	const float S = FMath::Max(SizeScale, 0.2f);
	// Pale blue-white, translucent and unlit: only the Glow pool (M_Trail) is see-through (the Puff pool is opaque and read as
	// snowballs), and its emissive is rgb * (0.5 + Emissive): 0.55 gives the colour itself, not grey.
	const FLinearColor Air(0.86f, 0.95f, 1.f);
	constexpr float AirGlow = 0.55f;
	for (const float Side : { -1.f, 1.f })
	{
		// Airflow arcs: a curve from just outside the flipper sweeping down and out (the down-stroke's push), cut into pieces that
		// fade in and out along it (a bump), so both ends are faded. They stay beside the body, never over the face.
		const int32 Arcs = FMath::Max(1, FMath::RoundToInt(2.f * Amount));
		for (int32 Arc = 0; Arc < Arcs; ++Arc)
		{
			constexpr int32 Pieces = 5;
			const float Reach = Rand.FRandRange(0.8f, 1.2f);
			const float Drop = Rand.FRandRange(115.f, 165.f) * S;
			const float Start = Rand.FRandRange(42.f, 56.f) * S;
			const float YJitter = Rand.FRandRange(-10.f, 10.f);
			const float Top = Rand.FRandRange(-5.f, 25.f) * S;
			auto ArcPoint = [&](float T)
			{
				return BodyLocation + FVector(Side * (Start + 75.f * Reach * S * T * T), YJitter, Top - Drop * T);
			};
			for (int32 Piece = 0; Piece < Pieces; ++Piece)
			{
				const float T0 = static_cast<float>(Piece) / Pieces;
				const float T1 = static_cast<float>(Piece + 1) / Pieces;
				const FVector A = ArcPoint(T0);
				const FVector B = ArcPoint(T1);
				const FVector Delta = B - A;
				const float Len = static_cast<float>(Delta.Size());
				const FVector Dir = Delta / FMath::Max(Len, 0.01f);
				FPinguParticleDesc D;
				D.Mesh = EPinguFXMesh::Glow;
				D.Location = (A + B) * 0.5f;
				D.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Dir).Rotator();
				D.Length = Len;
				D.Size = FMath::Lerp(5.f, 11.f, FMath::Sin(PI * (T0 + T1) * 0.5f)) * S;
				D.EndSize = D.Size * 0.4f;
				D.Curve = EPinguSizeCurve::Linear;
				D.Life = Rand.FRandRange(0.26f, 0.34f);
				D.Opacity = 0.6f * FMath::Sin(PI * (T0 + T1) * 0.5f);
				D.bFadeOut = true;
				D.Emissive = AirGlow;
				D.Color = Air;
				D.Velocity = FVector(Side * 40.f, 0.f, -260.f);
				D.Drag = 3.f;
				SpawnInternal(D, false);
			}
		}
		// Mist curls: a few short soft strokes (translucent, growing, gone in a third of a second) pushed down and out.
		const int32 Curls = FMath::Max(1, FMath::RoundToInt(2.f * Amount));
		for (int32 Index = 0; Index < Curls; ++Index)
		{
			const FVector Dir = FVector(Side * Rand.FRandRange(0.3f, 0.9f), 0.f, -Rand.FRandRange(0.4f, 1.f)).GetSafeNormal();
			FPinguParticleDesc D;
			D.Mesh = EPinguFXMesh::Glow;
			D.Location = BodyLocation + FVector(Side * Rand.FRandRange(48.f, 66.f) * S, Rand.FRandRange(-10.f, 10.f), Rand.FRandRange(-45.f, -5.f) * S);
			D.Velocity = Dir * Rand.FRandRange(160.f, 300.f);
			D.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, FVector(Side * 0.5f, 0.f, -1.f).GetSafeNormal()).Rotator();
			D.Length = Rand.FRandRange(26.f, 42.f) * S;
			D.Size = Rand.FRandRange(9.f, 14.f) * S;
			D.EndSize = D.Size * 2.2f;
			D.Curve = EPinguSizeCurve::Linear;
			D.Life = Rand.FRandRange(0.28f, 0.4f);
			D.Opacity = 0.3f;
			D.bFadeOut = true;
			D.Emissive = AirGlow;
			D.Color = Air;
			D.Drag = 5.f;
			SpawnInternal(D, false);
		}
	}
	// Near the ground (about 2 m): a ring of snow dust blown outward along it.
	constexpr float Reach = 200.f;
	if (GroundDistance >= 0.f && GroundDistance <= Reach * S)
	{
		const float Near = 1.f - 0.6f * GroundDistance / (Reach * S);
		const FLinearColor Snow(0.95f, 0.97f, 1.f);
		// Dust sweeps: low airflow arcs along the snow, blown out from under the penguin (chained pieces that fade in and out, so
		// both ends are soft), a little snow-white and a touch brighter than the air above.
		for (const float Side : { -1.f, 1.f })
		{
			const int32 Sweeps = FMath::Max(1, FMath::RoundToInt(3.f * Near * Amount));
			for (int32 Sweep = 0; Sweep < Sweeps; ++Sweep)
			{
				constexpr int32 Pieces = 5;
				const float Out = Rand.FRandRange(110.f, 190.f) * S;
				const float Lift = Rand.FRandRange(14.f, 34.f) * S;
				const float YJitter = Rand.FRandRange(-25.f, 25.f);
				const float Base = Rand.FRandRange(6.f, 16.f);
				auto SweepPoint = [&](float T)
				{
					return GroundPoint + FVector(Side * (22.f * S + Out * T), YJitter, Base + Lift * FMath::Sin(PI * T));
				};
				for (int32 Piece = 0; Piece < Pieces; ++Piece)
				{
					const float T0 = static_cast<float>(Piece) / Pieces;
					const float T1 = static_cast<float>(Piece + 1) / Pieces;
					const FVector A = SweepPoint(T0);
					const FVector B = SweepPoint(T1);
					const FVector Delta = B - A;
					const float Len = static_cast<float>(Delta.Size());
					const float Bump = FMath::Sin(PI * (T0 + T1) * 0.5f);
					FPinguParticleDesc D;
					D.Mesh = EPinguFXMesh::Glow;
					D.Location = (A + B) * 0.5f;
					D.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Delta / FMath::Max(Len, 0.01f)).Rotator();
					D.Length = Len * 1.08f;
					D.Size = FMath::Lerp(6.f, 14.f, Bump) * S;
					D.EndSize = D.Size * 1.5f;
					D.Curve = EPinguSizeCurve::Linear;
					D.Life = Rand.FRandRange(0.35f, 0.5f);
					D.Opacity = 0.7f * Bump;
					D.bFadeOut = true;
					D.Emissive = 0.6f;
					D.Color = Snow;
					D.Velocity = FVector(Side * 160.f * Near, 0.f, 15.f);
					D.Drag = 3.f;
					SpawnInternal(D, false);
				}
			}
		}
	}
}

void APinguFX::PlayTrail(const FVector& Location, float Size, const FLinearColor& Color, const FVector& Velocity)
{
	// Called every frame by moving things: thinned out on lower effects settings, but never below half.
	if (Density < 0.999f && Rand.FRand() > FMath::Max(Density, 0.5f))
	{
		return;
	}
	FPinguParticleDesc D;
	D.Mesh = EPinguFXMesh::Puff;
	D.Location = Location + Rand.VRand() * Size * 0.25f;
	D.Velocity = Velocity + FVector(0.f, 0.f, Rand.FRandRange(10.f, 40.f));
	D.Size = Size * Rand.FRandRange(0.8f, 1.2f);
	D.Curve = EPinguSizeCurve::Linear;
	D.EndSize = 0.f;
	D.Life = Rand.FRandRange(0.25f, 0.4f);
	D.Drag = 3.f;
	D.Color = Color;
	SpawnInternal(D, false);
}

void APinguFX::PlayShards(const FVector& Location, int32 Count, float Speed, const FLinearColor& Color)
{
	if (Count >= 2)
	{
		UltraBurst(Location, Color, Count * 2, Count, Count / 2 + 2, 1, Speed, 14.f, FVector::UpVector, 1.f);
	}
	const int32 Num = ScaleCount(Count);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Shard;
		D.Location = Location;
		D.Velocity = PinguFXPrivate::RandomInCone(Rand, FVector::UpVector, 1.f) * Speed * Rand.FRandRange(0.4f, 1.f);
		D.Size = Rand.FRandRange(14.f, 30.f);
		D.EndSize = D.Size * 0.2f;
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = Rand.FRandRange(0.45f, 0.8f);
		D.Gravity = 1700.f;
		D.Drag = 0.8f;
		D.Color = Color;
		D.Emissive = 0.35f;
		D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f));
		D.Spin = FVector(Rand.FRandRange(-720.f, 720.f), Rand.FRandRange(-720.f, 720.f), Rand.FRandRange(-720.f, 720.f));
		SpawnInternal(D, false);
	}
}

void APinguFX::PlayStars(const FVector& Location, int32 Count, float Speed)
{
	if (Count >= 2)
	{
		UltraGlints(Location + FVector(0.f, 30.f, 0.f), FLinearColor(1.f, 0.8f, 0.25f), Count * 2, 30.f + Speed * 0.08f);
	}
	const int32 Num = ScaleCount(Count);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		const float Angle = (Index + Rand.FRand() * 0.6f) / FMath::Max(Num, 1) * 2.f * UE_PI;
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Star;
		D.Location = Location + FVector(0.f, 30.f, 0.f);
		D.Velocity = FVector(FMath::Cos(Angle), 0.f, FMath::Sin(Angle) * 0.8f + 0.35f) * Speed * Rand.FRandRange(0.7f, 1.1f);
		D.Size = Rand.FRandRange(20.f, 34.f);
		D.Life = Rand.FRandRange(0.45f, 0.7f);
		D.Gravity = 600.f;
		D.Drag = 2.5f;
		D.Emissive = 1.2f;
		D.Color = FLinearColor(1.f, 0.78f, 0.12f);
		D.Spin = FVector(0.f, 0.f, Rand.FRandRange(-540.f, 540.f));
		SpawnInternal(D, false);
	}
}

void APinguFX::PlaySparkles(const FVector& Location, int32 Count, float Radius, const FLinearColor& Color)
{
	if (Count >= 3)
	{
		UltraGlints(Location, Color, Count, Radius); // not the single sparkles of trails and fuses (every few frames)
	}
	const int32 Num = ScaleCount(Count);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		FPinguParticleDesc D;
		D.Mesh = Rand.FRand() < 0.5f ? EPinguFXMesh::Star : EPinguFXMesh::Flake;
		D.Location = Location + FVector(Rand.FRandRange(-Radius, Radius), Rand.FRandRange(10.f, 60.f), Rand.FRandRange(-Radius, Radius));
		D.Velocity = FVector(Rand.FRandRange(-40.f, 40.f), 0.f, Rand.FRandRange(30.f, 120.f));
		D.Size = Rand.FRandRange(8.f, 16.f);
		D.Life = Rand.FRandRange(0.35f, 0.7f);
		D.Drag = 2.f;
		D.Emissive = 2.f;
		D.Color = Color;
		D.Spin = FVector(0.f, 0.f, Rand.FRandRange(-360.f, 360.f));
		SpawnInternal(D, false);
	}
}

void APinguFX::PlayRing(const FVector& Location, float Radius, const FLinearColor& Color, float Duration)
{
	UltraRing(Location + FVector(0.f, 40.f, 0.f), Color, Radius, Duration);
	FPinguParticleDesc D;
	D.Mesh = EPinguFXMesh::Ring;
	D.Location = Location + FVector(0.f, 40.f, 0.f);
	D.Size = Radius * 0.35f;
	D.EndSize = Radius * 2.f;
	D.Curve = EPinguSizeCurve::Linear;
	D.Life = Duration;
	D.bFadeOut = true;
	D.Emissive = 0.8f;
	D.Color = Color;
	SpawnInternal(D, false);
}

void APinguFX::PlayFlakes(const FVector& Location, int32 Count, float Speed, const FLinearColor& Color)
{
	if (Count >= 3)
	{
		UltraBurst(Location, Color, Count * 2, 0, Count / 2, 0, Speed, 10.f, FVector::UpVector, 1.f);
	}
	const int32 Num = ScaleCount(Count);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Flake;
		D.Location = Location + Rand.VRand() * 20.f;
		D.Velocity = PinguFXPrivate::RandomInCone(Rand, FVector::UpVector, 1.f) * Speed * Rand.FRandRange(0.3f, 1.f);
		D.Size = Rand.FRandRange(12.f, 24.f);
		D.Life = Rand.FRandRange(0.5f, 0.9f);
		D.Drag = 3.f;
		D.Gravity = 150.f;
		D.Emissive = 0.6f;
		D.Color = Color;
		D.Rotation = FRotator(0.f, 0.f, Rand.FRandRange(0.f, 360.f));
		D.Spin = FVector(0.f, 0.f, Rand.FRandRange(-400.f, 400.f));
		SpawnInternal(D, false);
	}
}

void APinguFX::PlayIceBurst(const FVector& Location, float Size, const FLinearColor& Color)
{
	using namespace PinguFXPrivate;
	const float S = FMath::Clamp(Size, 0.2f, 4.f);
	UltraBurst(Location, Color, 40, 24, 12, 4, IceBurstUltraSpeed * S, 18.f * S, FVector::UpVector, 1.f);
	++UltraNesting; // the burst layer has its own chunks and glints: the ring and puffs below bring none
	// A white flash where the block was (in front of the penguin).
	FPinguParticleDesc Flash;
	Flash.Mesh = EPinguFXMesh::Puff;
	Flash.Location = Location + FVector(0.f, 30.f * S, 0.f);
	Flash.Size = 80.f * S;
	Flash.Life = 0.08f;
	Flash.Emissive = 2.f;
	Flash.Color = FMath::Lerp(Color, FLinearColor::White, 0.6f);
	SpawnInternal(Flash, false);
	// The block's pieces: big fat chunks of clear blue ice starting all over where the block was, in front of the penguin
	// (not hidden inside it), flying out on the play plane away from the middle and a bit up, slowing, tumbling as they
	// fall. Deep ice blue with a few paler ones, little glow (a glow washes them out to white in the sun); spread round
	// the circle evenly with some jitter so even the few chunks of the lowest setting fly out on every side.
	const FLinearColor Pale = FMath::Lerp(Color, FLinearColor::White, 0.3f);
	const FLinearColor Deep(Color.R * 0.35f, Color.G * 0.62f, Color.B);
	const int32 NumChunks = FMath::Max(ScaleCount(IceBurstChunks), IceBurstMinChunks);
	const float FirstAngle = Rand.FRandRange(0.f, 2.f * UE_PI);
	for (int32 Index = 0; Index < NumChunks; ++Index)
	{
		const float Angle = FirstAngle + (Index + Rand.FRandRange(-0.3f, 0.3f)) * 2.f * UE_PI / NumChunks;
		const float Reach = Rand.FRandRange(0.35f, 1.f);
		const FVector Start(FMath::Cos(Angle) * IceBurstHalfWidth * Reach * S, Rand.FRandRange(IceBurstNearDepth, IceBurstFarDepth) * S,
			FMath::Sin(Angle) * IceBurstHalfHeight * Reach * S);
		FVector Out(FMath::Cos(Angle), 0.f, FMath::Sin(Angle) + IceBurstLift);
		Out.Y = Rand.FRandRange(0.05f, 0.25f);
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Shard;
		D.Location = Location + Start;
		D.Velocity = Out.GetSafeNormal() * Rand.FRandRange(IceBurstMinSpeed, IceBurstMaxSpeed) * S;
		D.Size = Rand.FRandRange(IceChunkMinSize, IceChunkMaxSize) * S;
		D.EndSize = D.Size * IceChunkEndShare;
		D.Length = D.Size * IceChunkThick; // (Length scales the mesh's Z: the chunk's thickness, see the constants)
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = Rand.FRandRange(0.95f, 1.25f);
		D.Gravity = IceBurstGravity;
		D.Drag = IceBurstDrag;
		D.Color = Index % 4 == 3 ? Pale : Deep;
		D.Emissive = 0.12f;
		D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f));
		D.Spin = FVector(Rand.FRandRange(-420.f, 420.f), Rand.FRandRange(-420.f, 420.f), Rand.FRandRange(-420.f, 420.f)); // heavy: slower
		SpawnInternal(D, false);
	}
	// Small glittering shards, faster, all round (in front of the penguin as well).
	const int32 NumBits = ScaleCount(IceBurstShards);
	for (int32 Index = 0; Index < NumBits; ++Index)
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Shard;
		D.Location = Location + FVector(0.f, 50.f * S, 0.f) + Rand.VRand() * 25.f * S;
		D.Velocity = RandomInCone(Rand, FVector::UpVector, 1.f) * Rand.FRandRange(650.f, 1350.f) * S;
		D.Size = Rand.FRandRange(10.f, 20.f) * S;
		D.EndSize = D.Size * 0.2f;
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = Rand.FRandRange(0.4f, 0.7f);
		D.Gravity = 1700.f;
		D.Drag = 0.8f;
		D.Color = Pale;
		D.Emissive = 0.9f;
		D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f));
		D.Spin = FVector(Rand.FRandRange(-720.f, 720.f), Rand.FRandRange(-720.f, 720.f), Rand.FRandRange(-720.f, 720.f));
		SpawnInternal(D, false);
	}
	// A little frost, a small quick ring and glints (kept small: the chunks are the burst).
	PlayPuffs(Location + FVector(0.f, 20.f * S, 0.f), 3, 26.f * S, 260.f * S, FVector::UpVector, 1.f, Pale);
	PlayRing(Location, 60.f * S, Color, 0.16f);
	PlaySparkles(Location, 6, 60.f * S, Pale);
	--UltraNesting;
}

void APinguFX::PlayFizzleDust(const FVector& Location, const FVector& Direction, const FLinearColor& Color, float Size, EPinguDustShape Shape)
{
	++FizzleDustEvents;
	const float S = FMath::Clamp(Size, 0.3f, 4.f);
	FVector Dir = Direction;
	Dir.Y = 0.f;
	Dir = Dir.IsNearlyZero() ? FVector(1.f, 0.f, 0.f) : Dir.GetSafeNormal();
	const FVector Perp(-Dir.Z, 0.f, Dir.X);
	const FLinearColor Bright = FMath::Lerp(Color, FLinearColor::White, 0.45f);
	const FLinearColor Glitter[3] = { FMath::Lerp(Color, FLinearColor::White, 0.7f), FLinearColor::White, Bright };
	// In front of the penguin (the camera looks along -Y at the play plane): the dust is never hidden in its body.
	const FVector Front(0.f, 30.f, 0.f);
	const auto Make = [this](const FPinguParticleDesc& Desc)
	{
		++FizzleDustParticles;
		SpawnInternal(Desc, false);
	};

	// ---- 1. The ability for a moment: a flash in its shape (it pops up and shrinks away in about a fifth of a second).
	switch (Shape)
	{
	case EPinguDustShape::Ball:
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Puff;
		D.Location = Location + Front;
		D.Size = 34.f * S;
		D.EndSize = 6.f * S;
		D.Curve = EPinguSizeCurve::Pop;
		D.Life = 0.24f;
		D.Emissive = 1.2f;
		D.Color = Bright;
		Make(D);
		D.Size = 18.f * S; // a white heart
		D.EndSize = 2.f * S;
		D.Life = 0.18f;
		D.Emissive = 2.4f;
		D.Color = FLinearColor::White;
		Make(D);
		break;
	}
	case EPinguDustShape::Spike:
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Glow;
		D.Location = Location + Dir * 38.f * S + Front;
		D.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Dir).Rotator();
		D.Length = 76.f * S;
		D.Size = 15.f * S;
		D.EndSize = 3.f * S;
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = 0.26f;
		D.Opacity = 0.95f;
		D.bFadeOut = true;
		D.Emissive = 2.4f;
		D.Color = Bright;
		Make(D);
		D.Size = 6.f * S; // the bright needle inside it
		D.EndSize = 1.f * S;
		D.Length = 70.f * S;
		D.Emissive = 3.f;
		D.Color = FLinearColor::White;
		Make(D);
		break;
	}
	case EPinguDustShape::Ring:
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Ring;
		D.Location = Location + FVector(0.f, 40.f, 0.f);
		D.Size = 112.f * S;
		D.EndSize = 76.f * S;
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = 0.28f;
		D.bFadeOut = true;
		D.Emissive = 1.2f;
		D.Color = Bright;
		Make(D);
		break;
	}
	case EPinguDustShape::Cone:
	{
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FPinguParticleDesc D;
			D.Mesh = EPinguFXMesh::Puff;
			D.Location = Location + Dir * (22.f + 30.f * Index) * S + Front;
			D.Size = (26.f + 14.f * Index) * S;
			D.EndSize = 4.f * S;
			D.Curve = EPinguSizeCurve::Pop;
			D.Life = 0.22f + 0.02f * Index;
			D.Emissive = 0.9f;
			D.Color = Bright;
			Make(D);
		}
		break;
	}
	case EPinguDustShape::Block:
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Shard;
		D.Location = Location + Front;
		D.Size = 38.f * S;
		D.EndSize = 6.f * S;
		D.Length = 32.f * S;
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = 0.24f;
		D.Emissive = 0.9f;
		D.Color = Bright;
		D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f));
		D.Spin = FVector(120.f, 160.f, 90.f);
		Make(D);
		break;
	}
	}

	// A random spot inside the shape the ability had (the dust starts where the flash was, so it reads as the shape crumbling).
	const auto PointInShape = [&]() -> FVector
	{
		switch (Shape)
		{
		case EPinguDustShape::Spike:
			return Dir * Rand.FRandRange(0.f, 76.f) * S + Perp * Rand.FRandRange(-5.f, 5.f) * S;
		case EPinguDustShape::Ring:
		{
			const float Angle = Rand.FRandRange(0.f, 2.f * UE_PI);
			return FVector(FMath::Cos(Angle), 0.f, FMath::Sin(Angle)) * 52.f * S * Rand.FRandRange(0.8f, 1.05f);
		}
		case EPinguDustShape::Cone:
		{
			const float Along = Rand.FRandRange(8.f, 100.f) * S;
			return Dir * Along + Perp * Along * Rand.FRandRange(-0.4f, 0.4f);
		}
		case EPinguDustShape::Block:
			return FVector(Rand.FRandRange(-20.f, 20.f), 0.f, Rand.FRandRange(-20.f, 20.f)) * S;
		case EPinguDustShape::Ball:
		default:
		{
			FVector Spot = Rand.VRand() * 17.f * S * FMath::Pow(Rand.FRand(), 0.33f);
			Spot.Y = 0.f;
			return Spot;
		}
		}
	};

	// ---- 2. The glitter: snowflakes and soft motes that start inside the shape, hang there a moment (gravity has hardly begun)
	// and drift down at different speeds, twinkling out over about a second.
	const int32 NumGlitter = FMath::Max(ScaleCount(26), 5);
	for (int32 Index = 0; Index < NumGlitter; ++Index)
	{
		const FVector Spot = PointInShape();
		FPinguParticleDesc D;
		// Snowflakes and soft motes, never the gold stars (those are the double's: a fizzle must not look like a win).
		const bool bMote = Index % 3 == 2;
		D.Mesh = bMote ? EPinguFXMesh::Puff : EPinguFXMesh::Flake;
		D.Location = Location + Spot + FVector(0.f, Rand.FRandRange(10.f, 55.f), 0.f);
		D.Velocity = FVector(Rand.FRandRange(-35.f, 35.f), 0.f, Rand.FRandRange(-10.f, 55.f)) + Spot.GetSafeNormal() * Rand.FRandRange(0.f, 40.f);
		D.Size = (bMote ? Rand.FRandRange(4.f, 9.f) : Rand.FRandRange(9.f, 17.f)) * FMath::Sqrt(S);
		D.EndSize = D.Size * 0.35f;
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = Rand.FRandRange(0.8f, 1.0f);
		D.Gravity = Rand.FRandRange(160.f, 520.f);
		D.Drag = 0.8f;
		D.Emissive = bMote ? 1.6f : 2.f;
		D.bFadeOut = true;
		D.Color = Glitter[(Index + Index / 3) % 3];
		D.Rotation = FRotator(0.f, 0.f, Rand.FRandRange(0.f, 360.f));
		D.Spin = FVector(0.f, 0.f, Rand.FRandRange(-420.f, 420.f));
		Make(D);
	}
	// ---- 3. The chips: a few bigger pieces in the ability's own colour that break off and drop faster.
	const int32 NumChips = FMath::Max(ScaleCount(7), 2);
	for (int32 Index = 0; Index < NumChips; ++Index)
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Shard;
		D.Location = Location + PointInShape() + FVector(0.f, Rand.FRandRange(10.f, 45.f), 0.f);
		D.Velocity = FVector(Rand.FRandRange(-30.f, 30.f), 0.f, Rand.FRandRange(0.f, 40.f));
		D.Size = Rand.FRandRange(12.f, 20.f) * FMath::Sqrt(S);
		D.EndSize = D.Size * 0.35f;
		D.Curve = EPinguSizeCurve::Linear;
		D.Life = Rand.FRandRange(0.5f, 0.7f);
		D.Gravity = Rand.FRandRange(650.f, 1000.f);
		D.Drag = 0.5f;
		D.Emissive = 0.3f;
		D.Color = Color;
		D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f));
		D.Spin = FVector(Rand.FRandRange(-500.f, 500.f), Rand.FRandRange(-500.f, 500.f), Rand.FRandRange(-500.f, 500.f));
		Make(D);
	}
	// ---- 4. A pinch of the old grey puff, and (Ultra) the twinkling glints.
	++UltraNesting;
	PlayPuffs(Location + Front * 0.5f, 3, 15.f * S, 70.f, FVector::UpVector, 0.7f, FLinearColor(0.5f, 0.5f, 0.53f));
	--UltraNesting;
	UltraGlints(Location + Front, Bright, 10, 26.f * S);
}

void APinguFX::PlaySplash(const FVector& Location)
{
	const FLinearColor Water(0.45f, 0.75f, 1.f);
	UltraBurst(Location, Water, 36, 0, 4, 6, 900.f, 22.f, FVector::UpVector, 0.45f);
	UltraRing(Location + FVector(0.f, 40.f, 10.f), FLinearColor(0.8f, 0.93f, 1.f), 160.f, 0.5f);
	++UltraNesting; // its ring and puffs below bring no layers of their own
	const int32 Num = ScaleCount(16);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Puff;
		D.Location = Location + FVector(Rand.FRandRange(-40.f, 40.f), Rand.FRandRange(-40.f, 40.f), 0.f);
		D.Velocity = PinguFXPrivate::RandomInCone(Rand, FVector::UpVector, 0.55f) * Rand.FRandRange(500.f, 1000.f);
		D.Size = Rand.FRandRange(18.f, 40.f);
		D.Life = Rand.FRandRange(0.55f, 0.9f);
		D.Gravity = 1900.f;
		D.Drag = 0.6f;
		D.Color = Index % 3 == 0 ? FLinearColor::White : Water;
		D.Emissive = 0.2f;
		SpawnInternal(D, false);
	}
	PlayRing(Location + FVector(0.f, 0.f, 10.f), 160.f, FLinearColor(0.8f, 0.93f, 1.f), 0.5f);
	PlayPuffs(Location, 6, 45.f, 250.f, FVector::UpVector, 1.f, FLinearColor::White);
	--UltraNesting;
}

void APinguFX::PlayExplosion(const FVector& Location, float Radius)
{
	// Radius: the blast's real reach (the snow bomb's push radius); its zone flashes exactly there on every setting.
	PlayZoneFlash(Location, Radius);
	UltraBlast(Location, Radius);
	++UltraNesting; // the blast layer has its own shards, glints and shockwave
	const float S = Radius / PinguFXPrivate::ExplosionLookRadius;
	// Flash.
	FPinguParticleDesc Flash;
	Flash.Mesh = EPinguFXMesh::Puff;
	Flash.Location = Location;
	Flash.Size = 240.f * S;
	Flash.Life = 0.12f;
	Flash.Emissive = 3.f;
	Flash.Color = FLinearColor(1.f, 0.95f, 0.8f);
	SpawnInternal(Flash, false);
	// Big snowy cloud (Ultra: fewer cartoon puffs, the blast layer's soft clouds fill it out).
	const bool bSoftClouds = bUltra && NiagaraSystems.IsValidIndex(PinguFXPrivate::LayerBlast) && NiagaraSystems[PinguFXPrivate::LayerBlast];
	const int32 Num = ScaleCount(bSoftClouds ? 10 : 18);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		const float Angle = Rand.FRandRange(0.f, 2.f * UE_PI);
		FPinguParticleDesc D;
		D.Mesh = EPinguFXMesh::Puff;
		D.Location = Location + FVector(FMath::Cos(Angle), Rand.FRandRange(-0.4f, 0.4f), FMath::Sin(Angle)) * 37.5f * S;
		D.Velocity = FVector(FMath::Cos(Angle), Rand.FRandRange(-0.3f, 0.3f), FMath::Sin(Angle) + 0.25f) * Rand.FRandRange(300.f, 750.f) * S;
		D.Size = Rand.FRandRange(45.f, 90.f) * S;
		D.Life = Rand.FRandRange(0.55f, 0.95f);
		D.Drag = 3.5f;
		D.Gravity = -60.f;
		D.Color = Index % 4 == 0 ? FLinearColor(0.75f, 0.88f, 1.f) : FLinearColor::White;
		D.Rotation = FRotator(Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.f, 360.f), 0.f);
		SpawnInternal(D, false);
	}
	PlayRing(Location, 210.f * S, FLinearColor(1.f, 0.9f, 0.6f), 0.4f);
	PlayShards(Location, 12, 1300.f, FLinearColor(0.7f, 0.9f, 1.f));
	PlayStars(Location, 6, 650.f);
	--UltraNesting;
}

void APinguFX::PlayGlowTrail(const FVector& From, const FVector& To, float Width, const FLinearColor& Color, float Opacity, float Life)
{
	const FVector Delta = To - From;
	const float SegmentLength = static_cast<float>(Delta.Size());
	if (SegmentLength < 1.f || Width <= 0.f || Opacity <= 0.f || Life <= 0.f)
	{
		return;
	}
	// One stretched cylinder per piece of the path, pieces touching end to end (no overlap, so no brighter
	// seams). When the pool is full, SpawnInternal recycles the most faded piece (the oldest tail end).
	FPinguParticleDesc D;
	D.Mesh = EPinguFXMesh::Glow;
	D.Location = From + Delta * 0.5f;
	D.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Delta / SegmentLength).Rotator();
	D.Length = SegmentLength;
	D.Size = Width;
	D.EndSize = Width * 0.5f;
	D.Curve = EPinguSizeCurve::Linear;
	D.Life = Life;
	D.Opacity = FMath::Min(Opacity, 1.f);
	D.bFadeOut = true;
	D.Color = Color;
	D.Emissive = PinguFXPrivate::TrailGlow;
	SpawnInternal(D, false);
}

// ---------------------------------------------------------------------------- afterimages

APinguFX::FGhost* APinguFX::AcquireGhost(int32 InMaxGhosts, bool bForSpirit)
{
	FGhost* Free = nullptr;
	FGhost* MostFaded = nullptr;
	FGhost* MostFadedAfterimage = nullptr; // the most faded one that is not a drifting spirit (a dash afterimage may take only these)
	int32 NumActive = 0;
	for (FGhost& Entry : Ghosts)
	{
		if (!Entry.bActive)
		{
			Free = Free ? Free : &Entry;
			continue;
		}
		++NumActive;
		if (!Entry.bDrift && (!MostFadedAfterimage || Entry.Age / FMath::Max(Entry.Life, 0.01f) > MostFadedAfterimage->Age / FMath::Max(MostFadedAfterimage->Life, 0.01f)))
		{
			MostFadedAfterimage = &Entry;
		}
		if (!MostFaded || Entry.Age / FMath::Max(Entry.Life, 0.01f) > MostFaded->Age / FMath::Max(MostFaded->Life, 0.01f))
		{
			MostFaded = &Entry;
		}
	}
	if (NumActive >= InMaxGhosts && MostFaded)
	{
		// At the cap (lower effects settings draw fewer): the most faded one makes room. A dash afterimage never
		// takes a knocked-out penguin's drifting spirit (it would pop out mid-fade); it is skipped instead.
		if (MostFadedAfterimage)
		{
			return MostFadedAfterimage;
		}
		return bForSpirit ? MostFaded : nullptr;
	}
	if (Free)
	{
		return Free;
	}
	if (Ghosts.Num() < PinguFXPrivate::MaxGhosts)
	{
		FGhost& New = Ghosts.AddDefaulted_GetRef();
		New.Mesh = NewObject<UPoseableMeshComponent>(this, NAME_None, RF_Transient);
		New.Mesh->SetupAttachment(Root);
		New.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		New.Mesh->SetCastShadow(false);
		// A blurry see-through copy: a low LOD (index 2, once the penguin has LODs) and no Lumen / ray tracing presence.
		New.Mesh->SetForcedLOD(3);
		New.Mesh->bAffectDistanceFieldLighting = false;
		New.Mesh->bAffectDynamicIndirectLighting = false;
		New.Mesh->bVisibleInRayTracing = false;
		New.Mesh->bVisibleInReflectionCaptures = false;
		New.Mesh->bVisibleInRealTimeSkyCaptures = false;
		New.Mesh->RegisterComponent();
		GhostMeshes.Add(New.Mesh);
		if (GhostMaterial)
		{
			New.Material = UMaterialInstanceDynamic::Create(GhostMaterial, this);
			GhostMaterials.Add(New.Material);
		}
		return &New;
	}
	// The pool itself is full: same rule, an afterimage leaves the spirits alone (null when only spirits are active).
	return (bForSpirit || !MostFaded || !MostFaded->bDrift) ? MostFaded : MostFadedAfterimage;
}

APinguFX::FGhost* APinguFX::StartGhost(USkeletalMeshComponent* Source, const FLinearColor& Color, float Life, float Opacity, bool bForSpirit)
{
	EnsureAssets();
	if (!Source || !Source->GetSkinnedAsset() || !GhostMaterial)
	{
		return nullptr;
	}
	// Every afterimage is a whole extra penguin to draw: fewer at the same time on lower effects settings.
	const int32 Cap = Density >= 0.999f ? PinguFXPrivate::MaxGhosts : FMath::Clamp(FMath::RoundToInt(PinguFXPrivate::MaxGhosts * Density), 2, PinguFXPrivate::MaxGhosts);
	FGhost* Afterimage = AcquireGhost(Cap, bForSpirit);
	if (!Afterimage || !Afterimage->Mesh)
	{
		return nullptr;
	}
	// The cheap one-slot ghost penguin when the art has it (the pose copy matches the bones by name).
	USkinnedAsset* const GhostAsset = GhostMesh ? static_cast<USkinnedAsset*>(GhostMesh.Get()) : Source->GetSkinnedAsset();
	if (Afterimage->Mesh->GetSkinnedAsset() != GhostAsset)
	{
		Afterimage->Mesh->SetSkinnedAssetAndUpdate(GhostAsset, true);
		for (int32 Slot = 0; Slot < Afterimage->Mesh->GetNumMaterials(); ++Slot)
		{
			Afterimage->Mesh->SetMaterial(Slot, Afterimage->Material);
		}
	}
	Afterimage->Mesh->CopyPoseFromSkeletalComponent(Source);
	Afterimage->Mesh->SetVisibility(true);
	Afterimage->Age = 0.f;
	Afterimage->Life = Life;
	Afterimage->Opacity = Opacity;
	Afterimage->bActive = true;
	Afterimage->bDrift = false;
	if (Afterimage->Material)
	{
		Afterimage->Material->SetVectorParameterValue(TEXT("Color"), Color);
		Afterimage->Material->SetScalarParameterValue(TEXT("Opacity"), Opacity);
	}
	return Afterimage;
}

void APinguFX::PlayGhost(USkeletalMeshComponent* Source, const FLinearColor& Color, float Life, float Opacity)
{
	if (FGhost* Afterimage = StartGhost(Source, Color, Life, Opacity))
	{
		Afterimage->Mesh->SetWorldTransform(Source->GetComponentTransform());
	}
}

void APinguFX::PlaySpirit(USkeletalMeshComponent* Source, const FVector& Origin, float Yaw, float Scale, float RiseSpeed, float Sway,
	const FLinearColor& Color, float Life, float Opacity, bool bRestPose)
{
	if (Scale <= 0.f || Life <= 0.f)
	{
		return;
	}
	FGhost* Drifting = StartGhost(Source, Color, Life, Opacity, true);
	if (!Drifting)
	{
		return;
	}
	if (bRestPose && Drifting->Mesh->RequiredBones.IsValid())
	{
		// Standing upright in the rest pose, whatever the penguin was doing: a copy of the knock-out pose (on its back, its
		// head toward the camera) is a stack of see-through rings, not a penguin. StartGhost copied the pose a moment ago:
		// overwritten here, before the copy is ever drawn.
		if (const USkinnedAsset* RestAsset = Drifting->Mesh->GetSkinnedAsset())
		{
			Drifting->Mesh->BoneSpaceTransforms = RestAsset->GetRefSkeleton().GetRefBonePose();
			Drifting->Mesh->MarkRefreshTransformDirty();
		}
	}
	Drifting->bDrift = true;
	Drifting->Origin = Origin;
	Drifting->Yaw = Yaw;
	Drifting->Scale = Scale;
	Drifting->RiseSpeed = RiseSpeed;
	Drifting->Sway = Sway;
	Drifting->Phase = Rand.FRandRange(-0.4f, 0.4f);
	UpdateGhost(*Drifting, 0.f); // placed (and see-through) before it is drawn the first time
}

void APinguFX::UpdateGhost(FGhost& Entry, float T)
{
	using namespace PinguFXPrivate;
	if (!Entry.bDrift)
	{
		if (Entry.Material)
		{
			Entry.Material->SetScalarParameterValue(TEXT("Opacity"), Entry.Opacity * (1.f - T) * (1.f - T));
		}
		return;
	}
	// A spirit pops in from a smaller size, drifts up (or down) a little slower toward its end and sways from side to side,
	// leaning into the sway; it fades in quickly and out over its second half.
	const float Wave = FMath::Sin(Entry.Age * 2.f * UE_PI * SpiritSwayRate + Entry.Phase);
	const float SwayIn = FMath::Min(Entry.Age / SpiritSwayInTime, 1.f);
	const float Drift = Entry.RiseSpeed * Entry.Age * (1.f - SpiritSlowDown * T);
	const FVector Where = Entry.Origin + FVector(Entry.Sway * Wave * SwayIn, 0.f, Drift);
	const float Pop = FMath::InterpEaseOut(SpiritPopFrom, 1.f, FMath::Min(T / SpiritPopTime, 1.f), 2.f);
	Entry.Mesh->SetWorldLocationAndRotation(Where, FRotator(-Wave * SwayIn * SpiritLean, Entry.Yaw, 0.f));
	Entry.Mesh->SetWorldScale3D(FVector(Entry.Scale * Pop));
	if (Entry.Material)
	{
		const float Fade = FMath::SmoothStep(0.f, SpiritFadeIn, T) * (1.f - FMath::SmoothStep(SpiritFadeOutFrom, 1.f, T));
		Entry.Material->SetScalarParameterValue(TEXT("Opacity"), Entry.Opacity * Fade);
	}
}

void APinguFX::UpdateGhosts(float Dt)
{
	for (FGhost& Entry : Ghosts)
	{
		if (!Entry.bActive || !Entry.Mesh)
		{
			continue;
		}
		Entry.Age += Dt;
		const float T = Entry.Age / FMath::Max(Entry.Life, 0.01f);
		if (T >= 1.f)
		{
			Entry.bActive = false;
			Entry.bDrift = false;
			Entry.Mesh->SetVisibility(false);
			continue;
		}
		UpdateGhost(Entry, T);
	}
}

// ---------------------------------------------------------------------------- feel

void APinguFX::PlayShake(float Trauma)
{
	if (APenguinCamera* Camera = APenguinCamera::FindOrSpawn(GetWorld()))
	{
		Camera->AddTrauma(Trauma);
	}
}

void APinguFX::HitStop(const UObject* Ctx, float RealSeconds, float Dilation)
{
	APinguFX* FX = Get(Ctx);
	if (!FX || PinguFXPrivate::IsNetworkWorld(FX->GetWorld()))
	{
		return; // a LAN game never stops the time (everybody's penguin would freeze)
	}
	if (RealSeconds > FX->HitStopLeft)
	{
		FX->HitStopLeft = RealSeconds;
	}
	FX->HitStopDilation = FMath::Clamp(Dilation, 0.001f, 1.f);
	FX->UpdateTimeDilation(0.f);
}

void APinguFX::SetSlowMotion(const UObject* Ctx, float Dilation)
{
	APinguFX* FX = Get(Ctx);
	if (!FX || PinguFXPrivate::IsNetworkWorld(FX->GetWorld()))
	{
		return; // solo only
	}
	FX->SlowMotion = FMath::Clamp(Dilation, 0.01f, 1.f);
	FX->UpdateTimeDilation(0.f);
}

void APinguFX::SetKillCamDilation(const UObject* Ctx, float Dilation)
{
	APinguFX* FX = Get(Ctx);
	if (!FX)
	{
		return;
	}
	const float Clamped = FMath::Clamp(Dilation, 0.01f, 1.f);
	if (!FMath::IsNearlyEqual(Clamped, FX->KillCamDilation))
	{
		FX->KillCamDilation = Clamped;
		FX->UpdateTimeDilation(0.f);
	}
}

void APinguFX::UpdateTimeDilation(float RealDt)
{
	HitStopLeft = FMath::Max(0.f, HitStopLeft - RealDt);
	const float Base = FMath::Min(SlowMotion, KillCamDilation);
	const float Wanted = HitStopLeft > 0.f ? FMath::Min(HitStopDilation, Base) : Base;
	if (!FMath::IsNearlyEqual(Wanted, AppliedDilation))
	{
		AppliedDilation = Wanted;
		UGameplayStatics::SetGlobalTimeDilation(this, Wanted);
	}
}

// ---------------------------------------------------------------------------- audio

USoundBase* APinguFX::GetSound(EPinguSound Which)
{
	EnsureAssets();
	const int32 Index = static_cast<int32>(Which);
	return Sounds.IsValidIndex(Index) ? Sounds[Index].Get() : nullptr;
}

void APinguFX::PlaySound(EPinguSound Which, float Volume, float PitchVariance, float Pitch)
{
	if (USoundBase* Asset = GetSound(Which))
	{
		// The kill cam's slow motion: deeper sounds (not the full slow-down: at 0.05x they would be a rumble).
		const float SlowPitch = FMath::Lerp(0.6f, 1.f, KillCamDilation);
		const float FinalPitch = Pitch * SlowPitch * (1.f + Rand.FRandRange(-PitchVariance, PitchVariance));
		UGameplayStatics::PlaySound2D(this, Asset, Volume * SfxVolume, FinalPitch);
	}
}

void APinguFX::Music(const UObject* Ctx, int32 Track)
{
	APinguFX* FX = Get(Ctx);
	if (!FX || !FX->MusicComponent || FX->MusicTrack == Track)
	{
		return;
	}
	FX->MusicTrack = Track;
	if (Track <= 0)
	{
		FX->MusicComponent->FadeOut(0.8f, 0.f);
		return;
	}
	const TCHAR* Path = Track == 1 ? TEXT("/Game/Audio/S_music_menu.S_music_menu") : TEXT("/Game/Audio/S_music_battle.S_music_battle");
	if (USoundBase* Song = LoadObject<USoundBase>(nullptr, Path, nullptr, LOAD_Quiet | LOAD_NoWarn))
	{
		FX->MusicComponent->Stop();
		FX->MusicComponent->SetSound(Song);
		// The music volume is the component's multiplier (SetVolumes changes it live); the fade goes to full level -
		// fading to the volume as well played the music at volume squared, and a track started while muted stayed silent.
		FX->MusicComponent->SetVolumeMultiplier(FX->MusicVolume);
		FX->MusicComponent->FadeIn(1.2f, 1.f);
	}
}

void APinguFX::SetVolumes(const UObject* Ctx, float InSfxVolume, float InMusicVolume)
{
	if (APinguFX* FX = Get(Ctx))
	{
		FX->SfxVolume = FMath::Clamp(InSfxVolume, 0.f, 1.f);
		FX->MusicVolume = FMath::Clamp(InMusicVolume, 0.f, 1.f);
		if (FX->MusicComponent)
		{
			FX->MusicComponent->SetVolumeMultiplier(FX->MusicVolume);
		}
	}
}

// ---------------------------------------------------------------------------- tick

void APinguFX::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Density = PinguFXPrivate::ReadDensity();
	bUltra = PinguFXPrivate::WantUltra();
	const float RealDt = FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f);
	UpdateTimeDilation(RealDt);

	const UWorld* World = GetWorld();
	if (World && World->IsPaused())
	{
		return;
	}
	const float Dt = FMath::Min(DeltaSeconds, 0.1f);
	UpdateUltraSnowfall(Dt);
	UpdateSnowfall(Dt);
	SimulateParticles(Dt);
	UpdateGhosts(Dt);
	UpdateZones(Dt);
	UpdateAshPiles();
}

// ---------------------------------------------------------------------------- LAN relay

APinguFXRelay::APinguFXRelay()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_LastDemotable; // after everything that plays effects this frame
	PrimaryActorTick.bTickEvenWhenPaused = true;
	SetActorEnableCollision(false);

	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(30.f); // no properties: a batch goes out right away with ForceNetUpdate

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void APinguFXRelay::BeginPlay()
{
	Super::BeginPlay();
	SetActorTickEnabled(HasAuthority()); // clients only receive
}

void APinguFXRelay::Queue(const FPinguFXEvent& Event)
{
	if (Pending.Events.Num() < PinguFXPrivate::MaxEventsPerBatch)
	{
		Pending.Events.Add(Event);
	}
}

void APinguFXRelay::QueueParticle(const FPinguParticleDesc& Desc)
{
	if (Pending.Particles.Num() < PinguFXPrivate::MaxParticlesPerBatch)
	{
		Pending.Particles.Add(Desc);
	}
}

void APinguFXRelay::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Flush();
}

void APinguFXRelay::Flush()
{
	if (Pending.Events.Num() == 0 && Pending.Particles.Num() == 0)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetRealTimeSeconds() : 0.0;
	if (LastFlushTime >= 0.0 && Now - LastFlushTime < PinguFXPrivate::MinFlushInterval)
	{
		return; // the next frame takes these along
	}
	LastFlushTime = Now;
	if (HasAuthority())
	{
		MulticastPlayFX(Pending);
		ForceNetUpdate(); // out with this frame's replication
	}
	Pending.Events.Reset();
	Pending.Particles.Reset();
}

void APinguFXRelay::MulticastPlayFX_Implementation(const FPinguFXBatch& Batch)
{
	if (HasAuthority())
	{
		return; // the host played them already
	}
	APinguFX* FX = APinguFX::Get(this);
	if (!FX)
	{
		return;
	}
	for (const FPinguFXEvent& Event : Batch.Events)
	{
		FX->PlayEvent(Event);
	}
	for (const FPinguParticleDesc& Desc : Batch.Particles)
	{
		FX->Spawn(Desc); // a client never broadcasts, so this only plays here
	}
}

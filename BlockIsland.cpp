// Pingu - floating block island on a grid.

#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PinguFX.h"
#include "PinguTuning.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Net/UnrealNetwork.h"

namespace BlockIslandPrivate
{
	static const FName IceTag(TEXT("Ice"));
	constexpr float SpawnHeightAboveSurface = 62.f;
	/** GetSpreadSpawnLocations: the outermost starting places are this many blocks in from the island's ends. */
	constexpr int32 SpawnEdgeMargin = 2;
	/** A LAN client whose blocks differ from the host's this long warns (an update may be on its way meanwhile). */
	constexpr float NetMismatchWarnDelay = 1.f;

	// ---- the arena's size (GetArenaSize)
	/** The arena's width (blocks, before the seed's jitter) for 2, 3, ..., 10 penguins: really small for two, a little
	 *  narrower per penguin the more there are. Four is what the arena was first tuned for (the level's RandomWidth 26, when a
	 *  match had at most four penguins): that row is the old island, block for block. */
	constexpr int32 ArenaWidthByPenguins[] = { 15, 20, 26, 30, 34, 37, 40, 43, 46 };
	constexpr int32 ArenaFewestPenguins = 2;
	static_assert(UE_ARRAY_COUNT(ArenaWidthByPenguins) == PinguMaxPenguins - ArenaFewestPenguins + 1, "a width for 2 to 10 penguins");
	/** The four-penguin width: the arena's other numbers scale with the base width against it. */
	constexpr float ArenaReferenceWidth = 26.f;
	/** A sized arena is never narrower than this (blocks); the old fixed one keeps its 18. Never wider than 60. */
	constexpr int32 ArenaMinWidth = 12;
	constexpr int32 LegacyArenaMinWidth = 18;
	constexpr int32 ArenaMaxWidth = 60;
	/** Smaller than this (scale): hills at most 2 rows high and platforms at most 4 above the ground - a small island is
	 *  lower too, so the camera comes in close. Bigger than this: hills up to 4 rows. */
	constexpr float SmallArenaScale = 0.8f;
	constexpr float BigArenaScale = 1.4f;
	/** The chance of a high platform (3 rows further up): none on the smallest arenas (scale 0.6), the old 0.25 at four
	 *  penguins, 0.4 on the biggest (scale 1.6 and up). */
	constexpr float HighPlatformScaleNone = 0.6f;
	constexpr float HighPlatformChanceFour = 0.25f;
	constexpr float HighPlatformScaleMost = 1.6f;
	constexpr float HighPlatformChanceMost = 0.4f;
	/** The old arena's numbers (four penguins), scaled: the jitter's range, hills, pits, ice patches, platforms. */
	constexpr float ArenaJitterDown = 2.f;
	constexpr float ArenaJitterUp = 6.f;
	constexpr float ArenaBumps = 3.f;
	constexpr int32 ArenaBumpsFewest = 2;
	constexpr int32 ArenaBumpsMost = 5;
	constexpr float ArenaPits = 2.f;
	constexpr float ArenaIcePatches = 2.f;
	constexpr float ArenaMirroredPlatformsMin = 2.f;
	constexpr float ArenaMirroredPlatformsMax = 3.f;
	constexpr float ArenaPlatformsMin = 3.f;
	constexpr float ArenaPlatformsMax = 5.f;
	constexpr int32 ArenaPlatformTries = 120;

	// ---- the arena's ends (GenerateArena)
	/** The chance that one end of an arena steps down to the sea in one-block steps (a Giant Snowball can roll off it). Before
	 *  2026-11-05 nothing made an end do that: only 18 % of the ends did it by luck of the hills (now about 65 %). */
	constexpr float ArenaEndStepChance = 0.6f;
	/** An end that steps down: this many columns (from the outermost) are one block lower each than the next one in. */
	constexpr int32 ArenaEndStepColumns = 3;
	/** The lowest a stepping end goes (the arena's own lowest top is -2). */
	constexpr int32 ArenaEndStepFloor = -3;

	/** The instance components in VisualCellKeys order (VisBig's keys are big blocks' bottom-left cells). */
	enum EVisualSlot : int32 { VisSnow, VisRock, VisDeepRock, VisIce, VisTemp, VisCrumble, VisCaps, VisBig, VisCount };

	// ---- big blocks and rising blocks
	static_assert(ABlockIsland::BigBlockCells == PinguTuning::IceBigBlockCells, "a big block of the island is a grown penguin's big block of ice");
	/** A rise's start: its look this tall (a share of the full block; never 0, a zero scale is no instance). */
	constexpr float RiseMinShare = 0.02f;
	/** Big blocks' instances along the depth axis: at most this many cells deep each (the island's 3 rows: 2 instances of 1.5). */
	constexpr int32 BigBlockDepthCells = 2;
	/** A penguin pushed out of new blocks (PushPenguinsOutOfBlocks) is set this far (cm) from them, ... */
	constexpr float PushOutGap = 2.f;
	/** ... reaching less than this far into them (cm) it is only grazing them (left alone), ... */
	constexpr float PushOutMargin = 2.f;
	/** ... set aside by less than this (cm) it keeps its speed (only nudged), else it is shoved at least SandShoveSpeed. */
	constexpr float PushOutNudge = 15.f;

	// ---- sand (AddSandBlocks)
	/** A sand block whose block goes shakes this long before it falls: a beat to jump off, and easy to read. */
	constexpr float SandWarnTime = 0.25f;
	/** Falling sand, cm/s^2 (ABlockIsland::FallingBlockGravity: everything that falls like a block uses it). */
	constexpr float SandGravity = ABlockIsland::FallingBlockGravity;
	/** Top falling speed, cm/s (ABlockIsland::FallingBlockMaxSpeed). */
	constexpr float SandMaxFallSpeed = ABlockIsland::FallingBlockMaxSpeed;
	/** A penguin under falling sand is shoved aside at least this fast (cm/s)... */
	constexpr float SandShoveSpeed = 380.f;
	/** ... with this little hop (cm/s) when it stood on the ground (in the air it only loses its upward speed). */
	constexpr float SandShoveHop = 180.f;
	/** Lifted onto a landed sand block (no room beside it): at least this upward speed (cm/s). */
	constexpr float SandLiftSpeed = 150.f;
	/** Gap (cm) between a moved penguin's capsule and the block. */
	constexpr float SandShoveGap = 2.f;
	/** A penguin (or a block that appeared) overlapping a sand block by at most this much (cm) is only grazing it. */
	constexpr float SandOverlapMargin = 2.f;
	/** Shoved aside from the ground: there must be ground at most this many cells under the new spot (no pit, no sea). */
	constexpr int32 SandShoveMaxDropCells = 3;
	/** Within this distance (cm) of the block's middle, a penguin is shoved the way it was going. */
	constexpr float SandShoveCenterBand = 12.f;
	/** A penguin only grazing the block (set aside less than this, cm) keeps its speed: no shove, no bonk. */
	constexpr float SandNudgeDistance = 15.f;
	/** Climbing out of blocks (a landing pops up past blocks in the way, a lifted penguin past blocks over its head). */
	constexpr int32 SandMaxClimbRows = 16;
	/** A landing this fast (cm/s) is a full thud (puffs, sound, shake). */
	constexpr float SandHardLandingSpeed = 2200.f;
	/** Shaking before the fall: rate (rad/s), sideways amplitude at the start and its growth toward the drop (cm at 1 m
	 *  blocks), and the tilt at the end (degrees). */
	constexpr float SandShakeRate = 55.f;
	constexpr float SandShakeMin = 1.5f;
	constexpr float SandShakeGrow = 5.f;
	constexpr float SandShakeTilt = 3.f;
	/** A penguin's splash is this far above SeaLevel (the sea surface): falling sand splashes there too. */
	constexpr float SeaSurfaceAboveLevel = 50.f;
	/** FallingBlocks components, in this order: snow, rock, deep rock, ice. */
	constexpr int32 SandSlotCount = 4;
	/** Sounds of sand: the creak before it falls (crumble, pitched up: a small block), the landing thud (iceberg crash,
	 *  pitched up, volume times the impact), the splash and the bonk of a shoved penguin (snowball hit, pitched down). */
	constexpr float SandWarnVolume = 0.4f;
	constexpr float SandWarnPitch = 1.35f;
	constexpr float SandLandVolume = 0.5f;
	constexpr float SandLandPitch = 1.3f;
	constexpr float SandSplashVolume = 0.45f;
	constexpr float SandBonkVolume = 0.4f;
	constexpr float SandBonkPitch = 0.8f;
	/** Camera shake of a hard landing (the iceberg's own crash is 0.6). */
	constexpr float SandLandShake = 0.1f;
	/** Falling sand coming down onto a penguin keeps it this far (cm) under its bottom while it pushes it down ... */
	constexpr float SandCarryGap = 1.f;
	/** ... and crushes it once ground is within this (cm) of where its feet would have to go. */
	constexpr float SandPressTolerance = 2.f;
	/** A crushed penguin is flung out to its side this fast (cm/s) and up (the knock-out's launch). */
	constexpr float SandCrushLaunchX = 500.f;
	constexpr float SandCrushLaunchZ = 300.f;

	// ---- broken ice
	/** Shards per broken ice block flying along the push (on top of a small burst of the usual shatter shards). */
	constexpr int32 IceShardsAlongPush = 4;
	constexpr float IceShardSpeed = 700.f;
	/** Half angle (radians) of the cone around the push the shards fly in. */
	constexpr float IceShardSpread = 0.6f;
	/** BreakIceInRadius without a push: the shards fly outward plus this much up. */
	constexpr float IceShardLift = 0.6f;
	/** Ice blocks breaking within this many seconds of each other make one shatter sound... */
	constexpr double ShatterSoundInterval = 0.06;
	/** ... a bit louder for every extra block. */
	constexpr float ShatterVolume = 0.7f;
	constexpr float ShatterVolumePerBlock = 0.1f;
	static const FLinearColor IceShardColor(0.6f, 0.9f, 1.f);
	static const FLinearColor IceDustColor(0.85f, 0.93f, 1.f);

	// ---- lines through the grid (IsLineClear)
	/** A line crossing both cell borders this close together (a share of the line) goes exactly through their corner. */
	constexpr double LineCornerTolerance = 1.e-9;

	/** How far (cm) sand falls Seconds after it starts from rest: constant gravity up to the top speed. The host and its
	 *  LAN clients compute the same fall from the same start time. */
	static float SandFallDistance(double Seconds)
	{
		if (Seconds <= 0.0)
		{
			return 0.f;
		}
		const float T = static_cast<float>(Seconds);
		const float TopSpeedTime = SandMaxFallSpeed / SandGravity;
		if (T <= TopSpeedTime)
		{
			return 0.5f * SandGravity * T * T;
		}
		return 0.5f * SandMaxFallSpeed * TopSpeedTime + SandMaxFallSpeed * (T - TopSpeedTime);
	}

	static float SandFallSpeed(double Seconds)
	{
		return FMath::Min(SandGravity * static_cast<float>(FMath::Max(Seconds, 0.0)), SandMaxFallSpeed);
	}

	/** What a falling sand block met in one frame (host): the box it swept, or the cell it landed in. */
	struct FSandContact
	{
		/** World box on the play plane. */
		FBox Box;
		/** World height of the block's bottom now. */
		float BottomZ;
		/** Falling speed now, or when it landed (cm/s). */
		float Speed;
		/** The cell it landed in (or, still falling, the one it fell from). */
		FIntPoint Cell;
		bool bLanded;
		/** Who made it fall, if known (FSandBlock::CausedBy). */
		APenguinCharacter* CausedBy;
		/** An iceberg's ice falling on down a lower column (FSandBlock::bIcebergIce). */
		bool bIcebergIce;
		/** Cells per side (a big block: BigBlockCells). */
		int32 Span;
	};

	/** Falling sand pressed a penguin against what is below it: knocked out (reason Crushed), flung out to its side - by
	 *  the iceberg (reason Iceberg, as if it had landed on it) when the block is an iceberg's ice falling on down a lower
	 *  column. False if that did not knock it out (the ice of a frozen penguin took it, ...). */
	static bool CrushUnderSand(APenguinCharacter* Penguin, APenguinCharacter* CausedBy, float MiddleX, bool bIcebergIce)
	{
		const FVector Loc = Penguin->GetActorLocation();
		FPenguinHit Hit;
		Hit.Instigator = CausedBy;
		if (bIcebergIce)
		{
			Hit.Source = EPenguinAbility::IcebergDrop;
		}
		Hit.SourceSize = 1.f; // falling ice has no size of its own: whoever made it fall, the victim's size alone counts
		Hit.bDeadly = true;
		Hit.Reason = bIcebergIce ? EPenguinKOReason::Iceberg : EPenguinKOReason::Crushed;
		Hit.Knockback = FVector2D(Loc.X < MiddleX ? -SandCrushLaunchX : SandCrushLaunchX, SandCrushLaunchZ);
		Hit.Location = Loc;
		return Penguin->ApplyPenguinHit(Hit);
	}

	/** The FallingBlocks component that draws a kind of block (INDEX_NONE: none). */
	static int32 SandSlot(EBlockKind Kind)
	{
		switch (Kind)
		{
		case EBlockKind::Snow: return 0;
		case EBlockKind::Rock: return 1;
		case EBlockKind::DeepRock: return 2;
		case EBlockKind::Ice: return 3;
		default: return INDEX_NONE;
		}
	}

	/** The "Kind" material parameter of each look (the art's block materials branch on it). */
	enum EBlockLook : int32 { LookSnow, LookRock, LookDeepRock, LookIce, LookTempIce, LookCrumble, LookSnowCap, LookBackdrop, LookOcean };

	/** Optional art asset /Game/Environment/<Name> (a per-kind mesh or material): null, without a warning, when absent. */
	template <typename T>
	static T* LoadOptionalArt(const TCHAR* Name)
	{
		const FString Package = FString(TEXT("/Game/Environment/")) + Name;
		if (!FPackageName::DoesPackageExist(Package))
		{
			return nullptr;
		}
		return LoadObject<T>(nullptr, *(Package + TEXT(".") + Name), nullptr, LOAD_Quiet | LOAD_NoWarn);
	}

	static int32 PackCell(const FIntPoint& Cell)
	{
		return static_cast<int32>((static_cast<uint32>(Cell.Y) << 16) | (static_cast<uint32>(Cell.X) & 0xFFFFu));
	}

	static FIntPoint UnpackCell(int32 Packed)
	{
		const uint32 Bits = static_cast<uint32>(Packed);
		return FIntPoint(static_cast<int16>(Bits & 0xFFFFu), static_cast<int16>(Bits >> 16));
	}

	/** A collision rectangle in cells (exact, so boxes that did not change can be kept). */
	static uint64 RectKey(int32 X, int32 Z, int32 Width, int32 Height, bool bIce)
	{
		return static_cast<uint64>(static_cast<uint16>(X))
			| (static_cast<uint64>(static_cast<uint16>(Z)) << 16)
			| (static_cast<uint64>(Width & 0x3FFF) << 32)
			| (static_cast<uint64>(Height & 0x3FFF) << 46)
			| (static_cast<uint64>(bIce ? 1 : 0) << 60);
	}

	static uint32 MixHash(uint32 Value)
	{
		Value ^= Value >> 16;
		Value *= 0x7FEB352Du;
		Value ^= Value >> 15;
		Value *= 0x846CA68Bu;
		Value ^= Value >> 16;
		return Value;
	}

	// The look of a block (its texture tile and tint) comes from the material's PerInstanceRandom, which the engine
	// draws from a stream by INSTANCE INDEX: take one block out and every later block's number shifts, so the textures
	// changed whenever a column crumbled. A seed per instance (AdditionalRandomSeeds resets the stream at every index)
	// from the cell, the depth row and the island's seed gives each block its own number wherever it sits in the list.
	static int32 StableLookSeed(const FIntPoint& Cell, int32 Row, int32 IslandSeed)
	{
		return static_cast<int32>(MixHash(MixHash(static_cast<uint32>(PackCell(Cell)) ^ (static_cast<uint32>(IslandSeed) * 0x9E3779B1u))
			+ static_cast<uint32>(Row) * 0x85EBCA6Bu + 0x1234567u));
	}

	static void ApplyStableLookSeeds(UInstancedStaticMeshComponent* Component, const TArray<int32>& Seeds)
	{
		Component->AdditionalRandomSeeds.SetNum(Seeds.Num());
		for (int32 Index = 0; Index < Seeds.Num(); ++Index)
		{
			Component->AdditionalRandomSeeds[Index].StartInstanceIndex = Index;
			Component->AdditionalRandomSeeds[Index].RandomSeed = Seeds[Index];
		}
	}

	template <typename ValueType>
	static bool SameKeys(const TMap<FIntPoint, ValueType>& A, const TMap<FIntPoint, ValueType>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const TPair<FIntPoint, ValueType>& Pair : A)
		{
			if (!B.Contains(Pair.Key))
			{
				return false;
			}
		}
		return true;
	}

	static bool SameCells(const TSet<FIntPoint>& A, const TSet<FIntPoint>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const FIntPoint& Cell : A)
		{
			if (!B.Contains(Cell))
			{
				return false;
			}
		}
		return true;
	}

	static const TCHAR* LayoutName(EIslandLayout Layout)
	{
		switch (Layout)
		{
		case EIslandLayout::MovementTest: return TEXT("movement test");
		case EIslandLayout::Random: return TEXT("random");
		case EIslandLayout::Flat: return TEXT("flat");
		case EIslandLayout::Arena: return TEXT("arena");
		case EIslandLayout::Lobby: return TEXT("lobby");
		}
		return TEXT("?");
	}
}

ABlockIsland::ABlockIsland()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// After the gameplay of the frame: all of its edits are rebuilt once, before rendering and replication.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	// LAN: the level's island is a net startup actor; the host's blocks reach every client through NetState.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.f); // a change goes out the frame it happens (ForceNetUpdate)

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	auto MakeInstances = [this](const TCHAR* Name)
	{
		UInstancedStaticMeshComponent* Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		Instances->SetupAttachment(Root);
		Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Instances->SetGenerateOverlapEvents(false);
		Instances->SetCanEverAffectNavigation(false);
		Instances->bUseGpuLodSelection = true; // LOD per block instance (near blocks detailed, far ones cheap), not per island
		return Instances;
	};
	SnowBlocks = MakeInstances(TEXT("SnowBlocks"));
	RockBlocks = MakeInstances(TEXT("RockBlocks"));
	DeepRockBlocks = MakeInstances(TEXT("DeepRockBlocks"));
	IceBlocks = MakeInstances(TEXT("IceBlocks"));
	TempBlocks = MakeInstances(TEXT("TempBlocks"));
	BigIceBlocks = MakeInstances(TEXT("BigIceBlocks"));
	CrumbleBlocks = MakeInstances(TEXT("CrumbleBlocks"));
	SnowCaps = MakeInstances(TEXT("SnowCaps"));
	Backdrop = MakeInstances(TEXT("Backdrop"));
	Ocean = MakeInstances(TEXT("Ocean"));
	// Shaking and falling sand, one component per look (in BlockIslandPrivate::SandSlot order). Empty ones draw nothing.
	for (const TCHAR* Name : { TEXT("FallingSnow"), TEXT("FallingRock"), TEXT("FallingDeepRock"), TEXT("FallingIce") })
	{
		FallingBlocks.Add(MakeInstances(Name));
	}
	BackdropPeaks = MakeInstances(TEXT("BackdropPeaks"));
	BackdropDomes = MakeInstances(TEXT("BackdropDomes"));
	Floes = MakeInstances(TEXT("Floes"));
	IceFoam = MakeInstances(TEXT("IceFoam"));
	Mountains = MakeInstances(TEXT("Mountains"));
	auto MakeRing = [this](const TCHAR* Name)
	{
		UStaticMeshComponent* Ring = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Ring->SetupAttachment(Root);
		Ring->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ring->SetGenerateOverlapEvents(false);
		Ring->SetCanEverAffectNavigation(false);
		Ring->SetCastShadow(false);
		Ring->bAffectDistanceFieldLighting = false;
		Ring->bAffectDynamicIndirectLighting = false;
		return Ring;
	};
	CloudRing = MakeRing(TEXT("CloudRing"));
	SkyRing = MakeRing(TEXT("SkyRing"));
	CloudRing->SetTranslucentSortPriority(-10); // behind every other see-through thing (snow, effects)
	SkyRing->SetTranslucentSortPriority(-20); // and the sky behind the clouds
	// The background casts no shadows, and a 10 km sea plane or far scaled boxes make poor distance fields (Lumen / DFAO).
	for (UPrimitiveComponent* Part : TArray<UPrimitiveComponent*>{ Backdrop, Ocean, BackdropPeaks, BackdropDomes, Floes, IceFoam, Mountains })
	{
		Part->SetCastShadow(false);
		Part->bAffectDistanceFieldLighting = false;
	}
	IceFoam->bAffectDynamicIndirectLighting = false;

	BlockMesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Environment/SM_Block.SM_Block")));
	SnowCapMesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Environment/SM_SnowCap.SM_SnowCap")));
	BlockMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Environment/M_Block.M_Block")));
	OceanMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Environment/M_Ocean.M_Ocean")));
}

void ABlockIsland::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(ABlockIsland, NetState, COND_None, REPNOTIFY_Always);
}

void ABlockIsland::ResolveAssets()
{
	UsedBlockMesh = BlockMesh.LoadSynchronous();
	if (!UsedBlockMesh)
	{
		UsedBlockMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}
	UsedSnowCapMesh = SnowCapMesh.LoadSynchronous();
	UsedMaterial = BlockMaterial.LoadSynchronous();
	if (!UsedMaterial)
	{
		UsedMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
}

void ABlockIsland::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	GenerateCells();
	bVisualSetupDirty = true;
	BuildVisuals();
}

void ABlockIsland::BeginPlay()
{
	Super::BeginPlay();
	// The effects actor (on the LAN host with its relay to the clients) is there before the first edit plays effects.
	APinguFX::Get(this);
	Rebuild();
}

void ABlockIsland::Rebuild()
{
	const UWorld* MyWorld = GetWorld();
	const bool bGame = MyWorld && MyWorld->IsGameWorld();
	if (bGame && IsNetClient() && NetState.Generation > 0)
	{
		// A LAN client always shows the host's island, whatever its own settings say.
		bNetStatePending = true;
		ApplyNetState();
		FlushEdits();
		return;
	}
	bCollapsing = false;
	Crumbling.Reset();
	BrokenBlocks = 0;
	GenerateCells();
	if (!bGame)
	{
		bVisualSetupDirty = true; // the editor's Rebuild button picks up changed looks
	}
	BuildVisuals();
	if (bGame)
	{
		BuildCollision();
	}
	bCollisionDirty = false;
	if (bGame && CanEdit())
	{
		ResetNetState();
	}
}

// ---------------------------------------------------------------------------- grid

EBlockKind ABlockIsland::GetBlock(int32 X, int32 Z) const
{
	const EBlockKind* Kind = Cells.Find(FIntPoint(X, Z));
	return Kind ? *Kind : EBlockKind::None;
}

void ABlockIsland::SetBlock(int32 X, int32 Z, EBlockKind Kind)
{
	if (Kind == EBlockKind::None)
	{
		Cells.Remove(FIntPoint(X, Z));
	}
	else
	{
		Cells.Add(FIntPoint(X, Z), Kind);
	}
}

FVector ABlockIsland::CellCenter(int32 X, int32 Z, float Y) const
{
	return FVector((X + 0.5f) * CellSize, Y, (Z + 0.5f) * CellSize);
}

void ABlockIsland::FillColumn(int32 X, int32 TopZ, int32 Depth)
{
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		EBlockKind Kind = EBlockKind::Rock;
		if (Index == 0)
		{
			Kind = EBlockKind::Snow;
		}
		else if (Depth > 3 && Index >= Depth - 2)
		{
			Kind = EBlockKind::DeepRock;
		}
		SetBlock(X, TopZ - Index, Kind);
	}
}

void ABlockIsland::GenerateCells()
{
	Cells.Reset();
	// Everything added at runtime belongs to the old layout: ice walls, icebergs, sand (shaking and falling too).
	TempCells.Reset();
	AddedCells.Reset();
	SandCells.Reset();
	SandBlocks.Reset();
	SandHiddenCells.Reset();
	BigBlocks.Reset();
	BigBlockOfCell.Reset();
	RisingCells.Reset();
	bSandInstancesDirty = true;
	SpawnPointsLocal.Reset();
	// Every generator draws only from its own seeded stream: the same Layout + Seed gives the same blocks on every PC.
	switch (Layout)
	{
	case EIslandLayout::MovementTest: GenerateMovementTest(); break;
	case EIslandLayout::Random: GenerateRandom(); break;
	case EIslandLayout::Flat: GenerateFlat(); break;
	case EIslandLayout::Arena: GenerateArena(); break;
	case EIslandLayout::Lobby: GenerateLobby(); break;
	}
	bCellsValid = true;
	BaseCells = Cells;
	BaseLayout = Layout;
	BaseSeed = Seed;
	BaseRandomWidth = RandomWidth;
	BaseArenaPenguins = ArenaPenguins;
	bHasBase = true;
	MarkCellsChanged();
	// The build limit's fixed frame (PinguBuildLimit): the generated blocks and their starts' height range, as the stage camera
	// frames a fresh island (APenguinCamera::GetIslandFrame).
	BaseLocalBounds = FBox(ForceInit);
	for (const TPair<FIntPoint, EBlockKind>& Pair : BaseCells)
	{
		const FVector Min(Pair.Key.X * CellSize, 0.f, Pair.Key.Y * CellSize);
		BaseLocalBounds += Min;
		BaseLocalBounds += Min + FVector(CellSize, 0.f, CellSize);
	}
	const float BaseTopZ = BaseLocalBounds.IsValid ? static_cast<float>(GetActorLocation().Z + BaseLocalBounds.Max.Z) : 0.f;
	const float BaseBottomZ = BaseLocalBounds.IsValid ? static_cast<float>(GetActorLocation().Z + BaseLocalBounds.Min.Z) : 0.f;
	BaseGroundMinZ = BaseTopZ;
	BaseGroundMaxZ = BaseBottomZ;
	for (const FVector& Stand : GetSpreadSpawnLocations(12))
	{
		BaseGroundMinZ = FMath::Min(BaseGroundMinZ, static_cast<float>(Stand.Z));
		BaseGroundMaxZ = FMath::Max(BaseGroundMaxZ, static_cast<float>(Stand.Z));
	}
	if (BaseGroundMinZ > BaseGroundMaxZ)
	{
		BaseGroundMinZ = BaseGroundMaxZ = BaseTopZ;
	}
}

void ABlockIsland::GenerateMovementTest()
{
	// Grid row Z covers z = [Z*100, (Z+1)*100]; the main walkable surface is at z = 0 (top of row -1).
	FRandomStream Rand(7);
	for (int32 X = -14; X <= 13; ++X)
	{
		if (X == -6 || X == -5)
		{
			continue; // 2-block pit: jump it (coyote time helps)
		}
		const float T = FMath::Abs((X + 0.5f) / 14.5f);
		const int32 Depth = 2 + FMath::RoundToInt((1.f - T * T) * 4.f) + Rand.RandRange(0, 1);
		int32 TopZ = -1;
		if (X >= 4 && X <= 7)
		{
			TopZ = 0; // one-block step
		}
		else if (X >= 8)
		{
			TopZ = 1; // two-block step
		}
		FillColumn(X, TopZ, Depth + TopZ + 1);
		if (X <= -9)
		{
			SetBlock(X, TopZ, EBlockKind::Ice); // slippery strip on the left
		}
	}

	auto Platform = [this](int32 FromX, int32 ToX, int32 Z, EBlockKind Kind)
	{
		for (int32 X = FromX; X <= ToX; ++X)
		{
			SetBlock(X, Z, Kind);
		}
	};
	Platform(-13, -10, 3, EBlockKind::Snow); // over the ice, top at 4 m
	Platform(-4, -2, 3, EBlockKind::Snow);   // needs the double jump, top at 4 m
	Platform(1, 3, 6, EBlockKind::Ice);      // icy ledge up high, top at 7 m
	Platform(5, 7, 4, EBlockKind::Snow);     // over the first step, top at 5 m
	Platform(10, 12, 5, EBlockKind::Snow);   // over the second step, top at 6 m

	ComputeSpawnPoints({ -1, 9, -8, 3 });
}

void ABlockIsland::GenerateFlat()
{
	for (int32 X = -30; X <= 29; ++X)
	{
		FillColumn(X, -1, 3);
	}
	ComputeSpawnPoints({ -20, 0, 10, 20 });
}

void ABlockIsland::GenerateRandom()
{
	FRandomStream Rand(Seed);
	const int32 Width = FMath::Clamp(RandomWidth + Rand.RandRange(-3, 3), 10, 80);
	const int32 X0 = -Width / 2;

	// Top surface: gentle random walk around z = 0.
	TArray<int32> Tops;
	Tops.SetNum(Width);
	int32 Top = -1;
	for (int32 Index = 0; Index < Width; ++Index)
	{
		if (Index > 2 && Rand.FRand() < 0.22f)
		{
			const float DownChance = Top > -1 ? 0.65f : 0.35f;
			Top = FMath::Clamp(Top + (Rand.FRand() < DownChance ? -1 : 1), -3, 2);
		}
		Tops[Index] = Top;
	}

	// Gaps to jump over.
	TSet<int32> Gaps;
	const int32 NumGaps = Width >= 18 ? Rand.RandRange(0, 2) : Rand.RandRange(0, 1);
	for (int32 Gap = 0; Gap < NumGaps; ++Gap)
	{
		const int32 Start = Rand.RandRange(3, FMath::Max(3, Width - 5));
		const int32 GapWidth = Rand.RandRange(1, 2);
		for (int32 Offset = 0; Offset < GapWidth; ++Offset)
		{
			Gaps.Add(Start + Offset);
		}
	}

	// Columns, deeper in the middle so the island tapers underneath.
	for (int32 Index = 0; Index < Width; ++Index)
	{
		if (Gaps.Contains(Index))
		{
			continue;
		}
		const float T = FMath::Abs((Index + 0.5f) / Width * 2.f - 1.f);
		const int32 Depth = 1 + FMath::RoundToInt((1.f - T * T) * (3.f + Rand.FRand() * 3.f)) + FMath::Max(0, Tops[Index] + 1);
		FillColumn(X0 + Index, Tops[Index], FMath::Max(Depth, 1));
	}

	// Ice patches.
	const int32 NumIce = Rand.RandRange(0, 2);
	for (int32 Patch = 0; Patch < NumIce; ++Patch)
	{
		const int32 Start = Rand.RandRange(0, Width - 2);
		const int32 Length = Rand.RandRange(2, 5);
		for (int32 Index = Start; Index < FMath::Min(Width, Start + Length); ++Index)
		{
			if (!Gaps.Contains(Index))
			{
				SetBlock(X0 + Index, Tops[Index], EBlockKind::Ice);
			}
		}
	}

	// Floating platforms with room around them.
	const int32 NumPlatforms = Rand.RandRange(3, 6);
	for (int32 Placed = 0, Tries = 0; Placed < NumPlatforms && Tries < 80; ++Tries)
	{
		const int32 PlatformWidth = Rand.RandRange(2, 5);
		const int32 StartIndex = Rand.RandRange(0, FMath::Max(0, Width - PlatformWidth));
		int32 Ground = -10;
		for (int32 Index = StartIndex; Index < StartIndex + PlatformWidth; ++Index)
		{
			Ground = FMath::Max(Ground, Tops[FMath::Clamp(Index, 0, Width - 1)]);
		}
		const int32 Z = Ground + Rand.RandRange(3, 6);
		bool bFree = true;
		for (int32 X = X0 + StartIndex - 1; X <= X0 + StartIndex + PlatformWidth && bFree; ++X)
		{
			for (int32 CheckZ = Z - 2; CheckZ <= Z + 2 && bFree; ++CheckZ)
			{
				bFree = GetBlock(X, CheckZ) == EBlockKind::None;
			}
		}
		if (!bFree)
		{
			continue;
		}
		const EBlockKind Kind = Rand.FRand() < 0.2f ? EBlockKind::Ice : EBlockKind::Snow;
		for (int32 Index = StartIndex; Index < StartIndex + PlatformWidth; ++Index)
		{
			SetBlock(X0 + Index, Z, Kind);
		}
		if (PlatformWidth >= 4 && Rand.FRand() < 0.5f)
		{
			for (int32 Index = StartIndex + 1; Index < StartIndex + PlatformWidth - 1; ++Index)
			{
				SetBlock(X0 + Index, Z - 1, EBlockKind::Rock);
			}
		}
		++Placed;
	}

	TArray<int32> Preferred;
	for (const float Fraction : { 0.3f, 0.7f, 0.15f, 0.85f })
	{
		Preferred.Add(X0 + FMath::RoundToInt(Fraction * (Width - 1)));
	}
	ComputeSpawnPoints(Preferred);
}

void ABlockIsland::ComputeSpawnPoints(const TArray<int32>& PreferredColumns)
{
	SpawnPointsLocal.Reset();
	if (Cells.Num() == 0)
	{
		return;
	}
	int32 MinZ = TNumericLimits<int32>::Max();
	int32 MaxZ = TNumericLimits<int32>::Lowest();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		MinZ = FMath::Min(MinZ, Pair.Key.Y);
		MaxZ = FMath::Max(MaxZ, Pair.Key.Y);
	}

	TSet<int32> UsedColumns;
	for (const int32 Column : PreferredColumns)
	{
		bool bPlaced = false;
		for (int32 Offset = 0; Offset <= 8 && !bPlaced; ++Offset)
		{
			for (const int32 Sign : { 1, -1 })
			{
				const int32 X = Column + Offset * Sign;
				if (UsedColumns.Contains(X))
				{
					continue;
				}
				// Lowest surface in this column with two free cells above it.
				for (int32 Z = MinZ; Z <= MaxZ; ++Z)
				{
					if (GetBlock(X, Z) != EBlockKind::None && GetBlock(X, Z + 1) == EBlockKind::None && GetBlock(X, Z + 2) == EBlockKind::None)
					{
						SpawnPointsLocal.Add(FVector((X + 0.5f) * CellSize, 0.f, (Z + 1) * CellSize + BlockIslandPrivate::SpawnHeightAboveSurface));
						UsedColumns.Add(X);
						bPlaced = true;
						break;
					}
				}
				if (bPlaced)
				{
					break;
				}
			}
		}
	}
}

TArray<FVector> ABlockIsland::GetSpawnLocations()
{
	if (!bCellsValid)
	{
		GenerateCells();
	}
	TArray<FVector> Result;
	const FTransform& ActorTransform = GetActorTransform();
	for (const FVector& Local : SpawnPointsLocal)
	{
		FVector World = ActorTransform.TransformPosition(Local);
		World.Y = 0.f; // the play plane
		Result.Add(World);
	}
	return Result;
}

TArray<FVector> ABlockIsland::GetSpreadSpawnLocations(int32 Count, bool bFlatOnly)
{
	using namespace BlockIslandPrivate;
	if (!bCellsValid)
	{
		GenerateCells();
	}
	TArray<FVector> Result;
	if (Count <= 0 || Cells.Num() == 0)
	{
		return Result;
	}
	int32 MinX = TNumericLimits<int32>::Max();
	int32 MaxX = TNumericLimits<int32>::Lowest();
	int32 MinZ = TNumericLimits<int32>::Max();
	int32 MaxZ = TNumericLimits<int32>::Lowest();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		MinX = FMath::Min(MinX, Pair.Key.X);
		MaxX = FMath::Max(MaxX, Pair.Key.X);
		MinZ = FMath::Min(MinZ, Pair.Key.Y);
		MaxZ = FMath::Max(MaxZ, Pair.Key.Y);
	}
	// Each column's place to stand, left to right: its lowest surface with two free cells above (the ground, not a
	// floating platform over it).
	TArray<FIntPoint> Stands;
	for (int32 X = MinX; X <= MaxX; ++X)
	{
		for (int32 Z = MinZ; Z <= MaxZ; ++Z)
		{
			if (GetBlock(X, Z) != EBlockKind::None && GetBlock(X, Z + 1) == EBlockKind::None && GetBlock(X, Z + 2) == EBlockKind::None)
			{
				Stands.Add(FIntPoint(X, Z));
				break;
			}
		}
	}
	if (Stands.Num() == 0)
	{
		return Result;
	}
	if (bFlatOnly)
	{
		// Not against a wall (2026-11-07, a lobby penguin stood against a one-block step and could not walk): the column's
		// neighbours on both sides have the same ground. Only when enough such places are left.
		TArray<FIntPoint> Flat;
		for (int32 Index = 1; Index + 1 < Stands.Num(); ++Index)
		{
			const FIntPoint& Before = Stands[Index - 1];
			const FIntPoint& After = Stands[Index + 1];
			if (Before.X == Stands[Index].X - 1 && After.X == Stands[Index].X + 1 && Before.Y == Stands[Index].Y && After.Y == Stands[Index].Y)
			{
				Flat.Add(Stands[Index]);
			}
		}
		if (Flat.Num() >= Count)
		{
			Stands = Flat;
		}
	}
	// Evenly from one end to the other, a couple of blocks in from the edges when the island is wide enough for that and
	// two blocks between neighbours; the nearest free column to each of those places.
	const int32 Span = Stands.Last().X - Stands[0].X;
	const int32 Margin = Span >= 2 * SpawnEdgeMargin + 2 * (Count - 1) ? SpawnEdgeMargin : 0;
	const float Lo = static_cast<float>(Stands[0].X + Margin);
	const float Hi = static_cast<float>(Stands.Last().X - Margin);
	TArray<bool> Used;
	Used.Init(false, Stands.Num());
	const FTransform& ActorTransform = GetActorTransform();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Want = Count > 1 ? FMath::Lerp(Lo, Hi, static_cast<float>(Index) / static_cast<float>(Count - 1)) : (Lo + Hi) * 0.5f;
		int32 Best = INDEX_NONE;
		for (int32 Candidate = 0; Candidate < Stands.Num(); ++Candidate)
		{
			if (!Used[Candidate] && (Best == INDEX_NONE || FMath::Abs(Stands[Candidate].X - Want) < FMath::Abs(Stands[Best].X - Want)))
			{
				Best = Candidate;
			}
		}
		if (Best == INDEX_NONE)
		{
			break;
		}
		Used[Best] = true;
		FVector World = ActorTransform.TransformPosition(FVector((Stands[Best].X + 0.5f) * CellSize, 0.f, (Stands[Best].Y + 1) * CellSize + SpawnHeightAboveSurface));
		World.Y = 0.f; // the play plane
		Result.Add(World);
	}
	Result.Sort([](const FVector& A, const FVector& B) { return A.X < B.X; });
	return Result;
}

// ---------------------------------------------------------------------------- visuals

UMaterialInstanceDynamic* ABlockIsland::MakeColorMaterial(const FLinearColor& Color, float Roughness, float Emissive, int32 Kind, UMaterialInterface* Parent)
{
	UMaterialInterface* const Base = Parent ? Parent : UsedMaterial.Get();
	if (!Base)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, this);
	Material->SetFlags(RF_Transient);
	Material->SetVectorParameterValue(TEXT("Color"), Color);
	Material->SetScalarParameterValue(TEXT("Roughness"), Roughness);
	Material->SetScalarParameterValue(TEXT("Emissive"), Emissive);
	Material->SetScalarParameterValue(TEXT("Kind"), static_cast<float>(Kind));
	Materials.Add(Material);
	return Material;
}

void ABlockIsland::SetupVisuals()
{
	using namespace BlockIslandPrivate;
	// Meshes and material instances: once (and in the editor after changes). New rounds and edits reuse them.
	bVisualSetupDirty = false;
	ResolveAssets();
	Materials.Reset();
	// Optional per-kind art (meshes / materials in /Game/Environment); without it every kind uses SM_Block / M_Block.
	const auto KindMesh = [this](const TCHAR* Name) -> UStaticMesh*
	{
		UStaticMesh* Mesh = LoadOptionalArt<UStaticMesh>(Name);
		return Mesh ? Mesh : UsedBlockMesh.Get();
	};
	UStaticMesh* const IceMesh = KindMesh(TEXT("SM_Block_Ice"));
	UStaticMesh* const SnowMesh = KindMesh(TEXT("SM_Block_Snow"));
	UStaticMesh* const RockMesh = KindMesh(TEXT("SM_Block_Rock"));
	UStaticMesh* const DeepRockMesh = KindMesh(TEXT("SM_Block_DeepRock"));
	SnowBlocks->SetStaticMesh(SnowMesh);
	RockBlocks->SetStaticMesh(RockMesh);
	DeepRockBlocks->SetStaticMesh(DeepRockMesh);
	IceBlocks->SetStaticMesh(IceMesh);
	TempBlocks->SetStaticMesh(IceMesh);
	BigIceBlocks->SetStaticMesh(IceMesh);
	CrumbleBlocks->SetStaticMesh(UsedBlockMesh);
	SnowCaps->SetStaticMesh(UsedSnowCapMesh);
	UMaterialInstanceDynamic* const SnowMaterial = MakeColorMaterial(SnowColor, 0.75f, 0.f, LookSnow, LoadOptionalArt<UMaterialInterface>(TEXT("M_Block_Snow")));
	UMaterialInstanceDynamic* const RockMaterial = MakeColorMaterial(RockColor, 0.9f, 0.f, LookRock, LoadOptionalArt<UMaterialInterface>(TEXT("M_Block_Rock")));
	UMaterialInstanceDynamic* const DeepRockMaterial = MakeColorMaterial(DeepRockColor, 0.95f, 0.f, LookDeepRock, LoadOptionalArt<UMaterialInterface>(TEXT("M_Block_DeepRock")));
	UMaterialInstanceDynamic* const AbilityIceMaterial = MakeColorMaterial(IceColor * 1.15f, 0.05f, 0.45f, LookTempIce, LoadOptionalArt<UMaterialInterface>(TEXT("M_Block_TempIce")));
	CrumbleBlocks->SetMaterial(0, MakeColorMaterial(FLinearColor(0.95f, 0.42f, 0.16f), 0.6f, 0.55f, LookCrumble, LoadOptionalArt<UMaterialInterface>(TEXT("M_Block_Crumble"))));
	SnowBlocks->SetMaterial(0, SnowMaterial);
	RockBlocks->SetMaterial(0, RockMaterial);
	DeepRockBlocks->SetMaterial(0, DeepRockMaterial);
	IceBlocks->SetMaterial(0, MakeColorMaterial(IceColor, 0.08f, 0.25f, LookIce, LoadOptionalArt<UMaterialInterface>(TEXT("M_Block_Ice"))));
	TempBlocks->SetMaterial(0, AbilityIceMaterial);
	BigIceBlocks->SetMaterial(0, AbilityIceMaterial);
	SnowCaps->SetMaterial(0, MakeColorMaterial(CapColor, 0.65f, 0.04f, LookSnowCap, LoadOptionalArt<UMaterialInterface>(TEXT("M_SnowCap"))));
	// Falling sand looks like the block it becomes (sand is always ability-made: its ice has the ability look).
	UStaticMesh* const SandMeshes[SandSlotCount] = { SnowMesh, RockMesh, DeepRockMesh, IceMesh };
	UMaterialInterface* const SandMaterials[SandSlotCount] = { SnowMaterial, RockMaterial, DeepRockMaterial, AbilityIceMaterial };
	for (int32 Slot = 0; Slot < FallingBlocks.Num() && Slot < SandSlotCount; ++Slot)
	{
		if (FallingBlocks[Slot])
		{
			FallingBlocks[Slot]->SetStaticMesh(SandMeshes[Slot]);
			FallingBlocks[Slot]->SetMaterial(0, SandMaterials[Slot]);
		}
	}
	bSandInstancesDirty = true;

	UsedBackdropMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_Iceberg"));
	UsedOceanMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_Ocean"));
	BackdropMaterial = MakeColorMaterial(FLinearColor(0.30f, 0.42f, 0.58f), 0.6f, 0.f, LookBackdrop, LoadOptionalArt<UMaterialInterface>(TEXT("M_Backdrop")));
	// The newer background (each part only when its art is there).
	UsedPeakMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_Iceberg_Peak"));
	UsedDomeMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_Iceberg_Dome"));
	UsedFloeMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_Floe"));
	UsedMountainMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_Mountains"));
	UsedCloudMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_CloudRing"));
	UsedFoamMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	IcebergMaterial = nullptr;
	FloeMaterial = nullptr;
	if (UMaterialInterface* Berg = LoadOptionalArt<UMaterialInterface>(TEXT("M_Iceberg")))
	{
		IcebergMaterial = UMaterialInstanceDynamic::Create(Berg, this);
		IcebergMaterial->SetFlags(RF_Transient);
		Materials.Add(IcebergMaterial);
		FloeMaterial = UMaterialInstanceDynamic::Create(Berg, this);
		FloeMaterial->SetFlags(RF_Transient);
		FloeMaterial->SetScalarParameterValue(TEXT("Bob"), 1.f); // floes bob on the swell
		FloeMaterial->SetVectorParameterValue(TEXT("Tint"), FLinearColor(0.84f, 0.92f, 1.f)); // old sea ice, a little blue
		Materials.Add(FloeMaterial);
	}
	FoamMaterial = LoadOptionalArt<UMaterialInterface>(TEXT("M_IceFoam"));
	MountainMaterial = LoadOptionalArt<UMaterialInterface>(TEXT("M_Mountain"));
	CloudMaterial = LoadOptionalArt<UMaterialInterface>(TEXT("M_CloudRing"));
	SkyMaterial = LoadOptionalArt<UMaterialInterface>(TEXT("M_SkyRing"));
	OceanSurfaceMaterial = nullptr;
	if (UMaterialInterface* Sea = OceanMaterial.LoadSynchronous())
	{
		OceanSurfaceMaterial = UMaterialInstanceDynamic::Create(Sea, this);
		OceanSurfaceMaterial->SetFlags(RF_Transient);
		OceanSurfaceMaterial->SetVectorParameterValue(TEXT("Color"), OceanColor);
		OceanSurfaceMaterial->SetScalarParameterValue(TEXT("Kind"), static_cast<float>(LookOcean));
		Materials.Add(OceanSurfaceMaterial);
	}
	else
	{
		OceanSurfaceMaterial = MakeColorMaterial(OceanColor, 0.06f, 0.f, LookOcean);
	}

	// Every instance component gets rebuilt, the backdrop too.
	VisualCellKeys.Reset();
	VisualCellKeys.SetNum(VisCount);
	bVisualsForceAll = true;
	bBackdropBuilt = false;
}

void ABlockIsland::BuildVisuals()
{
	using namespace BlockIslandPrivate;
	bVisualsDirty = false;
	if (bVisualSetupDirty || !UsedBlockMesh)
	{
		SetupVisuals();
	}
	if (VisualCellKeys.Num() != VisCount)
	{
		VisualCellKeys.Reset();
		VisualCellKeys.SetNum(VisCount);
	}
	UInstancedStaticMeshComponent* const Components[VisCount] = { SnowBlocks, RockBlocks, DeepRockBlocks, IceBlocks, TempBlocks, CrumbleBlocks, SnowCaps, BigIceBlocks };

	// Which cells every component shows (shaking sand is drawn by FallingBlocks; crumbling beats temporary and ability ice
	// beats the block kind; caps on exposed tops; a big block is one instance, by its bottom-left cell).
	if (VisualListScratch.Num() != VisCount)
	{
		VisualListScratch.SetNum(VisCount);
	}
	TArray<int32>* Lists = VisualListScratch.GetData();
	for (int32 Slot = 0; Slot < VisCount; ++Slot)
	{
		Lists[Slot].Reset();
	}
	const bool bCaps = UsedSnowCapMesh != nullptr;
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		if (SandHiddenCells.Contains(Pair.Key))
		{
			continue;
		}
		const int32 Packed = PackCell(Pair.Key);
		if (Crumbling.Contains(Pair.Key))
		{
			Lists[VisCrumble].Add(Packed);
			continue;
		}
		if (const FIntPoint* BigOrigin = BigBlockOfCell.Find(Pair.Key))
		{
			if (*BigOrigin == Pair.Key)
			{
				Lists[VisBig].Add(Packed);
			}
			continue;
		}
		// Ice made by abilities keeps its bright look whether it melts or not.
		if (TempCells.Contains(Pair.Key) || (Pair.Value == EBlockKind::Ice && AddedCells.Contains(Pair.Key)))
		{
			Lists[VisTemp].Add(Packed);
			continue;
		}
		switch (Pair.Value)
		{
		case EBlockKind::Snow: Lists[VisSnow].Add(Packed); break;
		case EBlockKind::Rock: Lists[VisRock].Add(Packed); break;
		case EBlockKind::DeepRock: Lists[VisDeepRock].Add(Packed); break;
		case EBlockKind::Ice: Lists[VisIce].Add(Packed); break;
		default: break;
		}
		if (bCaps && Pair.Value != EBlockKind::Ice && GetBlock(Pair.Key.X, Pair.Key.Y + 1) == EBlockKind::None)
		{
			Lists[VisCaps].Add(Packed);
		}
	}

	// Only the components whose cells changed get new instances (a crater does not touch the ice walls, ...).
	const bool bForceAll = bVisualsForceAll;
	bVisualsForceAll = false;
	if (bForceAll)
	{
		bSandInstancesDirty = true; // new meshes, cell size or depth: the falling sand too
	}
	const float Scale = CellSize / 100.f;
	const int32 Rows = FMath::Max(DepthRows, 1);
	for (int32 Slot = 0; Slot < VisCount; ++Slot)
	{
		if (!bForceAll && Lists[Slot] == VisualCellKeys[Slot])
		{
			continue;
		}
		VisualCellKeys[Slot] = Lists[Slot];
		TransformScratch.Reset();
		TArray<int32> LookSeeds;
		if (Slot == VisBig)
		{
			// One scaled-up block per big block (a few along the depth axis), on the cells it covers.
			BigInstanceOrigins.Reset();
			const int32 Instances = GetDepthInstances(BigBlockCells);
			for (const int32 Packed : Lists[Slot])
			{
				const FIntPoint Origin = UnpackCell(Packed);
				for (int32 Index = 0; Index < Instances; ++Index)
				{
					TransformScratch.Add(MakeBlockInstance(Origin, BigBlockCells, Index, Origin.Y * CellSize, BigBlockCells * CellSize));
					BigInstanceOrigins.Add(Origin);
					LookSeeds.Add(StableLookSeed(Origin, Index, Seed));
				}
			}
			Components[Slot]->ClearInstances();
			if (TransformScratch.Num() > 0)
			{
				ApplyStableLookSeeds(Components[Slot], LookSeeds);
				Components[Slot]->AddInstances(TransformScratch, false);
			}
			continue;
		}
		TArray<FIntPoint>* InstanceCells = Slot == VisTemp ? &TempInstanceCells : (Slot == VisCrumble ? &CrumbleInstanceCells : nullptr);
		if (InstanceCells)
		{
			InstanceCells->Reset();
		}
		for (const int32 Packed : Lists[Slot])
		{
			const FIntPoint Cell = UnpackCell(Packed);
			for (int32 Row = 0; Row < Rows; ++Row)
			{
				const float Y = (Row - (Rows - 1) * 0.5f) * CellSize;
				if (Slot == VisCaps)
				{
					const bool bFlip = ((static_cast<uint32>(Cell.X) * 73856093u) ^ (static_cast<uint32>(Cell.Y) * 19349663u) ^ (static_cast<uint32>(Row) * 83492791u)) & 1u;
					TransformScratch.Add(FTransform(FRotator(0.f, bFlip ? 180.f : 0.f, 0.f), CellCenter(Cell.X, Cell.Y, Y), FVector(Scale)));
				}
				else
				{
					TransformScratch.Add(FTransform(FRotator::ZeroRotator, CellCenter(Cell.X, Cell.Y, Y), FVector(Scale)));
				}
				LookSeeds.Add(StableLookSeed(Cell, Row, Seed));
				if (InstanceCells)
				{
					InstanceCells->Add(Cell);
				}
			}
		}
		Components[Slot]->ClearInstances();
		if (TransformScratch.Num() > 0)
		{
			ApplyStableLookSeeds(Components[Slot], LookSeeds);
			Components[Slot]->AddInstances(TransformScratch, false);
		}
	}
	BuildBackdrop();
}

void ABlockIsland::BuildBackdrop()
{
	using namespace BlockIslandPrivate;
	UInstancedStaticMeshComponent* const Parts[] = { Backdrop, Ocean, BackdropPeaks, BackdropDomes, Floes, IceFoam, Mountains };
	if (!bBuildBackdrop || !UsedBlockMesh)
	{
		for (UInstancedStaticMeshComponent* Part : Parts)
		{
			if (Part && Part->GetInstanceCount() > 0)
			{
				Part->ClearInstances();
			}
		}
		for (UStaticMeshComponent* Ring : { CloudRing.Get(), SkyRing.Get() })
		{
			if (Ring)
			{
				Ring->SetVisibility(false);
			}
		}
		bBackdropBuilt = false;
		return;
	}
	if (bBackdropBuilt && BackdropSeed == Seed)
	{
		return; // visual only, from the seed: the same seed gives the same icebergs
	}
	bBackdropBuilt = true;
	BackdropSeed = Seed;
	for (UInstancedStaticMeshComponent* Part : Parts)
	{
		Part->ClearInstances();
	}
	const float SeaZ = SeaLevel - SeaRise + SeaSurfaceAboveLevel; // (a risen sea: its components carry the rise, SetSeaRise)
	const FBox IslandBounds = GetBounds();
	const float MidX = IslandBounds.IsValid ? static_cast<float>(IslandBounds.GetCenter().X - GetActorLocation().X) : 0.f;
	UMaterialInterface* const BergMaterial = IcebergMaterial ? static_cast<UMaterialInterface*>(IcebergMaterial) : static_cast<UMaterialInterface*>(BackdropMaterial);
	// The tabular bergs: the eroded slab of Tools/make_backdrop_fbx.py when it is there (else the old block-like one,
	// stacked with a smaller chunk on top).
	UStaticMesh* const TabularMesh = LoadOptionalArt<UStaticMesh>(TEXT("SM_Iceberg_Tabular"));
	Backdrop->SetStaticMesh(TabularMesh ? TabularMesh : (UsedBackdropMesh ? UsedBackdropMesh.Get() : UsedBlockMesh.Get()));
	Ocean->SetStaticMesh(UsedOceanMesh ? UsedOceanMesh.Get() : UsedBlockMesh.Get());
	Backdrop->SetMaterial(0, BergMaterial);
	Ocean->SetMaterial(0, OceanSurfaceMaterial);

	// The sea, out to the horizon (5 km; the mountains stand in it, between them it runs on to the sky).
	Ocean->AddInstance(FTransform(FRotator::ZeroRotator, FVector(0.f, -247000.f, SeaLevel - SeaRise), FVector(10000.f, 5060.f, 1.f)));

	// Icebergs behind the island: the old tabular ones, peaked and domed ones (when their meshes exist), each with foam
	// round its waterline.
	const bool bFoam = UsedFoamMesh && FoamMaterial;
	TArray<FTransform> Tabular, Peaks, Domes, Foam, FloeList;
	FRandomStream Rand(Seed * 31 + 5);
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const float Y = -Rand.FRandRange(22000.f, 50000.f);
		const float X = MidX + Rand.FRandRange(-16000.f, 16000.f) * (-Y / 14000.f);
		const float Far = -Y / 18000.f; // the further, the bigger (they still look smaller)
		const FRotator Rotation(0.f, Rand.FRandRange(-25.f, 25.f), 0.f);
		const float Pick = Rand.FRand();
		const int32 Kind = (Pick < (TabularMesh ? 0.25f : 0.15f) || (!UsedPeakMesh && !UsedDomeMesh)) ? 0 : ((Pick < 0.6f && UsedPeakMesh) || !UsedDomeMesh ? 1 : 2);
		FVector Size;
		if (Kind == 1)
		{
			Size = FVector(Rand.FRandRange(1400.f, 3200.f), Rand.FRandRange(1200.f, 2800.f), Rand.FRandRange(1100.f, 2400.f) * Far);
		}
		else if (Kind == 2)
		{
			Size = FVector(Rand.FRandRange(1800.f, 4200.f), Rand.FRandRange(1500.f, 3400.f), Rand.FRandRange(700.f, 1600.f) * Far);
		}
		else
		{
			Size = TabularMesh
				? FVector(Rand.FRandRange(1500.f, 5000.f), Rand.FRandRange(1200.f, 3500.f), Rand.FRandRange(700.f, 1800.f) * Far)
				: FVector(Rand.FRandRange(1500.f, 5000.f), Rand.FRandRange(1200.f, 3500.f), Rand.FRandRange(400.f, 1500.f) * Far);
		}
		// About a fifth of a berg is under water (its cliffs meet the sea); the old block-like tabular ones sit 3 m deep.
		const float Sink = (Kind == 0 && !TabularMesh) ? 300.f : Size.Z * 0.2f;
		const FVector Center(X, Y, SeaZ + Size.Z * 0.5f - Sink);
		const FTransform Berg(Rotation, Center, Size / 100.f);
		(Kind == 1 ? Peaks : (Kind == 2 ? Domes : Tabular)).Add(Berg);
		const bool bTopChunk = Kind == 0 && Rand.FRand() < 0.6f;
		if (bTopChunk && !TabularMesh)
		{
			// A smaller chunk on top for a blocky silhouette.
			const FVector TopSize(Size.X * Rand.FRandRange(0.35f, 0.6f), Size.Y * 0.7f, Rand.FRandRange(300.f, 900.f));
			const FVector TopCenter = Center + FVector(Rand.FRandRange(-0.2f, 0.2f) * Size.X, 0.f, Size.Z * 0.5f + TopSize.Z * 0.5f - 40.f);
			Tabular.Add(FTransform(Rotation, TopCenter, TopSize / 100.f));
		}
		if (bFoam)
		{
			Foam.Add(FTransform(Rotation, FVector(X, Y, SeaZ + 4.f), FVector(Size.X * 1.6f / 100.f, Size.Y * 1.6f / 100.f, 1.f)));
		}
	}
	// Floes: small flat ice floating nearer, behind the island (never under it, where penguins fall), bobbing.
	if (UsedFloeMesh)
	{
		for (int32 Index = 0; Index < 16; ++Index)
		{
			const float Y = -Rand.FRandRange(6500.f, 26000.f);
			const float X = MidX + Rand.FRandRange(-1.f, 1.f) * (9000.f + (-Y) * 0.55f);
			const FVector Size(Rand.FRandRange(220.f, 760.f), Rand.FRandRange(200.f, 600.f), Rand.FRandRange(45.f, 80.f));
			const FRotator Rotation(0.f, Rand.FRandRange(0.f, 360.f), 0.f);
			FloeList.Add(FTransform(Rotation, FVector(X, Y, SeaZ + 14.f - Size.Z * 0.5f), Size / 100.f));
			if (bFoam)
			{
				Foam.Add(FTransform(Rotation, FVector(X, Y, SeaZ + 3.f), FVector(Size.X * 1.75f / 100.f, Size.Y * 1.75f / 100.f, 1.f)));
			}
		}
	}
	Backdrop->AddInstances(Tabular, false);
	if (UsedPeakMesh)
	{
		BackdropPeaks->SetStaticMesh(UsedPeakMesh);
		BackdropPeaks->SetMaterial(0, BergMaterial);
		BackdropPeaks->AddInstances(Peaks, false);
	}
	if (UsedDomeMesh)
	{
		BackdropDomes->SetStaticMesh(UsedDomeMesh);
		BackdropDomes->SetMaterial(0, BergMaterial);
		BackdropDomes->AddInstances(Domes, false);
	}
	if (UsedFloeMesh)
	{
		Floes->SetStaticMesh(UsedFloeMesh);
		Floes->SetMaterial(0, FloeMaterial ? static_cast<UMaterialInterface*>(FloeMaterial) : BergMaterial);
		Floes->AddInstances(FloeList, false);
	}
	if (bFoam)
	{
		IceFoam->SetStaticMesh(UsedFoamMesh);
		IceFoam->SetMaterial(0, FoamMaterial);
		IceFoam->AddInstances(Foam, false);
	}

	// Mountains on the horizon: a far, higher range and a nearer, lower one; gaps where the sea runs on to the sky.
	// (The camera looks 8 degrees down: only about 2 degrees of sky show above the horizon - the peaks reach 0.6 - 1.2.)
	if (UsedMountainMesh)
	{
		Mountains->SetStaticMesh(UsedMountainMesh);
		if (MountainMaterial)
		{
			Mountains->SetMaterial(0, MountainMaterial);
		}
		TArray<FTransform> Ranges;
		FRandomStream Hills(4711); // the same mountains every round
		struct FRange { float Y; float Length; float Height; float Depth; float Gap; };
		for (const FRange& Range : { FRange{ -215000.f, 230000.f, 7400.f, 60000.f, 0.15f }, FRange{ -150000.f, 150000.f, 4600.f, 40000.f, 0.35f } })
		{
			for (float X = -380000.f + Hills.FRandRange(0.f, Range.Length * 0.5f); X < 380000.f; X += Range.Length * Hills.FRandRange(0.75f, 1.2f))
			{
				if (Hills.FRand() < Range.Gap)
				{
					continue; // open sea there
				}
				const float Height = Range.Height * Hills.FRandRange(0.75f, 1.15f);
				const float Mirror = Hills.FRand() < 0.5f ? -1.f : 1.f;
				Ranges.Add(FTransform(FRotator(0.f, Hills.FRandRange(-4.f, 4.f), 0.f), FVector(MidX + X, Range.Y + Hills.FRandRange(-8000.f, 8000.f), SeaZ + Height * 0.5f - 300.f),
					FVector(Range.Length / 100.f * Mirror, Range.Depth / 100.f, Height / 100.f)));
			}
		}
		Mountains->AddInstances(Ranges, false);
	}

	// Clouds: a cirrostratus veil (M_CloudRing works out where each pixel's view ray meets a high cloud deck, so the ring's
	// own shape does not show) on a ring 3 km round the island, from half a degree above the horizon up to about 33
	// degrees: the game camera shows only its lowest degrees, the menu and close views more of it.
	if (CloudRing)
	{
		const bool bClouds = UsedCloudMesh && CloudMaterial;
		CloudRing->SetVisibility(bClouds);
		if (bClouds)
		{
			CloudRing->SetStaticMesh(UsedCloudMesh);
			CloudRing->SetMaterial(0, CloudMaterial);
			CloudRing->SetRelativeLocationAndRotation(FVector(MidX, 0.f, 101660.f), FRotator::ZeroRotator);
			CloudRing->SetRelativeScale3D(FVector(6000.f, 6000.f, 2000.f));
		}
	}
	// The sky band behind the clouds: 3.2 km out, from just below the sea's horizon (its haze hides where the sea ends)
	// up to about 12 degrees (it fades into the real sky above 5).
	if (SkyRing)
	{
		const bool bSky = UsedCloudMesh && SkyMaterial;
		SkyRing->SetVisibility(bSky);
		if (bSky)
		{
			SkyRing->SetStaticMesh(UsedCloudMesh);
			SkyRing->SetMaterial(0, SkyMaterial);
			SkyRing->SetRelativeLocationAndRotation(FVector(MidX, 0.f, 34240.f), FRotator::ZeroRotator);
			SkyRing->SetRelativeScale3D(FVector(6400.f, 6400.f, 719.f));
		}
	}
}

void ABlockIsland::UpdateMeltVisuals()
{
	if (!TempBlocks || TempCells.Num() == 0)
	{
		return;
	}
	const float Scale = CellSize / 100.f;
	const int32 Rows = FMath::Max(DepthRows, 1);
	bool bAnyMelting = false;
	for (int32 Index = 0; Index < TempInstanceCells.Num(); ++Index)
	{
		const FIntPoint Cell = TempInstanceCells[Index];
		const FTimedCell* Timed = TempCells.Find(Cell);
		if (!Timed || Timed->Left > MeltTime)
		{
			continue;
		}
		bAnyMelting = true;
		const float Alpha = FMath::Clamp(Timed->Left / FMath::Max(MeltTime, 0.01f), 0.f, 1.f);
		const int32 Row = Index % Rows;
		const float Y = (Row - (Rows - 1) * 0.5f) * CellSize;
		const float S = FMath::Lerp(0.15f, 1.f, Alpha);
		const FVector Center = CellCenter(Cell.X, Cell.Y, Y) - FVector(0.f, 0.f, (1.f - S) * CellSize * 0.5f);
		TempBlocks->UpdateInstanceTransform(Index, FTransform(FRotator::ZeroRotator, Center, FVector(Scale * S, Scale * S, Scale * S * 0.9f)), false, false, true);
	}
	if (bAnyMelting)
	{
		APinguFX::MarkInstancesChanged(TempBlocks); // just these instances go to the GPU, no new render state
	}

	// A temporary big block melts the same way, as one.
	bool bAnyBigMelting = false;
	const int32 BigInstances = GetDepthInstances(BigBlockCells);
	for (int32 Index = 0; BigIceBlocks && Index < BigInstanceOrigins.Num() && Index < BigIceBlocks->GetInstanceCount(); ++Index)
	{
		const FIntPoint Origin = BigInstanceOrigins[Index];
		const FTimedCell* Timed = TempCells.Find(Origin);
		if (!Timed || Timed->Left > MeltTime)
		{
			continue;
		}
		bAnyBigMelting = true;
		const float S = FMath::Lerp(0.15f, 1.f, FMath::Clamp(Timed->Left / FMath::Max(MeltTime, 0.01f), 0.f, 1.f));
		const float Full = BigBlockCells * CellSize;
		FTransform Melted = MakeBlockInstance(Origin, BigBlockCells, Index % FMath::Max(BigInstances, 1), Origin.Y * CellSize, Full * S * 0.9f);
		Melted.SetScale3D(Melted.GetScale3D() * FVector(S, S, 1.f));
		BigIceBlocks->UpdateInstanceTransform(Index, Melted, false, false, true);
	}
	if (bAnyBigMelting)
	{
		APinguFX::MarkInstancesChanged(BigIceBlocks);
	}
}

int32 ABlockIsland::GetDepthInstances(int32 Span) const
{
	const int32 Rows = FMath::Max(DepthRows, 1);
	// A block: one per row. A big block: as few as keep each at most BigBlockDepthCells deep (3 rows: two of 1.5).
	return Span <= 1 ? Rows : FMath::Max(1, FMath::DivideAndRoundUp(Rows, BlockIslandPrivate::BigBlockDepthCells));
}

float ABlockIsland::GetDepthInstanceCells(int32 Span) const
{
	return static_cast<float>(FMath::Max(DepthRows, 1)) / static_cast<float>(GetDepthInstances(Span));
}

float ABlockIsland::GetDepthInstanceY(int32 Span, int32 Index) const
{
	const int32 Count = GetDepthInstances(Span);
	return (Index - (Count - 1) * 0.5f) * GetDepthInstanceCells(Span) * CellSize;
}

FTransform ABlockIsland::MakeBlockInstance(const FIntPoint& Cell, int32 Span, int32 Index, float Bottom, float Height) const
{
	const float Scale = CellSize / 100.f;
	const float Width = Span * CellSize;
	const FVector Middle(Cell.X * CellSize + Width * 0.5f, GetDepthInstanceY(Span, Index), Bottom + Height * 0.5f);
	// The block mesh is a 100 cm cube: as wide and tall as Span cells (Height tall), as deep as its share of the depth.
	return FTransform(FRotator::ZeroRotator, Middle, FVector(Scale * Span, Scale * GetDepthInstanceCells(Span), Scale * Height / FMath::Max(CellSize, 1.f)));
}

void ABlockIsland::UpdateRiseVisuals()
{
	using namespace BlockIslandPrivate;
	if (RisingCells.Num() == 0)
	{
		return;
	}
	const double Now = GetNetTime();
	// How far each rising block's look has come (eased: quick at first, settling into place).
	const auto Rise = [this, Now](const FIntPoint& Cell, int32 Span, int32 Index, FTransform& Out) -> bool
	{
		const FRiseCell* Rising = RisingCells.Find(Cell);
		if (!Rising)
		{
			return false;
		}
		const float Alpha = GetRiseAlpha(Cell);
		const float Eased = FMath::Max(1.f - FMath::Pow(1.f - Alpha, 3.f), RiseMinShare);
		// The wall grows up out of its floor: every block keeps its share of the wall's height.
		const float Floor = Rising->FloorRow * CellSize;
		const float Bottom = Floor + (Cell.Y * CellSize - Floor) * Eased;
		Out = MakeBlockInstance(Cell, Span, Index, Bottom, Span * CellSize * Eased);
		return true;
	};
	const int32 Rows = FMath::Max(DepthRows, 1);
	bool bTemp = false;
	for (int32 Index = 0; TempBlocks && Index < TempInstanceCells.Num() && Index < TempBlocks->GetInstanceCount(); ++Index)
	{
		FTransform Instance;
		if (Rise(TempInstanceCells[Index], 1, Index % Rows, Instance))
		{
			TempBlocks->UpdateInstanceTransform(Index, Instance, false, false, true);
			bTemp = true;
		}
	}
	bool bBig = false;
	const int32 BigInstances = FMath::Max(GetDepthInstances(BigBlockCells), 1);
	for (int32 Index = 0; BigIceBlocks && Index < BigInstanceOrigins.Num() && Index < BigIceBlocks->GetInstanceCount(); ++Index)
	{
		FTransform Instance;
		if (Rise(BigInstanceOrigins[Index], BigBlockCells, Index % BigInstances, Instance))
		{
			BigIceBlocks->UpdateInstanceTransform(Index, Instance, false, false, true);
			bBig = true;
		}
	}
	if (bTemp)
	{
		APinguFX::MarkInstancesChanged(TempBlocks);
	}
	if (bBig)
	{
		APinguFX::MarkInstancesChanged(BigIceBlocks);
	}
	// Risen: the full block was just drawn, nothing more to do for it.
	for (auto It = RisingCells.CreateIterator(); It; ++It)
	{
		if (Now >= It.Value().Start + It.Value().Duration)
		{
			It.RemoveCurrent();
		}
	}
}

float ABlockIsland::GetRiseAlpha(const FIntPoint& Cell) const
{
	const FRiseCell* Rising = RisingCells.Find(Cell);
	if (!Rising || Rising->Duration <= 0.f)
	{
		return 1.f;
	}
	return FMath::Clamp(static_cast<float>((GetNetTime() - Rising->Start) / Rising->Duration), 0.f, 1.f);
}

void ABlockIsland::UpdateCrumbleVisuals()
{
	if (!CrumbleBlocks || CrumbleInstanceCells.Num() == 0 || CrumbleBlocks->GetInstanceCount() < CrumbleInstanceCells.Num())
	{
		return;
	}
	const float Scale = CellSize / 100.f;
	const int32 Rows = FMath::Max(DepthRows, 1);
	TransformScratch.Reset();
	for (int32 Index = 0; Index < CrumbleInstanceCells.Num(); ++Index)
	{
		const FIntPoint Cell = CrumbleInstanceCells[Index];
		const FTimedCell* Timed = Crumbling.Find(Cell);
		const float Alpha = Timed ? 1.f - FMath::Clamp(Timed->Left / FMath::Max(CollapseWarnTime, 0.01f), 0.f, 1.f) : 0.f; // 0 -> 1 toward the fall
		const int32 Row = Index % Rows;
		const float Y = (Row - (Rows - 1) * 0.5f) * CellSize;
		// Shakes harder the closer it gets to falling, and sags a little at the end.
		const float Phase = CrumbleTime * 38.f + Cell.X * 1.7f + Cell.Y * 2.3f + Row;
		const float Amp = (1.5f + 7.f * Alpha * Alpha) * Scale;
		const FVector Jitter(FMath::Sin(Phase) * Amp, 0.f, FMath::Cos(Phase * 1.3f) * Amp * 0.6f - Alpha * Alpha * 14.f * Scale);
		TransformScratch.Add(FTransform(FRotator(0.f, 0.f, FMath::Sin(Phase * 0.7f) * 4.f * Alpha), CellCenter(Cell.X, Cell.Y, Y) + Jitter,
			FVector(Scale * (1.f - 0.08f * Alpha))));
	}
	// One batched update of the shaking blocks; only their instance data goes to the GPU (no render state rebuild).
	CrumbleBlocks->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
	APinguFX::MarkInstancesChanged(CrumbleBlocks);
}

// ---------------------------------------------------------------------------- collision

void ABlockIsland::BuildCollision()
{
	using namespace BlockIslandPrivate;
	bCollisionDirty = false;
	const UWorld* MyWorld = GetWorld();
	if (!MyWorld || !MyWorld->IsGameWorld())
	{
		return;
	}

	const float Depth = FMath::Max(DepthRows, 1) * CellSize;
	if (CollisionBuiltCellSize != CellSize || CollisionBuiltDepth != Depth)
	{
		// Another cell size or depth: none of the boxes fits any more.
		for (UBoxComponent* Box : CollisionBoxes)
		{
			if (Box)
			{
				Box->DestroyComponent();
			}
		}
		CollisionBoxes.Reset();
		CollisionKeys.Reset();
		CollisionBuiltCellSize = CellSize;
		CollisionBuiltDepth = Depth;
	}

	// Greedy merge into rectangles on a dense grid (ice separately, so it can be tagged slippery):
	// bottom row first, left to right, as wide as possible, then as high as possible. Exactly the blocks, no seams.
	TArray<uint64>& NewKeys = CollisionKeyScratch;
	NewKeys.Reset();
	if (Cells.Num() > 0)
	{
		int32 MinX = TNumericLimits<int32>::Max(), MaxX = TNumericLimits<int32>::Lowest();
		int32 MinZ = TNumericLimits<int32>::Max(), MaxZ = TNumericLimits<int32>::Lowest();
		for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
		{
			MinX = FMath::Min(MinX, Pair.Key.X);
			MaxX = FMath::Max(MaxX, Pair.Key.X);
			MinZ = FMath::Min(MinZ, Pair.Key.Y);
			MaxZ = FMath::Max(MaxZ, Pair.Key.Y);
		}
		const int32 GridWidth = MaxX - MinX + 1;
		const int32 GridHeight = MaxZ - MinZ + 1;
		GridScratch.Reset();
		GridScratch.SetNumZeroed(GridWidth * GridHeight);
		uint8* Grid = GridScratch.GetData();
		for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
		{
			Grid[(Pair.Key.Y - MinZ) * GridWidth + (Pair.Key.X - MinX)] = Pair.Value == EBlockKind::Ice ? 2 : 1;
		}
		for (int32 Z = 0; Z < GridHeight; ++Z)
		{
			for (int32 X = 0; X < GridWidth; ++X)
			{
				const uint8 Class = Grid[Z * GridWidth + X];
				if (Class == 0)
				{
					continue;
				}
				int32 Width = 1;
				while (X + Width < GridWidth && Grid[Z * GridWidth + X + Width] == Class)
				{
					++Width;
				}
				int32 Height = 1;
				for (bool bGrow = true; bGrow && Z + Height < GridHeight;)
				{
					for (int32 Offset = 0; Offset < Width; ++Offset)
					{
						if (Grid[(Z + Height) * GridWidth + X + Offset] != Class)
						{
							bGrow = false;
							break;
						}
					}
					if (bGrow)
					{
						++Height;
					}
				}
				for (int32 DZ = 0; DZ < Height; ++DZ)
				{
					for (int32 DX = 0; DX < Width; ++DX)
					{
						Grid[(Z + DZ) * GridWidth + X + DX] = 0;
					}
				}
				NewKeys.Add(RectKey(MinX + X, MinZ + Z, Width, Height, Class == 2));
			}
		}
	}

	// Boxes whose rectangle is still there stay as they are (no physics work, penguins keep their floor); the
	// rest are replaced. Boxes are never moved: a moved box would carry the penguins standing on it along.
	TMap<uint64, int32>& OldIndexByKey = CollisionIndexScratch;
	OldIndexByKey.Reset();
	for (int32 Index = 0; Index < CollisionKeys.Num(); ++Index)
	{
		OldIndexByKey.Add(CollisionKeys[Index], Index);
	}
	TArray<TObjectPtr<UBoxComponent>> NewBoxes;
	NewBoxes.Reserve(NewKeys.Num());
	for (const uint64 Key : NewKeys)
	{
		if (const int32* OldIndex = OldIndexByKey.Find(Key))
		{
			if (UBoxComponent* Kept = CollisionBoxes[*OldIndex])
			{
				NewBoxes.Add(Kept);
				CollisionBoxes[*OldIndex] = nullptr;
				continue;
			}
		}
		const int32 X = static_cast<int16>(Key & 0xFFFFu);
		const int32 Z = static_cast<int16>((Key >> 16) & 0xFFFFu);
		const int32 Width = static_cast<int32>((Key >> 32) & 0x3FFFu);
		const int32 Height = static_cast<int32>((Key >> 46) & 0x3FFFu);
		const bool bIce = ((Key >> 60) & 1u) != 0;
		UBoxComponent* Box = NewObject<UBoxComponent>(this, NAME_None, RF_Transient);
		Box->SetupAttachment(Root);
		Box->SetRelativeLocation(FVector((X + Width * 0.5f) * CellSize, 0.f, (Z + Height * 0.5f) * CellSize));
		Box->SetBoxExtent(FVector(Width * CellSize * 0.5f, Depth * 0.5f, Height * CellSize * 0.5f), false);
		Box->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Box->SetGenerateOverlapEvents(false);
		Box->SetCanEverAffectNavigation(false);
		Box->SetHiddenInGame(true);
		if (bIce)
		{
			Box->ComponentTags.Add(IceTag);
		}
		Box->RegisterComponent();
		NewBoxes.Add(Box);
	}
	for (UBoxComponent* Box : CollisionBoxes)
	{
		if (Box)
		{
			Box->DestroyComponent();
		}
	}
	CollisionBoxes = MoveTemp(NewBoxes);
	CollisionKeys = NewKeys;
}

// ---------------------------------------------------------------------------- arena generator

FPinguArenaSize ABlockIsland::GetArenaSize(int32 Penguins, int32 LegacyWidth)
{
	using namespace BlockIslandPrivate;
	FPinguArenaSize Arena;
	Arena.MaxWidth = ArenaMaxWidth;
	if (Penguins <= 0)
	{
		// The old fixed arena: RandomWidth wide, the numbers it was tuned with (the struct's defaults).
		Arena.BaseWidth = LegacyWidth;
		Arena.MinWidth = LegacyArenaMinWidth;
		return Arena;
	}
	Arena.Penguins = FMath::Clamp(Penguins, ArenaFewestPenguins, PinguMaxPenguins);
	Arena.BaseWidth = ArenaWidthByPenguins[Arena.Penguins - ArenaFewestPenguins];
	Arena.MinWidth = ArenaMinWidth;
	// Everything else grows (or shrinks) with the width; at four penguins (scale 1) these are the old numbers exactly.
	const float Scale = static_cast<float>(Arena.BaseWidth) / ArenaReferenceWidth;
	Arena.Scale = Scale;
	Arena.JitterMin = -FMath::RoundToInt(ArenaJitterDown * Scale);
	Arena.JitterMax = FMath::RoundToInt(ArenaJitterUp * FMath::Min(Scale, 1.f));
	Arena.HillTopMax = Scale < SmallArenaScale ? 2 : (Scale > BigArenaScale ? 4 : 3);
	Arena.BumpsMax = FMath::Clamp(FMath::RoundToInt(ArenaBumps * Scale), ArenaBumpsFewest, ArenaBumpsMost);
	Arena.PitsMax = FMath::Max(1, FMath::RoundToInt(ArenaPits * Scale));
	Arena.IcePatchesMax = FMath::Max(1, FMath::RoundToInt(ArenaIcePatches * Scale));
	Arena.MirroredPlatformsMin = FMath::Max(1, FMath::RoundToInt(ArenaMirroredPlatformsMin * Scale));
	Arena.MirroredPlatformsMax = FMath::Max(1, FMath::RoundToInt(ArenaMirroredPlatformsMax * Scale));
	Arena.PlatformsMin = FMath::Max(2, FMath::RoundToInt(ArenaPlatformsMin * Scale));
	Arena.PlatformsMax = FMath::Max(2, FMath::RoundToInt(ArenaPlatformsMax * Scale));
	Arena.PlatformLiftMax = Scale < SmallArenaScale ? 4 : 5;
	if (Scale < 1.f)
	{
		Arena.HighPlatformChance = FMath::GetMappedRangeValueClamped(FVector2f(HighPlatformScaleNone, 1.f), FVector2f(0.f, HighPlatformChanceFour), Scale);
	}
	else if (Scale > 1.f)
	{
		Arena.HighPlatformChance = FMath::GetMappedRangeValueClamped(FVector2f(1.f, HighPlatformScaleMost), FVector2f(HighPlatformChanceFour, HighPlatformChanceMost), Scale);
	}
	Arena.PlatformTries = FMath::RoundToInt(static_cast<float>(ArenaPlatformTries) * FMath::Max(1.f, Scale));
	return Arena;
}

float ABlockIsland::GetArenaScale() const
{
	const int32 Penguins = GetArenaPenguins();
	return Penguins > 0 ? GetArenaSize(Penguins).Scale : 1.f;
}

void ABlockIsland::GenerateArena()
{
	// As big as ArenaPenguins says (the old fixed size, RandomWidth wide, when it is 0). The draws from the seed come in the
	// same order at every size: four penguins give exactly the island the old fixed size gave.
	const FPinguArenaSize Arena = GetArenaSize(ArenaPenguins, RandomWidth);
	FRandomStream Rand(Seed);
	const int32 Style = Rand.RandRange(0, 3);
	const bool bMirror = Rand.FRand() < 0.55f;
	const int32 Width = FMath::Clamp(Arena.BaseWidth + Rand.RandRange(Arena.JitterMin, Arena.JitterMax), Arena.MinWidth, Arena.MaxWidth);
	const int32 X0 = -Width / 2;

	// ---- surface: a few smooth bumps (hills and dips)
	TArray<int32> Tops;
	Tops.Init(-1, Width);
	TArray<bool> Solid;
	Solid.Init(true, Width);
	TArray<float> Height;
	Height.Init(0.f, Width);
	const int32 NumBumps = Rand.RandRange(1, Arena.BumpsMax);
	for (int32 Bump = 0; Bump < NumBumps; ++Bump)
	{
		const float Center = Rand.FRandRange(0.1f, 0.9f) * Width;
		const float BumpWidth = Rand.FRandRange(2.5f, 6.f);
		const float BumpHeight = Rand.FRandRange(-1.2f, 3.2f);
		for (int32 Index = 0; Index < Width; ++Index)
		{
			const float D = (Index + 0.5f - Center) / BumpWidth;
			Height[Index] += BumpHeight * FMath::Exp(-D * D);
		}
	}
	for (int32 Index = 0; Index < Width; ++Index)
	{
		Tops[Index] = FMath::Clamp(-1 + FMath::RoundToInt(Height[Index]), -2, Arena.HillTopMax);
	}

	switch (Style)
	{
	case 1: // twin islands with a gap in the middle
	{
		const int32 GapWidth = Rand.RandRange(2, 4);
		const int32 Start = Width / 2 - GapWidth / 2;
		for (int32 Index = Start; Index < Start + GapWidth; ++Index)
		{
			Solid[Index] = false;
		}
		break;
	}
	case 2: // terraces up to a central plateau
		for (int32 Index = 0; Index < Width; ++Index)
		{
			const float T = 1.f - FMath::Abs((Index + 0.5f) / Width * 2.f - 1.f);
			Tops[Index] = -2 + FMath::FloorToInt(T * 3.4f);
		}
		break;
	case 3: // valley: high sides, low middle
		for (int32 Index = 0; Index < Width; ++Index)
		{
			const float T = FMath::Abs((Index + 0.5f) / Width * 2.f - 1.f);
			Tops[Index] = -1 + FMath::FloorToInt(T * T * 3.6f);
		}
		break;
	default: // rolling hills with a pit or two
	{
		const int32 NumGaps = Rand.RandRange(0, Arena.PitsMax);
		for (int32 Gap = 0; Gap < NumGaps; ++Gap)
		{
			const int32 GapWidth = Rand.RandRange(1, 2);
			const int32 Start = Rand.RandRange(4, FMath::Max(4, Width - 5 - GapWidth));
			for (int32 Index = Start; Index < FMath::Min(Width, Start + GapWidth); ++Index)
			{
				Solid[Index] = false;
			}
		}
		break;
	}
	}

	if (bMirror)
	{
		for (int32 Index = 0; Index < Width / 2; ++Index)
		{
			Tops[Width - 1 - Index] = Tops[Index];
			Solid[Width - 1 - Index] = Solid[Index];
		}
	}

	// ---- the ends: most of them step down to the sea in one-block steps, so a rolling Giant Snowball can roll off. Their
	// own random stream, so everything else the seed makes stays as it was; the two ends are decided one by one.
	{
		FRandomStream EndRand(Seed ^ 0x3D6E5A1);
		const float Chance = EndStepChanceOverride >= 0.f ? EndStepChanceOverride : BlockIslandPrivate::ArenaEndStepChance;
		const int32 Steps = BlockIslandPrivate::ArenaEndStepColumns;
		for (const int32 Side : { 0, 1 })
		{
			if (EndRand.FRand() >= Chance || Width < 2 * Steps + 6)
			{
				continue;
			}
			// d = 0 is the outermost column; the column Steps in is the base the steps come down from (never a pit).
			auto ColumnAt = [&](int32 Depth) { return Side == 0 ? Depth : Width - 1 - Depth; };
			const int32 BaseTop = Tops[ColumnAt(Steps)];
			for (int32 Depth = 0; Depth < Steps; ++Depth)
			{
				const int32 Column = ColumnAt(Depth);
				Solid[Column] = true;
				Tops[Column] = FMath::Clamp(BaseTop - (Steps - Depth), BlockIslandPrivate::ArenaEndStepFloor, BaseTop);
			}
			Solid[ColumnAt(Steps)] = true;
		}
	}

	// ---- columns, deeper in the middle so the island tapers underneath
	for (int32 Index = 0; Index < Width; ++Index)
	{
		if (!Solid[Index])
		{
			continue;
		}
		const float T = FMath::Abs((Index + 0.5f) / Width * 2.f - 1.f);
		const int32 Depth = 2 + FMath::RoundToInt((1.f - T * T) * (3.f + Rand.FRand() * 2.5f)) + FMath::Max(0, Tops[Index] + 1);
		FillColumn(X0 + Index, Tops[Index], Depth);
	}

	// ---- ice patches on the surface
	const int32 NumIce = Rand.RandRange(0, Arena.IcePatchesMax);
	for (int32 Patch = 0; Patch < NumIce; ++Patch)
	{
		const int32 Start = Rand.RandRange(0, Width - 3);
		const int32 Length = Rand.RandRange(2, 4);
		for (int32 Index = Start; Index < FMath::Min(Width, Start + Length); ++Index)
		{
			for (const int32 Column : { Index, bMirror ? Width - 1 - Index : Index })
			{
				if (Solid[Column] && GetBlock(X0 + Column, Tops[Column]) != EBlockKind::None)
				{
					SetBlock(X0 + Column, Tops[Column], EBlockKind::Ice);
				}
			}
		}
	}

	// ---- floating platforms (mirrored when the island is)
	auto GroundAt = [&](int32 Index)
	{
		return (Index >= 0 && Index < Width && Solid[Index]) ? Tops[Index] : -3;
	};
	const int32 NumPlatforms = bMirror ? Rand.RandRange(Arena.MirroredPlatformsMin, Arena.MirroredPlatformsMax)
		: Rand.RandRange(Arena.PlatformsMin, Arena.PlatformsMax); // mirrored ones come in pairs
	for (int32 Placed = 0, Tries = 0; Placed < NumPlatforms && Tries < Arena.PlatformTries; ++Tries)
	{
		const int32 PlatformWidth = Rand.RandRange(2, 5);
		const int32 MaxStart = bMirror ? FMath::Max(0, Width / 2 - PlatformWidth) : FMath::Max(0, Width - PlatformWidth);
		const int32 StartIndex = Rand.RandRange(0, MaxStart);
		int32 Ground = -10;
		for (int32 Index = StartIndex; Index < StartIndex + PlatformWidth; ++Index)
		{
			Ground = FMath::Max(Ground, GroundAt(Index));
		}
		const int32 Z = Ground + Rand.RandRange(3, Arena.PlatformLiftMax) + (Rand.FRand() < Arena.HighPlatformChance ? 3 : 0);
		auto IsFree = [&](int32 FromIndex)
		{
			// Two free cells all around, so platforms never merge into a ceiling.
			for (int32 X = X0 + FromIndex - 2; X <= X0 + FromIndex + PlatformWidth + 1; ++X)
			{
				for (int32 CheckZ = Z - 2; CheckZ <= Z + 2; ++CheckZ)
				{
					if (GetBlock(X, CheckZ) != EBlockKind::None)
					{
						return false;
					}
				}
			}
			return true;
		};
		const int32 MirrorStart = Width - StartIndex - PlatformWidth;
		if (!IsFree(StartIndex) || (bMirror && !IsFree(MirrorStart)))
		{
			continue;
		}
		const EBlockKind Kind = Rand.FRand() < 0.18f ? EBlockKind::Ice : EBlockKind::Snow;
		const bool bUnderside = PlatformWidth >= 4 && Rand.FRand() < 0.5f;
		for (const int32 From : { StartIndex, MirrorStart })
		{
			for (int32 Index = From; Index < From + PlatformWidth; ++Index)
			{
				SetBlock(X0 + Index, Z, Kind);
				if (bUnderside && Index > From && Index < From + PlatformWidth - 1)
				{
					SetBlock(X0 + Index, Z - 1, EBlockKind::Rock);
				}
			}
			if (!bMirror)
			{
				break;
			}
		}
		++Placed;
	}

	TArray<int32> Preferred;
	for (const float Fraction : { 0.18f, 0.82f, 0.4f, 0.6f })
	{
		Preferred.Add(X0 + FMath::RoundToInt(Fraction * (Width - 1)));
	}
	ComputeSpawnPoints(Preferred);
}

void ABlockIsland::GenerateLobby()
{
	// The lobby: as small as the two-penguin arena or a bit smaller (12 or 13 blocks), made of the arena's own blocks - snow
	// on rock, deep rock at the bottom, deeper in the middle - and mostly flat so everybody can stand around. At its right
	// end a step two blocks wide and one high (the lobby's "+" stands on it); on the left, by the seed, a low step or a
	// little floating platform; a short patch of ice. No pits, nothing to fall through.
	FRandomStream Rand(Seed);
	const int32 Width = 12 + Rand.RandRange(0, 1);
	const int32 X0 = -Width / 2;
	TArray<int32> Tops;
	Tops.Init(-1, Width);
	Tops[Width - 1] = 0;
	Tops[Width - 2] = 0;
	const int32 LeftStyle = Rand.RandRange(0, 2); // 0: a low step at the left end, 1: a floating platform, 2: both flat
	if (LeftStyle == 0)
	{
		Tops[0] = 0;
	}
	for (int32 Index = 0; Index < Width; ++Index)
	{
		const float T = FMath::Abs((Index + 0.5f) / Width * 2.f - 1.f);
		const int32 Depth = 2 + FMath::RoundToInt((1.f - T * T) * (3.f + Rand.FRand() * 2.f)) + FMath::Max(0, Tops[Index] + 1);
		FillColumn(X0 + Index, Tops[Index], Depth);
	}
	// A short patch of ice on the flat, left of the middle (never on the steps).
	const int32 IceStart = Rand.RandRange(2, FMath::Max(2, Width / 2 - 3));
	const int32 IceLength = Rand.RandRange(2, 3);
	for (int32 Index = IceStart; Index < FMath::Min(IceStart + IceLength, Width - 3); ++Index)
	{
		if (Tops[Index] == -1)
		{
			SetBlock(X0 + Index, -1, EBlockKind::Ice);
		}
	}
	if (LeftStyle == 1 || (LeftStyle == 2 && Rand.FRand() < 0.5f))
	{
		// A little snow platform up high on the left (three blocks, top at 3 m: a jump and a double jump).
		const int32 From = Rand.RandRange(1, 2);
		for (int32 Index = From; Index < From + 3; ++Index)
		{
			SetBlock(X0 + Index, 2, EBlockKind::Snow);
		}
	}
	TArray<int32> Preferred;
	for (const float Fraction : { 0.25f, 0.6f, 0.4f, 0.12f, 0.75f, 0.5f })
	{
		Preferred.Add(X0 + FMath::RoundToInt(Fraction * (Width - 1)));
	}
	ComputeSpawnPoints(Preferred);
}

bool ABlockIsland::GetLobbyStepTop(FVector& OutTop) const
{
	if (BaseLayout != EIslandLayout::Lobby || Cells.Num() == 0)
	{
		return false;
	}
	// The right-end step: the two rightmost columns' tops (one block above the flat).
	int32 MaxX = TNumericLimits<int32>::Lowest();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		MaxX = FMath::Max(MaxX, Pair.Key.X);
	}
	int32 TopZ = TNumericLimits<int32>::Lowest();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		if (Pair.Key.X == MaxX)
		{
			TopZ = FMath::Max(TopZ, Pair.Key.Y);
		}
	}
	OutTop = GetActorLocation() + FVector(MaxX * CellSize, 0.f, (TopZ + 1) * CellSize);
	OutTop.Y = 0.f;
	return true;
}

// ---------------------------------------------------------------------------- runtime

ABlockIsland* ABlockIsland::Get(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<ABlockIsland> It(const_cast<UWorld*>(World)); It; ++It)
	{
		// The autotest's flat strip is far away; prefer the arena at the origin.
		if (FMath::Abs(It->GetActorLocation().X) < 5000.f)
		{
			return *It;
		}
	}
	for (TActorIterator<ABlockIsland> It(const_cast<UWorld*>(World)); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

ABlockIsland* ABlockIsland::FindNear(const UWorld* World, const FVector& Location)
{
	if (!World)
	{
		return nullptr;
	}
	ABlockIsland* Best = nullptr;
	double BestDistance = TNumericLimits<double>::Max();
	for (TActorIterator<ABlockIsland> It(const_cast<UWorld*>(World)); It; ++It)
	{
		const FBox Bounds = It->GetBounds();
		const double Distance = Bounds.IsValid ? FMath::Max(0.0, FMath::Max(Bounds.Min.X - Location.X, Location.X - Bounds.Max.X)) : TNumericLimits<double>::Max() * 0.5;
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = *It;
		}
	}
	return Best ? Best : Get(World);
}

void ABlockIsland::RegenerateArena(int32 NewSeed)
{
	if (!CanEdit())
	{
		return; // a LAN client gets the host's new arena by itself
	}
	Layout = EIslandLayout::Arena;
	Seed = NewSeed;
	TempCells.Reset();
	Rebuild();
}

void ABlockIsland::RegenerateLobby(int32 NewSeed)
{
	if (!CanEdit())
	{
		return; // a LAN client gets the host's lobby island by itself
	}
	Layout = EIslandLayout::Lobby;
	Seed = NewSeed;
	TempCells.Reset();
	Rebuild();
}

void ABlockIsland::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Now = GetNetTime();
	if (CanEdit())
	{
		// Ice walls and icebergs melt away.
		if (TempCells.Num() > 0)
		{
			CellScratch.Reset();
			for (TPair<FIntPoint, FTimedCell>& Pair : TempCells)
			{
				Pair.Value.Left -= DeltaSeconds;
				if (Pair.Value.Left <= 0.f)
				{
					CellScratch.Add(Pair.Key);
				}
			}
			for (const FIntPoint& Cell : CellScratch)
			{
				RemoveCell(Cell);
			}
			if (CellScratch.Num() > 0)
			{
				MarkCellsChanged();
			}
		}
		if (bCollapsing || Crumbling.Num() > 0)
		{
			UpdateCollapse(DeltaSeconds);
		}
		// After this frame's removals: the sand they left without a block under it shakes, falls and lands.
		if (SandCells.Num() > 0 || SandBlocks.Num() > 0)
		{
			UpdateSand(Now);
		}
	}
	else
	{
		// LAN client: what melts and falls is the host's call (NetState); here it only animates toward it.
		for (TPair<FIntPoint, FTimedCell>& Pair : TempCells)
		{
			Pair.Value.Left = FMath::Max(Pair.Value.Left - DeltaSeconds, 0.f);
		}
		if (bCollapsing || Crumbling.Num() > 0)
		{
			CrumbleTime += DeltaSeconds;
			for (TPair<FIntPoint, FTimedCell>& Pair : Crumbling)
			{
				Pair.Value.Left = FMath::Max(Pair.Value.Left - DeltaSeconds, 0.f);
			}
			if (bCollapsing)
			{
				CollapseRadius = FMath::Max(0.f, CollapseRadius - CollapseSpeed * DeltaSeconds);
			}
		}
		if (bNetStatePending)
		{
			ApplyNetState();
		}
		// Falling sand where the host's fall has it now (the blocks it leaves and becomes come with NetState).
		MoveFallingSand(Now, false);
		if (!bNetSynced && !bNetNeverSyncedWarned && GetGameTimeSinceCreation() > 15.f)
		{
			bNetNeverSyncedWarned = true;
			UE_LOG(LogPingu, Warning, TEXT("[ISLAND] client: still no island from the host after 15 s - is the level's BlockIsland set to replicate?"));
		}
		if (bNetMismatch)
		{
			NetMismatchTime += DeltaSeconds;
			if (NetMismatchTime > BlockIslandPrivate::NetMismatchWarnDelay && NetMismatchWarnedGeneration != NetState.Generation)
			{
				NetMismatchWarnedGeneration = NetState.Generation;
				UE_LOG(LogPingu, Warning, TEXT("[ISLAND] client: the blocks differ from the host's (generation %d revision %d, %s seed %d): %d blocks, checksum %08x, host %08x"),
					NetState.Generation, NetState.Revision, BlockIslandPrivate::LayoutName(NetState.Layout), NetState.Seed, Cells.Num(), GetCellChecksum(),
					static_cast<uint32>(NetState.Checksum));
			}
		}
		else
		{
			NetMismatchTime = 0.f;
		}
	}
	// Everything this frame changed: rebuilt once (instances, collision) and, on the LAN host, sent.
	FlushEdits();
	UpdateMeltVisuals();
	UpdateRiseVisuals();
	UpdateCrumbleVisuals();
	UpdateSandVisuals(Now);
}

void ABlockIsland::FlushEdits()
{
	if (bVisualsDirty)
	{
		BuildVisuals();
	}
	if (bCollisionDirty)
	{
		BuildCollision();
	}
	if (bNetDirty)
	{
		RefreshNetState();
	}
}

void ABlockIsland::RebuildAfterEdits()
{
	// Every edit marks what it changed; Tick rebuilds that once at the end of the frame, however many edits there were.
	if (Cells.Num() > 0 || CollisionBoxes.Num() > 0)
	{
		bVisualsDirty = true;
		bCollisionDirty = true;
	}
}

void ABlockIsland::MarkCellsChanged()
{
	bVisualsDirty = true;
	bCollisionDirty = true;
	bNetDirty = true;
	bBoundsValid = false;
	bSandCheckDirty = true;
}

void ABlockIsland::MarkVisualsChanged()
{
	bVisualsDirty = true;
	bNetDirty = true;
}

bool ABlockIsland::RemoveCell(const FIntPoint& Cell)
{
	// A big block missing a cell is one no more: the others stay as ordinary blocks (the callers that break a big block
	// whole remove those too).
	if (const FIntPoint* BigOrigin = BigBlockOfCell.Find(Cell))
	{
		const FIntPoint Origin = *BigOrigin;
		DissolveBigBlock(Origin);
	}
	TempCells.Remove(Cell);
	Crumbling.Remove(Cell);
	AddedCells.Remove(Cell);
	SandCells.Remove(Cell);
	RisingCells.Remove(Cell);
	if (SandHiddenCells.Remove(Cell) > 0)
	{
		// Shaking sand that breaks does not fall any more (the sand on it falls at its time all the same).
		SandBlocks.RemoveAll([&Cell](const FSandBlock& Block) { return !Block.bFalling && Block.Start == Cell; });
		bSandInstancesDirty = true;
	}
	// Whatever stood on it is checked once this frame (UpdateSand): the callers mark the cells changed.
	return Cells.Remove(Cell) > 0;
}

// ---------------------------------------------------------------------------- sudden death

void ABlockIsland::StartCollapse(float StepInterval, float WarnTime)
{
	if (!CanEdit() || Cells.Num() == 0)
	{
		return;
	}
	// From the outermost block toward the middle of the island.
	float MinX = TNumericLimits<float>::Max();
	float MaxX = TNumericLimits<float>::Lowest();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		const float CellX = static_cast<float>(CellToWorld(Pair.Key).X);
		MinX = FMath::Min(MinX, CellX);
		MaxX = FMath::Max(MaxX, CellX);
	}
	CollapseCenterX = 0.5f * (MinX + MaxX);
	CollapseRadius = 0.5f * (MaxX - MinX) + CellSize * 0.5f; // the outermost columns start shaking after half a step
	CollapseSpeed = CellSize / FMath::Max(StepInterval, 0.05f);
	CollapseWarnTime = FMath::Max(WarnTime, 0.1f);
	bCollapsing = true;
	bNetDirty = true;
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Shake(this, 0.35f);
	APinguFX::Sound(this, EPinguSound::Crumble, 1.f, 0.f, 0.8f);
}

void ABlockIsland::StopCollapse()
{
	if (!CanEdit() || (!bCollapsing && Crumbling.Num() == 0))
	{
		return;
	}
	bCollapsing = false;
	bNetDirty = true;
	if (Crumbling.Num() > 0)
	{
		Crumbling.Reset(); // the blocks that were shaking stay where they are
		MarkVisualsChanged();
	}
}

// ---------------------------------------------------------------------------- round hazards (APinguRoundHazards)

void ABlockIsland::SetSeaRise(float Rise)
{
	Rise = FMath::Max(Rise, 0.f);
	if (FMath::IsNearlyEqual(Rise, SeaRise, 0.01f))
	{
		return;
	}
	if (SeaRise <= 0.f)
	{
		SeaLevelBeforeRise = SeaLevel; // the sea as the island made it (BuildBackdrop draws it there, minus the rise)
	}
	SeaRise = Rise;
	SeaLevel = SeaLevelBeforeRise + Rise;
	// The look: the sea plane and what floats on it move up as a whole (no instance rebuild).
	for (USceneComponent* Part : TArray<USceneComponent*>{ Ocean, Floes, IceFoam })
	{
		if (Part)
		{
			if (Part->Mobility != EComponentMobility::Movable)
			{
				Part->SetMobility(EComponentMobility::Movable);
			}
			Part->SetRelativeLocation(FVector(0.f, 0.f, Rise));
		}
	}
}

int32 ABlockIsland::CrumbleOutside(float MinX, float MaxX, float WarnTime)
{
	if (!CanEdit())
	{
		return 0;
	}
	const double Now = GetNetTime();
	CollapseWarnTime = FMath::Max(WarnTime, 0.1f); // (the shaking's look and the clients read it)
	int32 Started = 0;
	CellScratch.Reset();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		const float CellX = static_cast<float>(CellToWorld(Pair.Key).X);
		if ((CellX < MinX || CellX > MaxX) && !Crumbling.Contains(Pair.Key))
		{
			FTimedCell& Timed = Crumbling.Add(Pair.Key);
			Timed.Left = CollapseWarnTime;
			Timed.At = Now + CollapseWarnTime;
			++Started;
			if (BigBlockOfCell.Contains(Pair.Key))
			{
				CellScratch.AddUnique(BigBlockOfCell[Pair.Key]);
			}
		}
	}
	// A big block reached by it crumbles whole, as in sudden death.
	for (const FIntPoint& Origin : CellScratch)
	{
		for (int32 Dx = 0; Dx < BigBlockCells; ++Dx)
		{
			for (int32 Dz = 0; Dz < BigBlockCells; ++Dz)
			{
				const FIntPoint Cell(Origin.X + Dx, Origin.Y + Dz);
				if (Cells.Contains(Cell) && !Crumbling.Contains(Cell))
				{
					FTimedCell& Timed = Crumbling.Add(Cell);
					Timed.Left = CollapseWarnTime;
					Timed.At = Now + CollapseWarnTime;
					++Started;
				}
			}
		}
		DissolveBigBlock(Origin);
	}
	if (Started > 0)
	{
		MarkVisualsChanged(); // the shaking blocks move to their own instances; UpdateCollapse drops them when their time is up
		bNetDirty = true;
	}
	return Started;
}

bool ABlockIsland::IsCrumblingOver(float MinX, float MaxX) const
{
	for (const TPair<FIntPoint, FTimedCell>& Pair : Crumbling)
	{
		const float CellX = static_cast<float>(CellToWorld(Pair.Key).X);
		if (CellX >= MinX && CellX <= MaxX)
		{
			return true;
		}
	}
	return false;
}

void ABlockIsland::UpdateCollapse(float DeltaSeconds)
{
	CrumbleTime += DeltaSeconds;

	// Blocks whose warning ran out fall into the sea.
	CellScratch.Reset();
	for (TPair<FIntPoint, FTimedCell>& Pair : Crumbling)
	{
		Pair.Value.Left -= DeltaSeconds;
		if (Pair.Value.Left <= 0.f)
		{
			CellScratch.Add(Pair.Key);
		}
	}
	if (CellScratch.Num() > 0)
	{
		for (const FIntPoint& Cell : CellScratch)
		{
			BrokenBlocks += RemoveCell(Cell) ? 1 : 0;
		}
		MarkCellsChanged();
		// A few chunks tumbling down (not one per block: a column can be a dozen of them).
		FPinguFXBroadcastScope Broadcast(this);
		const int32 Stride = FMath::Max(1, CellScratch.Num() / 6);
		for (int32 Index = 0; Index < CellScratch.Num(); Index += Stride)
		{
			const FVector Where = CellToWorld(CellScratch[Index]);
			APinguFX::Shards(this, Where, 3, 420.f, FLinearColor(0.75f, 0.82f, 0.95f));
			APinguFX::Puffs(this, Where, 2, 40.f, 180.f, FVector(0.f, 0.f, -1.f), 1.f, FLinearColor(0.85f, 0.9f, 1.f));
		}
		APinguFX::Sound(this, EPinguSound::Crumble, 0.55f);
		APinguFX::Shake(this, 0.08f);
	}

	if (!bCollapsing)
	{
		return;
	}
	CollapseRadius = FMath::Max(0.f, CollapseRadius - CollapseSpeed * DeltaSeconds);
	// Everything outside the shrinking radius starts to crumble - ice walls and icebergs built out there too.
	const double Now = GetNetTime();
	bool bStarted = false;
	CellScratch.Reset();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		if (!Crumbling.Contains(Pair.Key) && FMath::Abs(static_cast<float>(CellToWorld(Pair.Key).X) - CollapseCenterX) > CollapseRadius)
		{
			FTimedCell& Timed = Crumbling.Add(Pair.Key);
			Timed.Left = CollapseWarnTime;
			Timed.At = Now + CollapseWarnTime;
			bStarted = true;
			if (BigBlockOfCell.Contains(Pair.Key))
			{
				CellScratch.AddUnique(BigBlockOfCell[Pair.Key]);
			}
		}
	}
	// A big block reached by it crumbles whole (its cells shake as ordinary blocks, all falling at the same time).
	for (const FIntPoint& Origin : CellScratch)
	{
		for (int32 Dx = 0; Dx < BigBlockCells; ++Dx)
		{
			for (int32 Dz = 0; Dz < BigBlockCells; ++Dz)
			{
				const FIntPoint Cell(Origin.X + Dx, Origin.Y + Dz);
				if (Cells.Contains(Cell) && !Crumbling.Contains(Cell))
				{
					FTimedCell& Timed = Crumbling.Add(Cell);
					Timed.Left = CollapseWarnTime;
					Timed.At = Now + CollapseWarnTime;
				}
			}
		}
		DissolveBigBlock(Origin);
	}
	if (bStarted)
	{
		MarkVisualsChanged(); // the shaking blocks move to their own instances; the collision stays
	}
	if (Cells.Num() == 0)
	{
		bCollapsing = false;
		bNetDirty = true;
	}
}

bool ABlockIsland::BreakBlockAt(const FVector& World, bool /*bRebuild*/)
{
	if (!CanEdit())
	{
		return false;
	}
	const FIntPoint Hit = WorldToCell(World);
	const EBlockKind Kind = GetBlock(Hit.X, Hit.Y);
	if (Kind == EBlockKind::None)
	{
		return false;
	}
	// A big block breaks whole.
	const FSandPiece Piece = GetPieceOf(Hit);
	DissolveBigBlock(Piece.Origin);
	const FLinearColor Color = Kind == EBlockKind::Ice ? IceColor : (Kind == EBlockKind::Snow ? SnowColor : FLinearColor(0.35f, 0.4f, 0.5f));
	FPinguFXBroadcastScope Broadcast(this);
	for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
	{
		for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
		{
			const FIntPoint Cell(Piece.Origin.X + Dx, Piece.Origin.Y + Dz);
			if (!RemoveCell(Cell))
			{
				continue;
			}
			++BrokenBlocks;
			const FVector Center = CellToWorld(Cell);
			APinguFX::Shards(this, Center, 6, 650.f, Color);
			APinguFX::Puffs(this, Center, 3, 38.f, 260.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.9f, 1.f));
		}
	}
	MarkCellsChanged();
	return true;
}

// ---------------------------------------------------------------------------- ice

bool ABlockIsland::BreakIceAt(const FVector& World, const FVector& PushDirection)
{
	if (!CanEdit())
	{
		return false;
	}
	const FIntPoint Cell = WorldToCell(World);
	if (GetBlock(Cell.X, Cell.Y) != EBlockKind::Ice)
	{
		return false;
	}
	// The shards and the sound play on every LAN client too (the block itself goes with NetState).
	FPinguFXBroadcastScope Broadcast(this);
	PlayShatterSound(BreakIceCell(Cell, PushDirection.IsNearlyZero() ? FVector::UpVector : PushDirection));
	return true;
}

bool ABlockIsland::TakeIceBlockAt(const FVector& World, FIntPoint& OutCell)
{
	if (!CanEdit())
	{
		return false;
	}
	const FIntPoint Cell = WorldToCell(World);
	if (GetBlock(Cell.X, Cell.Y) != EBlockKind::Ice || !RemoveCell(Cell))
	{
		return false;
	}
	// Not broken: the block flies on as an AHookedIceBlock (hooked or blown by a gust) and comes back as sand ice where that
	// lands.
	OutCell = Cell;
	MarkCellsChanged();
	return true;
}

void ABlockIsland::GetAbilityIceLook(UStaticMesh*& OutMesh, UMaterialInterface*& OutMaterial) const
{
	OutMesh = TempBlocks ? TempBlocks->GetStaticMesh() : nullptr;
	OutMaterial = TempBlocks ? TempBlocks->GetMaterial(0) : nullptr;
}

int32 ABlockIsland::BreakIceInRadius(const FVector& Center, float Radius, int32 MaxBlocks, const FVector& PushDirection)
{
	using namespace BlockIslandPrivate;
	if (!CanEdit() || MaxBlocks <= 0 || Radius < 0.f)
	{
		return 0;
	}
	// Ice blocks with their centre in the circle (on the play plane), nearest first.
	TArray<TPair<float, FIntPoint>> Found;
	const FVector Middle(Center.X, 0.f, Center.Z);
	const FIntPoint C = WorldToCell(Middle);
	const int32 Reach = FMath::CeilToInt(Radius / CellSize) + 1;
	for (int32 X = C.X - Reach; X <= C.X + Reach; ++X)
	{
		for (int32 Z = C.Y - Reach; Z <= C.Y + Reach; ++Z)
		{
			if (GetBlock(X, Z) != EBlockKind::Ice)
			{
				continue;
			}
			const float DistSq = static_cast<float>(FVector::DistSquared(CellToWorld(FIntPoint(X, Z)), Middle));
			if (DistSq <= Radius * Radius)
			{
				Found.Add(TPair<float, FIntPoint>(DistSq, FIntPoint(X, Z)));
			}
		}
	}
	if (Found.Num() == 0)
	{
		return 0;
	}
	Found.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
	FPinguFXBroadcastScope Broadcast(this);
	// Up to MaxBlocks blocks (a big block is one: the rest of its cells in the list are gone with it).
	int32 Blocks = 0;
	int32 Count = 0;
	for (int32 Index = 0; Index < Found.Num() && Blocks < MaxBlocks; ++Index)
	{
		const FIntPoint Cell = Found[Index].Value;
		if (GetBlock(Cell.X, Cell.Y) != EBlockKind::Ice)
		{
			continue;
		}
		FVector Direction = PushDirection;
		if (Direction.IsNearlyZero())
		{
			// Outward from the middle of the blast, and up.
			Direction = (CellToWorld(Cell) - Middle).GetSafeNormal() + FVector::UpVector * IceShardLift;
		}
		Count += BreakIceCell(Cell, Direction);
		++Blocks;
	}
	PlayShatterSound(Count);
	return Count;
}

int32 ABlockIsland::BreakIceCell(const FIntPoint& Cell, const FVector& Direction)
{
	// A big block breaks whole: every one of its cells.
	const FSandPiece Piece = GetPieceOf(Cell);
	if (Piece.Span <= 1)
	{
		return BreakOneIceCell(Cell, Direction) ? 1 : 0;
	}
	DissolveBigBlock(Piece.Origin);
	int32 Count = 0;
	for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
	{
		for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
		{
			const FIntPoint Part(Piece.Origin.X + Dx, Piece.Origin.Y + Dz);
			if (GetBlock(Part.X, Part.Y) == EBlockKind::Ice)
			{
				Count += BreakOneIceCell(Part, Direction) ? 1 : 0;
			}
		}
	}
	return Count;
}

bool ABlockIsland::BreakOneIceCell(const FIntPoint& Cell, const FVector& Direction)
{
	using namespace BlockIslandPrivate;
	if (!RemoveCell(Cell))
	{
		return false;
	}
	++BrokenBlocks;
	MarkCellsChanged();
	const FVector Center = CellToWorld(Cell);
	FVector Push = FVector(Direction.X, 0.f, Direction.Z).GetSafeNormal();
	if (Push.IsNearlyZero())
	{
		Push = FVector::UpVector;
	}
	if (APinguFX* FX = APinguFX::Get(this))
	{
		// Shards flying along the push (a cone turned on the play plane), from all over the block (DepthRows deep). Like
		// APinguFX's shard burst, only aimed.
		const float Depth = FMath::Max(DepthRows, 1) * CellSize;
		const int32 Count = FMath::Max(1, FMath::RoundToInt(IceShardsAlongPush * FX->GetDensity()));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Angle = FMath::FRandRange(-IceShardSpread, IceShardSpread);
			const float Cos = FMath::Cos(Angle);
			const float Sin = FMath::Sin(Angle);
			const FVector Out(Push.X * Cos - Push.Z * Sin, FMath::FRandRange(-0.25f, 0.25f), Push.X * Sin + Push.Z * Cos);
			FPinguParticleDesc Shard;
			Shard.Mesh = EPinguFXMesh::Shard;
			Shard.Location = Center + FVector(FMath::FRandRange(-0.3f, 0.3f) * CellSize, FMath::FRandRange(-0.4f, 0.4f) * Depth, FMath::FRandRange(-0.3f, 0.3f) * CellSize);
			Shard.Velocity = Out.GetSafeNormal() * IceShardSpeed * FMath::FRandRange(0.5f, 1.f);
			Shard.Size = FMath::FRandRange(16.f, 32.f);
			Shard.EndSize = Shard.Size * 0.2f;
			Shard.Curve = EPinguSizeCurve::Linear;
			Shard.Life = FMath::FRandRange(0.5f, 0.85f);
			Shard.Gravity = 1700.f;
			Shard.Drag = 0.8f;
			Shard.Color = IceShardColor;
			Shard.Emissive = 0.4f;
			Shard.Rotation = FRotator(FMath::FRandRange(0.f, 360.f), FMath::FRandRange(0.f, 360.f), FMath::FRandRange(0.f, 360.f));
			Shard.Spin = FVector(FMath::FRandRange(-720.f, 720.f), FMath::FRandRange(-720.f, 720.f), FMath::FRandRange(-720.f, 720.f));
			FX->Spawn(Shard);
		}
	}
	APinguFX::Shards(this, Center, 3, 450.f, IceShardColor);
	APinguFX::Puffs(this, Center, 3, 34.f, 240.f, Push, 1.f, IceDustColor);
	return true;
}

void ABlockIsland::PlayShatterSound(int32 NumBlocks)
{
	using namespace BlockIslandPrivate;
	const UWorld* MyWorld = GetWorld();
	const double Now = MyWorld ? MyWorld->GetTimeSeconds() : 0.0;
	if (NumBlocks <= 0 || (LastShatterSoundTime >= 0.0 && Now >= LastShatterSoundTime && Now - LastShatterSoundTime < ShatterSoundInterval))
	{
		return; // blocks breaking together make one sound
	}
	LastShatterSoundTime = Now;
	APinguFX::Sound(this, EPinguSound::Shatter, FMath::Min(ShatterVolume + ShatterVolumePerBlock * (NumBlocks - 1), 1.f));
}

// ---------------------------------------------------------------------------- sand

TArray<FIntPoint> ABlockIsland::AddSandBlocks(const TArray<FIntPoint>& InCells, EBlockKind Kind, APenguinCharacter* CausedBy, bool bIcebergIce,
	const TArray<FIntPoint>* BigOrigins)
{
	using namespace BlockIslandPrivate;
	TArray<FIntPoint> Taken;
	if (!CanEdit() || Kind == EBlockKind::None)
	{
		return Taken;
	}
	const double Now = GetNetTime();
	// A cell falling sand is passing through right now is taken as well.
	const auto IsFree = [this, &Taken](const FIntPoint& Cell)
	{
		if (IsSolidCell(Cell) || Taken.Contains(Cell))
		{
			return false;
		}
		return !SandBlocks.ContainsByPredicate([this, &Cell](const FSandBlock& Block)
		{
			const int32 Span = FMath::Max(Block.Span, 1);
			return Block.bFalling && !Block.bDone && Cell.X >= Block.Start.X && Cell.X < Block.Start.X + Span
				&& Cell.Y * CellSize < Block.Bottom + Span * CellSize - SandOverlapMargin && Block.Bottom < (Cell.Y + 1) * CellSize - SandOverlapMargin;
		});
	};
	// The pieces: the big blocks asked for (every one of their cells among InCells), the other cells one by one.
	TArray<FSandPiece> Pieces;
	TSet<FIntPoint> InBig;
	if (BigOrigins)
	{
		const TSet<FIntPoint> Asked(InCells);
		for (const FIntPoint& Origin : *BigOrigins)
		{
			bool bWhole = true;
			for (int32 Dx = 0; Dx < BigBlockCells && bWhole; ++Dx)
			{
				for (int32 Dz = 0; Dz < BigBlockCells && bWhole; ++Dz)
				{
					const FIntPoint Cell(Origin.X + Dx, Origin.Y + Dz);
					bWhole = Asked.Contains(Cell) && !InBig.Contains(Cell);
				}
			}
			if (bWhole)
			{
				Pieces.Add({ Origin, BigBlockCells });
				for (int32 Dx = 0; Dx < BigBlockCells; ++Dx)
				{
					for (int32 Dz = 0; Dz < BigBlockCells; ++Dz)
					{
						InBig.Add(FIntPoint(Origin.X + Dx, Origin.Y + Dz));
					}
				}
			}
		}
	}
	for (const FIntPoint& Cell : InCells)
	{
		if (!InBig.Contains(Cell))
		{
			Pieces.Add({ Cell, 1 });
		}
	}
	// Lowest first, so a block resting on another one of the same batch is a block at once too.
	Pieces.Sort([](const FSandPiece& A, const FSandPiece& B) { return A.Origin.Y != B.Origin.Y ? A.Origin.Y < B.Origin.Y : A.Origin.X < B.Origin.X; });
	TArray<FSandPiece> Resting;
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const FSandPiece Piece = Pieces[Index];
		bool bFree = true;
		for (int32 Dx = 0; Dx < Piece.Span && bFree; ++Dx)
		{
			for (int32 Dz = 0; Dz < Piece.Span && bFree; ++Dz)
			{
				bFree = IsFree(FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y + Dz));
			}
		}
		if (!bFree)
		{
			if (Piece.Span > 1)
			{
				// Not all of it fits: its free cells go one by one (in their place in the order).
				for (int32 Dz = Piece.Span - 1; Dz >= 0; --Dz)
				{
					for (int32 Dx = Piece.Span - 1; Dx >= 0; --Dx)
					{
						Pieces.Insert({ FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y + Dz), 1 }, Index + 1);
					}
				}
			}
			continue;
		}
		for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
		{
			for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
			{
				Taken.Add(FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y + Dz));
			}
		}
		if (IsPieceSupported(Piece))
		{
			// On a block: blocks right away.
			for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
			{
				for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
				{
					const FIntPoint Cell(Piece.Origin.X + Dx, Piece.Origin.Y + Dz);
					Cells.Add(Cell, Kind);
					SandCells.Add(Cell);
					AddedCells.Add(Cell);
				}
			}
			if (Piece.Span > 1)
			{
				MakeBigBlock(Piece.Origin);
			}
			Resting.Add(Piece);
		}
		else
		{
			// In the air: it falls from here at once (no shaking, it never rested).
			FSandBlock& Block = SandBlocks.AddDefaulted_GetRef();
			Block.Start = Piece.Origin;
			Block.Span = Piece.Span;
			Block.StartTime = Now;
			Block.Kind = Kind;
			Block.Bottom = Piece.Origin.Y * CellSize;
			Block.bFalling = true;
			Block.CausedBy = CausedBy;
			Block.bIcebergIce = bIcebergIce;
			bSandInstancesDirty = true;
			bNetDirty = true;
		}
	}
	if (Resting.Num() > 0)
	{
		MarkCellsChanged();
		// Never inside a block: whoever stands where one appeared is moved out of it.
		FPinguFXBroadcastScope Broadcast(this);
		for (const FSandPiece& Piece : Resting)
		{
			const FVector Low = GetActorLocation() + FVector(Piece.Origin.X * CellSize, 0.f, Piece.Origin.Y * CellSize);
			const float Size = Piece.Span * CellSize;
			ShovePenguinsOutOfSand(FBox(FVector(Low.X, 0.f, Low.Z), FVector(Low.X + Size, 0.f, Low.Z + Size)), static_cast<float>(Low.Z), true, false, 0.f, nullptr, false, Size);
		}
	}
	return Taken;
}

bool ABlockIsland::IsSandFalling() const
{
	return SandBlocks.Num() > 0;
}

bool ABlockIsland::IsSandFallingOver(float MinX, float MaxX, float AboveZ, float* OutBlockX) const
{
	const FVector Origin = GetActorLocation();
	for (const FSandBlock& Block : SandBlocks)
	{
		const float Width = FMath::Max(Block.Span, 1) * CellSize;
		const float BlockLeft = static_cast<float>(Origin.X) + Block.Start.X * CellSize;
		const float BlockTop = static_cast<float>(Origin.Z) + Block.Bottom + Width;
		if (!Block.bDone && BlockLeft < MaxX && BlockLeft + Width > MinX && BlockTop > AboveZ)
		{
			if (OutBlockX)
			{
				*OutBlockX = BlockLeft + Width * 0.5f;
			}
			return true;
		}
	}
	return false;
}

bool ABlockIsland::GetFallingSandBottom(float MinX, float MaxX, float& OutBottomZ) const
{
	const FVector Origin = GetActorLocation();
	bool bFound = false;
	for (const FSandBlock& Block : SandBlocks)
	{
		const float BlockLeft = static_cast<float>(Origin.X) + Block.Start.X * CellSize;
		if (!Block.bDone && BlockLeft < MaxX && BlockLeft + FMath::Max(Block.Span, 1) * CellSize > MinX)
		{
			const float Bottom = static_cast<float>(Origin.Z) + Block.Bottom;
			OutBottomZ = bFound ? FMath::Min(OutBottomZ, Bottom) : Bottom;
			bFound = true;
		}
	}
	return bFound;
}

void ABlockIsland::UpdateSand(double Now)
{
	if (bSandCheckDirty)
	{
		bSandCheckDirty = false;
		StartSandWarnings(Now);
	}
	StartSandFalls(Now);
	MoveFallingSand(Now, true);
}

void ABlockIsland::StartSandWarnings(double Now)
{
	using namespace BlockIslandPrivate;
	// The bottom of every stack of resting sand that has nothing under it any more (a big block: nothing under any of its
	// bottom cells).
	TArray<FSandPiece> Pieces;
	for (const FIntPoint& Cell : SandCells)
	{
		if (SandHiddenCells.Contains(Cell))
		{
			continue;
		}
		const FSandPiece Piece = GetPieceOf(Cell);
		if (Piece.Origin == Cell && !IsPieceSupported(Piece))
		{
			Pieces.Add(Piece);
		}
	}
	if (Pieces.Num() == 0)
	{
		return;
	}
	const int32 Lowest = Pieces.Num();
	// They and the sand stacked on them (with nothing else under it) shake together, then fall together. They stay blocks
	// until then.
	CollectSandStack(Pieces, true);
	const double FallTime = Now + SandWarnTime;
	FPinguFXBroadcastScope Broadcast(this);
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const FSandPiece& Piece = Pieces[Index];
		FSandBlock& Block = SandBlocks.AddDefaulted_GetRef();
		Block.Start = Piece.Origin;
		Block.Span = Piece.Span;
		Block.StartTime = FallTime;
		Block.Kind = GetBlock(Piece.Origin.X, Piece.Origin.Y);
		Block.Bottom = Piece.Origin.Y * CellSize;
		Block.bFromIsland = true;
		for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
		{
			for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
			{
				SandHiddenCells.Add(FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y + Dz), FallTime);
			}
		}
		if (Index < Lowest)
		{
			// Ice dust trickling from under it: it is about to go.
			const FVector Under = GetActorLocation() + FVector((Piece.Origin.X + Piece.Span * 0.5f) * CellSize, 0.f, Piece.Origin.Y * CellSize);
			APinguFX::Puffs(this, FVector(Under.X, 0.f, Under.Z), 3 * Piece.Span, 22.f * Piece.Span, 90.f, FVector(0.f, 0.f, -1.f), 0.8f, IceDustColor);
		}
	}
	APinguFX::Sound(this, EPinguSound::Crumble, SandWarnVolume, 0.06f, SandWarnPitch);
	bSandInstancesDirty = true;
	MarkVisualsChanged(); // the shaking blocks move to FallingBlocks; the collision stays
}

void ABlockIsland::StartSandFalls(double Now)
{
	// Shaking sand whose time is up, lowest first.
	SandOrderScratch.Reset();
	for (int32 Index = 0; Index < SandBlocks.Num(); ++Index)
	{
		const FSandBlock& Block = SandBlocks[Index];
		if (!Block.bFalling && !Block.bDone && Block.StartTime <= Now)
		{
			SandOrderScratch.Add(Index);
		}
	}
	if (SandOrderScratch.Num() == 0)
	{
		return;
	}
	SandOrderScratch.Sort([this](int32 A, int32 B)
	{
		const FIntPoint& StartA = SandBlocks[A].Start;
		const FIntPoint& StartB = SandBlocks[B].Start;
		return StartA.Y != StartB.Y ? StartA.Y < StartB.Y : StartA.X < StartB.X;
	});
	for (const int32 Index : SandOrderScratch)
	{
		if (SandBlocks[Index].bFalling || SandBlocks[Index].bDone)
		{
			continue; // went with the block under it
		}
		// (BeginSandFall may add entries: no reference into SandBlocks is kept across it.)
		const FSandPiece Piece = { SandBlocks[Index].Start, FMath::Max(SandBlocks[Index].Span, 1) };
		const double StartTime = SandBlocks[Index].StartTime;
		if (IsPieceSupported(Piece))
		{
			// Something is under it again (an ice wall went up meanwhile): it stays a block.
			SandBlocks[Index].bDone = true;
			for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
			{
				for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
				{
					SandHiddenCells.Remove(FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y + Dz));
				}
			}
			bSandInstancesDirty = true;
			MarkVisualsChanged();
			continue;
		}
		// It leaves the island with the sand stacked on it (also sand that landed on it while it shook).
		TArray<FSandPiece> Going = { Piece };
		CollectSandStack(Going, false);
		for (const FSandPiece& Part : Going)
		{
			BeginSandFall(Part, StartTime);
		}
	}
	SandBlocks.RemoveAll([](const FSandBlock& Block) { return Block.bDone; });
}

void ABlockIsland::BeginSandFall(const FSandPiece& Piece, double StartTime)
{
	int32 Index = SandBlocks.IndexOfByPredicate([&Piece](const FSandBlock& Block) { return !Block.bFalling && !Block.bDone && Block.Start == Piece.Origin && Block.Span == Piece.Span; });
	if (Index == INDEX_NONE)
	{
		Index = SandBlocks.AddDefaulted();
		SandBlocks[Index].Start = Piece.Origin;
		SandBlocks[Index].Span = Piece.Span;
	}
	FSandBlock& Block = SandBlocks[Index];
	Block.StartTime = StartTime;
	Block.Kind = GetBlock(Piece.Origin.X, Piece.Origin.Y); // frozen while it shook: it falls as ice
	Block.Bottom = Piece.Origin.Y * CellSize;
	Block.bFalling = true;
	Block.bFromIsland = true;
	// Not blocks while it falls: no collision, no queries, not in the LAN cells (it is in NetState.Falling). A big block is
	// one again where it lands.
	if (Piece.Span > 1)
	{
		BigBlocks.Remove(Piece.Origin);
	}
	for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
	{
		for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
		{
			const FIntPoint Cell(Piece.Origin.X + Dx, Piece.Origin.Y + Dz);
			Cells.Remove(Cell);
			SandCells.Remove(Cell);
			AddedCells.Remove(Cell);
			Crumbling.Remove(Cell);
			TempCells.Remove(Cell);
			SandHiddenCells.Remove(Cell);
			RisingCells.Remove(Cell);
			BigBlockOfCell.Remove(Cell);
		}
	}
	bSandInstancesDirty = true;
	MarkCellsChanged();
}

// ---------------------------------------------------------------------------- big blocks

bool ABlockIsland::IsBigBlockCell(const FIntPoint& Cell, FIntPoint* OutOrigin) const
{
	const FIntPoint* Origin = BigBlockOfCell.Find(Cell);
	if (Origin && OutOrigin)
	{
		*OutOrigin = *Origin;
	}
	return Origin != nullptr;
}

int32 ABlockIsland::GetNumFallingBigBlocks() const
{
	int32 Count = 0;
	for (const FSandBlock& Block : SandBlocks)
	{
		Count += !Block.bDone && Block.Span > 1 ? 1 : 0;
	}
	return Count;
}

bool ABlockIsland::MakeBigBlock(const FIntPoint& Origin)
{
	const EBlockKind* First = Cells.Find(Origin);
	if (!First || BigBlocks.Contains(Origin))
	{
		return false;
	}
	const EBlockKind Kind = *First;
	const bool bSand = SandCells.Contains(Origin);
	for (int32 Dx = 0; Dx < BigBlockCells; ++Dx)
	{
		for (int32 Dz = 0; Dz < BigBlockCells; ++Dz)
		{
			const FIntPoint Cell(Origin.X + Dx, Origin.Y + Dz);
			const EBlockKind* Other = Cells.Find(Cell);
			if (!Other || *Other != Kind || SandCells.Contains(Cell) != bSand || BigBlockOfCell.Contains(Cell) || Crumbling.Contains(Cell)
				|| SandHiddenCells.Contains(Cell))
			{
				return false;
			}
		}
	}
	BigBlocks.Add(Origin);
	for (int32 Dx = 0; Dx < BigBlockCells; ++Dx)
	{
		for (int32 Dz = 0; Dz < BigBlockCells; ++Dz)
		{
			BigBlockOfCell.Add(FIntPoint(Origin.X + Dx, Origin.Y + Dz), Origin);
		}
	}
	MarkVisualsChanged();
	return true;
}

void ABlockIsland::DissolveBigBlock(const FIntPoint& Origin)
{
	if (!BigBlocks.Remove(Origin))
	{
		return;
	}
	bool bWasShaking = false;
	for (int32 Dx = 0; Dx < BigBlockCells; ++Dx)
	{
		for (int32 Dz = 0; Dz < BigBlockCells; ++Dz)
		{
			const FIntPoint Cell(Origin.X + Dx, Origin.Y + Dz);
			BigBlockOfCell.Remove(Cell);
			bWasShaking |= SandHiddenCells.Remove(Cell) > 0;
		}
	}
	if (bWasShaking)
	{
		// It shook as one: it does not fall as one. Its cells rest again and are looked at anew this frame (UpdateSand).
		SandBlocks.RemoveAll([&Origin](const FSandBlock& Block) { return !Block.bFalling && Block.Start == Origin && Block.Span > 1; });
		bSandInstancesDirty = true;
		bSandCheckDirty = true;
	}
	MarkVisualsChanged();
}

ABlockIsland::FSandPiece ABlockIsland::GetPieceOf(const FIntPoint& Cell) const
{
	FSandPiece Piece;
	if (const FIntPoint* Origin = BigBlockOfCell.Find(Cell))
	{
		Piece.Origin = *Origin;
		Piece.Span = BigBlockCells;
	}
	else
	{
		Piece.Origin = Cell;
	}
	return Piece;
}

bool ABlockIsland::IsPieceSupported(const FSandPiece& Piece) const
{
	for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
	{
		if (IsSolidCell(FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y - 1)))
		{
			return true;
		}
	}
	return false;
}

void ABlockIsland::CollectSandStack(TArray<FSandPiece>& InOutPieces, bool bSkipShaking) const
{
	// Every cell of the pieces that go.
	TSet<FIntPoint> Going;
	const auto AddCells = [&Going](const FSandPiece& Piece)
	{
		for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
		{
			for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
			{
				Going.Add(FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y + Dz));
			}
		}
	};
	for (const FSandPiece& Piece : InOutPieces)
	{
		AddCells(Piece);
	}
	// Up from each: the sand resting right on top of it goes too, unless something that stays holds it up (a big block over
	// two columns, one of them still standing).
	for (int32 Index = 0; Index < InOutPieces.Num(); ++Index)
	{
		const FSandPiece Below = InOutPieces[Index];
		for (int32 Dx = 0; Dx < Below.Span; ++Dx)
		{
			const FIntPoint Above(Below.Origin.X + Dx, Below.Origin.Y + Below.Span);
			if (!SandCells.Contains(Above) || Going.Contains(Above) || (bSkipShaking && SandHiddenCells.Contains(Above)))
			{
				continue;
			}
			const FSandPiece Piece = GetPieceOf(Above);
			bool bHeld = false;
			for (int32 Ex = 0; Ex < Piece.Span && !bHeld; ++Ex)
			{
				const FIntPoint Under(Piece.Origin.X + Ex, Piece.Origin.Y - 1);
				bHeld = IsSolidCell(Under) && !Going.Contains(Under);
			}
			if (!bHeld)
			{
				InOutPieces.Add(Piece);
				AddCells(Piece);
			}
		}
	}
}

bool ABlockIsland::IsSandSupport(const FIntPoint& Cell, double Now) const
{
	if (!Cells.Contains(Cell))
	{
		return false;
	}
	// A LAN client still has shaking sand whose fall started on the host's clock until the host's state arrives.
	const double* FallsAt = SandHiddenCells.Find(Cell);
	return !FallsAt || *FallsAt > Now;
}

void ABlockIsland::MoveFallingSand(double Now, bool bAuthority)
{
	using namespace BlockIslandPrivate;
	// The blocks on their way down: the host's left the island; a client draws them from the host's start time on.
	SandOrderScratch.Reset();
	SandFreeScratch.SetNumUninitialized(SandBlocks.Num());
	for (int32 Index = 0; Index < SandBlocks.Num(); ++Index)
	{
		const FSandBlock& Block = SandBlocks[Index];
		if (Block.bDone || !(bAuthority ? Block.bFalling : Now >= Block.StartTime))
		{
			continue;
		}
		// Free fall from its cell (the same function of time on every machine).
		SandFreeScratch[Index] = Block.Start.Y * CellSize - SandFallDistance(Now - Block.StartTime);
		SandOrderScratch.Add(Index);
	}
	if (SandOrderScratch.Num() == 0)
	{
		return;
	}
	// Lowest first (by where they are, not where free fall would have them: a block that started earlier is faster, but
	// never passes the one falling under it), so each lands on one that landed now. A big block covers several columns: what
	// falls under it is looked up in every one of them.
	SandOrderScratch.Sort([this](int32 A, int32 B)
	{
		const FSandBlock& BlockA = SandBlocks[A];
		const FSandBlock& BlockB = SandBlocks[B];
		return BlockA.Bottom != BlockB.Bottom ? BlockA.Bottom < BlockB.Bottom : BlockA.Start.X < BlockB.Start.X;
	});

	// Landing and splash effects, shoves and broken projectiles reach the LAN clients (on a client this does nothing).
	FPinguFXBroadcastScope Broadcast(this);
	const FVector Origin = GetActorLocation();
	const FBox Bounds = GetBounds();
	// Under the island's lowest block there is nothing to land on: from there it falls into the sea.
	const int32 LowestRow = Bounds.IsValid ? FMath::FloorToInt(static_cast<float>(Bounds.Min.Z - Origin.Z) / CellSize) : 0;
	const float SeaSurface = SeaLevel + SeaSurfaceAboveLevel - static_cast<float>(Origin.Z);
	TArray<FSandContact> Contacts;
	// Per column: the top of what falls (or landed) under the next block this frame.
	TMap<int32, float>& BelowTop = SandBelowTopScratch;
	BelowTop.Reset();
	bool bRemoved = false;
	bool bSplashSound = false;
	for (const int32 Index : SandOrderScratch)
	{
		FSandBlock& Block = SandBlocks[Index];
		const int32 Span = FMath::Max(Block.Span, 1);
		const float Size = Span * CellSize;
		const int32 FirstColumn = Block.Start.X;
		const int32 EndColumn = FirstColumn + Span;
		float Under = TNumericLimits<float>::Lowest();
		for (int32 Column = FirstColumn; Column < EndColumn; ++Column)
		{
			if (const float* Top = BelowTop.Find(Column))
			{
				Under = FMath::Max(Under, *Top);
			}
		}
		const float Previous = Block.Bottom;
		// Free fall, riding on the block falling under it, never back up.
		const float NewBottom = FMath::Min(FMath::Max(SandFreeScratch[Index], Under), Previous);

		// Where it lands: on top of a block that appeared where it is (an ice wall rose through it) - not while it is still
		// in its own cell, where a LAN client may have it or a newer block - else on the first block under where it was
		// last frame (however far a long frame takes it) - the highest of those under any of its columns.
		// (No landing: the lowest int, not INDEX_NONE - row -1 is a real row, a block landing on the row under the ground.)
		constexpr int32 NoRow = TNumericLimits<int32>::Lowest();
		int32 LandRow = NoRow;
		const int32 OverlapLow = FMath::FloorToInt((Previous + SandOverlapMargin) / CellSize);
		const int32 OverlapHigh = FMath::FloorToInt((Previous + Size - SandOverlapMargin) / CellSize);
		const bool bInOwnCell = Previous >= Block.Start.Y * CellSize - SandOverlapMargin;
		for (int32 Column = FirstColumn; Column < EndColumn && !bInOwnCell; ++Column)
		{
			for (int32 Row = OverlapHigh; Row >= OverlapLow; --Row)
			{
				if (IsSandSupport(FIntPoint(Column, Row), Now))
				{
					LandRow = FMath::Max(LandRow, Row + 1);
					break;
				}
			}
		}
		if (LandRow == NoRow)
		{
			int32 Top = NoRow;
			for (int32 Column = FirstColumn; Column < EndColumn; ++Column)
			{
				for (int32 Row = OverlapLow - 1; Row >= LowestRow; --Row)
				{
					if (IsSandSupport(FIntPoint(Column, Row), Now))
					{
						Top = FMath::Max(Top, Row + 1);
						break;
					}
				}
			}
			if (Top != NoRow && NewBottom <= Top * CellSize)
			{
				LandRow = Top;
			}
		}

		if (LandRow != NoRow)
		{
			// Never into a block: up to the first rows that are free in every column it covers.
			const auto IsBlocked = [this, Now, FirstColumn, EndColumn, Span](int32 Row)
			{
				for (int32 Column = FirstColumn; Column < EndColumn; ++Column)
				{
					for (int32 Dz = 0; Dz < Span; ++Dz)
					{
						if (IsSandSupport(FIntPoint(Column, Row + Dz), Now))
						{
							return true;
						}
					}
				}
				return false;
			};
			for (int32 Guard = 0; Guard < SandMaxClimbRows && IsBlocked(LandRow); ++Guard)
			{
				++LandRow;
			}
			Block.Bottom = LandRow * CellSize;
			for (int32 Column = FirstColumn; Column < EndColumn; ++Column)
			{
				BelowTop.Add(Column, Block.Bottom + Size);
			}
			if (bAuthority)
			{
				// Blocks of the island again. (A client keeps it resting there until the host's state says so.)
				const FIntPoint Cell(FirstColumn, LandRow);
				LandSandBlock(Block, Cell);
				Block.bDone = true;
				bRemoved = true;
				const FVector Low(Origin.X + FirstColumn * CellSize, 0.f, Origin.Z + Block.Bottom);
				Contacts.Add({ FBox(Low, Low + FVector(Size, 0.f, Size)), static_cast<float>(Low.Z), SandFallSpeed(Now - Block.StartTime), Cell, true, Block.CausedBy.Get(),
					Block.bIcebergIce, Span });
			}
			continue;
		}
		if (bAuthority && NewBottom + Size * 0.5f <= SeaSurface)
		{
			// Past the island and into the sea: gone (blocks the island lost count as broken). One splash sound a frame.
			APinguFX::Splash(this, FVector(Origin.X + (FirstColumn + Span * 0.5f) * CellSize, 0.f, SeaLevel + SeaSurfaceAboveLevel));
			if (!bSplashSound)
			{
				bSplashSound = true;
				APinguFX::Sound(this, EPinguSound::Splash, SandSplashVolume);
			}
			BrokenBlocks += Block.bFromIsland ? Span * Span : 0;
			Block.bDone = true;
			bRemoved = true;
			for (int32 Column = FirstColumn; Column < EndColumn; ++Column)
			{
				BelowTop.Add(Column, NewBottom + Size);
			}
			continue;
		}
		// A client keeps drawing it just under the sea surface until the host says it is gone.
		Block.Bottom = bAuthority ? NewBottom : FMath::Max(NewBottom, SeaSurface - 2.f * Size);
		for (int32 Column = FirstColumn; Column < EndColumn; ++Column)
		{
			BelowTop.Add(Column, Block.Bottom + Size);
		}
		if (bAuthority)
		{
			// Everything it passed through this frame.
			const FVector Low(Origin.X + FirstColumn * CellSize, 0.f, Origin.Z + NewBottom);
			const FVector High(Low.X + Size, 0.f, Origin.Z + Previous + Size);
			Contacts.Add({ FBox(Low, High), static_cast<float>(Low.Z), SandFallSpeed(Now - Block.StartTime), Block.Start, false, Block.CausedBy.Get(), Block.bIcebergIce, Span });
		}
	}
	if (!bAuthority)
	{
		return;
	}

	// Penguins out of the way, effects where blocks landed, projectiles and pegs in the way broken.
	SandBoxScratch.Reset();
	float Hardest = 0.f;
	int32 NumLanded = 0;
	for (const FSandContact& Contact : Contacts)
	{
		SandBoxScratch.Add(Contact.Box);
		// (It came down there: whoever is under it is pushed down and crushed against what is below.)
		ShovePenguinsOutOfSand(Contact.Box, Contact.BottomZ, Contact.bLanded, true, Contact.Speed, Contact.CausedBy, Contact.bIcebergIce, Contact.Span * CellSize);
		if (!Contact.bLanded)
		{
			continue;
		}
		++NumLanded;
		Hardest = FMath::Max(Hardest, Contact.Speed);
		// Snow puffs where it hits the ground, not where it lands on sand that landed with it.
		const bool bOnLanded = Contacts.ContainsByPredicate([&Contact](const FSandContact& Other)
		{
			return Other.bLanded && Other.Cell.Y + Other.Span == Contact.Cell.Y && Other.Cell.X < Contact.Cell.X + Contact.Span && Contact.Cell.X < Other.Cell.X + Other.Span;
		});
		if (!bOnLanded)
		{
			APinguFX::GroundPuffs(this, FVector(Contact.Box.GetCenter().X, 0.f, Contact.BottomZ), FMath::Clamp(Contact.Speed / SandHardLandingSpeed, 0.3f, 1.2f));
		}
	}
	if (NumLanded > 0)
	{
		// One thud for all that landed now, heavier the faster they came down.
		const float Impact = FMath::Clamp(Hardest / SandHardLandingSpeed, 0.3f, 1.f);
		APinguFX::Sound(this, EPinguSound::IcebergCrash, SandLandVolume * Impact, 0.08f, SandLandPitch);
		APinguFX::Shake(this, SandLandShake * Impact);
	}
	SmashProjectilesInSand(SandBoxScratch);
	if (bRemoved)
	{
		SandBlocks.RemoveAll([](const FSandBlock& Block) { return Block.bDone; });
		bSandInstancesDirty = true;
		bNetDirty = true;
	}
}

bool ABlockIsland::PressUnderFallingIce(APenguinCharacter* Penguin, const FBox& Box, float FallSpeed, APenguinCharacter* CausedBy, bool& bOutCrushed)
{
	using namespace BlockIslandPrivate;
	bOutCrushed = false;
	const UCapsuleComponent* Capsule = Penguin ? Penguin->GetCapsuleComponent() : nullptr;
	if (!CanEdit() || !Capsule || !Penguin->IsAlive() || Penguin->IsHidden() || Penguin == CausedBy || Penguin->IsShielded() || Penguin->IsVictorious())
	{
		return false;
	}
	// The same rule as falling sand's (ShovePenguinsOutOfSand): its middle in the column, under the block's middle.
	const FVector Loc = Penguin->GetActorLocation();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float Half = Capsule->GetScaledCapsuleHalfHeight();
	const float BottomZ = static_cast<float>(Box.Min.Z);
	const float MiddleZ = static_cast<float>(Box.Min.Z + Box.Max.Z) * 0.5f;
	const float MiddleX = static_cast<float>(Box.Min.X + Box.Max.X) * 0.5f;
	const bool bTouches = Loc.X + Radius > Box.Min.X + SandOverlapMargin && Loc.X - Radius < Box.Max.X - SandOverlapMargin
		&& Loc.Z + Half > Box.Min.Z + SandOverlapMargin && Loc.Z - Half < Box.Max.Z - SandOverlapMargin;
	const bool bOnTop = static_cast<float>(Loc.Z) - Half >= MiddleZ;
	if (!bTouches || bOnTop || Loc.X < Box.Min.X || Loc.X > Box.Max.X || static_cast<float>(Loc.Z) >= MiddleZ)
	{
		return false;
	}
	const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
	const FVector Velocity = Move ? Move->Velocity : FVector::ZeroVector;
	const float TargetZ = BottomZ - Half - SandCarryGap;
	const float Feet = static_cast<float>(Loc.Z) - Half;
	const float TargetFeet = TargetZ - Half;
	float GroundZ = 0.f;
	const bool bPressed = FindGroundUnderCapsule(Loc, Radius, Half, Feet - TargetFeet + SandPressTolerance, GroundZ) && GroundZ >= TargetFeet - SandPressTolerance;
	if (!bPressed)
	{
		// Down with it (NetTeleport also moves a LAN player's penguin on its own machine, which owns its position).
		Penguin->NetTeleport(FVector(Loc.X, 0.f, TargetZ), FVector(Velocity.X, 0.f, FMath::Min(-FallSpeed, static_cast<float>(Velocity.Z))), true);
		return true;
	}
	FPinguFXBroadcastScope Broadcast(this);
	bOutCrushed = CrushUnderSand(Penguin, CausedBy, MiddleX, false);
	return true;
}

void ABlockIsland::LandSandBlock(const FSandBlock& Block, const FIntPoint& Cell)
{
	const int32 Span = FMath::Max(Block.Span, 1);
	for (int32 Dx = 0; Dx < Span; ++Dx)
	{
		for (int32 Dz = 0; Dz < Span; ++Dz)
		{
			const FIntPoint Part(Cell.X + Dx, Cell.Y + Dz);
			Cells.Add(Part, Block.Kind);
			SandCells.Add(Part);
			AddedCells.Add(Part);
		}
	}
	if (Span > 1)
	{
		MakeBigBlock(Cell);
	}
	MarkCellsChanged();
}

void ABlockIsland::ShovePenguinsOutOfSand(const FBox& World, float BlockBottomZ, bool bLanded, bool bFell, float FallSpeed, APenguinCharacter* CausedBy,
	bool bIcebergIce, float BlockHeight)
{
	using namespace BlockIslandPrivate;
	UWorld* MyWorld = GetWorld();
	if (!MyWorld)
	{
		return;
	}
	const float BlockMiddleZ = BlockBottomZ + (BlockHeight > 0.f ? BlockHeight : CellSize) * 0.5f;
	const float BoxMiddleX = static_cast<float>(World.Min.X + World.Max.X) * 0.5f;
	for (TActorIterator<APenguinCharacter> It(MyWorld); It; ++It)
	{
		// (A penguin crushed by an earlier block this frame is out: IsAlive below.)
		APenguinCharacter* Penguin = *It;
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		if (!Penguin->IsAlive() || Penguin->IsHidden() || !Capsule)
		{
			continue;
		}
		const FVector Loc = Penguin->GetActorLocation();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const float Half = Capsule->GetScaledCapsuleHalfHeight();
		if (Loc.X + Radius <= World.Min.X + SandOverlapMargin || Loc.X - Radius >= World.Max.X - SandOverlapMargin
			|| Loc.Z + Half <= World.Min.Z + SandOverlapMargin || Loc.Z - Half >= World.Max.Z - SandOverlapMargin)
		{
			continue;
		}
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const FVector Velocity = Move ? Move->Velocity : FVector::ZeroVector;
		const bool bOnTop = static_cast<float>(Loc.Z) - Half >= BlockMiddleZ;

		// A block that came down onto a penguin (its middle in the column, under the block's middle) pushes it down with it,
		// kept just under the block, and crushes it once that presses it against what is below. Its maker, a shell and the
		// round winner are moved out of it below instead (as is one the hit did not knock out). Friendly fire: the maker's
		// teammates are crushed like anybody else (only the maker is spared, as by its own iceberg).
		const bool bUnder = bFell && !bOnTop && Loc.X >= World.Min.X && Loc.X <= World.Max.X && static_cast<float>(Loc.Z) < BlockMiddleZ;
		if (bUnder && Penguin != CausedBy && !Penguin->IsShielded() && !Penguin->IsVictorious())
		{
			const float TargetZ = BlockBottomZ - Half - SandCarryGap;
			const float Feet = static_cast<float>(Loc.Z) - Half;
			const float TargetFeet = TargetZ - Half;
			float GroundZ = 0.f;
			// Landed, the block rests on what is below it: whoever is under its middle is pressed against that.
			const bool bPressed = bLanded
				|| (FindGroundUnderCapsule(Loc, Radius, Half, Feet - TargetFeet + SandPressTolerance, GroundZ) && GroundZ >= TargetFeet - SandPressTolerance);
			if (!bPressed)
			{
				// NetTeleport also moves a LAN player's penguin on its own machine (whose simulation owns its position).
				Penguin->NetTeleport(FVector(Loc.X, 0.f, TargetZ), FVector(Velocity.X, 0.f, FMath::Min(-FallSpeed, static_cast<float>(Velocity.Z))), true);
				continue;
			}
			if (CrushUnderSand(Penguin, CausedBy, BoxMiddleX, bIcebergIce))
			{
				continue;
			}
		}

		bool bMoved = false;
		if (!bOnTop)
		{
			// Under it (or beside it): shoved out sideways - to the side it is on, or the way it was going when it is
			// right under the middle - onto ground (not into a pit or the sea), with a little hop. In the air it only
			// loses its upward speed (a bonk on the head). The shove itself is never a knock-out, never a stun.
			float Side = Loc.X < BoxMiddleX ? -1.f : 1.f;
			if (FMath::Abs(static_cast<float>(Loc.X) - BoxMiddleX) < SandShoveCenterBand && FMath::Abs(static_cast<float>(Velocity.X)) > 1.f)
			{
				Side = FMath::Sign(static_cast<float>(Velocity.X));
			}
			const bool bOnGround = Move && Move->IsMovingOnGround();
			for (const float Dir : { Side, -Side })
			{
				const FVector Target(Dir < 0.f ? World.Min.X - Radius - SandShoveGap : World.Max.X + Radius + SandShoveGap, 0.f, Loc.Z);
				float GroundZ = 0.f;
				if (!HasRoomForCapsule(Target, Radius, Half)
					|| (bOnGround && !FindGroundBelow(Target - FVector(0.f, 0.f, Half - SandShoveGap), SandShoveMaxDropCells * CellSize, GroundZ)))
				{
					continue;
				}
				// NetTeleport also moves a LAN player's penguin on its own machine (whose simulation owns its position).
				if (FMath::Abs(static_cast<float>(Target.X - Loc.X)) < SandNudgeDistance)
				{
					Penguin->NetTeleport(Target, Velocity, false); // only grazing it: set aside, nothing else
				}
				else
				{
					// Already going that way: it keeps its speed (at least the shove); else just the shove.
					const float AlongX = static_cast<float>(Velocity.X) * Dir;
					const FVector Shove(Dir * FMath::Max(AlongX, SandShoveSpeed), 0.f, bOnGround ? SandShoveHop : FMath::Min(static_cast<float>(Velocity.Z), 0.f));
					Penguin->NetTeleport(Target, Shove, true);
					APinguFX::Puffs(this, FVector(Target.X - Dir * Radius, 0.f, Loc.Z + Half * 0.5f), 3, 24.f, 160.f, FVector(Dir, 0.f, 0.3f), 0.8f, IceDustColor);
					APinguFX::Sound(this, EPinguSound::SnowballHit, SandBonkVolume, 0.08f, SandBonkPitch);
				}
				bMoved = true;
				break;
			}
		}
		if (bMoved || !bLanded)
		{
			// Done - or riding down on a falling block (it drops away under its feet by itself), or with no room beside a
			// falling one (lifted out once it lands).
			continue;
		}
		// Up onto the landed block (and whatever is stacked on it), with head room in every column the capsule touches.
		int32 TopRow = WorldToCell(FVector(BoxMiddleX, 0.f, World.Max.Z - SandShoveGap)).Y;
		const int32 MinColumn = WorldToCell(FVector(Loc.X - Radius + SandShoveGap, 0.f, Loc.Z)).X;
		const int32 MaxColumn = WorldToCell(FVector(Loc.X + Radius - SandShoveGap, 0.f, Loc.Z)).X;
		const int32 HeadCells = FMath::CeilToInt(2.f * Half / CellSize);
		for (int32 Guard = 0; Guard < SandMaxClimbRows; ++Guard)
		{
			bool bBlocked = false;
			for (int32 X = MinColumn; X <= MaxColumn && !bBlocked; ++X)
			{
				for (int32 H = 1; H <= HeadCells && !bBlocked; ++H)
				{
					bBlocked = IsSolidCell(FIntPoint(X, TopRow + H));
				}
			}
			if (!bBlocked)
			{
				break;
			}
			++TopRow;
		}
		// Pulled up from under it with a little upward speed; one that stood on it (sunk in a little) is only set on top.
		const float TopZ = static_cast<float>(GetActorLocation().Z) + (TopRow + 1) * CellSize;
		const float LiftZ = FMath::Max(static_cast<float>(Velocity.Z), bOnTop ? 0.f : SandLiftSpeed);
		Penguin->NetTeleport(FVector(Loc.X, 0.f, TopZ + Half + SandShoveGap), FVector(Velocity.X, 0.f, LiftZ), true);
	}
}

void ABlockIsland::SmashProjectilesInSand(const TArray<FBox>& Boxes)
{
	if (Boxes.Num() == 0)
	{
		return;
	}
	TArray<APenguinProjectile*> Projectiles;
	APenguinProjectile::GetAll(GetWorld(), Projectiles);
	for (APenguinProjectile* Projectile : Projectiles)
	{
		if (!IsValid(Projectile))
		{
			continue;
		}
		// An icicle peg by its top (it sticks out of a wall into the column), anything else by its ball.
		const bool bPeg = Projectile->HasPeg();
		const FVector Where = bPeg ? Projectile->GetPegTop() : Projectile->GetActorLocation();
		const float Reach = bPeg ? 0.f : Projectile->GetRadius();
		for (const FBox& Box : Boxes)
		{
			if (Where.X + Reach > Box.Min.X && Where.X - Reach < Box.Max.X && Where.Z + Reach > Box.Min.Z && Where.Z - Reach < Box.Max.Z)
			{
				Projectile->Shatter(); // bombs go off, the rest breaks (with its own effects)
				break;
			}
		}
	}
}

bool ABlockIsland::HasRoomForCapsule(const FVector& Location, float Radius, float HalfHeight) const
{
	// The cells its box covers, a hair inside the edges (touching a block is fine).
	const float Inset = BlockIslandPrivate::SandOverlapMargin;
	const FIntPoint Low = WorldToCell(FVector(Location.X - Radius + Inset, 0.f, Location.Z - HalfHeight + Inset));
	const FIntPoint High = WorldToCell(FVector(Location.X + Radius - Inset, 0.f, Location.Z + HalfHeight - Inset));
	for (int32 X = Low.X; X <= High.X; ++X)
	{
		for (int32 Z = Low.Y; Z <= High.Y; ++Z)
		{
			if (IsSolidCell(FIntPoint(X, Z)))
			{
				return false;
			}
		}
	}
	// Nor under sand that falls or is about to (an iceberg falls as two columns: never shoved from one into the other).
	const FVector Origin = GetActorLocation();
	const float Feet = static_cast<float>(Location.Z) - HalfHeight;
	for (const FSandBlock& Block : SandBlocks)
	{
		const float Size = FMath::Max(Block.Span, 1) * CellSize;
		const float BlockLeft = static_cast<float>(Origin.X) + Block.Start.X * CellSize;
		if (!Block.bDone && Location.X + Radius > BlockLeft + Inset && Location.X - Radius < BlockLeft + Size - Inset
			&& static_cast<float>(Origin.Z) + Block.Bottom + Size > Feet + Inset)
		{
			return false;
		}
	}
	return true;
}

int32 ABlockIsland::SmashProjectilesInCells(const TArray<FIntPoint>& InCells)
{
	if (!CanEdit() || InCells.Num() == 0)
	{
		return 0;
	}
	TArray<FBox> Boxes;
	const FVector Origin = GetActorLocation();
	for (const FIntPoint& Cell : InCells)
	{
		const FVector Low(Origin.X + Cell.X * CellSize, 0.f, Origin.Z + Cell.Y * CellSize);
		Boxes.Add(FBox(Low, Low + FVector(CellSize, 0.f, CellSize)));
	}
	TArray<APenguinProjectile*> Before;
	APenguinProjectile::GetAll(GetWorld(), Before);
	int32 Alive = 0;
	for (const APenguinProjectile* Projectile : Before)
	{
		Alive += IsValid(Projectile) && !Projectile->IsActorBeingDestroyed() ? 1 : 0;
	}
	FPinguFXBroadcastScope Broadcast(this);
	SmashProjectilesInSand(Boxes);
	int32 After = 0;
	for (const APenguinProjectile* Projectile : Before)
	{
		After += IsValid(Projectile) && !Projectile->IsActorBeingDestroyed() ? 1 : 0;
	}
	return FMath::Max(Alive - After, 0);
}

int32 ABlockIsland::PushPenguinsOutOfBlocks(const TArray<FIntPoint>& InCells, float LiftSpeed)
{
	using namespace BlockIslandPrivate;
	UWorld* MyWorld = GetWorld();
	if (!CanEdit() || !MyWorld || InCells.Num() == 0)
	{
		return 0;
	}
	const TSet<FIntPoint> NewCells(InCells);
	const FVector Origin = GetActorLocation();
	FPinguFXBroadcastScope Broadcast(this);
	int32 Moved = 0;
	for (TActorIterator<APenguinCharacter> It(MyWorld); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		if (!Penguin->IsAlive() || Penguin->IsHidden() || !Capsule)
		{
			continue;
		}
		const FVector Loc = Penguin->GetActorLocation();
		const float Radius = Capsule->GetScaledCapsuleRadius();
		const float Half = Capsule->GetScaledCapsuleHalfHeight();
		// The new cells its box reaches into (more than a graze), and how far they go at its height.
		int32 TopRow = TNumericLimits<int32>::Lowest();
		float NewLeft = TNumericLimits<float>::Max();
		float NewRight = TNumericLimits<float>::Lowest();
		bool bMiddleIn = false;
		const int32 MiddleColumn = WorldToCell(Loc).X;
		for (const FIntPoint& Cell : InCells)
		{
			const float Left = static_cast<float>(Origin.X) + Cell.X * CellSize;
			const float Bottom = static_cast<float>(Origin.Z) + Cell.Y * CellSize;
			if (Loc.X + Radius <= Left + PushOutMargin || Loc.X - Radius >= Left + CellSize - PushOutMargin
				|| Loc.Z + Half <= Bottom + PushOutMargin || Loc.Z - Half >= Bottom + CellSize - PushOutMargin)
			{
				continue;
			}
			TopRow = FMath::Max(TopRow, Cell.Y);
			bMiddleIn |= Cell.X == MiddleColumn;
		}
		if (TopRow == TNumericLimits<int32>::Lowest())
		{
			continue;
		}
		// The wall's extent along X at its height (every new cell in the rows the capsule spans).
		const int32 LowRow = WorldToCell(FVector(Loc.X, 0.f, Loc.Z - Half + PushOutMargin)).Y;
		const int32 HighRow = WorldToCell(FVector(Loc.X, 0.f, Loc.Z + Half - PushOutMargin)).Y;
		for (const FIntPoint& Cell : InCells)
		{
			if (Cell.Y >= LowRow && Cell.Y <= HighRow)
			{
				NewLeft = FMath::Min(NewLeft, static_cast<float>(Origin.X) + Cell.X * CellSize);
				NewRight = FMath::Max(NewRight, static_cast<float>(Origin.X) + (Cell.X + 1) * CellSize);
			}
		}
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const FVector Velocity = Move ? Move->Velocity : FVector::ZeroVector;
		const bool bOnGround = Move && Move->IsMovingOnGround();

		// Up: onto the new blocks, with head room in every column the capsule touches. Clean only if it climbs past nothing
		// but new blocks (a ceiling right over them would lift it through that).
		const int32 MinColumn = WorldToCell(FVector(Loc.X - Radius + PushOutMargin, 0.f, Loc.Z)).X;
		const int32 MaxColumn = WorldToCell(FVector(Loc.X + Radius - PushOutMargin, 0.f, Loc.Z)).X;
		const int32 HeadCells = FMath::CeilToInt(2.f * Half / CellSize);
		int32 UpRow = TopRow;
		bool bUpClean = true;
		for (int32 Guard = 0; Guard < SandMaxClimbRows; ++Guard)
		{
			bool bBlocked = false;
			for (int32 X = MinColumn; X <= MaxColumn; ++X)
			{
				for (int32 H = 1; H <= HeadCells; ++H)
				{
					const FIntPoint Over(X, UpRow + H);
					if (IsSolidCell(Over))
					{
						bBlocked = true;
						bUpClean &= NewCells.Contains(Over);
					}
				}
			}
			if (!bBlocked)
			{
				break;
			}
			++UpRow;
		}
		const float UpZ = static_cast<float>(Origin.Z) + (UpRow + 1) * CellSize + Half + PushOutGap;
		const FVector UpTarget(Loc.X, 0.f, UpZ);
		const FVector UpVelocity(Velocity.X, 0.f, FMath::Max(static_cast<float>(Velocity.Z), LiftSpeed));

		// Aside: just past the wall at its height, toward the side it is on (where there is room, onto ground when it stood on
		// ground: no pit, no sea).
		const float MiddleX = 0.5f * (NewLeft + NewRight);
		const float Side = Loc.X < MiddleX ? -1.f : 1.f;
		const auto SideTarget = [&](float Dir, FVector& OutTarget) -> bool
		{
			if (NewLeft > NewRight)
			{
				return false;
			}
			OutTarget = FVector(Dir < 0.f ? NewLeft - Radius - PushOutGap : NewRight + Radius + PushOutGap, 0.f, Loc.Z);
			float GroundZ = 0.f;
			return HasRoomForCapsule(OutTarget, Radius, Half)
				&& (!bOnGround || FindGroundBelow(OutTarget - FVector(0.f, 0.f, Half - PushOutGap), SandShoveMaxDropCells * CellSize, GroundZ));
		};
		const auto MoveAside = [&](const FVector& Target, float Dir)
		{
			if (FMath::Abs(static_cast<float>(Target.X - Loc.X)) < PushOutNudge)
			{
				Penguin->NetTeleport(Target, Velocity, false); // only grazing it: set aside, nothing else
				return;
			}
			const float AlongX = static_cast<float>(Velocity.X) * Dir;
			Penguin->NetTeleport(Target, FVector(Dir * FMath::Max(AlongX, SandShoveSpeed), 0.f, bOnGround ? SandShoveHop : FMath::Min(static_cast<float>(Velocity.Z), 0.f)), true);
			APinguFX::Puffs(this, FVector(Target.X - Dir * Radius, 0.f, Loc.Z + Half * 0.5f), 3, 24.f, 160.f, FVector(Dir, 0.f, 0.3f), 0.8f, IceDustColor);
		};

		FVector Aside = FVector::ZeroVector;
		float AsideDir = Side;
		bool bAside = SideTarget(Side, Aside);
		if (!bAside && SideTarget(-Side, Aside))
		{
			bAside = true;
			AsideDir = -Side;
		}
		// Its middle in a column of them: up (the wall lifts it). Only reaching in from the side: aside, or up if that is
		// shorter (it barely stands on them). Up through a ceiling is the last thing.
		bool bUp = bUpClean;
		if (bUpClean && bAside && !bMiddleIn)
		{
			bUp = FMath::Abs(UpZ - static_cast<float>(Loc.Z)) < FMath::Abs(static_cast<float>(Aside.X - Loc.X));
		}
		else if (!bUpClean)
		{
			bUp = !bAside;
		}
		if (bUp)
		{
			Penguin->NetTeleport(UpTarget, UpVelocity, true);
		}
		else
		{
			MoveAside(Aside, AsideDir);
		}
		UE_LOG(LogPingu, Log, TEXT("[ISLAND] %s pushed out of new blocks: %s (middle in a column %d, up clean %d, aside %d), %.0f cm"), *Penguin->GetDisplayName(),
			bUp ? TEXT("up") : TEXT("aside"), bMiddleIn ? 1 : 0, bUpClean ? 1 : 0, bAside ? 1 : 0, bUp ? UpZ - Loc.Z : FMath::Abs(Aside.X - Loc.X));
		++Moved;
	}
	return Moved;
}

void ABlockIsland::RiseBlocks(const TArray<FIntPoint>& InCells, float Duration)
{
	if (!CanEdit() || InCells.Num() == 0 || Duration <= 0.f)
	{
		return;
	}
	int32 FloorRow = TNumericLimits<int32>::Max();
	for (const FIntPoint& Cell : InCells)
	{
		FloorRow = FMath::Min(FloorRow, Cell.Y);
	}
	const double Now = GetNetTime();
	for (const FIntPoint& Cell : InCells)
	{
		if (Cells.Contains(Cell))
		{
			FRiseCell& Rise = RisingCells.Add(Cell);
			Rise.FloorRow = FloorRow;
			Rise.Start = Now;
			Rise.Duration = Duration;
		}
	}
	bNetDirty = true;
}

void ABlockIsland::RebuildSandInstances()
{
	using namespace BlockIslandPrivate;
	bSandInstancesDirty = false;
	SandInstanceEntries.SetNum(FallingBlocks.Num());
	for (TArray<int32>& Entries : SandInstanceEntries)
	{
		Entries.Reset();
	}
	for (int32 Index = 0; Index < SandBlocks.Num(); ++Index)
	{
		const int32 Slot = SandSlot(SandBlocks[Index].Kind);
		if (!SandBlocks[Index].bDone && SandInstanceEntries.IsValidIndex(Slot))
		{
			SandInstanceEntries[Slot].Add(Index);
		}
	}
	for (int32 Slot = 0; Slot < FallingBlocks.Num(); ++Slot)
	{
		UInstancedStaticMeshComponent* Instances = FallingBlocks[Slot];
		if (!Instances)
		{
			continue;
		}
		if (Instances->GetInstanceCount() > 0)
		{
			Instances->ClearInstances();
		}
		if (SandInstanceEntries[Slot].Num() == 0)
		{
			continue;
		}
		TransformScratch.Reset();
		TArray<int32> LookSeeds;
		for (const int32 Index : SandInstanceEntries[Slot])
		{
			// A block per depth row; a big block as one scaled-up block (a few along the depth axis).
			const FSandBlock& Block = SandBlocks[Index];
			const int32 Span = FMath::Max(Block.Span, 1);
			for (int32 Row = 0; Row < GetDepthInstances(Span); ++Row)
			{
				TransformScratch.Add(MakeBlockInstance(Block.Start, Span, Row, Block.Bottom, Span * CellSize));
				LookSeeds.Add(StableLookSeed(Block.Start, Row, Seed));
			}
		}
		ApplyStableLookSeeds(Instances, LookSeeds);
		Instances->AddInstances(TransformScratch, false);
	}
}

void ABlockIsland::UpdateSandVisuals(double Now)
{
	using namespace BlockIslandPrivate;
	if (bSandInstancesDirty)
	{
		RebuildSandInstances();
	}
	if (SandBlocks.Num() == 0)
	{
		return;
	}
	const float Scale = CellSize / 100.f;
	for (int32 Slot = 0; Slot < FallingBlocks.Num() && Slot < SandInstanceEntries.Num(); ++Slot)
	{
		UInstancedStaticMeshComponent* Instances = FallingBlocks[Slot];
		const TArray<int32>& Entries = SandInstanceEntries[Slot];
		int32 Expected = 0;
		for (const int32 Index : Entries)
		{
			Expected += GetDepthInstances(FMath::Max(SandBlocks[Index].Span, 1));
		}
		if (!Instances || Entries.Num() == 0 || Instances->GetInstanceCount() != Expected)
		{
			continue;
		}
		TransformScratch.Reset();
		for (const int32 Index : Entries)
		{
			const FSandBlock& Block = SandBlocks[Index];
			const int32 Span = FMath::Max(Block.Span, 1);
			const float Wait = static_cast<float>(Block.StartTime - Now);
			for (int32 Row = 0; Row < GetDepthInstances(Span); ++Row)
			{
				FTransform Instance = MakeBlockInstance(Block.Start, Span, Row, Block.Bottom, Span * CellSize);
				if (Wait > 0.f)
				{
					// Shaking before it falls, harder toward the drop.
					const float Alpha = 1.f - FMath::Clamp(Wait / SandWarnTime, 0.f, 1.f);
					const float Phase = Wait * SandShakeRate + Block.Start.X * 1.7f + Block.Start.Y * 2.3f + Row;
					const float Amp = (SandShakeMin + SandShakeGrow * Alpha) * Scale;
					Instance.AddToTranslation(FVector(FMath::Sin(Phase) * Amp, 0.f, FMath::Cos(Phase * 1.3f) * Amp * 0.5f));
					Instance.SetRotation(FRotator(0.f, 0.f, FMath::Sin(Phase * 0.7f) * SandShakeTilt * Alpha).Quaternion());
				}
				TransformScratch.Add(Instance);
			}
		}
		// One batched update; only the instance data goes to the GPU (no render state rebuild).
		Instances->BatchUpdateInstancesTransforms(0, TransformScratch, false, false, true);
		APinguFX::MarkInstancesChanged(Instances);
	}
}

FIntPoint ABlockIsland::WorldToCell(const FVector& World) const
{
	const FVector Local = World - GetActorLocation();
	return FIntPoint(FMath::FloorToInt(Local.X / CellSize), FMath::FloorToInt(Local.Z / CellSize));
}

FVector ABlockIsland::CellToWorld(const FIntPoint& Cell) const
{
	FVector World = GetActorLocation() + CellCenter(Cell.X, Cell.Y);
	World.Y = 0.f;
	return World;
}

bool ABlockIsland::FindGroundBelow(const FVector& From, float MaxDrop, float& OutGroundZ) const
{
	const FIntPoint Start = WorldToCell(From);
	const int32 Steps = FMath::CeilToInt(FMath::Max(MaxDrop, 0.f) / CellSize) + 1;
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const FIntPoint Cell(Start.X, Start.Y - Step);
		if (IsSolidCell(Cell))
		{
			OutGroundZ = GetActorLocation().Z + (Cell.Y + 1) * CellSize;
			return OutGroundZ <= From.Z + 1.f;
		}
	}
	return false;
}

bool ABlockIsland::FindGroundUnderCapsule(const FVector& Location, float Radius, float HalfHeight, float MaxDrop, float& OutGroundZ) const
{
	// Under its middle and under both sides (a hair inside them: a block it only touches at the side is not under it).
	const float Feet = static_cast<float>(Location.Z) - HalfHeight;
	const float Side = FMath::Max(Radius - BlockIslandPrivate::SandOverlapMargin, 0.f);
	bool bFound = false;
	for (const float Dx : { -Side, 0.f, Side })
	{
		float GroundZ = 0.f;
		if (FindGroundBelow(FVector(Location.X + Dx, 0.f, Feet + 1.f), MaxDrop + 1.f, GroundZ) && (!bFound || GroundZ > OutGroundZ))
		{
			OutGroundZ = GroundZ;
			bFound = true;
		}
	}
	return bFound;
}

FBox ABlockIsland::GetBounds() const
{
	if (!bBoundsValid)
	{
		bBoundsValid = true;
		CachedLocalBounds = FBox(ForceInit);
		for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
		{
			const FVector Min(Pair.Key.X * CellSize, 0.f, Pair.Key.Y * CellSize);
			CachedLocalBounds += Min;
			CachedLocalBounds += Min + FVector(CellSize, 0.f, CellSize);
		}
	}
	if (!CachedLocalBounds.IsValid)
	{
		return CachedLocalBounds;
	}
	const FVector Origin = GetActorLocation();
	return FBox(Origin + CachedLocalBounds.Min, Origin + CachedLocalBounds.Max);
}

bool ABlockIsland::GetBaseFrame(FBox& OutBounds, float& OutGroundMinZ, float& OutGroundMaxZ) const
{
	if (!bHasBase || !BaseLocalBounds.IsValid)
	{
		return false;
	}
	const FVector Origin = GetActorLocation();
	OutBounds = FBox(Origin + BaseLocalBounds.Min, Origin + BaseLocalBounds.Max);
	OutGroundMinZ = BaseGroundMinZ;
	OutGroundMaxZ = BaseGroundMaxZ;
	return true;
}

TArray<FIntPoint> ABlockIsland::AddBlocks(const TArray<FIntPoint>& InCells, EBlockKind Kind, float Lifetime, const TArray<FIntPoint>* BigOrigins)
{
	TArray<FIntPoint> Added;
	if (!CanEdit() || Kind == EBlockKind::None)
	{
		return Added;
	}
	const double Now = Lifetime > 0.f ? GetNetTime() : 0.0;
	for (const FIntPoint& Cell : InCells)
	{
		if (IsSolidCell(Cell))
		{
			continue;
		}
		Cells.Add(Cell, Kind);
		AddedCells.Add(Cell);
		if (Lifetime > 0.f)
		{
			FTimedCell& Timed = TempCells.Add(Cell);
			Timed.Left = Lifetime;
			Timed.At = Now + Lifetime;
		}
		Added.Add(Cell);
	}
	if (Added.Num() > 0)
	{
		MarkCellsChanged();
	}
	// The big blocks asked for whose cells all went in now.
	if (BigOrigins && Added.Num() > 0)
	{
		const TSet<FIntPoint> AddedSet(Added);
		for (const FIntPoint& Origin : *BigOrigins)
		{
			bool bWhole = true;
			for (int32 Dx = 0; Dx < BigBlockCells && bWhole; ++Dx)
			{
				for (int32 Dz = 0; Dz < BigBlockCells && bWhole; ++Dz)
				{
					bWhole = AddedSet.Contains(FIntPoint(Origin.X + Dx, Origin.Y + Dz));
				}
			}
			if (bWhole)
			{
				MakeBigBlock(Origin);
			}
		}
	}
	return Added;
}

int32 ABlockIsland::FreezeBlocksInRadius(const FVector& Center, float Radius, bool /*bRebuild*/)
{
	if (!CanEdit())
	{
		return 0;
	}
	const FIntPoint C = WorldToCell(Center);
	const int32 Reach = FMath::CeilToInt(Radius / CellSize) + 1;
	int32 Frozen = 0;
	for (int32 X = C.X - Reach; X <= C.X + Reach; ++X)
	{
		for (int32 Z = C.Y - Reach; Z <= C.Y + Reach; ++Z)
		{
			EBlockKind* Kind = Cells.Find(FIntPoint(X, Z));
			if (!Kind || *Kind != EBlockKind::Snow || IsSolidCell(FIntPoint(X, Z + 1)))
			{
				continue; // only snow with open air above it
			}
			const FVector World = CellToWorld(FIntPoint(X, Z));
			if (FVector::DistSquared2D(FVector(World.X, World.Z, 0.f), FVector(Center.X, Center.Z, 0.f)) > Radius * Radius)
			{
				continue;
			}
			*Kind = EBlockKind::Ice;
			++Frozen;
		}
	}
	if (Frozen > 0)
	{
		MarkCellsChanged();
	}
	return Frozen;
}

int32 ABlockIsland::RemoveBlocksInRadius(const FVector& Center, float Radius, TArray<FVector>* OutRemovedCenters)
{
	if (!CanEdit())
	{
		return 0;
	}
	const FIntPoint C = WorldToCell(Center);
	const int32 Reach = FMath::CeilToInt(Radius / CellSize) + 1;
	int32 Removed = 0;
	for (int32 X = C.X - Reach; X <= C.X + Reach; ++X)
	{
		for (int32 Z = C.Y - Reach; Z <= C.Y + Reach; ++Z)
		{
			const FIntPoint Cell(X, Z);
			if (!IsSolidCell(Cell))
			{
				continue;
			}
			const FVector World = CellToWorld(Cell);
			if (FVector::DistSquared2D(FVector(World.X, World.Z, 0.f), FVector(Center.X, Center.Z, 0.f)) <= Radius * Radius)
			{
				// A big block goes whole (its cells outside the radius too).
				const FSandPiece Piece = GetPieceOf(Cell);
				DissolveBigBlock(Piece.Origin);
				for (int32 Dx = 0; Dx < Piece.Span; ++Dx)
				{
					for (int32 Dz = 0; Dz < Piece.Span; ++Dz)
					{
						const FIntPoint Part = Piece.Span > 1 ? FIntPoint(Piece.Origin.X + Dx, Piece.Origin.Y + Dz) : Cell;
						if (!RemoveCell(Part))
						{
							continue;
						}
						if (OutRemovedCenters)
						{
							OutRemovedCenters->Add(CellToWorld(Part));
						}
						++Removed;
					}
				}
			}
		}
	}
	if (Removed > 0)
	{
		BrokenBlocks += Removed;
		MarkCellsChanged();
	}
	return Removed;
}

void ABlockIsland::ClearTemporaryBlocks()
{
	if (!CanEdit() || (AddedCells.Num() == 0 && TempCells.Num() == 0 && SandBlocks.Num() == 0))
	{
		return;
	}
	// Every block added at runtime - temporary or permanent ice, sand (shaking and falling too). Generated blocks and
	// frozen snow stay.
	for (const FIntPoint& Cell : AddedCells)
	{
		Cells.Remove(Cell);
		Crumbling.Remove(Cell);
	}
	for (const TPair<FIntPoint, FTimedCell>& Pair : TempCells)
	{
		Cells.Remove(Pair.Key);
		Crumbling.Remove(Pair.Key);
	}
	TempCells.Reset();
	AddedCells.Reset();
	SandCells.Reset();
	SandBlocks.Reset();
	SandHiddenCells.Reset();
	BigBlocks.Reset();
	BigBlockOfCell.Reset();
	RisingCells.Reset();
	bSandInstancesDirty = true;
	MarkCellsChanged();
}

// ---------------------------------------------------------------------------- LAN

bool ABlockIsland::CanEdit() const
{
	return HasAuthority() && GetNetMode() != NM_Client;
}

bool ABlockIsland::IsNetClient() const
{
	return GetNetMode() == NM_Client;
}

bool ABlockIsland::IsNetHost() const
{
	const ENetMode Mode = GetNetMode();
	return Mode == NM_ListenServer || Mode == NM_DedicatedServer;
}

bool ABlockIsland::IsNetSynced() const
{
	return !IsNetClient() || bNetSynced;
}

bool ABlockIsland::MatchesHostIsland() const
{
	return !IsNetClient() || (bNetSynced && !bNetMismatch);
}

double ABlockIsland::GetNetTime() const
{
	const UWorld* MyWorld = GetWorld();
	if (!MyWorld)
	{
		return 0.0;
	}
	if (const AGameStateBase* GameState = MyWorld->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return MyWorld->GetTimeSeconds();
}

bool ABlockIsland::GetLookSignatures(TMap<FIntVector, uint32>& Out) const
{
	using namespace BlockIslandPrivate;
	Out.Reset();
	if (bVisualsDirty || VisualCellKeys.Num() != VisCount)
	{
		return false;
	}
	const UInstancedStaticMeshComponent* const Components[VisCount] = { SnowBlocks, RockBlocks, DeepRockBlocks, IceBlocks, TempBlocks, CrumbleBlocks, SnowCaps, BigIceBlocks };
	const int32 Rows = FMath::Max(DepthRows, 1);
	for (int32 Slot = 0; Slot < VisCount; ++Slot)
	{
		const UInstancedStaticMeshComponent* Component = Components[Slot];
		const TArray<int32>& Keys = VisualCellKeys[Slot];
		const int32 PerCell = Slot == VisBig ? GetDepthInstances(BigBlockCells) : Rows;
		if (!Component || Component->GetInstanceCount() != Keys.Num() * PerCell)
		{
			return false;
		}
		// The numbers the renderer gives the instances: one stream from InstancingRandomSeed, started over at every index that
		// AdditionalRandomSeeds names (what FInstanceDataHelpers::GenerateInstanceRandomIDs does; that header is not public).
		TArray<float> Randoms;
		Randoms.SetNumUninitialized(Component->GetInstanceCount());
		{
			FRandomStream Stream(Component->InstancingRandomSeed);
			int32 NextSeed = 0;
			for (int32 Instance = 0; Instance < Randoms.Num(); ++Instance)
			{
				if (Component->AdditionalRandomSeeds.IsValidIndex(NextSeed) && Component->AdditionalRandomSeeds[NextSeed].StartInstanceIndex == Instance)
				{
					Stream = FRandomStream(Component->AdditionalRandomSeeds[NextSeed].RandomSeed);
					++NextSeed;
				}
				Randoms[Instance] = Stream.GetFraction();
			}
		}
		int32 Index = 0;
		for (const int32 Packed : Keys)
		{
			const FIntPoint Cell = UnpackCell(Packed);
			for (int32 Row = 0; Row < PerCell; ++Row, ++Index)
			{
				FTransform Transform;
				Component->GetInstanceTransform(Index, Transform, false);
				const FVector Place = Transform.GetTranslation();
				uint32 Hash = MixHash(static_cast<uint32>(Slot) + 1u);
				Hash = MixHash(Hash ^ FMath::RoundToInt(Randoms[Index] * 1000000.f));
				Hash = MixHash(Hash ^ static_cast<uint32>(FMath::RoundToInt(Place.X)));
				Hash = MixHash(Hash ^ static_cast<uint32>(FMath::RoundToInt(Place.Z)));
				Hash = MixHash(Hash ^ static_cast<uint32>(FMath::RoundToInt(Place.Y)));
				Hash = MixHash(Hash ^ static_cast<uint32>(FMath::RoundToInt(Transform.Rotator().Yaw)));
				Out.Add(FIntVector(Cell.X, Cell.Y, Slot * 100 + Row), Hash);
			}
		}
	}
	return true;
}

uint32 ABlockIsland::GetCellChecksum() const
{
	using namespace BlockIslandPrivate;
	uint32 Sum = static_cast<uint32>(Cells.Num()) * 0x9E3779B9u;
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		Sum += MixHash(static_cast<uint32>(PackCell(Pair.Key)) * 8u + static_cast<uint32>(Pair.Value));
	}
	// The big blocks too (the same blocks drawn differently are not the same island).
	for (const FIntPoint& Origin : BigBlocks)
	{
		Sum += MixHash(static_cast<uint32>(PackCell(Origin)) ^ 0xB16B10C5u);
	}
	return Sum;
}

void ABlockIsland::ResetNetState()
{
	// A new generation: the clients regenerate from Layout + Seed, the differences start empty.
	++NetState.Generation;
	NetState.Layout = Layout;
	NetState.Seed = Seed;
	NetState.RandomWidth = RandomWidth;
	NetState.ArenaPenguins = ArenaPenguins;
	NetState.DepthRows = DepthRows;
	NetState.CellSize = CellSize;
	NetState.Removed.Reset();
	NetRemovedSet.Reset();
	RefreshNetState();
	if (IsNetHost())
	{
		UE_LOG(LogPingu, Display, TEXT("[ISLAND] host: generation %d, %s seed %d, for %d penguins, %d blocks, checksum %08x"),
			NetState.Generation, BlockIslandPrivate::LayoutName(Layout), Seed, ArenaPenguins, Cells.Num(), GetCellChecksum());
	}
}

void ABlockIsland::RefreshNetState()
{
	using namespace BlockIslandPrivate;
	bNetDirty = false;
	if (!IsNetHost())
	{
		return; // solo: nobody to tell
	}
	// Generated blocks that are gone, appended in the order they went (the replicated array only grows).
	for (const TPair<FIntPoint, EBlockKind>& Base : BaseCells)
	{
		if (!Cells.Contains(Base.Key) && !NetRemovedSet.Contains(Base.Key))
		{
			NetRemovedSet.Add(Base.Key);
			NetState.Removed.Add(PackCell(Base.Key));
		}
	}
	// Blocks that are not as generated: new ones, ones back in a crater, frozen snow, everything that melts.
	NetState.Changed.Reset();
	for (const TPair<FIntPoint, EBlockKind>& Pair : Cells)
	{
		const EBlockKind* BaseKind = BaseCells.Find(Pair.Key);
		const FTimedCell* Temp = TempCells.Find(Pair.Key);
		if (!BaseKind || *BaseKind != Pair.Value || Temp || NetRemovedSet.Contains(Pair.Key))
		{
			FIslandNetCell& Entry = NetState.Changed.AddDefaulted_GetRef();
			Entry.Cell = PackCell(Pair.Key);
			Entry.Kind = Pair.Value;
			Entry.Time = Temp ? Temp->At : 0.0;
		}
	}
	NetState.Crumbling.Reset();
	for (const TPair<FIntPoint, FTimedCell>& Pair : Crumbling)
	{
		FIslandNetCell& Entry = NetState.Crumbling.AddDefaulted_GetRef();
		Entry.Cell = PackCell(Pair.Key);
		Entry.Kind = GetBlock(Pair.Key.X, Pair.Key.Y);
		Entry.Time = Pair.Value.At;
	}
	// Sand shaking or falling: where it falls from and when it starts (the clients run the same fall). Only when sand
	// starts shaking, starts falling, lands or splashes - never while it falls.
	NetState.Falling.Reset();
	for (const FSandBlock& Block : SandBlocks)
	{
		if (Block.bDone)
		{
			continue;
		}
		FIslandNetCell& Entry = NetState.Falling.AddDefaulted_GetRef();
		Entry.Cell = PackCell(Block.Start);
		Entry.Kind = Block.Kind;
		Entry.Time = Block.StartTime;
		Entry.Span = static_cast<uint8>(FMath::Clamp(Block.Span, 1, 255));
	}
	NetState.BigBlocks.Reset();
	for (const FIntPoint& Origin : BigBlocks)
	{
		NetState.BigBlocks.Add(PackCell(Origin));
	}
	// Rises still under way (a client joining later finds them over and ignores them).
	NetState.Rising.Reset();
	const double RefreshTime = GetNetTime();
	for (const TPair<FIntPoint, FRiseCell>& Pair : RisingCells)
	{
		if (Pair.Value.Start + Pair.Value.Duration > RefreshTime)
		{
			FIslandNetRise& Entry = NetState.Rising.AddDefaulted_GetRef();
			Entry.Cell = PackCell(Pair.Key);
			Entry.FloorRow = Pair.Value.FloorRow;
			Entry.Start = Pair.Value.Start;
			Entry.Duration = Pair.Value.Duration;
		}
	}
	NetState.Time = GetNetTime();
	NetState.bCollapsing = bCollapsing;
	if (bCollapsing)
	{
		NetState.CollapseCenterX = CollapseCenterX;
		NetState.CollapseRadius = CollapseRadius;
		NetState.CollapseTime = GetNetTime();
		NetState.CollapseSpeed = CollapseSpeed;
	}
	NetState.CollapseWarnTime = CollapseWarnTime; // (also without sudden death: a Crumbling Island's shaking, CrumbleOutside)
	NetState.BrokenBlocks = BrokenBlocks;
	NetState.Checksum = static_cast<int32>(GetCellChecksum());
	++NetState.Revision;
	ForceNetUpdate();
}

void ABlockIsland::OnRep_NetState()
{
	bNetStatePending = true;
	if (HasActorBegunPlay())
	{
		// Right away (this is before the frame's movement): the own penguin collides with the new blocks this frame.
		ApplyNetState();
		FlushEdits();
	}
}

void ABlockIsland::ApplyNetState()
{
	using namespace BlockIslandPrivate;
	bNetStatePending = false;
	const UWorld* MyWorld = GetWorld();
	if (NetState.Generation <= 0 || !MyWorld || !MyWorld->IsGameWorld())
	{
		return; // nothing from the host yet: the level's own island stays until it arrives
	}

	// The host's settings decide the generated blocks, their boxes and the backdrop.
	if (NetState.DepthRows > 0 && NetState.DepthRows != DepthRows)
	{
		DepthRows = NetState.DepthRows;
		bVisualsForceAll = true;
		MarkCellsChanged();
	}
	if (NetState.CellSize > 0.f && NetState.CellSize != CellSize)
	{
		CellSize = NetState.CellSize;
		bVisualsForceAll = true;
		MarkCellsChanged();
	}
	if (!bHasBase || BaseLayout != NetState.Layout || BaseSeed != NetState.Seed || BaseRandomWidth != NetState.RandomWidth
		|| BaseArenaPenguins != NetState.ArenaPenguins)
	{
		Layout = NetState.Layout;
		Seed = NetState.Seed;
		RandomWidth = NetState.RandomWidth;
		ArenaPenguins = NetState.ArenaPenguins; // the island's size: as many penguins as the host made it for
		GenerateCells(); // exactly the host's generated blocks (deterministic from the seed)
	}

	// The host's blocks: the generated ones, minus the removed, plus the changed.
	const double Now = GetNetTime();
	CellMapScratch = BaseCells;
	NetRemovedSet.Reset();
	for (const int32 Packed : NetState.Removed)
	{
		const FIntPoint Cell = UnpackCell(Packed);
		CellMapScratch.Remove(Cell);
		NetRemovedSet.Add(Cell);
	}
	TMap<FIntPoint, FTimedCell> NewTemp;
	TSet<FIntPoint> NewAdded;
	for (const FIslandNetCell& Entry : NetState.Changed)
	{
		const FIntPoint Cell = UnpackCell(Entry.Cell);
		if (Entry.Kind == EBlockKind::None)
		{
			continue;
		}
		CellMapScratch.Add(Cell, Entry.Kind);
		if (Entry.Time > 0.0)
		{
			FTimedCell& Timed = NewTemp.Add(Cell);
			Timed.At = Entry.Time;
			Timed.Left = FMath::Max(static_cast<float>(Entry.Time - Now), 0.f);
		}
		// Added at runtime (not a generated block, or back in a generated cell that was emptied): the ability-ice look.
		if (!BaseCells.Contains(Cell) || NetRemovedSet.Contains(Cell))
		{
			NewAdded.Add(Cell);
		}
	}
	// Sand shaking or falling: the host's start times; this machine runs the same fall (where it was drawn is kept).
	TArray<FSandBlock> NewSand;
	TMap<FIntPoint, double> NewHidden;
	for (const FIslandNetCell& Entry : NetState.Falling)
	{
		FSandBlock& Block = NewSand.AddDefaulted_GetRef();
		Block.Start = UnpackCell(Entry.Cell);
		Block.Span = FMath::Max(static_cast<int32>(Entry.Span), 1);
		Block.StartTime = Entry.Time;
		Block.Kind = Entry.Kind;
		// New here: from its cell (the first move catches up with the host's fall, landing on whatever is in the way).
		Block.Bottom = Block.Start.Y * CellSize;
		for (const FSandBlock& Old : SandBlocks)
		{
			if (Old.Start == Block.Start && Old.StartTime == Block.StartTime && Old.Span == Block.Span)
			{
				Block.Bottom = Old.Bottom;
				break;
			}
		}
		// The host had not dropped it yet when it sent this: it is still a block here, drawn by FallingBlocks (shaking,
		// then falling on this machine's clock while the cell waits for the host's next state).
		if (Entry.Time > NetState.Time && CellMapScratch.Contains(Block.Start))
		{
			for (int32 Dx = 0; Dx < Block.Span; ++Dx)
			{
				for (int32 Dz = 0; Dz < Block.Span; ++Dz)
				{
					NewHidden.Add(FIntPoint(Block.Start.X + Dx, Block.Start.Y + Dz), Entry.Time);
				}
			}
		}
	}
	bool bSandChanged = NewSand.Num() != SandBlocks.Num();
	for (int32 Index = 0; Index < NewSand.Num() && !bSandChanged; ++Index)
	{
		bSandChanged = NewSand[Index].Start != SandBlocks[Index].Start || NewSand[Index].StartTime != SandBlocks[Index].StartTime
			|| NewSand[Index].Kind != SandBlocks[Index].Kind || NewSand[Index].Span != SandBlocks[Index].Span;
	}
	if (bSandChanged)
	{
		bSandInstancesDirty = true;
	}
	if (!SameKeys(NewHidden, SandHiddenCells) || !SameCells(NewAdded, AddedCells))
	{
		MarkVisualsChanged();
	}
	SandBlocks = MoveTemp(NewSand);
	SandHiddenCells = MoveTemp(NewHidden);
	AddedCells = MoveTemp(NewAdded);
	TMap<FIntPoint, FTimedCell> NewCrumbling;
	for (const FIslandNetCell& Entry : NetState.Crumbling)
	{
		const FIntPoint Cell = UnpackCell(Entry.Cell);
		if (CellMapScratch.Contains(Cell))
		{
			FTimedCell& Timed = NewCrumbling.Add(Cell);
			Timed.At = Entry.Time;
			Timed.Left = FMath::Max(static_cast<float>(Entry.Time - Now), 0.f);
		}
	}

	if (CellMapScratch.Num() != Cells.Num() || !CellMapScratch.OrderIndependentCompareEqual(Cells))
	{
		Cells = CellMapScratch;
		MarkCellsChanged();
	}
	// The host's big blocks (each with all of its cells here).
	TSet<FIntPoint> NewBig;
	for (const int32 Packed : NetState.BigBlocks)
	{
		const FIntPoint Origin = UnpackCell(Packed);
		const EBlockKind* First = Cells.Find(Origin);
		bool bWhole = First != nullptr;
		for (int32 Dx = 0; Dx < BigBlockCells && bWhole; ++Dx)
		{
			for (int32 Dz = 0; Dz < BigBlockCells && bWhole; ++Dz)
			{
				const EBlockKind* Other = Cells.Find(FIntPoint(Origin.X + Dx, Origin.Y + Dz));
				bWhole = Other && *Other == *First;
			}
		}
		if (bWhole)
		{
			NewBig.Add(Origin);
		}
	}
	if (!SameCells(NewBig, BigBlocks))
	{
		BigBlocks = MoveTemp(NewBig);
		BigBlockOfCell.Reset();
		for (const FIntPoint& Origin : BigBlocks)
		{
			for (int32 Dx = 0; Dx < BigBlockCells; ++Dx)
			{
				for (int32 Dz = 0; Dz < BigBlockCells; ++Dz)
				{
					BigBlockOfCell.Add(FIntPoint(Origin.X + Dx, Origin.Y + Dz), Origin);
				}
			}
		}
		MarkVisualsChanged();
	}
	// Rises still under way on this machine's clock (the host's start times).
	for (const FIslandNetRise& Entry : NetState.Rising)
	{
		const FIntPoint Cell = UnpackCell(Entry.Cell);
		if (Entry.Start + Entry.Duration > Now && Cells.Contains(Cell))
		{
			FRiseCell& Rise = RisingCells.Add(Cell);
			Rise.FloorRow = Entry.FloorRow;
			Rise.Start = Entry.Start;
			Rise.Duration = Entry.Duration;
		}
	}
	for (auto It = RisingCells.CreateIterator(); It; ++It)
	{
		if (!Cells.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	if (!SameKeys(NewTemp, TempCells) || !SameKeys(NewCrumbling, Crumbling))
	{
		MarkVisualsChanged();
	}
	TempCells = MoveTemp(NewTemp);
	Crumbling = MoveTemp(NewCrumbling);

	bCollapsing = NetState.bCollapsing;
	if (NetState.CollapseWarnTime > 0.f)
	{
		CollapseWarnTime = NetState.CollapseWarnTime;
	}
	if (bCollapsing)
	{
		CollapseCenterX = NetState.CollapseCenterX;
		CollapseSpeed = NetState.CollapseSpeed;
		CollapseRadius = FMath::Max(0.f, NetState.CollapseRadius - CollapseSpeed * static_cast<float>(FMath::Max(Now - NetState.CollapseTime, 0.0)));
	}
	BrokenBlocks = NetState.BrokenBlocks;
	bNetSynced = true;

	const uint32 Checksum = GetCellChecksum();
	bNetMismatch = static_cast<int32>(Checksum) != NetState.Checksum;
	if (!bNetMismatch && NetLoggedGeneration != NetState.Generation)
	{
		NetLoggedGeneration = NetState.Generation;
		UE_LOG(LogPingu, Display, TEXT("[ISLAND] client: generation %d, %s seed %d, for %d penguins, %d blocks, checksum %08x (same as the host)"),
			NetState.Generation, LayoutName(NetState.Layout), NetState.Seed, NetState.ArenaPenguins, Cells.Num(), Checksum);
	}
}

// ---------------------------------------------------------------------------- lines through the grid

bool ABlockIsland::IsLineClear(const FVector& From, const FVector& To) const
{
	using namespace BlockIslandPrivate;
	if (Cells.Num() == 0 || CellSize <= 0.f)
	{
		return true;
	}
	// A grid walk (DDA) on the play plane, in cells relative to the island: every cell the segment passes through, from
	// From's own to To's own, in order. The end cells count like any other: the abilities test points on the penguins'
	// capsule axes, and a capsule keeps its radius away from every block, so a penguin touching a wall never blocks its
	// own lines (nothing to skip there).
	const FVector Origin = GetActorLocation();
	const double StartX = (From.X - Origin.X) / CellSize;
	const double StartZ = (From.Z - Origin.Z) / CellSize;
	const double Dx = (To.X - Origin.X) / CellSize - StartX;
	const double Dz = (To.Z - Origin.Z) / CellSize - StartZ;
	FIntPoint Cell(FMath::FloorToInt32(StartX), FMath::FloorToInt32(StartZ));
	if (IsSolidCell(Cell))
	{
		return false;
	}
	// How much of the segment (0..1) crosses one cell along each axis, and where it crosses the next cell border.
	const double Never = TNumericLimits<double>::Max();
	const int32 StepX = Dx > 0.0 ? 1 : -1;
	const int32 StepZ = Dz > 0.0 ? 1 : -1;
	const double DeltaX = Dx != 0.0 ? 1.0 / FMath::Abs(Dx) : Never;
	const double DeltaZ = Dz != 0.0 ? 1.0 / FMath::Abs(Dz) : Never;
	double NextX = Dx > 0.0 ? (Cell.X + 1 - StartX) * DeltaX : (Dx < 0.0 ? (StartX - Cell.X) * DeltaX : Never);
	double NextZ = Dz > 0.0 ? (Cell.Y + 1 - StartZ) * DeltaZ : (Dz < 0.0 ? (StartZ - Cell.Y) * DeltaZ : Never);
	// (Never more cells than the borders the segment crosses; the guard only protects against rounding.)
	const int32 MaxSteps = FMath::CeilToInt32(FMath::Abs(Dx)) + FMath::CeilToInt32(FMath::Abs(Dz)) + 2;
	for (int32 Step = 0; Step < MaxSteps && FMath::Min(NextX, NextZ) <= 1.0; ++Step)
	{
		if (FMath::Abs(NextX - NextZ) <= LineCornerTolerance)
		{
			// Exactly through a corner: the line squeezes past it unless the blocks on both sides of it meet there.
			if (IsSolidCell(FIntPoint(Cell.X + StepX, Cell.Y)) && IsSolidCell(FIntPoint(Cell.X, Cell.Y + StepZ)))
			{
				return false;
			}
			Cell += FIntPoint(StepX, StepZ);
			NextX += DeltaX;
			NextZ += DeltaZ;
		}
		else if (NextX < NextZ)
		{
			Cell.X += StepX;
			NextX += DeltaX;
		}
		else
		{
			Cell.Y += StepZ;
			NextZ += DeltaZ;
		}
		if (IsSolidCell(Cell))
		{
			return false;
		}
	}
	return true;
}

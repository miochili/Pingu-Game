// Pingu - floating block island on a grid.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BlockIsland.generated.h"

class APenguinCharacter;
class UBoxComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

UENUM(BlueprintType)
enum class EIslandLayout : uint8
{
	/** Hand-made course to test every move: gap, steps, ice, floating platforms. */
	MovementTest,
	/** Random blocky island from Seed (simple generator). */
	Random,
	/** Long flat strip (used by the automated movement test). */
	Flat,
	/** Battle arena: hills, pits, twin islands, terraces or valleys, floating platforms. */
	Arena,
	/** The lobby (2026-10-23): a small arena-looking island, mostly flat - a step at its right end (the lobby's "+" stands
	 *  there), maybe a low step or a little platform on the left, a patch of ice; no pits. Drawn from Seed like the others. */
	Lobby,
};

UENUM(BlueprintType)
enum class EBlockKind : uint8
{
	None,
	Snow,
	Rock,
	DeepRock,
	Ice,
};

/** LAN: one block that is not as the generator made it (added, frozen, temporary, crumbling or falling sand). */
USTRUCT()
struct FIslandNetCell
{
	GENERATED_BODY()

	/** The cell, packed: X in the low 16 bits, Z in the high 16 bits. */
	UPROPERTY()
	int32 Cell = 0;

	UPROPERTY()
	EBlockKind Kind = EBlockKind::None;

	/** Host world time it melts (temporary block), falls (crumbling block) or starts falling (sand); 0 = never. */
	UPROPERTY()
	double Time = 0.0;

	/** Falling sand: cells per side - 1, or ABlockIsland::BigBlockCells for a big block falling as one (Cell is its bottom-left
	 *  cell). */
	UPROPERTY()
	uint8 Span = 1;
};

/** LAN: blocks rising into place (an ice wall's: solid from the start, their look grows up out of the wall's floor). */
USTRUCT()
struct FIslandNetRise
{
	GENERATED_BODY()

	/** The cell, packed (as FIslandNetCell::Cell). */
	UPROPERTY()
	int32 Cell = 0;

	/** The row the wall grows up from (its lowest). */
	UPROPERTY()
	int32 FloorRow = 0;

	/** Host world time the rise starts, and how long it takes (s). */
	UPROPERTY()
	double Start = 0.0;

	UPROPERTY()
	float Duration = 0.f;
};

/**
 * LAN: everything a client needs to end up with exactly the host's blocks. The blocks are the generator's
 * (deterministic from Layout + Seed) minus Removed plus Changed. The host refreshes it at the end of every frame
 * that changed the island and it replicates as one state, so a client that joins late gets the current island too.
 * The times are host world times (AGameStateBase::GetServerWorldTimeSeconds on the clients).
 */
USTRUCT()
struct FIslandNetState
{
	GENERATED_BODY()

	/** Bumped by every rebuild on the host (0 = nothing from the host yet). */
	UPROPERTY()
	int32 Generation = 0;

	/** Bumped by every refresh. */
	UPROPERTY()
	int32 Revision = 0;

	UPROPERTY()
	EIslandLayout Layout = EIslandLayout::MovementTest;

	UPROPERTY()
	int32 Seed = 0;

	UPROPERTY()
	int32 RandomWidth = 0;

	/** The penguins the arena was made for (ABlockIsland::ArenaPenguins: its size). */
	UPROPERTY()
	int32 ArenaPenguins = 0;

	UPROPERTY()
	int32 DepthRows = 0;

	UPROPERTY()
	float CellSize = 0.f;

	/** Generated blocks that are gone (packed cells) in the order they went: it only grows until the next rebuild. */
	UPROPERTY()
	TArray<int32> Removed;

	/** Blocks that differ from the generated ones: added, re-added, frozen; temporary ones with their melt time. */
	UPROPERTY()
	TArray<FIslandNetCell> Changed;

	/** Sudden death: blocks shaking before they fall, with the time they fall. */
	UPROPERTY()
	TArray<FIslandNetCell> Crumbling;

	/** Sand blocks shaking before they fall, or falling: the cell each one falls from and the time it starts falling.
	 *  A shaking one is still a block (in Changed); a falling one is not. Clients animate the fall from these alone. */
	UPROPERTY()
	TArray<FIslandNetCell> Falling;

	/** Big blocks (ABlockIsland::BigBlockCells x BigBlockCells cells drawn as one block): the packed bottom-left cell of each. */
	UPROPERTY()
	TArray<int32> BigBlocks;

	/** Blocks whose look is still rising into place (old entries the clients' clocks have passed do nothing). */
	UPROPERTY()
	TArray<FIslandNetRise> Rising;

	/** Host world time of this refresh (a client knows which sand had started falling when it was sent). */
	UPROPERTY()
	double Time = 0.0;

	UPROPERTY()
	bool bCollapsing = false;

	UPROPERTY()
	float CollapseCenterX = 0.f;

	/** Collapse radius at CollapseTime; from there it shrinks by CollapseSpeed per second. */
	UPROPERTY()
	float CollapseRadius = 0.f;

	UPROPERTY()
	double CollapseTime = 0.0;

	UPROPERTY()
	float CollapseSpeed = 0.f;

	UPROPERTY()
	float CollapseWarnTime = 0.f;

	UPROPERTY()
	int32 BrokenBlocks = 0;

	/** The host's GetCellChecksum() of this state (a client that ends up different logs a warning). */
	UPROPERTY()
	int32 Checksum = 0;
};

/**
 * How an arena is made for a number of penguins (ABlockIsland::GetArenaSize): the generator's numbers. The more penguins,
 * the wider the island, with more hills, pits, ice and platforms; a small one is lower too. Four penguins give the arena
 * as it was first tuned (RandomWidth 26), block for block.
 */
struct FPinguArenaSize
{
	/** The penguins it is made for (0: the old fixed size, RandomWidth wide). */
	int32 Penguins = 0;
	/** Width in blocks before the seed's jitter (RandRange(JitterMin, JitterMax) more), kept within MinWidth..MaxWidth. */
	int32 BaseWidth = 26;
	int32 JitterMin = -2;
	int32 JitterMax = 6;
	int32 MinWidth = 18;
	int32 MaxWidth = 60;
	/** BaseWidth against the four-penguin arena's (26): 1 for that one and the old fixed size. */
	float Scale = 1.f;
	/** The highest a hill's top gets (grid row; the plain ground is row -1). */
	int32 HillTopMax = 3;
	/** At most this many hills and dips, pits (the rolling hills), ice patches. */
	int32 BumpsMax = 3;
	int32 PitsMax = 2;
	int32 IcePatchesMax = 2;
	/** Floating platforms: mirrored pairs on a mirrored island, single ones otherwise. */
	int32 MirroredPlatformsMin = 2;
	int32 MirroredPlatformsMax = 3;
	int32 PlatformsMin = 3;
	int32 PlatformsMax = 5;
	/** A platform floats 3 to this many rows above the ground under it, a high one 3 rows more (this often). */
	int32 PlatformLiftMax = 5;
	float HighPlatformChance = 0.25f;
	/** Tries to find room for the platforms. */
	int32 PlatformTries = 120;

	/** The narrowest and the widest the island comes out (blocks). */
	int32 GetNarrowest() const { return FMath::Clamp(BaseWidth + JitterMin, MinWidth, MaxWidth); }
	int32 GetWidest() const { return FMath::Clamp(BaseWidth + JitterMax, MinWidth, MaxWidth); }
};

/**
 * Builds a floating island out of 1 m blocks on the X/Z grid (the play plane).
 * Visuals: instanced rounded blocks (a few rows deep) + snow caps on exposed tops.
 * Collision: the grid is merged into as few boxes as possible, so there are no seams to catch on.
 * Ice blocks get their own collision boxes tagged "Ice" (slippery in UPenguinMovementComponent).
 * Blocks can be added at runtime (ice walls, icebergs: temporary ones melt, permanent ones stay until the next island)
 * and blown up. Sand blocks (AddSandBlocks) fall like Minecraft sand whenever nothing is under them.
 *
 * Edits change the blocks right away (every query sees them); the instances, the collision and the LAN state are
 * rebuilt once at the end of the frame, however many edits there were. A falling sand block is not a block while it
 * falls (no collision, not in any query): it is drawn by its own instances and becomes a block again where it lands.
 *
 * Big blocks (a grown penguin's ice: AddBlocks / AddSandBlocks with big blocks asked for): BigBlockCells x BigBlockCells
 * cells drawn as one scaled-up block on the cell lattice. Underneath they are ordinary cells (collision, queries, bots); the
 * grouping only decides the look and keeps them together: as sand a big block shakes, falls and lands as one piece (it is
 * held up by any block under its bottom row). Breaking one of its cells (shots, blasts, BreakIceAt ...) breaks the whole
 * big block; one taken out whole (the hook, a tornado: TakeIceBlockAt) leaves the others behind as ordinary ice blocks, as
 * does a big block starting to crumble (sudden death: all its cells crumble together).
 *
 * LAN: the level's island replicates (a net startup actor). Edits only happen on the host (on a client the edit
 * functions do nothing); the host's blocks reach every client by themselves (FIslandNetState), and each client
 * collides its own penguin with its own copy. Runtime-spawned islands (the autotest's strip) are for solo play.
 */
UCLASS()
class PENGUIN_API ABlockIsland : public AActor
{
	GENERATED_BODY()

public:
	ABlockIsland();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Falling blocks, cm/s^2: a little more than a penguin's own falling gravity (UPenguinMovementComponent: a 290 cm jump
	 *  in 0.36 s, times FallGravityMultiplier 1.6 = ~7160), so a penguin standing on a block that drops away rides down on
	 *  it instead of sinking into it. Falling sand uses it, and so does everything else that falls like a block: a frozen
	 *  penguin's ice cube, an ice block the fish hook pulls out (AHookedIceBlock). The same on the way up and down. */
	static constexpr float FallingBlockGravity = 7500.f;
	/** Top falling speed of a falling block, cm/s (above a penguin's MaxFallSpeed 2200: a block never falls back into its
	 *  rider). */
	static constexpr float FallingBlockMaxSpeed = 3000.f;

	/** A big block is this many cells per side (PinguTuning::IceBigBlockCells: a 4x grown penguin's ice). */
	static constexpr int32 BigBlockCells = 2;

	/** The arena (the island near the world origin), or the first island. */
	static ABlockIsland* Get(const UWorld* World);

	/** The island closest to a location (abilities act on the island they happen over). */
	static ABlockIsland* FindNear(const UWorld* World, const FVector& Location);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island")
	EIslandLayout Layout = EIslandLayout::MovementTest;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island")
	int32 Seed = 1337;

	/** Approximate width (blocks) of a random island. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island", meta = (ClampMin = "8", ClampMax = "80"))
	int32 RandomWidth = 26;

	/** Arena: the penguins it is made for, 2 to 10 - the more, the bigger (GetArenaSize). 0: the old fixed size, RandomWidth
	 *  wide (the tests' arenas). The game sets it for every round and for the lobby. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island", meta = (ClampMin = "0", ClampMax = "10"))
	int32 ArenaPenguins = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island", meta = (ClampMin = "25"))
	float CellSize = 100.f;

	/** Visual rows of blocks along the depth axis (the collision always covers the full depth). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island", meta = (ClampMin = "1", ClampMax = "9"))
	int32 DepthRows = 3;

	/** Ocean far below and icebergs in the background. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island")
	bool bBuildBackdrop = true;

	/** Height of the sea surface. Falling into it knocks you out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island", meta = (ForceUnits = "cm"))
	float SeaLevel = -1650.f;

	/** Temporary blocks shrink away during their last seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island", meta = (ForceUnits = "s"))
	float MeltTime = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	TSoftObjectPtr<UStaticMesh> BlockMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	TSoftObjectPtr<UStaticMesh> SnowCapMesh;

	/** Needs a vector parameter "Color" (M_Block does; the engine BasicShapeMaterial works too). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	TSoftObjectPtr<UMaterialInterface> BlockMaterial;

	/** Animated sea (falls back to a flat BlockMaterial colour). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	TSoftObjectPtr<UMaterialInterface> OceanMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	FLinearColor SnowColor = FLinearColor(0.80f, 0.88f, 0.96f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	FLinearColor RockColor = FLinearColor(0.055f, 0.075f, 0.13f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	FLinearColor DeepRockColor = FLinearColor(0.025f, 0.032f, 0.06f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	FLinearColor IceColor = FLinearColor(0.35f, 0.75f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	FLinearColor CapColor = FLinearColor(0.93f, 0.97f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Island|Look")
	FLinearColor OceanColor = FLinearColor(0.012f, 0.05f, 0.11f);

	/** Rebuild from the current settings (visuals, and collision when playing). On a LAN client: the host's island. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Island")
	void Rebuild();

	/** New arena from a seed (a new round), as big as ArenaPenguins says. */
	void RegenerateArena(int32 NewSeed);
	/** Tests only: the chance that an arena's end steps down to the sea (negative: the game's own, 0.6; 0: the old arenas). */
	static inline float EndStepChanceOverride = -1.f;
	/** The lobby's island from a seed (EIslandLayout::Lobby). Solo / LAN host; the clients get it with the island's state. */
	void RegenerateLobby(int32 NewSeed);
	/** The lobby island: the top of its right-end step (world, the middle of the step's top). False on other layouts. */
	bool GetLobbyStepTop(FVector& OutTop) const;

	/** The arena's numbers for Penguins (2 - 10; 0: the old fixed size, LegacyWidth wide). */
	static FPinguArenaSize GetArenaSize(int32 Penguins, int32 LegacyWidth = 26);
	/** The penguins the current island was made for (0: the old fixed size or another layout; a LAN client: the host's). */
	int32 GetArenaPenguins() const { return BaseLayout == EIslandLayout::Arena ? BaseArenaPenguins : 0; }
	/** The current island's size against the four-penguin arena (GetArenaSize's Scale): under 1 for a small one, 1 for the
	 *  old fixed size and the other layouts. The stage camera keeps less room around a small island. */
	float GetArenaScale() const;
	/** LAN client: its blocks are the host's (the checksum the host sent matches). Always true in solo and on the host. */
	bool MatchesHostIsland() const;

	/** World-space spawn points on top of the island (left to right order is spread out). */
	UFUNCTION(BlueprintCallable, Category = "Island")
	TArray<FVector> GetSpawnLocations();

	/**
	 * Count places to start at, as far from each other as the island allows: evenly spread from one end of its ground to
	 * the other (a couple of blocks in from the edges), left to right, each on the ground of its column with room to
	 * stand. Fewer when the island has fewer such columns. World space, on the play plane.
	 * bFlatOnly (the round's starts): only columns with the same ground on both sides, so nobody starts against a step or a wall
	 * (when the island has enough of them, else all columns).
	 */
	TArray<FVector> GetSpreadSpawnLocations(int32 Count, bool bFlatOnly = false);

	UFUNCTION(BlueprintPure, Category = "Island")
	EBlockKind GetBlock(int32 X, int32 Z) const;

	// ------------------------------------------------------------------ queries (bots, abilities)
	FIntPoint WorldToCell(const FVector& World) const;
	/** Centre of a cell on the play plane. */
	FVector CellToWorld(const FIntPoint& Cell) const;
	bool IsSolidCell(const FIntPoint& Cell) const { return GetBlock(Cell.X, Cell.Y) != EBlockKind::None; }
	bool IsSolidAt(const FVector& World) const { return IsSolidCell(WorldToCell(World)); }
	EBlockKind GetBlockAt(const FVector& World) const { const FIntPoint Cell = WorldToCell(World); return GetBlock(Cell.X, Cell.Y); }
	/** Frost breath / ice gun: exposed snow blocks within Radius of Center turn into slippery ice. Returns how many.
	 *  (bRebuild is kept for older callers: the rebuild always happens once at the end of the frame.) */
	int32 FreezeBlocksInRadius(const FVector& Center, float Radius, bool bRebuild = true);
	/** Edits are rebuilt once at the end of the frame anyway; kept for the callers that batch with bRebuild = false. */
	void RebuildAfterEdits();
	/** Rebuilds the instances and the collision right now (only for code that traces against new blocks this frame). */
	void FlushEdits();
	/** Top of the first solid block at or below From, searching MaxDrop down. */
	bool FindGroundBelow(const FVector& From, float MaxDrop, float& OutGroundZ) const;
	/** The highest ground under a capsule at Location (under its middle and both sides), searched from its feet down
	 *  MaxDrop: what it would be pressed against when pushed down (falling ice crushes it there). False if none. */
	bool FindGroundUnderCapsule(const FVector& Location, float Radius, float HalfHeight, float MaxDrop, float& OutGroundZ) const;
	/** A capsule at Location fits without touching a block, and no sand falls (or shakes) above it. */
	bool CanFitCapsule(const FVector& Location, float Radius, float HalfHeight) const { return HasRoomForCapsule(Location, Radius, HalfHeight); }
	/** World box around every block (on the play plane). */
	FBox GetBounds() const;
	/** The island as generated (no blocks added or gone since; world, play plane) and the lowest / highest of its 12 spread
	 *  starts' Z (as the stage camera frames it): the build limit's fixed frame (PinguBuildLimit). False before generation. */
	bool GetBaseFrame(FBox& OutBounds, float& OutGroundMinZ, float& OutGroundMaxZ) const;
	/** A block that melts away (a temporary ice wall or iceberg block); permanent ones are not temporary. */
	bool IsTemporaryCell(const FIntPoint& Cell) const { return TempCells.Contains(Cell); }
	/** Cell is part of a big block (OutOrigin: the big block's bottom-left cell). Every machine. */
	bool IsBigBlockCell(const FIntPoint& Cell, FIntPoint* OutOrigin = nullptr) const;
	/** Big blocks resting on the island (shaking ones too; falling ones are not blocks). Every machine. */
	int32 GetNumBigBlocks() const { return BigBlocks.Num(); }
	/** Their bottom-left cells. */
	void GetBigBlocks(TArray<FIntPoint>& OutOrigins) const { OutOrigins = BigBlocks.Array(); }
	/** Shaking or falling sand that is a big block falling as one. Every machine. */
	int32 GetNumFallingBigBlocks() const;
	/** Cell is a block of sand (it falls when nothing holds it up: iceberg ice, ...). Solo / LAN host (tests). */
	bool IsSandCell(const FIntPoint& Cell) const { return SandCells.Contains(Cell); }
	/** How far the look of Cell has risen into place (RiseBlocks): 0 at the start, 1 once there (or not rising). Every machine. */
	float GetRiseAlpha(const FIntPoint& Cell) const;
	/** Some block's look is rising right now. Every machine. */
	bool IsAnyBlockRising() const { return RisingCells.Num() > 0; }

	// ------------------------------------------------------------------ runtime changes (solo / LAN host only)
	/** Adds blocks into empty cells. Lifetime <= 0 keeps them until the next island (or ClearTemporaryBlocks).
	 *  BigOrigins: bottom-left cells of big blocks to make of them (each one whose cells were all added). Returns the cells
	 *  added. */
	TArray<FIntPoint> AddBlocks(const TArray<FIntPoint>& InCells, EBlockKind Kind, float Lifetime, const TArray<FIntPoint>* BigOrigins = nullptr);
	/** Blocks just added (AddBlocks) rise into place: solid from now on, their look grows up out of the lowest row of these
	 *  cells over Duration (a wall out of its floor; a ledge, all in one row, from its bottom). Every machine shows it. */
	void RiseBlocks(const TArray<FIntPoint>& InCells, float Duration);
	/**
	 * Host: penguins inside these cells (blocks that just appeared: an ice wall) are moved out of them - never knocked out,
	 * never left inside. One whose middle is in a column of them goes up onto them (with head room; it rises with them at
	 * LiftSpeed at least); one only reaching into them from the side is set aside to that side (or up, if that is shorter).
	 * Up past other blocks (a ceiling right over them) it is not: then out to an open side instead (onto ground when it stood
	 * on ground), and only as a last resort up past whatever is in the way. NetTeleport: a LAN player's own penguin is moved on
	 * its own machine. Returns how many were moved.
	 */
	int32 PushPenguinsOutOfBlocks(const TArray<FIntPoint>& InCells, float LiftSpeed = 250.f);
	/** Host: shots and icicle pegs inside these cells hit them: they break (bombs go off), as under falling ice. Returns
	 *  how many. */
	int32 SmashProjectilesInCells(const TArray<FIntPoint>& InCells);
	/** Removes every block added at runtime right away: ice walls, icebergs and sand, temporary or permanent. */
	void ClearTemporaryBlocks();

	/** Blows up every block whose centre is inside the radius. Returns how many went. */
	int32 RemoveBlocksInRadius(const FVector& Center, float Radius, TArray<FVector>* OutRemovedCenters = nullptr);

	/** Breaks the one block at World (debris, no sound). Returns false if there is none.
	 *  (bRebuild is kept for older callers: the rebuild always happens once at the end of the frame.) */
	bool BreakBlockAt(const FVector& World, bool bRebuild = true);

	// ------------------------------------------------------------------ ice (ice walls, iceberg blocks, frozen snow) - solo / LAN host
	/** Breaks the ice block at World (shards flying along PushDirection - up when zero - and a shatter sound); any other
	 *  kind stays. Temporary or permanent, it counts in GetBrokenBlocks and the sand on it falls. Returns false if there
	 *  is no ice there. Every ability that hits ice breaks it this way, one block at a time (a big block is one block: all of
	 *  its cells break). */
	bool BreakIceAt(const FVector& World, const FVector& PushDirection = FVector::ZeroVector);
	/** Breaks up to MaxBlocks ice blocks whose centres are within Radius of Center (on the play plane), nearest first (a big
	 *  block - any of its cells within reach - is one of them, all its cells break); the shards fly along PushDirection, or
	 *  outward from Center (and up) when it is zero. One shatter sound for all. Returns how many cells broke. */
	int32 BreakIceInRadius(const FVector& Center, float Radius, int32 MaxBlocks = 1, const FVector& PushDirection = FVector::ZeroVector);
	/** Iceberg blocks: permanent blocks that behave like sand - each one drops down its column until it rests on a block
	 *  (they stack), falls again whenever the block under it goes, and past the bottom of the island drops into the sea.
	 *  The cells may be in the air; occupied ones are skipped. Returns the cells taken (the blocks fall from there).
	 *  Cells with a block under them are blocks at once; the others fall right away (a sand block whose block goes later
	 *  shakes for a moment first). Falling sand coming down onto a penguin (its middle in the column) pushes it down and
	 *  crushes it once it is pressed against what is below (knock-out reason Crushed); penguins beside it are shoved out
	 *  of its way, one standing on it rides along. CausedBy (who made these blocks, if anybody) gets the knock-out and is
	 *  never crushed by them itself. bIcebergIce: the ice of an iceberg that stopped on its other column (AIcebergDrop):
	 *  what it crushes on its way down is knocked out by the iceberg (reason Iceberg) - only once it really comes down on
	 *  it, like any falling sand. BigOrigins: bottom-left cells of big blocks among the cells (all of their cells free): each
	 *  is one piece of sand - held up by any block under its bottom row, else it falls as one. */
	TArray<FIntPoint> AddSandBlocks(const TArray<FIntPoint>& InCells, EBlockKind Kind, APenguinCharacter* CausedBy = nullptr, bool bIcebergIce = false,
		const TArray<FIntPoint>* BigOrigins = nullptr);
	/** The fish hook pulls an ice block out of the island, or the gust blows one out (an AHookedIceBlock carries it on and
	 *  puts it back where it lands, as sand ice): the ice block at World goes without breaking - no shards, not counted as
	 *  broken - and the sand on it falls. OutCell: the cell it was in. False if there is no ice there. A cell of a big block:
	 *  that one cell goes, the big block falls apart into its other cells (ordinary ice blocks, which can be taken too). */
	bool TakeIceBlockAt(const FVector& World, FIntPoint& OutCell);
	/** The mesh and material ability-made ice is drawn with (sand ice too), for an ice block flying on its own. Every
	 *  machine (nullptrs before the island has its look). */
	void GetAbilityIceLook(UStaticMesh*& OutMesh, UMaterialInterface*& OutMaterial) const;
	/** Some sand block is shaking or falling right now (bots keep clear of falling ice). Every machine. */
	bool IsSandFalling() const;
	/** Some sand block is shaking or falling over the world X range [MinX, MaxX] with its top above AboveZ (a bot steps
	 *  out from under it); OutBlockX gets the world X of that block's middle. Every machine. */
	bool IsSandFallingOver(float MinX, float MaxX, float AboveZ, float* OutBlockX = nullptr) const;
	/** The bottom (world Z) of the lowest sand block shaking or falling over the world X range [MinX, MaxX]; false if
	 *  there is none. Every machine (the autotest measures where falling ice is). */
	bool GetFallingSandBottom(float MinX, float MaxX, float& OutBottomZ) const;
	/**
	 * Host: a block of ice falling on its own (a tornado threw it: AHookedIceBlock) coming down onto Penguin - Box is the
	 * block on the play plane, falling FallSpeed (cm/s). As under falling sand: a penguin whose middle is in the block's column
	 * and under the block's middle is pushed down with it (kept just under its bottom) and crushed once that presses it against
	 * what is below (knock-out reason Crushed, CausedBy's) - never CausedBy itself, a shelled penguin or the round winner.
	 * True if it was under the block that way (pushed down or crushed); bOutCrushed: it was knocked out.
	 */
	bool PressUnderFallingIce(APenguinCharacter* Penguin, const FBox& Box, float FallSpeed, APenguinCharacter* CausedBy, bool& bOutCrushed);

	/** No solid block between From and To on the play plane (walls, ice, any kind - falling sand is not one yet; the cells
	 *  of both end points count too): area abilities (gust, noot, frost, slap) do not reach through walls. Every machine. */
	bool IsLineClear(const FVector& From, const FVector& To) const;

	/** Blocks of the island (falling sand is not one until it lands). */
	int32 GetNumBlocks() const { return Cells.Num(); }
	/** How many blocks were destroyed since the island was (re)generated (craters, crumbling, broken ice, sand that
	 *  fell off the island into the sea). */
	int32 GetBrokenBlocks() const { return BrokenBlocks; }

	// ------------------------------------------------------------------ sudden death
	/** The island crumbles from its outer edges toward its middle: a column every StepInterval seconds,
	 *  each one shaking and glowing for WarnTime before it falls into the sea. */
	void StartCollapse(float StepInterval = 0.8f, float WarnTime = 1.2f);
	/** Stops crumbling (blocks that are shaking stay). */
	void StopCollapse();
	bool IsCollapsing() const { return bCollapsing; }
	/** Blocks further than this (X distance from GetCollapseCenterX) are crumbling or gone. */
	float GetCollapseRadius() const { return CollapseRadius; }
	float GetCollapseCenterX() const { return CollapseCenterX; }

	// ------------------------------------------------------------------ round hazards (APinguRoundHazards)
	/** Rising Sea: the sea Rise cm above where the island made it (SeaLevel follows, so every sea check does; the sea's look,
	 *  the floes and their foam move with it). 0 puts it back. Every machine sets its own (from the replicated hazard). */
	void SetSeaRise(float Rise);
	float GetSeaRise() const { return SeaRise; }
	/** Crumbling Island: every block whose middle is left of MinX or right of MaxX (world X) shakes for WarnTime, then falls
	 *  into the sea (sudden death's crumbling). Solo / LAN host. Returns how many started shaking. */
	int32 CrumbleOutside(float MinX, float MaxX, float WarnTime);
	/** Some block whose middle is over the world X range [MinX, MaxX] is shaking before it falls. Every machine. */
	bool IsCrumblingOver(float MinX, float MaxX) const;

	// ------------------------------------------------------------------ LAN
	/** False on a LAN client until the host's island has arrived (true in solo and on the host). */
	bool IsNetSynced() const;
	/** Order-independent hash of every block and its kind: the host and its clients have the same one. */
	uint32 GetCellChecksum() const;
	/** Tests: what every drawn block instance looks like - per (cell X, cell Z, component * 100 + depth row) a hash of its component, the random number
	 *  its material reads (PerInstanceRandom: the texture tile and tint) and where it sits (not the scale: ice melts). Returns false when
	 *  a component's instances are not the cells' ones yet (the visuals are not built). The numbers must follow the cell, never the
	 *  instance's place in the list (a crumbled column used to reshuffle them all). */
	bool GetLookSignatures(TMap<FIntVector, uint32>& Out) const;
	/** How often the island was (re)generated; on a LAN client the host's count. */
	int32 GetGeneration() const { return NetState.Generation; }

private:
	/** Temporary or crumbling block. */
	struct FTimedCell
	{
		/** Seconds left. */
		float Left = 0.f;
		/** Host world time it runs out (LAN clients animate toward it). */
		double At = 0.0;
	};

	/** A sand block shaking before it falls (still a block in Cells) or falling (not a block until it lands). The host's
	 *  are the real ones; a LAN client draws its copies from NetState.Falling (the fall is the same function of time). */
	struct FSandBlock
	{
		/** The cell it falls from (X = column, Y = row); a big block's bottom-left cell. */
		FIntPoint Start = FIntPoint(0, 0);
		/** Cells per side: 1, or BigBlockCells for a big block falling as one. */
		int32 Span = 1;
		/** Host world time it starts falling (from rest); until then it shakes in Start. */
		double StartTime = 0.0;
		EBlockKind Kind = EBlockKind::None;
		/** Bottom of the block in island space (cm) as of the last update. */
		float Bottom = 0.f;
		/** Host: it left Cells and falls. */
		bool bFalling = false;
		/** Host: it was a block of the island before it fell (lost in the sea it counts as broken). */
		bool bFromIsland = false;
		/** Host: landed, in the sea or cancelled - removed after this update. */
		bool bDone = false;
		/** Host: who made it fall, if known (AddSandBlocks): it gets the knock-out of a penguin crushed under it, and is
		 *  never crushed by it itself. */
		TWeakObjectPtr<APenguinCharacter> CausedBy;
		/** Host: the ice of an iceberg falling on down a lower column (AddSandBlocks): a penguin it crushes is knocked out
		 *  by the iceberg (reason Iceberg). Only this first fall: once it rests, it is sand like any other. */
		bool bIcebergIce = false;
	};

	/** A piece of sand: one cell (Span 1) or a big block (its bottom-left cell, Span BigBlockCells). */
	struct FSandPiece
	{
		FIntPoint Origin = FIntPoint(0, 0);
		int32 Span = 1;
	};

	/** A block whose look rises into place (RiseBlocks). */
	struct FRiseCell
	{
		/** The row it grows up from (the lowest of its wall). */
		int32 FloorRow = 0;
		/** Host world time the rise starts and how long it takes. */
		double Start = 0.0;
		float Duration = 0.f;
	};

	void GenerateCells();
	void GenerateMovementTest();
	void GenerateRandom();
	void GenerateFlat();
	void GenerateArena();
	void GenerateLobby();
	void FillColumn(int32 X, int32 TopZ, int32 Depth);
	void SetBlock(int32 X, int32 Z, EBlockKind Kind);
	void ComputeSpawnPoints(const TArray<int32>& PreferredColumns);
	void SetupVisuals();
	void BuildVisuals();
	void BuildCollision();
	void BuildBackdrop();
	void UpdateMeltVisuals();
	/** Every frame while blocks rise into place (RisingCells): their instances grow up out of their wall's floor; finished
	 *  rises are dropped. */
	void UpdateRiseVisuals();
	/** Sudden death step: warns and drops blocks (marks what changed). */
	void UpdateCollapse(float DeltaSeconds);
	void UpdateCrumbleVisuals();
	/** The instances of a block (Span 1) or a big block (Span BigBlockCells) along the depth axis, how deep each one is (in
	 *  cells), and the middle (island space, on the depth axis) of instance Index. */
	int32 GetDepthInstances(int32 Span) const;
	float GetDepthInstanceCells(int32 Span) const;
	float GetDepthInstanceY(int32 Span, int32 Index) const;
	/** The transform of a block's instance Index along the depth axis: Cell its bottom-left cell, Bottom its bottom (island space,
	 *  cm), Height how tall it is drawn (cm; a full block: Span x CellSize). */
	FTransform MakeBlockInstance(const FIntPoint& Cell, int32 Span, int32 Index, float Bottom, float Height) const;
	/** A material instance for one kind of block: Parent (an optional per-kind art material) or the block material, with
	 *  the parameters Color, Roughness, Emissive and Kind (see EBlockLook in the .cpp: 0 snow, 1 rock, 2 deep rock,
	 *  3 ice, 4 temporary ice, 5 crumbling, 6 snow cap, 7 backdrop, 8 ocean). */
	UMaterialInstanceDynamic* MakeColorMaterial(const FLinearColor& Color, float Roughness, float Emissive, int32 Kind = 0, UMaterialInterface* Parent = nullptr);
	FVector CellCenter(int32 X, int32 Z, float Y = 0.f) const;
	void ResolveAssets();

	/** Solo or the LAN host: the island may change. A LAN client only shows the host's island. */
	bool CanEdit() const;
	bool IsNetClient() const;
	bool IsNetHost() const;
	/** Host world time (the same clock on the LAN clients). */
	double GetNetTime() const;
	/** The blocks changed: instances, collision, bounds and the LAN state need a rebuild. */
	void MarkCellsChanged();
	/** Only which blocks melt or crumble changed: instances and the LAN state. */
	void MarkVisualsChanged();
	/** Removes one block with everything attached to it. Returns false if there was none. The sand on it falls (the
	 *  support check runs once per frame, see UpdateSand). */
	bool RemoveCell(const FIntPoint& Cell);

	// ---- big blocks
	/** Makes the BigBlockCells x BigBlockCells cells from Origin (bottom-left) one big block: all of them blocks of the same
	 *  kind, all sand or none, none in another big block, crumbling or shaking. False (nothing changes) otherwise. */
	bool MakeBigBlock(const FIntPoint& Origin);
	/** The big block at Origin is one no more: its cells stay as ordinary blocks (shaking as one, they rest again and the sand
	 *  check looks at them anew). */
	void DissolveBigBlock(const FIntPoint& Origin);
	/** The piece of sand (or block) Cell belongs to: its big block, or the cell itself. */
	FSandPiece GetPieceOf(const FIntPoint& Cell) const;
	/** A block right under one of the piece's bottom cells. */
	bool IsPieceSupported(const FSandPiece& Piece) const;
	/** The pieces of sand resting on InOutPieces (and on those, ...) with nothing else under them go with them: appended.
	 *  bSkipShaking: sand already shaking (on its way at its own time) stays out. */
	void CollectSandStack(TArray<FSandPiece>& InOutPieces, bool bSkipShaking) const;

	// ---- ice and sand
	/** Removes the ice block in Cell (counted as broken) with shards flying along Direction - all of a big block's cells.
	 *  No sound. Returns how many cells broke. */
	int32 BreakIceCell(const FIntPoint& Cell, const FVector& Direction);
	/** BreakIceCell for one cell of the island. */
	bool BreakOneIceCell(const FIntPoint& Cell, const FVector& Direction);
	/** The shatter sound of one or more ice blocks breaking now (at most one per short moment). */
	void PlayShatterSound(int32 NumBlocks);
	/** Host, once per frame: resting sand with nothing under it starts shaking, shaking sand falls, falling sand moves. */
	void UpdateSand(double Now);
	/** Host: sand stacks with nothing under them shake for SandWarnTime (see the .cpp), then fall. */
	void StartSandWarnings(double Now);
	/** Host: shaking sand whose time is up leaves the island and falls, with the sand stacked on it. */
	void StartSandFalls(double Now);
	/** Host: the sand piece stops being blocks and falls from there, starting at StartTime. */
	void BeginSandFall(const FSandPiece& Piece, double StartTime);
	/** Moves the falling sand to Now. Host (bAuthority): lands it, drops it into the sea, shoves penguins and breaks
	 *  projectiles in its way. LAN client: only where to draw it (resting on what the client has until the host says). */
	void MoveFallingSand(double Now, bool bAuthority);
	/** Host: the sand piece is blocks again (it landed) with its bottom-left cell in Cell (a big block one again). */
	void LandSandBlock(const FSandBlock& Block, const FIntPoint& Cell);
	/** Solid for falling sand: a block, except one that is shaking sand whose fall already started (a LAN client still
	 *  has it until the host's state arrives). */
	bool IsSandSupport(const FIntPoint& Cell, double Now) const;
	/** Host: penguins overlapping a sand block (World box on the play plane) are moved out of it: aside onto ground when
	 *  there is room, else (bLanded) up onto it. BlockBottomZ: a penguin standing on its upper half rides a falling block
	 *  (left alone) or is lifted onto a landed one. bFell: the block came down there (falling, or landed this frame) at
	 *  FallSpeed (cm/s) - a penguin under it (its middle in the column) is pushed down with it and crushed once pressed
	 *  against what is below (never CausedBy itself, a shell or the round winner: those are moved out of it as before;
	 *  CausedBy's teammates are crushed too - friendly fire). A block that just appeared (bFell false) never knocks anyone out.
	 *  bIcebergIce: the block is an iceberg's ice falling on down (the knock-out is the iceberg's, reason Iceberg).
	 *  BlockHeight: how tall the block is (cm; 0: a cell - a big block is BigBlockCells cells). */
	void ShovePenguinsOutOfSand(const FBox& World, float BlockBottomZ, bool bLanded, bool bFell = false, float FallSpeed = 0.f, APenguinCharacter* CausedBy = nullptr,
		bool bIcebergIce = false, float BlockHeight = 0.f);
	/** Host: projectiles and icicle pegs inside any of the boxes break (bombs go off), as under a falling iceberg. */
	void SmashProjectilesInSand(const TArray<FBox>& Boxes);
	/** A capsule at Location fits without touching a block, and no sand falls (or shakes) above it. */
	bool HasRoomForCapsule(const FVector& Location, float Radius, float HalfHeight) const;
	/** Rebuilds the instances of the falling-sand components (sand blocks came or went). */
	void RebuildSandInstances();
	/** Every frame: shaking and falling sand where it is now (no render state rebuild). */
	void UpdateSandVisuals(double Now);
	/** Host: copies the current island into NetState (once per frame with changes). */
	void RefreshNetState();
	/** LAN client: makes the island exactly what NetState says. */
	void ApplyNetState();
	void ResetNetState();

	UFUNCTION()
	void OnRep_NetState();

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> SnowBlocks;

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> RockBlocks;

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> DeepRockBlocks;

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> IceBlocks;

	/** Ice made by abilities: ice walls, icebergs, sand (brighter than the island's own ice; temporary ones melt, so they
	 *  get their own instances). */
	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> TempBlocks;

	/** Big blocks (always ability ice): one scaled-up block per big block, with TempBlocks' mesh and material. */
	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> BigIceBlocks;

	/** Blocks about to crumble away (sudden death): glowing and shaking. */
	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> CrumbleBlocks;

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> SnowCaps;

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> Backdrop;

	UPROPERTY(VisibleAnywhere, Category = "Island")
	TObjectPtr<UInstancedStaticMeshComponent> Ocean;

	/** The background beyond the old tabular icebergs (BuildBackdrop; each only when its art exists in /Game/Environment):
	 *  peaked and domed icebergs, small floes bobbing on the swell, the foam around bergs and floes, the mountain ranges
	 *  on the horizon, and the ring of clouds above them. */
	UPROPERTY(VisibleAnywhere, Category = "Island|Backdrop")
	TObjectPtr<UInstancedStaticMeshComponent> BackdropPeaks;

	UPROPERTY(VisibleAnywhere, Category = "Island|Backdrop")
	TObjectPtr<UInstancedStaticMeshComponent> BackdropDomes;

	UPROPERTY(VisibleAnywhere, Category = "Island|Backdrop")
	TObjectPtr<UInstancedStaticMeshComponent> Floes;

	UPROPERTY(VisibleAnywhere, Category = "Island|Backdrop")
	TObjectPtr<UInstancedStaticMeshComponent> IceFoam;

	UPROPERTY(VisibleAnywhere, Category = "Island|Backdrop")
	TObjectPtr<UInstancedStaticMeshComponent> Mountains;

	UPROPERTY(VisibleAnywhere, Category = "Island|Backdrop")
	TObjectPtr<UStaticMeshComponent> CloudRing;

	/** The sky band behind the clouds (the cloud ring's mesh, larger, with M_SkyRing): the few degrees of sky the
	 *  camera shows, a clear blue instead of the grey-white haze the fog makes of the sky there. */
	UPROPERTY(VisibleAnywhere, Category = "Island|Backdrop")
	TObjectPtr<UStaticMeshComponent> SkyRing;

	/** Sand shaking before it falls and falling sand, one component per look (snow, rock, deep rock, ability ice); their
	 *  instances move every frame without touching the island's own instances or its collision. */
	UPROPERTY(VisibleAnywhere, Category = "Island")
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> FallingBlocks;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UBoxComponent>> CollisionBoxes;
	/** The rectangle of every CollisionBoxes entry (boxes that stay the same are kept across rebuilds). */
	TArray<uint64> CollisionKeys;
	float CollisionBuiltCellSize = 0.f;
	float CollisionBuiltDepth = 0.f;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
	/** Meshes and materials need to be (re)assigned; runtime block edits only rebuild the instances. */
	bool bVisualSetupDirty = true;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedBlockMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedSnowCapMesh;

	/** Optional art: the backdrop iceberg and sea meshes (/Game/Environment/SM_Iceberg, SM_Ocean); else the block mesh. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedBackdropMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedOceanMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> UsedMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BackdropMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> OceanSurfaceMaterial;

	/** The new background's meshes (null where the art is missing) and its materials (M_Iceberg for the icebergs, the
	 *  same with Bob 1 for the floes, M_IceFoam, M_Mountain, M_CloudRing). */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedPeakMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedDomeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedFloeMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedFoamMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedMountainMesh;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> UsedCloudMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> IcebergMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FloeMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FoamMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> MountainMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> CloudMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SkyMaterial;

	/** Host -> LAN clients. */
	UPROPERTY(ReplicatedUsing = OnRep_NetState, Transient)
	FIslandNetState NetState;

	TMap<FIntPoint, EBlockKind> Cells;
	/** What the generator made (the LAN state is the difference to it). */
	TMap<FIntPoint, EBlockKind> BaseCells;
	EIslandLayout BaseLayout = EIslandLayout::MovementTest;
	int32 BaseSeed = 0;
	int32 BaseRandomWidth = 0;
	int32 BaseArenaPenguins = 0;
	bool bHasBase = false;
	/** GetBaseFrame: made with BaseCells (local bounds; the starts' Z range, world). */
	FBox BaseLocalBounds = FBox(ForceInit);
	float BaseGroundMinZ = 0.f;
	float BaseGroundMaxZ = 0.f;
	/** Generated blocks listed in NetState.Removed (host: already sent; LAN client: the host's list). */
	TSet<FIntPoint> NetRemovedSet;
	/** Temporary ice walls and icebergs: they melt. */
	TMap<FIntPoint, FTimedCell> TempCells;
	/** Cell of every TempBlocks instance (DepthRows instances per cell). */
	TArray<FIntPoint> TempInstanceCells;
	/** Big blocks: their bottom-left cells (resting or shaking; a falling one is a SandBlocks entry), and every cell of one
	 *  with the bottom-left cell of its big block. */
	TSet<FIntPoint> BigBlocks;
	TMap<FIntPoint, FIntPoint> BigBlockOfCell;
	/** The big block of every BigIceBlocks instance (GetDepthInstances(BigBlockCells) instances per big block). */
	TArray<FIntPoint> BigInstanceOrigins;
	/** Blocks whose look is rising into place (RiseBlocks; a LAN client: from NetState.Rising). */
	TMap<FIntPoint, FRiseCell> RisingCells;
	/** Blocks added at runtime (AddBlocks, sand): their ice has the ability look, ClearTemporaryBlocks wipes them. On a
	 *  LAN client: the changed blocks that are not generated ones or are back in an emptied generated cell. */
	TSet<FIntPoint> AddedCells;
	/** Host: sand blocks of the island (resting or shaking); falling ones are in SandBlocks only. */
	TSet<FIntPoint> SandCells;
	/** Shaking and falling sand (host: the real ones; LAN client: from NetState.Falling). */
	TArray<FSandBlock> SandBlocks;
	/** Cells of shaking sand (drawn by FallingBlocks, not by the island's instances) and the time they start falling. On a
	 *  LAN client: the ones the host had not dropped yet when it sent its state. */
	TMap<FIntPoint, double> SandHiddenCells;
	/** Per FallingBlocks component: the SandBlocks entry each block of instances draws (DepthRows instances each). */
	TArray<TArray<int32>> SandInstanceEntries;
	/** The falling-sand instances need rebuilding (sand blocks came or went). */
	bool bSandInstancesDirty = false;
	/** Host: a block changed, so resting sand may have lost what it stands on. */
	bool bSandCheckDirty = false;
	/** World time the last shatter sound played (several blocks breaking at once make one sound). */
	double LastShatterSoundTime = -1.0;
	/** Sudden death: cells that are about to fall. */
	TMap<FIntPoint, FTimedCell> Crumbling;
	TArray<FIntPoint> CrumbleInstanceCells;
	int32 BrokenBlocks = 0;
	bool bCollapsing = false;
	float CollapseRadius = 0.f;
	float CollapseCenterX = 0.f;
	float CollapseSpeed = 0.f;
	float CollapseWarnTime = 1.2f;
	float CrumbleTime = 0.f;
	/** Rising Sea (SetSeaRise): how far the sea is up now (cm), and the SeaLevel it rose from. */
	float SeaRise = 0.f;
	float SeaLevelBeforeRise = 0.f;
	TArray<FVector> SpawnPointsLocal;
	bool bCellsValid = false;

	bool bVisualsDirty = false;
	/** Rebuild every instance component (new meshes, cell size or depth), not only the ones whose cells changed. */
	bool bVisualsForceAll = false;
	bool bCollisionDirty = false;
	bool bNetDirty = false;
	/** LAN client: NetState arrived and is not applied yet. */
	bool bNetStatePending = false;
	/** LAN client: the host's island has been applied at least once. */
	bool bNetSynced = false;
	bool bNetNeverSyncedWarned = false;
	/** LAN client: the blocks differ from the host's checksum, and since how long (warns once per generation). */
	bool bNetMismatch = false;
	float NetMismatchTime = 0.f;
	int32 NetMismatchWarnedGeneration = -1;
	int32 NetLoggedGeneration = -1;

	/** Bounds of the blocks in cells (GetBounds is called every frame by the camera and every penguin). */
	mutable FBox CachedLocalBounds = FBox(ForceInit);
	mutable bool bBoundsValid = false;

	/** Backdrop: rebuilt only for a new seed. */
	int32 BackdropSeed = 0;
	bool bBackdropBuilt = false;

	/** Per instance component: the cells it shows (unchanged ones are not rebuilt). */
	TArray<TArray<int32>> VisualCellKeys;
	/** Scratch (kept between frames, so editing allocates next to nothing). */
	TArray<TArray<int32>> VisualListScratch;
	TArray<FIntPoint> CellScratch;
	TArray<FTransform> TransformScratch;
	TArray<uint8> GridScratch;
	TArray<uint64> CollisionKeyScratch;
	TMap<uint64, int32> CollisionIndexScratch;
	TMap<FIntPoint, EBlockKind> CellMapScratch;
	TArray<int32> SandOrderScratch;
	TArray<float> SandFreeScratch;
	TArray<FBox> SandBoxScratch;
	TMap<int32, float> SandBelowTopScratch;
};

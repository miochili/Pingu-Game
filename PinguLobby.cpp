// Pingu - the lobby island: puffs, pokes, hats, the bots' strolls, 3D tags, the "+" and the lobby's clicks (PinguLobby.h).

#include "PinguLobby.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinBot.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PenguinPlayerState.h"
#include "PinguFX.h"
#include "PinguMenu3D.h"
#include "Text3DComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Net/UnrealNetwork.h"

namespace PinguLobbyPrivate
{
	/** The pop in (out of the dust) and the shrink away (into it). */
	constexpr float PopInTime = 0.42f;
	constexpr float PopOutTime = 0.3f;
	/** A poke's hop-spin lasts this long (PenguinAnimInstance plays it; the same number there). */
	constexpr float PokeTime = 0.42f;
	/** A client: a penguin that turned up here this recently but has no entry yet stays tiny (its entry is on its way). */
	constexpr float FreshPenguinAge = 0.6f;

	// The spots (2026-10-27, Linus: "make it so u cant walk in the lobby but just sit or hang there when joinging so penguins cant
	// overlap"): every penguin sits on the snow / a block, or hangs from an edge, on a spot of its own. Sizes in cm.
	constexpr int32 WantSpots = 10;
	/** A seated penguin's body: half its width, its height (feet up); a seat keeps this far from its block's end. */
	constexpr float SeatHalfWidth = 48.f;
	constexpr float SeatHeight = 112.f;
	constexpr float SeatEdgeKeep = 42.f;
	/** Sitting draws the body this much lower than standing (the tags and clicks follow). */
	constexpr float SitLower = 26.f;
	/** Hanging: the capsule stands this far in from the edge; the body is drawn out over the edge (its middle HangOut past it)
	 *  and down (its feet HangDrop under the edge's top). Its tags go further out (HangTagOut past the body's middle). */
	constexpr float HangCapsuleIn = 36.f;
	constexpr float HangOut = 36.f;
	constexpr float HangDrop = 88.f;
	constexpr float HangTagOut = 45.f;
	/** A hang needs more drop than this under the edge (cells), or the sea. */
	constexpr int32 HangMinDropCells = 2;
	/** The spots' packing: the narrowest gap between two seats' middles tried (wider first, as wide as 10 spots allow). */
	constexpr float SeatGapMax = 190.f;
	constexpr float SeatGapMin = 100.f;
	/** What a tag may cover, for the packing (its name pill centred over the head, the x on its right). */
	constexpr float TagReserveLeft = 72.f;
	constexpr float TagReserveRight = 96.f;
	/** A second tag row this much higher (two close neighbours: their tags one over the other, never pushed off). */
	constexpr float TagRowLift = 88.f;
	/** Off its spot this long (pushed, a test's warp): put back. */
	constexpr float OffSpotDistance = 60.f;
	constexpr float OffSpotTime = 1.5f;

	// The waiting gags: one starts every GagGap seconds, never more than MaxGags at once; a penguin waits GagRest after its own.
	constexpr int32 MaxGags = 2;
	constexpr float GagGapMin = 1.4f;
	constexpr float GagGapMax = 3.4f;
	constexpr float FirstGagDelay = 1.5f;
	constexpr float GagRest = 3.f;
	/** A noot reaches a neighbour this far away (X). */
	constexpr float NootFriendRange = 420.f;
	/** The fish's fat: holds this long (s) before it shrinks back. */
	constexpr float FatHoldMin = 9.f;
	constexpr float FatHoldMax = 14.f;
	/** The fish prop's length (cm). */
	constexpr float FishLength = 52.f;

	// Tags (cm, on a plane in front of the penguins: the camera looks from +Y). Rounded slabs with Titan One letters (the 3D
	// menu's toolkit, PinguMenu3D), lit like the island.
	constexpr float TagY = 185.f;
	constexpr float TagAboveHead = 14.f;
	constexpr float NameSize = 20.f;
	constexpr float SkillSize = 12.f;
	constexpr float PlateHeight = 36.f;
	constexpr float SkillPlateHeight = 24.f;
	constexpr float PlatePadding = 14.f;
	constexpr float SlabDepth = 12.f;
	constexpr float SkillGap = 7.f;
	/** The x: a round disc (2026-10-24, "make the x button perfectly round"). */
	constexpr float RemoveSize = 30.f;
	/** How far round each thing a click still counts (cm on its plane; 2026-10-24: generous, the x was a 12 px target). */
	constexpr float RemoveHitMargin = 7.f;
	constexpr float SkillHitMargin = 10.f;
	constexpr float NameHitMargin = 6.f;
	constexpr float BodyHitMargin = 14.f;
	/** The penguin is under the cursor within this box around its middle (half width, below, above). */
	constexpr float PenguinHalfWidth = 48.f;
	constexpr float PenguinBelow = 70.f;
	constexpr float PenguinAbove = 75.f;
	/** The "+": this far above the step's top, this big (cm), this much room for a click. */
	constexpr float PlusAbove = 95.f;
	constexpr float PlusSize = 76.f;
	constexpr float PlusTextSize = 46.f;
	/** The clear space round the "+" (cm past its hover size and its bob) no tag stands in. */
	constexpr float PlusClearance = 18.f;
	constexpr float PlusClickRadius = 62.f;

	// Colours (linear): the menus' navy plates, gold for the pick, the difficulty's green / gold / red, the "+" orange.
	const FLinearColor PlateColor(0.012f, 0.02f, 0.06f);
	const FLinearColor PlateLip(0.004f, 0.007f, 0.022f);
	const FLinearColor FocusColor(1.f, 0.6f, 0.08f);
	const FLinearColor SkillColors[3] = { FLinearColor(0.08f, 0.5f, 0.12f), FLinearColor(0.85f, 0.5f, 0.02f), FLinearColor(0.7f, 0.06f, 0.04f) };
	const FLinearColor RemoveColor(0.6f, 0.05f, 0.04f);
	/** The bots' x without its disc (a red word on the scene) and the "+" without its ball: the word's colour, and while hovered. */
	const FLinearColor PlainRemoveColor(0.95f, 0.27f, 0.2f);
	const FLinearColor PlainPlusColor(0.96f, 0.96f, 0.92f);
	const FLinearColor PlainPlusHover(1.f, 0.82f, 0.3f);
	const FLinearColor PlusColor(1.f, 0.42f, 0.04f);
	const FLinearColor PlusLip(0.45f, 0.12f, 0.01f);
	const FLinearColor TextColor(0.96f, 0.97f, 1.f);
	const FLinearColor MagicColor(0.85f, 0.7f, 1.f);

	static const TCHAR* SkillName(int32 Skill)
	{
		switch (FMath::Clamp(Skill, 0, 2))
		{
		case 0: return TEXT("EASY");
		case 1: return TEXT("NORMAL");
		default: return TEXT("HARD");
		}
	}

	static float EaseOutBack(float T)
	{
		const float C1 = 1.9f;
		const float C3 = C1 + 1.f;
		const float U = T - 1.f;
		return 1.f + C3 * U * U * U + C1 * U * U;
	}

	static double Now(const UWorld* World)
	{
		const APenguinGameState* GameState = World ? APenguinGameState::Get(World) : nullptr;
		return GameState ? GameState->GetFlowTime() : (World ? World->GetTimeSeconds() : 0.0);
	}

	/** Where a ray meets the plane Y = PlaneY (false if it runs along it). */
	static bool RayToPlane(const FVector& Origin, const FVector& Direction, float PlaneY, FVector& OutPoint)
	{
		if (FMath::Abs(Direction.Y) < 1.e-4)
		{
			return false;
		}
		const double T = (PlaneY - Origin.Y) / Direction.Y;
		if (T <= 0.0)
		{
			return false;
		}
		OutPoint = Origin + Direction * T;
		return true;
	}
}

// ---------------------------------------------------------------------------- free functions

namespace PinguLobbyPrivate
{
	static bool bBotsFrozen = false;
	static bool bGagsPaused = false;
}

void PinguLobby::SetBotsFrozen(bool bFrozen)
{
	PinguLobbyPrivate::bBotsFrozen = bFrozen;
}

void PinguLobby::SetGagsPaused(bool bPaused)
{
	PinguLobbyPrivate::bGagsPaused = bPaused;
}

float PinguLobby::GetGagLength(EGag Gag)
{
	switch (Gag)
	{
	case EGag::Fish: return 3.6f;
	case EGag::NootFriend: return 1.9f;
	case EGag::NootCamera: return 1.9f;
	case EGag::Wave: return 2.2f;
	case EGag::Doze: return 5.2f;
	case EGag::Dance: return 3.2f;
	case EGag::Shiver: return 2.4f;
	default: return 0.f;
	}
}

const TCHAR* PinguLobby::GetGagName(EGag Gag)
{
	switch (Gag)
	{
	case EGag::Fish: return TEXT("fish");
	case EGag::NootFriend: return TEXT("noot at a neighbour");
	case EGag::NootCamera: return TEXT("noot at the camera");
	case EGag::Wave: return TEXT("wave");
	case EGag::Doze: return TEXT("doze");
	case EGag::Dance: return TEXT("dance");
	case EGag::Shiver: return TEXT("shiver");
	default: return TEXT("none");
	}
}

PinguLobby::FAnimState PinguLobby::GetAnimState(const APenguinCharacter* Penguin)
{
	FAnimState State;
	const UWorld* World = Penguin ? Penguin->GetWorld() : nullptr;
	const APinguLobby* Lobby = World ? APinguLobby::Get(World) : nullptr;
	if (!Lobby || !Lobby->IsShowing())
	{
		return State;
	}
	State.bLobby = true;
	Lobby->GetPoke(Penguin, State.PokeAge, State.PokeDir);
	Lobby->FillAnimState(Penguin, State);
	return State;
}

void PinguLobby::MagicPuff(const UObject* WorldContext, const FVector& Feet, const FLinearColor& Color, bool bIn)
{
	using namespace PinguLobbyPrivate;
	const FVector Middle = Feet + FVector(0.f, 0.f, 55.f);
	const FLinearColor Dust = FMath::Lerp(FLinearColor::White, MagicColor, 0.45f);
	APinguFX::Puffs(WorldContext, Middle, bIn ? 14 : 12, 48.f, 230.f, FVector::UpVector, 1.f, Dust);
	APinguFX::Puffs(WorldContext, Feet + FVector(0.f, 0.f, 10.f), 8, 34.f, 300.f, FVector::UpVector, 0.25f, FLinearColor::White);
	APinguFX::Sparkles(WorldContext, Middle, 22, 85.f, FMath::Lerp(MagicColor, Color, 0.35f));
	APinguFX::Sparkles(WorldContext, Middle + FVector(0.f, 0.f, 30.f), 10, 60.f, FLinearColor(1.f, 0.9f, 0.55f));
	APinguFX::Stars(WorldContext, Middle, bIn ? 6 : 4, 300.f);
	APinguFX::Ring(WorldContext, Feet + FVector(0.f, 0.f, 6.f), 85.f, FMath::Lerp(MagicColor, Color, 0.5f), 0.35f);
	APinguFX::Sound(WorldContext, EPinguSound::Warp, 0.55f, 0.05f, bIn ? 1.35f : 1.05f);
	APinguFX::Sound(WorldContext, EPinguSound::Pickup, 0.45f, 0.05f, bIn ? 1.2f : 0.85f);
}

// ---------------------------------------------------------------------------- actor

APinguLobby::APinguLobby()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(20.f);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	WalkRand.Initialize(4245);
}

void APinguLobby::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguLobby, Entries);
}

APinguLobby* APinguLobby::Get(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<APinguLobby> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

APinguLobby* APinguLobby::FindOrSpawn(UWorld* World)
{
	APinguLobby* Lobby = Get(World);
	if (!Lobby && World && World->GetNetMode() != NM_Client)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Lobby = World->SpawnActor<APinguLobby>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	return Lobby;
}

void APinguLobby::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ResetLooks();
	Super::EndPlay(EndPlayReason);
}

bool APinguLobby::IsShowing() const
{
	const UWorld* World = GetWorld();
	const APenguinGameState* GameState = World ? APenguinGameState::Get(World) : nullptr;
	const ABlockIsland* Island = World ? ABlockIsland::Get(World) : nullptr;
	return GameState && Island && PinguMenu3D::UseIslandLobby() && GameState->GetPhase() == EPinguPhase::Lobby && Island->Layout == EIslandLayout::Lobby;
}

int32 APinguLobby::GetLobbyTeam(const APenguinCharacter* Penguin)
{
	if (!Penguin)
	{
		return -1;
	}
	if (const APenguinPlayerState* Player = Penguin->GetPlayerState<APenguinPlayerState>())
	{
		return Player->GetTeamChoice();
	}
	const APenguinGameState* GameState = APenguinGameState::Get(Penguin);
	if (!GameState)
	{
		return -1;
	}
	const FString& Name = Penguin->GetDisplayName();
	const FPinguBotSetup* Setup = GameState->GetSettings().Bots.FindByPredicate([&Name](const FPinguBotSetup& Bot) { return Bot.Name == Name; });
	return Setup ? Setup->Team : -1;
}

const FPinguLobbyPenguin* APinguLobby::FindEntry(const APenguinCharacter* Penguin) const
{
	return Penguin ? Entries.FindByPredicate([Penguin](const FPinguLobbyPenguin& Entry) { return Entry.Penguin == Penguin; }) : nullptr;
}

void APinguLobby::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.1f);
	if (HasAuthority())
	{
		TickServer(Dt);
	}
	TickLooks(Dt);
}

// ---------------------------------------------------------------------------- server

void APinguLobby::OnLobbyStarted()
{
	Entries.Reset();
	ServerSeen.Reset();
	bServerStarted = false; // the next server tick takes everybody as already there (and puts them on their spots)
	NextGagAt = static_cast<float>(PinguLobbyPrivate::Now(GetWorld())) + PinguLobbyPrivate::FirstGagDelay;
	BuildSpots();
	ForceNetUpdate();
}

namespace PinguLobbyPrivate
{
	/** A rectangle on the play plane (X across, Z up). */
	struct FRect
	{
		float X0 = 0.f;
		float Z0 = 0.f;
		float X1 = 0.f;
		float Z1 = 0.f;
		bool Overlaps(const FRect& Other, float Margin = 0.f) const
		{
			return X0 < Other.X1 + Margin && Other.X0 < X1 + Margin && Z0 < Other.Z1 + Margin && Other.Z0 < Z1 + Margin;
		}
	};

	/** A place a penguin could wait: its capsule's place, its pose, what its body and its tags cover. */
	struct FSpotCandidate
	{
		FVector Location = FVector::ZeroVector;
		PinguLobby::EPose Pose = PinguLobby::EPose::Sit;
		int8 Side = 1;
		FRect Body;
		FRect Tags;
		/** Its surface (seats on the same lane share their tag rows); hangs have a lane of their own. */
		int32 Lane = 0;
	};

	/** The capsule's half height the spots are planned with (the penguin's: PenguinCharacter's CapsuleHalfHeight). */
	constexpr float PlanHalfHeight = 56.f;
	/** The tags' extent over a head (HeadZ: UpdateTags) - from the skill pill's foot to the name pill's top. */
	constexpr float TagBottom = 6.f;
	constexpr float TagTop = 84.f;

	FSpotCandidate MakeSeat(float X, float Top, int32 Lane)
	{
		FSpotCandidate Seat;
		Seat.Location = FVector(X, 0.f, Top + PlanHalfHeight + 4.f);
		// (Standing since 2026-10-27, Linus: "make the binguin stand on the platform ... and not be slightly glitshed in the
		// ground": the sitting pose sank the body into the snow.)
		Seat.Pose = PinguLobby::EPose::Stand;
		Seat.Lane = Lane;
		Seat.Body = { X - SeatHalfWidth, Top, X + SeatHalfWidth, Top + SeatHeight };
		const float Head = Top + 2.f * PlanHalfHeight + 12.f;
		// The low row (the packing lifts it a row when it would cover a neighbour's tag or body - as UpdateTags does).
		Seat.Tags = { X - TagReserveLeft, Head + TagBottom, X + TagReserveRight, Head + TagTop };
		return Seat;
	}

	FSpotCandidate MakeHang(float Edge, float Top, int8 Side, int32 Lane)
	{
		FSpotCandidate Hang;
		Hang.Location = FVector(Edge - Side * HangCapsuleIn, 0.f, Top + PlanHalfHeight + 4.f);
		Hang.Pose = PinguLobby::EPose::Hang;
		Hang.Side = Side;
		Hang.Lane = Lane;
		const float BodyX = Edge + Side * HangOut;
		Hang.Body = { BodyX - SeatHalfWidth, Top - HangDrop, BodyX + SeatHalfWidth, Top + 10.f };
		const float Head = Top + 2.f * PlanHalfHeight + 12.f - HangDrop;
		const float TagX = BodyX + Side * HangTagOut;
		Hang.Tags = { TagX - TagReserveLeft, Head + TagBottom, TagX + TagReserveRight, Head + TagTop };
		return Hang;
	}
}

void APinguLobby::BuildSpots()
{
	using namespace PinguLobbyPrivate;
	Spots.Reset();
	const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	if (!HasAuthority() || !Island || Island->Layout != EIslandLayout::Lobby)
	{
		return;
	}
	const FBox Bounds = Island->GetBounds();
	if (!Bounds.IsValid)
	{
		return;
	}
	const float Cell = Island->CellSize;
	const FVector Origin = Island->GetActorLocation();
	const FIntPoint Lo = Island->WorldToCell(Bounds.Min + FVector(1.f, 0.f, 1.f));
	const FIntPoint Hi = Island->WorldToCell(Bounds.Max - FVector(1.f, 0.f, 1.f));
	// The "+" floats over the step at the right end: nobody sits there, no body or tag comes near it.
	FVector Step;
	const bool bStep = Island->GetLobbyStepTop(Step);
	const float StepLeft = bStep ? static_cast<float>(Step.X) - Cell : TNumericLimits<float>::Max();
	const FRect PlusBox = bStep ? FRect{ static_cast<float>(Step.X) - 0.5f * PlusSize - 10.f, static_cast<float>(Step.Z) + PlusAbove - 0.5f * PlusSize - 10.f,
		static_cast<float>(Step.X) + 0.5f * PlusSize + 10.f, static_cast<float>(Step.Z) + PlusAbove + 0.5f * PlusSize + 10.f } : FRect{ 1.e9f, 1.e9f, 1.e9f, 1.e9f };

	// The surfaces to sit on (a block with two free cells over it), as lanes: runs of neighbouring columns at the same height.
	struct FLane
	{
		int32 FirstX = 0;
		int32 LastX = 0;
		int32 TopCell = 0;
	};
	TArray<FLane> Lanes;
	for (int32 X = Lo.X; X <= Hi.X; ++X)
	{
		if (Origin.X + (X + 1) * Cell > StepLeft + 1.f)
		{
			continue; // the "+"'s step
		}
		for (int32 Z = Lo.Y; Z <= Hi.Y; ++Z)
		{
			if (Island->GetBlock(X, Z) == EBlockKind::None || Island->GetBlock(X, Z + 1) != EBlockKind::None || Island->GetBlock(X, Z + 2) != EBlockKind::None)
			{
				continue;
			}
			FLane* Lane = Lanes.FindByPredicate([X, Z](const FLane& Item) { return Item.TopCell == Z && Item.LastX == X - 1; });
			if (Lane)
			{
				Lane->LastX = X;
			}
			else
			{
				Lanes.Add({ X, X, Z });
			}
		}
	}
	// Candidates: hangs off the lanes' ends where it drops far enough (or into the sea), then the seats along every lane.
	TArray<FSpotCandidate> Hangs;
	TArray<TArray<FSpotCandidate>> Seats;
	// (The longest first - the ground: most seats there; a short step or platform takes what is left.)
	Lanes.Sort([](const FLane& A, const FLane& B) { return A.LastX - A.FirstX != B.LastX - B.FirstX ? A.LastX - A.FirstX > B.LastX - B.FirstX : A.TopCell > B.TopCell; });
	for (int32 Index = 0; Index < Lanes.Num(); ++Index)
	{
		const FLane& Lane = Lanes[Index];
		const float Top = static_cast<float>(Origin.Z) + (Lane.TopCell + 1) * Cell;
		const float X0 = static_cast<float>(Origin.X) + Lane.FirstX * Cell;
		const float X1 = static_cast<float>(Origin.X) + (Lane.LastX + 1) * Cell;
		// (No hangs off the lanes' ends since 2026-10-27 - Linus: "remove the one pingu in the lobby hanging from the edge".)
		TArray<FSpotCandidate>& Row = Seats.AddDefaulted_GetRef();
		for (float X = X0 + SeatEdgeKeep; X <= X1 - SeatEdgeKeep + 0.1f; X += 5.f)
		{
			Row.Add(MakeSeat(X, Top, Index));
		}
	}
	// Packed with the widest gap between seats that still gives everybody a spot: first the bodies, left to right over every lane
	// at once (no two overlap, nothing near the "+"); then the tags, the lowest heads first and each from the left, in the low row
	// or a row up when the low one would cover another penguin's body or a tag placed before (UpdateTags lays them out the same
	// way); a seat whose tag fits in neither is dropped.
	TArray<FSpotCandidate> All;
	for (const FSpotCandidate& Hang : Hangs)
	{
		All.Add(Hang);
	}
	for (const TArray<FSpotCandidate>& Row : Seats)
	{
		All.Append(Row);
	}
	All.StableSort([](const FSpotCandidate& A, const FSpotCandidate& B) { return A.Location.X != B.Location.X ? A.Location.X < B.Location.X : A.Location.Z > B.Location.Z; });
	TArray<FSpotCandidate> Best;
	float BestGap = 0.f;
	for (float Gap = SeatGapMax; Gap >= SeatGapMin - 0.1f; Gap -= 5.f)
	{
		TArray<FSpotCandidate> Bodies;
		for (const FSpotCandidate& Candidate : All)
		{
			bool bFits = !Candidate.Body.Overlaps(PlusBox);
			for (const FSpotCandidate& Old : Bodies)
			{
				bFits &= !Candidate.Body.Overlaps(Old.Body, 4.f) && !(Candidate.Lane == Old.Lane && FMath::Abs(Candidate.Location.X - Old.Location.X) < Gap);
			}
			if (bFits)
			{
				Bodies.Add(Candidate);
			}
		}
		Bodies.StableSort([](const FSpotCandidate& A, const FSpotCandidate& B) { return A.Location.Z != B.Location.Z ? A.Location.Z < B.Location.Z : A.Location.X < B.Location.X; });
		TArray<FSpotCandidate> Taken;
		for (int32 Index = 0; Index < Bodies.Num(); ++Index)
		{
			for (int32 Row = 0; Row < 2; ++Row)
			{
				FSpotCandidate Placed = Bodies[Index];
				Placed.Tags.Z0 += Row * TagRowLift;
				Placed.Tags.Z1 += Row * TagRowLift;
				bool bClear = !Placed.Tags.Overlaps(PlusBox);
				for (int32 Other = 0; Other < Bodies.Num(); ++Other)
				{
					bClear &= Other == Index || !Placed.Tags.Overlaps(Bodies[Other].Body);
				}
				for (const FSpotCandidate& Old : Taken)
				{
					bClear &= !Placed.Tags.Overlaps(Old.Tags);
				}
				if (bClear)
				{
					Taken.Add(Placed);
					break;
				}
			}
		}
		if (Taken.Num() > Best.Num() || Best.Num() == 0)
		{
			Best = Taken;
			BestGap = Gap;
		}
		if (Taken.Num() >= WantSpots)
		{
			Best = Taken;
			BestGap = Gap;
			break;
		}
	}
	if (Best.Num() == 0)
	{
		return;
	}
	// The order newcomers get them in: the first one a little left of the middle on the ground, then always the spot farthest
	// from all taken ones (a few penguins spread over the whole island, high and low).
	const float MiddleX = static_cast<float>(Bounds.GetCenter().X) - Cell;
	int32 First = 0;
	for (int32 Index = 1; Index < Best.Num(); ++Index)
	{
		const auto Score = [MiddleX](const FSpotCandidate& Spot)
		{
			return FMath::Abs(Spot.Location.X - MiddleX) + Spot.Location.Z * 2.0 + (Spot.Pose == PinguLobby::EPose::Hang ? 1.e5 : 0.0);
		};
		if (Score(Best[Index]) < Score(Best[First]))
		{
			First = Index;
		}
	}
	TArray<bool> Used;
	Used.Init(false, Best.Num());
	TArray<int32> Order = { First };
	Used[First] = true;
	while (Order.Num() < Best.Num())
	{
		int32 Pick = INDEX_NONE;
		double PickDistance = -1.0;
		for (int32 Index = 0; Index < Best.Num(); ++Index)
		{
			if (Used[Index])
			{
				continue;
			}
			double Nearest = TNumericLimits<double>::Max();
			for (const int32 Chosen : Order)
			{
				Nearest = FMath::Min(Nearest, FVector::Dist(Best[Index].Location, Best[Chosen].Location));
			}
			if (Nearest > PickDistance)
			{
				PickDistance = Nearest;
				Pick = Index;
			}
		}
		Used[Pick] = true;
		Order.Add(Pick);
	}
	FString Line;
	for (const int32 Index : Order)
	{
		FSpot& Spot = Spots.AddDefaulted_GetRef();
		Spot.Location = Best[Index].Location;
		Spot.Pose = Best[Index].Pose;
		Spot.Side = Best[Index].Side;
		Line += FString::Printf(TEXT(" %s %.0f,%.0f;"), Spot.Pose == PinguLobby::EPose::Hang ? TEXT("hang") : TEXT("stand"), Spot.Location.X, Spot.Location.Z);
	}
	UE_LOG(LogPingu, Display, TEXT("[LOBBY] %d spots (seats %.0f cm apart):%s"), Spots.Num(), BestGap, *Line);
}

int32 APinguLobby::GetSpotOf(const APenguinCharacter* Penguin) const
{
	const FServerSeen* Seen = ServerSeen.FindByPredicate([Penguin](const FServerSeen& Item) { return Item.Penguin.Get() == Penguin; });
	return Seen ? Seen->Spot : INDEX_NONE;
}

int32 APinguLobby::AssignSpot(const APenguinCharacter* Penguin) const
{
	for (int32 Index = 0; Index < Spots.Num(); ++Index)
	{
		const bool bTaken = ServerSeen.ContainsByPredicate([Index, Penguin](const FServerSeen& Seen)
		{
			return Seen.Spot == Index && Seen.Penguin.IsValid() && Seen.Penguin.Get() != Penguin;
		});
		if (!bTaken)
		{
			return Index;
		}
	}
	return INDEX_NONE; // more penguins than spots (never with ten): it stands where it is
}

void APinguLobby::PlaceOnSpot(APenguinCharacter* Penguin, int32 Spot)
{
	if (!HasAuthority() || !IsValid(Penguin) || !Spots.IsValidIndex(Spot))
	{
		return;
	}
	const FSpot& Where = Spots[Spot];
	Penguin->ApplyVirtualMoveInput(FVector2D::ZeroVector);
	if (FVector::Dist2D(FVector(Penguin->GetActorLocation().X, Penguin->GetActorLocation().Z, 0.f), FVector(Where.Location.X, Where.Location.Z, 0.f)) > 8.f)
	{
		// (A LAN player's own penguin: the server's reset reaches its owner too.)
		Penguin->ResetForRound(Where.Location);
	}
	if (FPinguLobbyPenguin* Entry = Entries.FindByPredicate([Penguin](const FPinguLobbyPenguin& Item) { return Item.Penguin == Penguin; }))
	{
		Entry->Pose = static_cast<uint8>(Where.Pose);
		// A seat turns from the side it faces (the middle, ResetForRound), a hang hangs out over its edge.
		Entry->Side = Where.Pose == PinguLobby::EPose::Hang ? Where.Side : (Where.Location.X > 0.f ? -1 : 1);
	}
}

void APinguLobby::HoldSpots(float Dt)
{
	using namespace PinguLobbyPrivate;
	for (FServerSeen& Seen : ServerSeen)
	{
		APenguinCharacter* Penguin = Seen.Penguin.Get();
		const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
		if (!Penguin || !Entry || Entry->PuffOutAt >= 0.f || !Spots.IsValidIndex(Seen.Spot))
		{
			continue;
		}
		const FVector Want = Spots[Seen.Spot].Location;
		const FVector At = Penguin->GetActorLocation();
		const bool bOff = FMath::Abs(At.X - Want.X) > OffSpotDistance || FMath::Abs(At.Z - Want.Z) > OffSpotDistance;
		Seen.OffSpotTime = bOff ? Seen.OffSpotTime + Dt : 0.f;
		if (Seen.OffSpotTime >= OffSpotTime)
		{
			UE_LOG(LogPingu, Log, TEXT("[LOBBY] %s was off its spot (%.0f, %.0f): back to %.0f, %.0f"), *Penguin->GetDisplayName(), At.X, At.Z, Want.X, Want.Z);
			Seen.OffSpotTime = 0.f;
			PlaceOnSpot(Penguin, Seen.Spot);
		}
	}
}

void APinguLobby::PuffAway(APenguinCharacter* Penguin)
{
	using namespace PinguLobbyPrivate;
	if (!HasAuthority() || !IsValid(Penguin))
	{
		return;
	}
	const float At = static_cast<float>(Now(GetWorld()));
	FPinguLobbyPenguin* Entry = Entries.FindByPredicate([Penguin](const FPinguLobbyPenguin& Item) { return Item.Penguin == Penguin; });
	if (!Entry)
	{
		Entry = &Entries.AddDefaulted_GetRef();
		Entry->Penguin = Penguin;
	}
	Entry->PuffOutAt = At;
	Entry->Gag = 0;
	ServerSeen.RemoveAll([Penguin](const FServerSeen& Seen) { return Seen.Penguin.Get() == Penguin; });
	Penguin->ApplyVirtualMoveInput(FVector2D::ZeroVector);
	Penguin->SetActorEnableCollision(false);
	Penguin->SetInputLocked(true);
	{
		FPinguFXBroadcastScope Broadcast(this);
		PinguLobby::MagicPuff(this, GetBodyCentre(Penguin) - FVector(0.f, 0.f, 55.f), Penguin->GetPlayerColor(), false);
	}
	// Gone once it has shrunk away (the entry goes with it).
	Penguin->SetLifeSpan(PopOutTime + 0.05f);
	ForceNetUpdate();
}

void APinguLobby::TickServer(float Dt)
{
	using namespace PinguLobbyPrivate;
	const UWorld* World = GetWorld();
	const APenguinGameMode* GameMode = World ? World->GetAuthGameMode<APenguinGameMode>() : nullptr;
	if (!GameMode || !IsShowing())
	{
		if (Entries.Num() > 0 || ServerSeen.Num() > 0)
		{
			Entries.Reset();
			ServerSeen.Reset();
			ForceNetUpdate();
		}
		Spots.Reset();
		bServerStarted = false;
		return;
	}
	if (Spots.Num() == 0)
	{
		BuildSpots();
	}
	const float At = static_cast<float>(Now(World));
	bool bChanged = false;
	// Gone: a player who left (their penguin is destroyed at once) - a puff where they stood. A bot sent away has its entry
	// until it is destroyed (PuffAway).
	for (int32 Index = ServerSeen.Num() - 1; Index >= 0; --Index)
	{
		if (!ServerSeen[Index].Penguin.IsValid())
		{
			FPinguFXBroadcastScope Broadcast(this);
			PinguLobby::MagicPuff(this, ServerSeen[Index].Feet, ServerSeen[Index].Color, false);
			UE_LOG(LogPingu, Display, TEXT("[LOBBY] a penguin left: puff at %.0f, %.0f"), ServerSeen[Index].Feet.X, ServerSeen[Index].Feet.Z);
			ServerSeen.RemoveAt(Index);
		}
	}
	const int32 Before = Entries.Num();
	Entries.RemoveAll([](const FPinguLobbyPenguin& Entry) { return !IsValid(Entry.Penguin); });
	bChanged |= Entries.Num() != Before;

	for (APenguinCharacter* Penguin : GameMode->GetPenguins())
	{
		if (!IsValid(Penguin))
		{
			continue;
		}
		const int32 Team = GetLobbyTeam(Penguin);
		FServerSeen* Seen = ServerSeen.FindByPredicate([Penguin](const FServerSeen& Item) { return Item.Penguin.Get() == Penguin; });
		if (!Seen)
		{
			// Arrived: a bot added, a player joined (or everybody, as the lobby opens: no puff then). Straight onto a spot of its
			// own (it pops up out of the dust there).
			const int32 Spot = AssignSpot(Penguin);
			Seen = &ServerSeen.AddDefaulted_GetRef();
			Seen->Penguin = Penguin;
			Seen->Team = Team;
			Seen->Spot = Spot;
			Seen->GagFreeAt = At + FirstGagDelay;
			FPinguLobbyPenguin& Entry = Entries.AddDefaulted_GetRef();
			Entry.Penguin = Penguin;
			Entry.PuffInAt = bServerStarted ? At : -1000.f;
			PlaceOnSpot(Penguin, Spot);
			if (bServerStarted)
			{
				FPinguFXBroadcastScope Broadcast(this);
				PinguLobby::MagicPuff(this, GetBodyCentre(Penguin) - FVector(0.f, 0.f, 55.f), Penguin->GetPlayerColor(), true);
				UE_LOG(LogPingu, Display, TEXT("[LOBBY] %s puffs in at %.0f, %.0f (spot %d)"), *Penguin->GetDisplayName(), Penguin->GetActorLocation().X,
					Penguin->GetActorLocation().Z, Spot);
			}
			bChanged = true;
		}
		else if (Seen->Team != Team)
		{
			// Its team changed (a click on it, a client's pick): the hop-spin, a sparkle in the new colour.
			const int32 Old = Seen->Team;
			Seen->Team = Team;
			if (FPinguLobbyPenguin* Entry = Entries.FindByPredicate([Penguin](const FPinguLobbyPenguin& Item) { return Item.Penguin == Penguin; }))
			{
				++Entry->PokeSerial;
				const bool bForward = Team == Old + 1 || (Old == PinguMaxTeams - 1 && Team == -1);
				Entry->PokeDir = bForward ? 1 : -1;
				bChanged = true;
			}
			FPinguFXBroadcastScope Broadcast(this);
			const FLinearColor Shown = Team >= 0 ? APenguinCharacter::GetTeamColor(Team) : Penguin->GetPlayerColor();
			APinguFX::Sparkles(this, GetBodyCentre(Penguin) + FVector(0.f, 0.f, 20.f), 12, 55.f, Shown);
			APinguFX::Sound(this, EPinguSound::Pickup, 0.35f, 0.08f, 1.5f);
			UE_LOG(LogPingu, Display, TEXT("[LOBBY] %s: team %d -> %d (poke)"), *Penguin->GetDisplayName(), Old, Team);
		}
		Seen->Feet = GetBodyCentre(Penguin) - FVector(0.f, 0.f, 55.f);
		Seen->Color = Penguin->GetPlayerColor();
	}
	bServerStarted = true;
	HoldSpots(Dt);
	TickGags(Dt);
	if (bChanged)
	{
		ForceNetUpdate();
	}
}

// ---------------------------------------------------------------------------- the waiting gags (server)

APenguinCharacter* APinguLobby::FindNeighbour(const APenguinCharacter* Penguin, float MaxDistance) const
{
	APenguinCharacter* Best = nullptr;
	float BestDistance = MaxDistance;
	const FVector From = GetBodyCentre(Penguin);
	for (const FPinguLobbyPenguin& Entry : Entries)
	{
		if (!IsValid(Entry.Penguin) || Entry.Penguin == Penguin || Entry.PuffOutAt >= 0.f)
		{
			continue;
		}
		const FVector To = GetBodyCentre(Entry.Penguin);
		const float Distance = static_cast<float>(FMath::Abs(To.X - From.X));
		if (Distance < BestDistance && Distance > 60.f && FMath::Abs(To.Z - From.Z) < 160.f)
		{
			BestDistance = Distance;
			Best = Entry.Penguin;
		}
	}
	return Best;
}

bool APinguLobby::StartGag(APenguinCharacter* Penguin, PinguLobby::EGag Gag, APenguinCharacter* Target)
{
	using namespace PinguLobbyPrivate;
	using PinguLobby::EGag;
	FPinguLobbyPenguin* Entry = Penguin ? Entries.FindByPredicate([Penguin](const FPinguLobbyPenguin& Item) { return Item.Penguin == Penguin; }) : nullptr;
	if (!HasAuthority() || !Entry || Entry->PuffOutAt >= 0.f || Gag == EGag::None || Gag >= EGag::Count)
	{
		return false;
	}
	const float At = static_cast<float>(Now(GetWorld()));
	if (Gag == EGag::NootFriend && !Target)
	{
		Target = FindNeighbour(Penguin, NootFriendRange * 1.5f);
	}
	if (Gag == EGag::NootFriend && !Target)
	{
		Gag = EGag::NootCamera; // nobody near: at the player instead
	}
	Entry->Gag = static_cast<uint8>(Gag);
	Entry->GagAt = At;
	Entry->GagTarget = Gag == EGag::NootFriend ? Target : nullptr;
	if (Gag == EGag::Fish)
	{
		Entry->FatAt = At + PinguLobby::FatDelay;
		Entry->FatUntil = Entry->FatAt + PinguLobby::FatSwell + WalkRand.FRandRange(FatHoldMin, FatHoldMax) + PinguLobby::FatShrink;
	}
	if (FServerSeen* Seen = ServerSeen.FindByPredicate([Penguin](const FServerSeen& Item) { return Item.Penguin.Get() == Penguin; }))
	{
		Seen->LastGag = static_cast<uint8>(Gag);
		Seen->GagFreeAt = At + PinguLobby::GetGagLength(Gag) + GagRest;
		Seen->EventsDoneTo = 0.f;
	}
	UE_LOG(LogPingu, Display, TEXT("[LOBBY] gag: %s - %s%s"), *Penguin->GetDisplayName(), PinguLobby::GetGagName(Gag),
		Target && Gag == EGag::NootFriend ? *FString::Printf(TEXT(" (at %s)"), *Target->GetDisplayName()) : TEXT(""));
	ForceNetUpdate();
	return true;
}

void APinguLobby::SetFatFor(APenguinCharacter* Penguin, float Seconds)
{
	using namespace PinguLobbyPrivate;
	FPinguLobbyPenguin* Entry = Penguin ? Entries.FindByPredicate([Penguin](const FPinguLobbyPenguin& Item) { return Item.Penguin == Penguin; }) : nullptr;
	if (!HasAuthority() || !Entry)
	{
		return;
	}
	const float At = static_cast<float>(Now(GetWorld()));
	Entry->FatAt = At - PinguLobby::FatSwell;
	Entry->FatUntil = At + FMath::Max(Seconds, 0.f);
	ForceNetUpdate();
}

void APinguLobby::TickGags(float Dt)
{
	using namespace PinguLobbyPrivate;
	using PinguLobby::EGag;
	const float At = static_cast<float>(Now(GetWorld()));
	// The gags playing: their moments (sounds, effects - every machine sees them); the fat ones going down again.
	int32 Playing = 0;
	for (const FPinguLobbyPenguin& Entry : Entries)
	{
		FServerSeen* Seen = ServerSeen.FindByPredicate([&Entry](const FServerSeen& Item) { return Item.Penguin.Get() == Entry.Penguin; });
		if (!IsValid(Entry.Penguin) || !Seen || Entry.PuffOutAt >= 0.f)
		{
			continue;
		}
		const EGag Gag = static_cast<EGag>(Entry.Gag);
		const float Length = PinguLobby::GetGagLength(Gag);
		const float Age = At - Entry.GagAt;
		if (Gag != EGag::None && Seen->EventsDoneTo < Length)
		{
			const float To = FMath::Min(Age, Length);
			FireGagEvents(Entry.Penguin, Entry, Seen->EventsDoneTo, To);
			Seen->EventsDoneTo = To;
		}
		Playing += Gag != EGag::None && Age < Length ? 1 : 0;
		if (Entry.FatUntil > At && At >= Entry.FatUntil - PinguLobby::FatShrink && Seen->FatShrinkFired != Entry.FatUntil)
		{
			// The fat goes: a long "pfff" out of the beak.
			Seen->FatShrinkFired = Entry.FatUntil;
			const FVector Body = GetBodyCentre(Entry.Penguin);
			FPinguFXBroadcastScope Broadcast(this);
			APinguFX::Sound(this, EPinguSound::Shrink, 0.45f, 0.05f, 0.9f);
			APinguFX::Puffs(this, Body + FVector(0.f, 30.f, 30.f), 7, 18.f, 220.f, FVector(0.f, 0.7f, 0.7f), 0.35f, FLinearColor::White);
		}
	}
	if (bGagsPaused || bBotsFrozen || Playing >= MaxGags || At < NextGagAt)
	{
		return;
	}
	// A new one: somebody who has rested since its last one, something it did not do last time.
	TArray<FServerSeen*> Free;
	for (FServerSeen& Seen : ServerSeen)
	{
		const FPinguLobbyPenguin* Entry = FindEntry(Seen.Penguin.Get());
		float PokeAge = -1.f;
		float PokeDir = 1.f;
		const bool bPoked = GetPoke(Seen.Penguin.Get(), PokeAge, PokeDir);
		if (Entry && Entry->PuffOutAt < 0.f && At >= Seen.GagFreeAt && At - Entry->PuffInAt > PopInTime + 1.f && !bPoked)
		{
			Free.Add(&Seen);
		}
	}
	if (Free.Num() == 0)
	{
		NextGagAt = At + 0.5f;
		return;
	}
	FServerSeen& Seen = *Free[WalkRand.RandRange(0, Free.Num() - 1)];
	APenguinCharacter* Penguin = Seen.Penguin.Get();
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	const bool bHang = Entry && Entry->Pose == static_cast<uint8>(PinguLobby::EPose::Hang);
	const bool bFat = Entry && Entry->FatUntil > At;
	APenguinCharacter* Neighbour = FindNeighbour(Penguin, NootFriendRange);
	struct FChoice
	{
		EGag Gag;
		float Weight;
	};
	TArray<FChoice> Choices;
	const auto Offer = [&Choices, &Seen](EGag Gag, float Weight, bool bCan)
	{
		if (bCan && Weight > 0.f && static_cast<uint8>(Gag) != Seen.LastGag)
		{
			Choices.Add({ Gag, Weight });
		}
	};
	Offer(EGag::Fish, bHang ? 1.5f : 3.f, !bFat);
	Offer(EGag::NootFriend, 3.f, Neighbour != nullptr);
	Offer(EGag::NootCamera, 2.f, true);
	Offer(EGag::Wave, bHang ? 3.f : 2.f, true);
	Offer(EGag::Doze, 2.f, !bHang);
	Offer(EGag::Dance, 2.f, !bHang);
	Offer(EGag::Shiver, 1.5f, true);
	float Total = 0.f;
	for (const FChoice& Choice : Choices)
	{
		Total += Choice.Weight;
	}
	float Roll = WalkRand.FRandRange(0.f, Total);
	EGag Pick = Choices.Num() > 0 ? Choices.Last().Gag : EGag::Wave;
	for (const FChoice& Choice : Choices)
	{
		Roll -= Choice.Weight;
		if (Roll <= 0.f)
		{
			Pick = Choice.Gag;
			break;
		}
	}
	StartGag(Penguin, Pick, Pick == EGag::NootFriend ? Neighbour : nullptr);
	NextGagAt = At + WalkRand.FRandRange(GagGapMin, GagGapMax);
}

void APinguLobby::FireGagEvents(APenguinCharacter* Penguin, const FPinguLobbyPenguin& Entry, float From, float To)
{
	using namespace PinguLobbyPrivate;
	using PinguLobby::EGag;
	const auto Crossed = [From, To](float T) { return From < T && To >= T; };
	const FVector Body = GetBodyCentre(Penguin);
	// The beak: high on the body, toward the camera (the penguins turn to it for most gags).
	const FVector Beak = Body + FVector(0.f, 28.f, 26.f);
	FPinguFXBroadcastScope Broadcast(this);
	switch (static_cast<EGag>(Entry.Gag))
	{
	case EGag::Fish:
		if (Crossed(0.05f))
		{
			APinguFX::Sound(this, EPinguSound::Pickup, 0.35f, 0.05f, 1.4f);
			APinguFX::Sparkles(this, Body + FVector(0.f, 40.f, 0.f), 6, 25.f, FLinearColor(1.f, 0.85f, 0.4f));
		}
		if (Crossed(PinguLobby::FishTossTime))
		{
			APinguFX::Sound(this, EPinguSound::Throw, 0.4f, 0.05f, 1.3f); // up it goes
		}
		if (Crossed(PinguLobby::FishGulpTime))
		{
			APinguFX::Sound(this, EPinguSound::Pickup, 0.6f, 0.03f, 0.5f); // gulp
			APinguFX::Sparkles(this, Beak, 5, 18.f, FLinearColor(0.7f, 0.85f, 1.f));
		}
		if (Crossed(PinguLobby::FatDelay))
		{
			APinguFX::Sound(this, EPinguSound::Grow, 0.55f, 0.05f, 1.15f);
			APinguFX::Puffs(this, Body, 6, 24.f, 260.f, FVector::UpVector, 1.f, FLinearColor::White);
		}
		if (Crossed(PinguLobby::FishBurpTime))
		{
			// A happy burp: a low squawk and a little cloud out of the beak.
			APinguFX::Sound(this, EPinguSound::Squawk, 0.5f, 0.02f, 0.5f);
			APinguFX::Puffs(this, Beak + FVector(0.f, 15.f, 0.f), 5, 16.f, 140.f, FVector(0.f, 1.f, 0.4f), 0.35f, FLinearColor(0.95f, 0.97f, 1.f));
			APinguFX::Ring(this, Beak + FVector(0.f, 30.f, 0.f), 20.f, FLinearColor(1.f, 1.f, 1.f), 0.25f);
		}
		break;
	case EGag::NootFriend:
	case EGag::NootCamera:
		for (int32 Blast = 0; Blast < 2; ++Blast)
		{
			if (!Crossed(PinguLobby::NootBlastTimes[Blast]))
			{
				continue;
			}
			// The noot's own look and sound (the beak stretches into the horn), but no blast: nothing is pushed.
			const APenguinCharacter* Target = Entry.GagTarget;
			const bool bFriend = Entry.Gag == static_cast<uint8>(EGag::NootFriend) && IsValid(Target);
			const float DirX = bFriend ? (Target->GetActorLocation().X >= Penguin->GetActorLocation().X ? 1.f : -1.f) : Penguin->GetFacing();
			Penguin->PlayAimedAction(EPenguinClip::Noot, 1.4f, FVector(DirX, 0.f, 0.f));
			APinguFX::Sound(this, EPinguSound::Noot, 0.7f, 0.03f, Blast == 0 ? 1.f : 1.12f);
			if (bFriend)
			{
				const FVector Mouth = Body + FVector(DirX * 40.f, 0.f, 26.f);
				for (int32 Index = 0; Index < 3; ++Index)
				{
					APinguFX::Ring(this, Mouth + FVector(DirX * (50.f + Index * 70.f), 0.f, 0.f), 26.f + Index * 18.f, FLinearColor(1.f, 0.92f, 0.6f), 0.18f + Index * 0.06f);
				}
				if (Blast == 0)
				{
					APinguFX::Sound(this, EPinguSound::Squawk, 0.3f, 0.05f, 1.5f);
					APinguFX::Stars(this, GetBodyCentre(Target) + FVector(0.f, 0.f, 50.f), 2, 140.f);
				}
			}
			else
			{
				// Straight at the player: rings coming out of the screen, a little shake.
				for (int32 Index = 0; Index < 3; ++Index)
				{
					APinguFX::Ring(this, Beak + FVector(0.f, 40.f + Index * 70.f, 0.f), 22.f + Index * 22.f, FLinearColor(1.f, 0.92f, 0.6f), 0.2f + Index * 0.07f);
				}
				APinguFX::Shake(this, 0.12f);
			}
		}
		break;
	case EGag::Doze:
		if (Crossed(PinguLobby::DozeStartleTime))
		{
			APinguFX::Sound(this, EPinguSound::Squawk, 0.45f, 0.05f, 1.35f);
			APinguFX::Stars(this, Body + FVector(0.f, 0.f, 60.f), 3, 160.f);
		}
		break;
	case EGag::Shiver:
		if (Crossed(0.05f))
		{
			APinguFX::Flakes(this, Body + FVector(0.f, 0.f, 90.f), 8, 50.f, FLinearColor(0.75f, 0.9f, 1.f));
		}
		for (const float Breath : { 0.35f, 1.05f, 1.75f })
		{
			if (Crossed(Breath))
			{
				APinguFX::Puffs(this, Beak, 3, 12.f, 70.f, FVector(0.f, 1.f, 0.3f), 0.3f, FLinearColor::White);
			}
		}
		break;
	case EGag::Dance:
		for (const float Beat : { 0.05f, 1.f, 2.f })
		{
			if (Crossed(Beat))
			{
				APinguFX::Sparkles(this, Body + FVector(0.f, 20.f, 40.f), 5, 45.f, Penguin->GetPlayerColor());
			}
		}
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------- every machine

void APinguLobby::ResetLooks()
{
	if (!bLooksApplied)
	{
		return;
	}
	bLooksApplied = false;
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<APenguinCharacter> It(const_cast<UWorld*>(World)); It; ++It)
		{
			It->SetHatColorIndex(-1);
			It->SetLobbyPopScale(1.f);
		}
	}
	for (FTagView& View : TagViews)
	{
		if (View.Root)
		{
			View.Root->SetVisibility(false, true);
		}
	}
	if (PlusRoot)
	{
		PlusRoot->SetVisibility(false, true);
	}
	bPlusShown = false;
	HideProps();
}

APenguinHUD* APinguLobby::GetLocalHUD(APlayerController*& OutPlayer) const
{
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	OutPlayer = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	return OutPlayer ? Cast<APenguinHUD>(OutPlayer->GetHUD()) : nullptr;
}

float APinguLobby::GetPuffInAge(const APenguinCharacter* Penguin) const
{
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	return Entry ? static_cast<float>(PinguLobbyPrivate::Now(GetWorld()) - Entry->PuffInAt) : -1.f;
}

float APinguLobby::GetPopScale(const APenguinCharacter* Penguin) const
{
	using namespace PinguLobbyPrivate;
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	const double At = Now(GetWorld());
	if (!Entry)
	{
		// A client: a penguin that just turned up waits tiny for its entry (it pops in with it).
		const UWorld* World = GetWorld();
		const bool bFresh = Penguin && World && !HasAuthority() && World->GetTimeSeconds() - Penguin->CreationTime < FreshPenguinAge;
		return bFresh ? 0.01f : 1.f;
	}
	if (Entry->PuffOutAt >= 0.f)
	{
		const float T = FMath::Clamp(static_cast<float>(At - Entry->PuffOutAt) / PopOutTime, 0.f, 1.f);
		// A little swell, then gone.
		return FMath::Max(0.01f, T < 0.25f ? 1.f + 0.6f * T : (1.f - (T - 0.25f) / 0.75f) * 1.15f);
	}
	const float T = static_cast<float>(At - Entry->PuffInAt) / PopInTime;
	if (T >= 1.f)
	{
		return 1.f;
	}
	return FMath::Max(0.01f, EaseOutBack(FMath::Clamp(T, 0.f, 1.f)));
}

void APinguLobby::TickLooks(float Dt)
{
	using namespace PinguLobbyPrivate;
	if (!IsShowing())
	{
		ResetLooks();
		EntriesSeen.Reset();
		PuffingOutSeen.Reset();
		Focused.Reset();
		bPlusFocused = false;
		Hovered = FHit();
		return;
	}
	bLooksApplied = true;
	const UWorld* World = GetWorld();
	const double At = Now(World);
	// Puffs and pokes as this machine sees them (the entries came by replication on a client).
	for (const FPinguLobbyPenguin& Entry : Entries)
	{
		APenguinCharacter* Penguin = Entry.Penguin;
		if (!IsValid(Penguin))
		{
			continue;
		}
		if (!EntriesSeen.Contains(Penguin))
		{
			EntriesSeen.Add(Penguin);
			PuffInsSeen += At - Entry.PuffInAt < 2.0 ? 1 : 0;
		}
		if (Entry.PuffOutAt >= 0.f && !PuffingOutSeen.Contains(Penguin))
		{
			PuffingOutSeen.Add(Penguin);
			++PuffOutsSeen;
		}
		FPokeSeen* Poke = PokeSeen.FindByPredicate([Penguin](const FPokeSeen& Item) { return Item.Penguin.Get() == Penguin; });
		if (!Poke)
		{
			Poke = &PokeSeen.AddDefaulted_GetRef();
			Poke->Penguin = Penguin;
			Poke->Serial = Entry.PokeSerial;
		}
		else if (Poke->Serial != Entry.PokeSerial)
		{
			Poke->Serial = Entry.PokeSerial;
			Poke->StartedAt = World->GetTimeSeconds();
			Poke->Dir = Entry.PokeDir >= 0 ? 1.f : -1.f;
			++PokesSeen;
		}
	}
	PokeSeen.RemoveAll([](const FPokeSeen& Item) { return !Item.Penguin.IsValid(); });
	EntriesSeen.RemoveAll([](const TWeakObjectPtr<APenguinCharacter>& Item) { return !Item.IsValid(); });
	PuffingOutSeen.RemoveAll([](const TWeakObjectPtr<APenguinCharacter>& Item) { return !Item.IsValid(); });

	// Hats in the picked teams' colours, the pop in / out.
	for (TActorIterator<APenguinCharacter> It(const_cast<UWorld*>(World)); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (!IsValid(Penguin))
		{
			continue;
		}
		Penguin->SetHatColorIndex(GetLobbyTeam(Penguin));
		Penguin->SetLobbyPopScale(GetPopScale(Penguin));
	}
	UpdateTags();
	UpdateProps();
	UpdatePlus(Dt);
	HandleLocalInput();
}

// ---------------------------------------------------------------------------- poses, gags and props (every machine)

namespace PinguLobbyPrivate
{
	/** How fat the fish made it at flow time At (0..1, a little more while it boings up). */
	static float FatAmount(const FPinguLobbyPenguin& Entry, double At)
	{
		if (At < Entry.FatAt || At >= Entry.FatUntil)
		{
			return 0.f;
		}
		const float Since = static_cast<float>(At - Entry.FatAt);
		const float Up = FMath::Clamp(Since / PinguLobby::FatSwell, 0.f, 1.f);
		const float Down = FMath::Clamp(static_cast<float>(Entry.FatUntil - At) / PinguLobby::FatShrink, 0.f, 1.f);
		// It boings up and wobbles a moment (a jelly belly), then holds.
		const float Wobble = 1.f + 0.12f * FMath::Sin(Since * 2.f * UE_PI * 3.f) * FMath::Exp(-2.5f * Since);
		return FMath::Max(0.f, FMath::Min(EaseOutBack(Up) * Wobble, FMath::SmoothStep(0.f, 1.f, Down)));
	}
}

FVector APinguLobby::GetVisualOffset(const APenguinCharacter* Penguin) const
{
	using namespace PinguLobbyPrivate;
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	if (!Entry || !IsShowing())
	{
		return FVector::ZeroVector;
	}
	switch (static_cast<PinguLobby::EPose>(Entry->Pose))
	{
	case PinguLobby::EPose::Sit: return FVector(0.f, 0.f, -SitLower);
	case PinguLobby::EPose::Hang: return FVector(Entry->Side * (HangCapsuleIn + HangOut), 0.f, -HangDrop);
	default: return FVector::ZeroVector;
	}
}

FVector APinguLobby::GetBodyCentre(const APenguinCharacter* Penguin) const
{
	return Penguin ? Penguin->GetActorLocation() + GetVisualOffset(Penguin) : FVector::ZeroVector;
}

PinguLobby::EPose APinguLobby::GetPose(const APenguinCharacter* Penguin) const
{
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	return Entry ? static_cast<PinguLobby::EPose>(Entry->Pose) : PinguLobby::EPose::Stand;
}

PinguLobby::EGag APinguLobby::GetGag(const APenguinCharacter* Penguin, float& OutAge) const
{
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	OutAge = -1.f;
	if (!Entry || Entry->Gag == 0 || Entry->PuffOutAt >= 0.f)
	{
		return PinguLobby::EGag::None;
	}
	const PinguLobby::EGag Gag = static_cast<PinguLobby::EGag>(FMath::Min<uint8>(Entry->Gag, static_cast<uint8>(PinguLobby::EGag::Count) - 1));
	const float Age = static_cast<float>(PinguLobbyPrivate::Now(GetWorld()) - Entry->GagAt);
	if (Age < 0.f || Age >= PinguLobby::GetGagLength(Gag))
	{
		return PinguLobby::EGag::None;
	}
	OutAge = Age;
	return Gag;
}

float APinguLobby::GetFat(const APenguinCharacter* Penguin) const
{
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	return Entry && IsShowing() ? PinguLobbyPrivate::FatAmount(*Entry, PinguLobbyPrivate::Now(GetWorld())) : 0.f;
}

void APinguLobby::FillAnimState(const APenguinCharacter* Penguin, PinguLobby::FAnimState& State) const
{
	using namespace PinguLobbyPrivate;
	const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
	if (!Entry)
	{
		return;
	}
	const double At = Now(GetWorld());
	State.Pose = static_cast<PinguLobby::EPose>(Entry->Pose);
	State.Side = Entry->Side >= 0 ? 1.f : -1.f;
	State.Offset = GetVisualOffset(Penguin);
	State.Gag = GetGag(Penguin, State.GagAge);
	if (State.Gag == PinguLobby::EGag::NootFriend && IsValid(Entry->GagTarget))
	{
		State.GagDir = Entry->GagTarget->GetActorLocation().X >= Penguin->GetActorLocation().X ? 1.f : -1.f;
	}
	State.Fat = FatAmount(*Entry, At);
	// A neighbour's noot at it: it flinches at each blast (never pushed).
	for (const FPinguLobbyPenguin& Other : Entries)
	{
		if (Other.Gag != static_cast<uint8>(PinguLobby::EGag::NootFriend) || Other.GagTarget != Penguin || !IsValid(Other.Penguin))
		{
			continue;
		}
		for (const float Blast : PinguLobby::NootBlastTimes)
		{
			const float Age = static_cast<float>(At - Other.GagAt - Blast);
			if (Age >= 0.f && Age < 0.6f && (State.FlinchAge < 0.f || Age < State.FlinchAge))
			{
				State.FlinchAge = Age;
				State.FlinchDir = Penguin->GetActorLocation().X >= Other.Penguin->GetActorLocation().X ? 1.f : -1.f;
			}
		}
	}
}

float APinguLobby::GetTagShift(const APenguinCharacter* Penguin) const
{
	const FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
	return View ? View->Shift : 0.f;
}

int32 APinguLobby::GetTagRow(const APenguinCharacter* Penguin) const
{
	const FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
	return View ? View->Row : 0;
}

int32 APinguLobby::CountShownProps() const
{
	int32 Count = 0;
	for (const FPropView& View : PropViews)
	{
		Count += View.Fish && View.Fish->IsVisible() ? 1 : 0;
		Count += View.ZRoot && View.ZRoot->IsVisible() ? 1 : 0;
	}
	return Count;
}

void APinguLobby::HideProps()
{
	for (FPropView& View : PropViews)
	{
		if (View.Fish && View.Fish->IsVisible())
		{
			View.Fish->SetVisibility(false);
		}
		if (View.ZRoot && View.ZRoot->IsVisible())
		{
			View.ZRoot->SetVisibility(false, true);
		}
	}
}

void APinguLobby::UpdateProps()
{
	using namespace PinguLobbyPrivate;
	using PinguLobby::EGag;
	const double At = Now(GetWorld());
	int32 Playing = 0;
	for (const FPinguLobbyPenguin& Entry : Entries)
	{
		const APenguinCharacter* Penguin = Entry.Penguin;
		if (!IsValid(Penguin))
		{
			continue;
		}
		FPropView* View = PropViews.FindByPredicate([Penguin](const FPropView& Item) { return Item.Penguin.Get() == Penguin; });
		if (!View)
		{
			View = PropViews.FindByPredicate([](const FPropView& Item) { return !Item.Penguin.IsValid(); });
			if (!View)
			{
				View = &PropViews.AddDefaulted_GetRef();
			}
			View->Penguin = Penguin;
			View->LastGagAt = -1000.f;
		}
		float Age = -1.f;
		const EGag Gag = GetGag(Penguin, Age);
		if (Entry.Gag != 0 && Entry.GagAt != View->LastGagAt)
		{
			// (Counted while it plays: one long over when this machine first saw the penguin is not.)
			View->LastGagAt = Entry.GagAt;
			GagsSeen += Gag != EGag::None ? 1 : 0;
		}
		Playing += Gag != EGag::None ? 1 : 0;
		const FVector Body = GetBodyCentre(Penguin);
		const float Pop = GetPopScale(Penguin);

		// The fish: held up, wriggling, lifted over the beak and dropped in (gulp).
		const bool bFish = Gag == EGag::Fish && Age < PinguLobby::FishGulpTime + 0.12f;
		if (bFish && !View->Fish)
		{
			View->Fish = NewObject<UStaticMeshComponent>(this);
			View->Fish->SetupAttachment(RootComponent);
			View->Fish->SetUsingAbsoluteLocation(true);
			View->Fish->SetUsingAbsoluteRotation(true);
			View->Fish->SetUsingAbsoluteScale(true);
			View->Fish->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			View->Fish->SetGenerateOverlapEvents(false);
			View->Fish->SetCastShadow(false);
			View->Fish->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Fish.SM_Fish"), nullptr, LOAD_Quiet | LOAD_NoWarn));
			View->Fish->RegisterComponent();
			Parts.Add(View->Fish);
			UE_LOG(LogPingu, Log, TEXT("[LOBBY] fish prop for %s: mesh %s, %s"), *Penguin->GetDisplayName(),
				View->Fish->GetStaticMesh() ? *View->Fish->GetStaticMesh()->GetName() : TEXT("none"),
				View->Fish->GetStaticMesh() ? *View->Fish->GetStaticMesh()->GetBounds().BoxExtent.ToString() : TEXT("-"));
		}
		if (View->Fish)
		{
			if (bFish && View->Fish->GetStaticMesh())
			{
				// Held up in the near flipper, wriggling; tossed up (spinning) at FishTossTime, high over the head, and down into the
				// open beak at FishGulpTime (gone in a blink: swallowed).
				const float Side = Entry.Side >= 0 ? 1.f : -1.f;
				const FVector Held = Body + FVector(Side * 34.f, 42.f, 2.f);
				// (The arc goes up beside the head, to the left of the name tag - its x is on the right - never behind the tags.)
				const FVector Apex = Body + FVector(-118.f, 60.f, 118.f);
				const FVector InBeak = Body + FVector(0.f, 30.f, 34.f);
				const float Toss = PinguLobby::FishTossTime;
				const float ApexAt = Toss + 0.38f;
				const float Catch = PinguLobby::FishGulpTime;
				FVector Where = Held;
				float Size = 1.f;
				float Pitch = 0.f;
				float Roll = 20.f * FMath::Sin(Age * 2.f * UE_PI * 4.f); // it wriggles
				if (Age < 0.3f)
				{
					Size = EaseOutBack(Age / 0.3f);
				}
				else if (Age < Toss)
				{
					// A wind-up just before the toss (down a little).
					const float Wind = FMath::SmoothStep(Toss - 0.22f, Toss, Age);
					Where += FVector(0.f, 0.f, 3.f * FMath::Sin(Age * 2.f * UE_PI * 2.f) - 14.f * Wind);
				}
				else if (Age < ApexAt)
				{
					const float T = (Age - Toss) / (ApexAt - Toss);
					const float Up = 1.f - (1.f - T) * (1.f - T); // fast up, slowing
					Where = FMath::Lerp(Held + FVector(0.f, 0.f, -14.f), Apex, FVector(T, T, Up));
					Pitch = 360.f * 2.f * T;
					Roll = 0.f;
				}
				else if (Age < Catch)
				{
					const float T = (Age - ApexAt) / (Catch - ApexAt);
					Where = FMath::Lerp(Apex, InBeak, FVector(T, T, T * T)); // falling faster and faster
					Pitch = 720.f + 270.f * FMath::SmoothStep(0.f, 1.f, T); // ends head first, down
					Roll = 0.f;
				}
				else
				{
					const float T = FMath::Clamp((Age - Catch) / 0.12f, 0.f, 1.f);
					Where = InBeak - FVector(0.f, 4.f, 10.f * T);
					Pitch = -90.f;
					Roll = 0.f;
					Size = 1.f - T;
				}
				const float MeshLength = FMath::Max(2.f * static_cast<float>(View->Fish->GetStaticMesh()->GetBounds().BoxExtent.X), 1.f);
				View->Fish->SetWorldLocation(Where);
				View->Fish->SetWorldRotation(FRotator(Pitch, Side > 0.f ? 0.f : 180.f, Roll));
				View->Fish->SetWorldScale3D(FVector(FMath::Max(Size * Pop, 0.01f) * FishLength / MeshLength));
				if (!View->Fish->IsVisible())
				{
					View->Fish->SetVisibility(true);
				}
			}
			else if (View->Fish->IsVisible())
			{
				View->Fish->SetVisibility(false);
			}
		}

		// The doze: Zzz floating up from its head until it startles awake.
		const bool bZzz = Gag == EGag::Doze && Age > 0.8f && Age < PinguLobby::DozeStartleTime;
		if (bZzz && !View->ZRoot)
		{
			View->ZRoot = MakeRoot();
			PinguMenu3D::FTextLook Look;
			Look.Size = 32.f;
			Look.Front = FLinearColor(0.85f, 0.93f, 1.f);
			Look.Outline = 0.14f;
			Look.Side = FLinearColor(0.03f, 0.06f, 0.16f);
			Look.Glow = 0.3f;
			Look.bMenuLight = false;
			Look.bOverlay = false;
			Look.Extrude = 0.25f;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				USceneComponent* Holder = NewObject<USceneComponent>(this);
				Holder->SetupAttachment(View->ZRoot);
				Holder->RegisterComponent();
				Parts.Add(Holder);
				if (UText3DComponent* Letter = PinguMenu3D::MakeText(this, Holder, FText::FromString(Index == 0 ? TEXT("z") : TEXT("Z")), Look))
				{
					Parts.Add(Letter);
				}
				View->Z[Index] = Holder;
			}
		}
		if (View->ZRoot)
		{
			if (bZzz)
			{
				View->ZRoot->SetWorldLocation(Body + FVector(-38.f, 200.f, 28.f));
				const float Run = Age - 0.8f;
				for (int32 Index = 0; Index < 3; ++Index)
				{
					// Each rises and drifts off to the side, growing, then shrinks away; one after the other.
					const float Own = Run - 0.5f * Index;
					const float P = FMath::Frac(FMath::Max(Own, 0.f) / 1.5f);
					const float Shown = Own > 0.f ? 1.f : 0.f;
					const float Scale = (0.6f + 0.7f * P) * FMath::Min(1.f, (1.f - P) / 0.25f) * FMath::Min(1.f, P / 0.12f) * Shown * Pop;
					View->Z[Index]->SetRelativeLocation(FVector(0.f, -8.f - 50.f * P + 6.f * FMath::Sin(P * 2.f * UE_PI), 70.f * P));
					View->Z[Index]->SetRelativeScale3D(FVector(FMath::Max(Scale, 0.01f)));
				}
				if (!View->ZRoot->IsVisible())
				{
					View->ZRoot->SetVisibility(true, true);
				}
			}
			else if (View->ZRoot->IsVisible())
			{
				View->ZRoot->SetVisibility(false, true);
			}
		}
	}
	// Props of penguins gone: hidden, handed on to the next newcomer.
	for (FPropView& View : PropViews)
	{
		if (!View.Penguin.IsValid() || !FindEntry(View.Penguin.Get()))
		{
			View.Penguin.Reset();
			if (View.Fish && View.Fish->IsVisible())
			{
				View.Fish->SetVisibility(false);
			}
			if (View.ZRoot && View.ZRoot->IsVisible())
			{
				View.ZRoot->SetVisibility(false, true);
			}
		}
	}
	MostGagsAtOnce = FMath::Max(MostGagsAtOnce, Playing);
}

// ---------------------------------------------------------------------------- parts

USceneComponent* APinguLobby::MakeRoot()
{
	USceneComponent* Root = NewObject<USceneComponent>(this);
	Root->SetupAttachment(RootComponent);
	Root->SetUsingAbsoluteLocation(true);
	Root->SetUsingAbsoluteRotation(true);
	Root->SetUsingAbsoluteScale(true);
	// Faces the stage camera (it looks from +Y toward -Y): the tag's -X toward it, its +Y reading to the right (world +X).
	Root->SetWorldRotation(FRotator(0.f, -90.f, 0.f));
	Root->RegisterComponent();
	Parts.Add(Root);
	return Root;
}

void APinguLobby::BuildPiece(FTagPiece& Piece, USceneComponent* Parent, float TextSize, const FLinearColor& Face, const FLinearColor& Lip, bool bPlate)
{
	using namespace PinguLobbyPrivate;
	Piece.Root = NewObject<USceneComponent>(this);
	Piece.Root->SetupAttachment(Parent);
	Piece.Root->RegisterComponent();
	Parts.Add(Piece.Root);
	if (bPlate && Piece.Slab.Build(this, Piece.Root, false))
	{
		Piece.Slab.SetColors(Face, Lip);
	}
	PinguMenu3D::FTextLook Look;
	Look.Size = TextSize;
	Look.Front = bPlate ? TextColor : Face;
	Look.Side = FLinearColor(0.02f, 0.03f, 0.08f);
	Look.bMenuLight = false;
	Look.Extrude = 0.18f;
	Piece.Text = PinguMenu3D::MakeText(this, Piece.Root, FText::GetEmpty(), Look);
	if (Piece.Text)
	{
		// In front of the slab, its letters standing on a line through the slab's middle minus half their height.
		Piece.Text->SetRelativeLocation(FVector(-Look.Extrude * TextSize - 1.f, 0.f, -0.5f * TextSize));
		Parts.Add(Piece.Text);
	}
}

float APinguLobby::LayPiece(FTagPiece& Piece, const FString& Text, float TextSize, float Height, float MinWidth, bool bRound)
{
	using namespace PinguLobbyPrivate;
	if (Piece.Text && Text != Piece.Shown)
	{
		Piece.Shown = Text;
		PinguMenu3D::SetText(Piece.Text, FText::FromString(Text));
		Piece.Width = 0.f;
	}
	if (Piece.Width <= 0.f && Piece.Text)
	{
		Piece.Width = PinguMenu3D::GetTextWidth(Piece.Text);
	}
	const float Width = bRound ? Height : FMath::Max(Piece.Width + 2.f * PlatePadding, MinWidth);
	if (Piece.Slab.IsBuilt())
	{
		Piece.Slab.SetShape(Width, Height, Height * 0.5f, SlabDepth);
	}
	return Width;
}

namespace PinguLobbyPrivate
{
	static void ShowPiece(USceneComponent* Root, bool bShow)
	{
		if (Root && Root->IsVisible() != bShow)
		{
			Root->SetVisibility(bShow, true);
		}
	}
}

void APinguLobby::UpdateTags()
{
	using namespace PinguLobbyPrivate;
	const UWorld* World = GetWorld();
	APlayerController* Player = nullptr;
	APenguinHUD* HUD = GetLocalHUD(Player);
	const bool bAuthority = HUD && HUD->HasMatchAuthority();
	const bool bCanRemove = HUD && HUD->CanRemoveBot();
	const APenguinGameState* GameState = APenguinGameState::Get(World);
	// The lobby's penguins (their entries: a penguin of a test's own is no member and gets no tag).
	TArray<APenguinCharacter*> Shown;
	for (const FPinguLobbyPenguin& Entry : Entries)
	{
		if (IsValid(Entry.Penguin) && !Entry.Penguin->GetDisplayName().IsEmpty())
		{
			Shown.Add(Entry.Penguin);
		}
	}
	// A view per penguin (made once, handed on to the next penguin when its own goes).
	for (FTagView& View : TagViews)
	{
		if (!View.Penguin.IsValid() || !Shown.Contains(View.Penguin.Get()))
		{
			View.Penguin.Reset();
		}
	}
	for (APenguinCharacter* Penguin : Shown)
	{
		FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
		if (!View)
		{
			View = TagViews.FindByPredicate([](const FTagView& Item) { return !Item.Penguin.IsValid(); });
		}
		if (!View)
		{
			View = &TagViews.AddDefaulted_GetRef();
			View->Root = MakeRoot();
			BuildPiece(View->Name, View->Root, NameSize, PlateColor, PlateLip);
			BuildPiece(View->Skill, View->Root, SkillSize, SkillColors[1], SkillColors[1] * 0.4f);
			// (Only a red X, no disc behind it - 2026-11-07, Linus: "the cross in the lobby to kick has no background"; its word is
			// drawn with the menu's edge and soft shadow.)
			BuildPiece(View->Remove, View->Root, RemoveSize * 0.9f, PlainRemoveColor, PlainRemoveColor * 0.4f, false);
			View->SkillShown = -1;
			View->bFocusShown = false;
		}
		View->Penguin = Penguin;
	}
	for (FTagView& View : TagViews)
	{
		APenguinCharacter* Penguin = View.Penguin.Get();
		View.Shift = 0.f;
		const FPinguLobbyPenguin* Entry = FindEntry(Penguin);
		const bool bLeaving = Entry && Entry->PuffOutAt >= 0.f;
		const float Pop = Penguin ? GetPopScale(Penguin) : 0.f;
		const bool bShow = Penguin && Penguin->IsAlive() && !Penguin->IsHidden() && !bLeaving && Pop > 0.6f;
		ShowPiece(View.Root, bShow);
		if (!bShow)
		{
			View.bSkillShown = false;
			View.bRemoveShown = false;
			View.NameText.Reset();
			continue;
		}
		const bool bBot = !Penguin->GetPlayerState();
		const float Size = FMath::Max(Penguin->GetSizeScale(), 0.3f);
		// Over the head as it is drawn (sitting a little lower; hanging off an edge: lower, out over the edge, the tag further out).
		const FVector At = GetBodyCentre(Penguin);
		const float HeadZ = static_cast<float>(At.Z) + (Penguin->GetCapsuleComponent() ? Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 56.f) + 12.f * Size;
		const float HangShift = Entry && Entry->Pose == static_cast<uint8>(PinguLobby::EPose::Hang) ? (Entry->Side >= 0 ? 1.f : -1.f) * HangTagOut : 0.f;
		View.LaneZ = HeadZ;
		View.Row = 0;
		const FString Name = Penguin->GetDisplayName();
		View.NameText = Name;
		const float NameWidth = LayPiece(View.Name, Name, NameSize, PlateHeight, PlateHeight * 1.6f);
		int32 Skill = -1;
		if (bBot && GameState)
		{
			const FPinguBotSetup* Setup = GameState->GetSettings().Bots.FindByPredicate([&Name](const FPinguBotSetup& Bot) { return Bot.Name == Name; });
			Skill = Setup ? FMath::Clamp(Setup->Skill, 0, 2) : -1;
		}
		View.bSkillShown = Skill >= 0;
		View.bRemoveShown = bBot && bAuthority && bCanRemove;
		// The tag's root sits over the penguin's head (pushed aside below when two tags would overlap); the pieces are laid out
		// in its own plane (Y right, Z up).
		const float X = static_cast<float>(At.X) + HangShift;
		const float SkillZ = TagAboveHead * 0.6f;
		const float NameZ = TagAboveHead + (View.bSkillShown ? SkillPlateHeight + SkillGap : 0.f) + 0.5f * PlateHeight;
		View.Root->SetWorldLocation(FVector(X, TagY, HeadZ));
		View.Name.Root->SetRelativeLocation(FVector(0.f, 0.f, NameZ));
		View.NameCentre = FVector(X, TagY, HeadZ + NameZ);
		View.NameHalf = FVector2D(NameWidth * 0.5f, PlateHeight * 0.5f);
		const bool bFocus = Focused.Get() == Penguin;
		const bool bNameHover = Hovered.Penguin.Get() == Penguin && (Hovered.Kind == EHit::Name || Hovered.Kind == EHit::Body);
		if (View.Name.Slab.IsBuilt())
		{
			View.bFocusShown = bFocus;
			SetPieceHover(View.Name, bNameHover, bFocus ? FocusColor : PlateColor, bFocus ? FocusColor * 0.45f : PlateLip, bFocus ? 0.35f : 0.f);
		}
		// A bot's difficulty (a click changes it: the host / solo player).
		ShowPiece(View.Skill.Root, View.bSkillShown);
		if (View.bSkillShown)
		{
			View.SkillText = SkillName(Skill);
			const float SkillWidth = LayPiece(View.Skill, View.SkillText, SkillSize, SkillPlateHeight, SkillPlateHeight * 2.f);
			if (View.Skill.Slab.IsBuilt())
			{
				View.SkillShown = Skill;
				SetPieceHover(View.Skill, Hovered.Kind == EHit::Skill && Hovered.Penguin.Get() == Penguin, SkillColors[Skill], SkillColors[Skill] * 0.4f);
			}
			View.Skill.Root->SetRelativeLocation(FVector(0.f, 0.f, SkillZ + 0.5f * SkillPlateHeight));
			View.SkillCentre = FVector(X, TagY, HeadZ + SkillZ + 0.5f * SkillPlateHeight);
			View.SkillHalf = FVector2D(SkillWidth * 0.5f, SkillPlateHeight * 0.5f);
		}
		// The x that sends a bot away (the host / solo player, while the match keeps its fewest bots without it): a little
		// round red button at the name tag's right end.
		ShowPiece(View.Remove.Root, View.bRemoveShown);
		if (View.bRemoveShown)
		{
			// A perfectly round disc (as wide as high), a little in front of the name tag, lit up while hovered.
			// (A capital X: its capitals' box is what the tag centres - a small x sat low in its round button, 2026-10-27.)
			LayPiece(View.Remove, TEXT("X"), RemoveSize * 0.9f, RemoveSize, RemoveSize, true);
			const float RightY = View.NameHalf.X + RemoveSize * 0.5f + 3.f;
			const float UpZ = NameZ + PlateHeight * 0.32f;
			View.Remove.Root->SetRelativeLocation(FVector(-4.f, RightY, UpZ));
			View.RemoveCentre = FVector(X + RightY, TagY, HeadZ + UpZ);
			SetPieceHover(View.Remove, Hovered.Kind == EHit::Remove && Hovered.Penguin.Get() == Penguin, PlainRemoveColor, PlainRemoveColor * 0.4f);
		}
	}

	// Close neighbours on the same surface (2026-10-27: the spots are close enough for ten): every second tag a row higher, so
	// no tag is pushed off its penguin. Tags that still overlap (a very long name) are pushed apart, each half the way.
	TArray<FTagView*> Order;
	for (FTagView& View : TagViews)
	{
		if (View.Penguin.IsValid() && !View.NameText.IsEmpty())
		{
			Order.Add(&View);
		}
	}
	// (The lowest heads first, each from the left - BuildSpots packs the spots in this order.)
	Order.Sort([](const FTagView& A, const FTagView& B) { return FMath::Abs(A.LaneZ - B.LaneZ) > 1.f ? A.LaneZ < B.LaneZ : A.NameCentre.X < B.NameCentre.X; });
	const auto Left = [](const FTagView& View) { return static_cast<float>(View.NameCentre.X + View.Shift) - View.NameHalf.X; };
	const auto Right = [](const FTagView& View) { return static_cast<float>(View.NameCentre.X + View.Shift) + View.NameHalf.X + (View.bRemoveShown ? RemoveSize + 3.f : 0.f); };
	const auto Bottom = [](const FTagView& View) { return View.bSkillShown ? static_cast<float>(View.SkillCentre.Z) - View.SkillHalf.Y : static_cast<float>(View.NameCentre.Z) - View.NameHalf.Y; };
	const auto Top = [](const FTagView& View) { return static_cast<float>(View.NameCentre.Z) + View.NameHalf.Y + 10.f; };
	// Left to right: a tag goes a row up when in the low row it would cover a tag placed before it or another penguin's body
	// (the spots are packed the same way: BuildSpots).
	const auto Covers = [&Order, &Left, &Right, &Bottom, &Top, this](const FTagView& This, int32 Count)
	{
		for (int32 Before = 0; Before < Count; ++Before)
		{
			const FTagView& Other = *Order[Before];
			if (Left(This) < Right(Other) + 6.f && Left(Other) < Right(This) + 6.f && Bottom(This) < Top(Other) && Bottom(Other) < Top(This))
			{
				return true;
			}
		}
		for (const FTagView* Other : Order)
		{
			const APenguinCharacter* Body = Other != &This ? Other->Penguin.Get() : nullptr;
			if (!Body)
			{
				continue;
			}
			const FVector Centre = GetBodyCentre(Body);
			if (Left(This) < Centre.X + PenguinHalfWidth && Centre.X - PenguinHalfWidth < Right(This) && Bottom(This) < Centre.Z + 65.f && Centre.Z - PenguinBelow < Top(This))
			{
				return true;
			}
		}
		return false;
	};
	for (int32 Index = 0; Index < Order.Num(); ++Index)
	{
		FTagView& This = *Order[Index];
		if (Covers(This, Index))
		{
			This.Row = 1;
			This.Root->SetWorldLocation(This.Root->GetComponentLocation() + FVector(0.f, 0.f, TagRowLift));
			This.NameCentre.Z += TagRowLift;
			This.SkillCentre.Z += TagRowLift;
			This.RemoveCentre.Z += TagRowLift;
		}
	}
	// The "+" keeps a clear space of its own (2026-10-29, Linus's 2000x1232 lobby: a bot's x nearly touched it and a click on that
	// x could press the "+"): a tag beside it at its height is pushed away from it, as far as its bob reaches plus PlusClearance.
	FVector PlusStep;
	const ABlockIsland* PlusIsland = ABlockIsland::Get(World);
	const bool bPlusZone = bAuthority && HUD->CanAddBot() && PlusIsland && PlusIsland->GetLobbyStepTop(PlusStep);
	const float PlusX = bPlusZone ? static_cast<float>(PlusStep.X) : 0.f;
	const float PlusZ = bPlusZone ? static_cast<float>(PlusStep.Z) + PlusAbove : 0.f;
	const float PlusReach = 0.5f * PlusSize * 1.12f + 6.f + PlusClearance;
	const auto KeepOffPlus = [&]()
	{
		if (!bPlusZone)
		{
			return;
		}
		for (FTagView* View : Order)
		{
			if (Bottom(*View) >= PlusZ + PlusReach || Top(*View) <= PlusZ - PlusReach)
			{
				continue;
			}
			const float LeftEdge = Left(*View);
			const float RightEdge = Right(*View);
			if (RightEdge <= PlusX - PlusReach || LeftEdge >= PlusX + PlusReach)
			{
				continue;
			}
			// To the side its middle is on (most tags stand left of the step).
			if (0.5f * (LeftEdge + RightEdge) <= PlusX)
			{
				View->Shift -= RightEdge - (PlusX - PlusReach);
			}
			else
			{
				View->Shift += (PlusX + PlusReach) - LeftEdge;
			}
		}
	};
	for (int32 Pass = 0; Pass < 40; ++Pass)
	{
		KeepOffPlus();
		for (int32 Index = 1; Index < Order.Num(); ++Index)
		{
			for (int32 Before = 0; Before < Index; ++Before)
			{
				FTagView& A = *Order[Before];
				FTagView& B = *Order[Index];
				if (Bottom(A) >= Top(B) || Bottom(B) >= Top(A))
				{
					continue; // one over the other
				}
				FTagView& L = A.NameCentre.X + A.Shift <= B.NameCentre.X + B.Shift ? A : B;
				FTagView& R = &L == &A ? B : A;
				const float Overlap = Right(L) + 6.f - Left(R);
				if (Overlap > 0.f)
				{
					L.Shift -= Overlap * 0.5f;
					R.Shift += Overlap * 0.5f;
				}
			}
		}
	}
	KeepOffPlus();
	for (FTagView* View : Order)
	{
		if (FMath::Abs(View->Shift) > 0.01f)
		{
			View->Root->SetWorldLocation(View->Root->GetComponentLocation() + FVector(View->Shift, 0.f, 0.f));
			View->NameCentre.X += View->Shift;
			View->SkillCentre.X += View->Shift;
			View->RemoveCentre.X += View->Shift;
		}
	}
}

void APinguLobby::UpdatePlus(float Dt)
{
	using namespace PinguLobbyPrivate;
	APlayerController* Player = nullptr;
	APenguinHUD* HUD = GetLocalHUD(Player);
	const ABlockIsland* Island = ABlockIsland::Get(GetWorld());
	FVector Step;
	const bool bWant = HUD && HUD->HasMatchAuthority() && HUD->CanAddBot() && Island && Island->GetLobbyStepTop(Step);
	if (!PlusRoot)
	{
		PlusRoot = MakeRoot();
		// (2026-11-07, Linus: "Just a big + (no background)": no ball any more, only a big "+" with the menu's edge and soft shadow.)
		BuildPiece(Plus, PlusRoot, PlusTextSize * 2.f, PlainPlusColor, PlusLip, false);
		LayPiece(Plus, TEXT("+"), PlusTextSize * 2.f, PlusSize, PlusSize);
	}
	bPlusShown = bWant;
	ShowPiece(PlusRoot, bWant);
	if (!bWant)
	{
		return;
	}
	PlusTime += Dt;
	PlusPress = FMath::Max(0.f, PlusPress - Dt);
	// Floats over the step, bobbing and rocking a little; it squashes when clicked and glows while picked.
	const float Bob = 6.f * FMath::Sin(PlusTime * 2.4f);
	const float Squash = 1.f - 0.25f * FMath::Sin(FMath::Clamp(PlusPress / 0.25f, 0.f, 1.f) * UE_PI);
	PlusCentre = Step + FVector(0.f, TagY, PlusAbove + Bob);
	PlusRoot->SetWorldLocation(PlusCentre);
	PlusRoot->SetWorldRotation(FRotator(0.f, -90.f, 4.f * FMath::Sin(PlusTime * 1.7f)));
	const bool bPlusHover = Hovered.Kind == EHit::Plus;
	PlusRoot->SetWorldScale3D(FVector(Squash * (bPlusFocused || bPlusHover ? 1.12f : 1.f)));
	if (Plus.Slab.IsBuilt())
	{
		const float Glow = (bPlusFocused || bPlusHover ? 0.55f : 0.12f) + 0.08f * FMath::Sin(PlusTime * 3.1f);
		Plus.Slab.SetColors(PlusColor, PlusLip, Glow);
	}
	else if (Plus.Text)
	{
		PinguMenu3D::SetTextColors(Plus.Text, bPlusFocused || bPlusHover ? PlainPlusHover : PlainPlusColor, PlusLip, 0.f);
	}
}

bool APinguLobby::GetPoke(const APenguinCharacter* Penguin, float& OutAge, float& OutDir) const
{
	const FPokeSeen* Poke = PokeSeen.FindByPredicate([Penguin](const FPokeSeen& Item) { return Item.Penguin.Get() == Penguin; });
	const UWorld* World = GetWorld();
	if (!Poke || !World || Poke->StartedAt < 0.0)
	{
		return false;
	}
	OutAge = static_cast<float>(World->GetTimeSeconds() - Poke->StartedAt);
	OutDir = Poke->Dir;
	return OutAge <= 1.f;
}

bool APinguLobby::GetPlusSpot(FVector& OutCentre) const
{
	OutCentre = PlusCentre;
	return bPlusShown;
}

bool APinguLobby::GetNameTagSpot(const APenguinCharacter* Penguin, FVector& OutCentre) const
{
	const FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
	if (!View || View->NameText.IsEmpty())
	{
		return false;
	}
	OutCentre = View->NameCentre;
	return true;
}

bool APinguLobby::GetSkillTagSpot(const APenguinCharacter* Penguin, FVector& OutCentre) const
{
	const FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
	if (!View || View->NameText.IsEmpty() || !View->bSkillShown)
	{
		return false;
	}
	OutCentre = View->SkillCentre;
	return true;
}

bool APinguLobby::GetRemoveSpot(const APenguinCharacter* Penguin, FVector& OutCentre) const
{
	const FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
	if (!View || View->NameText.IsEmpty() || !View->bRemoveShown)
	{
		return false;
	}
	OutCentre = View->RemoveCentre;
	return true;
}

FString APinguLobby::GetNameTagText(const APenguinCharacter* Penguin) const
{
	const FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
	return View ? View->NameText : FString();
}

FString APinguLobby::GetSkillTagText(const APenguinCharacter* Penguin) const
{
	const FTagView* View = TagViews.FindByPredicate([Penguin](const FTagView& Item) { return Item.Penguin.Get() == Penguin; });
	return View && View->bSkillShown && !View->NameText.IsEmpty() ? View->SkillText : FString();
}

// ---------------------------------------------------------------------------- clicks and keys

TArray<APenguinCharacter*> APinguLobby::GetPickable() const
{
	TArray<APenguinCharacter*> Pickable;
	APlayerController* Player = nullptr;
	APenguinHUD* HUD = GetLocalHUD(Player);
	const bool bAuthority = HUD && HUD->HasMatchAuthority();
	for (const FPinguLobbyPenguin& Entry : Entries)
	{
		APenguinCharacter* Penguin = Entry.Penguin;
		if (!IsValid(Penguin) || Entry.PuffOutAt >= 0.f)
		{
			continue;
		}
		// The host picks anybody's (bots; the other people only for a look); a LAN client only its own.
		const bool bOwn = Player && Player->GetPawn() == Penguin;
		if (bOwn || (bAuthority && !Penguin->GetPlayerState()))
		{
			Pickable.Add(Penguin);
		}
	}
	Pickable.Sort([](const APenguinCharacter& A, const APenguinCharacter& B) { return A.GetActorLocation().X < B.GetActorLocation().X; });
	return Pickable;
}

bool APinguLobby::ScreenToPlanes(APlayerController* Player, const FVector2D& ScreenPosition, FVector& OutTagPoint, FVector& OutBodyPoint) const
{
	FVector Origin;
	FVector Direction;
	if (!Player)
	{
		APlayerController* LocalPlayer = nullptr;
		GetLocalHUD(LocalPlayer);
		Player = LocalPlayer;
	}
	// The player controller's own deprojection: the projection the picture was drawn with (right in PIE at any DPI).
	if (!Player || !Player->DeprojectScreenPositionToWorld(ScreenPosition.X, ScreenPosition.Y, Origin, Direction))
	{
		return false;
	}
	// The tags and the "+" stand in front of the island (TagY), the penguins on the play plane (Y = 0): the ray meets each.
	return PinguLobbyPrivate::RayToPlane(Origin, Direction, PinguLobbyPrivate::TagY, OutTagPoint)
		&& PinguLobbyPrivate::RayToPlane(Origin, Direction, 0.f, OutBodyPoint);
}

bool APinguLobby::ClickAtScreen(APlayerController* Player, const FVector2D& ScreenPosition, bool bRight)
{
	// A 3D menu button there (START, TRAITS, SETTINGS, BACK, the address line): the click is the menu's, never the island's.
	if (APinguMenu3D::HasButtonAt(GetWorld(), ScreenPosition))
	{
		return false;
	}
	FVector TagPoint;
	FVector BodyPoint;
	if (!ScreenToPlanes(Player, ScreenPosition, TagPoint, BodyPoint))
	{
		return false;
	}
	return ClickAtPoints(Player, TagPoint, BodyPoint, bRight);
}

bool APinguLobby::ClickAt(APlayerController* Player, const FVector& WorldPoint, bool bRight)
{
	return ClickAtPoints(Player, WorldPoint, WorldPoint, bRight);
}

APinguLobby::FHit APinguLobby::ProbeAtScreen(APlayerController* Player, const FVector2D& ScreenPosition) const
{
	FHit Hit;
	FVector TagPoint;
	FVector BodyPoint;
	int32 BotIndex = INDEX_NONE;
	if (!APinguMenu3D::HasButtonAt(GetWorld(), ScreenPosition) && ScreenToPlanes(Player, ScreenPosition, TagPoint, BodyPoint))
	{
		Hit = ResolveAt(Player, TagPoint, BodyPoint, false, BotIndex);
	}
	return Hit;
}

void APinguLobby::HoverAtScreen(const FVector2D& ScreenPosition)
{
	Hovered = IsShowing() ? ProbeAtScreen(nullptr, ScreenPosition) : FHit();
}

const TCHAR* APinguLobby::HitName(EHit Kind)
{
	switch (Kind)
	{
	case EHit::Remove: return TEXT("x");
	case EHit::Skill: return TEXT("skill");
	case EHit::Body: return TEXT("body");
	case EHit::Plus: return TEXT("plus");
	case EHit::Name: return TEXT("name");
	default: return TEXT("none");
	}
}

TArray<APinguLobby::FTarget> APinguLobby::GetClickTargets() const
{
	using namespace PinguLobbyPrivate;
	TArray<FTarget> Targets;
	for (const FTagView& View : TagViews)
	{
		const APenguinCharacter* Penguin = View.Penguin.Get();
		if (!Penguin || View.NameText.IsEmpty() || !View.Root || !View.Root->IsVisible())
		{
			continue;
		}
		if (View.bRemoveShown)
		{
			FTarget& T = Targets.AddDefaulted_GetRef();
			T.Kind = EHit::Remove; T.Penguin = Penguin; T.Centre = View.RemoveCentre; T.Half = FVector2D(0.5f * RemoveSize, 0.5f * RemoveSize);
		}
		if (View.bSkillShown)
		{
			FTarget& T = Targets.AddDefaulted_GetRef();
			T.Kind = EHit::Skill; T.Penguin = Penguin; T.Centre = View.SkillCentre; T.Half = View.SkillHalf;
		}
		{
			// The name tag's part not under the x (the x's margin reaches into its right end).
			const float Trim = View.bRemoveShown ? RemoveHitMargin + 4.f : 2.f;
			FTarget& T = Targets.AddDefaulted_GetRef();
			T.Kind = EHit::Name; T.Penguin = Penguin;
			T.Centre = View.NameCentre - FVector(0.5f * Trim, 0.f, 0.f);
			T.Half = FVector2D(View.NameHalf.X - 0.5f * Trim - 1.f, View.NameHalf.Y);
		}
		{
			// The body up to just under its tags (they stand in front and take the clicks there).
			const FVector At = GetBodyCentre(Penguin);
			const float Size = FMath::Max(Penguin->GetSizeScale(), 0.5f);
			const float Capsule = Penguin->GetCapsuleComponent() ? Penguin->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 56.f;
			const float Top = FMath::Min(PenguinAbove * Size, Capsule + 12.f * Size - 3.f);
			FTarget& T = Targets.AddDefaulted_GetRef();
			T.Kind = EHit::Body; T.Penguin = Penguin;
			T.Centre = FVector(At.X, 0.f, At.Z + 0.5f * (Top - PenguinBelow * Size));
			T.Half = FVector2D(PenguinHalfWidth * Size, 0.5f * (Top + PenguinBelow * Size));
		}
	}
	if (bPlusShown)
	{
		FTarget& T = Targets.AddDefaulted_GetRef();
		T.Kind = EHit::Plus; T.Centre = PlusCentre; T.Half = FVector2D(0.5f * PlusSize, 0.5f * PlusSize);
	}
	return Targets;
}

APinguLobby::FHit APinguLobby::ResolveAt(APlayerController* Player, const FVector& TagPoint, const FVector& BodyPoint, bool bRight, int32& OutBotIndex) const
{
	using namespace PinguLobbyPrivate;
	FHit Hit;
	OutBotIndex = INDEX_NONE;
	const APenguinGameState* GameState = APenguinGameState::Get(GetWorld());
	const auto BotIndexOf = [GameState](const APenguinCharacter* Penguin)
	{
		if (!GameState || !Penguin || Penguin->GetPlayerState())
		{
			return int32(INDEX_NONE);
		}
		const FString& Name = Penguin->GetDisplayName();
		return GameState->GetSettings().Bots.IndexOfByPredicate([&Name](const FPinguBotSetup& Bot) { return Bot.Name == Name; });
	};
	const FVector2D P(TagPoint.X, TagPoint.Z);
	const FVector2D B(BodyPoint.X, BodyPoint.Z);
	// Front to back, each with a margin round it (a click just off a thing still counts): a bot's x, the "+" (it floats in front
	// of everything else, even a bot that wandered onto its step), the difficulty tags and the name tags (in front of the
	// penguins), then the penguins themselves.
	// The "+" wins over everything only on its own face; its generous margin ring (up to PlusClickRadius) comes last, so a
	// bot's x or a penguin standing next to it is never stolen by it.
	const float PlusDistance = FVector2D::Distance(P, FVector2D(PlusCentre.X, PlusCentre.Z));
	const bool bPlusAvailable = !bRight && bPlusShown;
	// (A bot's x first since 2026-10-29: it always wins, even over the "+"'s face - the tags keep a clear space round the "+"
	// anyway, UpdateTags.)
	for (const FTagView& View : TagViews)
	{
		const APenguinCharacter* Penguin = View.Penguin.Get();
		if (Penguin && View.bRemoveShown && FVector2D::Distance(P, FVector2D(View.RemoveCentre.X, View.RemoveCentre.Z)) < 0.5f * RemoveSize + RemoveHitMargin)
		{
			Hit.Kind = EHit::Remove;
			Hit.Penguin = Penguin;
			OutBotIndex = BotIndexOf(Penguin);
			return Hit;
		}
	}
	if (bPlusAvailable && PlusDistance < 0.5f * PlusSize + 4.f)
	{
		Hit.Kind = EHit::Plus;
		return Hit;
	}
	for (const FTagView& View : TagViews)
	{
		const APenguinCharacter* Penguin = View.Penguin.Get();
		if (Penguin && View.bSkillShown && FMath::Abs(P.X - View.SkillCentre.X) < View.SkillHalf.X + SkillHitMargin
			&& FMath::Abs(P.Y - View.SkillCentre.Z) < View.SkillHalf.Y + SkillHitMargin)
		{
			Hit.Kind = EHit::Skill;
			Hit.Penguin = Penguin;
			OutBotIndex = BotIndexOf(Penguin);
			return Hit;
		}
	}
	for (const FTagView& View : TagViews)
	{
		const APenguinCharacter* Penguin = View.Penguin.Get();
		if (Penguin && !View.NameText.IsEmpty() && FMath::Abs(P.X - View.NameCentre.X) < View.NameHalf.X + NameHitMargin
			&& FMath::Abs(P.Y - View.NameCentre.Z) < View.NameHalf.Y + NameHitMargin)
		{
			Hit.Kind = EHit::Name;
			Hit.Penguin = Penguin;
			OutBotIndex = BotIndexOf(Penguin);
			return Hit;
		}
	}
	const APenguinCharacter* Body = nullptr;
	float Best = TNumericLimits<float>::Max();
	for (const FPinguLobbyPenguin& Entry : Entries)
	{
		const APenguinCharacter* Penguin = Entry.Penguin;
		if (!IsValid(Penguin) || Entry.PuffOutAt >= 0.f || !Penguin->IsAlive())
		{
			continue;
		}
		const FVector At = GetBodyCentre(Penguin);
		const float Size = FMath::Max(Penguin->GetSizeScale(), 0.5f);
		const bool bBody = FMath::Abs(B.X - At.X) < PenguinHalfWidth * Size + BodyHitMargin && B.Y > At.Z - PenguinBelow * Size - BodyHitMargin
			&& B.Y < At.Z + PenguinAbove * Size + BodyHitMargin;
		if (bBody && FMath::Abs(static_cast<float>(B.X - At.X)) < Best)
		{
			Best = FMath::Abs(static_cast<float>(B.X - At.X));
			Body = Penguin;
		}
	}
	if (Body)
	{
		Hit.Kind = EHit::Body;
		Hit.Penguin = Body;
		OutBotIndex = BotIndexOf(Body);
		return Hit;
	}
	if (bPlusAvailable && PlusDistance < PlusClickRadius)
	{
		Hit.Kind = EHit::Plus;
	}
	return Hit;
}

bool APinguLobby::ClickAtPoints(APlayerController* Player, const FVector& TagPoint, const FVector& BodyPoint, bool bRight)
{
	using namespace PinguLobbyPrivate;
	APlayerController* LocalPlayer = nullptr;
	APenguinHUD* HUD = GetLocalHUD(LocalPlayer);
	if (!HUD || !IsShowing())
	{
		return false;
	}
	Player = Player ? Player : LocalPlayer;
	const bool bAuthority = HUD->HasMatchAuthority();
	int32 Index = INDEX_NONE;
	const FHit Hit = ResolveAt(Player, TagPoint, BodyPoint, bRight, Index);
	if (bClickProbe)
	{
		LastProbe = Hit;
		return Hit.Kind != EHit::None;
	}
	const APenguinCharacter* Penguin = Hit.Penguin.Get();
	const int32 Delta = bRight ? -1 : 1;
	switch (Hit.Kind)
	{
	case EHit::Remove:
		if (Index != INDEX_NONE && bAuthority && Penguin)
		{
			UE_LOG(LogPingu, Display, TEXT("[LOBBY] click: x on %s"), *Penguin->GetDisplayName());
			HUD->PlayUISound(true);
			HUD->Action_RemoveBot(Index);
			return true;
		}
		return false;
	case EHit::Skill:
		if (Index != INDEX_NONE && bAuthority && Penguin)
		{
			UE_LOG(LogPingu, Display, TEXT("[LOBBY] click: difficulty of %s %s"), *Penguin->GetDisplayName(), bRight ? TEXT("back") : TEXT("on"));
			HUD->PlayUISound(true);
			HUD->Action_ChangeBotSkill(Index, Delta);
			return true;
		}
		return false;
	case EHit::Plus:
		UE_LOG(LogPingu, Display, TEXT("[LOBBY] click: +"));
		PlusPress = 0.25f;
		HUD->PlayUISound(true);
		HUD->Action_AddBot();
		return true;
	case EHit::Body:
	case EHit::Name:
		// A penguin (or its name tag): its team, forward with the left button, back with the right one.
		if (Player && Penguin && Player->GetPawn() == Penguin)
		{
			UE_LOG(LogPingu, Display, TEXT("[LOBBY] click: own penguin's team %s"), bRight ? TEXT("back") : TEXT("on"));
			HUD->Action_ChangeTeamChoice(Delta, true);
			return true;
		}
		if (Index != INDEX_NONE && bAuthority && Penguin)
		{
			UE_LOG(LogPingu, Display, TEXT("[LOBBY] click: %s's team %s"), *Penguin->GetDisplayName(), bRight ? TEXT("back") : TEXT("on"));
			HUD->Action_ChangeBotTeam(Index, Delta);
			return true;
		}
		return false; // somebody else's penguin (a LAN client cannot change it)
	default:
		return false;
	}
}

void APinguLobby::SetPieceHover(FTagPiece& Piece, bool bHover, const FLinearColor& Face, const FLinearColor& Lip, float BaseGlow)
{
	if (Piece.Slab.IsBuilt())
	{
		Piece.Slab.SetColors(bHover ? FMath::Lerp(Face, FLinearColor::White, 0.18f) : Face, Lip, bHover ? FMath::Max(BaseGlow, 0.3f) : BaseGlow);
	}
	else if (Piece.Text)
	{
		// (No disc: the word itself lights up.)
		PinguMenu3D::SetTextColors(Piece.Text, bHover ? FMath::Lerp(Face, FLinearColor::White, 0.4f) : Face, Lip, 0.f);
	}
	if (Piece.Root)
	{
		Piece.Root->SetRelativeScale3D(FVector(bHover ? 1.12f : 1.f));
	}
}

void APinguLobby::HandleLocalInput()
{
	APlayerController* Player = nullptr;
	APenguinHUD* HUD = GetLocalHUD(Player);
	if (!Player || !HUD || HUD->IsMenuOpen())
	{
		return;
	}
	// The clicks come through the lobby screen's Slate layer (SPinguMenu3DInput, bLobbyIsland: it takes them all). One that
	// reached the game's viewport instead (the layer was not there: never in a game) still counts here.
	if (Player->WasInputKeyJustPressed(EKeys::LeftMouseButton) || Player->WasInputKeyJustPressed(EKeys::RightMouseButton))
	{
		const bool bRight = Player->WasInputKeyJustPressed(EKeys::RightMouseButton) && !Player->WasInputKeyJustPressed(EKeys::LeftMouseButton);
		float MouseX = 0.f;
		float MouseY = 0.f;
		if (Player->GetMousePosition(MouseX, MouseY))
		{
			UE_LOG(LogPingu, Log, TEXT("[LOBBY] a click reached the viewport, not the island's layer (%.0f, %.0f)"), MouseX, MouseY);
			ClickAtScreen(Player, FVector2D(MouseX, MouseY), bRight);
		}
	}
	// The keyboard / gamepad pick: Tab (d-pad) walks over the penguins one can change and the "+".
	const bool bNext = Player->WasInputKeyJustPressed(EKeys::Tab) && !Player->IsInputKeyDown(EKeys::LeftShift) && !Player->IsInputKeyDown(EKeys::RightShift);
	const bool bPrev = Player->WasInputKeyJustPressed(EKeys::Tab) && !bNext;
	const bool bPadNext = Player->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Right);
	const bool bPadPrev = Player->WasInputKeyJustPressed(EKeys::Gamepad_DPad_Left);
	TArray<APenguinCharacter*> Pickable = GetPickable();
	if (bNext || bPrev || bPadNext || bPadPrev)
	{
		const int32 Count = Pickable.Num() + (bPlusShown ? 1 : 0);
		if (Count > 0)
		{
			int32 Current = bPlusFocused ? Pickable.Num() : Pickable.IndexOfByKey(Focused.Get());
			const int32 Step = (bNext || bPadNext) ? 1 : -1;
			Current = Current == INDEX_NONE ? (Step > 0 ? 0 : Count - 1) : (Current + Step + Count) % Count;
			bPlusFocused = bPlusShown && Current == Pickable.Num();
			Focused = bPlusFocused ? nullptr : Pickable[Current];
			HUD->PlayUISound(false);
		}
	}
	if (Focused.IsValid() && !Pickable.Contains(Focused.Get()))
	{
		Focused.Reset();
	}
	bPlusFocused = bPlusFocused && bPlusShown;
	const bool bConfirm = Player->WasInputKeyJustPressed(EKeys::Enter) || Player->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top);
	const bool bBack = Player->WasInputKeyJustPressed(EKeys::BackSpace) || Player->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Left);
	if (bPlusFocused && bConfirm)
	{
		ClickAt(Player, PlusCentre, false);
	}
	else if (const APenguinCharacter* Picked = Focused.Get())
	{
		if (bConfirm || bBack)
		{
			ClickAt(Player, GetBodyCentre(Picked), bBack);
		}
		else if (Player->WasInputKeyJustPressed(EKeys::Delete))
		{
			FVector Spot;
			if (GetRemoveSpot(Picked, Spot))
			{
				ClickAt(Player, Spot, false);
			}
		}
	}
}

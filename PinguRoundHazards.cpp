// Pingu - the round hazards (see PinguRoundHazards.h).

#include "PinguRoundHazards.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinProjectile.h"
#include "PenguinTraitComponent.h"
#include "PinguBuildLimit.h"
#include "PinguFX.h"
#include "PinguSettings.h"
#include "PinguUI.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Net/UnrealNetwork.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"

namespace PinguRoundHazardsPrivate
{
	/** Every machine draws at most this many rain rings at once (more drops than that never show at the same time). */
	constexpr int32 MaxRings = 8;
	static const TCHAR* RingMaterialPath = TEXT("/Game/FX/M_BlastZone.M_BlastZone");
	/** The rings: a strong ice blue (white would vanish against the snow and the sky) with a softer blue disc inside; the ring's
	 *  clock fills toward the landing. */
	const FLinearColor RingColor(0.25f, 0.62f, 1.3f);
	const FLinearColor RingCore(0.12f, 0.4f, 1.f);
	constexpr float RingInner = 0.55f;
	constexpr float RingWidthCm = 13.f;
	/** Wind streaks per second at full effects (the effects density scales them): in the warning, in the gust. */
	constexpr float StreaksWarning = 10.f;
	constexpr float StreaksGust = 34.f;
	/** In front of the island's blocks (the camera looks along -Y; the blocks reach 150 cm toward it). */
	constexpr float StreakY = 220.f;
	/** The arrow: its middle at this share of the screen height from the top, this long (share of the screen height). */
	constexpr float ArrowTop = 0.16f;
	constexpr float ArrowLength = 0.075f;

	static float SmoothAlpha(float A)
	{
		A = FMath::Clamp(A, 0.f, 1.f);
		return A * A * (3.f - 2.f * A);
	}
}

bool PinguRoundHazards::IsHazard(EPinguTrait Trait)
{
	return Trait == EPinguTrait::RisingSea || Trait == EPinguTrait::CrumblingIsland || Trait == EPinguTrait::Wind || Trait == EPinguTrait::SnowballRain;
}

// ============================================================================ the wind arrow (this machine's screen)

/** A small clean arrow at the top of the screen pointing where the wind will blow: snow white with the HUD's ink outline. */
class SPinguWindArrow : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguWindArrow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, APinguRoundHazards* InHazards)
	{
		Hazards = InHazards;
		SetVisibility(EVisibility::HitTestInvisible);
	}

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
	{
		return FVector2D::ZeroVector; // fills the viewport
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		using namespace PinguRoundHazardsPrivate;
		const APinguRoundHazards* H = Hazards.Get();
		const float Opacity = H ? H->GetArrowOpacity() : 0.f;
		const int32 Dir = H ? H->GetGustDir() : 0;
		if (Opacity <= 0.01f || Dir == 0)
		{
			return LayerId;
		}
		const FPinguStyle& Style = FPinguStyle::Get();
		const FVector2f Size = AllottedGeometry.GetLocalSize();
		const float S = FMath::Max(Size.Y * ArrowLength, 20.f);
		// In the warning it nudges toward where the wind will blow; in the gust it rests.
		const float Bob = H->IsWindWarning() ? FMath::Max(0.f, FMath::Sin(H->GetNow() * 2.f * UE_PI * 2.5f)) * 0.1f * S : 0.f;
		const FVector2f Mid(Size.X * 0.5f + Dir * Bob, Size.Y * ArrowTop);
		const FLinearColor Fill = PinguUI::WithAlpha(Style.Snow, Opacity);
		const FLinearColor Line = PinguUI::WithAlpha(Style.Ink, 0.85f * Opacity);
		const float O = 0.045f * S; // the outline

		// Arrow-space (pointing right, units of S) to the screen.
		auto P = [&](float X, float Y) { return Mid + FVector2f(Dir * X * S, Y * S); };
		auto Bar = [&](float X0, float X1, float Y, float Half, float Grow, const FLinearColor& Color, int32 Layer)
		{
			const float L = FMath::Min(P(X0, 0.f).X, P(X1, 0.f).X) - Grow;
			const float R = FMath::Max(P(X0, 0.f).X, P(X1, 0.f).X) + Grow;
			const float T = Mid.Y + (Y - Half) * S - Grow;
			const float B = Mid.Y + (Y + Half) * S + Grow;
			RoundBrush = FSlateRoundedBoxBrush(FLinearColor::White, 0.5f * (B - T));
			FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(FVector2f(R - L, B - T), FSlateLayoutTransform(FVector2f(L, T))),
				&RoundBrush, ESlateDrawEffect::None, Color);
		};
		const float PixelsPerUnit = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
		const float Feather = 1.2f / FMath::Max(PixelsPerUnit, 0.01f);
		auto Head = [&](float Grow, const FLinearColor& Color, int32 Layer)
		{
			Verts.Reset();
			Indices.Reset();
			const float G = Grow / S;
			PinguUI::AddTriangle(Verts, Indices, AllottedGeometry.GetAccumulatedRenderTransform(), P(0.5f + 1.8f * G, 0.f), P(0.04f - G, -0.28f - 1.5f * G),
				P(0.04f - G, 0.28f + 1.5f * G), Color, Feather);
			FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, Style.GetWhiteHandle(), Verts, Indices, nullptr, 0, 0, ESlateDrawEffect::None,
				ESlateBatchDrawFlag::None);
		};
		// The ink first (a little bigger), the snow over it: the shaft, the head and two speed lines behind it.
		Bar(-0.42f, 0.12f, 0.f, 0.09f, O, Line, LayerId);
		Bar(-0.95f, -0.62f, -0.2f, 0.045f, O, Line, LayerId);
		Bar(-0.88f, -0.56f, 0.2f, 0.045f, O, Line, LayerId);
		Head(O, Line, LayerId);
		Bar(-0.42f, 0.12f, 0.f, 0.09f, 0.f, Fill, LayerId + 1);
		Bar(-0.95f, -0.62f, -0.2f, 0.045f, 0.f, Fill, LayerId + 1);
		Bar(-0.88f, -0.56f, 0.2f, 0.045f, 0.f, Fill, LayerId + 1);
		Head(0.f, Fill, LayerId + 2);
		return LayerId + 2;
	}

private:
	TWeakObjectPtr<APinguRoundHazards> Hazards;
	mutable FSlateRoundedBoxBrush RoundBrush = FSlateRoundedBoxBrush(FLinearColor::White, 4.f);
	mutable TArray<FSlateVertex> Verts;
	mutable TArray<SlateIndex> Indices;
};

// ============================================================================ the actor

APinguRoundHazards::APinguRoundHazards()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.f);
	SetMinNetUpdateFrequency(2.f);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void APinguRoundHazards::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguRoundHazards, Hazard);
	DOREPLIFETIME(APinguRoundHazards, StartTime);
	DOREPLIFETIME(APinguRoundHazards, StopTime);
	DOREPLIFETIME(APinguRoundHazards, SeaTop);
	DOREPLIFETIME(APinguRoundHazards, GustAt);
	DOREPLIFETIME(APinguRoundHazards, GustDir);
	DOREPLIFETIME(APinguRoundHazards, GustSerial);
	DOREPLIFETIME(APinguRoundHazards, Crumbles);
	DOREPLIFETIME(APinguRoundHazards, CrumbleLeftX);
	DOREPLIFETIME(APinguRoundHazards, CrumbleRightX);
	DOREPLIFETIME(APinguRoundHazards, RainDrops);
	DOREPLIFETIME(APinguRoundHazards, RainSerial);
}

APinguRoundHazards* APinguRoundHazards::Get(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<APinguRoundHazards> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

void APinguRoundHazards::ClearAll(UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	for (TActorIterator<APinguRoundHazards> It(World); It; ++It)
	{
		It->Destroy();
	}
}

void APinguRoundHazards::SetupForRound(UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	ClearAll(World);
	// The round's trait (Traits mode deals everybody the same one): the first hazard trait anybody has.
	EPinguTrait Trait = EPinguTrait::None;
	for (TActorIterator<APenguinCharacter> It(World); It && Trait == EPinguTrait::None; ++It)
	{
		const UPenguinTraitComponent* Traits = IsValid(*It) ? It->GetTraits() : nullptr;
		if (Traits && PinguRoundHazards::IsHazard(Traits->GetTrait()))
		{
			Trait = Traits->GetTrait();
		}
	}
	const APenguinGameState* RoundState = APenguinGameState::Get(World);
	const EPinguTrait Forced = GetForcedHazard(RoundState ? RoundState->GetRoundNumber() : 1);
	if (Forced != EPinguTrait::None && RoundState && RoundState->GetPhase() != EPinguPhase::Lobby && RoundState->GetPhase() != EPinguPhase::Menu)
	{
		Trait = Forced;
	}
	if (Trait == EPinguTrait::None)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguRoundHazards* Hazards = World->SpawnActor<APinguRoundHazards>(APinguRoundHazards::StaticClass(), FTransform::Identity, Params);
	if (Hazards)
	{
		Hazards->Hazard = Trait;
		const ABlockIsland* Island = ABlockIsland::Get(World);
		Hazards->Rand.Initialize((Island ? Island->Seed : 1) ^ 0x48415A);
		const APenguinGameState* PenguinState = APenguinGameState::Get(World);
		UE_LOG(LogPingu, Display, TEXT("[HAZARD] round %d: %s (from the GO)"), PenguinState ? PenguinState->GetRoundNumber() : 0, *PinguTraits::GetLogName(Trait));
	}
}

EPinguTrait APinguRoundHazards::GetForcedHazard(int32 Round)
{
	if (!UPinguSettings::IsAutomatedRun())
	{
		return EPinguTrait::None;
	}
	FString List;
	if (!FParse::Value(FCommandLine::Get(), TEXT("PinguHazard="), List))
	{
		List = FPlatformMisc::GetEnvironmentVariable(TEXT("PINGU_HAZARD"));
	}
	TArray<EPinguTrait> Hazards;
	for (const EPinguTrait Trait : PinguTraits::ParseList(List))
	{
		if (PinguRoundHazards::IsHazard(Trait))
		{
			Hazards.Add(Trait);
		}
	}
	return Hazards.Num() > 0 ? Hazards[FMath::Max(Round - 1, 0) % Hazards.Num()] : EPinguTrait::None;
}

APinguRoundHazards* APinguRoundHazards::StartForTest(UWorld* World, EPinguTrait Trait, float Ago)
{
	if (!World || World->GetNetMode() == NM_Client || !PinguRoundHazards::IsHazard(Trait))
	{
		return nullptr;
	}
	ClearAll(World);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguRoundHazards* Hazards = World->SpawnActor<APinguRoundHazards>(APinguRoundHazards::StaticClass(), FTransform::Identity, Params);
	if (Hazards)
	{
		Hazards->Hazard = Trait;
		Hazards->bTestRun = true;
		const ABlockIsland* Island = ABlockIsland::Get(World);
		Hazards->Rand.Initialize((Island ? Island->Seed : 1) ^ 0x48415A);
		Hazards->StartHazard(Hazards->GetNow() - FMath::Max(Ago, 0.f));
	}
	return Hazards;
}

ABlockIsland* APinguRoundHazards::GetIsland() const
{
	return ABlockIsland::Get(GetWorld());
}

float APinguRoundHazards::GetNow() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.f;
	}
	if (World->GetNetMode() != NM_Standalone)
	{
		if (const AGameStateBase* State = World->GetGameState())
		{
			return static_cast<float>(State->GetServerWorldTimeSeconds());
		}
	}
	return static_cast<float>(World->GetTimeSeconds());
}

float APinguRoundHazards::GetHazardTime() const
{
	if (!HasStarted())
	{
		return 0.f;
	}
	const float End = HasStopped() ? StopTime : GetNow();
	return FMath::Max(End - StartTime, 0.f);
}

void APinguRoundHazards::BeginPlay()
{
	Super::BeginPlay();
	SetActorTickEnabled(true);
	bNetTestLog = FString(FCommandLine::Get()).Contains(TEXT("-PinguNetTest"));
}

void APinguRoundHazards::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RemoveArrow();
	if (bSeaApplied)
	{
		if (ABlockIsland* Island = GetIsland())
		{
			Island->SetSeaRise(0.f); // the sea back where the island made it
		}
		bSeaApplied = false;
	}
	Super::EndPlay(EndPlayReason);
}

void APinguRoundHazards::StartHazard(float Now)
{
	StartTime = Now;
	StopTime = NotYet;
	ABlockIsland* Island = GetIsland();
	CellSize = Island ? Island->CellSize : 100.f;
	FBox Base(ForceInit);
	float GroundMinZ = 0.f;
	float GroundMaxZ = 0.f;
	const bool bBase = Island && Island->GetBaseFrame(Base, GroundMinZ, GroundMaxZ);
	if (!bBase && Island)
	{
		Base = Island->GetBounds();
		GroundMinZ = static_cast<float>(Base.Max.Z) - 300.f;
	}
	switch (Hazard)
	{
	case EPinguTrait::RisingSea:
		if (Island)
		{
			const float SeaAtStart = Island->SeaLevel - Island->GetSeaRise();
			SeaTop = FMath::Max(0.f, GroundMinZ - PinguHazardTuning::SeaTopBelowGround - SeaAtStart);
		}
		break;
	case EPinguTrait::CrumblingIsland:
		if (Base.IsValid)
		{
			CrumbleLeftX = static_cast<float>(Base.Min.X) + 0.5f * CellSize;
			CrumbleRightX = static_cast<float>(Base.Max.X) - 0.5f * CellSize;
		}
		NextCrumbleAt = StartTime + PinguHazardTuning::CrumbleEvery;
		break;
	case EPinguTrait::Wind:
		NextGustAt = StartTime + PinguHazardTuning::WindFirstAfter;
		break;
	case EPinguTrait::SnowballRain:
		NextRainAt = StartTime + PinguHazardTuning::RainFirstAfter;
		break;
	default:
		break;
	}
	UE_LOG(LogPingu, Display, TEXT("[HAZARD] %s starts (sea top +%.0f cm, crumble columns %.0f .. %.0f)"), *PinguTraits::GetLogName(Hazard), SeaTop,
		CrumbleLeftX, CrumbleRightX);
	ForceNetUpdate();
}

void APinguRoundHazards::StopNow()
{
	if (!HasAuthority() || HasStopped())
	{
		return;
	}
	const float Now = GetNow();
	if (!HasStarted())
	{
		StartTime = Now;
	}
	StopTime = Now;
	UE_LOG(LogPingu, Display, TEXT("[HAZARD] %s stops after %.1f s (gusts %d, crumbles %d, drops %d, sea +%.0f cm)"), *PinguTraits::GetLogName(Hazard),
		StopTime - StartTime, GustSerial, Crumbles, RainSerial, GetSeaRiseNow());
	ForceNetUpdate();
}

void APinguRoundHazards::ForceNextDrop(float X)
{
	bForcedDrop = true;
	ForcedDropX = X;
	NextRainAt = 0.f;
}

void APinguRoundHazards::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Now = GetNow();
	if (HasAuthority())
	{
		ServerTick(Now);
		if (!IsValid(this) || IsActorBeingDestroyed())
		{
			return;
		}
	}
	ApplySea();
	ApplyWind(DeltaSeconds);
	if (GetNetMode() != NM_DedicatedServer)
	{
		UpdateRings();
		UpdateStreaks(DeltaSeconds);
		UpdateArrow();
	}
	if (bNetTestLog)
	{
		UpdateNetTestLog();
	}
}

void APinguRoundHazards::UpdateNetTestLog()
{
	const APenguinGameState* PenguinState = APenguinGameState::Get(this);
	const int32 Round = PenguinState ? PenguinState->GetRoundNumber() : 0;
	const ABlockIsland* Island = GetIsland();
	// What every machine must see the same (the host's decisions).
	while (LoggedGusts < GustSerial)
	{
		++LoggedGusts;
		UE_LOG(LogPingu, Display, TEXT("[NETTEST] round %d hazard gust %d: %s"), Round, LoggedGusts,
			LoggedGusts == GustSerial ? (GustDir > 0 ? TEXT("right") : TEXT("left")) : TEXT("(missed)"));
	}
	if (LoggedCrumbles < Crumbles)
	{
		LoggedCrumbles = Crumbles;
		UE_LOG(LogPingu, Display, TEXT("[NETTEST] round %d hazard crumble %d: columns %.0f .. %.0f stay"), Round, Crumbles, CrumbleLeftX, CrumbleRightX);
	}
	for (const FPinguRainDrop& Drop : RainDrops)
	{
		if (Drop.Serial > LoggedDrops)
		{
			LoggedDrops = Drop.Serial;
			UE_LOG(LogPingu, Display, TEXT("[NETTEST] round %d hazard drop %d at x %.0f"), Round, Drop.Serial, Drop.X);
		}
	}
	// This machine's own looks of it.
	SeenArrowMax = FMath::Max(SeenArrowMax, ArrowOpacity);
	SeenWindMoved = FMath::Max(SeenWindMoved, WindMoved);
	SeenRingsMax = FMath::Max(SeenRingsMax, ShownRings);
	if (Island && Crumbles > 0)
	{
		bSeenShake |= Island->IsCrumblingOver(CrumbleLeftX - 1.5f * CellSize, CrumbleLeftX - 0.5f * CellSize);
	}
	if (!HasStopped() || bLoggedStop)
	{
		return;
	}
	bLoggedStop = true;
	const float Rise = GetSeaRiseNow();
	UE_LOG(LogPingu, Display, TEXT("[NETTEST] round %d hazard %s stopped: sea +%.0f cm, gusts %d, crumbles %d, drops %d"), Round,
		*PinguTraits::GetLogName(Hazard), Rise, GustSerial, Crumbles, RainSerial);
	bool bOk = true;
	FString What;
	switch (Hazard)
	{
	case EPinguTrait::RisingSea:
		bOk = Island && FMath::Abs(Island->GetSeaRise() - Rise) < 1.f && Rise > 50.f;
		What = FString::Printf(TEXT("its sea +%.0f (the hazard's +%.0f)"), Island ? Island->GetSeaRise() : -1.f, Rise);
		break;
	case EPinguTrait::Wind:
		bOk = GustSerial == 0 || (SeenArrowMax > 0.9f && SeenWindMoved >= 1);
		What = FString::Printf(TEXT("gusts %d, arrow %.2f, own penguins moved %d"), GustSerial, SeenArrowMax, SeenWindMoved);
		break;
	case EPinguTrait::CrumblingIsland:
		bOk = Crumbles == 0 || bSeenShake;
		What = FString::Printf(TEXT("crumbles %d, shaking seen %d"), Crumbles, bSeenShake ? 1 : 0);
		break;
	case EPinguTrait::SnowballRain:
		bOk = RainSerial < 2 || SeenRingsMax >= 1;
		What = FString::Printf(TEXT("drops %d, rings at most %d"), RainSerial, SeenRingsMax);
		break;
	default:
		break;
	}
	UE_LOG(LogPingu, Display, TEXT("[NETTEST] RESULT %s hazard %s round %d: %s %s"), HasAuthority() ? TEXT("host") : TEXT("client"),
		*PinguTraits::GetLogName(Hazard), Round, *What, bOk ? TEXT("ok") : TEXT("FAIL"));
}

void APinguRoundHazards::ServerTick(float Now)
{
	if (!bTestRun)
	{
		// The round's flow: from the GO to the round's end. Out of the rounds (lobby, menu, practice) it goes.
		const APenguinGameState* PenguinState = APenguinGameState::Get(this);
		const EPinguPhase Phase = PenguinState ? PenguinState->GetPhase() : EPinguPhase::Fight;
		if (Phase == EPinguPhase::Lobby || Phase == EPinguPhase::Menu || Phase == EPinguPhase::Practice)
		{
			Destroy();
			return;
		}
		if (!HasStarted() && Phase == EPinguPhase::Fight)
		{
			StartHazard(Now);
		}
		else if (HasStarted() && !HasStopped() && Phase != EPinguPhase::Fight)
		{
			StopNow();
		}
	}
	if (!IsRunning())
	{
		return;
	}
	switch (Hazard)
	{
	case EPinguTrait::CrumblingIsland: TickCrumble(Now); break;
	case EPinguTrait::Wind: TickWind(Now); break;
	case EPinguTrait::SnowballRain: TickRain(Now); break;
	default: break;
	}
}

// ---------------------------------------------------------------------------- Rising Sea

float APinguRoundHazards::GetSeaRiseNow() const
{
	if (Hazard != EPinguTrait::RisingSea || !HasStarted() || SeaTop <= 0.f)
	{
		return 0.f;
	}
	const float A = FMath::Clamp(GetHazardTime() / PinguHazardTuning::SeaRiseTime, 0.f, 1.f);
	return SeaTop * (1.f - FMath::Square(1.f - A));
}

void APinguRoundHazards::ApplySea()
{
	if (Hazard != EPinguTrait::RisingSea)
	{
		return;
	}
	if (ABlockIsland* Island = GetIsland())
	{
		Island->SetSeaRise(GetSeaRiseNow());
		bSeaApplied = true;
	}
}

// ---------------------------------------------------------------------------- Crumbling Island

void APinguRoundHazards::TickCrumble(float Now)
{
	using namespace PinguHazardTuning;
	ABlockIsland* Island = GetIsland();
	if (!Island || Now < NextCrumbleAt)
	{
		return;
	}
	if (Island->IsCollapsing())
	{
		NextCrumbleAt = TNumericLimits<float>::Max(); // sudden death crumbles it all from here
		return;
	}
	// The outermost columns that still stand (blocks gone at the ends already: the next ones in).
	const FBox Bounds = Island->GetBounds();
	if (Bounds.IsValid)
	{
		CrumbleLeftX = FMath::Max(CrumbleLeftX, static_cast<float>(Bounds.Min.X) + 0.5f * CellSize);
		CrumbleRightX = FMath::Min(CrumbleRightX, static_cast<float>(Bounds.Max.X) - 0.5f * CellSize);
	}
	const int32 Columns = FMath::RoundToInt((CrumbleRightX - CrumbleLeftX) / CellSize) + 1;
	if (Columns - 2 < CrumbleMinColumns)
	{
		NextCrumbleAt = TNumericLimits<float>::Max(); // never narrower
		UE_LOG(LogPingu, Display, TEXT("[HAZARD] crumbling stops: %d columns left"), Columns);
		return;
	}
	NextCrumbleAt += CrumbleEvery;
	const int32 Started = Island->CrumbleOutside(CrumbleLeftX + 0.5f * CellSize, CrumbleRightX - 0.5f * CellSize, CrumbleWarn);
	CrumbleLeftX += CellSize;
	CrumbleRightX -= CellSize;
	++Crumbles;
	{
		FPinguFXBroadcastScope Broadcast(this);
		APinguFX::Sound(this, EPinguSound::Crumble, 0.8f, 0.05f, 0.85f);
		APinguFX::Shake(this, 0.12f);
	}
	UE_LOG(LogPingu, Display, TEXT("[HAZARD] crumble %d: %d blocks shake, %d columns stay"), Crumbles, Started, Columns - 2);
	ForceNetUpdate();
}

// ---------------------------------------------------------------------------- Wind

void APinguRoundHazards::TickWind(float Now)
{
	if (Now < NextGustAt)
	{
		return;
	}
	GustAt = Now;
	GustDir = Rand.FRand() < 0.5f ? -1 : 1;
	++GustSerial;
	NextGustAt = Now + PinguHazardTuning::WindEvery;
	UE_LOG(LogPingu, Display, TEXT("[HAZARD] gust %d: %s"), GustSerial, GustDir > 0 ? TEXT("to the right") : TEXT("to the left"));
	ForceNetUpdate();
}

bool APinguRoundHazards::IsWindWarning() const
{
	if (Hazard != EPinguTrait::Wind || !IsRunning())
	{
		return false;
	}
	const float Since = GetNow() - GustAt;
	return Since >= 0.f && Since < PinguHazardTuning::WindWarn;
}

bool APinguRoundHazards::IsGustBlowing() const
{
	if (Hazard != EPinguTrait::Wind || !IsRunning())
	{
		return false;
	}
	const float T = GetNow() - GustAt - PinguHazardTuning::WindWarn;
	return T >= 0.f && T < PinguHazardTuning::WindGust;
}

float APinguRoundHazards::GetWindNow() const
{
	using namespace PinguHazardTuning;
	if (!IsGustBlowing())
	{
		return 0.f;
	}
	const float T = GetNow() - GustAt - WindWarn;
	const float Envelope = FMath::Clamp(FMath::Min(T, WindGust - T) / WindRamp, 0.f, 1.f);
	return GustDir * WindSpeed * PinguRoundHazardsPrivate::SmoothAlpha(Envelope);
}

void APinguRoundHazards::ApplyWind(float DeltaSeconds)
{
	WindMoved = 0;
	const float Wind = GetWindNow();
	if (FMath::IsNearlyZero(Wind) || DeltaSeconds <= 0.f)
	{
		return;
	}
	// Like a conveyor belt under everybody: each machine moves the penguins it runs (its own player; the host its bots too),
	// the others' moves come from where those run.
	const bool bAuthority = HasAuthority();
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (!IsValid(Penguin) || Penguin->IsKnockedOut() || !(Penguin->IsLocallyControlled() || (bAuthority && !Penguin->IsPlayerControlled())))
		{
			continue;
		}
		UPenguinMovementComponent* Movement = Penguin->GetPenguinMovement();
		if (!Movement || !Movement->UpdatedComponent)
		{
			continue;
		}
		FHitResult Hit;
		Movement->SafeMoveUpdatedComponent(FVector(Wind * DeltaSeconds, 0.f, 0.f), Movement->UpdatedComponent->GetComponentQuat(), true, Hit);
		++WindMoved;
	}
}

void APinguRoundHazards::UpdateStreaks(float DeltaSeconds)
{
	using namespace PinguRoundHazardsPrivate;
	if (Hazard != EPinguTrait::Wind)
	{
		return;
	}
	// The gust's sound once per gust (every machine its own).
	if (GustSerial != SeenGustSerial && IsWindWarning())
	{
		SeenGustSerial = GustSerial;
		APinguFX::Sound(this, EPinguSound::Gust, 0.45f, 0.08f, 0.8f);
	}
	const bool bWarning = IsWindWarning();
	const bool bBlowing = IsGustBlowing();
	const ABlockIsland* Island = GetIsland();
	const APinguFX* FX = APinguFX::Get(this);
	if (!(bWarning || bBlowing) || !Island || !FX)
	{
		StreakCarry = 0.f;
		return;
	}
	const FBox Bounds = Island->GetBounds();
	if (!Bounds.IsValid)
	{
		return;
	}
	const float Strength = bBlowing ? FMath::Abs(GetWindNow()) / PinguHazardTuning::WindSpeed : 1.f;
	StreakCarry += DeltaSeconds * (bBlowing ? StreaksGust : StreaksWarning) * FX->GetDensity() * FMath::Max(Strength, 0.3f);
	const float Dir = static_cast<float>(GustDir);
	while (StreakCarry >= 1.f)
	{
		StreakCarry -= 1.f;
		const float X = FMath::FRandRange(static_cast<float>(Bounds.Min.X) - 700.f, static_cast<float>(Bounds.Max.X) + 700.f);
		// Over the ground where the penguins are (the island's middle height up to a little over its top).
		const float Z = FMath::FRandRange(0.5f * static_cast<float>(Bounds.Min.Z + Bounds.Max.Z), static_cast<float>(Bounds.Max.Z) + 150.f);
		const float Length = FMath::FRandRange(180.f, 340.f) * (bBlowing ? 1.f : 0.7f);
		const FVector From(X, StreakY, Z);
		const FVector To = From + FVector(Dir * Length, 0.f, FMath::FRandRange(-12.f, 12.f));
		APinguFX::GlowTrail(this, From, To, FMath::FRandRange(7.f, 11.f), FLinearColor(1.4f, 1.45f, 1.5f), bBlowing ? 0.6f : 0.35f, 0.45f);
	}
}

void APinguRoundHazards::UpdateArrow()
{
	using namespace PinguHazardTuning;
	if (Hazard != EPinguTrait::Wind)
	{
		return;
	}
	// How strongly the arrow shows: in on the warning, steady through the gust, out at its end.
	ArrowOpacity = 0.f;
	if (IsRunning() && GustSerial > 0)
	{
		const float Since = GetNow() - GustAt;
		if (Since >= 0.f && Since < WindWarn)
		{
			ArrowOpacity = PinguRoundHazardsPrivate::SmoothAlpha(Since / 0.15f);
		}
		else if (Since >= WindWarn && Since < WindWarn + WindGust)
		{
			ArrowOpacity = 0.8f * PinguRoundHazardsPrivate::SmoothAlpha((WindWarn + WindGust - Since) / 0.3f);
		}
	}
	if (ArrowWidget.IsValid())
	{
		return;
	}
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (!Viewport || !FSlateApplication::IsInitialized())
	{
		return;
	}
	ArrowWidget = SNew(SPinguWindArrow, this);
	Viewport->AddViewportWidgetContent(ArrowWidget.ToSharedRef(), 5); // under the HUD's menus (10)
	ArrowViewport = Viewport;
}

void APinguRoundHazards::RemoveArrow()
{
	if (ArrowWidget.IsValid() && ArrowViewport.IsValid())
	{
		ArrowViewport->RemoveViewportWidgetContent(ArrowWidget.ToSharedRef());
	}
	ArrowWidget.Reset();
	ArrowViewport.Reset();
	ArrowOpacity = 0.f;
}

// ---------------------------------------------------------------------------- Snowball Rain

void APinguRoundHazards::TickRain(float Now)
{
	using namespace PinguHazardTuning;
	if (Now >= NextRainAt)
	{
		const bool bForced = bForcedDrop;
		bForcedDrop = false;
		PlanDrop(Now, bForced ? ForcedDropX : TNumericLimits<float>::Lowest());
		// About every RainEvery s on a four-penguin arena; a bigger island gets more (the same rain per metre).
		const ABlockIsland* Island = GetIsland();
		const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
		const float Columns = Bounds.IsValid ? static_cast<float>(Bounds.Max.X - Bounds.Min.X) / CellSize : 18.f;
		const float Scale = FMath::Clamp(18.f / FMath::Max(Columns, 1.f), 0.45f, 1.2f);
		NextRainAt = Now + RainEvery * Scale + Rand.FRandRange(-RainJitter, RainJitter) * Scale;
	}
	for (FPinguRainDrop& Drop : RainDrops)
	{
		if (!Drop.bThrown && Now >= Drop.LandAt - RainFallTime)
		{
			Drop.bThrown = true;
			ThrowDrop(Drop);
		}
	}
	const int32 Before = RainDrops.Num();
	RainDrops.RemoveAll([Now](const FPinguRainDrop& Drop) { return Drop.bThrown && Now > Drop.LandAt + 1.f; });
	if (RainDrops.Num() != Before)
	{
		ForceNetUpdate();
	}
}

namespace PinguRoundHazardsPrivate
{
/** Where a drop is thrown from: over the build limit's top line (off the screen), back along its slant. */
static float RainTopZ(const ABlockIsland* Island)
{
	FPinguBuildLimit Limit;
	if (Island && PinguBuildLimit::Get(Island, Limit))
	{
		return Limit.TopZ + PinguHazardTuning::RainAboveTop;
	}
	const FBox Bounds = Island ? Island->GetBounds() : FBox(ForceInit);
	return Bounds.IsValid ? static_cast<float>(Bounds.Max.Z) + 1000.f : 1200.f;
}

/** A drop's throw: where from and how fast, so that it comes down on (X, LandZ) after RainFallTime. */
static void RainThrow(float TopZ, float X, float LandZ, int32 Side, FVector& OutStart, FVector& OutVelocity)
{
	using namespace PinguHazardTuning;
	const float T = RainFallTime;
	const float Vx = Side * RainSideSpeed;
	const float Down = FMath::Max(0.f, (TopZ - LandZ - 0.5f * RainGravity * T * T) / T);
	OutStart = FVector(X - Vx * T, 0.f, LandZ + Down * T + 0.5f * RainGravity * T * T);
	OutVelocity = FVector(Vx, 0.f, -Down);
}
}

bool APinguRoundHazards::PlanDrop(float Now, float ForcedX)
{
	using namespace PinguHazardTuning;
	using namespace PinguRoundHazardsPrivate;
	const ABlockIsland* Island = GetIsland();
	if (!Island)
	{
		return false;
	}
	const FBox Bounds = Island->GetBounds();
	if (!Bounds.IsValid)
	{
		return false;
	}
	const bool bForced = ForcedX > TNumericLimits<float>::Lowest();
	const float TopZ = RainTopZ(Island);
	const float SnowballRadius = 24.f;
	for (int32 Try = 0; Try < (bForced ? 1 : 8); ++Try)
	{
		const float X = bForced ? ForcedX
			: Rand.FRandRange(static_cast<float>(Bounds.Min.X) + CellSize, static_cast<float>(Bounds.Max.X) - CellSize);
		float GroundZ = 0.f;
		if (!Island->FindGroundBelow(FVector(X, 0.f, TopZ), TopZ - Island->SeaLevel, GroundZ) || GroundZ < Island->SeaLevel + 150.f)
		{
			continue; // a pit: it would fall into the sea
		}
		// From the left or the right at a slant, whichever way is clear of blocks (else straight down).
		const int32 First = Rand.FRand() < 0.5f ? -1 : 1;
		int32 Side = 0;
		for (const int32 Candidate : { First, -First })
		{
			FVector Start, Velocity;
			RainThrow(TopZ, X, GroundZ + SnowballRadius, Candidate, Start, Velocity);
			bool bClear = true;
			for (int32 Step = 1; Step < 10 && bClear; ++Step)
			{
				const float T = RainFallTime * Step / 10.f;
				const FVector At = Start + Velocity * T + FVector(0.f, 0.f, -0.5f * RainGravity * T * T);
				bClear = !Island->IsSolidAt(At);
			}
			if (bClear)
			{
				Side = Candidate;
				break;
			}
		}
		FPinguRainDrop& Drop = RainDrops.AddDefaulted_GetRef();
		Drop.X = X;
		Drop.Z = GroundZ;
		Drop.ShowAt = Now;
		Drop.LandAt = Now + RainLead + RainFallTime;
		Drop.Side = static_cast<int8>(Side);
		Drop.Serial = ++RainSerial;
		ForceNetUpdate();
		return true;
	}
	return false;
}

void APinguRoundHazards::ThrowDrop(FPinguRainDrop& Drop)
{
	using namespace PinguRoundHazardsPrivate;
	const ABlockIsland* Island = GetIsland();
	FVector Start, Velocity;
	RainThrow(RainTopZ(Island), Drop.X, Drop.Z + 24.f, Drop.Side, Start, Velocity);
	// Nobody's snowball: it hits like any snowball (no combo, no snowball back to anybody).
	if (APenguinProjectile* Ball = APenguinProjectile::Launch(GetWorld(), EPenguinProjectileKind::Snowball, nullptr, Start, Velocity))
	{
		Ball->SetGravity(PinguHazardTuning::RainGravity);
	}
}

void APinguRoundHazards::UpdateRings()
{
	using namespace PinguRoundHazardsPrivate;
	ShownRings = 0;
	if (Hazard != EPinguTrait::SnowballRain || bNoRings)
	{
		return;
	}
	const float Now = GetNow();
	int32 Used = 0;
	for (const FPinguRainDrop& Drop : RainDrops)
	{
		if (Used >= MaxRings || Now < Drop.ShowAt || Now > Drop.LandAt + 0.15f || HasStopped())
		{
			continue;
		}
		if (!Rings.IsValidIndex(Used))
		{
			UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
			UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, RingMaterialPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
			if (!Plane || !Material)
			{
				bNoRings = true; // (the fx setup phase builds M_BlastZone)
				return;
			}
			UStaticMeshComponent* Ring = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			Ring->SetupAttachment(RootComponent);
			Ring->SetUsingAbsoluteLocation(true);
			Ring->SetUsingAbsoluteRotation(true);
			Ring->SetUsingAbsoluteScale(true);
			Ring->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Ring->SetGenerateOverlapEvents(false);
			Ring->SetCanEverAffectNavigation(false);
			Ring->SetCastShadow(false);
			Ring->bAffectDistanceFieldLighting = false;
			Ring->bAffectDynamicIndirectLighting = false;
			Ring->bVisibleInRayTracing = false;
			Ring->bVisibleInReflectionCaptures = false;
			Ring->bVisibleInRealTimeSkyCaptures = false;
			Ring->SetStaticMesh(Plane);
			UMaterialInstanceDynamic* RingMaterial = UMaterialInstanceDynamic::Create(Material, this);
			Ring->SetMaterial(0, RingMaterial);
			Ring->SetTranslucentSortPriority(2);
			Ring->RegisterComponent();
			Rings.Add(Ring);
			RingMaterials.Add(RingMaterial);
		}
		UStaticMeshComponent* Ring = Rings[Used];
		UMaterialInstanceDynamic* RingMaterial = RingMaterials[Used];
		++Used;
		const float Span = FMath::Max(Drop.LandAt - Drop.ShowAt, 0.1f);
		const float Progress = FMath::Clamp((Now - Drop.ShowAt) / Span, 0.f, 1.f);
		const float FadeIn = FMath::Clamp((Now - Drop.ShowAt) / 0.15f, 0.f, 1.f);
		const float FadeOut = FMath::Clamp((Drop.LandAt + 0.15f - Now) / 0.15f, 0.f, 1.f);
		const float Radius = PinguHazardTuning::RainRingRadius;
		const float Diameter = 2.f * Radius / 100.f; // the engine plane is 100 cm across
		Ring->SetWorldLocationAndRotation(FVector(Drop.X, 0.f, Drop.Z + 0.55f * Radius), FRotator(0.f, 0.f, 90.f)); // facing the camera
		Ring->SetWorldScale3D(FVector(Diameter, Diameter, 1.f));
		Ring->SetVisibility(true);
		if (RingMaterial)
		{
			RingMaterial->SetVectorParameterValue(TEXT("Color"), RingColor);
			RingMaterial->SetVectorParameterValue(TEXT("CoreColor"), RingCore);
			RingMaterial->SetScalarParameterValue(TEXT("Progress"), Progress);
			RingMaterial->SetScalarParameterValue(TEXT("Pulse"), 0.5f + 0.5f * FMath::Sin(Now * 2.f * UE_PI * FMath::Lerp(2.f, 7.f, Progress)));
			RingMaterial->SetScalarParameterValue(TEXT("Opacity"), FadeIn * FadeOut);
			RingMaterial->SetScalarParameterValue(TEXT("Inner"), RingInner);
			RingMaterial->SetScalarParameterValue(TEXT("RingWidth"), FMath::Clamp(RingWidthCm / Radius, 0.005f, 0.3f));
		}
	}
	ShownRings = Used;
	for (int32 Index = Used; Index < Rings.Num(); ++Index)
	{
		if (Rings[Index] && Rings[Index]->IsVisible())
		{
			Rings[Index]->SetVisibility(false);
		}
	}
}

// ---------------------------------------------------------------------------- bots

bool APinguRoundHazards::SteerBot(const APenguinCharacter* Bot, const ABlockIsland* Island, float& Desired)
{
	const APinguRoundHazards* H = Bot ? Get(Bot->GetWorld()) : nullptr;
	if (!H || !H->IsRunning() || !Island)
	{
		return false;
	}
	const float X = static_cast<float>(Bot->GetActorLocation().X);
	const float Radius = Bot->GetSimpleCollisionRadius();
	switch (H->Hazard)
	{
	case EPinguTrait::CrumblingIsland:
		// Off the shaking blocks (and the ones next to them), toward the middle of what stays.
		if (Island->IsCrumblingOver(X - Radius - 80.f, X + Radius + 80.f))
		{
			const float Middle = 0.5f * (H->CrumbleLeftX + H->CrumbleRightX);
			Desired = X < Middle ? 1.f : -1.f;
			return true;
		}
		return false;
	case EPinguTrait::SnowballRain:
	{
		// Out of a ring about to be hit.
		const float Now = H->GetNow();
		for (const FPinguRainDrop& Drop : H->RainDrops)
		{
			const float Left = Drop.LandAt - Now;
			if (Left > -0.05f && Left < 1.f && FMath::Abs(X - Drop.X) < Radius + PinguHazardTuning::RainRingRadius + 30.f)
			{
				Desired = X >= Drop.X ? 1.f : -1.f;
				return true;
			}
		}
		return false;
	}
	case EPinguTrait::Wind:
	{
		// Near the edge it blows toward: lean into it.
		if (!H->IsWindWarning() && !H->IsGustBlowing())
		{
			return false;
		}
		const FBox Bounds = Island->GetBounds();
		if (!Bounds.IsValid)
		{
			return false;
		}
		const float Dir = static_cast<float>(H->GustDir);
		const float ToEdge = Dir > 0.f ? static_cast<float>(Bounds.Max.X) - X : X - static_cast<float>(Bounds.Min.X);
		if (ToEdge < 700.f)
		{
			Desired = -Dir;
			return true;
		}
		return false;
	}
	default:
		return false;
	}
}

// Pingu - the build limit (see PinguBuildLimit.h).

#include "PinguBuildLimit.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PinguFX.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace PinguBuildLimitPrivate
{
	/** The stage camera's framing is worked out for this screen shape (the game's; a test window's own shape does not move it). */
	constexpr float Aspect = 16.f / 9.f;
	/** A block may touch the line (rounding). */
	constexpr float Slack = 1.f;

	static int32 Refusals = 0;
	static FVector LastRefusedAt = FVector::ZeroVector;
	static FString LastRefused;

	/** The world's stage camera (its numbers may be edited in the level), else the class defaults: the same on every machine. */
	static const APenguinCamera* GetCameraNumbers(const UWorld* World)
	{
		if (World)
		{
			for (TActorIterator<APenguinCamera> It(const_cast<UWorld*>(World)); It; ++It)
			{
				return *It;
			}
		}
		return GetDefault<APenguinCamera>();
	}
}

bool PinguBuildLimit::Get(const ABlockIsland* Island, FPinguBuildLimit& Out)
{
	using namespace PinguBuildLimitPrivate;
	if (!Island || (Island->Layout != EIslandLayout::Arena && Island->Layout != EIslandLayout::Lobby))
	{
		return false;
	}
	FBox Bounds;
	float GroundMinZ = 0.f;
	float GroundMaxZ = 0.f;
	if (!Island->GetBaseFrame(Bounds, GroundMinZ, GroundMaxZ))
	{
		return false;
	}
	const APenguinCamera* Cam = GetCameraNumbers(Island->GetWorld());
	if (!Cam)
	{
		return false;
	}
	// The island's frame as the stage camera makes it for a fresh island (APenguinCamera::GetIslandFrame, without the penguins).
	FBox Frame;
	if (Island->Layout == EIslandLayout::Lobby)
	{
		Frame = FBox(FVector(Bounds.Min.X - Cam->LobbyMargin.X, 0.f, GroundMinZ - Cam->LobbyMarginBelow),
			FVector(Bounds.Max.X + Cam->LobbyMargin.X, 0.f, FMath::Max(static_cast<float>(Bounds.Max.Z), GroundMaxZ) + Cam->LobbyMargin.Y));
	}
	else
	{
		const float MarginScale = FMath::Clamp(Island->GetArenaScale(), Cam->MinIslandMarginScale, 1.f);
		Frame = FBox(FVector(Bounds.Min.X - Cam->IslandMargin.X * MarginScale, 0.f, GroundMinZ - Cam->IslandMarginBelow * MarginScale),
			FVector(Bounds.Max.X + Cam->IslandMargin.X * MarginScale, 0.f, FMath::Max(static_cast<float>(Bounds.Max.Z), GroundMaxZ) + Cam->IslandMargin.Y * MarginScale));
	}
	// Its distance (APenguinCamera::Tick: the frame fitted, never nearer than MinDistance, never further than MaxFrameDistance).
	const float TanHalfH = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Cam->FieldOfView, 5.f, 170.f) * 0.5f));
	const float TanHalfV = TanHalfH / Aspect;
	const float Width = static_cast<float>(Frame.Max.X - Frame.Min.X);
	const float Height = static_cast<float>(Frame.Max.Z - Frame.Min.Z);
	const float Needed = FMath::Max(Width * 0.5f / TanHalfH, Height * 0.5f / TanHalfV);
	const float Distance = FMath::Clamp(Needed, Cam->MinDistance, Cam->MaxFrameDistance);
	// The top edge of the view on the play plane, the camera looking at the frame's middle from Distance, pitched down a little.
	const float PitchRad = FMath::DegreesToRadians(Cam->Pitch);
	const float HalfV = FMath::Atan(TanHalfV);
	const float MiddleZ = static_cast<float>(Frame.GetCenter().Z);
	const float CameraZ = MiddleZ - FMath::Sin(PitchRad) * Distance;
	const float CameraBack = FMath::Cos(PitchRad) * Distance;
	const float ViewTop = CameraZ + CameraBack * FMath::Tan(HalfV + PitchRad);
	// (An island too big to show whole is followed inside its frame: the view never goes above the frame's top then.)
	Out.TopZ = FMath::Max(ViewTop, static_cast<float>(Frame.Max.Z));

	const FVector Origin = Island->GetActorLocation();
	const float Cell = Island->CellSize;
	const int32 FirstColumn = FMath::RoundToInt(static_cast<float>(Bounds.Min.X - Origin.X) / Cell);
	const int32 LastColumn = FMath::RoundToInt(static_cast<float>(Bounds.Max.X - Origin.X) / Cell) - 1;
	Out.MinColumn = FirstColumn - SideCells;
	Out.MaxColumn = LastColumn + SideCells;
	Out.MinX = static_cast<float>(Origin.X) + Out.MinColumn * Cell;
	Out.MaxX = static_cast<float>(Origin.X) + (Out.MaxColumn + 1) * Cell;
	return true;
}

bool PinguBuildLimit::AllowsCells(const ABlockIsland* Island, const TArray<FIntPoint>& Cells)
{
	FPinguBuildLimit Limit;
	if (!Get(Island, Limit))
	{
		return true;
	}
	const float OriginZ = static_cast<float>(Island->GetActorLocation().Z);
	for (const FIntPoint& Cell : Cells)
	{
		const float Top = OriginZ + (Cell.Y + 1) * Island->CellSize;
		if (Cell.X < Limit.MinColumn || Cell.X > Limit.MaxColumn || Top > Limit.TopZ + PinguBuildLimitPrivate::Slack)
		{
			return false;
		}
	}
	return true;
}

bool PinguBuildLimit::AllowsSpan(const ABlockIsland* Island, float MinX, float MaxX, float TopZ)
{
	using namespace PinguBuildLimitPrivate;
	FPinguBuildLimit Limit;
	if (!Get(Island, Limit))
	{
		return true;
	}
	return MinX >= Limit.MinX - Slack && MaxX <= Limit.MaxX + Slack && TopZ <= Limit.TopZ + Slack;
}

FVector PinguBuildLimit::ClampInside(const ABlockIsland* Island, const FVector& Where)
{
	FPinguBuildLimit Limit;
	if (!Get(Island, Limit))
	{
		return Where;
	}
	// (A little inside: the puff is seen whole.)
	const float Keep = 60.f;
	return FVector(FMath::Clamp(static_cast<float>(Where.X), Limit.MinX + Keep, Limit.MaxX - Keep), 0.f,
		FMath::Min(static_cast<float>(Where.Z), Limit.TopZ - Keep));
}

void PinguBuildLimit::ShowRefused(const UObject* Ctx, const ABlockIsland* Island, const FVector& Where, const TCHAR* What)
{
	using namespace PinguBuildLimitPrivate;
	const FVector At = ClampInside(Island, Where);
	++Refusals;
	LastRefusedAt = At;
	LastRefused = What ? What : TEXT("");
	// A small grey puff, a dull red ring: "no". (Inside the ability's broadcast scope, every LAN machine sees it.)
	APinguFX::Puffs(Ctx, At, 6, 20.f, 150.f, FVector::UpVector, 1.f, FLinearColor(0.62f, 0.66f, 0.72f));
	APinguFX::Ring(Ctx, At, 38.f, FLinearColor(0.85f, 0.32f, 0.28f), 0.3f);
	APinguFX::Sound(Ctx, EPinguSound::UIBack, 0.45f, 0.04f, 0.8f);
	UE_LOG(LogPingu, Log, TEXT("[BUILDLIMIT] %s refused at %s"), *LastRefused, *At.ToCompactString());
}

int32 PinguBuildLimit::GetRefusals()
{
	return PinguBuildLimitPrivate::Refusals;
}

FVector PinguBuildLimit::GetLastRefusedAt()
{
	return PinguBuildLimitPrivate::LastRefusedAt;
}

FString PinguBuildLimit::GetLastRefused()
{
	return PinguBuildLimitPrivate::LastRefused;
}

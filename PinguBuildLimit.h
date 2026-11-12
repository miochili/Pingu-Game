// Pingu - the build limit (2026-11-05, card build-limit-snowman; Linus: "make it so nothing can get placed above the space on the
// screen so bots cant just be there with platfrom spawn and the same for some headromm on both sides of the island and below u can
// always place").
//
// Everything an ability leaves solid or standing in the world (the Ice Wall's blocks and ledges, the Iceberg Drop's ice, the
// Snowman, the grow ray's new ice block) must lie inside a fixed box per island:
//  - top: the top of the stage camera's view when it shows the whole island as generated (16:9, the camera's field of view, pitch,
//    margins and distance limits; never the camera distance setting, the live zoom or the penguins): the same line on every
//    machine for the whole round;
//  - sides: SideCells (6) cell columns beyond the island's outermost generated columns;
//  - below: no limit.
// Only arenas and the lobby island have one (the stage camera frames only those); the practice course and the tests' strips don't.
// The host decides (abilities run only there), so a LAN client can't place past it. A refused use spends nothing (no cooldown, no
// use count) and shows a small "no" puff (ShowRefused). Bots ask first (UPenguinAbilityComponent::IsPlacementAllowed) and never try.

#pragma once

#include "CoreMinimal.h"

class ABlockIsland;

/** The build limit of one island (world, play plane). */
struct FPinguBuildLimit
{
	/** The allowed cell columns (island cell X) and their outer edges (world X). */
	int32 MinColumn = 0;
	int32 MaxColumn = 0;
	float MinX = 0.f;
	float MaxX = 0.f;
	/** Nothing may reach above this (world Z). */
	float TopZ = 0.f;
};

namespace PinguBuildLimit
{
	/** Columns allowed beyond the island's outermost generated ones, on each side. */
	constexpr int32 SideCells = 6;

	/** Island's limit (cached per island generation). False: this island has none (practice course, test strips). */
	PENGUIN_API bool Get(const ABlockIsland* Island, FPinguBuildLimit& Out);
	/** Every cell inside the limit (its whole block under TopZ, its column inside the side columns). True without a limit. */
	PENGUIN_API bool AllowsCells(const ABlockIsland* Island, const TArray<FIntPoint>& Cells);
	/** Something from MinX to MaxX (world X) reaching up to TopZ inside the limit. True without a limit. */
	PENGUIN_API bool AllowsSpan(const ABlockIsland* Island, float MinX, float MaxX, float TopZ);
	/** Where (world) moved inside the limit (for the "no" puff: on screen, not past the top). */
	PENGUIN_API FVector ClampInside(const ABlockIsland* Island, const FVector& Where);
	/** Server / solo, inside the ability's FX broadcast: the small "no" puff at Where (clamped inside the limit) and a dull
	 *  click. Counted for the tests. */
	PENGUIN_API void ShowRefused(const UObject* Ctx, const ABlockIsland* Island, const FVector& Where, const TCHAR* What);

	/** Tests: refused placements so far (this process), the last one's place and what it was. */
	PENGUIN_API int32 GetRefusals();
	PENGUIN_API FVector GetLastRefusedAt();
	PENGUIN_API FString GetLastRefused();
}

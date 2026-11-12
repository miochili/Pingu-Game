// Pingu - player names in a LAN game (2026-10-30, card session-hardening; the cheat audit: "any characters, lookalikes of the
// host's name, text that mimics a feed line"). The host cleans every name a joiner sends and hands out number suffixes for
// names that are, or look like, one already in the lobby. Pure functions (tested by autotest anticheat_Names).

#pragma once

#include "CoreMinimal.h"

namespace PinguNames
{
	/** The longest name (characters). */
	constexpr int32 MaxLength = 20;

	/** A name as the game shows it: control, zero-width, direction-mark and surrogate characters dropped, every kind of white
	 *  space one plain space (runs of them one), "?" dropped (it would break the join URL), trimmed, cut to MaxLength. Empty
	 *  in, or nothing left: empty out (the caller picks "Pingu"). */
	PENGUIN_API FString Sanitize(const FString& Name);

	/** What two names must not differ in to count as the same: lower case, no accents, Cyrillic / Greek / full-width letters
	 *  as their Latin lookalikes, 0 O, 1 l I |, 5 S, 3 E, 4 A, "rn" m, "vv" w, no spaces or marks between. */
	PENGUIN_API FString Fold(const FString& Name);

	/** Name (already Sanitize d), or Name + " 2" / " 3" ... (still within MaxLength) when its Fold equals one of Others'. */
	PENGUIN_API FString MakeUnique(const FString& Name, const TArray<FString>& Others);
}

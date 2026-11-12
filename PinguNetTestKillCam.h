// Pingu - the LAN test's kill cam check (card 2026-11-07_kill-cam; PinguKillCam.h). Both machines watch every round's end: the
// round-winning knock-out plays in slow motion here too (the host's plan, this machine's own curve), it is back at exactly 1x
// before the winner banner, and a joiner's clock stays with the host's through it (its server time offset moves less than
// MaxDrift). The other LAN checks of the rounds after it show the rest stays in sync.
#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace PinguNetTestKillCam
{
	/** Every frame of the LAN test, on both machines. */
	void Tick(const UWorld* World, bool bHost);
	/** The check line (APinguNetTest's Check). */
	void AddChecks(TFunctionRef<void(const TCHAR*, bool, const FString&)> Check);
}

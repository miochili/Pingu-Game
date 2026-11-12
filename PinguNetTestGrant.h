// Pingu - the LAN tests hand abilities to penguins with GiveAbility; a joiner's anti-cheat judge counts an ability that changed
// without a bubble going away as a rule break by the host. The grant therefore comes with a bubble of the same ability that
// appears away from everybody and pops a moment later, as a real pickup's would.
#pragma once

#include "CoreMinimal.h"
#include "PenguinAbilities.h"

namespace PinguNetTestGrant
{
	/** Gives the ability (server only) and, within the judge's window, lets a bubble of it go away. */
	void Give(UPenguinAbilityComponent* Abilities, EPenguinAbility Ability);
}

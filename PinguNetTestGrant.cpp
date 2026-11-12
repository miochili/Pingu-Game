// Pingu - see PinguNetTestGrant.h.

#include "PinguNetTestGrant.h"

#if !UE_BUILD_SHIPPING // (a test hook: not in the Shipping exe - 2026-11-05, card host-cheat-fix)
#include "PinguAntiCheat.h"
#include "PinguPickup.h"
#include "Engine/World.h"
#include "TimerManager.h"

void PinguNetTestGrant::Give(UPenguinAbilityComponent* Abilities, EPenguinAbility Ability)
{
	if (!Abilities)
	{
		return;
	}
	UWorld* World = Abilities->GetWorld();
	if (World)
	{
		// The joiners' judge: our script hands this out, not a bubble near the penguin (dev joiners only honour it).
		APinguAntiCheat::DevQuietScript(World, 4.f);
		// Far above the fight, out of everybody's reach; it goes away 0.5 s later (the clients must have seen it exist first).
		if (APinguPickup* Bubble = APinguPickup::Spawn(World, Ability, FVector(0.0, 0.0, 60000.0)))
		{
			const TWeakObjectPtr<APinguPickup> Weak(Bubble);
			FTimerHandle Handle;
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Weak]()
			{
				if (Weak.IsValid())
				{
					Weak->Destroy();
				}
			}), 0.5f, false);
		}
	}
	Abilities->GiveAbility(Ability);
}
#else
void PinguNetTestGrant::Give(UPenguinAbilityComponent*, EPenguinAbility)
{
}
#endif

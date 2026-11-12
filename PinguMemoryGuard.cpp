// Pingu - the guard against memory trainers: see PinguMemoryGuard.h.

#include "PinguMemoryGuard.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PinguAntiCheat.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/DelayedAutoRegister.h"
#include "UObject/UnrealType.h"

namespace PinguMemoryGuardPrivate
{
	int32 MovementRestores = 0;
	double LastLog = -100.0;
#if !UE_BUILD_SHIPPING
	bool bForcedOn = false;
	double LastTrainerWrite = -100.0;
#endif

	/** The numbers of the movement component the guard watches: every editable number our class declares (the tuning). */
	const TArray<FProperty*>& Watched()
	{
		static TArray<FProperty*> List;
		static bool bMade = false;
		if (!bMade)
		{
			bMade = true;
			const UClass* Class = UPenguinMovementComponent::StaticClass();
			for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				const bool bNumber = Property->IsA<FFloatProperty>() || Property->IsA<FDoubleProperty>() || Property->IsA<FIntProperty>()
					|| Property->IsA<FBoolProperty>();
				if (bNumber && Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Net)
					&& Property->ArrayDim == 1)
				{
					List.Add(Property);
				}
			}
		}
		return List;
	}

	float FloatOf(const UObject* Object, const TCHAR* Name, float Fallback)
	{
		if (const FFloatProperty* Property = FindFProperty<FFloatProperty>(Object->GetClass(), Name))
		{
			return Property->GetPropertyValue_InContainer(Object);
		}
		return Fallback;
	}

	/** What RecomputeJumpPhysics makes of the tuning (UPenguinMovementComponent): the engine's numbers that follow from it. */
	struct FDerived
	{
		float GravityScale = 1.f;
		float JumpZVelocity = 0.f;
		float MaxWalkSpeed = 0.f;
		float MaxAcceleration = 0.f;
	};
	FDerived DeriveFrom(const UPenguinMovementComponent* Move)
	{
		FDerived Out;
		const float H = FMath::Max(FloatOf(Move, TEXT("JumpHeight"), 100.f), 1.f);
		const float T = FMath::Max(FloatOf(Move, TEXT("JumpTimeToApex"), 0.3f), 0.05f);
		const float G = 2.f * H / (T * T);
		const UWorld* World = Move->GetWorld();
		const float WorldGravity = World ? FMath::Max(FMath::Abs(World->GetGravityZ()), 1.f) : 980.f;
		Out.GravityScale = G / WorldGravity;
		Out.JumpZVelocity = 2.f * H / T;
		Out.MaxWalkSpeed = FloatOf(Move, TEXT("RunSpeed"), Move->MaxWalkSpeed);
		Out.MaxAcceleration = FloatOf(Move, TEXT("GroundAcceleration"), Move->MaxAcceleration);
		return Out;
	}

	/** Per movement component: whether its engine numbers matched the formula when we first saw it (if not, a future change of
	 *  RecomputeJumpPhysics: those four are left alone rather than "fixed" wrongly). */
	TMap<TWeakObjectPtr<const UPenguinMovementComponent>, bool>& DerivedOk()
	{
		static TMap<TWeakObjectPtr<const UPenguinMovementComponent>, bool> Map;
		return Map;
	}

	bool Near(float A, float B)
	{
		return FMath::Abs(A - B) <= FMath::Max(0.01f * FMath::Abs(B), 0.01f);
	}

	/** Puts one penguin's movement back to its class defaults; returns how many numbers it had to fix. */
	int32 GuardMovement(UPenguinMovementComponent* Move)
	{
		const UObject* Archetype = Move ? Move->GetArchetype() : nullptr;
		if (!Archetype || Archetype->GetClass() != Move->GetClass())
		{
			return 0;
		}
		int32 Fixed = 0;
		for (FProperty* Property : Watched())
		{
			void* Here = Property->ContainerPtrToValuePtr<void>(Move);
			const void* There = Property->ContainerPtrToValuePtr<void>(Archetype);
			if (!Property->Identical(Here, There))
			{
				Property->CopyCompleteValue(Here, There);
				++Fixed;
			}
		}
		// The engine's four that follow from the tuning (checked once against the formula: see DerivedOk).
		const FDerived Want = DeriveFrom(Move);
		TMap<TWeakObjectPtr<const UPenguinMovementComponent>, bool>& Map = DerivedOk();
		bool* Ok = Map.Find(Move);
		if (!Ok)
		{
			for (auto It = Map.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid())
				{
					It.RemoveCurrent();
				}
			}
			const bool bMatches = Fixed == 0 && Near(Move->GravityScale, Want.GravityScale) && Near(Move->JumpZVelocity, Want.JumpZVelocity)
				&& Near(Move->MaxWalkSpeed, Want.MaxWalkSpeed) && Near(Move->MaxAcceleration, Want.MaxAcceleration);
			Ok = &Map.Add(Move, bMatches);
			if (!bMatches)
			{
				UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] memory guard: %s's gravity / jump / walk numbers differ from the tuning's at the start - not guarded"),
					*GetNameSafe(Move->GetOwner()));
			}
		}
		if (*Ok)
		{
			if (!Near(Move->GravityScale, Want.GravityScale)) { Move->GravityScale = Want.GravityScale; ++Fixed; }
			if (!Near(Move->JumpZVelocity, Want.JumpZVelocity)) { Move->JumpZVelocity = Want.JumpZVelocity; ++Fixed; }
			if (!Near(Move->MaxWalkSpeed, Want.MaxWalkSpeed)) { Move->MaxWalkSpeed = Want.MaxWalkSpeed; ++Fixed; }
			if (!Near(Move->MaxAcceleration, Want.MaxAcceleration)) { Move->MaxAcceleration = Want.MaxAcceleration; ++Fixed; }
		}
		return Fixed;
	}

	void OnPreActorTick(UWorld* World, ELevelTick, float)
	{
		PinguMemoryGuard::TickWorld(World);
	}

#if !UE_BUILD_SHIPPING
	void OnPostActorTick(UWorld* World, ELevelTick, float)
	{
		// The dev trainer writes after the actors ticked (as an outside program may at any moment): the guard must still win
		// before the next use, and the abilities' PreReplication before anything goes out.
		if (!World || !World->IsGameWorld() || World->GetNetMode() == NM_Client || !PinguAntiCheat::IsDevTrainer())
		{
			return;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now - LastTrainerWrite < 0.05)
		{
			return;
		}
		LastTrainerWrite = Now;
		PinguMemoryGuard::DevTrainerWrite(World);
	}
#endif

	void Register()
	{
		FWorldDelegates::OnWorldPreActorTick.AddStatic(&OnPreActorTick);
#if !UE_BUILD_SHIPPING
		FWorldDelegates::OnWorldPostActorTick.AddStatic(&OnPostActorTick);
#endif
	}
	static FDelayedAutoRegisterHelper GRegister(EDelayedRegisterRunPhase::EndOfEngineInit, []() { Register(); });
}

bool PinguMemoryGuard::IsActive()
{
	if (PinguAntiCheat::IsDevNoGuard())
	{
		return false;
	}
#if !UE_BUILD_SHIPPING
	if (PinguMemoryGuardPrivate::bForcedOn)
	{
		return true;
	}
#endif
	return !GIsEditor;
}

void PinguMemoryGuard::TickWorld(UWorld* World)
{
	using namespace PinguMemoryGuardPrivate;
	if (!World || !World->IsGameWorld() || !IsActive())
	{
		return;
	}
	// Server / solo: every penguin (the host simulates its own and the bots). A LAN joiner: its own penguin (it moves it).
	const bool bClient = World->GetNetMode() == NM_Client;
	int32 Fixed = 0;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (!IsValid(Penguin) || (bClient && !Penguin->IsLocallyControlled()))
		{
			continue;
		}
		Fixed += GuardMovement(Penguin->GetPenguinMovement());
	}
	if (Fixed > 0)
	{
		MovementRestores += Fixed;
		const double Now = FPlatformTime::Seconds();
		if (Now - LastLog > 5.0)
		{
			LastLog = Now;
			UE_LOG(LogPingu, Warning, TEXT("[ANTICHEAT] memory guard: movement numbers changed from outside the game - put back (%d so far)"),
				MovementRestores);
		}
	}
}

int32 PinguMemoryGuard::GetMovementRestores()
{
	return PinguMemoryGuardPrivate::MovementRestores;
}

int32 PinguMemoryGuard::GetWatchedCount()
{
	return PinguMemoryGuardPrivate::Watched().Num() + 4;
}

#if !UE_BUILD_SHIPPING
void PinguMemoryGuard::SetForcedOn(bool bOn)
{
	PinguMemoryGuardPrivate::bForcedOn = bOn;
}

void PinguMemoryGuard::DevTrainerWrite(UWorld* World, APenguinCharacter* Target)
{
	using namespace PinguMemoryGuardPrivate;
	if (!Target && World)
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (const APlayerController* Controller = It->Get(); Controller && Controller->IsLocalController())
			{
				Target = Cast<APenguinCharacter>(Controller->GetPawn());
				break;
			}
		}
	}
	if (!Target)
	{
		return;
	}
	// The loadout it "picked" and no cooldowns (raw writes into the replicated arrays, past the game's own code) ...
	if (UPenguinAbilityComponent* Abilities = Target->GetAbilities())
	{
		const EPenguinAbility Picks[] = { EPenguinAbility::SnowBomb, EPenguinAbility::IceGun, EPenguinAbility::Noot };
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			Abilities->DevTrainerWrite(Slot, Picks[Slot], Slot == 2);
		}
	}
	// ... and the movement numbers it edits, by their names as the trainer finds them.
	if (UPenguinMovementComponent* Move = Target->GetPenguinMovement())
	{
		const UObject* Archetype = Move->GetArchetype();
		const auto Scale = [Move, Archetype](const TCHAR* Name, float Factor)
		{
			if (FFloatProperty* Property = FindFProperty<FFloatProperty>(Move->GetClass(), Name))
			{
				Property->SetPropertyValue_InContainer(Move, Property->GetPropertyValue_InContainer(Archetype ? Archetype : Move) * Factor);
			}
		};
		Scale(TEXT("RunSpeed"), 2.f);
		Scale(TEXT("DashCooldown"), 0.1f);
		if (FIntProperty* Jumps = FindFProperty<FIntProperty>(Move->GetClass(), TEXT("MaxAirJumps")))
		{
			Jumps->SetPropertyValue_InContainer(Move, 4);
		}
		Move->GravityScale = DeriveFrom(Move).GravityScale * 0.5f;
		Move->MaxWalkSpeed = DeriveFrom(Move).MaxWalkSpeed; // (follows the doubled run speed, as the trainer's write would)
	}
}
#endif

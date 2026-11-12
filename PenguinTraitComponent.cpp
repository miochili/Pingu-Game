// Pingu - a penguin's trait: the state, its replication and the reveal's clock (the effects: PenguinTraitEffects.cpp;
// the looks: PenguinTraitLooks.cpp).

#include "PenguinTraitComponent.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "Net/UnrealNetwork.h"

namespace PenguinTraitComponentPrivate
{
	/** GetRevealAge without a reveal: long past every step of it. */
	constexpr float NoRevealAge = 1.e6f;
}

FString FPinguTraitCounters::ToString() const
{
	return FString::Printf(TEXT("uses %d, doubles %d, fizzles %d, swaps %d, reflections %d, second chances %d, ice cells %d, melted blocks %d, leech %.1f s"),
		Uses, Doubles, Fizzles, Swaps, Reflections, SecondChances, IceCells, MeltedBlocks, LeechSeconds);
}

UPenguinTraitComponent::UPenguinTraitComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	// The trait and its events go to every LAN machine (the penguin replicates, so do we).
	SetIsReplicatedByDefault(true);
}

void UPenguinTraitComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPenguinTraitComponent, State);
	DOREPLIFETIME(UPenguinTraitComponent, Events);
}

void UPenguinTraitComponent::BeginPlay()
{
	Super::BeginPlay();
	// A machine that sees the penguin for the first time (a LAN client joining late): whatever trait it has already shows,
	// without the transform's burst (the look decides from GetRevealAge).
	bTransformDone = !IsServer() || !HasAnyTrait() || IsTransformed();
	if (HasAnyTrait())
	{
		OnLookStateChanged();
	}
}

void UPenguinTraitComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EndLooks();
	Super::EndPlay(EndPlayReason);
}

APenguinCharacter* UPenguinTraitComponent::GetPenguin() const
{
	return Cast<APenguinCharacter>(GetOwner());
}

bool UPenguinTraitComponent::IsServer() const
{
	const AActor* OwnerActor = GetOwner();
	return OwnerActor && OwnerActor->HasAuthority();
}

float UPenguinTraitComponent::GetRevealAge() const
{
	if (State.RevealStart < 0.f)
	{
		return PenguinTraitComponentPrivate::NoRevealAge;
	}
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	return GameState ? static_cast<float>(GameState->GetFlowTime() - State.RevealStart) : PenguinTraitComponentPrivate::NoRevealAge;
}

void UPenguinTraitComponent::SetTrait(EPinguTrait NewTrait, float InRevealStart)
{
	if (!IsServer())
	{
		return; // the trait comes from the server
	}
	const EPinguTrait Clean = PinguTraits::IsReal(NewTrait) ? NewTrait : EPinguTrait::None;
	if (Clean == EPinguTrait::None && State.Trait == EPinguTrait::None)
	{
		return; // nothing to take away: no new Serial (the lobby and the menu clear every round)
	}
	ResetEffects();
	State.Trait = Clean;
	State.RevealStart = Clean != EPinguTrait::None ? InRevealStart : -1.f;
	++State.Serial;
	Events = FPinguTraitEvents();
	bTransformDone = false;
	if (Clean == EPinguTrait::None)
	{
		bTransformDone = true;
		// The base size back to normal (a size ray on top keeps its steps).
		if (APenguinCharacter* Penguin = GetPenguin())
		{
			Penguin->SetBaseSize(1.f);
		}
	}
	OnLookStateChanged(); // a listen server gets no replication notifies of its own
	if (AActor* OwnerActor = GetOwner())
	{
		OwnerActor->ForceNetUpdate();
	}
	UpdateTransform(); // without a reveal: at once
}

void UPenguinTraitComponent::UpdateTransform()
{
	if (bTransformDone || !IsServer() || !HasAnyTrait() || !IsTransformed())
	{
		return;
	}
	bTransformDone = true;
	OnTransformed();
}

void UPenguinTraitComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (IsServer() && HasAnyTrait())
	{
		UpdateTransform();
		TickEffects(DeltaTime);
	}
	TickLooks(DeltaTime);
}

void UPenguinTraitComponent::OnRep_State()
{
	OnLookStateChanged();
}

void UPenguinTraitComponent::OnRep_Events()
{
	OnLookStateChanged();
}

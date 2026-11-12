// Pingu - ability picks (see PinguAbilityPicks.h).

#include "PinguAbilityPicks.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinPlayerState.h"
#include "PinguSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

bool PinguPicks::IsValid(const TArray<uint8>& Picks)
{
	if (Picks.Num() == 0)
	{
		return true;
	}
	if (Picks.Num() != NumSlots)
	{
		return false;
	}
	TSet<uint8> Seen;
	for (const uint8 Value : Picks)
	{
		if (Value >= static_cast<uint8>(EPenguinAbility::Count))
		{
			return false;
		}
		if (Value != 0)
		{
			bool bAlready = false;
			Seen.Add(Value, &bAlready);
			if (bAlready)
			{
				return false; // the same ability twice
			}
		}
	}
	return true;
}

bool PinguPicks::HasAny(const TArray<uint8>& Picks)
{
	return Picks.ContainsByPredicate([](uint8 Value) { return Value != 0; });
}

TArray<EPenguinAbility> PinguPicks::ToAbilities(const TArray<uint8>& Picks)
{
	TArray<EPenguinAbility> Out;
	Out.Init(EPenguinAbility::None, NumSlots);
	for (int32 Index = 0; Index < NumSlots && Index < Picks.Num(); ++Index)
	{
		Out[Index] = Picks[Index] < static_cast<uint8>(EPenguinAbility::Count) ? static_cast<EPenguinAbility>(Picks[Index]) : EPenguinAbility::None;
	}
	return Out;
}

TArray<uint8> PinguPicks::ToBytes(const TArray<EPenguinAbility>& Picks)
{
	TArray<uint8> Out;
	Out.Init(0, NumSlots);
	for (int32 Index = 0; Index < NumSlots && Index < Picks.Num(); ++Index)
	{
		Out[Index] = static_cast<uint8>(Picks[Index]);
	}
	return HasAny(Out) ? Out : TArray<uint8>();
}

FString PinguPicks::Describe(const TArray<uint8>& Picks)
{
	if (!HasAny(Picks))
	{
		return TEXT("all random");
	}
	return FString::JoinBy(ToAbilities(Picks), TEXT(", "), [](EPenguinAbility Ability)
		{
			return Ability == EPenguinAbility::None ? FString(TEXT("random")) : FString(UPenguinAbilityComponent::GetInfo(Ability).Name);
		});
}

TArray<EPenguinAbility> PinguPicks::MakeLoadout(FRandomStream& Rand, int32 NumAbilities, const TArray<uint8>& Picks)
{
	if (!HasAny(Picks) || !IsValid(Picks))
	{
		return UPenguinAbilityComponent::RollLoadout(Rand, NumAbilities); // RANDOM: the very same rolls as ever
	}
	const TArray<EPenguinAbility> Picked = ToAbilities(Picks);
	// The bag: every ability this penguin did not pick (an unpicked slot never rolls a copy of a picked one).
	TArray<EPenguinAbility> Bag;
	for (int32 Index = 1; Index < static_cast<int32>(EPenguinAbility::Count); ++Index)
	{
		const EPenguinAbility Ability = static_cast<EPenguinAbility>(Index);
		if (!Picked.Contains(Ability))
		{
			Bag.Add(Ability);
		}
	}
	const auto Draw = [&Rand, &Bag]()
	{
		const int32 Pick = Rand.RandRange(0, Bag.Num() - 1);
		const EPenguinAbility Ability = Bag[Pick];
		Bag.RemoveAt(Pick);
		return Ability;
	};
	if (NumAbilities >= NumSlots)
	{
		TArray<EPenguinAbility> Loadout = Picked;
		for (EPenguinAbility& Ability : Loadout)
		{
			if (Ability == EPenguinAbility::None)
			{
				Ability = Draw();
			}
		}
		return Loadout;
	}
	// Two abilities (Second Chance) in the outer slots: the first two picks in slot order, rolled up to two.
	TArray<EPenguinAbility> Two;
	for (const EPenguinAbility Ability : Picked)
	{
		if (Ability != EPenguinAbility::None && Two.Num() < FMath::Max(NumAbilities, 1))
		{
			Two.Add(Ability);
		}
	}
	while (Two.Num() < FMath::Max(NumAbilities, 1))
	{
		Two.Add(Draw());
	}
	TArray<EPenguinAbility> Loadout = { Two[0], EPenguinAbility::None, EPenguinAbility::None };
	if (Two.Num() > 1)
	{
		Loadout[2] = Two[1];
	}
	return Loadout;
}

TArray<EPenguinAbility> PinguPicks::FitShared(const TArray<EPenguinAbility>& Shared, int32 NumAbilities)
{
	if (NumAbilities >= NumSlots || Shared.Num() < 2)
	{
		return Shared;
	}
	TArray<EPenguinAbility> Loadout = { Shared[0], EPenguinAbility::None, EPenguinAbility::None };
	if (NumAbilities > 1)
	{
		Loadout[2] = Shared[1];
	}
	return Loadout;
}

bool PinguPicks::IsSameAbilitiesRound(const TArray<EPinguTrait>& Traits)
{
	return Traits.Contains(EPinguTrait::SameAbilities);
}

bool PinguPicks::IsChooseOn(const UObject* WorldContext)
{
	const APenguinGameState* GameState = APenguinGameState::Get(WorldContext);
	return GameState && GameState->GetSettings().bChooseAbilities;
}

TArray<uint8> PinguPicks::GetPicksFor(const APenguinCharacter* Penguin)
{
	if (!Penguin || !IsChooseOn(Penguin))
	{
		return TArray<uint8>();
	}
	const APenguinPlayerState* State = Penguin->GetPlayerState<APenguinPlayerState>();
	if (!State || State->IsABot() || !Penguin->IsPlayerControlled())
	{
		return TArray<uint8>(); // bots always roll
	}
	return IsValid(State->GetPicks()) && HasAny(State->GetPicks()) ? State->GetPicks() : TArray<uint8>();
}

bool PinguPicks::LoadChooseSetting()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("PinguChoose")))
	{
		return true;
	}
	if (UPinguSettings::IsAutomatedRun())
	{
		return false; // (tests never read the player's own choice)
	}
	const UPinguSettings* Settings = UPinguSettings::Get();
	return Settings && Settings->GetChooseAbilities();
}

void PinguPicks::SaveChooseSetting(bool bOn)
{
	if (UPinguSettings* Settings = UPinguSettings::Get())
	{
		Settings->SetChooseAbilities(bOn); // (writes nothing on automated runs)
	}
}

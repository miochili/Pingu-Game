// Pingu - the LAN anti-cheat (PinguAntiCheat.h, 2026-10-29). The autotest's steps (APenguinAutoTest::AddAntiCheatSteps; filter
// anticheat_ + spawn_target):
//
//  anticheat_FairSeed    commit-reveal: a commit never gives the secret away, the seed changes with every joiner's value and not
//                        with their order, the deal is the same on every machine, random (3 different abilities, no forced killer; two
//                        with Second Chance), traits different within a round, forced ones kept, the bag carries over (all 17
//                        dealt before one comes again), 3000 seeds deal every ability about as often.
//  anticheat_HostGates   the host's gates: the rate limit (40 a second, a burst of 40), an icicle hold cut to what the host saw, a
//                        joiner's moves (walking, dashing, a knockback flight after a push, a host teleport) taken, a teleport and
//                        a run at 5000 cm/s refused, the pull-back after 0.5 s of refusals; the host's own rules on our penguin
//                        (a use, the same again on its cooldown, a slot it does not have, frozen).
//  anticheat_Judge       the joiner's judge fed with made-up replicated streams: a legal one (cooldowns to the rules with
//                        observations at random times, a snowball refund, a bubble taken, a giant snowball let go, Leech, two uses
//                        in one update, walking, dashes, a knockback flight, a warp) gives 0 points; each cheat (another loadout,
//                        no cooldown, a short one, an early use, an ability with no bubble, teleports, a speed run) is caught,
//                        and only a repeated one makes the joiner leave.
//  anticheat_FileCheck   the join check's answer: our files hashed, the same nonce gives the same answer, another nonce another.
//  anticheat_Tamper      the tamper helper's foreign-DLL logic (PinguTamper::IsForeignModule) with a made-up game folder and
//                        shipped-files list: DLLs outside the folder and our own shipped files are fine, UE4SS-style proxies
//                        dropped into the folder (dwmapi, xinput, UE4SS, even a dbghelp copy) are foreign; without the staging
//                        manifests only known proxy names are (every shipped DLL stays fine); paths normalised; a clean build's
//                        handshake token is empty. The live OS checks run only in a packaged Shipping game.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PinguAntiCheat.h"
#include "PinguMemoryGuard.h"
#include "PinguNames.h"
#include "PinguTamper.h"
#include "PinguTuning.h"
#include "HAL/PlatformTime.h"

namespace PenguinAutoTestAntiCheatPrivate
{
	FString Ok(bool bOk) { return bOk ? TEXT("ok") : TEXT("FAIL"); }

	/** A made-up penguin for the judge: slot, name, abilities. */
	FPinguSeenPenguin MakeSeen(int32 Slot, const TArray<EPenguinAbility>& Abilities, EPinguTrait Trait = EPinguTrait::None)
	{
		FPinguSeenPenguin Seen;
		Seen.Slot = Slot;
		Seen.Name = FString::Printf(TEXT("Pingu %d"), Slot);
		Seen.Trait = Trait;
		for (const EPenguinAbility Ability : Abilities)
		{
			FPinguSeenSlot Entry;
			Entry.Ability = Ability;
			Seen.Slots.Add(Entry);
		}
		return Seen;
	}

	/** The host's side of a made-up fight: uses an ability (its cooldown to the rules, or a cheat's). */
	void Use(FPinguSeenPenguin& Seen, int32 Slot, double HostNow, float Length)
	{
		FPinguSeenSlot& Entry = Seen.Slots[Slot];
		++Entry.Uses;
		Entry.Length = Length;
		Entry.End = HostNow + Length + (Entry.Ability == EPenguinAbility::GiantSnowball ? PinguTuning::GiantBuildTime : 0.f);
	}

	float RuleOf(EPenguinAbility Ability, EPinguTrait Trait = EPinguTrait::None)
	{
		return UPenguinAbilityComponent::GetInfo(Ability).Cooldown * PinguTraits::GetInfo(Trait).N.CooldownScale;
	}

	/** A deal with one penguin (slot) for the judge's loadout check. */
	FPinguFairDeal OneDeal(int32 Slot, const TArray<EPenguinAbility>& Loadout, EPinguTrait Trait = EPinguTrait::None)
	{
		FPinguFairDeal Deal;
		Deal.Slots.Add(static_cast<uint8>(Slot));
		Deal.Traits.Add(Trait);
		Deal.Loadouts.Add(Loadout);
		return Deal;
	}
}

void APenguinAutoTest::AddAntiCheatSteps()
{
	using namespace PenguinAutoTestAntiCheatPrivate;

	// ---------------------------------------------------------------- commit-reveal and the deal
	AddStep(TEXT("anticheat_FairSeed"), 0.1f,
		[this]
		{
			// The commit: a hash, the same for the same secret and serial, another for anything else.
			const FString Secret = PinguFair::MakeSecret();
			const FString Commit = PinguFair::CommitOf(Secret, 7);
			const bool bCommitOk = Commit.Len() == 40 && Commit == PinguFair::CommitOf(Secret, 7) && Commit != PinguFair::CommitOf(Secret, 8)
				&& Commit != PinguFair::CommitOf(PinguFair::MakeSecret(), 7) && !Commit.Contains(Secret) && Secret.Len() == 32
				&& PinguFair::IsValidValue(Secret) && !PinguFair::IsValidValue(TEXT("zz")) && Secret != PinguFair::MakeSecret();
			Report(FString::Printf(TEXT("RESULT anticheat commit: 40 hex digits, same for the same secret, another for another serial or secret, secrets random %s"),
				*Ok(bCommitOk)));

			// The seed: every joiner's value counts, their order does not.
			FPinguFairReveal Reveal;
			Reveal.Serial = 7;
			Reveal.Round = 3;
			Reveal.Secret = Secret;
			Reveal.Slots = { 0, 1, 2, 5 };
			FPinguFairContribution A;
			A.Player = 256;
			A.Value = TEXT("0123456789ABCDEF0123456789ABCDEF");
			FPinguFairContribution B;
			B.Player = 257;
			B.Value = TEXT("FEDCBA9876543210FEDCBA9876543210");
			Reveal.Contributions = { A, B };
			const int32 Seed = PinguFair::SeedOf(Reveal);
			FPinguFairReveal Swapped = Reveal;
			Swapped.Contributions = { B, A };
			FPinguFairReveal Changed = Reveal;
			Changed.Contributions[1].Value = TEXT("FEDCBA9876543210FEDCBA9876543211");
			FPinguFairReveal Fewer = Reveal;
			Fewer.Contributions.RemoveAt(1);
			FPinguFairReveal OtherSecret = Reveal;
			OtherSecret.Secret = PinguFair::MakeSecret();
			const bool bSeedOk = Seed == PinguFair::SeedOf(Reveal) && Seed == PinguFair::SeedOf(Swapped) && Seed != PinguFair::SeedOf(Changed)
				&& Seed != PinguFair::SeedOf(Fewer) && Seed != PinguFair::SeedOf(OtherSecret);
			Report(FString::Printf(TEXT("RESULT anticheat seed: the same from the same values in any order, another when a value changes, is left out or the secret changes %s"),
				*Ok(bSeedOk)));

			// The deal: the same on every machine, fair, traits different within a round, forced ones kept.
			FPinguFairDeal First;
			FPinguFairDeal Second;
			PinguFair::Deal(Reveal, First);
			PinguFair::Deal(Reveal, Second);
			bool bSame = First.Slots == Second.Slots && First.Traits == Second.Traits && First.Loadouts.Num() == Second.Loadouts.Num();
			for (int32 Index = 0; bSame && Index < First.Loadouts.Num(); ++Index)
			{
				bSame = First.Loadouts[Index] == Second.Loadouts[Index];
			}
			Report(FString::Printf(TEXT("RESULT anticheat deal same twice: %d penguins, slot 0 %s %s"), First.Slots.Num(),
				*FString::JoinBy(First.Loadouts.IsValidIndex(0) ? First.Loadouts[0] : TArray<EPenguinAbility>(), TEXT(", "),
					[](EPenguinAbility Ability) { return FString(UPenguinAbilityComponent::GetInfo(Ability).Name); }), *Ok(bSame && First.Slots.Num() == 4)));

			FPinguFairReveal Traits = Reveal;
			Traits.bTraits = true;
			Traits.Slots = { 0, 1, 2, 3 };
			int32 Unfair = 0;
			int32 RepeatsInRound = 0;
			int32 WrongCount = 0;
			int32 RoundsToSeeAll = -1;
			int32 MixedRounds = 0;
			TSet<EPinguTrait> SeenTraits;
			for (int32 Round = 1; Round <= 80; ++Round)
			{
				Traits.Round = Round;
				Traits.Serial = 100 + Round;
				FPinguFairDeal Deal;
				PinguFair::Deal(Traits, Deal);
				TSet<EPinguTrait> InRound;
				for (int32 Index = 0; Index < Deal.Slots.Num(); ++Index)
				{
					InRound.Add(Deal.Traits[Index]);
					SeenTraits.Add(Deal.Traits[Index]);
					TArray<EPenguinAbility> Owned;
					for (const EPenguinAbility Ability : Deal.Loadouts[Index])
					{
						if (Ability != EPenguinAbility::None)
						{
							Owned.Add(Ability);
						}
					}
					Unfair += TSet<EPenguinAbility>(Owned).Num() == Owned.Num() ? 0 : 1; // no ability twice (the rule: random, no duplicates)
					WrongCount += Owned.Num() == PinguTraits::GetInfo(Deal.Traits[Index]).N.NumAbilities ? 0 : 1;
				}
				// (2026-11-07, shared-traits: everybody has the round's one trait; none comes twice before all of them came.)
				MixedRounds += InRound.Num() == 1 ? 0 : 1;
				RepeatsInRound += Round <= PinguNumTraits && SeenTraits.Num() != Round ? 1 : 0;
				if (RoundsToSeeAll < 0 && SeenTraits.Num() == PinguNumTraits)
				{
					RoundsToSeeAll = Round;
				}
				// The match's bag carries over to the next round (as the host keeps it).
				Traits.TraitBag.Reset();
				for (const EPinguTrait Trait : Deal.BagAfter)
				{
					Traits.TraitBag.Add(static_cast<uint8>(Trait));
				}
			}
			// One trait a round for all 4: every one of them by round PinguNumTraits (the bag: every trait once before any comes again).
			Report(FString::Printf(TEXT("RESULT anticheat deal traits: 80 rounds of 4, %d loadouts with an ability twice, %d with the wrong number of abilities, %d rounds with different traits, %d repeats before the bag was empty, all %d traits by round %d (want %d) %s"),
				Unfair, WrongCount, MixedRounds, RepeatsInRound, PinguNumTraits, RoundsToSeeAll, PinguNumTraits,
				*Ok(Unfair == 0 && WrongCount == 0 && MixedRounds == 0 && RepeatsInRound == 0 && RoundsToSeeAll == PinguNumTraits)));

			// The host's checklist: only the ticked traits come (three ticked: each once in every three rounds).
			{
				FPinguFairReveal Ticked = Traits;
				Ticked.TraitBag.Reset();
				const EPinguTrait Allowed[] = { EPinguTrait::Giant, EPinguTrait::LowGravity, EPinguTrait::Jester };
				Ticked.TraitsOff = ~uint64(0);
				for (const EPinguTrait Trait : Allowed)
				{
					Ticked.TraitsOff &= ~(uint64(1) << static_cast<int32>(Trait));
				}
				int32 Outside = 0;
				int32 BadTriples = 0;
				TArray<EPinguTrait> Triple;
				for (int32 Round = 1; Round <= 12; ++Round)
				{
					Ticked.Round = Round;
					Ticked.Serial = 300 + Round;
					FPinguFairDeal Deal;
					PinguFair::Deal(Ticked, Deal);
					const EPinguTrait Got = Deal.Traits.Num() > 0 ? Deal.Traits[0] : EPinguTrait::None;
					Outside += Got == Allowed[0] || Got == Allowed[1] || Got == Allowed[2] ? 0 : 1;
					Triple.AddUnique(Got);
					if (Round % 3 == 0)
					{
						BadTriples += Triple.Num() == 3 ? 0 : 1;
						Triple.Reset();
					}
					Ticked.TraitBag.Reset();
					for (const EPinguTrait Trait : Deal.BagAfter)
					{
						Ticked.TraitBag.Add(static_cast<uint8>(Trait));
					}
				}
				Report(FString::Printf(TEXT("RESULT anticheat deal ticked traits: 3 of %d ticked, 12 rounds: %d outside them, %d threes with a repeat %s"),
					PinguNumTraits, Outside, BadTriples, *Ok(Outside == 0 && BadTriples == 0)));
			}

			FPinguFairReveal Forced = Traits;
			Forced.ForcedTraits.Init(static_cast<uint8>(EPinguTrait::None), 3);
			Forced.ForcedTraits[1] = static_cast<uint8>(EPinguTrait::Giant);
			Forced.ForcedTraits[2] = static_cast<uint8>(EPinguTrait::SecondChance);
			FPinguFairDeal ForcedDeal;
			PinguFair::Deal(Forced, ForcedDeal);
			const int32 Second2 = ForcedDeal.IndexOf(2);
			// (Slot 0 forced to none; slot 3, not forced, gets the round's drawn trait.)
			const bool bForcedOk = ForcedDeal.Traits[ForcedDeal.IndexOf(1)] == EPinguTrait::Giant && ForcedDeal.Traits[Second2] == EPinguTrait::SecondChance
				&& ForcedDeal.Loadouts[Second2][1] == EPenguinAbility::None && ForcedDeal.Traits[ForcedDeal.IndexOf(0)] == EPinguTrait::None
				&& PinguTraits::IsReal(ForcedDeal.Traits[ForcedDeal.IndexOf(3)]);
			Report(FString::Printf(TEXT("RESULT anticheat deal forced traits: slot 1 %s, slot 2 %s (middle slot empty), slot 3 a drawn %s %s"),
				*PinguTraits::GetLogName(ForcedDeal.Traits[ForcedDeal.IndexOf(1)]), *PinguTraits::GetLogName(ForcedDeal.Traits[Second2]),
				*PinguTraits::GetLogName(ForcedDeal.Traits[ForcedDeal.IndexOf(3)]), *Ok(bForcedOk)));

			// No ability favoured: 3000 deals of one penguin.
			TMap<EPenguinAbility, int32> Counts;
			FPinguFairReveal Many = Reveal;
			Many.Slots = { 0 };
			for (int32 Index = 0; Index < 3000; ++Index)
			{
				Many.Serial = Index;
				FPinguFairDeal Deal;
				PinguFair::Deal(Many, Deal);
				for (const EPenguinAbility Ability : Deal.Loadouts[0])
				{
					++Counts.FindOrAdd(Ability);
				}
			}
			int32 Least = MAX_int32;
			int32 Most = 0;
			for (int32 Value = 1; Value < static_cast<int32>(EPenguinAbility::Count); ++Value)
			{
				const int32 Count = Counts.FindRef(static_cast<EPenguinAbility>(Value));
				Least = FMath::Min(Least, Count);
				Most = FMath::Max(Most, Count);
			}
			const float Mean = 9000.f / (static_cast<int32>(EPenguinAbility::Count) - 1);
			Report(FString::Printf(TEXT("RESULT anticheat deal spread: 3000 deals, every ability %d to %d times (mean %.0f; want each above a third of it) %s"),
				Least, Most, Mean, *Ok(Least > Mean / 3.f)));
		},
		[](float) {}, [] {});

	// ---------------------------------------------------------------- the host's gates
	const TSharedRef<double> GateStart = MakeShared<double>(0.0);
	AddStep(TEXT("anticheat_HostGates"), 0.6f,
		[this, GateStart]
		{
			// The rate limit: a burst of 40 ability requests passes, the 41st does not.
			int32 Passed = 0;
			for (int32 Index = 0; Index < 41; ++Index)
			{
				Passed += PinguAntiCheat::AllowRequest(this, PinguAntiCheat::EGate::Ability) ? 1 : 0;
			}
			*GateStart = FPlatformTime::Seconds();
			const int32 Ignored = PinguAntiCheat::GetIgnoredCount();
			Report(FString::Printf(TEXT("RESULT anticheat rate limit: %d of 41 requests in a burst pass (want 40) %s"), Passed, *Ok(Passed == 40)));

			// An icicle hold: never longer than the host saw it (+0.35 s).
			const float Cut = PinguAntiCheat::ClampHold(this, 5.f, 1.0);
			const float Kept = PinguAntiCheat::ClampHold(this, 1.2f, 1.0);
			Report(FString::Printf(TEXT("RESULT anticheat icicle hold: 5 s claimed after 1 s cut to %.2f (want 1.35), 1.2 s kept %.2f %s"), Cut, Kept,
				*Ok(FMath::IsNearlyEqual(Cut, 1.35f, 0.01f) && FMath::IsNearlyEqual(Kept, 1.2f, 0.01f) && PinguAntiCheat::GetIgnoredCount() == Ignored + 1)));

			// A joiner's moves, 60 a second (made-up host clock).
			UObject* Joiner = GetWorld();
			double Now = 1000.0;
			FVector Server(0.f, 0.f, 100.f);
			bool bPull = false;
			int32 LegalRefused = 0;
			const auto Move = [&](const FVector& To, bool bWalking, bool bMovedByHost, bool bPushed)
			{
				Now += 1.0 / 60.0;
				const bool bTaken = PinguAntiCheat::AcceptMove(Joiner, Server, To, bWalking, bMovedByHost, bPushed, Now, bPull);
				if (bTaken)
				{
					Server = To;
				}
				return bTaken;
			};
			// Walking (560 cm/s) for a second, a dash (1900) for 0.3 s, walking again, a jump and a fall (2200).
			for (int32 Frame = 0; Frame < 60; ++Frame)
			{
				LegalRefused += Move(Server + FVector(560.f / 60.f, 0.f, 0.f), true, false, false) ? 0 : 1;
			}
			for (int32 Frame = 0; Frame < 18; ++Frame)
			{
				LegalRefused += Move(Server + FVector(1900.f / 60.f, 0.f, 0.f), true, false, false) ? 0 : 1;
			}
			for (int32 Frame = 0; Frame < 60; ++Frame)
			{
				LegalRefused += Move(Server + FVector(560.f / 60.f, 0.f, (Frame < 30 ? 1100.f : -2200.f) / 60.f), Frame >= 50, false, false) ? 0 : 1;
			}
			// The host teleports it (a warp): taken while the host's move is on its way; a knockback flight at 8000 cm/s after a push.
			LegalRefused += Move(Server + FVector(2500.f, 0.f, 0.f), false, true, false) ? 0 : 1;
			for (int32 Frame = 0; Frame < 30; ++Frame)
			{
				LegalRefused += Move(Server + FVector(8000.f / 60.f, 0.f, 0.f), Frame > 20, false, true) ? 0 : 1;
			}
			Report(FString::Printf(TEXT("RESULT anticheat legal moves: walking, a dash, a jump and fall, a host teleport, a knockback flight: %d refused (want 0) %s"),
				LegalRefused, *Ok(LegalRefused == 0)));

			// A teleport of 30 m: refused, again and again, until the host pulls it back after 0.5 s.
			const FVector Before = Server;
			int32 Refused = 0;
			int32 PullFrame = -1;
			for (int32 Frame = 0; Frame < 40; ++Frame)
			{
				if (!Move(Before + FVector(3000.f, 0.f, 0.f), false, false, false))
				{
					++Refused;
				}
				if (bPull && PullFrame < 0)
				{
					PullFrame = Frame;
				}
			}
			Report(FString::Printf(TEXT("RESULT anticheat teleport: %d of 40 moves refused, the host stayed at x %.0f, pulled back at frame %d (want about 30) %s"),
				Refused, Server.X, PullFrame, *Ok(Refused == 40 && FMath::IsNearlyEqual(Server.X, Before.X, 1.0) && PullFrame >= 28 && PullFrame <= 32)));

			// A run at 5000 cm/s on the ground: refused once a 0.6 s window shows it.
			int32 SpeedRefused = 0;
			for (int32 Frame = 0; Frame < 60; ++Frame)
			{
				SpeedRefused += Move(Server + FVector(5000.f / 60.f, 0.f, 0.f), true, false, false) ? 0 : 1;
			}
			Report(FString::Printf(TEXT("RESULT anticheat speed run: 5000 cm/s on the ground for a second: %d moves refused (want 1 or more) %s"), SpeedRefused,
				*Ok(SpeedRefused >= 1)));
		},
		[](float) {},
		[this, GateStart]
		{
			// The bucket refilled (40 a second) while the step ran.
			const double Elapsed = FPlatformTime::Seconds() - *GateStart;
			int32 Passed = 0;
			for (int32 Index = 0; Index < 60; ++Index)
			{
				Passed += PinguAntiCheat::AllowRequest(this, PinguAntiCheat::EGate::Ability) ? 1 : 0;
			}
			const int32 Want = FMath::Min(40, FMath::FloorToInt(Elapsed * 40.0));
			Report(FString::Printf(TEXT("RESULT anticheat rate refill: %.2f s later %d requests pass (want about %d) %s"), Elapsed, Passed, Want,
				*Ok(FMath::Abs(Passed - Want) <= 2)));
		});

	// The host's own rules on our penguin (what a joiner's request reaches through UseAbility).
	AddStep(TEXT("anticheat_HostRules"), 0.5f,
		[this]
		{
			SpawnTarget();
			PrepareBrawl(900.f, { EPenguinAbility::IceShell, EPenguinAbility::Noot, EPenguinAbility::IceWall });
		},
		[this](float T)
		{
			UPenguinAbilityComponent* Abilities = Penguin.IsValid() ? Penguin->GetAbilities() : nullptr;
			UPenguinMovementComponent* PenguinMove = Penguin.IsValid() ? Penguin->GetPenguinMovement() : nullptr;
			if (!Abilities || !PenguinMove || CountA > 0 || T < 0.1f)
			{
				return;
			}
			CountA = 1;
			const FVector Aim = Penguin->GetAimOrigin() + FVector(400.f, 0.f, 0.f);
			const bool bFirst = Abilities->UseAbility(0, Aim);
			const bool bAgain = Abilities->UseAbility(0, Aim);
			const bool bNoSlot = Abilities->UseAbility(7, Aim);
			PenguinMove->SetFrozen(1.f);
			const bool bFrozen = Abilities->UseAbility(1, Aim);
			PenguinMove->SetFrozen(0.f);
			Report(FString::Printf(TEXT("RESULT anticheat host rules: a use %d, the same on its cooldown %d, a slot it has not %d, frozen %d (want 1 0 0 0) %s"),
				bFirst ? 1 : 0, bAgain ? 1 : 0, bNoSlot ? 1 : 0, bFrozen ? 1 : 0, *Ok(bFirst && !bAgain && !bNoSlot && !bFrozen)));
		},
		[] {});

	// ---------------------------------------------------------------- the joiner's judge
	AddStep(TEXT("anticheat_Judge"), 0.1f,
		[this]
		{
			FRandomStream Rand(4242);
			// ---- a legal stream: 60 s of a fight, the host's clock, the joiner looking at random moments with a clock error
			{
				FPinguCheatJudge Judge;
				const TArray<EPenguinAbility> Loadout = { EPenguinAbility::Snowball, EPenguinAbility::IceWall, EPenguinAbility::GiantSnowball };
				FPinguSeenPenguin Seen = MakeSeen(2, Loadout);
				FPinguSeenPenguin Leech = MakeSeen(3, { EPenguinAbility::FlipperSlap, EPenguinAbility::Gust, EPenguinAbility::FrostBreath }, EPinguTrait::Leech);
				// The one Leech and the snowball hit (its hits give the snowball back, cut Leech's cooldowns).
				FPinguSeenPenguin Victim = MakeSeen(4, { EPenguinAbility::Noot, EPenguinAbility::Gust, EPenguinAbility::IceGun });
				Victim.Position = FVector(3000.f, 0.f, 100.f);
				FPinguFairDeal Deal = OneDeal(2, Loadout);
				Deal.Slots.Add(3);
				Deal.Traits.Add(EPinguTrait::Leech);
				Deal.Loadouts.Add({ EPenguinAbility::FlipperSlap, EPenguinAbility::Gust, EPenguinAbility::FrostBreath });
				Deal.Slots.Add(4);
				Deal.Traits.Add(EPinguTrait::None);
				Deal.Loadouts.Add(Victim.Slots.Num() == 3 ? TArray<EPenguinAbility>{ EPenguinAbility::Noot, EPenguinAbility::Gust, EPenguinAbility::IceGun } : TArray<EPenguinAbility>());
				double Host = 500.0;
				double Client = 20.0;
				Judge.SetRound(4, false, 0.f, Client);
				Judge.ExpectDeal(4, Deal);
				Judge.Observe(Client, Host, Seen); // the countdown: the deal is seen
				Judge.Observe(Client, Host, Leech);
				Judge.Observe(Client, Host, Victim);
				float FightAge = 0.f;
				double NextUse[3] = { Host + 0.3, Host + 0.5, Host + 1.0 };
				double LeechNext = Host + 0.2;
				bool bRefunded = false;
				bool bBubble = false;
				bool bRecharge = false;
				bool bGiantCancel = false;
				bool bDouble = false;
				bool bWarp = false;
				int32 Shots = 0;
				// Movement: the bot walks, dashes, is hit and flies, warps.
				FVector At(0.f, 0.f, 100.f);
				double Motion = Host;
				int32 Hits = 0;
				Seen.bJudgeMotion = true;
				while (FightAge < 60.f)
				{
					const double Step = Rand.FRandRange(0.016f, 0.2f); // the joiner's frames, and late updates
					Host += Step;
					Client += Step;
					FightAge += static_cast<float>(Step);
					if (!bDouble && FightAge > 30.f && Host >= NextUse[1])
					{
						// A slow update (the host's frame, the network): two ice walls in it, the second a cooldown after the first.
						bDouble = true;
						const float Rule = RuleOf(Seen.Slots[1].Ability);
						Use(Seen, 1, Host, Rule);
						Use(Seen, 1, Host + Rule, Rule);
						Host += Rule + 0.05;
						Client += Rule + 0.05;
						FightAge += Rule + 0.05f;
						NextUse[1] = Seen.Slots[1].End;
					}
					// The host's side: uses at once when ready (a little later at random).
					for (int32 Slot = 0; Slot < 3; ++Slot)
					{
						if (Host >= NextUse[Slot])
						{
							const float Rule = RuleOf(Seen.Slots[Slot].Ability);
							Use(Seen, Slot, NextUse[Slot], Rule);
							NextUse[Slot] = Seen.Slots[Slot].End + Rand.FRandRange(0.f, 0.5f);
							if (Slot == 0 && Seen.Slots[0].Ability == EPenguinAbility::Snowball)
							{
								// Its projectile shows up a moment later (the shot check: held, and counted).
								Judge.NoteShot(Client + 0.05, Host + 0.05, Seen.Slot, EPenguinAbility::Snowball);
								++Shots;
							}
							if (Slot == 0 && !bRefunded && FightAge > 10.f && Seen.Slots[0].Ability == EPenguinAbility::Snowball)
							{
								// The snowball hit at once: given back (end 0), thrown again right away (in the same update).
								bRefunded = true;
								++Victim.HitEvents;
								Seen.Slots[0].End = 0.0;
								Use(Seen, 0, NextUse[0] - Rule, Rule);
							}
							if (Slot == 2 && !bGiantCancel && FightAge > 20.f)
							{
								// The giant snowball's roll-up let go early: its cooldown starts again from now (sooner than planned).
								bGiantCancel = true;
								Judge.Observe(Client, Host, Seen);
								Seen.Slots[2].End = Host + 0.5 + Seen.Slots[2].Length;
								NextUse[2] = Seen.Slots[2].End;
							}
						}
					}
					if (!bBubble && FightAge > 40.f)
					{
						// A bubble taken: the middle slot is a Noot now (the bubble went away 0.3 s before we see the slot, right here).
						bBubble = true;
						Judge.NotePickupGone(EPenguinAbility::Noot, Client - 0.3, At + FVector(120.f, 0.f, 0.f));
						Seen.Slots[1].Ability = EPenguinAbility::Noot;
						++Seen.Slots[1].Swaps;
						Seen.Slots[1].End = 0.0;
						NextUse[1] = Host + 0.4;
					}
					if (!bRecharge && FightAge > 45.f && Seen.Slots[2].End > Host + 1.0)
					{
						// A giant snowball bubble taken while its cooldown runs: recharged (the bubble went away a moment after).
						bRecharge = true;
						++Seen.Slots[2].Swaps;
						Seen.Slots[2].End = 0.0;
						Judge.Observe(Client, Host, Seen);
						Judge.NotePickupGone(EPenguinAbility::GiantSnowball, Client + 0.1, At);
						NextUse[2] = Host + 0.3;
					}
					if (Host >= LeechNext)
					{
						Use(Leech, 0, LeechNext, RuleOf(EPenguinAbility::FlipperSlap, EPinguTrait::Leech));
						LeechNext = Leech.Slots[0].End - 2.0 + Rand.FRandRange(0.f, 0.3f);
						// It hit: Leech takes 2 s off (an end cut while it runs; its replicated cut counter goes up).
						Judge.Observe(Client, Host, Leech);
						Leech.Slots[0].End -= 2.0;
						++Leech.LeechCuts;
						++Victim.HitEvents;
					}
					// Motion: walking, a dash every 5 s, a hit every 12 s and a fast flight, a warp at 50 s (with its use).
					const float Phase = FMath::Fmod(FightAge, 12.f);
					float Speed = 560.f;
					bool bWalking = true;
					if (FMath::Fmod(FightAge, 5.f) < 0.2f)
					{
						Speed = 1900.f;
					}
					if (Phase > 11.f)
					{
						if (Seen.HitEvents == Hits)
						{
							++Seen.HitEvents;
						}
						Speed = 8000.f;
						bWalking = false;
					}
					else
					{
						Hits = Seen.HitEvents;
					}
					At.X += Speed * Step * (FMath::Fmod(FightAge, 20.f) < 10.f ? 1.f : -1.f);
					if (!bWarp && FightAge > 50.f && Seen.Slots.IsValidIndex(1))
					{
						bWarp = true;
						Seen.Slots[0].Ability = EPenguinAbility::SnowWarp;
						++Seen.Slots[0].Swaps; // (a bubble gave the warp a moment before: ready at once, as GiveAbility makes it)
						Seen.Slots[0].End = 0.0;
						Judge.NotePickupGone(EPenguinAbility::SnowWarp, Client, At);
						Seen.Position = At;
						Judge.Observe(Client, Host, Seen);
						Use(Seen, 0, Host, RuleOf(EPenguinAbility::SnowWarp));
						NextUse[0] = Seen.Slots[0].End;
						At.X += 2500.f;
					}
					Motion = Host - 0.03;
					Seen.bNewSample = true;
					Seen.MotionTime = Motion;
					Seen.Location = At;
					Seen.Position = At;
					Seen.bWalking = bWalking;
					Judge.SetRound(4, true, FightAge, Client);
					// The joiner's clock is a little off the host's (it only matters for slow updates).
					Judge.Observe(Client, Host + Rand.FRandRange(-0.2f, 0.2f), Seen);
					Judge.Observe(Client, Host, Leech);
					Judge.Observe(Client, Host, Victim);
					Judge.Tick(Client);
				}
				Report(FString::Printf(TEXT("RESULT anticheat judge legal: 60 s, %d deals verified, %d uses judged, %d shots, %d motion samples, a refund, a bubble, a recharge, a giant let go, Leech, two uses in one update, hits, a warp: %d points (want 0)%s %s"),
					Judge.GetDealsVerified(), Judge.GetUsesJudged(), Shots, Judge.GetSamplesJudged(), Judge.GetPoints(),
					Judge.GetStrikes().Num() > 0 ? *FString::Printf(TEXT(" - %s"), *Judge.GetStrikes()[0]) : TEXT(""),
					*Ok(Judge.GetPoints() == 0 && Judge.GetDealsVerified() == 3 && Judge.GetUsesJudged() > 20 && Judge.GetSamplesJudged() > 100 && bDouble
						&& bRefunded && bGiantCancel && bWarp && bRecharge && Shots > 10)));
			}

			// ---- the cheats, each in a judge of its own. Runs the made-up fight for Seconds with a use of slot 0 whenever
			// UseEvery has passed (the cheat's cooldown) and the given length / end; returns the judge's points.
			// bTrainerZero: the end is set as the rules say and then written to 0 before the next update (PinguTrainer.exe).
			const auto Fight = [&Rand](FPinguCheatJudge& Judge, FPinguSeenPenguin& Seen, float Seconds, float UseEvery, float Length, bool bSetEnd,
				bool bTrainerZero = false)
			{
				double Host = 100.0;
				double Client = 10.0;
				float Age = 0.f;
				double Next = Host + 0.2;
				Judge.SetRound(1, true, 2.f, Client);
				Judge.Observe(Client, Host, Seen);
				while (Age < Seconds)
				{
					const double Step = Rand.FRandRange(0.016f, 0.05f);
					Host += Step;
					Client += Step;
					Age += static_cast<float>(Step);
					if (Host >= Next)
					{
						FPinguSeenSlot& Entry = Seen.Slots[0];
						++Entry.Uses;
						Entry.Length = Length;
						if (bSetEnd)
						{
							Entry.End = Next + Length;
						}
						Next += UseEvery;
						if (bTrainerZero)
						{
							Judge.SetRound(1, true, 2.f + Age, Client);
							Judge.Observe(Client, Host, Seen); // (this update still had the end)
							Entry.End = 0.0;
						}
					}
					Judge.SetRound(1, true, 2.f + Age, Client);
					Judge.Observe(Client, Host, Seen);
					Judge.Tick(Client);
				}
			};
			const TArray<EPenguinAbility> Loadout = { EPenguinAbility::IceWall, EPenguinAbility::Noot, EPenguinAbility::SnowBomb };
			const float WallRule = RuleOf(EPenguinAbility::IceWall);
			struct FCase
			{
				const TCHAR* Name;
				int32 Points = 0;
				bool bLeft = false;
				FString First;
			};
			TArray<FCase> Cases;
			const auto Note = [&Cases](const TCHAR* Name, const FPinguCheatJudge& Judge)
			{
				FCase Case;
				Case.Name = Name;
				Case.Points = Judge.GetPoints();
				Case.bLeft = Judge.ShouldLeave();
				Case.First = Judge.GetStrikes().Num() > 0 ? Judge.GetStrikes()[0] : FString(TEXT("nothing seen"));
				Cases.Add(Case);
			};
			// A round of a made-up fight: the deal shows in the countdown, then (bFightAfter) Seconds of the fight with Seen.
			const auto Round = [](FPinguCheatJudge& Judge, int32 RoundNumber, const FPinguFairDeal& Deal, const FPinguSeenPenguin& Countdown,
				const FPinguSeenPenguin& Fighting, double Client, float Seconds)
			{
				Judge.SetRound(RoundNumber, false, 0.f, Client);
				Judge.ExpectDeal(RoundNumber, Deal);
				Judge.Observe(Client, 50.0 + Client, Countdown);
				Judge.Tick(Client);
				for (float Age = 0.f; Age < Seconds; Age += 0.05f)
				{
					Judge.SetRound(RoundNumber, true, Age, Client + 1.0 + Age);
					Judge.Observe(Client + 1.0 + Age, 51.0 + Client + Age, Fighting);
					Judge.Tick(Client + 1.0 + Age);
				}
			};
			{
				// PinguTrainer.exe's picks: the deal shows first (the countdown), then the host's own three for the whole fight.
				FPinguCheatJudge Judge;
				const FPinguSeenPenguin Dealt = MakeSeen(0, Loadout);
				const FPinguSeenPenguin Picked = MakeSeen(0, { EPenguinAbility::SnowBomb, EPenguinAbility::IceGun, EPenguinAbility::Icicle });
				Round(Judge, 2, OneDeal(0, Loadout), Dealt, Picked, 1.0, 5.f);
				Note(TEXT("trainer picks after the deal showed"), Judge);
			}
			{
				// One ability picked, its swap counter bumped as a bubble's would be (the old judge excused that slot).
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Picked = MakeSeen(0, { EPenguinAbility::IceWall, EPenguinAbility::Icicle, EPenguinAbility::SnowBomb });
				Picked.Slots[1].Swaps = 1;
				Round(Judge, 2, OneDeal(0, Loadout), MakeSeen(0, Loadout), Picked, 1.0, 10.f);
				Note(TEXT("one pick with a swap count, 10 s"), Judge);
			}
			{
				// A bubble of the picked ability went away - but far from the penguin.
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Picked = MakeSeen(0, { EPenguinAbility::IceWall, EPenguinAbility::Icicle, EPenguinAbility::SnowBomb });
				Picked.Slots[1].Swaps = 1;
				Judge.NotePickupGone(EPenguinAbility::Icicle, 3.0, FVector(5000.f, 0.f, 0.f));
				Round(Judge, 2, OneDeal(0, Loadout), MakeSeen(0, Loadout), Picked, 1.0, 10.f);
				Note(TEXT("a pick with a bubble far away, 10 s"), Judge);
			}
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, Loadout);
				Fight(Judge, Seen, 4.f, 0.5f, WallRule, false); // used every 0.5 s, its end never set
				Note(TEXT("no cooldown for 4 s"), Judge);
			}
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, Loadout);
				Fight(Judge, Seen, 6.f, 0.3f, WallRule, true, true); // PinguTrainer.exe: the end written to 0 after every use
				Note(TEXT("trainer zeroes the cooldown"), Judge);
			}
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, { EPenguinAbility::Snowball, EPenguinAbility::Noot, EPenguinAbility::SnowBomb });
				Fight(Judge, Seen, 6.f, 0.15f, RuleOf(EPenguinAbility::Snowball), true, true); // snowballs every 0.15 s, no hits
				Note(TEXT("snowball spam with no hits"), Judge);
			}
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, Loadout);
				Fight(Judge, Seen, 15.f, WallRule * 0.3f, WallRule * 0.3f, true); // a cooldown of a third
				Note(TEXT("short cooldown"), Judge);
			}
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, Loadout);
				Fight(Judge, Seen, 15.f, WallRule * 0.5f, WallRule, true); // the right length, used twice as often
				Note(TEXT("early uses"), Judge);
			}
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, Loadout);
				Judge.SetRound(1, true, 2.f, 1.0);
				Judge.Observe(1.0, 50.0, Seen);
				for (int32 Index = 0; Index < 3; ++Index)
				{
					// A use counted and then the counter written back (a hidden use).
					++Seen.Slots[0].Uses;
					Seen.Slots[0].End = 51.0 + Index * 4.0 + WallRule;
					Seen.Slots[0].Length = WallRule;
					Judge.Observe(2.0 + Index * 4.0, 51.0 + Index * 4.0, Seen);
					--Seen.Slots[0].Uses;
					Judge.Observe(2.1 + Index * 4.0, 51.1 + Index * 4.0, Seen);
					Judge.Tick(2.1 + Index * 4.0);
				}
				Note(TEXT("use counter written back"), Judge);
			}
			{
				// Shots: snow bombs from a penguin that never had one (four: the first two only noted), then a held ability's
				// shots the host never counted.
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, { EPenguinAbility::Snowball, EPenguinAbility::Noot, EPenguinAbility::IceGun });
				Judge.SetRound(1, true, 2.f, 1.0);
				Judge.Observe(1.0, 50.0, Seen);
				for (int32 Index = 0; Index < 4; ++Index)
				{
					Judge.NoteShot(2.0 + Index * 4.0, 51.0 + Index * 4.0, 0, EPenguinAbility::SnowBomb);
					Judge.Observe(4.0 + Index * 4.0, 53.0 + Index * 4.0, Seen);
					Judge.Tick(4.0 + Index * 4.0);
				}
				Note(TEXT("shots of an ability it does not have"), Judge);
			}
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, { EPenguinAbility::Snowball, EPenguinAbility::Noot, EPenguinAbility::IceGun });
				Judge.SetRound(1, true, 2.f, 1.0);
				Judge.Observe(1.0, 50.0, Seen);
				for (int32 Index = 0; Index < 5; ++Index)
				{
					Judge.NoteShot(2.0 + Index * 3.5, 51.0 + Index * 3.5, 0, EPenguinAbility::IceGun);
					Judge.Observe(4.0 + Index * 3.5, 53.0 + Index * 3.5, Seen);
					Judge.Tick(4.0 + Index * 3.5);
				}
				Note(TEXT("shots the host never counted"), Judge);
			}
			{
				// A new round fought with no reveal; and a round dealt twice is CheckReveal's (anticheat_Reveal).
				FPinguCheatJudge Judge;
				Judge.SetRound(5, false, 0.f, 1.0);
				Judge.ExpectRevealFor(5);
				Judge.SetRound(5, true, 1.f, 2.0);
				Judge.SetRound(5, true, 3.5f, 4.5);
				Note(TEXT("a round with no reveal"), Judge);
			}
			{
				// The host's penguin jumps 20 m every 3.3 s.
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, Loadout);
				Seen.bJudgeMotion = true;
				Judge.SetRound(1, true, 2.f, 1.0);
				FVector At(0.f, 0.f, 100.f);
				double Host = 10.0;
				for (int32 Frame = 0; Frame < 900; ++Frame)
				{
					Host += 1.0 / 60.0;
					At.X += (Frame % 200 == 199) ? 2000.f : 560.f / 60.f;
					Seen.bNewSample = true;
					Seen.MotionTime = Host;
					Seen.Location = At;
					Seen.bWalking = true;
					Judge.SetRound(1, true, 2.f + Frame / 60.f, 1.0 + Frame / 60.0);
					Judge.Observe(1.0 + Frame / 60.0, Host, Seen);
				}
				Note(TEXT("teleports"), Judge);
			}
			{
				// The host's penguin runs at 6000 cm/s.
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen = MakeSeen(0, Loadout);
				Seen.bJudgeMotion = true;
				Judge.SetRound(1, true, 2.f, 1.0);
				FVector At(0.f, 0.f, 100.f);
				double Host = 10.0;
				for (int32 Frame = 0; Frame < 900; ++Frame)
				{
					Host += 1.0 / 60.0;
					At.X += 6000.f / 60.f * (Frame % 240 < 120 ? 1.f : -1.f);
					Seen.bNewSample = true;
					Seen.MotionTime = Host;
					Seen.Location = At;
					Seen.bWalking = true;
					Judge.SetRound(1, true, 2.f + Frame / 60.f, 1.0 + Frame / 60.0);
					Judge.Observe(1.0 + Frame / 60.0, Host, Seen);
				}
				Note(TEXT("speed run"), Judge);
			}
			// Every cheat seen and every one makes the joiner leave (since 2026-11-05: the trainer's picks and cooldowns too).
			for (const FCase& Case : Cases)
			{
				Report(FString::Printf(TEXT("RESULT anticheat judge %s: %d points, %s (want at least %d, leaves); first: %s %s"), Case.Name, Case.Points,
					Case.bLeft ? TEXT("leaves") : TEXT("stays"), FPinguCheatJudge::LeaveAt, *Case.First, *Ok(Case.bLeft)));
			}
		},
		[](float) {}, [] {});

	// ---------------------------------------------------------------- the reveal's checks (2026-11-05, host-cheat-fix)
	AddStep(TEXT("anticheat_Reveal"), 0.1f,
		[this]
		{
			FPinguFairReveal Reveal;
			Reveal.Serial = 7;
			Reveal.Round = 3;
			Reveal.Secret = PinguFair::MakeSecret();
			Reveal.Slots = { 0, 1, 2 };
			Reveal.Contributions = { { 11, TEXT("AAAA") }, { 12, TEXT("BBBB") } };
			FPinguRevealCheck Honest;
			Honest.SeenHash = PinguFair::CommitOf(Reveal.Secret, Reveal.Serial);
			Honest.OwnPlayer = 11;
			Honest.OwnValue = TEXT("aaaa");
			Honest.OwnSentAgo = 6.0;
			Honest.KnownJoiners = { 11, 12 };
			Honest.SeenReceived = { { 11, TEXT("AAAA") }, { 12, TEXT("BBBB") } };
			Honest.RoundsDealt = { 1, 2 };
			Honest.PrevRound = 2;
			const auto Points = [](const FPinguFairReveal& R, const FPinguRevealCheck& C)
			{
				int32 Sum = 0;
				for (const TPair<int32, FString>& Problem : PinguFair::CheckReveal(R, C))
				{
					Sum += Problem.Key;
				}
				return Sum;
			};
			struct FRevealCase
			{
				const TCHAR* Name;
				int32 Points;
				int32 Want;
			};
			TArray<FRevealCase> Cases;
			Cases.Add({ TEXT("honest"), Points(Reveal, Honest), 0 });
			{
				FPinguFairReveal R = Reveal;
				R.Secret = PinguFair::MakeSecret();
				Cases.Add({ TEXT("another secret"), Points(R, Honest), FPinguCheatJudge::LeaveAt });
			}
			{
				FPinguFairReveal R = Reveal;
				R.Contributions[0].Value = TEXT("CCCC");
				Cases.Add({ TEXT("our value changed"), Points(R, Honest), FPinguCheatJudge::LeaveAt });
			}
			{
				FPinguFairReveal R = Reveal;
				R.Contributions.Add({ 99, TEXT("1234ABCD") }); // a made-up player: the host grinds the seed with it
				Cases.Add({ TEXT("a made-up value"), Points(R, Honest), FPinguCheatJudge::LeaveAt });
			}
			{
				FPinguFairReveal R = Reveal;
				R.Contributions[1].Value = TEXT("BBBC");
				Cases.Add({ TEXT("another joiner's value changed"), Points(R, Honest), FPinguCheatJudge::LeaveAt });
			}
			{
				FPinguFairReveal R = Reveal;
				R.Contributions.RemoveAt(1);
				Cases.Add({ TEXT("a received value left out"), Points(R, Honest), FPinguCheatJudge::LeaveAt });
			}
			{
				FPinguRevealCheck C = Honest;
				C.RoundsDealt.Add(3);
				Cases.Add({ TEXT("the round dealt again"), Points(Reveal, C), FPinguCheatJudge::LeaveAt });
			}
			{
				FPinguFairReveal R = Reveal;
				R.bTraits = true;
				R.TraitBag = { 3, 4 };
				FPinguRevealCheck C = Honest;
				C.bPrevTraits = true;
				C.PrevBagAfter = { 3, 4, 5 };
				Cases.Add({ TEXT("another trait bag"), Points(R, C), FPinguCheatJudge::LeaveAt });
				C.PrevBagAfter = { 3, 4 };
				Cases.Add({ TEXT("the same trait bag"), Points(R, C), 0 });
			}
			{
				FPinguFairReveal R = Reveal;
				R.Contributions.RemoveAt(0);
				FPinguRevealCheck C = Honest;
				C.SeenReceived.Remove(11);
				Cases.Add({ TEXT("our value left out (crossed)"), Points(R, C), 1 });
			}
			{
				FPinguRevealCheck C = Honest;
				C.SeenHash.Reset();
				FPinguFairReveal R = Reveal;
				R.Secret = TEXT("0");
				Cases.Add({ TEXT("a commit from before we came"), Points(R, C), 0 });
			}
			for (const FRevealCase& Case : Cases)
			{
				Report(FString::Printf(TEXT("RESULT anticheat reveal %s: %d points (want %d) %s"), Case.Name, Case.Points, Case.Want, *Ok(Case.Points == Case.Want)));
			}
			const bool bSteps = PinguFair::IsCommitStepFine(0, 5, 0) && PinguFair::IsCommitStepFine(4, 5, 4) && !PinguFair::IsCommitStepFine(4, 6, 4)
				&& !PinguFair::IsCommitStepFine(4, 5, 3);
			Report(FString::Printf(TEXT("RESULT anticheat commit steps: the first, the next after its reveal fine; one skipped, one with no reveal caught %s"), *Ok(bSteps)));
		},
		[](float) {}, [] {});

#if !UE_BUILD_SHIPPING // (the dev trainer is not in the Shipping exe)
	// ---------------------------------------------------------------- the memory guards (2026-11-05, host-cheat-fix)
	// PinguTrainer.exe wrote "Abilities", "CooldownEnds" and the movement tuning by name. The names are gone, the host keeps a
	// hidden shadow of the loadout and the cooldowns, and the movement numbers go back to the class defaults every frame.
	AddStep(TEXT("anticheat_Guard"), 1.6f,
		[this]
		{
			SpawnTarget();
			PrepareBrawl(900.f, { EPenguinAbility::Snowball, EPenguinAbility::IceWall, EPenguinAbility::Noot });
			PinguMemoryGuard::SetForcedOn(true);
			CountA = 0;
			CountB = 0;
			Value = 0.f;
			const UClass* Class = UPenguinAbilityComponent::StaticClass();
			const bool bOldGone = !FindFProperty<FProperty>(Class, TEXT("Abilities")) && !FindFProperty<FProperty>(Class, TEXT("CooldownEnds"))
				&& !FindFProperty<FProperty>(Class, TEXT("CooldownLengths")) && !FindFProperty<FProperty>(Class, TEXT("UseCounts"))
				&& !FindFProperty<FProperty>(Class, TEXT("SwapCounts"));
			const bool bNewThere = FindFProperty<FProperty>(Class, TEXT("Vq")) && FindFProperty<FProperty>(Class, TEXT("Tz"));
			const bool bAcOld = !FindFProperty<FProperty>(APinguAntiCheat::StaticClass(), TEXT("Reveal")) && !FindFProperty<FProperty>(APinguAntiCheat::StaticClass(), TEXT("Commit"));
			Report(FString::Printf(TEXT("RESULT anticheat names: Abilities / CooldownEnds / CooldownLengths / UseCounts / SwapCounts / Commit / Reveal not found by name %d, the new ones replicate %d, %d movement numbers guarded %s"),
				bOldGone && bAcOld ? 1 : 0, bNewThere ? 1 : 0, PinguMemoryGuard::GetWatchedCount(), *Ok(bOldGone && bAcOld && bNewThere && PinguMemoryGuard::GetWatchedCount() > 40)));
		},
		[this](float T)
		{
			UPenguinAbilityComponent* Abilities = Penguin.IsValid() ? Penguin->GetAbilities() : nullptr;
			UPenguinMovementComponent* Move = Penguin.IsValid() ? Penguin->GetPenguinMovement() : nullptr;
			if (!Abilities || !Move || T < 0.1f)
			{
				return;
			}
			// What the trainer wrote last frame must be gone now (the guard ran before this frame's ticks) ...
			const UPenguinMovementComponent* Archetype = Cast<UPenguinMovementComponent>(Move->GetArchetype());
			if (CountB > 0 && Archetype && !FMath::IsNearlyEqual(Move->RunSpeed, Archetype->RunSpeed))
			{
				++CountA; // a frame that still had the trainer's run speed
			}
			// ... and the ice wall (3.5 s cooldown) is used whenever the game lets us: once in 1.5 s.
			const FVector Aim = Penguin->GetAimOrigin() + FVector(600.f, 0.f, 200.f);
			if (Abilities->UseAbility(1, Aim))
			{
				Value += 1.f;
			}
			// The trainer, every 50 ms: Snow Bomb / Ice Gun / Noot, every cooldown 0, run speed x2, 4 air jumps, half gravity.
			if (FMath::Fmod(T, 0.05f) < 0.02f || CountB == 0)
			{
				PinguMemoryGuard::DevTrainerWrite(GetWorld(), Penguin.Get());
				++CountB;
			}
		},
		[this]
		{
			UPenguinAbilityComponent* Abilities = Penguin.IsValid() ? Penguin->GetAbilities() : nullptr;
			if (Abilities)
			{
				Abilities->GuardShadow();
			}
			const bool bLoadout = Abilities && Abilities->GetAbility(0) == EPenguinAbility::Snowball && Abilities->GetAbility(1) == EPenguinAbility::IceWall
				&& Abilities->GetAbility(2) == EPenguinAbility::Noot;
			const int32 Restores = Abilities ? Abilities->GetShadowRestores() : 0;
			Report(FString::Printf(TEXT("RESULT anticheat guard: %d trainer writes, ice wall used %d time(s) in 1.5 s (want 1), loadout as dealt %d, %d loadout / cooldown restores, %d movement numbers put back, %d frame(s) kept the trainer's run speed (want 0) %s"),
				CountB, FMath::RoundToInt32(Value), bLoadout ? 1 : 0, Restores, PinguMemoryGuard::GetMovementRestores(), CountA,
				*Ok(CountB > 10 && FMath::RoundToInt32(Value) == 1 && bLoadout && Restores > 5 && PinguMemoryGuard::GetMovementRestores() > 10 && CountA == 0)));
			PinguMemoryGuard::SetForcedOn(false);
		});

#endif

	// ---------------------------------------------------------------- names (2026-10-30, session hardening)
	AddStep(TEXT("anticheat_Names"), 0.1f,
		[this]
		{
			using namespace PinguNames;
			// Cleaning: trimmed, no control / zero-width / direction characters, one plain space, "?" gone, at most 20 characters.
			const bool bClean = Sanitize(TEXT("  Frosty  ")) == TEXT("Frosty") && Sanitize(TEXT("Fro\u200Bsty")) == TEXT("Frosty")
				&& Sanitize(TEXT("Fro\u0001sty\u007F")) == TEXT("Frosty") && Sanitize(TEXT("A\u202Eb\uFEFF")) == TEXT("Ab")
				&& Sanitize(TEXT("a   b	c\u00A0d")) == TEXT("a b c d") && Sanitize(TEXT("hi?there")) == TEXT("hithere")
				&& Sanitize(TEXT("\u200B\u200C \u0002")).IsEmpty() && Sanitize(FString(TEXT("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"))).Len() == MaxLength
				&& Sanitize(TEXT("Pingu")) == TEXT("Pingu");
			Report(FString::Printf(TEXT("RESULT anticheat names: invisible, control and direction characters dropped, spaces one, length cap %d %s"), MaxLength, *Ok(bClean)));

			// Lookalikes: case, 0 / O, 1 / l / I, Cyrillic and Greek letters, full-width forms, spaces and dots between.
			const FString Frosty = Fold(TEXT("Frosty"));
			const bool bAlike = Fold(TEXT("FROSTY")) == Frosty && Fold(TEXT("Fr0sty")) == Frosty && Fold(TEXT("Fr\u043Esty")) == Frosty
				&& Fold(TEXT("Fr\u03BFsty")) == Frosty && Fold(TEXT("Fr o.s_ty")) == Frosty && Fold(TEXT("\uFF26\uFF52\uFF4F\uFF53\uFF54\uFF59")) == Frosty
				&& Fold(TEXT("Fr\u00F3sty")) == Frosty && Fold(TEXT("Frost\u0443")) == Frosty && Fold(TEXT("Ilya")) == Fold(TEXT("llya"))
				&& Fold(TEXT("Pingu")) == Fold(TEXT("P1ngu")) && Fold(TEXT("modern")) == Fold(TEXT("rnodern"));
			const bool bApart = Fold(TEXT("Pingu")) != Fold(TEXT("Pongu")) && Frosty != Fold(TEXT("Frostie")) && Frosty != Fold(TEXT("Frosty2")) && Fold(TEXT("Bob")) != Fold(TEXT("Rob"));
			Report(FString::Printf(TEXT("RESULT anticheat names: lookalikes (case, 0 / O, l / I / 1, Cyrillic / Greek / full-width letters, spaces) fold to one, other names stay apart %s"),
				*Ok(bAlike && bApart)));

			// Numbers: the later one gets " 2", " 3" ...; names nobody has stay as they are; the cap holds.
			const FString Long = Sanitize(TEXT("abcdefghijklmnopqrst"));
			const FString LongUnique = MakeUnique(Long, { Long });
			const bool bNumbers = MakeUnique(TEXT("Frosty"), { TEXT("Frosty") }) == TEXT("Frosty 2")
				&& MakeUnique(TEXT("Frosty"), { TEXT("Frosty"), TEXT("Frosty 2") }) == TEXT("Frosty 3")
				&& MakeUnique(TEXT("Frosty"), { TEXT("Fr0sty") }) == TEXT("Frosty 2") && MakeUnique(TEXT("Frosty"), { TEXT("Pebble"), TEXT("Waddles") }) == TEXT("Frosty")
				&& MakeUnique(TEXT("Frosty"), {}) == TEXT("Frosty") && LongUnique.Len() <= MaxLength && LongUnique != Long && LongUnique.EndsWith(TEXT(" 2"));
			Report(FString::Printf(TEXT("RESULT anticheat names: the same or a lookalike name gets a number (Frosty 2, Frosty 3), the cap holds (\"%s\") %s"), *LongUnique, *Ok(bNumbers)));
		},
		[](float) {}, [] {});

	// ---------------------------------------------------------------- the join check
	AddStep(TEXT("anticheat_FileCheck"), 12.f,
		[this]
		{
			PinguAntiCheat::StartFileDigest();
			CountA = 0;
		},
		[this](float T)
		{
			if (CountA > 0)
			{
				return;
			}
			FString First;
			if (!PinguAntiCheat::AnswerNonce(TEXT("00112233445566778899AABBCCDDEEFF"), First))
			{
				return; // still hashing
			}
			CountA = 1;
			FString Again;
			FString Other;
			PinguAntiCheat::AnswerNonce(TEXT("00112233445566778899AABBCCDDEEFF"), Again);
			PinguAntiCheat::AnswerNonce(TEXT("00112233445566778899AABBCCDDEEF0"), Other);
			// The ranges a nonce asks for: six stretches inside the files, the same for the same nonce, others for another nonce.
			const TArray<int64> Sizes = { 100000000, 1000, 5000000 };
			const FString RangeNonce = TEXT("00112233445566778899AABBCCDDEEFF");
			const TArray<PinguAntiCheat::FFileRange> Ranges = PinguAntiCheat::DeriveRanges(RangeNonce, Sizes);
			bool bRangesIn = Ranges.Num() == 6;
			for (const PinguAntiCheat::FFileRange& Range : Ranges)
			{
				bRangesIn = bRangesIn && Sizes.IsValidIndex(Range.File) && Range.Offset >= 0 && Range.Length > 0 && Range.Offset + Range.Length <= Sizes[Range.File];
			}
			const TArray<PinguAntiCheat::FFileRange> Same = PinguAntiCheat::DeriveRanges(RangeNonce, Sizes);
			const TArray<PinguAntiCheat::FFileRange> OtherRanges = PinguAntiCheat::DeriveRanges(TEXT("00112233445566778899AABBCCDDEEF0"), Sizes);
			bool bSame = Same.Num() == Ranges.Num();
			bool bOther = false;
			for (int32 Index = 0; Index < FMath::Min(Ranges.Num(), Same.Num()); ++Index)
			{
				bSame = bSame && Same[Index].File == Ranges[Index].File && Same[Index].Offset == Ranges[Index].Offset && Same[Index].Length == Ranges[Index].Length;
			}
			for (int32 Index = 0; Index < FMath::Min(Ranges.Num(), OtherRanges.Num()); ++Index)
			{
				bOther = bOther || OtherRanges[Index].Offset != Ranges[Index].Offset;
			}
			Report(FString::Printf(TEXT("RESULT anticheat file ranges: %d stretches of 64-256 KB all inside the files %d, the same nonce the same ones %d, another nonce others %d, no files none %d %s"),
				Ranges.Num(), bRangesIn ? 1 : 0, bSame ? 1 : 0, bOther ? 1 : 0, PinguAntiCheat::DeriveRanges(RangeNonce, {}).Num() == 0 ? 1 : 0,
				*Ok(bRangesIn && bSame && bOther && PinguAntiCheat::DeriveRanges(RangeNonce, {}).Num() == 0)));
			// (answer: the first 8 hex digits of the answer to the fixed nonce - two builds with the same files print the same,
			// a modified exe or pak another: that is what makes a peer refuse it)
			Report(FString::Printf(TEXT("RESULT anticheat file check: hashed %s after %.1f s, build \"%s\", answer %s; the same nonce the same answer, another nonce another %s"),
				*PinguAntiCheat::DescribeDigest(), T, *PinguAntiCheat::GetBuildId(), *First.Left(8),
				*Ok(First.Len() == 40 && First == Again && First != Other && !PinguAntiCheat::DescribeDigest().IsEmpty())));
		},
		[this]
		{
			if (CountA == 0)
			{
				Report(TEXT("RESULT anticheat file check: our files were not hashed in 12 s FAIL"));
			}
		});

	// ---------------------------------------------------------------- the tamper helper's foreign-DLL logic
	// Pure, no world: PinguTamper::IsForeignModule with a made-up game folder, a made-up shipped-files list and a list of
	// loaded modules. The live OS checks (code section, debugger, enumerating our modules) run only in a packaged
	// Shipping game, so this exercises the one piece that classifies a module - the piece a false positive would hurt.
	AddStep(TEXT("anticheat_Tamper"), 0.1f,
		[]{},
		[this](float)
		{
			const FString Root = TEXT("C:/Game");
			TSet<FString> Shipped;
			for (const TCHAR* Path : {
				TEXT("C:/Game/Pingu Brawl.exe"),
				TEXT("C:/Game/Penguin/Binaries/Win64/Penguin-Win64-Shipping.exe"),
				TEXT("C:/Game/Penguin/Binaries/Win64/tbb12.dll"),
				TEXT("C:/Game/Engine/Binaries/ThirdParty/DbgHelp/dbghelp.dll") })
			{
				Shipped.Add(PinguTamper::NormalizePath(Path));
			}
			struct FCase { const TCHAR* Path; bool bForeign; };
			const FCase Cases[] = {
				// Outside the game folder: never foreign (Windows, drivers, Steam / Discord / NVIDIA overlays).
				{ TEXT("C:/Windows/System32/ntdll.dll"), false },
				{ TEXT("C:/Program Files/NVIDIA Corporation/NvCamera/overlay.dll"), false },
				{ TEXT("C:/Program Files/Discord/overlay.dll"), false },
				// Our own shipped files: fine.
				{ TEXT("C:/Game/Penguin/Binaries/Win64/Penguin-Win64-Shipping.exe"), false },
				{ TEXT("C:/Game/Penguin/Binaries/Win64/tbb12.dll"), false },
				{ TEXT("C:/Game/Engine/Binaries/ThirdParty/DbgHelp/dbghelp.dll"), false },
				// Case and slashes do not matter.
				{ TEXT("c:\\game\\penguin\\binaries\\win64\\tbb12.dll"), false },
				// UE4SS-style proxies dropped into the game folder: foreign.
				{ TEXT("C:/Game/Penguin/Binaries/Win64/dwmapi.dll"), true },
				{ TEXT("C:/Game/Penguin/Binaries/Win64/xinput1_3.dll"), true },
				{ TEXT("C:/Game/Penguin/Binaries/Win64/UE4SS.dll"), true },
				// dbghelp.dll is shipped - but at the engine path; a copy dropped next to the exe is still foreign (a name
				// allowlist would miss this; the per-path check catches it).
				{ TEXT("C:/Game/Penguin/Binaries/Win64/dbghelp.dll"), true },
			};
			int32 Right = 0;
			int32 Total = UE_ARRAY_COUNT(Cases);
			for (const FCase& Case : Cases)
			{
				if (PinguTamper::IsForeignModule(Case.Path, Root, Shipped) == Case.bForeign)
				{
					++Right;
				}
			}
			// A copy without the staging manifests (nothing says what we shipped): only known proxy names are foreign, so
			// every DLL we ship stays fine.
			const FCase NoManifest[] = {
				{ TEXT("C:/Game/Penguin/Binaries/Win64/tbb12.dll"), false },
				{ TEXT("C:/Game/Penguin/Binaries/Win64/D3D12/x64/D3D12Core.dll"), false },
				{ TEXT("C:/Game/Engine/Binaries/ThirdParty/DbgHelp/dbghelp.dll"), false },
				{ TEXT("C:/Game/Engine/Binaries/ThirdParty/Windows/XAudio2_9/x64/xaudio2_9redist.dll"), false },
				{ TEXT("C:/Program Files/Discord/overlay.dll"), false },
				{ TEXT("C:/Game/Penguin/Binaries/Win64/dwmapi.dll"), true },
				{ TEXT("C:/Game/Penguin/Binaries/Win64/UE4SS.dll"), true },
			};
			Total += UE_ARRAY_COUNT(NoManifest);
			for (const FCase& Case : NoManifest)
			{
				if (PinguTamper::IsForeignModule(Case.Path, Root, TSet<FString>(), false) == Case.bForeign)
				{
					++Right;
				}
			}
			// Normalising: backslashes to slashes, lowercased, a trailing slash dropped.
			const bool bNorm = PinguTamper::NormalizePath(TEXT("C:\\Game\\Sub\\")) == TEXT("c:/game/sub")
				&& PinguTamper::NormalizePath(TEXT("C:/Game")) == TEXT("c:/game");
			// A clean build never reports a startup tamper, so its handshake token stays empty (answers match peers).
			const bool bCleanToken = PinguTamper::StartupToken().IsEmpty();
			Report(FString::Printf(TEXT("RESULT anticheat tamper modules: %d of %d classified right, paths normalised %d, clean token empty %d %s"),
				Right, Total, bNorm ? 1 : 0, bCleanToken ? 1 : 0, *Ok(Right == Total && bNorm && bCleanToken)));
		},
		[]{});
}

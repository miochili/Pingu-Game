// Pingu - the ability picks' deal and its anti-cheat (2026-11-07, card ability-picks; PinguAbilityPicks.h). Autotest steps
// (APenguinAutoTest::AddAbilityPicksSteps; filter anticheat_):
//
//  anticheat_PicksDeal    PinguFair::Deal with picks: 30 rounds keep every pick in its slot, unpicked slots roll and never copy a
//                         pick, nobody holds an ability twice; another penguin's loadout does not move with one's picks (a slot
//                         rolls from a stream of its own); RANDOM ignores picks; Second Chance takes the first two picks; Same
//                         Abilities gives everybody one shared roll over the picks; PinguPicks::MakeLoadout with no picks rolls as
//                         RollLoadout (solo RANDOM unchanged); IsValid / the player state's own checks.
//  anticheat_PicksReveal  PinguFair::CheckReveal's picks rules: honest CHOOSE 0 points; picks with RANDOM, a mode the lobby did not
//                         show, a mode changed in the match, a bot with picks, our picks other than we asked, a slot's picks changed
//                         in the match, an ability twice, picks for the wrong number of slots: each 4 (leave); an older ask of ours
//                         and picks gone in the match (a late joiner in the slot) 0.
//  anticheat_PicksJudge   the joiner's judge with a deal of chosen loadouts: a penguin holding its picks gives 0 points; the host's
//                         loadout overwritten outside its picks (and outside the deal) is caught and the joiner leaves.

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PinguAbilityPicks.h"
#include "PinguAntiCheat.h"

namespace PenguinAutoTestAbilityPicksPrivate
{
	FString Ok(bool bOk) { return bOk ? TEXT("ok") : TEXT("FAIL"); }

	TArray<uint8> Bytes(EPenguinAbility A, EPenguinAbility B, EPenguinAbility C)
	{
		return { static_cast<uint8>(A), static_cast<uint8>(B), static_cast<uint8>(C) };
	}

	/** A reveal of three penguins (slots 0, 1, 2) with these picks by slot. */
	FPinguFairReveal MakeReveal(int32 Serial, const TArray<TArray<uint8>>& PicksBySlot, bool bChoose)
	{
		FPinguFairReveal Reveal;
		Reveal.Serial = Serial;
		Reveal.Round = 1 + Serial % 7;
		Reveal.Secret = TEXT("0123456789ABCDEF0123456789ABCDEF");
		Reveal.Contributions = { { 256, FString::Printf(TEXT("%08X%08X"), Serial * 7919, Serial * 104729) } };
		Reveal.Slots = { 0, 1, 2 };
		Reveal.bChoose = bChoose;
		bool bAny = false;
		Reveal.Picks.Init(0, 3 * PinguPicks::NumSlots);
		for (int32 Slot = 0; Slot < 3 && Slot < PicksBySlot.Num(); ++Slot)
		{
			for (int32 Index = 0; Index < PinguPicks::NumSlots && Index < PicksBySlot[Slot].Num(); ++Index)
			{
				Reveal.Picks[Slot * PinguPicks::NumSlots + Index] = PicksBySlot[Slot][Index];
				bAny |= PicksBySlot[Slot][Index] != 0;
			}
		}
		if (!bAny)
		{
			Reveal.Picks.Reset();
		}
		return Reveal;
	}

	bool Distinct(const TArray<EPenguinAbility>& Loadout)
	{
		TSet<EPenguinAbility> Seen;
		for (const EPenguinAbility Ability : Loadout)
		{
			if (Ability != EPenguinAbility::None)
			{
				bool bAlready = false;
				Seen.Add(Ability, &bAlready);
				if (bAlready)
				{
					return false;
				}
			}
		}
		return true;
	}
}

void APenguinAutoTest::AddAbilityPicksSteps()
{
	using namespace PenguinAutoTestAbilityPicksPrivate;
	using A = EPenguinAbility;

	AddStep(TEXT("anticheat_PicksDeal"), 0.1f,
		[this]
		{
			const TArray<uint8> Picks0 = Bytes(A::Snowball, A::None, A::Flap);  // two picks, the middle random
			const TArray<uint8> Picks1 = Bytes(A::Icicle, A::IceWall, A::Noot); // all three
			int32 Kept = 0;
			int32 Twice = 0;
			int32 CopiedPick = 0;
			int32 Moved = 0;
			int32 RandomMiddles = 0;
			TSet<A> Middles;
			for (int32 Serial = 1; Serial <= 30; ++Serial)
			{
				FPinguFairDeal Deal;
				PinguFair::Deal(MakeReveal(Serial, { Picks0, Picks1, {} }, true), Deal);
				const TArray<A>& L0 = Deal.Loadouts[Deal.IndexOf(0)];
				const TArray<A>& L1 = Deal.Loadouts[Deal.IndexOf(1)];
				const TArray<A>& L2 = Deal.Loadouts[Deal.IndexOf(2)];
				Kept += (L0[0] == A::Snowball && L0[2] == A::Flap && L1 == PinguPicks::ToAbilities(Picks1)) ? 1 : 0;
				CopiedPick += (L0[1] == A::Snowball || L0[1] == A::Flap || L0[1] == A::None) ? 1 : 0;
				Middles.Add(L0[1]);
				RandomMiddles += L0[1] != A::None ? 1 : 0;
				Twice += (Distinct(L0) && Distinct(L1) && Distinct(L2) && !L2.Contains(A::None)) ? 0 : 1;
				// Slot 2 has no picks: its loadout must be the same whatever the others picked (and with RANDOM).
				FPinguFairDeal NoPicks;
				PinguFair::Deal(MakeReveal(Serial, { {}, {}, {} }, true), NoPicks);
				FPinguFairDeal Random;
				PinguFair::Deal(MakeReveal(Serial, { Picks0, Picks1, {} }, false), Random);
				Moved += (NoPicks.Loadouts[NoPicks.IndexOf(2)] == L2 && Random.Loadouts[Random.IndexOf(2)] == L2) ? 0 : 1;
				// RANDOM ignores the picks: slot 0 and 1 as with none.
				Moved += (Random.Loadouts[0] == NoPicks.Loadouts[0] && Random.Loadouts[1] == NoPicks.Loadouts[1]) ? 0 : 1;
			}
			Report(FString::Printf(TEXT("RESULT anticheat picks dealt: 30 rounds, picks kept %d (want 30), a pick copied into a random slot %d (want 0), an ability twice %d (want 0), slot 1's middle rolled %d kinds, the others' loadouts moved by picks or RANDOM %d (want 0) %s"),
				Kept, CopiedPick, Twice, Middles.Num(), Moved, *Ok(Kept == 30 && CopiedPick == 0 && Twice == 0 && Middles.Num() >= 8 && Moved == 0)));

			// Second Chance (two abilities, slots 0 and 2): the first two picks; Same Abilities: one roll for everybody.
			FPinguFairReveal Second = MakeReveal(5, { Bytes(A::None, A::Gust, A::IceShell), Picks1, {} }, true);
			Second.bTraits = true;
			Second.ForcedTraits = { static_cast<uint8>(EPinguTrait::SecondChance), static_cast<uint8>(EPinguTrait::Giant), static_cast<uint8>(EPinguTrait::Giant) };
			FPinguFairDeal SecondDeal;
			PinguFair::Deal(Second, SecondDeal);
			const TArray<A>& S0 = SecondDeal.Loadouts[SecondDeal.IndexOf(0)];
			const bool bSecond = S0.Num() == 3 && S0[0] == A::Gust && S0[1] == A::None && S0[2] == A::IceShell;
			FPinguFairReveal Same = MakeReveal(6, { Picks0, Picks1, {} }, true);
			Same.bTraits = true;
			Same.ForcedTraits = { static_cast<uint8>(EPinguTrait::SameAbilities) };
			FPinguFairDeal SameDeal;
			PinguFair::Deal(Same, SameDeal);
			const bool bSame = SameDeal.Loadouts.Num() == 3 && SameDeal.Loadouts[0] == SameDeal.Loadouts[1] && SameDeal.Loadouts[1] == SameDeal.Loadouts[2]
				&& Distinct(SameDeal.Loadouts[0]) && !SameDeal.Loadouts[0].Contains(A::None) && SameDeal.Traits[0] == EPinguTrait::SameAbilities;
			int32 SameDiffers = 0; // over 20 rounds the shared roll is not just slot 1's picks
			for (int32 Serial = 10; Serial < 30; ++Serial)
			{
				FPinguFairReveal R = Same;
				R.Serial = Serial;
				FPinguFairDeal D;
				PinguFair::Deal(R, D);
				SameDiffers += D.Loadouts[1] != PinguPicks::ToAbilities(Picks1) ? 1 : 0;
			}
			Report(FString::Printf(TEXT("RESULT anticheat picks traits: Second Chance %s (want Gust, -, Ice Shell), Same Abilities everybody %s, over the picks in %d of 20 rounds %s"),
				*PinguPicks::Describe(PinguPicks::ToBytes(S0)), *PinguPicks::Describe(PinguPicks::ToBytes(SameDeal.Loadouts[0])), SameDiffers,
				*Ok(bSecond && bSame && SameDiffers >= 19)));

			// Solo RANDOM: MakeLoadout with no picks rolls exactly as RollLoadout (the same stream, the same result).
			int32 SoloSame = 0;
			for (int32 Seed = 0; Seed < 50; ++Seed)
			{
				FRandomStream RA(Seed);
				FRandomStream RB(Seed);
				const int32 Num = Seed % 5 == 0 ? 2 : 3;
				const bool bEqual = PinguPicks::MakeLoadout(RA, Num, {}) == UPenguinAbilityComponent::RollLoadout(RB, Num);
				SoloSame += bEqual && RA.GetCurrentSeed() == RB.GetCurrentSeed() ? 1 : 0;
			}
			const bool bValid = PinguPicks::IsValid({}) && PinguPicks::IsValid(Picks0) && !PinguPicks::IsValid(Bytes(A::Flap, A::None, A::Flap))
				&& !PinguPicks::IsValid({ 1, 2 }) && !PinguPicks::IsValid({ 1, 2, static_cast<uint8>(A::Count) }) && PinguPicks::IsValid(Bytes(A::None, A::None, A::None));
			Report(FString::Printf(TEXT("RESULT anticheat picks solo: no picks rolls as before %d of 50, the checks (three, different, real) %d %s"),
				SoloSame, bValid ? 1 : 0, *Ok(SoloSame == 50 && bValid)));
		},
		[](float) {}, [] {});

	AddStep(TEXT("anticheat_PicksReveal"), 0.1f,
		[this]
		{
			// We are slot 1 (we asked for Icicle, Ice Wall, Noot; before that only Snowball), slot 0 is the host, slot 2 a bot.
			const TArray<uint8> Host = Bytes(A::SnowBomb, A::None, A::GiantSnowball);
			const TArray<uint8> Ours = Bytes(A::Icicle, A::IceWall, A::Noot);
			const TArray<uint8> Older = Bytes(A::Snowball, A::None, A::None);
			FPinguFairReveal Reveal = MakeReveal(7, { Host, Ours, {} }, true);
			FPinguRevealCheck Honest;
			Honest.SeenHash = PinguFair::CommitOf(Reveal.Secret, Reveal.Serial);
			Honest.OwnPlayer = 256;
			Honest.KnownJoiners = { 256 };
			Honest.SeenReceived = { { 256, Reveal.Contributions[0].Value } };
			Honest.OwnSlot = 1;
			Honest.OwnPicksSent = { Older, Ours };
			Honest.ChooseSeen = 2;
			Honest.BotSlots = { 2 };
			const auto Points = [](const FPinguFairReveal& R, const FPinguRevealCheck& C)
			{
				int32 Sum = 0;
				for (const TPair<int32, FString>& Problem : PinguFair::CheckReveal(R, C))
				{
					Sum += Problem.Key;
				}
				return Sum;
			};
			struct FCase
			{
				const TCHAR* Name;
				int32 Points;
				int32 Want;
			};
			const int32 Leave = FPinguCheatJudge::LeaveAt;
			TArray<FCase> Cases;
			Cases.Add({ TEXT("honest CHOOSE"), Points(Reveal, Honest), 0 });
			{
				FPinguRevealCheck C = Honest;
				C.ChooseSeen = 1;
				FPinguFairReveal R = MakeReveal(7, {}, false);
				Cases.Add({ TEXT("honest RANDOM"), Points(R, C), 0 });
				Cases.Add({ TEXT("picks with RANDOM"), Points(MakeReveal(7, { Host, {}, {} }, false), C), Leave });
				Cases.Add({ TEXT("CHOOSE while the lobby said RANDOM"), Points(Reveal, C), Leave });
			}
			{
				FPinguRevealCheck C = Honest;
				C.ChooseSeen = 3; // (switched a moment ago: both are fine)
				Cases.Add({ TEXT("the switch just flipped"), Points(Reveal, C), 0 });
				C.MatchChoose = 0;
				Cases.Add({ TEXT("RANDOM -> CHOOSE within the match"), Points(Reveal, C), Leave });
			}
			Cases.Add({ TEXT("a bot with picks"), Points(MakeReveal(7, { Host, Ours, Bytes(A::SnowBomb, A::Icicle, A::IcebergDrop) }, true), Honest), Leave });
			Cases.Add({ TEXT("our picks faked by the host"), Points(MakeReveal(7, { Host, Bytes(A::Flap, A::None, A::None), {} }, true), Honest), Leave });
			Cases.Add({ TEXT("our older ask"), Points(MakeReveal(7, { Host, Older, {} }, true), Honest), 0 });
			{
				FPinguRevealCheck C = Honest;
				C.OwnPicksSent.Reset();
				Cases.Add({ TEXT("picks for us though we never picked"), Points(Reveal, C), Leave });
				Cases.Add({ TEXT("none for us, never picked"), Points(MakeReveal(7, { Host, {}, {} }, true), C), 0 });
			}
			{
				FPinguRevealCheck C = Honest;
				C.MatchChoose = 1;
				C.MatchPicks = { { 0, Bytes(A::Snowball, A::None, A::GiantSnowball) }, { 1, Ours }, { 2, {} } };
				Cases.Add({ TEXT("the host's picks changed in the match"), Points(Reveal, C), Leave });
				C.MatchPicks[0] = Host;
				Cases.Add({ TEXT("the same picks next round"), Points(Reveal, C), 0 });
				Cases.Add({ TEXT("picks gone (a late joiner's slot)"), Points(MakeReveal(7, { {}, Ours, {} }, true), C), 0 });
				C.MatchPicks[0] = TArray<uint8>();
				Cases.Add({ TEXT("picks out of nowhere in the match"), Points(Reveal, C), Leave });
			}
			Cases.Add({ TEXT("an ability twice"), Points(MakeReveal(7, { Bytes(A::SnowBomb, A::SnowBomb, A::None), Ours, {} }, true), Honest), Leave });
			{
				FPinguFairReveal R = Reveal;
				R.Picks.SetNum(6);
				Cases.Add({ TEXT("picks for two slots of three"), Points(R, Honest), Leave });
			}
			for (const FCase& Case : Cases)
			{
				Report(FString::Printf(TEXT("RESULT anticheat picks reveal %s: %d points (want %d) %s"), Case.Name, Case.Points, Case.Want, *Ok(Case.Points == Case.Want)));
			}
		},
		[](float) {}, [] {});

	AddStep(TEXT("anticheat_PicksJudge"), 0.1f,
		[this]
		{
			// A deal with picks (slot 0: Snow Bomb, random, Giant Snowball), then 12 s of a fight.
			FPinguFairDeal Deal;
			PinguFair::Deal(MakeReveal(9, { Bytes(A::SnowBomb, A::None, A::GiantSnowball), {}, {} }, true), Deal);
			const auto Fight = [&Deal](const TArray<A>& Held)
			{
				FPinguCheatJudge Judge;
				FPinguSeenPenguin Seen;
				Seen.Slot = 0;
				Seen.Name = TEXT("Host");
				for (const A Ability : Held)
				{
					FPinguSeenSlot Entry;
					Entry.Ability = Ability;
					Seen.Slots.Add(Entry);
				}
				FPinguSeenPenguin Countdown = Seen;
				for (int32 Index = 0; Index < Countdown.Slots.Num(); ++Index)
				{
					Countdown.Slots[Index].Ability = Deal.Loadouts[0][Index];
				}
				const double Client = 1.0;
				Judge.SetRound(3, false, 0.f, Client);
				Judge.ExpectDeal(3, Deal);
				Judge.Observe(Client, 50.0 + Client, Countdown);
				Judge.Tick(Client);
				for (float Age = 0.f; Age < 12.f; Age += 0.05f)
				{
					Judge.SetRound(3, true, Age, Client + 1.0 + Age);
					Judge.Observe(Client + 1.0 + Age, 51.0 + Client + Age, Seen);
					Judge.Tick(Client + 1.0 + Age);
				}
				return Judge;
			};
			const FPinguCheatJudge Honest = Fight(Deal.Loadouts[0]);
			TArray<A> Changed = Deal.Loadouts[0];
			Changed[1] = Changed[1] == A::Icicle ? A::Noot : A::Icicle; // the random middle overwritten with one the host likes
			const FPinguCheatJudge Cheat = Fight(Changed);
			Report(FString::Printf(TEXT("RESULT anticheat picks judge: the chosen loadout %s held 12 s %d points (want 0), its random slot overwritten for 12 s %d points, leaves %d (want 1) %s"),
				*PinguPicks::Describe(PinguPicks::ToBytes(Deal.Loadouts[0])), Honest.GetPoints(), Cheat.GetPoints(), Cheat.ShouldLeave() ? 1 : 0,
				*Ok(Honest.GetPoints() == 0 && Honest.GetDealsVerified() >= 1 && Cheat.ShouldLeave())));
		},
		[](float) {}, [] {});
}

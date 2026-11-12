// Pingu - anti-cheat for LAN games: the host's request gates, fair picks (commit-reveal), the joiner's judge, the join handshake.
// What it is and what it can't stop: PinguAntiCheat.h and docs/FEATURES.md (7.1).

#include "PinguAntiCheat.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinGameMode.h"
#include "PenguinGameState.h"
#include "PenguinMovementComponent.h"
#include "PenguinPlayerController.h"
#include "PenguinPlayerState.h"
#include "PenguinTraitComponent.h"
#include "PinguAbilityPicks.h"
#include "PinguGiantSnowball.h"
#include "PinguNetSubsystem.h"
#include "PinguPickup.h"
#include "PinguPortal.h"
#include "PinguSnowman.h"
#include "PenguinProjectile.h"
#include "PinguMemoryGuard.h"
#include "PinguTamper.h"
#include "PinguTuning.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Net/UnrealNetwork.h"
#include <atomic>

namespace PinguAntiCheatPrivate
{
	FString HashHex(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		uint8 Hash[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
		return BytesToHex(Hash, FSHA1::DigestSize);
	}

	/** -PinguCheat=<list> (dev builds only). */
	bool HasCheat(const TCHAR* Name)
	{
#if UE_BUILD_SHIPPING
		return false;
#else
		static const FString List = []
		{
			FString Value;
			FParse::Value(FCommandLine::Get(), TEXT("PinguCheat="), Value, false); // (the whole list: not cut at its first comma)
			return Value.ToLower();
		}();
		if (List.IsEmpty())
		{
			return false;
		}
		TArray<FString> Parts;
		List.ParseIntoArray(Parts, TEXT(","));
		return Parts.Contains(FString(Name).ToLower());
#endif
	}

	// ---- host: the gates
	struct FGateState
	{
		double Tokens = -1.0;
		double Last = 0.0;
	};
	struct FJoinerState
	{
		FGateState Gates[static_cast<int32>(PinguAntiCheat::EGate::Count)];
		double LastLog = -100.0;
		int32 Unlogged = 0;
		// moves
		bool bHaveMove = false;
		double LastMoveTime = 0.0;
		double RefusedSince = -1.0;
		double WindowStart = -1.0;
		FVector WindowFrom = FVector::ZeroVector;
	};
	TMap<TWeakObjectPtr<const UObject>, FJoinerState>& Joiners()
	{
		static TMap<TWeakObjectPtr<const UObject>, FJoinerState> Map;
		return Map;
	}
	FJoinerState& JoinerOf(const UObject* Who)
	{
		TMap<TWeakObjectPtr<const UObject>, FJoinerState>& Map = Joiners();
		if (Map.Num() > 64)
		{
			for (auto It = Map.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid())
				{
					It.RemoveCurrent();
				}
			}
		}
		return Map.FindOrAdd(TWeakObjectPtr<const UObject>(Who));
	}
	/** Requests per second and the burst a joiner may send through each gate (generous: an honest player mashing three keys
	 *  stays far below; the aim updates come at most every frame of a 50 fps client). */
	constexpr float GateRate[] = { 40.f, 120.f, 10.f, 4.f };
	constexpr float GateBurst[] = { 40.f, 120.f, 20.f, 8.f };
	int32 IgnoredCount = 0;
	int32 IgnoredMoves = 0;
	TArray<FString> IgnoredLines;
	/** An icicle hold may be this much longer than the host saw it (the press and the release travel the same way). */
	constexpr double HoldSlack = 0.35;
	/** A move further than this (cm) + MoveSpeed x the time since the joiner's last move is a teleport. */
	constexpr float MoveSlack = 250.f; // (400 before 2026-10-30: a 4 m hop every move)
	constexpr float MoveSpeed = 9000.f;
	/** On the ground faster than this on average over GroundWindow (cm/s) without a push from the host. */
	constexpr float MoveGroundSpeed = 4200.f;
	constexpr double MoveGroundWindow = 0.6;
	/** Refused this long in a row: the host moves the joiner's penguin back to where the host has it. */
	constexpr double PullBackAfter = 0.5;

	// ---- the handshake
	struct FDigest
	{
		FCriticalSection Lock;
		bool bStarted = false;
		std::atomic<bool> bReady { false };
		FString Digest;
		FString Description;
		/** The hashed files (sorted by name) and their sizes; set before the hashing starts, then only read. */
		TArray<FString> Paths;
		TArray<int64> Sizes;
		/** The ranges' hash for the nonces seen (a nonce asks for the same ones every time). */
		TMap<FString, FString> RangeHashes;
	};
	FDigest& Digest()
	{
		static FDigest Instance;
		return Instance;
	}
	struct FHostShake
	{
		FString Nonce;
		double SentAt = 0.0;
		bool bDone = false;
		double KickAt = -1.0;
		// an answer that came before our own digest was ready
		bool bAnswerWaiting = false;
		FString Answer;
		FString JoinerNonce;
		FString JoinerBuildId;
		/** When the joiner passed (dev kick switch). */
		double PassedAt = -1.0;
	};
	TMap<TWeakObjectPtr<APlayerController>, FHostShake>& HostShakes()
	{
		static TMap<TWeakObjectPtr<APlayerController>, FHostShake> Map;
		return Map;
	}
	struct FClientShake
	{
		double FirstTick = -1.0;
		bool bHello = false;
		bool bAnswered = false;
		bool bDone = false;
		FString HostNonce;
		FString HostBuildId;
		FString OwnNonce;
		double AnsweredAt = 0.0;
	};
	TMap<TWeakObjectPtr<APlayerController>, FClientShake>& ClientShakes()
	{
		static TMap<TWeakObjectPtr<APlayerController>, FClientShake> Map;
		return Map;
	}
	/** A joiner gets this long to answer the host's hello (it hashes its files first: about a second for a packaged game). */
	constexpr double AnswerTimeout = 90.0;
	/** A joiner leaves a host that has not said hello after this long. */
	constexpr double HelloTimeout = 60.0;
	int32 HandshakesOk = 0;
	int32 HandshakesFailed = 0;
	/** Contributions are expected in a reveal when they went out this long before it arrived. */
	constexpr double ContributionLead = 4.0;

	PinguAntiCheat::FClientReport& Report()
	{
		static PinguAntiCheat::FClientReport Instance;
		return Instance;
	}

	bool IsHexText(const FString& Text, int32 MaxLength)
	{
		if (Text.IsEmpty() || Text.Len() > MaxLength)
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsHexDigit(Char))
			{
				return false;
			}
		}
		return true;
	}

	FString LoadoutText(const TArray<EPenguinAbility>& Loadout)
	{
		FString Text;
		for (const EPenguinAbility Ability : Loadout)
		{
			Text += FString::Printf(TEXT("%s%s"), Text.IsEmpty() ? TEXT("") : TEXT(", "),
				Ability == EPenguinAbility::None ? TEXT("-") : UPenguinAbilityComponent::GetInfo(Ability).Name);
		}
		return Text;
	}
}

// ============================================================================ fair picks

FString PinguFair::MakeSecret()
{
	return (FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits)).Left(32).ToUpper();
}

FString PinguFair::CommitOf(const FString& Secret, int32 Serial)
{
	return PinguAntiCheatPrivate::HashHex(FString::Printf(TEXT("pingu-commit|%d|%s"), Serial, *Secret.ToUpper()));
}

bool PinguFair::IsValidValue(const FString& Value)
{
	return PinguAntiCheatPrivate::IsHexText(Value, 64);
}

int32 PinguFair::SeedOf(const FPinguFairReveal& Reveal)
{
	TArray<FPinguFairContribution> Sorted = Reveal.Contributions;
	Sorted.Sort([](const FPinguFairContribution& A, const FPinguFairContribution& B) { return A.Player < B.Player; });
	FString Text = FString::Printf(TEXT("pingu-seed|%d|%d|%s"), Reveal.Serial, Reveal.Round, *Reveal.Secret.ToUpper());
	for (const FPinguFairContribution& Contribution : Sorted)
	{
		Text += FString::Printf(TEXT("|%d:%s"), Contribution.Player, *Contribution.Value.ToUpper());
	}
	const FTCHARToUTF8 Utf8(*Text);
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
	return static_cast<int32>((uint32(Hash[0]) << 24) | (uint32(Hash[1]) << 16) | (uint32(Hash[2]) << 8) | uint32(Hash[3]));
}

void PinguFair::Deal(const FPinguFairReveal& Reveal, FPinguFairDeal& Out)
{
	Out = FPinguFairDeal();
	for (const uint8 Slot : Reveal.Slots)
	{
		Out.Slots.AddUnique(Slot);
	}
	Out.Slots.Sort();
	const int32 Seed = SeedOf(Reveal);
	FRandomStream AbilityRand(Seed);
	FRandomStream TraitRand(Seed ^ 0x5EED7A17);
	TArray<EPinguTrait> Bag;
	for (const uint8 Value : Reveal.TraitBag)
	{
		const EPinguTrait Trait = static_cast<EPinguTrait>(Value);
		if (PinguTraits::IsReal(Trait))
		{
			Bag.Add(Trait);
		}
	}
	Out.Traits.Init(EPinguTrait::None, Out.Slots.Num());
	if (Reveal.bTraits)
	{
		// As APenguinGameMode::DealTraits (2026-11-07, shared-traits): ONE trait for everybody - a test's one forced trait, else
		// the next from the match's bag (a fresh bag holds the host's ticked traits); a test's traits forced by slot go over it.
		const EPinguTrait Shared = Reveal.ForcedTraits.Num() == 1 ? static_cast<EPinguTrait>(Reveal.ForcedTraits[0])
			: PinguTraits::DrawShared(Bag, TraitRand, Reveal.TraitsOff);
		for (int32 Index = 0; Index < Out.Slots.Num(); ++Index)
		{
			const int32 Slot = Out.Slots[Index];
			Out.Traits[Index] = Reveal.ForcedTraits.Num() > 1 && Reveal.ForcedTraits.IsValidIndex(Slot) ? static_cast<EPinguTrait>(Reveal.ForcedTraits[Slot]) : Shared;
		}
	}
	Out.BagAfter = Bag;
	// The loadouts (2026-11-07, card ability-picks): a slot's picks (CHOOSE) stay, the rest roll as ever (PinguPicks::MakeLoadout:
	// with no picks exactly RollLoadout); Same Abilities: one roll of three for everybody, over any picks.
	const bool bSame = PinguPicks::IsSameAbilitiesRound(Out.Traits);
	const TArray<EPenguinAbility> Shared = bSame ? UPenguinAbilityComponent::RollAbilities(AbilityRand, PinguPicks::NumSlots) : TArray<EPenguinAbility>();
	for (int32 Index = 0; Index < Out.Slots.Num(); ++Index)
	{
		const int32 NumAbilities = PinguTraits::GetInfo(Out.Traits[Index]).N.NumAbilities;
		if (bSame)
		{
			Out.Loadouts.Add(PinguPicks::FitShared(Shared, NumAbilities));
			continue;
		}
		// Each slot rolls from a stream of its own (pingu-ac5): how many rolls one penguin's picks leave open can't move anybody
		// else's loadout (a host knows the seed when it deals and could otherwise try its own picks until the others' suit it).
		FRandomStream SlotRand(static_cast<int32>(static_cast<uint32>(Seed) ^ (0x9E3779B9u * static_cast<uint32>(Out.Slots[Index] + 1))));
		Out.Loadouts.Add(PinguPicks::MakeLoadout(SlotRand, NumAbilities, Reveal.bChoose ? PicksOf(Reveal, Out.Slots[Index]) : TArray<uint8>()));
	}
}

TArray<uint8> PinguFair::PicksOf(const FPinguFairReveal& Reveal, int32 Slot)
{
	const int32 Index = Reveal.Slots.IndexOfByKey(static_cast<uint8>(Slot));
	TArray<uint8> Picks;
	if (Index != INDEX_NONE && Reveal.Picks.Num() == Reveal.Slots.Num() * PinguPicks::NumSlots)
	{
		Picks.Append(Reveal.Picks.GetData() + Index * PinguPicks::NumSlots, PinguPicks::NumSlots);
	}
	return PinguPicks::HasAny(Picks) ? Picks : TArray<uint8>();
}

// ============================================================================ the joiner's judge
//
// 2026-11-05 (card host-cheat-fix): a host with a memory trainer (PinguTrainer.exe) wrote its own loadout and zeroed its
// cooldowns, and no joiner left. Why: the deal was checked once - the moment the dealt loadout first showed (often in the
// intro, before the trainer's next write) it counted as verified and was never looked at again, and the slots were only
// tracked from the fight's start, so a loadout overwritten in between never changed in the judge's eyes; a slot whose swap
// counter had ever moved was excused; any swap-counter change excused the cooldown too; the snowball (and Leech) were never
// cooldown-checked; everything else came from the host's own numbers, 1 point per kind every 3 s. Now:
//  - the loadout: every frame of the round, every ability a penguin holds must be dealt, or come from a bubble that went away
//    near it, or from a Jester swap (2 points per ability, again every 3 s while it holds it);
//  - the uses: timed by when they arrive here (not by the host's numbers), k more uses of one ability need k cooldowns (the
//    trait's scale; a hit anywhere may give a snowball back or cut a Leech's cooldowns; a bubble recharges) - RateSlack;
//    the host's counters may never go back; a cooldown cut needs a bubble, a hit (snowball, Leech) or the giant snowball;
//  - the shots: a projectile, snowman or giant snowball must come from an ability its penguin holds, and from a use the host
//    counted (else the host hid it);
//  - the deal itself: see PinguFair::CheckReveal, IsCommitStepFine, ExpectRevealFor.

void FPinguCheatJudge::SetRound(int32 InRound, bool bInFight, float InFightAge, double ClientNow)
{
	if (InRound != Round)
	{
		Round = InRound;
		if (ExpectedRound != Round)
		{
			ExpectedRound = -1; // (an old match's deal; a reveal of this round that came first stays)
		}
		for (TPair<int32, FTrack>& Pair : Tracks)
		{
			FTrack& Track = Pair.Value;
			Track.bValid = false;
			Track.bLoadoutChecked = false;
			Track.bHaveSample = false;
			Track.WindowStart = -1.0;
			Track.bEarnedSet = false;
			Track.Earned.Reset();
			Track.Unearned.Reset();
			Track.Armed.Reset();
			Track.bTraitChecked = false;
			Track.bNotDealtStruck = false;
			Track.bVerified = false;
			Track.UseTimes.Reset();
			Track.Counted.Reset();
			Track.Held.Reset();
			Track.Uncounted.Reset();
			Track.Unheld.Reset();
		}
		PendingSwaps.Reset();
		Pendings.Reset();
	}
	if (!bInFight)
	{
		for (TPair<int32, FTrack>& Pair : Tracks)
		{
			Pair.Value.bValid = false; // the next fight starts from what it sees then (the uses; the loadout is judged all round)
		}
	}
	bFight = bInFight;
	FightAge = InFightAge;
	// A new round's reveal must have come by now (a host that deals without one is dealing what it likes).
	if (bFight && FightAge >= RevealGrace && RevealWanted == Round && Round > 0)
	{
		RevealWanted = -1;
		if (!RevealedRounds.Contains(Round))
		{
			Strike(LeaveAt, -1, TEXT("no reveal"), FString::Printf(TEXT("round %d is fought and its deal was never revealed"), Round), ClientNow);
		}
	}
}

void FPinguCheatJudge::ExpectDeal(int32 InRound, const FPinguFairDeal& Deal)
{
	RevealedRounds.Add(InRound); // (a second deal of a round is PinguFair::CheckReveal's)
	ExpectedRound = InRound;
	Expected = Deal;
	for (TPair<int32, FTrack>& Pair : Tracks)
	{
		FTrack& Track = Pair.Value;
		Track.bLoadoutChecked = false;
		Track.bEarnedSet = false;
		Track.Earned.Reset();
		Track.Unearned.Reset();
		Track.Armed.Reset();
		Track.bTraitChecked = false;
		Track.bNotDealtStruck = false;
		Track.bVerified = false;
	}
}

void FPinguCheatJudge::ExpectRevealFor(int32 InRound)
{
	RevealWanted = InRound;
}

void FPinguCheatJudge::NewMatch()
{
	RevealedRounds.Reset();
	RevealWanted = -1;
}

void FPinguCheatJudge::Observe(double ClientNow, double ServerNow, const FPinguSeenPenguin& Seen)
{
	if (Seen.Slot < 0)
	{
		return;
	}
	FTrack& Track = Tracks.FindOrAdd(Seen.Slot);
	// Where it was (a bubble must go away near it), its hits (snowball refunds, Leech), knock-outs, Jester's swaps.
	Track.Positions.Add(TPair<double, FVector>(ClientNow, Seen.Position));
	while (Track.Positions.Num() > 2 && ClientNow - Track.Positions[0].Key > 2.0 * PickupWindow)
	{
		Track.Positions.RemoveAt(0, EAllowShrinking::No);
	}
	Track.Size = FMath::Max(Seen.Size, 0.1f);
	if (Track.Hits >= 0 && Seen.HitEvents > Track.Hits)
	{
		for (int32 Index = 0; Index < FMath::Min(Seen.HitEvents - Track.Hits, 8); ++Index)
		{
			HitTimes.Add(ServerNow);
		}
	}
	Track.Hits = Seen.HitEvents;
	if (Track.Jester >= 0 && Seen.JesterSwaps != Track.Jester)
	{
		Track.JesterBumps.Add(ClientNow);
		Track.LastJester = ClientNow;
		for (const TPair<EPenguinAbility, double>& Arrived : Track.Arrived)
		{
			if (ClientNow - Arrived.Value <= PickupWindow)
			{
				Track.UseTimes.Remove(Arrived.Key); // (the swap came before its counter: see JudgeSlots)
			}
		}
	}
	Track.Jester = Seen.JesterSwaps;
	if (Track.LeechCuts >= 0 && Seen.LeechCuts != Track.LeechCuts)
	{
		const int32 Cuts = (Seen.LeechCuts - Track.LeechCuts + 256) % 256; // (a byte that wraps)
		for (int32 Index = 0; Index < FMath::Min(Cuts, 16); ++Index)
		{
			Track.LeechTimes.Add(ServerNow);
		}
		Track.LeechTimes.RemoveAll([ServerNow](double At) { return ServerNow - At > 40.0; });
	}
	Track.LeechCuts = Seen.LeechCuts;
	if (Seen.bKnockedOut && !Track.bOut)
	{
		Track.UseTimes.Reset(); // (out: whatever comes next starts afresh)
	}
	Track.bOut = Seen.bKnockedOut;
	for (const FPinguSeenSlot& Slot : Seen.Slots)
	{
		if (Slot.Ability != EPenguinAbility::None)
		{
			Track.Held.Add(Slot.Ability, ClientNow);
		}
	}
	JudgeLoadout(Track, ClientNow, Seen);
	if (bFight)
	{
		JudgeSlots(Track, ServerNow, ClientNow, Seen);
	}
	if (Seen.bJudgeMotion)
	{
		JudgeMotion(Track, ClientNow, Seen);
	}
	Track.ObservedAt = ServerNow;
}

bool FPinguCheatJudge::TakenNear(FTrack& Track, EPenguinAbility Ability, double At, bool bConsume)
{
	const float Range = TakeRange + TakeRangePerSize * Track.Size;
	for (FGone& Gone : GonePickups)
	{
		if (Gone.bUsed || Gone.Ability != Ability || FMath::Abs(Gone.At - At) > PickupWindow)
		{
			continue;
		}
		// Near any of its places around then, or the way between two of them (a warp grabs the bubbles on its path).
		bool bNear = false;
		for (int32 Index = 0; Index < Track.Positions.Num() && !bNear; ++Index)
		{
			const TPair<double, FVector>& Here = Track.Positions[Index];
			if (FMath::Abs(Here.Key - Gone.At) > 1.0)
			{
				continue;
			}
			bNear = FVector::Dist(Here.Value, Gone.Where) <= Range;
			if (!bNear && Index > 0)
			{
				bNear = FMath::PointDistToSegment(Gone.Where, Track.Positions[Index - 1].Value, Here.Value) <= Range;
			}
		}
		if (bNear)
		{
			Gone.bUsed |= bConsume;
			return true;
		}
	}
	return false;
}

bool FPinguCheatJudge::HitNear(double ServerAt, double Window) const
{
	for (const double Hit : HitTimes)
	{
		if (FMath::Abs(Hit - ServerAt) <= Window)
		{
			return true;
		}
	}
	return false;
}

int32 FPinguCheatJudge::CountHits(double From, double To) const
{
	int32 Count = 0;
	for (const double Hit : HitTimes)
	{
		Count += Hit >= From && Hit <= To ? 1 : 0;
	}
	return Count;
}

void FPinguCheatJudge::JudgeLoadout(FTrack& Track, double ClientNow, const FPinguSeenPenguin& Seen)
{
	if (ExpectedRound != Round || ExpectedRound < 0 || Seen.bKnockedOut)
	{
		return;
	}
	const int32 Index = Expected.IndexOf(Seen.Slot);
	const bool bLate = bFight && FightAge >= LoadoutGrace;
	if (Index == INDEX_NONE)
	{
		if (bLate && !Track.bNotDealtStruck)
		{
			Track.bNotDealtStruck = true;
			Strike(2, Seen.Slot, TEXT("not in the deal"), FString::Printf(TEXT("%s fights in round %d but was not dealt"), *Seen.Name, Round), ClientNow);
		}
		if (!bLate)
		{
			return; // (from the fight on: nothing it holds was dealt)
		}
	}
	const TArray<EPenguinAbility> NoneDealt;
	const TArray<EPenguinAbility>& Want = Index != INDEX_NONE ? Expected.Loadouts[Index] : NoneDealt;
	if (!Track.bEarnedSet)
	{
		Track.bEarnedSet = true;
		for (const EPenguinAbility Ability : Want)
		{
			if (Ability != EPenguinAbility::None)
			{
				Track.Earned.Add(Ability);
			}
		}
	}
	if (Index != INDEX_NONE)
	{
		bool bSame = Seen.Slots.Num() == Want.Num();
		for (int32 Slot = 0; bSame && Slot < Want.Num(); ++Slot)
		{
			bSame = Seen.Slots[Slot].Ability == Want[Slot];
		}
		if (bSame && !Track.bVerified)
		{
			Track.bVerified = true;
			++DealsVerified;
		}
		if (bLate && !Track.bTraitChecked)
		{
			Track.bTraitChecked = true;
			const EPinguTrait WantTrait = Expected.Traits[Index];
			if (Seen.Trait != WantTrait)
			{
				Strike(2, Seen.Slot, TEXT("trait"), FString::Printf(TEXT("%s has the trait %s in round %d, the deal gave %s"), *Seen.Name,
					*PinguTraits::GetLogName(Seen.Trait), Round, *PinguTraits::GetLogName(WantTrait)), ClientNow);
			}
		}
	}
	Track.Armed.SetNum(FMath::Max(Track.Armed.Num(), Seen.Slots.Num()));
	for (int32 Slot = 0; Slot < Seen.Slots.Num(); ++Slot)
	{
		const EPenguinAbility Ability = Seen.Slots[Slot].Ability;
		if (Ability == EPenguinAbility::None)
		{
			continue;
		}
		if (Want.IsValidIndex(Slot) && Want[Slot] == Ability)
		{
			Track.Armed[Slot] = true;
		}
		if (bDevScript)
		{
			Track.Earned.Add(Ability); // (dev builds: the net test's script handed it out)
			continue;
		}
		if (Track.Earned.Contains(Ability) || (!Track.Armed[Slot] && !bLate))
		{
			continue; // dealt, from a bubble, or the deal still on its way here
		}
		if (!Track.Unearned.Contains(Ability))
		{
			Track.Unearned.Add(Ability, ClientNow);
		}
	}
	(void)Track.bLoadoutChecked;
}

void FPinguCheatJudge::AddUse(FTrack& Track, int32 Slot, const FString& Name, EPinguTrait Trait, EPenguinAbility Ability, double ServerAt, double ClientNow)
{
	TArray<double>& Times = Track.UseTimes.FindOrAdd(Ability);
	Times.Add(ServerAt);
	while (Times.Num() > 1 && ServerAt - Times[0] > 30.0)
	{
		Times.RemoveAt(0);
	}
	Times.Sort();
	const float Rule = UPenguinAbilityComponent::GetInfo(Ability).Cooldown * PinguTraits::GetInfo(Trait).N.CooldownScale;
	if (Rule < 0.3f || Times.Num() < 2)
	{
		return;
	}
	const bool bSnowball = Ability == EPenguinAbility::Snowball;
	const bool bLeech = Trait == EPinguTrait::Leech;
	const int32 Last = Times.Num() - 1;
	for (int32 First = Last - 1; First >= 0; --First)
	{
		const int32 Gaps = Last - First;
		const double Span = Times[Last] - Times[First];
		// A hit anywhere may have given a snowball back (the combo); each of Leech's own cuts takes LeechCooldownCut off.
		int32 Hits = bSnowball ? CountHits(Times[First] - 0.3, Times[Last] + 0.3) : 0;
		if (bLeech)
		{
			for (const double At : Track.LeechTimes)
			{
				Hits += At >= Times[First] - 0.3 && At <= Times[Last] + 0.3 ? 1 : 0;
			}
		}
		const double Need = bSnowball ? FMath::Max(0, Gaps - Hits) * Rule : Gaps * Rule - (bLeech ? PinguTraitTuning::LeechCooldownCut * Hits : 0.0);
		if (Span + RateSlack < Need)
		{
			Strike(Span < 0.5 * Need ? 2 : 1, Slot, *FString::Printf(TEXT("too many uses %s"), UPenguinAbilityComponent::GetInfo(Ability).Name),
				FString::Printf(TEXT("%s used %s %d more time(s) within %.2f s, its cooldown (%.2f s) allows %d"), *Name,
					UPenguinAbilityComponent::GetInfo(Ability).Name, Gaps, Span, Rule, FMath::FloorToInt32((Span + RateSlack) / Rule) + (bSnowball ? Hits : 0)),
				ClientNow);
			return;
		}
	}
}

void FPinguCheatJudge::JudgeSlots(FTrack& Track, double ServerNow, double ClientNow, const FPinguSeenPenguin& Seen)
{
	if (!Track.bValid || Track.Slots.Num() != Seen.Slots.Num() || bDevScript)
	{
		// (dev builds: while the net test's script hands out abilities - ready at once, with no bubble near - its uses and
		// cooldowns are not judged, and what came before does not count against what comes after)
		if (bDevScript)
		{
			Track.UseTimes.Reset();
			Track.Counted.Reset();
		}
		Track.Slots = Seen.Slots;
		Track.bValid = true;
		return;
	}
	const float Scale = PinguTraits::GetInfo(Seen.Trait).N.CooldownScale;
	const bool bLeech = Seen.Trait == EPinguTrait::Leech;
	for (int32 Index = 0; Index < Seen.Slots.Num(); ++Index)
	{
		const FPinguSeenSlot& Prev = Track.Slots[Index];
		const FPinguSeenSlot& Now = Seen.Slots[Index];
		if (Now.Uses < Prev.Uses || Now.Swaps < Prev.Swaps)
		{
			// Only a new loadout resets them, and that never happens in a fight: written back from outside (a use hidden).
			Strike(2, Seen.Slot, TEXT("counter back"), FString::Printf(TEXT("%s's slot %d: its use / bubble counters went back (%d -> %d, %d -> %d)"),
				*Seen.Name, Index, Prev.Uses, Now.Uses, Prev.Swaps, Now.Swaps), ClientNow);
			continue;
		}
		if (Now.Ability != Prev.Ability || Now.Ability == EPenguinAbility::None)
		{
			// (a new ability: the loadout check judges where it came from. A Jester swap hands it the old slot's cooldown, so
			// its own earlier uses no longer count.)
			if (Now.Ability != EPenguinAbility::None)
			{
				Track.Arrived.Add(Now.Ability, ClientNow);
				if (FMath::Abs(ClientNow - Track.LastJester) <= PickupWindow)
				{
					Track.UseTimes.Remove(Now.Ability);
				}
			}
			continue;
		}
		const EPenguinAbility Ability = Now.Ability;
		const TCHAR* Name = UPenguinAbilityComponent::GetInfo(Ability).Name;
		const bool bSnowball = Ability == EPenguinAbility::Snowball;
		const bool bGiant = Ability == EPenguinAbility::GiantSnowball;
		const float Rule = UPenguinAbilityComponent::GetInfo(Ability).Cooldown * Scale;
		if (Now.Uses > Prev.Uses)
		{
			const int32 Count = Now.Uses - Prev.Uses;
			UsesJudged += Count;
			if (Ability == EPenguinAbility::SnowWarp)
			{
				Track.QuietUntil = FMath::Max(Track.QuietUntil, Track.SampleTime + MotionQuiet); // a warp moves it far
			}
			// Timed by their arrival (the host's numbers are not trusted): several in one update are spread over the time since
			// we last looked, as far apart as their cooldown allows at most.
			const double Gap = Track.ObservedAt > 0.0 ? FMath::Max(ServerNow - Track.ObservedAt, 0.0) : 0.0;
			const double Spacing = Count > 1 ? FMath::Min(static_cast<double>(Rule), Gap / (Count - 1)) : 0.0;
			for (int32 Use = 0; Use < Count; ++Use)
			{
				Track.Counted.Add(TPair<EPenguinAbility, double>(Ability, ClientNow));
				AddUse(Track, Seen.Slot, Seen.Name, Seen.Trait, Ability, ServerNow - (Count - 1 - Use) * Spacing, ClientNow);
			}
			if (Rule < 0.5f)
			{
				continue;
			}
			if (Now.Length + 0.05f < Rule * 0.97f)
			{
				Strike(1, Seen.Slot, TEXT("short cooldown"), FString::Printf(TEXT("%s's %s: a %.2f s cooldown, the rules say %.2f s"), *Seen.Name, Name,
					Now.Length, Rule), ClientNow);
				continue;
			}
			if (FMath::IsNearlyEqual(Now.End, Prev.End, 0.001) || Now.End <= 0.0)
			{
				const FString Detail = FString::Printf(TEXT("%s used %s (%d use%s) and no cooldown started"), *Seen.Name, Name, Count, Count == 1 ? TEXT("") : TEXT("s"));
				if ((bSnowball || bLeech) && Now.End <= 0.0)
				{
					// A snowball that hit at once is given back, a Leech's hit may cut it to nothing: fine with a hit about then.
					FPending Pending;
					Pending.Kind = EPending::Refund;
					Pending.Slot = Seen.Slot;
					Pending.Ability = Ability;
					Pending.Since = ClientNow;
					Pending.ServerAt = ServerNow;
					Pending.Name = Seen.Name;
					Pending.Detail = Detail;
					Pending.bHitOk = bSnowball;
					Pending.bLeech = bLeech;
					Pendings.Add(Pending);
				}
				else
				{
					Strike(1, Seen.Slot, TEXT("no cooldown"), Detail, ClientNow);
				}
				continue;
			}
			if (bLeech || bSnowball)
			{
				continue; // (their cooldowns may legally have been cut in between)
			}
			const double UseTime = Now.End - Now.Length - (bGiant ? PinguTuning::GiantBuildTime : 0.f);
			double Lower = Prev.End;
			if (Count > 1)
			{
				Lower = FMath::Max(Lower, Track.ObservedAt - 1.0);
			}
			const double Want = Lower + (Count - 1) * Rule;
			if (Lower > 0.0 && UseTime + CooldownSlack < Want)
			{
				Strike(1, Seen.Slot, TEXT("early use"), FString::Printf(TEXT("%s used %s %.2f s before its cooldown was over (%d use%s)"), *Seen.Name, Name,
					Want - UseTime, Count, Count == 1 ? TEXT("") : TEXT("s")), ClientNow);
			}
			continue;
		}
		// (a slot Jester filled a moment ago: it carries the old ability's cooldown, and a giant snowball's roll-up let go
		// afterwards restarts that slot's cooldown - a cut seen in a long traits match)
		const double* ArrivedAt = Track.Arrived.Find(Ability);
		const bool bJesterSlot = ArrivedAt && ClientNow - *ArrivedAt < 10.0 && FMath::Abs(*ArrivedAt - Track.LastJester) <= PickupWindow;
		if (Now.End + 0.05 < Prev.End && Prev.End > ServerNow + 0.1 && !bGiant && !bJesterSlot)
		{
			// A running cooldown cut: a bubble of it (recharged), a hit (snowball, Leech) - or written from outside.
			FPending Pending;
			Pending.Kind = EPending::Cut;
			Pending.Slot = Seen.Slot;
			Pending.Ability = Ability;
			Pending.Since = ClientNow;
			Pending.ServerAt = ServerNow;
			Pending.Name = Seen.Name;
			Pending.bHitOk = bSnowball;
			Pending.bLeech = bLeech;
			Pending.Detail = FString::Printf(TEXT("%s's %s: its cooldown was cut by %.2f s"), *Seen.Name, Name, Prev.End - FMath::Max(Now.End, ServerNow));
			Pendings.Add(Pending);
		}
	}
	Track.Slots = Seen.Slots;
}

void FPinguCheatJudge::JudgeMotion(FTrack& Track, double ClientNow, const FPinguSeenPenguin& Seen)
{
	const double At = Seen.bNewSample ? Seen.MotionTime : Track.SampleTime;
	if (Seen.HitEvents != Track.HitEvents || Seen.bMotionExempt || !bFight)
	{
		Track.QuietUntil = FMath::Max(Track.QuietUntil, At + MotionQuiet);
		Track.HitEvents = Seen.HitEvents;
	}
	if (!Seen.bNewSample)
	{
		return;
	}
	const double Dt = Seen.MotionTime - Track.SampleTime;
	if (!Track.bHaveSample || Dt <= 0.0 || Dt > 30.0)
	{
		Track.bHaveSample = true;
		Track.SampleTime = Seen.MotionTime;
		Track.SampleAt = Seen.Location;
		Track.WindowStart = -1.0;
		return;
	}
	if (Seen.MotionTime >= Track.QuietUntil)
	{
		++SamplesJudged;
		const double Distance = FVector::Dist(Seen.Location, Track.SampleAt);
		if (Distance > JumpSlack + JumpSpeed * Dt)
		{
			Strike(1, Seen.Slot, TEXT("teleport"), FString::Printf(TEXT("%s moved %.0f cm in %.3f s (at most %.0f)"), *Seen.Name, Distance, Dt,
				JumpSlack + JumpSpeed * Dt), ClientNow);
		}
		if (Seen.bWalking)
		{
			if (Track.WindowStart < 0.0)
			{
				Track.WindowStart = Track.SampleTime;
				Track.WindowFrom = Track.SampleAt;
			}
			const double Span = Seen.MotionTime - Track.WindowStart;
			if (Span >= GroundWindow)
			{
				const double Speed = FMath::Abs(Seen.Location.X - Track.WindowFrom.X) / Span;
				if (Speed > GroundSpeed)
				{
					Strike(1, Seen.Slot, TEXT("speed"), FString::Printf(TEXT("%s ran %.0f cm/s on the ground for %.2f s (at most %.0f)"), *Seen.Name, Speed, Span,
						GroundSpeed), ClientNow);
				}
				Track.WindowStart = Seen.MotionTime;
				Track.WindowFrom = Seen.Location;
			}
		}
		else
		{
			Track.WindowStart = -1.0;
		}
	}
	else
	{
		Track.WindowStart = -1.0;
	}
	Track.SampleTime = Seen.MotionTime;
	Track.SampleAt = Seen.Location;
}

void FPinguCheatJudge::NotePickupGone(EPenguinAbility Ability, double ClientNow, const FVector& Where)
{
	FGone Gone;
	Gone.Ability = Ability;
	Gone.At = ClientNow;
	Gone.Where = Where;
	GonePickups.Add(Gone);
}

void FPinguCheatJudge::NoteShot(double ClientNow, double ServerNow, int32 Slot, EPenguinAbility Ability)
{
	// (not in the fight's first moments: a use from the countdown or the round before makes things then - a snowman built
	// 0.45 s after its cast)
	if (bDevScript || Slot < 0 || Ability == EPenguinAbility::None || !Tracks.Contains(Slot) || !bFight || FightAge < 2.f)
	{
		return;
	}
	FPending Pending;
	Pending.Kind = EPending::Shot;
	Pending.Slot = Slot;
	Pending.Ability = Ability;
	Pending.Since = ClientNow;
	Pending.ServerAt = ServerNow;
	Pendings.Add(Pending);
}

void FPinguCheatJudge::Tick(double ClientNow)
{
	// The loadouts: an ability that was not dealt needs a bubble that went away near the penguin, or a Jester swap.
	for (TPair<int32, FTrack>& Pair : Tracks)
	{
		FTrack& Track = Pair.Value;
		for (auto It = Track.Unearned.CreateIterator(); It; ++It)
		{
			const EPenguinAbility Ability = It.Key();
			const double Since = It.Value();
			if (bDevScript)
			{
				Track.Earned.Add(Ability); // (a script grant that came just before its dev flag)
				It.RemoveCurrent();
				continue;
			}
			bool bJester = false;
			for (int32 Index = 0; Index < Track.JesterBumps.Num(); ++Index)
			{
				if (FMath::Abs(Track.JesterBumps[Index] - Since) <= PickupWindow)
				{
					Track.JesterBumps.RemoveAt(Index);
					bJester = true;
					break;
				}
			}
			if (bJester || TakenNear(Track, Ability, Since, true))
			{
				Track.Earned.Add(Ability);
				Track.UseTimes.Remove(Ability); // (a bubble's ability is ready at once)
				It.RemoveCurrent();
				continue;
			}
			if (ClientNow - Since > PickupWindow)
			{
				const FString Kind = FString::Printf(TEXT("not dealt %s"), UPenguinAbilityComponent::GetInfo(Ability).Name);
				Strike(2, Pair.Key, *Kind, FString::Printf(TEXT("slot %d holds %s in round %d: not dealt, no bubble went away near it"), Pair.Key,
					UPenguinAbilityComponent::GetInfo(Ability).Name, Round), ClientNow);
				It.Value() = ClientNow; // (again after a while if it keeps it)
			}
		}
		Track.JesterBumps.RemoveAll([ClientNow](double At) { return ClientNow - At > 2.0 * PickupWindow; });
		Track.Counted.RemoveAll([ClientNow](const TPair<EPenguinAbility, double>& Entry) { return ClientNow - Entry.Value > 2.0 * ShotWindow; });
	}
	// What waited for its excuse.
	for (int32 Index = Pendings.Num() - 1; Index >= 0; --Index)
	{
		const FPending Pending = Pendings[Index];
		FTrack* Track = Tracks.Find(Pending.Slot);
		if (!Track)
		{
			Pendings.RemoveAt(Index);
			continue;
		}
		const bool bDue = ClientNow - Pending.Since > (Pending.Kind == EPending::Shot ? ShotWindow : PickupWindow);
		bool bSettled = false;
		if (Pending.Kind == EPending::Cut || Pending.Kind == EPending::Refund)
		{
			if (TakenNear(*Track, Pending.Ability, Pending.Since, true))
			{
				Track->UseTimes.Remove(Pending.Ability); // recharged by a bubble
				bSettled = true;
			}
			else if (Pending.bHitOk && HitNear(Pending.ServerAt, 1.5))
			{
				bSettled = true;
			}
			else if (Pending.bLeech && Track->LeechTimes.ContainsByPredicate([&Pending](double At) { return FMath::Abs(At - Pending.ServerAt) <= 1.5; }))
			{
				bSettled = true;
			}
			else if (bDue)
			{
				Strike(1, Pending.Slot, Pending.Kind == EPending::Cut ? TEXT("cooldown cut") : TEXT("no cooldown"), Pending.Detail, ClientNow);
				bSettled = true;
			}
		}
		else
		{
			// A shot: its penguin must have held the ability about then, and the host must have counted the use.
			// (held at any time this round: a snowman, a giant snowball's catch or a shot handed on can outlast its maker's
			// ability by a long while)
			const bool bHeld = Track->Held.Contains(Pending.Ability);
			const int32 Counted = Track->Counted.IndexOfByPredicate([&Pending](const TPair<EPenguinAbility, double>& Entry)
			{
				return Entry.Key == Pending.Ability && FMath::Abs(Entry.Value - Pending.Since) <= ShotWindow;
			});
			if (bHeld && Counted != INDEX_NONE)
			{
				Track->Counted.RemoveAt(Counted);
				bSettled = true;
			}
			else if (bDue)
			{
				const TCHAR* AbilityName = UPenguinAbilityComponent::GetInfo(Pending.Ability).Name;
				if (!bHeld)
				{
					// Once in a match can be a shot that changed hands in the frame it was made (seen in long bot matches);
					// three of one ability in a round it never had are a host using what it does not have.
					TArray<double>& Unheld = Track->Unheld.FindOrAdd(Pending.Ability);
					Unheld.Add(ClientNow);
					UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] slot %d made a %s it never had this round (%d)"), Pending.Slot, AbilityName, Unheld.Num());
					if (Unheld.Num() >= 3)
					{
						Strike(2, Pending.Slot, *FString::Printf(TEXT("shot not held %s"), AbilityName),
							FString::Printf(TEXT("slot %d made %d %ss this round and never had it"), Pending.Slot, Unheld.Num(), AbilityName), ClientNow);
					}
				}
				else
				{
					// A use the host never counted: once can be a corner case (a shot handed on in the very frame it was
					// made); three of one ability within 20 s are a hidden cooldown.
					TArray<double>& Uncounted = Track->Uncounted.FindOrAdd(Pending.Ability);
					Uncounted.Add(ClientNow);
					Uncounted.RemoveAll([ClientNow](double At) { return ClientNow - At > 20.0; });
					if (Uncounted.Num() >= 3)
					{
						Strike(2, Pending.Slot, TEXT("uses not counted"), FString::Printf(TEXT("slot %d made %d %ss within 20 s the host counted no use for"),
							Pending.Slot, Uncounted.Num(), AbilityName), ClientNow);
					}
				}
				bSettled = true;
			}
		}
		if (bSettled)
		{
			Pendings.RemoveAt(Index);
		}
	}
	// The old swap check is gone (a swap counter is the host's own number: 2026-11-05); keep the list empty.
	PendingSwaps.Reset();
	GonePickups.RemoveAll([ClientNow](const FGone& Entry) { return ClientNow - Entry.At > 2.0 * PickupWindow; });
	if (HitTimes.Num() > 256)
	{
		HitTimes.RemoveAt(0, HitTimes.Num() - 256);
	}
}

void FPinguCheatJudge::Strike(int32 InPoints, int32 Slot, const TCHAR* Kind, const FString& Detail, double ClientNow)
{
	const FString Key = FString::Printf(TEXT("%s|%d"), Kind, Slot);
	if (const double* Last = LastStrike.Find(Key))
	{
		if (ClientNow - *Last < StrikeSpacing && InPoints < LeaveAt)
		{
			return;
		}
	}
	LastStrike.Add(Key, ClientNow);
	Points += InPoints;
	const FString Line = FString::Printf(TEXT("%s: %s (+%d, %d of %d)"), Kind, *Detail, InPoints, Points, LeaveAt);
	Strikes.Add(Line);
	UE_LOG(LogPingu, Warning, TEXT("[ANTICHEAT] the host's game breaks the rules: %s"), *Line);
}

// ============================================================================ the reveal's checks (2026-11-05)

TArray<TPair<int32, FString>> PinguFair::CheckReveal(const FPinguFairReveal& Reveal, const FPinguRevealCheck& Check)
{
	using namespace PinguAntiCheatPrivate;
	TArray<TPair<int32, FString>> Out;
	const int32 Leave = FPinguCheatJudge::LeaveAt;
	if (Check.SeenHash.IsEmpty())
	{
		return Out; // (dealt from a commit that came before we did: nothing to hold it to)
	}
	if (CommitOf(Reveal.Secret, Reveal.Serial) != Check.SeenHash)
	{
		Out.Add({ Leave, FString::Printf(TEXT("round %d: the host's secret does not match its commit %d"), Reveal.Round, Reveal.Serial) });
	}
	TSet<int32> Seen;
	for (const FPinguFairContribution& Entry : Reveal.Contributions)
	{
		if (Seen.Contains(Entry.Player))
		{
			Out.Add({ Leave, FString::Printf(TEXT("round %d: two values under player %d"), Reveal.Round, Entry.Player) });
			continue;
		}
		Seen.Add(Entry.Player);
		if (Entry.Player == Check.OwnPlayer)
		{
			if (!Check.OwnValue.IsEmpty() && Entry.Value.ToUpper() != Check.OwnValue.ToUpper())
			{
				Out.Add({ Leave, FString::Printf(TEXT("round %d: the host changed the value we added"), Reveal.Round) });
			}
			continue;
		}
		if (!Check.KnownJoiners.Contains(Entry.Player))
		{
			// Not a joiner of this game: made up (or the host's own) - with it the host can grind the seed to a deal it likes.
			Out.Add({ Leave, FString::Printf(TEXT("round %d: a value under player %d, who is no joiner"), Reveal.Round, Entry.Player) });
			continue;
		}
		if (const FString* Shown = Check.SeenReceived.Find(Entry.Player))
		{
			if (Shown->ToUpper() != Entry.Value.ToUpper())
			{
				Out.Add({ Leave, FString::Printf(TEXT("round %d: player %d's value is not the one the host showed"), Reveal.Round, Entry.Player) });
			}
		}
	}
	for (const TPair<int32, FString>& Shown : Check.SeenReceived)
	{
		if (!Seen.Contains(Shown.Key) && Shown.Key != Check.OwnPlayer && !Check.LeftJoiners.Contains(Shown.Key))
		{
			Out.Add({ Leave, FString::Printf(TEXT("round %d: player %d's value was received and then left out"), Reveal.Round, Shown.Key) });
		}
	}
	if (!Check.OwnValue.IsEmpty() && !Seen.Contains(Check.OwnPlayer))
	{
		if (Check.SeenReceived.Contains(Check.OwnPlayer))
		{
			Out.Add({ Leave, FString::Printf(TEXT("round %d: the host received our value and left it out"), Reveal.Round) });
		}
		else if (Check.OwnSentAgo >= ContributionLead)
		{
			Out.Add({ 1, FString::Printf(TEXT("round %d: the host left out the value we sent %.1f s before"), Reveal.Round, Check.OwnSentAgo) });
		}
	}
	if (Check.RoundsDealt.Contains(Reveal.Round))
	{
		Out.Add({ Leave, FString::Printf(TEXT("round %d was dealt a second time in this match"), Reveal.Round) });
	}
	if (Check.PrevRound >= 0 && Check.PrevRound == Reveal.Round - 1 && Check.bPrevTraits && Reveal.bTraits && Reveal.TraitBag != Check.PrevBagAfter)
	{
		Out.Add({ Leave, FString::Printf(TEXT("round %d: the trait bag is not what round %d left"), Reveal.Round, Check.PrevRound) });
	}
	// Ability picks (2026-11-07, card ability-picks). A host may deal picks only as the lobby's setting says, never for a bot,
	// never for us other than we asked, never changed within a match (the lobby is the only place to pick).
	const bool bPicksShaped = Reveal.Picks.Num() == 0 || Reveal.Picks.Num() == Reveal.Slots.Num() * PinguPicks::NumSlots;
	if (!bPicksShaped)
	{
		Out.Add({ Leave, FString::Printf(TEXT("round %d: picks for %d slots, %d penguins dealt"), Reveal.Round, Reveal.Picks.Num() / PinguPicks::NumSlots, Reveal.Slots.Num()) });
	}
	if (Check.ChooseSeen != 0 && (Check.ChooseSeen & (Reveal.bChoose ? 2 : 1)) == 0)
	{
		Out.Add({ Leave, FString::Printf(TEXT("round %d: dealt as abilities %s, the lobby said %s"), Reveal.Round, Reveal.bChoose ? TEXT("CHOOSE") : TEXT("RANDOM"),
			Reveal.bChoose ? TEXT("RANDOM") : TEXT("CHOOSE")) });
	}
	if (Check.MatchChoose >= 0 && (Check.MatchChoose != 0) != Reveal.bChoose)
	{
		Out.Add({ Leave, FString::Printf(TEXT("round %d: abilities RANDOM / CHOOSE changed within the match"), Reveal.Round) });
	}
	for (int32 Index = 0; bPicksShaped && Index < Reveal.Slots.Num(); ++Index)
	{
		const int32 Slot = Reveal.Slots[Index];
		TArray<uint8> Picks;
		if (Reveal.Picks.Num() > 0)
		{
			Picks.Append(Reveal.Picks.GetData() + Index * PinguPicks::NumSlots, PinguPicks::NumSlots);
		}
		const bool bAny = PinguPicks::HasAny(Picks);
		if (!PinguPicks::IsValid(Picks))
		{
			Out.Add({ Leave, FString::Printf(TEXT("round %d: slot %d's picks are no three different abilities"), Reveal.Round, Slot) });
			continue;
		}
		if (bAny && !Reveal.bChoose)
		{
			Out.Add({ Leave, FString::Printf(TEXT("round %d: slot %d has picks (%s) with abilities RANDOM"), Reveal.Round, Slot, *PinguPicks::Describe(Picks)) });
		}
		if (bAny && Check.BotSlots.Contains(Slot))
		{
			Out.Add({ Leave, FString::Printf(TEXT("round %d: bot slot %d has picks (%s)"), Reveal.Round, Slot, *PinguPicks::Describe(Picks)) });
		}
		if (Slot == Check.OwnSlot && Check.OwnSlot >= 0)
		{
			const TArray<uint8> Mine = bAny ? Picks : TArray<uint8>();
			const bool bAsked = !bAny || Check.OwnPicksSent.ContainsByPredicate([&Mine](const TArray<uint8>& Sent)
				{
					return (PinguPicks::HasAny(Sent) ? Sent : TArray<uint8>()) == Mine;
				});
			if (!bAsked)
			{
				Out.Add({ Leave, FString::Printf(TEXT("round %d: our picks dealt as %s, we never asked for that"), Reveal.Round, *PinguPicks::Describe(Picks)) });
			}
		}
		if (const TArray<uint8>* Before = Check.MatchPicks.Find(Slot))
		{
			// (Picks to none is harmless - a person left and a late joiner took the slot; none to picks or other picks is not.)
			if (bAny && *Before != Picks)
			{
				Out.Add({ Leave, FString::Printf(TEXT("round %d: slot %d's picks changed within the match (%s -> %s)"), Reveal.Round, Slot,
					*PinguPicks::Describe(*Before), *PinguPicks::Describe(Picks)) });
			}
		}
	}
	return Out;
}

bool PinguFair::IsCommitStepFine(int32 LastSeenSerial, int32 NewSerial, int32 RevealSerialNow)
{
	return LastSeenSerial <= 0 || (NewSerial == LastSeenSerial + 1 && RevealSerialNow == LastSeenSerial);
}

// ============================================================================ switches and the host's gates

bool PinguAntiCheat::IsHostPickCheat()
{
	return PinguAntiCheatPrivate::HasCheat(TEXT("picks"));
}

bool PinguAntiCheat::IsHostCooldownCheat(const APenguinCharacter* Penguin)
{
#if UE_BUILD_SHIPPING
	return false;
#else
	return Penguin && PinguAntiCheatPrivate::HasCheat(TEXT("cooldown")) && Penguin->GetNetMode() == NM_ListenServer && Penguin->IsLocallyControlled();
#endif
}

bool PinguAntiCheat::IsClientCheat()
{
	return PinguAntiCheatPrivate::HasCheat(TEXT("client"));
}

bool PinguAntiCheat::IsAnyCheat()
{
	return IsHostPickCheat() || PinguAntiCheatPrivate::HasCheat(TEXT("cooldown")) || IsClientCheat() || IsDevTrainer();
}

bool PinguAntiCheat::IsDevTrainer()
{
	return PinguAntiCheatPrivate::HasCheat(TEXT("trainer"));
}

bool PinguAntiCheat::IsDevNoGuard()
{
	return PinguAntiCheatPrivate::HasCheat(TEXT("noguard"));
}

bool PinguAntiCheat::AllowRequest(const UObject* Who, EGate Gate)
{
	using namespace PinguAntiCheatPrivate;
	if (!Who)
	{
		return false;
	}
	FJoinerState& Joiner = JoinerOf(Who);
	const int32 Index = static_cast<int32>(Gate);
	FGateState& State = Joiner.Gates[Index];
	const double Now = FPlatformTime::Seconds();
	if (State.Tokens < 0.0)
	{
		State.Tokens = GateBurst[Index];
		State.Last = Now;
	}
	State.Tokens = FMath::Min<double>(GateBurst[Index], State.Tokens + (Now - State.Last) * GateRate[Index]);
	State.Last = Now;
	if (State.Tokens < 1.0)
	{
		static const TCHAR* GateNames[] = { TEXT("ability requests"), TEXT("aim updates"), TEXT("lobby requests"), TEXT("seed values") };
		NoteIgnored(Who, GateNames[Index], FString::Printf(TEXT("more than %.0f a second"), GateRate[Index]));
		return false;
	}
	State.Tokens -= 1.0;
	return true;
}

void PinguAntiCheat::NoteIgnored(const UObject* Who, const TCHAR* What, const FString& Detail)
{
	using namespace PinguAntiCheatPrivate;
	++IgnoredCount;
	if (IgnoredLines.Num() < 20)
	{
		IgnoredLines.Add(FString::Printf(TEXT("%s: %s (%s)"), *GetNameSafe(Who), What, *Detail));
	}
	FJoinerState& Joiner = JoinerOf(Who);
	const double Now = FPlatformTime::Seconds();
	if (Now - Joiner.LastLog < 1.0)
	{
		++Joiner.Unlogged;
		return;
	}
	UE_LOG(LogPingu, Warning, TEXT("[ANTICHEAT] host ignored %s: %s (%s)%s"), *GetNameSafe(Who), What, *Detail,
		Joiner.Unlogged > 0 ? *FString::Printf(TEXT("; %d more since the last line"), Joiner.Unlogged) : TEXT(""));
	Joiner.LastLog = Now;
	Joiner.Unlogged = 0;
}

int32 PinguAntiCheat::GetIgnoredCount()
{
	return PinguAntiCheatPrivate::IgnoredCount;
}

const TArray<FString>& PinguAntiCheat::GetIgnoredLines()
{
	return PinguAntiCheatPrivate::IgnoredLines;
}

int32 PinguAntiCheat::GetIgnoredMoves()
{
	return PinguAntiCheatPrivate::IgnoredMoves;
}

float PinguAntiCheat::ClampHold(const UObject* Who, float ClaimedSeconds, double ServerSeenSeconds)
{
	const double Most = FMath::Max(0.0, ServerSeenSeconds) + PinguAntiCheatPrivate::HoldSlack;
	if (ClaimedSeconds > Most)
	{
		NoteIgnored(Who, TEXT("an icicle hold"), FString::Printf(TEXT("%.2f s claimed, the host saw %.2f s: cut"), ClaimedSeconds, ServerSeenSeconds));
		return static_cast<float>(Most);
	}
	return ClaimedSeconds;
}

bool PinguAntiCheat::AcceptMove(const UObject* Who, const FVector& ServerLoc, const FVector& ClientLoc, bool bWalking, bool bMovedByHost, bool bPushed,
	double Now, bool& bOutPullBack)
{
	using namespace PinguAntiCheatPrivate;
	bOutPullBack = false;
	FJoinerState& Joiner = JoinerOf(Who);
	// The time this move covers: since the joiner's previous move arrived (refused or not; at most 0.25 s, so a joiner that
	// goes quiet can't save up a jump).
	const double Dt = Joiner.bHaveMove ? FMath::Clamp(Now - Joiner.LastMoveTime, 0.0, 0.25) : 0.0;
	Joiner.LastMoveTime = Now;
	if (!Joiner.bHaveMove || bMovedByHost || ClientLoc.ContainsNaN())
	{
		Joiner.bHaveMove = !ClientLoc.ContainsNaN();
		Joiner.RefusedSince = -1.0;
		Joiner.WindowStart = -1.0;
		return !ClientLoc.ContainsNaN();
	}
	const double Distance = FVector::Dist(ServerLoc, ClientLoc);
	const double Allowed = (MoveSlack + MoveSpeed * Dt) * (bPushed ? 3.0 : 1.0);
	FString Why;
	if (Distance > Allowed)
	{
		Why = FString::Printf(TEXT("a jump of %.0f cm in %.3f s (at most %.0f)"), Distance, Dt, Allowed);
	}
	else if (bWalking && !bPushed)
	{
		if (Joiner.WindowStart < 0.0)
		{
			Joiner.WindowStart = Now - Dt;
			Joiner.WindowFrom = ServerLoc;
		}
		const double Span = Now - Joiner.WindowStart;
		if (Span >= MoveGroundWindow)
		{
			const double Speed = FMath::Abs(ClientLoc.X - Joiner.WindowFrom.X) / Span;
			if (Speed > MoveGroundSpeed)
			{
				Why = FString::Printf(TEXT("%.0f cm/s on the ground for %.2f s"), Speed, Span);
			}
			Joiner.WindowStart = Now;
			Joiner.WindowFrom = ClientLoc;
		}
	}
	else
	{
		Joiner.WindowStart = -1.0;
	}
	if (Why.IsEmpty())
	{
		Joiner.RefusedSince = -1.0;
		return true;
	}
	++IgnoredMoves;
	NoteIgnored(Who, TEXT("a move"), Why);
	Joiner.WindowStart = -1.0;
	if (Joiner.RefusedSince < 0.0)
	{
		Joiner.RefusedSince = Now;
	}
	if (Now - Joiner.RefusedSince >= PullBackAfter - 0.001)
	{
		// The host moves it back to where it has it (an honest machine never gets here); still judged against that place.
		bOutPullBack = true;
		Joiner.RefusedSince = -1.0;
	}
	return false;
}

// ============================================================================ the join handshake

FString PinguAntiCheat::GetBuildId()
{
	// pingu-ac5 (2026-11-07, card ability-picks): the reveal carries the ability picks and every slot rolls from its own stream.
	// pingu-ac4 (2026-11-05, card host-cheat-fix): the memory guards and the stricter judge; older games can't join.
	return FString::Printf(TEXT("%s|%s|pingu-ac5"), *FEngineVersion::Current().ToString(EVersionComponent::Patch), LexToString(FApp::GetBuildConfiguration()));
}

void PinguAntiCheat::StartFileDigest()
{
	using namespace PinguAntiCheatPrivate;
	PinguTamper::Init(); // capture the startup tamper state (a no-op outside a packaged Shipping game)
	FDigest& State = Digest();
	{
		FScopeLock Guard(&State.Lock);
		if (State.bStarted)
		{
			return;
		}
		State.bStarted = true;
	}
	// What the game is made of: a packaged game's exe and its paks; an editor run's game module (its content is not cooked).
	TArray<FString> Files;
	if (FPlatformProperties::RequiresCookedData())
	{
		Files.Add(FPlatformProcess::ExecutablePath());
		const FString PakDir = FPaths::ProjectContentDir() / TEXT("Paks");
		for (const TCHAR* Pattern : { TEXT("*.pak"), TEXT("*.utoc"), TEXT("*.ucas"), TEXT("*.sig") })
		{
			TArray<FString> Found;
			IFileManager::Get().FindFiles(Found, *(PakDir / Pattern), true, false);
			for (const FString& Name : Found)
			{
				Files.Add(PakDir / Name);
			}
		}
		// The staging file lists at the build root: PinguTamper reads them to tell our DLLs from foreign ones, so editing
		// them (to slip a DLL in) changes the answer too. Only those present (a copy without them hashes without them).
		for (const TCHAR* Manifest : { TEXT("Manifest_NonUFSFiles_Win64.txt"), TEXT("Manifest_UFSFiles_Win64.txt") })
		{
			const FString Path = FPaths::RootDir() / Manifest;
			if (IFileManager::Get().FileExists(*Path))
			{
				Files.Add(Path);
			}
		}
	}
	else
	{
#if IS_MONOLITHIC
		// (A monolithic build has the game code inside the exe.)
		Files.Add(FPlatformProcess::ExecutablePath());
#else
		const FString Module = FModuleManager::Get().GetModuleFilename(TEXT("Penguin"));
		Files.Add(Module.IsEmpty() ? FString(FPlatformProcess::ExecutablePath()) : Module);
#endif
	}
	Files.Sort([](const FString& A, const FString& B) { return FPaths::GetCleanFilename(A) < FPaths::GetCleanFilename(B); });
	{
		FScopeLock Guard(&State.Lock);
		State.Paths = Files;
		State.Sizes.Reset();
		for (const FString& Path : Files)
		{
			State.Sizes.Add(IFileManager::Get().FileSize(*Path));
		}
	}
	UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] hashing %d game file(s) for the join check"), Files.Num());
	Async(EAsyncExecution::Thread, [Files]()
	{
		const double Start = FPlatformTime::Seconds();
		FString Combined = PinguAntiCheat::GetBuildId();
		FString Description;
		int64 Total = 0;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(1 << 20);
		for (const FString& Path : Files)
		{
			FSHA1 Sha;
			int64 Size = 0;
			if (TUniquePtr<FArchive> Reader = TUniquePtr<FArchive>(IFileManager::Get().CreateFileReader(*Path, FILEREAD_AllowWrite)))
			{
				Size = Reader->TotalSize();
				int64 Left = Size;
				while (Left > 0 && !Reader->IsError())
				{
					const int64 Chunk = FMath::Min<int64>(Left, Buffer.Num());
					Reader->Serialize(Buffer.GetData(), Chunk);
					Sha.Update(Buffer.GetData(), static_cast<uint64>(Chunk));
					Left -= Chunk;
				}
			}
			Sha.Final();
			uint8 Hash[FSHA1::DigestSize];
			Sha.GetHash(Hash);
			const FString Name = FPaths::GetCleanFilename(Path);
			Combined += FString::Printf(TEXT("|%s:%lld:%s"), *Name, Size, *BytesToHex(Hash, FSHA1::DigestSize));
			Description += FString::Printf(TEXT("%s%s (%.1f MB)"), Description.IsEmpty() ? TEXT("") : TEXT(", "), *Name, Size / 1048576.0);
			Total += Size;
		}
		const FString Result = HashHex(Combined);
		FDigest& Done = Digest();
		{
			FScopeLock Guard(&Done.Lock);
			Done.Digest = Result;
			Done.Description = Description;
		}
		Done.bReady = true;
		UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] game files hashed in %.1f s (%.0f MB): %s"), FPlatformTime::Seconds() - Start, Total / 1048576.0, *Description);
	});
}

TArray<PinguAntiCheat::FFileRange> PinguAntiCheat::DeriveRanges(const FString& Nonce, const TArray<int64>& FileSizes)
{
	constexpr int32 RangeCount = 6;
	constexpr int64 MinLength = 64 * 1024;
	constexpr int64 MaxLength = 256 * 1024;
	TArray<FFileRange> Out;
	int64 Total = 0;
	for (const int64 Size : FileSizes)
	{
		Total += FMath::Max<int64>(Size, 0);
	}
	if (Total <= 0)
	{
		return Out;
	}
	uint8 Hash[FSHA1::DigestSize];
	const FTCHARToUTF8 Bytes(*FString::Printf(TEXT("pingu-ranges|%s"), *Nonce.ToUpper()));
	FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Hash);
	int32 Seed = 0;
	FMemory::Memcpy(&Seed, Hash, sizeof(Seed));
	FRandomStream Stream(Seed);
	for (int32 Index = 0; Index < RangeCount; ++Index)
	{
		// A random byte of all the files together (so big files are asked about more), then a stretch from there on.
		int64 Position = static_cast<int64>(Stream.FRand() * static_cast<double>(Total));
		Position = FMath::Clamp<int64>(Position, 0, Total - 1);
		const int64 Wanted = MinLength + static_cast<int64>(Stream.FRand() * static_cast<double>(MaxLength - MinLength));
		for (int32 File = 0; File < FileSizes.Num(); ++File)
		{
			const int64 Size = FMath::Max<int64>(FileSizes[File], 0);
			if (Position >= Size)
			{
				Position -= Size;
				continue;
			}
			FFileRange Range;
			Range.File = File;
			Range.Length = FMath::Min(Wanted, Size);
			Range.Offset = FMath::Min(Position, Size - Range.Length);
			Out.Add(Range);
			break;
		}
	}
	return Out;
}

bool PinguAntiCheat::AnswerNonce(const FString& Nonce, FString& OutAnswer)
{
	using namespace PinguAntiCheatPrivate;
	StartFileDigest();
	FDigest& State = Digest();
	if (!State.bReady)
	{
		return false;
	}
	const FString Key = Nonce.ToUpper();
	FString Value;
	FString Ranges;
	TArray<FString> Paths;
	TArray<int64> Sizes;
	{
		FScopeLock Guard(&State.Lock);
		Value = State.Digest;
		if (const FString* Known = State.RangeHashes.Find(Key))
		{
			Ranges = *Known;
		}
		else
		{
			Paths = State.Paths;
			Sizes = State.Sizes;
		}
	}
	if (Ranges.IsEmpty())
	{
		// The stretches of the files this nonce asks for, read from disk now: a recorded answer to another nonce is no help.
		for (const FFileRange& Range : DeriveRanges(Key, Sizes))
		{
			FSHA1 Sha;
			if (TUniquePtr<FArchive> Reader = TUniquePtr<FArchive>(IFileManager::Get().CreateFileReader(*Paths[Range.File], FILEREAD_AllowWrite)))
			{
				TArray<uint8> Buffer;
				Buffer.SetNumUninitialized(static_cast<int32>(Range.Length));
				Reader->Seek(Range.Offset);
				Reader->Serialize(Buffer.GetData(), Range.Length);
				Sha.Update(Buffer.GetData(), static_cast<uint64>(Range.Length));
			}
			Sha.Final();
			uint8 Hash[FSHA1::DigestSize];
			Sha.GetHash(Hash);
			Ranges += FString::Printf(TEXT("%d:%lld:%lld:%s|"), Range.File, Range.Offset, Range.Length, *BytesToHex(Hash, FSHA1::DigestSize));
		}
		if (Ranges.IsEmpty())
		{
			Ranges = TEXT("none");
		}
		FScopeLock Guard(&State.Lock);
		if (State.RangeHashes.Num() > 64)
		{
			State.RangeHashes.Reset();
		}
		State.RangeHashes.Add(Key, Ranges);
	}
	// A tampered packaged game (a patched exe / pak, a foreign DLL in the game folder, a debugger) folds a non-empty
	// token in here, so its answer silently fails to match an honest peer's and it is refused / left. Honest builds use
	// an empty token, so their answers match. (The dev switch -PinguCheat=tamper fakes it for the LAN test.)
	FString Tamper = PinguTamper::StartupToken();
#if !UE_BUILD_SHIPPING
	if (PinguAntiCheatPrivate::HasCheat(TEXT("tamper")))
	{
		Tamper = TEXT("t-dev");
	}
#endif
	OutAnswer = HashHex(FString::Printf(TEXT("pingu-answer|%s|%s|%s|%s|%s"), *Key, *GetBuildId(), *Value, *Ranges, *Tamper));
	return true;
}

FString PinguAntiCheat::DescribeDigest()
{
	using namespace PinguAntiCheatPrivate;
	FDigest& State = Digest();
	FScopeLock Guard(&State.Lock);
	return State.Description;
}

void PinguAntiCheat::HostOnAnswer(APlayerController* Joiner, const FString& Answer, const FString& JoinerNonce, const FString& JoinerBuildId)
{
	using namespace PinguAntiCheatPrivate;
	if (!Joiner || !AllowRequest(Joiner, EGate::Lobby))
	{
		return;
	}
	FHostShake* Shake = HostShakes().Find(Joiner);
	if (!Shake || Shake->bDone || Shake->bAnswerWaiting)
	{
		return;
	}
	Shake->bAnswerWaiting = true;
	Shake->Answer = Answer.Left(128);
	Shake->JoinerNonce = JoinerNonce.Left(128);
	Shake->JoinerBuildId = JoinerBuildId.Left(200);
	// Judged in the host's tick (APinguAntiCheat::HostTick), as soon as our own files are hashed.
}

void PinguAntiCheat::ClientOnHello(APlayerController* Own, const FString& HostNonce, const FString& HostBuildId)
{
	using namespace PinguAntiCheatPrivate;
	if (!Own)
	{
		return;
	}
	FClientShake& Shake = ClientShakes().FindOrAdd(Own);
	if (Shake.bHello)
	{
		return;
	}
	Shake.bHello = true;
	Shake.HostNonce = HostNonce.Left(128);
	Shake.HostBuildId = HostBuildId.Left(200);
	StartFileDigest();
}

void PinguAntiCheat::ClientOnHostAnswer(APlayerController* Own, const FString& Answer)
{
	using namespace PinguAntiCheatPrivate;
	FClientShake* Shake = Own ? ClientShakes().Find(Own) : nullptr;
	if (!Shake || !Shake->bAnswered || Shake->bDone)
	{
		return;
	}
	FString Expected;
	if (!AnswerNonce(Shake->OwnNonce, Expected))
	{
		return; // (can't be: we answered, so ours is hashed)
	}
	Shake->bDone = true;
	if (Answer != Expected)
	{
		LeaveQuietly(Own, TEXT("the host's game files differ from ours (its answer to our nonce is wrong)"));
		return;
	}
	Report().bHandshakeOk = true;
	UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] join check passed: the host has the same build and files (%s)"), *DescribeDigest());
}

void PinguAntiCheat::ClientOnRefused(APlayerController* Own)
{
	if (PinguAntiCheatPrivate::FClientShake* Shake = Own ? PinguAntiCheatPrivate::ClientShakes().Find(Own) : nullptr)
	{
		Shake->bDone = true;
	}
	LeaveQuietly(Own, TEXT("the host says our build or game files differ from its"));
}

void PinguAntiCheat::TickClientHandshake(APlayerController* Own, float DeltaTime)
{
	using namespace PinguAntiCheatPrivate;
	if (!Own || Own->GetNetMode() != NM_Client)
	{
		return;
	}
	FClientShake& Shake = ClientShakes().FindOrAdd(Own);
	const double Now = FPlatformTime::Seconds();
	if (Shake.FirstTick < 0.0)
	{
		Shake.FirstTick = Now;
		StartFileDigest();
	}
	if (Shake.bDone)
	{
		return;
	}
	if (!Shake.bHello)
	{
		if (Now - Shake.FirstTick > HelloTimeout)
		{
			Shake.bDone = true;
			LeaveQuietly(Own, TEXT("the host never sent its join check (another version of the game)"));
		}
		return;
	}
	if (!Shake.bAnswered)
	{
		if (Shake.HostBuildId != GetBuildId())
		{
			Shake.bDone = true;
			LeaveQuietly(Own, FString::Printf(TEXT("the host's build is \"%s\", ours \"%s\""), *Shake.HostBuildId, *GetBuildId()));
			return;
		}
		FString Answer;
		if (!AnswerNonce(Shake.HostNonce, Answer))
		{
			return; // our files are still being hashed
		}
#if !UE_BUILD_SHIPPING
		if (HasCheat(TEXT("badhash")))
		{
			Answer = HashHex(Answer + TEXT("|a client that hashed other files")); // -PinguCheat=badhash: an answer no host can match
		}
#endif
		Shake.OwnNonce = PinguFair::MakeSecret();
		Shake.bAnswered = true;
		Shake.AnsweredAt = Now;
		if (APenguinPlayerController* PenguinController = Cast<APenguinPlayerController>(Own))
		{
			PenguinController->Rq19(Answer, Shake.OwnNonce, GetBuildId());
		}
		return;
	}
	if (Now - Shake.AnsweredAt > AnswerTimeout)
	{
		Shake.bDone = true;
		LeaveQuietly(Own, TEXT("the host never answered our join check"));
	}
}

void PinguAntiCheat::LeaveQuietly(const UObject* WorldContext, const FString& Why)
{
	using namespace PinguAntiCheatPrivate;
	FClientReport& Out = Report();
	if (Out.bLeft)
	{
		return;
	}
	Out.bLeft = true;
	Out.LeftWhy = Why;
	UE_LOG(LogPingu, Warning, TEXT("[ANTICHEAT] leaving the game: %s"), *Why);
	for (const FString& Line : Out.Strikes)
	{
		UE_LOG(LogPingu, Warning, TEXT("[ANTICHEAT]   seen: %s"), *Line);
	}
	if (UPinguNetSubsystem* Net = UPinguNetSubsystem::Get(WorldContext))
	{
		Net->LeaveWithError(FText::FromString(TEXT("Your games don't match.")));
	}
}

const PinguAntiCheat::FClientReport& PinguAntiCheat::GetClientReport()
{
	return PinguAntiCheatPrivate::Report();
}

int32 PinguAntiCheat::GetHandshakesOk()
{
	return PinguAntiCheatPrivate::HandshakesOk;
}

int32 PinguAntiCheat::GetHandshakesFailed()
{
	return PinguAntiCheatPrivate::HandshakesFailed;
}

// ============================================================================ the actor

APinguAntiCheat::APinguAntiCheat()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10.f);
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

APinguAntiCheat* APinguAntiCheat::Get(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<APinguAntiCheat> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

APinguAntiCheat* APinguAntiCheat::GetOrSpawn(UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Standalone || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	if (APinguAntiCheat* Existing = Get(World))
	{
		return Existing;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguAntiCheat* Spawned = World->SpawnActor<APinguAntiCheat>(Params);
	PinguAntiCheat::StartFileDigest();
	return Spawned;
}

void APinguAntiCheat::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguAntiCheat, Pc); // Commit
	DOREPLIFETIME(APinguAntiCheat, Pr); // Reveal
	DOREPLIFETIME(APinguAntiCheat, Pv); // Received
	DOREPLIFETIME(APinguAntiCheat, HostSlot);
	DOREPLIFETIME(APinguAntiCheat, DevQuietUntil);
	DOREPLIFETIME(APinguAntiCheat, DevScriptUntil);
}

void APinguAntiCheat::DevQuietScript(UWorld* World, float Seconds)
{
#if !UE_BUILD_SHIPPING
	if (APinguAntiCheat* Self = World ? Get(World) : nullptr)
	{
		if (Self->bSelfJudge)
		{
			return;
		}
		const float Until = static_cast<float>(World->GetTimeSeconds()) + Seconds;
		if (Until > Self->DevScriptUntil + 0.5f)
		{
			Self->DevScriptUntil = Until;
		}
	}
#endif
}

void APinguAntiCheat::DevQuietMotion(UWorld* World, float Seconds)
{
#if !UE_BUILD_SHIPPING
	if (APinguAntiCheat* Self = World ? Get(World) : nullptr)
	{
		const float Until = static_cast<float>(World->GetTimeSeconds()) + Seconds;
		if (Until > Self->DevQuietUntil + 0.5f)
		{
			Self->DevQuietUntil = Until;
		}
	}
#endif
}

void APinguAntiCheat::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bSelfJudge)
	{
		SelfJudgeTick(); // (tests: a solo world's penguins through a joiner's eyes)
		return;
	}
	if (HasAuthority())
	{
		HostTick();
	}
	else
	{
		ClientTick(DeltaSeconds);
	}
}

void APinguAntiCheat::HostMakeCommit(int32 Serial)
{
	Secret = PinguFair::MakeSecret();
	Commit.Serial = Serial;
	Commit.Hash = PinguFair::CommitOf(Secret, Serial);
	Received.Reset();
	ForceNetUpdate();
}

bool APinguAntiCheat::HostDeal(UWorld* World, int32 Round, const TArray<uint8>& Slots, bool bTraits, const TArray<EPinguTrait>& Forced,
	uint64 TraitsOff, TArray<EPinguTrait>& InOutBag, FPinguFairDeal& OutDeal, bool bPicks)
{
	APinguAntiCheat* Self = GetOrSpawn(World);
	if (!Self)
	{
		return false;
	}
	if (Self->Commit.Serial == 0)
	{
		Self->HostMakeCommit(1);
	}
	FPinguFairReveal Out;
	Out.Serial = Self->Commit.Serial;
	Out.Round = Round;
	Out.Secret = Self->Secret;
	Out.Contributions = Self->Received;
	Out.Slots = Slots;
	Out.Slots.Sort();
	Out.bTraits = bTraits;
	for (const EPinguTrait Trait : InOutBag)
	{
		Out.TraitBag.Add(static_cast<uint8>(Trait));
	}
	for (const EPinguTrait Trait : Forced)
	{
		Out.ForcedTraits.Add(static_cast<uint8>(Trait));
	}
	Out.TraitsOff = TraitsOff;
	// Abilities CHOOSE (a match's round): every person's picks, by slot (bots and RANDOM: none).
	Out.bChoose = bPicks && PinguPicks::IsChooseOn(World);
	if (Out.bChoose)
	{
		TArray<uint8> Picks;
		Picks.Init(0, Out.Slots.Num() * PinguPicks::NumSlots);
		bool bAny = false;
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			const int32 Index = Out.Slots.IndexOfByKey(static_cast<uint8>(FMath::Clamp(It->GetPlayerSlot(), 0, 255)));
			const TArray<uint8> Mine = PinguPicks::GetPicksFor(*It);
			if (Index != INDEX_NONE && Mine.Num() == PinguPicks::NumSlots)
			{
				FMemory::Memcpy(Picks.GetData() + Index * PinguPicks::NumSlots, Mine.GetData(), PinguPicks::NumSlots);
				bAny = true;
			}
		}
		if (bAny)
		{
			Out.Picks = MoveTemp(Picks);
		}
	}
	PinguFair::Deal(Out, OutDeal);
	if (bTraits)
	{
		InOutBag = OutDeal.BagAfter;
	}
	Self->Reveal = Out;
	UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] round %d dealt from commit %d with %d joiner value(s), seed %d"), Round, Out.Serial,
		Out.Contributions.Num(), PinguFair::SeedOf(Out));
	Self->HostMakeCommit(Out.Serial + 1);
	return true;
}

void APinguAntiCheat::HostReceive(APlayerController* From, int32 Serial, const FString& Value)
{
	if (!From || !PinguAntiCheat::AllowRequest(From, PinguAntiCheat::EGate::Fair))
	{
		return;
	}
	if (Serial != Commit.Serial)
	{
		return; // (one for a commit that is already dealt: it crossed the new commit on the way)
	}
	const APlayerState* State = From->GetPlayerState<APlayerState>();
	if (!State || !PinguFair::IsValidValue(Value))
	{
		PinguAntiCheat::NoteIgnored(From, TEXT("a seed value"), TEXT("not hex"));
		return;
	}
	const int32 Player = State->GetPlayerId();
	if (Received.ContainsByPredicate([Player](const FPinguFairContribution& Entry) { return Entry.Player == Player; }))
	{
		return; // the first one counts
	}
	FPinguFairContribution Entry;
	Entry.Player = Player;
	Entry.Value = Value.ToUpper();
	Received.Add(Entry);
	ForceNetUpdate();
}

void APinguAntiCheat::HostForgetPlayer(int32 Player)
{
	if (Received.RemoveAll([Player](const FPinguFairContribution& Entry) { return Entry.Player == Player; }) > 0)
	{
		ForceNetUpdate();
	}
}

void APinguAntiCheat::HostTick()
{
	using namespace PinguAntiCheatPrivate;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (Commit.Serial == 0)
	{
		HostMakeCommit(1);
	}
	// The host's own penguin (the joiners judge its movement; the bots' they know as bots).
	int32 Slot = -1;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();
		if (Controller && Controller->IsLocalController())
		{
			if (const APenguinCharacter* Penguin = Cast<APenguinCharacter>(Controller->GetPawn()))
			{
				Slot = Penguin->GetPlayerSlot();
			}
			break;
		}
	}
	if (HostSlot != Slot)
	{
		HostSlot = Slot;
	}

	// The joiners' hellos: a nonce each, their answer judged as soon as our own files are hashed.
	const double Now = FPlatformTime::Seconds();
	TMap<TWeakObjectPtr<APlayerController>, FHostShake>& Shakes = HostShakes();
	for (auto It = Shakes.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APenguinPlayerController* Controller = Cast<APenguinPlayerController>(It->Get());
		if (!Controller || Controller->IsLocalController() || !Controller->GetNetConnection())
		{
			continue;
		}
		FHostShake* Shake = Shakes.Find(Controller);
		if (!Shake)
		{
			FHostShake New;
			New.Nonce = PinguFair::MakeSecret();
			New.SentAt = Now;
			Shake = &Shakes.Add(Controller, New);
			Controller->Rq18(New.Nonce, PinguAntiCheat::GetBuildId());
			continue;
		}
		if (Shake->KickAt >= 0.0)
		{
			if (Now >= Shake->KickAt)
			{
				Shake->KickAt = -1.0;
				AGameModeBase* GameMode = World->GetAuthGameMode();
				if (GameMode && GameMode->GameSession)
				{
					GameMode->GameSession->KickPlayer(Controller, FText::FromString(TEXT("Your games don't match.")));
				}
			}
			continue;
		}
#if !UE_BUILD_SHIPPING
		// -PinguCheat=kick (the LAN test): the host removes a joiner 12 s after it passed the join check, as the lobby's x would.
		// (Once: the kicked joiner comes back at once - 2026-11-07, no ban - and must stay this time.)
		static bool bDevKickDone = false;
		if (Shake->PassedAt > 0.0 && Now - Shake->PassedAt > 12.0 && HasCheat(TEXT("kick")) && !bDevKickDone)
		{
			bDevKickDone = true;
			Shake->PassedAt = -1.0;
			if (APenguinGameMode* PenguinMode = World->GetAuthGameMode<APenguinGameMode>())
			{
				PenguinMode->KickPlayer(Controller->GetPlayerState<APenguinPlayerState>());
			}
		}
#endif
		if (Shake->bDone)
		{
			continue;
		}
		bool bRefuse = false;
		FString Why;
		if (Shake->bAnswerWaiting)
		{
			FString Want;
			if (!PinguAntiCheat::AnswerNonce(Shake->Nonce, Want))
			{
				continue;
			}
			Shake->bDone = true;
			if (Shake->JoinerBuildId != PinguAntiCheat::GetBuildId())
			{
				bRefuse = true;
				Why = FString::Printf(TEXT("its build is \"%s\", ours \"%s\""), *Shake->JoinerBuildId, *PinguAntiCheat::GetBuildId());
			}
			else if (Shake->Answer != Want)
			{
				bRefuse = true;
				Why = TEXT("its game files differ from ours");
			}
			else
			{
				FString Back;
				const FString JoinerNonce = IsHexText(Shake->JoinerNonce, 64) ? Shake->JoinerNonce : FString(TEXT("0"));
				PinguAntiCheat::AnswerNonce(JoinerNonce, Back);
				Controller->Rq20(Back);
				Shake->PassedAt = Now;
				++HandshakesOk;
				UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] join check passed for %s"), *GetNameSafe(Controller->PlayerState));
			}
		}
		else if (Now - Shake->SentAt > AnswerTimeout)
		{
			Shake->bDone = true;
			bRefuse = true;
			Why = TEXT("no answer to the join check");
		}
		if (bRefuse)
		{
			++HandshakesFailed;
			UE_LOG(LogPingu, Warning, TEXT("[ANTICHEAT] %s can't play here: %s"), *GetNameSafe(Controller->PlayerState), *Why);
			Controller->Rq21();
			Shake->KickAt = Now + 2.0; // (a joiner that does not leave by itself)
		}
	}
}

void APinguAntiCheat::ClientTick(float DeltaSeconds)
{
	using namespace PinguAntiCheatPrivate;
	UWorld* World = GetWorld();
	if (!World || bLeaving)
	{
		return;
	}
	// A packaged Shipping game that has been tampered with since startup (patched code, a foreign DLL loaded into the
	// game folder, a debugger): leave quietly, like any other failed check. Always a no-op in the editor, PIE and -game
	// tests, so solo play and the tests are unaffected.
	if (PinguTamper::Recheck())
	{
		bLeaving = true;
		PinguAntiCheat::LeaveQuietly(this, TEXT("this game has been tampered with"));
		return;
	}
	const UGameInstance* GameInstance = World->GetGameInstance();
	APenguinPlayerController* Own = GameInstance ? Cast<APenguinPlayerController>(GameInstance->GetFirstLocalPlayerController(World)) : nullptr;
	const APlayerState* OwnState = Own ? Own->GetPlayerState<APlayerState>() : nullptr;
	const double Now = FPlatformTime::Seconds();

	// The people in the game and the match (before the reveal is judged: it needs both).
	ClientWatchGame(Now);

	// A new commit: remember it, add our value. A commit must follow the reveal of the one before (no peeking and rolling again).
	if (Commit.Serial != 0)
	{
		if (const FSeenCommit* Seen = SeenCommits.Find(Commit.Serial))
		{
			if (Seen->Hash != Commit.Hash)
			{
				Judge.Strike(FPinguCheatJudge::LeaveAt, -1, TEXT("commit changed"), FString::Printf(TEXT("the host changed its commit %d"), Commit.Serial), Now);
				SeenCommits.Add(Commit.Serial, { Commit.Hash, Now });
			}
		}
		else if (Own && OwnState)
		{
			if (LastCommitSerial > 0 && Commit.Serial != LastCommitSerial)
			{
				CommitStepFrom = LastCommitSerial;
				CommitStepTo = Commit.Serial;
				CommitStepSince = Now;
			}
			SeenCommits.Add(Commit.Serial, { Commit.Hash, Now });
			const FString Value = PinguFair::MakeSecret();
			SentValues.Add(Commit.Serial, TPair<FString, double>(Value, Now));
			Own->Rq23(Commit.Serial, Value);
			++Report().Contributions;
			LastCommitSerial = Commit.Serial;
		}
		// What the host shows it received for this commit (the reveal must carry exactly these).
		TMap<int32, FString>& Shown = SeenReceived.FindOrAdd(Commit.Serial);
		for (const FPinguFairContribution& Entry : Received)
		{
			if (!Shown.Contains(Entry.Player))
			{
				Shown.Add(Entry.Player, Entry.Value.ToUpper());
			}
		}
	}
	if (Reveal.Serial != 0 && Reveal.Serial != LastRevealSerial)
	{
		LastRevealSerial = Reveal.Serial;
		ClientCheckReveal();
	}
	if (CommitStepSince >= 0.0)
	{
		// (the reveal of the old commit comes in the same update as the new commit; a moment's grace all the same)
		if (PinguFair::IsCommitStepFine(CommitStepFrom, CommitStepTo, LastRevealSerial))
		{
			CommitStepSince = -1.0;
		}
		else if (Now - CommitStepSince > 2.0)
		{
			CommitStepSince = -1.0;
			Judge.Strike(FPinguCheatJudge::LeaveAt, -1, TEXT("commit dropped"), FString::Printf(TEXT("the host went from commit %d to %d without revealing %d"),
				CommitStepFrom, CommitStepTo, CommitStepFrom), Now);
		}
	}

	// The fight as this joiner sees it.
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (GameState)
	{
		const bool bFight = GameState->GetPhase() == EPinguPhase::Fight;
		if (bFight && FightStartedAt < 0.f)
		{
			FightStartedAt = World->GetRealTimeSeconds();
		}
		else if (!bFight)
		{
			FightStartedAt = -1.f;
		}
		const float FightAge = bFight ? World->GetRealTimeSeconds() - FightStartedAt : 0.f;
#if !UE_BUILD_SHIPPING
		// (the net test's grants, and its holds: its script throws things there too)
		Judge.SetDevScript(GameState->GetServerWorldTimeSeconds() < FMath::Max(DevScriptUntil, DevQuietUntil));
#endif
		Judge.SetRound(GameState->GetRoundNumber(), bFight, FightAge, Now);
		ClientWatchPenguins(Now, GameState->GetServerWorldTimeSeconds());
		ClientWatchShots(Now, GameState->GetServerWorldTimeSeconds());
	}
	Judge.Tick(Now);
	PinguAntiCheat::FClientReport& Out = Report();
	Out.Points = Judge.GetPoints();
	Out.DealsVerified = Judge.GetDealsVerified();
	Out.UsesJudged = Judge.GetUsesJudged();
	Out.SamplesJudged = Judge.GetSamplesJudged();
	Out.Strikes = Judge.GetStrikes();
	if (Judge.ShouldLeave())
	{
		bLeaving = true;
		PinguAntiCheat::LeaveQuietly(this, FString::Printf(TEXT("%d points of clear rule breaks by the host's game"), Judge.GetPoints()));
		return;
	}
	ClientDevCheat(DeltaSeconds);
}

void APinguAntiCheat::ClientWatchGame(double Now)
{
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!GameState)
	{
		return;
	}
	// The joiners: every person's PlayerId but the host's own (the one whose penguin is HostSlot) - nobody else may add a value.
	int32 HostPlayer = INDEX_NONE;
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		if (HostSlot >= 0 && It->GetPlayerSlot() == HostSlot && It->GetPlayerState())
		{
			HostPlayer = It->GetPlayerState()->GetPlayerId();
		}
	}
	PresentJoiners.Reset();
	for (const APlayerState* State : GameState->PlayerArray)
	{
		if (State && !State->IsABot() && State->GetPlayerId() != HostPlayer)
		{
			KnownJoiners.Add(State->GetPlayerId());
			PresentJoiners.Add(State->GetPlayerId());
		}
	}
	if (HostPlayer != INDEX_NONE)
	{
		KnownJoiners.Remove(HostPlayer);
		PresentJoiners.Remove(HostPlayer);
	}
	// The match: the lobby or the results start a new one (round numbers begin again); a new round we saw begin while we had
	// a commit must be revealed.
	const uint8 Phase = static_cast<uint8>(GameState->GetPhase());
	if (Phase != LastPhaseSeen)
	{
		LastPhaseSeen = Phase;
		if (GameState->GetPhase() == EPinguPhase::Lobby || GameState->GetPhase() == EPinguPhase::MatchOver)
		{
			RoundsDealt.Reset();
			PrevDealtRound = -1;
			MatchChoose = -1;
			MatchPicks.Reset();
			Judge.NewMatch();
		}
	}
	// Abilities RANDOM / CHOOSE as the lobby shows it (a reveal may only deal as it was a moment ago: CheckReveal).
	(GameState->GetSettings().bChooseAbilities ? ChooseOnSeenAt : ChooseOffSeenAt) = Now;
	const int32 RoundNow = GameState->GetRoundNumber();
	if (RoundNow != LastRoundSeen)
	{
		if (LastRoundSeen >= 0 && RoundNow > 0 && SeenCommits.Num() > 0)
		{
			Judge.ExpectRevealFor(RoundNow);
		}
		LastRoundSeen = RoundNow;
	}
	(void)Now;
}

void APinguAntiCheat::ClientCheckReveal()
{
	using namespace PinguAntiCheatPrivate;
	const double Now = FPlatformTime::Seconds();
	const FPinguFairReveal& R = Reveal;
	const FSeenCommit* Seen = SeenCommits.Find(R.Serial);
	if (!Seen)
	{
		UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] round %d: dealt from commit %d, which came before we did: not checked"), R.Round, R.Serial);
		Judge.NoteRevealed(R.Round);
		RoundsDealt.Add(R.Round);
		PrevDealtRound = -1;
		return;
	}
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const APlayerController* Own = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	const APlayerState* OwnState = Own ? Own->GetPlayerState<APlayerState>() : nullptr;
	FPinguRevealCheck Check;
	Check.SeenHash = Seen->Hash;
	Check.OwnPlayer = OwnState ? OwnState->GetPlayerId() : INDEX_NONE;
	if (const TPair<FString, double>* Sent = SentValues.Find(R.Serial))
	{
		Check.OwnValue = Sent->Key;
		Check.OwnSentAgo = Now - Sent->Value;
	}
	Check.KnownJoiners = KnownJoiners;
	Check.LeftJoiners = KnownJoiners.Difference(PresentJoiners);
	if (const TMap<int32, FString>* Shown = SeenReceived.Find(R.Serial))
	{
		Check.SeenReceived = *Shown;
	}
	Check.RoundsDealt = RoundsDealt;
	Check.PrevRound = PrevDealtRound;
	Check.bPrevTraits = bPrevDealtTraits;
	Check.PrevBagAfter = PrevBagAfter;
	// Ability picks (2026-11-07): ours as we asked for them, the bots', the setting of the last moments, the match so far.
	if (const APenguinPlayerState* OwnPenguinState = Cast<APenguinPlayerState>(OwnState))
	{
		Check.OwnSlot = OwnPenguinState->GetPenguinSlot();
		Check.OwnPicksSent = OwnPenguinState->GetSentPicks();
	}
	Check.ChooseSeen = static_cast<uint8>((Now - ChooseOffSeenAt < 10.0 ? 1 : 0) | (Now - ChooseOnSeenAt < 10.0 ? 2 : 0));
	for (TActorIterator<APenguinCharacter> It(GetWorld()); It; ++It)
	{
		const APlayerState* PenguinState = It->GetPlayerState();
		if (!PenguinState || PenguinState->IsABot())
		{
			Check.BotSlots.Add(It->GetPlayerSlot());
		}
	}
	Check.MatchChoose = MatchChoose;
	Check.MatchPicks = MatchPicks;
	for (const TPair<int32, FString>& Problem : PinguFair::CheckReveal(R, Check))
	{
		Judge.Strike(Problem.Key, -1, TEXT("reveal"), Problem.Value, Now);
	}
#if UE_BUILD_SHIPPING
	if (R.ForcedTraits.Num() > 0)
	{
		Judge.Strike(FPinguCheatJudge::LeaveAt, -1, TEXT("forced traits"), FString::Printf(TEXT("round %d: the host forced %d trait(s)"), R.Round, R.ForcedTraits.Num()), Now);
	}
#endif
	FPinguFairDeal Deal;
	PinguFair::Deal(R, Deal);
	Judge.ExpectDeal(R.Round, Deal);
	if (MatchChoose < 0)
	{
		MatchChoose = R.bChoose ? 1 : 0;
	}
	for (const uint8 Slot : R.Slots)
	{
		MatchPicks.Add(Slot, PinguFair::PicksOf(R, Slot));
	}
	RoundsDealt.Add(R.Round);
	PrevDealtRound = R.Round;
	bPrevDealtTraits = R.bTraits;
	PrevBagAfter.Reset();
	for (const EPinguTrait Trait : Deal.BagAfter)
	{
		PrevBagAfter.Add(static_cast<uint8>(Trait));
	}
	SeenReceived.Remove(R.Serial);
	FString Text;
	for (int32 Index = 0; Index < Deal.Slots.Num(); ++Index)
	{
		Text += FString::Printf(TEXT("%s%d: %s%s"), Text.IsEmpty() ? TEXT("") : TEXT("; "), Deal.Slots[Index], *LoadoutText(Deal.Loadouts[Index]),
			R.bTraits ? *FString::Printf(TEXT(" (%s)"), *PinguTraits::GetLogName(Deal.Traits[Index])) : TEXT(""));
	}
	UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] round %d deal recomputed from commit %d (%d value(s)): %s"), R.Round, R.Serial, R.Contributions.Num(), *Text);
}

namespace PinguAntiCheatPrivate
{
	/** The ability that makes a projectile of this kind. */
	EPenguinAbility AbilityOfShot(EPenguinProjectileKind Kind)
	{
		switch (Kind)
		{
		case EPenguinProjectileKind::Snowball: return EPenguinAbility::Snowball;
		case EPenguinProjectileKind::Icicle: return EPenguinAbility::Icicle;
		case EPenguinProjectileKind::Bomb: return EPenguinAbility::SnowBomb;
		case EPenguinProjectileKind::FishHook: return EPenguinAbility::FishHook;
		case EPenguinProjectileKind::IceBolt: return EPenguinAbility::IceGun;
		case EPenguinProjectileKind::ShrinkRay: return EPenguinAbility::ShrinkRay;
		case EPenguinProjectileKind::GrowRay: return EPenguinAbility::GrowRay;
		default: return EPenguinAbility::None;
		}
	}
}

void APinguAntiCheat::ClientWatchShots(double ClientNow, double ServerNow)
{
	using namespace PinguAntiCheatPrivate;
	UWorld* World = GetWorld();
	for (auto It = SeenShots.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
	TArray<APinguGiantSnowball*> Giants;
	for (TActorIterator<APinguGiantSnowball> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Giants.Add(*It);
		}
	}
	TArray<APinguSnowman*> Snowmen;
	APinguSnowman::GetAll(World, Snowmen);
	// Where giant snowballs and snowmen were a moment ago: a giant spits its catch out as it pops (gone then), a snowman's last
	// snowball may fly on after it broke.
	for (const APinguGiantSnowball* Giant : Giants)
	{
		RecentSources.Add({ Giant->GetActorLocation(), Giant->GetDiameter() * 0.5f + 400.f, -1, ClientNow });
	}
	for (const APinguSnowman* Snowman : Snowmen)
	{
		const APenguinCharacter* Builder = IsValid(Snowman) ? Snowman->GetBuilder() : nullptr;
		RecentSources.Add({ Snowman->GetActorLocation(), 600.f * (Builder ? FMath::Max(1.f, Builder->GetSizeScale()) : 1.f),
			Builder ? Builder->GetPlayerSlot() : -2, ClientNow });
	}
	// Icicle pegs and pads (stuck icicles): wind (a tornado, a noot, a push) blows one out as a new icicle of the blower's, a fish
	// hook yanks one out as the thrower's (APenguinProjectile::BlowOut / YankPeg) - nobody's use of Icicle. (Slot -3: icicles only,
	// any shooter. 2026-11-07, shared-traits: Low Gravity rounds and ten tornadoes made three of those a round - a false
	// "shot not held Icicle".)
	for (TActorIterator<APenguinProjectile> It(World); It; ++It)
	{
		const APenguinProjectile* Peg = *It;
		if (!IsValid(Peg) || Peg->GetKind() != EPenguinProjectileKind::Icicle || Peg->IsFlying())
		{
			continue;
		}
		const FVector PegAt = Peg->GetActorLocation();
		FRecentSource* Known = RecentSources.FindByPredicate([&PegAt](const FRecentSource& Source) { return Source.Slot == -3 && FVector::DistSquared(Source.Where, PegAt) < 1.f; });
		if (Known)
		{
			Known->At = ClientNow;
		}
		else
		{
			RecentSources.Add({ PegAt, 250.f, -3, ClientNow });
		}
	}
	RecentSources.RemoveAll([ClientNow](const FRecentSource& Source) { return ClientNow - Source.At > 1.5; });
	const bool bPrimed = bShotsPrimed;
	bShotsPrimed = true;
	const auto SlotOf = [](const APenguinCharacter* Penguin) { return IsValid(Penguin) ? Penguin->GetPlayerSlot() : -1; };
	// Projectiles: a snowman's snowballs and what a giant snowball spits out again are no use of anybody's.
	for (TActorIterator<APenguinProjectile> It(World); It; ++It)
	{
		APenguinProjectile* Shot = *It;
		if (!IsValid(Shot) || SeenShots.Contains(Shot))
		{
			continue;
		}
		SeenShots.Add(Shot);
		const APenguinCharacter* Shooter = Shot->GetShooter();
		if (!bPrimed || !Shooter)
		{
			continue;
		}
		const FVector At = Shot->GetActorLocation();
		bool bOther = false;
		for (const FRecentSource& Source : RecentSources)
		{
			// A giant's spit (any kind, anybody's) or the builder's snowman's snowball.
			const bool bMatches = Source.Slot == -1 || (Shot->GetKind() == EPenguinProjectileKind::Snowball && Source.Slot == Shooter->GetPlayerSlot())
				|| (Source.Slot == -3 && Shot->GetKind() == EPenguinProjectileKind::Icicle);
			bOther |= bMatches && FVector::Dist(Source.Where, At) < Source.Radius;
		}
		if (!bOther)
		{
			Judge.NoteShot(ClientNow, ServerNow, SlotOf(Shooter), AbilityOfShot(Shot->GetKind()));
		}
	}
	for (APinguSnowman* Snowman : Snowmen)
	{
		if (IsValid(Snowman) && !SeenShots.Contains(Snowman))
		{
			SeenShots.Add(Snowman);
			if (bPrimed && Snowman->GetBuilder())
			{
				Judge.NoteShot(ClientNow, ServerNow, SlotOf(Snowman->GetBuilder()), EPenguinAbility::Snowman);
			}
		}
	}
	for (APinguGiantSnowball* Giant : Giants)
	{
		if (!SeenShots.Contains(Giant))
		{
			SeenShots.Add(Giant);
			if (bPrimed && Giant->GetRoller())
			{
				Judge.NoteShot(ClientNow, ServerNow, SlotOf(Giant->GetRoller()), EPenguinAbility::GiantSnowball);
			}
		}
	}
}

void APinguAntiCheat::ClientWatchPenguins(double ClientNow, double ServerNow)
{
	UWorld* World = GetWorld();
	// The bubbles: one that goes away may have given its ability to whoever was near it.
	for (TActorIterator<APinguPickup> It(World); It; ++It)
	{
		if (IsValid(*It) && It->GetAbility() != EPenguinAbility::None)
		{
			FSeenPickup& Entry = SeenPickups.FindOrAdd(*It);
			Entry.Ability = It->GetAbility();
			Entry.Where = It->GetActorLocation();
		}
	}
	for (auto It = SeenPickups.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || It.Key()->IsActorBeingDestroyed())
		{
			Judge.NotePickupGone(It.Value().Ability, ClientNow, It.Value().Where);
			It.RemoveCurrent();
		}
	}
	TArray<APinguPortal*> Portals;
	APinguPortal::GetAll(World, Portals);
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	const double Flow = GameState ? GameState->GetFlowTime() : 0.0;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		const UPenguinAbilityComponent* Abilities = IsValid(Penguin) ? Penguin->GetAbilities() : nullptr;
		const UPenguinMovementComponent* Move = Abilities ? Penguin->GetPenguinMovement() : nullptr;
		if (!Move)
		{
			continue;
		}
		const UPenguinTraitComponent* Traits = Penguin->GetTraits();
		FPinguSeenPenguin Seen;
		Seen.Slot = Penguin->GetPlayerSlot();
		Seen.Name = Penguin->GetDisplayName();
		Seen.Trait = Traits ? Traits->GetTrait() : EPinguTrait::None;
		Seen.Position = Penguin->GetActorLocation();
		Seen.Size = Penguin->GetSizeScale();
		Seen.JesterSwaps = Traits ? Traits->GetEvents().JesterSwaps : 0;
		Seen.LeechCuts = Traits ? Traits->GetEvents().LeechCuts : 0;
		Seen.bKnockedOut = Penguin->IsKnockedOut();
		Seen.HitEvents = Move->HitEvents;
		for (int32 Slot = 0; Slot < Abilities->GetAbilities().Num(); ++Slot)
		{
			FPinguSeenSlot Entry;
			Entry.Ability = Abilities->GetAbility(Slot);
			Entry.Uses = Abilities->GetUseCount(Slot);
			Entry.Swaps = Abilities->GetSwapCount(Slot);
			Entry.End = Abilities->GetCooldownEnd(Slot);
			Entry.Length = Abilities->GetRecordedCooldownLength(Slot);
			Seen.Slots.Add(Entry);
		}
		// Movement: only what the host simulates itself (its own penguin, the bots); a joiner's comes from that joiner.
		// (Bots have a PlayerState too: IsABot.) The self judge (tests) watches every penguin's.
		const bool bBot = Penguin->GetPlayerState() == nullptr || Penguin->GetPlayerState()->IsABot();
		Seen.bJudgeMotion = bSelfJudge || (Penguin->GetLocalRole() == ROLE_SimulatedProxy && (bBot || (HostSlot >= 0 && Seen.Slot == HostSlot)));
		if (Seen.bJudgeMotion)
		{
			// A new sample: the host's transform time stamp moved, or (where it never does) the replicated position did - then
			// timed by its arrival on the host's clock (lag jitter is far inside the judge's slack).
			const float Stamp = Penguin->GetReplicatedServerLastTransformUpdateTimeStamp();
			const FVector At = bSelfJudge ? Penguin->GetActorLocation() : Penguin->GetReplicatedMovement().Location;
			FSeenMotion* Last = SeenStamps.Find(Penguin);
			const bool bFirst = Last == nullptr;
			if (bFirst)
			{
				Last = &SeenStamps.Add(Penguin);
			}
			const bool bStampMoved = !bFirst && Last->Stamp != Stamp;
			Last->bStampMoves |= bStampMoved && !bSelfJudge;
			Seen.bNewSample = bFirst || (Last->bStampMoves ? bStampMoved : !At.Equals(Last->Location, 0.1));
			Seen.MotionTime = Last->bStampMoves ? static_cast<double>(Stamp) : ServerNow;
			if (!Last->bLogged && Seen.bNewSample && !bFirst)
			{
				Last->bLogged = true;
				UE_LOG(LogPingu, Log, TEXT("[ANTICHEAT] judging %s's motion (samples timed by %s)"), *Seen.Name,
					Last->bStampMoves ? TEXT("the host's time stamps") : TEXT("their arrival"));
			}
			Last->Stamp = Stamp;
			Last->Location = At;
			Seen.Location = At;
			Seen.bWalking = Move->MovementMode == MOVE_Walking;
			const float SecondChanceAt = Traits ? Traits->GetEvents().SecondChanceAt : -1.f;
#if !UE_BUILD_SHIPPING
			Seen.bMotionExempt = ServerNow < DevQuietUntil; // (the net test's own script moving penguins about)
#endif
			Seen.bMotionExempt |= Penguin->IsKnockedOut() || Penguin->IsHidden() || Portals.Num() > 0 || Move->IsHovering() || Move->IsFrozen()
				|| Move->IsStunned() || Move->IsTornadoPulled() || Move->IsFloating() || Move->IsPassingThroughIsland()
				|| APinguGiantSnowball::FindHolding(World, Penguin) != nullptr || (SecondChanceAt >= 0.f && Flow - SecondChanceAt < 5.0);
		}
		Judge.Observe(ClientNow, ServerNow, Seen);
	}
}

// ---------------------------------------------------------------------------- the self judge (tests, dev builds)

#if !UE_BUILD_SHIPPING
APinguAntiCheat* APinguAntiCheat::GetSelfJudge(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<APinguAntiCheat> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It) && It->bSelfJudge)
		{
			return *It;
		}
	}
	return nullptr;
}

APinguAntiCheat* APinguAntiCheat::StartSelfJudge(UWorld* World)
{
	if (!World || World->GetNetMode() != NM_Standalone)
	{
		return nullptr;
	}
	if (APinguAntiCheat* Existing = GetSelfJudge(World))
	{
		return Existing;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguAntiCheat* Self = World->SpawnActorDeferred<APinguAntiCheat>(APinguAntiCheat::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Self)
	{
		Self->bSelfJudge = true;
		Self->SetReplicates(false);
		Self->PrimaryActorTick.TickGroup = TG_PostUpdateWork; // after the game mode and the abilities' own guard
		Self->FinishSpawning(FTransform::Identity);
		UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] self judge on: this world's penguins are judged as a joiner would judge them"));
	}
	return Self;
}
#endif

void APinguAntiCheat::SelfJudgeTick()
{
	// The server's own state through the joiner's eyes: the deal of each round is what the penguins hold when its fight
	// starts (solo rolls are not commit-reveal), every clock is the world's.
	UWorld* World = GetWorld();
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!World || !GameState)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	const EPinguPhase Phase = GameState->GetPhase();
	const bool bFight = Phase == EPinguPhase::Fight;
	const int32 RoundNow = GameState->GetRoundNumber();
	if (Phase == EPinguPhase::MatchOver || Phase == EPinguPhase::Lobby)
	{
		Judge.NewMatch();
		SelfRound = -1;
	}
	// The round's "deal": the loadouts in the frame the round is set up (the intro begins; we tick after the game mode and
	// before anything outside the game - the dev trainer - writes again).
	const bool bRoundOn = Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || bFight;
	if (bRoundOn && RoundNow != SelfRound)
	{
		SelfRound = RoundNow;
		FPinguFairDeal Deal;
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			const UPenguinAbilityComponent* Abilities = IsValid(*It) ? It->GetAbilities() : nullptr;
			if (!Abilities || It->GetPlayerSlot() < 0)
			{
				continue;
			}
			Deal.Slots.Add(static_cast<uint8>(It->GetPlayerSlot()));
			const UPenguinTraitComponent* Traits = It->GetTraits();
			Deal.Traits.Add(Traits ? Traits->GetTrait() : EPinguTrait::None);
			Deal.Loadouts.Add(Abilities->GetAbilities());
		}
		Judge.ExpectDeal(RoundNow, Deal);
	}
	if (bFight && FightStartedAt < 0.f)
	{
		FightStartedAt = static_cast<float>(Now);
	}
	else if (!bFight)
	{
		FightStartedAt = -1.f;
	}
	Judge.SetRound(RoundNow, bFight, bFight ? static_cast<float>(Now) - FightStartedAt : 0.f, Now);
	ClientWatchPenguins(Now, Now);
	ClientWatchShots(Now, Now);
	Judge.Tick(Now);
	int32 Restores = 0;
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		Restores += IsValid(*It) && It->GetAbilities() ? It->GetAbilities()->GetShadowRestores() : 0;
	}
	SelfRestores = FMath::Max(SelfRestores, Restores);
}

void APinguAntiCheat::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bSelfJudge)
	{
		// The matchtest / bigtest / endlesstest line: an honest bots' match must give the joiner's judge nothing. With the dev
		// trainer (-PinguCheat=trainer): the guards undo every write (restores, and still nothing for the judge); with
		// -PinguCheat=trainer,noguard the trainer works and the judge must reach its leave line.
		const bool bTrainer = PinguAntiCheat::IsDevTrainer();
		const bool bRaw = bTrainer && PinguAntiCheat::IsDevNoGuard();
		const bool bOk = bRaw ? Judge.ShouldLeave()
			: Judge.GetPoints() == 0 && Judge.GetUsesJudged() > 0 && (!bTrainer || (SelfRestores > 0 && PinguMemoryGuard::GetMovementRestores() > 0));
		UE_LOG(LogPingu, Display, TEXT("[MATCHTEST] RESULT anticheat self judge%s: %d uses judged, %d deals verified, %d motion samples, %d points (want %s), %d loadout / cooldown and %d movement writes undone%s %s"),
			bRaw ? TEXT(" (trainer, no guard)") : (bTrainer ? TEXT(" (trainer, guarded)") : TEXT("")),
			Judge.GetUsesJudged(), Judge.GetDealsVerified(), Judge.GetSamplesJudged(), Judge.GetPoints(),
			bRaw ? TEXT("4 or more: it leaves") : TEXT("0"), SelfRestores, PinguMemoryGuard::GetMovementRestores(),
			Judge.GetStrikes().Num() > 0 ? *FString::Printf(TEXT(" - first: %s"), *Judge.GetStrikes()[0]) : TEXT(""), bOk ? TEXT("ok") : TEXT("FAIL"));
		for (int32 Index = 0; Index < FMath::Min(Judge.GetStrikes().Num(), 12); ++Index)
		{
			UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] self judge strike %d: %s"), Index + 1, *Judge.GetStrikes()[Index]);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void APinguAntiCheat::ClientDevCheat(float DeltaSeconds)
{
#if !UE_BUILD_SHIPPING
	if (PinguAntiCheatPrivate::HasCheat(TEXT("settings")))
	{
		// A joiner that fights the host for its settings: four times a second (any phase).
		DevCheatClock += DeltaSeconds;
		if (DevCheatClock >= 0.25f)
		{
			DevCheatClock = 0.f;
			UWorld* CheatWorld = GetWorld();
			const UGameInstance* CheatInstance = CheatWorld ? CheatWorld->GetGameInstance() : nullptr;
			if (APenguinPlayerController* CheatController = Cast<APenguinPlayerController>(CheatInstance ? CheatInstance->GetFirstLocalPlayerController(CheatWorld) : nullptr))
			{
				CheatController->DevCheatHostOnlyRequests();
			}
		}
		return;
	}
	if (!PinguAntiCheat::IsClientCheat())
	{
		return;
	}
	const APenguinGameState* GameState = APenguinGameState::Get(this);
	if (!GameState || GameState->GetPhase() != EPinguPhase::Fight)
	{
		return;
	}
	UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const APlayerController* Own = GameInstance ? GameInstance->GetFirstLocalPlayerController(World) : nullptr;
	APenguinCharacter* Penguin = Own ? Cast<APenguinCharacter>(Own->GetPawn()) : nullptr;
	if (!Penguin || !Penguin->IsAlive())
	{
		return;
	}
	// A cheating joiner: every 0.1 s six ability requests (far over the rate) and one for a slot it does not have ...
	DevCheatClock += DeltaSeconds;
	if (DevCheatClock >= 0.1f)
	{
		DevCheatClock = 0.f;
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Penguin->DevCheatRequestAbility(Index % 3);
		}
		Penguin->DevCheatRequestAbility(7);
	}
	// ... and every 3 s its penguin jumps 20 m along the island (its own machine moves it: the host must not follow).
	DevTeleportClock += DeltaSeconds;
	if (DevTeleportClock >= 3.f)
	{
		DevTeleportClock = 0.f;
		const FVector From = Penguin->GetActorLocation();
		Penguin->SetActorLocation(From + FVector(From.X > 0.f ? -2000.f : 2000.f, 0.f, 300.f), false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogPingu, Display, TEXT("[ANTICHEAT] dev cheat: teleported our own penguin from x %.0f to x %.0f"), From.X, Penguin->GetActorLocation().X);
	}
#endif
}

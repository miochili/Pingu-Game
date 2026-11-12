// Pingu - the traits' table (see PinguTraitTable.h).

#include "PinguTraitTable.h"
#include "Penguin.h"
#include "PinguTuning.h"

namespace PinguTraitTablePrivate
{
	/** The text and look of a trait (its numbers: MakeNumbers). One row per EPinguTrait, in order. */
	struct FTraitRow
	{
		EPinguTrait Trait;
		const TCHAR* Name;
		const TCHAR* IconName;
		const TCHAR* Upside;
		const TCHAR* Downside;
		FLinearColor Color;
		FVector BodyScale;
	};

	static const FTraitRow Rows[] = {
		{ EPinguTrait::None, TEXT("None"), TEXT(""), TEXT(""), TEXT(""), FLinearColor(1.f, 1.f, 1.f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Heavyweight, TEXT("Heavyweight"), TEXT("Heavyweight"),
			TEXT("Hard to knock away: hits push you only about a third as far."),
			TEXT("No double jump, and you run, dash and jump a little less."),
			FLinearColor(0.55f, 0.45f, 0.35f), FVector(1.3f, 1.3f, 0.95f) },
		{ EPinguTrait::QuickDraw, TEXT("Quick Draw"), TEXT("QuickDraw"),
			TEXT("Your abilities recharge twice as fast."),
			TEXT("They hit only about two thirds as hard."),
			FLinearColor(1.f, 0.85f, 0.2f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::SlowBurn, TEXT("Slow Burn"), TEXT("SlowBurn"),
			TEXT("Your abilities hit almost twice as hard."),
			TEXT("They take twice as long to recharge."),
			FLinearColor(1.f, 0.45f, 0.15f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::IceTouch, TEXT("Ice Touch"), TEXT("IceTouch"),
			TEXT("The snow under your feet turns to ice, shots that touch you freeze and drop, and whoever you touch gets frozen feet."),
			TEXT("You stand on your own ice, so you slip."),
			FLinearColor(0.55f, 0.9f, 1.f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Featherweight, TEXT("Featherweight"), TEXT("Featherweight"),
			TEXT("A triple jump, and you fall slowly."),
			TEXT("Light as a feather: hits push you 1.5 times as far."),
			FLinearColor(0.95f, 0.95f, 1.f), FVector(0.88f, 0.88f, 1.04f) },
		{ EPinguTrait::Tiny, TEXT("Tiny"), TEXT("Tiny"),
			TEXT("Small and quick: you fit through gaps one block high and run faster."),
			TEXT("Small things fly further when hit, your shots are smaller, and one shrink ray takes you to the smallest size."),
			FLinearColor(1.f, 0.55f, 0.8f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Giant, TEXT("Giant"), TEXT("Giant"),
			TEXT("Huge: you push hard, are hard to push, and hard landings break ice."),
			TEXT("A huge target that jumps lower and falls faster."),
			FLinearColor(0.5f, 0.75f, 0.35f), FVector(0.9f, 0.9f, 1.08f) },
		{ EPinguTrait::GlassCannon, TEXT("Glass Cannon"), TEXT("GlassCannon"),
			TEXT("Your hits push 1.5 times as far."),
			TEXT("So do the hits you take."),
			FLinearColor(1.f, 0.4f, 0.6f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Rubber, TEXT("Rubber"), TEXT("Rubber"),
			TEXT("Every fall bounces you back up 70 % as high, and knocked flying you bounce off walls."),
			TEXT("Every landing bounces, and you can't grab ledges."),
			FLinearColor(1.f, 0.3f, 0.55f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::SecondChance, TEXT("Second Chance"), TEXT("SecondChance"),
			TEXT("Once a round, the first time you fall into the sea, a fish throws you high over the island and back onto it (unless you are far out)."),
			TEXT("Only two abilities, and a bubble you pick up swaps one of them."),
			FLinearColor(0.3f, 0.6f, 1.f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Leech, TEXT("Leech"), TEXT("Leech"),
			TEXT("Every hit you land takes 2 seconds off all your cooldowns."),
			TEXT("Your cooldowns are 1.5 times as long."),
			FLinearColor(0.6f, 0.15f, 0.35f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Gambler, TEXT("Gambler"), TEXT("Gambler"),
			TEXT("Six in ten of your uses hit twice as hard."),
			TEXT("The other four turn into sparkling dust: nothing happens (and half the cooldown)."),
			FLinearColor(0.2f, 0.8f, 0.4f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Speedster, TEXT("Speedster"), TEXT("Speedster"),
			TEXT("You run and dash 1.5 times as fast."),
			TEXT("Your feet slip: slow to speed up, to turn and to stop."),
			FLinearColor(1.f, 0.2f, 0.15f), FVector(0.92f, 0.92f, 1.04f) },
		{ EPinguTrait::HotFeet, TEXT("Hot Feet"), TEXT("HotFeet"),
			TEXT("You can't be frozen, and you melt the ice you touch."),
			TEXT("A block you stand on for a second melts away under you."),
			FLinearColor(1.f, 0.35f, 0.05f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Mirror, TEXT("Mirror"), TEXT("Mirror"),
			TEXT("The first shot that hits you every 6 seconds flies back at its thrower."),
			TEXT("Hits push you a quarter further."),
			FLinearColor(0.8f, 0.85f, 0.95f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Sticky, TEXT("Sticky"), TEXT("Sticky"),
			TEXT("Stick to any wall until you press away from it, jump off it, and grab ledges from much further away."),
			TEXT("No belly slide, and less control in the air."),
			FLinearColor(0.45f, 0.85f, 0.2f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Jester, TEXT("Jester"), TEXT("Jester"),
			TEXT("Your abilities hit 1.3 times as hard."),
			TEXT("Every ability you use turns into a random other one."),
			FLinearColor(0.65f, 0.3f, 0.95f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::SameAbilities, TEXT("Same Abilities"), TEXT("SameAbilities"),
			TEXT("Everybody gets the same three abilities this round: nobody has a better hand."),
			TEXT("Your own picks wait for the next round."),
			FLinearColor(0.95f, 0.75f, 0.35f), FVector(1.f, 1.f, 1.f) },
		// ---- 2026-11-07, card shared-traits: made for everybody having the same one
		{ EPinguTrait::Pinball, TEXT("Pinball"), TEXT("Pinball"),
			TEXT("Every hit sends penguins twice as far, from the first second."),
			TEXT("Yours too: one good hit and you are in the sea."),
			FLinearColor(1.f, 0.3f, 0.75f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::FastGrowth, TEXT("Fast Growth"), TEXT("FastGrowth"),
			TEXT("Knockback grows three times as fast as the fight goes on."),
			TEXT("Long fights get wild quickly."),
			FLinearColor(1.f, 0.6f, 0.2f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::DamagePercent, TEXT("Damage %"), TEXT("DamagePercent"),
			TEXT("Every hit a penguin takes makes the next ones send it 12 % further."),
			TEXT("Your own % grows too: it shows under you."),
			FLinearColor(1.f, 0.25f, 0.2f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Recoil, TEXT("Recoil"), TEXT("Recoil"),
			TEXT("Every hit you land pushes you back with half its force: hit to fly."),
			TEXT("Mind where you stand when you hit."),
			FLinearColor(0.85f, 0.55f, 0.3f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::HeavyHitters, TEXT("Heavy Hitters"), TEXT("HeavyHitters"),
			TEXT("Every hit pushes 1.5 times as far."),
			TEXT("Abilities take 1.5 times as long to recharge."),
			FLinearColor(0.6f, 0.35f, 0.25f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::LowGravity, TEXT("Low Gravity"), TEXT("LowGravity"),
			TEXT("Gravity is less than half: huge jumps, slow falls."),
			TEXT("Knocked flying, you float a long way."),
			FLinearColor(0.7f, 0.6f, 1.f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::HighGravity, TEXT("High Gravity"), TEXT("HighGravity"),
			TEXT("Gravity is 1.6 times as strong: hits send you less high."),
			TEXT("Low jumps and fast falls."),
			FLinearColor(0.35f, 0.3f, 0.55f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::IceRink, TEXT("Ice Rink"), TEXT("IceRink"),
			TEXT("The whole island turns to ice: slide into the others."),
			TEXT("Everybody slides: hard to stop at an edge."),
			FLinearColor(0.6f, 0.9f, 1.f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Turbo, TEXT("Turbo"), TEXT("Turbo"),
			TEXT("Everybody runs 1.5 times as fast and abilities recharge twice as fast."),
			TEXT("Everything happens twice as fast."),
			FLinearColor(1.f, 0.85f, 0.1f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::DeadlyEdges, TEXT("Deadly Edges"), TEXT("DeadlyEdges"),
			TEXT("Knock them past the island's left or right side and they are out at once."),
			TEXT("No flying back from out there: stay away from the red sides."),
			FLinearColor(0.95f, 0.15f, 0.15f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Bouncy, TEXT("Bouncy"), TEXT("Bouncy"),
			TEXT("Knocked flying, you bounce off walls and blocks and keep most of your speed."),
			TEXT("A bounce can send you right back out to sea."),
			FLinearColor(1.f, 0.45f, 0.65f), FVector(1.f, 1.f, 1.f) },
		// ---- 2026-11-07, card round-hazards: a hazard for the whole island (PinguRoundHazards.cpp); neutral numbers
		{ EPinguTrait::RisingSea, TEXT("Rising Sea"), TEXT("RisingSea"),
			TEXT("The sea rises through the round: knocked-off penguins go under sooner."),
			TEXT("Less room below the island to fly back."),
			FLinearColor(0.2f, 0.55f, 0.95f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::CrumblingIsland, TEXT("Crumbling Island"), TEXT("CrumblingIsland"),
			TEXT("Every 10 seconds the outermost blocks at both ends shake and break off."),
			TEXT("The island shrinks: keep off the shaking blocks."),
			FLinearColor(0.7f, 0.5f, 0.32f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::Wind, TEXT("Wind"), TEXT("Wind"),
			TEXT("Gusts push everybody left or right: let them carry your enemies off."),
			TEXT("Watch the arrow and walk against the wind."),
			FLinearColor(0.6f, 0.9f, 0.95f), FVector(1.f, 1.f, 1.f) },
		{ EPinguTrait::SnowballRain, TEXT("Snowball Rain"), TEXT("SnowballRain"),
			TEXT("Snowballs fall from the sky and knock whoever they hit."),
			TEXT("Step out of the rings where they land."),
			FLinearColor(0.85f, 0.93f, 1.f), FVector(1.f, 1.f, 1.f) },
	};
	static_assert(UE_ARRAY_COUNT(Rows) == static_cast<int32>(EPinguTrait::Count), "trait table out of sync with EPinguTrait");

	/** Section 1.2 of the traits plan: what each trait changes. */
	static FPinguTraitNumbers MakeNumbers(EPinguTrait Trait)
	{
		FPinguTraitNumbers N;
		switch (Trait)
		{
		case EPinguTrait::Heavyweight:
			N.KnockbackTaken = 0.35f;
			N.AirJumps = 0;
			N.RunScale = 0.8f;
			N.DashScale = 0.85f;
			N.JumpScale = 0.9f;
			break;
		case EPinguTrait::QuickDraw:
			N.CooldownScale = 0.5f;
			N.Strength = 0.7f;
			break;
		case EPinguTrait::SlowBurn:
			N.Strength = 1.75f;
			N.CooldownScale = 2.f;
			break;
		case EPinguTrait::IceTouch:
			break; // all of it is the trait's own gameplay (its ice, the touches): PinguTraitTuning
		case EPinguTrait::Featherweight:
			N.AirJumps = 2;
			N.FallGravityScale = 0.5f;
			N.MaxFallSpeed = 950.f;
			N.KnockbackTaken = 1.5f;
			break;
		case EPinguTrait::Tiny:
			N.BaseSize = PinguShrinkRayFactor; // one shrink step: 0.707 (79 cm tall: fits a one-block gap)
			N.RunScale = 1.25f;                // x1.12 after the 0.90 the size costs
			break;
		case EPinguTrait::Giant:
			N.BaseSize = PinguGrowRayFactor * PinguGrowRayFactor; // two grow steps: 1.682
			N.JumpScale = 0.8f;
			N.FallGravityScale = 1.3f;
			break;
		case EPinguTrait::GlassCannon:
			N.KnockbackDealt = 1.5f;
			N.KnockbackTaken = 1.5f;
			break;
		case EPinguTrait::Rubber:
			N.bCanGrabLedge = false;
			break;
		case EPinguTrait::SecondChance:
			N.NumAbilities = 2;
			break;
		case EPinguTrait::Leech:
			N.CooldownScale = 1.5f;
			break;
		case EPinguTrait::Gambler:
			break; // rolls each use's strength itself (PinguTraitTuning::Gamble*)
		case EPinguTrait::Speedster:
			N.RunScale = 1.5f;
			N.DashScale = 1.5f;
			N.GripAccelScale = 0.35f;
			N.GripBrakeScale = 0.32f; // from a full run it takes about 8x as far to stop as a normal penguin (Linus)
			break;
		case EPinguTrait::HotFeet:
			N.bFreezeImmune = true;
			break;
		case EPinguTrait::Mirror:
			N.KnockbackTaken = 1.25f;
			break;
		case EPinguTrait::Sticky:
			N.bCanBellySlide = false;
			N.AirControlScale = 0.55f;
			N.ClimbUpScale = 1.7f;      // 150 -> 255 cm
			N.ClimbForwardScale = 2.25f; // 40 -> 90 cm
			break;
		case EPinguTrait::Jester:
			N.Strength = 1.3f;
			break;
		// ---- shared-traits (2026-11-07): Pinball, Fast Growth, Damage %, Recoil, Ice Rink and Deadly Edges are gameplay of their
		// own (APenguinGameState::GetKnockbackScale, PenguinTraitEffects.cpp)
		case EPinguTrait::HeavyHitters:
			N.KnockbackDealt = PinguTraitTuning::HeavyHittersKnockback;
			N.CooldownScale = PinguTraitTuning::HeavyHittersCooldown;
			break;
		case EPinguTrait::LowGravity:
			N.GravityScale = PinguTraitTuning::LowGravityScale;
			break;
		case EPinguTrait::HighGravity:
			N.GravityScale = PinguTraitTuning::HighGravityScale;
			break;
		case EPinguTrait::Turbo:
			N.RunScale = PinguTraitTuning::TurboRunScale;
			N.DashScale = PinguTraitTuning::TurboRunScale;
			N.CooldownScale = PinguTraitTuning::TurboCooldown;
			break;
		case EPinguTrait::Bouncy:
			N.FlightWallBounce = PinguTraitTuning::BouncyWallKeep;
			break;
		case EPinguTrait::IceRink:
			N.bAllFloorsIce = true;
			break;
		default:
			break;
		}
		return N;
	}

	static TArray<FPinguTraitInfo> BuildTable()
	{
		TArray<FPinguTraitInfo> Table;
		Table.SetNum(UE_ARRAY_COUNT(Rows));
		for (int32 Index = 0; Index < Table.Num(); ++Index)
		{
			const FTraitRow& Row = Rows[Index];
			ensureMsgf(static_cast<int32>(Row.Trait) == Index, TEXT("trait table row %d is %d"), Index, static_cast<int32>(Row.Trait));
			FPinguTraitInfo& Info = Table[Index];
			Info.Trait = Row.Trait;
			Info.Name = Row.Name;
			Info.IconName = Row.IconName;
			Info.Upside = Row.Upside;
			Info.Downside = Row.Downside;
			Info.Color = Row.Color;
			Info.BodyScale = Row.BodyScale;
			Info.N = MakeNumbers(Row.Trait);
		}
		return Table;
	}

	static const TArray<FPinguTraitInfo>& Table()
	{
		static const TArray<FPinguTraitInfo> Built = BuildTable();
		return Built;
	}

	/** Lower case, no spaces: "Glass Cannon" -> "glasscannon". */
	static FString NameKey(const FString& Name)
	{
		FString Key = Name.ToLower();
		Key.ReplaceInline(TEXT(" "), TEXT(""));
		Key.ReplaceInline(TEXT("_"), TEXT(""));
		Key.ReplaceInline(TEXT("-"), TEXT(""));
		return Key;
	}

	/** How much further a penguin of a base size flies from a normal-size attacker's hit (ApplyPenguinHit's size rule). */
	static float SizeKnockback(float BaseSize)
	{
		using namespace PinguTuning;
		return FMath::Clamp(FMath::Pow(1.f / FMath::Max(BaseSize, 0.1f), SizeKnockbackPower), SizeKnockbackMin, SizeKnockbackMax);
	}

	/** How much further its hits send a normal-size penguin (the same rule the other way round). */
	static float SizeKnockbackDealt(float BaseSize)
	{
		return SizeKnockback(1.f / FMath::Max(BaseSize, 0.1f));
	}

	/** Its running and jump speed from the size alone (UPenguinMovementComponent::SetSizeScale). */
	static float SizeRunScale(float BaseSize)
	{
		return FMath::Pow(FMath::Max(BaseSize, 0.1f), PinguTuning::SizeSpeedPower);
	}

	static float SizeJumpScale(float BaseSize)
	{
		return FMath::Pow(FMath::Max(BaseSize, 0.1f), PinguTuning::SizeJumpPower);
	}

	static const TCHAR* const StatKindNames[] = {
		TEXT("Cooldown"), TEXT("Knockback"), TEXT("Lift"), TEXT("Speed"), TEXT("Range"), TEXT("Radius"), TEXT("KillRadius"), TEXT("Cone"),
		TEXT("Duration"), TEXT("Freeze"), TEXT("Stun"), TEXT("Slip"), TEXT("Blocks"), TEXT("Fuse"), TEXT("SizeStep"),
		TEXT("CooldownScale"), TEXT("Strength"), TEXT("KnockbackTaken"), TEXT("KnockbackDealt"), TEXT("RunSpeed"), TEXT("DashSpeed"),
		TEXT("Jump"), TEXT("AirJumps"), TEXT("FallGravity"), TEXT("FallSpeed"), TEXT("Size"), TEXT("Grip"), TEXT("Braking"),
		TEXT("AirControl"), TEXT("ClimbReach"), TEXT("LedgeGrab"), TEXT("BellySlide"), TEXT("FreezeImmune"), TEXT("Abilities"),
		TEXT("Rescues"), TEXT("WallBounce"), TEXT("FloorBounce"), TEXT("Chance"), TEXT("FizzleChance"), TEXT("CooldownCut"),
		TEXT("Recharge"), TEXT("Cling"), TEXT("MeltTime"), TEXT("SwapDelay"), TEXT("ClimbReachForward"), TEXT("ClingFallSpeed"),
		TEXT("WallJump"), TEXT("ReturnSpeed"), TEXT("RescueThrow"), TEXT("MeltReach"), TEXT("ComboStep"), TEXT("ComboMax"),
		TEXT("Gravity"), TEXT("KnockbackGrowth"), TEXT("DamageStep"), TEXT("Recoil"), TEXT("EdgeKnockOut"),
	};
	static_assert(UE_ARRAY_COUNT(StatKindNames) == static_cast<int32>(EPinguStatKind::Count), "stat kind names out of sync with EPinguStatKind");
}

const FPinguTraitInfo& PinguTraits::GetInfo(EPinguTrait Trait)
{
	const TArray<FPinguTraitInfo>& Table = PinguTraitTablePrivate::Table();
	const int32 Index = static_cast<int32>(Trait);
	return Table[Table.IsValidIndex(Index) ? Index : 0];
}

const FPinguTraitNumbers& PinguTraits::GetNeutralNumbers()
{
	return GetInfo(EPinguTrait::None).N;
}

bool PinguTraits::IsReal(EPinguTrait Trait)
{
	return Trait > EPinguTrait::None && Trait < EPinguTrait::Count;
}

EPinguTrait PinguTraits::FindByName(const FString& Name)
{
	const FString Key = PinguTraitTablePrivate::NameKey(Name.TrimStartAndEnd());
	if (Key.IsEmpty())
	{
		return EPinguTrait::None;
	}
	for (const FPinguTraitInfo& Info : PinguTraitTablePrivate::Table())
	{
		if (IsReal(Info.Trait) && (PinguTraitTablePrivate::NameKey(Info.Name) == Key || PinguTraitTablePrivate::NameKey(Info.IconName) == Key))
		{
			return Info.Trait;
		}
	}
	return EPinguTrait::None;
}

TArray<EPinguTrait> PinguTraits::ParseList(const FString& List)
{
	TArray<FString> Names;
	List.ParseIntoArray(Names, TEXT(","), true);
	TArray<EPinguTrait> Out;
	for (const FString& Name : Names)
	{
		const FString Clean = Name.TrimStartAndEnd();
		const EPinguTrait Trait = FindByName(Clean);
		if (IsReal(Trait))
		{
			Out.Add(Trait);
		}
		else if (Clean.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			Out.Add(EPinguTrait::None); // no trait for that slot
		}
		else
		{
			UE_LOG(LogPingu, Warning, TEXT("[TRAITS] unknown trait \"%s\" (skipped)"), *Clean);
		}
	}
	return Out;
}

FString PinguTraits::GetLogName(EPinguTrait Trait)
{
	return IsReal(Trait) ? FString(GetInfo(Trait).Name) : FString(TEXT("none"));
}

TArray<EPinguTrait> PinguTraits::GetAll()
{
	TArray<EPinguTrait> All;
	for (int32 Index = 1; Index < static_cast<int32>(EPinguTrait::Count); ++Index)
	{
		All.Add(static_cast<EPinguTrait>(Index));
	}
	return All;
}

void PinguTraits::GetStatLines(EPinguTrait Trait, TArray<FPinguStatLine>& Out)
{
	using namespace PinguTraitTuning;
	Out.Reset();
	const FPinguTraitNumbers& N = GetInfo(Trait).N;
	const auto Up = [&Out](EPinguStatKind Kind, float Value) { Out.Add({ Kind, Value, true }); };
	const auto Down = [&Out](EPinguStatKind Kind, float Value) { Out.Add({ Kind, Value, false }); };
	switch (Trait)
	{
	case EPinguTrait::Heavyweight:
		Up(EPinguStatKind::KnockbackTaken, N.KnockbackTaken);
		Down(EPinguStatKind::AirJumps, static_cast<float>(N.AirJumps));
		Down(EPinguStatKind::RunSpeed, N.RunScale);
		Down(EPinguStatKind::DashSpeed, N.DashScale);
		Down(EPinguStatKind::Jump, N.JumpScale);
		break;
	case EPinguTrait::QuickDraw:
		Up(EPinguStatKind::CooldownScale, N.CooldownScale);
		Down(EPinguStatKind::Strength, N.Strength);
		break;
	case EPinguTrait::SlowBurn:
		Up(EPinguStatKind::Strength, N.Strength);
		Down(EPinguStatKind::CooldownScale, N.CooldownScale);
		break;
	case EPinguTrait::IceTouch:
		// The snow under its feet turns to ice (this far around), shots that touch it freeze (this long) and drop, whoever it
		// touches gets frozen feet (this long) - and it stands on its own ice: the ice floor's grip and braking.
		Up(EPinguStatKind::Radius, IceTouchFloorRadius);
		Up(EPinguStatKind::Freeze, IceTouchShotFreeze);
		Up(EPinguStatKind::Slip, IceTouchFeetTime);
		Down(EPinguStatKind::Grip, IceTouchGrip);
		Down(EPinguStatKind::Braking, IceTouchBraking);
		break;
	case EPinguTrait::Featherweight:
		Up(EPinguStatKind::AirJumps, static_cast<float>(N.AirJumps));
		Up(EPinguStatKind::FallGravity, N.FallGravityScale);
		Up(EPinguStatKind::FallSpeed, N.MaxFallSpeed);
		Down(EPinguStatKind::KnockbackTaken, N.KnockbackTaken);
		break;
	case EPinguTrait::Tiny:
		// What it really runs, jumps and knocks with: the trait's numbers and its size together (against a normal penguin).
		Up(EPinguStatKind::Size, N.BaseSize);
		Up(EPinguStatKind::RunSpeed, N.RunScale * PinguTraitTablePrivate::SizeRunScale(N.BaseSize));
		Down(EPinguStatKind::KnockbackTaken, PinguTraitTablePrivate::SizeKnockback(N.BaseSize));
		Down(EPinguStatKind::KnockbackDealt, PinguTraitTablePrivate::SizeKnockbackDealt(N.BaseSize));
		Down(EPinguStatKind::Jump, N.JumpScale * PinguTraitTablePrivate::SizeJumpScale(N.BaseSize));
		break;
	case EPinguTrait::Giant:
		Up(EPinguStatKind::Size, N.BaseSize);
		Up(EPinguStatKind::KnockbackDealt, PinguTraitTablePrivate::SizeKnockbackDealt(N.BaseSize));
		Up(EPinguStatKind::KnockbackTaken, PinguTraitTablePrivate::SizeKnockback(N.BaseSize));
		Up(EPinguStatKind::RunSpeed, N.RunScale * PinguTraitTablePrivate::SizeRunScale(N.BaseSize));
		Up(EPinguStatKind::Blocks, static_cast<float>(GiantBreakBlocks));
		Down(EPinguStatKind::Jump, N.JumpScale * PinguTraitTablePrivate::SizeJumpScale(N.BaseSize));
		Down(EPinguStatKind::FallGravity, N.FallGravityScale);
		break;
	case EPinguTrait::GlassCannon:
		Up(EPinguStatKind::KnockbackDealt, N.KnockbackDealt);
		Down(EPinguStatKind::KnockbackTaken, N.KnockbackTaken);
		break;
	case EPinguTrait::Rubber:
		// Every fall bounces back up 70 % as high (the PINGUPEDIA calls it BOUNCE HEIGHT); knocked flying, off walls too.
		Up(EPinguStatKind::FloorBounce, RubberBounceHeight);
		Up(EPinguStatKind::WallBounce, RubberWallKeep);
		Down(EPinguStatKind::LedgeGrab, N.bCanGrabLedge ? 1.f : 0.f);
		break;
	case EPinguTrait::SecondChance:
		// Once a round the fish throws it high over the island toward its middle (the throw is worked out for each fall:
		// PenguinTraitEffects.cpp, TrySecondChance - no single number to show).
		Up(EPinguStatKind::Rescues, static_cast<float>(SecondChanceRescues));
		Up(EPinguStatKind::RescueThrow, 1.f);
		Down(EPinguStatKind::Abilities, static_cast<float>(N.NumAbilities));
		break;
	case EPinguTrait::Leech:
		Up(EPinguStatKind::CooldownCut, LeechCooldownCut);
		Down(EPinguStatKind::CooldownScale, N.CooldownScale);
		break;
	case EPinguTrait::Gambler:
		Up(EPinguStatKind::Chance, GambleDoubleChance);
		Up(EPinguStatKind::Strength, GambleDoubleStrength);
		Down(EPinguStatKind::FizzleChance, 1.f - GambleDoubleChance);
		break;
	case EPinguTrait::Speedster:
		Up(EPinguStatKind::RunSpeed, N.RunScale);
		Up(EPinguStatKind::DashSpeed, N.DashScale);
		Down(EPinguStatKind::Grip, N.GripAccelScale);
		Down(EPinguStatKind::Braking, N.GripBrakeScale);
		break;
	case EPinguTrait::HotFeet:
		Up(EPinguStatKind::FreezeImmune, N.bFreezeImmune ? 1.f : 0.f);
		Up(EPinguStatKind::MeltReach, HotFeetTouchReach);
		Down(EPinguStatKind::MeltTime, HotFeetMeltTime);
		break;
	case EPinguTrait::Mirror:
		Up(EPinguStatKind::Recharge, MirrorRecharge);
		Up(EPinguStatKind::ReturnSpeed, MirrorSpeed);
		Down(EPinguStatKind::KnockbackTaken, N.KnockbackTaken);
		break;
	case EPinguTrait::Sticky:
		// It sticks to any wall as long as it likes (YES); down held it slides down it (WALL SLIDE).
		Up(EPinguStatKind::Cling, 1.f);
		Up(EPinguStatKind::ClingFallSpeed, StickyClingSlideSpeed);
		Up(EPinguStatKind::WallJump, StickyWallJumpSide);
		Up(EPinguStatKind::ClimbReach, N.ClimbUpScale);
		Up(EPinguStatKind::ClimbReachForward, N.ClimbForwardScale);
		Down(EPinguStatKind::BellySlide, N.bCanBellySlide ? 1.f : 0.f);
		Down(EPinguStatKind::AirControl, N.AirControlScale);
		break;
	case EPinguTrait::Jester:
		Up(EPinguStatKind::Strength, N.Strength);
		Down(EPinguStatKind::SwapDelay, JesterSwapDelay);
		break;
	// ---- shared-traits (2026-11-07): everybody has them, so the upside and the downside are often the same number
	case EPinguTrait::Pinball:
		Up(EPinguStatKind::KnockbackDealt, PinballKnockback);
		Down(EPinguStatKind::KnockbackTaken, PinballKnockback);
		break;
	case EPinguTrait::FastGrowth:
		Up(EPinguStatKind::KnockbackGrowth, FastGrowthPerSecond);
		break;
	case EPinguTrait::DamagePercent:
		Down(EPinguStatKind::DamageStep, DamageStep);
		break;
	case EPinguTrait::Recoil:
		Down(EPinguStatKind::Recoil, RecoilShare);
		break;
	case EPinguTrait::HeavyHitters:
		Up(EPinguStatKind::KnockbackDealt, N.KnockbackDealt);
		Down(EPinguStatKind::CooldownScale, N.CooldownScale);
		break;
	case EPinguTrait::LowGravity:
	case EPinguTrait::HighGravity:
		Up(EPinguStatKind::Gravity, N.GravityScale);
		break;
	case EPinguTrait::IceRink:
		Down(EPinguStatKind::Grip, IceTouchGrip);
		Down(EPinguStatKind::Braking, IceTouchBraking);
		break;
	case EPinguTrait::Turbo:
		Up(EPinguStatKind::RunSpeed, N.RunScale);
		Up(EPinguStatKind::CooldownScale, N.CooldownScale);
		break;
	case EPinguTrait::DeadlyEdges:
		Up(EPinguStatKind::EdgeKnockOut, 1.f);
		break;
	case EPinguTrait::Bouncy:
		Up(EPinguStatKind::WallBounce, N.FlightWallBounce);
		break;
	default:
		break;
	}
}

EPinguStatUnit PinguTraits::GetStatUnit(EPinguStatKind Kind)
{
	switch (Kind)
	{
	case EPinguStatKind::Cooldown:
	case EPinguStatKind::Duration:
	case EPinguStatKind::Freeze:
	case EPinguStatKind::Stun:
	case EPinguStatKind::Slip:
	case EPinguStatKind::Fuse:
	case EPinguStatKind::CooldownCut:
	case EPinguStatKind::Recharge:
	case EPinguStatKind::MeltTime:
	case EPinguStatKind::SwapDelay:
		return EPinguStatUnit::Seconds;
	case EPinguStatKind::MeltReach:
		return EPinguStatUnit::Centimetres;
	case EPinguStatKind::ClingFallSpeed:
	case EPinguStatKind::WallJump:
		return EPinguStatUnit::Speed;
	case EPinguStatKind::RescueThrow:
	case EPinguStatKind::Cling:
		return EPinguStatUnit::Flag;
	case EPinguStatKind::Range:
	case EPinguStatKind::Radius:
	case EPinguStatKind::KillRadius:
		return EPinguStatUnit::Centimetres;
	case EPinguStatKind::Knockback:
	case EPinguStatKind::Lift:
	case EPinguStatKind::Speed:
	case EPinguStatKind::FallSpeed:
		return EPinguStatUnit::Speed;
	case EPinguStatKind::Blocks:
	case EPinguStatKind::AirJumps:
	case EPinguStatKind::Abilities:
	case EPinguStatKind::Rescues:
		return EPinguStatUnit::Count;
	case EPinguStatKind::Chance:
	case EPinguStatKind::FizzleChance:
	case EPinguStatKind::WallBounce:
	case EPinguStatKind::FloorBounce:
	case EPinguStatKind::ComboStep:
	case EPinguStatKind::KnockbackGrowth:
	case EPinguStatKind::DamageStep:
	case EPinguStatKind::Recoil:
		return EPinguStatUnit::Share;
	case EPinguStatKind::Cone:
		return EPinguStatUnit::Degrees;
	case EPinguStatKind::LedgeGrab:
	case EPinguStatKind::BellySlide:
	case EPinguStatKind::FreezeImmune:
	case EPinguStatKind::EdgeKnockOut:
		return EPinguStatUnit::Flag;
	default:
		return EPinguStatUnit::Multiplier;
	}
}

const TCHAR* PinguTraits::GetStatKindName(EPinguStatKind Kind)
{
	const int32 Index = static_cast<int32>(Kind);
	return Index >= 0 && Index < static_cast<int32>(EPinguStatKind::Count) ? PinguTraitTablePrivate::StatKindNames[Index] : TEXT("?");
}

EPinguTrait PinguTraits::DrawShared(TArray<EPinguTrait>& Bag, FRandomStream& Rand, uint64 OffMask)
{
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		if (Bag.Num() == 0)
		{
			// A fresh bag: every ticked trait once, shuffled (nothing ticked: every trait).
			for (const EPinguTrait Trait : GetAll())
			{
				if (!IsOffIn(OffMask, Trait))
				{
					Bag.Add(Trait);
				}
			}
			if (Bag.Num() == 0)
			{
				Bag = GetAll();
			}
			for (int32 Index = Bag.Num() - 1; Index > 0; --Index)
			{
				Bag.Swap(Index, Rand.RandRange(0, Index));
			}
		}
		while (Bag.Num() > 0)
		{
			const EPinguTrait Trait = Bag[0];
			Bag.RemoveAt(0);
			if (IsReal(Trait) && !IsOffIn(OffMask, Trait))
			{
				return Trait;
			}
		}
		// (Everything left was unticked since: a fresh bag.)
	}
	return EPinguTrait::None;
}

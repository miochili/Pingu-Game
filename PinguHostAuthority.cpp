// Pingu - the host's say over a LAN joiner's own penguin: the move judge, the slide and landing gates, the numbers.
// What it is and what it can't stop: PinguHostAuthority.h and docs/FEATURES.md (7.1).

#include "PinguHostAuthority.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace PinguHostAuthorityPrivate
{
	/** The ceiling is run on in steps this long (s). */
	constexpr double CeilingStep = 1.0 / 240.0;
	/** A gap between two moves longer than this is run on as this long (the ceiling only falls further: never lenient). */
	constexpr double MaxAdvance = 3.0;
	/** Rubber: a landing's bounce counts for this long after it. */
	constexpr double BounceMemory = 0.5;
	/** A push's sideways budget: its speed with this much room. */
	constexpr float BudgetRoom = 1.15f;
	constexpr float BudgetExtra = 150.f;
}

// ============================================================================ the move judge

float FPinguMoveJudge::CreditSpeed(const FPinguMoveLimits& Limits)
{
	return FMath::Sqrt(2.f * FMath::Max(Limits.Gravity, 1.f) * FMath::Max(Limits.CreditHeight, 1.f));
}

void FPinguMoveJudge::Reset()
{
	*this = FPinguMoveJudge();
}

void FPinguMoveJudge::Stamp(double Now)
{
	CeilingHistory.Add({ Now, CeilZ });
	// Keep what the lag slack needs (and always the newest).
	while (CeilingHistory.Num() > 1 && CeilingHistory[0].Time < Now - LagSlack - 0.02)
	{
		CeilingHistory.RemoveAt(0, EAllowShrinking::No);
	}
	if (CeilingHistory.Num() > 30)
	{
		CeilingHistory.RemoveAt(0, CeilingHistory.Num() - 30, EAllowShrinking::No);
	}
}

void FPinguMoveJudge::Advance(double Now, const FPinguMoveLimits& Limits)
{
	using namespace PinguHostAuthorityPrivate;
	const double Dt = FMath::Clamp(Now - LastTime, 0.0, MaxAdvance);
	LastTime = Now;
	if (Dt <= 0.0)
	{
		return;
	}
	const int32 Steps = FMath::Max(1, FMath::CeilToInt(Dt / CeilingStep));
	const float H = static_cast<float>(Dt / Steps);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const float G = Limits.Gravity * (FMath::Abs(CeilV) < Limits.ApexSpeed ? Limits.ApexGravityScale : 1.f);
		CeilV = FMath::Max(CeilV - G * H, -Limits.SlowestFall);
		CeilZ += CeilV * H;
	}
	SideBudget = FMath::Max(0.f, SideBudget - BudgetDecay * static_cast<float>(Dt));
	Stamp(Now);
}

void FPinguMoveJudge::StartArc(double Now, float Level, float Speed, const FPinguMoveLimits& Limits)
{
	using namespace PinguHostAuthorityPrivate;
	// The joiner's move is up to LagSlack late: the arc as it is that much later counts as well (its top never changes).
	CeilZ = Level;
	CeilV = Speed;
	float Highest = CeilZ;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt(LagSlack / CeilingStep));
	const float H = static_cast<float>(LagSlack / Steps);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const float G = Limits.Gravity * (FMath::Abs(CeilV) < Limits.ApexSpeed ? Limits.ApexGravityScale : 1.f);
		CeilV = FMath::Max(CeilV - G * H, -Limits.SlowestFall);
		CeilZ += CeilV * H;
		Highest = FMath::Max(Highest, CeilZ);
	}
	CeilingHistory.Reset();
	CeilingHistory.Add({ Now, Highest });
	CeilingHistory.Add({ Now, CeilZ });
	LastTime = Now;
}

float FPinguMoveJudge::GetAllowedZ() const
{
	float Highest = CeilZ;
	for (const FStamp& Entry : CeilingHistory)
	{
		Highest = FMath::Max(Highest, Entry.Value);
	}
	return Highest + RiseSlack;
}

void FPinguMoveJudge::ForgetPath()
{
	SidePath.Reset();
}

void FPinguMoveJudge::FollowHost(double Now, const FVector& Location, const FVector& Velocity, bool bSupported, const FPinguMoveLimits& Limits)
{
	using namespace PinguHostAuthorityPrivate;
	bHave = true;
	LastTime = Now;
	const float Z = static_cast<float>(Location.Z);
	CeilZ = Z;
	CeilV = FMath::Max(static_cast<float>(Velocity.Z), -Limits.SlowestFall);
	CeilingHistory.Reset();
	Stamp(Now);
	if (bSupported)
	{
		Credits = Limits.Credits;
		TopZ = Z;
	}
	else
	{
		TopZ = FMath::Max(TopZ, Z);
	}
	// The host's own simulation: its speed is real (a flight goes on after the host hands the penguin back).
	SideBudget = FMath::Max(SideBudget, FMath::Abs(static_cast<float>(Velocity.X)) * BudgetRoom + BudgetExtra);
	SidePath.Add({ Now, static_cast<float>(Location.X) });
	while (SidePath.Num() > 1 && SidePath[0].Time < Now - SideWindow)
	{
		SidePath.RemoveAt(0, EAllowShrinking::No);
	}
}

void FPinguMoveJudge::NoteHostPush(double Now, const FVector& Location, const FVector& Velocity, bool bReplace, bool bRefresh, const FPinguMoveLimits& Limits)
{
	using namespace PinguHostAuthorityPrivate;
	if (!bHave)
	{
		FollowHost(Now, Location, FVector::ZeroVector, false, Limits);
	}
	Advance(Now, Limits);
	const float Up = static_cast<float>(Velocity.Z);
	const float Side = FMath::Abs(static_cast<float>(Velocity.X));
	// From where the host has it (it may be above the ceiling: the host put it there).
	CeilZ = FMath::Max(CeilZ, static_cast<float>(Location.Z));
	if (bReplace)
	{
		CeilV = FMath::Max(CeilV, Up);
		SideBudget = FMath::Max(SideBudget, Side * BudgetRoom + BudgetExtra);
	}
	else
	{
		if (Up > 0.f)
		{
			CeilV = FMath::Max(CeilV, 0.f) + Up;
		}
		SideBudget = FMath::Max(SideBudget, Limits.SideSpeed + Side * BudgetRoom);
	}
	if (bRefresh)
	{
		Credits = Limits.Credits;
	}
	Stamp(Now);
}

bool FPinguMoveJudge::Judge(const FPinguMoveSample& Move, const FPinguMoveLimits& Limits, FString& OutWhy)
{
	using namespace PinguHostAuthorityPrivate;
	const float X = static_cast<float>(Move.Location.X);
	const float Z = static_cast<float>(Move.Location.Z);
	if (!bHave)
	{
		FollowHost(Move.Now, Move.Location, FVector::ZeroVector, Move.bSupported, Limits);
		return true;
	}
	Advance(Move.Now, Limits);

	// Sideways: its average speed over the window (from the oldest position the host took in it).
	while (SidePath.Num() > 1 && SidePath[0].Time < Move.Now - SideWindow)
	{
		SidePath.RemoveAt(0, EAllowShrinking::No);
	}
	if (SidePath.Num() > 0)
	{
		const double Span = Move.Now - SidePath[0].Time;
		if (Span >= SideMinSpan && Span <= SideWindow + 0.05)
		{
			const float Cap = FMath::Max(Limits.SideSpeed, SideBudget);
			const float Way = FMath::Abs(X - SidePath[0].Value);
			if (Way > Cap * static_cast<float>(Span) + SideSlack)
			{
				OutWhy = FString::Printf(TEXT("%.0f cm/s sideways for %.2f s (at most %.0f)"), Way / Span, Span, Cap);
				return false;
			}
		}
	}

	// Up: never above the ceiling - unless one of its own lifts (or a pad, or Rubber's bounce) starts a new arc.
	if (!Move.bFreeVertical && Z > GetAllowedZ())
	{
		float Level = GetAllowedZ() - RiseSlack;
		float Speed = CreditSpeed(Limits);
		bool bFree = false;
		// A lift right after another (a jump and an air dash at once): one combo from the first one's place, never a stack.
		const bool bCombo = Move.Now - LastArcTime < ComboTime;
		if (bCombo)
		{
			Level = LastArcLevel;
			Speed *= FMath::Sqrt(ComboHeightScale);
		}
		if (Move.bNearPad)
		{
			Speed = FMath::Max(Speed, Limits.PadSpeed * 1.1f);
			bFree = true;
		}
		if (Limits.BounceShare > 0.f)
		{
			// Rubber bounces off a landing: back up to its share of the height fallen (from the landing just seen, or from
			// the highest point since it stood if the bottom of the bounce was never seen).
			const float FromTop = FMath::Sqrt(2.f * Limits.Gravity * Limits.BounceShare * FMath::Max(0.f, TopZ - Level)) * 1.1f + 100.f;
			const float Bounce = FMath::Max(Move.Now <= BounceUntil ? BounceSpeed : 0.f, CeilV < 0.f ? FromTop : 0.f);
			if (Bounce > Speed)
			{
				Speed = Bounce;
				bFree = true;
			}
		}
		if (!bFree && Credits <= 0)
		{
			OutWhy = FString::Printf(TEXT("rose to z %.0f, its jumps allow %.0f (none left)"), Z, GetAllowedZ());
			return false;
		}
		// Kept in case the new arc does not reach either (then nothing was used up).
		const float OldZ = CeilZ;
		const float OldV = CeilV;
		const TArray<FStamp, TInlineAllocator<32>> OldHistory = CeilingHistory;
		StartArc(Move.Now, Level, Speed, Limits);
		if (Z > GetAllowedZ())
		{
			OutWhy = FString::Printf(TEXT("rose to z %.0f, even a new jump allows %.0f"), Z, GetAllowedZ());
			CeilZ = OldZ;
			CeilV = OldV;
			CeilingHistory = OldHistory;
			return false;
		}
		if (!bFree)
		{
			--Credits;
		}
		else
		{
			BounceUntil = -1.0;
			TopZ = Level;
		}
		if (!bCombo || bFree)
		{
			LastArcTime = Move.Now;
			LastArcLevel = Level;
		}
	}

	// Taken: the judge's idea follows it.
	if (Move.bFreeVertical)
	{
		// Lifted or floating: when that ends, the arc goes on from here (as if it had just jumped: lenient).
		CeilZ = FMath::Max(CeilZ, Z);
		CeilV = FMath::Max(CeilV, CreditSpeed(Limits));
		Stamp(Move.Now);
	}
	if (Move.bSupported)
	{
		if (Limits.BounceShare > 0.f && TopZ - Z > 40.f)
		{
			BounceSpeed = FMath::Sqrt(2.f * Limits.Gravity * Limits.BounceShare * (TopZ - Z)) * 1.1f + 100.f;
			BounceUntil = Move.Now + BounceMemory;
		}
		CeilZ = Z;
		CeilV = 0.f;
		Credits = Limits.Credits;
		TopZ = Z;
		LastArcTime = -100.0;
		CeilingHistory.Reset();
		Stamp(Move.Now);
	}
	else
	{
		TopZ = FMath::Max(TopZ, Z);
	}
	SidePath.Add({ Move.Now, X });
	return true;
}

// ============================================================================ the slide and landing gates

bool FPinguSlideGate::Request(double Now, bool bCanSlide, float Cooldown)
{
	if (!bCanSlide || Now - LastStart < static_cast<double>(Cooldown) - Slack)
	{
		++Refused;
		return false;
	}
	LastStart = Now;
	++Accepted;
	return true;
}

void FPinguLandGate::Observe(float Z, bool bSupported)
{
	if (bSupported)
	{
		return;
	}
	TopZ = bSawAir ? FMath::Max(TopZ, Z) : Z;
	bSawAir = true;
}

bool FPinguLandGate::Request(float Z, float ClaimedSpeed, const FPinguMoveLimits& Limits, float FastFallGravityScale, float FastFallStartSpeed, float& OutSpeed)
{
	if (!bSawAir)
	{
		++Refused;
		return false;
	}
	bSawAir = false;
	// The fastest landing that fall allows: a fast fall all the way down (its start speed and its gravity).
	const float Fall = FMath::Max(0.f, TopZ - Z);
	const float Most = FMath::Sqrt(2.f * Limits.Gravity * FMath::Max(FastFallGravityScale, 1.f) * Fall) + FastFallStartSpeed + 150.f;
	OutSpeed = FMath::Clamp(ClaimedSpeed, 0.f, Most);
	return true;
}

// ============================================================================ numbers and the dev switches

PinguHostAuthority::FStats& PinguHostAuthority::Stats()
{
	static FStats Numbers;
	return Numbers;
}

float PinguHostAuthority::CorrectionsPerMinute()
{
	const FStats& Numbers = Stats();
	if (Numbers.FirstMoveTime < 0.0)
	{
		return 0.f;
	}
	const double Minutes = FMath::Max((FPlatformTime::Seconds() - Numbers.FirstMoveTime) / 60.0, 1.0 / 60.0);
	return static_cast<float>((Numbers.Refused + Numbers.PullBacks + Numbers.HandoverMisses) / Minutes);
}

FString PinguHostAuthority::DescribeStats()
{
	const FStats& N = Stats();
	return FString::Printf(TEXT("%d moves refused (+ %d while taken over), %d take-overs, %d pull-backs, %d of %d hand-backs off the host's path, %d quiet joiners moved, ")
		TEXT("%d moves off Y 0, slides %d counted / %d refused, %d landings refused, %d stomp landings ignored; %.1f corrections a minute"),
		N.Refused, N.IgnoredWhileTaken, N.Penalties, N.PullBacks, N.HandoverMisses, N.Handovers, N.Silences, N.YClamped, N.SlidesAccepted, N.SlidesRefused,
		N.LandsRefused, N.StompClaimsIgnored, CorrectionsPerMinute());
}

bool PinguHostAuthority::HasDevCheat(const TCHAR* Name)
{
#if UE_BUILD_SHIPPING
	return false;
#else
	static const TArray<FString> List = []
	{
		FString Value;
		FParse::Value(FCommandLine::Get(), TEXT("PinguCheat="), Value, false); // (the whole list: not stopped at a comma)
		TArray<FString> Parts;
		Value.ToLower().ParseIntoArray(Parts, TEXT(","));
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
		}
		return Parts;
	}();
	return List.Contains(FString(Name).ToLower());
#endif
}

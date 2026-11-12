// Pingu - the bots don't stand around and don't loop through portals (card 2026-11-07_bots-aggro). The autotest's steps
// (APenguinAutoTest::AddBotAggroSteps; filter bot_Idle,bot_Portal + spawn_target; bot_ runs them with the older bot steps):
//
//  bot_IdleEngage          a bot with the tornado (used only where it throws toward the sea: not in the strip's middle) and
//                          the Ice Shell stands 1.2 m from our penguin: before 2026-11-07 it stood next to it for ever doing
//                          nothing (0 hits in 22 s). Now it goes in 15 s after its last fight (UPenguinBotComponent::
//                          AggroIdleTime), finds its tornado no use and belly-slides into our penguin: a hit within 20 s
//                          (114_bot_IdleEngage_going_in, 114_bot_IdleEngage_hit).
//  bot_IdleEngageEasy      the same on easy (a hit within 21 s).
//  bot_IdleEngageEdge      the same with our penguin 3 m from the strip's end (here the tornado is worth it): never into the sea.
//  bot_PortalLoopFall      a portal with one end on the strip and the other straight above it (a Snow Warp straight up): the
//                          bot dropped onto the lower end - at most 2 trips in 10 s, never knocked out (114_bot_portal_loop).
//  bot_PortalLoopNear      a portal whose far end stands next to our penguin: once through it, then no back and forth.
//  bot_PortalLoopShort     a short portal on the bot's way (2.6 m), and bot_PortalLoopShortLeft: the loop bot_PortalFuzz found
//                          in the old brain (ends 1.44 m apart, walking left: 5 trips in 6 s). At most 2 trips, at our penguin
//                          within 7 s (114_bot_PortalLoopShort*).
//  bot_PortalShortcutWall  a rock wall too high to jump between the bot and our penguin, a portal across it: the bot goes
//                          through (the only way), at most twice, and gets to our side within 3 s (114_bot_portal_use).
//  bot_PortalFuzz00..35    36 random layouts (seed 20261107; ends on the strip or up to 4.8 m up, bot and our penguin
//                          anywhere, two random close-range abilities, any skill), 6 s each: the bot's own trips; 3 or more in
//                          one layout is a loop (logged with its layout). The last one reports: want 0 loops.
//
// The dummy plays the bot (the test drives our own penguin's stick every frame, which would fight a brain); our penguin
// stands still as its target and is put back on its spot after a hit.

#include "PenguinAutoTest.h"
#include "BlockIsland.h"
#include "Penguin.h"
#include "PenguinAbilities.h"
#include "PenguinBot.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PinguPortal.h"

void APenguinAutoTest::AddBotAggroSteps()
{
	const FVector Base(FlatIslandX - 300.f, 0.f, 62.f);
	// The dummy at Base + BotOffset, our penguin at Base + TargetOffset; everybody for themselves.
	const auto PrepareBot = [this, Base](float BotOffset, float TargetOffset)
	{
		SpawnTarget(); // (a filtered run has no dummy yet)
		PrepareBrawl(BotOffset, {});
		Teleport(Base + FVector(TargetOffset, 0.f, 0.f));
		Penguin->SetTeam(-1);
		if (Target.IsValid())
		{
			Target->SetTeam(-1);
			SavedTargetAbilities = Target->GetAbilities()->GetAbilities();
		}
		OwnHitsAtStart = Move->HitEvents;
		TimeA = -1.f;
		TimeB = -1.f;
		CountA = 0;
		CountB = 0;
		Value = 1.e9f;
	};
	const auto StartBot = [this](const TArray<EPenguinAbility>& Abilities, int32 Difficulty)
	{
		APenguinCharacter* Bot = Target.Get();
		if (!Bot || !Bot->GetBot())
		{
			return;
		}
		Bot->GetAbilities()->SetAbilities(Abilities);
		Bot->GetBot()->SetBotEnabled(true, Difficulty);
	};
	const auto StopBot = [this, Base]()
	{
		if (Target.IsValid())
		{
			if (Target->GetBot())
			{
				Target->GetBot()->SetBotEnabled(false);
			}
			if (SavedTargetAbilities.Num() > 0)
			{
				Target->GetAbilities()->SetAbilities(SavedTargetAbilities);
			}
		}
		APinguPortal::CloseAll(GetWorld());
		if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
		{
			Flat->ClearTemporaryBlocks();
		}
	};
	// Our penguin back on its spot (PortalEndB.X, standing) a moment after a hit (or after it was knocked out).
	const auto KeepTargetOnSpot = [this](float T)
	{
		const FVector Spot(PortalEndB.X, 0.f, 62.f);
		if (!Penguin->IsAlive())
		{
			Penguin->ResetForRound(Spot);
			Penguin->SetInputLocked(false);
			return;
		}
		if (TimeB > 0.f && T - TimeB > 1.2f && FMath::Abs(static_cast<float>(Location().X - Spot.X)) > 40.f && Move->IsMovingOnGround())
		{
			Teleport(Spot);
		}
	};

	// ---- Idle -> in: a bot that only has its belly slide to attack with, inside the distance it keeps.
	struct FIdleCase
	{
		const TCHAR* Name;
		int32 Difficulty;
		bool bAtEdge;
		float Limit;
	};
	for (const FIdleCase& Case : { FIdleCase{ TEXT("bot_IdleEngage"), 1, false, 20.f }, FIdleCase{ TEXT("bot_IdleEngageEasy"), 0, false, 21.f },
			 FIdleCase{ TEXT("bot_IdleEngageEdge"), 1, true, 0.f } })
	{
		AddStep(Case.Name, 23.f,
			[this, Base, Case, PrepareBot, StartBot]
			{
				float Spot = 600.f;
				if (Case.bAtEdge)
				{
					const ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base);
					Spot = Flat ? static_cast<float>(Flat->GetBounds().Max.X - Base.X) - 300.f : 2600.f;
				}
				PortalEndB = Base + FVector(Spot, 0.f, 0.f); // (our penguin's spot)
				PrepareBot(Spot - 120.f, Spot);
				StartBot({ EPenguinAbility::Gust, EPenguinAbility::IceShell }, Case.Difficulty);
			},
			[this, Case, KeepTargetOnSpot](float T)
			{
				const APenguinCharacter* Bot = Target.Get();
				if (!Bot)
				{
					return;
				}
				if (!Bot->IsAlive())
				{
					CountB = 1;
				}
				// When it starts going in (the brain's own flag), with a picture of it on its way.
				if (MarkA < 0.f && Bot->GetBot() && Bot->GetBot()->IsAggressive())
				{
					MarkA = T;
				}
				if (MarkA > 0.f && !bFlag && T > MarkA + 1.6f)
				{
					bFlag = true;
					Shot(*FString::Printf(TEXT("114_%s_going_in"), Case.Name));
				}
				const int32 Hits = Move->HitEvents - OwnHitsAtStart;
				if (Hits > CountA)
				{
					CountA = Hits;
					TimeA = TimeA < 0.f ? T : TimeA;
					TimeB = T;
					if (!bShotDone)
					{
						bShotDone = true;
						Shot(*FString::Printf(TEXT("114_%s_hit"), Case.Name));
					}
				}
				if (T > 15.f && Bot->IsAlive())
				{
					Value = FMath::Min(Value, static_cast<float>(FVector::Dist(Bot->GetActorLocation(), Location())));
				}
				KeepTargetOnSpot(T);
			},
			[this, Case, StopBot]
			{
				// At the strip's end a slide that misses would go on into the sea: there the bot may hold back - it only must
				// stay in.
				// Going in: 15 s after its last fight (here: the start, or an early hit at the edge), within a think or two.
				const float GoesIn = MarkA;
				const float Expected = (TimeA > 0.f && (MarkA < 0.f || TimeA < MarkA)) ? -1.f : UPenguinBotComponent::AggroIdleTime;
				const bool bInTime = Expected < 0.f ? true : (GoesIn >= Expected - 0.1f && GoesIn <= Expected + 0.5f);
				const bool bOk = bInTime && CountB == 0 && (Case.bAtEdge || (TimeA > 0.f && TimeA <= Case.Limit));
				Report(FString::Printf(TEXT("RESULT %s: went in after %.1f s (want %.0f), first hit on our penguin after %.1f s (want within %.0f%s), hits %d, closest after 15 s %.0f cm, bot knocked out %d (want 0): %s"),
					Case.Name, GoesIn, UPenguinBotComponent::AggroIdleTime, TimeA, Case.Limit, Case.bAtEdge ? TEXT(", not needed at the edge") : TEXT(""),
					CountA, Value, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
				StopBot();
			});
	}

	// ---- A portal straight up: falling into the lower end throws the bot out of the upper one, onto the lower one again.
	AddStep(TEXT("bot_PortalLoopFall"), 10.f,
		[this, Base, PrepareBot, StartBot]
		{
			PortalEndA = Base + FVector(300.f, 0.f, 0.f);
			PortalEndB = Base + FVector(630.f, 0.f, 0.f); // (our penguin's spot)
			PrepareBot(300.f, 630.f);
			TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), PortalEndA, PortalEndA + FVector(0.f, 0.f, 420.f));
			if (Target.IsValid())
			{
				Target->SetActorLocation(PortalEndA + FVector(0.f, 0.f, 200.f), false, nullptr, ETeleportType::TeleportPhysics);
				Target->GetPenguinMovement()->Velocity = FVector::ZeroVector;
				Target->GetPenguinMovement()->SetMovementMode(MOVE_Falling);
			}
			StartBot({ EPenguinAbility::IceShell, EPenguinAbility::IceWall }, 1);
		},
		[this, KeepTargetOnSpot](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			if (Bot && !Bot->IsAlive())
			{
				CountB = 1;
			}
			const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : 0;
			if (!bShotDone && Transits >= 1 && T > 0.3f)
			{
				bShotDone = true;
				Shot(TEXT("114_bot_portal_loop"));
			}
			KeepTargetOnSpot(T);
		},
		[this, StopBot]
		{
			const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : -1;
			const bool bOk = Transits >= 0 && Transits <= 2 && CountB == 0;
			Report(FString::Printf(TEXT("RESULT bot portal loop fall: through the portal %d times in 10 s (want at most 2), knocked out %d (want 0): %s"),
				Transits, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
			StopBot();
		});

	// ---- The far end next to our penguin: once through it (a shortcut), then no back and forth.
	AddStep(TEXT("bot_PortalLoopNear"), 10.f,
		[this, Base, PrepareBot, StartBot]
		{
			PortalEndA = Base + FVector(0.f, 0.f, 0.f);
			PortalEndB = Base + FVector(1550.f, 0.f, 0.f); // (our penguin's spot)
			PrepareBot(-500.f, 1550.f);
			TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), PortalEndA, Base + FVector(1400.f, 0.f, 0.f));
			StartBot({ EPenguinAbility::IceShell, EPenguinAbility::IceWall }, 1);
		},
		[this, KeepTargetOnSpot](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			if (Bot && !Bot->IsAlive())
			{
				CountB = 1;
			}
			KeepTargetOnSpot(T);
		},
		[this, StopBot]
		{
			const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : -1;
			const bool bOk = Transits >= 1 && Transits <= 2 && CountB == 0;
			Report(FString::Printf(TEXT("RESULT bot portal loop near: through the portal %d times in 10 s (want 1 or 2), knocked out %d (want 0): %s"),
				Transits, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
			StopBot();
		});

	// ---- A short portal on the bot's way (a short Snow Warp: both ends on the strip, 1.4 - 2.6 m apart). The second case is
	// the loop bot_PortalFuzz found in the old brain (layout 23 of seed 20261107): walking left, it jumped over the near end
	// (through it was "no real shortcut"), came down in the far one, out of the near one again, and so on - 5 trips in 6 s.
	// Now: through it (it is on the way anyway, a little gain will do) or over both, and on to our penguin.
	struct FShortCase
	{
		const TCHAR* Name;
		float EndA;
		float EndB;
		float BotX;
		float SpotX;
		EPenguinAbility First;
		EPenguinAbility Second;
	};
	for (const FShortCase& Case : { FShortCase{ TEXT("bot_PortalLoopShort"), 300.f, 560.f, 0.f, 1500.f, EPenguinAbility::FlipperSlap, EPenguinAbility::IceShell },
			 FShortCase{ TEXT("bot_PortalLoopShortLeft"), 633.f, 489.f, 2226.f, -24.f, EPenguinAbility::FrostBreath, EPenguinAbility::Gust } })
	{
		AddStep(Case.Name, 10.f,
			[this, Base, Case, PrepareBot, StartBot]
			{
				PortalEndA = Base + FVector(Case.EndA, 0.f, 0.f);
				PortalEndB = Base + FVector(Case.SpotX, 0.f, 0.f); // (our penguin's spot)
				PrepareBot(Case.BotX, Case.SpotX);
				TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), PortalEndA, Base + FVector(Case.EndB, 0.f, 0.f));
				StartBot({ Case.First, Case.Second }, 1);
			},
			[this, Base, Case, KeepTargetOnSpot](float T)
			{
				const APenguinCharacter* Bot = Target.Get();
				if (!Bot)
				{
					return;
				}
				if (!Bot->IsAlive())
				{
					CountB = 1;
				}
				if (Bot->IsAlive() && CountB == 0 && TimeA < 0.f && FMath::Abs(static_cast<float>(Bot->GetActorLocation().X - Base.X) - Case.SpotX) < 300.f)
				{
					TimeA = T; // past the portal, at our penguin
				}
				if (!bShotDone && TestPortal.IsValid() && TestPortal->GetTransitCount() >= 1)
				{
					bShotDone = true;
					Shot(*FString::Printf(TEXT("114_%s"), Case.Name));
				}
				KeepTargetOnSpot(T);
			},
			[this, Case, StopBot]
			{
				const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : -1;
				const bool bOk = Transits >= 0 && Transits <= 2 && TimeA > 0.f && TimeA < 7.f && CountB == 0;
				Report(FString::Printf(TEXT("RESULT %s (ends %.0f cm apart): through the portal %d times in 10 s (want at most 2), at our penguin after %.2f s (want within 7), knocked out %d (want 0): %s"),
					Case.Name, FMath::Abs(Case.EndB - Case.EndA), Transits, TimeA, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
				StopBot();
			});
	}

	// ---- A wall too high to jump (8 blocks of rock) between the bot and our penguin, a portal across it: the only way.
	AddStep(TEXT("bot_PortalShortcutWall"), 4.f,
		[this, Base, PrepareBot, StartBot]
		{
			PortalEndA = Base + FVector(300.f, 0.f, 0.f);
			PortalEndB = Base + FVector(1500.f, 0.f, 0.f); // (our penguin's spot)
			PrepareBot(0.f, 1500.f);
			if (ABlockIsland* Flat = ABlockIsland::FindNear(GetWorld(), Base))
			{
				const FIntPoint Foot = Flat->WorldToCell(Base + FVector(650.f, 0.f, 10.f));
				TArray<FIntPoint> Cells;
				for (int32 Row = 0; Row < 8; ++Row)
				{
					Cells.Add(FIntPoint(Foot.X, Foot.Y + Row));
				}
				Flat->AddBlocks(Cells, EBlockKind::Rock, 0.f);
			}
			TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), PortalEndA, Base + FVector(1000.f, 0.f, 0.f));
			StartBot({ EPenguinAbility::IceShell, EPenguinAbility::IceWall }, 1);
		},
		[this, Base](float T)
		{
			const APenguinCharacter* Bot = Target.Get();
			if (!Bot)
			{
				return;
			}
			if (!Bot->IsAlive())
			{
				CountB = 1;
			}
			if (Bot->IsAlive() && CountB == 0 && TimeA < 0.f && Bot->GetActorLocation().X > Base.X + 900.f)
			{
				TimeA = T; // on our side of the wall
			}
			if (TimeA > 0.f && !bShotDone && T > TimeA + 0.15f)
			{
				bShotDone = true;
				Shot(TEXT("114_bot_portal_use"));
			}
		},
		[this, StopBot]
		{
			const int32 Transits = TestPortal.IsValid() ? TestPortal->GetTransitCount() : -1;
			const bool bOk = Transits >= 1 && Transits <= 2 && TimeA > 0.f && TimeA < 3.f && CountB == 0;
			Report(FString::Printf(TEXT("RESULT bot portal shortcut wall: through the portal %d times (want 1 or 2), on our side after %.2f s (want within 3), knocked out %d (want 0): %s"),
				Transits, TimeA, CountB, bOk ? TEXT("ok") : TEXT("FAIL")));
			StopBot();
		});

	// ---- Many portals, many fights: random layouts (ends on the strip or up in the air, near or far apart), the bot with two
	// random close-range abilities, our penguin standing somewhere. Per layout the bot's own trips (its place jumping from
	// one end into the other) in 6 s: 3 or more is a loop. The layouts that looped are logged.
	struct FPortalFuzz
	{
		FRandomStream Rand{ 20261107 };
		int32 Layouts = 0;
		int32 Loops = 0;
		int32 Trips = 0;
		int32 MostTrips = 0;
		int32 KnockedOut = 0;
		int32 LayoutTrips = 0;
		FVector LastBot = FVector::ZeroVector;
		FVector EndA = FVector::ZeroVector;
		FVector EndB = FVector::ZeroVector;
		FString Layout;
	};
	const TSharedPtr<FPortalFuzz> Fuzz = MakeShared<FPortalFuzz>();
	constexpr int32 FuzzLayouts = 36;
	for (int32 Index = 0; Index < FuzzLayouts; ++Index)
	{
		const bool bLast = Index == FuzzLayouts - 1;
		AddStep(*FString::Printf(TEXT("bot_PortalFuzz%02d"), Index), 6.f,
			[this, Base, Fuzz, PrepareBot, StartBot]
			{
				FRandomStream& Rand = Fuzz->Rand;
				const auto EndHeight = [&Rand]() { return Rand.FRand() < 0.55f ? 0.f : 80.f * Rand.RandRange(1, 6); };
				const float Ax = Rand.FRandRange(-500.f, 2200.f);
				const float Bx = FMath::Clamp(Ax + Rand.FRandRange(-1400.f, 1400.f), -700.f, 2600.f);
				Fuzz->EndA = Base + FVector(Ax, 0.f, EndHeight());
				Fuzz->EndB = Base + FVector(Bx, 0.f, EndHeight());
				float BotX = 0.f;
				float SpotX = 0.f;
				for (int32 Try = 0; Try < 20; ++Try)
				{
					BotX = Rand.FRandRange(-700.f, 2600.f);
					SpotX = Rand.FRandRange(-700.f, 2600.f);
					if (FMath::Abs(BotX - Ax) > 150.f && FMath::Abs(BotX - Bx) > 150.f && FMath::Abs(SpotX - Ax) > 150.f
						&& FMath::Abs(SpotX - Bx) > 150.f && FMath::Abs(SpotX - BotX) > 200.f)
					{
						break;
					}
				}
				static const EPenguinAbility Kit[] = { EPenguinAbility::FlipperSlap, EPenguinAbility::BellyStomp, EPenguinAbility::Noot,
					EPenguinAbility::FrostBreath, EPenguinAbility::IceShell, EPenguinAbility::Gust, EPenguinAbility::IceWall };
				const EPenguinAbility First = Kit[Rand.RandRange(0, UE_ARRAY_COUNT(Kit) - 1)];
				EPenguinAbility Second = Kit[Rand.RandRange(0, UE_ARRAY_COUNT(Kit) - 1)];
				Second = Second == First ? EPenguinAbility::IceShell : Second;
				const int32 Skill = Rand.RandRange(0, 2);
				PortalEndB = Base + FVector(SpotX, 0.f, 0.f); // (our penguin's spot)
				PrepareBot(BotX, SpotX);
				TestPortal = APinguPortal::Open(GetWorld(), Penguin.Get(), Fuzz->EndA, Fuzz->EndB);
				StartBot({ First, Second }, Skill);
				Fuzz->LayoutTrips = 0;
				Fuzz->LastBot = Target.IsValid() ? Target->GetActorLocation() : FVector::ZeroVector;
				Fuzz->Layout = FString::Printf(TEXT("ends %.0f,%.0f -> %.0f,%.0f, bot %.0f, target %.0f, %s + %s, skill %d"),
					Ax, Fuzz->EndA.Z - Base.Z, Bx, Fuzz->EndB.Z - Base.Z, BotX, SpotX, UPenguinAbilityComponent::GetInfo(First).Name,
					UPenguinAbilityComponent::GetInfo(Second).Name, Skill);
			},
			[this, Fuzz, KeepTargetOnSpot](float T)
			{
				const APenguinCharacter* Bot = Target.Get();
				if (!Bot || !TestPortal.IsValid())
				{
					return;
				}
				if (!Bot->IsAlive())
				{
					CountB = 1;
				}
				const FVector Now = Bot->GetActorLocation();
				const float Jumped = static_cast<float>(FVector::Dist(Now, Fuzz->LastBot));
				const float Span = static_cast<float>(FVector::Dist(Fuzz->EndA, Fuzz->EndB));
				if (Bot->IsAlive() && Jumped > 100.f && Jumped > Span * 0.5f
					&& ((TestPortal->IsInEnd(0, Fuzz->LastBot, 120.f) && TestPortal->IsInEnd(1, Now, 120.f))
						|| (TestPortal->IsInEnd(1, Fuzz->LastBot, 120.f) && TestPortal->IsInEnd(0, Now, 120.f))))
				{
					++Fuzz->LayoutTrips;
				}
				Fuzz->LastBot = Now;
				KeepTargetOnSpot(T);
			},
			[this, Fuzz, bLast, StopBot]
			{
				++Fuzz->Layouts;
				Fuzz->Trips += Fuzz->LayoutTrips;
				Fuzz->MostTrips = FMath::Max(Fuzz->MostTrips, Fuzz->LayoutTrips);
				Fuzz->KnockedOut += CountB;
				if (Fuzz->LayoutTrips >= 3)
				{
					++Fuzz->Loops;
					Report(FString::Printf(TEXT("bot portal fuzz: LOOP %d trips in 6 s: %s"), Fuzz->LayoutTrips, *Fuzz->Layout));
				}
				StopBot();
				if (bLast)
				{
					const bool bOk = Fuzz->Loops == 0;
					Report(FString::Printf(TEXT("RESULT bot portal fuzz: %d layouts, %d trips, at most %d in one (6 s), %d loops (3 or more: want 0), bot knocked out in %d: %s"),
						Fuzz->Layouts, Fuzz->Trips, Fuzz->MostTrips, Fuzz->Loops, Fuzz->KnockedOut, bOk ? TEXT("ok") : TEXT("FAIL")));
				}
			});
	}
}

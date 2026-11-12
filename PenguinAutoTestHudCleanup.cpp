// Pingu - the HUD cleanup's checks (2026-11-07, card hud-cleanup): the current trait is only its badge in the top-left corner
// for the whole round (no wooden plate), the kill feed and the FPS counter have no plate. APenguinAutoTest::AddHudCleanupSteps:
//
//  look_HudTraitCorner   our penguin gets Giant with a reveal, the FPS counter is on and the feed has lines: while the reveal's
//                        cards fly the corner icon is not there; once the penguin changed it is, in the top-left corner of the
//                        window (its left / top within 6 % of the width / 12 % of the height, 4-12 % of the height big); when
//                        the trait goes it goes. Shots 120_hud_corner_* (with the UI).

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinTraitComponent.h"
#include "PinguSettings.h"
#include "PinguUI.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

namespace PenguinAutoTestHudCleanupPrivate
{
	struct FCorner
	{
		float Start = 0.f;
		bool bFpsWas = false;
		bool bShotEarly = false;
		bool bShotLate = false;
		bool bFreshFeed = false;
		bool bCleared = false;
		uint64 ClearedFrame = 0;
		bool bShownDuringFlight = false;
		bool bShownAfter = false;
		bool bShownAfterClear = false;
		bool bTrait = false;
		FVector2f Viewport = FVector2f::ZeroVector;
		FSlateRect Rect;
		float Size = 0.f;
	};
	constexpr float Lead = 0.3f;
}

void APenguinAutoTest::AddHudCleanupSteps()
{
	using namespace PenguinAutoTestHudCleanupPrivate;
	const TSharedRef<FCorner> L = MakeShared<FCorner>();
	AddStep(TEXT("look_HudTraitCorner"), Lead + UPenguinTraitComponent::RevealEndAt + 2.2f,
		[this, L]
		{
			*L = FCorner();
			SpawnTarget();
			PrepareBrawl(900.f, {});
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				FPinguGraphicsOptions Options = Settings->GetGraphics();
				L->bFpsWas = Options.bShowFPS;
				Options.bShowFPS = true;
				Settings->SetGraphics(Options);
			}
			if (APenguinGameState* GameState = APenguinGameState::Get(this))
			{
				L->Start = static_cast<float>(GameState->GetFlowTime()) + Lead;
				GameState->AddFeed(TEXT("FROSTY knocked out WADDLES"), FLinearColor(1.f, 0.55f, 0.2f));
				GameState->AddFeed(TEXT("PEBBLE joined"), FLinearColor(0.5f, 0.9f, 1.f));
				GameState->AddFeed(TEXT("NOOT left"), FLinearColor(0.8f, 0.8f, 0.8f));
			}
			if (UPenguinTraitComponent* Traits = Penguin.IsValid() ? Penguin->GetTraits() : nullptr)
			{
				Traits->SetTrait(EPinguTrait::Giant, L->Start);
			}
			if (GEngine && GEngine->GameViewport)
			{
				FVector2D Size;
				GEngine->GameViewport->GetViewportSize(Size);
				L->Viewport = FVector2f(static_cast<float>(Size.X), static_cast<float>(Size.Y));
			}
		},
		[this, L](float T)
		{
			FCorner& S = *L;
			const PinguUI::FTraitCornerStats& Stats = PinguUI::GetTraitCornerStats();
			const float Age = T - Lead;
			const bool bShown = Stats.bShown && Stats.Trait == EPinguTrait::Giant;
			if (Age > 0.4f && Age < UPenguinTraitComponent::TransformAt - 0.05f && Stats.bShown)
			{
				S.bShownDuringFlight = true;
			}
			if (!S.bShotEarly && Age > 1.6f)
			{
				S.bShotEarly = true;
				Shot(TEXT("120_hud_corner_reveal"), true);
			}
			if (Age > UPenguinTraitComponent::RevealEndAt + 0.1f && Age < UPenguinTraitComponent::RevealEndAt + 1.1f && bShown)
			{
				S.bShownAfter = true;
				S.Rect = Stats.Rect;
				S.Size = Stats.Size;
			}
			if (!S.bFreshFeed && Age > UPenguinTraitComponent::RevealEndAt + 0.4f)
			{
				// Fresh lines, so the shot shows them at full opacity (the first ones are old by now).
				S.bFreshFeed = true;
				if (APenguinGameState* GameState = APenguinGameState::Get(this))
				{
					GameState->AddFeed(TEXT("FROSTY knocked out WADDLES"), FLinearColor(1.f, 0.55f, 0.2f));
					GameState->AddFeed(TEXT("PEBBLE joined"), FLinearColor(0.5f, 0.9f, 1.f));
					GameState->AddFeed(TEXT("NOOT left"), FLinearColor(0.8f, 0.8f, 0.8f));
				}
			}
			if (!S.bShotLate && Age > UPenguinTraitComponent::RevealEndAt + 0.7f)
			{
				S.bShotLate = true;
				Shot(TEXT("120_hud_corner_icon"), true);
			}
			if (!S.bCleared && Age > UPenguinTraitComponent::RevealEndAt + 1.3f)
			{
				S.bCleared = true;
				S.ClearedFrame = GFrameCounter;
				if (UPenguinTraitComponent* Traits = Penguin.IsValid() ? Penguin->GetTraits() : nullptr)
				{
					Traits->ClearTrait();
				}
			}
			if (S.bCleared && GFrameCounter > S.ClearedFrame + 3 && Stats.Frame > S.ClearedFrame + 2 && Stats.bShown)
			{
				S.bShownAfterClear = true;
			}
		},
		[this, L]
		{
			FCorner& S = *L;
			if (UPinguSettings* Settings = UPinguSettings::Get())
			{
				FPinguGraphicsOptions Options = Settings->GetGraphics();
				Options.bShowFPS = S.bFpsWas;
				Settings->SetGraphics(Options);
			}
			if (UPenguinTraitComponent* Traits = Penguin.IsValid() ? Penguin->GetTraits() : nullptr)
			{
				Traits->ClearTrait();
			}
			PrepareBrawl(2400.f, {});
			const float W = FMath::Max(S.Viewport.X, 1.f);
			const float H = FMath::Max(S.Viewport.Y, 1.f);
			const bool bPlace = S.Rect.Left >= 0.f && S.Rect.Top >= 0.f && S.Rect.Left <= 0.06f * W && S.Rect.Top <= 0.12f * H;
			const float Big = S.Rect.GetSize().Y / H;
			const bool bSize = Big >= 0.04f && Big <= 0.12f;
			const bool bOk = S.bShownAfter && !S.bShownDuringFlight && !S.bShownAfterClear && bPlace && bSize;
			Report(FString::Printf(TEXT("RESULT hud trait corner: shown after the change %d (want 1), during the flight %d (want 0), after the trait went %d (want 0), "
				"icon at %.0f, %.0f px, %.0f px high in a %.0f x %.0f window (%.1f %% of its height, want 4-12), in the corner %d: %s"),
				S.bShownAfter ? 1 : 0, S.bShownDuringFlight ? 1 : 0, S.bShownAfterClear ? 1 : 0, S.Rect.Left, S.Rect.Top, S.Rect.GetSize().Y, W, H, 100.f * Big,
				bPlace ? 1 : 0, bOk ? TEXT("ok") : TEXT("FAIL")));
		});
}

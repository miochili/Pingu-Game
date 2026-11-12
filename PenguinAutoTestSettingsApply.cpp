// Pingu - the settings page's APPLY / "Apply changes?" board checks (2026-11-07, card settings-apply-res). Run inside the lobbytest's
// 3D menu stage (PenguinAutoTestMenu3D.cpp, UpdateMenu3DLobbyCheck) on the real settings page, one RESULT line:
//   - APPLY is greyed while nothing is changed;
//   - a changed volume + a window mode draft (Fullscreen: the draft's resolution is the monitor's native one, the real window
//     mode and resolution stay untouched until APPLY);
//   - BACK with changes opens the board (the page stays); Esc (TogglePauseMenu) goes back into the settings; BACK again, then
//     the keyboard / gamepad path: APPLY has the focus, Right moves it to DISCARD, Enter (Accept) presses it: the old values are
//     back (volume, window mode), the lobby shows, nothing is pending;
//   - the settings again: a change, a real click on the APPLY plank: nothing pending, APPLY greys out, the volume stays, the real
//     window mode / resolution take the draft (Fullscreen at the native resolution), then everything is put back (nothing is saved
//     in an automated run);
//   - the native resolution rule as plain numbers (UPinguSettings::ResolveResolution).

#include "PenguinAutoTest.h"
#include "Penguin.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "PinguSettings.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"

namespace PenguinAutoTestSettingsApplyPrivate
{
	struct FApplyFlow
	{
		int32 Stage = 0;
		float StageAt = -1.f;
		float MasterBefore = 1.f;
		EWindowMode::Type ModeBefore = EWindowMode::Windowed;
		FIntPoint ResolutionBefore = FIntPoint::ZeroValue;
		bool bCleanDisabled = false;
		bool bDraftOnly = false;
		bool bDraftNative = false;
		bool bBoardShown = false;
		bool bEscClosed = false;
		bool bBoardAgain = false;
		bool bFocusOnApply = false;
		bool bFocusMoved = false;
		bool bDiscarded = false;
		bool bApplyClicked = false;
		bool bApplied = false;
		bool bApplyGreyed = false;
		bool bRealMode = false;
		bool bRestored = false;
		bool bRule = false;
	};

	static FApplyFlow Flow;

	static int32 FindButton(const APinguMenu3D* Menu, FName Id)
	{
		for (int32 Index = 0; Menu && Index < Menu->GetButtonCount(); ++Index)
		{
			if (Menu->GetButtonInfo(Index).Id == Id)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}

bool PinguSettingsApplyFlow(APenguinHUD* HUD, APinguMenu3D* Menu, float Now, TFunctionRef<void(bool, const FString&)> Check)
{
	using namespace PenguinAutoTestSettingsApplyPrivate;
	FApplyFlow& F = Flow;
	UPinguSettings* Prefs = UPinguSettings::Get();
	UGameUserSettings* Real = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (!HUD || !Menu || !Prefs || !Real)
	{
		Check(false, TEXT("settings APPLY: no HUD / menu / settings"));
		return false;
	}
	auto Click = [Menu](FName Id)
	{
		const int32 Index = FindButton(Menu, Id);
		return Index != INDEX_NONE && Menu->GetButtonInfo(Index).bShown && Menu->GetButtonInfo(Index).bEnabled && Menu->PointerDown(Menu->GetButtonInfo(Index).ScreenCenter);
	};
	if (F.StageAt < 0.f)
	{
		F.StageAt = Now;
	}
	switch (F.Stage)
	{
	case 0:
	{
		if (Now - F.StageAt < 0.5f)
		{
			return true;
		}
		const FIntPoint NativeRes = UPinguSettings::GetNativeResolution();
		F.MasterBefore = Prefs->GetMasterVolume();
		F.ModeBefore = Real->GetFullscreenMode();
		F.ResolutionBefore = Real->GetScreenResolution();
		const int32 Apply = FindButton(Menu, TEXT("Apply"));
		F.bCleanDisabled = Apply != INDEX_NONE && Menu->GetButtonInfo(Apply).bShown && !Menu->GetButtonInfo(Apply).bEnabled && !HUD->HasSettingsPending();
		// Two changes: a volume, and Fullscreen as a draft.
		Prefs->SetVolumes(F.MasterBefore > 0.5f ? 0.3f : 0.9f, Prefs->GetMusicVolume(), Prefs->GetSfxVolume());
		const bool bToFullscreen = F.ModeBefore != EWindowMode::Fullscreen;
		Prefs->SetWindowMode(bToFullscreen ? EWindowMode::Fullscreen : EWindowMode::Windowed);
		F.bDraftOnly = Real->GetFullscreenMode() == F.ModeBefore && Real->GetScreenResolution() == F.ResolutionBefore && HUD->HasSettingsPending();
		F.bDraftNative = !bToFullscreen || (Prefs->GetWindowMode() == EWindowMode::Fullscreen && Prefs->GetResolution() == NativeRes);
		HUD->Action_Back();
		F.bBoardShown = HUD->HasSettingsConfirm() && HUD->GetScreen() == EPinguScreen::Settings;
		F.StageAt = Now;
		F.Stage = 1;
		return true;
	}
	case 1:
	{
		if (Now - F.StageAt < 0.6f)
		{
			return true;
		}
		F.bBoardShown = F.bBoardShown && Menu->GetOverlay() == EPinguMenu3DPage::Confirm && FindButton(Menu, TEXT("ConfirmApply")) != INDEX_NONE && FindButton(Menu, TEXT("ConfirmDiscard")) != INDEX_NONE;
		// Esc: back into the settings, the changes still pending.
		HUD->TogglePauseMenu();
		F.bEscClosed = !HUD->HasSettingsConfirm() && HUD->GetScreen() == EPinguScreen::Settings && HUD->HasSettingsPending();
		F.StageAt = Now;
		F.Stage = 10;
		return true;
	}
	case 10:
	{
		if (Now - F.StageAt < 0.6f)
		{
			return true;
		}
		// The page is back as it was (its planks and buttons shown again, the page's own buttons reachable), then BACK again.
		const int32 Apply = FindButton(Menu, TEXT("Apply"));
		const int32 Preset = FindButton(Menu, TEXT("Preset0"));
		F.bEscClosed = F.bEscClosed && Menu->GetOverlay() == EPinguMenu3DPage::None && Apply != INDEX_NONE && Menu->GetButtonInfo(Apply).bShown && Menu->GetButtonInfo(Apply).bEnabled
			&& Preset != INDEX_NONE && Menu->GetButtonInfo(Preset).bShown;
		HUD->Action_Back();
		F.bBoardAgain = HUD->HasSettingsConfirm();
		F.StageAt = Now;
		F.Stage = 2;
		return true;
	}
	case 2:
	{
		if (Now - F.StageAt < 0.6f)
		{
			return true;
		}
		// The keyboard / gamepad path: APPLY has the focus, Right moves to DISCARD, Enter presses it.
		F.bFocusOnApply = Menu->GetOverlay() == EPinguMenu3DPage::Confirm && Menu->GetHotId() == FName(TEXT("ConfirmApply"));
		Menu->Navigate(EUINavigation::Right);
		F.bFocusMoved = Menu->GetHotId() == FName(TEXT("ConfirmDiscard"));
		Menu->Accept();
		F.StageAt = Now;
		F.Stage = 3;
		return true;
	}
	case 3:
	{
		if (Now - F.StageAt < 0.8f)
		{
			return true;
		}
		F.bDiscarded = HUD->GetScreen() != EPinguScreen::Settings && !HUD->HasSettingsConfirm() && FMath::IsNearlyEqual(Prefs->GetMasterVolume(), F.MasterBefore, 0.0001f)
			&& Prefs->GetWindowMode() == F.ModeBefore && Real->GetFullscreenMode() == F.ModeBefore && Real->GetScreenResolution() == F.ResolutionBefore && !Prefs->IsEditSession();
		// The settings again.
		Click(TEXT("Settings"));
		F.StageAt = Now;
		F.Stage = 4;
		return true;
	}
	case 4:
	{
		if (Now - F.StageAt < 0.8f)
		{
			return true;
		}
		if (HUD->GetScreen() != EPinguScreen::Settings || !Prefs->IsEditSession())
		{
			F.Stage = 99;
			Check(false, TEXT("settings APPLY: the settings did not open again after DISCARD"));
			return false;
		}
		Prefs->SetVolumes(F.MasterBefore > 0.5f ? 0.3f : 0.9f, Prefs->GetMusicVolume(), Prefs->GetSfxVolume());
		Prefs->SetWindowMode(EWindowMode::Fullscreen);
		const bool bPending = HUD->HasSettingsPending();
		F.bApplyClicked = Click(TEXT("Apply")) && bPending;
		F.StageAt = Now;
		F.Stage = 5;
		return true;
	}
	case 5:
	{
		if (Now - F.StageAt < 0.8f)
		{
			return true;
		}
		const float Want = F.MasterBefore > 0.5f ? 0.3f : 0.9f;
		const int32 Apply = FindButton(Menu, TEXT("Apply"));
		F.bApplied = !HUD->HasSettingsPending() && FMath::IsNearlyEqual(Prefs->GetMasterVolume(), Want, 0.0001f);
		F.bApplyGreyed = Apply != INDEX_NONE && !Menu->GetButtonInfo(Apply).bEnabled;
		F.bRealMode = Real->GetFullscreenMode() == EWindowMode::Fullscreen && Real->GetScreenResolution() == UPinguSettings::GetNativeResolution();
		// Everything back (an automated run saves nothing).
		Prefs->SetVolumes(F.MasterBefore, Prefs->GetMusicVolume(), Prefs->GetSfxVolume());
		Prefs->SetWindowMode(F.ModeBefore);
		if (F.ModeBefore == EWindowMode::Windowed)
		{
			Prefs->SetResolution(F.ResolutionBefore);
		}
		Prefs->ApplyEditSession();
		F.bRestored = FMath::IsNearlyEqual(Prefs->GetMasterVolume(), F.MasterBefore, 0.0001f) && Real->GetFullscreenMode() == F.ModeBefore
			&& (F.ModeBefore != EWindowMode::Windowed || Real->GetScreenResolution() == F.ResolutionBefore) && !Prefs->HasPendingChanges();
		// The rule as numbers.
		const FIntPoint Small(1280, 720);
		const FIntPoint Big(2560, 1440);
		F.bRule = UPinguSettings::ResolveResolution(EWindowMode::Fullscreen, Small, Big) == Big
			&& UPinguSettings::ResolveResolution(EWindowMode::WindowedFullscreen, Small, Big) == Big
			&& UPinguSettings::ResolveResolution(EWindowMode::Windowed, Small, Big) == Small
			&& UPinguSettings::ResolveResolution(EWindowMode::Fullscreen, Small, FIntPoint::ZeroValue) == Small
			&& UPinguSettings::IsNativeResolutionMode(EWindowMode::Fullscreen) && UPinguSettings::IsNativeResolutionMode(EWindowMode::WindowedFullscreen)
			&& !UPinguSettings::IsNativeResolutionMode(EWindowMode::Windowed);
		const bool bOk = F.bCleanDisabled && F.bDraftOnly && F.bDraftNative && F.bBoardShown && F.bEscClosed && F.bBoardAgain && F.bFocusOnApply && F.bFocusMoved
			&& F.bDiscarded && F.bApplyClicked && F.bApplied && F.bApplyGreyed && F.bRealMode && F.bRestored && F.bRule;
		Check(bOk, FString::Printf(TEXT("settings APPLY: APPLY greyed while clean %d; a volume + a Fullscreen draft changed only the draft %d (native %d); BACK opened the board %d, Esc closed it %d, BACK again %d; keyboard: focus on APPLY %d, Right to DISCARD %d, Enter discarded (old values back, settings closed) %d; click on APPLY: pending gone %d, saved %d, greyed %d, real window mode Fullscreen at the native resolution %d; restored %d; native rule %d"),
			F.bCleanDisabled ? 1 : 0, F.bDraftOnly ? 1 : 0, F.bDraftNative ? 1 : 0, F.bBoardShown ? 1 : 0, F.bEscClosed ? 1 : 0, F.bBoardAgain ? 1 : 0, F.bFocusOnApply ? 1 : 0,
			F.bFocusMoved ? 1 : 0, F.bDiscarded ? 1 : 0, F.bApplyClicked ? 1 : 0, F.bApplied ? 1 : 0, F.bApplyGreyed ? 1 : 0, F.bRealMode ? 1 : 0, F.bRestored ? 1 : 0, F.bRule ? 1 : 0));
		F.Stage = 99;
		return false;
	}
	default:
		return false;
	}
}

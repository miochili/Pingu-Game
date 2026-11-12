// Pingu - the lobby test's check of the host's address line (APinguLobbyTest::UpdateCopyAddressCheck): the line copies the
// address it shows to the clipboard, says COPIED! for a moment and then goes back. It runs in the solo lobby on the real
// HUD and the real LAN subsystem (its adapters, not the menu screenshots' made-up address): the line itself shows only for
// a LAN host, and the screenshots' check presses it with a real key (APenguinHUD::CheckCopyAddressShot).
//
// A test must leave nothing of its own on the player's clipboard: its text is saved first and put back at the end.

#include "PenguinAutoTest.h"
#include "PenguinHUD.h"
#include "PinguNetSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"

namespace PenguinAutoTestCopyAddressPrivate
{
	double SlateNow()
	{
		return FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetCurrentTime() : FPlatformTime::Seconds();
	}

	/** COPIED! is looked at in the middle of its time, and after it (real seconds since the copy). */
	constexpr double MidAt = 1.0;
	constexpr double GoneAt = APenguinHUD::CopiedFeedbackTime + 0.2;

	/** A home / office network address: 192.168.x.x, 10.x.x.x, 172.16.x.x - 172.31.x.x. */
	bool IsPrivateAddress(const FString& Ip)
	{
		TArray<FString> Parts;
		Ip.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() != 4)
		{
			return false;
		}
		const int32 A = FCString::Atoi(*Parts[0]);
		const int32 B = FCString::Atoi(*Parts[1]);
		return A == 10 || (A == 192 && B == 168) || (A == 172 && B >= 16 && B <= 31);
	}

	/** "a.b.c.d" with an optional ":port". */
	bool LooksLikeAddress(const FString& Text)
	{
		FString Host = Text;
		int32 Colon = INDEX_NONE;
		if (Text.FindChar(TEXT(':'), Colon))
		{
			Host = Text.Left(Colon);
			const FString Port = Text.Mid(Colon + 1);
			if (Port.IsEmpty() || !Port.IsNumeric())
			{
				return false;
			}
		}
		TArray<FString> Parts;
		Host.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() != 4)
		{
			return false;
		}
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || !Part.IsNumeric() || FCString::Atoi(*Part) > 255)
			{
				return false;
			}
		}
		return true;
	}
}

bool APinguLobbyTest::UpdateCopyAddressCheck(APenguinHUD* LobbyHUD, TFunctionRef<void(bool, const FString&)> Check)
{
	using namespace PenguinAutoTestCopyAddressPrivate;
	constexpr int32 Done = 99;
	if (CopyStep == Done)
	{
		return false;
	}
	switch (CopyStep)
	{
	case 0:
	{
		// The player's clipboard text is kept; the clipboard gets something else; the action runs; what it put there is read.
		CopySaved = APenguinHUD::ReadClipboardText();
		APenguinHUD::CopyTextToClipboard(TEXT("pingu test: not the address"));
		const bool bBefore = LobbyHUD->IsHostAddressCopied();
		LobbyHUD->Action_CopyHostAddress();
		CopyShown = LobbyHUD->GetHostAddressText();
		CopyHeld = APenguinHUD::ReadClipboardText();
		CopyAt = SlateNow();
		// This PC's LAN address is the one the system routes outwards from (its Wi-Fi / cable), never a virtual adapter's
		// (VMware / Hyper-V / VirtualBox host-only networks are 192.168.x.1 too: the line once showed the VMware one here).
		CopyRoute = UPinguNetSubsystem::GetRouteAddressText();
		int32 Colon = INDEX_NONE;
		const FString ShownHost = CopyShown.FindChar(TEXT(':'), Colon) ? CopyShown.Left(Colon) : CopyShown;
		bCopyRouteOk = !IsPrivateAddress(CopyRoute) || ShownHost == CopyRoute;
		bCopyShownBefore = bBefore;
		bCopyShownAtOnce = LobbyHUD->IsHostAddressCopied();
		bCopyShownMid = false;
		CopyStep = 1;
		return true;
	}

	case 1:
	{
		const double Age = SlateNow() - CopyAt;
		if (Age >= MidAt && !bCopyShownMidSampled)
		{
			bCopyShownMid = LobbyHUD->IsHostAddressCopied();
			bCopyShownMidSampled = true;
		}
		if (Age < GoneAt)
		{
			return true;
		}
		const bool bGone = !LobbyHUD->IsHostAddressCopied();
		const bool bRestored = APenguinHUD::CopyTextToClipboard(CopySaved);
		const bool bOk = !CopyShown.IsEmpty() && CopyHeld == CopyShown && LooksLikeAddress(CopyShown) && bCopyRouteOk && !bCopyShownBefore && bCopyShownAtOnce
			&& bCopyShownMid && bGone && bRestored;
		Check(bOk, FString::Printf(TEXT("copy the host address: the clipboard holds \"%s\" (the lobby line shows \"%s\", %s; the route out is \"%s\": same %d), COPIED! was off before %d, on at once %d, still on after %.1f s %d, gone after %.2f s %d; the clipboard text from before put back (%d characters) %d"),
			*CopyHeld, *CopyShown, LooksLikeAddress(CopyShown) ? TEXT("an address") : TEXT("NOT an address"), CopyRoute.IsEmpty() ? TEXT("none") : *CopyRoute,
			bCopyRouteOk ? 1 : 0, bCopyShownBefore ? 0 : 1, bCopyShownAtOnce ? 1 : 0, MidAt, bCopyShownMid ? 1 : 0, Age, bGone ? 1 : 0, CopySaved.Len(),
			bRestored ? 1 : 0));
		CopyStep = Done;
		return false;
	}

	default:
		return false;
	}
}

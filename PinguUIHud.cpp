// Pingu - the root widget (every screen in one overlay, the crisp DPI scale, keys, focus targets), shared building
// pieces of the screens, and the in-game HUD: banner, countdown, leaderboard between rounds, help, lobby pill, FPS.
// Everything here reads APenguinGameState through the HUD: it works the same on the host and on LAN clients.

#include "PinguUI.h"
#include "PinguWoodStyle.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "PinguMenu3DOverlay.h"
#include "PinguSettings.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PinguUI"

namespace PinguUIHudPrivate
{
	/** A font for words straight on the scene (the FPS counter, the feed; no plate behind them): a dark edge, plus the text's own soft shadow. */
	static FSlateFontInfo PlainTextFont(FSlateFontInfo Font)
	{
		Font.OutlineSettings.OutlineSize = FMath::Max(Font.OutlineSettings.OutlineSize, 3);
		Font.OutlineSettings.OutlineColor = FLinearColor(0.02f, 0.015f, 0.01f, 1.f);
		return Font;
	}

	/** Seconds the leaderboard takes to come in, and the delay between its rows. */
	constexpr float BoardInTime = 0.18f;
	constexpr float BoardRowDelay = 0.04f;
	/** The round winner's new win fills in this long after the leaderboard starts to come in, with a pop of FreshWinPopTime. */
	constexpr float FreshWinDelay = 0.4f;
	constexpr float FreshWinPopTime = 0.3f;
	/** While the leaderboard shows, the banner moves up by this much (a translation: the glyphs stay sharp). */
	constexpr float BannerLift = 150.f;
	/** A new banner text drops in from this far above. */
	constexpr float BannerDrop = 28.f;
	constexpr float BannerInTime = 0.14f;
	/** The ring burst behind the countdown numbers. */
	constexpr float BurstTime = 0.5f;
	/** How long "SUDDEN DEATH!" shows. */
	constexpr float SuddenDeathBanner = 1.8f;
	/** Feed lines (knock-outs, players joining / leaving): slide in, stay, fade, then fold away (all done before the
	 *  game state drops a line, after 5 s). */
	constexpr int32 MaxFeedRows = 5;
	constexpr float FeedInTime = 0.14f;
	constexpr float FeedFadeAt = 4.4f;
	constexpr float FeedFadeTime = 0.25f;
	constexpr float FeedFoldTime = 0.16f;
	constexpr float FeedLineHeight = 40.f;
	constexpr float FeedGap = 8.f;
	/** A round not won (yet). */
	static FLinearColor PipOffColor() { return FLinearColor(0.12f, 0.18f, 0.3f, 0.9f); }
	/** The board between rounds: up to this many rows at full size, more get compact rows. */
	constexpr int32 BoardFullRows = 5;
	/** First to at most this many: a pip per win on the board between rounds; more: the count ("7 / 20"). */
	constexpr int32 BoardMaxPips = 10;
	/** The banner moves up by this much more for every compact row (the board is taller). */
	constexpr float BannerLiftPerRow = 22.f;

	/** One line of the controls help: what, and the keys (from the bindings). */
	struct FHelpLine
	{
		FText Label;
		TAttribute<FText> Keys;
		bool bPracticeOnly;
	};

	/** The UI scale: a multiple of 1/8 so the 8-unit grid is whole pixels; about 1 unit = 1 pixel at 1080p. */
	static float ComputeUIScale(const FVector2f& PixelSize)
	{
		const float Raw = FMath::Min(PixelSize.Y / 1000.f, PixelSize.X / 1500.f);
		return FMath::Clamp(FMath::FloorToFloat(Raw * 8.f) / 8.f, 0.5f, 4.f);
	}
}

// ============================================================================ root

void SPinguRoot::Construct(const FArguments& InArgs)
{
	HUD = InArgs._HUD;
	// Only the screens take clicks; everything else goes through to the game viewport.
	SetVisibility(EVisibility::SelfHitTestInvisible);
	ChildSlot
	[
		SNew(SDPIScaler)
		.DPIScale_Lambda([this]() { return DPIFactor; })
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				BuildGameHUD()
			]
			+ SOverlay::Slot()
			[
				BuildMainMenu()
			]
			+ SOverlay::Slot()
			[
				BuildLan()
			]
			+ SOverlay::Slot()
			[
				BuildLobby()
			]
			+ SOverlay::Slot()
			[
				BuildPause()
			]
			+ SOverlay::Slot()
			[
				BuildResults()
			]
			+ SOverlay::Slot()
			[
				BuildSettings()
			]
			+ SOverlay::Slot()
			[
				BuildControls()
			]
			+ SOverlay::Slot()
			[
				BuildPingupedia()
			]
			+ SOverlay::Slot()
			[
				BuildTraitPicks()
			]
			+ SOverlay::Slot()
			[
				BuildError()
			]
			+ SOverlay::Slot()
			[
				BuildCapture()
			]
			// The 3D menus' flat text (the PINGUPEDIA's descriptions, the error popup's message), drawn where the 3D panels are;
			// above the popups' dim, under the FPS counter and the cursor halo. Nothing without the 3D menus.
			+ SOverlay::Slot()
			[
				PinguMenu3D::IsEnabled() ? StaticCastSharedRef<SWidget>(SNew(SPinguMenu3DOverlay).HUD(HUD)) : StaticCastSharedRef<SWidget>(SNew(SSpacer))
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Top)
			.Padding(16.f)
			[
				BuildFPS()
			]
			// A soft dark glow around the mouse pointer (over everything: the system draws the arrow itself on top).
			+ SOverlay::Slot()
			[
				SNew(SPinguCursorHalo)
			]
		]
	];
}

TSharedPtr<SWidget> SPinguRoot::GetScreenWidget(EPinguScreen Screen) const
{
	const TSharedPtr<SWidget>* Found = Screens.Find(static_cast<uint8>(Screen));
	return Found ? *Found : nullptr;
}

TSharedPtr<SWidget> SPinguRoot::GetFocusTarget(EPinguScreen Screen) const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return nullptr;
	}
	// The 3D screens: their input layer (the 3D menu picks the hot button itself: BACK while joining, LEAVE on a client, the
	// settings page's first control, the PINGUPEDIA's selected row).
	if (PinguMenu3D::IsEnabled() && Screen != EPinguScreen::Lobby)
	{
		const TSharedPtr<SWidget>* Layer = FirstFocus.Find(static_cast<uint8>(Screen));
		return Layer ? *Layer : nullptr;
	}
	switch (Screen)
	{
	case EPinguScreen::Lan:
		if (PenguinHUD->IsConnecting())
		{
			return LanBack; // everything else waits while joining
		}
		break;
	case EPinguScreen::Lobby:
		if (LobbyMenu3DInput.IsValid())
		{
			return LobbyMenu3DInput; // the 3D strip (only while it has the keys: APenguinHUD::IsLobbyStripFocused)
		}
		return PenguinHUD->HasMatchAuthority() ? LobbyStart : LobbySettings;
	case EPinguScreen::Settings:
		return SettingsTabFirst[FMath::Clamp(SettingsTab, 0, 2)]; // the first control of the page that shows
	case EPinguScreen::Results:
		if (!PenguinHUD->HasMatchAuthority())
		{
			return ResultsLeave;
		}
		break;
	case EPinguScreen::Pingupedia:
		return GetPingupediaFocus();
	default:
		break;
	}
	const TSharedPtr<SWidget>* Found = FirstFocus.Find(static_cast<uint8>(Screen));
	return Found ? *Found : nullptr;
}

TSharedPtr<SWidget> SPinguRoot::GetErrorFocusTarget() const
{
	return ErrorOk;
}

bool SPinguRoot::IsTyping() const
{
	if (PinguMenu3D::IsEnabled())
	{
		// The 3D LAN page: its hot button is a text field (the name, the address).
		const APinguMenu3D* Menu3D = HUD.IsValid() ? APinguMenu3D::Find(HUD->GetWorld()) : nullptr;
		if (Menu3D && Menu3D->IsTyping())
		{
			return true;
		}
	}
	return (NameBox.IsValid() && NameBox->HasKeyboardFocus()) || (AddressBox.IsValid() && AddressBox->HasKeyboardFocus());
}

void SPinguRoot::OnScreenShown(EPinguScreen Screen)
{
	Hint = FText::GetEmpty();
	if (Screen == EPinguScreen::Settings)
	{
		SettingsTab = 0;
		if (APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() && HUD.IsValid() ? APinguMenu3D::Get(HUD.Get()) : nullptr)
		{
			Menu3D->SelectSettingsTab(0, false); // a fresh visit starts on GRAPHICS
		}
	}
	if (Screen == EPinguScreen::Pingupedia)
	{
		OnPingupediaShown();
	}
}

void SPinguRoot::CommitTextBoxes()
{
	if (APinguMenu3D* Menu3D = PinguMenu3D::IsEnabled() && HUD.IsValid() ? APinguMenu3D::Get(HUD.Get()) : nullptr)
	{
		Menu3D->CommitFields();
	}
	if (NameBox.IsValid() && HUD.IsValid())
	{
		HUD->Action_SetPlayerName(NameBox->GetText().ToString());
	}
}

FReply SPinguRoot::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// Keys the focused control did not take. The menus own the input here (the game gets none): Esc / gamepad B /
	// Start and the pause keys back out of a screen, resume, or close the lobby card (TogglePauseMenu).
	APenguinHUD* PenguinHUD = HUD.Get();
	if (PenguinHUD && !InKeyEvent.IsRepeat() && (PenguinHUD->GetScreen() != EPinguScreen::None || PenguinHUD->HasErrorPopup()))
	{
		const FKey Key = InKeyEvent.GetKey();
		bool bBack = Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right || Key == EKeys::Gamepad_Special_Right;
		if (!bBack && !IsTyping())
		{
			// P and the other pause keys, but not while a name is typed.
			const UPinguSettings* Settings = UPinguSettings::Get();
			bBack = Settings && Settings->GetKeys(EPinguInputAction::Pause).Contains(Key);
		}
		if (bBack)
		{
			PenguinHUD->TogglePauseMenu();
			return FReply::Handled();
		}
		// Settings pages: the shoulder buttons flip through them (keyboard: click a tab or use Tab).
		if (PenguinHUD->GetScreen() == EPinguScreen::Settings && !PenguinHUD->HasErrorPopup()
			&& (Key == EKeys::Gamepad_LeftShoulder || Key == EKeys::Gamepad_RightShoulder))
		{
			PenguinHUD->PlayUISound(false);
			SelectSettingsTab(SettingsTab + (Key == EKeys::Gamepad_RightShoulder ? 1 : -1), true);
			return FReply::Handled();
		}
		// The PINGUPEDIA's tabs (the shoulder buttons, ...).
		if (PenguinHUD->GetScreen() == EPinguScreen::Pingupedia && !PenguinHUD->HasErrorPopup() && HandlePingupediaKey(InKeyEvent))
		{
			return FReply::Handled();
		}
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SPinguRoot::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	using namespace PinguUIHudPrivate;
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	// Crisp DPI: the whole UI at a multiple of 1/8 pixels per unit, whatever the engine's own DPI curve says.
	const float EngineScale = FMath::Max(AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), 0.01f);
	const FVector2f PixelSize = AllottedGeometry.GetAbsoluteSize();
	if (PixelSize.X > 16.f && PixelSize.Y > 16.f)
	{
		UIScale = ComputeUIScale(PixelSize);
		DPIFactor = UIScale / EngineScale;
		PinguUI::SetUIScale(UIScale);
	}

	// FPS counter (real frame time, 4 updates a second).
	FpsFrames++;
	FpsTime += InDeltaTime;
	if (FpsTime >= 0.25f)
	{
		FpsShown = FMath::RoundToInt(static_cast<float>(FpsFrames) / FMath::Max(FpsTime, 0.001f));
		FpsFrames = 0;
		FpsTime = 0.f;
	}

	// A focus move waiting for its widget to show (settings pages).
	if (const TSharedPtr<SWidget> Pending = PendingFocus.Pin())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetAllUserFocus(Pending, EFocusCause::Navigation);
		}
		if (Pending->HasAnyUserFocus().IsSet() || InCurrentTime > PendingFocusUntil)
		{
			PendingFocus.Reset();
		}
	}

	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return;
	}
	const EPinguPhase Phase = PenguinHUD->GetPhase();
	const EPinguScreen Screen = PenguinHUD->GetScreen();

	// Leaderboard rows: built when a round ends, rebuilt only when the round, the line-up or a score changes (on a LAN
	// client the scores may arrive a moment after the phase).
	if (Phase == EPinguPhase::RoundOver && PenguinHUD->GetGameState())
	{
		const int32 Round = PenguinHUD->GetGameState()->GetRoundNumber();
		uint32 ScoreKey = static_cast<uint32>(PenguinHUD->GetRoundWinnerSlot() + 7) * 13u + static_cast<uint32>(PenguinHUD->GetRoundWinnerTeam() + 3);
		for (const FPinguScore& Score : PenguinHUD->GetScores())
		{
			ScoreKey = ScoreKey * 31u + static_cast<uint32>(Score.Slot * 1000 + Score.Wins * 100 + (Score.Team + 1) * 10);
		}
		for (const int32 Team : PenguinHUD->GetTeamBoard())
		{
			ScoreKey = ScoreKey * 17u + static_cast<uint32>(Team * 100 + PenguinHUD->GetTeamWinCount(Team));
		}
		if (BuiltBoardRound != Round || BuiltBoardScores != static_cast<int32>(ScoreKey))
		{
			BuiltBoardRound = Round;
			BuiltBoardScores = static_cast<int32>(ScoreKey);
			RebuildLeaderboard();
		}
	}
	else
	{
		BuiltBoardRound = -1;
	}

	// Lists of the screens: rebuilt when what they show changes (their texts follow the data by themselves).
	if (Screen == EPinguScreen::Results)
	{
		RebuildStandings();
	}
	if (Screen == EPinguScreen::Lobby || Phase == EPinguPhase::Lobby)
	{
		LobbyCards = PenguinHUD->GetLobbyCards(); // (the cards' team and skill read this, once per frame)
		RebuildLobbyPlayers();
	}
	if (Screen == EPinguScreen::Lan)
	{
		RebuildFoundGames();
	}
}

// ============================================================================ shared pieces

TSharedRef<SWidget> SPinguRoot::MakeCard(const TSharedRef<SWidget>& Content, const FMargin& Padding, float Width) const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	return SNew(SBox)
		.WidthOverride(Width > 0.f ? FOptionalSize(Width) : FOptionalSize())
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape).Wood(true)
				.FillColor(Style.Panel)
				.OutlineColor(Style.Ink)
				.OutlineWidth(FPinguStyle::CardLineWidth)
				.Radius(FPinguStyle::CardRadius)
			]
			+ SOverlay::Slot()
			.Padding(Padding)
			[
				Content
			]
		];
}

TSharedRef<SWidget> SPinguRoot::MakeTitle(const FText& Text, float Size) const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	return SNew(STextBlock)
		.Text(Text)
		.Font(FPinguStyle::Title(Size, Size >= 48.f ? 4 : 3))
		.ColorAndOpacity(Style.Snow)
		.ShadowOffset(FVector2D(0.f, 4.f))
		.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.6f));
}

TSharedRef<SWidget> SPinguRoot::MakeSection(const FText& Text) const
{
	return SNew(STextBlock)
		.Text(Text)
		.Font(FPinguStyle::Title(18.f))
		.ColorAndOpacity(FPinguStyle::Get().Ice);
}

TSharedRef<SWidget> SPinguRoot::MakeTextField(TSharedPtr<SEditableTextBox>& OutBox, TAttribute<FText> Text, const FText& HintText, float Width, bool bName)
{
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedPtr<SEditableTextBox>* BoxPtr = &OutBox; // a member of this root: lives as long as the lambdas
	return SNew(SBox)
		.WidthOverride(Width > 0.f ? FOptionalSize(Width) : FOptionalSize())
		.HeightOverride(FPinguStyle::RowHeight)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape)
				.FillColor(Style.PanelDeep)
				.OutlineColor_Lambda([BoxPtr]()
				{
					const FPinguStyle& InnerStyle = FPinguStyle::Get();
					const bool bFocused = BoxPtr->IsValid() && (*BoxPtr)->HasKeyboardFocus();
					const bool bHovered = BoxPtr->IsValid() && (*BoxPtr)->IsHovered();
					return bFocused ? InnerStyle.Ring : (bHovered ? InnerStyle.IceDim : InnerStyle.Ink);
				})
				.OutlineWidth_Lambda([BoxPtr]() { return BoxPtr->IsValid() && (*BoxPtr)->HasKeyboardFocus() ? FPinguStyle::RingWidth : FPinguStyle::LineWidth; })
				.Radius(FPinguStyle::RowRadius)
			]
			+ SOverlay::Slot()
			.VAlign(VAlign_Center)
			[
				SAssignNew(OutBox, SEditableTextBox)
				.Style(&Style.TextBox)
				.Text(Text)
				.HintText(HintText)
				.SelectAllTextWhenFocused(true)
				.ClearKeyboardFocusOnCommit(false)
				.RevertTextOnEscape(false)
				.MaximumLength(bName ? 20 : 64)
				.OnKeyDownHandler_Lambda([](const FGeometry&, const FKeyEvent& KeyEvent) -> FReply
				{
					// Up / Down leave the box (a one-line box would keep them), like on every other control.
					const FKey Key = KeyEvent.GetKey();
					if (Key == EKeys::Up || Key == EKeys::Down)
					{
						return FReply::Handled().SetNavigation(Key == EKeys::Up ? EUINavigation::Up : EUINavigation::Down,
							ENavigationGenesis::Keyboard, ENavigationSource::FocusedWidget);
					}
					return FReply::Unhandled();
				})
				.OnTextCommitted_Lambda([this, bName](const FText& NewText, ETextCommit::Type CommitType)
				{
					if (!HUD.IsValid())
					{
						return;
					}
					if (bName)
					{
						HUD->Action_SetPlayerName(NewText.ToString());
					}
					else if (CommitType == ETextCommit::OnEnter)
					{
						HUD->PlayUISound(true);
						HUD->Action_JoinGame(NewText.ToString());
					}
				})
			]
		];
}

TSharedRef<SWidget> SPinguRoot::MakePortrait(int32 Slot, float Size, int32 TeamRing) const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	const FLinearColor Color = APenguinCharacter::GetSlotColor(Slot);
	// The face, then its beanie layer in the slot's own colour on top (everybody keeps theirs in a team match too).
	TSharedRef<SOverlay> Portrait = SNew(SOverlay);
	if (TeamRing >= 0)
	{
		// The team: a ring in its colour just outside the circle (outside the box: the layout does not change).
		const float Ring = FMath::Clamp(FMath::RoundToFloat(Size * 0.09f), 3.f, 6.f);
		Portrait->AddSlot()
		.Padding(-(Ring + 1.f))
		[
			SNew(SPinguShape)
			.FillColor(FLinearColor::Transparent)
			.OutlineColor(APenguinCharacter::GetTeamColor(TeamRing))
			.OutlineWidth(Ring)
			.Pill(true)
		];
	}
	Portrait->AddSlot()
	[
		SNew(SPinguShape)
		.FillColor(Color * 0.75f + FLinearColor(0.05f, 0.05f, 0.08f, 0.25f))
		.OutlineColor(Style.Ink)
		.OutlineWidth(FPinguStyle::LineWidth)
		.Pill(true)
	];
	Portrait->AddSlot()
	.Padding(3.f)
	[
		SNew(SImage)
		.Image(HUD.IsValid() ? HUD->GetPortraitBrush(Slot, Size) : &Style.NoBrush)
	];
	Portrait->AddSlot()
	.Padding(3.f)
	[
		SNew(SImage)
		.Image(HUD.IsValid() ? HUD->GetPortraitFeltBrush(Slot, Size) : &Style.NoBrush)
		.ColorAndOpacity(APenguinCharacter::GetBeanieTint(Slot))
	];
	return SNew(SBox)
		.WidthOverride(Size)
		.HeightOverride(Size)
		[
			Portrait
		];
}

TSharedRef<SWidget> SPinguRoot::MakeTeamTag(int32 Team, float FontSize) const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	const bool bNone = Team < 0;
	return MakeTag(bNone ? LOCTEXT("TeamNoneTag", "NO TEAM") : APenguinCharacter::GetTeamName(Team),
		bNone ? PinguUI::WithAlpha(Style.IceDim, 0.45f) : APenguinCharacter::GetTeamColor(Team), bNone ? Style.Snow : Style.Ink, FontSize);
}

TSharedRef<SWidget> SPinguRoot::MakeTag(TAttribute<FText> Text, const FLinearColor& Fill, const FLinearColor& TextColor, float FontSize) const
{
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			// (2026-11-07, Linus: "eighter wodden planks or no wood": a small plank, the tag's colour in its words.)
			SNew(SPinguShape).Wood(true)
			.FillColor(FLinearColor(1.f, 1.f, 1.f, Fill.A > 0.6f ? 1.f : 0.8f))
			.OutlineColor(FPinguStyle::Get().Ink)
			.OutlineWidth(FPinguStyle::LineWidth)
			.Pill(true)
		]
		+ SOverlay::Slot()
		.Padding(FMargin(10.f, 2.f, 10.f, 3.f))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Text)
			.Font(PinguWood::OnWoodFont(FPinguStyle::Title(FontSize, 2)))
			.ColorAndOpacity(PinguWood::GetAtlas() ? PinguWood::WordsOf(Fill) : TextColor)
		];
}

TSharedRef<SWidget> SPinguRoot::MakeScreen(EPinguScreen Screen, const TSharedRef<SWidget>& Content, bool bDim)
{
	TSharedRef<SPinguScreen> ScreenWidget = SNew(SPinguScreen)
		.HUD(HUD)
		.Screen(Screen)
		.bDim(bDim)
		[
			Content
		];
	Screens.Add(static_cast<uint8>(Screen), ScreenWidget);
	return ScreenWidget;
}

// ============================================================================ in-game HUD

bool SPinguRoot::IsHUDVisible() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || !PenguinHUD->GetGameState())
	{
		return false;
	}
	const EPinguPhase Phase = PenguinHUD->GetPhase();
	return Phase != EPinguPhase::Menu && Phase != EPinguPhase::MatchOver;
}

TSharedRef<SWidget> SPinguRoot::BuildGameHUD()
{
	using namespace PinguUIHudPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	return SNew(SOverlay)
		.Visibility(EVisibility::HitTestInvisible)

		// ---- a LAN client that is still joining: the game state is not there yet
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Visibility_Lambda([this]()
			{
				const APenguinHUD* PenguinHUD = HUD.Get();
				const UWorld* World = PenguinHUD ? PenguinHUD->GetWorld() : nullptr;
				// (While the join is still pending the LAN screen shows it, with a CANCEL.)
				const bool bJoining = World && World->GetNetMode() == NM_Client && !PenguinHUD->IsConnecting()
					&& (!PenguinHUD->GetGameState() || PenguinHUD->GetPhase() == EPinguPhase::Menu);
				return bJoining ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
			})
			.Text(LOCTEXT("Joining", "JOINING..."))
			.Font(FPinguStyle::Title(48.f, 4))
			.ColorAndOpacity(Style.Snow)
		]

		+ SOverlay::Slot()
		[
			SNew(SOverlay)
			.Visibility_Lambda([this]() { return IsHUDVisible() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })

			// ---- ability icons above the players' heads, bubbles, LAN name tags (under everything else)
			+ SOverlay::Slot()
			[
				SNew(SPinguOverheadAbilities)
				.HUD(HUD)
				// Also in -PinguMatchTest: its screenshots show them. With the 3D menus the board between rounds stands in the world,
				// under every Slate widget: the icons step aside while it shows (the Slate board covered them itself).
				.Visibility_Lambda([this]()
				{
					const APenguinHUD* PenguinHUD = HUD.Get();
					const bool bMenuPage = PenguinHUD && PenguinHUD->GetScreen() != EPinguScreen::None && APenguinHUD::ScreenDims(PenguinHUD->GetScreen());
					return PinguMenu3D::IsEnabled() && (GetLeaderboardIn() > 0.f || bMenuPage) ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
				})
			]

			// ---- Traits mode: the round's trait cards above the penguins and their flight into them (under the banner)
			+ SOverlay::Slot()
			[
				PinguUI::MakeTraitReveal(HUD)
			]

			// ---- Traits mode: the viewer's current trait, only its badge, top left for the whole round
			+ SOverlay::Slot()
			[
				PinguUI::MakeTraitCorner(HUD)
			]

			// ---- leaderboard between rounds (comes in under the winner banner)
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(0.f, 120.f, 0.f, 0.f)
			[
				SAssignNew(LeaderboardCard, SBorder)
				.BorderImage(&Style.NoBrush)
				.Padding(0.f)
				// (With the 3D menus the board between rounds is APinguMenu3D's Board page: this card stays away.)
				.Visibility_Lambda([this]() { return GetLeaderboardIn() > 0.f && !PinguMenu3D::IsEnabled() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				.ColorAndOpacity_Lambda([this]() { return FLinearColor(1.f, 1.f, 1.f, FMath::Clamp(GetLeaderboardIn() * 1.5f, 0.f, 1.f)); })
				.RenderTransform_Lambda([this]()
				{
					return TOptional<FSlateRenderTransform>(FSlateRenderTransform(FVector2f(0.f, PinguUI::SnapUnits(24.f * (1.f - GetLeaderboardIn())))));
				})
				[
					MakeCard(
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SAssignNew(LeaderboardBox, SVerticalBox)
						]
						// The next round waits for a key: once the board has been up a moment, it says so.
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Center)
						.Padding(0.f, 10.f, 0.f, 2.f)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]()
							{
								const APenguinHUD* PenguinHUD = HUD.Get();
								const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
								if (!GameState || !GameState->IsWaitingForContinue())
								{
									return EVisibility::Collapsed;
								}
								return GetContinueHintIn() > 0.f ? EVisibility::HitTestInvisible : EVisibility::Hidden;
							})
							.Text(LOCTEXT("PressAnyKey", "PRESS ANY KEY TO CONTINUE"))
							.Font(FPinguStyle::Title(20.f, 2))
							.ColorAndOpacity_Lambda([this]()
							{
								// In, then a slow gentle pulse.
								const float Pulse = 0.8f + 0.2f * FMath::Cos(static_cast<float>(PinguUI::Now()) * 4.f);
								return FSlateColor(PinguUI::WithAlpha(FPinguStyle::Get().Gold, GetContinueHintIn() * Pulse));
							})
						],
						FMargin(16.f, 16.f))
				]
			]

			// ---- banner: ROUND 1, 3 2 1 GO!, FROSTY WINS!
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 0.f, 140.f)
			[
				BuildBanner()
			]

			// ---- controls help (H / F1)
			+ SOverlay::Slot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			.Padding(24.f, 0.f, 0.f, 0.f)
			[
				BuildHelp()
			]

			// ---- lobby: who is here, while the lobby card is closed
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Top)
			.Padding(0.f, 24.f, 0.f, 0.f)
			[
				BuildLobbyPill()
			]

			// ---- joined during a match: in from the next round (same place as the lobby pill, which never shows in a match)
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Top)
			.Padding(0.f, 24.f, 0.f, 0.f)
			[
				BuildWaitingPill()
			]

			// ---- feed: knock-outs, players joining and leaving (top right, under the FPS counter when it shows)
			+ SOverlay::Slot()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Top)
			[
				SNew(SBox)
				.Padding_Lambda([]()
				{
					const UPinguSettings* Settings = UPinguSettings::Get();
					const bool bFps = Settings && Settings->GetGraphics().bShowFPS;
					return FMargin(0.f, bFps ? 56.f : 16.f, 16.f, 0.f);
				})
				[
					BuildFeed()
				]
			]
		];
}

TSharedRef<SWidget> SPinguRoot::BuildBanner()
{
	using namespace PinguUIHudPrivate;
	return SNew(SOverlay)
		// The countdown burst: a ring that grows (a shape: it is resized, never scaled, so its edge stays sharp).
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				const APenguinHUD* PenguinHUD = HUD.Get();
				const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
				const bool bCount = GameState && GameState->GetCountdownNumber() >= 0 && !GetBannerText().IsEmpty();
				return bCount ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
			})
			.WidthOverride_Lambda([this]()
			{
				const float T = FMath::Clamp(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BurstTime, 0.f, 1.f);
				return FOptionalSize(FMath::RoundToFloat(140.f + 200.f * PinguUI::EaseOut(T)));
			})
			.HeightOverride_Lambda([this]()
			{
				const float T = FMath::Clamp(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BurstTime, 0.f, 1.f);
				return FOptionalSize(FMath::RoundToFloat(140.f + 200.f * PinguUI::EaseOut(T)));
			})
			[
				SNew(SPinguShape)
				.FillColor(FLinearColor::Transparent)
				.OutlineColor_Lambda([this]()
				{
					const float T = FMath::Clamp(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BurstTime, 0.f, 1.f);
					return PinguUI::WithAlpha(GetBannerColor(), 0.9f * FMath::Square(1.f - T));
				})
				.OutlineWidth_Lambda([this]()
				{
					const float T = FMath::Clamp(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BurstTime, 0.f, 1.f);
					return FMath::Lerp(10.f, 2.f, T);
				})
				.Pill(true)
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			// The round-win banner (kill cam included) stands on a banner plank (style 5, docs/UI_STYLE.md: snow on top, nails,
			// icicles under it); ROUND 1, the countdown, GO! and SUDDEN DEATH! stay plain words. Plank and words move together.
			SNew(SOverlay)
			.RenderTransform_Lambda([this]() { return GetBannerTransform(); })
			+ SOverlay::Slot()
			[
				SNew(SPinguShape).Wood(true).Banner(true)
				.Visibility_Lambda([this]()
				{
					return IsWinBanner() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
				})
				.FillColor_Lambda([this]()
				{
					const float Appear = PinguUI::EaseOut(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BannerInTime);
					return FLinearColor(1.f, 1.f, 1.f, Appear);
				})
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.Padding_Lambda([this]()
				{
					// (Room for the plank's ends and its snow; the capitals centred on the body.)
					return IsWinBanner() ? FMargin(64.f, 30.f, 64.f, 26.f) : FMargin(0.f);
				})
				[
			SAssignNew(BannerText, STextBlock)
			.Text_Lambda([this]() { return GetBannerText(); })
			.Font_Lambda([this]()
			{
				// The countdown numbers big, the win banner at the banner size on its plank, everything else a little smaller
				// (one size each: no font animation).
				const APenguinHUD* PenguinHUD = HUD.Get();
				const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
				const bool bNumber = GameState && GameState->GetPhase() == EPinguPhase::Countdown;
				if (IsWinBanner())
				{
					return PinguWood::OnWoodFont(FPinguStyle::Title(64.f, 5));
				}
				return bNumber ? FPinguStyle::Title(140.f, 7) : FPinguStyle::Title(96.f, 6);
			})
			.ColorAndOpacity_Lambda([this]()
			{
				const float Appear = PinguUI::EaseOut(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BannerInTime);
				return FSlateColor(PinguUI::WithAlpha(GetBannerColor(), Appear));
			})
			.ShadowOffset(FVector2D(0.f, 6.f))
			.ShadowColorAndOpacity_Lambda([this]()
			{
				const float Appear = PinguUI::EaseOut(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BannerInTime);
				return PinguUI::WithAlpha(FPinguStyle::Get().Ink, 0.45f * Appear);
			})
				]
			]
		];
}

bool SPinguRoot::IsWinBanner() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
	return GameState && GameState->GetPhase() == EPinguPhase::RoundOver && PinguWood::GetAtlas() && !GetBannerText().IsEmpty();
}

TSharedRef<SWidget> SPinguRoot::BuildHelp()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	// The keys come from the player's bindings (rebindable), so the help is always right.
	auto KeysOf = [](EPinguInputAction Action, bool bAll) -> FText
	{
		const UPinguSettings* Settings = UPinguSettings::Get();
		if (!Settings)
		{
			return FText::GetEmpty();
		}
		TArray<FString> Labels;
		for (const FKey& Key : Settings->GetKeys(Action))
		{
			Labels.Add(UPinguSettings::GetKeyLabel(Key).ToString());
			if (!bAll)
			{
				break;
			}
		}
		return Labels.Num() > 0 ? FText::FromString(FString::Join(Labels, TEXT(" / "))) : LOCTEXT("Unbound", "-");
	};
	using PinguUIHudPrivate::FHelpLine;
	TArray<FHelpLine> Lines;
	Lines.Add({ LOCTEXT("HelpMove", "Move"), TAttribute<FText>::CreateLambda([KeysOf]()
	{
		return FText::Format(LOCTEXT("HelpMoveKeys", "{0} / {1}"), KeysOf(EPinguInputAction::MoveLeft, false), KeysOf(EPinguInputAction::MoveRight, false));
	}), false });
	Lines.Add({ LOCTEXT("HelpJump", "Jump"), TAttribute<FText>::CreateLambda([KeysOf]() { return KeysOf(EPinguInputAction::Jump, true); }), false });
	Lines.Add({ LOCTEXT("HelpUp", "Aim dash up / down"), TAttribute<FText>::CreateLambda([KeysOf]()
	{
		return FText::Format(LOCTEXT("HelpUpKeys", "{0} / {1}"), KeysOf(EPinguInputAction::Up, false), KeysOf(EPinguInputAction::Down, false));
	}), false });
	Lines.Add({ LOCTEXT("HelpDash", "Dash"), TAttribute<FText>::CreateLambda([KeysOf]() { return KeysOf(EPinguInputAction::Dash, true); }), false });
	Lines.Add({ LOCTEXT("HelpBellySlide", "Belly slide"), TAttribute<FText>::CreateLambda([KeysOf]() { return KeysOf(EPinguInputAction::BellySlide, true); }), false });
	Lines.Add({ LOCTEXT("HelpAim", "Aim"), TAttribute<FText>(LOCTEXT("HelpAimKeys", "Mouse / right stick")), false });
	Lines.Add({ LOCTEXT("HelpAbilities", "Abilities"), TAttribute<FText>::CreateLambda([KeysOf]()
	{
		return FText::Format(LOCTEXT("HelpAbilityKeys", "{0} / {1} / {2}"), KeysOf(EPinguInputAction::Ability1, false),
			KeysOf(EPinguInputAction::Ability2, false), KeysOf(EPinguInputAction::Ability3, false));
	}), false });
	// Holding a shrink / grow ray's key uses it on yourself (APenguinCharacter::SetAbilityKeyDown).
	Lines.Add({ LOCTEXT("HelpSelfRay", "Size ray on you"), TAttribute<FText>(LOCTEXT("HelpSelfRayKeys", "Hold its key")), false });
	Lines.Add({ LOCTEXT("HelpRespawn", "Respawn"), TAttribute<FText>(LOCTEXT("HelpRespawnKey", "R")), true });
	Lines.Add({ LOCTEXT("HelpPause", "Menu"), TAttribute<FText>::CreateLambda([]()
	{
		return PinguUI::MenuKeysText();
	}), false });
	Lines.Add({ LOCTEXT("HelpHelp", "This help"), TAttribute<FText>(LOCTEXT("HelpHelpKey", "H")), false });

	TSharedRef<SVerticalBox> HelpBox = SNew(SVerticalBox);
	HelpBox->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
	[
		SNew(STextBlock).Text(LOCTEXT("HelpTitle", "CONTROLS")).Font(FPinguStyle::Title(28.f, 2)).ColorAndOpacity(Style.Gold)
	];
	for (const FHelpLine& Line : Lines)
	{
		const bool bPracticeOnly = Line.bPracticeOnly;
		HelpBox->AddSlot().AutoHeight().Padding(0.f, 4.f)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this, bPracticeOnly]()
			{
				// Respawning by key: practice and the LAN lobby.
				const EPinguPhase Phase = HUD.IsValid() ? HUD->GetPhase() : EPinguPhase::Menu;
				const bool bShow = !bPracticeOnly || Phase == EPinguPhase::Practice || Phase == EPinguPhase::Lobby;
				return bShow ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
			})
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(208.f)
				[
					SNew(STextBlock).Text(Line.Label).Font(FPinguStyle::Body(18.f)).ColorAndOpacity(Style.Ice)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock).Text(Line.Keys).Font(FPinguStyle::Title(18.f)).ColorAndOpacity(Style.Snow)
			]
		];
	}
	return SNew(SBox)
		.Visibility_Lambda([this]() { return HUD.IsValid() && HUD->IsHelpVisible() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
		[
			MakeCard(HelpBox, FMargin(24.f, 16.f, 32.f, 16.f))
		];
}

TSharedRef<SWidget> SPinguRoot::BuildLobbyPill()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	return SNew(SBox)
		.Visibility_Lambda([this]()
		{
			const bool bShow = HUD.IsValid() && HUD->GetPhase() == EPinguPhase::Lobby && HUD->GetScreen() == EPinguScreen::None;
			return bShow ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
		})
		.HeightOverride(48.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape).Wood(true)
				.FillColor(Style.Panel)
				.OutlineColor(Style.Ink)
				.OutlineWidth(FPinguStyle::CardLineWidth)
				.Pill(true)
			]
			+ SOverlay::Slot()
			.Padding(FMargin(24.f, 0.f))
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("PillLobby", "LOBBY")).Font(FPinguStyle::Title(22.f)).ColorAndOpacity(Style.Gold)
				]
				// The key that brings the lobby back.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([]() { return PinguUI::MenuKeysText(); })
					.Font(FPinguStyle::Title(18.f))
					.ColorAndOpacity(Style.Ice)
				]
			]
		];
}

TSharedRef<SWidget> SPinguRoot::BuildWaitingPill()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	return SNew(SBox)
		.Visibility_Lambda([this]()
		{
			// While this PC's player, who joined during a match, watches it (the lobby pill's place: never in a match).
			const APenguinHUD* PenguinHUD = HUD.Get();
			const EPinguPhase Phase = PenguinHUD ? PenguinHUD->GetPhase() : EPinguPhase::Menu;
			const bool bMatch = Phase == EPinguPhase::Intro || Phase == EPinguPhase::Countdown || Phase == EPinguPhase::Fight || Phase == EPinguPhase::RoundOver;
			const bool bShow = PenguinHUD && bMatch && PenguinHUD->IsLocalWaitingForRound() && PenguinHUD->GetScreen() == EPinguScreen::None;
			return bShow ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
		})
		.HeightOverride(48.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape).Wood(true)
				.FillColor(Style.Panel)
				.OutlineColor(Style.Gold)
				.OutlineWidth(FPinguStyle::CardLineWidth)
				.Pill(true)
			]
			+ SOverlay::Slot()
			.Padding(FMargin(24.f, 0.f))
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("PillWatching", "WATCHING")).Font(FPinguStyle::Title(22.f)).ColorAndOpacity(Style.Gold)
			]
		];
}

TSharedRef<SWidget> SPinguRoot::BuildFPS()
{
	return SNew(SBox)
		.Visibility_Lambda([]()
		{
			const UPinguSettings* Settings = UPinguSettings::Get();
			return Settings && Settings->GetGraphics().bShowFPS ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
		})
		.HeightOverride(32.f)
		.MinDesiredWidth(96.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.f, 0.f))
			[
				SNew(STextBlock)
				.ShadowOffset(FVector2D(0.f, 2.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
				.Text_Lambda([this]() { return FText::Format(LOCTEXT("Fps", "{0} FPS"), FText::AsNumber(FpsShown)); })
				.Font(PinguUIHudPrivate::PlainTextFont(FPinguStyle::Title(16.f, 2)))
				.ColorAndOpacity_Lambda([this]()
				{
					const FPinguStyle& InnerStyle = FPinguStyle::Get();
					return FSlateColor(FpsShown >= 55 ? InnerStyle.Green : (FpsShown >= 30 ? InnerStyle.Gold : InnerStyle.Red));
				})
			]
		];
}

// ---------------------------------------------------------------------------- feed

const FPinguFeedEntry* SPinguRoot::GetFeedLine(int32 Row, float& OutAge) const
{
	using namespace PinguUIHudPrivate;
	OutAge = 0.f;
	const APenguinHUD* PenguinHUD = HUD.Get();
	const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
	if (!GameState)
	{
		return nullptr;
	}
	// The newest lines (the game state keeps a handful, the oldest first).
	const TArray<FPinguFeedEntry>& Feed = GameState->GetFeed();
	const int32 Index = FMath::Max(Feed.Num() - MaxFeedRows, 0) + Row;
	if (!Feed.IsValidIndex(Index) || Feed[Index].Text.IsEmpty())
	{
		return nullptr;
	}
	OutAge = GameState->GetFeedAge(Feed[Index]);
	return OutAge < FeedFadeAt + FeedFadeTime + FeedFoldTime ? &Feed[Index] : nullptr;
}

TSharedRef<SWidget> SPinguRoot::BuildFeed()
{
	using namespace PinguUIHudPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	// One row per line, the newest at the bottom. Rows follow the game state by themselves (no rebuilds).
	TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);
	for (int32 Row = 0; Row < MaxFeedRows; ++Row)
	{
		Lines->AddSlot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		[
			SNew(SBox)
			.Visibility_Lambda([this, Row]()
			{
				float Age = 0.f;
				return GetFeedLine(Row, Age) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
			})
			// An old line folds away (a layout size in whole pixels): the lines under it move up instead of jumping.
			.HeightOverride_Lambda([this, Row]()
			{
				float Age = 0.f;
				GetFeedLine(Row, Age);
				const float Fold = PinguUI::EaseOut((Age - FeedFadeAt - FeedFadeTime) / FeedFoldTime);
				return FOptionalSize(PinguUI::SnapUnits((FeedLineHeight + FeedGap) * (1.f - Fold)));
			})
			.VAlign(VAlign_Top)
			[
				SNew(SBorder)
				.BorderImage(&Style.NoBrush)
				.Padding(0.f)
				.ColorAndOpacity_Lambda([this, Row]()
				{
					float Age = 0.f;
					GetFeedLine(Row, Age);
					const float In = PinguUI::EaseOut(Age / FeedInTime);
					const float Out = 1.f - FMath::Clamp((Age - FeedFadeAt) / FeedFadeTime, 0.f, 1.f);
					return FLinearColor(1.f, 1.f, 1.f, FMath::Min(In, Out));
				})
				// A new line slides in from the right (a translation in whole pixels: the text stays sharp).
				.RenderTransform_Lambda([this, Row]()
				{
					float Age = 0.f;
					GetFeedLine(Row, Age);
					const float In = PinguUI::EaseOut(Age / FeedInTime);
					return TOptional<FSlateRenderTransform>(FSlateRenderTransform(FVector2f(PinguUI::SnapUnits(32.f * (1.f - In)), 0.f)));
				})
				[
					SNew(SBox)
					.HeightOverride(FeedLineHeight)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						.VAlign(VAlign_Center)
						.Padding(FMargin(4.f, 0.f))
						[
							SNew(STextBlock)
							.ShadowOffset(FVector2D(0.f, 2.f))
							.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
							.Text_Lambda([this, Row]()
							{
								float Age = 0.f;
								const FPinguFeedEntry* Line = GetFeedLine(Row, Age);
								return Line ? FText::FromString(Line->Text) : FText::GetEmpty();
							})
							.Font(PinguUIHudPrivate::PlainTextFont(FPinguStyle::Body(18.f, 2)))
							.ColorAndOpacity_Lambda([this, Row]()
							{
								float Age = 0.f;
								const FPinguFeedEntry* Line = GetFeedLine(Row, Age);
								const FLinearColor Color = Line ? Line->Color.CopyWithNewOpacity(1.f) : FLinearColor::White;
								return FSlateColor(FMath::Lerp(Color, FLinearColor::White, 0.3f));
							})
						]
					]
				]
			]
		];
	}
	return Lines;
}

// ---------------------------------------------------------------------------- leaderboard between rounds

float SPinguRoot::GetLeaderboardIn() const
{
	using namespace PinguUIHudPrivate;
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || PenguinHUD->GetPhase() != EPinguPhase::RoundOver)
	{
		return 0.f;
	}
	const float T = (PenguinHUD->GetPhaseTime() - APenguinGameState::LeaderboardDelay) / BoardInTime;
	return T <= 0.f ? 0.f : PinguUI::EaseOut(T);
}

float SPinguRoot::GetFreshWinTime() const
{
	using namespace PinguUIHudPrivate;
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || PenguinHUD->GetPhase() != EPinguPhase::RoundOver)
	{
		return -1.f;
	}
	return PenguinHUD->GetPhaseTime() - APenguinGameState::LeaderboardDelay - FreshWinDelay;
}

float SPinguRoot::GetContinueHintIn() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || !PenguinHUD->IsWaitingForContinueKey())
	{
		return 0.f;
	}
	return FMath::Max(PinguUI::EaseOut((PenguinHUD->GetPhaseTime() - APenguinGameState::ContinueAfter) / 0.25f), 0.01f);
}

int32 SPinguRoot::GetBoardRows() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return 0;
	}
	return PenguinHUD->IsTeamMatch() ? PenguinHUD->GetTeamBoard().Num() : PenguinHUD->GetScores().Num();
}

void SPinguRoot::RebuildLeaderboard()
{
	using namespace PinguUIHudPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!LeaderboardBox.IsValid() || !PenguinHUD)
	{
		return;
	}
	LeaderboardBox->ClearChildren();
	// Between rounds the board shows only the wins (the knock-outs and the rest wait for the results). A team match: a
	// row per team, its penguins' portraits and the team's wins.
	const TArray<FPinguScore>& Scores = PenguinHUD->GetScores();
	const bool bTeams = PenguinHUD->IsTeamMatch();
	const APenguinGameState* GameState = PenguinHUD->GetGameState();
	const TArray<int32> Ranking = bTeams ? PenguinHUD->GetTeamBoard() : (GameState ? GameState->GetWinsBoard() : PenguinHUD->GetLeaderboard());
	const int32 RoundWinner = bTeams ? PenguinHUD->GetRoundWinnerTeam() : PenguinHUD->GetRoundWinnerSlot();
	const FPinguMatchSettings MatchSettings = PenguinHUD->GetMatchSettings();
	const bool bEndless = MatchSettings.bEndless;
	const int32 Rounds = FMath::Clamp(MatchSettings.RoundsToWin, 1, PinguMaxRoundsToWin);
	// Endless, or more wins to go than pips fit: one gold pip and the count.
	const bool bCount = bEndless || Rounds > BoardMaxPips;
	// Up to five rows at full size; ten penguins get compact rows (the board stays on the screen).
	const bool bCompact = Ranking.Num() > BoardFullRows;
	const float RowHeight = bCompact ? 46.f : 68.f;
	const float PortraitSize = bCompact ? 36.f : 52.f;
	const float NameFont = bCompact ? 22.f : 28.f;
	const float PipSize = bCompact ? 15.f : 18.f;
	// Same column widths on every row.
	const float WinsWidth = bEndless ? 80.f : (bCount ? 150.f : static_cast<float>(Rounds) * (PipSize + 6.f));
	// The name part too: in a team match a team's row (its name, its penguins' portraits) and the row of a penguin on its
	// own get the same width - the biggest team decides - so the wins line up.
	int32 MostMembers = 1;
	if (bTeams)
	{
		for (const int32 Team : Ranking)
		{
			MostMembers = FMath::Max(MostMembers, Scores.FilterByPredicate([Team](const FPinguScore& Score) { return Score.Team == Team; }).Num());
		}
	}
	const float MemberStep = PortraitSize - 10.f + 9.f;
	const float TeamLabelWidth = bCompact ? 196.f : 236.f;
	const float PersonNameWidth = bCompact ? 210.f : 232.f;
	const float YouTagRoom = PenguinHUD->IsNetworkGame() ? 72.f : 0.f;
	const float PersonWhoWidth = PortraitSize + 16.f + PersonNameWidth;
	const float WhoWidth = bTeams ? FMath::Max(TeamLabelWidth + 8.f + static_cast<float>(MostMembers) * MemberStep + YouTagRoom, PersonWhoWidth)
		: PersonWhoWidth;

	// Wins: a pip per round to win (gold = won); endless or first to more than 10: one gold pip and the count ("7", or
	// "7 / 20"). The round winner's new win fills in a moment after the board shows: the pip grows and shrinks back (a
	// shape resized in a fixed box, so neither its edge nor the row moves); the count text just changes.
	const auto MakeWins = [this, &Style, bEndless, bCount, Rounds, PipSize, NameFont](int32 Wins, bool bWinner) -> TSharedRef<SWidget>
	{
		TSharedRef<SHorizontalBox> WinsBox = SNew(SHorizontalBox);
		if (bCount)
		{
			WinsBox->AddSlot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(PipSize + 4.f).HeightOverride(PipSize + 4.f)
				[
					SNew(SPinguShape).FillColor(Style.Gold).OutlineColor(Style.Ink).OutlineWidth(FPinguStyle::LineWidth).Pill(true)
				]
			];
			WinsBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text_Lambda([this, Wins, bWinner, bEndless, Rounds]()
				{
					const int32 Shown = bWinner && GetFreshWinTime() < 0.f ? FMath::Max(Wins - 1, 0) : Wins;
					return bEndless ? FText::AsNumber(Shown) : FText::Format(LOCTEXT("BoardWinsOf", "{0} / {1}"), FText::AsNumber(Shown), FText::AsNumber(Rounds));
				})
				.Font(FPinguStyle::Title(NameFont, 2))
				.ColorAndOpacity(Style.Gold)
			];
			return WinsBox;
		}
		for (int32 Pip = 0; Pip < Rounds; ++Pip)
		{
			const bool bFresh = bWinner && Pip == Wins - 1;
			const bool bWon = Pip < Wins;
			WinsBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(3.f, 0.f)
			[
				SNew(SBox).WidthOverride(PipSize).HeightOverride(PipSize).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBox)
					.WidthOverride_Lambda([this, bFresh, PipSize]()
					{
						const float Pop = bFresh ? FMath::Sin(UE_PI * FMath::Clamp(GetFreshWinTime() / FreshWinPopTime, 0.f, 1.f)) : 0.f;
						return FOptionalSize(FMath::RoundToFloat(PipSize + 10.f * Pop));
					})
					.HeightOverride_Lambda([this, bFresh, PipSize]()
					{
						const float Pop = bFresh ? FMath::Sin(UE_PI * FMath::Clamp(GetFreshWinTime() / FreshWinPopTime, 0.f, 1.f)) : 0.f;
						return FOptionalSize(FMath::RoundToFloat(PipSize + 10.f * Pop));
					})
					[
						SNew(SPinguShape)
						.FillColor_Lambda([this, bFresh, bWon]()
						{
							const bool bLit = bFresh ? GetFreshWinTime() >= 0.f : bWon;
							return bLit ? FPinguStyle::Get().Gold : PipOffColor();
						})
						.OutlineColor(Style.Ink)
						.OutlineWidth(FPinguStyle::LineWidth)
						.Pill(true)
					]
				]
			];
		}
		return WinsBox;
	};

	for (int32 Rank = 0; Rank < Ranking.Num(); ++Rank)
	{
		TSharedPtr<SWidget> Who;
		int32 Wins = 0;
		bool bWinner = false;
		// A team match: a row per team - or, for a penguin on its own (a team of one), its own row like any other match's.
		int32 Solo = INDEX_NONE;
		if (bTeams)
		{
			int32 Count = 0;
			for (int32 Index = 0; Index < Scores.Num(); ++Index)
			{
				if (Scores[Index].Team == Ranking[Rank])
				{
					Solo = Count == 0 ? Index : INDEX_NONE;
					++Count;
				}
			}
		}
		if (bTeams && Solo == INDEX_NONE)
		{
			// The team: its name in its colour, its penguins' portraits (a YOU tag where this PC's player is in it).
			const int32 Team = Ranking[Rank];
			Wins = PenguinHUD->GetTeamWinCount(Team);
			bWinner = Team == RoundWinner;
			TSharedRef<SHorizontalBox> Members = SNew(SHorizontalBox);
			bool bYouHere = false;
			int32 Count = 0;
			for (int32 Index = 0; Index < Scores.Num(); ++Index)
			{
				if (Scores[Index].Team != Team)
				{
					continue;
				}
				bYouHere |= PenguinHUD->IsLocalScore(Scores[Index]);
				Members->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(Count > 0 ? 9.f : 0.f, 0.f, 0.f, 0.f)
				[
					MakePortrait(Scores[Index].Slot, PortraitSize - 10.f, Team)
				];
				++Count;
			}
			Who = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					// (Room for the longest: "PURPLE TEAM".)
					SNew(SBox).WidthOverride(TeamLabelWidth)
					[
						SNew(STextBlock)
						.Text(FText::Format(LOCTEXT("BoardTeam", "{0} TEAM"), APenguinCharacter::GetTeamName(Team)))
						.Font(FPinguStyle::Title(NameFont, 2))
						.ColorAndOpacity(FMath::Lerp(APenguinCharacter::GetTeamColor(Team), FLinearColor::White, 0.2f))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SBox).MinDesiredWidth(static_cast<float>(MostMembers) * MemberStep)
					[
						Members
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SBox)
					.Visibility(bYouHere && PenguinHUD->IsNetworkGame() ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
					[
						MakeTag(LOCTEXT("TagYouTeam", "YOU"), Style.Gold, Style.Ink)
					]
				];
		}
		else
		{
			const int32 Index = bTeams ? Solo : Ranking[Rank];
			if (!Scores.IsValidIndex(Index))
			{
				continue;
			}
			const FPinguScore& Score = Scores[Index];
			const FLinearColor Color = APenguinCharacter::GetSlotColor(Score.Slot);
			Wins = bTeams ? PenguinHUD->GetTeamWinCount(Score.Team) : Score.Wins;
			bWinner = bTeams ? Score.Team == RoundWinner : Score.Slot == RoundWinner;
			Who = SNew(SHorizontalBox)
				// portrait in a circle of the player's colour
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakePortrait(Score.Slot, PortraitSize)
				]
				// name (+ a YOU tag in LAN games)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16.f, 0.f, 0.f, 0.f)
				[
					SNew(SBox).WidthOverride(PersonNameWidth)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock).Text(PinguUI::Upper(Score.Name)).Font(FPinguStyle::Title(NameFont, 2)).ColorAndOpacity(FMath::Lerp(Color, FLinearColor::White, 0.25f))
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
						[
							SNew(SBox)
							.Visibility(PenguinHUD->IsNetworkGame() && PenguinHUD->IsLocalScore(Score) ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
							[
								MakeTag(LOCTEXT("TagYou", "YOU"), Style.Gold, Style.Ink)
							]
						]
					]
				];
		}

		const float RowDelay = static_cast<float>(Rank) * BoardRowDelay;
		LeaderboardBox->AddSlot()
		.AutoHeight()
		.Padding(0.f, bCompact ? 2.f : 4.f)
		[
			// Each row comes in a moment after the one above it (opacity + a few whole pixels).
			SNew(SBorder)
			.BorderImage(&Style.NoBrush)
			.Padding(0.f)
			.ColorAndOpacity_Lambda([this, RowDelay]()
			{
				const APenguinHUD* InnerHUD = HUD.Get();
				const float T = InnerHUD ? (InnerHUD->GetPhaseTime() - APenguinGameState::LeaderboardDelay - RowDelay) / 0.16f : 1.f;
				return FLinearColor(1.f, 1.f, 1.f, PinguUI::EaseOut(T) * (T > 0.f ? 1.f : 0.f));
			})
			.RenderTransform_Lambda([this, RowDelay]()
			{
				const APenguinHUD* InnerHUD = HUD.Get();
				const float T = InnerHUD ? (InnerHUD->GetPhaseTime() - APenguinGameState::LeaderboardDelay - RowDelay) / 0.16f : 1.f;
				return TOptional<FSlateRenderTransform>(FSlateRenderTransform(FVector2f(0.f, PinguUI::SnapUnits(12.f * (1.f - PinguUI::EaseOut(T))))));
			})
			[
				SNew(SBox)
				.HeightOverride(RowHeight)
				[
					SNew(SOverlay)
					// The round winner's row: a gold rim (no scaling).
					+ SOverlay::Slot()
					[
						SNew(SPinguShape)
						.FillColor(Style.Row)
						.OutlineColor(bWinner ? Style.Gold : Style.Ink)
						.OutlineWidth(bWinner ? FPinguStyle::RingWidth : FPinguStyle::LineWidth)
						.Radius(FPinguStyle::RowRadius + 4.f)
					]
					+ SOverlay::Slot()
					.Padding(FMargin(8.f, 0.f, 20.f, 0.f))
					[
						SNew(SHorizontalBox)
						// rank
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SBox).WidthOverride(40.f).HAlign(HAlign_Center)
							[
								SNew(STextBlock).Text(FText::AsNumber(Rank + 1)).Font(FPinguStyle::Title(bCompact ? 24.f : 30.f, 2)).ColorAndOpacity(Rank == 0 ? Style.Gold : Style.Ice)
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
						[
							SNew(SBox).WidthOverride(WhoWidth).HAlign(HAlign_Left)
							[
								Who.ToSharedRef()
							]
						]
						// round wins (only the wins: the rest of the statistics come with the results)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
						[
							SNew(SBox).MinDesiredWidth(WinsWidth).HAlign(HAlign_Left)
							[
								MakeWins(Wins, bWinner)
							]
						]
					]
				]
			]
		];
	}
}

// ---------------------------------------------------------------------------- banner

FText SPinguRoot::GetBannerText() const
{
	using namespace PinguUIHudPrivate;
	const APenguinHUD* PenguinHUD = HUD.Get();
	const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
	if (!GameState)
	{
		return FText::GetEmpty();
	}
	switch (GameState->GetPhase())
	{
	case EPinguPhase::Intro:
		// Traits mode: the banner makes room for the trait cards after a moment (the intro is longer then).
		if (GameState->AreTraitsOn() && GameState->GetPhaseTime() >= APenguinGameState::TraitBannerTime)
		{
			return FText::GetEmpty();
		}
		return FText::Format(LOCTEXT("Round", "ROUND {0}"), FText::AsNumber(GameState->GetRoundNumber()));
	case EPinguPhase::Countdown:
	{
		const int32 Number = GameState->GetCountdownNumber();
		return Number > 0 ? FText::AsNumber(Number) : FText::GetEmpty();
	}
	case EPinguPhase::Fight:
	{
		if (GameState->GetCountdownNumber() == 0)
		{
			return LOCTEXT("Go", "GO!");
		}
		// Timed from the moment this machine sees the island start to crumble (its start time is on the game's flow
		// clock: the server time in LAN games, a clock of its own in solo games).
		const float CollapseStart = GameState->GetCollapseStartTime();
		if (CollapseStart != SeenCollapseStart)
		{
			SeenCollapseStart = CollapseStart;
			SuddenDeathSeenAt = CollapseStart >= 0.f ? PinguUI::Now() : -100.0;
		}
		if (CollapseStart >= 0.f && PinguUI::Now() - SuddenDeathSeenAt < SuddenDeathBanner)
		{
			return LOCTEXT("SuddenDeath", "SUDDEN DEATH!");
		}
		return FText::GetEmpty();
	}
	case EPinguPhase::RoundOver:
	{
		if (PenguinHUD->IsTeamMatch())
		{
			// A team match: the team left standing.
			const int32 Team = PenguinHUD->GetRoundWinnerTeam();
			if (Team < 0)
			{
				return LOCTEXT("Draw", "DRAW!");
			}
			// Somebody on their own (a team of one): by name, like in any other match.
			if (const FPinguScore* Lone = PenguinHUD->GetLoneTeamMember(Team))
			{
				return PenguinHUD->IsLocalScore(*Lone) ? LOCTEXT("YouWinRound", "YOU WIN THE ROUND!") : FText::Format(LOCTEXT("WinsRound", "{0} WINS!"), PinguUI::Upper(Lone->Name));
			}
			if (Team == GameState->GetLocalTeam())
			{
				return LOCTEXT("YourTeamWinsRound", "YOUR TEAM WINS THE ROUND!");
			}
			return FText::Format(LOCTEXT("TeamWinsRound", "{0} TEAM WINS!"), APenguinCharacter::GetTeamName(Team));
		}
		const FPinguScore* Winner = PenguinHUD->FindScoreBySlot(PenguinHUD->GetRoundWinnerSlot());
		if (!Winner)
		{
			return LOCTEXT("Draw", "DRAW!");
		}
		if (PenguinHUD->IsLocalScore(*Winner))
		{
			return LOCTEXT("YouWinRound", "YOU WIN THE ROUND!");
		}
		return FText::Format(LOCTEXT("WinsRound", "{0} WINS!"), PinguUI::Upper(Winner->Name));
	}
	default:
		return FText::GetEmpty();
	}
}

double SPinguRoot::GetBannerChangedAt() const
{
	const FString Banner = GetBannerText().ToString();
	if (Banner != LastBanner)
	{
		LastBanner = Banner;
		BannerChangedAt = PinguUI::Now();
	}
	return BannerChangedAt;
}

FLinearColor SPinguRoot::GetBannerColor() const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	const APenguinHUD* PenguinHUD = HUD.Get();
	const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
	if (!GameState)
	{
		return Style.Snow;
	}
	switch (GameState->GetPhase())
	{
	case EPinguPhase::Countdown: return Style.Gold;
	case EPinguPhase::Fight: return GameState->GetCountdownNumber() == 0 ? Style.Orange : Style.Red;
	case EPinguPhase::RoundOver:
	{
		if (PenguinHUD->IsTeamMatch())
		{
			const int32 Team = PenguinHUD->GetRoundWinnerTeam();
			if (Team < 0)
			{
				return Style.Ice;
			}
			if (const FPinguScore* Lone = PenguinHUD->GetLoneTeamMember(Team))
			{
				return PenguinHUD->IsLocalScore(*Lone) ? Style.Gold : FMath::Lerp(APenguinCharacter::GetSlotColor(Lone->Slot), FLinearColor::White, 0.2f);
			}
			return Team == GameState->GetLocalTeam() ? Style.Gold : FMath::Lerp(APenguinCharacter::GetTeamColor(Team), FLinearColor::White, 0.2f);
		}
		const FPinguScore* Winner = PenguinHUD->FindScoreBySlot(PenguinHUD->GetRoundWinnerSlot());
		if (!Winner)
		{
			return Style.Ice;
		}
		return PenguinHUD->IsLocalScore(*Winner) ? Style.Gold : FMath::Lerp(APenguinCharacter::GetSlotColor(Winner->Slot), FLinearColor::White, 0.2f);
	}
	default: return Style.Snow;
	}
}

TOptional<FSlateRenderTransform> SPinguRoot::GetBannerTransform() const
{
	using namespace PinguUIHudPrivate;
	// A new text drops in from a little above; between rounds the banner moves up to make room for the leaderboard.
	// Translations only, rounded to whole pixels: the glyphs stay sharp.
	const float Appear = PinguUI::EaseOut(static_cast<float>(PinguUI::Now() - GetBannerChangedAt()) / BannerInTime);
	float Lift = 0.f;
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (PenguinHUD && PenguinHUD->GetPhase() == EPinguPhase::RoundOver)
	{
		constexpr float Delay = APenguinGameState::LeaderboardDelay;
		Lift = FMath::SmoothStep(Delay - 0.1f, Delay + 0.15f, PenguinHUD->GetPhaseTime());
	}
	// A taller board (compact rows for more than five penguins) pushes it further up.
	const float RowsLift = BannerLiftPerRow * static_cast<float>(FMath::Max(GetBoardRows() - BoardFullRows, 0));
	// The winner banner sits in the top third (its middle a fifth of the way down; card 2026-11-07_kill-cam): the kill
	// cam's knock-out below it stays in view. (It starts 70 units above the middle: the slot's bottom padding.)
	float WinLift = 0.f;
	if (PenguinHUD && PenguinHUD->GetPhase() == EPinguPhase::RoundOver)
	{
		const float Height = static_cast<float>(GetTickSpaceGeometry().GetLocalSize().Y);
		WinLift = FMath::Max(0.f, Height * 0.3f - 70.f);
	}
	const float OffsetY = -BannerDrop * (1.f - Appear) - FMath::Max(WinLift, (BannerLift + RowsLift) * Lift);
	return FSlateRenderTransform(FVector2f(0.f, PinguUI::SnapUnits(OffsetY)));
}

#undef LOCTEXT_NAMESPACE

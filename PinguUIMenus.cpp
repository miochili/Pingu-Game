// Pingu - the menu screens: main menu, LAN (host / find / join), lobby, pause, results, the error popup and the
// "press a key" card. They read the replicated game state, the LAN subsystem and the settings through the HUD and
// call its Action_* functions (the HUD calls the game mode only where this machine has the authority).

#include "PinguUI.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PinguMenu3D.h"
#include "PinguMenu3DOverlay.h"
#include "PinguNetSubsystem.h"
#include "PinguSettings.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PinguUI"

namespace PinguUIMenusPrivate
{
	static FText DifficultyName(int32 Difficulty)
	{
		switch (FMath::Clamp(Difficulty, 0, 2))
		{
		case 0: return LOCTEXT("Easy", "EASY");
		case 2: return LOCTEXT("Hard", "HARD");
		default: return LOCTEXT("Normal", "NORMAL");
		}
	}

	/** "Waiting..." with 0-3 dots (only the end of the text moves). */
	static FString Dots()
	{
		static const TCHAR* Steps[] = { TEXT(""), TEXT("."), TEXT(".."), TEXT("...") };
		return Steps[static_cast<int32>(PinguUI::Now() * 3.0) % 4];
	}

	static EVisibility ShowIf(bool bShow)
	{
		return bShow ? EVisibility::Visible : EVisibility::Collapsed;
	}

	/** The results' statistic columns (WINS, KOS, FALLS, HITS). */
	constexpr float StatWidth = 80.f;

	/** The results card's width. */
	constexpr float ResultsWidth = 880.f;

	/** How wide a text is in a font (Slate units, its outline included); 0 before Slate can measure. */
	static float MeasureText(const FText& Text, const FSlateFontInfo& Font)
	{
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer() || Text.IsEmpty())
		{
			return 0.f;
		}
		return static_cast<float>(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font).X) + 2.f * Font.OutlineSettings.OutlineSize;
	}

	/** A title font that fits Width (Slate units): Size, or a smaller size for a long text (never under MinSize). */
	static FSlateFontInfo FitTitleFont(const FText& Text, float Size, int32 Outline, float Width, float MinSize = 36.f, float* OutSize = nullptr)
	{
		// (Sizes are the layout's: FPinguStyle::Title draws Titan One smaller than that; OutSize gets the layout size chosen.)
		FSlateFontInfo Font = FPinguStyle::Title(Size, Outline);
		float Chosen = Size;
		const float Measured = MeasureText(Text, Font);
		if (Measured > Width && Measured > 1.f)
		{
			Chosen = FMath::Max(FMath::FloorToFloat(Size * Width / Measured), MinSize);
			Font = FPinguStyle::Title(Chosen, Outline);
		}
		if (OutSize)
		{
			*OutSize = Chosen;
		}
		return Font;
	}

	/** The title menu's buttons under PLAY: their height, label size, and the room a label has (the card's 416 units less
	 *  the button's 16 + 16 label padding). */
	constexpr float MenuButtonHeight = 60.f;
	constexpr float MenuButtonFont = 28.f;
	constexpr float MenuLabelRoom = 384.f;
	/** A menu label is never smaller than this. */
	constexpr float MenuLabelMinFont = 22.f;

	/** The lobby's cards: five to a row, two rows (ten penguins). */
	constexpr int32 LobbyCardsPerRow = 5;
	constexpr float LobbyCardWidth = 156.f;
	constexpr float LobbyCardHeight = 240.f;
	constexpr float LobbyCardGap = 12.f;
	constexpr float LobbyPortraitSize = 84.f;
	/** The lobby's bottom row: SETTINGS | BACK (LEAVE / CLOSE GAME) ... TRAITS | START, in the cards' width (828). The small
	 *  buttons keep 168 units for their label (CLOSE GAME and TRAITS OFF need about 160). */
	constexpr float LobbyButtonWidth = 184.f;
	constexpr float LobbyStartWidth = 240.f;
	static const FMargin LobbyButtonPadding(8.f, 0.f);
	/** The host's address line (a button that copies the address): its height, and its copy icon (two pages, the second this
	 *  far down and right of the first). */
	constexpr float LobbyAddressHeight = 36.f;
	constexpr float LobbyCopyIconSize = 20.f;
	constexpr float LobbyCopyIconShift = 6.f;

	/** What the lobby's TRAITS button says: ON (orange) or OFF. */
	static FText TraitsLabel(bool bOn)
	{
		return bOn ? LOCTEXT("LobbyTraitsOn", "TRAITS ON") : LOCTEXT("LobbyTraitsOff", "TRAITS OFF");
	}
}

// ============================================================================ main menu

TSharedRef<SWidget> SPinguRoot::BuildMainMenu()
{
	using namespace PinguUIMenusPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedPtr<SPinguButton> PlayButton;

	// The 3D title (PinguMenu3D): the screen is an invisible layer that hands the mouse and the keys to the 3D menu in the
	// world, which shows the logo and the buttons itself. -PinguOldMenus: the flat card below.
	if (PinguMenu3D::IsEnabled())
	{
		APinguMenu3D::Get(HUD.Get()); // made now: its pages are built before the title first shows
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		FirstFocus.Add(static_cast<uint8>(EPinguScreen::MainMenu), Input);
		return MakeScreen(EPinguScreen::MainMenu, Input, false);
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		// The logo: two title lines tucked together.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("TitlePingu", "PINGU"))
			.Font(FPinguStyle::Title(104.f, 6))
			.ColorAndOpacity(Style.Snow)
			.ShadowOffset(FVector2D(0.f, 6.f))
			.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.5f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.f, -24.f, 0.f, 0.f) // (the two lines tucked together: Titan One's line is a little shorter than Lilita One's)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("TitleBrawl", "BRAWL"))
			.Font(FPinguStyle::Title(72.f, 5))
			.ColorAndOpacity(Style.Orange)
			.ShadowOffset(FVector2D(0.f, 5.f))
			.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.5f))
		]
		// PLAY: a match against bots (in a lobby of your own), ONLINE: with others on the LAN, SETTINGS (the controls too),
		// PINGUPEDIA: every ability and trait with its numbers.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SAssignNew(PlayButton, SPinguButton)
			.Kind(EPinguButtonKind::Primary)
			.Height(72.f)
			.FontSize(34.f)
			.Text(LOCTEXT("Play", "PLAY"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Play(); } })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Height(MenuButtonHeight)
			.FontSize(MenuButtonFont)
			.Text(LOCTEXT("Online", "ONLINE"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Lan(); } })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Height(MenuButtonHeight)
			.FontSize(MenuButtonFont)
			.Text(LOCTEXT("Settings", "SETTINGS"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Settings(); } })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			// The longest label of the menu: measured, and a size smaller should it ever not fit (the menu screenshots
			// log its width).
			SNew(SPinguButton)
			.Height(MenuButtonHeight)
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Pingupedia(); } })
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Pingupedia", "PINGUPEDIA"))
				.Font_Lambda([]()
				{
					const FText Label = LOCTEXT("Pingupedia", "PINGUPEDIA");
					float LayoutSize = MenuButtonFont;
					const FSlateFontInfo Font = FitTitleFont(Label, MenuButtonFont, 0, MenuLabelRoom, MenuLabelMinFont, &LayoutSize);
					PinguUI::FPingupediaStats& PediaStats = PinguUI::MutablePingupediaStats();
					PediaStats.LabelWidth = MeasureText(Label, Font);
					PediaStats.LabelRoom = MenuLabelRoom;
					PediaStats.LabelFont = LayoutSize;
					return Font;
				})
				.ColorAndOpacity(Style.Snow)
				.ShadowOffset(FVector2D(0.f, 2.f))
				.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.85f))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Kind(EPinguButtonKind::Ghost)
			.Height(FPinguStyle::SmallHeight)
			.FontSize(20.f)
			.Text(LOCTEXT("Quit", "QUIT"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Quit(); } })
		];
	FirstFocus.Add(static_cast<uint8>(EPinguScreen::MainMenu), PlayButton);

	return MakeScreen(EPinguScreen::MainMenu,
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Center)
		.Padding(56.f, 32.f)
		[
			MakeCard(Box, FMargin(32.f, 16.f, 32.f, 32.f), 480.f)
		],
		false);
}

// ============================================================================ LAN

TSharedRef<SWidget> SPinguRoot::BuildLan()
{
	using namespace PinguUIMenusPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedPtr<SPinguButton> HostButton;

	// The 3D ONLINE page (PinguMenu3D): the screen is the invisible input layer; the 3D menu shows the fields, the found games
	// and the buttons itself (-PinguOldMenus: the flat card below).
	if (PinguMenu3D::IsEnabled())
	{
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		FirstFocus.Add(static_cast<uint8>(EPinguScreen::Lan), Input);
		return MakeScreen(EPinguScreen::Lan, Input, false);
	}

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeTitle(LOCTEXT("LanTitle", "ONLINE"))
		]
		// Your name.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(192.f)
				[
					MakeSection(LOCTEXT("YourName", "YOUR NAME"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				MakeTextField(NameBox, TAttribute<FText>::CreateLambda([this]() { return FText::FromString(HUD.IsValid() ? HUD->GetPlayerName() : FString()); }),
					LOCTEXT("NameHint", "Your name"), 0.f, true)
			]
		]
		// Host.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SAssignNew(HostButton, SPinguButton)
			.Kind(EPinguButtonKind::Primary)
			.Text(LOCTEXT("HostGame", "HOST GAME"))
			.HUD(HUD)
			.IsEnabled_Lambda([this]() { return HUD.IsValid() && !HUD->IsConnecting(); })
			.OnClicked_Lambda([this]()
			{
				CommitTextBoxes();
				if (HUD.IsValid())
				{
					HUD->Action_HostGame();
				}
			})
		]
		// Found games.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 32.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				MakeSection(LOCTEXT("FoundGames", "GAMES ON YOUR NETWORK"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(120.f)
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return HUD.IsValid() && HUD->IsSearching() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					.Text_Lambda([]() { return FText::Format(LOCTEXT("Searching", "SEARCHING{0}"), FText::FromString(Dots())); })
					.Font(FPinguStyle::Title(16.f))
					.ColorAndOpacity(Style.Ice)
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SAssignNew(FoundGamesBox, SVerticalBox)
		]
		// Join by address.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			MakeSection(LOCTEXT("JoinByAddress", "JOIN BY ADDRESS"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				MakeTextField(AddressBox, TAttribute<FText>(FText::GetEmpty()), LOCTEXT("AddressHint", "192.168.1.20"), 0.f, false)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SPinguButton)
				.Width(160.f)
				.Height(FPinguStyle::RowHeight)
				.Text(LOCTEXT("Join", "JOIN"))
				.HUD(HUD)
				.IsEnabled_Lambda([this]() { return HUD.IsValid() && !HUD->IsConnecting(); })
				.OnClicked_Lambda([this]()
				{
					CommitTextBoxes();
					if (HUD.IsValid() && AddressBox.IsValid())
					{
						HUD->Action_JoinGame(AddressBox->GetText().ToString());
					}
				})
			]
		]
		// Status: joining.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Visibility_Lambda([this]() { return HUD.IsValid() && HUD->IsConnecting() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			.Text_Lambda([this]()
			{
				const FString Target = HUD.IsValid() ? HUD->GetJoinTarget() : FString();
				return FText::Format(LOCTEXT("Connecting", "Connecting to {0}{1}"), FText::FromString(Target), FText::FromString(Dots()));
			})
			.Font(FPinguStyle::Body(20.f))
			.ColorAndOpacity(Style.Gold)
		]
		// Back (cancels a join under way).
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		.HAlign(HAlign_Left)
		[
			SAssignNew(LanBack, SPinguButton)
			.Kind(EPinguButtonKind::Ghost)
			.Width(200.f)
			.Height(FPinguStyle::SmallHeight)
			.FontSize(20.f)
			.Text_Lambda([this]() { return HUD.IsValid() && HUD->IsConnecting() ? LOCTEXT("CancelJoin", "CANCEL") : LOCTEXT("LanBack", "BACK"); })
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Back(); } })
		];
	FirstFocus.Add(static_cast<uint8>(EPinguScreen::Lan), HostButton);

	return MakeScreen(EPinguScreen::Lan,
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MakeCard(Box, FMargin(40.f, 24.f, 40.f, 32.f), 760.f)
		]);
}

void SPinguRoot::RebuildFoundGames()
{
	using namespace PinguUIMenusPrivate;
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || !FoundGamesBox.IsValid())
	{
		return;
	}
	// Rebuilt only when games come or go; the players / in-match texts follow the beacon by themselves.
	const TArray<FPinguLanGame>& Games = PenguinHUD->GetFoundGames();
	constexpr int32 MaxShown = 4;
	FString Key = PenguinHUD->IsSearching() ? TEXT("s") : TEXT("-");
	// The total is part of the key: the "+ N more" text is built once, so it must rebuild when the count changes.
	Key += FString::Printf(TEXT("#%d"), Games.Num());
	for (int32 Index = 0; Index < FMath::Min(Games.Num(), MaxShown); ++Index)
	{
		Key += TEXT("|") + Games[Index].Address;
	}
	if (Key == BuiltGamesKey)
	{
		return;
	}
	BuiltGamesKey = Key;
	FoundGamesBox->ClearChildren();
	const FPinguStyle& Style = FPinguStyle::Get();

	if (Games.Num() == 0)
	{
		FoundGamesBox->AddSlot()
		.AutoHeight()
		[
			SNew(SBox)
			.HeightOverride(56.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SPinguShape)
					.FillColor(PinguUI::WithAlpha(Style.PanelDeep, 0.6f))
					.OutlineColor(PinguUI::WithAlpha(Style.IceDim, 0.35f))
					.OutlineWidth(FPinguStyle::LineWidth)
					.Radius(FPinguStyle::RowRadius)
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return HUD.IsValid() && HUD->IsSearching()
							? LOCTEXT("NoGamesYet", "No games yet")
							: LOCTEXT("NotSearching", "Not searching");
					})
					.Font(FPinguStyle::Body(16.f))
					.ColorAndOpacity(Style.IceDim)
				]
			]
		];
		return;
	}

	for (int32 Index = 0; Index < FMath::Min(Games.Num(), MaxShown); ++Index)
	{
		const FString Address = Games[Index].Address;
		// The live entry of this game (by address), or null once it is gone.
		auto FindGame = [this, Address]() -> const FPinguLanGame*
		{
			const APenguinHUD* InnerHUD = HUD.Get();
			return InnerHUD ? InnerHUD->GetFoundGames().FindByPredicate([&Address](const FPinguLanGame& Game) { return Game.Address == Address; }) : nullptr;
		};
		FoundGamesBox->AddSlot()
		.AutoHeight()
		.Padding(0.f, Index > 0 ? 8.f : 0.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Kind(EPinguButtonKind::Row)
			.Height(56.f)
			.HAlign(HAlign_Fill)
			.ContentPadding(FMargin(16.f, 0.f))
			.HUD(HUD)
			.IsEnabled_Lambda([this, FindGame]()
			{
				// A running match is fine (you play from its next round); a full game is not.
				const FPinguLanGame* Game = FindGame();
				return Game && Game->Players < Game->MaxPlayers && HUD.IsValid() && !HUD->IsConnecting();
			})
			.OnClicked_Lambda([this, Address]()
			{
				CommitTextBoxes();
				if (HUD.IsValid())
				{
					HUD->Action_JoinFound(Address);
				}
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([FindGame]()
					{
						const FPinguLanGame* Game = FindGame();
						return FText::Format(LOCTEXT("GameOf", "{0}'S GAME"), PinguUI::Upper(Game ? Game->HostName : FString()));
					})
					.Font(FPinguStyle::Title(22.f))
					.ColorAndOpacity(Style.Snow)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([FindGame]()
					{
						const FPinguLanGame* Game = FindGame();
						return FText::Format(LOCTEXT("GamePlayers", "{0}/{1} PLAYERS"), FText::AsNumber(Game ? Game->Players : 0), FText::AsNumber(Game ? Game->MaxPlayers : PinguMaxPenguins));
					})
					.Font(FPinguStyle::Title(16.f))
					.ColorAndOpacity(Style.Ice)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(208.f)
					.HAlign(HAlign_Right)
					[
						SNew(SOverlay)
						// Full: nobody gets in.
						+ SOverlay::Slot()
						.HAlign(HAlign_Right)
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.Visibility_Lambda([FindGame]()
							{
								const FPinguLanGame* Game = FindGame();
								return ShowIf(Game && Game->Players >= Game->MaxPlayers);
							})
							[
								MakeTag(LOCTEXT("Full", "FULL"), Style.Red, Style.Snow)
							]
						]
						// Open: JOIN (a running match says so: you are in from its next round).
						+ SOverlay::Slot()
						.HAlign(HAlign_Right)
						.VAlign(VAlign_Center)
						[
							SNew(SHorizontalBox)
							.Visibility_Lambda([FindGame]()
							{
								const FPinguLanGame* Game = FindGame();
								return ShowIf(Game && Game->Players < Game->MaxPlayers);
							})
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
							[
								SNew(SBox)
								.Visibility_Lambda([FindGame]()
								{
									const FPinguLanGame* Game = FindGame();
									return ShowIf(Game && Game->bInMatch);
								})
								[
									MakeTag(LOCTEXT("InMatch", "IN MATCH"), Style.Ice, Style.Ink)
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(STextBlock).Text(LOCTEXT("JoinShort", "JOIN")).Font(FPinguStyle::Title(20.f)).ColorAndOpacity(Style.Gold)
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
							[
								SNew(SPinguArrow).Direction(1).Size(14.f).Color(Style.Gold)
							]
						]
					]
				]
			]
		];
	}
	if (Games.Num() > MaxShown)
	{
		FoundGamesBox->AddSlot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("MoreGames", "+ {0} more"), FText::AsNumber(Games.Num() - MaxShown)))
			.Font(FPinguStyle::Body(16.f))
			.ColorAndOpacity(Style.IceDim)
		];
	}
}

// ============================================================================ lobby

TSharedRef<SWidget> SPinguRoot::BuildLobby()
{
	using namespace PinguUIMenusPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	auto HostOnly = [this]() { return ShowIf(HUD.IsValid() && HUD->HasMatchAuthority()); };
	auto ClientOnly = [this]() { return ShowIf(HUD.IsValid() && !HUD->HasMatchAuthority()); };
	auto SoloOnly = [this]() { return ShowIf(HUD.IsValid() && !HUD->IsNetworkGame()); };
	auto LanOnly = [this]() { return ShowIf(HUD.IsValid() && HUD->IsNetworkGame()); };
	const float GridWidth = LobbyCardsPerRow * LobbyCardWidth + (LobbyCardsPerRow - 1) * LobbyCardGap;

	// The 3D lobby (PinguMenu3D, with the lobby island): its controls (SETTINGS, BACK / LEAVE / CLOSE GAME, TRAITS, START or
	// "Waiting for ...", the host's address line) are 3D buttons on a strip at the bottom of the picture, the penguins stand on the
	// island. The lobby island is no menu (the penguins run around, the mouse goes to the game), so this screen is empty and lets
	// every click through: APinguMenu3D reads the mouse from the player controller itself, as APinguLobby does.
	if (PinguMenu3D::IsEnabled())
	{
		APinguMenu3D::Get(HUD.Get());
		// The island's input layer (always up): every click goes through it - a strip button, else the island's penguins, tags, x
		// and "+" (APinguLobby) - so no click depends on the viewport's mouse capture (PIE, DPI, the lock). It takes the keyboard
		// focus only while the strip has the keys (Esc / gamepad Start: arrows / d-pad along the strip, Enter / A press, Esc / B
		// back to the island); otherwise the keys stay on the game (Tab, Enter, Delete for the lobby's picks).
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD).bLobbyIsland(true);
		LobbyMenu3DInput = Input;
		return MakeScreen(EPinguScreen::Lobby,
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Visibility(EVisibility::HitTestInvisible)
				[
					SAssignNew(LobbyPlayersBox, SVerticalBox)
					.Visibility(EVisibility::Collapsed)
				]
			]
			+ SOverlay::Slot()
			[
				Input
			],
			false);
	}

	// The lobby island (no -PinguOldMenus): everybody stands on the island (APinguLobby: tags, "+", clicks), so no title and
	// no cards here - only the lobby's buttons, at the bottom of the screen.
	const bool bIsland = !PinguUseOldMenus();
	const EVisibility CardsShown = bIsland ? EVisibility::Collapsed : EVisibility::Visible;
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility(CardsShown)
			[
				MakeTitle(LOCTEXT("LobbyTitle", "LOBBY"), 48.f)
			]
		]
		// ---- the cards: everybody (five to a row), the bots, the + card
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, bIsland ? 0.f : 16.f, 0.f, 0.f)
		[
			SNew(SBox)
			.Visibility(CardsShown)
			.WidthOverride(GridWidth)
			.MinDesiredHeight(2.f * LobbyCardHeight + LobbyCardGap)
			[
				SAssignNew(LobbyPlayersBox, SVerticalBox)
			]
		]
		// ---- settings, back (solo) / close the game (host) / leave (others); TRAITS ON / OFF; START (the host) or waiting
		// (the others)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, bIsland ? 0.f : 20.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SAssignNew(LobbySettings, SPinguButton)
				.Width(LobbyButtonWidth)
				.ContentPadding(LobbyButtonPadding)
				.Height(FPinguStyle::SmallHeight)
				.FontSize(20.f)
				.Text(LOCTEXT("LobbySettings", "SETTINGS"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Settings(); } })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SPinguButton)
					.Visibility_Lambda(SoloOnly)
					.Kind(EPinguButtonKind::Ghost)
					.Width(LobbyButtonWidth)
					.ContentPadding(LobbyButtonPadding)
					.Height(FPinguStyle::SmallHeight)
					.FontSize(20.f)
					.Text(LOCTEXT("LobbyBack", "BACK"))
					.HUD(HUD)
					.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_MainMenu(); } })
				]
				+ SOverlay::Slot()
				[
					SNew(SPinguButton)
					.Visibility_Lambda(LanOnly)
					.Kind(EPinguButtonKind::Danger)
					.Width(LobbyButtonWidth)
					.ContentPadding(LobbyButtonPadding)
					.Height(FPinguStyle::SmallHeight)
					.FontSize(20.f)
					.Text_Lambda([this]() { return HUD.IsValid() && HUD->HasMatchAuthority() ? LOCTEXT("CloseGame", "CLOSE GAME") : LOCTEXT("LeaveLobby", "LEAVE"); })
					.HUD(HUD)
					.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_LeaveGame(); } })
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SSpacer)
			]
			// TRAITS: off by default, orange when on. The host (and solo) switch it; the others see what the host chose.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)
			[
				SNew(SBox)
				.WidthOverride(LobbyButtonWidth)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SPinguButton)
						.Visibility_Lambda(HostOnly)
						.Kind(EPinguButtonKind::Segment)
						.Height(FPinguStyle::SmallHeight)
						.FontSize(20.f)
						.ContentPadding(LobbyButtonPadding)
						.Text_Lambda([this]() { return TraitsLabel(HUD.IsValid() && HUD->AreTraitsOn()); })
						.IsSelected_Lambda([this]() { return HUD.IsValid() && HUD->AreTraitsOn(); })
						.HUD(HUD)
						.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_ToggleTraits(); } })
					]
					// The others: the same look, but only to see (no focus, no click - Linus: the host decides).
					+ SOverlay::Slot()
					[
						SNew(SBox)
						.Visibility_Lambda([this]() { return HUD.IsValid() && !HUD->HasMatchAuthority() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
						.HeightOverride(FPinguStyle::SmallHeight)
						[
							SNew(SOverlay)
							+ SOverlay::Slot()
							[
								SNew(SPinguShape)
								.FillColor_Lambda([this]() { return HUD.IsValid() && HUD->AreTraitsOn() ? FPinguStyle::Get().OrangeLip : FPinguStyle::Get().PanelDeep; })
								.OutlineColor(Style.Ink)
								.OutlineWidth(FPinguStyle::LineWidth)
								.Radius(FPinguStyle::ButtonRadius)
							]
							+ SOverlay::Slot()
							.Padding(FMargin(0.f, 0.f, 0.f, FPinguStyle::LipHeight))
							[
								SNew(SPinguShape)
								.FillColor_Lambda([this]() { return HUD.IsValid() && HUD->AreTraitsOn() ? FPinguStyle::Get().Orange : FPinguStyle::Get().Row; })
								.OutlineColor(Style.Ink)
								.OutlineWidth(FPinguStyle::LineWidth)
								.Radius(FPinguStyle::ButtonRadius)
							]
							+ SOverlay::Slot()
							.Padding(FMargin(8.f, 0.f, 8.f, FPinguStyle::LipHeight))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text_Lambda([this]() { return TraitsLabel(HUD.IsValid() && HUD->AreTraitsOn()); })
								.Font(FPinguStyle::Title(20.f))
								.ColorAndOpacity_Lambda([this]() { return FSlateColor(HUD.IsValid() && HUD->AreTraitsOn() ? FPinguStyle::Get().Snow : FPinguStyle::Get().Ice); })
							]
						]
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(LobbyStartWidth)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(LobbyStart, SPinguButton)
						.Visibility_Lambda(HostOnly)
						.Kind(EPinguButtonKind::Primary)
						.Height(64.f)
						.FontSize(28.f)
						.Text(LOCTEXT("StartMatch", "START"))
						.HUD(HUD)
						.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_StartLobbyMatch(); } })
					]
					+ SOverlay::Slot()
					.VAlign(VAlign_Center)
					.HAlign(HAlign_Right)
					[
						SNew(STextBlock)
						.Visibility_Lambda(ClientOnly)
						.Text_Lambda([this]()
						{
							return FText::Format(LOCTEXT("WaitingForHost", "Waiting for {0}{1}"), FText::FromString(HUD.IsValid() ? HUD->GetHostName() : FString()), FText::FromString(Dots()));
						})
						.Font(FPinguStyle::Body(20.f))
						.ColorAndOpacity(Style.Gold)
						.AutoWrapText(true)
					]
				]
			]
		]
		// ---- the host: how the others get in. The whole line is a button: a click (or the confirm key on it) copies the
		// address to the clipboard, and the address says COPIED! for a moment (the label and the copy icon stay).
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 12.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this]() { return ShowIf(HUD.IsValid() && HUD->HasMatchAuthority() && HUD->IsNetworkGame()); })
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SAssignNew(LobbyAddress, SPinguButton)
				.Kind(EPinguButtonKind::Ghost)
				.Height(LobbyAddressHeight)
				.ContentPadding(FMargin(12.f, 0.f))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_CopyHostAddress(); } })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(LOCTEXT("OthersJoin", "Others join with")).Font(FPinguStyle::Body(16.f)).ColorAndOpacity(Style.Ice)
					]
					// The address and COPIED! share one place (the wider of the two sets the size: the line never jumps).
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						.HAlign(HAlign_Center)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]() { return HUD.IsValid() && HUD->IsHostAddressCopied() ? EVisibility::Hidden : EVisibility::Visible; })
							.Text_Lambda([this]() { return FText::FromString(HUD.IsValid() ? HUD->GetHostAddressText() : FString()); })
							.Font(FPinguStyle::Title(20.f))
							.ColorAndOpacity(Style.Gold)
						]
						+ SOverlay::Slot()
						.HAlign(HAlign_Center)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]() { return HUD.IsValid() && HUD->IsHostAddressCopied() ? EVisibility::Visible : EVisibility::Hidden; })
							.Text(LOCTEXT("AddressCopied", "COPIED!"))
							.Font(FPinguStyle::Title(20.f))
							.ColorAndOpacity(Style.Green)
						]
					]
					// The copy icon: two pages (the front one hides the back one's corner), green while COPIED! shows.
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
					[
						SNew(SBox)
						.WidthOverride(LobbyCopyIconSize)
						.HeightOverride(LobbyCopyIconSize)
						[
							SNew(SOverlay)
							+ SOverlay::Slot()
							.Padding(FMargin(0.f, 0.f, LobbyCopyIconShift, LobbyCopyIconShift))
							[
								SNew(SPinguShape)
								.FillColor(FLinearColor::Transparent)
								.OutlineColor_Lambda([this]() { return HUD.IsValid() && HUD->IsHostAddressCopied() ? FPinguStyle::Get().Green : FPinguStyle::Get().Ice; })
								.OutlineWidth(FPinguStyle::LineWidth)
								.Radius(3.f)
							]
							+ SOverlay::Slot()
							.Padding(FMargin(LobbyCopyIconShift, LobbyCopyIconShift, 0.f, 0.f))
							[
								SNew(SPinguShape)
								.FillColor_Lambda([this]()
								{
									// The card's colour, a little lighter where the line's face is (hovered / focused).
									const FPinguStyle& Current = FPinguStyle::Get();
									const TSharedPtr<SWidget> Line = LobbyAddress;
									const bool bHot = Line.IsValid() && StaticCastSharedPtr<SPinguButton>(Line)->IsHot();
									return bHot ? FMath::Lerp(Current.Panel, Current.Ice, 0.14f) : Current.Panel;
								})
								.OutlineColor_Lambda([this]() { return HUD.IsValid() && HUD->IsHostAddressCopied() ? FPinguStyle::Get().Green : FPinguStyle::Get().Ice; })
								.OutlineWidth(FPinguStyle::LineWidth)
								.Radius(3.f)
							]
						]
					]
				]
			]
		];

	return MakeScreen(EPinguScreen::Lobby,
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(bIsland ? VAlign_Bottom : VAlign_Center)
		.Padding(FMargin(32.f, 24.f))
		[
			MakeCard(Box, bIsland ? FMargin(28.f, 16.f, 28.f, 18.f) : FMargin(36.f, 20.f, 36.f, 28.f), GridWidth + 72.f)
		],
		false);
}

void SPinguRoot::RebuildLobbyPlayers()
{
	using namespace PinguUIMenusPrivate;
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || !LobbyPlayersBox.IsValid())
	{
		return;
	}
	// Rebuilt only when cards come or go (or change hands); their team and skill follow the data by themselves.
	const bool bAuthority = PenguinHUD->HasMatchAuthority();
	const bool bAdd = PenguinHUD->CanAddBot();
	const bool bRemove = PenguinHUD->CanRemoveBot();
	FString Key = FString::Printf(TEXT("%d%d%d%d:"), bAuthority ? 1 : 0, PenguinHUD->IsNetworkGame() ? 1 : 0, bAdd ? 1 : 0, bRemove ? 1 : 0);
	for (const FPinguLobbyPlayer& Card : LobbyCards)
	{
		Key += FString::Printf(TEXT("%s/%d/%d/%d/%d/%d|"), *Card.Name, Card.Slot, Card.bHost ? 1 : 0, Card.bYou ? 1 : 0, Card.bBot ? 1 : 0, Card.BotIndex);
	}
	if (Key == BuiltLobbyKey)
	{
		return;
	}
	BuiltLobbyKey = Key;

	// The keyboard focus stays where it was across the rebuild (a + or an X pressed, somebody joining or leaving): on the
	// same card (who it is, wherever it moved), else the card now at its place, else the + card - never somewhere else.
	FString FocusedId;
	int32 FocusedPlace = INDEX_NONE;
	const bool bAddFocused = LobbyAddButton.IsValid() && LobbyAddButton->HasAnyUserFocusOrFocusedDescendants();
	for (int32 Index = 0; Index < LobbyCardWidgets.Num(); ++Index)
	{
		if (LobbyCardWidgets[Index].IsValid() && LobbyCardWidgets[Index]->HasAnyUserFocusOrFocusedDescendants())
		{
			FocusedPlace = Index;
			FocusedId = LobbyCardIds.IsValidIndex(Index) ? LobbyCardIds[Index] : FString();
		}
	}

	LobbyPlayersBox->ClearChildren();
	LobbyCardWidgets.Reset();
	LobbyCardButtons.Reset();
	LobbyCardIds.Reset();
	LobbyAddButton.Reset();
	TArray<TSharedRef<SWidget>> Widgets;
	for (int32 Index = 0; Index < LobbyCards.Num(); ++Index)
	{
		Widgets.Add(MakeLobbyCard(Index));
		LobbyCardWidgets.Add(Widgets.Last());
		LobbyCardIds.Add(FString::Printf(TEXT("%s:%s"), LobbyCards[Index].bBot ? TEXT("B") : TEXT("P"), *LobbyCards[Index].Name));
	}
	if (bAdd)
	{
		Widgets.Add(MakeAddBotCard());
	}
	// The places nobody has yet: faint empty seats (the grid always shows all ten).
	const FPinguStyle& Style = FPinguStyle::Get();
	while (Widgets.Num() < PinguMaxPenguins)
	{
		Widgets.Add(SNew(SBox)
			.WidthOverride(LobbyCardWidth)
			.HeightOverride(LobbyCardHeight)
			[
				SNew(SPinguShape)
				.FillColor(PinguUI::WithAlpha(Style.PanelDeep, 0.35f))
				.OutlineColor(PinguUI::WithAlpha(Style.IceDim, 0.12f))
				.OutlineWidth(FPinguStyle::LineWidth)
				.Radius(FPinguStyle::RowRadius)
			]);
	}
	for (int32 First = 0; First < Widgets.Num(); First += LobbyCardsPerRow)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (int32 Index = First; Index < FMath::Min(First + LobbyCardsPerRow, Widgets.Num()); ++Index)
		{
			Row->AddSlot().AutoWidth().Padding(Index > First ? LobbyCardGap : 0.f, 0.f, 0.f, 0.f)
			[
				Widgets[Index]
			];
		}
		LobbyPlayersBox->AddSlot()
		.AutoHeight()
		.Padding(0.f, First > 0 ? LobbyCardGap : 0.f, 0.f, 0.f)
		[
			Row
		];
	}

	TSharedPtr<SWidget> Back;
	if (FocusedPlace != INDEX_NONE)
	{
		// The same card; gone (its X): yours at its place, then after it, then before it.
		const int32 Same = FocusedId.IsEmpty() ? INDEX_NONE : LobbyCardIds.IndexOfByKey(FocusedId);
		if (LobbyCardButtons.IsValidIndex(Same) && LobbyCardButtons[Same].IsValid())
		{
			Back = LobbyCardButtons[Same];
		}
		for (int32 Index = FocusedPlace; !Back.IsValid() && Index < LobbyCardButtons.Num(); ++Index)
		{
			Back = LobbyCardButtons[Index];
		}
		for (int32 Index = FMath::Min(FocusedPlace, LobbyCardButtons.Num()) - 1; !Back.IsValid() && Index >= 0; --Index)
		{
			Back = LobbyCardButtons[Index];
		}
	}
	if (FocusedPlace != INDEX_NONE || bAddFocused)
	{
		if (!Back.IsValid())
		{
			Back = LobbyAddButton;
		}
		for (int32 Index = LobbyCardButtons.Num() - 1; !Back.IsValid() && Index >= 0; --Index)
		{
			Back = LobbyCardButtons[Index];
		}
	}
	if (Back.IsValid())
	{
		PendingFocus = Back;
		PendingFocusUntil = PinguUI::Now() + 0.5;
	}
}

TSharedRef<SWidget> SPinguRoot::MakeLobbyCard(int32 Index)
{
	using namespace PinguUIMenusPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	const APenguinHUD* PenguinHUD = HUD.Get();
	const FPinguLobbyPlayer Card = LobbyCards.IsValidIndex(Index) ? LobbyCards[Index] : FPinguLobbyPlayer();
	const bool bAuthority = PenguinHUD && PenguinHUD->HasMatchAuthority();
	const bool bNetwork = PenguinHUD && PenguinHUD->IsNetworkGame();
	// Yours to change: your own card, and - the host's - the bots'.
	const bool bMine = Card.bYou || (Card.bBot && bAuthority);
	const bool bBot = Card.bBot;
	const int32 BotIndex = Card.BotIndex;
	const FLinearColor Color = APenguinCharacter::GetSlotColor(Card.Slot);
	// The team and the skill of this frame's card (they change without a rebuild).
	auto TeamOf = [this, Index]() { return LobbyCards.IsValidIndex(Index) ? LobbyCards[Index].TeamChoice : -1; };
	auto SkillOf = [this, Index]() { return LobbyCards.IsValidIndex(Index) ? LobbyCards[Index].Skill : 1; };

	// The portrait in its own colour, the team's ring around it.
	constexpr float Ring = 6.f;
	TSharedRef<SWidget> Portrait = SNew(SBox)
		.WidthOverride(LobbyPortraitSize)
		.HeightOverride(LobbyPortraitSize)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			.Padding(-(Ring + 2.f))
			[
				SNew(SPinguShape)
				.Visibility_Lambda([TeamOf]() { return TeamOf() >= 0 ? EVisibility::HitTestInvisible : EVisibility::Hidden; })
				.FillColor(FLinearColor::Transparent)
				.OutlineColor_Lambda([TeamOf]() { return APenguinCharacter::GetTeamColor(FMath::Max(TeamOf(), 0)); })
				.OutlineWidth(Ring)
				.Pill(true)
			]
			+ SOverlay::Slot()
			[
				SNew(SPinguShape)
				.FillColor(Color * 0.75f + FLinearColor(0.05f, 0.05f, 0.08f, 0.25f))
				.OutlineColor(Style.Ink)
				.OutlineWidth(FPinguStyle::LineWidth)
				.Pill(true)
			]
			+ SOverlay::Slot()
			.Padding(4.f)
			[
				SNew(SImage)
				.Image(PenguinHUD ? PenguinHUD->GetPortraitBrush(Card.Slot) : &Style.NoBrush)
			]
			+ SOverlay::Slot()
			.Padding(4.f)
			[
				SNew(SImage)
				.Image(PenguinHUD ? PenguinHUD->GetPortraitFeltBrush(Card.Slot) : &Style.NoBrush)
				.ColorAndOpacity(APenguinCharacter::GetBeanieTint(Card.Slot))
			]
		];

	// The team: a pill in its colour with its name (NO TEAM: grey).
	TSharedRef<SWidget> TeamPill = SNew(SBox)
		.HeightOverride(26.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape)
				.FillColor_Lambda([TeamOf]()
				{
					const int32 Team = TeamOf();
					return Team >= 0 ? APenguinCharacter::GetTeamColor(Team) : PinguUI::WithAlpha(FPinguStyle::Get().IceDim, 0.35f);
				})
				.OutlineColor(Style.Ink)
				.OutlineWidth(FPinguStyle::LineWidth)
				.Pill(true)
			]
			+ SOverlay::Slot()
			.Padding(FMargin(8.f, 0.f, 8.f, 1.f))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([TeamOf]()
				{
					const int32 Team = TeamOf();
					return Team >= 0 ? FText::Format(LOCTEXT("CardTeam", "{0} TEAM"), APenguinCharacter::GetTeamName(Team)) : LOCTEXT("CardNoTeam", "NO TEAM");
				})
				// (Room for the longest, "YELLOW TEAM", on the card.)
				.Font(FPinguStyle::Title(14.f))
				.ColorAndOpacity_Lambda([TeamOf]() { return FSlateColor(TeamOf() >= 0 ? FPinguStyle::Get().Ink : FPinguStyle::Get().Snow); })
			]
		];

	TSharedRef<SVerticalBox> Middle = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.f, 34.f, 0.f, 0.f)
		[
			Portrait
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(8.f, 10.f, 8.f, 0.f)
		[
			SNew(SBox)
			.MaxDesiredWidth(LobbyCardWidth - 16.f)
			[
				SNew(STextBlock)
				.Text(PinguUI::Upper(Card.Name))
				.Font(FPinguStyle::Title(20.f, 2))
				.ColorAndOpacity(FMath::Lerp(Color, FLinearColor::White, 0.3f))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.f, 6.f, 0.f, 0.f)
		[
			TeamPill
		];
	if (bBot)
	{
		// A bot's skill: the host clicks through EASY, NORMAL, HARD (a right click goes back).
		TSharedRef<SWidget> Skill = bAuthority
			? StaticCastSharedRef<SWidget>(SNew(SPinguButton)
				.Kind(EPinguButtonKind::Segment)
				.Width(116.f)
				.Height(30.f)
				.FontSize(15.f)
				.ContentPadding(FMargin(4.f, 0.f))
				.Text_Lambda([SkillOf]() { return DifficultyName(SkillOf()); })
				.HUD(HUD)
				.OnClicked_Lambda([this, BotIndex]() { if (HUD.IsValid()) { HUD->Action_ChangeBotSkill(BotIndex, 1); } })
				.OnRightClicked_Lambda([this, BotIndex]() { if (HUD.IsValid()) { HUD->Action_ChangeBotSkill(BotIndex, -1); } }))
			: MakeTag(TAttribute<FText>::CreateLambda([SkillOf]() { return DifficultyName(SkillOf()); }), Style.Row, Style.Ice, 15.f);
		Middle->AddSlot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			Skill
		];
	}

	// The corner: YOU, the LAN host, BOT - and a bot's X (the host, while it may go).
	TSharedRef<SOverlay> Face = SNew(SOverlay)
		+ SOverlay::Slot()
		[
			// Your own card: a gold rim.
			SNew(SPinguShape)
			.Visibility(Card.bYou ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
			.FillColor(FLinearColor::Transparent)
			.OutlineColor(Style.Gold)
			.OutlineWidth(FPinguStyle::RingWidth)
			.Radius(FPinguStyle::RowRadius)
		]
		+ SOverlay::Slot()
		[
			Middle
		];
	TSharedPtr<SWidget> CornerTag;
	if (Card.bYou)
	{
		CornerTag = MakeTag(LOCTEXT("TagYouLobby", "YOU"), Style.Gold, Style.Ink, 12.f);
	}
	else if (Card.bHost && bNetwork)
	{
		CornerTag = MakeTag(LOCTEXT("TagHost", "HOST"), Style.Ice, Style.Ink, 12.f);
	}
	else if (bBot)
	{
		CornerTag = MakeTag(LOCTEXT("TagBot", "BOT"), PinguUI::WithAlpha(Style.IceDim, 0.5f), Style.Snow, 12.f);
	}
	if (CornerTag.IsValid())
	{
		// (Above the team ring: the portrait starts below the tag.)
		Face->AddSlot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(6.f)
		[
			CornerTag.ToSharedRef()
		];
	}
	if (bBot && bAuthority && PenguinHUD && PenguinHUD->CanRemoveBot())
	{
		Face->AddSlot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(6.f)
		[
			SNew(SPinguButton)
			.Kind(EPinguButtonKind::Ghost)
			.Width(30.f)
			.Height(30.f)
			.FontSize(16.f)
			.ContentPadding(FMargin(0.f))
			.Text(LOCTEXT("RemoveBot", "X"))
			.HUD(HUD)
			.OnClicked_Lambda([this, BotIndex]() { if (HUD.IsValid()) { HUD->Action_RemoveBot(BotIndex); } })
			.OnRightClicked_Lambda([]() {}) // (a right click on the X is not one on the card under it)
		];
	}
	TSharedRef<SWidget> Content = SNew(SBox)
		.WidthOverride(LobbyCardWidth)
		.HeightOverride(LobbyCardHeight)
		[
			Face
		];

	if (!bMine)
	{
		// Somebody else's: just the card.
		LobbyCardButtons.Add(nullptr);
		return SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SPinguShape)
				.FillColor(Style.Row)
				.Radius(FPinguStyle::RowRadius)
			]
			+ SOverlay::Slot()
			[
				Content
			];
	}
	// Yours: a click picks the next team (a right click the one before).
	TSharedRef<SPinguButton> Button = SNew(SPinguButton)
		.Kind(EPinguButtonKind::Row)
		.Width(LobbyCardWidth)
		.Height(LobbyCardHeight)
		.HAlign(HAlign_Fill)
		.ContentPadding(FMargin(0.f))
		.HUD(HUD)
		.OnClicked_Lambda([this, bBot, BotIndex]()
		{
			if (HUD.IsValid())
			{
				bBot ? HUD->Action_ChangeBotTeam(BotIndex, 1) : HUD->Action_ChangeTeamChoice(1, true);
			}
		})
		.OnRightClicked_Lambda([this, bBot, BotIndex]()
		{
			if (HUD.IsValid())
			{
				bBot ? HUD->Action_ChangeBotTeam(BotIndex, -1) : HUD->Action_ChangeTeamChoice(-1, true);
			}
		})
		[
			Content
		];
	LobbyCardButtons.Add(Button);
	return Button;
}

TSharedRef<SWidget> SPinguRoot::MakeAddBotCard()
{
	using namespace PinguUIMenusPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedRef<SPinguButton> Button = SNew(SPinguButton)
		.Kind(EPinguButtonKind::Row)
		.Width(LobbyCardWidth)
		.Height(LobbyCardHeight)
		.HAlign(HAlign_Center)
		.ContentPadding(FMargin(0.f))
		.HUD(HUD)
		.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_AddBot(); } })
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(LobbyPortraitSize)
				.HeightOverride(LobbyPortraitSize)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SPinguShape)
						.FillColor(PinguUI::WithAlpha(Style.PanelDeep, 0.6f))
						.OutlineColor(PinguUI::WithAlpha(Style.Ice, 0.7f))
						.OutlineWidth(FPinguStyle::RingWidth)
						.Pill(true)
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Padding(0.f, 0.f, 0.f, 6.f)
					[
						SNew(STextBlock).Text(LOCTEXT("AddBotPlus", "+")).Font(FPinguStyle::Title(60.f, 3)).ColorAndOpacity(Style.Snow)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(0.f, 14.f, 0.f, 0.f)
			[
				SNew(STextBlock).Text(LOCTEXT("AddBot", "ADD BOT")).Font(FPinguStyle::Title(20.f)).ColorAndOpacity(Style.Ice)
			]
		];
	LobbyAddButton = Button;
	return Button;
}

// ============================================================================ pause

TSharedRef<SWidget> SPinguRoot::BuildPause()
{
	using namespace PinguUIMenusPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedPtr<SPinguButton> ResumeButton;

	// The 3D pause page: the input layer only (the dim stays: the game darkens behind the menu).
	if (PinguMenu3D::IsEnabled())
	{
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		FirstFocus.Add(static_cast<uint8>(EPinguScreen::Pause), Input);
		return MakeScreen(EPinguScreen::Pause, Input, false);
	}
	auto SoloOnly = [this]() { return ShowIf(HUD.IsValid() && !HUD->IsNetworkGame()); };
	auto LanOnly = [this]() { return ShowIf(HUD.IsValid() && HUD->IsNetworkGame()); };

	// Only what is needed: RESUME, END MATCH (solo / the LAN host, in a match), SETTINGS (the controls open from there),
	// then out: MAIN MENU (solo: back to the title screen - quitting is the title's QUIT) or CLOSE GAME / LEAVE GAME (LAN).
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return HUD.IsValid() && HUD->IsNetworkGame() ? LOCTEXT("LanMenu", "MENU") : LOCTEXT("Paused", "PAUSED"); })
			.Font(FPinguStyle::Title(56.f, 4))
			.ColorAndOpacity(Style.Snow)
			.ShadowOffset(FVector2D(0.f, 4.f))
			.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.6f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SAssignNew(ResumeButton, SPinguButton)
			.Kind(EPinguButtonKind::Primary)
			.Text(LOCTEXT("Resume", "RESUME"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Resume(); } })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Visibility_Lambda([this]() { return ShowIf(HUD.IsValid() && HUD->CanEndMatch()); })
			.Text(LOCTEXT("EndMatch", "END MATCH"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_EndMatch(); } })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Text(LOCTEXT("PauseSettings", "SETTINGS"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Settings(); } })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			SNew(SPinguButton)
			.Visibility_Lambda(LanOnly)
			.Kind(EPinguButtonKind::Danger)
			.Height(FPinguStyle::SmallHeight)
			.FontSize(20.f)
			.Text_Lambda([this]() { return HUD.IsValid() && HUD->HasMatchAuthority() ? LOCTEXT("EndGame", "CLOSE GAME") : LOCTEXT("LeaveGame", "LEAVE GAME"); })
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_LeaveGame(); } })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		[
			// Out of the match to the title screen (quitting the game is the title's QUIT).
			SNew(SPinguButton)
			.Visibility_Lambda(SoloOnly)
			.Kind(EPinguButtonKind::Ghost)
			.Height(FPinguStyle::SmallHeight)
			.FontSize(20.f)
			.Text(LOCTEXT("MainMenu", "MAIN MENU"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_MainMenu(); } })
		];
	FirstFocus.Add(static_cast<uint8>(EPinguScreen::Pause), ResumeButton);

	return MakeScreen(EPinguScreen::Pause,
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MakeCard(Box, FMargin(40.f, 24.f, 40.f, 32.f), 456.f)
		]);
}

// ============================================================================ results

TSharedRef<SWidget> SPinguRoot::BuildResults()
{
	using namespace PinguUIMenusPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedPtr<SPinguButton> RematchButton;

	// The 3D results page.
	if (PinguMenu3D::IsEnabled())
	{
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		FirstFocus.Add(static_cast<uint8>(EPinguScreen::Results), Input);
		return MakeScreen(EPinguScreen::Results, Input, false);
	}
	auto CanChoose = [this]() { return ShowIf(HUD.IsValid() && HUD->HasMatchAuthority()); };
	auto MustWait = [this]() { return ShowIf(HUD.IsValid() && !HUD->HasMatchAuthority()); };

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return GetResultsTitle(); })
			// A long name ("PURPLE TEAM WINS!", a long player name) gets a smaller size: it always fits the card.
			.Font_Lambda([this]() { return FitTitleFont(GetResultsTitle(), 80.f, 6, ResultsWidth - 96.f - 24.f); })
			.ColorAndOpacity_Lambda([this]()
			{
				bool bLocalWin = false;
				if (HUD.IsValid() && HUD->IsTeamMatch())
				{
					const FPinguScore* Local = HUD->GetScores().FindByPredicate([this](const FPinguScore& Score) { return HUD->IsLocalScore(Score); });
					bLocalWin = Local && HUD->GetMatchWinnerTeam() >= 0 && Local->Team == HUD->GetMatchWinnerTeam();
				}
				else
				{
					const FPinguScore* Winner = HUD.IsValid() ? HUD->FindScoreBySlot(HUD->GetMatchWinnerSlot()) : nullptr;
					bLocalWin = Winner && HUD->IsLocalScore(*Winner);
				}
				return FSlateColor(bLocalWin ? FPinguStyle::Get().Gold : FPinguStyle::Get().Snow);
			})
			.ShadowOffset(FVector2D(0.f, 6.f))
			.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.5f))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SAssignNew(StandingsBox, SVerticalBox)
		]
		// Solo / host: choose. Clients: wait (or leave).
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda(CanChoose)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SAssignNew(RematchButton, SPinguButton)
				.Kind(EPinguButtonKind::Primary)
				.Width(256.f)
				.Text(LOCTEXT("Rematch", "REMATCH"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_Rematch(); } })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(16.f, 0.f, 0.f, 0.f)
			[
				// Back to the lobby (solo: your own - the bots, their teams and skills as you left them).
				SNew(SPinguButton)
				.Width(256.f)
				.Text(LOCTEXT("ToLobby", "LOBBY"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_ReturnToLobby(); } })
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SNew(SVerticalBox)
			.Visibility_Lambda(MustWait)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return FText::Format(LOCTEXT("WaitingChoice", "Waiting for {0}{1}"), FText::FromString(HUD.IsValid() ? HUD->GetHostName() : FString()), FText::FromString(Dots()));
				})
				.Font(FPinguStyle::Body(20.f))
				.ColorAndOpacity(Style.Gold)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.f, 16.f, 0.f, 0.f)
			[
				SAssignNew(ResultsLeave, SPinguButton)
				.Kind(EPinguButtonKind::Danger)
				.Width(256.f)
				.Text(LOCTEXT("ResultsLeave", "LEAVE"))
				.HUD(HUD)
				.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_LeaveGame(); } })
			]
		];
	FirstFocus.Add(static_cast<uint8>(EPinguScreen::Results), RematchButton);

	return MakeScreen(EPinguScreen::Results,
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MakeCard(Box, FMargin(48.f, 24.f, 48.f, 32.f), ResultsWidth)
		]);
}

FText SPinguRoot::GetResultsTitle() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return FText::GetEmpty();
	}
	const FPinguScore* Winner = nullptr;
	if (PenguinHUD->IsTeamMatch())
	{
		// A team match: the team that won it (VICTORY! for this PC's player's team) - somebody on their own by name.
		const int32 Team = PenguinHUD->GetMatchWinnerTeam();
		if (Team < 0)
		{
			return LOCTEXT("MatchOver", "MATCH OVER");
		}
		Winner = PenguinHUD->GetLoneTeamMember(Team);
		if (!Winner)
		{
			const FPinguScore* Local = PenguinHUD->GetScores().FindByPredicate([PenguinHUD](const FPinguScore& Score) { return PenguinHUD->IsLocalScore(Score); });
			return Local && Local->Team == Team ? LOCTEXT("Victory", "VICTORY!") : FText::Format(LOCTEXT("TeamWinsMatch", "{0} TEAM WINS!"), APenguinCharacter::GetTeamName(Team));
		}
	}
	else
	{
		Winner = PenguinHUD->FindScoreBySlot(PenguinHUD->GetMatchWinnerSlot());
	}
	if (!Winner)
	{
		return LOCTEXT("MatchOver", "MATCH OVER");
	}
	return PenguinHUD->IsLocalScore(*Winner) ? LOCTEXT("Victory", "VICTORY!") : FText::Format(LOCTEXT("WinsMatch", "{0} WINS!"), PinguUI::Upper(Winner->Name));
}

void SPinguRoot::RebuildStandings()
{
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || !StandingsBox.IsValid())
	{
		return;
	}
	// The match is over: now every statistic (between rounds the board showed only the wins).
	const TArray<FPinguScore>& Scores = PenguinHUD->GetScores();
	const bool bTeams = PenguinHUD->IsTeamMatch();
	TArray<int32> Ranking = PenguinHUD->GetLeaderboard();
	TArray<int32> TeamOrder;
	if (bTeams)
	{
		// The best team's penguins first, each team's in the order of their own statistics.
		TeamOrder = PenguinHUD->GetTeamBoard();
		Ranking.StableSort([&Scores, &TeamOrder](int32 A, int32 B)
		{
			const int32 TeamA = TeamOrder.IndexOfByKey(Scores[A].Team);
			const int32 TeamB = TeamOrder.IndexOfByKey(Scores[B].Team);
			return TeamA < TeamB;
		});
	}
	const int32 MatchWinner = PenguinHUD->GetMatchWinnerSlot();
	const int32 WinnerTeam = PenguinHUD->GetMatchWinnerTeam();
	FString Key = FString::Printf(TEXT("%d:%d:%d:"), MatchWinner, WinnerTeam, bTeams ? 1 : 0);
	for (const int32 Index : Ranking)
	{
		if (Scores.IsValidIndex(Index))
		{
			const FPinguScore& Score = Scores[Index];
			Key += FString::Printf(TEXT("%s/%d/%d/%d/%d/%d/%d|"), *Score.Name, Score.Slot, Score.Wins, Score.KOs, Score.Falls, Score.Hits, Score.Team);
		}
	}
	if (Key == BuiltStandingsKey)
	{
		return;
	}
	BuiltStandingsKey = Key;
	StandingsBox->ClearChildren();
	const FPinguStyle& Style = FPinguStyle::Get();
	// More than six penguins: compact rows (ten fit on the card).
	const bool bCompact = Ranking.Num() > 6;
	const float RowHeight = bCompact ? 44.f : 58.f;
	const float NameFont = bCompact ? 22.f : 26.f;
	const float StatFont = bCompact ? 22.f : 24.f;
	const auto StatColumn = [&Style, StatFont](const FText& Text, const FLinearColor& Color) -> TSharedRef<SWidget>
	{
		return SNew(SBox).WidthOverride(PinguUIMenusPrivate::StatWidth).HAlign(HAlign_Center)
			[
				SNew(STextBlock).Text(Text).Font(FPinguStyle::Title(StatFont)).ColorAndOpacity(Color)
			];
	};

	// The column labels. (A team match: the teams in the table itself - teammates together, a ring and a pill in their
	// team's colour; a team's wins are each of its penguins' wins.)
	StandingsBox->AddSlot()
	.AutoHeight()
	.Padding(FMargin(12.f, 0.f, 24.f, 6.f))
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SSpacer)
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			StatColumn(LOCTEXT("ColWins", "WINS"), Style.IceDim)
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			StatColumn(LOCTEXT("ColKOs", "KOS"), Style.IceDim)
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			StatColumn(LOCTEXT("ColFalls", "FALLS"), Style.IceDim)
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			StatColumn(LOCTEXT("ColHits", "HITS"), Style.IceDim)
		]
	];

	int32 Rank = 0;
	for (const int32 Index : Ranking)
	{
		if (!Scores.IsValidIndex(Index))
		{
			continue;
		}
		const FPinguScore& Score = Scores[Index];
		const int32 ColorIndex = PenguinHUD->GetScoreColorIndex(Score);
		const FLinearColor Color = APenguinCharacter::GetSlotColor(ColorIndex);
		const bool bWinner = bTeams ? (WinnerTeam >= 0 && Score.Team == WinnerTeam) : Score.Slot == MatchWinner;
		const bool bYou = PenguinHUD->IsNetworkGame() && PenguinHUD->IsLocalScore(Score);
		const int32 TeamRing = PenguinHUD->GetScoreTeamRing(Score);
		// A little more room where the next team starts.
		const bool bNewTeam = bTeams && Rank > 0 && Scores.IsValidIndex(Ranking[Rank - 1]) && Scores[Ranking[Rank - 1]].Team != Score.Team;
		StandingsBox->AddSlot()
		.AutoHeight()
		.Padding(0.f, Rank > 0 ? (bCompact ? 4.f : 8.f) + (bNewTeam ? 6.f : 0.f) : 0.f, 0.f, 0.f)
		[
			SNew(SBox)
			.HeightOverride(RowHeight)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SPinguShape)
					.FillColor(Style.Row)
					.OutlineColor(bWinner ? Style.Gold : Style.Ink)
					.OutlineWidth(bWinner ? FPinguStyle::RingWidth : FPinguStyle::LineWidth)
					.Radius(FPinguStyle::RowRadius + 4.f)
				]
				+ SOverlay::Slot()
				.Padding(FMargin(12.f, 0.f, 24.f, 0.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox).WidthOverride(40.f).HAlign(HAlign_Center)
						[
							SNew(STextBlock).Text(FText::AsNumber(Rank + 1)).Font(FPinguStyle::Title(bCompact ? 26.f : 30.f, 2)).ColorAndOpacity(Rank == 0 ? Style.Gold : Style.Ice)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
					[
						MakePortrait(Score.Slot, bCompact ? 34.f : 44.f, TeamRing)
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(16.f, 0.f, 8.f, 0.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(PinguUI::Upper(Score.Name))
							.Font(FPinguStyle::Title(NameFont, 2))
							.ColorAndOpacity(FMath::Lerp(Color, FLinearColor::White, 0.25f))
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
						[
							SNew(SBox)
							.Visibility(TeamRing >= 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
							[
								MakeTeamTag(TeamRing >= 0 ? TeamRing : 0, 12.f)
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
						[
							SNew(SBox)
							.Visibility(bYou ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
							[
								MakeTag(LOCTEXT("TagYouResults", "YOU"), Style.Gold, Style.Ink)
							]
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						StatColumn(FText::AsNumber(Score.Wins), Style.Gold)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						StatColumn(FText::AsNumber(Score.KOs), Style.Snow)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						StatColumn(FText::AsNumber(Score.Falls), Style.Ice)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						StatColumn(FText::AsNumber(Score.Hits), Style.Ice)
					]
				]
			]
		];
		++Rank;
	}
}

// ============================================================================ error popup + key capture card

TSharedRef<SWidget> SPinguRoot::BuildError()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedPtr<SPinguButton> OkButton;

	// The 3D error popup: the modal layer darkens the screen and holds the input layer (its OK is a 3D button: the Error overlay
	// page of APinguMenu3D; the message is flat text drawn by the root's SPinguMenu3DOverlay).
	if (PinguMenu3D::IsEnabled())
	{
		TSharedRef<SPinguMenu3DInput> Input = SNew(SPinguMenu3DInput).HUD(HUD);
		ErrorOk = Input;
		SAssignNew(ErrorLayer, SPinguModal)
		.Visibility_Lambda([this]() { return HUD.IsValid() && HUD->HasErrorPopup() ? EVisibility::Visible : EVisibility::Collapsed; })
		.FocusTarget_Lambda([this]() { return ErrorOk; })
		[
			Input // (the darkening behind the popup is the 3D menu's own dim plane)
		];
		return ErrorLayer.ToSharedRef();
	}
	TSharedRef<SWidget> Card = MakeCard(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeSection(LOCTEXT("ErrorTitle", "ONLINE"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return HUD.IsValid() ? HUD->GetNetError() : FText::GetEmpty(); })
			.Font(FPinguStyle::Body(22.f))
			.ColorAndOpacity(Style.Snow)
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Center)
		.Padding(0.f, 24.f, 0.f, 0.f)
		[
			SAssignNew(OkButton, SPinguButton)
			.Kind(EPinguButtonKind::Primary)
			.Width(200.f)
			.Text(LOCTEXT("ErrorOk", "OK"))
			.HUD(HUD)
			.OnClicked_Lambda([this]() { if (HUD.IsValid()) { HUD->Action_DismissError(); } })
		],
		FMargin(40.f, 24.f, 40.f, 32.f), 576.f);
	ErrorOk = OkButton;

	SAssignNew(ErrorLayer, SPinguModal)
	.Visibility_Lambda([this]() { return HUD.IsValid() && HUD->HasErrorPopup() ? EVisibility::Visible : EVisibility::Collapsed; })
	.FocusTarget_Lambda([this]() { return ErrorOk; })
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(&Style.WhiteBrush).ColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.6f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			Card
		]
	];
	return ErrorLayer.ToSharedRef();
}

TSharedRef<SWidget> SPinguRoot::BuildCapture()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	// The 3D PRESS A KEY card is the Capture overlay page of APinguMenu3D: this layer only darkens the screen and takes the keys.
	if (PinguMenu3D::IsEnabled())
	{
		SAssignNew(CaptureLayer, SPinguKeyCapture)
		.HUD(HUD)
		[
			SNew(SSpacer) // (the darkening is the 3D menu's own dim plane)
		];
		return CaptureLayer.ToSharedRef();
	}
	SAssignNew(CaptureLayer, SPinguKeyCapture)
	.HUD(HUD)
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(&Style.WhiteBrush).ColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.55f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MakeCard(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PressAKey", "PRESS A KEY"))
					.Font(FPinguStyle::Title(48.f, 4))
					.ColorAndOpacity(Style.Gold)
					.ShadowOffset(FVector2D(0.f, 4.f))
					.ShadowColorAndOpacity(PinguUI::WithAlpha(Style.Ink, 0.6f))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.f, 8.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return HUD.IsValid() ? HUD->GetCaptureMessage() : FText::GetEmpty(); })
					.Font(FPinguStyle::Body(20.f))
					.ColorAndOpacity(Style.Snow)
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.f, 16.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CaptureHint", "Keyboard keys and mouse buttons.  ESC cancels,  BACKSPACE clears."))
					.Font(FPinguStyle::Body(16.f))
					.ColorAndOpacity(Style.IceDim)
				],
				FMargin(40.f, 24.f, 40.f, 32.f), 640.f)
		]
	];
	return CaptureLayer.ToSharedRef();
}

#undef LOCTEXT_NAMESPACE

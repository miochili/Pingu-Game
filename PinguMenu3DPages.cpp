// Pingu - the 3D menu's pages that are not the title or the lobby strip (PinguMenu3D.h): the pause menu, the LAN screen, the
// results, the round board between rounds, the PRESS A KEY card and the ONLINE error popup. Each one is today's Slate layout
// (PinguUIMenus.cpp / PinguUIHud.cpp) as a framed wooden board (2026-10-26; before: a navy plane) with the same buttons as
// wooden signs in front of it, 3D Titan One labels for the short texts, and dynamic rows (found games, standings, the board's
// rows: dark wooden strips) rebuilt when their data changes.
// The settings and controls pages: PinguMenu3DSettings.cpp; the PINGUPEDIA: PinguMenu3DPedia.cpp.

#include "PinguMenu3D.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PinguNetSubsystem.h"
#include "PinguUI.h"
#include "Components/StaticMeshComponent.h"
#include "Text3DComponent.h"

#define LOCTEXT_NAMESPACE "PinguMenu3D"

namespace PinguMenu3DPagesPrivate
{
	/** "Waiting..." with 0-3 dots (only the end of the text moves). */
	static FString Dots()
	{
		static const TCHAR* Steps[] = { TEXT(""), TEXT("."), TEXT(".."), TEXT("...") };
		return Steps[static_cast<int32>(PinguUI::Now() * 3.0) % 4];
	}

	/** Rows and decorations of a page stand out of its plane by this much (less than the buttons: a quiet relief). */
	constexpr float RowRelief = 6.f;
	/** The results card. */
	constexpr float ResultsWidth = 880.f;
	constexpr float StatWidth = 80.f;
	/** The board between rounds: up to this many rows at full size, more get compact rows; a pip per win up to this many. */
	constexpr int32 BoardFullRows = 5;
	constexpr int32 BoardMaxPips = 10;
	constexpr float FreshWinDelay = 0.4f;
}

// ============================================================================ pause

void APinguMenu3D::BuildPause()
{
	using namespace PinguMenu3DPagesPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	auto Network = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsNetworkGame(); };
	auto Authority = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->HasMatchAuthority(); };
	auto Act = [WeakHUD](void (APenguinHUD::*Action)())
	{
		return [WeakHUD, Action]() { if (APenguinHUD* PenguinHUD = WeakHUD.Get()) { (PenguinHUD->*Action)(); } };
	};
	// Today's pause card: 456 wide; PAUSED (LAN: MENU), then RESUME / END MATCH / SETTINGS, and out: CLOSE GAME / LEAVE GAME
	// (LAN) or MAIN MENU (solo). The buttons stack: a hidden one takes no room.
	constexpr float Width = 456.f;
	constexpr float ButtonWidth = Width - 80.f;
	BeginPage(Pause, EPinguMenu3DPage::Pause, FVector2D(Width, 420.f));
	Pause.bStack = true;
	Pause.StackPlaneTop = 210.f;
	Pause.StackTop = Pause.StackPlaneTop - 24.f - 45.f - 24.f;
	Pause.StackBottom = 32.f;
	Pause.DefaultFocus = TEXT("Resume");
	AddLabel(Pause, nullptr, FVector2D(0.f, Pause.StackPlaneTop - 24.f - 22.f), 45.f,
		[Network] { return Network() ? LOCTEXT("PauseLanMenu", "MENU") : LOCTEXT("PausePaused", "PAUSED"); }, Style.Snow,
		PinguMenu3D::ETextAlign::Center, Width - 64.f, true, 10.f);
	FButton& Resume = AddButton(Pause, TEXT("Resume"), EPinguMenu3DKind::Primary, FVector2D(0.f, 0.f), FVector2D(ButtonWidth, 56.f), 20.f,
		[] { return LOCTEXT("PauseResume", "RESUME"); }, Act(&APenguinHUD::Action_Resume));
	Resume.StackGap = 0.f;
	FButton& End = AddButton(Pause, TEXT("EndMatch"), EPinguMenu3DKind::Normal, FVector2D(0.f, 0.f), FVector2D(ButtonWidth, 56.f), 20.f,
		[] { return LOCTEXT("PauseEndMatch", "END MATCH"); }, Act(&APenguinHUD::Action_EndMatch));
	End.StackGap = 14.f; // (room for the snow on top of each sign, style 5)
	End.Visible = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->CanEndMatch(); };
	FButton& SettingsButton = AddButton(Pause, TEXT("Settings"), EPinguMenu3DKind::Normal, FVector2D(0.f, 0.f), FVector2D(ButtonWidth, 56.f), 20.f,
		[] { return LOCTEXT("PauseSettings", "SETTINGS"); }, Act(&APenguinHUD::Action_Settings));
	SettingsButton.StackGap = 14.f;
	// The host's KICK buttons (2026-11-07; the lobby's red x is the same kick): one per joiner, only while there is one.
	// A plain kick: "The host kicked you." on their screen, they can join again at once.
	for (int32 Joiner = 0; Joiner < PinguMaxPenguins - 1; ++Joiner)
	{
		FButton& Kick = AddButton(Pause, FName(*FString::Printf(TEXT("Kick%d"), Joiner)), EPinguMenu3DKind::Danger, FVector2D(0.f, 0.f), FVector2D(ButtonWidth, 40.f), 15.f,
			[WeakHUD, Joiner]
			{
				const APenguinHUD* H = WeakHUD.Get();
				const TArray<FPinguLobbyPlayer> Targets = H ? H->GetKickTargets() : TArray<FPinguLobbyPlayer>();
				return Targets.IsValidIndex(Joiner) ? FText::Format(LOCTEXT("PauseKick", "KICK {0}"), FText::FromString(Targets[Joiner].Name.ToUpper())) : FText::GetEmpty();
			},
			[WeakHUD, Joiner]
			{
				if (APenguinHUD* H = WeakHUD.Get())
				{
					const TArray<FPinguLobbyPlayer> Targets = H->GetKickTargets();
					if (Targets.IsValidIndex(Joiner))
					{
						H->Action_KickPlayer(Targets[Joiner].PlayerId);
					}
				}
			});
		Kick.StackGap = 6.f;
		Kick.Visible = [WeakHUD, Joiner] { const APenguinHUD* H = WeakHUD.Get(); return H && H->GetKickTargets().IsValidIndex(Joiner); };
	}
	FButton& Leave = AddButton(Pause, TEXT("Leave"), EPinguMenu3DKind::Danger, FVector2D(0.f, 0.f), FVector2D(ButtonWidth, 44.f), 16.f,
		[Authority] { return Authority() ? LOCTEXT("PauseCloseGame", "CLOSE GAME") : LOCTEXT("PauseLeaveGame", "LEAVE GAME"); },
		Act(&APenguinHUD::Action_LeaveGame));
	Leave.StackGap = 16.f;
	Leave.Visible = Network;
	FButton& Main = AddButton(Pause, TEXT("MainMenu"), EPinguMenu3DKind::Ghost, FVector2D(0.f, 0.f), FVector2D(ButtonWidth, 44.f), 16.f,
		[] { return LOCTEXT("PauseMainMenu", "MAIN MENU"); }, Act(&APenguinHUD::Action_MainMenu));
	Main.StackGap = 16.f;
	Main.Visible = [Network] { return !Network(); };
	EndStatic(Pause);
}

// ============================================================================ LAN

void APinguMenu3D::BuildLan()
{
	using namespace PinguMenu3DPagesPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	auto Connecting = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsConnecting(); };
	auto Searching = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsSearching(); };
	// Today's ONLINE card: 760 wide, 40 of padding; YOUR NAME + a field, HOST GAME, the games on the network (four rows of
	// room), JOIN BY ADDRESS + a field and JOIN, "Connecting to ...", BACK / CANCEL.
	constexpr float Width = 760.f;
	constexpr float Inner = Width - 80.f;
	constexpr float Left = -0.5f * Inner;
	constexpr float Right = 0.5f * Inner;
	constexpr float Height = 796.f;
	BeginPage(Lan, EPinguMenu3DPage::Lan, FVector2D(Width, Height));
	Lan.DefaultFocus = TEXT("Host");
	float Y = 24.f; // from the top edge, down
	auto Z = [Height](float FromTop) { return 0.5f * Height - FromTop; };

	AddLabel(Lan, nullptr, FVector2D(Left, Z(Y + 26.f)), 42.f, [] { return LOCTEXT("LanTitle", "ONLINE"); }, Style.Snow, PinguMenu3D::ETextAlign::Left, 0.f, true, 10.f);
	Y += 52.f + 8.f;
	// Your name.
	AddLabel(Lan, nullptr, FVector2D(Left, Z(Y + 24.f)), 14.f, [] { return LOCTEXT("LanYourName", "YOUR NAME"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	FButton& Name = AddField(Lan, TEXT("Name"), FVector2D(Left + 192.f + 0.5f * (Inner - 192.f), Z(Y + 24.f)), FVector2D(Inner - 192.f, 48.f),
		LOCTEXT("LanNameHint", "Your name"), 20,
		[WeakHUD](const FString& Text) { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_SetPlayerName(Text); } },
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_SetPlayerName(H->GetPlayerName()); } });
	Name.FieldText = HUD.IsValid() ? HUD->GetPlayerName() : FString();
	UpdateField(Name, false);
	Y += 48.f + 24.f;
	// Host.
	FButton& Host = AddButton(Lan, TEXT("Host"), EPinguMenu3DKind::Primary, FVector2D(0.f, Z(Y + 28.f)), FVector2D(Inner, 56.f), 20.f,
		[] { return LOCTEXT("LanHostGame", "HOST GAME"); },
		[this, WeakHUD] { CommitFields(); if (APenguinHUD* H = WeakHUD.Get()) { H->Action_HostGame(); } });
	Host.Enabled = [Connecting] { return !Connecting(); };
	Y += 56.f + 32.f;
	// Found games.
	AddLabel(Lan, nullptr, FVector2D(Left, Z(Y + 13.f)), 14.f, [] { return LOCTEXT("LanFoundGames", "GAMES ON YOUR NETWORK"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	{
		const int32 SearchLabel = AddLabel(Lan, nullptr, FVector2D(Right, Z(Y + 13.f)), 13.f,
			[] { return FText::Format(LOCTEXT("LanSearching", "SEARCHING{0}"), FText::FromString(Dots())); }, Style.Ice, PinguMenu3D::ETextAlign::Right);
		Lan.Labels[SearchLabel].Visible = Searching;
	}
	Y += 26.f + 8.f;
	// The rows' room (four rows of 56 with 8 between, and a line for "+ N more"): the rows are built in UpdateLan.
	Lan.ListTop = Z(Y + 28.f);
	Lan.ListStep = 64.f;
	{
		// The empty well: "No games yet" / "Not searching".
		const int32 Well = AddDecoWood(Lan, nullptr, FVector2D(0.f, Z(Y + 28.f)), FVector2D(Inner, 56.f), 8.f, 4.f, PinguMenu3D::WoodWell, PinguMenu3D::WoodWellLip, 2.f, 0.6f);
		Lan.Decos[Well].Visible = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->GetFoundGames().Num() == 0; };
		const int32 Empty = AddLabel(Lan, nullptr, FVector2D(0.f, Z(Y + 28.f)), 13.f,
			[Searching] { return Searching() ? LOCTEXT("LanNoGamesYet", "No games yet") : LOCTEXT("LanNotSearching", "Not searching"); }, Style.IceDim,
			PinguMenu3D::ETextAlign::Center, 0.f, false, 4.f);
		Lan.Labels[Empty].Visible = Lan.Decos[Well].Visible;
		const int32 More = AddLabel(Lan, nullptr, FVector2D(Left, Z(Y + 4.f * 64.f + 4.f)), 12.f,
			[WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return FText::Format(LOCTEXT("LanMoreGames", "+ {0} more"), FText::AsNumber(H ? FMath::Max(H->GetFoundGames().Num() - 4, 0) : 0)); },
			Style.IceDim, PinguMenu3D::ETextAlign::Left);
		Lan.Labels[More].Visible = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->GetFoundGames().Num() > 4; };
	}
	Y += 4.f * 64.f + 8.f + 24.f;
	// Join by address.
	AddLabel(Lan, nullptr, FVector2D(Left, Z(Y + 13.f)), 14.f, [] { return LOCTEXT("LanJoinByAddress", "JOIN BY ADDRESS"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	Y += 26.f + 8.f;
	{
		const float FieldWidth = Inner - 8.f - 160.f;
		FButton& Address = AddField(Lan, TEXT("Address"), FVector2D(Left + 0.5f * FieldWidth, Z(Y + 24.f)), FVector2D(FieldWidth, 48.f),
			LOCTEXT("LanAddressHint", "192.168.1.20"), 64, nullptr,
			[this, WeakHUD]
			{
				// Enter in the address field: join it (the sound as the Slate box played).
				CommitFields();
				APenguinHUD* H = WeakHUD.Get();
				if (!H || !Lan.bBuilt)
				{
					return;
				}
				for (const FButton& Button : Lan.Buttons)
				{
					if (Button.Id == FName(TEXT("Address")))
					{
						H->Action_JoinGame(Button.FieldText);
					}
				}
			});
		(void)Address;
		FButton& Join = AddButton(Lan, TEXT("Join"), EPinguMenu3DKind::Normal, FVector2D(Right - 80.f, Z(Y + 24.f)), FVector2D(160.f, 48.f), 17.f,
			[] { return LOCTEXT("LanJoin", "JOIN"); },
			[this, WeakHUD]
			{
				CommitFields();
				APenguinHUD* H = WeakHUD.Get();
				if (!H || !Lan.bBuilt)
				{
					return;
				}
				for (const FButton& Button : Lan.Buttons)
				{
					if (Button.Id == FName(TEXT("Address")))
					{
						H->Action_JoinGame(Button.FieldText);
					}
				}
			});
		Join.Enabled = [Connecting] { return !Connecting(); };
	}
	Y += 48.f + 16.f;
	// Status: joining.
	{
		const int32 Status = AddLabel(Lan, nullptr, FVector2D(Left, Z(Y + 12.f)), 15.f,
			[WeakHUD]
			{
				const APenguinHUD* H = WeakHUD.Get();
				return FText::Format(LOCTEXT("LanConnecting", "Connecting to {0}{1}"), FText::FromString(H ? H->GetJoinTarget() : FString()), FText::FromString(Dots()));
			}, Style.Gold, PinguMenu3D::ETextAlign::Left, Inner);
		Lan.Labels[Status].Visible = Connecting;
	}
	Y += 24.f + 24.f;
	// Back (cancels a join under way).
	AddButton(Lan, TEXT("Back"), EPinguMenu3DKind::Ghost, FVector2D(Left + 100.f, Z(Y + 22.f)), FVector2D(200.f, 44.f), 16.f,
		[Connecting] { return Connecting() ? LOCTEXT("LanCancelJoin", "CANCEL") : LOCTEXT("LanBack", "BACK"); },
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Back(); } });
	EndStatic(Lan);
}

void APinguMenu3D::UpdateLan(FPageParts& Parts)
{
	using namespace PinguMenu3DPagesPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return;
	}
	// The name field follows the HUD's name when the page opens (the field keeps what is typed after that).
	for (FButton& Button : Parts.Buttons)
	{
		if (Button.Id == FName(TEXT("Name")) && Parts.Appear <= 0.f)
		{
			Button.FieldText = PenguinHUD->GetPlayerName();
		}
	}
	// The found games' rows: rebuilt only when games come or go (their texts follow the beacon by themselves).
	const TArray<FPinguLanGame>& Games = PenguinHUD->GetFoundGames();
	constexpr int32 MaxShown = 4;
	FString Key = FString::Printf(TEXT("#%d"), Games.Num());
	for (int32 Index = 0; Index < FMath::Min(Games.Num(), MaxShown); ++Index)
	{
		Key += TEXT("|") + Games[Index].Address;
	}
	if (Key == Parts.RowsKey)
	{
		return;
	}
	ClearRows(Parts);
	Parts.RowsKey = Key;
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	constexpr float Inner = 680.f;
	for (int32 Index = 0; Index < FMath::Min(Games.Num(), MaxShown); ++Index)
	{
		const FString Address = Games[Index].Address;
		// The live entry of this game (by address), or null once it is gone.
		auto FindGame = [WeakHUD, Address]() -> const FPinguLanGame*
		{
			const APenguinHUD* InnerHUD = WeakHUD.Get();
			return InnerHUD ? InnerHUD->GetFoundGames().FindByPredicate([&Address](const FPinguLanGame& Game) { return Game.Address == Address; }) : nullptr;
		};
		const FVector2D Center(0.f, Parts.ListTop - Index * Parts.ListStep);
		const int32 ButtonIndex = Parts.Buttons.Num();
		{
			FButton& Row = AddButton(Parts, FName(*FString::Printf(TEXT("Game%d"), Index)), EPinguMenu3DKind::ListRow, Center, FVector2D(Inner, 56.f), 17.f,
				[FindGame] { const FPinguLanGame* Game = FindGame(); return FText::Format(LOCTEXT("LanGameOf", "{0}'S GAME"), PinguUI::Upper(Game ? Game->HostName : FString())); },
				[this, WeakHUD, Address] { CommitFields(); if (APenguinHUD* H = WeakHUD.Get()) { H->Action_JoinFound(Address); } });
			Row.Enabled = [FindGame, WeakHUD]
			{
				// A running match is fine (you play from its next round); a full game is not.
				const FPinguLanGame* Game = FindGame();
				const APenguinHUD* H = WeakHUD.Get();
				return Game && Game->Players < Game->MaxPlayers && H && !H->IsConnecting();
			};
		}
		USceneComponent* Body = Parts.Buttons[ButtonIndex].Body;
		// "2/10 PLAYERS" after the name's room, then on the right: FULL, or [IN MATCH] JOIN >.
		AddLabel(Parts, Body, FVector2D(0.5f * Inner - 16.f - 208.f - 16.f, 0.f), 13.f,
			[FindGame]
			{
				const FPinguLanGame* Game = FindGame();
				return FText::Format(LOCTEXT("LanGamePlayers", "{0}/{1} PLAYERS"), FText::AsNumber(Game ? Game->Players : 0), FText::AsNumber(Game ? Game->MaxPlayers : PinguMaxPenguins));
			}, Style.Ice, PinguMenu3D::ETextAlign::Right, 0.f, false, 1.f);
		const FPinguLanGame& Game = Games[Index];
		const bool bFull = Game.Players >= Game.MaxPlayers;
		if (bFull)
		{
			AddPill(Parts, Body, FVector2D(0.5f * Inner - 16.f, 0.f), 24.f, 11.f, LOCTEXT("LanFull", "FULL"), Style.Red, Style.Snow, 1.f, PinguMenu3D::ETextAlign::Right);
		}
		else
		{
			// JOIN > in gold (the arrow a small label), IN MATCH before it for a running game.
			const int32 Arrow = AddLabel(Parts, Body, FVector2D(0.5f * Inner - 16.f, 0.f), 13.f, [] { return FText::FromString(TEXT(">")); }, Style.Gold,
				PinguMenu3D::ETextAlign::Right, 0.f, false, 1.f);
			const float ArrowWidth = PinguMenu3D::GetTextWidth(Parts.Labels[Arrow].Text);
			const int32 JoinLabel = AddLabel(Parts, Body, FVector2D(0.5f * Inner - 16.f - ArrowWidth - 8.f, 0.f), 16.f, [] { return LOCTEXT("LanJoinShort", "JOIN"); }, Style.Gold,
				PinguMenu3D::ETextAlign::Right, 0.f, false, 1.f);
			if (Game.bInMatch)
			{
				const float JoinWidth = PinguMenu3D::GetTextWidth(Parts.Labels[JoinLabel].Text);
				AddPill(Parts, Body, FVector2D(0.5f * Inner - 16.f - ArrowWidth - 8.f - JoinWidth - 12.f, 0.f), 24.f, 11.f, LOCTEXT("LanInMatch", "IN MATCH"), Style.Ice, Style.Ink, 1.f,
					PinguMenu3D::ETextAlign::Right);
			}
		}
		Parts.Buttons[ButtonIndex].LabelColor = [] { return FPinguStyle::Get().Snow; };
		if (Parts.bShown)
		{
			SetButtonVisible(Parts.Buttons[ButtonIndex], true);
		}
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] LAN rows rebuilt: %d game(s)"), Games.Num());
}

// ============================================================================ results

void APinguMenu3D::BuildResults()
{
	using namespace PinguMenu3DPagesPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	auto Authority = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->HasMatchAuthority(); };
	// Today's results card: 880 wide; VICTORY! / X WINS! / MATCH OVER, the standings, REMATCH + LOBBY (the host / solo) or
	// "Waiting for <host>..." + LEAVE (a client). The rows and the height come with the scores (UpdateResults).
	BeginPage(Results, EPinguMenu3DPage::Results, FVector2D(ResultsWidth, 500.f));
	Results.DefaultFocus = TEXT("Rematch");
	Results.StackPlaneTop = 250.f;
	// The title: gold when this PC's player (or their team) won. A long name gets a smaller size: it always fits the card.
	AddLabel(Results, nullptr, FVector2D(0.f, Results.StackPlaneTop - 24.f - 32.f), 60.f,
		[WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H ? H->GetResultsTitle() : FText::GetEmpty(); }, Style.Snow,
		PinguMenu3D::ETextAlign::Center, ResultsWidth - 96.f - 24.f, true, 10.f);
	Results.Labels.Last().Color = [WeakHUD]
	{
		const APenguinHUD* H = WeakHUD.Get();
		bool bLocalWin = false;
		if (H && H->IsTeamMatch())
		{
			const FPinguScore* Local = H->GetScores().FindByPredicate([H](const FPinguScore& Score) { return H->IsLocalScore(Score); });
			bLocalWin = Local && H->GetMatchWinnerTeam() >= 0 && Local->Team == H->GetMatchWinnerTeam();
		}
		else if (H)
		{
			const FPinguScore* Winner = H->FindScoreBySlot(H->GetMatchWinnerSlot());
			bLocalWin = Winner && H->IsLocalScore(*Winner);
		}
		return bLocalWin ? FPinguStyle::Get().Gold : FPinguStyle::Get().Snow;
	};
	// The column labels (the rows go under them).
	const float RightEdge = 0.5f * ResultsWidth - 48.f - 24.f;
	const FText Columns[4] = { LOCTEXT("ResultsColWins", "WINS"), LOCTEXT("ResultsColKOs", "KOS"), LOCTEXT("ResultsColFalls", "FALLS"), LOCTEXT("ResultsColHits", "HITS") };
	for (int32 Column = 0; Column < 4; ++Column)
	{
		const FText Text = Columns[Column];
		AddLabel(Results, nullptr, FVector2D(RightEdge - (3 - Column) * StatWidth - 0.5f * StatWidth, Results.StackPlaneTop - 24.f - 80.f - 24.f - 10.f), 13.f,
			[Text] { return Text; }, Style.IceDim);
	}
	// Solo / host: choose. Clients: wait (or leave). Placed by UpdateResults under the rows.
	FButton& Rematch = AddButton(Results, TEXT("Rematch"), EPinguMenu3DKind::Primary, FVector2D(-136.f, 0.f), FVector2D(256.f, 56.f), 20.f,
		[] { return LOCTEXT("ResultsRematch", "REMATCH"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Rematch(); } });
	Rematch.Visible = Authority;
	FButton& ToLobby = AddButton(Results, TEXT("Lobby"), EPinguMenu3DKind::Normal, FVector2D(136.f, 0.f), FVector2D(256.f, 56.f), 20.f,
		[] { return LOCTEXT("ResultsToLobby", "LOBBY"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ReturnToLobby(); } });
	ToLobby.Visible = Authority;
	FButton& Leave = AddButton(Results, TEXT("Leave"), EPinguMenu3DKind::Danger, FVector2D(0.f, 0.f), FVector2D(256.f, 56.f), 20.f,
		[] { return LOCTEXT("ResultsLeave", "LEAVE"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_LeaveGame(); } });
	Leave.Visible = [Authority] { return !Authority(); };
	const int32 Waiting = AddLabel(Results, nullptr, FVector2D(0.f, 0.f), 15.f,
		[WeakHUD]
		{
			const APenguinHUD* H = WeakHUD.Get();
			return FText::Format(LOCTEXT("ResultsWaitingChoice", "Waiting for {0}{1}"), FText::FromString(H ? H->GetHostName() : FString()), FText::FromString(Dots()));
		}, Style.Gold, PinguMenu3D::ETextAlign::Center, ResultsWidth - 96.f);
	Results.Labels[Waiting].Visible = [Authority] { return !Authority(); };
	EndStatic(Results);
}

void APinguMenu3D::UpdateResults(FPageParts& Parts)
{
	using namespace PinguMenu3DPagesPrivate;
	using namespace PinguMenu3D;
	const FPinguStyle& Style = FPinguStyle::Get();
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return;
	}
	// The match is over: now every statistic (between rounds the board showed only the wins). Rebuilt when the standings change.
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
			return TeamOrder.IndexOfByKey(Scores[A].Team) < TeamOrder.IndexOfByKey(Scores[B].Team);
		});
	}
	const int32 MatchWinner = PenguinHUD->GetMatchWinnerSlot();
	const int32 WinnerTeam = PenguinHUD->GetMatchWinnerTeam();
	const bool bAuthority = PenguinHUD->HasMatchAuthority();
	FString Key = FString::Printf(TEXT("%d:%d:%d:%d:"), MatchWinner, WinnerTeam, bTeams ? 1 : 0, bAuthority ? 1 : 0);
	for (const int32 Index : Ranking)
	{
		if (Scores.IsValidIndex(Index))
		{
			const FPinguScore& Score = Scores[Index];
			Key += FString::Printf(TEXT("%s/%d/%d/%d/%d/%d/%d|"), *Score.Name, Score.Slot, Score.Wins, Score.KOs, Score.Falls, Score.Hits, Score.Team);
		}
	}
	if (Key == Parts.RowsKey)
	{
		return;
	}
	ClearRows(Parts);
	Parts.RowsKey = Key;

	// More than six penguins: compact rows (ten fit on the card).
	const bool bCompact = Ranking.Num() > 6;
	const float RowHeight = bCompact ? 44.f : 58.f;
	const float RowGap = bCompact ? 4.f : 8.f;
	const float NameSize = bCompact ? 17.f : 21.f;
	const float StatSize = bCompact ? 17.f : 19.f;
	const float Portrait = bCompact ? 34.f : 44.f;
	// (Positions on a row: from its middle; the row spans the card's inside, 784 wide.)
	const float RowLeft = -0.5f * (ResultsWidth - 96.f);
	const float RightEdge = 0.5f * (ResultsWidth - 96.f) - 24.f;
	float Y = 24.f + 80.f + 24.f + 20.f + 6.f; // under the title and the column labels
	int32 Rank = 0;
	for (const int32 Index : Ranking)
	{
		if (!Scores.IsValidIndex(Index))
		{
			continue;
		}
		const FPinguScore& Score = Scores[Index];
		const FLinearColor Color = APenguinCharacter::GetSlotColor(PenguinHUD->GetScoreColorIndex(Score));
		const bool bWinner = bTeams ? (WinnerTeam >= 0 && Score.Team == WinnerTeam) : Score.Slot == MatchWinner;
		const bool bYou = PenguinHUD->IsNetworkGame() && PenguinHUD->IsLocalScore(Score);
		const int32 TeamRing = PenguinHUD->GetScoreTeamRing(Score);
		// A little more room where the next team starts.
		const bool bNewTeam = bTeams && Rank > 0 && Scores.IsValidIndex(Ranking[Rank - 1]) && Scores[Ranking[Rank - 1]].Team != Score.Team;
		Y += Rank > 0 ? RowGap + (bNewTeam ? 6.f : 0.f) : 0.f;
		const float RowZ = Parts.StackPlaneTop - Y - 0.5f * RowHeight;
		// The row: a dark slab (the winner's with a gold rim), the rank, the portrait, the name (+ team / YOU tags), the numbers.
		const int32 RowDeco = AddDecoWood(Parts, nullptr, FVector2D(0.f, RowZ), FVector2D(ResultsWidth - 96.f, RowHeight), 8.f, 6.f, PinguMenu3D::WoodRow, PinguMenu3D::WoodRowLip, RowRelief, 0.75f);
		USceneComponent* RowRoot = Parts.Decos[RowDeco].Root;
		if (bWinner)
		{
			AddDecoRing(Parts, RowRoot, FVector2D(0.f, 0.f), FVector2D(ResultsWidth - 96.f, RowHeight), 14.f, 0.f, Style.Gold, FMath::Lerp(Style.Gold, Style.Ink, 0.5f), 0.f);
		}
		AddLabel(Parts, RowRoot, FVector2D(RowLeft + 12.f + 20.f, 0.f), bCompact ? 21.f : 24.f, [Rank] { return FText::AsNumber(Rank + 1); },
			Rank == 0 ? Style.Gold : Style.Ice, PinguMenu3D::ETextAlign::Center, 0.f, false, 1.f);
		AddPortrait(Parts, RowRoot, FVector2D(RowLeft + 12.f + 40.f + 8.f + 0.5f * Portrait, 0.f), Portrait, Score.Slot, TeamRing, 1.f);
		const float NameLeft = RowLeft + 12.f + 40.f + 8.f + Portrait + 16.f;
		const float NameRoom = RightEdge - 4.f * StatWidth - 8.f - NameLeft - (TeamRing >= 0 ? 90.f : 0.f) - (bYou ? 60.f : 0.f);
		const FString NameString = PinguUI::Upper(Score.Name).ToString();
		const int32 NameLabel = AddLabel(Parts, RowRoot, FVector2D(NameLeft, 0.f), NameSize, [NameString] { return FText::FromString(NameString); },
			FMath::Lerp(Color, FLinearColor::White, 0.25f), PinguMenu3D::ETextAlign::Left, NameRoom, false, 1.f);
		float TagX = NameLeft + PinguMenu3D::GetTextWidth(Parts.Labels[NameLabel].Text) + 8.f;
		if (TeamRing >= 0)
		{
			TagX += AddPill(Parts, RowRoot, FVector2D(TagX, 0.f), 22.f, 10.f, APenguinCharacter::GetTeamName(TeamRing), APenguinCharacter::GetTeamColor(TeamRing), Style.Ink, 1.f) + 8.f;
		}
		if (bYou)
		{
			AddPill(Parts, RowRoot, FVector2D(TagX, 0.f), 22.f, 10.f, LOCTEXT("ResultsTagYou", "YOU"), Style.Gold, Style.Ink, 1.f);
		}
		const int32 Values[4] = { Score.Wins, Score.KOs, Score.Falls, Score.Hits };
		const FLinearColor ValueColors[4] = { Style.Gold, Style.Snow, Style.Ice, Style.Ice };
		for (int32 Column = 0; Column < 4; ++Column)
		{
			const int32 Value = Values[Column];
			AddLabel(Parts, RowRoot, FVector2D(RightEdge - (3 - Column) * StatWidth - 0.5f * StatWidth, 0.f), StatSize, [Value] { return FText::AsNumber(Value); },
				ValueColors[Column], PinguMenu3D::ETextAlign::Center, 0.f, false, 1.f);
		}
		Y += RowHeight;
		++Rank;
	}
	// The buttons under the rows, then the plane's height.
	Y += 24.f;
	const float ButtonZ = Parts.StackPlaneTop - Y - 28.f;
	for (FButton& Button : Parts.Buttons)
	{
		if (Button.Id == FName(TEXT("Rematch")) || Button.Id == FName(TEXT("Lobby")))
		{
			Button.Center.Y = ButtonZ;
		}
		else if (Button.Id == FName(TEXT("Leave")))
		{
			Button.Center.Y = ButtonZ - 24.f - 16.f;
		}
		Button.Root->SetRelativeLocation(FVector(0.f, Button.Center.X, Button.Center.Y));
	}
	for (FLabel& Label : Parts.Labels)
	{
		if (Label.Text && Label.Label && Label.Visible && !bAuthority && &Label == &Parts.Labels[Parts.StaticLabels - 1])
		{
			Label.Text->SetRelativeLocation(FVector(Label.Text->GetRelativeLocation().X, 0.f, ButtonZ + 16.f - 7.5f));
			if (Label.Ink)
			{
				Label.Ink->SetRelativeLocation(FVector(Label.Ink->GetRelativeLocation().X, 0.f, ButtonZ + 16.f - 7.5f));
			}
		}
	}
	Y += 56.f + (bAuthority ? 0.f : 24.f + 16.f) + 32.f;
	if (Parts.Plane.IsBuilt())
	{
		SetPlaneShape(Parts, ResultsWidth, Y);
		Parts.Plane.Root->SetRelativeLocation(FVector(0.f, 0.f, Parts.StackPlaneTop - 0.5f * Y));
	}
	// The card centred on the picture.
	Parts.PlaceOffset = FVector2D(0.f, -(Parts.StackPlaneTop - 0.5f * Y));
	if (Parts.bShown)
	{
		for (int32 Index = Parts.StaticButtons; Index < Parts.Buttons.Num(); ++Index)
		{
			SetButtonVisible(Parts.Buttons[Index], !Parts.Buttons[Index].Visible || Parts.Buttons[Index].Visible());
		}
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] results rows rebuilt: %d rows, card %.0f high"), Rank, Y);
}

// ============================================================================ the round board

void APinguMenu3D::BuildBoard()
{
	using namespace PinguMenu3DPagesPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	// The leaderboard between rounds: a card under the winner banner with a row per penguin (a team match: per team), its
	// wins as pips; "PRESS ANY KEY TO CONTINUE" once a key takes us on. The rows come with the scores (UpdateBoard).
	BeginPage(Board, EPinguMenu3DPage::Board, FVector2D(600.f, 300.f));
	Board.StackPlaneTop = 150.f;
	Board.PlaceOffset = FVector2D(0.f, -60.f);
	const int32 Hint = AddLabel(Board, nullptr, FVector2D(0.f, 0.f), 16.f, [] { return LOCTEXT("BoardPressAnyKey", "PRESS ANY KEY TO CONTINUE"); }, Style.Gold,
		PinguMenu3D::ETextAlign::Center, 0.f, true, 4.f);
	Board.Labels[Hint].Visible = [WeakHUD]
	{
		const APenguinHUD* H = WeakHUD.Get();
		const APenguinGameState* GameState = H ? H->GetGameState() : nullptr;
		return GameState && GameState->IsWaitingForContinue() && H->IsWaitingForContinueKey();
	};
	Board.Labels[Hint].Glow = [] { return 0.25f + 0.25f * FMath::Cos(static_cast<float>(PinguUI::Now()) * 4.f); };
	EndStatic(Board);
}

void APinguMenu3D::UpdateBoard(FPageParts& Parts)
{
	using namespace PinguMenu3DPagesPrivate;
	using namespace PinguMenu3D;
	const FPinguStyle& Style = FPinguStyle::Get();
	const APenguinHUD* PenguinHUD = HUD.Get();
	const APenguinGameState* GameState = PenguinHUD ? PenguinHUD->GetGameState() : nullptr;
	if (!PenguinHUD || !GameState)
	{
		return;
	}
	// Between rounds the board shows only the wins (the knock-outs and the rest wait for the results). A team match: a row
	// per team, its penguins' portraits and the team's wins. Rebuilt when the round, the line-up or a score changes.
	const TArray<FPinguScore>& Scores = PenguinHUD->GetScores();
	const bool bTeams = PenguinHUD->IsTeamMatch();
	const TArray<int32> Ranking = bTeams ? PenguinHUD->GetTeamBoard() : GameState->GetWinsBoard();
	const int32 RoundWinner = bTeams ? PenguinHUD->GetRoundWinnerTeam() : PenguinHUD->GetRoundWinnerSlot();
	const FPinguMatchSettings MatchSettings = PenguinHUD->GetMatchSettings();
	const bool bEndless = MatchSettings.bEndless;
	const int32 Rounds = FMath::Clamp(MatchSettings.RoundsToWin, 1, PinguMaxRoundsToWin);
	const bool bCount = bEndless || Rounds > BoardMaxPips;
	FString Key = FString::Printf(TEXT("%d:%d:%d:%d:%d:"), GameState->GetRoundNumber(), RoundWinner, bTeams ? 1 : 0, bEndless ? 1 : 0, Rounds);
	for (const int32 Index : Ranking)
	{
		Key += FString::Printf(TEXT("%d/%d|"), Index, bTeams ? PenguinHUD->GetTeamWinCount(Index) : (Scores.IsValidIndex(Index) ? Scores[Index].Wins : -1));
	}
	for (const FPinguScore& Score : Scores)
	{
		Key += FString::Printf(TEXT("%s.%d.%d,"), *Score.Name, Score.Slot, Score.Team);
	}
	if (Key == Parts.RowsKey)
	{
		return;
	}
	ClearRows(Parts);
	Parts.RowsKey = Key;

	// Up to five rows at full size; ten penguins get compact rows (the board stays on the screen).
	const bool bCompact = Ranking.Num() > BoardFullRows;
	const float RowHeight = bCompact ? 46.f : 68.f;
	const float RowGap = bCompact ? 4.f : 8.f;
	const float PortraitSize = bCompact ? 36.f : 52.f;
	const float NameSize = bCompact ? 17.f : 22.f;
	const float PipSize = bCompact ? 15.f : 18.f;
	const float WinsWidth = bEndless ? 80.f : (bCount ? 150.f : static_cast<float>(Rounds) * (PipSize + 6.f));
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
	const float WhoWidth = bTeams ? FMath::Max(TeamLabelWidth + 8.f + static_cast<float>(MostMembers) * MemberStep + YouTagRoom, PersonWhoWidth) : PersonWhoWidth;
	const float RowWidth = 8.f + 40.f + 8.f + WhoWidth + 8.f + WinsWidth + 20.f;
	const float Width = RowWidth + 32.f;
	const float Left = -0.5f * RowWidth;
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	float Y = 16.f;
	for (int32 Rank = 0; Rank < Ranking.Num(); ++Rank)
	{
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
		Y += Rank > 0 ? RowGap : 0.f;
		const float RowZ = Parts.StackPlaneTop - Y - 0.5f * RowHeight;
		const int32 RowDeco = AddDecoWood(Parts, nullptr, FVector2D(0.f, RowZ), FVector2D(RowWidth, RowHeight), 8.f, 6.f, PinguMenu3D::WoodRow, PinguMenu3D::WoodRowLip, RowRelief, 0.75f);
		USceneComponent* RowRoot = Parts.Decos[RowDeco].Root;
		float X = Left + 8.f;
		if (bTeams && Solo == INDEX_NONE)
		{
			// The team: its name in its colour, its penguins' portraits (a YOU tag where this PC's player is in it).
			const int32 Team = Ranking[Rank];
			Wins = PenguinHUD->GetTeamWinCount(Team);
			bWinner = Team == RoundWinner;
			AddLabel(Parts, RowRoot, FVector2D(X + 20.f, 0.f), bCompact ? 19.f : 24.f, [Rank] { return FText::AsNumber(Rank + 1); }, Rank == 0 ? Style.Gold : Style.Ice,
				PinguMenu3D::ETextAlign::Center, 0.f, false, 1.f);
			X += 40.f + 8.f;
			const FText TeamText = FText::Format(LOCTEXT("BoardTeam", "{0} TEAM"), APenguinCharacter::GetTeamName(Team));
			AddLabel(Parts, RowRoot, FVector2D(X, 0.f), NameSize, [TeamText] { return TeamText; }, FMath::Lerp(APenguinCharacter::GetTeamColor(Team), FLinearColor::White, 0.2f),
				PinguMenu3D::ETextAlign::Left, TeamLabelWidth, false, 1.f);
			X += TeamLabelWidth + 8.f;
			bool bYouHere = false;
			int32 Count = 0;
			for (int32 Index = 0; Index < Scores.Num(); ++Index)
			{
				if (Scores[Index].Team != Team)
				{
					continue;
				}
				bYouHere |= PenguinHUD->IsLocalScore(Scores[Index]);
				AddPortrait(Parts, RowRoot, FVector2D(X + 0.5f * (PortraitSize - 10.f), 0.f), PortraitSize - 10.f, Scores[Index].Slot, Team, 1.f);
				X += MemberStep;
				++Count;
			}
			X += static_cast<float>(MostMembers - Count) * MemberStep;
			if (bYouHere && PenguinHUD->IsNetworkGame())
			{
				AddPill(Parts, RowRoot, FVector2D(X + 8.f, 0.f), 22.f, 10.f, LOCTEXT("BoardTagYouTeam", "YOU"), Style.Gold, Style.Ink, 1.f);
			}
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
			AddLabel(Parts, RowRoot, FVector2D(X + 20.f, 0.f), bCompact ? 19.f : 24.f, [Rank] { return FText::AsNumber(Rank + 1); }, Rank == 0 ? Style.Gold : Style.Ice,
				PinguMenu3D::ETextAlign::Center, 0.f, false, 1.f);
			X += 40.f + 8.f;
			AddPortrait(Parts, RowRoot, FVector2D(X + 0.5f * PortraitSize, 0.f), PortraitSize, Score.Slot, -1, 1.f);
			X += PortraitSize + 16.f;
			const FString NameString = PinguUI::Upper(Score.Name).ToString();
			const int32 NameLabel = AddLabel(Parts, RowRoot, FVector2D(X, 0.f), NameSize, [NameString] { return FText::FromString(NameString); },
				FMath::Lerp(Color, FLinearColor::White, 0.25f), PinguMenu3D::ETextAlign::Left, PersonNameWidth - YouTagRoom, false, 1.f);
			if (PenguinHUD->IsNetworkGame() && PenguinHUD->IsLocalScore(Score))
			{
				AddPill(Parts, RowRoot, FVector2D(X + PinguMenu3D::GetTextWidth(Parts.Labels[NameLabel].Text) + 8.f, 0.f), 22.f, 10.f, LOCTEXT("BoardTagYou", "YOU"), Style.Gold, Style.Ink, 1.f);
			}
		}
		// The wins: a pip per round to win (gold = won); endless or first to more than 10: one gold pip and the count. The round
		// winner's new win fills in a moment after the board shows.
		const float WinsLeft = Left + 8.f + 40.f + 8.f + WhoWidth + 8.f;
		if (bCount)
		{
			AddDecoSlab(Parts, RowRoot, FVector2D(WinsLeft + 0.5f * (PipSize + 4.f), 0.f), FVector2D(PipSize + 4.f, PipSize + 4.f), 0.5f * (PipSize + 4.f), 5.f, Style.Gold,
				FMath::Lerp(Style.Gold, Style.Ink, 0.5f), 2.f);
			AddLabel(Parts, RowRoot, FVector2D(WinsLeft + PipSize + 4.f + 8.f, 0.f), NameSize,
				[WeakHUD, Wins, bWinner, bEndless, Rounds]
				{
					const APenguinHUD* H = WeakHUD.Get();
					const bool bFresh = bWinner && H && H->GetPhaseTime() - APenguinGameState::LeaderboardDelay < FreshWinDelay;
					const int32 Shown = bFresh ? FMath::Max(Wins - 1, 0) : Wins;
					return bEndless ? FText::AsNumber(Shown) : FText::Format(LOCTEXT("BoardWinsOf", "{0} / {1}"), FText::AsNumber(Shown), FText::AsNumber(Rounds));
				}, Style.Gold, PinguMenu3D::ETextAlign::Left, 0.f, false, 1.f);
		}
		else
		{
			for (int32 Pip = 0; Pip < Rounds; ++Pip)
			{
				const bool bFresh = bWinner && Pip == Wins - 1;
				const bool bWon = Pip < Wins;
				const int32 PipDeco = AddDecoSlab(Parts, RowRoot, FVector2D(WinsLeft + 3.f + Pip * (PipSize + 6.f) + 0.5f * PipSize, 0.f), FVector2D(PipSize, PipSize), 0.5f * PipSize, 5.f,
					bWon ? Style.Gold : PinguMenu3D::WoodEdge * 2.f, PinguMenu3D::WoodEdge, 2.f);
				if (bFresh)
				{
					Parts.Decos[PipDeco].Color = [WeakHUD]
					{
						const APenguinHUD* H = WeakHUD.Get();
						const bool bLit = H && H->GetPhaseTime() - APenguinGameState::LeaderboardDelay >= FreshWinDelay;
						return bLit ? FPinguStyle::Get().Gold : PinguMenu3D::WoodEdge * 2.f;
					};
				}
			}
		}
		if (bWinner)
		{
			AddDecoRing(Parts, RowRoot, FVector2D(0.f, 0.f), FVector2D(RowWidth, RowHeight), 14.f, 0.f, Style.Gold, FMath::Lerp(Style.Gold, Style.Ink, 0.5f), 0.f);
		}
		Y += RowHeight;
	}
	// The hint under the rows, then the plane.
	Y += 10.f;
	for (FLabel& Label : Parts.Labels)
	{
		if (Label.Text && &Label == &Parts.Labels[0])
		{
			Label.Text->SetRelativeLocation(FVector(Label.Text->GetRelativeLocation().X, 0.f, Parts.StackPlaneTop - Y - 10.f - 8.f));
			if (Label.Ink)
			{
				Label.Ink->SetRelativeLocation(FVector(Label.Ink->GetRelativeLocation().X, 0.f, Parts.StackPlaneTop - Y - 10.f - 8.f));
			}
		}
	}
	Y += 20.f + 2.f + 16.f;
	if (Parts.Plane.IsBuilt())
	{
		SetPlaneShape(Parts, Width, Y);
		Parts.Plane.Root->SetRelativeLocation(FVector(0.f, 0.f, Parts.StackPlaneTop - 0.5f * Y));
	}
	Parts.PlaceOffset = FVector2D(0.f, -60.f - (Parts.StackPlaneTop - 0.5f * Y));
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] board rows rebuilt: %d rows, card %.0f x %.0f"), Ranking.Num(), Width, Y);
}

// ============================================================================ the key capture card and the error popup

void APinguMenu3D::BuildCapture()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	// PRESS A KEY, "for JUMP (other key)", the hint. No buttons: the Slate key capture layer takes the keys (Esc cancels).
	constexpr float Width = 640.f;
	constexpr float Height = 184.f;
	BeginPage(Capture, EPinguMenu3DPage::Capture, FVector2D(Width, Height));
	AddLabel(Capture, nullptr, FVector2D(0.f, 0.5f * Height - 24.f - 20.f), 38.f, [] { return LOCTEXT("CapturePressAKey", "PRESS A KEY"); }, Style.Gold,
		PinguMenu3D::ETextAlign::Center, Width - 80.f, true, 10.f);
	AddLabel(Capture, nullptr, FVector2D(0.f, 0.5f * Height - 24.f - 48.f - 8.f - 10.f), 15.f,
		[WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H ? H->GetCaptureMessage() : FText::GetEmpty(); }, Style.Snow, PinguMenu3D::ETextAlign::Center, Width - 80.f);
	// (Two lines, 2026-10-26: in one line it ran far past the card's width - the fit check found it.)
	AddFlat(Capture, nullptr, -0.5f * Width + 40.f, 0.5f * Height - 24.f - 48.f - 8.f - 24.f - 16.f, Width - 80.f, 48.f,
		[] { return LOCTEXT("CaptureHint", "Keyboard keys and mouse buttons.\nESC cancels,  BACKSPACE clears."); }, false, 16.f, Style.IceDim, 1);
	EndStatic(Capture);
}

void APinguMenu3D::UpdateCapture(FPageParts& Parts)
{
	(void)Parts;
}

void APinguMenu3D::BuildError()
{
	const FPinguStyle& Style = FPinguStyle::Get();
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	// ONLINE, the message, OK.
	constexpr float Width = 576.f;
	constexpr float Height = 264.f;
	BeginPage(Error, EPinguMenu3DPage::Error, FVector2D(Width, Height));
	Error.DefaultFocus = TEXT("Ok");
	AddLabel(Error, nullptr, FVector2D(-0.5f * Width + 40.f, 0.5f * Height - 24.f - 13.f), 14.f, [] { return LOCTEXT("ErrorTitle", "ONLINE"); }, Style.Ice, PinguMenu3D::ETextAlign::Left);
	AddFlat(Error, nullptr, -0.5f * Width + 40.f, 0.5f * Height - 24.f - 26.f - 8.f, Width - 80.f, 96.f,
		[WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H ? H->GetNetError() : FText::GetEmpty(); }, false, 22.f, Style.Snow, 0);
	AddButton(Error, TEXT("Ok"), EPinguMenu3DKind::Primary, FVector2D(0.f, -0.5f * Height + 32.f + 28.f), FVector2D(200.f, 56.f), 20.f,
		[] { return LOCTEXT("ErrorOk", "OK"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_DismissError(); } });
	EndStatic(Error);
}

#undef LOCTEXT_NAMESPACE

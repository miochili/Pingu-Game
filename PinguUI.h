// Pingu - the Slate UI: style, crisp widgets (shapes, buttons, option rows, sliders, key caps), the overhead icons and
// the root widget that holds every screen and the in-game HUD.
//
// Crispness rules (Linus wants sharp text and edges): text is never scaled by a render transform (that blurs the glyphs).
// Animations change opacity, pixel-snapped translations or layout sizes of shapes only. Every rounded box goes through
// SPinguShape, which fades its outline together with its fill. The whole UI runs at a DPI scale that is a multiple of
// 1/8 (SPinguRoot), so the 8-unit spacing grid always lands on whole pixels.

#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/SlateDelegates.h"
#include "Input/NavigationReply.h"
#include "Layout/Margin.h"
#include "PenguinHUD.h"
#include "PenguinTypes.h"
#include "Rendering/RenderingCommon.h"
#include "Rendering/SlateResourceHandle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

class APenguinCharacter;
class APenguinHUD;
class APlayerController;
class UPenguinMovementComponent;
class SEditableTextBox;
class STextBlock;
class SVerticalBox;
struct FPinguFeedEntry;
/** The PINGUPEDIA's state (pages, the selected entry, its widgets): defined in PinguUIPingupedia.cpp. */
struct FPinguPediaState;
enum class EPinguInputAction : uint8;
enum class EPinguScreen : uint8;

// ============================================================================ style

/** Fonts, colours, brushes and sizes of the Pingu UI (cartoon arctic: navy outlines, icy blues, snow, orange accents). */
class FPinguStyle
{
public:
	static const FPinguStyle& Get();

	/** Display font (Titan One; Lilita One if it is missing): titles, buttons, numbers. Outline in Ink. Size is the size the
	 *  layout was made for: Titan One is drawn at TitleWideScale of it (it is wider than Lilita One), so labels keep their room. */
	static FSlateFontInfo Title(float Size, int32 OutlineSize = 0);
	static constexpr float TitleWideScale = 0.85f;
	/** Rounded body font (Sniglet ExtraBold): descriptions, values, lists. */
	static FSlateFontInfo Body(float Size, int32 OutlineSize = 0);

	// ---- palette (linear colours made from sRGB)
	FLinearColor Ink;           // outlines, text shadows
	FLinearColor Panel;         // cards
	FLinearColor PanelDeep;     // wells inside cards (text boxes, lists)
	FLinearColor Row;           // option rows on cards
	FLinearColor RowHot;
	FLinearColor Blue, BlueHot, BluePressed, BlueLip;
	FLinearColor Orange, OrangeHot, OrangePressed, OrangeLip;
	FLinearColor Red, RedHot, RedPressed, RedLip;
	FLinearColor Snow;
	FLinearColor Ice;
	FLinearColor IceDim;        // secondary text
	FLinearColor Gold;
	FLinearColor Green;
	FLinearColor Disabled;      // disabled button faces
	FLinearColor Ring;          // keyboard / gamepad focus ring
	FLinearColor Dim;           // full-screen dim behind modal cards

	FSlateBrush WhiteBrush;
	FSlateBrush NoBrush;
	/** A true circle at every size (portrait backgrounds, pips). */
	FSlateRoundedBoxBrush CircleBrush;
	/** Text boxes draw no background of their own: an SPinguShape behind them does (it fades properly). */
	FEditableTextBoxStyle TextBox;

	// ---- sizes (Slate units; 8-unit grid)
	static constexpr float Grid = 8.f;
	static constexpr float ButtonHeight = 56.f;
	static constexpr float RowHeight = 48.f;
	static constexpr float SmallHeight = 44.f;
	static constexpr float ButtonRadius = 12.f;
	static constexpr float RowRadius = 10.f;
	static constexpr float CardRadius = 20.f;
	static constexpr float LineWidth = 2.f;
	static constexpr float CardLineWidth = 3.f;
	static constexpr float LipHeight = 4.f;
	static constexpr float RingWidth = 3.f;
	static constexpr float RingGap = 2.f;
	/** Screen fade + slide in (seconds, units). */
	static constexpr float ScreenInTime = 0.14f;
	static constexpr float ScreenInOffset = 16.f;

	/** The white texture for custom vertices (triangles, pies, the cooldown clock), resolved on first use: the core
	 *  style's white image (a colour brush has no texture, so no handle). Invalid until Slate has it; drawing with an
	 *  invalid handle still works (plain vertex colours). */
	const FSlateResourceHandle& GetWhiteHandle() const;

private:
	FPinguStyle();
	FString TitleFontPath;
	/** The title font is Titan One (else Lilita One). */
	bool bTitleWide = false;
	FString BodyFontPath;
	TSharedPtr<const FCompositeFont> TitleFont;
	TSharedPtr<const FCompositeFont> BodyFont;
	mutable FSlateResourceHandle WhiteHandle;
	mutable bool bWhiteHandleResolved = false;
	mutable int32 WhiteHandleTries = 0;
};

namespace PinguUI
{
	/** Slate's real time (keeps running while the game is paused). */
	double Now();
	/** 0 -> 1 ease-out (cubic). */
	float EaseOut(float Alpha);
	/** Rounds a distance in Slate units to whole pixels at the UI scale (crisp translations). */
	float SnapUnits(float Units);
	/** The current pixels per Slate unit of the UI (set by SPinguRoot). */
	float GetUIScale();
	void SetUIScale(float Scale);
	/** Upper-case text from a string. */
	FText Upper(const FString& Text);
	/** The keys that open the in-game menu, for hints ("ESC / P"). In the editor's Play mode Esc stops Play (an editor
	 *  shortcut that comes before the game), so there only the bound Pause keys are shown. */
	FText MenuKeysText();
	FLinearColor Hex(uint8 R, uint8 G, uint8 B, float A = 1.f);
	FLinearColor WithAlpha(const FLinearColor& Color, float Alpha);
	/** A feathered (anti-aliased) filled triangle into a custom-vertex list, in local units. */
	void AddTriangle(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& A, const FVector2f& B, const FVector2f& C, const FLinearColor& Color, float Feather);

	/** The traits' reveal at the start of a round (Traits mode): a card above every penguin, then their flight into the
	 *  penguins (PinguUITraits.cpp). A painted widget over the whole game HUD, under the banner; it shows nothing outside
	 *  a reveal. */
	TSharedRef<SWidget> MakeTraitReveal(const TWeakObjectPtr<APenguinHUD>& HUD);

	/** The viewer's current trait, only its badge, in the top-left corner for the whole round once the reveal is over
	 *  (2026-11-07: no wooden plate behind it). Painted widget (PinguUITraits.cpp); shows nothing without a trait. */
	TSharedRef<SWidget> MakeTraitCorner(const TWeakObjectPtr<APenguinHUD>& HUD);

	/** What the corner icon drew in its last paint (the tests read it): Frame = GFrameCounter of that paint. */
	struct FTraitCornerStats
	{
		bool bShown = false;
		EPinguTrait Trait = EPinguTrait::None;
		/** The badge's rectangle in pixels of the window (its draw size is Size Slate units). */
		FSlateRect Rect;
		float Size = 0.f;
		uint64 Frame = 0;
	};
	const FTraitCornerStats& GetTraitCornerStats();

	/** What the reveal drew in its last paint (the tests read it). */
	struct FTraitRevealStats
	{
		/** Cards showing, cards flying into their penguins, landing flashes. */
		int32 Cards = 0;
		int32 Flying = 0;
		int32 Flashes = 0;
		/** Rows of cards over the penguins in the picture (1..3) and whether two cards overlapped. */
		int32 Rows = 0;
		bool bOverlap = false;
		/** Cards of penguins out of the picture (lined up at the screen's edge, next to their markers), the off-screen
		 *  markers (SPinguOverheadAbilities), and among the cards showing: pairs that overlap, cards covering a marker, cards
		 *  not wholly in the picture (all three want 0). */
		int32 EdgeCards = 0;
		int32 Markers = 0;
		int32 OverlapPairs = 0;
		int32 CardsOverMarkers = 0;
		int32 CardsOutside = 0;
		/** The trait names on the cards (screen order). */
		TArray<FString> Names;
		/** The round's one trait (2026-11-07, everybody the same): its banner shows (instead of the cards), how long it has shown,
		 *  its name, the lines it holds (name, upside, downside), its box and whether it left the picture. */
		bool bBanner = false;
		float BannerAge = -1.f;
		FString BannerName;
		int32 BannerLines = 0;
		FSlateRect BannerRect;
		bool bBannerOutside = false;
		/** Deadly Edges: the red edge lines drawn (0..2) and their screen X (Slate units; -1 none). */
		int32 EdgeLines = 0;
		float EdgeLeftX = -1.f;
		float EdgeRightX = -1.f;
		/** GFrameCounter of that paint (0: never painted). */
		uint64 Frame = 0;
	};
	const FTraitRevealStats& GetTraitRevealStats();

	/** The PINGUPEDIA's numbers for the menu screenshots' check (PinguUIPingupedia.cpp): the entries on its two pages, and
	 *  the title menu's PINGUPEDIA label - its width at the font it got and the room its button has (PinguUIMenus.cpp). */
	struct FPingupediaStats
	{
		int32 Abilities = 0;
		int32 Traits = 0;
		/** The tallest entry card (all 33 measured: Slate units), which one it is, and the room the detail panel has. */
		float TallestCard = 0.f;
		FString TallestName;
		float CardRoom = 0.f;
		/** Stat lines on any card that show a code name (a stat kind without a name of its own yet): there must be none. */
		int32 RawLabels = 0;
		FString RawLabelNames;
		float LabelWidth = 0.f;
		float LabelRoom = 0.f;
		float LabelFont = 0.f;
	};
	const FPingupediaStats& GetPingupediaStats();
	FPingupediaStats& MutablePingupediaStats();
	/** The PINGUPEDIA's words and numbers, for the 3D page too (PinguUIPingupedia.cpp): an entry's name (Page 0 abilities, 1
	 *  traits), a stat line's name on that entry's card (bOutRaw: it has no name of its own yet), a value with its unit, and
	 *  the movement whose numbers the abilities' lines read. */
	FText PingupediaEntryName(int32 Page, int32 Entry);
	FText PingupediaStatLabel(EPinguStatKind Kind, int32 Page, int32 Entry, bool bUpside, bool* bOutRaw = nullptr);
	FText PingupediaFormatValue(EPinguStatKind Kind, float Value);
	const UPenguinMovementComponent* PingupediaFindMovement(const APenguinHUD* PenguinHUD);
}

// ============================================================================ shapes

/**
 * A crisp rounded box: fill + outline + corner radius. Slate draws a rounded-box brush's outline in its own colour (the
 * draw tint only reaches the fill), so this widget multiplies both by its widget opacity itself: screens fade cleanly.
 */
class SPinguShape : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguShape)
		: _FillColor(FLinearColor::White)
		, _OutlineColor(FLinearColor::Transparent)
		, _OutlineWidth(0.f)
		, _Radius(8.f)
		, _Pill(false)
		, _Wood(false)
		, _Banner(false)
	{
		_Visibility = EVisibility::HitTestInvisible;
	}
		SLATE_ATTRIBUTE(FLinearColor, FillColor)
		SLATE_ATTRIBUTE(FLinearColor, OutlineColor)
		SLATE_ATTRIBUTE(float, OutlineWidth)
		SLATE_ARGUMENT(float, Radius)
		/** Corners are half the height (a pill / a circle). */
		SLATE_ARGUMENT(bool, Pill)
		/** The UI's one wood (2026-11-07, PinguWoodStyle.h): a rendered plank (a pill plank with Pill) instead of the fill; the
		 *  fill's alpha still fades it, the outline is not drawn. Falls back to the fill without the wood atlas. */
		SLATE_ARGUMENT(bool, Wood)
		/** With Wood: the banner plank (snow, nails, icicles: the round-win banner, docs/UI_STYLE.md). */
		SLATE_ARGUMENT(bool, Banner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

protected:
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	TAttribute<FLinearColor> FillColorAttr;
	TAttribute<FLinearColor> OutlineColorAttr;
	TAttribute<float> OutlineWidthAttr;
	bool bWood = false;
	bool bPill = false;
	bool bBanner = false;
	mutable FSlateRoundedBoxBrush ShapeBrush{ FLinearColor::White, 8.f };
};

/** A filled triangle pointing left / right / down (option arrows, list markers). */
class SPinguArrow : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguArrow)
		: _Direction(1)
		, _Color(FLinearColor::White)
		, _Size(14.f)
	{
		_Visibility = EVisibility::HitTestInvisible;
	}
		/** -1 left, +1 right, 0 down. */
		SLATE_ARGUMENT(int32, Direction)
		SLATE_ATTRIBUTE(FLinearColor, Color)
		SLATE_ARGUMENT(float, Size)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

protected:
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	int32 Direction = 1;
	float ArrowSize = 14.f;
	TAttribute<FLinearColor> ColorAttr;
	mutable TArray<FSlateVertex> Verts;
	mutable TArray<SlateIndex> Indices;
};

// ============================================================================ buttons and rows

/** How a button looks. */
enum class EPinguButtonKind : uint8
{
	Primary,   // orange, the main action of a screen
	Normal,    // blue
	Ghost,     // outlined only (BACK, QUIT)
	Danger,    // red (LEAVE)
	Row,       // a flat option / list row (focus ring, lighter when hot)
	KeyCap,    // a key on the controls screen (snow face, navy text)
	Arrow,     // the small mouse arrows inside option rows (never focusable)
	Segment,   // preset strip buttons (orange when selected)
};

DECLARE_DELEGATE_OneParam(FPinguStepDelegate, int32);

/**
 * The one clickable control of the UI: buttons, option rows, slider rows, key caps, list entries.
 * Snappy: the action fires on press (mouse down / Enter / gamepad A) with a click sound, the look changes at once.
 * Hover takes the keyboard focus (one highlighted control at a time), the focus ring shows for keyboard / gamepad.
 * With OnStep bound, Left / Right (arrows, d-pad, stick) call it with -1 / +1 instead of moving the focus.
 * No render transforms: pressing moves the face down by changing its padding (whole units).
 */
class SPinguButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguButton)
		: _Kind(EPinguButtonKind::Normal)
		, _Width(0.f)
		, _Height(FPinguStyle::ButtonHeight)
		, _IsFocusable(true)
		, _IsSelected(false)
		, _HAlign(HAlign_Center)
		, _ContentPadding(FMargin(16.f, 0.f))
		, _FontSize(24.f)
	{}
		/** Custom content (replaces the label). */
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_ATTRIBUTE(FText, Text)
		SLATE_ARGUMENT(EPinguButtonKind, Kind)
		/** 0 = fill the slot. */
		SLATE_ARGUMENT(float, Width)
		SLATE_ARGUMENT(float, Height)
		SLATE_ARGUMENT(bool, IsFocusable)
		/** Toggles and preset buttons. */
		SLATE_ATTRIBUTE(bool, IsSelected)
		SLATE_ARGUMENT(EHorizontalAlignment, HAlign)
		SLATE_ARGUMENT(FMargin, ContentPadding)
		SLATE_ARGUMENT(float, FontSize)
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
		SLATE_EVENT(FSimpleDelegate, OnClicked)
		/** A right click (with a click sound): e.g. a lobby card steps back through the teams. Unbound: not handled. */
		SLATE_EVENT(FSimpleDelegate, OnRightClicked)
		SLATE_EVENT(FPinguStepDelegate, OnStep)
		/** The control got the focus (screens show a hint for it). */
		SLATE_EVENT(FSimpleDelegate, OnFocused)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override;
	virtual bool IsInteractable() const override;
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual void OnFocusLost(const FFocusEvent& InFocusEvent) override;
	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;

	/** Hovered or focused. */
	bool IsHot() const;
	bool HasFocusRing() const;
	bool LooksPressed() const;
	/** Left / Right on an option row (the inner arrows call it too, silently: their own press already clicked). */
	void Step(int32 Direction, bool bPlaySound = true);

private:
	void Press();
	FLinearColor GetFaceColor() const;
	FLinearColor GetLipColor() const;
	FLinearColor GetFaceOutline() const;
	FLinearColor GetLabelColor() const;
	FMargin GetFacePadding() const;
	FMargin GetContentSlotPadding() const;

	TWeakObjectPtr<APenguinHUD> HUD;
	FSimpleDelegate OnClickedDelegate;
	FSimpleDelegate OnRightClickedDelegate;
	FPinguStepDelegate OnStepDelegate;
	FSimpleDelegate OnFocusedDelegate;
	TAttribute<bool> SelectedAttr;
	EPinguButtonKind Kind = EPinguButtonKind::Normal;
	FMargin LabelPadding;
	bool bFocusable = true;
	bool bMouseHeld = false;
	bool bKeyHeld = false;
	double PressTime = -10.0;
	/** When this widget was built: a double click on a very young widget is the second click of a rebuilt screen. */
	double ConstructTime = 0.0;
};

/** The bar of a volume slider: drag or click to set 0..1 (the row around it handles keys). */
class SPinguSlider : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguSlider)
		: _Value(0.5f)
		, _Step(0.05f)
	{}
		SLATE_ATTRIBUTE(float, Value)
		SLATE_ARGUMENT(float, Step)
		/** Lit while the row around it is hot. */
		SLATE_ATTRIBUTE(bool, IsHot)
		SLATE_EVENT(FOnFloatValueChanged, OnValueChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

protected:
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	void SetFromPointer(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);

	TAttribute<float> ValueAttr;
	TAttribute<bool> HotAttr;
	float StepSize = 0.05f;
	FOnFloatValueChanged OnValueChangedDelegate;
	bool bDragging = false;
	mutable FSlateRoundedBoxBrush TrackBrush{ FLinearColor::White, 6.f };
	mutable FSlateRoundedBoxBrush KnobBrush{ FLinearColor::White, 12.f };
};

/** A screen: fades in (opacity) and slides up a few pixels (whole pixels) when it appears. Clicks on its empty parts
 *  keep the focus on the menu instead of dropping it into the game viewport. */
class SPinguScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguScreen)
		: _Screen(static_cast<EPinguScreen>(0))
		, _bDim(true)
	{}
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
		/** Which screen this is (it shows while the HUD says so). */
		SLATE_ARGUMENT(EPinguScreen, Screen)
		/** A darkening layer behind the content. */
		SLATE_ARGUMENT(bool, bDim)
		SLATE_ATTRIBUTE(FLinearColor, DimColor)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	EPinguScreen GetScreenId() const { return ScreenId; }

private:
	/** 0 -> 1 while it fades in: a function of the time the HUD switched screens (no tick order to depend on). */
	float GetAppear() const;
	float GetDimAppear() const;

	TWeakObjectPtr<APenguinHUD> HUD;
	EPinguScreen ScreenId;
	TSharedPtr<SWidget> Body;
};

/** Full-screen layer while a key is being rebound: takes every key and mouse button (Escape cancels, Backspace /
 *  Delete clear, anything CanBind accepts is bound). */
class SPinguKeyCapture : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguKeyCapture) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;

private:
	TWeakObjectPtr<APenguinHUD> HUD;
};

/** A modal card layer (the error popup): keyboard / gamepad navigation stays inside it and clicks next to it keep the
 *  focus on it, so nothing under it can be pressed by accident. */
class SPinguModal : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguModal) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
		/** Where the focus goes back to after a click next to the card. */
		SLATE_ATTRIBUTE(TSharedPtr<SWidget>, FocusTarget)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;

private:
	TAttribute<TSharedPtr<SWidget>> FocusTarget;
};

// ============================================================================ overhead icons

/**
 * The ability icons above the head of every penguin (like Bopl Battle): small round icons with a dark clock sweep over
 * the part that is still recharging and a little pop when an ability is ready again. The local players' own icons are
 * full size, everybody else's (other people, the bots) a bit smaller.
 * The three icons form a small triangle: LMB bottom left, RMB bottom right, MMB (where pickups go) on top.
 * Also draws the ability bubbles (pickups), in LAN games a small name tag above every other penguin, and a marker at
 * the screen's edge for every penguin flung out of the picture (its portrait and an arrow pointing to it).
 * Painted directly (no child widgets); it fills the HUD and follows the penguins every frame.
 */
/**
 * A soft dark glow around the mouse pointer: hardly noticeable, but the arrow is easier to find on bright snow. The system
 * draws the pointer itself (on top of everything); this only paints the glow under it, sized from the pointer's own size
 * (it grows with the Windows scaling like the arrow does). Nothing while the game is not the active app or the pointer is
 * outside the game's picture. Never takes a click.
 */
class SPinguCursorHalo : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguCursorHalo)
	{
		_Visibility = EVisibility::HitTestInvisible;
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** How dark the middle of the glow is (0..1): the edge fades out to nothing. */
	static constexpr float Darkness = 0.26f;
	/** Its radius against the pointer's size (an arrow of a 32 px pointer: about 23 px). */
	static constexpr float RadiusShare = 0.72f;
	/** Its middle from the pointer's tip, against the pointer's size: over the arrow's body (it points up and left). */
	static constexpr float CenterShareX = 0.2f;
	static constexpr float CenterShareY = 0.3f;

protected:
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual bool ComputeVolatility() const override { return true; }

private:
	mutable TArray<FSlateVertex> Verts;
	mutable TArray<SlateIndex> Indices;
};

class SPinguOverheadAbilities : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguOverheadAbilities) {}
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** Diameter of one icon (Slate units = pixels at 1080p). */
	static constexpr float IconSize = 34.f;
	/** Space between two neighbouring icons of the triangle. */
	static constexpr float IconGap = 4.f;
	/** Width of the coloured ring around an icon. */
	static constexpr float RingWidth = 2.5f;
	/** Head clearance above the capsule top (cm). */
	static constexpr float HeadClearance = 35.f;
	/** The "ready" pop: scale 1 -> 1 + PopScale -> 1 over PopDuration seconds (shapes only; there is no text). */
	static constexpr float PopDuration = 0.2f;
	static constexpr float PopScale = 0.18f;
	/** The pickup pop (a bubble swapped or recharged the slot): twice as big, with a white ring flash. */
	static constexpr float SwapPopDuration = 0.35f;
	static constexpr float SwapPopScale = 2.f * PopScale;
	/** Gambler's fizzle (Traits mode): the slot's icon shivers, crumbles into sparkling dust (gone by DustGoneAt), the dust falls
	 *  and fades, and the icon pops back in with its cooldown clock from DustBackAt on, DustBackTime long. DustPerIcon pieces
	 *  at full effects density (fewer on Low). A double makes the icon shine gold for ShineTime. */
	static constexpr float DustShiverTime = 0.06f;
	static constexpr float DustGoneAt = 0.42f;
	static constexpr float DustBackAt = 0.74f;
	static constexpr float DustBackTime = 0.26f;
	static constexpr float DustTime = DustBackAt + DustBackTime;
	static constexpr int32 DustPerIcon = 30;
	static constexpr float ShineTime = 0.55f;
	/** The snowball combo's pop: a "x3" (the hit's number in its thrower's chain, from the second hit on) pops out of the victim's
	 *  chest, rises ComboPopRise Slate units and fades over ComboPopTime. It grows from ComboPopFontMin to ComboPopFontMax and turns
	 *  from white through gold to hot orange as the push gets to its cap. */
	static constexpr float ComboPopTime = 0.85f;
	static constexpr float ComboPopRise = 70.f;
	static constexpr float ComboPopFontMin = 32.f;
	static constexpr float ComboPopFontMax = 58.f;
	/** Ability bubbles: icon diameter, ring width and the period of the slow ring pulse (seconds). */
	static constexpr float PickupSize = 58.f;
	static constexpr float PickupRingWidth = 4.f;
	static constexpr float PickupPulsePeriod = 1.6f;
	/** LAN name tags: font size and the gap above the head (cm) - above the icons where a penguin shows them. */
	static constexpr float TagFontSize = 13.f;
	static constexpr float TagClearance = 28.f;
	/** Everybody else's icons, against the local players' own. */
	static constexpr float OtherIconScale = 0.7f;
	/** Team matches: a thin ring in the team's colour around each of a penguin's icons (teammates wear the same colour; a
	 *  penguin on its own - it picked no team - has none). Just outside the icon's own ring, so the ability stays in full
	 *  view: its width, the room between it and the icon, and how much further apart ringed icons stand (rings of
	 *  neighbouring icons never touch). */
	static constexpr float TeamIconRingWidth = 2.5f;
	static constexpr float TeamIconRingGap = 1.f;
	static constexpr float TeamIconSpread = 5.f;
	/** Off-screen markers in a team match: a ring in the team's colour around the marker's circle (width, room). */
	static constexpr float TeamRingWidth = 3.5f;
	static constexpr float TeamRingGap = 3.f;
	/** Height of a penguin's icon triangle (Slate units) at a scale, with team rings or without. */
	static float GetIconsHeight(float Scale, bool bTeamRings);

	/** What the last paint drew above the heads (the lobby test reads it). */
	struct FPaintStats
	{
		/** Display names of the penguins whose icons got their team's rings, and how many rings that was. */
		TArray<FString> Ringed;
		int32 IconRings = 0;
		/** The biggest ring's outer diameter against its icon's (the old ring around all three icons: about 2.7), and the
		 *  smallest room between an icon's edge and the inside of its ring (>= 0: the ring covers nothing of the icon). */
		float RingToIcon = 0.f;
		float MinClearance = 0.f;
		/** GFrameCounter of that paint. */
		uint64 Frame = 0;
		/** Gambler: in that paint, the icons crumbling to dust and shining, and the dust pieces drawn. */
		int32 DustIcons = 0;
		int32 ShineIcons = 0;
		int32 DustPieces = 0;
		/** The age (s) of the youngest dust / shine drawn in that paint (-1: none); the UI runs on real time, a test's game
		 *  clock does not, so the tests time their pictures by these. */
		float DustAge = -1.f;
		float ShineAge = -1.f;
		/** Gambler: how many fizzles / doubles started an icon's dust / shine so far (never reset). */
		int32 DustStarts = 0;
		int32 ShineStarts = 0;
		/** The snowball combo: in that paint, the pops drawn and the age (s) of the youngest (-1: none); how many pops started so far
		 *  (never reset) and the number of the last one. */
		int32 ComboPops = 0;
		float ComboPopAge = -1.f;
		int32 ComboPopStarts = 0;
		int32 LastComboCount = 0;
		/** Traits mode, Damage %: in that paint, the damage labels drawn under the penguins and the highest number on them. */
		int32 DamageLabels = 0;
		int32 MaxDamageShown = -1;
	};
	static const FPaintStats& GetLastPaintStats() { return MutableStats(); }
	/** Off-screen markers: diameter, ring width, distance of the circle from the screen's edge, and the arrow's size. */
	static constexpr float MarkerSize = 50.f;
	static constexpr float MarkerRing = 3.f;
	static constexpr float MarkerEdge = 10.f;
	static constexpr float MarkerArrow = 11.f;

protected:
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual bool ComputeVolatility() const override { return true; }

private:
	/** A penguin and the animation state of its three icons. */
	struct FTrackedPenguin
	{
		TWeakObjectPtr<APenguinCharacter> Penguin;
		TWeakObjectPtr<APlayerController> Controller;
		/** A local player's own penguin (full-size icons). */
		bool bLocal = false;
		EPenguinAbility Shown[3] = { EPenguinAbility::None, EPenguinAbility::None, EPenguinAbility::None };
		bool bWasReady[3] = { true, true, true };
		/** Seconds since the slot became ready again (the pop plays while this is below PopDuration). */
		float SinceReady[3] = { PopDuration, PopDuration, PopDuration };
		/** Pickup count of each slot and seconds since it last went up (the pickup pop). */
		int32 SwapCount[3] = { 0, 0, 0 };
		float SinceSwap[3] = { SwapPopDuration, SwapPopDuration, SwapPopDuration };
		/** Gambler: the last roll's serial seen (a new one starts a dust or a shine), and per slot the seconds since its icon
		 *  began to crumble / shine (DustTime / ShineTime or more: not now) and the dust's seed. */
		uint8 SeenGamble = 0;
		float DustAge[3] = { DustTime, DustTime, DustTime };
		float ShineAge[3] = { ShineTime, ShineTime, ShineTime };
		int32 DustSeed[3] = { 0, 0, 0 };
		/** The snowball combo: the last pop's serial seen (a new one starts a pop), its number and seconds since it started. */
		uint8 SeenCombo = 0;
		int32 ComboCount = 0;
		float ComboAge = ComboPopTime;
		bool bSeen = false;
	};

	/** Dark disc with a coloured ring (Layer) and the ability art on top (Layer + 1). Diameter and ring width in local units. */
	void PaintDisc(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
		float Diameter, float Ring, const FLinearColor& RingColor, const FLinearColor& DiscColor, const FSlateBrush* Art, const FLinearColor& ArtTint) const;
	/** Gambler's fizzle: the dust of an icon (Diameter wide at Center) Age seconds into its crumbling - glitter and chips in the
	 *  ability's colour that fall and fade. Returns the next free layer after Layer. */
	int32 PaintIconDust(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
		float Diameter, const FLinearColor& AbilityColor, float Age, int32 Seed, const FLinearColor& Fade) const;
	/** Gambler's double: a golden glint over the icon, Age seconds into its shine. */
	int32 PaintIconShine(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
		float Diameter, float Age, const FLinearColor& Fade) const;
	/** The snowball combo's pop over a victim: Count ("x3") Age seconds into its rise, at Center (the victim's chest on screen).
	 *  Returns the next free layer after Layer. */
	int32 PaintComboPop(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
		int32 Count, float Age, const FLinearColor& Fade) const;
	/** Just the ring, empty inside. */
	void PaintRing(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
		float Diameter, float Ring, const FLinearColor& RingColor) const;
	/** LAN: the names above the other penguins. Returns the next free layer. */
	int32 PaintNameTags(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FLinearColor& Fade, float UnitsPerPixel) const;
	/** Penguins out of the picture: a marker at the screen's edge each. Returns the next free layer. */
	int32 PaintOffscreenMarkers(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FLinearColor& Fade,
		float UnitsPerPixel) const;
	static FPaintStats& MutableStats();
	/** A slot's portrait (face + its beanie layer tinted with ColorIndex's colour) in a square of Size around Center. */
	void PaintPortrait(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center, float Size,
		int32 Slot, int32 ColorIndex, const FLinearColor& Fade) const;

	TWeakObjectPtr<APenguinHUD> HUD;
	TArray<FTrackedPenguin> Tracked;
	double LastTickTime = -1.0;
	/** Round icon: the fill is the dark disc (tint), the outline is the ability-coloured ring (set per icon while painting). */
	mutable FSlateRoundedBoxBrush IconBrush{ FLinearColor::White, IconSize * 0.5f, FLinearColor::White, RingWidth };
	/** Name tag pill (outline set per tag). */
	mutable FSlateRoundedBoxBrush TagBrush{ FLinearColor::White, 10.f, FLinearColor::White, 2.f };
	/** Measured name widths (names rarely change). */
	mutable TMap<FString, float> TagWidths;
	mutable TArray<FSlateVertex> PieVerts;
	mutable TArray<SlateIndex> PieIndices;
};

// ============================================================================ root

/**
 * Every screen of the game in one overlay; each part shows itself depending on the HUD's screen and the replicated
 * game state. Built once; lists that depend on data (lobby players, found games, standings, leaderboard) are rebuilt
 * when that data changes.
 */
class SPinguRoot : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguRoot) {}
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** The widget that should get the focus first on a screen (null for none). */
	TSharedPtr<SWidget> GetFocusTarget(EPinguScreen Screen) const;
	/** The container of a screen (to know whether the focus is inside it). */
	TSharedPtr<SWidget> GetScreenWidget(EPinguScreen Screen) const;
	TSharedPtr<SWidget> GetErrorFocusTarget() const;
	/** The host's lobby line "Others join with <address>": a button that copies the address (the menu screenshots press it). */
	TSharedPtr<SWidget> GetLobbyAddressWidget() const { return LobbyAddress; }
	TSharedPtr<SWidget> GetCaptureWidget() const { return CaptureLayer; }
	/** A text box has the keyboard (letters are typed, not shortcuts). */
	bool IsTyping() const;
	/** Pixels per Slate unit of the UI (a multiple of 1/8). */
	float GetUIScale() const { return UIScale; }
	/** The hint line of the settings / controls screens. */
	void SetHint(const FText& InHint) { Hint = InHint; }
	FText GetHint() const { return Hint; }
	/** The name / address boxes write their text back (before hosting / joining / leaving the screen). */
	void CommitTextBoxes();
	/** The HUD switched to this screen (fresh hint, the settings start on their first page). */
	void OnScreenShown(EPinguScreen Screen);
	/** Settings pages (GRAPHICS / DISPLAY / AUDIO): a click on a tab, or LB / RB anywhere on the screen. */
	void SelectSettingsTab(int32 Tab, bool bFocusFirst);
	/** What the banner says now: "ROUND 2", "3", "GO!", "FROSTY WINS!", "RED TEAM WINS!" ... (PinguUIHud.cpp). */
	FText GetBannerText() const;
	/** The round-win banner (RoundOver, kill cam included): it stands on a banner plank. */
	bool IsWinBanner() const;
	/** VICTORY! / FROSTY WINS! / PURPLE TEAM WINS! (the results' title; PinguUIMenus.cpp). */
	FText GetResultsTitle() const;
	/** PINGUPEDIA: shows Page (0 abilities, 1 traits) with its Entry selected (clamped); bFocus moves the keyboard focus
	 *  to that entry's row (PinguUIPingupedia.cpp; the menu screenshots call it too). */
	void SelectPingupediaPage(int32 Page, int32 Entry, bool bFocus);
	/** PINGUPEDIA: the page and the entry that show; true when that entry's row is wholly inside the list's view (the menu
	 *  screenshots check it after scrolling to the last trait). */
	bool GetPingupediaView(int32& OutPage, int32& OutEntry) const;

private:
	// ---- building (PinguUIHud.cpp)
	TSharedRef<SWidget> BuildGameHUD();
	TSharedRef<SWidget> BuildHelp();
	TSharedRef<SWidget> BuildLobbyPill();
	TSharedRef<SWidget> BuildFPS();
	TSharedRef<SWidget> BuildFeed();
	TSharedRef<SWidget> BuildBanner();
	/** "YOU PLAY FROM THE NEXT ROUND" at the top while this PC's player, who joined during a match, waits. */
	TSharedRef<SWidget> BuildWaitingPill();
	/** Line Row of the feed (the oldest first) while it shows, with its age in seconds; null if there is none. */
	const FPinguFeedEntry* GetFeedLine(int32 Row, float& OutAge) const;
	void RebuildLeaderboard();
	/** When the banner text last changed: updated by whichever banner attribute is evaluated first in a frame, so a new
	 *  text never shows for a frame before its drop-in starts. */
	double GetBannerChangedAt() const;
	FLinearColor GetBannerColor() const;
	TOptional<FSlateRenderTransform> GetBannerTransform() const;
	bool IsHUDVisible() const;
	/** Between rounds: 0 until the leaderboard shows, then 0 -> 1 while it comes in. */
	float GetLeaderboardIn() const;
	/** Seconds since the round winner's new win was added on the leaderboard (negative before that). */
	float GetFreshWinTime() const;
	/** Rows the leaderboard between rounds has (penguins, or teams in a team match): the banner moves up further for more. */
	int32 GetBoardRows() const;
	/** Under the leaderboard between rounds: "PRESS ANY KEY" once a key takes us on (0 -> 1 as it comes in, 0 hidden). */
	float GetContinueHintIn() const;

	// ---- menus (PinguUIMenus.cpp)
	TSharedRef<SWidget> BuildMainMenu();
	TSharedRef<SWidget> BuildLan();
	TSharedRef<SWidget> BuildLobby();
	TSharedRef<SWidget> BuildPause();
	TSharedRef<SWidget> BuildResults();
	TSharedRef<SWidget> BuildError();
	TSharedRef<SWidget> BuildCapture();
	void RebuildFoundGames();
	void RebuildLobbyPlayers();
	void RebuildStandings();
	/** A lobby card (Index into LobbyCards): the penguin's portrait (its team's ring around it), name, team and - a bot - its
	 *  skill. Clickable where this player may change it (your own card; the host: the bots' cards). The team and the skill
	 *  follow the data by themselves (a click does not rebuild the cards). */
	TSharedRef<SWidget> MakeLobbyCard(int32 Index);
	/** The lobby's + card (the host, while there is room): one more bot. */
	TSharedRef<SWidget> MakeAddBotCard();

	// ---- settings + controls (PinguUISettings.cpp)
	TSharedRef<SWidget> BuildSettings();
	TSharedRef<SWidget> BuildControls();
	/** "LABEL   < VALUE >": Left / Right (and the arrows) step and stop at the ends (bWrap false), a click / Accept
	 *  goes to the next value and wraps around (bWrap true). */
	TSharedRef<SPinguButton> MakeOptionRow(const FText& Label, TAttribute<FText> Value, TFunction<void(int32, bool)> OnChange, const FText& InHint,
		TAttribute<bool> Enabled = TAttribute<bool>(true), float Height = FPinguStyle::SmallHeight, float ValueWidth = 168.f);
	TSharedRef<SPinguButton> MakeSliderRow(const FText& Label, TAttribute<float> Value, TFunction<void(float)> OnChanged, const FText& InHint);
	TSharedRef<SPinguButton> MakeKeyCap(EPinguInputAction Action, int32 Index);

	// ---- PINGUPEDIA (PinguUIPingupedia.cpp): every ability and every trait, with their numbers
	TSharedRef<SWidget> BuildPingupedia();
	/** The host's TRAITS list (3D menus only: its input layer; PinguMenu3DTraits.cpp). */
	TSharedRef<SWidget> BuildTraitPicks();
	/** The widget that gets the focus first on the PINGUPEDIA (null: none yet). */
	TSharedPtr<SWidget> GetPingupediaFocus() const;
	/** The HUD switched to the PINGUPEDIA. */
	void OnPingupediaShown();
	/** A key the PINGUPEDIA's controls did not take (LB / RB switch the tabs). True: handled. */
	bool HandlePingupediaKey(const FKeyEvent& InKeyEvent);

	// ---- shared pieces
	TSharedRef<SWidget> MakeCard(const TSharedRef<SWidget>& Content, const FMargin& Padding, float Width = 0.f) const;
	TSharedRef<SWidget> MakeTitle(const FText& Text, float Size = 52.f) const;
	TSharedRef<SWidget> MakeSection(const FText& Text) const;
	TSharedRef<SWidget> MakeTextField(TSharedPtr<SEditableTextBox>& OutBox, TAttribute<FText> Text, const FText& HintText, float Width, bool bName);
	/** A slot's portrait in a circle of its own colour (the circle and the beanie); TeamRing >= 0: a ring in that team's
	 *  colour around it (a team match - just outside the circle, so the size stays). */
	TSharedRef<SWidget> MakePortrait(int32 Slot, float Size, int32 TeamRing = -1) const;
	/** A small pill in a team's colour with its name ("RED"); NO TEAM for -1. */
	TSharedRef<SWidget> MakeTeamTag(int32 Team, float FontSize = 14.f) const;
	TSharedRef<SWidget> MakeTag(TAttribute<FText> Text, const FLinearColor& Fill, const FLinearColor& TextColor, float FontSize = 14.f) const;
	TSharedRef<SWidget> MakeScreen(EPinguScreen Screen, const TSharedRef<SWidget>& Content, bool bDim = true);

	TWeakObjectPtr<APenguinHUD> HUD;
	/** Pixels per unit of the UI, and the extra factor on top of the engine's own DPI scale that gives it. */
	float UIScale = 1.f;
	float DPIFactor = 1.f;
	FText Hint;

	// screens
	TMap<uint8, TSharedPtr<SWidget>> Screens;
	TMap<uint8, TSharedPtr<SWidget>> FirstFocus;
	TSharedPtr<SWidget> LanBack;
	TSharedPtr<SWidget> LobbyStart;
	TSharedPtr<SWidget> LobbySettings;
	/** The 3D lobby: the invisible layer that hands the keys to the 3D button strip while it has the keyboard focus (Esc / Start). */
	TSharedPtr<SWidget> LobbyMenu3DInput;
	TSharedPtr<SWidget> LobbyAddress;
	TSharedPtr<SWidget> ResultsLeave;
	TSharedPtr<SWidget> ErrorOk;
	TSharedPtr<SWidget> ErrorLayer;
	TSharedPtr<SPinguKeyCapture> CaptureLayer;

	/** A focus change asked for while its target was still hidden (a settings page that is about to show): applied from
	 *  Tick, after the layout pass made it visible. */
	TWeakPtr<SWidget> PendingFocus;
	double PendingFocusUntil = 0.0;

	// PINGUPEDIA (its state lives in PinguUIPingupedia.cpp)
	TSharedPtr<FPinguPediaState> Pedia;

	// settings pages
	int32 SettingsTab = 0;
	TSharedPtr<SWidget> SettingsTabFirst[3];
	TSharedPtr<SWidget> SettingsTabButtons[3];

	// text boxes
	TSharedPtr<SEditableTextBox> NameBox;
	TSharedPtr<SEditableTextBox> AddressBox;

	// lists rebuilt from data
	TSharedPtr<SVerticalBox> FoundGamesBox;
	TSharedPtr<SVerticalBox> LobbyPlayersBox;
	/** The lobby's cards of this frame (the HUD's GetLobbyCards, once per frame for all the cards' attributes). */
	TArray<FPinguLobbyPlayer> LobbyCards;
	/** Every card, the clickable ones (null where a card is not yours to change), who each one is ("P:name" / "B:name")
	 *  and the + card: the keyboard focus stays on the same card when the cards are rebuilt. */
	TArray<TSharedPtr<SWidget>> LobbyCardWidgets;
	TArray<TSharedPtr<SWidget>> LobbyCardButtons;
	TArray<FString> LobbyCardIds;
	TSharedPtr<SWidget> LobbyAddButton;
	TSharedPtr<SVerticalBox> StandingsBox;
	TSharedPtr<SVerticalBox> LeaderboardBox;
	TSharedPtr<SWidget> LeaderboardCard;
	FString BuiltGamesKey;
	FString BuiltLobbyKey;
	FString BuiltStandingsKey;
	int32 BuiltBoardRound = -1;
	int32 BuiltBoardScores = -1;

	// banner
	mutable FString LastBanner;
	mutable double BannerChangedAt = -10.0;
	TSharedPtr<SWidget> BannerText;
	/** Sudden death as this machine saw it start (its start time is on the game's flow clock, which differs between
	 *  solo and LAN games: the banner times itself from the moment it was seen here). */
	mutable float SeenCollapseStart = -1.f;
	mutable double SuddenDeathSeenAt = -100.0;

	// fps
	int32 FpsFrames = 0;
	float FpsTime = 0.f;
	int32 FpsShown = 0;
};

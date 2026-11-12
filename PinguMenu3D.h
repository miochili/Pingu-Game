// Pingu - the 3D menus: every menu screen (the title, the lobby's strip, pause, LAN, results, the round board, settings, controls,
// the PINGUPEDIA, the key capture card and the error popup) as real 3D models in front of the stage camera - since 2026-10-26
// snowy wood (Linus's mockup): plank signs with arrow points, nails and a snow slab, framed plank boards, a rope and a yellow
// arrow for the focus, a tilt for the hover, extruded Titan One letters - plus the small toolkit they are made of (3D text,
// rounded slabs, rings, textured icons) that other 3D UI can use.
// Long body text (the PINGUPEDIA's descriptions and stat lines) is flat Slate text drawn on the face of a 3D panel by
// SPinguMenu3DOverlay (PinguMenu3DOverlay.h): it follows the panel's projection every frame (2026-10-24, the whole-menu job).
//
// How it fits in: the Slate UI still owns the screens, the focus and the keys. On the title and the lobby the Slate screen holds an
// invisible full-screen layer (SPinguMenu3DInput) that takes the focus and passes the mouse and the keys to APinguMenu3D, which
// shows the page of the HUD's screen, traces the pointer into the 3D buttons and calls the same APenguinHUD::Action_* functions the
// Slate buttons call. -PinguOldMenus (or [PinguMenus] bOldMenus=True in DefaultGame.ini) gives the old flat Slate title and lobby.
//
// The mouse (2026-10-24, Linus: "the lobby registers the mouse really bad"): every pointer position is turned into a pixel of the
// game's own viewport through the viewport widget's geometry (PinguMenu3D::ScreenToViewportPixel: right inside the editor's PIE
// window at any DPI) and into a ray with the player controller's own deprojection (the projection the picture was drawn with: no
// field-of-view or aspect guesses). Buttons are hit on a generous rectangle round their face; a press happens on the mouse-down and
// the layer keeps the mouse until the release. On the lobby island the layer stays up and takes every click (the strip's buttons
// first, then APinguLobby's penguins, tags, x and "+"), so no click ever depends on the viewport's mouse capture.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Fonts/SlateFontInfo.h"
#include "Input/NavigationReply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SLeafWidget.h"
#include "PinguMenu3D.generated.h"

class APenguinHUD;
enum class EPinguScreen : uint8;
class UDirectionalLightComponent;
class UTexture;
class UFont;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPostProcessComponent;
class UPrimitiveComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UText3DComponent;

namespace PinguMenu3D
{
	/** The 3D title and lobby are on: not -PinguOldMenus, not [PinguMenus] bOldMenus in the game config, and the assets are there
	 *  (/Game/UI/Menu3D: the slab pieces and M_Menu3D). Decided once per run. */
	bool IsEnabled();
	/** The menus are drawn in 2D (2026-10-27, card 2026-10-27_gui-2d; PinguMenu2D.h): the default. -Pingu3DMenus, [PinguMenus]
	 *  b3DMenus=True in the game config or the environment variable PINGU_3D_MENUS=1 give the 3D menus (real meshes) back.
	 *  Decided once per run; only means something while IsEnabled(). */
	PENGUIN_API bool IsFlat();
	/** The lobby island (2026-10-23 .. 10-29: the penguins on a small island, tags over their heads, a strip of signs) instead of
	 *  the wooden card lobby (the default since 2026-10-29, Linus: "i think the old lobby was better, can you use the old layout
	 *  and things but with the new wooden style"): -PinguIslandLobby, [PinguMenus] bIslandLobby=True or PINGU_ISLAND_LOBBY=1.
	 *  Only while IsEnabled() (-PinguOldMenus keeps the old Slate card lobby). Decided once per run. */
	PENGUIN_API bool UseIslandLobby();
	/** Everything made while one of these lives is real 3D even with the 2D menus: slabs, rings and texts (the lobby's yellow "+"
	 *  sphere with its 3D "+", Linus 2026-10-29: "make the + in the lobby + sphere 3d like before again"). */
	struct PENGUIN_API FForce3DScope
	{
		FForce3DScope();
		~FForce3DScope();
	};
	/** The 3D text's font: /Game/UI/Fonts/TitanOne, or (asset missing) a runtime font made from the TTF Slate uses. */
	UFont* GetFont();
	/** The menu's words as a crisp Slate overlay (default since 2026-10-26, third pass) or as the 3D letters (tests' before / after). */
	PENGUIN_API bool IsTextOverlayOn();
	PENGUIN_API void SetTextOverlay(bool bOn);
	/** One text drawn by the overlay (default) or as its own 3D letters (the lobby's "+": its 3D look, Linus 2026-10-27). */
	PENGUIN_API void SetTextInOverlay(USceneComponent* Text, bool bOverlay);
	/** The lit menu material /Game/UI/Menu3D/M_Menu3D: vector Color (front), Lip (where the mesh's vertex colour R is 1: a slab's
	 *  side walls), scalar Glow (emissive = colour x Glow), Roughness, Specular. */
	UMaterialInterface* GetMaterial();
	/** The wood of the signs and boards (2026-10-26, Linus's mockup): /Game/UI/Menu3D/M_Menu3D_Wood (M_Menu3D's Color / Lip /
	 *  Glow shaded by the grain texture T_UI3D_Wood, mapped on each slab's face; Grain, GrainScale, GrainOffset, Vertical,
	 *  PlankWidth, PlankDark, PlankTone). Falls back to GetMaterial() when it is not imported. */
	UMaterialInterface* GetWoodMaterial();
	/** The focus rope round a sign: /Game/UI/Menu3D/M_Menu3D_Rope (twisted strands; Color, Lip, Period, Glow), else GetMaterial(). */
	UMaterialInterface* GetRopeMaterial();
	/** The wooden signs' extra pieces (make_ui_meshes.py): the arrow point, the focus arrow, the snow slab, a nail head, an icicle. */
	enum class EPiece : uint8 { ArrowTip, Pointer, SnowCap, Nail, Icicle };
	UStaticMesh* GetPieceMesh(EPiece Piece);
	/** A plank sign's corner radius against its height (the arrow point's bevel is made for it: make_ui_meshes.py SIGN_RADIUS),
	 *  and the point's length against the height (TIP_LEN / 2) and its straight base under the plank's end (TIP_BASE / 2). */
	constexpr float SignRadiusShare = 0.2f;
	constexpr float TipLengthShare = 0.4f;
	constexpr float TipBaseShare = 0.275f;
	/** The wood menus' colours the pages share (linear): the dark stain of wells, rows and strips (light text reads on it) and the
	 *  letters' dark brown edge. */
	inline const FLinearColor WoodWell(0.075f, 0.04f, 0.02f);
	inline const FLinearColor WoodWellLip(0.03f, 0.016f, 0.008f);
	inline const FLinearColor WoodRow(0.12f, 0.065f, 0.03f);
	inline const FLinearColor WoodRowLip(0.045f, 0.023f, 0.011f);
	inline const FLinearColor WoodEdge(0.022f, 0.011f, 0.005f);
	/** A tag's words on its dark wooden pill (2026-10-26, fourth pass: the pills are stained wood, the tag's colour - red for a
	 *  knock-out, ... - is in its words), made light enough to read. */
	inline FLinearColor TagTextColor(const FLinearColor& TagColor)
	{
		FLinearColor Color = FMath::Lerp(TagColor, FLinearColor::White, 0.3f);
		for (int32 Step = 0; Step < 4 && Color.GetLuminance() < 0.35f; ++Step)
		{
			Color = FMath::Lerp(Color, FLinearColor::White, 0.3f);
		}
		Color.A = 1.f;
		return Color;
	}
	/** The lighting channel only the menu's own light reaches (menu parts are lit by it and the sky light, never by the sun). */
	constexpr int32 MenuLightChannel = 2;
	/** A page's plane: its corner radius and depth (design pixels); button labels' depth as a share of their capitals. */
	constexpr float PlaneRadius = 30.f;
	constexpr float PlaneDepth = 36.f;
	constexpr float LabelDepth = 0.22f;
	/** A menu part (bMenuLight: the menu's own light only) or a world part (channel 0, like the island): no shadows, no
	 *  collision, never in the way of the camera or a trace. */
	void PreparePrimitive(UPrimitiveComponent* Primitive, bool bMenuLight);

	// ---- the mouse (see the file comment)
	/** A Slate screen-space position (what a pointer event or FSlateApplication::GetCursorPos gives) as a pixel of this world's
	 *  game viewport (what APlayerController::GetMousePosition / DeprojectScreenPositionToWorld use), through the viewport
	 *  widget's own geometry: its place in the window and its DPI scale. False without a viewport. */
	PENGUIN_API bool ScreenToViewportPixel(const UWorld* World, const FVector2D& ScreenPosition, FVector2D& OutPixel);
	/** The same mapping with any geometry (tests: a made-up viewport geometry with an offset and a DPI scale, as a PIE viewport
	 *  inside the editor has). */
	PENGUIN_API FVector2D PixelFromGeometry(const FGeometry& ViewportGeometry, const FVector2D& ScreenPosition);
	PENGUIN_API FVector2D ScreenFromGeometry(const FGeometry& ViewportGeometry, const FVector2D& Pixel);
	/** The other way: a viewport pixel as a Slate screen-space position (false without a viewport). */
	PENGUIN_API bool ViewportPixelToScreen(const UWorld* World, const FVector2D& Pixel, FVector2D& OutScreenPosition);
	/** The viewport pixel the OS cursor is over now (false: no cursor, no viewport). */
	PENGUIN_API bool GetCursorViewportPixel(const UWorld* World, FVector2D& OutPixel);
	/** The viewport's size in pixels (false: none yet). */
	PENGUIN_API bool GetViewportPixelSize(const APlayerController* Controller, FVector2D& OutSize);

	enum class ETextAlign : uint8 { Left, Center, Right };

	/** How a 3D text looks. Sizes are in the parent's units. */
	struct FTextLook
	{
		/** The height of the capitals. The text's origin is on its baseline (capitals stand on it and reach up Size); horizontally
		 *  at its left end, middle or right end (Align). */
		float Size = 40.f;
		/** Depth of the letters and the rounding of their front edges, as shares of Size. */
		float Extrude = 0.2f;
		float Bevel = 0.05f;
		FLinearColor Front = FLinearColor::White;
		/** The letters' sides (their extrusion). */
		FLinearColor Side = FLinearColor(0.004f, 0.009f, 0.03f);
		float Glow = 0.f;
		ETextAlign Align = ETextAlign::Center;
		/** The text is never wider than this (scaled down to fit); 0: no limit. */
		float MaxWidth = 0.f;
		/** True: the 3D menu's own light (lighting channel MenuLightChannel). False: lit like the world (name tags on the island). */
		bool bMenuLight = true;
		/** Drawn by the menu's Slate overlay (crisp at any resolution; the 3D glyphs only keep the layout) - 2026-10-26: the menu's
		 *  words and the lobby's name and difficulty tags. */
		bool bOverlay = true;
		/** A fat ink outline behind the letters (the logo's): the glyphs grown by this share of Size, OutlineDepth deep, in Side's
		 *  colour. 0: none. */
		float Outline = 0.f;
	};

	/** A 3D text (Text3D: real extruded, bevelled glyph meshes) attached to Parent. Its glyphs face the parent's -X (the front at
	 *  X = 0, the depth toward +X), it reads along +Y, up is +Z (to face the stage camera, which looks from +Y toward -Y, give it a
	 *  yaw of -90). Built at once (no frame without letters). */
	UText3DComponent* MakeText(AActor* Owner, USceneComponent* Parent, const FText& Text, const FTextLook& Look);
	/** The menu's texts: MakeText with the 3D menus; with the 2D menus (IsFlat) a plain transform node that keeps the same layout
	 *  (its width measured with the Slate font: the overlay draws the words) - no glyph meshes at all. */
	USceneComponent* MakeMenuText(AActor* Owner, USceneComponent* Parent, const FText& Text, const FTextLook& Look);
	/** Changes the text (rebuilt at once; the look and the width limit stay). Nothing happens when the text is the same. These
	 *  take a MakeText or a MakeMenuText text. */
	void SetText(USceneComponent* Text, const FText& NewText);
	void SetTextColors(USceneComponent* Text, const FLinearColor& Front, const FLinearColor& Side, float Glow = 0.f);
	/** The width of the text as shown (parent units, its scale to fit included); 0 before it is built. */
	float GetTextWidth(USceneComponent* Text);
	/** A text drawn raised in 2D (not by the overlay: the lobby's "+" on its ball): its component, its words, colours and the box
	 *  of its capitals in its own space (Y from MinY to MaxY, Z from Baseline up Cap). */
	struct FRaisedText
	{
		const USceneComponent* Component = nullptr;
		FString String;
		FLinearColor Front = FLinearColor::White;
		FLinearColor Side = FLinearColor::Black;
		float MinY = 0.f;
		float MaxY = 0.f;
		float Baseline = 0.f;
		float Cap = 1.f;
	};
	/** Owner's texts that show and are not overlay words (2D menus only; every part they hang on shows). */
	PENGUIN_API void GetRaisedTexts(const AActor* Owner, TArray<FRaisedText>& Out);

	/**
	 * A rounded, bevelled slab of any size made of the four 9-slice pieces (/Game/UI/Menu3D/SM_UI3D_*): its front face is the
	 * rectangle Width x Height centred on its root's origin at X = 0 (facing -X), its corners rounded with Radius, its front
	 * edge bevelled, its side walls going back Depth toward +X. Parts are owned by the actor that built it.
	 */
	struct FSlab
	{
		USceneComponent* Root = nullptr;
		UStaticMeshComponent* Parts[9] = {};
		UMaterialInstanceDynamic* Material = nullptr;
		float Width = 0.f;
		float Height = 0.f;
		float Radius = 0.f;
		float Depth = 0.f;

		/** Makes the parts (false: the assets are missing). bShadow: a soft contact shadow instead of a lit slab (the translucent
		 *  M_Menu3D_Shadow: black on the face, fading out across the bevel; SetOpacity sets how dark). */
		/** Wood (M_Menu3D_Wood) instead of the plain menu material: its parts carry their place in the slab (custom primitive
		 *  data SlabMap) so the grain runs on across the joins. */
		bool bWood = false;

		bool Build(AActor* Owner, USceneComponent* Parent, bool bMenuLight = true, bool bShadow = false, bool bInWood = false);
		void SetShape(float InWidth, float InHeight, float InRadius, float InDepth);
		void SetColors(const FLinearColor& Face, const FLinearColor& Lip, float Glow = 0.f);
		/** The wood's look (a wood slab only): how strong the grain is (painted signs less), where the grain sits (a sign's own
		 *  offset), upright planks of PlankWidth with dark grooves and a tone per plank (boards; PlankDark 0: one plank). */
		void SetWood(float Grain, const FVector2D& GrainOffset, bool bVertical = false, float PlankWidth = 100.f, float PlankDark = 0.f,
			float PlankTone = 0.f, const FVector2D& GrainScale = FVector2D(520.f, 150.f));
		void SetOpacity(float Opacity);
		/** The rendered wood's picture (2D menus only, PinguWoodStyle.h): its cut (PinguWood::ECut), state (PinguWood::EState)
		 *  and, for board planks, which one. */
		void SetWoodImage(uint8 Cut, uint8 State, uint8 Variant = 0);
		/** The grain's strength alone (a sign's grain follows its paint). */
		void SetGrain(float Grain);
		void SetVisible(bool bVisible);
		bool IsBuilt() const { return Root != nullptr; }
	};

	/**
	 * The selection ring: a round tube running round a rounded rectangle at a gap (the pieces /Game/UI/Menu3D/SM_UI3D_Ring*).
	 * Its centre line is the rectangle (InnerWidth + 2 (Gap + Tube)) x (InnerHeight + 2 (Gap + Tube)) with the corner radius
	 * InnerRadius + Gap + Tube, in the plane X = 0 of its root (the tube half in front of it, half behind). The tube's radius
	 * follows the corner radius (the pieces' TUBE share, about 0.17).
	 */
	struct FRing
	{
		USceneComponent* Root = nullptr;
		UStaticMeshComponent* Parts[8] = {};
		UMaterialInstanceDynamic* Material = nullptr;
		float Tube = 0.f;
		float Width = 0.f;
		float Height = 0.f;

		/** bRope: the twisted rope material (M_Menu3D_Rope: the focus outline of the wood menus). */
		bool Build(AActor* Owner, USceneComponent* Parent, bool bMenuLight = true, bool bRope = false);
		/** Round a slab of InnerWidth x InnerHeight with the corner radius InnerRadius, Gap away from its edge. */
		void SetShape(float InnerWidth, float InnerHeight, float InnerRadius, float Gap);
		void SetColors(const FLinearColor& Front, const FLinearColor& Back, float Glow = 0.f);
		void SetVisible(bool bVisible);
		bool IsBuilt() const { return Root != nullptr; }
	};
}

/** Which page of the 3D menu shows (one at a time), and the overlays that show on top of a page (Capture, Error). */
UENUM()
enum class EPinguMenu3DPage : uint8
{
	None,
	Title,
	Lobby,
	Pause,
	Lan,
	Results,
	Settings,
	Controls,
	Pingupedia,
	/** The round board between rounds (the HUD's leaderboard; no screen: it shows with the RoundOver phase). */
	Board,
	/** Overlays: the PRESS A KEY card while a key is being bound, the ONLINE error popup. */
	Capture,
	Error,
	/** The settings page's "Apply changes?" board (2026-11-07; PinguMenu3DSettings.cpp): APPLY / DISCARD. */
	Confirm,
	/** The host's TRAITS list (2026-11-07, shared-traits; PinguMenu3DTraits.cpp): which traits are in the round bag. */
	TraitPicks,
	Count UMETA(Hidden)
};

/** What a 3D button looks like (as the Slate buttons of the same kind). */
enum class EPinguMenu3DKind : uint8
{
	Primary,  // orange (PLAY, START)
	Normal,   // blue
	Ghost,    // flat and dark (QUIT, the address line)
	Danger,   // red (LEAVE, CLOSE GAME)
	Toggle,   // TRAITS: dark when off, orange when on
	Row,      // an option row (settings): dark, its label on the left, a value with arrows on the right; Left / Right step it
	Segment,  // a tab or a preset: blue when selected, dark otherwise
	KeyCap,   // a key cap of the controls: dark, orange while that key is being bound
	Slider,   // a volume row: a track with a knob, a percentage; a click or drag on the track sets it, Left / Right step it
	Field,    // a text field (the LAN screen's name and address): typed into while it is hot, Enter does its action
	ListRow,  // a row of a list (a found game, a PINGUPEDIA entry): dark; a selected one has a gold rim
	Arrow,    // a small arrow button next to a value (not focusable: the keys step the row itself)
	Plain,    // only its label, no plank, no shadow (2026-11-07: the lobby's "+" and the kick / remove x): it pops, tilts and brightens as the others do
};

/** A 3D button as the tests see it. */
struct FPinguMenu3DButtonInfo
{
	FName Id;
	FString Label;
	/** A row's value text (option rows: "100%"; a field: its text). */
	FString Value;
	bool bShown = false;
	bool bFocusable = false;
	/** Not greyed out (the settings page's APPLY is disabled while nothing is changed). */
	bool bEnabled = true;
	bool bSelected = false;
	/** The hot button (hovered or keyboard focused: one at a time) and its focus rope (0..1: shows for a keyboard / controller
	 *  focus, not for the mouse - the wood menus, 2026-10-26). */
	bool bHot = false;
	float Outline = 0.f;
	/** The mouse hovers it (its tilt, degrees: the hovered sign tilts and wiggles), the yellow focus arrow points at it. */
	float Tilt = 0.f;
	bool bFocusArrow = false;
	/** How far it stands out toward the camera (design pixels; more while hot) and how far a press pushed it in (0..1). */
	float Pop = 0.f;
	float Push = 0.f;
	/** Its face's middle and size on the screen (0..1 of the viewport). */
	FVector2D ScreenCenter = FVector2D::ZeroVector;
	FVector2D ScreenSize = FVector2D::ZeroVector;
};

/** One of the menu's texts as the Slate overlay draws it (2026-10-26, third pass): the words, their colour, and where the
 *  capitals' box of the 3D text is on the screen (viewport pixels: its top left, top right and bottom left on the baseline). */
struct FPinguMenu3DTextDraw
{
	FString Text;
	FLinearColor Color = FLinearColor::White;
	FVector2D TopLeft = FVector2D::ZeroVector;
	FVector2D TopRight = FVector2D::ZeroVector;
	FVector2D BottomLeft = FVector2D::ZeroVector;
	/** A heading or the logo (a thicker edge). */
	bool bBig = false;
};

/**
 * The 3D title and lobby controls (see the file comment). One per local UI (the first player's HUD), spawned by the Slate root
 * when it builds the 3D screens. It follows the camera every frame (after the camera moved), on a plane MenuDistance in front of
 * it, scaled so that one unit of its pages is one pixel of a 1920 wide picture (whatever the resolution: geometry, so always
 * crisp). Lit by its own light (lighting channel 2) and the sky; no shadows, no collision, opaque.
 */
UCLASS(NotPlaceable, Transient)
class PENGUIN_API APinguMenu3D : public AActor
{
	GENERATED_BODY()

public:
	APinguMenu3D();

	/** This HUD's 3D menu (spawned on the first call). Null without a world. */
	static APinguMenu3D* Get(APenguinHUD* HUD);
	/** The world's 3D menu, if there is one (tests). */
	static APinguMenu3D* Find(const UWorld* World);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// ---- input (from SPinguMenu3DInput: the pointer as 0..1 of the game's viewport, or as a viewport pixel)
	/** The pointer moved: the button under it becomes the hot one (hover takes the focus, as in the Slate menus). */
	void PointerMove(const FVector2D& ViewportUV);
	void PointerMovePixel(const FVector2D& ViewportPixel);
	void PointerLeave();
	/** A left click: presses the button under the pointer. False: no button there. */
	bool PointerDown(const FVector2D& ViewportUV);
	bool PointerDownPixel(const FVector2D& ViewportPixel);
	/** A press is still pushing its button in (its action has not fired yet; another press waits). */
	bool IsPressPending() const { return PendingPress != INDEX_NONE; }
	/** Up / Down / Left / Right (and Next / Previous) from the keys, the d-pad or the stick: the focus moves to the nearest
	 *  button that way. True when it moved. */
	bool Navigate(EUINavigation Direction);
	/** Enter / gamepad A: presses the hot button. */
	bool Accept();

	/** A 3D button shows under this point of the viewport (pixels): the lobby island leaves clicks there to the menu. */
	static bool HasButtonAt(const UWorld* World, const FVector2D& ViewportPixel);

	/** The page that shows (None: nothing). */
	EPinguMenu3DPage GetPage() const { return Page; }
	/** The focus goes to this button of the page that shows (true: found and focusable). */
	bool FocusButton(FName Id);
	/** The focus goes to the page's default button (title: PLAY; lobby: START for the host / solo, else SETTINGS). */
	void FocusDefault();
	FName GetHotId() const;
	/** The overlay that shows on top of the page (None: none). */
	EPinguMenu3DPage GetOverlay() const { return Overlay; }

	// ---- more input (from SPinguMenu3DInput)
	/** The mouse moved while this menu holds it after a press (a slider's knob follows). */
	void PointerDragPixel(const FVector2D& ViewportPixel);
	void PointerUp();
	/** The wheel over a list (the PINGUPEDIA's): Delta rows (positive: up). True when a list took it. */
	bool Scroll(float Delta);
	/** A typed character for the hot text field. True when a field took it. */
	bool TypeChar(TCHAR Char);
	/** A key for the hot text field (Backspace, Delete, Ctrl+V). True when a field took it. */
	bool FieldKey(const FKeyEvent& KeyEvent);
	/** The hot button is a text field: letters are typed, not shortcuts (P must not back out). */
	bool IsTyping() const;
	/** The settings' pages (GRAPHICS / DISPLAY / AUDIO): a tab, LB / RB; bFocusFirst puts the focus on the page's first control. */
	void SelectSettingsTab(int32 Tab, bool bFocusFirst);
	int32 GetSettingsTab() const { return SettingsTab; }
	/** The PINGUPEDIA: show Page (0 abilities, 1 traits) with Entry selected (clamped), the list scrolled to it; bFocus makes its row hot. */
	void SelectPingupediaPage(int32 Page, int32 Entry, bool bFocus);
	/** The PINGUPEDIA's page and entry that show; true when the entry's row is inside the list's window (the menushots check). */
	bool GetPingupediaView(int32& OutPage, int32& OutEntry) const;
	/** The LAN screen's name field written back to the HUD (before hosting / joining / leaving the screen). */
	void CommitFields();
	/** The flat texts the Slate overlay draws on the page that shows (PinguMenu3DOverlay): their count, each one's text, look and
	 *  where its box is on the screen (viewport pixels: the top left, the top right and the bottom left corner of the box as the
	 *  3D panel stands now). FlatVersion changes whenever the set of flats changes (a page switch, a rebuilt list). */
	int32 GetFlatVersion() const { return FlatVersion; }
	int32 GetFlatCount() const;
	bool GetFlat(int32 Index, FText& OutText, FSlateFontInfo& OutFont, FLinearColor& OutColor, int32& OutJustify, bool& OutVisible) const;
	bool GetFlatFrame(int32 Index, FVector2D& OutTopLeft, FVector2D& OutTopRight, FVector2D& OutBottomLeft) const;
	/** Every text of the menu that shows now, where it is on the screen (the overlay draws them crisp at the screen's own
	 *  resolution; the 3D glyphs are hidden and keep the layout). */
	void GetOverlayTexts(TArray<FPinguMenu3DTextDraw>& Out) const;
	/** Tests (2026-10-26, fourth pass): how many of the input page's shown texts run past the box they stand in (their sign or
	 *  row, a strip or pill, the board; Margin in from the ends); OutChecked how many were looked at, OutList the misfits. */
	int32 CountTextMisfits(int32& OutChecked, FString& OutList, float Margin = 1.f) const;
	/** A button's label: the middle of its capitals on the screen (viewport pixels; tests). */
	bool GetButtonTextPixel(int32 Index, FVector2D& OutCenter) const;
	/** A flat's box size in page units (what its text was laid out with). */
	bool GetFlatSize(int32 Index, float& OutWidth, float& OutHeight) const;

	// ---- tests
	/** Shows a page whatever the HUD's screen says (None: back to the HUD). With OnAction bound the buttons call it with their
	 *  id instead of doing their action. */
	void SetTestPage(EPinguMenu3DPage InPage, TFunction<void(FName)> OnAction = nullptr);
	int32 GetButtonCount() const;
	FPinguMenu3DButtonInfo GetButtonInfo(int32 Index) const;
	/** The viewport pixel of a point of a button's face right now (FaceShare: -1..1 across its width and height; the face as it
	 *  stands, popped out or pushed in, the card's sway and parallax included). False: no such button or behind the camera. */
	bool GetButtonFacePixel(int32 Index, const FVector2D& FaceShare, FVector2D& OutPixel) const;
	/** The ids of the page's focusable buttons in their order from the top (title) / from the left (lobby). */
	TArray<FName> GetFocusOrder() const;
	/** The 3D page of a HUD screen (None: the screen stays Slate - the in-fight HUD). */
	static EPinguMenu3DPage PageOfScreen(EPinguScreen Screen);
	static const TCHAR* PageName(EPinguMenu3DPage InPage);
	/** The page's plane (its backplane) as it stands on the screen: its middle and size (0..1 of the viewport). */
	bool GetPlaneScreenRect(FVector2D& OutCenter, FVector2D& OutSize) const;
	/** The real frame time (ms, averaged over the last second) while a page shows: the fps note of the pause / results pages. */
	float GetShownFrameMs() const { return ShownFrameMs; }
	/** How many presses fired their action so far (never reset). */
	int32 GetActionsFired() const { return ActionsFired; }

	// ---- the 2D menus (PinguMenu2D: the painter)
	/** The menu's own camera in 2D (the actor stands still; the real camera's field of view) and the viewport's size. */
	bool GetFlatView(FVector& OutOrigin, FRotator& OutRotation, float& OutTanX, float& OutTanY, FVector2D& OutSize) const;
	/** The player's camera this frame (what the island is seen through). */
	bool GetCameraView(FVector& OutOrigin, FRotator& OutRotation, float& OutTanX, float& OutTanY) const { return GetView(OutOrigin, OutRotation, OutTanX, OutTanY); }
	/** How dark the game is behind the page (0..1; the 3D menu's post-process dim). */
	float GetFlatDim() const { return DimAmount; }
	const USceneComponent* GetMenuRootComponent() const { return MenuRoot; }
	/** The player whose view the menu stands in. */
	APlayerController* GetMenuController() const { return GetController(); }
	/** The overlay page's root (PRESS A KEY, the error popup) while one shows. */
	const USceneComponent* GetOverlayRootComponent() const;
	/** Tests: the menu's render parts (primitive components: 0 with the 2D menus) and its 2D parts. */
	void CountParts(int32& OutPrimitives, int32& OutFlatParts) const;
	/** Tests (the frame time without any menu): nothing of the menu shows - 3D: the actor hidden, 2D: nothing painted. */
	void SetHiddenForTest(bool bHide);
	bool IsHiddenForTest() const { return bHiddenForTest; }

	/** The menu's distance from the camera (cm) and how wide its design picture is (units). */
	static constexpr float MenuDistance = 200.f;
	static constexpr float DesignWidth = 1920.f;

private:
	struct FButton
	{
		FName Id;
		EPinguMenu3DKind Kind = EPinguMenu3DKind::Normal;
		/** Its face's middle (page units) and size; the label's capital height. */
		FVector2D Center = FVector2D::ZeroVector;
		FVector2D Size = FVector2D(416.f, 60.f);
		float LabelSize = 24.f;
		TFunction<FText()> Label;
		TFunction<void()> Action;
		/** Shown at all (null: always), focusable / pressable (null: always), on (a toggle). */
		TFunction<bool()> Visible;
		TFunction<bool()> Enabled;
		TFunction<bool()> Selected;
		/** The action fires at the press (a copy: tests read the clipboard right after), not after the push-in. */
		bool bInstant = false;
		/** Takes the keyboard focus / is hot at all (an arrow button is not: the keys step its row). */
		bool bFocusable = true;
		/** A row's value text on its right ("100%"), and how wide its box is. */
		TFunction<FText()> Value;
		float ValueWidth = 168.f;
		/** Left / Right (keys, d-pad, stick) while hot: steps a row / slider / list (-1 / +1). */
		TFunction<void(int32)> OnStep;
		/** Called when the button becomes hot (a PINGUPEDIA row selects its entry). */
		TFunction<void()> OnHot;
		/** The label's place: centred (buttons) or at the left edge with LabelPad (rows); its colour (null: by the kind). */
		PinguMenu3D::ETextAlign LabelAlign = PinguMenu3D::ETextAlign::Center;
		float LabelPad = 16.f;
		TFunction<FLinearColor()> LabelColor;
		/** No ink outline behind the label (small row texts read better without). */
		bool bNoInk = false;
		/** How far it stands out of the plane (< 0: by its kind). */
		float Protrude = -1.f;
		/** Plain buttons (EPinguMenu3DKind::Plain): the label's colour while hot (the hover and the focus, which look alike). */
		FLinearColor PlainHover = FLinearColor(1.f, 0.82f, 0.3f);
		/** A slider: its value (0..1) and the setter. */
		TFunction<float()> SliderValue;
		TFunction<void(float)> OnSlider;
		/** A text field: what is typed, the grey hint while empty, the most characters, the change callback. */
		FString FieldText;
		FText FieldHint;
		int32 FieldMax = 64;
		TFunction<void(const FString&)> OnFieldChanged;
		/** A list's row: which list (the page's list id) and its place in it (navigation and scrolling go by these). */
		int32 ListId = INDEX_NONE;
		int32 ListIndex = INDEX_NONE;
		/** Stacked pages (pause): the gap above this button; the page stacks its visible buttons from the top. */
		float StackGap = -1.f;

		USceneComponent* Root = nullptr;
		USceneComponent* Body = nullptr;
		/** Where the clicks are traced (and the tests click): the face where it is pushed or popped to, but never turned - the
		 *  hover tilt, the wiggle, the sway and the page's drop-in only move the picture (Body), never a click target. */
		USceneComponent* Hit = nullptr;
		PinguMenu3D::FSlab Slab;
		/** The wooden sign (2026-10-26, Linus's mockup): its arrow point (+1 right, -1 left, 0 none) and the parts that make it
		 *  a sign - the point, the snow slab on its top edge, two nails, a post under it (the lobby strip). */
		int32 Point = 0;
		bool bSign = false;
		bool bSnow = false;
		bool bNails = false;
		/** A toggle row of a dense grid (the trait LIST): the row plank (row_hover when hot), no snow (docs/UI_STYLE.md). */
		bool bRowPlank = false;
		bool bShadowed = true;
		UStaticMeshComponent* Tip = nullptr;
		UStaticMeshComponent* Snow = nullptr;
		UStaticMeshComponent* Nails[2] = {};
		PinguMenu3D::FSlab Post;
		/** A small fixed tilt (degrees: hand-nailed signs are never quite straight) and its sway's phase. */
		float StaticRoll = 0.f;
		float SwayPhase = 0.f;
		/** Its place in the page's drop-in order (signs come one after another). */
		int32 DropOrder = 0;
		/** Eased: the keyboard focus (the rope), the mouse hover (the tilt); how long the hover has lasted (the wiggle). */
		float Focus = 0.f;
		float Hover = 0.f;
		float HoverT = 10.f;
		/** The hot ring round the button (a gap away from it) and its soft shadow on the plane. */
		PinguMenu3D::FRing Ring;
		PinguMenu3D::FSlab Shadow;
		/** The selected list row's gold rim. */
		PinguMenu3D::FRing SelectRing;
		USceneComponent* Text = nullptr;
		/** The label's ink outline (the letters grown, dark, just behind them: the old labels' ink shadow). */
		USceneComponent* Ink = nullptr;
		USceneComponent* ValueText = nullptr;
		/** A slider's track and knob; a field's caret. */
		PinguMenu3D::FSlab Track;
		PinguMenu3D::FSlab Knob;
		PinguMenu3D::FSlab Caret;
		FString ShownLabel;
		FString ShownValue;
		bool bShown = false;
		float Hot = 0.f;      // 0..1 eased
		float PushT = 10.f;   // seconds since the last press
	};

	/** A 3D text of a page (a title, a section heading, a status line). */
	struct FLabel
	{
		USceneComponent* Text = nullptr;
		USceneComponent* Ink = nullptr;
		TFunction<FText()> Label;
		TFunction<bool()> Visible;
		TFunction<FLinearColor()> Color;
		TFunction<float()> Glow;
		FString Shown;
		FLinearColor ShownColor = FLinearColor::Black;
		FLinearColor Side = FLinearColor::Black;
		float ShownGlow = 0.f;
		bool bShown = true;
	};

	/** A flat Slate text on the face of a 3D panel (SPinguMenu3DOverlay draws it): its box in the space of a part of the page. */
	struct FFlat
	{
		USceneComponent* Space = nullptr;
		/** The box: its left and top in Space (page units; Y right, Z up: Top is a Z), its size. */
		float Left = 0.f;
		float Top = 0.f;
		float Width = 100.f;
		float Height = 20.f;
		TFunction<FText()> Text;
		TFunction<bool()> Visible;
		TFunction<FLinearColor()> Color;
		bool bTitleFont = true;
		float Size = 16.f;
		/** 0 left, 1 centre, 2 right. */
		int32 Justify = 0;
	};

	/** A decorative part of a page (a well, a pill, a row's background, a pip, an icon) with its own show rule. */
	struct FDeco
	{
		PinguMenu3D::FSlab Slab;
		PinguMenu3D::FRing Ring;
		/** A big wooden well (a list, a card) is a few stained planks of different lengths instead of one box. */
		TArray<PinguMenu3D::FSlab> Extra;
		UStaticMeshComponent* Icon = nullptr;
		UMaterialInstanceDynamic* IconMaterial = nullptr;
		USceneComponent* Root = nullptr;
		TFunction<bool()> Visible;
		TFunction<FLinearColor()> Color;
		FLinearColor Lip = FLinearColor::Black;
		FLinearColor ShownColor = FLinearColor::Black;
		bool bShown = true;
	};



	struct FPageParts
	{
		EPinguMenu3DPage Id = EPinguMenu3DPage::None;
		USceneComponent* Root = nullptr;
		/** The page's backing: a framed board of upright wooden planks (2026-10-26; before: a navy plane) - its frame and the
		 *  snow slab on the frame's top. The title and the lobby strip have none (signs on posts): bNoBoard. */
		PinguMenu3D::FSlab Plane;
		PinguMenu3D::FSlab Frame[4];
		UStaticMeshComponent* FrameSnow = nullptr;
		/** (2026-10-26, second pass: Linus "dont make so clean boxes ... make on diffrent distance cut off wodden planks") The
		 *  board is loose horizontal planks of different lengths, each a little askew, nailed on: PlankCount of them in use. */
		TArray<PinguMenu3D::FSlab> Planks;
		TArray<UStaticMeshComponent*> PlankNails;
		int32 PlankCount = 0;
		/** The dark wood behind a board's planks (it shows in the gaps between them). */
		PinguMenu3D::FSlab Backing;
		bool bNoBoard = false;
		/** The board's planks: their height (0: the usual BoardRow) and how much longer than the board they may be (< 0: the usual 70). */
		float PlankRow = 0.f;
		float PlankOverhang = -1.f;
		/** The hot button got hot from the mouse (a hover: the sign tilts) or from the keys (a focus: the rope and the arrow). */
		bool bHotByMouse = false;
		/** The cursor is on the mouse-hot sign (it tilts only while the cursor is on it). */
		bool bCursorOnHot = true;
		/** Seconds since the page showed (the signs' drop-in). */
		float OpenT = 0.f;
		/** Where the page stands (an offset from the middle of the picture, page units) and how it is turned. */
		FVector2D PlaceOffset = FVector2D::ZeroVector;
		float Pitch = 0.f;
		float Yaw = 0.f;
		TArray<FButton> Buttons;
		TArray<FLabel> Labels;
		TArray<FFlat> Flats;
		TArray<FDeco> Decos;
		int32 Hot = INDEX_NONE;
		float Appear = 0.f;
		bool bShown = false;
		bool bBuilt = false;
		/** The default hot button's id (the page's first focus). */
		FName DefaultFocus;
		/** A list's scroll: the first row shown and how many rows fit (0: no list); where its first shown row's middle is (page
		 *  units on the list's root) and the step between rows; how many rows it has. */
		int32 ListScroll = 0;
		int32 ListWindow = 0;
		float ListTop = 0.f;
		float ListStep = 56.f;
		int32 ListCount = 0;
		/** Which list's rows the window shows (the PINGUPEDIA has one list per page; a row's ListId). */
		int32 ActiveList = 0;
		/** Stacked pages: the page stacks its visible buttons (StackGap) from StackTop down (page Z); the plane's top edge stays
		 *  at StackPlaneTop and its height follows the stack, StackBottom under the last button. */
		bool bStack = false;
		float StackTop = 0.f;
		float StackPlaneTop = 0.f;
		float StackBottom = 0.f;
		/** The first dynamic element of each array (everything from there on belongs to the rows built last) and their key. */
		int32 StaticButtons = 0;
		int32 StaticLabels = 0;
		int32 StaticFlats = 0;
		int32 StaticDecos = 0;
		FString RowsKey;
		/** The parts the rows hang on (destroyed with them). */
		TArray<USceneComponent*> RowRoots;
	};

	void BuildTitle();
	void BuildLobby();
	/** The wooden card lobby (PinguMenu3DLobby.cpp, 2026-10-29): the Oct 22 card panel in wood - a card per player, ADD BOT, the
	 *  signs, the address line - over the title's island (no lobby island). UpdateCardLobby rebuilds the cards when they change. */
	void BuildCardLobby();
	void UpdateCardLobby(FPageParts& Parts);
	// ---- the other pages (PinguMenu3DPages.cpp, PinguMenu3DSettings.cpp, PinguMenu3DPedia.cpp)
	void BuildPause();
	void BuildLan();
	void BuildResults();
	void BuildBoard();
	void BuildCapture();
	void BuildError();
	void BuildConfirm();
	void BuildSettings();
	void BuildControls();
	void BuildPingupedia();
	/** The host's TRAITS list (PinguMenu3DTraits.cpp). */
	void BuildTraitPicks();
	/** Per-frame work of a page (its dynamic rows, its lists' windows). */
	void UpdatePage(FPageParts& Parts, float Delta);
	void UpdateLan(FPageParts& Parts);
	void UpdateResults(FPageParts& Parts);
	void UpdateBoard(FPageParts& Parts);
	void UpdateSettings(FPageParts& Parts);
	void UpdatePingupedia(FPageParts& Parts);
	void UpdateCapture(FPageParts& Parts);
	/** The PINGUPEDIA's detail card for the selected entry (rebuilt when it changes). */
	void RebuildPediaDetail(FPageParts& Parts);
	/** The PINGUPEDIA's fit numbers (every card measured once: PinguUI::FPingupediaStats). */
	void MeasurePediaCards();
	/** Makes a page's parts if not yet (the title and the lobby at BeginPlay, the others on their first show). */
	void EnsureBuilt(FPageParts& Parts);
	/** A page's root and plane. */
	void BeginPage(FPageParts& Parts, EPinguMenu3DPage Id, const FVector2D& PlaneSize);
	/** Marks the end of a page's static parts: what comes after belongs to its dynamic rows. */
	void EndStatic(FPageParts& Parts);
	FButton& AddButton(FPageParts& Parts, FName Id, EPinguMenu3DKind Kind, const FVector2D& Center, const FVector2D& Size, float LabelSize,
		TFunction<FText()> Label, TFunction<void()> Action, USceneComponent* Parent = nullptr);
	/** A row with a value and arrows (settings). */
	FButton& AddOptionRow(FPageParts& Parts, FName Id, const FVector2D& Center, float Width, TFunction<FText()> Label, TFunction<FText()> Value,
		TFunction<void(int32, bool)> OnChange, float ValueWidth = 168.f, TFunction<bool()> Visible = nullptr);
	FButton& AddSlider(FPageParts& Parts, FName Id, const FVector2D& Center, float Width, TFunction<FText()> Label, TFunction<float()> Value,
		TFunction<void(float)> OnChanged);
	FButton& AddField(FPageParts& Parts, FName Id, const FVector2D& Center, const FVector2D& Size, const FText& Hint, int32 MaxLength,
		TFunction<void(const FString&)> OnChanged, TFunction<void()> OnEnter);
	int32 AddLabel(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, TFunction<FText()> Label,
		const FLinearColor& Color, PinguMenu3D::ETextAlign Align = PinguMenu3D::ETextAlign::Center, float MaxWidth = 0.f, bool bInk = false,
		float Protrude = 0.f);
	int32 AddFlat(FPageParts& Parts, USceneComponent* Space, float Left, float Top, float Width, float Height, TFunction<FText()> Text,
		bool bTitleFont, float Size, const FLinearColor& Color, int32 Justify = 0);
	/** A decorative slab (a disc when Radius is half the size), at Pos on Parent, Depth deep, its face at X = -Protrude. */
	int32 AddDecoSlab(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, const FVector2D& Size, float Radius, float Depth,
		const FLinearColor& Face, const FLinearColor& Lip, float Protrude = 0.f);
	/** A wooden slab (a row's plank, a well, a strip): M_Menu3D_Wood in Face / Lip, its grain Grain strong. */
	int32 AddDecoWood(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, const FVector2D& Size, float Radius, float Depth,
		const FLinearColor& Face, const FLinearColor& Lip, float Protrude = 0.f, float Grain = 0.8f, bool bPlanks = true);
	/** The page's board (plane, frame, snow) at this size; its middle stays where the plane's root is. */
	void SetPlaneShape(FPageParts& Parts, float Width, float Height);
	/** Lays loose planks over a Width x Height area of Parent (rows about RowHeight high, longer than the area by up to Overhang,
	 *  each shifted and a little askew; Seed makes every board its own): builds more planks into Planks as needed, hides the
	 *  rest; Nails (optional) get two per plank. Returns how many are in use. */
	int32 LayPlanks(TArray<PinguMenu3D::FSlab>& Planks, TArray<UStaticMeshComponent*>* Nails, USceneComponent* Parent, float Width, float Height,
		int32 Seed, const FLinearColor& Face, const FLinearColor& Lip, float Grain, float Depth, float RowHeight, float Overhang, bool bVisible);
	/** A button's sign parts (the plank, its point, snow, nails, rope, shadow, post) laid out for its Size and Point. */
	void ApplySignShape(FButton& Button);
	/** A static mesh part of the menu (a piece of make_ui_meshes.py, the engine's cylinder) with a material, on Parent. */
	UStaticMeshComponent* MakePart(USceneComponent* Parent, UStaticMesh* Mesh, UMaterialInterface* Material);
	/** A snow puff off a pressed sign (a few snow bits that fly and shrink: 0.45 s). */
	void SpawnPuff(const FButton& Button);
	void UpdatePuff(float Delta);
	/** The yellow arrow beside the keyboard-focused sign (it bobs). */
	void UpdateFocusArrow(float Delta);
	/** A ring (a rim) round a rectangle of Size with corner Radius at a gap, at Pos on Parent. */
	int32 AddDecoRing(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, const FVector2D& Size, float Radius, float Gap,
		const FLinearColor& Front, const FLinearColor& Back, float Protrude = 0.f);
	/** A textured square (an icon, a portrait): the engine's plane with M_Menu3D_Icon. */
	int32 AddDecoIcon(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, UTexture* Texture, const FLinearColor& Tint,
		float Protrude = 0.f);
	/** A portrait: a disc in the slot's colour, the face and its tinted beanie, a team ring when TeamRing >= 0. */
	void AddPortrait(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, int32 Slot, int32 TeamRing, float Protrude);
	/** A small pill with a word (tags: YOU, FULL, a team's name); the width it took. */
	float AddPill(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Height, float TextSize, const FText& Text,
		const FLinearColor& Fill, const FLinearColor& TextColor, float Protrude, PinguMenu3D::ETextAlign Anchor = PinguMenu3D::ETextAlign::Left);
	/** An entry's icon of the PINGUPEDIA (an ability's art on a dark disc in a ring of its colour, a trait's badge). */
	void AddEntryIcon(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, int32 PediaPageIndex, int32 Entry, float Protrude);
	/** Destroys a page's dynamic rows (everything after its static parts). */
	void ClearRows(FPageParts& Parts);
	void DestroyButton(FButton& Button);
	void DestroyLabel(FLabel& Label);
	void DestroyDeco(FDeco& Deco);
	void DestroyText(USceneComponent*& Text);
	void DestroySlab(PinguMenu3D::FSlab& Slab);
	void DestroyRing(PinguMenu3D::FRing& Ring);
	FPageParts* GetParts(EPinguMenu3DPage InPage);
	const FPageParts* GetParts(EPinguMenu3DPage InPage) const;
	/** The parts the input goes to: the overlay's while one shows, else the page's. */
	FPageParts* GetInputParts();
	const FPageParts* GetInputParts() const;
	EPinguMenu3DPage ComputePage() const;
	EPinguMenu3DPage ComputeOverlay() const;
	void ShowPage(EPinguMenu3DPage NewPage);
	void ShowOverlay(EPinguMenu3DPage NewOverlay);
	void UpdateLabels(FPageParts& Parts);
	void UpdateDecos(FPageParts& Parts);
	void UpdateStack(FPageParts& Parts);
	/** A page's place in front of the camera (its root): the title and the lobby have their own; the rest stand in the middle. */
	void PlacePage(FPageParts& Parts, float DesignHeight);
	void PlaceOnCamera();
	/** Every frame: the OS cursor's place in the viewport (through the viewport widget's geometry) makes the button under it hot
	 *  once the cursor moved (a keyboard pick stays until it does); on the lobby island (hover only) always, and a button that
	 *  pops out under a still cursor is noticed too. Also the parallax: the card turns a little toward the cursor. */
	void PollHover();
	/** The lobby island is no menu: its strip has hover only (no keyboard focus) while the keys are on the island. */
	bool IsMouseOnlyPage() const;
	void UpdateLobbyLayout(FPageParts& Parts);
	void UpdateButtons(FPageParts& Parts, float Delta);
	void UpdateButtonLook(FButton& Button, const FPageParts& Parts, bool bHot, float Delta);
	/** The page's board (plane, frame, snow) shows unless the page has none. */
	void ShowBoard(FPageParts& Parts);
	void SetButtonVisible(FButton& Button, bool bVisible);
	/** Puts the label (and its ink) at Y across the face (0: the middle), capitals centred on the face; the ink takes the letters' scale. */
	void PlaceLabel(FButton& Button, float Y);
	/** A slider's knob, a field's caret and text. */
	void UpdateSlider(FButton& Button);
	void UpdateField(FButton& Button, bool bHot);
	/** Sets a slider from a point on its face (page units across the button; a press or a drag). */
	void SetSliderFromFace(FButton& Button, float FaceY);
	/** The point of a button's face under the viewport pixel (page units from its middle); false: not on it. */
	bool TraceFacePoint(const FButton& Button, const FVector2D& ViewportPixel, FVector2D& OutFace) const;
	/** A list's rows: those inside the window show; Index into view. */
	void ScrollListTo(FPageParts& Parts, int32 Index);
	void ApplyListWindow(FPageParts& Parts);
	bool IsButtonLive(const FButton& Button) const;
	/** The button under the viewport point (its index in the page that shows, INDEX_NONE: none): the player controller's own
	 *  ray through the pixel, against a generous rectangle round each button's face (where it is now, popped out or pushed in,
	 *  and where it rests). */
	int32 TraceButton(const FVector2D& ViewportUV) const;
	int32 TraceButtonPixel(const FVector2D& ViewportPixel) const;
	/** The viewport pixel of a point (page units) of a scene component's plane X = 0. */
	bool ProjectLocal(const USceneComponent* Space, const FVector& Local, FVector2D& OutPixel) const;
	int32 TraceButtonRay(const FVector& Origin, const FVector& Direction) const;
	/** The viewport pixel of a point of the menu (false: behind the camera; the 2D menus: through their own camera). */
	bool ProjectToPixel(const FVector& World, FVector2D& OutPixel) const;
	/** The viewport pixel of a point of the world (the player's camera: the lobby's tags). */
	bool ProjectWorldToPixel(const FVector& World, FVector2D& OutPixel) const;
	APlayerController* GetController() const;
	void Press(int32 Index, bool bFromKey);
	void FireAction(FButton& Button);
	/** bMouse: a hover (the sign tilts), else a keyboard / controller focus (the rope and the arrow). */
	void SetHot(int32 Index, bool bSound, bool bMouse = false);
	int32 GetDefaultFocus() const;
	/** The camera's view this frame and the viewport's size: the ray through a viewport point, and the point back. */
	bool GetView(FVector& OutOrigin, FRotator& OutRotation, float& OutTanX, float& OutTanY) const;

	TWeakObjectPtr<APenguinHUD> HUD;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> MenuRoot;
	UPROPERTY(Transient)
	TObjectPtr<UDirectionalLightComponent> KeyLight;
	UPROPERTY(Transient)
	TObjectPtr<UDirectionalLightComponent> FillLight;

	FPageParts Title;
	FPageParts Lobby;
	FPageParts Pause;
	FPageParts Lan;
	FPageParts Results;
	FPageParts Settings;
	FPageParts Controls;
	FPageParts Pingupedia;
	FPageParts Board;
	FPageParts Capture;
	FPageParts Error;
	FPageParts Confirm;
	FPageParts TraitPicks;
	/** The overlay that shows on top of the page (Capture / Error) - it takes the input while it shows. */
	EPinguMenu3DPage Overlay = EPinguMenu3DPage::None;
	/** The settings' page that shows (0 graphics, 1 display, 2 audio). */
	int32 SettingsTab = 0;
	/** The PINGUPEDIA: the page (0 abilities, 1 traits) and the entry it shows, and what its detail card was built for. */
	int32 PediaPage = 0;
	int32 PediaEntry = 0;
	int32 PediaShownPage = -1;
	int32 PediaShownEntry = -1;
	/** The PINGUPEDIA's roots: the list's (its rows are buttons) and the detail card's. */
	USceneComponent* PediaListRoot = nullptr;
	USceneComponent* PediaDetailRoot = nullptr;
	/** The PINGUPEDIA's scroll bar (a track and a thumb: decos). */
	int32 PediaScrollTrack = INDEX_NONE;
	int32 PediaScrollThumb = INDEX_NONE;
	int32 FlatVersion = 0;
	/** A slider the mouse is dragging (INDEX_NONE: none). */
	int32 DragButton = INDEX_NONE;
	/** The caret's blink clock. */
	float CaretClock = 0.f;
	/** The icon material (M_Menu3D_Icon) and the engine's plane, loaded once. */
	TWeakObjectPtr<UMaterialInterface> IconMaterial;
	TWeakObjectPtr<UStaticMesh> IconMesh;
	/** The darkening behind a menu page over the game (and a little more under an overlay): a post-process material after the
	 *  tonemapper (M_Menu3D_DimPP) that darkens every pixel without a menu part (the parts write custom depth). A dark plane in
	 *  the scene did not work: the eye adaptation brightened everything back; the Slate dim darkened the 3D menu itself. */
	UPROPERTY(Transient)
	TObjectPtr<UPostProcessComponent> DimPostProcess;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DimMaterial;
	void UpdateDims();
	/** The dim UpdateDims last set (the 2D painter draws it). */
	float DimAmount = 0.f;
	bool bHiddenForTest = false;
	/** The 2D menus' virtual camera's place (the actor's place there) and the last scale given to the menu's root. */
	float FlatUnitsPerPixel = -1.f;
	/** Where the 2D menus' actor stands (far from everything; nothing of it renders or collides). */
	static inline const FVector FlatPlace = FVector(0.0, 0.0, 60000.0);
	/** A ray through a viewport pixel: the player's camera (3D) or the menu's own (2D). */
	bool DeprojectPixel(const FVector2D& ViewportPixel, FVector& OutOrigin, FVector& OutDirection) const;
	/** The real frame time (ms) averaged over the last second while a page shows (the fps note). */
	float ShownFrameMs = 0.f;
	double FrameAccum = 0.0;
	int32 FrameCount = 0;
	/** Extra parts of the pages (the logo, the lobby's waiting line and TRAITS display, the copy icon). */
	USceneComponent* LogoPingu = nullptr;
	USceneComponent* LogoBrawl = nullptr;
	USceneComponent* LogoPinguInk = nullptr;
	USceneComponent* LogoBrawlInk = nullptr;
	USceneComponent* WaitingText = nullptr;
	FString WaitingShown;
	PinguMenu3D::FSlab TraitsDisplay;
	PinguMenu3D::FSlab TraitsDisplayPost;
	/** The arrow point the next AddButton gives its sign (+1 right, -1 left; 0 after it). */
	int32 NextSignPoint = 0;
	/** The next AddButton is a toggle row (FButton::bRowPlank; false after it). */
	bool bNextRowPlank = false;
	USceneComponent* TraitsDisplayText = nullptr;
	FString TraitsDisplayShown;
	PinguMenu3D::FSlab CopyBack;
	PinguMenu3D::FSlab CopyFront;
	USceneComponent* CopyIcon = nullptr;
	/** The wood menus' shared parts (2026-10-26): the plain materials of the snow, the nails, the icicles, the ropes and the
	 *  focus arrow (one instance each), the title's hanging board (it swings on its ropes; its planks, the post the signs are
	 *  nailed to), the focus arrow, the snow puff's bits. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SnowMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> NailMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> IceMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RopeLineMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PointerMaterial;
	USceneComponent* TitleBoard = nullptr;
	PinguMenu3D::FSlab TitlePlanks[3];
	PinguMenu3D::FSlab TitlePost;
	UStaticMeshComponent* FocusArrow = nullptr;
	float FocusArrowIn = 0.f;
	struct FPuffBit
	{
		UStaticMeshComponent* Mesh = nullptr;
		FVector Velocity = FVector::ZeroVector;
		float Age = 10.f;
		float Size = 6.f;
	};
	TArray<FPuffBit> Puff;

	EPinguMenu3DPage Page = EPinguMenu3DPage::None;
	EPinguMenu3DPage TestPage = EPinguMenu3DPage::None;
	TFunction<void(FName)> TestAction;
	bool bBuilt = false;
	/** The pointer is over the game's picture (0..1) and where. */
	bool bPointerIn = false;
	FVector2D PointerUV = FVector2D::ZeroVector;
	/** The OS cursor's viewport pixel at the last poll (PollHover; -1: none yet) and the eased parallax (-1..1 of the picture). */
	FVector2D LastCursorPixel = FVector2D(-1.f, -1.f);
	FVector2D Parallax = FVector2D::ZeroVector;
	/** A press waiting for its push-in before the action (INDEX_NONE: none) and the time left. */
	int32 PendingPress = INDEX_NONE;
	float PendingLeft = 0.f;
	int32 ActionsFired = 0;
	float Clock = 0.f;
	/** The lobby page is the wooden card panel (not the island's strip). */
	bool bCardLobby = false;
	/** Where the lobby's address line starts (its left end, page units): it grows to the right with its text. */
	float AddressLeft = -450.f;
	/** The card lobby's hot button when Esc hid the panel (it comes back there). */
	FName LobbyLastHot;
	/** The card lobby's board as it grows and shrinks with the cards (2026-10-29, the declutter): its width and the cards' height
	 *  now and where they are going, its CLIENT labels (TRAITS state, "Waiting for ..."). */
	float CardLobbyW = 0.f;
	float CardLobbyGridH = 0.f;
	float CardLobbyTargetW = 0.f;
	float CardLobbyTargetGridH = 0.f;
	int32 CardTraitsLabel = INDEX_NONE;
	int32 CardWaitingLabel = INDEX_NONE;
	/** Places the card lobby's board and its signs for this frame's (eased) size. */
	void LayoutCardLobby(FPageParts& Parts, float Delta);
	/** The card lobby's ability picks (2026-11-07, card ability-picks; PinguMenu3DPicks.cpp): under the cards, the host's
	 *  ABILITIES RANDOM / CHOOSE sign and - CHOOSE - your three slots; a slot clicked opens the icon grid (RANDOM + every
	 *  ability; one another slot holds is dimmed and can't be taken). Its height (0: nothing shows), its part of the rows key,
	 *  and its rows from page height Top down. */
	float GetPicksHeight() const;
	FString GetPicksKey() const;
	void AddPicksRows(FPageParts& Parts, float Top);
	/** The slot whose icon grid is open (-1: none). */
	int32 PickerSlot = -1;
	/** The lobby's layout key (which buttons show): the strip is laid out again when it changes. */
	int32 LobbyLayoutKey = -1;
	/** The view this frame (set in PlaceOnCamera). */
	FVector ViewOrigin = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	float ViewTanX = 0.3f;
	float ViewTanY = 0.17f;
	bool bViewValid = false;
};

/**
 * The invisible full-screen Slate layer of a 3D screen: it takes the keyboard focus (so the HUD's focus rules work as on any
 * screen), passes the pointer, clicks, Enter / gamepad A and the navigation keys to the 3D menu, and leaves everything else
 * (Esc, B, the pause keys) to the root like a Slate button would. On the title a click next to the buttons is not handled (the
 * screen under it keeps the focus on the menu).
 *
 * bLobbyIsland: the lobby island's layer. It is always up and takes every click: a strip button first, else APinguLobby (a penguin,
 * a tag, an x, the "+"; the right button goes back), and the island gets the keyboard focus back (the keys stay on the game; it
 * takes the keyboard focus itself only while the strip has the keys: APenguinHUD::IsLobbyStripFocused). A click on the island
 * while the strip has the keys gives them back and still counts.
 */
class SPinguMenu3DInput : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SPinguMenu3DInput) : _bLobbyIsland(false) {}
		SLATE_ARGUMENT(TWeakObjectPtr<APenguinHUD>, HUD)
		SLATE_ARGUMENT(bool, bLobbyIsland)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FNavigationReply OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent) override;

	/** Clicks this layer handled since it was made (tests). */
	int32 GetClicksTaken() const { return ClicksTaken; }

protected:
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	APinguMenu3D* GetMenu() const;
	/** The event's place as a viewport pixel (the viewport widget's geometry; this layer's own as a stand-in). */
	FVector2D ToPixel(const FGeometry& MyGeometry, const FVector2D& ScreenPosition) const;

	TWeakObjectPtr<APenguinHUD> HUD;
	bool bLobbyIsland = false;
	int32 ClicksTaken = 0;
};

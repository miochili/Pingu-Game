// Pingu - the 3D menus (see PinguMenu3D.h): the toolkit (3D text, rounded slabs), APinguMenu3D (the title and the lobby's
// controls in 3D in front of the camera) and SPinguMenu3DInput (the Slate layer that hands it the mouse and the keys).

#include "PinguMenu3D.h"
#include "PinguMenu2D.h"
#include "Penguin.h"
#include "PenguinCharacter.h"
#include "PenguinGameState.h"
#include "PenguinHUD.h"
#include "PinguLobby.h"
#include "PinguUI.h"
#include "PinguSettings.h"
#include "PinguWoodStyle.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Texture.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Text.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Layout/Geometry.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Extensions/Text3DDefaultGeometryExtension.h"
#include "Extensions/Text3DDefaultMaterialExtension.h"
#include "Fonts/CompositeFont.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Text3DComponent.h"
#include "UObject/UObjectGlobals.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"

#define LOCTEXT_NAMESPACE "PinguMenu3D"

namespace PinguMenu3DPrivate
{
	static const TCHAR* PiecePaths[7] = {
		TEXT("/Game/UI/Menu3D/SM_UI3D_Corner.SM_UI3D_Corner"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_EdgeTop.SM_UI3D_EdgeTop"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_EdgeSide.SM_UI3D_EdgeSide"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_Center.SM_UI3D_Center"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_RingCorner.SM_UI3D_RingCorner"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_RingEdgeTop.SM_UI3D_RingEdgeTop"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_RingEdgeSide.SM_UI3D_RingEdgeSide"),
	};
	constexpr int32 SlabPieces = 4;
	constexpr int32 RingPieceFirst = 4;
	/** The wooden signs' pieces (2026-10-26), in the order of PinguMenu3D::EPiece. Optional: without them the signs are plain
	 *  planks (the 3D menus stay on). */
	static const TCHAR* SignPiecePaths[5] = {
		TEXT("/Game/UI/Menu3D/SM_UI3D_ArrowTip.SM_UI3D_ArrowTip"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_Pointer.SM_UI3D_Pointer"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_SnowCap.SM_UI3D_SnowCap"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_Nail.SM_UI3D_Nail"),
		TEXT("/Game/UI/Menu3D/SM_UI3D_Icicle.SM_UI3D_Icicle"),
	};
	static const TCHAR* WoodMaterialPath = TEXT("/Game/UI/Menu3D/M_Menu3D_Wood.M_Menu3D_Wood");
	static const TCHAR* RopeMaterialPath = TEXT("/Game/UI/Menu3D/M_Menu3D_Rope.M_Menu3D_Rope");
	static const TCHAR* CylinderPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	static const TCHAR* SpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	static const TCHAR* MaterialPath = TEXT("/Game/UI/Menu3D/M_Menu3D.M_Menu3D");
	static const TCHAR* ShadowMaterialPath = TEXT("/Game/UI/Menu3D/M_Menu3D_Shadow.M_Menu3D_Shadow");
	/** The icons' material (a texture on the engine's plane: portraits, ability icons, trait badges) and that plane. */
	static const TCHAR* IconMaterialPath = TEXT("/Game/UI/Menu3D/M_Menu3D_Icon.M_Menu3D_Icon");
	static const TCHAR* IconMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");
	/** The ring pieces' tube radius as a share of their centre line's radius (make_ui_meshes.py TUBE). */
	constexpr float RingTube = 0.17f;
	static const TCHAR* FontAssetPath = TEXT("/Game/UI/Fonts/TitanOne.TitanOne");
	/** The pieces are modelled with a corner radius (and a depth) of 100 units. */
	constexpr float PieceUnit = 100.f;
	/** Text3D's font size the letters are built at; their capitals' height at that size is measured once (CapAt100). */
	constexpr float BuildFontSize = 100.f;

	static UStaticMesh* GetPiece(int32 Index)
	{
		return LoadObject<UStaticMesh>(nullptr, PiecePaths[Index], nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** What a 3D text was made with (its look, the text, its scale to fit, its two materials). */
	struct FTextState
	{
		PinguMenu3D::FTextLook Look;
		FString Text;
		float Scale = 1.f;
		float BoxWidth = 0.f;
		/** The glyphs' box along the line (unscaled, the text's own Y) and the letters' colour: the Slate overlay draws the menu's
		 *  texts from these (2026-10-26, third pass), the 3D glyphs stay hidden (bOverlay). */
		float BoxMinY = 0.f;
		float BoxMaxY = 0.f;
		FLinearColor Front = FLinearColor::White;
		FLinearColor Side = FLinearColor::Black;
		bool bOverlay = false;
		/** A 2D menu text (MakeMenuText with the 2D menus): no glyphs, its box measured with the Slate font. */
		bool bFlat = false;
		TWeakObjectPtr<UMaterialInstanceDynamic> FrontMaterial;
		TWeakObjectPtr<UMaterialInstanceDynamic> SideMaterial;
	};
	static TMap<TWeakObjectPtr<USceneComponent>, FTextState>& TextStates()
	{
		static TMap<TWeakObjectPtr<USceneComponent>, FTextState> States;
		return States;
	}
	/** FForce3DScope's depth: while > 0 nothing is built flat. */
	static int32 GForce3D = 0;
	/** Build this part as a 2D part (the 2D menus, no FForce3DScope). */
	static bool BuildFlat()
	{
		return PinguMenu3D::IsFlat() && GForce3D == 0;
	}
	/** Texts made inside an FForce3DScope keep their 3D letters with the 2D menus. */
	static TSet<TWeakObjectPtr<USceneComponent>>& Forced3DTexts()
	{
		static TSet<TWeakObjectPtr<USceneComponent>> Texts;
		return Texts;
	}
	/** The menu's words are drawn by the Slate overlay (true) or as the 3D letters (false: the tests' before / after pictures). */
	static bool GTextOverlay = true;
	/** The capitals' height of a text built at BuildFontSize (measured on "H"; 0: not yet). */
	static float CapAt100 = 0.f;
	static float BaselineAt100 = 0.f;

	/** The glyphs' box in the text component's own space (unscaled); invalid before they are built. */
	static FBox GetLocalBox(const UText3DComponent* Text)
	{
		FBox Box(ForceInit);
		if (!Text)
		{
			return Box;
		}
		TArray<USceneComponent*> Children;
		Text->GetChildrenComponents(true, Children);
		const FTransform TextTransform = Text->GetComponentTransform();
		for (const USceneComponent* Child : Children)
		{
			const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Child);
			if (!Mesh || !Mesh->GetStaticMesh())
			{
				continue;
			}
			const FTransform Relative = Mesh->GetComponentTransform().GetRelativeTransform(TextTransform);
			Box += Mesh->CalcBounds(Relative).GetBox();
		}
		return Box;
	}

	static void PrepareChildren(UText3DComponent* Text, bool bMenuLight, bool bHide)
	{
		if (!Text)
		{
			return;
		}
		TArray<USceneComponent*> Children;
		Text->GetChildrenComponents(true, Children);
		for (USceneComponent* Child : Children)
		{
			if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Child))
			{
				PinguMenu3D::PreparePrimitive(Primitive, bMenuLight);
				// (2026-10-26, third pass - Linus: "make the text way better readable ... a overlay gui so it renders on all
				// graphics settigs clean": the menu's letters and their dark edges are drawn by the Slate overlay at the
				// screen's own resolution; the 3D glyphs only keep the layout - their size, width and place.)
				// (With the 2D menus no 3D letters show at all: the overlay draws the words, the painter the lobby's "+".)
				Primitive->SetHiddenInGame((bHide && GTextOverlay) || (PinguMenu3D::IsFlat() && !Forced3DTexts().Contains(Text)));
			}
		}
	}

	static EText3DHorizontalTextAlignment ToText3D(PinguMenu3D::ETextAlign Align)
	{
		switch (Align)
		{
		case PinguMenu3D::ETextAlign::Left: return EText3DHorizontalTextAlignment::Left;
		case PinguMenu3D::ETextAlign::Right: return EText3DHorizontalTextAlignment::Right;
		default: return EText3DHorizontalTextAlignment::Center;
		}
	}

	/** Builds the text now (not on Text3D's next tick) and puts its glyphs on the menu's lighting. */
	static void BuildNow(UText3DComponent* Text, bool bMenuLight, bool bHide)
	{
		Text->RequestUpdate(EText3DRendererFlags::All, true);
		PrepareChildren(Text, bMenuLight, bHide);
	}

	/** The scale that gives the capitals their size (and keeps the text inside its width). */
	/** A 2D text's box (MakeMenuText): Text3D's glyph box at BuildFontSize, measured with the same font in Slate (Text3D builds
	 *  its letters at Slate's size scale: 100 = an em of 133; the box leaves out the first and last letters' side bearings). */
	constexpr float FlatCapAt100 = 94.2f;
	constexpr float FlatBearing = 9.f;
	static void MeasureFlat(FTextState& State)
	{
		float Width = 0.f;
		if (!State.Text.IsEmpty() && FSlateApplication::IsInitialized())
		{
			FSlateFontInfo Font = FPinguStyle::Title(BuildFontSize);
			Font.Size = BuildFontSize;
			Font.LetterSpacing = 0;
			Font.OutlineSettings.OutlineSize = 0;
			const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
			Width = FMath::Max(static_cast<float>(Measure->Measure(State.Text, Font).X) - FlatBearing, 1.f);
		}
		State.BoxWidth = Width;
		switch (State.Look.Align)
		{
		case PinguMenu3D::ETextAlign::Left: State.BoxMinY = 0.5f * FlatBearing; break;
		case PinguMenu3D::ETextAlign::Right: State.BoxMinY = -0.5f * FlatBearing - Width; break;
		default: State.BoxMinY = -0.5f * Width; break;
		}
		State.BoxMaxY = State.BoxMinY + Width;
	}

	static void FitScale(USceneComponent* Text, FTextState& State)
	{
		if (State.bFlat)
		{
			MeasureFlat(State);
		}
		else
		{
			const FBox Box = GetLocalBox(Cast<UText3DComponent>(Text));
			State.BoxWidth = Box.IsValid ? static_cast<float>(Box.Max.Y - Box.Min.Y) : 0.f;
			State.BoxMinY = Box.IsValid ? static_cast<float>(Box.Min.Y) : 0.f;
			State.BoxMaxY = Box.IsValid ? static_cast<float>(Box.Max.Y) : 0.f;
		}
		float Scale = State.Look.Size / FMath::Max(CapAt100, 1.f);
		if (State.Look.MaxWidth > 0.f && State.BoxWidth * Scale > State.Look.MaxWidth && State.BoxWidth > 1.f)
		{
			Scale = State.Look.MaxWidth / State.BoxWidth;
		}
		State.Scale = Scale;
		Text->SetRelativeScale3D(FVector(Scale));
	}

	/** Measures the capitals once: a hidden "H" built at BuildFontSize. */
	static void MeasureCaps(AActor* Owner)
	{
		if (CapAt100 > 0.f || !Owner || !Owner->GetRootComponent())
		{
			return;
		}
		UText3DComponent* Probe = NewObject<UText3DComponent>(Owner, NAME_None, RF_Transient);
		Probe->SetupAttachment(Owner->GetRootComponent());
		Probe->SetUseBlockingBuild(true);
		Probe->RegisterComponent();
		Probe->SetFont(PinguMenu3D::GetFont());
		Probe->SetFontSize(BuildFontSize);
		Probe->SetVerticalAlignment(EText3DVerticalTextAlignment::FirstLine);
		Probe->SetHorizontalAlignment(EText3DHorizontalTextAlignment::Left);
		Probe->SetExtrude(1.f);
		Probe->SetText(FText::FromString(TEXT("H")));
		Probe->RequestUpdate(EText3DRendererFlags::All, true);
		const FBox Box = GetLocalBox(Probe);
		if (Box.IsValid && Box.Max.Z - Box.Min.Z > 1.0)
		{
			CapAt100 = static_cast<float>(Box.Max.Z - Box.Min.Z);
			BaselineAt100 = static_cast<float>(Box.Min.Z);
		}
		else
		{
			CapAt100 = 71.f; // about what Titan One's capitals are at 100
		}
		UE_LOG(LogPingu, Log, TEXT("[MENU3D] capitals at Text3D size %.0f: %.1f high, baseline at %.1f (glyph box %s)"),
			BuildFontSize, CapAt100, BaselineAt100, Box.IsValid ? *Box.ToString() : TEXT("none"));
		// Text3D's glyph parts belong to the actor: they go with the probe.
		TArray<USceneComponent*> ProbeParts;
		Probe->GetChildrenComponents(true, ProbeParts);
		Probe->DestroyComponent();
		for (USceneComponent* Part : ProbeParts)
		{
			if (IsValid(Part))
			{
				Part->DestroyComponent();
			}
		}
	}

	/** Eases 0 -> 1 (cubic out). */
	static float EaseOut(float T)
	{
		const float X = 1.f - FMath::Clamp(T, 0.f, 1.f);
		return 1.f - X * X * X;
	}

	/** Moves Value toward Target at Speed per second. */
	static float Approach(float Value, float Target, float Speed, float Delta)
	{
		return Value < Target ? FMath::Min(Target, Value + Speed * Delta) : FMath::Max(Target, Value - Speed * Delta);
	}

	// ---- the menu's look (design pixels of a 1920 wide picture; colours from FPinguStyle)
	/** The title card: its place from the left edge, its size, its corner radius and depth. */
	constexpr float TitleMargin = 56.f;
	constexpr float TitleWidth = 480.f;
	constexpr float TitleHeight = 642.f;
	/** The title card turns a little toward the middle of the picture (degrees about its vertical). */
	constexpr float TitleYaw = -16.f;
	using PinguMenu3D::PlaneRadius;
	using PinguMenu3D::PlaneDepth;
	using PinguMenu3D::LabelDepth;
	/** The lobby's strip: the buttons' row (828 wide as the old card's until 2026-10-26; 900 since the third pass: more room between
	 *  the signs), its padding, its bottom margin, its tilt back, its size (1.45: Linus "a bit bigger higher and a tad bit more spaced out"). */
	constexpr float StripInner = 900.f;

	constexpr float StripPad = 28.f;
	constexpr float StripBottom = 72.f;
	constexpr float StripPitch = -13.f;
	constexpr float LobbyScale = 1.45f;
	/** A button: how far it stands out of the plane (2026-10-24: a good deal more, with a shadow on the plane - "layer the items
	 *  in front of the back plane"), how much further when hot, how deep a press pushes it; its depth. */
	constexpr float ButtonProtrude = 24.f;
	constexpr float GhostProtrude = 14.f;
	/** Rows, key caps, list rows and sliders stand out less (many on a page: a quieter relief); a text field is a shallow well in
	 *  front of the plane; an arrow button a little knob. */
	constexpr float RowProtrude = 10.f;
	constexpr float FieldProtrude = 5.f;
	/** (2026-10-26: the arrows stand in front of their row - Linus: the focused row keeps its arrows visible; the row no longer
	 *  pops when focused, the rope shows it - and sit ArrowInset further in, so the row's own edge stays the row's to click.) */
	constexpr float ArrowProtrude = 15.f;
	constexpr float ArrowInset = 16.f;
	/** The other pages (pause, LAN, ...) stand in the middle of the picture, turned a little less by the cursor than the title. */
	constexpr float PageParallaxShare = 0.5f;
	/** An overlay (PRESS A KEY, the error popup) floats this much nearer the camera than the page under it. */
	constexpr float OverlayLift = 70.f;
	/** The caret of a text field blinks with this period (s). */
	constexpr float CaretPeriod = 1.f;
	constexpr float HotPop = 12.f;
	constexpr float HotGrow = 0.03f;
	constexpr float PushDepth = 16.f;
	constexpr float PushTime = 0.16f;
	/** The press's action waits for the push-in (s). */
	constexpr float ActionDelay = 0.09f;
	/** The hot ring: the gap between the button's edge and the ring, its glow (little: it is lit, a 3D tube, not a flat glow), how
	 *  fast it comes and goes, how far behind the face its tube's centre sits. */
	constexpr float RingGap = 5.f;
	constexpr float RingGlow = 0.55f;
	constexpr float HotSpeed = 9.f;
	constexpr float RingBack = 5.f;
	/** The buttons' soft shadow on the plane: how dark, how far its soft edge reaches past the button, its offset (down and to
	 *  the right: the key light comes from up left). */
	constexpr float ShadowOpacity = 0.68f;
	constexpr float ShadowSpread = 12.f;
	constexpr float ShadowOffsetY = 5.f;
	constexpr float ShadowOffsetZ = -7.f;
	/** The mouse hits a button on its face plus this much round it (design pixels). */
	constexpr float HitMargin = 7.f;
	/** The parallax: the title card turns toward the cursor by up to this much (degrees), the lobby strip tips. */
	constexpr float ParallaxYaw = 4.f;
	constexpr float ParallaxPitch = 3.f;
	/** Button labels: their ink outline's width and depth (shares of the capitals' height; the depth: PinguMenu3D::LabelDepth). */
	constexpr float LabelInkWidth = 0.13f;
	constexpr float LabelInkDepth = 0.12f;
	/** The title card sways a little (degrees, its period in seconds): it is a thing in the world, not a picture. */
	constexpr float SwayYaw = 0.9f;
	constexpr float SwayPitch = 0.6f;
	constexpr float SwayPeriod = 7.f;
	/** A page comes in over this long (s). */
	constexpr float AppearTime = 0.28f;

	// ---- the wood menus (2026-10-26, Linus's mockup: "have this style for the buttons ... a contiiuos snow slab and not points
	// ... add some small animations"). Colours are linear; the grain texture shades them (M_Menu3D_Wood).
	/** Natural wood (the plain signs), a light birch (PLAY, START, BACK: the primary signs), dark weathered wood (QUIT, the
	 *  address line, rows), a dark stain (wells, fields, the rows of lists: light text reads on it), the boards' planks and frame. */
	// (Style 5 "Snowy cabin", 2026-11-07, docs/UI_STYLE.md: the 3D menus' wood is the cabin mid #98572B (PinguWood::Mid3D), its
	// light #B9743F and dark #5E2F14; the 2D menus draw the cabin atlas and ignore these.)
	static const FLinearColor WoodFace(0.31f, 0.095f, 0.024f);
	static const FLinearColor WoodLip(0.11f, 0.028f, 0.007f);
	static const FLinearColor WoodLight(0.40f, 0.135f, 0.036f);
	static const FLinearColor WoodLightLip(0.14f, 0.04f, 0.01f);
	static const FLinearColor WoodDark(0.16f, 0.05f, 0.013f);
	static const FLinearColor WoodDarkLip(0.06f, 0.016f, 0.004f);
	static const FLinearColor WoodStain(0.112f, 0.028f, 0.007f);
	static const FLinearColor WoodStainLip(0.045f, 0.011f, 0.003f);
	static const FLinearColor BoardFace(0.28f, 0.086f, 0.022f);
	static const FLinearColor BoardLip(0.10f, 0.026f, 0.006f);
	static const FLinearColor FrameFace(0.1f, 0.052f, 0.022f);
	static const FLinearColor FrameLip(0.04f, 0.02f, 0.008f);
	/** The letters' dark edge and sides on wood (a dark brown, not the navy ink). */
	static const FLinearColor WoodInk(0.0232f, 0.0070f, 0.0018f);
	/** The pale blue texts of the navy menus read as cream on wood. */
	static const FLinearColor Cream(0.95f, 0.84f, 0.6f);
	static const FLinearColor CreamDim(0.66f, 0.5f, 0.3f);
	static const FLinearColor SnowWhite(0.92f, 0.95f, 1.f);
	static const FLinearColor SnowShade(0.42f, 0.55f, 0.78f);
	/** Painted signs keep a little of their grain. */
	constexpr float PaintGrain = 0.45f;
	/** A plank sign: its snow slab's height (share of the sign's height, at most), the nails' size and inset from the ends. */
	constexpr float SnowShare = 0.24f;
	constexpr float SnowMax = 15.f;
	constexpr float NailSize = 4.2f;
	constexpr float NailInset = 15.f;
	/** The boards: the frame's width, how far it stands out of the planks, the planks' width. */
	constexpr float FrameWidth = 22.f;
	constexpr float FrameProtrude = 6.f;
	constexpr float BoardPlank = 96.f;
	constexpr float BoardRadius = 8.f;
	/** The loose planks of a board: about this high, a calm grain (texts stand on them). */
	constexpr float BoardRow = 84.f;
	constexpr float BoardGrain = 0.55f;
	/** The smallest 3D text: a label, a tag, a button's word. */
	constexpr float MinLabelSize = 13.f;
	constexpr float MinButtonLabel = 14.f;
	/** Mouse hover: the sign tilts (degrees) and wiggles as the hover starts (a damped swing). */
	constexpr float HoverTilt = 3.2f;
	constexpr float WiggleAmp = 2.4f;
	constexpr float WiggleFreq = 21.f;
	constexpr float WiggleDamp = 6.5f;
	/** The signs sway on their post (degrees, period s); the title board swings on its ropes. */
	constexpr float SignSway = 0.7f;
	constexpr float SignSwayPeriod = 5.5f;
	constexpr float BoardSwing = 0.9f;
	constexpr float BoardSwingPeriod = 6.5f;
	/** A page opens: its signs drop in one after another (each over DropTime, DropStep apart, at most DropMaxDelay late). */
	constexpr float DropTime = 0.3f;
	constexpr float DropStep = 0.035f;
	constexpr float DropMaxDelay = 0.2f;
	constexpr float DropHeight = 70.f;
	constexpr float DropSwing = 7.f;
	/** The focus arrow: its size (half its height), its gap from the sign, how far it bobs. */
	constexpr float ArrowHalf = 15.f;
	constexpr float ArrowGap = 16.f;
	constexpr float ArrowBob = 6.f;
	/** The snow puff of a press. */
	constexpr int32 PuffBits = 9;
	constexpr float PuffTime = 0.45f;

	/** Text colours on wood (the navy menus' pale blue becomes cream). */
	static FLinearColor OnWood(const FLinearColor& Color)
	{
		const FPinguStyle& Style = FPinguStyle::Get();
		if (Color.Equals(Style.Ice, 0.001f))
		{
			return Cream;
		}
		if (Color.Equals(Style.IceDim, 0.001f))
		{
			return CreamDim;
		}
		if (Color.Equals(Style.Ink, 0.001f))
		{
			return WoodInk;
		}
		return Color;
	}

	/** A small stable number 0..1 from an index (a sign's own grain offset, tilt, sway phase). */
	static float Hash01(int32 Index)
	{
		const float X = FMath::Sin(static_cast<float>(Index) * 12.9898f + 78.233f) * 43758.5453f;
		return X - FMath::FloorToFloat(X);
	}

	/** Eases 0 -> 1 with a little overshoot (a sign dropping onto its nail). */
	static float EaseOutBack(float T)
	{
		const float X = FMath::Clamp(T, 0.f, 1.f) - 1.f;
		constexpr float S = 1.4f;
		return 1.f + X * X * ((S + 1.f) * X + S);
	}
}

// ============================================================================ toolkit

bool PinguMenu3D::IsEnabled()
{
	using namespace PinguMenu3DPrivate;
	static int32 Cached = -1;
	if (Cached >= 0)
	{
		return Cached == 1;
	}
	// One switch for the 3D menus and the lobby island (Penguin.cpp: -PinguOldMenus or [PinguMenus] bOldMenus in the game config).
	const bool bOld = PinguUseOldMenus();
	bool bAssets = GetMaterial() != nullptr;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(PiecePaths) && bAssets; ++Index)
	{
		bAssets = GetPiece(Index) != nullptr;
	}
	Cached = !bOld && bAssets ? 1 : 0;
	UE_LOG(LogPingu, Display, TEXT("[MENU3D] 3D title and lobby %s (old menus asked: %d, assets there: %d)"),
		Cached ? TEXT("ON") : TEXT("off"), bOld ? 1 : 0, bAssets ? 1 : 0);
	return Cached == 1;
}

bool PinguMenu3D::UseIslandLobby()
{
	static int32 Cached = -1;
	if (Cached >= 0)
	{
		return Cached == 1;
	}
	if (!IsEnabled())
	{
		Cached = 0;
		return false;
	}
	bool bConfigIsland = false;
	if (GConfig)
	{
		GConfig->GetBool(TEXT("PinguMenus"), TEXT("bIslandLobby"), bConfigIsland, GGameIni);
	}
	const bool bEnvIsland = FPlatformMisc::GetEnvironmentVariable(TEXT("PINGU_ISLAND_LOBBY")) == TEXT("1");
	const bool bIsland = FParse::Param(FCommandLine::Get(), TEXT("PinguIslandLobby")) || bConfigIsland || bEnvIsland;
	Cached = bIsland ? 1 : 0;
	UE_LOG(LogPingu, Display, TEXT("[MENU3D] lobby: %s"), bIsland ? TEXT("the lobby island") : TEXT("the wooden card panel"));
	return Cached == 1;
}

PinguMenu3D::FForce3DScope::FForce3DScope()
{
	++PinguMenu3DPrivate::GForce3D;
}

PinguMenu3D::FForce3DScope::~FForce3DScope()
{
	--PinguMenu3DPrivate::GForce3D;
}

bool PinguMenu3D::IsFlat()
{
	static int32 Cached = -1;
	if (Cached >= 0)
	{
		return Cached == 1;
	}
	if (!IsEnabled())
	{
		Cached = 0;
		return false;
	}
	// (2026-10-27, card 2026-10-27_gui-2d - Linus: "make it 2d agin ... so its wayy less performance impact". The 3D menus stay
	// behind a switch until he decides: the command line, the game config or - for the tests' 3D runs - the environment.)
	bool bConfig3D = false;
	if (GConfig)
	{
		GConfig->GetBool(TEXT("PinguMenus"), TEXT("b3DMenus"), bConfig3D, GGameIni);
	}
	const bool bEnv3D = FPlatformMisc::GetEnvironmentVariable(TEXT("PINGU_3D_MENUS")) == TEXT("1");
	const bool b3D = FParse::Param(FCommandLine::Get(), TEXT("Pingu3DMenus")) || bConfig3D || bEnv3D;
	Cached = b3D ? 0 : 1;
	UE_LOG(LogPingu, Display, TEXT("[MENU3D] menus drawn %s (3D asked: switch %d, config %d, environment %d)"), b3D ? TEXT("in 3D") : TEXT("in 2D"),
		FParse::Param(FCommandLine::Get(), TEXT("Pingu3DMenus")) ? 1 : 0, bConfig3D ? 1 : 0, bEnv3D ? 1 : 0);
	return Cached == 1;
}

UFont* PinguMenu3D::GetFont()
{
	using namespace PinguMenu3DPrivate;
	static TWeakObjectPtr<UFont> Cached;
	if (UFont* Font = Cached.Get())
	{
		return Font;
	}
	UFont* Font = LoadObject<UFont>(nullptr, FontAssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Font)
	{
		// No asset (not imported yet): a runtime font on the TTF the Slate UI uses (kept for the whole run).
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("Slate/Fonts/TitanOne-Regular.ttf"));
		Font = NewObject<UFont>(GetTransientPackage(), TEXT("PinguTitanOneRuntime"), RF_Transient);
		Font->FontCacheType = EFontCacheType::Runtime;
		Font->GetMutableInternalCompositeFont().DefaultTypeface.Fonts.Add(
			FTypefaceEntry(TEXT("Regular"), File, EFontHinting::Default, EFontLoadingPolicy::LazyLoad));
		Font->AddToRoot();
		UE_LOG(LogPingu, Warning, TEXT("[MENU3D] %s is missing: 3D text uses %s"), FontAssetPath, *File);
	}
	Cached = Font;
	return Font;
}

UMaterialInterface* PinguMenu3D::GetMaterial()
{
	return LoadObject<UMaterialInterface>(nullptr, PinguMenu3DPrivate::MaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

UMaterialInterface* PinguMenu3D::GetWoodMaterial()
{
	UMaterialInterface* Wood = LoadObject<UMaterialInterface>(nullptr, PinguMenu3DPrivate::WoodMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	return Wood ? Wood : GetMaterial();
}

UMaterialInterface* PinguMenu3D::GetRopeMaterial()
{
	UMaterialInterface* Rope = LoadObject<UMaterialInterface>(nullptr, PinguMenu3DPrivate::RopeMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	return Rope ? Rope : GetMaterial();
}

UStaticMesh* PinguMenu3D::GetPieceMesh(EPiece Piece)
{
	const int32 Index = static_cast<int32>(Piece);
	return Index >= 0 && Index < UE_ARRAY_COUNT(PinguMenu3DPrivate::SignPiecePaths)
		? LoadObject<UStaticMesh>(nullptr, PinguMenu3DPrivate::SignPiecePaths[Index], nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
}

void PinguMenu3D::PreparePrimitive(UPrimitiveComponent* Primitive, bool bMenuLight)
{
	if (!Primitive)
	{
		return;
	}
	Primitive->SetCastShadow(false);
	Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Primitive->SetCollisionResponseToAllChannels(ECR_Ignore);
	Primitive->SetGenerateOverlapEvents(false);
	Primitive->SetCanEverAffectNavigation(false);
	Primitive->bReceivesDecals = false;
	if (bMenuLight)
	{
		// Near the camera and lit by its own light: out of the island's lighting, reflections and ray tracing. It writes custom
		// depth: the darkening behind a menu page (M_Menu3D_DimPP) spares what is of the menu.
		Primitive->SetRenderCustomDepth(true);
		Primitive->SetLightingChannels(false, false, true);
		Primitive->bAffectDynamicIndirectLighting = false;
		Primitive->bAffectDistanceFieldLighting = false;
		Primitive->bVisibleInRayTracing = false;
		Primitive->bVisibleInReflectionCaptures = false;
		Primitive->bVisibleInRealTimeSkyCaptures = false;
		Primitive->MarkRenderStateDirty();
	}
}

// ---- the mouse

namespace PinguMenu3DPrivate
{
	/** This world's game viewport (its FSceneViewport: it knows its widget's geometry in the window). */
	static FSceneViewport* GetSceneViewport(const UWorld* World)
	{
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		if (!Client && GEngine)
		{
			Client = GEngine->GameViewport;
		}
		return Client ? Client->GetGameViewport() : nullptr;
	}
}

FVector2D PinguMenu3D::PixelFromGeometry(const FGeometry& ViewportGeometry, const FVector2D& ScreenPosition)
{
	// As FSceneViewport::UpdateCachedCursorPos: the widget's local point times its accumulated scale (DPI) = render pixels.
	const FVector2D Local = ViewportGeometry.AbsoluteToLocal(ScreenPosition);
	return Local * ViewportGeometry.GetAccumulatedLayoutTransform().GetScale();
}

FVector2D PinguMenu3D::ScreenFromGeometry(const FGeometry& ViewportGeometry, const FVector2D& Pixel)
{
	const float Scale = FMath::Max(ViewportGeometry.GetAccumulatedLayoutTransform().GetScale(), 1.e-4f);
	return ViewportGeometry.LocalToAbsolute(Pixel / Scale);
}

bool PinguMenu3D::ScreenToViewportPixel(const UWorld* World, const FVector2D& ScreenPosition, FVector2D& OutPixel)
{
	const FSceneViewport* Viewport = PinguMenu3DPrivate::GetSceneViewport(World);
	if (!Viewport)
	{
		return false;
	}
	const FGeometry& Geometry = Viewport->GetCachedGeometry();
	if (Geometry.GetLocalSize().X <= 1.0 || Geometry.GetLocalSize().Y <= 1.0)
	{
		return false;
	}
	OutPixel = PixelFromGeometry(Geometry, ScreenPosition);
	return true;
}

bool PinguMenu3D::ViewportPixelToScreen(const UWorld* World, const FVector2D& Pixel, FVector2D& OutScreenPosition)
{
	const FSceneViewport* Viewport = PinguMenu3DPrivate::GetSceneViewport(World);
	if (!Viewport)
	{
		return false;
	}
	const FGeometry& Geometry = Viewport->GetCachedGeometry();
	if (Geometry.GetLocalSize().X <= 1.0 || Geometry.GetLocalSize().Y <= 1.0)
	{
		return false;
	}
	OutScreenPosition = ScreenFromGeometry(Geometry, Pixel);
	return true;
}

bool PinguMenu3D::GetCursorViewportPixel(const UWorld* World, FVector2D& OutPixel)
{
	if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().IsMouseAttached())
	{
		return false;
	}
	return ScreenToViewportPixel(World, FSlateApplication::Get().GetCursorPos(), OutPixel);
}

bool PinguMenu3D::GetViewportPixelSize(const APlayerController* Controller, FVector2D& OutSize)
{
	int32 SizeX = 0;
	int32 SizeY = 0;
	if (Controller)
	{
		Controller->GetViewportSize(SizeX, SizeY);
	}
	OutSize = FVector2D(SizeX, SizeY);
	return SizeX > 0 && SizeY > 0;
}

UText3DComponent* PinguMenu3D::MakeText(AActor* Owner, USceneComponent* Parent, const FText& Text, const FTextLook& Look)
{
	using namespace PinguMenu3DPrivate;
	if (!Owner || !Parent)
	{
		return nullptr;
	}
	MeasureCaps(Owner);
	UText3DComponent* Comp = NewObject<UText3DComponent>(Owner, NAME_None, RF_Transient);
	if (GForce3D > 0)
	{
		Forced3DTexts().Add(Comp);
	}
	Comp->SetupAttachment(Parent);
	Comp->SetUseBlockingBuild(true);
	Comp->RegisterComponent();
	Comp->SetFont(GetFont());
	Comp->SetFontSize(BuildFontSize);
	Comp->SetEnforceUpperCase(false);
	Comp->SetHorizontalAlignment(ToText3D(Look.Align));
	Comp->SetVerticalAlignment(EText3DVerticalTextAlignment::FirstLine);
	Comp->SetCastShadow(false);
	const float Cap = FMath::Max(CapAt100, 1.f);
	if (Look.Outline > 0.f)
	{
		// The ink outline behind the letters: the glyphs grown (Text3D's outline, filled), no bevel (it cannot have both).
		Comp->SetExtrude(FMath::Max(Look.Extrude, 0.01f) * Cap);
		Comp->SetHasOutline(true);
		Comp->SetOutlineExpand(Look.Outline * Cap);
		if (UText3DDefaultGeometryExtension* Geometry = Comp->GetCastedGeometryExtension<UText3DDefaultGeometryExtension>())
		{
			// SetOutlineType is not exported from Text3D: set the property and let SetHasOutline above rebuild with it.
			if (FProperty* Property = Geometry->GetClass()->FindPropertyByName(TEXT("OutlineType")))
			{
				if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
				{
					EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Geometry),
						static_cast<int64>(EText3DOutlineType::Fill));
				}
			}
		}
	}
	else
	{
		Comp->SetExtrude(FMath::Max(Look.Extrude, 0.01f) * Cap);
		Comp->SetBevelType(EText3DBevelType::Convex);
		Comp->SetBevelSegments(4);
		Comp->SetBevel(FMath::Max(Look.Bevel, 0.f) * Cap);
	}
	if (UText3DDefaultGeometryExtension* Geometry = Comp->GetCastedGeometryExtension<UText3DDefaultGeometryExtension>())
	{
		Geometry->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	FTextState State;
	State.Look = Look;
	State.bOverlay = Look.bOverlay && Look.Outline <= 0.f;
	State.Front = Look.Front;
	State.Text = Text.ToString();
	if (UMaterialInterface* Base = GetMaterial())
	{
		UMaterialInstanceDynamic* Front = UMaterialInstanceDynamic::Create(Base, Owner);
		UMaterialInstanceDynamic* Side = UMaterialInstanceDynamic::Create(Base, Owner);
		State.FrontMaterial = Front;
		State.SideMaterial = Side;
		if (UText3DDefaultMaterialExtension* Materials = Comp->GetCastedMaterialExtension<UText3DDefaultMaterialExtension>())
		{
			Materials->SetStyle(EText3DMaterialStyle::Custom);
			Materials->SetUseSingleMaterial(false);
			Materials->SetFrontMaterial(Front);
			Materials->SetBevelMaterial(Look.Outline > 0.f ? Side : Front);
			Materials->SetExtrudeMaterial(Side);
			Materials->SetBackMaterial(Side);
		}
	}
	Comp->SetText(Text);
	BuildNow(Comp, Look.bMenuLight, Look.bOverlay);
	FitScale(Comp, State);
	if (Look.Outline > 0.f)
	{
		UE_LOG(LogPingu, Log, TEXT("[MENU3D] outline text \"%s\": glyph box %s, scale %.3f"), *State.Text, *GetLocalBox(Comp).ToString(), State.Scale);
	}
	State.Side = Look.Side;
	TextStates().Add(Comp, State);
	SetTextColors(Comp, Look.Front, Look.Side, Look.Glow);
	// Text3D may build again later (a font or a setting it changes by itself): its new glyphs get the same treatment.
	TWeakObjectPtr<UText3DComponent> Weak(Comp);
	const bool bMenuLight = Look.bMenuLight;
	Comp->OnTextPostUpdate().AddLambda([Weak, bMenuLight](UText3DComponent*, EText3DRendererFlags)
	{
		// (Whether the overlay draws it can change after it was made: SetTextInOverlay.)
		const FTextState* Current = TextStates().Find(TWeakObjectPtr<USceneComponent>(Weak.Get()));
		PrepareChildren(Weak.Get(), bMenuLight, !Current || Current->Look.bOverlay);
	});
	return Comp;
}

USceneComponent* PinguMenu3D::MakeMenuText(AActor* Owner, USceneComponent* Parent, const FText& Text, const FTextLook& Look)
{
	using namespace PinguMenu3DPrivate;
	if (!IsFlat())
	{
		return MakeText(Owner, Parent, Text, Look);
	}
	if (!Owner || !Parent)
	{
		return nullptr;
	}
	if (CapAt100 <= 0.f)
	{
		// (Measured on Text3D's "H" in the 3D menus: 94.2 at size 100, standing on the baseline.)
		CapAt100 = FlatCapAt100;
		BaselineAt100 = 0.f;
	}
	USceneComponent* Comp = NewObject<USceneComponent>(Owner, NAME_None, RF_Transient);
	Comp->SetupAttachment(Parent);
	Comp->RegisterComponent();
	FTextState State;
	State.Look = Look;
	State.bOverlay = Look.bOverlay && Look.Outline <= 0.f;
	State.Front = Look.Front;
	State.Side = Look.Side;
	State.Text = Text.ToString();
	State.bFlat = true;
	FitScale(Comp, State);
	TextStates().Add(Comp, State);
	return Comp;
}

void PinguMenu3D::GetRaisedTexts(const AActor* Owner, TArray<FRaisedText>& Out)
{
	using namespace PinguMenu3DPrivate;
	Out.Reset();
	if (!Owner || !IsFlat())
	{
		return;
	}
	for (const TPair<TWeakObjectPtr<USceneComponent>, FTextState>& Pair : TextStates())
	{
		const USceneComponent* Text = Pair.Key.Get();
		const FTextState& State = Pair.Value;
		if (!Text || Text->GetOwner() != Owner || State.bOverlay || State.Look.Outline > 0.f || State.Text.IsEmpty() || State.BoxMaxY <= State.BoxMinY
			|| Forced3DTexts().Contains(Pair.Key))
		{
			continue;
		}
		bool bShown = true;
		for (const USceneComponent* Part = Text; Part && bShown; Part = Part->GetAttachParent())
		{
			bShown = Part->IsVisible();
		}
		if (!bShown)
		{
			continue;
		}
		FRaisedText& Item = Out.AddDefaulted_GetRef();
		Item.Component = Text;
		Item.String = State.Text;
		Item.Front = State.Front;
		Item.Side = State.Side;
		Item.MinY = State.BoxMinY;
		Item.MaxY = State.BoxMaxY;
		Item.Baseline = BaselineAt100;
		Item.Cap = FMath::Max(CapAt100, 1.f);
	}
}

void PinguMenu3D::SetText(USceneComponent* Text, const FText& NewText)
{
	using namespace PinguMenu3DPrivate;
	FTextState* State = Text ? TextStates().Find(Text) : nullptr;
	if (!State)
	{
		return;
	}
	const FString String = NewText.ToString();
	if (String == State->Text)
	{
		return;
	}
	State->Text = String;
	if (UText3DComponent* Text3D = Cast<UText3DComponent>(Text))
	{
		Text3D->SetText(NewText);
		BuildNow(Text3D, State->Look.bMenuLight, State->Look.bOverlay);
	}
	FitScale(Text, *State);
}

void PinguMenu3D::SetTextColors(USceneComponent* Text, const FLinearColor& Front, const FLinearColor& Side, float Glow)
{
	using namespace PinguMenu3DPrivate;
	FTextState* State = Text ? TextStates().Find(Text) : nullptr;
	if (!State)
	{
		return;
	}
	State->Front = Front;
	State->Side = Side;
	// Letters have no lip: Lip = Color (Text3D's glyph meshes carry a white vertex colour).
	if (UMaterialInstanceDynamic* FrontMaterial = State->FrontMaterial.Get())
	{
		FrontMaterial->SetVectorParameterValue(TEXT("Color"), Front);
		FrontMaterial->SetVectorParameterValue(TEXT("Lip"), Front);
		// Light letters glow a little (2026-10-26, second pass): they stay crisp and light whatever the lighting and the
		// exposure do to the wood around them. Dark letters (an edge, ink) do not.
		const float Light = Front.GetLuminance();
		FrontMaterial->SetScalarParameterValue(TEXT("Glow"), FMath::Max(Glow, Light > 0.3f && State->Look.Outline <= 0.f ? 0.22f : 0.f));
	}
	if (UMaterialInstanceDynamic* SideMaterial = State->SideMaterial.Get())
	{
		SideMaterial->SetVectorParameterValue(TEXT("Color"), Side);
		SideMaterial->SetVectorParameterValue(TEXT("Lip"), Side);
		SideMaterial->SetScalarParameterValue(TEXT("Glow"), 0.f);
		SideMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.6f);
	}
}

float PinguMenu3D::GetTextWidth(USceneComponent* Text)
{
	using namespace PinguMenu3DPrivate;
	const FTextState* State = Text ? TextStates().Find(Text) : nullptr;
	return State ? State->BoxWidth * State->Scale : 0.f;
}

// ---- slabs

bool PinguMenu3D::FSlab::Build(AActor* Owner, USceneComponent* Parent, bool bMenuLight, bool bShadow, bool bInWood)
{
	using namespace PinguMenu3DPrivate;
	UStaticMesh* Pieces[SlabPieces];
	for (int32 Index = 0; Index < SlabPieces; ++Index)
	{
		Pieces[Index] = GetPiece(Index);
		if (!Pieces[Index])
		{
			return false;
		}
	}
	bWood = bInWood && !bShadow;
	if (BuildFlat())
	{
		// The 2D menus: one transform node that remembers the slab (PinguMenu2D paints it); no pieces, no material.
		if (!Owner || !Parent)
		{
			return false;
		}
		UPinguMenu2DPart* Part = NewObject<UPinguMenu2DPart>(Owner, NAME_None, RF_Transient);
		Part->Kind = EPinguMenu2DKind::Slab;
		Part->bWood = bWood;
		Part->bShadow = bShadow;
		Part->bWorldLit = !bMenuLight;
		Part->SetupAttachment(Parent);
		Part->RegisterComponent();
		Root = Part;
		Material = nullptr;
		return true;
	}
	UMaterialInterface* Base = bShadow ? LoadObject<UMaterialInterface>(nullptr, ShadowMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet)
		: (bWood ? GetWoodMaterial() : GetMaterial());
	if (!Owner || !Parent || !Base)
	{
		return false;
	}
	Root = NewObject<USceneComponent>(Owner, NAME_None, RF_Transient);
	Root->SetupAttachment(Parent);
	Root->RegisterComponent();
	Material = UMaterialInstanceDynamic::Create(Base, Owner);
	// 0-3 corners (top right, top left, bottom right, bottom left), 4-5 top / bottom edge, 6-7 right / left edge, 8 the middle.
	static const int32 PieceOf[9] = { 0, 0, 0, 0, 1, 1, 2, 2, 3 };
	for (int32 Index = 0; Index < 9; ++Index)
	{
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
		Part->SetStaticMesh(Pieces[PieceOf[Index]]);
		Part->SetupAttachment(Root);
		Part->SetMobility(EComponentMobility::Movable);
		PreparePrimitive(Part, bMenuLight);
		Part->RegisterComponent();
		Part->SetMaterial(0, Material);
		Parts[Index] = Part;
	}
	return true;
}

void PinguMenu3D::FSlab::SetShape(float InWidth, float InHeight, float InRadius, float InDepth)
{
	using namespace PinguMenu3DPrivate;
	if (!Root)
	{
		return;
	}
	Width = FMath::Max(InWidth, 1.f);
	Height = FMath::Max(InHeight, 1.f);
	Radius = FMath::Clamp(InRadius, 0.5f, 0.5f * FMath::Min(Width, Height));
	Depth = FMath::Max(InDepth, 0.5f);
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->Width = Width;
		Part->Height = Height;
		Part->Radius = Radius;
		Part->Depth = Depth;
		return;
	}
	const float A = 0.5f * Width - Radius;   // the straight part's half width
	const float C = 0.5f * Height - Radius;  // and half height
	const float S = Radius / PieceUnit;
	const float DX = Depth / PieceUnit;
	const float LenY = 2.f * A / PieceUnit;
	const float LenZ = 2.f * C / PieceUnit;
	// (Each part also carries its place in the slab - SlabMap, custom primitive data 0..3: offset Y, Z and scale Y, Z - so a
	// material can map the whole slab's face: the wood grain runs on across the joins.)
	auto Place = [](UStaticMeshComponent* Part, const FVector& Location, const FVector& Scale, bool bUsed)
	{
		if (!Part)
		{
			return;
		}
		Part->SetRelativeLocation(Location);
		Part->SetRelativeScale3D(bUsed ? Scale : FVector(1.f));
		Part->SetCustomPrimitiveDataVector4(0, FVector4(Location.Y, Location.Z, Scale.Y, Scale.Z));
		Part->ComponentTags.Remove(TEXT("PinguUnused"));
		if (!bUsed)
		{
			Part->ComponentTags.Add(TEXT("PinguUnused"));
		}
	};
	const bool bWide = A > 0.01f;
	const bool bTall = C > 0.01f;
	Place(Parts[0], FVector(0.f, A, C), FVector(DX, S, S), true);
	Place(Parts[1], FVector(0.f, -A, C), FVector(DX, -S, S), true);
	Place(Parts[2], FVector(0.f, A, -C), FVector(DX, S, -S), true);
	Place(Parts[3], FVector(0.f, -A, -C), FVector(DX, -S, -S), true);
	Place(Parts[4], FVector(0.f, -A, C), FVector(DX, LenY, S), bWide);
	Place(Parts[5], FVector(0.f, -A, -C), FVector(DX, LenY, -S), bWide);
	Place(Parts[6], FVector(0.f, A, -C), FVector(DX, S, LenZ), bTall);
	Place(Parts[7], FVector(0.f, -A, -C), FVector(DX, -S, LenZ), bTall);
	Place(Parts[8], FVector(0.f, -A, -C), FVector(1.f, LenY, LenZ), bWide && bTall);
	SetVisible(Root->IsVisible());
}

void PinguMenu3D::FSlab::SetColors(const FLinearColor& Face, const FLinearColor& Lip, float Glow)
{
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->Face = Face;
		Part->Lip = Lip;
		Part->Glow = Glow;
	}
	if (Material)
	{
		Material->SetVectorParameterValue(TEXT("Color"), Face);
		Material->SetVectorParameterValue(TEXT("Lip"), Lip);
		Material->SetScalarParameterValue(TEXT("Glow"), Glow);
	}
}

void PinguMenu3D::FSlab::SetWood(float Grain, const FVector2D& GrainOffset, bool bVertical, float PlankWidth, float PlankDark, float PlankTone,
	const FVector2D& GrainScale)
{
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->Grain = Grain;
		Part->GrainOffset = GrainOffset;
		Part->GrainScale = GrainScale;
		Part->bVertical = bVertical;
		return;
	}
	if (!Material || !bWood)
	{
		return;
	}
	Material->SetScalarParameterValue(TEXT("Grain"), Grain);
	Material->SetVectorParameterValue(TEXT("GrainOffset"), FLinearColor(GrainOffset.X, GrainOffset.Y, 0.f, 0.f));
	Material->SetVectorParameterValue(TEXT("GrainScale"), FLinearColor(GrainScale.X, GrainScale.Y, 0.f, 0.f));
	Material->SetScalarParameterValue(TEXT("Vertical"), bVertical ? 1.f : 0.f);
	Material->SetScalarParameterValue(TEXT("PlankWidth"), FMath::Max(PlankWidth, 1.f));
	Material->SetScalarParameterValue(TEXT("PlankDark"), PlankDark);
	Material->SetScalarParameterValue(TEXT("PlankTone"), PlankTone);
}

void PinguMenu3D::FSlab::SetOpacity(float Opacity)
{
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->Opacity = Opacity;
	}
	if (Material)
	{
		Material->SetScalarParameterValue(TEXT("Opacity"), Opacity);
	}
}

void PinguMenu3D::FSlab::SetWoodImage(uint8 Cut, uint8 State, uint8 Variant)
{
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->WoodCut = Cut;
		Part->WoodState = State;
		Part->WoodVariant = Variant;
	}
}

void PinguMenu3D::FSlab::SetGrain(float Grain)
{
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->Grain = Grain;
	}
	if (Material && bWood)
	{
		Material->SetScalarParameterValue(TEXT("Grain"), Grain);
	}
}

void PinguMenu3D::FSlab::SetVisible(bool bVisible)
{
	if (!Root)
	{
		return;
	}
	Root->SetVisibility(bVisible, false);
	for (UStaticMeshComponent* Part : Parts)
	{
		if (Part)
		{
			Part->SetVisibility(bVisible && !Part->ComponentHasTag(TEXT("PinguUnused")), false);
		}
	}
}

// ---- rings

bool PinguMenu3D::FRing::Build(AActor* Owner, USceneComponent* Parent, bool bMenuLight, bool bRope)
{
	using namespace PinguMenu3DPrivate;
	UStaticMesh* Pieces[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Pieces[Index] = GetPiece(RingPieceFirst + Index);
		if (!Pieces[Index])
		{
			return false;
		}
	}
	if (BuildFlat())
	{
		if (!Owner || !Parent)
		{
			return false;
		}
		UPinguMenu2DPart* Part = NewObject<UPinguMenu2DPart>(Owner, NAME_None, RF_Transient);
		Part->Kind = EPinguMenu2DKind::Ring;
		Part->bRope = bRope;
		Part->bWorldLit = !bMenuLight;
		Part->SetupAttachment(Parent);
		Part->RegisterComponent();
		Root = Part;
		Material = nullptr;
		return true;
	}
	UMaterialInterface* Base = bRope ? GetRopeMaterial() : GetMaterial();
	if (!Owner || !Parent || !Base)
	{
		return false;
	}
	Root = NewObject<USceneComponent>(Owner, NAME_None, RF_Transient);
	Root->SetupAttachment(Parent);
	Root->RegisterComponent();
	Material = UMaterialInstanceDynamic::Create(Base, Owner);
	// 0-3 corners (top right, top left, bottom right, bottom left), 4-5 top / bottom edge, 6-7 right / left edge.
	static const int32 PieceOf[8] = { 0, 0, 0, 0, 1, 1, 2, 2 };
	for (int32 Index = 0; Index < 8; ++Index)
	{
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
		Part->SetStaticMesh(Pieces[PieceOf[Index]]);
		Part->SetupAttachment(Root);
		Part->SetMobility(EComponentMobility::Movable);
		PreparePrimitive(Part, bMenuLight);
		Part->RegisterComponent();
		Part->SetMaterial(0, Material);
		Parts[Index] = Part;
	}
	return true;
}

void PinguMenu3D::FRing::SetShape(float InnerWidth, float InnerHeight, float InnerRadius, float Gap)
{
	using namespace PinguMenu3DPrivate;
	if (!Root)
	{
		return;
	}
	// The centre line's corner radius R: the tube is RingTube R, and R = InnerRadius + Gap + tube.
	const float R = FMath::Max((FMath::Max(InnerRadius, 1.f) + Gap) / (1.f - RingTube), 1.f);
	Tube = RingTube * R;
	Width = InnerWidth + 2.f * (Gap + Tube);
	Height = InnerHeight + 2.f * (Gap + Tube);
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->Width = Width;
		Part->Height = Height;
		Part->Radius = R;
		Part->Tube = Tube;
		return;
	}
	const float A = FMath::Max(0.5f * Width - R, 0.f);
	const float C = FMath::Max(0.5f * Height - R, 0.f);
	const float S = R / PieceUnit;
	const float LenY = 2.f * A / PieceUnit;
	const float LenZ = 2.f * C / PieceUnit;
	// (Each part also carries its place in the slab - SlabMap, custom primitive data 0..3: offset Y, Z and scale Y, Z - so a
	// material can map the whole slab's face: the wood grain runs on across the joins.)
	auto Place = [](UStaticMeshComponent* Part, const FVector& Location, const FVector& Scale, bool bUsed)
	{
		if (!Part)
		{
			return;
		}
		Part->SetRelativeLocation(Location);
		Part->SetRelativeScale3D(bUsed ? Scale : FVector(1.f));
		Part->SetCustomPrimitiveDataVector4(0, FVector4(Location.Y, Location.Z, Scale.Y, Scale.Z));
		Part->ComponentTags.Remove(TEXT("PinguUnused"));
		if (!bUsed)
		{
			Part->ComponentTags.Add(TEXT("PinguUnused"));
		}
	};
	const bool bWide = A > 0.01f;
	const bool bTall = C > 0.01f;
	Place(Parts[0], FVector(0.f, A, C), FVector(S, S, S), true);
	Place(Parts[1], FVector(0.f, -A, C), FVector(S, -S, S), true);
	Place(Parts[2], FVector(0.f, A, -C), FVector(S, S, -S), true);
	Place(Parts[3], FVector(0.f, -A, -C), FVector(S, -S, -S), true);
	Place(Parts[4], FVector(0.f, -A, C), FVector(S, LenY, S), bWide);
	Place(Parts[5], FVector(0.f, -A, -C), FVector(S, LenY, -S), bWide);
	Place(Parts[6], FVector(0.f, A, -C), FVector(S, S, LenZ), bTall);
	Place(Parts[7], FVector(0.f, -A, -C), FVector(S, -S, LenZ), bTall);
	SetVisible(Root->IsVisible());
}

void PinguMenu3D::FRing::SetColors(const FLinearColor& Front, const FLinearColor& Back, float Glow)
{
	if (UPinguMenu2DPart* Part = Cast<UPinguMenu2DPart>(Root))
	{
		Part->Face = Front;
		Part->Lip = Back;
		Part->Glow = Glow;
	}
	if (Material)
	{
		Material->SetVectorParameterValue(TEXT("Color"), Front);
		Material->SetVectorParameterValue(TEXT("Lip"), Back);
		Material->SetScalarParameterValue(TEXT("Glow"), Glow);
	}
}

void PinguMenu3D::FRing::SetVisible(bool bVisible)
{
	if (!Root)
	{
		return;
	}
	Root->SetVisibility(bVisible, false);
	for (UStaticMeshComponent* Part : Parts)
	{
		if (Part)
		{
			Part->SetVisibility(bVisible && !Part->ComponentHasTag(TEXT("PinguUnused")), false);
		}
	}
}

// ============================================================================ the menu actor

APinguMenu3D::APinguMenu3D()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	// After the camera moved this frame (the player controllers update their cameras between TG_PostPhysics and this group).
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	SetReplicates(false);
	SetCanBeDamaged(false);

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);
	SceneRoot->SetMobility(EComponentMobility::Movable);
	MenuRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MenuRoot"));
	MenuRoot->SetupAttachment(SceneRoot);

	// The menu's own light: from up left, a little behind the camera (it turns with the camera), only for the menu's parts
	// (lighting channel 2). No shadows, no part in the global illumination.
	auto MakeLight = [this](const TCHAR* Name, const FRotator& Direction, float Intensity, const FLinearColor& Color)
	{
		UDirectionalLightComponent* Light = CreateDefaultSubobject<UDirectionalLightComponent>(Name);
		Light->SetupAttachment(GetRootComponent());
		Light->SetRelativeRotation(Direction);
		Light->SetMobility(EComponentMobility::Movable);
		Light->Intensity = Intensity;
		Light->LightColor = Color.ToFColor(true);
		Light->CastShadows = false;
		Light->bAffectsWorld = true;
		Light->bAffectGlobalIllumination = false;
		Light->IndirectLightingIntensity = 0.f;
		Light->VolumetricScatteringIntensity = 0.f;
		Light->bAtmosphereSunLight = false;
		// Never the scene's main directional light (forward shading, translucency, water and fog keep using the sun).
		Light->ForwardShadingPriority = -100;
		Light->LightingChannels.bChannel0 = false;
		Light->LightingChannels.bChannel1 = false;
		Light->LightingChannels.bChannel2 = true;
		return Light;
	};
	KeyLight = MakeLight(TEXT("KeyLight"), FRotator(-38.f, 28.f, 0.f), 1.9f, FLinearColor(1.f, 0.9f, 0.78f));
	FillLight = MakeLight(TEXT("FillLight"), FRotator(20.f, -40.f, 0.f), 0.25f, FLinearColor(1.f, 0.95f, 0.9f));
}

APinguMenu3D* APinguMenu3D::Get(APenguinHUD* InHUD)
{
	UWorld* World = InHUD ? InHUD->GetWorld() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<APinguMenu3D> It(World); It; ++It)
	{
		if (It->HUD.Get() == InHUD && !It->IsActorBeingDestroyed())
		{
			return *It;
		}
	}
	// Deferred: the HUD must be known before BeginPlay builds the pages (their buttons call its actions).
	APinguMenu3D* Menu = World->SpawnActorDeferred<APinguMenu3D>(APinguMenu3D::StaticClass(), FTransform::Identity, InHUD, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Menu)
	{
		Menu->HUD = InHUD;
		Menu->FinishSpawning(FTransform::Identity);
	}
	return Menu;
}

APinguMenu3D* APinguMenu3D::Find(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<APinguMenu3D> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			return *It;
		}
	}
	return nullptr;
}

void APinguMenu3D::BeginPlay()
{
	using namespace PinguMenu3DPrivate;
	Super::BeginPlay();
	if (bBuilt || !PinguMenu3D::IsEnabled())
	{
		return;
	}
	bBuilt = true;
	// The title and the lobby's strip now (the title shows at once); every other page on its first show (EnsureBuilt).
	const double T0 = FPlatformTime::Seconds();
	Title.Id = EPinguMenu3DPage::Title;
	Lobby.Id = EPinguMenu3DPage::Lobby;
	Pause.Id = EPinguMenu3DPage::Pause;
	Lan.Id = EPinguMenu3DPage::Lan;
	Results.Id = EPinguMenu3DPage::Results;
	Settings.Id = EPinguMenu3DPage::Settings;
	Controls.Id = EPinguMenu3DPage::Controls;
	Pingupedia.Id = EPinguMenu3DPage::Pingupedia;
	Board.Id = EPinguMenu3DPage::Board;
	Capture.Id = EPinguMenu3DPage::Capture;
	Error.Id = EPinguMenu3DPage::Error;
	Confirm.Id = EPinguMenu3DPage::Confirm;
	TraitPicks.Id = EPinguMenu3DPage::TraitPicks;
	// The wood menus' shared plain materials (one instance each): snow, iron nails, icicles, the hanging board's ropes, the
	// yellow focus arrow.
	const bool bFlat = PinguMenu3D::IsFlat();
	if (bFlat)
	{
		// The 2D menus (2026-10-27): no lights, no materials, no darkening post process; the actor stands still (its pages are
		// seen through its own camera: GetFlatView), so moving the camera never moves its thousands of parts.
		for (UDirectionalLightComponent* Light : { KeyLight.Get(), FillLight.Get() })
		{
			if (Light)
			{
				Light->bAffectsWorld = false;
				Light->SetVisibility(false);
				Light->MarkRenderStateDirty();
			}
		}
		SetActorLocationAndRotation(FlatPlace, FRotator::ZeroRotator);
	}
	if (UMaterialInterface* Plain = bFlat ? nullptr : PinguMenu3D::GetMaterial())
	{
		auto MakeMid = [this, Plain](const FLinearColor& Color, const FLinearColor& Lip, float Glow, float Roughness)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Plain, this);
			Mid->SetVectorParameterValue(TEXT("Color"), Color);
			Mid->SetVectorParameterValue(TEXT("Lip"), Lip);
			Mid->SetScalarParameterValue(TEXT("Glow"), Glow);
			Mid->SetScalarParameterValue(TEXT("Roughness"), Roughness);
			return Mid;
		};
		SnowMaterial = MakeMid(SnowWhite, SnowShade, 0.04f, 0.6f);
		NailMaterial = MakeMid(FLinearColor(0.11f, 0.11f, 0.12f), FLinearColor(0.03f, 0.03f, 0.035f), 0.f, 0.35f);
		IceMaterial = MakeMid(FLinearColor(0.55f, 0.78f, 0.95f), FLinearColor(0.85f, 0.95f, 1.f), 0.12f, 0.15f);
		RopeLineMaterial = MakeMid(FLinearColor(0.52f, 0.38f, 0.2f), FLinearColor(0.25f, 0.16f, 0.08f), 0.f, 0.85f);
		PointerMaterial = MakeMid(FPinguStyle::Get().Gold, FMath::Lerp(FPinguStyle::Get().Gold, WoodInk, 0.45f), 0.3f, 0.4f);
	}
	FocusArrow = MakePart(MenuRoot, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::Pointer), PointerMaterial);
	if (FocusArrow)
	{
		FocusArrow->SetVisibility(false);
	}
	if (UPinguMenu2DPart* ArrowPart = Cast<UPinguMenu2DPart>(FocusArrow))
	{
		ArrowPart->bOnTop = true;
	}
	if (UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, SpherePath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		for (int32 Index = 0; Index < PuffBits; ++Index)
		{
			FPuffBit& Bit = Puff.AddDefaulted_GetRef();
			Bit.Mesh = MakePart(MenuRoot, Sphere, SnowMaterial);
			if (Bit.Mesh)
			{
				Bit.Mesh->SetVisibility(false);
			}
			if (UPinguMenu2DPart* BitPart = Cast<UPinguMenu2DPart>(Bit.Mesh))
			{
				BitPart->bOnTop = true;
			}
		}
	}
	EnsureBuilt(Title);
	EnsureBuilt(Lobby);
	// The darkening behind the pages that dim the game (pause, settings, ...): a post-process blendable on this actor.
	if (bFlat)
	{
		// (The 2D painter darkens the game itself: GetFlatDim.)
	}
	else if (UMaterialInterface* DimBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/UI/Menu3D/M_Menu3D_DimPP.M_Menu3D_DimPP"), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		DimMaterial = UMaterialInstanceDynamic::Create(DimBase, this);
		DimMaterial->SetScalarParameterValue(TEXT("Dim"), 0.f);
		DimPostProcess = NewObject<UPostProcessComponent>(this, NAME_None, RF_Transient);
		DimPostProcess->SetupAttachment(GetRootComponent());
		DimPostProcess->bUnbound = true;
		DimPostProcess->Priority = 100.f;
		DimPostProcess->BlendWeight = 1.f;
		DimPostProcess->bEnabled = false;
		DimPostProcess->Settings.AddBlendable(DimMaterial, 1.f);
		DimPostProcess->RegisterComponent();
	}
	else
	{
		UE_LOG(LogPingu, Warning, TEXT("[MENU3D] M_Menu3D_DimPP is missing (run phase:ui3d): no darkening behind the menus"));
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] built: title %d buttons, lobby %d buttons (%.0f ms)"), Title.Buttons.Num(), Lobby.Buttons.Num(),
		1000.0 * (FPlatformTime::Seconds() - T0));
}

void APinguMenu3D::UpdateDims()
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	// The game darkens behind the pages that darkened it as Slate screens (APenguinHUD::ScreenDims); the title, the lobby strip
	// and the round board leave it bright. An overlay (PRESS A KEY, the error popup) darkens a little more.
	const bool bPageDims = Page == EPinguMenu3DPage::Pause || Page == EPinguMenu3DPage::Lan || Page == EPinguMenu3DPage::Results
		|| Page == EPinguMenu3DPage::Settings || Page == EPinguMenu3DPage::Controls || Page == EPinguMenu3DPage::Pingupedia
		|| Page == EPinguMenu3DPage::TraitPicks;
	const FPageParts* Parts = GetParts(Page);
	const float PageIn = Parts ? EaseOut(Parts->Appear) : 0.f;
	const FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	const float OverlayIn = OverlayParts ? EaseOut(OverlayParts->Appear) : 0.f;
	const float Dim = FMath::Clamp((bPageDims ? FMath::Max(Style.Dim.A, 0.45f) * PageIn : 0.f) + 0.3f * OverlayIn, 0.f, 0.75f);
	DimAmount = Dim;
	if (DimPostProcess && DimMaterial)
	{
		DimMaterial->SetScalarParameterValue(TEXT("Dim"), Dim);
		DimPostProcess->bEnabled = Dim > 0.005f;
	}
}

void APinguMenu3D::EnsureBuilt(FPageParts& Parts)
{
	if (Parts.bBuilt)
	{
		return;
	}
	Parts.bBuilt = true;
	const double T0 = FPlatformTime::Seconds();
	switch (Parts.Id)
	{
	case EPinguMenu3DPage::Title: BuildTitle(); break;
	case EPinguMenu3DPage::Lobby:
		if (PinguMenu3D::UseIslandLobby())
		{
			BuildLobby();
		}
		else
		{
			BuildCardLobby();
		}
		break;
	case EPinguMenu3DPage::Pause: BuildPause(); break;
	case EPinguMenu3DPage::Lan: BuildLan(); break;
	case EPinguMenu3DPage::Results: BuildResults(); break;
	case EPinguMenu3DPage::Settings: BuildSettings(); break;
	case EPinguMenu3DPage::Controls: BuildControls(); break;
	case EPinguMenu3DPage::Pingupedia: BuildPingupedia(); break;
	case EPinguMenu3DPage::Board: BuildBoard(); break;
	case EPinguMenu3DPage::Capture: BuildCapture(); break;
	case EPinguMenu3DPage::Error: BuildError(); break;
	case EPinguMenu3DPage::Confirm: BuildConfirm(); break;
	case EPinguMenu3DPage::TraitPicks: BuildTraitPicks(); break;
	default: break;
	}
	if (Parts.Root)
	{
		Parts.Root->SetVisibility(false, true);
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] page %s built: %d buttons, %d labels, %d flats, %d decos (%.0f ms)"), PageName(Parts.Id), Parts.Buttons.Num(),
		Parts.Labels.Num(), Parts.Flats.Num(), Parts.Decos.Num(), 1000.0 * (FPlatformTime::Seconds() - T0));
}

void APinguMenu3D::BeginPage(FPageParts& Parts, EPinguMenu3DPage Id, const FVector2D& PlaneSize)
{
	using namespace PinguMenu3DPrivate;
	Parts.Id = Id;
	Parts.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Parts.Root->SetupAttachment(MenuRoot);
	Parts.Root->RegisterComponent();
	if (PlaneSize.X > 0.f && PlaneSize.Y > 0.f)
	{
		// The page's backing (2026-10-26; Linus: "drop the blue panel like the mockup"): a board of upright wooden planks in a
		// darker frame, a snow slab on the frame's top - like the mockups' settings board. Long text and many rows need it.
		// (Since the second pass the plane is only the board's size: loose planks of different lengths show instead.)
		Parts.Plane.Build(this, Parts.Root);
		Parts.FrameSnow = MakePart(Parts.Plane.Root, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::SnowCap), SnowMaterial);
		SetPlaneShape(Parts, PlaneSize.X, PlaneSize.Y);
	}
}

void APinguMenu3D::EndStatic(FPageParts& Parts)
{
	Parts.StaticButtons = Parts.Buttons.Num();
	Parts.StaticLabels = Parts.Labels.Num();
	Parts.StaticFlats = Parts.Flats.Num();
	Parts.StaticDecos = Parts.Decos.Num();
}

namespace PinguMenu3DPrivate
{
	/** How far a button of a kind stands out of its plane. */
	static float ProtrudeOf(EPinguMenu3DKind Kind)
	{
		switch (Kind)
		{
		case EPinguMenu3DKind::Ghost: return GhostProtrude;
		case EPinguMenu3DKind::Row:
		case EPinguMenu3DKind::Slider:
		case EPinguMenu3DKind::ListRow:
		case EPinguMenu3DKind::KeyCap: return RowProtrude;
		case EPinguMenu3DKind::Field: return FieldProtrude;
		case EPinguMenu3DKind::Arrow: return ArrowProtrude;
		default: return ButtonProtrude;
		}
	}
}

APinguMenu3D::FButton& APinguMenu3D::AddButton(FPageParts& Parts, FName Id, EPinguMenu3DKind Kind, const FVector2D& Center, const FVector2D& Size,
	float LabelSize, TFunction<FText()> Label, TFunction<void()> Action, USceneComponent* Parent)
{
	using namespace PinguMenu3DPrivate;
	FButton& Button = Parts.Buttons.AddDefaulted_GetRef();
	Button.Id = Id;
	Button.Kind = Kind;
	const bool bPlain = Kind == EPinguMenu3DKind::Plain;
	Button.Center = Center;
	Button.Size = Size;
	// (Never smaller than MinLabelSize: small texts were hard to read on wood.)
	Button.LabelSize = FMath::Max(LabelSize, MinButtonLabel);
	LabelSize = Button.LabelSize;
	Button.Label = MoveTemp(Label);
	Button.Action = MoveTemp(Action);
	Button.bFocusable = Kind != EPinguMenu3DKind::Arrow;
	if (Kind == EPinguMenu3DKind::Row || Kind == EPinguMenu3DKind::Slider || Kind == EPinguMenu3DKind::Field || Kind == EPinguMenu3DKind::ListRow)
	{
		Button.LabelAlign = PinguMenu3D::ETextAlign::Left;
		Button.bNoInk = true;
	}
	if (Kind == EPinguMenu3DKind::KeyCap || Kind == EPinguMenu3DKind::Arrow || Kind == EPinguMenu3DKind::Segment)
	{
		Button.bNoInk = true;
	}

	Button.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Button.Root->SetupAttachment(Parent ? Parent : Parts.Root);
	Button.Root->RegisterComponent();
	Button.Root->SetRelativeLocation(FVector(0.f, Center.X, Center.Y));
	Button.Body = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	// (The 2D painter takes a plank's place on its page without the Body's animations: PinguMenu2D::GetRestPlace.)
	Button.Body->ComponentTags.Add(PinguMenu2D::RestBodyTag);
	Button.Body->SetupAttachment(Button.Root);
	Button.Body->RegisterComponent();
	Button.Hit = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Button.Hit->SetupAttachment(Button.Root);
	Button.Hit->RegisterComponent();

	// The wooden sign (2026-10-26, Linus's mockup): the buttons are plank signs (an arrow point where a page asks for one:
	// Button.Point, ApplySignShape), nailed on, a continuous snow slab on the top edge; rows, fields, key caps and list rows are
	// quieter planks in the boards (no snow, no nails). Every part shares the plank's wood material instance.
	const bool bRowish = Kind == EPinguMenu3DKind::Row || Kind == EPinguMenu3DKind::Slider || Kind == EPinguMenu3DKind::Field
		|| Kind == EPinguMenu3DKind::ListRow || Kind == EPinguMenu3DKind::KeyCap || Kind == EPinguMenu3DKind::Arrow;
	Button.bSign = !bRowish && !bPlain;
	Button.Point = Button.bSign ? NextSignPoint : 0;
	NextSignPoint = 0;
	Button.bRowPlank = bNextRowPlank;
	bNextRowPlank = false;
	// (2D, style 5: no snow on the standing lobby cards - the second row's snow covered the first row's pills.)
	Button.bSnow = Button.bSign && Size.Y >= 36.f && !Button.bRowPlank && !(PinguMenu3D::IsFlat() && Size.Y > Size.X);
	// (2D, style 5: nails only on boards, the title and banners - never on buttons, UI_STYLE section 4.)
	Button.bNails = Button.bSign && Size.X >= 3.f * Size.Y * 0.5f && !PinguMenu3D::IsFlat();
	// (No soft shadow under list rows: in the PINGUPEDIA's list the shadows doubled up between the rows, 2026-10-27.)
	Button.bShadowed = !Parts.bNoBoard && Kind != EPinguMenu3DKind::ListRow && !bPlain;
	Button.DropOrder = Parts.Buttons.Num() - 1;
	Button.SwayPhase = 2.f * PI * Hash01(Button.DropOrder + 31 * static_cast<int32>(Parts.Id));
	Button.StaticRoll = Button.bSign && Parts.bNoBoard ? (Hash01(Button.DropOrder * 7 + 3) - 0.5f) * 2.4f : 0.f;
	const float Protrude = ProtrudeOf(Kind);
	Button.Slab.Build(this, Button.Body, true, false, true);
	Button.Slab.Depth = Protrude + 8.f;
	if (bPlain)
	{
		Button.Slab.SetVisible(false); // (a plain button has no plank: SetButtonVisible keeps it hidden)
	}
	// The focus rope: a twisted rope round the sign, a gap away from its edge, a little behind its face (it rides with the body).
	Button.Ring.Build(this, Button.Body, true, true);
	Button.Ring.Root->SetRelativeLocation(FVector(RingBack, 0.f, 0.f));
	Button.Ring.SetVisible(false);
	// Its shadow on the board (under the root: it stays on the board while the sign pops). Signs on posts (the title, the lobby
	// strip) have nothing behind them to cast it on.
	if (Button.bShadowed && Button.Shadow.Build(this, Button.Root, true, true))
	{
		Button.Shadow.Root->SetRelativeLocation(FVector(-1.5f, ShadowOffsetY, ShadowOffsetZ));
		Button.Shadow.SetOpacity(ShadowOpacity);
	}
	if (UStaticMesh* TipMesh = PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::ArrowTip))
	{
		if (Button.bSign)
		{
			Button.Tip = MakePart(Button.Body, TipMesh, Button.Slab.Material);
			Button.Tip->SetVisibility(false);
			if (UPinguMenu2DPart* TipPart = Cast<UPinguMenu2DPart>(Button.Tip))
			{
				TipPart->ColorFrom = Cast<UPinguMenu2DPart>(Button.Slab.Root);
			}
		}
	}
	if (Button.bSnow)
	{
		Button.Snow = MakePart(Button.Body, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::SnowCap), SnowMaterial);
	}
	if (Button.bNails)
	{
		for (UStaticMeshComponent*& Nail : Button.Nails)
		{
			Nail = MakePart(Button.Body, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::Nail), NailMaterial);
		}
	}
	ApplySignShape(Button);

	// The selected list row's gold rim (a thin ring close to the edge).
	if (Kind == EPinguMenu3DKind::ListRow)
	{
		Button.SelectRing.Build(this, Button.Body);
		Button.SelectRing.Root->SetRelativeLocation(FVector(2.f, 0.f, 0.f));
		Button.SelectRing.SetShape(Size.X - 6.f, Size.Y - 6.f, Button.Slab.Radius - 3.f, 0.f);
		Button.SelectRing.SetColors(FPinguStyle::Get().Gold, FMath::Lerp(FPinguStyle::Get().Gold, WoodInk, 0.5f), 0.1f);
		Button.SelectRing.SetVisible(false);
	}

	PinguMenu3D::FTextLook Look;
	Look.Size = LabelSize;
	Look.Extrude = LabelDepth;
	Look.Bevel = 0.05f;
	Look.Align = Button.LabelAlign;
	// (Signs with nails keep their letters between the nails.)
	Look.MaxWidth = bPlain ? 0.f : Kind == EPinguMenu3DKind::Field ? Size.X - 2.f * Button.LabelPad - 8.f
		: (Button.bNails ? Size.X - (Button.Point != 0 ? PinguMenu3D::TipLengthShare * Size.Y : 0.f) - 2.f * (NailInset + NailSize) - 10.f : Size.X - 28.f);
	Look.Front = FPinguStyle::Get().Snow;
	Look.Side = WoodInk;
	Button.ShownLabel = Button.Label ? Button.Label().ToString() : FString();
	Button.Text = PinguMenu3D::MakeMenuText(this, Button.Body, FText::FromString(Button.ShownLabel), Look);
	if (!Button.bNoInk)
	{
		PinguMenu3D::FTextLook InkLook = Look;
		InkLook.Extrude = LabelInkDepth;
		InkLook.Outline = LabelInkWidth;
		InkLook.Front = WoodInk;
		InkLook.Side = WoodInk;
		Button.Ink = PinguMenu3D::MakeMenuText(this, Button.Body, FText::FromString(Button.ShownLabel), InkLook);
	}
	PlaceLabel(Button, Button.LabelAlign == PinguMenu3D::ETextAlign::Left ? -0.5f * Size.X + Button.LabelPad : 0.f);
	return Button;
}

void APinguMenu3D::PlaceLabel(FButton& Button, float Y)
{
	using namespace PinguMenu3DPrivate;
	// The letters stand on the face (their back just inside it), their capitals centred on the button; the ink behind them,
	// half as far out, at the letters' own scale (both are fitted to the same width, the ink's box is a little wider).
	// (A centred label sits on the plank's middle, not on the arrow point's.)
	const float Shift = Button.LabelAlign == PinguMenu3D::ETextAlign::Center && Button.Slab.Root ? static_cast<float>(Button.Slab.Root->GetRelativeLocation().Y) : 0.f;
	if (Button.Text)
	{
		Button.Text->SetRelativeLocation(FVector(-LabelDepth * Button.LabelSize + 0.5f, Y + Shift, -0.5f * Button.LabelSize));
	}
	if (Button.Ink && Button.Text)
	{
		Button.Ink->SetRelativeScale3D(Button.Text->GetRelativeScale3D());
		Button.Ink->SetRelativeLocation(FVector(-0.5f * LabelDepth * Button.LabelSize, Y + Shift, -0.5f * Button.LabelSize));
	}
}

UStaticMeshComponent* APinguMenu3D::MakePart(USceneComponent* Parent, UStaticMesh* Mesh, UMaterialInterface* Material)
{
	if (!Parent || !Mesh)
	{
		return nullptr;
	}
	if (PinguMenu3D::IsFlat())
	{
		// The 2D menus: which piece it is (the painter draws its shape); the focus arrow keeps its colours here.
		UPinguMenu2DPart* Flat = NewObject<UPinguMenu2DPart>(this, NAME_None, RF_Transient);
		Flat->Kind = EPinguMenu2DKind::Piece;
		Flat->Piece = EPinguMenu2DPiece::Ball;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(PinguMenu3DPrivate::SignPiecePaths); ++Index)
		{
			if (PinguMenu3D::GetPieceMesh(static_cast<PinguMenu3D::EPiece>(Index)) == Mesh)
			{
				Flat->Piece = static_cast<EPinguMenu2DPiece>(Index);
			}
		}
		if (Flat->Piece == EPinguMenu2DPiece::Pointer)
		{
			Flat->Face = FPinguStyle::Get().Gold;
			Flat->Lip = FMath::Lerp(FPinguStyle::Get().Gold, PinguMenu3DPrivate::WoodInk, 0.45f);
			Flat->Glow = 0.3f;
		}
		Flat->SetupAttachment(Parent);
		Flat->RegisterComponent();
		return Flat;
	}
	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
	Part->SetStaticMesh(Mesh);
	Part->SetupAttachment(Parent);
	Part->SetMobility(EComponentMobility::Movable);
	PinguMenu3D::PreparePrimitive(Part, true);
	Part->RegisterComponent();
	if (Material)
	{
		Part->SetMaterial(0, Material);
	}
	return Part;
}

void APinguMenu3D::ApplySignShape(FButton& Button)
{
	using namespace PinguMenu3DPrivate;
	const float W = Button.Size.X;
	const float H = Button.Size.Y;
	const float Depth = Button.Slab.Depth > 0.f ? Button.Slab.Depth : ProtrudeOf(Button.Kind) + 8.f;
	// A plank: nearly square corners (the arrow point's bevel is made for SignRadiusShare); rows and the like a little rounder.
	const float Radius = Button.bSign ? PinguMenu3D::SignRadiusShare * H : FMath::Min(10.f, 0.3f * H);
	// The arrow point takes the end of the sign's width: the plank is shorter by it and shifted away from it.
	// (2D, style 5: no arrow points - straight cabin planks, UI_STYLE "no ovals, rope or arrows".)
	const int32 Point = Button.Tip && !PinguMenu3D::IsFlat() ? FMath::Clamp(Button.Point, -1, 1) : 0;
	const float TipLength = Point != 0 ? PinguMenu3D::TipLengthShare * H : 0.f;
	const float BodyWidth = FMath::Max(W - TipLength, H);
	const float BodyCenter = -static_cast<float>(Point) * 0.5f * TipLength;
	if (Button.Slab.Root)
	{
		Button.Slab.Root->SetRelativeLocation(FVector(0.f, BodyCenter, 0.f));
	}
	Button.Slab.SetShape(BodyWidth, H, Radius, Depth);
	// Every sign its own piece of the board (the grain does not repeat from sign to sign).
	const float Seed = Hash01(Button.DropOrder * 13 + static_cast<int32>(Button.Center.Y) + 7);
	Button.Slab.SetWood(1.f, FVector2D(Seed * 0.83f, Hash01(Button.DropOrder * 5 + 1) * 0.71f), false, 100.f, 0.f, 0.f,
		FVector2D(FMath::Clamp(W * 1.4f, 260.f, 700.f), FMath::Clamp(H * 2.6f, 110.f, 190.f)));
	if (Button.Tip)
	{
		Button.Tip->SetVisibility(Point != 0 && Button.bShown);
		if (Point != 0)
		{
			const float End = BodyCenter + static_cast<float>(Point) * 0.5f * BodyWidth;
			const FVector Scale(Depth / 100.f, static_cast<float>(Point) * 0.5f * H / 100.f, 0.5f * H / 100.f);
			Button.Tip->SetRelativeLocation(FVector(0.f, End, 0.f));
			Button.Tip->SetRelativeScale3D(Scale);
			// The grain runs on from the plank (its place in the plank's own space).
			Button.Tip->SetCustomPrimitiveDataVector4(0, FVector4(End - BodyCenter, 0.f, Scale.Y, Scale.Z));
		}
	}
	if (Button.Snow)
	{
		// One continuous snow slab along the flat top edge (Linus: "a contiiuos snow slab and not points").
		const float SnowHeight = FMath::Min(SnowMax, SnowShare * H);
		Button.Snow->SetRelativeLocation(FVector(0.f, BodyCenter + static_cast<float>(Point) * 0.1f * Radius, 0.5f * H - 1.5f));
		Button.Snow->SetRelativeScale3D(FVector(Depth / 100.f, (BodyWidth - 0.6f * Radius) / 100.f, SnowHeight / 100.f));
		if (UPinguMenu2DPart* Flat = Cast<UPinguMenu2DPart>(Button.Snow))
		{
			// (2D: the snow strip is laid over the plank's whole body, its scale from the plank's height.)
			Flat->Width = BodyWidth;
			Flat->Height = H;
		}
	}
	for (int32 Side = 0; Side < 2; ++Side)
	{
		if (UStaticMeshComponent* Nail = Button.Nails[Side])
		{
			const float Y = BodyCenter + (Side == 0 ? -1.f : 1.f) * (0.5f * BodyWidth - NailInset);
			Nail->SetRelativeLocation(FVector(-0.2f, Y, 0.f));
			Nail->SetRelativeScale3D(FVector(NailSize / 100.f));
		}
	}
	Button.Ring.SetShape(W, H, Radius, RingGap);
	if (Button.Shadow.IsBuilt())
	{
		Button.Shadow.SetShape(W + 2.f * ShadowSpread, H + 2.f * ShadowSpread, Radius + ShadowSpread, 1.f);
	}
}

int32 APinguMenu3D::LayPlanks(TArray<PinguMenu3D::FSlab>& Planks, TArray<UStaticMeshComponent*>* Nails, USceneComponent* Parent, float Width, float Height,
	int32 Seed, const FLinearColor& Face, const FLinearColor& Lip, float Grain, float Depth, float RowHeight, float Overhang, bool bVisible)
{
	using namespace PinguMenu3DPrivate;
	if (!Parent)
	{
		return 0;
	}
	// Rows of planks covering the area (a little past its top and bottom), each one longer than the area by a different amount,
	// shifted sideways, tilted a fraction of a degree, its neighbours overlapping it a little (every other one a hair further
	// back, so the overlaps never flicker): loose cut-off planks, not a clean box.
	const float Cover = Height + 8.f;
	const int32 Count = FMath::Clamp(FMath::CeilToInt(Cover / FMath::Max(RowHeight, 20.f)), 1, 16);
	const float Step = Cover / static_cast<float>(Count);
	for (int32 Index = 0; Index < FMath::Max(Count, Planks.Num()); ++Index)
	{
		if (Index >= Planks.Num())
		{
			PinguMenu3D::FSlab& Added = Planks.AddDefaulted_GetRef();
			if (!Added.Build(this, Parent, true, false, true))
			{
				Planks.Pop();
				break;
			}
			if (Nails)
			{
				for (int32 Side = 0; Side < 2; ++Side)
				{
					Nails->Add(MakePart(Added.Root, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::Nail), NailMaterial));
				}
			}
		}
		PinguMenu3D::FSlab& Plank = Planks[Index];
		const bool bUsed = Index < Count;
		Plank.SetVisible(bUsed && bVisible);
		if (Nails)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				if (Nails->IsValidIndex(Index * 2 + Side) && (*Nails)[Index * 2 + Side])
				{
					(*Nails)[Index * 2 + Side]->SetVisibility(bUsed && bVisible);
				}
			}
		}
		if (!bUsed)
		{
			continue;
		}
		const float R1 = Hash01(Seed * 31 + Index * 7 + 1);
		const float R2 = Hash01(Seed * 17 + Index * 13 + 5);
		const float R3 = Hash01(Seed * 23 + Index * 3 + 9);
		const float Extra = 0.35f * Overhang + 0.65f * Overhang * R1;
		const float Shift = (R2 - 0.5f) * FMath::Max(Extra - 8.f, 0.f);
		const float PlankWidth = Width + Extra;
		// A board (with nails): planks with a small dark gap between them (the backing shows), each at its own depth, a deeper
		// bevel (2026-10-27, Linus: "make the planks lock a bit more 3d"); a well: overlapping, no gaps.
		const bool bBoardPlanks = Nails != nullptr;
		const float PlankHeight = bBoardPlanks ? Step - 4.f : Step + 14.f;
		// (Askew at most so far that its ends keep the gap / the overlap: the rows never touch or open up.)
		const float MaxRoll = FMath::Min(0.8f, FMath::RadiansToDegrees(FMath::Atan((bBoardPlanks ? 1.5f : 6.f) / FMath::Max(0.5f * PlankWidth, 1.f))));
		const float Z = 0.5f * Cover - Step * (static_cast<float>(Index) + 0.5f);
		// (A board's planks (with nails) step back, a well's step forward: a well never sinks into the board under it.)
		Plank.Root->SetRelativeLocation(FVector(bBoardPlanks ? 5.f * Hash01(Seed * 3 + Index * 29) : -(Index % 2) * 1.2f, Shift, Z));
		Plank.Root->SetRelativeRotation(FRotator(0.f, 0.f, (R3 - 0.5f) * 2.f * MaxRoll));
		Plank.SetShape(PlankWidth, PlankHeight, bBoardPlanks ? FMath::Min(11.f, 0.2f * PlankHeight) : FMath::Min(6.f, 0.2f * PlankHeight), Depth);
		Plank.SetColors(Face * (0.88f + 0.24f * Hash01(Seed + Index * 11)), Lip);
		Plank.SetWood(Grain, FVector2D(R1 * 0.9f, R2 * 0.7f), false, 100.f, 0.f, 0.f, FVector2D(FMath::Clamp(PlankWidth * 0.9f, 300.f, 900.f), 170.f));
		// (2D: a cabin board plank; its variant seeds its tiles.)
		Plank.SetWoodImage(static_cast<uint8>(PinguWood::ECut::Board), 0, static_cast<uint8>((Seed + Index * 3) % 7));
		if (Nails)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				if (Nails->IsValidIndex(Index * 2 + Side) && (*Nails)[Index * 2 + Side])
				{
					UStaticMeshComponent* Nail = (*Nails)[Index * 2 + Side];
					// (Inset max(15, 0.22 H) from the ends, at mid height: UI_STYLE section 4.)
					const float Inset = FMath::Max(15.f, 0.22f * PlankHeight);
					const float NailZ = PinguMenu3D::IsFlat() ? 0.f : (Hash01(Seed + Index * 5 + Side) - 0.5f) * 0.3f * PlankHeight;
					Nail->SetRelativeLocation(FVector(-0.2f, (Side == 0 ? -1.f : 1.f) * (0.5f * PlankWidth - Inset), NailZ));
					Nail->SetRelativeScale3D(FVector(NailSize / 100.f));
				}
			}
		}
	}
	return FMath::Min(Count, Planks.Num());
}

void APinguMenu3D::SetPlaneShape(FPageParts& Parts, float Width, float Height)
{
	using namespace PinguMenu3DPrivate;
	if (!Parts.Plane.IsBuilt())
	{
		return;
	}
	// The plane keeps the board's size (it is never shown); the planks show.
	Parts.Plane.SetShape(Width, Height, BoardRadius, PlaneDepth);
	Parts.Plane.SetVisible(false);
	if (!Parts.Backing.IsBuilt() && !Parts.bNoBoard && Parts.Backing.Build(this, Parts.Plane.Root))
	{
		// The dark wood behind the planks: it shows in the small gaps between them (the board's depth).
		Parts.Backing.SetColors(WoodInk * 2.f, WoodInk);
		Parts.Backing.Root->SetRelativeLocation(FVector(14.f, 0.f, 0.f));
	}
	if (Parts.Backing.IsBuilt())
	{
		Parts.Backing.SetShape(FMath::Max(Width - 24.f, 10.f), FMath::Max(Height - 8.f, 10.f), 10.f, 20.f);
		Parts.Backing.SetVisible(Parts.bShown && !Parts.bNoBoard);
	}
	Parts.PlankCount = LayPlanks(Parts.Planks, &Parts.PlankNails, Parts.Plane.Root, Width, Height, 7 + 13 * static_cast<int32>(Parts.Id),
		BoardFace, BoardLip, BoardGrain, PlaneDepth, Parts.PlankRow > 0.f ? Parts.PlankRow : BoardRow, Parts.PlankOverhang >= 0.f ? Parts.PlankOverhang : 70.f,
		Parts.bShown && !Parts.bNoBoard);
	if (Parts.FrameSnow && Parts.Planks.Num() > 0 && Parts.Planks[0].Root)
	{
		// One continuous snow slab along the top plank.
		const PinguMenu3D::FSlab& Top = Parts.Planks[0];
		if (Parts.FrameSnow->GetAttachParent() != Top.Root)
		{
			Parts.FrameSnow->AttachToComponent(Top.Root, FAttachmentTransformRules::KeepRelativeTransform);
		}
		Parts.FrameSnow->SetRelativeLocation(FVector(0.f, 0.f, 0.5f * Top.Height - 1.5f));
		Parts.FrameSnow->SetRelativeScale3D(FVector(PlaneDepth / 100.f, (Top.Width - 14.f) / 100.f, 16.f / 100.f));
		if (UPinguMenu2DPart* Flat = Cast<UPinguMenu2DPart>(Parts.FrameSnow))
		{
			Flat->Width = Top.Width;
			Flat->Height = Top.Height;
		}
	}
}

int32 APinguMenu3D::AddDecoWood(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, const FVector2D& Size, float Radius, float Depth,
	const FLinearColor& Face, const FLinearColor& Lip, float Protrude, float Grain, bool bPlanks)
{
	FDeco& Deco = Parts.Decos.AddDefaulted_GetRef();
	Deco.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Deco.Root->SetupAttachment(Parent ? Parent : Parts.Root);
	Deco.Root->RegisterComponent();
	Deco.Root->SetRelativeLocation(FVector(-Protrude, Pos.X, Pos.Y));
	const int32 Index = Parts.Decos.Num();
	if (Size.Y >= 110.f && bPlanks)
	{
		// A big well (a list, a card, the found games): a few stained planks of different lengths (2026-10-26, second pass: no
		// crisp boxes), calm grain under the text.
		LayPlanks(Deco.Extra, nullptr, Deco.Root, Size.X - 10.f, Size.Y, 101 + Index * 7, Face, Lip, FMath::Min(Grain, 0.45f), Depth, 64.f, 22.f, true);
		Deco.ShownColor = Face;
		Deco.Lip = Lip;
		return Parts.Decos.Num() - 1;
	}
	Deco.Slab.Build(this, Deco.Root, true, false, true);
	// A strip: its own length (a few units either way) and a hair askew; the texts on its root stay straight.
	const float Jitter = (PinguMenu3DPrivate::Hash01(Index * 19 + 3) - 0.5f) * FMath::Min(14.f, 0.04f * Size.X);
	Deco.Slab.Root->SetRelativeLocation(FVector(0.f, 0.5f * Jitter, 0.f));
	Deco.Slab.Root->SetRelativeRotation(FRotator(0.f, 0.f, (PinguMenu3DPrivate::Hash01(Index * 23 + 9) - 0.5f) * FMath::Min(0.8f, 160.f / FMath::Max(Size.X, 1.f))));
	Deco.Slab.SetShape(Size.X + Jitter, Size.Y, FMath::Min(Radius, 6.f), Depth);
	Deco.Slab.SetColors(Face, Lip);
	Deco.Slab.SetWood(FMath::Min(Grain, 0.5f), FVector2D(PinguMenu3DPrivate::Hash01(Index * 3 + 11), PinguMenu3DPrivate::Hash01(Index * 7 + 5)), false, 100.f, 0.f, 0.f,
		FVector2D(FMath::Clamp(Size.X * 1.2f, 260.f, 800.f), FMath::Clamp(Size.Y * 2.5f, 110.f, 220.f)));
	// (2D: a strip sunk into its board: the inset sign picture.)
	Deco.Slab.SetWoodImage(static_cast<uint8>(PinguWood::ECut::Auto), static_cast<uint8>(PinguWood::EState::Inset));
	Deco.ShownColor = Face;
	Deco.Lip = Lip;
	return Parts.Decos.Num() - 1;
}

void APinguMenu3D::SpawnPuff(const FButton& Button)
{
	using namespace PinguMenu3DPrivate;
	if (!Button.Hit || !MenuRoot || Puff.Num() == 0)
	{
		return;
	}
	if (PinguMenu3D::IsFlat() && PinguWood::GetAtlas())
	{
		// (Style 5, 2026-11-07: no puff on the cabin planks - its balls sat in a row on the snow like stray pips; clean GUI.)
		return;
	}
	// A few snow bits shaken off the sign's top edge: they fly out toward the camera and up, fall and shrink (page units in the
	// menu's own space, so they ride along with the camera like the menu).
	const FTransform ToWorld = Button.Hit->GetComponentTransform();
	const FTransform& Menu = MenuRoot->GetComponentTransform();
	for (int32 Index = 0; Index < Puff.Num(); ++Index)
	{
		FPuffBit& Bit = Puff[Index];
		if (!Bit.Mesh)
		{
			continue;
		}
		const float Along = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Puff.Num()) - 0.5f;
		const FVector Local(-4.f, Along * 0.8f * Button.Size.X, 0.5f * Button.Size.Y + 4.f);
		Bit.Mesh->SetRelativeLocation(Menu.InverseTransformPosition(ToWorld.TransformPosition(Local)));
		Bit.Velocity = FVector(-60.f - 40.f * Hash01(Index + 3), Along * 140.f + (Hash01(Index) - 0.5f) * 60.f, 70.f + 90.f * Hash01(Index * 5 + 2));
		Bit.Size = 3.5f + 3.f * Hash01(Index * 11 + 4);
		Bit.Age = 0.f;
		Bit.Mesh->SetVisibility(true);
	}
}

void APinguMenu3D::UpdatePuff(float Delta)
{
	using namespace PinguMenu3DPrivate;
	for (FPuffBit& Bit : Puff)
	{
		if (!Bit.Mesh || Bit.Age >= PuffTime)
		{
			continue;
		}
		Bit.Age += Delta;
		if (Bit.Age >= PuffTime)
		{
			Bit.Mesh->SetVisibility(false);
			continue;
		}
		Bit.Velocity.Z -= 520.f * Delta;
		Bit.Mesh->SetRelativeLocation(Bit.Mesh->GetRelativeLocation() + Bit.Velocity * Delta);
		const float Left = 1.f - Bit.Age / PuffTime;
		// (The engine's sphere is 100 across.)
		Bit.Mesh->SetRelativeScale3D(FVector(Bit.Size * (0.4f + 0.6f * Left) / 100.f));
	}
}

void APinguMenu3D::UpdateFocusArrow(float Delta)
{
	using namespace PinguMenu3DPrivate;
	if (!FocusArrow)
	{
		return;
	}
	// The yellow arrow beside the sign the keys / the controller are on (not for the mouse: the hovered sign tilts instead).
	FPageParts* Parts = GetInputParts();
	FButton* Target = nullptr;
	// (2026-11-07, Linus: no arrow for the keyboard focus - it looks like the hover: PinguWood::HotLook.)
	if (PinguWood::HotLook().bArrow && Parts && Parts->bShown && !Parts->bHotByMouse && !IsMouseOnlyPage() && Parts->Buttons.IsValidIndex(Parts->Hot))
	{
		FButton& Hot = Parts->Buttons[Parts->Hot];
		if (Hot.bShown && Hot.Hit && Hot.bFocusable)
		{
			Target = &Hot;
		}
	}
	FocusArrowIn = Approach(FocusArrowIn, Target ? 1.f : 0.f, 8.f, Delta);
	if (!Target)
	{
		if (FocusArrowIn <= 0.f)
		{
			FocusArrow->SetVisibility(false);
		}
		return;
	}
	if (FocusArrow->GetAttachParent() != Target->Hit)
	{
		FocusArrow->AttachToComponent(Target->Hit, FAttachmentTransformRules::KeepRelativeTransform);
	}
	const float Bob = ArrowBob * (0.5f + 0.5f * FMath::Sin(2.f * PI * Clock * 1.6f));
	const float Half = FMath::Min(ArrowHalf, 0.36f * Target->Size.Y + 4.f);
	const float Length = 1.5f * Half;
	const float In = EaseOut(FocusArrowIn);
	if (Parts->Id == EPinguMenu3DPage::Lobby && !bCardLobby)
	{
		// The lobby strip's signs stand close side by side: the arrow points down at the sign from above its snow instead.
		FocusArrow->SetRelativeRotation(FRotator(0.f, 0.f, 90.f));
		FocusArrow->SetRelativeLocation(FVector(-2.f, 0.f, 0.5f * Target->Size.Y + SnowMax + 4.f + Length + Bob + (1.f - In) * 30.f));
	}
	else
	{
		FocusArrow->SetRelativeRotation(FRotator::ZeroRotator);
		FocusArrow->SetRelativeLocation(FVector(-2.f, -0.5f * Target->Size.X - ArrowGap - Length - Bob - (1.f - In) * 30.f, 0.f));
	}
	FocusArrow->SetRelativeScale3D(FVector(8.f / 100.f, Half / 100.f * In, Half / 100.f * In));
	FocusArrow->SetVisibility(true);
}

APinguMenu3D::FButton& APinguMenu3D::AddOptionRow(FPageParts& Parts, FName Id, const FVector2D& Center, float Width, TFunction<FText()> Label,
	TFunction<FText()> Value, TFunction<void(int32, bool)> OnChange, float ValueWidth, TFunction<bool()> Visible)
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	// "LABEL        < VALUE >": a click or Enter goes to the next value and wraps, Left / Right (and the arrows) step and stop
	// at the ends (as the Slate row).
	TSharedRef<TFunction<void(int32, bool)>> Change = MakeShared<TFunction<void(int32, bool)>>(MoveTemp(OnChange));
	const int32 Index = Parts.Buttons.Num();
	{
		FButton& Row = AddButton(Parts, Id, EPinguMenu3DKind::Row, Center, FVector2D(Width, 40.f), 14.f, MoveTemp(Label), [Change] { (*Change)(1, true); });
		Row.Value = MoveTemp(Value);
		Row.ValueWidth = ValueWidth;
		Row.Visible = Visible;
		Row.OnStep = [Change](int32 Direction) { (*Change)(Direction, false); };
		PinguMenu3D::FTextLook Look;
		Look.Size = 14.f;
		Look.Extrude = LabelDepth;
		Look.Bevel = 0.05f;
		Look.Align = PinguMenu3D::ETextAlign::Center;
		Look.MaxWidth = ValueWidth - 8.f;
		Look.Front = Style.Snow;
		Look.Side = Style.Ink;
		Row.ShownValue = Row.Value ? Row.Value().ToString() : FString();
		Row.ValueText = PinguMenu3D::MakeMenuText(this, Row.Body, FText::FromString(Row.ShownValue), Look);
		if (Row.ValueText)
		{
			Row.ValueText->SetRelativeLocation(FVector(-LabelDepth * 14.f + 0.5f, 0.5f * Width - 4.f - ArrowInset - 32.f - 0.5f * ValueWidth, -7.f));
		}
	}
	// The arrows: small knobs either side of the value (not focusable: the keys step the row). (The row's reference above is
	// not used past here: adding the arrows may move the array.)
	const float Right = 0.5f * Width - 4.f - ArrowInset;
	const FVector2D ArrowSize(28.f, 28.f);
	auto AddArrow = [&](const TCHAR* Suffix, float X, int32 Direction)
	{
		FButton& Arrow = AddButton(Parts, FName(*(Id.ToString() + Suffix)), EPinguMenu3DKind::Arrow, FVector2D(Center.X + X, Center.Y), ArrowSize, 12.f,
			[Direction] { return FText::FromString(Direction < 0 ? TEXT("<") : TEXT(">")); }, [Change, Direction] { (*Change)(Direction, false); });
		Arrow.Visible = Visible;
	};
	AddArrow(TEXT("Prev"), Right - 32.f - ValueWidth - 16.f, -1);
	AddArrow(TEXT("Next"), Right - 16.f, 1);
	return Parts.Buttons[Index];
}

APinguMenu3D::FButton& APinguMenu3D::AddSlider(FPageParts& Parts, FName Id, const FVector2D& Center, float Width, TFunction<FText()> Label,
	TFunction<float()> Value, TFunction<void(float)> OnChanged)
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	TSharedRef<TFunction<void(float)>> Change = MakeShared<TFunction<void(float)>>(MoveTemp(OnChanged));
	TSharedRef<TFunction<float()>> Get = MakeShared<TFunction<float()>>(MoveTemp(Value));
	// "LABEL      [====o----]  80%": a click / drag on the track sets it; Left / Right step by 5 %; Enter does nothing.
	FButton& Row = AddButton(Parts, Id, EPinguMenu3DKind::Slider, Center, FVector2D(Width, 40.f), 14.f, MoveTemp(Label), nullptr);
	Row.SliderValue = [Get] { return (*Get)(); };
	Row.OnSlider = [Change](float NewValue) { (*Change)(NewValue); };
	Row.OnStep = [Get, Change](int32 Direction) { (*Change)(FMath::Clamp(FMath::RoundToFloat(((*Get)() + 0.05f * Direction) * 20.f) / 20.f, 0.f, 1.f)); };
	Row.Value = [Get] { return FText::Format(LOCTEXT("Percent", "{0}%"), FText::AsNumber(FMath::RoundToInt((*Get)() * 100.f))); };
	Row.ValueWidth = 64.f;
	PinguMenu3D::FTextLook Look;
	Look.Size = 14.f;
	Look.Extrude = LabelDepth;
	Look.Bevel = 0.05f;
	Look.Align = PinguMenu3D::ETextAlign::Right;
	Look.Front = Style.Snow;
	Look.Side = Style.Ink;
	Row.ShownValue = Row.Value().ToString();
	Row.ValueText = PinguMenu3D::MakeMenuText(this, Row.Body, FText::FromString(Row.ShownValue), Look);
	if (Row.ValueText)
	{
		Row.ValueText->SetRelativeLocation(FVector(-LabelDepth * 14.f + 0.5f, 0.5f * Width - 16.f, -7.f));
	}
	// The track: a dark groove; the knob: a snow disc that rides on it.
	const float TrackCenter = 0.5f * Width - 16.f - 64.f - 100.f;
	Row.Track.Build(this, Row.Body);
	Row.Track.SetShape(200.f, 8.f, 4.f, 3.f);
	Row.Track.Root->SetRelativeLocation(FVector(-2.f, TrackCenter, 0.f));
	Row.Track.SetColors(WoodInk, WoodInk);
	// The knob: a round slice of a log (wood).
	Row.Knob.Build(this, Row.Body, true, false, true);
	Row.Knob.SetShape(22.f, 22.f, 11.f, 9.f);
	Row.Knob.SetColors(WoodLight, WoodLightLip);
	Row.Knob.SetWood(1.f, FVector2D(0.3f, 0.6f), true, 100.f, 0.f, 0.f, FVector2D(60.f, 40.f));
	UpdateSlider(Row);
	return Row;
}

APinguMenu3D::FButton& APinguMenu3D::AddField(FPageParts& Parts, FName Id, const FVector2D& Center, const FVector2D& Size, const FText& Hint,
	int32 MaxLength, TFunction<void(const FString&)> OnChanged, TFunction<void()> OnEnter)
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	FButton& Field = AddButton(Parts, Id, EPinguMenu3DKind::Field, Center, Size, 15.f, [] { return FText::GetEmpty(); }, MoveTemp(OnEnter));
	Field.FieldHint = Hint;
	Field.FieldMax = MaxLength;
	Field.OnFieldChanged = MoveTemp(OnChanged);
	// A click into the field only makes it hot (the typing goes there); Enter does the action.
	Field.Caret.Build(this, Field.Body);
	Field.Caret.SetShape(2.5f, 20.f, 1.f, 2.f);
	Field.Caret.SetColors(Style.Snow, Style.Snow, 0.3f);
	Field.Caret.SetVisible(false);
	UpdateField(Field, false);
	return Field;
}

int32 APinguMenu3D::AddLabel(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, TFunction<FText()> Label,
	const FLinearColor& Color, PinguMenu3D::ETextAlign Align, float MaxWidth, bool bInk, float Protrude)
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	// (Never smaller than MinLabelSize: small texts were hard to read on wood - 2026-10-26, second pass.)
	Size = FMath::Max(Size, MinLabelSize);
	FLabel& Item = Parts.Labels.AddDefaulted_GetRef();
	Item.Label = MoveTemp(Label);
	Item.Shown = Item.Label ? Item.Label().ToString() : FString();
	Item.ShownColor = OnWood(Color);
	Item.Side = FMath::Lerp(Item.ShownColor, WoodInk, 0.6f);
	PinguMenu3D::FTextLook Look;
	Look.Size = Size;
	Look.Extrude = Size >= 30.f ? 0.16f : LabelDepth;
	Look.Bevel = Size >= 30.f ? 0.045f : 0.05f;
	Look.Align = Align;
	Look.MaxWidth = MaxWidth;
	Look.Front = Item.ShownColor;
	Look.Side = Item.Side;
	Item.Text = PinguMenu3D::MakeMenuText(this, Parent ? Parent : Parts.Root, FText::FromString(Item.Shown), Look);
	if (Item.Text)
	{
		Item.Text->SetRelativeLocation(FVector(-Look.Extrude * Size - Protrude - 0.5f, Pos.X, Pos.Y - 0.5f * Size));
	}
	// (On a board the letters get a dark edge too - light text on the light planks needs it, 2026-10-26; texts on rows and
	// wells do not.)
	if (bInk || (Parent == nullptr && !Parts.bNoBoard && Parts.Plane.IsBuilt()))
	{
		PinguMenu3D::FTextLook InkLook = Look;
		InkLook.Extrude = 0.12f;
		InkLook.Outline = 0.12f;
		InkLook.Front = WoodInk;
		InkLook.Side = WoodInk;
		Item.Ink = PinguMenu3D::MakeMenuText(this, Parent ? Parent : Parts.Root, FText::FromString(Item.Shown), InkLook);
		if (Item.Ink)
		{
			Item.Ink->SetRelativeScale3D(Item.Text ? Item.Text->GetRelativeScale3D() : Item.Ink->GetRelativeScale3D());
			Item.Ink->SetRelativeLocation(FVector(-InkLook.Extrude * Size - Protrude + 1.f, Pos.X, Pos.Y - 0.5f * Size));
		}
	}
	return Parts.Labels.Num() - 1;
}

int32 APinguMenu3D::AddFlat(FPageParts& Parts, USceneComponent* Space, float Left, float Top, float Width, float Height, TFunction<FText()> Text,
	bool bTitleFont, float Size, const FLinearColor& Color, int32 Justify)
{
	FFlat& Flat = Parts.Flats.AddDefaulted_GetRef();
	Flat.Space = Space ? Space : Parts.Root;
	Flat.Left = Left;
	Flat.Top = Top;
	Flat.Width = Width;
	Flat.Height = Height;
	Flat.Text = MoveTemp(Text);
	Flat.bTitleFont = bTitleFont;
	Flat.Size = Size;
	const FLinearColor Fixed = Color;
	Flat.Color = [Fixed] { return Fixed; };
	Flat.Justify = Justify;
	++FlatVersion;
	return Parts.Flats.Num() - 1;
}

int32 APinguMenu3D::AddDecoSlab(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, const FVector2D& Size, float Radius, float Depth,
	const FLinearColor& Face, const FLinearColor& Lip, float Protrude)
{
	FDeco& Deco = Parts.Decos.AddDefaulted_GetRef();
	Deco.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Deco.Root->SetupAttachment(Parent ? Parent : Parts.Root);
	Deco.Root->RegisterComponent();
	Deco.Root->SetRelativeLocation(FVector(-Protrude, Pos.X, Pos.Y));
	Deco.Slab.Build(this, Deco.Root);
	Deco.Slab.SetShape(Size.X, Size.Y, Radius, Depth);
	Deco.Slab.SetColors(Face, Lip);
	Deco.ShownColor = Face;
	Deco.Lip = Lip;
	return Parts.Decos.Num() - 1;
}

int32 APinguMenu3D::AddDecoRing(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, const FVector2D& Size, float Radius, float Gap,
	const FLinearColor& Front, const FLinearColor& Back, float Protrude)
{
	using namespace PinguMenu3DPrivate;
	// A round ring in the 2D menus (the portraits' team rings) is a render, not custom vertices: a very high-poly torus drawn in
	// Blender (Tools/ArtGen/make_ui_ring.py, /Game/UI/Menu3D/T_UI2D_Ring), neutral, tinted with the team colour: perfectly round at
	// any resolution (2026-11-07, Linus: "make the ring round ur penguin in the lobby also be a render that was way higher poly so
	// no edges"). Same size as the tube it replaces: centre line R, tube 0.17 R, outer R + T; the picture's outer edge is
	// RingTextureFill of its half size. Without the texture (not imported yet) the vertex ring below is the fallback.
	if (PinguMenu3D::IsFlat() && FMath::IsNearlyEqual(Size.X, Size.Y, 0.01) && Radius >= 0.5f * Size.X - 0.01f)
	{
		static const TCHAR* RingTexturePath = TEXT("/Game/UI/Menu3D/T_UI2D_Ring.T_UI2D_Ring");
		constexpr float RingTextureFill = 0.96f;
		UTexture* RingTexture = LoadObject<UTexture>(nullptr, RingTexturePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (RingTexture)
		{
			const float R = FMath::Max((FMath::Max(Radius, 1.f) + Gap) / (1.f - RingTube), 1.f);
			const float Outer = R * (1.f + RingTube);
			const int32 Index = AddDecoIcon(Parts, Parent, Pos, 2.f * Outer / RingTextureFill, RingTexture, Front, Protrude);
			Parts.Decos[Index].Lip = Back;
			// The shine: the render's white highlight over the tinted tube (a separate picture, so it stays white in every team colour).
			if (UTexture* Gloss = LoadObject<UTexture>(nullptr, TEXT("/Game/UI/Menu3D/T_UI2D_RingGloss.T_UI2D_RingGloss"), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				AddDecoIcon(Parts, Parent, Pos, 2.f * Outer / RingTextureFill, Gloss, FLinearColor::White, Protrude + 0.2f);
			}
			return Index;
		}
	}
	FDeco& Deco = Parts.Decos.AddDefaulted_GetRef();
	Deco.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Deco.Root->SetupAttachment(Parent ? Parent : Parts.Root);
	Deco.Root->RegisterComponent();
	Deco.Root->SetRelativeLocation(FVector(-Protrude, Pos.X, Pos.Y));
	Deco.Ring.Build(this, Deco.Root);
	Deco.Ring.SetShape(Size.X, Size.Y, Radius, Gap);
	Deco.Ring.SetColors(Front, Back, 0.f);
	Deco.ShownColor = Front;
	Deco.Lip = Back;
	return Parts.Decos.Num() - 1;
}

int32 APinguMenu3D::AddDecoIcon(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, UTexture* Texture, const FLinearColor& Tint,
	float Protrude)
{
	using namespace PinguMenu3DPrivate;
	if (!IconMesh.IsValid())
	{
		IconMesh = LoadObject<UStaticMesh>(nullptr, IconMeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!IconMaterial.IsValid())
	{
		IconMaterial = LoadObject<UMaterialInterface>(nullptr, IconMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	FDeco& Deco = Parts.Decos.AddDefaulted_GetRef();
	Deco.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Deco.Root->SetupAttachment(Parent ? Parent : Parts.Root);
	Deco.Root->RegisterComponent();
	Deco.Root->SetRelativeLocation(FVector(-Protrude, Pos.X, Pos.Y));
	Deco.ShownColor = Tint;
	if (PinguMenu3D::IsFlat())
	{
		// The 2D menus: the picture on a square of Size (the painter draws the texture).
		if (Texture)
		{
			UPinguMenu2DPart* Flat = NewObject<UPinguMenu2DPart>(this, NAME_None, RF_Transient);
			Flat->Kind = EPinguMenu2DKind::Icon;
			Flat->Texture = Texture;
			Flat->Tint = Tint;
			Flat->SetupAttachment(Deco.Root);
			Flat->RegisterComponent();
			Flat->SetRelativeScale3D(FVector(Size / 100.f));
			Deco.Icon = Flat;
		}
		return Parts.Decos.Num() - 1;
	}
	if (!IconMesh.IsValid() || !IconMaterial.IsValid() || !Texture)
	{
		return Parts.Decos.Num() - 1;
	}
	Deco.Icon = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
	Deco.Icon->SetStaticMesh(IconMesh.Get());
	Deco.Icon->SetupAttachment(Deco.Root);
	Deco.Icon->SetMobility(EComponentMobility::Movable);
	PinguMenu3D::PreparePrimitive(Deco.Icon, true);
	// The engine's plane lies in XY facing +Z, its picture's top at -Y: turned to face the camera (-X), the picture's right along
	// +Y and its top up (+Z); 100 units wide.
	Deco.Icon->SetRelativeRotation(FMatrix(FVector(0.f, 1.f, 0.f), FVector(0.f, 0.f, -1.f), FVector(-1.f, 0.f, 0.f), FVector::ZeroVector).Rotator());
	Deco.Icon->SetRelativeScale3D(FVector(Size / 100.f));
	Deco.Icon->RegisterComponent();
	Deco.IconMaterial = UMaterialInstanceDynamic::Create(IconMaterial.Get(), this);
	Deco.IconMaterial->SetTextureParameterValue(TEXT("Tex"), Texture);
	Deco.IconMaterial->SetVectorParameterValue(TEXT("Tint"), Tint);
	Deco.Icon->SetMaterial(0, Deco.IconMaterial);
	return Parts.Decos.Num() - 1;
}

void APinguMenu3D::AddPortrait(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Size, int32 Slot, int32 TeamRing, float Protrude)
{
	const FPinguStyle& Style = FPinguStyle::Get();
	const APenguinHUD* PenguinHUD = HUD.Get();
	const FLinearColor Color = APenguinCharacter::GetSlotColor(Slot);
	// A disc in the slot's colour, the face on it, the beanie layer tinted over it; a team: a ring in its colour outside.
	AddDecoSlab(Parts, Parent, Pos, FVector2D(Size, Size), 0.5f * Size, 6.f, Color * 0.75f + FLinearColor(0.05f, 0.05f, 0.08f, 0.f), Style.Ink, Protrude);
	const FSlateBrush* Face = PenguinHUD ? PenguinHUD->GetPortraitBrush(Slot, Size) : nullptr;
	const FSlateBrush* Felt = PenguinHUD ? PenguinHUD->GetPortraitFeltBrush(Slot, Size) : nullptr;
	UTexture* FaceTexture = Face ? Cast<UTexture>(Face->GetResourceObject()) : nullptr;
	UTexture* FeltTexture = Felt ? Cast<UTexture>(Felt->GetResourceObject()) : nullptr;
	if (FaceTexture)
	{
		AddDecoIcon(Parts, Parent, Pos, Size - 6.f, FaceTexture, FLinearColor::White, Protrude + 1.f);
	}
	if (FeltTexture)
	{
		AddDecoIcon(Parts, Parent, Pos, Size - 6.f, FeltTexture, APenguinCharacter::GetBeanieTint(Slot), Protrude + 1.5f);
	}
	if (TeamRing >= 0)
	{
		AddDecoRing(Parts, Parent, Pos, FVector2D(Size, Size), 0.5f * Size, 1.f, APenguinCharacter::GetTeamColor(TeamRing),
			FMath::Lerp(APenguinCharacter::GetTeamColor(TeamRing), Style.Ink, 0.5f), Protrude);
	}
}

float APinguMenu3D::AddPill(FPageParts& Parts, USceneComponent* Parent, const FVector2D& Pos, float Height, float TextSize, const FText& Text,
	const FLinearColor& Fill, const FLinearColor& TextColor, float Protrude, PinguMenu3D::ETextAlign Anchor)
{
	const FPinguStyle& Style = FPinguStyle::Get();
	// (2026-11-07, Linus: "make every thing thats wood in gui a wood plank and not just a wooden oval": in 2D a pill is a small
	// plank of the UI's wood; a coloured pill (FULL, IN MATCH, YOU, a team, CUSTOM) puts its colour into its words.)
	const bool bWoodChip = PinguMenu3D::IsFlat() && PinguWood::GetAtlas() != nullptr;
	const bool bWoodFill = Fill.Equals(PinguMenu3D::WoodRow * 0.9f, 0.01f);
	const FLinearColor Words = bWoodChip && !bWoodFill ? PinguWood::WordsOf(Fill) : TextColor;
	// The word first (its width decides the pill's), then the pill under it.
	const int32 LabelIndex = AddLabel(Parts, Parent, FVector2D(0.f, Pos.Y), TextSize, [Text] { return Text; }, Words, PinguMenu3D::ETextAlign::Center, 0.f,
		false, Protrude + 4.f);
	const float TextWidth = Parts.Labels.IsValidIndex(LabelIndex) ? PinguMenu3D::GetTextWidth(Parts.Labels[LabelIndex].Text) : 40.f;
	// (16 each side since 2026-10-26: the overlay's dark edge round the words needs the room.)
	const float Width = TextWidth + 2.f * 16.f;
	const float Center = Anchor == PinguMenu3D::ETextAlign::Left ? Pos.X + 0.5f * Width : (Anchor == PinguMenu3D::ETextAlign::Right ? Pos.X - 0.5f * Width : Pos.X);
	if (Parts.Labels.IsValidIndex(LabelIndex) && Parts.Labels[LabelIndex].Text)
	{
		const FVector Old = Parts.Labels[LabelIndex].Text->GetRelativeLocation();
		Parts.Labels[LabelIndex].Text->SetRelativeLocation(FVector(Old.X, Center, Old.Z));
		if (USceneComponent* PillInk = Parts.Labels[LabelIndex].Ink)
		{
			const FVector OldInk = PillInk->GetRelativeLocation();
			PillInk->SetRelativeLocation(FVector(OldInk.X, Center, OldInk.Z));
		}
	}
	const int32 Deco = AddDecoSlab(Parts, Parent, FVector2D(Center, Pos.Y), FVector2D(Width, Height), 0.5f * Height, 4.f, Fill, FMath::Lerp(Fill, Style.Ink, 0.5f), Protrude);
	if (bWoodChip && Parts.Decos.IsValidIndex(Deco))
	{
		if (UPinguMenu2DPart* Chip = Cast<UPinguMenu2DPart>(Parts.Decos[Deco].Slab.Root))
		{
			Chip->bWood = true;
			Parts.Decos[Deco].Slab.SetWoodImage(static_cast<uint8>(PinguWood::ECut::Pill), static_cast<uint8>(PinguWood::EState::Normal));
		}
	}
	return Width;
}

void APinguMenu3D::DestroyText(USceneComponent*& Text)
{
	if (!Text)
	{
		return;
	}
	PinguMenu3DPrivate::TextStates().Remove(Text);
	// Text3D's glyph parts belong to the actor: they go with the text.
	TArray<USceneComponent*> GlyphParts;
	Text->GetChildrenComponents(true, GlyphParts);
	Text->DestroyComponent();
	for (USceneComponent* Part : GlyphParts)
	{
		if (IsValid(Part))
		{
			Part->DestroyComponent();
		}
	}
	Text = nullptr;
}

void APinguMenu3D::DestroySlab(PinguMenu3D::FSlab& Slab)
{
	for (UStaticMeshComponent*& Part : Slab.Parts)
	{
		if (Part)
		{
			Part->DestroyComponent();
			Part = nullptr;
		}
	}
	if (Slab.Root)
	{
		Slab.Root->DestroyComponent();
		Slab.Root = nullptr;
	}
	Slab.Material = nullptr;
}

void APinguMenu3D::DestroyRing(PinguMenu3D::FRing& Ring)
{
	for (UStaticMeshComponent*& Part : Ring.Parts)
	{
		if (Part)
		{
			Part->DestroyComponent();
			Part = nullptr;
		}
	}
	if (Ring.Root)
	{
		Ring.Root->DestroyComponent();
		Ring.Root = nullptr;
	}
	Ring.Material = nullptr;
}

void APinguMenu3D::DestroyButton(FButton& Button)
{
	DestroyText(Button.Text);
	DestroyText(Button.Ink);
	DestroyText(Button.ValueText);
	DestroySlab(Button.Slab);
	DestroySlab(Button.Shadow);
	DestroySlab(Button.Track);
	DestroySlab(Button.Knob);
	DestroySlab(Button.Caret);
	DestroyRing(Button.Ring);
	DestroyRing(Button.SelectRing);
	DestroySlab(Button.Post);
	// (The focus arrow may be on this sign: it goes back to the menu's root first.)
	if (FocusArrow && Button.Hit && FocusArrow->GetAttachParent() == Button.Hit)
	{
		FocusArrow->AttachToComponent(MenuRoot, FAttachmentTransformRules::KeepRelativeTransform);
		FocusArrow->SetVisibility(false);
	}
	for (UStaticMeshComponent* Part : { Button.Tip, Button.Snow, Button.Nails[0], Button.Nails[1] })
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	Button.Tip = nullptr;
	Button.Snow = nullptr;
	Button.Nails[0] = nullptr;
	Button.Nails[1] = nullptr;
	if (Button.Hit)
	{
		Button.Hit->DestroyComponent();
		Button.Hit = nullptr;
	}
	if (Button.Body)
	{
		Button.Body->DestroyComponent();
		Button.Body = nullptr;
	}
	if (Button.Root)
	{
		Button.Root->DestroyComponent();
		Button.Root = nullptr;
	}
}

void APinguMenu3D::DestroyLabel(FLabel& Label)
{
	DestroyText(Label.Text);
	DestroyText(Label.Ink);
}

void APinguMenu3D::DestroyDeco(FDeco& Deco)
{
	DestroySlab(Deco.Slab);
	for (PinguMenu3D::FSlab& Plank : Deco.Extra)
	{
		DestroySlab(Plank);
	}
	Deco.Extra.Reset();
	DestroyRing(Deco.Ring);
	if (Deco.Icon)
	{
		Deco.Icon->DestroyComponent();
		Deco.Icon = nullptr;
	}
	if (Deco.Root)
	{
		Deco.Root->DestroyComponent();
		Deco.Root = nullptr;
	}
}

void APinguMenu3D::ClearRows(FPageParts& Parts)
{
	for (int32 Index = Parts.Buttons.Num() - 1; Index >= Parts.StaticButtons; --Index)
	{
		DestroyButton(Parts.Buttons[Index]);
	}
	Parts.Buttons.SetNum(Parts.StaticButtons);
	for (int32 Index = Parts.Labels.Num() - 1; Index >= Parts.StaticLabels; --Index)
	{
		DestroyLabel(Parts.Labels[Index]);
	}
	Parts.Labels.SetNum(Parts.StaticLabels);
	for (int32 Index = Parts.Decos.Num() - 1; Index >= Parts.StaticDecos; --Index)
	{
		DestroyDeco(Parts.Decos[Index]);
	}
	Parts.Decos.SetNum(Parts.StaticDecos);
	if (Parts.Flats.Num() != Parts.StaticFlats)
	{
		Parts.Flats.SetNum(Parts.StaticFlats);
		++FlatVersion;
	}
	for (USceneComponent* Root : Parts.RowRoots)
	{
		if (Root)
		{
			Root->DestroyComponent();
		}
	}
	Parts.RowRoots.Reset();
	if (Parts.Hot >= Parts.Buttons.Num())
	{
		Parts.Hot = INDEX_NONE;
	}
	Parts.RowsKey.Reset();
}

void APinguMenu3D::BuildTitle()
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	Title.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Title.Root->SetupAttachment(MenuRoot);
	Title.Root->RegisterComponent();

	// (The plane only keeps the card's size: the title has no backing - a hanging board and signs on a post, Linus's mockup.)
	Title.bNoBoard = true;
	Title.Plane.Build(this, Title.Root);
	Title.Plane.SetShape(TitleWidth, TitleHeight, PlaneRadius, PlaneDepth);
	Title.Plane.SetVisible(false);

	// The hanging title board (2026-10-26, Linus's mockup): three wooden planks on two ropes, snow on top, icicles under it,
	// the logo on it. It swings a little on its ropes from TitleBoard (its pivot high up, where the ropes go out of the picture).
	const float Top = 0.5f * TitleHeight;
	constexpr float BoardPivot = 700.f;
	TitleBoard = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	TitleBoard->SetupAttachment(Title.Root);
	TitleBoard->RegisterComponent();
	TitleBoard->SetRelativeLocation(FVector(0.f, 0.f, BoardPivot));
	struct FPlankPlace { float Z; float Width; float Y; };
	const FPlankPlace Planks[3] = { { Top - 53.f, 500.f, -6.f }, { Top - 125.f, 520.f, 6.f }, { Top - 197.f, 494.f, -3.f } };
	constexpr float PlankHeight = 74.f;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		PinguMenu3D::FSlab& Plank = TitlePlanks[Index];
		if (!Plank.Build(this, TitleBoard, true, false, true))
		{
			continue;
		}
		Plank.Root->SetRelativeLocation(FVector(0.f, Planks[Index].Y, Planks[Index].Z - BoardPivot));
		Plank.SetShape(Planks[Index].Width, PlankHeight, 8.f, 28.f);
		Plank.SetColors(FMath::Lerp(WoodFace, WoodLight, 0.25f * Index), WoodLip);
		Plank.SetWood(0.75f, FVector2D(0.31f * Index, 0.17f + 0.23f * Index), false, 100.f, 0.f, 0.f, FVector2D(640.f, 170.f));
		Plank.SetWoodImage(static_cast<uint8>(PinguWood::ECut::Board), 0, static_cast<uint8>(Index + 1));
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (UStaticMeshComponent* Nail = MakePart(Plank.Root, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::Nail), NailMaterial))
			{
				Nail->SetRelativeLocation(FVector(-0.2f, (Side == 0 ? -1.f : 1.f) * (0.5f * Planks[Index].Width - 18.f), (Index % 2 == 0 ? 8.f : -8.f)));
				Nail->SetRelativeScale3D(FVector(5.f / 100.f));
			}
		}
	}
	if (UStaticMeshComponent* BoardSnow = MakePart(TitleBoard, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::SnowCap), SnowMaterial))
	{
		BoardSnow->SetRelativeLocation(FVector(0.f, Planks[0].Y, Planks[0].Z + 0.5f * PlankHeight - 2.f - BoardPivot));
		BoardSnow->SetRelativeScale3D(FVector(28.f / 100.f, (Planks[0].Width - 10.f) / 100.f, 20.f / 100.f));
		if (UPinguMenu2DPart* Flat = Cast<UPinguMenu2DPart>(BoardSnow))
		{
			Flat->Width = Planks[0].Width;
			Flat->Height = PlankHeight;
		}
	}
	// Icicles under the bottom plank (lengths and places fixed: the same board every time).
	{
		const float Under = Planks[2].Z - 0.5f * PlankHeight + 2.f - BoardPivot;
		static const float IcicleY[] = { -214.f, -170.f, -118.f, -64.f, -22.f, 30.f, 86.f, 132.f, 181.f, 222.f };
		static const float IcicleLength[] = { 34.f, 20.f, 44.f, 16.f, 30.f, 22.f, 40.f, 18.f, 28.f, 36.f };
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(IcicleY); ++Index)
		{
			// (2D, style 5: 3 to 5 icicles per plank, UI_STYLE section 4.)
			if (PinguMenu3D::IsFlat() && Index % 2 == 1)
			{
				continue;
			}
			if (UStaticMeshComponent* Icicle = MakePart(TitleBoard, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::Icicle), IceMaterial))
			{
				Icicle->SetRelativeLocation(FVector(4.f, Planks[2].Y + IcicleY[Index], Under));
				Icicle->SetRelativeScale3D(FVector(IcicleLength[Index] / 100.f));
			}
		}
	}
	// (No ropes since 2026-10-26. The post the signs are nailed to came back on 2026-10-27 - Linus: "add the posts again for the
	// main menue and the lobby but only there": from under the board down out of the picture, snow on its top.)
	if (TitlePost.Build(this, Title.Root, true, false, true))
	{
		constexpr float PostTop = 64.f;
		constexpr float PostBottom = -900.f;
		TitlePost.Root->SetRelativeLocation(FVector(10.f, 0.f, 0.5f * (PostTop + PostBottom)));
		TitlePost.SetShape(36.f, PostTop - PostBottom, 6.f, 26.f);
		TitlePost.SetColors(WoodDark, WoodDarkLip);
		TitlePost.SetWood(1.f, FVector2D(0.5f, 0.2f), true, 100.f, 0.f, 0.f, FVector2D(700.f, 90.f));
		if (UStaticMeshComponent* PostSnow = MakePart(TitlePost.Root, PinguMenu3D::GetPieceMesh(PinguMenu3D::EPiece::SnowCap), SnowMaterial))
		{
			PostSnow->SetRelativeLocation(FVector(0.f, 0.f, PostTop - 2.f - 0.5f * (PostTop + PostBottom)));
			PostSnow->SetRelativeScale3D(FVector(26.f / 100.f, 40.f / 100.f, 16.f / 100.f));
		}
	}

	// The logo: PINGU (snow) over BRAWL (orange), each with a fat dark edge behind it, left-aligned like the old card's, on the board.
	const float Left = -0.5f * TitleWidth + 32.f;
	auto Logo = [this, Left, BoardPivot](const FText& Text, float Cap, float Baseline, const FLinearColor& Front, USceneComponent*& OutText, USceneComponent*& OutInk)
	{
		PinguMenu3D::FTextLook Look;
		Look.Size = Cap;
		Look.Extrude = 0.16f;
		Look.Bevel = 0.045f;
		Look.Align = PinguMenu3D::ETextAlign::Left;
		Look.MaxWidth = TitleWidth - 64.f;
		Look.Front = Front;
		Look.Side = FMath::Lerp(Front, PinguMenu3DPrivate::WoodInk, 0.55f);
		OutText = PinguMenu3D::MakeMenuText(this, TitleBoard, Text, Look);
		PinguMenu3D::FTextLook Ink = Look;
		Ink.Extrude = 0.12f;
		Ink.Outline = 0.07f;
		Ink.Front = PinguMenu3DPrivate::WoodInk;
		Ink.Side = PinguMenu3DPrivate::WoodInk;
		OutInk = PinguMenu3D::MakeMenuText(this, TitleBoard, Text, Ink);
		if (OutText)
		{
			OutText->SetRelativeLocation(FVector(-Look.Extrude * Cap - 4.f, Left, Baseline - BoardPivot));
		}
		if (OutInk)
		{
			// Same scale as the letters (the outline must not shrink them apart), a little further back.
			OutInk->SetRelativeScale3D(OutText ? OutText->GetRelativeScale3D() : OutInk->GetRelativeScale3D());
			OutInk->SetRelativeLocation(FVector(-Ink.Extrude * Cap + 1.f, Left, Baseline - BoardPivot));
		}
	};
	Logo(LOCTEXT("TitlePingu", "PINGU"), 86.f, Top - 36.f - 92.f, Style.Snow, LogoPingu, LogoPinguInk);
	Logo(LOCTEXT("TitleBrawl", "BRAWL"), 60.f, Top - 36.f - 92.f - 16.f - 64.f, Style.Orange, LogoBrawl, LogoBrawlInk);

	const float ButtonWidth = TitleWidth - 64.f;
	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	auto Act = [WeakHUD](void (APenguinHUD::*Action)())
	{
		return [WeakHUD, Action]() { if (APenguinHUD* PenguinHUD = WeakHUD.Get()) { (PenguinHUD->*Action)(); } };
	};
	// The signs point right / left / right ... as in the mockup.
	NextSignPoint = 1;
	AddButton(Title, TEXT("Play"), EPinguMenu3DKind::Primary, FVector2D(0.f, 41.f), FVector2D(ButtonWidth, 76.f), 28.f,
		[] { return LOCTEXT("Play", "PLAY"); }, Act(&APenguinHUD::Action_Play));
	NextSignPoint = -1;
	AddButton(Title, TEXT("Online"), EPinguMenu3DKind::Normal, FVector2D(0.f, -43.f), FVector2D(ButtonWidth, 60.f), 22.f,
		[] { return LOCTEXT("Online", "ONLINE"); }, Act(&APenguinHUD::Action_Lan));
	NextSignPoint = 1;
	AddButton(Title, TEXT("Settings"), EPinguMenu3DKind::Normal, FVector2D(0.f, -117.f), FVector2D(ButtonWidth, 60.f), 22.f,
		[] { return LOCTEXT("Settings", "SETTINGS"); }, Act(&APenguinHUD::Action_Settings));
	NextSignPoint = -1;
	AddButton(Title, TEXT("Pingupedia"), EPinguMenu3DKind::Normal, FVector2D(0.f, -191.f), FVector2D(ButtonWidth, 60.f), 22.f,
		[] { return LOCTEXT("Pingupedia", "PINGUPEDIA"); }, Act(&APenguinHUD::Action_Pingupedia));
	NextSignPoint = 1;
	AddButton(Title, TEXT("Quit"), EPinguMenu3DKind::Ghost, FVector2D(0.f, -266.f), FVector2D(ButtonWidth, 46.f), 16.f,
		[] { return LOCTEXT("Quit", "QUIT"); }, Act(&APenguinHUD::Action_Quit));

	// The PINGUPEDIA label's size for the menu screenshots' check (the Slate title measured its label the same way).
	if (USceneComponent* PediaLabel = Title.Buttons[3].Text)
	{
		PinguUI::FPingupediaStats& PediaStats = PinguUI::MutablePingupediaStats();
		PediaStats.LabelWidth = PinguMenu3D::GetTextWidth(PediaLabel);
		PediaStats.LabelRoom = ButtonWidth - 32.f;
		PediaStats.LabelFont = Title.Buttons[3].LabelSize;
	}
}

void APinguMenu3D::BuildLobby()
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	Lobby.Root = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	Lobby.Root->SetupAttachment(MenuRoot);
	Lobby.Root->RegisterComponent();
	// (The plane only keeps the strip's size - its place at the bottom: the strip has no backing since 2026-10-26, the signs
	// stand on their own posts like the mockup's.)
	Lobby.bNoBoard = true;
	Lobby.Plane.Build(this, Lobby.Root);
	Lobby.Plane.SetShape(StripInner + 2.f * StripPad, 108.f, PlaneRadius, PlaneDepth);
	Lobby.Plane.SetVisible(false);

	TWeakObjectPtr<APenguinHUD> WeakHUD = HUD;
	auto Authority = [WeakHUD]() { const APenguinHUD* H = WeakHUD.Get(); return H && H->HasMatchAuthority(); };
	auto Network = [WeakHUD]() { const APenguinHUD* H = WeakHUD.Get(); return H && H->IsNetworkGame(); };
	const float SmallW = 184.f;
	const float SmallH = 44.f;
	const float Left = -0.5f * StripInner;
	const float Right = 0.5f * StripInner;
	AddressLeft = Left;

	// (BACK / LEAVE at the left end and SETTINGS next to it since 2026-10-29 - Linus: "switch back and settings position".)
	AddButton(Lobby, TEXT("Settings"), EPinguMenu3DKind::Normal, FVector2D(Left + SmallW + 24.f + 0.5f * SmallW, 0.f), FVector2D(SmallW, SmallH), 15.f,
		[] { return LOCTEXT("LobbySettings", "SETTINGS"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_Settings(); } });
	NextSignPoint = -1;
	FButton& Back = AddButton(Lobby, TEXT("Back"), EPinguMenu3DKind::Ghost, FVector2D(Left + 0.5f * SmallW, 0.f), FVector2D(SmallW, SmallH), 15.f,
		[] { return LOCTEXT("LobbyBack", "BACK"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_MainMenu(); } });
	Back.Visible = [Network] { return !Network(); };
	NextSignPoint = -1;
	FButton& Leave = AddButton(Lobby, TEXT("Leave"), EPinguMenu3DKind::Danger, Back.Center, FVector2D(SmallW, SmallH), 15.f,
		[Authority] { return Authority() ? LOCTEXT("CloseGame", "CLOSE GAME") : LOCTEXT("LeaveLobby", "LEAVE"); },
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_LeaveGame(); } });
	Leave.Visible = Network;
	const float StartW = 240.f;
	NextSignPoint = 1;
	FButton& Start = AddButton(Lobby, TEXT("Start"), EPinguMenu3DKind::Primary, FVector2D(Right - 0.5f * StartW, 0.f), FVector2D(StartW, 64.f), 22.f,
		[] { return LOCTEXT("StartMatch", "START"); }, [WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_StartLobbyMatch(); } });
	Start.Visible = Authority;
	FButton& Traits = AddButton(Lobby, TEXT("Traits"), EPinguMenu3DKind::Toggle, FVector2D(Right - StartW - 28.f - 0.5f * SmallW, 0.f), FVector2D(SmallW, SmallH), 15.f,
		[WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->AreTraitsOn() ? LOCTEXT("LobbyTraitsOn", "TRAITS ON") : LOCTEXT("LobbyTraitsOff", "TRAITS OFF"); },
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_ToggleTraits(); } });
	Traits.Visible = Authority;
	Traits.Selected = [WeakHUD] { const APenguinHUD* H = WeakHUD.Get(); return H && H->AreTraitsOn(); };

	// The host's address line: "Others join with <address>" and a copy icon; a click copies the address (COPIED! for a moment).
	FButton& Address = AddButton(Lobby, TEXT("Address"), EPinguMenu3DKind::Ghost, FVector2D(Left + 210.f, -60.f), FVector2D(420.f, 40.f), 14.f,
		[WeakHUD]
		{
			const APenguinHUD* H = WeakHUD.Get();
			if (H && H->IsHostAddressCopied())
			{
				return FText::Format(LOCTEXT("OthersJoinCopied", "Others join with  {0}"), LOCTEXT("AddressCopied", "COPIED!"));
			}
			return FText::Format(LOCTEXT("OthersJoinAddress", "Others join with  {0}"), FText::FromString(H ? H->GetHostAddressText() : FString()));
		},
		[WeakHUD] { if (APenguinHUD* H = WeakHUD.Get()) { H->Action_CopyHostAddress(); } });
	Address.Visible = [Authority, Network] { return Authority() && Network(); };
	Address.bInstant = true;
	CopyIcon = NewObject<USceneComponent>(this, NAME_None, RF_Transient);
	CopyIcon->SetupAttachment(Address.Body);
	CopyIcon->RegisterComponent();
	CopyBack.Build(this, CopyIcon);
	CopyBack.SetShape(13.f, 15.f, 3.f, 4.f);
	CopyBack.Root->SetRelativeLocation(FVector(-2.f, -3.f, 3.f));
	CopyFront.Build(this, CopyIcon);
	CopyFront.SetShape(13.f, 15.f, 3.f, 4.f);
	CopyFront.Root->SetRelativeLocation(FVector(-5.f, 3.f, -3.f));

	// A LAN client: the host's TRAITS choice to see (no button), and "Waiting for <host>..." where START is.
	TraitsDisplay.Build(this, Lobby.Root, true, false, true);
	TraitsDisplay.SetShape(SmallW, SmallH, PinguMenu3D::SignRadiusShare * SmallH, ButtonProtrude + 6.f);
	TraitsDisplay.SetWood(1.f, FVector2D(0.4f, 0.7f), false, 100.f, 0.f, 0.f, FVector2D(300.f, 120.f));
	// Every sign of the strip stands on its own post again (2026-10-27; down out of the picture, behind the sign; the address
	// line is nailed across the posts of SETTINGS and BACK).
	auto AddPost = [this](PinguMenu3D::FSlab& Post, USceneComponent* Parent, float BackX)
	{
		if (Post.Build(this, Parent, true, false, true))
		{
			constexpr float PostLength = 420.f;
			Post.Root->SetRelativeLocation(FVector(BackX, 0.f, -0.5f * PostLength + 6.f));
			Post.SetShape(22.f, PostLength, 4.f, 18.f);
			Post.SetColors(PinguMenu3DPrivate::WoodDark, PinguMenu3DPrivate::WoodDarkLip);
			Post.SetWood(1.f, FVector2D(0.2f, 0.5f), true, 100.f, 0.f, 0.f, FVector2D(600.f, 80.f));
		}
	};
	for (FButton& Button : Lobby.Buttons)
	{
		if (Button.Id != FName(TEXT("Address")))
		{
			AddPost(Button.Post, Button.Root, 2.f);
		}
	}
	AddPost(TraitsDisplayPost, TraitsDisplay.Root, ButtonProtrude + 2.f);
	TraitsDisplay.Root->SetRelativeLocation(FVector(-ButtonProtrude, Traits.Center.X, Traits.Center.Y));
	PinguMenu3D::FTextLook DisplayLook;
	DisplayLook.Size = 15.f;
	DisplayLook.Extrude = 0.22f;
	DisplayLook.MaxWidth = SmallW - 28.f;
	DisplayLook.Front = Style.Ice;
	DisplayLook.Side = Style.Ink;
	TraitsDisplayText = PinguMenu3D::MakeMenuText(this, TraitsDisplay.Root, LOCTEXT("LobbyTraitsOff", "TRAITS OFF"), DisplayLook);
	if (TraitsDisplayText)
	{
		TraitsDisplayText->SetRelativeLocation(FVector(-DisplayLook.Extrude * DisplayLook.Size + 0.5f, 0.f, -0.5f * DisplayLook.Size));
	}
	PinguMenu3D::FTextLook WaitLook;
	// In START's place (its width: a long host name makes it smaller, never over TRAITS).
	WaitLook.Size = 16.f;
	WaitLook.Extrude = 0.2f;
	WaitLook.Align = PinguMenu3D::ETextAlign::Center;
	WaitLook.MaxWidth = StartW - 12.f;
	WaitLook.Front = Style.Gold;
	WaitLook.Side = FMath::Lerp(Style.Gold, Style.Ink, 0.6f);
	WaitingText = PinguMenu3D::MakeMenuText(this, Lobby.Root, FText::GetEmpty(), WaitLook);
}

APinguMenu3D::FPageParts* APinguMenu3D::GetParts(EPinguMenu3DPage InPage)
{
	switch (InPage)
	{
	case EPinguMenu3DPage::Title: return &Title;
	case EPinguMenu3DPage::Lobby: return &Lobby;
	case EPinguMenu3DPage::Pause: return &Pause;
	case EPinguMenu3DPage::Lan: return &Lan;
	case EPinguMenu3DPage::Results: return &Results;
	case EPinguMenu3DPage::Settings: return &Settings;
	case EPinguMenu3DPage::Controls: return &Controls;
	case EPinguMenu3DPage::Pingupedia: return &Pingupedia;
	case EPinguMenu3DPage::Board: return &Board;
	case EPinguMenu3DPage::Capture: return &Capture;
	case EPinguMenu3DPage::Error: return &Error;
	case EPinguMenu3DPage::Confirm: return &Confirm;
	case EPinguMenu3DPage::TraitPicks: return &TraitPicks;
	default: return nullptr;
	}
}

const APinguMenu3D::FPageParts* APinguMenu3D::GetParts(EPinguMenu3DPage InPage) const
{
	return const_cast<APinguMenu3D*>(this)->GetParts(InPage);
}

APinguMenu3D::FPageParts* APinguMenu3D::GetInputParts()
{
	// An overlay (PRESS A KEY, the error popup) takes the input while it shows.
	FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	return OverlayParts && OverlayParts->bShown ? OverlayParts : GetParts(Page);
}

const APinguMenu3D::FPageParts* APinguMenu3D::GetInputParts() const
{
	return const_cast<APinguMenu3D*>(this)->GetInputParts();
}

EPinguMenu3DPage APinguMenu3D::PageOfScreen(EPinguScreen Screen)
{
	switch (Screen)
	{
	case EPinguScreen::MainMenu: return EPinguMenu3DPage::Title;
	case EPinguScreen::Lobby: return EPinguMenu3DPage::Lobby;
	case EPinguScreen::Pause: return EPinguMenu3DPage::Pause;
	case EPinguScreen::Lan: return EPinguMenu3DPage::Lan;
	case EPinguScreen::Results: return EPinguMenu3DPage::Results;
	case EPinguScreen::Settings: return EPinguMenu3DPage::Settings;
	case EPinguScreen::Controls: return EPinguMenu3DPage::Controls;
	case EPinguScreen::Pingupedia: return EPinguMenu3DPage::Pingupedia;
	case EPinguScreen::TraitPicks: return EPinguMenu3DPage::TraitPicks;
	default: return EPinguMenu3DPage::None;
	}
}

const TCHAR* APinguMenu3D::PageName(EPinguMenu3DPage InPage)
{
	switch (InPage)
	{
	case EPinguMenu3DPage::Title: return TEXT("title");
	case EPinguMenu3DPage::Lobby: return TEXT("lobby");
	case EPinguMenu3DPage::Pause: return TEXT("pause");
	case EPinguMenu3DPage::Lan: return TEXT("lan");
	case EPinguMenu3DPage::Results: return TEXT("results");
	case EPinguMenu3DPage::Settings: return TEXT("settings");
	case EPinguMenu3DPage::Controls: return TEXT("controls");
	case EPinguMenu3DPage::Pingupedia: return TEXT("pingupedia");
	case EPinguMenu3DPage::Board: return TEXT("board");
	case EPinguMenu3DPage::Capture: return TEXT("capture");
	case EPinguMenu3DPage::Error: return TEXT("error");
	case EPinguMenu3DPage::Confirm: return TEXT("confirm");
	case EPinguMenu3DPage::TraitPicks: return TEXT("traitpicks");
	default: return TEXT("none");
	}
}

EPinguMenu3DPage APinguMenu3D::ComputePage() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return EPinguMenu3DPage::None;
	}
	const EPinguMenu3DPage OfScreen = PageOfScreen(PenguinHUD->GetScreen());
	if (OfScreen != EPinguMenu3DPage::None)
	{
		return OfScreen;
	}
	// No screen: the round board between rounds (the HUD's leaderboard) once its delay is over.
	if (PenguinHUD->GetScreen() == EPinguScreen::None && PenguinHUD->GetPhase() == EPinguPhase::RoundOver && PenguinHUD->GetGameState()
		&& PenguinHUD->GetPhaseTime() >= APenguinGameState::LeaderboardDelay)
	{
		return EPinguMenu3DPage::Board;
	}
	return EPinguMenu3DPage::None;
}

EPinguMenu3DPage APinguMenu3D::ComputeOverlay() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || TestPage != EPinguMenu3DPage::None)
	{
		return EPinguMenu3DPage::None;
	}
	if (PenguinHUD->IsCapturingKey())
	{
		return EPinguMenu3DPage::Capture;
	}
	if (PenguinHUD->HasErrorPopup())
	{
		return EPinguMenu3DPage::Error;
	}
	if (PenguinHUD->HasSettingsConfirm() && Page == EPinguMenu3DPage::Settings)
	{
		return EPinguMenu3DPage::Confirm;
	}
	return EPinguMenu3DPage::None;
}

void APinguMenu3D::ShowPage(EPinguMenu3DPage NewPage)
{
	// (The card lobby hidden by Esc - the player runs about on the island - keeps its hot button for its return; any other way
	// out of the lobby forgets it.)
	if (Page == EPinguMenu3DPage::Lobby && bCardLobby)
	{
		LobbyLastHot = NewPage == EPinguMenu3DPage::None ? GetHotId() : NAME_None;
	}
	else if (NewPage != EPinguMenu3DPage::Lobby)
	{
		LobbyLastHot = NAME_None;
	}
	if (FPageParts* Old = GetParts(Page))
	{
		Old->Root->SetVisibility(false, true);
		Old->bShown = false;
	}
	if (Page == EPinguMenu3DPage::Lobby && NewPage != EPinguMenu3DPage::Lobby)
	{
		// The lobby strip's copy icon is built from its own slabs: hide them with the page (a stray blue square stayed on
		// the other pages, bottom middle).
		if (CopyIcon)
		{
			CopyIcon->SetVisibility(false, false);
		}
		CopyBack.SetVisible(false);
		CopyFront.SetVisible(false);
	}
	Page = NewPage;
	PendingPress = INDEX_NONE;
	DragButton = INDEX_NONE;
	++FlatVersion;
	if (FPageParts* Parts = GetParts(Page))
	{
		EnsureBuilt(*Parts);
		Parts->Appear = 0.f;
		Parts->OpenT = 0.f;
		Parts->bHotByMouse = false;
		Parts->bShown = true;
		Parts->Root->SetVisibility(true, false);
		if (Page == EPinguMenu3DPage::Title)
		{
			// The hanging board, its ropes, icicles and the post: everything under the title shows (the buttons and the hidden
			// plane are put right just below).
			Parts->Root->SetVisibility(true, true);
		}
		ShowBoard(*Parts);
		for (FButton& Button : Parts->Buttons)
		{
			Button.Hot = 0.f;
			Button.Focus = 0.f;
			Button.Hover = 0.f;
			Button.HoverT = 10.f;
			Button.PushT = 10.f;
			SetButtonVisible(Button, !Button.Visible || Button.Visible());
		}
		if (LogoPingu && Page == EPinguMenu3DPage::Title)
		{
			for (USceneComponent* Logo : { LogoPingu, LogoBrawl, LogoPinguInk, LogoBrawlInk })
			{
				if (Logo)
				{
					Logo->SetVisibility(true, true);
				}
			}
		}
		if (Page == EPinguMenu3DPage::Lobby && !bCardLobby)
		{
			LobbyLayoutKey = -1;
			UpdateLobbyLayout(*Parts);
		}
		if (Page == EPinguMenu3DPage::Lobby && bCardLobby)
		{
			// (The cards are made again: everything on them shows - the page's hiding reached their own parts.)
			Parts->RowsKey.Reset();
		}
		if (Page == EPinguMenu3DPage::Pingupedia)
		{
			// A fresh visit builds the card again (the penguins' movement numbers may not have been there yet).
			PediaShownPage = -1;
			PediaShownEntry = -1;
		}
		// The root was hidden with everything under it: the labels and decos are shown again by their rules.
		for (FLabel& Label : Parts->Labels)
		{
			Label.bShown = !(!Label.Visible || Label.Visible());
		}
		for (FDeco& Deco : Parts->Decos)
		{
			Deco.bShown = !(!Deco.Visible || Deco.Visible());
		}
		UpdateLabels(*Parts);
		UpdateDecos(*Parts);
		if (Parts->ListWindow > 0)
		{
			ApplyListWindow(*Parts);
		}
		UpdateStack(*Parts);
		UpdatePage(*Parts, 0.f);
		Parts->Hot = INDEX_NONE;
		Parts->Hot = GetDefaultFocus();
		// The card lobby back after Esc (its run on the island): the focus where it was.
		if (Page == EPinguMenu3DPage::Lobby && bCardLobby && !LobbyLastHot.IsNone())
		{
			FocusButton(LobbyLastHot);
			LobbyLastHot = NAME_None;
		}
		UE_LOG(LogPingu, Log, TEXT("[MENU3D] page %s shows (%d buttons)"), PageName(Page), Parts->Buttons.Num());
	}
}

void APinguMenu3D::ShowBoard(FPageParts& Parts)
{
	const bool bBoard = Parts.Plane.IsBuilt() && !Parts.bNoBoard;
	Parts.Plane.SetVisible(false);
	Parts.Backing.SetVisible(bBoard);
	for (int32 Index = 0; Index < Parts.Planks.Num(); ++Index)
	{
		const bool bPlank = bBoard && Index < Parts.PlankCount;
		Parts.Planks[Index].SetVisible(bPlank);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (Parts.PlankNails.IsValidIndex(Index * 2 + Side) && Parts.PlankNails[Index * 2 + Side])
			{
				Parts.PlankNails[Index * 2 + Side]->SetVisibility(bPlank);
			}
		}
	}
	if (Parts.FrameSnow)
	{
		Parts.FrameSnow->SetVisibility(bBoard);
	}
}

void APinguMenu3D::ShowOverlay(EPinguMenu3DPage NewOverlay)
{
	const EPinguMenu3DPage OldOverlay = Overlay;
	if (FPageParts* Old = GetParts(Overlay))
	{
		Old->Root->SetVisibility(false, true);
		Old->bShown = false;
	}
	// The "Apply changes?" board stands alone (2026-11-07): the settings page behind it is hidden (a small board over the stacked
	// planks of a tall page looks like a mess) and comes back, as it was, when the board closes.
	if (FPageParts* Behind = Page != EPinguMenu3DPage::None ? GetParts(Page) : nullptr)
	{
		if (NewOverlay == EPinguMenu3DPage::Confirm && Behind->bShown)
		{
			Behind->Root->SetVisibility(false, true);
			Behind->bShown = false;
			++FlatVersion;
		}
		else if (OldOverlay == EPinguMenu3DPage::Confirm && NewOverlay != EPinguMenu3DPage::Confirm && !Behind->bShown && Behind->Root)
		{
			Behind->bShown = true;
			Behind->Root->SetVisibility(true, false);
			ShowBoard(*Behind);
			for (FButton& Button : Behind->Buttons)
			{
				SetButtonVisible(Button, !Button.Visible || Button.Visible());
			}
			for (FLabel& Label : Behind->Labels)
			{
				Label.bShown = !(!Label.Visible || Label.Visible());
			}
			for (FDeco& Deco : Behind->Decos)
			{
				Deco.bShown = !(!Deco.Visible || Deco.Visible());
			}
			UpdateLabels(*Behind);
			UpdateDecos(*Behind);
			UpdateStack(*Behind);
			++FlatVersion;
		}
	}
	Overlay = NewOverlay;
	PendingPress = INDEX_NONE;
	DragButton = INDEX_NONE;
	++FlatVersion;
	if (FPageParts* Parts = GetParts(Overlay))
	{
		EnsureBuilt(*Parts);
		Parts->Appear = 0.f;
		Parts->OpenT = 0.f;
		Parts->bHotByMouse = false;
		Parts->bShown = true;
		Parts->Root->SetVisibility(true, false);
		ShowBoard(*Parts);
		for (FButton& Button : Parts->Buttons)
		{
			Button.Hot = 0.f;
			Button.Focus = 0.f;
			Button.Hover = 0.f;
			Button.HoverT = 10.f;
			Button.PushT = 10.f;
			SetButtonVisible(Button, !Button.Visible || Button.Visible());
		}
		for (FLabel& Label : Parts->Labels)
		{
			Label.bShown = !(!Label.Visible || Label.Visible());
		}
		for (FDeco& Deco : Parts->Decos)
		{
			Deco.bShown = !(!Deco.Visible || Deco.Visible());
		}
		UpdateLabels(*Parts);
		UpdateDecos(*Parts);
		UpdatePage(*Parts, 0.f);
		Parts->Hot = INDEX_NONE;
		Parts->Hot = GetDefaultFocus();
	}
}

int32 APinguMenu3D::GetDefaultFocus() const
{
	const FPageParts* Parts = GetInputParts();
	if (!Parts || IsMouseOnlyPage())
	{
		return INDEX_NONE;
	}
	const APenguinHUD* PenguinHUD = HUD.Get();
	// The title: PLAY. The lobby: START (the host / solo), else SETTINGS. The rest: as the Slate screens (the LAN screen's BACK
	// while joining, the results' LEAVE on a client, the settings page's first control, the PINGUPEDIA's selected row).
	FName Want = Parts->DefaultFocus;
	switch (Parts->Id)
	{
	case EPinguMenu3DPage::Title: Want = TEXT("Play"); break;
	case EPinguMenu3DPage::Lobby: Want = !PenguinHUD || PenguinHUD->HasMatchAuthority() ? TEXT("Start") : TEXT("Settings"); break;
	case EPinguMenu3DPage::Lan: Want = PenguinHUD && PenguinHUD->IsConnecting() ? TEXT("Back") : TEXT("Host"); break;
	case EPinguMenu3DPage::Results: Want = PenguinHUD && !PenguinHUD->HasMatchAuthority() ? TEXT("Leave") : TEXT("Rematch"); break;
	case EPinguMenu3DPage::Settings: Want = SettingsTab == 1 ? TEXT("WindowMode") : (SettingsTab == 2 ? TEXT("Master") : TEXT("Preset0")); break;
	case EPinguMenu3DPage::Pingupedia: Want = FName(*FString::Printf(TEXT("Entry%d_%d"), PediaPage, PediaEntry)); break;
	default: break;
	}
	int32 First = INDEX_NONE;
	for (int32 Index = 0; Index < Parts->Buttons.Num(); ++Index)
	{
		if (!IsButtonLive(Parts->Buttons[Index]) || !Parts->Buttons[Index].bFocusable)
		{
			continue;
		}
		if (Parts->Buttons[Index].Id == Want)
		{
			return Index;
		}
		if (First == INDEX_NONE)
		{
			First = Index;
		}
	}
	return First;
}

bool APinguMenu3D::IsButtonLive(const FButton& Button) const
{
	return Button.bShown && Button.Root && (!Button.Enabled || Button.Enabled());
}

void APinguMenu3D::SetTestPage(EPinguMenu3DPage InPage, TFunction<void(FName)> OnAction)
{
	TestPage = InPage;
	TestAction = MoveTemp(OnAction);
}

bool APinguMenu3D::GetView(FVector& OutOrigin, FRotator& OutRotation, float& OutTanX, float& OutTanY) const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	const APlayerController* Controller = PenguinHUD ? PenguinHUD->PlayerOwner.Get() : nullptr;
	if (!Controller)
	{
		if (const UWorld* World = GetWorld())
		{
			Controller = World->GetFirstPlayerController();
		}
	}
	if (!Controller || !Controller->PlayerCameraManager)
	{
		return false;
	}
	const FMinimalViewInfo& View = Controller->PlayerCameraManager->GetCameraCacheView();
	int32 SizeX = 0;
	int32 SizeY = 0;
	Controller->GetViewportSize(SizeX, SizeY);
	const float Aspect = SizeX > 0 && SizeY > 0 ? static_cast<float>(SizeX) / static_cast<float>(SizeY) : 16.f / 9.f;
	OutOrigin = View.Location;
	OutRotation = View.Rotation;
	// The horizontal field of view is kept (the engine's default), the vertical one follows the picture's shape.
	OutTanX = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(View.FOV, 5.f, 170.f) * 0.5f));
	OutTanY = OutTanX / FMath::Max(Aspect, 0.1f);
	return true;
}

APlayerController* APinguMenu3D::GetController() const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	APlayerController* Controller = PenguinHUD ? PenguinHUD->PlayerOwner.Get() : nullptr;
	if (!Controller)
	{
		if (const UWorld* World = GetWorld())
		{
			Controller = World->GetFirstPlayerController();
		}
	}
	return Controller;
}

bool APinguMenu3D::ProjectToPixel(const FVector& World, FVector2D& OutPixel) const
{
	if (PinguMenu3D::IsFlat())
	{
		// The 2D menus: the menu's own camera (the same projection the 3D menu had in front of the real one).
		FVector Origin;
		FRotator Rotation;
		float TanX = 0.f;
		float TanY = 0.f;
		FVector2D Size;
		if (!GetFlatView(Origin, Rotation, TanX, TanY, Size))
		{
			return false;
		}
		const FVector Local = Rotation.UnrotateVector(World - Origin);
		if (Local.X < 1.0)
		{
			return false;
		}
		OutPixel = FVector2D(0.5 * Size.X * (1.0 + Local.Y / (Local.X * TanX)), 0.5 * Size.Y * (1.0 - Local.Z / (Local.X * TanY)));
		return true;
	}
	return ProjectWorldToPixel(World, OutPixel);
}

bool APinguMenu3D::ProjectWorldToPixel(const FVector& World, FVector2D& OutPixel) const
{
	APlayerController* Controller = GetController();
	return Controller && Controller->ProjectWorldLocationToScreen(World, OutPixel, false);
}

bool APinguMenu3D::DeprojectPixel(const FVector2D& ViewportPixel, FVector& OutOrigin, FVector& OutDirection) const
{
	if (PinguMenu3D::IsFlat())
	{
		FRotator Rotation;
		float TanX = 0.f;
		float TanY = 0.f;
		FVector2D Size;
		if (!GetFlatView(OutOrigin, Rotation, TanX, TanY, Size) || Size.X < 1.0 || Size.Y < 1.0)
		{
			return false;
		}
		const FVector Local(1.0, (2.0 * ViewportPixel.X / Size.X - 1.0) * TanX, (1.0 - 2.0 * ViewportPixel.Y / Size.Y) * TanY);
		OutDirection = Rotation.RotateVector(Local).GetSafeNormal();
		return true;
	}
	APlayerController* Controller = GetController();
	return Controller && Controller->DeprojectScreenPositionToWorld(ViewportPixel.X, ViewportPixel.Y, OutOrigin, OutDirection);
}

bool APinguMenu3D::GetFlatView(FVector& OutOrigin, FRotator& OutRotation, float& OutTanX, float& OutTanY, FVector2D& OutSize) const
{
	FVector CameraOrigin;
	FRotator CameraRotation;
	if (!GetView(CameraOrigin, CameraRotation, OutTanX, OutTanY) || !PinguMenu3D::GetViewportPixelSize(GetController(), OutSize))
	{
		return false;
	}
	// The menu's camera stands MenuDistance in front of its pages, looking along the actor's forward (in 3D: the real camera).
	OutRotation = GetActorRotation();
	OutOrigin = GetActorLocation() - OutRotation.Vector() * MenuDistance;
	return true;
}

void APinguMenu3D::SetHiddenForTest(bool bHide)
{
	bHiddenForTest = bHide;
	if (!PinguMenu3D::IsFlat())
	{
		SetActorHiddenInGame(bHide);
	}
}

const USceneComponent* APinguMenu3D::GetOverlayRootComponent() const
{
	const FPageParts* Parts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	return Parts && Parts->bShown ? Parts->Root : nullptr;
}

void APinguMenu3D::CountParts(int32& OutPrimitives, int32& OutFlatParts) const
{
	OutPrimitives = 0;
	OutFlatParts = 0;
	TInlineComponentArray<UPrimitiveComponent*> Primitives(this);
	for (const UPrimitiveComponent* Primitive : Primitives)
	{
		if (Cast<UPinguMenu2DPart>(Primitive))
		{
			++OutFlatParts;
		}
		else if (Primitive && Primitive->IsRegistered())
		{
			++OutPrimitives;
		}
	}
}

void APinguMenu3D::PlaceOnCamera()
{
	using namespace PinguMenu3DPrivate;
	bViewValid = GetView(ViewOrigin, ViewRotation, ViewTanX, ViewTanY);
	if (!bViewValid)
	{
		return;
	}
	// One page unit = one pixel of a DesignWidth wide picture, on the plane MenuDistance away.
	const float UnitsPerPixel = 2.f * MenuDistance * ViewTanX / DesignWidth;
	const bool bFlat = PinguMenu3D::IsFlat();
	if (bFlat)
	{
		// The 2D menus: the actor stands still (its own camera, GetFlatView); only a new field of view rescales it.
		if (!GetActorLocation().Equals(FlatPlace, 0.01))
		{
			SetActorLocationAndRotation(FlatPlace, FRotator::ZeroRotator);
		}
		if (!FMath::IsNearlyEqual(UnitsPerPixel, FlatUnitsPerPixel, 1.e-6f))
		{
			FlatUnitsPerPixel = UnitsPerPixel;
			MenuRoot->SetRelativeScale3D(FVector(UnitsPerPixel));
		}
	}
	else
	{
		SetActorLocationAndRotation(ViewOrigin + ViewRotation.Vector() * MenuDistance, ViewRotation);
		MenuRoot->SetRelativeScale3D(FVector(UnitsPerPixel));
	}
	const float DesignHeight = DesignWidth * ViewTanY / ViewTanX;

	// Where the pages stand (the title at the left like the old card; the lobby's strip at the bottom), sliding in as they come.
	const float TitleIn = EaseOut(Title.Appear);
	// (The 2D menus move only the page that shows: a hidden page's parts are not touched every frame.)
	if (!bFlat || Title.bShown)
	{
		Title.Root->SetRelativeLocation(FVector(0.f, -0.5f * DesignWidth + TitleMargin + 0.5f * TitleWidth - (1.f - TitleIn) * 720.f, 0.f));
		const float Sway = 2.f * PI * Clock / SwayPeriod;
		// The sway and the parallax (the card turns a little toward the cursor): the depth reads as the picture shifts.
		Title.Root->SetRelativeRotation(FRotator(SwayPitch * FMath::Sin(0.7f * Sway) - ParallaxPitch * Parallax.Y,
			TitleYaw - (1.f - TitleIn) * 25.f + SwayYaw * FMath::Sin(Sway) + ParallaxYaw * Parallax.X, 0.f));
		if (TitleBoard)
		{
			// The hanging board swings a little on its ropes (its pivot is where the ropes leave the picture).
			TitleBoard->SetRelativeRotation(FRotator(0.f, 0.f, BoardSwing * FMath::Sin(2.f * PI * Clock / BoardSwingPeriod + 0.6f)));
		}
	}
	const float LobbyIn = EaseOut(Lobby.Appear);
	const float StripHeight = Lobby.Plane.Height;
	// (The strip's signs 1.25x since 2026-10-26: Linus "make it so the buttons in the lobby are a bit bigger"; its bottom stays put.)
	if (bCardLobby)
	{
		// (The card lobby is a page in the middle like the others.)
		if (Lobby.bBuilt && Lobby.Root && (!bFlat || Lobby.bShown))
		{
			PlacePage(Lobby, DesignHeight);
		}
	}
	else if (!bFlat || Lobby.bShown)
	{
		Lobby.Root->SetRelativeScale3D(FVector(LobbyScale));
		Lobby.Root->SetRelativeLocation(FVector(0.f, 0.f, -0.5f * DesignHeight + StripBottom + 0.5f * LobbyScale * StripHeight - (1.f - LobbyIn) * 260.f));
		Lobby.Root->SetRelativeRotation(FRotator(StripPitch - 0.5f * ParallaxPitch * Parallax.Y, 0.3f * ParallaxYaw * Parallax.X, 0.f));
	}
	// The other pages stand in the middle of the picture (the overlays a little nearer the camera).
	for (FPageParts* Parts : { &Pause, &Lan, &Results, &Settings, &Controls, &Pingupedia, &Board, &Capture, &Error, &Confirm })
	{
		if (Parts->bBuilt && Parts->Root && (!bFlat || Parts->bShown))
		{
			PlacePage(*Parts, DesignHeight);
		}
	}
}

void APinguMenu3D::PlacePage(FPageParts& Parts, float DesignHeight)
{
	using namespace PinguMenu3DPrivate;
	const float In = EaseOut(Parts.Appear);
	const bool bOverlay = Parts.Id == EPinguMenu3DPage::Capture || Parts.Id == EPinguMenu3DPage::Error || Parts.Id == EPinguMenu3DPage::Confirm;
	// Slides up into place as it comes (and the overlays come from nearer the camera); the parallax turns it a little toward
	// the cursor, half as much as the title card.
	Parts.Root->SetRelativeLocation(FVector((bOverlay ? -OverlayLift : 0.f) - (1.f - In) * (bOverlay ? 40.f : 0.f), Parts.PlaceOffset.X,
		Parts.PlaceOffset.Y - (1.f - In) * 90.f));
	Parts.Root->SetRelativeRotation(FRotator(Parts.Pitch - PageParallaxShare * ParallaxPitch * Parallax.Y, Parts.Yaw + PageParallaxShare * ParallaxYaw * Parallax.X, 0.f));
	(void)DesignHeight;
}

void APinguMenu3D::UpdateLobbyLayout(FPageParts& Parts)
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	const APenguinHUD* PenguinHUD = HUD.Get();
	const bool bAuthority = PenguinHUD && PenguinHUD->HasMatchAuthority();
	const bool bNetwork = PenguinHUD && PenguinHUD->IsNetworkGame();
	const bool bAddress = bAuthority && bNetwork;
	const int32 Key = (bAuthority ? 1 : 0) | (bNetwork ? 2 : 0);
	if (Key != LobbyLayoutKey)
	{
		LobbyLayoutKey = Key;
		// With the address line the strip is taller: the row at its top, the line under it.
		const float Height = bAddress ? 22.f + 64.f + 10.f + 40.f + 18.f : 108.f;
		Parts.Plane.SetShape(StripInner + 2.f * StripPad, Height, PlaneRadius, PlaneDepth);
		const float RowZ = bAddress ? 0.5f * Height - 22.f - 32.f : 0.f;
		const float AddressZ = RowZ - 32.f - 10.f - 20.f;
		for (FButton& Button : Parts.Buttons)
		{
			Button.Center.Y = Button.Id == FName(TEXT("Address")) ? AddressZ : RowZ;
			Button.Root->SetRelativeLocation(FVector(0.f, Button.Center.X, Button.Center.Y));
		}
		if (TraitsDisplay.Root)
		{
			const FVector Old = TraitsDisplay.Root->GetRelativeLocation();
			TraitsDisplay.Root->SetRelativeLocation(FVector(Old.X, Old.Y, RowZ));
		}
		if (WaitingText)
		{
			WaitingText->SetRelativeLocation(FVector(-4.f, 0.5f * StripInner - 120.f, RowZ - 8.f));
		}
	}

	// The client's TRAITS display and "Waiting for <host>...".
	const bool bClient = !bAuthority;
	TraitsDisplay.SetVisible(Parts.bShown && bClient);
	TraitsDisplayPost.SetVisible(Parts.bShown && bClient);
	if (TraitsDisplayText)
	{
		TraitsDisplayText->SetVisibility(Parts.bShown && bClient, true);
		const bool bOn = PenguinHUD && PenguinHUD->AreTraitsOn();
		const FText Label = bOn ? LOCTEXT("LobbyTraitsOn", "TRAITS ON") : LOCTEXT("LobbyTraitsOff", "TRAITS OFF");
		if (Label.ToString() != TraitsDisplayShown)
		{
			TraitsDisplayShown = Label.ToString();
			PinguMenu3D::SetText(TraitsDisplayText, Label);
		}
		TraitsDisplay.SetColors(bOn ? Style.Orange : WoodDark, bOn ? Style.OrangeLip : WoodDarkLip);
		TraitsDisplay.SetWood(bOn ? PaintGrain : 1.f, FVector2D(0.4f, 0.7f), false, 100.f, 0.f, 0.f, FVector2D(300.f, 120.f));
		PinguMenu3D::SetTextColors(TraitsDisplayText, bOn ? Style.Snow : Style.Ice, Style.Ink);
	}
	if (WaitingText)
	{
		WaitingText->SetVisibility(Parts.bShown && bClient, true);
		if (bClient)
		{
			static const TCHAR* Steps[] = { TEXT(""), TEXT("."), TEXT(".."), TEXT("...") };
			const FString Dots = Steps[static_cast<int32>(PinguUI::Now() * 3.0) % 4];
			const FString Wanted = FText::Format(LOCTEXT("WaitingForHost", "Waiting for {0}{1}"),
				FText::FromString(PenguinHUD ? PenguinHUD->GetHostName() : FString()), FText::FromString(Dots)).ToString();
			if (Wanted != WaitingShown)
			{
				WaitingShown = Wanted;
				PinguMenu3D::SetText(WaitingText, FText::FromString(Wanted));
			}
		}
	}
}

void APinguMenu3D::SetButtonVisible(FButton& Button, bool bVisible)
{
	Button.bShown = bVisible;
	Button.Root->SetVisibility(bVisible, false);
	Button.Body->SetVisibility(bVisible, false);
	Button.Slab.SetVisible(bVisible && Button.Kind != EPinguMenu3DKind::Plain);
	Button.Ring.SetVisible(false);
	Button.Shadow.SetVisible(bVisible);
	Button.SelectRing.SetVisible(bVisible && Button.Selected && Button.Selected());
	Button.Track.SetVisible(bVisible);
	Button.Knob.SetVisible(bVisible);
	Button.Caret.SetVisible(false);
	if (Button.Tip)
	{
		Button.Tip->SetVisibility(bVisible && Button.Point != 0);
	}
	if (Button.Snow)
	{
		Button.Snow->SetVisibility(bVisible);
	}
	for (UStaticMeshComponent* Nail : Button.Nails)
	{
		if (Nail)
		{
			Nail->SetVisibility(bVisible);
		}
	}
	Button.Post.SetVisible(bVisible);
	if (Button.Text)
	{
		Button.Text->SetVisibility(bVisible, true);
	}
	if (Button.Ink)
	{
		Button.Ink->SetVisibility(bVisible, true);
	}
	if (Button.ValueText)
	{
		Button.ValueText->SetVisibility(bVisible, true);
	}
	// (The LAN page's address field has the same id: only the lobby strip's address line owns the copy icon.)
	if (Button.Id == FName(TEXT("Address")) && Button.Kind != EPinguMenu3DKind::Field)
	{
		if (CopyIcon)
		{
			CopyIcon->SetVisibility(bVisible, false);
		}
		CopyBack.SetVisible(bVisible);
		CopyFront.SetVisible(bVisible);
	}
}

void APinguMenu3D::UpdateButtonLook(FButton& Button, const FPageParts& Parts, bool bHot, float Delta)
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	// The wood menus (2026-10-26, Linus's mockup): the keyboard / controller focus gets the rope round the sign (and the yellow
	// arrow beside it: UpdateFocusArrow); the mouse hover tilts the sign and wiggles it as the hover starts. Both pop it out a
	// little (the old hot pop); rows, fields and list rows stay flat in their board (the rope says it - and a focused settings
	// row keeps its < > arrows in sight).
	const bool bMouse = Parts.bHotByMouse;
	// The hot look (PinguWood::HotLook, 2026-11-07, Linus: the keyboard / controller focus looks exactly like the mouse hover).
	const PinguWood::FHotLook& HotLook = PinguWood::HotLook();
	const bool bHovered = bHot && (bMouse ? Parts.bCursorOnHot : HotLook.bFocusLikeHover);
	Button.Hot = Approach(Button.Hot, bHot ? 1.f : 0.f, HotSpeed, Delta);
	Button.Focus = Approach(Button.Focus, bHot && !bMouse ? 1.f : 0.f, HotSpeed, Delta);
	Button.Hover = Approach(Button.Hover, bHovered ? 1.f : 0.f, HotSpeed, Delta);
	if (!bHovered)
	{
		Button.HoverT = 10.f;
	}
	else
	{
		Button.HoverT = Button.HoverT >= 10.f ? 0.f : FMath::Min(Button.HoverT + Delta, 5.f);
	}
	Button.PushT += Delta;
	const float Hot = EaseOut(Button.Hot);

	// The press: in fast, out a little slower.
	float Push = 0.f;
	if (Button.PushT < PushTime)
	{
		const float T = Button.PushT / PushTime;
		Push = T < 0.35f ? T / 0.35f : 1.f - (T - 0.35f) / 0.65f;
		Push = FMath::Clamp(Push, 0.f, 1.f);
	}
	const float Protrude = Button.Protrude >= 0.f ? Button.Protrude : ProtrudeOf(Button.Kind);
	const bool bQuiet = Button.Kind == EPinguMenu3DKind::Row || Button.Kind == EPinguMenu3DKind::Slider || Button.Kind == EPinguMenu3DKind::Field
		|| Button.Kind == EPinguMenu3DKind::ListRow || Button.Kind == EPinguMenu3DKind::Arrow;
	const float Pop = bQuiet ? 0.f : HotPop;
	const float Grow = bQuiet ? 0.f : HotGrow;
	const float FaceX = -(Protrude + Pop * Hot) + (bQuiet ? 0.5f : 1.f) * PushDepth * Push;
	// The click target: popped / pushed with the face, never turned or dropped (the picture's animations never move it).
	if (Button.Hit)
	{
		Button.Hit->SetRelativeLocation(FVector(FaceX, 0.f, 0.f));
	}
	// The picture: the page's drop-in (one sign after another, a small swing as it lands), the sway on the post (signs on posts
	// only), the hover tilt and its wiggle, a fixed hand-nailed tilt.
	const float DropAt = FMath::Min(static_cast<float>(Button.DropOrder) * DropStep, DropMaxDelay);
	const float DropT = FMath::Clamp((Parts.OpenT - DropAt) / DropTime, 0.f, 1.f);
	const float Landed = EaseOutBack(DropT);
	const float DropZ = (1.f - Landed) * DropHeight;
	const float SwingSide = (Button.DropOrder % 2 == 0) ? 1.f : -1.f;
	float Roll = (1.f - FMath::Clamp(Landed, 0.f, 1.f)) * DropSwing * SwingSide;
	if (Button.bSign || Button.Kind == EPinguMenu3DKind::Plain)
	{
		Roll += Button.StaticRoll;
		if (Parts.bNoBoard)
		{
			Roll += SignSway * FMath::Sin(2.f * PI * Clock / SignSwayPeriod + Button.SwayPhase);
		}
		const float Wiggle = Button.HoverT < 10.f ? WiggleAmp * FMath::Sin(Button.HoverT * WiggleFreq) * FMath::Exp(-Button.HoverT * WiggleDamp) : 0.f;
		const float TiltSide = Button.Point != 0 ? -static_cast<float>(Button.Point) : 1.f;
		Roll += TiltSide * (HoverTilt * EaseOut(Button.Hover) + Wiggle);
	}
	Button.Body->SetRelativeLocation(FVector(FaceX, 0.f, DropZ));
	Button.Body->SetRelativeRotation(FRotator(0.f, 0.f, Roll));
	Button.Body->SetRelativeScale3D(FVector(1.f + Grow * Hot - 0.02f * Push));

	// The wood: natural signs, a light birch for the primary ones (PLAY, START, ...), dark weathered wood for QUIT and the
	// rows; painted orange when selected (a tab, a preset, TRAITS on, a key cap being bound), painted red for LEAVE / CLOSE GAME.
	FLinearColor Face = WoodFace;
	FLinearColor Lip = WoodLip;
	FLinearColor Label = Style.Snow;
	// (A calmer grain under the letters since the second pass: it fought with small texts.)
	float Grain = 0.7f;
	float Lighten = 0.3f;
	const bool bSelected = Button.Selected && Button.Selected();
	const FLinearColor Painted = FMath::Lerp(Style.Orange, Style.Gold, 0.25f);
	switch (Button.Kind)
	{
	case EPinguMenu3DKind::Primary:
		// (Gold letters on the natural wood: the mockup's PLAY; a lighter face washed the gold out.)
		Face = FMath::Lerp(WoodFace, WoodLight, 0.35f); Lip = WoodLip; Label = Style.Gold;
		break;
	case EPinguMenu3DKind::Danger:
		Face = Style.Red; Lip = Style.RedLip; Grain = PaintGrain; Lighten = 0.18f;
		break;
	case EPinguMenu3DKind::Ghost:
		Face = WoodDark; Lip = WoodDarkLip; Label = Cream; Lighten = 0.45f;
		break;
	case EPinguMenu3DKind::Toggle:
	case EPinguMenu3DKind::Segment:
	case EPinguMenu3DKind::KeyCap:
		if (bSelected)
		{
			Face = Painted; Lip = Style.OrangeLip; Grain = PaintGrain; Lighten = 0.15f;
		}
		else if (Button.Kind == EPinguMenu3DKind::Toggle)
		{
			Face = WoodDark; Lip = WoodDarkLip; Label = Cream; Lighten = 0.45f;
		}
		else
		{
			Label = Button.Kind == EPinguMenu3DKind::KeyCap ? Style.Snow : Cream;
		}
		break;
	case EPinguMenu3DKind::Row:
	case EPinguMenu3DKind::Slider:
	case EPinguMenu3DKind::ListRow:
		Face = WoodStain; Lip = WoodStainLip; Label = Button.Kind == EPinguMenu3DKind::ListRow ? Style.Snow : Cream; Grain = 0.4f; Lighten = 0.6f;
		break;
	case EPinguMenu3DKind::Arrow:
		Face = FMath::Lerp(Style.Gold, Style.Orange, 0.3f); Lip = Style.OrangeLip; Label = WoodInk; Grain = PaintGrain; Lighten = 0.2f;
		break;
	case EPinguMenu3DKind::Field:
		Face = WoodStain * 0.6f; Lip = WoodStainLip; Grain = 0.5f; Lighten = 0.8f;
		Label = Button.FieldText.IsEmpty() ? CreamDim : Style.Snow;
		break;
	default:
		break;
	}
	if (Button.Enabled && !Button.Enabled())
	{
		Face = FMath::Lerp(WoodDark, FLinearColor(0.08f, 0.08f, 0.08f), 0.5f); Lip = WoodDarkLip; Label = CreamDim; Grain = 0.6f;
	}
	// The rendered wood (2026-11-07, card 2026-11-07_wood-ui; Linus: "make all the ui wooden and same light brown tone and not
	// diffrent"): every sign is the same light-brown plank picture; its state picks the picture, the words carry the colour
	// (PinguWood's palette: gold for the main action and a selected tab, red for LEAVE, cream otherwise). The 3D menus keep the
	// colours above.
	{
		using PinguWood::EState;
		const bool bQuietRow = Button.Kind == EPinguMenu3DKind::Row || Button.Kind == EPinguMenu3DKind::Slider
			|| Button.Kind == EPinguMenu3DKind::ListRow || Button.Kind == EPinguMenu3DKind::Field || Button.bRowPlank;
		EState State = bQuietRow ? EState::Inset : EState::Normal;
		if (PinguMenu3D::IsFlat())
		{
			// (Style 5: words on wood are #FFF4E2, UI_STYLE section 5.)
			Label = PinguWood::Text;
		}
		switch (Button.Kind)
		{
		case EPinguMenu3DKind::Primary: Label = PinguWood::Accent; break;
		case EPinguMenu3DKind::Danger: Label = PinguWood::Danger; break;
		case EPinguMenu3DKind::Arrow: Label = PinguWood::Text; break;
		case EPinguMenu3DKind::Toggle:
		case EPinguMenu3DKind::Segment:
		case EPinguMenu3DKind::KeyCap:
			if (bSelected)
			{
				State = EState::Selected;
				// (A selected option is icy, the value colour; the accent stays for the main action.)
				Label = PinguMenu3D::IsFlat() ? PinguWood::Value : PinguWood::Accent;
			}
			break;
		default:
			break;
		}
		if (Button.Kind == EPinguMenu3DKind::Field && Button.FieldText.IsEmpty())
		{
			Label = PinguWood::TextDim;
		}
		if (bHot && State != EState::Selected)
		{
			State = HotLook.HotState;
		}
		if (Push > 0.25f)
		{
			State = EState::Pressed;
		}
		if (Button.Enabled && !Button.Enabled())
		{
			State = EState::Disabled;
			Label = PinguWood::TextDim;
		}
		// (Rows keep the row plank when hot: their hover picture is row_hover.)
		Button.Slab.SetWoodImage(static_cast<uint8>(bQuietRow ? PinguWood::ECut::Row : PinguWood::ECut::Auto), static_cast<uint8>(State));
	}
	if (Button.LabelColor)
	{
		Label = OnWood(Button.LabelColor());
	}
	if (Button.Kind == EPinguMenu3DKind::Plain)
	{
		// No plank to light up: the hover and the focus (they look alike) turn its word to the hover colour.
		Label = FMath::Lerp(Label, Button.PlainHover, Hot);
	}
	// (Lit wood: a hot sign a little lighter, not washed out.)
	Button.Slab.SetColors(Face * (1.f + Lighten * Hot), Lip);
	Button.Slab.SetGrain(Grain);
	if (Button.Text)
	{
		// The focused sign's letters turn gold (the mockup's PLAY); fields, list rows and coloured labels keep theirs.
		const bool bKeepColor = Button.Kind == EPinguMenu3DKind::Field || Button.LabelColor != nullptr || Button.Kind == EPinguMenu3DKind::ListRow;
		const FLinearColor Shown = Label; // (2026-11-07: no gold letters for the keyboard focus - it looks like the hover)
		PinguMenu3D::SetTextColors(Button.Text, Shown, WoodInk, 0.f);
	}
	if (Button.ValueText)
	{
		PinguMenu3D::SetTextColors(Button.ValueText, Style.Snow, WoodInk, 0.f);
	}
	Button.SelectRing.SetVisible(Button.bShown && bSelected && Button.Kind == EPinguMenu3DKind::ListRow);
	if (Button.Kind == EPinguMenu3DKind::Slider)
	{
		UpdateSlider(Button);
	}
	if (Button.Kind == EPinguMenu3DKind::Field)
	{
		UpdateField(Button, bHot);
	}

	// The rope: a twisted rope round the sign a small gap away; it grows out of the sign's edge as the focus comes.
	const float Focus = EaseOut(Button.Focus);
	const bool bRope = HotLook.bRope && Focus > 0.01f;
	Button.Ring.SetVisible(Button.bShown && bRope);
	if (bRope)
	{
		const float Gap = FMath::Lerp(-2.f, RingGap, Focus);
		// Round the sign and its snow slab (2026-10-26, third pass: the rope cut through the snow).
		const float Snow = Button.Snow ? FMath::Min(SnowMax, SnowShare * Button.Size.Y) : 0.f;
		Button.Ring.Root->SetRelativeLocation(FVector(RingBack, 0.f, 0.5f * Snow));
		Button.Ring.SetShape(Button.Size.X, Button.Size.Y + Snow, Button.Slab.Radius, Gap);
		Button.Ring.SetColors(FLinearColor(0.86f, 0.68f, 0.4f), FLinearColor(0.36f, 0.22f, 0.1f), 0.12f * Focus);
	}
	// The shadow on the board spreads a little as the sign pops out toward the camera.
	if (Button.Shadow.IsBuilt())
	{
		const float Spread = ShadowSpread + 4.f * Hot;
		Button.Shadow.SetShape(Button.Size.X + 2.f * Spread, Button.Size.Y + 2.f * Spread, Button.Slab.Radius + Spread, 1.f);
		Button.Shadow.SetOpacity(ShadowOpacity * (1.f - 0.25f * Push));
	}
}

void APinguMenu3D::UpdateButtons(FPageParts& Parts, float Delta)
{
	// Under an overlay the page's buttons rest (no hot one).
	const bool bInput = &Parts == GetInputParts();
	for (int32 Index = 0; Index < Parts.Buttons.Num(); ++Index)
	{
		FButton& Button = Parts.Buttons[Index];
		if (Button.Visible)
		{
			const bool bWant = Button.Visible();
			if (bWant != Button.bShown)
			{
				SetButtonVisible(Button, bWant);
			}
		}
		if (!Button.bShown)
		{
			continue;
		}
		// The label follows its text (BACK / LEAVE / CLOSE GAME, TRAITS ON / OFF, the address / COPIED!, a key cap's key).
		if (Button.Label && Button.Kind != EPinguMenu3DKind::Field)
		{
			const FString Wanted = Button.Label().ToString();
			if (Wanted != Button.ShownLabel)
			{
				Button.ShownLabel = Wanted;
				PinguMenu3D::SetText(Button.Text, FText::FromString(Wanted));
				PinguMenu3D::SetText(Button.Ink, FText::FromString(Wanted));
				// (PlaceLabel adds the plank's own shift again.)
				const float Shift = Button.LabelAlign == PinguMenu3D::ETextAlign::Center && Button.Slab.Root ? static_cast<float>(Button.Slab.Root->GetRelativeLocation().Y) : 0.f;
				PlaceLabel(Button, Button.Text ? static_cast<float>(Button.Text->GetRelativeLocation().Y) - Shift : 0.f);
			}
		}
		if (Button.Value && Button.ValueText)
		{
			const FString Wanted = Button.Value().ToString();
			if (Wanted != Button.ShownValue)
			{
				Button.ShownValue = Wanted;
				PinguMenu3D::SetText(Button.ValueText, FText::FromString(Wanted));
			}
		}
		UpdateButtonLook(Button, Parts, bInput && Index == Parts.Hot, Delta);
	}
	UpdateLabels(Parts);
	UpdateDecos(Parts);
	UpdateStack(Parts);
	// The hot one went away (a button that hid): the page's default (a mouse-only page: none).
	if (&Parts != GetInputParts())
	{
		return;
	}
	if (Parts.Hot != INDEX_NONE && (!Parts.Buttons.IsValidIndex(Parts.Hot) || !IsButtonLive(Parts.Buttons[Parts.Hot])))
	{
		Parts.Hot = GetDefaultFocus();
		Parts.bHotByMouse = false;
	}
	else if (Parts.Hot == INDEX_NONE && !IsMouseOnlyPage())
	{
		Parts.Hot = GetDefaultFocus();
		Parts.bHotByMouse = false;
	}
}

void APinguMenu3D::UpdateLabels(FPageParts& Parts)
{
	for (FLabel& Label : Parts.Labels)
	{
		const bool bWant = !Label.Visible || Label.Visible();
		if (bWant != Label.bShown)
		{
			Label.bShown = bWant;
			if (Label.Text)
			{
				Label.Text->SetVisibility(bWant, true);
			}
			if (Label.Ink)
			{
				Label.Ink->SetVisibility(bWant, true);
			}
		}
		if (!bWant)
		{
			continue;
		}
		if (Label.Label)
		{
			const FString Wanted = Label.Label().ToString();
			if (Wanted != Label.Shown)
			{
				Label.Shown = Wanted;
				PinguMenu3D::SetText(Label.Text, FText::FromString(Wanted));
				PinguMenu3D::SetText(Label.Ink, FText::FromString(Wanted));
				if (Label.Ink && Label.Text)
				{
					Label.Ink->SetRelativeScale3D(Label.Text->GetRelativeScale3D());
				}
			}
		}
		const FLinearColor WantColor = Label.Color ? PinguMenu3DPrivate::OnWood(Label.Color()) : Label.ShownColor;
		const float WantGlow = Label.Glow ? Label.Glow() : Label.ShownGlow;
		if (!WantColor.Equals(Label.ShownColor, 0.002f) || !FMath::IsNearlyEqual(WantGlow, Label.ShownGlow, 0.005f))
		{
			Label.ShownColor = WantColor;
			Label.ShownGlow = WantGlow;
			Label.Side = FMath::Lerp(WantColor, PinguMenu3DPrivate::WoodInk, 0.6f);
			PinguMenu3D::SetTextColors(Label.Text, WantColor, Label.Side, WantGlow);
		}
	}
}

void APinguMenu3D::UpdateDecos(FPageParts& Parts)
{
	for (FDeco& Deco : Parts.Decos)
	{
		const bool bWant = !Deco.Visible || Deco.Visible();
		if (bWant != Deco.bShown)
		{
			Deco.bShown = bWant;
			if (Deco.Root)
			{
				Deco.Root->SetVisibility(bWant, false);
			}
			Deco.Slab.SetVisible(bWant);
			Deco.Ring.SetVisible(bWant);
			for (PinguMenu3D::FSlab& Plank : Deco.Extra)
			{
				Plank.SetVisible(bWant);
			}
			if (Deco.Icon)
			{
				Deco.Icon->SetVisibility(bWant, false);
			}
		}
		if (!bWant || !Deco.Color)
		{
			continue;
		}
		const FLinearColor WantColor = Deco.Color();
		if (!WantColor.Equals(Deco.ShownColor, 0.002f))
		{
			Deco.ShownColor = WantColor;
			if (Deco.Slab.IsBuilt())
			{
				Deco.Slab.SetColors(WantColor, Deco.Lip);
			}
			if (Deco.Ring.IsBuilt())
			{
				Deco.Ring.SetColors(WantColor, Deco.Lip);
			}
			if (Deco.IconMaterial)
			{
				Deco.IconMaterial->SetVectorParameterValue(TEXT("Tint"), WantColor);
			}
			if (UPinguMenu2DPart* IconPart = Cast<UPinguMenu2DPart>(Deco.Icon))
			{
				IconPart->Tint = WantColor;
			}
		}
	}
}

void APinguMenu3D::UpdateStack(FPageParts& Parts)
{
	using namespace PinguMenu3DPrivate;
	if (!Parts.bStack)
	{
		return;
	}
	// The visible stacked buttons from the top down (a hidden one takes no room: END MATCH, LEAVE / MAIN MENU); the plane's top
	// edge stays put and its height follows the stack.
	float Z = Parts.StackTop;
	for (FButton& Button : Parts.Buttons)
	{
		if (Button.StackGap < 0.f || !Button.bShown)
		{
			continue;
		}
		Z -= Button.StackGap;
		const float Center = Z - 0.5f * Button.Size.Y;
		if (!FMath::IsNearlyEqual(Center, Button.Center.Y, 0.1f))
		{
			Button.Center.Y = Center;
			Button.Root->SetRelativeLocation(FVector(0.f, Button.Center.X, Center));
		}
		Z -= Button.Size.Y;
	}
	const float Height = Parts.StackPlaneTop - Z + Parts.StackBottom;
	if (Parts.Plane.IsBuilt() && !FMath::IsNearlyEqual(Height, Parts.Plane.Height, 0.5f))
	{
		SetPlaneShape(Parts, Parts.Plane.Width, Height);
		Parts.Plane.Root->SetRelativeLocation(FVector(0.f, 0.f, Parts.StackPlaneTop - 0.5f * Height));
	}
}

void APinguMenu3D::UpdateSlider(FButton& Button)
{
	if (!Button.Knob.IsBuilt() || !Button.Track.IsBuilt() || !Button.SliderValue)
	{
		return;
	}
	const float Value = FMath::Clamp(Button.SliderValue(), 0.f, 1.f);
	const float TrackCenter = static_cast<float>(Button.Track.Root->GetRelativeLocation().Y);
	Button.Knob.Root->SetRelativeLocation(FVector(-4.f, TrackCenter - 100.f + 9.f + Value * 182.f, 0.f));
	// (The knob: a light log slice; a little lighter the higher the value.)
	Button.Knob.SetColors(FMath::Lerp(PinguMenu3DPrivate::WoodLight, PinguMenu3DPrivate::WoodLight * 1.3f, Value), PinguMenu3DPrivate::WoodLightLip);
}

void APinguMenu3D::SetSliderFromFace(FButton& Button, float FaceY)
{
	if (!Button.Track.IsBuilt() || !Button.OnSlider)
	{
		return;
	}
	const float TrackCenter = static_cast<float>(Button.Track.Root->GetRelativeLocation().Y);
	const float Value = FMath::Clamp((FaceY - (TrackCenter - 100.f + 9.f)) / 182.f, 0.f, 1.f);
	// In steps of 5 % as the keys (a drag still feels smooth: 20 steps over 200 units).
	Button.OnSlider(FMath::RoundToFloat(Value * 20.f) / 20.f);
}

void APinguMenu3D::UpdateField(FButton& Button, bool bHot)
{
	using namespace PinguMenu3DPrivate;
	const FPinguStyle& Style = FPinguStyle::Get();
	// The text, or the grey hint while empty; the caret after the text while the field is hot (it blinks).
	const bool bEmpty = Button.FieldText.IsEmpty();
	const FString Wanted = bEmpty ? Button.FieldHint.ToString() : Button.FieldText;
	if (Wanted != Button.ShownLabel)
	{
		Button.ShownLabel = Wanted;
		PinguMenu3D::SetText(Button.Text, FText::FromString(Wanted));
	}
	if (Button.Text)
	{
		PinguMenu3D::SetTextColors(Button.Text, bEmpty ? Style.IceDim : Style.Snow, Style.Ink, 0.f);
	}
	if (Button.Caret.IsBuilt())
	{
		const bool bBlink = FMath::Fmod(CaretClock, CaretPeriod) < 0.55f * CaretPeriod;
		Button.Caret.SetVisible(Button.bShown && bHot && bBlink);
		const float TextWidth = bEmpty ? 0.f : PinguMenu3D::GetTextWidth(Button.Text);
		Button.Caret.Root->SetRelativeLocation(FVector(-3.f, -0.5f * Button.Size.X + Button.LabelPad + TextWidth + 3.f, 0.f));
	}
}

void APinguMenu3D::Tick(float DeltaSeconds)
{
	using namespace PinguMenu3DPrivate;
	Super::Tick(DeltaSeconds);
	if (!bBuilt)
	{
		return;
	}
	// The UI's own pace (pauses and slow motion do not slow the menu).
	const float Delta = FMath::Min(static_cast<float>(FApp::GetDeltaTime()), 0.1f);
	Clock += Delta;

	CaretClock += Delta;
	// The real frame time while a page shows (the fps note of the pause / results pages).
	{
		static double LastReal = 0.0;
		const double Now = FPlatformTime::Seconds();
		if (LastReal > 0.0 && Page != EPinguMenu3DPage::None)
		{
			FrameAccum += Now - LastReal;
			++FrameCount;
			if (FrameAccum >= 1.0)
			{
				ShownFrameMs = static_cast<float>(1000.0 * FrameAccum / FMath::Max(FrameCount, 1));
				FrameAccum = 0.0;
				FrameCount = 0;
			}
		}
		LastReal = Now;
	}

	const EPinguMenu3DPage Wanted = TestPage != EPinguMenu3DPage::None ? TestPage : ComputePage();
	if (Wanted != Page)
	{
		ShowPage(Wanted);
	}
	const EPinguMenu3DPage WantedOverlay = Page != EPinguMenu3DPage::None ? ComputeOverlay() : EPinguMenu3DPage::None;
	if (WantedOverlay != Overlay)
	{
		ShowOverlay(WantedOverlay);
	}
	FPageParts* Parts = GetParts(Page);
	FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	if (Parts)
	{
		Parts->Appear = FMath::Min(1.f, Parts->Appear + Delta / AppearTime);
		Parts->OpenT += Delta;
		if (Page == EPinguMenu3DPage::Lobby && !bCardLobby)
		{
			UpdateLobbyLayout(*Parts);
		}
		UpdatePage(*Parts, Delta);
	}
	if (OverlayParts)
	{
		OverlayParts->Appear = FMath::Min(1.f, OverlayParts->Appear + Delta / AppearTime);
		OverlayParts->OpenT += Delta;
		UpdatePage(*OverlayParts, Delta);
	}
	PlaceOnCamera();
	UpdateDims();
	if (Parts)
	{
		PollHover();
	}
	if (Parts)
	{
		// The address line: its text left-aligned in it, the copy icon after the text, green while COPIED! shows.
		if (Page == EPinguMenu3DPage::Lobby)
		{
			for (FButton& Button : Parts->Buttons)
			{
				if (Button.Id == FName(TEXT("Address")) && Button.Text && Button.bShown)
				{
					const float TextWidth = PinguMenu3D::GetTextWidth(Button.Text);
					const float Width = FMath::Max(240.f, TextWidth + 24.f + 34.f + (bCardLobby ? 14.f : 0.f));
					if (!FMath::IsNearlyEqual(Width, Button.Size.X, 0.5f))
					{
						Button.Size.X = Width;
						ApplySignShape(Button);
						// (The card lobby: centred under its signs.)
						Button.Center.X = bCardLobby ? 0.f : AddressLeft + 0.5f * Width;
						Button.Root->SetRelativeLocation(FVector(0.f, Button.Center.X, Button.Center.Y));
					}
					PlaceLabel(Button, -0.5f * Width + 12.f + 0.5f * TextWidth);
					CopyIcon->SetRelativeLocation(FVector(0.f, 0.5f * Width - 22.f, 0.f));
				}
			}
		}
		UpdateButtons(*Parts, Delta);
		if (OverlayParts)
		{
			UpdateButtons(*OverlayParts, Delta);
		}
		// The copy line: green while it says COPIED! (after UpdateButtons, which gives every label its usual colour).
		if (Page == EPinguMenu3DPage::Lobby)
		{
			for (FButton& Button : Parts->Buttons)
			{
				if (Button.Id == FName(TEXT("Address")) && Button.Text && Button.bShown)
				{
					const FPinguStyle& Style = FPinguStyle::Get();
					const bool bCopied = HUD.IsValid() && HUD->IsHostAddressCopied();
					const FLinearColor IconColor = bCopied ? Style.Green : Style.Ice;
					CopyBack.SetColors(FMath::Lerp(IconColor, Style.Ink, 0.45f), Style.Ink);
					CopyFront.SetColors(IconColor, FMath::Lerp(IconColor, Style.Ink, 0.5f));
					if (bCopied)
					{
						PinguMenu3D::SetTextColors(Button.Text, Style.Green, Style.Ink);
					}
				}
			}
		}
	}

	UpdatePuff(Delta);
	UpdateFocusArrow(Delta);

	// A press waiting for its push-in.
	if (PendingPress != INDEX_NONE)
	{
		PendingLeft -= Delta;
		if (PendingLeft <= 0.f)
		{
			const int32 Index = PendingPress;
			PendingPress = INDEX_NONE;
			FPageParts* InputParts = GetInputParts();
			if (InputParts && InputParts->Buttons.IsValidIndex(Index))
			{
				FireAction(InputParts->Buttons[Index]);
			}
		}
	}
}

void APinguMenu3D::UpdatePage(FPageParts& Parts, float Delta)
{
	(void)Delta;
	switch (Parts.Id)
	{
	case EPinguMenu3DPage::Lan: UpdateLan(Parts); break;
	case EPinguMenu3DPage::Results: UpdateResults(Parts); break;
	case EPinguMenu3DPage::Board: UpdateBoard(Parts); break;
	case EPinguMenu3DPage::Settings: UpdateSettings(Parts); break;
	case EPinguMenu3DPage::Pingupedia: UpdatePingupedia(Parts); break;
	case EPinguMenu3DPage::Capture: UpdateCapture(Parts); break;
	case EPinguMenu3DPage::Lobby:
		if (bCardLobby)
		{
			UpdateCardLobby(Parts);
		}
		break;
	default: break;
	}
}

// ---- lists

void APinguMenu3D::ApplyListWindow(FPageParts& Parts)
{
	if (Parts.ListWindow <= 0)
	{
		return;
	}
	Parts.ListScroll = FMath::Clamp(Parts.ListScroll, 0, FMath::Max(Parts.ListCount - Parts.ListWindow, 0));
	for (FButton& Button : Parts.Buttons)
	{
		if (Button.ListId < 0)
		{
			continue;
		}
		if (Button.ListId != Parts.ActiveList)
		{
			if (Button.bShown)
			{
				SetButtonVisible(Button, false);
			}
			continue;
		}
		const int32 Row = Button.ListIndex - Parts.ListScroll;
		// (A row's own rule - its page of the PINGUPEDIA - decides too; it reads the window as well.)
		const bool bInWindow = Row >= 0 && Row < Parts.ListWindow && (!Button.Visible || Button.Visible());
		Button.Center.Y = Parts.ListTop - Row * Parts.ListStep;
		if (Button.Root)
		{
			Button.Root->SetRelativeLocation(FVector(0.f, Button.Center.X, Button.Center.Y));
		}
		if (bInWindow != Button.bShown)
		{
			SetButtonVisible(Button, bInWindow);
		}
	}
}

void APinguMenu3D::ScrollListTo(FPageParts& Parts, int32 Index)
{
	if (Parts.ListWindow <= 0)
	{
		return;
	}
	if (Index < Parts.ListScroll)
	{
		Parts.ListScroll = Index;
	}
	else if (Index >= Parts.ListScroll + Parts.ListWindow)
	{
		Parts.ListScroll = Index - Parts.ListWindow + 1;
	}
	ApplyListWindow(Parts);
}

bool APinguMenu3D::Scroll(float Delta)
{
	FPageParts* Parts = GetInputParts();
	if (!Parts || Parts->ListWindow <= 0 || Parts->ListCount <= Parts->ListWindow)
	{
		return false;
	}
	const int32 Before = Parts->ListScroll;
	Parts->ListScroll -= Delta > 0.f ? 1 : -1;
	ApplyListWindow(*Parts);
	// The row under the resting cursor changed: looked at again on the next poll.
	LastCursorPixel = FVector2D(-1.f, -1.f);
	return Parts->ListScroll != Before;
}

// ---- text fields

bool APinguMenu3D::IsTyping() const
{
	const FPageParts* Parts = GetInputParts();
	return Parts && Parts->bShown && Parts->Buttons.IsValidIndex(Parts->Hot) && Parts->Buttons[Parts->Hot].Kind == EPinguMenu3DKind::Field
		&& Parts->Appear > 0.f;
}

bool APinguMenu3D::TypeChar(TCHAR Char)
{
	FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Parts->Hot) || Parts->Buttons[Parts->Hot].Kind != EPinguMenu3DKind::Field)
	{
		return false;
	}
	FButton& Field = Parts->Buttons[Parts->Hot];
	// Printable characters only (Enter, Backspace, Esc and Tab come as keys).
	if (Char < 32 || Char == 127)
	{
		return false;
	}
	if (Field.FieldText.Len() >= Field.FieldMax)
	{
		return true;
	}
	Field.FieldText.AppendChar(Char);
	if (Field.OnFieldChanged)
	{
		Field.OnFieldChanged(Field.FieldText);
	}
	CaretClock = 0.f;
	return true;
}

bool APinguMenu3D::FieldKey(const FKeyEvent& KeyEvent)
{
	FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Parts->Hot) || Parts->Buttons[Parts->Hot].Kind != EPinguMenu3DKind::Field)
	{
		return false;
	}
	FButton& Field = Parts->Buttons[Parts->Hot];
	const FKey Key = KeyEvent.GetKey();
	bool bChanged = false;
	if (Key == EKeys::BackSpace)
	{
		if (Field.FieldText.Len() > 0)
		{
			Field.FieldText.LeftChopInline(1);
			bChanged = true;
		}
	}
	else if (Key == EKeys::Delete)
	{
		if (Field.FieldText.Len() > 0)
		{
			Field.FieldText.Reset();
			bChanged = true;
		}
	}
	else if (Key == EKeys::V && KeyEvent.IsControlDown())
	{
		// Ctrl+V: the clipboard's text (one line, trimmed) appended.
		FString Pasted;
		FPlatformApplicationMisc::ClipboardPaste(Pasted);
		Pasted = Pasted.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
		if (!Pasted.IsEmpty())
		{
			Field.FieldText = (Field.FieldText + Pasted).Left(Field.FieldMax);
			bChanged = true;
		}
	}
	else if (Key == EKeys::Left || Key == EKeys::Right || Key == EKeys::Home || Key == EKeys::End)
	{
		return true; // the caret is always at the end: these do nothing, and they must not move the focus away
	}
	else
	{
		return false;
	}
	if (bChanged && Field.OnFieldChanged)
	{
		Field.OnFieldChanged(Field.FieldText);
	}
	CaretClock = 0.f;
	return true;
}

void APinguMenu3D::CommitFields()
{
	APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD || !Lan.bBuilt)
	{
		return;
	}
	for (const FButton& Button : Lan.Buttons)
	{
		if (Button.Id == FName(TEXT("Name")) && Button.Kind == EPinguMenu3DKind::Field)
		{
			PenguinHUD->Action_SetPlayerName(Button.FieldText);
		}
	}
}

// ---- the overlay's texts (every 3D text of the menu, drawn by SPinguMenu3DOverlay)

void PinguMenu3D::SetTextInOverlay(USceneComponent* Text, bool bOverlay)
{
	using namespace PinguMenu3DPrivate;
	FTextState* State = Text ? TextStates().Find(Text) : nullptr;
	if (!State)
	{
		return;
	}
	State->Look.bOverlay = bOverlay;
	State->bOverlay = bOverlay && State->Look.Outline <= 0.f;
	PrepareChildren(Cast<UText3DComponent>(Text), State->Look.bMenuLight, bOverlay);
}

bool PinguMenu3D::IsTextOverlayOn()
{
	return PinguMenu3DPrivate::GTextOverlay;
}

void PinguMenu3D::SetTextOverlay(bool bOn)
{
	using namespace PinguMenu3DPrivate;
	GTextOverlay = bOn;
	for (TPair<TWeakObjectPtr<USceneComponent>, FTextState>& Pair : TextStates())
	{
		if (UText3DComponent* Text = Cast<UText3DComponent>(Pair.Key.Get()))
		{
			PrepareChildren(Text, Pair.Value.Look.bMenuLight, Pair.Value.Look.bOverlay);
		}
	}
}

void APinguMenu3D::GetOverlayTexts(TArray<FPinguMenu3DTextDraw>& Out) const
{
	using namespace PinguMenu3DPrivate;
	Out.Reset();
	if (Page == EPinguMenu3DPage::None || bHiddenForTest || (!GTextOverlay && !PinguMenu3D::IsFlat()))
	{
		return;
	}
	// Under an overlay page (PRESS A KEY, the error popup) only its own texts: the page under it never shows through.
	const FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	const USceneComponent* OnlyUnder = OverlayParts && OverlayParts->bShown ? OverlayParts->Root : nullptr;
	const FPageParts* PageParts = GetParts(Page);
	const USceneComponent* PageRoot = PageParts && PageParts->bShown ? PageParts->Root : nullptr;
	if (!PageRoot && !OnlyUnder)
	{
		return;
	}
	const float Cap = FMath::Max(CapAt100, 1.f);
	for (const TPair<TWeakObjectPtr<USceneComponent>, FTextState>& Pair : TextStates())
	{
		const USceneComponent* Text = Pair.Key.Get();
		const FTextState& State = Pair.Value;
		if (!Text || !State.bOverlay || State.Text.IsEmpty() || State.BoxMaxY <= State.BoxMinY || !Text->IsVisible())
		{
			continue;
		}
		if (Text->GetOwner() == this)
		{
			// The menu's own: shown when the text itself shows (signs, rows and labels show and hide their texts) and it belongs
			// to the page that shows (its page's root is one of its parents; the in-between parts keep no visibility of their own).
			bool bOnPage = false;
			for (const USceneComponent* Part = Text->GetAttachParent(); Part; Part = Part->GetAttachParent())
			{
				if (Part == (OnlyUnder ? OnlyUnder : PageRoot))
				{
					bOnPage = true;
					break;
				}
			}
			if (!bOnPage)
			{
				continue;
			}
		}
		else
		{
			// The lobby island's name and difficulty tags (2026-10-26, fourth pass): only while the lobby shows with no page over
			// it, in this world, and when every part they hang on shows.
			if (Page != EPinguMenu3DPage::Lobby || OnlyUnder || Text->GetWorld() != GetWorld())
			{
				continue;
			}
			bool bShown = true;
			for (const USceneComponent* Part = Text->GetAttachParent(); Part && bShown; Part = Part->GetAttachParent())
			{
				bShown = Part->IsVisible();
			}
			if (!bShown)
			{
				continue;
			}
		}
		// The capitals' box on the screen: its top left, top right and bottom left (on the baseline) - the 3D letters' place, so
		// the words ride along with every turn, tilt, drop and push of their sign.
		const FTransform& ToWorld = Text->GetComponentTransform();
		FPinguMenu3DTextDraw Draw;
		// (The lobby's tags stand in the world: the player's camera; the menu's words: the menu's own.)
		const bool bWorld = Text->GetOwner() != this;
		auto Project = [this, bWorld](const FVector& Point, FVector2D& OutPixel) { return bWorld ? ProjectWorldToPixel(Point, OutPixel) : ProjectToPixel(Point, OutPixel); };
		if (!Project(ToWorld.TransformPosition(FVector(0.f, State.BoxMinY, BaselineAt100 + Cap)), Draw.TopLeft)
			|| !Project(ToWorld.TransformPosition(FVector(0.f, State.BoxMaxY, BaselineAt100 + Cap)), Draw.TopRight)
			|| !Project(ToWorld.TransformPosition(FVector(0.f, State.BoxMinY, BaselineAt100)), Draw.BottomLeft))
		{
			continue;
		}
		Draw.Text = State.Text;
		Draw.Color = State.Front;
		Draw.bBig = State.Look.Size >= 40.f;
		Out.Add(MoveTemp(Draw));
	}
}

int32 APinguMenu3D::CountTextMisfits(int32& OutChecked, FString& OutList, float Margin) const
{
	using namespace PinguMenu3DPrivate;
	OutChecked = 0;
	OutList.Reset();
	const FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->bShown)
	{
		return 0;
	}
	// The box each text must stay in: its sign / row (the button's face), its strip or pill (a deco's slab), or the page's board.
	struct FBox2 { const USceneComponent* Space; float Width; float Height; FString Name; };
	TMap<const USceneComponent*, FBox2> Boxes;
	for (const FButton& Button : Parts->Buttons)
	{
		if (Button.Body && Button.bShown)
		{
			Boxes.Add(Button.Body, { Button.Body, static_cast<float>(Button.Size.X), static_cast<float>(Button.Size.Y), Button.Id.ToString() });
		}
	}
	for (const FDeco& Deco : Parts->Decos)
	{
		if (Deco.Root && Deco.Slab.IsBuilt())
		{
			Boxes.Add(Deco.Root, { Deco.Slab.Root, Deco.Slab.Width, Deco.Slab.Height, TEXT("strip") });
		}
	}
	if (Parts->Root && Parts->Plane.IsBuilt() && !Parts->bNoBoard)
	{
		Boxes.Add(Parts->Root, { Parts->Plane.Root, Parts->Plane.Width, Parts->Plane.Height, TEXT("board") });
	}
	int32 Misfits = 0;
	// The flat texts (paragraphs, stat names and values, hints): each line no wider than its box (else it would wrap again and
	// run past the box's height), all lines within the box's height, and a stat's name and value side by side with a gap.
	if (FSlateApplication::IsInitialized())
	{
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		auto FontOf = [](const FFlat& Flat) { return Flat.bTitleFont ? FPinguStyle::Title(Flat.Size) : FPinguStyle::Body(Flat.Size); };
		auto Note = [&OutList, &Misfits](const FString& Line)
		{
			++Misfits;
			if (OutList.Len() < 600)
			{
				OutList += Line;
			}
		};
		for (int32 Index = 0; Index < Parts->Flats.Num(); ++Index)
		{
			const FFlat& Flat = Parts->Flats[Index];
			if (!Flat.Space || !Flat.Text || (Flat.Visible && !Flat.Visible()))
			{
				continue;
			}
			const FString Text = Flat.Text().ToString();
			if (Text.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}
			++OutChecked;
			const FSlateFontInfo Font = FontOf(Flat);
			TArray<FString> Lines;
			Text.ParseIntoArray(Lines, TEXT("\n"), false);
			float Widest = 0.f;
			for (const FString& Line : Lines)
			{
				Widest = FMath::Max(Widest, static_cast<float>(Measure->Measure(Line, Font).X));
			}
			const float Tall = static_cast<float>(Lines.Num()) * Measure->GetMaxCharacterHeight(Font);
			if (Widest > Flat.Width + 0.5f)
			{
				Note(FString::Printf(TEXT("\"%s\" line %.0f wide in %.0f; "), *Text.Left(24), Widest, Flat.Width));
			}
			else if (Tall > Flat.Height + 0.5f * Measure->GetMaxCharacterHeight(Font))
			{
				Note(FString::Printf(TEXT("\"%s\" %.0f high in %.0f; "), *Text.Left(24), Tall, Flat.Height));
			}
			// A name and its value in the same box (left / right): a gap between them.
			for (int32 Other = Index + 1; Other < Parts->Flats.Num(); ++Other)
			{
				const FFlat& Value = Parts->Flats[Other];
				if (Value.Space != Flat.Space || Flat.Justify != 0 || Value.Justify != 2 || !Value.Text || !FMath::IsNearlyEqual(Value.Width, Flat.Width, 0.5f))
				{
					continue;
				}
				const float NameWidth = static_cast<float>(Measure->Measure(Text, Font).X);
				const float ValueWidth = static_cast<float>(Measure->Measure(Value.Text().ToString(), FontOf(Value)).X);
				if (NameWidth + ValueWidth + 18.f > Flat.Width)
				{
					Note(FString::Printf(TEXT("\"%s\" + \"%s\" %.0f wide in %.0f; "), *Text, *Value.Text().ToString(), NameWidth + ValueWidth + 18.f, Flat.Width));
				}
			}
		}
	}
	const float Cap = FMath::Max(CapAt100, 1.f);
	for (const TPair<TWeakObjectPtr<USceneComponent>, FTextState>& Pair : TextStates())
	{
		const USceneComponent* Text = Pair.Key.Get();
		const FTextState& State = Pair.Value;
		if (!Text || Text->GetOwner() != this || !State.bOverlay || State.Text.TrimStartAndEnd().IsEmpty() || !Text->IsVisible() || State.BoxMaxY <= State.BoxMinY)
		{
			continue;
		}
		const FBox2* Box = nullptr;
		bool bOnPage = false;
		for (const USceneComponent* Part = Text->GetAttachParent(); Part; Part = Part->GetAttachParent())
		{
			if (!Box)
			{
				Box = Boxes.Find(Part);
			}
			if (Part == Parts->Root)
			{
				bOnPage = true;
				break;
			}
		}
		if (!bOnPage || !Box || !Box->Space)
		{
			continue;
		}
		++OutChecked;
		// The capitals' box (its four corners) in the box's own space: inside its face, Margin in from the ends.
		const FTransform& ToWorld = Text->GetComponentTransform();
		const FTransform& BoxWorld = Box->Space->GetComponentTransform();
		float Over = 0.f;
		for (const FVector& Corner : { FVector(0.f, State.BoxMinY, BaselineAt100), FVector(0.f, State.BoxMaxY, BaselineAt100),
			FVector(0.f, State.BoxMinY, BaselineAt100 + Cap), FVector(0.f, State.BoxMaxY, BaselineAt100 + Cap) })
		{
			const FVector Local = BoxWorld.InverseTransformPosition(ToWorld.TransformPosition(Corner));
			Over = FMath::Max(Over, static_cast<float>(FMath::Abs(Local.Y)) - (0.5f * Box->Width - Margin));
			Over = FMath::Max(Over, static_cast<float>(FMath::Abs(Local.Z)) - 0.5f * Box->Height);
		}
		if (Over > 0.f)
		{
			++Misfits;
			if (OutList.Len() < 600)
			{
				OutList += FString::Printf(TEXT("%s\"%s\" in %s %.0f over; "), OutList.IsEmpty() ? TEXT("") : TEXT(""), *State.Text, *Box->Name, Over);
			}
		}
	}
	return Misfits;
}

bool APinguMenu3D::GetButtonTextPixel(int32 Index, FVector2D& OutCenter) const
{
	using namespace PinguMenu3DPrivate;
	const FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Index) || !Parts->Buttons[Index].Text)
	{
		return false;
	}
	const USceneComponent* Text = Parts->Buttons[Index].Text;
	const FTextState* State = TextStates().Find(Text);
	if (!State)
	{
		return false;
	}
	const FVector Middle(0.f, 0.5f * (State->BoxMinY + State->BoxMaxY), BaselineAt100 + 0.5f * FMath::Max(CapAt100, 1.f));
	return ProjectToPixel(Text->GetComponentTransform().TransformPosition(Middle), OutCenter);
}

// ---- the flat texts (SPinguMenu3DOverlay)

int32 APinguMenu3D::GetFlatCount() const
{
	const FPageParts* Parts = GetParts(Page);
	const FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	return (Parts ? Parts->Flats.Num() : 0) + (OverlayParts ? OverlayParts->Flats.Num() : 0);
}

bool APinguMenu3D::GetFlat(int32 Index, FText& OutText, FSlateFontInfo& OutFont, FLinearColor& OutColor, int32& OutJustify, bool& OutVisible) const
{
	const FPageParts* Parts = GetParts(Page);
	const FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	const FFlat* Flat = nullptr;
	if (Parts && Index < Parts->Flats.Num())
	{
		Flat = &Parts->Flats[Index];
	}
	else if (OverlayParts && Parts && Index - Parts->Flats.Num() < OverlayParts->Flats.Num())
	{
		Flat = &OverlayParts->Flats[Index - Parts->Flats.Num()];
	}
	if (!Flat)
	{
		return false;
	}
	OutText = Flat->Text ? Flat->Text() : FText::GetEmpty();
	// (No outline: it made every line wider and taller than the card was laid out for - the paragraphs wrapped early and ran onto
	// the stat strips, labels touched their values. The overlay gives the flats a dark shadow instead, which takes no room.)
	OutFont = Flat->bTitleFont ? FPinguStyle::Title(Flat->Size) : FPinguStyle::Body(Flat->Size);
	OutColor = Flat->Color ? PinguMenu3DPrivate::OnWood(Flat->Color()) : FLinearColor::White;
	OutJustify = Flat->Justify;
	OutVisible = (!Flat->Visible || Flat->Visible()) && Flat->Space != nullptr && !bHiddenForTest;
	return true;
}

bool APinguMenu3D::ProjectLocal(const USceneComponent* Space, const FVector& Local, FVector2D& OutPixel) const
{
	return Space && ProjectToPixel(Space->GetComponentTransform().TransformPosition(Local), OutPixel);
}

bool APinguMenu3D::GetFlatFrame(int32 Index, FVector2D& OutTopLeft, FVector2D& OutTopRight, FVector2D& OutBottomLeft) const
{
	const FPageParts* Parts = GetParts(Page);
	const FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	const FFlat* Flat = nullptr;
	if (Parts && Index < Parts->Flats.Num())
	{
		Flat = &Parts->Flats[Index];
	}
	else if (OverlayParts && Parts && Index - Parts->Flats.Num() < OverlayParts->Flats.Num())
	{
		Flat = &OverlayParts->Flats[Index - Parts->Flats.Num()];
	}
	if (!Flat || !Flat->Space)
	{
		return false;
	}
	return ProjectLocal(Flat->Space, FVector(-2.f, Flat->Left, Flat->Top), OutTopLeft)
		&& ProjectLocal(Flat->Space, FVector(-2.f, Flat->Left + Flat->Width, Flat->Top), OutTopRight)
		&& ProjectLocal(Flat->Space, FVector(-2.f, Flat->Left, Flat->Top - Flat->Height), OutBottomLeft);
}

bool APinguMenu3D::GetFlatSize(int32 Index, float& OutWidth, float& OutHeight) const
{
	const FPageParts* Parts = GetParts(Page);
	const FPageParts* OverlayParts = Overlay != EPinguMenu3DPage::None ? GetParts(Overlay) : nullptr;
	const FFlat* Flat = nullptr;
	if (Parts && Index < Parts->Flats.Num())
	{
		Flat = &Parts->Flats[Index];
	}
	else if (OverlayParts && Parts && Index - Parts->Flats.Num() < OverlayParts->Flats.Num())
	{
		Flat = &OverlayParts->Flats[Index - Parts->Flats.Num()];
	}
	if (!Flat)
	{
		return false;
	}
	OutWidth = Flat->Width;
	OutHeight = Flat->Height;
	return true;
}

bool APinguMenu3D::GetPlaneScreenRect(FVector2D& OutCenter, FVector2D& OutSize) const
{
	const FPageParts* Parts = GetParts(Page);
	FVector2D ViewSize;
	if (!Parts || !Parts->Plane.IsBuilt() || !PinguMenu3D::GetViewportPixelSize(GetController(), ViewSize))
	{
		return false;
	}
	FVector2D C;
	FVector2D Lo;
	FVector2D Hi;
	if (!ProjectLocal(Parts->Plane.Root, FVector::ZeroVector, C) || !ProjectLocal(Parts->Plane.Root, FVector(0.f, -0.5f * Parts->Plane.Width, -0.5f * Parts->Plane.Height), Lo)
		|| !ProjectLocal(Parts->Plane.Root, FVector(0.f, 0.5f * Parts->Plane.Width, 0.5f * Parts->Plane.Height), Hi))
	{
		return false;
	}
	OutCenter = C / ViewSize;
	OutSize = FVector2D(FMath::Abs(Hi.X - Lo.X), FMath::Abs(Hi.Y - Lo.Y)) / ViewSize;
	return true;
}

// ---- input

int32 APinguMenu3D::TraceButton(const FVector2D& ViewportUV) const
{
	FVector2D Size;
	if (!PinguMenu3D::GetViewportPixelSize(GetController(), Size))
	{
		return INDEX_NONE;
	}
	return TraceButtonPixel(ViewportUV * Size);
}

int32 APinguMenu3D::TraceButtonPixel(const FVector2D& ViewportPixel) const
{
	// The engine's own ray through the pixel: the projection the picture was drawn with (any FOV, aspect or viewport; the 2D
	// menus: their own camera).
	FVector Origin;
	FVector Direction;
	if (!DeprojectPixel(ViewportPixel, Origin, Direction))
	{
		return INDEX_NONE;
	}
	return TraceButtonRay(Origin, Direction);
}

int32 APinguMenu3D::TraceButtonRay(const FVector& Origin, const FVector& Direction) const
{
	using namespace PinguMenu3DPrivate;
	const FPageParts* Parts = GetInputParts();
	if (!Parts || Parts->Appear < 0.6f)
	{
		return INDEX_NONE;
	}
	// Two passes: first the faces themselves (the nearest one the ray crosses), then with a margin round each (a click just
	// off a button still counts, but never steals a point that is squarely on a neighbour: a popped-out hot button stands
	// nearer the camera and its margin would otherwise cover the next button's edge).
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const float Margin = Pass == 0 ? 0.f : HitMargin;
		int32 Best = INDEX_NONE;
		double BestT = TNumericLimits<double>::Max();
		double BestScore = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Parts->Buttons.Num(); ++Index)
		{
			const FButton& Button = Parts->Buttons[Index];
			if (!IsButtonLive(Button) || !Button.Body || !Button.Root)
			{
				continue;
			}
			// Into the button's own space: its face is the plane X = 0 there, Size wide and high round the origin. Tried where
			// the face is now (popped out, pushed in) and where it rests (the root): a click on a button that is moving under
			// the cursor hits either way.
			const USceneComponent* Spaces[2] = { Button.Hit ? Button.Hit : Button.Body, Button.Root };
			for (const USceneComponent* Space : Spaces)
			{
				const FTransform& ToWorld = Space->GetComponentTransform();
				const FVector A = ToWorld.InverseTransformPosition(Origin);
				const FVector B = ToWorld.InverseTransformPosition(Origin + Direction * 100000.0);
				const double DX = B.X - A.X;
				if (FMath::Abs(DX) < 1e-6)
				{
					continue;
				}
				const double T = -A.X / DX;
				if (T <= 0.0 || (Pass == 0 && T >= BestT))
				{
					continue;
				}
				const FVector Hit = A + (B - A) * T;
				if (FMath::Abs(Hit.Y) <= 0.5 * Button.Size.X + Margin && FMath::Abs(Hit.Z) <= 0.5 * Button.Size.Y + Margin)
				{
					// In the margin pass the button the point is least far outside of wins (then the nearer one): a click just
					// off a row that is being pushed in (it shrinks a little toward the picture's middle) stays that row's,
					// not its neighbour's (2026-10-26: rows no longer pop out when hot, so depth alone no longer decided it).
					const double Outside = FMath::Max(FMath::Abs(Hit.Y) - 0.5 * Button.Size.X, 0.0) + FMath::Max(FMath::Abs(Hit.Z) - 0.5 * Button.Size.Y, 0.0);
					const double Score = Pass == 0 ? T : Outside * 1000.0 + T * 1e-6;
					if (Score < BestScore)
					{
						Best = Index;
						BestT = T;
						BestScore = Score;
					}
				}
			}
		}
		if (Best != INDEX_NONE)
		{
			return Best;
		}
	}
	return INDEX_NONE;
}

bool APinguMenu3D::TraceFacePoint(const FButton& Button, const FVector2D& ViewportPixel, FVector2D& OutFace) const
{
	FVector Origin;
	FVector Direction;
	const USceneComponent* Face = Button.Hit ? Button.Hit : Button.Body;
	if (!Face || !DeprojectPixel(ViewportPixel, Origin, Direction))
	{
		return false;
	}
	const FTransform& ToWorld = Face->GetComponentTransform();
	const FVector A = ToWorld.InverseTransformPosition(Origin);
	const FVector B = ToWorld.InverseTransformPosition(Origin + Direction * 100000.0);
	const double DX = B.X - A.X;
	if (FMath::Abs(DX) < 1e-6)
	{
		return false;
	}
	const double T = -A.X / DX;
	if (T <= 0.0)
	{
		return false;
	}
	const FVector Hit = A + (B - A) * T;
	OutFace = FVector2D(Hit.Y, Hit.Z);
	return true;
}

bool APinguMenu3D::GetButtonFacePixel(int32 Index, const FVector2D& FaceShare, FVector2D& OutPixel) const
{
	const FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Index) || !Parts->Buttons[Index].Body)
	{
		return false;
	}
	const FButton& Button = Parts->Buttons[Index];
	const USceneComponent* Face = Button.Hit ? Button.Hit : Button.Body;
	const FVector World = Face->GetComponentTransform().TransformPosition(FVector(0.f, 0.5f * FaceShare.X * Button.Size.X, 0.5f * FaceShare.Y * Button.Size.Y));
	return ProjectToPixel(World, OutPixel);
}

void APinguMenu3D::SetHot(int32 Index, bool bSound, bool bMouse)
{
	FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Index))
	{
		return;
	}
	// (Who made it hot decides its look: the mouse tilts it, the keys put the rope and the arrow on it.)
	Parts->bHotByMouse = bMouse;
	if (bMouse)
	{
		Parts->bCursorOnHot = true;
	}
	if (Index == Parts->Hot)
	{
		return;
	}
	Parts->Hot = Index;
	if (bSound && HUD.IsValid())
	{
		HUD->PlayUISound(false);
	}
	if (Parts->Buttons[Index].OnHot)
	{
		Parts->Buttons[Index].OnHot();
	}
}

void APinguMenu3D::PointerMove(const FVector2D& ViewportUV)
{
	bPointerIn = true;
	PointerUV = ViewportUV;
	const int32 Under = TraceButton(ViewportUV);
	if (Under != INDEX_NONE)
	{
		SetHot(Under, false, true);
	}
	else if (FPageParts* Off = GetInputParts())
	{
		// The cursor left the signs: the hovered one stops tilting (it stays the hot one for Enter).
		Off->bCursorOnHot = false;
	}
	if (Under == INDEX_NONE && IsMouseOnlyPage())
	{
		if (FPageParts* Parts = GetInputParts())
		{
			Parts->Hot = INDEX_NONE;
		}
	}
}

void APinguMenu3D::PointerMovePixel(const FVector2D& ViewportPixel)
{
	FVector2D Size;
	if (PinguMenu3D::GetViewportPixelSize(GetController(), Size))
	{
		PointerMove(ViewportPixel / Size);
	}
}

void APinguMenu3D::PointerLeave()
{
	bPointerIn = false;
	if (IsMouseOnlyPage())
	{
		if (FPageParts* Parts = GetInputParts())
		{
			Parts->Hot = INDEX_NONE;
		}
	}
}

void APinguMenu3D::PointerDragPixel(const FVector2D& ViewportPixel)
{
	FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(DragButton))
	{
		return;
	}
	// A slider's knob follows the mouse along its track while the button is held.
	FButton& Button = Parts->Buttons[DragButton];
	FVector2D Face;
	if (Button.Kind == EPinguMenu3DKind::Slider && TraceFacePoint(Button, ViewportPixel, Face))
	{
		SetSliderFromFace(Button, static_cast<float>(Face.X));
	}
}

void APinguMenu3D::PointerUp()
{
	DragButton = INDEX_NONE;
}

bool APinguMenu3D::IsMouseOnlyPage() const
{
	// (Not while the strip has the keys: Esc / gamepad Start on the island, APenguinHUD::IsLobbyStripFocused.)
	return Page == EPinguMenu3DPage::Lobby && TestPage == EPinguMenu3DPage::None && HUD.IsValid() && HUD->IsLobbyIslandScreen()
		&& !HUD->IsLobbyStripFocused();
}

void APinguMenu3D::FocusDefault()
{
	if (FPageParts* Parts = GetInputParts())
	{
		Parts->Hot = INDEX_NONE;
		Parts->Hot = GetDefaultFocus();
		Parts->bHotByMouse = false;
	}
}

void APinguMenu3D::PollHover()
{
	using namespace PinguMenu3DPrivate;
	FPageParts* Parts = GetInputParts();
	FVector2D Pixel;
	FVector2D Size;
	if (!Parts || !PinguMenu3D::GetViewportPixelSize(GetController(), Size) || !PinguMenu3D::GetCursorViewportPixel(GetWorld(), Pixel))
	{
		Parallax = FVector2D(Approach(Parallax.X, 0.f, 2.f, 0.016f), Approach(Parallax.Y, 0.f, 2.f, 0.016f));
		return;
	}
	const bool bInside = Pixel.X >= 0.f && Pixel.Y >= 0.f && Pixel.X <= Size.X && Pixel.Y <= Size.Y;
	// The parallax follows the cursor over the picture (eased; back to the middle when it is away).
	const FVector2D Want = bInside ? FVector2D(2.f * Pixel.X / Size.X - 1.f, 2.f * Pixel.Y / Size.Y - 1.f) : FVector2D::ZeroVector;
	Parallax = FVector2D(Approach(Parallax.X, Want.X, 2.5f, 0.016f), Approach(Parallax.Y, Want.Y, 2.5f, 0.016f));

	const bool bFirst = LastCursorPixel.X < 0.f;
	const bool bMoved = bFirst || FVector2D::Distance(Pixel, LastCursorPixel) > 0.5f;
	LastCursorPixel = Pixel;
	if (!bInside)
	{
		if (IsMouseOnlyPage() && bMoved)
		{
			Parts->Hot = INDEX_NONE;
		}
		return;
	}
	if (IsMouseOnlyPage())
	{
		// Hover only (the keys are on the island): the button under the cursor is hot, none when it is elsewhere - every frame,
		// so a button that pops out or moves under a resting cursor is noticed.
		Parts->Hot = TraceButtonPixel(Pixel);
		Parts->bHotByMouse = true;
		Parts->bCursorOnHot = Parts->Hot != INDEX_NONE;
		bPointerIn = true;
		PointerUV = Pixel / Size;
		return;
	}
	// A menu page: the cursor moving over a button makes it hot (a keyboard pick stays until the cursor moves). The first
	// sample only remembers where the cursor is (a test's pointer or keyboard pick must not be undone by a resting cursor).
	if (!bMoved || bFirst)
	{
		return;
	}
	const int32 Under = TraceButtonPixel(Pixel);
	if (Under != INDEX_NONE)
	{
		SetHot(Under, false, true);
	}
	else
	{
		Parts->bCursorOnHot = false;
	}
	bPointerIn = true;
	PointerUV = Pixel / Size;
}

bool APinguMenu3D::HasButtonAt(const UWorld* World, const FVector2D& ViewportPixel)
{
	const APinguMenu3D* Menu = Find(World);
	return Menu && Menu->Page != EPinguMenu3DPage::None && Menu->TraceButtonPixel(ViewportPixel) != INDEX_NONE;
}

bool APinguMenu3D::PointerDown(const FVector2D& ViewportUV)
{
	FVector2D Size;
	return PinguMenu3D::GetViewportPixelSize(GetController(), Size) && PointerDownPixel(ViewportUV * Size);
}

bool APinguMenu3D::PointerDownPixel(const FVector2D& ViewportPixel)
{
	FVector2D Size;
	if (PinguMenu3D::GetViewportPixelSize(GetController(), Size))
	{
		PointerUV = ViewportPixel / Size;
	}
	const int32 Under = TraceButtonPixel(ViewportPixel);
	if (Under == INDEX_NONE)
	{
		return false;
	}
	SetHot(Under, false, true);
	FPageParts* Parts = GetInputParts();
	FButton* Button = Parts && Parts->Buttons.IsValidIndex(Under) ? &Parts->Buttons[Under] : nullptr;
	if (Button && Button->Kind == EPinguMenu3DKind::Slider)
	{
		// A slider: the click sets the knob where it landed on the track (and a drag goes on from there); no push-in.
		FVector2D Face;
		if (TraceFacePoint(*Button, ViewportPixel, Face))
		{
			SetSliderFromFace(*Button, static_cast<float>(Face.X));
		}
		DragButton = Under;
		if (HUD.IsValid())
		{
			HUD->PlayUISound(true);
		}
		if (TestAction)
		{
			++ActionsFired;
			TestAction(Button->Id);
		}
		return true;
	}
	if (Button && Button->Kind == EPinguMenu3DKind::Field)
	{
		// A field: the click only puts the typing there (Enter does its action).
		CaretClock = 0.f;
		if (TestAction)
		{
			++ActionsFired;
			TestAction(Button->Id);
		}
		return true;
	}
	Press(Under, false);
	return true;
}

bool APinguMenu3D::Navigate(EUINavigation Direction)
{
	FPageParts* Parts = GetInputParts();
	if (!Parts || Parts->Buttons.Num() == 0)
	{
		return false;
	}
	const TArray<FName> Order = GetFocusOrder();
	if (Direction == EUINavigation::Next || Direction == EUINavigation::Previous)
	{
		const FName Current = GetHotId();
		int32 At = Order.IndexOfByKey(Current);
		At = At == INDEX_NONE ? 0 : (At + (Direction == EUINavigation::Next ? 1 : Order.Num() - 1)) % Order.Num();
		return Order.IsValidIndex(At) && FocusButton(Order[At]);
	}
	if (!Parts->Buttons.IsValidIndex(Parts->Hot))
	{
		Parts->Hot = GetDefaultFocus();
		return Parts->Hot != INDEX_NONE;
	}
	// A row with a value, a slider, a list row: Left / Right step it (the keys do what the arrows do).
	FButton& HotButton = Parts->Buttons[Parts->Hot];
	if (HotButton.OnStep && (Direction == EUINavigation::Left || Direction == EUINavigation::Right))
	{
		HotButton.OnStep(Direction == EUINavigation::Right ? 1 : -1);
		if (HUD.IsValid())
		{
			HUD->PlayUISound(false);
		}
		return true;
	}
	// Inside a list, Up / Down go to the next row (scrolled into view); past its ends they leave the list.
	if (HotButton.ListId >= 0 && (Direction == EUINavigation::Up || Direction == EUINavigation::Down))
	{
		const int32 WantIndex = HotButton.ListIndex + (Direction == EUINavigation::Down ? 1 : -1);
		for (int32 Index = 0; Index < Parts->Buttons.Num(); ++Index)
		{
			FButton& Other = Parts->Buttons[Index];
			if (Other.ListId == HotButton.ListId && Other.ListIndex == WantIndex && Other.Root && (!Other.Enabled || Other.Enabled()))
			{
				ScrollListTo(*Parts, WantIndex);
				SetHot(Index, true);
				return true;
			}
		}
	}
	FVector2D Want(0.f, 0.f);
	switch (Direction)
	{
	case EUINavigation::Up: Want = FVector2D(0.f, 1.f); break;
	case EUINavigation::Down: Want = FVector2D(0.f, -1.f); break;
	case EUINavigation::Left: Want = FVector2D(-1.f, 0.f); break;
	case EUINavigation::Right: Want = FVector2D(1.f, 0.f); break;
	default: return false;
	}
	// The nearest button that way (straight on counts more than across).
	const FVector2D From = Parts->Buttons[Parts->Hot].Center;
	int32 Best = INDEX_NONE;
	float BestScore = TNumericLimits<float>::Max();
	for (int32 Index = 0; Index < Parts->Buttons.Num(); ++Index)
	{
		if (Index == Parts->Hot || !IsButtonLive(Parts->Buttons[Index]) || !Parts->Buttons[Index].bFocusable)
		{
			continue;
		}
		const FVector2D To = Parts->Buttons[Index].Center - From;
		const float Along = FVector2D::DotProduct(To, Want);
		if (Along <= 1.f)
		{
			continue;
		}
		const float Across = FMath::Abs(FVector2D::CrossProduct(Want, To));
		const float Score = Along + 2.f * Across;
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Index;
		}
	}
	if (Best == INDEX_NONE)
	{
		return false;
	}
	SetHot(Best, true);
	return true;
}

bool APinguMenu3D::Accept()
{
	FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Parts->Hot) || !IsButtonLive(Parts->Buttons[Parts->Hot]))
	{
		return false;
	}
	if (Parts->Buttons[Parts->Hot].Kind == EPinguMenu3DKind::Slider)
	{
		return true; // a slider has no press (Left / Right move it)
	}
	Press(Parts->Hot, true);
	return true;
}

void APinguMenu3D::Press(int32 Index, bool bFromKey)
{
	using namespace PinguMenu3DPrivate;
	FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Index) || PendingPress != INDEX_NONE)
	{
		return;
	}
	FButton& Button = Parts->Buttons[Index];
	if (!Button.Action && !TestAction)
	{
		return;
	}
	Button.PushT = 0.f;
	if (HUD.IsValid())
	{
		HUD->PlayUISound(true);
	}
	if (Button.bSign)
	{
		SpawnPuff(Button);
	}
	UE_LOG(LogPingu, Log, TEXT("[MENU3D] press %s (%s)"), *Button.Id.ToString(), bFromKey ? TEXT("key") : TEXT("mouse"));
	if (Button.bInstant)
	{
		FireAction(Button);
		return;
	}
	PendingPress = Index;
	PendingLeft = ActionDelay;
}

void APinguMenu3D::FireAction(FButton& Button)
{
	++ActionsFired;
	if (TestAction)
	{
		TestAction(Button.Id);
		return;
	}
	if (Button.Action)
	{
		Button.Action();
	}
}

bool APinguMenu3D::FocusButton(FName Id)
{
	FPageParts* Parts = GetInputParts();
	if (!Parts)
	{
		return false;
	}
	for (int32 Index = 0; Index < Parts->Buttons.Num(); ++Index)
	{
		if (Parts->Buttons[Index].Id == Id && IsButtonLive(Parts->Buttons[Index]))
		{
			SetHot(Index, false);
			return true;
		}
	}
	return false;
}

FName APinguMenu3D::GetHotId() const
{
	const FPageParts* Parts = GetInputParts();
	return Parts && Parts->Buttons.IsValidIndex(Parts->Hot) ? Parts->Buttons[Parts->Hot].Id : NAME_None;
}

int32 APinguMenu3D::GetButtonCount() const
{
	const FPageParts* Parts = GetInputParts();
	return Parts ? Parts->Buttons.Num() : 0;
}

FPinguMenu3DButtonInfo APinguMenu3D::GetButtonInfo(int32 Index) const
{
	using namespace PinguMenu3DPrivate;
	FPinguMenu3DButtonInfo Info;
	const FPageParts* Parts = GetInputParts();
	if (!Parts || !Parts->Buttons.IsValidIndex(Index))
	{
		return Info;
	}
	const FButton& Button = Parts->Buttons[Index];
	Info.Id = Button.Id;
	Info.Label = Button.Kind == EPinguMenu3DKind::Field ? Button.FieldText : Button.ShownLabel;
	Info.Value = Button.ShownValue;
	Info.bShown = Button.bShown && Button.Root && Button.Root->IsVisible();
	Info.bFocusable = IsButtonLive(Button) && Button.bFocusable;
	Info.bEnabled = !Button.Enabled || Button.Enabled();
	Info.bSelected = Button.Selected && Button.Selected();
	Info.bHot = Index == Parts->Hot;
	// (The rope shows: its first piece in 3D, its 2D part's own visibility in 2D.)
	const bool bRopeShown = Button.Ring.Root && (Button.Ring.Parts[0] ? Button.Ring.Parts[0]->IsVisible() : (Cast<UPinguMenu2DPart>(Button.Ring.Root) && Button.Ring.Root->IsVisible()));
	Info.Outline = bRopeShown ? EaseOut(Button.Focus) : 0.f;
	Info.Tilt = Button.bSign ? HoverTilt * EaseOut(Button.Hover) : 0.f;
	Info.bFocusArrow = FocusArrow && Button.Hit && FocusArrow->GetAttachParent() == Button.Hit && FocusArrow->IsVisible() && FocusArrowIn > 0.5f;
	const USceneComponent* HitSpace = Button.Hit ? Button.Hit : Button.Body;
	Info.Pop = HitSpace ? static_cast<float>(-HitSpace->GetRelativeLocation().X) : 0.f;
	Info.Push = Button.PushT < PushTime ? 1.f - Button.PushT / PushTime : 0.f;
	FVector2D ViewSize;
	if (HitSpace && PinguMenu3D::GetViewportPixelSize(GetController(), ViewSize))
	{
		// The face's middle and its size on the screen (0..1 of the viewport), with the engine's own projection.
		const FTransform& ToWorld = HitSpace->GetComponentTransform();
		FVector2D C;
		FVector2D Lo;
		FVector2D Hi;
		if (ProjectToPixel(ToWorld.TransformPosition(FVector::ZeroVector), C)
			&& ProjectToPixel(ToWorld.TransformPosition(FVector(0.f, -0.5f * Button.Size.X, -0.5f * Button.Size.Y)), Lo)
			&& ProjectToPixel(ToWorld.TransformPosition(FVector(0.f, 0.5f * Button.Size.X, 0.5f * Button.Size.Y)), Hi))
		{
			Info.ScreenCenter = C / ViewSize;
			Info.ScreenSize = FVector2D(FMath::Abs(Hi.X - Lo.X), FMath::Abs(Hi.Y - Lo.Y)) / ViewSize;
		}
	}
	return Info;
}

TArray<FName> APinguMenu3D::GetFocusOrder() const
{
	TArray<FName> Order;
	const FPageParts* Parts = GetInputParts();
	if (!Parts)
	{
		return Order;
	}
	TArray<const FButton*> Live;
	for (const FButton& Button : Parts->Buttons)
	{
		if (IsButtonLive(Button) && Button.bFocusable)
		{
			Live.Add(&Button);
		}
	}
	// Reading order: top to bottom, then left to right.
	Live.Sort([](const FButton& A, const FButton& B)
	{
		if (!FMath::IsNearlyEqual(A.Center.Y, B.Center.Y, 4.f))
		{
			return A.Center.Y > B.Center.Y;
		}
		return A.Center.X < B.Center.X;
	});
	for (const FButton* Button : Live)
	{
		Order.Add(Button->Id);
	}
	return Order;
}

// ============================================================================ the Slate layer

void SPinguMenu3DInput::Construct(const FArguments& InArgs)
{
	HUD = InArgs._HUD;
	// (The wooden card lobby, 2026-10-29: the lobby screen's layer is an ordinary menu layer then.)
	bLobbyIsland = InArgs._bLobbyIsland && PinguMenu3D::UseIslandLobby();
}

bool SPinguMenu3DInput::SupportsKeyboardFocus() const
{
	// The lobby island's layer takes the keys only while the strip has them (else they stay on the game: Tab, Enter, Esc, P...).
	return !bLobbyIsland || (HUD.IsValid() && HUD->IsLobbyStripFocused());
}

APinguMenu3D* SPinguMenu3DInput::GetMenu() const
{
	return HUD.IsValid() ? APinguMenu3D::Get(HUD.Get()) : nullptr;
}

FVector2D SPinguMenu3DInput::ToPixel(const FGeometry& MyGeometry, const FVector2D& ScreenPosition) const
{
	FVector2D Pixel;
	const UWorld* World = HUD.IsValid() ? HUD->GetWorld() : nullptr;
	if (PinguMenu3D::ScreenToViewportPixel(World, ScreenPosition, Pixel))
	{
		return Pixel;
	}
	// No scene viewport (never in a game): this layer fills the viewport, so its own geometry does.
	return PinguMenu3D::PixelFromGeometry(MyGeometry, ScreenPosition);
}

FReply SPinguMenu3DInput::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (APinguMenu3D* Menu = GetMenu())
	{
		if (HasMouseCapture())
		{
			// Held after a press: a slider's knob follows (nothing else changes under a held button).
			Menu->PointerDragPixel(ToPixel(MyGeometry, MouseEvent.GetScreenSpacePosition()));
			return FReply::Handled();
		}
		const FName Before = Menu->GetHotId();
		Menu->PointerMovePixel(ToPixel(MyGeometry, MouseEvent.GetScreenSpacePosition()));
		// Hover takes the focus (as the Slate buttons): the keys go on from where the mouse is.
		if (SupportsKeyboardFocus() && Menu->GetHotId() != Before && !HasAnyUserFocus().IsSet() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetUserFocus(MouseEvent.GetUserIndex(), AsShared(), EFocusCause::Mouse);
		}
	}
	if (bLobbyIsland)
	{
		if (APinguLobby* Lobby = APinguLobby::Get(HUD.IsValid() ? HUD->GetWorld() : nullptr))
		{
			Lobby->HoverAtScreen(ToPixel(MyGeometry, MouseEvent.GetScreenSpacePosition()));
		}
	}
	return FReply::Unhandled();
}

void SPinguMenu3DInput::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	SLeafWidget::OnMouseLeave(MouseEvent);
	if (APinguMenu3D* Menu = GetMenu())
	{
		Menu->PointerLeave();
	}
}

FReply SPinguMenu3DInput::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const bool bLeft = MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton;
	const bool bRight = MouseEvent.GetEffectingButton() == EKeys::RightMouseButton;
	if (!bLeft && !bRight)
	{
		return FReply::Unhandled();
	}
	APinguMenu3D* Menu = GetMenu();
	const FVector2D Pixel = ToPixel(MyGeometry, MouseEvent.GetScreenSpacePosition());
	// A 3D button: pressed on the mouse-down; this layer keeps the mouse until the release (nothing else sees the up).
	if (bLeft && Menu && Menu->PointerDownPixel(Pixel))
	{
		++ClicksTaken;
		FReply Reply = FReply::Handled().CaptureMouse(AsShared());
		if (SupportsKeyboardFocus())
		{
			Reply.SetUserFocus(AsShared(), EFocusCause::Mouse);
		}
		return Reply;
	}
	if (!bLobbyIsland)
	{
		return FReply::Unhandled();
	}
	// The lobby island: the keys back to the island if the strip had them, then the island's own click (a penguin, a tag, an x,
	// the "+"; the right button goes back). Handled either way: the viewport never has to capture the mouse for a click to
	// count, and the keyboard focus goes to the game's viewport (the keys stay on the game).
	++ClicksTaken;
	if (HUD.IsValid() && HUD->IsLobbyStripFocused())
	{
		HUD->SetLobbyStripFocus(false);
	}
	if (APinguLobby* Lobby = APinguLobby::Get(HUD.IsValid() ? HUD->GetWorld() : nullptr))
	{
		Lobby->ClickAtScreen(HUD.IsValid() ? HUD->PlayerOwner.Get() : nullptr, Pixel, bRight);
	}
	FReply Reply = FReply::Handled().CaptureMouse(AsShared());
	const UWorld* World = HUD.IsValid() ? HUD->GetWorld() : nullptr;
	const TSharedPtr<SViewport> ViewportWidget = World && World->GetGameViewport() ? World->GetGameViewport()->GetGameViewportWidget() : nullptr;
	if (ViewportWidget.IsValid())
	{
		Reply.SetUserFocus(ViewportWidget.ToSharedRef(), EFocusCause::Mouse);
	}
	return Reply;
}

FReply SPinguMenu3DInput::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (APinguMenu3D* Menu = GetMenu())
	{
		Menu->PointerUp();
	}
	if (HasMouseCapture())
	{
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FReply SPinguMenu3DInput::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	APinguMenu3D* Menu = GetMenu();
	if (Menu && !bLobbyIsland && Menu->Scroll(MouseEvent.GetWheelDelta()))
	{
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SPinguMenu3DInput::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	// Typed characters go to the hot text field (the LAN screen's name and address).
	APinguMenu3D* Menu = GetMenu();
	if (Menu && !bLobbyIsland && Menu->TypeChar(InCharacterEvent.GetCharacter()))
	{
		return FReply::Handled();
	}
	return SLeafWidget::OnKeyChar(MyGeometry, InCharacterEvent);
}

FReply SPinguMenu3DInput::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	// A fast second click is a click too.
	return OnMouseButtonDown(InMyGeometry, InMouseEvent);
}

FReply SPinguMenu3DInput::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	APinguMenu3D* Menu = GetMenu();
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetNavigationActionFromKey(InKeyEvent) == EUINavigationAction::Accept)
	{
		if (!InKeyEvent.IsRepeat() && Menu)
		{
			Menu->Accept();
		}
		return FReply::Handled();
	}
	// A text field's keys (Backspace, Delete, Ctrl+V; Left / Right stay in the field).
	if (Menu && !bLobbyIsland && Menu->FieldKey(InKeyEvent))
	{
		return FReply::Handled();
	}
	return SLeafWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FNavigationReply SPinguMenu3DInput::OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	const EUINavigation Navigation = InNavigationEvent.GetNavigationType();
	if (Navigation == EUINavigation::Up || Navigation == EUINavigation::Down || Navigation == EUINavigation::Left
		|| Navigation == EUINavigation::Right || Navigation == EUINavigation::Next || Navigation == EUINavigation::Previous)
	{
		if (APinguMenu3D* Menu = GetMenu())
		{
			Menu->Navigate(Navigation);
		}
		return FNavigationReply::Stop();
	}
	return SLeafWidget::OnNavigation(MyGeometry, InNavigationEvent);
}

int32 SPinguMenu3DInput::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	return LayerId; // nothing: the menu is in the world
}

FVector2D SPinguMenu3DInput::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(8.f, 8.f);
}

#undef LOCTEXT_NAMESPACE

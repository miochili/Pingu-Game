// Pingu - the Slate UI: style, crisp widgets (shapes, buttons, option rows, sliders, key caps), the overhead icons.
// The screens are built in PinguUIHud.cpp (root + in-game HUD), PinguUIMenus.cpp and PinguUISettings.cpp.

#include "PinguUI.h"
#include "PinguWoodStyle.h"
#include "Penguin.h"
#include "PinguLobby.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinHUD.h"
#include "PenguinMovementComponent.h"
#include "PenguinTraitComponent.h"
#include "PinguPickup.h"
#include "PinguSettings.h"
#include "PinguTuning.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fonts/CompositeFont.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Styling/WidgetStyle.h"
#include "Textures/SlateShaderResource.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "PinguUI"

namespace PinguUIPrivate
{
	/** Pixels per Slate unit of the UI (SPinguRoot sets it every frame). */
	static float GUIScale = 1.f;
	/** Where custom vertices sample the white texture: its middle (it may sit in an atlas; FPinguStyle::GetWhiteHandle). */
	static FVector2f GWhiteUV(0.5f, 0.5f);

	// ---- ability icons above the heads
	/** Dark disc behind an ability icon. */
	static FLinearColor IconDiscColor() { return PinguUI::Hex(22, 40, 74, 0.9f); }
	/** The dark grey clock over the part of an icon still recharging (the art shows through only faintly). */
	static FLinearColor CooldownShade() { return PinguUI::Hex(24, 26, 32, 0.8f); }
	/** The ring while recharging: dark grey, with the ability colour filling it clockwise as the ability charges. */
	static FLinearColor RingDimColor() { return PinguUI::Hex(64, 68, 80, 0.95f); }
	/** The clock hand: the edge between the charged part and the dark rest. */
	static FLinearColor ClockHandColor() { return PinguUI::Hex(255, 255, 255, 0.95f); }
	/** Fill of a ring drawn with the disc brush: invisible but not 0 (Slate may skip a box with a 0 alpha tint, outline and all). */
	static FLinearColor NoFill() { return FLinearColor(0.f, 0.f, 0.f, 0.001f); }

	/** 0 -> 1 -> 0 over a pop that started Since seconds ago. */
	static float PopEnvelope(float Since, float Duration)
	{
		return Since >= 0.f && Since < Duration ? FMath::Sin(UE_PI * FMath::Clamp(Since / Duration, 0.f, 1.f)) : 0.f;
	}

	static FSlateVertex MakeVertex(const FSlateRenderTransform& ToWindow, const FVector2f& LocalPosition, const FColor& VertexColor)
	{
		FSlateVertex Vertex;
		Vertex.TexCoords[0] = GWhiteUV.X;
		Vertex.TexCoords[1] = GWhiteUV.Y;
		Vertex.TexCoords[2] = 1.f;
		Vertex.TexCoords[3] = 1.f;
		Vertex.MaterialTexCoords = FVector2f(0.5f, 0.5f);
		Vertex.Position = ToWindow.TransformPoint(LocalPosition);
		Vertex.Color = VertexColor;
		Vertex.SecondaryColor = FColor(0, 0, 0, 0);
		Vertex.PixelSize[0] = 0;
		Vertex.PixelSize[1] = 0;
		return Vertex;
	}

	/**
	 * Appends a clock-sweep pie to a triangle list: the part of the circle that is still Covered (0..1), from the
	 * clock hand at (1 - Covered) of a turn clockwise to 12 o'clock. Local units; the rim fades out over Feather.
	 */
	static void AddPie(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& Center, float Radius, float Feather, float Covered, const FLinearColor& PieColor)
	{
		const float Start = (1.f - Covered) * UE_TWO_PI;
		const float Sweep = UE_TWO_PI - Start;
		const int32 Segments = FMath::Clamp(FMath::CeilToInt(Covered * 48.f), 2, 48);
		const FColor Solid = PieColor.ToFColor(true);
		const FColor Clear = PieColor.CopyWithNewOpacity(0.f).ToFColor(true);
		const float Inner = FMath::Max(Radius - Feather * 0.5f, 0.f);
		const float Outer = Radius + Feather * 0.5f;

		const int32 First = Verts.Num();
		Verts.Add(MakeVertex(ToWindow, Center, Solid));
		for (int32 StepIndex = 0; StepIndex <= Segments; ++StepIndex)
		{
			// Clockwise from 12 o'clock (Slate's y axis points down).
			const float Angle = Start + Sweep * (static_cast<float>(StepIndex) / static_cast<float>(Segments));
			const FVector2f Direction(FMath::Sin(Angle), -FMath::Cos(Angle));
			Verts.Add(MakeVertex(ToWindow, Center + Direction * Inner, Solid));
			Verts.Add(MakeVertex(ToWindow, Center + Direction * Outer, Clear));
		}
		for (int32 StepIndex = 0; StepIndex < Segments; ++StepIndex)
		{
			const int32 InnerA = First + 1 + StepIndex * 2;
			const int32 OuterA = InnerA + 1;
			const int32 InnerB = InnerA + 2;
			const int32 OuterB = InnerA + 3;
			const int32 Triangles[] = { First, InnerA, InnerB, InnerA, OuterA, OuterB, InnerA, OuterB, InnerB };
			for (const int32 Corner : Triangles)
			{
				Indices.Add(static_cast<SlateIndex>(Corner));
			}
		}
	}

	/**
	 * Appends a ring arc to a triangle list: between InnerRadius and OuterRadius, from FromTurn to ToTurn (turns clockwise
	 * from 12 o'clock). Both edges of the ring fade out over Feather.
	 */
	static void AddArc(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& Center, float InnerRadius, float OuterRadius, float FromTurn, float ToTurn, float Feather, const FLinearColor& ArcColor)
	{
		const float Turns = ToTurn - FromTurn;
		if (Turns <= 0.f || OuterRadius <= InnerRadius)
		{
			return;
		}
		const int32 Segments = FMath::Clamp(FMath::CeilToInt(Turns * 64.f), 2, 64);
		const FColor Solid = ArcColor.ToFColor(true);
		const FColor Clear = ArcColor.CopyWithNewOpacity(0.f).ToFColor(true);
		const float Middle = 0.5f * (InnerRadius + OuterRadius);
		const float Radii[4] = { FMath::Max(InnerRadius - Feather * 0.5f, 0.f), FMath::Min(InnerRadius + Feather * 0.5f, Middle),
			FMath::Max(OuterRadius - Feather * 0.5f, Middle), OuterRadius + Feather * 0.5f };
		const FColor Colors[4] = { Clear, Solid, Solid, Clear };
		const int32 First = Verts.Num();
		for (int32 StepIndex = 0; StepIndex <= Segments; ++StepIndex)
		{
			const float Angle = (FromTurn + Turns * (static_cast<float>(StepIndex) / static_cast<float>(Segments))) * UE_TWO_PI;
			const FVector2f Direction(FMath::Sin(Angle), -FMath::Cos(Angle));
			for (int32 Ring = 0; Ring < 4; ++Ring)
			{
				Verts.Add(MakeVertex(ToWindow, Center + Direction * Radii[Ring], Colors[Ring]));
			}
		}
		for (int32 StepIndex = 0; StepIndex < Segments; ++StepIndex)
		{
			for (int32 Band = 0; Band < 3; ++Band)
			{
				const int32 A = First + StepIndex * 4 + Band;
				const int32 Quad[] = { A, A + 1, A + 5, A, A + 5, A + 4 };
				for (const int32 Corner : Quad)
				{
					Indices.Add(static_cast<SlateIndex>(Corner));
				}
			}
		}
	}

	/** Appends a straight line From -> To, Width wide, its sides fading out over Feather. */
	static void AddLine(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& From, const FVector2f& To, float Width, float Feather, const FLinearColor& LineColor)
	{
		const FVector2f Along = (To - From).GetSafeNormal();
		if (Along.IsNearlyZero())
		{
			return;
		}
		const FVector2f Side(-Along.Y, Along.X);
		const float Core = FMath::Max(Width - Feather, 0.f) * 0.5f;
		const float Edge = (Width + Feather) * 0.5f;
		const float Offsets[4] = { -Edge, -Core, Core, Edge };
		const FColor Solid = LineColor.ToFColor(true);
		const FColor Clear = LineColor.CopyWithNewOpacity(0.f).ToFColor(true);
		const FColor Colors[4] = { Clear, Solid, Solid, Clear };
		const int32 First = Verts.Num();
		for (const FVector2f& End : { From, To })
		{
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Verts.Add(MakeVertex(ToWindow, End + Side * Offsets[Index], Colors[Index]));
			}
		}
		for (int32 Band = 0; Band < 3; ++Band)
		{
			const int32 A = First + Band;
			const int32 Quad[] = { A, A + 1, A + 5, A, A + 5, A + 4 };
			for (const int32 Corner : Quad)
			{
				Indices.Add(static_cast<SlateIndex>(Corner));
			}
		}
	}

	/** Appends a soft round glow: DiscColor in the middle, fading smoothly to nothing at Radius (local units). */
	static void AddSoftDisc(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& Center, float Radius, const FLinearColor& DiscColor)
	{
		// Rings out from the middle with a smooth (bell-like) fall-off of the opacity.
		static const float RingRadius[] = { 0.f, 0.3f, 0.55f, 0.78f, 1.f };
		static const float RingAlpha[] = { 1.f, 0.88f, 0.55f, 0.2f, 0.f };
		constexpr int32 NumRings = UE_ARRAY_COUNT(RingRadius);
		constexpr int32 Segments = 40;
		const int32 First = Verts.Num();
		Verts.Add(MakeVertex(ToWindow, Center, DiscColor.ToFColor(true)));
		for (int32 Ring = 1; Ring < NumRings; ++Ring)
		{
			const FColor RingColor = DiscColor.CopyWithNewOpacity(DiscColor.A * RingAlpha[Ring]).ToFColor(true);
			for (int32 StepIndex = 0; StepIndex < Segments; ++StepIndex)
			{
				const float Angle = UE_TWO_PI * static_cast<float>(StepIndex) / static_cast<float>(Segments);
				Verts.Add(MakeVertex(ToWindow, Center + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * (Radius * RingRadius[Ring]), RingColor));
			}
		}
		// The middle fan, then a band of quads between each two rings.
		for (int32 StepIndex = 0; StepIndex < Segments; ++StepIndex)
		{
			const int32 Next = (StepIndex + 1) % Segments;
			Indices.Add(static_cast<SlateIndex>(First));
			Indices.Add(static_cast<SlateIndex>(First + 1 + StepIndex));
			Indices.Add(static_cast<SlateIndex>(First + 1 + Next));
		}
		for (int32 Ring = 1; Ring + 1 < NumRings; ++Ring)
		{
			const int32 Inner = First + 1 + (Ring - 1) * Segments;
			const int32 Outer = Inner + Segments;
			for (int32 StepIndex = 0; StepIndex < Segments; ++StepIndex)
			{
				const int32 Next = (StepIndex + 1) % Segments;
				const int32 Quad[] = { Inner + StepIndex, Outer + StepIndex, Outer + Next, Inner + StepIndex, Outer + Next, Inner + Next };
				for (const int32 Corner : Quad)
				{
					Indices.Add(static_cast<SlateIndex>(Corner));
				}
			}
		}
	}

	/** Appends a four-pointed glint: bright in the middle, its tips fading to nothing (Radius: the tips' distance, Turn: its
	 *  rotation in radians). */
	static void AddSparkle(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& Center, float Radius, float Turn, const FLinearColor& SparkleColor)
	{
		const FColor Solid = SparkleColor.ToFColor(true);
		const FColor Soft = SparkleColor.CopyWithNewOpacity(SparkleColor.A * 0.55f).ToFColor(true);
		const FColor Clear = SparkleColor.CopyWithNewOpacity(0.f).ToFColor(true);
		const int32 First = Verts.Num();
		Verts.Add(MakeVertex(ToWindow, Center, Solid));
		for (int32 Point = 0; Point < 4; ++Point)
		{
			const float TipAngle = Turn + static_cast<float>(Point) * UE_HALF_PI;
			const float WaistAngle = TipAngle + 0.25f * UE_PI;
			Verts.Add(MakeVertex(ToWindow, Center + FVector2f(FMath::Cos(TipAngle), FMath::Sin(TipAngle)) * Radius, Clear));
			Verts.Add(MakeVertex(ToWindow, Center + FVector2f(FMath::Cos(WaistAngle), FMath::Sin(WaistAngle)) * Radius * 0.24f, Soft));
		}
		for (int32 Point = 0; Point < 4; ++Point)
		{
			const int32 Tip = First + 1 + Point * 2;
			const int32 Waist = Tip + 1;
			const int32 PreviousWaist = First + 1 + ((Point + 3) % 4) * 2 + 1;
			const int32 Triangles[] = { First, PreviousWaist, Tip, First, Tip, Waist };
			for (const int32 Corner : Triangles)
			{
				Indices.Add(static_cast<SlateIndex>(Corner));
			}
		}
	}

	/** Appends a little tilted square (a chip of a crumbling icon). */
	static void AddCrumb(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& Center, float HalfSize, float Turn, const FLinearColor& CrumbColor)
	{
		const FColor Solid = CrumbColor.ToFColor(true);
		const int32 First = Verts.Num();
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			const float Angle = Turn + 0.25f * UE_PI + static_cast<float>(Corner) * UE_HALF_PI;
			Verts.Add(MakeVertex(ToWindow, Center + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * HalfSize * 1.4142f, Solid));
		}
		const int32 Triangles[] = { First, First + 1, First + 2, First, First + 2, First + 3 };
		for (const int32 Corner : Triangles)
		{
			Indices.Add(static_cast<SlateIndex>(Corner));
		}
	}

	/** The top of a penguin's head standing up: the icons and name tags stay there while it lies low on its belly too. */
	static FVector StandingHeadTop(const APenguinCharacter* Penguin)
	{
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 56.f;
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const float Standing = Move ? FMath::Max(Move->GetStandingHalfHeight(), HalfHeight) : HalfHeight;
		return Penguin->GetActorLocation() + FVector(0.f, 0.f, 2.f * Standing - HalfHeight); // feet + the standing height
	}

	/** A corner moved along its bisector so the triangle's edges move by Distance (positive = outward). */
	static FVector2f OffsetCorner(const FVector2f& Corner, const FVector2f& Prev, const FVector2f& Next, float Distance)
	{
		const FVector2f ToPrev = (Prev - Corner).GetSafeNormal();
		const FVector2f ToNext = (Next - Corner).GetSafeNormal();
		const FVector2f Inward = (ToPrev + ToNext).GetSafeNormal();
		const float SinHalf = FMath::Max(FMath::Abs(FVector2f::CrossProduct(ToPrev, Inward)), 0.2f);
		return Corner - Inward * (Distance / SinHalf);
	}
}

// ============================================================================ helpers

namespace PinguUI
{
	double Now()
	{
		return FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetCurrentTime() : FPlatformTime::Seconds();
	}

	float EaseOut(float Alpha)
	{
		const float T = 1.f - FMath::Clamp(Alpha, 0.f, 1.f);
		return 1.f - T * T * T;
	}

	float GetUIScale()
	{
		return PinguUIPrivate::GUIScale;
	}

	void SetUIScale(float Scale)
	{
		PinguUIPrivate::GUIScale = FMath::Max(Scale, 0.1f);
	}

	float SnapUnits(float Units)
	{
		const float Scale = GetUIScale();
		return FMath::RoundToFloat(Units * Scale) / Scale;
	}

	FText Upper(const FString& Text)
	{
		return FText::FromString(Text.ToUpper());
	}

	FText MenuKeysText()
	{
		TArray<FString> Labels;
		if (!GIsEditor)
		{
			Labels.Add(TEXT("ESC"));
		}
		if (const UPinguSettings* Settings = UPinguSettings::Get())
		{
			for (const FKey& Key : Settings->GetKeys(EPinguInputAction::Pause))
			{
				Labels.AddUnique(UPinguSettings::GetKeyLabel(Key).ToString());
			}
		}
		if (Labels.Num() == 0)
		{
			Labels.Add(TEXT("ESC"));
		}
		return FText::FromString(FString::Join(Labels, TEXT(" / ")));
	}

	FLinearColor Hex(uint8 R, uint8 G, uint8 B, float A)
	{
		FLinearColor Color = FLinearColor::FromSRGBColor(FColor(R, G, B));
		Color.A = A;
		return Color;
	}

	FLinearColor WithAlpha(const FLinearColor& Color, float Alpha)
	{
		return Color.CopyWithNewOpacity(Alpha);
	}

	void AddTriangle(TArray<FSlateVertex>& Verts, TArray<SlateIndex>& Indices, const FSlateRenderTransform& ToWindow,
		const FVector2f& A, const FVector2f& B, const FVector2f& C, const FLinearColor& Color, float Feather)
	{
		using namespace PinguUIPrivate;
		const FVector2f Corners[3] = { A, B, C };
		const FColor Solid = Color.ToFColor(true);
		const FColor Clear = Color.CopyWithNewOpacity(0.f).ToFColor(true);
		const int32 First = Verts.Num();
		// 0..2: the solid core (edges pulled in by half the feather), 3..5: the clear rim (pushed out by half).
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Verts.Add(MakeVertex(ToWindow, OffsetCorner(Corners[Index], Corners[(Index + 2) % 3], Corners[(Index + 1) % 3], -0.5f * Feather), Solid));
		}
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Verts.Add(MakeVertex(ToWindow, OffsetCorner(Corners[Index], Corners[(Index + 2) % 3], Corners[(Index + 1) % 3], 0.5f * Feather), Clear));
		}
		const int32 Triangles[] = {
			0, 1, 2,
			0, 3, 4, 0, 4, 1,
			1, 4, 5, 1, 5, 2,
			2, 5, 3, 2, 3, 0 };
		for (const int32 Corner : Triangles)
		{
			Indices.Add(static_cast<SlateIndex>(First + Corner));
		}
	}
}

// ============================================================================ style

const FPinguStyle& FPinguStyle::Get()
{
	static TUniquePtr<FPinguStyle> Instance;
	if (!Instance)
	{
		Instance = TUniquePtr<FPinguStyle>(new FPinguStyle());
	}
	return *Instance;
}

FPinguStyle::FPinguStyle()
	: Ink(PinguUI::Hex(11, 23, 48))
	, Panel(PinguUI::Hex(18, 38, 74, 0.96f))
	, PanelDeep(PinguUI::Hex(10, 24, 50))
	, Row(PinguUI::Hex(28, 56, 104))
	, RowHot(PinguUI::Hex(40, 77, 138))
	, Blue(PinguUI::Hex(47, 107, 192))
	, BlueHot(PinguUI::Hex(72, 138, 226))
	, BluePressed(PinguUI::Hex(37, 88, 163))
	, BlueLip(PinguUI::Hex(24, 58, 116))
	, Orange(PinguUI::Hex(255, 138, 31))
	, OrangeHot(PinguUI::Hex(255, 165, 74))
	, OrangePressed(PinguUI::Hex(232, 115, 12))
	, OrangeLip(PinguUI::Hex(178, 80, 8))
	, Red(PinguUI::Hex(229, 72, 77))
	, RedHot(PinguUI::Hex(244, 101, 105))
	, RedPressed(PinguUI::Hex(204, 55, 61))
	, RedLip(PinguUI::Hex(140, 34, 40))
	, Snow(PinguUI::Hex(244, 250, 255))
	, Ice(PinguUI::Hex(159, 227, 255))
	, IceDim(PinguUI::Hex(124, 160, 204))
	, Gold(PinguUI::Hex(255, 201, 60))
	, Green(PinguUI::Hex(95, 211, 122))
	, Disabled(PinguUI::Hex(48, 62, 88))
	, Ring(PinguUI::Hex(244, 250, 255))
	, Dim(FLinearColor(0.f, 0.f, 0.f, 0.5f))
	, CircleBrush(FLinearColor::White, 40.f)
{
	WhiteBrush = FSlateColorBrush(FLinearColor::White);
	NoBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
	// A true circle at every size: with a fixed radius, small circles had nothing left to draw.
	CircleBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;

	// Headings, buttons and numbers: Titan One (2026-10-24, Linus's pick after Rammetto One; the 3D menu's letters are the same
	// font), Lilita One if it is missing. Long texts stay in the rounder, more readable Sniglet (Body()).
	TitleFontPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("Slate/Fonts/TitanOne-Regular.ttf"));
	bTitleWide = FPaths::FileExists(TitleFontPath);
	if (!bTitleWide)
	{
		TitleFontPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("Slate/Fonts/LilitaOne-Regular.ttf"));
	}
	BodyFontPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("Slate/Fonts/Sniglet-ExtraBold.ttf"));
	if (FPaths::FileExists(TitleFontPath))
	{
		TitleFont = MakeShared<FStandaloneCompositeFont>(NAME_None, TitleFontPath, EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
	}
	if (FPaths::FileExists(BodyFontPath))
	{
		BodyFont = MakeShared<FStandaloneCompositeFont>(NAME_None, BodyFontPath, EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
	}

	// Text boxes: the text only; an SPinguShape behind them draws the box (it fades with the screen).
	TextBox = FEditableTextBoxStyle::GetDefault();
	FTextBlockStyle TextBoxText = TextBox.TextStyle;
	// Not Body(): that goes through Get(), and the one style object does not exist yet while it is being built.
	FSlateFontInfo TextBoxFont = BodyFont.IsValid() ? FSlateFontInfo(BodyFont, 20.f) : FCoreStyle::GetDefaultFontStyle("Bold", 20.f);
	TextBoxFont.OutlineSettings.OutlineSize = 0;
	TextBoxFont.OutlineSettings.OutlineColor = Ink;
	TextBoxText.SetFont(TextBoxFont);
	TextBoxText.SetColorAndOpacity(FSlateColor(Snow));
	TextBoxText.SetSelectedBackgroundColor(FSlateColor(PinguUI::WithAlpha(Ice, 0.45f)));
	TextBox.SetTextStyle(TextBoxText)
		.SetBackgroundImageNormal(NoBrush)
		.SetBackgroundImageHovered(NoBrush)
		.SetBackgroundImageFocused(NoBrush)
		.SetBackgroundImageReadOnly(NoBrush)
		.SetPadding(FMargin(16.f, 10.f))
		.SetForegroundColor(FSlateColor(Snow))
		.SetFocusedForegroundColor(FSlateColor(Snow))
		.SetReadOnlyForegroundColor(FSlateColor(IceDim))
		.SetBackgroundColor(FSlateColor(FLinearColor::White));
}

FSlateFontInfo FPinguStyle::Title(float Size, int32 OutlineSize)
{
	const FPinguStyle& Style = Get();
	// Titan One is wider than Lilita One (about x1.18 at the same size): drawn at TitleWideScale of the size asked for,
	// so every label keeps about the width it was laid out for (its capitals a little smaller), and it needs no extra tracking.
	const float Drawn = Style.bTitleWide ? FMath::RoundToFloat(Size * TitleWideScale) : Size;
	FSlateFontInfo Font = Style.TitleFont.IsValid() ? FSlateFontInfo(Style.TitleFont, Drawn) : FCoreStyle::GetDefaultFontStyle("Bold", Drawn);
	Font.OutlineSettings.OutlineSize = OutlineSize;
	Font.OutlineSettings.OutlineColor = Style.Ink;
	// A little tracking on small caps labels, none on big titles.
	Font.LetterSpacing = Size <= 24.f ? (Style.bTitleWide ? 20 : 40) : 0;
	return Font;
}

FSlateFontInfo FPinguStyle::Body(float Size, int32 OutlineSize)
{
	const FPinguStyle& Style = Get();
	FSlateFontInfo Font = Style.BodyFont.IsValid() ? FSlateFontInfo(Style.BodyFont, Size) : FCoreStyle::GetDefaultFontStyle("Bold", Size);
	Font.OutlineSettings.OutlineSize = OutlineSize;
	Font.OutlineSettings.OutlineColor = Style.Ink;
	return Font;
}

const FSlateResourceHandle& FPinguStyle::GetWhiteHandle() const
{
	// Resolving a handle is not cheap: once it works (a few tries while Slate loads its images). A colour brush has no
	// texture - no handle at all - so the core style's white image; the vertices sample its middle (it may sit in an atlas).
	if (!bWhiteHandleResolved && WhiteHandleTries < 120 && FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			++WhiteHandleTries;
			const FSlateBrush* Box = FCoreStyle::Get().GetBrush("GenericWhiteBox");
			WhiteHandle = Renderer->GetResourceHandle(Box ? *Box : WhiteBrush, FVector2f(0.f, 0.f), 1.f);
			if (const FSlateShaderResourceProxy* Proxy = WhiteHandle.GetResourceProxy())
			{
				PinguUIPrivate::GWhiteUV = Proxy->StartUV + Proxy->SizeUV * 0.5f;
				bWhiteHandleResolved = true;
				UE_LOG(LogPingu, Log, TEXT("[UI] white texture for the shapes: ok (uv %.3f %.3f)"), PinguUIPrivate::GWhiteUV.X, PinguUIPrivate::GWhiteUV.Y);
			}
		}
	}
	return WhiteHandle;
}

// ============================================================================ shapes

void SPinguShape::Construct(const FArguments& InArgs)
{
	FillColorAttr = InArgs._FillColor;
	OutlineColorAttr = InArgs._OutlineColor;
	OutlineWidthAttr = InArgs._OutlineWidth;
	ShapeBrush.OutlineSettings.CornerRadii = FVector4(InArgs._Radius, InArgs._Radius, InArgs._Radius, InArgs._Radius);
	ShapeBrush.OutlineSettings.RoundingType = InArgs._Pill ? ESlateBrushRoundingType::HalfHeightRadius : ESlateBrushRoundingType::FixedRadius;
	bWood = InArgs._Wood;
	bPill = InArgs._Pill;
	bBanner = InArgs._Banner;
}

FVector2D SPinguShape::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D::ZeroVector; // fills its slot
}

int32 SPinguShape::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();
	FLinearColor Fill = FillColorAttr.Get() * Tint;
	const FLinearColor Line = OutlineColorAttr.Get() * Tint;
	const float LineWidth = OutlineWidthAttr.Get();
	const bool bLine = LineWidth > 0.f && Line.A > 0.002f;
	if (Fill.A <= 0.002f && !bLine)
	{
		return LayerId;
	}
	if (bWood)
	{
		// Style 5 (docs/UI_STYLE.md): a pill plank for tags, else a panel board (stacked boards when tall) with its snow and nails.
		PinguWood::FPlankLook Look;
		Look.Cut = bBanner ? PinguWood::ECut::Banner : bPill ? PinguWood::ECut::Pill : PinguWood::ECut::Board;
		Look.Opacity = FillColorAttr.Get().A * Tint.A;
		const FVector2f Size = AllottedGeometry.GetLocalSize();
		Look.Seed = static_cast<uint32>(Size.X * 13.f + Size.Y * 7.f);
		if (PinguWood::PaintPlank(OutDrawElements, LayerId, AllottedGeometry, FVector2f(0.f, 0.f), Size, Look))
		{
			return LayerId;
		}
	}
	if (Fill.A <= 0.002f)
	{
		Fill = PinguUIPrivate::NoFill();
	}
	// The outline does not take the draw tint: its colour already has the widget opacity in it.
	ShapeBrush.OutlineSettings.Color = FSlateColor(Line);
	ShapeBrush.OutlineSettings.Width = bLine ? LineWidth : 0.f;
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), &ShapeBrush, ESlateDrawEffect::None, Fill);
	return LayerId;
}

void SPinguArrow::Construct(const FArguments& InArgs)
{
	Direction = InArgs._Direction;
	ArrowSize = InArgs._Size;
	ColorAttr = InArgs._Color;
}

FVector2D SPinguArrow::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(ArrowSize, ArrowSize);
}

int32 SPinguArrow::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FLinearColor Color = ColorAttr.Get() * InWidgetStyle.GetColorAndOpacityTint();
	const FSlateResourceHandle& White = FPinguStyle::Get().GetWhiteHandle(); // (invalid: plain vertex colours)
	if (Color.A <= 0.002f)
	{
		return LayerId;
	}
	const FVector2f Size = AllottedGeometry.GetLocalSize();
	const float Half = 0.5f * FMath::Min(Size.X, Size.Y);
	const FVector2f Center = Size * 0.5f;
	FVector2f A, B, C;
	if (Direction < 0)
	{
		A = Center + FVector2f(-0.8f * Half, 0.f);
		B = Center + FVector2f(0.6f * Half, -Half);
		C = Center + FVector2f(0.6f * Half, Half);
	}
	else if (Direction > 0)
	{
		A = Center + FVector2f(0.8f * Half, 0.f);
		B = Center + FVector2f(-0.6f * Half, Half);
		C = Center + FVector2f(-0.6f * Half, -Half);
	}
	else
	{
		A = Center + FVector2f(0.f, 0.8f * Half);
		B = Center + FVector2f(-Half, -0.6f * Half);
		C = Center + FVector2f(Half, -0.6f * Half);
	}
	const float PixelsPerUnit = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
	Verts.Reset();
	Indices.Reset();
	PinguUI::AddTriangle(Verts, Indices, AllottedGeometry.GetAccumulatedRenderTransform(), A, B, C, Color, 1.2f / FMath::Max(PixelsPerUnit, 0.01f));
	FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, White, Verts, Indices, nullptr, 0, 0, ESlateDrawEffect::None, ESlateBatchDrawFlag::None);
	return LayerId;
}

// ============================================================================ button

void SPinguButton::Construct(const FArguments& InArgs)
{
	const FPinguStyle& Style = FPinguStyle::Get();
	HUD = InArgs._HUD;
	ConstructTime = PinguUI::Now();
	Kind = InArgs._Kind;
	bFocusable = InArgs._IsFocusable && Kind != EPinguButtonKind::Arrow;
	OnClickedDelegate = InArgs._OnClicked;
	OnRightClickedDelegate = InArgs._OnRightClicked;
	OnStepDelegate = InArgs._OnStep;
	OnFocusedDelegate = InArgs._OnFocused;
	SelectedAttr = InArgs._IsSelected;
	LabelPadding = InArgs._ContentPadding;

	const bool bFlat = Kind == EPinguButtonKind::Row || Kind == EPinguButtonKind::Ghost || Kind == EPinguButtonKind::Arrow;
	const float Radius = Kind == EPinguButtonKind::Row || Kind == EPinguButtonKind::KeyCap ? FPinguStyle::RowRadius
		: (Kind == EPinguButtonKind::Arrow ? 8.f : FPinguStyle::ButtonRadius);
	const float Ring = FPinguStyle::RingWidth + FPinguStyle::RingGap;

	TSharedRef<SWidget> Content = InArgs._Content.Widget;
	if (Content == SNullWidget::NullWidget)
	{
		// Buttons on a coloured face get a hard navy drop shadow (crisp, no blur).
		const bool bShadow = Kind == EPinguButtonKind::Primary || Kind == EPinguButtonKind::Normal || Kind == EPinguButtonKind::Danger;
		Content = SNew(STextBlock)
			.Text(InArgs._Text)
			.Font(FPinguStyle::Title(InArgs._FontSize))
			.ColorAndOpacity_Lambda([this]() { return FSlateColor(GetLabelColor()); })
			.ShadowOffset(bShadow ? FVector2D(0.f, 2.f) : FVector2D::ZeroVector)
			.ShadowColorAndOpacity(bShadow ? PinguUI::WithAlpha(Style.Ink, 0.85f) : FLinearColor::Transparent);
	}

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(InArgs._Width > 0.f ? FOptionalSize(InArgs._Width) : FOptionalSize())
		.HeightOverride(InArgs._Height)
		[
			SNew(SOverlay)
			// Focus ring: outside the button, so it never changes the layout.
			+ SOverlay::Slot()
			.Padding(-Ring)
			[
				SNew(SPinguShape)
				.FillColor(FLinearColor::Transparent)
				.OutlineColor_Lambda([this]() { return HasFocusRing() ? FPinguStyle::Get().Ring : FLinearColor::Transparent; })
				.OutlineWidth(FPinguStyle::RingWidth)
				.Radius(Radius + Ring)
			]
			// The lip: the darker underside the face stands on (it sinks onto it when pressed).
			+ SOverlay::Slot()
			[
				SNew(SPinguShape)
				.Visibility(bFlat ? EVisibility::Collapsed : EVisibility::HitTestInvisible)
				.FillColor_Lambda([this]() { return GetLipColor(); })
				.OutlineColor(Style.Ink)
				.OutlineWidth(FPinguStyle::LineWidth)
				.Radius(Radius)
			]
			// The face.
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Padding_Lambda([this]() { return GetFacePadding(); })
				[
					SNew(SPinguShape)
					.FillColor_Lambda([this]() { return GetFaceColor(); })
					.OutlineColor_Lambda([this]() { return GetFaceOutline(); })
					.OutlineWidth(FPinguStyle::LineWidth)
					.Radius(Radius)
				]
			]
			// The label / content moves with the face.
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Padding_Lambda([this]() { return GetContentSlotPadding(); })
				.HAlign(InArgs._HAlign)
				.VAlign(VAlign_Center)
				[
					Content
				]
			]
		]
	];
}

bool SPinguButton::SupportsKeyboardFocus() const
{
	return bFocusable && IsEnabled();
}

bool SPinguButton::IsInteractable() const
{
	return IsEnabled();
}

bool SPinguButton::IsHot() const
{
	return IsEnabled() && (IsHovered() || HasAnyUserFocus().IsSet());
}

bool SPinguButton::HasFocusRing() const
{
	return bFocusable && IsEnabled() && HasAnyUserFocus().IsSet();
}

bool SPinguButton::LooksPressed() const
{
	return IsEnabled() && ((bMouseHeld && IsHovered()) || bKeyHeld || PinguUI::Now() - PressTime < 0.09);
}

FMargin SPinguButton::GetFacePadding() const
{
	const bool bLip = Kind == EPinguButtonKind::Primary || Kind == EPinguButtonKind::Normal || Kind == EPinguButtonKind::Danger
		|| Kind == EPinguButtonKind::KeyCap || Kind == EPinguButtonKind::Segment;
	if (!bLip)
	{
		return FMargin(0.f);
	}
	// Whole units: the face sits LipHeight above the bottom and sinks 3 units when pressed.
	return LooksPressed() ? FMargin(0.f, FPinguStyle::LipHeight - 1.f, 0.f, 1.f) : FMargin(0.f, 0.f, 0.f, FPinguStyle::LipHeight);
}

FMargin SPinguButton::GetContentSlotPadding() const
{
	const FMargin Face = GetFacePadding();
	return FMargin(LabelPadding.Left + Face.Left, LabelPadding.Top + Face.Top, LabelPadding.Right + Face.Right, LabelPadding.Bottom + Face.Bottom);
}

FLinearColor SPinguButton::GetFaceColor() const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	if (!IsEnabled())
	{
		return Kind == EPinguButtonKind::Ghost || Kind == EPinguButtonKind::Arrow ? FLinearColor::Transparent : Style.Disabled;
	}
	const bool bHot = IsHot();
	const bool bPressed = LooksPressed();
	switch (Kind)
	{
	case EPinguButtonKind::Primary:
		return bPressed ? Style.OrangePressed : (bHot ? Style.OrangeHot : Style.Orange);
	case EPinguButtonKind::Danger:
		return bPressed ? Style.RedPressed : (bHot ? Style.RedHot : Style.Red);
	case EPinguButtonKind::Ghost:
		return bPressed ? PinguUI::WithAlpha(Style.Ice, 0.22f) : (bHot ? PinguUI::WithAlpha(Style.Ice, 0.14f) : FLinearColor::Transparent);
	case EPinguButtonKind::Row:
		return bPressed ? Style.Row : (bHot ? Style.RowHot : Style.Row);
	case EPinguButtonKind::KeyCap:
		if (SelectedAttr.Get())
		{
			return Style.Orange; // waiting for a key
		}
		return bPressed ? Style.Ice : (bHot ? FLinearColor::White : Style.Snow);
	case EPinguButtonKind::Arrow:
		return bPressed ? PinguUI::WithAlpha(Style.Ice, 0.3f) : (bHot ? PinguUI::WithAlpha(Style.Ice, 0.18f) : FLinearColor::Transparent);
	case EPinguButtonKind::Segment:
		if (SelectedAttr.Get())
		{
			return bPressed ? Style.OrangePressed : (bHot ? Style.OrangeHot : Style.Orange);
		}
		return bPressed ? Style.BluePressed : (bHot ? Style.RowHot : Style.Row);
	case EPinguButtonKind::Normal:
	default:
		return bPressed ? Style.BluePressed : (bHot ? Style.BlueHot : Style.Blue);
	}
}

FLinearColor SPinguButton::GetLipColor() const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	if (!IsEnabled())
	{
		return Style.PanelDeep;
	}
	switch (Kind)
	{
	case EPinguButtonKind::Primary: return Style.OrangeLip;
	case EPinguButtonKind::Danger: return Style.RedLip;
	case EPinguButtonKind::KeyCap: return SelectedAttr.Get() ? Style.OrangeLip : Style.IceDim;
	case EPinguButtonKind::Segment: return SelectedAttr.Get() ? Style.OrangeLip : Style.PanelDeep;
	default: return Style.BlueLip;
	}
}

FLinearColor SPinguButton::GetFaceOutline() const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	switch (Kind)
	{
	case EPinguButtonKind::Ghost:
		return IsEnabled() ? PinguUI::WithAlpha(Style.Ice, IsHot() ? 1.f : 0.55f) : PinguUI::WithAlpha(Style.IceDim, 0.4f);
	case EPinguButtonKind::Row:
	case EPinguButtonKind::Arrow:
		return FLinearColor::Transparent;
	default:
		return Style.Ink;
	}
}

FLinearColor SPinguButton::GetLabelColor() const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	if (!IsEnabled())
	{
		return PinguUI::WithAlpha(Style.IceDim, 0.7f);
	}
	switch (Kind)
	{
	case EPinguButtonKind::KeyCap: return SelectedAttr.Get() ? Style.Snow : Style.Ink;
	case EPinguButtonKind::Ghost: return IsHot() ? Style.Snow : Style.Ice;
	case EPinguButtonKind::Segment: return SelectedAttr.Get() || IsHot() ? Style.Snow : Style.Ice;
	default: return Style.Snow;
	}
}

void SPinguButton::Press()
{
	PressTime = PinguUI::Now();
	if (HUD.IsValid())
	{
		HUD->PlayUISound(true);
	}
	OnClickedDelegate.ExecuteIfBound();
}

void SPinguButton::Step(int32 Direction, bool bPlaySound)
{
	if (!IsEnabled() || !OnStepDelegate.IsBound())
	{
		return;
	}
	PressTime = PinguUI::Now();
	if (bPlaySound && HUD.IsValid())
	{
		HUD->PlayUISound(false);
	}
	OnStepDelegate.Execute(Direction);
}

FReply SPinguButton::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	// A tick when the player moves onto it (not when a screen opens with the focus already here).
	const EFocusCause Cause = InFocusEvent.GetCause();
	if ((Cause == EFocusCause::Navigation || Cause == EFocusCause::Mouse) && HUD.IsValid())
	{
		HUD->PlayUISound(false);
	}
	OnFocusedDelegate.ExecuteIfBound();
	return SCompoundWidget::OnFocusReceived(MyGeometry, InFocusEvent);
}

void SPinguButton::OnFocusLost(const FFocusEvent& InFocusEvent)
{
	bKeyHeld = false;
	SCompoundWidget::OnFocusLost(InFocusEvent);
}

void SPinguButton::OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	SCompoundWidget::OnMouseEnter(MyGeometry, MouseEvent);
	// Hover takes the focus: one highlighted control at a time, and the keys go on from where the mouse is.
	// Never out of a text box the player is typing in.
	if (!SupportsKeyboardFocus() || HasAnyUserFocus().IsSet() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(MouseEvent.GetUserIndex());
	if (Focused.IsValid() && Focused->GetType() == FName(TEXT("SEditableText")))
	{
		return;
	}
	Slate.SetUserFocus(MouseEvent.GetUserIndex(), AsShared(), EFocusCause::Mouse);
}

void SPinguButton::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	SCompoundWidget::OnMouseLeave(MouseEvent);
	bMouseHeld = false;
}

FReply SPinguButton::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton && OnRightClickedDelegate.IsBound())
	{
		if (IsEnabled())
		{
			PressTime = PinguUI::Now();
			if (HUD.IsValid())
			{
				HUD->PlayUISound(true);
			}
			OnRightClickedDelegate.Execute();
		}
		return FReply::Handled();
	}
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	if (IsEnabled())
	{
		bMouseHeld = true;
		Press(); // on press, not on release: snappy
	}
	return FReply::Handled();
}

FReply SPinguButton::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	// A quick second click arrives as a double click: it is just another press (option arrows clicked fast).
	// A widget younger than 0.6 s cannot be the target of a deliberate double click: it is the second click of a
	// double click that rebuilt the screen (lobby cards after + or X), so it must not press the new widget.
	if (PinguUI::Now() - ConstructTime < 0.6)
	{
		return FReply::Handled();
	}
	return OnMouseButtonDown(InMyGeometry, InMouseEvent);
}

FReply SPinguButton::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && bMouseHeld)
	{
		bMouseHeld = false;
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SPinguButton::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetNavigationActionFromKey(InKeyEvent) == EUINavigationAction::Accept)
	{
		if (!InKeyEvent.IsRepeat() && IsEnabled())
		{
			bKeyHeld = true;
			Press();
		}
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SPinguButton::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetNavigationActionFromKey(InKeyEvent) == EUINavigationAction::Accept)
	{
		bKeyHeld = false;
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyUp(MyGeometry, InKeyEvent);
}

FNavigationReply SPinguButton::OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	// Option rows: left / right (arrow keys, d-pad, stick) change the value instead of moving the focus.
	const EUINavigation Navigation = InNavigationEvent.GetNavigationType();
	if (OnStepDelegate.IsBound() && (Navigation == EUINavigation::Left || Navigation == EUINavigation::Right))
	{
		Step(Navigation == EUINavigation::Left ? -1 : 1);
		return FNavigationReply::Stop();
	}
	return SCompoundWidget::OnNavigation(MyGeometry, InNavigationEvent);
}

// ============================================================================ slider

void SPinguSlider::Construct(const FArguments& InArgs)
{
	ValueAttr = InArgs._Value;
	HotAttr = InArgs._IsHot;
	StepSize = FMath::Max(InArgs._Step, 0.01f);
	OnValueChangedDelegate = InArgs._OnValueChanged;
	TrackBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
	KnobBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
}

FVector2D SPinguSlider::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(160.f, 24.f);
}

void SPinguSlider::SetFromPointer(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	constexpr float Knob = 24.f;
	const FVector2f Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const float Usable = FMath::Max(static_cast<float>(MyGeometry.GetLocalSize().X) - Knob, 1.f);
	float NewValue = FMath::Clamp((Local.X - 0.5f * Knob) / Usable, 0.f, 1.f);
	NewValue = FMath::Clamp(FMath::RoundToFloat(NewValue / StepSize) * StepSize, 0.f, 1.f);
	if (!FMath::IsNearlyEqual(NewValue, ValueAttr.Get(), 0.001f))
	{
		OnValueChangedDelegate.ExecuteIfBound(NewValue);
	}
}

FReply SPinguSlider::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !IsEnabled())
	{
		return FReply::Unhandled();
	}
	bDragging = true;
	SetFromPointer(MyGeometry, MouseEvent);
	return FReply::Handled().CaptureMouse(AsShared());
}

FReply SPinguSlider::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bDragging)
	{
		return FReply::Unhandled();
	}
	if (!HasMouseCapture())
	{
		bDragging = false; // the capture went away (window lost focus)
		return FReply::Unhandled();
	}
	SetFromPointer(MyGeometry, MouseEvent);
	return FReply::Handled();
}

FReply SPinguSlider::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bDragging && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bDragging = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

int32 SPinguSlider::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FPinguStyle& Style = FPinguStyle::Get();
	const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();
	const FVector2f Size = AllottedGeometry.GetLocalSize();
	constexpr float Track = 12.f;
	constexpr float Knob = 24.f;
	const float Value = FMath::Clamp(ValueAttr.Get(), 0.f, 1.f);
	const float Usable = FMath::Max(Size.X - Knob, 1.f);
	const float TrackY = FMath::RoundToFloat((Size.Y - Track) * 0.5f);
	const bool bHot = HotAttr.Get() || bDragging;

	// Track (well) with a navy outline, the filled part, then the knob.
	TrackBrush.OutlineSettings.Color = FSlateColor(Style.Ink * Tint);
	TrackBrush.OutlineSettings.Width = FPinguStyle::LineWidth;
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(FVector2f(Size.X, Track), FSlateLayoutTransform(1.f, FVector2f(0.f, TrackY))),
		&TrackBrush, ESlateDrawEffect::None, Style.PanelDeep * Tint);
	const float FillWidth = 0.5f * Knob + Value * Usable;
	TrackBrush.OutlineSettings.Width = 0.f;
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(FVector2f(FillWidth, Track), FSlateLayoutTransform(1.f, FVector2f(0.f, TrackY))),
		&TrackBrush, ESlateDrawEffect::None, (bHot ? Style.Orange : Style.Ice) * Tint);
	KnobBrush.OutlineSettings.Color = FSlateColor(Style.Ink * Tint);
	KnobBrush.OutlineSettings.Width = FPinguStyle::LineWidth;
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2, AllottedGeometry.ToPaintGeometry(FVector2f(Knob, Knob), FSlateLayoutTransform(1.f, FVector2f(Value * Usable, FMath::RoundToFloat((Size.Y - Knob) * 0.5f)))),
		&KnobBrush, ESlateDrawEffect::None, (bHot ? FLinearColor::White : Style.Snow) * Tint);
	return LayerId + 2;
}

// ============================================================================ screen

void SPinguScreen::Construct(const FArguments& InArgs)
{
	HUD = InArgs._HUD;
	ScreenId = InArgs._Screen;
	const FPinguStyle& Style = FPinguStyle::Get();
	TAttribute<FLinearColor> DimColor = InArgs._DimColor.IsSet() ? InArgs._DimColor : TAttribute<FLinearColor>(Style.Dim);

	SetVisibility(TAttribute<EVisibility>::CreateLambda([this]()
	{
		if (!HUD.IsValid() || HUD->GetScreen() != ScreenId)
		{
			return EVisibility::Collapsed;
		}
		// The lobby island: clicks on empty parts go through to the island (its penguins, tags and "+": APinguLobby).
		return ScreenId == EPinguScreen::Lobby && HUD->IsLobbyIslandScreen() ? EVisibility::SelfHitTestInvisible : EVisibility::Visible;
	}));

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Visibility(InArgs._bDim ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
			.Image(&Style.WhiteBrush)
			.ColorAndOpacity_Lambda([this, DimColor]() { return FSlateColor(DimColor.Get() * FLinearColor(1.f, 1.f, 1.f, GetDimAppear())); })
		]
		+ SOverlay::Slot()
		[
			SAssignNew(Body, SBorder)
			.BorderImage(&Style.NoBrush)
			.Padding(0.f)
			.ColorAndOpacity_Lambda([this]() { return FLinearColor(1.f, 1.f, 1.f, GetAppear()); })
			.RenderTransform_Lambda([this]()
			{
				// Slides up into place over whole pixels (a translation never blurs the glyphs).
				return TOptional<FSlateRenderTransform>(FSlateRenderTransform(FVector2f(0.f, PinguUI::SnapUnits((1.f - GetAppear()) * FPinguStyle::ScreenInOffset))));
			})
			[
				InArgs._Content.Widget
			]
		]
	];
}

float SPinguScreen::GetAppear() const
{
	return HUD.IsValid() ? HUD->GetScreenAppear() : 1.f;
}

float SPinguScreen::GetDimAppear() const
{
	return HUD.IsValid() ? HUD->GetDimAppear() : 1.f;
}

FReply SPinguScreen::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	// A click on an empty part of the menu: the focus stays on the menu (Slate would hand it to the game viewport).
	const TSharedPtr<SWidget> Focus = HUD.IsValid() ? HUD->GetFocusWidget() : nullptr;
	if (Focus.IsValid())
	{
		return FReply::Handled().SetUserFocus(Focus.ToSharedRef(), EFocusCause::SetDirectly);
	}
	return FReply::Handled();
}

// ============================================================================ key capture

void SPinguKeyCapture::Construct(const FArguments& InArgs)
{
	HUD = InArgs._HUD;
	SetVisibility(TAttribute<EVisibility>::CreateLambda([this]()
	{
		return HUD.IsValid() && HUD->IsCapturingKey() ? EVisibility::Visible : EVisibility::Collapsed;
	}));
	ChildSlot
	[
		InArgs._Content.Widget
	];
}

FReply SPinguKeyCapture::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (!InKeyEvent.IsRepeat() && HUD.IsValid())
	{
		HUD->HandleCapturedKey(InKeyEvent.GetKey());
	}
	return FReply::Handled();
}

FReply SPinguKeyCapture::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	return FReply::Handled();
}

FReply SPinguKeyCapture::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (HUD.IsValid())
	{
		HUD->HandleCapturedKey(MouseEvent.GetEffectingButton());
	}
	return FReply::Handled();
}

FReply SPinguKeyCapture::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	// The second click of a double click on the key cap that opened the capture: not a new key (a deliberate mouse
	// button bind is a later, single click).
	return FReply::Handled();
}

FReply SPinguKeyCapture::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	return FReply::Handled();
}

FNavigationReply SPinguKeyCapture::OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	return FNavigationReply::Stop(); // the gamepad stick must not move the focus away while a key is wanted
}

// ============================================================================ modal

void SPinguModal::Construct(const FArguments& InArgs)
{
	FocusTarget = InArgs._FocusTarget;
	ChildSlot
	[
		InArgs._Content.Widget
	];
}

FReply SPinguModal::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const TSharedPtr<SWidget> Target = FocusTarget.Get();
	return Target.IsValid() ? FReply::Handled().SetUserFocus(Target.ToSharedRef(), EFocusCause::SetDirectly) : FReply::Handled();
}

FNavigationReply SPinguModal::OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	return FNavigationReply::Stop(); // one button inside: the focus must not wander to the screen underneath
}

// ============================================================================ ability icons above the heads

// ============================================================================ cursor halo

void SPinguCursorHalo::Construct(const FArguments& InArgs)
{
}

FVector2D SPinguCursorHalo::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D::ZeroVector; // fills whatever the overlay gives it
}

int32 SPinguCursorHalo::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return LayerId;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	// Another app in front (the pointer is not ours), the pointer outside the game's picture, or a test taking screenshots
	// (they show the UI, not the system's arrow): nothing.
	if (!Slate.IsActive() || UPinguSettings::IsAutomatedRun())
	{
		return LayerId;
	}
	const FVector2f Pointer = FVector2f(AllottedGeometry.AbsoluteToLocal(Slate.GetCursorPos()));
	const FVector2f Area = FVector2f(AllottedGeometry.GetLocalSize());
	if (Pointer.X < 0.f || Pointer.Y < 0.f || Pointer.X > Area.X || Pointer.Y > Area.Y)
	{
		return LayerId;
	}
	// The pointer's size in pixels (32 at 100 % Windows scaling), in this widget's units.
	const float PixelsPerUnit = FMath::Max(AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), 0.01f);
	const FVector2D CursorSize = Slate.GetCursorSize();
	const float PointerPixels = CursorSize.Y >= 8.0 ? static_cast<float>(CursorSize.Y) : 32.f;
	const float Units = PointerPixels / PixelsPerUnit;
	const FVector2f Center = Pointer + FVector2f(CenterShareX, CenterShareY) * Units;
	Verts.Reset();
	Indices.Reset();
	PinguUIPrivate::AddSoftDisc(Verts, Indices, AllottedGeometry.GetAccumulatedRenderTransform(), Center, RadiusShare * Units,
		FLinearColor(0.f, 0.f, 0.02f, Darkness) * InWidgetStyle.GetColorAndOpacityTint());
	FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, FPinguStyle::Get().GetWhiteHandle(), Verts, Indices, nullptr, 0, 0,
		ESlateDrawEffect::None, ESlateBatchDrawFlag::None);
	return LayerId + 1;
}

// ============================================================================ above the heads

void SPinguOverheadAbilities::Construct(const FArguments& InArgs)
{
	HUD = InArgs._HUD;
	SetCanTick(true); // Tick follows the local players and runs the "ready" pops
}

FVector2D SPinguOverheadAbilities::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D::ZeroVector; // fills whatever the HUD overlay gives it
}

void SPinguOverheadAbilities::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SLeafWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	// Hidden for a while (menu, results): start fresh so nothing pops when the next round shows the icons again.
	if (LastTickTime < 0.0 || InCurrentTime - LastTickTime > 0.25)
	{
		Tracked.Reset();
	}
	LastTickTime = InCurrentTime;

	for (FTrackedPenguin& Entry : Tracked)
	{
		Entry.bSeen = false;
	}
	// Every penguin: the local players' own (full size, seen through their controller) and everybody else's (people and
	// bots, a bit smaller, seen through the first local player's view). On a LAN client the abilities are replicated.
	const UWorld* World = HUD.IsValid() ? HUD->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	TMap<const APenguinCharacter*, APlayerController*> LocalPenguins;
	if (GameInstance)
	{
		for (const ULocalPlayer* LocalPlayer : GameInstance->GetLocalPlayers())
		{
			APlayerController* Controller = LocalPlayer ? LocalPlayer->GetPlayerController(World) : nullptr;
			if (const APenguinCharacter* Penguin = Controller ? Cast<APenguinCharacter>(Controller->GetPawn()) : nullptr)
			{
				LocalPenguins.Add(Penguin, Controller);
			}
		}
	}
	APlayerController* Viewer = World ? World->GetFirstPlayerController() : nullptr;
	if (World && Viewer)
	{
		for (TActorIterator<APenguinCharacter> It(World); It; ++It)
		{
			APenguinCharacter* Penguin = *It;
			const UPenguinAbilityComponent* Abilities = IsValid(Penguin) ? Penguin->GetAbilities() : nullptr;
			if (!Abilities)
			{
				continue;
			}
			FTrackedPenguin* Entry = Tracked.FindByPredicate([Penguin](const FTrackedPenguin& Item) { return Item.Penguin.Get() == Penguin; });
			const bool bNew = Entry == nullptr;
			if (bNew)
			{
				Entry = &Tracked.AddDefaulted_GetRef();
				Entry->Penguin = Penguin;
			}
			APlayerController* const* LocalController = LocalPenguins.Find(Penguin);
			Entry->bLocal = LocalController != nullptr;
			Entry->Controller = LocalController ? *LocalController : Viewer;
			Entry->bSeen = true;
			for (int32 Slot = 0; Slot < 3; ++Slot)
			{
				const EPenguinAbility Ability = Abilities->GetAbility(Slot);
				const bool bReady = Abilities->GetCooldownFraction(Slot) <= 0.f;
				const int32 Swaps = Abilities->GetSwapCount(Slot);
				if (!bNew && Swaps > Entry->SwapCount[Slot])
				{
					// A pickup swapped or recharged this slot: the big pop (instead of the "ready" one).
					Entry->Shown[Slot] = Ability;
					Entry->bWasReady[Slot] = bReady;
					Entry->SinceReady[Slot] = PopDuration;
					Entry->SinceSwap[Slot] = 0.f;
					Entry->DustAge[Slot] = DustTime; // (another ability now: the old one's dust and shine are over)
					Entry->ShineAge[Slot] = ShineTime;
				}
				else if (bNew || Ability != Entry->Shown[Slot])
				{
					// A new ability in this slot (new round): no pop.
					Entry->Shown[Slot] = Ability;
					Entry->bWasReady[Slot] = bReady;
					Entry->SinceReady[Slot] = PopDuration;
					Entry->SinceSwap[Slot] = SwapPopDuration;
					Entry->DustAge[Slot] = DustTime;
					Entry->ShineAge[Slot] = ShineTime;
				}
				Entry->SwapCount[Slot] = Swaps;
				if (bReady && !Entry->bWasReady[Slot])
				{
					Entry->SinceReady[Slot] = 0.f;
				}
				Entry->bWasReady[Slot] = bReady;
				Entry->SinceReady[Slot] = FMath::Min(Entry->SinceReady[Slot] + InDeltaTime, PopDuration);
				Entry->SinceSwap[Slot] = FMath::Min(Entry->SinceSwap[Slot] + InDeltaTime, SwapPopDuration);
				Entry->DustAge[Slot] = FMath::Min(Entry->DustAge[Slot] + InDeltaTime, DustTime);
				Entry->ShineAge[Slot] = FMath::Min(Entry->ShineAge[Slot] + InDeltaTime, ShineTime);
			}
			// Gambler: a roll that fizzled makes its slot's icon crumble into sparkling dust, a double makes it shine. Every
			// machine reads the replicated roll (serial, double or not, slot, ability); a penguin seen for the first time (a late
			// joiner, the icons shown again after a screen) just notes the serial.
			if (const UPenguinTraitComponent* Traits = Penguin->GetTraits())
			{
				const FPinguTraitEvents& Events = Traits->GetEvents();
				const bool bRolled = Events.GambleSerial != Entry->SeenGamble;
				Entry->SeenGamble = Events.GambleSerial;
				const int32 RolledSlot = Events.GambleSlot;
				if (bRolled && !bNew && RolledSlot >= 0 && RolledSlot < 3
					&& Abilities->GetAbility(RolledSlot) == static_cast<EPenguinAbility>(Events.GambleAbility))
				{
					if (Events.bGambleDouble)
					{
						Entry->ShineAge[RolledSlot] = 0.f;
						++MutableStats().ShineStarts;
					}
					else
					{
						Entry->DustAge[RolledSlot] = 0.f;
						Entry->DustSeed[RolledSlot] = static_cast<int32>(Events.GambleSerial) * 977 + RolledSlot * 31 + Penguin->GetPlayerSlot() * 7919 + 17;
						++MutableStats().DustStarts;
					}
				}
			}
			// The snowball combo: a hit that was its thrower's second or later landed on this penguin (every machine reads the
			// victim's replicated serial); a penguin seen for the first time just notes it. The number pops over its chest.
			{
				const uint8 ComboSerial = Penguin->GetComboPopSerial();
				if (ComboSerial != Entry->SeenCombo)
				{
					Entry->SeenCombo = ComboSerial;
					if (!bNew)
					{
						Entry->ComboCount = Penguin->GetComboPopCount();
						Entry->ComboAge = 0.f;
						FPaintStats& ComboStats = MutableStats();
						++ComboStats.ComboPopStarts;
						ComboStats.LastComboCount = Entry->ComboCount;
					}
				}
				Entry->ComboAge = FMath::Min(Entry->ComboAge + InDeltaTime, ComboPopTime);
			}
		}
	}
	// The local players' own icons are painted last (on top of everybody else's).
	Tracked.StableSort([](const FTrackedPenguin& A, const FTrackedPenguin& B) { return !A.bLocal && B.bLocal; });
	Tracked.RemoveAll([](const FTrackedPenguin& Item) { return !Item.bSeen; });
}

int32 SPinguOverheadAbilities::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace PinguUIPrivate;
	const UWorld* World = HUD.IsValid() ? HUD->GetWorld() : nullptr;
	if (!World)
	{
		return LayerId;
	}
	const FLinearColor Fade = InWidgetStyle.GetColorAndOpacityTint();
	// Viewport pixels -> local Slate units (this widget fills the whole viewport; the scale includes the DPI scale).
	const float PixelsPerUnit = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
	const float UnitsPerPixel = PixelsPerUnit > UE_KINDA_SMALL_NUMBER ? 1.f / PixelsPerUnit : 1.f;
	const FSlateRenderTransform& ToWindow = AllottedGeometry.GetAccumulatedRenderTransform();
	const FLinearColor ShadeColor = CooldownShade() * Fade;
	const FLinearColor HandColor = ClockHandColor() * Fade;
	const FSlateResourceHandle& White = FPinguStyle::Get().GetWhiteHandle();
	// Every icon and bubble gets its own layers, so overlapping ones stack cleanly.
	int32 Layer = LayerId;

	// ---- LAN: name tags above the other penguins (under the bubbles and icons)
	Layer = PaintNameTags(OutDrawElements, AllottedGeometry, Layer, Fade, UnitsPerPixel);

	// ---- ability bubbles (under the players' own icons), also when no local penguin is around
	TArray<APinguPickup*> Pickups;
	APinguPickup::GetAll(World, Pickups);
	const APlayerController* Viewer = World->GetFirstPlayerController();
	for (const APinguPickup* Pickup : Pickups)
	{
		if (!Viewer || !IsValid(Pickup) || Pickup->IsHidden() || Pickup->GetAbility() == EPenguinAbility::None)
		{
			continue;
		}
		const float Opacity = FMath::Clamp(Pickup->GetOpacity(), 0.f, 1.f);
		const float BubbleScale = FMath::Max(Pickup->GetIconScale(), 0.f);
		FVector2D ScreenPosition;
		if (Opacity <= 0.f || BubbleScale * PickupSize < 1.f || !Viewer->ProjectWorldLocationToScreen(Pickup->GetIconLocation(), ScreenPosition, false))
		{
			continue;
		}
		const FVector2f Center(static_cast<float>(ScreenPosition.X) * UnitsPerPixel, static_cast<float>(ScreenPosition.Y) * UnitsPerPixel);
		const FLinearColor BubbleFade = Fade.CopyWithNewOpacity(Fade.A * Opacity);
		const float Diameter = PickupSize * BubbleScale;
		const EPenguinAbility Ability = Pickup->GetAbility();
		const FLinearColor AbilityColor = UPenguinAbilityComponent::GetInfo(Ability).Color;

		// Slow soft pulse: the ring brightens and a faint halo ripples out of it.
		const float Phase = FMath::Frac(Pickup->GetGameTimeSinceCreation() / PickupPulsePeriod);
		const float Glow = 0.5f + 0.5f * FMath::Cos(UE_TWO_PI * Phase);
		PaintRing(OutDrawElements, AllottedGeometry, Layer, Center, Diameter * (1.f + 0.35f * Phase), 2.f * BubbleScale,
			AbilityColor.CopyWithNewOpacity(0.5f * FMath::Square(1.f - Phase)) * BubbleFade);
		FLinearColor RingColor = FMath::Lerp(AbilityColor, FLinearColor::White, 0.4f * Glow);
		RingColor.A = 1.f;
		const FSlateBrush* Art = HUD->GetAbilityBrush(Ability);
		PaintDisc(OutDrawElements, AllottedGeometry, Layer + 1, Center, Diameter, PickupRingWidth * BubbleScale * (1.f + 0.25f * Glow),
			RingColor * BubbleFade, IconDiscColor() * BubbleFade, Art, Art ? Art->GetTint(InWidgetStyle) * BubbleFade : BubbleFade);
		Layer += 3;
	}

	// ---- every penguin's icons: a small triangle standing on the head point (the local players' own full size and last,
	// everybody else's smaller); in a team match a thin ring in the team's colour around each icon
	FPaintStats& PaintStats = MutableStats();
	PaintStats.Ringed.Reset();
	PaintStats.IconRings = 0;
	PaintStats.RingToIcon = 0.f;
	PaintStats.MinClearance = 0.f;
	PaintStats.DustIcons = 0;
	PaintStats.ShineIcons = 0;
	PaintStats.DustPieces = 0;
	PaintStats.DustAge = -1.f;
	PaintStats.ShineAge = -1.f;
	PaintStats.ComboPops = 0;
	PaintStats.ComboPopAge = -1.f;
	PaintStats.DamageLabels = 0;
	PaintStats.MaxDamageShown = -1;
	PaintStats.Frame = GFrameCounter;
	for (const FTrackedPenguin& Entry : Tracked)
	{
		const APenguinCharacter* Penguin = Entry.Penguin.Get();
		const APlayerController* Controller = Entry.Controller.Get();
		const UPenguinAbilityComponent* Abilities = Penguin ? Penguin->GetAbilities() : nullptr;
		if (!Abilities || !Controller || Penguin->IsKnockedOut() || Penguin->IsHidden())
		{
			continue;
		}
		// A team of two or more: its colour (somebody on their own - no team picked - has no ring, like everybody in a match
		// without teams). Ringed icons stand a little further apart, so the rings of neighbours never touch.
		const int32 RingTeam = HUD->GetPenguinTeamRing(Penguin);
		const float Size = Entry.bLocal ? 1.f : OtherIconScale;
		const float Icon = IconSize * Size;
		const float StepSize = (IconSize + IconGap + (RingTeam >= 0 ? TeamIconSpread : 0.f)) * Size;
		const FVector2f SlotOffsets[3] = {
			FVector2f(-0.5f * StepSize, -0.5f * Icon), // LMB: bottom left
			FVector2f(0.f, -0.5f * Icon - 0.866f * StepSize), // MMB: on top between the other two (equilateral triangle)
			FVector2f(0.5f * StepSize, -0.5f * Icon), // RMB: bottom right
		};
		const FVector Head = StandingHeadTop(Penguin) + FVector(0.f, 0.f, HeadClearance);
		FVector2D ScreenPosition;
		if (!Controller->ProjectWorldLocationToScreen(Head, ScreenPosition, false))
		{
			continue;
		}
		const FVector2f Anchor(static_cast<float>(ScreenPosition.X) * UnitsPerPixel, static_cast<float>(ScreenPosition.Y) * UnitsPerPixel);

		// How far each icon is into a pop (ready again, or a pickup).
		float ReadyPops[3];
		float SwapPops[3];
		float Pops[3];
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			ReadyPops[Slot] = PopEnvelope(Entry.SinceReady[Slot], PopDuration);
			SwapPops[Slot] = PopEnvelope(Entry.SinceSwap[Slot], SwapPopDuration);
			Pops[Slot] = FMath::Max(ReadyPops[Slot], SwapPops[Slot]);
		}

		// The team rings: one around each icon, just outside its own ring (under the icons: they never cover the ability),
		// a dark rim under each for the snow. They grow and shrink with their icon's pop.
		if (RingTeam >= 0)
		{
			FLinearColor TeamColor = APenguinCharacter::GetTeamColor(RingTeam);
			TeamColor.A = 1.f;
			const float RingWidthHere = TeamIconRingWidth * FMath::Max(Size, 0.85f);
			const float GapHere = TeamIconRingGap * Size;
			for (int32 Slot = 0; Slot < 3; ++Slot)
			{
				if (Abilities->GetAbility(Slot) == EPenguinAbility::None)
				{
					continue;
				}
				const float IconDiameter = IconSize * (1.f + PopScale * ReadyPops[Slot] + SwapPopScale * SwapPops[Slot]) * Size;
				const float Diameter = IconDiameter + 2.f * (GapHere + RingWidthHere);
				const FVector2f Center = Anchor + SlotOffsets[Slot];
				PaintRing(OutDrawElements, AllottedGeometry, Layer, Center, Diameter + 2.f, RingWidthHere + 2.f, PinguUI::WithAlpha(FPinguStyle::Get().Ink, 0.55f) * Fade);
				PaintRing(OutDrawElements, AllottedGeometry, Layer + 1, Center, Diameter, RingWidthHere, TeamColor * Fade);
				const float Clearance = 0.5f * Diameter - RingWidthHere - 0.5f * IconDiameter;
				PaintStats.MinClearance = PaintStats.IconRings == 0 ? Clearance : FMath::Min(PaintStats.MinClearance, Clearance);
				PaintStats.RingToIcon = FMath::Max(PaintStats.RingToIcon, Diameter / FMath::Max(IconDiameter, 1.f));
				++PaintStats.IconRings;
			}
			PaintStats.Ringed.Add(Penguin->GetDisplayName());
			Layer += 2;
		}

		// Draw order: the side icons, then the top one; an icon that is popping goes over its neighbours.
		int32 Order[3] = { 0, 2, 1 };
		for (int32 Index = 1; Index < 3; ++Index)
		{
			for (int32 Back = Index; Back > 0 && Pops[Order[Back - 1]] > Pops[Order[Back]]; --Back)
			{
				Swap(Order[Back - 1], Order[Back]);
			}
		}

		for (const int32 Slot : Order)
		{
			const EPenguinAbility Ability = Abilities->GetAbility(Slot);
			if (Ability == EPenguinAbility::None)
			{
				continue;
			}
			const float Covered = Abilities->GetCooldownFraction(Slot);
			const float ReadyPop = PopEnvelope(Entry.SinceReady[Slot], PopDuration);
			const float SwapPop = PopEnvelope(Entry.SinceSwap[Slot], SwapPopDuration);
			// Gambler: an icon that fizzled crumbles away (it shivers, fades into its dust, and pops back in with its clock);
			// one that doubled shines gold and swells.
			const float DustAge = Entry.DustAge[Slot];
			const float ShineAge = Entry.ShineAge[Slot];
			const bool bDusting = DustAge < DustTime;
			const bool bShining = ShineAge < ShineTime;
			float Vis = 1.f;
			float DustScale = 1.f;
			FVector2f Shiver = FVector2f::ZeroVector;
			if (bDusting)
			{
				if (DustAge < DustGoneAt)
				{
					// Shivering from the start, fading out between DustShiverTime and DustGoneAt, a little smaller as it goes.
					const float Out = FMath::Clamp((DustAge - DustShiverTime) / (DustGoneAt - DustShiverTime), 0.f, 1.f);
					Vis = 1.f - Out * Out;
					DustScale = 1.f - 0.18f * Out;
					const float Shake = (1.f - 0.5f * Out) * 1.6f * Size;
					Shiver = FVector2f(FMath::Sin(DustAge * 140.f) * Shake, FMath::Cos(DustAge * 170.f) * 0.5f * Shake);
				}
				else if (DustAge < DustBackAt)
				{
					Vis = 0.f;
				}
				else
				{
					// Back: pops in from small, a touch too big, then settles.
					const float Back = FMath::Clamp((DustAge - DustBackAt) / DustBackTime, 0.f, 1.f);
					Vis = FMath::Min(Back * 2.f, 1.f);
					DustScale = 0.55f + 0.45f * Back + 0.14f * FMath::Sin(UE_PI * Back);
				}
			}
			const float ShinePop = bShining ? PopEnvelope(ShineAge, ShineTime) : 0.f;
			const float IconScale = (1.f + PopScale * ReadyPop + SwapPopScale * SwapPop + 0.3f * ShinePop) * Size * DustScale;
			const FVector2f Center = Anchor + SlotOffsets[Slot] + Shiver;
			FLinearColor SlotFade = Fade;
			SlotFade.A *= Vis;

			// Disc + ring: the ring is dark grey while recharging, the ability colour when ready and flashes white in a pop.
			const FLinearColor AbilityColor = UPenguinAbilityComponent::GetInfo(Ability).Color;
			FLinearColor RingColor = Covered > 0.f ? RingDimColor() : AbilityColor;
			RingColor = FMath::Lerp(RingColor, FLinearColor::White, FMath::Max(0.7f * ReadyPop, 0.85f * SwapPop));
			RingColor = FMath::Lerp(RingColor, FLinearColor(1.f, 0.82f, 0.25f), 0.9f * ShinePop);
			RingColor.A = Covered > 0.f ? RingDimColor().A : 1.f;
			const FSlateBrush* Art = HUD->GetAbilityBrush(Ability);
			if (Vis > 0.004f)
			{
				PaintDisc(OutDrawElements, AllottedGeometry, Layer, Center, IconSize * IconScale, RingWidth * IconScale,
					RingColor * SlotFade, IconDiscColor() * SlotFade, Art, Art ? Art->GetTint(InWidgetStyle) * SlotFade : SlotFade);
			}

			// Recharging: a dark grey clock over the part still to charge - its hand (a white line) sweeps clockwise from
			// 12 o'clock and uncovers the icon - and the ring fills with the ability colour behind the hand.
			if (Covered > 0.f && Vis > 0.004f)
			{
				const float Outer = IconSize * 0.5f * IconScale;
				const float Inner = (IconSize * 0.5f - RingWidth) * IconScale;
				const float Feather = 1.2f * UnitsPerPixel;
				const float HandTurn = 1.f - Covered;
				const FVector2f HandDirection(FMath::Sin(HandTurn * UE_TWO_PI), -FMath::Cos(HandTurn * UE_TWO_PI));
				FLinearColor SlotShade = ShadeColor;
				SlotShade.A *= Vis;
				FLinearColor SlotHand = HandColor;
				SlotHand.A *= Vis;
				PieVerts.Reset();
				PieIndices.Reset();
				AddPie(PieVerts, PieIndices, ToWindow, Center, Inner, Feather, Covered, SlotShade);
				AddArc(PieVerts, PieIndices, ToWindow, Center, Inner, Outer, 0.f, HandTurn, Feather, AbilityColor * SlotFade);
				AddLine(PieVerts, PieIndices, ToWindow, Center, Center + HandDirection * Outer, FMath::Max(2.f * IconScale, 1.6f * UnitsPerPixel),
					Feather, SlotHand);
				FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer + 2, White, PieVerts, PieIndices, nullptr, 0, 0,
					ESlateDrawEffect::None, ESlateBatchDrawFlag::None);
			}

			// Pickup pop: a white ring flashes out of the icon.
			if (Entry.SinceSwap[Slot] < SwapPopDuration)
			{
				const float Flash = FMath::Clamp(Entry.SinceSwap[Slot] / SwapPopDuration, 0.f, 1.f);
				PaintRing(OutDrawElements, AllottedGeometry, Layer + 3, Center, Icon * (1.1f + 0.9f * Flash), FMath::Lerp(4.f, 1.f, Flash) * Size,
					FLinearColor::White.CopyWithNewOpacity(FMath::Square(1.f - Flash)) * Fade);
			}
			// Gambler: the crumbled icon's dust (over its neighbours), or the double's golden glints.
			if (bDusting)
			{
				PaintIconDust(OutDrawElements, AllottedGeometry, Layer + 4, Anchor + SlotOffsets[Slot], Icon, AbilityColor, DustAge, Entry.DustSeed[Slot], Fade);
				PaintStats.DustAge = PaintStats.DustAge < 0.f ? DustAge : FMath::Min(PaintStats.DustAge, DustAge);
				++PaintStats.DustIcons;
			}
			if (bShining)
			{
				PaintIconShine(OutDrawElements, AllottedGeometry, Layer + 4, Center, Icon, ShineAge, Fade);
				PaintStats.ShineAge = PaintStats.ShineAge < 0.f ? ShineAge : FMath::Min(PaintStats.ShineAge, ShineAge);
				++PaintStats.ShineIcons;
			}
			Layer += 5;
		}
		// The snowball combo's pop over the victim's chest (on top of its icons).
		if (Entry.ComboAge < ComboPopTime && Entry.ComboCount >= 2)
		{
			FVector2D ChestPosition;
			if (Controller->ProjectWorldLocationToScreen(Penguin->GetAimOrigin(), ChestPosition, false))
			{
				const FVector2f Chest(static_cast<float>(ChestPosition.X) * UnitsPerPixel, static_cast<float>(ChestPosition.Y) * UnitsPerPixel);
				Layer = PaintComboPop(OutDrawElements, AllottedGeometry, Layer, Chest, Entry.ComboCount, Entry.ComboAge, Fade);
				++PaintStats.ComboPops;
				PaintStats.ComboPopAge = PaintStats.ComboPopAge < 0.f ? Entry.ComboAge : FMath::Min(PaintStats.ComboPopAge, Entry.ComboAge);
			}
		}
		// Traits mode, Damage % (2026-11-07): the damage it took this round, small under its feet - white, warming to red.
		const UPenguinTraitComponent* DamageTraits = Penguin->GetTraits();
		FVector2D FeetPosition;
		if (DamageTraits && DamageTraits->HasTrait(EPinguTrait::DamagePercent) && DamageTraits->IsTransformed() && FSlateApplication::IsInitialized()
			&& FSlateApplication::Get().GetRenderer() && Controller->ProjectWorldLocationToScreen(Penguin->GetFeetLocation(), FeetPosition, false))
		{
			const int32 Damage = DamageTraits->GetDamagePercent();
			const FString Label = FString::Printf(TEXT("%d%%"), Damage);
			const FSlateFontInfo Font = FPinguStyle::Title(Entry.bLocal ? 15.f : 13.f, 2);
			const FVector2f TextSize = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Label, Font);
			const FVector2f Feet(static_cast<float>(FeetPosition.X) * UnitsPerPixel, static_cast<float>(FeetPosition.Y) * UnitsPerPixel);
			const float Heat = FMath::Clamp(Damage / 150.f, 0.f, 1.f);
			const FLinearColor Color = FMath::Lerp(FLinearColor::White, FLinearColor(1.f, 0.25f, 0.15f), Heat);
			FSlateDrawElement::MakeText(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Feet + FVector2f(-0.5f * TextSize.X, 4.f))),
				Label, Font, ESlateDrawEffect::None, Color * Fade);
			++Layer;
			++PaintStats.DamageLabels;
			PaintStats.MaxDamageShown = FMath::Max(PaintStats.MaxDamageShown, Damage);
		}
	}

	// ---- penguins flung out of the picture: a marker at the screen's edge (over everything else)
	Layer = PaintOffscreenMarkers(OutDrawElements, AllottedGeometry, Layer, Fade, UnitsPerPixel);
	return Layer > LayerId ? Layer - 1 : LayerId;
}

int32 SPinguOverheadAbilities::PaintIconDust(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer,
	const FVector2f& Center, float Diameter, const FLinearColor& AbilityColor, float Age, int32 Seed, const FLinearColor& Fade) const
{
	using namespace PinguUIPrivate;
	const FSlateRenderTransform& ToWindow = AllottedGeometry.GetAccumulatedRenderTransform();
	const float Unit = Diameter / IconSize; // 1 for a full-size icon
	// Fewer pieces on lower effects settings: the same stream, so the ones that stay are the first of the full set.
	const int32 NumPieces = FMath::Clamp(FMath::RoundToInt(DustPerIcon * UPinguSettings::GetFXDensity()), 8, DustPerIcon);
	const FLinearColor Chip = FMath::Lerp(AbilityColor, FLinearColor(0.09f, 0.16f, 0.32f), 0.3f);
	const FLinearColor Glints[3] = { FLinearColor::White, FMath::Lerp(AbilityColor, FLinearColor::White, 0.65f), FMath::Lerp(AbilityColor, FLinearColor::White, 0.3f) };
	FRandomStream Rand(Seed);
	PieVerts.Reset();
	PieIndices.Reset();
	int32 Drawn = 0;
	for (int32 Index = 0; Index < DustPerIcon; ++Index)
	{
		// (every number is drawn for every piece, so a piece is the same on every setting and every frame)
		const float Angle = Rand.FRandRange(0.f, UE_TWO_PI);
		const float Reach = FMath::Sqrt(Rand.FRand()) * 0.46f * Diameter;
		const float Delay = Rand.FRandRange(0.02f, 0.26f);
		const float Life = Rand.FRandRange(0.5f, 0.74f);
		const FVector2f Velocity(Rand.FRandRange(-22.f, 22.f) * Unit, Rand.FRandRange(-40.f, 6.f) * Unit);
		const float Gravity = Rand.FRandRange(160.f, 320.f) * Unit;
		const float Turn = Rand.FRandRange(0.f, UE_TWO_PI);
		const float Spin = Rand.FRandRange(-9.f, 9.f);
		const float Radius = Rand.FRandRange(4.2f, 7.6f) * Unit;
		const float Phase = Rand.FRandRange(0.f, UE_TWO_PI);
		const int32 Palette = Rand.RandRange(0, 2);
		const float T = Age - Delay;
		if (Index >= NumPieces || T < 0.f || T > Life)
		{
			continue;
		}
		const float U = T / Life;
		const FVector2f Position = Center + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * Reach + Velocity * T + FVector2f(0.f, 0.5f * Gravity * T * T);
		const float Alpha = (1.f - U * U) * (0.7f + 0.3f * FMath::Sin(T * 34.f + Phase));
		if (Index % 3 == 0)
		{
			// A chip of the icon itself: a little tilted square in the ability's dark colour, tumbling down.
			AddCrumb(PieVerts, PieIndices, ToWindow, Position, Radius * 0.62f * (1.f - 0.4f * U), Turn + Spin * T, Chip.CopyWithNewOpacity(Chip.A * FMath::Min(Alpha * 1.4f, 1.f)) * Fade);
		}
		else
		{
			// Glitter: a four-pointed twinkle that spins and shrinks as it falls.
			AddSparkle(PieVerts, PieIndices, ToWindow, Position, Radius * 1.5f * (1.f - 0.45f * U), Turn + Spin * T, Glints[Palette].CopyWithNewOpacity(FMath::Clamp(Alpha, 0.f, 1.f)) * Fade);
		}
		++Drawn;
	}
	if (Drawn > 0)
	{
		FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, FPinguStyle::Get().GetWhiteHandle(), PieVerts, PieIndices, nullptr, 0, 0,
			ESlateDrawEffect::None, ESlateBatchDrawFlag::None);
	}
	MutableStats().DustPieces += Drawn;
	return Layer + 1;
}

int32 SPinguOverheadAbilities::PaintIconShine(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer,
	const FVector2f& Center, float Diameter, float Age, const FLinearColor& Fade) const
{
	using namespace PinguUIPrivate;
	const FSlateRenderTransform& ToWindow = AllottedGeometry.GetAccumulatedRenderTransform();
	// Three golden glints that swell and fade one after another, on the icon's upper right, left and lower right.
	static const FVector2f Spots[3] = { FVector2f(0.30f, -0.30f), FVector2f(-0.34f, -0.06f), FVector2f(0.12f, 0.36f) };
	static const float Starts[3] = { 0.f, 0.08f, 0.16f };
	static const float Sizes[3] = { 0.78f, 0.5f, 0.42f };
	PieVerts.Reset();
	PieIndices.Reset();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const float Span = ShineTime - Starts[Index];
		const float Env = PopEnvelope(Age - Starts[Index], Span);
		if (Env <= 0.01f)
		{
			continue;
		}
		const FLinearColor Gold = FLinearColor(1.f, 0.9f, 0.45f, FMath::Clamp(Env * 1.2f, 0.f, 1.f)) * Fade;
		AddSparkle(PieVerts, PieIndices, ToWindow, Center + Spots[Index] * Diameter, Diameter * Sizes[Index] * (0.15f + 0.85f * Env), 0.4f * Age * (Index + 1), Gold);
	}
	if (PieIndices.Num() > 0)
	{
		FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, FPinguStyle::Get().GetWhiteHandle(), PieVerts, PieIndices, nullptr, 0, 0,
			ESlateDrawEffect::None, ESlateBatchDrawFlag::None);
	}
	return Layer + 1;
}

int32 SPinguOverheadAbilities::PaintComboPop(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer,
	const FVector2f& Center, int32 Count, float Age, const FLinearColor& Fade) const
{
	if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
	{
		return Layer;
	}
	// How far the push has got to its cap: the bigger and hotter, the further along (a 2nd hit is small and white, the capped
	// ones the biggest and hot orange).
	const float Build = FMath::Clamp((PinguTuning::GetSnowballComboScale(Count) - 1.f) / (PinguTuning::SnowballComboMax - 1.f), 0.f, 1.f);
	const float FontSize = FMath::RoundToFloat(FMath::Lerp(ComboPopFontMin, ComboPopFontMax, Build));
	const FSlateFontInfo Font = FPinguStyle::Title(FontSize, 3);
	const FString Label = FString::Printf(TEXT("x%d"), Count);
	const FVector2f TextSize = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Label, Font);
	const float Life = FMath::Clamp(Age / ComboPopTime, 0.f, 1.f);
	// It pops out small, overshoots and settles within a tenth of a second, rises (fast at first) and fades out over the last
	// 40 % of its time.
	const float PopIn = FMath::Clamp(Age / 0.1f, 0.f, 1.f);
	const float Scale = (0.45f + 0.55f * PopIn) + 0.3f * FMath::Sin(UE_PI * PopIn);
	const float Alpha = Life < 0.6f ? 1.f : FMath::Clamp(1.f - (Life - 0.6f) / 0.4f, 0.f, 1.f);
	const float Rise = ComboPopRise * (1.f - FMath::Square(1.f - Life));
	// White, then gold, then hot orange as the push gets to its cap.
	const FLinearColor Gold(1.f, 0.82f, 0.25f);
	const FLinearColor Hot(1.f, 0.42f, 0.1f);
	const FLinearColor Color = Build < 0.5f ? FMath::Lerp(FLinearColor::White, Gold, Build * 2.f) : FMath::Lerp(Gold, Hot, (Build - 0.5f) * 2.f);
	const FVector2f TopLeft = Center + FVector2f(0.f, -Rise - 0.5f * ComboPopFontMax) - TextSize * (0.5f * Scale);
	FSlateDrawElement::MakeText(OutDrawElements, Layer,
		AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Scale, TopLeft)),
		Label, Font, ESlateDrawEffect::None, Color.CopyWithNewOpacity(Alpha) * Fade);
	return Layer + 1;
}

int32 SPinguOverheadAbilities::PaintNameTags(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer,
	const FLinearColor& Fade, float UnitsPerPixel) const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	const UWorld* World = PenguinHUD ? PenguinHUD->GetWorld() : nullptr;
	const APlayerController* Viewer = World ? World->GetFirstPlayerController() : nullptr;
	if (!Viewer || !PenguinHUD->IsNetworkGame() || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
	{
		return Layer;
	}
	// The lobby island has its own 3D name tags (APinguLobby).
	if (const APinguLobby* Lobby = APinguLobby::Get(World); Lobby && Lobby->IsShowing())
	{
		return Layer;
	}
	const FPinguStyle& Style = FPinguStyle::Get();
	const FSlateFontInfo Font = FPinguStyle::Body(TagFontSize);
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const float PixelsPerUnit = 1.f / UnitsPerPixel;
	constexpr float PadX = 8.f;
	constexpr float PadY = 2.f;

	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		// Everybody but this PC's own player (it has the ability icons): other humans and the bots.
		if (!IsValid(Penguin) || Penguin->IsKnockedOut() || Penguin->IsHidden() || (Penguin->IsPlayerControlled() && Penguin->IsLocallyControlled()))
		{
			continue;
		}
		const FString& Name = Penguin->GetDisplayName();
		if (Name.IsEmpty())
		{
			continue;
		}
		// Above the penguin's (small) ability icons, which stand on its head point.
		FVector2D ScreenPosition;
		if (!Viewer->ProjectWorldLocationToScreen(PinguUIPrivate::StandingHeadTop(Penguin) + FVector(0.f, 0.f, HeadClearance), ScreenPosition, false))
		{
			continue;
		}
		const float IconsHigh = GetIconsHeight(OtherIconScale, PenguinHUD->GetPenguinTeamRing(Penguin) >= 0) + 4.f;
		ScreenPosition.Y -= IconsHigh * PixelsPerUnit;
		const FString Label = Name.ToUpper();
		float* KnownWidth = TagWidths.Find(Label);
		FVector2f TextSize(0.f, 0.f);
		if (KnownWidth)
		{
			TextSize = FVector2f(*KnownWidth, TagFontSize * 1.5f);
		}
		else
		{
			const FVector2f Measured = Measure->Measure(Label, Font);
			TextSize = FVector2f(Measured.X, TagFontSize * 1.5f);
			TagWidths.Add(Label, Measured.X);
		}
		// The pill's bottom centre sits on the point; its corner is rounded to a whole pixel (crisp text).
		const FVector2f PillSize(FMath::CeilToFloat(TextSize.X + 2.f * PadX), FMath::CeilToFloat(TextSize.Y + 2.f * PadY));
		const float LeftPixels = FMath::RoundToFloat(static_cast<float>(ScreenPosition.X) - 0.5f * PillSize.X * PixelsPerUnit);
		const float TopPixels = FMath::RoundToFloat(static_cast<float>(ScreenPosition.Y) - PillSize.Y * PixelsPerUnit);
		const FVector2f TopLeft(LeftPixels * UnitsPerPixel, TopPixels * UnitsPerPixel);
		const FLinearColor SlotColor = Penguin->GetPlayerColor(); // (its own colour, also in a team match)

		TagBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
		TagBrush.OutlineSettings.Color = FSlateColor(SlotColor * Fade);
		TagBrush.OutlineSettings.Width = FPinguStyle::LineWidth;
		// (The UI's wood, 2026-11-07: PinguWoodStyle.h.)
		PinguWood::FPlankLook TagLook;
		TagLook.Cut = PinguWood::ECut::Pill;
		TagLook.Opacity = Fade.A;
		TagLook.Seed = GetTypeHash(Label);
		if (!PinguWood::PaintPlank(OutDrawElements, Layer, AllottedGeometry, TopLeft, PillSize, TagLook))
		{
			FSlateDrawElement::MakeBox(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(PillSize, FSlateLayoutTransform(1.f, TopLeft)),
				&TagBrush, ESlateDrawEffect::None, PinguUI::WithAlpha(Style.Ink, 0.82f) * Fade);
		}
		FSlateDrawElement::MakeText(OutDrawElements, Layer + 1,
			AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(1.f, TopLeft + FVector2f(PadX, PadY))),
			Label, Font, ESlateDrawEffect::None, FMath::Lerp(SlotColor, FLinearColor::White, 0.55f) * Fade);
		Layer += 2;
	}
	return Layer;
}

int32 SPinguOverheadAbilities::PaintOffscreenMarkers(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer,
	const FLinearColor& Fade, float UnitsPerPixel) const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	const UWorld* World = PenguinHUD ? PenguinHUD->GetWorld() : nullptr;
	const APlayerController* Viewer = World ? World->GetFirstPlayerController() : nullptr;
	if (!Viewer)
	{
		return Layer;
	}
	const FVector2f Area = AllottedGeometry.GetLocalSize();
	const float Radius = MarkerSize * 0.5f;
	const float Inset = MarkerEdge + Radius;
	if (Area.X < 4.f * Inset || Area.Y < 4.f * Inset)
	{
		return Layer;
	}
	const FSlateRenderTransform& ToWindow = AllottedGeometry.GetAccumulatedRenderTransform();
	const FSlateResourceHandle& White = FPinguStyle::Get().GetWhiteHandle();
	const APawn* OwnPawn = Viewer->GetPawn();
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		const APenguinCharacter* Penguin = *It;
		if (!IsValid(Penguin) || Penguin->IsKnockedOut() || Penguin->IsHidden())
		{
			continue;
		}
		FVector2D ScreenPosition;
		if (!Viewer->ProjectWorldLocationToScreen(Penguin->GetActorLocation(), ScreenPosition, false))
		{
			continue;
		}
		const FVector2f Where(static_cast<float>(ScreenPosition.X) * UnitsPerPixel, static_cast<float>(ScreenPosition.Y) * UnitsPerPixel);
		// Still (partly) in the picture: no marker (a body's width of slack at the edges).
		constexpr float Slack = 28.f;
		if (Where.X >= -Slack && Where.X <= Area.X + Slack && Where.Y >= -Slack && Where.Y <= Area.Y + Slack)
		{
			continue;
		}
		const FVector2f Center(FMath::Clamp(Where.X, Inset, Area.X - Inset), FMath::Clamp(Where.Y, Inset, Area.Y - Inset));
		FVector2f Dir = Where - Center;
		Dir = Dir.SizeSquared() > 1.f ? Dir.GetSafeNormal() : FVector2f(0.f, -1.f);
		const int32 ColorIndex = Penguin->GetColorIndex();
		const FLinearColor Color = APenguinCharacter::GetSlotColor(ColorIndex);
		const bool bOwn = Penguin == OwnPawn;

		// The arrow pointing to where the penguin is, out of the circle's edge.
		PieVerts.Reset();
		PieIndices.Reset();
		const FVector2f Side(-Dir.Y, Dir.X);
		const FVector2f Base = Center + Dir * (Radius - 3.f);
		PinguUI::AddTriangle(PieVerts, PieIndices, ToWindow, Center + Dir * (Radius + MarkerArrow), Base + Side * (MarkerArrow * 0.8f),
			Base - Side * (MarkerArrow * 0.8f), (bOwn ? FPinguStyle::Get().Gold : Color) * Fade, 1.2f * UnitsPerPixel);
		FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, White, PieVerts, PieIndices, nullptr, 0, 0, ESlateDrawEffect::None, ESlateBatchDrawFlag::None);
		// The circle in the penguin's colour (gold for this PC's own), its portrait inside.
		PaintDisc(OutDrawElements, AllottedGeometry, Layer + 1, Center, MarkerSize, MarkerRing, (bOwn ? FPinguStyle::Get().Gold : Color) * Fade,
			(Color * 0.55f + FLinearColor(0.03f, 0.04f, 0.08f, 0.45f)) * Fade, nullptr, Fade);
		PaintPortrait(OutDrawElements, AllottedGeometry, Layer + 3, Center, MarkerSize - 2.f * MarkerRing - 2.f, Penguin->GetPlayerSlot(), ColorIndex, Fade);
		// A team match: its team's ring around the circle, in the colour its icons' rings have (a dark rim under it for the
		// snow; somebody on their own has none).
		const int32 RingTeam = PenguinHUD->GetPenguinTeamRing(Penguin);
		if (RingTeam >= 0)
		{
			FLinearColor TeamColor = APenguinCharacter::GetTeamColor(RingTeam);
			TeamColor.A = 1.f;
			const float Diameter = MarkerSize + 2.f * (TeamRingGap + TeamRingWidth);
			PaintRing(OutDrawElements, AllottedGeometry, Layer + 5, Center, Diameter + 2.f, TeamRingWidth + 2.f, PinguUI::WithAlpha(FPinguStyle::Get().Ink, 0.55f) * Fade);
			PaintRing(OutDrawElements, AllottedGeometry, Layer + 6, Center, Diameter, TeamRingWidth, TeamColor * Fade);
		}
		Layer += 7;
	}
	return Layer;
}

float SPinguOverheadAbilities::GetIconsHeight(float Scale, bool bTeamRings)
{
	// The bottom icons' height plus the top one's rise (an equilateral triangle); ringed icons stand further apart and
	// their rings (and rims) stick out a little above the top one.
	const float Step = IconSize + IconGap + (bTeamRings ? TeamIconSpread : 0.f);
	const float RingRoom = bTeamRings ? (TeamIconRingGap + TeamIconRingWidth) * Scale + 1.f : 0.f;
	return (IconSize + 0.866f * Step) * Scale + RingRoom;
}

SPinguOverheadAbilities::FPaintStats& SPinguOverheadAbilities::MutableStats()
{
	static FPaintStats Stats;
	return Stats;
}

void SPinguOverheadAbilities::PaintPortrait(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
	float Size, int32 Slot, int32 ColorIndex, const FLinearColor& Fade) const
{
	const APenguinHUD* PenguinHUD = HUD.Get();
	if (!PenguinHUD)
	{
		return;
	}
	const FVector2f Box(Size, Size);
	const FPaintGeometry Geometry = AllottedGeometry.ToPaintGeometry(Box, FSlateLayoutTransform(1.f, Center - Box * 0.5f));
	const FSlateBrush* Face = PenguinHUD->GetPortraitBrush(Slot, Size);
	const FSlateBrush* Felt = PenguinHUD->GetPortraitFeltBrush(Slot, Size);
	if (Face && Face->GetDrawType() != ESlateBrushDrawType::NoDrawType)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, Layer, Geometry, Face, ESlateDrawEffect::None, Fade);
	}
	if (Felt && Felt->GetDrawType() != ESlateBrushDrawType::NoDrawType)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1, Geometry, Felt, ESlateDrawEffect::None, APenguinCharacter::GetBeanieTint(ColorIndex) * Fade);
	}
}

void SPinguOverheadAbilities::PaintDisc(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
	float Diameter, float Ring, const FLinearColor& RingColor, const FLinearColor& DiscColor, const FSlateBrush* Art, const FLinearColor& ArtTint) const
{
	// The brush is a circle of IconSize: it is scaled to the diameter (which scales the ring too, so its width is divided back out).
	const float DiscScale = FMath::Max(Diameter, 1.f) / IconSize;
	const FVector2f DiscSize(IconSize, IconSize);
	IconBrush.OutlineSettings.Color = FSlateColor(RingColor);
	IconBrush.OutlineSettings.Width = Ring / DiscScale;
	FSlateDrawElement::MakeBox(OutDrawElements, Layer,
		AllottedGeometry.ToPaintGeometry(DiscSize, FSlateLayoutTransform(DiscScale, Center - DiscSize * (0.5f * DiscScale))),
		&IconBrush, ESlateDrawEffect::None, DiscColor);
	if (Art && Art->GetDrawType() != ESlateBrushDrawType::NoDrawType)
	{
		// The icon textures are transparent with the art in the middle (it reaches ~1.1x the half width): this keeps it inside the ring.
		const float ArtExtent = Diameter * (1.f - 2.f * RingWidth / IconSize) * 0.88f;
		const FVector2f ArtSize(ArtExtent, ArtExtent);
		FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1,
			AllottedGeometry.ToPaintGeometry(ArtSize, FSlateLayoutTransform(1.f, Center - ArtSize * 0.5f)),
			Art, ESlateDrawEffect::None, ArtTint);
	}
}

void SPinguOverheadAbilities::PaintRing(FSlateWindowElementList& OutDrawElements, const FGeometry& AllottedGeometry, int32 Layer, const FVector2f& Center,
	float Diameter, float Ring, const FLinearColor& RingColor) const
{
	PaintDisc(OutDrawElements, AllottedGeometry, Layer, Center, Diameter, Ring, RingColor, PinguUIPrivate::NoFill(), nullptr, FLinearColor::White);
}

#undef LOCTEXT_NAMESPACE

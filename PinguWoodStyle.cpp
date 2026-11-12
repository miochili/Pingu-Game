// Pingu - the one wood of the whole UI (see PinguWoodStyle.h; the rules: docs/UI_STYLE.md). The atlas layout mirrors
// SourceArt/UI3D/T_UI2D_CabinAtlas.json (Tools/ArtGen/make_ui_wood_cabin.py). 2026-11-07 (card trait-logo-wood-tiling; Linus:
// "matke the texture so it doesnt repeat cus u can see the tiling really well"): 2048 x 4096, 4 tiles per strip; each plank picks
// its tiles from its seed - never the same twice running, half of them mirrored, lengths jittered (PlankTiles).

#include "PinguWoodStyle.h"
#include "Penguin.h"
#include "Engine/Texture.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Geometry.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"

namespace PinguWoodPrivate
{
	static const TCHAR* AtlasPath = TEXT("/Game/UI/Menu3D/T_UI2D_CabinAtlas.T_UI2D_CabinAtlas");

	/** T_UI2D_CabinAtlas.json "pieces": the plank strips (x, y in true texels; all are 1752 wide - 4 tiles - the track 1696). */
	struct FStripRect { float X, Y, W, H, BodyH; };
	// [class][normal, hover, disabled]: -1 X = the atlas has no such state (the normal one is used).
	static const FStripRect StripRects[static_cast<int32>(PinguWood::EClass::Count)][3] = {
		/* Board   */ { { 16.f, 1420.f, 1752.f, 296.f, 208.f }, { -1.f, 0.f, 0.f, 0.f, 0.f }, { -1.f, 0.f, 0.f, 0.f, 0.f } },
		/* Row     */ { { 16.f, 2608.f, 1752.f, 260.f, 172.f }, { 16.f, 2884.f, 1752.f, 260.f, 172.f }, { -1.f, 0.f, 0.f, 0.f, 0.f } },
		/* ButtonS */ { { 16.f, 1732.f, 1752.f, 276.f, 188.f }, { 16.f, 2024.f, 1752.f, 276.f, 188.f }, { 16.f, 2316.f, 1752.f, 276.f, 188.f } },
		/* ButtonL */ { { 16.f, 412.f, 1752.f, 320.f, 232.f }, { 16.f, 748.f, 1752.f, 320.f, 232.f }, { 16.f, 1084.f, 1752.f, 320.f, 232.f } },
		/* Banner  */ { { 16.f, 16.f, 1752.f, 380.f, 292.f }, { -1.f, 0.f, 0.f, 0.f, 0.f }, { -1.f, 0.f, 0.f, 0.f, 0.f } },
		/* Pill    */ { { 16.f, 3160.f, 1752.f, 208.f, 120.f }, { -1.f, 0.f, 0.f, 0.f, 0.f }, { -1.f, 0.f, 0.f, 0.f, 0.f } },
		/* Track   */ { { 16.f, 3536.f, 1696.f, 64.f, 40.f }, { -1.f, 0.f, 0.f, 0.f, 0.f }, { -1.f, 0.f, 0.f, 0.f, 0.f } },
		/* Snow    */ { { 16.f, 3384.f, 1752.f, 136.f, 208.f }, { -1.f, 0.f, 0.f, 0.f, 0.f }, { -1.f, 0.f, 0.f, 0.f, 0.f } },
	};
	constexpr float SnowTopLine = 64.f;
	/** (x, y, w, h) of ESprite in true texels. */
	static const PinguWood::FRect SpriteRects[static_cast<int32>(PinguWood::ESprite::Count)] = {
		{ 1836.f, 876.f, 40.f, 40.f },   // nail_0
		{ 1892.f, 876.f, 40.f, 40.f },   // nail_1
		{ 1784.f, 876.f, 36.f, 40.f },   // icicle_0
		{ 1904.f, 800.f, 40.f, 52.f },   // icicle_1
		{ 1784.f, 720.f, 42.f, 64.f },   // icicle_2
		{ 1972.f, 488.f, 46.f, 76.f },   // icicle_3
		{ 1960.f, 800.f, 38.f, 48.f },   // icicle_4
		{ 1842.f, 720.f, 104.f, 60.f },  // knot_0
		{ 1784.f, 800.f, 104.f, 60.f },  // knot_1
		{ 1948.f, 876.f, 32.f, 32.f },   // pip
		{ 1996.f, 876.f, 16.f, 16.f },   // solid
		{ 1784.f, 16.f, 224.f, 224.f },  // well
		{ 1784.f, 256.f, 172.f, 216.f }, // knob_normal
		{ 1784.f, 488.f, 172.f, 216.f }, // knob_hover
	};
	/** A true texel row -> the V the quads carry (V / AtlasSize = the texture coordinate; the atlas is twice as high as wide). */
	constexpr float VScale = PinguWood::AtlasSize / PinguWood::AtlasHeight;
	/** A nail's head is 18.4 texels across (its rect 40 holds its contact shadow). */
	constexpr float NailDiameter = 18.4f;

	static uint32 Mix(uint32 A, uint32 B)
	{
		uint32 H = A * 0x9E3779B1u ^ (B + 0x7F4A7C15u + (A << 6) + (A >> 2));
		H ^= H >> 15;
		H *= 0x2C1B3C6Du;
		H ^= H >> 12;
		return H;
	}

	struct FHolder
	{
		TStrongObjectPtr<UTexture> Atlas;
		bool bTried = false;
		FSlateBrush Brush;
		bool bBrush = false;
	};
	static FHolder& Holder()
	{
		static FHolder Value;
		return Value;
	}

	/** One strip at uniform scale K (units per texel), body top left at (X, Y), body W long: the columns of UI_STYLE section 2,
	 *  the middle's tiles from PinguWood::PlankTiles (variant, mirrored, length). */
	static void LayoutStrip(const PinguWood::FStrip& S, float X, float Y, float W, float K, float TopRow, uint32 Seed, bool bMirrorOk,
		TArray<PinguWood::FQuad>& Out, int32 StartState = -1)
	{
		const float Top = Y - TopRow * K;
		const float Bottom = Top + S.H * K;
		float CapK = K;
		float M = W - 2.f * S.Cap * K;
		const float MinMid = 0.25f * S.Tile * K;
		if (M < MinMid)
		{
			// (Narrower than the class allows: the ends along shrink a little, the middle keeps a quarter tile.)
			CapK = FMath::Max(W - MinMid, 0.f) / (2.f * S.Cap);
			M = W - 2.f * S.Cap * CapK;
		}
		static TArray<PinguWood::FTilePick> Picks;
		PinguWood::PlankTiles(Seed, M, S.Tile * K, S.Tiles, bMirrorOk, Picks, bMirrorOk ? StartState : (StartState >= 0 ? StartState % S.Tiles : -1));
		float Cursor = X - S.PadSide * CapK;
		auto Add = [&Out, Top, Bottom, &S](float X0, float X1, float U0, float U1)
		{
			PinguWood::FQuad Q;
			Q.X0 = X0; Q.X1 = X1; Q.Y0 = Top; Q.Y1 = Bottom;
			Q.U0 = U0; Q.U1 = U1; Q.V0 = S.Y * VScale; Q.V1 = (S.Y + S.H) * VScale;
			Out.Add(Q);
		};
		const float LeftW = (S.PadSide + S.Cap) * CapK;
		Add(Cursor, Cursor + LeftW, S.X, S.X + S.PadSide + S.Cap);
		Cursor += LeftW;
		for (int32 Index = 0; Index < Picks.Num(); ++Index)
		{
			const PinguWood::FTilePick& Pick = Picks[Index];
			const float T0 = S.X + S.PadSide + S.Cap + static_cast<float>(Pick.Variant) * S.Tile;
			// (The last tile ends exactly where the right cap starts: no float drift.)
			const float Next = Index + 1 == Picks.Num() ? X + W - S.Cap * CapK : Cursor + Pick.Length;
			// A mirrored tile runs its texels right to left: both its ends are the shared join column, so it still joins.
			Add(Cursor, Next, Pick.bMirrored ? T0 + S.Tile : T0, Pick.bMirrored ? T0 : T0 + S.Tile);
			Cursor = Next;
		}
		const float U0 = S.X + S.PadSide + S.Cap + static_cast<float>(S.Tiles) * S.Tile;
		Add(Cursor, Cursor + (S.W - (U0 - S.X)) * CapK, U0, S.X + S.W);
	}
}

void PinguWood::PlankTiles(uint32 Seed, float Middle, float TileLength, int32 Tiles, bool bMirrorOk, TArray<FTilePick>& Out, int32 StartState)
{
	using namespace PinguWoodPrivate;
	Out.Reset();
	Tiles = FMath::Max(Tiles, 1);
	const int32 N = FMath::Max(1, FMath::RoundToInt(Middle / FMath::Max(TileLength, 0.001f)));
	float Total = 0.f;
	int32 Previous = -1;
	for (int32 Index = 0; Index < N; ++Index)
	{
		const uint32 Base = static_cast<uint32>(Index) * 7u;
		const uint32 H = Mix(Seed, Base + 3u);
		FTilePick& Pick = Out.AddDefaulted_GetRef();
		if (StartState >= 0)
		{
			// A given start (the menus: neighbours got different ones): the tiles step 3 states on from it (of 8: 4 tiles x
			// mirrored; of 4 without mirroring). Two planks with different starts then differ at every tile, and no tile follows
			// itself (3 is never a whole number of 4).
			const int32 States = bMirrorOk ? 2 * Tiles : Tiles;
			const int32 State = (StartState + 3 * Index) % States;
			Pick.Variant = State % Tiles;
			Pick.bMirrored = bMirrorOk && State >= Tiles;
		}
		else
		{
			// From the seed alone: the first tile any of the strip's, every next one another than the one before (never the same
			// stretch twice running), mirrored by a coin.
			Pick.Variant = Previous < 0 || Tiles < 2 ? static_cast<int32>(H % static_cast<uint32>(Tiles))
				: (Previous + 1 + static_cast<int32>((H >> 4) % static_cast<uint32>(Tiles - 1))) % Tiles;
			Pick.bMirrored = bMirrorOk && ((Mix(Seed, Base + 5u) >> 7) & 1u) != 0u;
		}
		Previous = Pick.Variant;
		// Lengths jittered 0.85 .. 1.15 (then fitted to the middle): two planks of one width never have their joins in a line.
		Pick.Length = N > 1 ? 0.85f + 0.30f * static_cast<float>((Mix(Seed, Base + 6u) >> 8) % 1000u) / 999.f : 1.f;
		Total += Pick.Length;
	}
	for (FTilePick& Pick : Out)
	{
		Pick.Length *= Middle / FMath::Max(Total, 0.001f);
	}
}

const PinguWood::FHotLook& PinguWood::HotLook()
{
	static const FHotLook Look;
	return Look;
}

UTexture* PinguWood::GetAtlas()
{
	using namespace PinguWoodPrivate;
	FHolder& H = Holder();
	if (!H.bTried)
	{
		H.bTried = true;
		if (UTexture* Texture = LoadObject<UTexture>(nullptr, AtlasPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			H.Atlas.Reset(Texture);
		}
		UE_LOG(LogPingu, Log, TEXT("[WOOD] UI wood atlas (cabin) %s"), H.Atlas.IsValid() ? TEXT("ok") : TEXT("missing (the old shaded wood)"));
	}
	return H.Atlas.Get();
}

PinguWood::FStrip PinguWood::Strip(EClass Class, EState State)
{
	using namespace PinguWoodPrivate;
	const int32 C = FMath::Clamp(static_cast<int32>(Class), 0, static_cast<int32>(EClass::Count) - 1);
	const EState Picture = PictureOf(State);
	int32 Variant = Picture == EState::Hovered ? 1 : Picture == EState::Disabled ? 2 : 0;
	if (StripRects[C][Variant].X < 0.f)
	{
		Variant = 0;
	}
	const FStripRect& R = StripRects[C][Variant];
	FStrip S;
	S.X = R.X; S.Y = R.Y; S.W = R.W; S.H = R.H; S.BodyH = R.BodyH;
	S.Tiles = AtlasTiles;
	if (Class == EClass::Track)
	{
		S.PadSide = 16.f; S.PadTop = 12.f; S.PadBottom = 12.f;
	}
	if (Class == EClass::Snow)
	{
		S.TopLine = SnowTopLine;
		S.PadTop = SnowTopLine;
		S.PadBottom = 0.f;
	}
	return S;
}

PinguWood::FRect PinguWood::Sprite(ESprite Piece)
{
	using namespace PinguWoodPrivate;
	return SpriteRects[FMath::Clamp(static_cast<int32>(Piece), 0, static_cast<int32>(ESprite::Count) - 1)];
}

PinguWood::FRect PinguWood::Solid()
{
	// (In UV texels, as the painters read it: V scaled like the quads'.)
	FRect R = Sprite(ESprite::Solid);
	R.Y *= PinguWoodPrivate::VScale;
	R.H *= PinguWoodPrivate::VScale;
	return R;
}

float PinguWood::BodyDesign(EClass Class)
{
	return Strip(Class, EState::Normal).BodyH / TexelsPerDesignPx;
}

PinguWood::EClass PinguWood::ClassOf(ECut Cut, EState State, float HeightDesign)
{
	switch (Cut)
	{
	case ECut::Board: return EClass::Board;
	case ECut::Pill: return EClass::Pill;
	case ECut::Row: return EClass::Row;
	case ECut::Banner: return EClass::Banner;
	case ECut::Title: return EClass::ButtonL;
	case ECut::Track: return EClass::Track;
	default:
		if (State == EState::Inset)
		{
			return EClass::Row;
		}
		// (The nearest button class: small 94, large 116.)
		return HeightDesign >= 105.f ? EClass::ButtonL : EClass::ButtonS;
	}
}

PinguWood::EState PinguWood::PictureOf(EState State)
{
	switch (State)
	{
	case EState::Hovered:
	case EState::Focused: return EState::Hovered;
	case EState::Disabled: return EState::Disabled;
	default: return EState::Normal;
	}
}

float PinguWood::ShadeOf(EState State)
{
	return State == EState::Pressed ? 0.86f : 1.f;
}

bool PinguWood::HasSnow(ECut Cut)
{
	return Cut == ECut::Auto || Cut == ECut::Sign || Cut == ECut::Title || Cut == ECut::Banner || Cut == ECut::Board;
}

bool PinguWood::HasNails(ECut Cut)
{
	return Cut == ECut::Board || Cut == ECut::Title || Cut == ECut::Banner;
}

bool PinguWood::HasIcicles(ECut Cut)
{
	return Cut == ECut::Title || Cut == ECut::Banner;
}

int32 PinguWood::PreferredStart(uint32 Seed)
{
	using namespace PinguWoodPrivate;
	return static_cast<int32>(Mix(Seed, 0x51u) % static_cast<uint32>(StartStates));
}

bool PinguWood::AreNeighbours(const FVector2D& CentreA, float WA, float HA, const FVector2D& CentreB, float WB, float HB)
{
	const float DX = static_cast<float>(FMath::Abs(CentreA.X - CentreB.X));
	const float DY = static_cast<float>(FMath::Abs(CentreA.Y - CentreB.Y));
	const float Overlap = FMath::Min(static_cast<float>(CentreA.X) + 0.5f * WA, static_cast<float>(CentreB.X) + 0.5f * WB)
		- FMath::Max(static_cast<float>(CentreA.X) - 0.5f * WA, static_cast<float>(CentreB.X) - 0.5f * WB);
	const float Tall = FMath::Max(HA, HB);
	const bool bStacked = DY > 0.3f * Tall && DY < 1.6f * Tall && Overlap > 0.5f * FMath::Min(WA, WB);
	const bool bInRow = DY <= 0.3f * Tall && DX - 0.5f * (WA + WB) < 40.f;
	return bStacked || bInRow;
}

void PinguWood::LayoutPlank(EClass Class, EState State, float W, float H, uint32 Seed, float UnitsPerDesignPx, TArray<FQuad>& Out, bool bOneBoard,
	int32 StartState)
{
	using namespace PinguWoodPrivate;
	if (W <= 0.f || H <= 0.f)
	{
		return;
	}
	const FStrip S = Strip(Class, State);
	// Taller than 1.25x its class: horizontal boards stacked (UI_STYLE section 3), the lowest first so each board's baked shadow
	// falls on the one under it.
	const int32 Boards = bOneBoard ? 1 : StackCount(Class, H, UnitsPerDesignPx);
	const float BoardH = H / static_cast<float>(Boards);
	const float K = BoardH / S.BodyH;
	for (int32 Board = Boards - 1; Board >= 0; --Board)
	{
		// (A given start: the top board takes it, each board under it 3 further on, so stacked boards differ too.)
		const int32 BoardStart = StartState >= 0 ? (StartState + 3 * Board) % StartStates : -1;
		LayoutStrip(S, 0.f, BoardH * static_cast<float>(Board), W, K, S.PadTop, Mix(Seed, 101u + static_cast<uint32>(Board)), Class != EClass::Snow, Out,
			BoardStart);
	}
}

int32 PinguWood::StackCount(EClass Class, float H, float UnitsPerDesignPx)
{
	const float Body = BodyDesign(Class) * UnitsPerDesignPx;
	return H > 1.25f * Body ? FMath::Max(2, FMath::RoundToInt(H / Body)) : 1;
}

void PinguWood::LayoutSnow(float W, float H, uint32 Seed, float UnitsPerDesignPx, TArray<FQuad>& Out)
{
	using namespace PinguWoodPrivate;
	if (W <= 0.f || H <= 0.f)
	{
		return;
	}
	const FStrip S = Strip(EClass::Snow, EState::Normal);
	// kS = (screen px per design px / 2) * clamp(H_design / 104, 0.55, 1.0) (UI_STYLE section 4 says 0.8; the menus' stacked
	// 44 - 56 px buttons need thinner snow so it never covers the button above: UI_STYLE section 10).
	const float HDesign = H / FMath::Max(UnitsPerDesignPx, 0.001f);
	const float K = UnitsPerDesignPx / TexelsPerDesignPx * FMath::Clamp(HDesign / 104.f, 0.55f, 1.f);
	// (The snow is never mirrored: its lumps are lit from the upper left.)
	LayoutStrip(S, 0.f, 0.f, W, K, S.TopLine, Mix(Seed, 77u), false, Out);
}

void PinguWood::LayoutSprite(ESprite Piece, float CX, float CY, float Scale, bool bTop, TArray<FQuad>& Out)
{
	const FRect R = Sprite(Piece);
	FQuad Q;
	Q.X0 = CX - 0.5f * R.W * Scale;
	Q.X1 = CX + 0.5f * R.W * Scale;
	Q.Y0 = bTop ? CY : CY - 0.5f * R.H * Scale;
	Q.Y1 = Q.Y0 + R.H * Scale;
	Q.U0 = R.X; Q.U1 = R.X + R.W; Q.V0 = R.Y * PinguWoodPrivate::VScale; Q.V1 = (R.Y + R.H) * PinguWoodPrivate::VScale;
	Out.Add(Q);
}

void PinguWood::LayoutNails(float W, float H, float UnitsPerDesignPx, TArray<FQuad>& Out)
{
	using namespace PinguWoodPrivate;
	if (W <= 0.f || H <= 0.f)
	{
		return;
	}
	const float U = FMath::Max(UnitsPerDesignPx, 0.001f);
	const float HDesign = H / U;
	// Inset max(15, 0.22 H) from each end; one at mid height, or two at 30 % / 70 % from 110 design px (UI_STYLE section 4).
	const float Inset = FMath::Max(15.f, 0.22f * HDesign) * U;
	// (9 design px across at a normal height, a little smaller on thin planks.)
	const float Scale = U / TexelsPerDesignPx * FMath::Clamp(HDesign / 104.f, 0.75f, 1.15f);
	const bool bTwo = HDesign >= 110.f;
	int32 Which = 0;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float X = Side == 0 ? Inset : W - Inset;
		if (bTwo)
		{
			LayoutSprite(Which++ % 2 == 0 ? ESprite::Nail0 : ESprite::Nail1, X, 0.3f * H, Scale, false, Out);
			LayoutSprite(Which++ % 2 == 0 ? ESprite::Nail0 : ESprite::Nail1, X, 0.7f * H, Scale, false, Out);
		}
		else
		{
			LayoutSprite(Which++ % 2 == 0 ? ESprite::Nail0 : ESprite::Nail1, X, 0.5f * H, Scale, false, Out);
		}
	}
}

void PinguWood::LayoutIcicles(float W, float H, uint32 Seed, float UnitsPerDesignPx, TArray<FQuad>& Out)
{
	using namespace PinguWoodPrivate;
	if (W <= 0.f || H <= 0.f)
	{
		return;
	}
	const float U = FMath::Max(UnitsPerDesignPx, 0.001f);
	const float Scale = U / TexelsPerDesignPx * FMath::Clamp(H / U / 116.f, 0.75f, 1.25f);
	// 3 to 5 by length, spaced unevenly, variants 0..4; hung 1 px into the lower edge.
	const int32 Count = FMath::Clamp(FMath::RoundToInt(W / U / 200.f), 3, 5);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Jitter = (static_cast<float>(Mix(Seed, 11u + static_cast<uint32>(Index)) % 1000u) / 1000.f - 0.5f) * 0.5f;
		const float T = (static_cast<float>(Index) + 0.5f + Jitter) / static_cast<float>(Count);
		const ESprite Piece = static_cast<ESprite>(static_cast<int32>(ESprite::Icicle0) + static_cast<int32>(Mix(Seed, 31u + static_cast<uint32>(Index)) % 5u));
		LayoutSprite(Piece, FMath::Lerp(0.08f, 0.92f, T) * W, H - 1.f * U, Scale, true, Out);
	}
}

bool PinguWood::PaintQuads(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& Offset, const TArray<FQuad>& Quads,
	const FLinearColor& Tint, float Scale)
{
	using namespace PinguWoodPrivate;
	UTexture* Atlas = GetAtlas();
	if (!Atlas || Quads.Num() == 0 || !FSlateApplication::IsInitialized())
	{
		return Atlas != nullptr;
	}
	FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
	if (!Renderer)
	{
		return false;
	}
	FHolder& H = Holder();
	if (!H.bBrush)
	{
		H.bBrush = true;
		H.Brush.SetResourceObject(Atlas);
		H.Brush.ImageSize = FVector2D(AtlasSize, AtlasHeight);
		H.Brush.DrawAs = ESlateBrushDrawType::Image;
	}
	const FSlateResourceHandle Handle = Renderer->GetResourceHandle(H.Brush, FVector2f(0.f, 0.f), 1.f);
	const FSlateShaderResourceProxy* Proxy = Handle.GetResourceProxy();
	if (!Proxy)
	{
		return false;
	}
	const FVector2f Start(Proxy->StartUV);
	const FVector2f SizeUV(Proxy->SizeUV);
	const FSlateRenderTransform& ToRender = Geometry.GetAccumulatedRenderTransform();
	const FColor Color = Tint.ToFColor(true);
	static TArray<FSlateVertex> Verts;
	static TArray<SlateIndex> Indices;
	Verts.Reset();
	Indices.Reset();
	for (const FQuad& Q : Quads)
	{
		const FVector2f Corners[4] = { { Q.X0, Q.Y0 }, { Q.X1, Q.Y0 }, { Q.X1, Q.Y1 }, { Q.X0, Q.Y1 } };
		const FVector2f Uvs[4] = { { Q.U0, Q.V0 }, { Q.U1, Q.V0 }, { Q.U1, Q.V1 }, { Q.U0, Q.V1 } };
		const int32 First = Verts.Num();
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			FSlateVertex V;
			const FVector2f UV = Start + SizeUV * (Uvs[Corner] / AtlasSize);
			V.TexCoords[0] = UV.X;
			V.TexCoords[1] = UV.Y;
			V.TexCoords[2] = 1.f;
			V.TexCoords[3] = 1.f;
			V.MaterialTexCoords = UV;
			V.Position = ToRender.TransformPoint(Offset + Corners[Corner] * Scale);
			V.Color = Color;
			V.SecondaryColor = FColor(0, 0, 0, 0);
			V.PixelSize[0] = 0;
			V.PixelSize[1] = 0;
			Verts.Add(V);
		}
		Indices.Append({ static_cast<SlateIndex>(First), static_cast<SlateIndex>(First + 1), static_cast<SlateIndex>(First + 2),
			static_cast<SlateIndex>(First), static_cast<SlateIndex>(First + 2), static_cast<SlateIndex>(First + 3) });
	}
	FSlateDrawElement::MakeCustomVerts(Out, Layer, Handle, Verts, Indices, nullptr, 0, 0, ESlateDrawEffect::None, ESlateBatchDrawFlag::None);
	return true;
}

bool PinguWood::PaintPlank(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, const FVector2f& TopLeft, const FVector2f& Size,
	const FPlankLook& Look, float Scale)
{
	if (!GetAtlas())
	{
		return false;
	}
	static TArray<FQuad> Quads;
	Quads.Reset();
	const EClass Class = ClassOf(Look.Cut, Look.State, Size.Y);
	LayoutPlank(Class, Look.State, Size.X, Size.Y, Look.Seed, 1.f, Quads, Look.bOneBoard);
	const bool bSnow = Look.Snow >= 0 ? Look.Snow > 0 : HasSnow(Look.Cut);
	const bool bNails = Look.Nails >= 0 ? Look.Nails > 0 : HasNails(Look.Cut);
	const bool bIcicles = Look.Icicles >= 0 ? Look.Icicles > 0 : HasIcicles(Look.Cut);
	if (bNails)
	{
		// (A stacked panel: its nails per board.)
		const int32 Boards = Look.bOneBoard ? 1 : StackCount(Class, Size.Y, 1.f);
		const float BoardH = Size.Y / static_cast<float>(Boards);
		for (int32 Board = 0; Board < Boards; ++Board)
		{
			const int32 First = Quads.Num();
			LayoutNails(Size.X, BoardH, 1.f, Quads);
			for (int32 Index = First; Index < Quads.Num(); ++Index)
			{
				Quads[Index].Y0 += BoardH * static_cast<float>(Board);
				Quads[Index].Y1 += BoardH * static_cast<float>(Board);
			}
		}
	}
	if (bIcicles)
	{
		LayoutIcicles(Size.X, Size.Y, Look.Seed, 1.f, Quads);
	}
	if (bSnow)
	{
		LayoutSnow(Size.X, Size.Y, Look.Seed, 1.f, Quads);
	}
	const float Shade = ShadeOf(Look.State);
	return PaintQuads(Out, Layer, Geometry, TopLeft, Quads, FLinearColor(Shade, Shade, Shade, Look.Opacity), Scale);
}

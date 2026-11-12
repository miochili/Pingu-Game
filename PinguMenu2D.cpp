// Pingu - the 2D menus' painter (see PinguMenu2D.h): every 2D part of the menu and of the lobby island drawn as shaded Slate
// geometry, in the 3D menu's own projection.

#include "PinguMenu2D.h"
#include "PinguMenu3D.h"
#include "PinguLobby.h"
#include "Penguin.h"
#include "PinguUI.h"
#include "PinguWoodStyle.h"
#include "Algo/Reverse.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture.h"
#include "GameFramework/PlayerController.h"
#include "SceneView.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Geometry.h"
#include "Rendering/DrawElements.h"
#include "Rendering/RenderingCommon.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"

namespace PinguMenu2DPrivate
{
	static uint32 GPartOrder = 0;
	static int32 GPaintedParts = 0;
	/** The first tile of every menu plank in this paint (PinguMenu2D::GetWoodStart). */
	static TMap<const UPinguMenu2DPart*, int8> GWoodStarts;
	static int32 GPaintedDraws = 0;
	static int32 GPaintedVerts = 0;
}

UPinguMenu2DPart::UPinguMenu2DPart()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCanEverAffectNavigation(false);
	SetGenerateOverlapEvents(false);
	CastShadow = false;
	Mobility = EComponentMobility::Movable;
	Order = ++PinguMenu2DPrivate::GPartOrder;
}

namespace PinguMenu2DPrivate
{
	// ---- the grain texture (Tools/ArtGen/make_ui_wood.py T_UI2D_Grain: its numbers are printed by the script)
	static const TCHAR* GrainPath = TEXT("/Game/UI/Menu3D/T_UI2D_Grain.T_UI2D_Grain");
	/** The 3D wood's grain factor f1 = 0.35 + 1.3 x linear grey at Grain 1: its smallest and largest value in the texture. */
	constexpr float GrainF1Min = 0.3591f;
	constexpr float GrainF1Max = 0.9731f;
	/** The middle of the texture's block of solid alpha (what the plain shapes sample). */
	static const FVector2f SolidTexel(0.26953f, 0.35938f);

	// ---- the light (the 3D menu's two directional lights; in view space: X away from the camera, Y right, Z up), the sky's
	// bluish light and the picture's response (the engine's filmic tone curve), fitted to the 3D menu's screenshots: the faces
	// of the natural, light, dark and painted signs, the boards and the arrow knobs (2026-10-27, within about 10 %).
	static const FVector KeyDir(-0.696, -0.370, 0.616);
	static const FVector FillDir(-0.720, 0.604, -0.342);
	static const FLinearColor Ambient(0.153f, 0.386f, 0.633f);
	constexpr float KeyGain = 0.874f;
	constexpr float FillGain = 0.114f;
	/** The island's light on the lobby's tags (they were lit by the sun, not by the menu's light). */
	constexpr float WorldGain = 1.f;
	/** The menu's exposure on the darkened pages and on the lobby's strip (the title's is 1). */
	constexpr float DimExposure = 2.1f;
	constexpr float LobbyExposure = 0.8f;
	/** The slab pieces' bevel: its width against the corner radius (make_ui_meshes.py BEVEL). */
	constexpr float Bevel = 0.55f;
	/** The wooden signs' pieces (make_ui_meshes.py, in its units of 100). */
	constexpr float TipBevel = 22.f;
	constexpr float PointerBevel = 30.f;
	static const FLinearColor SnowWhite(0.92f, 0.95f, 1.f);
	static const FLinearColor SnowShade(0.42f, 0.55f, 0.78f);
	static const FLinearColor NailColor(0.11f, 0.11f, 0.12f);
	static const FLinearColor NailLip(0.03f, 0.03f, 0.035f);
	static const FLinearColor IceColor(0.55f, 0.78f, 0.95f);
	static const FLinearColor IceLip(0.85f, 0.95f, 1.f);
	/** The focus rope's strands (M_Menu3D_Rope's Period). */
	constexpr float RopePeriod = 7.f;

	static float Smooth(float E0, float E1, float X)
	{
		const float T = FMath::Clamp((X - E0) / (E1 - E0), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	/** A camera: where it is, its axes, its field of view, the viewport's size. */
	struct FCam
	{
		FVector O = FVector::ZeroVector;
		FVector F = FVector::ForwardVector;
		FVector R = FVector::RightVector;
		FVector U = FVector::UpVector;
		double TanX = 0.5;
		double TanY = 0.3;
		double W = 1280.0;
		double H = 720.0;
		/** The engine's own view projection (the world's parts, 2026-10-29): what the island, the penguins, the tags' words and
		 *  the clicks are projected with - at any aspect (the engine keeps the vertical field of view of a 16:9 picture, so a
		 *  home-made projection drifted sideways in a 1.62 window: Linus's tags sat beside their words). */
		bool bMatrix = false;
		FMatrix ViewProj = FMatrix::Identity;
		FVector2D RectMin = FVector2D::ZeroVector;
		FVector2D RectSize = FVector2D(1280.0, 720.0);
	};

	/** A part's projection: its local points to viewport pixels, its local normals to the view. */
	struct FProj
	{
		FVector Ax[3];
		double C[3] = { 0.0, 0.0, 0.0 };
		FQuat Rot = FQuat::Identity;
		FVector InvScale = FVector::OneVector;
		FVector Scale = FVector::OneVector;
		const FCam* Cam = nullptr;
		/** Local -> clip space (the engine's projection, Cam->bMatrix). */
		FMatrix Clip = FMatrix::Identity;

		bool Project(const FVector& Local, FVector2D& OutPixel) const
		{
			if (Cam->bMatrix)
			{
				// As FSceneView::ProjectWorldToScreen.
				const FVector4 V = Clip.TransformFVector4(FVector4(Local, 1.0));
				if (V.W <= 0.01)
				{
					return false;
				}
				OutPixel.X = Cam->RectMin.X + (0.5 + 0.5 * V.X / V.W) * Cam->RectSize.X;
				OutPixel.Y = Cam->RectMin.Y + (0.5 - 0.5 * V.Y / V.W) * Cam->RectSize.Y;
				return true;
			}
			const double Fw = FVector::DotProduct(Ax[0], Local) + C[0];
			if (Fw < 1.0)
			{
				return false;
			}
			const double Rt = FVector::DotProduct(Ax[1], Local) + C[1];
			const double Up = FVector::DotProduct(Ax[2], Local) + C[2];
			OutPixel.X = 0.5 * Cam->W * (1.0 + Rt / (Fw * Cam->TanX));
			OutPixel.Y = 0.5 * Cam->H * (1.0 - Up / (Fw * Cam->TanY));
			return true;
		}
		/** A local normal (of the part's unscaled shape) as a view-space direction (the inverse transpose of the scale). */
		FVector Normal(const FVector& Local) const
		{
			const FVector World = Rot.RotateVector(Local * InvScale);
			const FVector View(FVector::DotProduct(Cam->F, World), FVector::DotProduct(Cam->R, World), FVector::DotProduct(Cam->U, World));
			return View.GetSafeNormal();
		}
	};

	static FProj MakeProj(const FTransform& T, const FCam& Cam)
	{
		FProj P;
		P.Cam = &Cam;
		const FMatrix M = T.ToMatrixWithScale();
		const FVector X = M.GetScaledAxis(EAxis::X);
		const FVector Y = M.GetScaledAxis(EAxis::Y);
		const FVector Z = M.GetScaledAxis(EAxis::Z);
		const FVector Org = M.GetOrigin() - Cam.O;
		const FVector* Axes[3] = { &Cam.F, &Cam.R, &Cam.U };
		for (int32 Row = 0; Row < 3; ++Row)
		{
			const FVector& A = *Axes[Row];
			P.Ax[Row] = FVector(FVector::DotProduct(A, X), FVector::DotProduct(A, Y), FVector::DotProduct(A, Z));
			P.C[Row] = FVector::DotProduct(A, Org);
		}
		if (Cam.bMatrix)
		{
			P.Clip = M * Cam.ViewProj;
		}
		P.Rot = T.GetRotation();
		P.Scale = T.GetScale3D();
		auto Inv = [](double S) { return FMath::Abs(S) > 1e-6 ? 1.0 / S : 0.0; };
		P.InvScale = FVector(Inv(P.Scale.X), Inv(P.Scale.Y), Inv(P.Scale.Z));
		return P;
	}

	/** How a shape is coloured: the 3D material's colours, wood, a shadow. */
	struct FLook
	{
		FLinearColor Face = FLinearColor::White;
		FLinearColor Lip = FLinearColor::Gray;
		float Glow = 0.f;
		float Gain = 1.f;
		float Spec = 0.f;
		float Shine = 20.f;
		bool bShadow = false;
		float Opacity = 1.f;
		bool bWood = false;
		float Grain = 0.f;
		FVector2D GrainOffset = FVector2D::ZeroVector;
		FVector2D GrainScale = FVector2D(520.f, 150.f);
		bool bVertical = false;
		/** The grain's space: this part's own (null) or the part it takes its wood from (an arrow point: its plank). */
		const FTransform* GrainFrom = nullptr;
		const FTransform* Own = nullptr;
	};

	/** The lit colour of an albedo with a view-space normal (the menu's two lights, the sky, a little shine, its glow). */
	static FLinearColor Lit(const FLinearColor& Albedo, const FVector& N, const FLook& Look)
	{
		const float Key = FMath::Max(0.f, static_cast<float>(FVector::DotProduct(N, KeyDir)));
		const float Fill = FMath::Max(0.f, static_cast<float>(FVector::DotProduct(N, FillDir)));
		const float Direct = KeyGain * Key + FillGain * Fill;
		const FLinearColor Light = FLinearColor(Ambient.R + Direct, Ambient.G + Direct, Ambient.B + Direct, 1.f) * Look.Gain;
		FLinearColor Color = Albedo * Light + Albedo * Look.Glow;
		if (Look.Spec > 0.f)
		{
			static const FVector Half = (KeyDir + FVector(-1.0, 0.0, 0.0)).GetSafeNormal();
			const float S = Look.Spec * FMath::Pow(FMath::Max(0.f, static_cast<float>(FVector::DotProduct(N, Half))), Look.Shine);
			Color += FLinearColor(S, S, S, 0.f);
		}
		return Color;
	}

	/** The picture's response: the filmic curve the 3D menu went through (ACES, fitted): saturated paint stays bright, the
	 *  dark wood keeps its depth, the snow tops out a little under white. */
	static FColor Display(const FLinearColor& Color, float Alpha)
	{
		auto Curve = [](float X)
		{
			X = FMath::Max(X, 0.f);
			return FMath::Clamp((X * (2.51f * X + 0.03f)) / (X * (2.43f * X + 0.59f) + 0.14f), 0.f, 1.f);
		};
		return FLinearColor(Curve(Color.R), Curve(Color.G), Curve(Color.B), FMath::Clamp(Alpha, 0.f, 1.f)).ToFColor(true);
	}

	/** The wood's base brightness and its grain's darkening for a grain strength K (see make_ui_wood.py). */
	static float WoodBase(float K)
	{
		return 1.f + K * (GrainF1Max - 1.f);
	}
	static float GrainAlpha(float K)
	{
		return FMath::Clamp(K * (GrainF1Max - GrainF1Min) / FMath::Max(WoodBase(K), 0.1f), 0.f, 1.f);
	}

	/** The vertices and triangles of one Slate draw. */
	struct FBuild
	{
		TArray<FSlateVertex> Verts;
		TArray<SlateIndex> Indices;
		/** Viewport pixel -> Slate's render space (an affine map). */
		FVector2D PxOrigin = FVector2D::ZeroVector;
		FVector2D PxX = FVector2D(1.0, 0.0);
		FVector2D PxY = FVector2D(0.0, 1.0);
		FVector2f SolidUV = FVector2f(0.5f, 0.5f);
		bool bGrain = false;
		/** The rendered wood atlas is the draw's texture (PinguWoodStyle.h): its place in Slate's texture space. */
		bool bAtlas = false;
		FVector2f AtlasStart = FVector2f(0.f, 0.f);
		FVector2f AtlasSizeUV = FVector2f(1.f, 1.f);
		FVector2f AtlasUV(float Px, float Py) const
		{
			return AtlasStart + AtlasSizeUV * FVector2f(Px / PinguWood::AtlasSize, Py / PinguWood::AtlasSize);
		}

		int32 Add(const FVector2D& Pixel, const FColor& Color, const FVector2f& UV)
		{
			FSlateVertex Vertex;
			Vertex.TexCoords[0] = UV.X;
			Vertex.TexCoords[1] = UV.Y;
			Vertex.TexCoords[2] = 1.f;
			Vertex.TexCoords[3] = 1.f;
			Vertex.MaterialTexCoords = UV;
			const FVector2D Pos = PxOrigin + PxX * Pixel.X + PxY * Pixel.Y;
			Vertex.Position = FVector2f(static_cast<float>(Pos.X), static_cast<float>(Pos.Y));
			Vertex.Color = Color;
			Vertex.SecondaryColor = FColor(0, 0, 0, 0);
			Vertex.PixelSize[0] = 0;
			Vertex.PixelSize[1] = 0;
			return Verts.Add(Vertex);
		}
		int32 AddSolid(const FVector2D& Pixel, const FColor& Color)
		{
			return Add(Pixel, Color, SolidUV);
		}
		/** A soft edge (2026-10-29, Linus: "make the loby gui ... look higher quality"): a band FeatherPixels wide outside a
		 *  chain of already added vertices (by index), fading from their colour to nothing - the shapes' edges are smooth at any
		 *  resolution instead of stair-stepped. The chain's own winding says which side is outside. */
		void Feather(const int32* Chain, int32 Count, bool bClosed, float FeatherPixels = 1.1f)
		{
			if (Count < 2)
			{
				return;
			}
			// One viewport pixel in render units, the chain's winding (its signed area).
			const float Pixel = static_cast<float>(PxX.Size()) * FeatherPixels;
			double Area = 0.0;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FVector2f A = Verts[Chain[Index]].Position;
				const FVector2f Bp = Verts[Chain[(Index + 1) % Count]].Position;
				Area += static_cast<double>(A.X) * Bp.Y - static_cast<double>(Bp.X) * A.Y;
			}
			if (FMath::Abs(Area) < 1.0)
			{
				return;
			}
			const float Side = (Area > 0.0 ? 1.f : -1.f) * FeatherSide;
			const int32 First = Verts.Num();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const int32 PrevIndex = bClosed ? (Index - 1 + Count) % Count : FMath::Max(Index - 1, 0);
				const int32 NextIndex = bClosed ? (Index + 1) % Count : FMath::Min(Index + 1, Count - 1);
				const FVector2f P0 = Verts[Chain[PrevIndex]].Position;
				const FVector2f P1 = Verts[Chain[Index]].Position;
				const FVector2f P2 = Verts[Chain[NextIndex]].Position;
				auto Out = [Side](const FVector2f& From, const FVector2f& To)
				{
					const FVector2f D = (To - From).GetSafeNormal();
					return FVector2f(D.Y, -D.X) * Side;
				};
				FVector2f N = (PrevIndex != Index ? Out(P0, P1) : FVector2f::ZeroVector) + (NextIndex != Index ? Out(P1, P2) : FVector2f::ZeroVector);
				N = N.GetSafeNormal();
				FSlateVertex Vertex = Verts[Chain[Index]];
				Vertex.Position = P1 + N * Pixel;
				Vertex.Color.A = 0;
				Verts.Add(Vertex);
			}
			const int32 Segments = bClosed ? Count : Count - 1;
			for (int32 Index = 0; Index < Segments; ++Index)
			{
				const int32 Next = (Index + 1) % Count;
				Quad(Chain[Index], Chain[Next], First + Next, First + Index);
			}
		}
		/** The same band on the other side of a closed chain (a ring's inner edge: the hole). */
		void FeatherInside(const int32* Chain, int32 Count)
		{
			FeatherSide = -1.f;
			Feather(Chain, Count, true);
			FeatherSide = 1.f;
		}
		float FeatherSide = 1.f;
		/** Drops what was added since (a shape that went behind the camera half way). */
		void Rollback(int32 VertCount, int32 IndexCount)
		{
			Verts.SetNum(VertCount);
			Indices.SetNum(IndexCount);
		}
		void Tri(int32 A, int32 B, int32 C)
		{
			if (A < 0 || B < 0 || C < 0)
			{
				return;
			}
			Indices.Add(static_cast<SlateIndex>(A));
			Indices.Add(static_cast<SlateIndex>(B));
			Indices.Add(static_cast<SlateIndex>(C));
		}
		void Quad(int32 A, int32 B, int32 C, int32 D)
		{
			Tri(A, B, C);
			Tri(A, C, D);
		}
	};

	/** A rounded rectangle's outline (Y right, Z up), Inset in from its edge: the points of its four corners (counter-clockwise
	 *  from the top right one) and their outward normals. */
	static void RoundRect(float W, float H, float R, float Inset, int32 Seg, TArray<FVector2D>& OutPts, TArray<FVector2D>& OutNormals)
	{
		OutPts.Reset();
		OutNormals.Reset();
		const float A = FMath::Max(0.5f * W - R, 0.f);
		const float C = FMath::Max(0.5f * H - R, 0.f);
		const float Rad = FMath::Max(R - Inset, 0.f);
		const FVector2D Centers[4] = { FVector2D(A, C), FVector2D(-A, C), FVector2D(-A, -C), FVector2D(A, -C) };
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			for (int32 Step = 0; Step <= Seg; ++Step)
			{
				const float Angle = FMath::DegreesToRadians(90.f * Corner + 90.f * Step / Seg);
				const FVector2D N(FMath::Cos(Angle), FMath::Sin(Angle));
				OutPts.Add(Centers[Corner] + N * Rad);
				OutNormals.Add(N);
			}
		}
	}

	/** How many pixels one local unit of the part is on the screen near its origin. */
	static float PixelsPerUnit(const FProj& P)
	{
		FVector2D A;
		FVector2D B;
		if (!P.Project(FVector::ZeroVector, A) || !P.Project(FVector(0.0, 10.0, 0.0), B))
		{
			return 1.f;
		}
		return static_cast<float>((B - A).Size()) / 10.f;
	}

	/** The grain's texture coordinates of a local point (M_Menu3D_Wood: page units / GrainScale + GrainOffset; Vertical turns it). */
	static FVector2f GrainUV(const FVector& Local, const FLook& Look)
	{
		FVector Space = Local;
		if (Look.GrainFrom && Look.Own)
		{
			Space = Look.GrainFrom->InverseTransformPosition(Look.Own->TransformPosition(Local));
		}
		const double Along = Look.bVertical ? Space.Z : Space.Y;
		const double Across = Look.bVertical ? Space.Y : Space.Z;
		return FVector2f(static_cast<float>(Along / FMath::Max(Look.GrainScale.X, 1.0) + Look.GrainOffset.X),
			static_cast<float>(-Across / FMath::Max(Look.GrainScale.Y, 1.0) + Look.GrainOffset.Y));
	}

	/** The grain over a convex outline (a fan from its first point): black, its texture's alpha the darkening. */
	static void DrawGrain(FBuild& B, const FProj& P, const TArray<FVector>& Outline, const FLook& Look)
	{
		if (!B.bGrain || !Look.bWood || Look.Grain <= 0.01f || Outline.Num() < 3)
		{
			return;
		}
		const FColor Dark = FLinearColor(0.f, 0.f, 0.f, GrainAlpha(Look.Grain) * Look.Opacity).ToFColor(false);
		const int32 First = B.Verts.Num();
		for (const FVector& Point : Outline)
		{
			FVector2D Pixel;
			if (!P.Project(Point, Pixel))
			{
				B.Verts.SetNum(First);
				return;
			}
			B.Add(Pixel, Dark, GrainUV(Point, Look));
		}
		for (int32 Index = 1; Index + 1 < Outline.Num(); ++Index)
		{
			B.Tri(First, First + Index, First + Index + 1);
		}
	}

	/** A rounded, bevelled slab (make_ui_meshes.py's 9-slice pieces): its side walls, its bevel's rings, its face, the grain. */
	static void DrawSlab(FBuild& B, const FProj& P, float W, float H, float R, float D, const FLook& Look)
	{
		R = FMath::Clamp(R, 0.5f, 0.5f * FMath::Min(W, H));
		const float Ppu = PixelsPerUnit(P);
		const int32 Seg = FMath::Clamp(FMath::CeilToInt(R * Ppu / 2.5f), 1, 8);
		const float Base = Look.bWood ? WoodBase(Look.Grain) : 1.f;
		static TArray<FVector2D> Pts;
		static TArray<FVector2D> Nrm;
		if (Look.bShadow)
		{
			// The soft contact shadow: black, solid on the face, fading to nothing across the bevel.
			const FColor Solid = FLinearColor(0.f, 0.f, 0.f, Look.Opacity).ToFColor(false);
			const FColor Clear = FLinearColor(0.f, 0.f, 0.f, 0.f).ToFColor(false);
			RoundRect(W, H, R, Bevel * R, Seg, Pts, Nrm);
			TArray<FVector2D> Inner = Pts;
			RoundRect(W, H, R, 0.f, Seg, Pts, Nrm);
			const int32 N = Pts.Num();
			const int32 First = B.Verts.Num();
			for (int32 Index = 0; Index < N; ++Index)
			{
				FVector2D In;
				FVector2D Out;
				if (!P.Project(FVector(0.0, Inner[Index].X, Inner[Index].Y), In) || !P.Project(FVector(0.0, Pts[Index].X, Pts[Index].Y), Out))
				{
					B.Verts.SetNum(First);
					return;
				}
				B.AddSolid(In, Solid);
				B.AddSolid(Out, Clear);
			}
			for (int32 Index = 0; Index < N; ++Index)
			{
				const int32 Next = (Index + 1) % N;
				B.Quad(First + 2 * Index, First + 2 * Index + 1, First + 2 * Next + 1, First + 2 * Next);
			}
			for (int32 Index = 1; Index + 1 < N; ++Index)
			{
				B.Tri(First, First + 2 * Index, First + 2 * (Index + 1));
			}
			return;
		}
		// The bevel's rings from the outer edge (its side, 90 degrees) to the face (0): its in-plane inset, its depth, its normal.
		const bool bFine = Bevel * R * Ppu >= 1.5f;
		static const float AnglesFine[4] = { 90.f, 60.f, 30.f, 0.f };
		static const float AnglesCoarse[2] = { 90.f, 0.f };
		const float* Angles = bFine ? AnglesFine : AnglesCoarse;
		const int32 Rings = bFine ? 4 : 2;
		RoundRect(W, H, R, 0.f, Seg, Pts, Nrm);
		const int32 N = Pts.Num();
		const int32 First = B.Verts.Num();
		const int32 IndexFirst = B.Indices.Num();
		// The side walls (seen where the slab is turned or off the middle of the picture): from the bevel's back to its depth.
		const float WallFront = D * Bevel;
		{
			for (int32 Index = 0; Index < N; ++Index)
			{
				const FVector Normal = P.Normal(FVector(0.0, Nrm[Index].X, Nrm[Index].Y));
				const FColor Color = Display(Lit(Look.Lip, Normal, Look) * Base, Look.Opacity);
				FVector2D Front;
				FVector2D Back;
				if (!P.Project(FVector(WallFront, Pts[Index].X, Pts[Index].Y), Front) || !P.Project(FVector(D, Pts[Index].X, Pts[Index].Y), Back))
				{
					B.Rollback(First, IndexFirst);
					return;
				}
				B.AddSolid(Back, Color);
				B.AddSolid(Front, Color);
			}
			for (int32 Index = 0; Index < N; ++Index)
			{
				const int32 Next = (Index + 1) % N;
				B.Quad(First + 2 * Index, First + 2 * Next, First + 2 * Next + 1, First + 2 * Index + 1);
			}
		}
		// The bevel: ring by ring.
		const int32 RingFirst = B.Verts.Num();
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			const float Theta = FMath::DegreesToRadians(Angles[Ring]);
			const float Inset = Bevel * R * (1.f - FMath::Sin(Theta));
			const float X = D * Bevel * (1.f - FMath::Cos(Theta));
			const float LipShare = Smooth(0.45f, 1.f, Angles[Ring] / 90.f);
			const FLinearColor Albedo = FMath::Lerp(Look.Face, Look.Lip, LipShare);
			static TArray<FVector2D> RingPts;
			static TArray<FVector2D> RingNrm;
			RoundRect(W, H, R, Inset, Seg, RingPts, RingNrm);
			for (int32 Index = 0; Index < N; ++Index)
			{
				const FVector2D& Out = RingNrm[Index];
				const FVector2D& Point = RingPts[Index];
				// (Normals of the scaled piece: its depth and its radius stretch the bevel's profile differently.)
				const FVector Normal = P.Normal(FVector(-FMath::Cos(Theta) / FMath::Max(D, 0.1f), FMath::Sin(Theta) * Out.X / R, FMath::Sin(Theta) * Out.Y / R));
				FVector2D Pixel;
				if (!P.Project(FVector(X, Point.X, Point.Y), Pixel))
				{
					B.Rollback(First, IndexFirst);
					return;
				}
				B.AddSolid(Pixel, Display(Lit(Albedo, Normal, Look) * Base, Look.Opacity));
			}
		}
		for (int32 Ring = 0; Ring + 1 < Rings; ++Ring)
		{
			const int32 Outer = RingFirst + Ring * N;
			const int32 Inner = RingFirst + (Ring + 1) * N;
			for (int32 Index = 0; Index < N; ++Index)
			{
				const int32 Next = (Index + 1) % N;
				B.Quad(Outer + Index, Outer + Next, Inner + Next, Inner + Index);
			}
		}
		// The face (the last ring is its outline).
		const int32 Face = RingFirst + (Rings - 1) * N;
		for (int32 Index = 1; Index + 1 < N; ++Index)
		{
			B.Tri(Face, Face + Index, Face + Index + 1);
		}
		// Its soft edge round the outer ring.
		{
			static TArray<int32> Rim;
			Rim.Reset();
			for (int32 Index = 0; Index < N; ++Index)
			{
				Rim.Add(RingFirst + Index);
			}
			B.Feather(Rim.GetData(), Rim.Num(), true);
		}
		// The grain over the whole front (face and bevel).
		if (Look.bWood)
		{
			static TArray<FVector> Outline;
			Outline.Reset();
			for (int32 Index = 0; Index < N; ++Index)
			{
				Outline.Add(FVector(0.0, Pts[Index].X, Pts[Index].Y));
			}
			DrawGrain(B, P, Outline, Look);
		}
	}

	/** A path round a rounded rectangle, every Step at most (the rope's strands need the points). */
	static void RoundPath(float W, float H, float R, float Step, TArray<FVector2D>& OutPts, TArray<FVector2D>& OutNormals)
	{
		OutPts.Reset();
		OutNormals.Reset();
		const float A = FMath::Max(0.5f * W - R, 0.f);
		const float C = FMath::Max(0.5f * H - R, 0.f);
		const FVector2D Centers[4] = { FVector2D(A, C), FVector2D(-A, C), FVector2D(-A, -C), FVector2D(A, -C) };
		const int32 ArcSeg = FMath::Clamp(FMath::CeilToInt(0.5f * PI * R / FMath::Max(Step, 0.5f)), 3, 48);
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			for (int32 Seg = 0; Seg <= ArcSeg; ++Seg)
			{
				const float Angle = FMath::DegreesToRadians(90.f * Corner + 90.f * Seg / ArcSeg);
				const FVector2D N(FMath::Cos(Angle), FMath::Sin(Angle));
				OutPts.Add(Centers[Corner] + N * R);
				OutNormals.Add(N);
			}
			// The straight edge to the next corner.
			const FVector2D End = OutPts.Last();
			const int32 NextCorner = (Corner + 1) % 4;
			const float NextAngle = FMath::DegreesToRadians(90.f * NextCorner);
			const FVector2D NextN(FMath::Cos(NextAngle), FMath::Sin(NextAngle));
			const FVector2D Start = Centers[NextCorner] + NextN * R;
			const float Length = static_cast<float>((Start - End).Size());
			const int32 Pieces = FMath::Clamp(FMath::FloorToInt(Length / FMath::Max(Step, 0.5f)), 0, 400);
			const FVector2D EdgeNormal = OutNormals.Last();
			for (int32 Piece = 1; Piece < Pieces; ++Piece)
			{
				OutPts.Add(FMath::Lerp(End, Start, static_cast<double>(Piece) / Pieces));
				OutNormals.Add(EdgeNormal);
			}
		}
	}

	/** A round tube along a rounded rectangle (its centre line W x H, corner R, tube T): the focus rope (twisted strands), a rim. */
	static void DrawRing(FBuild& B, const FProj& P, float W, float H, float R, float T, bool bRope, const FLook& Look)
	{
		if (T <= 0.01f)
		{
			return;
		}
		static TArray<FVector2D> Pts;
		static TArray<FVector2D> Nrm;
		const float Ppu = PixelsPerUnit(P);
		const float Step = bRope ? 0.25f * RopePeriod : FMath::Max(3.f / FMath::Max(Ppu, 0.05f), 0.5f * R);
		RoundPath(W, H, R, Step, Pts, Nrm);
		const int32 N = Pts.Num();
		static const float Angles[5] = { -90.f, -45.f, 0.f, 45.f, 90.f };
		const int32 First = B.Verts.Num();
		for (int32 Row = 0; Row < 5; ++Row)
		{
			const float Phi = FMath::DegreesToRadians(Angles[Row]);
			const float Offset = T * FMath::Sin(Phi);
			const float X = -T * FMath::Cos(Phi);
			for (int32 Index = 0; Index < N; ++Index)
			{
				const FVector2D Point = Pts[Index] + Nrm[Index] * Offset;
				const FVector Normal = P.Normal(FVector(-FMath::Cos(Phi), FMath::Sin(Phi) * Nrm[Index].X, FMath::Sin(Phi) * Nrm[Index].Y));
				FLinearColor Albedo;
				if (bRope)
				{
					// M_Menu3D_Rope: diagonal strands, (|2 frac((y + z) / Period) - 1|)^2 from Color to Lip.
					const float S = static_cast<float>((Point.X + Point.Y) / RopePeriod);
					const float Tri = FMath::Abs(2.f * FMath::Frac(S) - 1.f);
					Albedo = FMath::Lerp(Look.Face, Look.Lip, Tri * Tri);
				}
				else
				{
					Albedo = FMath::Lerp(Look.Face, Look.Lip, 0.5f * (1.f - FMath::Cos(Phi)));
				}
				FVector2D Pixel;
				if (!P.Project(FVector(X, Point.X, Point.Y), Pixel))
				{
					B.Verts.SetNum(First);
					return;
				}
				B.AddSolid(Pixel, Display(Lit(Albedo, Normal, Look), Look.Opacity));
			}
		}
		for (int32 Row = 0; Row < 4; ++Row)
		{
			const int32 A = First + Row * N;
			const int32 C = First + (Row + 1) * N;
			for (int32 Index = 0; Index < N; ++Index)
			{
				const int32 Next = (Index + 1) % N;
				B.Quad(A + Index, A + Next, C + Next, C + Index);
			}
		}
		// Soft edges on both sides of the tube (the inner one fades inward: the chain is walked the other way round).
		{
			static TArray<int32> Rim;
			Rim.Reset();
			for (int32 Index = 0; Index < N; ++Index)
			{
				Rim.Add(First + 4 * N + Index);
			}
			B.Feather(Rim.GetData(), Rim.Num(), true);
			Rim.Reset();
			for (int32 Index = N - 1; Index >= 0; --Index)
			{
				Rim.Add(First + Index);
			}
			// (Reversed: its "outside" is the hole in the middle.)
			B.FeatherInside(Rim.GetData(), Rim.Num());
		}
	}

	/** A polygon moved inward by D (mitred; the outline runs clockwise seen from the camera), and its edges' outward normals
	 *  (make_ui_meshes.py _offset_polyline). */
	static void OffsetPoly(const TArray<FVector2D>& Pts, float D, bool bClosed, TArray<FVector2D>& Out, TArray<FVector2D>& EdgeNormals)
	{
		const int32 N = Pts.Num();
		const int32 Segs = bClosed ? N : N - 1;
		EdgeNormals.Reset();
		for (int32 Index = 0; Index < Segs; ++Index)
		{
			const FVector2D A = Pts[Index];
			const FVector2D Bp = Pts[(Index + 1) % N];
			const FVector2D Dir = (Bp - A).GetSafeNormal();
			EdgeNormals.Add(FVector2D(-Dir.Y, Dir.X));
		}
		Out.Reset();
		for (int32 Index = 0; Index < N; ++Index)
		{
			FVector2D Na;
			FVector2D Nb;
			if (!bClosed && Index == 0)
			{
				Na = Nb = EdgeNormals[0];
			}
			else if (!bClosed && Index == N - 1)
			{
				Na = Nb = EdgeNormals.Last();
			}
			else
			{
				Na = EdgeNormals[(Index - 1 + Segs) % Segs];
				Nb = EdgeNormals[Index % Segs];
			}
			const FVector2D Mid = (Na + Nb).GetSafeNormal();
			const double CosHalf = FMath::Max(FVector2D::DotProduct(Mid, Nb), 0.2);
			Out.Add(Pts[Index] - Mid * (D / CosHalf));
		}
	}

	/** A flat bevelled polygon (make_ui_meshes.py make_bevel_polygon: the arrow point, the focus arrow), in the piece's units of
	 *  100 (depth 100). Open: the edge from the last point back to the first has no bevel and no wall. */
	static void DrawBevelPolygon(FBuild& B, const FProj& P, const TArray<FVector2D>& Pts, bool bClosed, float BevelWidth, const FLook& Look)
	{
		static TArray<FVector2D> Edge;
		static TArray<FVector2D> Inner;
		static TArray<FVector2D> Offset;
		const float Base = Look.bWood ? WoodBase(Look.Grain) : 1.f;
		const int32 N = Pts.Num();
		const int32 Segs = bClosed ? N : N - 1;
		static const float Angles[4] = { 90.f, 60.f, 30.f, 0.f };
		const int32 First = B.Verts.Num();
		const int32 IndexFirst = B.Indices.Num();
		static TArray<int32> Rim;
		Rim.Reset();
		OffsetPoly(Pts, 0.f, bClosed, Offset, Edge);
		// Per edge: its wall and its bevel strip (crisp corners, as the mesh).
		for (int32 Seg = 0; Seg < Segs; ++Seg)
		{
			const int32 Ends[2] = { Seg, (Seg + 1) % N };
			const FVector2D& En = Edge[Seg];
			int32 Column[2][5];
			for (int32 End = 0; End < 2; ++End)
			{
				for (int32 Row = 0; Row < 4; ++Row)
				{
					const float Theta = FMath::DegreesToRadians(Angles[Row]);
					const float Inset = BevelWidth * (1.f - FMath::Sin(Theta));
					OffsetPoly(Pts, Inset, bClosed, Inner, Offset);
					const FVector2D Point = Inner[Ends[End]];
					const float X = 100.f * Bevel * (1.f - FMath::Cos(Theta));
					const FLinearColor Albedo = FMath::Lerp(Look.Face, Look.Lip, Smooth(0.45f, 1.f, Angles[Row] / 90.f));
					const FVector Normal = P.Normal(FVector(-FMath::Cos(Theta), FMath::Sin(Theta) * En.X, FMath::Sin(Theta) * En.Y));
					FVector2D Pixel;
					if (!P.Project(FVector(X, Point.X, Point.Y), Pixel))
					{
						B.Rollback(First, IndexFirst);
						return;
					}
					Column[End][Row + 1] = B.AddSolid(Pixel, Display(Lit(Albedo, Normal, Look) * Base, Look.Opacity));
					if (Row == 0 && (End == 0 || (!bClosed && Seg == Segs - 1)))
					{
						Rim.Add(Column[End][1]);
					}
				}
				// The wall's back.
				FVector2D Back;
				if (!P.Project(FVector(100.0, Pts[Ends[End]].X, Pts[Ends[End]].Y), Back))
				{
					B.Rollback(First, IndexFirst);
					return;
				}
				const FVector WallNormal = P.Normal(FVector(0.0, En.X, En.Y));
				Column[End][0] = B.AddSolid(Back, Display(Lit(Look.Lip, WallNormal, Look) * Base, Look.Opacity));
			}
			// (The wall first: a quad from the back to the bevel's outer row, in the wall's own colour.)
			{
				const FVector WallNormal = P.Normal(FVector(0.0, En.X, En.Y));
				const FColor WallColor = Display(Lit(Look.Lip, WallNormal, Look) * Base, Look.Opacity);
				FVector2D F0;
				FVector2D F1;
				P.Project(FVector(100.0 * Bevel, Pts[Ends[0]].X, Pts[Ends[0]].Y), F0);
				P.Project(FVector(100.0 * Bevel, Pts[Ends[1]].X, Pts[Ends[1]].Y), F1);
				const int32 W0 = B.AddSolid(F0, WallColor);
				const int32 W1 = B.AddSolid(F1, WallColor);
				B.Quad(Column[0][0], Column[1][0], W1, W0);
			}
			for (int32 Row = 1; Row < 4; ++Row)
			{
				B.Quad(Column[0][Row], Column[1][Row], Column[1][Row + 1], Column[0][Row + 1]);
			}
		}
		// The soft edge along the outer row (an open outline: not across its base, inside the plank).
		B.Feather(Rim.GetData(), Rim.Num(), bClosed);
		// The face: the inner outline, a fan.
		OffsetPoly(Pts, BevelWidth, bClosed, Inner, Offset);
		const FColor FaceColor = Display(Lit(Look.Face, P.Normal(FVector(-1.0, 0.0, 0.0)), Look) * Base, Look.Opacity);
		const int32 FaceFirst = B.Verts.Num();
		for (const FVector2D& Point : Inner)
		{
			FVector2D Pixel;
			if (!P.Project(FVector(0.0, Point.X, Point.Y), Pixel))
			{
				B.Rollback(First, IndexFirst);
				return;
			}
			B.AddSolid(Pixel, FaceColor);
		}
		for (int32 Index = 1; Index + 1 < Inner.Num(); ++Index)
		{
			B.Tri(FaceFirst, FaceFirst + Index, FaceFirst + Index + 1);
		}
		if (Look.bWood)
		{
			static TArray<FVector> Outline;
			Outline.Reset();
			for (const FVector2D& Point : Pts)
			{
				Outline.Add(FVector(0.0, Point.X, Point.Y));
			}
			DrawGrain(B, P, Outline, Look);
		}
	}

	/** A dome or a ball seen from the front: rings of shading round its middle (Radius in the part's units). */
	static void DrawDome(FBuild& B, const FProj& P, const FVector& Center, float Radius, float Height, const FLinearColor& Face, const FLinearColor& Lip, float LipAtEdge, const FLook& Look)
	{
		static const float Rings[4] = { 0.f, 0.5f, 0.82f, 1.f };
		constexpr int32 Around = 14;
		const int32 First = B.Verts.Num();
		for (int32 Ring = 0; Ring < 4; ++Ring)
		{
			const float Rho = Rings[Ring];
			const int32 Count = Ring == 0 ? 1 : Around;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const float Angle = 2.f * PI * Index / Around;
				const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
				const float Up = FMath::Sqrt(FMath::Max(1.f - Rho * Rho, 0.f));
				const FVector Point = Center + FVector(-Height * Up, Radius * Rho * Dir.X, Radius * Rho * Dir.Y);
				const FVector Normal = P.Normal(FVector(-Up / FMath::Max(Height, 0.01f), Rho * Dir.X / Radius, Rho * Dir.Y / Radius));
				const FLinearColor Albedo = FMath::Lerp(Face, Lip, LipAtEdge * Rho);
				FVector2D Pixel;
				if (!P.Project(Point, Pixel))
				{
					B.Verts.SetNum(First);
					return;
				}
				B.AddSolid(Pixel, Display(Lit(Albedo, Normal, Look), Look.Opacity));
			}
		}
		for (int32 Index = 0; Index < Around; ++Index)
		{
			const int32 Next = (Index + 1) % Around;
			B.Tri(First, First + 1 + Index, First + 1 + Next);
		}
		for (int32 Ring = 1; Ring < 3; ++Ring)
		{
			const int32 A = First + 1 + (Ring - 1) * Around;
			const int32 C = First + 1 + Ring * Around;
			for (int32 Index = 0; Index < Around; ++Index)
			{
				const int32 Next = (Index + 1) % Around;
				B.Quad(A + Index, A + Next, C + Next, C + Index);
			}
		}
		int32 Rim[Around];
		for (int32 Index = 0; Index < Around; ++Index)
		{
			Rim[Index] = First + 1 + 2 * Around + Index;
		}
		B.Feather(Rim, Around, true);
	}

	/** The snow slab on a sign (make_ui_meshes.py make_snow_cap, in its units of 100): one long soft drift, drawn from its back
	 *  row to its front so the front covers the back. */
	static void DrawSnowCap(FBuild& B, const FProj& P, const FLook& Look)
	{
		constexpr int32 NU = 24;
		static const int32 Rows[] = { 0, 1, 2, 4, 6, 8, 10, 13, 16, 20 };
		constexpr int32 NV = UE_ARRAY_COUNT(Rows);
		FVector Grid[NU + 1][NV];
		float Lips[NU + 1][NV];
		for (int32 I = 0; I <= NU; ++I)
		{
			const float U = static_cast<float>(I) / NU;
			const float Y = U - 0.5f;
			const float End = FMath::Min(U, 1.f - U);
			const float Taper = FMath::Sqrt(Smooth(0.f, 0.07f, End));
			const float Swell = 1.f + 0.10f * FMath::Sin(2.f * PI * (1.3f * U + 0.15f)) + 0.05f * FMath::Sin(2.f * PI * (3.1f * U + 0.6f));
			for (int32 J = 0; J < NV; ++J)
			{
				const int32 Row = Rows[J];
				const float A = PI * Row / 20.f;
				float X = 0.44f - 0.56f * FMath::Cos(A);
				float Z = FMath::Pow(FMath::Sin(A), 0.6f) * Swell;
				if (Row == 0)
				{
					Z = -0.28f - 0.08f * FMath::Sin(2.f * PI * 2.3f * U);
				}
				else if (Row == 1)
				{
					Z = FMath::Max(Z * 0.6f, 0.05f);
				}
				Z *= Taper;
				X = 0.44f + (X - 0.44f) * (0.6f + 0.4f * Taper);
				Grid[I][J] = FVector(X, Y, Z) * 100.0;
				Lips[I][J] = 1.f - Smooth(0.f, 0.5f, FMath::Max(Z, 0.f));
			}
		}
		const int32 First = B.Verts.Num();
		for (int32 I = 0; I <= NU; ++I)
		{
			for (int32 J = 0; J < NV; ++J)
			{
				// The normal from the neighbours, in the scaled piece's space (it is stretched along its length and height).
				const FVector Du = (Grid[FMath::Min(I + 1, NU)][J] - Grid[FMath::Max(I - 1, 0)][J]) * P.Scale;
				const FVector Dv = (Grid[I][FMath::Min(J + 1, NV - 1)] - Grid[I][FMath::Max(J - 1, 0)]) * P.Scale;
				FVector Local = FVector::CrossProduct(Dv, Du).GetSafeNormal();
				if (Local.X > 0.0)
				{
					Local = -Local;
				}
				// (P.Normal undoes a scale: the normal is in the scaled space already.)
				const FVector World = P.Rot.RotateVector(Local);
				const FVector Normal(FVector::DotProduct(P.Cam->F, World), FVector::DotProduct(P.Cam->R, World), FVector::DotProduct(P.Cam->U, World));
				FVector2D Pixel;
				if (!P.Project(Grid[I][J], Pixel))
				{
					B.Verts.SetNum(First);
					return;
				}
				B.AddSolid(Pixel, Display(Lit(FMath::Lerp(Look.Face, Look.Lip, Lips[I][J]), Normal, Look), Look.Opacity));
			}
		}
		// From the back row (the highest index) to the front: the front is drawn last.
		for (int32 J = NV - 2; J >= 0; --J)
		{
			for (int32 I = 0; I < NU; ++I)
			{
				const int32 A = First + I * NV + J;
				const int32 Bi = First + (I + 1) * NV + J;
				B.Quad(A, Bi, Bi + 1, A + 1);
			}
		}
	}

	/** An icicle (make_ui_meshes.py make_icicle, units of 100: it hangs from Z = 0 to -104): its front half. */
	static void DrawIcicle(FBuild& B, const FProj& P, const FLook& Look)
	{
		static const float Levels[] = { 0.f, 0.2f, 0.4f, 0.6f, 0.8f, 0.92f };
		constexpr int32 NL = UE_ARRAY_COUNT(Levels);
		static const float Around[5] = { -90.f, -45.f, 0.f, 45.f, 90.f };
		const int32 First = B.Verts.Num();
		for (int32 L = 0; L < NL; ++L)
		{
			const float T = Levels[L];
			const float R = 17.f * FMath::Pow(1.f - T, 1.15f);
			const float Wob = 2.f * FMath::Sin(7.f * T);
			for (int32 K = 0; K < 5; ++K)
			{
				const float Phi = FMath::DegreesToRadians(Around[K]);
				const FVector Point(8.5f + Wob - R * FMath::Cos(Phi), Wob + R * FMath::Sin(Phi), -100.f * T);
				const FVector Normal = P.Normal(FVector(-FMath::Cos(Phi), FMath::Sin(Phi), 0.2f));
				FVector2D Pixel;
				if (!P.Project(Point, Pixel))
				{
					B.Verts.SetNum(First);
					return;
				}
				B.AddSolid(Pixel, Display(Lit(FMath::Lerp(Look.Face, Look.Lip, T), Normal, Look), Look.Opacity));
			}
		}
		FVector2D TipPixel;
		if (!P.Project(FVector(8.5f, 0.f, -104.f), TipPixel))
		{
			B.Verts.SetNum(First);
			return;
		}
		const int32 Tip = B.AddSolid(TipPixel, Display(Lit(Look.Lip, P.Normal(FVector(-1.0, 0.0, -0.3)), Look), Look.Opacity));
		for (int32 L = 0; L + 1 < NL; ++L)
		{
			for (int32 K = 0; K < 4; ++K)
			{
				const int32 A = First + L * 5 + K;
				B.Quad(A, A + 1, A + 6, A + 5);
			}
		}
		for (int32 K = 0; K < 4; ++K)
		{
			const int32 A = First + (NL - 1) * 5 + K;
			B.Tri(A, A + 1, Tip);
		}
	}

	// ---- the rendered wood: style 5 "Snowy cabin" (2026-11-07, card 2026-11-07_wood-style5-apply; PinguWoodStyle.h, docs/UI_STYLE.md):
	// every wood slab, snow slab, nail and icicle is laid out by PinguWood's strip rule (one uniform scale from the height, caps never
	// stretched, whole tiles A / B in between) and drawn as pictures of T_UI2D_CabinAtlas through the part's projection.

	/** A quad of the picture: local corners (Y right, Z up, on the face X = 0) and their atlas pixels. */
	static void ImageQuad(FBuild& B, const FProj& P, const FVector2D& L0, const FVector2D& L1, const FVector2D& L2, const FVector2D& L3,
		const FVector2f& T0, const FVector2f& T1, const FVector2f& T2, const FVector2f& T3, const FColor& Color)
	{
		const FVector2D Ls[4] = { L0, L1, L2, L3 };
		const FVector2f Ts[4] = { T0, T1, T2, T3 };
		int32 Index[4];
		const int32 First = B.Verts.Num();
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			FVector2D Pixel;
			if (!P.Project(FVector(0.0, Ls[Corner].X, Ls[Corner].Y), Pixel))
			{
				B.Verts.SetNum(First);
				return;
			}
			Index[Corner] = B.Add(Pixel, Color, B.AtlasUV(Ts[Corner].X, Ts[Corner].Y));
		}
		B.Quad(Index[0], Index[1], Index[2], Index[3]);
	}

	/** PinguWood's quads (plank space: X right, Y down from Origin) on the part's face. Vertical: the plank's length runs up (the
	 *  title's post: the one standing piece, a post is not a plank of words). */
	static void DrawQuads(FBuild& B, const FProj& P, const TArray<PinguWood::FQuad>& Quads, const FVector2D& Origin, bool bVertical, const FColor& Color)
	{
		for (const PinguWood::FQuad& Q : Quads)
		{
			auto Local = [&Origin, bVertical](float X, float Y)
			{
				// (Plank space -> the face: Y right = X, Z up = -Y; standing: the length along Z.)
				return bVertical ? FVector2D(Origin.Y + Y, Origin.X + X) : FVector2D(Origin.X + X, Origin.Y - Y);
			};
			ImageQuad(B, P, Local(Q.X0, Q.Y0), Local(Q.X1, Q.Y0), Local(Q.X1, Q.Y1), Local(Q.X0, Q.Y1),
				FVector2f(Q.U0, Q.V0), FVector2f(Q.U1, Q.V0), FVector2f(Q.U1, Q.V1), FVector2f(Q.U0, Q.V1), Color);
		}
	}

	static FColor WoodColor(const FLook& Look, uint8 State)
	{
		const float Shade = PinguWood::ShadeOf(static_cast<PinguWood::EState>(State));
		return FLinearColor(Shade, Shade, Shade, Look.Opacity).ToFColor(false);
	}

	/** A wood slab W x H (its centre at the origin) as a cabin plank of its cut's class: page units are design px. */
	static void DrawWoodImage(FBuild& B, const FProj& P, float W, float H, const FLook& Look, uint8 CutIn, uint8 StateIn, uint32 Seed, int32 StartState)
	{
		using namespace PinguWood;
		const ECut Cut = static_cast<ECut>(CutIn);
		const EState State = static_cast<EState>(StateIn);
		const bool bVert = Look.bVertical;
		const float Along = bVert ? H : W;
		const float Across = bVert ? W : H;
		const EClass Class = ClassOf(Cut, State, Across);
		static TArray<FQuad> Quads;
		Quads.Reset();
		LayoutPlank(Class, State, Along, Across, Seed, 1.f, Quads, false, StartState);
		DrawQuads(B, P, Quads, bVert ? FVector2D(-0.5f * Along, -0.5f * Across) : FVector2D(-0.5f * Along, 0.5f * Across), bVert, WoodColor(Look, StateIn));
	}

	/** The part's transform without its own scale (its parent's): its local units are page units again (snow, nails, icicles are
	 *  pieces scaled to their size in 3D; the pictures have their own size). */
	static FProj UnscaledProj(const UPinguMenu2DPart& Part, const FCam& Cam)
	{
		FTransform T = Part.GetComponentTransform();
		const USceneComponent* Parent = Part.GetAttachParent();
		T.SetScale3D(Parent ? Parent->GetComponentScale() : FVector::OneVector);
		return MakeProj(T, Cam);
	}

	/** The snow slab on a plank (the part sits on the plank's top edge, centred; Width / Height: the plank's body). */
	static void DrawSnowImage(FBuild& B, const UPinguMenu2DPart& Part, const FCam& Cam, const FLook& Look)
	{
		const FProj P = UnscaledProj(Part, Cam);
		const FVector Rel = Part.GetRelativeScale3D();
		const float W = Part.Width > 0.f ? Part.Width : static_cast<float>(Rel.Y) * 100.f + 8.f;
		const float H = Part.Height > 0.f ? Part.Height : 94.f;
		static TArray<PinguWood::FQuad> Quads;
		Quads.Reset();
		PinguWood::LayoutSnow(W, H, static_cast<uint32>(Part.Order * 2654435761u), 1.f, Quads);
		// (The part sits 1.5 under the top edge: the strip's top line goes on the edge itself.)
		const uint8 State = Part.ColorFrom.IsValid() ? Part.ColorFrom->WoodState : 0;
		DrawQuads(B, P, Quads, FVector2D(-0.5f * W, 1.5f), false, WoodColor(Look, State));
	}

	/** A nail (9 design px across, its contact shadow included). */
	static void DrawNailImage(FBuild& B, const UPinguMenu2DPart& Part, const FCam& Cam, const FLook& Look)
	{
		const FProj P = UnscaledProj(Part, Cam);
		static TArray<PinguWood::FQuad> Quads;
		Quads.Reset();
		PinguWood::LayoutSprite((Part.Order & 1u) ? PinguWood::ESprite::Nail1 : PinguWood::ESprite::Nail0, 0.f, 0.f, 0.5f, false, Quads);
		DrawQuads(B, P, Quads, FVector2D::ZeroVector, false, WoodColor(Look, 0));
	}

	/** An icicle hung from its top centre (its 3D length picks the nearest of the five pictures). */
	static void DrawIcicleImage(FBuild& B, const UPinguMenu2DPart& Part, const FCam& Cam, const FLook& Look)
	{
		const FProj P = UnscaledProj(Part, Cam);
		using PinguWood::ESprite;
		// (Lengths in design px: 14, 20, 26, 32, 18.)
		static const float Lengths[5] = { 14.f, 20.f, 26.f, 32.f, 18.f };
		const float Want = 0.7f * static_cast<float>(Part.GetRelativeScale3D().Z) * 100.f;
		int32 Best = 0;
		for (int32 Index = 1; Index < 5; ++Index)
		{
			if (FMath::Abs(Lengths[Index] - Want) < FMath::Abs(Lengths[Best] - Want))
			{
				Best = Index;
			}
		}
		static TArray<PinguWood::FQuad> Quads;
		Quads.Reset();
		PinguWood::LayoutSprite(static_cast<ESprite>(static_cast<int32>(ESprite::Icicle0) + Best), 0.f, 0.f, 0.5f, true, Quads);
		DrawQuads(B, P, Quads, FVector2D::ZeroVector, false, WoodColor(Look, 0));
	}
	/** The grain texture's Slate brush and handle (kept for the run). */
	struct FTextures
	{
		TStrongObjectPtr<UTexture> Grain;
		TSharedPtr<FSlateBrush> GrainBrush;
		bool bTried = false;
		TMap<TWeakObjectPtr<UTexture>, TSharedPtr<FSlateBrush>> Icons;
		TSharedPtr<FSlateBrush> AtlasBrush;
	};
	static FTextures& Textures()
	{
		static FTextures Value;
		return Value;
	}

	static TSharedPtr<FSlateBrush> MakeBrush(UTexture* Texture)
	{
		TSharedPtr<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Texture);
		Brush->ImageSize = FVector2D(256.f, 256.f);
		Brush->DrawAs = ESlateBrushDrawType::Image;
		return Brush;
	}

	struct FItem
	{
		const UPinguMenu2DPart* Part = nullptr;
		int32 Group = 0;
		double Depth = 0.0;
		uint32 Order = 0;
	};

	/** One part into the build (icons are drawn by the caller: another texture). */
	static void DrawPart(FBuild& B, const UPinguMenu2DPart& Part, const FCam& Cam, float Exposure)
	{
		const FTransform& Own = Part.GetComponentTransform();
		const FProj P = MakeProj(Own, Cam);
		FLook Look;
		Look.Face = Part.Face;
		Look.Lip = Part.Lip;
		Look.Glow = Part.Glow;
		Look.Opacity = Part.Opacity;
		Look.Gain = Part.bWorldLit ? WorldGain : Exposure;
		Look.Own = &Own;
		const UPinguMenu2DPart* Wood = &Part;
		if (const UPinguMenu2DPart* From = Part.ColorFrom.Get())
		{
			Wood = From;
			Look.Face = From->Face;
			Look.Lip = From->Lip;
			Look.Glow = From->Glow;
			Look.GrainFrom = &From->GetComponentTransform();
		}
		Look.bWood = Wood->bWood;
		Look.Grain = Wood->Grain;
		Look.GrainOffset = Wood->GrainOffset;
		Look.GrainScale = Wood->GrainScale;
		Look.bVertical = Wood->bVertical;
		switch (Part.Kind)
		{
		case EPinguMenu2DKind::Slab:
			Look.bShadow = Part.bShadow;
			if (!Part.bWood && !Part.bShadow)
			{
				Look.Spec = 0.25f;
				Look.Shine = 24.f;
			}
			if (B.bAtlas && Part.bWood && !Part.bShadow)
			{
				// (The plank's id: its grain offset, set per plank by the menu code, and its board variant.)
				const uint32 Seed = static_cast<uint32>(FMath::Abs(Part.GrainOffset.X * 7919.0 + Part.GrainOffset.Y * 104729.0)) + 977u * Part.WoodVariant;
				const int8* Start = GWoodStarts.Find(&Part);
				DrawWoodImage(B, P, Part.Width, Part.Height, Look, Part.WoodCut, Part.WoodState, Seed, Start ? *Start : -1);
				break;
			}
			if (B.bAtlas && Part.bShadow)
			{
				// (The cabin planks carry their drop shadow baked in: no second one, UI_STYLE section 6.)
				break;
			}
			DrawSlab(B, P, Part.Width, Part.Height, Part.Radius, FMath::Max(Part.Depth, 0.5f), Look);
			break;
		case EPinguMenu2DKind::Ring:
			Look.bWood = false;
			DrawRing(B, P, Part.Width, Part.Height, Part.Radius, Part.Tube, Part.bRope, Look);
			break;
		case EPinguMenu2DKind::Piece:
			switch (Part.Piece)
			{
			case EPinguMenu2DPiece::ArrowTip:
			{
				if (B.bAtlas)
				{
					// (No arrow points on the cabin planks: the signs are straight planks, UI_STYLE.)
					break;
				}
				static TArray<FVector2D> Pts;
				Pts.Reset();
				Pts.Append({ FVector2D(-55.0, 100.0), FVector2D(0.0, 100.0), FVector2D(80.0, 0.0), FVector2D(0.0, -100.0), FVector2D(-55.0, -100.0) });
				// (A left-pointing tip is the same piece mirrored by its scale: its outline stays as it is in its own space.)
				DrawBevelPolygon(B, P, Pts, false, TipBevel, Look);
				break;
			}
			case EPinguMenu2DPiece::Pointer:
			{
				static TArray<FVector2D> Pts;
				Pts.Reset();
				Pts.Append({ FVector2D(0.0, 100.0), FVector2D(150.0, 0.0), FVector2D(0.0, -100.0) });
				Look.bWood = false;
				Look.Spec = 0.3f;
				DrawBevelPolygon(B, P, Pts, true, PointerBevel, Look);
				break;
			}
			case EPinguMenu2DPiece::SnowCap:
				if (B.bAtlas)
				{
					DrawSnowImage(B, Part, Cam, Look);
					break;
				}
				Look.bWood = false;
				Look.Face = SnowWhite;
				Look.Lip = SnowShade;
				Look.Glow = 0.04f;
				DrawSnowCap(B, P, Look);
				break;
			case EPinguMenu2DPiece::Nail:
				if (B.bAtlas)
				{
					DrawNailImage(B, Part, Cam, Look);
					break;
				}
				Look.bWood = false;
				Look.Spec = 0.55f;
				Look.Shine = 30.f;
				DrawDome(B, P, FVector::ZeroVector, 100.f, 45.f, NailColor, NailLip, 0.5f, Look);
				break;
			case EPinguMenu2DPiece::Ball:
				Look.bWood = false;
				Look.Glow = 0.04f;
				DrawDome(B, P, FVector::ZeroVector, 50.f, 50.f, SnowWhite, SnowShade, 0.6f, Look);
				break;
			case EPinguMenu2DPiece::Icicle:
				if (B.bAtlas)
				{
					DrawIcicleImage(B, Part, Cam, Look);
					break;
				}
				Look.bWood = false;
				Look.Glow = 0.12f;
				Look.Spec = 0.4f;
				Look.Shine = 40.f;
				Look.Face = IceColor;
				Look.Lip = IceLip;
				DrawIcicle(B, P, Look);
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
	}

	/** A raised glyph of the lobby (the "+" on its ball: Linus 2026-10-27 "leave the + ball like it was before"): two bevelled
	 *  bars in the text's colours, in the text's own space. */
	static void DrawRaised(FBuild& B, const PinguMenu3D::FRaisedText& Text, const FCam& Cam)
	{
		const FString Glyph = Text.String.TrimStartAndEnd();
		const bool bPlus = Glyph == TEXT("+");
		const bool bCross = Glyph == TEXT("X") || Glyph == TEXT("x");
		if ((!bPlus && !bCross) || !Text.Component)
		{
			return;
		}
		// (The lobby's x: "X" in Titan One, two thick strokes; the "+": two bars, a little under the capitals' middle.)
		const float Width = FMath::Max(Text.MaxY - Text.MinY, 1.f);
		const float Mid = 0.5f * (Text.MinY + Text.MaxY);
		const float Center = Text.Baseline + (bPlus ? 0.42f : 0.5f) * Text.Cap;
		FLook Look;
		Look.Face = Text.Front;
		Look.Lip = Text.Side;
		Look.Gain = WorldGain;
		Look.Spec = 0.2f;
		const float Depth = 0.18f * Text.Cap;
		for (int32 Stroke = 0; Stroke < 2; ++Stroke)
		{
			const float Diagonal = FMath::RadiansToDegrees(FMath::Atan2(Text.Cap, Width));
			const float Roll = bPlus ? (Stroke == 0 ? 0.f : 90.f) : (Stroke == 0 ? Diagonal : -Diagonal);
			const float Length = bPlus ? Width : 0.92f * FMath::Sqrt(Width * Width + Text.Cap * Text.Cap);
			const float Thick = bPlus ? 0.36f * Width : 0.34f * Width;
			const FTransform Place(FRotator(0.f, 0.f, Roll).Quaternion(), FVector(-0.5f, Mid, Center));
			const FProj P = MakeProj(Place * Text.Component->GetComponentTransform(), Cam);
			DrawSlab(B, P, Length, Thick, 0.5f * Thick, Depth, Look);
		}
	}

	static bool ChainVisible(const USceneComponent* Part)
	{
		for (const USceneComponent* Item = Part; Item; Item = Item->GetAttachParent())
		{
			if (!Item->IsVisible())
			{
				return false;
			}
		}
		return true;
	}
}

FVector2D PinguMenu2D::GetRestPlace(const UPinguMenu2DPart& Part)
{
	// The part's place under its page's root, every link of the chain taken at its relative transform except a button's Body
	// (RestBodyTag: its drop-in, tilt, grow and press), so it is the place the page laid it out at.
	// (Up to the page's root, left out: its place - and its own open animation - is the same for all its planks.)
	const APinguMenu3D* Menu = Cast<APinguMenu3D>(Part.GetOwner());
	const USceneComponent* MenuRoot = Menu ? Menu->GetMenuRootComponent() : nullptr;
	FTransform Local = FTransform::Identity;
	const USceneComponent* Item = &Part;
	while (Item && Item->GetAttachParent() && Item->GetAttachParent() != MenuRoot)
	{
		if (!Item->ComponentHasTag(RestBodyTag))
		{
			Local = Local * Item->GetRelativeTransform();
		}
		Item = Item->GetAttachParent();
	}
	const FVector At = Local.GetLocation();
	return FVector2D(At.Y, At.Z);
}

int32 PinguMenu2D::GetWoodStart(const UPinguMenu2DPart* Part)
{
	const int8* Start = PinguMenu2DPrivate::GWoodStarts.Find(Part);
	return Start ? *Start : -1;
}

int32 PinguMenu2D::GetPaintedParts()
{
	return PinguMenu2DPrivate::GPaintedParts;
}

int32 PinguMenu2D::GetPaintedDraws()
{
	return PinguMenu2DPrivate::GPaintedDraws;
}

int32 PinguMenu2D::GetPaintedVerts()
{
	return PinguMenu2DPrivate::GPaintedVerts;
}

bool PinguMenu2D::ProjectWorldForTest(APinguMenu3D* Menu, const FVector& World, FVector2D& OutPixel)
{
	using namespace PinguMenu2DPrivate;
	APlayerController* Controller = Menu ? Menu->GetMenuController() : nullptr;
	ULocalPlayer* Player = Controller ? Controller->GetLocalPlayer() : nullptr;
	FSceneViewProjectionData Projection;
	if (!Player || !Player->ViewportClient || !Player->ViewportClient->Viewport || !Player->GetProjectionData(Player->ViewportClient->Viewport, Projection))
	{
		return false;
	}
	FCam Cam;
	const FIntRect Rect = Projection.GetConstrainedViewRect();
	Cam.bMatrix = true;
	Cam.ViewProj = Projection.ComputeViewProjectionMatrix();
	Cam.RectMin = FVector2D(Rect.Min.X, Rect.Min.Y);
	Cam.RectSize = FVector2D(Rect.Width(), Rect.Height());
	const FProj P = MakeProj(FTransform(World), Cam);
	return P.Project(FVector::ZeroVector, OutPixel);
}

int32 PinguMenu2D::Paint(const UWorld* World, APinguMenu3D* Menu, const FGeometry& Geometry, FSlateWindowElementList& OutDrawElements, int32 LayerId)
{
	using namespace PinguMenu2DPrivate;
	GPaintedParts = 0;
	GWoodStarts.Reset();
	GPaintedDraws = 0;
	GPaintedVerts = 0;
	if (!World || !Menu || !PinguMenu3D::IsFlat() || Menu->IsHiddenForTest() || !FSlateApplication::IsInitialized())
	{
		return LayerId;
	}
	FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer();
	if (!Renderer)
	{
		return LayerId;
	}
	// The cameras: the menu's own (it stands still in 2D) and the player's (the lobby's tags are in the world).
	FCam MenuCam;
	FCam WorldCam;
	{
		FVector Origin;
		FRotator Rotation;
		float TanX = 0.f;
		float TanY = 0.f;
		FVector2D Size;
		if (!Menu->GetFlatView(Origin, Rotation, TanX, TanY, Size))
		{
			return LayerId;
		}
		const FRotationMatrix Axes(Rotation);
		MenuCam.O = Origin;
		MenuCam.F = Axes.GetScaledAxis(EAxis::X);
		MenuCam.R = Axes.GetScaledAxis(EAxis::Y);
		MenuCam.U = Axes.GetScaledAxis(EAxis::Z);
		MenuCam.TanX = TanX;
		MenuCam.TanY = TanY;
		MenuCam.W = Size.X;
		MenuCam.H = Size.Y;
		WorldCam = MenuCam;
		// The world's parts: the engine's own projection of this player's view (its constrained view rectangle, its aspect rule).
		if (APlayerController* Controller = Menu->GetMenuController())
		{
			ULocalPlayer* Player = Controller->GetLocalPlayer();
			FSceneViewProjectionData Projection;
			if (Player && Player->ViewportClient && Player->ViewportClient->Viewport
				&& Player->GetProjectionData(Player->ViewportClient->Viewport, Projection))
			{
				const FIntRect Rect = Projection.GetConstrainedViewRect();
				WorldCam.bMatrix = Rect.Width() > 0 && Rect.Height() > 0;
				WorldCam.ViewProj = Projection.ComputeViewProjectionMatrix();
				WorldCam.RectMin = FVector2D(Rect.Min.X, Rect.Min.Y);
				WorldCam.RectSize = FVector2D(Rect.Width(), Rect.Height());
			}
		}
		if (Menu->GetCameraView(Origin, Rotation, TanX, TanY))
		{
			const FRotationMatrix WorldAxes(Rotation);
			WorldCam.O = Origin;
			WorldCam.F = WorldAxes.GetScaledAxis(EAxis::X);
			WorldCam.R = WorldAxes.GetScaledAxis(EAxis::Y);
			WorldCam.U = WorldAxes.GetScaledAxis(EAxis::Z);
			WorldCam.TanX = TanX;
			WorldCam.TanY = TanY;
		}
	}
	static FBuild B;
	B.Verts.Reset();
	B.Indices.Reset();
	// Viewport pixels -> this widget's render space (through the viewport widget's geometry, as the words and the clicks).
	{
		FVector2D S0;
		FVector2D S1;
		FVector2D S2;
		if (!PinguMenu3D::ViewportPixelToScreen(World, FVector2D(0.0, 0.0), S0) || !PinguMenu3D::ViewportPixelToScreen(World, FVector2D(1000.0, 0.0), S1)
			|| !PinguMenu3D::ViewportPixelToScreen(World, FVector2D(0.0, 1000.0), S2))
		{
			return LayerId;
		}
		const FSlateRenderTransform& ToRender = Geometry.GetAccumulatedRenderTransform();
		auto Map = [&Geometry, &ToRender](const FVector2D& Screen)
		{
			const FVector2f Local = FVector2f(Geometry.AbsoluteToLocal(Screen));
			return FVector2D(ToRender.TransformPoint(Local));
		};
		const FVector2D R0 = Map(S0);
		B.PxOrigin = R0;
		B.PxX = (Map(S1) - R0) / 1000.0;
		B.PxY = (Map(S2) - R0) / 1000.0;
	}
	// The texture: the grain (its solid block for the plain shapes), or the white box without it.
	FTextures& Tex = Textures();
	if (!Tex.bTried)
	{
		Tex.bTried = true;
		if (UTexture* Grain = LoadObject<UTexture>(nullptr, GrainPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Tex.Grain.Reset(Grain);
			Tex.GrainBrush = MakeBrush(Grain);
		}
		UE_LOG(LogPingu, Log, TEXT("[MENU2D] painter: grain texture %s"), Tex.Grain.IsValid() ? TEXT("ok") : TEXT("missing (plain wood)"));
	}
	FSlateResourceHandle MainHandle;
	B.bGrain = false;
	B.bAtlas = false;
	// The rendered wood atlas (2026-11-07): every wood surface and the plain shapes (its solid block) in one draw.
	if (!Tex.AtlasBrush.IsValid())
	{
		if (UTexture* Atlas = PinguWood::GetAtlas())
		{
			Tex.AtlasBrush = MakeBrush(Atlas);
		}
	}
	if (Tex.AtlasBrush.IsValid())
	{
		MainHandle = Renderer->GetResourceHandle(*Tex.AtlasBrush, FVector2f(0.f, 0.f), 1.f);
		if (const FSlateShaderResourceProxy* Proxy = MainHandle.GetResourceProxy())
		{
			B.AtlasStart = FVector2f(Proxy->StartUV);
			B.AtlasSizeUV = FVector2f(Proxy->SizeUV);
			const PinguWood::FRect Solid = PinguWood::Solid();
			B.SolidUV = B.AtlasUV(Solid.X + 0.5f * Solid.W, Solid.Y + 0.5f * Solid.H);
			B.bAtlas = true;
		}
	}
	if (!B.bAtlas && Tex.GrainBrush.IsValid())
	{
		MainHandle = Renderer->GetResourceHandle(*Tex.GrainBrush, FVector2f(0.f, 0.f), 1.f);
		if (const FSlateShaderResourceProxy* Proxy = MainHandle.GetResourceProxy())
		{
			B.SolidUV = Proxy->StartUV + Proxy->SizeUV * SolidTexel;
			B.bGrain = true;
		}
	}
	if (!B.bGrain && !B.bAtlas)
	{
		MainHandle = FPinguStyle::Get().GetWhiteHandle();
		if (const FSlateShaderResourceProxy* Proxy = MainHandle.GetResourceProxy())
		{
			B.SolidUV = Proxy->StartUV + Proxy->SizeUV * 0.5f;
		}
	}
	int32 Layer = LayerId;
	auto Flush = [&]()
	{
		if (B.Indices.Num() > 0)
		{
			FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, MainHandle, B.Verts, B.Indices, nullptr, 0, 0, ESlateDrawEffect::None,
				B.bGrain ? (ESlateBatchDrawFlag::TileU | ESlateBatchDrawFlag::TileV) : ESlateBatchDrawFlag::None);
			++GPaintedDraws;
			GPaintedVerts += B.Verts.Num();
			++Layer;
		}
		B.Verts.Reset();
		B.Indices.Reset();
	};
	auto DrawIcon = [&](const UPinguMenu2DPart& Part, const FCam& Cam)
	{
		UTexture* Texture = Part.Texture.Get();
		if (!Texture)
		{
			return;
		}
		TSharedPtr<FSlateBrush>& Brush = Tex.Icons.FindOrAdd(Texture);
		if (!Brush.IsValid())
		{
			Brush = MakeBrush(Texture);
		}
		const FSlateResourceHandle Handle = Renderer->GetResourceHandle(*Brush, FVector2f(0.f, 0.f), 1.f);
		const FSlateShaderResourceProxy* Proxy = Handle.GetResourceProxy();
		if (!Proxy)
		{
			return;
		}
		const FProj P = MakeProj(Part.GetComponentTransform(), Cam);
		static const FVector2D Corners[4] = { FVector2D(-50.0, 50.0), FVector2D(50.0, 50.0), FVector2D(50.0, -50.0), FVector2D(-50.0, -50.0) };
		static const FVector2f UVs[4] = { FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f) };
		Flush();
		const FColor Tint = FLinearColor(Part.Tint.R, Part.Tint.G, Part.Tint.B, Part.Tint.A * Part.Opacity).ToFColor(true);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FVector2D Pixel;
			if (!P.Project(FVector(0.0, Corners[Index].X, Corners[Index].Y), Pixel))
			{
				B.Verts.Reset();
				return;
			}
			B.Add(Pixel, Tint, Proxy->StartUV + Proxy->SizeUV * UVs[Index]);
		}
		B.Quad(0, 1, 2, 3);
		FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, Handle, B.Verts, B.Indices, nullptr, 0, 0);
		++GPaintedDraws;
		++Layer;
		B.Verts.Reset();
		B.Indices.Reset();
	};

	// 1. The lobby island's tags, x and "+" (in the world, behind every menu page).
	static TArray<FItem> Items;
	Items.Reset();
	APinguLobby* Lobby = APinguLobby::Get(World);
	if (Lobby && !Lobby->IsHidden())
	{
		TInlineComponentArray<UPinguMenu2DPart*> Parts(Lobby);
		for (const UPinguMenu2DPart* Part : Parts)
		{
			if (Part && Part->IsVisible() && ChainVisible(Part))
			{
				FItem& Item = Items.AddDefaulted_GetRef();
				Item.Part = Part;
				Item.Depth = FVector::DotProduct(Part->GetComponentLocation() - WorldCam.O, WorldCam.F);
				Item.Order = Part->Order;
			}
		}
		Items.Sort([](const FItem& A, const FItem& Bi) { return A.Depth != Bi.Depth ? A.Depth > Bi.Depth : A.Order < Bi.Order; });
		for (const FItem& Item : Items)
		{
			if (Item.Part->Kind == EPinguMenu2DKind::Icon)
			{
				DrawIcon(*Item.Part, WorldCam);
			}
			else
			{
				DrawPart(B, *Item.Part, WorldCam, WorldGain);
			}
			++GPaintedParts;
		}
		static TArray<PinguMenu3D::FRaisedText> Raised;
		PinguMenu3D::GetRaisedTexts(Lobby, Raised);
		for (const PinguMenu3D::FRaisedText& Text : Raised)
		{
			DrawRaised(B, Text, WorldCam);
		}
	}
	// 2. The darkening behind a page (pause, settings, ...: the 3D menu's post-process dim).
	const float Dim = Menu->GetFlatDim();
	// The 3D menu was seen through the camera's exposure: its boards came out about twice as light on the darkened pages, the
	// lobby's strip a little darker than the title (measured on the 3D menu's screenshots, 2026-10-27).
	const float Exposure = Menu->GetPage() == EPinguMenu3DPage::Lobby ? LobbyExposure : 1.f + (DimExposure - 1.f) * FMath::Clamp(Dim / 0.45f, 0.f, 1.f);
	if (Dim > 0.003f)
	{
		const FColor Black = FLinearColor(0.f, 0.f, 0.f, Dim).ToFColor(false);
		const int32 A = B.AddSolid(FVector2D(-10.0, -10.0), Black);
		const int32 Bi = B.AddSolid(FVector2D(MenuCam.W + 10.0, -10.0), Black);
		const int32 C = B.AddSolid(FVector2D(MenuCam.W + 10.0, MenuCam.H + 10.0), Black);
		const int32 D = B.AddSolid(FVector2D(-10.0, MenuCam.H + 10.0), Black);
		B.Quad(A, Bi, C, D);
	}
	// 3. The menu: the page, then the overlay card (PRESS A KEY, the error popup), then the focus arrow and the snow puff; each
	// back to front by its depth on its page (the 3D menu's depth order).
	Items.Reset();
	const USceneComponent* MenuRoot = Menu->GetMenuRootComponent();
	const USceneComponent* OverlayRoot = Menu->GetOverlayRootComponent();
	{
		TInlineComponentArray<UPinguMenu2DPart*> Parts(Menu);
		for (const UPinguMenu2DPart* Part : Parts)
		{
			if (!Part || !Part->IsVisible())
			{
				continue;
			}
			// Its page: the part under the menu's root it hangs on.
			const USceneComponent* PageRoot = Part;
			while (PageRoot && PageRoot->GetAttachParent() && PageRoot->GetAttachParent() != MenuRoot)
			{
				PageRoot = PageRoot->GetAttachParent();
			}
			if (!PageRoot || (!PageRoot->IsVisible() && PageRoot != Part))
			{
				continue;
			}
			FItem& Item = Items.AddDefaulted_GetRef();
			Item.Part = Part;
			Item.Order = Part->Order;
			Item.Group = Part->bOnTop ? 3 : (PageRoot == OverlayRoot && OverlayRoot ? 2 : 1);
			Item.Depth = PageRoot->GetComponentTransform().InverseTransformPosition(Part->GetComponentLocation()).X;
		}
	}
	Items.Sort([](const FItem& A, const FItem& Bi)
	{
		if (A.Group != Bi.Group)
		{
			return A.Group < Bi.Group;
		}
		if (FMath::Abs(A.Depth - Bi.Depth) > 0.01)
		{
			return A.Depth > Bi.Depth;
		}
		return A.Order < Bi.Order;
	});
	// The wood's first tiles (card 2026-11-07_trait-logo-wood-tiling; Linus: "u can see the tiling really well"): every plank
	// starts where its seed likes, unless a neighbour of its class (PinguWood::AreNeighbours, at their places at rest) made
	// earlier already starts there - then the next free start. In the parts' making order, so it is the same every frame.
	if (B.bAtlas)
	{
		struct FWoodPlank
		{
			const UPinguMenu2DPart* Part = nullptr;
			PinguWood::EClass Class = PinguWood::EClass::Board;
			FVector2D Place = FVector2D::ZeroVector;
			int32 Start = -1;
		};
		static TArray<FWoodPlank> Planks;
		Planks.Reset();
		for (const FItem& Item : Items)
		{
			const UPinguMenu2DPart* Part = Item.Part;
			if (Part->Kind == EPinguMenu2DKind::Slab && Part->bWood && !Part->bShadow && !Part->bVertical && Item.Group < 3)
			{
				FWoodPlank& Plank = Planks.AddDefaulted_GetRef();
				Plank.Part = Part;
				Plank.Class = PinguWood::ClassOf(static_cast<PinguWood::ECut>(Part->WoodCut), static_cast<PinguWood::EState>(Part->WoodState), Part->Height);
				Plank.Place = PinguMenu2D::GetRestPlace(*Part);
			}
		}
		Planks.Sort([](const FWoodPlank& A, const FWoodPlank& Bi) { return A.Part->Order < Bi.Part->Order; });
		for (int32 Index = 0; Index < Planks.Num(); ++Index)
		{
			FWoodPlank& Plank = Planks[Index];
			const UPinguMenu2DPart& Part = *Plank.Part;
			const uint32 Seed = static_cast<uint32>(FMath::Abs(Part.GrainOffset.X * 7919.0 + Part.GrainOffset.Y * 104729.0)) + 977u * Part.WoodVariant;
			uint32 Taken = 0;
			for (int32 Before = 0; Before < Index; ++Before)
			{
				const FWoodPlank& Other = Planks[Before];
				if (Other.Class == Plank.Class && PinguWood::AreNeighbours(Plank.Place, Part.Width, Part.Height, Other.Place, Other.Part->Width, Other.Part->Height))
				{
					Taken |= 1u << Other.Start;
				}
			}
			int32 Start = PinguWood::PreferredStart(Seed);
			for (int32 Try = 0; Try < PinguWood::StartStates && (Taken & (1u << Start)) != 0u; ++Try)
			{
				Start = (Start + 1) % PinguWood::StartStates;
			}
			Plank.Start = Start;
			GWoodStarts.Add(Plank.Part, static_cast<int8>(Start));
		}
	}
	for (const FItem& Item : Items)
	{
		if (Item.Part->Kind == EPinguMenu2DKind::Icon)
		{
			DrawIcon(*Item.Part, MenuCam);
		}
		else
		{
			DrawPart(B, *Item.Part, MenuCam, Exposure);
		}
		++GPaintedParts;
	}
	// 4. Raised glyphs of the menu (the card lobby's "+" on its yellow ball), over its parts.
	{
		static TArray<PinguMenu3D::FRaisedText> Raised;
		PinguMenu3D::GetRaisedTexts(Menu, Raised);
		for (const PinguMenu3D::FRaisedText& Text : Raised)
		{
			DrawRaised(B, Text, MenuCam);
		}
	}
	Flush();
	return Layer;
}

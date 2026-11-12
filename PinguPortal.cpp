// Pingu - Snow Warp's portal (see PinguPortal.h).

#include "PinguPortal.h"
#include "Penguin.h"
#include "PenguinCamera.h"
#include "PenguinCharacter.h"
#include "PenguinMovementComponent.h"
#include "PenguinPlayerController.h"
#include "PenguinProjectile.h"
#include "PinguFX.h"
#include "PinguPickup.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

namespace PinguPortalPrivate
{
	/** Parts per end (Parts): the frame ring, two swirling rings inside it, the glowing sheet. */
	enum EPart : int32 { Frame, SwirlOuter, SwirlInner, Sheet, NumParts };

	/** Something goes through only once it has been this far (cm) out of both ends since it came out of one (or since it
	 *  was first seen in one). */
	constexpr float ExitMargin = 12.f;
	/** A shot or bubble comes out where it went in, relative to the end, but at most this share of the way to the rim. */
	constexpr float MaxExitOffset = 0.75f;
	/** It pops open in this long (s). */
	constexpr float OpenTime = 0.3f;
	/** The swirls turn this fast (degrees a second; the inner one the other way). */
	constexpr float SwirlOuterSpeed = 150.f;
	constexpr float SwirlInnerSpeed = -260.f;
	/** Sizes of the swirling rings and the sheet against the frame. */
	constexpr float SwirlOuterShare = 0.64f;
	constexpr float SwirlInnerShare = 0.34f;
	constexpr float SheetShare = 0.9f;
	/** The sheet's depth (cm): a flat lens. */
	constexpr float SheetDepth = 7.f;
	/** A sparkle on each end this often (s). */
	constexpr float SparkleInterval = 0.2f;
	/** SM_Ring: 100 cm across at scale 1 (a torus facing the camera); SM_Bubble: a 100 cm ball. */
	constexpr float MeshSize = 100.f;

	const FLinearColor SwirlColor(0.6f, 0.36f, 1.f);
	const FLinearColor SheetColor(0.5f, 0.28f, 1.f);
	const FLinearColor SparkleColor(0.78f, 0.58f, 1.f);

	bool IsServerWorld(const UWorld* World)
	{
		return World && World->GetNetMode() != NM_Client;
	}

	/** TickTransit: something that went into an end this frame (From), at Point. */
	struct FEntry
	{
		TWeakObjectPtr<AActor> Thing;
		int32 From = 0;
		FVector Point = FVector::ZeroVector;
	};

	/** A fish hook is reeling Penguin in (the hook holds it: no portal takes it). */
	bool IsHooked(const UWorld* World, const APenguinCharacter* Penguin)
	{
		TArray<APenguinProjectile*> Shots;
		APenguinProjectile::GetAll(World, Shots);
		for (const APenguinProjectile* Shot : Shots)
		{
			if (Shot->GetKind() == EPenguinProjectileKind::FishHook && Shot->GetHookedPenguin() == Penguin)
			{
				return true;
			}
		}
		return false;
	}
}

APinguPortal::APinguPortal()
{
	using namespace PinguPortalPrivate;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // after everything moved this frame
	SetActorEnableCollision(false);

	// LAN: spawned by the server; it never moves, only its ends and penguin replicate.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(10.f);
	SetMinNetUpdateFrequency(2.f);

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;

	static const TCHAR* PartNames[2][NumParts] = {
		{ TEXT("FrameA"), TEXT("SwirlOuterA"), TEXT("SwirlInnerA"), TEXT("SheetA") },
		{ TEXT("FrameB"), TEXT("SwirlOuterB"), TEXT("SwirlInnerB"), TEXT("SheetB") },
	};
	for (int32 End = 0; End < 2; ++End)
	{
		for (int32 Part = 0; Part < NumParts; ++Part)
		{
			UStaticMeshComponent* Mesh = CreateDefaultSubobject<UStaticMeshComponent>(PartNames[End][Part]);
			Mesh->SetupAttachment(SceneRoot);
			Mesh->SetUsingAbsoluteLocation(true);
			Mesh->SetUsingAbsoluteRotation(true);
			Mesh->SetUsingAbsoluteScale(true);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetGenerateOverlapEvents(false);
			Mesh->SetCanEverAffectNavigation(false);
			Mesh->SetCastShadow(false);
			Mesh->SetVisibility(false); // until the ends are known (UpdateLook)
			Parts.Add(Mesh);
		}
	}
}

void APinguPortal::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APinguPortal, EndA);
	DOREPLIFETIME(APinguPortal, EndB);
	DOREPLIFETIME(APinguPortal, PortalOwner);
}

void APinguPortal::BeginPlay()
{
	Super::BeginPlay();
	SetupLook();
}

void APinguPortal::OnRep_Portal()
{
	ApplyColors();
}

// ---------------------------------------------------------------------------- opening and closing

APinguPortal* APinguPortal::Open(UWorld* World, APenguinCharacter* InOwner, const FVector& From, const FVector& To)
{
	using namespace PinguPortalPrivate;
	if (!IsServerWorld(World) || !InOwner)
	{
		return nullptr;
	}
	if (APinguPortal* Old = FindOwnedBy(World, InOwner))
	{
		Old->Close(); // one portal each: the new one replaces it
	}
	const FVector A(From.X, 0.f, From.Z);
	const FVector B(To.X, 0.f, To.Z);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APinguPortal* Portal = World->SpawnActor<APinguPortal>(APinguPortal::StaticClass(), FTransform(A), Params);
	if (!Portal)
	{
		return nullptr;
	}
	Portal->EndA = A;
	Portal->EndB = B;
	Portal->PortalOwner = InOwner;
	Portal->ApplyColors();
	// (Whatever is in an end already - the warper where it arrived, a shot it swapped places with - goes through only
	// once it has been out: TickTransit.)
	FPinguFXBroadcastScope Broadcast(Portal);
	for (const FVector& End : { A, B })
	{
		APinguFX::Ring(Portal, End, EndHalfHeight, SparkleColor, 0.3f);
		APinguFX::Sparkles(Portal, End, 6, EndHalfWidth, SparkleColor);
	}
	return Portal;
}

void APinguPortal::Close()
{
	using namespace PinguPortalPrivate;
	if (!HasAuthority() || IsActorBeingDestroyed())
	{
		return;
	}
	FPinguFXBroadcastScope Broadcast(this);
	for (const FVector& End : { EndA, EndB })
	{
		APinguFX::Puffs(this, End, 6, 30.f, 220.f, FVector::UpVector, 1.f, FLinearColor(0.85f, 0.8f, 1.f));
		APinguFX::Sparkles(this, End, 5, EndHalfWidth, SparkleColor);
	}
	Destroy();
}

void APinguPortal::CloseAll(UWorld* World)
{
	if (!PinguPortalPrivate::IsServerWorld(World))
	{
		return;
	}
	TArray<APinguPortal*> All;
	GetAll(World, All);
	for (APinguPortal* Portal : All)
	{
		Portal->Destroy();
	}
}

int32 APinguPortal::CloseInArea(UWorld* World, const APenguinCharacter* By, const FVector& Origin, float AreaRadius)
{
	if (!PinguPortalPrivate::IsServerWorld(World) || AreaRadius <= 0.f)
	{
		return 0;
	}
	TArray<APinguPortal*> All;
	GetAll(World, All);
	int32 Closed = 0;
	for (APinguPortal* Portal : All)
	{
		if (By && Portal->GetPortalOwner() == By)
		{
			continue; // your own portal stays (like your own shots)
		}
		for (int32 End = 0; End < 2; ++End)
		{
			// The end's oval reaches into the wind (its middle within the radius plus the oval's half width).
			FVector D = Portal->GetEnd(End) - Origin;
			D.Y = 0.f;
			if (D.Size() <= AreaRadius + EndHalfWidth)
			{
				UE_LOG(LogPingu, Display, TEXT("[GAME] a gust blows out %s's portal"),
					Portal->GetPortalOwner() ? *Portal->GetPortalOwner()->GetDisplayName() : TEXT("nobody"));
				Portal->Close(); // a puff at both ends (on every machine), then it goes
				++Closed;
				break;
			}
		}
	}
	return Closed;
}

void APinguPortal::GetAll(const UWorld* World, TArray<APinguPortal*>& Out)
{
	Out.Reset();
	if (!World)
	{
		return;
	}
	for (TActorIterator<APinguPortal> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed())
		{
			Out.Add(*It);
		}
	}
}

APinguPortal* APinguPortal::FindOwnedBy(const UWorld* World, const APenguinCharacter* InOwner)
{
	TArray<APinguPortal*> All;
	GetAll(World, All);
	for (APinguPortal* Portal : All)
	{
		if (InOwner && Portal->GetPortalOwner() == InOwner)
		{
			return Portal;
		}
	}
	return nullptr;
}

bool APinguPortal::IsInEnd(int32 Index, const FVector& Point, float Margin) const
{
	const FVector End = GetEnd(Index);
	const float Dx = static_cast<float>(Point.X - End.X) / (EndHalfWidth + Margin);
	const float Dz = static_cast<float>(Point.Z - End.Z) / (EndHalfHeight + Margin);
	return Dx * Dx + Dz * Dz < 1.f;
}

// ---------------------------------------------------------------------------- Snow Warp stays on screen

FBox2D APinguPortal::ToReferenceView(const FBox2D& View)
{
	if (!View.bIsValid)
	{
		return View;
	}
	const double MiddleX = 0.5 * (View.Min.X + View.Max.X);
	const double HalfWidth = 0.5 * (View.Max.Y - View.Min.Y) * WarpReferenceAspect;
	FBox2D Cut = View;
	Cut.Min.X = FMath::Max(View.Min.X, MiddleX - HalfWidth);
	Cut.Max.X = FMath::Min(View.Max.X, MiddleX + HalfWidth);
	return Cut;
}

bool APinguPortal::GetWarpView(const APenguinCharacter* Placer, FBox2D& OutView)
{
	// On an arena only (rounds, the lobby, the menu's demo: the stage camera frames the island). The practice course and the
	// tests' close-up cameras: no limit.
	FBox2D Frame(ForceInit);
	const APenguinCamera* StageCamera = Placer ? APenguinCamera::FindOrSpawn(Placer->GetWorld()) : nullptr;
	if (!StageCamera || !StageCamera->GetSharedFrameRect(Frame))
	{
		return false;
	}
	// The player's own screen (a LAN client's as its machine sent it), no wider than 16:9. A bot (or a player whose screen
	// is not known yet): the island as the camera frames it.
	FBox2D View(ForceInit);
	const APenguinPlayerController* PlayerController = Cast<APenguinPlayerController>(Placer->GetController());
	View = PlayerController && PlayerController->GetWarpView(View) ? ToReferenceView(View) : Frame;
	const FVector2D Margin = View.GetExtent() * WarpViewMargin;
	OutView = FBox2D(View.Min + Margin, View.Max - Margin);
	return true;
}

FVector APinguPortal::ClampWarpMove(const APenguinCharacter* Placer, const FVector& Start, const FVector& Delta)
{
	FBox2D View(ForceInit);
	if (!GetWarpView(Placer, View))
	{
		return Delta;
	}
	// Room for all of the penguin at the exit, and for the warp's lift above where it aims.
	const UCapsuleComponent* Capsule = Placer->GetCapsuleComponent();
	const double Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 40.0;
	const double Half = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 60.0;
	FVector2D RoomMin = View.Min + FVector2D(Radius, Half);
	FVector2D RoomMax = View.Max - FVector2D(Radius, Half + WarpLiftHalfHeights * Half);
	for (int32 Axis = 0; Axis < 2; ++Axis)
	{
		if (RoomMin[Axis] > RoomMax[Axis])
		{
			RoomMin[Axis] = RoomMax[Axis] = 0.5 * (View.Min[Axis] + View.Max[Axis]); // a giant on a small screen: the middle
		}
	}
	const FVector End = Start + Delta;
	const FVector Kept(FMath::Clamp(End.X, RoomMin.X, RoomMax.X), End.Y, FMath::Clamp(End.Z, RoomMin.Y, RoomMax.Y));
	FVector Move = Kept - Start;
	if (Move.SizeSquared() > Delta.SizeSquared())
	{
		// The warper is outside its own view (a moment's lag): toward it, but no further than the warp goes.
		Move = Move.GetSafeNormal() * Delta.Size();
	}
	if (!Move.Equals(Delta, 1.0))
	{
		UE_LOG(LogPingu, Display, TEXT("[WARP] %s's exit kept on its screen: %.0f, %.0f instead of %.0f, %.0f (view x %.0f..%.0f, z %.0f..%.0f)"),
			*Placer->GetDisplayName(), Start.X + Move.X, Start.Z + Move.Z, End.X, End.Z, View.Min.X, View.Max.X, View.Min.Y, View.Max.Y);
	}
	return Move;
}

bool APinguPortal::IsInWarpView(const APenguinCharacter* Placer, const FVector& Where)
{
	FBox2D View(ForceInit);
	return !GetWarpView(Placer, View) || View.IsInside(FVector2D(Where.X, Where.Z));
}

// ---------------------------------------------------------------------------- going through

void APinguPortal::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority())
	{
		if (!IsValid(PortalOwner))
		{
			Destroy(); // its penguin left the game
			return;
		}
		TickTransit(DeltaSeconds);
	}
	UpdateLook(DeltaSeconds);
}

void APinguPortal::ForEachTraveller(float DeltaSeconds, TFunctionRef<void(AActor* Thing, const FVector& Point, const FVector& Back)> Visit) const
{
	using namespace PinguPortalPrivate;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Penguins still in (a hooked one is held by the hook), by their middle.
	for (TActorIterator<APenguinCharacter> It(World); It; ++It)
	{
		APenguinCharacter* Penguin = *It;
		if (IsValid(Penguin) && Penguin->IsAlive() && !Penguin->IsHidden() && !IsHooked(World, Penguin))
		{
			Visit(Penguin, Penguin->GetActorLocation(), FVector::ZeroVector);
		}
	}
	// Shots in flight and bombs (not the fish hook: its line ties it to the thrower).
	TArray<APenguinProjectile*> Shots;
	APenguinProjectile::GetAll(World, Shots);
	for (APenguinProjectile* Shot : Shots)
	{
		if (Shot->IsFlying() && Shot->GetKind() != EPenguinProjectileKind::FishHook)
		{
			Visit(Shot, Shot->GetActorLocation(), Shot->GetVelocity() * (DeltaSeconds * 0.5f));
		}
	}
	// Ability bubbles (not one on a hook).
	TArray<APinguPickup*> Bubbles;
	APinguPickup::GetAll(World, Bubbles);
	for (APinguPickup* Bubble : Bubbles)
	{
		if (!Bubble->IsOnHook())
		{
			Visit(Bubble, Bubble->GetActorLocation(), FVector::ZeroVector);
		}
	}
}

void APinguPortal::TickTransit(float DeltaSeconds)
{
	using namespace PinguPortalPrivate;
	Eligible.RemoveAllSwap([](const TWeakObjectPtr<AActor>& Thing) { return !Thing.IsValid(); });
	// Who goes through this frame (collected first: sending them through moves things around).
	TArray<FEntry> Entered;
	ForEachTraveller(DeltaSeconds, [this, &Entered](AActor* Thing, const FVector& Point, const FVector& Back)
	{
		const int32 InEnd = IsInEnd(0, Point) ? 0 : (IsInEnd(1, Point) ? 1 : INDEX_NONE);
		const FVector Before = Point - Back; // half a frame back along its path (fast shots)
		if (Eligible.Contains(Thing))
		{
			// It had been out: in an end now, or it flew through an end's edge during this frame - through it goes.
			if (InEnd != INDEX_NONE)
			{
				Entered.Add({ Thing, InEnd, Point });
			}
			else if (!Back.IsNearlyZero() && (IsInEnd(0, Before) || IsInEnd(1, Before)))
			{
				Entered.Add({ Thing, IsInEnd(0, Before) ? 0 : 1, Before });
			}
			return;
		}
		// Not out of the ends since we know it (it stood or appeared in one, or just came out of one).
		if (!IsInEnd(0, Point, ExitMargin) && !IsInEnd(1, Point, ExitMargin))
		{
			Eligible.Add(Thing); // out now: the next time it comes into an end, it goes through
		}
		else if (InEnd != INDEX_NONE && !Back.IsNearlyZero() && !IsInEnd(InEnd, Before, ExitMargin))
		{
			Entered.Add({ Thing, InEnd, Point }); // a fast shot seen for the first time, flown into an end this frame
		}
	});
	for (const FEntry& Entry : Entered)
	{
		if (AActor* Thing = Entry.Thing.Get())
		{
			// In the other end now (or it stays in this one: no room for a penguin there) - out again before the next time.
			Eligible.Remove(Thing);
			SendThrough(Thing, Entry.From, Entry.Point);
		}
	}
}

bool APinguPortal::SendThrough(AActor* Thing, int32 From, const FVector& Point)
{
	using namespace PinguPortalPrivate;
	UWorld* World = GetWorld();
	const FVector In = GetEnd(From);
	const FVector Out = GetEnd(1 - From);
	if (APenguinCharacter* Penguin = Cast<APenguinCharacter>(Thing))
	{
		// A penguin comes out of the middle of the other end - if there is room for it there (an ice wall built into
		// that end keeps penguins out; shots still fly into it).
		const UCapsuleComponent* Capsule = Penguin->GetCapsuleComponent();
		FCollisionQueryParams Params(SCENE_QUERY_STAT(PinguPortal), false, Penguin);
		const FCollisionShape Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight());
		if (World->OverlapBlockingTestByChannel(Out, FQuat::Identity, ECC_Pawn, Shape, Params))
		{
			return false;
		}
		// With the speed it had: sliding, stomping, flying or frozen it goes on doing it.
		const UPenguinMovementComponent* Move = Penguin->GetPenguinMovement();
		const FVector KeepVelocity = Move ? Move->Velocity : FVector::ZeroVector;
		Penguin->NetTeleport(Out, KeepVelocity, Move && !Move->IsMovingOnGround(), true);
	}
	else
	{
		// A shot or a bubble comes out where it went in, relative to the end (well inside its rim).
		FVector Offset = Point - In;
		Offset.Y = 0.f;
		const float Dx = static_cast<float>(Offset.X) / EndHalfWidth;
		const float Dz = static_cast<float>(Offset.Z) / EndHalfHeight;
		const float Reach = FMath::Sqrt(Dx * Dx + Dz * Dz);
		if (Reach > MaxExitOffset)
		{
			Offset *= MaxExitOffset / Reach;
		}
		if (APenguinProjectile* Shot = Cast<APenguinProjectile>(Thing))
		{
			Shot->WarpTo(Out + Offset);
		}
		else if (APinguPickup* Bubble = Cast<APinguPickup>(Thing))
		{
			Bubble->WarpTo(Out + Offset);
		}
	}
	++TransitCount;
	FPinguFXBroadcastScope Broadcast(this);
	APinguFX::Sparkles(this, In, 4, EndHalfWidth * 0.6f, SparkleColor);
	APinguFX::Sparkles(this, Out, 6, EndHalfWidth * 0.6f, SparkleColor);
	APinguFX::Sound(this, EPinguSound::Warp, Cast<APenguinCharacter>(Thing) ? 0.7f : 0.35f, 0.08f, 1.25f);
	UE_LOG(LogPingu, Verbose, TEXT("[PORTAL] %s went through (%d so far)"), *Thing->GetName(), TransitCount);
	return true;
}

// ---------------------------------------------------------------------------- look

void APinguPortal::SetupLook()
{
	using namespace PinguPortalPrivate;
	UStaticMesh* Ring = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Ring.SM_Ring"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	UStaticMesh* Ball = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FX/SM_Bubble.SM_Bubble"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	if (!Ball)
	{
		Ball = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	}
	if (!Ring)
	{
		Ring = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	}
	UMaterialInterface* Prop = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Prop.M_Prop"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	UMaterialInterface* Bubble = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/FX/M_Bubble.M_Bubble"), nullptr, LOAD_Quiet | LOAD_NoWarn);
	if (Prop)
	{
		FrameMaterial = UMaterialInstanceDynamic::Create(Prop, this);
		FrameMaterial->SetScalarParameterValue(TEXT("Emissive"), 1.f);
		SwirlMaterial = UMaterialInstanceDynamic::Create(Prop, this);
		SwirlMaterial->SetVectorParameterValue(TEXT("Color"), SwirlColor);
		SwirlMaterial->SetScalarParameterValue(TEXT("Emissive"), 1.2f);
	}
	if (Bubble)
	{
		SheetMaterial = UMaterialInstanceDynamic::Create(Bubble, this);
		SheetMaterial->SetVectorParameterValue(TEXT("Color"), SheetColor);
		SheetMaterial->SetScalarParameterValue(TEXT("Opacity"), 2.2f); // M_Bubble: mostly clear in the middle, this makes it a violet haze
	}
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		UStaticMeshComponent* Mesh = Parts[Index];
		const int32 Part = Index % NumParts;
		Mesh->SetStaticMesh(Part == Sheet ? Ball : Ring);
		UMaterialInterface* Material = nullptr;
		if (Part == Sheet)
		{
			Material = SheetMaterial;
		}
		else if (Part == Frame)
		{
			Material = FrameMaterial;
		}
		else
		{
			Material = SwirlMaterial;
		}
		if (Material)
		{
			Mesh->SetMaterial(0, Material);
		}
	}
	bLookReady = true;
	ApplyColors();
}

void APinguPortal::ApplyColors()
{
	// The frame in its penguin's colour: whose portal it is.
	if (FrameMaterial)
	{
		const APenguinCharacter* Penguin = PortalOwner.Get();
		FrameMaterial->SetVectorParameterValue(TEXT("Color"), Penguin ? Penguin->GetPlayerColor() : PinguPortalPrivate::SwirlColor);
	}
}

void APinguPortal::UpdateLook(float DeltaSeconds)
{
	using namespace PinguPortalPrivate;
	if (!bLookReady || (EndA.IsZero() && EndB.IsZero()))
	{
		return; // a client before the ends arrived
	}
	Age += DeltaSeconds;
	// Pops open (a little overshoot), then breathes.
	const float T = FMath::Clamp(Age / OpenTime, 0.f, 1.f);
	const float Pop = 1.f + 2.70158f * FMath::Pow(T - 1.f, 3.f) + 1.70158f * FMath::Pow(T - 1.f, 2.f); // ease out back
	const float Breath = 1.f + 0.035f * FMath::Sin(Age * 5.f);
	const FVector FrameScale(2.f * EndHalfWidth / MeshSize, 0.9f, 2.f * EndHalfHeight / MeshSize);
	for (int32 End = 0; End < 2; ++End)
	{
		const FVector Center = GetEnd(End);
		for (int32 Part = 0; Part < NumParts; ++Part)
		{
			UStaticMeshComponent* Mesh = Parts[End * NumParts + Part];
			FVector Scale = FrameScale * FMath::Max(Pop, 0.01f);
			FRotator Spin = FRotator::ZeroRotator;
			FVector Where = Center;
			switch (Part)
			{
			case Frame:
				Scale *= Breath;
				break;
			case SwirlOuter:
				Scale *= SwirlOuterShare;
				Spin.Pitch = Age * SwirlOuterSpeed; // turns in the play plane (about y)
				break;
			case SwirlInner:
				Scale *= SwirlInnerShare;
				Spin.Pitch = Age * SwirlInnerSpeed;
				break;
			default: // the sheet: a flat glowing lens a little behind the rings
				Scale = FVector(Scale.X * SheetShare, SheetDepth / MeshSize, Scale.Z * SheetShare) / Breath;
				Where.Y -= 4.f;
				break;
			}
			Mesh->SetWorldLocationAndRotation(Where, Spin);
			Mesh->SetWorldScale3D(Scale);
			if (!Mesh->IsVisible())
			{
				Mesh->SetVisibility(true);
			}
		}
	}
	// Now and then a sparkle on the rims (every machine its own).
	SparkleTimer -= DeltaSeconds;
	if (SparkleTimer <= 0.f)
	{
		SparkleTimer = SparkleInterval;
		for (int32 End = 0; End < 2; ++End)
		{
			const float Angle = FMath::FRandRange(0.f, 2.f * UE_PI);
			const FVector Rim = GetEnd(End) + FVector(FMath::Cos(Angle) * EndHalfWidth, 0.f, FMath::Sin(Angle) * EndHalfHeight);
			APinguFX::Sparkles(this, Rim, 1, 8.f, SparkleColor);
		}
	}
}

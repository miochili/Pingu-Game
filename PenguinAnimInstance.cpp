// Pingu - code-driven animation.

#include "PenguinAnimInstance.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimationPoseData.h"
#include "BonePose.h"
#include "PenguinAbilities.h"
#include "PenguinCharacter.h"
#include "PenguinTraitLooks.h"
#include "PinguLobby.h"

namespace PenguinAnimPrivate
{
	constexpr float Fps = 24.f; // clips are authored at 24 fps in Blender
	// Ground speed (cm/s) each generated gait clip is built for (Tools/penguin_anims.py GROUND_SPEED).
	constexpr float WaddleSpeed = 200.f;
	constexpr float JogSpeed = 380.f;
	constexpr float RunFastSpeed = 560.f;

	static EPenguinClip Fallback(EPenguinClip Clip)
	{
		switch (Clip)
		{
		case EPenguinClip::Waddle: return EPenguinClip::Walk;
		case EPenguinClip::Jog: return EPenguinClip::Run;
		case EPenguinClip::RunFast: return EPenguinClip::Sprint;
		case EPenguinClip::FallFlail: return EPenguinClip::FallLoop;
		case EPenguinClip::Tumble: return EPenguinClip::FallLoop;
		case EPenguinClip::Frozen: return EPenguinClip::Idle;
		case EPenguinClip::Victory: return EPenguinClip::Idle;
		case EPenguinClip::Shield: return EPenguinClip::Idle;
		case EPenguinClip::GroundPound: return EPenguinClip::FallLoop;
		case EPenguinClip::KO: return EPenguinClip::LandHard;
		case EPenguinClip::Toss:
		case EPenguinClip::TossL: return EPenguinClip::Throw;
		case EPenguinClip::ThrowUp:
		case EPenguinClip::ThrowUpL: return EPenguinClip::ThrowToss;
		case EPenguinClip::Swat:
		case EPenguinClip::SwatL: return EPenguinClip::SlapQuick;
		case EPenguinClip::Spin: return EPenguinClip::SlapSpin;
		case EPenguinClip::Cast:
		case EPenguinClip::Summon: return EPenguinClip::SlapHeavy;
		case EPenguinClip::Noot: return EPenguinClip::Cast;
		case EPenguinClip::Climb: return EPenguinClip::DoubleJump;
		case EPenguinClip::Slip: return EPenguinClip::SlideStart; // icy feet slip onto the belly: at least dive onto it
		default: return EPenguinClip::Count;
		}
	}

	static float NominalSpeed(EPenguinClip Clip)
	{
		switch (Clip)
		{
		case EPenguinClip::Waddle: return WaddleSpeed;
		case EPenguinClip::Jog: return JogSpeed;
		default: return RunFastSpeed;
		}
	}

	static void BlendInto(FPoseContext& Target, const FPoseContext& Source, float Alpha)
	{
		for (const FCompactPoseBoneIndex BoneIndex : Target.Pose.ForEachBoneIndex())
		{
			Target.Pose[BoneIndex].BlendWith(Source.Pose[BoneIndex], Alpha);
		}
	}

	/** How long a replicated movement event waits for its movement mode (s). */
	constexpr float EventGrace = 0.15f;

	// The icicle throw's flipper poses (Flare / Swing, degrees from the flipper's rest; Lean: the body forward). The arm swings
	// in the body's side plane - the plane the camera looks at - only a little out to the side: Swing -90 points it straight
	// back, -180 straight up, -270 straight forward (it goes on over the head the throw's way). Charging: it reaches behind
	// the back for the icicle, cocks it up over the head as it comes out, then draws back as the charge grows; the throw
	// swings it over the head to the front and through.
	constexpr float ThrowGrabTime = 0.04f;        // s into the pull-out: the flipper is behind the back
	constexpr float ThrowFadeIn = 0.04f;          // s for the pose to take over ...
	constexpr float ThrowFadeOut = 0.12f;         // ... and to give the flipper back after the follow-through
	constexpr float ThrowSwapTime = 0.1f;         // s for the two arms to swap roles when the aim crosses over (the penguin turns round)
	constexpr float ThrowGrab[3] = { 22.f, -60.f, 0.f };     // down and back behind the body
	constexpr float ThrowCocked[3] = { 15.f, -165.f, -6.f }; // up over the head, a little back
	constexpr float ThrowDrawn[3] = { 15.f, -128.f, -14.f }; // full charge: drawn far back, leaning back
	// (The face is turned a little toward the camera: a flipper pointing straight ahead at head height would cover it. So the
	// icicle leaves with the flipper high in front, above the face, and it follows through low, below it.)
	constexpr float ThrowRelease[3] = { 20.f, -208.f, 12.f };// the icicle leaves: high in front, leaning in
	constexpr float ThrowFollow[3] = { 30.f, -292.f, 9.f };  // and through, down in front
	constexpr float ThrowTrembleFlare = 4.f;      // the arm trembles more and more as it charges, this much (degrees) at full ...
	constexpr float ThrowTrembleSwing = 3.f;
	constexpr float ThrowTrembleRate = 19.f;      // ... this fast (Hz)
	constexpr float ThrowOtherFlare = 35.f;       // the other flipper out for balance
	constexpr float ThrowOtherSwing = 12.f;

	/**
	 * Replicated movement only: an event (counter change) seen before the movement mode it belongs to is held back until
	 * that mode shows up - for at most EventGrace, then it is dropped (as it would be without the wait).
	 */
	static void HoldUntilMode(bool& bEvent, float& PendingAge, bool bModeReady, float Dt)
	{
		if (bEvent)
		{
			PendingAge = bModeReady ? -1.f : 0.f;
			bEvent = bModeReady;
			return;
		}
		if (PendingAge < 0.f)
		{
			return;
		}
		if (bModeReady)
		{
			bEvent = true;
			PendingAge = -1.f;
			return;
		}
		PendingAge += Dt;
		if (PendingAge > EventGrace)
		{
			PendingAge = -1.f;
		}
	}
}

FAnimInstanceProxy* UPenguinAnimInstance::CreateAnimInstanceProxy()
{
	return new FPenguinAnimInstanceProxy(this);
}

void UPenguinAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FPenguinAnimInstanceProxy*>(InProxy);
}

// ---------------------------------------------------------------------------- helpers

bool FPenguinAnimInstanceProxy::IsLoopingClip(EPenguinClip Clip)
{
	switch (Clip)
	{
	case EPenguinClip::Idle:
	case EPenguinClip::WalkSlow:
	case EPenguinClip::Walk:
	case EPenguinClip::WalkFast:
	case EPenguinClip::Run:
	case EPenguinClip::Sprint:
	case EPenguinClip::WalkIce:
	case EPenguinClip::DashLoop:
	case EPenguinClip::JumpLoop:
	case EPenguinClip::FallLoop:
	case EPenguinClip::Slide:
	case EPenguinClip::ThrowAim:
	case EPenguinClip::Waddle:
	case EPenguinClip::Jog:
	case EPenguinClip::RunFast:
	case EPenguinClip::FallFlail:
	case EPenguinClip::Tumble:
	case EPenguinClip::Frozen:
	case EPenguinClip::Victory:
	case EPenguinClip::Shield:
	case EPenguinClip::GroundPound:
		return true;
	default:
		return false;
	}
}

bool FPenguinAnimInstanceProxy::IsFidget(EPenguinClip Clip)
{
	return Clip == EPenguinClip::IdleLook || Clip == EPenguinClip::IdleFlap || Clip == EPenguinClip::IdleTap;
}

const UAnimSequence* FPenguinAnimInstanceProxy::GetClip(EPenguinClip Clip) const
{
	const int32 Index = static_cast<int32>(Clip);
	const UAnimSequence* Sequence = ClipTable.IsValidIndex(Index) ? ClipTable[Index] : nullptr;
	if (!Sequence)
	{
		const EPenguinClip Other = PenguinAnimPrivate::Fallback(Clip);
		const int32 OtherIndex = static_cast<int32>(Other);
		if (Other != EPenguinClip::Count && ClipTable.IsValidIndex(OtherIndex))
		{
			Sequence = ClipTable[OtherIndex];
		}
	}
	return Sequence;
}

// ---------------------------------------------------------------------------- game thread

void FPenguinAnimInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	In = FPenguinAnimInputs();
	ClipTable.SetNumZeroed(static_cast<int32>(EPenguinClip::Count));
	if (const APenguinCharacter* Penguin = Cast<APenguinCharacter>(InAnimInstance->GetOwningActor()))
	{
		Penguin->GetAnimInputs(In);
		// Flap: the beat runs while it flaps and eases out after (ApplyFlap).
		FlapAlpha = FMath::FInterpTo(FlapAlpha, In.bFlapping ? 1.f : 0.f, DeltaSeconds, In.bFlapping ? 20.f : 8.f);
		FlapClock = FlapAlpha > 0.01f ? FlapClock + DeltaSeconds : 0.f;
		BodyShape = PinguTraitLooks::GetBodyShape(Penguin);
		const PinguLobby::FAnimState Lobby = PinguLobby::GetAnimState(Penguin);
		bLobby = Lobby.bLobby;
		LobbyPokeAge = Lobby.PokeAge;
		LobbyPokeDir = Lobby.PokeDir;
		const USkeletalMeshComponent* SkelMesh = Penguin->GetMesh();
		MeshUnitsPerCm = SkelMesh ? 1.f / FMath::Max(static_cast<float>(SkelMesh->GetComponentScale().Z), 0.01f) : 1.f;
		LobbyPose = static_cast<uint8>(Lobby.Pose);
		LobbySide = Lobby.Side;
		LobbyGag = static_cast<uint8>(Lobby.Gag);
		LobbyGagAge = Lobby.GagAge;
		LobbyGagDir = Lobby.GagDir;
		LobbyFlinchAge = Lobby.FlinchAge;
		LobbyFlinchDir = Lobby.FlinchDir;
		if (SkelMesh && bLobby)
		{
			// The mesh looks along its +Y: the beak's way in the world now; world vectors into the component's frame.
			const FQuat Rotation = SkelMesh->GetComponentQuat();
			const FVector Beak = Rotation.RotateVector(FVector(0.f, 1.f, 0.f));
			LobbyFacingYaw = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Beak.Y), static_cast<float>(Beak.X)));
			LobbyOffset = Rotation.UnrotateVector(Lobby.Offset) * MeshUnitsPerCm;
			LobbyViewAxis = Rotation.UnrotateVector(FVector(0.f, 1.f, 0.f));
		}
		else
		{
			LobbyOffset = FVector::ZeroVector;
		}
		// The fish's fat (cosmetic, lobby only): rounder and a little wider forward, hardly taller.
		// The fish going down: a stretch, then a bump that swells the belly a moment (before the fat).
		if (Lobby.Gag == PinguLobby::EGag::Fish && Lobby.GagAge > PinguLobby::FishGulpTime)
		{
			const float Since = Lobby.GagAge - PinguLobby::FishGulpTime;
			const float Stretch = Since < 0.15f ? FMath::Sin(Since / 0.15f * UE_PI) : 0.f;
			const float Bulge = Since >= 0.12f && Since < 0.42f ? FMath::Sin((Since - 0.12f) / 0.3f * UE_PI) : 0.f;
			BodyShape *= FVector(1.f - 0.06f * Stretch + 0.16f * Bulge, 1.f - 0.06f * Stretch + 0.16f * Bulge, 1.f + 0.1f * Stretch - 0.04f * Bulge);
		}
		if (Lobby.Fat > 0.001f)
		{
			BodyShape *= FVector(1.f + 0.7f * Lobby.Fat, 1.f + 0.7f * Lobby.Fat, 1.f + 0.08f * Lobby.Fat);
		}
		bRemotePenguin = !Penguin->IsLocallySimulated();
		ThrowStage = 0;
		if (const UPenguinAbilityComponent* PenguinAbilities = Penguin->GetAbilities())
		{
			ThrowStage = static_cast<uint8>(PenguinAbilities->GetIcicleShow());
			ThrowStageTime = PenguinAbilities->GetIcicleShowTime();
			ThrowChargeTime = PenguinAbilities->GetIcicleChargeTime();
		}
		for (int32 Index = 0; Index < static_cast<int32>(EPenguinClip::Count); ++Index)
		{
			ClipTable[Index] = Penguin->GetAnimClip(static_cast<EPenguinClip>(Index));
		}
	}
}

// ---------------------------------------------------------------------------- worker thread

void FPenguinAnimInstanceProxy::Update(float DeltaSeconds)
{
	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.1f);
	LastDt = Dt;
	TotalTime += Dt;
	if (!bStarted)
	{
		bStarted = true;
		Rand.Initialize(static_cast<int32>(reinterpret_cast<UPTRINT>(this) & 0x7fffffff));
		NextFidget = Rand.FRandRange(4.f, 8.f);
		BlinkTimer = Rand.FRandRange(1.5f, 4.f);
		SeenJump = In.JumpCount;
		SeenAirJump = In.AirJumpCount;
		SeenDash = In.DashCount;
		SeenSlide = In.SlideCount;
		SeenLand = In.LandCount;
		SeenHit = In.HitCount;
		SeenAction = In.ActionCount;
		PlayLocomotion(0.f);
	}
	UpdateLocomotion(Dt);
	UpdateStateMachine(Dt);
	UpdateAction(Dt, CurrentMode);
	UpdateLobby(Dt, CurrentMode);
	UpdateIcicleThrow(Dt);
	AdvanceLayers(Dt);

	// Blink every few seconds (sometimes twice).
	if (BlinkTime >= 0.f)
	{
		BlinkTime += Dt;
		if (BlinkTime > 0.15f)
		{
			BlinkTime = -1.f;
		}
	}
	else
	{
		BlinkTimer -= Dt;
		if (BlinkTimer <= 0.f)
		{
			BlinkTime = 0.f;
			BlinkTimer = Rand.FRand() < 0.2f ? 0.22f : Rand.FRandRange(2.2f, 5.f);
		}
	}
}

void FPenguinAnimInstanceProxy::Play(EPenguinClip Clip, float BlendTime, float Rate, float StartTime)
{
	FPenguinAnimLayer Layer;
	Layer.Clip = Clip;
	Layer.Sequence = GetClip(Clip);
	Layer.Length = Layer.Sequence ? Layer.Sequence->GetPlayLength() : 0.f;
	Layer.bLoop = IsLoopingClip(Clip);
	Layer.Rate = Rate;
	Layer.Time = FMath::Clamp(StartTime, 0.f, FMath::Max(Layer.Length, 0.f));
	if (BlendTime <= UE_KINDA_SMALL_NUMBER)
	{
		Layers.Reset();
		Layer.Weight = 1.f;
		Layer.FadeInSpeed = 1000.f;
	}
	else
	{
		Layer.Weight = 0.f;
		Layer.FadeInSpeed = 1.f / BlendTime;
	}
	Layers.Add(Layer);
	while (Layers.Num() > 6)
	{
		Layers.RemoveAt(0);
	}
	CurrentClip = Clip;
}

void FPenguinAnimInstanceProxy::PlayLocomotion(float BlendTime)
{
	FPenguinAnimLayer Layer;
	Layer.Clip = EPenguinClip::Idle;
	Layer.bLocomotion = true;
	Layer.bLoop = true;
	if (BlendTime <= UE_KINDA_SMALL_NUMBER)
	{
		Layers.Reset();
		Layer.Weight = 1.f;
		Layer.FadeInSpeed = 1000.f;
	}
	else
	{
		Layer.Weight = 0.f;
		Layer.FadeInSpeed = 1.f / BlendTime;
	}
	Layers.Add(Layer);
	while (Layers.Num() > 6)
	{
		Layers.RemoveAt(0);
	}
	CurrentClip = EPenguinClip::Idle;
}

bool FPenguinAnimInstanceProxy::PlayOneShot(EPenguinClip Clip, float BlendTime, float Rate, float StartFrame)
{
	if (!GetClip(Clip))
	{
		return false;
	}
	Play(Clip, BlendTime, Rate, StartFrame / PenguinAnimPrivate::Fps);
	bOneShotActive = true;
	return true;
}

bool FPenguinAnimInstanceProxy::IsTopLayerFinished() const
{
	if (Layers.Num() == 0)
	{
		return true;
	}
	const FPenguinAnimLayer& Top = Layers.Last();
	return !Top.bLocomotion && !Top.bLoop && Top.Time >= Top.Length - 0.001f;
}

bool FPenguinAnimInstanceProxy::IsOneShotStillValid(EMode Mode) const
{
	switch (CurrentClip)
	{
	case EPenguinClip::JumpStart:
	case EPenguinClip::JumpStartRun:
	case EPenguinClip::DoubleJump:
	case EPenguinClip::JumpApex:
	case EPenguinClip::DashToFall:
		return Mode == EMode::Air;
	case EPenguinClip::DashStart:
		return Mode == EMode::Dash;
	case EPenguinClip::SlideStart:
	case EPenguinClip::Slip:
		return Mode == EMode::Slide;
	case EPenguinClip::DashEnd:
	case EPenguinClip::SlideEnd:
		return Mode == EMode::Ground;
	case EPenguinClip::Land:
	case EPenguinClip::LandHard:
		// Landing recovery is cancelled as soon as you want to move.
		return Mode == EMode::Ground && FMath::Abs(In.InputX) < 0.1f && In.SpeedX < 200.f;
	case EPenguinClip::Skid:
		// Over once we run the new way.
		return Mode == EMode::Ground && !(In.SpeedX > 220.f && In.InputX * In.VelocityX > 0.f);
	case EPenguinClip::Stop:
		return Mode == EMode::Ground && FMath::Abs(In.InputX) < 0.1f;
	case EPenguinClip::IdleLook:
	case EPenguinClip::IdleFlap:
	case EPenguinClip::IdleTap:
	case EPenguinClip::Taunt: // (only the lobby island plays it as a one-shot idle; in a match it is an action)
		return Mode == EMode::Ground && FMath::Abs(In.InputX) < 0.1f && In.SpeedX < 40.f;
	case EPenguinClip::Hit:
		return Mode == EMode::Ground || Mode == EMode::Air || Mode == EMode::StunAir;
	default:
		return false;
	}
}

void FPenguinAnimInstanceProxy::UpdateLocomotion(float Dt)
{
	using namespace PenguinAnimPrivate;
	// Mesh-relative speed: a grown penguin's longer legs take fewer steps for the same ground speed (no moonwalking).
	const float Target = In.bGrounded ? In.SpeedX / FMath::Max(In.SizeScale, 0.1f) : 0.f;
	LocoSpeed = FMath::FInterpTo(LocoSpeed, Target, Dt, 20.f);
	const float S = LocoSpeed;
	MoveAlpha = FMath::SmoothStep(12.f, 80.f, S);

	if (S <= WaddleSpeed)
	{
		GaitA = GaitB = EPenguinClip::Waddle;
		GaitBlend = 0.f;
	}
	else if (S <= JogSpeed)
	{
		GaitA = EPenguinClip::Waddle;
		GaitB = EPenguinClip::Jog;
		GaitBlend = (S - WaddleSpeed) / (JogSpeed - WaddleSpeed);
	}
	else if (S <= RunFastSpeed)
	{
		GaitA = EPenguinClip::Jog;
		GaitB = EPenguinClip::RunFast;
		GaitBlend = (S - JogSpeed) / (RunFastSpeed - JogSpeed);
	}
	else
	{
		GaitA = GaitB = EPenguinClip::RunFast;
		GaitBlend = 0.f;
	}

	// One shared foot phase: it advances by speed / stride, so the planted foot matches the ground.
	auto Stride = [this](EPenguinClip Clip)
	{
		const UAnimSequence* Sequence = GetClip(Clip);
		const float Length = Sequence ? FMath::Max(Sequence->GetPlayLength(), 0.05f) : 0.4f;
		return NominalSpeed(Clip) * Length;
	};
	const float BlendedStride = FMath::Lerp(Stride(GaitA), Stride(GaitB), GaitBlend);
	if (MoveAlpha > 0.001f)
	{
		// Very slow shuffles keep a lively cadence (a tiny bit of foot slide below 90 cm/s).
		const float EffectiveSpeed = FMath::Clamp(S, 90.f, RunFastSpeed * 1.7f);
		LocoPhase = FMath::Fmod(LocoPhase + Dt * EffectiveSpeed / FMath::Max(BlendedStride, 1.f), 1.f);
	}
	else
	{
		LocoPhase = 0.f;
	}
	IdleTime += Dt;
}

void FPenguinAnimInstanceProxy::UpdateStateMachine(float Dt)
{
	EMode Mode;
	if (In.bKO)
	{
		Mode = EMode::KO;
	}
	else if (In.bFrozen)
	{
		Mode = EMode::Frozen;
	}
	else if (In.bStomping)
	{
		Mode = EMode::Stomp;
	}
	else if (In.bDashing)
	{
		Mode = EMode::Dash;
	}
	else if (In.bSliding && GetClip(EPenguinClip::Slide))
	{
		// On the belly (a slide, or a belly-flop dive in the air). Without the slide clips the character tips the mesh
		// over by hand and the ordinary clips go on (never the reference pose).
		Mode = EMode::Slide;
	}
	else if (In.bVictory && In.bGrounded)
	{
		Mode = EMode::Victory;
	}
	else if (In.bStunned && !In.bGrounded)
	{
		Mode = EMode::StunAir;
	}
	else
	{
		Mode = In.bGrounded ? EMode::Ground : EMode::Air;
	}
	CurrentMode = Mode;

	bool bJumpEvent = In.JumpCount != SeenJump;
	bool bAirJumpEvent = In.AirJumpCount != SeenAirJump;
	bool bLandEvent = In.LandCount != SeenLand;
	const bool bHitEvent = In.HitCount != SeenHit;
	if (bRemotePenguin)
	{
		using namespace PenguinAnimPrivate;
		HoldUntilMode(bJumpEvent, PendingJumpAge, Mode == EMode::Air, Dt);
		HoldUntilMode(bAirJumpEvent, PendingAirJumpAge, Mode == EMode::Air, Dt);
		HoldUntilMode(bLandEvent, PendingLandAge, Mode == EMode::Ground, Dt);
	}
	SeenJump = In.JumpCount;
	SeenAirJump = In.AirJumpCount;
	SeenDash = In.DashCount;
	SeenSlide = In.SlideCount;
	SeenLand = In.LandCount;
	SeenHit = In.HitCount;

	// ---- getting hit plays a reaction whatever else was going on
	bool bHitPlayed = false;
	if (bHitEvent && Mode != EMode::KO && Mode != EMode::Frozen)
	{
		bHitPlayed = PlayOneShot(EPenguinClip::Hit, 0.03f, 1.7f, 1.f);
	}

	// ---- entering a new mode
	if (Mode != PrevMode)
	{
		if (!bHitPlayed)
		{
			bOneShotActive = false;
		}
		switch (Mode)
		{
		case EMode::KO:
			bOneShotActive = false;
			Play(EPenguinClip::KO, 0.05f, 1.15f);
			break;
		case EMode::Frozen:
			bOneShotActive = false;
			Play(EPenguinClip::Frozen, 0.05f, 1.f);
			break;
		case EMode::Stomp:
			bOneShotActive = false;
			Play(EPenguinClip::GroundPound, 0.06f, 1.f);
			break;
		case EMode::Victory:
			bOneShotActive = false;
			Play(EPenguinClip::Victory, 0.15f, 1.f);
			break;
		case EMode::Dash:
			PlayOneShot(EPenguinClip::DashStart, 0.04f, 1.6f, 2.f);
			break;
		case EMode::Slide:
			if (In.bLastSlideWasSlip)
			{
				PlayOneShot(EPenguinClip::Slip, 0.05f, 1.5f, 4.f);
			}
			else
			{
				PlayOneShot(EPenguinClip::SlideStart, 0.05f, 1.7f, 1.f);
			}
			break;
		case EMode::StunAir:
			break;
		case EMode::Ground:
			if (PrevMode == EMode::Dash)
			{
				PlayOneShot(EPenguinClip::DashEnd, 0.05f, 1.5f);
			}
			else if (PrevMode == EMode::Slide)
			{
				PlayOneShot(EPenguinClip::SlideEnd, 0.06f, 1.6f);
			}
			else if (PrevMode == EMode::Stomp)
			{
				PlayOneShot(EPenguinClip::LandHard, 0.04f, 1.4f, 2.f);
			}
			break;
		case EMode::Air:
			if (PrevMode == EMode::Dash)
			{
				PlayOneShot(EPenguinClip::DashToFall, 0.05f, 1.4f);
			}
			bApexPending = false;
			break;
		}
	}
	const EMode LastMode = PrevMode;
	PrevMode = Mode;

	// Modes that own the whole pose (their clip was started on entry).
	if (Mode == EMode::KO || Mode == EMode::Frozen || Mode == EMode::Stomp || Mode == EMode::Victory)
	{
		return;
	}

	// ---- events
	if (Mode == EMode::Air && bJumpEvent)
	{
		// Our jump is instant, so skip the crouch anticipation at the start of the clip.
		if (In.bLastJumpFromRun)
		{
			PlayOneShot(EPenguinClip::JumpStartRun, 0.04f, 1.35f, 2.f);
		}
		else
		{
			PlayOneShot(EPenguinClip::JumpStart, 0.04f, 1.35f, 3.f);
		}
		bApexPending = true;
	}
	if (Mode == EMode::Air && bAirJumpEvent)
	{
		PlayOneShot(EPenguinClip::DoubleJump, 0.04f, 1.35f, 1.f);
		bApexPending = false; // the flip already covers the apex
	}
	if (Mode == EMode::Ground && bLandEvent && LastMode != EMode::Stomp
		&& CurrentClip != EPenguinClip::DashEnd && CurrentClip != EPenguinClip::SlideEnd && CurrentClip != EPenguinClip::Hit)
	{
		const bool bWantsToMove = FMath::Abs(In.InputX) > 0.1f || In.SpeedX > 200.f;
		if (!bWantsToMove)
		{
			if (In.LastLandSpeed > 2100.f)
			{
				PlayOneShot(EPenguinClip::LandHard, 0.04f, 1.25f, 1.f);
			}
			else
			{
				PlayOneShot(EPenguinClip::Land, 0.04f, 1.3f, 2.f);
			}
		}
		else if (CurrentClip != EPenguinClip::Hit)
		{
			bOneShotActive = false;
		}
	}

	// ---- apex: from rising to falling after a jump
	if (Mode == EMode::Air && bApexPending && In.VelocityZ < 260.f)
	{
		bApexPending = false;
		if (!(bOneShotActive && (CurrentClip == EPenguinClip::DoubleJump || CurrentClip == EPenguinClip::Hit)))
		{
			PlayOneShot(EPenguinClip::JumpApex, 0.07f, 1.2f);
		}
	}

	// ---- skid / stop on the ground
	if (Mode == EMode::Ground && In.bGrounded && !In.bOnIce)
	{
		const bool bInterruptible = !bOneShotActive || IsFidget(CurrentClip) || CurrentClip == EPenguinClip::Stop
			|| CurrentClip == EPenguinClip::Land || CurrentClip == EPenguinClip::DashEnd;
		if (FMath::Abs(In.InputX) > 0.1f)
		{
			RecentRunSpeed = FMath::Max(RecentRunSpeed, In.SpeedX);
		}
		else
		{
			RecentRunSpeed = FMath::Max(0.f, RecentRunSpeed - Dt * 900.f);
		}
		if (bInterruptible && CurrentClip != EPenguinClip::Skid && In.SpeedX > 300.f && FMath::Abs(In.InputX) > 0.5f && In.InputX * In.VelocityX < 0.f)
		{
			PlayOneShot(EPenguinClip::Skid, 0.05f, 1.9f, 1.5f);
		}
		else if (bInterruptible && IsLocomotionTop() && FMath::Abs(In.InputX) < 0.1f && RecentRunSpeed > 380.f && In.SpeedX < RecentRunSpeed * 0.85f)
		{
			if (PlayOneShot(EPenguinClip::Stop, 0.08f, 1.5f))
			{
				RecentRunSpeed = 0.f;
			}
		}
	}
	else
	{
		RecentRunSpeed = 0.f;
	}

	// ---- idle fidgets
	if (Mode == EMode::Ground && IsLocomotionTop() && LocoSpeed < 15.f && FMath::Abs(In.InputX) < 0.1f && !bActionPlaying && !bLobby)
	{
		IdleTimer += Dt;
		if (IdleTimer > NextFidget)
		{
			static const EPenguinClip Fidgets[] = { EPenguinClip::IdleLook, EPenguinClip::IdleFlap, EPenguinClip::IdleTap };
			PlayOneShot(Fidgets[Rand.RandRange(0, 2)], 0.25f, 1.f);
			IdleTimer = 0.f;
			NextFidget = Rand.FRandRange(5.f, 9.f);
		}
	}
	else if (!IsFidget(CurrentClip))
	{
		IdleTimer = 0.f;
	}

	// ---- one-shots run to their end unless the state they belong to is left
	if (bOneShotActive)
	{
		if (!IsOneShotStillValid(Mode) || IsTopLayerFinished())
		{
			bOneShotActive = false;
		}
		else
		{
			return;
		}
	}

	// ---- looping clip for the current mode
	EPenguinClip Loop = EPenguinClip::Idle;
	bool bLocomotion = false;
	float Rate = 1.f;
	float Blend = 0.12f;
	switch (Mode)
	{
	case EMode::Dash:
		Loop = EPenguinClip::DashLoop;
		Rate = 1.6f;
		Blend = 0.05f;
		break;
	case EMode::Slide:
		Loop = EPenguinClip::Slide;
		Rate = FMath::GetMappedRangeValueClamped(FVector2f(250.f, 1200.f), FVector2f(0.6f, 1.4f), In.SpeedX);
		Blend = 0.08f;
		break;
	case EMode::StunAir:
		Loop = In.LastHitSpeed > 1100.f ? EPenguinClip::Tumble : EPenguinClip::FallFlail;
		Rate = 1.3f;
		Blend = 0.1f;
		break;
	case EMode::Air:
		if ((In.bFallingToSea && In.VelocityZ < -300.f) || (In.AirTime > 1.1f && In.VelocityZ < -1000.f))
		{
			Loop = EPenguinClip::FallFlail;
			Blend = 0.18f;
		}
		else
		{
			Loop = In.VelocityZ > 260.f ? EPenguinClip::JumpLoop : EPenguinClip::FallLoop;
			Blend = CurrentClip == EPenguinClip::JumpApex ? 0.12f : 0.1f;
		}
		Rate = 1.2f;
		break;
	case EMode::Ground:
	default:
		if (In.bOnIce && (In.SpeedX > 40.f || FMath::Abs(In.InputX) > 0.1f) && GetClip(EPenguinClip::WalkIce))
		{
			Loop = EPenguinClip::WalkIce;
			Rate = FMath::GetMappedRangeValueClamped(FVector2f(0.f, 560.f), FVector2f(0.9f, 2.f), In.SpeedX);
		}
		else
		{
			bLocomotion = true;
		}
		if (CurrentClip == EPenguinClip::Land || CurrentClip == EPenguinClip::LandHard || CurrentClip == EPenguinClip::DashEnd
			|| CurrentClip == EPenguinClip::SlideEnd || CurrentClip == EPenguinClip::Skid)
		{
			Blend = 0.1f;
		}
		else if (CurrentClip == EPenguinClip::FallLoop || CurrentClip == EPenguinClip::JumpLoop || CurrentClip == EPenguinClip::JumpApex
			|| CurrentClip == EPenguinClip::DoubleJump || CurrentClip == EPenguinClip::JumpStart || CurrentClip == EPenguinClip::JumpStartRun
			|| CurrentClip == EPenguinClip::FallFlail)
		{
			Blend = 0.06f; // landing straight into a run
		}
		else if (CurrentClip == EPenguinClip::Stop || IsFidget(CurrentClip) || CurrentClip == EPenguinClip::Victory)
		{
			Blend = 0.22f;
		}
		break;
	}

	if (bLocomotion)
	{
		if (!IsLocomotionTop())
		{
			PlayLocomotion(Blend);
		}
	}
	else if (Loop != CurrentClip || IsLocomotionTop())
	{
		Play(Loop, Blend, Rate);
	}
	else if (Layers.Num() > 0)
	{
		FPenguinAnimLayer& Top = Layers.Last();
		Top.Rate = FMath::FInterpTo(Top.Rate, Rate, Dt, 8.f);
	}
}

void FPenguinAnimInstanceProxy::UpdateAction(float Dt, EMode Mode)
{
	if (In.ActionCount != SeenAction)
	{
		SeenAction = In.ActionCount;
		const UAnimSequence* Sequence = In.ActionClip != EPenguinClip::Count ? GetClip(In.ActionClip) : nullptr;
		if (Sequence)
		{
			ActionSequence = Sequence;
			ActionLength = Sequence->GetPlayLength();
			ActionTime = FMath::Min(1.f / PenguinAnimPrivate::Fps, ActionLength);
			ActionRate = FMath::Max(In.ActionRate, 0.1f);
			bActionPlaying = true;
			IdleTimer = 0.f;
		}
	}
	const bool bBlocked = Mode == EMode::KO || Mode == EMode::Frozen || Mode == EMode::Stomp || Mode == EMode::Dash || Mode == EMode::Slide;
	if (bActionPlaying)
	{
		ActionTime += Dt * ActionRate;
		if (ActionTime >= ActionLength || bBlocked)
		{
			ActionTime = FMath::Min(ActionTime, ActionLength);
			bActionPlaying = false;
		}
	}
	if (bActionPlaying)
	{
		ActionWeight = FMath::Min(1.f, ActionWeight + Dt / 0.05f);
	}
	else
	{
		ActionWeight = FMath::Max(0.f, ActionWeight - Dt / (bBlocked ? 0.06f : 0.14f));
	}
}

void FPenguinAnimInstanceProxy::AdvanceLayers(float Dt)
{
	if (Layers.Num() == 0)
	{
		return;
	}
	for (FPenguinAnimLayer& Layer : Layers)
	{
		if (Layer.bLocomotion)
		{
			continue;
		}
		Layer.Time += Dt * Layer.Rate;
		if (Layer.Length > UE_KINDA_SMALL_NUMBER)
		{
			if (Layer.bLoop)
			{
				Layer.Time = FMath::Fmod(Layer.Time, Layer.Length);
				if (Layer.Time < 0.f)
				{
					Layer.Time += Layer.Length;
				}
			}
			else
			{
				Layer.Time = FMath::Min(Layer.Time, Layer.Length);
			}
		}
		else
		{
			Layer.Time = 0.f;
		}
	}

	// The newest layer fades in; the older ones share what is left, keeping their ratios.
	FPenguinAnimLayer& Top = Layers.Last();
	Top.Weight = FMath::Min(1.f, Top.Weight + Dt * Top.FadeInSpeed);
	const float Remaining = 1.f - Top.Weight;
	float OlderTotal = 0.f;
	for (int32 Index = 0; Index < Layers.Num() - 1; ++Index)
	{
		OlderTotal += Layers[Index].Weight;
	}
	for (int32 Index = 0; Index < Layers.Num() - 1; ++Index)
	{
		Layers[Index].Weight = OlderTotal > UE_KINDA_SMALL_NUMBER ? Layers[Index].Weight * Remaining / OlderTotal : 0.f;
	}
	for (int32 Index = Layers.Num() - 2; Index >= 0; --Index)
	{
		if (Layers[Index].Weight < 0.002f)
		{
			Layers.RemoveAt(Index);
		}
	}
}

// ---------------------------------------------------------------------------- evaluation

void FPenguinAnimInstanceProxy::EvaluateClip(const UAnimSequence* Sequence, float Time, bool bLoop, FPoseContext& Pose) const
{
	if (Sequence && Sequence->GetSkeleton())
	{
		FAnimationPoseData PoseData(Pose);
		Sequence->GetAnimationPose(PoseData, FAnimExtractContext(static_cast<double>(Time), false, FDeltaTimeRecord(), bLoop));
	}
	else
	{
		Pose.ResetToRefPose();
	}
}

void FPenguinAnimInstanceProxy::EvaluateLocomotion(FPoseContext& Pose) const
{
	using namespace PenguinAnimPrivate;
	if (MoveAlpha < 0.999f)
	{
		const UAnimSequence* Idle = GetClip(EPenguinClip::Idle);
		const float Length = Idle ? FMath::Max(Idle->GetPlayLength(), 0.01f) : 1.f;
		EvaluateClip(Idle, FMath::Fmod(IdleTime, Length), true, Pose);
	}
	if (MoveAlpha > 0.001f)
	{
		FPoseContext Gait(Pose);
		const UAnimSequence* A = GetClip(GaitA);
		const float LengthA = A ? A->GetPlayLength() : 0.f;
		EvaluateClip(A, LocoPhase * LengthA, true, Gait);
		if (GaitBlend > 0.001f && GaitB != GaitA)
		{
			FPoseContext Second(Pose);
			const UAnimSequence* B = GetClip(GaitB);
			const float LengthB = B ? B->GetPlayLength() : 0.f;
			EvaluateClip(B, LocoPhase * LengthB, true, Second);
			BlendInto(Gait, Second, GaitBlend);
		}
		if (MoveAlpha >= 0.999f)
		{
			Pose.Pose = Gait.Pose;
		}
		else
		{
			BlendInto(Pose, Gait, MoveAlpha);
		}
	}
}

void FPenguinAnimInstanceProxy::EvaluateLayer(const FPenguinAnimLayer& Layer, FPoseContext& Pose) const
{
	if (Layer.bLocomotion)
	{
		EvaluateLocomotion(Pose);
	}
	else
	{
		EvaluateClip(Layer.Sequence, Layer.Time, Layer.bLoop, Pose);
	}
}

void FPenguinAnimInstanceProxy::CachePenguinBones(const FBoneContainer& Bones)
{
	bBonesCached = true;
	const FReferenceSkeleton& RefSkeleton = Bones.GetReferenceSkeleton();
	BoneKinds.Init(EBoneKind::Other, RefSkeleton.GetNum());
	for (int32 Index = 0; Index < RefSkeleton.GetNum(); ++Index)
	{
		const FString Name = RefSkeleton.GetBoneName(Index).ToString();
		EBoneKind Kind = EBoneKind::Other;
		if (Name == TEXT("Root"))
		{
			Kind = EBoneKind::Root;
			MeshRoot = Index;
		}
		else if (Name == TEXT("Body"))
		{
			Kind = EBoneKind::Body;
			MeshBody = Index;
		}
		else if (Name.StartsWith(TEXT("Flipper")) || Name.StartsWith(TEXT("Hand")))
		{
			Kind = EBoneKind::Flipper;
		}
		else if (Name == TEXT("Tail"))
		{
			Kind = EBoneKind::Tail;
		}
		else if (Name.StartsWith(TEXT("Foot")))
		{
			Kind = EBoneKind::Foot;
		}
		else if (Name == TEXT("Beak"))
		{
			Kind = EBoneKind::Beak; // (its own bone on Body since the fat-beak fix: it keeps its shape, see ApplyBodyShape)
		}
		else if (Name == TEXT("Eye_L"))
		{
			Kind = EBoneKind::Eye;
			MeshEye[0] = Index;
		}
		else if (Name == TEXT("Eye_R"))
		{
			Kind = EBoneKind::Eye;
			MeshEye[1] = Index;
		}
		else if (Name.StartsWith(TEXT("Eye")))
		{
			Kind = EBoneKind::Eye;
		}
		BoneKinds[Index] = Kind;
	}
	// What ApplyBodyShape keeps in shape: the bones on Body and the feet.
	MeshBodyChildren.Reset();
	MeshFeet.Reset();
	for (int32 Index = 0; Index < RefSkeleton.GetNum(); ++Index)
	{
		if (MeshBody != INDEX_NONE && RefSkeleton.GetParentIndex(Index) == MeshBody)
		{
			MeshBodyChildren.Add(Index);
		}
		else if (BoneKinds[Index] == EBoneKind::Foot && RefSkeleton.GetParentIndex(Index) != INDEX_NONE && BoneKinds[RefSkeleton.GetParentIndex(Index)] != EBoneKind::Foot)
		{
			MeshFeet.Add(Index);
		}
	}

	// Reference pose in component space: the penguin looks along +Y (mesh space).
	TArray<FTransform> ComponentSpace;
	const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();
	ComponentSpace.SetNum(RefPose.Num());
	for (int32 Index = 0; Index < RefPose.Num(); ++Index)
	{
		const int32 Parent = RefSkeleton.GetParentIndex(Index);
		ComponentSpace[Index] = Parent == INDEX_NONE ? RefPose[Index] : RefPose[Index] * ComponentSpace[Parent];
	}
	const FVector Forward(0.f, 1.f, 0.f);
	const FVector Up(0.f, 0.f, 1.f);
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		EyeRestLook[Eye] = Forward;
		if (ComponentSpace.IsValidIndex(MeshEye[Eye]))
		{
			const FQuat Rotation = ComponentSpace[MeshEye[Eye]].GetRotation();
			EyeRestLook[Eye] = Rotation.UnrotateVector(Forward);
			const FVector LocalUp = Rotation.UnrotateVector(Up).GetAbs();
			EyeBlinkAxis[Eye] = LocalUp.X >= LocalUp.Y && LocalUp.X >= LocalUp.Z ? 0 : (LocalUp.Y >= LocalUp.Z ? 1 : 2);
		}
	}
	if (ComponentSpace.IsValidIndex(MeshBody))
	{
		const float RootZ = ComponentSpace.IsValidIndex(MeshRoot) ? static_cast<float>(ComponentSpace[MeshRoot].GetLocation().Z) : 0.f;
		BodyRestHeight = FMath::Max(static_cast<float>(ComponentSpace[MeshBody].GetLocation().Z) - RootZ, 1.f);
		BodyRestForward = ComponentSpace[MeshBody].GetRotation().UnrotateVector(Forward);
		BodyRestUp = ComponentSpace[MeshBody].GetRotation().UnrotateVector(Up);
		BodyRestRotation = ComponentSpace[MeshBody].GetRotation();
	}
	// The flippers for the icicle throw: their rest rotation, the way they hang (shoulder to hand) and their side.
	static const TCHAR* const FlipperNames[2] = { TEXT("Flipper_L"), TEXT("Flipper_R") };
	static const TCHAR* const HandNames[2] = { TEXT("Hand_L"), TEXT("Hand_R") };
	for (int32 Side = 0; Side < 2; ++Side)
	{
		MeshFlipper[Side] = RefSkeleton.FindBoneIndex(FName(FlipperNames[Side]));
		const int32 MeshHand = RefSkeleton.FindBoneIndex(FName(HandNames[Side]));
		if (!ComponentSpace.IsValidIndex(MeshFlipper[Side]))
		{
			MeshFlipper[Side] = INDEX_NONE;
			continue;
		}
		const FTransform& Shoulder = ComponentSpace[MeshFlipper[Side]];
		FlipperRestRotation[Side] = Shoulder.GetRotation();
		if (ComponentSpace.IsValidIndex(MeshHand))
		{
			const FVector Hang = (ComponentSpace[MeshHand].GetLocation() - Shoulder.GetLocation()).GetSafeNormal();
			FlipperRestDir[Side] = Hang.IsNearlyZero() ? FVector(0.f, 0.f, -1.f) : Hang;
		}
		const float BodyX = ComponentSpace.IsValidIndex(MeshBody) ? static_cast<float>(ComponentSpace[MeshBody].GetLocation().X) : 0.f;
		const float Offset = static_cast<float>(Shoulder.GetLocation().X) - BodyX;
		FlipperSide[Side] = FMath::Abs(Offset) > 0.01f ? FMath::Sign(Offset) : (Side == 0 ? 1.f : -1.f);
	}
}

float FPenguinAnimInstanceProxy::ActionMask(EBoneKind Kind) const
{
	const bool bStill = In.bGrounded && LocoSpeed < 60.f && CurrentMode == EMode::Ground;
	switch (Kind)
	{
	case EBoneKind::Flipper: return 1.f;
	case EBoneKind::Body: return bStill ? 1.f : 0.65f;
	case EBoneKind::Tail: return 0.6f;
	case EBoneKind::Root:
	case EBoneKind::Foot: return bStill ? 1.f : 0.f;
	case EBoneKind::Eye:
	case EBoneKind::Beak: return 0.f;
	default: return bStill ? 1.f : 0.5f;
	}
}

void FPenguinAnimInstanceProxy::ApplyAction(FPoseContext& Output)
{
	if (ActionWeight <= 0.001f || !ActionSequence)
	{
		return;
	}
	FPoseContext Action(Output);
	EvaluateClip(ActionSequence, ActionTime, false, Action);
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	for (const FCompactPoseBoneIndex BoneIndex : Output.Pose.ForEachBoneIndex())
	{
		const int32 MeshIndex = Bones.MakeMeshPoseIndex(BoneIndex).GetInt();
		const EBoneKind Kind = BoneKinds.IsValidIndex(MeshIndex) ? BoneKinds[MeshIndex] : EBoneKind::Other;
		const float Alpha = ActionWeight * ActionMask(Kind);
		if (Alpha > 0.001f)
		{
			Output.Pose[BoneIndex].BlendWith(Action.Pose[BoneIndex], Alpha);
		}
	}
}

void FPenguinAnimInstanceProxy::ApplyHonkAim(FPoseContext& Output)
{
	if (In.HonkWeight <= 0.001f || MeshBody == INDEX_NONE)
	{
		return;
	}
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex BodyIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBody));
	if (!BodyIndex.IsValid())
	{
		return;
	}
	FCSPose<FCompactPose> CS;
	CS.InitPose(Output.Pose);
	const FQuat BodyRotation = CS.GetComponentSpaceTransform(BodyIndex).GetRotation();
	// Where the beak points now (mesh: +Y forward, +Z up; whatever the clip does), and where the noot wants it: the turn
	// between them about the penguin's side axis only (the clip's roll and buzz stay).
	const FVector Beak = BodyRotation.RotateVector(BodyRestForward);
	const FVector Now = FVector(0.f, Beak.Y, Beak.Z).GetSafeNormal();
	if (Now.IsNearlyZero())
	{
		return;
	}
	const float Pitch = FMath::DegreesToRadians(In.HonkPitch);
	const FVector Want(0.f, FMath::Cos(Pitch), FMath::Sin(Pitch));
	const FQuat Turn = FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(Now, Want), FMath::Clamp(In.HonkWeight, 0.f, 1.f));
	const FCompactPoseBoneIndex Parent = Output.Pose.GetParentBoneIndex(BodyIndex);
	const FQuat ParentRotation = Parent.IsValid() ? CS.GetComponentSpaceTransform(Parent).GetRotation() : FQuat::Identity;
	Output.Pose[BodyIndex].SetRotation((ParentRotation.Inverse() * (Turn * BodyRotation)).GetNormalized());
}

void FPenguinAnimInstanceProxy::UpdateIcicleThrow(float Dt)
{
	using namespace PenguinAnimPrivate;
	const auto ArmFrom = [](const float (&Key)[3])
	{
		FThrowArm Arm;
		Arm.Flare = Key[0];
		Arm.Swing = Key[1];
		Arm.Lean = Key[2];
		return Arm;
	};
	const auto MixArms = [](const FThrowArm& A, const FThrowArm& B, float Alpha)
	{
		FThrowArm Arm;
		Arm.Flare = FMath::Lerp(A.Flare, B.Flare, Alpha);
		Arm.Swing = FMath::Lerp(A.Swing, B.Swing, Alpha);
		Arm.Lean = FMath::Lerp(A.Lean, B.Lean, Alpha);
		return Arm;
	};
	// Where the charge has the arm: grabbing behind the back, cocking over the head, drawn back as it charges.
	const auto ChargedArm = [&](float ChargeTime)
	{
		constexpr float PullOut = UPenguinAbilityComponent::IciclePullOutTime;
		if (ChargeTime < ThrowGrabTime)
		{
			return ArmFrom(ThrowGrab);
		}
		if (ChargeTime < PullOut)
		{
			return MixArms(ArmFrom(ThrowGrab), ArmFrom(ThrowCocked), FMath::SmoothStep(0.f, 1.f, (ChargeTime - ThrowGrabTime) / (PullOut - ThrowGrabTime)));
		}
		const float Draw = FMath::Clamp((ChargeTime - PullOut) / (UPenguinAbilityComponent::IcicleFullChargeTime - PullOut), 0.f, 1.f);
		return MixArms(ArmFrom(ThrowCocked), ArmFrom(ThrowDrawn), FMath::SmoothStep(0.f, 1.f, Draw));
	};
	const EPinguIcicleShow Stage = static_cast<EPinguIcicleShow>(ThrowStage);
	const bool bActive = Stage != EPinguIcicleShow::None && CurrentMode != EMode::KO && CurrentMode != EMode::Frozen;
	if (bActive)
	{
		if (Stage == EPinguIcicleShow::Charging)
		{
			ThrowPose = ChargedArm(ThrowChargeTime);
			// The arm trembles a little, more and more as the charge builds (the icicle in the hand shakes with it: the same
			// build, UPenguinAbilityComponent::GetChargeShakeBuild); the most at full charge.
			const float Tremble = ThrowChargeTime > UPenguinAbilityComponent::IciclePullOutTime ? UPenguinAbilityComponent::GetChargeShakeBuild(ThrowChargeTime) : 0.f;
			ThrowPose.Flare += Tremble * ThrowTrembleFlare * FMath::Sin(TotalTime * 2.f * UE_PI * ThrowTrembleRate);
			ThrowPose.Swing += Tremble * ThrowTrembleSwing * FMath::Sin(TotalTime * 2.f * UE_PI * ThrowTrembleRate * 1.21f + 1.f);
		}
		else
		{
			// Thrown: from where the charge had it, over the head to the front (the icicle leaves at the end of the swing)
			// and through.
			constexpr float SwingTime = UPenguinAbilityComponent::IcicleSwingTime;
			const float S = ThrowStageTime;
			ThrowPose = S < SwingTime
				? MixArms(ChargedArm(ThrowChargeTime), ArmFrom(ThrowRelease), FMath::SmoothStep(0.f, 1.f, S / SwingTime))
				: MixArms(ArmFrom(ThrowRelease), ArmFrom(ThrowFollow), FMath::SmoothStep(0.f, 1.f, (S - SwingTime) / UPenguinAbilityComponent::IcicleFollowTime));
		}
		// The flipper the camera sees throws (the icicle is in its hand): turned right that is Flipper_L. The penguin faces its aim
		// while it charges (APenguinCharacter::GetVisualFacing: not the movement's facing). If the aim crosses over, the arms swap
		// roles over ThrowSwapTime (the icicle in the hand swaps at the same pace: UPenguinAbilityComponent::UpdateHandIcicle).
		const float WantLeft = In.VisualFacing > 0.f ? 1.f : 0.f;
		ThrowLeftBlend = ThrowWeight <= 0.001f ? WantLeft : FMath::FInterpConstantTo(ThrowLeftBlend, WantLeft, Dt, 1.f / ThrowSwapTime);
		ThrowWeight = FMath::Min(1.f, ThrowWeight + Dt / ThrowFadeIn);
	}
	else
	{
		ThrowWeight = FMath::Max(0.f, ThrowWeight - Dt / ThrowFadeOut);
	}
}

void FPenguinAnimInstanceProxy::ApplyIcicleThrow(FPoseContext& Output)
{
	using namespace PenguinAnimPrivate;
	if (ThrowWeight <= 0.001f || MeshBody == INDEX_NONE)
	{
		return;
	}
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex BodyIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBody));
	if (!BodyIndex.IsValid())
	{
		return;
	}
	const float W = FMath::Clamp(ThrowWeight, 0.f, 1.f);
	// Mesh space: the penguin looks along +Y, up is +Z, its sides along X.
	const FVector Up(0.f, 0.f, 1.f);
	const FVector Forward(0.f, 1.f, 0.f);
	const FVector SwingAxis = FVector::CrossProduct(-Up, Forward).GetSafeNormal(); // turns a hanging flipper forward
	// A flipper posed the clips' way (Flare / Swing from its rest, as in Tools/penguin_anims.py), in the body's frame.
	const auto PoseFlipper = [&](int32 Side, float Flare, float Swing, float Alpha)
	{
		const int32 MeshIndex = MeshFlipper[Side];
		const FCompactPoseBoneIndex Index = MeshIndex != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex)) : FCompactPoseBoneIndex(INDEX_NONE);
		if (!Index.IsValid() || Alpha <= 0.001f)
		{
			return;
		}
		const FVector Out(FlipperSide[Side], 0.f, 0.f);
		const FVector FlareAxis = FVector::CrossProduct(-Up, Out).GetSafeNormal(); // lifts a hanging flipper out to its side
		const FQuat Turn = FQuat(FlareAxis, FMath::DegreesToRadians(Flare)) * FQuat(SwingAxis, FMath::DegreesToRadians(Swing));
		// Rotated about its shoulder in the body's rest frame; as a local rotation under the body that holds however the body moves.
		const FQuat Local = (BodyRestRotation.Inverse() * Turn * FlipperRestRotation[Side]).GetNormalized();
		FTransform& Bone = Output.Pose[Index];
		Bone.SetRotation(FQuat::Slerp(Bone.GetRotation(), Local, Alpha).GetNormalized());
	};
	// The near flipper throws, the other is out for balance; while the penguin turns round the two blend (a flipper's pose is
	// the throw's by its share, so the arm never jumps from one side to the other).
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float Throws = FMath::Clamp(Side == 0 ? ThrowLeftBlend : 1.f - ThrowLeftBlend, 0.f, 1.f);
		PoseFlipper(Side, FMath::Lerp(ThrowOtherFlare, ThrowPose.Flare, Throws), FMath::Lerp(ThrowOtherSwing, ThrowPose.Swing, Throws),
			W * FMath::Lerp(0.7f, 1.f, Throws));
	}

	// The body leans back as it draws and into the throw (about its base: the feet stay).
	if (FMath::Abs(ThrowPose.Lean) > 0.01f)
	{
		FCSPose<FCompactPose> CS;
		CS.InitPose(Output.Pose);
		const FQuat BodyRotation = CS.GetComponentSpaceTransform(BodyIndex).GetRotation();
		// Turning about SwingAxis tips the top backward: forward is the other way.
		const FQuat Lean(SwingAxis, FMath::DegreesToRadians(-ThrowPose.Lean * W));
		const FCompactPoseBoneIndex Parent = Output.Pose.GetParentBoneIndex(BodyIndex);
		const FQuat ParentRotation = Parent.IsValid() ? CS.GetComponentSpaceTransform(Parent).GetRotation() : FQuat::Identity;
		Output.Pose[BodyIndex].SetRotation((ParentRotation.Inverse() * (Lean * BodyRotation)).GetNormalized());
	}
}

void FPenguinAnimInstanceProxy::ApplyLookAt(FPoseContext& Output)
{
	if (!In.bHasLookTarget)
	{
		return;
	}
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex BodyIndex = MeshBody != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBody)) : FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex EyeIndex[2] = { FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE) };
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		EyeIndex[Eye] = MeshEye[Eye] != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshEye[Eye])) : FCompactPoseBoneIndex(INDEX_NONE);
	}
	const FVector Target = In.ComponentToWorld.InverseTransformPosition(In.LookTarget);
	const float Dt = LastDt;

	// ---- only the eyes follow the cursor: the body faces where the penguin moves (no lean toward the aim)
	AttentionYaw = FMath::FInterpTo(AttentionYaw, 0.f, Dt, 8.f);
	AttentionPitch = FMath::FInterpTo(AttentionPitch, 0.f, Dt, 8.f);

	// ---- eyes
	if (!EyeIndex[0].IsValid() && !EyeIndex[1].IsValid())
	{
		return;
	}
	FCSPose<FCompactPose> CS;
	CS.InitPose(Output.Pose);
	constexpr float MaxAngle = 48.f;
	for (int32 Eye = 0; Eye < 2; ++Eye)
	{
		if (!EyeIndex[Eye].IsValid())
		{
			continue;
		}
		const FTransform EyeCS = CS.GetComponentSpaceTransform(EyeIndex[Eye]);
		const FCompactPoseBoneIndex Parent = Output.Pose.GetParentBoneIndex(EyeIndex[Eye]);
		const FQuat ParentRotation = Parent.IsValid() ? CS.GetComponentSpaceTransform(Parent).GetRotation() : FQuat::Identity;
		const FVector HeadForward = BodyIndex.IsValid()
			? CS.GetComponentSpaceTransform(BodyIndex).GetRotation().RotateVector(BodyRestForward).GetSafeNormal()
			: FVector(0.f, 1.f, 0.f);

		FVector Desired;
		if (CurrentMode == EMode::KO || In.bDizzy)
		{
			// Dizzy (knocked out, or a long stun - the slap's): the eyes roll around.
			const float Angle = TotalTime * 13.f + Eye * UE_PI;
			const FVector Side = FVector::CrossProduct(HeadForward, FVector::UpVector).GetSafeNormal();
			Desired = (HeadForward + (Side * FMath::Cos(Angle) + FVector::UpVector * FMath::Sin(Angle)) * 0.7f).GetSafeNormal();
		}
		else
		{
			Desired = (Target - EyeCS.GetLocation()).GetSafeNormal();
			const float Cos = FVector::DotProduct(Desired, HeadForward);
			const float Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cos, -1.f, 1.f)));
			if (Angle > MaxAngle)
			{
				// Keep the pupils on the face: turn from the head's forward toward the target by MaxAngle.
				const FVector Axis = FVector::CrossProduct(HeadForward, Desired).GetSafeNormal();
				Desired = Axis.IsNearlyZero() ? HeadForward : FQuat(Axis, FMath::DegreesToRadians(MaxAngle)).RotateVector(HeadForward);
			}
		}

		if (!bGazeInit)
		{
			SmoothedGaze[Eye] = Desired;
		}
		else if (CurrentMode != EMode::Frozen)
		{
			const float Alpha = 1.f - FMath::Exp(-26.f * Dt);
			SmoothedGaze[Eye] = FMath::Lerp(SmoothedGaze[Eye], Desired, Alpha).GetSafeNormal();
		}

		const FVector Gaze = EyeCS.GetRotation().RotateVector(EyeRestLook[Eye]).GetSafeNormal();
		const FQuat Delta = FQuat::FindBetweenNormals(Gaze, SmoothedGaze[Eye]);
		const FQuat NewEye = Delta * EyeCS.GetRotation();
		FTransform& Local = Output.Pose[EyeIndex[Eye]];
		Local.SetRotation((ParentRotation.Inverse() * NewEye).GetNormalized());

		// Blink: squash the eye on its vertical axis.
		if (BlinkTime >= 0.f && CurrentMode != EMode::KO && CurrentMode != EMode::Frozen)
		{
			const float T = FMath::Clamp(BlinkTime / 0.15f, 0.f, 1.f);
			const float Squeeze = 1.f - 0.88f * FMath::Sin(T * UE_PI);
			FVector Scale = Local.GetScale3D();
			Scale[EyeBlinkAxis[Eye]] *= Squeeze;
			Local.SetScale3D(Scale);
		}
	}
	bGazeInit = true;
}

// ---------------------------------------------------------------------------- the lobby island

namespace PenguinAnimPrivate
{
	/** The poke (a click on the penguin): a hop with a full turn, this long, this high (cm). */
	constexpr float LobbyPokeTime = 0.42f;
	constexpr float LobbyPokeHop = 38.f;
	/** Sitting: the body this much lower (share of its height) and leaning back (degrees). */
	constexpr float LobbySitDrop = 0.32f;
	constexpr float LobbySitLean = -12.f;
	/** Sitting turns this far from its side toward the camera (degrees; 90: straight at it). */
	constexpr float LobbySitTurn = 50.f;
	/** How fast it turns (1/s) and how fast the lobby pose comes in (s). */
	constexpr float LobbyTurnRate = 9.f;
	constexpr float LobbyPoseFade = 0.3f;

	/** A gag's weight at Age of Length: in over 0.25 s, out over the last 0.3 s. */
	static float GagWeight(float Age, float Length)
	{
		if (Age < 0.f || Length <= 0.f)
		{
			return 0.f;
		}
		return FMath::SmoothStep(0.f, 1.f, FMath::Min(Age / 0.25f, (Length - Age) / 0.3f));
	}

	/** A short bump 0 -> 1 -> 0 over Length from Age 0 (sin of half a turn). */
	static float Bump(float Age, float Length)
	{
		return Age >= 0.f && Age < Length ? FMath::Sin(Age / Length * UE_PI) : 0.f;
	}
}

void FPenguinAnimInstanceProxy::UpdateLobby(float Dt, EMode Mode)
{
	using namespace PenguinAnimPrivate;
	using PinguLobby::EGag;
	if (!bLobby)
	{
		LobbyPoseWeight = 0.f;
		LobbyYaw = 0.f;
		bLobbyYawInit = false;
		return;
	}
	LobbyPoseWeight = FMath::Min(1.f, LobbyPoseWeight + Dt / LobbyPoseFade);
	// Where it looks (world yaw: 0 = +X, 90 = the camera): a seat three quarters to the camera from its side, a hang straight
	// at it; a gag turns it to the camera or to the neighbour it noots at.
	const PinguLobby::EPose Pose = static_cast<PinguLobby::EPose>(LobbyPose);
	const EGag Gag = static_cast<EGag>(LobbyGag);
	float Want = LobbyFacingYaw;
	if (Pose == PinguLobby::EPose::Sit || Pose == PinguLobby::EPose::Stand)
	{
		Want = LobbySide > 0.f ? LobbySitTurn : 180.f - LobbySitTurn;
	}
	else if (Pose == PinguLobby::EPose::Hang)
	{
		Want = 90.f;
	}
	const float GW = GagWeight(LobbyGagAge, PinguLobby::GetGagLength(Gag));
	if (GW > 0.3f)
	{
		switch (Gag)
		{
		case EGag::NootCamera:
		case EGag::Wave:
		case EGag::Dance:
		case EGag::Shiver:
			Want = 90.f;
			break;
		case EGag::NootFriend:
			// Side on to the neighbour (a hanging one only half way: it holds on).
			Want = LobbyGagDir >= 0.f ? 0.f : 180.f;
			if (Pose == PinguLobby::EPose::Hang)
			{
				Want = LobbyGagDir >= 0.f ? 35.f : 145.f;
			}
			break;
		case EGag::Fish:
			Want = LobbySide > 0.f ? 70.f : 110.f;
			break;
		case EGag::Doze:
			if (LobbyGagAge > PinguLobby::DozeStartleTime + 0.35f)
			{
				// Awake again: looks round, who was that?
				Want += 30.f * FMath::Sin((LobbyGagAge - PinguLobby::DozeStartleTime) * 2.f * UE_PI * 1.2f);
			}
			break;
		default:
			break;
		}
	}
	const float Target = FMath::UnwindDegrees(Want - LobbyFacingYaw);
	if (!bLobbyYawInit)
	{
		bLobbyYawInit = true;
		LobbyYaw = Target;
	}
	LobbyYaw += FMath::UnwindDegrees(Target - LobbyYaw) * (1.f - FMath::Exp(-LobbyTurnRate * Dt));
}

void FPenguinAnimInstanceProxy::PoseFlipper(FPoseContext& Output, int32 Side, float Flare, float Swing, float Alpha) const
{
	const int32 MeshIndex = MeshFlipper[Side];
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex Index = MeshIndex != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex)) : FCompactPoseBoneIndex(INDEX_NONE);
	if (!Index.IsValid() || Alpha <= 0.001f)
	{
		return;
	}
	// As the icicle throw does it (ApplyIcicleThrow): about the shoulder in the body's rest frame, a local rotation under the body.
	const FVector Up(0.f, 0.f, 1.f);
	const FVector Forward(0.f, 1.f, 0.f);
	const FVector SwingAxis = FVector::CrossProduct(-Up, Forward).GetSafeNormal();
	const FVector Out(FlipperSide[Side], 0.f, 0.f);
	const FVector FlareAxis = FVector::CrossProduct(-Up, Out).GetSafeNormal();
	const FQuat Turn = FQuat(FlareAxis, FMath::DegreesToRadians(Flare)) * FQuat(SwingAxis, FMath::DegreesToRadians(Swing));
	const FQuat Local = (BodyRestRotation.Inverse() * Turn * FlipperRestRotation[Side]).GetNormalized();
	FTransform& Bone = Output.Pose[Index];
	Bone.SetRotation(FQuat::Slerp(Bone.GetRotation(), Local, FMath::Clamp(Alpha, 0.f, 1.f)).GetNormalized());
}

void FPenguinAnimInstanceProxy::ApplyFlap(FPoseContext& Output)
{
	if (FlapAlpha <= 0.01f)
	{
		return;
	}
	// About 7 beats a second: both flippers from low out at the sides (25 degrees) to high over the shoulders (135), the
	// down-stroke quicker than the up-stroke (a bird's power stroke), a little forward on the way down; the body bobs with it.
	constexpr float BeatsPerSecond = 7.f;
	const float Phase = FMath::Frac(FlapClock * BeatsPerSecond);
	const float Stroke = Phase < 0.4f ? 1.f - FMath::InterpEaseInOut(0.f, 1.f, Phase / 0.4f, 2.f) : FMath::InterpEaseInOut(0.f, 1.f, (Phase - 0.4f) / 0.6f, 2.f);
	const float Flare = FMath::Lerp(25.f, 135.f, Stroke);
	const float Swing = FMath::Lerp(25.f, 5.f, Stroke);
	PoseFlipper(Output, 0, Flare, Swing, FlapAlpha);
	PoseFlipper(Output, 1, Flare, Swing, FlapAlpha);
	PoseBody(Output, 6.f * (1.f - Stroke) * FlapAlpha, 0.02f * (1.f - Stroke) * FlapAlpha);
}

void FPenguinAnimInstanceProxy::PoseBody(FPoseContext& Output, float Lean, float Drop) const
{
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex BodyIndex = MeshBody != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBody)) : FCompactPoseBoneIndex(INDEX_NONE);
	if (!BodyIndex.IsValid())
	{
		return;
	}
	FCSPose<FCompactPose> CS;
	CS.InitPose(Output.Pose);
	const FVector Up(0.f, 0.f, 1.f);
	const FVector Forward(0.f, 1.f, 0.f);
	const FVector SwingAxis = FVector::CrossProduct(-Up, Forward).GetSafeNormal();
	const FQuat BodyRotation = CS.GetComponentSpaceTransform(BodyIndex).GetRotation();
	const FCompactPoseBoneIndex Parent = Output.Pose.GetParentBoneIndex(BodyIndex);
	const FQuat ParentRotation = Parent.IsValid() ? CS.GetComponentSpaceTransform(Parent).GetRotation() : FQuat::Identity;
	if (FMath::Abs(Lean) > 0.01f)
	{
		// Turning about SwingAxis tips the top backward: forward is the other way (ApplyIcicleThrow).
		const FQuat Tip(SwingAxis, FMath::DegreesToRadians(-Lean));
		Output.Pose[BodyIndex].SetRotation((ParentRotation.Inverse() * (Tip * BodyRotation)).GetNormalized());
	}
	if (Drop > 0.001f)
	{
		// Straight down in component space, as a move of the bone in its parent's space (the parent's rotation and scale undone).
		const FTransform ParentTransform = Parent.IsValid() ? CS.GetComponentSpaceTransform(Parent) : FTransform::Identity;
		const FVector Down = ParentTransform.InverseTransformVector(FVector(0.f, 0.f, -Drop * BodyRestHeight));
		Output.Pose[BodyIndex].AddToTranslation(Down);
	}
}

void FPenguinAnimInstanceProxy::ApplyLobbyPose(FPoseContext& Output)
{
	using namespace PenguinAnimPrivate;
	using PinguLobby::EGag;
	if (!bLobby || MeshBody == INDEX_NONE)
	{
		return;
	}
	const float PW = FMath::SmoothStep(0.f, 1.f, LobbyPoseWeight);
	const PinguLobby::EPose Pose = static_cast<PinguLobby::EPose>(LobbyPose);
	const bool bHang = Pose == PinguLobby::EPose::Hang;
	const EGag Gag = static_cast<EGag>(LobbyGag);
	const float T = LobbyGagAge;
	const float GW = GagWeight(T, PinguLobby::GetGagLength(Gag));
	// The flipper on its facing side (the one the camera sees from the side; Flipper_L when it faces right, as the icicle throw
	// has it) does the holding and waving.
	const int32 Near = In.VisualFacing > 0.f ? 0 : 1;
	const int32 Far = 1 - Near;

	// The waiting pose: sitting on its tail (lower, leaning back, flippers resting, breathing), or hanging by both flippers.
	float Lean = 0.f;
	float Drop = 0.f;
	if (Pose == PinguLobby::EPose::Sit)
	{
		Lean = LobbySitLean * PW;
		Drop = (LobbySitDrop + 0.012f * FMath::Sin(TotalTime * 2.f * UE_PI * 0.45f)) * PW;
	}
	else if (bHang)
	{
		Lean = 3.f * PW;
	}
	// What the gag does with the body.
	switch (Gag)
	{
	case EGag::Fish:
	{
		// Looks down at the fish, a little crouch to toss it, then head back with the beak up to catch it, a nod as it goes down.
		const float Toss = PinguLobby::FishTossTime;
		const float Catch = PinguLobby::FishGulpTime;
		const float Look = T < Toss - 0.25f ? 10.f * FMath::SmoothStep(0.2f, 0.6f, T) : 10.f + 8.f * FMath::SmoothStep(Toss - 0.25f, Toss, T);
		const float Back = -30.f * FMath::SmoothStep(Toss + 0.15f, Toss + 0.45f, T) * (1.f - FMath::SmoothStep(Catch + 0.05f, Catch + 0.35f, T));
		Lean += GW * (T < Toss + 0.15f ? Look * (1.f - FMath::SmoothStep(Toss, Toss + 0.15f, T)) : Back);
		Lean += GW * 10.f * Bump(T - Catch - 0.2f, 0.35f); // the gulp's nod
		break;
	}
	case EGag::NootFriend:
	case EGag::NootCamera:
		// A deep breath (leaning back), then the two blasts.
		Lean += GW * (T < PinguLobby::NootBlastTimes[0] ? -12.f * FMath::SmoothStep(0.f, 0.45f, T) : 4.f);
		break;
	case EGag::Wave:
		Lean += GW * -5.f;
		break;
	case EGag::Doze:
	{
		// Nods off (leaning forward, breathing slowly), startles awake.
		const float Sleep = FMath::SmoothStep(0.4f, 1.4f, T) * (1.f - FMath::SmoothStep(PinguLobby::DozeStartleTime, PinguLobby::DozeStartleTime + 0.12f, T));
		Lean += GW * Sleep * (26.f + 4.f * FMath::Sin(T * 2.f * UE_PI * 0.5f));
		Lean -= GW * 12.f * Bump(T - PinguLobby::DozeStartleTime, 0.45f);
		break;
	}
	case EGag::Shiver:
		Lean += GW * 6.f;
		break;
	default:
		break;
	}
	// A neighbour nooted at it: it jumps back from the blast.
	Lean -= 14.f * Bump(LobbyFlinchAge, 0.45f);
	if (FMath::Abs(Lean) > 0.01f || Drop > 0.001f)
	{
		PoseBody(Output, Lean, FMath::Max(Drop, 0.f));
	}

	// The flippers: the pose's, then the gag's over them.
	if (Pose == PinguLobby::EPose::Sit)
	{
		PoseFlipper(Output, 0, 10.f, 22.f, PW * 0.8f);
		PoseFlipper(Output, 1, 10.f, 22.f, PW * 0.8f);
	}
	else if (bHang)
	{
		PoseFlipper(Output, 0, 14.f, -172.f, PW);
		PoseFlipper(Output, 1, 14.f, -172.f, PW);
	}
	if (GW > 0.001f)
	{
		switch (Gag)
		{
		case EGag::Fish:
		{
			// Holds the fish up in front, dips it, flings it up (the flipper follows through high), both flippers out to catch;
			// then pats its round belly.
			const float Toss = PinguLobby::FishTossTime;
			const float Dip = FMath::SmoothStep(Toss - 0.22f, Toss, T);
			const float Fling = FMath::SmoothStep(Toss, Toss + 0.12f, T) * (1.f - FMath::SmoothStep(Toss + 0.45f, Toss + 0.8f, T));
			if (T < PinguLobby::FishGulpTime)
			{
				PoseFlipper(Output, Near, 25.f, FMath::Lerp(FMath::Lerp(65.f, 30.f, Dip), 160.f, Fling), GW);
				const float Ready = FMath::SmoothStep(Toss + 0.3f, Toss + 0.55f, T);
				PoseFlipper(Output, Far, 40.f * Ready, -20.f * Ready, GW * Ready);
			}
			if (T > PinguLobby::FatDelay)
			{
				const float Pat = FMath::SmoothStep(PinguLobby::FatDelay, PinguLobby::FatDelay + 0.3f, T) * GW;
				PoseFlipper(Output, Near, 12.f, 30.f + 10.f * FMath::Sin(T * 2.f * UE_PI * 3.f), Pat);
				if (!bHang)
				{
					PoseFlipper(Output, Far, 12.f, 30.f + 10.f * FMath::Sin(T * 2.f * UE_PI * 3.f + 1.f), Pat);
				}
			}
			break;
		}
		case EGag::NootFriend:
		case EGag::NootCamera:
			if (!bHang)
			{
				const float Out = T < PinguLobby::NootBlastTimes[0] ? FMath::SmoothStep(0.f, 0.4f, T) : 1.f;
				PoseFlipper(Output, 0, 50.f * Out, -10.f * Out, GW);
				PoseFlipper(Output, 1, 50.f * Out, -10.f * Out, GW);
			}
			break;
		case EGag::Wave:
			// The near flipper up and waving (hanging: it lets go with that one and holds on with the other).
			PoseFlipper(Output, Near, 32.f + 22.f * FMath::Sin(T * 2.f * UE_PI * 2.6f), -168.f, GW);
			if (!bHang)
			{
				PoseFlipper(Output, Far, 12.f, 6.f, GW * 0.6f);
			}
			break;
		case EGag::Doze:
			if (!bHang)
			{
				const float Fling = Bump(T - PinguLobby::DozeStartleTime, 0.5f);
				PoseFlipper(Output, 0, FMath::Lerp(4.f, 75.f, Fling), FMath::Lerp(12.f, 0.f, Fling), GW);
				PoseFlipper(Output, 1, FMath::Lerp(4.f, 75.f, Fling), FMath::Lerp(12.f, 0.f, Fling), GW);
			}
			break;
		case EGag::Dance:
		{
			const float Beat = FMath::Sin(T * 2.f * UE_PI * 1.6f);
			PoseFlipper(Output, 0, 50.f, -110.f + 60.f * Beat, GW);
			PoseFlipper(Output, 1, 50.f, -110.f - 60.f * Beat, GW);
			break;
		}
		case EGag::Shiver:
			if (!bHang)
			{
				// Hugging itself.
				PoseFlipper(Output, 0, 2.f, 42.f + 4.f * FMath::Sin(T * 2.f * UE_PI * 15.f), GW);
				PoseFlipper(Output, 1, 2.f, 42.f + 4.f * FMath::Sin(T * 2.f * UE_PI * 15.f + 2.f), GW);
			}
			break;
		default:
			break;
		}
	}
	// The poke's and the flinch's flippers flung out.
	const float Fling = FMath::Max(LobbyPokeAge >= 0.f ? Bump(LobbyPokeAge, LobbyPokeTime) : 0.f, Bump(LobbyFlinchAge, 0.45f));
	if (Fling > 0.001f)
	{
		PoseFlipper(Output, 0, 70.f, 0.f, Fling);
		PoseFlipper(Output, 1, 70.f, 0.f, Fling);
	}
}

void FPenguinAnimInstanceProxy::ApplyLobbyPlacement(FPoseContext& Output)
{
	using namespace PenguinAnimPrivate;
	using PinguLobby::EGag;
	if (!bLobby || MeshRoot == INDEX_NONE)
	{
		return;
	}
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex RootIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshRoot));
	if (!RootIndex.IsValid())
	{
		return;
	}
	const float PW = FMath::SmoothStep(0.f, 1.f, LobbyPoseWeight);
	const EGag Gag = static_cast<EGag>(LobbyGag);
	const float T = LobbyGagAge;
	const float GW = GagWeight(T, PinguLobby::GetGagLength(Gag));
	float Lift = 0.f; // cm
	float Roll = 0.f; // degrees about the view's axis (a sway as the camera sees it)
	float Spin = 0.f; // degrees about up
	if (static_cast<PinguLobby::EPose>(LobbyPose) == PinguLobby::EPose::Hang)
	{
		Roll += 2.5f * FMath::Sin(TotalTime * 2.f * UE_PI * 0.35f) * PW;
	}
	switch (Gag)
	{
	case EGag::Dance:
	{
		const float Beat = T * 2.f * UE_PI * 1.6f;
		Roll += 10.f * FMath::Sin(Beat) * GW;
		Lift += 6.f * FMath::Abs(FMath::Sin(Beat)) * GW;
		break;
	}
	case EGag::Shiver:
		Roll += 3.f * FMath::Sin(T * 2.f * UE_PI * 15.f) * GW;
		break;
	case EGag::Doze:
		Lift += 26.f * Bump(T - PinguLobby::DozeStartleTime, 0.4f);
		break;
	case EGag::Fish:
		Lift += 9.f * Bump(T - PinguLobby::FishGulpTime + 0.12f, 0.3f); // a little hop to catch it
		Lift += 6.f * Bump(T - PinguLobby::FatDelay, 0.35f); // boing
		Lift += 3.f * FMath::Abs(FMath::Sin((T - PinguLobby::FatDelay - 0.4f) * 2.f * UE_PI * 1.5f)) * FMath::SmoothStep(PinguLobby::FatDelay + 0.4f, PinguLobby::FatDelay + 0.6f, T) * GW; // happy bounce
		break;
	default:
		break;
	}
	Lift += 16.f * Bump(LobbyFlinchAge, 0.45f);
	Roll += 8.f * LobbyFlinchDir * Bump(LobbyFlinchAge, 0.45f);
	if (LobbyPokeAge >= 0.f && LobbyPokeAge < LobbyPokeTime)
	{
		const float P = LobbyPokeAge / LobbyPokeTime;
		Spin += LobbyPokeDir * 360.f * FMath::SmoothStep(0.f, 1.f, P);
		Lift += FMath::Sin(P * UE_PI) * LobbyPokeHop;
	}
	// The turn and the move are meant in component space (Z up, the view's axis); the Root bone may hang under a parent bone
	// with its own rotation and scale (the import's armature): through its parent's component-space transform.
	FCSPose<FCompactPose> CS;
	CS.InitPose(Output.Pose);
	const FCompactPoseBoneIndex Parent = Output.Pose.GetParentBoneIndex(RootIndex);
	const FTransform ParentCS = Parent.IsValid() ? CS.GetComponentSpaceTransform(Parent) : FTransform::Identity;
	const FTransform RootCS = CS.GetComponentSpaceTransform(RootIndex);
	const FQuat Turn = FQuat(LobbyViewAxis.GetSafeNormal(), FMath::DegreesToRadians(Roll)) * FQuat(FVector(0.f, 0.f, 1.f), FMath::DegreesToRadians(LobbyYaw * PW + Spin));
	FTransform Moved = RootCS;
	Moved.SetRotation((Turn * RootCS.GetRotation()).GetNormalized());
	Moved.AddToTranslation(LobbyOffset * PW + FVector(0.f, 0.f, Lift * MeshUnitsPerCm));
	Output.Pose[RootIndex] = Moved.GetRelativeTransform(ParentCS);
}

bool FPenguinAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	if (!bBonesCached)
	{
		CachePenguinBones(Output.Pose.GetBoneContainer());
	}
	if (Layers.Num() == 0)
	{
		Output.ResetToRefPose();
		return true;
	}

	EvaluateLayer(Layers[0], Output);
	float Accumulated = Layers[0].Weight;
	for (int32 Index = 1; Index < Layers.Num(); ++Index)
	{
		const FPenguinAnimLayer& Layer = Layers[Index];
		if (Layer.Weight <= UE_KINDA_SMALL_NUMBER)
		{
			continue;
		}
		FPoseContext LayerPose(Output);
		EvaluateLayer(Layer, LayerPose);
		Accumulated += Layer.Weight;
		PenguinAnimPrivate::BlendInto(Output, LayerPose, Layer.Weight / FMath::Max(Accumulated, UE_KINDA_SMALL_NUMBER));
	}
	ApplyAction(Output);
	Output.Pose.NormalizeRotations();
	ApplyLobbyPose(Output);
	ApplyHonkAim(Output);
	ApplyIcicleThrow(Output);
	ApplyFlap(Output);
	ApplyLookAt(Output);
	ApplyBodyShape(Output);
	ApplyLobbyPlacement(Output); // (last: the lobby turns and moves the whole penguin)
	return true;
}

void FPenguinAnimInstanceProxy::ApplyBodyShape(FPoseContext& Output)
{
	if (BodyShape.Equals(FVector::OneVector, 1.e-3) || MeshBody == INDEX_NONE)
	{
		return;
	}
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex BodyIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBody));
	if (!BodyIndex.IsValid())
	{
		return;
	}
	// The shape is given along the mesh's axes (side, forward, up); the Body bone scales along its own: how much each of
	// its axes (in the rest pose) stretches. The body's parts on it change shape with it.
	FVector Along;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		FVector Unit = FVector::ZeroVector;
		Unit[Axis] = 1.f;
		Along[Axis] = static_cast<float>((BodyRestRotation.RotateVector(Unit) * BodyShape).Size());
	}
	FTransform& BodyLocal = Output.Pose[BodyIndex];
	BodyLocal.SetScale3D(BodyLocal.GetScale3D() * Along);
	// A bone on Body takes Body's scale along its own axes (a bone's scale is per axis: no shear), so each of them gets
	// the inverse back: its own shape again - the eyes exactly round at their normal size (the snow bomb's white eyes
	// fit them), the beak the same (a fat penguin keeps a normal beak, Linus: not a stretched one), the flippers and the tail at the body's average size. Their places still move with the body's shape.
	const float Average = FMath::Pow(FMath::Max(static_cast<float>(BodyShape.X * BodyShape.Y * BodyShape.Z), 1.e-3f), 1.f / 3.f);
	for (const int32 Child : MeshBodyChildren)
	{
		const FCompactPoseBoneIndex ChildIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Child));
		if (!ChildIndex.IsValid())
		{
			continue;
		}
		const float Keep = BoneKinds.IsValidIndex(Child) && (BoneKinds[Child] == EBoneKind::Eye || BoneKinds[Child] == EBoneKind::Beak) ? 1.f : Average;
		FTransform& Local = Output.Pose[ChildIndex];
		Local.SetScale3D(Local.GetScale3D() * FVector(Keep / Along.X, Keep / Along.Y, Keep / Along.Z));
	}
	// The feet hang on the root, not on Body: the body's average size too.
	for (const int32 Foot : MeshFeet)
	{
		const FCompactPoseBoneIndex FootIndex = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Foot));
		if (FootIndex.IsValid())
		{
			FTransform& Local = Output.Pose[FootIndex];
			Local.SetScale3D(Local.GetScale3D() * Average);
		}
	}
}

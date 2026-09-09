#include "VPBroadcastOutput.h"

#include "Algo/AllOf.h"
#include "SpoutSenderComponent.h"
#include "VPBroadcastRenderer.h"
#include "VPAnimInstance.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "HAL/PlatformTime.h"
#include "Misc/Char.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
const TCHAR* BroadcastSettingsSlot = TEXT("VPBroadcastSettings");
constexpr int32 BroadcastSettingsUserIndex = 0;
}

AVPBroadcastOutput::AVPBroadcastOutput()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("AvatarCapture"));
	CaptureComponent->SetupAttachment(SceneRoot);
	CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
	CaptureComponent->CompositeMode = ESceneCaptureCompositeMode::SCCM_Overwrite;
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->bAlwaysPersistRenderingState = true;
	CaptureComponent->bUseRayTracingIfEnabled = false;
	CaptureComponent->ShowFlags.SetAtmosphere(false);
	CaptureComponent->ShowFlags.SetFog(false);
	CaptureComponent->ShowFlags.SetMotionBlur(false);
	CaptureComponent->ShowFlags.SetSkyLighting(false);

	AvatarKeyLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("AvatarKeyLight"));
	AvatarKeyLight->SetupAttachment(CaptureComponent);
	AvatarKeyLight->SetMobility(EComponentMobility::Movable);
	AvatarKeyLight->SetIntensity(1.0f);
	AvatarKeyLight->SetLightColor(FLinearColor::White);
	AvatarKeyLight->SetCastShadows(false);
	AvatarKeyLight->SetAffectReflection(false);
	AvatarKeyLight->SetAffectGlobalIllumination(false);
	AvatarKeyLight->SetSpecularScale(0.15f);
	AvatarKeyLight->SetLightingChannels(false, true, false);

	SpoutSender = CreateDefaultSubobject<USpoutSenderComponent>(TEXT("SpoutSender"));
}

FTransform AVPBroadcastOutput::CalculateInitialCameraTransform(
	const FVector& BoundsOrigin,
	float CameraDistance,
	const FRotator& ReferenceCameraRotation)
{
	FVector ViewDirection = ReferenceCameraRotation.Vector();
	ViewDirection.Z = 0.0f;
	ViewDirection = ViewDirection.GetSafeNormal();
	if (ViewDirection.IsNearlyZero())
	{
		ViewDirection = FVector::ForwardVector;
	}

	const FVector CameraLocation = BoundsOrigin - ViewDirection * CameraDistance;
	return FTransform(ViewDirection.Rotation(), CameraLocation);
}

FVector AVPBroadcastOutput::CalculateBodyCenteredFramingOrigin(
	const FVector& BoundsOrigin,
	const FVector& BodyAnchor,
	const FRotator& ReferenceCameraRotation)
{
	FVector ViewDirection = ReferenceCameraRotation.Vector();
	ViewDirection.Z = 0.0f;
	ViewDirection = ViewDirection.GetSafeNormal();
	if (ViewDirection.IsNearlyZero())
	{
		ViewDirection = FVector::ForwardVector;
	}

	const FVector CameraRight = FVector::CrossProduct(FVector::UpVector, ViewDirection).GetSafeNormal();
	const float HorizontalOffset = FVector::DotProduct(BodyAnchor - BoundsOrigin, CameraRight);
	return BoundsOrigin + CameraRight * HorizontalOffset;
}

void AVPBroadcastOutput::BeginPlay()
{
	Super::BeginPlay();
	LoadSettings();
	ApplyOutputFPS();
	CreateBroadcastTexture();
	RenderBroadcastFrame();

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem)
	{
		ObsControlSocket = SocketSubsystem->CreateSocket(
			NAME_DGram,
			TEXT("VP OBS background control"),
			false);
		if (ObsControlSocket)
		{
			ObsControlSocket->SetNonBlocking(true);
		}
	}
	FString ParsedSessionToken;
	if (FParse::Value(FCommandLine::Get(), TEXT("VPSessionToken="), ParsedSessionToken))
	{
		ParsedSessionToken.TrimStartAndEndInline();
		const bool bValidToken = ParsedSessionToken.Len() == 32 &&
			Algo::AllOf(ParsedSessionToken, [](const TCHAR Character)
			{
				return FChar::IsHexDigit(Character);
			});
		if (bValidToken)
		{
			LifecycleSessionToken = ParsedSessionToken.ToLower();
			SendLifecycleCommand(TEXT("hello"));
			LastLifecycleHeartbeatSeconds = FPlatformTime::Seconds();
			UE_LOG(LogTemp, Log, TEXT("[VPLifecycle] Managed session connected."));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[VPLifecycle] Ignored invalid session token."));
		}
	}

	if (SpoutSender && BroadcastTexture)
	{
		SpoutSender->SetTickAfterActor(this);
		SpoutSender->StartBroadcastFromRenderTarget(
			BroadcastTexture,
			SpoutSenderName,
			OutputFramesPerSecond,
			false);
		bSpoutStartRequested = true;
		UE_LOG(LogTemp, Log, TEXT("[VPBroadcast] Spout sender '%s' requested at %dx%d %dfps."),
			*SpoutSenderName, OutputWidth, OutputHeight, OutputFramesPerSecond);
	}

	SendObsBackgroundCommand();
	SendObsFPSCommand();
}

void AVPBroadcastOutput::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SaveCameraForActiveAvatar();
	if (!LifecycleSessionToken.IsEmpty())
	{
		SendLifecycleCommand(TEXT("shutdown"));
		SendLifecycleCommand(TEXT("shutdown"));
		SendLifecycleCommand(TEXT("shutdown"));
	}
	if (SpoutSender)
	{
		SpoutSender->StopBroadcast();
	}
	bSpoutStartRequested = false;

	if (ObsControlSocket)
	{
		if (ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SocketSubsystem->DestroySocket(ObsControlSocket);
		}
		ObsControlSocket = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void AVPBroadcastOutput::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	PollObsControlResponses();
	const double NowSeconds = FPlatformTime::Seconds();
	if (!LifecycleSessionToken.IsEmpty() &&
		NowSeconds - LastLifecycleHeartbeatSeconds >= LifecycleHeartbeatIntervalSeconds)
	{
		SendLifecycleCommand(TEXT("heartbeat"));
		LastLifecycleHeartbeatSeconds = NowSeconds;
	}
	RenderBroadcastFrame();
}

void AVPBroadcastOutput::CreateBroadcastTexture()
{
	AvatarCaptureTexture = NewObject<UTextureRenderTarget2D>(this, TEXT("VPAvatarCaptureTexture"));
	if (!AvatarCaptureTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("[VPBroadcast] Failed to create avatar capture render target."));
		return;
	}
	AvatarCaptureTexture->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
	AvatarCaptureTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
	AvatarCaptureTexture->bAutoGenerateMips = false;
	AvatarCaptureTexture->InitAutoFormat(OutputWidth, OutputHeight);
	AvatarCaptureTexture->UpdateResourceImmediate(true);

	BroadcastTexture = NewObject<UTextureRenderTarget2D>(this, TEXT("VPBroadcastTexture"));
	if (!BroadcastTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("[VPBroadcast] Failed to create render target."));
		return;
	}

	BroadcastTexture->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8_SRGB;
	BroadcastTexture->ClearColor = GetOutputClearColor();
	BroadcastTexture->bAutoGenerateMips = false;
	BroadcastTexture->InitAutoFormat(OutputWidth, OutputHeight);
	BroadcastTexture->UpdateResourceImmediate(true);
	CaptureComponent->TextureTarget = AvatarCaptureTexture;
}

void AVPBroadcastOutput::RenderBroadcastFrame()
{
	if (!CaptureComponent || !AvatarCaptureTexture || !BroadcastTexture)
	{
		return;
	}

	if (!CapturedAvatar.IsValid())
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(this, BroadcastTexture, GetOutputClearColor());
		return;
	}

	CaptureComponent->CaptureScene();
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	VPBroadcastRenderer::EnqueueAvatarComposite(
		AvatarCaptureTexture,
		BroadcastTexture,
		BackgroundColor,
		AvatarExposureStops,
		BackgroundMode == EVPBroadcastBackgroundMode::BackgroundRemoved,
		World->GetFeatureLevel());
}

void AVPBroadcastOutput::SetCapturedAvatar(AActor* AvatarActor, const FString& AvatarId)
{
	if (!AvatarActor || AvatarId.IsEmpty() || !CaptureComponent)
	{
		return;
	}

	SaveCameraForActiveAvatar();
	CapturedAvatar = AvatarActor;
	ActiveAvatarId = AvatarId;
	CaptureComponent->ClearShowOnlyComponents();
	CaptureComponent->ShowOnlyActorComponents(AvatarActor, true);
	bInitialCameraFramed = RestoreCameraForActiveAvatar();
	if (!bInitialCameraFramed)
	{
		FrameAvatarForInitialView();
		SaveCameraForActiveAvatar();
	}
	UE_LOG(LogTemp, Log, TEXT("[VPBroadcast] Capturing avatar '%s' (%s) only."),
		*AvatarActor->GetName(), *AvatarId);
}

void AVPBroadcastOutput::ClearCapturedAvatar()
{
	SaveCameraForActiveAvatar();
	CapturedAvatar.Reset();
	ActiveAvatarId.Reset();
	bInitialCameraFramed = false;
	if (CaptureComponent)
	{
		CaptureComponent->ClearShowOnlyComponents();
	}
}

void AVPBroadcastOutput::FrameAvatarForInitialView()
{
	if (!CaptureComponent || !CapturedAvatar.IsValid())
	{
		return;
	}

	FVector BoundsOrigin;
	FVector BoundsExtent;
	CapturedAvatar->GetActorBounds(false, BoundsOrigin, BoundsExtent, true);
	if (BoundsExtent.IsNearlyZero())
	{
		return;
	}

	FRotator ReferenceCameraRotation = FRotator::ZeroRotator;
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (CameraManager)
	{
		ReferenceCameraRotation = CameraManager->GetCameraRotation();
	}

	FVector FramingOrigin = BoundsOrigin;
	FVector BodyAnchor;
	if (TryGetBodyFramingAnchor(BodyAnchor))
	{
		FramingOrigin = CalculateBodyCenteredFramingOrigin(
			BoundsOrigin,
			BodyAnchor,
			ReferenceCameraRotation);
	}

	constexpr float HorizontalFOVDegrees = 40.0f;
	constexpr float AspectRatio = static_cast<float>(OutputWidth) / static_cast<float>(OutputHeight);
	constexpr float Padding = 1.18f;
	const float HorizontalHalfAngle = FMath::DegreesToRadians(HorizontalFOVDegrees * 0.5f);
	const float VerticalHalfAngle = FMath::Atan(FMath::Tan(HorizontalHalfAngle) / AspectRatio);
	const float HorizontalCenterOffset = FVector::Dist2D(BoundsOrigin, FramingOrigin);
	const float HorizontalExtent = FMath::Max(BoundsExtent.X, BoundsExtent.Y) + HorizontalCenterOffset;
	const float HorizontalDistance = HorizontalExtent / FMath::Max(0.01f, FMath::Tan(HorizontalHalfAngle));
	const float VerticalDistance = BoundsExtent.Z / FMath::Max(0.01f, FMath::Tan(VerticalHalfAngle));
	const float CameraDistance = FMath::Max(100.0f, FMath::Max(HorizontalDistance, VerticalDistance) * Padding);
	const FTransform CameraTransform = CalculateInitialCameraTransform(
		FramingOrigin,
		CameraDistance,
		ReferenceCameraRotation);

	CaptureComponent->SetWorldLocationAndRotation(CameraTransform.GetLocation(), CameraTransform.Rotator());
	CaptureComponent->FOVAngle = HorizontalFOVDegrees;
	bInitialCameraFramed = true;
	UE_LOG(LogTemp, Log,
		TEXT("[VPBroadcast] Initial full-body camera framed. BoundsOrigin=%s FramingOrigin=%s Extent=%s Distance=%.1f FOV=%.1f"),
		*BoundsOrigin.ToCompactString(),
		*FramingOrigin.ToCompactString(),
		*BoundsExtent.ToCompactString(),
		CameraDistance,
		HorizontalFOVDegrees);
}

bool AVPBroadcastOutput::TryGetBodyFramingAnchor(FVector& OutAnchor) const
{
	if (!CapturedAvatar.IsValid())
	{
		return false;
	}

	TInlineComponentArray<USkeletalMeshComponent*> MeshComponents;
	CapturedAvatar->GetComponents(MeshComponents);
	for (const USkeletalMeshComponent* MeshComponent : MeshComponents)
	{
		const UVPAnimInstance* AnimInstance = MeshComponent
			? Cast<UVPAnimInstance>(MeshComponent->GetAnimInstance())
			: nullptr;
		if (!AnimInstance ||
			MeshComponent->GetBoneIndex(AnimInstance->HeadBoneName) == INDEX_NONE ||
			MeshComponent->GetBoneIndex(AnimInstance->LeftUpperArmBoneName) == INDEX_NONE ||
			MeshComponent->GetBoneIndex(AnimInstance->RightUpperArmBoneName) == INDEX_NONE)
		{
			continue;
		}

		OutAnchor = (
			MeshComponent->GetBoneLocation(AnimInstance->HeadBoneName) +
			MeshComponent->GetBoneLocation(AnimInstance->LeftUpperArmBoneName) +
			MeshComponent->GetBoneLocation(AnimInstance->RightUpperArmBoneName)) / 3.0f;
		return !OutAnchor.ContainsNaN();
	}

	return false;
}

bool AVPBroadcastOutput::RestoreCameraForActiveAvatar()
{
	if (!CaptureComponent || !CapturedAvatar.IsValid() || ActiveAvatarId.IsEmpty())
	{
		return false;
	}
	const FVPAvatarCameraSettings* Settings = AvatarCameraSettings.Find(ActiveAvatarId);
	if (!Settings || !Settings->bHasSavedView ||
		!FMath::IsFinite(Settings->FieldOfView) ||
		Settings->FieldOfView < 15.0f || Settings->FieldOfView > 90.0f ||
		Settings->RelativeTransform.ContainsNaN())
	{
		return false;
	}

	CaptureComponent->SetWorldTransform(
		Settings->RelativeTransform * CapturedAvatar->GetActorTransform());
	CaptureComponent->FOVAngle = Settings->FieldOfView;
	UE_LOG(LogTemp, Log, TEXT("[VPBroadcast] Restored camera for avatar %s."), *ActiveAvatarId);
	return true;
}

void AVPBroadcastOutput::SaveCameraForActiveAvatar()
{
	if (!CaptureComponent || !CapturedAvatar.IsValid() || ActiveAvatarId.IsEmpty())
	{
		return;
	}
	FVPAvatarCameraSettings& Settings = AvatarCameraSettings.FindOrAdd(ActiveAvatarId);
	Settings.bHasSavedView = true;
	Settings.RelativeTransform = CaptureComponent->GetComponentTransform().GetRelativeTransform(
		CapturedAvatar->GetActorTransform());
	Settings.FieldOfView = CaptureComponent->FOVAngle;
	SaveSettings();
}

void AVPBroadcastOutput::PanCamera(const FVector2D& ScreenDelta)
{
	if (!CaptureComponent || !CapturedAvatar.IsValid())
	{
		return;
	}
	constexpr float UnitsPerPixel = 0.22f;
	const FVector Offset = CaptureComponent->GetRightVector() * (-ScreenDelta.X * UnitsPerPixel) +
		CaptureComponent->GetUpVector() * (ScreenDelta.Y * UnitsPerPixel);
	CaptureComponent->AddWorldOffset(Offset);
	SaveCameraForActiveAvatar();
}

void AVPBroadcastOutput::AdjustCameraZoom(float WheelDelta)
{
	if (!CaptureComponent || !CapturedAvatar.IsValid() || FMath::IsNearlyZero(WheelDelta))
	{
		return;
	}
	CaptureComponent->FOVAngle = FMath::Clamp(
		CaptureComponent->FOVAngle - WheelDelta * 3.0f,
		18.0f,
		80.0f);
	SaveCameraForActiveAvatar();
}

void AVPBroadcastOutput::ResetCameraToFullBody()
{
	if (ActiveAvatarId.IsEmpty())
	{
		return;
	}
	AvatarCameraSettings.Remove(ActiveAvatarId);
	bInitialCameraFramed = false;
	FrameAvatarForInitialView();
	SaveCameraForActiveAvatar();
}

bool AVPBroadcastOutput::DeleteCameraProfile(const FString& AvatarId)
{
	if (AvatarId.IsEmpty())
	{
		return true;
	}
	AvatarCameraSettings.Remove(AvatarId);
	return SaveSettings();
}

bool AVPBroadcastOutput::PruneCameraProfiles(
	const TSet<FString>& ValidAvatarIds,
	int32& OutRemovedCount)
{
	OutRemovedCount = RemoveCameraProfilesNotIn(AvatarCameraSettings, ValidAvatarIds);
	return OutRemovedCount == 0 || SaveSettings();
}

int32 AVPBroadcastOutput::RemoveCameraProfilesNotIn(
	TMap<FString, FVPAvatarCameraSettings>& CameraProfiles,
	const TSet<FString>& ValidAvatarIds)
{
	int32 RemovedCount = 0;
	for (auto It = CameraProfiles.CreateIterator(); It; ++It)
	{
		if (!ValidAvatarIds.Contains(It.Key()))
		{
			It.RemoveCurrent();
			++RemovedCount;
		}
	}
	return RemovedCount;
}

void AVPBroadcastOutput::SetBackgroundMode(EVPBroadcastBackgroundMode NewMode)
{
	if (BackgroundMode == NewMode)
	{
		return;
	}
	BackgroundMode = NewMode;
	ApplyBackgroundColor();
	SaveSettings();
	SendObsBackgroundCommand();
}

void AVPBroadcastOutput::SetBackgroundColor(const FLinearColor& NewColor)
{
	const FLinearColor PreviousColor = BackgroundColor;
	PreviewBackgroundColor(NewColor);
	if (BackgroundColor.Equals(PreviousColor))
	{
		return;
	}
	CommitBackgroundColor();
}

void AVPBroadcastOutput::PreviewBackgroundColor(const FLinearColor& NewColor)
{
	const FLinearColor Sanitized(
		FMath::Clamp(NewColor.R, 0.0f, 1.0f),
		FMath::Clamp(NewColor.G, 0.0f, 1.0f),
		FMath::Clamp(NewColor.B, 0.0f, 1.0f),
		1.0f);
	if (BackgroundColor.Equals(Sanitized))
	{
		return;
	}
	BackgroundColor = Sanitized;
	if (BroadcastTexture)
	{
		BroadcastTexture->ClearColor = GetOutputClearColor();
	}
}

void AVPBroadcastOutput::CommitBackgroundColor()
{
	ApplyBackgroundColor();
	SaveSettings();
}

bool AVPBroadcastOutput::SetBackgroundColorHex(const FString& HexColor)
{
	FLinearColor Parsed;
	if (!ParseHexColor(HexColor, Parsed))
	{
		return false;
	}
	SetBackgroundColor(Parsed);
	return true;
}

bool AVPBroadcastOutput::ParseHexColor(const FString& HexColor, FLinearColor& OutColor)
{
	FString Normalized = HexColor.TrimStartAndEnd();
	Normalized.RemoveFromStart(TEXT("#"));
	if (Normalized.Len() != 6)
	{
		return false;
	}

	for (const TCHAR Character : Normalized)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}

	const FColor Color = FColor::FromHex(Normalized);
	OutColor = FLinearColor::FromSRGBColor(Color);
	OutColor.A = 1.0f;
	return true;
}

FLinearColor AVPBroadcastOutput::FromSrgb8(uint8 Red, uint8 Green, uint8 Blue)
{
	return FLinearColor::FromSRGBColor(FColor(Red, Green, Blue, 255));
}

float AVPBroadcastOutput::SanitizeAvatarExposure(float ExposureStops)
{
	return FMath::IsFinite(ExposureStops)
		? FMath::Clamp(ExposureStops, -2.0f, 2.0f)
		: 0.0f;
}

int32 AVPBroadcastOutput::SanitizeOutputFPS(int32 FramesPerSecond)
{
	return FMath::Clamp(FramesPerSecond, 15, 144);
}

void AVPBroadcastOutput::PreviewAvatarExposure(float NewExposureStops)
{
	AvatarExposureStops = SanitizeAvatarExposure(NewExposureStops);
}

void AVPBroadcastOutput::CommitAvatarExposure()
{
	SaveSettings();
}

void AVPBroadcastOutput::PreviewOutputFPS(int32 NewFPS)
{
	const int32 Sanitized = SanitizeOutputFPS(NewFPS);
	if (OutputFramesPerSecond == Sanitized)
	{
		return;
	}

	OutputFramesPerSecond = Sanitized;
	ApplyOutputFPS();
}

void AVPBroadcastOutput::CommitOutputFPS()
{
	SaveSettings();
	SendObsFPSCommand();
}

FString AVPBroadcastOutput::GetBackgroundColorHex() const
{
	return BackgroundColor.ToFColorSRGB().ToHex().Left(6);
}

FString AVPBroadcastOutput::GetBackgroundModeLabel() const
{
	return BackgroundMode == EVPBroadcastBackgroundMode::BackgroundRemoved
		? TEXT("배경 제거")
		: TEXT("단색 배경");
}

void AVPBroadcastOutput::ApplyBackgroundColor()
{
	if (!BroadcastTexture)
	{
		return;
	}
	BroadcastTexture->ClearColor = GetOutputClearColor();
}

void AVPBroadcastOutput::ApplyOutputFPS()
{
	OutputFramesPerSecond = SanitizeOutputFPS(OutputFramesPerSecond);
	if (GEngine)
	{
		GEngine->SetMaxFPS(static_cast<float>(OutputFramesPerSecond));
	}
	if (SpoutSender)
	{
		SpoutSender->BroadcastFPS = OutputFramesPerSecond;
		SpoutSender->SetComponentTickInterval(1.0f / static_cast<float>(OutputFramesPerSecond));
	}
	UE_LOG(LogTemp, Log, TEXT("[VPBroadcast] App, capture, and Spout target set to %d fps."),
		OutputFramesPerSecond);
}

FLinearColor AVPBroadcastOutput::GetOutputClearColor() const
{
	return BackgroundMode == EVPBroadcastBackgroundMode::BackgroundRemoved
		? FLinearColor::Transparent
		: BackgroundColor;
}

void AVPBroadcastOutput::LoadSettings()
{
	if (USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(
		BroadcastSettingsSlot, BroadcastSettingsUserIndex))
	{
		if (const UVPBroadcastSettingsSaveGame* Settings =
			Cast<UVPBroadcastSettingsSaveGame>(Loaded))
		{
			BackgroundMode = Settings->BackgroundMode == EVPBroadcastBackgroundMode::SolidColor
				? EVPBroadcastBackgroundMode::SolidColor
				: EVPBroadcastBackgroundMode::BackgroundRemoved;
			BackgroundColor = Settings->BackgroundColor;
			BackgroundColor.A = 1.0f;
			AvatarExposureStops = SanitizeAvatarExposure(Settings->AvatarExposureStops);
			OutputFramesPerSecond = SanitizeOutputFPS(Settings->OutputFramesPerSecond);
			AvatarCameraSettings = Settings->AvatarCameraSettings;
		}
	}
}

bool AVPBroadcastOutput::SaveSettings() const
{
	UVPBroadcastSettingsSaveGame* Settings = Cast<UVPBroadcastSettingsSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UVPBroadcastSettingsSaveGame::StaticClass()));
	if (!Settings)
	{
		return false;
	}
	Settings->BackgroundMode = BackgroundMode;
	Settings->BackgroundColor = BackgroundColor;
	Settings->AvatarExposureStops = AvatarExposureStops;
	Settings->OutputFramesPerSecond = OutputFramesPerSecond;
	Settings->AvatarCameraSettings = AvatarCameraSettings;
	return UGameplayStatics::SaveGameToSlot(
		Settings, BroadcastSettingsSlot, BroadcastSettingsUserIndex);
}

void AVPBroadcastOutput::SendObsBackgroundCommand()
{
	const TCHAR* Mode = BackgroundMode == EVPBroadcastBackgroundMode::BackgroundRemoved
		? TEXT("background_removed")
		: TEXT("solid_color");
	const FString Payload = FString::Printf(
		TEXT("{\"version\":1,\"type\":\"broadcast_background\",\"mode\":\"%s\",")
		TEXT("\"sender\":\"%s\",\"source\":\"%s\",\"filter\":\"%s\"}"),
		Mode,
		*SpoutSenderName,
		*ObsSourceName,
		*ObsFilterName);
	SendControlPayload(Payload);
}

void AVPBroadcastOutput::SendObsFPSCommand()
{
	const FString Payload = FString::Printf(
		TEXT("{\"version\":1,\"type\":\"broadcast_fps\",\"fps\":%d}"),
		OutputFramesPerSecond);
	ObsFPSStatusText = FString::Printf(
		TEXT("앱·Spout %d FPS · OBS 확인 중"),
		OutputFramesPerSecond);
	SendControlPayload(Payload);
}

void AVPBroadcastOutput::PollObsControlResponses()
{
	if (!ObsControlSocket)
	{
		return;
	}

	uint32 PendingBytes = 0;
	while (ObsControlSocket->HasPendingData(PendingBytes))
	{
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(FMath::Min<uint32>(PendingBytes, 2048u) + 1u);
		TSharedRef<FInternetAddr> ReplyAddress =
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		int32 BytesRead = 0;
		if (!ObsControlSocket->RecvFrom(
			Buffer.GetData(),
			Buffer.Num() - 1,
			BytesRead,
			*ReplyAddress) || BytesRead <= 0)
		{
			break;
		}
		Buffer[BytesRead] = 0;

		const FString Payload(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Buffer.GetData())));
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() ||
			Root->GetIntegerField(TEXT("version")) != 1 ||
			Root->GetStringField(TEXT("type")) != TEXT("broadcast_fps_result"))
		{
			continue;
		}

		const FString Status = Root->GetStringField(TEXT("status"));
		const int32 RequestedFPS = SanitizeOutputFPS(
			Root->GetIntegerField(TEXT("requested_fps")));
		double ObsFPS = 0.0;
		Root->TryGetNumberField(TEXT("obs_fps"), ObsFPS);
		const FString ObsFPSLabel = FMath::IsNearlyEqual(
			static_cast<float>(ObsFPS),
			FMath::RoundToFloat(static_cast<float>(ObsFPS)),
			0.001f)
			? FString::FromInt(FMath::RoundToInt(ObsFPS))
			: FString::Printf(TEXT("%.2f"), ObsFPS);

		if (Status == TEXT("applied") || Status == TEXT("unchanged"))
		{
			ObsFPSStatusText = FString::Printf(
				TEXT("앱·Spout·OBS %d FPS · 동기화됨"),
				RequestedFPS);
		}
		else if (Status == TEXT("output_active"))
		{
			ObsFPSStatusText = FString::Printf(
				TEXT("앱·Spout %d FPS · OBS 출력 중 (%s FPS 유지)"),
				RequestedFPS,
				*ObsFPSLabel);
			ObsFPSNoticeText = FString::Printf(
				TEXT("OBS가 출력 중이므로 송출 FPS는 변하지 않습니다.\n")
				TEXT("현재 앱과 Spout만 %d FPS로 변경되었습니다."),
				RequestedFPS);
			++ObsFPSNoticeRevision;
		}
		else if (Status == TEXT("unavailable"))
		{
			ObsFPSStatusText = FString::Printf(
				TEXT("앱·Spout %d FPS · OBS 연결 안 됨"),
				RequestedFPS);
		}
		else if (Status == TEXT("failed"))
		{
			ObsFPSStatusText = FString::Printf(
				TEXT("앱·Spout %d FPS · OBS 동기화 실패"),
				RequestedFPS);
		}
	}
}

void AVPBroadcastOutput::SendLifecycleCommand(const TCHAR* Event)
{
	if (LifecycleSessionToken.IsEmpty())
	{
		return;
	}
	const FString Payload = FString::Printf(
		TEXT("{\"version\":1,\"type\":\"pipeline_lifecycle\",\"event\":\"%s\",\"token\":\"%s\"}"),
		Event,
		*LifecycleSessionToken);
	SendControlPayload(Payload);
}

void AVPBroadcastOutput::SendControlPayload(const FString& Payload)
{
	if (!ObsControlSocket)
	{
		return;
	}
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		return;
	}
	bool bAddressValid = false;
	TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
	Address->SetIp(TEXT("127.0.0.1"), bAddressValid);
	Address->SetPort(ObsControlPort);
	if (!bAddressValid)
	{
		return;
	}
	FTCHARToUTF8 Utf8Payload(*Payload);
	int32 BytesSent = 0;
	ObsControlSocket->SendTo(
		reinterpret_cast<const uint8*>(Utf8Payload.Get()),
		Utf8Payload.Length(),
		BytesSent,
		*Address);
}

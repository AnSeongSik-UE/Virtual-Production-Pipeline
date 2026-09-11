#if WITH_DEV_AUTOMATION_TESTS

#include <limits>

#include "Misc/AutomationTest.h"
#include "Components/SkeletalMeshComponent.h"
#include "Kismet/GameplayStatics.h"
#include "VPAvatarManager.h"
#include "VPAnimInstance.h"
#include "VPBroadcastOutput.h"
#include "VPBroadcastPreview.h"
#include "VPRuntimeAvatarAnimInstance.h"
#include "VPTrackingDashboard.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPBroadcastPreviewAspectFitTest,
	"VPPipeline.UI.BroadcastPreviewAspectFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPBroadcastPreviewAspectFitTest::RunTest(const FString& Parameters)
{
	const FSlateRect ExactRect = UVPBroadcastPreview::CalculateAspectFitRect(FVector2D(1920.0, 1080.0));
	TestTrue(TEXT("A 16:9 window uses its full area"),
		FMath::IsNearlyEqual(ExactRect.Left, 0.0f, 0.001f) &&
		FMath::IsNearlyEqual(ExactRect.Top, 0.0f, 0.001f) &&
		FMath::IsNearlyEqual(ExactRect.Right, 1920.0f, 0.001f) &&
		FMath::IsNearlyEqual(ExactRect.Bottom, 1080.0f, 0.001f));

	const FSlateRect TallRect = UVPBroadcastPreview::CalculateAspectFitRect(FVector2D(1000.0, 1000.0));
	TestTrue(TEXT("A square window is letterboxed vertically"),
		FMath::IsNearlyEqual(TallRect.Left, 0.0f, 0.001f) &&
		FMath::IsNearlyEqual(TallRect.Top, 218.75f, 0.001f) &&
		FMath::IsNearlyEqual(TallRect.Right, 1000.0f, 0.001f) &&
		FMath::IsNearlyEqual(TallRect.Bottom, 781.25f, 0.001f));

	const FSlateRect WideRect = UVPBroadcastPreview::CalculateAspectFitRect(FVector2D(2000.0, 800.0));
	TestTrue(TEXT("A wide window is pillarboxed horizontally"),
		FMath::IsNearlyEqual(WideRect.Left, 288.8889f, 0.001f) &&
		FMath::IsNearlyEqual(WideRect.Top, 0.0f) &&
		FMath::IsNearlyEqual(WideRect.Right, 1711.1111f, 0.001f) &&
		FMath::IsNearlyEqual(WideRect.Bottom, 800.0f));

	const FSlateRect EmptyRect = UVPBroadcastPreview::CalculateAspectFitRect(FVector2D::ZeroVector);
	TestTrue(TEXT("An invalid window returns an empty rectangle"),
		FMath::IsNearlyEqual(EmptyRect.Left, EmptyRect.Right) &&
		FMath::IsNearlyEqual(EmptyRect.Top, EmptyRect.Bottom));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPTrackingDashboardGuidanceTest,
	"VPPipeline.UI.GuidanceText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPTrackingDashboardGuidanceTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Idle dashboard asks for calibration"),
		UVPTrackingDashboard::BuildGuidanceText(
			EVPCalibrationState::Idle,
			EVPArmValidationStage::Idle,
			FString(),
			FString()).ToString(),
		FString(TEXT("1. 중립 자세 캘리브레이션을 시작하세요.")));

	TestEqual(
		TEXT("Active calibration status takes priority"),
		UVPTrackingDashboard::BuildGuidanceText(
			EVPCalibrationState::CountingDown,
			EVPArmValidationStage::Idle,
			TEXT("중립 자세 유지: 2.0초"),
			FString()).ToString(),
		FString(TEXT("중립 자세 유지: 2.0초")));

	TestEqual(
		TEXT("Validation stage uses its live instruction"),
		UVPTrackingDashboard::BuildGuidanceText(
			EVPCalibrationState::Succeeded,
			EVPArmValidationStage::LeftArmRaised,
			TEXT("캘리브레이션 완료"),
			TEXT("2/4: 본인 오른팔은 내리고 왼팔을 옆으로 어깨 높이(약 90도)까지 들어 1초 유지하세요.")).ToString(),
		FString(TEXT("2/4: 본인 오른팔은 내리고 왼팔을 옆으로 어깨 높이(약 90도)까지 들어 1초 유지하세요.")));

	USkeletalMeshComponent* InstructionMesh = NewObject<USkeletalMeshComponent>();
	UVPAnimInstance* InstructionAnim = NewObject<UVPAnimInstance>(InstructionMesh);
	InstructionAnim->bHasNeutralCalibration = true;
	TestTrue(TEXT("Neutral validation starts when calibration exists"), InstructionAnim->StartArmValidation());
	TestTrue(TEXT("Neutral instruction specifies forward-facing posture"),
		InstructionAnim->ArmValidationStatus.Contains(TEXT("정면")));
	TestTrue(TEXT("Neutral instruction specifies arms beside the body"),
		InstructionAnim->ArmValidationStatus.Contains(TEXT("몸 옆")));
	TestTrue(TEXT("Neutral instruction specifies a hold duration"),
		InstructionAnim->ArmValidationStatus.Contains(TEXT("1초")));

	TestEqual(
		TEXT("Successful validation result remains visible"),
		UVPTrackingDashboard::BuildGuidanceText(
			EVPCalibrationState::Succeeded,
			EVPArmValidationStage::Passed,
			TEXT("캘리브레이션 완료"),
			TEXT("팔 검증 통과")).ToString(),
		FString(TEXT("팔 검증 통과")));

	TestFalse(
		TEXT("Idle guidance is hidden"),
		UVPTrackingDashboard::ShouldShowGuidancePanel(
			EVPCalibrationState::Idle,
			EVPArmValidationStage::Idle,
			0.0f,
			0.0f));
	TestTrue(
		TEXT("Active calibration guidance is visible"),
		UVPTrackingDashboard::ShouldShowGuidancePanel(
			EVPCalibrationState::CountingDown,
			EVPArmValidationStage::Idle,
			0.0f,
			0.0f));
	TestTrue(
		TEXT("A calibration failure remains visible during its display period"),
		UVPTrackingDashboard::ShouldShowGuidancePanel(
			EVPCalibrationState::Failed,
			EVPArmValidationStage::Idle,
			0.0f,
			1.0f));
	TestFalse(
		TEXT("A calibration failure hides after its display period"),
		UVPTrackingDashboard::ShouldShowGuidancePanel(
			EVPCalibrationState::Failed,
			EVPArmValidationStage::Idle,
			0.0f,
			0.0f));
	TestTrue(
		TEXT("Failed validation guidance remains visible"),
		UVPTrackingDashboard::ShouldShowGuidancePanel(
			EVPCalibrationState::Succeeded,
			EVPArmValidationStage::Failed,
			0.0f,
			0.0f));
	TestTrue(
		TEXT("A successful result remains visible during its grace period"),
		UVPTrackingDashboard::ShouldShowGuidancePanel(
			EVPCalibrationState::Succeeded,
			EVPArmValidationStage::Passed,
			1.0f,
			0.0f));
	TestFalse(
		TEXT("A successful result hides after its grace period"),
		UVPTrackingDashboard::ShouldShowGuidancePanel(
			EVPCalibrationState::Succeeded,
			EVPArmValidationStage::Passed,
			0.0f,
			0.0f));

	const FString DeleteConfirmation =
		UVPTrackingDashboard::BuildDeleteAvatarConfirmationText(TEXT("Seed-san")).ToString();
	TestTrue(TEXT("Delete confirmation identifies the avatar"), DeleteConfirmation.Contains(TEXT("Seed-san")));
	TestTrue(TEXT("Delete confirmation discloses camera removal"), DeleteConfirmation.Contains(TEXT("카메라")));
	TestTrue(TEXT("Delete confirmation discloses calibration removal"), DeleteConfirmation.Contains(TEXT("캘리브레이션")));
	TestTrue(TEXT("Delete confirmation preserves the source VRM"), DeleteConfirmation.Contains(TEXT("원본 VRM 파일은 유지")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPRuntimeAvatarArmLiftAxisTest,
	"VPPipeline.Avatar.ReferencePoseArmLiftAxis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPRuntimeAvatarArmLiftAxisTest::RunTest(const FString& Parameters)
{
	FVector LeftAxis;
	float LeftReferenceElevation = 0.0f;
	TestTrue(
		TEXT("Left reference arm produces a lift axis"),
		UVPRuntimeAvatarAnimInstance::CalculateArmLiftAxis(
			FVector(0.0, -1.0, 0.0),
			FQuat::Identity,
			LeftAxis,
			LeftReferenceElevation));
	TestEqual(TEXT("Horizontal reference arm elevation is 90 degrees"), LeftReferenceElevation, 90.0f);
	const FVector HorizontalLeft = FQuat(
		LeftAxis,
		FMath::DegreesToRadians(90.0f - LeftReferenceElevation)).RotateVector(
			FVector(0.0, -1.0, 0.0));
	TestTrue(
		TEXT("A 90-degree tracked target keeps a T-pose arm horizontal"),
		HorizontalLeft.Equals(FVector(0.0, -1.0, 0.0), 0.001f));
	const FVector NeutralLeft = FQuat(
		LeftAxis,
		FMath::DegreesToRadians(-LeftReferenceElevation)).RotateVector(
			FVector(0.0, -1.0, 0.0));
	TestTrue(TEXT("Zero tracked angle lowers a T-pose arm"), NeutralLeft.Equals(FVector::DownVector, 0.001f));
	TestEqual(
		TEXT("Rest tracking elevation displays a 35-degree A-pose"),
		UVPRuntimeAvatarAnimInstance::MapTrackingElevationToVisualElevation(0.0f),
		35.0f);
	TestEqual(
		TEXT("Negative tracking elevation does not collapse the visual rest pose"),
		UVPRuntimeAvatarAnimInstance::MapTrackingElevationToVisualElevation(-30.0f),
		35.0f);
	TestEqual(
		TEXT("Mid lift transitions continuously from A-pose to horizontal"),
		UVPRuntimeAvatarAnimInstance::MapTrackingElevationToVisualElevation(45.0f),
		62.5f);
	TestEqual(
		TEXT("Horizontal tracking remains horizontal"),
		UVPRuntimeAvatarAnimInstance::MapTrackingElevationToVisualElevation(90.0f),
		90.0f);
	TestEqual(
		TEXT("Above-horizontal tracking keeps its original elevation"),
		UVPRuntimeAvatarAnimInstance::MapTrackingElevationToVisualElevation(120.0f),
		120.0f);

	FVector RightAxis;
	float RightReferenceElevation = 0.0f;
	TestTrue(
		TEXT("Right reference arm produces a lift axis"),
		UVPRuntimeAvatarAnimInstance::CalculateArmLiftAxis(
			FVector(0.0, 1.0, 0.0),
			FQuat::Identity,
			RightAxis,
			RightReferenceElevation));
	const FVector HorizontalRight = FQuat(
		RightAxis,
		FMath::DegreesToRadians(90.0f - RightReferenceElevation)).RotateVector(
			FVector(0.0, 1.0, 0.0));
	TestTrue(
		TEXT("A 90-degree tracked target keeps the right arm horizontal"),
		HorizontalRight.Equals(FVector(0.0, 1.0, 0.0), 0.001f));

	const FVector AposeDirection = FVector(0.0, 1.0, -1.0).GetSafeNormal();
	float AposeReferenceElevation = 0.0f;
	TestTrue(
		TEXT("A-pose reference arm produces a lift mapping"),
		UVPRuntimeAvatarAnimInstance::CalculateArmLiftAxis(
			AposeDirection,
			FQuat::Identity,
			RightAxis,
			AposeReferenceElevation));
	TestTrue(
		TEXT("A-pose reference elevation is 45 degrees"),
		FMath::IsNearlyEqual(AposeReferenceElevation, 45.0f, 0.001f));
	const FVector HorizontalFromApose = FQuat(
		RightAxis,
		FMath::DegreesToRadians(90.0f - AposeReferenceElevation)).RotateVector(AposeDirection);
	TestTrue(
		TEXT("A 90-degree tracked target does not overshoot an A-pose arm"),
		HorizontalFromApose.Equals(FVector(0.0, 1.0, 0.0), 0.001f));

	TestFalse(
		TEXT("A vertical reference arm falls back instead of producing an unstable axis"),
		UVPRuntimeAvatarAnimInstance::CalculateArmLiftAxis(
			FVector::UpVector,
			FQuat::Identity,
			LeftAxis,
			LeftReferenceElevation));

	const FRotator SourceHeadRotation(10.0f, 20.0f, -5.0f);
	const FQuat SplitRotation =
		UVPRuntimeAvatarAnimInstance::ScaleBoneRotation(SourceHeadRotation, 0.3f).Quaternion() *
		UVPRuntimeAvatarAnimInstance::ScaleBoneRotation(SourceHeadRotation, 0.7f).Quaternion();
	TestTrue(TEXT("Neck and head weights preserve the total rotation"),
		SplitRotation.Equals(SourceHeadRotation.Quaternion(), 0.002f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPTrackingTransportStatusTest,
	"VPPipeline.UI.TrackingTransportStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPTrackingTransportStatusTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("A long minimized interval restarts the input-rate sample"),
		UVPTrackingDashboard::ShouldRestartPacketRateSample(47.0));
	TestFalse(TEXT("A normal frame interval keeps the input-rate sample"),
		UVPTrackingDashboard::ShouldRestartPacketRateSample(1.0 / 60.0));
	TestTrue(TEXT("Wall-clock rate stays near 30 after 1411 packets over 47 seconds"),
		FMath::IsNearlyEqual(
			UVPTrackingDashboard::CalculatePacketRate(1411, 47.0),
			30.0213f,
			0.001f));
	TestEqual(TEXT("Invalid elapsed time produces a zero rate"),
		UVPTrackingDashboard::CalculatePacketRate(30, 0.0),
		0.0f);

	const FString Connecting = UVPTrackingDashboard::BuildTrackingStatusText(
		false, 0.0f, 0.0f, false, false, false, false).ToString();
	TestTrue(TEXT("Initial sample reports connecting"), Connecting.Contains(TEXT("데이터 연결 중")));
	TestFalse(TEXT("User status does not expose UDP counter"), Connecting.Contains(TEXT("UDP")));

	const FString Healthy = UVPTrackingDashboard::BuildTrackingStatusText(
		true, 29.6f, 0.1f, true, true, true, true).ToString();
	TestTrue(TEXT("Healthy tracking input rate is distinct from broadcast FPS"),
		Healthy.Contains(TEXT("트래킹 입력 정상 · 30회/초")));
	TestFalse(TEXT("Tracking input rate does not use the broadcast FPS unit"),
		Healthy.Contains(TEXT("fps")) || Healthy.Contains(TEXT("FPS")));
	TestTrue(TEXT("Tracking summaries remain visible"), Healthy.Contains(TEXT("오른팔 추적")));

	const FString Delayed = UVPTrackingDashboard::BuildTrackingStatusText(
		true, 12.4f, 0.1f, true, true, true, false).ToString();
	TestTrue(TEXT("Low tracking input rate reports delay"),
		Delayed.Contains(TEXT("트래킹 입력 지연 · 12회/초")));

	const FString Disconnected = UVPTrackingDashboard::BuildTrackingStatusText(
		true, 30.0f, 0.5f, false, false, false, false).ToString();
	TestTrue(TEXT("Silence reports disconnection"), Disconnected.Contains(TEXT("데이터 끊김")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPTrackingPoseOverlayProjectionTest,
	"VPPipeline.UI.PoseOverlayProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPTrackingPoseOverlayProjectionTest::RunTest(const FString& Parameters)
{
	const FVector2D Projected = UVPTrackingDashboard::ProjectPoseLandmark(
		FVector(0.25, 0.75, 0.0),
		FVector2D(100.0, 50.0),
		FVector2D(200.0, 100.0));
	TestEqual(TEXT("Projected X"), Projected.X, 150.0);
	TestEqual(TEXT("Projected Y"), Projected.Y, 125.0);

	const FVector2D Clamped = UVPTrackingDashboard::ProjectPoseLandmark(
		FVector(-1.0, 2.0, 0.0),
		FVector2D::ZeroVector,
		FVector2D(200.0, 100.0));
	TestEqual(TEXT("X clamps to left edge"), Clamped.X, 0.0);
	TestEqual(TEXT("Y clamps to bottom edge"), Clamped.Y, 100.0);

	FVPPoseLandmark Landmark;
	Landmark.Visibility = 0.8f;
	Landmark.Presence = 0.45f;
	TestEqual(
		TEXT("Confidence uses the weaker signal"),
		UVPTrackingDashboard::GetPoseLandmarkConfidence(Landmark),
		0.45f);

	FVPTrackingFrame PoseFrame;
	PoseFrame.bPoseTracked = true;
	PoseFrame.PoseLandmarks.SetNum(33);
	for (FVPPoseLandmark& PoseLandmark : PoseFrame.PoseLandmarks)
	{
		PoseLandmark.Visibility = 0.9f;
		PoseLandmark.Presence = 0.9f;
	}
	PoseFrame.PoseLandmarks[13].Presence = 0.28f;
	float WeakestConfidence = 0.0f;
	const FString PoseStatus = UVPTrackingDashboard::BuildPoseStatusText(
		PoseFrame, WeakestConfidence).ToString();
	TestTrue(TEXT("Weak pose status names the affected elbow"),
		PoseStatus.Contains(TEXT("왼쪽 팔꿈치")));
	TestTrue(TEXT("Weak pose status preserves the measured confidence"),
		PoseStatus.Contains(TEXT("0.28")));
	TestTrue(TEXT("Weak pose confidence is returned for color selection"),
		FMath::IsNearlyEqual(WeakestConfidence, 0.28f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPBroadcastHexColorTest,
	"VPPipeline.Broadcast.HexColor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPBroadcastHexColorTest::RunTest(const FString& Parameters)
{
	FLinearColor Parsed;
	TestTrue(TEXT("Valid #RRGGBB parses"), AVPBroadcastOutput::ParseHexColor(TEXT("#0099FF"), Parsed));
	TestEqual(TEXT("Parsed color round-trips"), Parsed.ToFColorSRGB().ToHex().Left(6), FString(TEXT("0099FF")));
	TestFalse(TEXT("Invalid hex is rejected"), AVPBroadcastOutput::ParseHexColor(TEXT("#GGFF00"), Parsed));
	TestFalse(TEXT("Short hex is rejected"), AVPBroadcastOutput::ParseHexColor(TEXT("#0F0"), Parsed));
	const FLinearColor SliderColor = AVPBroadcastOutput::FromSrgb8(0, 153, 255);
	TestEqual(
		TEXT("RGB slider values round-trip through sRGB conversion"),
		SliderColor.ToFColorSRGB(),
		FColor(0, 153, 255, 255));
	TestEqual(
		TEXT("Avatar exposure clamps low"),
		AVPBroadcastOutput::SanitizeAvatarExposure(-3.0f),
		-2.0f);
	TestEqual(
		TEXT("Avatar exposure keeps an in-range value"),
		AVPBroadcastOutput::SanitizeAvatarExposure(-0.7f),
		-0.7f);
	TestEqual(
		TEXT("Avatar exposure clamps high"),
		AVPBroadcastOutput::SanitizeAvatarExposure(3.0f),
		2.0f);
	TestEqual(
		TEXT("Invalid avatar exposure resets to neutral"),
		AVPBroadcastOutput::SanitizeAvatarExposure(std::numeric_limits<float>::quiet_NaN()),
		0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPAvatarIdentityValidationTest,
	"VPPipeline.Avatar.IdentityValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPAvatarIdentityValidationTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> FirstData = { 0x56, 0x52, 0x4d, 0x31 };
	const TArray<uint8> SecondData = { 0x56, 0x52, 0x4d, 0x32 };
	const FString FirstId = AVPAvatarManager::BuildAvatarId(FirstData);
	TestEqual(TEXT("BLAKE3 avatar ID has 64 characters"), FirstId.Len(), 64);
	TestEqual(
		TEXT("Same content produces the same avatar ID"),
		AVPAvatarManager::BuildAvatarId(FirstData),
		FirstId);
	TestNotEqual(
		TEXT("Different content produces a different avatar ID"),
		AVPAvatarManager::BuildAvatarId(SecondData),
		FirstId);
	TestTrue(TEXT("Generated avatar ID is valid"), AVPAvatarManager::IsValidAvatarId(FirstId));
	TestFalse(
		TEXT("Path traversal cannot be used as an avatar ID"),
		AVPAvatarManager::IsValidAvatarId(TEXT("../avatar")));
	TestTrue(
		TEXT("VRM extension is case-insensitive"),
		AVPAvatarManager::IsSupportedAvatarPath(TEXT("C:/Avatars/performer.VRM")));
	TestFalse(
		TEXT("Non-VRM extension is rejected"),
		AVPAvatarManager::IsSupportedAvatarPath(TEXT("C:/Avatars/performer.fbx")));
	const FString DuplicateNotice = AVPAvatarManager::BuildAlreadyRegisteredNotice(TEXT("MINI"));
	TestTrue(TEXT("Duplicate notice identifies the avatar"), DuplicateNotice.Contains(TEXT("MINI")));
	TestTrue(TEXT("Duplicate notice explains reuse"), DuplicateNotice.Contains(TEXT("기존 보관함 항목")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPAvatarActiveStateInvariantTest,
	"VPPipeline.Avatar.ActiveStateInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPAvatarActiveStateInvariantTest::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Only a complete runtime avatar state is active"),
		AVPAvatarManager::IsActiveAvatarStateComplete(true, true, true, true));
	TestFalse(
		TEXT("A stale AnimInstance without an avatar id is inactive"),
		AVPAvatarManager::IsActiveAvatarStateComplete(false, true, false, true));
	TestFalse(
		TEXT("An avatar id without a skeletal mesh is inactive"),
		AVPAvatarManager::IsActiveAvatarStateComplete(true, true, false, true));
	TestFalse(
		TEXT("A mesh without its runtime AssetList is inactive"),
		AVPAvatarManager::IsActiveAvatarStateComplete(true, false, true, true));
	TestFalse(
		TEXT("A mesh without a tracking AnimInstance is inactive"),
		AVPAvatarManager::IsActiveAvatarStateComplete(true, true, true, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPAvatarOrphanProfilePruningTest,
	"VPPipeline.Avatar.OrphanProfilePruning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPAvatarOrphanProfilePruningTest::RunTest(const FString& Parameters)
{
	const FString ValidAvatarId = FString::ChrN(64, TEXT('a'));
	const FString OrphanAvatarId = FString::ChrN(64, TEXT('b'));
	const TSet<FString> ValidAvatarIds = { ValidAvatarId };

	TMap<FString, FVPAvatarCameraSettings> CameraProfiles;
	CameraProfiles.Add(ValidAvatarId, FVPAvatarCameraSettings());
	CameraProfiles.Add(OrphanAvatarId, FVPAvatarCameraSettings());
	TestEqual(
		TEXT("One orphan camera profile is removed"),
		AVPBroadcastOutput::RemoveCameraProfilesNotIn(CameraProfiles, ValidAvatarIds),
		1);
	TestTrue(TEXT("Valid camera profile remains"), CameraProfiles.Contains(ValidAvatarId));
	TestFalse(TEXT("Orphan camera profile is gone"), CameraProfiles.Contains(OrphanAvatarId));

	TMap<FString, FVPAvatarTrackingProfile> TrackingProfiles;
	TrackingProfiles.Add(ValidAvatarId, FVPAvatarTrackingProfile());
	TrackingProfiles.Add(OrphanAvatarId, FVPAvatarTrackingProfile());
	TrackingProfiles.Add(TEXT("/Game/Legacy/AvatarMesh.AvatarMesh"), FVPAvatarTrackingProfile());
	TestEqual(
		TEXT("One orphan managed tracking profile is removed"),
		UVPAnimInstance::RemoveManagedTrackingProfilesNotIn(
			TrackingProfiles, ValidAvatarIds),
		1);
	TestTrue(TEXT("Valid managed tracking profile remains"), TrackingProfiles.Contains(ValidAvatarId));
	TestFalse(TEXT("Orphan managed tracking profile is gone"), TrackingProfiles.Contains(OrphanAvatarId));
	TestTrue(
		TEXT("Legacy asset-path profile is preserved"),
		TrackingProfiles.Contains(TEXT("/Game/Legacy/AvatarMesh.AvatarMesh")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPAvatarLibrarySerializationTest,
	"VPPipeline.Avatar.LibrarySerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPAvatarLibrarySerializationTest::RunTest(const FString& Parameters)
{
	UVPAvatarLibrarySaveGame* Source = NewObject<UVPAvatarLibrarySaveGame>();
	const FString AvatarId = FString::ChrN(64, TEXT('a'));
	FVPAvatarLibraryEntry Entry;
	Entry.AvatarId = AvatarId;
	Entry.DisplayName = TEXT("방송용 아바타");
	Source->Entries.Add(AvatarId, Entry);
	Source->LastSelectedAvatarId = AvatarId;

	TArray<uint8> Bytes;
	TestTrue(TEXT("Avatar library serializes"), UGameplayStatics::SaveGameToMemory(Source, Bytes));
	const UVPAvatarLibrarySaveGame* Loaded = Cast<UVPAvatarLibrarySaveGame>(
		UGameplayStatics::LoadGameFromMemory(Bytes));
	TestNotNull(TEXT("Avatar library deserializes"), Loaded);
	if (Loaded)
	{
		TestEqual(TEXT("Selected avatar ID persists"), Loaded->LastSelectedAvatarId, AvatarId);
		TestEqual(TEXT("Avatar entry persists"), Loaded->Entries.Num(), 1);
		TestEqual(
			TEXT("Avatar display name persists"),
			Loaded->Entries.FindChecked(AvatarId).DisplayName,
			FString(TEXT("방송용 아바타")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPAvatarCameraSerializationTest,
	"VPPipeline.Broadcast.PerAvatarCameraSerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPAvatarCameraSerializationTest::RunTest(const FString& Parameters)
{
	UVPBroadcastSettingsSaveGame* Source = NewObject<UVPBroadcastSettingsSaveGame>();
	Source->BackgroundMode = EVPBroadcastBackgroundMode::SolidColor;
	Source->BackgroundColor = FLinearColor(0.2f, 0.3f, 0.4f, 1.0f);
	Source->AvatarExposureStops = -0.7f;
	Source->OutputFramesPerSecond = 48;
	FVPAvatarCameraSettings Camera;
	Camera.bHasSavedView = true;
	Camera.RelativeTransform = FTransform(
		FRotator(-4.0, 181.0, 0.0), FVector(150.0, -20.0, 135.0));
	Camera.FieldOfView = 52.0f;
	Camera.bHasOrbitPivot = true;
	Camera.RelativeOrbitPivot = FVector(5.0f, 12.0f, 130.0f);
	Source->AvatarCameraSettings.Add(TEXT("avatar-a"), Camera);

	TArray<uint8> Bytes;
	TestTrue(TEXT("Broadcast settings serialize"), UGameplayStatics::SaveGameToMemory(Source, Bytes));
	const UVPBroadcastSettingsSaveGame* Loaded = Cast<UVPBroadcastSettingsSaveGame>(
		UGameplayStatics::LoadGameFromMemory(Bytes));
	TestNotNull(TEXT("Broadcast settings deserialize"), Loaded);
	if (Loaded)
	{
		TestEqual(TEXT("Background mode persists"), Loaded->BackgroundMode, EVPBroadcastBackgroundMode::SolidColor);
		TestTrue(TEXT("Background color persists"), Loaded->BackgroundColor.Equals(Source->BackgroundColor));
		TestEqual(TEXT("Avatar exposure persists"), Loaded->AvatarExposureStops, Source->AvatarExposureStops);
		TestEqual(TEXT("Output FPS persists"), Loaded->OutputFramesPerSecond, 48);
		const FVPAvatarCameraSettings* LoadedCamera = Loaded->AvatarCameraSettings.Find(TEXT("avatar-a"));
		TestNotNull(TEXT("Avatar-specific camera persists"), LoadedCamera);
		if (LoadedCamera)
		{
			TestTrue(TEXT("Camera transform persists"), LoadedCamera->RelativeTransform.Equals(Camera.RelativeTransform));
			TestEqual(TEXT("Camera FOV persists"), LoadedCamera->FieldOfView, Camera.FieldOfView);
			TestTrue(TEXT("Camera orbit pivot flag persists"), LoadedCamera->bHasOrbitPivot);
			TestTrue(
				TEXT("Camera orbit pivot persists"),
				LoadedCamera->RelativeOrbitPivot.Equals(Camera.RelativeOrbitPivot));
		}
	}
	TestEqual(TEXT("Output FPS clamps low"), AVPBroadcastOutput::SanitizeOutputFPS(1), 15);
	TestEqual(TEXT("Output FPS keeps valid value"), AVPBroadcastOutput::SanitizeOutputFPS(72), 72);
	TestEqual(TEXT("Output FPS clamps high"), AVPBroadcastOutput::SanitizeOutputFPS(240), 144);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPBroadcastInitialCameraDirectionTest,
	"VPPipeline.Broadcast.InitialCameraDirection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPBroadcastInitialCameraDirectionTest::RunTest(const FString& Parameters)
{
	const FRotator ReferenceRotation(18.0, 35.0, 12.0);
	const FVector CenteredBoundsOrigin(0.0, 0.0, 100.0);
	const FVector OffsetBoundsOrigin(240.0, -175.0, 100.0);
	constexpr float CameraDistance = 500.0f;

	const FTransform CenteredTransform = AVPBroadcastOutput::CalculateInitialCameraTransform(
		CenteredBoundsOrigin,
		CameraDistance,
		ReferenceRotation);
	const FTransform OffsetTransform = AVPBroadcastOutput::CalculateInitialCameraTransform(
		OffsetBoundsOrigin,
		CameraDistance,
		ReferenceRotation);
	const FRotator CenteredRotation = CenteredTransform.Rotator();
	const FRotator OffsetRotation = OffsetTransform.Rotator();

	TestTrue(
		TEXT("Lateral bounds offset does not alter camera orientation"),
		CenteredRotation.Equals(OffsetRotation, 0.001f));
	TestTrue(TEXT("Initial camera pitch is level"), FMath::IsNearlyZero(CenteredRotation.Pitch, 0.001f));
	TestTrue(TEXT("Initial camera roll is level"), FMath::IsNearlyZero(CenteredRotation.Roll, 0.001f));
	TestTrue(TEXT("Initial camera keeps reference yaw"), FMath::IsNearlyEqual(CenteredRotation.Yaw, 35.0f, 0.001f));
	TestTrue(
		TEXT("Bounds offset only translates the camera"),
		(OffsetTransform.GetLocation() - CenteredTransform.GetLocation()).Equals(
			OffsetBoundsOrigin - CenteredBoundsOrigin,
			0.001f));

	const FVector AsymmetricBoundsOrigin(40.0, 180.0, 100.0);
	const FVector HumanoidBodyAnchor(25.0, 20.0, 145.0);
	const FVector FramingOrigin = AVPBroadcastOutput::CalculateBodyCenteredFramingOrigin(
		AsymmetricBoundsOrigin,
		HumanoidBodyAnchor,
		FRotator::ZeroRotator);
	TestTrue(
		TEXT("Body anchor controls horizontal framing center"),
		FramingOrigin.Equals(FVector(40.0, 20.0, 100.0), 0.001f));
	TestTrue(
		TEXT("Body centering preserves bounds vertical center"),
		FMath::IsNearlyEqual(FramingOrigin.Z, AsymmetricBoundsOrigin.Z, 0.001f));

	const FRotator FrontRotation = AVPBroadcastOutput::GetAvatarFrontCameraRotation();
	TestTrue(
		TEXT("VRM front camera uses the normalized front yaw"),
		FMath::IsNearlyEqual(FrontRotation.Yaw, -90.0f, 0.001f));
	const FTransform FrontTransform = AVPBroadcastOutput::CalculateInitialCameraTransform(
		CenteredBoundsOrigin,
		CameraDistance,
		FrontRotation);
	TestTrue(
		TEXT("VRM front camera is placed on the avatar-facing side"),
		FrontTransform.GetLocation().Equals(FVector(0.0f, CameraDistance, 100.0f), 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPAvatarHumanoidFramingBoundsTest,
	"VPPipeline.Broadcast.HumanoidFramingBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPAvatarHumanoidFramingBoundsTest::RunTest(const FString& Parameters)
{
	const FBox PathologicalRenderBounds(
		FVector(-900.0f, -500.0f, -400.0f),
		FVector(1200.0f, 700.0f, 1600.0f));
	FBox FramingBounds;
	TestTrue(
		TEXT("Valid humanoid landmarks produce framing bounds"),
		AVPAvatarManager::CalculateHumanoidFramingBounds(
			PathologicalRenderBounds,
			FVector(0.0f, 0.0f, 160.0f),
			FVector(-70.0f, 0.0f, 100.0f),
			FVector(70.0f, 0.0f, 100.0f),
			FVector(-10.0f, 0.0f, 0.0f),
			FVector(10.0f, 0.0f, 0.0f),
			FramingBounds));
	TestTrue(
		TEXT("Pathological horizontal render bounds are capped"),
		FramingBounds.Min.X >= -104.01f && FramingBounds.Max.X <= 104.01f);
	TestTrue(
		TEXT("Pathological vertical render bounds are capped with accessory headroom"),
		FMath::IsNearlyEqual(FramingBounds.Min.Z, -12.8f, 0.01f) &&
		FMath::IsNearlyEqual(FramingBounds.Max.Z, 216.0f, 0.01f));
	TestTrue(
		TEXT("Humanoid framing is vertically centered around the capped body range"),
		FMath::IsNearlyEqual(FramingBounds.GetCenter().Z, 101.6f, 0.01f));

	FBox InvalidBounds;
	TestFalse(
		TEXT("Degenerate head-to-foot height falls back to actor bounds"),
		AVPAvatarManager::CalculateHumanoidFramingBounds(
			PathologicalRenderBounds,
			FVector::ZeroVector,
			FVector::ZeroVector,
			FVector::ZeroVector,
			FVector::ZeroVector,
			FVector::ZeroVector,
			InvalidBounds));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPBroadcastCameraOrbitTest,
	"VPPipeline.Broadcast.CameraOrbit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPBroadcastCameraOrbitTest::RunTest(const FString& Parameters)
{
	const FVector Pivot(0.0f, 0.0f, 120.0f);
	const FTransform Initial = AVPBroadcastOutput::CalculateInitialCameraTransform(
		Pivot,
		500.0f,
		AVPBroadcastOutput::GetAvatarFrontCameraRotation());
	const FTransform Orbited = AVPBroadcastOutput::CalculateOrbitCameraTransform(
		Initial,
		Pivot,
		FVector2D(120.0f, -40.0f));

	TestTrue(
		TEXT("Orbit preserves distance from pivot"),
		FMath::IsNearlyEqual(
			FVector::Distance(Orbited.GetLocation(), Pivot),
			FVector::Distance(Initial.GetLocation(), Pivot),
			0.01f));
	const FVector ExpectedView = (Pivot - Orbited.GetLocation()).GetSafeNormal();
	TestTrue(
		TEXT("Orbit camera keeps looking at pivot"),
		Orbited.GetRotation().GetForwardVector().Equals(ExpectedView, 0.001f));
	TestFalse(
		TEXT("Horizontal orbit changes camera yaw"),
		FMath::IsNearlyEqual(Orbited.Rotator().Yaw, Initial.Rotator().Yaw, 0.001f));

	const FTransform PitchClamped = AVPBroadcastOutput::CalculateOrbitCameraTransform(
		Initial,
		Pivot,
		FVector2D(0.0f, 10000.0f));
	TestTrue(
		TEXT("Vertical orbit pitch is clamped"),
		FMath::IsNearlyEqual(PitchClamped.Rotator().Pitch, 60.0f, 0.01f));
	return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VPAnimInstance.h"
#include "VPUDPReceiver.h"
#include "Kismet/GameplayStatics.h"

namespace VPUDPReceiverTests
{
template <typename T>
void AppendValue(TArray<uint8>& Data, const T& Value)
{
	const int32 Offset = Data.AddUninitialized(sizeof(T));
	FMemory::Memcpy(Data.GetData() + Offset, &Value, sizeof(T));
}

TArray<uint8> BuildValidPacket()
{
	TArray<uint8> Data;
	Data.Append({ 'V', 'P', 'T', 'P' });
	AppendValue(Data, static_cast<uint8>(3));
	AppendValue(Data, static_cast<uint8>(3));
	AppendValue(Data, static_cast<uint16>(0));
	AppendValue(Data, static_cast<uint32>(42));
	AppendValue(Data, 1234.5);
	AppendValue(Data, static_cast<uint16>(52));
	for (int32 Index = 0; Index < 52; ++Index)
	{
		AppendValue(Data, Index == 9 ? 0.75f : 0.0f);
	}
	const float IdentityFaceRotation[9] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	};
	for (const float Value : IdentityFaceRotation)
	{
		AppendValue(Data, Value);
	}
	AppendValue(Data, static_cast<uint16>(33));
	for (int32 Index = 0; Index < 33; ++Index)
	{
		AppendValue(Data, 0.5f);
		AppendValue(Data, 0.25f);
		AppendValue(Data, 0.0f);
		AppendValue(Data, 0.9f);
		AppendValue(Data, 0.8f);
	}
	return Data;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUDPReceiverValidPacketTest,
	"VPPipeline.UDP.Protocol.ValidPacket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUDPReceiverValidPacketTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Packet = VPUDPReceiverTests::BuildValidPacket();
	FVPTrackingFrame Frame;
	TestEqual(TEXT("VPTP schema-3 packet size"), Packet.Num(), 928);
	TestTrue(TEXT("Valid packet decodes"), UVPUDPReceiver::DecodePacket(Packet.GetData(), Packet.Num(), Frame));
	TestTrue(TEXT("Frame is valid"), Frame.bIsValid);
	TestTrue(TEXT("Face flag"), Frame.bFaceTracked);
	TestTrue(TEXT("Pose flag"), Frame.bPoseTracked);
	TestEqual(TEXT("Frame id"), Frame.FrameId, static_cast<int64>(42));
	TestEqual(TEXT("Blendshape count"), Frame.FaceData.Blendshapes.Num(), 52);
	TestEqual(TEXT("Pose count"), Frame.PoseLandmarks.Num(), 33);
	TestTrue(TEXT("Identity face rotation decodes"), Frame.FaceRotation.IsNearlyZero(0.001f));

	TArray<uint8> YawPacket = VPUDPReceiverTests::BuildValidPacket();
	const float YawRadians = FMath::DegreesToRadians(30.0f);
	const float YawMatrix[9] = {
		FMath::Cos(YawRadians), 0.0f, FMath::Sin(YawRadians),
		0.0f, 1.0f, 0.0f,
		-FMath::Sin(YawRadians), 0.0f, FMath::Cos(YawRadians),
	};
	FMemory::Memcpy(YawPacket.GetData() + 230, YawMatrix, sizeof(YawMatrix));
	TestTrue(TEXT("Face-yaw packet decodes"),
		UVPUDPReceiver::DecodePacket(YawPacket.GetData(), YawPacket.Num(), Frame));
	TestTrue(TEXT("Face matrix yields semantic yaw"),
		FMath::IsNearlyEqual(Frame.FaceRotation.Yaw, 30.0f, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUDPReceiverMalformedPacketTest,
	"VPPipeline.UDP.Protocol.MalformedPackets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUDPReceiverMalformedPacketTest::RunTest(const FString& Parameters)
{
	FVPTrackingFrame Frame;
	const TArray<uint8> Valid = VPUDPReceiverTests::BuildValidPacket();

	TArray<uint8> Truncated = Valid;
	Truncated.Pop();
	TestFalse(TEXT("Truncated packet rejected"),
		UVPUDPReceiver::DecodePacket(Truncated.GetData(), Truncated.Num(), Frame));

	TArray<uint8> Extra = Valid;
	Extra.Add(0);
	TestFalse(TEXT("Extra byte rejected"),
		UVPUDPReceiver::DecodePacket(Extra.GetData(), Extra.Num(), Frame));

	TArray<uint8> WrongMagic = Valid;
	WrongMagic[2] = 'F';
	TestFalse(TEXT("Wrong packet identifier rejected"),
		UVPUDPReceiver::DecodePacket(WrongMagic.GetData(), WrongMagic.Num(), Frame));

	TArray<uint8> WrongVersion = Valid;
	WrongVersion[4] = 2;
	TestFalse(TEXT("Old schema rejected"),
		UVPUDPReceiver::DecodePacket(WrongVersion.GetData(), WrongVersion.Num(), Frame));

	TArray<uint8> WrongCount = Valid;
	const uint16 Count = 51;
	FMemory::Memcpy(WrongCount.GetData() + 20, &Count, sizeof(Count));
	TestFalse(TEXT("Wrong blendshape count rejected"),
		UVPUDPReceiver::DecodePacket(WrongCount.GetData(), WrongCount.Num(), Frame));

	TArray<uint8> NotFinite = Valid;
	const uint32 NaNBits = 0x7FC00000;
	float NaN = 0.0f;
	FMemory::Memcpy(&NaN, &NaNBits, sizeof(NaN));
	FMemory::Memcpy(NotFinite.GetData() + 22, &NaN, sizeof(NaN));
	TestFalse(TEXT("NaN rejected"),
		UVPUDPReceiver::DecodePacket(NotFinite.GetData(), NotFinite.Num(), Frame));

	TArray<uint8> Infinity = Valid;
	const uint32 InfBits = 0x7F800000;
	float Inf = 0.0f;
	FMemory::Memcpy(&Inf, &InfBits, sizeof(Inf));
	FMemory::Memcpy(Infinity.GetData() + 22, &Inf, sizeof(Inf));
	TestFalse(TEXT("Inf rejected"),
		UVPUDPReceiver::DecodePacket(Infinity.GetData(), Infinity.Num(), Frame));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUDPReceiverLatencyStatisticsTest,
	"VPPipeline.UDP.Transport.LatencyStatistics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUDPReceiverLatencyStatisticsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Zero latency is valid"), UVPUDPReceiver::IsLatencySampleValid(0.0));
	TestTrue(TEXT("Ten-second guard boundary is valid"),
		UVPUDPReceiver::IsLatencySampleValid(10000.0));
	TestFalse(TEXT("Negative latency is rejected"),
		UVPUDPReceiver::IsLatencySampleValid(-0.01));
	TestFalse(TEXT("Clock-domain mismatch is rejected"),
		UVPUDPReceiver::IsLatencySampleValid(10000.01));
	const uint64 DoubleNaNBits = 0x7FF8000000000000ULL;
	double DoubleNaN = 0.0;
	FMemory::Memcpy(&DoubleNaN, &DoubleNaNBits, sizeof(DoubleNaN));
	TestFalse(TEXT("Non-finite latency is rejected"),
		UVPUDPReceiver::IsLatencySampleValid(DoubleNaN));

	TArray<float> Samples;
	for (int32 Value = 1; Value <= 100; ++Value)
	{
		Samples.Add(static_cast<float>(Value));
	}
	TestEqual(TEXT("Nearest-rank P95 is calculated"),
		UVPUDPReceiver::CalculatePercentile95(Samples), 95.0f);
	const TArray<float> EmptySamples;
	TestEqual(TEXT("Empty P95 is zero"),
		UVPUDPReceiver::CalculatePercentile95(EmptySamples), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUDPReceiverLatestFrameMailboxTest,
	"VPPipeline.UDP.Transport.LatestFrameMailbox",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUDPReceiverLatestFrameMailboxTest::RunTest(const FString& Parameters)
{
	FVPTrackingLatestFrameMailbox Mailbox;
	FVPTrackingFrame Output;
	TestFalse(TEXT("Empty mailbox has no frame"), Mailbox.Pop(Output));

	FVPTrackingFrame First;
	First.FrameId = 1;
	First.Timestamp = 100.0;
	TestFalse(TEXT("First frame fills an empty mailbox"), Mailbox.Push(First));

	FVPTrackingFrame Newest;
	Newest.FrameId = 2;
	Newest.Timestamp = 200.0;
	TestTrue(TEXT("Newest frame supersedes an undelivered frame"), Mailbox.Push(Newest));
	TestTrue(TEXT("Latest frame is available"), Mailbox.Pop(Output));
	TestEqual(TEXT("Mailbox delivers only the newest frame"), Output.FrameId, static_cast<int64>(2));
	TestEqual(TEXT("Newest timestamp is retained"), Output.Timestamp, 200.0);
	TestFalse(TEXT("Mailbox is empty after one delivery"), Mailbox.Pop(Output));

	Mailbox.Push(First);
	Mailbox.Reset();
	TestFalse(TEXT("Reset clears a pending frame"), Mailbox.Pop(Output));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUpperArmAngleTest,
	"VPPipeline.Tracking.UpperArmAngle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUpperArmAngleTest::RunTest(const FString& Parameters)
{
	FVPTrackingFrame Frame;
	Frame.bIsValid = true;
	Frame.bPoseTracked = true;
	Frame.PoseLandmarks.SetNum(33);
	for (FVPPoseLandmark& Landmark : Frame.PoseLandmarks)
	{
		Landmark.Visibility = 0.9f;
		Landmark.Presence = 0.8f;
	}
	Frame.PoseLandmarks[11].Position = FVector(0.5f, 0.4f, 0.0f);
	// MediaPipe anatomical left appears on the image right in an unmirrored frame.
	Frame.PoseLandmarks[13].Position = FVector(0.7f, 0.4f, 0.0f);
	Frame.PoseLandmarks[12].Position = FVector(0.5f, 0.4f, 0.0f);
	Frame.PoseLandmarks[14].Position = FVector(0.3f, 0.4f, 0.0f);

	float Angle = 0.0f;
	float Confidence = 0.0f;
	TestTrue(TEXT("Left arm is tracked"),
		UVPAnimInstance::CalculateUpperArmAngle(Frame, true, 0.5f, Angle, Confidence));
	TestEqual(TEXT("Left horizontal angle"), Angle, 90.0f);
	TestEqual(TEXT("Left confidence"), Confidence, 0.8f);
	TestTrue(TEXT("Right arm is tracked"),
		UVPAnimInstance::CalculateUpperArmAngle(Frame, false, 0.5f, Angle, Confidence));
	TestEqual(TEXT("Right horizontal angle"), Angle, 90.0f);

	Frame.PoseLandmarks[14].Visibility = 0.1f;
	TestFalse(TEXT("Low-confidence arm is rejected"),
		UVPAnimInstance::CalculateUpperArmAngle(Frame, false, 0.5f, Angle, Confidence));

	Frame.PoseLandmarks[13].Position = FVector(0.4f, 0.4f, -0.15f);
	float ForwardAngle = 0.0f;
	bool bInward = false;
	TestTrue(TEXT("Inward left arm still produces a safe target"),
		UVPAnimInstance::CalculateUpperArmPose(
			Frame, true, 0.5f, Angle, ForwardAngle, Confidence, bInward));
	TestTrue(TEXT("Cross-body input is identified"), bInward);
	TestTrue(TEXT("Cross-body height is kept out of the torso"), Angle >= 0.0f);
	TestTrue(TEXT("Cross-body arm is biased in front"), ForwardAngle >= 30.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPHeadRollIsolationTest,
	"VPPipeline.Tracking.HeadRollIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPHeadRollIsolationTest::RunTest(const FString& Parameters)
{
	FVPTrackingFrame Frame;
	Frame.bIsValid = true;
	Frame.bPoseTracked = true;
	Frame.PoseLandmarks.SetNum(33);
	for (FVPPoseLandmark& Landmark : Frame.PoseLandmarks)
	{
		Landmark.Visibility = 0.9f;
		Landmark.Presence = 0.9f;
	}

	// A deliberately tilted shoulder line represents a one-arm-raised pose.
	Frame.PoseLandmarks[11].Position = FVector(0.65f, 0.25f, 0.0f);
	Frame.PoseLandmarks[12].Position = FVector(0.35f, 0.55f, 0.0f);
	Frame.PoseLandmarks[2].Position = FVector(0.58f, 0.20f, 0.0f);
	Frame.PoseLandmarks[5].Position = FVector(0.42f, 0.20f, 0.0f);

	float RollDegrees = 0.0f;
	TestTrue(
		TEXT("Level eyes provide head roll while one shoulder is raised"),
		UVPAnimInstance::CalculateHeadRollDegrees(Frame, 0.3f, RollDegrees));
	TestTrue(
		TEXT("Shoulder tilt does not contaminate head roll"),
		FMath::IsNearlyZero(RollDegrees, 0.001f));

	Frame.PoseLandmarks[5].Position.Y = 0.28f;
	TestTrue(
		TEXT("Actual eye-line tilt is measured"),
		UVPAnimInstance::CalculateHeadRollDegrees(Frame, 0.3f, RollDegrees));
	TestTrue(TEXT("Eye-line tilt produces a non-zero roll"), FMath::Abs(RollDegrees) > 20.0f);

	Frame.PoseLandmarks[2].Visibility = 0.1f;
	Frame.PoseLandmarks[7].Position = FVector(0.62f, 0.24f, 0.0f);
	Frame.PoseLandmarks[8].Position = FVector(0.38f, 0.24f, 0.0f);
	TestTrue(
		TEXT("Ear line is used when an eye is unreliable"),
		UVPAnimInstance::CalculateHeadRollDegrees(Frame, 0.3f, RollDegrees));
	TestTrue(TEXT("Level ears provide zero fallback roll"), FMath::IsNearlyZero(RollDegrees, 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUpperArmProfileMappingTest,
	"VPPipeline.Tracking.UpperArmProfileMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUpperArmProfileMappingTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Neutral calibration removes the rest angle"),
		UVPAnimInstance::MapUpperArmOutput(35.0f, 35.0f, 1.0f, false, -30.0f, 140.0f),
		0.0f);
	TestEqual(
		TEXT("Gain is applied after neutral subtraction"),
		UVPAnimInstance::MapUpperArmOutput(75.0f, 35.0f, 1.5f, false, -30.0f, 140.0f),
		60.0f);
	TestEqual(
		TEXT("Default outward lift is not trapped at the negative clamp"),
		UVPAnimInstance::MapUpperArmOutput(90.0f, 0.0f, 1.0f, false, -30.0f, 140.0f),
		90.0f);
	TestEqual(
		TEXT("Inversion and lower clamp are applied"),
		UVPAnimInstance::MapUpperArmOutput(95.0f, 35.0f, 1.0f, true, -30.0f, 140.0f),
		-30.0f);
	TestEqual(
		TEXT("Angle wrapping uses the shortest direction"),
		UVPAnimInstance::MapUpperArmOutput(-170.0f, 170.0f, 1.0f, false, -30.0f, 140.0f),
		20.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUpperArmInputFilterTest,
	"VPPipeline.Tracking.UpperArmInputFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUpperArmInputFilterTest::RunTest(const FString& Parameters)
{
	float Filtered = 0.0f;
	float LastRaw = 0.0f;
	int32 RejectedFrames = 0;
	bool bInitialized = false;
	float Output = 0.0f;

	TestTrue(TEXT("First sample initializes the filter"),
		UVPAnimInstance::FilterUpperArmInputStep(
			0.0f, 1.0f / 60.0f, 12.0f, 60.0f,
			Filtered, LastRaw, RejectedFrames, bInitialized, Output));
	TestFalse(TEXT("First large jump is held"),
		UVPAnimInstance::FilterUpperArmInputStep(
			100.0f, 1.0f / 60.0f, 12.0f, 60.0f,
			Filtered, LastRaw, RejectedFrames, bInitialized, Output));
	TestEqual(TEXT("Held output remains stable"), Output, 0.0f);
	TestFalse(TEXT("Second large jump is held"),
		UVPAnimInstance::FilterUpperArmInputStep(
			100.0f, 1.0f / 60.0f, 12.0f, 60.0f,
			Filtered, LastRaw, RejectedFrames, bInitialized, Output));
	TestTrue(TEXT("Persistent third sample is accepted"),
		UVPAnimInstance::FilterUpperArmInputStep(
			100.0f, 1.0f / 60.0f, 12.0f, 60.0f,
			Filtered, LastRaw, RejectedFrames, bInitialized, Output));
	TestEqual(TEXT("Persistent input becomes the new baseline"), Output, 100.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUpperArmConfidenceHysteresisTest,
	"VPPipeline.Tracking.UpperArmConfidenceHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUpperArmConfidenceHysteresisTest::RunTest(const FString& Parameters)
{
	bool bTracked = false;
	bool bFreshSample = false;
	float BelowThresholdSeconds = 0.0f;
	bTracked = UVPAnimInstance::UpdateArmTrackingStateStep(
		bTracked, true, 0.8f, 0.1f, 0.5f, 0.3f, 0.25f,
		BelowThresholdSeconds, bFreshSample);
	TestTrue(TEXT("High confidence acquires tracking"), bTracked);
	TestTrue(TEXT("Acquired sample is fresh"), bFreshSample);

	bTracked = UVPAnimInstance::UpdateArmTrackingStateStep(
		bTracked, true, 0.1f, 0.1f, 0.5f, 0.3f, 0.25f,
		BelowThresholdSeconds, bFreshSample);
	TestTrue(TEXT("Short confidence drop is held"), bTracked);
	TestFalse(TEXT("Held sample is not fresh"), bFreshSample);
	bTracked = UVPAnimInstance::UpdateArmTrackingStateStep(
		bTracked, false, 0.0f, 0.16f, 0.5f, 0.3f, 0.25f,
		BelowThresholdSeconds, bFreshSample);
	TestFalse(TEXT("Tracking releases after grace"), bTracked);

	bTracked = UVPAnimInstance::UpdateArmTrackingStateStep(
		bTracked, true, 0.4f, 0.1f, 0.5f, 0.3f, 0.25f,
		BelowThresholdSeconds, bFreshSample);
	TestFalse(TEXT("Release threshold alone cannot reacquire"), bTracked);
	bTracked = UVPAnimInstance::UpdateArmTrackingStateStep(
		bTracked, true, 0.6f, 0.1f, 0.5f, 0.3f, 0.25f,
		BelowThresholdSeconds, bFreshSample);
	TestTrue(TEXT("Acquire threshold restores tracking"), bTracked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPTimeBasedSmoothingTest,
	"VPPipeline.Tracking.TimeBasedSmoothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPTimeBasedSmoothingTest::RunTest(const FString& Parameters)
{
	const TArray<float> WrappedSamples = { 179.0f, -179.0f, 180.0f };
	const float MeanAngle = UVPAnimInstance::CalculateCircularMeanDegrees(WrappedSamples);
	TestTrue(TEXT("Circular mean stays at the wrap boundary"),
		FMath::Abs(FMath::Abs(MeanAngle) - 180.0f) < 0.1f);

	FRotator At30Fps = FRotator::ZeroRotator;
	FRotator At120Fps = FRotator::ZeroRotator;
	const FRotator Target(0.0f, 90.0f, 0.0f);
	for (int32 Index = 0; Index < 30; ++Index)
	{
		At30Fps = UVPAnimInstance::SmoothHeadRotationStep(
			At30Fps, Target, 1.0f / 30.0f, 8.0f, 720.0f);
	}
	for (int32 Index = 0; Index < 120; ++Index)
	{
		At120Fps = UVPAnimInstance::SmoothHeadRotationStep(
			At120Fps, Target, 1.0f / 120.0f, 8.0f, 720.0f);
	}
	TestTrue(TEXT("One-second smoothing is frame-rate independent"),
		FMath::Abs(At30Fps.Yaw - At120Fps.Yaw) < 0.5f);

	const FRotator DeadZoned = UVPAnimInstance::ApplyHeadRotationDeadZone(
		FRotator(1.0f, -3.0f, 2.0f), 1.5f);
	TestTrue(TEXT("Head micro-motion is removed"),
		FMath::IsNearlyZero(DeadZoned.Pitch, 0.001f));
	TestTrue(TEXT("Head yaw remains continuous outside dead zone"),
		FMath::IsNearlyEqual(DeadZoned.Yaw, -1.5f, 0.001f));
	TestTrue(TEXT("Head roll subtracts the dead-zone edge"),
		FMath::IsNearlyEqual(DeadZoned.Roll, 0.5f, 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPArmValidationPoseTest,
	"VPPipeline.Tracking.ArmValidationPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPArmValidationPoseTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Neutral stage accepts both arms near zero"),
		UVPAnimInstance::IsArmValidationPoseSatisfied(
			EVPArmValidationStage::Neutral, 5.0f, -8.0f, 15.0f, 35.0f, 25.0f));
	TestTrue(TEXT("Left stage accepts only the left arm raised"),
		UVPAnimInstance::IsArmValidationPoseSatisfied(
			EVPArmValidationStage::LeftArmRaised, 60.0f, 8.0f, 15.0f, 35.0f, 25.0f));
	TestFalse(TEXT("Left stage rejects both arms raised"),
		UVPAnimInstance::IsArmValidationPoseSatisfied(
			EVPArmValidationStage::LeftArmRaised, 60.0f, 55.0f, 15.0f, 35.0f, 25.0f));
	TestTrue(TEXT("Right stage accepts negative mapped rotation magnitude"),
		UVPAnimInstance::IsArmValidationPoseSatisfied(
			EVPArmValidationStage::RightArmRaised, 5.0f, -50.0f, 15.0f, 35.0f, 25.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPArmValidationFeedbackTest,
	"VPPipeline.Tracking.ArmValidationFeedback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPArmValidationFeedbackTest::RunTest(const FString& Parameters)
{
	const FString LowConfidence = UVPAnimInstance::BuildArmValidationFeedback(
		EVPArmValidationStage::LeftArmRaised,
		false,
		true,
		0.0f,
		0.0f,
		0.36f,
		0.8f,
		0.5f,
		15.0f,
		35.0f,
		25.0f);
	TestTrue(TEXT("Low confidence identifies the user's left elbow"), LowConfidence.Contains(TEXT("왼쪽")));
	TestTrue(TEXT("Low confidence preserves the measured value"), LowConfidence.Contains(TEXT("0.36")));

	const FString RaiseLeft = UVPAnimInstance::BuildArmValidationFeedback(
		EVPArmValidationStage::LeftArmRaised,
		true,
		true,
		24.0f,
		5.0f,
		0.8f,
		0.8f,
		0.5f,
		15.0f,
		35.0f,
		25.0f);
	TestTrue(TEXT("Low left angle tells the user to raise it"), RaiseLeft.Contains(TEXT("더 드세요")));
	TestTrue(TEXT("Low left angle shows the required angle"), RaiseLeft.Contains(TEXT("35°")));

	const FString LowerOtherArm = UVPAnimInstance::BuildArmValidationFeedback(
		EVPArmValidationStage::LeftArmRaised,
		true,
		true,
		50.0f,
		31.0f,
		0.8f,
		0.8f,
		0.5f,
		15.0f,
		35.0f,
		25.0f);
	TestTrue(TEXT("Raised opposite arm is identified"), LowerOtherArm.Contains(TEXT("오른팔")));
	TestTrue(TEXT("Raised opposite arm gets an actionable correction"), LowerOtherArm.Contains(TEXT("내리세요")));

	const FString HoldPose = UVPAnimInstance::BuildArmValidationFeedback(
		EVPArmValidationStage::LeftArmRaised,
		true,
		true,
		50.0f,
		5.0f,
		0.8f,
		0.8f,
		0.5f,
		15.0f,
		35.0f,
		25.0f);
	TestTrue(TEXT("Satisfied pose tells the user to hold"), HoldPose.Contains(TEXT("그대로 유지")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPTrackingProfileSerializationTest,
	"VPPipeline.Tracking.ProfileSerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPTrackingProfileSerializationTest::RunTest(const FString& Parameters)
{
	UVPTrackingProfileSaveGame* Source = NewObject<UVPTrackingProfileSaveGame>();
	FVPAvatarTrackingProfile Profile;
	Profile.bSwapUpperArms = true;
	Profile.LeftGain = 1.25f;
	Profile.bHasNeutralCalibration = true;
	Profile.NeutralCalibrationVersion = 2;
	Profile.LeftNeutralInputAngle = 12.0f;
	Profile.RightNeutralInputAngle = -8.0f;
	Profile.LeftNeutralForwardAngle = 3.0f;
	Profile.RightNeutralForwardAngle = -2.0f;
	Source->Profiles.Add(TEXT("/Game/TestAvatar"), Profile);

	TArray<uint8> Bytes;
	TestTrue(TEXT("Profile SaveGame serializes to memory"),
		UGameplayStatics::SaveGameToMemory(Source, Bytes));
	UVPTrackingProfileSaveGame* Loaded = Cast<UVPTrackingProfileSaveGame>(
		UGameplayStatics::LoadGameFromMemory(Bytes));
	TestNotNull(TEXT("Profile SaveGame deserializes"), Loaded);
	if (!Loaded)
	{
		return false;
	}

	const FVPAvatarTrackingProfile* LoadedProfile = Loaded->Profiles.Find(TEXT("/Game/TestAvatar"));
	TestNotNull(TEXT("Avatar profile key survives serialization"), LoadedProfile);
	if (!LoadedProfile)
	{
		return false;
	}

	TestTrue(TEXT("Swap setting survives serialization"), LoadedProfile->bSwapUpperArms);
	TestEqual(TEXT("Gain survives serialization"), LoadedProfile->LeftGain, 1.25f);
	TestTrue(TEXT("Calibration flag survives serialization"), LoadedProfile->bHasNeutralCalibration);
	TestEqual(TEXT("Calibration revision survives serialization"), LoadedProfile->NeutralCalibrationVersion, 2);
	TestEqual(TEXT("Left neutral survives serialization"), LoadedProfile->LeftNeutralInputAngle, 12.0f);
	TestEqual(TEXT("Right neutral survives serialization"), LoadedProfile->RightNeutralInputAngle, -8.0f);
	TestEqual(TEXT("Left forward neutral survives serialization"), LoadedProfile->LeftNeutralForwardAngle, 3.0f);
	TestEqual(TEXT("Right forward neutral survives serialization"), LoadedProfile->RightNeutralForwardAngle, -2.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVPUDPReceiverParserBenchmark,
	"VPPipeline.Performance.UDPParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVPUDPReceiverParserBenchmark::RunTest(const FString& Parameters)
{
	constexpr int32 Iterations = 100000;
	const TArray<uint8> Packet = VPUDPReceiverTests::BuildValidPacket();
	FVPTrackingFrame Frame;
	const uint64 StartCycles = FPlatformTime::Cycles64();
	for (int32 Index = 0; Index < Iterations; ++Index)
	{
		if (!UVPUDPReceiver::DecodePacket(Packet.GetData(), Packet.Num(), Frame))
		{
			AddError(FString::Printf(TEXT("Decode failed at iteration %d"), Index));
			return false;
		}
	}
	const double ElapsedSeconds = FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StartCycles);
	const double MicrosecondsPerPacket = ElapsedSeconds * 1000000.0 / Iterations;
	AddInfo(FString::Printf(
		TEXT("Decoded %d VPTP schema-3 packets in %.6f s (%.2f packets/s, %.3f us/packet)"),
		Iterations,
		ElapsedSeconds,
		Iterations / ElapsedSeconds,
		MicrosecondsPerPacket));
	return true;
}

#endif

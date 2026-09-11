#include "VPUDPReceiver.h"
#include "Common/UdpSocketBuilder.h"
#include "Misc/ScopeLock.h"

// Virtual Production Tracking Packet (VPTP), schema 3. Matches vp-tracker/protocol.py.
static constexpr uint8 PACKET_MAGIC[] = { 'V', 'P', 'T', 'P' };
static constexpr uint8 PACKET_SCHEMA_VERSION = 3;
static constexpr uint8 FACE_TRACKED_FLAG = 1 << 0;
static constexpr uint8 POSE_TRACKED_FLAG = 1 << 1;
static constexpr uint8 KNOWN_FLAGS = FACE_TRACKED_FLAG | POSE_TRACKED_FLAG;
static constexpr int32 HEADER_SIZE = 4;
static constexpr int32 FLOAT_SIZE = 4;
static constexpr int32 EXPECTED_BLENDSHAPE_COUNT = 52;
static constexpr int32 EXPECTED_POSE_COUNT = 33;
static constexpr int32 FIXED_HEADER_SIZE = 22;
static constexpr int32 FACE_ROTATION_FLOAT_COUNT = 9;
static constexpr int32 EXPECTED_PACKET_SIZE = FIXED_HEADER_SIZE
	+ EXPECTED_BLENDSHAPE_COUNT * FLOAT_SIZE
	+ FACE_ROTATION_FLOAT_COUNT * FLOAT_SIZE
	+ 2
	+ EXPECTED_POSE_COUNT * 5 * FLOAT_SIZE;
static constexpr double MAX_VALID_LATENCY_MILLISECONDS = 10000.0;
static constexpr int32 MAX_RECENT_LATENCY_SAMPLES = 300;
static constexpr double STATISTICS_REFRESH_SECONDS = 1.0;
static constexpr double STATISTICS_LOG_SECONDS = 5.0;

namespace
{
double GetRawHighResolutionTimestampMilliseconds()
{
	// FPlatformTime::Seconds adds a large diagnostic offset on Windows. Raw
	// QPC cycles match Python time.perf_counter without that engine-only offset.
	return FPlatformTime::ToSeconds64(FPlatformTime::Cycles64()) * 1000.0;
}
}

// ARKit 52 blendshape names (fixed order matching MediaPipe output)
static const TArray<FName> BlendshapeNameList = {
	FName("_neutral"),
	FName("browDownLeft"), FName("browDownRight"),
	FName("browInnerUp"),
	FName("browOuterUpLeft"), FName("browOuterUpRight"),
	FName("cheekPuff"),
	FName("cheekSquintLeft"), FName("cheekSquintRight"),
	FName("eyeBlinkLeft"), FName("eyeBlinkRight"),
	FName("eyeLookDownLeft"), FName("eyeLookDownRight"),
	FName("eyeLookInLeft"), FName("eyeLookInRight"),
	FName("eyeLookOutLeft"), FName("eyeLookOutRight"),
	FName("eyeLookUpLeft"), FName("eyeLookUpRight"),
	FName("eyeSquintLeft"), FName("eyeSquintRight"),
	FName("eyeWideLeft"), FName("eyeWideRight"),
	FName("jawForward"), FName("jawLeft"), FName("jawOpen"), FName("jawRight"),
	FName("mouthClose"),
	FName("mouthDimpleLeft"), FName("mouthDimpleRight"),
	FName("mouthFrownLeft"), FName("mouthFrownRight"),
	FName("mouthFunnel"),
	FName("mouthLeft"),
	FName("mouthLowerDownLeft"), FName("mouthLowerDownRight"),
	FName("mouthPressLeft"), FName("mouthPressRight"),
	FName("mouthPucker"),
	FName("mouthRight"),
	FName("mouthRollLower"), FName("mouthRollUpper"),
	FName("mouthShrugLower"), FName("mouthShrugUpper"),
	FName("mouthSmileLeft"), FName("mouthSmileRight"),
	FName("mouthStretchLeft"), FName("mouthStretchRight"),
	FName("mouthUpperUpLeft"), FName("mouthUpperUpRight"),
	FName("noseSneerLeft"), FName("noseSneerRight")
};

bool FVPTrackingLatestFrameMailbox::Push(const FVPTrackingFrame& Frame)
{
	FScopeLock Lock(&Mutex);
	const bool bReplacedPendingFrame = bHasPendingFrame;
	PendingFrame = Frame;
	bHasPendingFrame = true;
	return bReplacedPendingFrame;
}

bool FVPTrackingLatestFrameMailbox::Pop(FVPTrackingFrame& OutFrame)
{
	FScopeLock Lock(&Mutex);
	if (!bHasPendingFrame)
	{
		return false;
	}

	OutFrame = MoveTemp(PendingFrame);
	bHasPendingFrame = false;
	return true;
}

void FVPTrackingLatestFrameMailbox::Reset()
{
	FScopeLock Lock(&Mutex);
	PendingFrame = FVPTrackingFrame();
	bHasPendingFrame = false;
}

UVPUDPReceiver::UVPUDPReceiver()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

const TArray<FName>& UVPUDPReceiver::GetBlendshapeNames()
{
	return BlendshapeNameList;
}

void UVPUDPReceiver::BeginPlay()
{
	Super::BeginPlay();

	PacketCount.Store(0);
	RejectedPacketCount.Store(0);
	DroppedPacketCount.Store(0);
	SupersededFrameCount.Store(0);
	ProtocolMismatchPacketCount.Store(0);
	LatestFrameMailbox.Reset();
	LatestFrame = FVPTrackingFrame();
	DeliveredFrameCount = 0;
	LatencySampleCount = 0;
	InvalidLatencySampleCount = 0;
	TotalCaptureToGameThreadMs = 0.0;
	MaxCaptureToGameThreadMs = 0.0f;
	NextLatencySampleIndex = 0;
	RecentCaptureToGameThreadMs.Reset(MAX_RECENT_LATENCY_SAMPLES);
	LatencySnapshot = FVPTrackingLatencySnapshot();
	LastStatisticsRefreshSeconds = FPlatformTime::Seconds();
	LastStatisticsLogSeconds = LastStatisticsRefreshSeconds;
	LastStatisticsLogPacketCount = 0;

	// Create UDP socket
	FIPv4Endpoint Endpoint(FIPv4Address(127, 0, 0, 1), ListenPort);

	Socket = FUdpSocketBuilder(TEXT("VPTrackerSocket"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToEndpoint(Endpoint)
		.WithReceiveBufferSize(65536)
		.Build();

	if (!Socket)
	{
		UE_LOG(LogTemp, Error, TEXT("[VPUDPReceiver] Failed to create socket on port %d"), ListenPort);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[VPUDPReceiver] Listening on 127.0.0.1:%d"), ListenPort);

	// Start async receiver on dedicated thread
	UDPReceiver = new FUdpSocketReceiver(
		Socket,
		FTimespan::FromMilliseconds(1),
		TEXT("VPTrackerRecvThread")
	);

	UDPReceiver->OnDataReceived().BindUObject(this, &UVPUDPReceiver::OnDataReceived);
	UDPReceiver->Start();
}

void UVPUDPReceiver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UDPReceiver)
	{
		UDPReceiver->Stop();
		delete UDPReceiver;
		UDPReceiver = nullptr;
	}

	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	LatestFrameMailbox.Reset();

	RefreshLatencySnapshot();
	UE_LOG(LogTemp, Log,
		TEXT("[VPUDPReceiver] Stopped. Accepted=%d Rejected=%d Dropped=%d Superseded=%d VersionMismatch=%d ")
		TEXT("Delivered=%d InvalidLatency=%d CaptureToUEAvg=%.2fms P95=%.2fms Max=%.2fms"),
		PacketCount.Load(),
		RejectedPacketCount.Load(),
		DroppedPacketCount.Load(),
		SupersededFrameCount.Load(),
		ProtocolMismatchPacketCount.Load(),
		LatencySnapshot.DeliveredFrameCount,
		LatencySnapshot.InvalidSampleCount,
		LatencySnapshot.AverageCaptureToGameThreadMs,
		LatencySnapshot.P95CaptureToGameThreadMs,
		LatencySnapshot.MaxCaptureToGameThreadMs);
	Super::EndPlay(EndPlayReason);
}

void UVPUDPReceiver::TickComponent(float DeltaTime, ELevelTick TickType,
                                    FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Consume at most one frame; the mailbox already retained the newest input.
	FVPTrackingFrame TempFrame;
	const bool bHasNewData = LatestFrameMailbox.Pop(TempFrame);

	if (bHasNewData)
	{
		LatestFrame = MoveTemp(TempFrame);
		const double NowSeconds = FPlatformTime::Seconds();
		LastFrameReceiveTimeSeconds = NowSeconds;
		bTrackingWasFresh = true;
		RecordLatencySample(LatestFrame, GetRawHighResolutionTimestampMilliseconds());
		OnTrackingDataReceived.Broadcast(LatestFrame);
	}
	else if (bTrackingWasFresh
		&& FPlatformTime::Seconds() - LastFrameReceiveTimeSeconds > TrackingTimeoutSeconds)
	{
		LatestFrame.bIsValid = false;
		LatestFrame.bFaceTracked = false;
		LatestFrame.bPoseTracked = false;
		bTrackingWasFresh = false;
		OnTrackingDataReceived.Broadcast(LatestFrame);
		UE_LOG(LogTemp, Warning, TEXT("[VPUDPReceiver] Tracking timed out after %.2f seconds"),
			TrackingTimeoutSeconds);
	}

	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds - LastStatisticsRefreshSeconds >= STATISTICS_REFRESH_SECONDS)
	{
		RefreshLatencySnapshot();
		LastStatisticsRefreshSeconds = NowSeconds;
	}
	LogPeriodicStatistics(NowSeconds);
}

FVPTrackingFrame UVPUDPReceiver::GetLatestTrackingData() const
{
	return LatestFrame;
}

void UVPUDPReceiver::OnDataReceived(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint)
{
	// Called on network thread
	const double ReceiveTimestampMilliseconds = GetRawHighResolutionTimestampMilliseconds();
	const uint8* RawData = Data->GetData();
	const int32 DataLen = Data->Num();
	if (DataLen >= 5 && RawData[0] == 'V' && RawData[1] == 'P' &&
		(FMemory::Memcmp(RawData, PACKET_MAGIC, HEADER_SIZE) != 0 ||
		 RawData[HEADER_SIZE] != PACKET_SCHEMA_VERSION))
	{
		++ProtocolMismatchPacketCount;
	}

	if (DecodePacket(RawData, DataLen, ParseBuffer))
	{
		ParseBuffer.ReceiveTimestampMilliseconds = ReceiveTimestampMilliseconds;
		if (LatestFrameMailbox.Push(ParseBuffer))
		{
			++SupersededFrameCount;
		}
		++PacketCount;
	}
	else
	{
		++RejectedPacketCount;
	}
}

bool UVPUDPReceiver::IsLatencySampleValid(double LatencyMilliseconds)
{
	return FMath::IsFinite(LatencyMilliseconds) &&
		LatencyMilliseconds >= 0.0 &&
		LatencyMilliseconds <= MAX_VALID_LATENCY_MILLISECONDS;
}

float UVPUDPReceiver::CalculatePercentile95(const TArray<float>& Samples)
{
	if (Samples.IsEmpty())
	{
		return 0.0f;
	}

	TArray<float> SortedSamples = Samples;
	SortedSamples.Sort();
	const int32 PercentileIndex = FMath::Clamp(
		FMath::CeilToInt(0.95f * SortedSamples.Num()) - 1,
		0,
		SortedSamples.Num() - 1);
	return SortedSamples[PercentileIndex];
}

void UVPUDPReceiver::RecordLatencySample(
	const FVPTrackingFrame& Frame,
	double GameThreadTimestampMilliseconds)
{
	++DeliveredFrameCount;
	LatencySnapshot.DeliveredFrameCount = DeliveredFrameCount;
	const double CaptureToReceiveMs = Frame.ReceiveTimestampMilliseconds - Frame.Timestamp;
	const double CaptureToGameThreadMs = GameThreadTimestampMilliseconds - Frame.Timestamp;
	if (!IsLatencySampleValid(CaptureToReceiveMs) ||
		!IsLatencySampleValid(CaptureToGameThreadMs) ||
		CaptureToGameThreadMs < CaptureToReceiveMs)
	{
		++InvalidLatencySampleCount;
		return;
	}

	const float DeliveryLatencyMs = static_cast<float>(CaptureToGameThreadMs);
	++LatencySampleCount;
	TotalCaptureToGameThreadMs += CaptureToGameThreadMs;
	MaxCaptureToGameThreadMs = FMath::Max(MaxCaptureToGameThreadMs, DeliveryLatencyMs);
	LatencySnapshot.bHasSamples = true;
	LatencySnapshot.InvalidSampleCount = InvalidLatencySampleCount;
	LatencySnapshot.LatestCaptureToReceiveMs = static_cast<float>(CaptureToReceiveMs);
	LatencySnapshot.LatestCaptureToGameThreadMs = DeliveryLatencyMs;

	if (RecentCaptureToGameThreadMs.Num() < MAX_RECENT_LATENCY_SAMPLES)
	{
		RecentCaptureToGameThreadMs.Add(DeliveryLatencyMs);
	}
	else
	{
		RecentCaptureToGameThreadMs[NextLatencySampleIndex] = DeliveryLatencyMs;
		NextLatencySampleIndex = (NextLatencySampleIndex + 1) % MAX_RECENT_LATENCY_SAMPLES;
	}
}

void UVPUDPReceiver::RefreshLatencySnapshot()
{
	LatencySnapshot.DeliveredFrameCount = DeliveredFrameCount;
	LatencySnapshot.InvalidSampleCount = InvalidLatencySampleCount;
	if (LatencySampleCount <= 0)
	{
		return;
	}

	LatencySnapshot.AverageCaptureToGameThreadMs = static_cast<float>(
		TotalCaptureToGameThreadMs / LatencySampleCount);
	LatencySnapshot.P95CaptureToGameThreadMs = CalculatePercentile95(
		RecentCaptureToGameThreadMs);
	LatencySnapshot.MaxCaptureToGameThreadMs = MaxCaptureToGameThreadMs;
}

void UVPUDPReceiver::LogPeriodicStatistics(double NowSeconds)
{
	const double ElapsedSeconds = NowSeconds - LastStatisticsLogSeconds;
	if (ElapsedSeconds < STATISTICS_LOG_SECONDS)
	{
		return;
	}

	const int32 AcceptedPackets = PacketCount.Load();
	const double AcceptedRate = (AcceptedPackets - LastStatisticsLogPacketCount) / ElapsedSeconds;
	UE_LOG(LogTemp, Log,
		TEXT("[VPUDPReceiver] Input=%.1ffps Accepted=%d Rejected=%d Dropped=%d Superseded=%d VersionMismatch=%d ")
		TEXT("Delivered=%d CaptureToReceive=%.2fms CaptureToUE=%.2fms Avg=%.2fms P95=%.2fms Max=%.2fms InvalidLatency=%d"),
		AcceptedRate,
		AcceptedPackets,
		RejectedPacketCount.Load(),
		DroppedPacketCount.Load(),
		SupersededFrameCount.Load(),
		ProtocolMismatchPacketCount.Load(),
		LatencySnapshot.DeliveredFrameCount,
		LatencySnapshot.LatestCaptureToReceiveMs,
		LatencySnapshot.LatestCaptureToGameThreadMs,
		LatencySnapshot.AverageCaptureToGameThreadMs,
		LatencySnapshot.P95CaptureToGameThreadMs,
		LatencySnapshot.MaxCaptureToGameThreadMs,
		LatencySnapshot.InvalidSampleCount);
	LastStatisticsLogSeconds = NowSeconds;
	LastStatisticsLogPacketCount = AcceptedPackets;
}

bool UVPUDPReceiver::DecodePacket(const uint8* RawData, int32 DataLen, FVPTrackingFrame& OutFrame)
{
	OutFrame = FVPTrackingFrame();
	if (!RawData || DataLen != EXPECTED_PACKET_SIZE)
	{
		return false;
	}

	// Verify magic header
	if (FMemory::Memcmp(RawData, PACKET_MAGIC, HEADER_SIZE) != 0)
	{
		return false;
	}

	int32 Offset = HEADER_SIZE;

	const uint8 Version = RawData[Offset++];
	const uint8 Flags = RawData[Offset++];
	uint16 Reserved = 0;
	FMemory::Memcpy(&Reserved, RawData + Offset, sizeof(Reserved));
	Offset += sizeof(Reserved);
	if (Version != PACKET_SCHEMA_VERSION || (Flags & ~KNOWN_FLAGS) != 0 || Reserved != 0)
	{
		return false;
	}

	uint32 FrameId = 0;
	FMemory::Memcpy(&FrameId, RawData + Offset, sizeof(FrameId));
	Offset += sizeof(FrameId);
	OutFrame.FrameId = static_cast<int64>(FrameId);
	OutFrame.bFaceTracked = (Flags & FACE_TRACKED_FLAG) != 0;
	OutFrame.bPoseTracked = (Flags & POSE_TRACKED_FLAG) != 0;

	FMemory::Memcpy(&OutFrame.Timestamp, RawData + Offset, sizeof(OutFrame.Timestamp));
	Offset += sizeof(OutFrame.Timestamp);
	if (!FMath::IsFinite(OutFrame.Timestamp) || OutFrame.Timestamp < 0.0)
	{
		return false;
	}

	uint16 NumBS = 0;
	FMemory::Memcpy(&NumBS, RawData + Offset, sizeof(NumBS));
	Offset += sizeof(NumBS);
	if (NumBS != EXPECTED_BLENDSHAPE_COUNT)
	{
		return false;
	}

	// Parse blendshapes
	OutFrame.FaceData.Blendshapes.Reset();
	const TArray<FName>& Names = GetBlendshapeNames();
	for (uint16 i = 0; i < NumBS; ++i)
	{
		float Value = 0.0f;
		FMemory::Memcpy(&Value, RawData + Offset, FLOAT_SIZE);
		Offset += FLOAT_SIZE;
		if (!FMath::IsFinite(Value) || Value < 0.0f || Value > 1.0f)
		{
			return false;
		}
		OutFrame.FaceData.Blendshapes.Add(Names[i], Value);
	}

	float FaceMatrix[FACE_ROTATION_FLOAT_COUNT] = {};
	for (float& Value : FaceMatrix)
	{
		FMemory::Memcpy(&Value, RawData + Offset, FLOAT_SIZE);
		Offset += FLOAT_SIZE;
		if (!FMath::IsFinite(Value))
		{
			return false;
		}
	}
	if (OutFrame.bFaceTracked)
	{
		// MediaPipe rows encode canonical face X/Y/Z axes. Extract intrinsic XYZ
		// angles and keep facial semantics; avatar-bone axis mapping happens later.
		const float Sy = FMath::Sqrt(
			FaceMatrix[0] * FaceMatrix[0] + FaceMatrix[3] * FaceMatrix[3]);
		if (Sy < KINDA_SMALL_NUMBER)
		{
			return false;
		}
		const float FacePitch = FMath::RadiansToDegrees(FMath::Atan2(FaceMatrix[7], FaceMatrix[8]));
		const float FaceYaw = FMath::RadiansToDegrees(FMath::Atan2(-FaceMatrix[6], Sy));
		const float FaceRoll = FMath::RadiansToDegrees(FMath::Atan2(FaceMatrix[3], FaceMatrix[0]));
		OutFrame.FaceRotation = FRotator(FacePitch, FaceYaw, FaceRoll).GetNormalized();
	}

	uint16 NumPose = 0;
	FMemory::Memcpy(&NumPose, RawData + Offset, sizeof(NumPose));
	Offset += sizeof(NumPose);
	if (NumPose != EXPECTED_POSE_COUNT)
	{
		return false;
	}

	// Parse pose landmarks
	OutFrame.PoseLandmarks.SetNum(NumPose);
	for (uint16 i = 0; i < NumPose; ++i)
	{
		float X, Y, Z, Visibility, Presence;
		FMemory::Memcpy(&X, RawData + Offset, FLOAT_SIZE); Offset += FLOAT_SIZE;
		FMemory::Memcpy(&Y, RawData + Offset, FLOAT_SIZE); Offset += FLOAT_SIZE;
		FMemory::Memcpy(&Z, RawData + Offset, FLOAT_SIZE); Offset += FLOAT_SIZE;
		FMemory::Memcpy(&Visibility, RawData + Offset, FLOAT_SIZE); Offset += FLOAT_SIZE;
		FMemory::Memcpy(&Presence, RawData + Offset, FLOAT_SIZE); Offset += FLOAT_SIZE;
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z)
			|| !FMath::IsFinite(Visibility) || !FMath::IsFinite(Presence)
			|| Visibility < 0.0f || Visibility > 1.0f
			|| Presence < 0.0f || Presence > 1.0f)
		{
			return false;
		}
		OutFrame.PoseLandmarks[i].Position = FVector(X, Y, Z);
		OutFrame.PoseLandmarks[i].Visibility = Visibility;
		OutFrame.PoseLandmarks[i].Presence = Presence;
	}

	if (Offset != DataLen)
	{
		return false;
	}

	OutFrame.bIsValid = true;
	return true;
}

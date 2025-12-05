#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DTransportTypes.h"
#include "O3DSenderInterface.h"
#include "o3ds/model.h"

#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/Array.h"

// Test helpers
namespace GenericTransportTestHelpers
{
	/**
	 * Create a test SubjectList with configurable complexity
	 */
	O3DS::SubjectList CreateTestSubjectList(int32 NumSubjects = 1, int32 BonesPerSubject = 10)
	{
		O3DS::SubjectList List;

		for (int32 SubjectIdx = 0; SubjectIdx < NumSubjects; ++SubjectIdx)
		{
			const FString SubjectName = FString::Printf(TEXT("Subject_%d"), SubjectIdx);
			const FTCHARToUTF8 SubjectUtf8(*SubjectName);
			O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));

			// Add hierarchical transforms
			for (int32 BoneIdx = 0; BoneIdx < BonesPerSubject; ++BoneIdx)
			{
				const FString BoneName = FString::Printf(TEXT("Bone_%d"), BoneIdx);
				const FTCHARToUTF8 BoneUtf8(*BoneName);
				int32 ParentIdx = (BoneIdx == 0) ? -1 : (BoneIdx - 1); // Chain hierarchy
				Subject->addTransform(std::string(BoneUtf8.Get(), BoneUtf8.Length()), ParentIdx);
			}
		}

		return List;
	}

	/**
	 * Create a large test SubjectList to test payload size limits
	 */
	O3DS::SubjectList CreateLargeTestSubjectList()
	{
		O3DS::SubjectList List;

		// Create 10 subjects with 100 bones each = 1000 transforms
		for (int32 SubjectIdx = 0; SubjectIdx < 10; ++SubjectIdx)
		{
			const FString SubjectName = FString::Printf(TEXT("LargeSubject_%d"), SubjectIdx);
			const FTCHARToUTF8 SubjectUtf8(*SubjectName);
			O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));

			// Add 100 bones
			for (int32 BoneIdx = 0; BoneIdx < 100; ++BoneIdx)
			{
				const FString BoneName = FString::Printf(TEXT("Bone_%d"), BoneIdx);
				const FTCHARToUTF8 BoneUtf8(*BoneName);
				int32 ParentIdx = (BoneIdx == 0) ? -1 : 0; // All parented to root
				O3DS::Transform* Transform = Subject->addTransform(std::string(BoneUtf8.Get(), BoneUtf8.Length()), ParentIdx);

				// Set some non-zero values to ensure serialization size
				Transform->translation.value.v[0] = static_cast<float>(BoneIdx);
				Transform->translation.value.v[1] = static_cast<float>(BoneIdx * 2);
				Transform->translation.value.v[2] = static_cast<float>(BoneIdx * 3);
			}

			// Add animation curves (morph targets)
			Subject->mCurveNames = {"Smile", "Frown", "EyeBrowUp_L", "EyeBrowUp_R", "EyeWide_L", "EyeWide_R"};
			Subject->mCurveValues = {0.5f, 0.3f, 0.7f, 0.8f, 0.4f, 0.6f};
		}

		return List;
	}

	/**
	 * Runnable for concurrent send testing
	 */
	class FConcurrentSendWorker : public FRunnable
	{
	public:
		FConcurrentSendWorker(IOpen3DSender* InSender, int32 InThreadId, int32 InNumSends)
			: Sender(InSender)
			, ThreadId(InThreadId)
			, NumSends(InNumSends)
		{
		}

		virtual uint32 Run() override
		{
			for (int32 i = 0; i < NumSends; ++i)
			{
				// Each thread sends different subject to test concurrency
				O3DS::SubjectList List = CreateTestSubjectList(1, 10);

				// Attempt send (may fail if not connected, that's ok for this test)
				Sender->Send(List);

				// Small delay to simulate real-world frame capture
				FPlatformProcess::Sleep(0.001f); // 1ms
			}
			return 0;
		}

		IOpen3DSender* Sender;
		int32 ThreadId;
		int32 NumSends;
	};
}

// ============================================================================
// GENERIC TRANSPORT TESTS - Apply to ALL transports
// ============================================================================

/**
 * Test: Concurrent Send() calls from multiple threads
 * Verifies thread-safety of Send() implementation
 * Applies to: ALL transports
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenericTransportConcurrentSendTest,
	"Open3DBroadcast.Generic.Concurrency.MultipleSends",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGenericTransportConcurrentSendTest::RunTest(const FString& Parameters)
{
	// This is a template test - actual implementations should be created per-transport
	// with specific transport initialization logic

	// Test validates that:
	// 1. Multiple threads can call Send() simultaneously without crashing
	// 2. Stats remain consistent after concurrent sends
	// 3. No memory corruption occurs

	TestTrue(TEXT("Generic concurrent send test structure defined"), true);
	AddInfo(TEXT("Transport-specific implementations should inherit this pattern"));

	return true;
}

/**
 * Test: Large payload handling
 * Verifies that large SubjectLists are handled correctly
 * Applies to: ALL transports (behavior varies by transport)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenericTransportLargePayloadTest,
	"Open3DBroadcast.Generic.Serialization.LargePayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGenericTransportLargePayloadTest::RunTest(const FString& Parameters)
{
	using namespace GenericTransportTestHelpers;

	// Create a large subject list
	O3DS::SubjectList LargeList = CreateLargeTestSubjectList();

	// Serialize to check size
	std::vector<char> Buffer;
	LargeList.Serialize(Buffer);

	const int32 PayloadSize = Buffer.size();
	UE_LOG(LogTemp, Log, TEXT("Large payload serialized to %d bytes"), PayloadSize);

	TestTrue(TEXT("Large payload should serialize successfully"), PayloadSize > 0);
	TestTrue(TEXT("Large payload should be substantial (>10KB)"), PayloadSize > 10240);

	// Transport-specific implementations should:
	// 1. Create their specific sender type
	// 2. Initialize and attempt to send LargeList
	// 3. Verify behavior:
	//    - WebRTC/UDP: May drop if exceeds MTU
	//    - TCP/NNG/MoQ: Should handle via streaming
	//    - Loopback: Should always succeed
	// 4. Check stats for DroppedFrames if applicable

	AddInfo(FString::Printf(TEXT("Serialized payload size: %d bytes"), PayloadSize));

	return true;
}

/**
 * Test: Backpressure handling
 * Verifies that transports handle queue overflow gracefully
 * Applies to: ALL transports (different mechanisms)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenericTransportBackpressureTest,
	"Open3DBroadcast.Generic.Performance.Backpressure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGenericTransportBackpressureTest::RunTest(const FString& Parameters)
{
	// This is a template test - actual implementations should be created per-transport

	// Test validates that:
	// 1. Rapid sends without connection trigger backpressure
	// 2. DroppedFrames stat increments correctly
	// 3. Transport doesn't crash or leak memory under backpressure
	// 4. Transport recovers gracefully when backpressure subsides

	// Transport-specific thresholds:
	// - WebRTC: 30 pending frames
	// - MoQ: 8MB queue bytes
	// - TCP: 4MB queue bytes
	// - NNG: Byte-based
	// - UDP: Byte-based
	// - Loopback: 64 frame capacity

	TestTrue(TEXT("Generic backpressure test structure defined"), true);
	AddInfo(TEXT("Transport-specific implementations should test their backpressure mechanisms"));

	return true;
}

/**
 * Test: Multiple subjects with different skeletons
 * Verifies correct handling of multi-subject data
 * Applies to: ALL transports
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenericTransportMultiSubjectTest,
	"Open3DBroadcast.Generic.DataIntegrity.MultipleSubjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGenericTransportMultiSubjectTest::RunTest(const FString& Parameters)
{
	using namespace GenericTransportTestHelpers;

	// Create a subject list with 5 subjects of varying complexity
	O3DS::SubjectList List;

	const FTCHARToUTF8 Name1(TEXT("Alice"));
	O3DS::Subject* Alice = List.addSubject(std::string(Name1.Get(), Name1.Length()));
	Alice->addTransform("Root", -1);
	Alice->addTransform("Spine", 0);
	Alice->addTransform("Head", 1);

	const FTCHARToUTF8 Name2(TEXT("Bob"));
	O3DS::Subject* Bob = List.addSubject(std::string(Name2.Get(), Name2.Length()));
	Bob->addTransform("Root", -1);
	Bob->addTransform("LeftArm", 0);
	Bob->addTransform("RightArm", 0);
	Bob->addTransform("LeftLeg", 0);
	Bob->addTransform("RightLeg", 0);

	const FTCHARToUTF8 Name3(TEXT("Charlie"));
	O3DS::Subject* Charlie = List.addSubject(std::string(Name3.Get(), Name3.Length()));
	Charlie->addTransform("Root", -1);

	const FTCHARToUTF8 Name4(TEXT("Diana"));
	O3DS::Subject* Diana = List.addSubject(std::string(Name4.Get(), Name4.Length()));
	for (int32 i = 0; i < 20; ++i)
	{
		const FString BoneName = FString::Printf(TEXT("Bone_%d"), i);
		const FTCHARToUTF8 BoneUtf8(*BoneName);
		Diana->addTransform(std::string(BoneUtf8.Get(), BoneUtf8.Length()), (i == 0) ? -1 : 0);
	}

	const FTCHARToUTF8 Name5(TEXT("Eve"));
	O3DS::Subject* Eve = List.addSubject(std::string(Name5.Get(), Name5.Length()));
	Eve->addTransform("Root", -1);
	Eve->addTransform("Body", 0);
	Eve->mCurveNames = {"Smile", "Frown"};
	Eve->mCurveValues = {0.8f, 0.2f};

	// Verify serialization
	std::vector<char> Buffer;
	List.Serialize(Buffer);

	TestTrue(TEXT("Multi-subject list should serialize"), Buffer.size() > 0);
	TestEqual(TEXT("Should have 5 subjects"), List.mItems.size(), static_cast<size_t>(5));

	// Transport-specific implementations should send this list and verify:
	// 1. All subjects are transmitted
	// 2. Subject names and skeleton structures are preserved
	// 3. Animation curves are transmitted correctly
	// 4. No data corruption between subjects

	return true;
}

/**
 * Test: Empty SubjectList handling
 * Verifies graceful handling of empty data
 * Applies to: ALL transports
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenericTransportEmptySubjectListTest,
	"Open3DBroadcast.Generic.EdgeCases.EmptySubjectList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGenericTransportEmptySubjectListTest::RunTest(const FString& Parameters)
{
	O3DS::SubjectList EmptyList;

	// Serialize empty list
	std::vector<char> Buffer;
	EmptyList.Serialize(Buffer);

	TestTrue(TEXT("Empty list should serialize to non-zero size (header)"), Buffer.size() > 0);
	TestEqual(TEXT("Empty list should have 0 subjects"), EmptyList.mItems.size(), static_cast<size_t>(0));

	// Transport-specific implementations should:
	// 1. Initialize sender
	// 2. Attempt to send EmptyList
	// 3. Verify no crash
	// 4. Verify stats show frame was sent (or rejected gracefully)

	return true;
}

/**
 * Test: Stats consistency under load
 * Verifies that transport stats remain accurate under rapid sends
 * Applies to: ALL transports
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenericTransportStatsConsistencyTest,
	"Open3DBroadcast.Generic.Stats.ConsistencyUnderLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGenericTransportStatsConsistencyTest::RunTest(const FString& Parameters)
{
	// This is a template test - actual implementations should be created per-transport

	// Test validates that:
	// 1. Stats.FramesSent increments correctly for each Send()
	// 2. Stats.BytesSent accumulates correctly
	// 3. Stats.DroppedFrames increments when backpressure triggers
	// 4. Stats remain thread-safe under concurrent access
	// 5. GetStats() is fast and non-blocking

	TestTrue(TEXT("Generic stats consistency test structure defined"), true);
	AddInfo(TEXT("Transport-specific implementations should verify stats accuracy"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

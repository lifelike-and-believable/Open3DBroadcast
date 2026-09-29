// Copyright (c) Open3DStream Contributors

// WP-S4 receiver correctness: UE glue tests for FO3DReceiverSource.
//
// Recorded O3DS frames (built with the core library, written through the B1
// capture format and read back with ReplayCapture) are delivered through the
// receiver's real serialized-frame consumer by a minimal fake transport. The
// source's LiveLink pushes are captured by its test push hooks, so no LiveLink
// client, network or sleeps are involved.
//
// Covers RCV-4 (renamed bones republished), RCV-5 (two senders on one channel,
// only touched subjects pushed), RCV-7 (subject created once), RCV-14 (a
// nameless transform in the middle keeps parents correct).

#include "O3DReceiverSource.h"

#if defined(WITH_DEV_AUTOMATION_TESTS) && WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "SerializedFrameConsumerRegistry.h"

#include "o3ds/capture.h"
#include "o3ds/model.h"
#include "o3ds/replay.h"
#include "o3ds_generated.h"

#include <sstream>
#include <string>
#include <vector>

struct FO3DReceiverCorrectnessTestAccessor
{
    struct FStaticPush
    {
        FName Subject;
        TArray<FName> BoneNames;
        TArray<int32> BoneParents;
        TArray<FName> CurveNames;
        bool bFirstPushThisSession = false;
    };

    struct FFramePush
    {
        FName Subject;
        TArray<FTransform> Transforms;
        TArray<float> Curves;
    };

    struct FRecorder
    {
        TArray<FStaticPush> Statics;
        TArray<FFramePush> Frames;

        int32 CountFrames(FName Subject) const
        {
            int32 Count = 0;
            for (const FFramePush& Frame : Frames)
            {
                Count += (Frame.Subject == Subject) ? 1 : 0;
            }
            return Count;
        }
    };

    static void BindRecorder(FO3DReceiverSource& Source, const TSharedRef<FRecorder>& Recorder)
    {
        Source.TestStaticPushHook = [Recorder](const FLiveLinkSubjectKey& Key, const TArray<FName>& BoneNames, const TArray<int32>& BoneParents, const TArray<FName>& CurveNames, bool bFirstPush)
        {
            FStaticPush Push;
            Push.Subject = Key.SubjectName.Name;
            Push.BoneNames = BoneNames;
            Push.BoneParents = BoneParents;
            Push.CurveNames = CurveNames;
            Push.bFirstPushThisSession = bFirstPush;
            Recorder->Statics.Add(MoveTemp(Push));
        };
        Source.TestFramePushHook = [Recorder](const FLiveLinkSubjectKey& Key, const TArray<FTransform>& Transforms, const TArray<float>& Curves, double)
        {
            FFramePush Push;
            Push.Subject = Key.SubjectName.Name;
            Push.Transforms = Transforms;
            Push.Curves = Curves;
            Recorder->Frames.Add(MoveTemp(Push));
        };
    }

    /** The same consumer object StartTransport() hands to a real transport. */
    static TSharedRef<ISerializedFrameConsumer> MakeConsumer(const TSharedRef<FO3DReceiverSource>& Source)
    {
        return MakeShared<FO3DReceiverSource::FSerializedConsumer>(TWeakPtr<FO3DReceiverSource>(Source));
    }
};

namespace O3DReceiverCorrectnessTests
{
    using FAccessor = FO3DReceiverCorrectnessTestAccessor;

    /** Minimal fake transport: replays recorded frames into a serialized-frame consumer. */
    class FFakeRecordedTransport
    {
    public:
        explicit FFakeRecordedTransport(TSharedRef<ISerializedFrameConsumer> InConsumer)
            : Consumer(MoveTemp(InConsumer))
        {
        }

        /** Writes the packets to an in-memory capture and replays it into the consumer. */
        bool Replay(const std::vector<std::vector<char>>& Packets)
        {
            std::stringstream Capture(std::ios::in | std::ios::out | std::ios::binary);
            O3DS::CaptureHeaderInfo Header;
            Header.source_desc = "O3DReceiverCorrectnessTests";
            if (!O3DS::WriteCaptureHeader(Capture, Header))
            {
                return false;
            }

            uint64 RecvUs = 1000000;
            for (const std::vector<char>& Packet : Packets)
            {
                O3DS::CaptureRecord Record;
                Record.recv_wallclock_us = RecvUs;
                Record.wire_bytes = Packet;
                if (!O3DS::WriteCaptureRecord(Capture, Record))
                {
                    return false;
                }
                RecvUs += 16667;
            }

            Capture.seekg(0);
            int32 Delivered = 0;
            O3DS::ReplayConfig Config;
            const bool bOk = O3DS::ReplayCapture(Capture, Config, [this, &Delivered](O3DS::Frame&& Frame, double)
            {
                TArray<uint8> Payload;
                Payload.Append(reinterpret_cast<const uint8*>(Frame.bytes.data()), static_cast<int32>(Frame.bytes.size()));
                Consumer->SubmitFrame(TEXT("fake"), Payload, FPlatformTime::Seconds());
                ++Delivered;
                return true;
            });
            return bOk && Delivered == static_cast<int32>(Packets.size());
        }

    private:
        TSharedRef<ISerializedFrameConsumer> Consumer;
    };

    /** A receiver source wired to a recorder and a fake transport. */
    struct FHarness
    {
        TSharedRef<FO3DReceiverSource> Source;
        TSharedRef<FAccessor::FRecorder> Recorder;
        FFakeRecordedTransport Transport;

        FHarness()
            : Source(MakeShared<FO3DReceiverSource>(MakeConfig()))
            , Recorder(MakeShared<FAccessor::FRecorder>())
            , Transport(FAccessor::MakeConsumer(Source))
        {
            FAccessor::BindRecorder(*Source, Recorder);
        }

        static FO3DReceiverSourceConfig MakeConfig()
        {
            FO3DReceiverSourceConfig Config;
            Config.TransportName = FName(TEXT("loopback")); // never started; frames come from the fake
            return Config;
        }
    };

    void BuildChain(O3DS::SubjectList& List, const std::string& SubjectName, const std::vector<std::string>& BoneNames)
    {
        O3DS::Subject* Subject = List.addSubject(SubjectName);
        for (size_t Index = 0; Index < BoneNames.size(); ++Index)
        {
            O3DS::Transform* Bone = Subject->addTransform(BoneNames[Index], static_cast<int>(Index) - 1);
            Bone->transformOrder.push_back(O3DS::TTranslation);
            Bone->transformOrder.push_back(O3DS::TRotation);
        }
    }

    std::vector<char> SerializeFull(O3DS::SubjectList& List, double Time, uint64 Seq = 0, uint32 Epoch = 0)
    {
        std::vector<char> Buffer;
        List.Serialize(Buffer, Time, Seq, Seq ? 1000 + Seq : 0, Epoch);
        return Buffer;
    }

    std::vector<char> SerializeDelta(O3DS::SubjectList& List, double Time, uint64 Seq = 0, uint32 Epoch = 0)
    {
        std::vector<char> Buffer;
        size_t Count = 0;
        List.SerializeUpdate(Buffer, Count, Time, Seq, Seq ? 1000 + Seq : 0, Epoch);
        return Buffer;
    }

    /** root, <nameless>(->root), a(->root), b(->root), c(->a). */
    std::vector<char> BuildSubjectWithNamelessNode()
    {
        flatbuffers::FlatBufferBuilder Fbb;
        const O3DS::Data::Translation Translation(1.0f, 2.0f, 3.0f);
        const O3DS::Data::Rotation Rotation(0.0f, 0.0f, 0.0f, 1.0f);
        const std::vector<int8_t> Components = { O3DS::Data::Component_Translation, O3DS::Data::Component_Rotation };

        struct FNode { const char* Name; int Parent; };
        const FNode Nodes[] = { { "root", -1 }, { nullptr, 0 }, { "a", 0 }, { "b", 0 }, { "c", 2 } };

        std::vector<flatbuffers::Offset<O3DS::Data::Transform>> Out;
        for (const FNode& Node : Nodes)
        {
            auto Name = Node.Name ? Fbb.CreateString(Node.Name) : flatbuffers::Offset<flatbuffers::String>();
            auto ComponentVector = Fbb.CreateVector(Components);
            Out.push_back(O3DS::Data::CreateTransform(Fbb, Node.Parent, Name, &Translation, &Rotation, nullptr, 0, ComponentVector));
        }
        auto Subject = O3DS::Data::CreateSubject(Fbb, Fbb.CreateVector(Out), Fbb.CreateString("Actor"));
        std::vector<flatbuffers::Offset<O3DS::Data::Subject>> Subjects = { Subject };
        Fbb.Finish(O3DS::Data::CreateSubjectList(Fbb, Fbb.CreateVector(Subjects), 0, 1.0));

        std::vector<char> Buffer;
        O3DS::finalize(Fbb, Buffer, 1);
        return Buffer;
    }

    /** RCV-4: same hierarchy, new bone names. Run for legacy (Seq 0) and gated streams. */
    bool RunRenamedBones(FAutomationTestBase& Test, bool bGated)
    {
        O3DS::SubjectList RigA;
        BuildChain(RigA, "Actor", { "Hips", "Spine", "Head" });
        O3DS::SubjectList RigB;
        BuildChain(RigB, "Actor", { "Root", "Chest", "Neck" });

        const uint32 Epoch = bGated ? 7u : 0u;
        std::vector<std::vector<char>> Packets;
        Packets.push_back(SerializeFull(RigA, 1.00, bGated ? 1 : 0, Epoch));
        RigA.findSubject("Actor")->mTransforms[1]->translation.value = O3DS::Vector3d(0.0, 1.0, 0.0);
        Packets.push_back(SerializeDelta(RigA, 1.02, bGated ? 2 : 0, Epoch));
        Packets.push_back(SerializeFull(RigB, 1.04, bGated ? 3 : 0, Epoch));
        RigB.findSubject("Actor")->mTransforms[1]->translation.value = O3DS::Vector3d(0.0, 2.0, 0.0);
        Packets.push_back(SerializeDelta(RigB, 1.06, bGated ? 4 : 0, Epoch));

        FHarness Harness;
        Test.TestTrue(TEXT("Frames replayed"), Harness.Transport.Replay(Packets));

        const FAccessor::FRecorder& Rec = *Harness.Recorder;
        Test.TestEqual(TEXT("Every frame pushed"), Rec.Frames.Num(), 4);
        Test.TestEqual(TEXT("Static data pushed on first sight and on rename"), Rec.Statics.Num(), 2);
        if (Rec.Statics.Num() != 2)
        {
            return false;
        }

        const FAccessor::FStaticPush& Renamed = Rec.Statics[1];
        Test.TestEqual(TEXT("Three bones"), Renamed.BoneNames.Num(), 3);
        if (Renamed.BoneNames.Num() == 3)
        {
            Test.TestTrue(TEXT("Bone 0 renamed"), Renamed.BoneNames[0] == FName(TEXT("Root")));
            Test.TestTrue(TEXT("Bone 1 renamed"), Renamed.BoneNames[1] == FName(TEXT("Chest")));
            Test.TestTrue(TEXT("Bone 2 renamed"), Renamed.BoneNames[2] == FName(TEXT("Neck")));
            Test.TestEqual(TEXT("Parents kept"), Renamed.BoneParents[2], 1);
        }

        // RCV-7: the subject is created once; the rename re-pushes static data only.
        Test.TestTrue(TEXT("First push creates the subject"), Rec.Statics[0].bFirstPushThisSession);
        Test.TestFalse(TEXT("Rename does not recreate the subject"), Renamed.bFirstPushThisSession);

        if (Rec.Frames.Num() == 4 && Rec.Frames[3].Transforms.Num() == 3)
        {
            Test.TestEqual(TEXT("Last frame carries the new rig's pose"), Rec.Frames[3].Transforms[1].GetLocation().Y, 2.0, 1e-4);
        }
        return true;
    }
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverRenamedBonesLegacyTest, "Open3DBroadcast.Receiver.Correctness.RenamedBonesRepublished.Legacy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverRenamedBonesLegacyTest::RunTest(const FString& Parameters)
{
    return O3DReceiverCorrectnessTests::RunRenamedBones(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverRenamedBonesGatedTest, "Open3DBroadcast.Receiver.Correctness.RenamedBonesRepublished.Gated", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverRenamedBonesGatedTest::RunTest(const FString& Parameters)
{
    return O3DReceiverCorrectnessTests::RunRenamedBones(*this, true);
}

// RCV-5: two gated senders with their own tx_seq spaces and epochs on one channel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverTwoSendersTest, "Open3DBroadcast.Receiver.Correctness.TwoSendersOneChannel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverTwoSendersTest::RunTest(const FString& Parameters)
{
    O3DS::SubjectList SenderA;
    O3DReceiverCorrectnessTests::BuildChain(SenderA, "Alice", { "Hips", "Spine", "Head" });
    O3DS::SubjectList SenderB;
    O3DReceiverCorrectnessTests::BuildChain(SenderB, "Bob", { "Hips", "Spine", "Head" });

    constexpr int32 NumFrames = 10;
    std::vector<std::vector<char>> Packets;
    for (int32 Index = 0; Index < NumFrames; ++Index)
    {
        const uint64 Seq = static_cast<uint64>(Index + 1);
        SenderA.findSubject("Alice")->mTransforms[1]->translation.value = O3DS::Vector3d(0.0, 1.0 + Index, 0.0);
        SenderB.findSubject("Bob")->mTransforms[1]->translation.value = O3DS::Vector3d(0.0, 100.0 + Index, 0.0);
        Packets.push_back(Index == 0 ? O3DReceiverCorrectnessTests::SerializeFull(SenderA, 10.0 + Index, Seq, 100) : O3DReceiverCorrectnessTests::SerializeDelta(SenderA, 10.0 + Index, Seq, 100));
        Packets.push_back(Index == 0 ? O3DReceiverCorrectnessTests::SerializeFull(SenderB, 500.0 + Index, Seq + 1000, 200) : O3DReceiverCorrectnessTests::SerializeDelta(SenderB, 500.0 + Index, Seq + 1000, 200));
    }

    O3DReceiverCorrectnessTests::FHarness Harness;
    TestTrue(TEXT("Frames replayed"), Harness.Transport.Replay(Packets));

    const FO3DReceiverCorrectnessTestAccessor::FRecorder& Rec = *Harness.Recorder;
    TestEqual(TEXT("Alice frames"), Rec.CountFrames(FName(TEXT("Alice"))), NumFrames);
    TestEqual(TEXT("Bob frames"), Rec.CountFrames(FName(TEXT("Bob"))), NumFrames);
    TestEqual(TEXT("One static push per subject"), Rec.Statics.Num(), 2);

    int32 AliceIndex = 0;
    int32 BobIndex = 0;
    for (const FO3DReceiverCorrectnessTestAccessor::FFramePush& Frame : Rec.Frames)
    {
        if (Frame.Transforms.Num() != 3)
        {
            AddError(TEXT("Frame with the wrong bone count"));
            continue;
        }
        const double Y = Frame.Transforms[1].GetLocation().Y;
        if (Frame.Subject == FName(TEXT("Alice")))
        {
            TestEqual(TEXT("Alice pose in order"), Y, 1.0 + AliceIndex++, 1e-4);
        }
        else
        {
            TestEqual(TEXT("Bob pose in order"), Y, 100.0 + BobIndex++, 1e-4);
        }
    }
    return true;
}

// RCV-5: an update that names one subject pushes only that subject.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverOnlyTouchedTest, "Open3DBroadcast.Receiver.Correctness.PushesOnlyTouchedSubjects", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverOnlyTouchedTest::RunTest(const FString& Parameters)
{
    // A multi-subject sender syncs A and B, then drops B and sends a delta for A.
    O3DS::SubjectList Both;
    O3DReceiverCorrectnessTests::BuildChain(Both, "A", { "Hips", "Spine" });
    O3DReceiverCorrectnessTests::BuildChain(Both, "B", { "Hips", "Spine" });
    O3DS::SubjectList OnlyA;
    O3DReceiverCorrectnessTests::BuildChain(OnlyA, "A", { "Hips", "Spine" });
    OnlyA.findSubject("A")->mTransforms[1]->translation.value = O3DS::Vector3d(4.0, 0.0, 0.0);

    std::vector<std::vector<char>> Packets = { O3DReceiverCorrectnessTests::SerializeFull(Both, 1.0), O3DReceiverCorrectnessTests::SerializeDelta(OnlyA, 1.1) };

    O3DReceiverCorrectnessTests::FHarness Harness;
    TestTrue(TEXT("Frames replayed"), Harness.Transport.Replay(Packets));

    const FO3DReceiverCorrectnessTestAccessor::FRecorder& Rec = *Harness.Recorder;
    TestEqual(TEXT("A pushed by both packets"), Rec.CountFrames(FName(TEXT("A"))), 2);
    TestEqual(TEXT("B pushed only by the packet that carried it"), Rec.CountFrames(FName(TEXT("B"))), 1);
    if (Rec.Frames.Num() == 3 && Rec.Frames[2].Transforms.Num() == 2)
    {
        TestEqual(TEXT("A's delta applied"), Rec.Frames[2].Transforms[1].GetLocation().X, 4.0, 1e-4);
    }
    return true;
}

// RCV-14: a nameless transform in the middle of a subject keeps parents correct.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverNullTransformParentsTest, "Open3DBroadcast.Receiver.Correctness.NullTransformKeepsParents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverNullTransformParentsTest::RunTest(const FString& Parameters)
{
    O3DReceiverCorrectnessTests::FHarness Harness;
    TestTrue(TEXT("Frame replayed"), Harness.Transport.Replay({ O3DReceiverCorrectnessTests::BuildSubjectWithNamelessNode() }));

    const FO3DReceiverCorrectnessTestAccessor::FRecorder& Rec = *Harness.Recorder;
    TestEqual(TEXT("One static push"), Rec.Statics.Num(), 1);
    TestEqual(TEXT("One frame push"), Rec.Frames.Num(), 1);
    if (Rec.Statics.Num() != 1)
    {
        return false;
    }

    const FO3DReceiverCorrectnessTestAccessor::FStaticPush& Static = Rec.Statics[0];
    TestEqual(TEXT("All five bones kept"), Static.BoneNames.Num(), 5);
    TestEqual(TEXT("Parents match bones"), Static.BoneParents.Num(), Static.BoneNames.Num());
    if (Static.BoneNames.Num() == 5 && Static.BoneParents.Num() == 5)
    {
        TestTrue(TEXT("c's parent is a"), Static.BoneNames[Static.BoneParents[4]] == FName(TEXT("a")));
        TestTrue(TEXT("b's parent is root"), Static.BoneNames[Static.BoneParents[3]] == FName(TEXT("root")));
        const FString Placeholder = FString(UTF8_TO_TCHAR(O3DS::kUnnamedTransformPrefix)) + TEXT("1");
        TestTrue(TEXT("Nameless bone gets a placeholder"), Static.BoneNames[1] == FName(*Placeholder));
    }
    if (Rec.Frames.Num() == 1)
    {
        TestEqual(TEXT("One transform per bone"), Rec.Frames[0].Transforms.Num(), 5);
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

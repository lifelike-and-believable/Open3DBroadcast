// Copyright (c) Open3DStream Contributors

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

// O3DS protocol API
#include "o3ds/model.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace O3DS;

static bool NearlyEqual(double A, double B, double Tolerance = 1e-4)
{
    return FMath::Abs((float)(A - B)) <= Tolerance;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSSubjectRoundTripTest,
    "Open3DStream.M2.RoundTrip.Subject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSSubjectRoundTripTest::RunTest(const FString& Parameters)
{
    SubjectList list;
    Subject* subj = list.addSubject("RT_Subject");

    // Build two-node hierarchy
    auto* root = subj->addTransform("Root", -1);
    root->translation.value = O3DS::Vector3d(1.0, 2.0, 3.0);
    root->rotation.value    = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
    root->scale.value       = O3DS::Vector3d(1.0, 1.0, 1.0);
    // Explicitly declare component order so components are serialized
    root->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };

    auto* child = subj->addTransform("Child", 0);
    child->translation.value = O3DS::Vector3d(-4.0, 5.0, -6.0);
    child->rotation.value    = O3DS::Vector4d(0.0, 0.7071, 0.0, 0.7071);
    child->scale.value       = O3DS::Vector3d(1.0, 2.0, 1.0);
    // Explicitly declare component order so components are serialized
    child->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };

    subj->mCurveNames = { "Smile", "Blink_L" };
    subj->mCurveValues = { 0.5f, 0.1f };

    // Ensure transform components/matrices are propagated prior to serialization
    subj->CalcMatrices();

    std::vector<char> buf;
    const double ts = 123.456;
    int rc = list.Serialize(buf, ts);
    // Some implementations return bytes written or 0; rely on buffer content instead of rc
    TestTrue(TEXT("Serialized some bytes"), buf.size() > 0);

    SubjectList parsed;
    bool ok = parsed.Parse(buf.data(), buf.size(), nullptr, true /*clearInactive*/);
    TestTrue(TEXT("Parse returned success"), ok);

    TestTrue(TEXT("Parsed one subject"), parsed.size() == 1);
    Subject* p = parsed[0];
    TestTrue(TEXT("Name round-tripped"), FString(p->mName.c_str()) == TEXT("RT_Subject"));

    // Transforms
    TestTrue(TEXT("Transform count == 2"), p->size() == 2);
    auto* pRoot = (*p).mTransforms[0];
    auto* pChild = (*p).mTransforms[1];

    TestTrue(TEXT("Root parentId == -1"), pRoot->mParentId == -1);
    TestTrue(TEXT("Child parentId == 0"), pChild->mParentId == 0);

    // Ensure matrices are computed from parsed components or matrices list
    p->CalcMatrices();

    auto rootT = pRoot->mMatrix.GetTranslation();
    if (!NearlyEqual(rootT[0], 1.0) || !NearlyEqual(rootT[1], 2.0) || !NearlyEqual(rootT[2], 3.0))
    {
        UE_LOG(LogTemp, Warning, TEXT("Root parsed T = (%f, %f, %f)"), (float)rootT[0], (float)rootT[1], (float)rootT[2]);
        UE_LOG(LogTemp, Warning, TEXT("Root component T = (%f, %f, %f)"),
            (float)pRoot->translation.value[0], (float)pRoot->translation.value[1], (float)pRoot->translation.value[2]);
    }

    TestTrue(TEXT("Root T.X equal"), NearlyEqual(rootT[0], 1.0));
    TestTrue(TEXT("Root T.Y equal"), NearlyEqual(rootT[1], 2.0));
    TestTrue(TEXT("Root T.Z equal"), NearlyEqual(rootT[2], 3.0));

    auto childT = pChild->mMatrix.GetTranslation();
    if (!NearlyEqual(childT[0], -4.0) || !NearlyEqual(childT[1], 5.0) || !NearlyEqual(childT[2], -6.0))
    {
        UE_LOG(LogTemp, Warning, TEXT("Child parsed T = (%f, %f, %f)"), (float)childT[0], (float)childT[1], (float)childT[2]);
        UE_LOG(LogTemp, Warning, TEXT("Child component T = (%f, %f, %f)"),
            (float)pChild->translation.value[0], (float)pChild->translation.value[1], (float)pChild->translation.value[2]);
    }

    TestTrue(TEXT("Child T.X equal"), NearlyEqual(childT[0], -4.0));
    TestTrue(TEXT("Child T.Y equal"), NearlyEqual(childT[1], 5.0));
    TestTrue(TEXT("Child T.Z equal"), NearlyEqual(childT[2], -6.0));

    // Curves
    TestTrue(TEXT("Curves names count == 2"), p->mCurveNames.size() == 2);
    TestTrue(TEXT("Curve0 name"), FString(p->mCurveNames[0].c_str()) == TEXT("Smile"));
    TestTrue(TEXT("Curve1 name"), FString(p->mCurveNames[1].c_str()) == TEXT("Blink_L"));
    TestTrue(TEXT("Curve values count == 2"), p->mCurveValues.size() == 2);
    TestTrue(TEXT("Curve0 value"), NearlyEqual(p->mCurveValues[0], 0.5));
    TestTrue(TEXT("Curve1 value"), NearlyEqual(p->mCurveValues[1], 0.1));

    TestTrue(TEXT("Timestamp set"), parsed.mTime == ts || parsed.mTime > 0.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSSubjectRoundTrip_NoCurves,
    "Open3DStream.M2.RoundTrip.SubjectNoCurves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSSubjectRoundTrip_NoCurves::RunTest(const FString& Parameters)
{
    SubjectList list;
    Subject* subj = list.addSubject("RT_NoCurves");

    auto* only = subj->addTransform("Solo", -1);
    only->translation.value = O3DS::Vector3d(0.0, 0.0, 0.0);
    only->rotation.value    = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
    only->scale.value       = O3DS::Vector3d(1.0, 1.0, 1.0);
    // Explicitly declare component order so components are serialized
    only->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };

    // Ensure matrices are propagated before serialization
    subj->CalcMatrices();

    std::vector<char> buf;
    int rc = list.Serialize(buf, 0.0);
    // Some implementations return bytes written or 0; rely on buffer content instead of rc
    TestTrue(TEXT("Serialized some bytes"), buf.size() > 0);

    SubjectList parsed;
    bool ok = parsed.Parse(buf.data(), buf.size(), nullptr, true);
    TestTrue(TEXT("Parse returned success"), ok);

    TestTrue(TEXT("Parsed one subject"), parsed.size() == 1);
    Subject* p = parsed[0];
    TestTrue(TEXT("Transform count == 1"), p->size() == 1);
    TestTrue(TEXT("Curve names empty"), p->mCurveNames.empty());
    TestTrue(TEXT("Curve values empty"), p->mCurveValues.empty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSSubjectUpdateRoundTripTest,
    "Open3DStream.M2.RoundTrip.SubjectUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FO3DSSubjectUpdateRoundTripTest::RunTest(const FString& Parameters)
{
    // Sender state
    SubjectList sender;
    Subject* subj = sender.addSubject("RT_Update");

    auto* root = subj->addTransform("Root", -1);
    root->translation.value = O3DS::Vector3d(0.0, 0.0, 0.0);
    root->rotation.value    = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
    root->scale.value       = O3DS::Vector3d(1.0, 1.0, 1.0);
    root->transformOrder    = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };

    auto* child = subj->addTransform("Child", 0);
    child->translation.value = O3DS::Vector3d(1.0, 0.0, 0.0);
    child->rotation.value    = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
    child->scale.value       = O3DS::Vector3d(1.0, 1.0, 1.0);
    child->transformOrder    = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };

    subj->CalcMatrices();

    // Send full descriptor frame
    std::vector<char> fullBuf;
    sender.Serialize(fullBuf, /*ts*/0.0);
    TestTrue(TEXT("Full serialize produced bytes"), fullBuf.size() > 0);

    // Receiver parses full
    SubjectList recv;
    bool ok = recv.Parse(fullBuf.data(), fullBuf.size(), nullptr, true);
    TestTrue(TEXT("Receiver parsed full"), ok);
    TestTrue(TEXT("Receiver has one subject"), recv.size() == 1);

    // Baseline receiver child's translation
    Subject* pr0 = recv[0];
    pr0->CalcMatrices();
    auto* rChild0 = (*pr0).mTransforms[1];
    auto baseChildT = rChild0->mMatrix.GetTranslation();

    // Seed last-sent state so deltas are relative to current values
    root->translation.sent();
    root->rotation.sent();
    root->scale.sent();
    child->translation.sent();
    child->rotation.sent();
    child->scale.sent();

    // Set delta threshold on the sender
    sender.SetDeltaThreshold(0.001);

    // 1) Update below threshold -> expect zero subject updates (count==0). Buffer may contain protocol overhead.
    root->translation.value = O3DS::Vector3d(0.0001, 0.0, 0.0); // below threshold

    std::vector<char> updSmall;
    size_t countSmall = 0;
    sender.SerializeUpdate(updSmall, countSmall, /*ts*/0.0);

    TestTrue(TEXT("Small update count == 0"), countSmall == 0);

    // If any bytes present, parsing them should not change the receiver's transforms
    if (!updSmall.empty())
    {
        bool okSmall = recv.Parse(updSmall.data(), updSmall.size(), nullptr, true);
        TestTrue(TEXT("Receiver parsed small update"), okSmall);

        Subject* pr1 = recv[0];
        pr1->CalcMatrices();
        auto* rChild1 = (*pr1).mTransforms[1];
        auto childT1 = rChild1->mMatrix.GetTranslation();

        TestTrue(TEXT("Child T.X unchanged on small update"), NearlyEqual(childT1[0], baseChildT[0]));
        TestTrue(TEXT("Child T.Y unchanged on small update"), NearlyEqual(childT1[1], baseChildT[1]));
        TestTrue(TEXT("Child T.Z unchanged on small update"), NearlyEqual(childT1[2], baseChildT[2]));
    }

    // 2) Update above threshold on child -> expect bytes and parsed state changes
    const O3DS::Vector3d NewChildT(1.2, 0.0, 0.0);
    child->translation.value = NewChildT;

    std::vector<char> updLarge;
    size_t countLarge = 0;
    sender.SerializeUpdate(updLarge, countLarge, /*ts*/0.0);

    TestTrue(TEXT("Large update produced bytes"), updLarge.size() > 0);
    TestTrue(TEXT("Large update count > 0"), countLarge > 0);

    bool ok2 = recv.Parse(updLarge.data(), updLarge.size(), nullptr, true);
    TestTrue(TEXT("Receiver parsed update"), ok2);

    // Validate receiver state
    Subject* pr = recv[0];
    pr->CalcMatrices();
    auto* rChild = (*pr).mTransforms[1];
    auto rChildT = rChild->mMatrix.GetTranslation();

    TestTrue(TEXT("Child T.X updated"), NearlyEqual(rChildT[0], NewChildT.v[0]));
    TestTrue(TEXT("Child T.Y unchanged"), NearlyEqual(rChildT[1], NewChildT.v[1]));
    TestTrue(TEXT("Child T.Z unchanged"), NearlyEqual(rChildT[2], NewChildT.v[2]));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

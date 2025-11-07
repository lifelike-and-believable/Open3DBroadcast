#include <gtest/gtest.h>
#include "flatbuffers/flatbuffers.h"
#include "o3ds_generated.h"
#include "subject_serializer_tx.cpp" // or include header to BuildSubjectListWithTx declaration

TEST(SerializationRoundTrip, UnsetDefaults) {
    flatbuffers::FlatBufferBuilder b;
    auto off = BuildSubjectListWithTx(b, /* other args */ false);
    b.Finish(off);
    auto sl = o3ds::GetSubjectList(b.GetBufferPointer());
    EXPECT_EQ(sl->tx_seq(), 0u);
    EXPECT_EQ(sl->tx_wallclock_us(), 0u);
}

TEST(SerializationMonotonic, SequenceIncrements) {
    flatbuffers::FlatBufferBuilder b1;
    auto off1 = BuildSubjectListWithTx(b1, /*...*/ true);
    b1.Finish(off1);
    auto sl1 = o3ds::GetSubjectList(b1.GetBufferPointer());
    uint64_t s1 = sl1->tx_seq();

    flatbuffers::FlatBufferBuilder b2;
    auto off2 = BuildSubjectListWithTx(b2, /*...*/ true);
    b2.Finish(off2);
    auto sl2 = o3ds::GetSubjectList(b2.GetBufferPointer());
    uint64_t s2 = sl2->tx_seq();

    EXPECT_GT(s2, s1);
}

// Additional tests: ordering [100,101,99,102], duplicates, stale-age. Adapt test harness to instantiate receiver state.
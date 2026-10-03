// O3DS::StreamWriter (ADR 0005 (iv), SND-15, CORE-29): every frame a sender
// writes is stamped with tx_seq, tx_wallclock_us and frame_epoch, the counter
// is never reset within a process, and each session gets a strictly larger
// epoch even when two start within one wall-clock second.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/residual_codec.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/sequencing.h"
#include "o3ds/stream_writer.h"

#include <memory>
#include <vector>

using namespace O3DS;

namespace
{
	void BuildSkeleton(SubjectList& list)
	{
		Subject* subject = list.addSubject("Actor");
		Transform* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);
	}

	PacketMeta Peek(const std::vector<char>& frame)
	{
		PacketMeta meta;
		O3DS_CHECK(PeekPacketMeta(frame.data(), frame.size(), meta));
		return meta;
	}
}

O3DS_TEST(StreamWriter_StampsEveryWriteInOrder)
{
	SubjectList list;
	BuildSkeleton(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;

	const uint64_t before = NowUtcMicros();
	std::vector<char> full;
	O3DS_CHECK(writer.WriteFull(*subject, full, 1.0) > 0);
	const PacketMeta first = Peek(full);
	O3DS_CHECK(first.tx_seq == 1);
	O3DS_CHECK(first.frame_epoch != 0);
	O3DS_CHECK(first.tx_wallclock_us >= before && first.tx_wallclock_us <= NowUtcMicros());

	subject->mTransforms[0]->translation.value = Vector3d(1.0, 0.0, 0.0);
	std::vector<char> update;
	size_t count = 0;
	O3DS_CHECK(writer.WriteUpdate(*subject, update, count, 1.0e-6, 1.02) > 0);
	const PacketMeta second = Peek(update);
	O3DS_CHECK(second.tx_seq == 2);
	O3DS_CHECK(second.frame_epoch == first.frame_epoch);

	QuantRanges ranges;
	subject->mTransforms[0]->translation.value = Vector3d(1.001, 0.0, 0.0);
	std::vector<char> quantized;
	count = 0;
	O3DS_CHECK(writer.WriteUpdate(*subject, quantized, count, 1.0e-6, 1.04, &ranges) > 0);
	O3DS_CHECK(Peek(quantized).tx_seq == 3);

	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	std::vector<char> residual;
	count = 0;
	O3DS_CHECK(writer.WriteResidual(*subject, residual, count, 1.0e-6, 1.06) > 0);
	O3DS_CHECK(Peek(residual).tx_seq == 4);

	std::vector<char> listFull;
	O3DS_CHECK(writer.WriteFull(list, listFull, 1.08) > 0);
	O3DS_CHECK(Peek(listFull).tx_seq == 5);
	O3DS_CHECK(writer.LastSeq() == 5);

	// The stamps are what a receiver parses.
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(full.data(), full.size()));
	O3DS_CHECK(receiver.Parse(update.data(), update.size()));
}

O3DS_TEST(StreamWriter_SessionsNeverReuseAnEpochOrResetTheCounter)
{
	SubjectList list;
	BuildSkeleton(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;
	O3DS_CHECK(writer.Epoch() == 0);

	writer.StartSession();
	const uint32_t firstEpoch = writer.Epoch();
	O3DS_CHECK(firstEpoch >= NewSessionEpoch() - 1);

	std::vector<char> frame;
	O3DS_CHECK(writer.WriteFull(*subject, frame, 1.0) > 0);
	O3DS_CHECK(Peek(frame).tx_seq == 1);

	// Two starts within the same second (a Stop/Start from a property edit)
	// still give strictly larger epochs (ADR 0005 (iv)).
	writer.StartSession();
	const uint32_t secondEpoch = writer.Epoch();
	writer.StartSession();
	const uint32_t thirdEpoch = writer.Epoch();
	O3DS_CHECK(secondEpoch > firstEpoch);
	O3DS_CHECK(thirdEpoch > secondEpoch);

	// The counter is not reset by a new session.
	O3DS_CHECK(writer.WriteFull(*subject, frame, 2.0) > 0);
	const PacketMeta meta = Peek(frame);
	O3DS_CHECK(meta.tx_seq == 2);
	O3DS_CHECK(meta.frame_epoch == thirdEpoch);
}

O3DS_TEST(StreamWriter_NewWriterAlwaysTakesALargerEpoch)
{
	// A sender whose writer is recreated within one second (state cleared on
	// Stop) restarts its counter, so it must also be in a newer epoch, or the
	// receiver's gate would drop the restarted frames as stale.
	StreamWriter first;
	first.StartSession();
	StreamWriter second;
	second.StartSession();
	StreamWriter third;
	O3DS_CHECK(third.Next().frame_epoch > second.Epoch());
	O3DS_CHECK(second.Epoch() > first.Epoch());
}

O3DS_TEST(StreamWriter_UnstampedWritesStayUnset)
{
	// The Subject overloads without a stamp still write 0 (legacy, unsequenced).
	SubjectList list;
	BuildSkeleton(list);
	std::vector<char> frame;
	O3DS_CHECK(list.findSubject("Actor")->Serialize(frame, 1.0) > 0);
	const PacketMeta meta = Peek(frame);
	O3DS_CHECK(meta.tx_seq == 0 && meta.tx_wallclock_us == 0 && meta.frame_epoch == 0);
}

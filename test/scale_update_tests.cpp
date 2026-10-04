// CORE-11, ADR 0005 (v): scale is sent in updates. The legacy and quantized
// delta path sends absolute scale behind the delta threshold; residual
// updates carry absolute scale (not residuals), on keyframes and when it
// moved past the threshold, and the receiver applies it. Before, scale froze
// at the last full Subject in both encodings.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/residual_codec.h"

#include <cmath>
#include <memory>
#include <vector>

using namespace O3DS;

namespace
{
	Subject* BuildActor(SubjectList& list)
	{
		Subject* subject = list.addSubject("Actor");
		for (int i = 0; i < 2; ++i)
		{
			Transform* t = subject->addTransform(i == 0 ? "Root" : "Head", i - 1);
			t->transformOrder.push_back(O3DS::TTranslation);
			t->transformOrder.push_back(O3DS::TRotation);
			t->transformOrder.push_back(O3DS::TScale);
		}
		return subject;
	}

	void Animate(Subject* subject, double t)
	{
		subject->mTransforms[0]->translation.value = Vector3d(t, 0.0, 0.0);
		subject->mTransforms[1]->scale.value = Vector3d(1.0 + 0.5 * std::sin(3.0 * t), 1.0, 1.0 - 0.25 * t);
	}

	bool SameScale(Subject* a, Subject* b, double tolerance)
	{
		for (size_t n = 0; n < a->mTransforms.size(); ++n)
		{
			for (int k = 0; k < 3; ++k)
			{
				if (std::abs(a->mTransforms[n]->scale.value.v[k] - b->mTransforms[n]->scale.value.v[k]) > tolerance)
					return false;
			}
		}
		return true;
	}

	size_t ScaleEntries(const std::vector<char>& frame)
	{
		const auto* list = O3DS::Data::GetSubjectList(frame.data() + 8);
		const auto* scales = list->updates()->Get(0)->scale();
		return scales ? scales->size() : 0;
	}
}

O3DS_TEST(ScaleUpdate_DeltaAndQuantizedPathsSendScale)
{
	for (bool quantized : { false, true })
	{
		SubjectList sender;
		Subject* subject = BuildActor(sender);
		QuantRanges ranges;
		ranges.byteRange = 0.01;
		ranges.halfRange = 1.0;

		std::vector<char> frame;
		Animate(subject, 0.0);
		O3DS_CHECK(subject->Serialize(frame, 0.0) > 0);
		SubjectList receiver;
		O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));

		for (int i = 1; i <= 20; ++i)
		{
			const double t = i * 0.05;
			Animate(subject, t);
			size_t count = 0;
			O3DS_CHECK(subject->SerializeUpdate(frame, count, 1.0e-6, t, quantized ? &ranges : nullptr) > 0);
			O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));
			O3DS_CHECK(SameScale(receiver.findSubject("Actor"), subject, 1.0e-5));
		}

		// A scale that did not move is not resent.
		size_t count = 0;
		O3DS_CHECK(subject->SerializeUpdate(frame, count, 1.0e-6, 2.0, quantized ? &ranges : nullptr) > 0);
		O3DS_CHECK(ScaleEntries(frame) == 0);
	}
}

O3DS_TEST(ScaleUpdate_ResidualPathSendsAbsoluteScale)
{
	SubjectList sender;
	Subject* subject = BuildActor(sender);
	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, /*keyframeIntervalFrames*/ 7));

	std::vector<char> frame;
	Animate(subject, 0.0);
	O3DS_CHECK(subject->Serialize(frame, 0.0) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));

	// The first update is a keyframe and carries every scale.
	size_t count = 0;
	Animate(subject, 0.02);
	O3DS_CHECK(subject->SerializeUpdateResidual(frame, count, 1.0e-6, 0.02) > 0);
	O3DS_CHECK(ScaleEntries(frame) == subject->mTransforms.size());
	O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));

	for (int i = 2; i <= 40; ++i)
	{
		const double t = i * 0.02;
		Animate(subject, t);
		O3DS_CHECK(subject->SerializeUpdateResidual(frame, count, 1.0e-6, t) > 0);
		O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));
		Subject* received = receiver.findSubject("Actor");
		O3DS_CHECK(SameScale(received, subject, 1.0e-5));
		// Translation still decodes through the residual path alongside it.
		O3DS_CHECK(std::abs(received->mTransforms[0]->translation.value.v[0] - t) < 1.0e-4);
	}
}

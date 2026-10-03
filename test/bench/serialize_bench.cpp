// Serialize/Parse benchmark for one 250-bone, 250-curve subject (ADR 0008
// item 11, CORE-7, CORE-18). It prints the mean time per call of each path
// and checks only correctness (round trip, CRC agreement), never speed, so
// it cannot fail on a slow or busy machine. Usage:
//   o3ds_core_bench [--iterations N]
// CTest runs it with a small N (label "bench"); run it by hand with the
// default for numbers worth recording.

#include "o3ds/crc32.h"
#include "o3ds/model.h"

#include "CRC.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace O3DS;

namespace
{
	using Clock = std::chrono::steady_clock;

	double MicrosPerCall(Clock::time_point start, int iterations)
	{
		return std::chrono::duration<double, std::micro>(Clock::now() - start).count() / iterations;
	}

	void BuildSubject(SubjectList& list)
	{
		Subject* subject = list.addSubject("MetaHuman");
		for (int i = 0; i < 250; ++i)
		{
			Transform* t = subject->addTransform("bone_" + std::to_string(i), i - 1);
			t->transformOrder = { TTranslation, TRotation, TScale };
			t->translation.value = Vector3d(0.1 * i, 0.2 * i, 0.3 * i);
		}
		for (int i = 0; i < 250; ++i)
		{
			subject->mCurveNames.push_back("CTRL_expressions_curve_" + std::to_string(i));
			subject->mCurveValues.push_back(0.001f * static_cast<float>(i));
		}
	}

	void Move(SubjectList& list, int frame)
	{
		Subject* subject = list.mItems[0];
		for (size_t i = 0; i < subject->mTransforms.mItems.size(); ++i)
		{
			Transform* t = subject->mTransforms.mItems[i];
			t->translation.value = Vector3d(0.1 * static_cast<double>(i) + 0.01 * frame, 0.2 * static_cast<double>(i), 0.3 * static_cast<double>(i));
		}
		for (size_t i = 0; i < subject->mCurveValues.size(); ++i)
		{
			subject->mCurveValues[i] = 0.001f * static_cast<float>(i) + 0.0001f * static_cast<float>(frame);
		}
	}
}

int main(int argc, char** argv)
{
	int iterations = 2000;
	for (int i = 1; i + 1 < argc; ++i)
	{
		if (std::strcmp(argv[i], "--iterations") == 0)
		{
			iterations = std::atoi(argv[i + 1]);
		}
	}
	if (iterations < 1)
	{
		iterations = 1;
	}

	SubjectList sender;
	BuildSubject(sender);
	std::vector<char> full;

	// Full sync (the plugin's legacy mode sends one every frame).
	Clock::time_point start = Clock::now();
	for (int i = 0; i < iterations; ++i)
	{
		sender.Serialize(full, 1.0 + i);
	}
	const double serializeUs = MicrosPerCall(start, iterations);

	SubjectList receiver;
	bool parsedAll = true;
	start = Clock::now();
	for (int i = 0; i < iterations; ++i)
	{
		parsedAll = receiver.Parse(full.data(), full.size()) && parsedAll;
	}
	const double parseUs = MicrosPerCall(start, iterations);

	// Updates after a full sync, every value moving.
	std::vector<char> update;
	size_t count = 0;
	start = Clock::now();
	for (int i = 0; i < iterations; ++i)
	{
		Move(sender, i);
		sender.SerializeUpdate(update, count, 2.0 + i);
	}
	const double updateUs = MicrosPerCall(start, iterations);

	// CRC alone, over the full sync's payload (after the 8-byte header).
	const char* payload = full.data() + 8;
	const size_t payloadSize = full.size() - 8;
	volatile std::uint32_t sink = 0;
	start = Clock::now();
	for (int i = 0; i < iterations; ++i)
	{
		sink = sink + CRCPP::CRC::Calculate(payload, payloadSize, CRCPP::CRC::CRC_32());
	}
	const double crcBitwiseUs = MicrosPerCall(start, iterations);
	start = Clock::now();
	for (int i = 0; i < iterations; ++i)
	{
		sink = sink + Crc32(payload, payloadSize);
	}
	const double crcTableUs = MicrosPerCall(start, iterations);

	const bool crcAgrees = Crc32(payload, payloadSize) == CRCPP::CRC::Calculate(payload, payloadSize, CRCPP::CRC::CRC_32());
	std::uint32_t header = 0;
	std::memcpy(&header, full.data() + 4, sizeof(header));
	const bool headerAgrees = header == Crc32(payload, payloadSize);
	const bool roundTrip = parsedAll && receiver.size() == 1 && receiver.mItems[0]->mTransforms.size() == 250
		&& receiver.mItems[0]->mCurveValues.size() == 250;

	std::printf("o3ds_core_bench: iterations=%d full_bytes=%zu update_bytes=%zu serialize_us=%.1f parse_us=%.1f "
		"serialize_update_us=%.1f crc_bitwise_us=%.1f crc_o3ds_us=%.1f\n",
		iterations, full.size(), update.size(), serializeUs, parseUs, updateUs, crcBitwiseUs, crcTableUs);

	if (!roundTrip)
	{
		std::printf("FAIL: the full sync did not parse back to one subject with 250 transforms and 250 curves\n");
		return 1;
	}
	if (!crcAgrees || !headerAgrees)
	{
		std::printf("FAIL: the table CRC differs from the bitwise CRC (or from the header)\n");
		return 1;
	}
	return 0;
}

// Wire compatibility tool for D8 (docs/adr/0009-protocol-versioning.md item
// 11). The same source builds against the current core and against the
// baseline commit (develop@7aea235), so one writer's frames can be read by
// the other reader:
//
//   compat_tool write <dir>   writes full.o3ds, delta.o3ds, quantized.o3ds
//                             and residual.o3ds (raw frames, header included)
//   compat_tool read <dir>    for each of delta, quantized and residual: a
//                             fresh SubjectList parses full.o3ds, then the
//                             frame; prints "<name> ok" or "<name> rejected:
//                             <error>", one line per frame, full first
//
// Uses only API that exists in both versions.
#include "o3ds/model.h"
#include "o3ds/predict/residual_codec.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
	void BuildSkeleton(O3DS::SubjectList& list)
	{
		O3DS::Subject* subject = list.addSubject("Actor");
		O3DS::Transform* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);
		O3DS::Transform* spine = subject->addTransform("Spine", 0);
		spine->transformOrder.push_back(O3DS::TTranslation);
		spine->transformOrder.push_back(O3DS::TRotation);
	}

	bool WriteFile(const std::string& path, const std::vector<char>& bytes)
	{
		std::ofstream out(path, std::ios::binary);
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		return static_cast<bool>(out);
	}

	bool ReadFile(const std::string& path, std::vector<char>& bytes)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return true;
	}

	int Write(const std::string& dir)
	{
		// Full snapshot and a plain delta.
		O3DS::SubjectList plain;
		BuildSkeleton(plain);
		std::vector<char> full;
		plain.Serialize(full, 1.0);
		plain.findSubject("Actor")->mTransforms[0]->translation.value = O3DS::Vector3d(0.25, 0.0, 0.0);
		std::vector<char> delta;
		size_t count = 0;
		plain.SerializeUpdate(delta, count, 1.02);

		// A quantized delta (D1): small motion, Byte tier.
		O3DS::SubjectList quantSender;
		BuildSkeleton(quantSender);
		std::vector<char> unused;
		quantSender.Serialize(unused, 1.0);
		quantSender.mQuantizationEnabled = true;
		quantSender.mQuantRanges.byteRange = 0.01;
		quantSender.mQuantRanges.halfRange = 1.0;
		quantSender.findSubject("Actor")->mTransforms[0]->translation.value = O3DS::Vector3d(0.005, -0.003, 0.002);
		std::vector<char> quantized;
		count = 0;
		quantSender.SerializeUpdate(quantized, count, 1.02);

		// A residual update (C2).
		O3DS::SubjectList residualSender;
		BuildSkeleton(residualSender);
		residualSender.findSubject("Actor")->SetResidualEncoder(std::make_unique<O3DS::ResidualEncoder>(O3DS::ResidualPredictorId::Linear));
		residualSender.Serialize(unused, 1.0);
		residualSender.findSubject("Actor")->mTransforms[0]->translation.value = O3DS::Vector3d(0.5, 0.0, 0.0);
		std::vector<char> residual;
		count = 0;
		residualSender.SerializeUpdateResidual(residual, count, 1.02);

		const bool ok = WriteFile(dir + "/full.o3ds", full) && WriteFile(dir + "/delta.o3ds", delta)
			&& WriteFile(dir + "/quantized.o3ds", quantized) && WriteFile(dir + "/residual.o3ds", residual);
		if (!ok)
		{
			std::fprintf(stderr, "could not write frames to %s\n", dir.c_str());
			return 2;
		}
		return 0;
	}

	int Read(const std::string& dir)
	{
		std::vector<char> full;
		if (!ReadFile(dir + "/full.o3ds", full))
		{
			std::fprintf(stderr, "missing %s/full.o3ds\n", dir.c_str());
			return 2;
		}

		{
			O3DS::SubjectList list;
			if (list.Parse(full.data(), full.size()))
				std::printf("full ok\n");
			else
				std::printf("full rejected: %s\n", list.mError.c_str());
		}

		for (const char* name : { "delta", "quantized", "residual" })
		{
			std::vector<char> frame;
			if (!ReadFile(dir + "/" + name + ".o3ds", frame))
			{
				std::fprintf(stderr, "missing %s/%s.o3ds\n", dir.c_str(), name);
				return 2;
			}
			O3DS::SubjectList list;
			list.Parse(full.data(), full.size());
			if (list.Parse(frame.data(), frame.size()))
				std::printf("%s ok\n", name);
			else
				std::printf("%s rejected: %s\n", name, list.mError.c_str());
		}
		return 0;
	}
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::string(argv[1]) == "write")
		return Write(argv[2]);
	if (argc == 3 && std::string(argv[1]) == "read")
		return Read(argv[2]);
	std::fprintf(stderr, "usage: compat_tool write|read <dir>\n");
	return 2;
}

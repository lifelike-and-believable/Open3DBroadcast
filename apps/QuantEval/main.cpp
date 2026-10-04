// QuantEval - measures what quantized encoding does to motion, as a receiver
// sees it (CORE-12). It plays a take through the real encoder and decoder
// (Subject::Serialize / SerializeUpdate with QuantRanges, SubjectList::Parse),
// with full syncs at the sender's interval, and compares the decoded pose with
// the input on every frame:
//
//   - rotation error per bone (degrees): mean, p95, max;
//   - jitter: frame-to-frame motion the decoded pose has that the input does
//     not (degrees per frame), for all bones and for idle bones only, since
//     idle animation is where the July 2026 quantization showed visible jitter
//     (#247);
//   - stale error: the error on bones that have not moved for a while (a
//     stopped bone left off by the last quantized value);
//   - bytes per frame.
//
// Input is the synthetic suite from PredictorEval (deterministic, a proxy for
// real mocap) or a recorded .o3dscap take of full frames. See README.md.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../PredictorEval/motion_corpus.h"
#include "o3ds/capture.h"
#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"
#include "o3ds/quant/channel_quant.h"

using namespace O3DS;

namespace
{
	//! One frame of input: per-bone translation, rotation and scale, and curves.
	struct Frame
	{
		double t = 0.0;
		std::vector<Vector3d> translations;
		std::vector<Quat> rotations;
		std::vector<Vector3d> scales;
		std::vector<float> curves;
	};

	struct Take
	{
		std::string name;
		std::vector<std::string> boneNames;
		std::vector<int> parents;
		std::vector<std::string> curveNames;
		std::vector<Frame> frames;
	};

	struct Config
	{
		std::string name;
		bool quantize = false;      //!< false: a full float frame every frame
		QuantRanges ranges;
		double deltaThreshold = 1.0e-4;
		double syncIntervalS = 1.0;
	};

	struct Results
	{
		std::vector<double> rotErrDeg;
		std::vector<double> jitterDeg;
		std::vector<double> idleJitterDeg;
		double maxTransErr = 0.0;
		double maxStaleDeg = 0.0;
		double bytes = 0.0;
		long frames = 0;
	};

	double AngleDeg(const Quat& a, const Quat& b)
	{
		const double na = std::sqrt(a.v[0] * a.v[0] + a.v[1] * a.v[1] + a.v[2] * a.v[2] + a.v[3] * a.v[3]);
		const double nb = std::sqrt(b.v[0] * b.v[0] + b.v[1] * b.v[1] + b.v[2] * b.v[2] + b.v[3] * b.v[3]);
		if (na < 1e-12 || nb < 1e-12)
			return 180.0;
		const double dot = std::abs(a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3]) / (na * nb);
		return 2.0 * std::acos(std::min(1.0, dot)) * 180.0 / 3.14159265358979323846;
	}

	double Percentile(std::vector<double> v, double p)
	{
		if (v.empty())
			return 0.0;
		std::sort(v.begin(), v.end());
		const size_t i = std::min(v.size() - 1, (size_t)std::floor(p * (double)(v.size() - 1) + 0.5));
		return v[i];
	}

	double Mean(const std::vector<double>& v)
	{
		double sum = 0.0;
		for (double x : v)
			sum += x;
		return v.empty() ? 0.0 : sum / (double)v.size();
	}

	Take FromClip(const Eval::MotionClip& clip)
	{
		Take take;
		take.name = clip.name;
		if (clip.samples.empty())
			return take;
		const size_t bones = clip.samples[0].rotations.size();
		for (size_t i = 0; i < bones; ++i)
		{
			take.boneNames.push_back("b" + std::to_string(i));
			take.parents.push_back((int)i - 1);
		}
		for (size_t c = 0; c < clip.samples[0].curves.size(); ++c)
			take.curveNames.push_back("c" + std::to_string(c));
		for (const PoseSample& s : clip.samples)
		{
			Frame f;
			f.t = s.t;
			f.translations = s.translations;
			f.rotations = s.rotations;
			f.scales = s.scales.empty() ? std::vector<Vector3d>(bones, Vector3d(1.0, 1.0, 1.0)) : s.scales;
			f.curves = s.curves;
			take.frames.push_back(std::move(f));
		}
		return take;
	}

	//! Reads a .o3dscap of frames and returns the named subject (or the first)
	//! as a take. Frames whose topology differs from the first are skipped.
	bool FromCapture(const std::string& path, const std::string& subjectName, Take& take)
	{
		std::ifstream in(path, std::ios::binary);
		CaptureHeaderInfo header;
		if (!in || !ReadCaptureHeader(in, header))
		{
			std::fprintf(stderr, "Cannot read capture header: %s\n", path.c_str());
			return false;
		}
		take.name = path;
		SubjectList list;
		CaptureRecord record;
		while (ReadCaptureRecord(in, record))
		{
			if (!list.Parse(record.wire_bytes.data(), record.wire_bytes.size()))
				continue;
			Subject* subject = subjectName.empty() ? (list.size() > 0 ? list[0] : nullptr) : list.findSubject(subjectName);
			if (subject == nullptr)
				continue;
			if (take.boneNames.empty())
			{
				for (auto* t : subject->mTransforms)
				{
					take.boneNames.push_back(t->mName);
					take.parents.push_back(t->mParentId);
				}
				take.curveNames = subject->mCurveNames;
			}
			if (subject->mTransforms.size() != take.boneNames.size() || subject->mCurveValues.size() != take.curveNames.size())
				continue;
			Frame f;
			f.t = list.mTime;
			for (auto* t : subject->mTransforms)
			{
				f.translations.push_back(t->translation.value);
				f.rotations.push_back(t->rotation.value);
				f.scales.push_back(t->scale.value);
			}
			f.curves = subject->mCurveValues;
			take.frames.push_back(std::move(f));
		}
		return !take.frames.empty();
	}

	Results Run(const Take& take, const Config& config, double idleDegPerFrame, int staleFrames)
	{
		Results r;
		SubjectList sender;
		Subject* subject = sender.addSubject("Eval");
		for (size_t i = 0; i < take.boneNames.size(); ++i)
		{
			Transform* t = subject->addTransform(take.boneNames[i], take.parents[i]);
			t->transformOrder.push_back(O3DS::TTranslation);
			t->transformOrder.push_back(O3DS::TRotation);
			t->transformOrder.push_back(O3DS::TScale);
		}
		subject->mCurveNames = take.curveNames;
		subject->mCurveValues.assign(take.curveNames.size(), 0.0f);

		SubjectList receiver;
		const size_t bones = take.boneNames.size();
		std::vector<Quat> prevRecv(bones), prevTruth(bones);
		std::vector<int> stillFor(bones, 0);
		double lastSync = -1.0e30;
		std::vector<char> buf;

		for (size_t f = 0; f < take.frames.size(); ++f)
		{
			const Frame& in = take.frames[f];
			for (size_t i = 0; i < bones; ++i)
			{
				subject->mTransforms[i]->translation.value = in.translations[i];
				subject->mTransforms[i]->rotation.value = in.rotations[i];
				subject->mTransforms[i]->scale.value = in.scales[i];
			}
			for (size_t c = 0; c < in.curves.size() && c < subject->mCurveValues.size(); ++c)
				subject->mCurveValues[c] = in.curves[c];

			const bool fullSync = !config.quantize || (in.t - lastSync) >= config.syncIntervalS;
			if (fullSync)
			{
				subject->Serialize(buf, in.t);
				lastSync = in.t;
			}
			else
			{
				size_t count = 0;
				subject->SerializeUpdate(buf, count, config.deltaThreshold, in.t, &config.ranges);
			}
			r.bytes += (double)buf.size();
			if (!receiver.Parse(buf.data(), buf.size()))
				continue;
			Subject* got = receiver.findSubject("Eval");
			if (got == nullptr || got->mTransforms.size() != bones)
				continue;
			++r.frames;

			for (size_t i = 0; i < bones; ++i)
			{
				const Quat& recv = got->mTransforms[i]->rotation.value;
				const Quat& truth = in.rotations[i];
				const double err = AngleDeg(recv, truth);
				r.rotErrDeg.push_back(err);
				const Vector3d& tr = got->mTransforms[i]->translation.value;
				const double dt = std::sqrt(std::pow(tr.v[0] - in.translations[i].v[0], 2) + std::pow(tr.v[1] - in.translations[i].v[1], 2) + std::pow(tr.v[2] - in.translations[i].v[2], 2));
				r.maxTransErr = std::max(r.maxTransErr, dt);
				if (f > 0)
				{
					const double truthStep = AngleDeg(truth, prevTruth[i]);
					const double recvStep = AngleDeg(recv, prevRecv[i]);
					const double jitter = std::abs(recvStep - truthStep);
					r.jitterDeg.push_back(jitter);
					const bool idle = truthStep < idleDegPerFrame;
					if (idle)
						r.idleJitterDeg.push_back(jitter);
					stillFor[i] = idle ? stillFor[i] + 1 : 0;
					if (stillFor[i] >= staleFrames)
						r.maxStaleDeg = std::max(r.maxStaleDeg, err);
				}
				prevRecv[i] = recv;
				prevTruth[i] = truth;
			}
		}
		return r;
	}

	void PrintHeader(bool csv)
	{
		if (csv)
			std::printf("take,config,frames,bytes_per_frame,rot_err_mean_deg,rot_err_p95_deg,rot_err_max_deg,jitter_p95_deg,jitter_max_deg,idle_jitter_p95_deg,idle_jitter_max_deg,stale_max_deg,trans_err_max\n");
		else
			std::printf("%-14s %-26s %8s %9s %9s %9s %9s %9s %9s %9s %9s %9s\n", "take", "config", "B/frame", "err mean", "err p95", "err max", "jit p95", "jit max", "idle p95", "idle max", "stale", "trans max");
	}

	void Print(bool csv, const std::string& take, const Config& c, const Results& r)
	{
		const double bpf = r.frames ? r.bytes / (double)r.frames : 0.0;
		if (csv)
			std::printf("%s,%s,%ld,%.1f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.6f\n", take.c_str(), c.name.c_str(), r.frames, bpf,
				Mean(r.rotErrDeg), Percentile(r.rotErrDeg, 0.95), Percentile(r.rotErrDeg, 1.0),
				Percentile(r.jitterDeg, 0.95), Percentile(r.jitterDeg, 1.0),
				Percentile(r.idleJitterDeg, 0.95), Percentile(r.idleJitterDeg, 1.0), r.maxStaleDeg, r.maxTransErr);
		else
			std::printf("%-14s %-26s %8.0f %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f %9.5f\n", take.c_str(), c.name.c_str(), bpf,
				Mean(r.rotErrDeg), Percentile(r.rotErrDeg, 0.95), Percentile(r.rotErrDeg, 1.0),
				Percentile(r.jitterDeg, 0.95), Percentile(r.jitterDeg, 1.0),
				Percentile(r.idleJitterDeg, 0.95), Percentile(r.idleJitterDeg, 1.0), r.maxStaleDeg, r.maxTransErr);
	}
}

int main(int argc, char** argv)
{
	bool csv = false;
	std::string capturePath;
	std::string subjectName;
	double idleDegPerFrame = 0.05;
	int staleFrames = 30;
	Config custom;
	bool haveCustom = false;
	custom.name = "custom";
	custom.quantize = true;

	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		auto next = [&](double& out) { if (i + 1 < argc) out = std::atof(argv[++i]); };
		if (a == "--csv") csv = true;
		else if (a == "--capture" && i + 1 < argc) capturePath = argv[++i];
		else if (a == "--subject" && i + 1 < argc) subjectName = argv[++i];
		else if (a == "--idle-deg") next(idleDegPerFrame);
		else if (a == "--stale-frames" && i + 1 < argc) staleFrames = std::atoi(argv[++i]);
		else if (a == "--byte-range") { next(custom.ranges.byteRange); haveCustom = true; }
		else if (a == "--half-range") { next(custom.ranges.halfRange); haveCustom = true; }
		else if (a == "--hysteresis") { next(custom.ranges.hysteresisFactor); haveCustom = true; }
		else if (a == "--delta") { next(custom.deltaThreshold); haveCustom = true; }
		else if (a == "--sync") { next(custom.syncIntervalS); haveCustom = true; }
		else if (a == "--help" || a == "-h")
		{
			std::printf("QuantEval [--capture file.o3dscap [--subject name]] [--csv] [--idle-deg 0.05] [--stale-frames 30]\n"
				"          [--byte-range R --half-range R --hysteresis H --delta D --sync S]  (adds a custom config)\n");
			return 0;
		}
	}

	// Full float frames (the reference), the UE defaults (O3DSenderComponent.h), and the same
	// without the Byte tier (byteRange 0: rotations and translations use Half or Full).
	std::vector<Config> configs;
	Config full;
	full.name = "full-float";
	configs.push_back(full);
	Config defaults;
	defaults.name = "quant-ue-defaults";
	defaults.quantize = true;
	configs.push_back(defaults);
	Config noByte = defaults;
	noByte.name = "quant-no-byte-tier";
	noByte.ranges.byteRange = 0.0;
	configs.push_back(noByte);
	if (haveCustom)
		configs.push_back(custom);

	std::vector<Take> takes;
	if (!capturePath.empty())
	{
		Take take;
		if (!FromCapture(capturePath, subjectName, take))
		{
			std::fprintf(stderr, "No usable frames in %s\n", capturePath.c_str());
			return 1;
		}
		takes.push_back(std::move(take));
	}
	else
	{
		for (const Eval::ClipSpec& spec : Eval::StandardSuite())
			takes.push_back(FromClip(Eval::GenerateClip(spec)));
		if (!csv)
			std::printf("Synthetic suite (a proxy for real mocap; see README.md). Idle: input step < %.3f deg/frame.\n\n", idleDegPerFrame);
	}

	PrintHeader(csv);
	for (const Take& take : takes)
		for (const Config& c : configs)
			Print(csv, take.name, c, Run(take, c, idleDegPerFrame, staleFrames));
	return 0;
}

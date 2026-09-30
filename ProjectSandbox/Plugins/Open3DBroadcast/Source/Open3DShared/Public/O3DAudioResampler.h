// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Stateful sample-rate converter for one interleaved audio stream (SND-21).
 *
 * - Carries its read position across buffers as an exact fraction (in units of
 *   1 / (OutRate / gcd)), so the output length never drifts from InFrames * OutRate / InRate
 *   and there is no discontinuity at buffer boundaries: splitting the input differently gives
 *   bit-identical output.
 * - When downsampling, an 8th-order Butterworth low-pass at 0.45 * OutRate runs before
 *   interpolation, so content above the new Nyquist frequency is attenuated instead of aliased.
 * - Interpolation between filtered input frames is linear.
 *
 * Not thread-safe: one instance per producer thread.
 */
class OPEN3DSHARED_API FO3DAudioResampler
{
public:
	FO3DAudioResampler();
	~FO3DAudioResampler();

	FO3DAudioResampler(const FO3DAudioResampler&) = delete;
	FO3DAudioResampler& operator=(const FO3DAudioResampler&) = delete;

	/**
	 * Set the conversion. Does nothing when the parameters are unchanged; otherwise resets the
	 * stream state. Returns false (and leaves the resampler unconfigured) for rates or channel
	 * counts that are not positive.
	 */
	bool Configure(int32 InSampleRate, int32 OutSampleRate, int32 InNumChannels);

	/** Forget the stream history (filter state, read position, last frame). Keeps the configuration. */
	void Reset();

	/**
	 * Convert NumFrames interleaved input frames. OutInterleaved is overwritten with the output
	 * frames (it keeps its allocation between calls). OutFirstFrameOffsetSec is the time of the
	 * first output frame relative to the first input frame of this call; it lies in
	 * (-1 / InSampleRate, 1 / OutSampleRate). Returns the number of output frames, which may be 0.
	 */
	int32 Process(const float* InInterleaved, int32 NumFrames, TArray<float>& OutInterleaved, double& OutFirstFrameOffsetSec);

	bool IsConfigured() const { return NumChannels > 0; }
	int32 GetInSampleRate() const { return InRate; }
	int32 GetOutSampleRate() const { return OutRate; }
	int32 GetNumChannels() const { return NumChannels; }

private:
	struct FBiquad
	{
		float B0 = 1.0f;
		float B1 = 0.0f;
		float B2 = 0.0f;
		float A1 = 0.0f;
		float A2 = 0.0f;
	};

	int32 InRate = 0;
	int32 OutRate = 0;
	int32 NumChannels = 0;

	/** Positions are counted in input frames times PositionDenominator. */
	int64 PositionDenominator = 1;
	/** Input frames per output frame, times PositionDenominator. */
	int64 PositionStep = 1;
	/** Position of the next output frame relative to the current buffer's first frame; >= -PositionDenominator. */
	int64 NextPosition = 0;

	bool bHasPreviousFrame = false;
	/** Last (filtered) input frame of the previous buffer, used for positions in (-1, 0). */
	TArray<float> PreviousFrame;

	/** Anti-aliasing sections; empty when not downsampling. */
	TArray<FBiquad> Sections;
	/** Two state values per section per channel (transposed direct form II). */
	TArray<float> FilterState;
	/** Filtered copy of the current buffer. */
	TArray<float> Filtered;
};

// ResamplingNAM — wraps a nam::DSP model with transparent sample-rate
// conversion via AudioDSPTools' ResamplingContainer (Lanczos, A=12). NAM
// models typically run at 48 kHz; the session may run at any rate.
//
// Carried over unchanged from this plug-in's earlier framework-based build,
// where it was proven against the same DSP core.

#pragma once

// Compatibility shims required by the iPlug2-derived LanczosResampler that
// AudioDSPTools' ResamplingContainer bundles. These symbols are normally
// provided by iPlug2's IPlugPlatform.h which we don't include.
#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 128
#endif
namespace iplug
{
static constexpr double PI = 3.14159265358979323846;
}

#include "NAM/dsp.h"
#include "NAM/slimmable.h"
#include "ResamplingContainer/ResamplingContainer.h"

#include <cmath>
#include <memory>

namespace NAMix
{

inline double GetNamEncapsulatedSampleRate(const std::unique_ptr<nam::DSP> &m)
{
    const double reported = m->GetExpectedSampleRate();
    return reported > 0.0 ? reported : 48000.0;
}

//------------------------------------------------------------------------
class ResamplingNAM : public nam::DSP
{
public:
    ResamplingNAM(std::unique_ptr<nam::DSP> enc, double externalSampleRate)
        : nam::DSP(1, 1, externalSampleRate), mEncapsulated(std::move(enc)),
          mResampler(GetNamEncapsulatedSampleRate(mEncapsulated))
    {
        if (mEncapsulated->HasLoudness())
            SetLoudness(mEncapsulated->GetLoudness());
        if (mEncapsulated->HasInputLevel())
            SetInputLevel(mEncapsulated->GetInputLevel());
        if (mEncapsulated->HasOutputLevel())
            SetOutputLevel(mEncapsulated->GetOutputLevel());
        // Size the buffers now, but do NOT prewarm here. The owner calls
        // Reset() with the real block size immediately afterwards and that one
        // prewarms; prewarming here as well runs a whole receptive field of
        // inference on the message thread twice per load, for nothing.
        resetInternal(externalSampleRate, 2048, false);
    }

    void process(NAM_SAMPLE **input, NAM_SAMPLE **output, const int num_frames) override
    {
        if (resamplerBypassed()) {
            mEncapsulated->process(input, output, num_frames);
        } else {
            mResampler.ProcessBlock(input, output, num_frames,
                                    [this](NAM_SAMPLE **in, NAM_SAMPLE **out, int n) {
                                        mEncapsulated->process(in, out, n);
                                    });
        }
    }

    void Reset(const double sampleRate, const int maxBlockSize) override
    {
        resetInternal(sampleRate, maxBlockSize, true);
    }

    // Latency this wrapper adds, in samples at the session rate. Zero when the
    // model already runs at the session rate, because process() then bypasses
    // the resampler entirely — reporting its latency anyway tells the host to
    // compensate for a delay that is not there, which pulls the track early.
    int GetLatency() const
    {
        return resamplerBypassed() ? 0 : mResampler.GetLatency();
    }

    // Non-null when the wrapped model supports dynamic size reduction (the
    // "slimmable" A2 WaveNet variant); the Slim parameter targets this.
    nam::SlimmableModel *GetSlimmableModel()
    {
        return dynamic_cast<nam::SlimmableModel *>(mEncapsulated.get());
    }

private:
    // The one place that decides whether the resampler is in the path at all.
    // process() and GetLatency() must agree on this, or the plug-in reports a
    // latency it is not actually introducing.
    bool resamplerBypassed() const
    {
        return GetNamEncapsulatedSampleRate(mEncapsulated) == mExpectedSampleRate;
    }

    void resetInternal(const double sampleRate, const int maxBlockSize, const bool prewarm)
    {
        mExpectedSampleRate = sampleRate;
        mMaxBufferSize = maxBlockSize;
        mResampler.Reset(sampleRate, maxBlockSize);

        const double encRate = GetNamEncapsulatedSampleRate(mEncapsulated);
        const int encBlock =
            static_cast<int>(std::ceil(static_cast<double>(maxBlockSize) * encRate / sampleRate));
        if (prewarm)
            mEncapsulated->ResetAndPrewarm(encRate, encBlock);
        else
            mEncapsulated->Reset(encRate, encBlock);
    }

    std::unique_ptr<nam::DSP> mEncapsulated;
    dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> mResampler;
};

} // namespace NAMix

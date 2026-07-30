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
        Reset(externalSampleRate, 2048);
    }

    void process(NAM_SAMPLE **input, NAM_SAMPLE **output, const int num_frames) override
    {
        const double encRate = GetNamEncapsulatedSampleRate(mEncapsulated);
        if (encRate == mExpectedSampleRate) {
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
        mExpectedSampleRate = sampleRate;
        mMaxBufferSize = maxBlockSize;
        mResampler.Reset(sampleRate, maxBlockSize);

        const double encRate = GetNamEncapsulatedSampleRate(mEncapsulated);
        const int encBlock =
            static_cast<int>(std::ceil(static_cast<double>(maxBlockSize) * encRate / sampleRate));
        mEncapsulated->ResetAndPrewarm(encRate, encBlock);
    }

    int GetLatency() const
    {
        return mResampler.GetLatency();
    }

    // Non-null when the wrapped model supports dynamic size reduction (the
    // "slimmable" A2 WaveNet variant); the Slim parameter targets this.
    nam::SlimmableModel *GetSlimmableModel()
    {
        return dynamic_cast<nam::SlimmableModel *>(mEncapsulated.get());
    }

private:
    std::unique_ptr<nam::DSP> mEncapsulated;
    dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> mResampler;
};

} // namespace NAMix

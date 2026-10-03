#pragma once

#include <algorithm>
#include <cmath>

namespace opm86
{

// Two cascaded RBJ low-pass sections (4th order), stereo.
class StereoLowPass
{
public:
    void setup(double cutoff, double sampleRate)
    {
        constexpr double kPi = 3.14159265358979323846;
        const double w = 2.0 * kPi * std::min(cutoff, sampleRate * 0.45) / sampleRate;
        const double cw = std::cos(w);
        // Q values of a 4th-order Butterworth split into two biquads.
        const double qs[2] = { 0.5412, 1.3066 };
        for (int s = 0; s < 2; ++s)
        {
            const double alpha = std::sin(w) / (2.0 * qs[s]);
            const double a0 = 1.0 + alpha;
            Coefs& c = coefs_[s];
            c.b0 = static_cast<float>((1.0 - cw) / 2.0 / a0);
            c.b1 = static_cast<float>((1.0 - cw) / a0);
            c.b2 = c.b0;
            c.a1 = static_cast<float>(-2.0 * cw / a0);
            c.a2 = static_cast<float>((1.0 - alpha) / a0);
        }
        reset();
    }

    void reset()
    {
        for (auto& stage : state_)
            for (auto& ch : stage)
                ch = {};
    }

    void process(float& left, float& right)
    {
        for (int s = 0; s < 2; ++s)
        {
            left = run(coefs_[s], state_[s][0], left);
            right = run(coefs_[s], state_[s][1], right);
        }
    }

private:
    struct Coefs { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State { float z1 = 0, z2 = 0; };

    static float run(const Coefs& c, State& s, float x)
    {
        const float y = c.b0 * x + s.z1;
        s.z1 = c.b1 * x - c.a1 * y + s.z2;
        s.z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    Coefs coefs_[2];
    State state_[2][2];
};

// Pulls samples from a chip at its native rate and interpolates them to
// the host rate with a 4-point Hermite curve.
class Resampler
{
public:
    void setup(double sourceRate, double targetRate)
    {
        step_ = sourceRate / targetRate;
        reset();
    }

    // offset, from 0 to 1, delays the output by that fraction of a source
    // sample, to start exactly at a point between two source samples.
    void reset(double offset = 0.0)
    {
        phase_ = 1.0 + offset;
        for (auto& h : history_)
            h[0] = h[1] = 0.0f;
    }

    // next(float& left, float& right) produces one source sample.
    template <typename Source>
    void process(float* left, float* right, int count, Source&& next)
    {
        for (int i = 0; i < count; ++i)
        {
            while (phase_ >= 1.0)
            {
                for (int k = 0; k < 3; ++k)
                {
                    history_[k][0] = history_[k + 1][0];
                    history_[k][1] = history_[k + 1][1];
                }
                next(history_[3][0], history_[3][1]);
                phase_ -= 1.0;
            }
            const auto t = static_cast<float>(phase_);
            left[i] = hermite(0, t);
            right[i] = hermite(1, t);
            phase_ += step_;
        }
    }

private:
    float hermite(int ch, float t) const
    {
        const float y0 = history_[0][ch], y1 = history_[1][ch], y2 = history_[2][ch], y3 = history_[3][ch];
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    double step_ = 1.0;
    double phase_ = 1.0;
    float history_[4][2] {};
};

} // namespace opm86

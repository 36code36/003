#include "PluginProcessor.h"
#include <cmath>
#include <vector>

namespace
{
constexpr float twoPi = 6.28318530718f;

enum Model { Wurli = 0, Rhodes, FMEP, Clav, Piano };

struct Partial { float ratio, amp, decay; };

// Partial tables for the additive instruments (built once)
const std::vector<Partial>& getTable (int m)
{
    static const std::vector<std::vector<Partial>> tables = []
    {
        std::vector<std::vector<Partial>> t (5);

        // Wurlitzer: reed, strong 2nd harmonic, slightly "barky"
        t[Wurli] = { {1,1,1}, {2,.55f,1.6f}, {3,.22f,2.2f}, {4,.10f,3.f}, {5,.05f,3.5f}, {6,.03f,4.f} };

        // Rhodes: soft fundamental plus short inharmonic "bell" tine partials
        t[Rhodes] = { {1,1,1}, {2,.15f,1.4f}, {3,.06f,1.8f}, {6.f,.22f,5.f}, {9.2f,.08f,8.f} };

        // Clavinet: bright pluck, upper harmonics die fast
        for (int n = 1; n <= 12; ++n)
            t[Clav].push_back ({ (float) n, 1.f / std::pow ((float) n, .8f), 1.f + .5f * (float) n });

        // Piano: inharmonic partials, doubled (2 strings) in the voice for beating
        for (int n = 1; n <= 10; ++n)
            t[Piano].push_back ({ (float) n * std::sqrt (1.f + .0004f * (float) (n * n)),
                                  std::pow ((float) n, -.9f), 1.f + .5f * (float) (n - 1) });
        return t;
    }();
    return tables[(size_t) m];
}

struct PianoSound : juce::SynthesiserSound
{
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

class PianoVoice : public juce::SynthesiserVoice
{
public:
    explicit PianoVoice (juce::AudioProcessorValueTreeState& s) : apvts (s) {}

    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<PianoSound*> (s) != nullptr; }

    void startNote (int midi, float v, juce::SynthesiserSound*, int) override
    {
        const double sr = getSampleRate();
        model = (int) apvts.getRawParameterValue ("model")->load();
        const float decayP = apvts.getRawParameterValue ("decay")->load();
        const float f0 = 440.f * std::pow (2.f, ((float) midi - 69.f) / 12.f);

        const float T0[] = { 2.5f, 4.f, 4.f, .6f, 6.f };
        const float rel[] = { .10f, .20f, .20f, .03f, .20f };
        float T = T0[model] * std::pow (2.f, -((float) midi - 60.f) / 24.f) * decayP;
        T = juce::jlimit (.05f, 30.f, T);
        baseRate = 6.9f / T; // 60 dB over T seconds

        level = v * .18f;
        atk = 0.f;
        atkInc = 1.f / (.002f * (float) sr);
        relGain = 1.f;
        relCoef = std::exp (-1.f / (rel[model] * (float) sr));
        releasing = false;
        std::fill (std::begin (ph), std::end (ph), 0.f);

        if (model == FMEP)
        {
            dphi[0] = twoPi * f0 / (float) sr;          // carrier
            dphi[1] = dphi[0];                          // modulator 1:1
            dphi[2] = twoPi * f0 * 14.f / (float) sr;   // tine
            tineOn = f0 * 14.f < .45f * (float) sr;
            fmIdx = 1.f + 3.f * v;
            envA = envI = envT = 1.f;
            cA = std::exp (-baseRate / (float) sr);
            cI = std::exp (-baseRate * 2.5f / (float) sr);
            cT = std::exp (-baseRate * 10.f / (float) sr);
            return;
        }

        parts = &getTable (model);
        nParts = (int) parts->size();
        nOsc = (model == Piano) ? 2 : 1;
        for (int i = 0; i < nParts; ++i)
        {
            const auto& p = (*parts)[(size_t) i];
            const float freq = f0 * p.ratio;
            amps[i] = (freq < .45f * (float) sr) ? p.amp * std::pow (v, .25f * (float) i) : 0.f;
            env[i] = 1.f;
            coef[i] = std::exp (-baseRate * p.decay / (float) sr);
            dphi[2 * i] = twoPi * freq / (float) sr;
            dphi[2 * i + 1] = dphi[2 * i] * 1.0007f;
        }
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff) releasing = true;
        else clearCurrentNote();
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! isVoiceActive()) return;
        const int chans = out.getNumChannels();

        for (int s = 0; s < num; ++s)
        {
            float x = 0.f, main = 0.f;

            if (model == FMEP)
            {
                x = std::sin (ph[0] + fmIdx * envI * std::sin (ph[1])) * envA;
                if (tineOn) x += .25f * std::sin (ph[2]) * envT;
                for (int k = 0; k < 3; ++k) { ph[k] += dphi[k]; if (ph[k] > twoPi) ph[k] -= twoPi; }
                envA *= cA; envI *= cI; envT *= cT;
                main = envA;
            }
            else
            {
                for (int i = 0; i < nParts; ++i)
                {
                    if (amps[i] <= 0.f) continue;
                    const float a = amps[i] * env[i];
                    if (nOsc == 2)
                    {
                        x += a * .55f * (std::sin (ph[2 * i]) + std::sin (ph[2 * i + 1]));
                        ph[2 * i + 1] += dphi[2 * i + 1];
                        if (ph[2 * i + 1] > twoPi) ph[2 * i + 1] -= twoPi;
                    }
                    else
                        x += a * std::sin (ph[2 * i]);
                    ph[2 * i] += dphi[2 * i];
                    if (ph[2 * i] > twoPi) ph[2 * i] -= twoPi;
                    env[i] *= coef[i];
                }
                main = env[0];
                if (model == Wurli)  x += .40f * x * x;  // asymmetric pickup distortion
                if (model == Rhodes) x += .25f * x * x;
            }

            atk = std::min (1.f, atk + atkInc);
            x *= atk * level * relGain;
            if (releasing) relGain *= relCoef;

            for (int c = 0; c < chans; ++c)
                out.addSample (c, start + s, x);

            if (relGain < .001f || main < .0005f)
            {
                clearCurrentNote();
                break;
            }
        }
    }

private:
    juce::AudioProcessorValueTreeState& apvts;
    const std::vector<Partial>* parts = nullptr;
    int model = 0, nParts = 0, nOsc = 1;
    float baseRate = 1.f, level = 0.f, atk = 0.f, atkInc = 0.f, relGain = 1.f, relCoef = .999f;
    bool releasing = false, tineOn = false;
    float ph[24] {}, dphi[24] {}, amps[12] {}, env[12] {}, coef[12] {};
    float fmIdx = 1.f, envA = 1.f, envI = 1.f, envT = 1.f, cA = 1.f, cI = 1.f, cT = 1.f;
};
} // namespace

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout MultiKeysProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "model", 1 }, "Instrument",
           StringArray { "Wurlitzer", "Rhodes", "FM E-Piano", "Clavinet", "Grand Piano" }, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "decay", 1 }, "Decay",
           NormalisableRange<float> (.25f, 3.f, .01f, .6f), 1.f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tremRate", 1 }, "Tremolo Rate",
           NormalisableRange<float> (.5f, 9.f), 5.f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tremDepth", 1 }, "Tremolo Depth",
           NormalisableRange<float> (0.f, 1.f), .25f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tone",
           NormalisableRange<float> (500.f, 18000.f, 1.f, .3f), 12000.f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "drive", 1 }, "Drive",
           NormalisableRange<float> (0.f, 1.f), .2f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "reverb", 1 }, "Reverb",
           NormalisableRange<float> (0.f, 1.f), .2f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "volume", 1 }, "Volume",
           NormalisableRange<float> (0.f, 1.f), .8f));
    return l;
}

MultiKeysProcessor::MultiKeysProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    for (int i = 0; i < 16; ++i) synth.addVoice (new PianoVoice (apvts));
    synth.addSound (new PianoSound());
    for (int m = 0; m < 5; ++m) getTable (m); // build tables up front
}

void MultiKeysProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    synth.setCurrentPlaybackSampleRate (sampleRate);
    reverb.setSampleRate (sampleRate);
    reverb.reset();
    lfoPhase = lpL = lpR = 0.f;
}

bool MultiKeysProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void MultiKeysProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    synth.renderNextBlock (buffer, midi, 0, n);

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    const float rate = get ("tremRate"), depth = get ("tremDepth"), cutoff = get ("tone");
    const float drive = get ("drive"), wet = get ("reverb"), vol = get ("volume") * 2.f;

    const float a = std::exp (-twoPi * cutoff / (float) sr);
    const float g = 1.f + drive * 8.f, norm = 1.f / std::sqrt (g);
    const float lfoInc = twoPi * rate / (float) sr;

    auto* L = buffer.getWritePointer (0);
    auto* R = buffer.getWritePointer (1);

    for (int s = 0; s < n; ++s)
    {
        const float lfo = std::sin (lfoPhase);
        lfoPhase += lfoInc;
        if (lfoPhase > twoPi) lfoPhase -= twoPi;

        // opposite-phase tremolo on L/R = classic suitcase auto-pan
        const float gl = 1.f - depth * .5f * (1.f + lfo);
        const float gr = 1.f - depth * .5f * (1.f - lfo);

        lpL = (1.f - a) * (L[s] * gl) + a * lpL;
        lpR = (1.f - a) * (R[s] * gr) + a * lpR;
        L[s] = std::tanh (lpL * g) * norm * vol;
        R[s] = std::tanh (lpR * g) * norm * vol;
    }

    juce::Reverb::Parameters rp;
    rp.roomSize = .6f; rp.damping = .5f; rp.width = 1.f;
    rp.wetLevel = wet * .4f; rp.dryLevel = 1.f; rp.freezeMode = 0.f;
    reverb.setParameters (rp);
    reverb.processStereo (L, R, n);
}

juce::AudioProcessorEditor* MultiKeysProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void MultiKeysProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void MultiKeysProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MultiKeysProcessor(); }

#include "Parameters.h"

namespace opm86
{

namespace
{

using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

const juce::StringArray kWaveNames { "Saw", "Square", "Triangle", "Noise" };
const juce::StringArray kPanNames { "Both", "Left", "Right" };

struct FieldSpec
{
    const char* label;
    int min, max;
};

// Ranges in kOperatorFields order; "am" is a toggle.
constexpr FieldSpec kFieldSpecs[kOperatorFieldCount] = {
    { "Total Level", 0, 127 }, { "Attack", 0, 31 },   { "Decay 1", 0, 31 },  { "Decay 2", 0, 31 },
    { "Release", 0, 15 },      { "Sustain", 0, 15 },  { "Multiple", 0, 15 }, { "Detune 1", -3, 3 },
    { "Detune 2", 0, 3 },      { "Key Scale", 0, 3 }, { "AM", 0, 1 },
};

int fieldValue(const FmOperator& o, int field)
{
    const int values[kOperatorFieldCount] = { o.tl, o.ar, o.d1r, o.d2r, o.rr, o.d1l, o.mul, o.dt1, o.dt2, o.ks, o.amEn };
    return values[field];
}

void addInt(Layout& layout, const juce::String& id, const juce::String& name, int min, int max, int value)
{
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID { id, 1 }, name, min, max, value));
}

void addChoice(Layout& layout, const juce::String& id, const juce::String& name, const juce::StringArray& choices,
               int value)
{
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { id, 1 }, name, choices, value));
}

void addFloat(Layout& layout, const juce::String& id, const juce::String& name, float value)
{
    const auto attributes = juce::AudioParameterFloatAttributes {}
                                .withStringFromValueFunction([](float v, int) {
                                    return juce::String(juce::roundToInt(v * 100.0f)) + "%";
                                })
                                .withValueFromStringFunction([](const juce::String& text) {
                                    return text.getFloatValue() / 100.0f;
                                });
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { id, 1 }, name,
                                                           juce::NormalisableRange<float>(0.0f, 1.0f), value,
                                                           attributes));
}

// Hi-hats cut each other off, as on a drum machine.
int defaultChoke(int pad)
{
    const int note = kFirstPadNote + pad;
    return note == 42 || note == 44 || note == 46 ? 1 : 0;
}

} // namespace

juce::String operatorParamId(int op, const char* field)
{
    return juce::String(kOperatorNames[op]).toLowerCase() + "_" + field;
}

juce::String padParamId(int pad, const char* field)
{
    return "pad" + juce::String(pad + 1).paddedLeft('0', 2) + "_" + field;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    Layout layout;
    const FmPatch& p = factoryPatches().front();

    addFloat(layout, "fm_level", "FM Level", 0.7f);
    addFloat(layout, "pcm_level", "PCM Level", 0.7f);
    addFloat(layout, "vel_sens", "Velocity Sensitivity", 0.5f);
    addInt(layout, "bend_range", "Bend Range", 0, 24, 2);
    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { "drum_channel", 1 }, "Drum Channel", 0, Engine::kDrumsOnAllChannels, 10,
        juce::AudioParameterIntAttributes {}
            .withStringFromValueFunction([](int v, int) {
                return v == 0 ? juce::String("Off") : v == Engine::kDrumsOnAllChannels ? juce::String("All") : juce::String(v);
            })
            .withValueFromStringFunction([](const juce::String& text) {
                return text.equalsIgnoreCase("all") ? Engine::kDrumsOnAllChannels
                                                    : text.equalsIgnoreCase("off") ? 0 : text.getIntValue();
            })));

    addInt(layout, "alg", "Algorithm", 0, 7, p.alg);
    addInt(layout, "fb", "Feedback", 0, 7, p.fb);
    addInt(layout, "lfo_freq", "LFO Rate", 0, 255, p.lfoFreq);
    addChoice(layout, "lfo_wave", "LFO Wave", kWaveNames, p.lfoWave);
    addInt(layout, "pmd", "PM Depth", 0, 127, p.pmd);
    addInt(layout, "amd", "AM Depth", 0, 127, p.amd);
    addInt(layout, "pms", "PM Sensitivity", 0, 7, p.pms);
    addInt(layout, "ams", "AM Sensitivity", 0, 3, p.ams);
    addChoice(layout, "pan", "Output", kPanNames, static_cast<int>(p.pan));

    for (int op = 0; op < 4; ++op)
    {
        for (int f = 0; f < kOperatorFieldCount; ++f)
        {
            const juce::String id = operatorParamId(op, kOperatorFields[f]);
            const juce::String name = juce::String(kOperatorNames[op]) + " " + kFieldSpecs[f].label;
            const int value = fieldValue(p.op[static_cast<size_t>(op)], f);
            if (f == kOperatorFieldCount - 1)
                layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { id, 1 }, name, value != 0));
            else
                addInt(layout, id, name, kFieldSpecs[f].min, kFieldSpecs[f].max, value);
        }
    }

    for (int pad = 0; pad < kPads; ++pad)
    {
        const juce::String name = "Pad " + juce::String(kFirstPadNote + pad);
        addInt(layout, padParamId(pad, "tune"), name + " Tune", -24, 24, 0);
        addInt(layout, padParamId(pad, "level"), name + " Level", 0, 127, 100);
        addInt(layout, padParamId(pad, "pan"), name + " Pan", -63, 63, 0);
        addInt(layout, padParamId(pad, "choke"), name + " Choke", 0, 3, defaultChoke(pad));
    }
    return layout;
}

ParameterRefs::ParameterRefs(juce::AudioProcessorValueTreeState& state)
{
    auto get = [&state](const juce::String& id) {
        auto* value = state.getRawParameterValue(id);
        jassert(value != nullptr);
        return value;
    };

    alg_ = get("alg");
    fb_ = get("fb");
    lfoFreq_ = get("lfo_freq");
    lfoWave_ = get("lfo_wave");
    pmd_ = get("pmd");
    amd_ = get("amd");
    pms_ = get("pms");
    ams_ = get("ams");
    pan_ = get("pan");
    for (int op = 0; op < 4; ++op)
        for (int f = 0; f < kOperatorFieldCount; ++f)
            ops_[static_cast<size_t>(op)][static_cast<size_t>(f)] = get(operatorParamId(op, kOperatorFields[f]));
    for (int pad = 0; pad < kPads; ++pad)
    {
        auto& refs = pads_[static_cast<size_t>(pad)];
        refs = { get(padParamId(pad, "tune")), get(padParamId(pad, "level")), get(padParamId(pad, "pan")),
                 get(padParamId(pad, "choke")) };
    }
    fmLevel_ = get("fm_level");
    pcmLevel_ = get("pcm_level");
    velSens_ = get("vel_sens");
    bendRange_ = get("bend_range");
    drumChannel_ = get("drum_channel");
}

FmPatch ParameterRefs::patch() const
{
    auto u8 = [](Value v) { return static_cast<uint8_t>(std::lround(v->load())); };

    FmPatch p;
    p.name.clear();
    p.alg = u8(alg_);
    p.fb = u8(fb_);
    p.lfoFreq = u8(lfoFreq_);
    p.lfoWave = u8(lfoWave_);
    p.pmd = u8(pmd_);
    p.amd = u8(amd_);
    p.pms = u8(pms_);
    p.ams = u8(ams_);
    p.pan = static_cast<FmPan>(u8(pan_));
    for (size_t i = 0; i < 4; ++i)
    {
        const auto& r = ops_[i];
        FmOperator& o = p.op[i];
        o.tl = u8(r[0]);
        o.ar = u8(r[1]);
        o.d1r = u8(r[2]);
        o.d2r = u8(r[3]);
        o.rr = u8(r[4]);
        o.d1l = u8(r[5]);
        o.mul = u8(r[6]);
        o.dt1 = static_cast<int8_t>(std::lround(r[7]->load()));
        o.dt2 = u8(r[8]);
        o.ks = u8(r[9]);
        o.amEn = r[10]->load() >= 0.5f;
    }
    return p;
}

PadParams ParameterRefs::pad(int index) const
{
    const auto& r = pads_[static_cast<size_t>(index)];
    auto i = [](Value v) { return static_cast<int>(std::lround(v->load())); };
    return { i(r[0]), i(r[1]), i(r[2]), i(r[3]) };
}

void applyPatchToParameters(juce::AudioProcessorValueTreeState& state, const FmPatch& patch)
{
    auto set = [&state](const juce::String& id, int value) {
        if (auto* param = state.getParameter(id))
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(value)));
    };

    set("alg", patch.alg);
    set("fb", patch.fb);
    set("lfo_freq", patch.lfoFreq);
    set("lfo_wave", patch.lfoWave);
    set("pmd", patch.pmd);
    set("amd", patch.amd);
    set("pms", patch.pms);
    set("ams", patch.ams);
    set("pan", static_cast<int>(patch.pan));
    for (int op = 0; op < 4; ++op)
        for (int f = 0; f < kOperatorFieldCount; ++f)
            set(operatorParamId(op, kOperatorFields[f]), fieldValue(patch.op[static_cast<size_t>(op)], f));
}

} // namespace opm86

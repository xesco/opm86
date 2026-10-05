#include "Instruments.h"

namespace opm86::instruments
{

juce::File folder()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Application Support/OPM86 Tracker/Instruments");
}

juce::File fileFor(const juce::String& name)
{
    return folder().getChildFile(juce::File::createLegalFileName(name.trim()) + ".json");
}

juce::var toJson(const FmPatch& patch, const juce::String& name)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("name", name.trim());
    root->setProperty("algorithm", patch.alg);
    root->setProperty("feedback", patch.fb);
    root->setProperty("pmSensitivity", patch.pms);
    root->setProperty("amSensitivity", patch.ams);
    root->setProperty("lfoRate", patch.lfoFreq);
    root->setProperty("lfoWave", patch.lfoWave);
    root->setProperty("pmDepth", patch.pmd);
    root->setProperty("amDepth", patch.amd);
    juce::Array<juce::var> ops;
    for (size_t i = 0; i < 4; ++i)
    {
        const FmOperator& o = patch.op[i];
        auto* op = new juce::DynamicObject();
        op->setProperty("operator", kOperatorNames[i]);
        op->setProperty("on", o.on);
        op->setProperty("level", o.tl);
        op->setProperty("multiple", o.mul);
        op->setProperty("detune", o.dt1);
        op->setProperty("coarse", o.dt2);
        op->setProperty("keyScaling", o.ks);
        op->setProperty("lfoAm", o.amEn);
        op->setProperty("attackRate", o.ar);
        op->setProperty("decay1Rate", o.d1r);
        op->setProperty("sustainLevel", o.d1l);
        op->setProperty("decay2Rate", o.d2r);
        op->setProperty("releaseRate", o.rr);
        ops.add(juce::var(op));
    }
    root->setProperty("operators", ops);
    return juce::var(root);
}

bool save(const FmPatch& patch, const juce::String& name, juce::String& error)
{
    if (name.trim().isEmpty())
    {
        error = "The instrument needs a name";
        return false;
    }
    const juce::File file = fileFor(name);
    if (!folder().createDirectory() || !file.replaceWithText(juce::JSON::toString(toJson(patch, name))))
    {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    return true;
}

std::vector<juce::File> list()
{
    std::vector<juce::File> out;
    for (const auto& f : folder().findChildFiles(juce::File::findFiles, false, "*.json"))
        out.push_back(f);
    std::sort(out.begin(), out.end(), [](const juce::File& a, const juce::File& b) {
        return a.getFileNameWithoutExtension().compareIgnoreCase(b.getFileNameWithoutExtension()) < 0;
    });
    return out;
}

std::optional<FmPatch> load(const juce::File& file, bool* lfo)
{
    return fromJson(juce::JSON::parse(file), lfo);
}

std::optional<FmPatch> fromJson(const juce::var& root, bool* lfo)
{
    const auto* ops = root["operators"].getArray();
    if (!root.isObject() || ops == nullptr || ops->size() != 4)
        return std::nullopt;
    // Each value within the chip's range.
    auto get = [](const juce::var& v, const char* key, int low, int high) {
        return static_cast<uint8_t>(juce::jlimit(low, high, static_cast<int>(v[key])));
    };
    FmPatch p;
    p.name = root["name"].toString().toStdString();
    p.alg = get(root, "algorithm", 0, 7);
    p.fb = get(root, "feedback", 0, 7);
    p.pms = get(root, "pmSensitivity", 0, 7);
    p.ams = get(root, "amSensitivity", 0, 3);
    p.lfoFreq = get(root, "lfoRate", 0, 255);
    p.lfoWave = get(root, "lfoWave", 0, 3);
    p.pmd = get(root, "pmDepth", 0, 127);
    p.amd = get(root, "amDepth", 0, 127);
    if (lfo)
        *lfo = root.hasProperty("lfoRate");
    for (int i = 0; i < 4; ++i)
    {
        const juce::var& v = (*ops)[i];
        FmOperator& o = p.op[static_cast<size_t>(i)];
        o.on = v.hasProperty("on") ? static_cast<bool>(v["on"]) : true;
        o.tl = get(v, "level", 0, 127);
        o.mul = get(v, "multiple", 0, 15);
        o.dt1 = static_cast<int8_t>(juce::jlimit(-3, 3, static_cast<int>(v["detune"])));
        o.dt2 = get(v, "coarse", 0, 3);
        o.ks = get(v, "keyScaling", 0, 3);
        o.amEn = static_cast<bool>(v["lfoAm"]);
        o.ar = get(v, "attackRate", 0, 31);
        o.d1r = get(v, "decay1Rate", 0, 31);
        o.d1l = get(v, "sustainLevel", 0, 15);
        o.d2r = get(v, "decay2Rate", 0, 31);
        o.rr = get(v, "releaseRate", 0, 15);
    }
    return p;
}

} // namespace opm86::instruments

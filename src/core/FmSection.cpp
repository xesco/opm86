#include "FmSection.h"

#include <algorithm>
#include <cmath>

namespace opm86
{

namespace
{

// Register offset of each operator, in signal order M1, C1, M2, C2.
constexpr int kSlotOffset[4] = { 0, 16, 8, 24 };

// The YM2151 numbers notes from C# and skips every fourth code.
constexpr uint8_t kNoteCode[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };

// Tuning is defined for a 3.579545 MHz clock; Out Run's 4 MHz runs sharp,
// so notes are computed against the nominal clock to stay in tune.
constexpr double kNominalClock = 3579545.0;

uint8_t dt1Code(int8_t dt1)
{
    return static_cast<uint8_t>(dt1 >= 0 ? dt1 : 4 - dt1);
}

} // namespace

FmSection::FmSection() : chip_(interface_)
{
    reset();
}

void FmSection::reset()
{
    chip_.reset();
    samples_ = 0;
    shadow_.fill(-1);
    lastPmd_ = lastAmd_ = -1;
    voices_ = {};
    for (Part& p : parts_)
        p.bend = p.modWheel = 0.0f, p.sustain = false, p.bendRange = -1, p.attenuation = 0, p.pan = -1;
    for (int ch = 0; ch < kVoices; ++ch)
        keyOff(ch);
    applyGlobal();
    for (int ch = 0; ch < kVoices; ++ch)
        applyChannel(ch);
}

void FmSection::setPatch(const FmPatch& patch)
{
    for (Part& p : parts_)
        p.patch = patch;
    applyGlobal();
    for (int ch = 0; ch < kVoices; ++ch)
        applyChannel(ch);
}

void FmSection::setPartPatch(int part, const FmPatch& patch)
{
    parts_[part].patch = patch;
    if (part == 0)
        applyGlobal();
    for (int ch = 0; ch < kVoices; ++ch)
        if (voices_[ch].part == part)
            applyChannel(ch);
}

void FmSection::setVelocitySensitivity(float amount)
{
    velocitySensitivity_ = std::clamp(amount, 0.0f, 1.0f);
}

void FmSection::setBendRange(int semitones)
{
    semitones = std::clamp(semitones, 0, 96);
    if (semitones == bendRange_)
        return;
    bendRange_ = semitones;
    for (int ch = 0; ch < kVoices; ++ch)
        applyPitch(ch);
}

void FmSection::setPartAttenuation(int part, int steps)
{
    parts_[static_cast<size_t>(part)].attenuation = std::clamp(steps, 0, 127);
    for (int ch = 0; ch < kVoices; ++ch)
        if (voices_[ch].part == part)
            applyChannel(ch);
}

void FmSection::setPartPan(int part, FmPan pan)
{
    parts_[static_cast<size_t>(part)].pan = static_cast<int>(pan);
    for (int ch = 0; ch < kVoices; ++ch)
        if (voices_[ch].part == part)
            applyChannel(ch);
}

void FmSection::setPartBendRange(int part, int semitones)
{
    parts_[static_cast<size_t>(part)].bendRange = std::clamp(semitones, 0, 96);
    for (int ch = 0; ch < kVoices; ++ch)
        if (voices_[ch].part == part)
            applyPitch(ch);
}

void FmSection::noteOn(int part, int note, int velocity)
{
    const int ch = pickVoice(part, note);
    Voice& v = voices_[ch];

    // The chip restarts a note only on a key on that follows a key off by at
    // least one sample. A voice whose key is still down, or went up in this
    // same sample, is keyed off now and keyed on after the next sample.
    const bool restart = v.keyed || v.offSample == samples_;
    if (restart)
        keyOff(ch);
    v.part = part;
    v.note = note;
    v.velocity = velocity;
    v.gate = true;
    v.sustained = false;
    v.pendingKeyOn = restart;
    v.age = ++clock_;
    applyChannel(ch);
    if (!restart)
        keyOn(ch);
}

void FmSection::noteOff(int part, int note)
{
    for (int ch = 0; ch < kVoices; ++ch)
    {
        Voice& v = voices_[ch];
        if (!v.gate || v.part != part || v.note != note)
            continue;
        if (parts_[part].sustain)
            v.sustained = true;
        else
            release(ch);
    }
}

void FmSection::setSustain(int part, bool down)
{
    parts_[part].sustain = down;
    if (down)
        return;
    for (int ch = 0; ch < kVoices; ++ch)
        if (voices_[ch].gate && voices_[ch].sustained && voices_[ch].part == part)
            release(ch);
}

void FmSection::setPitchBend(int part, float amount)
{
    if (amount == parts_[part].bend)
        return;
    parts_[part].bend = amount;
    for (int ch = 0; ch < kVoices; ++ch)
        if (voices_[ch].part == part)
            applyPitch(ch);
}

void FmSection::setModWheel(int part, float amount)
{
    if (amount == parts_[part].modWheel)
        return;
    parts_[part].modWheel = amount;
    applyGlobal();
}

void FmSection::allNotesOff()
{
    for (int ch = 0; ch < kVoices; ++ch)
    {
        release(ch);
        voices_[ch].sustained = false;
    }
}

void FmSection::generate(int32_t& left, int32_t& right)
{
    ymfm::ym2151::output_data out;
    chip_.generate(&out);
    left = out.data[0];
    right = out.data[1];

    for (int ch = 0; ch < kVoices; ++ch)
    {
        Voice& v = voices_[ch];
        if (v.pendingKeyOn)
        {
            v.pendingKeyOn = false;
            keyOn(ch);
        }
    }
    ++samples_;
}

void FmSection::write(uint8_t reg, uint8_t value)
{
    if (shadow_[reg] == value)
        return;
    writeAlways(reg, value);
}

void FmSection::writeAlways(uint8_t reg, uint8_t value)
{
    shadow_[reg] = value;
    chip_.write(0, reg);
    chip_.write(1, value);
}

void FmSection::applyGlobal()
{
    const FmPatch& p = parts_[0].patch;
    write(0x18, p.lfoFreq);
    write(0x1B, p.lfoWave & 3);

    // The mod wheel of any part deepens the shared vibrato.
    float wheel = 0.0f;
    for (const Part& part : parts_)
        wheel = std::max(wheel, part.modWheel);

    // AMD and PMD share register 0x19, told apart by bit 7.
    const int amd = p.amd & 0x7F;
    const int pmd = std::max<int>(p.pmd & 0x7F, static_cast<int>(std::lround(wheel * 127.0f)));
    if (amd != lastAmd_)
    {
        lastAmd_ = amd;
        writeAlways(0x19, static_cast<uint8_t>(amd));
    }
    if (pmd != lastPmd_)
    {
        lastPmd_ = pmd;
        writeAlways(0x19, static_cast<uint8_t>(0x80 | pmd));
    }
}

void FmSection::applyChannel(int ch)
{
    static constexpr uint8_t kPanBits[3] = { 0xC0, 0x40, 0x80 };
    const auto c = static_cast<uint8_t>(ch);
    const Voice& v = voices_[ch];
    const Part& part = parts_[static_cast<size_t>(v.part)];
    const FmPatch& p = part.patch;
    const int pan = part.pan >= 0 ? part.pan : static_cast<int>(p.pan);

    write(0x20 + c, static_cast<uint8_t>(kPanBits[pan] | (p.fb & 7) << 3 | (p.alg & 7)));
    write(0x38 + c, static_cast<uint8_t>((p.pms & 7) << 4 | (p.ams & 3)));

    // Velocity attenuates the carriers only, so the timbre stays the same.
    const float quiet = 1.0f - static_cast<float>(std::clamp(v.velocity, 0, 127)) / 127.0f;
    const int velocityTl = static_cast<int>(std::lround(quiet * velocitySensitivity_ * 64.0f)) + part.attenuation;

    for (int i = 0; i < 4; ++i)
    {
        const FmOperator& o = p.op[i];
        const auto s = static_cast<uint8_t>(ch + kSlotOffset[i]);
        const bool carrier = (kCarrierMask[p.alg & 7] >> i) & 1;
        const int tl = std::min(127, o.tl + (carrier ? velocityTl : 0));

        write(0x40 + s, static_cast<uint8_t>(dt1Code(o.dt1) << 4 | (o.mul & 15)));
        write(0x60 + s, static_cast<uint8_t>(tl));
        write(0x80 + s, static_cast<uint8_t>((o.ks & 3) << 6 | (o.ar & 31)));
        write(0xA0 + s, static_cast<uint8_t>((o.amEn ? 0x80 : 0) | (o.d1r & 31)));
        write(0xC0 + s, static_cast<uint8_t>((o.dt2 & 3) << 6 | (o.d2r & 31)));
        write(0xE0 + s, static_cast<uint8_t>((o.d1l & 15) << 4 | (o.rr & 15)));
    }

    applyPitch(ch);
}

void FmSection::applyPitch(int ch)
{
    const Voice& v = voices_[ch];
    if (v.note < 0)
        return;

    // Semitones above C#0 at the nominal clock; A4 (440 Hz) is octave 4, note 8.
    const double clockShift = 12.0 * std::log2(kNominalClock / kClock);
    const Part& part = parts_[static_cast<size_t>(v.part)];
    const int range = part.bendRange >= 0 ? part.bendRange : bendRange_;
    const double n = (v.note - 69) + 56.0 + clockShift + part.bend * range;
    // The key fraction has 1/64-semitone steps.
    const int steps = std::clamp(static_cast<int>(std::lround(n * 64.0)), 0, 96 * 64 - 1);
    const int whole = steps / 64;
    const int fraction = steps % 64;
    const auto c = static_cast<uint8_t>(ch);
    write(0x28 + c, static_cast<uint8_t>((whole / 12) << 4 | kNoteCode[whole % 12]));
    write(0x30 + c, static_cast<uint8_t>(fraction << 2));
}

void FmSection::release(int ch)
{
    Voice& v = voices_[ch];
    v.gate = v.sustained = v.pendingKeyOn = false;
    v.age = ++clock_;
    keyOff(ch);
}

void FmSection::keyOff(int ch)
{
    Voice& v = voices_[ch];
    if (v.keyed)
        v.offSample = samples_;
    v.keyed = false;
    writeAlways(0x08, static_cast<uint8_t>(ch));
}

void FmSection::keyOn(int ch)
{
    voices_[ch].keyed = true;
    writeAlways(0x08, static_cast<uint8_t>(0x78 | ch));
}

int FmSection::pickVoice(int part, int note) const
{
    for (int ch = 0; ch < kVoices; ++ch)
        if (voices_[ch].part == part && voices_[ch].note == note)
            return ch;

    // A part goes back to the released voice it played last, as a game's
    // driver keeps a part on one chip channel; then the released voice that
    // has been free longest; then the oldest held note.
    int best = -1;
    for (int ch = 0; ch < kVoices; ++ch)
        if (!voices_[ch].gate && voices_[ch].part == part && voices_[ch].note >= 0
            && (best < 0 || voices_[ch].age > voices_[best].age))
            best = ch;
    if (best >= 0)
        return best;
    for (int ch = 0; ch < kVoices; ++ch)
        if (!voices_[ch].gate && (best < 0 || voices_[ch].age < voices_[best].age))
            best = ch;
    if (best >= 0)
        return best;

    best = 0;
    for (int ch = 1; ch < kVoices; ++ch)
        if (voices_[ch].age < voices_[best].age)
            best = ch;
    return best;
}

} // namespace opm86

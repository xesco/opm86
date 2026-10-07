#include "ChipView.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace opm86
{

namespace
{

constexpr double kGlow = 0.25 * kVgmRate; // a written value glows this long
constexpr double kNever = -1.0e18;

// The operators in register order, and each one's key-on bit in a 0x08 write.
constexpr const char* kOperators[4] = { "M1", "M2", "C1", "C2" };
constexpr int kKeyBit[4] = { 0, 2, 1, 3 };

// A value's cell: its text, with a glow behind it when just written.
void cell(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, float glow, juce::Colour accent, bool dim = false)
{
    if (glow > 0.0f)
    {
        g.setColour(accent.withAlpha(0.55f * glow));
        g.fillRoundedRectangle(r.reduced(1.0f, 1.0f), 3.0f);
    }
    g.setColour(glow > 0.5f ? juce::Colours::white : dim ? theme::faint : theme::text.withAlpha(0.85f));
    g.drawText(text, r, juce::Justification::centred, false);
}

// A section's frame: a dark panel with the chip's name and what it plays.
juce::Rectangle<float> section(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& name, const juce::String& role,
                               juce::Colour accent)
{
    g.setColour(theme::panel);
    g.fillRoundedRectangle(area, 8.0f);
    g.setColour(accent.withAlpha(0.35f));
    g.drawRoundedRectangle(area.reduced(0.5f), 8.0f, 1.0f);
    auto inner = area.reduced(14.0f, 10.0f);
    auto header = inner.removeFromTop(28.0f);
    // A chip: a body with pins along both sides.
    const auto body = header.removeFromLeft(14.0f).withSizeKeepingCentre(10.0f, 16.0f);
    g.setColour(accent);
    for (float pin = body.getY() + 2.5f; pin < body.getBottom() - 1.5f; pin += 3.5f)
        g.fillRect(body.getX() - 2.5f, pin, 2.5f, 1.2f), g.fillRect(body.getRight(), pin, 2.5f, 1.2f);
    g.drawRoundedRectangle(body, 2.0f, 1.2f);
    header.removeFromLeft(10.0f);
    g.setFont(theme::pixel(15.0f));
    g.drawText(name, header, juce::Justification::centredLeft, false);
    g.setFont(theme::mono(15.0f, true));
    g.setColour(theme::dim);
    g.drawText(role, header.withTrimmedLeft(static_cast<float>(juce::GlyphArrangement::getStringWidthInt(theme::pixel(15.0f), name)) + 16.0f),
               juce::Justification::centredLeft, false);
    inner.removeFromTop(8.0f);
    return inner;
}

juce::String hex(int value, int digits) { return juce::String::toHexString(value).toUpperCase().paddedLeft('0', digits); }

// What each label means, shown when the pointer rests on it.
juce::String explain(const juce::String& label)
{
    static const std::map<juce::String, juce::String> kMeaning = {
        { "LFO", "LFO rate: how fast the low-frequency oscillator runs" },
        { "PMD", "Pitch modulation depth: how far the LFO moves pitch" },
        { "AMD", "Amplitude modulation depth: how far the LFO moves volume" },
        { "WAVE", "LFO waveform" },
        { "NOISE", "Noise generator, on channel 8's last operator, and its frequency" },
        { "L", "Output to the left" },
        { "R", "Output to the right" },
        { "FB", "Feedback: how much M1 modulates itself" },
        { "ALG", "Algorithm: how the four operators connect" },
        { "KC", "Key code: octave and note" },
        { "KF", "Key fraction: pitch between notes" },
        { "PMS", "Pitch modulation sensitivity: how much the LFO bends this channel" },
        { "AMS", "Amplitude modulation sensitivity: how much the LFO moves its volume" },
        { "M1", "Operator M1, modulator 1" },
        { "M2", "Operator M2, modulator 2" },
        { "C1", "Operator C1, carrier 1" },
        { "C2", "Operator C2, carrier 2" },
        { "TL", "Total level: the operator's volume, 0 loudest" },
        { "AR", "Attack rate: how fast the sound rises" },
        { "D1R", "First decay rate: how fast it falls after the attack" },
        { "D1L", "First decay level: where the first decay stops" },
        { "D2R", "Second decay rate: how fast it falls while the key is held" },
        { "RR", "Release rate: how fast it fades after the key is let go" },
        { "MUL", "Frequency multiple of the note" },
        { "DT1", "Fine detune" },
        { "DT2", "Coarse detune" },
        { "KS", "Key scaling: faster envelopes on higher notes" },
        { "AM", "Amplitude modulation: the LFO moves this operator's volume" },
        { "START", "Where the sample starts in the ROM" },
        { "LOOP", "Where the sample loops back to" },
        { "END", "The page where the sample ends" },
        { "RATE", "Playback step: 128 plays the sample as recorded" },
        { "LOOPS", "Whether the sample loops at its end" },
        { "VOL L", "Volume on the left" },
        { "VOL R", "Volume on the right" },
    };
    if (label.startsWith("CH"))
        return "Channel " + label.substring(2);
    const auto it = kMeaning.find(label);
    return it != kMeaning.end() ? it->second : juce::String();
}

} // namespace

ChipView::ChipView(AppModel& model) : model_(model)
{
    setOpaque(true);
}

void ChipView::tip(juce::Rectangle<float> area, const juce::String& label)
{
    tips_.emplace_back(area, label);
}

juce::String ChipView::getTooltip()
{
    const auto at = getMouseXYRelative().toFloat();
    for (const auto& [area, label] : tips_)
        if (area.contains(at))
            return explain(label);
    return {};
}

void ChipView::write(bool fm, uint16_t reg, uint8_t value, double time)
{
    const auto r = static_cast<size_t>(reg & 0xFF);
    if (fm)
    {
        if (r == 0x08)
        {
            // Key on and off: the channel, and its operators in bits 3 to 6.
            keyOn_[value & 7] = static_cast<uint8_t>((value >> 3) & 15);
            keyWritten_[value & 7] = time;
            return;
        }
        if (r == 0x19)
            ((value & 0x80) ? pmd_ : amd_) = value & 0x7F;
        ym_[r] = value;
        ymWritten_[r] = time;
    }
    else
    {
        pcm_[r] = value;
        pcmWritten_[r] = time;
    }
}

void ChipView::catchUp()
{
    const VgmSong& song = model_.tune().source();
    const double now = model_.displayPosition();
    // A new song, or the playhead moved back: replayed from the start.
    if (&song != song_ || now < at_)
    {
        song_ = &song;
        ymNext_ = pcmNext_ = 0;
        ym_.fill(0), pcm_.fill(0), keyOn_.fill(0);
        ymWritten_.fill(kNever), pcmWritten_.fill(kNever), keyWritten_.fill(kNever);
        pmd_ = amd_ = 0;
    }
    for (; ymNext_ < song.ym.size() && song.ym[ymNext_].time <= now; ++ymNext_)
        write(true, song.ym[ymNext_].address, song.ym[ymNext_].value, song.ym[ymNext_].time);
    for (; pcmNext_ < song.pcm.size() && song.pcm[pcmNext_].time <= now; ++pcmNext_)
        write(false, song.pcm[pcmNext_].address, song.pcm[pcmNext_].value, song.pcm[pcmNext_].time);
    at_ = now;
}

float ChipView::glow(bool fm, int reg) const
{
    const double written = (fm ? ymWritten_ : pcmWritten_)[static_cast<size_t>(reg & 0xFF)];
    return static_cast<float>(std::clamp(1.0 - (at_ - written) / kGlow, 0.0, 1.0));
}

void ChipView::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    tips_.clear();
    if (!model_.hasTune())
        return;
    catchUp();
    // The FM chip on the left, the PCM chip on the right, apart.
    auto area = getLocalBounds().toFloat().reduced(12.0f);
    const auto pcm = area.removeFromRight(std::max(360.0f, area.getWidth() * 0.34f));
    area.removeFromRight(12.0f);
    paintFm(g, area);
    paintPcm(g, pcm);
}

void ChipView::paintFm(juce::Graphics& g, juce::Rectangle<float> area)
{
    const juce::Colour accent = theme::cyan;
    auto inner = section(g, area, "YM2151", juce::String::fromUTF8("FM  \xc2\xb7  8 CHANNELS  \xc2\xb7  4 OPERATORS EACH"), accent);

    // The chip-wide settings: the LFO and the noise.
    {
        static const char* kWaves[4] = { "SAW", "SQUARE", "TRIANGLE", "NOISE" };
        auto row = inner.removeFromTop(26.0f);
        g.setFont(theme::mono(15.0f, true));
        const auto item = [&](const juce::String& label, const juce::String& value, float glowing, float width) {
            auto r = row.removeFromLeft(width);
            g.setColour(theme::dim);
            const auto at = r.removeFromLeft(static_cast<float>(juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), label)) + 6.0f);
            g.drawText(label, at, juce::Justification::centredLeft, false);
            tip(at, label);
            cell(g, r.removeFromLeft(r.getWidth() - 10.0f), value, glowing, accent);
        };
        item("LFO", juce::String(ym_[0x18]), glow(true, 0x18), 110.0f);
        item("PMD", juce::String(pmd_), glow(true, 0x19), 110.0f);
        item("AMD", juce::String(amd_), glow(true, 0x19), 110.0f);
        item("WAVE", kWaves[ym_[0x1B] & 3], glow(true, 0x1B), 170.0f);
        item("NOISE", (ym_[0x0F] & 0x80) ? "ON " + juce::String(ym_[0x0F] & 31) : juce::String("OFF"), glow(true, 0x0F), 150.0f);
        inner.removeFromTop(8.0f);
    }

    // The channels, four down in each of two columns.
    const float gap = 10.0f;
    const float cardW = (inner.getWidth() - gap) / 2.0f, cardH = (inner.getHeight() - 3.0f * gap) / 4.0f;
    static const char* kColumns[11] = { "TL", "AR", "D1R", "D1L", "D2R", "RR", "MUL", "DT1", "DT2", "KS", "AM" };
    for (int ch = 0; ch < 8; ++ch)
    {
        const auto card = juce::Rectangle<float>(inner.getX() + static_cast<float>(ch / 4) * (cardW + gap),
                                                 inner.getY() + static_cast<float>(ch % 4) * (cardH + gap), cardW, cardH);
        const bool keyed = keyOn_[static_cast<size_t>(ch)] != 0;
        g.setColour(theme::background.withAlpha(0.6f));
        g.fillRoundedRectangle(card, 6.0f);
        g.setColour(keyed ? accent.withAlpha(0.5f) : theme::gridBar.withAlpha(0.6f));
        g.drawRoundedRectangle(card.reduced(0.5f), 6.0f, 1.0f);
        // The channel's header: a band across the top of the card with its
        // number, the note it plays, its sides, feedback and algorithm, tinted
        // while it plays. Fonts follow the room: the values fill their rows,
        // short of the columns' width.
        auto c = card;
        auto band = c.removeFromTop(std::min(32.0f, card.getHeight() / 5.2f));
        {
            juce::Path top;
            top.addRoundedRectangle(band.getX(), band.getY(), band.getWidth(), band.getHeight(), 6.0f, 6.0f, true, true, false, false);
            g.setColour(keyed ? accent.withAlpha(0.14f) : theme::panelLight.withAlpha(0.7f));
            g.fillPath(top);
            g.setColour(keyed ? accent.withAlpha(0.35f) : theme::gridBar.withAlpha(0.6f));
            g.fillRect(band.getX(), band.getBottom() - 1.0f, band.getWidth(), 1.0f);
        }
        c = c.reduced(10.0f, 4.0f);
        auto head = band.reduced(10.0f, 3.0f);
        const float headFont = std::clamp(head.getHeight() * 0.66f, 11.0f, 20.0f);
        g.setFont(theme::mono(headFont, true));
        g.setColour(keyed ? theme::text : theme::dim);
        tip(head.withWidth(headFont * 2.6f), "CH" + juce::String(ch + 1));
        g.drawText("CH" + juce::String(ch + 1), head.removeFromLeft(headFont * 2.6f), juce::Justification::centredLeft, false);
        const int kc = ym_[static_cast<size_t>(0x28 + ch)], code = kc & 15;
        const int midi = 13 + ((kc >> 4) & 7) * 12 + code - code / 4; // the YM2151 counts notes from C#, skipping every fourth code
        const auto pill = head.removeFromLeft(headFont * 3.6f);
        const juce::Colour pitch = theme::pitchColour(midi);
        g.setColour(keyed ? pitch : pitch.withAlpha(0.18f));
        g.fillRoundedRectangle(pill, pill.getHeight() * 0.5f);
        // A new note, keyed on or its pitch written, rings the pill white.
        const float struck = std::max(glow(true, 0x28 + ch), static_cast<float>(std::clamp(1.0 - (at_ - keyWritten_[static_cast<size_t>(ch)]) / kGlow, 0.0, 1.0)));
        if (struck > 0.0f)
        {
            g.setColour(juce::Colours::white.withAlpha(0.9f * struck));
            g.drawRoundedRectangle(pill.reduced(0.75f), pill.getHeight() * 0.5f, 1.5f);
        }
        g.setColour(keyed ? theme::background : theme::faint);
        g.drawText(theme::noteName(midi).removeCharacters("-"), pill, juce::Justification::centred, false);
        const int rl = ym_[static_cast<size_t>(0x20 + ch)];
        head.removeFromLeft(headFont * 0.8f);
        const float unit = headFont * 0.62f; // one character
        for (const auto& [side, bit] : { std::pair { "L", 0x40 }, std::pair { "R", 0x80 } })
        {
            const auto at = head.removeFromLeft(unit * 2.0f);
            cell(g, at, side, glow(true, 0x20 + ch), accent, (rl & bit) == 0);
            tip(at, side);
        }
        head.removeFromLeft(unit);
        g.setColour(theme::dim);
        tip(head.withWidth(unit * 2.5f), "FB");
        g.drawText("FB", head.removeFromLeft(unit * 2.5f), juce::Justification::centredLeft, false);
        cell(g, head.removeFromLeft(unit * 2.0f), juce::String((rl >> 3) & 7), glow(true, 0x20 + ch), accent);
        head.removeFromLeft(unit);
        g.setColour(theme::dim);
        tip(head.withWidth(unit * 3.5f), "ALG");
        g.drawText("ALG", head.removeFromLeft(unit * 3.5f), juce::Justification::centredLeft, false);
        cell(g, head.removeFromLeft(unit * 2.0f), juce::String(rl & 7), glow(true, 0x20 + ch), accent);
        // The pitch as written, key code and key fraction, and the LFO's depths on this channel.
        const int pmsAms = ym_[static_cast<size_t>(0x38 + ch)];
        const auto field = [&](const juce::String& name, const juce::String& value, int reg, float chars) {
            head.removeFromLeft(unit);
            g.setColour(theme::dim);
            tip(head.withWidth(unit * (static_cast<float>(name.length()) + 0.5f)), name);
            g.drawText(name, head.removeFromLeft(unit * (static_cast<float>(name.length()) + 0.5f)), juce::Justification::centredLeft, false);
            cell(g, head.removeFromLeft(unit * chars), value, glow(true, reg), accent);
        };
        field("KC", hex(kc, 2), 0x28 + ch, 3.0f);
        field("KF", juce::String(ym_[static_cast<size_t>(0x30 + ch)] >> 2), 0x30 + ch, 3.0f);
        field("PMS", juce::String((pmsAms >> 4) & 7), 0x38 + ch, 2.0f);
        field("AMS", juce::String(pmsAms & 3), 0x38 + ch, 2.0f);

        // Its operators: a row each, the key-on light and the eleven values.
        const float rowH = c.getHeight() / 4.7f;
        auto columns = c.removeFromTop(rowH * 0.7f);
        const float valueFont = std::clamp(std::min(rowH * 0.78f, (c.getWidth() / 12.5f) * 0.55f), 10.0f, 24.0f);
        const float labelW = valueFont * 2.8f, colW = (c.getWidth() - labelW) / 11.0f;
        columns.removeFromLeft(labelW);
        g.setFont(theme::mono(valueFont * 0.8f));
        g.setColour(theme::dim);
        for (const char* name : kColumns)
        {
            tip(columns.withWidth(colW), name);
            g.drawText(name, columns.removeFromLeft(colW), juce::Justification::centred, false);
        }
        g.setFont(theme::mono(valueFont, true));
        for (int op = 0; op < 4; ++op)
        {
            auto row = c.removeFromTop(rowH);
            const int slot = ch + op * 8;
            const bool on = (keyOn_[static_cast<size_t>(ch)] >> kKeyBit[op]) & 1;
            auto label = row.removeFromLeft(labelW);
            const auto light = label.removeFromLeft(valueFont * 0.9f).withSizeKeepingCentre(valueFont * 0.5f, valueFont * 0.5f);
            if (on)
            {
                g.setColour(accent.withAlpha(0.3f));
                g.fillEllipse(light.expanded(3.0f));
            }
            g.setColour(on ? accent : theme::gridBar);
            g.fillEllipse(light);
            g.setColour(on ? theme::text : theme::dim);
            g.drawText(kOperators[op], label, juce::Justification::centredLeft, false);
            tip(label, kOperators[op]);

            const int dtMul = ym_[static_cast<size_t>(0x40 + slot)], tl = ym_[static_cast<size_t>(0x60 + slot)] & 0x7F;
            const int ksAr = ym_[static_cast<size_t>(0x80 + slot)], amD1r = ym_[static_cast<size_t>(0xA0 + slot)];
            const int dt2D2r = ym_[static_cast<size_t>(0xC0 + slot)], d1lRr = ym_[static_cast<size_t>(0xE0 + slot)];
            // The level, loudest at TL 0, as a bar behind its value.
            const auto tlCell = row.removeFromLeft(colW);
            g.setColour(accent.withAlpha(0.18f));
            g.fillRoundedRectangle(tlCell.reduced(2.0f, 3.0f).withWidth((tlCell.getWidth() - 4.0f) * static_cast<float>(127 - tl) / 127.0f), 2.0f);
            cell(g, tlCell, juce::String(tl), glow(true, 0x60 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(ksAr & 31), glow(true, 0x80 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(amD1r & 31), glow(true, 0xA0 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(d1lRr >> 4), glow(true, 0xE0 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(dt2D2r & 31), glow(true, 0xC0 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(d1lRr & 15), glow(true, 0xE0 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(dtMul & 15), glow(true, 0x40 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String((dtMul >> 4) & 7), glow(true, 0x40 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(dt2D2r >> 6), glow(true, 0xC0 + slot), accent);
            cell(g, row.removeFromLeft(colW), juce::String(ksAr >> 6), glow(true, 0x80 + slot), accent);
            cell(g, row.removeFromLeft(colW), (amD1r & 0x80) ? "ON" : "-", glow(true, 0xA0 + slot), accent, (amD1r & 0x80) == 0);
        }
    }
}

void ChipView::paintPcm(juce::Graphics& g, juce::Rectangle<float> area)
{
    const juce::Colour accent = theme::orange;
    auto inner = section(g, area, "SEGAPCM", juce::String::fromUTF8("SAMPLES  \xc2\xb7  16 CHANNELS"), accent);

    // A row per channel: whether it plays, where its sample starts, loops and
    // ends in the ROM, its rate, and its volume on each side as a bar.
    static const char* kColumns[7] = { "START", "LOOP", "END", "RATE", "LOOPS", "VOL L", "VOL R" };
    const float rowH = inner.getHeight() / 16.7f;
    const float valueFont = std::clamp(std::min(rowH * 0.72f, inner.getWidth() / 8.0f * 0.3f), 10.0f, 22.0f);
    const float labelW = valueFont * 3.6f, colW = (inner.getWidth() - labelW) / 7.0f;
    auto columns = inner.removeFromTop(rowH * 0.7f);
    columns.removeFromLeft(labelW);
    g.setFont(theme::mono(valueFont * 0.8f));
    g.setColour(theme::dim);
    for (const char* name : kColumns)
    {
        tip(columns.withWidth(colW), name);
        g.drawText(name, columns.removeFromLeft(colW), juce::Justification::centred, false);
    }
    g.setFont(theme::mono(valueFont, true));
    for (int ch = 0; ch < 16; ++ch)
    {
        auto row = inner.removeFromTop(rowH);
        const int base = ch * 8;
        const int flags = pcm_[static_cast<size_t>(0x86 + base)];
        const bool on = (flags & 1) == 0 && (pcm_[static_cast<size_t>(0x02 + base)] || pcm_[static_cast<size_t>(0x03 + base)]);
        if (ch % 2 == 1)
        {
            g.setColour(theme::background.withAlpha(0.35f));
            g.fillRoundedRectangle(row, 3.0f);
        }
        auto label = row.removeFromLeft(labelW);
        const auto light = label.removeFromLeft(valueFont * 0.9f).withSizeKeepingCentre(valueFont * 0.5f, valueFont * 0.5f);
        if (on)
        {
            g.setColour(accent.withAlpha(0.3f * (0.4f + 0.6f * glow(false, 0x86 + base))));
            g.fillEllipse(light.expanded(3.0f));
        }
        g.setColour(on ? accent : theme::gridBar);
        g.fillEllipse(light);
        g.setColour(on ? theme::text : theme::dim);
        g.drawText("CH" + juce::String(ch + 1), label, juce::Justification::centredLeft, false);
        tip(label, "CH" + juce::String(ch + 1));

        const int start = pcm_[static_cast<size_t>(0x84 + base)] | pcm_[static_cast<size_t>(0x85 + base)] << 8;
        const int loop = pcm_[static_cast<size_t>(0x04 + base)] | pcm_[static_cast<size_t>(0x05 + base)] << 8;
        cell(g, row.removeFromLeft(colW), hex(start, 4), std::max(glow(false, 0x84 + base), glow(false, 0x85 + base)), accent, !on);
        cell(g, row.removeFromLeft(colW), hex(loop, 4), std::max(glow(false, 0x04 + base), glow(false, 0x05 + base)), accent, !on);
        cell(g, row.removeFromLeft(colW), hex(pcm_[static_cast<size_t>(0x06 + base)], 2) + "00", glow(false, 0x06 + base), accent, !on);
        cell(g, row.removeFromLeft(colW), juce::String(pcm_[static_cast<size_t>(0x07 + base)]), glow(false, 0x07 + base), accent, !on);
        cell(g, row.removeFromLeft(colW), (flags & 2) ? "-" : "YES", glow(false, 0x86 + base), accent, !on);
        for (const int side : { 0x02, 0x03 })
        {
            const auto r = row.removeFromLeft(colW).reduced(6.0f, rowH * 0.3f);
            const int volume = pcm_[static_cast<size_t>(side + base)] & 0x7F;
            g.setColour(theme::gridBar.withAlpha(0.5f));
            g.fillRoundedRectangle(r, 2.0f);
            g.setColour(on ? accent.withAlpha(0.6f + 0.4f * glow(false, side + base)) : theme::faint);
            g.fillRoundedRectangle(r.withWidth(r.getWidth() * static_cast<float>(volume) / 127.0f), 2.0f);
        }
    }
}

} // namespace opm86

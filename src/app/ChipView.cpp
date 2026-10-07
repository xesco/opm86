#include "ChipView.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace opm86
{

namespace
{

constexpr double kGlow = 0.25 * kVgmRate; // a written value glows this long
constexpr float kBankRow = 56.0f;          // the tallest a bank's row in the sample map grows
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

// The width a section's title takes in its header: the chip, its name and what it plays.
float titleWidth(const juce::String& name, const juce::String& role)
{
    return 24.0f + static_cast<float>(juce::GlyphArrangement::getStringWidthInt(theme::pixel(15.0f), name)) + 16.0f
           + static_cast<float>(juce::GlyphArrangement::getStringWidthInt(theme::mono(15.0f, true), role)) + 20.0f;
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
        { "TIMER A", "Timer A: its 10-bit count and how often it fires; a game's sound driver often keeps time with it" },
        { "TIMER B", "Timer B: its 8-bit count and how often it fires" },
        { "LOAD", "Which timers run" },
        { "IRQ", "Which timers interrupt the sound CPU when they fire" },
        { "CSM", "Composite sine mode: timer A keys every channel on, for speech" },
        { "CONTROL", "The timers' control: which run, which interrupt, and composite sine mode" },
        { "PINS", "The two control output pins" },
        { "CT1", "Control output pin 1, which some boards use to switch sound" },
        { "CT2", "Control output pin 2" },
        { "TEST", "The test register; bit 1 holds the LFO at its start" },
        { "L", "Output to the left" },
        { "R", "Output to the right" },
        { "FB", "Feedback: how much M1 modulates itself" },
        { "ALG", "Algorithm: how the four operators connect" },
        { "KC", "Key code: octave and note" },
        { "KF", "Key fraction: pitch between notes" },
        { "PMS", "Pitch modulation sensitivity: how much the LFO bends this channel" },
        { "BANK", "The 64 KB bank of the sample ROM its sample is in" },
        { "AMS", "Amplitude modulation sensitivity: how much the LFO moves its volume" },
        { "M1", "Operator M1, modulator 1: in most algorithms it shapes another operator's sound; it can feed back into itself" },
        { "M2", "Operator M2, modulator 2: in most algorithms it shapes another operator's sound" },
        { "C1", "Operator C1, carrier 1: in many algorithms it is heard, or shapes C2" },
        { "C2", "Operator C2, carrier 2: always heard" },
        { "GRAPH", "Draw the algorithm and the envelopes" },
        { "TABLE", "List the values" },
        { "OUT", "The channel's output: the operators that are heard" },
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
        { "FLAGS", "The channel's flag register: bit 0 stops it, bit 1 stops it at its end instead of looping, bits 4 to 6 its bank" },
        { "BANK ROW", "A 64 KB bank of the sample ROM, and the song's samples in it: a sample lights and fills as a channel plays it, with the channel's number and where it reads" },
        { "OUT", "The channel's output now" },
        { "HZ", "The operator's frequency, about: the channel's note times MUL, raised by DT2; DT1's fine detune is left out" },
        { "WRITES", "How many writes reached the chip in the last second" },
        { "LAST", "The last write: the register, then the value written" },
        { "VOL L", "Volume on the left" },
        { "VOL R", "Volume on the right" },
    };
    if (label.startsWith("CH"))
        return "Channel " + label.substring(2);
    const auto it = kMeaning.find(label);
    return it != kMeaning.end() ? it->second : juce::String();
}

} // namespace

ChipView::ChipView(AppModel& model, const TrackerView& tracks) : model_(model), tracks_(tracks)
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
        ymChanged_[r] = ym_[r] != value;
        ym_[r] = value;
        ymWritten_[r] = time;
    }
    else
    {
        pcmChanged_[r] = pcm_[r] != value;
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
        if (&song != song_)
        {
            // The song's samples: at each trigger, the channel's bank, start and end.
            samples_.clear();
            std::array<uint8_t, 256> regs {};
            for (const VgmWrite& w : song.pcm)
            {
                const auto r = static_cast<size_t>(w.address & 0xFF);
                regs[r] = w.value;
                if ((r & 0x87) == 0x86 && (w.value & 1) == 0)
                {
                    const size_t base = r & 0x78; // the channel's eight registers, as ch x 8
                    samples_.push_back({ static_cast<uint32_t>(w.value & 0x70) << 12,
                                         static_cast<uint32_t>(regs[base | 0x84] | regs[base | 0x85] << 8),
                                         static_cast<uint32_t>((regs[base | 0x06] + 1) << 8) });
                }
            }
            std::sort(samples_.begin(), samples_.end());
            samples_.erase(std::unique(samples_.begin(), samples_.end()), samples_.end());
        }
        song_ = &song;
        ymNext_ = pcmNext_ = 0;
        ym_.fill(0), pcm_.fill(0), keyOn_.fill(0);
        ymWritten_.fill(kNever), pcmWritten_.fill(kNever), keyWritten_.fill(kNever);
        ymChanged_.fill(false), pcmChanged_.fill(false);
        pmd_ = amd_ = 0;
    }
    for (; ymNext_ < song.ym.size() && song.ym[ymNext_].time <= now; ++ymNext_)
        write(true, song.ym[ymNext_].address, song.ym[ymNext_].value, song.ym[ymNext_].time);
    for (; pcmNext_ < song.pcm.size() && song.pcm[pcmNext_].time <= now; ++pcmNext_)
        write(false, song.pcm[pcmNext_].address, song.pcm[pcmNext_].value, song.pcm[pcmNext_].time);
    at_ = now;
}

float ChipView::liveLevel(int slot) const
{
    return live_.valid ? live_.level[static_cast<size_t>(slot)] : 0.0f;
}

float ChipView::glow(bool fm, int reg) const
{
    // A new value glows full; the same value written again, faintly.
    const auto r = static_cast<size_t>(reg & 0xFF);
    const double written = (fm ? ymWritten_ : pcmWritten_)[r];
    const float strength = (fm ? ymChanged_ : pcmChanged_)[r] ? 1.0f : 0.35f;
    return strength * static_cast<float>(std::clamp(1.0 - (at_ - written) / kGlow, 0.0, 1.0));
}

void ChipView::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    tips_.clear();
    if (!model_.hasTune())
        return;
    catchUp();
    live_ = model_.chipState();
    const double started = juce::Time::getMillisecondCounterHiRes();
    // The FM chip on the left, the PCM chip on the right, apart.
    auto area = getLocalBounds().toFloat().reduced(12.0f);
    const auto pcm = area.removeFromRight(std::max(360.0f, area.getWidth() * 0.34f));
    area.removeFromRight(12.0f);
    paintFm(g, area);
    paintPcm(g, pcm);
    lastPaint_ = started;
}

void ChipView::paintFm(juce::Graphics& g, juce::Rectangle<float> area)
{
    const juce::Colour accent = theme::cyan;
    const juce::String role = juce::String::fromUTF8("FM  \xc2\xb7  8 CHANNELS  \xc2\xb7  4 OPERATORS EACH");
    auto inner = section(g, area, "YM2151", role, accent);
    // The bus, in the header after the title.
    paintBus(g, area.reduced(14.0f, 10.0f).removeFromTop(28.0f).withTrimmedLeft(titleWidth("YM2151", role)), true, accent);

    // The chip-wide settings, in a card of their own like a channel's: a
    // header band, then groups side by side, each named over its values
    // and set apart by a line: the LFO, the noise, the two timers (a sound
    // driver often keeps time with them) and their control, the output pins
    // and the test register.
    {
        static const char* kWaves[4] = { "SAW", "SQUARE", "TRIANGLE", "NOISE" };
        const int timerA = ym_[0x10] << 2 | (ym_[0x11] & 3), timerB = ym_[0x12], control = ym_[0x14], pins = ym_[0x1B];
        const double rate = song_ != nullptr ? song_->fmRate : 62500.0; // the chip's samples a second
        auto hz = [](double f) { return juce::String(f, f < 100.0 ? 1 : 0) + "Hz"; };
        auto pair = [](bool a, bool b) { return juce::String(a ? "A" : "-") + " " + (b ? "B" : "-"); };
        struct Item
        {
            juce::String label, tipKey, value;
            float glowing;
        };
        struct Group
        {
            juce::String name;
            std::vector<Item> items;
        };
        const std::vector<Group> groups = {
            { "LFO", { { "RATE", "LFO", juce::String(ym_[0x18]), glow(true, 0x18) }, { "PMD", "PMD", juce::String(pmd_), glow(true, 0x19) },
                       { "AMD", "AMD", juce::String(amd_), glow(true, 0x19) }, { "WAVE", "WAVE", kWaves[ym_[0x1B] & 3], glow(true, 0x1B) } } },
            { "NOISE", { { "FREQ", "NOISE", (ym_[0x0F] & 0x80) ? juce::String(ym_[0x0F] & 31) : juce::String("OFF"), glow(true, 0x0F) } } },
            { "TIMER A", { { "", "TIMER A", hex(timerA, 3), std::max(glow(true, 0x10), glow(true, 0x11)) },
                           { "", "TIMER A", hz(rate / (1024 - timerA)), std::max(glow(true, 0x10), glow(true, 0x11)) } } },
            { "TIMER B", { { "", "TIMER B", hex(timerB, 2), glow(true, 0x12) }, { "", "TIMER B", hz(rate / (16.0 * (256 - timerB))), glow(true, 0x12) } } },
            { "CONTROL", { { "LOAD", "LOAD", pair(control & 1, control & 2), glow(true, 0x14) }, { "IRQ", "IRQ", pair(control & 4, control & 8), glow(true, 0x14) },
                           { "CSM", "CSM", (control & 0x80) ? "ON" : "OFF", glow(true, 0x14) } } },
            { "PINS", { { "CT1", "CT1", juce::String((pins >> 6) & 1), glow(true, 0x1B) }, { "CT2", "CT2", juce::String((pins >> 7) & 1), glow(true, 0x1B) } } },
            { "TEST", { { "", "TEST", hex(ym_[0x01], 2), glow(true, 0x01) } } },
        };

        const auto card = inner.removeFromTop(78.0f);
        inner.removeFromTop(10.0f);
        g.setColour(theme::background.withAlpha(0.6f));
        g.fillRoundedRectangle(card, 6.0f);
        g.setColour(theme::gridBar.withAlpha(0.6f));
        g.drawRoundedRectangle(card.reduced(0.5f), 6.0f, 1.0f);
        auto c = card;
        auto band = c.removeFromTop(28.0f);
        {
            juce::Path top;
            top.addRoundedRectangle(band.getX(), band.getY(), band.getWidth(), band.getHeight(), 6.0f, 6.0f, true, true, false, false);
            g.setColour(theme::panelLight.withAlpha(0.7f));
            g.fillPath(top);
            g.setColour(theme::gridBar.withAlpha(0.6f));
            g.fillRect(band.getX(), band.getBottom() - 1.0f, band.getWidth(), 1.0f);
            auto head = band.reduced(10.0f, 0.0f);
            g.setFont(theme::mono(15.0f, true));
            g.setColour(theme::text);
            const float nameW = static_cast<float>(juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), "CHIP")) + 12.0f;
            g.drawText("CHIP", head.removeFromLeft(nameW), juce::Justification::centredLeft, false);
            g.setFont(theme::mono(13.0f));
            g.setColour(theme::dim);
            g.drawText("SHARED BY ALL CHANNELS", head, juce::Justification::centredLeft, false);
        }

        // The groups, as wide as their values need, spread over the card.
        const auto labelFont = theme::mono(12.0f), valueFont = theme::mono(14.0f, true), captionFont = theme::mono(11.0f);
        auto widthOf = [](const juce::Font& f, const juce::String& t) { return static_cast<float>(juce::GlyphArrangement::getStringWidthInt(f, t)); };
        auto itemW = [&](const Item& it) { return (it.label.isEmpty() ? 0.0f : widthOf(labelFont, it.label) + 5.0f) + widthOf(valueFont, it.value) + 14.0f; };
        std::vector<float> widths;
        float total = 0.0f;
        for (const Group& grp : groups)
        {
            float w = 0.0f;
            for (const Item& it : grp.items)
                w += itemW(it);
            widths.push_back(std::max(w, widthOf(captionFont, grp.name) + 8.0f));
            total += widths.back();
        }
        auto body = c.reduced(12.0f, 8.0f);
        const float spare = std::max(0.0f, (body.getWidth() - total) / static_cast<float>(groups.size() - 1));
        for (size_t k = 0; k < groups.size(); ++k)
        {
            auto slot = body.removeFromLeft(widths[k]);
            if (k + 1 < groups.size())
            {
                // The line between this group and the next, in the middle of the space between them.
                const auto between = body.removeFromLeft(spare);
                g.setColour(theme::gridBar.withAlpha(0.6f));
                g.fillRect(between.getCentreX(), body.getY() + 2.0f, 1.0f, body.getHeight() - 4.0f);
            }
            g.setFont(captionFont);
            g.setColour(theme::dim);
            const auto caption = slot.removeFromTop(14.0f);
            g.drawText(groups[k].name, caption, juce::Justification::centredLeft, false);
            tip(caption.withWidth(widthOf(captionFont, groups[k].name) + 4.0f), groups[k].name);
            for (const Item& it : groups[k].items)
            {
                auto r = slot.removeFromLeft(itemW(it)).withTrimmedTop(2.0f);
                if (it.label.isNotEmpty())
                {
                    g.setFont(labelFont);
                    g.setColour(theme::dim);
                    const auto at = r.removeFromLeft(widthOf(labelFont, it.label) + 5.0f);
                    g.drawText(it.label, at, juce::Justification::centredLeft, false);
                    tip(at, it.tipKey);
                }
                g.setFont(valueFont);
                cell(g, r.withTrimmedRight(8.0f), it.value, it.glowing, accent);
            }
        }
    }

    // The channels, four down in each of two columns.
    const float gap = 10.0f;
    const float cardW = (inner.getWidth() - gap) / 2.0f, cardH = (inner.getHeight() - 3.0f * gap) / 4.0f;
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
            // The channel's output now, a meter right under the band.
            paintMeter(g, juce::Rectangle<float>(band.getX() + 10.0f, band.getBottom() + 4.0f, band.getWidth() - 20.0f, 5.0f), ch);
        }
        c = c.reduced(10.0f, 4.0f);
        c.removeFromTop(9.0f); // the meter's row
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

        // A switch at the header's end: the operators listed or drawn.
        drawToggle_[static_cast<size_t>(ch)] = head.removeFromRight(head.getHeight() * 1.5f).reduced(0.0f, 1.0f);
        {
            const auto t = drawToggle_[static_cast<size_t>(ch)];
            const bool over = isMouseOver() && t.contains(getMouseXYRelative().toFloat());
            g.setColour(drawn_[static_cast<size_t>(ch)] ? accent.withAlpha(0.25f) : over ? theme::panelLight : theme::background.withAlpha(0.6f));
            g.fillRoundedRectangle(t, 4.0f);
            g.setColour(drawn_[static_cast<size_t>(ch)] ? accent : theme::dim);
            g.drawRoundedRectangle(t.reduced(0.5f), 4.0f, 1.0f);
            // An envelope's shape: rise, fall, hold and fade.
            const auto i2 = t.reduced(t.getWidth() * 0.22f, t.getHeight() * 0.28f);
            juce::Path shape;
            shape.startNewSubPath(i2.getX(), i2.getBottom());
            shape.lineTo(i2.getX() + i2.getWidth() * 0.2f, i2.getY());
            shape.lineTo(i2.getX() + i2.getWidth() * 0.4f, i2.getY() + i2.getHeight() * 0.45f);
            shape.lineTo(i2.getX() + i2.getWidth() * 0.75f, i2.getY() + i2.getHeight() * 0.55f);
            shape.lineTo(i2.getRight(), i2.getBottom());
            g.strokePath(shape, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            tip(t, drawn_[static_cast<size_t>(ch)] ? "TABLE" : "GRAPH");
        }

        if (drawn_[static_cast<size_t>(ch)])
            paintOperatorGraph(g, c, ch, accent);
        else
            paintOperatorTable(g, c, ch, accent);
    }
}

void ChipView::paintOperatorTable(juce::Graphics& g, juce::Rectangle<float> c, int ch, juce::Colour accent)
{
    static const char* kColumns[11] = { "TL", "AR", "D1R", "D1L", "D2R", "RR", "MUL", "DT1", "DT2", "KS", "AM" };
    // Its operators: a row each, the key-on light and the eleven values.
    const float rowH = c.getHeight() / 4.7f;
    auto columns = c.removeFromTop(rowH * 0.7f);
    const float valueFont = std::clamp(std::min(rowH * 0.78f, (c.getWidth() / 12.5f) * 0.55f), 10.0f, 24.0f);
    const float labelW = valueFont * 2.8f, colW = (c.getWidth() - labelW) / 11.0f;
    fmFont_ = valueFont;
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
        tip(label, kOperators[op]); // the light and the name
        // The light: as bright as the operator's envelope is loud now.
        const float live = liveLevel(slot);
        const auto light = label.removeFromLeft(valueFont * 0.9f).withSizeKeepingCentre(valueFont * 0.5f, valueFont * 0.5f);
        if (live > 0.0f)
        {
            g.setColour(accent.withAlpha(0.35f * live));
            g.fillEllipse(light.expanded(3.0f));
        }
        g.setColour(live > 0.0f ? theme::gridBar.interpolatedWith(accent, 0.25f + 0.75f * live) : theme::gridBar);
        g.fillEllipse(light);
        g.setColour(on ? theme::text : theme::dim);
        g.drawText(kOperators[op], label, juce::Justification::centredLeft, false);

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

namespace
{
// The eight algorithms: where each operator sits, in columns and rows, and
// what it feeds. Operators in the chip's order, M1, C1, M2, C2; a target of
// 4 is the output.
struct Algorithm
{
    float column[4], row[4];
    std::vector<std::pair<int, int>> wires;
};
const Algorithm& algorithm(int alg)
{
    static const Algorithm kAlgorithms[8] = {
        { { 0, 1, 2, 3 }, { 1, 1, 1, 1 }, { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 } } },
        { { 0, 0, 1, 2 }, { 0, 2, 1, 1 }, { { 0, 2 }, { 1, 2 }, { 2, 3 }, { 3, 4 } } },
        { { 1, 0, 1, 2 }, { 0, 2, 2, 1 }, { { 1, 2 }, { 0, 3 }, { 2, 3 }, { 3, 4 } } },
        { { 0, 1, 1, 2 }, { 0, 0, 2, 1 }, { { 0, 1 }, { 1, 3 }, { 2, 3 }, { 3, 4 } } },
        { { 0, 1, 0, 1 }, { 0, 0, 2, 2 }, { { 0, 1 }, { 2, 3 }, { 1, 4 }, { 3, 4 } } },
        { { 0, 1, 1, 1 }, { 1, 0, 1, 2 }, { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 4 }, { 2, 4 }, { 3, 4 } } },
        { { 0, 1, 1, 1 }, { 0, 0, 1, 2 }, { { 0, 1 }, { 1, 4 }, { 2, 4 }, { 3, 4 } } },
        { { 0, 0, 0, 0 }, { 0, 0.67f, 1.33f, 2 }, { { 0, 4 }, { 1, 4 }, { 2, 4 }, { 3, 4 } } },
    };
    return kAlgorithms[alg & 7];
}
constexpr const char* kChipOrder[4] = { "M1", "C1", "M2", "C2" };
constexpr int kRegisterOrder[4] = { 0, 2, 1, 3 }; // each one's place in the register order M1, M2, C1, C2
} // namespace

void ChipView::paintOperatorGraph(juce::Graphics& g, juce::Rectangle<float> area, int ch, juce::Colour accent)
{
    const int rlFbAlg = ym_[static_cast<size_t>(0x20 + ch)];
    const Algorithm& alg = algorithm(rlFbAlg & 7);
    area.removeFromTop(8.0f); // room under the header, for M1's feedback loop
    auto diagram = area.removeFromLeft(area.getWidth() * 0.5f).reduced(4.0f, 4.0f);
    area.removeFromLeft(10.0f);

    // The algorithm: a box per operator, lit while keyed and as bright as
    // its level, wired to what it feeds, and the output at the right.
    float columns = 0;
    for (const float c : alg.column)
        columns = std::max(columns, c);
    const float cellW = diagram.getWidth() / (columns + 2.0f), cellH = diagram.getHeight() / 3.0f;
    // The boxes leave room under them for each operator's frequency.
    // Never thinner than its name needs, as the cell allows.
    const float boxW = cellW * 0.62f, boxH = std::max(std::min(cellH * 0.55f, boxW * 0.6f), std::min(cellH * 0.9f, 16.0f));
    auto box = [&](int d) {
        return juce::Rectangle<float>(boxW, boxH).withCentre({ diagram.getX() + (alg.column[d] + 0.5f) * cellW, diagram.getY() + (alg.row[d] + 0.5f) * cellH });
    };
    const auto out = juce::Rectangle<float>(boxW, boxH).withCentre({ diagram.getRight() - cellW * 0.5f, diagram.getCentreY() });
    // An operator's light: its envelope now, as loud as its level allows.
    auto level = [&](int d) {
        const int slot = ch + kRegisterOrder[d] * 8;
        const int tl = ym_[static_cast<size_t>(0x60 + slot)] & 0x7F;
        return liveLevel(slot) * (0.25f + 0.75f * static_cast<float>(127 - tl) / 127.0f);
    };
    for (const auto& [from, to] : alg.wires)
    {
        const auto a = box(from).getCentre().withX(box(from).getRight());
        const auto end = to == 4 ? out : box(to);
        const auto b = end.getCentre().withX(end.getX());
        juce::Path wire;
        wire.startNewSubPath(a);
        wire.cubicTo(a.x + (b.x - a.x) * 0.5f, a.y, a.x + (b.x - a.x) * 0.5f, b.y, b.x, b.y);
        const float lit = level(from);
        g.setColour(lit > 0.0f ? accent.withAlpha(0.35f + 0.65f * lit) : theme::gridBar);
        g.strokePath(wire, juce::PathStrokeType(lit > 0.0f ? 2.0f : 1.2f));
    }
    // M1's feedback: a loop over its box, as strong as FB.
    if (const int fb = (rlFbAlg >> 3) & 7; fb > 0)
    {
        const auto m1 = box(0);
        juce::Path loop;
        loop.startNewSubPath(m1.getRight() - boxW * 0.2f, m1.getY());
        loop.cubicTo(m1.getRight() - boxW * 0.2f, m1.getY() - boxH * 0.7f, m1.getX() + boxW * 0.2f, m1.getY() - boxH * 0.7f, m1.getX() + boxW * 0.2f, m1.getY());
        g.setColour(level(0) > 0.0f ? accent.withAlpha(0.3f + 0.1f * static_cast<float>(fb)) : theme::gridBar);
        g.strokePath(loop, juce::PathStrokeType(1.0f + 0.25f * static_cast<float>(fb)));
    }
    const float font = std::clamp(boxH * 0.55f, 9.0f, 16.0f);
    g.setFont(theme::mono(font, true));
    for (int d = 0; d < 4; ++d)
    {
        const auto r = box(d);
        const float lit = level(d);
        if (lit > 0.0f)
        {
            g.setColour(accent.withAlpha(0.25f * lit));
            g.fillRoundedRectangle(r.expanded(3.0f), 6.0f);
        }
        g.setColour(lit > 0.0f ? accent.withAlpha(0.2f + 0.6f * lit) : theme::panelLight);
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(lit > 0.0f ? accent : theme::gridBar);
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(lit > 0.5f ? theme::background : theme::text);
        g.drawText(kChipOrder[d], r, juce::Justification::centred, false);
        tip(r, kChipOrder[d]);
    }

    // Each operator's frequency, under its box: the channel's note from its
    // key code and fraction, at the chip's clock, times MUL (0 is a half),
    // raised by DT2's coarse detune. DT1's few cents are left out.
    {
        const int kc = ym_[static_cast<size_t>(0x28 + ch)], code = kc & 15;
        const double clockShift = 12.0 * std::log2((song_ != nullptr ? song_->fmRate : 62500.0) * 64.0 / 3579545.0);
        const double note = 13.0 + ((kc >> 4) & 7) * 12 + code - code / 4 + (ym_[static_cast<size_t>(0x30 + ch)] >> 2) / 64.0 + clockShift;
        const double base = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
        static constexpr double kCoarse[4] = { 0.0, 600.0, 781.0, 950.0 }; // DT2, in cents
        const float room = (cellH - boxH) * 0.5f, under = std::min(room * 0.8f, 12.0f);
        g.setFont(theme::mono(under));
        for (int d = 0; room >= 9.0f && d < 4; ++d) // short of room, left out
        {
            const int slot = ch + kRegisterOrder[d] * 8;
            const int mul = ym_[static_cast<size_t>(0x40 + slot)] & 15, dt2 = ym_[static_cast<size_t>(0xC0 + slot)] >> 6;
            const double hz = base * (mul == 0 ? 0.5 : mul) * std::pow(2.0, kCoarse[dt2] / 1200.0);
            const auto r = box(d);
            const auto label = juce::Rectangle<float>(r.getX() - 12.0f, r.getBottom() + 1.0f, r.getWidth() + 24.0f, under + 2.0f);
            g.setColour(level(d) > 0.0f ? theme::text : theme::dim);
            g.drawText(juce::String(juce::roundToInt(hz)) + "Hz", label, juce::Justification::centred, false);
            tip(label, "HZ");
        }
    }
    g.setColour(theme::background);
    g.fillRoundedRectangle(out, 4.0f);
    g.setColour(keyOn_[static_cast<size_t>(ch)] ? accent : theme::gridBar);
    g.drawRoundedRectangle(out.reduced(0.5f), 4.0f, 1.0f);
    g.setFont(theme::mono(font * 0.85f, true));
    g.drawText("OUT", out, juce::Justification::centred, false);
    tip(out, "OUT");

    // The envelopes, one per operator: the rise at AR, the fall at D1R down
    // to D1L, the slope at D2R while held, and the fade at RR; as tall as
    // the operator's level, and bright while it is keyed.
    const float gap = 6.0f;
    const float graphW = (area.getWidth() - gap) / 2.0f, graphH = (area.getHeight() - gap) / 2.0f;
    for (int d = 0; d < 4; ++d)
    {
        const int op = kRegisterOrder[d], slot = ch + op * 8;
        const auto r = juce::Rectangle<float>(area.getX() + static_cast<float>(d % 2) * (graphW + gap),
                                              area.getY() + static_cast<float>(d / 2) * (graphH + gap), graphW, graphH);
        const bool on = (keyOn_[static_cast<size_t>(ch)] >> kKeyBit[op]) & 1;
        g.setColour(theme::background.withAlpha(0.5f));
        g.fillRoundedRectangle(r, 4.0f);
        const int ar = ym_[static_cast<size_t>(0x80 + slot)] & 31, d1r = ym_[static_cast<size_t>(0xA0 + slot)] & 31;
        const int d2r = ym_[static_cast<size_t>(0xC0 + slot)] & 31, d1l = ym_[static_cast<size_t>(0xE0 + slot)] >> 4;
        const int rr = ym_[static_cast<size_t>(0xE0 + slot)] & 15, tl = ym_[static_cast<size_t>(0x60 + slot)] & 0x7F;
        // Faster rates take less width; a rate of 0 holds.
        const float attack = ar == 0 ? 1.0f : static_cast<float>(32 - ar) / 32.0f;
        const float decay = d1r == 0 ? 1.0f : static_cast<float>(32 - d1r) / 32.0f;
        const float release = static_cast<float>(16 - rr) / 16.0f;
        const float sustain = 1.0f - static_cast<float>(d1l) / 15.0f;
        const float held = sustain * (1.0f - std::min(1.0f, static_cast<float>(d2r) / 31.0f * 0.9f));
        const float total = attack * 0.25f + decay * 0.25f + 0.3f + release * 0.25f;
        // The name in a column of its own at the left, the shape beside it.
        const float nameFont = std::clamp(graphH * 0.3f, 9.0f, 13.0f);
        auto plot = r.reduced(6.0f, 6.0f);
        const auto nameStrip = plot.removeFromLeft(nameFont * 1.6f);
        plot.removeFromLeft(4.0f);
        const float height = plot.getHeight() * (0.15f + 0.85f * static_cast<float>(127 - tl) / 127.0f);
        auto x = [&](float t) { return plot.getX() + plot.getWidth() * t / total; };
        auto y = [&](float lv) { return plot.getBottom() - height * lv; };
        juce::Path shape;
        float t = 0.0f;
        shape.startNewSubPath(x(t), y(0.0f));
        shape.lineTo(x(t += attack * 0.25f), y(1.0f));
        shape.lineTo(x(t += decay * 0.25f), y(sustain));
        shape.lineTo(x(t += 0.3f), y(held));
        shape.lineTo(x(t += release * 0.25f), y(0.0f));

        // Where the envelope is now: on its stage's stretch, at its level.
        const float times[5] = { 0.0f, attack * 0.25f, attack * 0.25f + decay * 0.25f, attack * 0.25f + decay * 0.25f + 0.3f, t };
        const auto stage = live_.valid ? live_.stage[static_cast<size_t>(slot)] : ChipState::Stage::Off;
        const float now = liveLevel(slot);
        auto along = [](float value, float from, float to) { return std::abs(to - from) < 1e-4f ? 0.0f : std::clamp((value - from) / (to - from), 0.0f, 1.0f); };
        int segment = -1;
        float fraction = 0.0f;
        switch (stage)
        {
            case ChipState::Stage::Attack: segment = 0, fraction = along(now, 0.0f, 1.0f); break;
            case ChipState::Stage::Decay: segment = 1, fraction = along(now, 1.0f, sustain); break;
            case ChipState::Stage::Sustain: segment = 2, fraction = along(now, sustain, held); break;
            case ChipState::Stage::Release: segment = 3, fraction = along(now, held, 0.0f); break;
            case ChipState::Stage::Off: break;
        }
        // On the drawn line: across its stretch by how far the stage has
        // gone, at the line's height there.
        const float levels[5] = { 0.0f, 1.0f, sustain, held, 0.0f };
        juce::Point<float> dot;
        if (segment >= 0)
            dot = { x(times[segment] + (times[segment + 1] - times[segment]) * fraction),
                    y(levels[segment] + (levels[segment + 1] - levels[segment]) * fraction) };
        juce::Path fill(shape);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(on ? 0.3f : 0.08f));
        g.fillPath(fill);
        g.setColour(on ? accent : accent.withAlpha(0.35f));
        g.strokePath(shape, juce::PathStrokeType(on ? 1.8f : 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (segment >= 0)
        {
            g.setColour(juce::Colours::white.withAlpha(0.25f));
            g.fillEllipse(juce::Rectangle<float>(10.0f, 10.0f).withCentre(dot));
            g.setColour(juce::Colours::white);
            g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(dot));
        }
        g.setFont(theme::mono(nameFont, true));
        g.setColour(on ? theme::text : theme::dim);
        g.drawText(kChipOrder[d], nameStrip, juce::Justification::centredLeft, false);
        tip(nameStrip, kChipOrder[d]);
    }
}

void ChipView::mouseDown(const juce::MouseEvent& e)
{
    for (size_t ch = 0; ch < drawToggle_.size(); ++ch)
        if (drawToggle_[ch].contains(e.position))
        {
            drawn_[ch] = !drawn_[ch];
            repaint();
            return;
        }
}

void ChipView::mouseMove(const juce::MouseEvent& e)
{
    const bool over = std::any_of(drawToggle_.begin(), drawToggle_.end(), [&](const auto& r) { return r.contains(e.position); });
    setMouseCursor(over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void ChipView::paintBus(juce::Graphics& g, juce::Rectangle<float> area, bool fm, juce::Colour accent)
{
    // A box at the header's right end: how many writes reached the chip in
    // the last second, and the last one, register and value.
    if (song_ == nullptr)
        return;
    const auto& writes = fm ? song_->ym : song_->pcm;
    const size_t end = fm ? ymNext_ : pcmNext_;
    int count = 0;
    for (size_t i = end; i-- > 0 && static_cast<double>(writes[i].time) >= at_ - kVgmRate;)
        ++count;
    const juce::String last = end > 0 ? hex(writes[end - 1].address & 0xFF, 2) + "=" + hex(writes[end - 1].value, 2) : juce::String("-");

    // A frame at the header's right end; inside, each readout a label and
    // then its value in a box as wide as its widest value, so nothing moves
    // as the numbers change. Short of room, the count alone.
    const auto labelFont = theme::mono(11.0f), valueFont = theme::mono(14.0f, true);
    auto widthOf = [](const juce::Font& f, const juce::String& t) { return static_cast<float>(juce::GlyphArrangement::getStringWidthInt(f, t)); };
    const float gap = 12.0f, pad = 8.0f, framePad = 8.0f, labelGap = 6.0f;
    auto fieldW = [&](const juce::String& label, const juce::String& widest) { return widthOf(labelFont, label) + labelGap + widthOf(valueFont, widest) + 2.0f * pad; };
    const float writesW = fieldW("WRITES", "9999/s"), lastW = fieldW("LAST", "FF=FF");
    const bool both = area.getWidth() >= writesW + gap + lastW + 2.0f * framePad;
    const float frameW = (both ? writesW + gap + lastW : writesW) + 2.0f * framePad;
    if (area.getWidth() < frameW)
        return;
    const auto frame = area.removeFromRight(frameW).expanded(0.0f, 3.0f); // taller than the header line, for room above and below
    g.setColour(theme::background.withAlpha(0.4f));
    g.fillRoundedRectangle(frame, 6.0f);
    g.setColour(accent.withAlpha(0.3f));
    g.drawRoundedRectangle(frame.reduced(0.5f), 6.0f, 1.0f);
    auto row = frame.reduced(framePad, 0.0f);
    const auto field = [&](float width, const juce::String& label, const juce::String& value) {
        auto r = row.removeFromLeft(width);
        g.setFont(labelFont);
        g.setColour(theme::dim);
        const auto at = r.removeFromLeft(widthOf(labelFont, label) + labelGap);
        g.drawText(label, at, juce::Justification::centredLeft, false);
        tip(at, label);
        const auto box = r.reduced(0.0f, 8.0f);
        g.setColour(theme::background.withAlpha(0.7f));
        g.fillRoundedRectangle(box, 4.0f);
        g.setColour(accent.withAlpha(0.45f));
        g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
        g.setFont(valueFont);
        g.setColour(theme::text);
        g.drawText(value, box, juce::Justification::centred, false);
    };
    field(writesW, "WRITES", juce::String(count) + "/s");
    if (both)
    {
        row.removeFromLeft(gap);
        field(lastW, "LAST", last);
    }
}

void ChipView::paintMeter(juce::Graphics& g, juce::Rectangle<float> area, int channel)
{
    // As on a track: cells white up to -12 dB, yellow to -6, orange to -3
    // and red above, dim where not lit, and a hold at the peak that waits a
    // second, then falls.
    const auto c = static_cast<size_t>(channel);
    const float level = tracks_.channelMeter(channel);
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (level >= hold_[c])
        hold_[c] = level, holdSince_[c] = now;
    else if (now - holdSince_[c] > 1000.0)
        hold_[c] = std::max(level, hold_[c] - static_cast<float>((now - lastPaint_) / 1000.0) * 0.6f);
    g.setColour(juce::Colour(0xff1a1c24));
    g.fillRoundedRectangle(area, 2.0f);
    const auto cells = area.reduced(1.0f);
    const int count = std::max(1, static_cast<int>(cells.getWidth() / 3.0f));
    const float width = cells.getWidth() / static_cast<float>(count);
    const int held = static_cast<int>(hold_[c] * static_cast<float>(count));
    auto zone = [](float db) { return (db + 36.0f) / 36.0f; }; // where a level in dB sits along the meter, from -36 dB
    for (int k = 0; k < count; ++k)
    {
        const float at = (static_cast<float>(k) + 0.5f) / static_cast<float>(count);
        const juce::Colour colour = at > zone(-3.0f) ? theme::danger : at > zone(-6.0f) ? theme::orange : at > zone(-12.0f) ? theme::yellow : juce::Colour(0xffeceaf4);
        const bool lit = at <= level || (k == held && hold_[c] > 0.02f);
        g.setColour(lit ? colour : colour.withMultipliedSaturation(0.8f).withMultipliedBrightness(0.3f));
        g.fillRect(cells.getX() + static_cast<float>(k) * width, cells.getY(), width - 1.0f, cells.getHeight());
    }
}

void ChipView::paintPcm(juce::Graphics& g, juce::Rectangle<float> area)
{
    const juce::Colour accent = theme::orange;
    const juce::String role = juce::String::fromUTF8("SAMPLES  \xc2\xb7  16 CHANNELS");
    auto inner = section(g, area, "SEGAPCM", role, accent);
    // The bus, in the header after the title.
    paintBus(g, area.reduced(14.0f, 10.0f).removeFromTop(28.0f).withTrimmedLeft(titleWidth("SEGAPCM", role)), false, accent);
    // The channels at the YM2151's size at the bottom, the samples above in the rest.
    // The samples as tall as their banks need, out of the room the channels
    // leave at their least; then a line, and the channels in the rest.
    const float valueFont = fmFont_;
    size_t banks = 0;
    for (size_t k = 0; k < samples_.size(); ++k)
        if (k == 0 || samples_[k].bank != samples_[k - 1].bank)
            ++banks;
    const float roomForMap = std::max(0.0f, inner.getHeight() - valueFont * 1.6f * 16.7f - 20.0f);
    const float mapH = banks == 0 ? std::min(roomForMap, 40.0f) : std::min(roomForMap, kBankRow * static_cast<float>(banks));
    paintSampleMap(g, inner.removeFromTop(mapH), accent);
    const auto between = inner.removeFromTop(20.0f);
    g.setColour(theme::gridBar.withAlpha(0.6f));
    g.fillRect(between.getX(), between.getCentreY(), between.getWidth(), 1.0f);
    paintPcmTable(g, inner, accent, valueFont);
}

void ChipView::paintSampleMap(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour accent)
{
    // A row per bank the song uses, a tile per sample in it, as wide as the
    // sample is long, within limits. A tile lights while a channel plays it,
    // fills as the channel goes through it, and shows the channel's number.
    std::vector<uint32_t> banks;
    for (const Sample& s : samples_)
        if (banks.empty() || banks.back() != s.bank)
            banks.push_back(s.bank);
    if (banks.empty())
    {
        g.setFont(theme::mono(fmFont_));
        g.setColour(theme::faint);
        g.drawText("No samples in this song", area, juce::Justification::centred, false);
        return;
    }
    // The rows share the room, short of growing past kBankRow each.
    const float rowH = std::min(area.getHeight() / static_cast<float>(banks.size()), kBankRow);
    const float font = std::clamp(rowH * 0.5f, 10.0f, fmFont_); // as big as the YM2151 values, short of half a row
    const float labelW = font * 4.2f, gap = 4.0f;
    for (size_t b = 0; b < banks.size(); ++b)
    {
        auto row = area.removeFromTop(rowH).reduced(0.0f, 3.0f);
        g.setFont(theme::mono(font));
        g.setColour(theme::dim);
        const auto bankLabel = row.removeFromLeft(labelW);
        g.drawText("BANK " + juce::String(banks[b] >> 16), bankLabel, juce::Justification::centredLeft, false);
        tip(bankLabel, "BANK ROW");
        std::vector<const Sample*> tiles;
        double total = 0;
        for (const Sample& s : samples_)
            if (s.bank == banks[b])
                tiles.push_back(&s), total += std::max<double>(1, s.end > s.start ? s.end - s.start : 1);
        const float n = static_cast<float>(tiles.size());
        const float room = row.getWidth() - gap * (n - 1.0f);
        const float least = std::min(font * 3.2f, room / n * 0.6f);
        float x = row.getX();
        for (const Sample* s : tiles)
        {
            const double length = std::max<double>(1, s->end > s->start ? s->end - s->start : 1);
            const float w = least + (room - least * n) * static_cast<float>(length / total);
            const auto tile = juce::Rectangle<float>(x, row.getY(), w, row.getHeight());
            x += w + gap;

            // The channels playing it, and how far the first one is.
            juce::String players;
            float fraction = -1.0f;
            for (int ch = 0; live_.valid && ch < 16; ++ch)
            {
                const int base = ch * 8;
                if (((live_.pcmPlaying >> ch) & 1) == 0 || static_cast<uint32_t>(pcm_[static_cast<size_t>(0x86 + base)] & 0x70) << 12 != s->bank)
                    continue;
                const uint32_t start = static_cast<uint32_t>(pcm_[static_cast<size_t>(0x84 + base)] | pcm_[static_cast<size_t>(0x85 + base)] << 8);
                const uint32_t end = static_cast<uint32_t>((pcm_[static_cast<size_t>(0x06 + base)] + 1) << 8);
                if (start != s->start || end != s->end)
                    continue;
                players << (players.isEmpty() ? "" : " ") << (ch + 1);
                if (fraction < 0.0f && end > start)
                    fraction = static_cast<float>(std::clamp(((live_.pcmAddress[static_cast<size_t>(ch)] & 0xFFFF) - static_cast<double>(start)) / (end - start), 0.0, 1.0));
            }
            const bool playing = players.isNotEmpty();
            g.setColour(playing ? accent.withAlpha(0.18f) : theme::background.withAlpha(0.6f));
            g.fillRoundedRectangle(tile, 4.0f);
            if (fraction >= 0.0f)
            {
                g.setColour(accent.withAlpha(0.5f));
                g.fillRoundedRectangle(tile.withWidth(std::max(4.0f, tile.getWidth() * fraction)), 4.0f);
            }
            g.setColour(playing ? accent : theme::gridBar.withAlpha(0.7f));
            g.drawRoundedRectangle(tile.reduced(0.5f), 4.0f, playing ? 1.5f : 1.0f);
            g.setFont(theme::mono(font, playing));
            g.setColour(playing ? theme::text : theme::faint);
            // The address in place, lit or not, smaller in a narrow tile; left
            // out only where even 8 points do not fit.
            const auto text = tile.reduced(3.0f, 2.0f);
            const float fits = std::min(font, text.getWidth() / 2.6f); // four hex digits are about 2.6 font sizes wide
            if (fits >= 8.0f)
            {
                g.setFont(theme::mono(fits, playing));
                g.drawText(hex(static_cast<int>(s->start), 4), text, juce::Justification::centred, false);
            }
            if (playing)
            {
                g.setColour(accent);
                g.drawText(players, tile.reduced(4.0f, 2.0f), juce::Justification::topRight, false);
                // Where the first of them reads now, in the bank, room allowing.
                if (fraction >= 0.0f && fits >= 8.0f)
                {
                    int now = 0;
                    for (int ch = 0; ch < 16; ++ch)
                        if (players.containsWholeWord(juce::String(ch + 1)))
                        {
                            now = static_cast<int>(live_.pcmAddress[static_cast<size_t>(ch)] & 0xFFFF);
                            break;
                        }
                    g.setColour(theme::text);
                    g.setFont(theme::mono(fits * 0.85f, true));
                    g.drawText(hex(now, 4), text, juce::Justification::bottomRight, false);
                }
            }
        }
    }
}

void ChipView::paintPcmTable(juce::Graphics& g, juce::Rectangle<float> inner, juce::Colour accent, float valueFont)
{
    // A row per channel: whether it plays, where its sample starts, loops and
    // ends in the ROM, its rate, and its volume on each side as a bar.
    static const char* kColumns[9] = { "START", "LOOP", "END", "RATE", "FLAGS", "BANK", "VOL L", "VOL R", "OUT" };
    const float rowH = inner.getHeight() / 16.7f;
    const float labelW = valueFont * 3.6f, colW = (inner.getWidth() - labelW) / 9.0f;
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
        tip(label, "CH" + juce::String(ch + 1));
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

        const int start = pcm_[static_cast<size_t>(0x84 + base)] | pcm_[static_cast<size_t>(0x85 + base)] << 8;
        const int loop = pcm_[static_cast<size_t>(0x04 + base)] | pcm_[static_cast<size_t>(0x05 + base)] << 8;
        cell(g, row.removeFromLeft(colW), hex(start, 4), std::max(glow(false, 0x84 + base), glow(false, 0x85 + base)), accent, !on);
        cell(g, row.removeFromLeft(colW), hex(loop, 4), std::max(glow(false, 0x04 + base), glow(false, 0x05 + base)), accent, !on);
        cell(g, row.removeFromLeft(colW), hex(pcm_[static_cast<size_t>(0x06 + base)], 2) + "00", glow(false, 0x06 + base), accent, !on);
        cell(g, row.removeFromLeft(colW), juce::String(pcm_[static_cast<size_t>(0x07 + base)]), glow(false, 0x07 + base), accent, !on);
        cell(g, row.removeFromLeft(colW), hex(flags, 2), glow(false, 0x86 + base), accent, !on);
        cell(g, row.removeFromLeft(colW), juce::String((flags >> 4) & 7), glow(false, 0x86 + base), accent, !on);
        for (const int side : { 0x02, 0x03 })
        {
            const auto r = row.removeFromLeft(colW).reduced(6.0f, rowH * 0.3f);
            const int volume = pcm_[static_cast<size_t>(side + base)] & 0x7F;
            g.setColour(theme::gridBar.withAlpha(0.5f));
            g.fillRoundedRectangle(r, 2.0f);
            g.setColour(on ? accent.withAlpha(0.6f + 0.4f * glow(false, side + base)) : theme::faint);
            g.fillRoundedRectangle(r.withWidth(r.getWidth() * static_cast<float>(volume) / 127.0f), 2.0f);
        }
        // Its output now.
        paintMeter(g, row.removeFromLeft(colW).reduced(6.0f, rowH * 0.25f), 8 + ch);
    }
}

} // namespace opm86

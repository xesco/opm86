#include "OpmBank.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace opm86
{

namespace
{

// The line without its comment and the spaces round it.
std::string trimmed(std::string line)
{
    if (const auto comment = line.find("//"); comment != std::string::npos)
        line.erase(comment);
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1);
}

// The whole numbers on a line, in order; a word that is not one ends the list.
std::vector<int> numbers(const std::string& text)
{
    std::vector<int> out;
    std::string spaced = text;
    std::replace(spaced.begin(), spaced.end(), ',', ' ');
    std::istringstream in(spaced);
    std::string word;
    while (in >> word)
    {
        char* end = nullptr;
        const long v = std::strtol(word.c_str(), &end, 10);
        if (end == word.c_str() || *end != '\0')
            break;
        out.push_back(static_cast<int>(std::clamp<long>(v, -100000, 100000)));
    }
    return out;
}

// A field from the list, clamped to its range, or the default when it is missing.
uint8_t field(const std::vector<int>& v, size_t i, int high, uint8_t otherwise)
{
    return i < v.size() ? static_cast<uint8_t>(std::clamp(v[i], 0, high)) : otherwise;
}

} // namespace

std::vector<OpmVoice> parseOpm(const std::string& text)
{
    std::vector<OpmVoice> voices;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw))
    {
        const std::string line = trimmed(raw);
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = line.substr(0, colon);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) != 0; }), key.end());
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const std::string rest = line.substr(colon + 1);
        if (key == "@")
        {
            // A new voice: its number, then its name, which may hold spaces.
            OpmVoice voice;
            const std::vector<int> v = numbers(rest);
            voice.number = v.empty() ? static_cast<int>(voices.size()) : std::clamp(v[0], 0, 127);
            std::istringstream words(rest);
            std::string number, name;
            words >> number;
            std::getline(words, name);
            voice.patch.name = v.empty() ? std::string() : trimmed(name);
            voices.push_back(voice);
            continue;
        }
        // The lines before the first voice belong to none.
        if (voices.empty())
            continue;
        OpmVoice& voice = voices.back();
        FmPatch& p = voice.patch;
        const std::vector<int> v = numbers(rest);
        if (key == "LFO")
        {
            voice.lfo = true;
            p.lfoFreq = field(v, 0, 255, p.lfoFreq);
            p.amd = field(v, 1, 127, p.amd);
            p.pmd = field(v, 2, 127, p.pmd);
            p.lfoWave = field(v, 3, 3, p.lfoWave);
            voice.noiseFreq = field(v, 4, 31, voice.noiseFreq);
        }
        else if (key == "CH")
        {
            // The output sides belong to a song's voices, so PAN is not kept.
            p.fb = field(v, 1, 7, p.fb);
            p.alg = field(v, 2, 7, p.alg);
            p.ams = field(v, 3, 3, p.ams);
            p.pms = field(v, 4, 7, p.pms);
            if (v.size() > 5)
                for (size_t i = 0; i < 4; ++i)
                    p.op[i].on = (v[5] & (0x08 << i)) != 0;
            if (v.size() > 6)
                voice.noise = v[6] != 0;
        }
        else if (key == "M1" || key == "C1" || key == "M2" || key == "C2")
        {
            // In the file's order, which is the patch's: M1, C1, M2, C2.
            const size_t i = key == "M1" ? 0 : key == "C1" ? 1 : key == "M2" ? 2 : 3;
            FmOperator& o = p.op[i];
            o.ar = field(v, 0, 31, o.ar);
            o.d1r = field(v, 1, 31, o.d1r);
            o.d2r = field(v, 2, 31, o.d2r);
            o.rr = field(v, 3, 15, o.rr);
            o.d1l = field(v, 4, 15, o.d1l);
            o.tl = field(v, 5, 127, o.tl);
            o.ks = field(v, 6, 3, o.ks);
            o.mul = field(v, 7, 15, o.mul);
            if (v.size() > 8)
            {
                // The register's sign and size: 4-7 detune down.
                const int dt1 = std::clamp(v[8], 0, 7);
                o.dt1 = static_cast<int8_t>(dt1 >= 4 ? 4 - dt1 : dt1);
            }
            o.dt2 = field(v, 9, 3, o.dt2);
            if (v.size() > 10)
                o.amEn = v[10] != 0;
        }
    }
    return voices;
}

std::string writeOpm(const FmPatch& patch)
{
    // VOPM's own header, which other programs' banks copy word for word.
    std::string out = "//MiOPMdrv sound bank Paramer Ver2002.04.22\n"
                      "//LFO: LFRQ AMD PMD WF NFRQ\n"
                      "//@:[Num] [Name]\n"
                      "//CH: PAN FL CON AMS PMS SLOT NE\n"
                      "//[OPname]: AR D1R D2R RR D1L TL KS MUL DT1 DT2 AMS-EN\n\n";
    // A name on one line, with no comment in it.
    std::string name = patch.name;
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
    if (const auto comment = name.find("//"); comment != std::string::npos)
        name.erase(comment);
    char line[160];
    out += "@:0 " + name + "\n";
    std::snprintf(line, sizeof line, "LFO: %3d %3d %3d %3d %3d\n", patch.lfoFreq, patch.amd & 0x7F, patch.pmd & 0x7F, patch.lfoWave & 3, 0);
    out += line;
    std::snprintf(line, sizeof line, "CH: %3d %3d %3d %3d %3d %3d %3d\n", 64, patch.fb & 7, patch.alg & 7, patch.ams & 3, patch.pms & 7, keyOnMask(patch), 0);
    out += line;
    for (size_t i = 0; i < 4; ++i)
    {
        const FmOperator& o = patch.op[i];
        const int dt1 = o.dt1 >= 0 ? o.dt1 : 4 - o.dt1;
        std::snprintf(line, sizeof line, "%s: %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d %3d\n", kOperatorNames[i], o.ar, o.d1r, o.d2r, o.rr, o.d1l, o.tl, o.ks,
                      o.mul, dt1, o.dt2, o.amEn ? 128 : 0);
        out += line;
    }
    return out;
}

} // namespace opm86

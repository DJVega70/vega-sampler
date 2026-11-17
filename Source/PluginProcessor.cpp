#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
enum { kMode, kSliceMode, kSlices, kSens, kRoot, kPitch, kFine, kAttack, kRelease, kCutoff, kReso, kReverse, kChoke, kLoopBpm, kGain, kLoop };
const char* ids[] = { "mode", "sliceMode", "slices", "sensitivity", "root", "pitch", "fine", "attack", "release", "cutoff", "resonance", "reverse", "choke", "loopBpm", "gain", "loop" };
constexpr int numP = 16;
constexpr int grainLen = 2048;
}

juce::AudioProcessorValueTreeState::ParameterLayout VegaSamplerProcessor::layout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> v;
    auto ms = [] (float x, int) { return x < 10 ? String (x, 1) + " ms" : String (roundToInt (x)) + " ms"; };
    auto F = [&] (int id, const char* name, NormalisableRange<float> r, float def, std::function<String (float, int)> f)
    { v.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ids[id], 1 }, name, r, def, AudioParameterFloatAttributes().withStringFromValueFunction (f))); };
    v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids[kMode], 1 }, "Mode", StringArray { "Slices", "Keys", "Stretch" }, 0));
    v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids[kSliceMode], 1 }, "Slice By", StringArray { "Grid", "Transients" }, 1));
    v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids[kSlices], 1 }, "Grid Slices", StringArray { "4", "8", "16", "32" }, 2));
    F (kSens, "Sensitivity", NormalisableRange<float> (0, 100), 55, [] (float x, int) { return String (roundToInt (x)) + "%"; });
    v.push_back (std::make_unique<AudioParameterInt> (ParameterID { ids[kRoot], 1 }, "Root Note", 24, 96, 60, AudioParameterIntAttributes().withStringFromValueFunction ([] (int n, int) { return MidiMessage::getMidiNoteName (n, true, true, 3); })));
    F (kPitch, "Pitch", NormalisableRange<float> (-24, 24, 1), 0, [] (float x, int) { return (x > 0 ? "+" : "") + String (roundToInt (x)) + " st"; });
    F (kFine, "Fine", NormalisableRange<float> (-100, 100), 0, [] (float x, int) { return (x > 0 ? "+" : "") + String (roundToInt (x)) + " ct"; });
    F (kAttack, "Attack", NormalisableRange<float> (0.1f, 2000, 0, 0.3f), 1, ms);
    F (kRelease, "Release", NormalisableRange<float> (1, 4000, 0, 0.3f), 30, ms);
    F (kCutoff, "Cutoff", NormalisableRange<float> (40, 20000, 0, 0.25f), 20000, [] (float x, int) { return x >= 1000 ? String (x / 1000, 1) + " kHz" : String (roundToInt (x)) + " Hz"; });
    F (kReso, "Resonance", NormalisableRange<float> (0, 100), 10, [] (float x, int) { return String (roundToInt (x)) + "%"; });
    v.push_back (std::make_unique<AudioParameterBool> (ParameterID { ids[kReverse], 1 }, "Reverse", false));
    v.push_back (std::make_unique<AudioParameterBool> (ParameterID { ids[kChoke], 1 }, "Choke (mono)", true));
    F (kLoopBpm, "Loop BPM", NormalisableRange<float> (60, 200), 120, [] (float x, int) { return String (x, 1) + " BPM"; });
    F (kGain, "Gain", NormalisableRange<float> (-24, 12), 0, [] (float x, int) { return String (x, 1) + " dB"; });
    v.push_back (std::make_unique<AudioParameterBool> (ParameterID { ids[kLoop], 1 }, "Loop", false));
    return { v.begin(), v.end() };
}

VegaSamplerProcessor::VegaSamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)), apvts (*this, nullptr, "PARAMS", layout())
{
    for (int i = 0; i < numP; ++i) p[i] = apvts.getRawParameterValue (ids[i]);
    for (auto& v : voicePos) v.store (-1);
}
VegaSamplerProcessor::~VegaSamplerProcessor() { loader.removeAllJobs (true, 20000); if (auto* d = current.exchange (nullptr)) retired.emplace_back (d); }

bool VegaSamplerProcessor::isBusesLayoutSupported (const BusesLayout& l) const { return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo(); }

void VegaSamplerProcessor::prepareToPlay (double s, int)
{
    sr = s;
    for (auto& v : voices) { v.active = false; v.adsr.setSampleRate (s); }
}

std::vector<int> VegaSamplerProcessor::detectTransients (const juce::AudioBuffer<float>& a, double rate, float sens)
{
    const int n = a.getNumSamples(), hop = 256, win = 512;
    std::vector<float> e;
    for (int s = 0; s + win <= n; s += hop)
    {
        double acc = 0; for (int i = 0; i < win; ++i) { const float x = 0.5f * (a.getSample (0, s + i) + a.getSample (a.getNumChannels() > 1 ? 1 : 0, s + i)); acc += x * x; }
        e.push_back ((float) (acc / win));
    }
    std::vector<int> out { 0 };
    const float ratio = 1.5f + (1.0f - sens) * 18.0f;           // higher sensitivity = smaller jump needed
    float peak = 0; for (float v : e) peak = std::max (peak, v);
    const float floorE = peak * std::pow (10.0f, -(20.0f + sens * 30.0f) / 10.0f);
    const int minGap = (int) (rate * 0.07);
    for (size_t k = 4; k < e.size(); ++k)
    {
        const float prev = (e[k - 2] + e[k - 3] + e[k - 4]) / 3.0f + 1.0e-10f;
        if (e[k] > floorE && e[k] / prev > ratio && e[k] >= e[k - 1])
        {
            // find the start of the rise inside the frame: walk back to the first sample above a fraction of the frame peak
            int pos = (int) k * hop;
            float pk = 0; for (int i = 0; i < win && pos + i < n; ++i) pk = std::max (pk, std::abs (a.getSample (0, pos + i)));
            int onset = pos - hop; for (int i = std::max (0, pos - hop); i < std::min (n, pos + win); ++i) if (std::abs (a.getSample (0, i)) > pk * 0.2f) { onset = i; break; }
            onset = std::max (0, onset - (int) (rate * 0.001));     // 1 ms pre-roll so the attack isn't clipped
            if (onset - out.back() >= minGap) out.push_back (onset);
        }
    }
    return out;
}

bool VegaSamplerProcessor::loadFile (const juce::File& f, juce::String& error)
{
    const juce::ScopedLock sl (loadLock);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) { error = "Can't read " + f.getFileName(); return false; }
    const int len = (int) std::min (r->lengthInSamples, (juce::int64) (r->sampleRate * 120));
    if (len < 64) { error = "File is empty"; return false; }
    auto d = std::make_unique<SampleData>();
    d->name = f.getFileNameWithoutExtension(); d->file = f; d->rate = r->sampleRate;
    d->audio.setSize (2, len);
    r->read (&d->audio, 0, len, 0, true, r->numChannels > 1);
    if (r->numChannels == 1) d->audio.copyFrom (1, 0, d->audio, 0, 0, len);
    // tempo guess: assume a whole number of bars (4/4) between 80 and 170 BPM, prefer ~124
    const double secs = len / r->sampleRate; double best = 120, bestErr = 1e9;
    for (int beats : { 1, 2, 4, 8, 16, 32, 64 }) { const double b = beats * 60.0 / secs; if (b >= 80 && b <= 170 && std::abs (b - 124) < bestErr) { best = b; bestErr = std::abs (b - 124); } }
    d->guessedBpm = best;
    // slices
    const bool trans = p[kSliceMode]->load() > 0.5f;
    if (trans) d->slices = detectTransients (d->audio, d->rate, p[kSens]->load() * 0.01f);
    if (! trans || d->slices.size() < 2) { const int nsl = 4 << (int) p[kSlices]->load(); d->slices.clear(); for (int i = 0; i < nsl; ++i) d->slices.push_back ((int) ((juce::int64) len * i / nsl)); }
    if (auto* lb = apvts.getParameter (ids[kLoopBpm])) juce::MessageManager::callAsync ([lb, best] { lb->setValueNotifyingHost (lb->convertTo0to1 ((float) best)); });
    if (auto* old = current.exchange (d.release())) retired.emplace_back (old);
    trimRetired();
    return true;
}

void VegaSamplerProcessor::reslice()
{
    const juce::ScopedLock sl (loadLock);
    const auto* cur = current.load(); if (cur == nullptr) return;
    auto d = std::make_unique<SampleData>();
    d->name = cur->name; d->file = cur->file; d->rate = cur->rate; d->audio.makeCopyOf (cur->audio); d->guessedBpm = cur->guessedBpm;
    const int len = d->audio.getNumSamples();
    if (p[kSliceMode]->load() > 0.5f) d->slices = detectTransients (d->audio, d->rate, p[kSens]->load() * 0.01f);
    if (p[kSliceMode]->load() < 0.5f || d->slices.size() < 2) { const int nsl = 4 << (int) p[kSlices]->load(); d->slices.clear(); for (int i = 0; i < nsl; ++i) d->slices.push_back ((int) ((juce::int64) len * i / nsl)); }
    if (auto* old = current.exchange (d.release())) retired.emplace_back (old);
    trimRetired();
}

float VegaSamplerProcessor::readSample (const SampleData& d, int ch, double pos) const
{
    const int n = d.audio.getNumSamples();
    if (pos < 0 || pos >= n - 1) return 0.0f;
    const int i = (int) pos; const float f = (float) (pos - i);
    const float* s = d.audio.getReadPointer (ch);
    const float y0 = i > 0 ? s[i - 1] : s[i], y1 = s[i], y2 = s[i + 1], y3 = i + 2 < n ? s[i + 2] : s[i + 1];
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void VegaSamplerProcessor::startVoice (int note, float vel, const SampleData& d)
{
    const int mode = (int) p[kMode]->load();
    int sliceIdx = -1;
    if (mode == Slices) { sliceIdx = note - firstSliceNote; if (sliceIdx < 0 || sliceIdx >= (int) d.slices.size()) return; }
    if (p[kChoke]->load() > 0.5f) for (auto& v : voices) if (v.active) v.adsr.noteOff();     // mono chopping: new hit releases the old one
    Voice* vo = nullptr;
    for (auto& v : voices) if (! v.active) { vo = &v; break; }
    if (vo == nullptr) vo = &voices[0];
    for (auto& v : voices) if (v.active && v.note == note) { vo = &v; break; }    // retrigger same key
    auto& v = *vo;
    v.data = &d; v.note = note; v.vel = vel; v.active = true;
    v.reverse = p[kReverse]->load() > 0.5f; v.loop = p[kLoop]->load() > 0.5f;
    const double pitch = std::pow (2.0, (p[kPitch]->load() + p[kFine]->load() * 0.01) / 12.0);
    if (mode == Slices) { v.start = d.slices[(size_t) sliceIdx]; v.end = d.sliceEnd (sliceIdx); v.inc = pitch * d.rate / sr; lastSlice.store (sliceIdx); }
    else
    {
        v.start = 0; v.end = d.audio.getNumSamples();
        const double keyRatio = std::pow (2.0, (note - (int) p[kRoot]->load()) / 12.0);
        v.inc = (mode == Keys ? keyRatio : 1.0) * pitch * d.rate / sr;
        if (mode == Stretch) { v.inc = keyRatio * pitch * d.rate / sr; v.readHead = v.reverse ? v.end - 1 : 0; v.hopCount = 0; v.nextGrain = 0; v.grainAge[0] = v.grainAge[1] = grainLen; }
    }
    v.pos = v.reverse ? v.end - 1 : v.start;
    v.adsr.setParameters ({ p[kAttack]->load() * 0.001f, 0.0f, 1.0f, p[kRelease]->load() * 0.001f });
    v.adsr.reset(); v.adsr.noteOn();
    for (int c = 0; c < 2; ++c) v.ic1[c] = v.ic2[c] = 0;
}

void VegaSamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals nd;
    const int n = buffer.getNumSamples();
    buffer.clear();
    const auto* d = current.load();
    if (auto* ph = getPlayHead()) if (auto pos = ph->getPosition()) if (auto b = pos->getBpm()) hostBpm = juce::jlimit (20.0, 400.0, *b);
    if (d == nullptr) { for (auto& v : voices) v.active = false; return; }
    if (const int an = auditionNote.exchange (-1); an >= 0) { midi.addEvent (juce::MidiMessage::noteOn (1, an, 0.9f), 0); auditionHeld = an; auditionLeft = (int) (sr * 0.8); }
    if (auditionHeld >= 0 && (auditionLeft -= n) <= 0) { midi.addEvent (juce::MidiMessage::noteOff (1, auditionHeld), 0); auditionHeld = -1; }

    const int mode = (int) p[kMode]->load();
    const float gain = juce::Decibels::decibelsToGain (p[kGain]->load());
    const float cutoff = p[kCutoff]->load(), k = 2.0f - 1.9f * p[kReso]->load() * 0.01f;
    const float g = std::tan (juce::MathConstants<float>::pi * std::min (cutoff, (float) sr * 0.45f) / (float) sr);
    const bool filt = cutoff < 19900.0f;
    const double stretchSpeed = hostBpm / std::max (20.0f, p[kLoopBpm]->load()) * d->rate / sr;   // source samples per output sample
    const int hop = grainLen / 2;
    float* L = buffer.getWritePointer (0); float* R = buffer.getWritePointer (1);
    auto it = midi.cbegin();
    for (int i = 0; i < n; ++i)
    {
        while (it != midi.cend() && (*it).samplePosition <= i)
        {
            const auto m = (*it).getMessage();
            if (m.isNoteOn()) startVoice (m.getNoteNumber(), m.getFloatVelocity(), *d);
            else if (m.isNoteOff()) for (auto& v : voices) if (v.active && v.note == m.getNoteNumber() && (mode != Slices || v.loop)) v.adsr.noteOff();
            else if (m.isAllNotesOff() || m.isAllSoundOff()) for (auto& v : voices) v.adsr.noteOff();
            ++it;
        }
        for (auto& v : voices)
        {
            if (! v.active) continue;
            if (v.data != d) { v.active = false; continue; }
            float s[2] {};
            if (mode == Stretch)
            {
                // two overlapping Hann grains read at pitch rate; the read head moves at tempo rate
                if (v.hopCount <= 0)
                {
                    v.grainPos[v.nextGrain] = v.readHead; v.grainAge[v.nextGrain] = 0; v.nextGrain ^= 1; v.hopCount = hop;
                }
                --v.hopCount;
                for (int gI = 0; gI < 2; ++gI)
                {
                    if (v.grainAge[gI] >= grainLen) continue;
                    const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * v.grainAge[gI] / grainLen);
                    for (int c = 0; c < 2; ++c) s[c] += readSample (*d, c, v.grainPos[gI]) * w;
                    v.grainPos[gI] += v.reverse ? -v.inc : v.inc; ++v.grainAge[gI];
                }
                v.readHead += v.reverse ? -stretchSpeed : stretchSpeed;
                const int len = v.end;
                if (v.readHead >= len || v.readHead < 0) { if (v.loop) v.readHead = v.reverse ? len - 1 : 0; else v.adsr.noteOff(); }
                v.pos = v.readHead;
            }
            else
            {
                for (int c = 0; c < 2; ++c) s[c] = readSample (*d, c, v.pos);
                v.pos += v.reverse ? -v.inc : v.inc;
                if (v.pos >= v.end || v.pos < v.start)
                {
                    if (v.loop) v.pos = v.reverse ? v.end - 1 : v.start;
                    else { v.active = false; continue; }
                }
            }
            const float env = v.adsr.getNextSample();
            if (! v.adsr.isActive()) { v.active = false; continue; }
            for (int c = 0; c < 2; ++c)
            {
                float y = s[c] * env * v.vel * gain;
                if (filt)
                {
                    const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
                    const float v3 = y - v.ic2[c], v1 = a1 * v.ic1[c] + a2 * v3, v2 = v.ic2[c] + a2 * v.ic1[c] + a3 * v3;
                    v.ic1[c] = 2 * v1 - v.ic1[c]; v.ic2[c] = 2 * v2 - v.ic2[c]; y = v2;
                }
                (c == 0 ? L : R)[i] += y;
            }
        }
    }
    for (size_t k2 = 0; k2 < voices.size(); ++k2) voicePos[k2].store (voices[k2].active ? (float) voices[k2].pos : -1.0f);
    outL.store (std::max (outL.load(), buffer.getMagnitude (0, 0, n))); outR.store (std::max (outR.load(), buffer.getMagnitude (1, 0, n)));
}

juce::File VegaSamplerProcessor::exportMidi (const juce::File& dest) const
{
    juce::MidiMessageSequence seq; const int tpq = 960;
    if (const auto* d = current.load())
    {
        const double spb = d->rate * 60.0 / std::max (20.0f, p[kLoopBpm]->load());       // source samples per beat
        for (size_t i = 0; i < d->slices.size(); ++i)
        {
            const double t0 = d->slices[i] / spb * tpq, t1 = d->sliceEnd ((int) i) / spb * tpq;
            seq.addEvent (juce::MidiMessage::noteOn (1, firstSliceNote + (int) i, (juce::uint8) 100), t0);
            seq.addEvent (juce::MidiMessage::noteOff (1, firstSliceNote + (int) i), std::max (t0 + 10, t1 - 1));
        }
    }
    seq.updateMatchedPairs();
    juce::MidiFile mf; mf.setTicksPerQuarterNote (tpq); mf.addTrack (seq);
    auto f = dest != juce::File() ? dest : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Vega Sampler Slices.mid");
    f.deleteFile(); juce::FileOutputStream os (f); if (os.openedOk()) mf.writeTo (os);
    return f;
}

void VegaSamplerProcessor::getStateInformation (juce::MemoryBlock& dst)
{
    auto s = apvts.copyState(); s.setProperty ("uiScale", uiScale.load(), nullptr);
    if (const auto* d = current.load())
    {
        s.setProperty ("sampleFile", d->file.getFullPathName(), nullptr);
        juce::StringArray sl; for (int v : d->slices) sl.add (juce::String (v));
        s.setProperty ("slicePoints", sl.joinIntoString (","), nullptr);
    }
    if (auto x = s.createXml()) copyXmlToBinary (*x, dst);
}
void VegaSamplerProcessor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
    {
        auto s = juce::ValueTree::fromXml (*x);
        if (! s.hasType (apvts.state.getType())) return;
        uiScale.store ((float) s.getProperty ("uiScale", 1.0f));
        const juce::File f (s.getProperty ("sampleFile").toString());
        const auto slicesStr = s.getProperty ("slicePoints").toString();
        apvts.replaceState (s);
        if (f.existsAsFile())
        {
            juce::String err;
            if (loadFile (f, err) && slicesStr.isNotEmpty())
            {   // restore exact slice points
                const juce::ScopedLock sl (loadLock);
                auto* cur = current.load(); auto d = std::make_unique<SampleData>();
                d->name = cur->name; d->file = cur->file; d->rate = cur->rate; d->audio.makeCopyOf (cur->audio); d->guessedBpm = cur->guessedBpm;
                for (auto& t : juce::StringArray::fromTokens (slicesStr, ",", "")) d->slices.push_back (juce::jlimit (0, d->audio.getNumSamples() - 1, t.getIntValue()));
                if (auto* old = current.exchange (d.release())) retired.emplace_back (old);
    trimRetired();
            }
        }
    }
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VegaSamplerProcessor(); }

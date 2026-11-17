#include "../Source/PluginProcessor.h"
#include <iostream>
static int fails = 0;
static void check (bool ok, const juce::String& m) { std::cout << (ok ? "PASS " : "FAIL ") << m << "\n"; if (! ok) ++fails; }
static void set (VegaSamplerProcessor& p, const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); }
const double fs = 48000; const int blk = 480;
struct Head : juce::AudioPlayHead { double bpm = 120; juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (bpm); return i; } };

// 2-second test loop at 48k (= 1 bar at 120 BPM): 4 hits (one per beat), each a decaying sine at a different pitch
static float loopSample (int i)
{
    const int beat = i / 24000, t = i % 24000;
    const double f = 200.0 * (beat + 1);
    return (float) (0.8 * std::exp (-t / 3000.0) * std::sin (2 * juce::MathConstants<double>::pi * f * t / fs));
}
static juce::File writeLoop()
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vega_sampler_test.wav");
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (file), fs, 2, 32, {}, 0));
    juce::AudioBuffer<float> b (2, 96000); for (int i = 0; i < 96000; ++i) { b.setSample (0, i, loopSample (i)); b.setSample (1, i, loopSample (i)); }
    w->writeFromAudioSampleBuffer (b, 0, 96000);
    return file;
}
static std::vector<float> play (VegaSamplerProcessor& p, int note, int samples)
{
    std::vector<float> out; juce::AudioBuffer<float> b (2, blk);
    for (int pos = 0; pos < samples; pos += blk)
    {
        juce::MidiBuffer m; if (pos == 0) m.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
        p.processBlock (b, m); for (int i = 0; i < blk; ++i) out.push_back (b.getSample (0, i));
    }
    return out;
}
static int crossings (const std::vector<float>& v, int a, int b) { int z = 0; for (int i = a + 1; i < b; ++i) z += (v[(size_t) i - 1] < 0) != (v[(size_t) i] < 0); return z; }

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto file = writeLoop();
    auto make = [&] { auto* p = new VegaSamplerProcessor(); p->setPlayConfigDetails (0, 2, fs, blk); p->prepareToPlay (fs, blk); return std::unique_ptr<VegaSamplerProcessor> (p); };
    {
        auto p = make(); set (*p, "sliceMode", 1); set (*p, "sensitivity", 55);
        juce::String err; check (p->loadFile (file, err), "loads a WAV " + err);
        const auto* d = p->sample();
        juce::String pos; bool ok = d != nullptr && d->slices.size() == 4;
        if (d) for (size_t i = 0; i < d->slices.size(); ++i) { pos << d->slices[i] << " "; ok &= std::abs (d->slices[i] - (int) i * 24000) < 200; }
        check (ok, "transient detection finds the 4 hits: " + pos);
        check (d && std::abs (d->guessedBpm - 120) < 0.5, "tempo guess " + juce::String (d ? d->guessedBpm : 0, 1) + " BPM");
        // slice mode: note 37 plays slice 2 exactly
        set (*p, "mode", 0); set (*p, "attack", 0.1f); set (*p, "release", 1);
        auto o = play (*p, 37, 24000);
        double err2 = 0; const int s1 = d->slices[1];
        for (int i = 200; i < 20000; ++i) err2 = std::max (err2, (double) std::abs (o[(size_t) i] - loopSample (s1 + i)));
        check (err2 < 0.02, "C#1 plays slice 2 sample-accurately (max error " + juce::String (err2, 4) + ")");
        // choke: a new slice cuts the previous one
        auto o2 = play (*p, 36, 4800);
        check (crossings (o2, 1000, 4800) > 0, "slice 1 plays after slice 2 (choke mode)");
        // grid slicing
        set (*p, "sliceMode", 0); set (*p, "slices", 1); p->reslice();
        check (p->sample()->slices.size() == 8 && p->sample()->slices[1] == 12000, "grid: 8 equal slices");
    }
    {   // keys mode: one octave up doubles the pitch and halves the time
        auto p = make(); juce::String err; p->loadFile (file, err);
        set (*p, "mode", 1); set (*p, "root", 60); set (*p, "release", 1);
        auto a = play (*p, 60, 12000); auto b = play (*p, 72, 12000);
        const int za = crossings (a, 100, 6000), zb = crossings (b, 100, 6000);
        check (std::abs (zb - 2 * za) <= 4, "keys: C5 vs C4 = double the pitch (" + juce::String (za) + " -> " + juce::String (zb) + " crossings)");
    }
    {   // stretch mode: host at 240 BPM plays the 120 BPM loop twice as fast without changing pitch
        auto p = make(); juce::String err; p->loadFile (file, err);
        Head head; head.bpm = 240; p->setPlayHead (&head);
        set (*p, "mode", 2); set (*p, "root", 60); set (*p, "loopBpm", 120); set (*p, "release", 1);
        auto o = play (*p, 60, 60000);
        // beat 2 of the loop (400 Hz) should now start ~12000 samples in; check its pitch ~400 Hz
        const int z = crossings (o, 12600, 15000);
        const double f = z / 2.0 / (2400.0 / fs);
        check (std::abs (f - 400) < 40, "stretch: 2nd hit arrives at 12000 (half time) still at 400 Hz (" + juce::String (f, 0) + " Hz)");
        double late = 0; for (int i = 50000; i < 60000; ++i) late = std::max (late, (double) std::abs (o[(size_t) i]));
        check (late < 0.02, "stretched loop finishes after one bar at the new tempo");
        p->setPlayHead (nullptr);
    }
    {   // MIDI export of slices (rebuild the loop)
        auto p = make(); set (*p, "sliceMode", 1); juce::String err; p->loadFile (file, err); set (*p, "loopBpm", 120);
        auto f = p->exportMidi(); juce::MidiFile mf; juce::FileInputStream in (f); mf.readFrom (in);
        std::vector<double> t; for (auto* e : *mf.getTrack (0)) if (e->message.isNoteOn()) t.push_back (e->message.getTimeStamp());
        check (t.size() == 4 && std::abs (t[1] - 960) < 10 && std::abs (t[3] - 2880) < 10, "slice MIDI rebuilds the loop on the beat grid");
        f.deleteFile();
    }
    {   // state restores the sample and slice points
        auto p = make(); set (*p, "sliceMode", 1); juce::String err; p->loadFile (file, err);
        juce::MemoryBlock mb; p->getStateInformation (mb);
        auto q = make(); q->setStateInformation (mb.getData(), (int) mb.getSize());
        check (q->sample() != nullptr && q->sample()->slices == p->sample()->slices, "project reload restores sample and slices");
    }
    file.deleteFile();
    std::cout << (fails == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string (fails)) << "\n";
    return fails == 0 ? 0 : 1;
}

#include "PluginEditor.h"

namespace
{
class WaveView : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit WaveView (VegaSamplerProcessor& p) : proc (p) {}
    std::function<void (const juce::File&)> onFile;
    void paint (juce::Graphics& g) override
    {
        vegaui::drawScreen (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().toFloat().reduced (10.0f, 22.0f);
        const auto* d = proc.sample();
        if (dragOver) { g.setColour (VegaColours::accent.withAlpha (0.15f)); g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f); }
        if (d == nullptr)
        {
            g.setColour (VegaColours::textDim); g.setFont (VegaFonts::ui (15.0f, true));
            g.drawText ("DROP A LOOP, VOCAL OR ONE-SHOT HERE", r.toNearestInt(), juce::Justification::centred);
            g.setFont (VegaFonts::ui (11.0f));
            g.drawText ("(or press LOAD - WAV, AIFF, FLAC, OGG, MP3)", r.toNearestInt().translated (0, 24), juce::Justification::centred);
            return;
        }
        if (d != cachedFor || (int) r.getWidth() != (int) peaks.size()) rebuild (*d, (int) r.getWidth());
        const int mode = (int) proc.apvts.getRawParameterValue ("mode")->load();
        const float mid = r.getCentreY(), h = r.getHeight() * 0.5f;
        const int len = d->audio.getNumSamples();
        // slice regions
        if (mode == VegaSamplerProcessor::Slices)
            for (size_t i = 0; i < d->slices.size(); ++i)
            {
                const float x0 = r.getX() + r.getWidth() * d->slices[i] / len, x1 = r.getX() + r.getWidth() * d->sliceEnd ((int) i) / len;
                if ((int) i == proc.lastSlice.load()) { g.setColour (VegaColours::accent.withAlpha (0.12f)); g.fillRect (x0, r.getY(), x1 - x0, r.getHeight()); }
                if ((int) i == hoverSlice) { g.setColour (juce::Colours::white.withAlpha (0.05f)); g.fillRect (x0, r.getY(), x1 - x0, r.getHeight()); }
            }
        g.setColour (VegaColours::accent.withAlpha (0.85f));
        for (int x = 0; x < (int) peaks.size(); ++x) g.drawVerticalLine ((int) r.getX() + x, mid - peaks[(size_t) x].second * h, mid - peaks[(size_t) x].first * h + 1);
        if (mode == VegaSamplerProcessor::Slices)
        {
            g.setFont (VegaFonts::ui (9.5f, true));
            for (size_t i = 0; i < d->slices.size(); ++i)
            {
                const float x = r.getX() + r.getWidth() * d->slices[i] / len;
                g.setColour (VegaColours::gold); g.fillRect (x, r.getY() - 14, 1.5f, r.getHeight() + 14);
                const juce::String lbl = juce::MidiMessage::getMidiNoteName (VegaSamplerProcessor::firstSliceNote + (int) i, true, true, 3);
                g.drawText (juce::String ((int) i + 1) + " " + lbl, (int) x + 3, (int) r.getY() - 16, 60, 12, juce::Justification::left);
            }
        }
        for (auto& vp : proc.voicePos)
        {
            const float pos = vp.load(); if (pos < 0) continue;
            g.setColour (juce::Colours::white); g.fillRect (r.getX() + r.getWidth() * pos / len, r.getY(), 1.5f, r.getHeight());
        }
        g.setColour (VegaColours::textDim); g.setFont (VegaFonts::ui (10.0f));
        g.drawText (d->name + "   " + juce::String (len / d->rate, 2) + " s   " + juce::String ((int) d->slices.size()) + " slices", getLocalBounds().reduced (10, 4).removeFromBottom (14), juce::Justification::left);
    }
    void mouseMove (const juce::MouseEvent& e) override { const int s = sliceAt (e.position.x); if (s != hoverSlice) { hoverSlice = s; repaint(); } }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (proc.sample() == nullptr) return;
        const int mode = (int) proc.apvts.getRawParameterValue ("mode")->load();
        if (mode == VegaSamplerProcessor::Slices) { const int s = sliceAt (e.position.x); if (s >= 0) proc.audition (VegaSamplerProcessor::firstSliceNote + s); }
        else proc.audition ((int) proc.apvts.getRawParameterValue ("root")->load());
    }
    bool isInterestedInFileDrag (const juce::StringArray& f) override { return f.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
    void filesDropped (const juce::StringArray& f, int, int) override { dragOver = false; repaint(); if (onFile) onFile (juce::File (f[0])); }
    void invalidate() { cachedFor = nullptr; repaint(); }
private:
    int sliceAt (float x) const
    {
        const auto* d = proc.sample(); if (d == nullptr) return -1;
        auto r = getLocalBounds().toFloat().reduced (10.0f, 22.0f);
        const double pos = (x - r.getX()) / r.getWidth() * d->audio.getNumSamples();
        for (int i = (int) d->slices.size() - 1; i >= 0; --i) if (pos >= d->slices[(size_t) i]) return i;
        return -1;
    }
    void rebuild (const SampleData& d, int w)
    {
        cachedFor = &d; peaks.assign ((size_t) std::max (1, w), { 0.0f, 0.0f });
        const int len = d.audio.getNumSamples();
        for (int x = 0; x < w; ++x)
        {
            const int a = (int) ((juce::int64) len * x / w), b = std::max (a + 1, (int) ((juce::int64) len * (x + 1) / w));
            float mn = 0, mx = 0;
            for (int i = a; i < b; ++i) { const float v = 0.5f * (d.audio.getSample (0, i) + d.audio.getSample (1, i)); mn = std::min (mn, v); mx = std::max (mx, v); }
            peaks[(size_t) x] = { mn, mx };
        }
    }
    VegaSamplerProcessor& proc; const SampleData* cachedFor = nullptr;
    std::vector<std::pair<float, float>> peaks; int hoverSlice = -1; bool dragOver = false;
};

class MidiDrag : public juce::Component
{
public:
    explicit MidiDrag (VegaSamplerProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (VegaColours::accent.withAlpha (0.2f)); g.fillRoundedRectangle (b, 4.0f);
        g.setColour (VegaColours::accent); g.drawRoundedRectangle (b, 4.0f, 1.2f);
        g.setFont (VegaFonts::ui (11.0f, true)); g.drawText ("DRAG SLICE MIDI", b.toNearestInt(), juce::Justification::centred);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 6 || proc.sample() == nullptr) return;
        dragging = true;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ proc.exportMidi().getFullPathName() }, false, this, [this] { dragging = false; });
    }
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }
private:
    VegaSamplerProcessor& proc; bool dragging = false;
};
}

class VegaSamplerEditor::Main : public juce::Component, private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit Main (VegaSamplerProcessor& p) : proc (p), a (p.apvts), wave (p), midiDrag (p),
        mode (a, "mode", "Mode"), sliceMode (a, "sliceMode", "Slice By"), slices (a, "slices", "Grid"), sens (a, "sensitivity", "Sensitivity"),
        root (a, "root", "Root Note"), pitch (a, "pitch", "Pitch"), fine (a, "fine", "Fine"), loopBpm (a, "loopBpm", "Loop BPM"),
        attack (a, "attack", "Attack"), release (a, "release", "Release"), cutoff (a, "cutoff", "Cutoff"), reso (a, "resonance", "Resonance"),
        reverse (a, "reverse", "Reverse"), choke (a, "choke", "Choke"), loop (a, "loop", "Loop"), gain (a, "gain", "Gain"),
        keyboard (keyState, juce::MidiKeyboardComponent::horizontalKeyboard)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &wave, &midiDrag, &mode, &sliceMode, &slices, &sens, &root, &pitch, &fine, &loopBpm, &attack, &release, &cutoff, &reso,
                                                                 &reverse, &choke, &loop, &gain, &meter, &keyboard })
            addAndMakeVisible (c);
        keyboard.setAvailableRange (24, 96); keyboard.setInterceptsMouseClicks (false, false);
        loadBtn.setButtonText ("LOAD..."); loadBtn.onClick = [this] { choose(); }; addAndMakeVisible (loadBtn);
        wave.onFile = [this] (const juce::File& f) { load (f); };
        for (auto* id : { "sliceMode", "slices", "sensitivity" }) a.addParameterListener (id, this);
        sens.slider.onDragEnd = [this] { proc.reslice(); wave.invalidate(); };
    }
    ~Main() override { for (auto* id : { "sliceMode", "slices", "sensitivity" }) a.removeParameterListener (id, this); }
    void refresh()
    {
        wave.repaint();
        meter.setLevels (proc.outL.exchange (0.0f), proc.outR.exchange (0.0f));
        const int m = (int) a.getRawParameterValue ("mode")->load();
        const bool sl = m == VegaSamplerProcessor::Slices;
        sliceMode.setEnabled (sl); slices.setEnabled (sl && a.getRawParameterValue ("sliceMode")->load() < 0.5f); sens.setEnabled (sl && a.getRawParameterValue ("sliceMode")->load() > 0.5f);
        root.setEnabled (! sl); loopBpm.setEnabled (m == VegaSamplerProcessor::Stretch); choke.setEnabled (true); midiDrag.setVisible (sl);
        // keyboard: show which keys are mapped
        keyState.reset();
        if (const auto* d = proc.sample())
        {
            if (sl) for (int i = 0; i < (int) d->slices.size() && i < 60; ++i) keyState.noteOn (1, VegaSamplerProcessor::firstSliceNote + i, 0.5f);
            else keyState.noteOn (1, (int) a.getRawParameterValue ("root")->load(), 1.0f);
        }
        if (status.isNotEmpty() && juce::Time::getMillisecondCounter() > statusUntil) { status.clear(); repaint(); }
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (VegaColours::background);
        vegaui::drawTopBar (g, W, "SAMPLER  VP-19");
        vegaui::drawBody (g, { 0, 35, W, H - 65 });
        if (status.isNotEmpty()) { g.setColour (VegaColours::gold); g.setFont (VegaFonts::ui (11.0f, true)); g.drawText (status, 420, 0, 520, 34, juce::Justification::centredRight); }
        vegaui::drawSection (g, { 0, 262, 330, 200 }, "SLICING", false);
        vegaui::drawSection (g, { 330, 262, 340, 200 }, "PITCH  /  TIME");
        vegaui::drawSection (g, { 670, 262, W - 670, 200 }, "AMP  /  FILTER");
        g.setFont (VegaFonts::ui (10.0f)); g.setColour (VegaColours::textDim);
        g.drawText ("SLICES: ONE KEY PER SLICE FROM C1   -   KEYS: CLASSIC CHROMATIC SAMPLER   -   STRETCH: LOOP FOLLOWS PROJECT TEMPO, KEYS CHANGE PITCH ONLY", 10, 468, W - 20, 14, juce::Justification::centred);
        vegaui::drawBottomBar (g, { 0, H - 30, W, 30 }, "DRAG SLICE MIDI INTO THE PIANO ROLL TO REBUILD THE LOOP, THEN REARRANGE THE NOTES TO CHOP IT");
    }
    void resized() override
    {
        loadBtn.setBounds (W - 160, 7, 80, 20);
        wave.setBounds (10, 44, W - 100, 212); meter.setBounds (W - 82, 40, 70, 216);
        mode.setBounds (14, 282, 140, 46); sliceMode.setBounds (14, 344, 140, 46); slices.setBounds (164, 282, 80, 46); sens.setBounds (246, 276, 80, 112);
        midiDrag.setBounds (14, 408, 150, 30);
        root.setBounds (338, 276, 80, 112); pitch.setBounds (418, 276, 80, 112); fine.setBounds (498, 276, 80, 112); loopBpm.setBounds (578, 276, 86, 112);
        reverse.setBounds (346, 384, 70, 76);
        attack.setBounds (680, 276, 80, 112); release.setBounds (760, 276, 80, 112); cutoff.setBounds (840, 276, 80, 112); reso.setBounds (920, 276, 80, 112);
        gain.setBounds (1004, 276, 86, 112); loop.setBounds (690, 384, 70, 76); choke.setBounds (770, 384, 70, 76);
        keyboard.setBounds (10, 486, W - 20, 118); keyboard.setKeyWidth ((W - 20) / 43.0f);
    }
private:
    void parameterChanged (const juce::String& id, float) override
    {
        if (id == "sensitivity") return;     // re-sliced on mouse-up
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<Main> (this)] { if (sp != nullptr) { sp->proc.reslice(); sp->wave.invalidate(); } });
    }
    void choose()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
    }
    void load (const juce::File& f)
    {
        status = "Loading " + f.getFileName() + "..."; statusUntil = juce::Time::getMillisecondCounter() + 60000; repaint();
        juce::Component::SafePointer<Main> safe (this);
        proc.loader.addJob ([this, f, safe]
        {
            juce::String err; const bool ok = proc.loadFile (f, err);
            juce::MessageManager::callAsync ([safe, ok, err] { if (safe == nullptr) return; safe->status = ok ? juce::String ("Loaded") : err; safe->statusUntil = juce::Time::getMillisecondCounter() + 3000; safe->wave.invalidate(); safe->repaint(); });
        });
    }
    VegaSamplerProcessor& proc; juce::AudioProcessorValueTreeState& a;
    WaveView wave; MidiDrag midiDrag;
    vegaui::Choice mode, sliceMode, slices; vegaui::Knob sens; vegaui::Knob root; vegaui::Knob pitch, fine, loopBpm, attack, release, cutoff, reso;
    vegaui::Toggle reverse, choke, loop; vegaui::Knob gain;
    vegaui::StereoMeter meter; juce::MidiKeyboardState keyState; juce::MidiKeyboardComponent keyboard;
    juce::TextButton loadBtn; std::unique_ptr<juce::FileChooser> chooser; juce::String status; juce::uint32 statusUntil = 0;
};

VegaSamplerEditor::VegaSamplerEditor (VegaSamplerProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    main = std::make_unique<Main> (p);
    addAndMakeVisible (*main);
    constrainer.setFixedAspectRatio ((double) W / H);
    constrainer.setSizeLimits (W / 2, H / 2, W * 2, H * 2);
    setConstrainer (&constrainer);
    setResizable (true, true);
    const float s = juce::jlimit (0.5f, 2.0f, proc.uiScale.load());
    setSize (juce::roundToInt (W * s), juce::roundToInt (H * s));
    startTimerHz (30);
}
VegaSamplerEditor::~VegaSamplerEditor() { stopTimer(); setConstrainer (nullptr); main.reset(); setLookAndFeel (nullptr); }
void VegaSamplerEditor::timerCallback() { main->refresh(); }
void VegaSamplerEditor::resized()
{
    main->setBounds (0, 0, W, H);
    const float s = (float) getWidth() / (float) W;
    main->setTransform (juce::AffineTransform::scale (s));
    proc.uiScale.store (s);
}
juce::AudioProcessorEditor* VegaSamplerProcessor::createEditor() { return new VegaSamplerEditor (*this); }

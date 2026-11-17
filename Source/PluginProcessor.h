#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

struct SampleData
{
    juce::String name; juce::File file;
    juce::AudioBuffer<float> audio;      // stereo, original sample rate
    double rate = 44100;
    std::vector<int> slices;             // slice start positions (first is 0)
    double guessedBpm = 120;
    int sliceEnd (int i) const { return i + 1 < (int) slices.size() ? slices[(size_t) i + 1] : audio.getNumSamples(); }
};

class VegaSamplerProcessor : public juce::AudioProcessor
{
public:
    VegaSamplerProcessor();
    ~VegaSamplerProcessor() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Vega Productions Sampler"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    juce::AudioProcessorValueTreeState apvts;
    enum Mode { Slices, Keys, Stretch };
    static constexpr int firstSliceNote = 36;

    bool loadFile (const juce::File&, juce::String& error);       // any thread
    void reslice();                                               // message thread: grid / transients from params
    const SampleData* sample() const { return current.load(); }
    static std::vector<int> detectTransients (const juce::AudioBuffer<float>&, double rate, float sensitivity);
    juce::File exportMidi (const juce::File& dest = {}) const;
    void audition (int note) { auditionNote.store (note); }

    std::array<std::atomic<float>, 16> voicePos {};             // UI: play positions (samples) of active voices, -1 = idle
    std::atomic<int> lastSlice { -1 };
    std::atomic<float> outL { 0 }, outR { 0 }, uiScale { 1.0f };
    juce::ThreadPool loader { 1 };
    double hostBpm = 120;

private:
    struct Voice
    {
        bool active = false; int note = 0; float vel = 1;
        double pos = 0, inc = 1; int start = 0, end = 0; bool reverse = false, loop = false;
        juce::ADSR adsr; float ic1[2] {}, ic2[2] {};
        // stretch (granular)
        double readHead = 0; int hopCount = 0; double grainPos[2] {}; int grainAge[2] {}; int nextGrain = 0;
        const SampleData* data = nullptr;
    };
    void startVoice (int note, float vel, const SampleData& d);
    float readSample (const SampleData& d, int ch, double pos) const;
    double sr = 48000;
    std::array<Voice, 16> voices;
    std::atomic<SampleData*> current { nullptr };
    std::vector<std::unique_ptr<SampleData>> retired;
    juce::CriticalSection loadLock;
    std::atomic<int> auditionNote { -1 }; int auditionHeld = -1, auditionLeft = 0;
    void trimRetired() { while (retired.size() > 3) retired.erase (retired.begin()); }   // audio thread only ever holds the newest one or two
    std::atomic<float>* p[16] {};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VegaSamplerProcessor)
};

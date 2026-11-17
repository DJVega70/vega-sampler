#pragma once
#include "PluginProcessor.h"
#include "VegaUI.h"

class VegaSamplerEditor : public juce::AudioProcessorEditor, public juce::DragAndDropContainer, private juce::Timer
{
public:
    explicit VegaSamplerEditor (VegaSamplerProcessor&);
    ~VegaSamplerEditor() override;
    void paint (juce::Graphics&) override {}
    void resized() override;
    static constexpr int W = 1100, H = 640;
    class Main;
private:
    void timerCallback() override;
    VegaSamplerProcessor& proc;
    VegaLookAndFeel lnf;
    std::unique_ptr<Main> main;
    juce::ComponentBoundsConstrainer constrainer;
};

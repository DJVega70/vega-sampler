cmake_minimum_required(VERSION 3.22)
project(VegaSampler VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.9 GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(VEGA_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND VEGA_FORMATS AU)
endif()

juce_add_plugin(VegaSampler
    COMPANY_NAME "Vega Productions"
    BUNDLE_ID com.vegaproductions.sampler
    PLUGIN_MANUFACTURER_CODE Vgap
    PLUGIN_CODE Vsm1
    FORMATS ${VEGA_FORMATS}
    PRODUCT_NAME "Vega Productions Sampler"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Instrument Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(VegaSampler PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(VegaSampler PUBLIC JUCE_USE_MP3AUDIOFORMAT=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(VegaSampler
    PRIVATE juce::juce_audio_utils juce::juce_dsp juce::juce_audio_formats
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

option(VEGA_TESTS "Build offline test" OFF)
if(VEGA_TESTS)
    juce_add_console_app(VegaSamplerTest PRODUCT_NAME "VegaSamplerTest")
    target_sources(VegaSamplerTest PRIVATE tests/SamplerTest.cpp)
    target_include_directories(VegaSamplerTest PRIVATE $<TARGET_PROPERTY:VegaSampler,INCLUDE_DIRECTORIES>)
    target_compile_definitions(VegaSamplerTest PRIVATE $<TARGET_PROPERTY:VegaSampler,COMPILE_DEFINITIONS>)
    target_link_libraries(VegaSamplerTest PRIVATE VegaSampler)
endif()

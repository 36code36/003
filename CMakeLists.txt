cmake_minimum_required(VERSION 3.22)
project(MultiKeys VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(MultiKeys
    COMPANY_NAME "xxxvi"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    PLUGIN_MANUFACTURER_CODE Xxvi
    PLUGIN_CODE Mkey
    FORMATS VST3
    PRODUCT_NAME "MultiKeys"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(MultiKeys PRIVATE Source/PluginProcessor.cpp Source/PluginProcessor.h)

target_compile_definitions(MultiKeys PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MultiKeys
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags)

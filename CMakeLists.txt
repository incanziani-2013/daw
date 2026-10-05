cmake_minimum_required(VERSION 3.22)
project(AQDAW VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(AQDAW
    PRODUCT_NAME "AQDAW"
    COMPANY_NAME "AQ")

target_sources(AQDAW PRIVATE
    Source/Main.cpp
    Source/MainComponent.h)

target_compile_definitions(AQDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

if(MSVC)
    target_compile_options(AQDAW PRIVATE /utf-8)
endif()

target_link_libraries(AQDAW PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_basics
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

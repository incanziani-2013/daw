#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// AQDAW v0.1 - Etapa 1 + 2: motor de audio, transporte, Channel Rack con
// secuenciador de pasos y carga de drumkits (carpeta o arrastrar y soltar).
class MainComponent : public juce::AudioAppComponent,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer
{
public:
    static constexpr int numChannels = 8;
    static constexpr int numSteps = 16;
    static constexpr int rowH = 44;

    MainComponent()
    {
        formatManager.registerBasicFormats();

        auto& lf = getLookAndFeel();
        const juce::Colour green(0xff8fe03f);
        lf.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffd0d4da));
        lf.setColour(juce::TextButton::textColourOnId, juce::Colour(0xffd0d4da));
        lf.setColour(juce::Slider::trackColourId, green);
        lf.setColour(juce::Slider::thumbColourId, juce::Colour(0xffe8ecf1));
        lf.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff2a2f37));
        lf.setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xffd0d4da));
        lf.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0x00000000));
        lf.setColour(juce::Label::textColourId, juce::Colour(0xffd0d4da));

        const char* names[numChannels] = { "Kick", "Snare", "Clap", "Hi-Hat",
                                           "Open Hat", "808", "Perc", "FX" };

        for (int ch = 0; ch < numChannels; ++ch)
        {
            channels[ch].name = names[ch];
            setSampleBuffer(ch, makeDefaultSample(ch, 44100.0), 44100.0);

            auto& nb = nameButtons[ch];
            nb.setButtonText(names[ch]);
            nb.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a404a));
            nb.onClick = [this, ch] { chooseSample(ch); };
            addAndMakeVisible(nb);

            auto& sl = volSliders[ch];
            sl.setSliderStyle(juce::Slider::LinearHorizontal);
            sl.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            sl.setRange(0.0, 1.0, 0.01);
            sl.setValue(0.8, juce::dontSendNotification);
            sl.onValueChange = [this, ch]
            {
                channels[ch].volume = (float) volSliders[ch].getValue();
            };
            addAndMakeVisible(sl);

            for (int s = 0; s < numSteps; ++s)
            {
                auto& b = stepButtons[ch][s];
                const bool alt = ((s / 4) % 2) == 1;
                b.setClickingTogglesState(true);
                b.setColour(juce::TextButton::buttonColourId,
                            alt ? juce::Colour(0xff3a404a) : juce::Colour(0xff2a2f37));
                b.setColour(juce::TextButton::buttonOnColourId, green);
                b.onClick = [this, ch, s]
                {
                    channels[ch].steps[s] = stepButtons[ch][s].getToggleState();
                };
                addAndMakeVisible(b);
            }
        }

        // Patron de ejemplo (trap a media velocidad)
        for (int s : { 0, 6, 10 })                    setStep(0, s, true);
        for (int s : { 8 })                           { setStep(1, s, true); setStep(2, s, true); }
        for (int s = 0; s < numSteps; s += 2)         setStep(3, s, true);
        for (int s : { 0, 10 })                       setStep(5, s, true);

        playButton.setButtonText("Play");
        playButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3d6b1f));
        playButton.onClick = [this] { resetRequested = true; playing = true; };
        addAndMakeVisible(playButton);

        stopButton.setButtonText("Stop");
        stopButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff6b2a1f));
        stopButton.onClick = [this] { playing = false; visualStep = -1; };
        addAndMakeVisible(stopButton);

        bpmLabel.setText("BPM", juce::dontSendNotification);
        addAndMakeVisible(bpmLabel);
        bpmSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        bpmSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 24);
        bpmSlider.setRange(60.0, 220.0, 1.0);
        bpmSlider.setValue(140.0, juce::dontSendNotification);
        bpmSlider.onValueChange = [this] { bpm = bpmSlider.getValue(); };
        addAndMakeVisible(bpmSlider);

        kitButton.setButtonText("Cargar kit (carpeta)");
        kitButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a404a));
        kitButton.onClick = [this] { chooseKitFolder(); };
        addAndMakeVisible(kitButton);

        statusLabel.setText("Arrastra una carpeta de kit o samples sueltos a la ventana",
                            juce::dontSendNotification);
        statusLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(statusLabel);

        setSize(1000, 480);
        setAudioChannels(0, 2);
        startTimerHz(30);
    }

    ~MainComponent() override
    {
        stopTimer();
        shutdownAudio();
    }

    // ---------------------------------------------------------------- audio
    void prepareToPlay(int, double sampleRate) override
    {
        deviceRate = sampleRate;
        sampleCounter = 0.0;
    }

    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        const juce::CriticalSection::ScopedTryLockType sl(lock);
        if (! sl.isLocked())
            return;

        auto* buf = info.buffer;
        const int n = info.numSamples;
        float* outL = buf->getWritePointer(0, info.startSample);
        float* outR = buf->getNumChannels() > 1 ? buf->getWritePointer(1, info.startSample) : nullptr;

        const bool isPlaying = playing.load();
        if (resetRequested.exchange(false))
        {
            currentStep = 0;
            sampleCounter = 0.0;
        }

        const double stepLen = deviceRate * 60.0 / juce::jmax(1.0, bpm.load()) / 4.0;

        for (int i = 0; i < n; ++i)
        {
            if (isPlaying)
            {
                if (sampleCounter <= 0.0)
                {
                    for (auto& c : channels)
                        if (c.steps[currentStep].load() && c.sample.getNumSamples() > 0)
                            c.pos = 0.0;

                    visualStep = currentStep;
                    currentStep = (currentStep + 1) % numSteps;
                    sampleCounter += stepLen;
                }
                sampleCounter -= 1.0;
            }

            float l = 0.0f, r = 0.0f;
            for (auto& c : channels)
            {
                if (c.pos < 0.0)
                    continue;

                const int len = c.sample.getNumSamples();
                const int i0 = (int) c.pos;
                if (i0 >= len - 1)
                {
                    c.pos = -1.0;
                    continue;
                }

                const float frac = (float) (c.pos - i0);
                const float* d0 = c.sample.getReadPointer(0);
                const float* d1 = c.sample.getReadPointer(c.sample.getNumChannels() > 1 ? 1 : 0);
                const float vol = c.volume.load();

                l += (d0[i0] + frac * (d0[i0 + 1] - d0[i0])) * vol;
                r += (d1[i0] + frac * (d1[i0 + 1] - d1[i0])) * vol;
                c.pos += c.sampleRate / deviceRate;
            }

            outL[i] = juce::jlimit(-1.0f, 1.0f, l * 0.6f);
            if (outR != nullptr)
                outR[i] = juce::jlimit(-1.0f, 1.0f, r * 0.6f);
        }
    }

    // ------------------------------------------------------------------- UI
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff15181c));
        g.setColour(juce::Colour(0xff1f2329));
        g.fillRoundedRectangle(rackBounds.toFloat(), 6.0f);
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        if (! playing.load())
            return;
        const int s = visualStep.load();
        if (s < 0 || s >= numSteps)
            return;

        auto r = stepButtons[0][s].getBounds().getUnion(stepButtons[numChannels - 1][s].getBounds());
        g.setColour(juce::Colours::white.withAlpha(0.14f));
        g.fillRect(r.expanded(1, 2));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12);

        auto top = area.removeFromTop(40);
        playButton.setBounds(top.removeFromLeft(80));
        top.removeFromLeft(8);
        stopButton.setBounds(top.removeFromLeft(80));
        top.removeFromLeft(16);
        bpmLabel.setBounds(top.removeFromLeft(40));
        bpmSlider.setBounds(top.removeFromLeft(200));
        top.removeFromLeft(16);
        kitButton.setBounds(top.removeFromLeft(160));
        top.removeFromLeft(12);
        statusLabel.setBounds(top);

        area.removeFromTop(12);
        rackBounds = area.expanded(6);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto row = area.removeFromTop(rowH);
            nameButtons[ch].setBounds(row.removeFromLeft(110).reduced(2));
            volSliders[ch].setBounds(row.removeFromLeft(100));
            row.removeFromLeft(10);
            const int stepW = row.getWidth() / numSteps;
            for (int s = 0; s < numSteps; ++s)
                stepButtons[ch][s].setBounds(row.removeFromLeft(stepW).reduced(2));
        }
    }

    // ---------------------------------------------------------- drag & drop
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }

    void filesDropped(const juce::StringArray& files, int, int y) override
    {
        if (files.isEmpty())
            return;

        juce::File first(files[0]);
        if (first.isDirectory())
        {
            loadKit(first);
            return;
        }

        int row = juce::jlimit(0, numChannels - 1, (y - rackBounds.getY()) / rowH);
        for (int i = 0; i < files.size(); ++i)
            loadSample((row + i) % numChannels, juce::File(files[i]));
    }

private:
    struct Channel
    {
        Channel() { for (auto& s : steps) s.store(false); }

        juce::String name;
        juce::AudioBuffer<float> sample;
        double sampleRate = 44100.0;
        std::array<std::atomic<bool>, numSteps> steps;
        std::atomic<float> volume { 0.8f };
        double pos = -1.0;
    };

    struct Rule
    {
        juce::StringArray include, exclude;
    };

    void timerCallback() override
    {
        const int s = playing.load() ? visualStep.load() : -1;
        if (s != lastShownStep)
        {
            lastShownStep = s;
            repaint();
        }
    }

    void setStep(int ch, int s, bool on)
    {
        channels[ch].steps[s] = on;
        stepButtons[ch][s].setToggleState(on, juce::dontSendNotification);
    }

    void setSampleBuffer(int ch, juce::AudioBuffer<float> buffer, double sr)
    {
        const juce::CriticalSection::ScopedLockType sl(lock);
        channels[ch].sample = std::move(buffer);
        channels[ch].sampleRate = sr;
        channels[ch].pos = -1.0;
    }

    bool loadSample(int ch, const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return false;

        const int maxLen = (int) (reader->sampleRate * 30.0);
        const int len = (int) juce::jmin<juce::int64>(reader->lengthInSamples, maxLen);
        const int chans = juce::jmin(2, (int) reader->numChannels);

        juce::AudioBuffer<float> buf(chans, len);
        reader->read(&buf, 0, len, 0, true, chans > 1);

        setSampleBuffer(ch, std::move(buf), reader->sampleRate);
        nameButtons[ch].setButtonText(file.getFileNameWithoutExtension().substring(0, 14));
        return true;
    }

    void chooseSample(int ch)
    {
        chooser = std::make_unique<juce::FileChooser>("Elegi un sample", juce::File(),
                                                      "*.wav;*.flac;*.ogg;*.aif;*.aiff");
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [this, ch](const juce::FileChooser& fc)
                             {
                                 auto f = fc.getResult();
                                 if (f.existsAsFile())
                                     loadSample(ch, f);
                             });
    }

    void chooseKitFolder()
    {
        chooser = std::make_unique<juce::FileChooser>("Elegi la carpeta del drumkit", juce::File());
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser& fc)
                             {
                                 auto f = fc.getResult();
                                 if (f.isDirectory())
                                     loadKit(f);
                             });
    }

    // Escanea la carpeta (con subcarpetas) y asigna sonidos a los canales
    // segun palabras clave del nombre: kick, snare, clap, hat, open, 808, perc.
    void loadKit(const juce::File& dir)
    {
        juce::Array<juce::File> found = dir.findChildFiles(juce::File::findFiles, true);
        std::vector<juce::File> audio;
        for (auto& f : found)
            if (f.hasFileExtension("wav;flac;ogg;aif;aiff"))
                audio.push_back(f);

        if (audio.empty())
        {
            statusLabel.setText("No encontre audios en esa carpeta (WAV, FLAC, OGG, AIFF)",
                                juce::dontSendNotification);
            return;
        }

        std::sort(audio.begin(), audio.end(), [](const juce::File& a, const juce::File& b)
                  { return a.getFullPathName().compareIgnoreCase(b.getFullPathName()) < 0; });

        const std::vector<Rule> rules = {
            { { "kick", "kik" }, {} },
            { { "snare", "snr" }, {} },
            { { "clap", "clp" }, {} },
            { { "hat", "hh" }, { "open", "ohh", "_oh" } },
            { { "open", "ohh", "_oh" }, {} },
            { { "808", "bass" }, {} },
            { { "perc", "rim", "tom", "shaker", "cowbell" }, {} },
            { {}, {} } // cualquiera que sobre
        };

        std::vector<bool> used(audio.size(), false);
        int loaded = 0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto& rule = rules[(size_t) ch];
            for (size_t i = 0; i < audio.size(); ++i)
            {
                if (used[i])
                    continue;

                const auto name = audio[i].getFileNameWithoutExtension().toLowerCase();
                bool ok = rule.include.isEmpty();
                for (auto& k : rule.include)
                    if (name.contains(k)) ok = true;
                for (auto& k : rule.exclude)
                    if (name.contains(k)) ok = false;

                if (ok && loadSample(ch, audio[i]))
                {
                    used[i] = true;
                    ++loaded;
                    break;
                }
            }
        }

        statusLabel.setText("Kit: " + dir.getFileName() + " (" + juce::String(loaded) + " sonidos)",
                            juce::dontSendNotification);
    }

    // Sonidos sinteticos para que suene algo desde el primer momento
    static juce::AudioBuffer<float> makeDefaultSample(int ch, double sr)
    {
        constexpr double twoPi = juce::MathConstants<double>::twoPi;
        const double lengths[numChannels] = { 0.45, 0.25, 0.22, 0.10, 0.45, 1.3, 0.2, 0.3 };
        const int len = (int) (lengths[ch] * sr);

        juce::AudioBuffer<float> b(1, len);
        auto* d = b.getWritePointer(0);
        juce::Random rng(1234 + ch);
        double phase = 0.0;
        float prev = 0.0f;

        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            const float noise = rng.nextFloat() * 2.0f - 1.0f;
            float v = 0.0f;

            switch (ch)
            {
                case 0:
                {
                    const double f = 45.0 + 130.0 * std::exp(-t * 28.0);
                    phase += twoPi * f / sr;
                    v = (float) (std::sin(phase) * std::exp(-t * 7.0));
                    break;
                }
                case 1:
                    v = (float) (noise * 0.7 * std::exp(-t * 22.0)
                                 + std::sin(twoPi * 190.0 * t) * 0.5 * std::exp(-t * 28.0));
                    break;
                case 2:
                {
                    const double gate = t < 0.03 ? 0.5 + 0.5 * std::sin(t * twoPi * 150.0) : 1.0;
                    v = (float) (noise * std::exp(-t * 20.0) * gate);
                    break;
                }
                case 3:
                case 4:
                {
                    const float hp = noise - prev;
                    prev = noise;
                    v = (float) (hp * 0.5 * std::exp(-t * (ch == 3 ? 70.0 : 9.0)));
                    break;
                }
                case 5:
                {
                    const double f = 42.0 + 20.0 * std::exp(-t * 6.0);
                    phase += twoPi * f / sr;
                    v = (float) std::tanh(2.0 * std::sin(phase) * std::exp(-t * 2.2));
                    break;
                }
                case 6:
                {
                    const double f = 330.0 - 120.0 * t * 5.0;
                    phase += twoPi * f / sr;
                    v = (float) (std::sin(phase) * std::exp(-t * 30.0));
                    break;
                }
                default:
                    v = (float) (std::sin(twoPi * 880.0 * t) * std::exp(-t * 14.0) * 0.6);
                    break;
            }
            d[i] = v * 0.9f;
        }

        for (int k = 0; k < 64 && k < len; ++k)
            d[len - 1 - k] *= (float) k / 64.0f;

        return b;
    }

    // --- estado compartido con el hilo de audio
    juce::CriticalSection lock;
    std::array<Channel, numChannels> channels;
    std::atomic<bool> playing { false };
    std::atomic<bool> resetRequested { false };
    std::atomic<double> bpm { 140.0 };
    std::atomic<int> visualStep { -1 };

    // --- solo hilo de audio
    double deviceRate = 44100.0;
    double sampleCounter = 0.0;
    int currentStep = 0;

    // --- UI
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TextButton playButton, stopButton, kitButton;
    juce::Label bpmLabel, statusLabel;
    juce::Slider bpmSlider;
    std::array<juce::TextButton, numChannels> nameButtons;
    std::array<juce::Slider, numChannels> volSliders;
    std::array<std::array<juce::TextButton, numSteps>, numChannels> stepButtons;
    juce::Rectangle<int> rackBounds;
    int lastShownStep = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

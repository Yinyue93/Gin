namespace
{
    const char* playSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path d="M8 5 L19 12 L8 19 Z" fill="currentColor" stroke="currentColor" stroke-width="1" stroke-linejoin="round"/></svg>)";
    const char* stopSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2" ry="2" fill="currentColor"/></svg>)";
    const char* menuSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect x="4" y="6" width="16" height="2" rx="1" fill="currentColor"/><rect x="4" y="11" width="16" height="2" rx="1" fill="currentColor"/><rect x="4" y="16" width="16" height="2" rx="1" fill="currentColor"/></svg>)";
    const char* recordSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><circle cx="12" cy="12" r="8" fill="currentColor"/></svg>)";
    // Circle with T cutout (Triggered)
    const char* triggerSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill-rule="evenodd" d="M12 4a8 8 0 1 0 0 16 8 8 0 0 0 0-16zM8 8h8v2h-3v7h-2v-7H8V8z" fill="currentColor"/></svg>)";
    // Circle with R cutout (Retrospective)
    // Arrow down into a tray (Export)
    const char* exportSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path d="M11 4h2v8.6l3.3-3.3 1.4 1.4L12 16.4l-5.7-5.7 1.4-1.4 3.3 3.3V4zM5 18h14v2H5v-2z" fill="currentColor"/></svg>)";
    const char* retroSVG = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill-rule="evenodd" d="M12 4a8 8 0 1 0 0 16 8 8 0 0 0 0-16zM9 7h5a3 3 0 0 1 .5 5.96L17 17h-2.5l-2.3-4H11v4H9V7zm2 2v2h3a1 1 0 1 0 0-2h-3z" fill="currentColor"/></svg>)";
}

//==============================================================================
// Renders the MIDI file on a thread, with a progress window
class SideBarComponent::ExportJob : public juce::ThreadWithProgressWindow
{
public:
    ExportJob (std::unique_ptr<juce::AudioProcessor> p, juce::File midi, juce::File wav,
               double rate, MidiFileExporter::Options o, juce::Component* centre,
               std::function<void (const MidiFileExporter::Result&)> done)
        : juce::ThreadWithProgressWindow ("Exporting " + wav.getFileName(), true, true, 10000, {}, centre),
          processor (std::move (p)), midiFile (std::move (midi)), wavFile (std::move (wav)),
          sampleRate (rate), options (o), onDone (std::move (done))
    {
    }

    void run() override
    {
        result = MidiFileExporter::render (*processor, midiFile, wavFile, sampleRate, options, [this] (double progress)
        {
            setProgress (progress);
            return ! threadShouldExit();
        });
    }

    void threadComplete (bool userPressedCancel) override
    {
        if (userPressedCancel)
            result.cancelled = true;

        if (onDone)
            onDone (result);
    }

    const juce::File& getWavFile() const { return wavFile; }

private:
    std::unique_ptr<juce::AudioProcessor> processor;
    juce::File midiFile, wavFile;
    double sampleRate;
    MidiFileExporter::Options options;
    std::function<void (const MidiFileExporter::Result&)> onDone;
    MidiFileExporter::Result result;
};

SideBarComponent::SideBarComponent (StandaloneFilterWindow& f)
  : filterWindow (f),
    player (f.pluginHolder->player),
    midiPlay ("play", playSVG, 4),
    midiStop ("stop", stopSVG, 4),
    midiMenu ("menu", menuSVG, 4),
    midiExport ("export", exportSVG, 4),
    samplePlay ("play", playSVG, 4),
    sampleStop ("stop", stopSVG, 4),
    sampleMenu ("menu", menuSVG, 4),
    recordNormal ("record", recordSVG, 4),
    recordTriggered ("trigger", triggerSVG, 4),
    recordRetrospective ("retro", retroSVG, 4),
    recordStop ("stop", stopSVG, 4),
    recordMenu ("menu", menuSVG, 4)
{
    setLookAndFeel (&lf);

    if (filterWindow.pluginHolder->processor->acceptsMidi())
    {
        addAndMakeVisible (midiHeader);

        midi.setColour (MidiFileComponent::backgroundColourId, findColour (GinLookAndFeel::displayColourId));
        midi.setColour (MidiFileComponent::noteColourId, findColour (GinLookAndFeel::accentColourId));
        midi.setColour (MidiFileComponent::playheadColourId, findColour (GinLookAndFeel::whiteColourId));
        midi.setColour (MidiFileComponent::outlineColourId, findColour (GinLookAndFeel::grey45ColourId));
        midi.setColour (MidiFileComponent::dropHighlightColourId, findColour (GinLookAndFeel::accentColourId).withAlpha (0.2f));

        midi.onFileDropped = [this]
        {
            addToMidiMRU (player.midiPlayer.getLoadedFile());
            updatePlayStopButtons();
            saveMidiFilePath();
        };
        midi.onPlaybackStateChanged = [this] { updatePlayStopButtons(); };

        restoreMidiFilePath();

        addAndMakeVisible (midi);

        midiPlay.setTooltip ("Play MIDI file");
        midiPlay.onClick = [this]
        {
            player.midiPlayer.play();
            updatePlayStopButtons();
        };
        addAndMakeVisible (midiPlay);

        midiStop.setTooltip ("Stop MIDI playback");
        midiStop.onClick = [this]
        {
            player.midiPlayer.stop();
            player.midiPlayer.setPlayheadPosition (0.0);
            updatePlayStopButtons();
        };
        addAndMakeVisible (midiStop);

        midiMenu.setTooltip ("MIDI options");
        midiMenu.onClick = [this] { showMidiMenu(); };
        addAndMakeVisible (midiMenu);

        midiExport.setTooltip ("Export MIDI file as WAV");
        midiExport.onClick = [this] { showExportDialog(); };
        addAndMakeVisible (midiExport);

        midiFileName.setFont (juce::FontOptions (11.0f));
        midiFileName.setColour (juce::Label::textColourId, findColour (GinLookAndFeel::whiteColourId));
        midiFileName.setMinimumHorizontalScale (1.0f);
        midiFileName.setBorderSize ({});
        addAndMakeVisible (midiFileName);

        // Restore loop setting
        if (auto* props = filterWindow.pluginHolder->settings.get())
            player.midiPlayer.setLooping (props->getBoolValue ("midiLoop", true));

        updatePlayStopButtons();
    }

    // Check if processor has audio input
    if (filterWindow.pluginHolder->processor->getTotalNumInputChannels() > 0)
    {
        addAndMakeVisible (sampleHeader);

        sample.setBackgroundColour (findColour (GinLookAndFeel::displayColourId));
        sample.setLineColour (findColour (GinLookAndFeel::accentColourId));

        sample.onFileDropped = [this]
        {
            addToSampleMRU (player.samplePlayer.getLoadedFile());
            updateSamplePlayStopButtons();
            saveSampleFilePath();
        };
        sample.onPlaybackStateChanged = [this] { updateSamplePlayStopButtons(); };

        restoreSampleFilePath();

        addAndMakeVisible (sample);

        samplePlay.setTooltip ("Play audio sample");
        samplePlay.onClick = [this]
        {
            player.samplePlayer.play();
            updateSamplePlayStopButtons();
        };
        addAndMakeVisible (samplePlay);

        sampleStop.setTooltip ("Stop sample playback");
        sampleStop.onClick = [this]
        {
            player.samplePlayer.stop();
            player.samplePlayer.setPosition (0.0);
            updateSamplePlayStopButtons();
        };
        addAndMakeVisible (sampleStop);

        sampleMenu.setTooltip ("Sample options");
        sampleMenu.onClick = [this] { showSampleMenu(); };
        addAndMakeVisible (sampleMenu);

        // Restore loop setting
        if (auto* props = filterWindow.pluginHolder->settings.get())
            player.samplePlayer.setLooping (props->getBoolValue ("sampleLoop", true));

        updateSamplePlayStopButtons();
    }

    {
        addAndMakeVisible (scopeHeader);

        auto accent = findColour (GinLookAndFeel::accentColourId);

        scope.setColour (TriggeredScope::lineColourId, findColour (GinLookAndFeel::grey45ColourId));

        for (int i = 0; i < 8; ++i)
        {
            auto c = accent.withRotatedHue (-0.05f + i * 0.1f);
            scope.setColour (TriggeredScope::traceColourId + i, c);
            scope.setColour (TriggeredScope::envelopeColourId + i, c.withAlpha (0.5f));
        }

        addAndMakeVisible (scope);
    }

    {
        addAndMakeVisible (spectrumHeader);

        auto accent = findColour (GinLookAndFeel::accentColourId);

        spectrum.setColour (SpectrumAnalyzer::lineColourId, findColour (GinLookAndFeel::grey45ColourId));

        for (int i = 0; i < 8; ++i)
        {
            auto c = accent.withRotatedHue (-0.05f + i * 0.1f);
            spectrum.setColour (SpectrumAnalyzer::traceColourId + i, c);
            spectrum.setColour (SpectrumAnalyzer::envelopeColourId + i, c.withAlpha (0.5f));
        }

        addAndMakeVisible (spectrum);
    }

    // Only show XY scope if stereo output
    if (filterWindow.pluginHolder->processor->getTotalNumOutputChannels() >= 2)
    {
        addAndMakeVisible (xyHeader);

        auto accent = findColour (GinLookAndFeel::accentColourId);

        xyScope.setColour (XYScope::lineColourId, findColour (GinLookAndFeel::grey45ColourId));
        xyScope.setColour (XYScope::traceColourId, accent);

        addAndMakeVisible (xyScope);
    }

    {
        addAndMakeVisible (recordHeader);

        recordBuffer.setBackgroundColour (findColour (GinLookAndFeel::displayColourId));
        recordBuffer.setLineColour (findColour (GinLookAndFeel::accentColourId));
        addAndMakeVisible (recordBuffer);

        recordNormal.setTooltip ("Start/stop recording");
        recordNormal.onClick = [this]
        {
            if (player.audioRecorder.isRecording())
                player.audioRecorder.stopRecording();
            else
                player.audioRecorder.startRecording();
            updateRecordButtons();
        };
        addAndMakeVisible (recordNormal);

        recordTriggered.setTooltip ("Triggered recording (starts when audio > 0dB)");
        recordTriggered.onClick = [this]
        {
            if (player.audioRecorder.isRecording() || player.audioRecorder.isWaitingForTrigger())
                player.audioRecorder.stopRecording();
            else
                player.audioRecorder.startTriggeredRecording();
            updateRecordButtons();
        };
        addAndMakeVisible (recordTriggered);

        recordRetrospective.setTooltip ("Save last 30 seconds to file");
        recordRetrospective.onClick = [this]
        {
            player.audioRecorder.saveRetrospective();
        };
        addAndMakeVisible (recordRetrospective);

        recordStop.setTooltip ("Stop recording");
        recordStop.onClick = [this]
        {
            player.audioRecorder.stopRecording();
            updateRecordButtons();
        };
        addAndMakeVisible (recordStop);

        recordMenu.setTooltip ("Recording options");
        recordMenu.onClick = [this] { showRecordMenu(); };
        addAndMakeVisible (recordMenu);

        player.audioRecorder.onRecordingStarted = [this] { updateRecordButtons(); updateRecordButtonColours(); };
        player.audioRecorder.onRecordingStopped = [this] { updateRecordButtons(); updateRecordButtonColours(); };

        startTimerHz (6); // 6 Hz for 3 Hz flash (on/off)

        // Restore destination folder
        if (auto* props = filterWindow.pluginHolder->settings.get())
        {
            auto path = props->getValue ("recordDestFolder", "");
            if (path.isNotEmpty())
            {
                juce::File folder (path);
                if (folder.isDirectory())
                    player.audioRecorder.setDestFolder (folder);
            }
        }

        updateRecordButtons();
    }

    {
        addAndMakeVisible (outputHeader);

        outputGain.setSliderStyle (juce::Slider::LinearHorizontal);
        outputGain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
        outputGain.setRange (-60.0, 12.0, 0.1);
        outputGain.setValue (0.0);
        outputGain.setTextValueSuffix (" dB");
        outputGain.setDoubleClickReturnValue (true, 0.0);
        outputGain.setTooltip ("Output gain");
        outputGain.onValueChange = [this]
        {
            auto dB = outputGain.getValue();
            auto gain = juce::Decibels::decibelsToGain (float (dB));
            player.setOutputGain (gain);

            if (auto* props = filterWindow.pluginHolder->settings.get())
                props->setValue ("outputGain", dB);
        };
        addAndMakeVisible (outputGain);

        // Restore output gain
        if (auto* props = filterWindow.pluginHolder->settings.get())
        {
            auto dB = props->getDoubleValue ("outputGain", 0.0);
            outputGain.setValue (dB, juce::dontSendNotification);
            player.setOutputGain (float (juce::Decibels::decibelsToGain (dB)));
        }
    }

    // Track initial channel counts
    lastInputChannels = filterWindow.pluginHolder->processor->getTotalNumInputChannels();
    lastOutputChannels = filterWindow.pluginHolder->processor->getTotalNumOutputChannels();

    // Listen for audio device changes
    filterWindow.getDeviceManager().addChangeListener (this);

    // Keys the focused component doesn't use bubble up to the window
    filterWindow.addKeyListener (this);

    // Reparent the controls without changing their individual visibility.
    while (getNumChildComponents() > 0)
        content.addChildComponent (getChildComponent (0));

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

SideBarComponent::~SideBarComponent()
{
    filterWindow.getDeviceManager().removeChangeListener (this);
    filterWindow.removeKeyListener (this);

    stopTimer();
    setLookAndFeel (nullptr);
}

bool SideBarComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    if (! filterWindow.pluginHolder->processor->acceptsMidi())
        return false;

    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0))
    {
        browseForMidiFile();
        return true;
    }

    if (key == juce::KeyPress (juce::KeyPress::spaceKey))
    {
        if (player.midiPlayer.isPlaying())
            midiStop.triggerClick();
        else if (player.midiPlayer.hasFileLoaded())
            midiPlay.triggerClick();

        return true;
    }

    return false;
}

void SideBarComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateComponentVisibility();
}

void SideBarComponent::updateComponentVisibility()
{
    auto* processor = filterWindow.pluginHolder->processor.get();
    int inputChannels = processor->getTotalNumInputChannels();
    int outputChannels = processor->getTotalNumOutputChannels();

    if (inputChannels == lastInputChannels && outputChannels == lastOutputChannels)
        return;

    lastInputChannels = inputChannels;
    lastOutputChannels = outputChannels;

    // Update sample section visibility based on input channels
    bool showSample = inputChannels > 0 && processor->acceptsMidi() == false;
    sampleHeader.setVisible (showSample);
    sample.setVisible (showSample);
    samplePlay.setVisible (showSample);
    sampleStop.setVisible (showSample);
    sampleMenu.setVisible (showSample);

    // Update XY scope visibility based on output channels
    bool showXY = outputChannels >= 2;
    xyHeader.setVisible (showXY);
    xyScope.setVisible (showXY);

    resized();
}

void SideBarComponent::timerCallback()
{
    flashState = ! flashState;
    updateRecordButtonColours();
}

void SideBarComponent::updatePlayStopButtons()
{
    const bool isPlaying = player.midiPlayer.isPlaying();
    midiPlay.setEnabled (! isPlaying && player.midiPlayer.hasFileLoaded());
    midiStop.setEnabled (isPlaying || player.midiPlayer.getPlayheadPosition() > 0.0);
    midiExport.setEnabled (player.midiPlayer.hasFileLoaded() && exportJob == nullptr);

    const auto file = player.midiPlayer.hasFileLoaded() ? player.midiPlayer.getLoadedFile() : juce::File();
    midiFileName.setText (file.getFileNameWithoutExtension(), juce::dontSendNotification);
    midiFileName.setTooltip (file.getFullPathName());
}

void SideBarComponent::showExportDialog()
{
    if (! player.midiPlayer.hasFileLoaded() || exportJob != nullptr)
        return;

    auto* props = filterWindow.pluginHolder->settings.get();

    auto normalize = std::make_shared<juce::ToggleButton> ("Normalize (peak at -1 dBTP)");
    normalize->setToggleState (props != nullptr && props->getBoolValue ("midiExportNormalize", false), juce::dontSendNotification);
    normalize->setSize (260, 24);

    auto* window = new juce::AlertWindow ("Export WAV",
                                          "Renders " + player.midiPlayer.getLoadedFile().getFileName()
                                          + " with the current settings. It never goes over -1 dBTP: without normalize,"
                                            " a render that would is turned down to fit.",
                                          juce::MessageBoxIconType::NoIcon, this);
    window->addCustomComponent (normalize.get());
    window->addButton ("Export...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // The window doesn't own the toggle: the callback keeps it alive
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<SideBarComponent> (this), normalize] (int button)
    {
        if (safe == nullptr || button != 1)
            return;

        const bool shouldNormalize = normalize->getToggleState();
        auto* settings = safe->filterWindow.pluginHolder->settings.get();

        if (settings != nullptr)
            settings->setValue ("midiExportNormalize", shouldNormalize);

        auto midiFile = safe->player.midiPlayer.getLoadedFile();
        auto folder = juce::File (settings != nullptr ? settings->getValue ("midiExportFolder", "") : juce::String());
        if (! folder.isDirectory())
            folder = midiFile.getParentDirectory();

        auto fc = std::make_shared<juce::FileChooser> ("Export WAV", folder.getChildFile (midiFile.getFileNameWithoutExtension() + ".wav"), "*.wav");
        auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                   | juce::FileBrowserComponent::warnAboutOverwriting;

        fc->launchAsync (flags, [safe, fc, shouldNormalize] (const juce::FileChooser&)
        {
            auto file = fc->getResult();
            if (safe == nullptr || file == juce::File())
                return;

            safe->exportMidiFile (file.withFileExtension (".wav"), shouldNormalize);
        });
    }), true);
}

void SideBarComponent::exportMidiFile (const juce::File& wavFile, bool normalize)
{
    if (exportJob != nullptr)
        return;

    auto* processor = filterWindow.pluginHolder->processor.get();

    // A second instance with the current state renders the file, so the
    // device keeps playing the first
    std::unique_ptr<juce::AudioProcessor> copy (createPluginFilterOfType (juce::AudioProcessor::wrapperType_Standalone));
    if (copy == nullptr)
        return;

    copy->setBusesLayout (processor->getBusesLayout());

    juce::MemoryBlock state;
    processor->getStateInformation (state);
    copy->setStateInformation (state.getData(), int (state.getSize()));

    if (auto* settings = filterWindow.pluginHolder->settings.get())
        settings->setValue ("midiExportFolder", wavFile.getParentDirectory().getFullPathName());

    auto rate = processor->getSampleRate();
    if (rate <= 0.0)
        rate = 48000.0;

    MidiFileExporter::Options options;
    options.normalize = normalize;

    exportJob = std::make_unique<ExportJob> (std::move (copy), player.midiPlayer.getLoadedFile(), wavFile, rate, options,
                                             getTopLevelComponent(),
                                             [this, normalize, options] (const MidiFileExporter::Result& result)
    {
        const auto file = exportJob->getWavFile();

        // Deleted once this callback has returned
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SideBarComponent> (this)]
        {
            if (safe != nullptr)
            {
                safe->exportJob.reset();
                safe->updatePlayStopButtons();
            }
        });

        if (result.cancelled)
            return;

        if (result.status.failed())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export failed",
                                                    result.status.getErrorMessage(), {}, this);
            return;
        }

        auto dB = [] (float gain) { return juce::String (juce::Decibels::gainToDecibels (gain, -150.0f), 1); };
        auto totalSeconds = juce::roundToInt (result.seconds);

        juce::String message;
        message << "Wrote " << file.getFileName() << " (" << totalSeconds / 60 << ":"
                << juce::String (totalSeconds % 60).paddedLeft ('0', 2) << ").\n\n";

        if (result.renderedPeak <= 0.0f)
            message << "The render is silent.";
        else
        {
            if (result.gain > 1.0f)
                message << "Normalized: raised " << dB (result.gain) << " dB. ";
            else if (result.gain < 1.0f)
                message << (normalize ? "Normalized: lowered " : "Lowered ") << dB (1.0f / result.gain) << " dB so it doesn't clip. ";

            message << "Peak " << dB (result.getWrittenPeak()) << " dBTP.";
        }

        if (result.tailCut)
            message << "\n\nIt was still sounding " << juce::String (options.maxTailSeconds, 0)
                    << " s after the MIDI file ended, so it fades out there.";

        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export finished", message, {}, this);
    });

    updatePlayStopButtons();
    exportJob->launchThread();
}

void SideBarComponent::saveMidiFilePath()
{
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        auto file = player.midiPlayer.getLoadedFile();
        props->setValue ("midiFilePath", file.getFullPathName());
    }
}

void SideBarComponent::restoreMidiFilePath()
{
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        auto path = props->getValue ("midiFilePath", "");
        if (path.isNotEmpty())
        {
            juce::File file (path);
            if (file.existsAsFile() && player.midiPlayer.load (file))
                updatePlayStopButtons();
        }
    }
}

void SideBarComponent::addToMidiMRU (const juce::File& file)
{
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        juce::StringArray mru;
        mru.addTokens (props->getValue ("midiMRU", ""), "|", "");
        mru.removeEmptyStrings();

        // Remove if already exists
        mru.removeString (file.getFullPathName());

        // Add to front
        mru.insert (0, file.getFullPathName());

        // Keep only 10
        while (mru.size() > 10)
            mru.remove (mru.size() - 1);

        props->setValue ("midiMRU", mru.joinIntoString ("|"));
    }
}

juce::StringArray SideBarComponent::getMidiMRU()
{
    juce::StringArray mru;
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        mru.addTokens (props->getValue ("midiMRU", ""), "|", "");
        mru.removeEmptyStrings();
    }
    return mru;
}

void SideBarComponent::browseForMidiFile()
{
    if (midiChooser != nullptr)
        return;

    auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        juce::File lastDir (props->getValue ("midiLastDirectory", ""));
        if (lastDir.isDirectory())
            folder = lastDir;
    }

    midiChooser = std::make_unique<juce::FileChooser> ("Load MIDI File", folder, "*.mid;*.midi");

    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    midiChooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        midiChooser.reset();

        if (! file.existsAsFile())
            return;

        if (auto* props = filterWindow.pluginHolder->settings.get())
            props->setValue ("midiLastDirectory", file.getParentDirectory().getFullPathName());

        if (player.midiPlayer.load (file))
        {
            addToMidiMRU (file);
            saveMidiFilePath();
            updatePlayStopButtons();
            midi.repaint();
        }
    });
}

void SideBarComponent::showMidiMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());

    const bool isLooping = player.midiPlayer.isLooping();
    menu.addItem ("Loop", true, isLooping, [this, isLooping]
    {
        player.midiPlayer.setLooping (! isLooping);
        if (auto* props = filterWindow.pluginHolder->settings.get())
            props->setValue ("midiLoop", ! isLooping);
    });

    menu.addSeparator();

    menu.addItem ("Load File...", [this] { browseForMidiFile(); });

    menu.addItem ("Clear File", player.midiPlayer.hasFileLoaded(), false, [this]
    {
        player.midiPlayer.clear();

        if (auto* props = filterWindow.pluginHolder->settings.get())
            props->setValue ("midiFilePath", "");

        updatePlayStopButtons();
        midi.repaint();
    });

    // Add MRU list
    auto mru = getMidiMRU();
    if (mru.size() > 0)
    {
        menu.addSeparator();

        for (int i = 0; i < mru.size(); ++i)
        {
            juce::File file (mru[i]);
            if (file.existsAsFile())
            {
                menu.addItem (file.getFileName(), [this, file]
                {
                    if (player.midiPlayer.load (file))
                    {
                        addToMidiMRU (file);
                        saveMidiFilePath();
                        updatePlayStopButtons();
                        midi.repaint();
                    }
                });
            }
        }
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (midiMenu));
}

void SideBarComponent::updateSamplePlayStopButtons()
{
    const bool isPlaying = player.samplePlayer.isPlaying();
    samplePlay.setEnabled (! isPlaying && player.samplePlayer.hasFileLoaded());
    sampleStop.setEnabled (isPlaying || player.samplePlayer.getPosition() > 0.0);
}

void SideBarComponent::saveSampleFilePath()
{
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        auto file = player.samplePlayer.getLoadedFile();
        props->setValue ("sampleFilePath", file.getFullPathName());
    }
}

void SideBarComponent::restoreSampleFilePath()
{
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        auto path = props->getValue ("sampleFilePath", "");
        if (path.isNotEmpty())
        {
            juce::File file (path);
            if (file.existsAsFile())
            {
                player.samplePlayer.load (file);
                sample.setBuffer (player.samplePlayer.getBuffer());
                updateSamplePlayStopButtons();
            }
        }
    }
}

void SideBarComponent::addToSampleMRU (const juce::File& file)
{
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        juce::StringArray mru;
        mru.addTokens (props->getValue ("sampleMRU", ""), "|", "");
        mru.removeEmptyStrings();

        // Remove if already exists
        mru.removeString (file.getFullPathName());

        // Add to front
        mru.insert (0, file.getFullPathName());

        // Keep only 10
        while (mru.size() > 10)
            mru.remove (mru.size() - 1);

        props->setValue ("sampleMRU", mru.joinIntoString ("|"));
    }
}

juce::StringArray SideBarComponent::getSampleMRU()
{
    juce::StringArray mru;
    if (auto* props = filterWindow.pluginHolder->settings.get())
    {
        mru.addTokens (props->getValue ("sampleMRU", ""), "|", "");
        mru.removeEmptyStrings();
    }
    return mru;
}

void SideBarComponent::showSampleMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());

    const bool isLooping = player.samplePlayer.isLooping();
    menu.addItem ("Loop", true, isLooping, [this, isLooping]
    {
        player.samplePlayer.setLooping (! isLooping);
        if (auto* props = filterWindow.pluginHolder->settings.get())
            props->setValue ("sampleLoop", ! isLooping);
    });

    menu.addSeparator();

    menu.addItem ("Load File...", [this]
    {
        auto fc = std::make_shared<juce::FileChooser> ("Load Audio File",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
            "*.wav;*.aiff;*.aif;*.mp3;*.ogg;*.flac");

        auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        fc->launchAsync (flags, [this, fc] (const juce::FileChooser&)
        {
            auto file = fc->getResult();
            if (file.existsAsFile())
            {
                player.samplePlayer.load (file);
                sample.setBuffer (player.samplePlayer.getBuffer());
                addToSampleMRU (file);
                saveSampleFilePath();
                updateSamplePlayStopButtons();
                sample.repaint();
            }
        });
    });

    menu.addItem ("Clear File", player.samplePlayer.hasFileLoaded(), false, [this]
    {
        player.samplePlayer.clear();
        sample.clear();

        if (auto* props = filterWindow.pluginHolder->settings.get())
            props->setValue ("sampleFilePath", "");

        updateSamplePlayStopButtons();
        sample.repaint();
    });

    // Add MRU list
    auto mru = getSampleMRU();
    if (mru.size() > 0)
    {
        menu.addSeparator();

        for (int i = 0; i < mru.size(); ++i)
        {
            juce::File file (mru[i]);
            if (file.existsAsFile())
            {
                menu.addItem (file.getFileName(), [this, file]
                {
                    player.samplePlayer.load (file);
                    sample.setBuffer (player.samplePlayer.getBuffer());
                    addToSampleMRU (file);
                    saveSampleFilePath();
                    updateSamplePlayStopButtons();
                    sample.repaint();
                });
            }
        }
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (sampleMenu));
}

void SideBarComponent::updateRecordButtons()
{
    const bool isRecording = player.audioRecorder.isRecording();
    const bool isWaiting = player.audioRecorder.isWaitingForTrigger();

    recordNormal.setEnabled (! isWaiting);
    recordTriggered.setEnabled (! isRecording || isWaiting);
    recordRetrospective.setEnabled (! isRecording && ! isWaiting);
    recordStop.setEnabled (isRecording || isWaiting);
}

void SideBarComponent::updateRecordButtonColours()
{
    const auto mode = player.audioRecorder.getRecordMode();
    auto accentColour = findColour (GinLookAndFeel::accentColourId);

    // Normal record button - accent only when recording via normal mode
    if (mode == AudioRecorder::RecordMode::normal)
        recordNormal.setColour (accentColour);
    else
        recordNormal.clearColour();

    // Triggered record button - flash when waiting, solid accent when triggered recording
    if (mode == AudioRecorder::RecordMode::waitingForTrigger)
    {
        if (flashState)
            recordTriggered.setColour (accentColour);
        else
            recordTriggered.clearColour();
    }
    else if (mode == AudioRecorder::RecordMode::triggered)
        recordTriggered.setColour (accentColour);
    else
        recordTriggered.clearColour();
}

void SideBarComponent::showRecordMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&getLookAndFeel());

    menu.addItem ("Set Destination Folder...", [this]
    {
        auto fc = std::make_shared<juce::FileChooser> ("Select Destination Folder",
            player.audioRecorder.getDestFolder());

        auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
        fc->launchAsync (flags, [this, fc] (const juce::FileChooser&)
        {
            auto folder = fc->getResult();
            if (folder.isDirectory())
            {
                player.audioRecorder.setDestFolder (folder);

                if (auto* props = filterWindow.pluginHolder->settings.get())
                    props->setValue ("recordDestFolder", folder.getFullPathName());
            }
        });
    });

    menu.addSeparator();

    auto destFolder = player.audioRecorder.getDestFolder();
    menu.addItem ("Open Destination Folder", destFolder.isDirectory(), false, [destFolder]
    {
        destFolder.revealToUser();
    });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (recordMenu));
}

void SideBarComponent::paint (juce::Graphics& g)
{
    juce::ColourGradient grad (findColour (GinLookAndFeel::matte1ColourId), 0, 0,
                               findColour (GinLookAndFeel::matte2ColourId), 0, float (getHeight()), false);

    g.setGradientFill (grad);
    g.fillAll();
}

void SideBarComponent::resized()
{
    viewport.setBounds (getLocalBounds());
    auto layout = SidebarLayout::create (getWidth(), midi.isVisible(), sample.isVisible(), xyScope.isVisible());
    const int contentWidth = juce::jmax (0, getWidth() - (layout.height > getHeight() ? viewport.getScrollBarThickness() : 0));
    layout = SidebarLayout::create (contentWidth, midi.isVisible(), sample.isVisible(), xyScope.isVisible());
    content.setSize (contentWidth, juce::jmax (getHeight(), layout.height));

    auto playerSection = [] (juce::Rectangle<int> rc, juce::Component& header, juce::Component& display,
                             juce::Component& play, juce::Component& stop, juce::Component& menu,
                             juce::Component* extra = nullptr)
    {
        header.setBounds (rc.removeFromTop (16));
        rc.removeFromTop (2);
        display.setBounds (rc.removeFromTop (35));
        rc.removeFromTop (2);
        play.setBounds (rc.removeFromLeft (16));
        rc.removeFromLeft (2);
        stop.setBounds (rc.removeFromLeft (16));
        rc.removeFromLeft (2);
        menu.setBounds (rc.removeFromLeft (16));

        if (extra != nullptr)
        {
            rc.removeFromLeft (2);
            extra->setBounds (rc.removeFromLeft (16));
        }

        return rc;
    };

    auto midiRow = playerSection (layout.midi, midiHeader, midi, midiPlay, midiStop, midiMenu, &midiExport);
    midiFileName.setBounds (midiRow.withTrimmedLeft (4).withHeight (16));
    playerSection (layout.sample, sampleHeader, sample, samplePlay, sampleStop, sampleMenu);

    auto graphSection = [] (juce::Rectangle<int> rc, juce::Component& header, juce::Component& display)
    {
        header.setBounds (rc.removeFromTop (16));
        rc.removeFromTop (2);
        display.setBounds (rc);
    };

    graphSection (layout.spectrum, spectrumHeader, spectrum);
    graphSection (layout.scope, scopeHeader, scope);
    graphSection (layout.xy, xyHeader, xyScope);
    graphSection (layout.output, outputHeader, outputGain);

    auto rc = layout.record;
    recordHeader.setBounds (rc.removeFromTop (16));
    rc.removeFromTop (2);
    recordBuffer.setBounds (rc.removeFromTop (35));
    rc.removeFromTop (2);
    for (auto* button : { &recordNormal, &recordTriggered, &recordRetrospective, &recordStop, &recordMenu })
    {
        button->setBounds (rc.removeFromLeft (16));
        rc.removeFromLeft (2);
    }
}

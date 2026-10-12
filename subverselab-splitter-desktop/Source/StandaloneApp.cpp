/*  ELEK as a desktop app (the family's pattern, MIHENK Source/plugin/StandaloneApp.cpp): the same processor and editor as the
    plug-in in one native window, with the wrapper's audio device output only (the app plays files, it never opens a microphone).
    This replaces JUCE's stock window: its grey title strip, the "Options" button and the yellow "input muted" bar.
    Files opened from the Finder ("Open With", dropped on the Dock icon) load as the track. Device and settings are remembered. */
#include <JuceHeader.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Theme.h"

namespace elek
{
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (juce::StandalonePluginHolder& h)
        : juce::DocumentWindow ("ELEK", Theme::background, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          holder (h)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (holder.processor->createEditorIfNeeded(), true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

private:
    juce::StandalonePluginHolder& holder;
};

class StandaloneApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "ELEK"; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "ELEK";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support/SubverseLab/ELEK";
        options.folderName = "SubverseLab/ELEK";
        settings.setStorageParameters (options);

        juce::StandalonePluginHolder::PluginInOuts outputOnly;
        outputOnly.numIns = 0;
        outputOnly.numOuts = 2;
        juce::Array<juce::StandalonePluginHolder::PluginInOuts> channels;
        channels.add (outputOnly);
        holder = std::make_unique<juce::StandalonePluginHolder> (settings.getUserSettings(), false, juce::String(), nullptr, channels, false);
        if (auto* p = dynamic_cast<SubverseSplitterAudioProcessor*> (holder->processor.get()))
            p->showAudioSettings = [this] { holder->showAudioSettingsDialog(); };
        window = std::make_unique<MainWindow> (*holder);
        openFromCommandLine (commandLine);
    }

    void anotherInstanceStarted (const juce::String& commandLine) override { openFromCommandLine (commandLine); }

    void shutdown() override
    {
        if (holder != nullptr) holder->savePluginState();
        window = nullptr;
        holder = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (holder != nullptr) holder->savePluginState();
        quit();
    }

private:
    void openFromCommandLine (const juce::String& commandLine)
    {
        if (holder == nullptr) return;
        auto* editor = dynamic_cast<SubverseSplitterAudioProcessorEditor*> (holder->processor->getActiveEditor());
        if (editor == nullptr) return;
        for (auto& token : juce::StringArray::fromTokens (commandLine, true))
        {
            const juce::File f (token.unquoted());
            if (f.existsAsFile() && editor->isInterestedInFileDrag (juce::StringArray (f.getFullPathName())))
            {
                editor->filesDropped (juce::StringArray (f.getFullPathName()), 0, 0);
                break;
            }
        }
    }

    juce::ApplicationProperties settings;
    std::unique_ptr<juce::StandalonePluginHolder> holder;
    std::unique_ptr<MainWindow> window;
};
} // namespace elek

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new elek::StandaloneApp(); }

#endif

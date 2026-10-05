/*  MIHENK as a desktop app: the same processor and editor as the plug-in, in one native window, with the wrapper's
    audio device (output only: the app plays files, it never opens a microphone). Files opened from the Finder
    ("Open With", dropped on the Dock icon) load as the source. Settings and the device are remembered. */
#include <JuceHeader.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace mihenk
{
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (juce::StandalonePluginHolder& h)
        : juce::DocumentWindow ("MIHENK", juce::Colour (0xff070a09), juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          holder (h)
    {
        setUsingNativeTitleBar (true);
        auto* editor = holder.processor->createEditorIfNeeded();
        setContentOwned (editor, true);
        setResizable (true, false);
        if (auto* ed = dynamic_cast<juce::AudioProcessorEditor*> (editor))
            if (auto* c = ed->getConstrainer())
            {
                // No aspect lock on the window: its height includes the title bar, which made the panel taller than
                // the content area and cut the bottom off. The editor letterboxes the panel instead.
                constrainer.setSizeLimits (c->getMinimumWidth(), c->getMinimumHeight() + getTitleBarHeight(), c->getMaximumWidth(), c->getMaximumHeight() + getTitleBarHeight());
                setConstrainer (&constrainer);
            }
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

private:
    juce::StandalonePluginHolder& holder;
    juce::ComponentBoundsConstrainer constrainer;
};

/*  Edit menu: Undo / Redo with the name of the step ("Undo Target CLUB -8") and Cmd+Z / Cmd+Shift+Z (Ctrl on Windows). Both act on the
    session's history (the intent, never the audio thread). While a text box is being typed into, the key is the text box's own. */
class EditMenu : public juce::MenuBarModel,
                 public juce::ApplicationCommandTarget,
                 private juce::ChangeListener
{
public:
    enum Commands { cmdUndo = 0x6d01, cmdRedo = 0x6d02 };

    explicit EditMenu (MihenkProcessor& p) : processor (p)
    {
        commands.registerAllCommandsForTarget (this);
        commands.setFirstCommandTarget (this);
        setApplicationCommandManagerToWatch (&commands);
        processor.getSession().addChangeListener (this);
    }
    ~EditMenu() override { processor.getSession().removeChangeListener (this); setApplicationCommandManagerToWatch (nullptr); }

    juce::ApplicationCommandManager& getCommands() noexcept { return commands; }

    juce::StringArray getMenuBarNames() override { return { "Edit" }; }
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override
    {
        auto& s = processor.getSession();
        juce::PopupMenu m;
        m.addCommandItem (&commands, cmdUndo, s.canUndo() ? "Undo " + s.undoName() : juce::String ("Undo"));
        m.addCommandItem (&commands, cmdRedo, s.canRedo() ? "Redo " + s.redoName() : juce::String ("Redo"));
        return m;
    }
    void menuItemSelected (int, int) override {}

    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>& c) override { c.add (cmdUndo); c.add (cmdRedo); }
    void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override
    {
        auto& s = processor.getSession();
        const bool redo = id == cmdRedo;
        info.setInfo (redo ? (s.canRedo() ? "Redo " + s.redoName() : juce::String ("Redo")) : (s.canUndo() ? "Undo " + s.undoName() : juce::String ("Undo")),
                      redo ? "Redo the last undone change" : "Undo the last change", "Edit", 0);
        info.addDefaultKeypress ('z', redo ? juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier : juce::ModifierKeys::commandModifier);
       #if ! JUCE_MAC
        if (redo) info.addDefaultKeypress ('y', juce::ModifierKeys::commandModifier);
       #endif
        info.setActive (textBoxHasFocus() || (redo ? s.canRedo() : s.canUndo()));   // while typing the key stays live: perform() hands it to the text box
    }
    bool perform (const InvocationInfo& info) override
    {
        if (info.commandID != cmdUndo && info.commandID != cmdRedo) return false;
        const bool redo = info.commandID == cmdRedo;
        if (auto* te = dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()))
        {
            auto mods = juce::ModifierKeys (juce::ModifierKeys::commandModifier | (redo ? juce::ModifierKeys::shiftModifier : 0));
            return te->keyPressed (juce::KeyPress ('z', mods, 'z'));
        }
        auto& s = processor.getSession();
        return redo ? s.redo() : s.undo();
    }

private:
    static bool textBoxHasFocus() { return dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr; }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { menuItemsChanged(); commands.commandStatusChanged(); }

    MihenkProcessor& processor;
    juce::ApplicationCommandManager commands;
};

class StandaloneApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MIHENK"; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "MIHENK";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support/SubverseLab/MIHENK";
        options.folderName = "SubverseLab/MIHENK";
        settings.setStorageParameters (options);

        juce::StandalonePluginHolder::PluginInOuts outputOnly;
        outputOnly.numIns = 0;
        outputOnly.numOuts = 2;
        juce::Array<juce::StandalonePluginHolder::PluginInOuts> channels;
        channels.add (outputOnly);
        holder = std::make_unique<juce::StandalonePluginHolder> (settings.getUserSettings(), false, juce::String(), nullptr, channels, false);
        if (auto* p = dynamic_cast<MihenkProcessor*> (holder->processor.get()))
            p->showAudioSettings = [this] { holder->showAudioSettingsDialog(); };
        window = std::make_unique<MainWindow> (*holder);
        if (auto* p = dynamic_cast<MihenkProcessor*> (holder->processor.get()))
        {
            editMenu = std::make_unique<EditMenu> (*p);
           #if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu (editMenu.get());
           #else
            window->setMenuBar (editMenu.get());
            window->addKeyListener (editMenu->getCommands().getKeyMappings());
           #endif
        }
        openFromCommandLine (commandLine);
    }

    void anotherInstanceStarted (const juce::String& commandLine) override { openFromCommandLine (commandLine); }

    void shutdown() override
    {
        if (holder != nullptr) holder->savePluginState();
        dropMenu();
        window = nullptr;
        holder = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (holder != nullptr) holder->savePluginState();
        dropMenu();
        quit();
    }

private:
    void dropMenu()
    {
        if (editMenu == nullptr) return;
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #else
        if (window != nullptr) { window->removeKeyListener (editMenu->getCommands().getKeyMappings()); window->setMenuBar (nullptr); }
       #endif
        editMenu = nullptr;
    }

    void openFromCommandLine (const juce::String& commandLine)
    {
        auto* p = holder != nullptr ? dynamic_cast<MihenkProcessor*> (holder->processor.get()) : nullptr;
        if (p == nullptr) return;
        for (auto& token : juce::StringArray::fromTokens (commandLine, true))
        {
            const juce::File f (token.unquoted());
            if (f.existsAsFile() && (f.hasFileExtension ("wav") || f.hasFileExtension ("aif") || f.hasFileExtension ("aiff")))
            {
                p->getSession().loadInput (f);
                break;
            }
        }
    }

    juce::ApplicationProperties settings;
    std::unique_ptr<juce::StandalonePluginHolder> holder;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<EditMenu> editMenu;
};
} // namespace mihenk

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new mihenk::StandaloneApp(); }

#endif

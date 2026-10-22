// rack_capture: opens Reason Rack Plugin's own window; the owner picks a patch there once and presses SAVE STATE; the plug-in state goes
// to out/rack/<name>.rackstate and rack_render renders any notes from it with no screen (SPEC "Reason as the reference").
//
//   rack_capture [--au] [--out-dir out/rack] [--plugin path]
// The window is a normal JUCE window on the owner's display. No audio device is opened. Nothing is installed or changed.
#include "rack_common.h"

namespace
{
struct Panel : juce::Component
{
    Panel (std::unique_ptr<juce::AudioPluginInstance> p, juce::PluginDescription d, juce::File dir)
        : plugin (std::move (p)), desc (std::move (d)), outDir (std::move (dir))
    {
        editor.reset (plugin->createEditorIfNeeded());
        if (editor != nullptr) addAndMakeVisible (*editor);
        name.setText ("reference-1", juce::dontSendNotification);
        name.setTextToShowWhenEmpty ("name of this reference (family-patch)", juce::Colours::grey);
        addAndMakeVisible (name);
        save.onClick = [this] { doSave(); };
        addAndMakeVisible (save);
        status.setText ("1. Pick the reference patch in the rack.  2. Type a name.  3. SAVE STATE.  Repeat for the next patch; close the window when done.", juce::dontSendNotification);
        addAndMakeVisible (status);
        const int w = editor != nullptr ? editor->getWidth() : 800, h = editor != nullptr ? editor->getHeight() : 400;
        setSize (juce::jmax (w, 640), h + 70);
    }
    ~Panel() override { editor = nullptr; }
    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (70).reduced (6);
        status.setBounds (bar.removeFromBottom (24));
        save.setBounds (bar.removeFromRight (140));
        name.setBounds (bar.reduced (0, 2));
        if (editor != nullptr) editor->setTopLeftPosition (r.getX() + (r.getWidth() - editor->getWidth()) / 2, r.getY());
    }
    void doSave()
    {
        auto nm = juce::File::createLegalFileName (name.getText().trim());
        if (nm.isEmpty()) { status.setText ("Type a name first.", juce::dontSendNotification); return; }
        juce::MemoryBlock state; plugin->getStateInformation (state);
        outDir.createDirectory();
        const auto f = outDir.getChildFile (nm + ".rackstate");
        const bool ok = rack::save (f, desc, state);
        status.setText (ok ? "Saved " + f.getFileName() + " (" + juce::String ((int) state.getSize()) + " bytes). Next patch, or close the window."
                           : "COULD NOT WRITE " + f.getFullPathName(), juce::dontSendNotification);
        std::printf ("%s %s %d bytes\n", ok ? "saved" : "FAILED", f.getFullPathName().toRawUTF8(), (int) state.getSize());
    }
    std::unique_ptr<juce::AudioPluginInstance> plugin; juce::PluginDescription desc; juce::File outDir;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::TextEditor name; juce::TextButton save { "SAVE STATE" }; juce::Label status;
};

struct Window : juce::DocumentWindow
{
    Window (std::unique_ptr<Panel> p) : juce::DocumentWindow ("rack_capture - Reason Rack Plugin", juce::Colours::darkgrey, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true); setContentOwned (p.release(), true); centreWithSize (getWidth(), getHeight()); setVisible (true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

struct App : juce::JUCEApplication
{
    const juce::String getApplicationName() override { return "rack_capture"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    void initialise (const juce::String& cmd) override
    {
        const auto args = juce::StringArray::fromTokens (cmd, true);
        const bool au = args.contains ("--au");
        auto arg = [&] (const juce::String& k, const juce::String& d) { const int i = args.indexOf (k); return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : d; };
        const juce::File bundle = arg ("--plugin", "").isNotEmpty() ? juce::File (arg ("--plugin", "")) : rack::defaultPlugin (au);
        juce::AudioPluginFormatManager fm; rack::addFormats (fm);
        juce::PluginDescription desc; juce::String err;
        if (! rack::findDescription (fm, bundle, desc, err)) { std::printf ("rack_capture: %s\n", err.toRawUTF8()); setApplicationReturnValue (2); quit(); return; }
        auto plugin = fm.createPluginInstance (desc, 44100.0, 512, err);
        if (plugin == nullptr) { std::printf ("rack_capture: could not load %s: %s\n", desc.name.toRawUTF8(), err.toRawUTF8()); setApplicationReturnValue (3); quit(); return; }
        plugin->prepareToPlay (44100.0, 512);
        std::printf ("rack_capture: loaded %s (%s)\n", desc.name.toRawUTF8(), desc.pluginFormatName.toRawUTF8());
        window = std::make_unique<Window> (std::make_unique<Panel> (std::move (plugin), desc, juce::File (arg ("--out-dir", "out/rack")).getChildFile ("")));
    }
    void shutdown() override { window = nullptr; }
    void systemRequestedQuit() override { quit(); }
    std::unique_ptr<Window> window;
};
}

START_JUCE_APPLICATION (App)

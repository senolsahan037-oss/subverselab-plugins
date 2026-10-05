#pragma once

#include <JuceHeader.h>
#include "Session.h"
#include "LiveAnalysis.h"
#include "Widgets.h"
#include "ProfilePanel.h"
#include "Panels.h"
#include "BatchPanel.h"
#include "Theme.h"

/*  The face of the MIHENK MH-86: a 1986 digital mastering processor as it left the factory, in SubverseLab colours.
    Rack ears, a dark green anodized front with cream silkscreen. Across the top the name plate, a VFD with the source
    and the status, and the keys (the app's transport, or LEARN in a DAW). In the middle the phosphor CRT (wave,
    spectrum, loudness: live and layered while the audio moves, the whole track when it stops) beside the measurement
    VFD, the loudness target and the DIAGNOSIS panel. Along the bottom the chain in signal order: input, equaliser
    (static, AIR, dynamic), compressor (WIDE or 3-BAND), stereo and low end, limiter, reference. Down the right edge,
    full height, the meters. Every control works and every number is measured.

    Designed at 1480 x 890 and scaled as a whole by the editor (larger than v1's 1320 x 830: the full-height meter
    column, the DIAGNOSIS panel and the dynamic-EQ / 3-BAND rows needed the room; at 97 % it still fits a 13-inch
    laptop screen).
*/
namespace mihenk
{
class MihenkProcessor;

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit MainComponent (MihenkProcessor& processor);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** Snapshot tool: pick the CRT view and settle the meters. */
    void setScopeMode (ui::CrtScope::Mode m);
    void settleForSnapshot();
    /** Snapshot tool: draw the live views as if playing (the live analysis must already hold data). */
    void setForceLive (bool on);
    /** Snapshot tool: select a band of the 3-BAND compressor. */
    void selectBand (int band);
    /** Snapshot tool: open the in-place value editor of a knob (by label) with some typed text. */
    void openValueEditorForSnapshot (const juce::String& knobLabel, const juce::String& typed);
    /** Snapshot tool / tests: use this folder for presets instead of Application Support. */
    void setPresetsFolder (const juce::File& f) { presetsDir = f; }
    /** Snapshot tool: show a preset name as loaded (baseline = the current settings). */
    void presetLoadedForSnapshot (const juce::String& name, bool factory) { markPresetBaseline (name, factory, {}); }
    LiveAnalysis& getLive() { return live; }
    /** Snapshot tool: open the profile panel, optionally with tags picked and a note typed. */
    void openProfileForSnapshot (const juce::StringArray& tagIds, const juce::String& text);
    /** Snapshot tool: draw a knob's tooltip in place (a TooltipWindow is a separate window and not in a snapshot). */
    void showTooltipForSnapshot (const juce::String& knobLabel);

    /** SIMPLE (starting point, target, diagnosis, macro knobs, meters and views) or ADVANCED (every control).
        persist = remember it on this computer (the snapshot tool and tests pass false). */
    void setFace (bool advancedFace, bool persist = true);
    bool isAdvancedFace() const noexcept { return advanced; }
    void showBatchPanel (bool show);
    ui::BatchPanel& getBatchPanel() noexcept { return batchPanel; }

    // Tests: knobs by label (the visible one when two share a label).
    double knobValueForTesting (const juce::String& label) const;
    juce::String knobTextForTesting (const juce::String& label) const;
    bool autoLampForTesting (const juce::String& label) const;
    bool knobVisibleForTesting (const juce::String& label) const;
    /** Turns the knob by hand (as a drag or a typed value would). */
    void turnKnobForTesting (const juce::String& label, double displayValue);
    void clickAutoLampForTesting (const juce::String& label);
    /** Tests: a real drag of the knob: mouse-down (gesture begins), `steps` value changes from `from` to `to`, mouse-up (gesture ends). */
    void dragKnobForTesting (const juce::String& label, double from, double to, int steps);
    /** Tests: the CHARACTER caps and RESET OVERRIDES as a click would press them. */
    void clickCharacterForTesting (PlanCharacter c);
    void clickResetOverridesForTesting();
    bool overrideRingForTesting (const juce::String& label) const;
    /** Runs the panel's 30 Hz timer n times (knob animation, lamps). */
    void tickForTesting (int n) { for (int i = 0; i < n; ++i) timerCallback(); }
    bool isComponentVisibleForTesting (const juce::String& name) const;
    /** Tests: a module switch / key by name (EQ IN, LOW CUT, 24 DB, LINEAR, AIR DYN, LOW-MID DYN, UPPER-MID DYN, TIGHTEN, DC FILTER, COMP IN,
        3-BAND, SOLO, BAND BYPASS, BAND LOW, BAND MID, BAND HIGH, WIDTH IN, PHASE SAFE, LIMITER IN, BYPASS) as a click sets it. False: no such switch. */
    bool clickSwitchForTesting (const juce::String& name, bool on);
    bool switchStateForTesting (const juce::String& name) const;
    bool switchEnabledForTesting (const juce::String& name) const;
    /** Tests: the automatable parameter id a knob moves ("" none), whether it is dimmed (a context it needs is off) or enabled, and the extra hint line it shows. */
    juce::String knobParamIdForTesting (const juce::String& label) const;
    bool knobDimmedForTesting (const juce::String& label) const;
    bool knobEnabledForTesting (const juce::String& label) const;
    juce::String knobHintForTesting (const juce::String& label) const;
    /** Tests: the knob's travel in display units (min, max) and its scale (display = stored * scale). "RELEASE#2" picks the second shown knob of that name. */
    bool knobRangeForTesting (const juce::String& label, double& mn, double& mx, double& scale) const;
    /** Tests: a click on the value text of a knob (what a stepped knob turns into "next step"). */
    void clickKnobValueForTesting (const juce::String& label);

    static constexpr int panelWidth = 1480, panelHeight = 890;

private:
    struct Knob
    {
        juce::String label;
        double scale = 1.0;                                   // display value = stored * scale
        std::function<double& (MasterSettings&)> ref;
        std::function<juce::String (double)> format;
        std::function<juce::String()> paramId;                // the automatable parameter it moves (band-aware)
        std::function<void()> after;
        ui::KnobSlider slider;
        std::unique_ptr<juce::Component> valueHit;
        int module = 0, col = 0, row = 0;
        int showIn = 0;                                       // 0 always, 1 WIDE only, 2 3-BAND only
        int face = 1;                                         // 0 both faces, 1 ADVANCED, 2 SIMPLE
        bool needsReference = false;                          // shown only while a reference is loaded
        int cx = -1, cy = -1, size = 42;                      // explicit position (TARGET block, SIMPLE face)
        juce::String hint;                                    // SIMPLE face: what the macro does
        int profileParam = -1;                                // personal profile: the learnable knob it is (-1 none)
        std::unique_ptr<ui::LockBadge> lockBadge;
        enum AutoKind { autoNone = 0, autoThreshold, autoRelease, autoTrim, autoDrive };
        int autoKind = autoNone;                              // a value MIHENK solves: AUTO lamp on the shoulder
        std::unique_ptr<ui::AutoLamp> autoLamp;
        std::function<double (const MasterSettings&)> display; // shown value when it is not ref * scale (TRIM in AUTO, GLUE in 3-BAND)
        double animTarget = 0.0;
        bool animating = false;
        // A knob with a few fixed positions (AIR FREQ): the slider runs 0..N, each position owns one equal zone [i, i+1) of the travel,
        // the pointer rests in the middle of its zone, and a drag that crosses into the next zone selects that step.
        std::vector<double> steps;
        juce::String tip;                                     // tooltip when no profile lock text is shown
        int stepIndex (double sliderValue) const noexcept
        {
            return juce::jlimit (0, (int) steps.size() - 1, (int) std::floor (sliderValue));
        }
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void buildPanelTexture();
    Knob& addKnob (int module, int col, int row, const juce::String& label, double min, double max, double def, double mid,
                   double interval, double scale, std::function<double& (MasterSettings&)> ref, std::function<juce::String (double)> format,
                   std::function<juce::String()> paramId);
    void syncFromSettings();
    void updateDimming();
    void updateModeVisibility();
    void chooseFile (bool reference);
    void chooseExport();
    void paintKnobLegends (juce::Graphics&);
    void beginValueEdit (Knob&);
    void finishValueEdit (bool apply);
    juce::Rectangle<int> valueRect (const Knob&) const;
    juce::File presetsFolder() const { return presetsDir; }
    void showPresetMenu();
    void applyFactory (Genre g);
    bool loadUserPreset (const juce::File& f);
    void savePresetAs (bool asScenario);
    void writePreset (const juce::String& name, bool asScenario);
    void deleteCurrentPreset();
    void markPresetBaseline (const juce::String& name, bool factory, const juce::File& file, int scenario = 0);
    void applyScenarioFromPanel (int scenario);
    void updatePresetDisplay();
    void updateLocks();
    void updateAutoLamps();
    bool isAuto (const Knob&) const;
    void setAuto (Knob&, bool on);
    bool knobShown (const Knob&) const;
    double displayValue (const Knob&) const;
    const Knob* findKnob (const juce::String& label) const;
    juce::Button* switchByName (const juce::String& name) const;
    juce::Point<int> knobCentre (const Knob&) const;
    void showProfilePanel (bool show);
    /** The knob's learnable parameter as it stands now (GLUE is learnable in WIDE only); -1 none. */
    int activeProfileParam (const Knob& k) const;
    bool threeBand() const { return session.settings.comp.mode == CompMode::threeBand; }
    BandSettings& band() { return session.settings.comp.bands[juce::jlimit (0, 2, ctx.selectedBand)]; }

    MihenkProcessor& processor;
    Session& session;
    LiveAnalysis live;
    ui::Context ctx;
    ui::MihenkLookAndFeel lnf;
    ui::CrtScope scope;
    ui::VfdTable table;
    ui::OutputMeters meters;
    ui::InfoDisplay info;
    ui::LearnDisplay learnDisplay;
    ui::DiagnosisPanel diagnosis;
    ui::CrossoverDisplay crossover;
    ui::GrTrace grTrace;
    ui::CorrelationMeter correlation;
    ui::LimiterReadout limiterReadout;
    ui::SmallVfd latencyVfd;
    ui::LoudnessVfd loudnessVfd;
    ui::ReferencePanel referencePanel;
    ui::ChainReadout chainReadout;
    ui::Lamp renderLamp { "RENDER", Theme::lamp }, targetLamp { "ON TARGET", Theme::teal }, limitLamp { "LIMIT WARN", Theme::coral };

    ui::KeyButton openKey { "OPEN" }, refKey { "REF" }, playKey { "PLAY" }, stopKey { "STOP" }, loopKey { "LOOP" },
                  abKey { "A / B" }, exportKey { "EXPORT" }, batchKey { "BATCH" }, audioKey { "AUDIO" }, learnKey { "LEARN" },
                  bypassKey { "BYPASS" }, matchRefKey { "MATCH TO\nREFERENCE" };
    ui::CapButton viewWave { "WAVE" }, viewSpec { "SPECTRUM" }, viewLoud { "LOUDNESS" };
    ui::CapButton layerIn { "IN" }, layerEq { "EQ" }, layerComp { "COMP" }, layerOut { "OUT" };
    ui::CapButton presetStream { "STREAM -14" }, presetApple { "APPLE -16" }, presetClub { "CLUB -8" }, presetKeep { "KEEP INPUT" }, presetCustom { "CUSTOM" };
    ui::CapButton bitsCap { "EXPORT 24-BIT", false }, abMatchCap { "A/B MATCHED" }, clearRef { "CLEAR", false };
    ui::CapButton faceSimple { "SIMPLE" }, faceAdvanced { "ADVANCED" };
    ui::CapButton resetAllCap { "RESET ALL", false }, resetOverridesCap { "RESET OVERRIDES", false };
    ui::CapButton charOff { "OFF" }, charGentle { "GENTLE" }, charFull { "FULL" };
    ui::PresetDisplay presetDisplay;
    ui::CapButton presetSave { "SAVE", false }, presetLoad { "LOAD", false }, presetDelete { "DELETE", false };
    ui::GrReadout grReadout;
    ui::CapButton noteKey { "NOTE" };
    ui::ProfilePanel profilePanel;
    ui::BatchPanel batchPanel;
    std::unique_ptr<juce::Component> tipPreview;
    std::unique_ptr<juce::TextEditor> valueEditor;
    Knob* editingKnob = nullptr;
    juce::File presetsDir, currentPresetFile;
    juce::String currentPresetName { "DEFAULT" }, presetBaseline;
    bool currentPresetFactory = false;
    int currentScenario = 0;
    ui::CapButton eqIn { "IN" }, eqLowCut { "LOW CUT" }, eqSteep { "24 DB" }, eqLinear { "LINEAR" };
    ui::CapButton dynLowMid { "LOW-MID DYN" }, dynUpperMid { "UPPER-MID DYN" }, dynTight { "TIGHTEN" }, eqAirDyn { "AIR DYN" };
    ui::CapButton dcFilter { "DC FILTER" };
    ui::CapButton compIn { "IN" }, compThreeBand { "3-BAND" };
    ui::CapButton bandLow { "LOW" }, bandMid { "MID" }, bandHigh { "HIGH" }, bandSolo { "SOLO" }, bandBypass { "BYPASS" };
    ui::CapButton widthIn { "IN" }, phaseSafe { "PHASE-SAFE" }, limIn { "IN" };

    std::vector<std::unique_ptr<Knob>> knobs;
    juce::Image panelTexture, mark, wordmark;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::RangedAudioParameter* gestureParam = nullptr;
    int exportBits = 24;
    bool syncing = false;
    int lastFindingsHash = -1;
    bool freezeLive = false;          // snapshot tool: keep the live analysis as it is
    bool advanced = false;            // the face (SIMPLE by default for a new user)
    bool animateKnobs = false;        // programmatic knob changes glide (off until the panel is built)
    juce::String visibilityKey;       // which knobs and boxes the panel texture was drawn for

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace mihenk

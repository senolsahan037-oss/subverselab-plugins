// wrapper_check: loads the REAL built bundles (VST3 and AU) the way a host does and measures the dry path, latency and default MIX through the wrapper.
//   wrapper_check <path/SEDA.vst3> [<path/SEDA.component>]
#include <JuceHeader.h>
#include <cmath>
#include <cstdio>

static int gFail = 0;
static void check (bool ok, const juce::String& s) { std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", s.toRawUTF8()); if (! ok) ++gFail; }

static juce::AudioProcessorParameter* findParam (juce::AudioProcessor& p, const char* name)
{
    for (auto* q : p.getParameters()) if (q->getName (64).equalsIgnoreCase (name)) return q;
    return nullptr;
}

static void runOne (juce::AudioPluginFormatManager& fm, const juce::String& path, const juce::String& label)
{
    juce::OwnedArray<juce::PluginDescription> descs;
    for (auto* f : fm.getFormats()) f->findAllTypesForFile (descs, path);
    if (descs.isEmpty()) { check (false, label + ": no plug-in found at " + path); return; }
    std::printf ("\n--- %s: %s (%s, %s) ---\n", label.toRawUTF8(), descs[0]->name.toRawUTF8(), descs[0]->pluginFormatName.toRawUTF8(), descs[0]->fileOrIdentifier.toRawUTF8());
    for (double sr : { 44100.0, 48000.0 })
        for (int block : { 32, 128, 512, 1024 })
            for (int useState = 0; useState < 2; ++useState)
            {
                juce::String err;
                auto p = fm.createPluginInstance (*descs[0], sr, block, err);
                if (p == nullptr) { check (false, label + ": cannot create: " + err); return; }
                p->enableAllBuses();
                p->setPlayConfigDetails (2, 2, sr, block);
                if (useState)
                {   // a typical saved state: MIX 40, TIME 300, FEEDBACK 30, LISTEN on (as a session would hold it)
                    juce::MemoryBlock mb; p->getStateInformation (mb);
                    if (auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()))
                    {
                        for (auto* c : xml->getChildIterator())
                        {
                            const auto id = c->getStringAttribute ("id");
                            if (id == "mix") c->setAttribute ("value", 40.0);
                            if (id == "time") c->setAttribute ("value", 300.0);
                            if (id == "feedback") c->setAttribute ("value", 30.0);
                            if (id == "listen") c->setAttribute ("value", 1.0);
                        }
                        xml->setAttribute ("stateV", 2);
                        juce::MemoryBlock out; juce::AudioProcessor::copyXmlToBinary (*xml, out);
                        p->setStateInformation (out.getData(), (int) out.getSize());
                    }
                }
                p->prepareToPlay (sr, block);
                auto* mixP = findParam (*p, "MIX");
                auto* timeP = findParam (*p, "TIME");
                auto* fbP = findParam (*p, "FEEDBACK");
                if (mixP == nullptr || timeP == nullptr) { check (false, label + ": MIX/TIME not found"); return; }
                const float defMix = mixP->getValue() * 100.0f;
                if (block == 32 && sr == 44100.0) std::printf ("    default MIX as the host sees it (%s): %.1f %%  text '%s'  latency reported %d\n", useState ? "after state" : "factory", defMix, mixP->getText (mixP->getValue(), 32).toRawUTF8(), p->getLatencySamples());
                for (float mix : { 0.0f, 40.0f, 100.0f })
                {
                    mixP->setValue (mix / 100.0f);
                    if (fbP) fbP->setValue (0.0f);
                    timeP->setValue (timeP->getValueForText ("300.0 ms"));
                    p->reset();
                    const int at = 20000, n = (int) sr + 20000;   // 0.4 s of silence first: the 20 ms MIX smoothing settles
                    juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi;
                    std::vector<float> out ((size_t) n, 0.0f);
                    for (int pos = 0; pos < n; pos += block)
                    {
                        const int len = juce::jmin (block, n - pos);
                        buf.setSize (2, len, false, false, true); buf.clear();
                        if (at >= pos && at < pos + len) { buf.setSample (0, at - pos, 0.5f); buf.setSample (1, at - pos, 0.5f); }
                        p->processBlock (buf, midi);
                        for (int i = 0; i < len; ++i) out[(size_t) (pos + i)] = buf.getSample (0, i);
                    }
                    const double g = 1.0 - mix / 100.0;
                    int first = -1; double lvl = 0.0;
                    if (g > 0.0) { for (int i = 0; i < n; ++i) if (std::abs (out[(size_t) i]) > 0.1 * g) { first = i; break; } lvl = 20.0 * std::log10 (std::abs (out[(size_t) at]) / (0.5 * g) + 1e-12); }
                    int pk = -1; double pv = 0; const int want = at + (int) std::lround (0.3 * sr);
                    for (int i = want - 3000; i < want + 3000; ++i) if (std::abs (out[(size_t) i]) > pv) { pv = std::abs (out[(size_t) i]); pk = i; }
                    const bool dryOk = g == 0.0 ? std::abs (out[(size_t) at]) < 1e-3 : (first == at && std::abs (lvl) <= 0.1);
                    const bool repOk = mix == 0.0f || std::abs (pk - want) <= (int) (0.005 * 0.3 * sr) + 2;
                    const bool ok = dryOk && repOk && p->getLatencySamples() == 0;
                    if (! ok || (block == 512 && useState == 0))
                        std::printf ("[%s] %s %.0f Hz block %d %s MIX %.0f: latency %d, first sample %d (want %d), dry level %+.2f dB, first repeat %d (want %d)\n", ok ? "PASS" : "FAIL", label.toRawUTF8(), sr, block, useState ? "state" : "factory", mix, p->getLatencySamples(), first, at, lvl, pk, want);
                    if (! ok) ++gFail;
                }
                p->releaseResources();
            }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());
   #if JUCE_MAC
    fm.addFormat (new juce::AudioUnitPluginFormat());
   #endif
    if (argc > 1) runOne (fm, argv[1], "VST3");
    if (argc > 2) runOne (fm, argv[2], "AU");
    std::printf ("\nwrapper_check: %s\n", gFail == 0 ? "all passed" : "FAILED");
    return gFail == 0 ? 0 : 1;
}

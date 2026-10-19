// The SEQ and DRUM patterns as MIDI clips for the DAW (owner 19 Oct: "DAW'a MIDI nasıl çekilecek"; SPEC "KEY, BPM and the DAW",
// RU-8's drag rules). Standard MIDI File type 0, 96 PPQ, the tempo and 4/4 in the file, swing and accents written in.
// SEQ: the notes the arp / sequencer make from C4 (60) (an offline svl::NoteGen with the current values: the same code
// that plays them); DRUM: tracks 1..8 on notes 36..43 (what the drum machine listens to), accent 127, hit 89.
// Length: whole bars that hold whole cycles of the pattern (a 16-step 1/16 pattern = 1 bar, 7 steps = 7 bars ... up to 8 bars;
// a pattern that never comes round within 8 bars: 4 bars). A step's ROW (a grid step up / down) is a pitch bend at JING FANG's bend
// range; the arp's octaves are in the keys. The MICRO rows of the keyboard are not in the clip: they are saved in the project.
#pragma once
#include <JuceHeader.h>

class JingFangProcessor;

namespace jfclip
{
juce::MidiFile make (const JingFangProcessor&, int unit);              // 0 SEQ, 1 DRUM
int bars (const JingFangProcessor&, int unit);
juce::String clipName (const JingFangProcessor&, int unit);           // "JING FANG <preset> SEQ"
juce::File writeTemp (const JingFangProcessor&, int unit);           // for a drag (the temp folder)
juce::File saveToMusic (const JingFangProcessor&, int unit);         // a click: ~/Music/JING FANG/MIDI, never overwrites
juce::File& saveFolderOverrideForTests();
}

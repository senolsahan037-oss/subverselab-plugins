// SVL engine parameters: one flat table, natural units. The plug-in (milestone 5) maps these one-to-one onto host parameters;
// the preset XML stores them by id. Plain C++17, no JUCE.
#pragma once
#include <array>
#include <cstring>
#include <string>

namespace svl {

enum class OscType : int { off, saw, square, triangle, sine, noiseWhite, noisePink, string, count };
enum class FilterMode : int { lp, bp, hp, notch, count };
enum class VoiceMode : int { poly, mono, legato, count };

inline const char* const kOscTypeNames[] = { "off", "saw", "square", "triangle", "sine", "noise_white", "noise_pink", "string" };
inline const char* const kFilterModeNames[] = { "lp", "bp", "hp", "notch" };
inline const char* const kVoiceModeNames[] = { "poly", "mono", "legato" };
inline const char* const kOffOnNames[] = { "off", "on" };
inline const char* const kFmTargetNames[] = { "osc1", "osc3", "both" };
inline const char* const kEchoTimeNames[] = { "1/16", "1/8", "1/8D", "1/4", "1/4D", "1/2" };
inline const char* const kModFxNames[] = { "off", "chorus", "flanger", "phaser" };
inline const char* const kEchoTypeNames[] = { "ping_pong", "stereo", "mono", "tape" };
inline const char* const kSeqDirNames[] = { "forward", "reverse", "bounce", "random" };
inline const char* const kTuneModeNames[] = { "normal", "micro" };
inline const char* const kRowNames[] = { "low", "mid", "high" };
inline const char* const kPitchClassIds[] = { "c", "db", "d", "eb", "e", "f", "gb", "g", "ab", "a", "bb", "b" };
inline const char* const kArpModeNames[] = { "up", "down", "updown", "downup", "random", "chord", "played" };
inline const char* const kStepRateNames[] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
inline const float kStepRateBeats[] = { 1.0f, 0.5f, 1.0f / 3, 0.25f, 1.0f / 6, 0.125f };
inline const char* const kSeqModeNames[] = { "hold", "free" };
inline const char* const kDrumStepNames[] = { "off", "on", "accent" };
inline const char* const kDrumModeNames[] = { "hold", "free" };
// arp.chord: every held key is expanded to this shape (intervals in keys, so they resolve through the tuning table)
inline const char* const kArpChordNames[] = { "off", "oct", "5th", "maj", "min", "sus2", "sus4", "maj7", "min7", "dom7", "add9", "min9" };
inline const int kArpChordShapes[12][5] = { { 0 }, { 0, 12 }, { 0, 7 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 2, 7 }, { 0, 5, 7 }, { 0, 4, 7, 11 },
                                            { 0, 3, 7, 10 }, { 0, 4, 7, 10 }, { 0, 4, 7, 14 }, { 0, 3, 7, 10, 14 } };
inline const int kArpChordSize[12] = { 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5 };
// daw.seq / daw.drums (RU-8's SET / MIDI rule): pattern = the device's own pattern plays; midi = only the DAW's notes play
inline const char* const kSourceNames[] = { "pattern", "midi" };

constexpr int kNumOsc = 3;

// Per-oscillator block, repeated kNumOsc times.
enum OscField : int { O_TYPE, O_LEVEL, O_OCT, O_SEMI, O_FINE, O_PW, O_PAN, O_KS_DECAY, O_KS_TONE, O_DECAY, O_UNI, O_DETUNE, O_SPREAD, kOscFields };

enum Param : int
{
    OSC_BASE = 0,
    FLT_MODE = OSC_BASE + kNumOsc * kOscFields,
    FLT_CUTOFF, FLT_RESO, FLT_DRIVE, FLT_KEYTRACK, FLT_ENVAMT, FLT_VELAMT,
    AENV_A, AENV_D, AENV_S, AENV_R, AENV_CA, AENV_CD, AENV_CR,
    FENV_A, FENV_D, FENV_S, FENV_R, FENV_CA, FENV_CD, FENV_CR,
    V_MODE, V_POLY, V_GLIDE, V_VELAMP, V_BEND,
    PENV_AMT, PENV_DECAY, CLICK_LEVEL, CLICK_DECAY, CLICK_HP,
    DRUM_MODE, DRUM_PITCH, DRUM_KEYTRACK, CHOKE, DRUM_ONESHOT, PENV_VEL, CLICK_VEL,
    DRIFT_CENTS, DRIFT_AMP, DRIFT_RATE, FLOOR_LEVEL,
    FM_AMOUNT, FM_TARGET,
    LFO_BASE, LFO_END = LFO_BASE + 3 * 5 - 1,          // 3 LFOs x { shape, rate, sync, retrigger, fade }
    MOD_BASE, MOD_END = MOD_BASE + 16 * 3 - 1,         // 16 slots x { source, destination, amount }
    TUNE_MODE, TUNE_EDO, TUNE_REF, ROW_UP, ROW_DOWN,
    TUNE_ROW0, TUNE_ROW11 = TUNE_ROW0 + 11,
    KSW_MODE, KSW_LOW,
    FXD_AMOUNT, FXD_TONE, FXD_MIX, FXE_TIME, FXE_FB, FXE_DAMP, FXE_MIX, FXR_SIZE, FXR_DECAY, FXR_PRE, FXR_DAMP, FXR_MIX,
    ARP_ON, ARP_MODE, ARP_OCT, ARP_RATE, ARP_GATE, ARP_SWING, ARP_LATCH, ARP_CHORD,
    SEQ_ON, SEQ_RATE, SEQ_LEN, SEQ_SWING, SEQ_MODE,
    SEQ_STEP_BASE, SEQ_STEP_END = SEQ_STEP_BASE + 16 * 7 - 1,   // 16 steps x { on, note, row, vel, gate, slide, accent }
    DRUMS_ON, DRUMS_RATE, DRUMS_LEN, DRUMS_SWING, DRUMS_LEVEL, DRUMS_KEYS, DRUMS_MODE, DRUMS_OCT,
    DRUM_TRACK_BASE, DRUM_TRACK_END = DRUM_TRACK_BASE + 8 * 5 - 1,   // 8 tracks x { level, pan, tune, decay, mute }
    DRUM_STEP_BASE, DRUM_STEP_END = DRUM_STEP_BASE + 8 * 16 - 1,     // 8 tracks x 16 steps (off / on / accent)
    SEQ_SOURCE, DRUMS_SOURCE,
    MASTER,
    // the FX rack's second layer (work order 20 Oct, item 4): EQ -> COMP -> DRIVE -> MOD -> ECHO -> REVERB. Appended, so every
    // earlier index keeps its number; a preset without them has them at their defaults (all off: the rack is bit-identical)
    FXQ_LOW, FXQ_MID, FXQ_MIDF, FXQ_HIGH,
    FXC_THRESH, FXC_RATIO, FXC_ATTACK, FXC_RELEASE, FXC_MAKEUP,
    FXM_TYPE, FXM_RATE, FXM_DEPTH, FXM_FB, FXM_MIX,
    FXE_TYPE,
    // the sequencer's 2.0 lanes (work order 20 Oct, item 5): the play direction (forward, reverse, bounce, random), and per step a probability and a ratchet
    SEQ_DIR,
    SEQ_PROB_BASE, SEQ_PROB_END = SEQ_PROB_BASE + 16 - 1,
    SEQ_RATCH_BASE, SEQ_RATCH_END = SEQ_RATCH_BASE + 16 - 1,
    kNumParams
};

constexpr int oscParam (int osc, OscField f) { return OSC_BASE + osc * kOscFields + f; }

constexpr int kNumLfo = 3, kNumModSlots = 16;
enum LfoField : int { L_SHAPE, L_RATE, L_SYNC, L_RETRIG, L_FADE, kLfoFields };
constexpr int lfoParam (int l, LfoField f) { return LFO_BASE + l * kLfoFields + f; }
enum ModField : int { M_SRC, M_DST, M_AMT, kModFields };
constexpr int modParam (int slot, ModField f) { return MOD_BASE + slot * kModFields + f; }

constexpr int kSeqSteps = 16;
enum StepField : int { S_ON, S_NOTE, S_ROW, S_VEL, S_GATE, S_SLIDE, S_ACCENT, kStepFields };
constexpr int stepParam (int step, StepField f) { return SEQ_STEP_BASE + step * kStepFields + f; }
constexpr int stepProbParam (int step) { return SEQ_PROB_BASE + step; }
constexpr int stepRatchetParam (int step) { return SEQ_RATCH_BASE + step; }
constexpr int kDrumTracks = 8, kDrumSteps = 16;
enum DrumTrackField : int { DT_LEVEL, DT_PAN, DT_TUNE, DT_DECAY, DT_MUTE, kDrumTrackFields };
constexpr int drumTrackParam (int track, DrumTrackField f) { return DRUM_TRACK_BASE + track * kDrumTrackFields + f; }
constexpr int drumStepParam (int track, int step) { return DRUM_STEP_BASE + track * kDrumSteps + step; }

enum class LfoShape : int { sine, triangle, saw, square, sampleHold, count };
// tempo divisions for lfo sync (in beats); index 0 = free (Hz)
inline const char* const kSyncNames[] = { "free", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T", "1/4D", "1/8D", "1/16D" };
inline const float kSyncBeats[] = { 0, 16, 8, 4, 2, 1, 0.5f, 0.25f, 0.125f, 2.0f / 3, 1.0f / 3, 1.0f / 6, 1.5f, 0.75f, 0.375f };
enum class ModSrc : int { none, lfo1, lfo2, lfo3, ampEnv, filterEnv, velocity, key, modWheel, aftertouch, random, count };
enum class ModDst : int { none, pitch, osc1Pitch, osc2Pitch, osc3Pitch, osc1Level, osc2Level, osc3Level, osc1Pw, osc2Pw, osc3Pw,
                          cutoff, reso, drive, fm, pan, amp, lfo1Rate, lfo2Rate, lfo3Rate, detune, count };
inline const char* const kModSrcNames[] = { "none", "lfo1", "lfo2", "lfo3", "amp_env", "filter_env", "velocity", "key", "mod_wheel",
                                            "aftertouch", "random" };
inline const char* const kModDstNames[] = { "none", "pitch", "osc1_pitch", "osc2_pitch", "osc3_pitch", "osc1_level", "osc2_level",
                                            "osc3_level", "osc1_pw", "osc2_pw", "osc3_pw", "cutoff", "reso", "drive", "fm", "pan", "amp",
                                            "lfo1_rate", "lfo2_rate", "lfo3_rate", "unison_detune" };
// full-scale range of each destination at amount 1 (units in the comment)
inline const float kModDstRange[] = { 0, 24 /*st*/, 24, 24, 24, 1 /*x level*/, 1, 1, 0.45f /*pw*/, 0.45f, 0.45f, 8 /*oct*/,
                                      1 /*reso*/, 36 /*dB*/, 10 /*index*/, 1 /*pan*/, 1 /*x amp*/, 4 /*oct*/, 4, 4, 1 /*x detune*/ };
inline const char* const kLfoShapeNames[] = { "sine", "triangle", "saw", "square", "sample_hold" };

struct ParamDef
{
    std::string id;
    bool isEnum = false;
    float min = 0, max = 1, def = 0;
    const char* const* names = nullptr;   // enum value names (index = value)
    int numNames = 0;
    const char* unit = "";
};

inline const std::array<ParamDef, kNumParams>& paramDefs()
{
    static const std::array<ParamDef, kNumParams> defs = []
    {
        std::array<ParamDef, kNumParams> d {};
        auto F = [&] (int i, const std::string& id, float mn, float mx, float df, const char* unit)
        { d[(size_t) i] = ParamDef { id, false, mn, mx, df, nullptr, 0, unit }; };
        auto E = [&] (int i, const std::string& id, const char* const* names, int n, int df)
        { d[(size_t) i] = ParamDef { id, true, 0, (float) (n - 1), (float) df, names, n, "" }; };

        for (int o = 0; o < kNumOsc; ++o)
        {
            const std::string p = "osc" + std::to_string (o + 1) + ".";
            E (oscParam (o, O_TYPE), p + "type", kOscTypeNames, (int) OscType::count, o == 0 ? (int) OscType::saw : (int) OscType::off);
            F (oscParam (o, O_LEVEL), p + "level", 0, 1, 0.8f, "");
            F (oscParam (o, O_OCT), p + "octave", -3, 3, 0, "oct");
            F (oscParam (o, O_SEMI), p + "semi", -12, 12, 0, "st");
            F (oscParam (o, O_FINE), p + "fine", -100, 100, 0, "ct");
            F (oscParam (o, O_PW), p + "pw", 0.05f, 0.95f, 0.5f, "");
            F (oscParam (o, O_PAN), p + "pan", -1, 1, 0, "");
            F (oscParam (o, O_KS_DECAY), p + "string_decay", 0.05f, 20, 3, "s");
            F (oscParam (o, O_KS_TONE), p + "string_tone", 0, 1, 0.5f, "");
            F (oscParam (o, O_DECAY), p + "decay", 0, 20, 0, "s");   // own fade to -60 dB after the note starts; 0 = none
            F (oscParam (o, O_UNI), p + "unison", 1, 8, 1, "");       // VA shapes only: stacked copies, detuned and spread
            F (oscParam (o, O_DETUNE), p + "unison_detune", 0, 100, 20, "ct");   // outermost copies are +- half of this
            F (oscParam (o, O_SPREAD), p + "unison_spread", 0, 1, 0.7f, "");
        }
        E (FLT_MODE, "filter.mode", kFilterModeNames, (int) FilterMode::count, (int) FilterMode::lp);
        F (FLT_CUTOFF, "filter.cutoff", 20, 20000, 12000, "Hz");
        F (FLT_RESO, "filter.reso", 0, 1, 0.1f, "");
        F (FLT_DRIVE, "filter.drive", 0, 36, 0, "dB");
        F (FLT_KEYTRACK, "filter.keytrack", 0, 1, 0, "");
        F (FLT_ENVAMT, "filter.env_amount", -8, 8, 0, "oct");
        F (FLT_VELAMT, "filter.vel_amount", 0, 4, 0, "oct");

        auto env = [&] (int base, const std::string& p, float a, float dd, float s, float r)
        {
            F (base + 0, p + "attack", 0, 20, a, "s");
            F (base + 1, p + "decay", 0.001f, 20, dd, "s");
            F (base + 2, p + "sustain", 0, 1, s, "");
            F (base + 3, p + "release", 0.001f, 20, r, "s");
            F (base + 4, p + "attack_curve", -1, 1, 0, "");
            F (base + 5, p + "decay_curve", -1, 1, 0.5f, "");
            F (base + 6, p + "release_curve", -1, 1, 0.5f, "");
        };
        env (AENV_A, "amp_env.", 0.003f, 0.3f, 1.0f, 0.15f);
        env (FENV_A, "filter_env.", 0.003f, 0.4f, 0.0f, 0.2f);

        E (V_MODE, "voice.mode", kVoiceModeNames, (int) VoiceMode::count, (int) VoiceMode::poly);
        F (V_POLY, "voice.polyphony", 1, 16, 16, "");
        F (V_GLIDE, "voice.glide", 0, 5, 0, "s");
        F (V_VELAMP, "voice.vel_to_amp", 0, 1, 0.7f, "");
        F (V_BEND, "voice.bend_range", 0, 24, 2, "st");
        F (PENV_AMT, "pitch_env.amount", -48, 48, 0, "st");      // added to the pitch at note start, falls to 10 % in decay
        F (PENV_DECAY, "pitch_env.decay", 0.001f, 5, 0.05f, "s");
        F (CLICK_LEVEL, "click.level", 0, 1, 0, "");            // noise burst after the filter, through a high-pass
        F (CLICK_DECAY, "click.decay", 0.0005f, 0.1f, 0.006f, "s");
        F (CLICK_HP, "click.tone", 200, 16000, 3000, "Hz");
        E (DRUM_MODE, "drum.mode", kOffOnNames, 2, 0);           // pitch = drum.pitch + keytrack x (key - 60)
        F (DRUM_PITCH, "drum.pitch", 0, 127, 36, "st");
        F (DRUM_KEYTRACK, "drum.keytrack", 0, 1, 0, "");
        F (CHOKE, "voice.choke", 0, 8, 0, "");                   // > 0: a new note cuts the voices that sound (same group)
        E (DRUM_ONESHOT, "drum.one_shot", kOffOnNames, 2, 0);    // on: note-off is ignored, the amp envelope skips sustain
        F (PENV_VEL, "pitch_env.velocity", 0, 1, 0, "");          // how much velocity scales the pitch envelope (1 = soft hits ~none)
        F (CLICK_VEL, "click.velocity", 0, 1, 0.6f, "");         // how much velocity scales the click (0.6 = the old fixed law)
        F (DRIFT_CENTS, "drift.pitch", 0, 50, 0, "ct");         // slow random pitch movement per voice
        F (DRIFT_AMP, "drift.level", 0, 6, 0, "dB");            // slow random level movement per voice
        F (DRIFT_RATE, "drift.rate", 0.05f, 20, 3, "Hz");
        F (FLOOR_LEVEL, "noise.floor", 0, 0.5f, 0, "");          // stereo pink noise under the note, before the filter
        for (int l = 0; l < kNumLfo; ++l)
        {
            const std::string p = "lfo" + std::to_string (l + 1) + ".";
            E (lfoParam (l, L_SHAPE), p + "shape", kLfoShapeNames, (int) LfoShape::count, 0);
            F (lfoParam (l, L_RATE), p + "rate", 0.01f, 100, 2, "Hz");   // up to 100 Hz: clap bursts, rough tremolo
            E (lfoParam (l, L_SYNC), p + "sync", kSyncNames, 15, 0);
            E (lfoParam (l, L_RETRIG), p + "retrigger", kOffOnNames, 2, 1);
            F (lfoParam (l, L_FADE), p + "fade", 0, 10, 0, "s");
        }
        for (int m = 0; m < kNumModSlots; ++m)
        {
            const std::string p = "mod" + std::to_string (m + 1) + ".";
            E (modParam (m, M_SRC), p + "source", kModSrcNames, (int) ModSrc::count, 0);
            E (modParam (m, M_DST), p + "destination", kModDstNames, (int) ModDst::count, 0);
            F (modParam (m, M_AMT), p + "amount", -1, 1, 0, "");
        }
        F (FM_AMOUNT, "fm.amount", 0, 10, 0, "");                // osc 2 phase-modulates the target (index, radians)
        E (FM_TARGET, "fm.target", kFmTargetNames, 3, 0);
        // Tuning. normal: every key is its usual note (any MIDI keyboard, controller or computer keyboard plays as usual).
        // micro: each pitch class sits on one of three rows (low / mid / high); a row is a whole number of grid steps away
        // (grid = octave / edo). Keyswitches (24 keys from tuning.keyswitch_low) move a pitch class to low / high from a
        // normal keyboard: first 12 keys toggle "low", next 12 toggle "high". Reference = A4 in Hz, both modes.
        E (TUNE_MODE, "tuning.mode", kTuneModeNames, 2, 0);
        F (TUNE_EDO, "tuning.edo", 5, 96, 53, "steps");
        F (TUNE_REF, "tuning.reference", 415, 466, 440, "Hz");
        F (ROW_UP, "tuning.row_up", 0, 12, 1, "steps");
        F (ROW_DOWN, "tuning.row_down", 0, 12, 1, "steps");
        for (int pc = 0; pc < 12; ++pc) E (TUNE_ROW0 + pc, std::string ("tuning.row.") + kPitchClassIds[pc], kRowNames, 3, 1);
        E (KSW_MODE, "tuning.keyswitch", kOffOnNames, 2, 0);
        F (KSW_LOW, "tuning.keyswitch_low", 0, 104, 24, "key");
        // FX rack after the voices: drive -> echo -> reverb (SvlFx.h). Every mix 0 = the rack is not run.
        F (FXD_AMOUNT, "fx.drive.amount", 0, 1, 0, "");
        F (FXD_TONE, "fx.drive.tone", 0, 1, 0.5f, "");
        F (FXD_MIX, "fx.drive.mix", 0, 1, 1, "");
        E (FXE_TIME, "fx.echo.time", kEchoTimeNames, 6, 2);        // synced to the host tempo (120 BPM standalone)
        F (FXE_FB, "fx.echo.feedback", 0, 0.95f, 0.35f, "");
        F (FXE_DAMP, "fx.echo.damp", 0, 1, 0.5f, "");
        F (FXE_MIX, "fx.echo.mix", 0, 1, 0, "");
        F (FXR_SIZE, "fx.reverb.size", 0, 1, 0.5f, "");
        F (FXR_DECAY, "fx.reverb.decay", 0.2f, 12, 2.0f, "s");
        F (FXR_PRE, "fx.reverb.predelay", 0, 200, 15, "ms");
        F (FXR_DAMP, "fx.reverb.damp", 0, 1, 0.5f, "");
        F (FXR_MIX, "fx.reverb.mix", 0, 1, 0, "");
        // Arpeggiator and step sequencer (SvlArpSeq.h), in front of the voices, on the host's tempo and bar grid (own clock at
        // 120 BPM when there is no host transport). Swing delays every second step by swing x half a step (0.667 = triplet feel).
        // Arp + seq both on: the arp gives the note order, the seq gives the rhythm (its rate, rests, velocity, gate, slide,
        // accent) and transposes each arp note by the step's note; arp mode chord + seq = every step that is on plays all held keys.
        // arp.latch keeps the keys after release (arp and seq hold).
        E (ARP_ON, "arp.on", kOffOnNames, 2, 0);
        E (ARP_MODE, "arp.mode", kArpModeNames, 7, 0);
        F (ARP_OCT, "arp.octaves", 1, 4, 1, "oct");
        E (ARP_RATE, "arp.rate", kStepRateNames, 6, 3);
        F (ARP_GATE, "arp.gate", 0.05f, 1, 0.6f, "");              // of the step
        F (ARP_SWING, "arp.swing", 0, 0.75f, 0, "");
        E (ARP_LATCH, "arp.latch", kOffOnNames, 2, 0);
        E (ARP_CHORD, "arp.chord", kArpChordNames, 12, 0);   // one key held plays a chord shape (owner 18 Oct: "kombinasyon çalması lazım")
        E (SEQ_ON, "seq.on", kOffOnNames, 2, 0);
        E (SEQ_RATE, "seq.rate", kStepRateNames, 6, 3);
        F (SEQ_LEN, "seq.length", 1, 16, 16, "steps");
        F (SEQ_SWING, "seq.swing", 0, 0.75f, 0, "");
        E (SEQ_MODE, "seq.mode", kSeqModeNames, 2, 0);              // hold: plays while a key is held, transposed by it (step note 0 =
                                                                    // the key); free: runs on its own from MIDI 60
        for (int st = 0; st < kSeqSteps; ++st)
        {
            const std::string p = "seq.s" + std::to_string (st + 1) + ".";
            E (stepParam (st, S_ON), p + "on", kOffOnNames, 2, 1);
            F (stepParam (st, S_NOTE), p + "note", -24, 24, 0, "st");
            E (stepParam (st, S_ROW), p + "row", kRowNames, 3, 1);       // low / high: tuning.row_down / row_up grid steps away
            F (stepParam (st, S_VEL), p + "vel", 0, 1, 0.8f, "");
            F (stepParam (st, S_GATE), p + "gate", 0.05f, 4, 0.6f, "steps");   // >= 1 ties into the next step (legato in mono)
            E (stepParam (st, S_SLIDE), p + "slide", kOffOnNames, 2, 0);       // hold into the next note and glide to it, no retrigger
            E (stepParam (st, S_ACCENT), p + "accent", kOffOnNames, 2, 0);     // velocity + 0.25
        }
        // Drum machine (SvlDrums.h): 8 tracks, each a full drum sound from a drum preset (the kit is the preset's <drumkit> line),
        // on the same clock as the seq; the tracks mix in after master.level, before the FX rack. drums.keys: MIDI 36..43 hit
        // tracks 1..8 (and do not play the synth) while the drum machine is on.
        E (DRUMS_ON, "drums.on", kOffOnNames, 2, 0);
        E (DRUMS_RATE, "drums.rate", kStepRateNames, 6, 3);
        F (DRUMS_LEN, "drums.length", 1, 16, 16, "steps");
        F (DRUMS_SWING, "drums.swing", 0, 0.75f, 0, "");
        F (DRUMS_LEVEL, "drums.level", -60, 6, 0, "dB");
        E (DRUMS_KEYS, "drums.keys", kOffOnNames, 2, 0);
        E (DRUMS_MODE, "drums.mode", kDrumModeNames, 2, 0);
        F (DRUMS_OCT, "drums.octave", -2, 2, 0, "oct");   // whole octaves (rounded), the whole kit, on top of each drums.tN.tune   // hold: runs while a key is held (owner 18 Oct); free: runs on its own
        for (int t = 0; t < kDrumTracks; ++t)
        {
            const std::string p = "drums.t" + std::to_string (t + 1) + ".";
            F (drumTrackParam (t, DT_LEVEL), p + "level", -60, 6, 0, "dB");
            F (drumTrackParam (t, DT_PAN), p + "pan", -1, 1, 0, "");
            F (drumTrackParam (t, DT_TUNE), p + "tune", -24, 24, 0, "st");
            F (drumTrackParam (t, DT_DECAY), p + "decay", 0.25f, 4, 1, "x");   // scales the sound's amp decay / release, osc decays, note length
            E (drumTrackParam (t, DT_MUTE), p + "mute", kOffOnNames, 2, 0);
            for (int st = 0; st < kDrumSteps; ++st)
                E (drumStepParam (t, st), p + "s" + std::to_string (st + 1), kDrumStepNames, 3, 0);
        }
        E (SEQ_SOURCE, "daw.seq", kSourceNames, 2, 0);        // midi: the arp and the seq are off, the DAW's notes play the voices
        E (DRUMS_SOURCE, "daw.drums", kSourceNames, 2, 0);    // midi: the pattern is silent, notes 36..43 hit tracks 1..8
        F (MASTER, "master.level", -60, 6, -6, "dB");
        // EQ: low shelf 120 Hz, a peak (Q 0.9) at mid_freq, high shelf 6 kHz; 0 dB everywhere = not run
        F (FXQ_LOW, "fx.eq.low", -12, 12, 0, "dB");
        F (FXQ_MID, "fx.eq.mid", -12, 12, 0, "dB");
        F (FXQ_MIDF, "fx.eq.mid_freq", 200, 8000, 1000, "Hz");
        F (FXQ_HIGH, "fx.eq.high", -12, 12, 0, "dB");
        // COMP: a stereo-linked feed-forward compressor (soft knee 6 dB); ratio 1 and makeup 0 = not run
        F (FXC_THRESH, "fx.comp.threshold", -40, 0, -12, "dB");
        F (FXC_RATIO, "fx.comp.ratio", 1, 20, 1, "");
        F (FXC_ATTACK, "fx.comp.attack", 0.0005f, 0.1f, 0.01f, "s");
        F (FXC_RELEASE, "fx.comp.release", 0.02f, 1.0f, 0.15f, "s");
        F (FXC_MAKEUP, "fx.comp.makeup", 0, 24, 0, "dB");
        // MOD: chorus / flanger / phaser; off = not run
        E (FXM_TYPE, "fx.mod.type", kModFxNames, 4, 0);
        F (FXM_RATE, "fx.mod.rate", 0.05f, 8, 0.6f, "Hz");
        F (FXM_DEPTH, "fx.mod.depth", 0, 1, 0.5f, "");
        F (FXM_FB, "fx.mod.feedback", 0, 0.9f, 0.3f, "");
        F (FXM_MIX, "fx.mod.mix", 0, 1, 0.5f, "");
        // ECHO type: ping-pong (the 1.0 echo), stereo (two lines), mono, tape (mono, wow and a darker loop)
        E (FXE_TYPE, "fx.echo.type", kEchoTypeNames, 4, 0);
        // the sequencer's lanes: direction (backward, ping-pong, random by the step's number, so a bar plays the same on every
        // render); PROB = the chance a step that is on plays (decided by the step's number too); RATCHET = the step plays 1..4
        // times in its own length. Defaults (forward, 1, 1) = the 1.0 sequencer exactly.
        E (SEQ_DIR, "seq.direction", kSeqDirNames, 4, 0);
        for (int st = 0; st < kSeqSteps; ++st)
        {
            const std::string p = "seq.s" + std::to_string (st + 1) + ".";
            F (stepProbParam (st), p + "prob", 0, 1, 1, "");
            F (stepRatchetParam (st), p + "ratchet", 1, 4, 1, "x");
        }
        return d;
    }();
    return defs;
}

inline int findParam (const std::string& id)
{
    const auto& d = paramDefs();
    for (int i = 0; i < kNumParams; ++i) if (d[(size_t) i].id == id) return i;
    return -1;
}

struct ParamSet
{
    std::array<float, kNumParams> v {};
    ParamSet() { reset(); }
    void reset() { const auto& d = paramDefs(); for (int i = 0; i < kNumParams; ++i) v[(size_t) i] = d[(size_t) i].def; }
    float operator[] (int i) const { return v[(size_t) i]; }
    float& operator[] (int i) { return v[(size_t) i]; }
};

inline float clampParam (int i, float x)
{
    const auto& d = paramDefs()[(size_t) i];
    if (! (x == x)) return d.def;   // NaN
    if (d.isEnum) x = (float) (int) (x + 0.5f);
    return x < d.min ? d.min : (x > d.max ? d.max : x);
}

} // namespace svl

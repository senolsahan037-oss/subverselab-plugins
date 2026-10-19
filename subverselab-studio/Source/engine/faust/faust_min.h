// The few Faust names the generated classes need (no Faust install required to build the engine). Plain C++.
#pragma once
#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif
#include <cstring>

struct Meta { virtual ~Meta() = default; virtual void declare (const char*, const char*) {} };
struct Soundfile;
struct UI
{
    virtual ~UI() = default;
    virtual void openTabBox (const char*) {}
    virtual void openHorizontalBox (const char*) {}
    virtual void openVerticalBox (const char*) {}
    virtual void closeBox() {}
    virtual void addButton (const char*, FAUSTFLOAT*) {}
    virtual void addCheckButton (const char*, FAUSTFLOAT*) {}
    virtual void addVerticalSlider (const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) { zone (l, z); }
    virtual void addHorizontalSlider (const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) { zone (l, z); }
    virtual void addNumEntry (const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) { zone (l, z); }
    virtual void addHorizontalBargraph (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) {}
    virtual void addVerticalBargraph (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) {}
    virtual void addSoundfile (const char*, const char*, Soundfile**) {}
    virtual void declare (FAUSTFLOAT*, const char*, const char*) {}
    virtual void zone (const char*, FAUSTFLOAT*) {}
};

/** finds a control's value pointer by its label (set up in prepare(), never on the audio thread) */
struct ZoneFinder : UI
{
    const char* want = nullptr;
    FAUSTFLOAT* found = nullptr;
    void zone (const char* l, FAUSTFLOAT* z) override { if (want != nullptr && std::strcmp (l, want) == 0) found = z; }
};

class FaustDsp { public: virtual ~FaustDsp() = default; };

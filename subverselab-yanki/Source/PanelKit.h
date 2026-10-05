#pragma once

#include <JuceHeader.h>

/*  The family drawing kit, ported 1:1 from the approved concept generator
    (subverselab-koro/tools/concepts.py, imported by subverselab-yanki/tools/
    concepts.py): plate, rack ears, screws, grooves, section brackets, the
    stroked product name, machined and bakelite pointer knobs, lamps, the bat
    toggle, keys, 7-segment digits and the model box. Every function draws in
    the concept's base units (the 1600 x 640 face), so the panel code can use
    the generator's coordinates unchanged; the caller sets the scale.

    Angles are degrees clockwise from 12 o'clock, as in the generator.
*/
namespace kit
{
using juce::Colour;
using juce::Graphics;
using juce::Point;
using juce::String;

struct Box
{
    float x0, y0, x1, y1;
    juce::Rectangle<float> rect() const { return { x0, y0, x1 - x0, y1 - y0 }; }
    Box expanded (float d) const { return { x0 - d, y0 - d, x1 + d, y1 + d }; }
};

inline Colour rgb (int r, int g, int b, int a = 255) { return Colour ((juce::uint8) r, (juce::uint8) g, (juce::uint8) b, (juce::uint8) a); }
inline String U (const char* utf8) { return String::fromUTF8 (utf8); }

// Palette (concepts.py)
const Colour CREAM = rgb (242, 239, 231), INK = rgb (3, 55, 51), GOLD = rgb (197, 160, 89), GOLD_L = rgb (223, 191, 122),
             GOLD_D = rgb (140, 111, 53), LAMP = rgb (246, 213, 142), MUTED = rgb (143, 170, 165), LED_RED = rgb (255, 64, 40),
             LED_RED_OFF = rgb (70, 14, 10), INK2 = rgb (73, 108, 104), SWELL_RED = rgb (236, 104, 78);

enum class Face { legend, fine, mono };

Point<float> polar (float cx, float cy, float r, float deg);
juce::Font font (Face, float size);

/** Text as the generator sets it: anchor "lm" / "mm" / "rm" (middle =
    halfway between ascender and descender), optional tracking in units. */
float text (Graphics&, float x, float y, const String&, Face, float size, Colour, const char* anchor = "mm", float track = 0.0f);
float tlen (const String&, Face, float size, float track = 0.0f);
/** Plate legend: cream silkscreen. */
float lt (Graphics&, float x, float y, const String&, float size = 10.0f, float track = 2.0f, Face = Face::legend,
          const char* anchor = "mm", Colour col = CREAM);

void line (Graphics&, Point<float> a, Point<float> b, float width, Colour);
void poly (Graphics&, const std::vector<Point<float>>&, Colour);
void rrect (Graphics&, Box, float radius, std::optional<Colour> fill, std::optional<Colour> outline = {}, float width = 1.0f);
void ellipse (Graphics&, Box, std::optional<Colour> fill, std::optional<Colour> outline = {}, float width = 1.0f);
/** Soft light or shadow: a blurred ellipse or rounded rectangle. */
void soft (Graphics&, Box, float blur, Colour colour = juce::Colours::black, float opacity = 0.5f, Point<float> offset = {},
           bool isEllipse = true, float radius = 0.0f);
/** Linear gradient across the box at an angle (90 = top to bottom), filling an ellipse or rounded rectangle. */
void grad (Graphics&, Box, Colour c0, Colour c1, float angle = 90.0f, bool isEllipse = true, float radius = 0.0f);
void glowPolys (Graphics&, const std::vector<std::vector<Point<float>>>&, Colour, float blur, float strength);

/** The brushed anodised plate, written straight into an image at `scale` pixels per unit. */
void plate (juce::Image&, float scale, Colour top, Colour bottom, float streak, float grain, int seed);

void screw (Graphics&, float cx, float cy, float r = 9.0f);
void rack (Graphics&);
void groove (Graphics&, float y, float x0 = 64.0f, float x1 = 1536.0f);
void bracket (Graphics&, float x0, float x1, float y, const String& title, float size = 10.5f);
float wordmark (Graphics&, const String& name, float x, float y, float h, Colour);

/** Knob scale (ticks and the three numbers) and the name/sub legends: the static print around a knob. */
void knobScale (Graphics&, float cx, float cy, float r, const juce::StringArray& nums, bool ticks = true,
                const String& label = {}, float lsize = 10.5f, const String& sub = {});
/** The machined knob itself at a value 0..1 (-135 .. +135 degrees). */
void knobBody (Graphics&, float cx, float cy, float r, float val);
void knobMarks (Graphics&, float cx, float cy, float r, const std::vector<std::pair<float, String>>& marks, float size = 7.5f);
void pointerScale (Graphics&, float cx, float cy, float r, const juce::StringArray& labels, const std::vector<float>& angles,
                   float tsize, float ring, const String& label = {}, float lsize = 10.5f);
void pointerBody (Graphics&, float cx, float cy, float r, float angle);

void lamp (Graphics&, float cx, float cy, float r, bool on, Colour col = LAMP, Colour off = rgb (70, 52, 22));
inline void redLed (Graphics& g, float cx, float cy, float r, bool on) { lamp (g, cx, cy, r, on, LED_RED, rgb (80, 18, 12)); }
void toggle (Graphics&, float cx, float cy, bool on, float s = 1.0f);
enum class KeyStyle { cream, dark };
void button (Graphics&, Box, const String& text, bool lit, KeyStyle, float size, float track);
/** 7-segment digits; a '.' after a digit lights that cell's decimal point. */
float seven (Graphics&, float x, float y, float h, const String& s, float pitch);
void modelBox (Graphics&, float x, float y, const String& text, float size = 10.0f);
} // namespace kit

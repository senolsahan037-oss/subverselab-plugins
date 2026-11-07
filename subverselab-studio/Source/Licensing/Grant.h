#pragma once
// A grant is what the server signs about this computer's rights. JING FANG takes only "owned" grants (no trial). Copied from
// AHENK (subverselab-fazilsay/Source/Licensing/Grant.h, 7 Nov 2026) under the jf namespace. Contract:
// 01_AI_Tools/subverselab-plugin-common/docs/TRIAL_AND_SIGNIN.md ("Signed grants").
// Wire form {"payload":"<compact JSON>","sig":"<hex>"}; sig = m^d mod n with m = SHA-256(payload) as a big-endian integer.
// Verified here with juce::RSAKey ("e,n" in hex): applyToValue (sig) must give m back. A file edited by hand fails.
#include <JuceHeader.h>

namespace jf::rights
{
struct Grant
{
    bool valid = false;                  // signature good and the payload well formed
    juce::String product, machine, kind, uid;
    juce::int64 issued = 0, starts = 0, ends = 0;
    juce::String payload, sig;           // as received: written back to disk unchanged
};

inline bool verifySignature (const juce::String& payload, const juce::String& sigHex, const juce::String& publicKeyJuce)
{
    if (payload.isEmpty() || sigHex.isEmpty() || sigHex.length() > 1024 || ! sigHex.containsOnly ("0123456789abcdefABCDEF")
        || ! publicKeyJuce.containsChar (','))
        return false;
    const juce::RSAKey key (publicKeyJuce);
    juce::BigInteger s;
    s.parseString (sigHex, 16);
    if (s.isZero()) return false;
    if (! key.applyToValue (s)) return false;
    const auto utf8 = payload.toUTF8();
    juce::BigInteger m;
    m.parseString (juce::SHA256 (utf8.getAddress(), utf8.sizeInBytes() - 1).toHexString(), 16);
    return ! m.isZero() && s == m;
}

inline juce::int64 int64Of (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble() ? (juce::int64) v : 0; }

// Parses and verifies the wire form. valid = false for anything that does not check out (the caller keeps nothing).
inline Grant parseGrant (const juce::var& wire, const juce::String& publicKeyJuce)
{
    Grant g;
    if (! wire.isObject()) return g;
    g.payload = wire.getProperty ("payload", {}).toString();
    g.sig = wire.getProperty ("sig", {}).toString();
    if (! verifySignature (g.payload, g.sig, publicKeyJuce)) return g;
    const auto p = juce::JSON::parse (g.payload);
    if (! p.isObject() || int64Of (p.getProperty ("v", 0)) != 1) return g;
    g.product = p.getProperty ("product", {}).toString();
    g.machine = p.getProperty ("machine", {}).toString();
    g.kind = p.getProperty ("kind", {}).toString();
    g.uid = p.getProperty ("uid", {}).toString();
    g.issued = int64Of (p.getProperty ("issued", 0));
    g.starts = int64Of (p.getProperty ("starts", 0));
    g.ends = int64Of (p.getProperty ("ends", 0));
    g.valid = (g.kind == "trial" || g.kind == "owned") && g.product.isNotEmpty() && g.machine.isNotEmpty() && g.ends > g.starts;
    return g;
}

inline Grant parseGrantText (const juce::String& json, const juce::String& publicKeyJuce) { return parseGrant (juce::JSON::parse (json), publicKeyJuce); }

inline juce::String wireOf (const Grant& g)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("payload", g.payload);
    o->setProperty ("sig", g.sig);
    return juce::JSON::toString (juce::var (o), true);
}
} // namespace jf::rights

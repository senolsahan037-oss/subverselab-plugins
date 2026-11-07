#pragma once
// JING FANG's rights on this computer: owned (signed in with the subverselab.com account that bought it) or locked.
// Owner 7 Nov 2026: "sadece satın alanlar kullansın" = only buyers use it: no trial, no free mode. Locked = the panel shows
// the sign-in and the plug-in makes no sound (the processor's job); a project keeps its settings.
//
// Contract: subverselab-plugin-common/docs/TRIAL_AND_SIGNIN.md (the browser sign-in, signed grants, grant refresh). This is
// AHENK's Rights (subverselab-fazilsay/Source/Licensing/Rights.h) without the trial: it never calls /api/trial/start (the
// server answers "unknown product" for jingfang there by design) and refuses a trial grant. Nobody types a key.
//
// Everything kept on disk is one server-signed grant (Grant.h) in <data folder>/Rights/owned.json; a hand-edited file fails
// the check. An owned grant lasts 45 days offline; the plug-in asks for a fresh one when it is older than 3 days and the
// network is there. A refund: the refresh answers "not owned" and the grant is dropped (locked again).
// Network calls run on their own threads and never block the panel or the audio thread.
#include <JuceHeader.h>

#include <functional>
#include <memory>

#include "Grant.h"

// 1 only in the offline check tools (CMake jf_tool); the plug-in target never defines it
#ifndef JF_RIGHTS_TESTS
 #define JF_RIGHTS_TESTS 0
#endif

namespace jf::rights
{
class Rights
{
public:
    enum class State { owned, locked };

    struct Config
    {
        juce::String product { "jingfang" };
        juce::String apiBase { "https://subverselab.com/api/" };
        juce::String publicKey;          // JUCE "e,n"
        juce::File folder;               // where owned.json lives
        juce::String machine;            // empty = machineId()
        bool network = true;             // false: the offline checks
        std::function<juce::int64()> clock;                  // empty = the system clock (ms)
        std::function<void (const juce::String&)> openUrl;   // empty = the default browser
       #if JF_RIGHTS_TESTS
        bool alwaysOwned = false;        // the offline check tools only (never compiled into the plug-in)
       #endif
    };

    static Config defaultConfig();       // the product's own: compiled-in production key, the data folder, the live API
    static juce::File defaultFolder();   // <Application Support>/SubverseLab/JING FANG/Rights (Windows: %LOCALAPPDATA%\SubverseLab\...)
    static std::shared_ptr<Rights> shared();   // one per process (every JING FANG in a session shares it), made on first use

    explicit Rights (Config);
    ~Rights();

    State state() const;
    bool owned() const { return state() == State::owned; }

    // Background check with the server: refreshes an owned grant when it is older than 3 days. Does nothing without an owned
    // grant. Safe to call often: one request in flight at most, and at most one every few minutes unless forced.
    void refreshAsync (bool force = false);
    bool refreshDue() const;             // an owned grant older than 3 days (the next online check asks for a new one)
    // The refresh's answer (message thread): 200 + ok -> the new grant; 200 + "not owned" (a refund) -> locked; anything else
    // (no network, 503 plain text, a malformed answer) -> nothing changes. Public so the offline checks can feed it answers.
    void applyRefreshReply (int status, const juce::String& reply);

    // Sign in through the browser (device flow): idle -> starting -> waiting (userCode shown, browser opened) -> done | failed.
    struct SignIn
    {
        enum class Phase { idle, starting, waiting, done, failed } phase = Phase::idle;
        juce::String userCode, verifyUrl, message;
    };
    void startSignIn();
    void cancelSignIn();
    SignIn signIn() const;
    void openVerifyPageAgain() const;    // the waiting phase's page, once more

    // Verifies and stores an owned grant (wire JSON). false = refused (bad signature, other product or computer, a trial).
    bool adopt (const juce::String& wireJson);
    void forgetOwned();                  // the server said the product is no longer owned (refund)

    std::function<void()> onChange;      // message thread: the state or the sign-in phase changed

    static juce::String machineId();     // svl-machine:<first 32 hex of SHA-256(device id)>
    juce::int64 now() const;
    const Config& config() const { return cfg; }
    static juce::String buyUrl() { return "https://subverselab.com/plugins/jingfang"; }

   #if JF_RIGHTS_TESTS
    void setSignInForTest (SignIn);      // the snapshot tool draws every phase without a network
   #endif

    static constexpr juce::int64 kDay = 86400000;
    static constexpr juce::int64 kRefreshAfter = 3 * kDay;

private:
    struct Shared;                       // what the worker threads may still touch after this object is gone
    void load();
    void notify();
    juce::File file (const char* name) const { return cfg.folder.getChildFile (name); }
    static juce::String post (const juce::String& url, const juce::var& body, int* status);

    Config cfg;
    std::shared_ptr<Shared> sh;
    Grant ownedGrant;
    juce::int64 lastRefresh = 0;
    mutable juce::CriticalSection lock;
    JUCE_DECLARE_NON_COPYABLE (Rights)
};
} // namespace jf::rights

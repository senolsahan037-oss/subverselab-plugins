#include "Rights.h"

#include "GrantKey.h"

#include <mutex>

namespace jf::rights
{
struct Rights::Shared
{
    std::atomic<bool> alive { true };
    Rights* owner = nullptr;                 // valid while alive (checked on the message thread only)
    std::atomic<bool> busy { false };        // a refresh request in flight
    std::atomic<bool> cancel { false };      // sign-in polling
    std::atomic<int> signInRun { 0 };        // which sign-in a worker belongs to (a new one makes the old stop)
    juce::CriticalSection lock;
    SignIn signIn;
};

namespace
{
juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> kv)
{
    auto* o = new juce::DynamicObject();
    for (auto& p : kv) o->setProperty (p.first, p.second);
    return juce::var (o);
}
}

juce::File Rights::defaultFolder()
{
   #if JUCE_WINDOWS
    auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
   #else
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");   // JUCE's userApplicationDataDirectory is ~/Library on macOS
   #endif
    return base.getChildFile ("SubverseLab").getChildFile ("JING FANG").getChildFile ("Rights");
}

Rights::Config Rights::defaultConfig()
{
    Config c;                                   // the live API; no environment variable changes it or the key
    c.publicKey = grantPublicKey();
    c.folder = defaultFolder();
    return c;
}

std::shared_ptr<Rights> Rights::shared()
{
    static std::mutex m;
    static std::weak_ptr<Rights> one;
    const std::lock_guard<std::mutex> g (m);
    auto r = one.lock();
    if (r == nullptr) { r = std::make_shared<Rights> (defaultConfig()); one = r; }
    return r;
}

Rights::Rights (Config c) : cfg (std::move (c)), sh (std::make_shared<Shared>())
{
    sh->owner = this;
    if (cfg.machine.isEmpty()) cfg.machine = machineId();
    load();
    if (cfg.network) refreshAsync (true);
}

Rights::~Rights()
{
    sh->alive = false;
    sh->cancel = true;
}

juce::String Rights::machineId()
{
    const auto id = juce::SystemStats::getUniqueDeviceID();
    const auto utf8 = id.toUTF8();
    return "svl-machine:" + juce::SHA256 (utf8.getAddress(), utf8.sizeInBytes() - 1).toHexString().substring (0, 32);
}

juce::int64 Rights::now() const { return cfg.clock ? cfg.clock() : juce::Time::currentTimeMillis(); }

void Rights::load()
{
    const juce::ScopedLock sl (lock);
   #if JF_RIGHTS_TESTS
    if (cfg.alwaysOwned) return;
   #endif
    cfg.folder.createDirectory();
    const auto o = parseGrantText (file ("owned.json").loadFileAsString(), cfg.publicKey);
    ownedGrant = o.valid && o.kind == "owned" && o.product == cfg.product && o.machine == cfg.machine ? o : Grant();
}

Rights::State Rights::state() const
{
   #if JF_RIGHTS_TESTS
    if (cfg.alwaysOwned) return State::owned;
   #endif
    const juce::ScopedLock sl (lock);
    const auto t = now();
    // a clock set back before the grant was issued does not stretch the 45 days: owned only inside [starts, ends)
    return ownedGrant.valid && t >= ownedGrant.starts - kDay && t < ownedGrant.ends ? State::owned : State::locked;
}

bool Rights::adopt (const juce::String& wireJson)
{
    const auto g = parseGrantText (wireJson, cfg.publicKey);
    if (! g.valid || g.kind != "owned" || g.product != cfg.product || g.machine != cfg.machine) return false;
    {
        const juce::ScopedLock sl (lock);
        ownedGrant = g;
        cfg.folder.createDirectory();
        file ("owned.json").replaceWithText (wireOf (g));
    }
    notify();
    return true;
}

void Rights::forgetOwned()
{
    { const juce::ScopedLock sl (lock); ownedGrant = Grant(); file ("owned.json").deleteFile(); }
    notify();
}

void Rights::notify()
{
    if (! onChange) return;
    if (juce::MessageManager::existsAndIsCurrentThread()) onChange();
    else { auto s = sh; juce::MessageManager::callAsync ([s] { if (s->alive && s->owner->onChange) s->owner->onChange(); }); }
}

juce::String Rights::post (const juce::String& url, const juce::var& body, int* status)
{
    int code = 0;
    const juce::URL u = juce::URL (url).withPOSTData (juce::JSON::toString (body, true));
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json")
                    .withConnectionTimeoutMs (8000)
                    .withStatusCode (&code);
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    const auto text = in != nullptr ? in->readEntireStreamAsString() : juce::String();
    if (status != nullptr) *status = in != nullptr ? code : 0;
    return text;
}

void Rights::refreshAsync (bool force)
{
    if (! cfg.network) return;
    if (! force && now() - lastRefresh < 5 * 60 * 1000) return;
    if (! refreshDue()) return;                                // nothing owned, or still fresh
    juce::String ownedWire;
    { const juce::ScopedLock sl (lock); ownedWire = wireOf (ownedGrant); }
    if (sh->busy.exchange (true)) return;
    lastRefresh = now();
    const auto c = cfg;
    auto s = sh;
    juce::Thread::launch ([s, c, ownedWire]
    {
        int status = 0;
        const auto reply = post (c.apiBase + "grant/refresh", obj ({ { "grant", juce::JSON::parse (ownedWire) } }), &status);
        juce::MessageManager::callAsync ([s, reply, status]
        {
            s->busy = false;
            if (s->alive) s->owner->applyRefreshReply (status, reply);
        });
    });
}

bool Rights::refreshDue() const
{
    const juce::ScopedLock sl (lock);
    return ownedGrant.valid && now() - ownedGrant.issued > kRefreshAfter;
}

void Rights::applyRefreshReply (int status, const juce::String& reply)
{
    const auto f = juce::JSON::parse (reply);   // plain text (a service failure) parses to void: nothing changes
    if (status != 200 || ! f.isObject()) return;
    if ((bool) f.getProperty ("ok", false)) adopt (juce::JSON::toString (f.getProperty ("grant", {}), true));
    else if (f.getProperty ("error", {}).toString() == "not owned") forgetOwned();
}

Rights::SignIn Rights::signIn() const { const juce::ScopedLock sl (sh->lock); return sh->signIn; }

#if JF_RIGHTS_TESTS
void Rights::setSignInForTest (SignIn si) { { const juce::ScopedLock sl (sh->lock); sh->signIn = std::move (si); } notify(); }
#endif

void Rights::openVerifyPageAgain() const
{
    const auto url = signIn().verifyUrl;
    if (! url.startsWith ("https://") || ! cfg.network) return;
    if (cfg.openUrl) cfg.openUrl (url); else juce::URL (url).launchInDefaultBrowser();
}

void Rights::cancelSignIn()
{
    sh->cancel = true;
    ++sh->signInRun;
    { const juce::ScopedLock sl (sh->lock); sh->signIn = SignIn(); }
    notify();
}

void Rights::startSignIn()
{
    if (! cfg.network) return;
    const int run = ++sh->signInRun;
    sh->cancel = false;
    { const juce::ScopedLock sl (sh->lock); sh->signIn = SignIn(); sh->signIn.phase = SignIn::Phase::starting; }
    notify();
    const auto c = cfg;
    auto s = sh;
    juce::Thread::launch ([s, c, run]
    {
        auto setPhase = [s, run] (SignIn::Phase p, juce::String msg, juce::String code = {}, juce::String url = {})
        {
            if (s->signInRun.load() != run) return;
            {
                const juce::ScopedLock sl (s->lock);
                s->signIn.phase = p;
                s->signIn.message = msg;
                if (code.isNotEmpty()) s->signIn.userCode = code;
                if (url.isNotEmpty()) s->signIn.verifyUrl = url;
            }
            juce::MessageManager::callAsync ([s] { if (s->alive && s->owner->onChange) s->owner->onChange(); });
        };
        int status = 0;
        const auto startBody = obj ({ { "product", c.product }, { "machine", c.machine },
                                      { "name", juce::SystemStats::getComputerName().substring (0, 64) } });
        const auto st = juce::JSON::parse (post (c.apiBase + "device/start", startBody, &status));
        if (status != 200 || ! st.isObject() || ! (bool) st.getProperty ("ok", false))
        {   // no answer at all = the network; an answer that refuses (4xx / 5xx / ok:false) = the service
            setPhase (SignIn::Phase::failed, status == 0 ? "NO CONNECTION TO SUBVERSELAB.COM. TRY AGAIN LATER."
                                                         : "SUBVERSELAB.COM COULD NOT START THE SIGN-IN. TRY AGAIN LATER.");
            return;
        }
        const auto deviceCode = st.getProperty ("device_code", {}).toString();
        const auto userCode = st.getProperty ("user_code", {}).toString();
        const auto url = st.getProperty ("verify_url", {}).toString();
        const int interval = juce::jlimit (2, 30, (int) st.getProperty ("interval", 3));
        const int expires = juce::jlimit (60, 3600, (int) st.getProperty ("expires_in", 600));
        if (! url.startsWith ("https://subverselab.com/"))
        {
            setPhase (SignIn::Phase::failed, "THE SERVER SENT AN ADDRESS THAT IS NOT SUBVERSELAB.COM.");
            return;
        }
        setPhase (SignIn::Phase::waiting, "FINISH IN THE BROWSER", userCode, url);
        juce::MessageManager::callAsync ([url, open = c.openUrl] { if (open) open (url); else juce::URL (url).launchInDefaultBrowser(); });
        const auto until = juce::Time::getMillisecondCounterHiRes() + expires * 1000.0;
        while (juce::Time::getMillisecondCounterHiRes() < until)
        {
            for (int i = 0; i < interval * 10; ++i)
            {
                if (s->cancel.load() || s->signInRun.load() != run || ! s->alive.load()) return;
                juce::Thread::sleep (100);
            }
            int ps = 0;
            const auto p = juce::JSON::parse (post (c.apiBase + "device/poll", obj ({ { "device_code", deviceCode } }), &ps));
            if (ps != 200 || ! p.isObject()) continue;                    // network hiccup: keep polling
            const auto phase = p.getProperty ("status", {}).toString();
            if (phase == "pending") continue;
            if (phase == "approved")
            {
                const auto wire = juce::JSON::toString (p.getProperty ("grant", {}), true);
                juce::MessageManager::callAsync ([s, wire, run, setPhase]
                {
                    if (! s->alive || s->signInRun.load() != run) return;
                    if (s->owner->adopt (wire)) setPhase (SignIn::Phase::done, "SIGNED IN. THANK YOU.");
                    else setPhase (SignIn::Phase::failed, "THE ANSWER FROM THE SERVER DID NOT CHECK OUT.");
                });
                return;
            }
            setPhase (SignIn::Phase::failed, "THE CODE EXPIRED. TRY AGAIN.");
            return;
        }
        setPhase (SignIn::Phase::failed, "THE CODE EXPIRED. TRY AGAIN.");
    });
}
} // namespace jf::rights

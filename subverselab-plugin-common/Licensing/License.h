#pragma once

/*  Licensing shared by the SubverseLab plugins, on Lemon Squeezy's License API
    (https://docs.lemonsqueezy.com/api/license-api). There is no demo mode: a
    plugin without a licence shows the activation screen and passes audio
    through untouched.

    - A key comes from a purchase, or from a free checkout with a DM coupon.
    - activate  POST /v1/licenses/activate    license_key, instance_name
      validate  POST /v1/licenses/validate    license_key, instance_id
      deactivate POST /v1/licenses/deactivate license_key, instance_id
      No API secret is involved; the key itself is the credential.
    - The activation is stored per product in
      ~/Library/Application Support/SubverseLab/Licenses/<product>.json (macOS) or
      %APPDATA%\SubverseLab\Licenses\<product>.json (Windows) and
      trusted offline. It is re-validated in the background once a week; only a
      clear "disabled / expired / instance gone" answer revokes it, never a
      network failure.
    - A key is accepted only if it belongs to our store and to one of the
      product ids listed for this plugin (single or bundle).
*/

#include <JuceHeader.h>
#include <atomic>
#include <functional>

namespace svl
{
struct LicenseConfig
{
    juce::String product;                 // "KUBBE", "KASET": file name and screen title
    juce::String buyUrl;                  // checkout page (buy, or redeem a DM code)
    juce::int64 storeId = 0;              // 0 = not configured yet: any store is accepted (development only)
    juce::Array<juce::int64> productIds;  // empty = any product in the store (development only)
    juce::String apiBase { "https://api.lemonsqueezy.com/v1/licenses/" };
};

class License
{
public:
    struct Result
    {
        bool ok = false;
        juce::String message, instanceId, status;
        bool definitelyRevoked = false;   // the server said no, as opposed to "could not ask"
    };

    explicit License (LicenseConfig c) : cfg (std::move (c)), alive (std::make_shared<std::atomic<bool>> (true))
    {
        load();
        if (licensed.load())
            revalidateIfDue();
    }

    ~License() { alive->store (false); }

    bool isLicensed() const { return licensed.load(); }
    const LicenseConfig& config() const { return cfg; }

    /** The last four characters of the key, for "Licensed ····ABCD". */
    juce::String keyHint() const
    {
        const juce::ScopedLock sl (lock);
        return key.length() >= 4 ? key.getLastCharacters (4).toUpperCase() : juce::String();
    }

    /** Runs the request off the message thread; done is called back on the message thread. */
    void activate (const juce::String& licenseKey, std::function<void (bool, juce::String)> done)
    {
        const auto k = licenseKey.trim();
        if (k.isEmpty()) { done (false, "Paste your licence key first."); return; }
        auto flag = alive;
        const auto c = cfg;   // the worker never touches *this: the plugin may be gone before the reply
        juce::Thread::launch ([this, flag, c, k, done]
        {
            const auto body = post (c.apiBase, "activate", { { "license_key", k }, { "instance_name", instanceName() } });
            const auto r = parseActivate (body, c);
            juce::MessageManager::callAsync ([this, flag, k, r, done]
            {
                if (! flag->load()) return;
                if (r.ok)
                {
                    {
                        const juce::ScopedLock sl (lock);
                        key = k;
                        instanceId = r.instanceId;
                        lastValidated = juce::Time::currentTimeMillis();
                    }
                    save();
                    licensed = true;
                }
                done (r.ok, r.message);
            });
        });
    }

    void deactivate (std::function<void (bool, juce::String)> done)
    {
        juce::String k, id;
        {
            const juce::ScopedLock sl (lock);
            k = key;
            id = instanceId;
        }
        auto flag = alive;
        const auto base = cfg.apiBase;
        juce::Thread::launch ([this, flag, base, k, id, done]
        {
            const auto body = post (base, "deactivate", { { "license_key", k }, { "instance_id", id } });
            const auto v = juce::JSON::parse (body);
            const bool ok = (bool) v.getProperty ("deactivated", false);
            const juce::String err = v.getProperty ("error", juce::var()).toString();
            juce::MessageManager::callAsync ([this, flag, ok, err, done]
            {
                if (! flag->load()) return;
                if (ok) forget();
                done (ok, ok ? "This computer is released. You can activate the key elsewhere."
                             : (err.isNotEmpty() ? err : "Could not reach the licence server. Check your connection."));
            });
        });
    }

    // Parsing is static so it can be tested without a network.
    static Result parseActivate (const juce::String& body, const LicenseConfig& cfg)
    {
        Result r;
        const auto v = juce::JSON::parse (body);
        if (! v.isObject())
        {
            r.message = "Could not reach the licence server. Check your connection and try again.";
            return r;
        }
        if (! (bool) v.getProperty ("activated", false))
        {
            const auto err = v.getProperty ("error", juce::var()).toString();
            r.definitelyRevoked = true;
            r.message = err.isNotEmpty() ? err : "This key could not be activated.";
            return r;
        }
        if (! belongsToUs (v, cfg))
        {
            r.definitelyRevoked = true;
            r.message = "This key is for a different product.";
            return r;
        }
        r.ok = true;
        r.instanceId = v.getProperty ("instance", juce::var()).getProperty ("id", juce::var()).toString();
        r.status = v.getProperty ("license_key", juce::var()).getProperty ("status", juce::var()).toString();
        r.message = cfg.product + " is licensed on this computer. Thank you.";
        return r;
    }

    static Result parseValidate (const juce::String& body, const LicenseConfig& cfg)
    {
        Result r;
        const auto v = juce::JSON::parse (body);
        if (! v.isObject())
            return r;   // could not ask: keep what we have
        const auto status = v.getProperty ("license_key", juce::var()).getProperty ("status", juce::var()).toString();
        r.status = status;
        if ((bool) v.getProperty ("valid", false) && belongsToUs (v, cfg))
        {
            r.ok = true;
            return r;
        }
        // An explicit "no": disabled, expired, or this instance no longer exists.
        r.definitelyRevoked = true;
        r.message = v.getProperty ("error", juce::var()).toString();
        return r;
    }

    /** Test hook: pretend a licence is (or is not) present, without touching the disk. */
    void setLicensedForTesting (bool on) { licensed = on; testing = true; }

    static juce::File folder()
    {
       #if JUCE_WINDOWS
        // userApplicationDataDirectory already resolves to %APPDATA% (Roaming) on Windows.
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("SubverseLab").getChildFile ("Licenses");
       #else
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Application Support").getChildFile ("SubverseLab").getChildFile ("Licenses");
       #endif
    }

private:
    static bool belongsToUs (const juce::var& v, const LicenseConfig& cfg)
    {
        const auto meta = v.getProperty ("meta", juce::var());
        const auto store = (juce::int64) meta.getProperty ("store_id", 0);
        const auto product = (juce::int64) meta.getProperty ("product_id", 0);
        if (cfg.storeId != 0 && store != cfg.storeId) return false;
        if (! cfg.productIds.isEmpty() && ! cfg.productIds.contains (product)) return false;
        return true;
    }

    static juce::String instanceName()
    {
        return juce::SystemStats::getComputerName().isNotEmpty() ? juce::SystemStats::getComputerName() : juce::String ("Mac");
    }

    static juce::String post (const juce::String& apiBase, const juce::String& endpoint,
                              std::initializer_list<std::pair<const char*, juce::String>> fields)
    {
        juce::URL url (apiBase + endpoint);
        for (auto& f : fields)
            url = url.withParameter (f.first, f.second);
        int status = 0;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                        .withExtraHeaders ("Accept: application/json")
                        .withConnectionTimeoutMs (15000)
                        .withStatusCode (&status);
        std::unique_ptr<juce::InputStream> in (url.createInputStream (opts));
        return in != nullptr ? in->readEntireStreamAsString() : juce::String();
    }

    juce::File file() const { return folder().getChildFile (cfg.product.toLowerCase() + ".json"); }

    void load()
    {
        const auto v = juce::JSON::parse (file().loadFileAsString());
        if (! v.isObject()) return;
        const juce::ScopedLock sl (lock);
        key = v.getProperty ("key", juce::var()).toString();
        instanceId = v.getProperty ("instance", juce::var()).toString();
        lastValidated = (juce::int64) v.getProperty ("validated", 0);
        licensed = key.isNotEmpty() && instanceId.isNotEmpty();
    }

    void save()
    {
        if (testing) return;
        auto* o = new juce::DynamicObject();
        {
            const juce::ScopedLock sl (lock);
            o->setProperty ("key", key);
            o->setProperty ("instance", instanceId);
            o->setProperty ("validated", lastValidated);
        }
        folder().createDirectory();
        file().replaceWithText (juce::JSON::toString (juce::var (o)));
    }

    void forget()
    {
        {
            const juce::ScopedLock sl (lock);
            key.clear();
            instanceId.clear();
        }
        licensed = false;
        if (! testing) file().deleteFile();
    }

    void revalidateIfDue()
    {
        juce::String k, id;
        {
            const juce::ScopedLock sl (lock);
            if (juce::Time::currentTimeMillis() - lastValidated < (juce::int64) 7 * 24 * 3600 * 1000) return;
            k = key;
            id = instanceId;
        }
        auto flag = alive;
        const auto c = cfg;
        juce::Thread::launch ([this, flag, c, k, id]
        {
            const auto r = parseValidate (post (c.apiBase, "validate", { { "license_key", k }, { "instance_id", id } }), c);
            juce::MessageManager::callAsync ([this, flag, r]
            {
                if (! flag->load()) return;
                if (r.ok)
                {
                    { const juce::ScopedLock sl (lock); lastValidated = juce::Time::currentTimeMillis(); }
                    save();
                }
                else if (r.definitelyRevoked)
                    forget();
            });
        });
    }

    LicenseConfig cfg;
    std::shared_ptr<std::atomic<bool>> alive;
    std::atomic<bool> licensed { false };
    bool testing = false;
    juce::CriticalSection lock;
    juce::String key, instanceId;
    juce::int64 lastValidated = 0;
};
} // namespace svl

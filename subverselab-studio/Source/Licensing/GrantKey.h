#pragma once
// Public key that checks the server's grants (subverselab-plugin-common/docs/TRIAL_AND_SIGNIN.md), JUCE RSAKey format "e,n" in hex.
// PRODUCTION KEY, the same one AHENK compiles (subverselab-fazilsay/Source/Licensing/GrantKey.h; created 7 Nov 2026 on the
// owner's go). Its private half lives only in Secret Manager (GRANT_SIGNING_KEY). The offline checks use a throwaway key from
// tests/fixtures/grant_fixture_jingfang.json, handed to Rights in code (JF_RIGHTS_TESTS builds only); nothing reads a key or
// unlocks from the environment.
namespace jf::rights
{
inline const char* grantPublicKey()
{
    return "10001,ea6515c45fb4cec932f2a18cbe9dd4b32c4e1036e854bdb743ad7b213cb474a58a030b3bb6ffc4bb85700277199cc825828c3d96ae4455e55c5bc5694fa6f2c305fd4ff60d6251aebe9b587e6dd61a9814e5e1c9cb53829a9af26f1a34aca3997252ed3287ecd8892b87f7fb8158091bf458629dfab843bd3f976e387fce11ddfe407831c21499fd301c643719efe134f819e8e8d306070cbf928c86d7d66dfd177ea03d9bb585867d6ebb28e1c1b55152ada6800e06f7111ac1b8ce3643d13c10111785a7d737b917b875a050b1db07895a1ffc6fd9a52a6f80ef83fcecb95d3bcb61b85e34fc0d26a58c72b3b1147c0e5d9092a1b707fa426f33861894d2a5";
}
inline constexpr bool kGrantKeyIsDev = false;
} // namespace jf::rights

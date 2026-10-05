#pragma once

/*  The seam for licensing. MIHENK is for the owner's own use for now, so
    everything is allowed. When it is sold, back this with
    subverselab-plugin-common/Licensing (svl::License, as KASET and KUBBE do):
    keep the audio path and the meters free of licence checks and gate only
    the actions listed here, so an unlicensed copy still measures honestly.
*/
namespace mihenk
{
struct Entitlement
{
    static bool mayExport()        { return true; }
    static bool mayBatchMaster()   { return true; }
};
} // namespace mihenk

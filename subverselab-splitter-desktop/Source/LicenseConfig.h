#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where ELEK licences come from (the family pattern, KASET Source/LicenseConfig.h).

    Lemon Squeezy store "SubverseLab" #486764. productIds: the ELEK product, created by the owner in Lemon Squeezy.
    While it is empty License.h refuses every Lemon Squeezy key, in Release and Debug alike (an empty id never means
    "accept everything"), so a build without the real id can only run Key & BPM.
*/
inline svl::LicenseConfig elekLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "ELEK";
    c.buyUrl = "https://subverselab.com/plugins/elek";
    c.storeId = 486764;                   // Lemon Squeezy store SubverseLab
    c.productIds = { 1433515 };           // ELEK, created 12 Oct 2026 (draft until the release)
    return c;
}

#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where KASET licences come from. Fill these in once the Lemon Squeezy store
    exists; until then any key from any store activates (development only).
      storeId    : Lemon Squeezy store id
      productIds : the KASET product and the KUBBE + KASET bundle
      buyUrl     : the checkout page (buying, or redeeming a DM code)
*/
inline svl::LicenseConfig kasetLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "KASET";
    c.buyUrl = "https://subverselab.com/plugins";   // TODO: Lemon Squeezy checkout URL
    c.storeId = 0;                                   // TODO before release
    // c.productIds = { 000000, 000000 };          // TODO before release: KASET, bundle
    return c;
}

#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where KUBBE licences come from. Fill these in once the Lemon Squeezy store
    exists; until then any key from any store activates (development only).
      storeId    : Lemon Squeezy store id
      productIds : the KUBBE product and the KUBBE + KASET bundle
      buyUrl     : the checkout page (buying, or redeeming a DM code)
*/
inline svl::LicenseConfig kubbeLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "KUBBE";
    c.buyUrl = "https://subverselab.com/plugins";   // TODO: Lemon Squeezy checkout URL
    c.storeId = 0;                                   // TODO before release
    // c.productIds = { 000000, 000000 };          // TODO before release: KUBBE, bundle
    return c;
}

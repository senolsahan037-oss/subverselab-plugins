#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where KUBBE licences come from. Fill these in once the Lemon Squeezy store
    exists; until then any Lemon Squeezy key from any store activates
    (development only).
      storeId    : Lemon Squeezy store id
      productIds : the KUBBE product and the KUBBE + KASET bundle
      buyUrl     : the checkout page (buying, or redeeming a DM code)
    SVL- launch keys are checked by our own server (svlApiBase, store 1):
      svlProductIds : 9001 KUBBE, 9003 the KUBBE + KASET launch bundle
*/
inline svl::LicenseConfig kubbeLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "KUBBE";
    c.buyUrl = "https://subverselab.com/launch";    // TODO for the paid phase: Lemon Squeezy checkout URL
    c.storeId = 0;                                   // TODO before release
    // c.productIds = { 000000, 000000 };          // TODO before release: KUBBE, bundle
    c.svlProductIds = { 9001, 9003 };              // KUBBE, launch bundle
    return c;
}

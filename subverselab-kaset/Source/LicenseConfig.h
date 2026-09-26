#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where KASET licences come from. Fill these in once the Lemon Squeezy store
    exists; until then any Lemon Squeezy key from any store activates
    (development only).
      storeId    : Lemon Squeezy store id
      productIds : the KASET product and the KUBBE + KASET bundle
      buyUrl     : the checkout page (buying, or redeeming a DM code)
    SVL- launch keys are checked by our own server (svlApiBase, store 1):
      svlProductIds : 9002 KASET, 9003 the KUBBE + KASET launch bundle
*/
inline svl::LicenseConfig kasetLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "KASET";
    c.buyUrl = "https://subverselab.com/launch";    // TODO for the paid phase: Lemon Squeezy checkout URL
    c.storeId = 0;                                   // TODO before release
    // c.productIds = { 000000, 000000 };          // TODO before release: KASET, bundle
    c.svlProductIds = { 9002, 9003 };              // KASET, launch bundle
    return c;
}

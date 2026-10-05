#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where HAVA licences come from, for a future paid phase (the plugin is built
    with SVL_LICENSING=OFF, as KUBBE 1.0.1 ships). Price and licence path are
    not set yet (PRODUCT_BRIEF.md, "Open").
      storeId    : Lemon Squeezy store id
      productIds : the HAVA product (and any bundle)
      buyUrl     : the checkout page
    SVL- launch keys are checked by our own server (svlApiBase, store 1):
      svlProductIds : not assigned yet for HAVA
*/
inline svl::LicenseConfig havaLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "HAVA";
    c.buyUrl = "https://subverselab.com/launch";    // TODO for the paid phase
    c.storeId = 0;                                   // TODO before release
    // c.productIds = { 000000 };                  // TODO before release
    // c.svlProductIds = { 0000 };                 // TODO: HAVA id on our licence API
    return c;
}

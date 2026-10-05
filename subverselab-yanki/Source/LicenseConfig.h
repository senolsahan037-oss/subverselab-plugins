#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where YANKI licences come from. Compiled only with -DSVL_LICENSING=ON
    (OFF by default, like KUBBE 1.0.1). Fill these in before a paid phase.
      storeId    : Lemon Squeezy store id
      productIds : the YANKI product (and any bundle)
      buyUrl     : the checkout page
    SVL- keys are checked by our own server (svlApiBase, store 1):
      svlProductIds : TODO, no YANKI product id is assigned yet
*/
inline svl::LicenseConfig yankiLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "YANKI";
    c.buyUrl = "https://subverselab.com/launch";    // TODO for the paid phase: checkout URL
    c.storeId = 0;                                   // TODO before release
    // c.productIds = { 000000 };                  // TODO before release
    // c.svlProductIds = { 0000 };                 // TODO: YANKI id on the SVL licence API
    return c;
}

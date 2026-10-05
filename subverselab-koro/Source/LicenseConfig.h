#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where KORO licences come from (compiled only with -DSVL_LICENSING=ON).
    KORO is a PAID product: the gate is the paid path KUBBE/KASET compiled out.
    Fill these in once the Lemon Squeezy product exists; until then any Lemon
    Squeezy key from any store activates (development only).
      storeId    : Lemon Squeezy store id
      productIds : KORO, and the planned bundle (KORO + YANKI + HAVA + MIHENK):
                   one key may activate several products, so the bundle id goes here too
      buyUrl     : the checkout page
*/
inline svl::LicenseConfig koroLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "KORO";
    c.buyUrl = "https://subverselab.com/koro";      // TODO before release: Lemon Squeezy checkout URL
    c.storeId = 0;                                   // TODO before release
    // c.productIds = { 000000, 000000 };          // TODO before release: KORO, bundle
    // c.svlProductIds = { };                      // KORO is not part of the free launch keys
    return c;
}

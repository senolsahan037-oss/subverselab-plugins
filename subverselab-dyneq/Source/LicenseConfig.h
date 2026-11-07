#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where DYNEQ licences come from. Compiled only with -DSVL_LICENSING=ON (OFF by default; P5 of SPEC.md turns it on).
    The seam is kept so the licence gate can be compiled back in without touching the processor. NOTHING HERE IS A REAL ID:
      storeId       : Lemon Squeezy store id                      TODO before release
      productIds    : the DYNEQ product (and any bundle)          TODO before release
      svlProductIds : id on the SVL licence API (our own server)  TODO, none assigned
      buyUrl        : the checkout page                           TODO
    DYNEQ is a working name; `product` changes with the owner's name for it.
*/
inline svl::LicenseConfig dyneqLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "ADALET";
    c.buyUrl = "https://subverselab.com/account";   // tester phase: the account page (keys are issued there)
    c.storeId = 0;                                   // Lemon Squeezy: not configured, so every Lemon Squeezy key is refused (fails closed)
    // productIds stays empty on purpose (same reason).
    // TESTER KEYS ONLY: these are tester keys on the SVL licence API (https://subverselab.com/api/license/, store id 1). The server answers
    // with the key's own product id, ADALET = 9022. A key can be switched off remotely on the server; the plug-in then revokes at its weekly re-validation.
    c.svlProductIds = { 9022 };
    return c;
}

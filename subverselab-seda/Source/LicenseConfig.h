#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where SEDA licences come from. Compiled only with -DSVL_LICENSING=ON (OFF by default; P5 of SPEC.md turns it on, KASET 1.1.0 pattern).
    The seam is kept so the licence gate can be compiled back in without touching the processor. NOTHING HERE IS A REAL ID:
      storeId       : Lemon Squeezy store id                      TODO before release
      productIds    : the SEDA product (and any bundle)           TODO before release
      svlProductIds : id on the SVL licence API (our own server)  TODO, none assigned
      buyUrl        : the checkout page                           TODO
    SEDA is a working name; `product` changes with the owner's name for it.
*/
inline svl::LicenseConfig sedaLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "SEDA";
    c.buyUrl = "https://subverselab.com/account";   // tester phase: the account page (keys are issued there)
    c.storeId = 0;                                   // Lemon Squeezy: not configured, so every Lemon Squeezy key is refused (fails closed)
    // productIds stays empty on purpose (same reason).
    // TESTER KEYS ONLY: these are tester keys on the SVL licence API (https://subverselab.com/api/license/, store id 1). The server answers
    // with the key's own product id, SEDA = 9021. A key can be switched off remotely on the server; the plug-in then revokes at its weekly re-validation.
    c.svlProductIds = { 9021 };
    return c;
}

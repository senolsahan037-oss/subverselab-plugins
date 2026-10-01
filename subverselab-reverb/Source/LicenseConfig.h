#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where KUBBE licences come from.

    Keys that start with SVL- go to our own server (svlApiBase, store 1):
      svlProductIds : 9001 KUBBE, 9003 the KUBBE + KASET launch bundle
    Every other key goes to the Lemon Squeezy License API, and is accepted only
    for the store and products below.

    Lemon Squeezy ids (store "SubverseLab" #486764, products created 1 Oct 2026).
    With either left at 0 / empty License.h refuses every Lemon Squeezy key,
    in Release and Debug alike (an empty id never means "accept everything").
      storeId    : Lemon Squeezy store id
      productIds : the KUBBE product and the KUBBE + KASET bundle
*/
inline svl::LicenseConfig kubbeLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "KUBBE";
    c.buyUrl = "https://subverselab.com/launch";
    c.storeId = 486764;                   // Lemon Squeezy store SubverseLab
    c.productIds = { 1402269, 1402289 };   // KUBBE, KUBBE + KASET Bundle
    c.svlProductIds = { 9001, 9003 };   // KUBBE, launch bundle
    return c;
}

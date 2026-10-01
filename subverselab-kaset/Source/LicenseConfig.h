#pragma once

#include "../../subverselab-plugin-common/Licensing/License.h"

/*  Where KASET licences come from.

    Keys that start with SVL- go to our own server (svlApiBase, store 1):
      svlProductIds : 9002 KASET, 9003 the KUBBE + KASET launch bundle
    Every other key goes to the Lemon Squeezy License API, and is accepted only
    for the store and products below.

    Lemon Squeezy ids (store "SubverseLab" #486764, products created 1 Oct 2026).
    With either left at 0 / empty License.h refuses every Lemon Squeezy key,
    in Release and Debug alike (an empty id never means "accept everything").
      storeId    : Lemon Squeezy store id
      productIds : the KASET product and the KUBBE + KASET bundle
*/
inline svl::LicenseConfig kasetLicenseConfig()
{
    svl::LicenseConfig c;
    c.product = "KASET";
    c.buyUrl = "https://subverselab.com/launch";
    c.storeId = 486764;                   // Lemon Squeezy store SubverseLab
    c.productIds = { 1402277, 1402289 };   // KASET, KUBBE + KASET Bundle
    c.svlProductIds = { 9002, 9003 };   // KASET, launch bundle
    return c;
}

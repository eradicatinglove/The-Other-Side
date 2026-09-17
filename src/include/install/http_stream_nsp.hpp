// Streams an NSP straight off HTTP into NCM storage - one sequential GET,
// no Range requests, no temp file. Alternative to the Range-based HTTPNSP
// path (see http_nsp.hpp), used for shop installs where Range requests
// were choking on some hosts.
#pragma once

#include <switch.h>
#include <string>
#include <vector>

namespace tin::install::nsp
{
    // throws on failure, same as Install::Prepare/Begin
    void InstallNspHttpStreamSequential(
        const std::string& url,
        const std::string& basicAuthUser,
        const std::string& basicAuthPass,
        NcmStorageId destStorageId,
        bool ignoreReqFirmVersion);

    // same idea but for jbod: titles with more than one part - each part
    // downloads in full (still no Range requests), fed in as one file
    void InstallNspHttpStreamJbodParts(
        const std::vector<std::string>& partUrls,
        const std::string& basicAuthUser,
        const std::string& basicAuthPass,
        NcmStorageId destStorageId,
        bool ignoreReqFirmVersion);
}

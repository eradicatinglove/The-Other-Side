#include "util/uid.hpp"

#include <array>
#include <mutex>
#include <string>

#include <switch.h>

namespace inst::util {
    std::string ComputeUidFromMmcCid()
    {
        static std::once_flag once;
        static std::string uid(64, '0');
        std::call_once(once, []() {
            FsDeviceOperator d = {};
            if (R_FAILED(fsOpenDeviceOperator(&d)))
                return;

            std::array<unsigned char, 16> mmcCid = {};
            const Result rc = fsDeviceOperatorGetMmcCid(&d, mmcCid.data(), mmcCid.size(), static_cast<s64>(mmcCid.size()));
            fsDeviceOperatorClose(&d);
            if (R_FAILED(rc))
                return;

            std::array<unsigned char, 32> hash = {};
            sha256CalculateHash(hash.data(), mmcCid.data(), mmcCid.size());

            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(hash.size() * 2);
            for (unsigned char b : hash) {
                out.push_back(kHex[(b >> 4) & 0xF]);
                out.push_back(kHex[b & 0xF]);
            }

            std::fill(mmcCid.begin(), mmcCid.end(), 0);
            std::fill(hash.begin(), hash.end(), 0);
            uid = out;
        });
        return uid;
    }
}


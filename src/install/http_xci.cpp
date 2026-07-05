/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "install/http_xci.hpp"

#include "nx/nca_writer.h"
#include "util/error.hpp"
#include "util/util.hpp"
#include "util/lang.hpp"
#include "ui/instPage.hpp"

namespace tin::install::xci
{
    HTTPXCI::HTTPXCI(std::string url) :
        m_download(url)
    {

    }

    void HTTPXCI::StreamToPlaceholder(std::shared_ptr<nx::ncm::ContentStorage>& contentStorage, NcmContentId ncaId)
    {
        const HFS0FileEntry* fileEntry = this->GetFileEntryByNcaId(ncaId);
        std::string ncaFileName = this->GetFileEntryName(fileEntry);

        LOG_DEBUG("Retrieving %s\n", ncaFileName.c_str());
        size_t ncaSize = fileEntry->fileSize;

        NcaWriter writer(ncaId, contentStorage);
        float progress = 0.0f;
        u64 fileStart = GetDataOffset() + fileEntry->dataOffset;
        u64 fileOff = 0;
        size_t readSize = 0x400000;
        auto readBuffer = std::make_unique<u8[]>(readSize);

        try {
            inst::ui::instPage::setInstInfoText("inst.info_page.top_info0"_lang + ncaFileName + "...");
            inst::ui::instPage::setInstBarPerc(0);
            while (fileOff < ncaSize) {
                if (inst::ui::instPage::isInstallCancelRequested()) {
                    THROW_FORMAT("Installation canceled.");
                }
                progress = (float)fileOff / (float)ncaSize;

                if (fileOff % (0x400000 * 3) == 0) {
                    LOG_DEBUG("> Progress: %lu/%lu MB (%d%s)\r", (fileOff / 1000000), (ncaSize / 1000000), (int)(progress * 100.0), "%");
                    inst::ui::instPage::setInstBarPerc((double)(progress * 100.0));
                }

                if (fileOff + readSize >= ncaSize)
                    readSize = ncaSize - fileOff;

                this->BufferData(readBuffer.get(), fileOff + fileStart, readSize);
                writer.write(readBuffer.get(), readSize);
                fileOff += readSize;
            }
            inst::ui::instPage::setInstBarPerc(100);
        } catch (std::exception& e) {
            LOG_DEBUG("something went wrong: %s\n", e.what());
            throw;
        }

        writer.close();
    }

    void HTTPXCI::BufferData(void* buf, off_t offset, size_t size)
    {
        m_download.BufferDataRange(buf, offset, size, nullptr);
    }
}

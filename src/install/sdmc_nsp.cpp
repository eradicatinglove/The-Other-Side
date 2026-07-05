#include "install/sdmc_nsp.hpp"
#include "util/error.hpp"
#include "util/debug.h"
#include "nx/nca_writer.h"
#include "ui/instPage.hpp"
#include "util/lang.hpp"
#include <chrono>
#include <cmath>

namespace tin::install::nsp
{
    SDMCNSP::SDMCNSP(std::string path)
    {
        m_nspFile = fopen((path).c_str(), "rb");
        if (!m_nspFile)
            THROW_FORMAT("can't open file at %s\n", path.c_str());
    }

    SDMCNSP::~SDMCNSP()
    {
        fclose(m_nspFile);
    }

    void SDMCNSP::StreamToPlaceholder(std::shared_ptr<nx::ncm::ContentStorage>& contentStorage, NcmContentId ncaId)
    {
        const PFS0FileEntry* fileEntry = this->GetFileEntryByNcaId(ncaId);
        std::string ncaFileName = this->GetFileEntryName(fileEntry);

        LOG_DEBUG("Retrieving %s\n", ncaFileName.c_str());
        size_t ncaSize = fileEntry->fileSize;

        NcaWriter writer(ncaId, contentStorage);

        float progress;

        u64 fileStart = GetDataOffset() + fileEntry->dataOffset;
        u64 fileOff = 0;
        size_t readSize = 0x400000; // 4MB buff
        auto readBuffer = std::make_unique<u8[]>(readSize);

        try
        {
            inst::ui::instPage::setInstInfoText("inst.info_page.top_info0"_lang + ncaFileName + "...");
            inst::ui::instPage::setInstBarPerc(0);
            inst::ui::instPage::setProgressDetailText("0% • Calculating... • -- MB/s");

            auto lastTime = std::chrono::steady_clock::now();
            std::uint64_t lastBytes = 0;
            double emaRate = 0.0;
            while (fileOff < ncaSize)
            {
                progress = (float) fileOff / (float) ncaSize;

                if (fileOff % (0x400000 * 3) == 0) {
                    LOG_DEBUG("> Progress: %lu/%lu MB (%d%s)\r", (fileOff / 1000000), (ncaSize / 1000000), (int)(progress * 100.0), "%");
                    inst::ui::instPage::setInstBarPerc((double)(progress * 100.0));

                    const auto now = std::chrono::steady_clock::now();
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
                    if (elapsed >= 1000) {
                        const auto delta = static_cast<std::uint64_t>(fileOff - lastBytes);
                        const double rate = (elapsed > 0) ? (double)delta / ((double)elapsed / 1000.0) : 0.0;
                        if (rate > 0.0) {
                            if (emaRate <= 0.0) {
                                emaRate = rate;
                            } else {
                                emaRate = (emaRate * 0.7) + (rate * 0.3);
                            }
                        }
                        lastBytes = fileOff;
                        lastTime = now;
                    }

                    std::string etaText = "Calculating...";
                    if (emaRate > 0.0 && fileOff < ncaSize) {
                        const auto remaining = static_cast<std::uint64_t>(ncaSize - fileOff);
                        const auto seconds = static_cast<std::uint64_t>(remaining / emaRate);
                        const auto h = seconds / 3600;
                        const auto m = (seconds % 3600) / 60;
                        const auto s = seconds % 60;
                        if (h > 0) {
                            etaText = std::to_string(h) + ":" + (m < 10 ? "0" : "") + std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
                        } else {
                            etaText = std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
                        }
                        etaText += " remaining";
                    }

                    std::string speedText;
                    if (emaRate > 0.0) {
                        const double mbps = emaRate / (1024.0 * 1024.0);
                        const double rounded = std::round(mbps * 10.0) / 10.0;
                        speedText = std::to_string(rounded);
                        if (speedText.find('.') != std::string::npos) {
                            while (!speedText.empty() && speedText.back() == '0') speedText.pop_back();
                            if (!speedText.empty() && speedText.back() == '.') speedText.pop_back();
                        }
                        speedText += " MB/s";
                    } else {
                        speedText = "-- MB/s";
                    }

                    const int pct = static_cast<int>(progress * 100.0 + 0.5);
                    std::string progressText = std::to_string(pct) + "% • " + etaText + " • " + speedText;
                    inst::ui::instPage::setProgressDetailText(progressText);
                }

                if (fileOff + readSize >= ncaSize) readSize = ncaSize - fileOff;

                this->BufferData(readBuffer.get(), fileOff + fileStart, readSize);
                writer.write(readBuffer.get(), readSize);

                fileOff += readSize;
            }
            inst::ui::instPage::setInstBarPerc(100);
            inst::ui::instPage::setProgressDetailText("100% • done");
        }
        catch (std::exception& e)
        {
            LOG_DEBUG("something went wrong: %s\n", e.what());
        }

        writer.close();
    }

    void SDMCNSP::BufferData(void* buf, off_t offset, size_t size)
    {
        fseeko(m_nspFile, offset, SEEK_SET);
        fread(buf, 1, size, m_nspFile);
    }
}

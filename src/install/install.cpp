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

#include "install/install.hpp"

#include <switch.h>
#include <cstring>
#include <memory>
#include <algorithm>
#include "util/error.hpp"
#include "util/install_diagnostics.hpp"
#include "ui/instPage.hpp"

#include "nx/ncm.hpp"
#include "util/config.hpp"
#include "util/title_util.hpp"


// TODO: Check NCA files are present
// TODO: Check tik/cert is present
namespace tin::install
{
    Install::Install(NcmStorageId destStorageId, bool ignoreReqFirmVersion) :
        m_destStorageId(destStorageId), m_ignoreReqFirmVersion(ignoreReqFirmVersion), m_contentMeta()
    {
        appletSetMediaPlaybackState(true);
    }

    Install::~Install()
    {
        appletSetMediaPlaybackState(false);
    }

    bool Install::IsSessionInstalledNca(const NcmContentId& ncaId) const
    {
        return std::any_of(m_sessionInstalledNcas.begin(), m_sessionInstalledNcas.end(),
            [&ncaId](const NcmContentId& existing) {
                return std::memcmp(&existing, &ncaId, sizeof(NcmContentId)) == 0;
            });
    }

    void Install::TrackSessionInstalledNca(const NcmContentId& ncaId)
    {
        if (!IsSessionInstalledNca(ncaId))
            m_sessionInstalledNcas.push_back(ncaId);
    }

    void Install::CleanupSessionInstalledNcas()
    {
        if (m_sessionInstalledNcas.empty())
            return;

        nx::ncm::ContentStorage contentStorage(m_destStorageId);
        for (const NcmContentId& ncaId : m_sessionInstalledNcas) {
            try {
                contentStorage.DeletePlaceholder(*(const NcmPlaceHolderId*)&ncaId);
            } catch (...) {}

            try {
                if (contentStorage.Has(ncaId))
                    contentStorage.Delete(ncaId);
            } catch (...) {}
        }
        m_sessionInstalledNcas.clear();
    }

    // TODO: Implement RAII on NcmContentMetaDatabase
    void Install::InstallContentMetaRecords(tin::data::ByteBuffer& installContentMetaBuf, int i)
    {
        NcmContentMetaDatabase contentMetaDatabase;
        NcmContentMetaKey contentMetaKey = m_contentMeta[i].GetContentMetaKey();

        try
        {
            ASSERT_OK(ncmOpenContentMetaDatabase(&contentMetaDatabase, m_destStorageId), "Failed to open content meta database");
            ASSERT_OK(ncmContentMetaDatabaseSet(&contentMetaDatabase, &contentMetaKey, (NcmContentMetaHeader*)installContentMetaBuf.GetData(), installContentMetaBuf.GetSize()), "Failed to set content records");
            ASSERT_OK(ncmContentMetaDatabaseCommit(&contentMetaDatabase), "Failed to commit content records");
        }
        catch (std::runtime_error& e)
        {
            serviceClose(&contentMetaDatabase.s);
            THROW_FORMAT(e.what());
        }

        serviceClose(&contentMetaDatabase.s);
    }

    void Install::InstallApplicationRecord(int i)
    {
        const u64 baseTitleId = tin::util::GetBaseTitleId(this->GetTitleId(i), this->GetContentMetaType(i));

        ContentStorageRecord storageRecord;
        storageRecord.metaRecord = m_contentMeta[i].GetContentMetaKey();
        storageRecord.storageId = m_destStorageId;

        LOG_DEBUG("Pushing application record...\n");
        ASSERT_OK(nsPushApplicationRecord(baseTitleId, NsApplicationRecordType_Installed, &storageRecord, 1), "Failed to push application record");
    }

    // Validate and obtain all data needed for install
    void Install::Prepare()
    {
        tin::data::ByteBuffer cnmtBuf;

        try {
            inst::diag::NoteStep("Prepare phase: reading CNMT records");
            std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> tupelList = this->ReadCNMT();
            inst::diag::NoteStep("Prepare phase: discovered " + std::to_string(tupelList.size()) + " CNMT record(s)");
            
            for (size_t i = 0; i < tupelList.size(); i++) {
                if (inst::ui::instPage::isInstallCancelRequested())
                    THROW_FORMAT("Installation canceled.");
                std::tuple<nx::ncm::ContentMeta, NcmContentInfo> cnmtTuple = tupelList[i];
                
                m_contentMeta.push_back(std::get<0>(cnmtTuple));
                NcmContentInfo cnmtContentRecord = std::get<1>(cnmtTuple);

                nx::ncm::ContentStorage contentStorage(m_destStorageId);

                if (!contentStorage.Has(cnmtContentRecord.content_id))
                {
                    LOG_DEBUG("Installing CNMT NCA...\n");
                    inst::diag::NoteStep("Prepare phase: installing CNMT NCA " + tin::util::GetNcaIdString(cnmtContentRecord.content_id));
                    this->InstallNCA(cnmtContentRecord.content_id);
                    TrackSessionInstalledNca(cnmtContentRecord.content_id);
                }
                else
                {
                    LOG_DEBUG("CNMT NCA already installed. Proceeding...\n");
                    inst::diag::NoteStep("Prepare phase: CNMT already present " + tin::util::GetNcaIdString(cnmtContentRecord.content_id));
                }

                // Parse data and create install content meta
                if (m_ignoreReqFirmVersion)
                    LOG_DEBUG("WARNING: Required system firmware version is being IGNORED!\n");

                tin::data::ByteBuffer installContentMetaBuf;
                m_contentMeta[i].GetInstallContentMeta(installContentMetaBuf, cnmtContentRecord, m_ignoreReqFirmVersion);
                inst::diag::NoteStep("Prepare phase: writing content meta record #" + std::to_string(i + 1));

                this->InstallContentMetaRecords(installContentMetaBuf, i);
                this->InstallApplicationRecord(i);
            }
        }
        catch (...) {
            CleanupSessionInstalledNcas();
            throw;
        }
    }

    void Install::Begin()
    {
        LOG_DEBUG("Installing ticket and cert...\n");
        inst::diag::NoteStep("Install phase: ticket/cert import");
        try
        {
            this->InstallTicketCert();
        }
        catch (std::runtime_error& e)
        {
            LOG_DEBUG("WARNING: Ticket installation failed! This may not be an issue, depending on your use case.\nProceed with caution!\n");
        }

        try {
            for (nx::ncm::ContentMeta contentMeta: m_contentMeta) {
                LOG_DEBUG("Installing NCAs...\n");
                inst::diag::NoteStep("Install phase: NCA validation " + std::string(inst::config::validateNCAs ? "enabled" : "disabled"));
                for (auto& record : contentMeta.GetContentInfos())
                {
                    if (inst::ui::instPage::isInstallCancelRequested())
                        THROW_FORMAT("Installation canceled.");
                    nx::ncm::ContentStorage contentStorage(m_destStorageId);
                    if (contentStorage.Has(record.content_id)) {
                        LOG_DEBUG("NCA already installed. Skipping %s\n", tin::util::GetNcaIdString(record.content_id).c_str());
                        inst::diag::NoteStep("Install phase: skip existing NCA " + tin::util::GetNcaIdString(record.content_id));
                        continue;
                    }

                    LOG_DEBUG("Installing from %s\n", tin::util::GetNcaIdString(record.content_id).c_str());
                    inst::diag::NoteStep("Install phase: writing NCA " + tin::util::GetNcaIdString(record.content_id));
                    this->InstallNCA(record.content_id);
                    TrackSessionInstalledNca(record.content_id);
                }
            }
        }
        catch (...) {
            CleanupSessionInstalledNcas();
            throw;
        }
    }

    u64 Install::GetTitleId(int i)
    {
        return m_contentMeta[i].GetContentMetaKey().id;
    }

    NcmContentMetaType Install::GetContentMetaType(int i)
    {
        return static_cast<NcmContentMetaType>(m_contentMeta[i].GetContentMetaKey().type);
    }
}

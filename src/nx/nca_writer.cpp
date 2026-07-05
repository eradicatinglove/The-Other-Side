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

#include "nx/nca_writer.h"
#include "util/error.hpp"
#include <zstd.h>
#include <string.h>
#include <memory>
#include "util/crypto.hpp"
#include "util/config.hpp"
#include "util/title_util.hpp"
#include "install/nca.hpp"
#include <limits>

// region Utility Functions, Classes, Structs

void append(std::vector<u8>& buffer, const u8* ptr, u64 sz)
{
     u64 offset = buffer.size();
     buffer.resize(offset + sz);
     memcpy(buffer.data() + offset, ptr, sz);
}

// Wrapper over AES128-CTR to handle seek+encrypt/decrypt
class Aes128CtrCipher
{
public:
     // NOTE: Switch is Little Endian so we byte-swap here for convenience
     Aes128CtrCipher(const u8* key, const u8* counter)
     : crypto(key, Crypto::AesCtr(Crypto::swapEndian(((const u64*)counter)[0])))
     {
     }

     void decrypt(void* p, u64 sz, u64 offset)
     {
          crypto.seek(offset);
          crypto.decrypt(p, p, sz);
     }

     void encrypt(void* p, u64 sz, u64 offset)
     {
          crypto.seek(offset);
          crypto.encrypt(p, p, sz);
     }

     Crypto::Aes128Ctr crypto; // Counter (Ctr) mode, for streaming
};

// Custom deleter for malloc'd memory
struct MallocDeleter {
     void operator()(void* ptr) const {
          if (ptr) free(ptr);
     }
};

// Custom deleter for ZSTD_DCtx
struct ZstdDCtxDeleter {
     void operator()(ZSTD_DCtx* ctx) const {
          if (ctx) ZSTD_freeDCtx(ctx);
     }
};

// endregion

// region Header Structs

// NCZSECTN Section Header structure
struct NczSectionHeader
{
     u64 offset;
     u64 size;
     u8 cryptoType;
     u8 padding1[7];
     u64 padding2;
     u8 cryptoKey[0x10];
     u8 cryptoCounter[0x10];
} NX_PACKED;

// NCZSECTN Body Header structure
class NczHeader
{
public:
     static const u64 MAGIC = 0x4E544345535A434E;
     static constexpr size_t MIN_HEADER_SIZE = sizeof(u64) * 2; // magic + sectionCount

     const bool isValid()
     {
          return m_magic == MAGIC && m_sectionCount < 0xFFFF;
     }

     const u64 size() const
     {
          return sizeof(m_magic) + sizeof(m_sectionCount) + sizeof(NczSectionHeader) * m_sectionCount;
     }

     const NczSectionHeader& section(u64 i) const
     {
          return m_sections[i];
     }

     const u64 sectionCount() const
     {
          return m_sectionCount;
     }

protected:
     u64 m_magic;
     u64 m_sectionCount;
     NczSectionHeader m_sections[1];
} NX_PACKED;

// NCZBLOCK header structure
struct NczBlockHeader {
     static const u64 NCZBLOCK_MAGIC = 0x4B434F4C425A434E;  // "NCZBLOCK" in little-endian
     static const u8 TYPE_ZSTD = 0x01;

     bool isValid() const
     {
          if (magic != NCZBLOCK_MAGIC)
          {
               [[maybe_unused]] const auto mbad = reinterpret_cast<const u8*>(&magic);
               [[maybe_unused]] const auto mgood = reinterpret_cast<const u8*>(&NCZBLOCK_MAGIC);
               LOG_DEBUG("[NczBlockHeader] ERROR: Invalid magic: %02X %02X %02X %02X %02X %02X %02X %02X (must be %02X %02X %02X %02X %02X %02X %02X %02X)",
                             mbad[0], mbad[1], mbad[2], mbad[3], mbad[4], mbad[5], mbad[6], mbad[7],
                             mgood[0], mgood[1], mgood[2], mgood[3], mgood[4], mgood[5], mgood[6], mgood[7]);
               return false;
          }
          if (blockSizeExponent < 14 || blockSizeExponent > 32)
          {
               LOG_DEBUG("[NczBlockHeader] ERROR: Invalid block size exponent: %u (must be 14-32)", blockSizeExponent);
               return false;
          }
          // Sanity check, not in source documentation for NCZBLOCK
          if (numberOfBlocks >= 0xFFFF)
          {
               LOG_DEBUG("[NczBlockHeader] ERROR: numberOfBlocks %u exceeds sanity limit", numberOfBlocks);
               return false;
          }
          // Not errors, but may lead to unexpected results, so let's warn
          if (version != 2)
          {
               LOG_DEBUG("[NczBlockHeader] WARNING: Unexpected version %u (expected 2)", version);
          }
          if (type != TYPE_ZSTD)
          {
               LOG_DEBUG("[NczBlockHeader] WARNING: Unexpected compression type %u (expected 1=ZSTD)", type);
          }
          return true;
     }

     u64 blockSize() const
     {
          return 1ULL << blockSizeExponent;
     }

     bool usesZstd() const
     {
          return type == TYPE_ZSTD;
     }

     u64 magic;                  // "NCZBLOCK"
     u8 version;                 // 0x02
     u8 type;                    // 0x01 = ZSTD
     u8 unused;                  // 0x00
     u8 blockSizeExponent;       // 14-32
     u32 numberOfBlocks;         // Little-endian
     u64 decompressedSize;       // Little-endian
     // Followed by: u32 compressedBlockSizes[numberOfBlocks]
} NX_PACKED;

// endregion

// region NcaBodyWriter Methods

NcaBodyWriter::NcaBodyWriter(const NcmContentId& ncaId, u64 offset, std::shared_ptr<nx::ncm::ContentStorage>& contentStorage)
: m_contentStorage(contentStorage), m_ncaId(ncaId), m_offset(offset)
{
     // Pre-allocate the memory to our buffer size to prevent reallocations/fragmentation
     m_contentBuffer.reserve(CONTENT_BUFFER_SIZE);
}

NcaBodyWriter::~NcaBodyWriter()
{
     NcaBodyWriter::close();
}

void NcaBodyWriter::close()
{
     if (isClosed()) return; // Idempotent close

     doBeforeClose(); // Derived close/flush delegates so content buffer is complete

     flushContentBuffer(); // Virtual dispatch - overrides expected to invoke parent

     doClose(); // Derived cleanup before finalizing close

     // Free resources
     m_contentBuffer.clear(); // reclaim ok
     m_contentStorage = NULL;

     CloseableWriter::close(); // Mark as closed after all cleanups are done
}

// Create write() callback for delegates
std::function<WriterFn> NcaBodyWriter::getDirectWriterFn()
{
     auto self = shared_from_this(); // shared_ptr<NcaBodyWriter>
     return [self](const u8* data, u64 size)
     {
          self->NcaBodyWriter::write(data, size);
     };
}

void NcaBodyWriter::write(const  u8* ptr, u64 sz)
{
     if (isClosed())
     {
          LOG_DEBUG("write() called on closed NcaBodyWriter");
          return;
     }

     if (!sz) return; // no data

     while (sz)
     {
          if (m_contentBuffer.size() < CONTENT_BUFFER_SIZE)
          {
               const u64 remainder = std::min(sz, CONTENT_BUFFER_SIZE - (u64)m_contentBuffer.size());
               append(m_contentBuffer, ptr, remainder);
               ptr += remainder;
               sz -= remainder;
          }

          if (m_contentBuffer.size() < CONTENT_BUFFER_SIZE)
          {
               // assert sz == 0
               return; // Need more data
          }

          // assert m_contentBuffer.size() == CONTENT_BUFFER_SIZE

          flushContentBuffer();
     }
}

void NcaBodyWriter::flushContentBuffer()
{
     if (isClosed())
     {
          LOG_DEBUG("flushContentBuffer() called on closed NcaBodyWriter");
          return;
     }

     if (m_contentBuffer.empty()) return; // No data

     if (m_contentStorage)
     {
          m_contentStorage->WritePlaceholder(*(NcmPlaceHolderId*)&m_ncaId, m_offset, m_contentBuffer.data(), m_contentBuffer.size());
          m_offset += m_contentBuffer.size();
     }

     m_contentBuffer.resize(0);
}

// endregion

// region NCZ Writer Delegates

// Pass-through Stream Writer - Handles streaming uncompressed / unknown data
class DirectStreamWriter : public CloseableWriter
{
public:
     explicit DirectStreamWriter(const std::function<WriterFn>& writeFn) : m_writeFn(writeFn)
     {
          if (!writeFn)
               THROW_FORMAT("DirectStreamWriter: WriterFn callback cannot be null");
     }
     ~DirectStreamWriter() override
     {
          DirectStreamWriter::close();
     }

     void write(const u8* data, u64 sz) override
     {
          if (isClosed())
          {
               LOG_DEBUG("write() called on closed DirectStreamWriter");
               return;
          }

          if (!sz) return; // no data

          m_writeFn(data, sz);
     }

     void close() override
     {
          if (isClosed()) return; // Idempotent close

          // Free resources
          m_writeFn = NULL;

          CloseableWriter::close();  // Mark as closed after all cleanups are done
     }

private:
     std::function<WriterFn> m_writeFn;
};

// ZSTD Stream Writer - handles streaming ZSTD compression
class ZstdStreamWriter : public CloseableWriter
{
public:
     static const u32 ZSTD_MAGIC = 0xFD2FB528u; // ZSTD frame magic number - 0x28B52FFD

     ZstdStreamWriter(const std::function<WriterFn>& writeFn)
          : m_writeFn(writeFn),
            m_buffInSize(ZSTD_DStreamInSize()),
            m_buffOutSize(ZSTD_DStreamOutSize()),
            m_buffOut(static_cast<u8*>(malloc(ZSTD_DStreamOutSize())), MallocDeleter()),
            m_dctx(ZSTD_createDCtx(), ZstdDCtxDeleter())
     {
          if (!writeFn)
               THROW_FORMAT("ZstdStreamWriter: WriterFn callback cannot be null");
          if (!m_buffOut || !m_dctx)
               THROW_FORMAT("ZstdStreamWriter: failed to allocate resources");
     }

     ~ZstdStreamWriter() override
     {
          ZstdStreamWriter::close();
     }

     void close() override
     {
          if (isClosed()) return; // Idempotent close

          // Free resources
          m_dctx.reset();    // Calls ZSTD_freeDCtx()
          m_buffOut.reset(); // Calls free()
          m_writeFn = NULL;

          CloseableWriter::close(); // Mark as closed after all cleanups are done
     }

     void write(const u8* ptr, u64 sz) override
     {
          if (isClosed())
          {
               LOG_DEBUG("write() called on closed ZstdStreamWriter");
               return;
          }

          while (sz > 0)
          {
               const size_t readChunkSz = std::min(sz, m_buffInSize);
               ZSTD_inBuffer input = { ptr, readChunkSz, 0 };

               while (input.pos < input.size)
               {
                    ZSTD_outBuffer output = { m_buffOut.get(), m_buffOutSize, 0 };
                    size_t const ret = ZSTD_decompressStream(m_dctx.get(), &output, &input);

                    if (ZSTD_isError(ret))
                    {
                         const char* errorName = ZSTD_getErrorName(ret);
                         THROW_FORMAT("ZstdStreamWriter: decompress error: %s", errorName);
                    }

                    // Write decompressed data to callback writer immediately
                    if (output.pos > 0)
                    {
                         m_writeFn(m_buffOut.get(), output.pos);
                    }
               }

               sz -= readChunkSz;
               ptr += readChunkSz;
          }
     }

private:
     std::function<WriterFn> m_writeFn;
     size_t m_buffInSize;
     size_t m_buffOutSize;
     std::unique_ptr<u8, MallocDeleter> m_buffOut;
     std::unique_ptr<ZSTD_DCtx, ZstdDCtxDeleter> m_dctx;
};

// NCZBLOCK Stream Writer - handles streaming NCZBLOCK compression
class NczBlockStreamWriter : public CloseableWriter
{
public:
     NczBlockStreamWriter(const std::function<WriterFn>& writeFn)
          : m_writeFn(writeFn),
            m_headerParsed(false), m_blockSizesParsed(false),
            m_currentBlockIdx(0), m_currentBlockReadOffset(0)
     {
          if (!writeFn)
               THROW_FORMAT("NczBlockStreamWriter: WriterFn callback cannot be null");
     }

     ~NczBlockStreamWriter() override
     {
          NczBlockStreamWriter::close();
     }

     void close() override
     {
          if (isClosed()) return; // Idempotent close

          // Free resources
          if (m_currentBlockWriter)
          {
               m_currentBlockWriter->close(); // Flush remaining data to writerFn
               m_currentBlockWriter = NULL;
          }
          m_writeFn = NULL;

          CloseableWriter::close(); // Mark as closed after all cleanups are done
     }

     void write(const u8* ptr, u64 sz) override
     {
          if (isClosed())
          {
               LOG_DEBUG("write() called on closed NczBlockStreamWriter");
               return;
          }

          if (!sz) return; // no data

          // Phase 1: Parse and save NCZBLOCK header
          if (!m_headerParsed)
          {
               // Need to buffer the full static header
               // before we can save a copy of it
               const u64 header_size = sizeof(NczBlockHeader);

               if (m_buffer.size() < header_size)
               {
                    const u64 remainder = std::min(sz, header_size - m_buffer.size());
                    append(m_buffer, ptr, remainder);
                    ptr += remainder;
                    sz -= remainder;
               }

               if (m_buffer.size() < header_size)
               {
                    // assert sz == 0
                    return; // Need more data
               }

               // assert m_buffer.size() == header_size

               // Now we can save a copy of the header
               memcpy(&m_header, m_buffer.data(), header_size);
               if (!m_header.isValid())
               {
                    THROW_FORMAT("NczBlockStreamWriter: invalid NCZBLOCK header");
               }

               m_headerParsed = true;
               m_buffer.resize(0);
          }

          // Phase 2: Parse per-block compressed sizes array
          if (!m_blockSizesParsed)
          {
               // Need to buffer the full sizes array
               const u64 buffer_size = sizeof(u32) * m_header.numberOfBlocks;

               if (m_buffer.size() < buffer_size)
               {
                    const u64 remainder = std::min(sz, buffer_size - m_buffer.size());
                    append(m_buffer, ptr, remainder);
                    ptr += remainder;
                    sz -= remainder;
               }

               if (m_buffer.size() < buffer_size)
               {
                    // assert sz == 0
                    return; // Need more data
               }

               // assert m_buffer.size() == buffer_size

               // Read compressed block sizes
               m_blockSizes.resize(m_header.numberOfBlocks);
               memcpy(m_blockSizes.data(), m_buffer.data(), m_header.numberOfBlocks * sizeof(u32));

               m_blockSizesParsed = true;

               // All future writes will go directly to output
               m_buffer.clear(); // reclaim ok
          }

          // Phase 3: Decompress blocks
          while (sz)
          {
               // Starting a new block?
               if (!m_currentBlockWriter)
               {
                    // Out of blocks?
                    if (m_currentBlockIdx >= m_header.numberOfBlocks)
                    {
                         return;
                    }

                    m_currentBlockReadOffset = 0;

                    const u64 compressedSize = m_blockSizes[m_currentBlockIdx];

                    u64 expectedDecompSize = m_header.blockSize();
                    // If last block, adjust expected decompressed to remaining
                    if (m_currentBlockIdx == m_header.numberOfBlocks - 1)
                    {
                         const u64 remainder = m_header.decompressedSize % m_header.blockSize();
                         if (remainder > 0) expectedDecompSize = remainder;
                         // TODO Log if expected < compressed ?
                    }

                    if (m_header.usesZstd())
                    {
                         // Even when zstd flagged, if no compression achieved, assume uncompressed
                         if (compressedSize < expectedDecompSize)
                         {
                              m_currentBlockWriter = std::make_unique<ZstdStreamWriter>(m_writeFn);
                         }
                         else
                         {
                              LOG_DEBUG("[NczBlockStreamWriter] Block (%d) appears to have no compression - Using Direct Writer", m_currentBlockIdx);
                              m_currentBlockWriter = std::make_unique<DirectStreamWriter>(m_writeFn);
                         }
                    }
                    else
                    {
                         LOG_DEBUG("[NczBlockStreamWriter] Block (%d) has Unknown type (%d) - Using Direct Writer", m_currentBlockIdx, m_header.type);
                         m_currentBlockWriter = std::make_unique<DirectStreamWriter>(m_writeFn);
                    }
               }

               const u64 blockSize = m_blockSizes[m_currentBlockIdx];
               const u64 remaining = std::min(sz, blockSize - m_currentBlockReadOffset);

               m_currentBlockWriter->write(ptr, remaining);
               m_currentBlockReadOffset += remaining;
               ptr += remaining;
               sz -= remaining;

               if (m_currentBlockReadOffset >= blockSize)
               {
                    m_currentBlockWriter->close();
                    m_currentBlockWriter = NULL;
                    m_currentBlockIdx++;
               }
          }
     }

private:
     std::function<WriterFn> m_writeFn;

     NczBlockHeader m_header;
     bool m_headerParsed;
     bool m_blockSizesParsed;
     std::vector<u32> m_blockSizes;

     u64 m_currentBlockIdx;
     u64 m_currentBlockReadOffset;
     std::unique_ptr<CloseableWriter> m_currentBlockWriter;

     std::vector<u8> m_buffer; // For header + block-sizes parsing phases
};

// endregion

class NczBodyWriter : public NcaBodyWriter
{
public:
     NczBodyWriter(const NcmContentId& ncaId, u64 offset, std::shared_ptr<nx::ncm::ContentStorage>& contentStorage) : NcaBodyWriter(ncaId, offset, contentStorage)
     {
     }

     ~NczBodyWriter() override
     {
          NcaBodyWriter::close();
     }

protected:

     void doClose() override
     {
          // Free resources
          currentSectionCipher.reset(); // unique_ptr handles delete
          sections.clear(); // reclaim ok
          m_writer = NULL;
     }

     void doBeforeClose() override
     {
          if (m_writer)
          {
               m_writer->close(); // Flush remaining data to writerFn
          }
     }

private:

     // Find the section index for the specified offset
     // Returns -1 if none found
     int getSectionIndexForOffset(u64 offset)
     {
          for (size_t i = 0; i < sections.size(); i++)
          {
               if (offset >= sections[i].offset && offset < sections[i].offset + sections[i].size)
               {
                    return static_cast<int>(i);
               }
          }
          return -1;
     }

     // Find the next closest section index for the specified offset
     // Returns -1 if none found
     int getNextSectionIndexForOffset(u64 offset)
     {
          int nextSectionIdx = -1;
          u64 nextSectionOffset = std::numeric_limits<u64>::max();
          for (size_t i = 0; i < sections.size(); i++)
          {
               if (sections[i].offset > offset && sections[i].offset < nextSectionOffset)
               {
                    nextSectionIdx = static_cast<int>(i);
                    nextSectionOffset = sections[i].offset;
               }
          }
          return nextSectionIdx;
     }

     bool encrypt(u8* ptr, u64 sz, u64 offset)
     {
          while (sz)
          {
               int offsetSectionIdx = getSectionIndexForOffset(offset);
               u64 chunk = sz;

               if (offsetSectionIdx >= 0)
               {
                    NczSectionHeader* offsetSection = &sections[offsetSectionIdx];

                    // Create new context if section changed
                    if (offsetSectionIdx != currentSectionIdx) // -1 => true
                    {
                         currentSectionCipher.reset(); // Delete early as we may not re-assign
                         if (offsetSection->cryptoType == 3)
                         {
                              currentSectionCipher = std::make_unique<Aes128CtrCipher>(offsetSection->cryptoKey, offsetSection->cryptoCounter);
                         }
                         currentSectionIdx = offsetSectionIdx;
                    }

                    const u64 sectionEnd = offsetSection->offset + offsetSection->size;

                    // assert offset < sectionEnd

                    chunk = std::min<u64>(sz, sectionEnd - offset);

                    // assert chunk > 0

                    if (currentSectionCipher)
                    {
                         currentSectionCipher->encrypt(ptr, chunk, offset);
                    }
               }
               else
               {
                    // When an offset doesn't fall within a defined section,
                    // (i.e. Its not encrypted - Possibly padding),
                    // We look for the next closest section to determine
                    // how many unencrypted bytes to account for
                    int nextSectionIdx = getNextSectionIndexForOffset(offset);
                    if (nextSectionIdx >= 0)
                    {
                         NczSectionHeader* nextSection = &sections[nextSectionIdx];
                         u64 nextSectionStart = nextSection->offset;

                         // assert offset < nextSectionStart

                         chunk = std::min<u64>(sz, nextSectionStart - offset);
                    }
               }

               // assert chunk > 0

               offset += chunk;
               ptr += chunk;
               sz -= chunk;
          }

          return true;
     }

protected:

     void flushContentBuffer() override
     {
          if (isClosed())
          {
               LOG_DEBUG("flushContentBuffer() called on closed NczBodyWriter");
               return;
          }

          const u64 encryptOffset = m_offset;
          const u64 encryptSize = m_contentBuffer.size();

          if (encryptSize)
          {
               // Apply section-based encryption in-place before flushing
               encrypt(m_contentBuffer.data(), encryptSize, encryptOffset);
          }

          NcaBodyWriter::flushContentBuffer();
     }

public:

     void write(const u8* ptr, u64 sz) override
     {
          if (isClosed())
          {
               LOG_DEBUG("write() called on closed NczBodyWriter");
               return;
          }

          if (!sz) return; // no data

          // Phase 1: Parse NCZSECTN header
          if (!m_sectionsInitialized)
          {
               // Need to buffer enough to get the section count
               // to compute the total size of the header.
               if (m_buffer.size() < NczHeader::MIN_HEADER_SIZE)
               {
                    const u64 remainder = std::min(sz, NczHeader::MIN_HEADER_SIZE - m_buffer.size());
                    append(m_buffer, ptr, remainder);
                    ptr += remainder;
                    sz -= remainder;
               }

               if (m_buffer.size() < NczHeader::MIN_HEADER_SIZE)
               {
                    // assert sz == 0
                    return; // Need more data
               }

               // assert m_buffer.size() >= NczHeader::MIN_HEADER_SIZE

               auto header = (NczHeader*)m_buffer.data();
               if (!header->isValid())
               {
                    THROW_FORMAT("Invalid NCZ Header");
               }

               // Need to buffer the rest of the header before
               // we can extract the sections
               const u64 header_size = header->size(); // Compute once

               if (m_buffer.size() < header_size)
               {
                    const u64 remainder = std::min(sz, header_size - m_buffer.size());
                    append(m_buffer, ptr, remainder);
                    ptr += remainder;
                    sz -= remainder;
               }

               if (m_buffer.size() < header_size)
               {
                    // assert sz == 0
                    return; // Need more data
               }

               // assert m_buffer.size() == header_size

               // Now we can initialize the sections
               header = (NczHeader*)m_buffer.data();

               for (u64 i = 0; i < header->sectionCount(); i++)
               {
                    sections.push_back(header->section(i));
               }

               m_sectionsInitialized = true;
               m_buffer.resize(0);
          }

          // Phase 2: Detect body compression format
          if (!m_writer)
          {
               // Need to buffer enough to identify magic
               const u64 header_size = sizeof(NczBlockHeader::NCZBLOCK_MAGIC);

               if (m_buffer.size() < header_size)
               {
                    const u64 remainder = std::min(sz, header_size - m_buffer.size());
                    append(m_buffer, ptr, remainder);
                    ptr += remainder;
                    sz -= remainder;
               }

               if (m_buffer.size() < header_size)
               {
                    // assert sz == 0
                    return; // Need more data
               }

               // assert m_buffer.size() == header_size

               // Now we can check for magic
               u64 magic64; memcpy(&magic64, m_buffer.data(), sizeof(magic64));
               u32 magic32; memcpy(&magic32, m_buffer.data(), sizeof(magic32));

               if (magic32 == ZstdStreamWriter::ZSTD_MAGIC)
               {
                    m_writer = std::make_unique<ZstdStreamWriter>(getDirectWriterFn());
               }
               else if (magic64 == NczBlockHeader::NCZBLOCK_MAGIC)
               {
                    m_writer = std::make_unique<NczBlockStreamWriter>(getDirectWriterFn());
               }
               else
               {
                    LOG_DEBUG("[NczBodyWriter] Unknown NCZ Body Content - Using Direct Writer");
                    m_writer = std::make_unique<DirectStreamWriter>(getDirectWriterFn());
               }

               // Flush buffer now because
               // future writes will go directly to m_writer
               m_writer->write(m_buffer.data(), m_buffer.size());
               m_buffer.clear(); // reclaim ok
          }

          // Phase 3: Forward all data to body processor
          // We'll encrypt processed data later in flushContentBuffer
          if (sz > 0)
          {
               m_writer->write(ptr, sz);
          }
     }

private:
     std::unique_ptr<CloseableWriter> m_writer;

     std::vector<u8> m_buffer;

     bool m_sectionsInitialized = false;

     std::vector<NczSectionHeader> sections;
     std::unique_ptr<Aes128CtrCipher> currentSectionCipher; // Crypto cipher for current section
     int currentSectionIdx = -1; // Track which section the cipher is for
};

// region NcaWriter Methods

NcaWriter::NcaWriter(const NcmContentId& ncaId, std::shared_ptr<nx::ncm::ContentStorage>& contentStorage) : m_ncaId(ncaId), m_contentStorage(contentStorage), m_writer(NULL)
{
}

NcaWriter::~NcaWriter()
{
     NcaWriter::close();
}

void NcaWriter::close()
{
     if (isClosed()) return; // Idempotent close

     if (m_writer)
     {
          m_writer->close();
          m_writer = NULL;
     }
     else if (!m_buffer.empty())
     {
          if (m_contentStorage)
          {
               flushHeader();
          }

          m_buffer.clear(); // reclaim ok
     }
     m_contentStorage = NULL;

     CloseableWriter::close(); // Mark as closed after all cleanups are done
}

void NcaWriter::write(const  u8* ptr, u64 sz)
{
     if (isClosed())
     {
          LOG_DEBUG("write() called on closed NcaWriter");
          return;
     }

     if (!sz) return; // no data

     if (!m_headerFlushed)
     {
          // Need to buffer the full header
          // before we can flush it
          if (m_buffer.size() < NCA_HEADER_SIZE)
          {
               const u64 remainder = std::min(sz, NCA_HEADER_SIZE - m_buffer.size());
               append(m_buffer, ptr, remainder);

               ptr += remainder;
               sz -= remainder;
          }

          if (m_buffer.size() < NCA_HEADER_SIZE)
          {
               // assert sz == 0
               return; // Need more data
          }

          // assert m_buffer.size() == NCA_HEADER_SIZE

          // Now we can flush the header
          flushHeader();
          m_headerFlushed = true;
          m_buffer.resize(0);
     }

     if (!sz) return; // no data

     if (!m_writer)
     {
          const u64 header_size = sizeof(NczHeader::MAGIC);

          // Need to buffer enough to identify magic headers
          if (m_buffer.size() < header_size)
          {
               const u64 remainder = std::min(sz, header_size - m_buffer.size());
               append(m_buffer, ptr, remainder);

               ptr += remainder;
               sz -= remainder;
          }

          if (m_buffer.size() < header_size)
          {
               // assert sz == 0
               return; // Need more data
          }

          // assert m_buffer.size() == header_size

          u64 magic; memcpy(&magic, m_buffer.data(), sizeof(magic));

          if (magic == NczHeader::MAGIC)
          {
               // NOTE: Don't clear header, it needs to be written to m_write for downstream consumption
               m_writer = std::make_shared<NczBodyWriter>(m_ncaId, NCA_HEADER_SIZE, m_contentStorage);
          }
          else
          {
               m_writer = std::make_shared<NcaBodyWriter>(m_ncaId, NCA_HEADER_SIZE, m_contentStorage);
          }

          // assert !m_buffer.empty()

          // Flush buffer now because
          // future writes will go directly to m_writer
          m_writer->write(m_buffer.data(), m_buffer.size());
          m_buffer.clear(); // reclaim ok
     }

     // assert m_writer
     // assert buffer.empty()

     if (!sz) return; // no data

     m_writer->write(ptr, sz);
}

void NcaWriter::flushHeader()
{
     if (isClosed())
     {
          LOG_DEBUG("flushHeader() called on closed NcaWriter");
          return;
     }

     tin::install::NcaHeader header;

     if (m_buffer.size() < sizeof(header))
     {
          LOG_DEBUG("Insufficient data to flush NCA header");
          return;
     }

     memcpy(&header, m_buffer.data(), sizeof(header));
     Crypto::AesXtr decryptor(Crypto::Keys().headerKey, false);
     Crypto::AesXtr encryptor(Crypto::Keys().headerKey, true);
     decryptor.decrypt(&header, &header, sizeof(header), 0, 0x200);

     if (header.magic == MAGIC_NCA3)
     {
          if (m_contentStorage)
          {
               m_contentStorage->CreatePlaceholder(m_ncaId, *(NcmPlaceHolderId*)&m_ncaId, header.nca_size);
          }
     }
     else
     {
          THROW_FORMAT("Invalid NCA magic");
     }

     if (header.distribution == 1)
     {
          header.distribution = 0;
     }
     encryptor.encrypt(m_buffer.data(), &header, sizeof(header), 0, 0x200);

     if (m_contentStorage)
     {
          m_contentStorage->WritePlaceholder(*(NcmPlaceHolderId*)&m_ncaId, 0, m_buffer.data(), m_buffer.size());
     }
}

// endregion

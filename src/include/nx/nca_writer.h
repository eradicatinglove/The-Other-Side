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

#pragma once
#include <functional>
#include <switch.h>
#include <vector>
#include "nx/ncm.hpp"
#include <memory>

class CloseableWriter
{
public:
    // If subsclass overrides close(), class destructor expected to call $CLASS::close()
	virtual ~CloseableWriter() { CloseableWriter::close(); }

	virtual void write(const u8* data, u64 size) = 0;

	// overrides should be idempotent
	// If subclass overrides, override expected to call $PARENT::close()
	virtual void close() { m_isClosed = true; }

	bool isClosed() const { return m_isClosed; }

private:
	bool m_isClosed = false;
};

// Simple call-back Writer with no life-cycle methods
using WriterFn = void(const u8* data, u64 size);

class NcaBodyWriter : public CloseableWriter, public std::enable_shared_from_this<NcaBodyWriter>
{
public:
	static constexpr u64 CONTENT_BUFFER_SIZE = 0x800000; // 8MB

	NcaBodyWriter(const NcmContentId& ncaId, u64 offset, std::shared_ptr<nx::ncm::ContentStorage>& contentStorage);
	~NcaBodyWriter() override;

	void write(const  u8* ptr, u64 sz) override;

	// Final - Subsclasses should override doBeforeClose() + doClose()
	void close() final;

	// Returns a Writer adapter that bypasses virtual dispatch and calls
	// NcaBodyWriter::write() directly, even when called on derived classes.
	// LIFETIME: Returned fn holds a shared_ptr to `this`.
	//           As such, NcaBodyWriter must be managed by a shared_ptr at call time.
	std::function<WriterFn> getDirectWriterFn();

protected:

	// Will be called before final flushContentBuffer()
	// Subclasses should close/flush their writer(s)
	virtual void doBeforeClose() {}

	// Will be called before freeing internal resources
	// Subsclasses should free their resourses
	virtual void doClose() {}

	// Subclasses who ovveride are expected to invoke $PARENT::flushContentBuffer()
	// To *actually* flush the buffer to content storage
	virtual void flushContentBuffer();

	std::vector<u8> m_contentBuffer;
	std::shared_ptr<nx::ncm::ContentStorage> m_contentStorage;
	NcmContentId m_ncaId;

	u64 m_offset;
};

class NcaWriter : public CloseableWriter
{
public:
	NcaWriter(const NcmContentId& ncaId, std::shared_ptr<nx::ncm::ContentStorage>& contentStorage);
	~NcaWriter() override;

	void close() override;
	void write(const  u8* ptr, u64 sz) override;
	void flushHeader();

protected:
	NcmContentId m_ncaId;
	std::shared_ptr<nx::ncm::ContentStorage> m_contentStorage;
	std::vector<u8> m_buffer;
	std::shared_ptr<NcaBodyWriter> m_writer; // Must be shared_ptr for getDirectWriterFn to work
	bool m_headerFlushed = false;
};

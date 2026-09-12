#include "tjsCommHead.h"

#include "SevenZipArchive.h"

#include "KrkrNSLog.h"
#include "KrkrNSSlowOperation.h"
#include "SharedMemoryStream.h"
#include "MsgIntf.h"
#include "StorageIntf.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

extern "C"
{
#include "7zip/7z.h"
#include "7zip/7zCrc.h"
}

namespace
{
constexpr size_t kSevenZipReadCacheSize = 64 * 1024;
constexpr Byte kSevenZipSignature[] = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};

static ISzAlloc SevenZipAllocator = {
	[](ISzAllocPtr, size_t size) -> void * { return std::malloc(size); },
	[](ISzAllocPtr, void *address) { std::free(address); }
};

bool IsSevenZipStream(tTJSBinaryStream *stream)
{
	if (!stream) return false;
	const tjs_uint64 original = stream->GetPosition();
	Byte signature[sizeof(kSevenZipSignature)] = {};
	const tjs_uint read = stream->Read(signature, sizeof(signature));
	stream->SetPosition(original);
	return read == sizeof(signature) &&
		std::memcmp(signature, kSevenZipSignature, sizeof(signature)) == 0;
}

// One most-recent solid block across all open archives, capped at 16 MiB.
// Streams retain their own shared reference when this slot is replaced. The
// cache dies with the last archive, including during an engine restart.
struct SevenZipBlockCache
{
	std::mutex Mutex;
	const void *Archive = nullptr;
	UInt32 Folder = 0xffffffffU;
	KrkrSharedBytes Bytes;
};

static std::shared_ptr<SevenZipBlockCache> GetSevenZipBlockCache()
{
	static std::mutex mutex;
	static std::weak_ptr<SevenZipBlockCache> weak;
	std::lock_guard<std::mutex> lock(mutex);
	auto cache = weak.lock();
	if (!cache) { cache = std::make_shared<SevenZipBlockCache>(); weak = cache; }
	return cache;
}

class tTVPSevenZipArchive final : public tTVPArchive
{
	struct tArchiveInputStream : public ISeekInStream
	{
		tTVPSevenZipArchive *Owner;
	};

	CSzArEx Database;
	tTJSBinaryStream *Stream;
	tArchiveInputStream ArchiveStream;
	CLookToRead2 LookStream;
	Byte ReadCache[kSevenZipReadCacheSize];
	std::vector<std::pair<ttstr, tjs_uint>> Files;
	std::shared_ptr<SevenZipBlockCache> Cache = GetSevenZipBlockCache();

	SRes Read(void *buffer, size_t *size)
	{
		const size_t requested = *size;
		const tjs_uint chunk = requested > std::numeric_limits<tjs_uint>::max()
			? std::numeric_limits<tjs_uint>::max()
			: static_cast<tjs_uint>(requested);
		*size = Stream->Read(buffer, chunk);
		// A short read, including zero at EOF, is valid for ISeekInStream.
		return SZ_OK;
	}

	SRes Seek(Int64 *position, ESzSeek origin)
	{
		tjs_int whence;
		switch (origin)
		{
		case SZ_SEEK_SET: whence = TJS_BS_SEEK_SET; break;
		case SZ_SEEK_CUR: whence = TJS_BS_SEEK_CUR; break;
		case SZ_SEEK_END: whence = TJS_BS_SEEK_END; break;
		default: return SZ_ERROR_PARAM;
		}
		try
		{
			*position = static_cast<Int64>(Stream->Seek(*position, whence));
			return SZ_OK;
		}
		catch (...)
		{
			return SZ_ERROR_READ;
		}
	}

public:
	tTVPSevenZipArchive(const ttstr &name, tTJSBinaryStream *stream)
		: tTVPArchive(name), Stream(stream)
	{
		ArchiveStream.Owner = this;
		ArchiveStream.Read = [](ISeekInStreamPtr input, void *buffer, size_t *size) -> SRes
		{
			return static_cast<const tArchiveInputStream *>(input)->Owner->Read(buffer, size);
		};
		ArchiveStream.Seek = [](ISeekInStreamPtr input, Int64 *position, ESzSeek origin) -> SRes
		{
			return static_cast<const tArchiveInputStream *>(input)->Owner->Seek(position, origin);
		};
		LookToRead2_CreateVTable(&LookStream, False);
		LookToRead2_INIT(&LookStream);
		LookStream.realStream = &ArchiveStream;
		LookStream.buf = ReadCache;
		LookStream.bufSize = sizeof(ReadCache);
		SzArEx_Init(&Database);
		if (!g_CrcTable[1]) CrcGenerateTable();
	}

	~tTVPSevenZipArchive() override
	{
		{
			std::lock_guard<std::mutex> lock(Cache->Mutex);
			if (Cache->Archive == this)
			{
				Cache->Bytes = {};
				Cache->Archive = nullptr;
			}
		}
		SzArEx_Free(&Database, &SevenZipAllocator);
		delete Stream;
	}

	SRes Open()
	{
		const SRes result = SzArEx_Open(
			&Database, &LookStream.vt, &SevenZipAllocator, &SevenZipAllocator);
		if (result != SZ_OK) return result;

		Files.reserve(Database.NumFiles);
		for (UInt32 i = 0; i < Database.NumFiles; ++i)
		{
			if (SzArEx_IsDir(&Database, i)) continue;
			const size_t length = SzArEx_GetFileNameUtf16(&Database, i, nullptr);
			if (length == 0 || length - 1 > static_cast<size_t>(std::numeric_limits<tjs_int>::max()))
				continue;
			std::vector<UInt16> name_buffer(length);
			SzArEx_GetFileNameUtf16(&Database, i, name_buffer.data());
			ttstr filename = TVPStringFromBMPUnicode(
				reinterpret_cast<const tjs_uint16 *>(name_buffer.data()),
				static_cast<tjs_int>(length - 1));
			NormalizeInArchiveStorageName(filename);
			Files.emplace_back(filename, static_cast<tjs_uint>(i));
		}
		std::sort(Files.begin(), Files.end(),
			[](const std::pair<ttstr, tjs_uint> &left,
			   const std::pair<ttstr, tjs_uint> &right)
			{
				return left.first < right.first;
			});
		return SZ_OK;
	}

	tjs_uint GetCount() override { return static_cast<tjs_uint>(Files.size()); }
	ttstr GetName(tjs_uint index) override { return Files[index].first; }

	tTJSBinaryStream *CreateStreamByIndex(tjs_uint index) override
	{
		if (index >= Files.size()) return nullptr;
		const UInt32 file_index = static_cast<UInt32>(Files[index].second);
		const UInt64 file_size = SzArEx_GetFileSize(&Database, file_index);
		if (file_size > static_cast<UInt64>(std::numeric_limits<size_t>::max())) return nullptr;
		std::lock_guard<std::mutex> lock(Cache->Mutex);
		const UInt32 folder = Database.FileToFolder[file_index];
		if (folder != 0xffffffffU && Cache->Archive == this && Cache->Folder == folder)
		{
			// Same bounds and per-file CRC checks as SzArEx_Extract's cached
			// path. Do not pass shared immutable storage to its realloc/free path.
			const UInt64 offset64 = Database.UnpackPositions[file_index] -
				Database.UnpackPositions[Database.FolderToFile[folder]];
			if (offset64 > Cache->Bytes.size || file_size > Cache->Bytes.size - offset64)
				return nullptr;
			auto bytes = Cache->Bytes.Slice(static_cast<size_t>(offset64), static_cast<size_t>(file_size));
			if (SzBitWithVals_Check(&Database.CRCs, file_index) &&
				CrcCalc(bytes.data, bytes.size) != Database.CRCs.Vals[file_index]) return nullptr;
			return new tTVPSharedMemoryStream(std::move(bytes));
		}
		KrkrNSSlowOperation slow("7z-decode");

		UInt32 block_index = 0xffffffffU;
		Byte *output = nullptr;
		size_t output_size = 0;
		size_t offset = 0;
		size_t processed = 0;
		SRes result;
		try
		{
			result = SzArEx_Extract(
				&Database, &LookStream.vt, file_index, &block_index, &output,
				&output_size, &offset, &processed, &SevenZipAllocator, &SevenZipAllocator);
		}
		catch (...)
		{
			ISzAlloc_Free(&SevenZipAllocator, output);
			throw;
		}
		if (result != SZ_OK || processed != static_cast<size_t>(file_size) ||
			offset > output_size || processed > output_size - offset)
		{
			KRKRNS_LOG("[7z] extract failed: index=%u result=%d size=%llu processed=%llu",
				static_cast<unsigned>(file_index), static_cast<int>(result),
				static_cast<unsigned long long>(file_size),
				static_cast<unsigned long long>(processed));
			ISzAlloc_Free(&SevenZipAllocator, output);
			return nullptr;
		}
		KrkrSharedBytes bytes{
			std::shared_ptr<const void>(output, [](const void *p) {
				ISzAlloc_Free(&SevenZipAllocator, const_cast<void *>(p));
			}), output, output_size};
		auto stream = std::make_unique<tTVPSharedMemoryStream>(bytes.Slice(offset, processed));
		if (folder != 0xffffffffU && output_size <= 16 * 1024 * 1024)
		{
			Cache->Bytes = std::move(bytes);
			Cache->Archive = this;
			Cache->Folder = folder;
		}
		return stream.release();
	}
};
}

bool TVPIs7ZArchive(const ttstr &name)
{
	tTJSBinaryStream *stream = nullptr;
	try
	{
		stream = TVPCreateStream(name);
		const bool result = IsSevenZipStream(stream);
		delete stream;
		return result;
	}
	catch (...)
	{
		delete stream;
		return false;
	}
}

tTVPArchive *TVPOpen7ZArchive(const ttstr &name, tTJSBinaryStream *stream)
{
	if (!IsSevenZipStream(stream)) return nullptr;

	auto *archive = new tTVPSevenZipArchive(name, stream);
	KRKRNS_LOG("[7z] opening archive: %s", [](const ttstr& n) {
		std::string utf8;
		for (tjs_uint i = 0; i < n.GetLen() && i < 200; ++i)
		{
			tjs_uint32 ch = static_cast<tjs_uint32>(n[i]);
			if (ch < 0x80) utf8 += static_cast<char>(ch);
			else if (ch < 0x800)
			{
				utf8 += static_cast<char>(0xC0 | (ch >> 6));
				utf8 += static_cast<char>(0x80 | (ch & 0x3F));
			}
			else
			{
				utf8 += static_cast<char>(0xE0 | (ch >> 12));
				utf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
				utf8 += static_cast<char>(0x80 | (ch & 0x3F));
			}
		}
		return utf8;
	}(name).c_str());
	const SRes result = archive->Open();
	if (result != SZ_OK)
	{
		KRKRNS_LOG("[7z] cannot open archive: result=%d", static_cast<int>(result));
		delete archive; // also consumes stream once the 7z signature matched
		TVPThrowExceptionMessage(TJS_W("Cannot open 7-Zip archive"));
	}
	KRKRNS_LOG("[7z] opened archive: files=%u", static_cast<unsigned>(archive->GetCount()));
	return archive;
}

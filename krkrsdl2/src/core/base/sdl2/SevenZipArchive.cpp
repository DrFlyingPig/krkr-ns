#include "tjsCommHead.h"

#include "SevenZipArchive.h"

#include "KrkrNSLog.h"
#include "MsgIntf.h"
#include "StorageIntf.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
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

// tTVPMemoryStream treats a supplied buffer as a non-owning reference.  The
// LZMA SDK returns an owning malloc allocation, so keep that ownership explicit
// and expose only the requested file's slice from a possibly solid block.
class tTVPSevenZipMemoryStream final : public tTJSBinaryStream
{
	Byte *Allocation;
	const Byte *Data;
	size_t Size;
	size_t Position;

public:
	tTVPSevenZipMemoryStream(Byte *allocation, const Byte *data, size_t size)
		: Allocation(allocation), Data(data), Size(size), Position(0) {}

	~tTVPSevenZipMemoryStream() override
	{
		ISzAlloc_Free(&SevenZipAllocator, Allocation);
	}

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) override
	{
		tjs_int64 base = 0;
		switch (whence)
		{
		case TJS_BS_SEEK_SET: base = 0; break;
		case TJS_BS_SEEK_CUR:
			if (Position > static_cast<size_t>(std::numeric_limits<tjs_int64>::max())) return Position;
			base = static_cast<tjs_int64>(Position);
			break;
		case TJS_BS_SEEK_END:
			if (Size > static_cast<size_t>(std::numeric_limits<tjs_int64>::max())) return Position;
			base = static_cast<tjs_int64>(Size);
			break;
		default:
			return Position;
		}
		if ((offset > 0 && base > std::numeric_limits<tjs_int64>::max() - offset) ||
			(offset < 0 && base < std::numeric_limits<tjs_int64>::min() - offset))
			return Position;
		const tjs_int64 next = base + offset;
		if (next < 0 || static_cast<tjs_uint64>(next) > static_cast<tjs_uint64>(Size))
			return Position;
		Position = static_cast<size_t>(next);
		return Position;
	}

	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size) override
	{
		const size_t available = Size - Position;
		const size_t amount = std::min<size_t>(available, read_size);
		if (amount != 0)
			std::memcpy(buffer, Data + Position, amount);
		Position += amount;
		return static_cast<tjs_uint>(amount);
	}

	tjs_uint TJS_INTF_METHOD Write(const void *, tjs_uint) override { return 0; }
	tjs_uint64 TJS_INTF_METHOD GetSize() override { return Size; }
};

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

		UInt32 block_index = 0xffffffffU;
		Byte *output = nullptr;
		size_t output_size = 0;
		size_t offset = 0;
		size_t processed = 0;
		KRKRNS_LOG("[7z] extract begin index=%u size=%llu",
			static_cast<unsigned>(file_index),
			static_cast<unsigned long long>(file_size));
		const SRes result = SzArEx_Extract(
			&Database, &LookStream.vt, file_index, &block_index, &output,
			&output_size, &offset, &processed, &SevenZipAllocator, &SevenZipAllocator);
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
		KRKRNS_LOG("[7z] extract done index=%u processed=%llu",
			static_cast<unsigned>(file_index),
			static_cast<unsigned long long>(processed));
		return new tTVPSevenZipMemoryStream(output, output ? output + offset : nullptr, processed);
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

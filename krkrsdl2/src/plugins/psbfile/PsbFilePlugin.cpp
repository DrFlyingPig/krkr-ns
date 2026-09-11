#include "tjsCommHead.h"
#include "PsbFilePlugin.h"

#include "CharacterSet.h"
#include "KrkrNSLog.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "tjsArray.h"
#include "tjsDictionary.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

namespace
{
using ByteVector = std::vector<tjs_uint8>;

// psbfile.dll exposes the resources embedded in a PSB through URLs such as
// psb://common.pimg/93.tlg.  A process-wide media object is intentional: KAG
// invalidates the PSBFile instance immediately after creating its layers, but
// those layers load their images asynchronously afterwards.
class tTVPPsbMedia final : public iTVPStorageMedia
{
	std::atomic<tjs_uint> RefCount;
	std::mutex Mutex;
	std::map<tjs_string, ByteVector> Resources;
	std::set<tjs_string> Containers;

	static tjs_string Normalized(const ttstr &value)
	{
		ttstr result(value);
		result.ToLowerCase();
		return result.AsStdString();
	}

public:
	tTVPPsbMedia() : RefCount(1) {}

	void TJS_INTF_METHOD AddRef() override { ++RefCount; }
	void TJS_INTF_METHOD Release() override
	{
		if (--RefCount == 0) delete this;
	}

	void TJS_INTF_METHOD GetName(ttstr &name) override { name = TJS_W("psb"); }
	void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) override { name.ToLowerCase(); }
	void TJS_INTF_METHOD NormalizePathName(ttstr &name) override { name.ToLowerCase(); }

	bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) override
	{
		const tjs_string key = Normalized(name);
		const size_t slash = key.find(TJS_W('/'));
		if (slash == tjs_string::npos || slash == 0) return false;
		const tjs_string containerKey = key.substr(0, slash);
		{
			std::lock_guard<std::mutex> lock(Mutex);
			if (Resources.find(key) != Resources.end()) return true;
			// A known container simply does not contain this extension/name.
			// Do not reparse it for every candidate tried by Layer.loadImages.
			if (Containers.find(containerKey) != Containers.end()) return false;
		}

		// Match psbfile's lazy-media behavior: scripts are allowed to refer to
		// psb://window.pimg/5.tlg before creating a PSBFile for window.pimg.
		// Loading while holding Mutex would deadlock when Load() commits the
		// parsed resources, so retry the lookup after the temporary loader exits.
		const ttstr container(containerKey);
		KRKRNS_LOG("[psb] lazy loading container=%s for resource=%s",
			container.AsNarrowStdString().c_str(), name.AsNarrowStdString().c_str());
		tTJSNI_PSBFile file;
		if (!file.Load(container)) return false;

		std::lock_guard<std::mutex> lock(Mutex);
		return Resources.find(key) != Resources.end();
	}

	tTJSBinaryStream *TJS_INTF_METHOD Open(const ttstr &name, tjs_uint32 flags) override
	{
		if ((flags & TJS_BS_ACCESS_MASK) != TJS_BS_READ)
			throw std::runtime_error("PSB media is read-only");

		ByteVector copy;
		{
			std::lock_guard<std::mutex> lock(Mutex);
			auto found = Resources.find(Normalized(name));
			if (found == Resources.end())
				throw std::runtime_error("PSB resource was not found");
			copy = found->second;
		}

		auto *stream = new tTVPMemoryStream();
		if (!copy.empty()) stream->WriteBuffer(copy.data(), static_cast<tjs_uint>(copy.size()));
		stream->SetPosition(0);
		return stream;
	}

	void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister *lister) override
	{
		const tjs_string prefix = Normalized(name);
		std::lock_guard<std::mutex> lock(Mutex);
		for (const auto &entry : Resources)
		{
			if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
			tjs_string tail = entry.first.substr(prefix.size());
			while (!tail.empty() && tail.front() == TJS_W('/')) tail.erase(tail.begin());
			if (!tail.empty() && tail.find(TJS_W('/')) == tjs_string::npos)
				lister->Add(ttstr(tail));
		}
	}

	void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) override { name.Clear(); }

	void Commit(const ttstr &container, const std::map<tjs_string, ByteVector> &resources)
	{
		ttstr normalizedContainer(container);
		normalizedContainer.ToLowerCase();
		const tjs_string prefix = normalizedContainer.AsStdString() + TJS_W("/");

		std::lock_guard<std::mutex> lock(Mutex);
		Containers.insert(normalizedContainer.AsStdString());
		for (const auto &entry : resources)
		{
			ttstr normalizedName(entry.first);
			normalizedName.ToLowerCase();
			Resources[prefix + normalizedName.AsStdString()] = entry.second;
		}
		KRKRNS_LOG("[psb] registered container=%s resources=%u",
			container.AsNarrowStdString().c_str(),
			static_cast<unsigned>(resources.size()));
	}

	/* Drop every parsed container/resource.  Called when a game session ends
	 * so the next game does not inherit (or keep resident) its PSB data. */
	void Clear()
	{
		std::lock_guard<std::mutex> lock(Mutex);
		Resources.clear();
		Containers.clear();
		KRKRNS_LOG("[psb] resources cleared");
	}
};

static tTVPPsbMedia *PsbMedia = nullptr;
static std::once_flag PsbMediaOnce;

static void EnsurePsbMedia()
{
	std::call_once(PsbMediaOnce, []() {
		PsbMedia = new tTVPPsbMedia();
		TVPRegisterStorageMedia(PsbMedia);
		PsbMedia->Release(); // the storage media manager owns the live reference
		KRKRNS_LOG("[psb] psb:// storage media registered");
	});
}

struct PsbHeader
{
	tjs_uint16 Version = 0;
	tjs_uint16 Encrypt = 0;
	tjs_uint32 OffsetEncrypt = 0;
	tjs_uint32 OffsetNames = 0;
	tjs_uint32 OffsetStrings = 0;
	tjs_uint32 OffsetStringsData = 0;
	tjs_uint32 OffsetChunkOffsets = 0;
	tjs_uint32 OffsetChunkLengths = 0;
	tjs_uint32 OffsetChunkData = 0;
	tjs_uint32 OffsetEntries = 0;
	tjs_uint32 Checksum = 0;
	tjs_uint32 OffsetExtraChunkOffsets = 0;
	tjs_uint32 OffsetExtraChunkLengths = 0;
	tjs_uint32 OffsetExtraChunkData = 0;
};

struct PsbArray
{
	std::vector<tjs_uint32> Values;
	size_t End = 0;
};

struct ParsedValue
{
	tTJSVariant Value;
	bool IsResource = false;
	tjs_uint32 ResourceIndex = 0;
	bool IsExtraResource = false;
};

class PsbReader
{
	ByteVector Data;
	PsbHeader Header;
	std::vector<tjs_string> Names;
	std::vector<tjs_string> Strings;
	std::vector<tjs_uint32> ChunkOffsets;
	std::vector<tjs_uint32> ChunkLengths;
	std::vector<tjs_uint32> ExtraChunkOffsets;
	std::vector<tjs_uint32> ExtraChunkLengths;
	std::map<tjs_uint32, tjs_string> ResourceNames;
	std::map<tjs_uint32, tjs_string> ExtraResourceNames;

	void Require(size_t offset, size_t length, const char *what) const
	{
		if (offset > Data.size() || length > Data.size() - offset)
			throw std::runtime_error(std::string("PSB out of range while reading ") + what);
	}

	tjs_uint64 ReadUInt(size_t offset, size_t width, const char *what) const
	{
		if (width > 8) throw std::runtime_error("Unsupported PSB integer width");
		Require(offset, width, what);
		tjs_uint64 value = 0;
		for (size_t i = 0; i < width; ++i)
			value |= static_cast<tjs_uint64>(Data[offset + i]) << (i * 8);
		return value;
	}

	tjs_uint16 Read16(size_t offset, const char *what) const
	{
		return static_cast<tjs_uint16>(ReadUInt(offset, 2, what));
	}

	tjs_uint32 Read32(size_t offset, const char *what) const
	{
		return static_cast<tjs_uint32>(ReadUInt(offset, 4, what));
	}

	PsbArray ReadArray(size_t offset) const
	{
		Require(offset, 1, "array tag");
		const int countWidth = static_cast<int>(Data[offset++]) - 0x0c;
		if (countWidth < 1 || countWidth > 8)
			throw std::runtime_error("Invalid PSB array count tag");
		const tjs_uint64 count64 = ReadUInt(offset, static_cast<size_t>(countWidth), "array count");
		offset += static_cast<size_t>(countWidth);
		if (count64 > 0x1000000u) throw std::runtime_error("PSB array is unreasonably large");

		Require(offset, 1, "array entry tag");
		const int entryWidth = static_cast<int>(Data[offset++]) - 0x0c;
		// PSB encodes the element width as an integer tag.  Empty arrays
		// legitimately use NUMBER_N0 (0x0c), which means a zero-byte width.
		if (entryWidth < 0 || entryWidth > 8)
			throw std::runtime_error("Invalid PSB array entry tag");

		PsbArray result;
		result.Values.reserve(static_cast<size_t>(count64));
		for (tjs_uint64 i = 0; i < count64; ++i)
		{
			const tjs_uint64 value = ReadUInt(offset, static_cast<size_t>(entryWidth), "array entry");
			if (value > 0xffffffffu) throw std::runtime_error("PSB array entry exceeds 32 bits");
			result.Values.push_back(static_cast<tjs_uint32>(value));
			offset += static_cast<size_t>(entryWidth);
		}
		result.End = offset;
		return result;
	}

	static tjs_string Utf8(const char *data, size_t length)
	{
		tjs_string result;
		if (length == 0) return result;
		if (!TVPUtf8ToUtf16(result, std::string(data, length)))
			throw std::runtime_error("Invalid UTF-8 in PSB string table");
		return result;
	}

	tjs_string ReadZeroString(size_t offset) const
	{
		Require(offset, 1, "string");
		size_t end = offset;
		while (end < Data.size() && Data[end] != 0) ++end;
		if (end == Data.size()) throw std::runtime_error("Unterminated PSB string");
		return Utf8(reinterpret_cast<const char *>(Data.data() + offset), end - offset);
	}

	void ParseHeader()
	{
		Require(0, 40, "header");
		if (std::memcmp(Data.data(), "PSB\0", 4) != 0)
			throw std::runtime_error("Not a PSB stream");

		Header.Version = Read16(4, "version");
		Header.Encrypt = Read16(6, "encryption flags");
		Header.OffsetEncrypt = Read32(8, "encryption offset");
		Header.OffsetNames = Read32(12, "names offset");
		Header.OffsetStrings = Read32(16, "strings offset");
		Header.OffsetStringsData = Read32(20, "string data offset");
		Header.OffsetChunkOffsets = Read32(24, "chunk offsets");
		Header.OffsetChunkLengths = Read32(28, "chunk lengths");
		Header.OffsetChunkData = Read32(32, "chunk data");
		Header.OffsetEntries = Read32(36, "entries offset");
		if (Header.Version >= 3)
		{
			Require(40, 4, "checksum");
			Header.Checksum = Read32(40, "checksum");
		}
		if (Header.Version >= 4)
		{
			Require(44, 12, "extra chunk header");
			Header.OffsetExtraChunkOffsets = Read32(44, "extra chunk offsets");
			Header.OffsetExtraChunkLengths = Read32(48, "extra chunk lengths");
			Header.OffsetExtraChunkData = Read32(52, "extra chunk data");
		}
		if (Header.Version < 1 || Header.Version > 4)
			throw std::runtime_error("Unsupported PSB version");
	}

	void LoadNames()
	{
		if (Header.Version == 1)
		{
			const PsbArray indexes = ReadArray(Header.OffsetEncrypt);
			Names.reserve(indexes.Values.size());
			for (tjs_uint32 index : indexes.Values)
				Names.push_back(ReadZeroString(static_cast<size_t>(Header.OffsetNames) + index));
			return;
		}

		const PsbArray charset = ReadArray(Header.OffsetNames);
		const PsbArray namesData = ReadArray(charset.End);
		const PsbArray nameIndexes = ReadArray(namesData.End);
		Names.reserve(nameIndexes.Values.size());

		for (tjs_uint32 index : nameIndexes.Values)
		{
			if (index >= namesData.Values.size()) throw std::runtime_error("Invalid PSB name index");
			tjs_uint32 chr = namesData.Values[index];
			std::string bytes;
			for (size_t guard = 0; chr != 0; ++guard)
			{
				if (guard > namesData.Values.size() || chr >= namesData.Values.size())
					throw std::runtime_error("Invalid PSB name tree");
				const tjs_uint32 code = namesData.Values[chr];
				if (code >= charset.Values.size()) throw std::runtime_error("Invalid PSB character table");
				const tjs_uint32 delta = charset.Values[code];
				if (chr < delta || chr - delta > 0xffu) throw std::runtime_error("Invalid PSB name byte");
				bytes.push_back(static_cast<char>(chr - delta));
				chr = code;
			}
			std::reverse(bytes.begin(), bytes.end());
			Names.push_back(Utf8(bytes.data(), bytes.size()));
		}
	}

	void LoadStrings()
	{
		const PsbArray offsets = ReadArray(Header.OffsetStrings);
		Strings.reserve(offsets.Values.size());
		for (tjs_uint32 offset : offsets.Values)
			Strings.push_back(ReadZeroString(static_cast<size_t>(Header.OffsetStringsData) + offset));
	}

	ByteVector ResourceBytes(tjs_uint32 index, bool extra) const
	{
		const auto &offsets = extra ? ExtraChunkOffsets : ChunkOffsets;
		const auto &lengths = extra ? ExtraChunkLengths : ChunkLengths;
		const tjs_uint32 base = extra ? Header.OffsetExtraChunkData : Header.OffsetChunkData;
		if (index >= offsets.size() || index >= lengths.size())
			throw std::runtime_error("Invalid PSB resource index");
		const size_t begin = static_cast<size_t>(base) + offsets[index];
		const size_t length = lengths[index];
		Require(begin, length, "resource data");
		return ByteVector(Data.begin() + begin, Data.begin() + begin + length);
	}

	ParsedValue ParseValue(size_t offset, unsigned depth)
	{
		if (depth > 256) throw std::runtime_error("PSB nesting is too deep");
		Require(offset, 1, "value tag");
		const tjs_uint8 tag = Data[offset++];
		ParsedValue result;

		if (tag == 0x00 || tag == 0x01) return result;
		if (tag == 0x02 || tag == 0x03)
		{
			result.Value = (tag == 0x03);
			return result;
		}
		if (tag >= 0x04 && tag <= 0x0c)
		{
			const size_t width = static_cast<size_t>(tag - 0x04);
			tjs_uint64 raw = ReadUInt(offset, width, "integer");
			if (width != 0 && width < 8 && (raw & (static_cast<tjs_uint64>(1) << (width * 8 - 1))))
				raw |= (~static_cast<tjs_uint64>(0)) << (width * 8);
			result.Value = static_cast<tjs_int64>(raw);
			return result;
		}
		if (tag >= 0x0d && tag <= 0x14)
		{
			const PsbArray values = ReadArray(offset - 1);
			iTJSDispatch2 *array = TJSCreateArrayObject();
			for (tjs_uint i = 0; i < values.Values.size(); ++i)
			{
				tTJSVariant value(static_cast<tjs_int64>(values.Values[i]));
				array->PropSetByNum(TJS_MEMBERENSURE, i, &value, array);
			}
			result.Value = tTJSVariant(array, array);
			array->Release();
			return result;
		}
		if (tag >= 0x15 && tag <= 0x18)
		{
			const tjs_uint32 index = static_cast<tjs_uint32>(ReadUInt(offset, tag - 0x14, "string index"));
			if (index >= Strings.size()) throw std::runtime_error("Invalid PSB string index");
			result.Value = ttstr(Strings[index]);
			return result;
		}
		if ((tag >= 0x19 && tag <= 0x1c) || (tag >= 0x22 && tag <= 0x25))
		{
			result.IsExtraResource = tag >= 0x22;
			const tjs_uint8 baseTag = result.IsExtraResource ? 0x21 : 0x18;
			result.ResourceIndex = static_cast<tjs_uint32>(ReadUInt(offset, tag - baseTag, "resource index"));
			const ByteVector bytes = ResourceBytes(result.ResourceIndex, result.IsExtraResource);
			result.Value = tTJSVariant(bytes.empty() ? nullptr : bytes.data(), static_cast<tjs_uint>(bytes.size()));
			result.IsResource = true;
			return result;
		}
		if (tag == 0x1d)
		{
			result.Value = static_cast<tjs_real>(0.0);
			return result;
		}
		if (tag == 0x1e)
		{
			const tjs_uint32 bits = Read32(offset, "float");
			float value;
			std::memcpy(&value, &bits, sizeof(value));
			result.Value = static_cast<tjs_real>(value);
			return result;
		}
		if (tag == 0x1f)
		{
			const tjs_uint64 bits = ReadUInt(offset, 8, "double");
			double value;
			std::memcpy(&value, &bits, sizeof(value));
			result.Value = static_cast<tjs_real>(value);
			return result;
		}
		if (tag == 0x20)
		{
			const PsbArray offsets = ReadArray(offset);
			const size_t base = offsets.End;
			iTJSDispatch2 *array = TJSCreateArrayObject();
			try
			{
				for (tjs_uint i = 0; i < offsets.Values.size(); ++i)
				{
					ParsedValue item = ParseValue(base + offsets.Values[i], depth + 1);
					array->PropSetByNum(TJS_MEMBERENSURE, i, &item.Value, array);
				}
				result.Value = tTJSVariant(array, array);
			}
			catch (...)
			{
				array->Release();
				throw;
			}
			array->Release();
			return result;
		}
		if (tag == 0x21)
		{
			const PsbArray nameIndexes = ReadArray(offset);
			const PsbArray offsets = ReadArray(nameIndexes.End);
			const size_t base = offsets.End;
			if (nameIndexes.Values.size() != offsets.Values.size())
				throw std::runtime_error("Mismatched PSB dictionary tables");

			iTJSDispatch2 *dictionary = TJSCreateDictionaryObject();
			try
			{
				for (size_t i = 0; i < nameIndexes.Values.size(); ++i)
				{
					const tjs_uint32 nameIndex = nameIndexes.Values[i];
					if (nameIndex >= Names.size()) throw std::runtime_error("Invalid PSB property name index");
					ParsedValue item = ParseValue(base + offsets.Values[i], depth + 1);
					const tjs_string &name = Names[nameIndex];
					dictionary->PropSet(TJS_MEMBERENSURE, name.c_str(), nullptr, &item.Value, dictionary);
					if (item.IsResource)
					{
						auto &resourceNames = item.IsExtraResource ? ExtraResourceNames : ResourceNames;
						if (resourceNames.find(item.ResourceIndex) == resourceNames.end())
							resourceNames[item.ResourceIndex] = name;
					}
				}
				result.Value = tTJSVariant(dictionary, dictionary);
			}
			catch (...)
			{
				dictionary->Release();
				throw;
			}
			dictionary->Release();
			return result;
		}

		throw std::runtime_error("Unknown PSB value tag");
	}

public:
	explicit PsbReader(ByteVector data) : Data(std::move(data)) {}

	ParsedValue Parse()
	{
		ParseHeader();
		LoadNames();
		LoadStrings();
		ChunkOffsets = ReadArray(Header.OffsetChunkOffsets).Values;
		ChunkLengths = ReadArray(Header.OffsetChunkLengths).Values;
		if (ChunkOffsets.size() != ChunkLengths.size())
			throw std::runtime_error("Mismatched PSB resource tables");
		if (Header.Version >= 4)
		{
			ExtraChunkOffsets = ReadArray(Header.OffsetExtraChunkOffsets).Values;
			ExtraChunkLengths = ReadArray(Header.OffsetExtraChunkLengths).Values;
			if (ExtraChunkOffsets.size() != ExtraChunkLengths.size())
				throw std::runtime_error("Mismatched PSB extra resource tables");
		}
		return ParseValue(Header.OffsetEntries, 0);
	}

	std::map<tjs_string, ByteVector> NamedResources() const
	{
		std::map<tjs_string, ByteVector> result;
		for (const auto &entry : ResourceNames)
			result[entry.second] = ResourceBytes(entry.first, false);
		for (const auto &entry : ExtraResourceNames)
			result[entry.second] = ResourceBytes(entry.first, true);
		return result;
	}

	tjs_uint16 Version() const { return Header.Version; }
	size_t ResourceCount() const { return ResourceNames.size() + ExtraResourceNames.size(); }
};

static ByteVector ReadAll(tTJSBinaryStream *stream)
{
	const tjs_uint64 size64 = stream->GetSize();
	if (size64 > 512u * 1024u * 1024u) throw std::runtime_error("PSB stream is too large");
	ByteVector data(static_cast<size_t>(size64));
	stream->SetPosition(0);
	if (!data.empty()) stream->ReadBuffer(data.data(), static_cast<tjs_uint>(data.size()));
	return data;
}

static ByteVector ExpandMdf(const ByteVector &input)
{
	// MDF files in the wild use both "MDF\0" and "mdf\0".  The classic
	// mdftool writes the lowercase signature, while newer implementations
	// commonly test for uppercase.  Treat the three letters case-insensitively
	// and keep the required NUL terminator strict.
	if (input.size() < 8 ||
		((input[0] | 0x20u) != 'm') ||
		((input[1] | 0x20u) != 'd') ||
		((input[2] | 0x20u) != 'f') ||
		input[3] != 0)
		return input;
	const tjs_uint32 outputSize = static_cast<tjs_uint32>(input[4]) |
		(static_cast<tjs_uint32>(input[5]) << 8) |
		(static_cast<tjs_uint32>(input[6]) << 16) |
		(static_cast<tjs_uint32>(input[7]) << 24);
	if (outputSize == 0 || outputSize > 512u * 1024u * 1024u)
		throw std::runtime_error("Invalid MDF output size");
	ByteVector output(outputSize);
	uLongf actual = outputSize;
	const int status = uncompress(output.data(), &actual, input.data() + 8,
		static_cast<uLong>(input.size() - 8));
	if (status != Z_OK) throw std::runtime_error("Unable to decompress MDF-wrapped PSB");
	output.resize(static_cast<size_t>(actual));
	return output;
}
} // namespace

tTJSNI_PSBFile::tTJSNI_PSBFile() : Root(nullptr) { EnsurePsbMedia(); }

tTJSNI_PSBFile::~tTJSNI_PSBFile() { Invalidate(); }

tjs_error TJS_INTF_METHOD tTJSNI_PSBFile::Construct(tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams > 0 && param[0]->Type() != tvtVoid)
	{
		if (param[0]->Type() != tvtString) return TJS_E_INVALIDPARAM;
		Load(ttstr(*param[0]));
	}
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_PSBFile::Invalidate()
{
	if (Root)
	{
		Root->Release();
		Root = nullptr;
	}
}

bool tTJSNI_PSBFile::Load(const ttstr &storage)
{
	try
	{
		const ttstr placed = TVPGetPlacedPath(storage);
		if (placed.IsEmpty()) throw std::runtime_error("PSB storage was not found");
		std::unique_ptr<tTJSBinaryStream> stream(TVPCreateStream(placed, TJS_BS_READ));
		PsbReader reader(ExpandMdf(ReadAll(stream.get())));
		ParsedValue root = reader.Parse();
		if (root.Value.Type() != tvtObject) throw std::runtime_error("PSB root is not a dictionary");

		iTJSDispatch2 *newRoot = root.Value.AsObject();
		// The script may use an extension-less auto-path lookup.  psb:// URLs,
		// however, are keyed by the placed container name (common.pimg, etc.).
		const ttstr container = TVPExtractStorageName(placed);
		PsbMedia->Commit(container, reader.NamedResources());
		Invalidate();
		Root = newRoot;
		KRKRNS_LOG("[psb] loaded %s as %s version=%u named-resources=%u",
			storage.AsNarrowStdString().c_str(), container.AsNarrowStdString().c_str(),
			static_cast<unsigned>(reader.Version()),
			static_cast<unsigned>(reader.ResourceCount()));
		return true;
	}
	catch (const std::exception &exception)
	{
		KRKRNS_LOG("[psb] load failed %s: %s", storage.AsStdString().c_str(), exception.what());
		return false;
	}
	catch (...)
	{
		KRKRNS_LOG("[psb] load failed %s: unknown exception", storage.AsStdString().c_str());
		return false;
	}
}

tjs_uint32 tTJSNC_PSBFile::ClassID = static_cast<tjs_uint32>(-1);

tTJSNC_PSBFile::tTJSNC_PSBFile() : inherited(TJS_W("PSBFile"))
{
	EnsurePsbMedia();
	TJS_BEGIN_NATIVE_MEMBERS(PSBFile)
	TJS_DECL_EMPTY_FINALIZE_METHOD

	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(_this, tTJSNI_PSBFile, PSBFile)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(PSBFile)

	TJS_BEGIN_NATIVE_METHOD_DECL(load)
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_PSBFile);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		if (param[0]->Type() != tvtString) return TJS_E_INVALIDPARAM;
		const bool loaded = _this->Load(ttstr(*param[0]));
		if (result) *result = loaded;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(load)

	TJS_BEGIN_NATIVE_PROP_DECL(root)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_PSBFile);
			if (_this->GetRoot()) *result = tTJSVariant(_this->GetRoot(), _this->GetRoot());
			else result->Clear();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(root)

	TJS_END_NATIVE_MEMBERS
}

tTJSNativeInstance *tTJSNC_PSBFile::CreateNativeInstance()
{
	return new tTJSNI_PSBFile();
}

tTJSNativeClass *TVPCreateNativeClass_PSBFile()
{
	return new tTJSNC_PSBFile();
}

// KRKR-ns: exported to SDLApplication.cpp's end-of-session cleanup.
void krkrsdl2_psb_clear_resources()
{
	if (PsbMedia) PsbMedia->Clear();
}

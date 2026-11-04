#include "tjsCommHead.h"
#include "PsbFilePlugin.h"

#include "CharacterSet.h"
#include "KrkrNSLog.h"
#include "KrkrNSSlowOperation.h"
#include "KrkrNSVMProf.h"
#include "KrkrNSPerf.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "SharedMemoryStream.h"
#include "tjsArray.h"
#include "proof_registry.h"
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
#include <typeinfo>
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
	std::map<tjs_string, KrkrSharedBytes> Resources;
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
		KRKRNS_LOG("[psb] lazy loading container");
		tTJSNI_PSBFile file;
		if (!file.Load(container)) return false;

		std::lock_guard<std::mutex> lock(Mutex);
		return Resources.find(key) != Resources.end();
	}

	tTJSBinaryStream *TJS_INTF_METHOD Open(const ttstr &name, tjs_uint32 flags) override
	{
		if ((flags & TJS_BS_ACCESS_MASK) != TJS_BS_READ)
			throw std::runtime_error("PSB media is read-only");

		KrkrSharedBytes bytes;
		{
			std::lock_guard<std::mutex> lock(Mutex);
			auto found = Resources.find(Normalized(name));
			if (found == Resources.end())
				throw std::runtime_error("PSB resource was not found");
			bytes = found->second;
		}

		return new tTVPSharedMemoryStream(std::move(bytes));
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

	void Commit(const ttstr &container, const std::map<tjs_string, KrkrSharedBytes> &resources)
	{
		ttstr normalizedContainer(container);
		normalizedContainer.ToLowerCase();
		const tjs_string prefix = normalizedContainer.AsStdString() + TJS_W("/");

		std::lock_guard<std::mutex> lock(Mutex);
		Containers.insert(normalizedContainer.AsStdString());
		// A reloaded container replaces its complete namespace. Existing streams
		// keep the previous immutable bytes until their last reader closes.
		auto old = Resources.lower_bound(prefix);
		while (old != Resources.end() && old->first.compare(0, prefix.size(), prefix) == 0)
			old = Resources.erase(old);
		for (const auto &entry : resources)
		{
			ttstr normalizedName(entry.first);
			normalizedName.ToLowerCase();
			Resources[prefix + normalizedName.AsStdString()] = entry.second;
		}
		KRKRNS_LOG("[psb] registered container resources=%u",
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


class PsbReader;
static iTJSDispatch2 *UnsafeCreateLazyPsbValue(std::shared_ptr<PsbReader>, size_t, unsigned, bool);

class PsbReader : public std::enable_shared_from_this<PsbReader>
{
public:
	// Parse cost is dominated by how many values are built into TJS objects, so
	// track that alongside time. A scene script taking seconds to parse is only
	// explicable by volume or by per-element overhead; these say which.
	const bool Profiling = krkrsdl2_perf_enabled();
	std::uint64_t ProbeCounter() const noexcept
	{
		// Large scenarios contain hundreds of thousands of nodes. Timer reads
		// are diagnostic work and must not run in normal game sessions.
		return Profiling ? static_cast<std::uint64_t>(SDL_GetPerformanceCounter()) : 0;
	}
	tjs_uint64 StatValues = 0, StatArrays = 0, StatArrayElements = 0;
	tjs_uint64 StatDicts = 0, StatDictMembers = 0;
	// Split inside Build: creating the TJS container, reserving its hash, and
	// inserting members have different remedies, and a script parse of seconds
	// can only be attributed after they are counted separately.
	tjs_uint64 TicksCreateDict = 0, TicksRebuildHash = 0, TicksSetMember = 0;
	tjs_uint64 TicksCreateArray = 0, TicksSetElement = 0;
	// Reading the per-node tables is billed separately from building TJS
	// objects: for a scene script most of the tree is plain strings and ints,
	// and only these counters can say whether the cost is in the tables or in
	// the variant assignments.
	tjs_uint64 TicksReadArray = 0, TicksStrings = 0, TicksScalars = 0;
	tjs_uint64 StatStrings = 0, StatScalars = 0;
	// Child time is recursively inclusive and non-additive. It must never be
	// subtracted from total parse time or mistaken for main-loop dispatch cost.
	tjs_uint64 TicksEntryTables = 0, TicksDispatch = 0;
private:
	bool UnsafeEnabled = false;
	bool UnsafeForceSynthetic = false;
	iTJSDispatch2 *UnsafePending = nullptr;
	size_t UnsafePendingOffset = 0;
	iTJSDispatch2 *UnsafeTakeTarget(size_t offset) {
		if (!UnsafePending || UnsafePendingOffset != offset) return nullptr;
		iTJSDispatch2 *result = UnsafePending;
		UnsafePending = nullptr;
		result->AddRef();
		return result;
	}
	std::shared_ptr<const ByteVector> Storage;
	const ByteVector &Data;
	PsbHeader Header;
	std::vector<ttstr> Names;
	std::vector<ttstr> Strings;
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

	PsbArray ReadEntryArray(size_t offset) const { return ReadArray(offset); }

	// Non-allocating reader for the validation walk. Validation only needs to
	// visit every entry once, and the scenario scenarios hold tens of thousands
	// of arrays: filling a std::vector per node made the walk cost ~70 ms per
	// load. The tags, widths and bounds are checked exactly as in ReadArray.
	struct PsbArrayView
	{
		size_t Entries = 0;
		tjs_uint64 Count = 0;
		size_t Width = 0;
		size_t End = 0;
	};

	PsbArrayView ReadArrayView(size_t offset) const
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

		PsbArrayView result;
		result.Entries = offset;
		result.Count = count64;
		result.Width = static_cast<size_t>(entryWidth);
		const tjs_uint64 span = count64 * static_cast<tjs_uint64>(entryWidth);
		const tjs_uint64 end64 = static_cast<tjs_uint64>(offset) + span;
		if (end64 > Data.size()) throw std::runtime_error("PSB array entries exceed the file");
		result.End = static_cast<size_t>(end64);
		return result;
	}

	tjs_uint32 ReadArrayViewElement(const PsbArrayView &view, tjs_uint64 index) const
	{
		size_t offset = view.Entries + static_cast<size_t>(index) * view.Width;
		const tjs_uint64 value = ReadUInt(offset, view.Width, "array entry");
		if (value > 0xffffffffu) throw std::runtime_error("PSB array entry exceeds 32 bits");
		return static_cast<tjs_uint32>(value);
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
				Names.emplace_back(ReadZeroString(static_cast<size_t>(Header.OffsetNames) + index));
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
			Names.emplace_back(Utf8(bytes.data(), bytes.size()));
		}
	}

	void LoadStrings()
	{
		const PsbArray offsets = ReadArray(Header.OffsetStrings);
		Strings.reserve(offsets.Values.size());
		for (tjs_uint32 offset : offsets.Values)
			Strings.emplace_back(ReadZeroString(static_cast<size_t>(Header.OffsetStringsData) + offset));
	}

	KrkrSharedBytes ResourceBytes(tjs_uint32 index, bool extra) const
	{
		const auto &offsets = extra ? ExtraChunkOffsets : ChunkOffsets;
		const auto &lengths = extra ? ExtraChunkLengths : ChunkLengths;
		const tjs_uint32 base = extra ? Header.OffsetExtraChunkData : Header.OffsetChunkData;
		if (index >= offsets.size() || index >= lengths.size())
			throw std::runtime_error("Invalid PSB resource index");
		const size_t begin = static_cast<size_t>(base) + offsets[index];
		const size_t length = lengths[index];
		Require(begin, length, "resource data");
		return {Storage, Data.data() + begin, length};
	}

	ParsedValue ParseValue(size_t offset, unsigned depth)
	{
		if (depth > 256) throw std::runtime_error("PSB nesting is too deep");
		Require(offset, 1, "value tag");
		const tjs_uint8 tag = Data[offset++];
		++StatValues;
		ParsedValue result;

		if (tag == 0x00 || tag == 0x01) return result;
		if (tag == 0x02 || tag == 0x03)
		{
			result.Value = (tag == 0x03);
			return result;
		}
		if (tag >= 0x04 && tag <= 0x0c)
		{
			const std::uint64_t tScalar = ProbeCounter();
			const size_t width = static_cast<size_t>(tag - 0x04);
			tjs_uint64 raw = ReadUInt(offset, width, "integer");
			if (width != 0 && width < 8 && (raw & (static_cast<tjs_uint64>(1) << (width * 8 - 1))))
				raw |= (~static_cast<tjs_uint64>(0)) << (width * 8);
			result.Value = static_cast<tjs_int64>(raw);
			TicksScalars += ProbeCounter() - tScalar;
			++StatScalars;
			return result;
		}
		if (tag >= 0x0d && tag <= 0x14)
		{
			const auto values = ReadEntryArray(offset - 1);
			++StatArrays;
			StatArrayElements += values.Values.size();
			const std::uint64_t tArrCreate = ProbeCounter();
			iTJSDispatch2 *array = UnsafeTakeTarget(offset - 1);
			if (!array && UnsafeEnabled) {
				array = UnsafeCreateLazyPsbValue(shared_from_this(), offset - 1, depth, true);
				result.Value = tTJSVariant(array, array);
				array->Release();
				return result;
			}
			if (!array) array = TJSCreateArrayObject();
			TicksCreateArray += ProbeCounter() - tArrCreate;
			try
			{
				const std::uint64_t tElem = ProbeCounter();
				for (tjs_uint i = 0; i < values.Values.size(); ++i)
				{
					tTJSVariant value(static_cast<tjs_int64>(values.Values[i]));
					array->PropSetByNum(TJS_MEMBERENSURE, i, &value, array);
					::krkr_exact_proof::TransactionPoint(krkr_lazy_transaction_test::Point::AfterInsert,array,i+1);
				}
				TicksSetElement += ProbeCounter() - tElem;
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
		if (tag >= 0x15 && tag <= 0x18)
		{
			const std::uint64_t tStr = ProbeCounter();
			const tjs_uint32 index = static_cast<tjs_uint32>(ReadUInt(offset, tag - 0x14, "string index"));
			if (index >= Strings.size()) throw std::runtime_error("Invalid PSB string index");
			// PSB strings are interned by index. TJS strings use copy-on-write,
			// so repeated references can share storage without aliasing edits.
			result.Value = Strings[index];
			TicksStrings += ProbeCounter() - tStr;
			++StatStrings;
			return result;
		}
		if ((tag >= 0x19 && tag <= 0x1c) || (tag >= 0x22 && tag <= 0x25))
		{
			result.IsExtraResource = tag >= 0x22;
			const tjs_uint8 baseTag = result.IsExtraResource ? 0x21 : 0x18;
			result.ResourceIndex = static_cast<tjs_uint32>(ReadUInt(offset, tag - baseTag, "resource index"));
			const auto bytes = ResourceBytes(result.ResourceIndex, result.IsExtraResource);
			// TJS owns its octet value; copy once directly from the PSB, without
			// constructing another full-size temporary resource vector.
			result.Value = tTJSVariant(bytes.size ? bytes.data : nullptr, static_cast<tjs_uint>(bytes.size));
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
			const std::uint64_t tTables = ProbeCounter();
			const auto offsets = ReadEntryArray(offset);
			TicksEntryTables += ProbeCounter() - tTables;
			const size_t base = offsets.End;
			++StatArrays;
			StatArrayElements += offsets.Values.size();
			const std::uint64_t tCreate = ProbeCounter();
			iTJSDispatch2 *array = UnsafeTakeTarget(offset - 1);
			if (!array && UnsafeEnabled) {
				array = UnsafeCreateLazyPsbValue(shared_from_this(), offset - 1, depth, true);
				result.Value = tTJSVariant(array, array);
				array->Release();
				return result;
			}
			if (!array) array = TJSCreateArrayObject();
			TicksCreateArray += ProbeCounter() - tCreate;
			try
			{
				for (tjs_uint i = 0; i < offsets.Values.size(); ++i)
				{
					// Keep recursive parsing outside the setter timer. Inclusive
					// child time must not masquerade as array insertion cost.
					const std::uint64_t tDispatch = ProbeCounter();
					ParsedValue item = ParseValue(base + offsets.Values[i], depth + 1);
					TicksDispatch += ProbeCounter() - tDispatch;
					const std::uint64_t tSet = ProbeCounter();
					array->PropSetByNum(TJS_MEMBERENSURE, i, &item.Value, array);
					::krkr_exact_proof::TransactionPoint(krkr_lazy_transaction_test::Point::AfterInsert,array,i+1);
					TicksSetElement += ProbeCounter() - tSet;
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
			const std::uint64_t tTables = ProbeCounter();
			const auto nameIndexes = ReadEntryArray(offset);
			const auto offsets = ReadEntryArray(nameIndexes.End);
			TicksEntryTables += ProbeCounter() - tTables;
			const size_t base = offsets.End;
			if (nameIndexes.Values.size() != offsets.Values.size())
				throw std::runtime_error("Mismatched PSB dictionary tables");
			++StatDicts;
			StatDictMembers += nameIndexes.Values.size();

			const std::uint64_t tCreate = ProbeCounter();
			iTJSDispatch2 *dictionary = UnsafeTakeTarget(offset - 1);
			if (!dictionary && UnsafeEnabled) {
				dictionary = UnsafeCreateLazyPsbValue(shared_from_this(), offset - 1, depth, false);
				result.Value = tTJSVariant(dictionary, dictionary);
				dictionary->Release();
				return result;
			}
			if (!dictionary) dictionary = TJSCreateDictionaryObject();
			TicksCreateDict += ProbeCounter() - tCreate;
			try
			{
				// Match Dictionary.assign's capacity reservation. Large UI tables
				// otherwise put thousands of keys into the default small hash.
				if (nameIndexes.Values.size() >= 8)
				{
					const std::uint64_t tHash = ProbeCounter();
					static_cast<tTJSDictionaryObject *>(dictionary)->RebuildHash(
						static_cast<tjs_int>(nameIndexes.Values.size()));
					TicksRebuildHash += ProbeCounter() - tHash;
				}
				for (size_t i = 0; i < nameIndexes.Values.size(); ++i)
				{
					const tjs_uint32 nameIndex = nameIndexes.Values[i];
					if (nameIndex >= Names.size()) throw std::runtime_error("Invalid PSB property name index");
					const std::uint64_t tDispatch = ProbeCounter();
					ParsedValue item = ParseValue(base + offsets.Values[i], depth + 1);
					TicksDispatch += ProbeCounter() - tDispatch;
					const ttstr &name = Names[nameIndex];
					// Repeated PSB keys share their TJS string and cached hash, as
					// native member copying already does. Dictionary symbols hold
					// their own references after this reader is destroyed. Preserve
					// the empty-name path: an empty ttstr has no variant string.
					const std::uint64_t tSet = ProbeCounter();
					if (auto *key = name.AsVariantStringNoAddRef())
						dictionary->PropSetByVS(TJS_MEMBERENSURE, key, &item.Value, dictionary);
					else
						dictionary->PropSet(TJS_MEMBERENSURE, name.c_str(), nullptr, &item.Value, dictionary);
					TicksSetMember += ProbeCounter() - tSet;
					::krkr_exact_proof::TransactionPoint(krkr_lazy_transaction_test::Point::AfterInsert,dictionary,i+1);
					if (item.IsResource)
					{
						auto &resourceNames = item.IsExtraResource ? ExtraResourceNames : ResourceNames;
						if (resourceNames.find(item.ResourceIndex) == resourceNames.end())
							resourceNames[item.ResourceIndex] = name.AsStdString();
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


    struct ValidatedResource { bool resource = false; bool extra = false; tjs_uint32 index = 0; };
    ValidatedResource UnsafeValidateValue(size_t offset, unsigned depth) {
        if (depth > 256) throw std::runtime_error("PSB nesting is too deep");
        Require(offset, 1, "value tag");
        const tjs_uint8 tag = Data[offset++];
        ValidatedResource result;
        if (tag <= 3 || tag == 0x1d) return result;
        if (tag >= 0x04 && tag <= 0x0c) {
            ReadUInt(offset, tag - 0x04, "integer"); return result;
        }
        if (tag >= 0x0d && tag <= 0x14) {
            ReadEntryArray(offset - 1); return result;
        }
        if (tag >= 0x15 && tag <= 0x18) {
            const tjs_uint32 index = static_cast<tjs_uint32>(ReadUInt(offset, tag - 0x14, "string index"));
            if (index >= Strings.size()) throw std::runtime_error("Invalid PSB string index");
            return result;
        }
        if ((tag >= 0x19 && tag <= 0x1c) || (tag >= 0x22 && tag <= 0x25)) {
            result.extra = tag >= 0x22;
            result.index = static_cast<tjs_uint32>(ReadUInt(offset, tag - (result.extra ? 0x21 : 0x18), "resource index"));
            ResourceBytes(result.index, result.extra);
            result.resource = true; return result;
        }
        if (tag == 0x1e) { Read32(offset, "float"); return result; }
        if (tag == 0x1f) { ReadUInt(offset, 8, "double"); return result; }
        if (tag == 0x20) {
            // Same checks as ReadEntryArray, but without materializing the
            // element vector; see ReadArrayView.
            const auto offsets = ReadArrayView(offset);
            for (tjs_uint64 i = 0; i < offsets.Count; ++i)
                UnsafeValidateValue(offsets.End + ReadArrayViewElement(offsets, i), depth + 1);
            return result;
        }
        if (tag == 0x21) {
            const auto names = ReadArrayView(offset);
            const auto offsets = ReadArrayView(names.End);
            if (names.Count != offsets.Count)
                throw std::runtime_error("Mismatched PSB dictionary tables");
            for (tjs_uint64 i = 0; i < names.Count; ++i) {
                const auto nameIndex = ReadArrayViewElement(names, i);
                if (nameIndex >= Names.size()) throw std::runtime_error("Invalid PSB property name index");
                const auto child = UnsafeValidateValue(offsets.End + ReadArrayViewElement(offsets, i), depth + 1);
                if (child.resource) {
                    auto &resourceNames = child.extra ? ExtraResourceNames : ResourceNames;
                    if (resourceNames.find(child.index) == resourceNames.end())
                        resourceNames[child.index] = Names[nameIndex].AsStdString();
                }
            }
            return result;
        }
        throw std::runtime_error("Unknown PSB value tag");
    }

public:
	explicit PsbReader(ByteVector data, bool forceSynthetic = false)
		: UnsafeForceSynthetic(forceSynthetic),
		  Storage(std::make_shared<const ByteVector>(std::move(data))), Data(*Storage) {}

	void UnsafePopulateInto(iTJSDispatch2 *target, size_t offset, unsigned depth) {
		if (UnsafePending) throw std::runtime_error("Lazy materialization reentered");
		UnsafePending = target; UnsafePendingOffset = offset;
		try {
			ParsedValue ignored = ParseValue(offset, depth);
			if (UnsafePending) throw std::runtime_error("Lazy target was not consumed");
		} catch (...) { UnsafePending = nullptr; throw; }
	}

	// Isolated proof only. Caller must keep the receiver alive for the *whole*
	// dispatch, not merely for this function. This remains an open limitation.
	void UnsafeMaterialize(iTJSDispatch2 *target, size_t offset, unsigned depth) {
		using namespace krkr_exact_proof;
		using krkr_lazy_transaction_test::Point;
		const bool isArray=typeid(*target)==typeid(tTJSArrayObject);
		const bool isDictionary=typeid(*target)==typeid(tTJSDictionaryObject);
		if (!isArray && !isDictionary) throw std::runtime_error("unexpected transaction target");
		auto *dst=static_cast<tTJSCustomObject *>(target);
		if (isDictionary && !dst->UnsafeTxnEmptyStorage())
			throw std::runtime_error("nonempty dictionary cannot use transaction");
		TransactionScope scope(target,nullptr);
		TransactionPoint(Point::BeforeStage,nullptr,0);
		iTJSDispatch2 *stage=isArray ? TJSCreateArrayObject() : TJSCreateDictionaryObject();
		if (!stage) throw std::runtime_error("stage creation failed");
		scope.stage=stage;
		tTJSCustomObject::UnsafeTxnTable *dictTable=nullptr;
		std::vector<tTJSVariant> *arrayItems=nullptr;
		bool detached=false;
		try {
			if ((isArray && typeid(*stage)!=typeid(tTJSArrayObject)) ||
			    (isDictionary && typeid(*stage)!=typeid(tTJSDictionaryObject)))
				throw std::runtime_error("stage factory changed");
			auto *st=static_cast<tTJSCustomObject *>(stage);
			if (isDictionary && (!st->UnsafeTxnEmptyStorage() ||
			    dst->UnsafeTxnHashSize()!=st->UnsafeTxnHashSize() ||
			    dst->UnsafeTxnHashMask()!=st->UnsafeTxnHashMask()))
				throw std::runtime_error("stage Dictionary is not an empty matching target");
			if (isArray) {
				auto *a=static_cast<tTJSArrayNI *>(dst->UnsafeTxnNative(TJSGetArrayClassID()));
				auto *b=static_cast<tTJSArrayNI *>(st->UnsafeTxnNative(TJSGetArrayClassID()));
				if (!a || !b || !a->Items.empty() || !b->Items.empty())
					throw std::runtime_error("stage Array is not an empty matching target");
			}
			UnsafePopulateInto(stage,offset,depth);
			TransactionPoint(Point::BeforeCommit,stage,0);
			if (isArray) {
				auto *a=static_cast<tTJSArrayNI *>(dst->UnsafeTxnNative(TJSGetArrayClassID()));
				auto *b=static_cast<tTJSArrayNI *>(st->UnsafeTxnNative(TJSGetArrayClassID()));
				if (!a || !b || !a->Items.empty()) throw std::runtime_error("invalid Array stage");
				for (const auto &v : b->Items) if (v.Type()==tvtObject) {
					const auto c=v.AsObjectClosureNoAddRef();
					if (c.Object==stage || c.ObjThis==stage || c.Object==target || c.ObjThis==target)
						throw std::runtime_error("self-bound Array stage");
				}
				arrayItems=new std::vector<tTJSVariant>();
				arrayItems->swap(b->Items); // default allocator; no callbacks
				detached=true;
			} else {
				dictTable=new tTJSCustomObject::UnsafeTxnTable();
				if (!st->UnsafeTxnDetachDictionary(*dictTable,*dst))
					throw std::runtime_error("unsafe Dictionary stage");
				detached=true;
			}
			TransactionPoint(Point::BeforeStageRelease,stage,0);
			// After this point a failing Release may have destroyed or partially
			// destroyed stage. Never touch stage again on any exception.
			if (stage->Release()!=0) throw std::runtime_error("stage escaped before commit");
			stage=nullptr;
			if (isArray) {
				auto *a=static_cast<tTJSArrayNI *>(dst->UnsafeTxnNative(TJSGetArrayClassID()));
				if (!a || !a->Items.empty()) throw std::runtime_error("target Array changed before commit");
				a->Items.swap(*arrayItems);
				delete arrayItems; arrayItems=nullptr; // now empty, no value releases
			} else {
				if (!dst->UnsafeTxnCommitDictionary(*dictTable))
					throw std::runtime_error("target Dictionary changed before commit");
				delete dictTable; dictTable=nullptr; // now contains no table
			}
		} catch (...) {
			auto initial=std::current_exception();
			if (detached) {
				// The detached holder has no automatic value destruction; after a
				// stage teardown error we deliberately quarantine it forever.
				Poison(target,initial);
			} else {
				try {
					TransactionPoint(Point::CleanupStep,stage,0);
					if (isArray) {
						auto *ni=static_cast<tTJSArrayNI *>(static_cast<tTJSCustomObject *>(stage)->UnsafeTxnNative(TJSGetArrayClassID()));
						if (!ni) throw std::runtime_error("stage Array lost native instance");
						static_cast<tTJSArrayObject *>(stage)->Clear(ni);
					} else static_cast<tTJSCustomObject *>(stage)->Clear();
					stage->Release();
					delete dictTable; delete arrayItems; // both empty when not detached
				} catch (...) {
					// Never retry an entry whose private cleanup is uncertain.
					Poison(target,std::current_exception());
				}
			}
			std::rethrow_exception(initial);
		}
	}

	ParsedValue Parse()
	{
		KrkrNSPerfScope perfTables(KrkrNSPerfOp::PsbTables);
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
		// The local names.psb override exists only to exercise the isolated
		// synthetic fixture. Its resource publication is intentionally incomplete.
		UnsafeEnabled = (UnsafeForceSynthetic || (ChunkOffsets.empty() && ExtraChunkOffsets.empty())) &&
			::krkr_exact_proof::FactoriesPristine();
		KRKRNS_LOG("[lazy-value-proof] enabled=%d chunks=%llu extra=%llu forced-synthetic=%d",
			UnsafeEnabled ? 1 : 0, static_cast<unsigned long long>(ChunkOffsets.size()),
			static_cast<unsigned long long>(ExtraChunkOffsets.size()), UnsafeForceSynthetic ? 1 : 0);
		perfTables.Finish();
		KrkrNSPerfScope perfBuild(KrkrNSPerfOp::PsbBuild);
		if (UnsafeEnabled) UnsafeValidateValue(Header.OffsetEntries, 0);
		return ParseValue(Header.OffsetEntries, 0);
	}

	std::map<tjs_string, KrkrSharedBytes> NamedResources() const
	{
		std::map<tjs_string, KrkrSharedBytes> result;
		for (const auto &entry : ResourceNames)
			result[entry.second] = ResourceBytes(entry.first, false);
		for (const auto &entry : ExtraResourceNames)
			result[entry.second] = ResourceBytes(entry.first, true);
		return result;
	}

	tjs_uint16 Version() const { return Header.Version; }
	size_t ResourceCount() const { return ResourceNames.size() + ExtraResourceNames.size(); }
};

static iTJSDispatch2 *UnsafeCreateLazyPsbValue(std::shared_ptr<PsbReader> reader,
        size_t offset, unsigned depth, bool isArray) {
    iTJSDispatch2 *object = isArray ? TJSCreateArrayObject() : TJSCreateDictionaryObject();
    if (!object) throw std::runtime_error("Exact lazy factory returned null");
    try {
        bool registered = false;
        const bool eligible = isArray || static_cast<tTJSCustomObject *>(object)->UnsafeTxnCount()==0;
        try {
            if (eligible) registered = ::krkr_exact_proof::Register(object, [reader, object, offset, depth] {
                reader->UnsafeMaterialize(object, offset, depth);
            });
        } catch (const std::bad_alloc &) { registered = false; }
        if (!registered) reader->UnsafePopulateInto(object, offset, depth);
        return object;
    } catch (...) { object->Release(); throw; }
}


static ByteVector ReadAll(tTJSBinaryStream *stream)
{
	const tjs_uint64 size64 = stream->GetSize();
	if (size64 > 512u * 1024u * 1024u) throw std::runtime_error("PSB stream is too large");
	ByteVector data(static_cast<size_t>(size64));
	stream->SetPosition(0);
	if (!data.empty()) stream->ReadBuffer(data.data(), static_cast<tjs_uint>(data.size()));
	return data;
}

static ByteVector ExpandMdf(ByteVector input)
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
	// A malformed but historically accepted MDF may declare far more output
	// than it produces. Do not retain that excess capacity in shared resources.
	if (actual < outputSize) return ByteVector(output.begin(), output.end());
	return output;
}
} // namespace

#include "proof_registry.inc"

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
		KrkrNSPerfScope perfRelease(KrkrNSPerfOp::PsbRelease);
		KrkrNSVMProfNativeScope profile("psb-root-release");
		Root->Release();
		Root = nullptr;
	}
}

bool tTJSNI_PSBFile::Load(const ttstr &storage)
{
	KrkrNSSlowOperation slow("psb-load", &storage);
	try
	{
		// Preserve phase logs while optional scopes separate nested I/O and
		// table/object work from the enclosing load and parse measurements.
		const ttstr placed = TVPGetPlacedPath(storage);
		if (placed.IsEmpty()) throw std::runtime_error("PSB storage was not found");
		const Uint32 t1 = SDL_GetTicks();
		KrkrNSPerfScope perfRead(KrkrNSPerfOp::PsbRead);
		std::unique_ptr<tTJSBinaryStream> stream(TVPCreateStream(placed, TJS_BS_READ));
		std::vector<tjs_uint8> raw = ReadAll(stream.get());
		perfRead.Bytes(raw.size());
		perfRead.Finish();
		const Uint32 t2 = SDL_GetTicks();
		KrkrNSPerfScope perfExpand(KrkrNSPerfOp::PsbExpand);
		std::vector<tjs_uint8> expanded = ExpandMdf(std::move(raw));
		perfExpand.Finish();
		const Uint32 t3 = SDL_GetTicks();
		const size_t expandedBytes = expanded.size();
		KrkrNSPerfScope perfParse(KrkrNSPerfOp::PsbParse, expandedBytes);
		// The reader and committed resource streams share ownership of these
		// bytes, so neither the input nor the expanded buffer needs a copy.
		const bool unsafeSynthetic = storage == TJS_W("names.psb");
		auto readerOwner = std::make_shared<PsbReader>(std::move(expanded), unsafeSynthetic);
		PsbReader &reader = *readerOwner;
		ParsedValue root = reader.Parse();
		perfParse.Finish();
		if (root.Value.Type() != tvtObject) throw std::runtime_error("PSB root is not a dictionary");
		const Uint32 t4 = SDL_GetTicks();

		iTJSDispatch2 *newRoot = root.Value.AsObjectNoAddRef();
		// The script may use an extension-less auto-path lookup.  psb:// URLs,
		// however, are keyed by the placed container name (common.pimg, etc.).
		const ttstr container = TVPExtractStorageName(placed);
		KrkrNSPerfScope perfCommit(KrkrNSPerfOp::PsbCommit);
		PsbMedia->Commit(container, reader.NamedResources());
		perfCommit.Finish();
		const Uint32 t5 = SDL_GetTicks();
		// No file name here on purpose: AsNarrowStdString() THROWS on a path the
		// active code page cannot represent, and game directories are routinely
		// CJK ("【KRKR】LimeLight_lj").  Naming the file in a diagnostic line cost
		// a title its boot once; the numbers are what matter.
		KRKRNS_LOG("[psb] phases read=%ums mdf=%ums parse=%ums commit=%ums",
			(unsigned)(t2 - t1), (unsigned)(t3 - t2),
			(unsigned)(t4 - t3), (unsigned)(t5 - t4));
		if (reader.Profiling)
		{
			KRKRNS_LOG("[psb] parsed values=%llu arrays=%llu array-elems=%llu dicts=%llu dict-members=%llu bytes=%llu",
				static_cast<unsigned long long>(reader.StatValues),
				static_cast<unsigned long long>(reader.StatArrays),
				static_cast<unsigned long long>(reader.StatArrayElements),
				static_cast<unsigned long long>(reader.StatDicts),
				static_cast<unsigned long long>(reader.StatDictMembers),
				static_cast<unsigned long long>(expandedBytes));
			// Attribution inside Parse: container creation, hash reservation and
			// member insertion are the only work besides reading tags.
			const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
			KRKRNS_LOG("[psb] build create-dict=%.1fms rebuild-hash=%.1fms set-member=%.1fms "
				"create-array=%.1fms set-element=%.1fms",
				reader.TicksCreateDict * 1000.0 / freq,
				reader.TicksRebuildHash * 1000.0 / freq,
				reader.TicksSetMember * 1000.0 / freq,
				reader.TicksCreateArray * 1000.0 / freq,
				reader.TicksSetElement * 1000.0 / freq);
			KRKRNS_LOG("[psb] leaves scalars=%llu (%.1fms) strings=%llu (%.1fms)",
				static_cast<unsigned long long>(reader.StatScalars),
				reader.TicksScalars * 1000.0 / freq,
				static_cast<unsigned long long>(reader.StatStrings),
				reader.TicksStrings * 1000.0 / freq);
			KRKRNS_LOG("[psb] tree entry-tables=%.1fms child-inclusive=%.1fms",
				reader.TicksEntryTables * 1000.0 / freq,
				reader.TicksDispatch * 1000.0 / freq);
#ifdef __SWITCH__
			const auto pool = krkrns::SymbolAllocator<
				sizeof(tTJSCustomObject::tTJSSymbolData),
				alignof(tTJSCustomObject::tTJSSymbolData)>::GetRuntimeStatistics();
			KRKRNS_LOG("[psb-pool] pages=%llu peak-pages=%llu standalone-live=%llu standalone-total=%llu",
				static_cast<unsigned long long>(pool.pages),
				static_cast<unsigned long long>(pool.peakPages),
				static_cast<unsigned long long>(pool.standaloneLive),
				static_cast<unsigned long long>(pool.standaloneTotal));
			const auto arrays = krkrns::SymbolArrayAllocator::GetRuntimeStatistics();
			// Independent pool high-water marks are summed; this is not a
			// simultaneous memory peak. Large/global arrays are not included.
			KRKRNS_LOG("[psb-array-pool] pages=%llu peak-pages-sum=%llu standalone-live=%llu standalone-total=%llu",
				static_cast<unsigned long long>(arrays.pages),
				static_cast<unsigned long long>(arrays.peakPagesSum),
				static_cast<unsigned long long>(arrays.standaloneLive),
				static_cast<unsigned long long>(arrays.standaloneTotal));
#endif
		}
		newRoot->AddRef();
		Invalidate();
		Root = newRoot;
		// Same reason as above: no names.  This line used to convert both paths
		// with AsNarrowStdString() on the SUCCESS path, so a PSB file under a CJK
		// game directory threw after its resources were already committed -- the
		// load reported failure while half its work had been done.
		KRKRNS_LOG("[psb] loaded version=%u named-resources=%u",
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

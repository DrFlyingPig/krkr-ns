// KRKR-ns: Kirikiroid2-compatible XP3 extraction filter.
//
// Titles with scrambled archive content (Yuzusoft's Riddle Joker and friends)
// ship an `xp3filter.tjs` beside their archives.  Kirikiroid2 reads that file
// and executes it in a DEDICATED per-thread tTJS instance whose Storages class
// carries setXP3ArchiveExtractionFilter/setXP3ArchiveContentFilter; the game
// script registers a TJS callback there and every extracted archive chunk is
// passed through it (h=adler hash, o=uncompressed offset, b=byte accessor,
// l=length).  The callback must NOT run in the main script engine: archive
// reads happen on worker threads while the main engine is mid-execution, so a
// private engine per thread is the only safe shape.
//
// Ported from Kirikiroid2 src/plugins/xp3filter.cpp (CBinaryAccessor and the
// decoder-engine plumbing), adapted to this tree's 4-field
// tTVPXP3ExtractionFilterInfo (no FileName) and no content-filter context.

#include <mutex>
#include <map>
#include <thread>

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "tjs.h"
#include "XP3Archive.h"
#include "StorageIntf.h"
#include "SystemIntf.h"
#include "DebugIntf.h"

#include "KrkrNSLog.h"

//---------------------------------------------------------------------------
// CBinaryAccessor -- TJS view of the raw extraction buffer.
// Supports b[i] read/write with compound assignment operators, a `ptr`
// cursor property, `count`, and xor/add(start, length, value) calls.
//---------------------------------------------------------------------------
class CBinaryAccessor : public tTJSDispatch
{
	unsigned int m_length;
	unsigned int m_curPos;
	unsigned char *m_buff;

public:
	CBinaryAccessor(unsigned char* buff, unsigned int len) :
		m_length(len), m_curPos(0), m_buff(buff) {;}

	tjs_error TJS_INTF_METHOD OperationByNum(tjs_uint32 flag, tjs_int num,
		tTJSVariant *result, const tTJSVariant *param, iTJSDispatch2 *objthis) override
	{
		num += m_curPos;
		if (num < 0 || num >= (tjs_int)m_length) return TJS_E_MEMBERNOTFOUND;
		unsigned char opnum = param->AsInteger();
		switch (flag & TJS_OP_MASK) {
		case TJS_OP_BAND: m_buff[num] &= opnum; break;
		case TJS_OP_BOR:  m_buff[num] |= opnum; break;
		case TJS_OP_BXOR: m_buff[num] ^= opnum; break;
		case TJS_OP_SUB:  m_buff[num] -= opnum; break;
		case TJS_OP_ADD:  m_buff[num] += opnum; break;
		case TJS_OP_MOD:  m_buff[num] %= opnum; break;
		case TJS_OP_DIV:  m_buff[num] /= (signed char)opnum; break;
		case TJS_OP_IDIV: m_buff[num] /= opnum; break;
		case TJS_OP_MUL:  m_buff[num] *= opnum; break;
		case TJS_OP_LOR:  m_buff[num] = m_buff[num] || opnum; break;
		case TJS_OP_LAND: m_buff[num] = m_buff[num] && opnum; break;
		case TJS_OP_SAR:  m_buff[num] >>= opnum; break;
		case TJS_OP_SAL:  m_buff[num] <<= opnum; break;
		case TJS_OP_SR:   m_buff[num] >>= opnum; break;
		case TJS_OP_INC:  m_buff[num] ++; break;
		case TJS_OP_DEC:  m_buff[num] --; break;
		default:
			return TJS_E_NOTIMPL;
		}
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD Operation(tjs_uint32 flag, const tjs_char *membername,
		tjs_uint32 *hint, tTJSVariant *result, const tTJSVariant *param,
		iTJSDispatch2 *objthis) override
	{
		if (membername) {
			static const ttstr str_ptr(TJS_W("ptr"));
			if (hint) {
				static const tjs_uint32 hash_ptr = tTJSHashFunc<tjs_char *>::Make(str_ptr.c_str());
				if (!*hint)
					*hint = tTJSHashFunc<tjs_char *>::Make(membername);
				if (*hint != hash_ptr)
					return TJS_E_NOTIMPL;
			} else if (str_ptr != membername) {
				return TJS_E_NOTIMPL;
			}
		}
		tjs_uint32 op = flag & TJS_OP_MASK;
		switch (op) {
		case TJS_OP_ADD: m_curPos += param->AsInteger(); break;
		case TJS_OP_SUB: m_curPos -= param->AsInteger(); break;
		case TJS_OP_INC: ++m_curPos; break;
		case TJS_OP_DEC: --m_curPos; break;
		default:
			return TJS_E_NOTIMPL;
		}
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD IsValid(tjs_uint32 flag, const tjs_char *membername,
		tjs_uint32 *hint, iTJSDispatch2 *objthis) override
	{
		return m_buff ? TJS_S_TRUE : TJS_S_FALSE;
	}

	tjs_error TJS_INTF_METHOD Invalidate(tjs_uint32 flag, const tjs_char *membername,
		tjs_uint32 *hint, iTJSDispatch2 *objthis) override
	{
		m_buff = nullptr;
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD GetCount(tjs_int *result, const tjs_char *membername,
		tjs_uint32 *hint, iTJSDispatch2 *objthis) override
	{
		if (membername) return TJS_E_MEMBERNOTFOUND;
		*result = m_length;
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD PropSetByNum(tjs_uint32 flag, tjs_int num,
		const tTJSVariant *param, iTJSDispatch2 *objthis) override
	{
		num += m_curPos;
		if (num < 0 || num >= (tjs_int)m_length) return TJS_E_MEMBERNOTFOUND;
		m_buff[num] = (unsigned char)param->AsInteger();
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD PropSet(tjs_uint32 flag, const tjs_char *membername,
		tjs_uint32 *hint, const tTJSVariant *param, iTJSDispatch2 *objthis) override
	{
		if (!membername) return TJS_E_NOTIMPL;
		if (!TJS_strcmp(membername, TJS_W("ptr"))) {
			m_curPos = (unsigned int)param->AsInteger();
			return TJS_S_OK;
		}
		return TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD PropGetByNum(tjs_uint32 flag, tjs_int num,
		tTJSVariant *result, iTJSDispatch2 *objthis) override
	{
		num += m_curPos;
		if (num < 0 || num >= (tjs_int)m_length) return TJS_E_MEMBERNOTFOUND;
		*result = (tjs_int32)m_buff[num];
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD PropGet(tjs_uint32 flag, const tjs_char *membername,
		tjs_uint32 *hint, tTJSVariant *result, iTJSDispatch2 *objthis) override
	{
		if (!membername) return TJS_E_NOTIMPL;
		if (!TJS_strcmp(membername, TJS_W("count"))) {
			*result = (tjs_int64)m_length;
		} else if (!TJS_strcmp(membername, TJS_W("ptr"))) {
			*result = (tjs_int64)m_curPos;
		} else {
			result->Clear();
		}
		return TJS_S_OK;
	}

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 flag, const tjs_char *membername,
		tjs_uint32 *hint, tTJSVariant *result, tjs_int numparams,
		tTJSVariant **param, iTJSDispatch2 *objthis) override
	{
		// The member hint is a name HASH, not a flag.  Storing a boolean in it
		// (as this used to) poisons the caller's cache: the next lookup of any
		// member reuses that bogus hint, dispatch goes to the wrong function,
		// and TJS reports "Member xor does not exist" for a method that is right
		// here.  The scripts this serves are the whole decryption path of an
		// encrypted title, so a failed xor() leaves archive chunks undecrypted
		// and the game reads garbage.  See how "ptr" is handled below for the
		// correct pattern.
		static const tjs_uint32 hash_xor = tTJSHashFunc<tjs_char *>::Make(TJS_W("xor"));
		static const tjs_uint32 hash_add = tTJSHashFunc<tjs_char *>::Make(TJS_W("add"));
		if (hint) {
			if (!*hint) *hint = tTJSHashFunc<tjs_char *>::Make(membername);
			if (*hint == hash_xor) return FuncXor(numparams, param);
			if (*hint == hash_add) return FuncAdd(numparams, param);
			return TJS_E_NOTIMPL;
		}
		if (!TJS_strcmp(membername, TJS_W("xor"))) return FuncXor(numparams, param);
		if (!TJS_strcmp(membername, TJS_W("add"))) return FuncAdd(numparams, param);
		return TJS_E_NOTIMPL;
	}

private:
	tjs_error FuncAdd(tjs_int numparams, tTJSVariant **param)
	{
		if (numparams < 3) {
			return TJS_E_BADPARAMCOUNT;
		}
		int bufoff = (int)param[0]->AsInteger();
		int len = (int)param[1]->AsInteger();
		unsigned char xorval = (unsigned char)param[2]->AsInteger();
		if (bufoff < 0 || len < 0 ||
			m_curPos + bufoff + len > m_length) return TJS_E_FAIL;
		unsigned char *buf = m_buff + m_curPos + bufoff;
		for (int i = 0; i < len; ++i)
			buf[i] += xorval;
		return TJS_S_OK;
	}

	tjs_error FuncXor(tjs_int numparams, tTJSVariant **param)
	{
		if (numparams < 3) {
			return TJS_E_BADPARAMCOUNT;
		}
		int bufoff = (int)param[0]->AsInteger();
		int len = (int)param[1]->AsInteger();
		unsigned char xorval = (unsigned char)param[2]->AsInteger();
		if (bufoff < 0 || len < 0 ||
			m_curPos + bufoff + len > m_length) return TJS_E_FAIL;
		unsigned char *buf = m_buff + m_curPos + bufoff;
		unsigned char *pend = buf + len;
		if (len > 32)
		{
			int PreFragLen = (int)((unsigned char*)((((intptr_t)buf) + 7)&~7) - buf);
			for (int i = 0; i < PreFragLen; i++) *(buf++) ^= xorval;

			uint64_t k = xorval;
			k |= k << 8;
			k |= k << 16;
			k |= k << 32;
			unsigned char* pVecEnd = (unsigned char*)(((intptr_t)pend)&~7) - 7;
			while (buf < pVecEnd) {
				*(uint64_t*)(void*)buf ^= k;
				buf += 8;
			}
		}
		while (buf < pend) *(buf++) ^= xorval;
		return TJS_S_OK;
	}
};
//---------------------------------------------------------------------------
// Per-thread decoder engine.  Each thread that touches filtered archives gets
// its own tTJS instance running the game's xp3filter.tjs; engines are rebuilt
// lazily when the launcher arms a different script (per game launch).
//---------------------------------------------------------------------------
struct XP3FilterDecoder {
	tTJS *ScriptEngine = nullptr;
	tTJSVariantClosure ManagedDecoder;
	tTJSVariantClosure ManagedFilter;
	tjs_uint ScriptVersion = 0;
};

static ttstr sXP3FilterScript;
static tjs_uint sXP3FilterScriptVersion = 0;

class XP3FilterRegister : public tTJSDispatch
{
    typedef tTJSDispatch inherited;
protected:
    XP3FilterDecoder *Decoder;
public:
    XP3FilterRegister(XP3FilterDecoder *decoder) : Decoder(decoder) {}
    tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 flag, const tjs_char *membername,
        tjs_uint32 *hint, tTJSVariant *result, tjs_int numparams,
        tTJSVariant **param, iTJSDispatch2 *objthis) override
    {
        if (membername) return inherited::FuncCall(flag, membername, hint,
            result, numparams, param, objthis);
        if (result) result->Clear();
        if (numparams < 1) return TJS_E_BADPARAMCOUNT;
        if (Decoder->ManagedDecoder.Object) Decoder->ManagedDecoder.Release();
        Decoder->ManagedDecoder = param[0]->AsObjectClosure();
        return TJS_S_OK;
    }
};

class XP3ContentFilterRegister : public XP3FilterRegister
{
	typedef XP3FilterRegister inherited;
public:
	XP3ContentFilterRegister(XP3FilterDecoder *decoder) : XP3FilterRegister(decoder) {}
	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 flag, const tjs_char *membername,
		tjs_uint32 *hint, tTJSVariant *result, tjs_int numparams,
		tTJSVariant **param, iTJSDispatch2 *objthis) override
	{
		if (membername) return inherited::FuncCall(flag, membername, hint,
			result, numparams, param, objthis);
		if (result) result->Clear();
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		if (Decoder->ManagedFilter.Object) Decoder->ManagedFilter.Release();
		Decoder->ManagedFilter = param[0]->AsObjectClosure();
		return TJS_S_OK;
	}
};

static XP3FilterDecoder *AddXP3Decoder()
{
	XP3FilterDecoder *decoder = new XP3FilterDecoder();
	decoder->ScriptEngine = new tTJS();
	decoder->ScriptVersion = sXP3FilterScriptVersion;
	iTJSDispatch2 *global = decoder->ScriptEngine->GetGlobalNoAddRef();
	tTJSVariant val;
	iTJSDispatch2 *dsp;
	// A minimal Storages class carrying the filter registration methods; the
	// game filter scripts only ever call these two members on it.
	tTJSNativeClass *cls = TVPCreateNativeClass_Storages();
	TJSNativeClassRegisterNCM(cls, TJS_W("setXP3ArchiveExtractionFilter"),
		new XP3FilterRegister(decoder), cls->GetClassName().c_str(), nitMethod, TJS_STATICMEMBER);
	TJSNativeClassRegisterNCM(cls, TJS_W("setXP3ArchiveContentFilter"),
		new XP3ContentFilterRegister(decoder), cls->GetClassName().c_str(), nitMethod, TJS_STATICMEMBER);
	dsp = cls;
	val = tTJSVariant(dsp);
	dsp->Release();
	global->PropSet(TJS_MEMBERENSURE|TJS_IGNOREPROP, TJS_W("Storages"), nullptr, &val, global);
	decoder->ScriptEngine->ExecScript(sXP3FilterScript.c_str());
	return decoder;
}

static XP3FilterDecoder *FetchXP3Decoder()
{
	thread_local XP3FilterDecoder *decoder = nullptr;
	if (!decoder || decoder->ScriptVersion != sXP3FilterScriptVersion)
	{
		// This thread's own stale engine; other threads rebuild on their next
		// fetch, so no cross-thread deletion ever happens.
		delete decoder;
		decoder = AddXP3Decoder();
	}
	return decoder;
}

// ---------------------------------------------------------------------------
// Native fast path for the common filter shape.
//
// A filter script is invoked once per 2048-byte chunk through its own TJS
// engine, which costs milliseconds per call on this target: a single 200 KB
// script then takes seconds to load, and titles that encrypt everything pay it
// on every file.  YuzuSoft's filter is a pure function of (hash, offset, size)
// -- derive a key from the file hash, XOR the chunk with its low byte, with one
// extra pass over the first byte at offset 0 -- which is trivial to do inline.
//
// The switch is EARNED, not assumed: the first few calls run BOTH paths and
// compare byte for byte, and only an exact match retires the script.  A filter
// that is not this shape, or that carries state between chunks, simply never
// verifies and keeps going through the script as before.
static bool sNativeXorVerified = false;
static bool sNativeXorDisabled = false;
static unsigned sNativeXorChecks = 0;
static const unsigned kNativeXorChecksRequired = 4;
static const tjs_uint32 kYuzuSoftHashKey = 0xABCD9876u;
static const unsigned char kYuzuSoftFirstFallback = 0x76;
static const unsigned char kYuzuSoftChunkFallback = 0xA5;

static unsigned char NativeXorKeyByte(tjs_uint32 k)
{
	return (k & 0xFF) ? (unsigned char)(k & 0xFF) : kYuzuSoftChunkFallback;
}

static void NativeXorDecrypt(unsigned char *buf, unsigned len, tjs_uint64 hash, tjs_uint64 offset)
{
	tjs_uint32 k = (tjs_uint32)hash ^ kYuzuSoftHashKey;
	// The extra first-byte pass belongs to offset 0 only -- the script guards it
	// with `if(!o && l)`, and applying it to every chunk would corrupt all but
	// the first one.
	if (offset == 0 && len > 0)
	{
		const unsigned char first = (k & 0xFF) ? (unsigned char)(k & 0xFF) : kYuzuSoftFirstFallback;
		buf[0] ^= first;
	}
	k = (k ^ (k >> 8) ^ (k >> 16) ^ (k >> 24)) & 0xFFFFFFFFu;
	const unsigned char key = NativeXorKeyByte(k);
	for (unsigned i = 0; i < len; ++i) buf[i] ^= key;
}

static void TVPXP3ArchiveExtractionFilterWrapper(tTVPXP3ExtractionFilterInfo *info)
{
	try
	{
		if (sNativeXorVerified)
		{
			NativeXorDecrypt((unsigned char*)info->Buffer, (unsigned)info->BufferSize, info->FileHash, info->Offset);
			return;
		}

		XP3FilterDecoder *decoder = FetchXP3Decoder();
		if (decoder && decoder->ManagedDecoder.Object)
		{
			// Keep the pre-filter bytes so the native path can be checked against
			// what the script produced.
			unsigned char *before = nullptr;
			if (!sNativeXorDisabled && sNativeXorChecks < kNativeXorChecksRequired)
			{
				before = new unsigned char[info->BufferSize];
				memcpy(before, info->Buffer, (size_t)info->BufferSize);
			}

			tTJSVariant FileHash((tjs_int64)info->FileHash);
			tTJSVariant Offset((tjs_int64)info->Offset);
			CBinaryAccessor *buf = new CBinaryAccessor((unsigned char*)info->Buffer, info->BufferSize);
			tTJSVariant Buffer(buf);
			buf->Release();
			tTJSVariant BufferSize((tjs_int64)info->BufferSize);
			tTJSVariant *vars[] = { &FileHash, &Offset, &Buffer, &BufferSize };
			decoder->ManagedDecoder.FuncCall(0, nullptr, nullptr, nullptr,
				sizeof(vars) / sizeof(vars[0]), vars, nullptr);

			if (before)
			{
				NativeXorDecrypt(before, (unsigned)info->BufferSize, info->FileHash, info->Offset);
				if (memcmp(before, info->Buffer, (size_t)info->BufferSize) == 0)
				{
					delete[] before;
					if (++sNativeXorChecks >= kNativeXorChecksRequired)
					{
						sNativeXorVerified = true;
						KRKRNS_LOG("[xp3filter] native xor path verified after %u chunks"
							" -- script call retired", sNativeXorChecks);
					}
				}
				else
				{
					delete[] before;
					sNativeXorDisabled = true;   // not this shape; script stays in charge
					KRKRNS_LOG("[xp3filter] native xor path rejected: script output differs");
				}
			}
		}
	}
	catch(eTJSError &e)
	{
		// A throwing filter would surface as an unreadable archive; log the
		// reason plus the arguments, so an incomplete or mis-ordered parameter
		// list can be told apart from a damaged archive.  Ours passes four
		// arguments where Kirikiroid2 passes six (it adds the file name and the
		// caller context); scripts that only declare four must still receive
		// them intact.
		KRKRNS_LOG("[xp3filter] callback threw at offset %lld size %u hash=%lld: %s",
			(long long)info->Offset, (unsigned)info->BufferSize,
			(long long)info->FileHash,
			e.GetMessage().AsNarrowStdString().c_str());
	}
	catch(...)
	{
		KRKRNS_LOG("[xp3filter] callback threw (non-script) at offset %lld size %u",
			(long long)info->Offset, (unsigned)info->BufferSize);
	}
}

void TVPSetXP3FilterScript(const ttstr & content)
{
	// Install the native hook (idempotent) and arm the script; per-thread
	// decoder engines rebuild lazily on their next extraction.
	TVPXP3ArchiveExtractionFilter = TVPXP3ArchiveExtractionFilterWrapper;
	if (sXP3FilterScript != content)
	{
		sXP3FilterScript = content;
		++sXP3FilterScriptVersion;
		KRKRNS_LOG("[xp3filter] script armed (%d chars)", (int)content.GetLen());
	}
}
//---------------------------------------------------------------------------

// varfile.dll: the "var://" storage.
//
// Ported from Kirikiroid2 (src/plugins/varfile.cpp).  It exposes a TJS
// dictionary (or array) tree as a storage: "var://./path" reads/writes octet
// members of the global object.  KAG titles use it as an in-memory working
// area (config snapshot, quick save staging) and probe its media name at boot.
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>
#include "ncbind/ncbind.hpp"
#include "StorageIntf.h"

#define NCB_MODULE_NAME TJS_W("varfile.dll")
#define BASENAME TJS_W("var")

// 辞書かどうかの判定
static bool isDirectory(tTJSVariant &base) {
	return base.Type() == tvtObject && base.AsObjectNoAddRef() != NULL;
}

// ファイルかどうかの判定
static bool isFile(const tTJSVariant &file) {
	return file.Type() == tvtOctet;
}

/**
 * Variant参照型ストリーム
 */
class VariantStream : public tTJSBinaryStream {

public:
	VariantStream(tTJSVariant &parent) : refCount(1), parent(parent), stream(0), cur(0) {};

	virtual ~VariantStream() {
		close();
	}

	/**
	 * ファイルを開く
	 */
	bool open(const ttstr &name, tjs_uint32 flags) {
		close();
		this->name = name;

		// 読み込みのみの場合
		if (flags == TJS_BS_READ) {
			parent.AsObjectClosureNoAddRef().PropGet(0, name.c_str(), NULL, &value, NULL);
			return isFile(value);
		}

		// 書き込みが必要な場合
		stream = new tTVPMemoryStream();

		// オブジェクトの内容を複製
		if (flags == TJS_BS_UPDATE || flags == TJS_BS_APPEND) {
			parent.AsObjectClosureNoAddRef().PropGet(0, name.c_str(), NULL, &value, NULL);
			if (isFile(value)) {
				stream->Write(value.AsOctetNoAddRef()->GetData(), value.AsOctetNoAddRef()->GetLength());
				stream->Seek(0, flags == TJS_BS_UPDATE ? TJS_BS_SEEK_SET : TJS_BS_SEEK_END);
			}
		}
		return true;
	}

	// ISequentialStream
	virtual tjs_uint TJS_INTF_METHOD Read(void *pv, tjs_uint cb) {
		if (stream) {
			return stream->Read(pv, cb);
		} else {
			const tjs_uint8 *base = getBase();
			tTVInteger size = getSize() - cur;
			if (base && cb > 0 && size > 0) {
				if (cb > size) {
					cb = size;
				}
				memcpy(pv, base + cur, cb);
				cur += cb;
				return cb;
			} else {
				return 0;
			}
		}
	}

	virtual tjs_uint TJS_INTF_METHOD Write(const void *pv, tjs_uint cb) {
		if (stream) {
			return stream->Write(pv, cb);
		} else {
			return 0;
		}
	}

	// IStream
	virtual tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 dlibMove, tjs_int dwOrigin) {
		if (stream) {
			return stream->Seek(dlibMove, dwOrigin);
		} else {
			switch (dwOrigin) {
			case TJS_BS_SEEK_CUR:
				cur += dlibMove;
				break;
			case TJS_BS_SEEK_SET:
				cur = dlibMove;
				break;
			case TJS_BS_SEEK_END:
				cur = getSize();
				cur += dlibMove;
				break;
			}
			return cur;
		}
	}

	virtual tjs_uint64 TJS_INTF_METHOD GetSize() {
		if (stream) {
			return stream->GetSize();
		} else {
			return getSize();
		}
	}

protected:

	/**
	 * ファイルを閉じる処理
	 */
	void close() {
		if (stream) {
			delete stream;
			stream = NULL;
		}
		value.Clear();
		cur = 0;
	}

	// 読み込み用メモリ領域取得
	const tjs_uint8 *getBase() {
		return isFile(value) ? value.AsOctetNoAddRef()->GetData() : NULL;
	}

	// 読み込み用メモリサイズ取得
	virtual tjs_uint64 TJS_INTF_METHOD getSize() {
		return isFile(value) ? value.AsOctetNoAddRef()->GetLength() : 0;
	}

private:
	int refCount;
	tTJSVariant parent;
	ttstr name;
	tTJSVariant value;
	tTJSBinaryStream *stream;
	tTVInteger cur;
};

/**
 * メンバ登録処理用
 */
class GetLister : public tTJSDispatch /** EnumMembers 用 */
{
public:
	GetLister(iTVPStorageLister *lister) : lister(lister) {};

	virtual tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag,
		const tjs_char * membername,
		tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams,
		tTJSVariant **param,
		iTJSDispatch2 *objthis)
	{
		if (numparams > 1) {
			tTVInteger flag = param[1]->AsInteger();
			if (!(flag & TJS_HIDDENMEMBER) && isFile(*param[2])) {
				lister->Add(ttstr(param[0]->GetString()));
			}
		}
		if (result) {
			*result = true;
		}
		return TJS_S_OK;
	}

private:
	iTVPStorageLister *lister;
};

/**
 * Varストレージ
 */
class VarStorage : public iTVPStorageMedia
{
public:
	VarStorage() : refCount(1) {
	}

	// -----------------------------------
	// iTVPStorageMedia Intefaces
	// -----------------------------------

	virtual void TJS_INTF_METHOD AddRef() {
		refCount++;
	};

	virtual void TJS_INTF_METHOD Release() {
		if (refCount == 1) {
			delete this;
		} else {
			refCount--;
		}
	};

	// returns media name like "file", "http" etc.
	virtual void TJS_INTF_METHOD GetName(ttstr &name) {
		name = BASENAME;
	}

	// normalize domain name according with the media's rule
	virtual void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) {
		// nothing to do
	}

	// normalize path name according with the media's rule
	virtual void TJS_INTF_METHOD NormalizePathName(ttstr &name) {
		// nothing to do
	}

	// check file existence
	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) {
		return isFile(getFile(name));
	}

	// open a storage and return a tTJSBinaryStream instance.
	virtual tTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags) {
		tTJSBinaryStream *ret = NULL;
		ttstr fname;
		tTJSVariant parent = getParentName(name, fname);
		if (isDirectory(parent) && fname.length() > 0) {
			VariantStream *stream = new VariantStream(parent);
			if (!stream->open(fname, flags)) {
				delete stream;
			} else {
				ret = stream;
			}
		}
		if (!ret) {
			TVPThrowExceptionMessage(TJS_W("cannot open memfile:%1"), name);
		}
		return ret;
	}

	// list files at given place
	virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister * lister) {
		tTJSVariant base = getFile(name);
		if (isDirectory(base)) {
			tTJSVariantClosure closure(new GetLister(lister));
			base.AsObjectClosureNoAddRef().EnumMembers(TJS_IGNOREPROP, &closure, NULL);
			closure.Release();
		}
	}

	// check wether given name is easily accessible from local OS filesystem.
	virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) {
		name = "";
	}

protected:

	virtual ~VarStorage() {
	}

	/*
	 * 親フォルダとパスを返す
	 */
	tTJSVariant getParentName(const ttstr &name, ttstr &fname) {
		// ドメイン部を分離
		const tjs_char *p = name.c_str();
		const tjs_char *q;
		if ((q = TJS_strchr(p, '/'))) {
			ttstr dname = ttstr(p, q-p);
			if (dname != TJS_W(".")) {
				TVPThrowExceptionMessage(TJS_W("no such domain:%1"), dname);
			}
		} else {
			TVPThrowExceptionMessage(TJS_W("invalid path:%1"), name);
		}
		// パス名
		ttstr path = ttstr(q+1);
		iTJSDispatch2 *global = TVPGetScriptDispatch();
		tTJSVariant base(global, global);
		while (path.length() > 0) {
			p = path.c_str();
			q = TJS_strchr(p, '/');
			if (q == NULL) {
				// ファイル
				break;
			} else if (q == p) {
				// フォルダ名が空
				base.Clear();
				break;
			} else {
				// フォルダ
				ttstr member = ttstr(p, q-p);
				tTJSVariant value;
				tTJSVariantClosure &o = base.AsObjectClosureNoAddRef();
				if (((o.IsInstanceOf(0, NULL, NULL, TJS_W("Array"), NULL) == TJS_S_TRUE &&
					  TJS_SUCCEEDED(o.PropGetByNum(0, (tjs_int)member.AsInteger(), &value, NULL))) ||
					 (TJS_SUCCEEDED(o.PropGet(0, member.c_str(), NULL, &value, NULL)))) && isDirectory(value)) {
					base = value;
					path = ttstr(q+1);
				} else {
					base.Clear();
					break;
				}
			}
		}
		fname = path;
		return base;
	}

	/*
	 * ファイル名に合致する変数を探して返す
	 */
	tTJSVariant getFile(const ttstr &name) {
		ttstr fname;
		tTJSVariant base = getParentName(name, fname);
		if (isDirectory(base) && fname.length() > 0) {
			// ファイル
			tTJSVariant value;
			if (TJS_SUCCEEDED(base.AsObjectClosureNoAddRef().PropGet(0, fname.c_str(), NULL, &value, NULL))) {
				base = value;
			} else {
				base.Clear();
			}
		}
		return base;
	}

private:
	tjs_uint refCount; //< リファレンスカウント
};

VarStorage *var = NULL;

/**
 * 登録処理前
 */
static void PreRegistCallback()
{
	if (var == NULL) {
		var = new VarStorage();
		TVPRegisterStorageMedia(var);
	}
}

/**
 * 開放処理後
 */
static void PostUnregistCallback()
{
	if (var != NULL) {
		TVPUnregisterStorageMedia(var);
		var->Release();
		var = NULL;
	}
}

NCB_PRE_REGIST_CALLBACK(PreRegistCallback);
NCB_POST_UNREGIST_CALLBACK(PostUnregistCallback);

/* Anchor: a static archive drops translation units that nothing references, and
   the only reference to this one is the ncbind auto-register table.  PluginImpl
   calls this function before ncbAutoRegister::LoadModule. */
extern "C" void krkrsdl2_link_varfile_plugin() {}

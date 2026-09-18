//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Universal Storage System
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "KrkrNSLog.h"

#include <algorithm>
#include <stdexcept>
#include <memory>
#include <set>
#include <vector>
#include "StorageIntf.h"
#include "BinaryStream.h"
#include <errno.h>
#include "CharacterSet.h"
#include "tjsUtils.h"
#include "MsgIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "TextStream.h"
#include "tjsArray.h"
#include "SysInitIntf.h"
#include "XP3Archive.h"
#include "TickCount.h"
#include "StringUtil.h"
#include "FilePathUtil.h"
#include "tjsDictionary.h"
#ifdef __SWITCH__
#include "ScriptMgnIntf.h"

extern std::vector<ttstr> krkrsdl2_list_game_directories();
extern std::vector<ttstr> krkrsdl2_list_game_files(const ttstr &game_directory);
extern bool krkrsdl2_is_builtin_plugin_name(const ttstr & short_name);
extern unsigned krkrsdl2_autocycle_round_count();
extern ttstr krkrsdl2_prepare_xp3_game(const ttstr &game_directory, const ttstr &selected);
extern void krkrsdl2_mount_xp3_resources();
extern bool krkrsdl2_can_launch_game();
extern bool TVPTerminateOnWindowClose;
#endif

#define TVP_DEFAULT_ARCHIVE_CACHE_NUM 64
#define TVP_DEFAULT_AUTOPATH_CACHE_NUM 256


//---------------------------------------------------------------------------
// オプション
//---------------------------------------------------------------------------
static bool TVPIsInitStorageOptions = false;
static bool TVPIgnoreFileProperty = false;
//---------------------------------------------------------------------------
static void TVPInitStorageOptions() {
	if( TVPIsInitStorageOptions ) return;

	tTJSVariant val;
	if( TVPGetCommandLine( TJS_W( "-ignorefileprop" ), &val ) ) {
		ttstr str( val );
		if( str == TJS_W( "yes" ) )
			TVPIgnoreFileProperty = true;
		else
			TVPIgnoreFileProperty = false;
	}
	TVPIsInitStorageOptions = true;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// global variables
//---------------------------------------------------------------------------
// current media ( ex. "http" "ftp" "file" )
ttstr TVPCurrentMedia;
// archive delimiter
// this changes '>' from '#' since 2.19 beta 14
tjs_char  TVPArchiveDelimiter = '>';
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// statics
//---------------------------------------------------------------------------
static tTJSCriticalSection TVPCreateStreamCS;
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// utilities
//---------------------------------------------------------------------------
ttstr TVPStringFromBMPUnicode(const tjs_uint16 *src, tjs_int maxlen)
{
	// convert to ttstr from BMP unicode
	if(sizeof(tjs_char) == 2)
	{
		// sizeof(tjs_char) is 2 (windows native)
		if(maxlen == -1)
			return ttstr((const tjs_char*)src);
		else
			return ttstr((const tjs_char*)src, maxlen);
	}
	else if(sizeof(tjs_char) == 4)
	{
		// sizeof(tjs_char) is 4 (UCS32)
  		// FIXME: NOT TESTED CODE
		tjs_int len = 0;
		const tjs_uint16 *p = src;
		while(*p) len++, p++;
		if(maxlen != -1 && len > maxlen) len = maxlen;
		ttstr ret((tTJSStringBufferLength)(len));
		tjs_char *dest = ret.Independ();
		p = src;
		while(len && *p)
		{
			*dest = *p;
			dest++;
			p++;
			len --;
		}
		*dest = 0;
		ret.FixLen();
		return ret;
	}
	return (const tjs_char*)TVPTjsCharMustBeTwoOrFour;
}
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// tTVPStorageMediaManager
//---------------------------------------------------------------------------
class tTVPStorageMediaManager
{
	class tMediaNameString : public tTJSString
	{
	public:
		bool operator == (const tMediaNameString &rhs) const
		{
			const tjs_char * l_p = c_str();
			const tjs_char * r_p = rhs.c_str();

			while(*l_p && *r_p)
			{
				if(*l_p == TJS_W(':')) break;
				if(*r_p == TJS_W(':')) break;
				if(*l_p != *r_p) break;
				l_p++;
				r_p++;
			}
			if((*l_p == TJS_W(':') || *l_p == 0) &&
				(*r_p == TJS_W(':') || *r_p == 0)) return true;
			return false;
		}
	};

	class tHashFunc
	{
	public:
		static tjs_uint32 Make(const tMediaNameString &key)
		{
			if(key.IsEmpty()) return 0;
			const tjs_char *str = key.c_str();
			tjs_uint32 ret = 0;
			while(*str && *str != ':')
			{
				ret += *str;
				ret += (ret << 10);
				ret ^= (ret >> 6);
				str++;
			}
			ret += (ret << 3);
			ret ^= (ret >> 11);
			ret += (ret << 15);
			if(!ret) ret = (tjs_uint32)-1;
			return ret;
		}
	};

	class tMediaRecord
	{
	public:
		ttstr CurrentDomain;
		ttstr CurrentPath;
		tTJSRefHolder<iTVPStorageMedia> MediaIntf;
		tjs_int MediaNameLen;
//		bool IsCaseSensitive;

		tMediaRecord(iTVPStorageMedia *media) : MediaIntf(media), CurrentDomain("."), CurrentPath("/")
			{ ttstr name; media->GetName(name); MediaNameLen = name.GetLen();
			/*IsCaseSensitive = media->IsCaseSensitive();*/ }

		const tjs_char *GetDomainAndPath(const ttstr &name)
		{
			return name.c_str() + MediaNameLen + 3;
				// 3 = strlen("://")
		}
	};

	typedef tTJSHashTable<tMediaNameString, tMediaRecord, tHashFunc, 16> tHashTable;

	tHashTable HashTable;

public:
	tTVPStorageMediaManager();
	~tTVPStorageMediaManager();

private:
	static void ThrowUnsupportedMediaType(const ttstr &name);
	tMediaRecord * GetMediaRecord(const ttstr &name);

public:
	void Register(iTVPStorageMedia * media);
	void Unregister(iTVPStorageMedia * media);

	ttstr NormalizeStorageName(const ttstr &name, ttstr *ret_media = NULL,
		ttstr *ret_domain = NULL, ttstr *ret_path = NULL);

	void SetCurrentDirectory(const ttstr &name);

	static ttstr ExtractMediaName(const ttstr &name);

	bool CheckExistentStorage(const ttstr & name);
	tTJSBinaryStream * Open(const ttstr & name, tjs_uint32 flags);
	void GetListAt(const ttstr &name, iTVPStorageLister *lister);
	ttstr GetLocallyAccessibleName(const ttstr &name);
} TVPStorageMediaManager;
//---------------------------------------------------------------------------
tTVPStorageMediaManager::tTVPStorageMediaManager()
{
	iTVPStorageMedia *filemedia = TVPCreateFileMedia();
	Register(filemedia);
	filemedia->Release();
}
//---------------------------------------------------------------------------
tTVPStorageMediaManager::~tTVPStorageMediaManager()
{
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::ThrowUnsupportedMediaType(const ttstr &name)
{
	TVPThrowExceptionMessage(TVPUnsupportedMediaName, ExtractMediaName(name));
}
//---------------------------------------------------------------------------
tTVPStorageMediaManager::tMediaRecord *
	tTVPStorageMediaManager::GetMediaRecord(const ttstr &name)
{
	tMediaRecord *rec = HashTable.Find(*(tMediaNameString*)&name);
	if(!rec) ThrowUnsupportedMediaType(name);
	return rec;
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::Register(iTVPStorageMedia * media)
{
	ttstr medianame;
	media->GetName(medianame);

	tMediaRecord *rec = HashTable.Find(*(tMediaNameString*)&medianame);
	if(rec)
		TVPThrowExceptionMessage( TVPMediaNameHadAlreadyBeenRegistered, medianame );

	tMediaRecord new_rec(media);

	HashTable.Add(*(tMediaNameString*)&medianame, new_rec);
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::Unregister(iTVPStorageMedia * media)
{
	ttstr medianame;
	media->GetName(medianame);

	tMediaRecord *rec = HashTable.Find(*(tMediaNameString*)&medianame);
	if(!rec)
		TVPThrowExceptionMessage( TVPMediaNameIsNotRegistered, medianame );
	HashTable.Delete(*(tMediaNameString*)&medianame);
}
//---------------------------------------------------------------------------
ttstr tTVPStorageMediaManager::NormalizeStorageName(const ttstr &name,
	ttstr *ret_media, ttstr *ret_domain, ttstr *ret_path)
{
	// Normalize storage name.

	// storage name is basically in following form:
	// media://domain/path

	// media is sort of access method, like "file", "http" ...etc.
	// domain represents in which computer the data is.
	// path is where the data is in the computer.

	// empty check
	if(name.IsEmpty()) return name; // empty name is empty name

	// pre-normalize
	const tjs_char *pca;//, *pcb, *pcc;
	tjs_char *pa, *pb, *pc;

	ttstr tmp(name);
	TVPPreNormalizeStorageName(tmp);

	// unify path delimiter
	pa = tmp.Independ();
	while(*pa)
	{
		if(*pa == TJS_W('\\')) *pa = TJS_W('/');
		pa++;
	}

	// save in-archive storage name and normalize it
	ttstr inarchive_name;
	bool inarc_name_found = false;
	pca = tmp.c_str();
	pa = const_cast<tjs_char *>(TJS_strchr(pca, TVPArchiveDelimiter));
	if(pa)
	{
		inarchive_name = ttstr(pa + 1);
		tTVPArchive::NormalizeInArchiveStorageName(inarchive_name);
		inarc_name_found = true;
		tmp = ttstr(pca, (int)(pa - pca));
	}
	if(tmp.IsEmpty()) TVPThrowExceptionMessage(TVPInvalidPathName, name);


	// split the name into media, domain, path
	// (and guess what component is omitted)
	ttstr media, domain, path;

	// - find media name
	//   media name is: /^[A-Za-z]+:/
	pa = pb = tmp.Independ();
	while(*pa)
	{
		if(!(
			*pa >= TJS_W('A') && *pa <= TJS_W('Z') ||
			*pa >= TJS_W('a') && *pa <= TJS_W('z') )) break;
		pa ++;
	}

	if(*pa == TJS_W(':'))
	{
		// media name found
		media = ttstr(pb, (int)(pa - pb));
		pa ++;
	}
	else
	{
		pa = pb;
	}

	// - find domain name
	// at this place, pa may point one of following:
	//  ///path        (domain is omitted)
	//  //domain/path  (none is omitted)
	//  /path          (domain is omitted)
	//  relative-path  (domain and current path are omitted)

	if(pa[0] == TJS_W('/'))
	{
		if(pa[1] == TJS_W('/'))
		{
			if(pa[2] == TJS_W('/'))
			{
				// slash count 3: domain is ommited
				pa += 2;
			}
			else
			{
				// slash count 2: none is omitted
				pa += 2;
				// find '/' as a domain delimiter
				pc = TJS_strchr(pa, TJS_W('/'));
				if(!pc)
					TVPThrowExceptionMessage(TVPInvalidPathName, name);
				domain = ttstr(pa, (int)(pc - pa));
				pa = pc;
			}
		}
		else
		{
			// slash count 1: domain is omitted
			;
			//
		}
	}

	// - get path name
	path = pa;

	// supply omitted and normalize
	if(media.IsEmpty())
	{
		media = TVPCurrentMedia;
	}
	else
	{
		// normalize media name ( make them all small )
		tjs_char *p = media.Independ();
		while(*p)
		{
			if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
				*p += (TJS_W('a') - TJS_W('A'));
			p ++;
		}
	}

	tMediaRecord * mediarec = GetMediaRecord(media);

	if(domain.IsEmpty()) domain = mediarec->CurrentDomain;
	mediarec->MediaIntf.GetObjectNoAddRef()->NormalizeDomainName(domain);

	if(path.IsEmpty())
	{
		path = TJS_W("/");
	}
	else if(path.c_str()[0] != TJS_W('/'))
	{
		path = mediarec->CurrentPath + path;
	}
	mediarec->MediaIntf.GetObjectNoAddRef()->NormalizePathName(path);

	// compress redudant path accesses
	if(inarc_name_found)
	{
		tjs_char tmp[2];
		tmp[0] = TVPArchiveDelimiter;
		tmp[1] = 0;
		path += tmp + inarchive_name;
	}

	pa = pb = pc = path.Independ(); // pa = read pointer, pb = write pointer, pc = start
	tjs_int dot_count = -1;

	while(true)
	{
		if(*pa == TVPArchiveDelimiter || *pa == TJS_W('/') || *pa == 0)
		{
			tjs_char delim = 0;

			if(*pa && dot_count == 0)
			{
				// duplicated slashes
				pb --;
			}
			else if(dot_count > 0)
			{
				pb --;
				while(pb >= pc)
				{
					if(*pb == TJS_W('/') || *pb == TVPArchiveDelimiter)
					{
						dot_count --;
						if(dot_count == 0)
						{
							delim = *pb;
							break;
						}
						if(*pb == TVPArchiveDelimiter) TVPThrowExceptionMessage(TVPInvalidPathName, name);
					}
					pb --;
				}
				if(pb < pc) TVPThrowExceptionMessage(TVPInvalidPathName, name);
			}

			if(!delim)
				*pb = *pa;
			else
				*pb = delim;
			if(*pa == 0) break;
			pb ++;
			pa ++;
			dot_count = 0;
		}
		else if(*pa == TJS_W('.'))
		{
			*(pb++) = *(pa++);
			if(dot_count != -1) dot_count ++;
		}
		else
		{
			*(pb++) = *(pa++);
			dot_count = -1;
		}
	}

	path.FixLen();

	// merge and return normalize storage name
	if(ret_media) *ret_media = media;
	if(ret_domain) *ret_domain = domain;
	if(ret_path) *ret_path = path;

	tmp = media + TJS_W("://") + domain + path;

	return tmp;
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::SetCurrentDirectory(const ttstr &name)
{
	tjs_char ch = name.GetLastChar();
	if(ch != TJS_W('/') && ch != TJS_W('\\') && ch != TVPArchiveDelimiter)
		TVPThrowExceptionMessage(TVPMissingPathDelimiterAtLast);

	ttstr media, domain, path;
	NormalizeStorageName(name, &media, &domain, &path);

	tMediaRecord *rec = GetMediaRecord(media);
	rec->CurrentDomain = domain;
	rec->CurrentPath = path;
	TVPCurrentMedia = media;
}
//---------------------------------------------------------------------------
ttstr tTVPStorageMediaManager::ExtractMediaName(const ttstr &name)
{
	// extract media name from normalized storage named "name".
	// returned media name does not contain colon.

	const tjs_char * p = name.c_str();
	const tjs_char * po = p;
	while(*p && *p != TJS_W(':')) p++;
	return ttstr(po, (int)(p - po));
}
//---------------------------------------------------------------------------
bool tTVPStorageMediaManager::CheckExistentStorage(const ttstr & name)
{
	// gateway for CheckExistentStorage
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	return rec->MediaIntf.GetObjectNoAddRef()->CheckExistentStorage(rec->GetDomainAndPath(name));
}
//---------------------------------------------------------------------------
tTJSBinaryStream * tTVPStorageMediaManager::Open(const ttstr & name, tjs_uint32 flags)
{
	// gateway for Open
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	return rec->MediaIntf.GetObjectNoAddRef()->Open(rec->GetDomainAndPath(name), flags);
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::GetListAt(const ttstr &name, iTVPStorageLister * lister)
{
	// gateway for GetListAt
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	/*return */rec->MediaIntf.GetObjectNoAddRef()->GetListAt(rec->GetDomainAndPath(name), lister);
}
//---------------------------------------------------------------------------
void TVPGetStorageListAt(const ttstr & name, iTVPStorageLister * lister)
{
	if(lister) TVPStorageMediaManager.GetListAt(TVPNormalizeStorageName(name), lister);
}
//---------------------------------------------------------------------------
ttstr tTVPStorageMediaManager::GetLocallyAccessibleName(const ttstr &name)
{
	// gateway for GetLocallyAccessibleName
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	ttstr dname = rec->GetDomainAndPath(name);
	rec->MediaIntf.GetObjectNoAddRef()->GetLocallyAccessibleName(dname);
	return dname;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TVPRegisterStorageMedia(iTVPStorageMedia *media)
{
	TVPStorageMediaManager.Register(media);
}
//---------------------------------------------------------------------------
void TVPUnregisterStorageMedia(iTVPStorageMedia *media)
{
	TVPStorageMediaManager.Unregister(media);
}
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// TVPNormalizeStorgeName : storage name normalization
//---------------------------------------------------------------------------
ttstr TVPNormalizeStorageName(const ttstr & _name)
	// TODO: check what is done in TVPNormalizeStorageName
{
	return TVPStorageMediaManager.NormalizeStorageName(_name);
}
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// TVPSetCurrentDirectory
//---------------------------------------------------------------------------
void TVPSetCurrentDirectory(const ttstr & _name)
{
	TVPStorageMediaManager.SetCurrentDirectory(_name);
	TVPClearStorageCaches();
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPGetLocalName and TVPGetLocallyAccessibleName
//---------------------------------------------------------------------------
void TVPGetLocalName(ttstr &name)
{
	ttstr tmp = TVPGetLocallyAccessibleName(name);
	if(tmp.IsEmpty()) TVPThrowExceptionMessage(TVPCannotGetLocalName, name);
	name = tmp;
}
//---------------------------------------------------------------------------
ttstr TVPGetLocallyAccessibleName(const ttstr &name)
{
	if(TJS_strchr(name.c_str(), TVPArchiveDelimiter)) return TJS_W("");
		 // in-archive storage is always not accessible from local file system
	return TVPStorageMediaManager.GetLocallyAccessibleName(name);
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// tTVPArchive
//---------------------------------------------------------------------------
void tTVPArchive::NormalizeInArchiveStorageName(ttstr & name)
{
	// normalization of in-archive storage name does :
	if(name.IsEmpty()) return;

	// make all characters small
	// change '\\' to '/'
	tjs_char *ptr = name.Independ();
	while(*ptr)
	{
		if(*ptr >= TJS_W('A') && *ptr <= TJS_W('Z'))
			*ptr += TJS_W('a') - TJS_W('A');
		else if(*ptr == TJS_W('\\'))
			*ptr = TJS_W('/');
		ptr++;
	}

	// eliminate duplicated slashes
	ptr = name.Independ();
	tjs_char *org_ptr = ptr;
	tjs_char *dest = ptr;
	while(*ptr)
	{
		if(*ptr != TJS_W('/'))
		{
			*dest = *ptr;
			ptr ++;
			dest ++;
		}
		else
		{
			if(ptr != org_ptr)
			{
				*dest = *ptr;
				ptr ++;
				dest ++;
			}
			while(*ptr == TJS_W('/')) ptr++;
		}
	}
	*dest = 0;

	name.FixLen();
}
//---------------------------------------------------------------------------
void tTVPArchive::AddToHash()
{
	// enter all names to the hash table
	tjs_uint Count = GetCount();
	tjs_uint i;
	for(i = 0; i < Count; i++)
	{
		ttstr name = GetName(i);
		NormalizeInArchiveStorageName(name);
		Hash.Add(name, i);
	}
}
//---------------------------------------------------------------------------
tTJSBinaryStream * tTVPArchive::CreateStream(const ttstr & name)
{
	if(name.IsEmpty()) return NULL;

	if(!Init)
	{
		Init = true;
		AddToHash();
	}

	tjs_uint *p = Hash.Find(name);
	if(!p) TVPThrowExceptionMessage(TVPStorageInArchiveNotFound,
		name, ArchiveName);

	return CreateStreamByIndex(*p);
}
//---------------------------------------------------------------------------
bool tTVPArchive::IsExistent(const ttstr & name)
{
	if(name.IsEmpty()) return false;

	if(!Init)
	{
		Init = true;
		AddToHash();
	}

	return Hash.Find(name) != NULL;
}
//---------------------------------------------------------------------------
tjs_int tTVPArchive::GetFirstIndexStartsWith(const ttstr & prefix)
{
	// returns first index which have 'prefix' at start of the name.
	// returns -1 if the target is not found.
	// the item must be sorted by ttstr::operator < , otherwise this function
	// will not work propertly.
	tjs_uint total_count = GetCount();
	tjs_int s = 0, e = total_count;
	while(e - s > 1)
	{
		tjs_int m = (e + s) / 2;
		if(!(GetName(m) < prefix))
		{
			// m is after or at the target
			e = m;
		}
		else
		{
			// m is before the target
			s = m;
		}
	}

	// at this point, s or s+1 should point the target.
	// be certain.
	if(s >= (tjs_int)total_count) return -1; // out of the index
	if(GetName(s).StartsWith(prefix)) return s;
	s++;
	if(s >= (tjs_int)total_count) return -1; // out of the index
	if(GetName(s).StartsWith(prefix)) return s;
	return -1;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// tTVPArchiveCache
//---------------------------------------------------------------------------
class tTVPArchiveCache
{
	typedef tTJSRefHolder<tTVPArchive> tHolder;
	tTJSHashCache<ttstr, tHolder> ArchiveCache;
	tTJSCriticalSection CS;


public:
	tTVPArchiveCache() : ArchiveCache(TVP_DEFAULT_ARCHIVE_CACHE_NUM)
	{
	}

	~tTVPArchiveCache()
	{
	}

	void SetMaxCount(tjs_int maxcount)
	{
		ArchiveCache.SetMaxCount(maxcount);
	}

	void Clear()
	{
		// releases all elements
		ArchiveCache.Clear();
	}

	tTVPArchive * Get(ttstr name)
	{
		name = TVPNormalizeStorageName(name);
		tTJSCSH csh(CS);
		tjs_uint32 hash = tTJSHashCache<ttstr, tHolder>::MakeHash(name);
		tHolder *ptr = ArchiveCache.FindAndTouchWithHash(name, hash);
		if(ptr)
		{
			// exist in the cache
			return ptr->GetObject();
		}

		if(!TVPIsExistentStorageNoSearch(name))
		{
			// storage not found
			TVPThrowExceptionMessage(TVPCannotFindStorage, name);
		}

		// not exist in the cache
		tTVPArchive *arc = TVPOpenArchive(name);
		tHolder holder(arc);
		ArchiveCache.AddWithHash(name, hash, holder);
		return arc;
	}

private:

} TVPArchiveCache;
static void TVPClearArchiveCache() { TVPArchiveCache.Clear(); }
static tTVPAtExit TVPClearArchiveCacheAtExit
	(TVP_ATEXIT_PRI_SHUTDOWN, TVPClearArchiveCache);
// KRKR-ns: exported for the "end game session -> back to launcher" path, which
// must not keep the finished game's archives open.
void krkrsdl2_clear_archive_cache() { TVPClearArchiveCache(); }
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// TVPIsExistentStorageNoSearch
//---------------------------------------------------------------------------
bool TVPIsExistentStorageNoSearchNoNormalize(const ttstr &name)
{
	// does name contain > ?
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	const tjs_char * sharp_pos = TJS_strchr(name.c_str(), TVPArchiveDelimiter);
	if(sharp_pos)
	{
		// this storagename indicates a file in an archive

		ttstr arcname(name, (int)(sharp_pos - name.c_str()));

		tTVPArchive *arc;
		arc = TVPArchiveCache.Get(arcname);
		bool ret;
		try
		{
			ttstr in_arc_name(sharp_pos + 1);
			tTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
			ret = arc->IsExistent(in_arc_name);
		}
		catch(...)
		{
			arc->Release();
			throw;
		}
		arc->Release();
		return ret;
	}

	return TVPStorageMediaManager.CheckExistentStorage(name);
}
//---------------------------------------------------------------------------
bool TVPIsExistentStorageNoSearch(const ttstr &_name)
{
	return TVPIsExistentStorageNoSearchNoNormalize(TVPNormalizeStorageName(_name));
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPExtractStorageExt
//---------------------------------------------------------------------------
ttstr TVPExtractStorageExt(const ttstr & name)
{
	// extract an extension from name.
	// returned string will contain extension delimiter ( '.' ), except for
	// missing extension of the input string.
	// ( returns null string when input string does not have an extension )

	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;
		if(*p == TJS_W('.'))
		{
			// found extension delimiter
			tjs_int extlen = (tjs_int)(slen - ( p - s ));
			return ttstr(p, extlen);
		}

		p--;
	}

	// not found
	return ttstr();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPExtractStorageName
//---------------------------------------------------------------------------
ttstr TVPExtractStorageName(const ttstr & name)
{
	// extract "name"'s storage name ( excluding path ) and return it.
	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;

		p--;
	}

	p++;
	if(p == s)
		return name;
	else
		return ttstr(p, (int)(slen - (p -s)));
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPExtractStoragePath
//---------------------------------------------------------------------------
ttstr TVPExtractStoragePath(const ttstr & name)
{
	// extract "name"'s path ( including last delimiter ) and return it.
	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;

		p--;
	}

	p++;
	return ttstr(s, (int)(p-s));
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPChopStorageExt
//---------------------------------------------------------------------------
extern ttstr TVPChopStorageExt(const ttstr & name)
{
	// chop storage's extension and return it.
	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;
		if(*p == TJS_W('.'))
		{
			// found extension delimiter
			return ttstr(s, (int)(p-s));
		}

		p--;
	}

	// not found
	return name;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// Auto search path support
//---------------------------------------------------------------------------
struct tTVPFileInfo
{
	static const tjs_int EXIST_PROP = 0x01;
	static const tjs_int EMPTY_FILE = 0x02;

	ttstr FilePath;
	iTJSDispatch2* Property = nullptr;
	tjs_int Flag = 0;

	tTVPFileInfo( const ttstr& path, tjs_int exist )
		: FilePath( path ), Flag( exist ) {
	}
	tTVPFileInfo( const ttstr& path, iTJSDispatch2* prop = nullptr )
	: FilePath(path), Property(prop)
	{
		if( Property ) Property->AddRef();
	}
	tTVPFileInfo( const tTVPFileInfo& info )
	: FilePath(info.FilePath), Property(info.Property), Flag(info.Flag )
	{
		if( Property ) Property->AddRef();
	}
	~tTVPFileInfo()
	{
		if( Property ) Property->Release();
	}
	tTVPFileInfo &operator=(const tTVPFileInfo &rhs)
	{
		if (this != &rhs) {
			if( Property ) Property->Release();
			FilePath = rhs.FilePath;
			Property = rhs.Property;
			Flag = rhs.Flag;
			if( Property )
			{
				Property->AddRef();
			}
		}
		return *this;
	}
	bool ExistProp() const {
		return (Flag & EXIST_PROP) != 0; 
	}
	bool ExistFile() const {
		return ( Flag & EMPTY_FILE ) == 0;
	}
};
#define TVP_AUTO_PATH_HASH_SIZE 1024
std::vector<ttstr> TVPAutoPathList;
tTJSHashCache<ttstr, ttstr> TVPAutoPathCache(TVP_DEFAULT_AUTOPATH_CACHE_NUM);
tTJSHashTable<ttstr, tTVPFileInfo, tTJSHashFunc<ttstr>, TVP_AUTO_PATH_HASH_SIZE>
	TVPAutoPathTable;
bool AutoPathTableInit = false;
// KRKR-ns: how many entries of TVPAutoPathList are already indexed in
// TVPAutoPathTable.  Adding an auto path invalidates only the lookup cache;
// the table is kept and the new entries are appended incrementally on the next
// rebuild.  KAG mounts one archive at a time during startup, and the previous
// full-table rebuild (sec: device log "Rebuilding Auto Path Table ... 220-550ms"
// repeated dozens of times) made the startup pause for many seconds.
static size_t AutoPathBuiltCount = 0;
// KRKR-ns diagnostic: how many unresolved-name reports to emit for the current
// session.  0 outside a probe, so normal runs pay nothing.  Sized to outlast
// boot: the earlier limit filled up entirely on optional files the game probes
// for (k2compat.xp3, patch_appendN.xp3, plugin DLLs), which are all absent by
// design and hid the real failure that happens later in the session.
static int AutoPathProbeFailuresLeft = 0;
// Every unexpected miss in the session, regardless of the logging sample size.
int AutoPathMissTotal = 0;
void krkrsdl2_arm_miss_probe()
{
	AutoPathProbeFailuresLeft = 50; // log sample
	AutoPathMissTotal = 0;          // full-session counter
}
int krkrsdl2_take_miss_total()
{
	const int total = AutoPathMissTotal;
	KRKRNS_LOG("[miss] session total (unexpected): %d", total);
	return total;
}

// KAG's Layer.loadImages probes sibling variants of a name ("3_p.png",
// "3_m.tlg", ...).  Those misses are by design and appear in normal sessions
// too, so they are counted but not logged.
static bool krkrns_variant_suffix_probe(const ttstr &name)
{
	ttstr base = TVPExtractStorageName(name);
	base.ToLowerCase();
	// Strip the extension, then look for a "_p"/"_m"/"_s" style variant tail.
	ttstr stem = TVPChopStorageExt(base);
	const tjs_char *s = stem.c_str();
	const size_t n = stem.GetLen();
	if (n < 3) return false;
	const tjs_char c = s[n - 2];
	if (c != TJS_W('_')) return false;
	const tjs_char t = s[n - 1];
	return t == TJS_W('p') || t == TJS_W('m') || t == TJS_W('s') || t == TJS_W('e');
}

// Names that are EXPECTED to be absent: KAG probes optional archives and
// per-game plugin DLLs at every boot, on every platform.  Reporting them hid
// the real unresolved resources, so filter them out of the probe.
static bool krkrns_miss_probe_ignored(const ttstr &name)
{
	if (name.IsEmpty()) return true;
	// Only the file name matters; the request may carry a full storage path.
	ttstr base = TVPExtractStorageName(name);
	base.ToLowerCase();
	const tjs_char *b = base.c_str();

	static const tjs_char * const ignored[] = {
		TJS_W("k2compat.xp3"), TJS_W("system.xp3"), TJS_W("sysscn.xp3"),
		TJS_W("others.xp3"), TJS_W("rule.xp3"), TJS_W("sound.xp3"),
		TJS_W("scenario.xp3"), TJS_W("image.xp3"), TJS_W("face.xp3"),
		TJS_W("init.xp3"), TJS_W("font.xp3"), TJS_W("sysse.xp3"),
		TJS_W("thum.xp3"), TJS_W("motion.xp3"), TJS_W("motiondx.xp3"),
		TJS_W("emote.xp3"), TJS_W("emotedx.xp3"), TJS_W("bishamon.xp3"),
		TJS_W("debug.xp3"), TJS_W("sdmotion.xp3"), TJS_W("packinone.dll"),
		TJS_W("fstat.dll"), TJS_W("messenger.dll"), TJS_W("autocycle.txt"),
	};
	for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i)
		if (TJS_strcmp(b, ignored[i]) == 0) return true;
	// patch_append0..9.xp3 / patchN.xp3 style optional patch packs.
	if (TJS_strncmp(b, TJS_W("patch_append"), 12) == 0) return true;
	return false;
}
//---------------------------------------------------------------------------
static void TVPClearAutoPathCache()
{
	TVPAutoPathCache.Clear();
	TVPAutoPathTable.Clear();
	AutoPathTableInit = false;
	AutoPathBuiltCount = 0;
}
// KRKR-ns: drop ONLY the name->placed-path lookup memo, keeping the
// filename->auto-path table and the incremental-rebuild latches.  The table
// is a pure function of TVPAutoPathList, so it may only be wiped when the
// LIST itself changes (add/remove/reset below) -- anything else only needs
// the memo.  The stock TVPClearStorageCaches wiped the table, and every
// WRITE-mode stream open goes through it: the second game's boot, which
// probes the system-save path several times, paid a full 97k-file table
// rebuild (~250ms each on the device) per save attempt.  Residual semantic
// difference: a NEW file created inside an existing auto-path folder is not
// picked up by unqualified lookups until the list next changes; no Switch
// title writes into its auto-path folders (saves live in the data path).
static void TVPClearAutoPathLookupCache()
{
	TVPAutoPathCache.Clear();
}
//---------------------------------------------------------------------------
struct tTVPClearAutoPathCacheCallback : public tTVPCompactEventCallbackIntf
{
	virtual void TJS_INTF_METHOD OnCompact(tjs_int level)
	{
		if(level >= TVP_COMPACT_LEVEL_DEACTIVATE)
		{
			// clear the auto search path cache on application deactivate
			tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
			TVPClearAutoPathCache();
		}
	}
} static TVPClearAutoPathCacheCallback;
static bool TVPClearAutoPathCacheCallbackInit = false;
//---------------------------------------------------------------------------
void TVPAddAutoPath(const ttstr & name)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	tjs_char lastchar = name.GetLastChar();
	if(lastchar != TVPArchiveDelimiter && lastchar != TJS_W('/') && lastchar != TJS_W('\\'))
		TVPThrowExceptionMessage(TVPMissingPathDelimiterAtLast);

	ttstr normalized = TVPNormalizeStorageName(name);

	std::vector<ttstr>::iterator i =
		std::find(TVPAutoPathList.begin(), TVPAutoPathList.end(), normalized);
	if(i == TVPAutoPathList.end())
		TVPAutoPathList.push_back(normalized);

	// KRKR-ns: the table is kept and extended incrementally — only the
	// filename lookup cache must go (the new path may shadow an existing
	// file).  The old TVPClearAutoPathCache() here forced a full rebuild on
	// the next lookup, which KAG startup hit once per mounted archive.
	TVPAutoPathCache.Clear();
}
//---------------------------------------------------------------------------
void TVPRemoveAutoPath(const ttstr &name)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	tjs_char lastchar = name.GetLastChar();
	if(lastchar != TVPArchiveDelimiter && lastchar != TJS_W('/') && lastchar != TJS_W('\\'))
		TVPThrowExceptionMessage(TVPMissingPathDelimiterAtLast);

	ttstr normalized = TVPNormalizeStorageName(name);

	std::vector<ttstr>::iterator i =
		std::find(TVPAutoPathList.begin(), TVPAutoPathList.end(), normalized);
	if(i != TVPAutoPathList.end())
		TVPAutoPathList.erase(i);

	// KRKR-ns: TVPAutoPathTable maps file name -> the path it was found under, so
	// an entry left behind here keeps resolving (and serving!) files from a path
	// that is no longer an auto path.  Removing from the list alone was silently
	// broken: TVPRebuildAutoPathTable's fast path compares AutoPathBuiltCount
	// against the list SIZE, and removing one entry while adding another keeps
	// those equal, so the stale entries survived indefinitely.
	//
	// Observed on the device: after ending a game and launching a second one, the
	// second game rendered the FIRST game's title screen and menus, because every
	// unqualified resource name still resolved into the finished game's archive.
	TVPClearAutoPathCache();
}
//---------------------------------------------------------------------------
/* Enumerate one auto-path entry (an archive>subdir or a real folder) and add
 * its files to TVPAutoPathTable.  Used by both the full rebuild and the
 * incremental append. */
static tjs_uint TVPEnumerateAutoPathEntry(const ttstr & path)
{
	tjs_uint count = 0;

	const tjs_char * sharp_pos = TJS_strchr(path.c_str(), TVPArchiveDelimiter);
	if(sharp_pos)
	{
		// this storagename indicates a file in an archive

		ttstr arcname(path, (int)(sharp_pos - path.c_str()));
		// Keep only "archive>" as the base.  An auto path may itself
		// target an in-archive directory (for example "uipsd.xp3>ini/");
		// appending the entry's full directory to that path would resolve
		// quickmenu.ini as ini/ini/quickmenu.ini.
		ttstr archive_root(path,
			(int)(sharp_pos - path.c_str()) + 1);
		ttstr in_arc_name(sharp_pos + 1);
		tTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
		tjs_int in_arc_name_len = in_arc_name.GetLen();

		tTVPArchive *arc;
		arc = TVPArchiveCache.Get(arcname);

		try
		{
			tjs_uint storagecount = arc->GetCount();

			// get first index which the item has 'in_arc_name' as its start
			// of the string.
			tjs_int i = arc->GetFirstIndexStartsWith(in_arc_name);
			if(i != -1)
			{
				for(; i < (tjs_int)storagecount; i++)
				{
					ttstr name = arc->GetName(i);

					if(name.StartsWith(in_arc_name))
					{
						// KRKR-ns patch: index files in subdirectories too
						// (KAG games keep system/..., scenario/..., etc.)
						// — upstream only indexed root files, which broke
						// "Cannot find storage system/Initialize.tjs".
						// The file path records the entry's absolute
						// in-archive directory, relative to archive_root.
						ttstr sname = TVPExtractStorageName(name);
						sname.ToLowerCase();
						// TODO アーカイブの時もプロパティ情報追加
						TVPAutoPathTable.Add(sname,
							tTVPFileInfo(archive_root + TVPExtractStoragePath(name)));
						count ++;
					}
					else
					{
						// no need to check more;
						// because the list is sorted by the name.
						break;
					}
				}
			}
		}
		catch(...)
		{
			arc->Release();
			throw;
		}
		arc->Release();
	}
	else
	{
		// normal folder
		class tLister : public iTVPStorageLister
		{
			const ttstr EXT;
		public:
			tLister() : EXT(TJS_W(".prop")) {}
			std::set<ttstr>		list;
			std::vector<ttstr>	prop;
			void TJS_INTF_METHOD Add(const ttstr &file)
			{
				ttstr ext = TVPExtractStorageExt( file );
				if( ext == EXT )
				{
					prop.push_back( file );
				}
				list.insert( file );
			}
		} lister;
		TVPStorageMediaManager.GetListAt(path, &lister);

		if( !TVPIgnoreFileProperty )
		{
			// プロパティがあるファイルを追加する
			for( auto i = lister.prop.begin(); i != lister.prop.end(); i++ ) {
				// プロパティがある場合はとりあえず登録だけしておき、プロパティ取得時に実際に読み込みを行う
				ttstr fname = TVPChopStorageExt( *i );
				auto file = lister.list.find( fname );
				if( file != lister.list.end() ) {
					ttstr sname = *file;
					sname.ToLowerCase();
					// ファイルがある場合
					lister.list.erase( sname );
					TVPAutoPathTable.Add( sname, tTVPFileInfo( path, tTVPFileInfo::EXIST_PROP ) );
				} else {
					ttstr sname = fname;
					sname.ToLowerCase();
					TVPAutoPathTable.Add( sname, tTVPFileInfo( path, tTVPFileInfo::EXIST_PROP | tTVPFileInfo::EMPTY_FILE ) );
				}
			}
		}
		// プロパティのないファイルを追加する
		for( auto i = lister.list.begin(); i != lister.list.end(); i++)
		{
			ttstr sname = *i;
			sname.ToLowerCase();
			TVPAutoPathTable.Add(sname, tTVPFileInfo(path) );
			count ++;
		}
	}

	return count;
}
//---------------------------------------------------------------------------
static tjs_uint TVPRebuildAutoPathTable()
{
	// KRKR-ns: incremental — when the table is already built, only the auto
	// paths added since the last build are enumerated and appended.  KAG
	// mounts archives one at a time, and the old version wiped the table on
	// every TVPAddAutoPath, so each mounted archive forced a full re-enumeration
	// of every archive on the next lookup ("Rebuilding Auto Path Table ..."
	// repeated dozens of times, 220-550ms each, on the device).
	if(AutoPathTableInit && AutoPathBuiltCount >= TVPAutoPathList.size())
		return 0; // nothing new since the last build

	TVPInitStorageOptions();

	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	tjs_uint64 tick = TVPGetTickCount();
	TVPAddLog( (const tjs_char*)TVPInfoRebuildingAutoPath );

	tjs_uint totalcount = 0;
	if(AutoPathTableInit)
	{
		const size_t total = TVPAutoPathList.size();
		for(size_t i = AutoPathBuiltCount; i < total; i++)
			totalcount += TVPEnumerateAutoPathEntry(TVPAutoPathList[i]);
		AutoPathBuiltCount = total;
	}
	else
	{
		TVPAutoPathTable.Clear();
		for(size_t i = 0; i < TVPAutoPathList.size(); i++)
			totalcount += TVPEnumerateAutoPathEntry(TVPAutoPathList[i]);
		AutoPathBuiltCount = TVPAutoPathList.size();
		AutoPathTableInit = true;
	}

	tjs_uint64 endtick = TVPGetTickCount();

	TVPAddLog(ttstr(TJS_W("(info) Total ")) +
			ttstr((tjs_int)totalcount) + TJS_W(" file(s) found, ") +
			ttstr((tjs_int)TVPAutoPathTable.GetCount()) + TJS_W(" file(s) activated.") + 
			TJS_W(" (") + ttstr((tjs_int)(endtick - tick)) + TJS_W("ms)"));

	return totalcount;
}
//---------------------------------------------------------------------------
// KRKR-ns diagnostic (cold path only): the auto-path table maps every file name
// to the path it was found under, so a stale entry silently serves assets from a
// finished game.  A session teardown logs this before and after dropping the
// game's paths; `table` must fall back to just the compat/patch entries.
// UTF-8 view of a storage path for the SD log (ttstr is UTF-16, and game
// directory names are non-ASCII, so AsNarrowStdString() is not usable here).
static std::string krkrns_utf8_of_path(const ttstr &s)
{
	std::string out;
	for (tjs_uint i = 0; i < s.GetLen() && i < 200; ++i)
	{
		tjs_uint32 ch = static_cast<tjs_uint32>(s[i]);
		if (ch < 0x80) out += static_cast<char>(ch);
		else if (ch < 0x800)
		{
			out += static_cast<char>(0xC0 | (ch >> 6));
			out += static_cast<char>(0x80 | (ch & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xE0 | (ch >> 12));
			out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (ch & 0x3F));
		}
	}
	return out;
}

// Caller holds TVPCreateStreamCS.
static tjs_int TVPRemoveAutoPathsUnderLocked(const ttstr &prefix)
{
	if (prefix.IsEmpty()) return 0;

	tjs_int removed = 0;
	for (size_t i = TVPAutoPathList.size(); i-- > 0;)
	{
		if (TVPAutoPathList[i].StartsWith(prefix))
		{
			TVPAutoPathList.erase(TVPAutoPathList.begin() + (ptrdiff_t)i);
			++removed;
		}
	}
	return removed;
}

tjs_int TVPRemoveAutoPathsUnder(const ttstr &prefix)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	const tjs_int removed = TVPRemoveAutoPathsUnderLocked(prefix);
	// The name->path table still maps bare names onto this game's archives;
	// dropping the list entries without invalidating it would leave those
	// mappings live.
	if (removed > 0) TVPClearAutoPathCache();
	return removed;
}
//---------------------------------------------------------------------------

void krkrsdl2_log_autopath_state(const char *when)
{
	KRKRNS_LOG("[autopath] %s: list=%d table=%d init=%d built=%d",
		when,
		(int)TVPAutoPathList.size(),
		(int)TVPAutoPathTable.GetCount(),
		(int)AutoPathTableInit,
		(int)AutoPathBuiltCount);

	// Counts alone cannot distinguish "the previous game's archive is still
	// listed" from "it is gone": list=140 after removing 11 entries looks the
	// same whether the removed ones were the game's or something else.  Dump the
	// entries that actually resolve files -- the archives -- so a cross-session
	// leak is visible directly instead of inferred.
	int shown = 0;
	for (size_t i = 0; i < TVPAutoPathList.size() && shown < 24; ++i)
	{
		const ttstr &p = TVPAutoPathList[i];
		const tjs_char *sharp = TJS_strchr(p.c_str(), TVPArchiveDelimiter);
		if (!sharp) continue; // plain folders are the permanent compat/patch ones
		KRKRNS_LOG("[autopath]   archive[%d] = %s", (int)i,
			krkrns_utf8_of_path(p).c_str());
		++shown;
	}
	if (shown == 0) KRKRNS_LOG("[autopath]   (no archive entries)");
}

// KRKR-ns diagnostic: prove how a prefix compares against the entries that are
// supposed to match it.  StartsWith() semantics (literal vs case-folded vs
// character-set) are not worth guessing about when the evidence is one line.
void krkrsdl2_probe_autopath_prefix(const ttstr &prefix, const char *when)
{
	KRKRNS_LOG("[autopath] prefix probe (%s): prefix=\"%s\" len=%d",
		when, krkrns_utf8_of_path(prefix).c_str(), (int)prefix.GetLen());
	int shown = 0;
	for (size_t i = 0; i < TVPAutoPathList.size() && shown < 6; ++i)
	{
		const ttstr &p = TVPAutoPathList[i];
		if (TJS_strchr(p.c_str(), TVPArchiveDelimiter) == nullptr) continue;
		KRKRNS_LOG("[autopath]   [%d] startsWith=%d  entry=\"%s\"", (int)i,
			(int)p.StartsWith(prefix), krkrns_utf8_of_path(p).c_str());
		++shown;
	}
}

// KRKR-ns: reset the auto-path subsystem to its pristine boot state.  Called by
// krkrsdl2_reinitialize_engine (SDLApplication.cpp).
//
// The engine rebuild tears down the script engine and window system, but the
// storage layer's process-global state survives it: the finished game's
// archive entries stayed in TVPAutoPathList, so every later mount started from
// a polluted list (observed growth across three emulator sessions: 12 -> 69 ->
// 80 entries, including duplicate `file://?/` and `file://` forms the game
// itself added).  Wipe the whole list, the name->path table and the resolution
// cache, then re-seed the single path boot itself seeds -- exactly the state
// krkrsdl2_init_platform_once leaves behind.  compat/patch paths do not need
// re-seeding here: every game mount removes and re-adds them last.
void krkrsdl2_reset_auto_paths()
{
	{
		tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
		TVPAutoPathList.clear();
		// Also drops the name->path table and its init/built latches, so the
		// next lookup rebuilds from the fresh list only.
		TVPClearAutoPathCache();
	}
	// Outside the lock: TVPAddAutoPath takes the same critical section.
	TVPAddAutoPath(ttstr(TJS_W("file://?/romfs:/")));
	KRKRNS_LOG("[autopath] reset to boot state (list=%d)", (int)TVPAutoPathList.size());
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPGetPlacedPath
//---------------------------------------------------------------------------
ttstr TVPGetPlacedPath(const ttstr & name)
{
	// search path and return the path which the "name" is placed.
	// returned name is normalized. returns empty string if the storage is not
	// found.
	if(!TVPClearAutoPathCacheCallbackInit)
	{
		TVPAddCompactEventHook(&TVPClearAutoPathCacheCallback);
		TVPClearAutoPathCacheCallbackInit = true;
	}

	ttstr * incache = TVPAutoPathCache.FindAndTouch(name);
	if(incache) return *incache; // found in cache

	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	ttstr normalized(TVPNormalizeStorageName(name));

	bool found = TVPIsExistentStorageNoSearchNoNormalize(normalized);
	if(found)
	{
		// found in current folder
		TVPAutoPathCache.Add(name, normalized);
		return normalized;
	}

	// not found in current folder
	// search through auto path table

	ttstr storagename = TVPExtractStorageName(normalized);
	storagename.ToLowerCase();

	TVPRebuildAutoPathTable(); // ensure auto path table
	tTVPFileInfo *result = TVPAutoPathTable.Find(storagename);
	if(result && (result->Flag & tTVPFileInfo::EMPTY_FILE) == 0 )
	{
		// found in table
		ttstr found = result->FilePath + storagename;
		TVPAutoPathCache.Add(name, found);
		return found;
	}

#ifdef __SWITCH__
	// KRKR-ns: games gate their subsystems (E-mote, text render, UI effects)
	// on file-existence probes of plugin names before calling Plugins.link.
	// Native builds answer those probes with real DLL files; our statically
	// linked plugins have none, so the game disables features that actually
	// work -- LimeLight skipped motion.tjs and then routed E-mote motion
	// files (.mtn) through Layer.loadImages, breaking the OP and title art.
	// Report built-in plugin names as existing, pointing at a harmless real
	// file: Plugins.link resolves built-ins natively and never reads it.
	{
		const std::string lname8 = krkrns_utf8_of_path(storagename);
		if (lname8.size() > 4 && lname8.compare(lname8.size() - 4, 4, ".dll") == 0 &&
			krkrsdl2_is_builtin_plugin_name(storagename))
		{
			const ttstr dummy(TJS_W("file://?/romfs:/compat/system/dummy_colorpicker.png"));
			TVPAutoPathCache.Add(name, dummy);
			return dummy;
		}
	}
#endif

#ifdef __SWITCH__
	// KRKR-ns diagnostic: an unqualified name that resolves nowhere is the
	// mechanism behind "missing images / videos after switching games".  Logging
	// the NAME (and the table state) separates "the resource is not in the game
	// at all" from "it is there but the search table lost it", which the caller's
	// generic failure message cannot show.
	//
	// The previous version stopped at a fixed budget, so BOTH sessions hit the
	// cap and their counts could not be compared at all.  Now every miss is
	// COUNTED for the whole session, while only a sample is logged.
	if (!krkrns_miss_probe_ignored(name))
	{
		++AutoPathMissTotal;
		// KAG's own variant search ("3_p.png", "3_m.tlg") is expected to miss;
		// it is Layer.loadImages trying suffixes, not a broken resource.
		if (AutoPathProbeFailuresLeft > 0 && !krkrns_variant_suffix_probe(name))
		{
			--AutoPathProbeFailuresLeft;
			KRKRNS_LOG("[miss] unresolved: %s  (table=%d init=%d list=%d)",
				krkrns_utf8_of_path(name).c_str(),
				(int)TVPAutoPathTable.GetCount(), (int)AutoPathTableInit,
				(int)TVPAutoPathList.size());
		}
	}
#endif

	// not found
	TVPAutoPathCache.Add(name, ttstr());
	return ttstr();
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
/**
 * TVPGetPlacedPath
 * @param name file name
 * @param extlist extension list(delimiter |) ex. ".bmp|.png|.jpg"
 * @return normalized path.
 */
ttstr TVPGetPlacedPath(const ttstr & name, const ttstr& extlist )
{
	tjs_string exts = extlist.AsStdString();
	std::vector<tjs_string> ext;
	split( exts, tjs_string( TJS_W( "|" ) ), ext );
	ttstr filename = TVPChopStorageExt( name );
	for( auto i = ext.begin(); i != ext.end(); i++ ) {
		if( !((*i).empty()) ) {
			ttstr fullname = filename + ttstr( *i );
			ttstr ret = TVPGetPlacedPath( fullname );
			if( !ret.IsEmpty() ) {
				return ret;
			}
		}
	}
	return ttstr();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPSearchPlacedPath
//---------------------------------------------------------------------------
ttstr TVPSearchPlacedPath(const ttstr & name)
{
	ttstr place = TVPGetPlacedPath(name);
	if(place.IsEmpty()) TVPThrowExceptionMessage(TVPCannotFindStorage, name);
	return place;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPIsExistentStorage
//---------------------------------------------------------------------------
bool TVPIsExistentStorage(const ttstr &name)
{
	return !TVPGetPlacedPath(name).IsEmpty();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPGetFilePropertyNoAddRef
//---------------------------------------------------------------------------
iTJSDispatch2* TVPGetFilePropertyNoAddRef( const ttstr& name )
{
	TVPRebuildAutoPathTable(); // ensure auto path table
	tTVPFileInfo *result = TVPAutoPathTable.Find( name );
	if( result && ( result->Flag & tTVPFileInfo::EXIST_PROP) ) {
		// found in table
		if( !result->Property ) {
			ttstr path = result->FilePath + name + ".prop";
			tTJSVariant dic;
			ttstr mode;
			if( TJSReadDictionaryObject( dic, path, mode ) == TJS_S_OK ) {
				result->Property = dic.AsObject();
			}
		}
		return result->Property;
	}
	return nullptr;
}


//---------------------------------------------------------------------------
// TVPCreateStream
//---------------------------------------------------------------------------
static tTJSBinaryStream * _TVPCreateStream(const ttstr & _name, tjs_uint32 flags)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	ttstr name;

	tjs_uint32 access = flags & TJS_BS_ACCESS_MASK;
	if(access == TJS_BS_WRITE)
		name = TVPNormalizeStorageName(_name);
	else
		name = TVPGetPlacedPath(_name); // file must exist

	if(name.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, _name);

	// does name contain > ?
	const tjs_char * sharp_pos = TJS_strchr(name.c_str(), TVPArchiveDelimiter);
	if(sharp_pos)
	{
		// this storagename indicates a file in an archive
		if((flags & TJS_BS_ACCESS_MASK ) !=TJS_BS_READ)
			TVPThrowExceptionMessage(TVPCannotWriteToArchive);

		ttstr arcname(name, (int)(sharp_pos - name.c_str()));

		tTVPArchive *arc;
		tTJSBinaryStream *stream;
		arc = TVPArchiveCache.Get(arcname);
		try
		{
			ttstr in_arc_name(sharp_pos + 1);
			tTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
			stream = arc->CreateStream(in_arc_name);
		}
		catch(...)
		{
			arc->Release();
			if(access >= 1) TVPClearStorageCaches();
			throw;
		}
		if(access >= 1) TVPClearStorageCaches();
		arc->Release();
		return stream;
	}

	tTJSBinaryStream *stream;
	try
	{
		stream = TVPStorageMediaManager.Open(name, flags);
	}
	catch(...)
	{
		if(access >= 1) TVPClearStorageCaches();
		throw;
	}
	if(access >= 1) TVPClearStorageCaches();
	return stream;
}

tTJSBinaryStream * TVPCreateStream(const ttstr & _name, tjs_uint32 flags)
{
#ifdef __SWITCH__
	// KRKR-ns diagnostic: every storage open is logged before execution and
	// reported again after success/failure, so a hang shows the exact file
	// whose open or first access blocked. ttstr is UTF-16; convert to UTF-8.
	std::string utf8;
	{
		ttstr n(_name);
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
	}
	static int openLogs = 0;
	const bool logThis = openLogs < 400;
	if (logThis) { openLogs++; KRKRNS_LOG("[open] %s", utf8.c_str()); }
	KRKRNS_STAGE(("open " + utf8).c_str());
	try
	{
		tTJSBinaryStream * stream = _TVPCreateStream(_name, flags);
		if (logThis) KRKRNS_LOG("[opened] %s", utf8.c_str());
		KRKRNS_STAGE("opened");
		return stream;
	}
	catch (...)
	{
		KRKRNS_LOG("[open-failed] %s", utf8.c_str());
		KRKRNS_STAGE("open failed");
		throw;
	}
#else
	try
	{
		return _TVPCreateStream(_name, flags);
	}
	catch(eTJSScriptException &e)
	{
		if(TJS_strchr(_name.c_str(), '#'))
			e.AppendMessage(TJS_W("[") +
				TVPFormatMessage(TVPFilenameContainsSharpWarn, _name) + TJS_W("]"));
		throw e;
	}
	catch(eTJSScriptError &e)
	{
		if(TJS_strchr(_name.c_str(), '#'))
			e.AppendMessage(TJS_W("[") +
				TVPFormatMessage(TVPFilenameContainsSharpWarn, _name) + TJS_W("]"));
		throw e;
	}
	catch(eTJSError &e)
	{
		if(TJS_strchr(_name.c_str(), '#'))
			e.AppendMessage(TJS_W("[") +
				TVPFormatMessage(TVPFilenameContainsSharpWarn, _name) + TJS_W("]"));
		throw e;
	}
	catch(...)
	{
		// check whether the filename contains '#' (former delimiter for archive
		// filename before 2.19 beta 14)
		if(TJS_strchr(_name.c_str(), '#'))
			TVPAddLog(TVPFormatMessage(TVPFilenameContainsSharpWarn, _name));
		throw;
	}
#endif
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPClearStorageCaches
//---------------------------------------------------------------------------
void TVPClearStorageCaches()
{
	// clear all storage related caches.  The auto-path TABLE survives: this
	// runs on every write-mode stream open and every directory change, and the
	// table depends only on the auto-path list, which neither touches.  Wiping
	// it here (stock behavior) forced a full re-enumeration of every mounted
	// archive on the next lookup -- 97k files / ~250ms on the device, once per
	// system-save probe during a game's boot.
	TVPClearXP3SegmentCache();
	TVPClearAutoPathLookupCache();
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNC_Storages
//---------------------------------------------------------------------------
// Storages.fstat / getTime / deleteFile / copyFile / dirlist / dirlistEx
//
// Ported from krkrsdl3's plugins/fstat.cpp, the ncbind plugin kirikiri exposes
// as fstat.dll.  A KAG BASE ADV SYSTEM title (v1_KR_Xmoe_晴菜花) leans on it in
// normal play: fstat while recording a jump point on every finished line, and
// deleteFile / copyFile / dirlistEx from its save screen.  Every missing member
// threw inside the game's own event handler ("Member ... does not exist") and
// left the game where it stood, so the set is provided in one go.
//
// Two rules of this port's storage layer shape the implementation:
//   * its file media keeps local names in the device form ("sdmc:/...") and
//     resolves them itself, so names are handed to the engine
//     (TVPGetStorageListAt, TVPCreateBinaryStreamForRead/Write) instead of
//     being converted to paths and opened behind its back;
//   * stat() is unusable here -- the media's own __SWITCH__ note records that
//     the emulator's fsdev hangs on it (its directory walk relies on d_type
//     for the same reason).  Nothing below calls stat: sizes come from an
//     opened stream, and the timestamp fields carry a valid but empty Date
//     (epoch 0).  Titles keep working -- mtime is read back with getTime(),
//     which a Date always answers -- and a directory that does not exist lists
//     as empty instead of throwing.
//
// Faithful to the reference otherwise: fstat reports the placed file's stream
// size (an archive member's uncompressed size) and omits it for a directory;
// dirlist yields names, dirlistEx yields %[name, size, attrib, mtime, atime,
// ctime] with attrib 0 for the regular files the listing yields; deleteFile
// removes only names with a local form and clears the storage caches on
// success; copyFile copies through the engine's streams, which also creates a
// missing destination folder.  The rest of the plugin (setTime, exportFile,
// truncateFile, moveFile, dirtree, md5, temporary files, file selector) stays
// out of this port.
//
// A Date for a POSIX timestamp, or an empty variant when there is none.  The
// Date class is looked up per call rather than cached: an in-process engine
// restart replaces the whole global object set, and a cached dispatcher would
// survive it as a dangling pointer.
static void TVPStoragesStoreDate(tTJSVariant & store, tjs_int64 seconds)
{
	if(seconds < 0) return;
	iTJSDispatch2 * global = TVPGetScriptDispatch();
	if(!global) return;
	tTJSVariant cls;
	if(TJS_FAILED(global->PropGet(0, TJS_W("Date"), nullptr, &cls, global))) return;
	iTJSDispatch2 * datecls = cls.AsObjectNoAddRef();
	if(!datecls) return;
	iTJSDispatch2 * obj = nullptr;
	if(TJS_FAILED(datecls->CreateNew(0, nullptr, nullptr, &obj, 0, nullptr, datecls)) || !obj) return;
	tTJSVariant setter_v;
	iTJSDispatch2 * setter = nullptr;
	if(TJS_SUCCEEDED(datecls->PropGet(0, TJS_W("setTime"), nullptr, &setter_v, datecls)))
		setter = setter_v.AsObjectNoAddRef();
	if(setter)
	{
		tTJSVariant ms((tjs_int64)seconds * 1000);
		tTJSVariant * param[] = { &ms };
		setter->FuncCall(0, nullptr, nullptr, nullptr, 1, param, obj);
	}
	store = tTJSVariant(obj, obj);
	obj->Release();
}

// Resolve a storage name the way the engine's own file paths do: the auto-path
// table first, then the file media -- which in game mode resolves a bare name
// ("savedata019.bmp", the shape KAG hands to these members) under the game
// directory.  Those bare names are not in the auto-path table, so asking the
// table alone silently resolves to nothing.
static ttstr TVPStoragesPlacedName(const ttstr & name)
{
	ttstr placed = TVPGetPlacedPath(name);
	if(placed.IsEmpty()) placed = TVPNormalizeStorageName(name);
	return placed;
}

static iTJSDispatch2 * TVPStoragesFstatDict(const ttstr & name, bool want_size)
{
	iTJSDispatch2 * dict = TJSCreateDictionaryObject();
	if(!dict) return nullptr;

	ttstr placed = TVPStoragesPlacedName(name);
	if(!placed.IsEmpty() && want_size)
	{
		tTJSBinaryStream * in = nullptr;
		try
		{
			in = TVPCreateBinaryStreamForRead(placed, TJS_W(""));
		}
		catch(...)
		{
			in = nullptr;
		}
		if(in)
		{
			tTJSVariant size((tjs_int64)in->GetSize());
			dict->PropSet(TJS_MEMBERENSURE, TJS_W("size"), nullptr, &size, dict);
			delete in;
		}
	}

	tTJSVariant mtime, ctime, atime;
	TVPStoragesStoreDate(mtime, 0);
	TVPStoragesStoreDate(ctime, 0);
	TVPStoragesStoreDate(atime, 0);
	dict->PropSet(TJS_MEMBERENSURE, TJS_W("mtime"), nullptr, &mtime, dict);
	dict->PropSet(TJS_MEMBERENSURE, TJS_W("ctime"), nullptr, &ctime, dict);
	dict->PropSet(TJS_MEMBERENSURE, TJS_W("atime"), nullptr, &atime, dict);

	return dict;
}

static bool TVPStoragesDeleteFile(const ttstr & file)
{
	ttstr placed = TVPStoragesPlacedName(file);
	// Entry log: a save screen that reports "nothing happened" has to be
	// distinguishable from one that never called this at all.
	KRKRNS_LOG("[fstat] deleteFile('%s') -> '%s'", krkrns_utf8_of_path(file).c_str(),
		krkrns_utf8_of_path(placed).c_str());
	if(placed.IsEmpty()) return false;
	// A name inside an archive has no local form and cannot be deleted.
	try
	{
		TVPGetLocalName(placed);
	}
	catch(...)
	{
		KRKRNS_LOG("[fstat] deleteFile: no local form for %s", krkrns_utf8_of_path(placed).c_str());
		return false;
	}
	tjs_string wide(placed.c_str());
	std::string path8;
	if(!TVPUtf16ToUtf8(path8, wide))
	{
		KRKRNS_LOG("[fstat] deleteFile: cannot convert %s", krkrns_utf8_of_path(placed).c_str());
		return false;
	}
	if(unlink(path8.c_str()) != 0)
	{
		const int err = errno;
		KRKRNS_LOG("[fstat] deleteFile: unlink(%s) failed, errno=%d", path8.c_str(), err);
		TVPAddLog(ttstr(TJS_W("deleteFile : ")) + placed + TJS_W("Failed"));
		return false;
	}
	KRKRNS_LOG("[fstat] deleteFile: removed %s", path8.c_str());
	TVPClearStorageCaches();
	return true;
}

static bool TVPStoragesCopyFile(const ttstr & from, const ttstr & to)
{
	try
	{
		ttstr src = TVPStoragesPlacedName(from);
		KRKRNS_LOG("[fstat] copyFile('%s' -> '%s')", krkrns_utf8_of_path(from).c_str(),
			krkrns_utf8_of_path(to).c_str());
		if(src.IsEmpty()) return false;
		ttstr dst = TVPNormalizeStorageName(to);

		tTJSBinaryStream * in = TVPCreateBinaryStreamForRead(src, TJS_W(""));
		if(!in) return false;
		tTJSBinaryStream * out = nullptr;
		try
		{
			out = TVPCreateBinaryStreamForWrite(dst, TJS_W(""));
		}
		catch(...)
		{
			delete in;
			return false;
		}
		if(!out)
		{
			delete in;
			return false;
		}
		tjs_uint8 buffer[1024 * 16];
		tjs_int32 size;
		while((size = in->Read(buffer, sizeof buffer)) > 0)
		{
			out->Write(buffer, size);
		}
		delete out;
		delete in;
		TVPClearStorageCaches();
		return true;
	}
	catch(...)
	{
		return false;
	}
}

// dirlist / dirlistEx share everything but what they put in the array: names
// versus %[name, size, attrib, times].  Both walk the directory through the
// engine's listing API, which is the only listing route that works on this
// target (see the notes above).
static tTJSVariant TVPStoragesDirList(const ttstr & dir, bool with_info)
{
	ttstr d = TVPNormalizeStorageName(dir);
	if(d.GetLastChar() != TJS_W('/'))
		TVPThrowExceptionMessage(TJS_W("'/' must be specified at the end of given directory name."));

	iTJSDispatch2 * array = TJSCreateArrayObject();
	tTJSVariant result(array, array);
	if(array) array->Release();

	// The lister interface, as the dirlist plugin in this port uses it.
	class tLister : public iTVPStorageLister
	{
	public:
		std::vector<ttstr> names;
		void TJS_INTF_METHOD Add(const ttstr & file) { names.push_back(file); }
	} lister;
	try
	{
		TVPGetStorageListAt(d, &lister);
	}
	catch(...)
	{
		// An unreadable or absent folder answers with an empty array.
		lister.names.clear();
	}
	const std::vector<ttstr> & names = lister.names;

	KRKRNS_LOG("[fstat] dirlist%s('%s') -> %u name(s)", with_info ? "Ex" : "",
		krkrns_utf8_of_path(d).c_str(), (unsigned)names.size());

	tjs_int count = 0;
	for(size_t i = 0; i < names.size(); i++)
	{
		const ttstr & name = names[i];
		if(with_info)
		{
			iTJSDispatch2 * dict = TJSCreateDictionaryObject();
			if(!dict) continue;

			tTJSVariant vname(name);
			dict->PropSet(TJS_MEMBERENSURE, TJS_W("name"), nullptr, &vname, dict);

			tjs_int64 size = 0;
			tTJSBinaryStream * in = nullptr;
			try
			{
				in = TVPCreateBinaryStreamForRead(d + name, TJS_W(""));
			}
			catch(...)
			{
				in = nullptr;
			}
			if(in)
			{
				size = (tjs_int64)in->GetSize();
				delete in;
			}
			tTJSVariant vsize(size);
			dict->PropSet(TJS_MEMBERENSURE, TJS_W("size"), nullptr, &vsize, dict);

			// The listing yields regular files, so no directory bit; without
			// stat there is no writability bit either.
			tTJSVariant vattr((tjs_int32)0);
			dict->PropSet(TJS_MEMBERENSURE, TJS_W("attrib"), nullptr, &vattr, dict);

			tTJSVariant mtime, ctime, atime;
			TVPStoragesStoreDate(mtime, 0);
			TVPStoragesStoreDate(ctime, 0);
			TVPStoragesStoreDate(atime, 0);
			dict->PropSet(TJS_MEMBERENSURE, TJS_W("mtime"), nullptr, &mtime, dict);
			dict->PropSet(TJS_MEMBERENSURE, TJS_W("ctime"), nullptr, &ctime, dict);
			dict->PropSet(TJS_MEMBERENSURE, TJS_W("atime"), nullptr, &atime, dict);

			tTJSVariant entry(dict, dict);
			array->PropSetByNum(0, count, &entry, array);
			dict->Release();
			count++;
		}
		else
		{
			tTJSVariant ventry(name);
			array->PropSetByNum(0, count, &ventry, array);
			count++;
		}
	}

	return result;
}
tjs_uint32 tTJSNC_Storages::ClassID = -1;
tTJSNC_Storages::tTJSNC_Storages() : inherited(TJS_W("Storages"))
{
	// registration of native members

	TJS_BEGIN_NATIVE_MEMBERS(Storages)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/addAutoPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	TVPAddAutoPath(path);

	if(result) result->Clear();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/addAutoPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/removeAutoPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	TVPRemoveAutoPath(path);

	if(result) result->Clear();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/removeAutoPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getFullPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPNormalizeStorageName(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getFullPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getPlacedPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];
	if(result)
	{
		if( numparams >= 2 )
		{
			ttstr ext = *param[1];
			*result = TVPGetPlacedPath( path, ext );
		}
		else
		{
			*result = TVPGetPlacedPath( path );
		}
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getPlacedPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isExistentStorage)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = (tjs_int)TVPIsExistentStorage(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/isExistentStorage)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/extractStorageExt)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPExtractStorageExt(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/extractStorageExt)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/extractStorageName)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPExtractStorageName(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/extractStorageName)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/extractStoragePath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPExtractStoragePath(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/extractStoragePath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/chopStorageExt)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPChopStorageExt(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/chopStorageExt)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearArchiveCache)
{
	TVPClearArchiveCache();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearArchiveCache)
//----------------------------------------------------------------------
#ifdef __SWITCH__
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getGameDirectoryList)
{
	if(result)
	{
		const auto directories = krkrsdl2_list_game_directories();
		iTJSDispatch2 *array = TJSCreateArrayObject();
		for(tjs_int i = 0; i < static_cast<tjs_int>(directories.size()); ++i)
		{
			tTJSVariant value(directories[static_cast<size_t>(i)]);
			array->PropSetByNum(TJS_MEMBERENSURE, i, &value, array);
		}
		*result = tTJSVariant(array, array);
		array->Release();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getGameDirectoryList)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getGameFileList)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	if(result)
	{
		const auto files = krkrsdl2_list_game_files(*param[0]);
		iTJSDispatch2 *array = TJSCreateArrayObject();
		for(tjs_int i = 0; i < static_cast<tjs_int>(files.size()); ++i)
		{
			tTJSVariant value(files[static_cast<size_t>(i)]);
			array->PropSetByNum(TJS_MEMBERENSURE, i, &value, array);
		}
		*result = tTJSVariant(array, array);
		array->Release();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getGameFileList)
//----------------------------------------------------------------------
#ifdef __SWITCH__
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getAutocycleRound)
{
	// Test harness support (see krkrsdl2_service_autocycle).  The in-process
	// engine restart rebuilds the script engine, so a TJS-side cycle counter
	// would reset to 0 and the harness would launch the first game folder
	// forever, never exercising a cross-game hand-over.  The native counter
	// survives the restart, so the launcher script rotates through the folders
	// based on this value instead.
	if(result) *result = (tjs_int)krkrsdl2_autocycle_round_count();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getAutocycleRound)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/canLaunchGame)
{
	// launchXP3 already refuses to start in applet (album) mode and returns
	// false, but the launcher cannot tell "not enough memory" apart from any
	// other false without asking first.  Expose the same predicate so the
	// picker can say so up front instead of after a dead button press.
	if(result) *result = krkrsdl2_can_launch_game();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/canLaunchGame)
//----------------------------------------------------------------------
#endif
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/launchXP3)
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	if(!krkrsdl2_can_launch_game())
	{
		KRKRNS_LOG("[launcher] launch blocked: full-memory application mode is required");
		if(result) *result = false;
		return TJS_S_OK;
	}
	const ttstr entry = krkrsdl2_prepare_xp3_game(*param[0], *param[1]);

	// KRKR-ns: Kirikiroid2-compatible per-game XP3 extraction filter.  Titles
	// with scrambled archive content (Riddle Joker) ship xp3filter.tjs beside
	// the archives; Kirikiroid2 feeds it to a dedicated decoder engine whose
	// registered TJS callback decrypts every extracted chunk.  Arm it here
	// (before any game content is read); games without one clear the filter.
	{
		ttstr filter_script;
		const ttstr filter_path = TVPProjectDir + TJS_W("xp3filter.tjs");
		if(TVPIsExistentStorageNoSearch(filter_path))
		{
			iTJSTextReadStream *stream = TVPCreateTextStreamForRead(filter_path, TJS_W(""));
			if(stream)
			{
				try
				{
					stream->Read(filter_script, 0);
				}
				catch(...)
				{
					filter_script.Clear();
				}
				delete stream;
			}
		}
		TVPSetXP3FilterScript(filter_script);
	}

	// Kirikiroid-compatible distributions commonly place patch.tjs beside
	// their XP3 files.  It installs decrypt hooks and links compatibility
	// modules (notably emoteplayer) before the archive startup script runs.
	const ttstr game_patch = TVPProjectDir + TJS_W("patch.tjs");
	if(TVPIsExistentStorageNoSearch(game_patch))
	{
		KRKRNS_LOG("[launcher] executing sibling patch.tjs before game startup");
		TVPExecuteStorage(game_patch);
	}
	krkrsdl2_mount_xp3_resources();

	// The classic kirikiri2 debug console is hidden by the game re-spawning
	// itself with "-debugwin=no" and exiting the first instance (see the
	// game's startup.tjs: `getArgument("-debugwin") != "no"` -> shellExecute
	// + exit).  There is no second process to spawn here, so supply the
	// steady-state argument directly: games observe "-debugwin=no" and skip
	// that respawn-and-exit branch entirely, exactly like the console-hidden
	// Windows launch they are distributed for.
	TVPSetCommandLine(TJS_W("-debugwin"), TJS_W("no"));

	// KAG-standard boot globals. Stock Windows system init defines these;
	// game startup scripts evaluate expressions like "kirikiriz -debugwin"
	// which throw and abort the game's boot when the identifiers are
	// missing (observed on first run without a saved Config).
	//
	// Only set what is missing: the script engine survives across game
	// sessions here, and a game may declare these as read-only properties
	// (a plain re-assignment then throws "Invalid operation for Read-only or
	// Write-only property" and the second launch fails).
	{
		TVPExecuteScript(TJS_W(
			"if (typeof(global.kirikiriz) == \"undefined\") global.kirikiriz = 1;\n"
			"if (typeof(global.debugwin) == \"undefined\") global.debugwin = 0;\n"
			"if (typeof(global.inXP3archivePacked) == \"undefined\") global.inXP3archivePacked = 1;\n"
			"if (typeof(global.convertMode) == \"undefined\") global.convertMode = 0;\n"
			"if (typeof(global.debugWindowEnabled) == \"undefined\") global.debugWindowEnabled = 0;\n"
		));
		KRKRNS_LOG("[launcher] KAG boot globals ensured");
	}

	// libnx SDL permits only one native window.  If the launcher Window is
	// supplied, invalidate it synchronously before the game constructs its
	// own Window.  Suppress last-window termination only for this hand-off.
	const bool previous_terminate_on_close = TVPTerminateOnWindowClose;
	try
	{
		if(numparams >= 3 && param[2]->Type() == tvtObject)
		{
			TVPTerminateOnWindowClose = false;
			tTJSVariantClosure launcher = param[2]->AsObjectClosureNoAddRef();
			if(launcher.Object)
				launcher.Invalidate(0, nullptr, nullptr, launcher.ObjThis);
			// NOTE: deliberately do not force-free leftover window forms here.
			// The game creates its own window right after this, and reaching
			// into the window list at this point risks freeing the very form the
			// engine is still tearing down.
		}
		KRKRNS_STAGE("game startup begin");
		{
			// KRKR-ns: AsStdString() truncates at the first 0x00 on Switch; print UTF-8 manually.
			std::string utf8;
			for (tjs_uint i = 0; i < entry.GetLen() && i < 300; ++i)
			{
				tjs_uint32 ch = static_cast<tjs_uint32>(entry[i]);
				if (ch == 0) break;
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
			KRKRNS_LOG("[launcher] executing game startup: %s", utf8.c_str());
		}
		{
			// KRKR-ns diagnostic: log the startup script bytecode size and
			// leading bytes so real-hardware and emulator copies can be
			// compared byte-for-byte (a mismatched data.xp3 > startup.tjs
			// would silently boot-differently on the console).
			try
			{
				tTJSBinaryStream *s = TVPCreateStream(entry, TJS_BS_READ);
				if (s)
				{
					tjs_uint64 sz = s->GetSize();
					KRKRNS_LOG("[launcher] startup bytecode size: %lld", (long long)sz);
					tjs_uint8 head[64];
					tjs_uint n = (tjs_uint)s->Read(head, sizeof(head));
					{
						char hex[256]; int p = 0;
						for (tjs_uint i = 0; i < n && p < (int)sizeof(hex) - 3; ++i)
							p += snprintf(hex + p, sizeof(hex) - p, "%02x", head[i]);
						hex[p] = 0;
						KRKRNS_LOG("[launcher] startup bytecode head: %s", hex);
					}
					delete s;
				}
				else
				{
					KRKRNS_LOG("[launcher] startup stream open failed");
				}
			}
			catch (...)
			{
				KRKRNS_LOG("[launcher] startup stream probe threw");
			}
		}
		// KRKR-ns: guarantee the KAG2 compatibility namespace before the game's
		// startup script runs.  A game ships its own system/k2compat.tjs, which
		// REPLACES global.Krkr2CompatUtils with a stub whose members are supposed
		// to come from k2compat.dll -- absent here.  That copy is loaded from the
		// game's archive and therefore WINS over our compat path, and it strips
		// loadPlugin/unloadPlugin/isLoaded/autoLoad from the namespace.
		//
		// Repairing only after the game's script has run is too late:
		// initialize.tjs -> KAGLoadScriptOnce -> custom.tjs calls
		// Krkr2CompatUtils.loadPlugin and the boot dies first.  Reinstall here,
		// immediately before startup, so the members KAG needs are always
		// present; ScriptMgnIntf.cpp also reinstalls after any k2compat.tjs the
		// game loads, covering the rest of the session.
		// KRKR-ns: run OUR compat stub here as well.  It is normally pulled in by
		// the game itself -- KAGEX titles execute Scripts.execStorage(
		// "k2compat.tjs") and the patch path resolves that to this file -- but a
		// plain KAG3 title never does, and it still needs the script-side
		// members: its system/Menus.tjs builds the whole system menu from
		// `class KAGMenuItem extends MenuItem`, and the engine cannot supply that
		// class itself (in kirikiri2 both MenuItem and Window.menu belong to
		// menu.dll, and a title without the plugin supplies its own).  The stub
		// is guarded against re-entry, so a title that also loads it later is
		// unaffected.
		try
		{
			TVPExecuteStorage(ttstr(TJS_W("file://?/romfs:/compat/system/k2compat.tjs")));
		}
		catch(...)
		{
			KRKRNS_LOG("[launcher] compat stub execution failed");
		}
		TVPExecuteStorage(ttstr(TJS_W("file://?/romfs:/compat/system/k2compat_reinstall.tjs")));
		TVPExecuteStorage(entry);
		KRKRNS_STAGE("game startup returned");
		KRKRNS_LOG("[launcher] game startup returned");
	}
	catch(...)
	{
		TVPTerminateOnWindowClose = previous_terminate_on_close;
		throw;
	}
	TVPTerminateOnWindowClose = previous_terminate_on_close;
	if(result) *result = true;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/launchXP3)
//----------------------------------------------------------------------
#endif
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getFileProperty) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];
	if( result ) {
		iTJSDispatch2* dic = TVPGetFilePropertyNoAddRef( path );
		*result = tTJSVariant( dic, dic );
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getFileProperty )
//----------------------------------------------------------------------
// See TVPStoragesFstatDict above: the read-only half of fstat.dll.
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/fstat) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	if( result ) {
		iTJSDispatch2* dic = TVPStoragesFstatDict( *param[0], true );
		if( dic ) {
			*result = tTJSVariant( dic, dic );
			dic->Release();
		}
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/fstat )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getTime) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	if( result ) {
		iTJSDispatch2* dic = TVPStoragesFstatDict( *param[0], false );
		if( dic ) {
			*result = tTJSVariant( dic, dic );
			dic->Release();
		}
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getTime )
//----------------------------------------------------------------------
// The save-screen half of the same plugin (see the notes above).
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/deleteFile) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	if( result ) *result = TVPStoragesDeleteFile( *param[0] );
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/deleteFile )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/copyFile) {
	if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
	// The reference's copyFile takes two arguments; KAG titles call it with a
	// third (failIfExist) that this port ignores, like the plugin would.
	if( result ) *result = TVPStoragesCopyFile( *param[0], *param[1] );
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/copyFile )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dirlist) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	if( result ) *result = TVPStoragesDirList( *param[0], false );
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/dirlist )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dirlistEx) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	if( result ) *result = TVPStoragesDirList( *param[0], true );
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/dirlistEx )
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
tTJSNativeInstance * tTJSNC_Storages::CreateNativeInstance()
{
	// this class cannot create an instance
	TVPThrowExceptionMessage(TVPCannotCreateInstance);

	return NULL;
}
//---------------------------------------------------------------------------




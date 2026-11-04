//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// TJS2 "Object" class implementation
//---------------------------------------------------------------------------
#ifndef tjsObjectH
#define tjsObjectH

#include <vector>
#include <utility>
#include "tjsInterface.h"
#include "tjsVariant.h"
#include "tjsUtils.h"
#include "tjsError.h"
#include "proof_registry.h"

// Isolated lazy experiment only; this header is not installed.
namespace krkr_exact_proof { void BeforeStoredEnum(TJS::iTJSDispatch2 *); }
#ifdef __SWITCH__
#include "KrkrNSSymbolAllocator.h"
#endif

namespace TJS
{
//---------------------------------------------------------------------------
// utility functions
//---------------------------------------------------------------------------
extern tjs_error
	TJSDefaultFuncCall(tjs_uint32 flag, tTJSVariant &targ, tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis);

extern tjs_error TJSDefaultPropGet(tjs_uint32 flag, tTJSVariant &targ, tTJSVariant *result,
	iTJSDispatch2 *objthis);

extern tjs_error TJSDefaultPropSet(tjs_uint32 flag, tTJSVariant &targ, const tTJSVariant *param,
	iTJSDispatch2 *objthis);

extern tjs_error
	TJSDefaultInvalidate(tjs_uint32 flag, tTJSVariant &targ, iTJSDispatch2 *objthis);

extern tjs_error
	TJSDefaultIsValid(tjs_uint32 flag, tTJSVariant &targ, iTJSDispatch2 * objthis);

extern tjs_error
	TJSDefaultCreateNew(tjs_uint32 flag, tTJSVariant &targ,
		iTJSDispatch2 **result, tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *objthis);

extern tjs_error
	TJSDefaultIsInstanceOf(tjs_uint32 flag, tTJSVariant &targ, const tjs_char *name,
		iTJSDispatch2 *objthis);

extern tjs_error
	TJSDefaultOperation(tjs_uint32 flag, tTJSVariant &targ,
		tTJSVariant *result, const tTJSVariant *param, iTJSDispatch2 *objthis);

TJS_EXP_FUNC_DEF(void, TJSDoVariantOperation, (tjs_int op, tTJSVariant &target, const tTJSVariant *param));
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// hash rebuilding
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(void, TJSDoRehash, ());
//---------------------------------------------------------------------------



/*[*/
//---------------------------------------------------------------------------
// tTJSDispatch
//---------------------------------------------------------------------------
/*
	tTJSDispatch is a base class that implements iTJSDispatch2, and every methods.
	most methods are maked as simply returning "TJS_E_NOTIMPL";
	methods, those access the object by index, call another methods that access
	the object by string.
*/
/*
#define TJS_SELECT_OBJTHIS(__closure__, __override__) \
	((__closure__).ObjThis?((__override__)?(__override__):(__closure__).ObjThis):(__override__))
*/
#define TJS_SELECT_OBJTHIS(__closure__, __override__) \
	((__closure__).ObjThis?(__closure__).ObjThis:(__override__))

class tTJSDispatch : public iTJSDispatch2
{
	virtual void BeforeDestruction(void) {;}
	bool BeforeDestructionCalled;
		// BeforeDestruction will be certainly called before object destruction
private:
	tjs_uint RefCount;
public:
	tTJSDispatch();
	virtual ~tTJSDispatch();

//	bool DestructionTrace;

public:
	tjs_uint TJS_INTF_METHOD  AddRef(void);
	tjs_uint TJS_INTF_METHOD  Release(void);

protected:
	tjs_uint GetRefCount() { return RefCount; }

public:
	tjs_error TJS_INTF_METHOD
	FuncCall(
		tjs_uint32 flag,
		const tjs_char * membername,
		tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams,
		tTJSVariant **param,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	FuncCallByNum(
		tjs_uint32 flag,
		tjs_int num,
		tTJSVariant *result,
		tjs_int numparams,
		tTJSVariant **param,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	PropGet(
		tjs_uint32 flag,
		const tjs_char * membername,
		tjs_uint32 *hint,
		tTJSVariant *result,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	PropGetByNum(
		tjs_uint32 flag,
		tjs_int num,
		tTJSVariant *result,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	PropSet(
		tjs_uint32 flag,
		const tjs_char *membername,
		tjs_uint32 *hint,
		const tTJSVariant *param,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	PropSetByNum(
		tjs_uint32 flag,
		tjs_int num,
		const tTJSVariant *param,
		iTJSDispatch2 *objthis
		);
	
	tjs_error TJS_INTF_METHOD
	GetCount(
		tjs_int *result,
		const tjs_char *membername,
		tjs_uint32 *hint,
		iTJSDispatch2 *objthis
		)
	{
		return TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	GetCountByNum(
		tjs_int *result,
		tjs_int num,
		iTJSDispatch2 *objthis
		);


	tjs_error TJS_INTF_METHOD
	PropSetByVS(tjs_uint32 flag, tTJSVariantString *membername,
		const tTJSVariant *param, iTJSDispatch2 *objthis)
	{
		return TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	EnumMembers(tjs_uint32 flag, tTJSVariantClosure *callback, iTJSDispatch2 *objthis)
	{
		return TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	DeleteMember(
		tjs_uint32 flag,
		const tjs_char *membername,
		tjs_uint32 *hint,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	DeleteMemberByNum(
		tjs_uint32 flag,
		tjs_int num,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	Invalidate(
		tjs_uint32 flag,
		const tjs_char *membername,
		tjs_uint32 *hint,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	InvalidateByNum(
		tjs_uint32 flag,
		tjs_int num,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	IsValid(
		tjs_uint32 flag,
		const tjs_char *membername,
		tjs_uint32 *hint,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	IsValidByNum(
		tjs_uint32 flag,
		tjs_int num,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	CreateNew(
		tjs_uint32 flag,
		const tjs_char * membername,
		tjs_uint32 *hint,
		iTJSDispatch2 **result,
		tjs_int numparams,
		tTJSVariant **param,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	CreateNewByNum(
		tjs_uint32 flag,
		tjs_int num,
		iTJSDispatch2 **result,
		tjs_int numparams,
		tTJSVariant **param,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	Reserved1(
		)
	{
		return TJS_E_NOTIMPL;
	}


	tjs_error TJS_INTF_METHOD
	IsInstanceOf(
		tjs_uint32 flag,
		const tjs_char *membername,
		tjs_uint32 *hint,
		const tjs_char *classname,
		iTJSDispatch2 *objthis
		)
	{
		return membername?TJS_E_MEMBERNOTFOUND:TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	IsInstanceOfByNum(
		tjs_uint32 flag,
		tjs_int num,
		const tjs_char *classname,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	Operation(
		tjs_uint32 flag,
		const tjs_char *membername,
		tjs_uint32 *hint,
		tTJSVariant *result,
		const tTJSVariant *param,
		iTJSDispatch2 *objthis
		);

	tjs_error TJS_INTF_METHOD
	OperationByNum(
		tjs_uint32 flag,
		tjs_int num,
		tTJSVariant *result,
		const tTJSVariant *param,
		iTJSDispatch2 *objthis
		);


	tjs_error TJS_INTF_METHOD
	NativeInstanceSupport(
		tjs_uint32 flag,
		tjs_int32 classid,
		iTJSNativeInstance **pointer
		)
	{
		return TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	ClassInstanceInfo(
		tjs_uint32 flag,
		tjs_uint num,
		tTJSVariant *value
		)
	{
		return TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	Reserved2(
		)
	{
		return TJS_E_NOTIMPL;
	}

	tjs_error TJS_INTF_METHOD
	Reserved3(
		)
	{
		return TJS_E_NOTIMPL;
	}


};
//---------------------------------------------------------------------------
/*]*/




//---------------------------------------------------------------------------
// tTJSCustomObject
//---------------------------------------------------------------------------

#define TJS_NAMESPACE_DEFAULT_HASH_BITS 3

extern tjs_int TJSObjectHashBitsLimit;
	// this limits hash table size


#define TJS_SYMBOL_USING	0x1
#define TJS_SYMBOL_INIT     0x2
#define TJS_SYMBOL_HIDDEN   0x8
#define TJS_SYMBOL_STATIC	0x10

#define TJS_MAX_NATIVE_CLASS 4
/*
	Number of "Native Class Instance" that can be used per a TJS Object is
	limited as the number above.
*/


// Optional typed callback for copying raw members. EnumMembers still dispatches
// virtually; custom enumerators can use the ordinary FuncCall interface. The
// fast path is only valid with IGNOREPROP and a value, and always continues.
class tTJSCustomObject;
class tTJSMemberCopyCallback : public tTJSDispatch
{
public:
	virtual void CopyMembers(const tTJSCustomObject &source) = 0;
};

class tTJSCustomObject : public tTJSDispatch
{
	typedef tTJSDispatch inherited;
#if defined(__SWITCH__) || defined(KRKRNS_NATIVE_ARRAY_FACTORY_TEST)
	// The private native Array factory can initialize a fresh symbol table
	// after checking the exact source/destination and reference types.
	friend class tTJSArrayClass;
#endif

	// tTJSSymbolData -----------------------------------------------------
public:
	struct tTJSSymbolData
	{
		tTJSVariantString *Name; // name
		tjs_uint32 Hash; // hash code of the name
		tjs_uint32 SymFlags; // management flags
		tjs_uint32 Flags;  // flags

		tTJSVariant_S Value; // the value
			/*
				TTJSVariant_S must work with construction that fills
					all member to zero.
			*/

		tTJSSymbolData * Next; // next chain

#ifdef __SWITCH__
		// Pool scalar collision nodes without changing their layout or the
		// ordinary new[] path used by the base hash table. Ownership remains
		// with each object and existing deletion/enumeration order is preserved.
		static void *operator new(std::size_t size)
		{
			if(size != sizeof(tTJSSymbolData)) throw std::bad_alloc();
			return krkrns::SymbolAllocator<sizeof(tTJSSymbolData),
				alignof(tTJSSymbolData)>::Allocate();
		}
		static void operator delete(void *pointer) noexcept
		{
			krkrns::SymbolAllocator<sizeof(tTJSSymbolData),
				alignof(tTJSSymbolData)>::Deallocate(pointer);
		}
		static void operator delete(void *pointer, std::size_t) noexcept
		{
			operator delete(pointer);
		}
		static void *operator new(std::size_t size, const std::nothrow_t&) noexcept
		{
			try { return operator new(size); }
			catch(...) { return nullptr; }
		}
		static void operator delete(void *pointer, const std::nothrow_t&) noexcept
		{
			operator delete(pointer);
		}
		static void *operator new(std::size_t, void *pointer) noexcept { return pointer; }
		static void operator delete(void *, void *) noexcept {}

		// The compiler owns the array cookie and supplies the same total size
		// to these two functions. Keep only the usual sized array deallocator
		// so trivial Symbol arrays still receive that compiler-managed cookie.
		static void *operator new[](std::size_t bytes)
		{
			static_assert(alignof(tTJSSymbolData) <= alignof(void*),
				"Symbol array pool must satisfy the element alignment");
			return krkrns::SymbolArrayAllocator::Allocate(bytes);
		}
		static void operator delete[](void *pointer, std::size_t bytes) noexcept
		{
			krkrns::SymbolArrayAllocator::Deallocate(pointer, bytes);
		}
		static void *operator new[](std::size_t, void *pointer) noexcept { return pointer; }
		static void operator delete[](void *, void *) noexcept {}
#endif

		void SelfClear(void)
		{
			memset(this, 0, sizeof(*this));
			SymFlags = TJS_SYMBOL_INIT;
		}

		void _SetName(const tjs_char * name)
		{
			if(Name) Name->Release(), Name = NULL;
			if(!name) TJS_eTJSError(TJSIDExpected);
			if(!name[0]) TJS_eTJSError(TJSIDExpected);
			Name = TJSAllocVariantString(name);
		}

		void SetName(const tjs_char * name, tjs_uint32 hash)
		{
			_SetName(name);
			Hash = hash;
		}

		void _SetName(tTJSVariantString *name)
		{
			if(name == Name) return;
			if(Name) Name->Release();
			Name = name;
			if(Name) Name->AddRef();
		}

		void SetName(tTJSVariantString *name, tjs_uint32 hash)
		{
			_SetName(name);
			Hash = hash;
		}

		const tjs_char * GetName() const
		{
			return (const tjs_char *)(*Name);
		}

		void PostClear()
		{
			if(Name) Name->Release(), Name = NULL;
			((tTJSVariant*)(&Value))->~tTJSVariant();
			memset(&Value, 0, sizeof(Value));
			SymFlags &= ~TJS_SYMBOL_USING;
		}

		void Destory()
		{
			if(Name) Name->Release();
			((tTJSVariant*)(&Value))->~tTJSVariant();
		}

		bool NameMatch(const tjs_char * name)
		{
			const tjs_char * this_name = GetName();
			if(this_name == name) return true;
			return !TJS_strcmp(name, this_name);
		}

		void ReShare();
	};



	//---------------------------------------------------------------------
	tjs_int Count;
	tjs_int HashMask;
	tjs_int HashSize;
	tTJSSymbolData * Symbols;
	tjs_uint RebuildHashMagic;
	bool IsInvalidated;
	bool IsInvalidating;
	iTJSNativeInstance* ClassInstances[TJS_MAX_NATIVE_CLASS];
	tjs_int32 ClassIDs[TJS_MAX_NATIVE_CLASS];


	void _Finalize(void);

	//---------------------------------------------------------------------
protected:
	bool GetValidity() const { return !IsInvalidated; }
	bool CallFinalize; // set false if this object does not need to call "finalize"
	ttstr finalize_name; // name of the 'finalize' method
	bool CallMissing; // set true if this object should call 'missing' method
	bool ProsessingMissing; // true if 'missing' method is being called
	ttstr missing_name; // name of the 'missing' method
	virtual void Finalize(void);
	std::vector<ttstr > ClassNames;

	//---------------------------------------------------------------------
public:

	tTJSCustomObject(tjs_int hashbits = TJS_NAMESPACE_DEFAULT_HASH_BITS);
	virtual ~tTJSCustomObject();

private:
	void BeforeDestruction(void);

	void CheckObjectClosureAdd(const tTJSVariant &val)
	{
		// adjust the reference counter when the object closure's "objthis" is
		// referring to the object itself.
		if(val.Type() == tvtObject)
		{
			iTJSDispatch2 *dsp = val.AsObjectClosureNoAddRef().ObjThis;
			if(dsp == (iTJSDispatch2*)this) this->Release();
		}
	}

	void CheckObjectClosureRemove(const tTJSVariant &val)
	{
		if(val.Type() == tvtObject)
		{
			iTJSDispatch2 *dsp = val.AsObjectClosureNoAddRef().ObjThis;
			if(dsp == (iTJSDispatch2*)this) this->AddRef();
		}
	}

	bool CallGetMissing(const tjs_char *name, tTJSVariant &result);
	bool CallSetMissing(const tjs_char *name, const tTJSVariant &value);

	tTJSSymbolData * Add(const tjs_char * name, tjs_uint32 *hint);
		// Adds the symbol, returns the newly created data;
		// if already exists, returns the data.

	tTJSSymbolData * Add(tTJSVariantString * name);
		// tTJSVariantString version of above.

	tTJSSymbolData * AddTo(tTJSVariantString *name,
		tTJSSymbolData *newdata, tjs_int newhashmask);
		// Adds member to the new hash space, used in RebuildHash

	void RebuildHash(); // rebuild hash table

	bool DeleteByName(const tjs_char * name, tjs_uint32 *hint);
		// Deletes Name

	void DeleteAllMembers(void);
		// Deletes all members
	void _DeleteAllMembers(void);
		// Deletes all members ( not to use std::vector )

	tTJSSymbolData * Find(const tjs_char * name, tjs_uint32 *hint) ;
		// Finds Name, returns its data; if not found, returns NULL

	static bool CallEnumCallbackForData(tjs_uint32 flags,
		tTJSVariant ** params,
		tTJSVariantClosure & callback, iTJSDispatch2 * objthis,
		const tTJSSymbolData * data);
	void InternalEnumMembers(tjs_uint32 flags, tTJSVariantClosure *callback,
		iTJSDispatch2 *objthis);
	//---------------------------------------------------------------------
public:
	// Isolated transaction experiment: these helpers bypass every dispatch hook.
	// Caller must own both exact native objects and run on the TJS thread.
	tjs_int UnsafeTxnCount() const noexcept { return Count; }
	bool UnsafeTxnEmptyStorage() const noexcept {
		if (Count || !Symbols) return false;
		for (tjs_int i=0; i<HashSize; ++i)
			if ((Symbols[i].SymFlags & TJS_SYMBOL_USING) || Symbols[i].Next) return false;
		return true;
	}
	tjs_int UnsafeTxnHashSize() const noexcept { return HashSize; }
	tjs_int UnsafeTxnHashMask() const noexcept { return HashMask; }
	tjs_uint UnsafeTxnHashMagic() const noexcept { return RebuildHashMagic; }
	const void *UnsafeTxnSymbols() const noexcept { return Symbols; }
	bool UnsafeTxnReadMember(const tjs_char *name, tTJSVariant &out) const {
		for (const tTJSSymbolData *b=Symbols; b<Symbols+HashSize; ++b)
			for (const tTJSSymbolData *d=b; d; d=d->Next)
				if ((d->SymFlags & TJS_SYMBOL_USING) && !TJS_strcmp((const tjs_char *)(*d->Name),name)) {
					out.CopyRef(*reinterpret_cast<const tTJSVariant *>(&d->Value)); return true;
				}
		return false;
	}
	// Test-only bypass to model legacy/foreign code that installed a native
	// instance before the REGISTER barrier. Never use in production.
	bool UnsafeTxnTestInstallNative(tjs_int32 id, iTJSNativeInstance *native) noexcept {
		if (id==-1 || !native) return false;
		for (tjs_int i=0; i<TJS_MAX_NATIVE_CLASS; ++i) if (ClassIDs[i]==id) return false;
		for (tjs_int i=0; i<TJS_MAX_NATIVE_CLASS; ++i) if (ClassIDs[i]==-1) {
			ClassIDs[i]=id; ClassInstances[i]=native; return true;
		}
		return false;
	}
	iTJSNativeInstance *UnsafeTxnNative(tjs_int32 id) const noexcept {
		for (tjs_int i=0; i<TJS_MAX_NATIVE_CLASS; ++i)
			if (ClassIDs[i]==id) return ClassInstances[i];
		return nullptr;
	}
	bool UnsafeTxnNoBoundValues(const tTJSCustomObject *target) const noexcept {
		for (const tTJSSymbolData *b=Symbols; b<Symbols+HashSize; ++b) {
			for (const tTJSSymbolData *d=b; d; d=d->Next) {
				if (!(d->SymFlags & TJS_SYMBOL_USING)) continue;
				const auto &v=*reinterpret_cast<const tTJSVariant *>(&d->Value);
				if (v.Type()!=tvtObject) continue;
				const auto c=v.AsObjectClosureNoAddRef();
				if (c.Object==this || c.ObjThis==this || c.Object==target || c.ObjThis==target)
					return false;
			}
		}
		return true;
	}
	// Non-owning by design: on any uncertain teardown the holder and its table
	// are quarantined together; its destructor must never implicitly release TJS
	// values during exception unwinding.
	struct UnsafeTxnTable {
		tjs_int count=0, mask=0, size=0;
		tjs_uint magic=0;
		tTJSSymbolData *symbols=nullptr;
	};
	bool UnsafeTxnDetachDictionary(UnsafeTxnTable &out, const tTJSCustomObject &target) {
		if (out.symbols || IsInvalidated || !UnsafeTxnNoBoundValues(&target)) return false;
		// Allocate before changing stage, so allocation failure leaves it intact.
		auto *empty=new tTJSSymbolData[HashSize];
		memset(empty,0,sizeof(tTJSSymbolData)*HashSize);
		out.count=Count; out.mask=HashMask; out.size=HashSize;
		out.magic=RebuildHashMagic; out.symbols=Symbols;
		Count=0; Symbols=empty;
		return true;
	}
	bool UnsafeTxnCommitDictionary(UnsafeTxnTable &in) noexcept {
		if (!UnsafeTxnEmptyStorage() || IsInvalidated || !in.symbols) return false;
		auto *oldEmpty=Symbols;
		Count=in.count; HashMask=in.mask; HashSize=in.size;
		RebuildHashMagic=in.magic; Symbols=in.symbols;
		in.symbols=nullptr;
		// Count==0 implies no constructed values or collision chains here.
		delete [] oldEmpty;
		return true;
	}

	// Raw, chain-first iteration for the typed native member copier. Property
	// access remains in EnumMembers; callers here only copy stored values.
	template<typename Consumer> void EnumStoredMembers(Consumer consumer) const
	{
		::krkr_exact_proof::DispatchHold proofHold(const_cast<tTJSCustomObject *>(this),nullptr);
		::krkr_exact_proof::BeforeStoredEnum(const_cast<tTJSCustomObject *>(this));
		auto call = [&consumer](const tTJSSymbolData *data) {
			if (!(data->SymFlags & TJS_SYMBOL_USING)) return;
			tjs_uint32 flags = 0;
			if (data->SymFlags & TJS_SYMBOL_HIDDEN) flags |= TJS_HIDDENMEMBER;
			if (data->SymFlags & TJS_SYMBOL_STATIC) flags |= TJS_STATICMEMBER;
			consumer(data->Name, flags, *reinterpret_cast<const tTJSVariant *>(&data->Value));
		};
		for (const tTJSSymbolData *bucket = Symbols, *end = Symbols + HashSize; bucket < end; ++bucket)
		{
			for (const tTJSSymbolData *data = bucket->Next; data; )
			{
				const tTJSSymbolData *next = data->Next;
				call(data);
				data = next;
			}
			call(bucket);
		}
	}
	void Clear() { DeleteAllMembers(); }
	/**
	 * rebuild hash table
	 */
	void RebuildHash( tjs_int requestcount );

	//---------------------------------------------------------------------
public:
	tjs_int GetValueInteger(const tjs_char * name, tjs_uint32 *hint);
		// service function for lexical analyzer
	//---------------------------------------------------------------------
public:

	tjs_error TJS_INTF_METHOD
	FuncCall(tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
	tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	PropGet(tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
	 tTJSVariant *result,
		iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	PropSet(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
	 const tTJSVariant *param,
		iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	GetCount(tjs_int *result, const tjs_char *membername, tjs_uint32 *hint,
	 iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	PropSetByVS(tjs_uint32 flag, tTJSVariantString *membername,
		const tTJSVariant *param, iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	EnumMembers(tjs_uint32 flag, tTJSVariantClosure *callback, iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	DeleteMember(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
	 iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	Invalidate(tjs_uint32 flag, const tjs_char *membername,  tjs_uint32 *hint,
	iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	IsValid(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
	 iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	CreateNew(tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
	 iTJSDispatch2 **result,
		tjs_int numparams, tTJSVariant **param,	iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	IsInstanceOf(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
	 const tjs_char *classname,
		iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	Operation(tjs_uint32 flag, const tjs_char *membername, tjs_uint32 *hint,
	 tTJSVariant *result,
		const tTJSVariant *param,	iTJSDispatch2 *objthis);

	tjs_error TJS_INTF_METHOD
	NativeInstanceSupport(tjs_uint32 flag, tjs_int32 classid,
		iTJSNativeInstance **pointer);

	tjs_error TJS_INTF_METHOD
	ClassInstanceInfo(tjs_uint32 flag, tjs_uint num, tTJSVariant *value);
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TJSCreateCustomObject
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(iTJSDispatch2 *, TJSCreateCustomObject, ());
//---------------------------------------------------------------------------


} // namespace TJS


#endif

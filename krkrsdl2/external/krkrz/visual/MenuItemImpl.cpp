//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000-2007 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "MenuItem" class implementation
//---------------------------------------------------------------------------
#include "tjsCommHead.h"


#include "EventIntf.h"
#include "MenuItemImpl.h"
#include "MsgIntf.h"
#include "WindowIntf.h"
#include "Application.h"
#include "tjsDictionary.h"
#include "ScriptMgnIntf.h"
static void TVPEnsureMenuPopupScript()
{
    tTJS *engine = TVPGetScriptEngine();
    if(!engine) return;
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    tTJSVariant helper;
    if(TJS_FAILED(global->PropGet(0, TJS_W("__krkrnsMenuPopup"), NULL,
        &helper, global)) || helper.Type() != tvtObject || !helper.AsObjectNoAddRef())
        TVPExecuteStorage(TJS_W("file://?/romfs:/compat/system/menu_popup.tjs"));
}

// Keep menu roots in their Window instance and class objects in the current
// script global. Neither may survive an engine restart in a process-wide map.
class WindowMenuProperty : public tTJSDispatch {
    tjs_error TJS_INTF_METHOD PropGet(tjs_uint32, const tjs_char *, tjs_uint32 *,
        tTJSVariant *result, iTJSDispatch2 *objthis) {
        if(!objthis) return TJS_E_INVALIDOBJECT;
        tTJSNI_Window *window = NULL;
        if(TJS_FAILED(objthis->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
            tTJSNC_Window::ClassID, (iTJSNativeInstance**)&window)) || !window)
            return TJS_E_INVALIDOBJECT;
        tTJSVariant value;
        if(TJS_FAILED(objthis->PropGet(TJS_MEMBERMUSTEXIST,
            TJS_W("_krkrnsNativeMenu"), NULL, &value, objthis)) ||
            value.Type() != tvtObject || !value.AsObjectNoAddRef() ||
            value.AsObjectNoAddRef()->IsValid(0, NULL, NULL,
                value.AsObjectNoAddRef()) != TJS_S_TRUE) {
            iTJSDispatch2 *menu = TVPCreateMenuItemObject(objthis);
            value = tTJSVariant(menu, menu);
            menu->Release();
            tjs_error hr = objthis->PropSet(TJS_MEMBERENSURE,
                TJS_W("_krkrnsNativeMenu"), NULL, &value, objthis);
            if(TJS_FAILED(hr)) return hr;
        }
        if(result) *result = value;
        return TJS_S_OK;
    }
    tjs_error TJS_INTF_METHOD PropSet(tjs_uint32, const tjs_char *, tjs_uint32 *,
        const tTJSVariant *, iTJSDispatch2 *) {
        return TJS_E_ACCESSDENYED;
    }
};

// This dispatch owns its table together with the native MenuItem class.
class MenuShortcutProperty : public tTJSDispatch {
    iTJSDispatch2 *Table;
public:
    explicit MenuShortcutProperty(iTJSDispatch2 *table) : Table(table) {}
    ~MenuShortcutProperty() { Table->Release(); }
    tjs_error TJS_INTF_METHOD PropGet(tjs_uint32, const tjs_char *, tjs_uint32 *,
        tTJSVariant *result, iTJSDispatch2 *) {
        if(result) *result = tTJSVariant(Table, Table);
        return TJS_S_OK;
    }
    tjs_error TJS_INTF_METHOD PropSet(tjs_uint32, const tjs_char *, tjs_uint32 *,
        const tTJSVariant *, iTJSDispatch2 *) {
        return TJS_E_ACCESSDENYED;
    }
};

//---------------------------------------------------------------------------
// tTJSNI_MenuItem
//---------------------------------------------------------------------------
tTJSNI_MenuItem::tTJSNI_MenuItem()
{
	IsChecked = false;
	IsAttched = true;
	IsEnabled = true;
	IsRadio = false;
	IsVisible = true;
	GroupIndex = 0;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_MenuItem::Construct(tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *tjs_obj)
{
	tjs_error hr = inherited::Construct(numparams, param, tjs_obj);
	if(TJS_FAILED(hr)) return hr;

	// create or attach MenuItem object
	if(Window)
	{
		//MenuItem = Window->GetRootMenuItem();
		IsAttched = true;
	}
	else
	{
// 		MenuItem = new TMenuItem();
// 		MenuItem->OnClick = std::bind(&tTJSNI_MenuItem::MenuItemClick, this);
		IsAttched = false;
	}

	// fetch initial caption
	if(!Window && numparams >= 2)
	{
		Caption = *param[1];
		//MenuItem->setCaption (Caption);
	}

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_MenuItem::Invalidate()
{
	// invalidate inherited
	inherited::Invalidate();  // this sets Owner = NULL

	// delete VCL object
	//if (!IsAttched && MenuItem) delete MenuItem, MenuItem = NULL;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::MenuItemClick()
{
	// VCL event handler
	// post to the event queue
	TVPPostInputEvent(new tTVPOnMenuItemClickInputEvent(this));
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::CanDeliverEvents() const
{
	// returns whether events can be delivered
	//if(!MenuItem) return false;
	bool enabled = true;
	
	const tTJSNI_MenuItem *item = this;
	while(item)
	{
		if (!item->GetEnabled())
		{
			enabled = false;
			break;
		}
		item = static_cast<const tTJSNI_MenuItem*>(item->GetParent());
	}
	return enabled;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Add(tTJSNI_MenuItem * item)
{
// 	if(MenuItem && item->MenuItem)
// 	{
// 		MenuItem->Add(item->MenuItem);
		AddChild(item);
	//}
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Insert(tTJSNI_MenuItem *item, tjs_int index)
{
    if(!Owner || !item || !item->Owner)
        TVPThrowExceptionMessage(TJS_W("Cannot attach to an invalid menu item."));
    if(index < 0 || index > static_cast<tjs_int>(Children.size()))
        TVPThrowExceptionMessage(TJS_W("Menu item index out of range."));
    if(item->Parent == this) {
        // An insertion at the end of the same tree means its last position.
        item->SetIndex(std::min(index, static_cast<tjs_int>(Children.size()) - 1));
        return;
    }
    AddChild(item, index);
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::Remove(tTJSNI_MenuItem *item)
{
	if(item->Parent != this || Children.Find(item) < 0)
		TVPThrowExceptionMessage(TVPNotChildMenuItem);
// 	if(MenuItem && item->MenuItem)
// 	{
// 		int index = MenuItem->IndexOf(item->MenuItem);
// 		if(index == -1) TVPThrowExceptionMessage(TVPNotChildMenuItem);
// 
// 		MenuItem->Delete(index);
		RemoveChild(item);
	//}
}
//---------------------------------------------------------------------------
void *tTJSNI_MenuItem::GetMenuItemHandleForPlugin() const
{
	/*if(!MenuItem)*/ return NULL;
	//return MenuItem->getHandle();
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_MenuItem::GetIndex() const
{
	if (!Parent) return 0;
	//if(!MenuItem) return 0;
	//return MenuItem->getMenuIndex();
	tjs_int idx = Parent->Children.Find((tTJSNI_BaseMenuItem*)this);
	if (idx < 0) idx = 0;
	return idx;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetIndex(tjs_int newIndex)
{
	if(!Parent) return;
	std::lock_guard<std::mutex> holder(Parent->Children.Lock);
	auto &siblings = Parent->Children;
	if(newIndex < 0 || newIndex >= static_cast<tjs_int>(siblings.size()))
		TVPThrowExceptionMessage(TJS_W("Menu item index out of range."));
	auto current = std::find(siblings.begin(), siblings.end(), this);
	if(current == siblings.end())
		TVPThrowExceptionMessage(TVPNotChildMenuItem);
	auto target = siblings.begin() + newIndex;
	if(current < target)
		std::rotate(current, current + 1, target + 1);
	else if(current > target)
		std::rotate(target, current, current + 1);
	Parent->ChildrenArrayValid = false;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetCaption(const ttstr & caption)
{
	//if(!MenuItem) return;
	Caption = caption;
// 	MenuItem->setAutoHotkeys (maManual);
// 	MenuItem->setCaption (caption);
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::GetCaption(ttstr & caption) const
{
	//if(!MenuItem) caption.Clear();
	caption = Caption;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetChecked(bool b)
{
	//if(!MenuItem) return;
	//MenuItem->setChecked (b);
	if (b && IsRadio && Parent) {
		for (tTJSNI_BaseMenuItem *_item : Parent->Children) {
			tTJSNI_MenuItem *item = static_cast<tTJSNI_MenuItem*>(_item);
			if (item->IsRadio && item->GroupIndex == GroupIndex)
				item->IsChecked = false;
		}
	}
	IsChecked = b;
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetChecked() const
{
// 	if(!MenuItem) return false;
// 	return MenuItem->getChecked();
	return IsChecked;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetEnabled(bool b)
{
	IsEnabled = b;
// 	if(!MenuItem) return;
// 	MenuItem->setEnabled (b);
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetEnabled() const
{
	return IsEnabled;
// 	if(!MenuItem) return false;
// 	return MenuItem->getEnabled();
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetGroup(tjs_int g)
{
	GroupIndex = g;
// 	if(!MenuItem) return;
// 	MenuItem->setGroupIndex ((BYTE)g);
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_MenuItem::GetGroup() const
{
	return GroupIndex;
// 	if(!MenuItem) return 0;
// 	return MenuItem->getGroupIndex();
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetRadio(bool b)
{
	IsRadio = b;
// 	if(!MenuItem) return;
// 	MenuItem->setRadioItem (b);
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetRadio() const
{
	return IsRadio;
// 	if(!MenuItem) return false;
// 	return MenuItem->getRadioItem();
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetShortcut(const ttstr & shortcut)
{
	// Preserve the script value without claiming a platform accelerator.
	Shortcut = shortcut;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::GetShortcut(ttstr & shortcut) const
{
	shortcut = Shortcut;
}
//---------------------------------------------------------------------------
void tTJSNI_MenuItem::SetVisible(bool b)
{
	IsVisible = b;
// 	if(!MenuItem) return;
// 	if(Window) Window->SetMenuBarVisible(b); else MenuItem->setVisible (b);
}
//---------------------------------------------------------------------------
bool tTJSNI_MenuItem::GetVisible() const
{
	return IsVisible;
// 	if(!MenuItem) return false;
// 	if(Window) return Window->GetMenuBarVisible(); else return MenuItem->getVisible();
}

//---------------------------------------------------------------------------
tjs_int tTJSNI_MenuItem::TrackPopup(tjs_uint32 flags, tjs_int x, tjs_int y) const
{
    if(!Owner || !CanDeliverEvents()) return 0;
    tTJS *engine = TVPGetScriptEngine();
    if(!engine) return 0;
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    tTJSVariant helper;
    if(TJS_FAILED(global->PropGet(0, TJS_W("__krkrnsMenuPopup"), NULL,
        &helper, global)) || helper.Type() != tvtObject || !helper.AsObjectNoAddRef()) {
        TVPEnsureMenuPopupScript();
        if(TJS_FAILED(global->PropGet(0, TJS_W("__krkrnsMenuPopup"), NULL,
            &helper, global)) || helper.Type() != tvtObject || !helper.AsObjectNoAddRef())
            return 0;
    }
    tTJSVariant args[] = {tTJSVariant(Owner, Owner), tTJSVariant((tjs_int64)flags),
        tTJSVariant(x), tTJSVariant(y)};
    tTJSVariant *params[] = {args, args + 1, args + 2, args + 3};
    tTJSVariant result;
    tjs_error hr = helper.AsObjectClosureNoAddRef().FuncCall(0, NULL, NULL,
        &result, 4, params, global);
    if(TJS_FAILED(hr)) return 0;
    return result.Type() == tvtVoid ? 1 : (tjs_int)result;
}

//---------------------------------------------------------------------------
// tTJSNC_MenuItem::CreateNativeInstance
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_MenuItem::CreateNativeInstance()
{
	return new tTJSNI_MenuItem();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateNativeClass_MenuItem
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_MenuItem()
{
	tTJSNativeClass *cls = new tTJSNC_MenuItem();
	static tjs_uint32 TJS_NCM_CLASSID;
	TJS_NCM_CLASSID = tTJSNC_MenuItem::ClassID;

//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(HMENU)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_MenuItem);
		if (result) *result = (tTVInteger)(void*)_this->GetMenuItemHandleForPlugin();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL_OUTER(cls, HMENU)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(__krkrnsNativeMenu)
{
    TJS_BEGIN_NATIVE_PROP_GETTER
    {
        if(result) *result = true;
        return TJS_S_OK;
    }
    TJS_END_NATIVE_PROP_GETTER
    TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL_OUTER(cls, __krkrnsNativeMenu)
    iTJSDispatch2 *textToKeycode = TJSCreateDictionaryObject();
    iTJSDispatch2 *keycodeToText = TJSCreateArrayObject();
    // The platform key names use Kirikiri virtual-key values. Modifiers are
    // interpreted by the popup/accelerator adapter, not by this table.
    auto addKey = [&](const ttstr &name, tjs_int key) {
        ttstr lower = name.AsLowerCase();
        tTJSVariant code(key), text(name);
        textToKeycode->PropSet(TJS_MEMBERENSURE, lower.c_str(), NULL, &code, textToKeycode);
        keycodeToText->PropSetByNum(TJS_MEMBERENSURE, key, &text, keycodeToText);
    };
    for(tjs_int key = '0'; key <= '9'; ++key) {
        tjs_char text[] = {(tjs_char)key, 0};
        addKey(ttstr(text), key);
    }
    for(tjs_int key = 'A'; key <= 'Z'; ++key) {
        tjs_char text[] = {(tjs_char)key, 0};
        addKey(ttstr(text), key);
    }
    for(tjs_int key = 1; key <= 24; ++key)
        addKey(ttstr(TJS_W("F")) + ttstr(key), 0x6f + key);
    const struct { const tjs_char *name; tjs_int key; } keys[] = {
        {TJS_W("Backspace"), 8}, {TJS_W("Tab"), 9}, {TJS_W("Enter"), 13},
        {TJS_W("Escape"), 27}, {TJS_W("Space"), 32}, {TJS_W("PageUp"), 33},
        {TJS_W("PageDown"), 34}, {TJS_W("End"), 35}, {TJS_W("Home"), 36},
        {TJS_W("Left"), 37}, {TJS_W("Up"), 38}, {TJS_W("Right"), 39},
        {TJS_W("Down"), 40}, {TJS_W("Insert"), 45}, {TJS_W("Delete"), 46}
    };
    for(const auto &key : keys) addKey(ttstr(key.name), key.key);
    for(tjs_int digit = 0; digit <= 9; ++digit)
        addKey(ttstr(TJS_W("Num ")) + ttstr(digit), 0x60 + digit);
    const struct { const tjs_char *name; tjs_int key; } aliases[] = {
        {TJS_W("BkSp"), 8}, {TJS_W("Back"), 8}, {TJS_W("Return"), 13},
        {TJS_W("Esc"), 27}, {TJS_W("PgUp"), 33}, {TJS_W("PgDn"), 34},
        {TJS_W("Ins"), 45}, {TJS_W("Del"), 46}, {TJS_W("Num *"), 106},
        {TJS_W("Num +"), 107}, {TJS_W("Num -"), 109}, {TJS_W("Num ."), 110},
        {TJS_W("Num /"), 111}, {TJS_W(";"), 186}, {TJS_W("="), 187},
        {TJS_W(","), 188}, {TJS_W("-"), 189}, {TJS_W("."), 190},
        {TJS_W("/"), 191}, {TJS_W("`"), 192}, {TJS_W("["), 219},
        {TJS_W("\\"), 220}, {TJS_W("]"), 221}, {TJS_W("'"), 222}
    };
    for(const auto &alias : aliases) {
        tTJSVariant key(alias.key);
        ttstr name(alias.name);
        name.ToLowerCase();
        textToKeycode->PropSet(TJS_MEMBERENSURE, name.c_str(), NULL, &key, textToKeycode);
        tTJSVariant existing;
        keycodeToText->PropGetByNum(0, alias.key, &existing, keycodeToText);
        if(existing.Type() != tvtString) {
            tTJSVariant text(alias.name);
            keycodeToText->PropSetByNum(TJS_MEMBERENSURE, alias.key, &text, keycodeToText);
        }
    }
    cls->RegisterNCM(TJS_W("textToKeycode"), new MenuShortcutProperty(textToKeycode),
        TJS_W("MenuItem"), nitProperty, TJS_STATICMEMBER);
    cls->RegisterNCM(TJS_W("keycodeToText"), new MenuShortcutProperty(keycodeToText),
        TJS_W("MenuItem"), nitProperty, TJS_STATICMEMBER);
    return cls;
}

void TVPRegisterMenuPlugin()
{
    tTJS *engine = TVPGetScriptEngine();
    if(!engine) return;
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    tTJSVariant menuClass;
    if(TJS_SUCCEEDED(global->PropGet(0, TJS_W("__krkrnsNativeMenuClass"), NULL,
        &menuClass, global)) && menuClass.Type() == tvtObject && menuClass.AsObjectNoAddRef() &&
        menuClass.AsObjectNoAddRef()->IsValid(0, NULL, NULL, menuClass.AsObjectNoAddRef()) == TJS_S_TRUE) {
        TVPEnsureMenuPopupScript();
        return;
    }
    tTJSVariant windowClass;
    if(TJS_FAILED(global->PropGet(0, TJS_W("Window"), NULL, &windowClass, global)) ||
        windowClass.Type() != tvtObject || !windowClass.AsObjectNoAddRef())
        TVPThrowExceptionMessage(TVPInternalError, TJS_W("TVPRegisterMenuPlugin"));
    iTJSDispatch2 *cls = TVPCreateNativeClass_MenuItem();
    menuClass = tTJSVariant(cls);
    cls->Release();
    iTJSDispatch2 *property = new WindowMenuProperty();
    tTJSVariant menuProperty(property);
    property->Release();
    const tjs_uint32 flags = TJS_MEMBERENSURE | TJS_IGNOREPROP;
    global->PropSet(flags, TJS_W("__krkrnsNativeMenuClass"), NULL, &menuClass, global);
    global->PropSet(flags, TJS_W("__krkrnsNativeMenuProperty"), NULL, &menuProperty, global);
    global->PropSet(flags, TJS_W("MenuItem"), NULL, &menuClass, global);
    iTJSDispatch2 *window = windowClass.AsObjectNoAddRef();
    window->PropSet(flags, TJS_W("menu"), NULL, &menuProperty, window);
    // Native Window construction copies its class members. Apply an explicitly
    // linked menu.dll to windows which were already constructed as well.
    for(tjs_int i = 0; i < TVPGetWindowCount(); ++i) {
        iTJSDispatch2 *owner = TVPGetWindowListAt(i)->GetOwnerNoAddRef();
        if(owner) owner->PropSet(flags, TJS_W("menu"), NULL, &menuProperty, owner);
    }
    // Install the accelerator bridge when menu.dll is explicitly linked, even
    // if the title uses shortcuts before opening its first popup.
    TVPEnsureMenuPopupScript();
}

void TVPUnregisterMenuPlugin()
{
    tTJS *engine = TVPGetScriptEngine();
    if(!engine) return;
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    tTJSVariant menuClass, menuProperty;
    if(TJS_FAILED(global->PropGet(0, TJS_W("__krkrnsNativeMenuClass"), NULL,
        &menuClass, global))) return;
    global->PropGet(0, TJS_W("__krkrnsNativeMenuProperty"), NULL, &menuProperty, global);
    auto deleteIfSame = [](iTJSDispatch2 *owner, const tjs_char *name,
        const tTJSVariant &expected) {
        tTJSVariant value;
        if(expected.Type() == tvtObject && TJS_SUCCEEDED(owner->PropGet(TJS_IGNOREPROP,
            name, NULL, &value, owner)) && value.Type() == tvtObject &&
            value.AsObjectNoAddRef() == expected.AsObjectNoAddRef())
            owner->DeleteMember(0, name, NULL, owner);
    };
    deleteIfSame(global, TJS_W("MenuItem"), menuClass);
    tTJSVariant windowClass;
    if(TJS_SUCCEEDED(global->PropGet(0, TJS_W("Window"), NULL, &windowClass, global)) &&
        windowClass.Type() == tvtObject && windowClass.AsObjectNoAddRef())
        deleteIfSame(windowClass.AsObjectNoAddRef(), TJS_W("menu"), menuProperty);
    for(tjs_int i = 0; i < TVPGetWindowCount(); ++i) {
        iTJSDispatch2 *owner = TVPGetWindowListAt(i)->GetOwnerNoAddRef();
        if(owner) deleteIfSame(owner, TJS_W("menu"), menuProperty);
    }
    global->DeleteMember(0, TJS_W("__krkrnsNativeMenuProperty"), NULL, global);
    global->DeleteMember(0, TJS_W("__krkrnsNativeMenuClass"), NULL, global);
}

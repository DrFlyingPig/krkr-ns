// addFont.dll: System.addFont(filename).
//
// Ported from Kirikiroid2 (src/plugins/addFont.cpp).  The desktop plugin
// registers a private font file with the engine; here the file goes through
// the FreeType face list, which is what the rasterizer actually reads.
// Titles ship their own font and call this before drawing any text; without
// the symbol the call is a hard error in their boot script.
#include "ncbind/ncbind.hpp"
#include <vector>
#include <string>

#define NCB_MODULE_NAME TJS_W("addFont.dll")

// Implemented in visual/FreeType.cpp (tjs_string is the TJS character string).
extern bool TVPAddFontToFreeType( const ttstr& storage, std::vector<tjs_string>* faces );

struct FontEx
{
	/**
	 * プライベートフォントの追加
	 * @param fontFileName フォントファイル名
	 * @return void:ファイルを開くのに失敗 0:フォント登録に失敗 数値:登録したフォントの数
	 */
	static tjs_error TJS_INTF_METHOD addFont(tTJSVariant *result,
											 tjs_int numparams,
											 tTJSVariant **param,
											 iTJSDispatch2 *objthis) {
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;

		ttstr filename = TVPGetPlacedPath(*param[0]);
		if (filename.length()) {
			std::vector<tjs_string> faces;
			const bool ok = TVPAddFontToFreeType(filename, &faces);
			if (result) {
				*result = ok ? (int)faces.size() : 0;
			}
			return TJS_S_OK;
		}
		return TJS_S_OK;
	}
};

// フックつきアタッチ
NCB_ATTACH_CLASS(FontEx, System) {
	RawCallback("addFont", &FontEx::addFont, TJS_STATICMEMBER);
}

/* Anchor: a static archive drops translation units that nothing references, and
   the only reference to this one is the ncbind auto-register table.  PluginImpl
   calls this function before ncbAutoRegister::LoadModule. */
extern "C" void krkrsdl2_link_addfont_plugin() {}

// getSample.dll: WaveSoundBuffer.getSample() and the sampleValue family.
//
// Ported from Kirikiroid2 (src/plugins/getSample.cpp).  Titles use it for
// lip-sync (mouth animation driven by the playing voice) and waveform
// displays; several probe `WaveSoundBuffer.sampleValue` before enabling the
// feature.  The engine side (getVisBuffer / useVisBuffer) already exists in
// QueueSoundBufferImpl, so the whole plugin is the small wrapper below.
#include "ncbind/ncbind.hpp"
#include <algorithm>
#include <cstdlib>

#include "DebugIntf.h"

#define NCB_MODULE_NAME TJS_W("getSample.dll")

//------------------------------------------------------------------------------------------------
// Legacy interface (kept for compatibility)

/**
 * サンプル値の取得（旧方式）
 * 現在の再生位置から指定数のサンプルを取得してその平均値を返します。
 * 値が負のサンプル値は無視されます。
 * @param n 取得するサンプルの数。省略すると 100
 */
tjs_error
getSample(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis)
{
	tjs_error ret = TJS_S_OK;
	tjs_int n = numparams > 0 ? (tjs_int)*param[0] : 100;
	if (n > 0 && result) {
		short *buf = (short*)malloc(n * sizeof(*buf));
		if (buf) {
			tTJSVariant buffer     = (tTVInteger)buf;
			tTJSVariant numsamples = n;
			tTJSVariant channel    = 1;
			tTJSVariant *p[3] = {&buffer, &numsamples, &channel};
			if (TJS_SUCCEEDED(ret = objthis->FuncCall(0, TJS_W("getVisBuffer"), NULL, NULL, 3, p, objthis))) {
				int c = 0;
				int sum = 0;
				for (int i = 0; i < n; i++) {
					if (buf[i] >= 0) {
						sum += buf[i]; c++;
					}
				}
				*result = c > 0 ? sum / c : 0;
			}
			free(buf);
		}
	}
	return ret;
}

NCB_ATTACH_FUNCTION(getSample, WaveSoundBuffer, getSample);

//------------------------------------------------------------------------------------------------
// New-style extended properties / methods

class WaveSoundBufferAdd {
protected:
	iTJSDispatch2 *objthis; //< オブジェクト情報の参照
	int counts, aheads;
	tjs_uint32 hint;
	tTJSVariant vBuffer, vNumSamples, vChannel, vAheads, *params[4];
	short *buf;

	static int defaultCounts, defaultAheads;
public:
	WaveSoundBufferAdd(iTJSDispatch2 *objthis)
		:   objthis(objthis),
			counts(defaultCounts),
			aheads(defaultAheads),
			hint(0)
	{
		buf = new short[counts];
		// useVisBuffer = true; にする
		tTJSVariant val(1);
		tjs_error r = objthis->PropSet(0, TJS_W("useVisBuffer"), NULL, &val, objthis);
		if (r != TJS_S_OK)
			TVPAddLog(ttstr(TJS_W("useVisBuffer=1 failed: ")) + ttstr(r));

		// getVisBuffer用の引数を作る
		vBuffer     = (tTVInteger)buf;
		vChannel    = 1;
		vNumSamples = counts;
		vAheads     = aheads;
		params[0] = &vBuffer;
		params[1] = &vNumSamples;
		params[2] = &vChannel;
		params[3] = &vAheads;
	}
	~WaveSoundBufferAdd() {
		delete[] buf;
	}

	/**
	 * サンプル値の取得（新方式）
	 * getVisBuffer(buf, sampleCount, 1, sampleAhead)でサンプルを取得し，
	 * (value/32768)^2の最大値を取得します。(0～1の実数で返ります)
	 * ※このプロパティを読み出すと暗黙でuseVisBuffer=trueに設定されます
	 */
	double getSampleValue() {
		{
			// one-shot: confirms a title actually reaches the lip-sync path
			static bool logged = false;
			if (!logged) {
				logged = true;
				TVPAddLog(ttstr(TJS_W("[getSample] sampleValue first call counts=")) + ttstr(counts) +
					ttstr(TJS_W(" aheads=")) + ttstr(aheads));
			}
		}
		memset(buf, 0, counts * sizeof(short));
		tTJSVariant result;
		tjs_error r = objthis->FuncCall(0, TJS_W("getVisBuffer"), &hint, &result, 4, params, objthis);

		if (r != TJS_S_OK) TVPAddLog(ttstr(TJS_W("getVisBuffer failed: ")) + ttstr(r));

		int cnt = (int)result.AsInteger();
		if (cnt > counts || cnt < 0) cnt = counts;

		// サンプルの絶対値の最大値を返す
		double max = 0;
		int imax = 0;
		for (int i = cnt - 1; i >= 0; i--) {
			imax = std::max(imax, (int)std::abs(buf[i]));
		}
		max = (imax) / +32768.0;
		max *= max;
		return max;
	}

	/**
	 * バッファ取得用パラメータプロパティ（sampleValueを参照）
	 * デフォルトはsetDefaultCounts/setDefaultAheadsで決定されます
	 */
	int  getSampleCount() const  { return counts; }
	void setSampleCount(int cnt) {
		counts = cnt;
		delete[] buf;
		buf = new short[counts];
		vBuffer     = (tTVInteger)buf;
		vNumSamples = counts;
	}
	int  getSampleAhead() const  { return aheads; }
	void setSampleAhead(int ahd) {
		vAheads     = (aheads = ahd);
	}

	/**
	 * 新方式のデフォルトのパラメータ設定用関数
	 * 以降で生成されるWaveSoundBufferのインスタンスについて
	 * sampleCount/sampleAheadのデフォルト値を設定できます
	 */
	static void setDefaultCounts(int cnt) { defaultCounts = cnt; }
	static void setDefaultAheads(int ahd) { defaultAheads = ahd; }
};

int WaveSoundBufferAdd::defaultCounts = 100;
int WaveSoundBufferAdd::defaultAheads = 0;

// インスタンスゲッタ
NCB_GET_INSTANCE_HOOK(WaveSoundBufferAdd)
{
	NCB_INSTANCE_GETTER(objthis) { // objthis を iTJSDispatch2* 型の引数とする
		ClassT* obj = GetNativeInstance(objthis);	// ネイティブインスタンスポインタ取得
		if (!obj) {
			obj = new ClassT(objthis);				// ない場合は生成する
			SetNativeInstance(objthis, obj);		// objthis に obj をネイティブインスタンスとして登録する
		}
		return obj;
	}
};

// 登録
NCB_ATTACH_CLASS_WITH_HOOK(WaveSoundBufferAdd, WaveSoundBuffer) {
	Property(TJS_W("sampleValue"), &Class::getSampleValue, (int)0);
	Property(TJS_W("sampleCount"), &Class::getSampleCount, &Class::setSampleCount);
	Property(TJS_W("sampleAhead"), &Class::getSampleAhead, &Class::setSampleAhead);
}
NCB_ATTACH_FUNCTION(setDefaultCounts, WaveSoundBuffer, WaveSoundBufferAdd::setDefaultCounts);
NCB_ATTACH_FUNCTION(setDefaultAheads, WaveSoundBuffer, WaveSoundBufferAdd::setDefaultAheads);

/* Anchor: a static archive drops translation units that nothing references, and
   the only reference to this one is the ncbind auto-register table.  PluginImpl
   calls this function before ncbAutoRegister::LoadModule. */
extern "C" void krkrsdl2_link_getsample_plugin() {}

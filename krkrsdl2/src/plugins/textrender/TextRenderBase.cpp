#include "tjsCommHead.h"
#include "TextRenderBase.h"

#include "FreeTypeFontRasterizer.h"
#include "KrkrNSLog.h"


#include "tjsArray.h"
#include "tjsDictionary.h"
#include "tvpfontstruc.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
struct TextRenderState
{
	bool Bold = false;
	bool Italic = false;
	ttstr Face = TJS_W("user");
	tjs_int FontSize = 24;
	double FontScale = 1.0;
	tjs_uint32 Color = 0xffffff;
	tjs_int RubySize = 10;
	tjs_int RubyOffset = -2;
	bool Shadow = true;
	tjs_uint32 ShadowColor = 0x000000;
	bool Edge = false;
	tjs_uint32 EdgeColor = 0x0080ff;
	tjs_int LineSpacing = 6;
	tjs_int Pitch = 0;
	tjs_int LineSize = 0;
	tjs_int Align = -1;
	tjs_int VAlign = -1;
	bool RenderOver = false;
	tjs_int RenderDelay = 1000;
	ttstr RenderText;
};

struct CharacterInfo
{
	bool Bold = false;
	bool Italic = false;
	bool Graph = false;
	bool Vertical = false;
	ttstr Face;
	tjs_int X = 0;
	tjs_int Y = 0;
	tjs_int Width = 0;
	tjs_int Size = 0;
	tjs_uint32 Color = 0xffffff;
	bool HasEdge = false;
	tjs_uint32 EdgeColor = 0;
	bool HasShadow = false;
	tjs_uint32 ShadowColor = 0;
	ttstr Text;
};

static bool SameName(const tjs_char *left, const tjs_char *right)
{
	return TJS_strcmp(left, right) == 0;
}

static bool ReadMember(const tTJSVariant &source, const tjs_char *name,
	tTJSVariant &value)
{
	if (source.Type() != tvtObject) return false;
	tTJSVariant copy(source);
	tTJSVariantClosure &object = copy.AsObjectClosureNoAddRef();
	if (!object.Object) return false;
	return TJS_SUCCEEDED(object.PropGet(TJS_IGNOREPROP, name, nullptr, &value, nullptr)) &&
		value.Type() != tvtVoid;
}

static void ReadBool(const tTJSVariant &source, const tjs_char *name, bool &target)
{
	tTJSVariant value;
	if (ReadMember(source, name, value)) target = static_cast<tjs_int>(value) != 0;
}

static void ReadInt(const tTJSVariant &source, const tjs_char *name, tjs_int &target)
{
	tTJSVariant value;
	if (ReadMember(source, name, value)) target = static_cast<tjs_int>(value);
}

static void ReadColor(const tTJSVariant &source, const tjs_char *name,
	tjs_uint32 &target)
{
	tTJSVariant value;
	if (ReadMember(source, name, value))
		target = static_cast<tjs_uint32>(static_cast<tjs_int>(value));
}

static void ReadReal(const tTJSVariant &source, const tjs_char *name, double &target)
{
	tTJSVariant value;
	if (ReadMember(source, name, value)) target = static_cast<tjs_real>(value);
}

static void ReadString(const tTJSVariant &source, const tjs_char *name, ttstr &target)
{
	tTJSVariant value;
	if (ReadMember(source, name, value) && value.Type() == tvtString) target = ttstr(value);
}

static void ReadState(const tTJSVariant &source, TextRenderState &target)
{
	ReadBool(source, TJS_W("bold"), target.Bold);
	ReadBool(source, TJS_W("italic"), target.Italic);
	ReadString(source, TJS_W("face"), target.Face);
	ReadInt(source, TJS_W("fontsize"), target.FontSize);
	ReadInt(source, TJS_W("fontSize"), target.FontSize);
	ReadReal(source, TJS_W("fontscale"), target.FontScale);
	ReadReal(source, TJS_W("fontScale"), target.FontScale);
	ReadColor(source, TJS_W("chColor"), target.Color);
	ReadInt(source, TJS_W("rubySize"), target.RubySize);
	ReadInt(source, TJS_W("rubyOffset"), target.RubyOffset);
	ReadBool(source, TJS_W("shadow"), target.Shadow);
	ReadColor(source, TJS_W("shadowColor"), target.ShadowColor);
	ReadBool(source, TJS_W("edge"), target.Edge);
	ReadColor(source, TJS_W("edgeColor"), target.EdgeColor);
	ReadInt(source, TJS_W("lineSpacing"), target.LineSpacing);
	ReadInt(source, TJS_W("pitch"), target.Pitch);
	ReadInt(source, TJS_W("lineSize"), target.LineSize);
	ReadInt(source, TJS_W("align"), target.Align);
	ReadInt(source, TJS_W("valign"), target.VAlign);
}

static void SetDictionaryMember(iTJSDispatch2 *dictionary, const tjs_char *name,
	const tTJSVariant &value)
{
	dictionary->PropSet(TJS_MEMBERENSURE, name, nullptr, &value, dictionary);
}

static tTJSVariant SerializeCharacter(const CharacterInfo &ch)
{
	iTJSDispatch2 *dictionary = TJSCreateDictionaryObject();
	SetDictionaryMember(dictionary, TJS_W("bold"), static_cast<tjs_int>(ch.Bold));
	SetDictionaryMember(dictionary, TJS_W("italic"), static_cast<tjs_int>(ch.Italic));
	SetDictionaryMember(dictionary, TJS_W("graph"), static_cast<tjs_int>(ch.Graph));
	SetDictionaryMember(dictionary, TJS_W("vertical"), static_cast<tjs_int>(ch.Vertical));
	SetDictionaryMember(dictionary, TJS_W("x"), ch.X);
	SetDictionaryMember(dictionary, TJS_W("y"), ch.Y);
	SetDictionaryMember(dictionary, TJS_W("cw"), ch.Width);
	SetDictionaryMember(dictionary, TJS_W("size"), ch.Size);
	SetDictionaryMember(dictionary, TJS_W("face"), ch.Face);
	SetDictionaryMember(dictionary, TJS_W("color"), static_cast<tjs_int>(ch.Color));
	if (ch.HasEdge)
		SetDictionaryMember(dictionary, TJS_W("edge"), static_cast<tjs_int>(ch.EdgeColor));
	else
	{
		tTJSVariant empty;
		SetDictionaryMember(dictionary, TJS_W("edge"), empty);
	}
	if (ch.HasShadow)
		SetDictionaryMember(dictionary, TJS_W("shadow"), static_cast<tjs_int>(ch.ShadowColor));
	else
	{
		tTJSVariant empty;
		SetDictionaryMember(dictionary, TJS_W("shadow"), empty);
	}
	SetDictionaryMember(dictionary, TJS_W("text"), ch.Text);
	tTJSVariant result(dictionary, dictionary);
	dictionary->Release();
	return result;
}

class NI_TextRenderBase : public tTJSNativeInstance
{
	TextRenderState Default;
	TextRenderState State;
	FontRasterizer *Rasterizer = nullptr;
	std::vector<CharacterInfo> Characters;
	std::vector<CharacterInfo> Buffer;

	tjs_int BoxWidth = 0;
	tjs_int BoxHeight = 0;
	tjs_int CurrentX = 0;
	tjs_int CurrentY = 0;
	tjs_int Indent = 0;
	tjs_int AutoIndent = 0;
	tjs_int RenderLeft = 0;
	tjs_int RenderTop = 0;
	tjs_int RenderRight = 0;
	tjs_int RenderBottom = 0;
	bool BeginningOfLine = true;
	bool Vertical = false;

	FontRasterizer *GetRasterizer()
	{
		if (!Rasterizer) Rasterizer = new FreeTypeFontRasterizer();
		return Rasterizer;
	}

	void UpdateFont()
	{
		tTVPFont font;
		font.Height = std::max<tjs_int>(1, State.FontSize);
		font.Flags = (State.Bold ? TVP_TF_BOLD : 0) |
			(State.Italic ? TVP_TF_ITALIC : 0);
		font.Angle = Vertical ? 2700 : 0;
		font.Face = State.Face;
		GetRasterizer()->ApplyFont(font);
	}

	tjs_int Ascent()
	{
		UpdateFont();
		const tjs_int ascent = GetRasterizer()->GetAscentHeight();
		return ascent > 0 ? ascent : std::max<tjs_int>(1, State.FontSize);
	}

	void LineBreak()
	{
		CurrentX = State.Align == 1 ? BoxWidth - Indent : Indent;
		BeginningOfLine = true;
		const tjs_int lineHeight = Ascent() + State.LineSpacing;
		if (State.VAlign == 1)
		{
			CurrentY -= lineHeight;
			RenderTop = std::min(RenderTop, CurrentY);
		}
		else
		{
			CurrentY += lineHeight;
			RenderBottom = std::max(RenderBottom, CurrentY + Ascent());
		}
	}

	void Flush(bool force)
	{
		if (Buffer.empty()) return;

		if (State.Align == 0)
		{
			tjs_int total = 0;
			for (size_t i = 0; i < Buffer.size(); ++i)
				total += Buffer[i].Width + (i + 1 < Buffer.size() ? State.Pitch : 0);
			if (BoxWidth > 0 && total > BoxWidth && !force)
			{
				LineBreak();
				Flush(true);
				return;
			}
			tjs_int x = std::max<tjs_int>(0, (BoxWidth - total) / 2);
			for (auto &ch : Buffer)
			{
				ch.X = x;
				ch.Y = CurrentY;
				x += ch.Width + State.Pitch;
			}
			CurrentX = x;
			RenderLeft = 0;
			RenderRight = std::max(RenderRight, BoxWidth);
		}
		else if (State.Align == 1)
		{
			tjs_int x = CurrentX > 0 ? CurrentX : BoxWidth;
			for (auto &ch : Buffer)
			{
				tjs_int next = x - ch.Width;
				if (BoxWidth > 0 && next < 0)
				{
					LineBreak();
					x = CurrentX;
					next = x - ch.Width;
				}
				ch.X = next;
				ch.Y = CurrentY;
				x = next - State.Pitch;
				RenderLeft = std::min(RenderLeft, x);
			}
			CurrentX = x;
		}
		else
		{
			tjs_int x = CurrentX;
			for (auto &ch : Buffer)
			{
				tjs_int next = x + ch.Width + State.Pitch;
				if (BoxWidth > 0 && next > BoxWidth)
				{
					LineBreak();
					x = CurrentX;
					next = x + ch.Width + State.Pitch;
				}
				ch.X = x;
				ch.Y = CurrentY;
				x = next;
				RenderRight = std::max(RenderRight, x);
			}
			CurrentX = x;
		}

		RenderTop = std::min(RenderTop, CurrentY);
		RenderBottom = std::max(RenderBottom, CurrentY + Ascent());
		for (const auto &ch : Buffer)
		{
			Characters.push_back(ch);
			// KRKR-ns: renderText must grow as soon as a character is rendered,
			// not on flush.  KAGEX titles (永不枯萎 xmoe localisation) read the
			// renderText property right after render() to feed the backlog
			// (HistoryTextStore.storeRender); the krkrsdl3 reference only
			// accumulates on flush, which returned an empty string there.
			// The accumulation therefore happens in PushCharacter/PushGraph.
		}
		Buffer.clear();
	}

	void PushCharacter(tjs_char code)
	{
		UpdateFont();
		tjs_int width = 0, height = 0;
		GetRasterizer()->GetTextExtent(code, width, height);
		if (width <= 0) width = std::max<tjs_int>(1, State.FontSize / 2);

		CharacterInfo info;
		info.Bold = State.Bold;
		info.Italic = State.Italic;
		info.Vertical = Vertical;
		info.Face = State.Face;
		info.Width = width;
		info.Size = State.FontSize;
		info.Color = State.Color;
		info.HasEdge = State.Edge;
		info.EdgeColor = State.EdgeColor;
		info.HasShadow = State.Shadow;
		info.ShadowColor = State.ShadowColor;
		info.Text = ttstr(&code, 1);
		State.RenderText += info.Text;
		Buffer.push_back(info);
		BeginningOfLine = false;
	}

	void PushGraph(const ttstr &name)
	{
		CharacterInfo info;
		info.Bold = State.Bold;
		info.Italic = State.Italic;
		info.Graph = true;
		info.Face = State.Face;
		info.Width = std::max<tjs_int>(1, State.FontSize);
		info.Size = State.FontSize;
		info.Color = State.Color;
		info.Text = name;
		State.RenderText += name;
		Buffer.push_back(info);
		BeginningOfLine = false;
	}

	static bool ReadNumber(const ttstr &text, tjs_uint &position, tjs_int &value)
	{
		bool negative = false;
		bool any = false;
		value = 0;
		for (; position < text.length(); ++position)
		{
			const tjs_char ch = text[position];
			if (ch >= TJS_W('0') && ch <= TJS_W('9'))
			{
				value = value * 10 + ch - TJS_W('0');
				any = true;
			}
			else if (ch == TJS_W('-')) negative = !negative;
			else if (ch == TJS_W(';')) break;
			else return false;
		}
		if (negative) value = -value;
		return any || (position < text.length() && text[position] == TJS_W(';'));
	}

	static ttstr ReadUntilSemicolon(const ttstr &text, tjs_uint &position)
	{
		ttstr value;
		for (; position < text.length() && text[position] != TJS_W(';'); ++position)
			value += text[position];
		return value;
	}

public:
	NI_TextRenderBase() { Clear(); }
	~NI_TextRenderBase() override { Invalidate(); }

	void TJS_INTF_METHOD Invalidate() override
	{
		if (Rasterizer)
		{
			Rasterizer->Release();
			Rasterizer = nullptr;
		}
	}

	void Clear()
	{
		Characters.clear();
		Buffer.clear();
		State = Default;
		State.RenderText = TJS_W("");
		State.RenderOver = false;
		CurrentX = 0;
		Indent = 0;
		BeginningOfLine = true;
		if (State.VAlign == 1)
		{
			CurrentY = std::max<tjs_int>(0, BoxHeight - State.FontSize);
			RenderTop = RenderBottom = BoxHeight;
		}
		else if (State.VAlign == 0)
		{
			// TextRender's 0 means centered. Treating it as top-aligned puts
			// choice captions above their frames. Match the upstream single-line
			// layout using the ascent of this instance's selected font.
			CurrentY = (BoxHeight - Ascent()) / 2;
			RenderTop = RenderBottom = 0;
		}
		else
		{
			CurrentY = 0;
			RenderTop = RenderBottom = 0;
		}
		RenderLeft = State.Align == 1 ? BoxWidth : 0;
		RenderRight = RenderLeft;
		UpdateFont();
	}

	bool Render(const ttstr &text, tjs_int autoIndent, tjs_int, tjs_int, bool)
	{
		AutoIndent = autoIndent;
		for (tjs_uint i = 0; i < text.length(); ++i)
		{
			const tjs_char ch = text[i];
			if (ch == TJS_W('%') && i + 1 < text.length())
			{
				const tjs_char command = text[++i];
				if (command == TJS_W('t') || command == TJS_W('f'))
				{
					++i;
					State.Face = ReadUntilSemicolon(text, i);
					UpdateFont();
					continue;
				}
				if (command == TJS_W('b') || command == TJS_W('i') ||
					command == TJS_W('s') || command == TJS_W('e'))
				{
					const bool enabled = i + 1 < text.length() && text[++i] == TJS_W('1');
					if (command == TJS_W('b')) State.Bold = enabled;
					if (command == TJS_W('i')) State.Italic = enabled;
					if (command == TJS_W('s')) State.Shadow = enabled;
					if (command == TJS_W('e')) State.Edge = enabled;
					UpdateFont();
					continue;
				}
				if (command == TJS_W('r'))
				{
					State = Default;
					UpdateFont();
					continue;
				}
				if (command == TJS_W('n') || command == TJS_W('p') ||
					command == TJS_W('d') || command == TJS_W('w') ||
					(command >= TJS_W('0') && command <= TJS_W('9')))
				{
					tjs_int value = command >= TJS_W('0') && command <= TJS_W('9')
						? command - TJS_W('0') : 0;
					++i;
					ReadNumber(text, i, value);
					if (command == TJS_W('n'))
					{
						Flush(true);
						for (tjs_int line = 0; line < std::max<tjs_int>(1, value); ++line)
							LineBreak();
					}
					else if (command == TJS_W('p')) State.Pitch = value;
					else if (command >= TJS_W('0') && command <= TJS_W('9'))
					{
						State.FontSize = std::max<tjs_int>(1, Default.FontSize * value / 100);
						UpdateFont();
					}
					continue;
				}
				if (command == TJS_W('l') || command == TJS_W('D'))
				{
					++i;
					ReadUntilSemicolon(text, i);
					continue;
				}
				// Alignment/style commands not implemented by the upstream portable
				// plug-in are harmless controls rather than printable characters.
				if (command == TJS_W('B') || command == TJS_W('S') ||
					command == TJS_W('C') || command == TJS_W('R') || command == TJS_W('L'))
					continue;
				PushCharacter(command);
				continue;
			}

			if (ch == TJS_W('\\') && i + 1 < text.length())
			{
				const tjs_char escaped = text[++i];
				if (escaped == TJS_W('n')) { Flush(true); LineBreak(); }
				else if (escaped == TJS_W('t') || escaped == TJS_W('w')) PushCharacter(TJS_W(' '));
				else if (escaped == TJS_W('i')) Indent = CurrentX;
				else if (escaped == TJS_W('r')) Indent = 0;
				else if (escaped != TJS_W('k') && escaped != TJS_W('x')) PushCharacter(escaped);
				continue;
			}
			if (ch == TJS_W('#'))
			{
				tjs_uint32 color = 0;
				bool any = false;
				while (++i < text.length() && text[i] != TJS_W(';'))
				{
					const tjs_char digit = text[i];
					tjs_uint32 value;
					if (digit >= TJS_W('0') && digit <= TJS_W('9')) value = digit - TJS_W('0');
					else if (digit >= TJS_W('a') && digit <= TJS_W('f')) value = 10 + digit - TJS_W('a');
					else if (digit >= TJS_W('A') && digit <= TJS_W('F')) value = 10 + digit - TJS_W('A');
					else continue;
					color = (color << 4) | value;
					any = true;
				}
				State.Color = any ? color : 0xffffff;
				continue;
			}
			if (ch == TJS_W('&'))
			{
				++i;
				PushGraph(ReadUntilSemicolon(text, i));
				continue;
			}
			if (ch == TJS_W('$'))
			{
				++i;
				ReadUntilSemicolon(text, i);
				continue;
			}
			if (ch == TJS_W('['))
			{
				while (i + 1 < text.length() && text[i + 1] != TJS_W(']')) ++i;
				if (i + 1 < text.length()) ++i;
				continue;
			}
			if (ch == TJS_W('\r')) continue;
			if (ch == TJS_W('\n')) { Flush(true); LineBreak(); continue; }
			PushCharacter(ch);
		}

		Flush(true);
		State.RenderOver = BoxHeight > 0 && CurrentY > BoxHeight;
		return !State.RenderOver;
	}

	void SetRenderSize(tjs_int width, tjs_int height)
	{
		BoxWidth = std::max<tjs_int>(0, width);
		BoxHeight = std::max<tjs_int>(0, height);
		Clear();
	}

	void SetDefault(const tTJSVariant &settings) { ReadState(settings, Default); }
	void SetOption(const tTJSVariant &) {}
	void Done() { Flush(true); }
	void ResetFont() { State = Default; UpdateFont(); }
	void ResetStyle() { State = Default; UpdateFont(); }

	tTJSVariant GetCharacters(tjs_int start, tjs_int end)
	{
		Flush(true);
		iTJSDispatch2 *array = TJSCreateArrayObject();
		size_t first = 0;
		size_t last = Characters.size();
		if (!(start == 0 && end == 0) && end >= start)
		{
			first = std::min<size_t>(std::max<tjs_int>(0, start), Characters.size());
			last = std::min<size_t>(std::max<tjs_int>(0, end), Characters.size());
		}
		for (size_t source = first, index = 0; source < last; ++source, ++index)
		{
			tTJSVariant value = SerializeCharacter(Characters[source]);
			array->PropSetByNum(TJS_MEMBERENSURE, static_cast<tjs_int>(index), &value, array);
		}
		tTJSVariant result(array, array);
		array->Release();
		return result;
	}

	tTJSVariant GetProperty(const tjs_char *name)
	{
		if (SameName(name, TJS_W("keyWait")))
		{
			iTJSDispatch2 *array = TJSCreateArrayObject();
			tTJSVariant value(array, array);
			array->Release();
			return value;
		}
		if (SameName(name, TJS_W("renderCount"))) return static_cast<tjs_int>(State.RenderText.length());
		if (SameName(name, TJS_W("renderText"))) return State.RenderText;
		if (SameName(name, TJS_W("renderLeft"))) return RenderLeft;
		if (SameName(name, TJS_W("renderRight"))) return RenderRight;
		if (SameName(name, TJS_W("renderTop"))) return RenderTop;
		if (SameName(name, TJS_W("renderBottom"))) return RenderBottom;
		if (SameName(name, TJS_W("renderOver"))) return static_cast<tjs_int>(State.RenderOver);
		if (SameName(name, TJS_W("renderDelay"))) return State.RenderDelay;
		if (SameName(name, TJS_W("vertical"))) return static_cast<tjs_int>(Vertical);
		if (SameName(name, TJS_W("bold"))) return static_cast<tjs_int>(State.Bold);
		if (SameName(name, TJS_W("italic"))) return static_cast<tjs_int>(State.Italic);
		if (SameName(name, TJS_W("face"))) return State.Face;
		if (SameName(name, TJS_W("fontSize"))) return State.FontSize;
		if (SameName(name, TJS_W("fontScale"))) return State.FontScale;
		if (SameName(name, TJS_W("chColor"))) return static_cast<tjs_int>(State.Color);
		if (SameName(name, TJS_W("rubySize"))) return State.RubySize;
		if (SameName(name, TJS_W("rubyOffset"))) return State.RubyOffset;
		if (SameName(name, TJS_W("shadow"))) return static_cast<tjs_int>(State.Shadow);
		if (SameName(name, TJS_W("shadowColor"))) return static_cast<tjs_int>(State.ShadowColor);
		if (SameName(name, TJS_W("edge"))) return static_cast<tjs_int>(State.Edge);
		if (SameName(name, TJS_W("edgeColor"))) return static_cast<tjs_int>(State.EdgeColor);
		if (SameName(name, TJS_W("lineSpacing"))) return State.LineSpacing;
		if (SameName(name, TJS_W("pitch"))) return State.Pitch;
		if (SameName(name, TJS_W("lineSize"))) return State.LineSize;
		if (SameName(name, TJS_W("align"))) return State.Align;
		if (SameName(name, TJS_W("valign"))) return State.VAlign;
		if (SameName(name, TJS_W("defaultBold"))) return static_cast<tjs_int>(Default.Bold);
		if (SameName(name, TJS_W("defaultItalic"))) return static_cast<tjs_int>(Default.Italic);
		if (SameName(name, TJS_W("defaultFace"))) return Default.Face;
		if (SameName(name, TJS_W("defaultFontSize"))) return Default.FontSize;
		if (SameName(name, TJS_W("defaultFontScale"))) return Default.FontScale;
		if (SameName(name, TJS_W("defaultChColor"))) return static_cast<tjs_int>(Default.Color);
		if (SameName(name, TJS_W("defaultRubySize"))) return Default.RubySize;
		if (SameName(name, TJS_W("defaultRubyOffset"))) return Default.RubyOffset;
		if (SameName(name, TJS_W("defaultShadow"))) return static_cast<tjs_int>(Default.Shadow);
		if (SameName(name, TJS_W("defaultShadowColor"))) return static_cast<tjs_int>(Default.ShadowColor);
		if (SameName(name, TJS_W("defaultEdge"))) return static_cast<tjs_int>(Default.Edge);
		if (SameName(name, TJS_W("defaultEdgeColor"))) return static_cast<tjs_int>(Default.EdgeColor);
		if (SameName(name, TJS_W("defaultLineSpacing"))) return Default.LineSpacing;
		if (SameName(name, TJS_W("defaultPitch"))) return Default.Pitch;
		if (SameName(name, TJS_W("defaultLineSize"))) return Default.LineSize;
		if (SameName(name, TJS_W("defaultAlign"))) return Default.Align;
		if (SameName(name, TJS_W("defaultValign"))) return Default.VAlign;
		return tTJSVariant();
	}

	void SetProperty(const tjs_char *name, const tTJSVariant &value)
	{
		const tjs_int integer = static_cast<tjs_int>(value);
		if (SameName(name, TJS_W("renderOver"))) State.RenderOver = integer != 0;
		else if (SameName(name, TJS_W("renderDelay"))) State.RenderDelay = integer;
		else if (SameName(name, TJS_W("renderText"))) State.RenderText = ttstr(value);
		else if (SameName(name, TJS_W("vertical"))) Vertical = integer != 0;
		else if (SameName(name, TJS_W("bold"))) State.Bold = integer != 0;
		else if (SameName(name, TJS_W("italic"))) State.Italic = integer != 0;
		else if (SameName(name, TJS_W("face"))) State.Face = ttstr(value);
		else if (SameName(name, TJS_W("fontSize"))) State.FontSize = integer;
		else if (SameName(name, TJS_W("fontScale"))) State.FontScale = static_cast<tjs_real>(value);
		else if (SameName(name, TJS_W("chColor"))) State.Color = static_cast<tjs_uint32>(integer);
		else if (SameName(name, TJS_W("rubySize"))) State.RubySize = integer;
		else if (SameName(name, TJS_W("rubyOffset"))) State.RubyOffset = integer;
		else if (SameName(name, TJS_W("shadow"))) State.Shadow = integer != 0;
		else if (SameName(name, TJS_W("shadowColor"))) State.ShadowColor = static_cast<tjs_uint32>(integer);
		else if (SameName(name, TJS_W("edge"))) State.Edge = integer != 0;
		else if (SameName(name, TJS_W("edgeColor"))) State.EdgeColor = static_cast<tjs_uint32>(integer);
		else if (SameName(name, TJS_W("lineSpacing"))) State.LineSpacing = integer;
		else if (SameName(name, TJS_W("pitch"))) State.Pitch = integer;
		else if (SameName(name, TJS_W("lineSize"))) State.LineSize = integer;
		else if (SameName(name, TJS_W("align"))) State.Align = integer;
		else if (SameName(name, TJS_W("valign"))) State.VAlign = integer;
		else if (SameName(name, TJS_W("defaultBold"))) Default.Bold = integer != 0;
		else if (SameName(name, TJS_W("defaultItalic"))) Default.Italic = integer != 0;
		else if (SameName(name, TJS_W("defaultFace"))) Default.Face = ttstr(value);
		else if (SameName(name, TJS_W("defaultFontSize"))) Default.FontSize = integer;
		else if (SameName(name, TJS_W("defaultFontScale"))) Default.FontScale = static_cast<tjs_real>(value);
		else if (SameName(name, TJS_W("defaultChColor"))) Default.Color = static_cast<tjs_uint32>(integer);
		else if (SameName(name, TJS_W("defaultRubySize"))) Default.RubySize = integer;
		else if (SameName(name, TJS_W("defaultRubyOffset"))) Default.RubyOffset = integer;
		else if (SameName(name, TJS_W("defaultShadow"))) Default.Shadow = integer != 0;
		else if (SameName(name, TJS_W("defaultShadowColor"))) Default.ShadowColor = static_cast<tjs_uint32>(integer);
		else if (SameName(name, TJS_W("defaultEdge"))) Default.Edge = integer != 0;
		else if (SameName(name, TJS_W("defaultEdgeColor"))) Default.EdgeColor = static_cast<tjs_uint32>(integer);
		else if (SameName(name, TJS_W("defaultLineSpacing"))) Default.LineSpacing = integer;
		else if (SameName(name, TJS_W("defaultPitch"))) Default.Pitch = integer;
		else if (SameName(name, TJS_W("defaultLineSize"))) Default.LineSize = integer;
		else if (SameName(name, TJS_W("defaultAlign"))) Default.Align = integer;
		else if (SameName(name, TJS_W("defaultValign"))) Default.VAlign = integer;
		UpdateFont();
	}

	tjs_int CharacterCount() const { return static_cast<tjs_int>(Characters.size()); }

	};

static iTJSNativeInstance *TJS_INTF_METHOD Create_NI_TextRenderBase()
{
	KRKRNS_LOG("[textrender] TextRenderBase instance created");
	return new NI_TextRenderBase();
}

} // namespace

#ifdef TJS_NATIVE_CLASSID_NAME
#undef TJS_NATIVE_CLASSID_NAME
#undef TJS_NCM_REG_THIS
#undef TJS_NATIVE_SET_ClassID
#endif
#define TJS_NCM_REG_THIS classobj
#define TJS_NATIVE_SET_ClassID TJS_NATIVE_CLASSID_NAME = TJS_NCM_CLASSID;
#define TJS_NATIVE_CLASSID_NAME ClassID_TextRenderBase
static tjs_int32 TJS_NATIVE_CLASSID_NAME = -1;

tTJSNativeClass *TVPCreateNativeClass_TextRenderBase()
{
	tTJSNativeClassForPlugin *classobj =
		TJSCreateNativeClassForPlugin(TJS_W("TextRenderBase"), Create_NI_TextRenderBase);

	TJS_BEGIN_NATIVE_MEMBERS(TextRenderBase)
		TJS_DECL_EMPTY_FINALIZE_METHOD

		TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(_this, NI_TextRenderBase, TextRenderBase)
		{
			return TJS_S_OK;
		}
		TJS_END_NATIVE_CONSTRUCTOR_DECL(TextRenderBase)

		TJS_BEGIN_NATIVE_METHOD_DECL(render)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			if (numparams < 1) return TJS_E_BADPARAMCOUNT;
			const ttstr text(*param[0]);
			const tjs_int autoIndent = TJS_PARAM_EXIST(1) ? static_cast<tjs_int>(*param[1]) : 0;
			const tjs_int diff = TJS_PARAM_EXIST(2) ? static_cast<tjs_int>(*param[2]) : 0;
			const tjs_int all = TJS_PARAM_EXIST(3) ? static_cast<tjs_int>(*param[3]) : 0;
			const bool same = TJS_PARAM_EXIST(4) && static_cast<tjs_int>(*param[4]) != 0;
			if (result) *result = static_cast<tjs_int>(_this->Render(text, autoIndent, diff, all, same));
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(render)

		TJS_BEGIN_NATIVE_METHOD_DECL(setRenderSize)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			if (numparams < 2) return TJS_E_BADPARAMCOUNT;
			_this->SetRenderSize(static_cast<tjs_int>(*param[0]), static_cast<tjs_int>(*param[1]));
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(setRenderSize)

		TJS_BEGIN_NATIVE_METHOD_DECL(setDefault)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			if (numparams < 1) return TJS_E_BADPARAMCOUNT;
			_this->SetDefault(*param[0]);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(setDefault)

		TJS_BEGIN_NATIVE_METHOD_DECL(setOption)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			if (numparams < 1) return TJS_E_BADPARAMCOUNT;
			_this->SetOption(*param[0]);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(setOption)

		TJS_BEGIN_NATIVE_METHOD_DECL(getCharacters)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			const tjs_int start = TJS_PARAM_EXIST(0) ? static_cast<tjs_int>(*param[0]) : 0;
			const tjs_int end = TJS_PARAM_EXIST(1) ? static_cast<tjs_int>(*param[1]) : 0;
			if (result) *result = _this->GetCharacters(start, end);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(getCharacters)

		TJS_BEGIN_NATIVE_METHOD_DECL(clear)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			_this->Clear();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(clear)

		TJS_BEGIN_NATIVE_METHOD_DECL(done)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			_this->Done();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(done)

		TJS_BEGIN_NATIVE_METHOD_DECL(resetFont)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			_this->ResetFont();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(resetFont)

		TJS_BEGIN_NATIVE_METHOD_DECL(resetStyle)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			_this->ResetStyle();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(resetStyle)

		TJS_BEGIN_NATIVE_METHOD_DECL(getKeyWait)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			if (result) *result = _this->GetProperty(TJS_W("keyWait"));
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(getKeyWait)

		TJS_BEGIN_NATIVE_METHOD_DECL(calcShowCount)
		{
			TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase);
			if (result) *result = _this->CharacterCount();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_METHOD_DECL(calcShowCount)

#define REGISTER_TEXT_RENDER_PROPERTY(name) \
		TJS_BEGIN_NATIVE_PROP_DECL(name) \
		{ \
			TJS_BEGIN_NATIVE_PROP_GETTER \
			{ \
				TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase); \
				if (result) *result = _this->GetProperty(TJS_W(#name)); \
				return TJS_S_OK; \
			} \
			TJS_END_NATIVE_PROP_GETTER \
			TJS_BEGIN_NATIVE_PROP_SETTER \
			{ \
				TJS_GET_NATIVE_INSTANCE(_this, NI_TextRenderBase); \
				_this->SetProperty(TJS_W(#name), *param); \
				return TJS_S_OK; \
			} \
			TJS_END_NATIVE_PROP_SETTER \
		} \
		TJS_END_NATIVE_PROP_DECL(name)

		REGISTER_TEXT_RENDER_PROPERTY(keyWait)
		REGISTER_TEXT_RENDER_PROPERTY(renderCount)
		REGISTER_TEXT_RENDER_PROPERTY(renderOver)
		REGISTER_TEXT_RENDER_PROPERTY(renderDelay)
		REGISTER_TEXT_RENDER_PROPERTY(renderText)
		REGISTER_TEXT_RENDER_PROPERTY(renderLeft)
		REGISTER_TEXT_RENDER_PROPERTY(renderRight)
		REGISTER_TEXT_RENDER_PROPERTY(renderTop)
		REGISTER_TEXT_RENDER_PROPERTY(renderBottom)
		REGISTER_TEXT_RENDER_PROPERTY(vertical)
		REGISTER_TEXT_RENDER_PROPERTY(bold)
		REGISTER_TEXT_RENDER_PROPERTY(italic)
		REGISTER_TEXT_RENDER_PROPERTY(face)
		REGISTER_TEXT_RENDER_PROPERTY(fontSize)
		REGISTER_TEXT_RENDER_PROPERTY(fontScale)
		REGISTER_TEXT_RENDER_PROPERTY(chColor)
		REGISTER_TEXT_RENDER_PROPERTY(rubySize)
		REGISTER_TEXT_RENDER_PROPERTY(rubyOffset)
		REGISTER_TEXT_RENDER_PROPERTY(shadow)
		REGISTER_TEXT_RENDER_PROPERTY(shadowColor)
		REGISTER_TEXT_RENDER_PROPERTY(edge)
		REGISTER_TEXT_RENDER_PROPERTY(edgeColor)
		REGISTER_TEXT_RENDER_PROPERTY(lineSpacing)
		REGISTER_TEXT_RENDER_PROPERTY(pitch)
		REGISTER_TEXT_RENDER_PROPERTY(lineSize)
		REGISTER_TEXT_RENDER_PROPERTY(align)
		REGISTER_TEXT_RENDER_PROPERTY(valign)
		REGISTER_TEXT_RENDER_PROPERTY(defaultBold)
		REGISTER_TEXT_RENDER_PROPERTY(defaultItalic)
		REGISTER_TEXT_RENDER_PROPERTY(defaultFace)
		REGISTER_TEXT_RENDER_PROPERTY(defaultFontSize)
		REGISTER_TEXT_RENDER_PROPERTY(defaultFontScale)
		REGISTER_TEXT_RENDER_PROPERTY(defaultChColor)
		REGISTER_TEXT_RENDER_PROPERTY(defaultRubySize)
		REGISTER_TEXT_RENDER_PROPERTY(defaultRubyOffset)
		REGISTER_TEXT_RENDER_PROPERTY(defaultShadow)
		REGISTER_TEXT_RENDER_PROPERTY(defaultShadowColor)
		REGISTER_TEXT_RENDER_PROPERTY(defaultEdge)
		REGISTER_TEXT_RENDER_PROPERTY(defaultEdgeColor)
		REGISTER_TEXT_RENDER_PROPERTY(defaultLineSpacing)
		REGISTER_TEXT_RENDER_PROPERTY(defaultPitch)
		REGISTER_TEXT_RENDER_PROPERTY(defaultLineSize)
		REGISTER_TEXT_RENDER_PROPERTY(defaultAlign)
		REGISTER_TEXT_RENDER_PROPERTY(defaultValign)

#undef REGISTER_TEXT_RENDER_PROPERTY
	TJS_END_NATIVE_MEMBERS

	return classobj;
}

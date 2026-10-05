`ScriptsEx.cpp` ports krkrsdl3's `plugins/scriptsEx.cpp` to this engine.  The
original source is present in this workspace at
`.zcode/upstream-krkrsdl3/plugins/scriptsEx.cpp`; the same plugin ships with
several titles under `sdmc:/switch/KRKR-ns/Game/*/plugin/scriptsEx.dll`.

Linking `scriptsEx.dll` attaches the `ScriptsAdd` class to `Scripts`:

| member | contract |
|---|---|
| `getObjectKeys(object)` | sorted key list of a dictionary |
| `getObjectCount(object)` | `GetCount()` of the object (dictionary members) |
| `getObjectContext(object)` / `isNullContext(object)` | object context, null test |
| `equalStruct(v1, v2)` / `equalStructNumericLoose(v1, v2)` | deep structural comparison, the loose variant treats ints and reals as equal |
| `foreach(object, func, ...)` | walks array elements or dictionary keys; a non-`void` return from the callback stops the walk |
| `getMD5HashString(octet)` | MD5 of an octet, from the engine's `md5.h` |
| `clone(object)` | deep copy |
| `propSet(obj, key, value, flags)` / `propGet(obj, key, flags)` | property access with the published `pf*` flag constants, by name or index |
| `safeEvalStorage(name, mode, context)` | reads a storage and evaluates it wrapped in `(const)[...]`, returning element 0 |
| `stringFuzzySearch(text, pattern, maxIgnore, chBits)` | bitap search returning the match offset or -1 |
| `rehash(dictionary)` | rehash in place |

Two port notes:

- The include block follows this tree: krkrsdl3's plugins include their
  platform's `PluginImpl.h` (as `TVPPlugin.h`) for `TVPDoTryBlock` /
  `tTVPExceptionDesc`, and this port keeps that plus `TextStream.h` and the
  engine's `md5.h`.  `TJS_N` (a narrow literal in krkrsdl3) is `TJS_W` here.
- `safeEvalStorage` no longer builds its expression through
  `tTJSString::AppendBuffer` + `memmove`.  That hand-built buffer relies on the
  string's exact length/capacity contract, which this engine's `tTJSString` does
  not share: the expression came out malformed and raised a syntax error.  The
  reference file already carries the equivalent simple `content +=` form in a
  comment, and the port uses that.

The storage body format matters for callers.  The wrapper is `(const)[<body>]`,
so the body must be a *constant* expression: a const dictionary spells its pairs
with `,` or `=>` (never the `:` of a normal dictionary literal), a scalar body
comes back as element 0 itself, and anything the constant grammar rejects raises
a syntax error.  The fixture asserts each of those cases.

`compat-patches/system/k2compat.tjs` used to publish script-level
`Scripts.getObjectKeys/getObjectValues/hasObjectKey` unconditionally; those
assignments are now guarded so a title that links the real plugin keeps the
plugin's implementation.

The included `LICENSE.krkrsdl3` is copied unchanged from krkrsdl3's root license
and applies to this adaptation.  `scriptsEx` itself derives from the
wamsoft/scriptsEx project referenced in the upstream header.

Verification: `tests/fixtures/scripts_ex/startup.tjs` on the real ARM engine
(43 checks: file probe, registration, every attachment, deep comparison and
clone semantics, foreach, property access by name and index, fuzzy search
offsets, rehash, and the four `safeEvalStorage` body forms).

In-game usage: scanning the XP3 script bytecode of the titles that link this
plugin (`tools/xp3_find_symbol.py`) finds `safeEvalStorage` (2-4 references) and
`getObjectContext` (2-3) in 魔女的夜宴, 千恋万花 v1.1 and 9-nine; the same scan
shows those titles' KAG layer code resolving `Scripts` members by name.  Five
sessions across those titles ran with the plugin linked and no member-related
script exception.

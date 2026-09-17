# KRKR-ns 上游差异清单（UPSTREAM_DELTA）

> 生成时间：2026-09-15 16:04 · 生成方式：`tools/upstream_delta.sh > docs/UPSTREAM_DELTA.md`（换行符不敏感）
> 只统计源码；第三方 vendored 目录（krkrz submodule 内容、zlib/SDL/FAudio/simde 等）不参与对比。

## 引擎层：`krkrsdl2/` vs krkrsdl2/krkrsdl2 @ bf207f2

### 我们的修改（25 个文件，按改动行数排序）

| 改动行数 | 文件 |
|---|---|
| 2609 | `src/core/sdl2/SDLApplication.cpp` |
| 1286 | `data/startup.tjs` |
| 446 | `src/core/visual/sdl2/VideoOvlImpl.cpp` |
| 199 | `src/core/base/sdl2/PluginImpl.cpp` |
| 154 | `src/core/base/sdl2/StorageImpl.cpp` |
| 138 | `src/core/sdl2/SDLBitmapCompletion.cpp` |
| 136 | `src/core/base/sdl2/SysInitImpl.cpp` |
| 128 | `src/core/environ/sdl2/Application.cpp` |
| 120 | `CMakeLists.txt` |
| 92 | `src/core/base/sdl2/SystemImpl.cpp` |
| 82 | `src/core/base/sdl2/NativeEventQueue.cpp` |
| 65 | `src/core/visual/sdl2/BitmapBitsAlloc.cpp` |
| 64 | `src/core/visual/sdl2/BasicDrawDevice.cpp` |
| 41 | `src/core/visual/sdl2/VideoOvlImpl.h` |
| 32 | `src/core/visual/win32/krmovie.h` |
| 16 | `src/core/visual/sdl2/WindowImpl.cpp` |
| 13 | `src/core/base/sdl2/EventImpl.cpp` |
| 11 | `src/core/visual/sdl2/TVPScreen.cpp` |
| 11 | `src/core/sdl2/SDLEntrypoint.cpp` |
| 8 | `src/core/base/sdl2/NativeEventQueue.h` |
| 6 | `src/core/sound/sdl2/FAudioDevice.cpp` |
| 6 | `src/core/environ/sdl2/ApplicationSpecialPath.h` |
| 5 | `src/core/visual/sdl2/BitmapBitsAlloc.h` |
| 4 | `src/core/sdl2/SDLApplication.h` |
| 2 | `src/config/src_list/kirikirisdl2/sources.txt` |

### 我们的新增（非第三方）

- `data/notosanssc.ttf`
- `src/core/base/sdl2/7zip/`
- `src/core/base/sdl2/SevenZipArchive.cpp`
- `src/core/base/sdl2/SevenZipArchive.h`
- `src/core/base/sdl2/XP3ExtractionFilter.cpp`
- `src/core/sdl2/BitmapBufferPool.h`
- `src/core/sdl2/BufferedWrite.h`
- `src/core/sdl2/GLComposite.cpp`
- `src/core/sdl2/GLCompositeBridge.h`
- `src/core/sdl2/KrkrNSLog.h`
- `src/core/sdl2/KrkrNSPaths.h`
- `src/core/sdl2/KrkrNSProf.h`
- `src/core/sdl2/KrkrNSSlowOperation.h`
- `src/core/sdl2/SDLBitmapBridge.h`
- `src/core/sdl2/SharedByteView.h`
- `src/core/sdl2/SharedMemoryStream.h`
- `src/core/sound/sdl2/VorbisWaveDecoder.cpp`
- `src/core/sound/sdl2/VorbisWaveDecoder.h`
- `src/core/visual/sdl2/SwitchMovieOverlay.cpp`
- `src/core/visual/sdl2/SwitchMovieOverlay.h`
- `src/plugins/PluginStub.h`
- `src/plugins/addfont/`
- `src/plugins/csvparser/`
- `src/plugins/dirlist/`
- `src/plugins/emoteplayer/`
- `src/plugins/fftgraph/`
- `src/plugins/getabout/`
- `src/plugins/getsample/`
- `src/plugins/kagparser/`
- `src/plugins/layerexbtoa/`
- `src/plugins/ncbind/`
- `src/plugins/psbfile/`
- `src/plugins/savestruct/`
- `src/plugins/textrender/`
- `src/plugins/varfile/`
- `src/plugins/win32dialog/`
- `src/plugins/wutcwf/`

## 内嵌引擎层：`krkrsdl2/external/krkrz/` vs krkrsdl2/krkrz @ b11c43a

### 我们的修改（36 个文件，按改动行数排序）

| 改动行数 | 文件 |
|---|---|
| 805 | `base/StorageIntf.cpp` |
| 756 | `base/ScriptMgnIntf.cpp` |
| 341 | `visual/LayerIntf.cpp` |
| 188 | `visual/GraphicsLoaderIntf.cpp` |
| 143 | `visual/LayerBitmapIntf.cpp` |
| 114 | `visual/FreeType.cpp` |
| 112 | `base/TextStream.cpp` |
| 99 | `utils/ThreadIntf.cpp` |
| 47 | `visual/LayerManager.cpp` |
| 45 | `tjs2/tjsInterCodeExec.cpp` |
| 41 | `tjs2/tjsArray.cpp` |
| 32 | `visual/gl/ResampleImage.cpp` |
| 30 | `tjs2/tjsVariantString.cpp` |
| 28 | `visual/WindowIntf.cpp` |
| 26 | `tjs2/tjsInterCodeGen.cpp` |
| 25 | `tjs2/tjsUtils.h` |
| 23 | `sound/OpusCodecDecoder.cpp` |
| 22 | `base/EventIntf.cpp` |
| 21 | `sound/QueueSoundBufferImpl.cpp` |
| 19 | `visual/TransIntf.cpp` |
| 19 | `sound/SoundPlayer.cpp` |
| 15 | `visual/LayerIntf.h` |
| 14 | `base/XP3Archive.cpp` |
| 10 | `base/SysInitIntf.cpp` |
| 9 | `utils/DebugIntf.cpp` |
| 8 | `visual/GraphicsLoaderIntf.h` |
| 8 | `sound/WaveIntf.cpp` |
| 6 | `visual/GraphicsLoadThread.cpp` |
| 6 | `visual/FreeType.h` |
| 5 | `tjs2/tjs.h` |
| 5 | `base/XP3Archive.h` |
| 4 | `visual/FreeTypeFontRasterizer.cpp` |
| 4 | `tjs2/tjsVariantString.h` |
| 4 | `base/StorageIntf.h` |
| 3 | `visual/LayerBitmapImpl.cpp` |
| 3 | `base/EventIntf.h` |

### 我们的新增（非第三方）

- `utils/gbk2unicode.c`
- `visual/PagedCharacterCache.h`

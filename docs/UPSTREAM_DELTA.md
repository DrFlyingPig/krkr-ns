# KRKR-ns 上游差异清单（UPSTREAM_DELTA）

> 生成时间：2026-10-01 13:06 · 生成方式：`tools/upstream_delta.sh > docs/UPSTREAM_DELTA.md`（换行符不敏感）
> 只统计源码；第三方 vendored 目录（krkrz submodule 内容、zlib/SDL/FAudio/simde 等）不参与对比。

## 引擎层：`krkrsdl2/` vs krkrsdl2/krkrsdl2 @ bf207f2

### 我们的修改（25 个文件，按改动行数排序）

| 改动行数 | 文件 |
|---|---|
| 3564 | `src/core/sdl2/SDLApplication.cpp` |
| 1209 | `data/startup.tjs` |
| 606 | `src/core/visual/sdl2/VideoOvlImpl.cpp` |
| 239 | `src/core/base/sdl2/PluginImpl.cpp` |
| 154 | `src/core/base/sdl2/StorageImpl.cpp` |
| 138 | `src/core/sdl2/SDLBitmapCompletion.cpp` |
| 136 | `src/core/base/sdl2/SysInitImpl.cpp` |
| 130 | `CMakeLists.txt` |
| 128 | `src/core/environ/sdl2/Application.cpp` |
| 92 | `src/core/base/sdl2/SystemImpl.cpp` |
| 82 | `src/core/base/sdl2/NativeEventQueue.cpp` |
| 81 | `src/core/visual/sdl2/BasicDrawDevice.cpp` |
| 65 | `src/core/visual/sdl2/BitmapBitsAlloc.cpp` |
| 52 | `src/core/visual/sdl2/VideoOvlImpl.h` |
| 32 | `src/core/visual/win32/krmovie.h` |
| 18 | `src/core/visual/sdl2/WindowImpl.cpp` |
| 15 | `src/core/base/sdl2/EventImpl.cpp` |
| 11 | `src/core/visual/sdl2/TVPScreen.cpp` |
| 11 | `src/core/sdl2/SDLEntrypoint.cpp` |
| 8 | `src/core/base/sdl2/NativeEventQueue.h` |
| 6 | `src/core/sound/sdl2/FAudioDevice.cpp` |
| 6 | `src/core/environ/sdl2/ApplicationSpecialPath.h` |
| 5 | `src/core/visual/sdl2/BitmapBitsAlloc.h` |
| 4 | `src/core/sdl2/SDLApplication.h` |
| 4 | `src/config/src_list/kirikirisdl2/sources.txt` |

### 我们的新增（非第三方）

- `data/launcher/`
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
- `src/core/sdl2/LauncherArtworkImage.cpp`
- `src/core/sdl2/LauncherArtworkRaster.h`
- `src/core/sdl2/LauncherArtworkStorage.cpp`
- `src/core/sdl2/LauncherArtworkStorage.h`
- `src/core/sdl2/LauncherArtworkTLG.h`
- `src/core/sdl2/SDLBitmapBridge.h`
- `src/core/sdl2/SharedByteView.h`
- `src/core/sdl2/SharedMemoryStream.h`
- `src/core/sound/sdl2/FFWaveDecoder.cpp`
- `src/core/sound/sdl2/FFWaveDecoder.h`
- `src/core/sound/sdl2/VorbisWaveDecoder.cpp`
- `src/core/sound/sdl2/VorbisWaveDecoder.h`
- `src/core/visual/sdl2/MovieAudioQueue.h`
- `src/core/visual/sdl2/SwitchMovieOverlay.cpp`
- `src/core/visual/sdl2/SwitchMovieOverlay.h`
- `src/plugins/PluginStub.h`
- `src/plugins/addfont/`
- `src/plugins/alphamovie/`
- `src/plugins/csvparser/`
- `src/plugins/dirlist/`
- `src/plugins/emoteplayer/`
- `src/plugins/fftgraph/`
- `src/plugins/getabout/`
- `src/plugins/getsample/`
- `src/plugins/kagparser/`
- `src/plugins/layerexbtoa/`
- `src/plugins/layerexraster/`
- `src/plugins/ncbind/`
- `src/plugins/psbfile/`
- `src/plugins/savestruct/`
- `src/plugins/textrender/`
- `src/plugins/varfile/`
- `src/plugins/win32dialog/`
- `src/plugins/wutcwf/`

## 内嵌引擎层：`krkrsdl2/external/krkrz/` vs krkrsdl2/krkrz @ b11c43a

### 我们的修改（46 个文件，按改动行数排序）

| 改动行数 | 文件 |
|---|---|
| 1526 | `base/StorageIntf.cpp` |
| 891 | `base/ScriptMgnIntf.cpp` |
| 810 | `visual/LayerIntf.cpp` |
| 188 | `visual/GraphicsLoaderIntf.cpp` |
| 159 | `visual/LayerBitmapIntf.cpp` |
| 159 | `utils/ThreadIntf.cpp` |
| 145 | `visual/TransIntf.cpp` |
| 144 | `tjs2/tjsInterCodeExec.cpp` |
| 142 | `visual/FreeType.cpp` |
| 141 | `base/TextStream.cpp` |
| 106 | `visual/WindowIntf.cpp` |
| 75 | `visual/FreeTypeFontRasterizer.cpp` |
| 62 | `base/XP3Archive.cpp` |
| 47 | `visual/LayerManager.cpp` |
| 41 | `tjs2/tjsArray.cpp` |
| 40 | `sound/SoundPlayer.cpp` |
| 39 | `sound/QueueSoundBufferImpl.cpp` |
| 37 | `base/EventIntf.cpp` |
| 33 | `visual/LayerIntf.h` |
| 32 | `visual/gl/ResampleImage.cpp` |
| 30 | `tjs2/tjsVariantString.cpp` |
| 30 | `sound/SoundBufferBaseImpl.cpp` |
| 26 | `tjs2/tjsInterCodeGen.cpp` |
| 25 | `tjs2/tjsUtils.h` |
| 23 | `sound/OpusCodecDecoder.cpp` |
| 21 | `base/XP3Archive.h` |
| 18 | `utils/DebugIntf.cpp` |
| 17 | `base/SystemIntf.cpp` |
| 16 | `sound/WaveIntf.cpp` |
| 15 | `visual/LayerBitmapImpl.cpp` |
| 11 | `tjs2/tjs.h` |
| 10 | `base/SysInitIntf.cpp` |
| 9 | `tjs2/tjsRegExp.cpp` |
| 8 | `visual/GraphicsLoaderIntf.h` |
| 6 | `visual/GraphicsLoadThread.cpp` |
| 6 | `visual/FreeType.h` |
| 5 | `base/TextStream.h` |
| 5 | `base/EventIntf.h` |
| 4 | `tjs2/tjsVariantString.h` |
| 4 | `base/StorageIntf.h` |
| 3 | `utils/TimerThread.cpp` |
| 3 | `sound/SoundEventThread.cpp` |
| 2 | `visual/LayerBitmapImpl.h` |
| 2 | `visual/FreeTypeFontRasterizer.h` |
| 2 | `tjs2/tjs.cpp` |
| 2 | `sound/SoundBufferBaseImpl.h` |

### 我们的新增（非第三方）

- `utils/gbk2unicode.c`
- `visual/MenuItemImpl.cpp`
- `visual/MenuItemImpl.h`
- `visual/MenuItemIntf.cpp`
- `visual/MenuItemIntf.h`
- `visual/PagedCharacterCache.h`

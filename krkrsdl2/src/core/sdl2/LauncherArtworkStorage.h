/* SPDX-License-Identifier: MIT */
#pragma once

#include "tjsCommHead.h"

// Launcher-only artwork: scans are explicit; normal library navigation only
// reads the saved metadata and the launcher's own thumbnail cache.
namespace krkrns_launcher_artwork {

// Each returned dispatch object has one reference owned by its caller.
iTJSDispatch2 *ScanGameArtwork(const ttstr &folder, const ttstr &sourceMode);
iTJSDispatch2 *GetGameArtworkChoices();
bool SetGameArtworkChoice(const ttstr &folder, const ttstr &role,
                          const ttstr &thumbnailPath);

// Implemented in LauncherArtworkImage.cpp. Roles: preview, avatar, grid.
ttstr MakeGameArtworkThumbnail(const ttstr &folder, const ttstr &source,
                              const ttstr &role);

// Validation does no filesystem I/O. Only this game's loose/XP3 images and
// images inside the global Artwork directory are valid sources.
bool ValidateArtworkSource(const ttstr &folder, const ttstr &source);
// Explicit scans/thumbnail generation only: bounds the XP3 index before the
// core parser runs. A member also validates that image's segment allocations.
void ValidateArtworkArchiveIndex(const ttstr &archivePath,
                                 const ttstr &member = ttstr());
// Ensures the launcher-owned directories exist and returns a native SD path
// ending in '/'. Throws on failure. No game directory is read or changed.
ttstr ArtworkCacheDirectory();
bool IsLauncherArtworkMode();

} // namespace krkrns_launcher_artwork

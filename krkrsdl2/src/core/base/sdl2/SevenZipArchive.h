#ifndef SevenZipArchiveH
#define SevenZipArchiveH

#include "tjsCommHead.h"

class tTVPArchive;

// Some commercial KRKR games ship a standard 7-Zip container with an .xp3
// suffix and mount it through Storages.addAutoPath.  Kirikiroid-compatible
// runtimes therefore treat the archive format independently from its suffix.
bool TVPIs7ZArchive(const ttstr &name);
tTVPArchive *TVPOpen7ZArchive(const ttstr &name, tTJSBinaryStream *stream);

#endif

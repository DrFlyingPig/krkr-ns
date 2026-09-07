# Embedded 7-Zip decoder

These decoder files are the minimal 7-Zip/LZMA SDK subset used by
`SevenZipArchive.cpp`. They were imported from the `krkrsdl3` Kirikiroid2
plug-in at commit `5a8bd422f82d3758045f403520a64b772a59f40c`.

The imported source files identify Igor Pavlov's LZMA SDK code as public
domain. Only archive parsing plus Copy/LZMA/LZMA2/filter decoding sources are
compiled; encoder and unrelated container sources are intentionally omitted.

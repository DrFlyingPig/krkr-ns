/* SPDX-License-Identifier: MIT */
/* KRKR-ns: single source of truth for the on-SD folder layout.
 *
 * Everything lives in one folder next to the NRO:
 *   KRKR-ns/krkrsdl2.nro   the executable
 *   KRKR-ns/Game/<dir>/    games (one directory per game)
 *   KRKR-ns/saves/<dir>/   save data, isolated per game directory
 *   KRKRNS_BASE_A "/patch/system/"  user overrides for the bundled compat layer
 *   KRKR-ns/log/           per-boot timestamped debug logs
 *   KRKR-ns/*.txt          runtime marker files (see README)
 *
 * KRKRNS_BASE_A is a narrow string literal (fopen/snprintf sites);
 * KRKRNS_BASE_L is the wide literal (ttstr/TJS sites).  Both concatenate
 * with following string literals:  fopen(KRKRNS_BASE_A "/saves", ...).
 */
#pragma once

#define KRKRNS_BASE_A "sdmc:/switch/KRKR-ns"
#define KRKRNS_BASE_L L"sdmc:/switch/KRKR-ns"
/* Matches TJS_W's encoding (tjsTypes.h: TJS_W(X) is u##X in the active
 * branch), so it concatenates with TJS_W("...") suffixes. */
#define KRKRNS_BASE_U u"sdmc:/switch/KRKR-ns"

/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_fontmap.h - per-game font family substitutions shared by OS back ends.
 */
#ifndef BGI_OS_FONTMAP_H_
#define BGI_OS_FONTMAP_H_
#include "bgi/common.h"

/* Resolve an engine face name (Shift-JIS) through fontmap.json.
 *
 * On success, `path` is a native path to the font file, `familyUtf8` is the
 * font's actual family name (used by GDI; ignored by the FreeType back end),
 * and `faceIndex` is the face index inside a collection (normally 0).
 * Paths in fontmap.json are UTF-8 and relative to the directory containing
 * that fontmap.json. Returns 1 for a matching, existing file; otherwise 0.
 */
int OsFontMap_Resolve(const char* requestedFace, char* path, size_t pathSize,
                      char* familyUtf8, size_t familySize, int* faceIndex);

#endif /* BGI_OS_FONTMAP_H_ */

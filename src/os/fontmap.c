/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fontmap.c - platform-independent reader for per-game font substitutions.
 * The platform back ends still load and rasterise the selected font.
 */
#include "bgi/os.h"
#include "bgi/json.h"
#include "bgi/os_fontmap.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONTMAP_FILE       "fontmap.json"
#define FONTMAP_MAX_BYTES  (256u * 1024u)
#define FONTMAP_PATH_MAX   1024
#define FONTMAP_FAMILY_MAX 256

typedef struct FontMapState
{
	JsonNode_t* document;
	char gameDir[FONTMAP_PATH_MAX];
	char binaryDir[FONTMAP_PATH_MAX];
	char baseDir[FONTMAP_PATH_MAX];
	int loaded;
} FontMapState_t;
static FontMapState_t gMap;

/* Fold ASCII only. UTF-8 non-ASCII bytes remain exact, which is important
 * for full-width Japanese family names. */
static int EqualFaceName(const char* a, const char* b)
{
	while(*a && *b)
	{
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if(ca < 0x80)
			ca = (unsigned char)tolower(ca);
		if(cb < 0x80)
			cb = (unsigned char)tolower(cb);
		if(ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static int JoinNative(char* out, size_t n, const char* dir, const char* leaf)
{
	int written;
	size_t len;
	if(!out || !n)
		return 0;
	out[0] = 0;
	if(!dir || !*dir)
	{
		written = snprintf(out, n, "%s", leaf ? leaf : "");
		return written >= 0 && (size_t)written < n;
	}
	written = snprintf(out, n, "%s", dir);
	if(written < 0 || (size_t)written >= n)
		return 0;
	len = (size_t)written;
	if(len && out[len - 1] != '/' && out[len - 1] != '\\')
	{
		if(len + 1 >= n)
			return 0;
		out[len++] = OS_NATIVE_SEP;
		out[len] = 0;
	}
	if(leaf && *leaf)
	{
		written = snprintf(out + len, n - len, "%s", leaf);
		if(written < 0 || (size_t)written >= n - len)
			return 0;
	}
	return 1;
}

static int IsAbsoluteNative(const char* path)
{
	if(!path || !*path)
		return 0;
#if defined(_WIN32)
	return path[0] == '/' || path[0] == '\\' ||
	       (isalpha((unsigned char)path[0]) && path[1] == ':');
#else
	return path[0] == '/';
#endif
}

static JsonNode_t* ReadDocument(const char* path)
{
	FILE* f = OS_NativeOpen(path, "rb");
	long length;
	char* text;
	JsonNode_t* doc;
	if(!f)
		return NULL;
	if(fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0 ||
	   (unsigned long)length > FONTMAP_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0)
	{
		fclose(f);
		return NULL;
	}
	text = (char*)malloc((size_t)length + 1);
	if(!text)
	{
		fclose(f);
		return NULL;
	}
	if(length && fread(text, 1, (size_t)length, f) != (size_t)length)
	{
		free(text);
		fclose(f);
		return NULL;
	}
	fclose(f);
	text[length] = 0;
	doc = Json_Parse(text, (size_t)length, NULL);
	free(text);
	if(!doc || !Json_Get(doc, "fonts") || Json_Get(doc, "fonts")->type != JSON_ARRAY)
	{
		Json_Free(doc);
		return NULL;
	}
	return doc;
}

static int TryDirectory(const char* dir)
{
	char mapPath[FONTMAP_PATH_MAX];
	JsonNode_t* doc;
	if(!dir || !*dir)
		return 0;
	if(!JoinNative(mapPath, sizeof mapPath, dir, FONTMAP_FILE))
		return 0;
	doc = ReadDocument(mapPath);
	if(!doc)
		return 0;
	gMap.document = doc;
	snprintf(gMap.baseDir, sizeof gMap.baseDir, "%s", dir);
	return 1;
}

/* The launcher can change the game directory after displaying its own UI.
 * Reload the map when either search root changes. */
static void EnsureLoaded(void)
{
	char gameDir[FONTMAP_PATH_MAX] = "";
	char binaryDir[FONTMAP_PATH_MAX] = "";
	if(gMap.loaded)
	{
		OS_GameDir(gameDir, sizeof gameDir);
		OS_BinaryDir(binaryDir, sizeof binaryDir);
		if(strcmp(gameDir, gMap.gameDir) == 0 && strcmp(binaryDir, gMap.binaryDir) == 0)
			return;
	}
	Json_Free(gMap.document);
	gMap.document = NULL;
	gMap.baseDir[0] = 0;
	OS_GameDir(gameDir, sizeof gameDir);
	OS_BinaryDir(binaryDir, sizeof binaryDir);
	snprintf(gMap.gameDir, sizeof gMap.gameDir, "%s", gameDir);
	snprintf(gMap.binaryDir, sizeof gMap.binaryDir, "%s", binaryDir);
	gMap.loaded = 1;
	/* A game-local fontmap wins over the executable-local default. */
	if(TryDirectory(gameDir))
		return;
	if(strcmp(gameDir, binaryDir) != 0)
		TryDirectory(binaryDir);
}

static int EntryMatches(const JsonNode_t* entry, const char* faceUtf8)
{
	const JsonNode_t* match = Json_Get(entry, "match");
	const JsonNode_t* alias;
	if(!match)
		return 0;
	if(match->type == JSON_STRING)
		return match->str && EqualFaceName(match->str, faceUtf8);
	if(match->type == JSON_ARRAY)
	{
		for(alias = match->child; alias; alias = alias->next)
			if(alias->type == JSON_STRING && alias->str && EqualFaceName(alias->str, faceUtf8))
				return 1;
	}
	return 0;
}

int OsFontMap_Resolve(const char* requestedFace, char* path, size_t pathSize,
                      char* familyUtf8, size_t familySize, int* faceIndex)
{
	char faceUtf8[0x400];
	const JsonNode_t* fonts;
	const JsonNode_t* entry;
	if(path && pathSize)
		path[0] = 0;
	if(familyUtf8 && familySize)
		familyUtf8[0] = 0;
	if(faceIndex)
		*faceIndex = 0;
	if(!requestedFace || !path || pathSize == 0 || !familyUtf8 || familySize == 0)
		return 0;
	if(OS_SjisToUtf8(requestedFace, faceUtf8, sizeof faceUtf8) == 0 || !faceUtf8[0])
		return 0;
	EnsureLoaded();
	if(!gMap.document)
		return 0;
	fonts = Json_Get(gMap.document, "fonts");
	for(entry = fonts ? fonts->child : NULL; entry; entry = entry->next)
	{
		const char* file;
		const char* family;
		char nativeFile[FONTMAP_PATH_MAX];
		FILE* check;
		double index;
		if(entry->type != JSON_OBJECT || !EntryMatches(entry, faceUtf8))
			continue;
		file = Json_String(entry, "file", NULL);
		if(!file || !*file)
			continue;
		OS_Utf8ToNative(file, nativeFile, sizeof nativeFile);
		if(IsAbsoluteNative(nativeFile))
		{
			int written = snprintf(path, pathSize, "%s", nativeFile);
			if(written < 0 || (size_t)written >= pathSize)
				continue;
		}
		else if(!JoinNative(path, pathSize, gMap.baseDir, nativeFile))
			continue;
		check = OS_NativeOpen(path, "rb");
		if(!check)
		{
			path[0] = 0;
			continue;
		}
		fclose(check);
		family = Json_String(entry, "family", faceUtf8);
		if(!family || !*family)
			family = faceUtf8;
		{
			int written = snprintf(familyUtf8, familySize, "%s", family);
			if(written < 0 || (size_t)written >= familySize)
				continue;
		}
		index = Json_Number(entry, "index", 0);
		if(faceIndex && index >= 0 && index <= 255)
			*faceIndex = (int)index;
		return 1;
	}
	return 0;
}

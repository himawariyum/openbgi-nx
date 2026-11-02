/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.c - Switch back end: FreeType fonts (inc/bgi/os.h)
 *
 * The engine asks for GDI fonts by Shift-JIS face name, a cell height and
 * the average (half-width) character width, and expects each character
 * drawn at the top-left corner of the buffer as TextOut() would: the cell
 * top at row 0, the baseline at the font's ascent, full-width glyphs twice
 * the given width.  Font files come from the game (OS_FontAddFile /
 * OS_FontAddMemory), romfs:/fonts or sdmc:/switch/openbgi/fonts; the face
 * name picks a serif file for a Mincho request and a sans file otherwise.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_switch.h"
#include "bgi/json.h"

#include <ctype.h>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SIZES_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

// ---- shared faces -----------------------------------------------------------

typedef struct FaceRec
{
	struct FaceRec* next;
	char path[1024];
	int index;
	FT_Face face;
	int refs;
} FaceRec_t;

#define FALLBACK_MAX  4
#define FALLBACK_MISS 8

typedef struct Fallback
{
	FaceRec_t* rec;
	FT_Size size;
} Fallback_t;

struct OsFont
{
	FaceRec_t* rec;
	FT_Size size;
	Fallback_t fb[FALLBACK_MAX];
	int fbCount;
	uint32_t fbMiss[FALLBACK_MISS];
	int fbMissCount;
	int height, width;
	int bold, italic, oblique, serif;
	int ascent;
	FT_F26Dot6 emW, emH;
	FT_Pos boldStrength;
};

static FT_Library gFt;
static FaceRec_t* gFaces;

// SWITCH_FONTMAP_PATCH_BEGIN
// Per-game font substitutions.  The Switch renderer already uses FreeType,
// so a mapping only changes the font file/face index selected by ResolveFace.
// No SDL renderer or additional devkitPro portlib is needed.
#define SWITCH_FONTMAP_FILE       "fontmap.json"
#define SWITCH_FONTMAP_MAX_BYTES  (256u * 1024u)
#define SWITCH_FONTMAP_PATH_MAX   1024
static JsonNode_t* gSwitchFontMap;
static char gSwitchFontMapRoot[SWITCH_FONTMAP_PATH_MAX];
static char gSwitchFontMapBase[SWITCH_FONTMAP_PATH_MAX];
static int gSwitchFontMapLoaded;
// SWITCH_FONTMAP_STATE_END


// font files the game made available, searched before the standard folders
#define SWITCH_FONT_FILES 8
static char gFontFiles[SWITCH_FONT_FILES][1024];
static int gFontFileCount;

static int FtInit(void)
{
	if(gFt)
		return 1;
	return FT_Init_FreeType(&gFt) == 0;
}

void OsSwitch_FontShutdown(void)
{
	if(gFaces)
		return;
	Json_Free(gSwitchFontMap);
	gSwitchFontMap = NULL;
	gSwitchFontMapRoot[0] = 0;
	gSwitchFontMapBase[0] = 0;
	gSwitchFontMapLoaded = 0;
	if(gFt)
	{
		FT_Done_FreeType(gFt);
		gFt = NULL;
	}
}

// ---- locating a font file ---------------------------------------------------

static int ContainsNoCase(const char* s, const char* sub)
{
	size_t n = strlen(sub);
	for(; *s; s++)
		if(strncasecmp(s, sub, n) == 0)
			return 1;
	return 0;
}

static int IsMinchoName(const char* utf8)
{
	if(strstr(utf8, "\xe6\x98\x8e\xe6\x9c\x9d") != NULL) // 明朝
		return 1;
	return ContainsNoCase(utf8, "mincho") || ContainsNoCase(utf8, "serif");
}

static int FindFontFile(const char* dir, const char* name, int depth, char* out, size_t n)
{
	DIR* d = opendir(dir);
	struct dirent* e;
	int found = 0;
	if(!d)
		return 0;
	while(!found && (e = readdir(d)) != NULL)
	{
		char path[1024];
		struct stat st;
		if(e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if(stat(path, &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode))
		{
			if(depth < 4)
				found = FindFontFile(path, name, depth + 1, out, n);
		}
		else if(strcasecmp(e->d_name, name) == 0)
		{
			snprintf(out, n, "%s", path);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

// any .ttf / .otf / .ttc below `dir`, for game data with unusual names
static int FindAnyFont(const char* dir, int depth, char* out, size_t n)
{
	DIR* d = opendir(dir);
	struct dirent* e;
	int found = 0;
	if(!d)
		return 0;
	while(!found && (e = readdir(d)) != NULL)
	{
		char path[1024];
		struct stat st;
		const char* dot;
		if(e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if(stat(path, &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode))
		{
			if(depth < 4)
				found = FindAnyFont(path, depth + 1, out, n);
			continue;
		}
		dot = strrchr(e->d_name, '.');
		if(dot && (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 || strcasecmp(dot, ".ttc") == 0))
		{
			snprintf(out, n, "%s", path);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

static const char* const kGothicFiles[] = {"NotoSansJP-Regular.otf", "NotoSansCJKjp-Regular.otf", "NotoSansCJK-Regular.ttc",
	"ipag.ttf", "ipagp.ttf", "TakaoGothic.ttf", "VL-Gothic-Regular.ttf", "DejaVuSans.ttf", NULL};
static const char* const kMinchoFiles[] = {"NotoSerifJP-Regular.otf", "NotoSerifCJKjp-Regular.otf", "NotoSerifCJK-Regular.ttc",
	"ipam.ttf", "ipamp.ttf", "TakaoMincho.ttf", "DejaVuSerif.ttf", NULL};

static const char* const kFontDirs[] = {"romfs:/fonts", "sdmc:/switch/openbgi/fonts", "sdmc:/switch", NULL};

static int FontMapIsAbsolute(const char* path)
{
	return path && (strncmp(path, "romfs:/", 7) == 0 || strncmp(path, "sdmc:/", 6) == 0);
}

static int FontMapJoin(char* out, size_t n, const char* dir, const char* leaf)
{
	int written;
	size_t len;
	if(!out || n == 0 || !dir || !leaf)
		return 0;
	written = snprintf(out, n, "%s", dir);
	if(written < 0 || (size_t)written >= n)
		return 0;
	len = (size_t)written;
	if(len && out[len - 1] != '/')
	{
		if(len + 1 >= n)
			return 0;
		out[len++] = '/';
		out[len] = 0;
	}
	written = snprintf(out + len, n - len, "%s", leaf);
	if(written < 0 || (size_t)written >= n - len)
		return 0;
	return 1;
}

static int FontMapLoadFromRoot(const char* root)
{
	char configPath[SWITCH_FONTMAP_PATH_MAX];
	FILE* f;
	long length;
	char* text;
	JsonNode_t* doc;
	const JsonNode_t* fonts;
	if(!FontMapJoin(configPath, sizeof configPath, root, SWITCH_FONTMAP_FILE))
		return 0;
	f = fopen(configPath, "rb");
	if(!f)
		return 0;
	if(fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0 ||
	   (unsigned long)length > SWITCH_FONTMAP_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0)
	{
		fclose(f);
		return 0;
	}
	text = (char*)malloc((size_t)length + 1);
	if(!text)
	{
		fclose(f);
		return 0;
	}
	if(length && fread(text, 1, (size_t)length, f) != (size_t)length)
	{
		free(text);
		fclose(f);
		return 0;
	}
	fclose(f);
	text[length] = 0;
	doc = Json_Parse(text, (size_t)length, NULL);
	free(text);
	fonts = doc ? Json_Get(doc, "fonts") : NULL;
	if(!fonts || fonts->type != JSON_ARRAY)
	{
		Json_Free(doc);
		return 0;
	}
	gSwitchFontMap = doc;
	snprintf(gSwitchFontMapBase, sizeof gSwitchFontMapBase, "%s", root);
	return 1;
}

/* Prefer a game-local map.  The SD fallback makes it possible to keep a
 * fontmap.json and replacement fonts in /switch/openbgi/ when the game is
 * supplied from RomFS.  Reload when the selected game root changes. */
static void FontMapEnsureLoaded(void)
{
	char gameRoot[SWITCH_FONTMAP_PATH_MAX];
	static const char kSdConfigRoot[] = "sdmc:/switch/openbgi/";
	OsSwitch_GameRoot(gameRoot, sizeof gameRoot);
	if(gSwitchFontMapLoaded && strcmp(gameRoot, gSwitchFontMapRoot) == 0)
		return;
	Json_Free(gSwitchFontMap);
	gSwitchFontMap = NULL;
	gSwitchFontMapBase[0] = 0;
	snprintf(gSwitchFontMapRoot, sizeof gSwitchFontMapRoot, "%s", gameRoot);
	gSwitchFontMapLoaded = 1;
	if(FontMapLoadFromRoot(gameRoot))
		return;
	if(strcmp(gameRoot, kSdConfigRoot) != 0)
		FontMapLoadFromRoot(kSdConfigRoot);
}

/* Compare face names case-insensitively for ASCII characters, but preserve
 * exact UTF-8 bytes for non-ASCII Japanese family names. */
static int FontMapNameEqual(const char* a, const char* b)
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

static int FontMapEntryMatches(const JsonNode_t* entry, const char* requestedUtf8)
{
	const JsonNode_t* match = Json_Get(entry, "match");
	const JsonNode_t* alias;
	if(!match)
		return 0;
	if(match->type == JSON_STRING)
		return match->str && FontMapNameEqual(match->str, requestedUtf8);
	if(match->type == JSON_ARRAY)
	{
		for(alias = match->child; alias; alias = alias->next)
			if(alias->type == JSON_STRING && alias->str && FontMapNameEqual(alias->str, requestedUtf8))
				return 1;
	}
	return 0;
}

/* Resolve a matching map entry to a Switch-native font path and collection
 * index.  A missing/invalid file is ignored, leaving normal font discovery
 * as the fallback. */
static int FontMapResolve(const char* sjisFace, char* out, size_t outSize, int* index)
{
	char requestedUtf8[0x400];
	const JsonNode_t* fonts;
	const JsonNode_t* entry;
	if(!sjisFace || !out || outSize == 0 || !index)
		return 0;
	out[0] = 0;
	*index = 0;
	OsSwitch_SjisToUtf8(sjisFace, requestedUtf8, sizeof requestedUtf8);
	if(!requestedUtf8[0])
		return 0;
	FontMapEnsureLoaded();
	if(!gSwitchFontMap)
		return 0;
	fonts = Json_Get(gSwitchFontMap, "fonts");
	for(entry = fonts ? fonts->child : NULL; entry; entry = entry->next)
	{
		const char* file;
		char candidate[SWITCH_FONTMAP_PATH_MAX];
		struct stat st;
		double faceIndex;
		if(entry->type != JSON_OBJECT || !FontMapEntryMatches(entry, requestedUtf8))
			continue;
		file = Json_String(entry, "file", NULL);
		if(!file || !*file)
			continue;
		if(FontMapIsAbsolute(file))
		{
			int written = snprintf(candidate, sizeof candidate, "%s", file);
			if(written < 0 || (size_t)written >= sizeof candidate)
				continue;
		}
		else if(!FontMapJoin(candidate, sizeof candidate, gSwitchFontMapBase, file))
			continue;
		if(stat(candidate, &st) != 0 || S_ISDIR(st.st_mode))
			continue;
		faceIndex = Json_Number(entry, "index", 0);
		if(faceIndex < 0 || faceIndex > 255 || faceIndex != (int)faceIndex)
			continue;
		{
			int written = snprintf(out, outSize, "%s", candidate);
			if(written < 0 || (size_t)written >= outSize)
			{
				out[0] = 0;
				continue;
			}
		}
		*index = (int)faceIndex;
		return 1;
	}
	return 0;
}
// SWITCH_FONTMAP_PATCH_END


// a registered file of the requested class, else the first registered one
static int ResolveRegistered(int mincho, char* out, size_t n)
{
	int i, first = -1;
	for(i = 0; i < gFontFileCount; i++)
	{
		int serif;
		if(first < 0)
			first = i;
		serif = ContainsNoCase(gFontFiles[i], "mincho") || ContainsNoCase(gFontFiles[i], "serif");
		if(serif == mincho)
		{
			snprintf(out, n, "%s", gFontFiles[i]);
			return 1;
		}
	}
	if(first >= 0)
	{
		snprintf(out, n, "%s", gFontFiles[first]);
		return 1;
	}
	return 0;
}

static int ResolveByScan(int mincho, char* out, size_t n)
{
	const char* const* names;
	const char* const* dir;
	int pass;
	for(pass = 0; pass < 2; pass++)
	{
		names = (mincho ^ pass) ? kMinchoFiles : kGothicFiles;
		for(; *names; names++)
			for(dir = kFontDirs; *dir; dir++)
				if(FindFontFile(*dir, *names, 0, out, n))
					return 1;
	}
	for(dir = kFontDirs; *dir; dir++)
		if(FindAnyFont(*dir, 0, out, n))
			return 1;
	return 0;
}

static int ResolveFace(const char* sjisFace, int bold, int italic, char* out, size_t n, int* index, int* mapped)
{
	char utf8[0x200];
	const char* env;
	int mincho;
	BGI_UNUSED(bold);
	BGI_UNUSED(italic);
	OsSwitch_SjisToUtf8(sjisFace ? sjisFace : "", utf8, sizeof utf8);
	mincho = IsMinchoName(utf8);
	*index = 0;
	*mapped = 0;
	env = getenv(mincho ? "BGI_FONT_MINCHO" : "BGI_FONT_GOTHIC");
	if(!env || !*env)
		env = getenv("BGI_FONT");
	if(env && *env)
	{
		snprintf(out, n, "%s", env);
		return 1;
	}
	if(FontMapResolve(sjisFace, out, n, index))
	{
		*mapped = 1;
		return 1;
	}
	if(ResolveRegistered(mincho, out, n))
		return 1;
	return ResolveByScan(mincho, out, n);
}

static FaceRec_t* FaceAcquire(const char* path, int index)
{
	FaceRec_t* r;
	for(r = gFaces; r; r = r->next)
	{
		if(r->index == index && strcmp(r->path, path) == 0)
		{
			r->refs++;
			return r;
		}
	}
	r = (FaceRec_t*)calloc(1, sizeof *r);
	if(!r)
		return NULL;
	if(FT_New_Face(gFt, path, index, &r->face) != 0)
	{
		free(r);
		return NULL;
	}
	if(FT_Select_Charmap(r->face, FT_ENCODING_UNICODE) != 0 && r->face->num_charmaps > 0)
		FT_Set_Charmap(r->face, r->face->charmaps[0]);
	snprintf(r->path, sizeof r->path, "%s", path);
	r->index = index;
	r->refs = 1;
	r->next = gFaces;
	gFaces = r;
	return r;
}

static void FaceRelease(FaceRec_t* r)
{
	FaceRec_t** pp;
	if(--r->refs > 0)
		return;
	for(pp = &gFaces; *pp; pp = &(*pp)->next)
	{
		if(*pp == r)
		{
			*pp = r->next;
			break;
		}
	}
	FT_Done_Face(r->face);
	free(r);
}

// ---- opening ----------------------------------------------------------------

static void CellMetrics(FT_Face face, FT_Pos* ascUnits, FT_Pos* descUnits)
{
	const TT_OS2* os2 = (const TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
	if(os2 && (os2->usWinAscent || os2->usWinDescent))
	{
		*ascUnits = os2->usWinAscent;
		*descUnits = os2->usWinDescent;
	}
	else
	{
		*ascUnits = face->ascender;
		*descUnits = -face->descender;
	}
	if(*ascUnits + *descUnits <= 0)
	{
		*ascUnits = face->units_per_EM;
		*descUnits = 0;
	}
}

static void AddFontFile(const char* path)
{
	int i;
	for(i = 0; i < gFontFileCount; i++)
		if(strcmp(gFontFiles[i], path) == 0)
			return;
	if(gFontFileCount < SWITCH_FONT_FILES)
		snprintf(gFontFiles[gFontFileCount++], sizeof gFontFiles[0], "%s", path);
}

int OS_FontAddFile(const char* path)
{
	char native[1024];
	struct stat st;
	OsSwitch_Path(path, native, sizeof native);
	if(stat(native, &st) != 0 || S_ISDIR(st.st_mode))
		return 0;
	AddFontFile(native);
	return 1;
}

int OS_FontAddMemory(const void* data, uint32_t size)
{
	static int counter;
	char path[0x100];
	FILE* f;
	if(!data || !size)
		return 0;
	snprintf(path, sizeof path, "sdmc:/switch/openbgi/fonts/bgi-mem-%d.ttf", counter++);
	mkdir("sdmc:/switch/openbgi/fonts", 0777);
	f = fopen(path, "wb");
	if(!f)
		return 0;
	if(fwrite(data, 1, size, f) != size)
	{
		fclose(f);
		return 0;
	}
	fclose(f);
	AddFontFile(path);
	return 1;
}

int OS_FontFaceExists(const char* face)
{
	BGI_UNUSED(face);
	return 1;
}

int OS_FontEnumFaces(char* buf, int charset, int* outBytes)
{
	BGI_UNUSED(buf);
	BGI_UNUSED(charset);
	if(outBytes)
		*outBytes = 0;
	return 0;
}

OsFont_t* OS_FontOpen(const char* face, int height, int width, int bold, int italic)
{
	char path[1024];
	int index = 0;
	int mapped = 0;
	OsFont_t* f;
	FT_Face ft;
	FT_Pos asc, desc, cell, fullAdv;
	FT_UInt gi;

	if(height <= 0 || !FtInit())
		return NULL;
	if(!ResolveFace(face, bold, italic, path, sizeof path, &index, &mapped))
		return NULL;
	f = (OsFont_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	f->rec = FaceAcquire(path, index);
	if(!f->rec)
	{
		free(f);
		return NULL;
	}
	ft = f->rec->face;
	f->height = height;
	f->width = width;
	f->bold = bold;
	f->italic = italic;
	{
		char utf8[0x200];
		OsSwitch_SjisToUtf8(face ? face : "", utf8, sizeof utf8);
		f->serif = IsMinchoName(utf8);
	}

	CellMetrics(ft, &asc, &desc);
	cell = asc + desc;
	f->emH = (FT_F26Dot6)(((int64_t)height * 64 * ft->units_per_EM + cell / 2) / cell);
	f->ascent = (int)(((int64_t)height * asc + cell / 2) / cell);
	fullAdv = 0;
	gi = FT_Get_Char_Index(ft, 0x3042);
	if(gi && FT_Load_Glyph(ft, gi, FT_LOAD_NO_SCALE) == 0)
		fullAdv = ft->glyph->advance.x;
	if(fullAdv <= 0)
		fullAdv = ft->units_per_EM;
	/* A replacement face has its own natural advance widths.  Do not
	 * force the original BGI face's width onto it (the Win32 backend uses
	 * CreateFontW(cWidth = 0) for the same reason). */
	if(width > 0 && !mapped)
		f->emW = (FT_F26Dot6)(((int64_t)width * 2 * 64 * ft->units_per_EM + fullAdv / 2) / fullAdv);
	else
		f->emW = f->emH;

	if(FT_New_Size(ft, &f->size) != 0)
	{
		FaceRelease(f->rec);
		free(f);
		return NULL;
	}
	FT_Activate_Size(f->size);
	if(FT_Set_Char_Size(ft, f->emW, f->emH, 72, 72) != 0)
	{
		FT_Done_Size(f->size);
		FaceRelease(f->rec);
		free(f);
		return NULL;
	}
	if(bold && !(ft->style_flags & FT_STYLE_FLAG_BOLD))
	{
		f->boldStrength = f->emH / 24;
		if(f->boldStrength < 64)
			f->boldStrength = 64;
	}
	f->oblique = italic && !(ft->style_flags & FT_STYLE_FLAG_ITALIC);
	return f;
}

OsFont_t* OS_FontOpenMono(const char* face, int size, int bold)
{
	return OS_FontOpen(face, size, size / 2, bold, 0);
}

void OS_FontClose(OsFont_t* f)
{
	int i;
	if(!f)
		return;
	for(i = 0; i < f->fbCount; i++)
	{
		FT_Done_Size(f->fb[i].size);
		FaceRelease(f->fb[i].rec);
	}
	FT_Done_Size(f->size);
	FaceRelease(f->rec);
	free(f);
}

// ---- rendering --------------------------------------------------------------

static uint32_t SjisCodePoint(const char* sjis, int len)
{
	return OsCommon_CodePoint(sjis, len, OsSwitch_SjisChar);
}

static int ResolveFallbackFile(OsFont_t* f, uint32_t cp, char* out, size_t n, int* index)
{
	static const char* const kFiles[] = {"DejaVuSans.ttf", "FreeSans.ttf", "FreeSerif.ttf", "unifont.otf", "unifont.ttf", NULL};
	const char* const* name;
	const char* const* dir;
	BGI_UNUSED(f);
	BGI_UNUSED(cp);
	*index = 0;
	for(name = kFiles; *name; name++)
		for(dir = kFontDirs; *dir; dir++)
			if(FindFontFile(*dir, *name, 0, out, n))
				return 1;
	return 0;
}

static int FallbackFor(OsFont_t* f, uint32_t cp, FT_Face* outFace, FT_Size* outSize)
{
	char path[1024];
	int i, index = 0;
	FaceRec_t* rec;
	FT_Size size;
	FT_Pos asc, desc, cell;
	FT_F26Dot6 emH, emW;

	for(i = 0; i < f->fbCount; i++)
	{
		if(FT_Get_Char_Index(f->fb[i].rec->face, cp))
		{
			*outFace = f->fb[i].rec->face;
			*outSize = f->fb[i].size;
			return 1;
		}
	}
	for(i = 0; i < f->fbMissCount; i++)
		if(f->fbMiss[i] == cp >> 8)
			return 0;
	rec = NULL;
	if(f->fbCount < FALLBACK_MAX && ResolveFallbackFile(f, cp, path, sizeof path, &index))
		rec = FaceAcquire(path, index);
	if(rec && (rec == f->rec || FT_Get_Char_Index(rec->face, cp) == 0))
	{
		FaceRelease(rec);
		rec = NULL;
	}
	if(rec && FT_New_Size(rec->face, &size) != 0)
	{
		FaceRelease(rec);
		rec = NULL;
	}
	if(!rec)
	{
		if(f->fbMissCount < FALLBACK_MISS)
			f->fbMiss[f->fbMissCount++] = cp >> 8;
		return 0;
	}
	CellMetrics(rec->face, &asc, &desc);
	cell = asc + desc;
	emH = (FT_F26Dot6)(((int64_t)f->height * 64 * rec->face->units_per_EM + cell / 2) / cell);
	emW = (FT_F26Dot6)((int64_t)emH * f->emW / (f->emH ? f->emH : 1));
	FT_Activate_Size(size);
	FT_Set_Char_Size(rec->face, emW, emH, 72, 72);
	f->fb[f->fbCount].rec = rec;
	f->fb[f->fbCount].size = size;
	f->fbCount++;
	*outFace = rec->face;
	*outSize = size;
	return 1;
}

static FT_GlyphSlot LoadGlyph(OsFont_t* f, uint32_t cp, FT_Render_Mode mode)
{
	FT_Face ft = f->rec->face;
	FT_Size size = f->size;
	FT_UInt gi = FT_Get_Char_Index(ft, cp);
	FT_Int32 flags = FT_LOAD_NO_BITMAP | (mode == FT_RENDER_MODE_MONO ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL);
	if(gi == 0 && cp > 0x20 && FallbackFor(f, cp, &ft, &size))
		gi = FT_Get_Char_Index(ft, cp);
	FT_Activate_Size(size);
	if(gi == 0 && cp != 0x20)
		gi = FT_Get_Char_Index(ft, 0x25a1);
	if(FT_Load_Glyph(ft, gi, flags) != 0)
		return NULL;
	if(f->boldStrength && ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
		FT_Outline_EmboldenXY(&ft->glyph->outline, f->boldStrength, 0);
	if(f->oblique && ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
		FT_GlyphSlot_Oblique(ft->glyph);
	return FT_Render_Glyph(ft->glyph, mode) == 0 ? ft->glyph : NULL;
}

void OS_FontRender(OsFont_t* f, const char* sjis, int len, uint8_t* buf, int pitch, int w, int h)
{
	FT_GlyphSlot g;
	int x0, y0, y, x;
	int r;
	for(r = 0; r < h; r++)
		memset(buf + (size_t)r * (size_t)pitch, 0, (size_t)w);
	if(!f || !(g = LoadGlyph(f, SjisCodePoint(sjis, len), FT_RENDER_MODE_MONO)))
		return;
	x0 = g->bitmap_left;
	y0 = f->ascent - g->bitmap_top;
	for(y = 0; y < (int)g->bitmap.rows; y++)
	{
		const uint8_t* row = g->bitmap.buffer + (size_t)y * (size_t)g->bitmap.pitch;
		int dy = y0 + y;
		if(dy < 0 || dy >= h)
			continue;
		for(x = 0; x < (int)g->bitmap.width; x++)
		{
			int dx = x0 + x;
			if(dx < 0 || dx >= w)
				continue;
			if(row[x >> 3] & (0x80 >> (x & 7)))
				buf[(size_t)dy * (size_t)pitch + dx] = 0xff;
		}
	}
}

void OS_FontRenderMono(OsFont_t* f, const char* sjis, int len, uint8_t* bits, int pitch, int w, int h)
{
	FT_GlyphSlot g;
	int x0, y0, y, x;
	memset(bits, 0, (size_t)pitch * (size_t)h);
	if(!f || !(g = LoadGlyph(f, SjisCodePoint(sjis, len), FT_RENDER_MODE_MONO)))
		return;
	x0 = g->bitmap_left;
	y0 = f->ascent - g->bitmap_top;
	for(y = 0; y < (int)g->bitmap.rows; y++)
	{
		const uint8_t* row = g->bitmap.buffer + (size_t)y * (size_t)g->bitmap.pitch;
		int dy = y0 + y;
		if(dy < 0 || dy >= h)
			continue;
		for(x = 0; x < (int)g->bitmap.width; x++)
		{
			int dx = x0 + x;
			if(dx < 0 || dx >= w)
				continue;
			if(row[x >> 3] & (0x80 >> (x & 7)))
				bits[(size_t)dy * (size_t)pitch + (dx >> 3)] |= (uint8_t)(0x80 >> (dx & 7));
		}
	}
}

int OS_FontCharAdvance(OsFont_t* f, const char* sjis, int len)
{
	FT_GlyphSlot g;
	if(!f || !(g = LoadGlyph(f, SjisCodePoint(sjis, len), FT_RENDER_MODE_MONO)))
		return 0;
	return (int)((g->advance.x + 32) >> 6);
}

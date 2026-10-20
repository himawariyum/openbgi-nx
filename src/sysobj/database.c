/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * database.c - the global database file "BGI.gdb" (inc/bgi/sysobj/database.h)
 *
 * The file is SDC-encoded; decoded, it has one of three layouts.  The two
 * older ones are fixed sections:
 *
 *     +000000  window left, top (screen coordinates of the window rectangle)
 *     +000008  the first 0x400 bytes of global memory
 *     +000408  the system area (0x40000 bytes)
 *     +040408  the name table: 0x1000 slots of 0x20 bytes (layout A, until
 *              1.494), 0x4000 slots (layout B, 1.529 to 1.599)
 *     +060408  (A) / +0C0408 (B): 0x800 flag-group records {name[0x18],
 *              bits, dataOffset}
 *     +070408  (A) / +0D0408 (B): the concatenated flag bytes, dataOffset
 *              relative to here
 *
 * 1.616 on write "BURIKO GDB 3.00", a sequence of sized sections (all
 * values 32-bit little-endian):
 *
 *     magic[16] "BURIKO GDB 3.00", the decoded size, window left, top,
 *     the length (0x400) and bytes of the global memory prefix,
 *     the length (0x100000) and bytes of the system area,
 *     the count and the strings of the global names (each with its NUL),
 *     the count of flag groups and per group its name (with its NUL), its
 *     bits and its (bits + 7) / 8 bytes, the latest registered first.
 *
 * Each build reads what it writes; 1.529 on also read the other fixed
 * layout (told apart by whether layout A's first flag record looks
 * valid), and 1.616 on read both of them as well.  The reimplementation
 * also reads the file it wrote itself for 1.616 on before it wrote GDB
 * 3.00: layout A with the system area widened to 0x100000 bytes, which
 * moves the later sections up by 0xC0000.
 */
#include "bgi/sysobj/database.h"
#include "bgi/sysobj/names.h"
#include "bgi/sysobj/flags.h"
#include "bgi/sysobj/strtables.h"
#include "bgi/codec.h"
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/display.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/os.h"
#include "bgi/version.h"

#define GDB_FILE      "BGI.gdb"
#define GDB_GLOBALS   0x008     // offset of the global-memory prefix (after the window position)
#define GDB_SYSAREA   0x408     // offset of the system area
#define GDB_MAXGROUPS 0x800     // flag-group records in the fixed layouts (and groups the manager holds until 1.599)
#define GDB_WORK      0x2000000 // the work buffer of the fixed layouts (the decoded file never exceeds it)
#define GDB3_MAGIC    "BURIKO GDB 3.00"

typedef struct GdbFlagRec // one flag-group record of the fixed layouts
{
	char name[0x18];     // the group's name, NUL-terminated
	uint32_t bits;       // flags in the group; 0 marks an unused record
	uint32_t dataOffset; // of the bits, from the start of the flag bytes
} GdbFlagRec_t;

typedef struct GdbLayout // the sections of a fixed layout
{
	uint32_t sysarea;   // the system area's bytes
	uint32_t names;     // offset of the name slots
	uint32_t nameSlots; // their count
	uint32_t flags;     // offset of the flag records
	uint32_t data;      // offset of the flag bytes
} GdbLayout_t;

static const GdbLayout_t kLayoutA = {0x40000, 0x40408, 0x1000, 0x60408, 0x70408};
static const GdbLayout_t kLayoutB = {0x40000, 0x40408, 0x4000, 0xC0408, 0xD0408};
static const GdbLayout_t kLayoutWide = {0x100000, 0x100408, 0x1000, 0x120408, 0x130408}; // OpenBGI's earlier files of 1.616 on

static uint32_t Rd32(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void Wr32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Where the database lives: next to the game until 1.69 build 444; from
 * build 451 on in the directory "80 39" set (gAltDir), which is the user's
 * data folder in those games. */
static void GdbPath(char* out, const char* name)
{
	if(gEngine->gen >= GEN_1_69_451)
		sprintf(out, "%s%s", gAltDir, name);
	else
		strcpy(out, name);
}

/* Read the encoded file into a fresh buffer (*size bytes); NULL and size
 * 0 when there is none.  The pre-451 builds go through the file set's
 * loader, the later ones through the file layer directly (the loader only
 * joins a bare name with its own directories). */
static uint8_t* GdbReadFile(uint32_t* size)
{
	char path[0x210];
	uint8_t* file;
	GdbPath(path, GDB_FILE);
	if(gEngine->gen < GEN_1_69_451)
	{
		*size = GetFileSize_(NULL, GDB_FILE);
		if(*size == 0)
			return NULL;
		file = (uint8_t*)BGI_Alloc(*size);
		LoadFile(file, NULL, GDB_FILE);
		return file;
	}
	else
	{
		File_t f;
		File_Ctor(&f);
		if(!File_OpenRead(&f, path))
		{
			File_Dtor(&f);
			*size = 0;
			return NULL;
		}
		*size = File_Size(&f);
		file = (uint8_t*)BGI_Alloc(*size ? *size : 1);
		*size = File_Read(&f, file, *size);
		File_Close(&f);
		File_Dtor(&f);
		return file;
	}
}

/* Decode an SDC image of `size` bytes (0xFFFFFFFF: not known) into a
 * fresh buffer; NULL when it is not SDC, shorter than its header says or
 * fails its checks.  1.616 on allocate the stated size and want exactly
 * that many bytes decoded; the older builds decode into their fixed work
 * buffer and take whatever came out.  *n receives the decoded size. */
static uint8_t* GdbDecode(const uint8_t* file, uint32_t size, uint32_t* n)
{
	const int exact = gEngine->gen >= GEN_1_616;
	uint8_t* db;
	uint32_t out;
	*n = 0;
	if(size < sizeof(SdcHeader_t) || !SdcIsFormat(file) ||
		((const SdcHeader_t*)file)->encLen > size - (uint32_t)sizeof(SdcHeader_t))
		return NULL;
	out = SdcOutSize(file);
	if(!exact && out > GDB_WORK)
		return NULL; // (more than the work buffer holds)
	db = (uint8_t*)BGI_Alloc(exact ? (out ? out : 1) : GDB_WORK);
	*n = SdcDecode(db, file);
	if(*n == 0 || (exact && *n != out))
	{
		BGI_Free(db);
		return NULL;
	}
	return db;
}

// ---- GDB 3.00 ------------------------------------------------------------------------

typedef struct Gdb3 // a view of a decoded "BURIKO GDB 3.00" payload
{
	int32_t x, y;        // the window position
	uint32_t gLen, sLen; // the global memory prefix's and the system area's lengths
	const uint8_t* g;
	const uint8_t* s;
	uint32_t nNames;
	const char* names; // nNames strings, one after the other
	uint32_t nFlags;
	const uint8_t* flags; // nFlags records {name\0, bits, bytes}
} Gdb3_t;

/* Take a payload apart; 0 when it is not GDB 3.00, its size field is not
 * `n`, or a section runs past the end (the original checks only the size
 * field) */
static int Gdb3_Parse(Gdb3_t* v, const uint8_t* db, uint32_t n)
{
	uint32_t o = 0x1c, i;
	if(n < 0x24 || memcmp(db, GDB3_MAGIC, 16) != 0 || Rd32(db + 0x10) != n)
		return 0;
	v->x = (int32_t)Rd32(db + 0x14);
	v->y = (int32_t)Rd32(db + 0x18);
	v->gLen = Rd32(db + o);
	v->g = db + o + 4;
	if(v->gLen > n - o - 4 || n - o - 4 - v->gLen < 4)
		return 0;
	o += 4 + v->gLen;
	v->sLen = Rd32(db + o);
	v->s = db + o + 4;
	if(v->sLen > n - o - 4 || n - o - 4 - v->sLen < 4)
		return 0;
	o += 4 + v->sLen;
	v->nNames = Rd32(db + o);
	o += 4;
	v->names = (const char*)db + o;
	for(i = 0; i < v->nNames; i++)
	{
		const uint8_t* z = (const uint8_t*)memchr(db + o, 0, n - o);
		if(!z)
			return 0;
		o = (uint32_t)(z - db) + 1;
	}
	if(n - o < 4)
		return 0;
	v->nFlags = Rd32(db + o);
	v->flags = db + o + 4;
	o += 4;
	for(i = 0; i < v->nFlags; i++)
	{ // checked here, so that the users can walk the records freely
		const uint8_t* z = (const uint8_t*)memchr(db + o, 0, n - o);
		uint32_t bits;
		if(!z)
			return 0;
		o = (uint32_t)(z - db) + 1;
		if(n - o < 4)
			return 0;
		bits = Rd32(db + o);
		o += 4;
		if(FlagBytes(bits) > n - o)
			return 0;
		o += FlagBytes(bits);
	}
	return 1;
}

/* the flag records of a parsed payload into the manager, the last first
 * (which rebuilds the original's list in its order): registered with
 * their bits when `merge` is 0, else merged (FlagMgr_Merge) */
static void Gdb3_ApplyFlags(const Gdb3_t* v, int merge)
{
	const uint8_t** rec;
	const uint8_t* p = v->flags;
	uint32_t i;
	if(v->nFlags == 0)
		return;
	rec = (const uint8_t**)BGI_Alloc(v->nFlags * sizeof *rec);
	for(i = 0; i < v->nFlags; i++)
	{
		rec[i] = p;
		p += strlen((const char*)p) + 1;
		p += 4 + FlagBytes(Rd32(p));
	}
	for(i = v->nFlags; i-- > 0;)
	{
		const char* name = (const char*)rec[i];
		const uint8_t* q = rec[i] + strlen(name) + 1;
		uint32_t bits = Rd32(q);
		if(merge)
			FlagMgr_Merge(gFlagMgr, name, bits, q + 4);
		else
			FlagMgr_RegisterWithData(gFlagMgr, name, bits, q + 4); // (a group of 0 bits fails quietly)
	}
	BGI_Free(rec);
}

// the GDB 3.00 payload of the live state (*size bytes)
static uint8_t* Gdb3_Build(uint32_t* size)
{
	uint32_t namesBytes = (uint32_t)StrTab_Gather(NULL, GNAME_TABLE), flagBytes = 4, total, o, i;
	uint32_t count = FlagMgr_Count(gFlagMgr);
	uint8_t* db;
	int wx, wy, ww, wh;
	for(i = 0; i < count; i++)
	{
		const FlagGroup_t* g = FlagMgr_Group(gFlagMgr, i);
		flagBytes += (uint32_t)strlen(g->name) + 1 + 4 + FlagBytes(g->bits);
	}
	total = 0x20 + 0x400 + 4 + gSysAreaSize + 4 + namesBytes + flagBytes;
	db = (uint8_t*)BGI_Calloc(total);
	OS_WindowGetRect(&wx, &wy, &ww, &wh); // the window rectangle's left / top
	memcpy(db, GDB3_MAGIC, 16);
	Wr32(db + 0x10, total);
	Wr32(db + 0x14, (uint32_t)wx);
	Wr32(db + 0x18, (uint32_t)wy);
	Wr32(db + 0x1c, 0x400);
	memcpy(db + 0x20, gGlobalMem, 0x400);
	o = 0x420;
	Wr32(db + o, gSysAreaSize);
	memcpy(db + o + 4, gSysArea, gSysAreaSize);
	o += 4 + gSysAreaSize;
	Wr32(db + o, (uint32_t)StrTab_Count(GNAME_TABLE));
	StrTab_Gather((char*)db + o + 4, GNAME_TABLE);
	o += 4 + namesBytes;
	Wr32(db + o, count);
	o += 4;
	for(i = count; i-- > 0;)
	{ // the latest registered first, as the original's list holds them
		const FlagGroup_t* g = FlagMgr_Group(gFlagMgr, i);
		size_t len = strlen(g->name) + 1;
		memcpy(db + o, g->name, len);
		o += (uint32_t)len;
		Wr32(db + o, g->bits);
		memcpy(db + o + 4, g->data, FlagBytes(g->bits));
		o += 4 + FlagBytes(g->bits);
	}
	*size = total;
	return db;
}

// ---- the fixed layouts ------------------------------------------------------------------

// the fixed layout a build writes: B from 1.529 to 1.599, else A
static const GdbLayout_t* OwnLayout(void)
{
	return gEngine->gen >= GEN_1_529 ? &kLayoutB : &kLayoutA;
}

// whether flag record 0 of layout L looks valid in a payload of n bytes
static int FirstRecordValid(const uint8_t* db, uint32_t n, const GdbLayout_t* L)
{
	const uint8_t* r = db + L->flags;
	if(n < L->flags + sizeof(GdbFlagRec_t))
		return 0;
	return r[0] != 0 && memchr(r, 0, 0x18) && strlen((const char*)r) < 0x18 && Rd32(r + 0x18) > 0 && Rd32(r + 0x1c) == 0;
}

/* The fixed layout of a decoded payload: until 1.494 layout A; 1.529 on A
 * when its first flag record looks valid, else B; for 1.616 on also the
 * widened A of OpenBGI's earlier files, when the flag bytes its records
 * describe end exactly at n */
static const GdbLayout_t* DetectLayout(const uint8_t* db, uint32_t n)
{
	if(gEngine->gen < GEN_1_529)
		return &kLayoutA;
	if(gEngine->gen >= GEN_1_616 && n >= kLayoutWide.data)
	{
		const GdbFlagRec_t* rec = (const GdbFlagRec_t*)(db + kLayoutWide.flags);
		uint64_t want = kLayoutWide.data;
		int i;
		for(i = 0; i < GDB_MAXGROUPS; i++)
			if(rec[i].bits)
				want += FlagBytes(rec[i].bits);
		if(want == n)
			return &kLayoutWide;
	}
	return FirstRecordValid(db, n, &kLayoutA) ? &kLayoutA : &kLayoutB;
}

// the fixed layout L of the live state into db (cleared, GDB_WORK bytes); the payload's size
static uint32_t Fixed_Build(uint8_t* db, const GdbLayout_t* L)
{
	GdbFlagRec_t* rec = (GdbFlagRec_t*)(db + L->flags);
	uint32_t dataOff = 0, i, count = FlagMgr_Count(gFlagMgr);
	int wx, wy, ww, wh;
	OS_WindowGetRect(&wx, &wy, &ww, &wh); // the window rectangle's left / top
	memcpy(db + 0, &wx, 4);
	memcpy(db + 4, &wy, 4);
	memcpy(db + GDB_GLOBALS, gGlobalMem, 0x400);
	memcpy(db + GDB_SYSAREA, gSysArea, gSysAreaSize < L->sysarea ? gSysAreaSize : L->sysarea);
	GName_ToSlots((char(*)[GNAME_SIZE])(db + L->names), (int)L->nameSlots);
	for(i = 0; i < count && i < GDB_MAXGROUPS; i++, rec++)
	{
		const FlagGroup_t* g = FlagMgr_Group(gFlagMgr, i);
		uint32_t n = FlagBytes(g->bits);
		snprintf(rec->name, sizeof rec->name, "%s", g->name);
		rec->bits = g->bits;
		rec->dataOffset = dataOff;
		memcpy(db + L->data + dataOff, g->data, n);
		dataOff += n;
	}
	return L->data + dataOff;
}

/* A fixed-layout payload into the live state: the global memory prefix,
 * the system area (the rest of a larger one cleared), the names and the
 * flag groups.  0 when the decoded size n matches the flag bytes the
 * records describe, else 0x80000002 (what was read stays applied). */
static uint32_t Fixed_Apply(const uint8_t* db, uint32_t n, const GdbLayout_t* L)
{
	const GdbFlagRec_t* rec = (const GdbFlagRec_t*)(db + L->flags);
	uint64_t dataTotal = 0;
	uint32_t copy = gSysAreaSize < L->sysarea ? gSysAreaSize : L->sysarea;
	int i;
	if(n < L->data)
		return 0x80000002u;
	memcpy(gGlobalMem, db + GDB_GLOBALS, 0x400);
	memcpy(gSysArea, db + GDB_SYSAREA, copy);
	memset(gSysArea + copy, 0, gSysAreaSize - copy);
	GName_FromSlots((const char(*)[GNAME_SIZE])(db + L->names), (int)L->nameSlots);
	FlagMgr_Clear(gFlagMgr);
	for(i = 0; i < GDB_MAXGROUPS; i++, rec++)
		if(rec->bits != 0)
		{
			char name[0x18];
			memcpy(name, rec->name, sizeof name);
			name[sizeof name - 1] = 0;
			if(L->data + (uint64_t)rec->dataOffset + FlagBytes(rec->bits) <= n)
				FlagMgr_RegisterWithData(gFlagMgr, name, rec->bits, db + L->data + rec->dataOffset);
			dataTotal += FlagBytes(rec->bits);
		}
	return n == L->data + dataTotal ? 0 : 0x80000002u;
}

// ---- the entry points ---------------------------------------------------------------------

/* Assemble the decoded image from the live state, encode it and write the
 * file.  When the encoder fails the raw image goes into BGIError.txt (in
 * the data directory from 1.616 on) and an error box is shown; the
 * (empty) file is still written.  1 when the file was written
 * completely. */
int GlobalDB_Save(void)
{
	const int v3 = gEngine->gen >= GEN_1_616;
	uint8_t* db;
	uint8_t* enc;
	uint32_t total, encSize, written;
	char path[0x210];
	if(v3)
		db = Gdb3_Build(&total);
	else
	{
		db = (uint8_t*)BGI_Calloc(GDB_WORK);
		total = Fixed_Build(db, OwnLayout());
	}
	enc = (uint8_t*)BGI_Alloc(total * 2);
	encSize = SdcEncode(enc, db, total);
	if(encSize == 0)
	{
		if(v3)
			GdbPath(path, "BGIError.txt");
		else
			strcpy(path, "BGIError.txt");
		WriteTextFile(path, db, total);
		ShowErrorBox(v3 ? MSG_SDC_ENCODE_FAILED_3 : MSG_SDC_ENCODE_FAILED);
	}
	GdbPath(path, GDB_FILE);
	written = (uint32_t)WriteTextFile(path, enc, encSize);
	BGI_Free(enc);
	BGI_Free(db);
	return written == encSize;
}

/* Read the file into the live state.  GDB 3.00 (1.616 on): the global
 * memory prefix, the system area (cleared past its stored length), the
 * names (replaced) and the flag groups; 0 whenever the size field is
 * right.  A fixed layout: see Fixed_Apply.  0x80000001 when there is no
 * file, 0x80000002 when it does not decode to its stated size, is too
 * short, or does not add up. */
uint32_t GlobalDB_Load(void)
{
	uint32_t size, n = 0, result = 0x80000002u;
	uint8_t* file = GdbReadFile(&size);
	uint8_t* db;
	Gdb3_t v;
	if(size == 0)
	{
		BGI_Free(file);
		return 0x80000001u;
	}
	db = GdbDecode(file, size, &n);
	BGI_Free(file);
	if(!db)
		return 0x80000002u;
	if(gEngine->gen >= GEN_1_616 && n >= 16 && memcmp(db, GDB3_MAGIC, 16) == 0)
	{
		if(Gdb3_Parse(&v, db, n))
		{
			uint32_t g = v.gLen < 0x400 ? v.gLen : 0x400, s = v.sLen < gSysAreaSize ? v.sLen : gSysAreaSize;
			memcpy(gGlobalMem, v.g, g);
			memcpy(gSysArea, v.s, s);
			memset(gSysArea + s, 0, gSysAreaSize - s);
			StrTab_SetAll(GNAME_TABLE, (int)v.nNames, v.names);
			FlagMgr_Clear(gFlagMgr);
			Gdb3_ApplyFlags(&v, 0);
			result = 0;
		}
	}
	else if(gEngine->gen < GEN_1_529 || n >= kLayoutA.data)
		result = Fixed_Apply(db, n, DetectLayout(db, n));
	BGI_Free(db);
	return result;
}

/* The saved window position, accepted only while a window of the picture
 * size plus its frame still fits on the screen from there.  1.616 on take
 * it from a GDB 3.00 file whose size field is right, or from a fixed
 * layout of at least layout B's size. */
int SavedWindowPos_Load(int* x, int* y)
{
	uint32_t size, n = 0;
	uint8_t* file = GdbReadFile(&size);
	uint8_t* db;
	int ok = 0, have = 0, px = 0, py = 0;
	if(size == 0)
	{
		BGI_Free(file);
		return 0;
	}
	db = GdbDecode(file, size, &n);
	BGI_Free(file);
	if(!db)
		return 0;
	if(gEngine->gen >= GEN_1_616 && n >= 0x1c && memcmp(db, GDB3_MAGIC, 16) == 0)
	{
		if(Rd32(db + 0x10) == n)
		{
			px = (int32_t)Rd32(db + 0x14);
			py = (int32_t)Rd32(db + 0x18);
			have = 1;
		}
	}
	else if(n >= (gEngine->gen >= GEN_1_616 ? kLayoutB.data : kLayoutA.data))
	{
		memcpy(&px, db + 0, 4);
		memcpy(&py, db + 4, 4);
		have = 1;
	}
	if(have)
	{
		int sw, sh, fw, fh, maxX, maxY, winW, winH;
		OS_ScreenSize(&sw, &sh);
		OS_FrameMetrics(&fw, &fh);
		Display_GetPictureSize(&winW, &winH);
		maxX = sw - winW - fw;
		maxY = sh - winH - fh;
		if(px >= 0 && px <= maxX && py >= 0 && py <= maxY)
		{
			*x = px;
			*y = py;
			ok = 1;
		}
	}
	BGI_Free(db);
	return ok;
}

uint32_t GlobalDB_MergeImage(const void* img, void* globals, uint32_t* globalsSize, void* sysarea, uint32_t* sysareaSize)
{
	uint32_t n = 0;
	uint8_t* db;
	Gdb3_t v;
	if(!img || !(db = GdbDecode((const uint8_t*)img, 0xFFFFFFFFu, &n)))
		return 0x80000002u;
	if(!Gdb3_Parse(&v, db, n))
	{
		BGI_Free(db);
		return 0x80000002u;
	}
	if(globals)
	{
		uint32_t g = v.gLen < 0x400 ? v.gLen : 0x400;
		memcpy(globals, v.g, g);
		memset((uint8_t*)globals + g, 0, 0x400 - g);
	}
	if(globalsSize)
		*globalsSize = 0x400;
	if(sysarea)
	{
		uint32_t s = v.sLen < 0x100000 ? v.sLen : 0x100000;
		memcpy(sysarea, v.s, s);
		memset((uint8_t*)sysarea + s, 0, 0x100000 - s);
	}
	if(sysareaSize)
		*sysareaSize = 0x100000;
	if(v.nNames >= 1)
	{
		const char* p = v.names;
		uint32_t i;
		for(i = 0; i < v.nNames; i++, p += strlen(p) + 1)
			StrTab_Add(GNAME_TABLE, p); // (present names are folded)
	}
	Gdb3_ApplyFlags(&v, 1);
	BGI_Free(db);
	return 0;
}

// Clear the system area, create the flag manager, empty the names (both storages, names.h).
void SysArea_Init(void)
{
	memset(gSysArea, 0, gSysAreaSize);
	gFlagMgr = FlagMgr_New(GDB_MAXGROUPS);
	memset(gNameTable, 0, sizeof gNameTable);
	StrTab_Remove(GNAME_TABLE);
}

void SysArea_Shutdown(void)
{
	if(gFlagMgr)
	{
		FlagMgr_Delete(gFlagMgr);
		gFlagMgr = NULL;
	}
}

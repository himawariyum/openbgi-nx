/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fileacc.c - the scripts' file accessor ("81 28" .. "81 2B", 1.547 on);
 *             interface in bgi/sys.h
 *
 * A script opens a file by its path (used as given, relative to the game
 * directory), gets a handle number and then reads or writes it in blocks;
 * the error-report window and the user-data backup use it.  The original
 * opens in the script's thread and queues every other request for its
 * loader thread, setting the script's status word to 0 until the request
 * is done; the scripts wait with `while (status == 0) yield`.  Here every
 * request runs at once and the status word receives its final value
 * directly, which the scripts cannot tell apart.
 *
 * The status values, from 1.573 on: the number of bytes read or written,
 * 1 for a seek or a close, -1 for a failure (nothing transferred, the end
 * of the file, a bad seek position).  In 1.547 and 1.553 a transfer of
 * nothing gives -2, a close has no status word and there is no seek.
 *
 * The direction of a transfer is the one the file was opened with: mode 0
 * reads (an existing file), mode 1 writes (truncating or creating) and,
 * from 1.573 on, mode 2 writes into the file as it is (created when
 * missing; a seek to -1 goes to its end).
 */
#include "bgi/sys.h"
#include "bgi/os.h"
#include "bgi/strutil.h"
#include "bgi/version.h"

#include <stdio.h>
#include <string.h>

typedef struct FileAccNode
{
	uint32_t id;    // the handle number the script holds
	char* name;     // the path in lower case, for the "already open" test
	OsFile_t* file; // the open file
	int mode;       // 0 read, 1 write (truncated), 2 write (kept)
	struct FileAccNode* next;
} FileAccNode_t;

static FileAccNode_t* gFileAcc; // the open files, the newest first
static uint32_t gFileAccLastId; // the last handle number given out (the first is 1)

// the node of a handle number; NULL when it is not open
static FileAccNode_t* FileAcc_Find(uint32_t id)
{
	FileAccNode_t* n;
	for(n = gFileAcc; n; n = n->next)
		if(n->id == id)
			return n;
	return NULL;
}

// close the file of a node and take the node out of the list
static void FileAcc_Free(FileAccNode_t* node)
{
	FileAccNode_t** p;
	for(p = &gFileAcc; *p; p = &(*p)->next)
		if(*p == node)
		{
			*p = node->next;
			break;
		}
	OS_FileClose(node->file);
	BGI_Free(node->name);
	BGI_Free(node);
}

int FileAcc_Open(uint32_t* outHandle, const char* path, int mode)
{
	char lower[0x30c];
	FileAccNode_t* n;
	OsFile_t* f;
	const int keepCase = gEngine->gen >= GEN_1_573; // 1.547 / 1.553 open the lower-case copy
	if(mode < 0 || mode > (keepCase ? 2 : 1))
		return 1;
	snprintf(lower, sizeof lower, "%s", path ? path : "");
	SjisStrLwr(lower);
	for(n = gFileAcc; n; n = n->next)
		if(strcmp(n->name, lower) == 0)
			return 2;
	path = keepCase ? (path ? path : "") : lower;
	f = mode == 0 ? OS_FileOpenRead(path) : mode == 1 ? OS_FileCreate(path)
													  : OS_FileOpenWrite(path);
	if(!f)
		return 3;
	n = (FileAccNode_t*)BGI_Alloc(sizeof *n);
	n->id = ++gFileAccLastId;
	n->name = BGI_Strdup(lower);
	n->file = f;
	n->mode = mode;
	n->next = gFileAcc;
	gFileAcc = n;
	if(outHandle)
		*outHandle = n->id;
	return 0;
}

int FileAcc_Close(int32_t* status, uint32_t handle)
{
	FileAccNode_t* n = FileAcc_Find(handle);
	if(gEngine->gen < GEN_1_573)
	{ // closed at once; there is no status word
		if(!n)
			return -1;
		FileAcc_Free(n);
		return 0;
	}
	if(!n)
		return 4;
	FileAcc_Free(n);
	if(status)
		*status = 1;
	return 0;
}

int FileAcc_Transfer(int32_t* status, uint32_t handle, void* buf, uint32_t size)
{
	FileAccNode_t* n;
	uint32_t done;
	const int old = gEngine->gen < GEN_1_573;
	if(old && size == 0)
		return 6;
	n = FileAcc_Find(handle);
	if(!n)
		return 4;
	done = n->mode == 0 ? OS_FileRead(n->file, buf, size) : OS_FileWrite(n->file, buf, size);
	if(status)
		*status = done ? (int32_t)done : old ? -2
											 : -1;
	return 0;
}

int FileAcc_Seek(int32_t* status, uint32_t handle, uint32_t pos)
{
	FileAccNode_t* n = FileAcc_Find(handle);
	uint32_t size;
	int ok = 0;
	if(!n)
		return 4;
	size = OS_FileSize(n->file);
	if(pos == 0xFFFFFFFFu)
		pos = size; // the end (a size that is not known fails)
	if(pos != 0xFFFFFFFFu && pos <= size)
		ok = OS_FileSeek(n->file, pos);
	if(status)
		*status = ok ? 1 : -1;
	return 0;
}

void FileAcc_CloseAll(void)
{
	while(gFileAcc)
		FileAcc_Free(gFileAcc);
}

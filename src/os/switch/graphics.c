/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * graphics.c - Switch back end: the deko3d device, the swapchain and the
 *              software framebuffer the window layer draws into.
 *
 * The engine draws into plain memory.  OsSwitch_Framebuffer() hands that
 * memory out; OsSwitch_GraphicsPresent() copies it into the acquired
 * swapchain image with the 2D engine (dkCmdBufCopyBufferToImage) and
 * presents it.  There are no shaders and no render pass: one linear
 * RGBA8 buffer is all this back end needs.
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DkDevice gSwitchDevice;
DkQueue gSwitchQueue;
DkSwapchain gSwitchSwapchain;
DkMemBlock gSwitchImageMemBlock;
DkImage gSwitchFramebuffers[2];
uint32_t gSwitchFramebufferWidth;
uint32_t gSwitchFramebufferHeight;

#define SWITCH_FB_COUNT   2
#define SWITCH_UPLOAD_COUNT 3
#define SWITCH_CMD_SIZE   (64 * 1024)
#define SWITCH_FB_FORMAT  DkImageFormat_RGBA8_Unorm

static DkCmdBuf gCmdBuf[SWITCH_UPLOAD_COUNT];
static DkMemBlock gCmdMem[SWITCH_UPLOAD_COUNT];
static DkMemBlock gUploadMem[SWITCH_UPLOAD_COUNT];
static DkFence gUploadFence[SWITCH_UPLOAD_COUNT];
static int gUploadFenceUsed[SWITCH_UPLOAD_COUNT];
static uint32_t gUploadIndex;
static DkImageLayout gImageLayout;
static DkImageView gImageViews[SWITCH_FB_COUNT];
static uint32_t gUploadSize;
static int gGraphicsReady;
static uint64_t gPresentMeasureStart;
static uint32_t gPresentMeasureCount;
static int gQueueErrorLogged;

// The CPU-side drawing surface is separate from the upload ring. This keeps
// the engine from modifying memory while the GPU is still reading it.
static uint32_t* gFramebuffer;
static int gPitchPixels;

int OsSwitch_GraphicsInit(int width, int height)
{
	DkDeviceMaker deviceMaker;
	DkMemBlockMaker memMaker;
	DkImageLayoutMaker layoutMaker;
	DkSwapchainMaker swapMaker;
	DkCmdBufMaker cmdMaker;
	DkImage const* images[SWITCH_FB_COUNT];
	uint32_t imageSize, imageAlign, i;

	if(gGraphicsReady)
		return 1;
	if(width <= 0 || height <= 0)
	{
		width = 1280;
		height = 720;
	}
	OsSwitch_Log("graphics init: %dx%d", width, height);

	gSwitchFramebufferWidth = (uint32_t)width;
	gSwitchFramebufferHeight = (uint32_t)height;
	gPitchPixels = width;
	gUploadSize = (uint32_t)width * (uint32_t)height * 4u;

	dkDeviceMakerDefaults(&deviceMaker);
	gSwitchDevice = dkDeviceCreate(&deviceMaker);
	if(!gSwitchDevice)
	{
		OsSwitch_Log("graphics: dkDeviceCreate FAILED");
		printf("graphics: dkDeviceCreate failed\n");
		return 0;
	}
	OsSwitch_Log("graphics: dkDeviceCreate OK");

	// Image layout shared by the swapchain images.
	dkImageLayoutMakerDefaults(&layoutMaker, gSwitchDevice);
	layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
	layoutMaker.format = SWITCH_FB_FORMAT;
	layoutMaker.dimensions[0] = gSwitchFramebufferWidth;
	layoutMaker.dimensions[1] = gSwitchFramebufferHeight;
	dkImageLayoutInitialize(&gImageLayout, &layoutMaker);

	imageSize = (uint32_t)dkImageLayoutGetSize(&gImageLayout);
	imageAlign = dkImageLayoutGetAlignment(&gImageLayout);
	imageSize = (imageSize + imageAlign - 1) & ~(imageAlign - 1);

	dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, imageSize * SWITCH_FB_COUNT);
	memMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	gSwitchImageMemBlock = dkMemBlockCreate(&memMaker);
	if(!gSwitchImageMemBlock)
	{
		OsSwitch_Log("graphics: framebuffer memory allocation FAILED");
		printf("graphics: framebuffer memory allocation failed\n");
		OsSwitch_GraphicsShutdown();
		return 0;
	}

	for(i = 0; i < SWITCH_FB_COUNT; i++)
	{
		images[i] = &gSwitchFramebuffers[i];
		dkImageInitialize(&gSwitchFramebuffers[i], &gImageLayout, gSwitchImageMemBlock, i * imageSize);
		dkImageViewDefaults(&gImageViews[i], &gSwitchFramebuffers[i]);
	}

	dkSwapchainMakerDefaults(&swapMaker, gSwitchDevice, nwindowGetDefault(), images, SWITCH_FB_COUNT);
	gSwitchSwapchain = dkSwapchainCreate(&swapMaker);
	if(!gSwitchSwapchain)
	{
		OsSwitch_Log("graphics: dkSwapchainCreate FAILED");
		printf("graphics: dkSwapchainCreate failed\n");
		OsSwitch_GraphicsShutdown();
		return 0;
	}
	OsSwitch_Log("graphics: dkSwapchainCreate OK");

	// Three upload/command slots let the CPU prepare the next frame while the
	// GPU consumes an earlier one. Each slot's fence protects both its upload
	// memory and its command memory until the submitted copy has completed.
	for(i = 0; i < SWITCH_UPLOAD_COUNT; i++)
	{
		dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, SWITCH_CMD_SIZE);
		memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		gCmdMem[i] = dkMemBlockCreate(&memMaker);
		if(!gCmdMem[i])
		{
			OsSwitch_Log("graphics: command memory allocation FAILED (slot %u)", (unsigned)i);
			printf("graphics: command memory allocation failed (slot %u)\n", i);
			OsSwitch_GraphicsShutdown();
			return 0;
		}
		dkCmdBufMakerDefaults(&cmdMaker, gSwitchDevice);
		gCmdBuf[i] = dkCmdBufCreate(&cmdMaker);
		if(!gCmdBuf[i])
		{
			OsSwitch_Log("graphics: dkCmdBufCreate FAILED (slot %u)", (unsigned)i);
			printf("graphics: dkCmdBufCreate failed (slot %u)\n", i);
			OsSwitch_GraphicsShutdown();
			return 0;
		}
		dkCmdBufAddMemory(gCmdBuf[i], gCmdMem[i], 0, SWITCH_CMD_SIZE);

		dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, gUploadSize);
		memMaker.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
		gUploadMem[i] = dkMemBlockCreate(&memMaker);
		if(!gUploadMem[i])
		{
			OsSwitch_Log("graphics: upload buffer allocation FAILED (slot %u)", (unsigned)i);
			printf("graphics: upload buffer allocation failed (slot %u)\n", i);
			OsSwitch_GraphicsShutdown();
			return 0;
		}
		gUploadFenceUsed[i] = 0;
	}

	// Stable software surface. Present copies it into the next available
	// upload-ring slot before submitting the GPU transfer.
	gFramebuffer = (uint32_t*)malloc(gUploadSize);
	if(!gFramebuffer)
	{
		OsSwitch_Log("graphics: software framebuffer allocation FAILED");
		printf("graphics: software framebuffer allocation failed\n");
		OsSwitch_GraphicsShutdown();
		return 0;
	}
	memset(gFramebuffer, 0, gUploadSize);

	// The render queue.
	{
		DkQueueMaker queueMaker;
		dkQueueMakerDefaults(&queueMaker, gSwitchDevice);
		queueMaker.flags = DkQueueFlags_Graphics;
		gSwitchQueue = dkQueueCreate(&queueMaker);
		if(!gSwitchQueue)
		{
			OsSwitch_Log("graphics: dkQueueCreate FAILED");
			printf("graphics: dkQueueCreate failed\n");
			OsSwitch_GraphicsShutdown();
			return 0;
		}
	}

	gGraphicsReady = 1;
	OsSwitch_Log("graphics: ready (%ux%u)", gSwitchFramebufferWidth, gSwitchFramebufferHeight);
	printf("graphics: %ux%u deko3d swapchain ready\n", gSwitchFramebufferWidth, gSwitchFramebufferHeight);
	return 1;
}

void OsSwitch_GraphicsShutdown(void)
{
	if(gSwitchQueue)
	{
		if(!gQueueErrorLogged && !dkQueueIsInErrorState(gSwitchQueue))
			dkQueueWaitIdle(gSwitchQueue);
		dkQueueDestroy(gSwitchQueue);
		gSwitchQueue = NULL;
	}
	{
		uint32_t i;
		for(i = 0; i < SWITCH_UPLOAD_COUNT; i++)
		{
			if(gCmdBuf[i])
			{
				dkCmdBufDestroy(gCmdBuf[i]);
				gCmdBuf[i] = NULL;
			}
			if(gCmdMem[i])
			{
				dkMemBlockDestroy(gCmdMem[i]);
				gCmdMem[i] = NULL;
			}
			if(gUploadMem[i])
			{
				dkMemBlockDestroy(gUploadMem[i]);
				gUploadMem[i] = NULL;
			}
			gUploadFenceUsed[i] = 0;
		}
	}
	if(gFramebuffer)
	{
		free(gFramebuffer);
		gFramebuffer = NULL;
	}
	gUploadIndex = 0;
	gPresentMeasureStart = 0;
	gPresentMeasureCount = 0;
	gQueueErrorLogged = 0;
	if(gSwitchSwapchain)
	{
		dkSwapchainDestroy(gSwitchSwapchain);
		gSwitchSwapchain = NULL;
	}
	if(gSwitchImageMemBlock)
	{
		dkMemBlockDestroy(gSwitchImageMemBlock);
		gSwitchImageMemBlock = NULL;
	}
	if(gSwitchDevice)
	{
		dkDeviceDestroy(gSwitchDevice);
		gSwitchDevice = NULL;
	}
	gGraphicsReady = 0;
}

uint32_t* OsSwitch_Framebuffer(int* pitchPixels, int* width, int* height)
{
	if(pitchPixels)
		*pitchPixels = gPitchPixels;
	if(width)
		*width = (int)gSwitchFramebufferWidth;
	if(height)
		*height = (int)gSwitchFramebufferHeight;
	return gFramebuffer;
}

void OsSwitch_FramebufferClear(uint32_t rgba)
{
	uint32_t i, n;
	if(!gFramebuffer)
		return;
	n = gSwitchFramebufferWidth * gSwitchFramebufferHeight;
	for(i = 0; i < n; i++)
		gFramebuffer[i] = rgba;
}

void OsSwitch_GraphicsPresent(void)
{
	int slot, upload;
	DkCopyBuf src;
	DkImageRect dst;
	DkResult waitResult;
	uint64_t now;

	if(!gGraphicsReady || !gFramebuffer)
		return;

	if(gQueueErrorLogged)
		return; // Once failed, the only legal queue operation is destruction.
	if(dkQueueIsInErrorState(gSwitchQueue))
	{
		gQueueErrorLogged = 1;
		printf("graphics: deko3d queue entered error state\n");
		OsSwitch_Log("graphics: deko3d queue entered error state; no more frames submitted");
		return;
	}

	slot = dkQueueAcquireImage(gSwitchQueue, gSwitchSwapchain);
	if(slot < 0 || slot >= SWITCH_FB_COUNT)
		return;

	// Reuse a ring slot only after its previous GPU copy has finished. Unlike
	// dkQueueWaitIdle(), this does not wait for unrelated queue/presentation
	// work. Static zero-initialized DkFence objects are empty on first use.
	upload = (int)(gUploadIndex++ % SWITCH_UPLOAD_COUNT);
	if(gUploadFenceUsed[upload])
	{
		waitResult = dkFenceWait(&gUploadFence[upload], -1);
		if(waitResult != DkResult_Success)
		{
			int queueError = dkQueueIsInErrorState(gSwitchQueue) ? 1 : 0;
			if(queueError)
				gQueueErrorLogged = 1;
			OsSwitch_Log("graphics: upload fence wait failed: %d (queue error=%d)",
				(int)waitResult, queueError);
			return;
		}
	}

	memcpy(dkMemBlockGetCpuAddr(gUploadMem[upload]), gFramebuffer, gUploadSize);
	dkMemBlockFlushCpuCache(gUploadMem[upload], 0, gUploadSize);

	src.addr = dkMemBlockGetGpuAddr(gUploadMem[upload]);
	src.rowLength = 0;   // tightly packed
	src.imageHeight = 0; // the rectangle's height

	dst.x = 0;
	dst.y = 0;
	dst.z = 0;
	dst.width = gSwitchFramebufferWidth;
	dst.height = gSwitchFramebufferHeight;
	dst.depth = 1;

	dkCmdBufClear(gCmdBuf[upload]);
	dkCmdBufCopyBufferToImage(gCmdBuf[upload], &src, &gImageViews[slot], &dst, 0);
	dkQueueSubmitCommands(gSwitchQueue, dkCmdBufFinishList(gCmdBuf[upload]));
	// This fence covers the transfer and command memory, not the display's
	// scanout. dkQueuePresentImage adds its own presentation fence and flush.
	dkQueueSignalFence(gSwitchQueue, &gUploadFence[upload], false);
	gUploadFenceUsed[upload] = 1;
	dkQueuePresentImage(gSwitchQueue, gSwitchSwapchain, slot);

	// A one-second rolling count distinguishes a slow software renderer from
	// a queue that is itself falling behind. Kept in startup.log by the earlier
	// diagnostics patch.
	now = OsSwitch_TicksNs();
	if(!gPresentMeasureStart)
		gPresentMeasureStart = now;
	gPresentMeasureCount++;
	if(now - gPresentMeasureStart >= 1000000000ull)
	{
		OsSwitch_Log("graphics: %.1f presents/s; queue error=%d",
			(double)gPresentMeasureCount * 1000000000.0 / (double)(now - gPresentMeasureStart),
			dkQueueIsInErrorState(gSwitchQueue) ? 1 : 0);
		gPresentMeasureCount = 0;
		gPresentMeasureStart = now;
	}
}

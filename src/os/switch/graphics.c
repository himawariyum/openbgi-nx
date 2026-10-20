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
#define SWITCH_CMD_SIZE   (64 * 1024)
#define SWITCH_FB_FORMAT  DkImageFormat_RGBA8_Unorm

static DkCmdBuf gCmdBuf;
static DkMemBlock gCmdMem;
static DkMemBlock gUploadMem;
static DkImageLayout gImageLayout;
static DkImageView gImageViews[SWITCH_FB_COUNT];
static uint32_t gUploadSize;
static int gGraphicsReady;

// The linear buffer the window layer draws into (RGBA8 in memory).
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

	gSwitchFramebufferWidth = (uint32_t)width;
	gSwitchFramebufferHeight = (uint32_t)height;
	gPitchPixels = width;
	gUploadSize = (uint32_t)width * (uint32_t)height * 4u;

	dkDeviceMakerDefaults(&deviceMaker);
	gSwitchDevice = dkDeviceCreate(&deviceMaker);
	if(!gSwitchDevice)
	{
		printf("graphics: dkDeviceCreate failed\n");
		return 0;
	}

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
		printf("graphics: dkSwapchainCreate failed\n");
		OsSwitch_GraphicsShutdown();
		return 0;
	}

	// Command memory and command buffer.
	dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, SWITCH_CMD_SIZE);
	memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	gCmdMem = dkMemBlockCreate(&memMaker);
	if(!gCmdMem)
	{
		printf("graphics: command memory allocation failed\n");
		OsSwitch_GraphicsShutdown();
		return 0;
	}
	dkCmdBufMakerDefaults(&cmdMaker, gSwitchDevice);
	gCmdBuf = dkCmdBufCreate(&cmdMaker);
	if(!gCmdBuf)
	{
		printf("graphics: dkCmdBufCreate failed\n");
		OsSwitch_GraphicsShutdown();
		return 0;
	}
	dkCmdBufAddMemory(gCmdBuf, gCmdMem, 0, SWITCH_CMD_SIZE);

	// The upload buffer: CPU-cached for fast drawing, flushed before each copy.
	dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, gUploadSize);
	memMaker.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
	gUploadMem = dkMemBlockCreate(&memMaker);
	if(!gUploadMem)
	{
		printf("graphics: upload buffer allocation failed\n");
		OsSwitch_GraphicsShutdown();
		return 0;
	}
	gFramebuffer = (uint32_t*)dkMemBlockGetCpuAddr(gUploadMem);
	memset(gFramebuffer, 0, gUploadSize);

	// The render queue.
	{
		DkQueueMaker queueMaker;
		dkQueueMakerDefaults(&queueMaker, gSwitchDevice);
		queueMaker.flags = DkQueueFlags_Graphics;
		gSwitchQueue = dkQueueCreate(&queueMaker);
		if(!gSwitchQueue)
		{
			printf("graphics: dkQueueCreate failed\n");
			OsSwitch_GraphicsShutdown();
			return 0;
		}
	}

	gGraphicsReady = 1;
	printf("graphics: %ux%u deko3d swapchain ready\n", gSwitchFramebufferWidth, gSwitchFramebufferHeight);
	return 1;
}

void OsSwitch_GraphicsShutdown(void)
{
	if(gSwitchQueue)
	{
		dkQueueWaitIdle(gSwitchQueue);
		dkQueueDestroy(gSwitchQueue);
		gSwitchQueue = NULL;
	}
	if(gCmdBuf)
	{
		dkCmdBufDestroy(gCmdBuf);
		gCmdBuf = NULL;
	}
	if(gCmdMem)
	{
		dkMemBlockDestroy(gCmdMem);
		gCmdMem = NULL;
	}
	if(gUploadMem)
	{
		dkMemBlockDestroy(gUploadMem);
		gUploadMem = NULL;
		gFramebuffer = NULL;
	}
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
	int slot;
	DkCopyBuf src;
	DkImageRect dst;

	if(!gGraphicsReady || !gFramebuffer)
		return;

	// The previous frame may still read the upload buffer.
	dkQueueWaitIdle(gSwitchQueue);
	dkMemBlockFlushCpuCache(gUploadMem, 0, gUploadSize);

	slot = dkQueueAcquireImage(gSwitchQueue, gSwitchSwapchain);
	if(slot < 0 || slot >= SWITCH_FB_COUNT)
		return;

	src.addr = dkMemBlockGetGpuAddr(gUploadMem);
	src.rowLength = 0;   // tightly packed
	src.imageHeight = 0; // the rectangle's height

	dst.x = 0;
	dst.y = 0;
	dst.z = 0;
	dst.width = gSwitchFramebufferWidth;
	dst.height = gSwitchFramebufferHeight;
	dst.depth = 1;

	dkCmdBufClear(gCmdBuf);
	dkCmdBufCopyBufferToImage(gCmdBuf, &src, &gImageViews[slot], &dst, 0);
	dkQueueSubmitCommands(gSwitchQueue, dkCmdBufFinishList(gCmdBuf));
	dkQueuePresentImage(gSwitchQueue, gSwitchSwapchain, slot);
}

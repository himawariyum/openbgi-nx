/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * graphics.c - Switch back end: the deko3d device, the swapchain, the
 *              software fallback framebuffer and optional 2D blitter.
 *
 * The default path uploads the software framebuffer to the swapchain.
 * BGI_SWITCH_GPU_BLIT preserves the previous swapchain image, then uploads
 * individual dirty surfaces to a reusable image and scales them into the
 * acquired image with the 2D engine.
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BGI_SWITCH_GPU_BLIT
#define BGI_SWITCH_GPU_BLIT 0
#endif

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
#define SWITCH_BLIT_MAX_WIDTH  1920
#define SWITCH_BLIT_MAX_HEIGHT 1080
#define SWITCH_BLIT_UPLOAD_SIZE (SWITCH_BLIT_MAX_WIDTH * SWITCH_BLIT_MAX_HEIGHT * 4u)
#define SWITCH_BLIT_BLACK_SIZE 64

static DkCmdBuf gCmdBuf[SWITCH_UPLOAD_COUNT];
static DkMemBlock gCmdMem[SWITCH_UPLOAD_COUNT];
static DkMemBlock gUploadMem[SWITCH_UPLOAD_COUNT];
static DkFence gUploadFence[SWITCH_UPLOAD_COUNT];
static int gUploadFenceUsed[SWITCH_UPLOAD_COUNT];
static uint32_t gUploadIndex;
static DkImageLayout gImageLayout;
static DkImageView gImageViews[SWITCH_FB_COUNT];
static DkImageLayout gBlitImageLayout;
static DkImageLayout gBlackImageLayout;
static DkImage gBlitImage;
static DkImage gBlackImage;
static DkImageView gBlitImageView;
static DkImageView gBlackImageView;
static DkMemBlock gBlitMem;
/* The cursor-free picture.  Every blit of the engine lands here; a present
 * copies it into the acquired swapchain image and draws the cursor on that
 * copy only, so a moving cursor never leaves a trail in the picture. */
static DkMemBlock gBaseMem;
static DkImage gBaseImage;
static DkImageView gBaseView;
static int gBaseValid;

/* The letterbox bars are outside the picture, so nothing the engine draws
 * should reach them, but should anything ever do (or a stray pixel remain), they are
 * blacked out again: at most every BARS_MIN_INTERVAL_NS, and only once
 * drawing has been quiet for BARS_QUIET_NS and something was drawn since the last clear. */
#define BARS_MIN_INTERVAL_NS 2000000000ull
#define BARS_QUIET_NS        300000000ull
static uint64_t gLastBlitNs, gBarsClearedNs;
static int gBlitSinceBars;
static uint32_t gUploadSize;
static int gGraphicsReady;
static uint64_t gPresentMeasureStart;
static uint32_t gPresentMeasureCount;
static int gQueueErrorLogged;
static int gGpuBlitReady;
static int gFrameActive, gFrameInitialized, gFrameImageSlot = -1;
static int gLastPresentedValid, gLastPresentedSlot = -1;
static int gFrameUploadSlot = -1;
static uint32_t gFrameUploadOffset;

static int OsSwitch_GraphicsAcquireUploadSlot(void);

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
	uint32_t imageSize, imageAlign, i, uploadBufferSize;
#if BGI_SWITCH_GPU_BLIT
	DkImageLayoutMaker blitLayoutMaker, blackLayoutMaker;
	uint32_t blitSize, blitAlign, blackSize, blackAlign, blackOffset, blitMemSize;
#endif

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
#if BGI_SWITCH_GPU_BLIT
	uploadBufferSize = SWITCH_BLIT_UPLOAD_SIZE;
#else
	uploadBufferSize = gUploadSize;
#endif

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
	layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
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

#if BGI_SWITCH_GPU_BLIT
	// One reusable source image is safe here: each submitted command list
	// copies its staging slot into this image and blits it before the next
	// command list overwrites it. Queue order preserves the copy/blit sequence.
	dkImageLayoutMakerDefaults(&blitLayoutMaker, gSwitchDevice);
	blitLayoutMaker.flags = DkImageFlags_Usage2DEngine;
	blitLayoutMaker.format = SWITCH_FB_FORMAT;
	blitLayoutMaker.dimensions[0] = SWITCH_BLIT_MAX_WIDTH;
	blitLayoutMaker.dimensions[1] = SWITCH_BLIT_MAX_HEIGHT;
	dkImageLayoutInitialize(&gBlitImageLayout, &blitLayoutMaker);
	blitSize = (uint32_t)dkImageLayoutGetSize(&gBlitImageLayout);
	blitAlign = dkImageLayoutGetAlignment(&gBlitImageLayout);
	blitSize = (blitSize + blitAlign - 1) & ~(blitAlign - 1);

	dkImageLayoutMakerDefaults(&blackLayoutMaker, gSwitchDevice);
	blackLayoutMaker.flags = DkImageFlags_Usage2DEngine;
	blackLayoutMaker.format = SWITCH_FB_FORMAT;
	blackLayoutMaker.dimensions[0] = SWITCH_BLIT_BLACK_SIZE;
	blackLayoutMaker.dimensions[1] = SWITCH_BLIT_BLACK_SIZE;
	dkImageLayoutInitialize(&gBlackImageLayout, &blackLayoutMaker);
	blackSize = (uint32_t)dkImageLayoutGetSize(&gBlackImageLayout);
	blackAlign = dkImageLayoutGetAlignment(&gBlackImageLayout);
	blackSize = (blackSize + blackAlign - 1) & ~(blackAlign - 1);
	blackOffset = (blitSize + blackAlign - 1) & ~(blackAlign - 1);
	blitMemSize = blackOffset + blackSize;
	blitMemSize = (blitMemSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
	dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, blitMemSize);
	memMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	gBlitMem = dkMemBlockCreate(&memMaker);
	if(gBlitMem)
	{
		uint32_t baseSize = (imageSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
		dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, baseSize);
		memMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
		gBaseMem = dkMemBlockCreate(&memMaker);
		if(gBaseMem)
		{
			dkImageInitialize(&gBaseImage, &gImageLayout, gBaseMem, 0);
			dkImageViewDefaults(&gBaseView, &gBaseImage);
		}
		else
			OsSwitch_Log("graphics: base image allocation failed; using CPU fallback");
		dkImageInitialize(&gBlitImage, &gBlitImageLayout, gBlitMem, 0);
		dkImageViewDefaults(&gBlitImageView, &gBlitImage);
		dkImageInitialize(&gBlackImage, &gBlackImageLayout, gBlitMem, blackOffset);
		dkImageViewDefaults(&gBlackImageView, &gBlackImage);
	}
	else
	{
		uploadBufferSize = gUploadSize;
		OsSwitch_Log("graphics: GPU blitter image allocation failed; using CPU fallback");
	}
#endif

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

		dkMemBlockMakerDefaults(&memMaker, gSwitchDevice, uploadBufferSize);
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

#if BGI_SWITCH_GPU_BLIT
	if(gBlitMem && gBaseMem)
	{
		DkCopyBuf blackSource;
		DkImageRect blackRect = {0, 0, 0, SWITCH_BLIT_BLACK_SIZE, SWITCH_BLIT_BLACK_SIZE, 1};
		uint32_t* pixels = (uint32_t*)dkMemBlockGetCpuAddr(gUploadMem[0]);
		uint32_t n;
		for(n = 0; n < SWITCH_BLIT_BLACK_SIZE * SWITCH_BLIT_BLACK_SIZE; n++)
			pixels[n] = 0xff000000u;
		dkMemBlockFlushCpuCache(gUploadMem[0], 0,
			SWITCH_BLIT_BLACK_SIZE * SWITCH_BLIT_BLACK_SIZE * 4u);
		blackSource.addr = dkMemBlockGetGpuAddr(gUploadMem[0]);
		blackSource.rowLength = 0;
		blackSource.imageHeight = 0;
		dkCmdBufClear(gCmdBuf[0]);
		dkCmdBufCopyBufferToImage(gCmdBuf[0], &blackSource, &gBlackImageView, &blackRect, 0);
		dkQueueSubmitCommands(gSwitchQueue, dkCmdBufFinishList(gCmdBuf[0]));
		dkQueueWaitIdle(gSwitchQueue);
		if(!dkQueueIsInErrorState(gSwitchQueue))
		{
			gGpuBlitReady = 1;
			OsSwitch_Log("graphics: hardware 2D blitter ready (%ux%u max source)",
				(unsigned)SWITCH_BLIT_MAX_WIDTH, (unsigned)SWITCH_BLIT_MAX_HEIGHT);
		}
		else
			OsSwitch_Log("graphics: hardware 2D blitter init failed; using CPU fallback");
	}
#endif

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
	if(gBlitMem)
	{
		dkMemBlockDestroy(gBlitMem);
		gBlitMem = NULL;
	}
	if(gBaseMem)
	{
		dkMemBlockDestroy(gBaseMem);
		gBaseMem = NULL;
	}
	gBaseValid = 0;
	if(gSwitchDevice)
	{
		dkDeviceDestroy(gSwitchDevice);
		gSwitchDevice = NULL;
	}
	gGraphicsReady = 0;
	gGpuBlitReady = 0;
	gFrameActive = 0;
	gFrameInitialized = 0;
	gFrameImageSlot = -1;
	gLastPresentedValid = 0;
	gLastPresentedSlot = -1;
	gFrameUploadSlot = -1;
	gFrameUploadOffset = 0;
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

static int OsSwitch_GraphicsAcquireUploadSlot(void)
{
	int upload = (int)(gUploadIndex++ % SWITCH_UPLOAD_COUNT);
	if(gUploadFenceUsed[upload])
	{
		DkResult waitResult;
		// A queue flush is required before waiting for a fence belonging to
		// commands which have not yet been flushed by present.
		dkQueueFlush(gSwitchQueue);
		waitResult = dkFenceWait(&gUploadFence[upload], -1);
		if(waitResult != DkResult_Success)
		{
			int queueError = dkQueueIsInErrorState(gSwitchQueue) ? 1 : 0;
			if(queueError)
				gQueueErrorLogged = 1;
			OsSwitch_Log("graphics: blit upload fence wait failed: %d (queue error=%d)",
				(int)waitResult, queueError);
			return -1;
		}
	}
	return upload;
}

static void OsSwitch_GraphicsSubmitUploadSlot(int upload)
{
	dkQueueSubmitCommands(gSwitchQueue, dkCmdBufFinishList(gCmdBuf[upload]));
	dkQueueSignalFence(gSwitchQueue, &gUploadFence[upload], false);
	gUploadFenceUsed[upload] = 1;
}

static int OsSwitch_GraphicsQueueBlack(void)
{
	DkImageRect srcRect = {0, 0, 0, SWITCH_BLIT_BLACK_SIZE, SWITCH_BLIT_BLACK_SIZE, 1};
	DkImageRect dstRect = {0, 0, 0, gSwitchFramebufferWidth, gSwitchFramebufferHeight, 1};
	if(!gGpuBlitReady || !gFrameActive || gFrameUploadSlot < 0)
		return 0;
	dkCmdBufBlitImage(gCmdBuf[gFrameUploadSlot], &gBlackImageView, &srcRect,
		&gBaseView, &dstRect,
		DkBlitFlag_ModeBlit | DkBlitFlag_FilterNearest, 0);
	gFrameInitialized = 1;
	gBaseValid = 1;
	return 1;
}

int OsSwitch_GraphicsBeginFrame(void)
{
	int slot;
	if(!gGraphicsReady || !gGpuBlitReady)
		return 0;
	if(gFrameActive)
		return 1;
	if(gQueueErrorLogged || dkQueueIsInErrorState(gSwitchQueue))
	{
		gQueueErrorLogged = 1;
		return 0;
	}
	slot = dkQueueAcquireImage(gSwitchQueue, gSwitchSwapchain);
	if(slot < 0 || slot >= SWITCH_FB_COUNT)
		return 0;
	gFrameImageSlot = slot;
	gFrameActive = 1;
	gFrameInitialized = 0;
	gFrameUploadSlot = OsSwitch_GraphicsAcquireUploadSlot();
	gFrameUploadOffset = 0;
	if(gFrameUploadSlot < 0)
	{
		gFrameActive = 0;
		gFrameImageSlot = -1;
		return 0;
	}
	dkCmdBufClear(gCmdBuf[gFrameUploadSlot]);
	// the base image keeps the picture between frames; only the very first one starts black
	if(!gBaseValid && !OsSwitch_GraphicsQueueBlack())
	{
		gFrameActive = 0;
		gFrameImageSlot = -1;
		return 0;
	}
	gFrameInitialized = 1;
	return 1;
}

int OsSwitch_GraphicsFillBlack(void)
{
	if(!OsSwitch_GraphicsBeginFrame())
		return 0;
	return OsSwitch_GraphicsQueueBlack();
}

static uint32_t OsSwitch_SurfaceRgb(const void* pixels, int pitch, int bpp, int x, int y)
{
	const uint8_t* row = (const uint8_t*)pixels + (size_t)y * (size_t)pitch;
	if(bpp == 16)
	{
		uint16_t v = *(const uint16_t*)(const void*)(row + (size_t)x * 2);
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
		r = (r << 3) | (r >> 2);
		g = (g << 3) | (g >> 2);
		b = (b << 3) | (b >> 2);
		return (r << 16) | (g << 8) | b;
	}
	return (*(const uint32_t*)(const void*)(row + (size_t)x * 4)) & 0x00ffffffu;
}

static uint32_t OsSwitch_PackRgb(uint32_t rgb)
{
	return 0xff000000u | ((rgb & 0xffu) << 16) | (rgb & 0x0000ff00u) | ((rgb >> 16) & 0xffu);
}

static uint32_t OsSwitch_LerpRgb(uint32_t a, uint32_t b, float t)
{
	uint32_t r = (uint32_t)((float)((a >> 16) & 255u) * (1.0f - t) + (float)((b >> 16) & 255u) * t + 0.5f);
	uint32_t g = (uint32_t)((float)((a >> 8) & 255u) * (1.0f - t) + (float)((b >> 8) & 255u) * t + 0.5f);
	uint32_t bl = (uint32_t)((float)(a & 255u) * (1.0f - t) + (float)(b & 255u) * t + 0.5f);
	return (r << 16) | (g << 8) | bl;
}

static void OsSwitch_PackSurface(const void* pixels, int pitch, int bpp,
	int srcX, int srcY, int srcW, int srcH, uint32_t* output, int outW, int outH)
{
	int x, y;
	if(srcW == outW && srcH == outH)
	{
		for(y = 0; y < outH; y++)
		{
			for(x = 0; x < outW; x++)
				output[(size_t)y * (size_t)outW + x] = OsSwitch_PackRgb(
					OsSwitch_SurfaceRgb(pixels, pitch, bpp, srcX + x, srcY + y));
		}
		return;
	}
	// Extremely large source rectangles are reduced once on the CPU to fit
	// the scratch image. Normal-sized surfaces are uploaded without resampling.
	for(y = 0; y < outH; y++)
	{
		float fy = ((float)y + 0.5f) * (float)srcH / (float)outH - 0.5f;
		int y0, y1;
		float ty;
		if(fy < 0.0f) fy = 0.0f;
		if(fy > (float)(srcH - 1)) fy = (float)(srcH - 1);
		y0 = (int)fy;
		y1 = y0 + 1 < srcH ? y0 + 1 : y0;
		ty = fy - (float)y0;
		for(x = 0; x < outW; x++)
		{
			float fx = ((float)x + 0.5f) * (float)srcW / (float)outW - 0.5f;
			int x0, x1;
			float tx;
			uint32_t p00, p10, p01, p11, top, bottom;
			if(fx < 0.0f) fx = 0.0f;
			if(fx > (float)(srcW - 1)) fx = (float)(srcW - 1);
			x0 = (int)fx;
			x1 = x0 + 1 < srcW ? x0 + 1 : x0;
			tx = fx - (float)x0;
			p00 = OsSwitch_SurfaceRgb(pixels, pitch, bpp, srcX + x0, srcY + y0);
			p10 = OsSwitch_SurfaceRgb(pixels, pitch, bpp, srcX + x1, srcY + y0);
			p01 = OsSwitch_SurfaceRgb(pixels, pitch, bpp, srcX + x0, srcY + y1);
			p11 = OsSwitch_SurfaceRgb(pixels, pitch, bpp, srcX + x1, srcY + y1);
			top = OsSwitch_LerpRgb(p00, p10, tx);
			bottom = OsSwitch_LerpRgb(p01, p11, tx);
			output[(size_t)y * (size_t)outW + x] = OsSwitch_PackRgb(OsSwitch_LerpRgb(top, bottom, ty));
		}
	}
}

int OsSwitch_GraphicsBlit(const void* pixels, int pitch, int bpp, int surfaceW, int surfaceH,
	int srcX, int srcY, int srcW, int srcH, int dstX, int dstY, int dstW, int dstH)
{
	int upload, outW, outH;
	uint32_t bytes, uploadOffset;
	uint32_t* uploadPixels;
	DkCopyBuf src;
	DkImageRect srcRect, dstRect;
	uint32_t filter;
	if(!pixels || pitch <= 0 || (bpp != 16 && bpp != 32) || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
		return 0;
	if(srcX < 0 || srcY < 0 || srcX + srcW > surfaceW || srcY + srcH > surfaceH)
		return 0;
	if(dstX < 0 || dstY < 0 || dstX + dstW > (int)gSwitchFramebufferWidth ||
		dstY + dstH > (int)gSwitchFramebufferHeight)
		return 0;
	if(!OsSwitch_GraphicsBeginFrame())
		return 0;
	if(!gFrameInitialized && !OsSwitch_GraphicsQueueBlack())
		return 0;

	outW = srcW;
	outH = srcH;
	if(outW > SWITCH_BLIT_MAX_WIDTH || outH > SWITCH_BLIT_MAX_HEIGHT)
	{
		float scaleX = (float)SWITCH_BLIT_MAX_WIDTH / (float)outW;
		float scaleY = (float)SWITCH_BLIT_MAX_HEIGHT / (float)outH;
		float scale = scaleX < scaleY ? scaleX : scaleY;
		outW = (int)((float)outW * scale + 0.5f);
		outH = (int)((float)outH * scale + 0.5f);
		if(outW < 1) outW = 1;
		if(outH < 1) outH = 1;
	}
	bytes = (uint32_t)outW * (uint32_t)outH * 4u;
	if(bytes > SWITCH_BLIT_UPLOAD_SIZE)
		return 0;

	bytes = (bytes + 0x3fu) & ~0x3fu;
	if(gFrameUploadSlot < 0)
		return 0;
	uploadOffset = (gFrameUploadOffset + 0x3fu) & ~0x3fu;
	if(uploadOffset + bytes > SWITCH_BLIT_UPLOAD_SIZE)
	{
		// Submit this batch and continue in another fenced slot. This keeps
		// dirty-rectangle frames to one command submission in the usual case.
		OsSwitch_GraphicsSubmitUploadSlot(gFrameUploadSlot);
		upload = OsSwitch_GraphicsAcquireUploadSlot();
		if(upload < 0)
			return 0;
		gFrameUploadSlot = upload;
		gFrameUploadOffset = 0;
		uploadOffset = 0;
		dkCmdBufClear(gCmdBuf[upload]);
	}
	upload = gFrameUploadSlot;
	uploadPixels = (uint32_t*)((uint8_t*)dkMemBlockGetCpuAddr(gUploadMem[upload]) + uploadOffset);
	OsSwitch_PackSurface(pixels, pitch, bpp, srcX, srcY, srcW, srcH, uploadPixels, outW, outH);
	dkMemBlockFlushCpuCache(gUploadMem[upload], uploadOffset, bytes);
	gFrameUploadOffset = uploadOffset + bytes;

	src.addr = dkMemBlockGetGpuAddr(gUploadMem[upload]) + uploadOffset;
	src.rowLength = 0;
	src.imageHeight = 0;
	srcRect.x = 0; srcRect.y = 0; srcRect.z = 0;
	srcRect.width = (uint32_t)outW; srcRect.height = (uint32_t)outH; srcRect.depth = 1;
	dstRect.x = (uint32_t)dstX; dstRect.y = (uint32_t)dstY; dstRect.z = 0;
	dstRect.width = (uint32_t)dstW; dstRect.height = (uint32_t)dstH; dstRect.depth = 1;
	filter = DkBlitFlag_ModeBlit;
	filter |= (outW != dstW || outH != dstH) ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest;

	dkCmdBufCopyBufferToImage(gCmdBuf[upload], &src, &gBlitImageView, &srcRect, 0);
	dkCmdBufBlitImage(gCmdBuf[upload], &gBlitImageView, &srcRect,
		&gBaseView, &dstRect, filter, 0);
	gLastBlitNs = OsSwitch_TicksNs();
	gBlitSinceBars = 1;
	return 1;
}

static void OsSwitch_GraphicsQueueBlackRect(int x, int y, int w, int h)
{
	DkImageRect srcRect = {0, 0, 0, SWITCH_BLIT_BLACK_SIZE, SWITCH_BLIT_BLACK_SIZE, 1};
	DkImageRect dstRect;
	if(w <= 0 || h <= 0)
		return;
	dstRect.x = (uint32_t)x; dstRect.y = (uint32_t)y; dstRect.z = 0;
	dstRect.width = (uint32_t)w; dstRect.height = (uint32_t)h; dstRect.depth = 1;
	dkCmdBufBlitImage(gCmdBuf[gFrameUploadSlot], &gBlackImageView, &srcRect, &gBaseView, &dstRect,
		DkBlitFlag_ModeBlit | DkBlitFlag_FilterNearest, 0);
}

static int OsSwitch_GraphicsBarsExist(void)
{
	int l, t, r, b;
	OsSwitch_PictureRect(&l, &t, &r, &b);
	return l > 0 || t > 0 || r < (int)gSwitchFramebufferWidth || b < (int)gSwitchFramebufferHeight;
}

static int OsSwitch_GraphicsBarsDue(uint64_t now)
{
	return gBlitSinceBars && now - gLastBlitNs >= BARS_QUIET_NS && now - gBarsClearedNs >= BARS_MIN_INTERVAL_NS;
}

// black over the bars of the base image (the picture itself is not touched)
static void OsSwitch_GraphicsClearBars(void)
{
	int l, t, r, b, w = (int)gSwitchFramebufferWidth, h = (int)gSwitchFramebufferHeight;
	OsSwitch_PictureRect(&l, &t, &r, &b);
	OsSwitch_GraphicsQueueBlackRect(0, 0, w, t);
	OsSwitch_GraphicsQueueBlackRect(0, b, w, h - b);
	OsSwitch_GraphicsQueueBlackRect(0, t, l, b - t);
	OsSwitch_GraphicsQueueBlackRect(r, t, w - r, b - t);
}

// called from the main loop: once the drawing has stopped, present a frame that clears the bars
void OsSwitch_GraphicsMaintain(void)
{
	uint64_t now;
	if(!gGpuBlitReady || !gBlitSinceBars)
		return;
	now = OsSwitch_TicksNs();
	if(!OsSwitch_GraphicsBarsDue(now))
		return;
	if(!OsSwitch_GraphicsBarsExist())
	{ // no bars (a picture that fills the screen): nothing to clear
		gBlitSinceBars = 0;
		return;
	}
	OsSwitch_GraphicsPresentCursor();
}

/* Copy the cursor-free base into the acquired image and draw the cursor on
 * the copy.  The arrow is drawn as horizontal runs, each a blit of one
 * pixel (black or white) of a 2 x 1 patch staged in the scratch image. */
static void OsSwitch_GraphicsComposeToSlot(void)
{
	DkImageRect full = {0, 0, 0, gSwitchFramebufferWidth, gSwitchFramebufferHeight, 1};
	DkImageRect patch = {0, 0, 0, 2, 1, 1};
	DkCopyBuf src;
	const char* const* bmp;
	uint32_t uploadOffset;
	uint32_t* px;
	int sx, sy, cw, ch, y, x, slot = gFrameUploadSlot;

	if(slot < 0)
		return;
	{
		uint64_t now = OsSwitch_TicksNs();
		if(OsSwitch_GraphicsBarsDue(now))
		{
			if(OsSwitch_GraphicsBarsExist())
				OsSwitch_GraphicsClearBars();
			gBlitSinceBars = 0;
			gBarsClearedNs = now;
		}
	}
	dkCmdBufBlitImage(gCmdBuf[slot], &gBaseView, &full, &gImageViews[gFrameImageSlot], &full,
		DkBlitFlag_ModeBlit | DkBlitFlag_FilterNearest, 0);
	if(!OsSwitch_CursorVisible(&sx, &sy))
		return;
	uploadOffset = (gFrameUploadOffset + 0x3fu) & ~0x3fu;
	if(uploadOffset + 64u > SWITCH_BLIT_UPLOAD_SIZE)
		return; // no room left in this slot: no cursor this frame
	px = (uint32_t*)((uint8_t*)dkMemBlockGetCpuAddr(gUploadMem[slot]) + uploadOffset);
	px[0] = 0xff000000u; // black (RGBA in memory)
	px[1] = 0xffffffffu; // white
	dkMemBlockFlushCpuCache(gUploadMem[slot], uploadOffset, 64);
	gFrameUploadOffset = uploadOffset + 64u;
	src.addr = dkMemBlockGetGpuAddr(gUploadMem[slot]) + uploadOffset;
	src.rowLength = 0;
	src.imageHeight = 0;
	dkCmdBufCopyBufferToImage(gCmdBuf[slot], &src, &gBlitImageView, &patch, 0);

	bmp = OsSwitch_CursorBitmap(&cw, &ch);
	for(y = 0; y < ch; y++)
	{
		int dy = sy + y;
		if(dy < 0 || dy >= (int)gSwitchFramebufferHeight)
			continue;
		for(x = 0; x < cw;)
		{
			char c = bmp[y][x];
			int start = x, dx, len;
			DkImageRect from, to;
			while(x < cw && bmp[y][x] == c)
				x++;
			if(c == ' ')
				continue;
			dx = sx + start;
			len = x - start;
			if(dx < 0)
			{
				len += dx;
				dx = 0;
			}
			if(dx + len > (int)gSwitchFramebufferWidth)
				len = (int)gSwitchFramebufferWidth - dx;
			if(len <= 0)
				continue;
			from.x = c == '#' ? 0 : 1; from.y = 0; from.z = 0; from.width = 1; from.height = 1; from.depth = 1;
			to.x = (uint32_t)dx; to.y = (uint32_t)dy; to.z = 0; to.width = (uint32_t)len; to.height = 1; to.depth = 1;
			dkCmdBufBlitImage(gCmdBuf[slot], &gBlitImageView, &from, &gImageViews[gFrameImageSlot], &to,
				DkBlitFlag_ModeBlit | DkBlitFlag_FilterNearest, 0);
		}
	}
}

// A frame for the cursor alone: it moved, but the engine has nothing to draw.
void OsSwitch_GraphicsPresentCursor(void)
{
	if(gGpuBlitReady)
	{
		if(OsSwitch_GraphicsBeginFrame())
			OsSwitch_GraphicsPresent();
	}
	else
		OsSwitch_GraphicsPresent();
}

static void OsSwitch_GraphicsMeasurePresent(void)
{
	uint64_t now = OsSwitch_TicksNs();
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


void OsSwitch_GraphicsPresent(void)
{
	static int sFirst;
	int slot, upload;
	if(!sFirst)
	{
		sFirst = 1;
		OsSwitch_Log("first frame presented");
	}
	DkCopyBuf src;
	DkImageRect dst;
	DkResult waitResult;

	if(!gGraphicsReady || !gFramebuffer)
		return;

	if(gFrameActive && gGpuBlitReady)
	{
		OsSwitch_WindowCompositeChildrenGpu();
		if(!gFrameInitialized && !OsSwitch_GraphicsQueueBlack())
			return;
		if(gQueueErrorLogged || dkQueueIsInErrorState(gSwitchQueue))
		{
			gQueueErrorLogged = 1;
			gFrameActive = 0;
			gFrameInitialized = 0;
			return;
		}
		OsSwitch_GraphicsComposeToSlot();
		if(gFrameUploadSlot >= 0)
		{
			OsSwitch_GraphicsSubmitUploadSlot(gFrameUploadSlot);
			gFrameUploadSlot = -1;
			gFrameUploadOffset = 0;
		}
		dkQueuePresentImage(gSwitchQueue, gSwitchSwapchain, gFrameImageSlot);
		gLastPresentedSlot = gFrameImageSlot;
		gLastPresentedValid = 1;
		gFrameActive = 0;
		gFrameInitialized = 0;
		gFrameImageSlot = -1;
		OsSwitch_GraphicsMeasurePresent();
		return;
	}

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

	OsSwitch_CursorDraw();
	memcpy(dkMemBlockGetCpuAddr(gUploadMem[upload]), gFramebuffer, gUploadSize);
	OsSwitch_CursorErase();
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

	OsSwitch_GraphicsMeasurePresent();
}

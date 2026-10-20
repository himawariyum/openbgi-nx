/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - Switch back end: audio output through audren (inc/bgi/os.h)
 *
 * The engine mixes at 44100 Hz; audren only offers 32 kHz and 48 kHz, so
 * everything is resampled to 48 kHz with linear interpolation.  A small
 * thread keeps a handful of wave buffers queued on a single stereo voice
 * and pulls new frames from the engine's fill callback as they finish.
 */
#include "bgi/os.h"
#include "bgi/os_switch.h"

#include <math.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#define SWITCH_AUDIO_RATE     48000
#define SWITCH_AUDIO_FRAMES   1024 // output frames per wave buffer
#define SWITCH_AUDIO_BUFFERS  4
#define SWITCH_AUDIO_FIFO     8192 // input frames held for resampling

typedef struct OsSwitchAudio
{
	AudioDriver driver;
	AudioDriverWaveBuf waves[SWITCH_AUDIO_BUFFERS];
	void* pool;
	size_t poolSize;
	Thread thread;
	int threadStarted;
	volatile int running;
	int driverOpen;
	int audrenOpen;
	int poolId;
	int inRate;
	double step; // input frames per output frame
	double frac; // position between the two input frames in the FIFO
	int16_t fifo[SWITCH_AUDIO_FIFO * 2];
	int fifoCount;
	OsAudioFill_t fill;
	void* user;
} OsSwitchAudio;

static OsSwitchAudio gAudio;

static const AudioRendererConfig gAudioConfig = {
	.output_rate = AudioRendererOutputRate_48kHz,
	.num_voices = 1,
	.num_effects = 0,
	.num_sinks = 1,
	.num_mix_objs = 1,
	.num_mix_buffers = 2,
};

// Ask the engine for more input frames, appended to the FIFO.
static void RefillFifo(int frames)
{
	int room;
	if(!gAudio.fill || frames <= 0)
		return;
	room = SWITCH_AUDIO_FIFO - gAudio.fifoCount;
	if(frames > room)
		frames = room;
	if(frames <= 0)
		return;
	gAudio.fill(gAudio.user, gAudio.fifo + gAudio.fifoCount * 2, frames);
	gAudio.fifoCount += frames;
}

// Resample the FIFO into one wave buffer (stereo interleaved).
static void FillWaveBuffer(int16_t* out)
{
	int n, required, consumed;
	double pos;

	required = (int)(gAudio.frac + SWITCH_AUDIO_FRAMES * gAudio.step) + 2;
	if(gAudio.fifoCount < required)
		RefillFifo(required - gAudio.fifoCount);
	if(gAudio.fifoCount < 2)
	{
		memset(out, 0, sizeof(int16_t) * 2 * SWITCH_AUDIO_FRAMES);
		return;
	}

	pos = gAudio.frac;
	for(n = 0; n < SWITCH_AUDIO_FRAMES; n++)
	{
		int i = (int)pos;
		double mu = pos - (double)i;
		int16_t l0, r0, l1, r1;
		if(i >= gAudio.fifoCount - 1)
			i = gAudio.fifoCount - 2;
		if(i < 0)
			i = 0;
		l0 = gAudio.fifo[i * 2 + 0];
		r0 = gAudio.fifo[i * 2 + 1];
		l1 = gAudio.fifo[(i + 1) * 2 + 0];
		r1 = gAudio.fifo[(i + 1) * 2 + 1];
		out[n * 2 + 0] = (int16_t)(l0 + (l1 - l0) * mu);
		out[n * 2 + 1] = (int16_t)(r0 + (r1 - r0) * mu);
		pos += gAudio.step;
	}

	consumed = (int)pos;
	if(consumed > gAudio.fifoCount - 1)
		consumed = gAudio.fifoCount - 1;
	if(consumed > 0)
	{
		memmove(gAudio.fifo, gAudio.fifo + consumed * 2,
			sizeof(int16_t) * 2 * (size_t)(gAudio.fifoCount - consumed));
		gAudio.fifoCount -= consumed;
		pos -= consumed;
	}
	gAudio.frac = pos;
}

// Queue a finished (or never used) wave buffer again.
static int QueueWave(AudioDriverWaveBuf* w)
{
	int16_t* data;
	if(!gAudio.running)
		return 0;
	data = (int16_t*)(void*)w->data_raw;
	FillWaveBuffer(data);
	armDCacheFlush(data, w->size);
	w->state = AudioDriverWaveBufState_Free;
	w->is_looping = false;
	w->start_sample_offset = 0;
	w->end_sample_offset = SWITCH_AUDIO_FRAMES;
	return audrvVoiceAddWaveBuf(&gAudio.driver, 0, w) ? 1 : 0;
}

static void AudioThreadFunc(void* unused)
{
	int i;
	BGI_UNUSED(unused);
	while(gAudio.running)
	{
		for(i = 0; i < SWITCH_AUDIO_BUFFERS; i++)
		{
			AudioDriverWaveBufState st = gAudio.waves[i].state;
			if(st == AudioDriverWaveBufState_Free || st == AudioDriverWaveBufState_Done)
				QueueWave(&gAudio.waves[i]);
		}
		audrvUpdate(&gAudio.driver);
		svcSleepThread(4000000ull); // 4 ms
	}
}

int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	static const u8 sinkChannels[] = {0, 1};
	Result rc;
	int i;

	if(!fill)
		return 0;
	OS_AudioStop();

	memset(&gAudio, 0, sizeof gAudio);
	gAudio.inRate = rate > 0 ? rate : 44100;
	gAudio.step = (double)gAudio.inRate / (double)SWITCH_AUDIO_RATE;
	gAudio.fill = fill;
	gAudio.user = user;

	rc = audrenInitialize(&gAudioConfig);
	if(R_FAILED(rc))
	{
		printf("audio: audrenInitialize failed: 0x%x\n", rc);
		return 0;
	}
	gAudio.audrenOpen = 1;

	rc = audrvCreate(&gAudio.driver, &gAudioConfig, 2);
	if(R_FAILED(rc))
	{
		printf("audio: audrvCreate failed: 0x%x\n", rc);
		OS_AudioStop();
		return 0;
	}
	gAudio.driverOpen = 1;

	gAudio.poolSize = (size_t)SWITCH_AUDIO_FRAMES * 2 * sizeof(int16_t) * SWITCH_AUDIO_BUFFERS;
	gAudio.poolSize = (gAudio.poolSize + 0xfff) & ~(size_t)0xfff;
	gAudio.pool = memalign(0x1000, gAudio.poolSize);
	if(!gAudio.pool)
	{
		printf("audio: wave buffer allocation failed\n");
		OS_AudioStop();
		return 0;
	}
	memset(gAudio.pool, 0, gAudio.poolSize);

	gAudio.poolId = audrvMemPoolAdd(&gAudio.driver, gAudio.pool, gAudio.poolSize);
	if(gAudio.poolId < 0)
	{
		printf("audio: audrvMemPoolAdd failed\n");
		OS_AudioStop();
		return 0;
	}
	audrvMemPoolAttach(&gAudio.driver, gAudio.poolId);
	audrvDeviceSinkAdd(&gAudio.driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sinkChannels);
	audrvUpdate(&gAudio.driver);
	rc = audrenStartAudioRenderer();
	if(R_FAILED(rc))
	{
		printf("audio: audrenStartAudioRenderer failed: 0x%x\n", rc);
		OS_AudioStop();
		return 0;
	}

	if(!audrvVoiceInit(&gAudio.driver, 0, 2, PcmFormat_Int16, SWITCH_AUDIO_RATE))
	{
		printf("audio: audrvVoiceInit failed\n");
		OS_AudioStop();
		return 0;
	}
	audrvVoiceSetDestinationMix(&gAudio.driver, 0, AUDREN_FINAL_MIX_ID);
	audrvVoiceSetMixFactor(&gAudio.driver, 0, 1.0f, 0, 0);
	audrvVoiceSetMixFactor(&gAudio.driver, 0, 1.0f, 0, 1);
	audrvVoiceSetVolume(&gAudio.driver, 0, 1.0f);
	audrvVoiceStart(&gAudio.driver, 0);

	gAudio.running = 1;
	for(i = 0; i < SWITCH_AUDIO_BUFFERS; i++)
	{
		int16_t* data = (int16_t*)((uint8_t*)gAudio.pool + (size_t)i * SWITCH_AUDIO_FRAMES * 2 * sizeof(int16_t));
		AudioDriverWaveBuf* w = &gAudio.waves[i];
		memset(w, 0, sizeof *w);
		w->data_raw = data;
		w->size = (u64)SWITCH_AUDIO_FRAMES * 2 * sizeof(int16_t);
		w->start_sample_offset = 0;
		w->end_sample_offset = SWITCH_AUDIO_FRAMES;
		w->state = AudioDriverWaveBufState_Free;
		QueueWave(w);
	}
	audrvUpdate(&gAudio.driver);

	if(R_FAILED(threadCreate(&gAudio.thread, AudioThreadFunc, NULL, NULL, 0x4000, 0x2c, -2)))
	{
		printf("audio: threadCreate failed\n");
		gAudio.running = 0;
		OS_AudioStop();
		return 0;
	}
	if(R_FAILED(threadStart(&gAudio.thread)))
	{
		printf("audio: threadStart failed\n");
		gAudio.running = 0;
		threadClose(&gAudio.thread);
		OS_AudioStop();
		return 0;
	}
	gAudio.threadStarted = 1;
	printf("audio: %d Hz -> %d Hz, %d buffers\n", gAudio.inRate, SWITCH_AUDIO_RATE, SWITCH_AUDIO_BUFFERS);
	return 1;
}

void OS_AudioStop(void)
{
	if(gAudio.threadStarted)
	{
		gAudio.running = 0;
		threadWaitForExit(&gAudio.thread);
		threadClose(&gAudio.thread);
		gAudio.threadStarted = 0;
	}
	gAudio.running = 0;
	if(gAudio.driverOpen)
	{
		audrvClose(&gAudio.driver);
		gAudio.driverOpen = 0;
	}
	if(gAudio.audrenOpen)
	{
		audrenExit();
		gAudio.audrenOpen = 0;
	}
	if(gAudio.pool)
	{
		free(gAudio.pool);
		gAudio.pool = NULL;
	}
	memset(&gAudio, 0, sizeof gAudio);
}

int OS_AudioAvailable(void)
{
	return 1;
}

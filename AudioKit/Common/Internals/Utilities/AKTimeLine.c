//
//  AKTimeLine.c
//  AudioKit
//
//  Created by David O'Neill on 8/28/17.
//  Copyright © 2017 AudioKit. All rights reserved.
//

#include "AKTimeLine.h"
#include <mach/mach_time.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>
#include <libc.h>

const AudioTimeStamp AudioTimeZero = {};



typedef struct {
    Float64         loopStart;
    Float64         loopEnd;
    AudioTimeStamp  baseTime;
    AudioTimeStamp  waitStart;
}AKTimeLineMessage;


static AudioTimeStamp TimeStampOffset(AudioTimeStamp timeStamp, SInt64 samples, double sampleRate);
static Boolean SampleAndHostTimeValid(AudioTimeStamp timeStamp);
static Boolean SampleTimeValid(AudioTimeStamp timeStamp);
static Boolean HostTimeValid(AudioTimeStamp timeStamp);
static AudioTimeStamp AudioTimeNow();
static AudioTimeStamp AudioTimeStampWithSampleHost(Float64 sampleTime, UInt64 hostTime);
static AudioTimeStamp extrapolateTime(AudioTimeStamp timeStamp, AudioTimeStamp anchorTime, double sampleRate);
static SInt64 safeSubtract(UInt64 a, UInt64 b);
static double ticksToSeconds();
static void AKTimelineSendMessage(AKTimeline *timeLine, AKTimeLineMessage message);
void AKTimelineSyncronize(AKTimeline *timeline);



void AKTimeLineInit(AKTimeline *timeline, double sampleRate, AKTimeLineCallback callback, void *callbackRef) {
    memset(timeline, 0, sizeof(AKTimeline));
    timeline->sampleRate = sampleRate;
    TPCircularBufferInit(&timeline->messageQueue, sizeof(AKTimeLineMessage) * 32);
    pthread_mutex_init(&timeline->messageQueueLock, NULL);
    timeline->callBack = callback;
    timeline->callbackRef = callbackRef;
}

void AKTimelineStart(AKTimeline *timeline) {
    while (!timeline->lastRenderFrames) {
        usleep(1000);
    }
    AudioTimeStamp nextRender = TimeStampOffset(timeline->lastRenderTime, timeline->lastRenderFrames, timeline->sampleRate);
    AKTimelineStartAtTime(timeline, nextRender);
}
void AKTimelineStartAtTime(AKTimeline *timeline, AudioTimeStamp audioTime) {
    if (AKTimelineIsStarted(timeline)) {
        return;
    }
    if (SampleAndHostTimeValid(timeline->anchorTime)) {
        audioTime = extrapolateTime(audioTime, timeline->anchorTime, timeline->sampleRate);
    }
    timeline->waitStart = audioTime;
    AKTimelineSetState(timeline, timeline->idleTime, timeline->loopStart, timeline->loopEnd, audioTime);
}
void AKTimelineSetLoop(AKTimeline *timeline, Float64 start, Float64 duration) {
    assert(start >= 0 && duration >= 0);
    timeline->loopStart = start;
    timeline->loopEnd = timeline->loopStart + duration;
    AKTimelineSyncronize(timeline);
}
Float64 AKTimelineTimeAtTime(AKTimeline *timeline, AudioTimeStamp audioTime) {
    if (!AKTimelineIsStarted(timeline)) {
        return timeline->idleTime;
    }
    if (!SampleAndHostTimeValid(timeline->anchorTime)) {
        return timeline->idleTime;
    }

    if (!SampleTimeValid(timeline->baseTime)) {
        timeline->baseTime = extrapolateTime(timeline->baseTime, timeline->anchorTime, timeline->sampleRate);
    }
    if (!SampleTimeValid(audioTime)) {
        audioTime = extrapolateTime(audioTime, timeline->anchorTime, timeline->sampleRate);
    }
    Float64 sampleTimeTotal = audioTime.mSampleTime - timeline->_baseTime.mSampleTime;
    if (!timeline->_loopEnd || sampleTimeTotal <= timeline->loopEnd) {
        return audioTime.mSampleTime - timeline->_baseTime.mSampleTime;
    }
    return timeline->_loopStart + fmod(sampleTimeTotal - timeline->_loopStart, timeline->_loopEnd - timeline->loopStart);
}
Float64 AKTimelineTime(AKTimeline *timeline) {
    return AKTimelineTimeAtTime(timeline, AudioTimeNow());
}
void AKTimelineStop(AKTimeline *timeline) {
    timeline->idleTime = AKTimelineTimeAtTime(timeline, AudioTimeNow());
    timeline->baseTime = AudioTimeZero;
    timeline->waitStart = AudioTimeZero;
    AKTimelineSyncronize(timeline);
}
void AKTimelineSyncronize(AKTimeline *timeline) {
    AKTimelineSendMessage(timeline, (AKTimeLineMessage) {
        .loopStart = timeline->loopStart,
        .loopEnd = timeline->loopEnd,
        .baseTime = timeline->baseTime,
        .waitStart = timeline->waitStart
    });
}

void AKTimelineSetTimeAtTime(AKTimeline *timeline, SInt64 sampleTime, AudioTimeStamp audioTime) {
    timeline->waitStart = AudioTimeZero;
    AKTimelineSetState(timeline, sampleTime, timeline->loopStart, timeline->loopEnd, audioTime);
}
void AKTimelineSetTime(AKTimeline *timeline, SInt64 sampleTime) {
    if (AKTimelineIsStarted(timeline)) {
        return AKTimelineSetTimeAtTime(timeline, sampleTime, AudioTimeNow());
    }
    timeline->idleTime = sampleTime;
}
Boolean AKTimelineIsStarted(AKTimeline *timeline) {
    return SampleTimeValid(timeline->baseTime) || HostTimeValid(timeline->baseTime);
}
void AKTimelineSetState(AKTimeline *timeline, SInt64 sampleTime, UInt32 loopSampleStart, UInt32 loopSampleEnd, AudioTimeStamp audioTime) {
    assert(SampleTimeValid(audioTime) || HostTimeValid(audioTime));
    audioTime = TimeStampOffset(audioTime, -sampleTime, timeline->sampleRate);
    if (SampleAndHostTimeValid(timeline->anchorTime)) {
        audioTime = extrapolateTime(audioTime, timeline->anchorTime, timeline->sampleRate);
    }
    timeline->baseTime = audioTime;
    timeline->loopStart = loopSampleStart;
    timeline->loopEnd = loopSampleEnd;
    AKTimelineSyncronize(timeline);
}
void AKTimelineSetRenderState(AKTimeline *timeline, Float64 sampleTime, Float64 loopStart, Float64 loopEnd, AudioTimeStamp audioTime) {
    assert(SampleTimeValid(audioTime) || HostTimeValid(audioTime));
    audioTime = TimeStampOffset(audioTime, -sampleTime, timeline->sampleRate);
    if (SampleAndHostTimeValid(timeline->anchorTime)) {
        audioTime = extrapolateTime(audioTime, timeline->anchorTime, timeline->sampleRate);
    }
    timeline->_baseTime = timeline->baseTime = audioTime;
    timeline->_loopStart = timeline->loopStart = loopStart;
    timeline->_loopEnd = timeline->loopEnd = loopEnd;
    timeline->_waitStart = timeline->waitStart = AudioTimeZero;

}
static void AKTimelineSendMessage(AKTimeline *timeLine, AKTimeLineMessage message) {
    AKTimeLineMessage *head = TPCircularBufferUnitHead(&timeLine->messageQueue, NULL, sizeof(AKTimeLineMessage));
    if (!head) {
        pthread_mutex_lock(&timeLine->messageQueueLock);
        while (!head) {
            TPCircularBufferUnitConsume(&timeLine->messageQueue);
            head = TPCircularBufferUnitHead(&timeLine->messageQueue, NULL, sizeof(AKTimeLineMessage));
        }
        pthread_mutex_unlock(&timeLine->messageQueueLock);
    }
    *head = message;
    TPCircularBufferUnitProduce(&timeLine->messageQueue);
}

void AKTimeLineRender(AKTimeline *timeline, const AudioTimeStamp *inTimeStamp, UInt32 inNumberFrames) {

    if(pthread_mutex_trylock(&timeline->messageQueueLock) == 0) {
        AKTimeLineMessage *message = TPCircularBufferUnitTail(&timeline->messageQueue, NULL, NULL);
        while (message) {
            timeline->_baseTime = message->baseTime;
            timeline->_loopStart = message->loopStart;
            timeline->_loopEnd = message->loopEnd;
            timeline->_waitStart = message->waitStart;
            TPCircularBufferUnitConsume(&timeline->messageQueue);
            message = TPCircularBufferUnitTail(&timeline->messageQueue, NULL, NULL);
        }
        pthread_mutex_unlock(&timeline->messageQueueLock);
    }
    if (!SampleAndHostTimeValid(timeline->anchorTime)) {
        timeline->anchorTime = *inTimeStamp;
    }
    timeline->lastRenderTime = *inTimeStamp;
    timeline->lastRenderFrames = inNumberFrames;

    if (!SampleTimeValid(timeline->_baseTime) && !HostTimeValid(timeline->_baseTime)) {
        return;
    }

    if (!SampleAndHostTimeValid(timeline->_baseTime)) {
        timeline->_baseTime = extrapolateTime(timeline->_baseTime, *inTimeStamp, timeline->sampleRate);
    }

    Float64 waitStart = 0;
    if (SampleTimeValid(timeline->_waitStart)) {
        waitStart = timeline->_waitStart.mSampleTime - timeline->_baseTime.mSampleTime;
    } else if (HostTimeValid(timeline->_waitStart)) {
        timeline->_waitStart = extrapolateTime(timeline->_waitStart, timeline->anchorTime, timeline->sampleRate);
        waitStart = timeline->_waitStart.mSampleTime - timeline->_baseTime.mSampleTime;
    }

    SInt64 startSample = waitStart > 0 ? waitStart : 0;


    AudioTimeStamp playerTime = AudioTimeStampWithSampleHost(inTimeStamp->mSampleTime - timeline->_baseTime.mSampleTime, inTimeStamp->mHostTime);

    UInt32 framesToRender = inNumberFrames;

//    Float64 samplesBelowZero = playerTime.mSampleTime < 0 ? -playerTime.mSampleTime : 0;
    Float64 samplesBelowZero = playerTime.mSampleTime < startSample ? startSample - playerTime.mSampleTime : 0;

    if (samplesBelowZero) {
        if (samplesBelowZero >= inNumberFrames) {
            //Won't reach zero
            return;
        }
        //Advance playerTime to zero
        framesToRender -= samplesBelowZero ;
        playerTime = TimeStampOffset(playerTime, samplesBelowZero, timeline->sampleRate);
    }

    // If non looping, render remaining frames.
    if (!timeline->_loopEnd) {
        if (timeline->callBack) {
            timeline->callBack(&playerTime, framesToRender, timeline->callbackRef);
        }
        return;
    }
    Float64 unlooped = playerTime.mSampleTime;
    UInt32 loopDur = timeline->_loopEnd - timeline->_loopStart;

    while (framesToRender) {
        if (unlooped >= timeline->_loopEnd) {
            playerTime.mSampleTime = timeline->_loopStart + fmod(unlooped - timeline->_loopStart, loopDur);
        }
        int frames = timeline->_loopEnd - playerTime.mSampleTime;
        frames = frames > framesToRender ? framesToRender : frames;
        if (timeline->callBack) {
            timeline->callBack(&playerTime, frames, timeline->callbackRef);
        }
        playerTime = TimeStampOffset(playerTime, frames, timeline->sampleRate);
        framesToRender -= frames;
        unlooped += frames;
    }

}

static AudioTimeStamp TimeStampOffset(AudioTimeStamp timeStamp, SInt64 samples, double sampleRate) {
    if (SampleTimeValid(timeStamp)) {
        timeStamp.mSampleTime += samples;
    }
    if (HostTimeValid(timeStamp)) {
        double seconds = samples / sampleRate;
        SInt64 offset = round(seconds / ticksToSeconds());
        if (offset < 0 && timeStamp.mHostTime < -offset) {
            //prevent UInt64 < 0
            return timeStamp;
        }
        timeStamp.mHostTime += offset;
    }
    return timeStamp;
}
static Boolean SampleAndHostTimeValid(AudioTimeStamp timeStamp) {
    return SampleTimeValid(timeStamp) && HostTimeValid(timeStamp);
}
static Boolean SampleTimeValid(AudioTimeStamp timeStamp) {
    return timeStamp.mFlags & kAudioTimeStampSampleTimeValid;
}
static Boolean HostTimeValid(AudioTimeStamp timeStamp) {
    return timeStamp.mFlags & kAudioTimeStampHostTimeValid;
}
static AudioTimeStamp AudioTimeNow() {
    return (AudioTimeStamp) {
        .mHostTime = mach_absolute_time(),
        .mFlags = kAudioTimeStampHostTimeValid
    };
}
static AudioTimeStamp AudioTimeStampWithSampleHost(Float64 sampleTime, UInt64 hostTime) {
    return (AudioTimeStamp) {
        .mSampleTime = sampleTime,
        .mHostTime = hostTime,
        .mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid
    };
}

static AudioTimeStamp extrapolateTime(AudioTimeStamp timeStamp, AudioTimeStamp anchorTime, double sampleRate){

    assert((SampleTimeValid(timeStamp) || HostTimeValid(timeStamp)) && SampleAndHostTimeValid(anchorTime));

    AudioTimeStamp result = timeStamp;
    if (SampleTimeValid(timeStamp)) {
        double secondsDiff = (timeStamp.mSampleTime - anchorTime.mSampleTime) / sampleRate;
        result.mHostTime = anchorTime.mHostTime + round(secondsDiff / ticksToSeconds());
        result.mFlags |= kAudioTimeStampHostTimeValid;
    } else {
        double secondsDiff = safeSubtract(timeStamp.mHostTime, anchorTime.mHostTime) * ticksToSeconds();
        result.mSampleTime = anchorTime.mSampleTime + round(secondsDiff * sampleRate);
        result.mFlags |= kAudioTimeStampSampleTimeValid;
    }
    return result;
}
static SInt64 safeSubtract(UInt64 a, UInt64 b) {
    return a >= b ? a - b : -(b - a);
}

static double ticksToSeconds(){
    static double ticksToSeconds = 0;
    if (!ticksToSeconds) {
        double timecon;
        mach_timebase_info_data_t tinfo;
        kern_return_t kerror;
        kerror = mach_timebase_info(&tinfo);
        timecon = (double)tinfo.numer / (double)tinfo.denom;
        ticksToSeconds = timecon * 0.000000001;
    }
    return ticksToSeconds;
}

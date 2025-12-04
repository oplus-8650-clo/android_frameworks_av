/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FRAME_DECODER_H_
#define FRAME_DECODER_H_

#include <memory>
#include <mutex>
#include <queue>
#include <vector>

// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
#include <media/stagefright/foundation/Mutexed.h>
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
#include <media/openmax/OMX_Video.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/AString.h>
#include <ui/GraphicTypes.h>
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
#include <utils/threads.h>
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder

namespace android {

struct AMessage;
struct MediaCodec;
class IMediaSource;
class MediaCodecBuffer;
class Surface;
class VideoFrame;
struct AsyncCodecHandler;

struct FrameRect {
    int32_t left, top, right, bottom;
};

struct InputBufferIndexQueue {
public:
    void enqueue(int32_t index);
    bool dequeue(int32_t* index, int32_t timeOutUs);

private:
    std::queue<int32_t> mQueue;
    std::mutex mMutex;
    std::condition_variable mCondition;
};

struct FrameDecoder : public RefBase {
    FrameDecoder(
            const AString &componentName,
            const sp<MetaData> &trackMeta,
            const sp<IMediaSource> &source);

    status_t init(int64_t frameTimeUs, int option, int colorFormat);

    sp<IMemory> extractFrame(FrameRect *rect = NULL);

    static sp<IMemory> getMetadataOnly(
            const sp<MetaData> &trackMeta, int colorFormat, bool preferHw,
            bool thumbnail = false, uint32_t bitDepth = 0);

    status_t handleInputBufferAsync(int32_t index);
    status_t handleOutputBufferAsync(int32_t index, int64_t timeUs);
    status_t handleOutputFormatChangeAsync(sp<AMessage> format);

    enum {
        kWhatCallbackNotify,
    };

protected:
    AString mComponentName;
    sp<AMessage> mOutputFormat;
    bool mUseBlockModel;

    virtual ~FrameDecoder();

    virtual sp<AMessage> onGetFormatAndSeekOptions(
            int64_t frameTimeUs,
            int seekMode,
            MediaSource::ReadOptions *options,
            sp<Surface> *window) = 0;

    virtual status_t onExtractRect(FrameRect *rect) = 0;

    virtual status_t onInputReceived(uint8_t* data, size_t size, MetaDataBase& sampleMeta,
                                     bool firstSample, uint32_t* flags) = 0;

    virtual status_t onOutputReceived(
            uint8_t* data,
            sp<ABuffer> imgObj,
            const sp<AMessage> &outputFormat,
            int64_t timeUs,
            bool *done) = 0;

// QTI_BEGIN: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
    virtual bool shouldDropOutput(int64_t ptsUs __unused) {
        return false;
    }

// QTI_END: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    virtual status_t extractInternal();

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    sp<MetaData> trackMeta()     const      { return mTrackMeta; }
    OMX_COLOR_FORMATTYPE dstFormat() const  { return mDstFormat; }
    ui::PixelFormat captureFormat() const   { return mCaptureFormat; }
    int32_t dstBpp()             const      { return mDstBpp; }
    void setFrame(const sp<IMemory> &frameMem) { mFrameMemory = frameMem; }
// QTI_BEGIN: 2018-04-26: Video: libstagefright: Enable optimizations for thumbnail session
    bool mIDRSent;
// QTI_END: 2018-04-26: Video: libstagefright: Enable optimizations for thumbnail session

// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    bool mHaveMoreInputs;
    bool mFirstSample;
    MediaSource::ReadOptions mReadOptions;
    sp<IMediaSource> mSource;
    sp<MediaCodec> mDecoder;
    sp<Surface> mSurface;

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
private:
    sp<MetaData> mTrackMeta;
    OMX_COLOR_FORMATTYPE mDstFormat;
    ui::PixelFormat mCaptureFormat;
    int32_t mDstBpp;
    sp<IMemory> mFrameMemory;
    sp<AsyncCodecHandler> mHandler;
    sp<ALooper> mAsyncLooper;
    bool mSourceStopped;
    bool mHandleOutputBufferAsyncDone;
    std::mutex mMutex;
    std::condition_variable mOutputFramePending;
    InputBufferIndexQueue mInputBufferIndexQueue;

    status_t extractInternalUsingBlockModel();

    DISALLOW_EVIL_CONSTRUCTORS(FrameDecoder);
};
struct FrameCaptureLayer;

struct AsyncCodecHandler : public AHandler {
public:
    explicit AsyncCodecHandler(const wp<FrameDecoder>& frameDecoder);
    virtual void onMessageReceived(const sp<AMessage>& msg);

private:
    wp<FrameDecoder> mFrameDecoder;
};

struct VideoFrameDecoder : public FrameDecoder {
    VideoFrameDecoder(
            const AString &componentName,
            const sp<MetaData> &trackMeta,
            const sp<IMediaSource> &source);

protected:
    virtual sp<AMessage> onGetFormatAndSeekOptions(
            int64_t frameTimeUs,
            int seekMode,
            MediaSource::ReadOptions *options,
            sp<Surface> *window) override;

    virtual status_t onExtractRect(FrameRect *rect) override {
        // Rect extraction for sequences is not supported for now.
        return (rect == NULL) ? OK : ERROR_UNSUPPORTED;
    }

    virtual status_t onInputReceived(uint8_t* data, size_t size, MetaDataBase& sampleMeta,
                                     bool firstSample, uint32_t* flags) override;

    virtual status_t onOutputReceived(
            uint8_t* data,
            sp<ABuffer> imgObj,
            const sp<AMessage> &outputFormat,
            int64_t timeUs,
            bool *done) override;

// QTI_BEGIN: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
    virtual bool shouldDropOutput(int64_t ptsUs) override {
        return !((mTargetTimeUs < 0LL) || (ptsUs >= mTargetTimeUs));
    }

// QTI_END: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
private:
    sp<FrameCaptureLayer> mCaptureLayer;
    VideoFrame *mFrame;
    bool mIsAvc;
    bool mIsHevc;
    MediaSource::ReadOptions::SeekMode mSeekMode;
    int64_t mTargetTimeUs;
    List<int64_t> mSampleDurations;
    int64_t mDefaultSampleDurationUs;

    sp<Surface> initSurface();
    status_t captureSurface();
};

// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
struct MediaImageDecoder : public FrameDecoder {
   MediaImageDecoder(
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
            const AString &componentName,
            const sp<MetaData> &trackMeta,
            const sp<IMediaSource> &source);

protected:
// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
    virtual ~MediaImageDecoder();
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class

    virtual sp<AMessage> onGetFormatAndSeekOptions(
            int64_t frameTimeUs,
            int seekMode,
            MediaSource::ReadOptions *options,
            sp<Surface> *window) override;

    virtual status_t onExtractRect(FrameRect *rect) override;

    virtual status_t onInputReceived(uint8_t* __unused, size_t __unused,
                                     MetaDataBase& sampleMeta __unused, bool firstSample __unused,
                                     uint32_t* flags __unused) override { return OK; }

    virtual status_t onOutputReceived(
            uint8_t* data,
            sp<ABuffer> imgObj,
            const sp<AMessage> &outputFormat,
            int64_t timeUs,
            bool *done) override;

// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    virtual status_t extractInternal() override;

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
private:
    VideoFrame *mFrame;
    int32_t mWidth;
    int32_t mHeight;
    int32_t mGridRows;
    int32_t mGridCols;
    int32_t mTileWidth;
    int32_t mTileHeight;
    int32_t mTilesDecoded;
    int32_t mTargetTiles;

// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    struct ImageOutputThread;
    sp<ImageOutputThread> mThread;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    bool mUseMultiThread;

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
    enum OutputThrSignalType {
        NONE,
        EXECUTE,
        EXIT
    };

// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    struct OutputInfo {
        OutputInfo()
            : mRetriesLeft(0),
              mErrorCode(OK),
              mDone(false),
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
              mThrStarted(false),
              mSignalType(NONE) {
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        }
        size_t mRetriesLeft;
        status_t mErrorCode;
        bool mDone;
        bool mThrStarted;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
        OutputThrSignalType mSignalType;
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        Condition mCond;
    };
    Mutexed<OutputInfo> mOutInfo;

    bool outputLoop();
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
};

}  // namespace android

#endif  // FRAME_DECODER_H_

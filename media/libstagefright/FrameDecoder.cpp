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

// QTI_BEGIN: 2022-10-06: Video: Merge "Revert "Dynamic Video Framework Log Enablement"" into t-keystone-qcom-dev
//#define LOG_NDEBUG 0
// QTI_END: 2022-10-06: Video: Merge "Revert "Dynamic Video Framework Log Enablement"" into t-keystone-qcom-dev
#define LOG_TAG "FrameDecoder"
#define ATRACE_TAG  ATRACE_TAG_VIDEO
#include "include/FrameDecoder.h"
#include <android_media_codec.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <gui/Surface.h>
#include <inttypes.h>
// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
#include <media/IMediaCodecList.h>
// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
#include <media/IMediaSource.h>
#include <media/MediaCodecBuffer.h>
#include <media/stagefright/CodecBase.h>
#include <media/stagefright/ColorConverter.h>
#include <media/stagefright/FrameCaptureProcessor.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecConstants.h>
// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
#include <media/stagefright/MediaCodecList.h>
// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/foundation/avc_utils.h>
#include <mediadrm/ICrypto.h>
#include <private/media/VideoFrame.h>
#include <utils/Log.h>
#include <utils/Trace.h>
#include "include/FrameCaptureLayer.h"
#include "include/HevcUtils.h"

#include <C2Buffer.h>
#include <Codec2BufferUtils.h>

namespace android {

static const int64_t kBufferTimeOutUs = 10000LL; // 10 msec
static const int64_t kAsyncBufferTimeOutUs = 2000000LL; // 2000 msec
static const size_t kRetryCount = 100; // must be >0
static const int64_t kDefaultSampleDurationUs = 33333LL; // 33ms
// For codec, 0 is the highest importance; higher the number lesser important.
// To make codec for thumbnail less important, give it a value more than 0.
static const int kThumbnailImportance = 1;

// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
enum HeifMode : uint32_t {
    DISABLE     = 0,
    TILE        = 1,
    ROW         = 2,
};

// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
sp<IMemory> allocVideoFrame(const sp<MetaData>& trackMeta,
        int32_t width, int32_t height, int32_t tileWidth, int32_t tileHeight,
        int32_t dstBpp, uint32_t bitDepth, bool allocRotated, bool metaOnly) {
    int32_t rotationAngle;
    if (!trackMeta->findInt32(kKeyRotation, &rotationAngle)) {
        rotationAngle = 0;  // By default, no rotation
    }
    uint32_t type;
    const void *iccData;
    size_t iccSize;
    if (!trackMeta->findData(kKeyIccProfile, &type, &iccData, &iccSize)){
        iccData = NULL;
        iccSize = 0;
    }

    int32_t sarWidth, sarHeight;
    int32_t displayWidth, displayHeight;
    if (trackMeta->findInt32(kKeySARWidth, &sarWidth)
            && trackMeta->findInt32(kKeySARHeight, &sarHeight)
            && sarHeight != 0) {
        int32_t multVal;
        if (width < 0 || sarWidth < 0 ||
            __builtin_mul_overflow(width, sarWidth, &multVal)) {
            ALOGE("displayWidth overflow %dx%d", width, sarWidth);
            return NULL;
        }
        displayWidth = (width * sarWidth) / sarHeight;
        displayHeight = height;
    } else if (trackMeta->findInt32(kKeyDisplayWidth, &displayWidth)
                && trackMeta->findInt32(kKeyDisplayHeight, &displayHeight)
                && displayWidth > 0 && displayHeight > 0
                && width > 0 && height > 0) {
        ALOGV("found display size %dx%d", displayWidth, displayHeight);
    } else {
        displayWidth = width;
        displayHeight = height;
    }
    int32_t displayLeft = 0;
    int32_t displayTop = 0;
    int32_t displayRight;
    int32_t displayBottom;
    if (trackMeta->findRect(kKeyCropRect, &displayLeft, &displayTop, &displayRight,
                            &displayBottom)) {
        if (displayLeft >= 0 && displayTop >= 0 && displayRight < width && displayBottom < height &&
            displayLeft <= displayRight && displayTop <= displayBottom) {
            displayWidth = displayRight - displayLeft + 1;
            displayHeight = displayBottom - displayTop + 1;
        } else {
            // Crop rectangle is invalid, use the whole frame.
            displayLeft = 0;
            displayTop = 0;
        }
    }

    if (allocRotated) {
        if (rotationAngle == 90 || rotationAngle == 270) {
            // swap width and height for 90 & 270 degrees rotation
            std::swap(width, height);
            std::swap(displayWidth, displayHeight);
            std::swap(tileWidth, tileHeight);
        }
        // Rotation is already applied.
        rotationAngle = 0;
    }

    if (!metaOnly) {
        int32_t multVal;
        if (width < 0 || height < 0 || dstBpp < 0 ||
            __builtin_mul_overflow(dstBpp, width, &multVal) ||
            __builtin_mul_overflow(multVal, height, &multVal)) {
            ALOGE("Frame size overflow %dx%d bpp %d", width, height, dstBpp);
            return NULL;
        }
    }

    VideoFrame frame(width, height, displayWidth, displayHeight, displayLeft, displayTop, tileWidth,
                     tileHeight, rotationAngle, dstBpp, bitDepth, !metaOnly, iccSize);

    size_t size = frame.getFlattenedSize();
    sp<MemoryHeapBase> heap = new MemoryHeapBase(size, 0, "MetadataRetrieverClient");
    if (heap == NULL) {
        ALOGE("failed to create MemoryDealer");
        return NULL;
    }
    sp<IMemory> frameMem = new MemoryBase(heap, 0, size);
    if (frameMem == NULL || frameMem->unsecurePointer() == NULL) {
        ALOGE("not enough memory for VideoFrame size=%zu", size);
        return NULL;
    }
    VideoFrame* frameCopy = static_cast<VideoFrame*>(frameMem->unsecurePointer());
    frameCopy->init(frame, iccData, iccSize);

    return frameMem;
}

sp<IMemory> allocVideoFrame(const sp<MetaData>& trackMeta,
        int32_t width, int32_t height, int32_t tileWidth, int32_t tileHeight,
        int32_t dstBpp, uint8_t bitDepth, bool allocRotated = false) {
    return allocVideoFrame(trackMeta, width, height, tileWidth, tileHeight, dstBpp, bitDepth,
            allocRotated, false /*metaOnly*/);
}

sp<IMemory> allocMetaFrame(const sp<MetaData>& trackMeta,
        int32_t width, int32_t height, int32_t tileWidth, int32_t tileHeight,
        int32_t dstBpp, uint8_t bitDepth) {
    return allocVideoFrame(trackMeta, width, height, tileWidth, tileHeight, dstBpp, bitDepth,
            false /*allocRotated*/, true /*metaOnly*/);
}

bool isAvif(const sp<MetaData> &trackMeta) {
    const char *mime;
    return trackMeta->findCString(kKeyMIMEType, &mime)
        && (!strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AV1)
            || !strcasecmp(mime, MEDIA_MIMETYPE_IMAGE_AVIF));
}

bool findThumbnailInfo(
        const sp<MetaData> &trackMeta, int32_t *width, int32_t *height,
        uint32_t *type = NULL, const void **data = NULL, size_t *size = NULL) {
    uint32_t dummyType;
    const void *dummyData;
    size_t dummySize;
    int codecConfigKey = isAvif(trackMeta) ? kKeyThumbnailAV1C : kKeyThumbnailHVCC;
    return trackMeta->findInt32(kKeyThumbnailWidth, width)
        && trackMeta->findInt32(kKeyThumbnailHeight, height)
        && trackMeta->findData(codecConfigKey,
                type ?: &dummyType, data ?: &dummyData, size ?: &dummySize);
}

bool findGridInfo(const sp<MetaData> &trackMeta,
        int32_t *tileWidth, int32_t *tileHeight, int32_t *gridRows, int32_t *gridCols) {
    return trackMeta->findInt32(kKeyTileWidth, tileWidth) && (*tileWidth > 0)
        && trackMeta->findInt32(kKeyTileHeight, tileHeight) && (*tileHeight > 0)
        && trackMeta->findInt32(kKeyGridRows, gridRows) && (*gridRows > 0)
        && trackMeta->findInt32(kKeyGridCols, gridCols) && (*gridCols > 0);
}

bool getDstColorFormat(
        android_pixel_format_t colorFormat,
        OMX_COLOR_FORMATTYPE *dstFormat,
        ui::PixelFormat *captureFormat,
        int32_t *dstBpp) {
    switch (colorFormat) {
        case HAL_PIXEL_FORMAT_RGB_565:
        {
            *dstFormat = OMX_COLOR_Format16bitRGB565;
            *captureFormat = ui::PixelFormat::RGB_565;
            *dstBpp = 2;
            return true;
        }
        case HAL_PIXEL_FORMAT_RGBA_8888:
        {
            *dstFormat = OMX_COLOR_Format32BitRGBA8888;
            *captureFormat = ui::PixelFormat::RGBA_8888;
            *dstBpp = 4;
            return true;
        }
        case HAL_PIXEL_FORMAT_BGRA_8888:
        {
            *dstFormat = OMX_COLOR_Format32bitBGRA8888;
            *captureFormat = ui::PixelFormat::BGRA_8888;
            *dstBpp = 4;
            return true;
        }
        case HAL_PIXEL_FORMAT_RGBA_1010102:
        {
            *dstFormat = (OMX_COLOR_FORMATTYPE)COLOR_Format32bitABGR2101010;
            *captureFormat = ui::PixelFormat::RGBA_1010102;
            *dstBpp = 4;
            return true;
        }
        default:
        {
            ALOGE("Unsupported color format: %d", colorFormat);
            break;
        }
    }
    return false;
}

// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
bool isFeatureSupported(const char *mimeType, const char *featureName) {
    sp<IMediaCodecList> list = MediaCodecList::getInstance();
    if (list == nullptr) {
        return false;
    }
    size_t index = 0;
    for (;;) {
        ssize_t matchIndex = list->findCodecByType(mimeType, false, index);
        if (matchIndex < 0) {
            break;
        }
        index = matchIndex + 1;
        const sp<MediaCodecInfo> info = list->getCodecInfo(matchIndex);
        CHECK(info != nullptr);
        sp<MediaCodecInfo::Capabilities> capabilities
                = info->getCapabilitiesFor(mimeType);
        if (capabilities == nullptr) {
            continue;
        }
        const sp<AMessage> &details = capabilities->getDetails();
        if (details->contains(featureName)) {
            ALOGV("found %s support %s", info->getCodecName(), featureName);
            return true;
        }
    }
    return false;
}

// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
uint32_t selectHeifMode(uint32_t gridNumInCols, uint32_t tileWidth, uint32_t tileHeight) {
    if (gridNumInCols <= 1 || tileWidth % 512 != 0 || tileHeight % 512 !=0) {
// QTI_BEGIN: 2025-05-22: Video: FrameDecoder: Don't enable ROW mode for single tile image
        return HeifMode::TILE;
    }
// QTI_END: 2025-05-22: Video: FrameDecoder: Don't enable ROW mode for single tile image
// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
    constexpr const char *FEATURE_ROW_BY_ROW = "feature-heic-row-by-row-decode";
    static uint32_t sHeifMode
            = isFeatureSupported(MEDIA_MIMETYPE_VIDEO_HEVC, FEATURE_ROW_BY_ROW)
                ? HeifMode::ROW : HeifMode::TILE;
    return sHeifMode;
}

// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
AsyncCodecHandler::AsyncCodecHandler(const wp<FrameDecoder>& frameDecoder) {
    mFrameDecoder = frameDecoder;
}

void AsyncCodecHandler::onMessageReceived(const sp<AMessage>& msg) {
    switch (msg->what()) {
        case FrameDecoder::kWhatCallbackNotify:
            int32_t callbackId;
            if (!msg->findInt32("callbackID", &callbackId)) {
                ALOGD("kWhatCallbackNotify: callbackID is expected.");
                break;
            }
            switch (callbackId) {
                case MediaCodec::CB_INPUT_AVAILABLE: {
                    int32_t index;
                    if (!msg->findInt32("index", &index)) {
                        ALOGD("CB_INPUT_AVAILABLE: index is expected.");
                        break;
                    }
                    ALOGD("CB_INPUT_AVAILABLE received, index is %d", index);
                    sp<FrameDecoder> frameDecoder = mFrameDecoder.promote();
                    if (frameDecoder != nullptr) {
                        frameDecoder->handleInputBufferAsync(index);
                    }
                    break;
                }
                case MediaCodec::CB_OUTPUT_AVAILABLE: {
                    int32_t index;
                    int64_t timeUs;
                    CHECK(msg->findInt32("index", &index));
                    CHECK(msg->findInt64("timeUs", &timeUs));
                    ALOGV("CB_OUTPUT_AVAILABLE received, index is %d", index);
                    sp<FrameDecoder> frameDecoder = mFrameDecoder.promote();
                    if (frameDecoder != nullptr) {
                        frameDecoder->handleOutputBufferAsync(index, timeUs);
                    }
                    break;
                }
                case MediaCodec::CB_OUTPUT_FORMAT_CHANGED: {
                    ALOGV("CB_OUTPUT_FORMAT_CHANGED received");
                    sp<AMessage> format;
                    if (!msg->findMessage("format", &format) || format == nullptr) {
                        ALOGD("CB_OUTPUT_FORMAT_CHANGED: format is expected.");
                        break;
                    }
                    sp<FrameDecoder> frameDecoder = mFrameDecoder.promote();
                    if (frameDecoder != nullptr) {
                        frameDecoder->handleOutputFormatChangeAsync(format);
                    }
                    break;
                }
                case MediaCodec::CB_ERROR: {
                    status_t err;
                    int32_t actionCode;
                    AString detail;
                    if (!msg->findInt32("err", &err)) {
                        ALOGD("CB_ERROR: err is expected.");
                        break;
                    }
                    if (!msg->findInt32("actionCode", &actionCode)) {
                        ALOGD("CB_ERROR: actionCode is expected.");
                        break;
                    }
                    msg->findString("detail", &detail);
                    ALOGI("Codec reported error(0x%x/%s), actionCode(%d), detail(%s)", err,
                          StrMediaError(err).c_str(), actionCode, detail.c_str());
                    break;
                }
                case MediaCodec::CB_REQUIRED_RESOURCES_CHANGED:
                case MediaCodec::CB_METRICS_FLUSHED:
                {
                    // Nothing to do. Informational. Safe to ignore.
                    break;
                }

                case MediaCodec::CB_LARGE_FRAME_OUTPUT_AVAILABLE:
                // unexpected as we are not using large frames
                case MediaCodec::CB_CRYPTO_ERROR:
                // unexpected as we are not using crypto
                default:
                {
                    ALOGD("kWhatCallbackNotify: callbackID(%d) is unexpected.", callbackId);
                    break;
                }
            }
            break;
        default:
            ALOGD("unexpected message received: %s", msg->debugString().c_str());
            break;
    }
}

void InputBufferIndexQueue::enqueue(int32_t index) {
    std::scoped_lock<std::mutex> lock(mMutex);
    mQueue.push(index);
    mCondition.notify_one();
}

bool InputBufferIndexQueue::dequeue(int32_t* index, int32_t timeOutUs) {
    std::unique_lock<std::mutex> lock(mMutex);
    bool hasAvailableIndex = mCondition.wait_for(lock, std::chrono::microseconds(timeOutUs),
                                                 [this] { return !mQueue.empty(); });
    if (hasAvailableIndex) {
        *index = mQueue.front();
        mQueue.pop();
        return true;
    } else {
        return false;
    }
}

//static
sp<IMemory> FrameDecoder::getMetadataOnly(
        const sp<MetaData> &trackMeta, int colorFormat, bool preferHw, bool thumbnail,
        uint32_t bitDepth) {
    OMX_COLOR_FORMATTYPE dstFormat;
    ui::PixelFormat captureFormat;
    int32_t dstBpp;
    if (!getDstColorFormat((android_pixel_format_t)colorFormat,
            &dstFormat, &captureFormat, &dstBpp)) {
        return NULL;
    }

    int32_t width, height, tileWidth = 0, tileHeight = 0;
    if (thumbnail) {
        if (!findThumbnailInfo(trackMeta, &width, &height)) {
            return NULL;
        }
    } else {
        CHECK(trackMeta->findInt32(kKeyWidth, &width));
        CHECK(trackMeta->findInt32(kKeyHeight, &height));

// QTI_BEGIN: 2025-07-30: Video: FrameDecoder: avoid overhead of repeated findGridInfo calls
        int32_t gridRows = 0, gridCols = 0;
// QTI_END: 2025-07-30: Video: FrameDecoder: avoid overhead of repeated findGridInfo calls
        if (!findGridInfo(trackMeta, &tileWidth, &tileHeight, &gridRows, &gridCols)) {
            tileWidth = tileHeight = 0;
        }

        if (preferHw && (selectHeifMode(gridCols, tileWidth, tileHeight) == HeifMode::ROW) &&
                !isAvif(trackMeta)) {
// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
            // In HEIF row-by-row mode, the basic output is a row.
            // All tiles on a row are stitched into one output, so the display
            // info notified to Skia needs to be updated to the row size.
            tileWidth *= gridCols;
            gridCols = 1;
        }
// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
    }

    sp<IMemory> metaMem =
            allocMetaFrame(trackMeta, width, height, tileWidth, tileHeight, dstBpp, bitDepth);
    if (metaMem == nullptr) {
        return NULL;
    }

    // try to fill sequence meta's duration based on average frame rate,
    // default to 33ms if frame rate is unavailable.
    int32_t frameRate;
    VideoFrame* meta = static_cast<VideoFrame*>(metaMem->unsecurePointer());
    if (trackMeta->findInt32(kKeyFrameRate, &frameRate) && frameRate > 0) {
        meta->mDurationUs = 1000000LL / frameRate;
    } else {
        meta->mDurationUs = kDefaultSampleDurationUs;
    }
    return metaMem;
}

FrameDecoder::FrameDecoder(
        const AString &componentName,
        const sp<MetaData> &trackMeta,
        const sp<IMediaSource> &source)
    : mIDRSent(false),
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
      mHaveMoreInputs(true),
      mFirstSample(true),
      mSource(source),
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
      mSourceStopped(false),
      mComponentName(componentName),
      mTrackMeta(trackMeta),
      mDstFormat(OMX_COLOR_Format16bitRGB565),
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
      mDstBpp(2) {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2020-08-20: Video: stagefright: FrameDecoder: set heif decoder hint
    ALOGD("FrameDecoder created");
// QTI_END: 2020-08-20: Video: stagefright: FrameDecoder: set heif decoder hint
}

FrameDecoder::~FrameDecoder() {
    if (mHandler != NULL) {
        mAsyncLooper->stop();
        mAsyncLooper->unregisterHandler(mHandler->id());
    }
    if (mDecoder != NULL) {
        mDecoder->release();
        if (!mSourceStopped) {
            mSource->stop();
        }
    }
// QTI_BEGIN: 2020-08-20: Video: stagefright: FrameDecoder: set heif decoder hint
    ALOGD("FrameDecoder destroyed");
// QTI_END: 2020-08-20: Video: stagefright: FrameDecoder: set heif decoder hint
}

bool isHDR(const sp<AMessage> &format) {
    uint32_t standard, transfer;
    if (!format->findInt32("color-standard", (int32_t*)&standard)) {
        standard = 0;
    }
    if (!format->findInt32("color-transfer", (int32_t*)&transfer)) {
        transfer = 0;
    }
    return standard == ColorUtils::kColorStandardBT2020 &&
            (transfer == ColorUtils::kColorTransferST2084 ||
            transfer == ColorUtils::kColorTransferHLG);
}

status_t FrameDecoder::init(
        int64_t frameTimeUs, int option, int colorFormat) {
    if (!getDstColorFormat((android_pixel_format_t)colorFormat,
            &mDstFormat, &mCaptureFormat, &mDstBpp)) {
        return ERROR_UNSUPPORTED;
    }

    sp<AMessage> videoFormat = onGetFormatAndSeekOptions(
            frameTimeUs, option, &mReadOptions, &mSurface);
    if (videoFormat == NULL) {
        ALOGE("video format or seek mode not supported");
        return ERROR_UNSUPPORTED;
    }

    status_t err;
    sp<ALooper> looper = new ALooper;
    looper->start();
    sp<MediaCodec> decoder = MediaCodec::CreateByComponentName(
            looper, mComponentName, &err);
    if (decoder.get() == NULL || err != OK) {
        ALOGW("Failed to instantiate decoder [%s]", mComponentName.c_str());
        return (decoder.get() == NULL) ? NO_MEMORY : err;
    }

    if (mUseBlockModel) {
        mAsyncLooper = new ALooper;
        mAsyncLooper->start();
        mHandler = new AsyncCodecHandler(wp<FrameDecoder>(this));
        mAsyncLooper->registerHandler(mHandler);
        sp<AMessage> callbackMsg = new AMessage(kWhatCallbackNotify, mHandler);
        decoder->setCallback(callbackMsg);
    }

    err = decoder->configure(
            videoFormat, mSurface, NULL /* crypto */,
            mUseBlockModel ? MediaCodec::CONFIGURE_FLAG_USE_BLOCK_MODEL : 0 /* flags */);
    if (err != OK) {
        ALOGW("configure returned error %d (%s)", err, asString(err));
        decoder->release();
        return err;
    }

    err = decoder->start();
    if (err != OK) {
        ALOGW("start returned error %d (%s)", err, asString(err));
        decoder->release();
        return err;
    }

    err = mSource->start();
    if (err != OK) {
        ALOGW("source failed to start: %d (%s)", err, asString(err));
        decoder->release();
        return err;
    }
    mDecoder = decoder;

    return OK;
}

sp<IMemory> FrameDecoder::extractFrame(FrameRect *rect) {
    ScopedTrace trace(ATRACE_TAG, "FrameDecoder::ExtractFrame");
    status_t err = onExtractRect(rect);
    if (err != OK) {
        ALOGE("onExtractRect error %d", err);
        return NULL;
    }

    if (!mUseBlockModel) {
        err = extractInternal();
    } else {
        err = extractInternalUsingBlockModel();
    }
    if (err != OK) {
        ALOGE("extractInternal error %d", err);
        return NULL;
    }

    return mFrameMemory;
}

status_t FrameDecoder::extractInternal() {
    status_t err = OK;
    bool done = false;
    size_t retriesLeft = kRetryCount;
    if (!mDecoder) {
        ALOGE("decoder is not initialized");
        return NO_INIT;
    }

    do {
        size_t index;
        int64_t ptsUs = 0LL;
        uint32_t flags = 0;

        // Queue as many inputs as we possibly can, then block on dequeuing
        // outputs. After getting each output, come back and queue the inputs
        // again to keep the decoder busy.
        while (mHaveMoreInputs) {
            err = mDecoder->dequeueInputBuffer(&index, 0);
            if (err != OK) {
                ALOGV("Timed out waiting for input");
                if (retriesLeft) {
                    err = OK;
                }
                break;
            }
            sp<MediaCodecBuffer> codecBuffer;
            err = mDecoder->getInputBuffer(index, &codecBuffer);
            if (err != OK) {
                ALOGE("failed to get input buffer %zu", index);
                break;
            }

            MediaBufferBase *mediaBuffer = NULL;

            err = mSource->read(&mediaBuffer, &mReadOptions);
            mReadOptions.clearSeekTo();
            if (err != OK) {
                mHaveMoreInputs = false;
                if (!mFirstSample && err == ERROR_END_OF_STREAM) {
                    (void)mDecoder->queueInputBuffer(
                            index, 0, 0, 0, MediaCodec::BUFFER_FLAG_EOS);
                    err = OK;
// QTI_BEGIN: 2018-05-10: Video: libstagefright: fix to retrieve thumbnail for HEIF content
                    flags |= MediaCodec::BUFFER_FLAG_EOS;
// QTI_END: 2018-05-10: Video: libstagefright: fix to retrieve thumbnail for HEIF content
                    mHaveMoreInputs = true;
                } else {
                    ALOGW("Input Error: err=%d", err);
                }
                break;
            }

            if (mediaBuffer->range_length() > codecBuffer->capacity()) {
                ALOGE("buffer size (%zu) too large for codec input size (%zu)",
                        mediaBuffer->range_length(), codecBuffer->capacity());
                mHaveMoreInputs = false;
                err = BAD_VALUE;
            } else {
                codecBuffer->setRange(0, mediaBuffer->range_length());

                CHECK(mediaBuffer->meta_data().findInt64(kKeyTime, &ptsUs));
                memcpy(codecBuffer->data(),
                        (const uint8_t*)mediaBuffer->data() + mediaBuffer->range_offset(),
                        mediaBuffer->range_length());

                onInputReceived(codecBuffer->data(), codecBuffer->size(), mediaBuffer->meta_data(),
                                mFirstSample, &flags);
                mFirstSample = false;
            }

            mediaBuffer->release();

            if (mHaveMoreInputs) {
                ALOGV("QueueInput: size=%zu ts=%" PRId64 " us flags=%x",
                        codecBuffer->size(), ptsUs, flags);

                err = mDecoder->queueInputBuffer(
                        index,
                        codecBuffer->offset(),
                        codecBuffer->size(),
                        ptsUs,
                        flags);

                if (flags & MediaCodec::BUFFER_FLAG_EOS) {
                    mHaveMoreInputs = false;
                }
            }
        }

        while (err == OK) {
            size_t offset, size;
            // wait for a decoded buffer
            err = mDecoder->dequeueOutputBuffer(
                    &index,
                    &offset,
                    &size,
                    &ptsUs,
                    &flags,
                    kBufferTimeOutUs);

            if (err == INFO_FORMAT_CHANGED) {
                ALOGV("Received format change");
                err = mDecoder->getOutputFormat(&mOutputFormat);
            } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
                ALOGV("Output buffers changed");
                err = OK;
            } else {
                if (err == -EAGAIN /* INFO_TRY_AGAIN_LATER */ && --retriesLeft > 0) {
                    ALOGV("Timed-out waiting for output.. retries left = %zu", retriesLeft);
                    err = OK;
                } else if (err == OK) {
                    // If we're seeking with CLOSEST option and obtained a valid targetTimeUs
                    // from the extractor, decode to the specified frame. Otherwise we're done.
                    ALOGV("Received an output buffer, timeUs=%lld", (long long)ptsUs);
                    sp<MediaCodecBuffer> videoFrameBuffer;
                    err = mDecoder->getOutputBuffer(index, &videoFrameBuffer);
                    if (err != OK) {
                        ALOGE("failed to get output buffer %zu", index);
                        break;
                    }
                    uint8_t* frameData = videoFrameBuffer->data();
                    sp<ABuffer> imageData;
                    videoFrameBuffer->meta()->findBuffer("image-data", &imageData);
                    if (mSurface != nullptr) {
// QTI_BEGIN: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
                        if (!shouldDropOutput(ptsUs)) {
                            mDecoder->renderOutputBufferAndRelease(index);
                        } else {
                            mDecoder->releaseOutputBuffer(index);
                        }
// QTI_END: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
                        err = onOutputReceived(frameData, imageData, mOutputFormat, ptsUs, &done);
                    } else {
                        err = onOutputReceived(frameData, imageData, mOutputFormat, ptsUs, &done);
                        mDecoder->releaseOutputBuffer(index);
                    }
                } else {
                    ALOGW("Received error %d (%s) instead of output", err, asString(err));
                    done = true;
                }
                break;
            }
        }
    } while (err == OK && !done);

    if (err != OK) {
        ALOGE("failed to get video frame (err %d)", err);
    }

    return err;
}

status_t FrameDecoder::extractInternalUsingBlockModel() {
    status_t err = OK;
    MediaBufferBase* mediaBuffer = NULL;
    int64_t ptsUs = 0LL;
    uint32_t flags = 0;
    int32_t index;
    mHandleOutputBufferAsyncDone = false;

    err = mSource->read(&mediaBuffer, &mReadOptions);
    mReadOptions.clearSeekTo();
    if (err != OK) {
        ALOGW("Input Error: err=%d", err);
        if (mediaBuffer) {
            mediaBuffer->release();
        }
        return err;
    }

    size_t inputSize = mediaBuffer->range_length();
    std::shared_ptr<C2LinearBlock> block =
            MediaCodec::FetchLinearBlock(inputSize, {std::string{mComponentName.c_str()}});
    C2WriteView view{block->map().get()};
    if (view.error() != C2_OK) {
        ALOGE("Fatal error: failed to allocate and map a block");
        mediaBuffer->release();
        return NO_MEMORY;
    }
    if (inputSize > view.capacity()) {
        ALOGE("Fatal error: allocated block is too small "
              "(input size %zu; block cap %u)",
              inputSize, view.capacity());
        mediaBuffer->release();
        return BAD_VALUE;
    }
    CHECK(mediaBuffer->meta_data().findInt64(kKeyTime, &ptsUs));
    memcpy(view.base(), (const uint8_t*)mediaBuffer->data() + mediaBuffer->range_offset(),
           inputSize);
    std::shared_ptr<C2Buffer> c2Buffer =
            C2Buffer::CreateLinearBuffer(block->share(0, inputSize, C2Fence{}));
    onInputReceived(view.base(), inputSize, mediaBuffer->meta_data(), true /* firstSample */,
                    &flags);
    flags |= MediaCodec::BUFFER_FLAG_EOS;
    mediaBuffer->release();

    std::vector<AccessUnitInfo> infoVec;
    infoVec.emplace_back(flags, inputSize, ptsUs);
    sp<BufferInfosWrapper> infos = new BufferInfosWrapper{std::move(infoVec)};

    if (!mInputBufferIndexQueue.dequeue(&index, kAsyncBufferTimeOutUs)) {
        ALOGE("No available input buffer index for async mode.");
        return TIMED_OUT;
    }

    AString errorDetailMsg;
    ALOGD("QueueLinearBlock: index=%d size=%zu ts=%" PRId64 " us flags=%x",
            index, inputSize, ptsUs,flags);
    err = mDecoder->queueBuffer(index, c2Buffer, infos, nullptr, &errorDetailMsg);
    if (err != OK) {
        ALOGE("failed to queueBuffer (err %d): %s", err, errorDetailMsg.c_str());
        return err;
    }

    // wait for handleOutputBufferAsync() to finish
    std::unique_lock _lk(mMutex);
    if (!mOutputFramePending.wait_for(_lk, std::chrono::microseconds(kAsyncBufferTimeOutUs),
                                 [this] { return mHandleOutputBufferAsyncDone; })) {
        ALOGE("%s timed out waiting for handleOutputBufferAsync() to complete.", __func__);
        mSource->stop();
        mSourceStopped = true;
    }
    return mHandleOutputBufferAsyncDone ? OK : TIMED_OUT;
}

//////////////////////////////////////////////////////////////////////

VideoFrameDecoder::VideoFrameDecoder(
        const AString &componentName,
        const sp<MetaData> &trackMeta,
        const sp<IMediaSource> &source)
    : FrameDecoder(componentName, trackMeta, source),
      mFrame(NULL),
      mIsAvc(false),
      mIsHevc(false),
      mSeekMode(MediaSource::ReadOptions::SEEK_PREVIOUS_SYNC),
      mTargetTimeUs(-1LL),
      mDefaultSampleDurationUs(0) {
}

status_t FrameDecoder::handleOutputFormatChangeAsync(sp<AMessage> format) {
    // Here format is MediaCodec's internal copy of output format.
    // Make a copy since the client might modify it.
    mOutputFormat = format->dup();
    ALOGD("receive output format in async mode: %s", mOutputFormat->debugString().c_str());
    return OK;
}

status_t FrameDecoder::handleInputBufferAsync(int32_t index) {
    mInputBufferIndexQueue.enqueue(index);
    return OK;
}

status_t FrameDecoder::handleOutputBufferAsync(int32_t index, int64_t timeUs) {
    if (mHandleOutputBufferAsyncDone) {
        // we have already processed an output buffer, skip others
        return OK;
    }

    status_t err = OK;
    sp<MediaCodecBuffer> videoFrameBuffer;
    err = mDecoder->getOutputBuffer(index, &videoFrameBuffer);
    if (err != OK || videoFrameBuffer == nullptr) {
        ALOGE("failed to get output buffer %d", index);
        return err;
    }

    bool onOutputReceivedDone = false;
    if (mSurface != nullptr) {
        mDecoder->renderOutputBufferAndRelease(index);
        // frameData and imgObj will be fetched by captureSurface() inside onOutputReceived()
        // explicitly pass null here
        err = onOutputReceived(nullptr, nullptr, mOutputFormat, timeUs, &onOutputReceivedDone);
    } else {
        // get stride and frame data for block model buffer
        std::shared_ptr<C2Buffer> c2buffer = videoFrameBuffer->asC2Buffer();
        if (!c2buffer
                || c2buffer->data().type() != C2BufferData::GRAPHIC
                || c2buffer->data().graphicBlocks().size() == 0u) {
            ALOGE("C2Buffer precond fail");
            return ERROR_MALFORMED;
        }

        std::unique_ptr<const C2GraphicView> view(std::make_unique<const C2GraphicView>(
            c2buffer->data().graphicBlocks()[0].map().get()));
        GraphicView2MediaImageConverter converter(*view, mOutputFormat, false /* copy */);
        if (converter.initCheck() != OK) {
            ALOGE("Converter init failed: %d", converter.initCheck());
            return NO_INIT;
        }

        uint8_t* frameData = converter.wrap()->data();
        sp<ABuffer> imageData = converter.imageData();
        if (imageData != nullptr) {
            mOutputFormat->setBuffer("image-data", imageData);
            MediaImage2 *img = (MediaImage2*) imageData->data();
            if (img->mNumPlanes > 0 && img->mType != img->MEDIA_IMAGE_TYPE_UNKNOWN) {
                int32_t stride = img->mPlane[0].mRowInc;
                mOutputFormat->setInt32(KEY_STRIDE, stride);
                ALOGD("updating stride = %d", stride);
            }
        }

        err = onOutputReceived(frameData, imageData, mOutputFormat, timeUs, &onOutputReceivedDone);
        mDecoder->releaseOutputBuffer(index);
    }

    if (err == OK && onOutputReceivedDone) {
        std::lock_guard _lm(mMutex);
        mHandleOutputBufferAsyncDone = true;
        mOutputFramePending.notify_one();
    }
    return err;
}

sp<AMessage> VideoFrameDecoder::onGetFormatAndSeekOptions(
        int64_t frameTimeUs, int seekMode,
        MediaSource::ReadOptions *options,
        sp<Surface> *window) {
    mSeekMode = static_cast<MediaSource::ReadOptions::SeekMode>(seekMode);
    if (mSeekMode < MediaSource::ReadOptions::SEEK_PREVIOUS_SYNC ||
            mSeekMode > MediaSource::ReadOptions::SEEK_FRAME_INDEX) {
        ALOGE("Unknown seek mode: %d", mSeekMode);
        return NULL;
    }

    const char *mime;
    if (!trackMeta()->findCString(kKeyMIMEType, &mime)) {
        ALOGE("Could not find mime type");
        return NULL;
    }

    mIsAvc = !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC);
    mIsHevc = !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_HEVC);

    if (frameTimeUs < 0) {
        int64_t thumbNailTime = -1ll;
        if (!trackMeta()->findInt64(kKeyThumbnailTime, &thumbNailTime)
                || thumbNailTime < 0) {
            thumbNailTime = 0;
        }
        options->setSeekTo(thumbNailTime, mSeekMode);
    } else {
        options->setSeekTo(frameTimeUs, mSeekMode);
    }

    sp<AMessage> videoFormat;
    if (convertMetaDataToMessage(trackMeta(), &videoFormat) != OK) {
        ALOGE("b/23680780");
        ALOGW("Failed to convert meta data to message");
        return NULL;
    }

    if (dstFormat() == COLOR_Format32bitABGR2101010) {
        videoFormat->setInt32("color-format", COLOR_FormatYUVP010);
    } else {
        videoFormat->setInt32("color-format", COLOR_FormatYUV420Flexible);
    }

    // For the thumbnail extraction case, try to allocate single buffer in both
    // input and output ports, if seeking to a sync frame. NOTE: This request may
    // fail if component requires more than that for decoding.
    bool isSeekingClosest = (mSeekMode == MediaSource::ReadOptions::SEEK_CLOSEST)
            || (mSeekMode == MediaSource::ReadOptions::SEEK_FRAME_INDEX);
    if (!isSeekingClosest) {
        if (mComponentName.startsWithIgnoreCase("c2.")) {
            mUseBlockModel = android::media::codec::provider_->thumbnail_block_model();
        } else {
            // OMX Codec
            videoFormat->setInt32("android._num-input-buffers", 1);
            videoFormat->setInt32("android._num-output-buffers", 1);
        }
// QTI_BEGIN: 2018-04-26: Video: libstagefright: Enable optimizations for thumbnail session
        videoFormat->setInt32("thumbnail-mode", 1);
// QTI_END: 2018-04-26: Video: libstagefright: Enable optimizations for thumbnail session
// QTI_BEGIN: 2020-04-16: Video: libstagefright: Correct the thumbnail extn key
        videoFormat->setInt32("vendor.qti-ext-dec-thumbnail-mode.value", 1);
// QTI_END: 2020-04-16: Video: libstagefright: Correct the thumbnail extn key
    }

// QTI_BEGIN: 2020-07-15: Video: Revert "Revert "stagefright: route all video thumbnails to surface""
    // force surface-mode for all thumbnails
    if (true /*isHDR(videoFormat)*/) {
// QTI_END: 2020-07-15: Video: Revert "Revert "stagefright: route all video thumbnails to surface""
        *window = initSurface();
        if (*window == NULL) {
            ALOGE("Failed to init surface control for HDR, fallback to non-hdr");
        } else {
// QTI_BEGIN: 2020-07-15: Video: Revert "Revert "stagefright: route all video thumbnails to surface""
            ALOGI("using surface mode");
// QTI_END: 2020-07-15: Video: Revert "Revert "stagefright: route all video thumbnails to surface""
            videoFormat->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
        }
    }

    // Set the importance for thumbnail.
    videoFormat->setInt32(KEY_IMPORTANCE, kThumbnailImportance);

    int32_t frameRate;
    if (trackMeta()->findInt32(kKeyFrameRate, &frameRate) && frameRate > 0) {
        mDefaultSampleDurationUs = 1000000LL / frameRate;
    } else {
        mDefaultSampleDurationUs = kDefaultSampleDurationUs;
    }

    return videoFormat;
}

status_t VideoFrameDecoder::onInputReceived(uint8_t* data, size_t size, MetaDataBase& sampleMeta,
                                            bool firstSample, uint32_t* flags) {
    bool isSeekingClosest = (mSeekMode == MediaSource::ReadOptions::SEEK_CLOSEST)
            || (mSeekMode == MediaSource::ReadOptions::SEEK_FRAME_INDEX);

    if (firstSample && isSeekingClosest) {
        sampleMeta.findInt64(kKeyTargetTime, &mTargetTimeUs);
        ALOGV("Seeking closest: targetTimeUs=%lld", (long long)mTargetTimeUs);
    }

    if ((!isSeekingClosest && ((mIsAvc && IsIDR(data, size)) || (mIsHevc && IsIDR(data, size))))
        || (mIDRSent == true)) {
        // Only need to decode one IDR frame, unless we're seeking with CLOSEST
        // option, in which case we need to actually decode to targetTimeUs.
// QTI_BEGIN: 2018-04-26: Video: libstagefright: Enable optimizations for thumbnail session
        mIDRSent == false ? mIDRSent = true : *flags |= MediaCodec::BUFFER_FLAG_EOS;
// QTI_END: 2018-04-26: Video: libstagefright: Enable optimizations for thumbnail session
    }
    int64_t durationUs;
    if (sampleMeta.findInt64(kKeyDuration, &durationUs)) {
        mSampleDurations.push_back(durationUs);
    } else {
        mSampleDurations.push_back(mDefaultSampleDurationUs);
    }
    return OK;
}

status_t VideoFrameDecoder::onOutputReceived(
        uint8_t* frameData,
        sp<ABuffer> imgObj,
        const sp<AMessage> &outputFormat,
        int64_t timeUs, bool *done) {
    int64_t durationUs = mDefaultSampleDurationUs;
    if (!mSampleDurations.empty()) {
        durationUs = *mSampleDurations.begin();
        mSampleDurations.erase(mSampleDurations.begin());
    }

    // If this is not the target frame, skip color convert.
// QTI_BEGIN: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
    if (shouldDropOutput(timeUs)) {
// QTI_END: 2020-07-15: Video: stagefright: FrameDecoder: fix stall during thumbnail decoding
        *done = false;
        return OK;
    }

    *done = true;

    if (outputFormat == NULL) {
        return ERROR_MALFORMED;
    }

    int32_t width, height, stride, srcFormat, slice_height;
    if (!outputFormat->findInt32("width", &width) ||
            !outputFormat->findInt32("height", &height) ||
// QTI_BEGIN: 2020-07-16: Video: stagefright: remove slice-height as mandatory parameter
            !outputFormat->findInt32("color-format", &srcFormat)) {
// QTI_END: 2020-07-16: Video: stagefright: remove slice-height as mandatory parameter
        ALOGE("format missing dimension or color: %s",
                outputFormat->debugString().c_str());
        return ERROR_MALFORMED;
    }

// QTI_BEGIN: 2020-07-16: Video: stagefright: remove slice-height as mandatory parameter
    if (!outputFormat->findInt32("slice-height", &slice_height)) {
        slice_height = height;
    }

// QTI_END: 2020-07-16: Video: stagefright: remove slice-height as mandatory parameter
    if (!outputFormat->findInt32("stride", &stride)) {
        if (mCaptureLayer == NULL) {
            ALOGE("format must have stride for byte buffer mode: %s",
                    outputFormat->debugString().c_str());
            return ERROR_MALFORMED;
        }
        // for surface output, set stride to width, we don't actually need it.
        stride = width;
    }

    int32_t crop_left, crop_top, crop_right, crop_bottom;
    if (!outputFormat->findRect("crop", &crop_left, &crop_top, &crop_right, &crop_bottom)) {
        crop_left = crop_top = 0;
        crop_right = width - 1;
        crop_bottom = height - 1;
    }

    if (outputFormat->findInt32("slice-height", &slice_height) && slice_height > 0) {
        height = slice_height;
    }

    uint32_t bitDepth = 8;
    if (COLOR_FormatYUVP010 == srcFormat) {
        bitDepth = 10;
    }

    if (mFrame == NULL) {
        sp<IMemory> frameMem = allocVideoFrame(
                trackMeta(),
                (crop_right - crop_left + 1),
                (crop_bottom - crop_top + 1),
                0,
                0,
                dstBpp(),
                bitDepth,
                mCaptureLayer != nullptr /*allocRotated*/);
        if (frameMem == nullptr) {
            return NO_MEMORY;
        }

        mFrame = static_cast<VideoFrame*>(frameMem->unsecurePointer());
        setFrame(frameMem);
    }

    mFrame->mDurationUs = durationUs;

    if (mCaptureLayer != nullptr) {
        return captureSurface();
    }
    ColorConverter colorConverter((OMX_COLOR_FORMATTYPE)srcFormat, dstFormat());

    uint32_t standard, range, transfer;
    if (!outputFormat->findInt32("color-standard", (int32_t*)&standard)) {
        standard = 0;
    }
    if (!outputFormat->findInt32("color-range", (int32_t*)&range)) {
        range = 0;
    }
    if (!outputFormat->findInt32("color-transfer", (int32_t*)&transfer)) {
        transfer = 0;
    }

    if (imgObj != nullptr) {
        MediaImage2 *imageData = nullptr;
        imageData = (MediaImage2 *)(imgObj.get()->data());
        if (imageData != nullptr) {
            colorConverter.setSrcMediaImage2(*imageData);
        }
    }
    if (srcFormat == COLOR_FormatYUV420Flexible && imgObj.get() == nullptr) {
        return ERROR_UNSUPPORTED;
    }
    colorConverter.setSrcColorSpace(standard, range, transfer);
    if (colorConverter.isValid()) {
        ScopedTrace trace(ATRACE_TAG, "FrameDecoder::ColorConverter");
        if (frameData == nullptr) {
            ALOGD("frameData is null for ColorConverter");
        }
        colorConverter.convert(
                (const uint8_t *)frameData,
// QTI_BEGIN: 2019-04-17: Video: libstagefright: Pass aligned height to color converter
                width, slice_height, stride,
// QTI_END: 2019-04-17: Video: libstagefright: Pass aligned height to color converter
                crop_left, crop_top, crop_right, crop_bottom,
                mFrame->getFlattenedData(),
                mFrame->mWidth, mFrame->mHeight, mFrame->mRowBytes,
                // since the frame is allocated with top-left adjusted,
                // the dst rect should start at {0,0} as well.
                0, 0, mFrame->mWidth - 1, mFrame->mHeight - 1);
        return OK;
    }

    ALOGE("Unable to convert from format 0x%08x to 0x%08x",
                srcFormat, dstFormat());
    return ERROR_UNSUPPORTED;
}

sp<Surface> VideoFrameDecoder::initSurface() {
    // create the consumer listener interface, and hold sp so that this
    // interface lives as long as the GraphicBufferSource.
    sp<FrameCaptureLayer> captureLayer = new FrameCaptureLayer();
    if (captureLayer->init() != OK) {
        ALOGE("failed to init capture layer");
        return nullptr;
    }
    mCaptureLayer = captureLayer;

    return captureLayer->getSurface();
}

status_t VideoFrameDecoder::captureSurface() {
    sp<GraphicBuffer> outBuffer;
    status_t err = mCaptureLayer->capture(
            captureFormat(), Rect(0, 0, mFrame->mWidth, mFrame->mHeight), &outBuffer);

    if (err != OK) {
        ALOGE("failed to capture layer (err %d)", err);
        return err;
    }

    ALOGV("capture: %dx%d, format %d, stride %d",
            outBuffer->getWidth(),
            outBuffer->getHeight(),
            outBuffer->getPixelFormat(),
            outBuffer->getStride());

    uint8_t *base;
    int32_t outBytesPerPixel, outBytesPerStride;
    err = outBuffer->lock(
            GraphicBuffer::USAGE_SW_READ_OFTEN,
            reinterpret_cast<void**>(&base),
            &outBytesPerPixel,
            &outBytesPerStride);
    if (err != OK) {
        ALOGE("failed to lock graphic buffer: err %d", err);
        return err;
    }

    uint8_t *dst = mFrame->getFlattenedData();
    for (size_t y = 0 ; y < fmin(mFrame->mHeight, outBuffer->getHeight()) ; y++) {
        memcpy(dst, base, fmin(mFrame->mWidth, outBuffer->getWidth()) * mFrame->mBytesPerPixel);
        dst += mFrame->mRowBytes;
        base += outBuffer->getStride() * mFrame->mBytesPerPixel;
    }
    outBuffer->unlock();
    return OK;
}

////////////////////////////////////////////////////////////////////////

// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
struct MediaImageDecoder::ImageOutputThread : public Thread {
    ImageOutputThread(MediaImageDecoder *mediaImageDecoder)
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
        : Thread(false /*canCallJava*/),
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
          mImageDecoder(mediaImageDecoder) {
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        ALOGD("ImageOutputThread created");
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    }

    virtual bool threadLoop() {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        return mImageDecoder->outputLoop();
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    }

protected:
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    virtual ~ImageOutputThread() {
        ALOGD("ImageOutputThread destroyed");
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    }

private:
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
    MediaImageDecoder *mImageDecoder;
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class

// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    DISALLOW_EVIL_CONSTRUCTORS(ImageOutputThread);
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
};

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
MediaImageDecoder::MediaImageDecoder(
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
        const AString &componentName,
        const sp<MetaData> &trackMeta,
        const sp<IMediaSource> &source)
    : FrameDecoder(componentName, trackMeta, source),
      mFrame(NULL),
      mWidth(0),
      mHeight(0),
      mGridRows(1),
      mGridCols(1),
      mTileWidth(0),
      mTileHeight(0),
      mTilesDecoded(0),
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
      mTargetTiles(0),
      mThread(NULL),
      mUseMultiThread(false) {
}

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
MediaImageDecoder::~MediaImageDecoder() {
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    if (mThread != NULL) {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
        {
            ALOGI("Signalling ImageOutputThread to exit");
            Mutexed<OutputInfo>::Locked outInfo(mOutInfo);
            outInfo->mSignalType = EXIT;
            outInfo->mCond.signal();
        }
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
        mThread->requestExitAndWait();
        mThread.clear();
    }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
}

// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
sp<AMessage> MediaImageDecoder::onGetFormatAndSeekOptions(
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
        int64_t frameTimeUs, int /*seekMode*/,
        MediaSource::ReadOptions *options, sp<Surface> * /*window*/) {
    sp<MetaData> overrideMeta;
    if (frameTimeUs < 0) {
        uint32_t type;
        const void *data;
        size_t size;

        // if we have a stand-alone thumbnail, set up the override meta,
        // and set seekTo time to -1.
        if (!findThumbnailInfo(trackMeta(), &mWidth, &mHeight, &type, &data, &size)) {
            ALOGE("Thumbnail not available");
            return NULL;
        }
        overrideMeta = new MetaData(*(trackMeta()));
        overrideMeta->remove(kKeyDisplayWidth);
        overrideMeta->remove(kKeyDisplayHeight);
        overrideMeta->setInt32(kKeyWidth, mWidth);
        overrideMeta->setInt32(kKeyHeight, mHeight);
        // The AV1 codec configuration data is passed via CSD0 to the AV1
        // decoder.
        const int codecConfigKey = isAvif(trackMeta()) ? kKeyOpaqueCSD0 : kKeyHVCC;
        overrideMeta->setData(codecConfigKey, type, data, size);
        options->setSeekTo(-1);
    } else {
        CHECK(trackMeta()->findInt32(kKeyWidth, &mWidth));
        CHECK(trackMeta()->findInt32(kKeyHeight, &mHeight));

        options->setSeekTo(frameTimeUs);
    }

    mGridRows = mGridCols = 1;
    if (overrideMeta == NULL) {
        // check if we're dealing with a tiled heif
        int32_t tileWidth, tileHeight, gridRows, gridCols;
        int32_t widthColsProduct = 0;
        int32_t heightRowsProduct = 0;
        if (findGridInfo(trackMeta(), &tileWidth, &tileHeight, &gridRows, &gridCols)) {
            if (__builtin_mul_overflow(tileWidth, gridCols, &widthColsProduct) ||
                    __builtin_mul_overflow(tileHeight, gridRows, &heightRowsProduct)) {
                ALOGE("Multiplication overflowed Grid size: %dx%d, Picture size: %dx%d",
                        gridCols, gridRows, tileWidth, tileHeight);
                return nullptr;
            }
            if (mWidth <= widthColsProduct && mHeight <= heightRowsProduct) {
                ALOGV("grid: %dx%d, tile size: %dx%d, picture size: %dx%d",
                        gridCols, gridRows, tileWidth, tileHeight, mWidth, mHeight);

                overrideMeta = new MetaData(*(trackMeta()));
                overrideMeta->setInt32(kKeyWidth, tileWidth);
                overrideMeta->setInt32(kKeyHeight, tileHeight);
                mTileWidth = tileWidth;
                mTileHeight = tileHeight;
                mGridCols = gridCols;
                mGridRows = gridRows;
            } else {
                ALOGW("ignore bad grid: %dx%d, tile size: %dx%d, picture size: %dx%d",
                        gridCols, gridRows, tileWidth, tileHeight, mWidth, mHeight);
            }
        }
        if (overrideMeta == NULL) {
            overrideMeta = trackMeta();
        }
    }
    mTargetTiles = mGridCols * mGridRows;

    sp<AMessage> videoFormat;
    if (convertMetaDataToMessage(overrideMeta, &videoFormat) != OK) {
        ALOGE("b/23680780");
        ALOGW("Failed to convert meta data to message");
        return NULL;
    }

    if (dstFormat() == COLOR_Format32bitABGR2101010) {
        videoFormat->setInt32("color-format", COLOR_FormatYUVP010);
    } else {
        videoFormat->setInt32("color-format", COLOR_FormatYUV420Flexible);
    }

    if ((mGridRows == 1) && (mGridCols == 1)) {
        videoFormat->setInt32("android._num-input-buffers", 1);
        videoFormat->setInt32("android._num-output-buffers", 1);
// QTI_BEGIN: 2018-05-10: Video: libstagefright: fix to retrieve thumbnail for HEIF content
        videoFormat->setInt32("thumbnail-mode", 1);
// QTI_END: 2018-05-10: Video: libstagefright: fix to retrieve thumbnail for HEIF content
// QTI_BEGIN: 2020-04-16: Video: libstagefright: Correct the thumbnail extn key
        videoFormat->setInt32("vendor.qti-ext-dec-thumbnail-mode.value", 1);
// QTI_END: 2020-04-16: Video: libstagefright: Correct the thumbnail extn key
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    } else {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        ALOGD("Enable multi-thread for Heif");
        mUseMultiThread = true;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    }

// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
    if (!isAvif(trackMeta())) {
// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
        bool isHW = mComponentName.startsWithIgnoreCase("c2.qti.");
        auto mode = isHW ? selectHeifMode(mGridCols, mTileWidth, mTileHeight) : HeifMode::TILE;
// QTI_BEGIN: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
        ALOGD("Setting HEIF mode %u", mode);
        if (mode == HeifMode::ROW) {
            mTileWidth *= mGridCols;
            mGridCols = 1;
            videoFormat->setInt32("vendor.qti-ext-heif-resolution.width", mWidth);
            videoFormat->setInt32("vendor.qti-ext-heif-resolution.height", mHeight);
        }
        videoFormat->setInt32("vendor.qti-ext-dec-heif-mode.value", mode);
        mTargetTiles = mGridCols * mGridRows;
    }

// QTI_END: 2025-04-28: Video: FrameDecoder: HEIF decoding optimization
    /// Set the importance for thumbnail.
    videoFormat->setInt32(KEY_IMPORTANCE, kThumbnailImportance);

    return videoFormat;
}

// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
status_t MediaImageDecoder::onExtractRect(FrameRect *rect) {
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
    // TODO:
    // This callback is for verifying whether we can decode the rect,
    // and if so, set up the internal variables for decoding.
    // Currently, rect decoding is restricted to sequentially decoding one
    // row of tiles at a time. We can't decode arbitrary rects, as the image
    // track doesn't yet support seeking by tiles. So all we do here is to
    // verify the rect against what we expect.
    // When seeking by tile is supported, this code should be updated to
    // set the seek parameters.
    if (rect == NULL) {
        if (mTilesDecoded > 0) {
            return ERROR_UNSUPPORTED;
        }
        mTargetTiles = mGridRows * mGridCols;
        return OK;
    }

    if (mTileWidth <= 0 || mTileHeight <=0) {
        return ERROR_UNSUPPORTED;
    }

    int32_t row = mTilesDecoded / mGridCols;
    int32_t expectedTop = row * mTileHeight;
    int32_t expectedBot = (row + 1) * mTileHeight;
    if (expectedBot > mHeight) {
        expectedBot = mHeight;
    }
    if (rect->left != 0 || rect->top != expectedTop
            || rect->right != mWidth || rect->bottom != expectedBot) {
        ALOGE("currently only support sequential decoding of slices");
        return ERROR_UNSUPPORTED;
    }

    // advance one row
    mTargetTiles = mTilesDecoded + mGridCols;
    return OK;
}

// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
status_t MediaImageDecoder::onOutputReceived(
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
        uint8_t* frameData,
        sp<ABuffer> imgObj,
        const sp<AMessage> &outputFormat, int64_t /*timeUs*/, bool *done) {
    if (outputFormat == NULL) {
        return ERROR_MALFORMED;
    }

    int32_t width, height, stride, slice_height;
    if (outputFormat->findInt32("width", &width) == false) {
        ALOGE("MediaImageDecoder::onOutputReceived:width is missing in outputFormat");
        return ERROR_MALFORMED;
    }
    if (outputFormat->findInt32("height", &height) == false) {
        ALOGE("MediaImageDecoder::onOutputReceived:height is missing in outputFormat");
        return ERROR_MALFORMED;
    }
    if (outputFormat->findInt32("stride", &stride) == false) {
        ALOGE("MediaImageDecoder::onOutputReceived:stride is missing in outputFormat");
        return ERROR_MALFORMED;
    }

    int32_t srcFormat;
    CHECK(outputFormat->findInt32("color-format", &srcFormat));

    uint32_t bitDepth = 8;
    if (COLOR_FormatYUVP010 == srcFormat) {
        bitDepth = 10;
    }

    if (outputFormat->findInt32("slice-height", &slice_height) == false) {
        ALOGE("MediaImageDecoder::onOutputReceived:slice-height is missing in outputFormat");
        return ERROR_MALFORMED;
// QTI_BEGIN: 2020-07-16: Video: stagefright: remove slice-height as mandatory parameter
    }
// QTI_END: 2020-07-16: Video: stagefright: remove slice-height as mandatory parameter

    if (mFrame == NULL) {
        sp<IMemory> frameMem = allocVideoFrame(
                trackMeta(), mWidth, mHeight, mTileWidth, mTileHeight, dstBpp(), bitDepth);

        if (frameMem == nullptr) {
            return NO_MEMORY;
        }

        mFrame = static_cast<VideoFrame*>(frameMem->unsecurePointer());

        setFrame(frameMem);
    }

    ColorConverter converter((OMX_COLOR_FORMATTYPE)srcFormat, dstFormat());

    uint32_t standard, range, transfer;
    if (!outputFormat->findInt32("color-standard", (int32_t*)&standard)) {
        standard = 0;
    }
    if (!outputFormat->findInt32("color-range", (int32_t*)&range)) {
        range = 0;
    }
    if (!outputFormat->findInt32("color-transfer", (int32_t*)&transfer)) {
        transfer = 0;
    }

    if (imgObj != nullptr) {
        MediaImage2 *imageData = nullptr;
        imageData = (MediaImage2 *)(imgObj.get()->data());
        if (imageData != nullptr) {
            converter.setSrcMediaImage2(*imageData);
        }
    }
    if (srcFormat == COLOR_FormatYUV420Flexible && imgObj.get() == nullptr) {
        return ERROR_UNSUPPORTED;
    }
    converter.setSrcColorSpace(standard, range, transfer);

    int32_t crop_left, crop_top, crop_right, crop_bottom;
    if (!outputFormat->findRect("crop", &crop_left, &crop_top, &crop_right, &crop_bottom)) {
        crop_left = crop_top = 0;
        crop_right = width - 1;
        crop_bottom = height - 1;
    }

    if (outputFormat->findInt32("slice-height", &slice_height) && slice_height > 0) {
        height = slice_height;
    }

    int32_t crop_width, crop_height;
    crop_width = crop_right - crop_left + 1;
    crop_height = crop_bottom - crop_top + 1;

    int32_t dstLeft, dstTop, dstRight, dstBottom;
    dstLeft = mTilesDecoded % mGridCols * crop_width;
    dstTop = mTilesDecoded / mGridCols * crop_height;
    dstRight = dstLeft + crop_width - 1;
    dstBottom = dstTop + crop_height - 1;

    // apply crop on bottom-right
    // TODO: need to move this into the color converter itself.
    if (dstRight >= mWidth) {
        crop_right = crop_left + mWidth - dstLeft - 1;
        dstRight = mWidth - 1;
    }
    if (dstBottom >= mHeight) {
        crop_bottom = crop_top + mHeight - dstTop - 1;
        dstBottom = mHeight - 1;
    }

    *done = (++mTilesDecoded >= mTargetTiles);

    if (converter.isValid()) {
        converter.convert(
                (const uint8_t *)frameData,
                width, height, stride,
                crop_left, crop_top, crop_right, crop_bottom,
                mFrame->getFlattenedData(),
                mFrame->mWidth, mFrame->mHeight, mFrame->mRowBytes,
                dstLeft, dstTop, dstRight, dstBottom);
        return OK;
    }

    ALOGE("Unable to convert from format 0x%08x to 0x%08x",
                srcFormat, dstFormat());
    return ERROR_UNSUPPORTED;
}

// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
bool MediaImageDecoder::outputLoop() {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    status_t err = OK;
    size_t index;
    int64_t ptsUs = 0LL;
    uint32_t flags = 0;
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
    constexpr nsecs_t kConditionTimeoutNs = nsecs_t(100) * 1000 * 1000;  // 100ms

// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    mOutInfo.lock()->mThrStarted = true;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
    do {
        if (err == OK){
            Mutexed<OutputInfo>::Locked outInfo(mOutInfo);
            if (outInfo->mSignalType == NONE) {
                err = outInfo.waitForConditionRelative(outInfo->mCond, kConditionTimeoutNs);
            }
        }

        if (err == OK) {
            // Check the signal type
            if (mOutInfo.lock()->mSignalType == EXIT) {
                ALOGI("Exiting the ImageOutputThread");
                return 0;
            } else if(mOutInfo.lock()->mSignalType == EXECUTE) {
                ALOGI("ImageOutputThread execution signalled ");
                mOutInfo.lock()->mSignalType = NONE;
                break;
            }
        } else if(err == -ETIMEDOUT) {
            err = OK;
            usleep(10 * 1000); // 10ms sleep
        } else {
            ALOGI("Error %d from waitForConditionRelative. Exiting thread!", err);
            return err;
        }
    } while(1);
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode

// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
    bool done = mOutInfo.lock()->mDone;
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    while (err == OK && !done) {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
        if (mOutInfo.lock()->mSignalType == EXIT) {
            ALOGI("Exiting the ImageOutputThread");
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
            return 0;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
        }

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        size_t offset, size;
        // wait for a decoded buffer
        err = mDecoder->dequeueOutputBuffer(
                &index,
                &offset,
                &size,
                &ptsUs,
                &flags,
                kBufferTimeOutUs);
        if (err == INFO_FORMAT_CHANGED) {
            ALOGV("Received format change");
            err = mDecoder->getOutputFormat(&mOutputFormat);
        } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
            ALOGV("Output buffers changed");
            err = OK;
        } else {
            if (err == -EAGAIN && --mOutInfo.lock()->mRetriesLeft > 0) {
                ALOGV("Timed-out waiting for output.. retries left = %zu",
                    mOutInfo.lock()->mRetriesLeft);
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                err = OK;
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
            } else if (err == OK) {
                // If we're seeking with CLOSEST option and obtained a valid targetTimeUs
                // from the extractor, decode to the specified frame. Otherwise we're done.
                ALOGV("Received an output buffer, timeUs=%lld", (long long)ptsUs);
                sp<MediaCodecBuffer> videoFrameBuffer;
                err = mDecoder->getOutputBuffer(index, &videoFrameBuffer);
                if (err != OK) {
                    ALOGE("failed to get output buffer %zu", index);
                    break;
                }
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                uint8_t* frameData = videoFrameBuffer->data();
                sp<ABuffer> imageData;
                videoFrameBuffer->meta()->findBuffer("image-data", &imageData);
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                if (mSurface != nullptr) {
                    if (!shouldDropOutput(ptsUs)) {
                        mDecoder->renderOutputBufferAndRelease(index);
                    } else {
                        mDecoder->releaseOutputBuffer(index);
                    }
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                    err = onOutputReceived(frameData, imageData, mOutputFormat, ptsUs, &done);
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                } else {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                    err = onOutputReceived(frameData, imageData, mOutputFormat, ptsUs, &done);
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                    mDecoder->releaseOutputBuffer(index);
                }
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
            } else {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                ALOGW("Received error %d (%s) instead of output", err, asString(err));
                done = true;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
            }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
            if(done) {
                mOutInfo.lock()->mDone = done;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
            }
        }
    }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    mOutInfo.lock()->mErrorCode = err;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""

// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    return done;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
}

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-03-06: Video: media: Rename ImageDecoder class
status_t MediaImageDecoder::extractInternal() {
// QTI_END: 2021-03-06: Video: media: Rename ImageDecoder class
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
    status_t err = OK;
    bool done = false;
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    bool outThreadRunning = false;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""

// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    {
        Mutexed<OutputInfo>::Locked outInfo(mOutInfo);
        outInfo->mRetriesLeft = kRetryCount;
        outInfo->mErrorCode = OK;
        outInfo->mDone = false;
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
        outInfo->mSignalType = NONE;
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
    }
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder

    if (mUseMultiThread && mThread == NULL) {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        mThread = new ImageOutputThread(this);
        err = mThread->run("ImageDecoderOutput");
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
        if (err != OK) {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
            ALOGE("Failed to create MediaImageDecoder output thread");
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
            mThread.clear();
            return err;
        }
    }

    do {
        size_t index;
        int64_t ptsUs = 0LL;
        uint32_t flags = 0;

        // Queue as many inputs as we possibly can, then block on dequeuing
        // outputs. After getting each output, come back and queue the inputs
        // again to keep the decoder busy.
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        while (mHaveMoreInputs) {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
            err = mDecoder->dequeueInputBuffer(&index, 0);
            if (err != OK) {
                ALOGV("Timed out waiting for input");
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                if (mOutInfo.lock()->mRetriesLeft) {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                    err = OK;
                }
                break;
            }
            sp<MediaCodecBuffer> codecBuffer;
            err = mDecoder->getInputBuffer(index, &codecBuffer);
            if (err != OK) {
                ALOGE("failed to get input buffer %zu", index);
                break;
            }

            MediaBufferBase *mediaBuffer = NULL;

            err = mSource->read(&mediaBuffer, &mReadOptions);
            mReadOptions.clearSeekTo();
            if (err != OK) {
                mHaveMoreInputs = false;
                if (!mFirstSample && err == ERROR_END_OF_STREAM) {
                    (void)mDecoder->queueInputBuffer(
                            index, 0, 0, 0, MediaCodec::BUFFER_FLAG_EOS);
                    err = OK;
                    flags |= MediaCodec::BUFFER_FLAG_EOS;
                } else {
                    ALOGW("Input Error: err=%d", err);
                }
                break;
            }

            if (mediaBuffer->range_length() > codecBuffer->capacity()) {
                ALOGE("buffer size (%zu) too large for codec input size (%zu)",
                        mediaBuffer->range_length(), codecBuffer->capacity());
                mHaveMoreInputs = false;
                err = BAD_VALUE;
            } else {
                codecBuffer->setRange(0, mediaBuffer->range_length());

                CHECK(mediaBuffer->meta_data().findInt64(kKeyTime, &ptsUs));
                memcpy(codecBuffer->data(),
                        (const uint8_t*)mediaBuffer->data() + mediaBuffer->range_offset(),
                        mediaBuffer->range_length());
                mFirstSample = false;
            }

            mediaBuffer->release();

            if (mHaveMoreInputs) {
                ALOGV("QueueInput: size=%zu ts=%" PRId64 " us flags=%x",
                        codecBuffer->size(), ptsUs, flags);

                err = mDecoder->queueInputBuffer(
                        index,
                        codecBuffer->offset(),
                        codecBuffer->size(),
                        ptsUs,
                        flags);

                if (flags & MediaCodec::BUFFER_FLAG_EOS) {
                    mHaveMoreInputs = false;
                }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""

                // Signal output thread after queueing at least 1 input
                if (mUseMultiThread && !outThreadRunning && mOutInfo.lock()->mThrStarted) {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
                    ALOGI("Signaling ImageOutputThread to start executing");
                    Mutexed<OutputInfo>::Locked outInfo(mOutInfo);
                    outInfo->mSignalType = EXECUTE;
                    outInfo->mCond.signal();
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                    outThreadRunning = true;
                }
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
            }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2023-06-20: Video: libstagefright: Stop processing inputs once output dequeuing is done

            // If output thread has completed processing, stop processing inputs
            if (mUseMultiThread && mOutInfo.lock()->mDone)
                break;
// QTI_END: 2023-06-20: Video: libstagefright: Stop processing inputs once output dequeuing is done
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
        }

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        // If output thread is still not running, signal it now.
        if (mUseMultiThread && !outThreadRunning && mOutInfo.lock()->mThrStarted) {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
            ALOGI("ImageOutputThread is not executing. Signaling now");
            Mutexed<OutputInfo>::Locked outInfo(mOutInfo);
            outInfo->mSignalType = EXECUTE;
            outInfo->mCond.signal();
// QTI_END: 2021-10-06: Video: libstagefright: Fix a corner case during HEIF decode
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
            outThreadRunning = true;
        }

        while (!mUseMultiThread && err == OK) {
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
            size_t offset, size;
            // wait for a decoded buffer
            err = mDecoder->dequeueOutputBuffer(
                    &index,
                    &offset,
                    &size,
                    &ptsUs,
                    &flags,
                    kBufferTimeOutUs);

            if (err == INFO_FORMAT_CHANGED) {
                ALOGV("Received format change");
                err = mDecoder->getOutputFormat(&mOutputFormat);
            } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
                ALOGV("Output buffers changed");
                err = OK;
            } else {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                if (err == -EAGAIN && --mOutInfo.lock()->mRetriesLeft > 0) {
                    ALOGV("Timed-out waiting for output.. retries left = %zu",
                        mOutInfo.lock()->mRetriesLeft);
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                    err = OK;
                } else if (err == OK) {
                    // If we're seeking with CLOSEST option and obtained a valid targetTimeUs
                    // from the extractor, decode to the specified frame. Otherwise we're done.
                    ALOGV("Received an output buffer, timeUs=%lld", (long long)ptsUs);
                    sp<MediaCodecBuffer> videoFrameBuffer;
                    err = mDecoder->getOutputBuffer(index, &videoFrameBuffer);
                    if (err != OK) {
                        ALOGE("failed to get output buffer %zu", index);
                        break;
                    }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                    uint8_t* frameData = videoFrameBuffer->data();
                    sp<ABuffer> imageData;
                    videoFrameBuffer->meta()->findBuffer("image-data", &imageData);
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                    if (mSurface != nullptr) {
                        if (!shouldDropOutput(ptsUs)) {
                            mDecoder->renderOutputBufferAndRelease(index);
                        } else {
                            mDecoder->releaseOutputBuffer(index);
                        }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                        err = onOutputReceived(frameData, imageData, mOutputFormat, ptsUs, &done);
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                    } else {
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                        err = onOutputReceived(frameData, imageData, mOutputFormat, ptsUs, &done);
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                        mDecoder->releaseOutputBuffer(index);
                    }
                } else {
                    ALOGW("Received error %d (%s) instead of output", err, asString(err));
                    done = true;
                }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
                if(done) {
                    mOutInfo.lock()->mDone = done;
                }
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
                break;
            }
        }
// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
// QTI_BEGIN: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
        err |= mOutInfo.lock()->mErrorCode;
    } while (err == OK && !mOutInfo.lock()->mDone);
// QTI_END: 2021-10-06: Video: Revert "Revert "Stagefright: Restructure HEIF decode multi-threading""
// QTI_BEGIN: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder

    if (err != OK) {
        ALOGE("failed to get video frame (err %d)", err);
    }

    return err;
}

// QTI_END: 2020-10-16: Video: stagefright: FrameDecoder: use 2 threads for heif decoder
}  // namespace android

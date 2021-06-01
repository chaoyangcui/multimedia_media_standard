/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "recorder_impl.h"
#include "media_log.h"
#include "securec.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>

namespace OHOS {
namespace Media {
#define CHECK_MEMBER_PTR_RETURN(param)                                \
    do {                                                              \
        if ((param) == nullptr) {                                       \
            MEDIA_ERR_LOG("ptr is null"); \
            return ERR_UNKNOWN;                                       \
        }                                                             \
    } while (0)

constexpr float RECORDER_DEFAULT_SPEED = 1.0f;
const double EPSINON = 1e-6;
constexpr uint32_t RECORDER_AUDIO_THREAD_PRIORITY = 19;
constexpr uint32_t RECORDER_VIDEO_THREAD_PRIORITY = 20;
constexpr uint32_t RECORDER_AUDIO_SAMPLES_PER_FRAME = 1024;

constexpr uint32_t RECORDER_VIDEO_SOURCE_ID_MASK = 0;
constexpr uint32_t RECORDER_AUDIO_SOURCE_ID_MASK = 0x100;
constexpr uint32_t RECORDER_DATA_SOURCE_ID_MASK = 0x200;
constexpr uint32_t INVALID_SOURCE_INDEX = 0xffffffff;

constexpr int32_t FRAME_RATE_FPS = 30;
constexpr int32_t BIT_RATE_KB = 4 * 1024;

Recorder::RecorderImpl::RecorderImpl()
{
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        sourceManager_[i].videoSource = nullptr;
        sourceManager_[i].videoSourceStarted = false;
        sourceManager_[i].videoSourcePaused = false;
        sourceManager_[i].videoTrackId = -1;
        sourceManager_[i].audioSource = nullptr;
        sourceManager_[i].audioSourceStarted = false;
        sourceManager_[i].audioSourcePaused = false;
        sourceManager_[i].audioTrackId = -1;
        sourceManager_[i].dataSource = nullptr;
        sourceManager_[i].dataSourceStarted = false;
        sourceManager_[i].dataSourcePaused = false;
        sourceManager_[i].dataTrackId = -1;
        sourceManager_[i].videoSourceConfig = {};
        sourceManager_[i].audioSourceConfig = {};
        sourceManager_[i].dataSourceConfig = {};
    }
    recorderSink_ = new(std::nothrow) RecorderSink();
    if (recorderSink_ != nullptr) {
        status_ = INITIALIZED;
    }
    MEDIA_INFO_LOG("ctor status_:%u", status_);
}

Recorder::RecorderImpl::~RecorderImpl()
{
    if (status_ != RELEASED) {
        (void)Release();
    }
    ResetConfig();
    if (recorderSink_ != nullptr) {
        delete recorderSink_;
        recorderSink_ = nullptr;
    }
    MEDIA_INFO_LOG("RecorderImpl dctor");
}

int32_t Recorder::RecorderImpl::ResetConfig()
{
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        sourceManager_[i].videoSourceStarted = false;
        sourceManager_[i].videoSourcePaused = false;
        sourceManager_[i].videoTrackId = -1;
        sourceManager_[i].audioSourceStarted = false;
        sourceManager_[i].audioSourcePaused = false;
        sourceManager_[i].audioTrackId = -1;
        sourceManager_[i].dataSourceStarted = false;
        sourceManager_[i].dataSourcePaused = false;
        sourceManager_[i].dataTrackId = -1;
        if (sourceManager_[i].videoSource != nullptr) {
            delete sourceManager_[i].videoSource;
            sourceManager_[i].videoSource = nullptr;
        }
        if (sourceManager_[i].audioSource != nullptr) {
            delete sourceManager_[i].audioSource;
            sourceManager_[i].audioSource = nullptr;
        }
        if (sourceManager_[i].dataSource != nullptr) {
            delete sourceManager_[i].dataSource;
            sourceManager_[i].dataSource = nullptr;
        }
        if (memset_s(&sourceManager_[i].videoSourceConfig, sizeof(RecorderVideoSourceConfig),
            0x00, sizeof(RecorderVideoSourceConfig)) != EOK) {
            MEDIA_ERR_LOG("Memset videoSourceConfig failed");
            return ERR_UNKNOWN;
        }
        sourceManager_[i].videoSourceConfig.speed = RECORDER_DEFAULT_SPEED;
        if (memset_s(&sourceManager_[i].audioSourceConfig, sizeof(RecorderAudioSourceConfig),
            0x00, sizeof(RecorderAudioSourceConfig)) != EOK) {
            MEDIA_ERR_LOG("Memset audioSourceConfig failed");
            return ERR_UNKNOWN;
        }
        if (memset_s(&sourceManager_[i].dataSourceConfig, sizeof(RecorderDataSourceConfig),
            0x00, sizeof(RecorderDataSourceConfig)) != EOK) {
            MEDIA_ERR_LOG("Memset dataSourceConfig failed");
            return ERR_UNKNOWN;
        }
    }
    MEDIA_INFO_LOG("ResetConfig SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::GetFreeVideoSourceID(int32_t &sourceId, uint32_t &freeIndex)
{
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].videoSource == nullptr) {
            sourceId = RECORDER_VIDEO_SOURCE_ID_MASK + i;
            freeIndex = i;
            return SUCCESS;
        }
    }
    MEDIA_ERR_LOG("Could get free video sourceId");
    return ERROR;
}

int32_t Recorder::RecorderImpl::GetFreeAudioSourceID(int32_t &sourceId, uint32_t &freeIndex)
{
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].audioSource == nullptr) {
            sourceId = RECORDER_AUDIO_SOURCE_ID_MASK + i;
            freeIndex = i;
            return SUCCESS;
        }
    }
    MEDIA_ERR_LOG("Could get free Audio sourceId");
    return ERROR;
}

int32_t Recorder::RecorderImpl::GetFreeDataSourceID(int32_t &sourceId, uint32_t &freeIndex)
{
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].dataSource == nullptr) {
            sourceId = RECORDER_DATA_SOURCE_ID_MASK + i;
            freeIndex = i;
            return SUCCESS;
        }
    }
    MEDIA_ERR_LOG("Could get free Data sourceId");
    return ERROR;
}

bool Recorder::RecorderImpl::GetIndexBySourceID(int32_t sourceId, uint32_t &validIndex)
{
    uint32_t index;
    if (static_cast<uint32_t>(sourceId) >= RECORDER_DATA_SOURCE_ID_MASK) { /* source type is data. */
        index = sourceId - RECORDER_DATA_SOURCE_ID_MASK;
    } else if (static_cast<uint32_t>(sourceId) >= RECORDER_AUDIO_SOURCE_ID_MASK) { /* source type is audio. */
        index = sourceId - RECORDER_AUDIO_SOURCE_ID_MASK;
    } else { /* source type is VIDEO. */
        index = sourceId - RECORDER_VIDEO_SOURCE_ID_MASK;
    }

    if (index >= RECORDER_SOURCE_MAX_CNT) {
        MEDIA_ERR_LOG("InValidSourceID sourceId:%x", sourceId);
        return false;
    }

    if (sourceManager_[index].videoSource != nullptr) {
        validIndex = index;
        return true;
    } else if (sourceManager_[index].audioSource != nullptr) {
        validIndex = index;
        return true;
    } else if (sourceManager_[index].dataSource != nullptr) {
        validIndex = index;
        return true;
    } else {
        validIndex = INVALID_SOURCE_INDEX;
        MEDIA_ERR_LOG("InValid source type.");
    }

    MEDIA_ERR_LOG("IsValidSourceID sourceId:%x", sourceId);
    return false;
}

int32_t Recorder::RecorderImpl::SetVideoSource(VideoSourceType source, int32_t &sourceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    if (source < VIDEO_SOURCE_SURFACE_YUV || source >= VIDEO_SOURCE_BUTT) {
        MEDIA_ERR_LOG("Only support  VIDEO_SOURCE_SURFACE_ES  source: %x is invalid", source);
        return ERR_INVALID_PARAM;
    }
    uint32_t freeIndex;
    int32_t ret = GetFreeVideoSourceID(sourceId, freeIndex);
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("GetFreeVideoSourceID  failed Ret: %d", ERR_NOFREE_CHANNEL);
        return ERR_NOFREE_CHANNEL;
    }

    sourceManager_[freeIndex].videoSource = new RecorderVideoSource();
    MEDIA_INFO_LOG("Video Source :%d Set SUCCESS sourceId:%x", source, sourceId);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetVideoEncoder(int32_t sourceId, VideoCodecFormat encoder)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (encoder != VIDEO_DEFAULT && encoder != H264 && encoder != HEVC) {
        MEDIA_ERR_LOG("Input VideoCodecFormat : %d is invalid", encoder);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].videoSourceConfig.videoFormat = encoder;
    MEDIA_INFO_LOG("Video Encoder :%d Set SUCCESS", encoder);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetVideoSize(int32_t sourceId, int32_t width, int32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (width <= 0 || height <= 0) {
        MEDIA_ERR_LOG("Input VideoSize width: %d width: %d is invalid", width, height);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].videoSourceConfig.width = width;
    sourceManager_[validIndex].videoSourceConfig.height = height;
    int ret = sourceManager_[validIndex].videoSource->SetSurfaceSize(width, height);
    MEDIA_INFO_LOG("Video Size width:%d height:%d", width, height);
    return ret;
}

int32_t Recorder::RecorderImpl::SetVideoFrameRate(int32_t sourceId, int32_t frameRate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (frameRate <= 0) {
        MEDIA_ERR_LOG("Input frameRate %d is invalid", frameRate);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].videoSourceConfig.frameRate = frameRate;
    MEDIA_INFO_LOG("Video frameRate:%d ", frameRate);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetVideoEncodingBitRate(int32_t sourceId, int32_t rate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (rate <= 0) {
        MEDIA_ERR_LOG("Input rate %d is invalid", rate);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].videoSourceConfig.bitRate = rate;
    MEDIA_INFO_LOG("Video Encoding BitRate:%d ", rate);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetCaptureRate(int32_t sourceId, double fps)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (((fps > -EPSINON) && (fps < EPSINON)) || fps < 0.0) {
        MEDIA_ERR_LOG("Input rate %lf is invalid", fps);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].videoSourceConfig.captureRate = fps;
    MEDIA_INFO_LOG("Video Capture Rate:%lf ", fps);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetOrientationHint(int32_t sourceId, int32_t degree)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (degree <= 0) {
        MEDIA_ERR_LOG("Input rate %d is invalid", degree);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].videoSourceConfig.degree = degree;
    return SUCCESS;
}

std::shared_ptr<OHOS::Surface> Recorder::RecorderImpl::GetSurface(int32_t sourceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return nullptr;
    }

    if (static_cast<uint32_t>(sourceId) >= RECORDER_DATA_SOURCE_ID_MASK) { /* source type is data. */
        return sourceManager_[validIndex].dataSource->GetSurface();
    } else { /* source type is VIDEO. */
        return sourceManager_[validIndex].videoSource->GetSurface();
    }
}

bool Recorder::RecorderImpl::IsValidAudioSource(AudioSourceType source)
{
    if (source <= AUDIO_SOURCE_INVALID || source > AUDIO_VOICE_PERFORMANCE) {
        MEDIA_ERR_LOG("Input AudioSourceType : %d is invalid", source);
        return false;
    }
    return true;
}

bool Recorder::RecorderImpl::IsPrepared()
{
    if (status_ != INITIALIZED) {
        MEDIA_ERR_LOG("IsPrepared ILLEGAL_STATE  status:%u", status_);
        return true;
    }
    return false;
}

int32_t Recorder::RecorderImpl::SetAudioSource(AudioSourceType source, int32_t &sourceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t freeIndex;
    int32_t ret = GetFreeAudioSourceID(sourceId, freeIndex);
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("GetFreeAudioSourceID  failed Ret: %d", ERR_NOFREE_CHANNEL);
        return ERR_NOFREE_CHANNEL;
    }
    if (!IsValidAudioSource(source)) {
        return ERR_INVALID_PARAM;
    }
    sourceManager_[freeIndex].audioSource = new RecorderAudioSource();
    sourceManager_[freeIndex].audioSourceConfig.inputSource = source;
    MEDIA_INFO_LOG("Audio Source :%d Set sourceId:%x SUCCESS", source, sourceId);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetAudioEncoder(int32_t sourceId, AudioCodecFormat encoder)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (encoder >= FORMAT_BUTT || encoder < AUDIO_DEFAULT) {
        MEDIA_ERR_LOG("Input AudioCodecFormat : %d is invalid", encoder);
        return ERR_INVALID_PARAM;
    }
    AudioCodecFormat audioFormat;
    switch (encoder) {
        case AAC_LC:
        case AAC_HE_V1:
        case AAC_HE_V2:
        case AAC_LD:
        case AAC_ELD:
            audioFormat = encoder;
            break;
        case AUDIO_DEFAULT:
            audioFormat = AAC_LC;
            break;
        default:
            MEDIA_ERR_LOG("Input AudioCodecFormat : %d is invalid", encoder);
            return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].audioSourceConfig.audioFormat = audioFormat;
    MEDIA_INFO_LOG("Audio Encoder :%d Set", encoder);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetAudioSampleRate(int32_t sourceId, int32_t rate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (rate <= 0) {
        MEDIA_ERR_LOG("Input AudioSampleRate %d is invalid", rate);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].audioSourceConfig.sampleRate = rate;
    MEDIA_INFO_LOG("Audio Sample Rate :%d Set", rate);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetAudioChannels(int32_t sourceId, int32_t num)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (num <= 0) {
        MEDIA_ERR_LOG("Input AudioChannels %d is invalid", num);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].audioSourceConfig.channelCount = num;
    MEDIA_INFO_LOG("Audio Channels :%d Set", num);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetAudioEncodingBitRate(int32_t sourceId, int32_t bitRate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    uint32_t validIndex;
    if (!GetIndexBySourceID(sourceId, validIndex)) {
        MEDIA_ERR_LOG("Input sourceId : %x is invalid", sourceId);
        return ERR_INVALID_PARAM;
    }
    if (bitRate <= 0) {
        MEDIA_ERR_LOG("Input AudioEncodingBitRate %d is invalid", bitRate);
        return ERR_INVALID_PARAM;
    }
    sourceManager_[validIndex].audioSourceConfig.bitRate = bitRate;
    MEDIA_INFO_LOG("Audio Encoding BitRate :%d Set", bitRate);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetDataSource(DataSourceType source, int32_t &sourceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }

    if (source > METADATA) {
        MEDIA_ERR_LOG("dataSource source =  %d is invalid", source);
        return ERR_INVALID_PARAM;
    }
    uint32_t freeIndex;
    int32_t ret = GetFreeDataSourceID(sourceId, freeIndex);
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("GetFreeVideoSourceID  failed Ret: %d", ERR_NOFREE_CHANNEL);
        return ERR_NOFREE_CHANNEL;
    }
    sourceManager_[freeIndex].dataSource = new RecorderDataSource();
    sourceManager_[freeIndex].dataSourceConfig.dataType = (DataType)source;
    MEDIA_INFO_LOG("Data Source Set SUCCESS sourceId:%x", sourceId);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetLocation(int32_t latitude, int32_t longitude)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    return recorderSink_->SetLocation(latitude, longitude);
}

int32_t Recorder::RecorderImpl::SetMaxDuration(int32_t duration)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    MEDIA_INFO_LOG("Max Duration :%d Set", duration);
    return recorderSink_->SetMaxDuration(duration);
}

int32_t Recorder::RecorderImpl::SetOutputFormat(OutputFormatType format)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    OutputFormat outPutFormat;
    switch (format) {
        case FORMAT_MPEG_4:
            outPutFormat = OUTPUT_FORMAT_MPEG_4;
            break;
        case FORMAT_TS:
            outPutFormat = OUTPUT_FORMAT_TS;
            break;
        case FORMAT_DEFAULT:
            outPutFormat = OUTPUT_FORMAT_MPEG_4;
            MEDIA_WARNING_LOG("format: %d use default OUTPUT_FORMAT_MPEG_4", format);
            break;
        default:
            MEDIA_ERR_LOG("invalid OutputFormatType : %d ", format);
            return ERR_INVALID_PARAM;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    MEDIA_INFO_LOG("Output Format :%d Set", format);
    return recorderSink_->SetOutputFormat(outPutFormat);
}

int32_t Recorder::RecorderImpl::SetOutputPath(const string &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    if (path.data() == nullptr) {
        MEDIA_ERR_LOG("The path is nullptr");
        return ERR_INVALID_PARAM;
    }
    if (access(path.c_str(), F_OK) == -1) {
        MEDIA_ERR_LOG("The path doesn't exist");
        return ERR_INVALID_PARAM;
    }
    struct stat fileStatus;
    stat(path.c_str(), &fileStatus);
    if (!S_ISDIR(fileStatus.st_mode)) {
        MEDIA_ERR_LOG("The path is a file!");
        return ERR_INVALID_PARAM;
    }
    if (access(path.c_str(),  W_OK) == -1) {
        MEDIA_ERR_LOG("The path has No write permission.");
        return ERR_INVALID_PARAM;
    }
    recorderSink_->SetOutputPath(path);
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::IsValidFileFd(int32_t fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        MEDIA_ERR_LOG("Fail to get File Status Flags err: %d", errno);
        return ERR_INVALID_OPERATION;
    }
    // fd must be in read-write mode or write-only mode.
    uint32_t flagsCheck = static_cast<uint32_t>(flags);
    if ((flagsCheck & (O_RDWR | O_WRONLY)) == 0) {
        MEDIA_ERR_LOG("File descriptor is not in read-write mode or write-only mode fd:%d", fd);
        return ERR_INVALID_OPERATION;
    }
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetOutputFile(int32_t fd)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    MEDIA_INFO_LOG("Output File :%d Set", fd);
    if (IsValidFileFd(fd) != SUCCESS) {
        MEDIA_ERR_LOG("Fail to get File Status Flags from fd: %d", fd);
        return ERR_INVALID_PARAM;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    return recorderSink_->SetOutputFile(fd);
}

int32_t Recorder::RecorderImpl::SetNextOutputFile(int32_t fd)
{
    std::lock_guard<std::mutex> lock(mutex_);
    MEDIA_INFO_LOG("Next Output File :%d Set", fd);
    if (IsValidFileFd(fd) != SUCCESS) {
        MEDIA_ERR_LOG("Fail to get File Status Flags from fd: %d", fd);
        return ERR_INVALID_PARAM;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    return recorderSink_->SetNextOutputFile(fd);
}

int32_t Recorder::RecorderImpl::SetMaxFileSize(int64_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    MEDIA_INFO_LOG("Max File Size :%lld Set", size);
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    return recorderSink_->SetMaxFileSize(size);
}

int32_t Recorder::RecorderImpl::SetRecorderCallback(const std::shared_ptr<RecorderCallback> &callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    if (callback == nullptr) {
        MEDIA_ERR_LOG("SetRecorderCallback callback is nullptr");
        return ERR_INVALID_PARAM;
    }
    MEDIA_INFO_LOG("RecorderCallback Set");
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    return recorderSink_->SetRecorderCallback(callback);
}

int32_t Recorder::RecorderImpl::PrepareAudioSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].audioSource != nullptr) {
            TrackSource trackSource;
            ret = GetAudioTrackSource(sourceManager_[i].audioSourceConfig, trackSource);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("GetAudioTrackSource  failed Ret: 0x%x", ret);
                return ret;
            }
            ret = sourceManager_[i].audioSource->Init(sourceManager_[i].audioSourceConfig);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("audioSource Init failed Ret: 0x%x", ret);
                return ret;
            }
            int32_t trackId;
            CHECK_MEMBER_PTR_RETURN(recorderSink_);
            ret = recorderSink_->AddTrackSource(trackSource, trackId);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("AddTrackSource  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].audioTrackId = trackId;
            MEDIA_INFO_LOG("recorderSink_ AddTrackSource .audioTrackId :0x%x",
                           sourceManager_[i].audioTrackId);
        }
    }
    MEDIA_INFO_LOG("Prepare AudioSource SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::PrepareDataSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].dataSource != nullptr) {
            TrackSource trackSource;
            ret = GetDataTrackSource(sourceManager_[i].dataSourceConfig, trackSource);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("GetDataTrackSource  failed Ret: 0x%x", ret);
                return ret;
            }
            int32_t trackId;
            CHECK_MEMBER_PTR_RETURN(recorderSink_);
            ret = recorderSink_->AddTrackSource(trackSource, trackId);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("AddTrackSource  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].dataTrackId = trackId;
            MEDIA_INFO_LOG("recorderSink_ AddTrackSource dataTrackId :0x%x",
                           sourceManager_[i].dataTrackId);
        }
    }
    MEDIA_INFO_LOG("Prepare DataSource SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::PrepareVideoSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].videoSource != nullptr) {
            TrackSource trackSource;
            ret = GetVideoTrackSource(sourceManager_[i].videoSourceConfig, trackSource);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("GetVideoTrackSource  failed Ret: 0x%x", ret);
                return ret;
            }
            int32_t trackId;
            CHECK_MEMBER_PTR_RETURN(recorderSink_);
            ret = recorderSink_->AddTrackSource(trackSource, trackId);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("AddTrackSource  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].videoTrackId = trackId;
            MEDIA_INFO_LOG("recorderSink_ AddTrackSource videoTrackId :0x%x",
                           sourceManager_[i].videoTrackId);
        }
    }
    MEDIA_INFO_LOG("Prepare VideoSource SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::GetVideoTrackSource(const RecorderVideoSourceConfig &videoSourceConfig,
                                                    TrackSource &trackSource)
{
    trackSource.trackSourceType = TRACK_SOURCE_TYPE_VIDEO;
    switch (videoSourceConfig.videoFormat) {
        case H264:
            trackSource.trackSourceInfo.videoInfo.codecType = CODEC_H264;
            break;
        case HEVC:
            trackSource.trackSourceInfo.videoInfo.codecType = CODEC_H265;
            break;
        default:
            MEDIA_ERR_LOG("unsupport videoFormat format: %d", videoSourceConfig.videoFormat);
            return ERR_INVALID_PARAM;
    }
    if (videoSourceConfig.width <= 0 || videoSourceConfig.height <= 0 ||
        videoSourceConfig.bitRate <= 0 || videoSourceConfig.frameRate <= 0) {
        MEDIA_ERR_LOG("VideoTrackSource not prepared");
        return ERR_INVALID_PARAM;
    }
    trackSource.trackSourceInfo.videoInfo.width = videoSourceConfig.width;
    trackSource.trackSourceInfo.videoInfo.height = videoSourceConfig.height;
    trackSource.trackSourceInfo.videoInfo.bitRate = videoSourceConfig.bitRate;
    trackSource.trackSourceInfo.videoInfo.frameRate = videoSourceConfig.frameRate;
    trackSource.trackSourceInfo.videoInfo.keyFrameInterval = videoSourceConfig.frameRate;
    trackSource.trackSourceInfo.videoInfo.speed = videoSourceConfig.speed;
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::GetAudioTrackSource(const RecorderAudioSourceConfig &audioSourceConfig,
                                                    TrackSource &trackSource)
{
    trackSource.trackSourceType = TRACK_SOURCE_TYPE_AUDIO;
    switch (audioSourceConfig.audioFormat) {
        case AAC_LC:
        case AAC_HE_V1:
        case AAC_HE_V2:
        case AAC_LD:
        case AAC_ELD:
        case AUDIO_DEFAULT:
            trackSource.trackSourceInfo.audioInfo.codecType = CODEC_AAC;
            break;
        default:
            MEDIA_ERR_LOG("unsupport audiFormat format: %d", audioSourceConfig.audioFormat);
            return ERR_INVALID_PARAM;
    }
    if (audioSourceConfig.bitRate == 0 ||
        audioSourceConfig.sampleRate == 0 ||
        audioSourceConfig.channelCount == 0) {
        MEDIA_ERR_LOG("not set bitRate:%d sampleRate:%d channelCount:%d", audioSourceConfig.bitRate,
                      audioSourceConfig.sampleRate, audioSourceConfig.channelCount);
        return ERR_INVALID_PARAM;
    }
    trackSource.trackSourceInfo.audioInfo.sampleRate = audioSourceConfig.sampleRate;
    trackSource.trackSourceInfo.audioInfo.channelCount = audioSourceConfig.channelCount;
    switch (audioSourceConfig.bitWidth) {
        case BIT_WIDTH_8:
         trackSource.trackSourceInfo.audioInfo.sampleBitWidth = AUDIO_SAMPLE_FMT_S8;
            break;
        case BIT_WIDTH_16:
         trackSource.trackSourceInfo.audioInfo.sampleBitWidth = AUDIO_SAMPLE_FMT_S16;
            break;
        case BIT_WIDTH_24:
            trackSource.trackSourceInfo.audioInfo.sampleBitWidth = AUDIO_SAMPLE_FMT_S24;
            break;
        default:
            MEDIA_WARNING_LOG("default sampleBitWidth: %d", audioSourceConfig.bitWidth);
            trackSource.trackSourceInfo.audioInfo.sampleBitWidth = AUDIO_SAMPLE_FMT_S16;
            break;
    }
    trackSource.trackSourceInfo.audioInfo.samplesPerFrame = RECORDER_AUDIO_SAMPLES_PER_FRAME;
    trackSource.trackSourceInfo.audioInfo.avgBytesPerSec = audioSourceConfig.bitRate;
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::GetDataTrackSource(const RecorderDataSourceConfig &dataSourceConfig,
    TrackSource &trackSource)
{
    trackSource.trackSourceType = TRACK_SOURCE_TYPE_DATA;
    trackSource.trackSourceInfo.dataInfo.frameRate = FRAME_RATE_FPS;
    trackSource.trackSourceInfo.dataInfo.bitRate = BIT_RATE_KB;
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::PrepareRecorderSink()
{
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    return recorderSink_->Prepare();
}

int32_t Recorder::RecorderImpl::Prepare()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (IsPrepared()) {
        MEDIA_ERR_LOG("IsPrepared status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    int32_t ret = PrepareRecorderSink();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("PrepareRecorderSink  failed Ret: %d", ret);
        return ret;
    }
    ret = PrepareVideoSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("PrepareVideoSource  failed Ret: %d", ret);
        return ret;
    }
    ret = PrepareAudioSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("PrepareAudioSource  failed Ret: %d", ret);
        return ret;
    }

    ret = PrepareDataSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("PrepareDataSource  failed Ret: %d", ret);
        return ret;
    }
    status_ = PREPPARED;
    MEDIA_INFO_LOG("Prepare SUCCESS");
    return SUCCESS;
}

void DataSourceProcess(const SourceManager *dataSourceManager, const RecorderSink *recorderSink)
{
    MEDIA_INFO_LOG("DataSourceManager start");
    if (dataSourceManager == nullptr || recorderSink == nullptr) {
        return;
    }
    if (dataSourceManager->dataSource == nullptr) {
        MEDIA_ERR_LOG("dataSource is nullptr");
        return;
    }
    prctl(PR_SET_NAME, "DataSourceProcess", 0, 0, 0);
    struct sched_param param = {};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_getschedparam(&attr, &param);
    param.sched_priority = RECORDER_AUDIO_THREAD_PRIORITY;
    pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    MEDIA_INFO_LOG("DataSourceProcess sched_priority:%d", param.sched_priority);
    while (dataSourceManager->dataSourceStarted) {
        RecorderSourceBuffer buffer;
        int32_t ret = dataSourceManager->dataSource->AcquireBuffer(buffer, true);
        if (ret != SUCCESS) {
            MEDIA_ERR_LOG("dataSource Read failed ret:0x%x", ret);
            continue;
        }
        if (!dataSourceManager->dataSourcePaused) {
            FormatFrame frameData;
            frameData.frameType = FRAME_TYPE_DATA;
            frameData.trackId = dataSourceManager->dataTrackId;
            frameData.isKeyFrame = false;
            frameData.timestampUs = buffer.timeStamp;
            frameData.data = buffer.dataAddr;
            frameData.len = buffer.dataLen;
            ret = recorderSink->WriteData(dataSourceManager->dataTrackId, frameData);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("Data WriteData failed 0x%x", ret);
            }
        }
        dataSourceManager->dataSource->ReleaseBuffer(buffer);
    }
    MEDIA_DEBUG_LOG("dataSourceManager over");
}

int32_t Recorder::RecorderImpl::StartDataSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].dataSource != nullptr) {
            ret = sourceManager_[i].dataSource->Start();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("DataSource Start  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].dataSourceStarted = true;
            sourceManager_[i].dataProcessThread = std::thread(DataSourceProcess, &sourceManager_[i], recorderSink_);
        }
    }
    MEDIA_INFO_LOG("Start Data Source SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::StopDataSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].dataSource != nullptr) {
            MEDIA_DEBUG_LOG("dataSource->Stop");
            sourceManager_[i].dataSourceStarted = false;
            ret = sourceManager_[i].dataSource->Stop();
            MEDIA_DEBUG_LOG("dataSource->Stop out");
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("dataSource Stop  failed Ret: 0x%x", ret);
                return ret;
            }
            MEDIA_DEBUG_LOG("dataProcessThread.join");
            sourceManager_[i].dataProcessThread.join();
            MEDIA_DEBUG_LOG("dataProcessThread.join out");
        }
    }
    MEDIA_DEBUG_LOG("Stop Data Source SUCCESS");
    return SUCCESS;
}

void AudioSourceProcess(const SourceManager *audioSourceManager, const RecorderSink *recorderSink)
{
    MEDIA_INFO_LOG("audioSourceManager");
    if ((audioSourceManager == nullptr) || (recorderSink == nullptr)) {
        return;
    }
    if (audioSourceManager->audioSource == nullptr) {
        MEDIA_ERR_LOG("audioSource is nullptr");
        return;
    }
    prctl(PR_SET_NAME, "AudioSourceProcess", 0, 0, 0);
    struct sched_param param = {};
    pthread_attr_t attr;
    /* initialized with default attributes */
    pthread_attr_init(&attr);
    pthread_attr_getschedparam(&attr, &param);
    param.sched_priority = RECORDER_AUDIO_THREAD_PRIORITY;
    pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    MEDIA_INFO_LOG("AudioSourceProcess sched_priority:%d", param.sched_priority);
    while (audioSourceManager->audioSourceStarted) {
        RecorderSourceBuffer buffer;
        int32_t ret = audioSourceManager->audioSource->AcquireBuffer(buffer, false);
        if (ret != SUCCESS) {
            MEDIA_ERR_LOG("audioSource Read failed ret:0x%x", ret);
            continue;
        }
        if (!audioSourceManager->audioSourcePaused) {
            FormatFrame frameData;
            frameData.frameType = FRAME_TYPE_AUDIO;
            frameData.trackId = audioSourceManager->audioTrackId;
            frameData.isKeyFrame = false;
            frameData.timestampUs = buffer.timeStamp;
            frameData.data = buffer.dataAddr;
            frameData.len = buffer.dataLen;
            ret = recorderSink->WriteData(audioSourceManager->audioTrackId, frameData);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("Audio WriteData failed 0x%x", ret);
            }
        }
        audioSourceManager->audioSource->ReleaseBuffer(buffer);
    }
    MEDIA_DEBUG_LOG("audioSourceManager over");
}

int32_t Recorder::RecorderImpl::StartAudioSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].audioSource != nullptr) {
            ret = sourceManager_[i].audioSource->Start();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("audioSource Start  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].audioSourceStarted = true;
            sourceManager_[i].audioProcessThread = std::thread(AudioSourceProcess, &sourceManager_[i], recorderSink_);
        }
    }
    MEDIA_INFO_LOG("Start Audio Source SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::StopAudioSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].audioSource != nullptr) {
            MEDIA_DEBUG_LOG("audioSource->Stop");
            sourceManager_[i].audioSourceStarted = false;
            ret = sourceManager_[i].audioSource->Stop();
            MEDIA_DEBUG_LOG("audioSource->Stop out");
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("audioSource Stop  failed Ret: 0x%x", ret);
                return ret;
            }
            MEDIA_DEBUG_LOG("audioProcessThread.join");
            sourceManager_[i].audioProcessThread.join();
            MEDIA_DEBUG_LOG("audioProcessThread.join out");
        }
    }
    MEDIA_DEBUG_LOG("Stop Audio Source SUCCESS");
    return SUCCESS;
}

void VideoSourceProcess(const SourceManager *videoSourceManager, const RecorderSink *recorderSink)
{
    MEDIA_DEBUG_LOG("videoSourceManager");
    if (videoSourceManager == nullptr) {
        return;
    }
    if (videoSourceManager->videoSource == nullptr || recorderSink == nullptr) {
        MEDIA_ERR_LOG("videoSource recorderSink is nullptr");
        return;
    }
    prctl(PR_SET_NAME, "VideoSourceProcess", 0, 0, 0);
    struct sched_param param = {};
    pthread_attr_t attr;
    /* initialized with default attributes */
    pthread_attr_init(&attr);
    pthread_attr_getschedparam(&attr, &param);
    param.sched_priority = RECORDER_VIDEO_THREAD_PRIORITY;
    pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    MEDIA_INFO_LOG("VideoSourceProcess sched_priority:%d", param.sched_priority);
    while (videoSourceManager->videoSourceStarted) {
        RecorderSourceBuffer buffer;
        int32_t ret = videoSourceManager->videoSource->AcquireBuffer(buffer, true);
        if (ret != SUCCESS) {
            MEDIA_ERR_LOG("videoSource AcquireBuffer failed ret:0x%x", ret);
            continue;
        }
        if (!videoSourceManager->videoSourcePaused) {
            FormatFrame frameData;
            frameData.frameType = FRAME_TYPE_VIDEO;
            frameData.trackId = videoSourceManager->videoTrackId;
            frameData.isKeyFrame = buffer.keyFrameFlag;
            frameData.timestampUs = buffer.timeStamp;
            frameData.data = buffer.dataAddr;
            frameData.len = buffer.dataLen;
            ret = recorderSink->WriteData(videoSourceManager->videoTrackId, frameData);
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("video WriteData failed 0x%x", ret);
            }
        }
        videoSourceManager->videoSource->ReleaseBuffer(buffer);
    }
    MEDIA_DEBUG_LOG("videoSourceManager over");
}


int32_t Recorder::RecorderImpl::StartVideoSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].videoSource != nullptr) {
            ret = sourceManager_[i].videoSource->Start();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("videoSource->Start  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].videoSourceStarted = true;
            sourceManager_[i].videoProcessThread = std::thread(VideoSourceProcess, &sourceManager_[i], recorderSink_);
        }
    }
    MEDIA_INFO_LOG("Start Video Source SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::StopVideoSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].videoSource != nullptr) {
            sourceManager_[i].videoSourceStarted = false;
            ret = sourceManager_[i].videoSource->Stop();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("videoSource->Start  failed Ret: 0x%x", ret);
                return ret;
            }
            MEDIA_DEBUG_LOG("videoProcessThread.join");
            sourceManager_[i].videoProcessThread.join();
            MEDIA_DEBUG_LOG("videoProcessThread.join over");
        }
    }
    MEDIA_DEBUG_LOG("Stop Video Source SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t time;
    struct timeval begin = { 0, 0 };
    struct timeval end = { 0, 0 };
    const int64_t timeUs = 1000000;
    const int64_t timeMs = 1000;
    int64_t diffSecToUs;
    int64_t diffUsec;
    gettimeofday(&begin, NULL);
    if (status_ != PREPPARED && status_ != PAUSED && status_ != STOPPED) {
        MEDIA_ERR_LOG("Start ILLEGAL_STATE  status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    int32_t ret = recorderSink_->Start();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("recorderSink_  Start failed Ret: %d", ret);
        return ret;
    }
    gettimeofday(&end, NULL);
    diffSecToUs = (int64_t)(end.tv_sec - begin.tv_sec) * timeUs;
    diffUsec = (int64_t)(end.tv_usec - begin.tv_usec);
    time = (int64_t)((diffSecToUs + diffUsec) / timeMs);
    MEDIA_INFO_LOG("recorderSink_ Start cost:%lld", time);
    gettimeofday(&begin, NULL);
    ret = StartVideoSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StartVideoSource  Start failed Ret: %d", ret);
        return ret;
    }
    gettimeofday(&end, NULL);
    diffSecToUs = (int64_t)(end.tv_sec - begin.tv_sec) * timeUs;
    diffUsec = (int64_t)(end.tv_usec - begin.tv_usec);
    time = (int64_t)((diffSecToUs + diffUsec) / timeMs);
    MEDIA_INFO_LOG("StartVideoSource  cost:%lld", time);
    gettimeofday(&begin, NULL);
    ret = StartAudioSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StartAudioSource  Start failed Ret: %d", ret);
        return ret;
    }
    ret = StartDataSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StartDataSource  Start failed Ret: %d", ret);
        return ret;
    }
    gettimeofday(&end, NULL);
    diffSecToUs = (int64_t)(end.tv_sec - begin.tv_sec) * timeUs;
    diffUsec = (int64_t)(end.tv_usec - begin.tv_usec);
    time = (int64_t)((diffSecToUs + diffUsec) / timeMs);
    MEDIA_INFO_LOG("StartAudioSource  cost:%lld", time);
    MEDIA_INFO_LOG("Start Recorder SUCCESS");
    status_ = RECORDING;
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::PauseAudioSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].audioSource != nullptr) {
            ret = sourceManager_[i].audioSource->Pause();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("audioSource Pause  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].audioSourcePaused = true;
        }
    }
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::PauseVideoSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].videoSource != nullptr) {
            ret = sourceManager_[i].videoSource->Pause();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("videoSource->Pause  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].videoSourcePaused = true;
        }
    }
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::PauseDataSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].dataSource != nullptr) {
            ret = sourceManager_[i].dataSource->Pause();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("dataSource->Pause  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].dataSourcePaused = true;
        }
    }
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::ResumeAudioSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].audioSource != nullptr) {
            ret = sourceManager_[i].audioSource->Resume();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("audioSource Resume  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].audioSourcePaused = false;
        }
    }
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::ResumeVideoSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].videoSource != nullptr) {
            ret = sourceManager_[i].videoSource->Resume();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("videoSource->Resume  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].videoSourcePaused = false;
        }
    }
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::ResumeDataSource()
{
    int32_t ret;
    for (uint32_t i = 0; i < RECORDER_SOURCE_MAX_CNT; i++) {
        if (sourceManager_[i].dataSource != nullptr) {
            ret = sourceManager_[i].dataSource->Resume();
            if (ret != SUCCESS) {
                MEDIA_ERR_LOG("dataSource->Resume  failed Ret: 0x%x", ret);
                return ret;
            }
            sourceManager_[i].dataSourcePaused = false;
        }
    }
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::Pause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != RECORDING) {
        MEDIA_ERR_LOG("Pause ILLEGAL_STATE  status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    int32_t ret = PauseVideoSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StopVideoSource  Pause failed Ret: %d", ret);
        return ret;
    }
    ret = PauseAudioSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StopAudioSource  Pause failed Ret: %d", ret);
        return ret;
    }
    ret = PauseDataSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StopDataSource  Pause failed Ret: %d", ret);
        return ret;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    ret = recorderSink_->Stop(false);
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("recorderSink_ Stop ! ret: 0x%x", ret);
        return ret;
    }
    status_ = PAUSED;
    MEDIA_INFO_LOG("Pause Recorder SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::Resume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != PAUSED) {
        MEDIA_ERR_LOG("Resume ILLEGAL_STATE  status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    int32_t ret = recorderSink_->Start();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("recorderSink_ Start ! ret: 0x%x", ret);
        return ret;
    }
    ret = ResumeVideoSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("VideoSource  Pause failed Ret: %d", ret);
        return ret;
    }
    ret = ResumeAudioSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("pAudioSource  Resume failed Ret: %d", ret);
        return ret;
    }
    ret = ResumeDataSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("pAudioSource  Resume failed Ret: %d", ret);
        return ret;
    }
    status_ = RECORDING;
    MEDIA_INFO_LOG("Resume Recorder SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::StopInternal(bool block)
{
    int64_t time;
    struct timeval begin = { 0, 0 };
    struct timeval end = { 0, 0 };
    const int64_t timeUs = 1000000;
    const int64_t timeMs = 1000;
    int64_t diffSecToUs;
    int64_t diffUsec;
    gettimeofday(&begin, NULL);
    MEDIA_DEBUG_LOG("StopInternal");
    int32_t ret = StopVideoSource();
    MEDIA_DEBUG_LOG("StopVideoSource");
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StopVideoSource  Start failed Ret: %d", ret);
        return ret;
    }
    gettimeofday(&end, NULL);
    diffSecToUs = (int64_t)(end.tv_sec - begin.tv_sec) * timeUs;
    diffUsec = (int64_t)(end.tv_usec - begin.tv_usec);
    time = (int64_t)((diffSecToUs + diffUsec) / timeMs);
    MEDIA_INFO_LOG("StopVideoSource cost:%lld", time);
    gettimeofday(&begin, NULL);
    MEDIA_DEBUG_LOG("StopAudioSource");
    ret = StopAudioSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StopAudioSource  Start failed Ret: %d", ret);
        return ret;
    }
    gettimeofday(&end, NULL);
    diffSecToUs = (int64_t)(end.tv_sec - begin.tv_sec) * timeUs;
    diffUsec = (int64_t)(end.tv_usec - begin.tv_usec);
    time = (int64_t)((diffSecToUs + diffUsec) / timeMs);
    MEDIA_INFO_LOG("StopAudioSource cost:%lld", time);
    CHECK_MEMBER_PTR_RETURN(recorderSink_);

    ret = StopDataSource();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("StopDataSource failed Ret: %d", ret);
        return ret;
    }

    gettimeofday(&begin, NULL);
    ret = recorderSink_->Stop(block);
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("recorderSink_ Stop ! ret: 0x%x", ret);
        return ret;
    }
    gettimeofday(&end, NULL);
    diffSecToUs = (int64_t)(end.tv_sec - begin.tv_sec) * timeUs;
    diffUsec = (int64_t)(end.tv_usec - begin.tv_usec);
    time = (int64_t)((diffSecToUs + diffUsec) / timeMs);
    MEDIA_INFO_LOG("recorderSink Stop cost:%lld", time);
    status_ = STOPPED;
    MEDIA_DEBUG_LOG("Stop Recorder SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::Stop(bool block)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != RECORDING &&
        status_ != PAUSED) {
        MEDIA_ERR_LOG(" Stop ILLEGAL_STATE  status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    return StopInternal(block);
}

int32_t Recorder::RecorderImpl::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t ret;
    if (status_ == RELEASED) {
        MEDIA_ERR_LOG("already RELEASED");
        return ERR_ILLEGAL_STATE;
    }
    if (status_ == RECORDING ||
        status_ == PAUSED) {
        if ((ret = StopInternal(false)) != SUCCESS) {
            MEDIA_ERR_LOG("Reset StopInternal err");
            return ret;
        }
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    ret = recorderSink_->Reset();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("recorderSink  Reset err:0x%x", ret);
        return ret;
    }
    ret = ResetConfig();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG(" ResetConfig err:0x%x", ret);
        return ret;
    }
    status_ = INITIALIZED;
    MEDIA_INFO_LOG("Reset Recorder SUCCESS");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::Release()
{
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t ret;
    if (status_ == RELEASED) {
        MEDIA_ERR_LOG("already Released");
        return ERR_ILLEGAL_STATE;
    }
    if (status_ == RECORDING ||
        status_ == PAUSED) {
        if ((ret = StopInternal(false)) != SUCCESS) {
            MEDIA_ERR_LOG("Release StopInternal err");
            return ret;
        }
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    ret = recorderSink_->Release();
    if (ret != SUCCESS) {
        MEDIA_ERR_LOG("recorderSink_  Release failed Ret: %d", ret);
        return ret;
    }
    status_ = RELEASED;
    MEDIA_INFO_LOG("Recorder Released");
    return SUCCESS;
}

int32_t Recorder::RecorderImpl::SetFileSplitDuration(FileSplitType type, int64_t timestamp, uint32_t duration)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != RECORDING) {
        MEDIA_ERR_LOG(" SetFileSplitDuration ILLEGAL_STATE  status:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    MEDIA_INFO_LOG("Set File Split Duration type:%d", type);
    ManualSplitType manualSplitType;
    switch (type) {
        case FILE_SPLIT_POST:
            manualSplitType = MANUAL_SPLIT_POST;
            break;
        case FILE_SPLIT_PRE:
            manualSplitType = MANUAL_SPLIT_PRE;
            break;
        case FILE_SPLIT_NORMAL:
            manualSplitType = MANUAL_SPLIT_NORMAL;
            break;
        default:
            MEDIA_ERR_LOG("unsupport FileSplitType type: %d", type);
            return ERR_INVALID_PARAM;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    return recorderSink_->SetFileSplitDuration(manualSplitType, timestamp, duration);
}

int32_t Recorder::RecorderImpl::SetParameter(int32_t sourceId, const Format &format)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == RELEASED) {
        MEDIA_ERR_LOG("when RELEASED can not Set Parameter:%u", status_);
        return ERR_ILLEGAL_STATE;
    }
    CHECK_MEMBER_PTR_RETURN(recorderSink_);
    float value;
    if (format.GetFloatValue(RECORDER_RECORD_SPEED, value)) {
        MEDIA_INFO_LOG("SetParameter RECORDER_RECORD_SPEED value = %f", value);
        sourceManager_[sourceId].videoSourceConfig.speed = value;
        return SUCCESS;
    }
    return recorderSink_->SetParameter(sourceId, format);
}
}  // namespace Media
}  // namespace OHOS


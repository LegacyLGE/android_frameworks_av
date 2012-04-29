/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "SoftAAC2"
#include <utils/Log.h>

#include "SoftAAC2.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>

#define FILEREAD_MAX_LAYERS 2

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SoftAAC2::SoftAAC2(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mAACDecoder(NULL),
      mStreamInfo(NULL),
      mIsADTS(false),
      mInputBufferCount(0),
      mSignalledError(false),
      mInputDiscontinuity(false),
      mAnchorTimeUs(0),
      mNumSamplesOutput(0),
      mOutputPortSettingsChange(NONE) {
    initPorts();
    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftAAC2::~SoftAAC2() {
    aacDecoder_Close(mAACDecoder);
}

void SoftAAC2::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 8192;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    def.format.audio.cMIMEType = const_cast<char *>("audio/aac");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 8192 * 2;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.audio.cMIMEType = const_cast<char *>("audio/raw");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    addPort(def);
}

status_t SoftAAC2::initDecoder() {
    status_t status = UNKNOWN_ERROR;
    mAACDecoder = aacDecoder_Open(TT_MP4_RAW, /* num layers */ 1);
    if (mAACDecoder != NULL) {
        mStreamInfo = aacDecoder_GetStreamInfo(mAACDecoder);
        if (mStreamInfo != NULL) {
            status = OK;
        }
    }
    mIsFirst = true;
    return status;
}

OMX_ERRORTYPE SoftAAC2::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamAudioAac:
        {
            OMX_AUDIO_PARAM_AACPROFILETYPE *aacParams =
                (OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (aacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            aacParams->nBitRate = 0;
            aacParams->nAudioBandWidth = 0;
            aacParams->nAACtools = 0;
            aacParams->nAACERtools = 0;
            aacParams->eAACProfile = OMX_AUDIO_AACObjectMain;

            aacParams->eAACStreamFormat =
                mIsADTS
                    ? OMX_AUDIO_AACStreamFormatMP4ADTS
                    : OMX_AUDIO_AACStreamFormatMP4FF;

            aacParams->eChannelMode = OMX_AUDIO_ChannelModeStereo;

            if (!isConfigured()) {
                aacParams->nChannels = 1;
                aacParams->nSampleRate = 44100;
                aacParams->nFrameLength = 0;
            } else {
                aacParams->nChannels = mStreamInfo->numChannels;
                aacParams->nSampleRate = mStreamInfo->sampleRate;
                aacParams->nFrameLength = mStreamInfo->frameSize;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm:
        {
            OMX_AUDIO_PARAM_PCMMODETYPE *pcmParams =
                (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (pcmParams->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            pcmParams->eNumData = OMX_NumericalDataSigned;
            pcmParams->eEndian = OMX_EndianBig;
            pcmParams->bInterleaved = OMX_TRUE;
            pcmParams->nBitPerSample = 16;
            pcmParams->ePCMMode = OMX_AUDIO_PCMModeLinear;
            pcmParams->eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcmParams->eChannelMapping[1] = OMX_AUDIO_ChannelRF;
            pcmParams->eChannelMapping[2] = OMX_AUDIO_ChannelCF;
            pcmParams->eChannelMapping[3] = OMX_AUDIO_ChannelLFE;
            pcmParams->eChannelMapping[4] = OMX_AUDIO_ChannelLS;
            pcmParams->eChannelMapping[5] = OMX_AUDIO_ChannelRS;

            if (!isConfigured()) {
                pcmParams->nChannels = 1;
                pcmParams->nSamplingRate = 44100;
            } else {
                pcmParams->nChannels = mStreamInfo->numChannels;
                pcmParams->nSamplingRate = mStreamInfo->sampleRate;
            }

            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }

}

OMX_ERRORTYPE SoftAAC2::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;

            if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.aac",
                        OMX_MAX_STRINGNAME_SIZE - 1)) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAac:
        {
            const OMX_AUDIO_PARAM_AACPROFILETYPE *aacParams =
                (const OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (aacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            if (aacParams->eAACStreamFormat == OMX_AUDIO_AACStreamFormatMP4FF) {
                mIsADTS = false;
            } else if (aacParams->eAACStreamFormat
                        == OMX_AUDIO_AACStreamFormatMP4ADTS) {
                mIsADTS = true;
            } else {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioPcm:
        {
            const OMX_AUDIO_PARAM_PCMMODETYPE *pcmParams =
                (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (pcmParams->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

bool SoftAAC2::isConfigured() const {
    return mInputBufferCount > 0;
}

void SoftAAC2::onQueueFilled(OMX_U32 portIndex) {
    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    UCHAR* inBuffer[FILEREAD_MAX_LAYERS];
    UINT inBufferLength[FILEREAD_MAX_LAYERS] = {0};
    UINT bytesValid[FILEREAD_MAX_LAYERS] = {0};

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);

    if (portIndex == 0 && mInputBufferCount == 0) {
        ++mInputBufferCount;
        BufferInfo *info = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *header = info->mHeader;

        inBuffer[0] = header->pBuffer + header->nOffset;
        inBufferLength[0] = header->nFilledLen;

        AAC_DECODER_ERROR decoderErr =
            aacDecoder_ConfigRaw(mAACDecoder,
                                 inBuffer,
                                 inBufferLength);

        if (decoderErr != AAC_DEC_OK) {
            mSignalledError = true;
            notify(OMX_EventError, OMX_ErrorUndefined, decoderErr, NULL);
            return;
        }
        inQueue.erase(inQueue.begin());
        info->mOwnedByUs = false;
        notifyEmptyBufferDone(header);

        notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
        mOutputPortSettingsChange = AWAITING_DISABLED;
        return;
    }

    while (!inQueue.empty() && !outQueue.empty()) {
        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

        BufferInfo *outInfo = *outQueue.begin();
        OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);

            // flush out the decoder's delayed data by calling DecodeFrame one more time, with
            // the AACDEC_FLUSH flag set
            INT_PCM *outBuffer =
                    reinterpret_cast<INT_PCM *>(outHeader->pBuffer + outHeader->nOffset);
            AAC_DECODER_ERROR decoderErr = aacDecoder_DecodeFrame(mAACDecoder,
                                                                  outBuffer,
                                                                  outHeader->nAllocLen,
                                                                  AACDEC_FLUSH);
            if (decoderErr != AAC_DEC_OK) {
                mSignalledError = true;
                notify(OMX_EventError, OMX_ErrorUndefined, decoderErr, NULL);
                return;
            }

            outHeader->nFilledLen =
                    mStreamInfo->frameSize * sizeof(int16_t) * mStreamInfo->numChannels;
            outHeader->nFlags = OMX_BUFFERFLAG_EOS;

            outQueue.erase(outQueue.begin());
            outInfo->mOwnedByUs = false;
            notifyFillBufferDone(outHeader);
            return;
        }

        if (inHeader->nOffset == 0) {
            mAnchorTimeUs = inHeader->nTimeStamp;
            mNumSamplesOutput = 0;
        }

        size_t adtsHeaderSize = 0;
        if (mIsADTS) {
            // skip 30 bits, aac_frame_length follows.
            // ssssssss ssssiiip ppffffPc ccohCCll llllllll lll?????

            const uint8_t *adtsHeader = inHeader->pBuffer + inHeader->nOffset;

            CHECK_GE(inHeader->nFilledLen, 7);

            bool protectionAbsent = (adtsHeader[1] & 1);

            unsigned aac_frame_length =
                ((adtsHeader[3] & 3) << 11)
                | (adtsHeader[4] << 3)
                | (adtsHeader[5] >> 5);

            CHECK_GE(inHeader->nFilledLen, aac_frame_length);

            adtsHeaderSize = (protectionAbsent ? 7 : 9);

            inBuffer[0] = (UCHAR *)adtsHeader + adtsHeaderSize;
            inBufferLength[0] = aac_frame_length - adtsHeaderSize;

            inHeader->nOffset += adtsHeaderSize;
            inHeader->nFilledLen -= adtsHeaderSize;
        } else {
            inBuffer[0] = inHeader->pBuffer + inHeader->nOffset;
            inBufferLength[0] = inHeader->nFilledLen;
        }

        // Fill and decode
        INT_PCM *outBuffer = reinterpret_cast<INT_PCM *>(outHeader->pBuffer + outHeader->nOffset);
        bytesValid[0] = inBufferLength[0];

        int flags = mInputDiscontinuity ? AACDEC_INTR : 0;
        int prevSampleRate = mStreamInfo->sampleRate;
        int prevNumChannels = mStreamInfo->numChannels;

        AAC_DECODER_ERROR decoderErr = AAC_DEC_NOT_ENOUGH_BITS;
        while (bytesValid[0] > 0 && decoderErr == AAC_DEC_NOT_ENOUGH_BITS) {
            aacDecoder_Fill(mAACDecoder,
                            inBuffer,
                            inBufferLength,
                            bytesValid);

            decoderErr = aacDecoder_DecodeFrame(mAACDecoder,
                                                outBuffer,
                                                outHeader->nAllocLen,
                                                flags);

        }
        mInputDiscontinuity = false;

        /*
         * AAC+/eAAC+ streams can be signalled in two ways: either explicitly
         * or implicitly, according to MPEG4 spec. AAC+/eAAC+ is a dual
         * rate system and the sampling rate in the final output is actually
         * doubled compared with the core AAC decoder sampling rate.
         *
         * Explicit signalling is done by explicitly defining SBR audio object
         * type in the bitstream. Implicit signalling is done by embedding
         * SBR content in AAC extension payload specific to SBR, and hence
         * requires an AAC decoder to perform pre-checks on actual audio frames.
         *
         * Thus, we could not say for sure whether a stream is
         * AAC+/eAAC+ until the first data frame is decoded.
         */
        if (mInputBufferCount <= 2) {
            if (mStreamInfo->sampleRate != prevSampleRate ||
                mStreamInfo->numChannels != prevNumChannels) {
                // We're going to want to revisit this input buffer, but
                // may have already advanced the offset. Undo that if
                // necessary.
                inHeader->nOffset -= adtsHeaderSize;
                inHeader->nFilledLen += adtsHeaderSize;

                notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
                mOutputPortSettingsChange = AWAITING_DISABLED;
                return;
            }
        }

        size_t numOutBytes =
            mStreamInfo->frameSize * sizeof(int16_t) * mStreamInfo->numChannels;

        if (decoderErr == AAC_DEC_OK) {
            UINT inBufferUsedLength = inBufferLength[0] - bytesValid[0];
            inHeader->nFilledLen -= inBufferUsedLength;
            inHeader->nOffset += inBufferUsedLength;
        } else {
            ALOGW("AAC decoder returned error %d, substituting silence",
                  decoderErr);

            memset(outHeader->pBuffer + outHeader->nOffset, 0, numOutBytes);

            // Discard input buffer.
            inHeader->nFilledLen = 0;

            // fall through
        }

        if (decoderErr == AAC_DEC_OK || mNumSamplesOutput > 0) {
            // We'll only output data if we successfully decoded it or
            // we've previously decoded valid data, in the latter case
            // (decode failed) we'll output a silent frame.
            if (mIsFirst) {
                mIsFirst = false;
                // the first decoded frame should be discarded to account for decoder delay
                numOutBytes = 0;
            }

            outHeader->nFilledLen = numOutBytes;
            outHeader->nFlags = 0;

            outHeader->nTimeStamp =
                mAnchorTimeUs
                    + (mNumSamplesOutput * 1000000ll) / mStreamInfo->sampleRate;

            mNumSamplesOutput += mStreamInfo->frameSize;

            outInfo->mOwnedByUs = false;
            outQueue.erase(outQueue.begin());
            outInfo = NULL;
            notifyFillBufferDone(outHeader);
            outHeader = NULL;
        }

        if (inHeader->nFilledLen == 0) {
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;
        }

        if (decoderErr == AAC_DEC_OK) {
            ++mInputBufferCount;
        }
    }
}

void SoftAAC2::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == 0) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        mInputDiscontinuity = true;
        mIsFirst = true;
    }
}

void SoftAAC2::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    if (portIndex != 1) {
        return;
    }

    switch (mOutputPortSettingsChange) {
        case NONE:
            break;

        case AWAITING_DISABLED:
        {
            CHECK(!enabled);
            mOutputPortSettingsChange = AWAITING_ENABLED;
            break;
        }

        default:
        {
            CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
            CHECK(enabled);
            mOutputPortSettingsChange = NONE;
            break;
        }
    }
}

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SoftAAC2(name, callbacks, appData, component);
}

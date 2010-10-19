/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioHardwareALSA"
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include <alsa/asoundlib.h>
#include "AudioHardwareALSA.h"
// #define READ_FRAME_SIZE 2080
// #define READ_FRAME_SIZE_STANDARD 4160

#include <dlfcn.h>

#define SND_MIXER_VOL_RANGE_MIN  (0)
#define SND_MIXER_VOL_RANGE_MAX  (100)

#define ALSA_NAME_MAX 128

#define ALSA_STRCAT(x,y) \
    if (strlen(x) + strlen(y) < ALSA_NAME_MAX) \
        strcat(x, y);

extern "C"
{
    extern int ffs(int i);

    //
    // Make sure this prototype is consistent with what's in
    // external/libasound/alsa-lib-1.0.16/src/pcm/pcm_null.c!
    //
    extern int snd_pcm_null_open(snd_pcm_t **pcmp,
                                 const char *name,
                                 snd_pcm_stream_t stream,
                                 int mode);

    //
    // Function for dlsym() to look up for creating a new AudioHardwareInterface.
    //
    android::AudioHardwareInterface *createAudioHardware(void) {
        return new android::AudioHardwareALSA();
    }
}         // extern "C"

namespace android
{

typedef AudioSystem::audio_devices audio_routes;
#define ROUTE_ALL            AudioSystem::DEVICE_OUT_ALL
#define ROUTE_EARPIECE       AudioSystem::DEVICE_OUT_EARPIECE
#define ROUTE_HEADSET        AudioSystem::DEVICE_OUT_WIRED_HEADSET
#define ROUTE_HEADPHONE      AudioSystem::DEVICE_OUT_WIRED_HEADPHONE
#define ROUTE_SPEAKER        AudioSystem::DEVICE_OUT_SPEAKER
#define ROUTE_BLUETOOTH_SCO  AudioSystem::DEVICE_OUT_BLUETOOTH_SCO
#define ROUTE_BLUETOOTH_SCO_HEADSET  AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET
#define ROUTE_BLUETOOTH_SCO_CARKIT  AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT
#define ROUTE_BLUETOOTH_A2DP AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP
#define ROUTE_BLUETOOTH_A2DP_HEADPHONES AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES
#define ROUTE_BLUETOOTH_A2DP_SPEAKER AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER

// ----------------------------------------------------------------------------


static const char _nullALSADeviceName[] = "NULL_Device";

static void ALSAErrorHandler(const char *file,
                             int line,
                             const char *function,
                             int err,
                             const char *fmt,
                             ...)
{
    char buf[BUFSIZ];
    va_list arg;
    int l;

    va_start(arg, fmt);
    l = snprintf(buf, BUFSIZ, "%s:%i:(%s) ", file, line, function);
    vsnprintf(buf + l, BUFSIZ - l, fmt, arg);
    buf[BUFSIZ-1] = '\0';
    LOGE("ALSALib %s.", buf);
    va_end(arg);
}

// ----------------------------------------------------------------------------

/* The following table(s) need to match in order of the route bits
 */
static const char *deviceSuffix[] = {
    // output devices
    /* ROUTE_EARPIECE       */ "_Earpiece",
    /* ROUTE_SPEAKER        */ "_Speaker",
    /* ROUTE_HEADSET        */ "_Headset",
    /* ROUTE_HEADPHONE      */ "_Headset",
    /* ROUTE_BLUETOOTH_SCO  */ "_Bluetooth",
    /* ROUTE_BLUETOOTH_SCO_HEADSET */ "_Bluetooth",
    /* ROUTE_BLUETOOTH_SCO_CARKIT  */ "_Bluetooth", //"_Bluetooth_Carkit"
    /* ROUTE_BLUETOOTH_A2DP        */ "_Bluetooth", //"_Bluetooth-A2DP"
    /* ROUTE_BLUETOOTH_A2DP_HEADPHONES */ "_Bluetooth", //"_Bluetooth-A2DP_HeadPhone"
    /* ROUTE_BLUETOOTH_A2DP_SPEAKER    */ "_Bluetooth",     // "_Bluetooth-A2DP_Speaker"
    /* ROUTE_AUX_DIGITAL */ "_AuxDigital",
    /* ROUTE_TV_OUT */ "_TvOut",
    /* ROUTE_AUX_DIGITAL */ "_ExtraDockSpeaker",
    /* ROUTE_NULL */ "_Null",
    /* ROUTE_NULL */ "_Null",
    /* ROUTE_DEFAULT */ "_OutDefault",

    // input devices
    /* ROUTE_COMMUNICATION         */ "_Communication",
    /* ROUTE_AMBIENT               */ "_Ambient",
    /* ROUTE_BUILTIN_MIC           */ "_Speaker",
    /* ROUTE_BLUETOOTH_SCO_HEADSET */ "_Bluetooth",
    /* ROUTE_WIRED_HEADSET         */ "_Headset",
    /* ROUTE_AUX_DIGITAL           */ "_AuxDigital",
    /* ROUTE_VOICE_CALL            */ "_VoiceCall",
    /* ROUTE_BACK_MIC              */ "_BackMic",
    /* ROUTE_IN_DEFAULT            */ "_InDefault",
};

static const int deviceSuffixLen = (sizeof(deviceSuffix) / sizeof(char *));

struct mixer_info_t;

struct alsa_properties_t
{
    const audio_routes  routes;
    const char         *propName;
    const char         *propDefault;
    mixer_info_t       *mInfo;
};

static alsa_properties_t masterPlaybackProp = {
    ROUTE_ALL, "alsa.mixer.playback.master", "PCM", NULL
};

static alsa_properties_t masterCaptureProp = {
    ROUTE_ALL, "alsa.mixer.capture.master", "Capture", NULL
};

static alsa_properties_t
mixerMasterProp[SND_PCM_STREAM_LAST+1] = {
    { ROUTE_ALL, "alsa.mixer.playback.master",  "PCM",     NULL},
    { ROUTE_ALL, "alsa.mixer.capture.master",   "Capture", NULL}
};

static alsa_properties_t
mixerProp[][SND_PCM_STREAM_LAST+1] = {
    {
        {ROUTE_EARPIECE,       "alsa.mixer.playback.earpiece",       "Earpiece", NULL},
        {ROUTE_EARPIECE,       "alsa.mixer.capture.earpiece",        "Capture",  NULL}
    },
    {
        {ROUTE_SPEAKER,        "alsa.mixer.playback.speaker",        "Speaker", NULL},
        {ROUTE_SPEAKER,        "alsa.mixer.capture.speaker",         "",        NULL}
    },
    {
        {ROUTE_BLUETOOTH_SCO,  "alsa.mixer.playback.bluetooth.sco",  "Bluetooth",         NULL},
        {ROUTE_BLUETOOTH_SCO,  "alsa.mixer.capture.bluetooth.sco",   "Bluetooth Capture", NULL}
    },
    {
        {ROUTE_HEADSET,        "alsa.mixer.playback.headset",        "Headphone", NULL},
        {ROUTE_HEADSET,        "alsa.mixer.capture.headset",         "Capture",   NULL}
    },
    {
        {ROUTE_BLUETOOTH_A2DP, "alsa.mixer.playback.bluetooth.a2dp", "Bluetooth A2DP",         NULL},
        {ROUTE_BLUETOOTH_A2DP, "alsa.mixer.capture.bluetooth.a2dp",  "Bluetooth A2DP Capture", NULL}
    },
    {
        {static_cast<audio_routes>(0), NULL, NULL, NULL},
        {static_cast<audio_routes>(0), NULL, NULL, NULL}
    }
};

const uint32_t AudioHardwareALSA::inputSamplingRates[] = {
        8000, 11025, 16000, 22050, 44100
};

// ----------------------------------------------------------------------------

AudioHardwareALSA::AudioHardwareALSA() :
    mOutput(0),
    mInput(0),
    mSecRilLibHandle(NULL),
    mRilClient(0),
    mVrModeEnabled(false),
    mActivatedCP(false),
    mBluetoothECOff(false)
{
    snd_lib_error_set_handler(&ALSAErrorHandler);
    mMixer = new ALSAMixer;

    loadRILD();
}

AudioHardwareALSA::~AudioHardwareALSA()
{
    if (mOutput) delete mOutput;
    if (mInput) delete mInput;
    if (mMixer) delete mMixer;

    if (mSecRilLibHandle) {
        if (disconnectRILD(mRilClient) != RIL_CLIENT_ERR_SUCCESS)
            LOGE("Disconnect_RILD() error");

        if (closeClientRILD(mRilClient) != RIL_CLIENT_ERR_SUCCESS)
            LOGE("CloseClient_RILD() error");

        mRilClient = 0;

        dlclose(mSecRilLibHandle);
        mSecRilLibHandle = NULL;
    }
}


void AudioHardwareALSA::loadRILD(void)
{
    mSecRilLibHandle = dlopen("libsecril-client.so", RTLD_NOW);

    if (mSecRilLibHandle) {
        LOGV("libsecril-client.so is loaded");

        openClientRILD   = (HRilClient (*)(void))
                              dlsym(mSecRilLibHandle, "OpenClient_RILD");
        disconnectRILD   = (int (*)(HRilClient))
                              dlsym(mSecRilLibHandle, "Disconnect_RILD");
        closeClientRILD  = (int (*)(HRilClient))
                              dlsym(mSecRilLibHandle, "CloseClient_RILD");
        isConnectedRILD  = (int (*)(HRilClient))
                              dlsym(mSecRilLibHandle, "isConnected_RILD");
        connectRILD      = (int (*)(HRilClient))
                              dlsym(mSecRilLibHandle, "Connect_RILD");
        setCallVolume    = (int (*)(HRilClient, SoundType, int))
                              dlsym(mSecRilLibHandle, "SetCallVolume");
        setCallAudioPath = (int (*)(HRilClient, AudioPath))
                              dlsym(mSecRilLibHandle, "SetCallAudioPath");
        setCallClockSync = (int (*)(HRilClient, SoundClockCondition))
                              dlsym(mSecRilLibHandle, "SetCallClockSync");

        if (!openClientRILD  || !disconnectRILD   || !closeClientRILD ||
            !isConnectedRILD || !connectRILD      ||
            !setCallVolume   || !setCallAudioPath || !setCallClockSync) {
            LOGE("Can't load all functions from libsecril-client.so");

            dlclose(mSecRilLibHandle);
            mSecRilLibHandle = NULL;
        } else {
            mRilClient = openClientRILD();
            if (!mRilClient) {
                LOGE("OpenClient_RILD() error");

                dlclose(mSecRilLibHandle);
                mSecRilLibHandle = NULL;
            }
        }
    } else {
        LOGE("Can't load libsecril-client.so");
    }
}


status_t AudioHardwareALSA::initCheck()
{
    if (mMixer && mMixer->isValid())
        return NO_ERROR;
    else
        return NO_INIT;
}


status_t AudioHardwareALSA::connectRILDIfRequired(void)
{
    if (!mSecRilLibHandle) {
        LOGE("connectIfRequired() lib is not loaded");
        return INVALID_OPERATION;
    }

    if (isConnectedRILD(mRilClient)) {
        return OK;
    }

    if (connectRILD(mRilClient) != RIL_CLIENT_ERR_SUCCESS) {
        LOGE("Connect_RILD() error");
        return INVALID_OPERATION;
    }

    return OK;
}


status_t AudioHardwareALSA::setVoiceVolume(float volume)
{
    LOGI("### setVoiceVolume");

    AutoMutex lock(mLock);
    // sangsu fix : transmic volume level IPC to modem
    if ( (AudioSystem::MODE_IN_CALL == mMode) && (mSecRilLibHandle) &&
         (connectRILDIfRequired() == OK) ) {

        uint32_t routes = AudioSystem::ROUTE_EARPIECE;
        if (mOutput != NULL) {
            routes = mOutput->device();
        }
        int int_volume = (int)(volume * 5);

        LOGI("### route(%d) call volume(%f)", routes, volume);
        switch (routes) {
            case AudioSystem::ROUTE_EARPIECE:
            case AudioSystem::ROUTE_HEADPHONE: // Use receive path with 3 pole headset.
                LOGI("### earpiece call volume");
                setCallVolume(mRilClient, SOUND_TYPE_VOICE, int_volume);
                break;

            case AudioSystem::ROUTE_SPEAKER:
                LOGI("### speaker call volume");
                setCallVolume(mRilClient, SOUND_TYPE_SPEAKER, int_volume);
                break;

            case AudioSystem::ROUTE_BLUETOOTH_SCO:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            case AudioSystem::ROUTE_BLUETOOTH_A2DP:
                LOGI("### bluetooth call volume");
                setCallVolume(mRilClient, SOUND_TYPE_BTVOICE, int_volume);
                break;

            case AudioSystem::ROUTE_HEADSET:
                LOGI("### headset call volume");
                setCallVolume(mRilClient, SOUND_TYPE_HEADSET, int_volume);
                break;

            default:
                LOGE("### Call volume setting error!!!0x%08x \n", routes);
                break;
        }
    }
    // sangsu fix end

    // The voice volume is used by the VOICE_CALL audio stream.
    if (mMixer)
        return mMixer->setVolume(ROUTE_EARPIECE, volume);
    else
        return INVALID_OPERATION;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    if (mMixer)
        return mMixer->setMasterVolume(volume);
    else
        return INVALID_OPERATION;
}

AudioStreamOut *
AudioHardwareALSA::openOutputStream(
                        uint32_t devices,
                        int *format,
                        uint32_t *channels,
                        uint32_t *sampleRate,
                        status_t *status)
{
    AudioStreamOutALSA *out = NULL;
    status_t  ret = NO_ERROR;
    {
        AutoMutex lock(mLock);

        // only one output stream allowed
        if (mOutput) {
            ret = ALREADY_EXISTS;
            goto exit;
        }

        LOGV("[[[[[[[[\n%s - format = %d, channels = %d, sampleRate = %d, devices = %d]]]]]]]]\n", __func__, *format, *channels, *sampleRate,devices);

        out = new AudioStreamOutALSA(this);

        ret = out->set(format, channels, sampleRate);

        if (ret == NO_ERROR) {
            mOutput = out;
        }
    }
exit:
    if (ret == NO_ERROR) {
        // Some information is expected to be available immediately after
        // the device is open.
        /* Tushar - Sets the current device output here - we may set device here */
        LOGI("%s] Setting ALSA device.", __func__);
        mOutput->setDevice(mMode, devices, PLAYBACK); /* tushar - Enable all devices as of now */
    } else if (out) {
        delete out;
    }
    if (status) {
        *status = ret;
    }
    return mOutput;
}

void
AudioHardwareALSA::closeOutputStream(AudioStreamOut* out)
{
    /* TODO:Tushar: May lead to segmentation fault - check*/
    //delete out;
    {
        AutoMutex lock(mLock);

        if (mOutput == 0 || mOutput != out) {
            LOGW("Attempt to close invalid output stream");
            return;
        }
        mOutput = 0;
    }
    delete out;
}


AudioStreamIn*
AudioHardwareALSA::openInputStream(
                                uint32_t devices,
                                int *format,
                                uint32_t *channels,
                                uint32_t *sampleRate,
                                status_t *status,
                                AudioSystem::audio_in_acoustics acoustics)
{
    AudioStreamInALSA *in = NULL;
    status_t ret = NO_ERROR;
    {
        AutoMutex lock(mLock);

        // only one input stream allowed
        if (mInput) {
            ret = ALREADY_EXISTS;
            goto exit;
        }

        in = new AudioStreamInALSA(this);

        ret = in->set(format, channels, sampleRate);
        if (ret == NO_ERROR) {
            mInput = in;
        }
    }
exit:
    if (ret == NO_ERROR) {
        // Some information is expected to be available immediately after
        // the device is open.
        mInput->setDevice(mMode, devices, CAPTURE);  /* Tushar - as per modified arch */
    } else if (in != NULL) {
        delete in;
    }
    if (status) {
        *status = ret;
    }
    return mInput;
}

void
AudioHardwareALSA::closeInputStream(AudioStreamIn* in)
{
    /* TODO:Tushar: May lead to segmentation fault - check*/
    //delete in;
    {
        AutoMutex lock(mLock);

        if (mInput == 0 || mInput != in) {
            LOGW("Attempt to close invalid input stream");
            return;
        } else {
            mInput = 0;
        }
    }
    delete in;
}


status_t AudioHardwareALSA::doRouting(uint32_t device, bool force)
{
    AutoMutex lock(mLock);
    return doRouting_l(device, force);
}

status_t AudioHardwareALSA::doRouting_l(uint32_t device, bool force)
{
    status_t ret;
    int mode = mMode;        // Prevent to changing mode on setup sequence.

    LOGV("doRouting: device %x, force %d", device, force);

    if (mOutput) {
        //device = 0; /* Tushar - temp implementation */
        if (device == AudioSystem::DEVICE_OUT_DEFAULT) {
            device = mOutput->device();
        }

        // Setup sound path for CP clocking
        if ( (AudioSystem::MODE_IN_CALL == mode) && (mSecRilLibHandle) &&
             (connectRILDIfRequired() == OK) ) {

            LOGI("### incall mode route (%d)", device);

            switch(device){
                case AudioSystem::ROUTE_EARPIECE:
                    LOGI("### incall mode earpiece route");
                    setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_HANDSET);
                    break;

                case AudioSystem::ROUTE_SPEAKER:
                    LOGI("### incall mode speaker route");
                    setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_SPEAKER);
                    break;

                case AudioSystem::ROUTE_BLUETOOTH_SCO:
                case AudioSystem::ROUTE_BLUETOOTH_SCO_HEADSET:
                case AudioSystem::ROUTE_BLUETOOTH_SCO_CARKIT:
                    LOGI("### incall mode bluetooth route %s NR", mBluetoothECOff ? "NO" : "");
                    if (mBluetoothECOff)
                        setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_BLUETOOTH_NO_NR);
                    else
                        setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_BLUETOOTH);
                    break;

                case AudioSystem::ROUTE_HEADSET :
                case AudioSystem::ROUTE_HEADPHONE :
                    LOGI("### incall mode headset route");
                    setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_HEADSET);
                    break;

                case AudioSystem::ROUTE_BLUETOOTH_A2DP:
                    LOGI("### incall mode bluetooth route");
                    setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_BLUETOOTH);
                    break;

                default:
                    LOGE("### incall mode Error!! route = [%d]", device);
                    break;
            }
        }

        ret = mOutput->setDevice(mode, device, PLAYBACK, force);

        return ret;
    }

    return NO_INIT;
}


status_t AudioHardwareALSA::setMicMute(bool state)
{
    if (mMixer)
        return mMixer->setCaptureMuteState(ROUTE_EARPIECE, state);

    return NO_INIT;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    if (mMixer)
        return mMixer->getCaptureMuteState(ROUTE_EARPIECE, state);

    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}


uint32_t AudioHardwareALSA::bufferRatio(uint32_t samplingRate) {
    switch (samplingRate) {
    case 8000:
    case 11025:
        return 4;
    case 16000:
    case 22050:
        return 2;
    case 44100:
    default:
        break;
    }
    return 1;
}


size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    if (sampleRate != 8000 && sampleRate != 11025 && sampleRate != 16000 &&
            sampleRate != 22050 && sampleRate != 44100) {
        LOGW("getInputBufferSize bad sampling rate: %d", sampleRate);
        return 0;
    }
    if (format != AudioSystem::PCM_16_BIT) {
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }
    if (channelCount != 1) {
        LOGW("getInputBufferSize bad channel count: %d", channelCount);
        return 0;
    }

    size_t size = (PERIOD_SZ_CAPTURE / bufferRatio(sampleRate)) * sizeof(int16_t);
    LOGV("getInputBufferSize() rate %d, ratio %d", sampleRate, size);
    return size;

}

uint32_t AudioHardwareALSA::checkInputSampleRate(uint32_t sampleRate)
{
    uint32_t i;
    uint32_t prevDelta;
    uint32_t delta;

    for (i = 0, prevDelta = 0xFFFFFFFF; i < sizeof(inputSamplingRates)/sizeof(uint32_t); i++, prevDelta = delta) {
        delta = abs(sampleRate - inputSamplingRates[i]);
        if (delta > prevDelta) break;
    }
    // i is always > 0 here
    return inputSamplingRates[i-1];
}

status_t AudioHardwareALSA::setMode(int mode)
{
    AutoMutex lock(mLock);
    int prevMode = mMode;
    status_t status = AudioHardwareBase::setMode(mode);
    LOGV("setMode() : new %d, old %d", mMode, prevMode);
    if (status == NO_ERROR) {
        if ( (mMode == AudioSystem::MODE_RINGTONE) || (mMode == AudioSystem::MODE_IN_CALL) )
        {
            if ( (!mActivatedCP) && (mSecRilLibHandle) && (connectRILDIfRequired() == OK) ) {
                setCallClockSync(mRilClient, SOUND_CLOCK_START);
                mActivatedCP = true;
            }
        }

        // make sure that doAudioRouteOrMute() is called by doRouting()
        // when entering or exiting in call mode even if the new device
        // selected is the same as current one.
        if ((prevMode != AudioSystem::MODE_IN_CALL) && (mMode == AudioSystem::MODE_IN_CALL)) {
            LOGV("setMode() entering call");
            doRouting_l(AudioSystem::DEVICE_OUT_DEFAULT, true);
            setVoiceRecordGain_l(false);
        }
        if ((prevMode == AudioSystem::MODE_IN_CALL) && (mMode != AudioSystem::MODE_IN_CALL)) {
            LOGV("setMode() exiting call");
            doRouting_l(AudioSystem::DEVICE_OUT_DEFAULT, true);
            if (mOutput != NULL && !mOutput->isActive()) {
                mOutput->close();
            }
        }

        if (mMode == AudioSystem::MODE_NORMAL) {
            if(mActivatedCP)
                mActivatedCP = false;
        }
    }

    return status;
}

int AudioHardwareALSA::setVoiceRecordGain(bool enable)
{
    AutoMutex lock(mLock);
    return setVoiceRecordGain_l(enable);
}

int AudioHardwareALSA::setVoiceRecordGain_l(bool enable)
{
     LOGI("[%s], enable=%d", __func__, enable);
     if (enable != mVrModeEnabled &&
         !(enable && (mMode == AudioSystem::MODE_IN_CALL))) {
         ALSAControl *alsaControl = new ALSAControl();
         status_t ret = alsaControl->set("Recognition Control", enable ? RECOGNITION_ON : RECOGNITION_OFF);
         delete alsaControl;
         mVrModeEnabled = enable;
     }

     return NO_ERROR;
}

status_t AudioHardwareALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 bt_nrec_key = String8("bt_headset_nrec");
    String8 value;

    LOGV("setParameters(%s)", keyValuePairs.string());

    if (param.get(bt_nrec_key, value) == NO_ERROR) {
        setBluetoothNrEcOnOff((value == "on") ? false : true);
    }

    return NO_ERROR;
}

void AudioHardwareALSA::setBluetoothNrEcOnOff(bool disable)
{
    LOGV("setBluetoothNrEcOnOff(%s)", disable ? "true" : "false");

    if (disable != mBluetoothECOff)
    {
        mBluetoothECOff = disable;

        if ( (mOutput)          && (AudioSystem::MODE_IN_CALL == mMode) &&
             (mSecRilLibHandle) && (connectRILDIfRequired() == OK)) {

            uint32_t device = mOutput->device();

            switch (device) {
                case AudioSystem::ROUTE_BLUETOOTH_SCO:
                case AudioSystem::ROUTE_BLUETOOTH_SCO_HEADSET:
                case AudioSystem::ROUTE_BLUETOOTH_SCO_CARKIT:
                    LOGV("### incall mode bluetooth EC %s route", mBluetoothECOff ? "OFF" : "ON");
                    if (mBluetoothECOff)
                        setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_BLUETOOTH_NO_NR);
                    else
                        setCallAudioPath(mRilClient, SOUND_AUDIO_PATH_BLUETOOTH);
                    break;

                default :
                    LOGE("Bluetooth path is not activated!!");
                    break;
            }
        }
    }
}


// ----------------------------------------------------------------------------

ALSAStreamOps::ALSAStreamOps() :
    mHandle(0),
    mHardwareParams(0),
    mSoftwareParams(0),
    mDevice(0)
{
    if (snd_pcm_hw_params_malloc(&mHardwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
    }

    if (snd_pcm_sw_params_malloc(&mSoftwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
    }
}

ALSAStreamOps::~ALSAStreamOps()
{
    AutoMutex lock(mLock);

    close();

    if (mHardwareParams)
        snd_pcm_hw_params_free(mHardwareParams);

    if (mSoftwareParams)
        snd_pcm_sw_params_free(mSoftwareParams);
}


status_t ALSAStreamOps::set(int      *pformat,
                            uint32_t *pchannels,
                            uint32_t *prate)
{
    int lformat = pformat ? *pformat : 0;
    unsigned int lchannels = pchannels ? *pchannels : 0;
    unsigned int lrate = prate ? *prate : 0;


    LOGD("ALSAStreamOps - input   - format = %d, channels = %d, rate = %d\n", lformat, lchannels, lrate);
    LOGD("ALSAStreamOps - default - format = %d, channelCount = %d, rate = %d\n", mDefaults->format, mDefaults->channelCount, mDefaults->sampleRate);

    if (lformat == 0) lformat = getAndroidFormat(mDefaults->format);//format();
    if (lchannels == 0) lchannels = getAndroidChannels(mDefaults->channelCount);// channelCount();
    if (lrate == 0) lrate = mDefaults->sampleRate;

    if ( (lformat != getAndroidFormat(mDefaults->format)) ||
          (lchannels != getAndroidChannels(mDefaults->channelCount)) ) {
        if (pformat)   *pformat = getAndroidFormat(mDefaults->format);
        if (pchannels) *pchannels = getAndroidChannels(mDefaults->channelCount);
        return BAD_VALUE;
    }
    if (mDefaults->direction == SND_PCM_STREAM_PLAYBACK) {
        if (lrate != mDefaults->sampleRate) {
            if (prate) *prate = mDefaults->sampleRate;
            return BAD_VALUE;
        }
    } else {
        uint32_t rate = AudioHardwareALSA::checkInputSampleRate(lrate);
        if (rate != lrate) {
            if (prate) *prate = rate;
            return BAD_VALUE;
        }
        lrate = rate;
    }
    mDefaults->bufferRatio = AudioHardwareALSA::bufferRatio(lrate);
    mDefaults->sampleRate = lrate;

    if(pformat)     *pformat = getAndroidFormat(mDefaults->format);
    if(pchannels)   *pchannels = getAndroidChannels(mDefaults->channelCount);
    if(prate)       *prate = mDefaults->sampleRate;

    return NO_ERROR;
}


uint32_t ALSAStreamOps::sampleRate() const
{
    return mDefaults->sampleRate;
}

status_t ALSAStreamOps::sampleRate(uint32_t rate)
{
    const char *stream;
    unsigned int requestedRate;
    int err;

    if (!mHandle)
        return NO_INIT;

    stream = streamName();
    requestedRate = rate;
    err = snd_pcm_hw_params_set_rate_near(mHandle,
                                          mHardwareParams,
                                          &requestedRate,
                                          0);

    if (err < 0) {
        LOGE("Unable to set %s sample rate to %u: %s",
            stream, rate, snd_strerror(err));
        return BAD_VALUE;
    }
    if (requestedRate != rate) {
        // Some devices have a fixed sample rate, and can not be changed.
        // This may cause resampling problems; i.e. PCM playback will be too
        // slow or fast.
        LOGW("Requested rate (%u HZ) does not match actual rate (%u HZ)",
            rate, requestedRate);
    }
    else {
        LOGD("Set %s sample rate to %u HZ", stream, requestedRate);
    }
    return NO_ERROR;
}

//
// Return the number of bytes (not frames)
//
size_t ALSAStreamOps::bufferSize() const
{
    int err;

    size_t size = ((mDefaults->periodSize / mDefaults->bufferRatio) * mDefaults->channelCount *
            snd_pcm_format_physical_width(mDefaults->format)) / 8;
    LOGV("bufferSize() channelCount %d, bufferRatio %d, size %d",
         mDefaults->channelCount, mDefaults->bufferRatio, size);
    return size;

}

int ALSAStreamOps::getAndroidFormat(snd_pcm_format_t format)
{
    int pcmFormatBitWidth;
    int audioSystemFormat;

    pcmFormatBitWidth = snd_pcm_format_physical_width(format);
    audioSystemFormat = AudioSystem::DEFAULT;
    switch(pcmFormatBitWidth) {
        case 8:
            audioSystemFormat = AudioSystem::PCM_8_BIT;
            break;

        case 16:
            audioSystemFormat = AudioSystem::PCM_16_BIT;
            break;

        default:
            LOG_FATAL("Unknown AudioSystem bit width %i!", pcmFormatBitWidth);
    }

    return audioSystemFormat;

}

int ALSAStreamOps::format() const
{
    snd_pcm_format_t ALSAFormat;
    int pcmFormatBitWidth;
    int audioSystemFormat;

    if (snd_pcm_hw_params_get_format(mHardwareParams, &ALSAFormat) < 0) {
        return -1;
    }

    pcmFormatBitWidth = snd_pcm_format_physical_width(ALSAFormat);
    audioSystemFormat = AudioSystem::DEFAULT;
    switch(pcmFormatBitWidth) {
        case 8:
            audioSystemFormat = AudioSystem::PCM_8_BIT;
            break;

        case 16:
            audioSystemFormat = AudioSystem::PCM_16_BIT;
            break;

        default:
            LOG_FATAL("Unknown AudioSystem bit width %i!", pcmFormatBitWidth);
    }

    return audioSystemFormat;
}

uint32_t ALSAStreamOps::getAndroidChannels(int channelCount) const
{
    int AudioSystemChannels = AudioSystem::DEFAULT;

    if (mDefaults->direction == SND_PCM_STREAM_PLAYBACK) {
        switch(channelCount){
        case 1:
            AudioSystemChannels = AudioSystem::CHANNEL_OUT_MONO;
            break;
        case 2:
            AudioSystemChannels = AudioSystem::CHANNEL_OUT_STEREO;
            break;
        case 4:
            AudioSystemChannels = AudioSystem::CHANNEL_OUT_QUAD;
            break;
        case 6:
            AudioSystemChannels = AudioSystem::CHANNEL_OUT_5POINT1;
            break;
        default:
            LOGE("FATAL: AudioSystem does not support %d output channels.", channelCount);
        }
    } else {
        switch(channelCount){
        case 1:
            AudioSystemChannels = AudioSystem::CHANNEL_IN_MONO;
            break;
        case 2:
            AudioSystemChannels = AudioSystem::CHANNEL_IN_STEREO;
            break;
        default:
            LOGE("FATAL: AudioSystem does not support %d input channels.", channelCount);
        }

    }
    return AudioSystemChannels;
}

uint32_t ALSAStreamOps::channels() const
{
    return getAndroidChannels(mDefaults->channelCount);
}

int ALSAStreamOps::channelCount() const
{
    return mDefaults->channelCount;
}

status_t ALSAStreamOps::channelCount(int channelCount) {
    int err;

    if (!mHandle)
        return NO_INIT;

    err = snd_pcm_hw_params_set_channels(mHandle, mHardwareParams, channelCount);
    if (err < 0) {
        LOGE("Unable to set channel count to %i: %s",
            channelCount, snd_strerror(err));
        return BAD_VALUE;
    }

    LOGD("Using %i %s for %s.",
         channelCount, channelCount == 1 ? "channel" : "channels", streamName());

    return NO_ERROR;
}

status_t ALSAStreamOps::open(int mode, uint32_t device)
{
    const char *stream = streamName();
    const char *devName = deviceName(mode, device);

    int         err;

    LOGI("Try to open ALSA %s device %s", stream, devName);

    for(;;) {
        // The PCM stream is opened in blocking mode, per ALSA defaults.  The
        // AudioFlinger seems to assume blocking mode too, so asynchronous mode
        // should not be used.
        err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
        if (err == 0) break;

        // See if there is a less specific name we can try.
        // Note: We are changing the contents of a const char * here.
        char *tail = strrchr(devName, '_');
        if (! tail) break;
        *tail = 0;
    }

    if (err < 0) {
        // None of the Android defined audio devices exist. Open a generic one.
        devName = "hw:00,1";    // 090507 SMDKC110 Froyo

        err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
        if (err < 0) {
            // Last resort is the NULL device (i.e. the bit bucket).
            devName = _nullALSADeviceName;
            err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
        }
    }

    mDevice = device;

    LOGI("Initialized ALSA %s device %s", stream, devName);
    return err;
}

void ALSAStreamOps::close()
{
    snd_pcm_t *handle = mHandle;
    mHandle = NULL;

    if (handle) {
        LOGV("ALSAStreamOps::close()");
        snd_pcm_drain(handle);
        snd_pcm_close(handle);
    }
}

status_t ALSAStreamOps::setSoftwareParams()
{
    if (!mHandle)
        return NO_INIT;

    int err;

    // Get the current software parameters
    err = snd_pcm_sw_params_current(mHandle, mSoftwareParams);
    if (err < 0) {
        LOGE("Unable to get software parameters: %s", snd_strerror(err));
        return NO_INIT;
    }

    snd_pcm_uframes_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;
    snd_pcm_uframes_t startThreshold;

    // Configure ALSA to start the transfer when the buffer is almost full.
    snd_pcm_get_params(mHandle, &bufferSize, &periodSize);
    LOGE("bufferSize %d, periodSize %d\n", (int)bufferSize, (int)periodSize);

    if (mDefaults->direction == SND_PCM_STREAM_PLAYBACK) {
        // For playback, configure ALSA to start the transfer when the
        // buffer is almost full.
        startThreshold = (bufferSize / periodSize) * periodSize;
        //startThreshold = 1;
    }
    else {
        // For recording, configure ALSA to start the transfer on the
        // first frame.
        startThreshold = 1;
    }

    err = snd_pcm_sw_params_set_start_threshold(mHandle,
        mSoftwareParams,
        startThreshold);
    if (err < 0) {
        LOGE("Unable to set start threshold to %lu frames: %s",
            startThreshold, snd_strerror(err));
        return NO_INIT;
    }

    // Stop the transfer when the buffer is full.
    err = snd_pcm_sw_params_set_stop_threshold(mHandle,
                                               mSoftwareParams,
                                               bufferSize);
    if (err < 0) {
        LOGE("Unable to set stop threshold to %lu frames: %s",
            bufferSize, snd_strerror(err));
        return NO_INIT;
    }

    // Allow the transfer to start when at least periodSize samples can be
    // processed.
    err = snd_pcm_sw_params_set_avail_min(mHandle,
                                          mSoftwareParams,
                                          periodSize);
    if (err < 0) {
        LOGE("Unable to configure available minimum to %lu: %s",
            periodSize, snd_strerror(err));
        return NO_INIT;
    }

    // Commit the software parameters back to the device.
    err = snd_pcm_sw_params(mHandle, mSoftwareParams);
    if (err < 0) {
        LOGE("Unable to configure software parameters: %s",
            snd_strerror(err));
        return NO_INIT;
    }

    return NO_ERROR;
}

status_t ALSAStreamOps::setPCMFormat(snd_pcm_format_t format)
{
    const char *formatDesc;
    const char *formatName;
    bool validFormat;
    int err;

    // snd_pcm_format_description() and snd_pcm_format_name() do not perform
    // proper bounds checking.
    validFormat = (static_cast<int>(format) > SND_PCM_FORMAT_UNKNOWN) &&
        (static_cast<int>(format) <= SND_PCM_FORMAT_LAST);
    formatDesc = validFormat ?
        snd_pcm_format_description(format) : "Invalid Format";
    formatName = validFormat ?
        snd_pcm_format_name(format) : "UNKNOWN";

    err = snd_pcm_hw_params_set_format(mHandle, mHardwareParams, format);
    if (err < 0) {
        LOGE("Unable to configure PCM format %s (%s): %s",
            formatName, formatDesc, snd_strerror(err));
        return NO_INIT;
    }

    LOGD("Set %s PCM format to %s (%s)", streamName(), formatName, formatDesc);
    return NO_ERROR;
}

status_t ALSAStreamOps::setHardwareResample(bool resample)
{
    int err;

    err = snd_pcm_hw_params_set_rate_resample(mHandle,
                                              mHardwareParams,
                                              static_cast<int>(resample));
    if (err < 0) {
        LOGE("Unable to %s hardware resampling: %s",
            resample ? "enable" : "disable",
            snd_strerror(err));
        return NO_INIT;
    }
    return NO_ERROR;
}

const char *ALSAStreamOps::streamName()
{
    // Don't use snd_pcm_stream(mHandle), as the PCM stream may not be
    // opened yet.  In such case, snd_pcm_stream() will abort().
    return snd_pcm_stream_name(mDefaults->direction);
}

//
// Set playback or capture PCM device.  It's possible to support audio output
// or input from multiple devices by using the ALSA plugins, but this is
// not supported for simplicity.
//
// The AudioHardwareALSA API does not allow one to set the input routing.
//
// If the "routes" value does not map to a valid device, the default playback
// device is used.
//
status_t ALSAStreamOps::setDevice(int mode, uint32_t device, uint audio_mode)
{
    // Close off previously opened device.
    // It would be nice to determine if the underlying device actually
    // changes, but we might be manipulating mixer settings (see asound.conf).
    //
    close();

    const char *stream = streamName();


    LOGD("\n------------------------>>>>>> ALSA OPEN mode %d,device %d \n",mode,device);

    status_t    status = open (mode, device);
    int     err;
    unsigned int period_val;

    if (status != NO_ERROR)
        return status;

    err = snd_pcm_hw_params_any(mHandle, mHardwareParams);
    if (err < 0) {
        LOGE("Unable to configure hardware: %s", snd_strerror(err));
        return NO_INIT;
    }

    status = setPCMFormat(mDefaults->format);

    // Set the interleaved read and write format.
    err = snd_pcm_hw_params_set_access(mHandle, mHardwareParams,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOGE("Unable to configure PCM read/write format: %s",
            snd_strerror(err));
        return NO_INIT;
    }


    //
    // Some devices do not have the default two channels.  Force an error to
    // prevent AudioMixer from crashing and taking the whole system down.
    //
    // Note that some devices will return an -EINVAL if the channel count
    // is queried before it has been set.  i.e. calling channelCount()
    // before channelCount(channels) may return -EINVAL.
    //
    status = channelCount(mDefaults->channelCount);
    if (status != NO_ERROR)
        return status;

    // Don't check for failure; some devices do not support the default
    // sample rate.
    // FIXME:: always use default sampling rate
    sampleRate(DEFAULT_SAMPLE_RATE);

    snd_pcm_uframes_t bufferSize = mDefaults->bufferSize;
    snd_pcm_uframes_t periodSize = mDefaults->periodSize;
    period_val = bufferSize/periodSize;

    unsigned int latency = mDefaults->latency;

    // Make sure we have at least the size we originally wanted
    err = snd_pcm_hw_params_set_buffer_size(mHandle, mHardwareParams, bufferSize);
    if (err < 0) {
        LOGE("Unable to set buffer size to %d:  %s",
             (int)bufferSize, snd_strerror(err));
        return NO_INIT;
    }

    err = snd_pcm_hw_params_set_period_size (mHandle, mHardwareParams, periodSize, NULL);
    if (err < 0) {
        LOGE("Unable to set the period size for latency: %s", snd_strerror(err));
        return NO_INIT;
    }

    err = snd_pcm_hw_params_get_period_size (mHardwareParams, &periodSize, NULL);
    if (err < 0) {
        LOGE("Unable to get the period size for latency: %s", snd_strerror(err));
        return NO_INIT;
    }

//    err = snd_pcm_hw_params_get_period_size (mHardwareParams, &periodSize, NULL);
//    if (err < 0) {
//        LOGE("Unable to get the period size for latency: %s", snd_strerror(err));
//        return NO_INIT;
//    }

//    // Setup buffers for latency
//    err = snd_pcm_hw_params_set_buffer_time_near (mHandle, mHardwareParams,
//                                                  &latency, NULL);
//    if(audio_mode == PLAYBACK) {
//        period_val = PERIODS_PLAYBACK;
//        if(snd_pcm_hw_params_set_periods(mHandle, mHardwareParams, period_val, 0) < 0)
//            LOGE("Fail to set period size %d for playback", period_val);
//    }
//    else
//        period_val = PERIODS_CAPTURE;
//
//    if (err < 0) {
//        LOGD("snd_pcm_hw_params_set_buffer_time_near() failed: %s", snd_strerror(err));
//        /* That didn't work, set the period instead */
//        unsigned int periodTime = latency / period_val;
//        err = snd_pcm_hw_params_set_period_time_near (mHandle, mHardwareParams,
//                                                      &periodTime, NULL);
//        if (err < 0) {
//            LOGE("Unable to set the period time for latency: %s", snd_strerror(err));
//            return NO_INIT;
//        }
//        err = snd_pcm_hw_params_get_period_size (mHardwareParams, &periodSize, NULL);
//        if (err < 0) {
//            LOGE("Unable to get the period size for latency: %s", snd_strerror(err));
//            return NO_INIT;
//        }
//        bufferSize = periodSize * period_val;
//        if (bufferSize < mDefaults->bufferSize)
//            bufferSize = mDefaults->bufferSize;
//        err = snd_pcm_hw_params_set_buffer_size_near (mHandle, mHardwareParams, &bufferSize);
//        if (err < 0) {
//            LOGE("Unable to set the buffer size for latency: %s", snd_strerror(err));
//            return NO_INIT;
//        }
//    } else {
//        LOGD("snd_pcm_hw_params_set_buffer_time_near() OK");
//        // OK, we got buffer time near what we expect. See what that did for bufferSize.
//        err = snd_pcm_hw_params_get_buffer_size (mHardwareParams, &bufferSize);
//        if (err < 0) {
//            LOGE("Unable to get the buffer size for latency: %s", snd_strerror(err));
//            return NO_INIT;
//        }
//        // Does set_buffer_time_near change the passed value? It should.
//        err = snd_pcm_hw_params_get_buffer_time (mHardwareParams, &latency, NULL);
//        if (err < 0) {
//            LOGE("Unable to get the buffer time for latency: %s", snd_strerror(err));
//            return NO_INIT;
//        }
//        LOGD("got latency %d for bufferSize %d", latency, bufferSize);
//        unsigned int periodTime = latency / period_val;
//        LOGD("got latency %d for bufferSize %d => periodTime %d", latency, bufferSize, periodTime);
//        err = snd_pcm_hw_params_set_period_time_near (mHandle, mHardwareParams,
//                                                      &periodTime, NULL);
//        if (err < 0) {
//            LOGE("Unable to set the period time for latency: %s", snd_strerror(err));
//            return NO_INIT;
//        }
//        err = snd_pcm_hw_params_get_period_size (mHardwareParams, &periodSize, NULL);
//        if (err < 0) {
//            LOGE("Unable to get the period size for latency: %s", snd_strerror(err));
//            return NO_INIT;
//        }
//    }

    LOGD("Buffer size: %d", (int)bufferSize);
    LOGD("Period size: %d", (int)periodSize);
    LOGD("Latency: %d", (int)latency);

    mDefaults->bufferSize = bufferSize;
    mDefaults->latency = latency;
    mDefaults->periodSize = periodSize;

    // Commit the hardware parameters back to the device.
    err = snd_pcm_hw_params(mHandle, mHardwareParams);
    if (err < 0) {
        LOGE("Unable to set hardware parameters: %s", snd_strerror(err));
        return NO_INIT;
    }

    status = setSoftwareParams();

    return status;
}

const char *ALSAStreamOps::deviceName(int mode, uint32_t device)
{
    static char devString[ALSA_NAME_MAX];
    int dev;
    int hasDevExt = 0;

    strcpy (devString, mDefaults->devicePrefix);

    for (dev=0; device; dev++)
        if (device & (1 << dev)) {
            /* Don't go past the end of our list */
            if (dev >= deviceSuffixLen)
                break;
            ALSA_STRCAT (devString, deviceSuffix[dev]);
            device &= ~(1 << dev);
            hasDevExt = 1;
        }

    if (hasDevExt)
        switch (mode) {
            case AudioSystem::MODE_NORMAL:
                ALSA_STRCAT (devString, "_normal");
                break;
            case AudioSystem::MODE_RINGTONE:
                ALSA_STRCAT (devString, "_ringtone");
                break;
            case AudioSystem::MODE_IN_CALL:
                ALSA_STRCAT (devString, "_incall");
                break;
        };

    return devString;
}

// ----------------------------------------------------------------------------

AudioStreamOutALSA::AudioStreamOutALSA(AudioHardwareALSA *parent) :
    mParent(parent),
    mPowerLock(false)
{
    static StreamDefaults _defaults = {
        devicePrefix   : "AndroidPlayback",
        direction      : SND_PCM_STREAM_PLAYBACK,
        format         : SND_PCM_FORMAT_S16_LE,   // AudioSystem::PCM_16_BIT
        channelCount       : 2,
        sampleRate     : DEFAULT_SAMPLE_RATE,
        bufferRatio   : 1,
        latency        : LATENCY_PLAYBACK_MS,               // Desired Delay in usec
        bufferSize     : BUFFER_SZ_PLAYBACK,                // Desired Number of samples
        periodSize     : PERIOD_SZ_PLAYBACK
};

    setStreamDefaults(&_defaults);
}

AudioStreamOutALSA::~AudioStreamOutALSA()
{
    standby();
}


/* New arch */
status_t AudioStreamOutALSA::setVolume(float left, float right)
{
    if (! mParent->mMixer || ! mDevice)
        return NO_INIT;

    /** Tushar - Need to decide on the volume value
     * that we pass onto the mixer. */
    return mParent->mMixer->setVolume (mDevice, (left + right)/2);
}

status_t AudioStreamOutALSA::setVolume(float volume)
{
    if (! mParent->mMixer || ! mDevice)
        return NO_INIT;

    return mParent->mMixer->setVolume (mDevice, volume);
}

/* New Arch */
status_t    AudioStreamOutALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    status_t status = NO_ERROR;
    int device;
    int value;
    LOGD("AudioStreamOutALSA::setParameters() %s", keyValuePairs.string());

    if (param.getInt(String8(AudioParameter::keyRouting), device) == NO_ERROR)
    {
        mParent->doRouting(device);

        param.remove(String8(AudioParameter::keyRouting));
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}


String8  AudioStreamOutALSA::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        param.addInt(key, (int)mDevice);
    }

    LOGD("AudioStreamOutALSA::getParameters() %s", param.toString().string());
    return param.toString();
}


status_t  AudioStreamOutALSA::getRenderPosition(uint32_t *dspFrames)
{

    //TODO: enable when supported by driver
    return INVALID_OPERATION;
}


ssize_t AudioStreamOutALSA::write(const void *buffer, size_t bytes)
{
    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;

    mParent->lock().lock();
    AutoMutex lock(mLock);

    if (!mPowerLock) {
        LOGD("Calling setDevice from write @..%d.\n",__LINE__);
        ALSAStreamOps::setDevice(mParent->mode(), mDevice, PLAYBACK);
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioOutLock");
        mPowerLock = true;
    }
    mParent->lock().unlock();

    do {
        // write correct number of bytes per attempt
        n = snd_pcm_writei(mHandle, (char *) buffer + sent, snd_pcm_bytes_to_frames(mHandle, bytes
                - sent));
        if (n == -EBADFD) {
            LOGD("Calling setDevice.. pcm_write returned error @..%d.\n",__LINE__);
            // Somehow the stream is in a bad state. The driver probably
            // has a bug and snd_pcm_recover() doesn't seem to handle this.
            ALSAStreamOps::setDevice(mParent->mode(), mDevice, PLAYBACK);
        } else if (n < 0) {
            if (mHandle) {
                // snd_pcm_recover() will return 0 if successful in recovering from
                //             // an error, or -errno if the error was unrecoverable.
                // We can make silent bit on as we are now handling the under-run and there will not be any data loss due to under-run
                n = snd_pcm_recover(mHandle, n, 1);
                if (n)
                    return static_cast<ssize_t> (n);
            }
        } else
            sent += static_cast<ssize_t> (snd_pcm_frames_to_bytes(mHandle, n));
    } while (mHandle && sent < bytes);
    //LOGI("Request Bytes=%d, Actual Written=%d",bytes,sent);
    return snd_pcm_frames_to_bytes(mHandle, sent);
}


status_t AudioStreamOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamOutALSA::setDevice(int mode,
                                       uint32_t newDevice,
                                       uint32_t audio_mode,
                                       bool force)
{
    AutoMutex lock(mLock);

    LOGV("AudioStreamOutALSA::setDevice(mode %d, newDevice %x, audio_mode %d), mDevice %x",
         mode, newDevice, audio_mode, mDevice);
    if (newDevice != mDevice || force) {
        return ALSAStreamOps::setDevice(mode, newDevice, audio_mode);
    }
    return NO_ERROR;
}

status_t AudioStreamOutALSA::standby() {
    AutoMutex _l(mParent->lock());
    AutoMutex lock(mLock);
    LOGD("Inside AudioStreamOutALSA::standby\n");

    if (mParent->mode() != AudioSystem::MODE_IN_CALL) {
        ALSAStreamOps::close();
    }

    if (mPowerLock) {
        release_wake_lock("AudioOutLock");
        mPowerLock = false;
    }
    return NO_ERROR;
}


#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioStreamOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mDefaults->latency);
}

// ----------------------------------------------------------------------------

AudioStreamInALSA::AudioStreamInALSA(AudioHardwareALSA *parent) :
    mParent(parent), mPowerLock(false),
    mDownSampler(NULL), mPcmIn(NULL)
{
    static StreamDefaults _defaults = {
        devicePrefix   : "AndroidRecord",
        direction      : SND_PCM_STREAM_CAPTURE,
        format         : SND_PCM_FORMAT_S16_LE,   // AudioSystem::PCM_16_BIT
        channelCount       : 1,
        sampleRate     : DEFAULT_SAMPLE_RATE,
        bufferRatio   : 1,
        latency        : LATENCY_CAPTURE_MS,// Desired Delay in usec
        bufferSize     : BUFFER_SZ_CAPTURE,      // Desired Number of samples
        periodSize     : PERIOD_SZ_CAPTURE
        };

    setStreamDefaults(&_defaults);
}

status_t AudioStreamInALSA::set(int *pformat,
                            uint32_t *pchannels,
                            uint32_t *prate)
{
    status_t status = ALSAStreamOps::set(pformat, pchannels, prate);
    if (status == NO_ERROR && prate && *prate != DEFAULT_SAMPLE_RATE) {
        mDownSampler = new ALSADownsampler(*prate,
                                           mDefaults->channelCount,
                                           PERIOD_SZ_CAPTURE,
                                           this);
        status = mDownSampler->initCheck();
        if (status != NO_ERROR) {
            return status;
        }
        mPcmIn = new int16_t[PERIOD_SZ_CAPTURE * mDefaults->channelCount];
    }
    return status;
}

AudioStreamInALSA::~AudioStreamInALSA()
{
    standby();
    if (mDownSampler != NULL) {
        delete mDownSampler;
    }
    if (mPcmIn != NULL) {
        delete[] mPcmIn;
    }
}

status_t AudioStreamInALSA::setGain(float gain)
{
    if (mParent->mMixer)
        return mParent->mMixer->setMasterGain (gain);
    else
        return NO_INIT;
}

ssize_t AudioStreamInALSA::read(void *buffer, ssize_t bytes)
{
    snd_pcm_sframes_t n;

    mParent->lock().lock();
    AutoMutex lock(mLock);
    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioInLock");

        LOGD("Calling setDevice from read@..%d.\n",__LINE__);
        ALSAStreamOps::setDevice(mParent->mode(), mDevice, CAPTURE);

        if (mDownSampler != NULL) {
            mDownSampler->reset();
            mReadStatus = 0;
            mInPcmInBuf = 0;
        }
        mPowerLock = true;
    }
    mParent->lock().unlock();

    if (!mHandle) {
        return -1;
    }

    // FIXME: only support reads of exactly bufferSize() for now
    if (bytes != (ssize_t)bufferSize()) {
        LOGW("AudioStreamInALSA::read bad read size %d expected %d", (int)bytes, bufferSize());
        return -1;
    }

    size_t frames = snd_pcm_bytes_to_frames(mHandle, bytes);
    do {
        if (mDownSampler) {
            status_t status = mDownSampler->resample((int16_t *)buffer, &frames);
            if (status != NO_ERROR) {
                if (mReadStatus != 0) {
                    n = mReadStatus;
                } else {
                    n = status;
                }
                frames = snd_pcm_bytes_to_frames(mHandle, bytes);
            } else {
                n = frames;
            }
        } else {
            n = snd_pcm_readi(mHandle,
                              (uint8_t *)buffer,
                              frames);
        }
        if (n < 0) {
            LOGD("AudioStreamInALSA::read error %d", (int)n);
            n = snd_pcm_recover(mHandle, n, 0);
            LOGD("AudioStreamInALSA::snd_pcm_recover error %d", (int)n);
            if (n)
                return static_cast<ssize_t> (n);
        }
    } while (n == 0);

    return snd_pcm_frames_to_bytes(mHandle, n);
}

status_t AudioStreamInALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamInALSA::setDevice(int mode,
                                      uint32_t newDevice,
                                      uint32_t audio_mode,
                                      bool force)
{
    AutoMutex lock(mLock);

    return ALSAStreamOps::setDevice(mode, newDevice, audio_mode);
}

status_t AudioStreamInALSA::standby()
{
    AutoMutex _l(mParent->lock());
    AutoMutex lock(mLock);

    LOGD("Entering AudioStreamInALSA::standby\n");

    ALSAStreamOps::close();

    if (mPowerLock) {
        release_wake_lock ("AudioInLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

/* New Arch */
status_t    AudioStreamInALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8("vr_mode");
    status_t status = NO_ERROR;
    int value;
    LOGD("AudioStreamInALSA::setParameters() %s", keyValuePairs.string());


    if (param.getInt(key, value) == NO_ERROR) {
        mParent->setVoiceRecordGain((value != 0));
        param.remove(key);
    }

    key = String8(AudioParameter::keyRouting);
    if (param.getInt(key, value) == NO_ERROR) {
        if(mHandle != NULL && value != 0)
            setDevice(mParent->mode(), value, CAPTURE);
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}


String8  AudioStreamInALSA::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        param.addInt(key, (int)mDevice);
    }

    LOGD("AudioStreamInALSA::getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioStreamInALSA::getNextBuffer(ALSABufferProvider::Buffer* buffer)
{
    if (mHandle == NULL) {
        buffer->raw = NULL;
        buffer->frameCount = 0;
        return NO_INIT;
    }

    if (mInPcmInBuf == 0) {
        while (mInPcmInBuf < PERIOD_SZ_CAPTURE) {
            mReadStatus = snd_pcm_readi(mHandle,
                              (uint8_t *)mPcmIn +
                              (mInPcmInBuf * mDefaults->channelCount * sizeof(int16_t)),
                              PERIOD_SZ_CAPTURE - mInPcmInBuf);
            if (mReadStatus < 0) {
                buffer->raw = NULL;
                buffer->frameCount = 0;
                LOGV("resampler read error %d", mReadStatus);
                return mReadStatus;
            }
            mInPcmInBuf += mReadStatus;
        }
    }

    buffer->frameCount = (buffer->frameCount > mInPcmInBuf) ? mInPcmInBuf : buffer->frameCount;
    buffer->i16 = mPcmIn + (PERIOD_SZ_CAPTURE - mInPcmInBuf) * mDefaults->channelCount;

    return NO_ERROR;
}

void AudioStreamInALSA::releaseBuffer(ALSABufferProvider::Buffer* buffer)
{
    mInPcmInBuf -= buffer->frameCount;
}


// ----------------------------------------------------------------------------

struct mixer_info_t
{
    mixer_info_t() :
        elem(0),
        min(SND_MIXER_VOL_RANGE_MIN),
        max(SND_MIXER_VOL_RANGE_MAX),
        mute(false)
    {
    }

    snd_mixer_elem_t *elem;
    long              min;
    long              max;
    long              volume;
    bool              mute;
    char              name[ALSA_NAME_MAX];
};

static int initMixer (snd_mixer_t **mixer, const char *name)
{
    int err;

    if ((err = snd_mixer_open(mixer, 0)) < 0) {
        LOGE("Unable to open mixer: %s", snd_strerror(err));
        return err;
    }

    if ((err = snd_mixer_attach(*mixer, name)) < 0) {
        LOGE("Unable to attach mixer to device %s: %s",
            name, snd_strerror(err));

        if ((err = snd_mixer_attach(*mixer, "hw:00")) < 0) {
            LOGE("Unable to attach mixer to device default: %s",
                snd_strerror(err));

            snd_mixer_close (*mixer);
            *mixer = NULL;
            return err;
        }
    }

    if ((err = snd_mixer_selem_register(*mixer, NULL, NULL)) < 0) {
        LOGE("Unable to register mixer elements: %s", snd_strerror(err));
        snd_mixer_close (*mixer);
        *mixer = NULL;
        return err;
    }

    // Get the mixer controls from the kernel
    if ((err = snd_mixer_load(*mixer)) < 0) {
        LOGE("Unable to load mixer elements: %s", snd_strerror(err));
        snd_mixer_close (*mixer);
        *mixer = NULL;
        return err;
    }

    return 0;
}

typedef int (*hasVolume_t)(snd_mixer_elem_t*);

static const hasVolume_t hasVolume[] = {
    snd_mixer_selem_has_playback_volume,
    snd_mixer_selem_has_capture_volume
};

typedef int (*getVolumeRange_t)(snd_mixer_elem_t*, long int*, long int*);

static const getVolumeRange_t getVolumeRange[] = {
    snd_mixer_selem_get_playback_volume_range,
    snd_mixer_selem_get_capture_volume_range
};

typedef int (*setVolume_t)(snd_mixer_elem_t*, long int);

static const setVolume_t setVol[] = {
    snd_mixer_selem_set_playback_volume_all,
    snd_mixer_selem_set_capture_volume_all
};

ALSAMixer::ALSAMixer()
{
    int err;

    initMixer (&mMixer[SND_PCM_STREAM_PLAYBACK], "AndroidPlayback");
    initMixer (&mMixer[SND_PCM_STREAM_CAPTURE], "AndroidRecord");

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {

        mixer_info_t *info = mixerMasterProp[i].mInfo = new mixer_info_t;

        property_get (mixerMasterProp[i].propName,
                      info->name,
                      mixerMasterProp[i].propDefault);

        for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
             elem;
             elem = snd_mixer_elem_next(elem)) {

            if (!snd_mixer_selem_is_active(elem))
                continue;

            snd_mixer_selem_get_id(elem, sid);

            // Find PCM playback volume control element.
            const char *elementName = snd_mixer_selem_id_get_name(sid);

            if (hasVolume[i] (elem))
                LOGD ("Mixer: element name: '%s'", elementName);

            if (info->elem == NULL &&
                strcmp(elementName, info->name) == 0 &&
                hasVolume[i] (elem)) {

                info->elem = elem;
                getVolumeRange[i] (elem, &info->min, &info->max);
                info->volume = info->max;
                setVol[i] (elem, info->volume);
                if (i == SND_PCM_STREAM_PLAYBACK &&
                    snd_mixer_selem_has_playback_switch (elem))
                    snd_mixer_selem_set_playback_switch_all (elem, 1);
                break;
            }
        }

        LOGD ("Mixer: master '%s' %s.", info->name, info->elem ? "found" : "not found");

        for (int j = 0; mixerProp[j][i].routes; j++) {

            mixer_info_t *info = mixerProp[j][i].mInfo = new mixer_info_t;

            property_get (mixerProp[j][i].propName,
                          info->name,
                          mixerProp[j][i].propDefault);

            for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
                 elem;
                 elem = snd_mixer_elem_next(elem)) {

                if (!snd_mixer_selem_is_active(elem))
                    continue;

                snd_mixer_selem_get_id(elem, sid);

                // Find PCM playback volume control element.
                const char *elementName = snd_mixer_selem_id_get_name(sid);

               if (info->elem == NULL &&
                    strcmp(elementName, info->name) == 0 &&
                    hasVolume[i] (elem)) {

                    info->elem = elem;
                    getVolumeRange[i] (elem, &info->min, &info->max);
                    info->volume = info->max;
                    setVol[i] (elem, info->volume);
                    if (i == SND_PCM_STREAM_PLAYBACK &&
                        snd_mixer_selem_has_playback_switch (elem))
                        snd_mixer_selem_set_playback_switch_all (elem, 1);
                    break;
                }
            }
            LOGD ("Mixer: route '%s' %s.", info->name, info->elem ? "found" : "not found");
        }
    }
    LOGD("mixer initialized.");
}

ALSAMixer::~ALSAMixer()
{
    for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {
        if (mMixer[i]) snd_mixer_close (mMixer[i]);
        if (mixerMasterProp[i].mInfo) {
            delete mixerMasterProp[i].mInfo;
            mixerMasterProp[i].mInfo = NULL;
        }
        for (int j = 0; mixerProp[j][i].routes; j++) {
            if (mixerProp[j][i].mInfo) {
                delete mixerProp[j][i].mInfo;
                mixerProp[j][i].mInfo = NULL;
            }
        }
    }
    LOGD("mixer destroyed.");
}

status_t ALSAMixer::setMasterVolume(float volume)
{
    mixer_info_t *info = mixerMasterProp[SND_PCM_STREAM_PLAYBACK].mInfo;
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + volume * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_playback_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setMasterGain(float gain)
{
    mixer_info_t *info = mixerMasterProp[SND_PCM_STREAM_CAPTURE].mInfo;
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + gain * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_capture_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setVolume(uint32_t device, float volume)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            long minVol = info->min;
            long maxVol = info->max;

            // Make sure volume is between bounds.
            long vol = minVol + volume * (maxVol - minVol);
            if (vol > maxVol) vol = maxVol;
            if (vol < minVol) vol = minVol;

            info->volume = vol;
            snd_mixer_selem_set_playback_volume_all (info->elem, vol);
        }

    return NO_ERROR;
}

status_t ALSAMixer::setGain(uint32_t device, float gain)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            long minVol = info->min;
            long maxVol = info->max;

            // Make sure volume is between bounds.
            long vol = minVol + gain * (maxVol - minVol);
            if (vol > maxVol) vol = maxVol;
            if (vol < minVol) vol = minVol;

            info->volume = vol;
            snd_mixer_selem_set_capture_volume_all (info->elem, vol);
        }

    return NO_ERROR;
}

status_t ALSAMixer::setCaptureMuteState(uint32_t device, bool state)
{
    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            if (snd_mixer_selem_has_capture_switch (info->elem)) {

                int err = snd_mixer_selem_set_capture_switch_all (info->elem, static_cast<int>(!state));
                if (err < 0) {
                    LOGE("Unable to %s capture mixer switch %s",
                        state ? "enable" : "disable", info->name);
                    return INVALID_OPERATION;
                }
            }

            info->mute = state;
        }

    return NO_ERROR;
}

status_t ALSAMixer::getCaptureMuteState(uint32_t device, bool *state)
{
    if (! state) return BAD_VALUE;

    for (int j = 0; mixerProp[j][SND_PCM_STREAM_CAPTURE].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_CAPTURE].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_CAPTURE].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            *state = info->mute;
            return NO_ERROR;
        }

    return BAD_VALUE;
}

status_t ALSAMixer::setPlaybackMuteState(uint32_t device, bool state)
{

    LOGE("\n set playback mute device %d, state %d \n", device,state);

    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            if (snd_mixer_selem_has_playback_switch (info->elem)) {

                int err = snd_mixer_selem_set_playback_switch_all (info->elem, static_cast<int>(!state));
                if (err < 0) {
                    LOGE("Unable to %s playback mixer switch %s",
                        state ? "enable" : "disable", info->name);
                    return INVALID_OPERATION;
                }
            }

            info->mute = state;
        }

    return NO_ERROR;
}

status_t ALSAMixer::getPlaybackMuteState(uint32_t device, bool *state)
{
    if (! state) return BAD_VALUE;

    for (int j = 0; mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes; j++)
        if (mixerProp[j][SND_PCM_STREAM_PLAYBACK].routes & device) {

            mixer_info_t *info = mixerProp[j][SND_PCM_STREAM_PLAYBACK].mInfo;
            if (!info || !info->elem) return INVALID_OPERATION;

            *state = info->mute;
            return NO_ERROR;
        }

    return BAD_VALUE;
}

// ----------------------------------------------------------------------------

ALSAControl::ALSAControl(const char *device)
{
    snd_ctl_open(&mHandle, device, 0);
}

ALSAControl::~ALSAControl()
{
    if (mHandle) snd_ctl_close(mHandle);
}

status_t ALSAControl::get(const char *name, unsigned int &value, int index)
{
    if (!mHandle) return NO_INIT;

    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *control;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&control);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_info_set_id(info, id);

    int ret = snd_ctl_elem_info(mHandle, info);
    if (ret < 0) return BAD_VALUE;

    snd_ctl_elem_info_get_id(info, id);
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    unsigned int count = snd_ctl_elem_info_get_count(info);
    if ((unsigned int)index >= count) return BAD_VALUE;

    snd_ctl_elem_value_set_id(control, id);

    ret = snd_ctl_elem_read(mHandle, control);
    if (ret < 0) return BAD_VALUE;

    switch (type) {
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            value = snd_ctl_elem_value_get_boolean(control, index);
            break;
        case SND_CTL_ELEM_TYPE_INTEGER:
            value = snd_ctl_elem_value_get_integer(control, index);
            break;
        case SND_CTL_ELEM_TYPE_INTEGER64:
            value = snd_ctl_elem_value_get_integer64(control, index);
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED:
            value = snd_ctl_elem_value_get_enumerated(control, index);
            break;
        case SND_CTL_ELEM_TYPE_BYTES:
            value = snd_ctl_elem_value_get_byte(control, index);
            break;
        default:
            return BAD_VALUE;
    }

    return NO_ERROR;
}

status_t ALSAControl::set(const char *name, unsigned int value, int index)
{
    if (!mHandle) return NO_INIT;

    snd_ctl_elem_id_t *id;
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *control;

    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&control);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_info_set_id(info, id);

    int ret = snd_ctl_elem_info(mHandle, info);
    if (ret < 0) return BAD_VALUE;

    snd_ctl_elem_info_get_id(info, id);
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    unsigned int count = snd_ctl_elem_info_get_count(info);

    if (index >= (int)count) return BAD_VALUE;

    if (index == -1)
        index = 0; // Range over all of them
    else
        count = index + 1; // Just do the one specified

    snd_ctl_elem_value_set_id(control, id);

    for (unsigned int i = index; i < count; i++)
        switch (type) {
            case SND_CTL_ELEM_TYPE_BOOLEAN:
                snd_ctl_elem_value_set_boolean(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_INTEGER:
                snd_ctl_elem_value_set_integer(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_INTEGER64:
                snd_ctl_elem_value_set_integer64(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_ENUMERATED:
                snd_ctl_elem_value_set_enumerated(control, i, value);
                break;
            case SND_CTL_ELEM_TYPE_BYTES:
                snd_ctl_elem_value_set_byte(control, i, value);
                break;
            default:
                break;
        }
    ret = snd_ctl_elem_write(mHandle, control);
    return (ret < 0) ? BAD_VALUE : NO_ERROR;
}

//------------------------------------------------------------------------------
// Downsampler
//------------------------------------------------------------------------------

/*
 * 2.30 fixed point FIR filter coefficients for conversion 44100 -> 22050.
 * (Works equivalently for 22010 -> 11025 or any other halving, of course.)
 *
 * Transition band from about 18 kHz, passband ripple < 0.1 dB,
 * stopband ripple at about -55 dB, linear phase.
 *
 * Design and display in MATLAB or Octave using:
 *
 * filter = fir1(19, 0.5); filter = round(filter * 2**30); freqz(filter * 2**-30);
 */
static const int32_t filter_22khz_coeff[] = {
    2089257, 2898328, -5820678, -10484531,
    19038724, 30542725, -50469415, -81505260,
    152544464, 478517512, 478517512, 152544464,
    -81505260, -50469415, 30542725, 19038724,
    -10484531, -5820678, 2898328, 2089257,
};
#define NUM_COEFF_22KHZ (sizeof(filter_22khz_coeff) / sizeof(filter_22khz_coeff[0]))
#define OVERLAP_22KHZ (NUM_COEFF_22KHZ - 2)

/*
 * Convolution of signals A and reverse(B). (In our case, the filter response
 * is symmetric, so the reversing doesn't matter.)
 * A is taken to be in 0.16 fixed-point, and B is taken to be in 2.30 fixed-point.
 * The answer will be in 16.16 fixed-point, unclipped.
 *
 * This function would probably be the prime candidate for SIMD conversion if
 * you want more speed.
 */
int32_t fir_convolve(const int16_t* a, const int32_t* b, int num_samples)
{
        int32_t sum = 1 << 13;
        for (int i = 0; i < num_samples; ++i) {
                sum += a[i] * (b[i] >> 16);
        }
        return sum >> 14;
}

/* Clip from 16.16 fixed-point to 0.16 fixed-point. */
int16_t clip(int32_t x)
{
    if (x < -32768) {
        return -32768;
    } else if (x > 32767) {
        return 32767;
    } else {
        return x;
    }
}

/*
 * Convert a chunk from 44 kHz to 22 kHz. Will update num_samples_in and num_samples_out
 * accordingly, since it may leave input samples in the buffer due to overlap.
 *
 * Input and output are taken to be in 0.16 fixed-point.
 */
void resample_2_1(int16_t* input, int16_t* output, int* num_samples_in, int* num_samples_out)
{
    if (*num_samples_in < (int)NUM_COEFF_22KHZ) {
        *num_samples_out = 0;
        return;
    }

    int odd_smp = *num_samples_in & 0x1;
    int num_samples = *num_samples_in - odd_smp - OVERLAP_22KHZ;

    for (int i = 0; i < num_samples; i += 2) {
            output[i / 2] = clip(fir_convolve(input + i, filter_22khz_coeff, NUM_COEFF_22KHZ));
    }

    memmove(input, input + num_samples, (OVERLAP_22KHZ + odd_smp) * sizeof(*input));
    *num_samples_out = num_samples / 2;
    *num_samples_in = OVERLAP_22KHZ + odd_smp;
}

/*
 * 2.30 fixed point FIR filter coefficients for conversion 22050 -> 16000,
 * or 11025 -> 8000.
 *
 * Transition band from about 14 kHz, passband ripple < 0.1 dB,
 * stopband ripple at about -50 dB, linear phase.
 *
 * Design and display in MATLAB or Octave using:
 *
 * filter = fir1(23, 16000 / 22050); filter = round(filter * 2**30); freqz(filter * 2**-30);
 */
static const int32_t filter_16khz_coeff[] = {
    2057290, -2973608, 1880478, 4362037,
    -14639744, 18523609, -1609189, -38502470,
    78073125, -68353935, -59103896, 617555440,
    617555440, -59103896, -68353935, 78073125,
    -38502470, -1609189, 18523609, -14639744,
    4362037, 1880478, -2973608, 2057290,
};
#define NUM_COEFF_16KHZ (sizeof(filter_16khz_coeff) / sizeof(filter_16khz_coeff[0]))
#define OVERLAP_16KHZ (NUM_COEFF_16KHZ - 1)

/*
 * Convert a chunk from 22 kHz to 16 kHz. Will update num_samples_in and
 * num_samples_out accordingly, since it may leave input samples in the buffer
 * due to overlap.
 *
 * This implementation is rather ad-hoc; it first low-pass filters the data
 * into a temporary buffer, and then converts chunks of 441 input samples at a
 * time into 320 output samples by simple linear interpolation. A better
 * implementation would use a polyphase filter bank to do these two operations
 * in one step.
 *
 * Input and output are taken to be in 0.16 fixed-point.
 */

#define RESAMPLE_16KHZ_SAMPLES_IN 441
#define RESAMPLE_16KHZ_SAMPLES_OUT 320

void resample_441_320(int16_t* input, int16_t* output, int* num_samples_in, int* num_samples_out)
{
    const int num_blocks = (*num_samples_in - OVERLAP_16KHZ) / RESAMPLE_16KHZ_SAMPLES_IN;
    if (num_blocks < 1) {
        *num_samples_out = 0;
        return;
    }

    for (int i = 0; i < num_blocks; ++i) {
        uint32_t tmp[RESAMPLE_16KHZ_SAMPLES_IN];
        for (int j = 0; j < RESAMPLE_16KHZ_SAMPLES_IN; ++j) {
            tmp[j] = fir_convolve(input + i * RESAMPLE_16KHZ_SAMPLES_IN + j,
                          filter_16khz_coeff,
                          NUM_COEFF_16KHZ);
        }

        const float step_float = (float)RESAMPLE_16KHZ_SAMPLES_IN / (float)RESAMPLE_16KHZ_SAMPLES_OUT;

        uint32_t in_sample_num = 0;   // 16.16 fixed point
        const uint32_t step = (uint32_t)(step_float * 65536.0f + 0.5f);  // 16.16 fixed point
        for (int j = 0; j < RESAMPLE_16KHZ_SAMPLES_OUT; ++j, in_sample_num += step) {
            const uint32_t whole = in_sample_num >> 16;
            const uint32_t frac = (in_sample_num & 0xffff);  // 0.16 fixed point
            const int32_t s1 = tmp[whole];
            const int32_t s2 = tmp[whole + 1];
            *output++ = clip(s1 + (((s2 - s1) * (int32_t)frac) >> 16));
        }
    }

    const int samples_consumed = num_blocks * RESAMPLE_16KHZ_SAMPLES_IN;
    memmove(input, input + samples_consumed, (*num_samples_in - samples_consumed) * sizeof(*input));
    *num_samples_in -= samples_consumed;
    *num_samples_out = RESAMPLE_16KHZ_SAMPLES_OUT * num_blocks;
}


ALSADownsampler::ALSADownsampler(uint32_t outSampleRate,
                                    uint32_t channelCount,
                                    uint32_t frameCount,
                                    ALSABufferProvider* provider)
    :  mStatus(NO_INIT), mProvider(provider), mSampleRate(outSampleRate),
       mChannelCount(channelCount), mFrameCount(frameCount),
       mInLeft(NULL), mInRight(NULL), mTmpLeft(NULL), mTmpRight(NULL),
       mTmp2Left(NULL), mTmp2Right(NULL), mOutLeft(NULL), mOutRight(NULL)

{
    LOGV("ALSADownsampler() cstor SR %d channels %d frames %d",
         mSampleRate, mChannelCount, mFrameCount);

    if (mSampleRate != 8000 && mSampleRate != 11025 && mSampleRate != 16000 &&
            mSampleRate != 22050) {
        LOGW("ALSADownsampler cstor: bad sampling rate: %d", mSampleRate);
        return;
    }

    mInLeft = new int16_t[mFrameCount];
    mInRight = new int16_t[mFrameCount];
    mTmpLeft = new int16_t[mFrameCount];
    mTmpRight = new int16_t[mFrameCount];
    mTmp2Left = new int16_t[mFrameCount];
    mTmp2Right = new int16_t[mFrameCount];
    mOutLeft = new int16_t[mFrameCount];
    mOutRight = new int16_t[mFrameCount];

    mStatus = NO_ERROR;
}

ALSADownsampler::~ALSADownsampler()
{
    if (mInLeft) delete[] mInLeft;
    if (mInRight) delete[] mInRight;
    if (mTmpLeft) delete[] mTmpLeft;
    if (mTmpRight) delete[] mTmpRight;
    if (mTmp2Left) delete[] mTmp2Left;
    if (mTmp2Right) delete[] mTmp2Right;
    if (mOutLeft) delete[] mOutLeft;
    if (mOutRight) delete[] mOutRight;
}

void ALSADownsampler::reset()
{
    mInInBuf = 0;
    mInTmpBuf = 0;
    mInTmp2Buf = 0;
    mOutBufPos = 0;
    mInOutBuf = 0;
}


int ALSADownsampler::resample(int16_t* out, size_t *outFrameCount)
{
    if (mStatus != NO_ERROR) {
        return mStatus;
    }

    if (out == NULL || outFrameCount == NULL) {
        return BAD_VALUE;
    }

    int16_t *outLeft = mTmp2Left;
    int16_t *outRight = mTmp2Left;
    if (mSampleRate == 22050) {
        outLeft = mTmpLeft;
        outRight = mTmpRight;
    } else if (mSampleRate == 8000){
        outLeft = mOutLeft;
        outRight = mOutRight;
    }

    int outFrames = 0;
    int remaingFrames = *outFrameCount;

    if (mInOutBuf) {
        int frames = (remaingFrames > mInOutBuf) ? mInOutBuf : remaingFrames;

        for (int i = 0; i < frames; ++i) {
            out[i] = outLeft[mOutBufPos + i];
        }
        if (mChannelCount == 2) {
            for (int i = 0; i < frames; ++i) {
                out[i * 2] = outLeft[mOutBufPos + i];
                out[i * 2 + 1] = outRight[mOutBufPos + i];
            }
        }
        remaingFrames -= frames;
        mInOutBuf -= frames;
        mOutBufPos += frames;
        outFrames += frames;
    }

    while (remaingFrames) {
        LOGW_IF((mInOutBuf != 0), "mInOutBuf should be 0 here");

        ALSABufferProvider::Buffer buf;
        buf.frameCount =  mFrameCount - mInInBuf;
        int ret = mProvider->getNextBuffer(&buf);
        if (buf.raw == NULL) {
            *outFrameCount = outFrames;
            return ret;
        }

        for (size_t i = 0; i < buf.frameCount; ++i) {
            mInLeft[i + mInInBuf] = buf.i16[i];
        }
        if (mChannelCount == 2) {
            for (size_t i = 0; i < buf.frameCount; ++i) {
                mInLeft[i + mInInBuf] = buf.i16[i * 2];
                mInRight[i + mInInBuf] = buf.i16[i * 2 + 1];
            }
        }
        mInInBuf += buf.frameCount;
        mProvider->releaseBuffer(&buf);

        /* 44010 -> 22050 */
        {
            int samples_in_left = mInInBuf;
            int samples_out_left;
            resample_2_1(mInLeft, mTmpLeft + mInTmpBuf, &samples_in_left, &samples_out_left);

            if (mChannelCount == 2) {
                int samples_in_right = mInInBuf;
                int samples_out_right;
                resample_2_1(mInRight, mTmpRight + mInTmpBuf, &samples_in_right, &samples_out_right);
            }

            mInInBuf = samples_in_left;
            mInTmpBuf += samples_out_left;
            mInOutBuf = samples_out_left;
        }

        if (mSampleRate == 11025 || mSampleRate == 8000) {
            /* 22050 - > 11025 */
            int samples_in_left = mInTmpBuf;
            int samples_out_left;
            resample_2_1(mTmpLeft, mTmp2Left + mInTmp2Buf, &samples_in_left, &samples_out_left);

            if (mChannelCount == 2) {
                int samples_in_right = mInTmpBuf;
                int samples_out_right;
                resample_2_1(mTmpRight, mTmp2Right + mInTmp2Buf, &samples_in_right, &samples_out_right);
            }


            mInTmpBuf = samples_in_left;
            mInTmp2Buf += samples_out_left;
            mInOutBuf = samples_out_left;

            if (mSampleRate == 8000) {
                /* 11025 -> 8000*/
                int samples_in_left = mInTmp2Buf;
                int samples_out_left;
                resample_441_320(mTmp2Left, mOutLeft, &samples_in_left, &samples_out_left);

                if (mChannelCount == 2) {
                    int samples_in_right = mInTmp2Buf;
                    int samples_out_right;
                    resample_441_320(mTmp2Right, mOutRight, &samples_in_right, &samples_out_right);
                }

                mInTmp2Buf = samples_in_left;
                mInOutBuf = samples_out_left;
            } else {
                mInTmp2Buf = 0;
            }

        } else if (mSampleRate == 16000) {
            /* 22050 -> 16000*/
            int samples_in_left = mInTmpBuf;
            int samples_out_left;
            resample_441_320(mTmpLeft, mTmp2Left, &samples_in_left, &samples_out_left);

            if (mChannelCount == 2) {
                int samples_in_right = mInTmpBuf;
                int samples_out_right;
                resample_441_320(mTmpRight, mTmp2Right, &samples_in_right, &samples_out_right);
            }

            mInTmpBuf = samples_in_left;
            mInOutBuf = samples_out_left;
        } else {
            mInTmpBuf = 0;
        }

        int frames = (remaingFrames > mInOutBuf) ? mInOutBuf : remaingFrames;

        for (int i = 0; i < frames; ++i) {
            out[outFrames + i] = outLeft[i];
        }
        if (mChannelCount == 2) {
            for (int i = 0; i < frames; ++i) {
                out[(outFrames + i) * 2] = outLeft[i];
                out[(outFrames + i) * 2 + 1] = outRight[i];
            }
        }
        remaingFrames -= frames;
        outFrames += frames;
        mOutBufPos = frames;
        mInOutBuf -= frames;
    }

    return 0;
}

};        // namespace android


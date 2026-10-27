[13:55, 10/27/2026] Kaspars: cmake_minimum_required(VERSION 3.18.1)
project("mididawpro_core")

include(FetchContent)

# Automātiski lejupielādē un sagatavo Google Oboe dzinēju
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com
    GIT_TAG        v1.9.3
)
FetchContent_MakeAvailable(oboe)

add_library(mididawpro_core SHARED src/main/cpp/native-lib.cpp)

# Saslēdz kopā Pro-Audio, AMidi un sistēmas logus
target_link_libraries(mididawpro_core
        oboe
        amidi
        log
        android)
[13:57, 10/27/2026] Kaspars: #include <jni.h>
#include <oboe/Oboe.h>
#include <amidi/AMidi.h>
#include <android/log.h>
#include <math.h>
#include <vector>

#define LOG_TAG "MidiDawProCore"
#define LOGE(...) _android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS_)

constexpr int MAX_VOICES = 8;
constexpr double SAMPLE_RATE = 48000.0;

struct Voice {
    int noteId = -1;
    double phase = 0.0;
    double phaseIncrement = 0.0;
    float amplitude = 0.0f;
    bool active = false;
};

class MidiDawProEngine : public oboe::AudioStreamDataCallback {
public:
    MidiDawProEngine() {
        mVoices.resize(MAX_VOICES);
        setFilterCutoff(1000.0f, 0.707f); 
    }

    void start() {
        oboe::AudioStreamBuilder builder;
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency)
               ->setSharingMode(oboe::SharingMode::Exclusive) // MMAP Exclusive dzelžu celiņš
               ->setFormat(oboe::AudioFormat::Float)          // Hi-Res 32-bit Float skaņa
               ->setChannelCount(oboe::ChannelCount::Stereo)
               ->setDataCallback(this);

        oboe::Result result = builder.openStream(mStream);
        if (result == oboe::Result::OK && mStream) {
            mStream->requestStart();
        } else {
            LOGE("Kļūda atverot Oboe plūsmu: %s", oboe::convertToText(result));
        }
    }

    void setMidiOutputPort(AMidiOutputPort* port) {
        mMidiOutputPort = port;
    }

    void setFilterCutoff(float cutoffHz, float resonance) {
        float omega = 2.0f * M_PI * cutoffHz / SAMPLE_RATE;
        float alpha = sin(omega) / (2.0f * resonance);
        mFilterA0 = 1.0f + alpha;
        mFilterA1 = -2.0f * cos(omega);
        mFilterA2 = 1.0f - alpha;
        mFilterB0 = (1.0f - cos(omega)) / 2.0f;
        mFilterB1 = 1.0f - cos(omega);
        mFilterB2 = (1.0f - cos(omega)) / 2.0f;
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override {
        float *floatData = static_cast<float *>(audioData);

        // 1. Reālā laika USB-OTG MIDI nolasīšana bez OS aiztures
        if (mMidiOutputPort != nullptr) {
            uint8_t midiBuffer[4];
            int32_t numBytesRead = 0;
            uint64_t timestamp = 0;
            
            while (AMidiOutputPort_receive(mMidiOutputPort, &numBytesRead, midiBuffer, sizeof(midiBuffer), &timestamp) > 0) {
                if (numBytesRead >= 3) {
                    uint8_t status = midiBuffer[0] & 0xF0;
                    uint8_t note = midiBuffer[1];
                    uint8_t velocity = midiBuffer[2];

                    if (status == 0x90 && velocity > 0) { 
                        noteOn(note, velocity);
                    } else if (status == 0x80 || (status == 0x90 && velocity == 0)) { 
                        noteOff(note);
                    }
                }
            }
        }

        // 2. Polifonisks sintezators + DSP Resonant Low-Pass Filtrs
        for (int i = 0; i < numFrames; ++i) {
            float mixedSample = 0.0f;

            for (auto& voice : mVoices) {
                if (voice.active) {
                    mixedSample += sin(voice.phase) * voice.amplitude;
                    voice.phase += voice.phaseIncrement;
                    if (voice.phase >= 2.0 * M_PI) voice.phase -= 2.0 * M_PI;
                }
            }

            float filteredSample = (mFilterB0 / mFilterA0) * mixedSample 
                                 + (mFilterB1 / mFilterA0) * mFilterX1 
                                 + (mFilterB2 / mFilterA0) * mFilterX2
                                 - (mFilterA1 / mFilterA0) * mFilterY1 
                                 - (mFilterA2 / mFilterA0) * mFilterY2;

            mFilterX2 = mFilterX1; mFilterX1 = mixedSample;
            mFilterY2 = mFilterY1; mFilterY1 = filteredSample;

            *floatData++ = filteredSample;
            *floatData++ = filteredSample;
        }
        return oboe::DataCallbackResult::Continue;
    }

    void stop() {
        if (mStream) {
            mStream->stop();
            mStream->close();
        }
    }

private:
    std::shared_ptr<oboe::AudioStream> mStream;
    AMidiOutputPort* mMidiOutputPort = nullptr;
    std::vector<Voice> mVoices;

    float mFilterX1 = 0, mFilterX2 = 0, mFilterY1 = 0, mFilterY2 = 0;
    float mFilterA0 = 1, mFilterA1 = 0, mFilterA2 = 0, mFilterB0 = 0, mFilterB1 = 0, mFilterB2 = 0;

    void noteOn(int note, int velocity) {
        for (auto& voice : mVoices) {
            if (!voice.active) {
                voice.noteId = note;
                double freq = 440.0 * pow(2.0, (note - 69.0) / 12.0);
                voice.phaseIncrement = freq * 2.0 * M_PI / SAMPLE_RATE;
                voice.amplitude = (velocity / 127.0f) * 0.15f; 
                voice.phase = 0.0;
                voice.active = true;
                break;
            }
        }
    }

    void noteOff(int note) {
        for (auto& voice : mVoices) {
            if (voice.active && voice.noteId == note) {
                voice.active = false;
            }
        }
    }
};

static MidiDawProEngine proEngine;

extern "C" JNIEXPORT void JNICALL
Java_com_mididaw_pro_NativeAudioEngine_init(JNIEnv *env, jclass clazz) {
    proEngine.start();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mididaw_pro_NativeAudioEngine_stop(JNIEnv *env, jclass clazz) {
    proEngine.stop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mididaw_pro_NativeAudioEngine_connectMidiDevice(JNIEnv *env, jclass clazz, jobject javaMidiDevice, jint portNumber) {
    AMidiDevice* midiDevice;
    media_status_t status = AMidiDevice_fromJava(env, javaMidiDevice, &midiDevice);
    if (status == AMIDIDEVICE_RESULT_SUCCESS) {
        AMidiOutputPort* outputPort;
        AMidiOutputPort_open(midiDevice, portNumber, &outputPort);
        proEngine.setMidiOutputPort(outputPort);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mididaw_pro_NativeAudioEngine_setFilterCutoff(JNIEnv *env, jclass clazz, jfloat cutoff, jfloat resonance) {
    proEngine.setFilterCutoff(cutoff, resonance);
}

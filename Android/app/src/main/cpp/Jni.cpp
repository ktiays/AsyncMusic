//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

// The JNI bridge between `NativeEngine.kt` and `Engine`. Handles are `Engine` pointers; the
// callbacks object is held globally for the engine's lifetime and called from the engine's
// threads, which are attached to the VM on first use and detached when they end.

#include <jni.h>
#include <pthread.h>

#include <memory>
#include <string>

#include "Engine.h"

using asyncmusic::DeviceIdentity;
using asyncmusic::GroupSession;
using asyncmusic::ShareCenter;
using asyncmusic::ShareDeviceKind;
using asyncmusic::ShareSourceKind;
using asyncmusic::SpeakerChannel;
using asyncmusic::SpeakerLayout;
using asyncmusic::android::Engine;

namespace {

JavaVM* gVM = nullptr;
pthread_key_t gDetachKey;

/// The calling thread's environment, attaching the thread if it is not a Java thread.
JNIEnv* currentEnv() {
    JNIEnv* env = nullptr;
    if (gVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    JavaVMAttachArgs arguments{JNI_VERSION_1_6, "asyncmusic", nullptr};
    if (gVM->AttachCurrentThread(&env, &arguments) != JNI_OK) return nullptr;
    // Detach when the thread ends.
    pthread_setspecific(gDetachKey, env);
    return env;
}

std::string toString(JNIEnv* env, jstring text) {
    if (text == nullptr) return {};
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result = chars != nullptr ? chars : "";
    if (chars != nullptr) env->ReleaseStringUTFChars(text, chars);
    return result;
}

/// Java's modified UTF-8 differs from UTF-8 for characters outside the BMP (emoji in names),
/// so strings cross the boundary as UTF-8 bytes and are decoded on the Kotlin side.
jbyteArray toBytes(JNIEnv* env, const std::string& text) {
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(text.size()));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte*>(text.data()));
    return bytes;
}

std::string fromBytes(JNIEnv* env, jbyteArray bytes) {
    if (bytes == nullptr) return {};
    const jsize length = env->GetArrayLength(bytes);
    std::string result(static_cast<size_t>(length), '\0');
    env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(result.data()));
    return result;
}

/// The engine and the Kotlin object its callbacks go to.
struct Bridge {
    jobject callbacks = nullptr;
    jmethodID onChange = nullptr;
    jmethodID onAlert = nullptr;
    jmethodID onSpeakerSettingsChange = nullptr;
    std::unique_ptr<Engine> engine;
};

Bridge* bridge(jlong handle) {
    return reinterpret_cast<Bridge*>(handle);
}

ShareCenter& center(jlong handle) {
    return bridge(handle)->engine->center();
}

void clearException(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

GroupSession::SpeakerSettings speakerSettings(jint layout, jlong left, jlong right) {
    GroupSession::SpeakerSettings settings;
    if (layout >= 0 && layout <= asyncmusic::kSpeakerLayoutMaximum) settings.layout = static_cast<SpeakerLayout>(layout);
    if (left != 0) settings.assignments[SpeakerChannel::left] = static_cast<uint64_t>(left);
    if (right != 0) settings.assignments[SpeakerChannel::right] = static_cast<uint64_t>(right);
    return settings;
}

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    gVM = vm;
    pthread_key_create(&gDetachKey, [](void*) { gVM->DetachCurrentThread(); });
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeCreate(JNIEnv* env, jclass, jobject callbacks, jlong id,
    jbyteArray name, jint deviceKind, jbyteArray model, jint layout, jlong left, jlong right, jdouble manualOffset, jfloat volume, jboolean alertsEnabled) {
    auto* result = new Bridge;
    result->callbacks = env->NewGlobalRef(callbacks);
    jclass type = env->GetObjectClass(callbacks);
    result->onChange = env->GetMethodID(type, "onChange", "()V");
    result->onAlert = env->GetMethodID(type, "onAlert", "([B)V");
    result->onSpeakerSettingsChange = env->GetMethodID(type, "onSpeakerSettingsChange", "(IJJ)V");
    env->DeleteLocalRef(type);

    Engine::Callbacks hooks;
    hooks.onChange = [result] {
        if (JNIEnv* env = currentEnv()) {
            env->CallVoidMethod(result->callbacks, result->onChange);
            clearException(env);
        }
    };
    hooks.onAlert = [result](const ShareCenter::Share& share) {
        JNIEnv* env = currentEnv();
        if (env == nullptr) return;
        jbyteArray json = toBytes(env, asyncmusic::android::shareJSON(share));
        env->CallVoidMethod(result->callbacks, result->onAlert, json);
        clearException(env);
        env->DeleteLocalRef(json);
    };
    hooks.onSpeakerSettingsChange = [result](const GroupSession::SpeakerSettings& settings) {
        JNIEnv* env = currentEnv();
        if (env == nullptr) return;
        const auto device = [&](SpeakerChannel channel) {
            const auto found = settings.assignments.find(channel);
            return static_cast<jlong>(found == settings.assignments.end() ? 0 : found->second);
        };
        env->CallVoidMethod(result->callbacks, result->onSpeakerSettingsChange, static_cast<jint>(settings.layout), device(SpeakerChannel::left),
            device(SpeakerChannel::right));
        clearException(env);
    };

    Engine::Options options;
    options.identity = DeviceIdentity{static_cast<uint64_t>(id), fromBytes(env, name)};
    options.device.kind = deviceKind >= 0 && deviceKind <= static_cast<int>(ShareDeviceKind::tablet) ? static_cast<ShareDeviceKind>(deviceKind)
                                                                                                    : ShareDeviceKind::unknown;
    options.device.model = fromBytes(env, model);
    options.speakers = speakerSettings(layout, left, right);
    options.manualOffset = manualOffset;
    options.volume = volume;
    options.alertsEnabled = alertsEnabled;
    result->engine = std::make_unique<Engine>(std::move(options), std::move(hooks));
    return reinterpret_cast<jlong>(result);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeDestroy(JNIEnv* env, jclass, jlong handle) {
    Bridge* target = bridge(handle);
    target->engine.reset();
    env->DeleteGlobalRef(target->callbacks);
    delete target;
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetIdentity(JNIEnv* env, jclass, jlong handle, jlong id, jbyteArray name) {
    bridge(handle)->engine->session().setIdentity(DeviceIdentity{static_cast<uint64_t>(id), fromBytes(env, name)});
}

// MARK: - Share center

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetMode(JNIEnv*, jclass, jlong handle, jboolean share) {
    center(handle).setMode(share ? ShareCenter::Mode::share : ShareCenter::Mode::listen);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetAlertsEnabled(JNIEnv*, jclass, jlong handle, jboolean enabled) {
    center(handle).setAlertsEnabled(enabled);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeJoin(JNIEnv*, jclass, jlong handle, jlong share) {
    center(handle).join(static_cast<uint64_t>(share));
}

extern "C" JNIEXPORT jboolean JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeJoinAddress(JNIEnv* env, jclass, jlong handle, jbyteArray name,
    jstring host, jint port) {
    if (port <= 0 || port > 65535) return JNI_FALSE;
    const auto address = asyncmusic::net::Address::fromString(toString(env, host), static_cast<uint16_t>(port));
    if (!address) return JNI_FALSE;
    center(handle).joinAddress(fromBytes(env, name), *address);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeLeave(JNIEnv*, jclass, jlong handle) {
    center(handle).leave();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeRequestChannel(JNIEnv*, jclass, jlong handle, jint channel) {
    if (channel <= 0 || channel > asyncmusic::kSpeakerChannelMaximum) return;
    center(handle).requestChannel(static_cast<SpeakerChannel>(channel));
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeDismissAlerts(JNIEnv*, jclass, jlong handle) {
    center(handle).dismissAlerts();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeRefresh(JNIEnv*, jclass, jlong handle) {
    center(handle).refresh();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSelectSource(JNIEnv* env, jclass, jlong handle, jbyteArray name, jint kind) {
    ShareCenter::Source source;
    source.name = fromBytes(env, name);
    source.kind = kind >= 0 && kind <= static_cast<int>(ShareSourceKind::system) ? static_cast<ShareSourceKind>(kind) : ShareSourceKind::unknown;
    center(handle).selectSource(std::move(source));
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeStartSharing(JNIEnv*, jclass, jlong handle) {
    center(handle).startSharing();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeStopSharing(JNIEnv*, jclass, jlong handle) {
    center(handle).stopSharing();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetLayout(JNIEnv*, jclass, jlong handle, jint layout) {
    if (layout < 0 || layout > asyncmusic::kSpeakerLayoutMaximum) return;
    center(handle).setLayout(static_cast<SpeakerLayout>(layout));
}

// MARK: - Source and output

extern "C" JNIEXPORT jbyteArray JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeLoadTrack(JNIEnv* env, jclass, jlong handle, jstring path,
    jbyteArray name, jbyteArray digest) {
    uint8_t bytes[32] = {};
    if (digest != nullptr && env->GetArrayLength(digest) == 32) env->GetByteArrayRegion(digest, 0, 32, reinterpret_cast<jbyte*>(bytes));
    const std::string error = bridge(handle)->engine->loadTrack(toString(env, path), fromBytes(env, name), bytes);
    return error.empty() ? nullptr : toBytes(env, error);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetSourceStatus(JNIEnv* env, jclass, jlong handle, jboolean isLoading,
    jbyteArray error) {
    bridge(handle)->engine->setSourceStatus(isLoading, fromBytes(env, error));
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeClearLog(JNIEnv*, jclass, jlong handle) {
    bridge(handle)->engine->session().clearLog();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetManualOffset(JNIEnv*, jclass, jlong handle, jdouble milliseconds) {
    bridge(handle)->engine->setManualOffset(milliseconds);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetVolume(JNIEnv*, jclass, jlong handle, jfloat volume) {
    bridge(handle)->engine->setVolume(volume);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetPausedHere(JNIEnv*, jclass, jlong handle, jboolean paused) {
    bridge(handle)->engine->setPausedHere(paused);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativePlaySelected(JNIEnv*, jclass, jlong handle) {
    bridge(handle)->engine->playSelected();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeStopPlayback(JNIEnv*, jclass, jlong handle) {
    bridge(handle)->engine->stopPlayback();
}

// MARK: - State

extern "C" JNIEXPORT jbyteArray JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeState(JNIEnv* env, jclass, jlong handle) {
    return toBytes(env, bridge(handle)->engine->stateJSON());
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSnapshot(JNIEnv* env, jclass, jlong handle) {
    return toBytes(env, bridge(handle)->engine->snapshotJSON());
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeLive(JNIEnv* env, jclass, jlong handle) {
    return toBytes(env, bridge(handle)->engine->liveJSON());
}

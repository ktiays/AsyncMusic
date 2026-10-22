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
#include <optional>
#include <string>

#include "Engine.h"

using asyncmusic::DeviceIdentity;
using asyncmusic::GroupSession;
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
    jmethodID onSpeakerSettingsChange = nullptr;
    jmethodID resolve = nullptr;
    std::unique_ptr<Engine> engine;
};

Bridge* bridge(jlong handle) {
    return reinterpret_cast<Bridge*>(handle);
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
    jbyteArray name, jint layout, jlong left, jlong right, jdouble manualOffset) {
    auto* result = new Bridge;
    result->callbacks = env->NewGlobalRef(callbacks);
    jclass type = env->GetObjectClass(callbacks);
    result->onChange = env->GetMethodID(type, "onChange", "()V");
    result->onSpeakerSettingsChange = env->GetMethodID(type, "onSpeakerSettingsChange", "(IJJ)V");
    result->resolve = env->GetMethodID(type, "resolve", "([B)Ljava/lang/String;");
    env->DeleteLocalRef(type);

    Engine::Callbacks hooks;
    hooks.onChange = [result] {
        if (JNIEnv* env = currentEnv()) {
            env->CallVoidMethod(result->callbacks, result->onChange);
            clearException(env);
        }
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
    hooks.resolve = [result](const std::string& leader) -> std::optional<asyncmusic::net::Address> {
        JNIEnv* env = currentEnv();
        if (env == nullptr) return std::nullopt;
        jbyteArray name = toBytes(env, leader);
        auto found = static_cast<jstring>(env->CallObjectMethod(result->callbacks, result->resolve, name));
        clearException(env);
        env->DeleteLocalRef(name);
        if (found == nullptr) return std::nullopt;
        // "host port", the host numeric (IPv6 possibly with a scope).
        const std::string text = toString(env, found);
        env->DeleteLocalRef(found);
        const size_t space = text.rfind(' ');
        if (space == std::string::npos) return std::nullopt;
        const int port = std::atoi(text.c_str() + space + 1);
        if (port <= 0 || port > 65535) return std::nullopt;
        return asyncmusic::net::Address::fromString(text.substr(0, space), static_cast<uint16_t>(port));
    };

    result->engine = std::make_unique<Engine>(DeviceIdentity{static_cast<uint64_t>(id), fromBytes(env, name)}, speakerSettings(layout, left, right),
        manualOffset, std::move(hooks));
    return reinterpret_cast<jlong>(result);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeDestroy(JNIEnv* env, jclass, jlong handle) {
    Bridge* target = bridge(handle);
    target->engine.reset();
    env->DeleteGlobalRef(target->callbacks);
    delete target;
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetRole(JNIEnv*, jclass, jlong handle, jint role) {
    bridge(handle)->engine->setRole(static_cast<GroupSession::Role>(role));
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetIdentity(JNIEnv* env, jclass, jlong handle, jlong id, jbyteArray name) {
    bridge(handle)->engine->session().setIdentity(DeviceIdentity{static_cast<uint64_t>(id), fromBytes(env, name)});
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeFollow(JNIEnv* env, jclass, jlong handle, jbyteArray leader) {
    bridge(handle)->engine->follow(fromBytes(env, leader));
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeLoadTrack(JNIEnv* env, jclass, jlong handle, jstring path,
    jbyteArray name, jbyteArray digest) {
    uint8_t bytes[32] = {};
    if (digest != nullptr && env->GetArrayLength(digest) == 32) env->GetByteArrayRegion(digest, 0, 32, reinterpret_cast<jbyte*>(bytes));
    const std::string error = bridge(handle)->engine->loadTrack(toString(env, path), fromBytes(env, name), bytes);
    return error.empty() ? nullptr : toBytes(env, error);
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativePlay(JNIEnv*, jclass, jlong handle, jlong source) {
    bridge(handle)->engine->session().play(static_cast<uint64_t>(source));
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeStop(JNIEnv*, jclass, jlong handle) {
    bridge(handle)->engine->session().stopPlayback();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetLayout(JNIEnv*, jclass, jlong handle, jint layout) {
    if (layout < 0 || layout > asyncmusic::kSpeakerLayoutMaximum) return;
    bridge(handle)->engine->session().setSpeakerLayout(static_cast<SpeakerLayout>(layout));
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeAssign(JNIEnv*, jclass, jlong handle, jlong device, jint channel) {
    if (channel <= 0 || channel > asyncmusic::kSpeakerChannelMaximum) return;
    const std::optional<uint64_t> target = device == 0 ? std::nullopt : std::optional<uint64_t>(static_cast<uint64_t>(device));
    bridge(handle)->engine->session().assign(target, static_cast<SpeakerChannel>(channel));
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSwapChannels(JNIEnv*, jclass, jlong handle) {
    bridge(handle)->engine->session().swapChannels();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeClearLog(JNIEnv*, jclass, jlong handle) {
    bridge(handle)->engine->session().clearLog();
}

extern "C" JNIEXPORT void JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSetManualOffset(JNIEnv*, jclass, jlong handle, jdouble milliseconds) {
    bridge(handle)->engine->setManualOffset(milliseconds);
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeSnapshot(JNIEnv* env, jclass, jlong handle) {
    return toBytes(env, bridge(handle)->engine->snapshotJSON());
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_me_ktiays_asyncmusic_engine_NativeEngine_nativeLive(JNIEnv* env, jclass, jlong handle) {
    return toBytes(env, bridge(handle)->engine->liveJSON());
}

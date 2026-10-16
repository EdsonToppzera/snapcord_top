#pragma once

#include <QString>

// Voice & audio preferences, persisted with QSettings.
struct VoiceSettings
{
    enum class InputMode { VoiceActivity, PushToTalk };

    QString inputDevice;  // empty = system default
    QString outputDevice; // empty = system default
    InputMode inputMode = InputMode::VoiceActivity;
    float activationThresholdDb = -50.0f; // voice activity threshold, in dBFS
    int pushToTalkKey = 0;                // native key code, 0 = not set
    int pushToTalkReleaseMs = 200;        // keeps transmitting briefly after the key is released
    int toggleMuteKey = 0;                // native key code to toggle microphone mute, 0 = not set
    float inputVolume = 1.0f;             // 0..2
    float outputVolume = 1.0f;            // 0..2
    bool automaticSensitivity = true;     // voice detection decides instead of the dB threshold
    bool noiseSuppression = true;
    bool echoCancellation = false;
    bool automaticGainControl = false;
    bool soundEffects = true;
    bool participantMuteSounds = false; // a sound when someone else in the call mutes or unmutes

    static VoiceSettings load();
    void save() const;

    // Per-user playback volume (0..2, 1 = unchanged); 0 also acts as a local mute.
    static float userVolume(const QString& userId);
    static void setUserVolume(const QString& userId, float volume);
};

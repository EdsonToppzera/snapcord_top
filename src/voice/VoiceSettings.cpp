#include "voice/VoiceSettings.h"

#include <QSettings>

VoiceSettings VoiceSettings::load()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("voice"));
    VoiceSettings result;
    result.inputDevice = settings.value(QStringLiteral("inputDevice")).toString();
    result.outputDevice = settings.value(QStringLiteral("outputDevice")).toString();
    result.inputMode = settings.value(QStringLiteral("inputMode")).toString() == u"pushToTalk"
        ? InputMode::PushToTalk
        : InputMode::VoiceActivity;
    result.activationThresholdDb = settings.value(QStringLiteral("activationThresholdDb"), -50.0).toFloat();
    result.pushToTalkKey = settings.value(QStringLiteral("pushToTalkKey"), 0).toInt();
    result.pushToTalkReleaseMs = settings.value(QStringLiteral("pushToTalkReleaseMs"), 200).toInt();
    result.toggleMuteKey = settings.value(QStringLiteral("toggleMuteKey"), 0).toInt();
    result.inputVolume = settings.value(QStringLiteral("inputVolume"), 1.0).toFloat();
    result.outputVolume = settings.value(QStringLiteral("outputVolume"), 1.0).toFloat();
    result.automaticSensitivity = settings.value(QStringLiteral("automaticSensitivity"), true).toBool();
    result.noiseSuppression = settings.value(QStringLiteral("noiseSuppression"), true).toBool();
    result.echoCancellation = settings.value(QStringLiteral("echoCancellation"), false).toBool();
    result.automaticGainControl = settings.value(QStringLiteral("automaticGainControl"), false).toBool();
    result.soundEffects = settings.value(QStringLiteral("soundEffects"), true).toBool();
    result.participantMuteSounds = settings.value(QStringLiteral("participantMuteSounds"), false).toBool();
    return result;
}

void VoiceSettings::save() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("voice"));
    settings.setValue(QStringLiteral("inputDevice"), inputDevice);
    settings.setValue(QStringLiteral("outputDevice"), outputDevice);
    settings.setValue(QStringLiteral("inputMode"),
                      inputMode == InputMode::PushToTalk ? QStringLiteral("pushToTalk") : QStringLiteral("voiceActivity"));
    settings.setValue(QStringLiteral("activationThresholdDb"), activationThresholdDb);
    settings.setValue(QStringLiteral("pushToTalkKey"), pushToTalkKey);
    settings.setValue(QStringLiteral("pushToTalkReleaseMs"), pushToTalkReleaseMs);
    settings.setValue(QStringLiteral("toggleMuteKey"), toggleMuteKey);
    settings.setValue(QStringLiteral("inputVolume"), inputVolume);
    settings.setValue(QStringLiteral("outputVolume"), outputVolume);
    settings.setValue(QStringLiteral("automaticSensitivity"), automaticSensitivity);
    settings.setValue(QStringLiteral("noiseSuppression"), noiseSuppression);
    settings.setValue(QStringLiteral("echoCancellation"), echoCancellation);
    settings.setValue(QStringLiteral("automaticGainControl"), automaticGainControl);
    settings.setValue(QStringLiteral("soundEffects"), soundEffects);
    settings.setValue(QStringLiteral("participantMuteSounds"), participantMuteSounds);
}

float VoiceSettings::userVolume(const QString& userId)
{
    return QSettings().value(QStringLiteral("voice/userVolumes/") + userId, 1.0).toFloat();
}

void VoiceSettings::setUserVolume(const QString& userId, float volume)
{
    QSettings settings;
    const QString key = QStringLiteral("voice/userVolumes/") + userId;
    if (qFuzzyCompare(volume, 1.0f))
        settings.remove(key);
    else
        settings.setValue(key, volume);
}

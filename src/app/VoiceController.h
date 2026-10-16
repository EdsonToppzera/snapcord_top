#pragma once

#include "voice/SoundEffects.h"
#include "voice/VoiceConnection.h"

#include <QObject>
#include <QSet>
#include <QTimer>

class Session;
struct VoiceState;

// Joins and leaves voice channels and private calls: asks the Gateway for a voice server, then hands the
// credentials to the VoiceConnection. Also keeps the self mute/deafen state in sync between Discord and
// the audio pipeline, and plays the interface sounds.
class VoiceController : public QObject
{
    Q_OBJECT

public:
    explicit VoiceController(Session* session, QObject* parent = nullptr);
    ~VoiceController() override;

    // An empty guildId means a direct message or group DM call.
    void join(const QString& guildId, const QString& channelId);
    // Joins a private call and rings the other recipients if nobody is in it yet.
    void startCall(const QString& channelId);
    void leave();

    QString guildId() const { return m_guildId; }
    QString channelId() const { return m_channelId; }
    VoiceConnection::State state() const { return m_demoCall ? VoiceConnection::State::Connected : m_connection->state(); }
    // Demo mode: shows a connected call with these users speaking, without any audio or network.
    void showDemoCall(const QString& guildId, const QString& channelId, const QStringList& speaking,
                      const QList<int>& pingHistory);
    VoiceConnection* connection() const { return m_connection; }
    SoundEffects* sounds() const { return m_sounds; }
    Session* session() const { return m_session; }

    bool isSelfMuted() const { return m_selfMuted; }
    bool isSelfDeafened() const { return m_selfDeafened; }
    // Same behavior as Discord: deafening also mutes, undeafening restores the previous mute state,
    // and unmuting while deafened undeafens too.
    void toggleMute();
    void toggleDeafen();

    bool isSpeaking(const QString& userId) const { return m_speaking.contains(userId); }
    float userVolume(const QString& userId) const;
    void setUserVolume(const QString& userId, float volume);

    // Recent voice server round trips (oldest first), for the connection info panel.
    const QList<int>& pingHistory() const { return m_pingHistory; }

    void applySettings(const VoiceSettings& settings);

signals:
    void channelChanged();
    void stateChanged(VoiceConnection::State state);
    void selfStateChanged();
    void speakingChanged(const QString& userId, bool speaking);
    void pingChanged(int milliseconds);
    void errorOccurred(const QString& message);

private:
    void onOwnVoiceState(const VoiceState& state);
    void onVoiceServer(const QString& guildId, const QString& channelId, const QString& endpoint, const QString& token);
    void maybeConnect();
    void sendVoiceState();
    void setSelfState(bool muted, bool deafened);
    void resetChannel();
    void updateParticipants();
    void updateHotkeyTimer();
    void pollHotkeys();

    Session* m_session;
    VoiceConnection* m_connection;
    SoundEffects* m_sounds;
    QString m_guildId;
    QString m_channelId;
    QString m_endpoint;
    QString m_voiceToken;
    bool m_haveVoiceState = false;
    bool m_ringWhenJoined = false;
    bool m_selfMuted = false;
    bool m_selfDeafened = false;
    bool m_mutedBeforeDeafen = false;
    QSet<QString> m_speaking;
    QSet<QString> m_participants; // other users in our channel, to play join/leave sounds
    QSet<QString> m_mutedParticipants; // the muted ones among them, for the optional mute/unmute sounds
    bool m_participantMuteSounds = false;
    bool m_participantsKnown = false;
    QList<int> m_pingHistory;
    bool m_demoCall = false;
    QTimer m_hotkeyTimer;
    int m_toggleMuteKey = 0;
    bool m_toggleMuteKeyDown = false;
};

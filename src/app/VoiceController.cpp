#include "VoiceController.h"

#include "core/Session.h"
#include "platform/KeyState.h"

#include <QApplication>
#include <QSettings>
#include <QWidget>

namespace {

constexpr int PingHistorySize = 60;
constexpr int HotkeyPollIntervalMs = 30;

} // namespace

VoiceController::VoiceController(Session* session, QObject* parent)
    : QObject(parent)
    , m_session(session)
    , m_connection(new VoiceConnection(this))
    , m_sounds(new SoundEffects(this))
{
    // Discord remembers mute/deafen across restarts; so does Snapcord.
    QSettings settings;
    m_selfMuted = settings.value(QStringLiteral("voice/selfMuted"), false).toBool();
    m_selfDeafened = settings.value(QStringLiteral("voice/selfDeafened"), false).toBool();
    m_connection->setSelfMuted(m_selfMuted);
    m_connection->setSelfDeafened(m_selfDeafened);

    const VoiceSettings voiceSettings = VoiceSettings::load();
    m_sounds->setEnabled(voiceSettings.soundEffects);
    m_sounds->setOutputDevice(voiceSettings.outputDevice);
    m_sounds->setVolume(voiceSettings.outputVolume);
    m_sounds->loadStyles();
    m_participantMuteSounds = voiceSettings.participantMuteSounds;
    m_toggleMuteKey = voiceSettings.toggleMuteKey;
    m_hotkeyTimer.setInterval(HotkeyPollIntervalMs);
    connect(&m_hotkeyTimer, &QTimer::timeout, this, &VoiceController::pollHotkeys);
    updateHotkeyTimer();

    connect(m_session, &Session::ownVoiceStateChanged, this, &VoiceController::onOwnVoiceState);
    connect(m_session, &Session::voiceServerUpdated, this, &VoiceController::onVoiceServer);
    connect(m_session, &Session::connectionStateChanged, this, [this](bool connected) {
        // After a Gateway reconnect, tell Discord again where we are (the voice connection itself survives).
        if (connected && !m_channelId.isEmpty())
            sendVoiceState();
    });
    connect(m_session, &Session::voiceStatesChanged, this, [this](const QString& guildId) {
        if (!m_guildId.isEmpty() && guildId == m_guildId)
            updateParticipants();
    });
    connect(m_session, &Session::callChanged, this, [this](const QString& channelId) {
        if (m_guildId.isEmpty() && channelId == m_channelId)
            updateParticipants();
    });

    connect(m_connection, &VoiceConnection::stateChanged, this, [this](VoiceConnection::State state) {
        if (state == VoiceConnection::State::Connected) {
            m_sounds->play(SoundEffects::Sound::Join);
            updateParticipants();
        }
        emit stateChanged(state);
    });
    connect(m_connection, &VoiceConnection::pingChanged, this, [this](int milliseconds) {
        m_pingHistory.append(milliseconds);
        if (m_pingHistory.size() > PingHistorySize)
            m_pingHistory.removeFirst();
        emit pingChanged(milliseconds);
    });
    connect(m_connection, &VoiceConnection::speakingChanged, this, [this](const QString& userId, bool speaking) {
        if (speaking)
            m_speaking.insert(userId);
        else
            m_speaking.remove(userId);
        emit speakingChanged(userId, speaking);
    });
    connect(m_connection, &VoiceConnection::failed, this, [this](const QString& reason) {
        if (!m_channelId.isEmpty()) {
            leave();
            emit errorOccurred(reason);
        }
    });
}

VoiceController::~VoiceController()
{
    if (!m_channelId.isEmpty())
        m_session->updateVoiceState(m_guildId, QString(), m_selfMuted, m_selfDeafened);
}

void VoiceController::join(const QString& guildId, const QString& channelId)
{
    if (guildId == m_guildId && channelId == m_channelId)
        return;
    if (!m_channelId.isEmpty() && (guildId != m_guildId || guildId.isEmpty())) {
        // Moving to another guild or call means another voice server: drop the current connection first.
        m_connection->disconnect();
    }
    m_guildId = guildId;
    m_channelId = channelId;
    m_endpoint.clear();
    m_voiceToken.clear();
    m_haveVoiceState = false;
    m_ringWhenJoined = false;
    m_participants.clear();
    m_mutedParticipants.clear();
    m_participantsKnown = false;
    m_pingHistory.clear();
    sendVoiceState();
    emit channelChanged();
}

void VoiceController::startCall(const QString& channelId)
{
    const Call* call = m_session->call(channelId);
    const bool callRunning = call && !call->voiceStates.isEmpty();
    join(QString(), channelId);
    // Discord creates the call when the first person joins; the caller then rings everyone else.
    m_ringWhenJoined = !callRunning;
}

void VoiceController::showDemoCall(const QString& guildId, const QString& channelId, const QStringList& speaking,
                                   const QList<int>& pingHistory)
{
    m_demoCall = true;
    m_guildId = guildId;
    m_channelId = channelId;
    m_pingHistory = pingHistory;
    m_speaking = QSet<QString>(speaking.cbegin(), speaking.cend());
    emit channelChanged();
    for (const QString& userId : speaking)
        emit speakingChanged(userId, true);
    if (!pingHistory.isEmpty())
        emit pingChanged(pingHistory.last());
}

void VoiceController::leave()
{
    if (m_channelId.isEmpty())
        return;
    m_connection->disconnect();
    m_session->updateVoiceState(m_guildId, QString(), m_selfMuted, m_selfDeafened);
    m_sounds->play(SoundEffects::Sound::Leave);
    resetChannel();
    emit channelChanged();
}

void VoiceController::resetChannel()
{
    m_guildId.clear();
    m_channelId.clear();
    m_endpoint.clear();
    m_voiceToken.clear();
    m_haveVoiceState = false;
    m_ringWhenJoined = false;
    m_participants.clear();
    m_mutedParticipants.clear();
    m_participantsKnown = false;
    m_demoCall = false;
    m_speaking.clear();
}

void VoiceController::toggleMute()
{
    if (m_selfDeafened)
        setSelfState(false, false);
    else
        setSelfState(!m_selfMuted, false);
}

void VoiceController::toggleDeafen()
{
    if (m_selfDeafened) {
        setSelfState(m_mutedBeforeDeafen, false);
    } else {
        m_mutedBeforeDeafen = m_selfMuted;
        setSelfState(true, true);
    }
}

void VoiceController::setSelfState(bool muted, bool deafened)
{
    if (deafened != m_selfDeafened)
        m_sounds->play(deafened ? SoundEffects::Sound::Deafen : SoundEffects::Sound::Undeafen);
    else if (muted != m_selfMuted)
        m_sounds->play(muted ? SoundEffects::Sound::Mute : SoundEffects::Sound::Unmute);

    m_selfMuted = muted;
    m_selfDeafened = deafened;
    m_connection->setSelfMuted(muted);
    m_connection->setSelfDeafened(deafened);
    QSettings settings;
    settings.setValue(QStringLiteral("voice/selfMuted"), muted);
    settings.setValue(QStringLiteral("voice/selfDeafened"), deafened);
    if (!m_channelId.isEmpty())
        sendVoiceState();
    emit selfStateChanged();
}

float VoiceController::userVolume(const QString& userId) const
{
    return VoiceSettings::userVolume(userId);
}

void VoiceController::setUserVolume(const QString& userId, float volume)
{
    VoiceSettings::setUserVolume(userId, volume);
    m_connection->setUserVolume(userId, volume);
}

void VoiceController::applySettings(const VoiceSettings& settings)
{
    settings.save();
    m_connection->applySettings(settings);
    m_sounds->setEnabled(settings.soundEffects);
    m_sounds->setOutputDevice(settings.outputDevice);
    m_sounds->setVolume(settings.outputVolume);
    m_participantMuteSounds = settings.participantMuteSounds;
    m_toggleMuteKey = settings.toggleMuteKey;
    m_toggleMuteKeyDown = m_toggleMuteKey != 0 && KeyState::isDown(m_toggleMuteKey);
    updateHotkeyTimer();
}

void VoiceController::sendVoiceState()
{
    m_session->updateVoiceState(m_guildId, m_channelId, m_selfMuted, m_selfDeafened);
}

void VoiceController::updateParticipants()
{
    if (m_channelId.isEmpty() || m_connection->state() != VoiceConnection::State::Connected)
        return;
    QSet<QString> current;
    QSet<QString> muted;
    for (const VoiceState& state : m_session->voiceStatesInChannel(m_guildId, m_channelId)) {
        if (state.userId == m_session->self().id)
            continue;
        current.insert(state.userId);
        if (state.selfMute || state.mute)
            muted.insert(state.userId);
    }
    // The first snapshot after connecting is just who was already there: no sound for it.
    if (m_participantsKnown) {
        // Mute changes only count for people who were already here and still are.
        const QSet<QString> stayed = current & m_participants;
        if (!(current - m_participants).isEmpty())
            m_sounds->play(SoundEffects::Sound::UserJoin);
        else if (!(m_participants - current).isEmpty())
            m_sounds->play(SoundEffects::Sound::UserLeave);
        else if (m_participantMuteSounds && !((muted - m_mutedParticipants) & stayed).isEmpty())
            m_sounds->play(SoundEffects::Sound::UserMute);
        else if (m_participantMuteSounds && !((m_mutedParticipants - muted) & stayed).isEmpty())
            m_sounds->play(SoundEffects::Sound::UserUnmute);
    }
    m_participants = current;
    m_mutedParticipants = muted;
    m_participantsKnown = true;
}

void VoiceController::onOwnVoiceState(const VoiceState& state)
{
    if (m_channelId.isEmpty())
        return;
    if (state.channelId.isEmpty()) {
        // Disconnected by someone else (kicked, or the channel was deleted).
        m_connection->disconnect();
        m_sounds->play(SoundEffects::Sound::Leave);
        resetChannel();
        emit channelChanged();
        return;
    }
    if (state.channelId != m_channelId) {
        // Moved to another channel by a moderator; a new voice server update follows.
        m_channelId = state.channelId;
        m_participants.clear();
        m_mutedParticipants.clear();
        m_participantsKnown = false;
        emit channelChanged();
    }
    if (m_ringWhenJoined && m_guildId.isEmpty()) {
        m_ringWhenJoined = false;
        m_session->ringCall(m_channelId);
    }
    m_haveVoiceState = true;
    maybeConnect();
}

void VoiceController::onVoiceServer(const QString& guildId, const QString& channelId, const QString& endpoint,
                                    const QString& token)
{
    if (m_channelId.isEmpty())
        return;
    // Guild voice servers are identified by guild; private calls by their channel.
    const bool ours = m_guildId.isEmpty() ? channelId == m_channelId : guildId == m_guildId;
    if (!ours)
        return;
    m_endpoint = endpoint;
    m_voiceToken = token;
    maybeConnect();
}

void VoiceController::maybeConnect()
{
    // Both the voice state (session ID) and the voice server (endpoint, token) are needed to connect.
    if (!m_haveVoiceState || m_endpoint.isEmpty() || m_voiceToken.isEmpty())
        return;

    VoiceGateway::Credentials credentials;
    credentials.endpoint = m_endpoint;
    credentials.serverId = m_guildId.isEmpty() ? m_channelId : m_guildId;
    credentials.userId = m_session->self().id;
    credentials.sessionId = m_session->sessionId();
    credentials.token = m_voiceToken;
    // Voice tokens are single-use: a reconnect needs a new voice server update.
    m_voiceToken.clear();
    m_connection->connectTo(credentials, m_channelId);
}

void VoiceController::updateHotkeyTimer()
{
    if (KeyState::isSupported() && m_toggleMuteKey != 0) {
        if (!m_hotkeyTimer.isActive()) {
            m_toggleMuteKeyDown = KeyState::isDown(m_toggleMuteKey);
            m_hotkeyTimer.start();
        }
    } else {
        m_hotkeyTimer.stop();
        m_toggleMuteKeyDown = false;
    }
}

void VoiceController::pollHotkeys()
{
    if (m_toggleMuteKey == 0)
        return;
    const bool down = KeyState::isDown(m_toggleMuteKey);
    if (QWidget* focused = QApplication::focusWidget();
        focused && focused->property("recordingKeybind").toBool()) {
        m_toggleMuteKeyDown = down;
        return;
    }
    if (down && !m_toggleMuteKeyDown)
        toggleMute();
    m_toggleMuteKeyDown = down;
}

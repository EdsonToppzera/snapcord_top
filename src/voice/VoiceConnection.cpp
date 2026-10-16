#include "voice/VoiceConnection.h"

#include "core/Log.h"
#include "platform/KeyState.h"
#include "voice/DaveSession.h"
#include "voice/Rtp.h"
#include "voice/TransportCipher.h"

#include <QDebug>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>

namespace {

constexpr size_t FrameSamples = OpusFormat::FrameSamples;
constexpr size_t StereoFrameSamples = FrameSamples * OpusFormat::Channels;
// Opus "silence" frame: sent five times when a speaker stops so receivers don't interpolate.
constexpr uint8_t SilenceFrame[3] = {0xF8, 0xFF, 0xFE};
constexpr int SilenceFramesAfterSpeech = 5;
// Voice activity stays on for 300 ms after the level drops below the threshold.
constexpr int VoiceHangoverFrames = 15;
// Automatic input sensitivity: RNNoise voice probability needed to transmit, and a level floor.
constexpr float VoiceProbabilityThreshold = 0.5f;
constexpr float AutomaticSensitivityFloorDb = -70.0f;
// A remote user counts as speaking for this long after their last audio packet.
constexpr int64_t SpeakingTimeoutMs = 250;
constexpr int64_t KeepAliveIntervalMs = 5000;
// How often packets from an SSRC without a known sender are tried against the participants' keys.
constexpr int64_t IdentifyIntervalMs = 200;
constexpr uint32_t UdpPingMagic = 0x1337CAFE;
constexpr int SpeakingFlagVoice = 1 << 0;

// Mix-bus limiter: the peak it allows, how fast the gain comes down (samples) and goes back up (per frame).
constexpr float LimiterCeiling = 0.9f;
constexpr float LimiterAttackSamples = 96.0f; // 2 ms
constexpr float LimiterRelease = 0.02f;       // back to full volume within about a second

bool isSilenceFrame(const uint8_t* data, size_t size)
{
    return size == sizeof(SilenceFrame) && std::memcmp(data, SilenceFrame, sizeof(SilenceFrame)) == 0;
}

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct VoiceConnection::Stream
{
    QString userId;
    JitterBuffer buffer;
    OpusDecoderWrapper decoder;
    std::atomic<float> volume{1.0f};
    std::atomic<int64_t> lastVoiceMs{0};
    // Playback thread: the last decoded packet, handed to the mix 20 ms at a time (packets can be longer).
    std::vector<uint8_t> packet;
    std::vector<float> pcm = std::vector<float>(static_cast<size_t>(OpusFormat::MaxFrameSamples) * OpusFormat::Channels);
    size_t pcmSamples = 0; // per channel
    size_t pcmOffset = 0;
    bool pcmIsVoice = false;
    int lastFrameSamples = OpusFormat::FrameSamples; // duration of a lost packet, for concealment
    bool formatLogged = false; // receive thread
};

VoiceConnection::VoiceConnection(QObject* parent)
    : QObject(parent)
    , m_processChunk(AudioProcessor::FrameSize)
    , m_echoChunk(AudioProcessor::FrameSize)
    , m_captureFrame(FrameSamples)
    , m_preRollFrame(FrameSamples)
    , m_stereoFrame(StereoFrameSamples)
    , m_opusPacket(OpusFormat::MaxPacketSize)
    , m_mixFrame(StereoFrameSamples)
    , m_decodeFrame(StereoFrameSamples)
    , m_mixReadOffset(StereoFrameSamples)
{
    m_mixStreams.reserve(64);
    m_speakingTimer.setInterval(100);
    connect(&m_speakingTimer, &QTimer::timeout, this, &VoiceConnection::updateSpeakingIndicators);

    connect(&m_gateway, &VoiceGateway::ready, this, &VoiceConnection::onReady);
    connect(&m_gateway, &VoiceGateway::sessionDescription, this, &VoiceConnection::onSessionDescription);
    connect(&m_gateway, &VoiceGateway::speaking, this, &VoiceConnection::onSpeaking);
    connect(&m_gateway, &VoiceGateway::clientsConnected, this, [this](const QStringList& userIds) {
        if (m_dave) {
            for (const QString& userId : userIds)
                m_dave->addUser(userId);
        }
    });
    connect(&m_gateway, &VoiceGateway::clientDisconnected, this, &VoiceConnection::onClientDisconnected);
    connect(&m_gateway, &VoiceGateway::daveJson, this, [this](int op, const QJsonObject& data) {
        if (m_dave)
            m_dave->handleJson(op, data);
    });
    connect(&m_gateway, &VoiceGateway::daveBinary, this, [this](int op, const QByteArray& payload) {
        if (m_dave)
            m_dave->handleBinary(op, payload);
    });
    connect(&m_gateway, &VoiceGateway::pingChanged, this, &VoiceConnection::pingChanged);
    connect(&m_gateway, &VoiceGateway::closed, this, &VoiceConnection::onClosed);

    applySettings(VoiceSettings::load());
}

VoiceConnection::~VoiceConnection()
{
    m_gateway.close();
    stopMedia();
}

void VoiceConnection::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    qCInfo(lcVoice) << "voice state" << state;
    emit stateChanged(state);
}

void VoiceConnection::connectTo(const VoiceGateway::Credentials& credentials, const QString& channelId)
{
    m_gateway.close();
    stopMedia();

    m_credentials = credentials;
    m_channelId = channelId;
    m_dave = std::make_unique<DaveSession>(
        credentials.userId, channelId,
        [this](int op, const QJsonObject& data) { m_gateway.sendJson(op, data); },
        [this](int op, const QByteArray& payload) { m_gateway.sendBinary(op, payload); });

    setState(State::Connecting);
    m_gateway.open(credentials, DaveSession::maxSupportedProtocolVersion());
}

void VoiceConnection::disconnect()
{
    m_gateway.close();
    stopMedia();
    m_dave.reset();
    setState(State::Disconnected);
}

void VoiceConnection::setSelfMuted(bool muted)
{
    m_selfMuted.store(muted);
}

void VoiceConnection::setSelfDeafened(bool deafened)
{
    m_selfDeafened.store(deafened);
}

void VoiceConnection::applySettings(const VoiceSettings& settings)
{
    m_pushToTalk.store(settings.inputMode == VoiceSettings::InputMode::PushToTalk);
    m_thresholdDb.store(settings.activationThresholdDb);
    m_pushToTalkKey.store(settings.pushToTalkKey);
    m_pushToTalkReleaseMs.store(settings.pushToTalkReleaseMs);
    m_inputGain.store(settings.inputVolume);
    m_outputGain.store(settings.outputVolume);
    m_automaticSensitivity.store(settings.automaticSensitivity);
    m_noiseSuppression.store(settings.noiseSuppression);
    m_echoCancellation.store(settings.echoCancellation);
    m_automaticGainControl.store(settings.automaticGainControl);
    // The processor is rebuilt on the capture thread, which owns it.
    m_processorConfigVersion.fetch_add(1);

    const bool devicesChanged = settings.inputDevice != m_inputDevice || settings.outputDevice != m_outputDevice;
    m_inputDevice = settings.inputDevice;
    m_outputDevice = settings.outputDevice;
    if (devicesChanged && m_mediaRunning)
        startAudioDevices();
}

void VoiceConnection::setUserVolume(const QString& userId, float volume)
{
    m_userVolumes.insert(userId, volume);
    std::lock_guard lock(m_streamsMutex);
    for (auto& [ssrc, stream] : m_streams) {
        if (stream->userId == userId)
            stream->volume.store(volume);
    }
}

void VoiceConnection::onReady(quint32 ssrc, const QString& ip, quint16 port, const QStringList& modes)
{
    m_ssrc = ssrc;
    const auto mode = TransportCipher::choose(modes);
    if (!mode) {
        disconnect();
        emit failed(tr("The voice server offered no supported encryption mode."));
        return;
    }
    m_encryptionMode = TransportCipher::name(*mode);

    if (!m_udp.connectTo(ip.toStdString(), port)) {
        disconnect();
        emit failed(tr("Could not reach the voice server."));
        return;
    }

    // IP discovery blocks for a moment, so it runs off the main thread.
    if (m_discoveryThread.joinable())
        m_discoveryThread.join();
    m_discoveryThread = std::thread([this, ssrc] {
        const auto endpoint = m_udp.discoverPublicEndpoint(ssrc, 4000);
        QMetaObject::invokeMethod(this, [this, endpoint] {
            if (m_state != State::Connecting)
                return;
            if (!endpoint) {
                qCWarning(lcVoice) << "IP discovery failed";
                disconnect();
                emit failed(tr("Could not establish the voice connection. A firewall may be blocking UDP traffic."));
                return;
            }
            qCInfo(lcVoice) << "IP discovery done";
            m_gateway.selectProtocol(QString::fromStdString(endpoint->address), endpoint->port, m_encryptionMode);
        }, Qt::QueuedConnection);
    });
}

void VoiceConnection::onSessionDescription(const QString& mode, const QByteArray& secretKey, int daveProtocolVersion)
{
    const auto cipherMode = TransportCipher::choose({mode});
    if (!cipherMode || secretKey.size() != 32 || !m_dave) {
        disconnect();
        emit failed(tr("The voice server sent an invalid session."));
        return;
    }
    if (m_mediaRunning) {
        // A new session description during a call only refreshes the protocol state.
        m_dave->start(daveProtocolVersion, m_ssrc);
        return;
    }

    std::array<uint8_t, 32> key;
    std::memcpy(key.data(), secretKey.constData(), key.size());
    m_cipher = std::make_unique<TransportCipher>(*cipherMode, key);
    m_daveProtocolVersion = daveProtocolVersion;
    m_dave->start(daveProtocolVersion, m_ssrc);

    // At least one Speaking payload must be sent before any audio.
    m_gateway.sendSpeaking(0, m_ssrc);
    startMedia();
    setState(State::Connected);
}

void VoiceConnection::onSpeaking(const QString& userId, quint32 ssrc, int)
{
    if (userId.isEmpty() || userId == m_credentials.userId)
        return;
    if (m_dave)
        m_dave->addUser(userId);

    std::lock_guard lock(m_streamsMutex);
    auto& stream = m_streams[ssrc];
    if (!stream || stream->userId != userId) {
        stream = std::make_shared<Stream>();
        stream->userId = userId;
        stream->volume.store(m_userVolumes.contains(userId) ? m_userVolumes.value(userId)
                                                             : VoiceSettings::userVolume(userId));
    }
}

void VoiceConnection::onClientDisconnected(const QString& userId)
{
    if (m_dave)
        m_dave->removeUser(userId);
    std::lock_guard lock(m_streamsMutex);
    for (auto it = m_streams.begin(); it != m_streams.end();) {
        if (it->second->userId == userId)
            it = m_streams.erase(it);
        else
            ++it;
    }
}

void VoiceConnection::onClosed(int code)
{
    stopMedia();
    m_dave.reset();
    setState(State::Disconnected);

    QString reason;
    switch (code) {
    case 4014:
        reason = tr("You were disconnected from the voice channel.");
        break;
    case 4017:
        reason = tr("This call requires end-to-end encryption, which could not be set up.");
        break;
    case 4006:
        reason = tr("The voice session expired. Please join the channel again.");
        break;
    default:
        reason = tr("The voice connection was lost (code %1).").arg(code);
        break;
    }
    emit failed(reason);
}

void VoiceConnection::startMedia()
{
    if (m_discoveryThread.joinable())
        m_discoveryThread.join();

    m_encoder = std::make_unique<OpusEncoderWrapper>();
    m_captureFill = 0;
    m_processFill = 0;
    m_hasPreRollFrame = false;
    m_frameVoiceProbability = -1.0f;
    m_echoReference.clear();
    m_inboundPacketLoss = 0.0;
    m_sequence = 0;
    m_timestamp = 0;
    m_transmitting = false;
    m_voiceHangoverFrames = 0;
    m_silenceFramesToSend = 0;
    m_mixReadOffset = m_mixFrame.size();

    m_identifyAttempts.clear();
    m_receiving.store(true);
    m_receiveThread = std::thread(&VoiceConnection::receiveLoop, this);
    m_mediaRunning = true;
    startAudioDevices();
    m_speakingTimer.start();
}

bool VoiceConnection::startAudioDevices()
{
    const bool captureOk = m_audio.startCapture(m_inputDevice, [this](const float* samples, int count) {
        captureSamples(samples, count);
    });
    const bool playbackOk = m_audio.startPlayback(m_outputDevice, [this](float* output, int count) {
        renderPlayback(output, count);
    });
    if (!captureOk)
        qWarning() << "Could not open the microphone" << m_inputDevice;
    if (!playbackOk)
        qWarning() << "Could not open the speakers" << m_outputDevice;
    return captureOk && playbackOk;
}

void VoiceConnection::stopMedia()
{
    m_speakingTimer.stop();
    m_audio.stopCapture();
    m_audio.stopPlayback();

    m_receiving.store(false);
    if (m_receiveThread.joinable())
        m_receiveThread.join();
    if (m_discoveryThread.joinable())
        m_discoveryThread.join();
    m_udp.close();

    {
        std::lock_guard lock(m_streamsMutex);
        m_streams.clear();
    }
    m_cipher.reset();
    m_encoder.reset();
    m_mediaRunning = false;
    m_selfSpeaking.store(false);
    m_inputLevelDb.store(-100.0f);

    for (auto it = m_speaking.cbegin(); it != m_speaking.cend(); ++it) {
        if (it.value())
            emit speakingChanged(it.key(), false);
    }
    m_speaking.clear();
    if (m_selfWasSpeaking) {
        m_selfWasSpeaking = false;
        emit speakingChanged(m_credentials.userId, false);
    }
}

VoiceConnection::ConnectionInfo VoiceConnection::connectionInfo()
{
    ConnectionInfo info;
    info.endpoint = m_credentials.endpoint;
    info.encryptionMode = m_encryptionMode;
    info.daveProtocolVersion = m_daveProtocolVersion;
    info.endToEndEncrypted = m_dave && m_dave->isEncrypting();
    info.inboundPacketLoss = m_inboundPacketLoss;
    return info;
}

void VoiceConnection::updateSpeakingIndicators()
{
    const int64_t now = nowMs();
    QSet<QString> speakingNow;
    {
        std::lock_guard lock(m_streamsMutex);
        for (const auto& [ssrc, stream] : m_streams) {
            if (now - stream->lastVoiceMs.load(std::memory_order_relaxed) < SpeakingTimeoutMs)
                speakingNow.insert(stream->userId);
        }
    }
    for (const QString& userId : speakingNow) {
        if (!m_speaking.value(userId)) {
            m_speaking.insert(userId, true);
            emit speakingChanged(userId, true);
        }
    }
    for (auto it = m_speaking.begin(); it != m_speaking.end(); ++it) {
        if (it.value() && !speakingNow.contains(it.key())) {
            it.value() = false;
            emit speakingChanged(it.key(), false);
        }
    }

    const bool selfSpeaking = m_selfSpeaking.load(std::memory_order_relaxed);
    if (selfSpeaking != m_selfWasSpeaking) {
        m_selfWasSpeaking = selfSpeaking;
        emit speakingChanged(m_credentials.userId, selfSpeaking);
    }

    // Inbound packet loss, from how many frames playback had to conceal (every 2 seconds).
    if (m_statisticsTicks % 20 == 0) {
        const uint32_t played = m_playedFrames.exchange(0);
        const uint32_t concealed = m_concealedFrames.exchange(0);
        if (played + concealed > 0)
            m_inboundPacketLoss = 100.0 * concealed / (played + concealed);
    }

    // Every 10 seconds (100 ticks of 100 ms).
    if (++m_statisticsTicks >= 100) {
        m_statisticsTicks = 0;
        qCInfo(lcVoice).nospace() << "media stats: sent " << m_stats.sent.exchange(0) << ", received "
                                  << m_stats.received.exchange(0) << ", transport failures "
                                  << m_stats.transportFailures.exchange(0) << ", E2EE encrypt failures "
                                  << m_stats.encryptFailures.exchange(0) << ", E2EE decrypt failures "
                                  << m_stats.decryptFailures.exchange(0) << ", unknown SSRC "
                                  << m_stats.unknownSsrc.exchange(0);
        qCInfo(lcVoice).nospace() << "microphone stats: voice frames " << m_stats.transmittedFrames.exchange(0)
                                  << ", muted frames " << m_stats.mutedFrames.exchange(0) << ", peak level "
                                  << qRound(m_stats.peakLevelDb.exchange(-100.0f)) << " dB, peak voice probability "
                                  << m_stats.peakVoiceProbability.exchange(-1.0f) << ", mode "
                                  << (m_pushToTalk ? "push to talk" : m_automaticSensitivity ? "automatic" : "threshold");
    }
}

// --- Capture thread ---------------------------------------------------------------------------------

void VoiceConnection::captureSamples(const float* samples, int count)
{
    // The processor works on 10 ms chunks; two processed chunks make one 20 ms Opus frame.
    while (count > 0) {
        const size_t chunk = std::min<size_t>(static_cast<size_t>(count), AudioProcessor::FrameSize - m_processFill);
        std::copy(samples, samples + chunk, m_processChunk.begin() + static_cast<ptrdiff_t>(m_processFill));
        m_processFill += chunk;
        samples += chunk;
        count -= static_cast<int>(chunk);
        if (m_processFill == AudioProcessor::FrameSize) {
            processChunk();
            m_processFill = 0;
        }
    }
}

void VoiceConnection::processChunk()
{
    const int configVersion = m_processorConfigVersion.load(std::memory_order_relaxed);
    if (configVersion != m_appliedProcessorConfig) {
        m_appliedProcessorConfig = configVersion;
        AudioProcessor::Options options;
        options.noiseSuppression = m_noiseSuppression.load();
        options.echoCancellation = m_echoCancellation.load();
        options.automaticGainControl = m_automaticGainControl.load();
        options.voiceDetection = m_automaticSensitivity.load();
        if (!(options == m_processor.options()))
            m_processor.configure(options);
    }

    const float gain = m_inputGain.load(std::memory_order_relaxed);
    for (float& sample : m_processChunk)
        sample *= gain;

    const float* reference = nullptr;
    if (m_processor.options().echoCancellation) {
        m_echoReference.pop(m_echoChunk.data());
        reference = m_echoChunk.data();
    }
    const float voice = m_processor.process(m_processChunk.data(), reference);
    m_frameVoiceProbability = std::max(m_frameVoiceProbability, voice);

    std::copy(m_processChunk.begin(), m_processChunk.end(),
              m_captureFrame.begin() + static_cast<ptrdiff_t>(m_captureFill));
    m_captureFill += m_processChunk.size();
    if (m_captureFill == FrameSamples) {
        processCaptureFrame();
        m_captureFill = 0;
        m_frameVoiceProbability = -1.0f;
    }
}

void VoiceConnection::processCaptureFrame()
{
    double energy = 0.0;
    for (const float sample : m_captureFrame)
        energy += double(sample) * sample;
    const double rms = std::sqrt(energy / FrameSamples);
    const float levelDb = rms > 1e-9 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f;
    m_inputLevelDb.store(levelDb, std::memory_order_relaxed);

    if (levelDb > m_stats.peakLevelDb.load(std::memory_order_relaxed))
        m_stats.peakLevelDb.store(levelDb, std::memory_order_relaxed);
    if (m_frameVoiceProbability > m_stats.peakVoiceProbability.load(std::memory_order_relaxed))
        m_stats.peakVoiceProbability.store(m_frameVoiceProbability, std::memory_order_relaxed);

    bool transmit = false;
    const bool muted = m_selfMuted.load(std::memory_order_relaxed) || m_selfDeafened.load(std::memory_order_relaxed);
    if (muted)
        m_stats.mutedFrames.fetch_add(1, std::memory_order_relaxed);
    if (!muted) {
        if (m_pushToTalk.load(std::memory_order_relaxed)) {
            const int key = m_pushToTalkKey.load(std::memory_order_relaxed);
            const int64_t now = nowMs();
            if (key != 0 && KeyState::isDown(key))
                m_pushToTalkUntilMs = now + m_pushToTalkReleaseMs.load(std::memory_order_relaxed);
            transmit = now < m_pushToTalkUntilMs;
        } else {
            // Automatic sensitivity trusts RNNoise's voice detection; the floor ignores near-silent frames.
            const bool voiceDetected = m_automaticSensitivity.load(std::memory_order_relaxed) && m_frameVoiceProbability >= 0.0f
                ? m_frameVoiceProbability >= VoiceProbabilityThreshold && levelDb > AutomaticSensitivityFloorDb
                : levelDb >= m_thresholdDb.load(std::memory_order_relaxed);
            if (voiceDetected)
                m_voiceHangoverFrames = VoiceHangoverFrames;
            else if (m_voiceHangoverFrames > 0)
                --m_voiceHangoverFrames;
            transmit = m_voiceHangoverFrames > 0;
        }
    }

    if (transmit) {
        if (!m_transmitting) {
            m_transmitting = true;
            m_silenceFramesToSend = 0;
            QMetaObject::invokeMethod(this, [this] { m_gateway.sendSpeaking(SpeakingFlagVoice, m_ssrc); },
                                      Qt::QueuedConnection);
            // Send the 20 ms frame captured right before VAD opened so the start of a word isn't clipped.
            if (m_hasPreRollFrame) {
                m_hasPreRollFrame = false;
                m_stats.transmittedFrames.fetch_add(1, std::memory_order_relaxed);
                encodeAndSend(m_preRollFrame, m_timestamp - static_cast<uint32_t>(FrameSamples));
            }
        }
        m_stats.transmittedFrames.fetch_add(1, std::memory_order_relaxed);
        encodeAndSend(m_captureFrame, m_timestamp);
    } else if (m_transmitting) {
        m_transmitting = false;
        m_hasPreRollFrame = false;
        m_silenceFramesToSend = SilenceFramesAfterSpeech;
        QMetaObject::invokeMethod(this, [this] { m_gateway.sendSpeaking(0, m_ssrc); }, Qt::QueuedConnection);
    }

    if (!transmit && m_silenceFramesToSend > 0) {
        --m_silenceFramesToSend;
        // Silence frames are not end-to-end encrypted: DAVE passes them through untouched.
        sendFrame(SilenceFrame, sizeof(SilenceFrame), m_timestamp, false);
    } else if (!transmit) {
        if (!muted) {
            std::copy(m_captureFrame.begin(), m_captureFrame.end(), m_preRollFrame.begin());
            m_hasPreRollFrame = true;
        } else {
            m_hasPreRollFrame = false;
        }
    }

    // The RTP timestamp reflects the 48 kHz sampling clock and must advance even during silent frames,
    // otherwise remote WebRTC receivers treat silent pauses as network jitter spikes and inflate their buffer.
    m_timestamp += static_cast<uint32_t>(FrameSamples);
    m_selfSpeaking.store(transmit, std::memory_order_relaxed);
}

void VoiceConnection::encodeAndSend(const std::vector<float>& monoFrame, uint32_t timestamp)
{
    for (size_t i = 0; i < FrameSamples; ++i) {
        const float sample = std::clamp(monoFrame[i], -1.0f, 1.0f);
        m_stereoFrame[2 * i] = sample;
        m_stereoFrame[2 * i + 1] = sample;
    }
    const int size = m_encoder ? m_encoder->encode(m_stereoFrame.data(), m_opusPacket.data(),
                                                   static_cast<int>(m_opusPacket.size()))
                               : 0;
    if (size > 0)
        sendFrame(m_opusPacket.data(), static_cast<size_t>(size), timestamp, true);
}

void VoiceConnection::sendFrame(const uint8_t* opus, size_t size, uint32_t timestamp, bool endToEndEncrypt)
{
    if (!m_cipher)
        return;
    const uint8_t* payload = opus;
    size_t payloadSize = size;
    if (endToEndEncrypt) {
        if (!m_dave || !m_dave->encrypt(m_ssrc, opus, size, m_davePacket)) {
            m_stats.encryptFailures.fetch_add(1, std::memory_order_relaxed);
            return; // no media key yet (the call is still negotiating encryption)
        }
        payload = m_davePacket.data();
        payloadSize = m_davePacket.size();
    }

    uint8_t header[Rtp::FixedHeaderSize];
    Rtp::writeHeader(header, m_sequence++, timestamp, m_ssrc);
    if (m_cipher->seal(header, sizeof(header), payload, payloadSize, m_udpPacket)
        && m_udp.send(m_udpPacket.data(), m_udpPacket.size())) {
        m_stats.sent.fetch_add(1, std::memory_order_relaxed);
    }
}

// --- Playback thread --------------------------------------------------------------------------------

void VoiceConnection::renderPlayback(float* output, int frameCount)
{
    float* const start = output;
    size_t needed = static_cast<size_t>(frameCount) * OpusFormat::Channels;
    while (needed > 0) {
        if (m_mixReadOffset >= m_mixFrame.size()) {
            mixNextFrame();
            m_mixReadOffset = 0;
        }
        const size_t chunk = std::min(needed, m_mixFrame.size() - m_mixReadOffset);
        std::memcpy(output, m_mixFrame.data() + m_mixReadOffset, chunk * sizeof(float));
        output += chunk;
        m_mixReadOffset += chunk;
        needed -= chunk;
    }
    // What the speakers play is the reference the echo canceller removes from the microphone.
    if (m_echoCancellation.load(std::memory_order_relaxed))
        m_echoReference.push(start, static_cast<size_t>(frameCount));
}

void VoiceConnection::mixNextFrame()
{
    std::fill(m_mixFrame.begin(), m_mixFrame.end(), 0.0f);
    {
        std::lock_guard lock(m_streamsMutex);
        for (const auto& [ssrc, stream] : m_streams)
            m_mixStreams.push_back(stream);
    }

    const bool deafened = m_selfDeafened.load(std::memory_order_relaxed);
    const float outputGain = m_outputGain.load(std::memory_order_relaxed);
    const int64_t now = nowMs();

    for (const auto& stream : m_mixStreams) {
        // Streams are always drained, even when deafened, so audio doesn't pile up.
        const size_t samples = readStream(*stream, now);
        const float gain = stream->volume.load(std::memory_order_relaxed) * outputGain;
        if (samples == 0 || deafened || gain <= 0.0f)
            continue;
        const size_t count = samples * OpusFormat::Channels;
        for (size_t i = 0; i < count && i < m_mixFrame.size(); ++i)
            m_mixFrame[i] += m_decodeFrame[i] * gain;
    }
    m_mixStreams.clear();
    limitMix();
}

size_t VoiceConnection::readStream(Stream& stream, int64_t now)
{
    size_t filled = 0;
    while (filled < FrameSamples) {
        if (stream.pcmOffset >= stream.pcmSamples && !decodeNext(stream))
            break;
        const size_t chunk = std::min(FrameSamples - filled, stream.pcmSamples - stream.pcmOffset);
        std::memcpy(m_decodeFrame.data() + filled * OpusFormat::Channels,
                    stream.pcm.data() + stream.pcmOffset * OpusFormat::Channels,
                    chunk * OpusFormat::Channels * sizeof(float));
        filled += chunk;
        stream.pcmOffset += chunk;
        if (stream.pcmIsVoice)
            stream.lastVoiceMs.store(now, std::memory_order_relaxed);
    }
    return filled;
}

bool VoiceConnection::decodeNext(Stream& stream)
{
    int samples = 0;
    stream.pcmIsVoice = false;
    switch (stream.buffer.pop(stream.packet)) {
    case JitterBuffer::Result::Packet:
        samples = stream.decoder.decode(stream.packet.data(), static_cast<int>(stream.packet.size()), stream.pcm.data(),
                                        OpusFormat::MaxFrameSamples);
        if (samples > 0)
            stream.lastFrameSamples = samples;
        stream.pcmIsVoice = !isSilenceFrame(stream.packet.data(), stream.packet.size());
        m_playedFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    case JitterBuffer::Result::Recover:
        samples = stream.decoder.recover(stream.packet.data(), static_cast<int>(stream.packet.size()), stream.pcm.data(),
                                         stream.lastFrameSamples);
        m_concealedFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    case JitterBuffer::Result::Lost:
        samples = stream.decoder.conceal(stream.pcm.data(), stream.lastFrameSamples);
        m_concealedFrames.fetch_add(1, std::memory_order_relaxed);
        break;
    case JitterBuffer::Result::Idle:
        break;
    }
    stream.pcmSamples = static_cast<size_t>(std::max(samples, 0));
    stream.pcmOffset = 0;
    return stream.pcmSamples > 0;
}

void VoiceConnection::limitMix()
{
    // Several loud speakers summed together easily pass full scale, and clipping that sum is what
    // crackles. The limiter lowers the gain just enough for the frame's peak, ramping it so the gain
    // itself never jumps, and recovers slowly afterwards.
    float peak = 0.0f;
    for (const float sample : m_mixFrame)
        peak = std::max(peak, std::abs(sample));
    const float target = peak > LimiterCeiling ? LimiterCeiling / peak : 1.0f;
    const float next = target < m_limiterGain ? target : std::min(target, m_limiterGain + LimiterRelease);

    const size_t frames = m_mixFrame.size() / OpusFormat::Channels;
    for (size_t i = 0; i < frames; ++i) {
        const float ramp = std::min(1.0f, static_cast<float>(i + 1) / LimiterAttackSamples);
        const float gain = m_limiterGain + (next - m_limiterGain) * ramp;
        for (int channel = 0; channel < OpusFormat::Channels; ++channel) {
            float& sample = m_mixFrame[i * OpusFormat::Channels + channel];
            sample *= gain;
            // Whatever still overshoots during the attack ramp is rounded off instead of cut flat.
            const float magnitude = std::abs(sample);
            if (magnitude > LimiterCeiling) {
                const float knee = 1.0f - LimiterCeiling;
                sample = std::copysign(LimiterCeiling + knee * std::tanh((magnitude - LimiterCeiling) / knee), sample);
            }
        }
    }
    m_limiterGain = next;
}

// --- Receive thread ---------------------------------------------------------------------------------

void VoiceConnection::receiveLoop()
{
    std::vector<uint8_t> buffer(2048);
    int64_t lastKeepAlive = 0;
    uint32_t pingSequence = 0;

    while (m_receiving.load(std::memory_order_relaxed)) {
        const int size = m_udp.receive(buffer.data(), buffer.size(), 100);
        if (size > 0)
            handlePacket(buffer.data(), static_cast<size_t>(size));

        // A small UDP ping keeps the NAT mapping alive while nobody is talking.
        const int64_t now = nowMs();
        if (now - lastKeepAlive >= KeepAliveIntervalMs) {
            lastKeepAlive = now;
            uint8_t ping[8];
            Rtp::writeU32(ping, UdpPingMagic);
            Rtp::writeU32(ping + 4, pingSequence++);
            m_udp.send(ping, sizeof(ping));
        }
    }
}

void VoiceConnection::handlePacket(const uint8_t* data, size_t size)
{
    thread_local std::vector<uint8_t> plaintext;
    thread_local std::vector<uint8_t> frame;

    const auto header = Rtp::parse(data, size);
    if (!header || header->payloadType != Rtp::OpusPayloadType || !m_cipher)
        return;
    m_stats.received.fetch_add(1, std::memory_order_relaxed);
    if (!m_cipher->open(data, size, header->clearSize, plaintext)) {
        m_stats.transportFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (plaintext.size() < header->extensionBodySize)
        return;
    const uint8_t* payload = plaintext.data() + header->extensionBodySize;
    const size_t payloadSize = plaintext.size() - header->extensionBodySize;

    std::shared_ptr<Stream> stream;
    {
        std::lock_guard lock(m_streamsMutex);
        const auto it = m_streams.find(header->ssrc);
        if (it != m_streams.end())
            stream = it->second;
    }
    if (!stream) {
        // No Speaking event has mapped this SSRC to a user yet. Usually it's on its way, but someone who
        // was already talking when we joined (a music bot streaming nonstop) may never send another one.
        m_stats.unknownSsrc.fetch_add(1, std::memory_order_relaxed);
        identifySender(header->ssrc, payload, payloadSize);
        return;
    }

    // The silence frames that end a sentence are not end-to-end encrypted. They still go through the
    // jitter buffer, so speech fades out cleanly instead of being stretched by packet loss concealment.
    if (isSilenceFrame(payload, payloadSize)) {
        stream->buffer.push(header->sequence, std::vector<uint8_t>(payload, payload + payloadSize));
        return;
    }
    if (!m_dave || !m_dave->decrypt(stream->userId, payload, payloadSize, frame)) {
        m_stats.decryptFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!stream->formatLogged) {
        stream->formatLogged = true;
        const int samples = OpusDecoderWrapper::packetSamples(frame.data(), static_cast<int>(frame.size()));
        qCInfo(lcVoice) << "receiving a stream with" << samples * 1000 / OpusFormat::SampleRate << "ms packets";
    }
    stream->buffer.push(header->sequence, frame);
}

void VoiceConnection::identifySender(uint32_t ssrc, const uint8_t* payload, size_t size)
{
    thread_local std::vector<uint8_t> frame;
    if (!m_dave || isSilenceFrame(payload, size))
        return;
    // Trying every user's key costs a few decryptions, so each unknown SSRC is tried a few times a second.
    const int64_t now = nowMs();
    int64_t& lastAttempt = m_identifyAttempts[ssrc];
    if (now - lastAttempt < IdentifyIntervalMs)
        return;
    lastAttempt = now;

    std::set<QString> mappedUsers;
    {
        std::lock_guard lock(m_streamsMutex);
        for (const auto& [streamSsrc, stream] : m_streams)
            mappedUsers.insert(stream->userId);
    }
    const QString userId = m_dave->identifySender(payload, size, mappedUsers, frame);
    if (userId.isEmpty())
        return;
    qCInfo(lcVoice) << "mapped an SSRC without a Speaking event to its sender";
    // The stream is created on the main thread like any other, with the user's saved volume.
    QMetaObject::invokeMethod(this, [this, userId, ssrc] { onSpeaking(userId, ssrc, 0); }, Qt::QueuedConnection);
}

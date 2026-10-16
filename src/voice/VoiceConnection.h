#pragma once

#include "voice/AudioEngine.h"
#include "voice/AudioProcessor.h"
#include "voice/JitterBuffer.h"
#include "voice/OpusCodec.h"
#include "voice/UdpSocket.h"
#include "voice/VoiceGateway.h"
#include "voice/VoiceSettings.h"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class DaveSession;
class TransportCipher;

// One voice call: signaling, UDP media transport, encryption and the audio pipeline.
//
// Threads: signaling runs on the main thread; microphone frames are encoded and sent from the capture
// callback; a receive thread decrypts incoming packets into per-speaker jitter buffers; the playback
// callback decodes and mixes them.
class VoiceConnection : public QObject
{
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Connected };
    Q_ENUM(State)

    explicit VoiceConnection(QObject* parent = nullptr);
    ~VoiceConnection() override;

    void connectTo(const VoiceGateway::Credentials& credentials, const QString& channelId);
    void disconnect();
    State state() const { return m_state; }

    void setSelfMuted(bool muted);
    void setSelfDeafened(bool deafened);
    void applySettings(const VoiceSettings& settings);
    void setUserVolume(const QString& userId, float volume);

    float inputLevelDb() const { return m_inputLevelDb.load(std::memory_order_relaxed); }
    AudioEngine& audio() { return m_audio; }

    // Details for the connection info panel.
    struct ConnectionInfo
    {
        QString endpoint;
        QString encryptionMode;
        int daveProtocolVersion = 0;
        bool endToEndEncrypted = false;
        double inboundPacketLoss = 0.0; // percent, over the last few seconds
    };
    ConnectionInfo connectionInfo();

signals:
    void stateChanged(VoiceConnection::State state);
    void speakingChanged(const QString& userId, bool speaking);
    void pingChanged(int milliseconds);
    void failed(const QString& reason);

private:
    struct Stream;

    void setState(State state);
    void onReady(quint32 ssrc, const QString& ip, quint16 port, const QStringList& modes);
    void onSessionDescription(const QString& mode, const QByteArray& secretKey, int daveProtocolVersion);
    void onSpeaking(const QString& userId, quint32 ssrc, int flags);
    void onClientDisconnected(const QString& userId);
    void onClosed(int code);
    void startMedia();
    void stopMedia();
    bool startAudioDevices();
    void updateSpeakingIndicators();

    // Audio and network threads.
    void captureSamples(const float* samples, int count);
    void processChunk();
    void processCaptureFrame();
    void encodeAndSend(const std::vector<float>& monoFrame, uint32_t timestamp);
    void sendFrame(const uint8_t* opus, size_t size, uint32_t timestamp, bool endToEndEncrypt);
    void renderPlayback(float* output, int frameCount);
    void mixNextFrame();
    // Puts the stream's next 20 ms (or less, when it runs dry) in m_decodeFrame; returns the samples per channel.
    size_t readStream(Stream& stream, int64_t now);
    bool decodeNext(Stream& stream);
    void limitMix();
    void receiveLoop();
    void handlePacket(const uint8_t* data, size_t size);
    void identifySender(uint32_t ssrc, const uint8_t* payload, size_t size);

    VoiceGateway m_gateway;
    AudioEngine m_audio;
    UdpSocket m_udp;
    std::unique_ptr<TransportCipher> m_cipher;
    std::unique_ptr<DaveSession> m_dave;
    std::thread m_receiveThread;
    std::thread m_discoveryThread;
    std::atomic<bool> m_receiving{false};

    State m_state = State::Disconnected;
    VoiceGateway::Credentials m_credentials;
    QString m_channelId;
    quint32 m_ssrc = 0;
    QString m_encryptionMode;
    bool m_mediaRunning = false;

    // Settings shared with the audio threads.
    std::atomic<bool> m_selfMuted{false};
    std::atomic<bool> m_selfDeafened{false};
    std::atomic<bool> m_pushToTalk{false};
    std::atomic<float> m_thresholdDb{-50.0f};
    std::atomic<int> m_pushToTalkKey{0};
    std::atomic<int> m_pushToTalkReleaseMs{200};
    std::atomic<float> m_inputGain{1.0f};
    std::atomic<float> m_outputGain{1.0f};
    std::atomic<bool> m_automaticSensitivity{true};
    std::atomic<bool> m_noiseSuppression{true};
    std::atomic<bool> m_echoCancellation{false};
    std::atomic<bool> m_automaticGainControl{false};
    std::atomic<int> m_processorConfigVersion{0};
    QString m_inputDevice;
    QString m_outputDevice;

    // Capture state (capture thread only).
    AudioProcessor m_processor;
    int m_appliedProcessorConfig = -1;
    std::vector<float> m_processChunk;
    size_t m_processFill = 0;
    std::vector<float> m_echoChunk;
    float m_frameVoiceProbability = -1.0f;
    std::vector<float> m_captureFrame;
    std::vector<float> m_preRollFrame;
    bool m_hasPreRollFrame = false;
    size_t m_captureFill = 0;
    std::vector<float> m_stereoFrame;
    std::vector<uint8_t> m_opusPacket;
    std::vector<uint8_t> m_davePacket;
    std::vector<uint8_t> m_udpPacket;
    std::unique_ptr<OpusEncoderWrapper> m_encoder;
    uint16_t m_sequence = 0;
    uint32_t m_timestamp = 0;
    bool m_transmitting = false;
    int m_voiceHangoverFrames = 0;
    int64_t m_pushToTalkUntilMs = 0;
    int m_silenceFramesToSend = 0;
    std::atomic<float> m_inputLevelDb{-100.0f};
    std::atomic<bool> m_selfSpeaking{false};

    // Remote speakers.
    std::mutex m_streamsMutex;
    std::unordered_map<quint32, std::shared_ptr<Stream>> m_streams; // by SSRC
    QHash<QString, float> m_userVolumes;                            // main thread copy
    std::vector<std::shared_ptr<Stream>> m_mixStreams;              // playback thread scratch
    std::unordered_map<quint32, int64_t> m_identifyAttempts;        // receive thread: unmapped SSRC -> last try
    std::vector<float> m_mixFrame;
    std::vector<float> m_decodeFrame;
    size_t m_mixReadOffset = 0;
    float m_limiterGain = 1.0f; // playback thread
    EchoReference m_echoReference;
    std::atomic<uint32_t> m_playedFrames{0};
    std::atomic<uint32_t> m_concealedFrames{0};
    double m_inboundPacketLoss = 0.0;
    int m_daveProtocolVersion = 0;

    QTimer m_speakingTimer;
    QHash<QString, bool> m_speaking;
    bool m_selfWasSpeaking = false;

    // Media statistics, logged periodically to help diagnose call problems.
    struct Statistics
    {
        std::atomic<uint32_t> sent{0};
        std::atomic<uint32_t> received{0};
        std::atomic<uint32_t> transportFailures{0};
        std::atomic<uint32_t> encryptFailures{0};
        std::atomic<uint32_t> decryptFailures{0};
        std::atomic<uint32_t> unknownSsrc{0};
        // Microphone side, to diagnose "nobody hears me": frames sent as voice, loudest level, highest
        // voice probability, and frames dropped because of mute/deafen.
        std::atomic<uint32_t> transmittedFrames{0};
        std::atomic<uint32_t> mutedFrames{0};
        std::atomic<float> peakLevelDb{-100.0f};
        std::atomic<float> peakVoiceProbability{-1.0f};
    };
    Statistics m_stats;
    int m_statisticsTicks = 0;
};

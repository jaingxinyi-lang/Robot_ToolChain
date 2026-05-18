#ifndef RS485_FRAME_CODEC_H
#define RS485_FRAME_CODEC_H

#include <QByteArray>
#include <QtGlobal>

class Rs485FrameCodec {
public:
    static constexpr quint8 PacketHead0 = 0xAA;
    static constexpr quint8 PacketHead1 = 0x55;
    static constexpr quint32 ControlSequence = 0xFFFFFFFFu;
    static constexpr quint32 InitSequence = 0x00000000u;
    static constexpr int MinPayloadLength = 1;
    static constexpr int MaxPayloadLength = 200;
    static constexpr int FrameOverhead = 2 + 4 + 1 + 2;
    static constexpr int MaxFrameLength = FrameOverhead + MaxPayloadLength;

    static quint16 calcChecksum16(const QByteArray &data, int length);
    static int tryExtractFrame(const QByteArray &rawBuffer, QByteArray *outFrame, int *consumedBytes);
    static bool checksumOk(const QByteArray &frame);
    static quint32 sequenceOf(const QByteArray &frame);
    static int payloadLengthOf(const QByteArray &frame);
    static int controlBaudRateOf(const QByteArray &frame);
};

#endif // RS485_FRAME_CODEC_H
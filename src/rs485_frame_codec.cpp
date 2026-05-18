#include "rs485_frame_codec.h"

quint16 Rs485FrameCodec::calcChecksum16(const QByteArray &data, int length)
{
    const int boundedLength = qBound(0, length, data.size());
    quint32 sum = 0;
    for (int offset = 0; offset < boundedLength; ++offset) {
        sum += static_cast<quint8>(data.at(offset));
    }
    return static_cast<quint16>(sum & 0xFFFFu);
}

int Rs485FrameCodec::tryExtractFrame(const QByteArray &rawBuffer, QByteArray *outFrame, int *consumedBytes)
{
    if (consumedBytes) {
        *consumedBytes = 0;
    }
    if (outFrame) {
        outFrame->clear();
    }

    const int rawLength = rawBuffer.size();
    for (int offset = 0; offset <= rawLength - FrameOverhead; ++offset) {
        const quint8 head0 = static_cast<quint8>(rawBuffer.at(offset));
        const quint8 head1 = static_cast<quint8>(rawBuffer.at(offset + 1));
        if (head0 != PacketHead0 || head1 != PacketHead1) {
            continue;
        }

        const int payloadLength = static_cast<quint8>(rawBuffer.at(offset + 6));
        if (payloadLength < MinPayloadLength || payloadLength > MaxPayloadLength) {
            if (consumedBytes) {
                *consumedBytes = offset + 2;
            }
            return -2;
        }

        const int frameLength = FrameOverhead + payloadLength;
        if (offset + frameLength > rawLength) {
            return 0;
        }

        if (outFrame) {
            *outFrame = rawBuffer.mid(offset, frameLength);
        }
        if (consumedBytes) {
            *consumedBytes = offset + frameLength;
        }
        return frameLength;
    }

    if (rawLength > 1 && consumedBytes) {
        *consumedBytes = rawLength - 1;
    }
    return 0;
}

bool Rs485FrameCodec::checksumOk(const QByteArray &frame)
{
    if (frame.size() < FrameOverhead) {
        return false;
    }

    const int payloadLength = payloadLengthOf(frame);
    if (frame.size() != FrameOverhead + payloadLength) {
        return false;
    }

    const int checksumOffset = 7 + payloadLength;
    const quint16 expected = static_cast<quint8>(frame.at(checksumOffset))
        | (static_cast<quint16>(static_cast<quint8>(frame.at(checksumOffset + 1))) << 8);
    const quint16 calculated = calcChecksum16(frame, 7 + payloadLength);
    return expected == calculated;
}

quint32 Rs485FrameCodec::sequenceOf(const QByteArray &frame)
{
    if (frame.size() < 6) {
        return 0;
    }

    return static_cast<quint8>(frame.at(2))
        | (static_cast<quint32>(static_cast<quint8>(frame.at(3))) << 8)
        | (static_cast<quint32>(static_cast<quint8>(frame.at(4))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(frame.at(5))) << 24);
}

int Rs485FrameCodec::payloadLengthOf(const QByteArray &frame)
{
    if (frame.size() < 7) {
        return 0;
    }
    return static_cast<quint8>(frame.at(6));
}

int Rs485FrameCodec::controlBaudRateOf(const QByteArray &frame)
{
    if (frame.size() < FrameOverhead + 4 || payloadLengthOf(frame) != 4) {
        return 0;
    }

    const quint32 baudRate = static_cast<quint8>(frame.at(7))
        | (static_cast<quint32>(static_cast<quint8>(frame.at(8))) << 8)
        | (static_cast<quint32>(static_cast<quint8>(frame.at(9))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(frame.at(10))) << 24);
    return static_cast<int>(baudRate);
}
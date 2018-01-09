//
//  MessageOut.cc
//  blip_cpp
//
//  Created by Jens Alfke on 4/5/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "MessageOut.hh"
#include "BLIPConnection.hh"
#include "BLIPInternal.hh"
#include "Codec.hh"
#include "Error.hh"
#include "varint.hh"
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {

    static const size_t kDataBufferSize = 4096;

    MessageOut::MessageOut(Connection *connection,
                           FrameFlags flags,
                           alloc_slice payload,
                           MessageDataSource dataSource,
                           MessageNo number)
    :Message(flags, number)
    ,_connection(connection)
    ,_contents(payload, dataSource)
    { }


    void MessageOut::nextFrameToSend(Codec &codec, slice &dst, FrameFlags &outFlags) {
        outFlags = flags();
        if (isAck()) {
            // Acks have no checksum and don't go through the codec
            slice &data = _contents.dataToSend();
            dst.writeFrom(data);
            _bytesSent += data.size;
            return;
        }

        size_t frameSize = dst.size;
        dst.setSize(dst.size - Codec::kChecksumSize);          // Reserve room for checksum at end

        // Write the frame:
        auto mode = hasFlag(kCompressed) ? Codec::Mode::SyncFlush : Codec::Mode::Raw;
        do {
            slice &data = _contents.dataToSend();
            if (data.size == 0)
                break;
            _uncompressedBytesSent += data.size;
            codec.write(data, dst, mode);
            _uncompressedBytesSent -= data.size;
        } while (dst.size >= 1024);

        if (codec.unflushedBytes() > 0)
            throw runtime_error("Compression buffer overflow");

        if (mode == Codec::Mode::SyncFlush) {
            size_t bytesWritten = (frameSize - Codec::kChecksumSize) - dst.size;
            if (bytesWritten > 0) {
                // SyncFlush always ends the output with the 4 bytes 00 00 FF FF.
                // We can remove those, then add them when reading the data back in.
                Assert(bytesWritten >= 4 && memcmp(&dst[-4], "\x00\x00\xFF\xFF", 4) == 0);
                dst.moveStart(-4);
            }
        }

        // Write the checksum:
        dst.setSize(dst.size + Codec::kChecksumSize);           // Undo "Reserve room..." above
        codec.writeChecksum(dst);

        // Compute the (compressed) frame size, and update running totals:
        frameSize -= dst.size;
        _bytesSent += frameSize;
        _unackedBytes += frameSize;

        // Update flags & state:
        MessageProgress::State state;
        if (_contents.hasMoreDataToSend()) {
            outFlags = (FrameFlags)(outFlags | kMoreComing);
            state = MessageProgress::kSending;
        } else if (noReply()) {
            state = MessageProgress::kComplete;
        } else {
            state = MessageProgress::kAwaitingReply;
        }
        LogVerbose(BLIPLog, "   Compressed %u bytes", (unsigned)_uncompressedBytesSent);//TEMP
        sendProgress(state, _uncompressedBytesSent, 0, nullptr);
    }


    void MessageOut::receivedAck(uint32_t byteCount) {
        if (byteCount <= _bytesSent)
            _unackedBytes = min(_unackedBytes, (uint32_t)(_bytesSent - byteCount));
    }


    MessageIn* MessageOut::createResponse() {
        if (type() != kRequestType || noReply())
            return nullptr;
        // Note: The MessageIn's flags will be updated when the 1st frame of the response arrives;
        // the type might become kErrorType, and kUrgent or kCompressed might be set.
        return new MessageIn(_connection, (FrameFlags)kResponseType, _number,
                             _onProgress, _uncompressedBytesSent);
    }


    void MessageOut::disconnected() {
        if (type() != kRequestType || noReply())
            return;
        Message::disconnected();
    }


    void MessageOut::dump(std::ostream& out, bool withBody) {
        slice props, body;
        _contents.getPropsAndBody(props, body);
        if (!withBody)
            body = nullslice;
        Message::dump(props, body, out);
    }


#pragma mark - DATA:


    MessageOut::Contents::Contents(alloc_slice payload, MessageDataSource dataSource)
    :_payload(payload)
    ,_unsentPayload(payload.buf, payload.size)
    ,_dataSource(dataSource)
    {
        assert(payload.size <= UINT32_MAX);
    }


    // Returns the next message-body data to send (as a slice _reference_)
    slice& MessageOut::Contents::dataToSend() {
        if (_unsentPayload.size == 0 && _dataSource) {
            _payload.reset();
            if (_unsentDataBuffer.size == 0) {
                readFromDataSource();
                if (_unsentDataBuffer.size == 0)
                    _dataBuffer.reset();
            }
            return _unsentDataBuffer;
        }
        return _unsentPayload;
    }


    // Is there more data to send?
    bool MessageOut::Contents::hasMoreDataToSend() const {
        return _unsentPayload.size > 0 || _unsentDataBuffer.size > 0 || _dataSource != nullptr;
    }


    // Refills _dataBuffer and _dataBufferAvail from _dataSource.
    void MessageOut::Contents::readFromDataSource() {
        if (!_dataBuffer)
            _dataBuffer.reset(kDataBufferSize);
        auto bytesWritten = _dataSource((void*)_dataBuffer.buf, _dataBuffer.size);
        _unsentDataBuffer = _dataBuffer.upTo(bytesWritten);
        if (bytesWritten < _dataBuffer.size) {
            // End of data source
            _dataSource = nullptr;
            if (bytesWritten < 0) {
                WarnError("Error from BLIP message dataSource");
                //FIX: How to report/handle the error?
            }
        }
    }


    void MessageOut::Contents::getPropsAndBody(slice &props, slice &body) const {
        props = _payload;
        if (props.size) {
            uint32_t propertiesSize;
            ReadUVarInt32(&props, &propertiesSize);
            props.setSize(propertiesSize);
        }
        body = slice(props.end(), _payload.end());
    }

} }

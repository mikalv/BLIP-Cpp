//
//  WebSocketImpl.hh
//  StreamTaskTest
//
//  Created by Jens Alfke on 3/15/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketInterface.hh"
#include "Logging.hh"
#include "Stopwatch.hh"
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <set>

namespace uWS {
    template <const bool isServer> class WebSocketProtocol;
}

namespace litecore { namespace websocket {
    class ProviderImpl;

    extern LogDomain WSLogDomain;

    /** Transport-agnostic implementation of WebSocket protocol.
        It doesn't transfer data or run the handshake; it just knows how to encode and decode
        messages. */
    class WebSocketImpl : public WebSocket, Logging {
    public:
        WebSocketImpl(ProviderImpl&, const Address&,
                      const fleeceapi::AllocedDict &options, bool framing);
        virtual ~WebSocketImpl();

        virtual bool send(fleece::slice message, bool binary =true) override;
        virtual void close(int status =kCodeNormal, fleece::slice message =fleece::nullslice) override;

        // Concrete socket implementation needs to call these:
        void gotHTTPResponse(int status, const fleeceapi::AllocedDict &headers);
        void onConnect();
        void onCloseRequested(int status, fleece::slice message);
        void onClose(int posixErrno);
        void onClose(CloseStatus);
        void onReceive(fleece::slice);
        void onWriteComplete(size_t);

        const fleeceapi::AllocedDict& options() const   {return _options;}

    protected:
        virtual std::string loggingIdentifier() const override;
        virtual void connect() override;

        ProviderImpl& provider()                    {return (ProviderImpl&)WebSocket::provider();}
        void disconnect();

    private:
        template <const bool isServer>
        friend class uWS::WebSocketProtocol;

        using ClientProtocol = uWS::WebSocketProtocol<false>;

        bool sendOp(fleece::slice, int opcode);
        bool handleFragment(char *data,
                            size_t length,
                            unsigned int remainingBytes,
                            int opCode,
                            bool fin);
        bool receivedMessage(int opCode, fleece::alloc_slice message);
        bool receivedClose(fleece::slice);

        fleeceapi::AllocedDict _options;
        bool _framing;                              // true if I should implement WebSocket framing
        std::unique_ptr<ClientProtocol> _protocol;  // 3rd party class that does the framing
        std::mutex _mutex;                          //
        fleece::alloc_slice _curMessage;            // Message being received
        int _curOpCode;
        size_t _curMessageLength;
        size_t _bufferedBytes {0};                  // # bytes written but not yet completed
        bool _closeSent {false}, _closeReceived {false};    // Close message sent or received?
        fleece::alloc_slice _closeMessage;                  // The encoded close request message

        // Connection diagnostics, logged on close:
        fleece::Stopwatch _timeConnected {false};           // Time since socket opened
        uint64_t _bytesSent {0}, _bytesReceived {0};// Total byte count sent/received
};


    /** Provider implementation that creates WebSocketImpls. */
    class ProviderImpl : public Provider {
    public:
        ProviderImpl() { }

    protected:
        friend class WebSocketImpl;

        // These methods have to be implemented in subclasses, to connect to the actual socket:

        virtual void openSocket(WebSocketImpl*) =0;
        virtual void closeSocket(WebSocketImpl*) =0;
        virtual void sendBytes(WebSocketImpl*, fleece::alloc_slice) =0;
        virtual void receiveComplete(WebSocketImpl*, size_t byteCount) =0;

        virtual void requestClose(WebSocketImpl*, int status, fleece::slice message) =0;
    };

} }

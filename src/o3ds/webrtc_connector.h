/*
Open 3D Stream

Copyright 2025 Alastair Macleod

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef O3DS_WEBRTC_CONNECTOR
#define O3DS_WEBRTC_CONNECTOR

#include "base_connector.h"
#include <string>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

// Forward declarations for libdatachannel types
namespace rtc {
    class PeerConnection;
    class DataChannel;
    class WebSocket;
    struct Configuration;
}

namespace O3DS
{
    //! WebRTC client connector using libdatachannel
    /*! \class WebRTCClient webrtc_connector.h o3ds/webrtc_connector.h
    WebRTC client that connects to a signaling server and establishes
    a peer-to-peer data channel for O3DS streaming.
    
    URL format: webrtc://signaling-server:port/room-id
    Example: webrtc://localhost:8080/myroom
    */
    class WebRTCClient : public AsyncConnector
    {
    public:
        WebRTCClient();
        virtual ~WebRTCClient();

        //! Start the WebRTC connection
        /*! \param url Format: webrtc://signaling-server:port/room-id
            \return true if signaling connection established */
        virtual bool start(const char* url) override;

        //! Stop the connection and clean up resources
        virtual void stop() override;

        //! Write data to the WebRTC data channel
        /*! \param data Pointer to data buffer
            \param len Size of data in bytes
            \return true if data was queued for sending */
        virtual bool write(const char* data, size_t len) override;

        //! Read data (not used for async connector)
        virtual size_t read(char* data, size_t len) override;

        //! Read data (not used for async connector)
        virtual size_t read(char** data, size_t* len) override;

        //! Set STUN/TURN servers for NAT traversal
        /*! \param stunServer STUN server URL (e.g., "stun:stun.l.google.com:19302")
            \param turnServer Optional TURN server URL
            \param turnUsername Optional TURN username
            \param turnPassword Optional TURN password */
        void setIceServers(const std::string& stunServer,
                          const std::string& turnServer = "",
                          const std::string& turnUsername = "",
                          const std::string& turnPassword = "");

    private:
        //! Parse URL into components
        bool parseUrl(const char* url, std::string& signalingServer, 
                     int& port, std::string& roomId);

        //! Connect to signaling server via WebSocket
        bool connectSignaling(const std::string& server, int port);

        //! Handle incoming signaling messages
        void onSignalingMessage(const std::string& message);

        //! Create and configure peer connection
        void createPeerConnection();

        //! Create data channel
        void createDataChannel();

        //! Handle data channel open event
        void onDataChannelOpen();

        //! Handle data channel close event
        void onDataChannelClose();

        //! Handle incoming data channel message
        void onDataChannelMessage(const std::vector<uint8_t>& data);

        //! Send message to signaling server
        bool sendSignalingMessage(const std::string& message);

        std::shared_ptr<rtc::PeerConnection> mPeerConnection;
        std::shared_ptr<rtc::DataChannel> mDataChannel;
        std::shared_ptr<rtc::WebSocket> mSignalingSocket;
        std::shared_ptr<rtc::Configuration> mRtcConfig;

        std::string mSignalingUrl;
        std::string mRoomId;
        
        std::mutex mDataMutex;
        std::queue<std::vector<uint8_t>> mIncomingData;
        
        bool mIsDataChannelOpen;
        bool mIsConnected;
    };

    //! WebRTC server connector (signaling + peer connection)
    /*! \class WebRTCServer webrtc_connector.h o3ds/webrtc_connector.h
    WebRTC server that runs a signaling server and accepts peer connections.
    Useful for broadcast scenarios where multiple clients connect.
    
    URL format: webrtc-server://0.0.0.0:port
    Example: webrtc-server://0.0.0.0:8080
    */
    class WebRTCServer : public AsyncConnector
    {
    public:
        WebRTCServer();
        virtual ~WebRTCServer();

        //! Start the WebRTC server
        /*! \param url Format: webrtc-server://address:port
            \return true if server started successfully */
        virtual bool start(const char* url) override;

        //! Stop the server
        virtual void stop() override;

        //! Broadcast data to all connected peers
        /*! \param data Pointer to data buffer
            \param len Size of data in bytes
            \return true if data was sent to at least one peer */
        virtual bool write(const char* data, size_t len) override;

        //! Read data (not used for async connector)
        virtual size_t read(char* data, size_t len) override;

        //! Read data (not used for async connector)
        virtual size_t read(char** data, size_t* len) override;

    private:
        class PeerHandler;
        
        std::vector<std::shared_ptr<PeerHandler>> mPeers;
        std::shared_ptr<rtc::WebSocket> mSignalingServer;
        std::mutex mPeersMutex;
        bool mIsRunning;
    };

} // namespace O3DS

#endif  // O3DS_WEBRTC_CONNECTOR

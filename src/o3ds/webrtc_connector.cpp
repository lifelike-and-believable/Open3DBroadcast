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

#include "webrtc_connector.h"

#ifdef O3DS_ENABLE_WEBRTC

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

namespace O3DS
{

//
// WebRTCClient Implementation
//

WebRTCClient::WebRTCClient()
    : mIsDataChannelOpen(false)
    , mIsConnected(false)
{
    mState = NOTSTARTED;
}

WebRTCClient::~WebRTCClient()
{
    stop();
}

bool WebRTCClient::parseUrl(const char* url, std::string& signalingServer, 
                             int& port, std::string& roomId)
{
    std::string urlStr(url);
    
    // Remove webrtc:// prefix
    if (urlStr.find("webrtc://") == 0) {
        urlStr = urlStr.substr(9);
    }
    
    // Parse: server:port/roomId
    size_t colonPos = urlStr.find(':');
    size_t slashPos = urlStr.find('/');
    
    if (colonPos == std::string::npos || slashPos == std::string::npos) {
        setError("Invalid URL format. Expected: webrtc://server:port/roomId");
        return false;
    }
    
    signalingServer = urlStr.substr(0, colonPos);
    
    std::string portStr = urlStr.substr(colonPos + 1, slashPos - colonPos - 1);
    port = std::stoi(portStr);
    
    roomId = urlStr.substr(slashPos + 1);
    
    return true;
}

bool WebRTCClient::start(const char* url)
{
    if (mState != NOTSTARTED && mState != CLOSED) {
        setError("Connection already started");
        return false;
    }
    
    std::string signalingServer;
    int port;
    
    if (!parseUrl(url, signalingServer, port, mRoomId)) {
        return false;
    }
    
    mState = STARTED;
    
    // Create RTC configuration with default STUN server
    mRtcConfig = std::make_shared<rtc::Configuration>();
    mRtcConfig->iceServers.emplace_back("stun:stun.l.google.com:19302");
    
    // Connect to signaling server
    if (!connectSignaling(signalingServer, port)) {
        mState = STATE_ERROR;
        return false;
    }
    
    // Create peer connection
    createPeerConnection();
    
    return true;
}

void WebRTCClient::stop()
{
    mIsConnected = false;
    mIsDataChannelOpen = false;
    
    if (mDataChannel) {
        mDataChannel->close();
        mDataChannel.reset();
    }
    
    if (mPeerConnection) {
        mPeerConnection->close();
        mPeerConnection.reset();
    }
    
    if (mSignalingSocket) {
        mSignalingSocket->close();
        mSignalingSocket.reset();
    }
    
    mState = CLOSED;
}

bool WebRTCClient::connectSignaling(const std::string& server, int port)
{
    try {
        std::stringstream ws_url;
        ws_url << "ws://" << server << ":" << port << "/ws";
        
        mSignalingSocket = std::make_shared<rtc::WebSocket>();
        
        mSignalingSocket->onOpen([this]() {
            // Join room
            json joinMsg;
            joinMsg["type"] = "join";
            joinMsg["roomId"] = mRoomId;
            sendSignalingMessage(joinMsg.dump());
        });
        
        mSignalingSocket->onMessage([this](auto data) {
            if (std::holds_alternative<std::string>(data)) {
                onSignalingMessage(std::get<std::string>(data));
            }
        });
        
        mSignalingSocket->onError([this](std::string error) {
            setError(("Signaling error: " + error).c_str());
            mState = STATE_ERROR;
        });
        
        mSignalingSocket->onClosed([this]() {
            mIsConnected = false;
        });
        
        mSignalingSocket->open(ws_url.str());
        
        return true;
    }
    catch (const std::exception& e) {
        setError(("Failed to connect to signaling server: " + std::string(e.what())).c_str());
        return false;
    }
}

void WebRTCClient::createPeerConnection()
{
    try {
        mPeerConnection = std::make_shared<rtc::PeerConnection>(*mRtcConfig);
        
        // Handle ICE candidate generation
        mPeerConnection->onLocalDescription([this](rtc::Description desc) {
            json msg;
            msg["type"] = desc.typeString();
            msg["sdp"] = std::string(desc);
            msg["roomId"] = mRoomId;
            sendSignalingMessage(msg.dump());
        });
        
        mPeerConnection->onLocalCandidate([this](rtc::Candidate candidate) {
            json msg;
            msg["type"] = "candidate";
            msg["candidate"] = std::string(candidate);
            msg["mid"] = candidate.mid();
            msg["roomId"] = mRoomId;
            sendSignalingMessage(msg.dump());
        });
        
        // Handle incoming data channel
        mPeerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
            mDataChannel = dc;
            createDataChannel();
        });
        
        mPeerConnection->onStateChange([this](rtc::PeerConnection::State state) {
            switch (state) {
                case rtc::PeerConnection::State::Connected:
                    mIsConnected = true;
                    mState = READING;
                    break;
                case rtc::PeerConnection::State::Failed:
                case rtc::PeerConnection::State::Disconnected:
                case rtc::PeerConnection::State::Closed:
                    mIsConnected = false;
                    mState = STATE_ERROR;
                    break;
                default:
                    break;
            }
        });
        
        // Create data channel
        createDataChannel();
        
    }
    catch (const std::exception& e) {
        setError(("Failed to create peer connection: " + std::string(e.what())).c_str());
        mState = STATE_ERROR;
    }
}

void WebRTCClient::createDataChannel()
{
    if (!mDataChannel) {
        mDataChannel = mPeerConnection->createDataChannel("o3ds-stream");
    }
    
    mDataChannel->onOpen([this]() {
        onDataChannelOpen();
    });
    
    mDataChannel->onClosed([this]() {
        onDataChannelClose();
    });
    
    mDataChannel->onMessage([this](rtc::message_variant data) {
        if (std::holds_alternative<rtc::binary>(data)) {
            const auto& bin = std::get<rtc::binary>(data);
            onDataChannelMessage(bin);
        }
    });
}

void WebRTCClient::onDataChannelOpen()
{
    mIsDataChannelOpen = true;
    mState = READING;
}

void WebRTCClient::onDataChannelClose()
{
    mIsDataChannelOpen = false;
}

void WebRTCClient::onDataChannelMessage(const rtc::binary& data)
{
    // Call user callback if set
    if (mInDataFunc && mContext) {
        // rtc::binary is std::vector<std::byte>; convert pointer type for callback
        mInDataFunc(mContext, (void*)data.data(), data.size());
    }
}

void WebRTCClient::onSignalingMessage(const std::string& message)
{
    try {
        json msg = json::parse(message);
        std::string type = msg["type"];
        
        if (type == "offer" || type == "answer") {
            rtc::Description desc(msg["sdp"], type);
            mPeerConnection->setRemoteDescription(desc);
            
            // If we received an offer, create an answer
            if (type == "offer") {
                // Answer will be sent via onLocalDescription callback
            }
        }
        else if (type == "candidate") {
            rtc::Candidate candidate(msg["candidate"], msg["mid"]);
            mPeerConnection->addRemoteCandidate(candidate);
        }
    }
    catch (const std::exception& e) {
        setError(("Failed to process signaling message: " + std::string(e.what())).c_str());
    }
}

bool WebRTCClient::sendSignalingMessage(const std::string& message)
{
    if (!mSignalingSocket || !mSignalingSocket->isOpen()) {
        return false;
    }
    
    try {
        mSignalingSocket->send(message);
        return true;
    }
    catch (const std::exception& e) {
        setError(("Failed to send signaling message: " + std::string(e.what())).c_str());
        return false;
    }
}

bool WebRTCClient::write(const char* data, size_t len)
{
    if (!mIsDataChannelOpen || !mDataChannel) {
        setError("Data channel not open");
        return false;
    }
    
    try {
        // rtc::binary is std::vector<std::byte>; construct from uint8_t*
        const std::byte* b = reinterpret_cast<const std::byte*>(data);
        rtc::binary bin(b, b + len);
        mDataChannel->send(bin);
        return true;
    }
    catch (const std::exception& e) {
        setError(("Failed to send data: " + std::string(e.what())).c_str());
        return false;
    }
}

size_t WebRTCClient::read(char* data, size_t len)
{
    // Not used for async connector
    return 0;
}

size_t WebRTCClient::read(char** data, size_t* len)
{
    // Not used for async connector
    return 0;
}

void WebRTCClient::setIceServers(const std::string& stunServer,
                                  const std::string& turnServer,
                                  const std::string& turnUsername,
                                  const std::string& turnPassword)
{
    if (!mRtcConfig) {
        mRtcConfig = std::make_shared<rtc::Configuration>();
    }
    
    mRtcConfig->iceServers.clear();
    
    if (!stunServer.empty()) {
        mRtcConfig->iceServers.emplace_back(stunServer);
    }
    
    if (!turnServer.empty()) {
        rtc::IceServer turn(turnServer);
        if (!turnUsername.empty()) {
            turn.username = turnUsername;
            turn.password = turnPassword;
        }
        mRtcConfig->iceServers.push_back(turn);
    }
}

//
// WebRTCServer Implementation
//

class WebRTCServer::PeerHandler {
public:
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::string peerId;
    bool isReady = false;
};

WebRTCServer::WebRTCServer()
    : mIsRunning(false)
{
    mState = NOTSTARTED;
}

WebRTCServer::~WebRTCServer()
{
    stop();
}

bool WebRTCServer::start(const char* url)
{
    // Server implementation would require a full signaling server
    // For now, return not implemented
    setError("WebRTC Server not yet implemented. Use WebRTC Client mode.");
    return false;
}

void WebRTCServer::stop()
{
    mIsRunning = false;
    
    std::lock_guard<std::mutex> lock(mPeersMutex);
    
    for (auto& peer : mPeers) {
        if (peer->dc) peer->dc->close();
        if (peer->pc) peer->pc->close();
    }
    
    mPeers.clear();
    
    if (mSignalingServer) {
        mSignalingServer->close();
        mSignalingServer.reset();
    }
    
    mState = CLOSED;
}

bool WebRTCServer::write(const char* data, size_t len)
{
    std::lock_guard<std::mutex> lock(mPeersMutex);
    
    if (mPeers.empty()) {
        return false;
    }
    
    rtc::binary bin(data, data + len);
    bool sentToAny = false;
    
    for (auto& peer : mPeers) {
        if (peer->isReady && peer->dc && peer->dc->isOpen()) {
            try {
                peer->dc->send(bin);
                sentToAny = true;
            }
            catch (...) {
                // Continue to next peer
            }
        }
    }
    
    return sentToAny;
}

size_t WebRTCServer::read(char* data, size_t len)
{
    return 0;
}

size_t WebRTCServer::read(char** data, size_t* len)
{
    return 0;
}

} // namespace O3DS

#endif // O3DS_ENABLE_WEBRTC

#include "GPUVoyeur.h"

#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <basetsd.h>

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>

#endif


void GPUVoyeur::CreateListenerSocketForClient()
{
    if (CaptureMode::Local == m_captureMode)
    {
        return;
    }

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
    {
        m_debugLogStream << "[error] WSAStartup failed" << std::endl;
        m_listenerSocket = (GvSocket)-1;
        return;
    }
#endif

    // protocol of 6 is explicitly requesting TCP, which should happen
    // with AF_INET and SOCK_STREAM
    m_listenerSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (GV_INVALID_SOCKET(m_listenerSocket))
    {
        m_debugLogStream << "[error] Listener socket creation failed" << std::endl;
        return;
    }

    SetSocketNonBlockingState(m_listenerSocket, 1);

    struct sockaddr_in Addr;
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = INADDR_ANY;

    Addr.sin_port = htons(m_port);
    if (0 != bind(m_listenerSocket, (sockaddr*)&Addr, sizeof(Addr)))
    {
        m_debugLogStream << "[error] Binding address to listener socket failed" << std::endl;
        CloseGvSocket(m_listenerSocket);
        return;
    }

    if (0 != listen(m_listenerSocket, 8))
    {
        m_debugLogStream << "[error] Listening to listener socket failed" << std::endl;
        CloseGvSocket(m_listenerSocket);
        return;
    }

    m_debugLogStream << "[info] Listener socket created successfully on port " << m_port << std::endl;

    m_liveConnectionSocket = (GvSocket)-1;
}

void GPUVoyeur::ShutDownSocketInfra()
{
    CloseGvSocket(m_listenerSocket);
    CloseGvSocket(m_liveConnectionSocket);
    CloseGvSocket(m_captureSocket);

#if defined(_WIN32)
    WSACleanup();
#endif

}

void GPUVoyeur::SetSocketNonBlockingState(GvSocket socket, int nonblockingState)
{
#if defined(_WIN32)
    u_long nonBlocking = nonblockingState ? 1 : 0;
    ioctlsocket(socket, FIONBIO, &nonBlocking);
#else
    int Options = fcntl(socket, F_GETFL);
    if (nonblockingState)
    {
        fcntl(socket, F_SETFL, Options | O_NONBLOCK);
    }
    else
    {
        fcntl(socket, F_SETFL, Options&(~O_NONBLOCK));
    }
#endif
}

void GPUVoyeur::CloseGvSocket(GvSocket& socket)
{
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif

    socket = (GvSocket)-1;

    // TODO: Do I want to grab the last error? Where do I log it?
}

// TODO: In general, we should probably be more prepared for incomplete/partial
// transmissions. We often assume entire chunk transmissions.

void GPUVoyeur::CheckForConnectionFromClient()
{
    // TODO: This should really be moved to a thread to try to get connected to the client

    if (!GV_INVALID_SOCKET(m_listenerSocket) &&
        GV_INVALID_SOCKET(m_liveConnectionSocket))
    {
        m_liveConnectionSocket = accept(m_listenerSocket, 0, 0);

        if (GV_INVALID_SOCKET(m_liveConnectionSocket))
        {
            return;
        }

        m_debugLogStream << "[info] Accepted connection from client on listener socket" << std::endl;

        uint32_t handshakeVal;
        int64_t actualBytesRead = recv(m_liveConnectionSocket, (char *)&handshakeVal, sizeof(uint32_t), 0);
        if ((actualBytesRead < 0) || 
            (kLogHandshake != handshakeVal))
        {
            m_debugLogStream << "[error] Receiving live handshake from client failed" << std::endl;
            CloseGvSocket(m_liveConnectionSocket);
            return;
        }

        m_debugLogStream << "[info] Valid handshake received from client" << std::endl;

        LogHeader logHeader;
        logHeader.handshake = kLogHandshake;
        logHeader.version = kCurrentVersion;
        logHeader.timestampPeriod = m_timestampPeriod;

        const char* logHeaderPtr = reinterpret_cast<const char *>(&logHeader);
        int64_t actualBytesSent = send(m_liveConnectionSocket, logHeaderPtr, sizeof(LogHeader), 0);
        if (actualBytesSent < 0)
        {
            m_debugLogStream << "[error] Transmitting live connection header to client failed" << std::endl;
            CloseGvSocket(m_liveConnectionSocket);
            return;
        }

        m_debugLogStream << "[info] Live connection header successfully transmitted to client" << std::endl;
    }
}

void GPUVoyeur::SendFrametimeToClient(uint64_t timestamp)
{
    if (!GV_INVALID_SOCKET(m_liveConnectionSocket))
    {
        const char* tsPtr = reinterpret_cast<const char *>(&timestamp);
        int64_t bytesSent = send(m_liveConnectionSocket, tsPtr, sizeof(uint64_t), 0);

        if (bytesSent < 0)
        {
            m_debugLogStream << "[error] Transmitting frametime to client failed" << std::endl;
            CloseGvSocket(m_liveConnectionSocket);
        }
    }
}

void GPUVoyeur::CheckForCaptureRequest()
{
    if (GV_INVALID_SOCKET(m_liveConnectionSocket))
    {
        // if we don't have a live connection, we can't capture
        return;
    }

    if (GV_INVALID_SOCKET(m_captureSocket))
    {
        GvSocket captureSocket = accept(m_listenerSocket, 0, 0);
        if (GV_INVALID_SOCKET(captureSocket))
        {
            return;
        }

        uint32_t handshake;
        int64_t actualBytesRead = recv(captureSocket, (char*)&handshake, sizeof(uint32_t), 0);
        if (actualBytesRead < 0)
        {
            m_debugLogStream << "[error] Receiving capture handshake from client failed" << std::endl;
            CloseGvSocket(captureSocket);
            return;
        }

        uint32_t numFramesToCapture;
        actualBytesRead = recv(captureSocket, (char*)&numFramesToCapture, sizeof(uint32_t), 0);
        if ((actualBytesRead < 0) || (0 == numFramesToCapture))
        {
            m_debugLogStream << "[error] Receiving number of frames to capture from client failed" << std::endl;
            CloseGvSocket(captureSocket);
            return;
        }

        uint32_t captureMarkerDepth;
        actualBytesRead = recv(captureSocket, (char*)&captureMarkerDepth, sizeof(uint32_t), 0);
        if (actualBytesRead < 0)
        {
            m_debugLogStream << "[error] Receiving capture marker depth from client failed" << std::endl;
            CloseGvSocket(captureSocket);
            return;
        }

        m_debugLogStream << "[info] Capture socket opened with client" << std::endl;

        // before we start our own log, we need to flush the current pending data
        // as it works better for the captured log to start clean (instead of having latent
        // data which might confuse users)
        // TODO: This strategy won't work as we get more async. We probably don't want to flush anywhere
        // besides the task that is handling writing out to the log. Doing the flush here, while in
        // line with the writes is ok for now. However, if we have different threads doing polling
        // and writing, might not be a good idea.
        FlushStagingBuffer();

        // Everything is good, we can arm our live state
        m_captureSocket = captureSocket;
        m_numFramesToCapture = numFramesToCapture;
        m_maxMarkerDepth = captureMarkerDepth;

        // We need to re-transmit log header and queue info that we saw!
        LogHeader logHeader;
        logHeader.handshake = kLogHandshake;
        logHeader.version = kCurrentVersion;
        logHeader.timestampPeriod = m_timestampPeriod;

        const char* logHeaderPtr = reinterpret_cast<const char *>(&logHeader);
        TransmitCaptureData(sizeof(LogHeader), logHeaderPtr);

        for (QueueInfoPacket& qiPacket : m_queueInfoCache)
        {
            const char* qiPtr = reinterpret_cast<const char *>(&qiPacket);
            TransmitCaptureData(sizeof(QueueInfoPacket), qiPtr);
        }

        m_debugLogStream << "[info] Capture header and queue info transmitted to client" << std::endl;

        // ok, we can now proceed to output data!
    }
}

void GPUVoyeur::TransmitCaptureData(uint32_t writeSize, const char * writeData)
{
    if (!GV_INVALID_SOCKET(m_captureSocket) && 
        (m_numFramesToCapture > 0) &&
        (writeSize > 0))
    {
        int64_t actualBytesSent = send(m_captureSocket, (const char *)&writeSize, sizeof(uint32_t), 0);
        if (actualBytesSent < 0)
        {
            m_debugLogStream << "[error] Transmitting size of capture block failed" << std::endl;
            CloseGvSocket(m_captureSocket);
            return;
        }

        actualBytesSent = send(m_captureSocket, writeData, writeSize, 0);
        if (actualBytesSent < 0)
        {
            m_debugLogStream << "[error] Transmitting capture block failed" << std::endl;
            CloseGvSocket(m_captureSocket);
            return;
        }
    }
}

void GPUVoyeur::DecrementCaptureFrameCount()
{
    if (!GV_INVALID_SOCKET(m_captureSocket))
    {
        if (m_numFramesToCapture > 0)
        {
            
            if (1 == m_numFramesToCapture)
            {
                // if this is our last pending frame, we need to flush everything
                // pending
                FlushStagingBuffer();

                // disable marker processing
                m_maxMarkerDepth = 0;

                uint32_t completionPacket = 0x0;
                int64_t actualBytesSent = send(m_captureSocket, (const char *)&completionPacket, sizeof(uint32_t), 0);
                if (actualBytesSent < 0)
                {
                    m_debugLogStream << "[error] Transmitting end of capture packet failed" << std::endl;
                    CloseGvSocket(m_captureSocket);
                }
            }

            m_numFramesToCapture--;
        }
        else
        {
            // we are done sending data, poll to confirm completions
            uint32_t completionSignal;
            int64_t actualBytesRead = recv(m_captureSocket, (char*)&completionSignal, sizeof(uint32_t), 0);

            if ((0xFFFFFFFF == completionSignal) || (actualBytesRead < 0))
            {
                CloseGvSocket(m_captureSocket);
            }

            // if 0 bytes read, we just check again later
        }
    }
}

#define LOG_CLASS "PeerConnection"

#include "../Include_i.h"

static volatile ATOMIC_BOOL gKvsWebRtcInitialized = (SIZE_T) FALSE;

#if (ENABLE_STREAMING)
/**
 * the initialization of srtp, after dtls session is done and receiving the srtp packets.
*/
STATUS allocateSrtp(PKvsPeerConnection pKvsPeerConnection)
{
    PDtlsKeyingMaterial pDtlsKeyingMaterial = NULL;
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    CHK(NULL != (pDtlsKeyingMaterial = (PDtlsKeyingMaterial) MEMALLOC(SIZEOF(DtlsKeyingMaterial))), STATUS_NOT_ENOUGH_MEMORY);
    MEMSET(pDtlsKeyingMaterial, 0, SIZEOF(DtlsKeyingMaterial));

    CHK(pKvsPeerConnection != NULL, STATUS_SUCCESS);
    CHK_STATUS(dtlsSessionVerifyRemoteCertificateFingerprint(pKvsPeerConnection->pDtlsSession, pKvsPeerConnection->remoteCertificateFingerprint));
    CHK_STATUS(dtlsSessionPopulateKeyingMaterial(pKvsPeerConnection->pDtlsSession, pDtlsKeyingMaterial));

    MUTEX_LOCK(pKvsPeerConnection->pSrtpSessionLock);
    locked = TRUE;

    CHK_STATUS(initSrtpSession(pKvsPeerConnection->dtlsIsServer ? pDtlsKeyingMaterial->clientWriteKey : pDtlsKeyingMaterial->serverWriteKey,
                               pKvsPeerConnection->dtlsIsServer ? pDtlsKeyingMaterial->serverWriteKey : pDtlsKeyingMaterial->clientWriteKey,
                               pDtlsKeyingMaterial->srtpProfile, &(pKvsPeerConnection->pSrtpSession)));

CleanUp:
    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->pSrtpSessionLock);
    }

    SAFE_MEMFREE(pDtlsKeyingMaterial);

    if (STATUS_FAILED(retStatus)) {
        DLOGW("dtlsSessionPopulateKeyingMaterial failed with 0x%08x", retStatus);
    }

    return retStatus;
}
#endif

#ifdef ENABLE_DATA_CHANNEL
STATUS allocateSctpSortDataChannelsDataCallback(UINT64 customData, PHashEntry pHashEntry)
{
    STATUS retStatus = STATUS_SUCCESS;
    PAllocateSctpSortDataChannelsData data = (PAllocateSctpSortDataChannelsData) customData;
    PKvsDataChannel pKvsDataChannel = (PKvsDataChannel) pHashEntry->value;

    CHK(customData != 0, STATUS_NULL_ARG);

    pKvsDataChannel->channelId = data->currentDataChannelId;
    CHK_STATUS(hashTablePut(data->pKvsPeerConnection->pDataChannels, pKvsDataChannel->channelId, (UINT64) pKvsDataChannel));

    data->currentDataChannelId += 2;

CleanUp:
    return retStatus;
}
/**
 * @brief the initialization of sctp, after dtls session is done. Start receiving the srtp packets.
 * 
 * @param[in]
*/
STATUS allocateSctp(PKvsPeerConnection pKvsPeerConnection)
{
    STATUS retStatus = STATUS_SUCCESS;
    SctpSessionCallbacks sctpSessionCallbacks;
    AllocateSctpSortDataChannelsData data;
    UINT32 currentDataChannelId = 0;
    PKvsDataChannel pKvsDataChannel = NULL;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    {

    /** #memory. */
    PDtlsKeyingMaterial dtlsKeyingMaterial = MEMALLOC(sizeof(DtlsKeyingMaterial));
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    MEMSET(dtlsKeyingMaterial, 0, SIZEOF(DtlsKeyingMaterial));

    CHK(pKvsPeerConnection != NULL, STATUS_SUCCESS);
    CHK_STATUS(dtlsSessionVerifyRemoteCertificateFingerprint(pKvsPeerConnection->pDtlsSession, pKvsPeerConnection->remoteCertificateFingerprint));
    CHK_STATUS(dtlsSessionPopulateKeyingMaterial(pKvsPeerConnection->pDtlsSession, dtlsKeyingMaterial));

    }
    currentDataChannelId = (pKvsPeerConnection->dtlsIsServer) ? 1 : 0;

    // Re-sort DataChannel hashmap using proper streamIds if we are offerer or answerer
    data.currentDataChannelId = currentDataChannelId;
    data.pKvsPeerConnection = pKvsPeerConnection;
    data.unkeyedDataChannels = pKvsPeerConnection->pDataChannels;
    CHK_STATUS(hashTableCreateWithParams(CODEC_HASH_TABLE_BUCKET_COUNT, CODEC_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pDataChannels));
    CHK_STATUS(hashTableIterateEntries(data.unkeyedDataChannels, (UINT64) &data, allocateSctpSortDataChannelsDataCallback));

    // Free unkeyed DataChannels
    CHK_LOG_ERR(hashTableClear(data.unkeyedDataChannels));
    CHK_LOG_ERR(hashTableFree(data.unkeyedDataChannels));

    // Create the SCTP Session
    sctpSessionCallbacks.outboundPacketFunc = onSctpSessionOutboundPacket;
    sctpSessionCallbacks.dataChannelMessageFunc = onSctpSessionDataChannelMessage;
    sctpSessionCallbacks.dataChannelOpenFunc = onSctpSessionDataChannelOpen;
    sctpSessionCallbacks.customData = (UINT64) pKvsPeerConnection;
    CHK_STATUS(createSctpSession(&sctpSessionCallbacks, &(pKvsPeerConnection->pSctpSession)));

    for (; currentDataChannelId < data.currentDataChannelId; currentDataChannelId += 2) {
        CHK_STATUS(hashTableGet(pKvsPeerConnection->pDataChannels, currentDataChannelId, (PUINT64) &pKvsDataChannel));
        CHK(pKvsDataChannel != NULL, STATUS_INTERNAL_ERROR);
        sctpSessionWriteDcep(pKvsPeerConnection->pSctpSession,
                             currentDataChannelId,
                             pKvsDataChannel->dataChannel.name,
                             STRLEN(pKvsDataChannel->dataChannel.name),
                             &pKvsDataChannel->rtcDataChannelInit);

        if (pKvsDataChannel->onOpen != NULL) {
            pKvsDataChannel->onOpen(pKvsDataChannel->onOpenCustomData, &pKvsDataChannel->dataChannel);
        }
    }

CleanUp:
    return retStatus;
}
#endif
/**
 * @brief the callback of inbound packet, and it is a packet arbitrator.
 * 
 * @param[in] customData the object of KvsPeerConnection.
 * @param[in] buff the buffer of packets.
 * @param[in] buffLen the length of buffer.
*/
VOID onInboundPacket(UINT64 customData, PBYTE buff, UINT32 buffLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    BOOL isDtlsConnected = FALSE;
    INT32 signedBuffLen = buffLen;

    CHK(signedBuffLen > 2 && pKvsPeerConnection != NULL, STATUS_SUCCESS);

    /*
     demux each packet off of its first byte
     https://tools.ietf.org/html/rfc5764#section-5.1.2
                 +----------------+
                  | 127 < B < 192 -+--> forward to RTP
                  |                |
      packet -->  |  19 < B < 64  -+--> forward to DTLS
                  |                |
                  |       B < 2   -+--> forward to STUN
                  +----------------+
    */
    if (buff[0] > 19 && buff[0] < 64) {
        dtlsSessionProcessPacket(pKvsPeerConnection->pDtlsSession, buff, &signedBuffLen);

        if (signedBuffLen > 0) {
#ifdef ENABLE_DATA_CHANNEL
            CHK_STATUS(putSctpPacket(pKvsPeerConnection->pSctpSession, buff, signedBuffLen));
#endif
        }

        CHK_STATUS(dtlsSessionIsInitFinished(pKvsPeerConnection->pDtlsSession, &isDtlsConnected));
        if (isDtlsConnected) {
            #if (ENABLE_STREAMING)
            if (pKvsPeerConnection->pSrtpSession == NULL) {
                CHK_STATUS(allocateSrtp(pKvsPeerConnection));
            }
            #endif

#ifdef ENABLE_DATA_CHANNEL
            if (pKvsPeerConnection->pSctpSession == NULL) {
                CHK_STATUS(allocateSctp(pKvsPeerConnection));
            }
#endif
        }

    } else if ((buff[0] > 127 && buff[0] < 192) && (pKvsPeerConnection->pSrtpSession != NULL)) {
        #if (ENABLE_STREAMING)
        if (buff[1] >= 192 && buff[1] <= 223) {
            if (STATUS_FAILED(retStatus = decryptSrtcpPacket(pKvsPeerConnection->pSrtpSession, buff, &signedBuffLen))) {
                DLOGW("decryptSrtcpPacket failed with 0x%08x", retStatus);
                CHK(FALSE, STATUS_SUCCESS);
            }

            CHK_STATUS(onRtcpPacket(pKvsPeerConnection, buff, signedBuffLen));
        } else {
            CHK_STATUS(sendPacketToRtpReceiver(pKvsPeerConnection, buff, signedBuffLen));
        }
        #endif
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
}

#if (ENABLE_STREAMING)
STATUS sendPacketToRtpReceiver(PKvsPeerConnection pKvsPeerConnection, PBYTE pBuffer, UINT32 bufferLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    PKvsRtpTransceiver pTransceiver;
    UINT64 item, now;
    UINT32 ssrc;
    PRtpPacket pRtpPacket = NULL;
    PBYTE pPayload = NULL;
    BOOL ownedByJitterBuffer = FALSE, discarded = FALSE;
    /** #stat. */
    UINT64 packetsReceived = 0, packetsFailedDecryption = 0, lastPacketReceivedTimestamp = 0, headerBytesReceived = 0, bytesReceived = 0,
           packetsDiscarded = 0;
    INT64 arrival, r_ts, transit, delta;

    CHK(pKvsPeerConnection != NULL && pBuffer != NULL, STATUS_NULL_ARG);
    CHK(bufferLen >= MIN_HEADER_LENGTH, STATUS_INVALID_ARG);

    /** get the ssrc from the header. */
    ssrc = getInt32(*(PUINT32)(pBuffer + SSRC_OFFSET));
    /** get the head of transceievers. */
    CHK_STATUS(doubleListGetHeadNode(pKvsPeerConnection->pTransceievers, &pCurNode));
    /** search the correct transceivers. */
    while (pCurNode != NULL) {
        /** get the data part of the current transceiever. */
        CHK_STATUS(doubleListGetNodeData(pCurNode, &item));
        pTransceiver = (PKvsRtpTransceiver) item;

        /** check the ssrc is correct or not. */
        if (pTransceiver->jitterBufferSsrc == ssrc) {
            packetsReceived++;
            if (STATUS_FAILED(retStatus = decryptSrtpPacket(pKvsPeerConnection->pSrtpSession, pBuffer, (PINT32) &bufferLen))) {
                DLOGW("decryptSrtpPacket failed with 0x%08x", retStatus);
                packetsFailedDecryption++;
                CHK(FALSE, STATUS_SUCCESS);
            }
            now = GETTIME();
            /** #memory. 
             * #YC_TBD, can we save this buffer? need to review this part. 
            */
            CHK(NULL != (pPayload = (PBYTE) MEMALLOC(bufferLen)), STATUS_NOT_ENOUGH_MEMORY);
            MEMCPY(pPayload, pBuffer, bufferLen);
            /** get the header of rtp packets. */
            CHK_STATUS(createRtpPacketFromBytes(pPayload, bufferLen, &pRtpPacket));
            pRtpPacket->receivedTime = now;

            /** Estimating the Interarrival Jitter */
            // https://tools.ietf.org/html/rfc3550#section-6.4.1
            // https://tools.ietf.org/html/rfc3550#appendix-A.8
            // interarrival jitter
            // arrival, the current time in the same units.
            // r_ts, the timestamp from   the incoming packet
            arrival = KVS_CONVERT_TIMESCALE(now, HUNDREDS_OF_NANOS_IN_A_SECOND, pTransceiver->pJitterBuffer->clockRate);
            r_ts = pRtpPacket->header.timestamp;
            transit = arrival - r_ts;
            delta = transit - pTransceiver->pJitterBuffer->transit;
            pTransceiver->pJitterBuffer->transit = transit;
            pTransceiver->pJitterBuffer->jitter += (1. / 16.) * ((DOUBLE) ABS(delta) - pTransceiver->pJitterBuffer->jitter);
            CHK_STATUS(jitterBufferPush(pTransceiver->pJitterBuffer, pRtpPacket, &discarded));
            /** #stat. */
            if (discarded) {
                packetsDiscarded++;
            }
            lastPacketReceivedTimestamp = KVS_CONVERT_TIMESCALE(now, HUNDREDS_OF_NANOS_IN_A_SECOND, 1000);
            /** #YC_TBD, #BUG. need to fix it. */
            headerBytesReceived += RTP_HEADER_LEN(pRtpPacket);
            bytesReceived += pRtpPacket->rawPacketLength - RTP_HEADER_LEN(pRtpPacket);
            ownedByJitterBuffer = TRUE;
            CHK(FALSE, STATUS_SUCCESS);
        }
        pCurNode = pCurNode->pNext;
    }

    DLOGW("No transceiver to handle inbound ssrc %u", ssrc);

CleanUp:
    if (packetsReceived > 0) {
        MUTEX_LOCK(pTransceiver->statsLock);
        pTransceiver->inboundStats.received.packetsReceived += packetsReceived;
        pTransceiver->inboundStats.packetsFailedDecryption += packetsFailedDecryption;
        pTransceiver->inboundStats.lastPacketReceivedTimestamp = lastPacketReceivedTimestamp;
        pTransceiver->inboundStats.headerBytesReceived += headerBytesReceived;
        pTransceiver->inboundStats.bytesReceived += bytesReceived;
        pTransceiver->inboundStats.received.jitter = pTransceiver->pJitterBuffer->jitter / pTransceiver->pJitterBuffer->clockRate;
        pTransceiver->inboundStats.received.packetsDiscarded = packetsDiscarded;
        MUTEX_UNLOCK(pTransceiver->statsLock);
    }
    if (!ownedByJitterBuffer) {
        SAFE_MEMFREE(pPayload);
        freeRtpPacket(&pRtpPacket);
        CHK_LOG_ERR(retStatus);
    }
    return retStatus;
}
#endif

STATUS changePeerConnectionState(PKvsPeerConnection pKvsPeerConnection, RTC_PEER_CONNECTION_STATE newState)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    /* new and closed state are terminal*/
    CHK(pKvsPeerConnection->connectionState != newState && pKvsPeerConnection->connectionState != RTC_PEER_CONNECTION_STATE_FAILED &&
            pKvsPeerConnection->connectionState != RTC_PEER_CONNECTION_STATE_CLOSED,
        retStatus);

    pKvsPeerConnection->connectionState = newState;
    MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = FALSE;

    if (pKvsPeerConnection->onConnectionStateChange != NULL) {
        pKvsPeerConnection->onConnectionStateChange(pKvsPeerConnection->onConnectionStateChangeCustomData, newState);
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

#if (ENABLE_STREAMING)
STATUS onFrameReadyFunc(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 frameSize)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pTransceiver = (PKvsRtpTransceiver) customData;
    PRtpPacket pPacket = NULL;
    Frame frame;
    UINT32 filledSize = 0;

    CHK(pTransceiver != NULL, STATUS_NULL_ARG);

    pPacket = pTransceiver->pJitterBuffer->pktBuffer[startIndex];
    // TODO: handle multi-packet frames
    CHK(pPacket != NULL, STATUS_NULL_ARG);
    MUTEX_LOCK(pTransceiver->statsLock);
    // https://www.w3.org/TR/webrtc-stats/#dom-rtcinboundrtpstreamstats-jitterbufferdelay
    pTransceiver->inboundStats.jitterBufferDelay += (DOUBLE)(GETTIME() - pPacket->receivedTime) / HUNDREDS_OF_NANOS_IN_A_SECOND;
    pTransceiver->inboundStats.jitterBufferEmittedCount++;
    if (MEDIA_STREAM_TRACK_KIND_VIDEO == pTransceiver->transceiver.receiver.track.kind) {
        pTransceiver->inboundStats.framesReceived++;
    }
    MUTEX_UNLOCK(pTransceiver->statsLock);
    /**
     * re-size the peer frame buffer if it is not enough.
     * #YC_TBD.
    */
    if (frameSize > pTransceiver->peerFrameBufferSize) {
        MEMFREE(pTransceiver->peerFrameBuffer);
        pTransceiver->peerFrameBufferSize = (UINT32)(frameSize * PEER_FRAME_BUFFER_SIZE_INCREMENT_FACTOR);
        pTransceiver->peerFrameBuffer = (PBYTE) MEMALLOC(pTransceiver->peerFrameBufferSize);
        CHK(pTransceiver->peerFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);
    }

    CHK_STATUS(jitterBufferFillFrameData(pTransceiver->pJitterBuffer, pTransceiver->peerFrameBuffer, frameSize, &filledSize, startIndex, endIndex));
    CHK(frameSize == filledSize, STATUS_INVALID_ARG_LEN);

    frame.version = FRAME_CURRENT_VERSION;
    frame.decodingTs = pPacket->header.timestamp * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
    frame.presentationTs = frame.decodingTs;
    frame.frameData = pTransceiver->peerFrameBuffer;
    frame.size = frameSize;
    frame.duration = 0;
    // TODO: Fill frame flag and track id and index if we need to, currently those are not used by RtcRtpTransceiver
    if (pTransceiver->onFrame != NULL) {
        pTransceiver->onFrame(pTransceiver->onFrameCustomData, &frame);
    }

CleanUp:

    return retStatus;
}

STATUS onFrameDroppedFunc(UINT64 customData, UINT16 startIndex, UINT16 endIndex, UINT32 timestamp)
{
    UNUSED_PARAM(endIndex);
    STATUS retStatus = STATUS_SUCCESS;
    PRtpPacket pPacket = NULL;
    PKvsRtpTransceiver pTransceiver = (PKvsRtpTransceiver) customData;
    DLOGW("Frame with timestamp %ld is dropped!", timestamp);
    CHK(pTransceiver != NULL, STATUS_NULL_ARG);
    pPacket = pTransceiver->pJitterBuffer->pktBuffer[startIndex];
    // TODO: handle multi-packet frames
    CHK(pPacket != NULL, STATUS_NULL_ARG);
    MUTEX_LOCK(pTransceiver->statsLock);
    // https://www.w3.org/TR/webrtc-stats/#dom-rtcinboundrtpstreamstats-jitterbufferdelay
    pTransceiver->inboundStats.jitterBufferDelay += (DOUBLE)(GETTIME() - pPacket->receivedTime) / HUNDREDS_OF_NANOS_IN_A_SECOND;
    pTransceiver->inboundStats.jitterBufferEmittedCount++;
    pTransceiver->inboundStats.received.framesDropped++;
    pTransceiver->inboundStats.received.fullFramesLost++;
    MUTEX_UNLOCK(pTransceiver->statsLock);

CleanUp:
    return retStatus;
}
#endif

VOID onIceConnectionStateChange(UINT64 customData, UINT64 connectionState)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    RTC_PEER_CONNECTION_STATE newConnectionState = RTC_PEER_CONNECTION_STATE_NEW;
    BOOL startDtlsSession = FALSE;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    switch (connectionState) {
        case ICE_AGENT_STATE_NEW:
            newConnectionState = RTC_PEER_CONNECTION_STATE_NEW;
            break;

        case ICE_AGENT_STATE_CHECK_CONNECTION:
            newConnectionState = RTC_PEER_CONNECTION_STATE_CONNECTING;
            break;

        case ICE_AGENT_STATE_CONNECTED:
            /* explicit fall-through */
        case ICE_AGENT_STATE_NOMINATING:
            /* explicit fall-through */
        case ICE_AGENT_STATE_READY:
            /* start dtlsSession as soon as ice is connected */
            startDtlsSession = TRUE;
            newConnectionState = RTC_PEER_CONNECTION_STATE_CONNECTED;
            break;

        case ICE_AGENT_STATE_DISCONNECTED:
            newConnectionState = RTC_PEER_CONNECTION_STATE_DISCONNECTED;
            break;

        case ICE_AGENT_STATE_FAILED:
            newConnectionState = RTC_PEER_CONNECTION_STATE_FAILED;
            break;

        default:
            DLOGW("Unknown ice agent state %" PRIu64, connectionState);
            break;
    }

    if (startDtlsSession) {
        /** Start DTLS handshake. Not thread safe. 
         * if ice agent is ready, we can start dtls session.
        */
        CHK_STATUS(dtlsSessionStart(pKvsPeerConnection->pDtlsSession, pKvsPeerConnection->dtlsIsServer));
    }

    CHK_STATUS(changePeerConnectionState(pKvsPeerConnection, newConnectionState));

CleanUp:

    CHK_LOG_ERR(retStatus);
}
/**
 * @brief the callback of local new candidate.
 *        example:
 *          {"candidate":"candidate:1 1 udp 1694498815 118.165.101.11 51917 typ srflx raddr 0.0.0.0 rport 0 generation 0 network-cost 999","sdpM
 * 
 * @param[in] the object of kvs peerconnection.
 * @param[in] 
*/
VOID onNewIceLocalCandidate(UINT64 customData, PCHAR candidateSdpStr)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    BOOL locked = FALSE;
    CHAR jsonStrBuffer[MAX_ICE_CANDIDATE_JSON_LEN];
    INT32 strCompleteLen = 0;
    PCHAR pIceCandidateStr = NULL;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    CHK(candidateSdpStr == NULL || STRLEN(candidateSdpStr) < MAX_SDP_ATTRIBUTE_VALUE_LENGTH, STATUS_INVALID_ARG);
    CHK(pKvsPeerConnection->onIceCandidate != NULL, retStatus); // do nothing if onIceCandidate is not implemented

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    if (candidateSdpStr != NULL) {
        /** ICE_CANDIDATE_JSON_TEMPLATE (PCHAR) "{\"candidate\":\"candidate:%s\",\"sdpMid\":\"0\",\"sdpMLineIndex\":0}" */
        strCompleteLen = SNPRINTF(jsonStrBuffer, ARRAY_SIZE(jsonStrBuffer), ICE_CANDIDATE_JSON_TEMPLATE, candidateSdpStr);
        CHK(strCompleteLen > 0, STATUS_INTERNAL_ERROR);
        CHK(strCompleteLen < (INT32) ARRAY_SIZE(jsonStrBuffer), STATUS_BUFFER_TOO_SMALL);
        pIceCandidateStr = jsonStrBuffer;
    }

    pKvsPeerConnection->onIceCandidate(pKvsPeerConnection->onIceCandidateCustomData, pIceCandidateStr);

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }
}

#ifdef ENABLE_DATA_CHANNEL
/**
 * @brief the callback of outbound sctp packets for webrtc client.
 * 
 * @param[]
 * @param[]
 * @param[]
*/
VOID onSctpSessionOutboundPacket(UINT64 customData, PBYTE pPacket, UINT32 packetLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    if (customData == 0) {
        return;
    }

    pKvsPeerConnection = (PKvsPeerConnection) customData;
    CHK_STATUS(dtlsSessionPutApplicationData(pKvsPeerConnection->pDtlsSession, pPacket, packetLen));

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        DLOGW("onSctpSessionOutboundPacket failed with 0x%08x", retStatus);
    }
}

VOID onSctpSessionDataChannelMessage(UINT64 customData, UINT32 channelId, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    PKvsDataChannel pKvsDataChannel = NULL;

    CHK(pKvsPeerConnection != NULL, STATUS_INTERNAL_ERROR);

    CHK_STATUS(hashTableGet(pKvsPeerConnection->pDataChannels, channelId, (PUINT64) &pKvsDataChannel));
    CHK(pKvsDataChannel != NULL && pKvsDataChannel->onMessage != NULL, STATUS_INTERNAL_ERROR);

    pKvsDataChannel->onMessage(pKvsDataChannel->onMessageCustomData, &pKvsDataChannel->dataChannel, isBinary, pMessage, pMessageLen);

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        DLOGW("onSctpSessionDataChannelMessage failed with 0x%08x", retStatus);
    }
}

VOID onSctpSessionDataChannelOpen(UINT64 customData, UINT32 channelId, PBYTE pName, UINT32 pNameLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) customData;
    PKvsDataChannel pKvsDataChannel = NULL;

    CHK(pKvsPeerConnection != NULL && pKvsPeerConnection->onDataChannel != NULL, STATUS_NULL_ARG);
    /** #memory. */
    pKvsDataChannel = (PKvsDataChannel) MEMCALLOC(1, SIZEOF(KvsDataChannel));
    CHK(pKvsDataChannel != NULL, STATUS_NOT_ENOUGH_MEMORY);

    STRNCPY(pKvsDataChannel->dataChannel.name, (PCHAR) pName, pNameLen);
    pKvsDataChannel->pRtcPeerConnection = (PRtcPeerConnection) pKvsPeerConnection;
    pKvsDataChannel->channelId = channelId;

    CHK_STATUS(hashTablePut(pKvsPeerConnection->pDataChannels, channelId, (UINT64) pKvsDataChannel));

    pKvsPeerConnection->onDataChannel(pKvsPeerConnection->onDataChannelCustomData, &(pKvsDataChannel->dataChannel));

CleanUp:

    CHK_LOG_ERR(retStatus);
}
#endif

/** 
 * @brief the callback of dtls outbound. #DTLS 
 * 
 * @param[]
 * @param[]
 * @param[]
*/
VOID onDtlsOutboundPacket(UINT64 customData, PBYTE pBuffer, UINT32 bufferLen)
{
    PKvsPeerConnection pKvsPeerConnection = NULL;
    if (customData == 0) {
        return;
    }

    pKvsPeerConnection = (PKvsPeerConnection) customData;
    iceAgentSendPacket(pKvsPeerConnection->pIceAgent, pBuffer, bufferLen);
}
/** #DTLS. */
VOID onDtlsStateChange(UINT64 customData, RTC_DTLS_TRANSPORT_STATE newDtlsState)
{
    PKvsPeerConnection pKvsPeerConnection = NULL;
    if (customData == 0) {
        return;
    }

    pKvsPeerConnection = (PKvsPeerConnection) customData;

    if (newDtlsState == CLOSED) {
        changePeerConnectionState(pKvsPeerConnection, RTC_PEER_CONNECTION_STATE_CLOSED);
    }
}

/* Generate a printable string that does not
 * need to be escaped when encoding in JSON
 */
STATUS generateJSONSafeString(PCHAR pDst, UINT32 len)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 i = 0;

    CHK(pDst != NULL, STATUS_NULL_ARG);

    for (i = 0; i < len; i++) {
        pDst[i] = VALID_CHAR_SET_FOR_JSON[RAND() % (ARRAY_SIZE(VALID_CHAR_SET_FOR_JSON) - 1)];
    }

CleanUp:

    LEAVES();
    return retStatus;
}

#if (ENABLE_STREAMING)
/**
 * @brief the callback of one peridic timer. It trigger itself periodically.
 *        Send the Sender Report RTCP Packet periodically after waiting for 2.5secs.
 * 
 * @param[]
 * @param[]
 * @param[]
 * 
*/
STATUS rtcpReportsCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    STATUS retStatus = STATUS_SUCCESS;
    BOOL ready = FALSE;
    UINT64 ntpTime, rtpTime, delay;
    UINT32 packetCount, octetCount, packetLen, allocSize, ssrc;
    PBYTE rawPacket = NULL;
    PKvsPeerConnection pKvsPeerConnection = NULL;

    PKvsRtpTransceiver pKvsRtpTransceiver = (PKvsRtpTransceiver) customData;
    CHK(pKvsRtpTransceiver != NULL && pKvsRtpTransceiver->pJitterBuffer != NULL && pKvsRtpTransceiver->pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    pKvsPeerConnection = pKvsRtpTransceiver->pKvsPeerConnection;

    ssrc = pKvsRtpTransceiver->sender.ssrc;
    DLOGS("rtcpReportsCallback %" PRIu64 " ssrc: %u rtxssrc: %u", currentTime, ssrc, pKvsRtpTransceiver->sender.rtxSsrc);

    // check if ice agent is connected, reschedule in 200msec if not
    ready = pKvsPeerConnection->pSrtpSession != NULL &&
            currentTime - pKvsRtpTransceiver->sender.firstFrameWallClockTime >= 2500 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;

    if (!ready) {
        DLOGV("sender report no frames sent %u", ssrc);
    } else {
        // create rtcp sender report packet
        // https://tools.ietf.org/html/rfc3550#section-6.4.1
        ntpTime = convertTimestampToNTP(currentTime);
        rtpTime = pKvsRtpTransceiver->sender.rtpTimeOffset +
            CONVERT_TIMESTAMP_TO_RTP(pKvsRtpTransceiver->pJitterBuffer->clockRate, currentTime - pKvsRtpTransceiver->sender.firstFrameWallClockTime);

        /** #stat */
        MUTEX_LOCK(pKvsRtpTransceiver->statsLock);
        packetCount = pKvsRtpTransceiver->outboundStats.sent.packetsSent;
        octetCount = pKvsRtpTransceiver->outboundStats.sent.bytesSent;
        MUTEX_UNLOCK(pKvsRtpTransceiver->statsLock);

        DLOGV("sender report %u %" PRIu64 " %" PRIu64 " : %u packets %u bytes", ssrc, ntpTime, rtpTime, packetCount, octetCount);
        packetLen = RTCP_PACKET_HEADER_LEN + 24;

        // srtp_protect_rtcp() in encryptRtcpPacket() assumes memory availability to write 10 bytes of authentication tag and
        // SRTP_MAX_TRAILER_LEN + 4 following the actual rtcp Packet payload
        allocSize = packetLen + SRTP_AUTH_TAG_OVERHEAD + SRTP_MAX_TRAILER_LEN + 4;
        /** #memory. */
        CHK(NULL != (rawPacket = (PBYTE) MEMALLOC(allocSize)), STATUS_NOT_ENOUGH_MEMORY);
        /** #YC_TBD, need to be implemented. */
        rawPacket[0] = RTCP_PACKET_VERSION_VAL << 6;
        rawPacket[RTCP_PACKET_TYPE_OFFSET] = RTCP_PACKET_TYPE_SENDER_REPORT;
        putUnalignedInt16BigEndian(rawPacket + RTCP_PACKET_LEN_OFFSET,
                                   (packetLen / RTCP_PACKET_LEN_WORD_SIZE) - 1); // The length of this RTCP packet in 32-bit words minus one
        putUnalignedInt32BigEndian(rawPacket + 4, ssrc);
        putUnalignedInt64BigEndian(rawPacket + 8, ntpTime);
        putUnalignedInt32BigEndian(rawPacket + 16, rtpTime);
        putUnalignedInt32BigEndian(rawPacket + 20, packetCount);
        putUnalignedInt32BigEndian(rawPacket + 24, octetCount);

        /** the encryption of rtcp packet. */
        CHK_STATUS(encryptRtcpPacket(pKvsPeerConnection->pSrtpSession, rawPacket, (PINT32) &packetLen));
        CHK_STATUS(iceAgentSendPacket(pKvsPeerConnection->pIceAgent, rawPacket, packetLen));
    }

    delay = 100 + (RAND() % 200);
    DLOGS("next sender report %u in %" PRIu64 " msec", ssrc, delay);
    // reschedule timer with 200msec +- 100ms
    CHK_STATUS(timerQueueAddTimer(pKvsPeerConnection->timerQueueHandle,
                                  delay * HUNDREDS_OF_NANOS_IN_A_MILLISECOND,
                                  TIMER_QUEUE_SINGLE_INVOCATION_PERIOD,
                                  rtcpReportsCallback,
                                  (UINT64) pKvsRtpTransceiver,
                                  &pKvsRtpTransceiver->rtcpReportsTimerId));

CleanUp:
    CHK_LOG_ERR(retStatus);
    SAFE_MEMFREE(rawPacket);
    return retStatus;
}
#endif
/**
 * @brief Initialize a RtcPeerConnection with the provided Configuration
 *
 * Reference: https://www.w3.org/TR/webrtc/#constructor
 *
 * @param[in] PConfiguration Configuration to initialize provided RtcPeerConnection
 * @param[in,out] PRtcPeerConnection Uninitialized RtcPeerConnection
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS createPeerConnection(PRtcConfiguration pConfiguration, PRtcPeerConnection* ppPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = NULL;
    IceAgentCallbacks iceAgentCallbacks;
    /** the callback of the dtls sessions. */
    DtlsSessionCallbacks dtlsSessionCallbacks;
    UINT32 logLevel = LOG_LEVEL_VERBOSE;
    PCHAR logLevelStr = NULL;
    PConnectionListener pConnectionListener = NULL;

    CHK(pConfiguration != NULL && ppPeerConnection != NULL, STATUS_NULL_ARG);

    MEMSET(&iceAgentCallbacks, 0, SIZEOF(IceAgentCallbacks));
    MEMSET(&dtlsSessionCallbacks, 0, SIZEOF(DtlsSessionCallbacks));
    /** #memory. */
    pKvsPeerConnection = (PKvsPeerConnection) MEMCALLOC(1, SIZEOF(KvsPeerConnection));
    CHK(pKvsPeerConnection != NULL, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(timerQueueCreate(&pKvsPeerConnection->timerQueueHandle));

    pKvsPeerConnection->peerConnection.version = PEER_CONNECTION_CURRENT_VERSION;
    /** generate the ufrag and pwd of ice randomly. */
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIceUfrag, LOCAL_ICE_UFRAG_LEN));
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIcePwd, LOCAL_ICE_PWD_LEN));
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localCNAME, LOCAL_CNAME_LEN));
    /** #DTLS.*/
    CHK_STATUS(createDtlsSession(&dtlsSessionCallbacks, 
                                    pKvsPeerConnection->timerQueueHandle, 
                                    pConfiguration->kvsRtcConfiguration.generatedCertificateBits,
                                    pConfiguration->kvsRtcConfiguration.generateRSACertificate, 
                                    pConfiguration->certificates, 
                                    &pKvsPeerConnection->pDtlsSession));

    CHK_STATUS(dtlsSessionOnOutBoundData(pKvsPeerConnection->pDtlsSession, (UINT64) pKvsPeerConnection, onDtlsOutboundPacket));
    CHK_STATUS(dtlsSessionOnStateChange(pKvsPeerConnection->pDtlsSession, (UINT64) pKvsPeerConnection, onDtlsStateChange));
    /** #codec for rtp? */
    CHK_STATUS(hashTableCreateWithParams(CODEC_HASH_TABLE_BUCKET_COUNT, CODEC_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pCodecTable));
    /** #data-channel. */
    CHK_STATUS(hashTableCreateWithParams(CODEC_HASH_TABLE_BUCKET_COUNT, CODEC_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pDataChannels));
    /** */
    CHK_STATUS(hashTableCreateWithParams(RTX_HASH_TABLE_BUCKET_COUNT, RTX_HASH_TABLE_BUCKET_LENGTH, &pKvsPeerConnection->pRtxTable));
    CHK_STATUS(doubleListCreate(&(pKvsPeerConnection->pTransceievers)));
    /** #YC_TBD, */
    #if 0
    if ((logLevelStr = GETENV(DEBUG_LOG_LEVEL_ENV_VAR)) != NULL) {
        CHK_STATUS(STRTOUI32(logLevelStr, NULL, 10, &logLevel));
        logLevel = MIN(MAX(logLevel, LOG_LEVEL_VERBOSE), LOG_LEVEL_SILENT);
    }
    #endif
    SET_LOGGER_LOG_LEVEL(logLevel);

    pKvsPeerConnection->pSrtpSessionLock = MUTEX_CREATE(TRUE);
    pKvsPeerConnection->peerConnectionObjLock = MUTEX_CREATE(FALSE);
    pKvsPeerConnection->connectionState = RTC_PEER_CONNECTION_STATE_NONE;
    pKvsPeerConnection->MTU = pConfiguration->kvsRtcConfiguration.maximumTransmissionUnit == 0
        ? DEFAULT_MTU_SIZE
        : pConfiguration->kvsRtcConfiguration.maximumTransmissionUnit;

    iceAgentCallbacks.customData = (UINT64) pKvsPeerConnection;
    iceAgentCallbacks.inboundPacketFn = onInboundPacket;
    iceAgentCallbacks.connectionStateChangedFn = onIceConnectionStateChange;
    iceAgentCallbacks.newLocalCandidateFn = onNewIceLocalCandidate;
    CHK_STATUS(createConnectionListener(&pConnectionListener));
    // IceAgent will own the lifecycle of pConnectionListener;
    /**
     * #ice.
    */
    CHK_STATUS(createIceAgent(pKvsPeerConnection->localIceUfrag, 
                              pKvsPeerConnection->localIcePwd, 
                              &iceAgentCallbacks, 
                              pConfiguration,
                              pKvsPeerConnection->timerQueueHandle, 
                              pConnectionListener, 
                              &pKvsPeerConnection->pIceAgent));

    NULLABLE_SET_EMPTY(pKvsPeerConnection->canTrickleIce);

    *ppPeerConnection = (PRtcPeerConnection) pKvsPeerConnection;

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (STATUS_FAILED(retStatus)) {
        freePeerConnection((PRtcPeerConnection*) &pKvsPeerConnection);
    }

    LEAVES();
    return retStatus;
}

STATUS freeHashEntry(UINT64 customData, PHashEntry pHashEntry)
{
    UNUSED_PARAM(customData);
    MEMFREE((PVOID) pHashEntry->value);
    return STATUS_SUCCESS;
}

/*
 * NOT thread-safe
 */
STATUS freePeerConnection(PRtcPeerConnection* ppPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection;
    PDoubleListNode pCurNode = NULL;
    UINT64 item = 0;

    CHK(ppPeerConnection != NULL, STATUS_NULL_ARG);

    pKvsPeerConnection = (PKvsPeerConnection) *ppPeerConnection;

    CHK(pKvsPeerConnection != NULL, retStatus);

    /* Shutdown IceAgent first so there is no more incoming packets which can cause
     * SCTP to be allocated again after SCTP is freed. */
    CHK_LOG_ERR(iceAgentShutdown(pKvsPeerConnection->pIceAgent));

    // free timer queue first to remove liveness provided by timer
    if (IS_VALID_TIMER_QUEUE_HANDLE(pKvsPeerConnection->timerQueueHandle)) {
        timerQueueShutdown(pKvsPeerConnection->timerQueueHandle);
    }

/* Free structs that have their own thread. SCTP has threads created by SCTP library. IceAgent has the
 * connectionListener thread. Free SCTP first so it wont try to send anything through ICE. */
#ifdef ENABLE_DATA_CHANNEL
    CHK_LOG_ERR(freeSctpSession(&pKvsPeerConnection->pSctpSession));
#endif
    CHK_LOG_ERR(freeIceAgent(&pKvsPeerConnection->pIceAgent));

    #if (ENABLE_STREAMING)
    // free transceivers
    CHK_LOG_ERR(doubleListGetHeadNode(pKvsPeerConnection->pTransceievers, &pCurNode));
    while (pCurNode != NULL) {
        CHK_LOG_ERR(doubleListGetNodeData(pCurNode, &item));
        CHK_LOG_ERR(freeKvsRtpTransceiver((PKvsRtpTransceiver*) &item));

        pCurNode = pCurNode->pNext;
    }
    #endif

#ifdef ENABLE_DATA_CHANNEL
    // Free DataChannels
    CHK_LOG_ERR(hashTableIterateEntries(pKvsPeerConnection->pDataChannels, 0, freeHashEntry));
    CHK_LOG_ERR(hashTableFree(pKvsPeerConnection->pDataChannels));
#endif

    // free rest of structs
    #if (ENABLE_STREAMING)
    CHK_LOG_ERR(freeSrtpSession(&pKvsPeerConnection->pSrtpSession));
    #endif

    CHK_LOG_ERR(freeDtlsSession(&pKvsPeerConnection->pDtlsSession));

    #if (ENABLE_STREAMING)
    CHK_LOG_ERR(doubleListFree(pKvsPeerConnection->pTransceievers));
    CHK_LOG_ERR(hashTableFree(pKvsPeerConnection->pCodecTable));
    CHK_LOG_ERR(hashTableFree(pKvsPeerConnection->pRtxTable));
    if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->pSrtpSessionLock)) {
        MUTEX_FREE(pKvsPeerConnection->pSrtpSessionLock);
    }
    #endif

    if (IS_VALID_MUTEX_VALUE(pKvsPeerConnection->peerConnectionObjLock)) {
        MUTEX_FREE(pKvsPeerConnection->peerConnectionObjLock);
    }

    if (IS_VALID_TIMER_QUEUE_HANDLE(pKvsPeerConnection->timerQueueHandle)) {
        timerQueueFree(&pKvsPeerConnection->timerQueueHandle);
    }

    SAFE_MEMFREE(pKvsPeerConnection);

    *ppPeerConnection = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}
/**
 * @brief Set a callback when new Ice collects new local candidate.
 *
 * NOTE: When IceAgent is done with collecting candidates,
 * RtcOnIceCandidate will be called with NULL.
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in] UINT64 User customData that will be passed along when RtcOnIceCandidate is called
 * @param[in] RtcOnIceCandidate User callback when new local candidate is found
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS peerConnectionOnIceCandidate(PRtcPeerConnection pRtcPeerConnection, UINT64 customData, RtcOnIceCandidate rtcOnIceCandidate)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL && rtcOnIceCandidate != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onIceCandidate = rtcOnIceCandidate;
    pKvsPeerConnection->onIceCandidateCustomData = customData;

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }

    LEAVES();
    return retStatus;
}
#ifdef ENABLE_DATA_CHANNEL
STATUS peerConnectionOnDataChannel(PRtcPeerConnection pRtcPeerConnection, UINT64 customData, RtcOnDataChannel rtcOnDataChannel)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL && rtcOnDataChannel != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onDataChannel = rtcOnDataChannel;
    pKvsPeerConnection->onDataChannelCustomData = customData;

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }

    LEAVES();
    return retStatus;
}
#endif

STATUS peerConnectionOnConnectionStateChange(PRtcPeerConnection pRtcPeerConnection,
                                             UINT64 customData,
                                             RtcOnConnectionStateChange rtcOnConnectionStateChange)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;
    BOOL locked = FALSE;

    CHK(pKvsPeerConnection != NULL && rtcOnConnectionStateChange != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pKvsPeerConnection->peerConnectionObjLock);
    locked = TRUE;

    pKvsPeerConnection->onConnectionStateChange = rtcOnConnectionStateChange;
    pKvsPeerConnection->onConnectionStateChangeCustomData = customData;

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pKvsPeerConnection->peerConnectionObjLock);
    }

    LEAVES();
    return retStatus;
}
/**
 * Load the sdp field of PRtcSessionDescriptionInit with pending or current local session description
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in,out] PRtcSessionDescriptionInit IN/PRtcSessionDescriptionInit whose sdp field will be modified.
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS peerConnectionGetLocalDescription(PRtcPeerConnection pRtcPeerConnection, PRtcSessionDescriptionInit pRtcSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSessionDescription pSessionDescription = NULL;
    UINT32 serializeLen = 0;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    CHK(pRtcPeerConnection != NULL && pRtcSessionDescriptionInit != NULL, STATUS_NULL_ARG);
    /** #memory. */
    CHK(NULL != (pSessionDescription = (PSessionDescription) MEMCALLOC(1, SIZEOF(SessionDescription))), STATUS_NOT_ENOUGH_MEMORY);

    if (pKvsPeerConnection->isOffer) {
        pRtcSessionDescriptionInit->type = SDP_TYPE_OFFER;
    } else {
        pRtcSessionDescriptionInit->type = SDP_TYPE_ANSWER;
    }

    CHK_STATUS(populateSessionDescription(pKvsPeerConnection, &(pKvsPeerConnection->remoteSessionDescription), pSessionDescription));
    CHK_STATUS(serializeSessionDescription(pSessionDescription, NULL, &serializeLen));
    CHK(serializeLen <= MAX_SESSION_DESCRIPTION_INIT_SDP_LEN, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(serializeSessionDescription(pSessionDescription, pRtcSessionDescriptionInit->sdp, &serializeLen));

CleanUp:

    SAFE_MEMFREE(pSessionDescription);

    LEAVES();
    return retStatus;
}
/**
 * Load the sdp field of PRtcSessionDescriptionInit with current local session description
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in,out] PRtcSessionDescriptionInit IN/PRtcSessionDescriptionInit whose sdp field will be modified.
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS peerConnectionGetCurrentLocalDescription(PRtcPeerConnection pRtcPeerConnection, PRtcSessionDescriptionInit pRtcSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSessionDescription pSessionDescription = NULL;
    UINT32 serializeLen = 0;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pRtcPeerConnection;

    CHK(pRtcPeerConnection != NULL && pRtcSessionDescriptionInit != NULL, STATUS_NULL_ARG);
    // do nothing if remote session description hasn't been received
    CHK(pKvsPeerConnection->remoteSessionDescription.sessionName[0] != '\0', retStatus);
    /** #memory. */
    CHK(NULL != (pSessionDescription = (PSessionDescription) MEMCALLOC(1, SIZEOF(SessionDescription))), STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(populateSessionDescription(pKvsPeerConnection, &(pKvsPeerConnection->remoteSessionDescription), pSessionDescription));
    CHK_STATUS(serializeSessionDescription(pSessionDescription, NULL, &serializeLen));
    CHK(serializeLen <= MAX_SESSION_DESCRIPTION_INIT_SDP_LEN, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(serializeSessionDescription(pSessionDescription, pRtcSessionDescriptionInit->sdp, &serializeLen));

CleanUp:

    SAFE_MEMFREE(pSessionDescription);

    LEAVES();
    return retStatus;
}
/**
 * @brief Instructs the RtcPeerConnection to apply
 * the supplied RtcSessionDescriptionInit as the remote description.
 *
 * Reference: https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-setremotedescription
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in,out] PRtcSessionDescriptionInit IN/RtcSessionDescriptionInit that becomes our new remote description
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS setRemoteDescription(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR remoteIceUfrag = NULL, remoteIcePwd = NULL;
    UINT32 i, j;

    CHK(pPeerConnection != NULL, STATUS_NULL_ARG);
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    PSessionDescription pSessionDescription = &pKvsPeerConnection->remoteSessionDescription;

    CHK(pSessionDescriptionInit != NULL, STATUS_NULL_ARG);

    MEMSET(pSessionDescription, 0x00, SIZEOF(SessionDescription));
    pKvsPeerConnection->dtlsIsServer = FALSE;
    /* Assume cant trickle at first */
    NULLABLE_SET_VALUE(pKvsPeerConnection->canTrickleIce, FALSE);

    CHK_STATUS(deserializeSessionDescription(pSessionDescription, pSessionDescriptionInit->sdp));

    for (i = 0; i < pSessionDescription->sessionAttributesCount; i++) {
        if (STRCMP(pSessionDescription->sdpAttributes[i].attributeName, "fingerprint") == 0) {
            STRNCPY(pKvsPeerConnection->remoteCertificateFingerprint, pSessionDescription->sdpAttributes[i].attributeValue + 8,
                    CERTIFICATE_FINGERPRINT_LENGTH);
        } else if (pKvsPeerConnection->isOffer && STRCMP(pSessionDescription->sdpAttributes[i].attributeName, "setup") == 0) {
            pKvsPeerConnection->dtlsIsServer = STRCMP(pSessionDescription->sdpAttributes[i].attributeValue, "active") == 0;
        }
    }

    for (i = 0; i < pSessionDescription->mediaCount; i++) {
#ifdef ENABLE_DATA_CHANNEL
        if (STRNCMP(pSessionDescription->mediaDescriptions[i].mediaName, "application", SIZEOF("application") - 1) == 0) {
            pKvsPeerConnection->sctpIsEnabled = TRUE;
        }
#endif

        for (j = 0; j < pSessionDescription->mediaDescriptions[i].mediaAttributesCount; j++) {
            if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "ice-ufrag") == 0) {
                remoteIceUfrag = pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue;
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "ice-pwd") == 0) {
                remoteIcePwd = pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue;
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "candidate") == 0) {
                // Ignore the return value, we have candidates we don't support yet like TURN
                iceAgentAddRemoteCandidate(pKvsPeerConnection->pIceAgent, pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue);
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "fingerprint") == 0) {
                STRNCPY(pKvsPeerConnection->remoteCertificateFingerprint,
                        pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue + 8, CERTIFICATE_FINGERPRINT_LENGTH);
            } else if (pKvsPeerConnection->isOffer &&
                       STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "setup") == 0) {
                pKvsPeerConnection->dtlsIsServer = STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue, "active") == 0;
            } else if (STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeName, "ice-options") == 0 &&
                       STRCMP(pSessionDescription->mediaDescriptions[i].sdpAttributes[j].attributeValue, "trickle") == 0) {
                NULLABLE_SET_VALUE(pKvsPeerConnection->canTrickleIce, TRUE);
            }
        }
    }

    CHK(remoteIceUfrag != NULL && remoteIcePwd != NULL, STATUS_SESSION_DESCRIPTION_MISSING_ICE_VALUES);
    CHK(pKvsPeerConnection->remoteCertificateFingerprint[0] != '\0', STATUS_SESSION_DESCRIPTION_MISSING_CERTIFICATE_FINGERPRINT);
    /** if the ufrag and pwd of remote ice agent is not null, we need to latch it and restart local ice agent. 
     * #YC_TBD, need to check it.
    */
    if (!IS_EMPTY_STRING(pKvsPeerConnection->remoteIceUfrag) && 
        !IS_EMPTY_STRING(pKvsPeerConnection->remoteIcePwd) &&
        STRNCMP(pKvsPeerConnection->remoteIceUfrag, remoteIceUfrag, MAX_ICE_UFRAG_LEN) != 0 &&
        STRNCMP(pKvsPeerConnection->remoteIcePwd, remoteIcePwd, MAX_ICE_PWD_LEN) != 0) {
        CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIceUfrag, LOCAL_ICE_UFRAG_LEN));
        CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIcePwd, LOCAL_ICE_PWD_LEN));
        CHK_STATUS(iceAgentRestart(pKvsPeerConnection->pIceAgent, pKvsPeerConnection->localIceUfrag, pKvsPeerConnection->localIcePwd));
        CHK_STATUS(iceAgentStartGathering(pKvsPeerConnection->pIceAgent));
    }

    STRNCPY(pKvsPeerConnection->remoteIceUfrag, remoteIceUfrag, MAX_ICE_UFRAG_LEN);
    STRNCPY(pKvsPeerConnection->remoteIcePwd, remoteIcePwd, MAX_ICE_PWD_LEN);

    CHK_STATUS(iceAgentStartAgent(pKvsPeerConnection->pIceAgent,
                                  pKvsPeerConnection->remoteIceUfrag,
                                  pKvsPeerConnection->remoteIcePwd,
                                  pKvsPeerConnection->isOffer));
    #if (ENABLE_STREAMING)
    if (!pKvsPeerConnection->isOffer) {
        CHK_STATUS(setPayloadTypesFromOffer(pKvsPeerConnection->pCodecTable, pKvsPeerConnection->pRtxTable, pSessionDescription));
    }
    CHK_STATUS(setTransceiverPayloadTypes(pKvsPeerConnection->pCodecTable, pKvsPeerConnection->pRtxTable, pKvsPeerConnection->pTransceievers));
    CHK_STATUS(setReceiversSsrc(pSessionDescription, pKvsPeerConnection->pTransceievers));
    #endif
    #if 0
    if (NULL != getenv(DEBUG_LOG_SDP)) 
    {
        DLOGD("REMOTE_SDP:%s\n", pSessionDescriptionInit->sdp);
    }
    #endif
CleanUp:

    LEAVES();
    return retStatus;
}

/**
 * @brief Populate the provided answer that contains an RFC 3264 offer
 * with the supported configurations for the session.
 *
 * Reference: https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-createoffer
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in,out] PRtcSessionDescriptionInit IN/answer that describes the supported configurations of the RtcPeerConnection
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS createOffer(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSessionDescription pSessionDescription = NULL;
    UINT32 serializeLen = 0;

    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pSessionDescriptionInit != NULL, STATUS_NULL_ARG);

    // SessionDescription is large enough structure to not define on the stack and use heap memory
    CHK(NULL != (pSessionDescription = (PSessionDescription) MEMCALLOC(1, SIZEOF(SessionDescription))), STATUS_NOT_ENOUGH_MEMORY);
    pSessionDescriptionInit->type = SDP_TYPE_OFFER;
    pKvsPeerConnection->isOffer = TRUE;

#ifdef ENABLE_DATA_CHANNEL
    pKvsPeerConnection->sctpIsEnabled = TRUE;
#endif
    #if (ENABLE_STREAMING)
    CHK_STATUS(setPayloadTypesForOffer(pKvsPeerConnection->pCodecTable));
    #endif

    CHK_STATUS(populateSessionDescription(pKvsPeerConnection, &(pKvsPeerConnection->remoteSessionDescription), pSessionDescription));
    CHK_STATUS(serializeSessionDescription(pSessionDescription, NULL, &serializeLen));
    CHK(serializeLen <= MAX_SESSION_DESCRIPTION_INIT_SDP_LEN, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(serializeSessionDescription(pSessionDescription, pSessionDescriptionInit->sdp, &serializeLen));

CleanUp:

    SAFE_MEMFREE(pSessionDescription);

    LEAVES();
    return retStatus;
}
/**
 * @brief Populate the provided answer that contains an RFC 3264 answer
 * with the supported configurations for the session.
 *
 * Reference: https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-createanswer
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in,out] PRtcSessionDescriptionInit IN/answer that describes the supported configurations of the RtcPeerConnection
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS createAnswer(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pSessionDescriptionInit != NULL, STATUS_NULL_ARG);
    CHK(pKvsPeerConnection->remoteSessionDescription.sessionName[0] != '\0', STATUS_PEERCONNECTION_CREATE_ANSWER_WITHOUT_REMOTE_DESCRIPTION);

    pSessionDescriptionInit->type = SDP_TYPE_ANSWER;

    CHK_STATUS(peerConnectionGetCurrentLocalDescription(pPeerConnection, pSessionDescriptionInit));

CleanUp:

    LEAVES();
    return retStatus;
}
/**
 * @brief Instructs the RtcPeerConnection to apply the supplied RtcSessionDescriptionInit
 * as the local description.
 *
 * Reference: https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-setlocaldescription
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param [in,out]PRtcSessionDescriptionInit IN/SessionDescriptionInit that becomes our new local description
 *
 * @return - STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS setLocalDescription(PRtcPeerConnection pPeerConnection, PRtcSessionDescriptionInit pSessionDescriptionInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pSessionDescriptionInit != NULL, STATUS_NULL_ARG);

    CHK_STATUS(iceAgentStartGathering(pKvsPeerConnection->pIceAgent));
    #if 0
    if (NULL != getenv(DEBUG_LOG_SDP)) 
    {
        DLOGD("LOCAL_SDP:%s", pSessionDescriptionInit->sdp);
    }
    #endif
CleanUp:

    LEAVES();
    return retStatus;
}

#if (ENABLE_STREAMING)
/**
 * @brief Create a new RtcRtpTransceiver and add it to the set of transceivers.
 *
 * Reference https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addtransceiver
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in] PRtcMediaStreamTrack Stream track information for the codec appropriate codec, or NULL for RECVONLY
 * @param[in] PRtcRtpTransceiverInit PRtcRtpTransceiverInit that may configure our new Transceiver
 * @param[in,out] PRtcRtpTransceiver* IN/Initialized and configured RtcRtpTransceiver
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS addTransceiver(PRtcPeerConnection pPeerConnection,
                      PRtcMediaStreamTrack pRtcMediaStreamTrack,
                      PRtcRtpTransceiverInit pRtcRtpTransceiverInit,
                      PRtcRtpTransceiver* ppRtcRtpTransceiver)
{
    //UNUSED_PARAM(pRtcRtpTransceiverInit);
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsRtpTransceiver pKvsRtpTransceiver = NULL;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    PJitterBuffer pJitterBuffer = NULL;
    DepayRtpPayloadFunc depayFunc;
    UINT32 clockRate = 0;
    UINT32 ssrc = (UINT32) RAND(), rtxSsrc = (UINT32) RAND();
    //RTC_RTP_TRANSCEIVER_DIRECTION direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
    //RTC_RTP_TRANSCEIVER_DIRECTION direction = RTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY;
    RTC_RTP_TRANSCEIVER_DIRECTION direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;
    

    if (pRtcRtpTransceiverInit != NULL) {
        direction = pRtcRtpTransceiverInit->direction;
    }

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    /**
     * set up the configuration of the codec.
    */
    switch (pRtcMediaStreamTrack->codec) {
        case RTC_CODEC_OPUS:
            depayFunc = depayOpusFromRtpPayload;
            clockRate = OPUS_CLOCKRATE;
            break;

        case RTC_CODEC_MULAW:
        case RTC_CODEC_ALAW:
            depayFunc = depayG711FromRtpPayload;
            clockRate = PCM_CLOCKRATE;
            break;

        case RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE:
            depayFunc = depayH264FromRtpPayload;
            clockRate = VIDEO_CLOCKRATE;
            break;

        case RTC_CODEC_VP8:
            depayFunc = depayVP8FromRtpPayload;
            clockRate = VIDEO_CLOCKRATE;
            break;

        default:
            CHK(FALSE, STATUS_NOT_IMPLEMENTED);
    }

    // TODO: Add ssrc duplicate detection here not only relying on RAND()
    CHK_STATUS(createKvsRtpTransceiver(direction,
                                       pKvsPeerConnection,
                                       ssrc,
                                       rtxSsrc,
                                       pRtcMediaStreamTrack,
                                       NULL,
                                       pRtcMediaStreamTrack->codec,
                                       &pKvsRtpTransceiver));

    CHK_STATUS(createJitterBuffer(onFrameReadyFunc,
                                  onFrameDroppedFunc,
                                  depayFunc,
                                  DEFAULT_JITTER_BUFFER_MAX_LATENCY,
                                  clockRate,
                                  (UINT64) pKvsRtpTransceiver,
                                  &pJitterBuffer));
    /** link the jitter buffer with transceiver. */
    CHK_STATUS(kvsRtpTransceiverSetJitterBuffer(pKvsRtpTransceiver, pJitterBuffer));

    // after pKvsRtpTransceiver is successfully created, jitterBuffer will be freed by pKvsRtpTransceiver.
    pJitterBuffer = NULL;

    CHK_STATUS(doubleListInsertItemHead(pKvsPeerConnection->pTransceievers, (UINT64) pKvsRtpTransceiver));
    *ppRtcRtpTransceiver = (PRtcRtpTransceiver) pKvsRtpTransceiver;
    /** #timer periodic timer. */
    CHK_STATUS(timerQueueAddTimer(pKvsPeerConnection->timerQueueHandle,
                                  RTCP_FIRST_REPORT_DELAY,
                                  TIMER_QUEUE_SINGLE_INVOCATION_PERIOD,
                                  rtcpReportsCallback,
                                  (UINT64) pKvsRtpTransceiver,
                                  &pKvsRtpTransceiver->rtcpReportsTimerId));

    pKvsRtpTransceiver = NULL;

CleanUp:

    if (pJitterBuffer != NULL) {
        freeJitterBuffer(&pJitterBuffer);
    }

    if (pKvsRtpTransceiver != NULL) {
        freeKvsRtpTransceiver(&pKvsRtpTransceiver);
    }

    LEAVES();
    return retStatus;
}

STATUS addSupportedCodec(PRtcPeerConnection pPeerConnection, RTC_CODEC rtcCodec)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    CHK_STATUS(hashTablePut(pKvsPeerConnection->pCodecTable, rtcCodec, 0));

CleanUp:

    LEAVES();
    return retStatus;
}
#endif
/**
 * @brief Provides a remote candidate to the ICE Agent.
 *
 * This method can also be used to indicate the end of remote candidates
 * when called with an empty string for the candidate member.
 *
 * Reference: https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addicecandidate
 *
 * @param[in] PRtcPeerConnection Initialized RtcPeerConnection
 * @param[in] PCHAR New remote ICE candidate to add
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
/**
 * retrieving the information of ice candidates from wss messages, and adding it into ice agent.
*/
STATUS addIceCandidate(PRtcPeerConnection pPeerConnection, PCHAR pIceCandidate)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL && pIceCandidate != NULL, STATUS_NULL_ARG);

    iceAgentAddRemoteCandidate(pKvsPeerConnection->pIceAgent, pIceCandidate);

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS restartIce(PRtcPeerConnection pPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);

    /* generate new local uFrag and uPwd and clear out remote uFrag and uPwd */
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIceUfrag, LOCAL_ICE_UFRAG_LEN));
    CHK_STATUS(generateJSONSafeString(pKvsPeerConnection->localIcePwd, LOCAL_ICE_PWD_LEN));
    pKvsPeerConnection->remoteIceUfrag[0] = '\0';
    pKvsPeerConnection->remoteIcePwd[0] = '\0';
    CHK_STATUS(iceAgentRestart(pKvsPeerConnection->pIceAgent, pKvsPeerConnection->localIceUfrag, pKvsPeerConnection->localIcePwd));

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS closePeerConnection(PRtcPeerConnection pPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    CHK_LOG_ERR(dtlsSessionShutdown(pKvsPeerConnection->pDtlsSession));
    CHK_LOG_ERR(iceAgentShutdown(pKvsPeerConnection->pIceAgent));

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

PUBLIC_API NullableBool canTrickleIceCandidates(PRtcPeerConnection pPeerConnection)
{
    NullableBool canTrickle = {FALSE, FALSE};
    PKvsPeerConnection pKvsPeerConnection = (PKvsPeerConnection) pPeerConnection;
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pKvsPeerConnection != NULL, STATUS_NULL_ARG);
    if (pKvsPeerConnection != NULL) {
        canTrickle = pKvsPeerConnection->canTrickleIce;
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
    return canTrickle;
}

STATUS initKvsWebRtc(VOID)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHK(!ATOMIC_LOAD_BOOL(&gKvsWebRtcInitialized), retStatus);

    SRAND(GETTIME());

    #if (ENABLE_STREAMING)
    CHK(srtp_init() == srtp_err_status_ok, STATUS_SRTP_INIT_FAILED);
    #endif

    // init endianness handling
    initializeEndianness();

    KVS_CRYPTO_INIT();

#ifdef ENABLE_DATA_CHANNEL
    CHK_STATUS(initSctpSession());
#endif

    ATOMIC_STORE_BOOL(&gKvsWebRtcInitialized, TRUE);

CleanUp:

    LEAVES();
    return retStatus;
}

STATUS deinitKvsWebRtc(VOID)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHK(ATOMIC_LOAD_BOOL(&gKvsWebRtcInitialized), retStatus);

#ifdef ENABLE_DATA_CHANNEL
    deinitSctpSession();
#endif
    #if (ENABLE_STREAMING)
    srtp_shutdown();
    #endif

    ATOMIC_STORE_BOOL(&gKvsWebRtcInitialized, FALSE);

CleanUp:

    LEAVES();
    return retStatus;
}

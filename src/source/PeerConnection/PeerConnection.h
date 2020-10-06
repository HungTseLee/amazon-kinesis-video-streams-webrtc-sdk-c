/*******************************************
PeerConnection internal include file
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PEERCONNECTION__
#define __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PEERCONNECTION__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define LOCAL_ICE_UFRAG_LEN 4   //!< at least 24bits of randomness. be at least 4 characters long
#define LOCAL_ICE_PWD_LEN   24  //!< at least 128bits of randomness. be at least 22 characters long
#define LOCAL_CNAME_LEN     16

// https://tools.ietf.org/html/rfc5245#section-15.4
 #define MAX_ICE_UFRAG_LEN 256   //!< The upper limit allows for buffer sizing in implementations.
#define MAX_ICE_PWD_LEN   256   //!< The upper limit allows for buffer sizing in implementations.

#define PEER_FRAME_BUFFER_SIZE_INCREMENT_FACTOR 1.5

// A non-comprehensive list of valid JSON characters
#define VALID_CHAR_SET_FOR_JSON "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/"

#define ICE_CANDIDATE_JSON_TEMPLATE (PCHAR) "{\"candidate\":\"candidate:%s\",\"sdpMid\":\"0\",\"sdpMLineIndex\":0}"

#define MAX_ICE_CANDIDATE_JSON_LEN (MAX_SDP_ATTRIBUTE_VALUE_LENGTH + SIZEOF(ICE_CANDIDATE_JSON_TEMPLATE) + 1)

#define CODEC_HASH_TABLE_BUCKET_COUNT  50
#define CODEC_HASH_TABLE_BUCKET_LENGTH 2
#define RTX_HASH_TABLE_BUCKET_COUNT    50
#define RTX_HASH_TABLE_BUCKET_LENGTH   2

#define DATA_CHANNEL_HASH_TABLE_BUCKET_COUNT  200
#define DATA_CHANNEL_HASH_TABLE_BUCKET_LENGTH 2

// Environment variable to display SDPs
#define DEBUG_LOG_SDP ((PCHAR) "DEBUG_LOG_SDP")

typedef enum {
    RTC_RTX_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE = 1,
    RTC_RTX_CODEC_VP8 = 2,
} RTX_CODEC;

typedef struct {
    RtcPeerConnection peerConnection;
    PIceAgent pIceAgent;
    PDtlsSession pDtlsSession;
    BOOL dtlsIsServer;//!< indicate the roles of dtls session. server or client.

    MUTEX pSrtpSessionLock;
    PSrtpSession pSrtpSession;
#ifdef ENABLE_DATA_CHANNEL
    PSctpSession pSctpSession;//!< the handler of the sctp session.
#endif
    SessionDescription remoteSessionDescription;
    PDoubleList pTransceievers;
    BOOL sctpIsEnabled;
    // https://tools.ietf.org/html/rfc5245#section-15.4
    CHAR localIceUfrag[LOCAL_ICE_UFRAG_LEN + 1];
    CHAR localIcePwd[LOCAL_ICE_PWD_LEN + 1];
    /** 
     * since this is from remote, we have to keep it as the maximum. 
     * The spec left this open, so the better way is we keep it open too.
     * #YC_TBD, need to be fixed. 
     * retrieve remote ice ufrag and pwd for the remote description.
     * */
    CHAR remoteIceUfrag[MAX_ICE_UFRAG_LEN + 1];
    CHAR remoteIcePwd[MAX_ICE_PWD_LEN + 1];
    /** this is used for the the sdp session. */
    CHAR localCNAME[LOCAL_CNAME_LEN + 1];

    CHAR remoteCertificateFingerprint[CERTIFICATE_FINGERPRINT_LENGTH + 1];

    MUTEX peerConnectionObjLock;

    BOOL isOffer;//!< do you create the offer by yourself. it means you are a client or not.

    TIMER_QUEUE_HANDLE timerQueueHandle;

    // Codecs that we support and their payloadTypes
    // When offering we generate values starting from 96
    // When answering this is populated from the remote offer
    PHashTable pCodecTable;

    // Payload types that we use to retransmit data
    // When answering this is populated from the remote offer
    PHashTable pRtxTable;

    // DataChannels keyed by streamId
    PHashTable pDataChannels;//!< stores the information of data channel. streamid, channel name...etc.

    UINT64 onDataChannelCustomData;
    RtcOnDataChannel onDataChannel;//!< the callback of data channel for the external interface of the peer connection.

    UINT64 onIceCandidateCustomData;
    /** https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-onicecandidate */
    RtcOnIceCandidate onIceCandidate;

    UINT64 onConnectionStateChangeCustomData;
    RtcOnConnectionStateChange onConnectionStateChange;
    RTC_PEER_CONNECTION_STATE connectionState;

    UINT16 MTU;

    NullableBool canTrickleIce;
} KvsPeerConnection, *PKvsPeerConnection;
#ifdef ENABLE_DATA_CHANNEL
typedef struct {
    UINT32 currentDataChannelId;//!< indicate the role of dtls server.
    PKvsPeerConnection pKvsPeerConnection;
    PHashTable unkeyedDataChannels;
} AllocateSctpSortDataChannelsData, *PAllocateSctpSortDataChannelsData;
#endif
STATUS onFrameReadyFunc(UINT64, UINT16, UINT16, UINT32);
STATUS onFrameDroppedFunc(UINT64, UINT16, UINT16, UINT32);
VOID onSctpSessionOutboundPacket(UINT64, PBYTE, UINT32);
/**
 * @brief the callback of the notification of receiving message over data channel for the internal sctp interface.
 * 
 * @param[in] customData the object of peer connection.
 * @param[in] the channel id
 * @param[in] isBinary bianry or string.
 * @param[in] pMessage the buffer of message.
 * @param[in] pMessageLen the length of the message buffer.
*/
VOID onSctpSessionDataChannelMessage(UINT64, UINT32, BOOL, PBYTE, UINT32);
/**
 * @brief the callback of the notification of opening data channel for the internal sctp interface.
 * 
 * @param[in] customData the object of peer connection.
 * @param[in] channelId the channel id
 * @param[in] pName the buffer of channel name
 * @param[in] nameLen the length of the channel name buffer.
*/
VOID onSctpSessionDataChannelOpen(UINT64, UINT32, PBYTE, UINT32);
/**
 * @brief   receive the rtp packet and send it to rtp-receiver.
 * 
 *              https://tools.ietf.org/html/rfc3550#section-5
 * 
 *              0                   1                   2                   3
 *              0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *              +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *              |V=2|P|X|  CC   |M|     PT      |       sequence number         |
 *              +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *              |                           timestamp                           |
 *              +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *              |           synchronization source (SSRC) identifier            |
 *              +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
 *              |            contributing source (CSRC) identifiers             |
 *              |                             ....                              |
 *              +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 
 * 
 * @param[in] pKvsPeerConnection
 * @param[in] pBuffer
 * @param[in] bufferLen
*/
STATUS sendPacketToRtpReceiver(PKvsPeerConnection, PBYTE, UINT32);
STATUS changePeerConnectionState(PKvsPeerConnection, RTC_PEER_CONNECTION_STATE);

#ifdef __cplusplus
}
#endif
#endif /* __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PEERCONNECTION__ */

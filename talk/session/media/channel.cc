/*
 * libjingle
 * Copyright 2004 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "talk/session/media/channel.h"

#include "talk/media/base/constants.h"
#include "talk/media/base/rtputils.h"
#include "webrtc/p2p/base/transportchannel.h"
#include "talk/session/media/channelmanager.h"
#include "webrtc/base/bind.h"
#include "webrtc/base/buffer.h"
#include "webrtc/base/byteorder.h"
#include "webrtc/base/common.h"
#include "webrtc/base/dscp.h"
#include "webrtc/base/logging.h"

namespace cricket {

using rtc::Bind;

enum {
  MSG_EARLYMEDIATIMEOUT = 1,
  MSG_SCREENCASTWINDOWEVENT,
  MSG_RTPPACKET,
  MSG_RTCPPACKET,
  MSG_CHANNEL_ERROR,
  MSG_READYTOSENDDATA,
  MSG_DATARECEIVED,
  MSG_FIRSTPACKETRECEIVED,
  MSG_STREAMCLOSEDREMOTELY,
};

// Value specified in RFC 5764.
static const char kDtlsSrtpExporterLabel[] = "EXTRACTOR-dtls_srtp";

static const int kAgcMinus10db = -10;

static void SafeSetError(const std::string& message, std::string* error_desc) {
  if (error_desc) {
    *error_desc = message;
  }
}

struct PacketMessageData : public rtc::MessageData {
  rtc::Buffer packet;
  rtc::DiffServCodePoint dscp;
};

struct ScreencastEventMessageData : public rtc::MessageData {
  ScreencastEventMessageData(uint32_t s, rtc::WindowEvent we)
      : ssrc(s), event(we) {}
  uint32_t ssrc;
  rtc::WindowEvent event;
};

struct VoiceChannelErrorMessageData : public rtc::MessageData {
  VoiceChannelErrorMessageData(uint32_t in_ssrc,
                               VoiceMediaChannel::Error in_error)
      : ssrc(in_ssrc), error(in_error) {}
  uint32_t ssrc;
  VoiceMediaChannel::Error error;
};

struct VideoChannelErrorMessageData : public rtc::MessageData {
  VideoChannelErrorMessageData(uint32_t in_ssrc,
                               VideoMediaChannel::Error in_error)
      : ssrc(in_ssrc), error(in_error) {}
  uint32_t ssrc;
  VideoMediaChannel::Error error;
};

struct DataChannelErrorMessageData : public rtc::MessageData {
  DataChannelErrorMessageData(uint32_t in_ssrc,
                              DataMediaChannel::Error in_error)
      : ssrc(in_ssrc), error(in_error) {}
  uint32_t ssrc;
  DataMediaChannel::Error error;
};


struct VideoChannel::ScreencastDetailsData {
  explicit ScreencastDetailsData(uint32_t s)
      : ssrc(s), fps(0), screencast_max_pixels(0) {}
  uint32_t ssrc;
  int fps;
  int screencast_max_pixels;
};

static const char* PacketType(bool rtcp) {
  return (!rtcp) ? "RTP" : "RTCP";
}

static bool ValidPacket(bool rtcp, const rtc::Buffer* packet) {
  // Check the packet size. We could check the header too if needed.
  return (packet &&
          packet->size() >= (!rtcp ? kMinRtpPacketLen : kMinRtcpPacketLen) &&
          packet->size() <= kMaxRtpPacketLen);
}

static bool IsReceiveContentDirection(MediaContentDirection direction) {
  return direction == MD_SENDRECV || direction == MD_RECVONLY;
}

static bool IsSendContentDirection(MediaContentDirection direction) {
  return direction == MD_SENDRECV || direction == MD_SENDONLY;
}

static const MediaContentDescription* GetContentDescription(
    const ContentInfo* cinfo) {
  if (cinfo == NULL)
    return NULL;
  return static_cast<const MediaContentDescription*>(cinfo->description);
}

template <class Codec>
void RtpParametersFromMediaDescription(
    const MediaContentDescriptionImpl<Codec>* desc,
    RtpParameters<Codec>* params) {
  // TODO(pthatcher): Remove this once we're sure no one will give us
  // a description without codecs (currently a CA_UPDATE with just
  // streams can).
  if (desc->has_codecs()) {
    params->codecs = desc->codecs();
  }
  // TODO(pthatcher): See if we really need
  // rtp_header_extensions_set() and remove it if we don't.
  if (desc->rtp_header_extensions_set()) {
    params->extensions = desc->rtp_header_extensions();
  }
}

template <class Codec, class Options>
void RtpSendParametersFromMediaDescription(
    const MediaContentDescriptionImpl<Codec>* desc,
    RtpSendParameters<Codec, Options>* send_params) {
  RtpParametersFromMediaDescription(desc, send_params);
  send_params->max_bandwidth_bps = desc->bandwidth();
}

BaseChannel::BaseChannel(rtc::Thread* thread,
                         MediaChannel* media_channel,
                         TransportController* transport_controller,
                         const std::string& content_name,
                         bool rtcp)
    : worker_thread_(thread),
      transport_controller_(transport_controller),
      media_channel_(media_channel),
      content_name_(content_name),
      rtcp_transport_enabled_(rtcp),
      transport_channel_(nullptr),
      rtcp_transport_channel_(nullptr),
      enabled_(false),
      writable_(false),
      rtp_ready_to_send_(false),
      rtcp_ready_to_send_(false),
      was_ever_writable_(false),
      local_content_direction_(MD_INACTIVE),
      remote_content_direction_(MD_INACTIVE),
      has_received_packet_(false),
      dtls_keyed_(false),
      secure_required_(false),
      rtp_abs_sendtime_extn_id_(-1) {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  LOG(LS_INFO) << "Created channel for " << content_name;
}

BaseChannel::~BaseChannel() {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  Deinit();
  StopConnectionMonitor();
  FlushRtcpMessages();  // Send any outstanding RTCP packets.
  worker_thread_->Clear(this);  // eats any outstanding messages or packets
  // We must destroy the media channel before the transport channel, otherwise
  // the media channel may try to send on the dead transport channel. NULLing
  // is not an effective strategy since the sends will come on another thread.
  delete media_channel_;
  // Note that we don't just call set_transport_channel(nullptr) because that
  // would call a pure virtual method which we can't do from a destructor.
  if (transport_channel_) {
    DisconnectFromTransportChannel(transport_channel_);
    transport_controller_->DestroyTransportChannel_w(
        transport_name_, cricket::ICE_CANDIDATE_COMPONENT_RTP);
  }
  if (rtcp_transport_channel_) {
    DisconnectFromTransportChannel(rtcp_transport_channel_);
    transport_controller_->DestroyTransportChannel_w(
        transport_name_, cricket::ICE_CANDIDATE_COMPONENT_RTCP);
  }
  LOG(LS_INFO) << "Destroyed channel";
}

bool BaseChannel::Init() {
  if (!SetTransport(content_name())) {
    return false;
  }

  if (!SetDtlsSrtpCiphers(transport_channel(), false)) {
    return false;
  }
  if (rtcp_transport_enabled() &&
      !SetDtlsSrtpCiphers(rtcp_transport_channel(), true)) {
    return false;
  }

  // Both RTP and RTCP channels are set, we can call SetInterface on
  // media channel and it can set network options.
  media_channel_->SetInterface(this);
  return true;
}

void BaseChannel::Deinit() {
  media_channel_->SetInterface(NULL);
}

bool BaseChannel::SetTransport(const std::string& transport_name) {
  return worker_thread_->Invoke<bool>(
      Bind(&BaseChannel::SetTransport_w, this, transport_name));
}

bool BaseChannel::SetTransport_w(const std::string& transport_name) {
  ASSERT(worker_thread_ == rtc::Thread::Current());

  if (transport_name == transport_name_) {
    // Nothing to do if transport name isn't changing
    return true;
  }

  set_transport_channel(transport_controller_->CreateTransportChannel_w(
      transport_name, cricket::ICE_CANDIDATE_COMPONENT_RTP));
  if (!transport_channel()) {
    return false;
  }
  if (rtcp_transport_enabled()) {
    LOG(LS_INFO) << "Create RTCP TransportChannel for " << content_name()
                 << " on " << transport_name << " transport ";
    set_rtcp_transport_channel(transport_controller_->CreateTransportChannel_w(
        transport_name, cricket::ICE_CANDIDATE_COMPONENT_RTCP));
    if (!rtcp_transport_channel()) {
      return false;
    }
  }

  transport_name_ = transport_name;
  return true;
}

void BaseChannel::set_transport_channel(TransportChannel* new_tc) {
  ASSERT(worker_thread_ == rtc::Thread::Current());

  TransportChannel* old_tc = transport_channel_;
  if (!old_tc && !new_tc) {
    // Nothing to do
    return;
  }
  ASSERT(old_tc != new_tc);

  if (old_tc) {
    DisconnectFromTransportChannel(old_tc);
    transport_controller_->DestroyTransportChannel_w(
        transport_name_, cricket::ICE_CANDIDATE_COMPONENT_RTP);
  }

  transport_channel_ = new_tc;

  if (new_tc) {
    ConnectToTransportChannel(new_tc);
    for (const auto& pair : socket_options_) {
      new_tc->SetOption(pair.first, pair.second);
    }
  }

  // Update aggregate writable/ready-to-send state between RTP and RTCP upon
  // setting new channel
  UpdateWritableState_w();
  SetReadyToSend(false, new_tc && new_tc->writable());
}

void BaseChannel::set_rtcp_transport_channel(TransportChannel* new_tc) {
  ASSERT(worker_thread_ == rtc::Thread::Current());

  TransportChannel* old_tc = rtcp_transport_channel_;
  if (!old_tc && !new_tc) {
    // Nothing to do
    return;
  }
  ASSERT(old_tc != new_tc);

  if (old_tc) {
    DisconnectFromTransportChannel(old_tc);
    transport_controller_->DestroyTransportChannel_w(
        transport_name_, cricket::ICE_CANDIDATE_COMPONENT_RTCP);
  }

  rtcp_transport_channel_ = new_tc;

  if (new_tc) {
    ConnectToTransportChannel(new_tc);
    for (const auto& pair : rtcp_socket_options_) {
      new_tc->SetOption(pair.first, pair.second);
    }
  }

  // Update aggregate writable/ready-to-send state between RTP and RTCP upon
  // setting new channel
  UpdateWritableState_w();
  SetReadyToSend(true, new_tc && new_tc->writable());
}

void BaseChannel::ConnectToTransportChannel(TransportChannel* tc) {
  ASSERT(worker_thread_ == rtc::Thread::Current());

  tc->SignalWritableState.connect(this, &BaseChannel::OnWritableState);
  tc->SignalReadPacket.connect(this, &BaseChannel::OnChannelRead);
  tc->SignalReadyToSend.connect(this, &BaseChannel::OnReadyToSend);
}

void BaseChannel::DisconnectFromTransportChannel(TransportChannel* tc) {
  ASSERT(worker_thread_ == rtc::Thread::Current());

  tc->SignalWritableState.disconnect(this);
  tc->SignalReadPacket.disconnect(this);
  tc->SignalReadyToSend.disconnect(this);
}

bool BaseChannel::Enable(bool enable) {
  worker_thread_->Invoke<void>(Bind(
      enable ? &BaseChannel::EnableMedia_w : &BaseChannel::DisableMedia_w,
      this));
  return true;
}

bool BaseChannel::AddRecvStream(const StreamParams& sp) {
  return InvokeOnWorker(Bind(&BaseChannel::AddRecvStream_w, this, sp));
}

bool BaseChannel::RemoveRecvStream(uint32_t ssrc) {
  return InvokeOnWorker(Bind(&BaseChannel::RemoveRecvStream_w, this, ssrc));
}

bool BaseChannel::AddSendStream(const StreamParams& sp) {
  return InvokeOnWorker(
      Bind(&MediaChannel::AddSendStream, media_channel(), sp));
}

bool BaseChannel::RemoveSendStream(uint32_t ssrc) {
  return InvokeOnWorker(
      Bind(&MediaChannel::RemoveSendStream, media_channel(), ssrc));
}

bool BaseChannel::SetLocalContent(const MediaContentDescription* content,
                                  ContentAction action,
                                  std::string* error_desc) {
  return InvokeOnWorker(Bind(&BaseChannel::SetLocalContent_w,
                             this, content, action, error_desc));
}

bool BaseChannel::SetRemoteContent(const MediaContentDescription* content,
                                   ContentAction action,
                                   std::string* error_desc) {
  return InvokeOnWorker(Bind(&BaseChannel::SetRemoteContent_w,
                             this, content, action, error_desc));
}

void BaseChannel::StartConnectionMonitor(int cms) {
  // We pass in the BaseChannel instead of the transport_channel_
  // because if the transport_channel_ changes, the ConnectionMonitor
  // would be pointing to the wrong TransportChannel.
  connection_monitor_.reset(new ConnectionMonitor(
      this, worker_thread(), rtc::Thread::Current()));
  connection_monitor_->SignalUpdate.connect(
      this, &BaseChannel::OnConnectionMonitorUpdate);
  connection_monitor_->Start(cms);
}

void BaseChannel::StopConnectionMonitor() {
  if (connection_monitor_) {
    connection_monitor_->Stop();
    connection_monitor_.reset();
  }
}

bool BaseChannel::GetConnectionStats(ConnectionInfos* infos) {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  return transport_channel_->GetStats(infos);
}

bool BaseChannel::IsReadyToReceive() const {
  // Receive data if we are enabled and have local content,
  return enabled() && IsReceiveContentDirection(local_content_direction_);
}

bool BaseChannel::IsReadyToSend() const {
  // Send outgoing data if we are enabled, have local and remote content,
  // and we have had some form of connectivity.
  return enabled() &&
         IsReceiveContentDirection(remote_content_direction_) &&
         IsSendContentDirection(local_content_direction_) &&
         was_ever_writable();
}

bool BaseChannel::SendPacket(rtc::Buffer* packet,
                             rtc::DiffServCodePoint dscp) {
  return SendPacket(false, packet, dscp);
}

bool BaseChannel::SendRtcp(rtc::Buffer* packet,
                           rtc::DiffServCodePoint dscp) {
  return SendPacket(true, packet, dscp);
}

int BaseChannel::SetOption(SocketType type, rtc::Socket::Option opt,
                           int value) {
  TransportChannel* channel = NULL;
  switch (type) {
    case ST_RTP:
      channel = transport_channel_;
      socket_options_.push_back(
          std::pair<rtc::Socket::Option, int>(opt, value));
      break;
    case ST_RTCP:
      channel = rtcp_transport_channel_;
      rtcp_socket_options_.push_back(
          std::pair<rtc::Socket::Option, int>(opt, value));
      break;
  }
  return channel ? channel->SetOption(opt, value) : -1;
}

void BaseChannel::OnWritableState(TransportChannel* channel) {
  ASSERT(channel == transport_channel_ || channel == rtcp_transport_channel_);
  UpdateWritableState_w();
}

void BaseChannel::OnChannelRead(TransportChannel* channel,
                                const char* data, size_t len,
                                const rtc::PacketTime& packet_time,
                                int flags) {
  // OnChannelRead gets called from P2PSocket; now pass data to MediaEngine
  ASSERT(worker_thread_ == rtc::Thread::Current());

  // When using RTCP multiplexing we might get RTCP packets on the RTP
  // transport. We feed RTP traffic into the demuxer to determine if it is RTCP.
  bool rtcp = PacketIsRtcp(channel, data, len);
  rtc::Buffer packet(data, len);
  HandlePacket(rtcp, &packet, packet_time);
}

void BaseChannel::OnReadyToSend(TransportChannel* channel) {
  ASSERT(channel == transport_channel_ || channel == rtcp_transport_channel_);
  SetReadyToSend(channel == rtcp_transport_channel_, true);
}

void BaseChannel::SetReadyToSend(bool rtcp, bool ready) {
  if (rtcp) {
    rtcp_ready_to_send_ = ready;
  } else {
    rtp_ready_to_send_ = ready;
  }

  if (rtp_ready_to_send_ &&
      // In the case of rtcp mux |rtcp_transport_channel_| will be null.
      (rtcp_ready_to_send_ || !rtcp_transport_channel_)) {
    // Notify the MediaChannel when both rtp and rtcp channel can send.
    media_channel_->OnReadyToSend(true);
  } else {
    // Notify the MediaChannel when either rtp or rtcp channel can't send.
    media_channel_->OnReadyToSend(false);
  }
}

bool BaseChannel::PacketIsRtcp(const TransportChannel* channel,
                               const char* data, size_t len) {
  return (channel == rtcp_transport_channel_ ||
          rtcp_mux_filter_.DemuxRtcp(data, static_cast<int>(len)));
}

bool BaseChannel::SendPacket(bool rtcp, rtc::Buffer* packet,
                             rtc::DiffServCodePoint dscp) {
  // SendPacket gets called from MediaEngine, typically on an encoder thread.
  // If the thread is not our worker thread, we will post to our worker
  // so that the real work happens on our worker. This avoids us having to
  // synchronize access to all the pieces of the send path, including
  // SRTP and the inner workings of the transport channels.
  // The only downside is that we can't return a proper failure code if
  // needed. Since UDP is unreliable anyway, this should be a non-issue.
  if (rtc::Thread::Current() != worker_thread_) {
    // Avoid a copy by transferring the ownership of the packet data.
    int message_id = (!rtcp) ? MSG_RTPPACKET : MSG_RTCPPACKET;
    PacketMessageData* data = new PacketMessageData;
    data->packet = packet->Pass();
    data->dscp = dscp;
    worker_thread_->Post(this, message_id, data);
    return true;
  }

  // Now that we are on the correct thread, ensure we have a place to send this
  // packet before doing anything. (We might get RTCP packets that we don't
  // intend to send.) If we've negotiated RTCP mux, send RTCP over the RTP
  // transport.
  TransportChannel* channel = (!rtcp || rtcp_mux_filter_.IsActive()) ?
      transport_channel_ : rtcp_transport_channel_;
  if (!channel || !channel->writable()) {
    return false;
  }

  // Protect ourselves against crazy data.
  if (!ValidPacket(rtcp, packet)) {
    LOG(LS_ERROR) << "Dropping outgoing " << content_name_ << " "
                  << PacketType(rtcp)
                  << " packet: wrong size=" << packet->size();
    return false;
  }

  rtc::PacketOptions options(dscp);
  // Protect if needed.
  if (srtp_filter_.IsActive()) {
    bool res;
    uint8_t* data = packet->data();
    int len = static_cast<int>(packet->size());
    if (!rtcp) {
    // If ENABLE_EXTERNAL_AUTH flag is on then packet authentication is not done
    // inside libsrtp for a RTP packet. A external HMAC module will be writing
    // a fake HMAC value. This is ONLY done for a RTP packet.
    // Socket layer will update rtp sendtime extension header if present in
    // packet with current time before updating the HMAC.
#if !defined(ENABLE_EXTERNAL_AUTH)
      res = srtp_filter_.ProtectRtp(
          data, len, static_cast<int>(packet->capacity()), &len);
#else
      options.packet_time_params.rtp_sendtime_extension_id =
          rtp_abs_sendtime_extn_id_;
      res = srtp_filter_.ProtectRtp(
          data, len, static_cast<int>(packet->capacity()), &len,
          &options.packet_time_params.srtp_packet_index);
      // If protection succeeds, let's get auth params from srtp.
      if (res) {
        uint8_t* auth_key = NULL;
        int key_len;
        res = srtp_filter_.GetRtpAuthParams(
            &auth_key, &key_len, &options.packet_time_params.srtp_auth_tag_len);
        if (res) {
          options.packet_time_params.srtp_auth_key.resize(key_len);
          options.packet_time_params.srtp_auth_key.assign(auth_key,
                                                          auth_key + key_len);
        }
      }
#endif
      if (!res) {
        int seq_num = -1;
        uint32_t ssrc = 0;
        GetRtpSeqNum(data, len, &seq_num);
        GetRtpSsrc(data, len, &ssrc);
        LOG(LS_ERROR) << "Failed to protect " << content_name_
                      << " RTP packet: size=" << len
                      << ", seqnum=" << seq_num << ", SSRC=" << ssrc;
        return false;
      }
    } else {
      res = srtp_filter_.ProtectRtcp(data, len,
                                     static_cast<int>(packet->capacity()),
                                     &len);
      if (!res) {
        int type = -1;
        GetRtcpType(data, len, &type);
        LOG(LS_ERROR) << "Failed to protect " << content_name_
                      << " RTCP packet: size=" << len << ", type=" << type;
        return false;
      }
    }

    // Update the length of the packet now that we've added the auth tag.
    packet->SetSize(len);
  } else if (secure_required_) {
    // This is a double check for something that supposedly can't happen.
    LOG(LS_ERROR) << "Can't send outgoing " << PacketType(rtcp)
                  << " packet when SRTP is inactive and crypto is required";

    ASSERT(false);
    return false;
  }

  // Bon voyage.
  int ret =
      channel->SendPacket(packet->data<char>(), packet->size(), options,
                          (secure() && secure_dtls()) ? PF_SRTP_BYPASS : 0);
  if (ret != static_cast<int>(packet->size())) {
    if (channel->GetError() == EWOULDBLOCK) {
      LOG(LS_WARNING) << "Got EWOULDBLOCK from socket.";
      SetReadyToSend(rtcp, false);
    }
    return false;
  }
  return true;
}

bool BaseChannel::WantsPacket(bool rtcp, rtc::Buffer* packet) {
  // Protect ourselves against crazy data.
  if (!ValidPacket(rtcp, packet)) {
    LOG(LS_ERROR) << "Dropping incoming " << content_name_ << " "
                  << PacketType(rtcp)
                  << " packet: wrong size=" << packet->size();
    return false;
  }

  // Bundle filter handles both rtp and rtcp packets.
  return bundle_filter_.DemuxPacket(packet->data<char>(), packet->size(), rtcp);
}

void BaseChannel::HandlePacket(bool rtcp, rtc::Buffer* packet,
                               const rtc::PacketTime& packet_time) {
  if (!WantsPacket(rtcp, packet)) {
    return;
  }

  // We are only interested in the first rtp packet because that
  // indicates the media has started flowing.
  if (!has_received_packet_ && !rtcp) {
    has_received_packet_ = true;
    signaling_thread()->Post(this, MSG_FIRSTPACKETRECEIVED);
  }

  // Unprotect the packet, if needed.
  if (srtp_filter_.IsActive()) {
    char* data = packet->data<char>();
    int len = static_cast<int>(packet->size());
    bool res;
    if (!rtcp) {
      res = srtp_filter_.UnprotectRtp(data, len, &len);
      if (!res) {
        int seq_num = -1;
        uint32_t ssrc = 0;
        GetRtpSeqNum(data, len, &seq_num);
        GetRtpSsrc(data, len, &ssrc);
        LOG(LS_ERROR) << "Failed to unprotect " << content_name_
                      << " RTP packet: size=" << len
                      << ", seqnum=" << seq_num << ", SSRC=" << ssrc;
        return;
      }
    } else {
      res = srtp_filter_.UnprotectRtcp(data, len, &len);
      if (!res) {
        int type = -1;
        GetRtcpType(data, len, &type);
        LOG(LS_ERROR) << "Failed to unprotect " << content_name_
                      << " RTCP packet: size=" << len << ", type=" << type;
        return;
      }
    }

    packet->SetSize(len);
  } else if (secure_required_) {
    // Our session description indicates that SRTP is required, but we got a
    // packet before our SRTP filter is active. This means either that
    // a) we got SRTP packets before we received the SDES keys, in which case
    //    we can't decrypt it anyway, or
    // b) we got SRTP packets before DTLS completed on both the RTP and RTCP
    //    channels, so we haven't yet extracted keys, even if DTLS did complete
    //    on the channel that the packets are being sent on. It's really good
    //    practice to wait for both RTP and RTCP to be good to go before sending
    //    media, to prevent weird failure modes, so it's fine for us to just eat
    //    packets here. This is all sidestepped if RTCP mux is used anyway.
    LOG(LS_WARNING) << "Can't process incoming " << PacketType(rtcp)
                    << " packet when SRTP is inactive and crypto is required";
    return;
  }

  // Push it down to the media channel.
  if (!rtcp) {
    media_channel_->OnPacketReceived(packet, packet_time);
  } else {
    media_channel_->OnRtcpReceived(packet, packet_time);
  }
}

bool BaseChannel::PushdownLocalDescription(
    const SessionDescription* local_desc, ContentAction action,
    std::string* error_desc) {
  const ContentInfo* content_info = GetFirstContent(local_desc);
  const MediaContentDescription* content_desc =
      GetContentDescription(content_info);
  if (content_desc && content_info && !content_info->rejected &&
      !SetLocalContent(content_desc, action, error_desc)) {
    LOG(LS_ERROR) << "Failure in SetLocalContent with action " << action;
    return false;
  }
  return true;
}

bool BaseChannel::PushdownRemoteDescription(
    const SessionDescription* remote_desc, ContentAction action,
    std::string* error_desc) {
  const ContentInfo* content_info = GetFirstContent(remote_desc);
  const MediaContentDescription* content_desc =
      GetContentDescription(content_info);
  if (content_desc && content_info && !content_info->rejected &&
      !SetRemoteContent(content_desc, action, error_desc)) {
    LOG(LS_ERROR) << "Failure in SetRemoteContent with action " << action;
    return false;
  }
  return true;
}

void BaseChannel::EnableMedia_w() {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  if (enabled_)
    return;

  LOG(LS_INFO) << "Channel enabled";
  enabled_ = true;
  ChangeState();
}

void BaseChannel::DisableMedia_w() {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  if (!enabled_)
    return;

  LOG(LS_INFO) << "Channel disabled";
  enabled_ = false;
  ChangeState();
}

void BaseChannel::UpdateWritableState_w() {
  if (transport_channel_ && transport_channel_->writable() &&
      (!rtcp_transport_channel_ || rtcp_transport_channel_->writable())) {
    ChannelWritable_w();
  } else {
    ChannelNotWritable_w();
  }
}

void BaseChannel::ChannelWritable_w() {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  if (writable_)
    return;

  LOG(LS_INFO) << "Channel writable (" << content_name_ << ")"
               << (was_ever_writable_ ? "" : " for the first time");

  std::vector<ConnectionInfo> infos;
  transport_channel_->GetStats(&infos);
  for (std::vector<ConnectionInfo>::const_iterator it = infos.begin();
       it != infos.end(); ++it) {
    if (it->best_connection) {
      LOG(LS_INFO) << "Using " << it->local_candidate.ToSensitiveString()
                   << "->" << it->remote_candidate.ToSensitiveString();
      break;
    }
  }

  // If we're doing DTLS-SRTP, now is the time.
  if (!was_ever_writable_ && ShouldSetupDtlsSrtp()) {
    if (!SetupDtlsSrtp(false)) {
      SignalDtlsSetupFailure_w(false);
      return;
    }

    if (rtcp_transport_channel_) {
      if (!SetupDtlsSrtp(true)) {
        SignalDtlsSetupFailure_w(true);
        return;
      }
    }
  }

  was_ever_writable_ = true;
  writable_ = true;
  ChangeState();
}

void BaseChannel::SignalDtlsSetupFailure_w(bool rtcp) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  signaling_thread()->Invoke<void>(Bind(
      &BaseChannel::SignalDtlsSetupFailure_s, this, rtcp));
}

void BaseChannel::SignalDtlsSetupFailure_s(bool rtcp) {
  ASSERT(signaling_thread() == rtc::Thread::Current());
  SignalDtlsSetupFailure(this, rtcp);
}

bool BaseChannel::SetDtlsSrtpCiphers(TransportChannel *tc, bool rtcp) {
  std::vector<std::string> ciphers;
  // We always use the default SRTP ciphers for RTCP, but we may use different
  // ciphers for RTP depending on the media type.
  if (!rtcp) {
    GetSrtpCryptoSuiteNames(&ciphers);
  } else {
    GetDefaultSrtpCryptoSuiteNames(&ciphers);
  }
  return tc->SetSrtpCiphers(ciphers);
}

bool BaseChannel::ShouldSetupDtlsSrtp() const {
  return true;
}

// This function returns true if either DTLS-SRTP is not in use
// *or* DTLS-SRTP is successfully set up.
bool BaseChannel::SetupDtlsSrtp(bool rtcp_channel) {
  bool ret = false;

  TransportChannel* channel =
      rtcp_channel ? rtcp_transport_channel_ : transport_channel_;

  // No DTLS
  if (!channel->IsDtlsActive())
    return true;

  std::string selected_cipher;

  if (!channel->GetSrtpCryptoSuite(&selected_cipher)) {
    LOG(LS_ERROR) << "No DTLS-SRTP selected cipher";
    return false;
  }

  LOG(LS_INFO) << "Installing keys from DTLS-SRTP on "
               << content_name() << " "
               << PacketType(rtcp_channel);

  // OK, we're now doing DTLS (RFC 5764)
  std::vector<unsigned char> dtls_buffer(SRTP_MASTER_KEY_KEY_LEN * 2 +
                                         SRTP_MASTER_KEY_SALT_LEN * 2);

  // RFC 5705 exporter using the RFC 5764 parameters
  if (!channel->ExportKeyingMaterial(
          kDtlsSrtpExporterLabel,
          NULL, 0, false,
          &dtls_buffer[0], dtls_buffer.size())) {
    LOG(LS_WARNING) << "DTLS-SRTP key export failed";
    ASSERT(false);  // This should never happen
    return false;
  }

  // Sync up the keys with the DTLS-SRTP interface
  std::vector<unsigned char> client_write_key(SRTP_MASTER_KEY_KEY_LEN +
    SRTP_MASTER_KEY_SALT_LEN);
  std::vector<unsigned char> server_write_key(SRTP_MASTER_KEY_KEY_LEN +
    SRTP_MASTER_KEY_SALT_LEN);
  size_t offset = 0;
  memcpy(&client_write_key[0], &dtls_buffer[offset],
    SRTP_MASTER_KEY_KEY_LEN);
  offset += SRTP_MASTER_KEY_KEY_LEN;
  memcpy(&server_write_key[0], &dtls_buffer[offset],
    SRTP_MASTER_KEY_KEY_LEN);
  offset += SRTP_MASTER_KEY_KEY_LEN;
  memcpy(&client_write_key[SRTP_MASTER_KEY_KEY_LEN],
    &dtls_buffer[offset], SRTP_MASTER_KEY_SALT_LEN);
  offset += SRTP_MASTER_KEY_SALT_LEN;
  memcpy(&server_write_key[SRTP_MASTER_KEY_KEY_LEN],
    &dtls_buffer[offset], SRTP_MASTER_KEY_SALT_LEN);

  std::vector<unsigned char> *send_key, *recv_key;
  rtc::SSLRole role;
  if (!channel->GetSslRole(&role)) {
    LOG(LS_WARNING) << "GetSslRole failed";
    return false;
  }

  if (role == rtc::SSL_SERVER) {
    send_key = &server_write_key;
    recv_key = &client_write_key;
  } else {
    send_key = &client_write_key;
    recv_key = &server_write_key;
  }

  if (rtcp_channel) {
    ret = srtp_filter_.SetRtcpParams(
        selected_cipher,
        &(*send_key)[0],
        static_cast<int>(send_key->size()),
        selected_cipher,
        &(*recv_key)[0],
        static_cast<int>(recv_key->size()));
  } else {
    ret = srtp_filter_.SetRtpParams(
        selected_cipher,
        &(*send_key)[0],
        static_cast<int>(send_key->size()),
        selected_cipher,
        &(*recv_key)[0],
        static_cast<int>(recv_key->size()));
  }

  if (!ret)
    LOG(LS_WARNING) << "DTLS-SRTP key installation failed";
  else
    dtls_keyed_ = true;

  return ret;
}

void BaseChannel::ChannelNotWritable_w() {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  if (!writable_)
    return;

  LOG(LS_INFO) << "Channel not writable (" << content_name_ << ")";
  writable_ = false;
  ChangeState();
}

bool BaseChannel::SetRtpTransportParameters_w(
    const MediaContentDescription* content,
    ContentAction action,
    ContentSource src,
    std::string* error_desc) {
  if (action == CA_UPDATE) {
    // These parameters never get changed by a CA_UDPATE.
    return true;
  }

  // Cache secure_required_ for belt and suspenders check on SendPacket
  if (src == CS_LOCAL) {
    set_secure_required(content->crypto_required() != CT_NONE);
  }

  if (!SetSrtp_w(content->cryptos(), action, src, error_desc)) {
    return false;
  }

  if (!SetRtcpMux_w(content->rtcp_mux(), action, src, error_desc)) {
    return false;
  }

  return true;
}

// |dtls| will be set to true if DTLS is active for transport channel and
// crypto is empty.
bool BaseChannel::CheckSrtpConfig(const std::vector<CryptoParams>& cryptos,
                                  bool* dtls,
                                  std::string* error_desc) {
  *dtls = transport_channel_->IsDtlsActive();
  if (*dtls && !cryptos.empty()) {
    SafeSetError("Cryptos must be empty when DTLS is active.",
                 error_desc);
    return false;
  }
  return true;
}

bool BaseChannel::SetSrtp_w(const std::vector<CryptoParams>& cryptos,
                            ContentAction action,
                            ContentSource src,
                            std::string* error_desc) {
  if (action == CA_UPDATE) {
    // no crypto params.
    return true;
  }
  bool ret = false;
  bool dtls = false;
  ret = CheckSrtpConfig(cryptos, &dtls, error_desc);
  if (!ret) {
    return false;
  }
  switch (action) {
    case CA_OFFER:
      // If DTLS is already active on the channel, we could be renegotiating
      // here. We don't update the srtp filter.
      if (!dtls) {
        ret = srtp_filter_.SetOffer(cryptos, src);
      }
      break;
    case CA_PRANSWER:
      // If we're doing DTLS-SRTP, we don't want to update the filter
      // with an answer, because we already have SRTP parameters.
      if (!dtls) {
        ret = srtp_filter_.SetProvisionalAnswer(cryptos, src);
      }
      break;
    case CA_ANSWER:
      // If we're doing DTLS-SRTP, we don't want to update the filter
      // with an answer, because we already have SRTP parameters.
      if (!dtls) {
        ret = srtp_filter_.SetAnswer(cryptos, src);
      }
      break;
    default:
      break;
  }
  if (!ret) {
    SafeSetError("Failed to setup SRTP filter.", error_desc);
    return false;
  }
  return true;
}

void BaseChannel::ActivateRtcpMux() {
  worker_thread_->Invoke<void>(Bind(
      &BaseChannel::ActivateRtcpMux_w, this));
}

void BaseChannel::ActivateRtcpMux_w() {
  if (!rtcp_mux_filter_.IsActive()) {
    rtcp_mux_filter_.SetActive();
    set_rtcp_transport_channel(nullptr);
    rtcp_transport_enabled_ = false;
  }
}

bool BaseChannel::SetRtcpMux_w(bool enable, ContentAction action,
                               ContentSource src,
                               std::string* error_desc) {
  bool ret = false;
  switch (action) {
    case CA_OFFER:
      ret = rtcp_mux_filter_.SetOffer(enable, src);
      break;
    case CA_PRANSWER:
      ret = rtcp_mux_filter_.SetProvisionalAnswer(enable, src);
      break;
    case CA_ANSWER:
      ret = rtcp_mux_filter_.SetAnswer(enable, src);
      if (ret && rtcp_mux_filter_.IsActive()) {
        // We activated RTCP mux, close down the RTCP transport.
        LOG(LS_INFO) << "Enabling rtcp-mux for " << content_name()
                     << " by destroying RTCP transport channel for "
                     << transport_name();
        set_rtcp_transport_channel(nullptr);
        rtcp_transport_enabled_ = false;
      }
      break;
    case CA_UPDATE:
      // No RTCP mux info.
      ret = true;
      break;
    default:
      break;
  }
  if (!ret) {
    SafeSetError("Failed to setup RTCP mux filter.", error_desc);
    return false;
  }
  // |rtcp_mux_filter_| can be active if |action| is CA_PRANSWER or
  // CA_ANSWER, but we only want to tear down the RTCP transport channel if we
  // received a final answer.
  if (rtcp_mux_filter_.IsActive()) {
    // If the RTP transport is already writable, then so are we.
    if (transport_channel_->writable()) {
      ChannelWritable_w();
    }
  }

  return true;
}

bool BaseChannel::AddRecvStream_w(const StreamParams& sp) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  if (!media_channel()->AddRecvStream(sp))
    return false;

  return bundle_filter_.AddStream(sp);
}

bool BaseChannel::RemoveRecvStream_w(uint32_t ssrc) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  bundle_filter_.RemoveStream(ssrc);
  return media_channel()->RemoveRecvStream(ssrc);
}

bool BaseChannel::UpdateLocalStreams_w(const std::vector<StreamParams>& streams,
                                       ContentAction action,
                                       std::string* error_desc) {
  if (!VERIFY(action == CA_OFFER || action == CA_ANSWER ||
              action == CA_PRANSWER || action == CA_UPDATE))
    return false;

  // If this is an update, streams only contain streams that have changed.
  if (action == CA_UPDATE) {
    for (StreamParamsVec::const_iterator it = streams.begin();
         it != streams.end(); ++it) {
      const StreamParams* existing_stream =
          GetStreamByIds(local_streams_, it->groupid, it->id);
      if (!existing_stream && it->has_ssrcs()) {
        if (media_channel()->AddSendStream(*it)) {
          local_streams_.push_back(*it);
          LOG(LS_INFO) << "Add send stream ssrc: " << it->first_ssrc();
        } else {
          std::ostringstream desc;
          desc << "Failed to add send stream ssrc: " << it->first_ssrc();
          SafeSetError(desc.str(), error_desc);
          return false;
        }
      } else if (existing_stream && !it->has_ssrcs()) {
        if (!media_channel()->RemoveSendStream(existing_stream->first_ssrc())) {
          std::ostringstream desc;
          desc << "Failed to remove send stream with ssrc "
               << it->first_ssrc() << ".";
          SafeSetError(desc.str(), error_desc);
          return false;
        }
        RemoveStreamBySsrc(&local_streams_, existing_stream->first_ssrc());
      } else {
        LOG(LS_WARNING) << "Ignore unsupported stream update";
      }
    }
    return true;
  }
  // Else streams are all the streams we want to send.

  // Check for streams that have been removed.
  bool ret = true;
  for (StreamParamsVec::const_iterator it = local_streams_.begin();
       it != local_streams_.end(); ++it) {
    if (!GetStreamBySsrc(streams, it->first_ssrc())) {
      if (!media_channel()->RemoveSendStream(it->first_ssrc())) {
        std::ostringstream desc;
        desc << "Failed to remove send stream with ssrc "
             << it->first_ssrc() << ".";
        SafeSetError(desc.str(), error_desc);
        ret = false;
      }
    }
  }
  // Check for new streams.
  for (StreamParamsVec::const_iterator it = streams.begin();
       it != streams.end(); ++it) {
    if (!GetStreamBySsrc(local_streams_, it->first_ssrc())) {
      if (media_channel()->AddSendStream(*it)) {
        LOG(LS_INFO) << "Add send ssrc: " << it->ssrcs[0];
      } else {
        std::ostringstream desc;
        desc << "Failed to add send stream ssrc: " << it->first_ssrc();
        SafeSetError(desc.str(), error_desc);
        ret = false;
      }
    }
  }
  local_streams_ = streams;
  return ret;
}

bool BaseChannel::UpdateRemoteStreams_w(
    const std::vector<StreamParams>& streams,
    ContentAction action,
    std::string* error_desc) {
  if (!VERIFY(action == CA_OFFER || action == CA_ANSWER ||
              action == CA_PRANSWER || action == CA_UPDATE))
    return false;

  // If this is an update, streams only contain streams that have changed.
  if (action == CA_UPDATE) {
    for (StreamParamsVec::const_iterator it = streams.begin();
         it != streams.end(); ++it) {
      const StreamParams* existing_stream =
          GetStreamByIds(remote_streams_, it->groupid, it->id);
      if (!existing_stream && it->has_ssrcs()) {
        if (AddRecvStream_w(*it)) {
          remote_streams_.push_back(*it);
          LOG(LS_INFO) << "Add remote stream ssrc: " << it->first_ssrc();
        } else {
          std::ostringstream desc;
          desc << "Failed to add remote stream ssrc: " << it->first_ssrc();
          SafeSetError(desc.str(), error_desc);
          return false;
        }
      } else if (existing_stream && !it->has_ssrcs()) {
        if (!RemoveRecvStream_w(existing_stream->first_ssrc())) {
          std::ostringstream desc;
          desc << "Failed to remove remote stream with ssrc "
               << it->first_ssrc() << ".";
          SafeSetError(desc.str(), error_desc);
          return false;
        }
        RemoveStreamBySsrc(&remote_streams_, existing_stream->first_ssrc());
      } else {
        LOG(LS_WARNING) << "Ignore unsupported stream update."
                        << " Stream exists? " << (existing_stream != nullptr)
                        << " new stream = " << it->ToString();
      }
    }
    return true;
  }
  // Else streams are all the streams we want to receive.

  // Check for streams that have been removed.
  bool ret = true;
  for (StreamParamsVec::const_iterator it = remote_streams_.begin();
       it != remote_streams_.end(); ++it) {
    if (!GetStreamBySsrc(streams, it->first_ssrc())) {
      if (!RemoveRecvStream_w(it->first_ssrc())) {
        std::ostringstream desc;
        desc << "Failed to remove remote stream with ssrc "
             << it->first_ssrc() << ".";
        SafeSetError(desc.str(), error_desc);
        ret = false;
      }
    }
  }
  // Check for new streams.
  for (StreamParamsVec::const_iterator it = streams.begin();
      it != streams.end(); ++it) {
    if (!GetStreamBySsrc(remote_streams_, it->first_ssrc())) {
      if (AddRecvStream_w(*it)) {
        LOG(LS_INFO) << "Add remote ssrc: " << it->ssrcs[0];
      } else {
        std::ostringstream desc;
        desc << "Failed to add remote stream ssrc: " << it->first_ssrc();
        SafeSetError(desc.str(), error_desc);
        ret = false;
      }
    }
  }
  remote_streams_ = streams;
  return ret;
}

void BaseChannel::MaybeCacheRtpAbsSendTimeHeaderExtension(
    const std::vector<RtpHeaderExtension>& extensions) {
  const RtpHeaderExtension* send_time_extension =
      FindHeaderExtension(extensions, kRtpAbsoluteSenderTimeHeaderExtension);
  rtp_abs_sendtime_extn_id_ =
      send_time_extension ? send_time_extension->id : -1;
}

void BaseChannel::OnMessage(rtc::Message *pmsg) {
  switch (pmsg->message_id) {
    case MSG_RTPPACKET:
    case MSG_RTCPPACKET: {
      PacketMessageData* data = static_cast<PacketMessageData*>(pmsg->pdata);
      SendPacket(pmsg->message_id == MSG_RTCPPACKET, &data->packet, data->dscp);
      delete data;  // because it is Posted
      break;
    }
    case MSG_FIRSTPACKETRECEIVED: {
      SignalFirstPacketReceived(this);
      break;
    }
  }
}

void BaseChannel::FlushRtcpMessages() {
  // Flush all remaining RTCP messages. This should only be called in
  // destructor.
  ASSERT(rtc::Thread::Current() == worker_thread_);
  rtc::MessageList rtcp_messages;
  worker_thread_->Clear(this, MSG_RTCPPACKET, &rtcp_messages);
  for (rtc::MessageList::iterator it = rtcp_messages.begin();
       it != rtcp_messages.end(); ++it) {
    worker_thread_->Send(this, MSG_RTCPPACKET, it->pdata);
  }
}

VoiceChannel::VoiceChannel(rtc::Thread* thread,
                           MediaEngineInterface* media_engine,
                           VoiceMediaChannel* media_channel,
                           TransportController* transport_controller,
                           const std::string& content_name,
                           bool rtcp)
    : BaseChannel(thread,
                  media_channel,
                  transport_controller,
                  content_name,
                  rtcp),
      media_engine_(media_engine),
      received_media_(false) {}

VoiceChannel::~VoiceChannel() {
  StopAudioMonitor();
  StopMediaMonitor();
  // this can't be done in the base class, since it calls a virtual
  DisableMedia_w();
  Deinit();
}

bool VoiceChannel::Init() {
  if (!BaseChannel::Init()) {
    return false;
  }
  return true;
}

bool VoiceChannel::SetAudioSend(uint32_t ssrc,
                                bool enable,
                                const AudioOptions* options,
                                AudioRenderer* renderer) {
  return InvokeOnWorker(Bind(&VoiceMediaChannel::SetAudioSend, media_channel(),
                             ssrc, enable, options, renderer));
}

// TODO(juberti): Handle early media the right way. We should get an explicit
// ringing message telling us to start playing local ringback, which we cancel
// if any early media actually arrives. For now, we do the opposite, which is
// to wait 1 second for early media, and start playing local ringback if none
// arrives.
void VoiceChannel::SetEarlyMedia(bool enable) {
  if (enable) {
    // Start the early media timeout
    worker_thread()->PostDelayed(kEarlyMediaTimeout, this,
                                MSG_EARLYMEDIATIMEOUT);
  } else {
    // Stop the timeout if currently going.
    worker_thread()->Clear(this, MSG_EARLYMEDIATIMEOUT);
  }
}

bool VoiceChannel::PressDTMF(int digit, bool playout) {
  int flags = DF_SEND;
  if (playout) {
    flags |= DF_PLAY;
  }
  int duration_ms = 160;
  return InsertDtmf(0, digit, duration_ms, flags);
}

bool VoiceChannel::CanInsertDtmf() {
  return InvokeOnWorker(Bind(&VoiceMediaChannel::CanInsertDtmf,
                             media_channel()));
}

bool VoiceChannel::InsertDtmf(uint32_t ssrc,
                              int event_code,
                              int duration,
                              int flags) {
  return InvokeOnWorker(Bind(&VoiceChannel::InsertDtmf_w, this,
                             ssrc, event_code, duration, flags));
}

bool VoiceChannel::SetOutputVolume(uint32_t ssrc, double volume) {
  return InvokeOnWorker(Bind(&VoiceMediaChannel::SetOutputVolume,
                             media_channel(), ssrc, volume));
}

bool VoiceChannel::GetStats(VoiceMediaInfo* stats) {
  return InvokeOnWorker(Bind(&VoiceMediaChannel::GetStats,
                             media_channel(), stats));
}

void VoiceChannel::StartMediaMonitor(int cms) {
  media_monitor_.reset(new VoiceMediaMonitor(media_channel(), worker_thread(),
      rtc::Thread::Current()));
  media_monitor_->SignalUpdate.connect(
      this, &VoiceChannel::OnMediaMonitorUpdate);
  media_monitor_->Start(cms);
}

void VoiceChannel::StopMediaMonitor() {
  if (media_monitor_) {
    media_monitor_->Stop();
    media_monitor_->SignalUpdate.disconnect(this);
    media_monitor_.reset();
  }
}

void VoiceChannel::StartAudioMonitor(int cms) {
  audio_monitor_.reset(new AudioMonitor(this, rtc::Thread::Current()));
  audio_monitor_
    ->SignalUpdate.connect(this, &VoiceChannel::OnAudioMonitorUpdate);
  audio_monitor_->Start(cms);
}

void VoiceChannel::StopAudioMonitor() {
  if (audio_monitor_) {
    audio_monitor_->Stop();
    audio_monitor_.reset();
  }
}

bool VoiceChannel::IsAudioMonitorRunning() const {
  return (audio_monitor_.get() != NULL);
}

int VoiceChannel::GetInputLevel_w() {
  return media_engine_->GetInputLevel();
}

int VoiceChannel::GetOutputLevel_w() {
  return media_channel()->GetOutputLevel();
}

void VoiceChannel::GetActiveStreams_w(AudioInfo::StreamList* actives) {
  media_channel()->GetActiveStreams(actives);
}

void VoiceChannel::OnChannelRead(TransportChannel* channel,
                                 const char* data, size_t len,
                                 const rtc::PacketTime& packet_time,
                                int flags) {
  BaseChannel::OnChannelRead(channel, data, len, packet_time, flags);

  // Set a flag when we've received an RTP packet. If we're waiting for early
  // media, this will disable the timeout.
  if (!received_media_ && !PacketIsRtcp(channel, data, len)) {
    received_media_ = true;
  }
}

void VoiceChannel::ChangeState() {
  // Render incoming data if we're the active call, and we have the local
  // content. We receive data on the default channel and multiplexed streams.
  bool recv = IsReadyToReceive();
  media_channel()->SetPlayout(recv);

  // Send outgoing data if we're the active call, we have the remote content,
  // and we have had some form of connectivity.
  bool send = IsReadyToSend();
  SendFlags send_flag = send ? SEND_MICROPHONE : SEND_NOTHING;
  if (!media_channel()->SetSend(send_flag)) {
    LOG(LS_ERROR) << "Failed to SetSend " << send_flag << " on voice channel";
  }

  LOG(LS_INFO) << "Changing voice state, recv=" << recv << " send=" << send;
}

const ContentInfo* VoiceChannel::GetFirstContent(
    const SessionDescription* sdesc) {
  return GetFirstAudioContent(sdesc);
}

bool VoiceChannel::SetLocalContent_w(const MediaContentDescription* content,
                                     ContentAction action,
                                     std::string* error_desc) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  LOG(LS_INFO) << "Setting local voice description";

  const AudioContentDescription* audio =
      static_cast<const AudioContentDescription*>(content);
  ASSERT(audio != NULL);
  if (!audio) {
    SafeSetError("Can't find audio content in local description.", error_desc);
    return false;
  }

  if (!SetRtpTransportParameters_w(content, action, CS_LOCAL, error_desc)) {
    return false;
  }

  AudioRecvParameters recv_params = last_recv_params_;
  RtpParametersFromMediaDescription(audio, &recv_params);
  if (!media_channel()->SetRecvParameters(recv_params)) {
    SafeSetError("Failed to set local audio description recv parameters.",
                 error_desc);
    return false;
  }
  for (const AudioCodec& codec : audio->codecs()) {
    bundle_filter()->AddPayloadType(codec.id);
  }
  last_recv_params_ = recv_params;

  // TODO(pthatcher): Move local streams into AudioSendParameters, and
  // only give it to the media channel once we have a remote
  // description too (without a remote description, we won't be able
  // to send them anyway).
  if (!UpdateLocalStreams_w(audio->streams(), action, error_desc)) {
    SafeSetError("Failed to set local audio description streams.", error_desc);
    return false;
  }

  set_local_content_direction(content->direction());
  ChangeState();
  return true;
}

bool VoiceChannel::SetRemoteContent_w(const MediaContentDescription* content,
                                      ContentAction action,
                                      std::string* error_desc) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  LOG(LS_INFO) << "Setting remote voice description";

  const AudioContentDescription* audio =
      static_cast<const AudioContentDescription*>(content);
  ASSERT(audio != NULL);
  if (!audio) {
    SafeSetError("Can't find audio content in remote description.", error_desc);
    return false;
  }

  if (!SetRtpTransportParameters_w(content, action, CS_REMOTE, error_desc)) {
    return false;
  }

  AudioSendParameters send_params = last_send_params_;
  RtpSendParametersFromMediaDescription(audio, &send_params);
  if (audio->conference_mode()) {
    send_params.options.conference_mode.Set(true);
  }
  if (audio->agc_minus_10db()) {
    send_params.options.adjust_agc_delta.Set(kAgcMinus10db);
  }
  if (!media_channel()->SetSendParameters(send_params)) {
    SafeSetError("Failed to set remote audio description send parameters.",
                 error_desc);
    return false;
  }
  last_send_params_ = send_params;

  // TODO(pthatcher): Move remote streams into AudioRecvParameters,
  // and only give it to the media channel once we have a local
  // description too (without a local description, we won't be able to
  // recv them anyway).
  if (!UpdateRemoteStreams_w(audio->streams(), action, error_desc)) {
    SafeSetError("Failed to set remote audio description streams.", error_desc);
    return false;
  }

  if (audio->rtp_header_extensions_set()) {
    MaybeCacheRtpAbsSendTimeHeaderExtension(audio->rtp_header_extensions());
  }

  set_remote_content_direction(content->direction());
  ChangeState();
  return true;
}

void VoiceChannel::HandleEarlyMediaTimeout() {
  // This occurs on the main thread, not the worker thread.
  if (!received_media_) {
    LOG(LS_INFO) << "No early media received before timeout";
    SignalEarlyMediaTimeout(this);
  }
}

bool VoiceChannel::InsertDtmf_w(uint32_t ssrc,
                                int event,
                                int duration,
                                int flags) {
  if (!enabled()) {
    return false;
  }

  return media_channel()->InsertDtmf(ssrc, event, duration, flags);
}

void VoiceChannel::OnMessage(rtc::Message *pmsg) {
  switch (pmsg->message_id) {
    case MSG_EARLYMEDIATIMEOUT:
      HandleEarlyMediaTimeout();
      break;
    case MSG_CHANNEL_ERROR: {
      VoiceChannelErrorMessageData* data =
          static_cast<VoiceChannelErrorMessageData*>(pmsg->pdata);
      delete data;
      break;
    }
    default:
      BaseChannel::OnMessage(pmsg);
      break;
  }
}

void VoiceChannel::OnConnectionMonitorUpdate(
    ConnectionMonitor* monitor, const std::vector<ConnectionInfo>& infos) {
  SignalConnectionMonitor(this, infos);
}

void VoiceChannel::OnMediaMonitorUpdate(
    VoiceMediaChannel* media_channel, const VoiceMediaInfo& info) {
  ASSERT(media_channel == this->media_channel());
  SignalMediaMonitor(this, info);
}

void VoiceChannel::OnAudioMonitorUpdate(AudioMonitor* monitor,
                                        const AudioInfo& info) {
  SignalAudioMonitor(this, info);
}

void VoiceChannel::GetSrtpCryptoSuiteNames(
    std::vector<std::string>* ciphers) const {
  GetSupportedAudioCryptoSuites(ciphers);
}

VideoChannel::VideoChannel(rtc::Thread* thread,
                           VideoMediaChannel* media_channel,
                           TransportController* transport_controller,
                           const std::string& content_name,
                           bool rtcp)
    : BaseChannel(thread,
                  media_channel,
                  transport_controller,
                  content_name,
                  rtcp),
      renderer_(NULL),
      previous_we_(rtc::WE_CLOSE) {}

bool VideoChannel::Init() {
  if (!BaseChannel::Init()) {
    return false;
  }
  return true;
}

VideoChannel::~VideoChannel() {
  std::vector<uint32_t> screencast_ssrcs;
  ScreencastMap::iterator iter;
  while (!screencast_capturers_.empty()) {
    if (!RemoveScreencast(screencast_capturers_.begin()->first)) {
      LOG(LS_ERROR) << "Unable to delete screencast with ssrc "
                    << screencast_capturers_.begin()->first;
      ASSERT(false);
      break;
    }
  }

  StopMediaMonitor();
  // this can't be done in the base class, since it calls a virtual
  DisableMedia_w();

  Deinit();
}

bool VideoChannel::SetRenderer(uint32_t ssrc, VideoRenderer* renderer) {
  worker_thread()->Invoke<void>(Bind(
      &VideoMediaChannel::SetRenderer, media_channel(), ssrc, renderer));
  return true;
}

bool VideoChannel::ApplyViewRequest(const ViewRequest& request) {
  return InvokeOnWorker(Bind(&VideoChannel::ApplyViewRequest_w, this, request));
}

bool VideoChannel::AddScreencast(uint32_t ssrc, VideoCapturer* capturer) {
  return worker_thread()->Invoke<bool>(Bind(
      &VideoChannel::AddScreencast_w, this, ssrc, capturer));
}

bool VideoChannel::SetCapturer(uint32_t ssrc, VideoCapturer* capturer) {
  return InvokeOnWorker(Bind(&VideoMediaChannel::SetCapturer,
                             media_channel(), ssrc, capturer));
}

bool VideoChannel::RemoveScreencast(uint32_t ssrc) {
  return InvokeOnWorker(Bind(&VideoChannel::RemoveScreencast_w, this, ssrc));
}

bool VideoChannel::IsScreencasting() {
  return InvokeOnWorker(Bind(&VideoChannel::IsScreencasting_w, this));
}

int VideoChannel::GetScreencastFps(uint32_t ssrc) {
  ScreencastDetailsData data(ssrc);
  worker_thread()->Invoke<void>(Bind(
      &VideoChannel::GetScreencastDetails_w, this, &data));
  return data.fps;
}

int VideoChannel::GetScreencastMaxPixels(uint32_t ssrc) {
  ScreencastDetailsData data(ssrc);
  worker_thread()->Invoke<void>(Bind(
      &VideoChannel::GetScreencastDetails_w, this, &data));
  return data.screencast_max_pixels;
}

bool VideoChannel::SendIntraFrame() {
  worker_thread()->Invoke<void>(Bind(
      &VideoMediaChannel::SendIntraFrame, media_channel()));
  return true;
}

bool VideoChannel::RequestIntraFrame() {
  worker_thread()->Invoke<void>(Bind(
      &VideoMediaChannel::RequestIntraFrame, media_channel()));
  return true;
}

bool VideoChannel::SetVideoSend(uint32_t ssrc,
                                bool mute,
                                const VideoOptions* options) {
  return InvokeOnWorker(Bind(&VideoMediaChannel::SetVideoSend, media_channel(),
                             ssrc, mute, options));
}

void VideoChannel::ChangeState() {
  // Send outgoing data if we're the active call, we have the remote content,
  // and we have had some form of connectivity.
  bool send = IsReadyToSend();
  if (!media_channel()->SetSend(send)) {
    LOG(LS_ERROR) << "Failed to SetSend on video channel";
    // TODO(gangji): Report error back to server.
  }

  LOG(LS_INFO) << "Changing video state, send=" << send;
}

bool VideoChannel::GetStats(VideoMediaInfo* stats) {
  return InvokeOnWorker(
      Bind(&VideoMediaChannel::GetStats, media_channel(), stats));
}

void VideoChannel::StartMediaMonitor(int cms) {
  media_monitor_.reset(new VideoMediaMonitor(media_channel(), worker_thread(),
      rtc::Thread::Current()));
  media_monitor_->SignalUpdate.connect(
      this, &VideoChannel::OnMediaMonitorUpdate);
  media_monitor_->Start(cms);
}

void VideoChannel::StopMediaMonitor() {
  if (media_monitor_) {
    media_monitor_->Stop();
    media_monitor_.reset();
  }
}

const ContentInfo* VideoChannel::GetFirstContent(
    const SessionDescription* sdesc) {
  return GetFirstVideoContent(sdesc);
}

bool VideoChannel::SetLocalContent_w(const MediaContentDescription* content,
                                     ContentAction action,
                                     std::string* error_desc) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  LOG(LS_INFO) << "Setting local video description";

  const VideoContentDescription* video =
      static_cast<const VideoContentDescription*>(content);
  ASSERT(video != NULL);
  if (!video) {
    SafeSetError("Can't find video content in local description.", error_desc);
    return false;
  }

  if (!SetRtpTransportParameters_w(content, action, CS_LOCAL, error_desc)) {
    return false;
  }

  VideoRecvParameters recv_params = last_recv_params_;
  RtpParametersFromMediaDescription(video, &recv_params);
  if (!media_channel()->SetRecvParameters(recv_params)) {
    SafeSetError("Failed to set local video description recv parameters.",
                 error_desc);
    return false;
  }
  for (const VideoCodec& codec : video->codecs()) {
    bundle_filter()->AddPayloadType(codec.id);
  }
  last_recv_params_ = recv_params;

  // TODO(pthatcher): Move local streams into VideoSendParameters, and
  // only give it to the media channel once we have a remote
  // description too (without a remote description, we won't be able
  // to send them anyway).
  if (!UpdateLocalStreams_w(video->streams(), action, error_desc)) {
    SafeSetError("Failed to set local video description streams.", error_desc);
    return false;
  }

  set_local_content_direction(content->direction());
  ChangeState();
  return true;
}

bool VideoChannel::SetRemoteContent_w(const MediaContentDescription* content,
                                      ContentAction action,
                                      std::string* error_desc) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  LOG(LS_INFO) << "Setting remote video description";

  const VideoContentDescription* video =
      static_cast<const VideoContentDescription*>(content);
  ASSERT(video != NULL);
  if (!video) {
    SafeSetError("Can't find video content in remote description.", error_desc);
    return false;
  }


  if (!SetRtpTransportParameters_w(content, action, CS_REMOTE, error_desc)) {
    return false;
  }

  VideoSendParameters send_params = last_send_params_;
  RtpSendParametersFromMediaDescription(video, &send_params);
  if (video->conference_mode()) {
    send_params.options.conference_mode.Set(true);
  }
  if (!media_channel()->SetSendParameters(send_params)) {
    SafeSetError("Failed to set remote video description send parameters.",
                 error_desc);
    return false;
  }
  last_send_params_ = send_params;

  // TODO(pthatcher): Move remote streams into VideoRecvParameters,
  // and only give it to the media channel once we have a local
  // description too (without a local description, we won't be able to
  // recv them anyway).
  if (!UpdateRemoteStreams_w(video->streams(), action, error_desc)) {
    SafeSetError("Failed to set remote video description streams.", error_desc);
    return false;
  }

  if (video->rtp_header_extensions_set()) {
    MaybeCacheRtpAbsSendTimeHeaderExtension(video->rtp_header_extensions());
  }

  set_remote_content_direction(content->direction());
  ChangeState();
  return true;
}

bool VideoChannel::ApplyViewRequest_w(const ViewRequest& request) {
  bool ret = true;
  // Set the send format for each of the local streams. If the view request
  // does not contain a local stream, set its send format to 0x0, which will
  // drop all frames.
  for (std::vector<StreamParams>::const_iterator it = local_streams().begin();
      it != local_streams().end(); ++it) {
    VideoFormat format(0, 0, 0, cricket::FOURCC_I420);
    StaticVideoViews::const_iterator view;
    for (view = request.static_video_views.begin();
         view != request.static_video_views.end(); ++view) {
      if (view->selector.Matches(*it)) {
        format.width = view->width;
        format.height = view->height;
        format.interval = cricket::VideoFormat::FpsToInterval(view->framerate);
        break;
      }
    }

    ret &= media_channel()->SetSendStreamFormat(it->first_ssrc(), format);
  }

  // Check if the view request has invalid streams.
  for (StaticVideoViews::const_iterator it = request.static_video_views.begin();
      it != request.static_video_views.end(); ++it) {
    if (!GetStream(local_streams(), it->selector)) {
      LOG(LS_WARNING) << "View request for ("
                      << it->selector.ssrc << ", '"
                      << it->selector.groupid << "', '"
                      << it->selector.streamid << "'"
                      << ") is not in the local streams.";
    }
  }

  return ret;
}

bool VideoChannel::AddScreencast_w(uint32_t ssrc, VideoCapturer* capturer) {
  if (screencast_capturers_.find(ssrc) != screencast_capturers_.end()) {
    return false;
  }
  capturer->SignalStateChange.connect(this, &VideoChannel::OnStateChange);
  screencast_capturers_[ssrc] = capturer;
  return true;
}

bool VideoChannel::RemoveScreencast_w(uint32_t ssrc) {
  ScreencastMap::iterator iter = screencast_capturers_.find(ssrc);
  if (iter  == screencast_capturers_.end()) {
    return false;
  }
  // Clean up VideoCapturer.
  delete iter->second;
  screencast_capturers_.erase(iter);
  return true;
}

bool VideoChannel::IsScreencasting_w() const {
  return !screencast_capturers_.empty();
}

void VideoChannel::GetScreencastDetails_w(
    ScreencastDetailsData* data) const {
  ScreencastMap::const_iterator iter = screencast_capturers_.find(data->ssrc);
  if (iter == screencast_capturers_.end()) {
    return;
  }
  VideoCapturer* capturer = iter->second;
  const VideoFormat* video_format = capturer->GetCaptureFormat();
  data->fps = VideoFormat::IntervalToFps(video_format->interval);
  data->screencast_max_pixels = capturer->screencast_max_pixels();
}

void VideoChannel::OnScreencastWindowEvent_s(uint32_t ssrc,
                                             rtc::WindowEvent we) {
  ASSERT(signaling_thread() == rtc::Thread::Current());
  SignalScreencastWindowEvent(ssrc, we);
}

void VideoChannel::OnMessage(rtc::Message *pmsg) {
  switch (pmsg->message_id) {
    case MSG_SCREENCASTWINDOWEVENT: {
      const ScreencastEventMessageData* data =
          static_cast<ScreencastEventMessageData*>(pmsg->pdata);
      OnScreencastWindowEvent_s(data->ssrc, data->event);
      delete data;
      break;
    }
    case MSG_CHANNEL_ERROR: {
      const VideoChannelErrorMessageData* data =
          static_cast<VideoChannelErrorMessageData*>(pmsg->pdata);
      delete data;
      break;
    }
    default:
      BaseChannel::OnMessage(pmsg);
      break;
  }
}

void VideoChannel::OnConnectionMonitorUpdate(
    ConnectionMonitor* monitor, const std::vector<ConnectionInfo> &infos) {
  SignalConnectionMonitor(this, infos);
}

// TODO(pthatcher): Look into removing duplicate code between
// audio, video, and data, perhaps by using templates.
void VideoChannel::OnMediaMonitorUpdate(
    VideoMediaChannel* media_channel, const VideoMediaInfo &info) {
  ASSERT(media_channel == this->media_channel());
  SignalMediaMonitor(this, info);
}

void VideoChannel::OnScreencastWindowEvent(uint32_t ssrc,
                                           rtc::WindowEvent event) {
  ScreencastEventMessageData* pdata =
      new ScreencastEventMessageData(ssrc, event);
  signaling_thread()->Post(this, MSG_SCREENCASTWINDOWEVENT, pdata);
}

void VideoChannel::OnStateChange(VideoCapturer* capturer, CaptureState ev) {
  // Map capturer events to window events. In the future we may want to simply
  // pass these events up directly.
  rtc::WindowEvent we;
  if (ev == CS_STOPPED) {
    we = rtc::WE_CLOSE;
  } else if (ev == CS_PAUSED) {
    we = rtc::WE_MINIMIZE;
  } else if (ev == CS_RUNNING && previous_we_ == rtc::WE_MINIMIZE) {
    we = rtc::WE_RESTORE;
  } else {
    return;
  }
  previous_we_ = we;

  uint32_t ssrc = 0;
  if (!GetLocalSsrc(capturer, &ssrc)) {
    return;
  }

  OnScreencastWindowEvent(ssrc, we);
}

bool VideoChannel::GetLocalSsrc(const VideoCapturer* capturer, uint32_t* ssrc) {
  *ssrc = 0;
  for (ScreencastMap::iterator iter = screencast_capturers_.begin();
       iter != screencast_capturers_.end(); ++iter) {
    if (iter->second == capturer) {
      *ssrc = iter->first;
      return true;
    }
  }
  return false;
}

void VideoChannel::GetSrtpCryptoSuiteNames(
    std::vector<std::string>* ciphers) const {
  GetSupportedVideoCryptoSuites(ciphers);
}

DataChannel::DataChannel(rtc::Thread* thread,
                         DataMediaChannel* media_channel,
                         TransportController* transport_controller,
                         const std::string& content_name,
                         bool rtcp)
    : BaseChannel(thread,
                  media_channel,
                  transport_controller,
                  content_name,
                  rtcp),
      data_channel_type_(cricket::DCT_NONE),
      ready_to_send_data_(false) {}

DataChannel::~DataChannel() {
  StopMediaMonitor();
  // this can't be done in the base class, since it calls a virtual
  DisableMedia_w();

  Deinit();
}

bool DataChannel::Init() {
  if (!BaseChannel::Init()) {
    return false;
  }
  media_channel()->SignalDataReceived.connect(
      this, &DataChannel::OnDataReceived);
  media_channel()->SignalReadyToSend.connect(
      this, &DataChannel::OnDataChannelReadyToSend);
  media_channel()->SignalStreamClosedRemotely.connect(
      this, &DataChannel::OnStreamClosedRemotely);
  return true;
}

bool DataChannel::SendData(const SendDataParams& params,
                           const rtc::Buffer& payload,
                           SendDataResult* result) {
  return InvokeOnWorker(Bind(&DataMediaChannel::SendData,
                             media_channel(), params, payload, result));
}

const ContentInfo* DataChannel::GetFirstContent(
    const SessionDescription* sdesc) {
  return GetFirstDataContent(sdesc);
}

bool DataChannel::WantsPacket(bool rtcp, rtc::Buffer* packet) {
  if (data_channel_type_ == DCT_SCTP) {
    // TODO(pthatcher): Do this in a more robust way by checking for
    // SCTP or DTLS.
    return !IsRtpPacket(packet->data(), packet->size());
  } else if (data_channel_type_ == DCT_RTP) {
    return BaseChannel::WantsPacket(rtcp, packet);
  }
  return false;
}

bool DataChannel::SetDataChannelType(DataChannelType new_data_channel_type,
                                     std::string* error_desc) {
  // It hasn't been set before, so set it now.
  if (data_channel_type_ == DCT_NONE) {
    data_channel_type_ = new_data_channel_type;
    return true;
  }

  // It's been set before, but doesn't match.  That's bad.
  if (data_channel_type_ != new_data_channel_type) {
    std::ostringstream desc;
    desc << "Data channel type mismatch."
         << " Expected " << data_channel_type_
         << " Got " << new_data_channel_type;
    SafeSetError(desc.str(), error_desc);
    return false;
  }

  // It's hasn't changed.  Nothing to do.
  return true;
}

bool DataChannel::SetDataChannelTypeFromContent(
    const DataContentDescription* content,
    std::string* error_desc) {
  bool is_sctp = ((content->protocol() == kMediaProtocolSctp) ||
                  (content->protocol() == kMediaProtocolDtlsSctp));
  DataChannelType data_channel_type = is_sctp ? DCT_SCTP : DCT_RTP;
  return SetDataChannelType(data_channel_type, error_desc);
}

bool DataChannel::SetLocalContent_w(const MediaContentDescription* content,
                                    ContentAction action,
                                    std::string* error_desc) {
  ASSERT(worker_thread() == rtc::Thread::Current());
  LOG(LS_INFO) << "Setting local data description";

  const DataContentDescription* data =
      static_cast<const DataContentDescription*>(content);
  ASSERT(data != NULL);
  if (!data) {
    SafeSetError("Can't find data content in local description.", error_desc);
    return false;
  }

  if (!SetDataChannelTypeFromContent(data, error_desc)) {
    return false;
  }

  if (data_channel_type_ == DCT_RTP) {
    if (!SetRtpTransportParameters_w(content, action, CS_LOCAL, error_desc)) {
      return false;
    }
  }

  // FYI: We send the SCTP port number (not to be confused with the
  // underlying UDP port number) as a codec parameter.  So even SCTP
  // data channels need codecs.
  DataRecvParameters recv_params = last_recv_params_;
  RtpParametersFromMediaDescription(data, &recv_params);
  if (!media_channel()->SetRecvParameters(recv_params)) {
    SafeSetError("Failed to set remote data description recv parameters.",
                 error_desc);
    return false;
  }
  if (data_channel_type_ == DCT_RTP) {
    for (const DataCodec& codec : data->codecs()) {
      bundle_filter()->AddPayloadType(codec.id);
    }
  }
  last_recv_params_ = recv_params;

  // TODO(pthatcher): Move local streams into DataSendParameters, and
  // only give it to the media channel once we have a remote
  // description too (without a remote description, we won't be able
  // to send them anyway).
  if (!UpdateLocalStreams_w(data->streams(), action, error_desc)) {
    SafeSetError("Failed to set local data description streams.", error_desc);
    return false;
  }

  set_local_content_direction(content->direction());
  ChangeState();
  return true;
}

bool DataChannel::SetRemoteContent_w(const MediaContentDescription* content,
                                     ContentAction action,
                                     std::string* error_desc) {
  ASSERT(worker_thread() == rtc::Thread::Current());

  const DataContentDescription* data =
      static_cast<const DataContentDescription*>(content);
  ASSERT(data != NULL);
  if (!data) {
    SafeSetError("Can't find data content in remote description.", error_desc);
    return false;
  }

  // If the remote data doesn't have codecs and isn't an update, it
  // must be empty, so ignore it.
  if (!data->has_codecs() && action != CA_UPDATE) {
    return true;
  }

  if (!SetDataChannelTypeFromContent(data, error_desc)) {
    return false;
  }

  LOG(LS_INFO) << "Setting remote data description";
  if (data_channel_type_ == DCT_RTP &&
      !SetRtpTransportParameters_w(content, action, CS_REMOTE, error_desc)) {
    return false;
  }


  DataSendParameters send_params = last_send_params_;
  RtpSendParametersFromMediaDescription<DataCodec>(data, &send_params);
  if (!media_channel()->SetSendParameters(send_params)) {
    SafeSetError("Failed to set remote data description send parameters.",
                 error_desc);
    return false;
  }
  last_send_params_ = send_params;

  // TODO(pthatcher): Move remote streams into DataRecvParameters,
  // and only give it to the media channel once we have a local
  // description too (without a local description, we won't be able to
  // recv them anyway).
  if (!UpdateRemoteStreams_w(data->streams(), action, error_desc)) {
    SafeSetError("Failed to set remote data description streams.",
                 error_desc);
    return false;
  }

  set_remote_content_direction(content->direction());
  ChangeState();
  return true;
}

void DataChannel::ChangeState() {
  // Render incoming data if we're the active call, and we have the local
  // content. We receive data on the default channel and multiplexed streams.
  bool recv = IsReadyToReceive();
  if (!media_channel()->SetReceive(recv)) {
    LOG(LS_ERROR) << "Failed to SetReceive on data channel";
  }

  // Send outgoing data if we're the active call, we have the remote content,
  // and we have had some form of connectivity.
  bool send = IsReadyToSend();
  if (!media_channel()->SetSend(send)) {
    LOG(LS_ERROR) << "Failed to SetSend on data channel";
  }

  // Trigger SignalReadyToSendData asynchronously.
  OnDataChannelReadyToSend(send);

  LOG(LS_INFO) << "Changing data state, recv=" << recv << " send=" << send;
}

void DataChannel::OnMessage(rtc::Message *pmsg) {
  switch (pmsg->message_id) {
    case MSG_READYTOSENDDATA: {
      DataChannelReadyToSendMessageData* data =
          static_cast<DataChannelReadyToSendMessageData*>(pmsg->pdata);
      ready_to_send_data_ = data->data();
      SignalReadyToSendData(ready_to_send_data_);
      delete data;
      break;
    }
    case MSG_DATARECEIVED: {
      DataReceivedMessageData* data =
          static_cast<DataReceivedMessageData*>(pmsg->pdata);
      SignalDataReceived(this, data->params, data->payload);
      delete data;
      break;
    }
    case MSG_CHANNEL_ERROR: {
      const DataChannelErrorMessageData* data =
          static_cast<DataChannelErrorMessageData*>(pmsg->pdata);
      delete data;
      break;
    }
    case MSG_STREAMCLOSEDREMOTELY: {
      rtc::TypedMessageData<uint32_t>* data =
          static_cast<rtc::TypedMessageData<uint32_t>*>(pmsg->pdata);
      SignalStreamClosedRemotely(data->data());
      delete data;
      break;
    }
    default:
      BaseChannel::OnMessage(pmsg);
      break;
  }
}

void DataChannel::OnConnectionMonitorUpdate(
    ConnectionMonitor* monitor, const std::vector<ConnectionInfo>& infos) {
  SignalConnectionMonitor(this, infos);
}

void DataChannel::StartMediaMonitor(int cms) {
  media_monitor_.reset(new DataMediaMonitor(media_channel(), worker_thread(),
      rtc::Thread::Current()));
  media_monitor_->SignalUpdate.connect(
      this, &DataChannel::OnMediaMonitorUpdate);
  media_monitor_->Start(cms);
}

void DataChannel::StopMediaMonitor() {
  if (media_monitor_) {
    media_monitor_->Stop();
    media_monitor_->SignalUpdate.disconnect(this);
    media_monitor_.reset();
  }
}

void DataChannel::OnMediaMonitorUpdate(
    DataMediaChannel* media_channel, const DataMediaInfo& info) {
  ASSERT(media_channel == this->media_channel());
  SignalMediaMonitor(this, info);
}

void DataChannel::OnDataReceived(
    const ReceiveDataParams& params, const char* data, size_t len) {
  DataReceivedMessageData* msg = new DataReceivedMessageData(
      params, data, len);
  signaling_thread()->Post(this, MSG_DATARECEIVED, msg);
}

void DataChannel::OnDataChannelError(uint32_t ssrc,
                                     DataMediaChannel::Error err) {
  DataChannelErrorMessageData* data = new DataChannelErrorMessageData(
      ssrc, err);
  signaling_thread()->Post(this, MSG_CHANNEL_ERROR, data);
}

void DataChannel::OnDataChannelReadyToSend(bool writable) {
  // This is usded for congestion control to indicate that the stream is ready
  // to send by the MediaChannel, as opposed to OnReadyToSend, which indicates
  // that the transport channel is ready.
  signaling_thread()->Post(this, MSG_READYTOSENDDATA,
                           new DataChannelReadyToSendMessageData(writable));
}

void DataChannel::GetSrtpCryptoSuiteNames(
    std::vector<std::string>* ciphers) const {
  GetSupportedDataCryptoSuites(ciphers);
}

bool DataChannel::ShouldSetupDtlsSrtp() const {
  return (data_channel_type_ == DCT_RTP);
}

void DataChannel::OnStreamClosedRemotely(uint32_t sid) {
  rtc::TypedMessageData<uint32_t>* message =
      new rtc::TypedMessageData<uint32_t>(sid);
  signaling_thread()->Post(this, MSG_STREAMCLOSEDREMOTELY, message);
}

}  // namespace cricket

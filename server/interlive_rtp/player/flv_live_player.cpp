#include "flv_live_player.h"

#include "util/util.h"
#include "util/log.h"
#include "util/access.h"
#include "fragment/fragment.h"
#include "connection.h"
#include "target.h"
#include "media_manager/cache_manager.h"
#include "player/module_player.h"

#define HTTP_CONNECTION_HEADER "Connection: Close\r\n"
#define NOCACHE_HEADER "Expires: -1\r\nCache-Control: private, max-age=0\r\nPragma: no-cache\r\n"

//////////////////////////////////////////////////////////////////////////

BaseLivePlayer::BaseLivePlayer(LiveConnection *c) {
  m_connection = c;
}

BaseLivePlayer::~BaseLivePlayer() {
}

//////////////////////////////////////////////////////////////////////////

FlvLivePlayer::FlvLivePlayer(LiveConnection *c) : BaseLivePlayer(c), _cmng(media_manager::CacheManager::get_player_cache_instance()) {
  m_written_header = false;
  m_written_first_tag = false;
  m_latest_blockid = -1;
}

void FlvLivePlayer::OnWrite() {
  LiveConnection *c = m_connection;
  StreamId_Ext streamid = c->streamid;

  if (!m_written_header) {
    int status_code = 0;
    fragment::FLVHeader flvheader(streamid);
    if (!_cmng->get_miniblock_flv_header(streamid, flvheader, status_code)) {
      WRN("req live header failed, streamid= %s, status code= %d",
        streamid.unparse().c_str(), status_code);
      return;
    }

    // reponse header
    char rsp[1024];
    int used = snprintf(rsp, sizeof(rsp),
      "HTTP/1.1 200 OK\r\n"
      "Server: Youku Live Forward\r\n"
      "Content-Type: video/x-flv\r\n"
      NOCACHE_HEADER
      HTTP_CONNECTION_HEADER "\r\n");
    if (0 != buffer_append_ptr(c->wb, rsp, used)) {
      LiveConnection::Destroy(c);
      return;
    }

    flvheader.copy_header_to_buffer(c->wb);

    m_written_header = true;
  }

  if (!m_written_first_tag) {
    int status_code;
    fragment::FLVMiniBlock* block = _cmng->get_latest_miniblock(streamid, status_code);
    if (status_code >= 0) {
      m_latest_blockid = block->get_seq();
      uint32_t timeoffset = 0;
      block->copy_payload_to_buffer(c->wb, timeoffset, FLV_FLAG_BOTH);
      m_written_first_tag = true;
    }
    else {
      return;
    }
  }

  int status_code = 0;
  while (status_code >= 0){
    fragment::FLVMiniBlock* block = _cmng->get_miniblock_by_seq(streamid, m_latest_blockid + 1, status_code);
    if (status_code >= 0) {
      m_latest_blockid = block->get_seq();
      uint32_t timeoffset = 0;
      block->copy_payload_to_buffer(c->wb, timeoffset, FLV_FLAG_BOTH);
    }
  }
}

//////////////////////////////////////////////////////////////////////////

CrossdomainLivePlayer::CrossdomainLivePlayer(LiveConnection *c, const player_config *config) : BaseLivePlayer(c), m_config(config) {
}

void CrossdomainLivePlayer::OnWrite() {
  LiveConnection *c = m_connection;

  char rsp[1024];
  int used = snprintf(rsp, sizeof(rsp),
    "HTTP/1.0 200 OK\r\n"
    "Server: Youku Live Forward\r\n"
    "Content-Type: application/xml;charset=utf-8\r\n"
    NOCACHE_HEADER
    HTTP_CONNECTION_HEADER
    "Content-Length: %zu\r\n\r\n",
    m_config->crossdomain_len);

  if (0 != buffer_expand_capacity(c->wb, m_config->crossdomain_len + used)) {
    ACCESS("crossdomain %s:%d GET /crossdomain.xml 500",
      c->remote_ip, (int)c->remote.sin_port);
    util_http_rsp_error_code(c->ev_socket.fd, HTTP_500);
    //conn_close(c);
    return;
  }
  buffer_append_ptr(c->wb, rsp, used);
  buffer_append_ptr(c->wb, m_config->crossdomain, m_config->crossdomain_len);

  ACCESS("crossdomain %s:%d GET /crossdomain.xml 200",
    c->remote_ip, (int)c->remote.sin_port);
}
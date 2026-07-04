#include "jsb_web_websocket_server.h"

#include "modules/modules_enabled.gen.h"

#if JSB_WITH_WEB && defined(WEB_ENABLED) && defined(MODULE_WEBSOCKET_ENABLED)

#include "core/io/net_socket.h"
#include "core/io/stream_peer_tcp.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "modules/websocket/packet_buffer.h"
#include "modules/websocket/websocket_peer.h"

#include <emscripten.h>

extern "C"
{
    typedef void (*GodotJSWSOnOpen)(void *p_ref, char *p_protocol);
    typedef void (*GodotJSWSOnMessage)(void *p_ref, const uint8_t *p_buf, int p_buf_len, int p_is_string);
    typedef void (*GodotJSWSOnClose)(void *p_ref, int p_code, const char *p_reason, int p_is_clean);
    typedef void (*GodotJSWSOnError)(void *p_ref);

    extern int godotjs_websocket_server_available();
    extern int godotjs_websocket_server_listen(int p_port, const char *p_bind_address, int p_max_pending);
    extern void godotjs_websocket_server_close(int p_server_id);
    extern int godotjs_websocket_server_pending(int p_server_id);
    extern int godotjs_websocket_server_accept(int p_server_id);
    extern int godotjs_websocket_server_get_local_port(int p_server_id);

    extern int godotjs_websocket_peer_connect(void *p_ref, const char *p_url, const char *p_proto, GodotJSWSOnOpen p_on_open, GodotJSWSOnMessage p_on_message, GodotJSWSOnError p_on_error, GodotJSWSOnClose p_on_close);
    extern int godotjs_websocket_peer_create_accepted(int p_peer_id, void *p_ref, GodotJSWSOnOpen p_on_open, GodotJSWSOnMessage p_on_message, GodotJSWSOnError p_on_error, GodotJSWSOnClose p_on_close);
    extern int godotjs_websocket_peer_send(int p_peer_id, const uint8_t *p_buf, int p_buf_len, int p_raw);
    extern int godotjs_websocket_peer_buffered_amount(int p_peer_id);
    extern void godotjs_websocket_peer_close(int p_peer_id, int p_code, const char *p_reason);
    extern void godotjs_websocket_peer_destroy(int p_peer_id);
}

namespace
{
    HashMap<uint16_t, int> s_pending_server_peers;
    uint16_t s_next_pending_peer_token = 1;

    String address_to_host(const NetSocket::Address &p_addr)
    {
        if (!p_addr.ip().is_valid() || p_addr.ip().is_wildcard())
        {
            return "*";
        }
        return String(p_addr.ip());
    }

    uint16_t queue_pending_peer(int p_peer_id)
    {
        if (p_peer_id <= 0)
        {
            return 0;
        }

        for (uint32_t i = 0; i < UINT16_MAX; i++)
        {
            const uint16_t token = s_next_pending_peer_token++;
            if (s_next_pending_peer_token == 0)
            {
                s_next_pending_peer_token = 1;
            }
            if (!s_pending_server_peers.has(token))
            {
                s_pending_server_peers.insert(token, p_peer_id);
                return token;
            }
        }
        return 0;
    }

    int take_pending_peer(uint16_t p_token)
    {
        if (p_token == 0)
        {
            return 0;
        }

        HashMap<uint16_t, int>::Iterator it = s_pending_server_peers.find(p_token);
        if (!it)
        {
            return 0;
        }

        const int peer_id = it->value;
        s_pending_server_peers.remove(it);
        return peer_id;
    }

    class GodotJSWebSocketNetSocket : public NetSocket
    {
        GDSOFTCLASS(GodotJSWebSocketNetSocket, NetSocket);

    private:
        bool open_state = false;
        bool listening = false;
        int server_id = 0;
        uint16_t accepted_peer_token = 0;
        Address bound_address;
        Address socket_address;

    protected:
        static Ref<NetSocket> _create_func()
        {
            return memnew(GodotJSWebSocketNetSocket);
        }

    public:
        static void make_default()
        {
            _create = _create_func;
        }

        Error open(Family p_family, Type p_type, IP::Type &r_ip_type) override
        {
            (void)r_ip_type;

            ERR_FAIL_COND_V(open_state, ERR_ALREADY_IN_USE);
            ERR_FAIL_COND_V(p_family != Family::INET || p_type != TYPE_TCP, ERR_UNAVAILABLE);
            ERR_FAIL_COND_V(!godotjs_websocket_server_available(), ERR_UNAVAILABLE);

            open_state = true;
            bound_address = Address(IPAddress("*"), 0);
            socket_address = bound_address;
            return OK;
        }

        void close() override
        {
            if (server_id > 0)
            {
                godotjs_websocket_server_close(server_id);
                server_id = 0;
            }

            if (accepted_peer_token > 0)
            {
                const int peer_id = take_pending_peer(accepted_peer_token);
                if (peer_id > 0)
                {
                    godotjs_websocket_peer_destroy(peer_id);
                }
                accepted_peer_token = 0;
            }

            open_state = false;
            listening = false;
        }

        Error bind(Address p_addr) override
        {
            ERR_FAIL_COND_V(!open_state, ERR_UNCONFIGURED);
            ERR_FAIL_COND_V(listening, ERR_ALREADY_IN_USE);

            bound_address = p_addr;
            socket_address = p_addr;
            return OK;
        }

        Error listen(int p_max_pending) override
        {
            ERR_FAIL_COND_V(!open_state, ERR_UNCONFIGURED);
            ERR_FAIL_COND_V(listening, ERR_ALREADY_IN_USE);

            const String bind_address = address_to_host(bound_address);
            const CharString bind_address_utf8 = bind_address.utf8();
            server_id = godotjs_websocket_server_listen(bound_address.port(), bind_address_utf8.get_data(), p_max_pending);
            if (server_id <= 0)
            {
                server_id = 0;
                return FAILED;
            }

            listening = true;
            return OK;
        }

        Error connect_to_host(Address) override
        {
            return ERR_UNAVAILABLE;
        }

        Error poll(PollType p_type, int) const override
        {
            ERR_FAIL_COND_V(!open_state, ERR_UNAVAILABLE);

            if (listening)
            {
                if (p_type == POLL_TYPE_OUT)
                {
                    return OK;
                }
                return godotjs_websocket_server_pending(server_id) > 0 ? OK : ERR_BUSY;
            }

            if (accepted_peer_token > 0)
            {
                return p_type == POLL_TYPE_IN ? ERR_BUSY : OK;
            }

            return ERR_BUSY;
        }

        Error recv(uint8_t *, int, int &r_read) override
        {
            r_read = 0;
            return ERR_UNAVAILABLE;
        }

        Error recvfrom(uint8_t *, int, int &r_read, IPAddress &, uint16_t &, bool = false) override
        {
            r_read = 0;
            return ERR_UNAVAILABLE;
        }

        Error send(const uint8_t *, int, int &r_sent) override
        {
            r_sent = 0;
            return ERR_UNAVAILABLE;
        }

        Error sendto(const uint8_t *, int, int &r_sent, IPAddress, uint16_t) override
        {
            r_sent = 0;
            return ERR_UNAVAILABLE;
        }

        Ref<NetSocket> accept(Address &r_addr) override
        {
            if (!listening || server_id <= 0)
            {
                return Ref<NetSocket>();
            }

            const int peer_id = godotjs_websocket_server_accept(server_id);
            if (peer_id <= 0)
            {
                return Ref<NetSocket>();
            }

            const uint16_t peer_token = queue_pending_peer(peer_id);
            if (peer_token == 0)
            {
                godotjs_websocket_peer_destroy(peer_id);
                return Ref<NetSocket>();
            }

            Ref<GodotJSWebSocketNetSocket> socket;
            socket.instantiate();
            socket->open_state = true;
            socket->accepted_peer_token = peer_token;
            socket->socket_address = Address(IPAddress("127.0.0.1"), peer_token);
            r_addr = socket->socket_address;
            return socket;
        }

        bool is_open() const override
        {
            return open_state;
        }

        int get_available_bytes() const override
        {
            return 0;
        }

        Error get_socket_address(Address *r_addr) const override
        {
            ERR_FAIL_NULL_V(r_addr, ERR_INVALID_PARAMETER);

            if (listening && server_id > 0)
            {
                const int local_port = godotjs_websocket_server_get_local_port(server_id);
                if (local_port > 0)
                {
                    *r_addr = Address(bound_address.ip(), local_port);
                    return OK;
                }
            }

            *r_addr = socket_address;
            return OK;
        }

        Error set_broadcasting_enabled(bool) override
        {
            return ERR_UNAVAILABLE;
        }

        void set_blocking_enabled(bool) override {}
        void set_ipv6_only_enabled(bool) override {}
        void set_tcp_no_delay_enabled(bool) override {}
        void set_reuse_address_enabled(bool) override {}

        Error join_multicast_group(const IPAddress &, const String &) override
        {
            return ERR_UNAVAILABLE;
        }

        Error leave_multicast_group(const IPAddress &, const String &) override
        {
            return ERR_UNAVAILABLE;
        }

        ~GodotJSWebSocketNetSocket()
        {
            close();
        }
    };

    class GodotJSWebSocketPeer : public WebSocketPeer
    {
    private:
        int peer_sock = 0;
        State ready_state = STATE_CLOSED;
        Vector<uint8_t> packet_buffer;
        PacketBuffer<uint8_t> in_buffer;
        uint8_t was_string = 0;
        int close_code = -1;
        String close_reason;
        String selected_protocol;
        String requested_url;

        static WebSocketPeer *_create(bool p_notify_postinitialize)
        {
            return static_cast<WebSocketPeer *>(ClassDB::creator<GodotJSWebSocketPeer>(p_notify_postinitialize));
        }

        static void _on_open(void *p_obj, char *p_protocol)
        {
            GodotJSWebSocketPeer *peer = static_cast<GodotJSWebSocketPeer *>(p_obj);
            peer->ready_state = STATE_OPEN;
            peer->selected_protocol.clear();
            if (p_protocol)
            {
                peer->selected_protocol.append_utf8(p_protocol);
            }
        }

        static void _on_message(void *p_obj, const uint8_t *p_data, int p_data_size, int p_is_string)
        {
            GodotJSWebSocketPeer *peer = static_cast<GodotJSWebSocketPeer *>(p_obj);
            uint8_t is_string = p_is_string ? 1 : 0;
            peer->in_buffer.write_packet(p_data, p_data_size, &is_string);
        }

        static void _on_error(void *p_obj)
        {
            GodotJSWebSocketPeer *peer = static_cast<GodotJSWebSocketPeer *>(p_obj);
            peer->ready_state = STATE_CLOSED;
        }

        static void _on_close(void *p_obj, int p_code, const char *p_reason, int)
        {
            GodotJSWebSocketPeer *peer = static_cast<GodotJSWebSocketPeer *>(p_obj);
            peer->close_code = p_code;
            peer->close_reason.clear();
            if (p_reason)
            {
                peer->close_reason.append_utf8(p_reason);
            }
            peer->ready_state = STATE_CLOSED;
        }

        void _init_buffers()
        {
            in_buffer.resize(Math::nearest_shift((uint32_t)inbound_buffer_size), max_queued_packets);
            packet_buffer.resize(inbound_buffer_size);
        }

        void _clear()
        {
            if (peer_sock > 0)
            {
                godotjs_websocket_peer_destroy(peer_sock);
                peer_sock = 0;
            }
            ready_state = STATE_CLOSED;
            was_string = 0;
            close_code = -1;
            close_reason.clear();
            selected_protocol.clear();
            requested_url.clear();
            in_buffer.clear();
            packet_buffer.clear();
        }

        Error _send(const uint8_t *p_buffer, int p_buffer_size, bool p_binary)
        {
            ERR_FAIL_COND_V(peer_sock <= 0, ERR_UNCONFIGURED);
            ERR_FAIL_COND_V(outbound_buffer_size > 0 && (get_current_outbound_buffered_amount() + p_buffer_size >= outbound_buffer_size), ERR_OUT_OF_MEMORY);

            if (godotjs_websocket_peer_send(peer_sock, p_buffer, p_buffer_size, p_binary ? 1 : 0) != 0)
            {
                return FAILED;
            }
            return OK;
        }

    public:
        static void make_default()
        {
            WebSocketPeer::_create = GodotJSWebSocketPeer::_create;
        }

        int get_available_packet_count() const override
        {
            return in_buffer.packets_left();
        }

        Error get_packet(const uint8_t **r_buffer, int &r_buffer_size) override
        {
            if (in_buffer.packets_left() == 0)
            {
                return ERR_UNAVAILABLE;
            }

            int read = 0;
            Error err = in_buffer.read_packet(packet_buffer.ptrw(), packet_buffer.size(), &was_string, read);
            ERR_FAIL_COND_V(err != OK, err);

            *r_buffer = packet_buffer.ptr();
            r_buffer_size = read;
            return OK;
        }

        Error put_packet(const uint8_t *p_buffer, int p_buffer_size) override
        {
            return _send(p_buffer, p_buffer_size, true);
        }

        int get_max_packet_size() const override
        {
            return packet_buffer.size();
        }

        Error send(const uint8_t *p_buffer, int p_buffer_size, WriteMode p_mode) override
        {
            return _send(p_buffer, p_buffer_size, p_mode == WRITE_MODE_BINARY);
        }

        Error connect_to_url(const String &p_url, const Ref<TLSOptions> &p_tls_options) override
        {
            ERR_FAIL_COND_V(p_url.is_empty(), ERR_INVALID_PARAMETER);
            ERR_FAIL_COND_V(p_tls_options.is_valid() && p_tls_options->is_server(), ERR_INVALID_PARAMETER);
            ERR_FAIL_COND_V(ready_state != STATE_CLOSED && ready_state != STATE_CLOSING, ERR_ALREADY_IN_USE);

            _clear();

            String host;
            String path;
            String scheme;
            String fragment;
            int port = 0;
            Error err = p_url.parse_url(scheme, host, port, path, fragment);
            ERR_FAIL_COND_V_MSG(err != OK, err, "Invalid URL: " + p_url);

            if (scheme.is_empty())
            {
                scheme = "ws://";
            }
            ERR_FAIL_COND_V_MSG(scheme != "ws://" && scheme != "wss://", ERR_INVALID_PARAMETER, vformat("Invalid protocol: \"%s\" (must be either \"ws://\" or \"wss://\").", scheme));

            String proto_string;
            for (int i = 0; i < supported_protocols.size(); i++)
            {
                if (i != 0)
                {
                    proto_string += ",";
                }
                proto_string += supported_protocols[i];
            }

            if (handshake_headers.size())
            {
                WARN_PRINT_ONCE("Custom headers are not supported in Web platform.");
            }

            requested_url = scheme + host;

            if (port && ((scheme == "ws://" && port != 80) || (scheme == "wss://" && port != 443)))
            {
                requested_url += ":" + String::num_int64(port);
            }

            if (!path.is_empty())
            {
                requested_url += path;
            }

            _init_buffers();

            const CharString requested_url_utf8 = requested_url.utf8();
            const CharString proto_string_utf8 = proto_string.utf8();
            ready_state = STATE_CONNECTING;
            peer_sock = godotjs_websocket_peer_connect(this, requested_url_utf8.get_data(), proto_string_utf8.get_data(), &_on_open, &_on_message, &_on_error, &_on_close);
            if (peer_sock <= 0)
            {
                _clear();
                return FAILED;
            }

            return OK;
        }

        Error accept_stream(const Ref<StreamPeer> &p_stream) override
        {
            ERR_FAIL_COND_V(p_stream.is_null(), ERR_INVALID_PARAMETER);
            ERR_FAIL_COND_V(ready_state != STATE_CLOSED && ready_state != STATE_CLOSING, ERR_ALREADY_IN_USE);

            ERR_FAIL_COND_V(!p_stream->is_class_ptr(StreamPeerTCP::get_class_ptr_static()), ERR_INVALID_PARAMETER);

            Ref<StreamPeerTCP> tcp = p_stream;
            const uint16_t peer_token = (uint16_t) tcp->get_connected_port();
            const int accepted_peer_id = take_pending_peer(peer_token);
            if (accepted_peer_id <= 0)
            {
                WARN_PRINT_ONCE("No host WebSocket is pending for the StreamPeerTCP passed to WebSocketPeer.accept_stream() in the GodotJS Web provider.");
                return ERR_UNAVAILABLE;
            }

            _clear();
            _init_buffers();

            ready_state = STATE_CONNECTING;
            peer_sock = godotjs_websocket_peer_create_accepted(accepted_peer_id, this, &_on_open, &_on_message, &_on_error, &_on_close);
            if (peer_sock <= 0)
            {
                godotjs_websocket_peer_destroy(accepted_peer_id);
                _clear();
                return FAILED;
            }

            if (ready_state == STATE_CONNECTING)
            {
                ready_state = STATE_OPEN;
            }
            return OK;
        }

        void close(int p_code = 1000, const String &p_reason = "") override
        {
            if (p_code < 0)
            {
                _clear();
                return;
            }

            if (ready_state == STATE_CONNECTING || ready_state == STATE_OPEN)
            {
                ready_state = STATE_CLOSING;
                if (peer_sock > 0)
                {
                    const CharString reason_utf8 = p_reason.utf8();
                    godotjs_websocket_peer_close(peer_sock, p_code, reason_utf8.get_data());
                }
                else
                {
                    ready_state = STATE_CLOSED;
                }
            }
            in_buffer.clear();
            packet_buffer.clear();
        }

        void poll() override
        {
            // Host WebSocket events are delivered by the JavaScript callbacks.
        }

        State get_ready_state() const override
        {
            return ready_state;
        }

        int get_close_code() const override
        {
            return close_code;
        }

        String get_close_reason() const override
        {
            return close_reason;
        }

        int get_current_outbound_buffered_amount() const override
        {
            return peer_sock > 0 ? godotjs_websocket_peer_buffered_amount(peer_sock) : 0;
        }

        IPAddress get_connected_host() const override
        {
            ERR_FAIL_V_MSG(IPAddress(), "Not supported in Web export.");
        }

        uint16_t get_connected_port() const override
        {
            ERR_FAIL_V_MSG(0, "Not supported in Web export.");
        }

        String get_selected_protocol() const override
        {
            return selected_protocol;
        }

        String get_requested_url() const override
        {
            return requested_url;
        }

        bool was_string_packet() const override
        {
            return was_string;
        }

        void set_no_delay(bool) override {}

        GodotJSWebSocketPeer() {}

        ~GodotJSWebSocketPeer()
        {
            _clear();
        }
    };
}

#endif // JSB_WITH_WEB && WEB_ENABLED && MODULE_WEBSOCKET_ENABLED

namespace jsb::impl
{
    void initialize_websocket_server()
    {
#if JSB_WITH_WEB && defined(WEB_ENABLED) && defined(MODULE_WEBSOCKET_ENABLED)
        if (godotjs_websocket_server_available())
        {
            GodotJSWebSocketNetSocket::make_default();
            GodotJSWebSocketPeer::make_default();
        }
#endif
    }
}

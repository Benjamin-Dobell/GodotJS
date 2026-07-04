/**************************************************************************/
/*  library_godotjs_websocket_server.js                                   */
/**************************************************************************/

const GodotJSWebSocketServerLibrary = {
	$GodotJSWebSocketServer__deps: ['$IDHandler', '$GodotRuntime'],
	$GodotJSWebSocketServer: {
		// Host hook: set globalThis.GodotJSWebSocketServerProvider (or GodotJSWebSocketServer)
		// with synchronous listen/poll(or pending)/accept/close methods.
		_global: function () {
			return globalThis;
		},

		_getProvider: function () {
			const g = GodotJSWebSocketServer._global();
			if (!g) {
				return null;
			}

			const provider = g['GodotJSWebSocketServerProvider'] || g['GodotJSWebSocketServer'] || g['__godotjs_websocket_server'];
			if (!provider || typeof provider['listen'] !== 'function' || typeof provider['accept'] !== 'function') {
				return null;
			}
			if (typeof provider['isAvailable'] === 'function' && !provider['isAvailable']()) {
				return null;
			}
			return provider;
		},

		_callProvider: function (provider, name, args, fallback) {
			const fn = provider && provider[name];
			if (typeof fn !== 'function') {
				return fallback;
			}
			try {
				return fn.apply(provider, args);
			} catch (e) {
				GodotRuntime.error('GodotJS WebSocket server provider failed in ' + name + ':', e);
				return fallback;
			}
		},

		_isOpen: function (socket) {
			if (!socket) {
				return false;
			}
			if (typeof socket.readyState !== 'number') {
				return true;
			}
			const open = typeof socket.OPEN === 'number' ? socket.OPEN : 1;
			return socket.readyState === open;
		},

		_canClose: function (socket) {
			if (!socket) {
				return false;
			}
			if (typeof socket.readyState !== 'number') {
				return true;
			}
			const closing = typeof socket.CLOSING === 'number' ? socket.CLOSING : 2;
			return socket.readyState < closing;
		},

		_protocol: function (socket) {
			if (!socket || typeof socket.protocol !== 'string') {
				return '';
			}
			return socket.protocol;
		},

		_notifyOpen: function (id, callback) {
			const ref = IDHandler.get(id);
			if (!ref || ref.open_notified) {
				return;
			}
			ref.open_notified = true;

			const c_str = GodotRuntime.allocString(GodotJSWebSocketServer._protocol(ref.socket));
			callback(c_str);
			GodotRuntime.free(c_str);
		},

		_decodeReason: function (reason) {
			if (typeof reason === 'string') {
				return reason;
			}
			if (reason instanceof ArrayBuffer) {
				return new TextDecoder('utf-8').decode(new Uint8Array(reason));
			}
			if (ArrayBuffer.isView(reason)) {
				return new TextDecoder('utf-8').decode(new Uint8Array(reason.buffer, reason.byteOffset, reason.byteLength));
			}
			if (typeof reason === 'undefined' || reason === null) {
				return '';
			}
			return String(reason);
		},

		_messageBytes: function (event, is_binary) {
			const data = event && typeof event.data !== 'undefined' ? event.data : event;
			let is_string = 0;
			let buffer = null;

			if (typeof data === 'string') {
				is_string = 1;
				buffer = new TextEncoder('utf-8').encode(data);
			} else if (data instanceof ArrayBuffer) {
				buffer = new Uint8Array(data);
			} else if (ArrayBuffer.isView(data)) {
				buffer = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
			} else {
				GodotRuntime.error('Unsupported WebSocket message type from host provider');
				return null;
			}

			if (is_binary === false) {
				is_string = 1;
			}
			return { 'buffer': buffer, 'is_string': is_string };
		},

		_onMessage: function (id, callback, event, is_binary) {
			const ref = IDHandler.get(id);
			if (!ref) {
				return;
			}

			const packet = GodotJSWebSocketServer._messageBytes(event, is_binary);
			if (!packet) {
				return;
			}

			const buffer = packet['buffer'];
			const len = buffer.length * buffer.BYTES_PER_ELEMENT;
			const out = GodotRuntime.malloc(len);
			HEAPU8.set(buffer, out);
			callback(out, len, packet['is_string']);
			GodotRuntime.free(out);
		},

		_onError: function (id, callback, event) {
			if (!IDHandler.get(id)) {
				return;
			}
			callback();
		},

		_onClose: function (id, callback, event, reason_arg) {
			const ref = IDHandler.get(id);
			if (!ref) {
				return;
			}

			const code = typeof event === 'number' ? event : (event && typeof event.code === 'number' ? event.code : 1005);
			const reason = typeof event === 'number' ? GodotJSWebSocketServer._decodeReason(reason_arg) : GodotJSWebSocketServer._decodeReason(event && event.reason);
			const was_clean = event && typeof event.wasClean === 'boolean' ? event.wasClean : true;
			const c_str = GodotRuntime.allocString(reason);
			callback(code, c_str, was_clean ? 1 : 0);
			GodotRuntime.free(c_str);
		},

		_addListener: function (ref, event, handler) {
			const socket = ref.socket;
			if (socket && typeof socket.addEventListener === 'function') {
				socket.addEventListener(event, handler);
				ref.cleanup.push(function () {
					socket.removeEventListener(event, handler);
				});
				return;
			}

			if (socket && typeof socket.on === 'function') {
				socket.on(event, handler);
				ref.cleanup.push(function () {
					if (typeof socket.off === 'function') {
						socket.off(event, handler);
					} else if (typeof socket.removeListener === 'function') {
						socket.removeListener(event, handler);
					}
				});
				return;
			}

			const property = 'on' + event;
			if (socket && property in socket) {
				const previous = socket[property];
				socket[property] = handler;
				ref.cleanup.push(function () {
					if (socket[property] === handler) {
						socket[property] = previous || null;
					}
				});
			}
		},

		_setupPeer: function (id, p_ref, p_on_open, p_on_message, p_on_error, p_on_close, accepted) {
			const ref = IDHandler.get(id);
			if (!ref || !ref.socket) {
				return 0;
			}

			const on_open = GodotRuntime.get_func(p_on_open).bind(null, p_ref);
			const on_message = GodotRuntime.get_func(p_on_message).bind(null, p_ref);
			const on_error = GodotRuntime.get_func(p_on_error).bind(null, p_ref);
			const on_close = GodotRuntime.get_func(p_on_close).bind(null, p_ref);

			ref.cleanup = ref.cleanup || [];
			ref.open_notified = false;

			try {
				if ('binaryType' in ref.socket) {
					ref.socket.binaryType = 'arraybuffer';
				}
				if (accepted && typeof ref.socket.accept === 'function') {
					ref.socket.accept();
				}
			} catch (e) {
				GodotRuntime.error('Failed to prepare host WebSocket:', e);
				return 0;
			}

			GodotJSWebSocketServer._addListener(ref, 'open', GodotJSWebSocketServer._notifyOpen.bind(null, id, on_open));
			GodotJSWebSocketServer._addListener(ref, 'message', GodotJSWebSocketServer._onMessage.bind(null, id, on_message));
			GodotJSWebSocketServer._addListener(ref, 'error', GodotJSWebSocketServer._onError.bind(null, id, on_error));
			GodotJSWebSocketServer._addListener(ref, 'close', GodotJSWebSocketServer._onClose.bind(null, id, on_close));

			if (accepted || GodotJSWebSocketServer._isOpen(ref.socket)) {
				GodotJSWebSocketServer._notifyOpen(id, on_open);
			}
			return id;
		},

		listen: function (port, bind_address, max_pending) {
			const provider = GodotJSWebSocketServer._getProvider();
			if (!provider) {
				return 0;
			}

			const options = {
				'port': port,
				'bindAddress': bind_address,
				'maxPending': max_pending,
			};
			const handle = GodotJSWebSocketServer._callProvider(provider, 'listen', [options, port, bind_address, max_pending], null);
			if (handle === null || typeof handle === 'undefined' || handle === false) {
				return 0;
			}

			return IDHandler.add({
				'provider': provider,
				'handle': handle,
				'port': port,
				'bindAddress': bind_address,
				'maxPending': max_pending,
			});
		},

		closeServer: function (server_id) {
			const server = IDHandler.get(server_id);
			if (!server) {
				return;
			}

			GodotJSWebSocketServer._callProvider(server.provider, 'close', [server.handle, server], undefined);
			IDHandler.remove(server_id);
		},

		pending: function (server_id) {
			const server = IDHandler.get(server_id);
			if (!server) {
				return 0;
			}

			let count = GodotJSWebSocketServer._callProvider(server.provider, 'poll', [server.handle, server], undefined);
			if (typeof count === 'undefined') {
				count = GodotJSWebSocketServer._callProvider(server.provider, 'pending', [server.handle, server], 0);
			}
			if (typeof count === 'boolean') {
				return count ? 1 : 0;
			}
			return Number(count) || 0;
		},

		accept: function (server_id) {
			const server = IDHandler.get(server_id);
			if (!server) {
				return 0;
			}

			const socket = GodotJSWebSocketServer._callProvider(server.provider, 'accept', [server.handle, server], null);
			if (!socket) {
				return 0;
			}

			return IDHandler.add({
				'socket': socket,
				'cleanup': [],
				'open_notified': false,
			});
		},

		localPort: function (server_id) {
			const server = IDHandler.get(server_id);
			if (!server) {
				return 0;
			}

			const port = GodotJSWebSocketServer._callProvider(server.provider, 'getLocalPort', [server.handle, server], server.port);
			return Number(port) || 0;
		},

		connect: function (p_ref, url, protocols, p_on_open, p_on_message, p_on_error, p_on_close) {
			if (typeof WebSocket === 'undefined') {
				return 0;
			}

			let socket = null;
			try {
				if (protocols) {
					socket = new WebSocket(url, protocols.split(','));
				} else {
					socket = new WebSocket(url);
				}
			} catch (e) {
				return 0;
			}

			const id = IDHandler.add({
				'socket': socket,
				'cleanup': [],
				'open_notified': false,
			});
			return GodotJSWebSocketServer._setupPeer(id, p_ref, p_on_open, p_on_message, p_on_error, p_on_close, false);
		},

		createAccepted: function (peer_id, p_ref, p_on_open, p_on_message, p_on_error, p_on_close) {
			return GodotJSWebSocketServer._setupPeer(peer_id, p_ref, p_on_open, p_on_message, p_on_error, p_on_close, true);
		},

		send: function (peer_id, data) {
			const ref = IDHandler.get(peer_id);
			if (!ref || !GodotJSWebSocketServer._isOpen(ref.socket)) {
				return 1;
			}

			try {
				ref.socket.send(data);
			} catch (e) {
				GodotRuntime.error('Failed to send host WebSocket packet:', e);
				return 1;
			}
			return 0;
		},

		bufferedAmount: function (peer_id) {
			const ref = IDHandler.get(peer_id);
			if (!ref || !ref.socket || typeof ref.socket.bufferedAmount !== 'number') {
				return 0;
			}
			return ref.socket.bufferedAmount;
		},

		closePeer: function (peer_id, code, reason) {
			const ref = IDHandler.get(peer_id);
			if (!ref || !GodotJSWebSocketServer._canClose(ref.socket) || typeof ref.socket.close !== 'function') {
				return;
			}

			try {
				ref.socket.close(code, reason);
			} catch (e) {
				GodotRuntime.error('Failed to close host WebSocket:', e);
			}
		},

		destroyPeer: function (peer_id) {
			const ref = IDHandler.get(peer_id);
			if (!ref) {
				return;
			}

			GodotJSWebSocketServer.closePeer(peer_id, 3001, 'destroyed');
			if (Array.isArray(ref.cleanup)) {
				for (let i = 0; i < ref.cleanup.length; i++) {
					ref.cleanup[i]();
				}
			}
			IDHandler.remove(peer_id);
		},
	},

	godotjs_websocket_server_available__proxy: 'sync',
	godotjs_websocket_server_available__sig: 'i',
	godotjs_websocket_server_available: function () {
		return GodotJSWebSocketServer._getProvider() ? 1 : 0;
	},

	godotjs_websocket_server_listen__proxy: 'sync',
	godotjs_websocket_server_listen__sig: 'iiii',
	godotjs_websocket_server_listen: function (p_port, p_bind_address, p_max_pending) {
		const bind_address = GodotRuntime.parseString(p_bind_address);
		return GodotJSWebSocketServer.listen(p_port, bind_address, p_max_pending);
	},

	godotjs_websocket_server_close__proxy: 'sync',
	godotjs_websocket_server_close__sig: 'vi',
	godotjs_websocket_server_close: function (p_server_id) {
		GodotJSWebSocketServer.closeServer(p_server_id);
	},

	godotjs_websocket_server_pending__proxy: 'sync',
	godotjs_websocket_server_pending__sig: 'ii',
	godotjs_websocket_server_pending: function (p_server_id) {
		return GodotJSWebSocketServer.pending(p_server_id);
	},

	godotjs_websocket_server_accept__proxy: 'sync',
	godotjs_websocket_server_accept__sig: 'ii',
	godotjs_websocket_server_accept: function (p_server_id) {
		return GodotJSWebSocketServer.accept(p_server_id);
	},

	godotjs_websocket_server_get_local_port__proxy: 'sync',
	godotjs_websocket_server_get_local_port__sig: 'ii',
	godotjs_websocket_server_get_local_port: function (p_server_id) {
		return GodotJSWebSocketServer.localPort(p_server_id);
	},

	godotjs_websocket_peer_connect__proxy: 'sync',
	godotjs_websocket_peer_connect__sig: 'iiiiiiii',
	godotjs_websocket_peer_connect: function (p_ref, p_url, p_proto, p_on_open, p_on_message, p_on_error, p_on_close) {
		const url = GodotRuntime.parseString(p_url);
		const protocols = GodotRuntime.parseString(p_proto);
		return GodotJSWebSocketServer.connect(p_ref, url, protocols, p_on_open, p_on_message, p_on_error, p_on_close);
	},

	godotjs_websocket_peer_create_accepted__proxy: 'sync',
	godotjs_websocket_peer_create_accepted__sig: 'iiiiiii',
	godotjs_websocket_peer_create_accepted: function (p_peer_id, p_ref, p_on_open, p_on_message, p_on_error, p_on_close) {
		return GodotJSWebSocketServer.createAccepted(p_peer_id, p_ref, p_on_open, p_on_message, p_on_error, p_on_close);
	},

	godotjs_websocket_peer_send__proxy: 'sync',
	godotjs_websocket_peer_send__sig: 'iiiii',
	godotjs_websocket_peer_send: function (p_peer_id, p_buf, p_buf_len, p_raw) {
		let out = GodotRuntime.heapSlice(HEAPU8, p_buf, p_buf_len);
		if (!p_raw) {
			out = new TextDecoder('utf-8').decode(out);
		}
		return GodotJSWebSocketServer.send(p_peer_id, out);
	},

	godotjs_websocket_peer_buffered_amount__proxy: 'sync',
	godotjs_websocket_peer_buffered_amount__sig: 'ii',
	godotjs_websocket_peer_buffered_amount: function (p_peer_id) {
		return GodotJSWebSocketServer.bufferedAmount(p_peer_id);
	},

	godotjs_websocket_peer_close__proxy: 'sync',
	godotjs_websocket_peer_close__sig: 'viii',
	godotjs_websocket_peer_close: function (p_peer_id, p_code, p_reason) {
		const reason = GodotRuntime.parseString(p_reason);
		GodotJSWebSocketServer.closePeer(p_peer_id, p_code, reason);
	},

	godotjs_websocket_peer_destroy__proxy: 'sync',
	godotjs_websocket_peer_destroy__sig: 'vi',
	godotjs_websocket_peer_destroy: function (p_peer_id) {
		GodotJSWebSocketServer.destroyPeer(p_peer_id);
	},
};

autoAddDeps(GodotJSWebSocketServerLibrary, '$GodotJSWebSocketServer');
mergeInto(LibraryManager.library, GodotJSWebSocketServerLibrary);

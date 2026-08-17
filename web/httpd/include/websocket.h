#pragma once

#include <string>

namespace rts2web
{

/**
 * RFC 6455 WebSocket protocol mechanics - handshake key computation and
 * frame encode/decode. No transport/socket handling here (that's
 * httpd.cpp, wired through libmicrohttpd's MHD_UPGRADE); this is just
 * the wire format.
 */

/** Compute Sec-WebSocket-Accept from the client's Sec-WebSocket-Key. */
std::string wsAcceptKey (const std::string &clientKey);

/** Encode a server->client text frame (unmasked - RFC 6455 5.1 forbids
 * the server from masking). */
std::string wsEncodeTextFrame (const std::string &payload);

/** Encode a server->client close frame (empty payload). */
std::string wsEncodeCloseFrame ();

/**
 * Parse one client->server frame out of buf (len bytes available).
 * Returns the number of bytes consumed, or 0 if buf doesn't yet hold a
 * complete frame - the caller should keep buffering and retry once more
 * data has arrived. On a nonzero return, opcode is the frame's RFC 6455
 * opcode (0x1 text, 0x2 binary, 0x8 close, 0x9 ping, 0xA pong, 0x0
 * continuation) and payload its unmasked contents.
 *
 * Deliberately minimal: this is a push-only channel (the client isn't
 * expected to send application data), so fragmented data-frame
 * reassembly isn't implemented - a continuation frame is just returned
 * as opcode 0x0 with its own payload, for the caller to ignore. Control
 * frames (close/ping/pong) are never fragmented per spec, so this is
 * always correct for the frames the caller actually acts on.
 */
size_t wsConsumeClientFrame (const unsigned char *buf, size_t len, int &opcode, std::string &payload);

}

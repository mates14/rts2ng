#include "websocket.h"

#include <openssl/evp.h>

#include <cstdint>
#include <cstring>

namespace
{
const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}

std::string rts2web::wsAcceptKey (const std::string &clientKey)
{
	std::string combined = clientKey + WS_GUID;

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digestLen = 0;
	EVP_Digest (combined.data (), combined.size (), digest, &digestLen, EVP_sha1 (), nullptr);

	// EVP_EncodeBlock's output is 4*ceil(n/3) bytes, no NUL guaranteed
	// beyond that - 32 bytes is comfortably more than SHA-1's 20-byte
	// digest needs (28 bytes encoded).
	unsigned char b64[32];
	int outLen = EVP_EncodeBlock (b64, digest, (int) digestLen);
	return std::string ((const char *) b64, outLen);
}

std::string rts2web::wsEncodeTextFrame (const std::string &payload)
{
	std::string frame;
	frame.push_back ((char) 0x81);				 // FIN=1, opcode=0x1 (text)

	size_t len = payload.size ();
	if (len <= 125)
	{
		frame.push_back ((char) len);
	}
	else if (len <= 0xFFFF)
	{
		frame.push_back ((char) 126);
		frame.push_back ((char) ((len >> 8) & 0xFF));
		frame.push_back ((char) (len & 0xFF));
	}
	else
	{
		frame.push_back ((char) 127);
		for (int i = 7; i >= 0; i--)
			frame.push_back ((char) ((len >> (8 * i)) & 0xFF));
	}
	frame.append (payload);
	return frame;
}

std::string rts2web::wsEncodeCloseFrame ()
{
	std::string frame;
	frame.push_back ((char) 0x88);				 // FIN=1, opcode=0x8 (close)
	frame.push_back ((char) 0x00);				 // zero-length payload
	return frame;
}

size_t rts2web::wsConsumeClientFrame (const unsigned char *buf, size_t len, int &opcode, std::string &payload)
{
	if (len < 2)
		return 0;

	bool masked = buf[1] & 0x80;
	uint64_t payloadLen = buf[1] & 0x7F;
	size_t pos = 2;

	if (payloadLen == 126)
	{
		if (len < pos + 2)
			return 0;
		payloadLen = ((uint64_t) buf[pos] << 8) | buf[pos + 1];
		pos += 2;
	}
	else if (payloadLen == 127)
	{
		if (len < pos + 8)
			return 0;
		payloadLen = 0;
		for (int i = 0; i < 8; i++)
			payloadLen = (payloadLen << 8) | buf[pos + i];
		pos += 8;
	}

	unsigned char maskKey[4] = { 0, 0, 0, 0 };
	if (masked)
	{
		if (len < pos + 4)
			return 0;
		memcpy (maskKey, buf + pos, 4);
		pos += 4;
	}

	if (len < pos + payloadLen)
		return 0;						 // incomplete frame - wait for more data

	opcode = buf[0] & 0x0F;
	payload.resize (payloadLen);
	for (uint64_t i = 0; i < payloadLen; i++)
		payload[i] = masked ? (char) (buf[pos + i] ^ maskKey[i % 4]) : (char) buf[pos + i];

	return pos + payloadLen;
}

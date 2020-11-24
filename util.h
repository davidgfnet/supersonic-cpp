
#ifndef __UTIL_HDR_H__
#define __UTIL_HDR_H__

#include <stdint.h>
#include <string>
#include <unordered_map>

// Decodes hex -> bin
unsigned char hexdec(char c);
std::string hexdecode(std::string s);

// 64 bit num hex encoded
std::string hexencode64(uint64_t n);
uint64_t hexdecode64(std::string s);

// Decodes a URL to its original string
std::string urldec(const std::string &s);

// Escapes strings (for XML and JSON)
std::string cescape(std::string content, bool isxml = false);

// Decodes a base64 encoded string to a war byte buffer
std::string base64Decode(const std::string & input);

// Parses variables in the body of a request, or in a GET query
std::unordered_multimap<std::string, std::string> parse_vars(std::string body);

// Parses a Range Header expression into byte ranges
std::pair<uint64_t, uint64_t> parse_range(std::string h);

// Some inline stuff, LE magic
static uint64_t a2i64(const uint8_t *h) {
	return  ((uint64_t)h[0] <<  0) |
	        ((uint64_t)h[1] <<  8) |
	        ((uint64_t)h[2] << 16) |
	        ((uint64_t)h[3] << 24) |
	        ((uint64_t)h[4] << 32) |
	        ((uint64_t)h[5] << 40) |
	        ((uint64_t)h[6] << 48) |
	        ((uint64_t)h[7] << 56);
}

static std::string i642a(uint64_t n) {
	std::string r(8, 0);
	for (unsigned i = 0; i < 8; i++)
		r[i] = n >> (i << 3);
	return r;
}

#endif


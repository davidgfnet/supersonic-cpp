
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

#endif


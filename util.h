
#ifndef __UTIL_HDR_H__
#define __UTIL_HDR_H__

#include <stdint.h>
#include <string>

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

#endif


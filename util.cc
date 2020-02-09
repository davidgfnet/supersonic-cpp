

#include "util.h"

unsigned char hexdec(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 0;
}

std::string hexdecode(std::string s) {
	if (s.size() & 1)
		return {};
	std::string ret;
	for (unsigned i = 0; i < s.size(); i += 2)
		ret.push_back((char)((hexdec(s[i]) << 4) | hexdec(s[i+1])));
	return ret;
}

uint64_t hexdecode64(std::string s) {
	uint64_t ret = 0;
	for (char c : s) {
		ret <<= 4;
		ret |= hexdec(c);
	}
	return ret;
}

std::string hexencode64(uint64_t n) {
	const static char hcs[] = "0123456789abcdef";
	std::string ret;
	for (unsigned i = 0; i < 16; i++) {
		ret = hcs[n & 15] + ret;
		n >>= 4;
	}
	return ret;
}

std::string urldec(const std::string &s) {
	std::string ret;
	for (unsigned i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size()) {
			ret += hexdecode(s.substr(i+1, 2));
			i += 2;
		}
		else
			ret.push_back(s[i]);
	}
	return ret;
}

std::string cescape(std::string content, bool isxml) {
	std::string escaped;
	for (auto c: content)
		if (c == '"') escaped += "&quot;";
		else if (isxml && c == '<') escaped += "&lt;";
		else if (isxml && c == '>') escaped += "&gt;";
		else if (isxml && c == '&') escaped += "&amp;";
		else escaped += c;
	return escaped;
}


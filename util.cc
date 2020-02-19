

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

std::string base64Decode(const std::string & input) {
	if (input.length() % 4)
		return "";

	//Setup a vector to hold the result
	std::string ret;
	unsigned int temp = 0;
	for (unsigned cursor = 0; cursor < input.size(); ) {
		for (unsigned i = 0; i < 4; i++) {
			unsigned char c = *(unsigned char*)&input[cursor];
			temp <<= 6;
			if       (c >= 0x41 && c <= 0x5A)
				temp |= c - 0x41;
			else if  (c >= 0x61 && c <= 0x7A)
				temp |= c - 0x47;
			else if  (c >= 0x30 && c <= 0x39)
				temp |= c + 0x04;
			else if  (c == 0x2B)
				temp |= 0x3E;
			else if  (c == 0x2F)
				temp |= 0x3F;
			else if  (c == '=') {
				if (input.size() - cursor == 1) {
					ret.push_back((temp >> 16) & 0x000000FF);
					ret.push_back((temp >> 8 ) & 0x000000FF);
					return ret;
				}
				else if (input.size() - cursor == 2) {
					ret.push_back((temp >> 10) & 0x000000FF);
					return ret;
				}
			}
			cursor++;
		}
		ret.push_back((temp >> 16) & 0x000000FF);
		ret.push_back((temp >> 8 ) & 0x000000FF);
		ret.push_back((temp      ) & 0x000000FF);
	}
	return ret;
}

std::unordered_multimap<std::string, std::string> parse_vars(std::string body) {
	std::unordered_multimap<std::string, std::string> vars;
	size_t p = 0;
	while (1) {
		size_t pe = body.find('&', p);
		std::string curv = pe != std::string::npos ? body.substr(p, pe - p) : body.substr(p);
		size_t peq = curv.find('=');
		if (peq != std::string::npos)
			vars.emplace(urldec(curv.substr(0, peq)), urldec(curv.substr(peq+1)));
		if (pe == std::string::npos)
			break;
		p = pe + 1;
	}

	return vars;
}

std::pair<uint64_t, uint64_t> parse_range(std::string h) {
	// h is like bytes=X-Y (we ignore other formats)
	if (h.size() < 8 || h.substr(0, 6) != "bytes=")
		return {0, ~0ULL};

	h = h.substr(6);
	uint64_t startoff = atoll(h.c_str());

	auto p = h.find('-');
	if (p == std::string::npos)
		return {0, ~0ULL};

	uint64_t csize = atoll(&h[p+1]);
	if (h.size() <= p+1)
		csize = ~0ULL;

	return {startoff, csize};
}


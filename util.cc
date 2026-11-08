

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
	for (unsigned char c: content) {
		if (isxml) {
			if (c == '"') escaped += "&quot;";
			else if (c == '\'') escaped += "&apos;";
			else if (c == '<') escaped += "&lt;";
			else if (c == '>') escaped += "&gt;";
			else if (c == '&') escaped += "&amp;";
			// Most control chars cannot be represented in XML 1.0, not even escaped
			else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
			else escaped += c;
		}
		else {
			if (c == '"') escaped += "\\\"";
			else if (c == '\\') escaped += "\\\\";
			else if (c < 0x20) {
				const static char hcs[] = "0123456789abcdef";
				escaped += "\\u00";
				escaped += hcs[c >> 4];
				escaped += hcs[c & 15];
			}
			else escaped += c;
		}
	}
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

static bool parse_u64(const std::string &s, uint64_t *out) {
	uint64_t v = 0;
	for (char c : s) {
		if (c < '0' || c > '9' || v > (~0ULL - (c - '0')) / 10)
			return false;
		v = v * 10 + (c - '0');
	}
	*out = v;
	return !s.empty();
}

int parse_range(const std::string &h, uint64_t fsz, uint64_t *first, uint64_t *size) {
	// h is like bytes=X-Y, bytes=X- or bytes=-N (we ignore other formats)
	auto p = h.find('-');
	if (h.compare(0, 6, "bytes=") || p == std::string::npos)
		return 0;

	uint64_t a = 0, b = ~0ULL;
	if (p == 6) {
		if (!parse_u64(h.substr(7), &b))
			return 0;
		if (!b || !fsz)
			return -1;
		*size = b < fsz ? b : fsz;
		*first = fsz - *size;
		return 1;
	}

	if (!parse_u64(h.substr(6, p - 6), &a))
		return 0;
	if (p + 1 < h.size() && (!parse_u64(h.substr(p + 1), &b) || b < a))
		return 0;
	if (a >= fsz)
		return -1;
	*first = a;
	*size = (b < fsz - 1 ? b : fsz - 1) - a + 1;
	return 1;
}


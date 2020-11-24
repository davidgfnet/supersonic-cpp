
#ifndef __FCGI_HLPR__H__
#define __FCGI_HLPR__H__

#include <string>

// FastCGI responder helpers
// Provides a simple responder for head/body and some ready to use responses
// Overloading the class one can create an object that streams data out.

class fcgi_responder {
public:
	virtual ~fcgi_responder() {}
	virtual std::string header() = 0;
	virtual std::string respond() = 0;
};

class str_resp : public fcgi_responder {
public:
	str_resp(std::string h, std::string b) : head(h), body(b) {}
	virtual std::string header() {
		return head;
	}
	virtual std::string respond() {
		// Responds once!
		std::string r = body;
		body.clear();
		return r;
	}
private:
	std::string head, body;
};

static str_resp *respond_not_found() {
	return new str_resp(
		"Status: 404\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 13\r\n\r\n", "URI not found");
}

static str_resp *respond_method_not_allowed() {
	return new str_resp(
		"Status: 405\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 18\r\n\r\n", "Method not allowed");
}

#endif



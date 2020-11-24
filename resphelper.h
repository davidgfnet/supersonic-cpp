
#ifndef _RESP_HLPR__H__
#define _RESP_HLPR__H__

#include <string>
#include <iostream>
#include <list>
#include <unordered_map>

#include "fcgihelper.h"

static std::unordered_map<std::string, std::string> mimetypes = {{"mp3", "audio/mpeg"}, {"ogg", "audio/ogg"} };

enum fmtType { TYPE_XML, TYPE_JSON, TYPE_JSONP };
enum datType { DATA_STR, DATA_INT, DATA_BOOL, DATA_NULL };

// Defines a data field, with its associated type and value
struct DataField {
	datType type;
	std::string str;
	long long integer;
	bool boolean;

	bool null() const { return type == DATA_NULL; }

	// Serializes the content to XML or JSON
	std::string tostr(bool isxml) const {
		switch (type) {
		case DATA_STR:
			return "\"" + cescape(str, isxml) + "\"";
		case DATA_INT:
			if (isxml)
				return "\"" + std::to_string(integer) + "\"";
			return std::to_string(integer);
		case DATA_BOOL:
			if (isxml)
				return "\"" + std::string(boolean ? "true" : "false") + "\"";
			return boolean ? "true" : "false";
		};
		return {};
	}
};

#define DS(x) DataField{.type = DATA_STR, .str = (x), .integer = 0, .boolean = false}
#define DI(x) DataField{.type = DATA_INT, .str = "", .integer = (long long)(x), .boolean = false}
#define DB(x) DataField{.type = DATA_BOOL, .str = "", .integer = 0, .boolean = (x)}
#define DN()  DataField{.type = DATA_NULL, .str = "", .integer = 0, .boolean = false}

// Response format helper, that wraps responses and provides helpers such as MIME
class RespFmt {
public:
	RespFmt(std::string fmt_str, std::string extended) : extended(extended) {
		if (fmt_str == "json")
			fmt = TYPE_JSON;
		else if (fmt_str == "jsonp")
			fmt = TYPE_JSONP;
		else
			fmt = TYPE_XML;
	}
	bool isjson() const { return fmt != TYPE_XML; }
	std::string mime() const {
		const char *types[] = {
			"text/xml",
			"application/json",
			"application/javascript"
		};
		return types[fmt];
	}
	std::string wrap(std::string c) {
		switch (fmt) {
		case TYPE_JSON:
			return "{" + c + "}";
		case TYPE_JSONP:
			return extended + "({" + c + "});";
		default:
			return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + c;
		};
	}

	fmtType fmt;
	std::string extended;
};

class Entity {
public:

	typedef std::unordered_map<std::string, DataField> FieldMap;
	typedef std::unordered_map<std::string, std::list<Entity>> ContentMap;

	Entity(RespFmt rfmt, std::string name, FieldMap attrs, std::list<Entity> cvec = {})
	 : rfmt(rfmt), vrep(true), name(name), attrs(attrs) {
		for (auto c: cvec)
			content[c.name].push_back(c);
	}

	Entity(RespFmt rfmt, std::string name, FieldMap attrs, Entity e)
	 : rfmt(rfmt), vrep(false), name(name), attrs(attrs) {
		content[e.name].push_back(e);
	}

	std::string to_string() const {
		if (rfmt.isjson())
			return "\"" + name + "\": " + this->content_string();
		else
			return this->content_string();
	}

	std::string content_string() const {
		if (rfmt.isjson()) {
			std::string c;
			for (const auto it: attrs)
				if (!it.second.null())
					c += "\"" + it.first + "\": " + it.second.tostr(false) + ",\n";
			for (const auto it: content) {
				if (vrep) {
					c += "\"" + cescape(it.first) + "\": [\n";
					for (const auto e: it.second)
						c += e.content_string() + ",\n";
					c = c.substr(0, c.size()-2);
					c += "],\n";
				} else {
					c += it.second.front().to_string() + "\n";
				}
			}
			if (attrs.size() || content.size())
				c = c.substr(0, c.size()-2);
			return "{\n" + c + "}\n";
		}else{
			std::string a, c;
			for (const auto it: attrs)
				if (!it.second.null())
					a += " " + it.first + "=" + it.second.tostr(true) + "";
			for (const auto it: content)
				for (const auto e: it.second)
					c += e.to_string();
			return "<" + name + a + ">\n" + c + "</" + name + ">\n";
		}
	}
	str_resp* respond() {
		std::string rtype = rfmt.mime();
		std::string c = rfmt.wrap(this->to_string());
		return new str_resp("Status: 200\r\n"
			"Content-Type: " + rtype + "\r\n"
			"Content-Length: " + std::to_string(c.size()) + "\r\n\r\n", c);
	}


	static Entity wrap(Entity e) {
		return Entity(e.rfmt, "subsonic-response",
		              {{"status", DS("ok")}, {"version", DS("1.9.0")}}, e);
	}
	static Entity wrap(RespFmt fmt) {
		return Entity(fmt, "subsonic-response",
		              {{"status", DS("ok")}, {"version", DS("1.9.0")}});
	}
	static Entity error(RespFmt fmt, unsigned code, std::string content) {
		return Entity(fmt, "subsonic-response",
		              {{"status", DS("failed")}, {"version", DS("1.9.0")}},
		              Entity(fmt, "error", {{"code", DI(code)}, {"message", DS(content)}}));
	}

	RespFmt rfmt;
	bool vrep;
	std::string name;
	FieldMap attrs;
	ContentMap content;
};


#endif


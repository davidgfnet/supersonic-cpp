
#define _FILE_OFFSET_BITS 64
#include <cstdlib>
#include <string>
#include <iostream>
#include <list>
#include <vector>
#include <thread>
#include <memory>
#include <unordered_map>
#include <map>
#include <sqlite3.h>
#include <fcgio.h>
#include <unistd.h>
#include <signal.h>

#include "util.h"
#include "queue.h"
#include "datamodel.h"

using namespace std;

typedef std::unordered_map<std::string, std::string> StrMap;
map<string, string> mimetypes = { {"mp3", "audio/mpeg"}, {"ogg", "audio/ogg"} };
enum fmtType { TYPE_XML, TYPE_JSON, TYPE_JSONP };
enum datType { DATA_STR, DATA_INT, DATA_BOOL, DATA_NULL };

struct web_req {
	uint64_t offset, lastbyte;
	std::string method, host, uri;
	StrMap vars;
};

struct DataField {
	datType type;
	std::string str;
	long long integer;
	bool boolean;

	bool null() const { return type == DATA_NULL; }
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

class fcgi_responder {
public:
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

str_resp *respond_not_found() {
	return new str_resp(
		"Status: 404\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 13\r\n\r\n", "URI not found");
}

str_resp *respond_method_not_allowed() {
	return new str_resp(
		"Status: 405\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 18\r\n\r\n", "Method not allowed");
}

static std::unordered_map<std::string, std::string> parse_vars(std::string body) {
	std::unordered_map<std::string, std::string> vars;
	size_t p = 0;
	while (1) {
		size_t pe = body.find('&', p);
		std::string curv = pe != std::string::npos ? body.substr(p, pe - p) : body.substr(p);
		size_t peq = curv.find('=');
		if (peq != std::string::npos)
			vars[urldec(curv.substr(0, peq))] = urldec(curv.substr(peq+1));
		if (pe == std::string::npos)
			break;
		p = pe + 1;
	}

	return vars;
}

static std::pair<uint64_t, uint64_t> parse_range(std::string h) {
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

class Entity {
public:
	Entity(RespFmt rfmt, string name, map<string, DataField> attrs, list<Entity> cvec = {})
	 : rfmt(rfmt), vrep(true), name(name), attrs(attrs) {
		for (auto c: cvec)
			content[c.name].push_back(c);
	}

	Entity(RespFmt rfmt, string name, map<string, DataField> attrs, Entity e)
	 : rfmt(rfmt), vrep(false), name(name), attrs(attrs) {
		content[e.name].push_back(e);
	}

	string to_string() const {
		if (rfmt.isjson())
			return "\"" + name + "\": " + this->content_string();
		else
			return this->content_string();
	}

	string content_string() const {
		if (rfmt.isjson()) {
			string c;
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
			string a, c;
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
	static Entity error(RespFmt fmt, unsigned code, string content) {
		return Entity(fmt, "subsonic-response",
		              {{"status", DS("failed")}, {"version", DS("1.9.0")}},
		              Entity(fmt, "error", {{"code", DI(code)}, {"message", DS(content)}}));
	}

	RespFmt rfmt;
	bool vrep;
	string name;
	map<string, DataField> attrs;
	map<string, list<Entity>> content;
};

static uint64_t fsize(FILE *fd) {
	fseeko(fd, 0, SEEK_END);
	uint64_t r = ftello(fd);
	fseeko(fd, 0, SEEK_SET);
	return r;
}

class SupersonicServer {
private:
	// Datamodel
	DataModel *model;

	// Thread to spawn
	std::thread cthread;

	// Shared queue
	ConcurrentQueue<std::unique_ptr<FCGX_Request>> *rq;

	// Signal end of workers
	bool end;

	class stream_responder : public fcgi_responder {
	public:
		stream_responder(FILE* f, uint64_t offset = 0, uint64_t size = ~0ULL)
		  : f(f), offset(offset), ret_size(size) {
			// Seek to offset
			fseeko(f, offset, SEEK_SET);
		}
		~stream_responder() { if (f) fclose(f); }

		virtual std::string header() {
			if (ret_size != ~0ULL || offset > 0)
				return "Status: 206\r\n"
					"Content-Type: application/octet-stream\r\n"
					"Content-Range: bytes " +
						std::to_string(offset) + "-" + std::to_string(offset + ret_size - 1) + "/*\r\n"
					"Content-Length: " + std::to_string(ret_size) + "\r\n\r\n";
			else
				return "Status: 200\r\n"
					"Content-Type: application/octet-stream\r\n"
					"Content-Length: " + std::to_string(ret_size) + "\r\n\r\n";
		}

		virtual std::string respond() {
			// Send "small" chunks as response, so we can easily abort if needed.
			const size_t blocksize = 64*1024;
			size_t toread = ret_size < blocksize ? ret_size : blocksize;
			if (!toread)
				return {};   // EOF

			char tmpbuf[blocksize];
			size_t read = fread(tmpbuf, 1, toread, f);
			if (read >= 0) {
				ret_size -= read;
				return std::string(tmpbuf, read);
			}
			return {};
		}

	private:
		FILE* f;
		uint64_t offset, ret_size;
	};

	bool checkCredentials(web_req& req) {
		string user = req.vars["u"];
		if (user.empty())
			return false;

		if (!req.vars["p"].empty()) {
			string pass = req.vars["p"];
			if (pass.substr(0, 4) == "enc:")
				pass = hexdecode(pass.substr(4));
			return model->checkCredentials(user, pass);
		}
		if (!req.vars["s"].empty() && !req.vars["t"].empty()) {
			return model->checkCredentialsMD5(user, req.vars["t"], req.vars["s"]);
		}
		return false;
	}

	str_resp* authErr(web_req & req) {
		RespFmt fmt(req.vars["f"], req.vars["callback"]);
		return Entity::error(fmt, 40, "Wrong username or password").respond();
	}

	// Returns Entities, album Name and song count
	std::list<Entity> listSongs(RespFmt fmt, std::string node, const Album *alb) {
		auto songs = model->getSongsByAlbum(alb->id);
		std::list<Entity> esongs;
		for (auto song: songs) {
			esongs.push_back(Entity(fmt, node, {
				{"id",       DS(song.sid()) },
				{"title",    DS(song.title) },
				{"parent",   DS(song.salbumid()) },
				{"album",    DS(song.album) },
				{"artist",   DS(song.artist) },
				{"track",    DI(song.trackn) },
				{"genre",    DS(song.genre) },
				{"duration", DI(song.duration) },
				{"year",     DI(song.year) },
				{"discNumber", DI(song.discn) },
				{"bitRate",  DI(song.bitRate) },
				{"suffix",   DS(song.type) },
				{"contentType", DS(mimetypes[song.type]) },
				{"isDir",    DB(false) },
				{"coverArt", alb->hascover ? DS(alb->sid()) : DN() }
			}));
		}
		return esongs;
	}

	fcgi_responder* handle(web_req& req, FCGX_Request *fastcgi_req) {
		if (req.method != "HEAD" && req.method != "GET" && req.method != "POST")
			return respond_method_not_allowed();

		// Check for auth user and kick out intruders
		if (!checkCredentials(req))
			return authErr(req);

		RespFmt rfmt(req.vars["f"], req.vars["callback"]);
		uint64_t reqid = hexdecode64(req.vars["id"]);

		if (req.uri == "/rest/getMusicDirectory.view") {
			list<Entity> entities;
			string tname;
			switch (model->classifyId(reqid)) {
			case TYPE_ALBUM: {
				Album alb = model->getAlbum(reqid);
				entities = listSongs(rfmt, "child", &alb);
				tname = alb.title;
				} break;
			case TYPE_ARTIST: {
				auto albums = model->getAlbumsByArtist(reqid);
				for (auto album: albums) {
					entities.push_back(Entity(rfmt, "child", {
						{"id",       DS(album.sid()) },
						{"title",    DS(album.title) },
						{"artist",   DS(album.artist) },
						{"parent",   DS(album.sartistid()) },
						{"isDir",    DB(true) },
						{"coverArt", album.hascover ? DS(album.sid()) : DN() }
					}));
					tname = album.artist;
				}
				} break;
			};

			return Entity::wrap(Entity(rfmt, "directory", {
			                    {"id", DS(req.vars["id"])},
			                    {"name", DS(tname)}},
			                    entities)).respond();
		}
		else if (req.uri == "/rest/getAlbumList.view" or req.uri == "/rest/getAlbumList2.view") {
			unsigned offset = 0, size = 50;
			try {
				offset = stoul(req.vars["offset"]);
			} catch (...) {}
			try {
				size   = stoul(req.vars["size"]);
			} catch (...) {}

			list<Entity> ealbums;
			auto albums = model->getAllAlbumsSorted(offset, size);
			for (auto album: albums) {
				ealbums.push_back(Entity(rfmt, "album", {
					{"id",       DS(album.sid())},
					{"title",    DS(album.title)},
					{"artist",   DS(album.artist)},
					{"parent",   DS(album.sartistid())},
					{"isDir",    DB(true)},
					{"coverArt", album.hascover ? DS(album.sid()) : DN() }
				}));
			}

			string tag = (req.uri == "/rest/getAlbumList2.view") ? "albumList2" : "albumList";
			return Entity::wrap(Entity(rfmt, tag, {}, ealbums)).respond();
		}

		else if (req.uri == "/rest/getAlbum.view") {
			Album alb = model->getAlbum(reqid);
			auto songs = listSongs(rfmt, "song", &alb);
			return Entity::wrap(Entity(rfmt, "album", {
			                    {"id",        DS(req.vars["id"])},
			                    {"name",      DS(alb.title)},
			                    {"songCount", DI(songs.size())},
			                    {"coverArt",  DS(req.vars["id"])}},
			                    songs)).respond();
		}

		else if (req.uri == "/rest/getRandomSongs.view") {
			unsigned size = 10;
			try {
				size = stoul(req.vars["size"]);
			} catch (...) {}

			auto songs = model->getRandomSongs(size);
			list<Entity> esongs;
			for (auto song: songs) {
				esongs.push_back(Entity(rfmt, "song", {
					{"id",       DS(song.sid()) },
					{"title",    DS(song.title) },
					{"parent",   DS(song.salbumid()) },
					{"album",    DS(song.album) },
					{"artist",   DS(song.artist) },
					{"track",    DI(song.trackn) },
					{"genre",    DS(song.genre) },
					{"duration", DI(song.duration) },
					{"year",     DI(song.year) },
					{"discNumber", DI(song.discn) },
					{"bitRate",  DI(song.bitRate) },
					{"suffix",   DS(song.type) },
					{"contentType", DS(mimetypes[song.type]) },
					{"isDir",    DB(false) },
					{"coverArt", DS(song.salbumid()) },
				}));
			}

			return Entity::wrap(Entity(rfmt, "randomSongs", {}, esongs)).respond();
		}

		else if (req.uri == "/rest/getIndexes.view") {
			list<Entity> eartists;
			for (auto artist: model->getArtists())
				eartists.push_back(Entity(rfmt, "artist", 
					{ {"id", DS(artist.sid())}, {"name", DS(artist.name)} }));

			return Entity::wrap(Entity(rfmt, "indexes", {
			                    {"lastModified", DI(1455843830000)},  // FIXME: Unix timestamp * 1000
			                    {"ignoredArticles", DS("The El La Los Las Le Les")}},
			                    list<Entity>{
			                    Entity(rfmt, "index", {{"name", DS("Music")}}, eartists)})).respond();
		}
		else if (req.uri == "/rest/getPlaylists.view") {
			return Entity::wrap(Entity(rfmt, "playlists", {}, {})).respond();
		}
		else if (req.uri == "/rest/getGenres.view") {
			return Entity::wrap(Entity(rfmt, "genres", {}, {})).respond();
		}
		else if (req.uri == "/rest/getPodcasts.view") {
			return Entity::wrap(Entity(rfmt, "podcasts", {}, {})).respond();
		}
		else if (req.uri == "/rest/stream.view") {
			// Lookup song_id and get a file name!
			FILE * fd = model->getSongFile(reqid);
			if (fd) {
				  // Easier to count this way, prevent overflow
				req.lastbyte = std::max(req.lastbyte, req.lastbyte + 1);
				uint64_t fsz = fsize(fd);
				if (req.lastbyte > fsz)
					req.lastbyte = fsz;

				if (req.lastbyte > req.offset)
					return new stream_responder(fd, req.offset, req.lastbyte - req.offset);
				fclose(fd);
			}
		}

		// Misc stuff, needs to be there just to make clients happy :)
		else if (req.uri == "/rest/getMusicFolders.view") {
			return Entity::wrap(Entity(rfmt, "musicFolders", {}, list<Entity>{
			                    Entity(rfmt, "musicFolder", {
			                            {"id", DS("1")},
			                            {"name", DS("Music")},
			                    })})).respond();
		}
		else if (req.uri == "/rest/getLicense.view") {
			return Entity::wrap(Entity(rfmt, "license", {
			                    {"valid", DB(true)},
			                    {"email", DS("example@example.com")},
			                    {"key",   DS("ABC123DEF")}})).respond();
		}
		else if (req.uri == "/rest/ping.view") {
			return Entity::wrap(rfmt).respond();
		}
		else if (req.uri == "/rest/getUser.view") {
			return Entity::wrap(Entity(rfmt, "user", {
			                    {"username",     DS("admin")},
			                    {"email",        DS("admin@example.com")},
			                    {"scrobblingEnabled", DB(true)},
			                    {"adminRole",    DB(true)},
			                    {"settingsRole", DB(true)},
			                    {"downloadRole", DB(true)},
			                    {"uploadRole",   DB(false)},
			                    {"playlistRole", DB(true)},
			                    {"coverArtRole", DB(true)},
			                    {"commentRole",  DB(false)},
			                    {"podcastRole",  DB(false)},
			                    {"streamRole",   DB(false)},
			                    {"jukeboxRole",  DB(false)},
			                    {"shareRole",    DB(false)},
			                    })).respond();
		}
		else if (req.uri == "/rest/getCoverArt.view") {
			auto albumid = reqid;
			if (model->classifyId(albumid) == TYPE_SONG) {
				auto song = model->getSong(albumid);
				if (song)
					albumid = song->albumid;
			}
			std::string img = model->getAlbumCover(albumid, req.vars["size"]);
			return new str_resp("Status: 200\r\n"
				"Content-Type: image/jpeg\r\n"
				"Content-Length: " + std::to_string(img.size()) + "\r\n\r\n", img);
		}


		return respond_not_found();
	}

public:
	SupersonicServer(DataModel *dbm, ConcurrentQueue<std::unique_ptr<FCGX_Request>> *rq)
	: model(dbm), rq(rq) {
		cthread = std::thread(&SupersonicServer::work, this);
	}

	~SupersonicServer() {
		cthread.join();
	}

	// Receives requests and processes them by replying via a side http call.
	void work() {
		std::unique_ptr<FCGX_Request> req;
		while (rq->pop(&req)) {
			// Get streams to write
			fcgi_streambuf reqout(req->out);
			std::iostream obuf(&reqout);

			// Parse inputs
			web_req wreq;
			wreq.method   = FCGX_GetParam("REQUEST_METHOD", req->envp) ?: "";
			wreq.uri      = FCGX_GetParam("DOCUMENT_URI", req->envp) ?: "";
			wreq.vars     = parse_vars(FCGX_GetParam("QUERY_STRING", req->envp) ?: "");
			wreq.host     = FCGX_GetParam("HTTP_HOST", req->envp) ?: "";
			std::tie(wreq.offset, wreq.lastbyte) = parse_range(FCGX_GetParam("HTTP_RANGE", req->envp) ?: "");

			std::unique_ptr<fcgi_responder> resp(this->handle(wreq, req.get()));

			// Respond with an immediate update JSON encoded too
			obuf << resp->header();  // Send header
			while (wreq.method != "HEAD") {
				std::string r = resp->respond();
				// Stop if EOF or there was a write error (pipe broken most likely)
				if (r.empty() || FCGX_GetError(req->out))
					break;
				obuf << r;
			}

			FCGX_Finish_r(req.get());
			req.reset();
		}
	}
};

bool serving = true;
void sighandler(int) {
	std::cerr << "Signal caught" << std::endl;
	// Just tweak a couple of vars really
	serving = false;
	// Ask for CGI lib shutdown
	FCGX_ShutdownPending();
	// Close stdin so we stop accepting
	close(0);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " database [nthreads]" << std::endl;
		return 1;
	}

	// Initialize the database backend.
	sqlite3* sqldb;
	if (SQLITE_OK != sqlite3_open_v2(argv[1], &sqldb, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL)) {
		std::cerr << "Could not open sqlite3 database!" << std::endl;
		return 1;
	}

	// Start FastCGI interface
	FCGX_Init();

	// Signal handling
	signal(SIGINT, sighandler); 
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	// Start worker threads for this
	unsigned nthreads = (argc >= 3) ? (atoi(argv[2]) & 255) : 4;

	DataModel dbm(sqldb);
	ConcurrentQueue<std::unique_ptr<FCGX_Request>> reqqueue;
	SupersonicServer *workers[nthreads];
	for (unsigned i = 0; i < nthreads; i++)
		workers[i] = new SupersonicServer(&dbm, &reqqueue);

	std::cerr << "All workers up, serving until SIGINT/SIGTERM" << std::endl;

	// Now keep ingesting incoming requests, we do this in the main
	// thread since threads are much slower, unlikely to be a bottleneck.
	while (serving) {
		std::unique_ptr<FCGX_Request> request(new FCGX_Request());
		FCGX_InitRequest(request.get(), 0, 0);

		if (FCGX_Accept_r(request.get()) >= 0)
			// Get a worker that's free and queue it there
			reqqueue.push(std::move(request));
	}

	std::cerr << "Signal caught! Starting shutdown" << std::endl;
	reqqueue.close();

	// Just go ahead and delete workers
	for (unsigned i = 0; i < nthreads; i++)
		delete workers[i];

	std::cerr << "All clear, service is down" << std::endl;
	sqlite3_close(sqldb);
}



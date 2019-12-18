
#define _FILE_OFFSET_BITS 64
#include <cstdlib>
#include <string>
#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <sqlite3.h>
#include <fcgio.h>
#include <unistd.h>
#include <signal.h>

#include "queue.h"

using namespace std;

typedef std::unordered_map<std::string, std::string> StrMap;
enum classTypes { TYPE_ALBUM, TYPE_ARTIST, TYPE_ERROR };
map<string, string> mimetypes = { {"mp3", "audio/mpeg"}, {"ogg", "audio/ogg"} };

struct web_req {
	uint64_t offset, lastbyte;
	std::string method, host, uri;
	StrMap vars;
};

class fcgi_responder {
public:
	virtual std::string respond() = 0;
};

class str_resp : public fcgi_responder {
public:
	str_resp(std::string c) : content(c) {}
	virtual std::string respond() {
		// Responds once!
		std::string r = content;
		content.clear();
		return r;
	}
private:
	std::string content;
};

str_resp *respond_not_found() {
	return new str_resp(
		"Status: 404\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 13\r\n\r\nURI not found");
}

str_resp *respond_method_not_allowed() {
	return new str_resp(
		"Status: 405\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 18\r\n\r\nMethod not allowed");
}

static unsigned char hexdec(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 0;
}
static std::string hexdecode(std::string s) {
	if (s.size() & 1)
		return {};
	std::string ret;
	for (unsigned i = 0; i < s.size(); i += 2)
		ret.push_back((char)((hexdec(s[i]) << 4) | hexdec(s[i+1])));
	return ret;
}

static std::string urldec(const std::string &s) {
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
	Entity(bool isjson, string name, map<string, string> attrs, vector<Entity> cvec = {})
	 : isjson(isjson), vrep(true), name(name), attrs(attrs) {
		for (auto c: cvec)
			content[c.name].push_back(c);
	}

	Entity(bool isjson, string name, map<string, string> attrs, Entity e)
	 : isjson(isjson), vrep(false), name(name), attrs(attrs) {
		content[e.name].push_back(e);
	}

	string to_string() const {
		if (isjson)
			return "\"" + name + "\": " + this->content_string();
		else
			return this->content_string();
	}

	string content_string() const {
		if (isjson) {
			string c;
			for (const auto it: attrs)
				c += "\"" + it.first + "\": \"" + cescape(it.second) + "\",\n";
			for (const auto it: content) {
				if (vrep) {
					c += "\"" + cescape(it.first) + "\": [\n";
					for (const auto e: it.second)
						c += e.content_string() + ",\n";
					c = c.substr(0, c.size()-2);
					c += "],\n";
				} else {
					c += it.second[0].to_string() + "\n";
				}
			}
			if (attrs.size() || content.size())
				c = c.substr(0, c.size()-2);
			return "{\n" + c + "}\n";
		}else{
			string a, c;
			for (const auto it: attrs)
				a += " " + it.first + "=\"" + cescape(it.second, true) + "\"";
			for (const auto it: content)
				for (const auto e: it.second)
					c += e.to_string();
			return "<" + name + a + ">\n" + c + "</" + name + ">\n";
		}
	}
	str_resp* respond() {
		std::string rtype = isjson ? "application/json" : "text/xml";
		std::string c = this->to_string();
		if (!isjson)
			c = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + c;
		return new str_resp("Status: 200\r\n"
			"Content-Type: " + rtype + "\r\n"
			"Content-Length: " + std::to_string(c.size()) + "\r\n\r\n" + c);
	}

	static string cescape(string content, bool isxml = false) {
		string escaped;
		for (auto c: content)
			if (c == '"') escaped += "&quot;";
			else if (isxml && c == '<') escaped += "&lt;";
			else if (isxml && c == '>') escaped += "&gt;";
			else if (isxml && c == '&') escaped += "&amp;";
			else escaped += c;
		return escaped;
	}

	static Entity wrap(Entity e) {
		return Entity(e.isjson, "subsonic-response",
		              {{"status", "ok"}, {"version", "1.9.0"}}, e);
	}
	static Entity wrap(bool isjson) {
		return Entity(isjson, "subsonic-response",
		              {{"status", "ok"}, {"version", "1.9.0"}});
	}
	static Entity error(bool isjson, unsigned code, string content) {
		return Entity(isjson, "subsonic-response",
		              {{"status", "failed"}, {"version", "1.9.0"}},
		              Entity(isjson, "error", {{"code", std::to_string(code)}, {"message", content}}));
	}

	bool isjson, vrep;
	string name;
	map<string, string> attrs;
	map<string, vector<Entity>> content;
};

class Artist {
public:
	Artist(sqlite3_stmt * stmt) {
		id       = string((char*)sqlite3_column_text (stmt, 0));
		name     = string((char*)sqlite3_column_text (stmt, 1));
	}
	string id, name;
};

class Album {
public:
	Album() {}
	Album(sqlite3_stmt * stmt) {
		id       = string((char*)sqlite3_column_text (stmt, 0));
		title    = string((char*)sqlite3_column_text (stmt, 1));
		artistid = string((char*)sqlite3_column_text (stmt, 2));
		artist   = string((char*)sqlite3_column_text (stmt, 3));
		hascover = sqlite3_column_int(stmt, 4);
	}
	string id, title, artistid, artist;
	int hascover;
};

class Song {
public:
	string id, title, albumid, album, artistid, artist;
	unsigned trackn, duration, year, discn, bitRate;
	string genre, type;

	Song(sqlite3_stmt * stmt) {
		id       = string((char*)sqlite3_column_text (stmt, 0));
		title    = string((char*)sqlite3_column_text (stmt, 1));
		albumid  = string((char*)sqlite3_column_text (stmt, 2));
		album    = string((char*)sqlite3_column_text (stmt, 3));
		artistid = string((char*)sqlite3_column_text (stmt, 4));
		artist   = string((char*)sqlite3_column_text (stmt, 5));

		trackn   = sqlite3_column_int(stmt, 6);
		discn    = sqlite3_column_int(stmt, 7);
		year     = sqlite3_column_int(stmt, 8);
		duration = sqlite3_column_int(stmt, 9);
		bitRate  = sqlite3_column_int(stmt,10);

		genre    = string((char*)sqlite3_column_text (stmt,11));
		type     = string((char*)sqlite3_column_text (stmt,12));
	}
};

class DataModel {
public:
	DataModel(sqlite3* sqldb) : sqldb(sqldb) { }

	bool checkCredentials(string user, string pass) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT * FROM users WHERE username=? AND password=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, user.c_str(), -1, NULL);
		sqlite3_bind_text(stmt, 2, pass.c_str(), -1, NULL);
		bool res = (sqlite3_step(stmt) == SQLITE_ROW);
		sqlite3_finalize(stmt);
		return res;
	}

	bool checkCredentialsMD5(string user, string token, string salt) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT * FROM users WHERE username=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, user.c_str(), -1, NULL);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			// Query pass and get MD5, compare
			return false;
		}
		sqlite3_finalize(stmt);
		return false;
	}

	string getAlbumCover(string id, string ssize) {
		unsigned size = atoi(ssize.c_str());
		const std::vector<std::string> fields = {
			"cover128", "cover256", "cover512", "cover1024", "cover"
		};
		unsigned off = (size > 1024) ? 4:
		               (size >  512) ? 3:
		               (size >  256) ? 2:
		               (size >  128) ? 1:0;

		string ret;
		for (unsigned i = off; i < 5 && ret.empty(); i++) {
			sqlite3_stmt *stmt;
			sqlite3_prepare_v2(sqldb, ("SELECT " + fields[i] +
			                           " FROM albums WHERE id=?").c_str(), -1, &stmt, NULL);
			sqlite3_bind_text(stmt, 1, id.c_str(), -1, NULL);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				auto length = sqlite3_column_bytes(stmt, 0);
				ret = string((char*)sqlite3_column_blob(stmt, 0), length);
			}
			sqlite3_finalize(stmt);
		}
		return ret;
	}

	FILE * getSongFile(string id) {
		string filename;
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT filename FROM songs WHERE id=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, id.c_str(), -1, NULL);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			filename = (char*)sqlite3_column_text (stmt, 0);
		}
		sqlite3_finalize(stmt);

		return fopen(filename.c_str(), "rb");
	}

	vector<Album> getAllAlbumsSorted(unsigned offset, unsigned size) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, NULL FROM albums ORDER BY `title` COLLATE NOCASE ASC LIMIT ? OFFSET ?", -1, &stmt, NULL);
		sqlite3_bind_int64(stmt, 1, size);
		sqlite3_bind_int64(stmt, 2, offset);

		vector<Album> albums;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			albums.push_back(Album(stmt));
		sqlite3_finalize(stmt);

		return albums;
	}

	vector<Album> getAlbumsByArtist(string artistid) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, hascover FROM albums WHERE artistid=? ORDER BY `title` COLLATE NOCASE ASC", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, artistid.c_str(), -1, NULL);

		vector<Album> albums;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			albums.push_back(Album(stmt));
		sqlite3_finalize(stmt);

		return albums;
	}

	Album getAlbum(string id) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, hascover FROM albums WHERE `id`=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, id.c_str(), -1, NULL);

		Album ret;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ret = Album(stmt);
		sqlite3_finalize(stmt);

		return ret;
	}

	vector<Artist> getArtists() {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, `name` FROM artists ORDER BY `name` COLLATE NOCASE ASC", -1, &stmt, NULL);

		vector<Artist> artists;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			artists.push_back(Artist(stmt));
		sqlite3_finalize(stmt);

		return artists;
	}

	vector<Song> getSongsByAlbum(string id) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, albumid, album, artistid, artist,"
			"trackn, discn, year, duration, bitRate, genre, type FROM songs "
			"WHERE `albumid`=? ORDER BY trackn, discn ASC", -1, &stmt, NULL);

		sqlite3_bind_text(stmt, 1, id.c_str(), -1, NULL);

		vector<Song> songs;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			songs.push_back(Song(stmt));
		sqlite3_finalize(stmt);

		return songs;
	}

	vector<Song> getRandomSongs(unsigned limit) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, albumid, album, artistid, artist, "
			"trackn, discn, year, duration, bitRate, genre, type FROM songs "
			"LIMIT ? OFFSET ? % ((Select count(*) from songs) - ?)", -1, &stmt, NULL);

		int randoffset = (rand() ^ (rand() << 8) ^ (rand() << 16));
		sqlite3_bind_int64(stmt, 1, limit);
		sqlite3_bind_int64(stmt, 2, randoffset);
		sqlite3_bind_int64(stmt, 3, limit-1);

		vector<Song> songs;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			songs.push_back(Song(stmt));
		sqlite3_finalize(stmt);

		random_shuffle(songs.begin(), songs.end());

		return songs;
	}

	classTypes classifyId(string id) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT 0 FROM `albums` WHERE `id`=? UNION SELECT 1 FROM `artists` WHERE `id`=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, id.c_str(), -1, NULL);
		sqlite3_bind_text(stmt, 2, id.c_str(), -1, NULL);

		classTypes ret = TYPE_ERROR;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ret = (classTypes)sqlite3_column_int (stmt, 0);
		sqlite3_finalize(stmt);

		return ret;
	}

private:
	sqlite3 * sqldb;
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
		  : f(f), offset(offset), ret_size(size), header(false) {
			// Seek to offset
			fseeko(f, offset, SEEK_SET);
		}
		~stream_responder() { if (f) fclose(f); }

		virtual std::string respond() {
			if (!header) {
				header = true;
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
		bool header;
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
		bool rjson = (req.vars["f"] == "json");
		return Entity::error(rjson, 40, "Wrong username or password").respond();
	}

	// Returns Entities, album Name and song count
	tuple<vector<Entity>, string, unsigned> listSongs(bool rjson,
		string node, string album_id) {

		Album alb = model->getAlbum(album_id);
		auto songs = model->getSongsByAlbum(album_id);
		vector<Entity> esongs;
		for (auto song: songs) {
			esongs.push_back(Entity(rjson, node, {
				{"id",       song.id },
				{"title",    song.title },
				{"parent",   song.albumid },
				{"album",    song.album },
				{"artist",   song.artist },
				{"track",    to_string(song.trackn) },
				{"genre",    song.genre },
				{"duration", to_string(song.duration) },
				{"year",     to_string(song.year) },
				{"discNumber", to_string(song.discn) },
				{"bitRate",  to_string(song.bitRate) },
				{"suffix",   song.type },
				{"contentType", mimetypes[song.type] },
				{"isDir",   "false" },
				{"coverArt", (alb.hascover ? album_id : "") }
			}));
		}
		return tuple<vector<Entity>, string, unsigned>(esongs, alb.title, songs.size());
	}

	fcgi_responder* handle(web_req& req, FCGX_Request *fastcgi_req) {
		if (req.method != "HEAD" && req.method != "GET" && req.method != "POST")
			return respond_method_not_allowed();

		// Check for auth user and kick out intruders
		if (!checkCredentials(req))
			return authErr(req);

		bool rjson = (req.vars["f"] == "json");

		if (req.uri == "/rest/getMusicDirectory.view") {
			vector<Entity> entities;
			string tname;
			switch (model->classifyId(req.vars["id"])) {
			case TYPE_ALBUM: {
				auto songs = listSongs(rjson, "child", req.vars["id"]);
				entities = get<0>(songs);
				tname = get<1>(songs);
				} break;
			case TYPE_ARTIST: {
				auto albums = model->getAlbumsByArtist(req.vars["id"]);
				for (auto album: albums) {
					entities.push_back(Entity(rjson, "child", {
						{"id",       album.id },
						{"title",    album.title },
						{"artist",   album.artist },
						{"parent",   album.artistid },
						{"isDir",   "true" },
						{"coverArt", (album.hascover ? album.id : "") }
					}));
					tname = album.artist;
				}
				} break;
			};

			return Entity::wrap(Entity(rjson, "directory", {
			                    {"id", req.vars["id"]},
			                    {"name", tname}},
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

			vector<Entity> ealbums;
			auto albums = model->getAllAlbumsSorted(offset, size);
			for (auto album: albums) {
				ealbums.push_back(Entity(rjson, "album", {
					{"id",       album.id},
					{"title",    album.title},
					{"artist",   album.artist},
					{"parent",   album.artistid},
					{"isDir",   "true"},
					{"coverArt", album.hascover ? album.id : ""}
				}));
			}

			string tag = (req.uri == "/rest/getAlbumList2.view") ? "albumList2" : "albumList";
			return Entity::wrap(Entity(rjson, tag, {}, ealbums)).respond();
		}

		else if (req.uri == "/rest/getAlbum.view") {
			auto songs = listSongs(rjson, "song", req.vars["id"]);
			return Entity::wrap(Entity(rjson, "album", {
			                    {"id",        req.vars["id"]},
			                    {"name",      get<1>(songs)},
			                    {"songCount", to_string(get<2>(songs))},
			                    {"coverArt",  req.vars["id"]}},
			                    get<0>(songs))).respond();
		}

		else if (req.uri == "/rest/getRandomSongs.view") {
			unsigned size = 10;
			try {
				size = stoul(req.vars["size"]);
			} catch (...) {}

			auto songs = model->getRandomSongs(size);
			vector<Entity> esongs;
			for (auto song: songs) {
				esongs.push_back(Entity(rjson, "song", {
					{"id",       song.id },
					{"title",    song.title },
					{"parent",   song.albumid },
					{"album",    song.album },
					{"artist",   song.artist },
					{"track",    to_string(song.trackn) },
					{"genre",    song.genre },
					{"duration", to_string(song.duration) },
					{"year",     to_string(song.year) },
					{"discNumber", to_string(song.discn) },
					{"bitRate",  to_string(song.bitRate) },
					{"suffix",   song.type },
					{"contentType", mimetypes[song.type] },
					{"isDir",   "false" },
					{"coverArt", song.albumid },
				}));
			}

			return Entity::wrap(Entity(rjson, "randomSongs", {}, esongs)).respond();
		}

		else if (req.uri == "/rest/getIndexes.view") {
			vector<Entity> eartists;
			for (auto artist: model->getArtists())
				eartists.push_back(Entity(rjson, "artist", 
					{ {"id", artist.id}, {"name", artist.name} }));

			return Entity::wrap(Entity(rjson, "indexes", {
			                    {"lastModified", "1455843830000"},  // FIXME: Unix timestamp * 1000
			                    {"ignoredArticles", "The El La Los Las Le Les"}},
			                    vector<Entity>{
			                    Entity(rjson, "index", {{"name", "Music"}}, eartists)})).respond();
		}

		else if (req.uri == "/rest/getCoverArt.view") {
			std::string img = model->getAlbumCover(req.vars["id"], req.vars["size"]);
			return new str_resp("Status: 200\r\n"
				"Content-Type: image/jpeg\r\n"
				"Content-Length: " + std::to_string(img.size()) + "\r\n\r\n" + img);
		}
		else if (req.uri == "/rest/stream.view") {
			// Lookup song_id and get a file name!
			string song_id = req.vars["id"];
			FILE * fd = model->getSongFile(song_id);
			if (fd) {
				  // Easier to count this way, prevent overflow
				req.lastbyte = std::max(req.lastbyte, req.lastbyte + 1);
				uint64_t fsz = fsize(fd);
	            //uint64_t max_filesize = fsize(fd) - req.offset;
	            if (req.lastbyte > fsz)
					req.lastbyte = fsz;

				if (req.lastbyte > req.offset)
					return new stream_responder(fd, req.offset, req.lastbyte - req.offset);
			}
		}

		// Misc stuff, needs to be there just to make clients happy :)
		else if (req.uri == "/rest/getMusicFolders.view") {
			return Entity::wrap(Entity(rjson, "musicFolders", {}, vector<Entity>{
			                    Entity(rjson, "musicFolder", {
			                            {"id", "1"},
			                            {"name", "Music"},
			                    })})).respond();
		}
		else if (req.uri == "/rest/getLicense.view") {
			return Entity::wrap(Entity(rjson, "license", {
			                    {"valid", "true"},
			                    {"email", "example@example.com"},
			                    {"key",   "ABC123DEF"}})).respond();
		}
		else if (req.uri == "/rest/ping.view") {
			return Entity::wrap(rjson).respond();
		}
		else if (req.uri == "/rest/getUser.view") {
			return Entity::wrap(Entity(rjson, "user", {
			                    {"username",     "admin"},
			                    {"email",        "admin@example.com"},
			                    {"scrobblingEnabled", "true"},
			                    {"adminRole",    "true"},
			                    {"settingsRole", "true"},
			                    {"downloadRole", "true"},
			                    {"uploadRole",   "false"},
			                    {"playlistRole", "true"},
			                    {"coverArtRole", "true"},
			                    {"commentRole",  "false"},
			                    {"podcastRole",  "false"},
			                    {"streamRole",   "false"},
			                    {"jukeboxRole",  "false"},
			                    {"shareRole",    "false"},
			                    })).respond();
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
			while (1) {
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



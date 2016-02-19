
#include <cstdlib>
#include <string>
#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <map>
#include <sqlite3.h>

#include <cxxhttpsrv/microrestd.h>

using namespace std;
using namespace cxxhttpsrv;

enum classTypes { TYPE_ALBUM, TYPE_ARTIST, TYPE_ERROR };
map<string, string> mimetypes = { {"mp3", "audio/mpeg"}, {"ogg", "audio/ogg"} };

void panic_if(bool cond, string text) {
	if (cond) {
		cerr << text << endl;
		exit(1);
	}
}

string XN(string tag, string content) {
	return "<" + tag + ">\n" + content + "</" + tag + ">\n";
}

string XN(string tag, map<string, string> attrs, string content = "") {
	string att;
	for (auto it: attrs)
		if (it.second.size())
			att += it.first + "=\"" + it.second + "\" ";
	att = "<" + tag + " " + att;
	if (content.size())
		return att + ">\n" + content + "</" + tag + ">\n";
	else
		return att + " />\n";
}

bool XML(cxxhttpsrv::rest_request& req, string content) {
	string escaped;
	for (auto c: content)
		if (c == '&')
			escaped += "&amp;";
		else
			escaped += c;

	return req.respond("text/xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<subsonic-response status=\"ok\" version=\"1.4.0\">\n" + escaped + "\n</subsonic-response>");
}

bool XMLERR(cxxhttpsrv::rest_request &req, unsigned code, string content) {
	return req.respond("text/xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<subsonic-response status=\"failed\" version=\"1.4.0\">\n"
		"<error code=\"" + to_string(code) + "\" message=\"" + content + "\"/>"
		"\n</subsonic-response>");
}

class Artist {
public:
	Artist(sqlite3_stmt * stmt) {
		id       = std::string((char*)sqlite3_column_text (stmt, 0));
		name     = std::string((char*)sqlite3_column_text (stmt, 1));
	}
	string id, name;
};

class Album {
public:
	Album() {}
	Album(sqlite3_stmt * stmt) {
		id       = std::string((char*)sqlite3_column_text (stmt, 0));
		title    = std::string((char*)sqlite3_column_text (stmt, 1));
		artistid = std::string((char*)sqlite3_column_text (stmt, 2));
		artist   = std::string((char*)sqlite3_column_text (stmt, 3));
		cover    = std::string((char*)sqlite3_column_blob (stmt, 4), sqlite3_column_bytes(stmt, 4));
	}
	string id, title, artistid, artist, cover;
};

class Song {
public:
	string id, title, albumid, album, artistid, artist;
	unsigned trackn, duration, year, discn, bitRate;
	string genre, type;

	Song(sqlite3_stmt * stmt) {
		id       = std::string((char*)sqlite3_column_text (stmt, 0));
		title    = std::string((char*)sqlite3_column_text (stmt, 1));
		albumid  = std::string((char*)sqlite3_column_text (stmt, 2));
		album    = std::string((char*)sqlite3_column_text (stmt, 3));
		artistid = std::string((char*)sqlite3_column_text (stmt, 4));
		artist   = std::string((char*)sqlite3_column_text (stmt, 5));

		trackn   = sqlite3_column_int(stmt, 6);
		discn    = sqlite3_column_int(stmt, 7);
		year     = sqlite3_column_int(stmt, 8);
		duration = sqlite3_column_int(stmt, 9);
		bitRate  = sqlite3_column_int(stmt,10);

		genre    = std::string((char*)sqlite3_column_text (stmt,11));
		type     = std::string((char*)sqlite3_column_text (stmt,12));
	}
};

class DataModel {
public:
	DataModel(string database_path) {
		int ok = sqlite3_open_v2(
			database_path.c_str(),
			&sqldb,
			SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
			NULL
		);
		panic_if(ok != SQLITE_OK, "Could not open sqlite3 database!");
	}

	~DataModel() {
		if (sqldb)
			sqlite3_close(sqldb);
	}

	bool open(const char *sdb) {
		sqlite3_open(sdb, &sqldb);
	}

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
			return true;
		}
		sqlite3_finalize(stmt);
		return false;
	}

	string getAlbumCover(string id, string size) {
		string ret;
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT cover FROM albums WHERE id=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, id.c_str(), -1, NULL);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			ret = (char*)sqlite3_column_text (stmt, 0);
		}
		sqlite3_finalize(stmt);
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
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, cover FROM albums LIMIT ? OFFSET ? ORDER BY `title` ASC", -1, &stmt, NULL);
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
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, cover FROM albums WHERE artistid=? ORDER BY `title` ASC", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, artistid.c_str(), -1, NULL);

		vector<Album> albums;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			albums.push_back(Album(stmt));
		sqlite3_finalize(stmt);

		return albums;
	}

	Album getAlbum(string id) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, cover FROM albums WHERE `id`=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, id.c_str(), -1, NULL);

		Album ret;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ret = Album(stmt);
		sqlite3_finalize(stmt);

		return ret;
	}

	vector<Artist> getArtists() {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, `name` FROM artists ORDER BY `name` ASC", -1, &stmt, NULL);

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

static string dehexify(string in) {
	string out;
	for (unsigned i = 0; i < in.size()/2; i++) {
		unsigned char hi = 0, lo = 0;
		if (in[2*i] >= '0' and in[2*i] <= '9')
			hi = in[2*i] - '0';
		else if (in[2*i] >= 'a' and in[2*i] <= 'f')
			hi = in[2*i] - 'a' + 10;
		else if (in[2*i] >= 'A' and in[2*i] <= 'F')
			hi = in[2*i] - 'A' + 10;

		if (in[2*i+1] >= '0' and in[2*i+1] <= '9')
			lo = in[2*i+1] - '0';
		else if (in[2*i+1] >= 'a' and in[2*i+1] <= 'f')
			lo = in[2*i+1] - 'a' + 10;
		else if (in[2*i+1] >= 'A' and in[2*i+1] <= 'F')
			lo = in[2*i+1] - 'A' + 10;

		out += (char)((hi << 4) | lo);
	}
	return out;
}

class file_service : public rest_service {
	class file_generator : public response_generator {
	public:
		file_generator(FILE* f) : f(f) {}
		~file_generator() { if (f) fclose(f); }

		virtual bool generate() override {
			size_t data_size = data.size();
			data.resize(data_size + 1024);
			size_t read = fread(data.data() + data_size, 1, 1024, f);
			data.resize(data_size + read);

			return read;
		}
		virtual string_piece current() const override {
			return string_piece(data.data(), data.size());
		}
		virtual void consume(size_t length) override {
			if (length >= data.size()) data.clear();
			else if (length) data.erase(data.begin(), data.begin() + length);
		}

	private:
		FILE* f;
		vector<char> data;
	};

	bool checkCredentials(rest_request& req) {
		string user = req.params["u"];
		if (user.empty())
			return false;

		if (!req.params["p"].empty()) {
			string pass = req.params["p"];
			if (pass.substr(0, 4) == "enc:")
				pass = dehexify(pass.substr(4));
			return model.checkCredentials(user, pass);
		}
		if (!req.params["s"].empty() && !req.params["t"].empty()) {
			return model.checkCredentialsMD5(user, req.params["t"], req.params["s"]);
		}
		return false;
	}

	bool authErr(rest_request & req) {
		return XMLERR(req, 40, "Wrong username or password");
	}

	DataModel model;

	// Returns XML, album Name and song count
	tuple<string, string, unsigned> listSongs(string node, string album_id) {
		string ret;
		Album alb = model.getAlbum(album_id);
		auto songs = model.getSongsByAlbum(album_id);
		for (auto song: songs) {
			ret += XN(node, {
				{"id",       song.id },
				{"title",    song.title },
				{"parent",   song.albumid },
				{"album",    song.album },
				{"artist",   song.artist },
				{"track",    to_string(song.trackn) },
				{"genre",    song.genre },
				{"duration", to_string(song.duration/1000) },
				{"year",     to_string(song.year) },
				{"discNumber", to_string(song.discn) },
				{"bitRate",  to_string(song.bitRate) },
				{"suffix",   song.type },
				{"contentType", mimetypes[song.type] },
				{"isDir",   "false" },
				{"coverArt", (alb.cover.size() ? album_id : "") }
			});
		}
		return tuple<string, string, unsigned>(ret, alb.title, songs.size());
	}

	public:
	virtual bool handle(rest_request& req) override {
		if (req.method != "HEAD" && req.method != "GET" && req.method != "POST") return req.respond_method_not_allowed("HEAD, GET, POST");

		// Check for auth user and kick out intruders
		if (!checkCredentials(req))
			return authErr(req);

		if (req.url == "/rest/getMusicDirectory.view") {
			string ret, tname;
			switch (model.classifyId(req.params["id"])) {
			case TYPE_ALBUM: {
				auto songs = listSongs("child", req.params["id"]);
				ret = get<0>(songs);
				tname = get<1>(songs);
				} break;
			case TYPE_ARTIST: {
				auto albums = model.getAlbumsByArtist(req.params["id"]);
				for (auto album: albums) {
					ret += XN("child", {
						{"id",       album.id },
						{"title",    album.title },
						{"artist",   album.artist },
						{"parent",   album.artistid },
						{"isDir",   "true" },
						{"coverArt", (album.cover.size() ? album.id : "") }
					});
					tname = album.artist;
				}
				} break;
			};

			return XML(req, 
				XN("directory", {
					{"id",   req.params["id"]},
					{"name", tname},
				}, ret)
			);
		}
		else if (req.url == "/rest/getAlbumList.view" or req.url == "/rest/getAlbumList2.view") {
			unsigned offset = 0, size = 50;
			try {
				offset = stoul(req.params["offset"]);
				size   = stoul(req.params["size"]);
			} catch (...) {}

			string ret;
			auto albums = model.getAllAlbumsSorted(offset, size);
			for (auto album: albums) {
				ret += XN("album", {
					{"id",       album.id},
					{"title",    album.title},
					{"artist",   album.artist},
					{"parent",   album.artistid},
					{"isDir",   "true"},
					{"coverArt", album.cover}
				});
			}

			return XML(req,
				XN("albumList", ret)
			);
		}

		else if (req.url == "/rest/getAlbum.view") {
			auto songs = listSongs("child", req.params["id"]);
			return XML(req,
				XN("album", {
					{"id",        req.params["id"]},
					{"name",      get<1>(songs)},
					{"songCount", to_string(get<2>(songs))},
					{"coverArt",  "FIXME"},
				},
				get<0>(songs))
			);
		}

		else if (req.url == "/rest/getRandomSongs.view") {
			unsigned size = 10;
			try {
				size = stoul(req.params["size"]);
			} catch (...) {}

			string ret;
			auto songs = model.getRandomSongs(size);
			for (auto song: songs) {
				ret += XN("song", {
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
					//{"coverArt", (alb.cover.size() ? album_id : "") }
				});
			}

			return XML(req,
				XN("randomSongs", ret)
			);
		}

		else if (req.url == "/rest/getIndexes.view") {
			auto artists = model.getArtists();

			string ret;
			for (auto artist: artists)
				ret += XN("artist", { {"id", artist.id}, {"name", artist.name} });

			return XML(req,
				XN("indexes", {
					{"lastModified", "1455843830000"},  // FIXME: Unix timestamp * 1000
					{"ignoredArticles", "The El La Los Las Le Les"},
				},
				ret)
			);
		}

		else if (req.url == "/rest/getCoverArt.view") {
			return req.respond("image/jpeg", model.getAlbumCover(req.params["id"], req.params["size"]));
		}
		else if (req.url == "/rest/stream.view") {
			// Lookup song_id and get a file name!
			string song_id = req.params["id"];
			FILE * fd = model.getSongFile(song_id);
			if (fd)
				return req.respond("application/octet-stream", new file_generator(fd));
		}

		// Misc stuff, needs to be there just to make clients happy :)
		else if (req.url == "/rest/getMusicFolders.view") {
			return XML(req,
				XN("musicFolders",
					XN("musicFolder", {
						{"id", "1"},
						{"name", "Music"}
					})
				)
			);
		}
		else if (req.url == "/rest/getLicense.view") {
			return XML(req,
				XN("license", {
					{"valid", "true"},
					{"email", "example@example.com"},
					{"key",   "ABC123DEF"},
				})
			);
		}
		else if (req.url == "/rest/ping.view") {
			return XML(req, "");
		}
		else if (req.url == "/rest/getUser.view") {
			return XML(req,
				XN("user", {
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
				})
			);
		}

		return req.respond_not_found();
	}

	file_service(string dbpath)
	: model(dbpath) {
		//
	}

	~file_service() {
		//
	}
};

int main(int argc, char* argv[]) {
	if (argc < 3) {
		fprintf(stderr, "Usage: %s database port [threads] [connection_limit]\n", argv[0]);
		return 1;
	}
	string database = argv[1];
	int port = stoi(argv[2]);
	int threads = argc >= 4 ? stoi(argv[3]) : 0;
	int connection_limit = argc >= 5 ? stoi(argv[4]) : 2;

	rest_server server;
	server.set_log_file(stderr);
	server.set_max_connections(connection_limit);
	server.set_threads(threads);

	cerr << "Loading sqlite database " << database << " ..." << endl;
	file_service service(database);
	if (!server.start(&service, port)) {
		fprintf(stderr, "Cannot start REST server!\n");
		return 1;
	}
	server.wait_until_signalled();
	server.stop();

	return 0;
}



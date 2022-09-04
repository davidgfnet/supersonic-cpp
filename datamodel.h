
#ifndef __DATA_MODEL__HH__
#define __DATA_MODEL__HH__

// Data model for the database. Represents Artists, Songs and Albums.

#include <string>
#include <list>
#include <vector>
#include <string>
#include <sqlite3.h>

#include "util.h"
#include "resphelper.h"

enum classTypes { TYPE_ALBUM = 0, TYPE_ARTIST = 1, TYPE_SONG = 2, TYPE_ERROR = 3 };

class IdObj {
public:
	std::string sid() const { return hexencode64(id); }
	uint64_t id;
};

class Artist : public IdObj {
public:
	Artist(sqlite3_stmt * stmt) {
		id       = sqlite3_column_int64 (stmt, 0);
		name     = std::string((char*)sqlite3_column_text (stmt, 1));
	}
	std::string name;
};

class Album : public IdObj {
public:
	Album() {}
	Album(sqlite3_stmt * stmt) {
		id       = sqlite3_column_int64 (stmt, 0);
		title    = std::string((char*)sqlite3_column_text (stmt, 1));
		artistid = sqlite3_column_int64 (stmt, 2);
		artist   = std::string((char*)sqlite3_column_text (stmt, 3));
		hascover = sqlite3_column_int(stmt, 4);
	}
	uint64_t artistid;
	std::string sartistid() const { return hexencode64(artistid); }
	std::string title, artist;
	int hascover;
};

class Song : public IdObj {
public:
	uint64_t albumid, artistid;
	std::string title, album, artist;
	unsigned trackn, duration, year, discn, bitRate;
	std::string genre, type;

	Song(sqlite3_stmt * stmt) {
		id       = sqlite3_column_int64 (stmt, 0);
		title    = std::string((char*)sqlite3_column_text (stmt, 1));
		albumid  = sqlite3_column_int64 (stmt, 2);
		album    = std::string((char*)sqlite3_column_text (stmt, 3));
		artistid = sqlite3_column_int64 (stmt, 4);
		artist   = std::string((char*)sqlite3_column_text (stmt, 5));

		trackn   = sqlite3_column_int(stmt, 6);
		discn    = sqlite3_column_int(stmt, 7);
		year     = sqlite3_column_int(stmt, 8);
		duration = sqlite3_column_int(stmt, 9);
		bitRate  = sqlite3_column_int(stmt,10);

		genre    = std::string((char*)sqlite3_column_text (stmt,11));
		type     = std::string((char*)sqlite3_column_text (stmt,12));
	}

	std::string sartistid() const { return hexencode64(artistid); }
	std::string salbumid()  const { return hexencode64(albumid); }

	std::unordered_map<std::string, DataField> getAttrs() const {
		return {
			{"id",       DS(sid()) },
			{"title",    DS(title) },
			{"parent",   DS(salbumid()) },
			{"album",    DS(album) },
			{"albumId",  DS(salbumid()) },
			{"artist",   DS(artist) },
			{"artistId", DS(sartistid()) },
			{"track",    DI(trackn) },
			{"genre",    DS(genre) },
			{"duration", DI(duration) },
			{"year",     DI(year) },
			{"discNumber", DI(discn) },
			{"bitRate",  DI(bitRate) },
			{"suffix",   DS(type) },
			{"contentType", DS(mimetypes[type]) },
			{"isDir",    DB(false) },
			{"coverArt", DS(salbumid()) },
		};
	}
};

class DataModel {
public:
	DataModel(sqlite3* sqldb) : sqldb(sqldb) { }

	bool checkCredentials(std::string user, std::string pass) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT * FROM users WHERE username=? AND password=?", -1, &stmt, NULL);
		sqlite3_bind_text(stmt, 1, user.c_str(), -1, NULL);
		sqlite3_bind_text(stmt, 2, pass.c_str(), -1, NULL);
		bool res = (sqlite3_step(stmt) == SQLITE_ROW);
		sqlite3_finalize(stmt);
		return res;
	}

	bool checkCredentialsMD5(std::string user, std::string token, std::string salt) {
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

	std::string getAlbumCover(uint64_t id, unsigned size) {
		const std::vector<std::string> fields = {
			"cover128", "cover256", "cover512", "cover1024", "cover"
		};
		unsigned off = (size > 1024 || !size) ? 4:
		               (size >  512) ? 3:
		               (size >  256) ? 2:
		               (size >  128) ? 1:0;

		std::string ret;
		for (unsigned i = off; i < 5 && ret.empty(); i++) {
			sqlite3_stmt *stmt;
			sqlite3_prepare_v2(sqldb, ("SELECT " + fields[i] +
			                           " FROM albums WHERE id=?").c_str(), -1, &stmt, NULL);
			sqlite3_bind_int64(stmt, 1, id);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				auto length = sqlite3_column_bytes(stmt, 0);
				ret = std::string((char*)sqlite3_column_blob(stmt, 0), length);
			}
			sqlite3_finalize(stmt);
		}
		return ret;
	}

	std::string getSongFile(uint64_t id) {
		std::string filename;
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT filename FROM songs WHERE id=?", -1, &stmt, NULL);
		sqlite3_bind_int64(stmt, 1, id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			filename = (char*)sqlite3_column_text (stmt, 0);
		}
		sqlite3_finalize(stmt);

		return filename;
	}

	std::list<Album> getAllAlbumsSorted(unsigned offset, unsigned size) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, hascover "
		                          "FROM albums ORDER BY `title` COLLATE NOCASE ASC "
		                          "LIMIT ? OFFSET ?", -1, &stmt, NULL);
		sqlite3_bind_int64(stmt, 1, size);
		sqlite3_bind_int64(stmt, 2, offset);

		std::list<Album> albums;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			albums.emplace_back(stmt);
		sqlite3_finalize(stmt);

		return albums;
	}

	std::list<Album> getAlbumsByArtist(uint64_t artistid) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, hascover "
		                          "FROM albums WHERE artistid=? ORDER BY `title` "
		                          "COLLATE NOCASE ASC", -1, &stmt, NULL);
		sqlite3_bind_int64(stmt, 1, artistid);

		std::list<Album> albums;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			albums.emplace_back(stmt);
		sqlite3_finalize(stmt);

		return albums;
	}

	Album getAlbum(uint64_t id) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, artistid, artist, hascover "
		                          "FROM albums WHERE `id`=?", -1, &stmt, NULL);
		sqlite3_bind_int64(stmt, 1, id);

		Album ret;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ret = Album(stmt);
		sqlite3_finalize(stmt);

		return ret;
	}

	std::list<Artist> getArtists() {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, `name` FROM artists ORDER BY `name` COLLATE NOCASE ASC", -1, &stmt, NULL);

		std::list<Artist> artists;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			artists.emplace_back(stmt);
		sqlite3_finalize(stmt);

		return artists;
	}

	std::unique_ptr<Song> getSong(uint64_t id) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, albumid, album, artistid, artist,"
			"trackn, discn, year, duration, bitRate, genre, type FROM songs "
			"WHERE `id`=?", -1, &stmt, NULL);
		sqlite3_bind_int64(stmt, 1, id);

		Song *ret = nullptr;
		if (sqlite3_step(stmt) == SQLITE_ROW)
			ret = new Song(stmt);
		sqlite3_finalize(stmt);

		return std::unique_ptr<Song>(ret);
	}

	std::list<Song> getSongsByAlbum(uint64_t id) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, albumid, album, artistid, artist,"
			"trackn, discn, year, duration, bitRate, genre, type FROM songs "
			"WHERE `albumid`=? ORDER BY trackn, discn ASC", -1, &stmt, NULL);

		sqlite3_bind_int64(stmt, 1, id);

		std::list<Song> songs;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			songs.emplace_back(stmt);
		sqlite3_finalize(stmt);

		return songs;
	}

	std::list<Song> getRandomSongs(unsigned limit) {
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "SELECT `id`, title, albumid, album, artistid, artist, "
			"trackn, discn, year, duration, bitRate, genre, type FROM songs "
			"ORDER BY random() LIMIT ?", -1, &stmt, NULL);
		sqlite3_bind_int64(stmt, 1, limit);

		std::list<Song> songs;
		while (sqlite3_step(stmt) == SQLITE_ROW)
			songs.emplace_back(stmt);
		sqlite3_finalize(stmt);

		return songs;
	}

	classTypes classifyId(uint64_t id) {
		unsigned cl = id >> 60;
		if (cl >= TYPE_ERROR)
			return TYPE_ERROR;
		return (classTypes)cl;
	}

private:
	sqlite3 * sqldb;
};

#endif


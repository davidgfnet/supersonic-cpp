
#include "userdata.h"
#include "datamodel.h"
#include "util.h"


// Intializes all the tables required for this to work

static const char * init_sql = "\
	CREATE TABLE `playlists` (\
		`id`       INTEGER NOT NULL UNIQUE PRIMARY KEY AUTOINCREMENT,\
		`user`     TEXT NOT NULL,\
		`name`     TEXT,\
		`comment`  TEXT,\
		`public`   INTEGER,\
		`songs`    BLOB\
	);\
";

PlayList::PlayList(sqlite3_stmt * stmt) {
	id       = sqlite3_column_int64 (stmt, 0);
	name     = std::string((char*)sqlite3_column_text (stmt, 1));
	comment  = std::string((char*)sqlite3_column_text (stmt, 2));
	username = std::string((char*)sqlite3_column_text (stmt, 3));
	upublic  = sqlite3_column_int64 (stmt, 4);

	auto length = sqlite3_column_bytes(stmt, 5);
	songs.reserve(length / 8);
	std::string data((char*)sqlite3_column_blob(stmt, 5), length);
	for (unsigned i = 0; i < length / 8U; i++)
		songs.push_back(a2i64((uint8_t*)&data[i * 8]));
}

void PlayList::flush(sqlite3 *db) const {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO playlists "
	                   "`id`, name, comment, user, public, songs VALUES "
	                   "(?, ?, ?, ?, ?, ?)", -1, &stmt, NULL);
	sqlite3_bind_int64(stmt, 1, id);
	sqlite3_bind_text (stmt, 2, name.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 3, comment.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 4, username.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 5, upublic ? 1 : 0);

	std::string data; data.reserve(songs.size() * sizeof(uint64_t));
	for (auto s : songs)
		data += i642a(s);
	sqlite3_bind_blob(stmt, 6, data.c_str(), data.size(), NULL);

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

UserData::UserData(sqlite3 *db) {
	// Run initial statements just in case, this should be a no-op if already there.
	sqlite3_exec(db, init_sql, NULL, NULL, NULL);

	this->dbh = db;
}

UserData::~UserData() {
	sqlite3_close(dbh);
}

std::unique_ptr<PlayList> UserData::getPlaylist(uint64_t pid) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(dbh, "SELECT `id`, name, comment, user, public, songs "
	                   "FROM playlists WHERE `id` = ?", -1, &stmt, NULL);
	sqlite3_bind_int64(stmt, 1, pid);
	PlayList *ret = nullptr;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		ret = new PlayList(stmt);
	sqlite3_finalize(stmt);
	return std::unique_ptr<PlayList>(ret);
}

std::list<PlayList> UserData::getPlaylists(std::string user) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(dbh, "SELECT `id`, name, comment, user, public, songs "
	                   "FROM playlists WHERE user = ?", -1, &stmt, NULL);
	sqlite3_bind_text(stmt, 1, user.c_str(), -1, NULL);
	std::list<PlayList> ret;
	while (sqlite3_step(stmt) == SQLITE_ROW)
		ret.push_back(PlayList(stmt));

	sqlite3_finalize(stmt);
	return ret;
}


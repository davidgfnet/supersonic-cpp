
#include <time.h>

#include "userdata.h"
#include "datamodel.h"
#include "util.h"


// Intializes all the tables required for this to work

static const char * init_sql = "\
	CREATE TABLE `playlists` (\
		`id`       INTEGER NOT NULL UNIQUE PRIMARY KEY AUTOINCREMENT,\
		`user`     TEXT NOT NULL,\
		`name`     TEXT NOT NULL,\
		`comment`  TEXT NOT NULL,\
		`public`   INTEGER NOT NULL,\
		`songs`    BLOB NOT NULL,\
		`created`  INTEGER NOT NULL,\
		`changed`  INTEGER NOT NULL\
	);\
";

#define PL_COLUMNS "`id`, name, comment, user, public, songs, created, changed"

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

	created  = sqlite3_column_int64 (stmt, 6);
	changed  = sqlite3_column_int64 (stmt, 7);
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
	sqlite3_prepare_v2(dbh, "SELECT " PL_COLUMNS " FROM playlists WHERE `id` = ?", -1, &stmt, NULL);
	sqlite3_bind_int64(stmt, 1, pid);
	PlayList *ret = nullptr;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		ret = new PlayList(stmt);
	sqlite3_finalize(stmt);
	return std::unique_ptr<PlayList>(ret);
}

std::list<PlayList> UserData::getPlaylists(std::string user) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(dbh, "SELECT " PL_COLUMNS " FROM playlists WHERE user = ? OR public = 1",
	                   -1, &stmt, NULL);
	sqlite3_bind_text(stmt, 1, user.c_str(), -1, NULL);
	std::list<PlayList> ret;
	while (sqlite3_step(stmt) == SQLITE_ROW)
		ret.push_back(PlayList(stmt));

	sqlite3_finalize(stmt);
	return ret;
}

std::unique_ptr<PlayList> UserData::createPlaylist(const std::string &user, const std::string &name) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(dbh, "INSERT INTO playlists (user, name, comment, public, songs, created, changed) "
	                   "VALUES (?, ?, '', 0, x'', ?, ?)", -1, &stmt, NULL);
	sqlite3_bind_text (stmt, 1, user.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 2, name.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 3, time(NULL));
	sqlite3_bind_int64(stmt, 4, time(NULL));
	int r = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (r != SQLITE_DONE)
		return nullptr;
	// Safe since mtx serializes all inserts on this (shared) connection
	return getPlaylist(sqlite3_last_insert_rowid(dbh));
}

void UserData::savePlaylist(PlayList &pl) {
	pl.changed = time(NULL);
	std::string data; data.reserve(pl.songs.size() * sizeof(uint64_t));
	for (auto s : pl.songs)
		data += i642a(s);

	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(dbh, "UPDATE playlists SET name = ?, comment = ?, public = ?, songs = ?, "
	                   "changed = ? WHERE `id` = ?", -1, &stmt, NULL);
	sqlite3_bind_text (stmt, 1, pl.name.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 2, pl.comment.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 3, pl.upublic ? 1 : 0);
	sqlite3_bind_blob (stmt, 4, data.data(), data.size(), NULL);
	sqlite3_bind_int64(stmt, 5, pl.changed);
	sqlite3_bind_int64(stmt, 6, pl.id);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void UserData::deletePlaylist(uint64_t pid) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(dbh, "DELETE FROM playlists WHERE `id` = ?", -1, &stmt, NULL);
	sqlite3_bind_int64(stmt, 1, pid);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}


#include <cstdlib>
#include <string>
#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <map>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string>
#include <locale>
#include <codecvt>

#include <sqlite3.h>
#include <openssl/sha.h>
#include <id3tag.h>

using namespace std;

const char * init_sql = "\
	CREATE TABLE `albums` (\
		`id`	INTEGER NOT NULL UNIQUE,\
		`title`	TEXT,\
		`artistid`	INTEGER,\
		`artist`	TEXT,\
		`cover`	BLOB,\
		PRIMARY KEY(id)\
	);\
	CREATE TABLE `artists` (\
		`id`	INTEGER NOT NULL UNIQUE,\
		`name`	TEXT,\
		PRIMARY KEY(id)\
	);\
	CREATE TABLE `songs` (\
		`id`	INTEGER NOT NULL UNIQUE,\
		`title`	TEXT,\
		`albumid`	INTEGER,\
		`album`	TEXT,\
		`artistid`	INTEGER,\
		`artist`	TEXT,\
		`trackn`	INTEGER,\
		`discn`	INTEGER,\
		`year`	INTEGER,\
		`duration`	INTEGER,\
		`bitRate`	INTEGER,\
		`genre`	TEXT,\
		`type`	TEXT,\
		`filename`	TEXT,\
		PRIMARY KEY(id)\
	);\
	CREATE TABLE `users` (\
		`username`	TEXT NOT NULL UNIQUE,\
		`password`	TEXT,\
		PRIMARY KEY(username)\
	);\
";

void panic_if(bool cond, string text) {
	if (cond) {
		cerr << text << endl;
		exit(1);
	}
}

uint64_t calcId(string s) {
	unsigned char md[20];
	SHA1((unsigned char*)s.c_str(), s.size(), md);

	uint64_t n = 0;
	for (uint64_t i = 0; i < 8; i++)
		n = (n << 8) | md[i];

	return n & 0x7FFFFFFFFFFFFFFF;
}

template <typename T>
string toUTF8(const basic_string<T, char_traits<T>, allocator<T>>& source)
{
    string result;

    wstring_convert<codecvt_utf8_utf16<T>, T> convertor;
    result = convertor.to_bytes(source);

    return result;
}

string iso88959_to_utf8(string inp)
{
    string ret;
	for (auto c: inp) {
		if (!(c & 0x80)) {
			ret += c;
		}
		else {
			ret += (char) (0xc0 | ((unsigned)c) >> 6);
			ret += (char) (0x80 | (c & 0x3F));
		}
    }
    return ret;
}

string mp3field(struct id3_tag const * tags, char const * fieldn) {
	struct id3_frame * f = id3_tag_findframe(tags, fieldn, 0);
	union id3_field * field;

	if (!f) {
		return "";
	}

	field = id3_frame_field(f, 1);
	if (field) {
		id3_ucs4_t const * encstr = id3_field_getstrings(field, 0);
		id3_utf8_t * content = id3_ucs4_utf8duplicate(encstr);
		string ret((char*)content);
		free(content);
		return ret;
	}

	return "";
}

void insert_artist(sqlite3 * sqldb, string artist) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(sqldb, "INSERT INTO `artists` (`id`, `name`) VALUES (?,?);", -1, &stmt, NULL);

	sqlite3_bind_int64(stmt, 1, calcId(artist));
	sqlite3_bind_text (stmt, 2, artist.c_str(), -1, NULL);

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void insert_album(sqlite3 * sqldb, string album, string artist) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(sqldb, "INSERT INTO `albums` "
		"(`id`, `title`, `artistid`, `artist`) VALUES (?,?,?,?);", -1, &stmt, NULL);

	sqlite3_bind_int64(stmt, 1, calcId(album + "@" + artist));
	sqlite3_bind_text (stmt, 2, album.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 3, calcId(artist));
	sqlite3_bind_text (stmt, 4, artist.c_str(), -1, NULL);

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void insert_song(sqlite3 * sqldb, string filename, string title, string artist, string album,
	string type, string genre, unsigned tn, unsigned year, unsigned discn, unsigned duration) {

	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(sqldb, "INSERT INTO `songs` "
		"(`id`, `title`, `albumid`, `album`, `artistid`, `artist`, `type`, `genre`, `trackn`, `year`, `discn`, `duration`, `bitRate`, `filename`)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?);", -1, &stmt, NULL);

	sqlite3_bind_int64(stmt, 1, calcId(to_string(tn) + "@" + to_string(discn) + "@" + title + "@" + album + "@" + artist));
	sqlite3_bind_text (stmt, 2, title.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 3, calcId(album + "@" + artist));
	sqlite3_bind_text (stmt, 4, album.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 5, calcId(artist));
	sqlite3_bind_text (stmt, 6, artist.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 7, type.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 8, genre.c_str(), -1, NULL);
	sqlite3_bind_int  (stmt, 9, tn);
	sqlite3_bind_int  (stmt,10, year);
	sqlite3_bind_int  (stmt,11, discn);
	sqlite3_bind_int  (stmt,12, duration);
	sqlite3_bind_int  (stmt,13, 0);
	sqlite3_bind_text (stmt,14, filename.c_str(), -1, NULL);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		cout << "Err " << filename << endl;
		cout << sqlite3_errmsg(sqldb) << endl;
	}

	sqlite3_finalize(stmt);

	insert_album(sqldb, album, artist);
	insert_artist(sqldb, artist);
}

void scan_music_file(sqlite3 * sqldb, string fullpath) {
	string ext = fullpath.substr(fullpath.size()-3);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

	if (ext == "mp3") {
		struct id3_file * mp3file = id3_file_open(fullpath.c_str(), ID3_FILE_MODE_READONLY);
		if (!mp3file) {
			cerr << "Couldn't process " << fullpath << endl;
			return;
		}
		struct id3_tag * tags = id3_file_tag(mp3file);
		
		string title  = mp3field(tags, ID3_FRAME_TITLE);
		string artist = mp3field(tags, ID3_FRAME_ARTIST);
		string album  = mp3field(tags, ID3_FRAME_ALBUM);
		string track  = mp3field(tags, ID3_FRAME_TRACK);
		string genre  = mp3field(tags, ID3_FRAME_GENRE);

		unsigned tn   = 0;

		try {
			tn = stoul(track);
		} catch(...) {}

		int year = -1;
		try {
			year = stoul(mp3field(tags, ID3_FRAME_YEAR));
		} catch (...) {}

		int discn = 0;
		try {
			discn = stoul(mp3field(tags, "TPOS"));
		} catch (...) {}

		int duration = 0;
		try {
			duration = stoul(mp3field(tags, "TLEN"));
		} catch (...) {}

		insert_song(sqldb, fullpath, title, artist, album, "mp3", genre, tn, year, discn, duration);

		id3_file_close(mp3file);
	}
	if (ext == "ogg") {
		
	}
}

void scan_fs(sqlite3 * sqldb, string name) {
	DIR *dir;
	struct dirent *entry;

	if (!(dir = opendir(name.c_str())))  return;
	if (!(entry = readdir(dir))) return;

	do {
		string fullpath = name + "/" + string(entry->d_name);
		string ext = fullpath.substr(fullpath.size()-3);

		struct stat statbuf;
		stat(fullpath.c_str(), &statbuf);
		if (S_ISDIR(statbuf.st_mode)) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
				continue;
			scan_fs(sqldb, fullpath);
		}
		else
			scan_music_file(sqldb, fullpath);
	} while (entry = readdir(dir));
	closedir(dir);
}


int main(int argc, char* argv[]) {
	if (argc < 3) {
		fprintf(stderr,
			"Usage: %s action [args...]\n"
			"  %s scan file.db musicdir/ \n"
			"  %s useradd file.db username password\n"
			"  %s userdel file.db username\n",
			argv[0],argv[0],argv[0],argv[0]);
		return 1;
	}
	string action = argv[1];
	string dbpath = argv[2];

	// Create a new sqlite db if file does not exist
	sqlite3 * sqldb;
	int ok = sqlite3_open_v2(
		dbpath.c_str(),
		&sqldb,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
		NULL
	);
	panic_if(ok != SQLITE_OK, "Could not open sqlite3 database!");

	if (action == "scan") {
		string musicdir = argv[3];

		sqlite3_exec(sqldb, init_sql, NULL, NULL, NULL);

		// Start scanning and adding stuff to the database
		scan_fs(sqldb, musicdir);
	}
	if (action == "useradd") {
		string user = argv[3];
		string pass = argv[4];

		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(sqldb, "INSERT INTO `users` (`username`, `password`) VALUES (?,?);", -1, &stmt, NULL);

		sqlite3_bind_text (stmt, 1, user.c_str(), -1, NULL);
		sqlite3_bind_text (stmt, 2, pass.c_str(), -1, NULL);

		if (sqlite3_step(stmt) != SQLITE_DONE)
			cerr << "Error adding user " << sqlite3_errmsg(sqldb) << endl;
		sqlite3_finalize(stmt);
	}

	// Close and write to disk
	sqlite3_close(sqldb);

	return 0;
}


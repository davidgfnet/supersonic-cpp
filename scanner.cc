
#include <cstdlib>
#include <string>
#include <iostream>
#include <vector>
#include <thread>
#include <algorithm>
#include <set>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string>
#include <openssl/sha.h>

#include <sqlite3.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/oggfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacpicture.h>

#include "util.h"
#include "cqueue.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBI_NO_STDIO
#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"

using namespace std;

const char * init_sql = "\
	CREATE TABLE `albums` (\
		`id`	INTEGER NOT NULL UNIQUE,\
		`title`	TEXT,\
		`artistid`	INTEGER,\
		`artist`	TEXT,\
		`hascover`	INTEGER,\
		`cover128`	BLOB,\
		`cover256`	BLOB,\
		`cover512`	BLOB,\
		`cover1024`	BLOB,\
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
		`timestamp`	INTEGER,\
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

enum classTypes { TYPE_ALBUM = 0, TYPE_ARTIST = 1, TYPE_SONG = 2 };

uint64_t calcId(string s, classTypes ctype) {
	uint8_t h[SHA256_DIGEST_LENGTH];
	SHA256((uint8_t*)s.c_str(), s.size(), h);

	uint64_t hash = a2i64(h);
	return (((uint64_t)ctype) << 60) | (hash & ((1ULL << 60) - 1));
}

void insert_artist(sqlite3 * sqldb, string artist) {
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(sqldb, "INSERT OR REPLACE INTO `artists` (`id`, `name`) VALUES (?,?);", -1, &stmt, NULL);

	sqlite3_bind_int64(stmt, 1, calcId(artist, TYPE_ARTIST));
	sqlite3_bind_text (stmt, 2, artist.c_str(), -1, NULL);

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void wfn(void *ctx, void *data, int size) {
	*((std::string*)ctx) += std::string((char*)data, size);
}

std::set<uint64_t> processed_albums;
std::mutex albummutex;

void insert_album(sqlite3 * sqldb, string album, string artist, string cover) {
	// Check if some other worker is already working/worked in this album.
	// This is to avoid processing covers more than once (expensive!)
	uint64_t albumid = calcId(album + "@" + artist, TYPE_ALBUM);
	if (!cover.empty()) {
		std::lock_guard<std::mutex> g(albummutex);
		if (processed_albums.count(albumid))
			return;
		processed_albums.insert(albumid);
	}

	// Create several versions of this cover, so we can serve different sizes
	const unsigned sizes[4] = {128, 256, 512, 1024};
	std::string smallcover[4];
	if (cover.size()) {
		int width, height, nchan;
		stbi_uc *original = stbi_load_from_memory((uint8_t*)cover.c_str(), cover.size(),
		                                          &width, &height, &nchan, 3);

		for (unsigned i = 0; i < 4; i++) {
			int nw, nh;
			if (width > height) {
				nw = sizes[i];
				nh = sizes[i] * (double)height / (double)width;
			}else{
				nh = sizes[i];
				nw = sizes[i] * (double)width / (double)height;
			}

			// We only shrink, never enlarge
			if (nw <= width && nh <= height) {
				unsigned osize = nw*nh*3;
				std::string tmpb(osize, '\0');
				stbir_resize_uint8(original, width, height, 0, (uint8_t*)&tmpb[0], nw, nh, 0, 3);

				stbi_write_jpg_to_func(wfn, &smallcover[i], nw, nh, 3, tmpb.c_str(), 70);
			}
		}
		stbi_image_free(original);
	}

	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(sqldb, "INSERT OR REPLACE INTO `albums` "
		"(`id`, `title`, `artistid`, `artist`, `hascover`, `cover`, "
		"`cover128`, `cover256`, `cover512`, `cover1024`) "
		"VALUES (?,?,?,?,?,?,?,?,?,?);", -1, &stmt, NULL);
	sqlite3_bind_int64(stmt, 1, albumid);
	sqlite3_bind_text (stmt, 2, album.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 3, calcId(artist, TYPE_ARTIST));
	sqlite3_bind_text (stmt, 4, artist.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 5, cover.size() ? 1 : 0);
	sqlite3_bind_blob (stmt, 6, cover.data(), cover.size(), NULL);
	sqlite3_bind_blob (stmt, 7, smallcover[0].data(), smallcover[0].size(), NULL);
	sqlite3_bind_blob (stmt, 8, smallcover[1].data(), smallcover[1].size(), NULL);
	sqlite3_bind_blob (stmt, 9, smallcover[2].data(), smallcover[2].size(), NULL);
	sqlite3_bind_blob (stmt,10, smallcover[3].data(), smallcover[3].size(), NULL);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

void insert_song(sqlite3 * sqldb, string filename, string title, string artist, string album, string type, string genre,
                 unsigned tn, unsigned year, unsigned discn, unsigned duration, unsigned bitrate, uint64_t timestamp) {

	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(sqldb, "INSERT OR REPLACE INTO `songs` "
		"(`id`, `title`, `albumid`, `album`, `artistid`, `artist`, `type`, "
		"`genre`, `trackn`, `year`, `discn`, `duration`, `bitRate`, `filename`, `timestamp`)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", -1, &stmt, NULL);

	sqlite3_bind_int64(stmt, 1, calcId(to_string(tn) + "@" + to_string(discn) + "@" + title + "@" + album + "@" + artist, TYPE_SONG));
	sqlite3_bind_text (stmt, 2, title.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 3, calcId(album + "@" + artist, TYPE_ALBUM));
	sqlite3_bind_text (stmt, 4, album.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 5, calcId(artist, TYPE_ARTIST));
	sqlite3_bind_text (stmt, 6, artist.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 7, type.c_str(), -1, NULL);
	sqlite3_bind_text (stmt, 8, genre.c_str(), -1, NULL);
	sqlite3_bind_int  (stmt, 9, tn);
	sqlite3_bind_int  (stmt,10, year);
	sqlite3_bind_int  (stmt,11, discn);
	sqlite3_bind_int  (stmt,12, duration);
	sqlite3_bind_int  (stmt,13, bitrate);
	sqlite3_bind_text (stmt,14, filename.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt,15, timestamp);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		cout << "Err " << filename << endl;
		cout << sqlite3_errmsg(sqldb) << endl;
	}

	sqlite3_finalize(stmt);
}

void scan_music_file(sqlite3 * sqldb, string fullpath) {
	string ext = fullpath.substr(fullpath.size()-3);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

	if (ext != "mp3" && ext != "ogg")
		return;

	// Do not process the file if it has (presumably) not changed
	struct stat attrs;
	stat(fullpath.c_str(), &attrs);

	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(sqldb, "SELECT timestamp FROM songs WHERE filename == ? AND timestamp == ?",
	                   -1, &stmt, NULL);
	sqlite3_bind_text (stmt, 1, fullpath.c_str(), -1, NULL);
	sqlite3_bind_int64(stmt, 2, attrs.st_mtime);
	bool nochanges = (sqlite3_step(stmt) == SQLITE_ROW);
	sqlite3_finalize(stmt);

	if (nochanges)
		return;

	TagLib::FileRef f(fullpath.c_str());
	if (f.isNull())
		return;

	TagLib::Tag *tag = f.tag();
	if (!tag)
		return;

	// Read basic properties
	TagLib::AudioProperties *properties = f.audioProperties();
	if (!properties)
		return;

	int discn = 0;
	if (tag->properties().contains("DISCNUMBER"))
		discn = tag->properties()["DISCNUMBER"][0].toInt();

	std::string albumartist = tag->artist().toCString(true);
	if (tag->properties().contains("ALBUMARTIST"))
		albumartist = tag->properties()["ALBUMARTIST"][0].toCString(true);

	string cover;
	if (ext == "mp3") {
		TagLib::MPEG::File audioFile(fullpath.c_str());
		TagLib::ID3v2::Tag *mp3_tag = audioFile.ID3v2Tag(true);

		if (mp3_tag) {
			auto frames = mp3_tag->frameList("APIC");
			if (!frames.isEmpty()) {
				auto frame = static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front());
				cover = string(frame->picture().data(), frame->picture().size());
			}
			if (mp3_tag->properties().contains("ALBUMARTIST"))
				albumartist = mp3_tag->properties()["ALBUMARTIST"][0].toCString(true);
			if (mp3_tag->properties().contains("DISCNUMBER"))
				discn = mp3_tag->properties()["DISCNUMBER"][0].toInt();
		}
	}
	else if (ext == "ogg") {
		auto vorbis_tag = dynamic_cast<TagLib::Ogg::XiphComment *>(tag);
		if (vorbis_tag) {
			// Rely on these fields better than any other generic ones.
			if (vorbis_tag->properties().contains("ALBUMARTIST"))
				albumartist = vorbis_tag->properties()["ALBUMARTIST"][0].toCString(true);
			if (vorbis_tag->properties().contains("DISCNUMBER"))
				discn = vorbis_tag->properties()["DISCNUMBER"][0].toInt();

			// Extract pictures one way
			for (auto t : std::vector<TagLib::FLAC::Picture::Type>({
				TagLib::FLAC::Picture::FrontCover,
				TagLib::FLAC::Picture::Media,
				TagLib::FLAC::Picture::Other})) {

				for (const auto & pic : vorbis_tag->pictureList())
					if (pic->type() == t && cover.empty())
						cover = std::string(pic->data().data(), pic->data().size());
			}
			if (vorbis_tag->pictureList().size() && cover.empty())
				cover = std::string(vorbis_tag->pictureList()[0]->data().data(),
				                    vorbis_tag->pictureList()[0]->data().size());

			// Or another :D
			if (vorbis_tag->properties().contains("METADATA_BLOCK_PICTURE")) {
				auto cdata = vorbis_tag->properties()["METADATA_BLOCK_PICTURE"][0].data(TagLib::String::UTF8);
				cover = base64Decode(string(cdata.data(), cdata.size()));
				TagLib::FLAC::Picture picture;
				picture.parse(TagLib::ByteVector(cover.c_str(), cover.size()));
				cover = string(picture.data().data(), picture.data().size());
			}
		}
	}

	insert_song(sqldb, fullpath, tag->title().toCString(true), albumartist,
		tag->album().toCString(true), ext, tag->genre().toCString(true),
		tag->track(), tag->year(), discn, properties->length(), properties->bitrate(), attrs.st_mtime);

	insert_album(sqldb, tag->album().toCString(true), albumartist, cover);
	insert_artist(sqldb, albumartist);
}

void scan_fs(string name, ConcurrentQueue<std::string> *fileq) {
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
			scan_fs(fullpath, fileq);
		}
		else
			fileq->push(fullpath);
	} while ((entry = readdir(dir)));
	closedir(dir);
}

void scan_worker(sqlite3 * sqldb, ConcurrentQueue<std::string> *fileq) {
	std::string filename;
	while (fileq->pop(&filename))
		scan_music_file(sqldb, filename);
}

void status_thread(ConcurrentQueue<std::string> *fileq) {
	while (!fileq->closed()) {
		std::cout << (fileq->queued() - fileq->size()) << "/" << fileq->queued() << "      \r";
		std::cout.flush();
		sleep(1);
	}
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
	unsigned nthreads = atoi(getenv("NTHREADS") ? : "4") & 255;
	std::cerr << "Using " << nthreads << " threads" << std::endl;

	// Create a new sqlite db if file does not exist
	sqlite3 * sqldb;
	int ok = sqlite3_open_v2(
		dbpath.c_str(),
		&sqldb,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_LOCK_EXCLUSIVE,
		NULL
	);
	panic_if(ok != SQLITE_OK, "Could not open sqlite3 database!");
	sqlite3_exec(sqldb, "PRAGMA synchronous = OFF", NULL, NULL, NULL);

	if (action == "scan") {
		string musicdir = argv[3];

		sqlite3_exec(sqldb, init_sql, NULL, NULL, NULL);

		// Start scanning and adding stuff to the database
		ConcurrentQueue<std::string> fileq(1024);
		std::vector<std::thread> tpool;
		for (unsigned i = 0; i < nthreads; i++)
			tpool.emplace_back(scan_worker, sqldb, &fileq);
		tpool.emplace_back(status_thread, &fileq);
		scan_fs(musicdir, &fileq);
		fileq.close();
		for (auto & t : tpool)
			t.join();
	}
	if (action == "useradd") {
		string user = argv[3];
		string pass = argv[4];

		sqlite3_exec(sqldb, init_sql, NULL, NULL, NULL);

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



#ifndef __USER_DATA__H__
#define __USER_DATA__H__

#include <list>
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <stdint.h>
#include <sqlite3.h>

class PlayList {
public:
	PlayList(sqlite3_stmt *);

	// Attrs
	uint64_t id;
	std::string name, comment, username;
	bool upublic;
	std::vector<uint64_t> songs;
	uint64_t created, changed;
};

class UserData {
private:
	sqlite3 *dbh;

public:
	UserData(sqlite3 *db);
	~UserData();

	// Hold this while doing read-modify-write on playlists
	std::mutex mtx;

	// No authentication here at all!
	std::unique_ptr<PlayList> getPlaylist(uint64_t pid);

	// Gets all playlists a user can see (users' and public ones), no auth as well.
	std::list<PlayList> getPlaylists(std::string user);

	// Creates an empty playlist, returns null on error
	std::unique_ptr<PlayList> createPlaylist(const std::string &user, const std::string &name);

	// Writes playlist into db
	void savePlaylist(PlayList &pl);

	void deletePlaylist(uint64_t pid);
};


#endif



#ifndef __USER_DATA__H__
#define __USER_DATA__H__

#include <list>
#include <vector>
#include <memory>
#include <string>
#include <stdint.h>
#include <sqlite3.h>

class PlayList {
public:
	PlayList(sqlite3_stmt *);

	// Flush changes to database
	void flush(sqlite3 *db) const;

	// Attrs
	uint64_t id;
	std::string name, comment, username;
	bool upublic;
	std::vector<uint64_t> songs;
};

class UserData {
private:
	sqlite3 *dbh;

public:
	UserData(sqlite3 *db);
	~UserData();

	// No authentication here at all!
	std::unique_ptr<PlayList> getPlaylist(uint64_t pid);

	// Gets all playlists for a user, no auth as well.
	std::list<PlayList> getPlaylists(std::string user);
};


#endif


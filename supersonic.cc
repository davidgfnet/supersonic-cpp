
#define _FILE_OFFSET_BITS 64
#include <cstdlib>
#include <string>
#include <iostream>
#include <list>
#include <thread>
#include <memory>
#include <unordered_map>
#include <fcgio.h>
#include <unistd.h>
#include <signal.h>

#include "argparse/argparse.hpp"
#include "util.h"
#include "queue.h"
#include "datamodel.h"
#include "userdata.h"
#include "fcgihelper.h"
#include "resphelper.h"

#define getone(m, k, def) \
	((m).find(k) == (m).end() ? def : (m).find(k)->second)


struct web_req {
	uint64_t offset, lastbyte;
	std::string method, host, uri;
	std::unordered_multimap<std::string, std::string> vars;
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

	// And user data DB
	UserData *udata;

	// Thread to spawn
	std::thread cthread;

	// Shared queue
	ConcurrentQueue<std::unique_ptr<FCGX_Request>> *rq;

	// Search directories
	std::vector<std::string> sdirs;

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

	bool checkCredentials(std::string user, web_req& req) {
		if (user.empty())
			return false;

		std::string pass = getone(req.vars, "p", "");
		if (!pass.empty()) {
			if (pass.substr(0, 4) == "enc:")
				pass = hexdecode(pass.substr(4));
			return model->checkCredentials(user, pass);
		}

		auto saltit = req.vars.find("s");
		auto toknit = req.vars.find("t");
		if (saltit != req.vars.end() && toknit != req.vars.end())
			return model->checkCredentialsMD5(user, toknit->second, saltit->second);
		return false;
	}

	str_resp* authErr(web_req & req) {
		RespFmt fmt(getone(req.vars, "f", ""), getone(req.vars, "callback", ""));
		return Entity::error(fmt, 40, "Wrong username or password").respond();
	}

	// Returns Entities, album Name and song count
	std::list<Entity> listSongs(RespFmt fmt, std::string node, const Album *alb) {
		auto songs = model->getSongsByAlbum(alb->id);
		std::list<Entity> esongs;
		for (auto song: songs)
			esongs.push_back(Entity(fmt, node, song.getAttrs()));
		return esongs;
	}

	fcgi_responder* handle(web_req& req, FCGX_Request *fastcgi_req) {
		if (req.method != "HEAD" && req.method != "GET" && req.method != "POST")
			return respond_method_not_allowed();

		std::string user = getone(req.vars, "u", "");

		// Check for auth user and kick out intruders
		if (!checkCredentials(user, req))
			return authErr(req);

		RespFmt rfmt(getone(req.vars, "f", ""), getone(req.vars, "callback", ""));
		uint64_t reqid = hexdecode64(getone(req.vars, "id", ""));

		if (req.uri == "/rest/getMusicDirectory.view") {
			std::list<Entity> entities;
			std::string tname;
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
			                    {"id",   DS(std::to_string(reqid))},
			                    {"name", DS(tname)}},
			                    entities)).respond();
		}
		else if (req.uri == "/rest/getAlbumList.view" or
		         req.uri == "/rest/getAlbumList2.view") {
			unsigned offset = atoi(getone(req.vars, "offset", "").c_str());
			unsigned size = req.vars.count("size") ? atoi(getone(req.vars, "size", "").c_str()) : 10;

			std::list<Entity> ealbums;
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

			std::string tag = (req.uri == "/rest/getAlbumList2.view") ? "albumList2" : "albumList";
			return Entity::wrap(Entity(rfmt, tag, {}, ealbums)).respond();
		}

		else if (req.uri == "/rest/getAlbum.view") {
			Album alb = model->getAlbum(reqid);
			auto songs = listSongs(rfmt, "song", &alb);
			return Entity::wrap(Entity(rfmt, "album", {
			                    {"id",        DS(std::to_string(reqid))},
			                    {"name",      DS(alb.title)},
			                    {"songCount", DI(songs.size())},
			                    {"coverArt",  DS(std::to_string(reqid))}},
			                    songs)).respond();
		}

		else if (req.uri == "/rest/getRandomSongs.view") {
			unsigned size = req.vars.count("size") ? atoi(getone(req.vars, "size", "").c_str()) : 10;

			auto songs = model->getRandomSongs(size);
			std::list<Entity> esongs;
			for (auto song: songs)
				esongs.push_back(Entity(rfmt, "song", song.getAttrs()));

			return Entity::wrap(Entity(rfmt, "randomSongs", {}, esongs)).respond();
		}

		else if (req.uri == "/rest/getIndexes.view") {
			std::list<Entity> eartists;
			for (auto artist: model->getArtists())
				eartists.push_back(Entity(rfmt, "artist", 
					{ {"id", DS(artist.sid())}, {"name", DS(artist.name)} }));

			return Entity::wrap(Entity(rfmt, "indexes", {
			                    {"lastModified", DI(1455843830000)},  // FIXME: Unix timestamp * 1000
			                    {"ignoredArticles", DS("The El La Los Las Le Les")}},
					    std::list<Entity>{
			                    Entity(rfmt, "index", {{"name", DS("Music")}}, eartists)})).respond();
		}
		else if (req.uri == "/rest/stream.view" or
		         req.uri == "/rest/download.view") {
			// Lookup song_id and get a file name!
			std::string fname = model->getSongFile(reqid);
			if (!fname.empty()) {
				FILE *fd = NULL;
				if (fname[0] == '/')
					// Use absolute path as is
					fd = fopen(fname.c_str(), "rb");
				else {
					// Try to open the file using all the search paths
					for (const auto & dir : sdirs) {
						fd = fopen((dir + "/" + fname).c_str(), "rb");
						if (fd)
							break;
					}
				}
				// Stream the data to the user if found
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
		}

		// Misc stuff, needs to be there just to make clients happy :)
		else if (req.uri == "/rest/getMusicFolders.view") {
			return Entity::wrap(Entity(rfmt, "musicFolders", {}, std::list<Entity>{
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
			                    {"streamRole",   DB(true)},
			                    {"jukeboxRole",  DB(false)},
			                    {"downloadRole", DB(true)},
			                    {"uploadRole",   DB(false)},
			                    {"playlistRole", DB(true)},
			                    {"coverArtRole", DB(false)},
			                    {"commentRole",  DB(false)},
			                    {"podcastRole",  DB(false)},
			                    {"shareRole",    DB(false)},
			                    {"videoConversionRole", DB(false)},
			                    })).respond();
		}
		else if (req.uri == "/rest/getCoverArt.view") {
			auto albumid = reqid;
			if (model->classifyId(albumid) == TYPE_SONG) {
				auto song = model->getSong(albumid);
				if (song)
					albumid = song->albumid;
			}
			unsigned size = atoi(getone(req.vars, "size", "").c_str());
			std::string img = model->getAlbumCover(albumid, size);
			return new str_resp("Status: 200\r\n"
				"Content-Type: image/jpeg\r\n"
				"Content-Length: " + std::to_string(img.size()) + "\r\n\r\n", img);
		}
		// Playlist management
		// getPlaylists getPlaylist createPlaylist updatePlaylist deletePlaylist 
		else if (req.uri == "/rest/getPlaylist.view") {
			auto pl = udata->getPlaylist(reqid);
			if (pl) {
				if (pl->upublic || pl->username == user) {
					std::list<Entity> esongs;
					for (auto songid : pl->songs) {
						auto song = model->getSong(songid);
						esongs.push_back(Entity(rfmt, "entry", song->getAttrs()));
					}
					return Entity::wrap(Entity(rfmt, "playlist", {
					                    {"id",        DS(std::to_string(reqid))},
					                    {"name",      DS(pl->name)},
					                    {"comment",   DS(pl->comment)},
					                    {"owner",     DS(pl->username)},
					                    {"public",    DB(pl->upublic)},
					                    {"songCount", DI(pl->songs.size())}},
					                    esongs)).respond();
				}
				else
					return Entity::error(rfmt, 50, "Permission denied").respond();
			}
			else
				return Entity::error(rfmt, 70, "Playlist not found").respond();
		}
		else if (req.uri == "/rest/getPlaylists.view") {
			std::list<Entity> eplaylists;
			for (const auto & pl : udata->getPlaylists(user)) {
				eplaylists.push_back(Entity::wrap(Entity(rfmt, "playlist", {
				                    {"id",        DS(std::to_string(reqid))},
				                    {"name",      DS(pl.name)},
				                    {"comment",   DS(pl.comment)},
				                    {"owner",     DS(pl.username)},
				                    {"public",    DB(pl.upublic)},
				                    {"songCount", DI(pl.songs.size())}})));
			}
			return Entity::wrap(Entity(rfmt, "playlists", {}, eplaylists)).respond();
		}

		// All the unsupported features, like podcasts & video calls are mocked out here.
		// Denies permissions to all updates and returns empty yet valid responses to all queries
		else if (req.uri == "/rest/getPlaylists.view")
			return Entity::wrap(Entity(rfmt, "playlists", {}, {})).respond();
		else if (req.uri == "/rest/getGenres.view")
			return Entity::wrap(Entity(rfmt, "genres", {}, {})).respond();
		else if (req.uri == "/rest/getPodcasts.view")
			return Entity::wrap(Entity(rfmt, "podcasts", {}, {})).respond();
		else if (req.uri == "/rest/getNewestPodcasts.view")
			return Entity::wrap(Entity(rfmt, "newestPodcasts", {}, {})).respond();
		else if (req.uri == "/rest/getInternetRadioStations.view")
			return Entity::wrap(Entity(rfmt, "internetRadioStations", {}, {})).respond();
		else if (req.uri == "/rest/getShares.view")
			return Entity::wrap(Entity(rfmt, "shares", {}, {})).respond();
		else if (req.uri == "/rest/getLyrics.view")
			return Entity::wrap(Entity(rfmt, "lyrics", {}, {})).respond();
		else if (req.uri == "/rest/getChatMessages.view")
			return Entity::wrap(Entity(rfmt, "chatMessages", {}, {})).respond();
		else if (req.uri == "/rest/refreshPodcasts.view" or
		         req.uri == "/rest/createPodcastChannel.view" or
		         req.uri == "/rest/deletePodcastChannel.view" or
		         req.uri == "/rest/deletePodcastEpisode.view" or
		         req.uri == "/rest/downloadPodcastEpisode.view" or
		         req.uri == "/rest/createInternetRadioStation.view" or
		         req.uri == "/rest/updateInternetRadioStation.view" or
		         req.uri == "/rest/deleteInternetRadioStation.view" or
		         req.uri == "/rest/createShare.view" or
		         req.uri == "/rest/updateShare.view" or
		         req.uri == "/rest/deleteShare.view" or
		         req.uri == "/rest/addChatMessage.view" or
		         req.uri == "/rest/createUser.view" or
		         req.uri == "/rest/updateUser.view" or
		         req.uri == "/rest/deleteUser.view" or
		         req.uri == "/rest/changePassword.view" or
		         req.uri == "/rest/jukeboxControl.view")
			return Entity::error(rfmt, 50, "Permission denied").respond();
		else if (req.uri == "/rest/getVideos.view")
			return Entity::wrap(Entity(rfmt, "videos", {}, {})).respond();

		return respond_not_found();
	}

public:
	SupersonicServer(DataModel *dbm, UserData *udata,
	                 ConcurrentQueue<std::unique_ptr<FCGX_Request>> *rq,
	                 std::vector<std::string> sdirs)
	: model(dbm), udata(udata), rq(rq), sdirs(sdirs) {
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
	ArgumentParser parser;
	parser.addArgument("-m", "--musicdb", 1, false);
	parser.addArgument("-u", "--userdb",  1, true);
	parser.addArgument("-t", "--threads", 1, true);
	parser.addArgument("-d", "--search-dir", '*');
	parser.parse(argc, (const char **)argv);

	// Initialize the database backend.
	sqlite3* sqldb;
	if (SQLITE_OK != sqlite3_open_v2(parser.retrieve<std::string>("m").c_str(), &sqldb,
	                                 SQLITE_OPEN_READONLY |
	                                 SQLITE_OPEN_FULLMUTEX, NULL)) {
		std::cerr << "Could not open sqlite3 music database!" << std::endl;
		return 1;
	}

	// Database to store user data, such as playlists. This is really optional.
	sqlite3* userdb;
	if (parser.count("u")) {
		if (SQLITE_OK != sqlite3_open_v2(parser.retrieve<std::string>("u").c_str(), &userdb,
		                                 SQLITE_OPEN_READWRITE |
		                                 SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL)) {
			std::cerr << "Could not open sqlite3 user database!" << std::endl;
			return 1;
		}
	} else {
		if (SQLITE_OK != sqlite3_open_v2("userdb", &userdb, SQLITE_OPEN_READWRITE |
		                                 SQLITE_OPEN_MEMORY | SQLITE_OPEN_FULLMUTEX, NULL)) {
			std::cerr << "Could not open a temporary (in-mem) sqlite3 user database!" << std::endl;
			return 1;
		}
		std::cerr << "WARNING: Using an in-memory database for user data, all data will be lost "
		             "on restart. If you want to use persistent storage use '--userdb'" << std::endl;
	}
	UserData udata(userdb);

	// Use the search dirs to retrieve the music files
	auto sdirs = parser.retrieve<std::vector<std::string>>("d");
	if (sdirs.empty())
		sdirs.push_back("/");     // Assuming aboslute paths in the database

	// Start FastCGI interface
	FCGX_Init();

	// Signal handling
	signal(SIGINT, sighandler); 
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	// Start worker threads for this
	unsigned nthreads = parser.count("t") ? atoi(parser.retrieve<std::string>("t").c_str()) : 4;
	DataModel dbm(sqldb);
	ConcurrentQueue<std::unique_ptr<FCGX_Request>> reqqueue;
	SupersonicServer *workers[nthreads];
	for (unsigned i = 0; i < nthreads; i++)
		workers[i] = new SupersonicServer(&dbm, &udata, &reqqueue, sdirs);

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

	std::cerr << "All clear, service is down, flushing databases ..." << std::endl;
	sqlite3_close(sqldb);
}



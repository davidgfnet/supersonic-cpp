
Supersonic
==========

A Subsonic music server implemented in C++

What is this?
-------------

Supersonic is a music server that implements the Subsonic API (well, not
completely!) allowing users to stream music to any Subsonic-compatible client.

It is implemented in C++ for low resource usage and speed, and provides a
FastCGI interface for a Webserver to use (that is, you can use Nginx as your
frontend).

How do I build, run and use it?
-------------------------------

To build simply run "make" and it should produce two binaries:

 * supersonic-server: Your server binary
 * supersonic-scanner: The CLI tool use to scan a music library

You need the following libraries to build and run it:

 * sqlite3: To create and query music library databases
 * libcrypto: Used for hashing stuff
 * libfcgi++ & libfcgi: Server uses this to interface FastCGI servers
 * libtag: Used to extract date from MP3 and OGG files

Now to scan your music library you can run:

```$
  supersonic-scanner scan music.sqlite /your/music/lib/path
```

This will create a database (or update an existing one) with all the songs
it can find. The scanner won't rescan any files that were already in the
database, unless they have been updated (mtime has changed!).

You will need users to access the service so run:

```$
  supersonic-scanner useradd music.sqlite someusername supersecurepass
  supersonic-scanner userdel music.sqlite someusername
```

This is a bit rough for now, sorry for that!

Once you are set you can start serving like this:

```$
  spawn-fcgi -u machineuser -s /var/www/somepath/sock -M 666 -n -- \
  supersonic --musicdb /db/path/music.db --search-dir /base/path/for/music/
```

Spawn-fcgi will spawn the server and attach it to a local socket (you can use
HTTP if you prefer, that depends on how you configure nginx/apache later).
You will need to pass the database path and as many --search-dir as you want
to indicate search paths for the music. The scanner stores relative paths
in the database, so you will need to tell the server where the music lives
(unless you specify absolute paths when scanning then you can simply use "/").

A simple example nginx config could look like:

```
  server {
    listen 12345;
    location / {
      include         fastcgi_params;
      fastcgi_pass    unix:/var/www/somepath/sock;
    }
  }
```



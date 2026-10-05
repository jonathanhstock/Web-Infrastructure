// Jonathan Stock
// COEN 162 Code Project 2 Cached Web Proxy -- corrected version
//
// What was wrong with cachedproxy.c
// ---------------------------------
//  1. The cache was never consulted. Every request was forwarded to the origin
//     server; "dateCache" was written but never read, and no If-Modified-Since
//     header was ever sent, so the proxy never got (or handled) a 304.
//  2. The response was processed one 1024-byte chunk at a time as if every
//     chunk were a full response:
//       - strstr(buffer2, "Date: ") only matches in the first chunk. On every
//         later chunk it returns NULL, and printf("%s", NULL) / memcpy(arr,
//         NULL, 35) crash the proxy (segfault) on any page bigger than 1 KB.
//       - buffer2 is never NUL-terminated, so strstr/strlen read past the end.
//       - fopen("webpage.html", "w") inside the loop truncates the file on
//         each chunk, so only the last chunk of the page was ever saved, and
//         every site shared the same single file.
//       - cacheIndex++ per chunk overflows dateCache[1][1024] on the second
//         chunk (writes past the end of the global array).
//  3. newDate is char[29] but the loop writes 35 bytes into it and reads
//     arr[i + 6] up to arr[40] of a 35-byte array: a stack buffer overflow,
//     and the result is never NUL-terminated.
//  4. write(newSocket, buffer2, 1024) always sends 1024 bytes, even when recv
//     returned fewer, so the browser receives garbage/zero bytes appended to
//     the page (proxy.c correctly used write(..., n)).
//  5. bzero(buffer2, strlen(buffer2)) relies on strlen of unterminated data.
//  6. read() of the browser request is not NUL-terminated and the buffer is
//     reused, so strcpy/strlen/strtok can pick up leftovers from an earlier,
//     longer request.
//  7. The Host header was assumed to be the 2nd line, a "host:port" value was
//     passed straight to gethostbyname, and a failed lookup (NULL) was
//     dereferenced.
//  8. The browser's keep-alive request was forwarded unchanged, so the origin
//     server holds the connection open and the recv loop can hang until the
//     server times out.
//  9. The global "int cache" is shadowed by the local "char *cache[1]".
//
// How this version works
// ----------------------
//  - Reads the full request header, finds the URL and Host header.
//  - Cache key = full URL. Each entry stores the file the response was saved
//    to and the Last-Modified (or Date) value of that response.
//  - On a cache hit it forwards the request with
//    "If-Modified-Since: <saved date>". If the server answers 304 Not Modified
//    the cached file is sent to the browser; otherwise the new response is
//    sent to the browser and replaces the cached copy.
//  - On a miss the response is streamed to the browser and saved to disk.
//  - "Connection: close" is forced so the server closes when it is done, and
//    the request line is rewritten from "GET http://host/path" (what browsers
//    send to a proxy) to "GET /path" (what origin servers expect).
//
// Build:  gcc -Wall -o cachedproxy-solution cachedproxy-solution.c
// Run:    ./cachedproxy-solution     then set the browser HTTP proxy to
//         127.0.0.1:8080, or: curl -v -x 127.0.0.1:8080 http://example.com/
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>

#define PROXY_PORT 8080
#define MAX_CACHE 16
#define REQ_SIZE 8192
#define BUF_SIZE 4096

struct cacheEntry
{
   int used;
   char url[2048];
   char date[128];    // Last-Modified (or Date) of the cached response
   char file[64];     // file the full response is saved in
};

struct cacheEntry cache[MAX_CACHE];
int nextSlot = 0;

// returns the cache entry for url, or NULL
struct cacheEntry *cacheLookup(const char *url)
{
   for (int i = 0; i < MAX_CACHE; i++)
      if (cache[i].used && strcmp(cache[i].url, url) == 0)
         return &cache[i];
   return NULL;
}

// returns a slot for url (existing one, or the next slot round-robin)
struct cacheEntry *cacheSlot(const char *url)
{
   struct cacheEntry *e = cacheLookup(url);
   if (e != NULL)
      return e;

   e = &cache[nextSlot];
   snprintf(e->file, sizeof e->file, "cache_%d.html", nextSlot);
   nextSlot = (nextSlot + 1) % MAX_CACHE;
   e->used = 0;
   return e;
}

// copies the value of header "name" out of a NUL-terminated header block
int getHeader(const char *headers, const char *name, char *out, size_t outSize)
{
   size_t nameLen = strlen(name);
   const char *line = headers;

   while (line != NULL && *line != '\0')
   {
      if (strncasecmp(line, name, nameLen) == 0 && line[nameLen] == ':')
      {
         const char *value = line + nameLen + 1;
         while (*value == ' ')
            value++;
         size_t len = strcspn(value, "\r\n");
         if (len >= outSize)
            len = outSize - 1;
         memcpy(out, value, len);
         out[len] = '\0';
         return 1;
      }
      line = strstr(line, "\r\n");
      if (line != NULL)
         line += 2;
   }
   return 0;
}

// write all n bytes (write() may write fewer than asked)
int writeAll(int fd, const char *buf, size_t n)
{
   while (n > 0)
   {
      ssize_t w = write(fd, buf, n);
      if (w <= 0)
         return -1;
      buf += w;
      n -= w;
   }
   return 0;
}

void sendError(int fd, const char *status)
{
   char msg[256];
   snprintf(msg, sizeof msg,
            "HTTP/1.0 %s\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s\n",
            status, status);
   writeAll(fd, msg, strlen(msg));
}

// connect to host:port, returns socket or -1
int connectToServer(const char *host, const char *port)
{
   struct addrinfo hints, *res, *p;
   int sock = -1;

   memset(&hints, 0, sizeof hints);
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_STREAM;

   if (getaddrinfo(host, port, &hints, &res) != 0)
      return -1;

   for (p = res; p != NULL; p = p->ai_next)
   {
      sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (sock < 0)
         continue;
      if (connect(sock, p->ai_addr, p->ai_addrlen) == 0)
      {
         printf("Connected to %s (%s)\n", host,
                inet_ntoa(((struct sockaddr_in *)p->ai_addr)->sin_addr));
         break;
      }
      close(sock);
      sock = -1;
   }
   freeaddrinfo(res);
   return sock;
}

// sends the cached file to the browser
int sendCachedFile(int fd, const char *file)
{
   FILE *fp = fopen(file, "rb");
   if (fp == NULL)
      return -1;

   char buf[BUF_SIZE];
   size_t n;
   while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
      if (writeAll(fd, buf, n) < 0)
         break;
   fclose(fp);
   return 0;
}

void handleClient(int newSocket)
{
   char request[REQ_SIZE];
   int total = 0;
   int n;

   // read the whole request header (until the blank line)
   while (total < REQ_SIZE - 1)
   {
      n = read(newSocket, request + total, REQ_SIZE - 1 - total);
      if (n <= 0)
         break;
      total += n;
      request[total] = '\0';
      if (strstr(request, "\r\n\r\n") != NULL)
         break;
   }
   request[total] = '\0';
   if (total == 0 || strstr(request, "\r\n\r\n") == NULL)
      return;

   // request line: METHOD URL VERSION
   char method[16], url[2048], version[16];
   if (sscanf(request, "%15s %2047s %15s", method, url, version) != 3)
   {
      sendError(newSocket, "400 Bad Request");
      return;
   }
   printf("\nRequest: %s %s\n", method, url);

   if (strcmp(method, "CONNECT") == 0)
   {
      // HTTPS tunnels are not supported by this proxy
      sendError(newSocket, "501 Not Implemented");
      return;
   }

   // Host header, split into host and port
   char hostHeader[256], host[256], port[8] = "80";
   if (!getHeader(request, "Host", hostHeader, sizeof hostHeader))
   {
      sendError(newSocket, "400 Bad Request");
      return;
   }
   strcpy(host, hostHeader);
   char *colon = strchr(host, ':');
   if (colon != NULL)
   {
      *colon = '\0';
      snprintf(port, sizeof port, "%s", colon + 1);
   }
   printf("Host: %s Port: %s\n", host, port);

   // only GET responses are cached; the key is host + url
   char key[2048];
   snprintf(key, sizeof key, "%s%s", url[0] == '/' ? host : "", url);
   int cacheable = (strcmp(method, "GET") == 0);
   struct cacheEntry *entry = cacheable ? cacheLookup(key) : NULL;

   if (entry != NULL)
      printf("Cache HIT for %s (saved %s), asking server if modified\n", key, entry->date);
   else
      printf("Cache MISS for %s\n", key);

   // rebuild the request: keep the browser's headers except the ones about
   // connections and conditional requests, which the proxy controls
   char outReq[REQ_SIZE + 512];
   int outLen = 0;
   char *line;
   char *end = strstr(request, "\r\n\r\n");

   // browsers send "GET http://host/path" to a proxy; the server expects "GET /path"
   const char *path = url;
   if (strncasecmp(url, "http://", 7) == 0)
   {
      path = strchr(url + 7, '/');
      if (path == NULL)
         path = "/";
   }
   outLen += snprintf(outReq, sizeof outReq, "%s %s %s\r\n", method, path, version);
   line = strstr(request, "\r\n") + 2;

   while (line < end + 2)
   {
      char *eol = strstr(line, "\r\n");
      int len = eol - line;
      int skip = (strncasecmp(line, "Connection:", 11) == 0 ||
                  strncasecmp(line, "Proxy-Connection:", 17) == 0 ||
                  strncasecmp(line, "Keep-Alive:", 11) == 0 ||
                  strncasecmp(line, "If-Modified-Since:", 18) == 0 ||
                  strncasecmp(line, "If-None-Match:", 14) == 0);
      if (!skip)
         outLen += snprintf(outReq + outLen, sizeof outReq - outLen, "%.*s\r\n", len, line);
      line = eol + 2;
   }
   if (entry != NULL)
      outLen += snprintf(outReq + outLen, sizeof outReq - outLen,
                         "If-Modified-Since: %s\r\n", entry->date);
   outLen += snprintf(outReq + outLen, sizeof outReq - outLen, "Connection: close\r\n\r\n");

   int clientSocket = connectToServer(host, port);
   if (clientSocket < 0)
   {
      printf("Could not connect to %s\n", host);
      sendError(newSocket, "502 Bad Gateway");
      return;
   }

   // forward the request (plus any body bytes already read, e.g. for POST)
   char *body = end + 4;
   writeAll(clientSocket, outReq, outLen);
   if (body < request + total)
      writeAll(clientSocket, body, request + total - body);

   // read the response header first so we can look at the status and dates
   char header[BUF_SIZE + 1];
   int hlen = 0;
   char *hend = NULL;
   while (hlen < BUF_SIZE)
   {
      n = recv(clientSocket, header + hlen, BUF_SIZE - hlen, 0);
      if (n <= 0)
         break;
      hlen += n;
      header[hlen] = '\0';
      if ((hend = strstr(header, "\r\n\r\n")) != NULL)
         break;
   }
   header[hlen] = '\0';

   int status = 0;
   sscanf(header, "HTTP/%*s %d", &status);
   printf("Server status: %d\n", status);

   if (status == 304 && entry != NULL)
   {
      // not modified: answer from the cache
      printf("Not modified, sending cached copy from %s\n", entry->file);
      if (sendCachedFile(newSocket, entry->file) < 0)
         sendError(newSocket, "500 Internal Server Error");
      close(clientSocket);
      return;
   }

   // save 200 responses to GET requests in the cache
   FILE *fp = NULL;
   char tmpFile[80];
   struct cacheEntry *slot = NULL;
   if (cacheable && status == 200 && hend != NULL)
   {
      slot = cacheSlot(key);
      snprintf(tmpFile, sizeof tmpFile, "%s.tmp", slot->file);
      fp = fopen(tmpFile, "wb");
   }

   // send what we already have, then stream the rest
   int ok = 1;
   if (writeAll(newSocket, header, hlen) < 0)
      ok = 0;
   if (fp != NULL)
      fwrite(header, 1, hlen, fp);

   char buffer2[BUF_SIZE];
   int bytes_read;
   while ((bytes_read = recv(clientSocket, buffer2, sizeof buffer2, 0)) > 0)
   {
      if (fp != NULL)
         fwrite(buffer2, 1, bytes_read, fp);
      if (ok && writeAll(newSocket, buffer2, bytes_read) < 0)
         ok = 0;    // browser went away; keep reading so the cache copy is complete
   }

   if (fp != NULL)
   {
      fclose(fp);
      if (bytes_read == 0 && rename(tmpFile, slot->file) == 0)
      {
         // remember the date to use in If-Modified-Since next time
         *hend = '\0';
         if (!getHeader(header, "Last-Modified", slot->date, sizeof slot->date))
            getHeader(header, "Date", slot->date, sizeof slot->date);
         snprintf(slot->url, sizeof slot->url, "%s", key);
         slot->used = 1;
         printf("Saved %s to %s (date: %s)\n", key, slot->file, slot->date);
      }
      else
      {
         remove(tmpFile);
      }
   }

   close(clientSocket);
}

int main()
{
   int welcomeSocket, newSocket;
   struct sockaddr_in serverAddr;
   struct sockaddr_storage serverStorage;
   socklen_t addr_size;

   // a browser closing early must not kill the proxy
   signal(SIGPIPE, SIG_IGN);
   setvbuf(stdout, NULL, _IOLBF, 0);

   welcomeSocket = socket(PF_INET, SOCK_STREAM, 0);
   int yes = 1;
   setsockopt(welcomeSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

   memset(&serverAddr, 0, sizeof serverAddr);
   serverAddr.sin_family = AF_INET;
   serverAddr.sin_port = htons(PROXY_PORT);
   serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

   if (bind(welcomeSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
   {
      perror("bind");
      exit(1);
   }
   if (listen(welcomeSocket, 5) < 0)
   {
      perror("listen");
      exit(1);
   }
   printf("Proxy IP: 127.0.0.1 Port: %d\nListening\n", PROXY_PORT);

   while (1)
   {
      addr_size = sizeof(serverStorage);
      newSocket = accept(welcomeSocket, (struct sockaddr *)&serverStorage, &addr_size);
      if (newSocket < 0)
         continue;
      handleClient(newSocket);
      close(newSocket);
   }
   return 0;
}

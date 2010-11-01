#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <stdexcept>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <string>

#define DEBUG_PRINTF printf
//#define DEBUG_PRINTF(x, ...)

#define DEFAULT_TIMEOUT 55 // seconds

#define RESPONSE_PRELUDE "}" // Any string you want to send directly before the JSON dict in the response payload.


std::map<std::string, std::string> static_resources;

class PollFD;
class ClientFD;

struct eventID {
  eventID() : id(0) {}
  eventID(const eventID& right) : id(right.id) {}
  eventID& operator=(const eventID& right) { id = right.id; return *this; }
  bool operator<(const eventID& right) const { return id < right.id; }
  bool operator==(const eventID& right) const { return id == right.id; }
  bool empty() const { return (id == 0); }
  eventID next() const { eventID e; e.id = id + 1; return e; }

  char* json_serialize(char* buf) const {
    int s = sprintf(buf, "\"%llu\"", id);
    return buf + s;
  }

  char* json_deserialize(char* buf) {
    if (*buf == '\'' || *buf == '"') ++buf;
    char* p = NULL;
    id = strtoul(buf, &p, 10);
    if (*p == '\'' || *p == '"') ++p;
    return p;
  }

  size_t json_serialize_space_needed() const { // can be larger, but never smaller, than the number of bytes json_serialize() will put in the buffer
    return 32;
  }

  uint64_t id;
};

struct versionID : public eventID {
  char* json_serialize(char* buf) const {
    int s = sprintf(buf, "%llu", id);
    return buf + s;
  }

  char* json_deserialize(char* buf) {
    char* p = NULL;
    id = strtoul(buf, &p, 10);
    return p;
  }
};

struct eventmap_value_t {
  eventmap_value_t() {}
  eventmap_value_t(const versionID& _v) : version(_v) {}

  versionID version;
  std::set<ClientFD*> subscribers;
  void subscribe(ClientFD* cfd) {
    subscribers.insert(cfd);
  }
  void unsubscribe(ClientFD* cfd) {
    subscribers.erase(cfd);
  }
};

std::map<eventID, eventmap_value_t> eventmap;

void update_event(const eventID& e, versionID& v);
int subscribe_port = 9090;
int notify_port = 9091;

std::list<PollFD*> pollfds_to_enqueue;

void enqueue_fd(PollFD* pfd) { pollfds_to_enqueue.push_back(pfd); }

struct timeout_queue_entry {
  ClientFD* cfd;
  time_t exptime;
  timeout_queue_entry* prev;
  timeout_queue_entry* next;
};

timeout_queue_entry *tq_front = NULL, *tq_back = NULL;

void timeout_dequeue(timeout_queue_entry* tqe) {
  if (tq_front == tqe) tq_front = tqe->next;
  if (tq_back == tqe) tq_back = tqe->prev;

  if (tqe->prev) tqe->prev->next = tqe->next;
  if (tqe->next) tqe->next->prev = tqe->prev;

  delete tqe;
}

timeout_queue_entry* timeout_enqueue(ClientFD* cfd, time_t timeout_seconds = DEFAULT_TIMEOUT) {
  timeout_queue_entry* tqe = new timeout_queue_entry;
  tqe->cfd = cfd;
  tqe->exptime = time(NULL) + timeout_seconds;
  tqe->next = NULL;

  tqe->prev = tq_back;
  if (tq_back) {
    tq_back->next = tqe;
  }
  tq_back = tqe;
  if (! tq_front) tq_front = tqe;
  return tqe;
}

class FmtException : public std::exception {
public:
  FmtException() throw() { msg[0] = '\0'; }
  FmtException(const char* fmt, ...) throw() {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, 224, fmt, ap);
    va_end(ap);
  }
  virtual const char* what() const throw() {
    return msg;
  }
  virtual ~FmtException() throw() {}
protected:
  char msg[224];
} ;

class PollFD {
public:
  virtual ~PollFD() {
    DEBUG_PRINTF("in dtor for pollFD 0x%p fd=%d\n", this, fd);
    if (fd >= 0) close(fd);
  }
  virtual int get_fd() {
    return fd;
  }
  virtual int get_epoll_event() {
    return EPOLLIN;
  }
  operator struct epoll_event*() {
    static struct epoll_event ee;
    ee.events = this->get_epoll_event();
    ee.data.ptr = this;
    return &ee; 
  }
  // process_events returns a flag that enumerates if we should continue to process events on this socket, if we should dequeue it and wait until the event monitor reenqueues it if we want to discard it
  virtual int process_events(short revents) = 0;
  static const int EVENT_RESPONSE_DROP = -1;
  static const int EVENT_RESPONSE_CONTINUE = 0;
  static const int EVENT_RESPONSE_DEQUEUE = 1;
  static const int EVENT_RESPONSE_CHANGE_EVENT = 2;
protected:
  int fd;
};

class ClientFD : public PollFD {
public:
  ClientFD(int _fd, bool _can_post_events) : can_post_events(_can_post_events), should_close_after_send(false), http_onepointone(true), state(STATE_READ_REQUEST), buffer(static_cast<char*>(malloc(1024))), buffer_size(1024), buffer_used(0), send_ptr(0), request_content_length(-1), tqe(NULL) {
    fd = _fd;
  }

  ~ClientFD() {
    DEBUG_PRINTF("in dtor for clientFD 0x%p fd=%d\n", this, fd);
    for (std::set<eventID>::iterator it = subscribed_events.begin(); it != subscribed_events.end(); ++it) {
      eventmap[*it].unsubscribe(this);
    }
    subscribed_events.clear();
    if (tqe) timeout_dequeue(tqe);
    free(buffer);
    // ~PollFD() handles cloing the FD
  }

  virtual int get_epoll_event() {
    return ((state == STATE_READ_REQUEST) ? EPOLLIN : EPOLLOUT); // IN for read_request, OUT for send_response or awoken (which transitions right into send_response)
  }

  virtual void awakeForTimeout() {
    // timeout entry already removed if we were called from it
    tqe = NULL;
    for (std::set<eventID>::iterator it = subscribed_events.begin(); it != subscribed_events.end(); ++it) {
      eventmap[*it].unsubscribe(this);
    }
    subscribed_events.clear();
    serialize_and_send_response();
    enqueue_fd(this);
  }

  virtual void awakeWithID(const eventID& id, const versionID& version, bool reenqueue = true) {
    if (tqe) timeout_dequeue(tqe);
    tqe = NULL;

    events.insert(std::make_pair(id, version));
    state = STATE_AWOKEN;
    if (reenqueue) enqueue_fd(this);
  }

  void transition_to_send_state() {
    request_content_length = -1;
    send_ptr = 0;
    state = STATE_SEND_RESPONSE;
    buffer[buffer_used] = '\0';
    DEBUG_PRINTF("sending response, length %zu: \"%s\"\n", buffer_used, buffer);
  }

  void send_response(const char* content_type, const char* response, size_t response_length, bool allow_cache) {

    if (buffer_size < (response_length+ 512)) {
      buffer_size = response_length  + 512;
      buffer = (char*) realloc(buffer, buffer_size);
    }

    const char* cache_headers =
      (allow_cache ? 
        ("Expires: Mon, 26 Jul 2027 05:00:00 GMT\r\n")
      :
        ("Expires: Mon, 26 Jul 1997 05:00:00 GMT\r\n"
         "Cache-Control: private, no-store, no-cache, must-revalidate, post-check=0, pre-check=0\r\n"
         "Pragma: no-cache\r\n")
      );

    size_t header_len = sprintf(buffer,
      "HTTP/%s 200 OK\r\n"
      "%s"
      "X-Served-By: enotify\r\n"
      "Content-Type: %s\r\n"
      "Connection: %s\r\n"
      "Content-Length: %zu\r\n"
      "\r\n", http_onepointone ? "1.1" : "1.0", cache_headers, content_type, http_onepointone ? "keep-alive" : "close", response_length);
    memcpy(buffer + header_len, response, response_length);
    buffer_used = header_len + response_length;

    transition_to_send_state();
  }

  void serialize_and_send_response() {
    size_t response_max = 1024;
    size_t response_used = 0;
    char* response = (char*)malloc(response_max);

    size_t jsonp_second_part_offset = std::string::npos;

    if (jsonp.empty()) {
      strcpy(response, RESPONSE_PRELUDE);
      response_used = strlen(RESPONSE_PRELUDE);
    } else {
      jsonp_second_part_offset = jsonp.find("%1%");
      if (jsonp_second_part_offset == std::string::npos) {
        strcpy(response, jsonp.c_str());
        response_used = jsonp.size();
      } else {
        memcpy(response, jsonp.c_str(), jsonp_second_part_offset);
        response_used = jsonp_second_part_offset;
        jsonp_second_part_offset += 3; // skip over %1%
      }
    }
    bool first = true;
    response[response_used++] = '{';
    for (std::map<eventID, versionID>::const_iterator it = events.begin(); it != events.end(); ++it) {
      size_t space_needed = it->first.json_serialize_space_needed() + it->second.json_serialize_space_needed() + 4;
      if (response_used + space_needed >= response_max) {
        response_max += std::max<size_t>(1024, space_needed);
        response = (char*)realloc(response, response_max);
      }
      char* p = response + response_used;
      if (!first) *p++ = ',';
      first = false;
      p = it->first.json_serialize(p);
      *p++ = ':';
      p = it->second.json_serialize(p);
      response_used = p - response;
    }
    response[response_used++] = '}';

    if (jsonp_second_part_offset != std::string::npos) {
      size_t second_part_size = jsonp.size() - jsonp_second_part_offset;

      if (response_used + second_part_size >= response_max) {
        response_max += std::max<size_t>(1024, second_part_size);
        response = (char*)realloc(response, response_max);
      }
      
      memcpy(response + response_used, jsonp.c_str() + jsonp_second_part_offset, second_part_size);
      response_used += second_part_size;
    }

    send_response("text/javascript", response, response_used, false);

    free(response);
  }

  int parse_http_request() {
    // This has three tasks to complete:
    // first, ensure that the request is complete
    // second, grab the request type (subscribe or notify)
    // third, decode the event ID -> version map and stash it in this->events

    if (buffer_used < 32) return REQUEST_INCOMPLETE;
    buffer[buffer_used] = '\0'; // ensure it's null terminated at least

    if (! strstr(buffer, "\r\n\r\n")) {
      DEBUG_PRINTF("Didn't find terminating \\r\\n\\r\\n, request incomplete?\n");
      return REQUEST_INCOMPLETE;
    }

    DEBUG_PRINTF("parsing request, length %zu: \"%s\"\n", buffer_used, buffer);
    // the saddest thing: c++ as a string mangling engine
    if (!strstr(buffer, "HTTP/1.1")) http_onepointone = false;
    should_close_after_send = !http_onepointone;

    if ((memcmp(buffer, "GET", 3) == 0) &&
        (memcmp(buffer, "GET /subscribe?", 14) != 0))  {
      // GET request: static resource.
      strtok(buffer, " ");
      char* static_uri = strtok(NULL, " ?\r\n");
      std::map<std::string, std::string>::iterator it = static_resources.find(static_uri);
      if (it == static_resources.end()) throw FmtException("Requested URI not found.");
      send_response("text/html", it->second.c_str(), it->second.size(), true);
      return REQUEST_HANDLED;
    }

    bool doing_post_request = (memcmp(buffer, "POST ", 5) == 0);

    if (! doing_post_request) {

      char* p = strstr(buffer, "/subscribe?");
      if (! p) {
        throw std::runtime_error("Unable to parse request: couldn't find GET vars.");
      }
      while (*p != '?') ++p; ++p; // skip to after the '?', the GET vars

      while (*p) {
        if (*p == 'j') {
          while (*p && *p != '=') ++p; ++p;
          char* jsonp_start = p;
          while (*p && *p != '&' && *p != ' ') ++p;
          jsonp.clear();
          jsonp.reserve(p - jsonp_start);
          for (; jsonp_start != p; ++jsonp_start) {
            if (*jsonp_start == '%') {
              jsonp_start++;
              char c[4];
              c[0] = *(jsonp_start++);
              c[1] = *(jsonp_start);
              c[2] = '\0';
              jsonp.push_back(strtol(c, NULL, 16));
            } else jsonp.push_back(*jsonp_start);
          }

          DEBUG_PRINTF("jsonp = \"%s\"\n", jsonp.c_str());
        } else if (*p == '&' || isspace(*p)) {
          ++p;
          continue;
        } else {
          eventID e;
          versionID v;
          p = e.json_deserialize(p);
          while (*p && *p != '=') ++p;
          if (!*p) break;
          ++p;
          p = v.json_deserialize(p);
          events.insert(std::make_pair(e, v));
        }
      }
    }

    // Check for "Connection: close" or something similar
    char* connection_header = strstr(buffer, "Connection:");
    if (connection_header) {
      char* p = connection_header + 11; // "Connection: "
      char buf[256]; char* bufptr = buf;
      while (*p != '\n' && *p != '\r' && *p) *bufptr++ = *p++;
      *bufptr = '\0';
      if (strstr(buf, "close")) should_close_after_send = true;
      DEBUG_PRINTF("found connection header \"%s\", close=%d\n", buf, should_close_after_send);
    }

    if (doing_post_request) {
      char* content_length_start = strstr(buffer, "Content-Length");
      if (content_length_start) {
        char* p = content_length_start;
        while (*p && *p != ' ') ++p;
        if (*p != '\0') {
          request_content_length = strtol(p, &p, 10);
        }
      }
      char* content_start = strstr(buffer, "\r\n\r\n");
      if (! content_start) content_start = strstr(buffer, "\n\n");
      if (! content_start) {
        DEBUG_PRINTF("unable to find content_start\n");
        return REQUEST_INCOMPLETE;
      }
      if (request_content_length >= 0) {
        DEBUG_PRINTF("found content length = %zu\n", request_content_length);
        size_t content_start_offset = content_start - buffer;
        size_t current_content_length = buffer_used - content_start_offset;
        if (current_content_length < (size_t)request_content_length) return REQUEST_INCOMPLETE;
      } else {
        DEBUG_PRINTF("no content length found in header\n");
      }
      
      // we have content, deserialize it into the events map
      char* p = content_start;
      while (*p && *p != '{') ++p;
      if (!*p) throw std::runtime_error("Malformed content: cannot find opening '{' for json object");
      while (*p && *p != '}') {
        ++p; // skip the preceding '{' or ','
        eventID e;
        versionID v;
        p = e.json_deserialize(p);
        while (*p && *p != ':') ++p;
        if (!*p) break;
        ++p; // skip :
        p = v.json_deserialize(p);
        events.insert(std::make_pair(e, v));
        while (*p && *p != ',' && *p != '}') ++p;
      }
      strtok(buffer, " ");
      char* uri = strtok(NULL, " \r\n");
      if (strstr(uri, "subscribe")) return REQUEST_SUBSCRIBE;
      else if (strstr(uri, "notify")) return REQUEST_NOTIFY;
      else throw FmtException("Unknown request URI \"%s\", not subscribe or notify", uri);
    } else return REQUEST_SUBSCRIBE;
  }

  virtual int process_events(short revents) {
    DEBUG_PRINTF("processing events fd=%d revents=%d state=%d\n", fd, revents, state);
    try {
      if (state == STATE_READ_REQUEST) {
        if (revents & EPOLLHUP) return EVENT_RESPONSE_DROP;

        int read_size = (buffer_size - buffer_used);
        if (!read_size) {
          buffer_size *= 2;
          if (buffer_size > max_request_size) throw std::runtime_error("max_request_size exceeded");
          buffer = static_cast<char*>(realloc(buffer, buffer_size));
          read_size = buffer_size - buffer_used;
        }
        ssize_t res = read(fd, buffer + buffer_used, read_size);
        if (res < 0) {
          if (errno == EAGAIN) {
            return EVENT_RESPONSE_CONTINUE; // unable to read more data, just reenqueue
          } else {
            return EVENT_RESPONSE_DROP; // other read error, drop conn
          }
        } else if (res == 0) return EVENT_RESPONSE_DROP; // res = 0 = connection closed, drop this & don't reenqueue
        buffer_used += res;

        int reqtype = parse_http_request();
        DEBUG_PRINTF("parse_http_request() response = %d\n", reqtype);
        if (reqtype == REQUEST_INCOMPLETE) return EVENT_RESPONSE_CONTINUE; // incomplete request, read more data
        else if (reqtype == REQUEST_HANDLED) return EVENT_RESPONSE_CHANGE_EVENT; // this request was handled and a state transition made
        else if (reqtype == REQUEST_NOTIFY) {
          if (! can_post_events) throw std::runtime_error("Not authorized");
          for (std::map<eventID, versionID>::iterator it = events.begin(); it != events.end(); ++it) {
            update_event(it->first, it->second);
          }
          serialize_and_send_response();
          return EVENT_RESPONSE_CHANGE_EVENT;
        } else { // reqtype == REQUEST_SUBSCRIBE
          state = STATE_WAITING_FOR_EVENTS;
          bool immediate_awake = false;
          std::map<eventID, versionID> events_copy;
          events_copy.swap(events);
          DEBUG_PRINTF("read subscribe request. events are:\n");
          for (std::map<eventID, versionID>::iterator it = events_copy.begin(); it != events_copy.end(); ++it) {
            DEBUG_PRINTF("%llu -> %llu\n", it->first.id, it->second.id);
            std::map<eventID, eventmap_value_t>::iterator evit = eventmap.find(it->first);
            if ((evit != eventmap.end()) && (!(evit->second.version == it->second))) {
              awakeWithID(it->first, evit->second.version, false);
              immediate_awake = true;
              DEBUG_PRINTF("immediate awake for event %llu new version %llu\n", it->first.id, evit->second.version.id);
            } else {
              if (evit == eventmap.end()) {
                evit = eventmap.insert(std::make_pair(it->first, eventmap_value_t(it->second))).first;
              }
              evit->second.subscribe(this);
              subscribed_events.insert(it->first);
            }
          }

          if (immediate_awake) {
            return EVENT_RESPONSE_CHANGE_EVENT;
          } else {
            tqe = timeout_enqueue(this);
            return EVENT_RESPONSE_DEQUEUE;
          }
        }
      }
      else if (state == STATE_SEND_RESPONSE) {
        if (revents & EPOLLHUP) return EVENT_RESPONSE_DROP;

        ssize_t res = write(fd, buffer + send_ptr, buffer_used - send_ptr);
        if (res == -1) {
          DEBUG_PRINTF("write error fd=%d error=%s\n", fd, strerror(errno));
          if (errno == EAGAIN) {
            return EVENT_RESPONSE_CONTINUE;
          } else {
            return EVENT_RESPONSE_DROP;
          }
        }
        send_ptr += res;
        DEBUG_PRINTF("sent %zd bytes on fd %d, new send ptr is %zd, bufsize=%zu\n", res, fd, send_ptr, buffer_used);
        if ((buffer_used - send_ptr) == 0) { // done sending response, prepare for next request
          DEBUG_PRINTF("response send complete should_close_after_send=%d\n", should_close_after_send);
          if (should_close_after_send) {
            return EVENT_RESPONSE_DROP;
          }
          state = STATE_READ_REQUEST;
          buffer_used = 0;
          events.clear();
          subscribed_events.clear();
          return EVENT_RESPONSE_CHANGE_EVENT;
        }
        return EVENT_RESPONSE_CONTINUE;
      } else if (state == STATE_AWOKEN || state == STATE_WAITING_FOR_EVENTS) {
        if (tqe) timeout_dequeue(tqe);
        tqe = NULL;
        for (std::set<eventID>::iterator it = subscribed_events.begin(); it != subscribed_events.end(); ++it) {
          eventmap[*it].unsubscribe(this);
        }
        subscribed_events.clear();

        if (revents & EPOLLHUP) return EVENT_RESPONSE_DROP;

        serialize_and_send_response();
      } else {
        DEBUG_PRINTF("awoke client FD in unknown state %d\n", state);
        throw std::runtime_error("unknown state");
      }

    } catch (const std::exception& e) {
      if (state == STATE_SEND_RESPONSE) return EVENT_RESPONSE_DROP; // just drop it if we were already sending a response.
      if (buffer_size < 1024) {
        free(buffer);
        buffer = static_cast<char*>( malloc(1024));
        buffer_size = 1024;
      }
      DEBUG_PRINTF("process_events xcpt: %s\n", e.what());
      buffer_used = snprintf(buffer, 1024,
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Expires: Mon, 26 Jul 1997 05:00:00 GMT\r\n"
        "Cache-Control: private, no-store, no-cache, must-revalidate, post-check=0, pre-check=0\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s\r\n", e.what());
      should_close_after_send = true;
      transition_to_send_state();
      return EVENT_RESPONSE_CHANGE_EVENT;
    }
    return EVENT_RESPONSE_CONTINUE;
  }

protected:
  static const size_t max_request_size = 32768;

  static const int STATE_READ_REQUEST = 1;
  static const int STATE_SEND_RESPONSE = 2;
  static const int STATE_WAITING_FOR_EVENTS = 3;
  static const int STATE_AWOKEN = 4;

  static const int REQUEST_INCOMPLETE = -1;
  static const int REQUEST_SUBSCRIBE = 1;
  static const int REQUEST_NOTIFY = 2;
  static const int REQUEST_HANDLED = 3;

  bool can_post_events;
  bool should_close_after_send;
  bool http_onepointone;
  int state;
  char* buffer;
  size_t buffer_size;
  size_t buffer_used;
  size_t send_ptr;
  ssize_t request_content_length;

  std::map<eventID, versionID> events;
  std::set<eventID> subscribed_events;
  std::string jsonp;
  timeout_queue_entry* tqe;
};

class ListenFD : public PollFD {
public:
  static const int queue_depth = 256;
  ListenFD(int _port, bool _can_post_events, const char* listen_address) : port(_port), can_post_events(_can_post_events) {
    fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd == -1) throw FmtException("socket(): %s", strerror(errno));

    int i = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(int));
    fcntl(fd, F_SETFL, O_NONBLOCK);


    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(struct sockaddr_in));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    if (listen_address) inet_aton(listen_address, &(local_addr.sin_addr));
    else local_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*) &local_addr, sizeof(struct sockaddr)) == -1) {
      const char* binderr = strerror(errno);
      close(fd);
      throw FmtException("bind(): %s", binderr);
    }

    if (listen(fd, queue_depth) == -1) {
      const char* listenerr = strerror(errno);
      close(fd);
      throw FmtException("listen(): %s", listenerr);
    }
  }

  virtual int process_events(short revents) {
    int cfd = -1;
    while ((cfd = accept(fd, NULL, NULL)) >= 0) {
      fcntl(cfd, F_SETFL, O_NONBLOCK);
      enqueue_fd(new ClientFD(cfd, can_post_events));
    }
    return EVENT_RESPONSE_CONTINUE;
  }
protected:
  int port;
  bool can_post_events;
};

void update_event(const eventID& e, versionID& v) {
  DEBUG_PRINTF("update_event eid=%llu version=%llu\n", e.id, v.id);
  eventmap_value_t& ev = eventmap[e];
  /*
  if (v.empty()) { // empty/0 just increments the version
    v = ev.version.next();
  } else if (v == ev.version) {
    return;
  } else if (v < ev.version) {
    v = ev.version;
    return;
  }*/
  if (ev.version == v) return; // do not wake subscribers if the data doesn't actually change.
  ev.version = v;
  DEBUG_PRINTF("update_event waking %zu subscribers\n", ev.subscribers.size());
  for (std::set<ClientFD*>::iterator it = ev.subscribers.begin(); it != ev.subscribers.end(); ++it) {
    (*it)->awakeWithID(e, v);
  }
}

int main(int argc, char* argv[]) {
  const struct rlimit r_core = {4*1000*1000*1000LL, 4*1000*1000*1000LL};
  setrlimit(RLIMIT_CORE, &r_core);
  signal(SIGPIPE, SIG_IGN);

  // increase max FDs
  struct rlimit fdmaxrl;
  int max_fds = (1<<20);
  for(fdmaxrl.rlim_cur = max_fds, fdmaxrl.rlim_max = max_fds;
      max_fds && (setrlimit(RLIMIT_NOFILE, &fdmaxrl) < 0);
      fdmaxrl.rlim_cur = max_fds, fdmaxrl.rlim_max = max_fds) {
    max_fds /= 2;
  }
  openlog("enotify", LOG_PID | LOG_NDELAY, LOG_DAEMON);

  bool nosetuid = false;
  bool daemonize = false;
  char* pidfile = NULL;
  char* listen_address = "127.0.0.1";
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--daemonize") == 0) {
      daemonize = true;
    } else if (strcmp(argv[i], "--pidfile") == 0) {
      pidfile = argv[++i];
    } else if (strcmp(argv[i], "--nosetuid") == 0) {
      nosetuid = true;
    } else if (strcmp(argv[i], "--static") == 0) {
      if ((i + 2) > argc) {
        printf("--static: expected arguments: static-uri, filename\n");
        exit(-1);
      }
      const char* static_uri = argv[++i];
      const char* static_path = argv[++i];
      FILE* fp = fopen(static_path, "rb"); 
      if (! fp) {
        printf("Unable to open static resource file \"%s\"\n", static_path);
        exit(-1);
      }
      fseek(fp, 0, SEEK_END);
      long flen = ftell(fp);
      char* buf = new char[flen];
      fseek(fp, 0, SEEK_SET);
      fread(buf, flen, 1, fp);
      fclose(fp);
      static_resources.insert(std::make_pair(std::string(static_uri), std::string(buf, flen)));
      delete[] buf;
    } else if (strcmp(argv[i], "--notify-port") == 0) {
      if ((i + 1) >= argc) {
        printf("--notify-port: expected argument: int port\n");
        exit(-1);
      }
      notify_port = atoi(argv[++i]);
    } else {
      listen_address = argv[i];
      char* p = strtok(listen_address, ":");
      p = strtok(NULL, ":");
      if (p) subscribe_port = atoi(p);
    }
  }

  try {
    ListenFD* subscribe_fd = new ListenFD(subscribe_port, false, listen_address);
    ListenFD* notify_fd = new ListenFD(notify_port, true, "127.0.0.1");

    int epfd = epoll_create(16384);
    if (epfd < 0) {
      printf("Unable to create epoll fd: %s\n", strerror(errno));
      exit(-1);
    }

    if (daemonize) {
      // kill stdin/stdout/stderr
      int devnullfd = fileno(fopen("/dev/null", "rw"));
      dup2(devnullfd, 0);
      dup2(devnullfd, 1);
      dup2(devnullfd, 2);
      pid_t p = fork();
      if (p) {
        if (pidfile) {
          FILE* fp = fopen(pidfile, "w");
          if (! fp) {
            printf("unable to create PID file %s\n", pidfile);
          } else {
            fprintf(fp, "%d", p);
            fclose(fp);
          }
        }
        exit(0); // kill parent process
      }
      setsid();

      // get out of whatever dir we were in, in preparation for setuid
      chdir("/tmp");
      if (! nosetuid) {
        // become nobody
        setuid(65534);
        setgid(65534);
      }
    }

    const size_t revent_max = 128;
    struct epoll_event* revents = new struct epoll_event[revent_max];
    epoll_ctl(epfd, EPOLL_CTL_ADD, subscribe_fd->get_fd(), *subscribe_fd);
    epoll_ctl(epfd, EPOLL_CTL_ADD, notify_fd->get_fd(), *notify_fd);

    syslog(LOG_INFO, "enotify: startup complete");

    while (true) {
      int res = epoll_wait(epfd, revents, revent_max, 1000);
      if (res == -1 && errno == EINTR) continue;
      if (res < 0) throw FmtException("epoll() failed: %s", strerror(errno));
      for (int ev = 0; ev < res; ++ev) {
        PollFD* pfd = (PollFD*)revents[ev].data.ptr;
        int process_res = pfd->process_events(revents[ev].events);
        if (process_res == PollFD::EVENT_RESPONSE_CHANGE_EVENT) {
          epoll_ctl(epfd, EPOLL_CTL_MOD, pfd->get_fd(), *pfd);
        }
        else if (process_res == PollFD::EVENT_RESPONSE_DEQUEUE || process_res == PollFD::EVENT_RESPONSE_DROP) {
          epoll_ctl(epfd, EPOLL_CTL_DEL, pfd->get_fd(), *pfd); 
          if (process_res == PollFD::EVENT_RESPONSE_DROP) {
            delete pfd;
          }
        }
      }
      // after processsing any results from polling, check the timeout list
      time_t now = time(NULL);
      while (tq_front) {
        if (tq_front->exptime <= now) {
          ClientFD* cfd = tq_front->cfd;
          DEBUG_PRINTF("awoke cfd %p for timeout\n", cfd);
          timeout_queue_entry* tq_front_next = tq_front->next;
          delete tq_front;

          if (tq_front_next) tq_front_next->prev = NULL;
          else tq_back = NULL;
          tq_front = tq_front_next;
          cfd->awakeForTimeout();
        } else break; // should be sorted, so once we hit one nonexpired entry we're done
      }
      // and now add any client FDs that were generated as a result of earlier processing
      while (! pollfds_to_enqueue.empty()) {
        PollFD* pfd = *pollfds_to_enqueue.begin();
        pollfds_to_enqueue.erase(pollfds_to_enqueue.begin());
        epoll_ctl(epfd, EPOLL_CTL_ADD, pfd->get_fd(), *pfd);
      }
    }

  } catch (const std::exception& e) {
    if (! daemonize) printf("exception in main(): %s\n", e.what());
    syslog(LOG_ERR, "enotify: exception in main(): %s", e.what());
    return -1;
  }
  return 0;
}


#include <apr.h>
#include <assert.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <event2/util.h>
#include <jansson.h>
#include <libspotify/api.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svn_diff.h>
#include <sys/queue.h>

#include "diff.h"

// Application key
extern const unsigned char g_appkey[]; 
extern const size_t g_appkey_size; 

// Account information
extern const char username[];
extern const char password[];

static int exit_status = EXIT_FAILURE;

// Spotify account information
struct account {
  char *username;
  char *password;
};

struct state {
  sp_session *session;

  struct event_base *event_base;
  struct event *async;
  struct event *timer;
  struct event *sigint;
  struct timeval next_timeout;

  struct evhttp *http;

  apr_pool_t *pool;
};

typedef void (*handle_playlist_fn)(sp_playlist *playlist,
                                   struct evhttp_request *request,
                                   void *userdata);

// State of a request as it's threaded through libspotify callbacks
struct playlist_handler {
  sp_playlist_callbacks *playlist_callbacks;
  struct evhttp_request *request;
  handle_playlist_fn callback;
  void *userdata;
};

static void send_reply(struct evhttp_request *request,
                       int code,
                       const char *message,
                       struct evbuffer *buf) {
  evhttp_add_header(evhttp_request_get_output_headers(request),
                    "Content-type", "application/json; charset=UTF-8");
  evhttp_send_reply(request, code, message, buf);
}

// Will wrap an error message in a JSON object before sending it
static void send_error(struct evhttp_request *request,
                       int code,
                       const char *message) {
  json_t *error_object = json_object();
  json_object_set(error_object, "message", json_string(message));
  struct evbuffer *buf = evhttp_request_get_output_buffer(request);
  char *error = json_dumps(error_object, JSON_COMPACT);
  json_decref(error_object);
  evbuffer_add(buf, error, strlen(error));
  send_reply(request, code, message, buf);
  free(error);
}

static void send_error_sp(struct evhttp_request *request,
                          int code,
                          sp_error error) {
  const char *message = sp_error_message(error);
  send_error(request, code, message);
}

static struct playlist_handler *register_playlist_callbacks(
    sp_playlist *playlist,
    struct evhttp_request *request,
    handle_playlist_fn callback,
    sp_playlist_callbacks *playlist_callbacks,
    void *userdata) {
  struct playlist_handler *handler = malloc(sizeof (struct playlist_handler));
  handler->request = request;
  handler->callback = callback;
  handler->playlist_callbacks = playlist_callbacks;
  handler->userdata = userdata;
  sp_playlist_add_callbacks(playlist, handler->playlist_callbacks, handler);
  return handler;
}

void playlist_dispatch(sp_playlist *playlist, void *userdata) {
  struct playlist_handler *handler = userdata;
  sp_playlist_remove_callbacks(playlist, handler->playlist_callbacks, handler);
  handler->playlist_callbacks = NULL;
  handler->callback(playlist, handler->request, handler->userdata);
  free(handler);
}

static void playlist_state_changed(sp_playlist *playlist, void *userdata) {
  if (!sp_playlist_is_loaded(playlist))
    return;

  playlist_dispatch(playlist, userdata);
}

static sp_playlist_callbacks playlist_state_changed_callbacks = {
  .playlist_state_changed = &playlist_state_changed
};

static void playlist_update_in_progress(sp_playlist *playlist,
                                        bool done,
                                        void *userdata) {
  if (done)
    playlist_dispatch(playlist, userdata);
}

static sp_playlist_callbacks playlist_update_in_progress_callbacks = {
  .playlist_update_in_progress = &playlist_update_in_progress
};

// HTTP handlers

// Standard response handler: 500 Not Implemented
static void not_implemented(sp_playlist *playlist,
                            struct evhttp_request *request,
                            void *userdata) {
  evhttp_send_error(request, 500, "Not Implemented");
}

// Responds with an entire playlist
static void get_playlist(sp_playlist *playlist,
                         struct evhttp_request *request,
                         void *userdata) {
  json_t *json = json_object();

  // URI
  sp_link *playlist_link = sp_link_create_from_playlist(playlist);
  char playlist_uri[64];
  sp_link_as_string(playlist_link, playlist_uri, 64);
  sp_link_release(playlist_link);
  json_object_set_new(json, "uri", json_string_nocheck(playlist_uri));

  // Title
  json_object_set_new(json, "title",
                      json_string_nocheck(sp_playlist_name(playlist)));

  // Creator
  sp_user *owner = sp_playlist_owner(playlist);
  const char *username = sp_user_display_name(owner);
  sp_user_release(owner);
  json_object_set_new(json, "creator", json_string_nocheck(username));

  // Collaborative
  json_object_set_new(json, "collaborative",
                      sp_playlist_is_collaborative(playlist) ? json_true()
                                                             : json_false());

  /*
  // Image
  byte image[20];

  if (sp_playlist_get_image(playlist, image)) {
    json_t *image_array = json_array();
    for (int i = 0; i < 20; i++) 
      json_array_append(image_array, json_integer((int) image[i]));
    json_object_set(json, "image", image_array);
  }
  */

  json_t *tracks = json_array();
  char track_uri[64];

  for (int i = 0; i < sp_playlist_num_tracks(playlist); i++) {
    sp_track *track = sp_playlist_track(playlist, i);
    sp_link *track_link = sp_link_create_from_track(track, 0);
    sp_link_as_string(track_link, track_uri, 64);
    json_array_append(tracks, json_string_nocheck(track_uri));
    sp_link_release(track_link);
  }

  json_object_set(json, "tracks", tracks);
  char *json_str = json_dumps(json, JSON_COMPACT);
  json_decref(json);
  struct evbuffer *buf = evhttp_request_get_output_buffer(request);
  evbuffer_add(buf, json_str, strlen(json_str));
  free(json_str);
  send_reply(request, 200, "OK", buf);
}

static void get_playlist_collaborative(sp_playlist *playlist,
                                       struct evhttp_request *request,
                                       void *userdata) {
  assert(sp_playlist_is_loaded(playlist));

  bool is_collaborative = sp_playlist_is_collaborative(playlist);
  sp_playlist_release(playlist);

  json_t *result = json_object();
  json_object_set_new(result, "collaborative",
                      is_collaborative ? json_true() : json_false());
  char *json = json_dumps(result, JSON_COMPACT);
  struct evbuffer *buf = evhttp_request_get_output_buffer(request);
  evbuffer_add(buf, json, strlen(json));
  send_reply(request, HTTP_OK, "OK", buf); 
}

static void put_playlist_add_tracks(sp_playlist *playlist,
                                    struct evhttp_request *request,
                                    void *userdata) {
  sp_session *session = userdata;
  const char *uri = evhttp_request_get_uri(request);
  struct evkeyvalq query_fields;
  evhttp_parse_query(uri, &query_fields);

  // Parse index
  const char *index_field = evhttp_find_header(&query_fields, "index");
  int index;

  if (index_field == NULL || sscanf(index_field, "%d", &index) <= 0) {
    send_error(request, HTTP_BADREQUEST,
               "Bad parameter: index must be numeric");
    return;
  }

  struct evbuffer *buf = evhttp_request_get_input_buffer(request);
  size_t buflen = evbuffer_get_length(buf);

  if (buflen == 0) {
    send_error(request, HTTP_BADREQUEST, "No body");
    return;
  }

  // Read request body
  char *request_body = calloc(buflen + 1, sizeof (char));
  strncpy(request_body, evbuffer_pullup(buf, buflen), buflen);
  request_body[buflen] = '\0';

  // Parse JSON
  json_error_t loads_error;
  json_t *json = json_loads(request_body, &loads_error);
  free(request_body);

  if (json == NULL) {
    send_error(request, HTTP_BADREQUEST,
               loads_error.text ? loads_error.text : "Unable to parse JSON");
    return;
  }

  if (!json_is_array(json)) {
    json_decref(json);
    send_error(request, HTTP_BADREQUEST, "Not valid JSON array");
    return;
  }

  // Handle empty array
  int num_tracks = json_array_size(json);

  if (num_tracks == 0) {
    struct evbuffer *buf = evbuffer_new();
    send_reply(request, HTTP_OK, "OK", buf); 
    evbuffer_free(buf);
    return;
  }

  const sp_track **tracks = calloc(num_tracks, sizeof (sp_track *));
  int num_valid_tracks = 0;

  for (int i = 0; i < num_tracks; i++) {
    json_t *item = json_array_get(json, i);

    if (!json_is_string(item)) {
      json_decref(item);
      continue;
    }

    char *uri = strdup(json_string_value(item));
    sp_link *track_link = sp_link_create_from_string(uri);
    free(uri);

    if (track_link == NULL)
      continue;

    if (sp_link_type(track_link) != SP_LINKTYPE_TRACK) {
      sp_link_release(track_link);
      continue;
    }

    sp_track *track = sp_link_as_track(track_link);
    
    if (track == NULL)
      continue;

    tracks[num_valid_tracks++] = track;
  }

  json_decref(json);
  
  // Bail if no tracks could be read from input
  if (num_valid_tracks == 0) {
    send_error(request, HTTP_BADREQUEST, "No valid tracks");
    free(tracks);
    return;
  }

  tracks = realloc(tracks, num_valid_tracks * sizeof (sp_track *));
  struct playlist_handler *handler = register_playlist_callbacks(
      playlist, request, &get_playlist,
      &playlist_update_in_progress_callbacks, NULL);
  sp_error add_tracks_error = sp_playlist_add_tracks(playlist, tracks,
                                                     num_valid_tracks,
                                                     index, session);

  if (add_tracks_error != SP_ERROR_OK) {
    sp_playlist_remove_callbacks(playlist, handler->playlist_callbacks,
                                 handler);
    free(handler);
    send_error_sp(request, HTTP_BADREQUEST, add_tracks_error);
  }

  free(tracks);
}

static void put_playlist_remove_tracks(sp_playlist *playlist,
                                       struct evhttp_request *request,
                                       void *userdata) {
  // sp_session *session = userdata;
  const char *uri = evhttp_request_get_uri(request);
  struct evkeyvalq query_fields;
  evhttp_parse_query(uri, &query_fields);

  // Parse index
  const char *index_field = evhttp_find_header(&query_fields, "index");
  int index;

  if (index_field == NULL ||
      sscanf(index_field, "%d", &index) <= 0 ||
      index < 0) {
    send_error(request, HTTP_BADREQUEST,
               "Bad parameter: index must be numeric");
    return;
  }

  const char *count_field = evhttp_find_header(&query_fields, "count");
  int count;

  if (count_field == NULL ||
      sscanf(count_field, "%d", &count) <= 0 ||
      count < 1) {
    send_error(request, HTTP_BADREQUEST,
               "Bad parameter: count must be numeric and positive");
    return;
  }

  int *tracks = calloc(count, sizeof(int));

  for (int i = 0; i < count; i++) 
    tracks[i] = index + i; 

  struct playlist_handler *handler = register_playlist_callbacks(
      playlist, request, &get_playlist,
      &playlist_update_in_progress_callbacks, NULL);
  sp_error remove_tracks_error = sp_playlist_remove_tracks(playlist, tracks, 
                                                           count);

  if (remove_tracks_error != SP_ERROR_OK) {
    sp_playlist_remove_callbacks(playlist, handler->playlist_callbacks, handler);
    free(handler);
    send_error_sp(request, HTTP_BADREQUEST, remove_tracks_error);
  }

  free(tracks);
}

static void put_playlist_patch(sp_playlist *playlist,
                               struct evhttp_request *request,
                               void *userdata) {
  struct state *state = userdata;
  struct evbuffer *buf = evhttp_request_get_input_buffer(request);
  size_t buflen = evbuffer_get_length(buf);

  if (buflen == 0) {
    send_error(request, HTTP_BADREQUEST, "No body");
    return;
  }

  // Read request body
  char *request_body = calloc(buflen + 1, sizeof (char));
  strncpy(request_body, evbuffer_pullup(buf, buflen), buflen);
  request_body[buflen] = '\0';

  // Parse JSON
  json_error_t loads_error;
  json_t *json = json_loads(request_body, &loads_error);
  free(request_body);

  if (json == NULL) {
    send_error(request, HTTP_BADREQUEST,
               loads_error.text ? loads_error.text : "Unable to parse JSON");
    return;
  }

  if (!json_is_array(json)) {
    json_decref(json);
    send_error(request, HTTP_BADREQUEST, "Not valid JSON array");
    return;
  }

  // Handle empty array
  int num_tracks = json_array_size(json);

  if (num_tracks == 0) {
    struct evbuffer *buf = evbuffer_new();
    send_reply(request, HTTP_OK, "OK", buf); 
    evbuffer_free(buf);
    return;
  }

  const sp_track **tracks = calloc(num_tracks, sizeof (sp_track *));
  int num_valid_tracks = 0;

  for (int i = 0; i < num_tracks; i++) {
    json_t *item = json_array_get(json, i);

    if (!json_is_string(item)) {
      json_decref(item);
      continue;
    }

    char *uri = strdup(json_string_value(item));
    sp_link *track_link = sp_link_create_from_string(uri);
    free(uri);

    if (track_link == NULL)
      continue;

    if (sp_link_type(track_link) != SP_LINKTYPE_TRACK) {
      sp_link_release(track_link);
      continue;
    }

    sp_track *track = sp_link_as_track(track_link);
    
    if (track == NULL)
      continue;

    tracks[num_valid_tracks++] = track;
  }

  json_decref(json);
  
  // Bail if no tracks could be read from input
  if (num_valid_tracks == 0) {
    send_error(request, HTTP_BADREQUEST, "No valid tracks");
    free(tracks);
    return;
  }

  tracks = realloc(tracks, num_valid_tracks * sizeof (sp_track *));

  // Apply diff
  apr_pool_t *pool = state->pool;
  svn_diff_t *diff;
  svn_error_t *diff_error = diff_playlist_tracks(&diff, playlist, tracks, 
                                                 num_valid_tracks, pool); 

  if (diff_error != SVN_NO_ERROR) {
    free(tracks);
    svn_handle_error2(diff_error, stderr, false, "Diff");
    send_error(request, HTTP_BADREQUEST, "Search failed");
    return;
  }

  svn_error_t *apply_error = diff_playlist_tracks_apply(diff, playlist, tracks,
                                                        num_valid_tracks,
                                                        state->session);

  svn_stream_t *out;
  svn_stream_for_stdout(&out, pool);
  diff_output_stdout(out, diff, playlist, tracks, num_valid_tracks, pool);

  if (apply_error != SVN_NO_ERROR) {
    free(tracks);
    svn_handle_error2(apply_error, stderr, false, "Updating playlist");
    send_error(request, HTTP_BADREQUEST, "Could not apply diff");
    return;
  }

  if (!sp_playlist_has_pending_changes(playlist)) {
    free(tracks);
    get_playlist(playlist, request, NULL);
    return;
  }

  free(tracks);
  register_playlist_callbacks(playlist, request, &get_playlist,
                              &playlist_update_in_progress_callbacks, NULL);
}

// Request dispatcher
static void handle_request(struct evhttp_request *request,
                            void *userdata) {
  evhttp_add_header(evhttp_request_get_output_headers(request),
                    "Server", "johan@liesen.se/spotify-api-server");

  // Check request method
  int http_method = evhttp_request_get_command(request);

  switch (http_method) {
    case EVHTTP_REQ_GET:
    case EVHTTP_REQ_PUT:
    case EVHTTP_REQ_POST:
      break;

    default:
      evhttp_send_error(request, 501, "Not Implemented");
      return;
  }

  struct state *state = userdata;
  sp_session *session = state->session;
  char *uri = evhttp_decode_uri(evhttp_request_get_uri(request));

  // Handle requests to /playlist/<playlist_uri>/<action>
  char *entity = strtok(uri, "/");

  if (entity == NULL || strncmp(entity, "playlist", 8) != 0) {
    evhttp_send_error(request, HTTP_BADREQUEST, "Bad Request");
    free(uri);
    return;
  }

  char *playlist_uri = strtok(NULL, "/");

  if (playlist_uri == NULL) {
    switch (http_method) {
      case EVHTTP_REQ_PUT:
      case EVHTTP_REQ_POST:
        // TODO(liesen): Add code to create playlists
        // request_callback = &put_playlist_create;
        not_implemented(NULL, request, NULL);
        break;

      default:
        send_error(request, HTTP_BADREQUEST, "Bad Request");
        break;
    }

    free(uri);
    return;
  }

  sp_link *playlist_link = sp_link_create_from_string(playlist_uri);

  if (playlist_link == NULL) {
    send_error(request, HTTP_NOTFOUND, "Link not found");
    free(uri);
    return;
  }

  if (sp_link_type(playlist_link) != SP_LINKTYPE_PLAYLIST) {
    sp_link_release(playlist_link);
    send_error(request, HTTP_BADREQUEST, "Not a playlist link");
    free(uri);
    return;
  }

  sp_playlist *playlist = sp_playlist_create(session, playlist_link);
  sp_link_release(playlist_link);

  if (playlist == NULL) {
    send_error(request, HTTP_NOTFOUND, "Playlist not found");
    free(uri);
    return;
  }

  sp_playlist_add_ref(playlist);

  // Dispatch request
  char *action = strtok(NULL, "/");
  free(uri);

  // Default request handler
  handle_playlist_fn request_callback = &not_implemented;
  void *callback_userdata = NULL;

  switch (http_method) {
  case EVHTTP_REQ_GET:
    {
      if (action == NULL) {
        // Send entire playlist
        request_callback = &get_playlist;
      } else if (strncmp(action, "collaborative", 13) == 0) {
        request_callback = &get_playlist_collaborative;
      }
    }
    break;

  case EVHTTP_REQ_PUT:
  case EVHTTP_REQ_POST:
    {
      callback_userdata = session;

      if (strncmp(action, "add", 3) == 0) {
        request_callback = &put_playlist_add_tracks;
      } else if (strncmp(action, "remove", 6) == 0) {
        request_callback = &put_playlist_remove_tracks;
      } else if (strncmp(action, "patch", 5) == 0) {
        callback_userdata = state;
        request_callback = &put_playlist_patch;
      }
    }
    break;
  }

  if (sp_playlist_is_loaded(playlist)) {
    request_callback(playlist, request, callback_userdata);
  } else {
    // Wait for playlist to load
    register_playlist_callbacks(playlist, request, request_callback,
                                &playlist_state_changed_callbacks,
                                callback_userdata);
  } 
}

static void playlistcontainer_loaded(sp_playlistcontainer *pc, void *userdata) {
  fprintf(stderr, "playlistcontainer_loaded\n");
  sp_session *session = userdata;
  struct state *state = sp_session_userdata(session);

  state->http = evhttp_new(state->event_base);
  evhttp_set_timeout(state->http, 60);
  evhttp_set_gencb(state->http, &handle_request, state);

  // TODO(liesen): Make address and port configurable
  if (evhttp_bind_socket(state->http, "0.0.0.0", 1337) == -1) {
    fprintf(stderr, "fail\n");
    sp_session_logout(session);
  }
}

static sp_playlistcontainer_callbacks playlistcontainer_callbacks = {
  .container_loaded = playlistcontainer_loaded
};

// Catches SIGINT and exits gracefully
static void sigint_handler(evutil_socket_t socket,
                           short what,
                           void *userdata) {
  fprintf(stderr, "signal_handler\n");
  struct state *state = userdata;
  sp_session_logout(state->session);
}

static void logged_out(sp_session *session) {
  fprintf(stderr, "logged_out\n");
  struct state *state = sp_session_userdata(session);
  event_del(state->async);
  event_del(state->timer);
  event_del(state->sigint);
  event_base_loopbreak(state->event_base);
  apr_pool_destroy(state->pool);
}


static void logged_in(sp_session *session, sp_error error) {
  if (error != SP_ERROR_OK) {
    fprintf(stderr, "%s\n", sp_error_message(error));
    exit_status = EXIT_FAILURE;
    logged_out(session);
    return;
  }

  struct state *state = sp_session_userdata(session);
  state->session = session;
  evsignal_add(state->sigint, NULL);

  sp_playlistcontainer *pc = sp_session_playlistcontainer(session);
  sp_playlistcontainer_add_callbacks(pc, &playlistcontainer_callbacks,
                                     session);
}

static void process_events(evutil_socket_t socket,
                           short what,
                           void *userdata) {
  struct state *state = userdata;
  event_del(state->timer);
  int timeout = 0;

  do {
    sp_session_process_events(state->session, &timeout);
  } while (timeout == 0);

  state->next_timeout.tv_sec = timeout / 1000;
  state->next_timeout.tv_usec = (timeout % 1000) * 1000;
  evtimer_add(state->timer, &state->next_timeout);
}

static void notify_main_thread(sp_session *session) {
  fprintf(stderr, "notify_main_thread\n");
  struct state *state = sp_session_userdata(session);
  event_active(state->async, 0, 1);
}

int main(int argc, char **argv) {
  struct account account = {
    .username = username,
    .password = password
  };

  // Initialize program state
  struct state *state = malloc(sizeof(struct state));

  // Initialize libev w/ pthreads
  evthread_use_pthreads();

  state->event_base = event_base_new();
  state->async = event_new(state->event_base, -1, 0, &process_events, state);
  state->timer = evtimer_new(state->event_base, &process_events, state);
  state->sigint = evsignal_new(state->event_base, SIGINT, &sigint_handler, state);

  // Initialize APR
  apr_status_t rv = apr_initialize();

  if (rv != APR_SUCCESS)
    return EXIT_FAILURE;

  apr_pool_create(&state->pool, NULL);

  // Initialize libspotify
  sp_session_callbacks session_callbacks = {
    .logged_in = &logged_in,
    .logged_out = &logged_out,
    .notify_main_thread = &notify_main_thread
  };

  sp_session_config session_config = {
    .api_version = SPOTIFY_API_VERSION,
    .application_key = g_appkey,
    .application_key_size = g_appkey_size,
    .cache_location = ".cache",
    .callbacks = &session_callbacks,
    .compress_playlists = false,
    .dont_save_metadata_for_playlists = false,
    .settings_location = ".settings",
    .user_agent = "sphttpd",
    .userdata = state,
  };

  sp_session *session;
  sp_error session_create_error = sp_session_create(&session_config,
                                                    &session);

  if (session_create_error != SP_ERROR_OK)
    return EXIT_FAILURE;

  // Log in to Spotify
  sp_session_login(session, account.username, account.password);

  event_base_dispatch(state->event_base);

  event_free(state->async);
  event_free(state->timer);
  if (state->http != NULL) evhttp_free(state->http);
  event_base_free(state->event_base);
  free(state);
  return exit_status;
}

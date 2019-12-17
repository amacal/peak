#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <uv.h>

typedef struct peak_multi_context_s {
  uv_loop_t *uv_loop;
  uv_timer_t *uv_timer;
  CURLM *multi_handle;
} peak_multi_context_t;

typedef struct peak_socket_context_s {
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
  CURLM *multi_handle;
} peak_socket_context_t;

int curlm_describe(char *context, int errornum) {
  printf("%s: %d, %s\n", context, errornum, curl_multi_strerror(errornum));
  return errornum;
}

size_t peak_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  return nmemb;
}

peak_socket_context_t* peak_create_socket_context(curl_socket_t sockfd, peak_multi_context_t *multi_context) {
  peak_socket_context_t *context;

  context = (peak_socket_context_t*)malloc(sizeof(*context));
  context->sockfd = sockfd;
  context->multi_handle = multi_context->multi_handle;

  uv_poll_init_socket(multi_context->uv_loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;

  return context;
}

peak_multi_context_t* peak_create_multi_context(uv_timer_t *uv_timer, uv_loop_t *uv_loop, CURLM *multi_handle) {
  peak_multi_context_t *context;
  context = (peak_multi_context_t*)malloc(sizeof(*context));

  context->uv_timer = uv_timer;
  context->uv_loop = uv_loop;
  context->multi_handle = multi_handle;

  return context;
}

void peak_uv_close_callback(uv_handle_t *handle) {
  free((peak_socket_context_t*)handle->data);
}

void check_multi_info(CURLM *multi_handle) {
  int pending;
  char *url;
  long code;

  CURLMsg *info;
  CURL *easy_handle;

  while ((info = curl_multi_info_read(multi_handle, &pending))) {
    switch (info->msg) {
      case CURLMSG_DONE:
        easy_handle = info->easy_handle;

        curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &url);
        printf("url: %s\n", url);

        curl_easy_getinfo(easy_handle, CURLINFO_RESPONSE_CODE, &code);
        printf("code: %ld\n", code);

        curl_multi_remove_handle(multi_handle, easy_handle);
        curl_easy_cleanup(easy_handle);
      break;
      default:
        printf("default\n");
    }
  }
}

void curl_perform(uv_poll_t *req, int status, int events) {
  int running, flags = 0;
  peak_socket_context_t *context = (peak_socket_context_t*)req->data;

  if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;

  curl_multi_socket_action(context->multi_handle, context->sockfd, flags, &running);
  check_multi_info(context->multi_handle);
}

int peak_socket_callback(CURL *easy, curl_socket_t socket, int action, void *userp, void *socketp) {
  int events = 0;
  peak_socket_context_t *socket_context;

  peak_multi_context_t *multi_context = (peak_multi_context_t*)userp;
  if (socketp) socket_context = (peak_socket_context_t*)socketp;

  switch (action) {
    case CURL_POLL_IN:
    case CURL_POLL_OUT:
    case CURL_POLL_INOUT:
      socket_context = socketp ? socket_context : peak_create_socket_context(socket, multi_context);
      if (!socketp) curl_multi_assign(multi_context->multi_handle, socket, (void*) socket_context);

      if (action != CURL_POLL_IN) events |= UV_WRITABLE;
      if (action != CURL_POLL_OUT) events |= UV_READABLE;

      uv_poll_start(&socket_context->poll_handle, events, curl_perform);
    break;
    case CURL_POLL_REMOVE:
      if (socketp) {
        uv_poll_stop(&socket_context->poll_handle);
        curl_multi_assign(multi_context->multi_handle, socket, NULL);
        uv_close((uv_handle_t*)&socket_context->poll_handle, peak_uv_close_callback);
      }
    break;
  }

  return 0;
}

void on_timeout(uv_timer_t *timer) {
  int handles, errornum;

  peak_multi_context_t *context;
  context = (peak_multi_context_t*)timer->data;

  errornum = curl_multi_socket_action(context->multi_handle, CURL_SOCKET_TIMEOUT, 0, &handles);
  if (errornum != CURLM_OK) curlm_describe("curl_multi_socket_action", errornum);

  check_multi_info(context->multi_handle);
}

int peak_timer_callback(CURLM *multi, long timeout_ms, void *userp) {
  peak_multi_context_t *context;
  context = (peak_multi_context_t*)userp;

  if (timeout_ms < 0) {
    uv_timer_stop(context->uv_timer);
    return 0;
  }

  if (timeout_ms == 0) timeout_ms = 1;
  uv_timer_start(context->uv_timer, on_timeout, timeout_ms, 0);

  return 0;
}

int main(int argc, char **argv) {

  int curle;
  uv_timer_t *timer;
  CURLM *multi_handle;
  peak_multi_context_t *multi_context;

  printf("libcurl: %s\n", curl_version_info(CURLVERSION_NOW)->version);
  printf("libuv: %s\n", uv_version_string());

  uv_loop_t *loop = uv_default_loop();

  curle = curl_global_init(CURL_GLOBAL_ALL);
  if (curle != 0) return -1;

  CURL *easy_handle = curl_easy_init();
  if (easy_handle == NULL) return -1;

  multi_handle = curl_multi_init();
  if (multi_handle == NULL) return -1;

  timer = (uv_timer_t*)malloc(sizeof(*timer));
  uv_timer_init(loop, timer);

  multi_context = peak_create_multi_context(timer, loop, multi_handle);
  timer->data = multi_context;

  curle = curl_easy_setopt(easy_handle, CURLOPT_URL, "https://releases.hashicorp.com/terraform/0.12.18/terraform_0.12.18_linux_amd64.zip");
  if (curle != CURLE_OK) return -2;

  curle = curl_easy_setopt(easy_handle, CURLOPT_WRITEFUNCTION, peak_write_callback);
  if (curle != CURLE_OK) return -2;

  curle = curl_multi_setopt(multi_handle, CURLMOPT_SOCKETFUNCTION, peak_socket_callback);
  if (curle != CURLM_OK) return -3;

  curle = curl_multi_setopt(multi_handle, CURLMOPT_SOCKETDATA, multi_context);
  if (curle != CURLM_OK) return -3;

  curle = curl_multi_setopt(multi_handle, CURLMOPT_TIMERFUNCTION, peak_timer_callback);
  if (curle != CURLM_OK) return -3;

  curle = curl_multi_setopt(multi_handle, CURLMOPT_TIMERDATA, multi_context);
  if (curle != CURLM_OK) return -3;

  curle = curl_multi_add_handle(multi_handle, easy_handle);
  if (curle != CURLM_OK) return -3;

  uv_run(loop, UV_RUN_DEFAULT);
  curl_multi_cleanup(multi_handle);

  return CURLE_OK;
}

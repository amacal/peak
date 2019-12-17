#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <uv.h>

uv_loop_t *loop;
uv_timer_t timer;
CURLM *multi_handle;

typedef struct curl_context_s {
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
} curl_context_t;

int curlm_describe(char *context, int errornum) {
  printf("%s: %d, %s\n", context, errornum, curl_multi_strerror(errornum));
  return errornum;
}

size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
  printf("wrote %zu bytes\n", nmemb);
  return nmemb;
}

int on_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
  return 0;
}

curl_context_t* create_curl_context(curl_socket_t sockfd) {
  curl_context_t *context;

  context = (curl_context_t *)malloc(sizeof(*context));
  context->sockfd = sockfd;

  uv_poll_init_socket(loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;

  return context;
}

void curl_close_cb(uv_handle_t *handle) {
  curl_context_t *context = (curl_context_t*)handle->data;
  free(context);
}

void check_multi_info(CURLM *multi) {
  CURLMsg *info;
  CURL *easy;
  int pending;

  while ((info = curl_multi_info_read(multi, &pending))) {
    printf("message: %d, pending: %d\n", info->msg, pending);
    switch (info->msg) {
      case CURLMSG_DONE:
        easy = info->easy_handle;
        printf("done\n");

        curl_multi_remove_handle(multi_handle, easy);
        curl_easy_cleanup(easy);
      default:
        printf("default\n");
    }
  }
}

void curl_perform(uv_poll_t *req, int status, int events) {
  int running, flags = 0;
  curl_context_t *context = (curl_context_t*)req->data;

  if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;

  curl_multi_socket_action(multi_handle, context->sockfd, flags, &running);
  check_multi_info(multi_handle);
}

int socket_callback(CURL *easy, curl_socket_t socket, int action, void *userp, void *socketp) {

  int events = 0;
  curl_context_t *context;

  if (socketp) {
    context = (curl_context_t*)socketp;
  }

  switch (action) {
    case CURL_POLL_IN:
    case CURL_POLL_OUT:
    case CURL_POLL_INOUT:
      context = socketp ? context : create_curl_context(socket);
      curl_multi_assign(multi_handle, socket, (void*) context);

      if (action != CURL_POLL_IN) events |= UV_WRITABLE;
      if (action != CURL_POLL_OUT) events |= UV_READABLE;

      printf("socket started: %d\n", action);
      uv_poll_start(&context->poll_handle, events, curl_perform);
    break;
    case CURL_POLL_REMOVE:
      if (socketp) {
        uv_poll_stop(&context->poll_handle);
        curl_multi_assign(multi_handle, socket, NULL);

        printf("socked removed\n");
        uv_close((uv_handle_t*)&context->poll_handle, curl_close_cb);
      }
    break;
  }

  return 0;
}

void on_timeout(uv_timer_t *timer) {
  int handles, errornum;

  errornum = curl_multi_socket_action(multi_handle, CURL_SOCKET_TIMEOUT, 0, &handles);
  if (errornum != CURLM_OK) curlm_describe("curl_multi_socket_action", errornum);

  printf("handles: %d\n", handles);
  check_multi_info(multi_handle);
}

int timer_callback(CURLM *multi, long timeout_ms, void *userp) {
  printf("timeout: %ld\n", timeout_ms);

  if (timeout_ms < 0) {
    uv_timer_stop(&timer);
    return 0;
  }

  if (timeout_ms == 0) {
    timeout_ms = 1;
  }

  uv_timer_start(&timer, on_timeout, timeout_ms, 0);
  return 0;
}

int curl_perform_transfer(CURLM *multi_handle) {
  int running, errornum;

  do {
    errornum = curl_multi_wait(multi_handle, NULL, 0, 1000, NULL);
    if (errornum != CURLM_OK) return curlm_describe("curl_multi_wait", errornum);

    errornum = curl_multi_perform(multi_handle, &running);
    if (errornum != CURLM_OK) return errornum;
  } while(running);

  return CURLM_OK;
}

int main(int argc, char **argv) {

  int curle;

  printf("%s\n", curl_version());
  printf("%s\n", uv_version_string());

  loop = uv_default_loop();
  uv_timer_init(loop, &timer);

  curle = curl_global_init(CURL_GLOBAL_ALL);
  if (curle != 0) return -1;

  CURL *easy_handle = curl_easy_init();
  if (easy_handle == NULL) return -1;

  multi_handle = curl_multi_init();
  if (multi_handle == NULL) return -1;

  curle = curl_easy_setopt(easy_handle, CURLOPT_NOPROGRESS, 0);
  if (curle != CURLE_OK) return -2;

  curle = curl_easy_setopt(easy_handle, CURLOPT_URL, "https://releases.hashicorp.com/terraform/0.12.18/terraform_0.12.18_linux_amd64.zip");
  if (curle != CURLE_OK) return -2;

  curle = curl_easy_setopt(easy_handle, CURLOPT_WRITEFUNCTION, on_write);
  if (curle != CURLE_OK) return -2;

  curle = curl_easy_setopt(easy_handle, CURLOPT_XFERINFOFUNCTION, on_progress);
  if (curle != CURLE_OK) return -2;

  curle = curl_multi_setopt(multi_handle, CURLMOPT_SOCKETFUNCTION, socket_callback);
  if (curle != CURLM_OK) return -3;

  curle = curl_multi_setopt(multi_handle, CURLMOPT_TIMERFUNCTION, timer_callback);
  if (curle != CURLM_OK) return -3;

  curle = curl_multi_add_handle(multi_handle, easy_handle);
  if (curle != CURLM_OK) return -3;

  //curle = curl_perform_transfer(multi_handle);
  //if (curle != CURLM_OK) return curlm_describe("curl_perform_transfer", curle);

  uv_run(loop, UV_RUN_DEFAULT);
  curl_multi_cleanup(multi_handle);

  return CURLE_OK;
}

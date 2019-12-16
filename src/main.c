#include <stdio.h>
#include <curl/curl.h>

size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
  return nmemb;
}

int on_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
  printf("total %zu, now %zu\n", dltotal, dlnow);
  return 0;
}

int main(int argc, char **argv) {
  CURL *easy_handle = curl_easy_init();

  curl_easy_setopt(easy_handle, CURLOPT_NOPROGRESS, 0);
  curl_easy_setopt(easy_handle, CURLOPT_URL, "https://example.com/");

  curl_easy_setopt(easy_handle, CURLOPT_WRITEFUNCTION, on_write);
  curl_easy_setopt(easy_handle, CURLOPT_XFERINFOFUNCTION, on_progress);

  curl_easy_perform(easy_handle);
  curl_easy_cleanup(easy_handle);

  return 0;
}

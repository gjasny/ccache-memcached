// ccache -- a fast C/C++ compiler cache
//
// Copyright (C) 2016 Anders Björklund
// Copyright (C) 2016-2018 Joel Rosdahl
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "ccache.h"

#ifdef HAVE_LIBCURL

#include <curl/curl.h>
#include <netinet/in.h>

#define HTTPCACHE_MAGIC "CCH1"

static char *cache_url;

int httpcache_init(char *conf)
{
    curl_global_init(CURL_GLOBAL_ALL);

    cache_url = strdup(conf);
	if (!cache_url || strlen(cache_url) == 0) {
		cc_log("Problem creating curl with conf %s:\n", conf);
		return -1;
	}
	return 0;
}

int httpcache_raw_set(const char *key, const char *data, size_t len)
{
    char* url = NULL;
    if (asprintf(&url, "%s/%s", cache_url, key) < 0) {
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

    struct curl_slist* header_chunk = NULL;
    header_chunk = curl_slist_append(NULL, "application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_chunk);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, len);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    //curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

    CURLcode curl_error = curl_easy_perform(curl);

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_easy_cleanup(curl);
    curl_slist_free_all(header_chunk);

    if (curl_error != CURLE_OK) {
        cc_log("Failed to move %s to http cache: return code: %li", key, response_code);
        return -1;
    }
    return 0;
}

// Blob format for storing:

//   char magic[4]; # 'CCH1', might change for other version of ccache
// # ccache will erase the blob in httpcache if wrong magic
//   uint32_t obj_len; # network endian
//   char *obj[obj_len];
//   uint32_t stderr_len; # network endian
//   char *stderr[stderr_len];
//   uint32_t dia_len; # network endian
//   char *dia[dia_len];
//   uint32_t dep_len; # network endian
//   char *dep[dep_len];

int httpcache_set(const char *key,
                  const char *obj,
                  const char *stderr,
                  const char *dia,
                  const char *dep,
                  size_t obj_len,
                  size_t stderr_len,
                  size_t dia_len,
                  size_t dep_len)
{
	size_t buf_len = 4 + 4*4 + obj_len + stderr_len + dia_len + dep_len;
	char *buf = x_malloc(buf_len);
	memcpy(buf, HTTPCACHE_MAGIC, 4);
	char *ptr = buf + 4;

#define PROCESS_ONE_BUFFER(src_ptr, src_len) \
	do { \
		*((uint32_t *)ptr) = htonl(src_len); \
		ptr += 4; \
		if (src_len > 0) { \
			memcpy(ptr, src_ptr, src_len); \
		} \
		ptr += src_len; \
	} while (false)

	PROCESS_ONE_BUFFER(obj, obj_len);
	PROCESS_ONE_BUFFER(stderr, stderr_len);
	PROCESS_ONE_BUFFER(dia, dia_len);
	PROCESS_ONE_BUFFER(dep, dep_len);

#undef PROCESS_ONE_BUFFER

	return httpcache_raw_set(key, buf, buf_len);
}

static void *httpcache_prune(const char *key)
{
	cc_log("key from httpcache has wrong data %s: pruning...", key);
	// Don't really care whether delete failed.

	// @todo implement DELETE

	return NULL;
}

struct writer {
    char **data;
    size_t *size;
};

static size_t httpcache_write(void *buffer, size_t size, size_t nmemb, void *stream)
{
    struct writer *out = (struct writer *)stream;

    size_t old_size = *(out->size);
    size_t write_size = size * nmemb;
    *(out->size) += write_size;

    *(out->data) = realloc(*(out->data), *(out->size));

    memcpy(*(out->data) + old_size, buffer, write_size);

    return nmemb;
}

// Caller should free the return value when done with the pointers.
int httpcache_raw_get(const char *key, char **data, size_t *size)
{
    char* url = NULL;
    if (asprintf(&url, "%s/%s", cache_url, key) < 0) {
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

    struct curl_slist* header_chunk = NULL;
    header_chunk = curl_slist_append(NULL, "application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_chunk);

    *data = NULL;
    *size = 0;
    struct writer callback_data = { data, size };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, httpcache_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callback_data);

    CURLcode curl_error = curl_easy_perform(curl);

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_easy_cleanup(curl);
    curl_slist_free_all(header_chunk);

    if (curl_error != CURLE_OK) {
        cc_log("Failed to move %s to http cache: return code: %li", key, response_code);
        return -1;
    }
    return 0;
}

// Caller should free the return value when done with the pointers.
void *httpcache_get(const char *key,
                    char **obj,
                    char **stderr,
                    char **dia,
                    char **dep,
                    size_t *obj_len,
                    size_t *stderr_len,
                    size_t *dia_len,
                    size_t *dep_len)
{
	char *value = NULL;
	size_t value_l = 0;
	int error;

	error = httpcache_raw_get(key, &value, &value_l);
	if (error) {
		cc_log("Failed to get key from httpcache %s", key);
		return NULL;
	}
	if (value_l < 20 || memcmp(value, HTTPCACHE_MAGIC, 4) != 0) {
		cc_log("wrong magic or length %.4s: %d", value, (int)value_l);
		free(value);
		return httpcache_prune(key);
	}

	char *ptr = value;
	// Skip the magic.
	ptr += 4;
	value_l -= 4;

#define PROCESS_ONE_BUFFER(dst_ptr, dst_len) \
	do { \
		if (value_l < 4) { \
			free(value); \
			cc_log("no more buffer for %s: %d", \
			       #dst_ptr, (int)value_l); \
			return httpcache_prune(key); \
		} \
		dst_len = ntohl(*((uint32_t *)ptr)); \
		ptr += 4; value_l -= 4; \
		if (value_l < dst_len) { \
			cc_log("no more buffer for %s: %d %d", \
			       #dst_ptr, (int)value_l, (int) dst_len); \
			free(value); \
			return httpcache_prune(key); \
		} \
		dst_ptr = ptr; \
		ptr += dst_len; value_l -= dst_len; \
	} while (false)

	PROCESS_ONE_BUFFER(*obj, *obj_len);
	PROCESS_ONE_BUFFER(*stderr, *stderr_len);
	PROCESS_ONE_BUFFER(*dia, *dia_len);
	PROCESS_ONE_BUFFER(*dep, *dep_len);

#undef PROCESS_ONE_BUFFER

	return value;
}

void httpcache_free(void *blob)
{
	free(blob);
}

int httpcache_release(void)
{
	return 0;
}

#endif // HAVE_LIBCURL

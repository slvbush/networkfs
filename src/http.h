#ifndef NETWORKFS_HTTP
#define NETWORKFS_HTTP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/**
 * networkfs_http_call - make a call to networkfs API.
 * @token:           Unique filesystem token.
 * @method:          API method name, e.g. "list" for fs.list.
 * @response_buffer: Pointer to memory space for writing the response.
 *                   There should be available at least @buffer_size bytes.
 * @args:            A vector of key-value pairs for the GET parameters.
 *
 * This method makes an HTTP call to networkfs API server and parses the result.
 *
 * Return:
 * * If HTTP session succeeds, returns `result->status`.
 *   `result->response` is written into @response_buffer.
 * * Otherwise, returns negated errno, either defined in `errno-base.h`
 *   or in `util.h`, and @response_buffer stays unaltered.
 */
int64_t networkfs_http_call(
    const char* token, const char* method, char* response_buffer,
    size_t buffer_size,
    std::span<const std::pair<std::string, std::string>> args);

#endif

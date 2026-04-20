#ifndef HTTP_RESPONSE_BUFFER_H
#define HTTP_RESPONSE_BUFFER_H

#include <cstddef>

class HTTPClient;

namespace http_response_buffer {

struct Buffer {
    char *data;
    size_t length;
    size_t capacity;
    bool from_psram;
};

bool read_all(HTTPClient &http, Buffer &buffer, const char *tag);
void release(Buffer &buffer);

} // namespace http_response_buffer

#endif // HTTP_RESPONSE_BUFFER_H

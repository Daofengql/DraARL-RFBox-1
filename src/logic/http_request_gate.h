#ifndef HTTP_REQUEST_GATE_H
#define HTTP_REQUEST_GATE_H

#include <Arduino.h>

namespace http_request_gate {

void init();
bool acquire(const char *owner, uint32_t timeout_ms);
void release(const char *owner);
const char *current_owner();

class ScopedLock {
public:
    ScopedLock(const char *owner, uint32_t timeout_ms);
    ~ScopedLock();

    bool locked() const;

private:
    const char *owner_;
    bool locked_;
};

} // namespace http_request_gate

#endif // HTTP_REQUEST_GATE_H

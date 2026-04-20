#include "version_utils.h"

#include <cctype>

namespace version_utils {
namespace {

unsigned long read_component(const char *&cursor, bool &has_component) {
    has_component = false;

    while (*cursor != '\0' && !std::isdigit(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }

    if (*cursor == '\0') {
        return 0;
    }

    has_component = true;
    unsigned long value = 0;
    while (*cursor != '\0' && std::isdigit(static_cast<unsigned char>(*cursor))) {
        value = (value * 10UL) + static_cast<unsigned long>(*cursor - '0');
        ++cursor;
    }

    return value;
}

} // namespace

int compare(const char *left, const char *right) {
    const char *lhs = left ? left : "";
    const char *rhs = right ? right : "";

    while (true) {
        bool lhs_has_component = false;
        bool rhs_has_component = false;
        const unsigned long lhs_value = read_component(lhs, lhs_has_component);
        const unsigned long rhs_value = read_component(rhs, rhs_has_component);

        if (!lhs_has_component && !rhs_has_component) {
            return 0;
        }
        if (lhs_value < rhs_value) {
            return -1;
        }
        if (lhs_value > rhs_value) {
            return 1;
        }
    }
}

} // namespace version_utils

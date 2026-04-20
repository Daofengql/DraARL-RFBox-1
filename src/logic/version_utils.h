#ifndef VERSION_UTILS_H
#define VERSION_UTILS_H

namespace version_utils {

// Returns -1 when left < right, 0 when equal, and 1 when left > right.
int compare(const char *left, const char *right);

} // namespace version_utils

#endif // VERSION_UTILS_H

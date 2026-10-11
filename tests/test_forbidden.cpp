// Standalone logic test — server refusal (HTTP 403).
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_forbidden.cpp -o /tmp/t && /tmp/t
//
// Exercises api/media/forbidden.hpp on the two error forms the app sees: the
// mpv reason built by MPVCore::getError and the HTTP client's exception text.

#include <cstdio>
#include <api/media/forbidden.hpp>

using media::isForbidden;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    CHECK(isForbidden("mpv -13: loading failed — ffmpeg: https: HTTP error 403 Forbidden; stream: Failed to open "
                      "https://<host>:32400/..."));
    CHECK(isForbidden("http status 403"));

    CHECK(!isForbidden(""));
    CHECK(!isForbidden("mpv -13: loading failed — ffmpeg: https: HTTP error 500 Internal Server Error"));
    CHECK(!isForbidden("http status 404"));
    CHECK(!isForbidden("Cancelled"));

    if (failures) {
        printf("test_forbidden: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_forbidden: OK\n");
    return 0;
}

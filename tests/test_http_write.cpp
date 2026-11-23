// Standalone logic test: how curl's data reaches a download's file, and how a write that fails
// (a full disk) stops the transfer instead of completing a truncated file.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_http_write.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <string>
#include <api/http_write.hpp>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

// A disk with room for `room` more bytes: past that, writes fail like ENOSPC.
class FullDisk : public std::streambuf {
public:
    explicit FullDisk(size_t room) : room(room) {}
    std::string data;

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streamsize fits = std::min<std::streamsize>(n, std::streamsize(room - data.size()));
        data.append(s, size_t(fits));
        return fits;
    }
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        if (data.size() >= room) return traits_type::eof();
        data.push_back(traits_type::to_char_type(c));
        return c;
    }

private:
    size_t room;
};

int main() {
    // all of it written: curl carries on
    std::ostringstream body;
    CHECK(writeChunk(body, "hello ", 6) == 6);
    CHECK(writeChunk(body, "world", 5) == 5);
    CHECK(body.str() == "hello world");
    CHECK(writeChunk(body, "", 0) == 0);  // an empty body: nothing to write, nothing failed

    // the disk fills mid-chunk: 0, so curl stops with CURLE_WRITE_ERROR
    FullDisk disk(10);
    std::ostream file(&disk);
    CHECK(writeChunk(file, "12345678", 8) == 8);
    CHECK(writeChunk(file, "abcdefgh", 8) == 0);
    // and every chunk after it, even one that would fit
    CHECK(writeChunk(file, "x", 1) == 0);

#ifdef __linux__
    // a real full device: /dev/full fails every write with ENOSPC (a chunk the size of curl's
    // largest, past the file buffer, so the write reaches the device)
    std::ofstream dev("/dev/full", std::ios::binary);
    if (dev) {
        std::string chunk(16384, 'x');
        CHECK(writeChunk(dev, chunk.data(), chunk.size()) == 0);
    } else {
        std::printf("note: no /dev/full, its case skipped\n");
    }
#endif

    if (fails) return 1;
    std::printf("test_http_write: all passed\n");
    return 0;
}

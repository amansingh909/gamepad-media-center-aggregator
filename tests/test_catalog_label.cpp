// pickCatalogLabel: a catalog's tab never reads blank.
#include <cstdio>
#include "api/stremio/catalog_label.hpp"

static int fails = 0;
#define CHECK(cond)                                         \
    do {                                                    \
        if (!(cond)) {                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            fails++;                                        \
        }                                                   \
    } while (0)

int main() {
    using stremio::pickCatalogLabel;
    CHECK(pickCatalogLabel("Popular", "top", "Cinemeta", "Movies") == "Popular");
    CHECK(pickCatalogLabel("pdm", "pdm", "Public Domain", "Movies") == "Public Domain");  // name = id
    CHECK(pickCatalogLabel("", "pdm", "Public Domain", "Movies") == "Public Domain");     // "name": ""
    CHECK(pickCatalogLabel("", "pdm", "", "Movies") == "pdm");                            // no addon name
    CHECK(pickCatalogLabel("pdm", "pdm", "", "Movies") == "pdm");
    CHECK(pickCatalogLabel("", "", "", "Movies 2") == "Movies 2");                        // nothing at all
    CHECK(stremio::genericCatalogLabel("Movies", 2) == "Movies 2");
    CHECK(stremio::genericCatalogLabel("Movies", 0) == "Movies");
    std::printf(fails ? "test_catalog_label: %d FAILED\n" : "test_catalog_label: all passed\n", fails);
    return fails ? 1 : 0;
}

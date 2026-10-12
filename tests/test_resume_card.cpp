// Standalone logic test: a Continue Watching card on the Home row (resume_card.hpp): its kind once
// its show's episodes are known, its second line, its caption and progress bar, and what A does.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_resume_card.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <vector>
#include <api/stremio/resume_card.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

using namespace stremio;

static const int64_t MIN = 60000;

static ResumeCard card(CardItem item, CardKind kind, int64_t offsetMs, int64_t durationMs) {
    ResumeCard c;
    c.item = item;
    c.kind = kind;
    c.offsetMs = offsetMs;
    c.durationMs = durationMs;
    return c;
}

/// "key a b", with as many numbers as the phrase takes
static std::string said(const Phrase& p) {
    std::string s = p.key;
    if (p.args >= 1) s += " " + std::to_string(p.a);
    if (p.args >= 2) s += " " + std::to_string(p.b);
    return s;
}

int main() {
    const CardItem movie = CardItem::Movie, episode = CardItem::Episode, show = CardItem::Show;
    const CardKind saved = CardKind::AsSaved, upNext = CardKind::UpNext, lastDone = CardKind::LastDone;

    // a show's second line: the episode, with its title once the show's episodes arrive
    CHECK_EQ(episodeLabel(2, 5, "Ozymandias"), "S2 E5 \xC2\xB7 Ozymandias");
    CHECK_EQ(episodeLabel(1, 2, ""), "S1 E2");

    // a card once its show's episodes are known (sorted, a special first)
    std::vector<EpisodeRef> eps = {{0, 1}, {1, 1}, {1, 2}, {2, 1}};
    CardResolve r = resolveCard(eps, 1, 2, 10 * MIN, 44 * MIN);
    CHECK(r.kind == saved && r.next == -1);  // in progress: as saved
    r = resolveCard(eps, 1, 2, 43 * MIN, 44 * MIN);
    CHECK(r.kind == upNext && r.next == 3);  // finished: the next one, into the next season
    r = resolveCard(eps, 2, 1, 43 * MIN, 44 * MIN);
    CHECK(r.kind == lastDone && r.next == -1);  // the show's last one finished
    r = resolveCard(eps, 5, 5, 43 * MIN, 44 * MIN);
    CHECK(r.kind == lastDone && r.next == -1);  // finished, and the show no longer lists it
    r = resolveCard({}, 1, 2, 43 * MIN, 44 * MIN);
    CHECK(r.kind == lastDone && r.next == -1);  // finished, and the show lists no episode
    r = resolveCard(eps, 1, 2, 1, 0);
    CHECK(r.kind == saved && r.next == -1);  // just advanced to it (1 ms, no length): as saved

    // the caption's first line
    CHECK_EQ(said(cardStatus(card(movie, saved, 30 * MIN, 94 * MIN))), "main/resume/hours_left 1 4");
    CHECK_EQ(said(cardStatus(card(episode, saved, 10 * MIN, 44 * MIN))), "main/resume/min_left 34");
    CHECK_EQ(said(cardStatus(card(episode, upNext, 0, 44 * MIN))), "main/resume/up_next");
    CHECK_EQ(said(cardStatus(card(episode, upNext, 0, 0))), "main/resume/up_next");
    CHECK_EQ(said(cardStatus(card(episode, saved, 43 * MIN, 44 * MIN))), "main/media/played");  // next not known yet
    CHECK_EQ(said(cardStatus(card(episode, lastDone, 43 * MIN, 44 * MIN))), "main/media/played");
    CHECK_EQ(said(cardStatus(card(episode, saved, 1, 0))), "");           // just advanced: no time left line
    CHECK_EQ(said(cardStatus(card(episode, saved, 20 * MIN, 0))), "");    // no length known
    CHECK_EQ(said(cardStatus(card(show, saved, 20 * MIN, 44 * MIN))), "");  // which episode isn't known

    // a progress bar only for what is in progress
    CHECK(cardProgress(card(movie, saved, 30 * MIN, 94 * MIN)));
    CHECK(cardProgress(card(episode, saved, 10 * MIN, 44 * MIN)));
    CHECK(!cardProgress(card(episode, upNext, 0, 44 * MIN)));
    CHECK(!cardProgress(card(episode, saved, 43 * MIN, 44 * MIN)));  // finished
    CHECK(!cardProgress(card(episode, lastDone, 43 * MIN, 44 * MIN)));
    CHECK(!cardProgress(card(episode, saved, 1, 0)));          // just advanced
    CHECK(!cardProgress(card(episode, saved, 500, 44 * MIN)));  // under a second is the start
    CHECK(!cardProgress(card(episode, saved, 20 * MIN, 0)));    // no length known
    CHECK(!cardProgress(card(show, saved, 20 * MIN, 44 * MIN)));

    // what A does
    CHECK(cardAction(card(movie, saved, 30 * MIN, 94 * MIN), false) == CardAction::Resume);
    CHECK(cardAction(card(movie, saved, 500, 94 * MIN), false) == CardAction::PlayFromStart);
    CHECK(cardAction(card(movie, saved, 90 * MIN, 94 * MIN), false) == CardAction::PlayFromStart);  // at its end
    CHECK(cardAction(card(episode, saved, 10 * MIN, 44 * MIN), false) == CardAction::Resume);
    CHECK(cardAction(card(episode, saved, 10 * MIN, 44 * MIN), true) == CardAction::Resume);
    CHECK(cardAction(card(episode, saved, 20 * MIN, 0), false) == CardAction::Resume);  // no length: as saved
    CHECK(cardAction(card(episode, saved, 1, 0), true) == CardAction::PlayFromStart);   // just advanced
    CHECK(cardAction(card(episode, upNext, 0, 44 * MIN), true) == CardAction::PlayFromStart);
    CHECK(cardAction(card(episode, lastDone, 43 * MIN, 44 * MIN), true) == CardAction::OpenShow);
    CHECK(cardAction(card(episode, saved, 43 * MIN, 44 * MIN), false) == CardAction::AskEpisodes);
    CHECK(cardAction(card(episode, saved, 43 * MIN, 44 * MIN), true) == CardAction::OpenShow);
    CHECK(cardAction(card(show, saved, 20 * MIN, 44 * MIN), false) == CardAction::OpenShow);

    if (failures == 0) printf("test_resume_card: all passed\n");
    return failures ? 1 : 0;
}

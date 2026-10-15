/*
    GMCA — Plex video player.
    Pipeline: PLEX_MIGRATION.md §2.7 (direct play, universal transcoder, timeline, scrobble).
*/

#pragma once

#include <borealis.hpp>
#include <utils/event.hpp>
#include <api/plex/types.hpp>
#ifdef USE_TORRENT
#include <memory>
#endif

class VideoView;
#ifdef USE_TORRENT
class TorrentLoadingView;
namespace torrent {
class Warmup;
}
#endif

class PlayerView : public brls::Box {
public:
    /// versionIndex selects which item.media[] source to play (default -1 = the
    /// first accessible version, i.e. unchanged Plex/Jellyfin behavior). The
    /// Stremio source picker passes an explicit index to honor the user's choice.
    PlayerView(const plex::Item& item, const int64_t seekMs = 0, int versionIndex = -1);
    ~PlayerView();

    /// As `seekMs`: play from the start, whatever position the item or the server has.
    static constexpr int64_t START_OVER = -1;

    /// Loads the show's episode list (previous/next navigation)
    void setSeries(const std::string& showRatingKey);
    void setTitie(const std::string& title);

#ifdef USE_TORRENT
    /// The torrent loading screen has the focus while it shows (B cancels).
    brls::View* getDefaultFocus() override;
    /// And gets it back, on the next frame, whenever the focus lands on the player under it
    /// (a dialog or an activity over the player closing hands the focus back to what had it
    /// before).
    void onChildFocusGained(brls::View* directChild, brls::View* focusedView) override;
#endif

#ifdef ANDROID
    void willDisappear(bool resetState) override {
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(235, 235, 235, 255));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(45, 45, 45, 255));
    }

    void willAppear(bool resetState) override {
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
    }
#endif

private:
    void setChapters(const std::vector<plex::Chapter>& chaps, int64_t durationMs);
    /// Fetches fresh metadata then resolves the playback URL via the backend
    void playMedia(const int64_t seekMs);
    /// Resolves the playback URL through the active backend (resolvePlayback,
    /// which decides direct vs transcode internally). forceDirect bypasses
    /// transcoding for the direct-play fallback after a transcode playback error
    /// (helps the Vita hardware decoder, which can choke on the transcoded stream).
    /// forceBitrate (bps, 0 = user setting) caps the stream for the
    /// transcode fallback after a direct-play error.
    void startPlayback(const int64_t seekMs, bool forceDirect = false, int64_t forceBitrate = 0);
    /// Tears the current Plex transcode session down server-side (no-op for
    /// direct play or non-Plex backends). Fire-and-forget; safe to call after
    /// `this` is gone.
    void stopTranscode();
    /// On a transcode playback error, retry once in direct play (helps Vita,
    /// where the hardware decoder can choke on the transcoded stream). Returns
    /// true when a fallback was started (so the error dialog is suppressed).
    bool tryDirectPlayFallback();
    /// On a direct-play error, retry once through the server transcoder: a
    /// server may refuse the raw part while a transcode session opens fine.
    /// Returns true when a fallback was started (so the error dialog is suppressed).
    bool tryTranscodeFallback();
    /// On a Plex direct-play error, retry once on the bare part URL, silently:
    /// a server may refuse ?download=1 to a user without download rights.
    bool tryPlainUrlFallback();
    /// On a Plex HTTP 403, explain the server's remote-playback rule instead of
    /// falling back to a transcode that the same rule refuses.
    bool explainRefusal();
    bool playIndex(int index);
    /// Resolves external subtitle sidecars for the current item through the
    /// backend (Stremio addons), lazily and only when the played item changes.
    /// Plex/Jellyfin embed theirs in the Media streams, so this is a no-op there.
    void resolveExternalSubtitles();
    /// sub-adds the resolved external subtitles into mpv, selecting the track
    /// matching the preferred-language setting (PLAYER_SUBTITLE_LANG). Called on
    /// every (re)load — mpv drops sub-add'ed tracks on each loadfile.
    void addExternalSubtitles();
    /// POST /:/timeline report (time/duration in ms)
    void reportTimeline(const std::string& state, int64_t timeMs);
    void reportStop();
    /// A file that ended (mpv's end of file, which reports no stop): its final position, once
    /// per load, before the next item or the close moves on.
    void reportEnd(int64_t timeMs);
    /// Marks as watched via /:/scrobble beyond the threshold (90%)
    void maybeScrobble(int64_t timeMs);
    bool toggleQuality();

    // Playback
    std::string itemId;  // ratingKey
    /// playMethod: "directplay" | "transcode" (VideoProfile display)
    std::string playMethod;
    /// stable play-session id for the whole playback session
    std::string sessionId;
    /// Plex universal-transcoder session, extracted from the resolved transcode
    /// URL so stopTranscode() can free it server-side. Empty for direct play or
    /// non-Plex backends. Regenerated (by the backend) on every (re)start.
    std::string transcodeSession;
    plex::Item item;     // fresh metadata (media/chapters/markers)
    plex::Media stream;  // selected version
    /// caller-chosen source index (Stremio picker); -1 = first accessible.
    /// Reset to -1 on episode switch so binge auto-picks the best source.
    int preferredVersion = -1;
    bool scrobbled = false;
    /// guards tryDirectPlayFallback so a failing stream falls back at most once
    /// per (re)load; reset by playMedia on every deliberate (re)start. Each
    /// fallback sets both flags, so a source that already failed is never retried.
    bool directPlayFallback = false;
    /// same guard for tryTranscodeFallback (direct play -> transcode)
    bool transcodeFallback = false;
    /// seekMs of the last startPlayback call: a load that fails to open leaves
    /// mpv playback_time at 0, so the fallbacks resume from this instead.
    int64_t lastSeekMs = 0;
    /// The first load of a Stremio title opened with no position: it starts where the account
    /// library says it stopped, which the detail brings (playMedia()).
    bool resumeSaved = false;
    /// reportEnd() ran for the current load (playMedia() starts a new one).
    bool endReported = false;
    /// The item's length, or mpv's when the item has none (Stremio episodes), in ms.
    int64_t durationMs() const;
    /// PlaybackSource::timelineOffsetMs of the current load; every position
    /// read from mpv goes through mediaTimeMs().
    int64_t timelineOffsetMs = 0;
    int64_t mediaTimeMs(double mpvTimeSec) const { return this->timelineOffsetMs + int64_t(mpvTimeSec) * 1000; }
    /// bitrate cap armed by tryTranscodeFallback (0 = none): once the transcoder
    /// rescued playback, later reloads stay on it. Cleared when the user picks a
    /// quality (toggleQuality).
    int64_t fallbackBitrate = 0;
    /// set by tryPlainUrlFallback: later reloads stay on the bare part URL
    bool plainPartUrl = false;
    std::vector<plex::Item> episodes;

    /// External subtitle sidecars (Stremio addons) for the current item, resolved
    /// lazily at play time and sub-add'ed on each (re)load. `externalSubsItem` is
    /// the ratingKey they belong to, so quality/track switches (same item) don't
    /// re-fetch while an episode switch does. `mpvLoaded` guards the async->sub-add
    /// timing (add on load OR when the fetch lands, whichever is last).
    std::vector<plex::Stream> externalSubs;
    std::string externalSubsItem;
    bool mpvLoaded = false;

    MPVEvent::Subscription eventSubscribeID;
    brls::VoidEvent::Subscription exitSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
    MPVCustomEvent::Subscription customEventSubscribeID;
    VideoView* view = nullptr;

#ifdef USE_TORRENT
    /// A torrent source (a local stream URL): shows the loading screen and buffers the
    /// start of the file, then calls startPlayback() again to play it. False for any other
    /// source, once the start is buffered, and for a torrent already warmed up in this
    /// player session (warmedUrl).
    bool warmUpTorrent(const int64_t seekMs, bool forceDirect, int64_t forceBitrate);
    /// mpv has data, the load failed or another source plays: the loading screen goes and
    /// the buffered start's reader with it (a warm-up still buffering is cancelled). Safe
    /// to call any time.
    void endTorrentLoading();
    /// mpv failed on a torrent source: says why in the engine's words, and closes.
    bool explainTorrentError();
    /// mpv reached the end of a torrent source's file: if well short of its duration, the
    /// engine broke the stream off, so it stops, says why and closes (true). False for any
    /// other source and for a file that played to its end.
    bool explainTorrentEnd();
    /// The focus is on this player, not on a dialog or an activity over it.
    bool focusInPlayer();

    TorrentLoadingView* torrentLoading = nullptr;
    std::shared_ptr<torrent::Warmup> warmup;  // the start being buffered, or held until mpv has data
    std::string warmupUrl;                    // the source it's for
    bool warmupReady = false;                 // its start is in: startPlayback() plays it
    int warmupGeneration = 0;                 // a warm-up's answer counts only if no newer load began
    bool focusRedirectPending = false;        // onChildFocusGained() has queued its redirect
    /// The local URL whose warm-up reported Ok in this player session: reloading it (a
    /// quality, audio or subtitle switch, a replay) goes straight to mpv. Cleared when
    /// another source plays or another torrent starts warming up.
    std::string warmedUrl;
#endif
};

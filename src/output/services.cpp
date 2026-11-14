#include "services.h"

#include <algorithm>
#include <cctype>

const std::vector<StreamService> &streamServices() {
    static const std::vector<StreamService> services = [] {
        std::vector<StreamService> s;
        {
            StreamService t;
            t.id = "twitch";
            t.name = "Twitch";
            t.servers = {{"Automatic (closest)", "rtmp://ingest.global-contribute.live-video.net/app"},
                         {"Automatic, encrypted (RTMPS)", "rtmps://ingest.global-contribute.live-video.net/app"}};
            t.videoCodecs = {"h264"};
            t.maxBitrateKbps = 6000;
            t.maxWidth = 1920;
            t.maxHeight = 1080;
            t.maxFps = 60;
            t.keyframeSeconds = 2;
            // Its broadcast documentation: the key followed by this query
            // takes the stream without showing it.
            t.testKeySuffix = "?bandwidthtest=true";
            t.domains = {"live-video.net", "twitch.tv"};
            s.push_back(t);
        }
        {
            StreamService y;
            y.id = "youtube";
            y.name = "YouTube";
            y.servers = {{"Primary, encrypted (RTMPS)", "rtmps://a.rtmps.youtube.com/live2"},
                         {"Backup, encrypted (RTMPS)", "rtmps://b.rtmps.youtube.com/live2?backup=1"},
                         {"Primary (RTMP)", "rtmp://a.rtmp.youtube.com/live2"},
                         {"HLS", "https://a.upload.youtube.com/http_upload_hls?cid={key}&copy=0&file=out.m3u8"}};
            y.videoCodecs = {"h264", "hevc", "av1"};
            y.maxBitrateKbps = 51000;
            y.maxWidth = 3840;
            y.maxHeight = 2160;
            y.maxFps = 60;
            y.keyframeSeconds = 2;
            y.domains = {"youtube.com"};
            s.push_back(y);
        }
        {
            StreamService f;
            f.id = "facebook";
            f.name = "Facebook Live";
            f.servers = {{"Default (RTMPS)", "rtmps://live-api-s.facebook.com:443/rtmp/"}};
            f.videoCodecs = {"h264"};
            f.maxBitrateKbps = 9000;
            f.maxWidth = 1920;
            f.maxHeight = 1080;
            f.maxFps = 60;
            f.keyframeSeconds = 2;
            f.domains = {"facebook.com"};
            s.push_back(f);
        }
        {
            StreamService r;
            r.id = "restream";
            r.name = "Restream";
            r.servers = {{"Automatic (closest)", "rtmp://live.restream.io/live"}};
            r.videoCodecs = {"h264"};
            r.keyframeSeconds = 2;
            r.domains = {"restream.io"};
            s.push_back(r);
        }
        return s;
    }();
    return services;
}

const StreamService *streamService(const std::string &id) {
    for (const StreamService &s : streamServices()) if (s.id == id) return &s;
    return nullptr;
}

const StreamService *serviceForServer(const std::string &server) {
    // scheme://[user@]host[:port]/…, lower-cased.
    const size_t scheme = server.find("://");
    std::string host = server.substr(scheme == std::string::npos ? 0 : scheme + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    if (const size_t at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
    host = host.substr(0, host.find(':'));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty()) return nullptr;
    for (const StreamService &s : streamServices())
        for (const std::string &domain : s.domains)
            if (host == domain || (host.size() > domain.size() && host.ends_with("." + domain))) return &s;
    return nullptr;
}

namespace {
// A key as it can sit in an address: what would end or break it there
// (spaces, controls, '#', bytes past ASCII) %-escaped; '?', '&' and '='
// stay, as services' own keys use them (a test mode, a query), and '%' too,
// so a key pasted already escaped is not escaped twice.
std::string addressSafe(const std::string &key) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : key) {
        if (c <= 0x20 || c >= 0x7f || c == '#' || c == '"' || c == '<' || c == '>' || c == '\\' ||
            c == '^' || c == '`' || c == '{' || c == '|' || c == '}') {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += char(c);
        }
    }
    return out;
}
}  // namespace

std::string streamUrl(const std::string &server, const std::string &given) {
    const std::string key = addressSafe(given);
    std::string url = server;
    const size_t at = url.find("{key}");
    if (at != std::string::npos) return url.replace(at, 5, key);
    if (key.empty()) return url;
    if (url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0) {
        // The key is the last path segment, before any query.
        const size_t query = url.find('?');
        std::string base = query == std::string::npos ? url : url.substr(0, query);
        const std::string rest = query == std::string::npos ? std::string() : url.substr(query);
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/" + key + rest;
    }
    if (url.rfind("srt://", 0) == 0 && url.find("streamid=") == std::string::npos)
        return url + (url.find('?') == std::string::npos ? "?" : "&") + "streamid=" + key;
    return url;
}

std::string withLogin(const std::string &url, const std::string &user, const std::string &password, std::string *why) {
    if (user.empty() && password.empty()) return url;
    auto fail = [&](const std::string &reason) { if (why) *why = reason; return std::string(); };
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return fail("the server address has no scheme");
    const std::string proto = url.substr(0, scheme);
    if (proto == "srt") {
        if (password.size() < 10 || password.size() > 79) return fail("an SRT passphrase is 10 to 79 characters");
        if (password.find_first_of("&#? ") != std::string::npos) return fail("an SRT passphrase cannot hold &, #, ? or spaces");
        return url + (url.find('?') == std::string::npos ? "?" : "&") + "passphrase=" + password;
    }
    if (proto != "rtmp" && proto != "rtmps" && proto != "http" && proto != "https")
        return fail("this kind of server takes no name and password");
    // Read back by splitting at the first '/' and the last '@' before it, and
    // the name at the first ':'.
    if (user.find_first_of(":@/?# ") != std::string::npos) return fail("the name cannot hold :, @, /, ?, # or spaces");
    if (password.find_first_of("/?# ") != std::string::npos) return fail("the password cannot hold /, ?, # or spaces");
    const size_t host = scheme + 3;
    const size_t path = url.find_first_of("/?#", host);
    if (url.substr(host, path == std::string::npos ? std::string::npos : path - host).find('@') != std::string::npos)
        return fail("the server address already holds a sign-in");
    return url.substr(0, host) + user + (password.empty() ? "" : ":" + password) + "@" + url.substr(host);
}

std::vector<std::string> serviceWarnings(const StreamService &service, const std::string &codec, int width, int height,
                                         double fps, int bitrateKbps, int keyframeSeconds) {
    std::vector<std::string> out;
    auto codecName = [](const std::string &c) { return c == "av1" ? std::string("AV1") : c == "hevc" ? std::string("HEVC") : std::string("H.264"); };
    if (!service.videoCodecs.empty() &&
        std::find(service.videoCodecs.begin(), service.videoCodecs.end(), codec) == service.videoCodecs.end()) {
        std::string takes;
        for (size_t i = 0; i < service.videoCodecs.size(); ++i)
            takes += (i ? (i + 1 == service.videoCodecs.size() ? " or " : ", ") : "") + codecName(service.videoCodecs[i]);
        out.push_back(service.name + " takes " + takes + ", not " + codecName(codec) + ".");
    }
    if (service.maxBitrateKbps > 0 && bitrateKbps > service.maxBitrateKbps)
        out.push_back(service.name + " recommends at most " + std::to_string(service.maxBitrateKbps) + " kb/s.");
    if (service.maxWidth > 0 && (width > service.maxWidth || height > service.maxHeight))
        out.push_back(service.name + " takes up to " + std::to_string(service.maxWidth) + " × " + std::to_string(service.maxHeight) + ".");
    if (service.maxFps > 0 && fps > service.maxFps + 0.01)
        out.push_back(service.name + " takes up to " + std::to_string(service.maxFps) + " frames per second.");
    if (service.keyframeSeconds > 0 && (keyframeSeconds <= 0 || keyframeSeconds > service.keyframeSeconds))
        out.push_back(service.name + " asks for a keyframe every " + std::to_string(service.keyframeSeconds) + " seconds.");
    return out;
}

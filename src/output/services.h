#pragma once

#include <string>
#include <vector>

// Where people stream to, from each service's own published setup notes.
// A server URL may hold "{key}" where the stream key goes; otherwise the key
// follows the server as the protocol expects (see streamUrl()).
struct StreamService {
    std::string id;
    std::string name;
    struct Server {
        std::string name;
        std::string url;
    };
    std::vector<Server> servers;
    // What it takes; empty means anything the protocol carries.
    std::vector<std::string> videoCodecs;   // "h264", "hevc", "av1"
    int maxBitrateKbps = 0;                 // its recommended ceiling; 0 = none given
    int maxWidth = 0, maxHeight = 0, maxFps = 0;
    int keyframeSeconds = 0;                // what it asks for; 0 = no rule
    std::string keyLabel = "Stream key";    // what the service calls it
    // Added to the stream key, it sends without going live (no viewers, no
    // notifications): what an upload test may use. Only where the service
    // documents one; empty means a test would go live.
    std::string testKeySuffix;
    // Where its servers are (any host in these): a server typed in by hand is
    // known as this service's by them.
    std::vector<std::string> domains;
};

const std::vector<StreamService> &streamServices();
// The service by id; nullptr for "custom" or anything unknown.
const StreamService *streamService(const std::string &id);
// The service whose server this address is on; nullptr when none is.
const StreamService *serviceForServer(const std::string &server);

// The address to send to: "{key}" replaced, or the key added as RTMP and SRT
// expect it (a path segment, the SRT streamid). WHIP takes its key as a
// bearer token instead, so it is left off the URL.
std::string streamUrl(const std::string &server, const std::string &key);

// The address with a sign-in: "name:password@" for RTMP(S) and HTTP(S), which
// the server asks for when it wants it, or the password as SRT's passphrase
// (the name is not used). Empty, with why, when it cannot be carried.
std::string withLogin(const std::string &url, const std::string &user, const std::string &password, std::string *why);

// What a service would refuse or warn about in these settings, for people.
std::vector<std::string> serviceWarnings(const StreamService &service, const std::string &codec, int width, int height,
                                         double fps, int bitrateKbps, int keyframeSeconds);

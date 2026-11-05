#include "adb_parse.h"

#include <algorithm>
#include <cctype>

#include "util.h"

namespace vr::adb {

bool Device::isQuest() const {
    static const char* const kProducts[] = {"eureka", "panther", "seacliff", "hollywood", "monterey"};
    if (containsIgnoreCase(model, "quest")) return true;
    for (const char* p : kProducts) {
        if (product == p) return true;
    }
    return false;
}

std::vector<Device> parseDevices(const std::string& output) {
    std::vector<Device> devices;
    for (const std::string& raw : splitLines(output)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '*' || startsWith(line, "List of devices") || startsWith(line, "adb ")) continue;
        const auto tokens = splitWhitespace(line);
        if (tokens.size() < 2) continue;
        Device d;
        d.serial = tokens[0];
        const std::string& s = tokens[1];
        if (s == "device") d.state = Device::State::Device;
        else if (s == "unauthorized") d.state = Device::State::Unauthorized;
        else if (s == "authorizing") d.state = Device::State::Authorizing;
        else if (s == "offline") d.state = Device::State::Offline;
        else if (s == "no" && tokens.size() > 2 && startsWith(tokens[2], "permissions")) d.state = Device::State::NoPermissions;
        else d.state = Device::State::Other;
        for (const std::string& t : tokens) {
            if (startsWith(t, "model:")) {
                d.model = t.substr(6);
                std::replace(d.model.begin(), d.model.end(), '_', ' ');
            } else if (startsWith(t, "product:")) {
                d.product = t.substr(8);
            }
        }
        devices.push_back(std::move(d));
    }
    return devices;
}

Selection select(const std::vector<Device>& devices, const std::optional<std::string>& preferredSerial) {
    std::vector<Device> candidates;
    for (const Device& d : devices) {
        if (!preferredSerial || d.serial == *preferredSerial) candidates.push_back(d);
    }
    std::vector<Device> ready;
    for (const Device& d : candidates) {
        if (d.state == Device::State::Device) ready.push_back(d);
    }
    Selection sel;
    const int others = int(devices.size()) - 1;
    const Device* quest = nullptr;
    for (const Device& d : ready) {
        if (d.isQuest() && (!quest || d.serial < quest->serial)) quest = &d;
    }
    if (quest) {
        sel.kind = Selection::Kind::Ready;
        sel.device = *quest;
        sel.others = others;
        return sel;
    }
    const auto first = [&](auto pred) -> const Device* {
        for (const Device& d : candidates) {
            if (pred(d)) return &d;
        }
        return nullptr;
    };
    if (auto d = first([](const Device& x) { return x.state == Device::State::Unauthorized; })) {
        sel.kind = Selection::Kind::Unauthorized;
        sel.device = *d;
        return sel;
    }
    if (auto d = first([](const Device& x) {
            return x.state == Device::State::Offline || x.state == Device::State::NoPermissions;
        })) {
        sel.kind = Selection::Kind::Offline;
        sel.device = *d;
        return sel;
    }
    if (auto d = first([](const Device& x) { return x.state == Device::State::Authorizing; })) {
        sel.kind = Selection::Kind::Authorizing;
        sel.device = *d;
        return sel;
    }
    if (ready.size() == 1) {
        sel.kind = Selection::Kind::Ready;
        sel.device = ready[0];
        sel.others = others;
        return sel;
    }
    if (ready.size() > 1) {
        sel.kind = Selection::Kind::Ambiguous;
        sel.count = int(ready.size());
    }
    return sel;
}

std::vector<ReverseEntry> parseReverseList(const std::string& output) {
    std::vector<ReverseEntry> entries;
    for (const std::string& line : splitLines(output)) {
        const auto tokens = splitWhitespace(line);
        if (tokens.size() < 2) continue;
        const std::string& remote = tokens[tokens.size() - 2];
        const std::string& local = tokens[tokens.size() - 1];
        if (remote.find(':') == std::string::npos || local.find(':') == std::string::npos) continue;
        entries.push_back({remote, local});
    }
    return entries;
}

ReverseDecision decideReverse(const std::vector<ReverseEntry>& entries, int port, std::string* existingLocal) {
    const std::string spec = "tcp:" + std::to_string(port);
    for (const ReverseEntry& e : entries) {
        if (e.remote != spec) continue;
        if (e.local == spec) return ReverseDecision::Present;
        if (existingLocal) *existingLocal = e.local;
        return ReverseDecision::Replace;
    }
    return ReverseDecision::Create;
}

std::string parseVersionName(const std::string& output) {
    for (const std::string& line : splitLines(output)) {
        const std::string t = trim(line);
        if (startsWith(t, "versionName=")) return t.substr(12);
    }
    return {};
}

std::optional<int> compareVersions(const std::string& a, const std::string& b) {
    const auto parts = [](const std::string& s) {
        std::vector<int> numbers;
        size_t i = 0;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
            if (s[i] == '.') {
                ++i;
                continue;
            }
            int v = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
            numbers.push_back(v);
        }
        return numbers;
    };
    const auto x = parts(a), y = parts(b);
    if (x.empty() || y.empty()) return std::nullopt;
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int l = i < x.size() ? x[i] : 0, r = i < y.size() ? y[i] : 0;
        if (l != r) return l < r ? -1 : 1;
    }
    return 0;
}

std::optional<std::string> installFailure(const std::string& output) {
    std::vector<std::string> lines;
    for (const std::string& l : splitLines(output)) lines.push_back(trim(l));
    for (const std::string& l : lines) {
        if (l == "Success") return std::nullopt;
    }
    for (const std::string& l : lines) {
        if (startsWith(l, "Failure") || l.find("INSTALL_FAILED") != std::string::npos || startsWith(l, "adb: failed")) {
            const size_t open = l.find('['), close = l.find(']', open == std::string::npos ? 0 : open);
            if (open != std::string::npos && close != std::string::npos) {
                std::string inner = l.substr(open + 1, close - open - 1);
                const size_t colon = inner.find(':');
                return trim(colon == std::string::npos ? inner : inner.substr(0, colon));
            }
            return l;
        }
    }
    return lines.empty() ? std::string("no output") : lines.back();
}

TravelMode parseTravelMode(const std::string& output) {
    for (const std::string& line : splitLines(output)) {
        const std::string t = trim(line);
        if (!startsWith(t, "* Travel Mode:")) continue;
        std::string v = trim(t.substr(14));
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (v == "true") return TravelMode::On;
        if (v == "false") return TravelMode::Off;
        return TravelMode::FormatChanged;
    }
    return TravelMode::FormatChanged;
}

}  // namespace vr::adb

#pragma once

#include <curl/curl.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "HTTPClient.h"
#include "NetworkClient.h"
#include "WString.h"

namespace freeink {

class SecureHttpClient {
 public:
  SecureHttpClient() {
    http_.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    setTimeout(15000);  // SDK default
  }
  using DataCallback = std::function<bool(const uint8_t*, size_t)>;
  using AbortCallback = std::function<bool()>;

  void setCACert(const char*) {}
  void setInsecure() { insecure_ = true; }
  void setReserveFullRecord(bool) {}
  void setUserAgent(const std::string& agent) {
    userAgent_ = agent;
    http_.addHeader("User-Agent", agent.c_str());
  }
  void setBasicAuth(const std::string& user, const std::string& pass) {
    authUser_ = user;
    authPass_ = pass;
    http_.setAuthorization(user.c_str(), pass.c_str());
  }
  void clearBasicAuth() { setBasicAuth("", ""); }

  // Like the SDK, begin() drops the previous request's headers; the user agent
  // and basic auth are client settings and stay.
  bool begin(const String& url) {
    streaming_ = false;
    url_ = url.c_str();
    requestHeaders_.clear();
    http_.end();
    http_.begin(client_, url.c_str());
    http_.addHeader("User-Agent", userAgent_.c_str());
    if (!authUser_.empty()) http_.setAuthorization(authUser_.c_str(), authPass_.c_str());
    callbackStopped_ = false;
    aborted_ = false;
    return !url.isEmpty();
  }

  bool begin(const std::string& url) { return begin(String(url)); }
  bool begin(const char* url) { return begin(String(url)); }

  // curl connects inside each request, so there is nothing to open early.
  bool connect(const AbortCallback& shouldAbort = nullptr) {
    aborted_ = shouldAbort && shouldAbort();
    return !aborted_;
  }

  void end() { http_.end(); }

  void addHeader(const char* name, const String& value) { addHeader(name, value.c_str()); }

  void addHeader(const char* name, const std::string& value) { addHeader(name, value.c_str()); }

  void addHeader(const char* name, const char* value) {
    requestHeaders_[name] = value;
    http_.addHeader(name, value);
  }
  void addHeader(const std::string& name, const std::string& value) { addHeader(name.c_str(), value.c_str()); }

  // Like the SDK, one value limits both connecting and each wait for data.
  void setTimeout(uint32_t ms) {
    timeoutMs_ = ms;
    http_.setConnectTimeout(static_cast<int32_t>(ms));
    http_.setTimeout(static_cast<uint16_t>(ms > UINT16_MAX ? UINT16_MAX : ms));
  }
  void setReuse(bool reuse) { http_.setReuse(reuse); }

  int GET() { return http_.GET(); }
  int POST(const String& payload) { return http_.POST(payload.c_str()); }
  int sendRequest(const char* method, const String& payload) { return http_.sendRequest(method, payload); }
  int sendRequest(const char* method, const std::string& payload) { return sendRequest(method, String(payload)); }

  int sendRequest(const char* method, const uint8_t* payload, size_t len, const DataCallback& onData,
                  const AbortCallback& shouldAbort = nullptr) {
    // Preserve canned routes used by existing offline simulator workflows.
    if (std::getenv("CROSSPOINT_SIM_HTTP_MOCK_ROOT") || std::getenv("CROSSPOINT_SIM_HTTP_ROUTES")) {
      streaming_ = false;
      aborted_ = shouldAbort && shouldAbort();
      if (aborted_) return -1;
      const std::string body = len ? std::string(reinterpret_cast<const char*>(payload), len) : std::string();
      const int status = sendRequest(method, body);
      const String response = http_.getString();
      if (status > 0 && onData && !response.isEmpty())
        callbackStopped_ = !onData(reinterpret_cast<const uint8_t*>(response.c_str()), response.length());
      aborted_ = shouldAbort && shouldAbort();
      return status;
    }
    streaming_ = true;
    streamStatus_ = 0;
    streamComplete_ = callbackStopped_ = aborted_ = false;
    responseHeaders_.clear();
    CURL* curl = curl_easy_init();
    if (!curl || (len && !payload)) {
      if (curl) curl_easy_cleanup(curl);
      return -1;
    }
    struct Call {
      SecureHttpClient* self;
      const DataCallback* data;
      const AbortCallback* abort;
    } call{this, &onData, &shouldAbort};
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeoutMs_));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>((timeoutMs_ + 999) / 1000));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (insecure_) {
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (!authUser_.empty()) {
      curl_easy_setopt(curl, CURLOPT_USERNAME, authUser_.c_str());
      curl_easy_setopt(curl, CURLOPT_PASSWORD, authPass_.c_str());
    }
    if (payload) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(len));
    }
    curl_slist* headers = nullptr;
    for (const auto& h : requestHeaders_) headers = curl_slist_append(headers, (h.first + ": " + h.second).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &call);
    curl_easy_setopt(
        curl, CURLOPT_HEADERFUNCTION, +[](char* ptr, size_t size, size_t count, void* context) -> size_t {
          auto& self = *static_cast<Call*>(context)->self;
          const size_t bytes = size * count;
          std::string line(ptr, bytes);
          if (line.rfind("HTTP/", 0) == 0) {
            self.responseHeaders_.clear();
            const size_t space = line.find(' ');
            self.streamStatus_ = space == std::string::npos ? 0 : std::atoi(line.c_str() + space + 1);
          } else if (const size_t colon = line.find(':'); colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const size_t start = line.find_first_not_of(" \t", colon + 1);
            const size_t end = line.find_last_not_of(" \t\r\n");
            self.responseHeaders_[key] =
                start == std::string::npos || end < start ? "" : line.substr(start, end - start + 1);
          }
          return bytes;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &call);
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t count, void* context) -> size_t {
          auto& call = *static_cast<Call*>(context);
          const size_t bytes = size * count;
          if (*call.abort && (*call.abort)()) {
            call.self->aborted_ = true;
            return 0;
          }
          if (*call.data && !(*call.data)(reinterpret_cast<const uint8_t*>(ptr), bytes)) {
            call.self->callbackStopped_ = true;
            return 0;
          }
          return bytes;
        });
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &call);
    curl_easy_setopt(
        curl, CURLOPT_XFERINFOFUNCTION, +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
          auto& call = *static_cast<Call*>(context);
          if (*call.abort && (*call.abort)()) {
            call.self->aborted_ = true;
            return 1;
          }
          return 0;
        });
    const CURLcode result = curl_easy_perform(curl);
    streamComplete_ = result == CURLE_OK;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return streamStatus_ ? streamStatus_ : -1;
  }

  int GET(const DataCallback& onData, const AbortCallback& shouldAbort = nullptr) {
    return sendRequest("GET", nullptr, 0, onData, shouldAbort);
  }

  String getString() { return http_.getString(); }
  int getSize() { return http_.getSize(); }
  int getStatus() const { return streaming_ ? streamStatus_ : http_.getStatus(); }
  bool hasContentLength() const {
    return streaming_ ? responseHeaders_.count("content-length") != 0 : http_.hasContentLength();
  }
  size_t getContentLength() const {
    return streaming_ ? std::strtoull(getHeader("content-length").c_str(), nullptr, 10) : http_.getContentLength();
  }
  std::string getHeader(const std::string& name) const {
    if (!streaming_) return http_.getHeader(name);
    const auto it = responseHeaders_.find(name);
    return it == responseHeaders_.end() ? "" : it->second;
  }
  bool responseComplete() const {
    return (streaming_ ? streamComplete_ : http_.responseComplete()) && !callbackStopped_ && !aborted_;
  }
  bool aborted() const { return aborted_; }
  std::vector<std::pair<std::string, std::string>> getHeaders() const {
    const auto& headers = streaming_ ? responseHeaders_ : http_.getHeaders();
    return {headers.begin(), headers.end()};
  }

  static bool resolveUrl(const std::string& baseUrl, const std::string& location, std::string& resolved) {
    if (location.find("://") != std::string::npos) {
      resolved = location;
      return true;
    }
    const size_t schemeEnd = baseUrl.find("://");
    if (schemeEnd == std::string::npos) return false;
    const size_t pathStart = baseUrl.find_first_of("/?#", schemeEnd + 3);
    const std::string origin = baseUrl.substr(0, pathStart);
    const std::string scheme = baseUrl.substr(0, schemeEnd);
    if (location.rfind("//", 0) == 0) {
      resolved = scheme + ":" + location;
    } else if (!location.empty() && location.front() == '/') {
      resolved = origin + location;
    } else {
      std::string path = pathStart == std::string::npos ? "/" : baseUrl.substr(pathStart);
      path.erase(path.find_first_of("?#") == std::string::npos ? path.size() : path.find_first_of("?#"));
      const size_t slash = path.rfind('/');
      resolved = origin + (slash == std::string::npos ? "/" : path.substr(0, slash + 1)) + location;
    }
    return true;
  }

  static bool tls13Available() { return true; }

 private:
  bool streaming_ = false, streamComplete_ = false, insecure_ = false;
  int streamStatus_ = 0;
  uint32_t timeoutMs_ = 15000;
  std::string url_;
  std::map<std::string, std::string> requestHeaders_, responseHeaders_;

  NetworkClientSecure client_;
  HTTPClient http_;
  std::string userAgent_ = "FreeInk-ESP32";  // SDK default
  std::string authUser_;
  std::string authPass_;
  bool callbackStopped_ = false;
  bool aborted_ = false;
};

}  // namespace freeink

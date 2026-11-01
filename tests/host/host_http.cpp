#include "HTTPClient.h"

namespace hostio {

namespace {
FakeHttp instance;
}

FakeHttp &http() { return instance; }

void resetHttp() { instance = FakeHttp(); }

const HttpRequest *FakeHttp::find(const std::string &method,
                                 const std::string &pathSuffix) const {
    for (const HttpRequest &request : requests) {
        if (request.method == method &&
            request.url.size() >= pathSuffix.size() &&
            request.url.compare(request.url.size() - pathSuffix.size(),
                                pathSuffix.size(), pathSuffix) == 0) {
            return &request;
        }
    }
    return nullptr;
}

size_t FakeHttp::count(const std::string &method,
                       const std::string &pathSuffix) const {
    size_t found = 0;
    for (const HttpRequest &request : requests) {
        if (request.method == method &&
            request.url.size() >= pathSuffix.size() &&
            request.url.compare(request.url.size() - pathSuffix.size(),
                                pathSuffix.size(), pathSuffix) == 0) {
            ++found;
        }
    }
    return found;
}

HttpResponse FakeHttp::take() {
    if (responses.empty()) {
        return HttpResponse{200, "{}"};
    }
    if (nextResponse >= responses.size()) {
        return responses.back();
    }
    return responses[nextResponse++];
}

}  // namespace hostio

bool HTTPClient::begin(WiFiClient &client, const String &url) {
    (void)client;
    url_ = url.c_str();
    response_.clear();
    contentType_.clear();
    open_ = url_.rfind("http://", 0) == 0 || url_.rfind("https://", 0) == 0;
    return open_;
}

void HTTPClient::end() { open_ = false; }

void HTTPClient::addHeader(const String &name, const String &value) {
    if (String("Content-Type") == name) {
        contentType_ = value.c_str();
    }
}

int HTTPClient::sendRequest(const char *method, uint8_t *payload, size_t size) {
    hostio::HttpRequest request;
    request.method = method;
    request.url = url_;
    request.contentType = contentType_;
    if (payload != nullptr && size > 0) {
        request.body.assign(reinterpret_cast<const char *>(payload), size);
    }
    hostio::http().requests.push_back(request);

    if (hostio::http().transportError != 0) {
        return hostio::http().transportError;
    }
    const hostio::HttpResponse response = hostio::http().take();
    response_ = response.body;
    return response.code;
}

String HTTPClient::errorToString(int error) {
    switch (error) {
        case HTTPC_ERROR_CONNECTION_REFUSED:
            return String("connection refused");
        case HTTPC_ERROR_READ_TIMEOUT:
            return String("read timeout");
        default:
            return String("transport error");
    }
}

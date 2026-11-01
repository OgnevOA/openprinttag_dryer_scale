// Host stand-in for HTTPClient and WiFiClient, backed by a fake Spoolman the
// tests can program.
//
// It records every request -- method, path and body -- and answers from a queue
// of canned responses, which is what makes the decisions testable: whether a
// measurement was sent at all, what weight went in it, and how a 404 or an
// unreachable server is reported. Nothing here parses HTTP; the real client's
// job is the transport, and the transport is the part a desktop test cannot
// check anyway.

#pragma once

#include <Arduino.h>
#include <WiFiClient.h>

#include <stdint.h>

#include <string>
#include <vector>

namespace hostio {

struct HttpRequest {
    std::string method;
    std::string url;
    std::string body;
    std::string contentType;
};

struct HttpResponse {
    int code = 200;
    std::string body;
};

struct FakeHttp {
    std::vector<HttpRequest> requests;

    // Answers are taken in order; the last one is repeated once the queue runs
    // dry, so a test that only cares about the first call does not have to
    // enumerate the rest.
    std::vector<HttpResponse> responses;
    size_t nextResponse = 0;

    // Set to a negative HTTPClient error code to simulate a server that is not
    // there, which is the failure this integration will actually meet.
    int transportError = 0;

    const HttpRequest *find(const std::string &method,
                            const std::string &pathSuffix) const;
    size_t count(const std::string &method,
                 const std::string &pathSuffix) const;
    HttpResponse take();
};

FakeHttp &http();
void resetHttp();

}  // namespace hostio

// The error codes the real client returns, only the ones worth simulating.
#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_READ_TIMEOUT (-11)

class HTTPClient {
public:
    bool begin(WiFiClient &client, const String &url);
    void end();

    void setTimeout(uint16_t ms) { (void)ms; }
    void setConnectTimeout(int32_t ms) { (void)ms; }
    void setReuse(bool reuse) { (void)reuse; }
    void addHeader(const String &name, const String &value);

    int sendRequest(const char *method, uint8_t *payload, size_t size);
    String getString() const { return String(response_.c_str()); }

    static String errorToString(int error);

private:
    std::string url_;
    std::string contentType_;
    std::string response_;
    bool open_ = false;
};

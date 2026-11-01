// Host stand-in for the ESP32 core's WebServer, with a way to synthesise a
// request so the portal's pages and forms can be tested without a browser.
//
// The provisioning form is the most dangerous input the device has -- it decides
// what gets written to a tag -- so being able to submit it from a test is worth
// more than any amount of manual clicking.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"

#define HTTP_GET 1
#define HTTP_POST 3
#define HTTP_ANY 255

#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)

enum HTTPUploadStatus {
    UPLOAD_FILE_START,
    UPLOAD_FILE_WRITE,
    UPLOAD_FILE_END,
    UPLOAD_FILE_ABORTED
};

struct HTTPUpload {
    HTTPUploadStatus status = UPLOAD_FILE_START;
    std::string filename;
    size_t currentSize = 0;
    size_t totalSize = 0;
    uint8_t *buf = nullptr;
};

class WebServer {
public:
    using Handler = std::function<void()>;

    explicit WebServer(uint16_t port = 80) : port_(port) {}

    void on(const char *uri, Handler handler);
    void on(const char *uri, int method, Handler handler);
    void on(const char *uri, int method, Handler handler, Handler uploadHandler);
    void onNotFound(Handler handler);
    void begin();
    void handleClient();
    void close();

    void send(int code, const char *contentType, const String &content);
    void send(int code, const char *contentType, const char *content);
    void sendContent(const String &content);
    void sendContent(const char *content, size_t size);
    void sendHeader(const char *name, const char *value, bool first = false);
    void setContentLength(size_t length);

    String arg(const char *name);
    String arg(int index);
    String argName(int index);
    bool hasArg(const char *name);
    int args();
    String uri();
    int method();
    HTTPUpload &upload();

    // Host-only: dispatches a request and returns what the handler produced.
    struct Response {
        bool handled = false;
        int code = 0;
        std::string contentType;
        std::string body;
        std::map<std::string, std::string> headers;
    };
    Response dispatch(const std::string &path, int method,
                      const std::vector<std::pair<std::string, std::string>>
                          &params = {});

private:
    struct Route {
        std::string uri;
        int method;
        Handler handler;
    };

    uint16_t port_;
    std::vector<Route> routes_;
    Handler notFound_;
    HTTPUpload upload_;

    std::vector<std::pair<std::string, std::string>> params_;
    std::string uri_;
    int method_ = HTTP_GET;
    Response response_;
};

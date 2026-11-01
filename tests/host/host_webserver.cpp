// Implementation of the host WebServer stub and the Update stand-in.

#include "WebServer.h"

#include "Update.h"

void WebServer::on(const char *uri, Handler handler) {
    routes_.push_back({uri, HTTP_ANY, handler});
}

void WebServer::on(const char *uri, int method, Handler handler) {
    routes_.push_back({uri, method, handler});
}

void WebServer::on(const char *uri, int method, Handler handler,
                   Handler uploadHandler) {
    routes_.push_back({uri, method, handler});
    // The upload handler is never invoked here: no test streams a firmware
    // image, and pretending to would prove nothing about the real flash path.
    (void)uploadHandler;
}

void WebServer::onNotFound(Handler handler) { notFound_ = handler; }
void WebServer::begin() {}
void WebServer::handleClient() {}
void WebServer::close() {}

void WebServer::send(int code, const char *contentType, const String &content) {
    response_.code = code;
    response_.contentType = contentType ? contentType : "";
    response_.body = content.c_str();
}

void WebServer::send(int code, const char *contentType, const char *content) {
    send(code, contentType, String(content));
}

void WebServer::sendContent(const String &content) {
    // Chunked replies land in the same body a plain send() would produce, so a
    // test reads them the same way regardless of how they were transmitted.
    response_.body += content.c_str();
}

void WebServer::sendContent(const char *content, size_t size) {
    // Length-counted, so binary payloads with embedded zeroes survive intact.
    response_.body.append(content, size);
}

void WebServer::sendHeader(const char *name, const char *value, bool) {
    response_.headers[name ? name : ""] = value ? value : "";
}

void WebServer::setContentLength(size_t) {}

String WebServer::arg(const char *name) {
    for (const auto &parameter : params_) {
        if (parameter.first == name) {
            return String(parameter.second);
        }
    }
    return String("");
}

String WebServer::arg(int index) {
    if (index < 0 || static_cast<size_t>(index) >= params_.size()) {
        return String("");
    }
    return String(params_[static_cast<size_t>(index)].second);
}

String WebServer::argName(int index) {
    if (index < 0 || static_cast<size_t>(index) >= params_.size()) {
        return String("");
    }
    return String(params_[static_cast<size_t>(index)].first);
}

bool WebServer::hasArg(const char *name) {
    for (const auto &parameter : params_) {
        if (parameter.first == name) {
            return true;
        }
    }
    return false;
}

int WebServer::args() { return static_cast<int>(params_.size()); }
String WebServer::uri() { return String(uri_); }
int WebServer::method() { return method_; }
HTTPUpload &WebServer::upload() { return upload_; }

WebServer::Response WebServer::dispatch(
    const std::string &path, int method,
    const std::vector<std::pair<std::string, std::string>> &params) {
    params_ = params;
    uri_ = path;
    method_ = method;
    response_ = Response{};

    for (const Route &route : routes_) {
        if (route.uri != path) {
            continue;
        }
        if (route.method != HTTP_ANY && route.method != method) {
            continue;
        }
        response_.handled = true;
        route.handler();
        return response_;
    }
    if (notFound_) {
        response_.handled = true;
        notFound_();
    }
    return response_;
}

// ---------------------------------------------------------------------------
// Update: accepts everything and does nothing, which is all a host build can
// honestly claim about flashing.
// ---------------------------------------------------------------------------
UpdateClass Update;

bool UpdateClass::begin(size_t) { return true; }
size_t UpdateClass::write(uint8_t *, size_t size) { return size; }
bool UpdateClass::end(bool) { return true; }
void UpdateClass::abort() {}
bool UpdateClass::hasError() { return false; }
const char *UpdateClass::errorString() { return "no error"; }

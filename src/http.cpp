#include "http.hpp"

#include <cassert>

#include "config.hpp"

std::optional<Method> parseMethod(std::string_view method)
{
    // RFC2616, 5.1.1: "The method is case-sensitive"
    if (method == "GET") {
        return Method::Get;
    } else if (method == "HEAD") {
        return Method::Head;
    } else if (method == "POST") {
        return Method::Post;
    } else if (method == "PUT") {
        return Method::Put;
    } else if (method == "DELETE") {
        return Method::Delete;
    } else if (method == "CONNECT") {
        return Method::Connect;
    } else if (method == "OPTIONS") {
        return Method::Options;
    } else if (method == "TRACE") {
        return Method::Trace;
    } else if (method == "PATCH") {
        return Method::Patch;
    }

    return std::nullopt;
}

namespace {
std::string removeDotSegments(std::string_view input)
{
    // RFC3986, 5.2.4: Remove Dot Segments
    // This algorithm is a bit different, because of the following assert (ensured in Url::parse).
    // If we leave the trailing slashes in the input buffer, we know that after every step in the
    // loop below, inputLeft still starts with a slash.
    assert(!input.empty() && input[0] == '/');
    std::string output;
    output.reserve(input.size());
    while (!input.empty()) {
        assert(input[0] == '/');

        if (input == "/") {
            output.push_back('/');
            break;
        } else {
            // I think it's not very clear, why this works in all cases, but if I go through all
            // cases one by one instead, it's just a bunch of ifs with the same code in each branch.
            const auto segmentLength = input.find('/', 1);
            const auto segment = input.substr(0, segmentLength);

            if (segment == "/.") {
                // do nothing
            } else if (segment == "/..") {
                // Removing trailing segment (including slash) from output buffer
                const auto lastSlash = output.rfind('/');
                if (lastSlash != std::string::npos) {
                    output.resize(lastSlash);
                } else {
                    // Considering that every segment starts with a slash, output must be empty
                    assert(output.empty());
                }
            } else {
                output.append(segment);
            }

            if (segmentLength == std::string_view::npos) {
                break;
            } else {
                input = input.substr(segmentLength);
            }
        }
    }
    if (output.empty()) {
        output.push_back('/');
    }
    return output;
}

bool isAlphaNum(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (ch >= 'A' || ch <= 'Z');
}

bool isSchemeChar(char ch)
{
    return isAlphaNum(ch) || ch == '+' || ch == '.' || ch == '-';
}
}

std::optional<Url> Url::parse(std::string_view urlStr)
{
    constexpr auto npos = std::string_view::npos;

    Url url;
    url.fullRaw = urlStr;

    // see RFC2515, 5.1.2
    if (url.fullRaw == "*") {
        return url;
    }

    // I don't *actually* support CONNECT, so I will not parse authority URIs.

    // RFC1808, 2.4.1: The fragment is not technically part of the URL
    const auto fragmentStart = urlStr.find('#');
    if (fragmentStart != npos) {
        url.fragment = urlStr.substr(fragmentStart + 1);
        urlStr = urlStr.substr(0, fragmentStart);
    }

    if (urlStr.empty()) {
        return std::nullopt;
    }

    // The other possible URLs are absoluteURI and abs_path and I have to parse absoluteURI:
    // "To allow for transition to absoluteURIs in all requests in future
    // versions of HTTP, all HTTP / 1.1 servers MUST accept the absoluteURI form in requests, even
    // though HTTP/1.1 clients will only generate them in requests to proxies."
    // I won't save any of the URI components that are part of absoluteURI (and not abs_path)
    // because I don't need them (even though I should).
    const auto colon = urlStr.find(':');
    if (colon != npos) {
        // RFC1808, 2.4.2: If all characters up to this colon are valid characters for a scheme,
        // [0, colon) is a scheme.
        bool isScheme = true;
        for (size_t i = 0; i < colon; ++i) {
            if (!isSchemeChar(urlStr[i])) {
                isScheme = false;
                break;
            }
        }

        if (isScheme) {
            // If we wanted to save the scheme
            urlStr = urlStr.substr(colon + 1);
        }
    }

    // RFC1808, 2.4.3
    if (urlStr.size() >= 2 && urlStr.substr(0, 2) == "//") {
        // I MUST (RFC2616, 5.2) with 400 if net_loc does not contain a valid host for this server,
        // but I don't want to add configuration for this, so I choose to be more "lenient" here and
        // ignore it completely. choose to be more "lenient" here and simply ignore it completely.
        urlStr = urlStr.substr(2, urlStr.find("/", 2));
    }

    // RFC1808, 2.4.4
    const auto queryStart = urlStr.find('?');
    if (queryStart != npos) {
        url.query = urlStr.substr(queryStart + 1);
        urlStr = urlStr.substr(0, queryStart);
    }

    // RFC1808, 2.4.5
    const auto paramsStart = urlStr.find(';');
    if (paramsStart != npos) {
        url.params = urlStr.substr(paramsStart + 1);
        urlStr = urlStr.substr(0, paramsStart);
    }

    // If the URI is absoluteURI, we jumped to the slash, otherwise it has to be
    // abs_path, which must start with a slash. (RFC1808, 2.2)
    if (urlStr.empty() || urlStr[0] != '/') {
        return std::nullopt;
    }
    url.path = removeDotSegments(urlStr);

    return url;
}

std::optional<Request> Request::parse(std::string_view requestStr)
{
    // e.g.: GET /foobar/barbar HTTP/1.1\r\nHost: example.org\r\n\r\n
    Request req;

    const auto requestLineEnd = requestStr.find("\r\n");
    if (requestLineEnd == std::string::npos) {
        return std::nullopt;
    }
    const auto requestLine = requestStr.substr(0, requestLineEnd);

    const auto methodDelim = requestLine.find(' ');
    if (methodDelim == std::string::npos) {
        return std::nullopt;
    }
    const auto methodStr = requestLine.substr(0, methodDelim);
    // We'll allow OPTIONS in HTTP/1.0 too
    const auto method = parseMethod(methodStr);
    if (!method) {
        return std::nullopt;
    }
    req.method = *method;

    // I could skip all whitespace here to be more robust, but RFC2616 5.1 only mentions 1 SP
    const auto urlStart = methodDelim + 1;
    if (urlStart >= requestLine.size()) {
        return std::nullopt;
    }
    // SHOULD actually return "414 Request-URI Too Long" here (RFC2616 3.2.1)
    const auto urlLen = requestLine.substr(urlStart, Config::get().maxUrlLength).find(' ');
    if (urlLen == std::string::npos) {
        return std::nullopt;
    }
    const auto url = Url::parse(requestLine.substr(urlStart, urlLen));
    if (!url) {
        return std::nullopt;
    }
    req.url = url.value();

    const auto versionStart = urlStart + urlLen + 1;
    if (versionStart > requestLine.size()) {
        return std::nullopt;
    }
    req.version = requestLine.substr(versionStart);

    if (req.version.size() != 8 || req.version.substr(0, 7) != "HTTP/1."
        || (req.version[7] != '0' && req.version[7] != '1')) {
        return std::nullopt;
    }

    size_t lineStart = requestLineEnd + 2;
    lineStart += 2;

    while (lineStart < requestStr.size()) {
        const auto lineEnd = requestStr.find("\r\n", lineStart);
        if (lineEnd == std::string_view::npos) {
            return std::nullopt;
        }
        if (lineStart == lineEnd) {
            // skip newlines and end header parsing
            lineStart += 2;
            break;
        } else {
            const auto line = requestStr.substr(lineStart, lineEnd - lineStart);
            auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                return std::nullopt;
            }
            const auto name = line.substr(0, colon);
            auto valueStart = colon + 1;
            while (valueStart < line.size() && isHttpWhitespace(line[valueStart])) {
                valueStart++;
            }
            auto valueEnd = valueStart;
            while (valueEnd < line.size() && !isHttpWhitespace(line[valueEnd])) {
                valueEnd++;
            }
            const auto value = line.substr(valueStart, valueEnd - valueStart);
            req.headers.add(name, value);
            lineStart = lineEnd + 2;
        }
    }

    return req;
}

Response::Response()
{
    headers.add("Connection", "close");
}

Response::Response(std::string body)
    : body(std::move(body))
{
    headers.add("Connection", "close");
    headers.add("Content-Type", "text/plain");
}

Response::Response(std::string body, std::string_view contentType)
    : body(std::move(body))
{
    headers.add("Connection", "close");
    headers.add("Content-Type", contentType);
}

Response::Response(StatusCode code, std::string body)
    : code(code)
    , body(std::move(body))
{
    headers.add("Connection", "close");
    headers.add("Content-Type", "text/plain");
}

Response::Response(StatusCode code, std::string body, std::string_view contentType)
    : code(code)
    , body(std::move(body))
{
    headers.add("Connection", "close");
    headers.add("Content-Type", contentType);
}

std::string Response::string() const
{
    std::string s;
    auto size = 12 + 2; // status line
    const auto headerEntries = headers.getEntries();
    for (const auto& [name, value] : headerEntries) {
        size += name.size() + value.size() + 4;
    }
    size += 2;
    size += body.size();
    s.append("HTTP/1.1 ");
    s.append(std::to_string(static_cast<int>(code)));
    s.append("\r\n");
    for (const auto& [name, value] : headerEntries) {
        s.append(name);
        s.append(": ");
        s.append(value);
        s.append("\r\n");
    }
    s.append("\r\n");
    s.append(body);
    return s;
}

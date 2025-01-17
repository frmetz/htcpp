#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "fd.hpp"
#include "http.hpp"
#include "ioqueue.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

Fd createTcpListenSocket(uint16_t listenPort, uint32_t listenAddr, int backlog);

struct Responder {
    virtual ~Responder() = default;
    virtual void respond(Response&& response) = 0;
};

// I really don't like this interface, but I feel like I have no choice. The "Responder"
// has to have some kind of type erasure (hence the virtual function), because eventually we need to
// call into a TCP and a SSL Session, which are different types (different template instantiations).
// And because we use a virtual function to respond, we need to use some kind of reference-type, but
// it needs to be copyable, to please std::function (I'm getting really tired of that requirement).
// I feel like I have no choice but to do it this way.
using RequestHandler = std::function<void(const Request&, std::shared_ptr<Responder>)>;

// Maybe I should put the function definitions into server.cpp and instantiate the template
// explicitly for TcpConnection and SslConnection, but I would have include ssl.hpp here, which I do
// not like right now.
template <typename ConnectionFactory>
class Server {
public:
    using Connection = typename ConnectionFactory::Connection;

    Server(IoQueue& io, ConnectionFactory factory, RequestHandler handler,
        Config::Server config = Config::Server {})
        : io_(io)
        , listenSocket_(
              createTcpListenSocket(config.listenPort, config.listenAddress, config.listenBacklog))
        , handler_(std::move(handler))
        , connectionFactory_(std::move(factory))
        , config_(std::move(config))
    {
        if (listenSocket_ == -1) {
            slog::fatal("Could not create listen socket: ", errnoToString(errno));
            std::exit(1);
        }
    };

    void start()
    {
        slog::info("Listening on ", ::inet_ntoa(::in_addr { config_.listenAddress }), ":",
            config_.listenPort);
        accept();
    }

private:
    class Session;

    struct SessionResponder : public Responder {
        // shared_ptr on the session to keep it alive as long as this lives
        std::shared_ptr<Session> session;

        SessionResponder(std::shared_ptr<Session> session)
            : session(std::move(session))
        {
        }

        void respond(Response&& response) override { session->respond(std::move(response)); }
    };

    // A Session will have ownership of itself and decide on its own when it's time to be
    // destroyed
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(std::unique_ptr<Connection> connection, RequestHandler& handler,
            std::string remoteAddr, const Config::Server& serverConfig)
            : connection_(std::move(connection))
            , handler_(handler)
            , remoteAddr_(std::move(remoteAddr))
            , trackInProgressHandle_(Metrics::get().connActive.labels().trackInProgress())
            , serverConfig_(serverConfig)
        {
            requestHeaderBuffer_.reserve(serverConfig_.maxRequestHeaderSize);
            requestBodyBuffer_.reserve(serverConfig_.maxRequestBodySize);
        }

        ~Session() = default;

        Session(const Session&) = default;
        Session(Session&&) = default;
        Session& operator=(const Session&) = default;
        Session& operator=(Session&&) = default;

        void start()
        {
            // readRequest is not part of the constructor and in this separate method,
            // because shared_from_this must not be called until the shared_ptr constructor has
            // completed. You would get a bad_weak_ptr exception in shared_from_this if you called
            // it from the Session constructor.
            requestStart_ = cpprom::now();
            readRequest();
        }

    private:
        friend class SessionResponder;

        // Inspired by this: https://github.com/expressjs/morgan#predefined-formats
        void accessLog(std::string_view requestLine, StatusCode responseStatus,
            size_t responseContentLength) const
        {
            if (serverConfig_.accesLog) {
                slog::info(remoteAddr_, " \"", requestLine, "\" ", static_cast<int>(responseStatus),
                    " ", responseContentLength);
            }
        }

        void readRequest()
        {
            requestHeaderBuffer_.clear();
            requestBodyBuffer_.clear();
            IoQueue::setAbsoluteTimeout(&recvTimeout_, serverConfig_.fullReadTimeoutMs);

            const auto recvLen = serverConfig_.maxRequestHeaderSize;
            requestHeaderBuffer_.append(recvLen, '\0');
            connection_->recv(requestHeaderBuffer_.data(), recvLen, &recvTimeout_,
                // `this->` before `shared_from_this` is necessary or you get an error
                // because of a dependent type lookup.
                [this, self = this->shared_from_this(), recvLen](
                    std::error_code ec, int readBytes) {
                    if (ec) {
                        Metrics::get().recvErrors.labels(ec.message()).inc();
                        slog::error("Error in recv (headers): ", ec.message());
                        // Error might be ECONNRESET, EPIPE (from send) or others, where we just
                        // want to close. There might be errors, where shutdown is better, but
                        // especially with SSL almost all errors here require us to NOT shutdown.
                        // Same applies for send below.
                        // A notable exception is ECANCELED caused by an expiration of the read
                        // timeout.
                        if (ec.value() == ECANCELED) {
                            connection_->shutdown([this, self = this->shared_from_this()](
                                                      std::error_code) { connection_->close(); });
                        } else {
                            connection_->close();
                        }
                        return;
                    }

                    if (readBytes == 0) {
                        connection_->close();
                        return;
                    }

                    requestHeaderBuffer_.resize(requestHeaderBuffer_.size() - recvLen + readBytes);

                    auto request = Request::parse(requestHeaderBuffer_);
                    if (!request) {
                        accessLog("INVALID REQUEST", StatusCode::BadRequest, 0);
                        Metrics::get().reqErrors.labels("parse error").inc();
                        respond("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", false);
                        return;
                    }
                    request_ = std::move(*request);

                    const auto contentLength = request_.headers.get("Content-Length");
                    if (contentLength) {
                        const auto length = parseInt<uint64_t>(*contentLength);
                        if (!length) {
                            accessLog(
                                "INVALID REQUEST (Content-Length)", StatusCode::BadRequest, 0);
                            Metrics::get().reqErrors.labels("invalid length").inc();
                            respond("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", false);
                            return;
                        }

                        if (*length > serverConfig_.maxRequestBodySize) {
                            accessLog("INVALID REQUEST (body size)", StatusCode::BadRequest, 0);
                            Metrics::get().reqErrors.labels("body too large").inc();
                            respond("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", false);
                        } else if (request_.body.size() < *length) {
                            requestBodyBuffer_.append(request_.body);
                            request_.body = std::string_view();
                            readRequestBody(*length);
                        } else {
                            request_.body = request_.body.substr(0, *length);
                            processRequest(request_);
                        }
                    } else {
                        processRequest(request_);
                    }
                });
        }

        void readRequestBody(size_t contentLength)
        {
            const auto sizeBeforeRead = requestBodyBuffer_.size();
            assert(sizeBeforeRead < contentLength);
            const auto recvLen = contentLength - sizeBeforeRead;
            requestBodyBuffer_.append(recvLen, '\0');
            auto buffer = requestBodyBuffer_.data() + sizeBeforeRead;
            connection_->recv(buffer, recvLen, &recvTimeout_,
                [this, self = this->shared_from_this(), recvLen, contentLength](
                    std::error_code ec, int readBytes) {
                    if (ec) {
                        Metrics::get().recvErrors.labels(ec.message()).inc();
                        slog::error("Error in recv (body): ", ec.message());
                        connection_->close();
                        return;
                    }

                    if (readBytes == 0) {
                        connection_->close();
                        return;
                    }

                    requestBodyBuffer_.resize(requestBodyBuffer_.size() - recvLen + readBytes);

                    if (requestBodyBuffer_.size() < contentLength) {
                        readRequestBody(contentLength);
                    } else {
                        assert(requestBodyBuffer_.size() == contentLength);
                        request_.body = std::string_view(requestBodyBuffer_);
                        processRequest(request_);
                    }
                });
        }

        bool getKeepAlive(const Request& request) const
        {
            const auto connectionHeader = request.headers.get("Connection");
            if (connectionHeader) {
                if (connectionHeader->find("close") != std::string_view::npos) {
                    return false;
                }
                // I should check case-insensitively here, but it's always lowercase in practice
                // (everywhere I tried)
                if (connectionHeader->find("keep-alive") != std::string_view::npos) {
                    return true;
                }
            }
            if (request.version == "HTTP/1.1") {
                return true;
            }
            return false;
        }

        void processRequest(const Request& request)
        {
            Metrics::get()
                .reqHeaderSize.labels(toString(request.method), request.url.path)
                .observe(requestHeaderBuffer_.size());
            Metrics::get()
                .reqBodySize.labels(toString(request.method), request.url.path)
                .observe(requestBodyBuffer_.size());
            handler_(request, std::make_shared<SessionResponder>(this->shared_from_this()));
        }

        void respond(Response&& response)
        {
            response_ = std::move(response);
            const auto status = std::to_string(static_cast<int>(response_.status));
            Metrics::get()
                .reqsTotal.labels(toString(request_.method), request_.url.path, status)
                .inc();
            accessLog(request_.requestLine, response_.status, response_.body.size());
            respond(response_.string(request_.version), getKeepAlive(request_));
        }

        void respond(std::string response, bool keepAlive)
        {
            // We need to keep the memory that is referenced in the SQE around, because we don't
            // know when the kernel will copy it, so we save it in this member variable, which
            // definitely lives longer than this send takes to complete.
            // I also prefer the member variable compared to moving it into the lambda, because
            // it can be reused for another request in this session.
            responseBuffer_ = std::move(response);
            responseSendOffset_ = 0;
            keepAlive_ = keepAlive;
            sendResponse();
        }

        void sendResponse()
        {
            assert(responseSendOffset_ < responseBuffer_.size());
            connection_->send(responseBuffer_.data() + responseSendOffset_,
                responseBuffer_.size() - responseSendOffset_,
                [this, self = this->shared_from_this()](std::error_code ec, int sentBytes) {
                    if (ec) {
                        // I think there are no errors, where we want to shutdown.
                        // Note that ec could be an error that can not be returned by ::send,
                        // because with SSL it might do ::recv as part of Connection::send.
                        Metrics::get().sendErrors.labels(ec.message()).inc();
                        slog::error("Error in send: ", ec.message());
                        connection_->close();
                        return;
                    }

                    if (sentBytes == 0) {
                        // I don't know when this would happen for TCP.
                        // For SSL this will happen, when the remote peer closed the
                        // connection during a recv that's part of an SSL_write.
                        // In that case we close (since we can't shutdown).
                        connection_->close();
                        return;
                    }

                    assert(sentBytes > 0);
                    if (responseSendOffset_ + sentBytes < responseBuffer_.size()) {
                        responseSendOffset_ += sentBytes;
                        sendResponse();
                        return;
                    }

                    // Only step these counters for successful sends
                    const auto method = toString(request_.method);
                    const auto status = std::to_string(static_cast<int>(response_.status));
                    Metrics::get()
                        .reqDuration.labels(method, request_.url.path)
                        .observe(cpprom::now() - requestStart_);
                    Metrics::get().respTotal.labels(method, request_.url.path, status).inc();
                    Metrics::get()
                        .respSize.labels(method, request_.url.path, status)
                        .observe(responseBuffer_.size());

                    if (keepAlive_) {
                        start();
                    } else {
                        shutdown();
                    }
                });
        }

        // If this only supported TCP, then using close everywhere would be fine.
        // The difference is most important for TLS, where shutdown will call SSL_shutdown.
        void shutdown()
        {
            connection_->shutdown([this, self = this->shared_from_this()](std::error_code) {
                // There is no way to recover, so ignore the error and close either way.
                connection_->close();
            });
        }

        std::unique_ptr<Connection> connection_;
        RequestHandler& handler_;
        std::string remoteAddr_;
        // The Request object is the result of request header parsing and consists of many
        // string_views referencing the buffer that the request was parsed from. If that buffer
        // would have to be resized (because of a large body not yet fully received), these
        // references would be invalidated. Hence the body is saved in a separate buffer.
        std::string requestHeaderBuffer_;
        std::string requestBodyBuffer_;
        std::string responseBuffer_;
        Request request_;
        Response response_;
        IoQueue::Timespec recvTimeout_;
        cpprom::Gauge::TrackInProgressHandle trackInProgressHandle_;
        double requestStart_;
        const Config::Server& serverConfig_;
        size_t responseSendOffset_ = 0;
        bool keepAlive_;
    };

    void accept()
    {
        // In the past there was a bug, where too many concurrent requests would fill up the SQR
        // with reads and writes so that it would run full and you could not add an accept SQE.
        // Essentially too high concurrency would push out the accept task and the server would stop
        // accepting connections.
        // For some reason I cannot reproduce it anymore. Maybe *something* has changed with a newer
        // kernel, but I can't imagine what that would be.
        // I will fix it anyway, because it should be dead code, if everything goes right anyways.
        // So essentially we *force* an accept SQE into the SQR by retrying again and again.
        // This is a busy loop, because we *really* want to get that accept into the SQR and we
        // don't have to worry about priority inversion (I think) because it's the kernel that's
        // consuming the currently present items.
        bool added = false;
        acceptAddrLen_ = sizeof(acceptAddr_);
        while (!added) {
            added = io_.accept(listenSocket_, &acceptAddr_, &acceptAddrLen_,
                [this](std::error_code ec, int fd) { handleAccept(ec, fd); });
        }
    }

    void handleAccept(std::error_code ec, int fd)
    {
        if (ec) {
            slog::error("Error in accept: ", ec.message());
            Metrics::get().acceptErrors.labels(ec.message()).inc();
        } else {
            static auto& connAccepted = Metrics::get().connAccepted.labels();
            connAccepted.inc();
            const auto addr = ::inet_ntoa(acceptAddr_.sin_addr);
            auto conn = connectionFactory_.create(io_, fd);
            if (conn) {
                std::make_shared<Session>(std::move(conn), handler_, addr, config_)->start();
            } else {
                slog::info("Could not create connection object (connection factory not ready)");
                io_.close(fd, [](std::error_code) {});
            }
        }

        accept();
    }

    IoQueue& io_;
    Fd listenSocket_;
    RequestHandler handler_;
    ::sockaddr_in acceptAddr_;
    ::socklen_t acceptAddrLen_;
    ConnectionFactory connectionFactory_;
    Config::Server config_;
};

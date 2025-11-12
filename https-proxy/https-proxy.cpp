//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: HTTP flex server (plain and SSL), asynchronous
//
//------------------------------------------------------------------------------

#include "cert_store.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/version.hpp>
#include <boost/config.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <openssl/ssl.h>

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

enum class yield_type : uint64_t {
    INVALID = 0,
    REQUEST,
    POLL_RESPONSE,
    POLL_RESPONSE_BODY,
};

struct yield_mmio_req final {
    uint64_t headers_count{0};
    uint64_t body_vaddr{0};
    uint64_t body_length{0};
    char url[4096]{};
    char method[32]{};
    char headers[64][2][256]{};
};

struct yield_mmio_res final {
    uint64_t ready_state{0};
    uint64_t status{0};
    uint64_t body_total_length{0};
    uint64_t headers_count{0};
    char headers[64][2][256]{};
};

extern "C" __attribute__((noinline, naked)) uint64_t softyield(uint64_t /*a0*/, uint64_t /*a1*/, uint64_t /*a2*/) {
    // NOLINTNEXTLINE(hicpp-no-assembler)
    asm volatile("sraiw x0, x31, 0\n\tret");
}

extern "C" __attribute__((noinline)) uint64_t rdcycle() {
    // NOLINTNEXTLINE(misc-const-correctness)
    uint64_t cycle{0};
    // NOLINTNEXTLINE(hicpp-no-assembler)
    asm volatile("rdcycle %0" : "=r"(cycle));
    return cycle;
}

template <size_t N>
static void strsvcopy(char (&dest)[N], std::string_view sv) {
    memcpy(dest, sv.data(), std::min(sv.length(), N));
    dest[std::min(sv.length(), N - 1)] = 0;
}

template <class Body, class Allocator>
static void fill_mmio_req(yield_mmio_req &mmio_req, const http::request<Body, http::basic_fields<Allocator>> &req) {
    const std::string_view host = req["Host"];
    const std::string_view method = req.method_string();
    const std::string url = std::string("https://").append(host).append(req.target());
    strsvcopy(mmio_req.url, url);
    strsvcopy(mmio_req.method, method);
    mmio_req.headers_count = 0;
    for (auto &field : req) {
        if (field.name() != http::field::user_agent && field.name() != http::field::host &&
            field.name() != http::field::content_length) {
            strsvcopy(mmio_req.headers[mmio_req.headers_count][0], field.name_string());
            strsvcopy(mmio_req.headers[mmio_req.headers_count][1], field.value());
            mmio_req.headers_count++;
            if (mmio_req.headers_count >= 64) {
                break;
            }
        }
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    mmio_req.body_vaddr = reinterpret_cast<uintptr_t>(req.body().c_str());
    mmio_req.body_length = req.body().length();
}

// Return a response for the given request.
//
// The concrete type of the response message (which depends on the
// request), is type-erased in message_generator.
template <class Body, class Allocator>
static http::message_generator handle_request(http::request<Body, http::basic_fields<Allocator>> &&req) {
    auto const bad_request = [&req](beast::string_view why) {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/plain");
        res.keep_alive(false);
        res.body() = std::string(why);
        res.prepare_payload();
        return res;
    };

    const uint64_t uid = rdcycle();
    yield_mmio_req mmio_req;
    fill_mmio_req(mmio_req, req);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (softyield(static_cast<uint64_t>(yield_type::REQUEST), uid, reinterpret_cast<uintptr_t>(&mmio_req)) != 0) {
        return bad_request("Request yield failed");
    }

    yield_mmio_res mmio_res;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (softyield(static_cast<uint64_t>(yield_type::POLL_RESPONSE), uid, reinterpret_cast<uintptr_t>(&mmio_res)) != 0) {
        return bad_request("Poll response headers yield failed");
    }

    std::string body;
    if (mmio_res.body_total_length > 0) {
        body.resize(mmio_res.body_total_length, '\x0');
        if (softyield(static_cast<uint64_t>(yield_type::POLL_RESPONSE_BODY),
                uid, // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                reinterpret_cast<uintptr_t>(body.data())) != 0) {
            return bad_request("Poll response body yield failed");
        }
    } else if (mmio_res.status == 0) {
        return bad_request("Fetch failed, either due to CORS policy violation or network error.");
    }

    // Respond request
    http::response<http::string_body> res{http::int_to_status(mmio_res.status), req.version()};
    for (uint64_t i = 0; i < mmio_res.headers_count; ++i) {
        res.set(mmio_res.headers[i][0], mmio_res.headers[i][1]);
    }
    res.keep_alive(false);
    res.body() = std::move(body);
    res.prepare_payload();

    std::ignore = std::move(req);
    return res;
}

//------------------------------------------------------------------------------

// Report a failure
static void fail(beast::error_code ec, char const *what) {
    // ssl::error::stream_truncated, also known as an SSL "short read",
    // indicates the peer closed the connection without performing the
    // required closing handshake (for example, Google does this to
    // improve performance). Generally this can be a security issue,
    // but if your communication protocol is self-terminated (as
    // it is with both HTTP and WebSocket) then you may simply
    // ignore the lack of close_notify.
    //
    // https://github.com/boostorg/beast/issues/38
    //
    // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
    //
    // When a short read would cut off the end of an HTTP message,
    // Beast returns the error beast::http::error::partial_message.
    // Therefore, if we see a short read here, it has occurred
    // after the message has been completed, so it is safe to ignore it.

    if (ec == net::ssl::error::stream_truncated) {
        return;
    }

    std::cerr << what << ": " << ec.message() << "\n";
}

// Handles an HTTP server connection.
// This uses the Curiously Recurring Template Pattern so that
// the same code works with both SSL streams and regular sockets.
template <class Derived>
class session {
    // Access the derived class, this is part of
    // the Curiously Recurring Template Pattern idiom.
    Derived &derived() {
        return static_cast<Derived &>(*this);
    }

    http::request<http::string_body> req_;

protected:
    beast::flat_buffer buffer_; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)

public:
    // Take ownership of the buffer
    explicit session(beast::flat_buffer buffer) : // NOLINT(bugprone-crtp-constructor-accessibility)
        buffer_(std::move(buffer)) {}

    void do_read() {
        // Set the timeout.
        // beast::get_lowest_layer(derived().stream()).expires_after(std::chrono::seconds(30));

        // Read a request
        http::async_read(derived().stream(), buffer_, req_,
            beast::bind_front_handler(&session::on_read, derived().shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        // This means they closed the connection
        if (ec == http::error::end_of_stream) {
            derived().do_eof();
            return;
        }

        if (ec) {
            fail(ec, "read");
            return;
        }

        // Send the response
        send_response(handle_request(std::move(req_)));
    }

    void send_response(http::message_generator &&msg) {
        const bool keep_alive = msg.keep_alive();

        // Write the response
        beast::async_write(derived().stream(), std::move(msg),
            beast::bind_front_handler(&session::on_write, derived().shared_from_this(), keep_alive));
    }

    void on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec) {
            fail(ec, "write");
            return;
        }

        if (!keep_alive) {
            // This means we should close the connection, usually because
            // the response indicated the "Connection: close" semantic.
            derived().do_eof();
            return;
        }

        // Read another request
        do_read();
    }
};

// Handles a plain HTTP connection
class plain_session : public session<plain_session>, public std::enable_shared_from_this<plain_session> {
    beast::tcp_stream stream_;

public:
    // Create the session
    plain_session(tcp::socket &&socket, beast::flat_buffer buffer) :
        session<plain_session>(std::move(buffer)),
        stream_(std::move(socket)) {}

    // Called by the base class
    beast::tcp_stream &stream() {
        return stream_;
    }

    // Start the asynchronous operation
    void run() {
        // We need to be executing within a strand to perform async operations
        // on the I/O objects in this session. Although not strictly necessary
        // for single-threaded contexts, this example code is written to be
        // thread-safe by default.
        net::dispatch(stream_.get_executor(), beast::bind_front_handler(&session::do_read, shared_from_this()));
    }

    void do_eof() {
        // Send a TCP shutdown
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec); // NOLINT(bugprone-unused-return-value,cert-err33-c)

        // At this point the connection is closed gracefully
    }
};

// Handles an SSL HTTP connection
class ssl_session : public session<ssl_session>, public std::enable_shared_from_this<ssl_session> {
    ssl::stream<beast::tcp_stream> stream_;

public:
    // Create the session
    ssl_session(tcp::socket &&socket, ssl::context &ctx, beast::flat_buffer buffer) :
        session<ssl_session>(std::move(buffer)),
        stream_(std::move(socket), ctx) {}

    // Called by the base class
    ssl::stream<beast::tcp_stream> &stream() {
        return stream_;
    }

    // Start the asynchronous operation
    void run() {
        auto self = shared_from_this();
        // We need to be executing within a strand to perform async operations
        // on the I/O objects in this session.
        net::dispatch(stream_.get_executor(), [self]() {
            // Set the timeout.
            // beast::get_lowest_layer(self->stream_).expires_after(std::chrono::seconds(30));

            // Perform the SSL handshake
            // Note, this is the buffered version of the handshake.
            self->stream_.async_handshake(ssl::stream_base::server, self->buffer_.data(),
                beast::bind_front_handler(&ssl_session::on_handshake, self));
        });
    }

    void on_handshake(beast::error_code ec, std::size_t bytes_used) {
        if (ec) {
            fail(ec, "handshake");
            return;
        }

        // Consume the portion of the buffer used by the handshake
        buffer_.consume(bytes_used);

        // Certificate injection happens in SNI callback before handshake completes
        do_read();
    }

    void do_eof() {
        // Set the timeout.
        // beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        // Perform the SSL shutdown
        stream_.async_shutdown(beast::bind_front_handler(&ssl_session::on_shutdown, shared_from_this()));
    }

    void on_shutdown(beast::error_code ec) { // NOLINT(readability-convert-member-functions-to-static)
        if (ec) {
            fail(ec, "shutdown");
            return;
        }

        // At this point the connection is closed gracefully
    }
};

//------------------------------------------------------------------------------

// SNI callback to capture hostname and inject certificate
static int sni_callback(SSL* ssl, int* /*ad*/, void* /*arg*/) {
    const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (servername && servername[0] != '\0') {
        std::string hostname(servername);
        auto cert_pair = cert_store::instance().issue_for_host(hostname);
        if (cert_pair.cert && cert_pair.key) {
            if (SSL_use_certificate(ssl, cert_pair.cert.get()) == 1 &&
                SSL_use_PrivateKey(ssl, cert_pair.key.get()) == 1) {
                // Certificate injected successfully
                return SSL_TLSEXT_ERR_OK;
            }
        }
    }
    return SSL_TLSEXT_ERR_OK;
}

// Detects SSL handshakes
class detect_session : public std::enable_shared_from_this<detect_session> {
    beast::tcp_stream stream_;
    ssl::context &ctx_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    beast::flat_buffer buffer_;

public:
    detect_session(tcp::socket &&socket, ssl::context &ctx) : stream_(std::move(socket)), ctx_(ctx) {}

    // Launch the detector
    void run() {
        // Set the timeout.
        // beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

        // Detect a TLS handshake
        async_detect_ssl(stream_, buffer_, beast::bind_front_handler(&detect_session::on_detect, shared_from_this()));
    }

    void on_detect(beast::error_code ec, bool result) {
        if (ec) {
            fail(ec, "detect");
            return;
        }

        if (result) {
            // Launch SSL session
            std::make_shared<ssl_session>(stream_.release_socket(), ctx_, std::move(buffer_))->run();
            return;
        }

        // Launch plain session
        std::make_shared<plain_session>(stream_.release_socket(), std::move(buffer_))->run();
    }
};

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener> {
    net::io_context &ioc_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    ssl::context &ctx_;    // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    tcp::acceptor acceptor_;

public:
    listener(net::io_context &ioc, ssl::context &ctx, const tcp::endpoint &endpoint) :
        ioc_(ioc),
        ctx_(ctx),
        acceptor_(net::make_strand(ioc)) {
        // NOLINTBEGIN(bugprone-unused-return-value,cert-err33-c)
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            fail(ec, "open");
            return;
        }

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            fail(ec, "set_option");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec) {
            fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            fail(ec, "listen");
            return;
        }
        // NOLINTEND(bugprone-unused-return-value,cert-err33-c)
    }

    // Start accepting incoming connections
    void run() {
        do_accept();
    }

private:
    void do_accept() {
        // The new connection gets its own strand
        acceptor_.async_accept(net::make_strand(ioc_),
            beast::bind_front_handler(&listener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (ec) {
            fail(ec, "accept");
        } else {
            // Create the detector session and run it
            std::make_shared<detect_session>(std::move(socket), ctx_)->run();
        }

        // Accept another connection
        do_accept();
    }
};

//------------------------------------------------------------------------------

int main(int argc, char *argv[]) try {
    // Check command line arguments.
    if (argc != 4) {
        std::cerr << "Usage: https-proxy <address> <port1> <port2>\n"
                  << "Example:\n"
                  << "    https-proxy 127.0.0.1 80 443\n";
        return EXIT_FAILURE;
    }
    auto const address = net::ip::make_address(argv[1]);
    auto const port1 = static_cast<uint16_t>(std::strtol(argv[2], nullptr, 10));
    auto const port2 = static_cast<uint16_t>(std::strtol(argv[3], nullptr, 10));
    auto const threads = 1;

    // The io_context is required for all I/O
    net::io_context ioc{threads};

    // The SSL context is required, and holds certificates
    ssl::context ctx{ssl::context::tlsv12};

    // Initialize certificate store
    if (!cert_store::instance().ensure_ca()) {
        std::cerr << "Failed to initialize certificate store\n";
        return EXIT_FAILURE;
    }

    // Set up SNI callback for dynamic certificate injection
    SSL_CTX_set_tlsext_servername_callback(ctx.native_handle(), sni_callback);
    SSL_CTX_set_tlsext_servername_arg(ctx.native_handle(), nullptr);

    // Set basic SSL context options
    ctx.set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::single_dh_use);

    // Create and launch a listening port
    std::make_shared<listener>(ioc, ctx, tcp::endpoint{address, port1})->run();
    std::make_shared<listener>(ioc, ctx, tcp::endpoint{address, port2})->run();

    // Run the I/O service on the requested number of threads
    std::vector<std::thread> v;
    v.reserve(threads - 1);
    for (auto i = threads - 1; i > 0; --i) {
        v.emplace_back([&ioc] { ioc.run(); });
    }
    ioc.run();

    return EXIT_SUCCESS;
} catch (const std::exception &e) {
    std::cerr << "main() exception: " << e.what() << "\n";
}

#include "http_server.h"
#include "common/hostname.h"
#include "exporter/DaemonMetricCollector.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

class http_connection : public std::enable_shared_from_this<http_connection> {
public:
  http_connection(tcp::socket socket) : socket_(std::move(socket)) {}

  // Initiate the asynchronous operations associated with the connection.
  void start() {
    read_request();
    check_deadline();
  }

private:
  tcp::socket socket_;
  beast::flat_buffer buffer_{8192};
  http::request<http::dynamic_body> request_;
  http::response<http::string_body> response_;

  net::steady_timer deadline_{socket_.get_executor(), std::chrono::seconds(60)};

  // Asynchronously receive a complete request message.
  void read_request() {
    auto self = shared_from_this();

    http::async_read(socket_, buffer_, request_,
                     [self](beast::error_code ec, std::size_t bytes_transferred) {
                       boost::ignore_unused(bytes_transferred);
                       if (!ec)
                         self->process_request();
                     });
  }

  // Determine what needs to be done with the request message.
  void process_request() {
    response_.version(request_.version());
    response_.keep_alive(request_.keep_alive());

    switch (request_.method()) {
    case http::verb::get:
      response_.result(http::status::ok);
      create_response();
      break;

    default:
      // We return responses indicating an error if
      // we do not recognize the request method.
      response_.result(http::status::method_not_allowed);
      response_.set(http::field::content_type, "text/plain");
      std::string body("Invalid request-method '" +
                       std::string(request_.method_string()) + "'");
      response_.body() = body;
      break;
    }

    write_response();
  }

  // Construct a response message based on the program state.
  void create_response() {
    if (request_.target() == "/") {
      response_.set(http::field::content_type, "text/html; charset=utf-8");
      std::string body("<html>\n"
                       "<head><title>Ceph Exporter</title></head>\n"
                       "<body>\n"
                       "<h1>Ceph Exporter</h1>\n"
                       "<p><a href='/metrics'>Metrics</a></p>"
                       "</body>\n"
                       "</html>\n");
      response_.body() = body;
    } else if (request_.target() == "/metrics") {
      response_.set(http::field::content_type, "text/plain; charset=utf-8");
      DaemonMetricCollector &collector = collector_instance();
      std::string metrics = collector.get_metrics();
      response_.body() = metrics;
    } else {
      response_.result(http::status::method_not_allowed);
      response_.set(http::field::content_type, "text/plain");
      response_.body() = "File not found \n";
    }
  }

  // Asynchronously transmit the response message.
  void write_response() {
    auto self = shared_from_this();

    response_.prepare_payload();

    http::async_write(socket_, response_,
                      [self](beast::error_code ec, std::size_t) {
                        self->socket_.shutdown(tcp::socket::shutdown_send, ec);
                        self->deadline_.cancel();
                      });
  }

  // Check whether we have spent enough time on this connection.
  void check_deadline() {
    auto self = shared_from_this();

    deadline_.async_wait([self](beast::error_code ec) {
      if (!ec) {
        // Close socket to cancel any outstanding operation.
        self->socket_.close(ec);
      }
    });
  }
};

// "Loop" forever accepting new connections.
void http_server(tcp::acceptor &acceptor, tcp::socket &socket) {
  acceptor.async_accept(socket, [&](beast::error_code ec) {
    if (!ec)
      std::make_shared<http_connection>(std::move(socket))->start();
    http_server(acceptor, socket);
  });
}

std::string dns_lookup(std::string hostname) {
  boost::asio::io_service io_service;
  boost::asio::ip::tcp::resolver resolver(io_service);
  boost::asio::ip::tcp::resolver::query query(hostname, "9926");
  boost::asio::ip::tcp::resolver::iterator iter = resolver.resolve(query);
  boost::asio::ip::tcp::endpoint endpoint = iter->endpoint();
  std::string ip_address = endpoint.address().to_string();

  return ip_address;
}

void http_server_thread_entrypoint(std::string cert_path, std::string key_path,
                                   std::string tls_options) {
  try {
    std::string hostname = ceph_get_short_hostname();

    std::string ip_address = dns_lookup(hostname);
    auto const address = net::ip::make_address(ip_address);
    unsigned short port = 9926;

    net::io_context ioc{1};

    tcp::acceptor acceptor{ioc, {address, port}};
    tcp::socket socket{ioc};
    http_server(acceptor, socket);
    std::cout << "Http server started" << std::endl;
    ioc.run();
  } catch (std::exception const &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    exit(EXIT_FAILURE);
  }
}

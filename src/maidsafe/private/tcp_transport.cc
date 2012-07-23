/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
* Neither the name of the maidsafe.net limited nor the names of its
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <functional>

#include "maidsafe/private/tcp_transport.h"

#include "maidsafe/common/log.h"

#include "maidsafe/private/message_handler.h"
#include "maidsafe/private/transport_pb.h"
#include "maidsafe/private/tcp_connection.h"
#include "maidsafe/private/utils.h"

namespace asio = boost::asio;
namespace bs = boost::system;
namespace ip = asio::ip;
namespace args = std::placeholders;

namespace maidsafe {

namespace priv {

TcpTransport::TcpTransport(boost::asio::io_service &asio_service) // NOLINT
    : Transport(asio_service),
      acceptor_(),
      connections_(),
      strand_(asio_service) {}

TcpTransport::~TcpTransport() {
  for (auto it = connections_.begin(); it != connections_.end();)
    (*it++)->Close();
}

TransportCondition TcpTransport::StartListening(const Endpoint &endpoint) {
  LOG(kError) << "StartListening (port "
                << listening_port_ << ").";
  if (listening_port_ != 0) {
    LOG(kError) << "StartListening - Already listening (port "
                << listening_port_ << ").";
    return kAlreadyStarted;
  }

  if (endpoint.port == 0) {
    LOG(kError) << "StartListening - Can't listen on port 0.";
    return kInvalidPort;
  }

  ip::tcp::endpoint ep(endpoint.ip, endpoint.port);
  acceptor_.reset(new ip::tcp::acceptor(asio_service_));

  bs::error_code ec;
  acceptor_->open(ep.protocol(), ec);

  if (ec) {
    LOG(kError) << "StartListening - Could not open the socket: "
                << ec.message();
    return kInvalidAddress;
  }
// Below option is interprated differently by Windows and shouldn't be used. On,
// Windows, this will allow two processes to listen on same port. On POSIX
// compliant OS, this option tells the kernel that even if given port is busy
// (only TIME_WAIT state), go ahead and reuse it anyway. If it is busy, but with
// another state, you will still get an address already in use error.
// http://msdn.microsoft.com/en-us/library/ms740621(VS.85).aspx
// http://www.unixguide.net/network/socketfaq/4.5.shtml
// http://old.nabble.com/Port-allocation-problem-on-windows-(incl.-patch)-td28241079.html
#ifndef WIN32
  acceptor_->set_option(ip::tcp::acceptor::reuse_address(true), ec);
#endif

  if (ec) {
    LOG(kError) << "StartListening - Could not set the reuse address option: "
                << ec.message();
    return kSetOptionFailure;
  }

  acceptor_->bind(ep, ec);

  if (ec) {
    LOG(kError) << "StartListening - Could not bind socket to endpoint: "
                << ec.message();
    return kBindError;
  }

  acceptor_->listen(asio::socket_base::max_connections, ec);

  if (ec) {
    LOG(kError) << "StartListening - Could not start listening: "
                << ec.message();
    return kListenError;
  }

  ConnectionPtr new_connection(
      new TcpConnection(shared_from_this(),
                        boost::asio::ip::tcp::endpoint()));
  listening_port_ = acceptor_->local_endpoint().port();
  transport_details_.endpoint.port = listening_port_;
  transport_details_.endpoint.ip = endpoint.ip;

  // The connection object is kept alive in the acceptor handler until
  // HandleAccept() is called.
  acceptor_->async_accept(new_connection->Socket(),
                          strand_.wrap(std::bind(&TcpTransport::HandleAccept,
                                                 shared_from_this(), acceptor_,
                                                 new_connection, args::_1)));
  return kSuccess;
}

TransportCondition TcpTransport::Bootstrap(
    const std::vector<Contact> &/*candidates*/) {
  return kSuccess;
}

void TcpTransport::StopListening() {
  if (acceptor_)
    strand_.dispatch(std::bind(&TcpTransport::CloseAcceptor, acceptor_));
  listening_port_ = 0;
  acceptor_.reset();
}

void TcpTransport::CloseAcceptor(AcceptorPtr acceptor) {
  boost::system::error_code ec;
  acceptor->close(ec);
}

void TcpTransport::HandleAccept(AcceptorPtr acceptor,
                                ConnectionPtr connection,
                                const bs::error_code &ec) {
  std::cout << "TRANSPORT: MESSAGE RECEIVED" << std::endl;
  if (!acceptor->is_open())
    return;

  if (!ec) {
    // It is safe to call DoInsertConnection directly because HandleAccept() is
    // already being called inside the strand.
    DoInsertConnection(connection);
    connection->StartReceiving();
  }

  ConnectionPtr new_connection(
      new TcpConnection(shared_from_this(),
                        boost::asio::ip::tcp::endpoint()));

  // The connection object is kept alive in the acceptor handler until
  // HandleAccept() is called.
  acceptor->async_accept(new_connection->Socket(),
                         strand_.wrap(std::bind(&TcpTransport::HandleAccept,
                                                shared_from_this(), acceptor,
                                                new_connection, args::_1)));
}

void TcpTransport::Send(const std::string &data,
                        const Endpoint &endpoint,
                        const Timeout &timeout) {
  DataSize msg_size(static_cast<DataSize>(data.size()));
  if (msg_size > kMaxTransportMessageSize()) {
    LOG(kError) << "Send - Data size " << msg_size
                << " bytes (exceeds limit of " << kMaxTransportMessageSize()
                << ")";
    Endpoint ep;
    (*on_error_)(kMessageSizeTooLarge, ep);
    return;
  }

  ip::tcp::endpoint tcp_endpoint(endpoint.ip, endpoint.port);
  ConnectionPtr connection(new TcpConnection(shared_from_this(),
                                             tcp_endpoint));
  InsertConnection(connection);
  connection->StartSending(data, timeout);
}

void TcpTransport::InsertConnection(ConnectionPtr connection) {
  strand_.dispatch(std::bind(&TcpTransport::DoInsertConnection,
                             shared_from_this(), connection));
}

void TcpTransport::DoInsertConnection(ConnectionPtr connection) {
  connections_.insert(connection);
}

void TcpTransport::RemoveConnection(ConnectionPtr connection) {
  strand_.dispatch(std::bind(&TcpTransport::DoRemoveConnection,
                             shared_from_this(), connection));
}

void TcpTransport::DoRemoveConnection(ConnectionPtr connection) {
  connections_.erase(connection);
}

}  // namespace priv

}  // namespace maidsafe

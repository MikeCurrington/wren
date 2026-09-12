#include "dap_io.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace wren
{
namespace debug
{
  DapConnection::~DapConnection()
  {
    stop();
  }

  bool DapConnection::listen(int port)
  {
#ifdef _WIN32
    if (!wsaInitialized_)
    {
      WSADATA wsaData;
      if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
      wsaInitialized_ = true;
    }
#endif

    listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ == kInvalidSocket) return false;

    int reuse = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR,
               &reuse, sizeof(reuse));

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0 ||
        ::listen(listenSocket_, 1) < 0)
    {
      closeSocket(listenSocket_);
      listenSocket_ = kInvalidSocket;
      return false;
    }

    return true;
  }

  void DapConnection::start(MessageCallback onMessage)
  {
    onMessage_ = std::move(onMessage);
    acceptThread_ = std::thread([this] { acceptLoop(); });
  }

  void DapConnection::acceptLoop()
  {
    for (;;)
    {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) return;
      }

      SocketFd socket = ::accept(listenSocket_, nullptr, nullptr);
      if (socket == kInvalidSocket)
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) return;
        continue;
      }

      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_)
        {
          lock.unlock();
          closeSocket(socket);
          return;
        }
        // If a client is already debugging, drop the new connection.
        if (clientSocket_ != kInvalidSocket)
        {
          lock.unlock();
          closeSocket(socket);
          continue;
        }
        clientSocket_ = socket;
      }

      if (receiveThread_.joinable()) receiveThread_.join();
      receiveThread_ = std::thread([this, socket] { receiveLoop(socket); });
    }
  }

  void DapConnection::receiveLoop(SocketFd socket)
  {
    // Bytes read from the socket that haven't been consumed by a message
    // yet. DAP frames arrive as "Content-Length: N\r\n" headers, a blank
    // line, then exactly N bytes of JSON.
    std::vector<char> buffer;
    char chunk[4096];

    for (;;)
    {
      SocketResult read = ::recv(socket, chunk,
                                 static_cast<int>(sizeof(chunk)), 0);
      if (read <= 0) break;
      buffer.insert(buffer.end(), chunk, chunk + read);

      for (;;)
      {
        const char* data = buffer.data();
        size_t size = buffer.size();

        const char* headerEnd = nullptr;
        size_t contentLength = 0;
        {
          // Find the blank line terminating the header block, parsing
          // Content-Length as we go.
          const char* cursor = data;
          const char* end = data + size;
          contentLength = 0;
          bool valid = false;
          while (cursor < end)
          {
            const char* lineEnd = static_cast<const char*>(
                memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
            if (lineEnd == nullptr) break;
            size_t lineLength = static_cast<size_t>(lineEnd - cursor);
            if (lineLength > 0 && cursor[lineLength - 1] == '\r') lineLength--;

            if (lineLength == 0)
            {
              headerEnd = lineEnd + 1;
              valid = true;
              break;
            }

            const char* prefix = "Content-Length:";
            size_t prefixLength = strlen(prefix);
            if (lineLength > prefixLength &&
                memcmp(cursor, prefix, prefixLength) == 0)
            {
              std::string value(cursor + prefixLength,
                                lineLength - prefixLength);
              contentLength = static_cast<size_t>(strtoull(value.c_str(),
                                                           nullptr, 10));
            }
            cursor = lineEnd + 1;
          }
          if (!valid || headerEnd == nullptr) break;
          if (contentLength == 0) break;
        }

        size_t headerSize = static_cast<size_t>(headerEnd - data);
        if (size - headerSize < contentLength) break;

        std::string payload(headerEnd, contentLength);
        buffer.erase(buffer.begin(),
                     buffer.begin() + static_cast<long>(headerSize + contentLength));

        Json message;
        if (Json::parse(payload, &message) && onMessage_)
        {
          onMessage_(message);
        }
      }
    }

    // The client went away. Release it so the accept loop can take the next
    // connection.
    std::unique_lock<std::mutex> lock(mutex_);
    if (clientSocket_ == socket) clientSocket_ = kInvalidSocket;
    closeSocket(socket);
  }

  bool DapConnection::sendAll(SocketFd socket, const char* data, size_t length)
  {
    size_t sent = 0;
    while (sent < length)
    {
      SocketResult result = ::send(socket, data + sent,
                                   static_cast<int>(length - sent), 0);
      if (result <= 0) return false;
      sent += static_cast<size_t>(result);
    }
    return true;
  }

  void DapConnection::send(const Json& message)
  {
    std::string payload = message.dump();
    std::string frame = "Content-Length: " +
        std::to_string(payload.size()) + "\r\n\r\n" + payload;

    std::unique_lock<std::mutex> lock(mutex_);
    if (clientSocket_ != kInvalidSocket)
    {
      // The mutex keeps concurrent sends from interleaving frames. If the
      // send fails the receive loop will notice the disconnect.
      sendAll(clientSocket_, frame.data(), frame.size());
    }
  }

  bool DapConnection::isConnected()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    return clientSocket_ != kInvalidSocket;
  }

  void DapConnection::disconnectClient()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (clientSocket_ != kInvalidSocket)
    {
      // Only shut the socket down: closing it here would race the receive
      // loop, which closes it when it exits.
      ::shutdown(clientSocket_, kShutdownBoth);
    }
  }

  void DapConnection::stop()
  {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      stopping_ = true;
      if (clientSocket_ != kInvalidSocket)
      {
        ::shutdown(clientSocket_, kShutdownBoth);
      }
    }

    if (listenSocket_ != kInvalidSocket)
    {
      ::shutdown(listenSocket_, kShutdownBoth);
      closeSocket(listenSocket_);
      listenSocket_ = kInvalidSocket;
    }

    if (acceptThread_.joinable()) acceptThread_.join();
    if (receiveThread_.joinable()) receiveThread_.join();

    std::unique_lock<std::mutex> lock(mutex_);
    if (clientSocket_ != kInvalidSocket)
    {
      closeSocket(clientSocket_);
      clientSocket_ = kInvalidSocket;
    }

#ifdef _WIN32
    if (wsaInitialized_)
    {
      WSACleanup();
      wsaInitialized_ = false;
    }
#endif
  }
}
}

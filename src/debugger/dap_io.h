#pragma once
#ifndef dap_io_h
#define dap_io_h

// The DAP wire format needs raw TCP sockets, which live in different
// headers with a slightly different API on Windows than on POSIX.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "wren_json.h"

namespace wren
{
namespace debug
{
#ifdef _WIN32
  using SocketFd = SOCKET;
  constexpr SocketFd kInvalidSocket = INVALID_SOCKET;
  using SocketResult = int;
  constexpr int kShutdownBoth = SD_BOTH;
  inline void closeSocket(SocketFd socket) { closesocket(socket); }
#else
  using SocketFd = int;
  constexpr SocketFd kInvalidSocket = -1;
  using SocketResult = ssize_t;
  constexpr int kShutdownBoth = SHUT_RDWR;
  inline void closeSocket(SocketFd socket) { ::close(socket); }
#endif

  // Accepts one debug adapter client at a time and exchanges Debug Adapter
  // Protocol messages with it over TCP, using the standard
  // "Content-Length: N\r\n\r\n" framing that DAP shares with LSP.
  //
  // A listener thread accepts connections; a receive thread reads messages
  // from the current client and delivers them to [onMessage]. Messages can be
  // sent from any thread. All of this runs independently of the VM thread:
  // the debugger session decides how to synchronize.
  class DapConnection
  {
    public:
      using MessageCallback = std::function<void(Json message)>;

      DapConnection() = default;
      ~DapConnection();

      DapConnection(const DapConnection&) = delete;
      DapConnection& operator=(const DapConnection&) = delete;

      // Binds and listens on [port]. Returns false if the socket couldn't be
      // created or bound.
      bool listen(int port);

      // Starts accepting clients in the background. When a client connects
      // and completes DAP handshake messages begin arriving via [onMessage].
      // After a client disconnects, the server keeps listening for the next
      // one.
      void start(MessageCallback onMessage);

      // Sends [message] to the current client, if one is connected.
      // Thread-safe.
      void send(const Json& message);

      // Returns true if a client is currently connected.
      bool isConnected();

      // Shuts down the current client connection so the receive loop exits
      // and the server can accept the next client. Unlike stop(), this is
      // safe to call from the receive thread (i.e. inside a message
      // callback).
      void disconnectClient();

      // Stops the accept and receive threads and closes all sockets.
      void stop();

    private:
      void acceptLoop();
      void receiveLoop(SocketFd socket);
      bool sendAll(SocketFd socket, const char* data, size_t length);

      SocketFd listenSocket_ = kInvalidSocket;
      SocketFd clientSocket_ = kInvalidSocket;
      std::thread acceptThread_;
      std::thread receiveThread_;
      std::mutex mutex_;
      MessageCallback onMessage_;
      bool stopping_ = false;
#ifdef _WIN32
      // Winsock must be initialized before the first socket call; WSAStartup
      // and WSACleanup are reference counted, so a started/cleanup pair per
      // connection is safe even with several instances alive.
      bool wsaInitialized_ = false;
#endif
  };
}
}

#endif

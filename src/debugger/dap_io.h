#pragma once
#ifndef dap_io_h
#define dap_io_h

#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "wren_json.h"

namespace wren
{
namespace debug
{
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
      void receiveLoop(int socket);
      bool sendAll(int socket, const char* data, size_t length);

      int listenSocket_ = -1;
      int clientSocket_ = -1;
      std::thread acceptThread_;
      std::thread receiveThread_;
      std::mutex mutex_;
      MessageCallback onMessage_;
      bool stopping_ = false;
  };
}
}

#endif

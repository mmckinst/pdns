#include <unistd.h>

#include "remote_logger.hh"

bool RemoteLogger::reconnect()
{
  if (d_socket >= 0) {
    close(d_socket);
  }
  try {
    d_socket = SSocket(d_remote.sin4.sin_family, SOCK_STREAM, 0);
    SConnect(d_socket, d_remote);
    setNonBlocking(d_socket);
  }
  catch(const std::exception& e) {
    std::cerr<<"Error connecting to remote logger "<<d_remote.toStringWithPort()<<": "<<e.what()<<std::endl;
    return false;
  }
  return true;
}

bool RemoteLogger::sendData(const char* buffer, size_t bufferSize)
{
  size_t pos = 0;
  while(pos < bufferSize) {
    ssize_t written = write(d_socket, buffer + pos, bufferSize - pos);
    if (written == -1) {
      int res = errno;
      if (res == EWOULDBLOCK || res == EAGAIN) {
        return false;
      }
      else if (res != EINTR) {
        reconnect();
        return false;
      }
    }
    else if (written == 0) {
      reconnect();
      return false;
    }
    else {
      pos += (size_t) written;
    }
  }

  return true;
}

void RemoteLogger::worker()
{
  while(true) {
    std::string data;
    {
      std::unique_lock<std::mutex> lock(d_writeMutex);
      d_queueCond.wait(lock, [this]{return (!d_writeQueue.empty()) || d_exiting;});
      if (d_exiting) {
        return;
      }
      data = d_writeQueue.front();
      d_writeQueue.pop();
    }

    try {
      uint16_t len = data.length();
      len = htons(len);
      writen2WithTimeout(d_socket, &len, sizeof(len), (int) d_timeout);
      writen2WithTimeout(d_socket, data.c_str(), data.length(), (int) d_timeout);
    }
    catch(const std::runtime_error& e) {
      //vinfolog("Error sending data to remote logger (%s): %s", d_remote.toStringWithPort(), e.what());
      while (!reconnect()) {
        sleep(d_reconnectWaitTime);
      }
    }
  }
}

void RemoteLogger::queueData(const std::string& data)
{
  {
    std::unique_lock<std::mutex> lock(d_writeMutex);
    if (d_writeQueue.size() >= d_maxQueuedEntries) {
      d_writeQueue.pop();
    }
    d_writeQueue.push(data);
  }
  d_queueCond.notify_one();
}

RemoteLogger::RemoteLogger(const ComboAddress& remote, uint16_t timeout, uint64_t maxQueuedEntries, uint8_t reconnectWaitTime): d_remote(remote), d_maxQueuedEntries(maxQueuedEntries), d_timeout(timeout), d_reconnectWaitTime(reconnectWaitTime), d_thread(&RemoteLogger::worker, this)
{
  reconnect();
}

RemoteLogger::~RemoteLogger()
{
  d_exiting = true;
  if (d_socket >= 0)
    close(d_socket);
  d_thread.join();
}


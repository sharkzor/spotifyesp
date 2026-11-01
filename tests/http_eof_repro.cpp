#include "HTTPClient.h"

#include <cstring>
#include <iostream>

namespace bell {
int SocketBuffer::open(const std::string& hostname, int, bool) {
  const char* response = "HTTP/1.1 206 Partial Content\r\nContent-Length: 0\r\n\r\n";
  std::string input = response;
  if (hostname == "healthy.test") input += response;
  std::memcpy(ibuf, input.data(), input.size());
  setg(ibuf, ibuf, ibuf + input.size());
  setp(obuf, obuf + bufLen);
  return 0;
}

int SocketBuffer::close() { return 0; }
int SocketBuffer::sync() {
  setp(obuf, obuf + bufLen);
  return 0;
}
SocketBuffer::int_type SocketBuffer::underflow() { return traits_type::eof(); }
SocketBuffer::int_type SocketBuffer::overflow(int_type c) {
  sync();
  return traits_type::not_eof(c);
}
std::streamsize SocketBuffer::xsgetn(char_type* output, std::streamsize count) {
  return std::streambuf::xsgetn(output, count);
}
std::streamsize SocketBuffer::xsputn(const char_type*, std::streamsize count) {
  return count;
}
}  // namespace bell

int main(int argc, char**) {
  const std::string url =
      argc > 1 ? "http://closed.test/audio" : "http://healthy.test/audio";
  auto response = bell::HTTPClient::get(url);
  std::cerr << "Initial range response parsed successfully\n";
  try {
    response->get(url);
    std::cerr << "Second range response parsed successfully\n";
  } catch (const std::runtime_error& e) {
    std::cerr << "Rejected: " << e.what() << "\n";
    return argc > 1 ? 0 : 1;
  }
  return argc > 1 ? 1 : 0;
}

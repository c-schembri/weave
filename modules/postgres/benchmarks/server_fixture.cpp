#include "tls_certificates.hpp"
#include <openssl/crypto.h>
#include <cstdio>
#include <iostream>
#include <string>

int main()
{
  std::filesystem::path directory;
  {
    fixture::Certificates files;
    directory = files.directory;
    if (OpenSSL_version_num() != OPENSSL_VERSION_NUMBER)
      return EXIT_FAILURE;

    std::printf(
      "%s\n%s\n%s\n%s\n",
      files.ca.c_str(),
      files.leaf.c_str(),
      files.private_key.c_str(),
      OpenSSL_version(OPENSSL_VERSION));
    std::fflush(stdout);

    std::string command;
    if (!std::getline(std::cin, command) || command != "stop")
      return EXIT_FAILURE;
  }
  return std::filesystem::exists(directory) ? EXIT_FAILURE : EXIT_SUCCESS;
}

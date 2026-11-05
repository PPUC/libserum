// serum-signing: keys and certificates for signed colorizations (see
// src/SerumSigning.h).
//
//   serum-signing keygen <secret-file>
//       Writes a new secret key (the seed, in hex) and prints its public key.
//   serum-signing public <secret-file>
//       Prints the public key of a secret key.
//   serum-signing certify <signer-secret-file> <issuer|author> <public-key>
//                         <identity> <name> [<signer-certificate-file>]
//       Prints a certificate for the public key, signed with the signer's
//       secret key, in its text form. A root key signs without a certificate
//       of its own; an issuer passes its own certificate, which is appended.
//   serum-signing show <certificate-file>
//       Prints the certificates and whether they lead to a trusted root key.
//   serum-signing check <file.cROMc>
//       Prints whether the file is signed, by whom, and whether it is
//       unchanged.
//
// The secret key of a root or issuer must not leave the hands of the people
// who keep it.

#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "SerumSigning.h"

using namespace serum_signing;

namespace {

std::string Hex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 15];
  }
  return out;
}

bool FromHex(const std::string& text, uint8_t* out, size_t size) {
  std::string digits;
  for (char c : text)
    if (!isspace((unsigned char)c)) digits += c;
  if (digits.size() != size * 2) return false;
  for (size_t i = 0; i < size; ++i) {
    unsigned value = 0;
    if (sscanf(digits.c_str() + 2 * i, "%2x", &value) != 1) return false;
    out[i] = (uint8_t)value;
  }
  return true;
}

bool ReadText(const char* path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool ReadSeed(const char* path, Seed& seed) {
  std::string text;
  if (!ReadText(path, text) || !FromHex(text, seed.data(), seed.size())) {
    fprintf(stderr, "%s holds no secret key\n", path);
    return false;
  }
  return true;
}

int Usage() {
  fprintf(stderr,
          "usage: serum-signing keygen <secret-file>\n"
          "       serum-signing public <secret-file>\n"
          "       serum-signing certify <signer-secret-file> <issuer|author> "
          "<public-key> <identity> <name> [<signer-certificate-file>]\n"
          "       serum-signing show <certificate-file>\n"
          "       serum-signing check <file.cROMc>\n");
  return 2;
}

void PrintChain(const std::vector<Certificate>& chain) {
  for (const Certificate& certificate : chain) {
    printf("%s %s (%s)\n  key    %s\n  signer %s\n",
           certificate.role == CertificateRole::Author ? "author" : "issuer",
           certificate.name.c_str(), certificate.identity.c_str(),
           Hex(certificate.key.data(), 32).c_str(),
           Hex(certificate.signer.data(), 32).c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string command = argv[1];
  if (command == "keygen" && argc == 3) {
    std::ifstream existing(argv[2]);
    if (existing) {
      fprintf(stderr, "%s exists; not overwritten\n", argv[2]);
      return 1;
    }
    std::random_device random;
    Seed seed;
    for (uint8_t& b : seed) b = (uint8_t)random();
    std::ofstream out(argv[2], std::ios::binary);
    out << Hex(seed.data(), seed.size()) << "\n";
    if (!out) {
      fprintf(stderr, "%s cannot be written\n", argv[2]);
      return 1;
    }
    const PublicKey key = PublicKeyFromSeed(seed);
    printf("%s\n", Hex(key.data(), key.size()).c_str());
    return 0;
  }
  if (command == "public" && argc == 3) {
    Seed seed;
    if (!ReadSeed(argv[2], seed)) return 1;
    const PublicKey key = PublicKeyFromSeed(seed);
    printf("%s\n", Hex(key.data(), key.size()).c_str());
    return 0;
  }
  if (command == "certify" && (argc == 7 || argc == 8)) {
    Seed signer;
    if (!ReadSeed(argv[2], signer)) return 1;
    const std::string role = argv[3];
    if (role != "issuer" && role != "author") return Usage();
    PublicKey key;
    if (!FromHex(argv[4], key.data(), key.size())) {
      fprintf(stderr, "%s is no public key\n", argv[4]);
      return 1;
    }
    std::vector<Certificate> chain;
    chain.push_back(Certify(
        role == "issuer" ? CertificateRole::Issuer : CertificateRole::Author,
        key, (int64_t)time(nullptr), argv[5], argv[6], signer));
    if (argc == 8) {
      std::string text;
      std::vector<uint8_t> bytes;
      std::vector<Certificate> signerChain;
      if (!ReadText(argv[7], text) || !ChainFromText(text, bytes) ||
          !ParseChain(bytes.data(), bytes.size(), signerChain)) {
        fprintf(stderr, "%s holds no certificate\n", argv[7]);
        return 1;
      }
      if (signerChain.front().key != PublicKeyFromSeed(signer)) {
        fprintf(stderr, "%s is not the certificate of that secret key\n",
                argv[7]);
        return 1;
      }
      chain.insert(chain.end(), signerChain.begin(), signerChain.end());
    }
    printf("%s\n", ChainToText(SerializeChain(chain)).c_str());
    return 0;
  }
  if (command == "show" && argc == 3) {
    std::string text;
    std::vector<uint8_t> bytes;
    std::vector<Certificate> chain;
    if (!ReadText(argv[2], text) || !ChainFromText(text, bytes) ||
        !ParseChain(bytes.data(), bytes.size(), chain)) {
      fprintf(stderr, "%s holds no certificate\n", argv[2]);
      return 1;
    }
    PrintChain(chain);
    std::string reason;
    if (chain.front().role == CertificateRole::Author) {
      if (CheckChain(chain, reason)) {
        printf("trusted\n");
        return 0;
      }
      printf("not trusted: %s\n", reason.c_str());
      return 1;
    }
    return 0;
  }
  if (command == "check" && argc == 3) {
    const CheckResult result = CheckFile(argv[2]);
    switch (result.authenticity) {
      case Authenticity::Unsigned:
        printf("not signed\n");
        return 1;
      case Authenticity::Signed:
        printf("signed by %s (%s)\n", result.name.c_str(),
               result.identity.c_str());
        return 0;
      case Authenticity::Modified:
        printf("modified: %s\n", result.reason.c_str());
        return 1;
    }
  }
  return Usage();
}

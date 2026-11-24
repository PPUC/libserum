#pragma once

// Signed colorizations: who signed a cROMc, and whether it was changed since.
//
// An author signs the cROMc their editor exports with their own Ed25519 key. A
// certificate says whose key that is (an identity such as "github:<id>" and a
// display name); it is signed by an issuing key, whose own certificate is
// signed by a root key, or by a root key directly. libserum knows the public
// root keys (kRootPublicKeys) and a list of revoked keys, nothing else: the
// check needs no connection.
//
// The signature is a block behind the compressed data of the cROMc
// (BuildTrailer() below). It signs the digest of every byte before it, which
// is taken in chunks (ContentDigest()).
//
// None of this protects anything: anyone can still change a cROMc and remove
// the signature. It tells a player whether the file is still the one its
// author signed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace serum_signing {

using PublicKey = std::array<uint8_t, 32>;
using Seed = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;
using Digest = std::array<uint8_t, 64>;

// SHA-512 (FIPS 180-4), fed in pieces.
class Sha512 {
 public:
  Sha512();
  void update(const void* data, size_t size);
  Digest finish();

 private:
  void block(const uint8_t* data);
  uint64_t m_state[8];
  uint8_t m_buffer[128];
  size_t m_buffered = 0;
  uint64_t m_length = 0;
};

Digest Sha512Of(const void* data, size_t size);

// The content of a signed cROMc -- every byte before its signature block --
// is hashed in chunks of kContentChunkBytes, the last one shorter, and the
// signature covers the digest of those digests: the SHA-512 of the chunk size
// (uint32), the content's length (uint64), both little-endian, and the
// SHA-512 of each chunk in order. A file of no bytes has no chunk.
constexpr uint32_t kContentChunkBytes = 1u << 20;

// The number of chunks of a content of `contentBytes` bytes.
uint64_t ContentChunks(uint64_t contentBytes);
// The digest the signature covers, of the chunks' digests.
Digest ContentDigest(const std::vector<Digest>& chunkDigests,
                     uint64_t contentBytes);
// The digest of a content in memory, chunk by chunk.
Digest ContentDigestOf(const void* data, size_t size);

// Ed25519 (RFC 8032). A secret key is its 32-byte seed.
PublicKey PublicKeyFromSeed(const Seed& seed);
Signature Sign(const Seed& seed, const void* message, size_t size);
bool Verify(const PublicKey& key, const void* message, size_t size,
            const Signature& signature);

enum class CertificateRole : uint8_t {
  // May sign author certificates.
  Issuer = 1,
  // May sign colorizations.
  Author = 2,
};

// A certificate, as bytes:
//   "SCRT", uint8 version (1), uint8 role, uint16 0,
//   the public key (32 bytes),
//   int64 time of issue (seconds since 1970, UTC),
//   uint16 identity length, the identity (UTF-8),
//   uint16 name length, the name (UTF-8),
//   the public key of the signer (32 bytes),
//   the signer's signature of every byte before it (64 bytes).
// Numbers little endian.
struct Certificate {
  CertificateRole role = CertificateRole::Author;
  PublicKey key{};
  int64_t issuedAt = 0;
  // Who the key belongs to: a provider and an ID that does not change, such
  // as "github:1234567". Never looked up.
  std::string identity;
  // The name to show, as it was when the certificate was issued.
  std::string name;
  PublicKey signer{};
  Signature signature{};

  // The bytes the signer signs.
  std::vector<uint8_t> signedBytes() const;
  std::vector<uint8_t> serialize() const;
  // False for bytes that are no certificate of a known version. Does not
  // check the signature.
  static bool parse(const uint8_t* data, size_t size, Certificate& out);
};

// A certificate signed with a secret key.
Certificate Certify(CertificateRole role, const PublicKey& key,
                    int64_t issuedAt, const std::string& identity,
                    const std::string& name, const Seed& signerSeed);

// A chain of certificates as an author hands it over, leaf first: the
// author's certificate, then the issuer's, if a root key did not sign the
// author's directly. As bytes: uint16 count, then per certificate uint32
// length and its bytes.
std::vector<uint8_t> SerializeChain(const std::vector<Certificate>& chain);
bool ParseChain(const uint8_t* data, size_t size,
                std::vector<Certificate>& out);

// The text form of a chain, to be copied around: "SERUM-CERTIFICATE-1:"
// followed by the bytes in base64.
std::string ChainToText(const std::vector<uint8_t>& chainBytes);
bool ChainFromText(const std::string& text, std::vector<uint8_t>& chainBytes);

// Whether the chain leads from an author key to a root key libserum knows,
// with every signature valid and no key revoked. reason says why not.
bool CheckChain(const std::vector<Certificate>& chain, std::string& reason);

// The root keys libserum trusts, and the keys it no longer trusts.
const std::vector<PublicKey>& RootPublicKeys();
const std::vector<PublicKey>& RevokedKeys();
// For tests only: the root keys to trust instead (empty: the built-in ones).
void SetRootPublicKeysForTesting(const std::vector<PublicKey>& keys);

// The block behind the compressed data of a signed cROMc:
//   "SSIG", uint16 version (2), uint16 0,
//   uint32 chain length, the chain (SerializeChain()),
//   the signature (64 bytes),
//   uint32 length of the whole block, "SSIG".
// The signature is the author key's of the message "SERUM-CROMC-2" followed
// by the ContentDigest() of every byte of the file before the block. A block
// of another version is not read: the file counts as modified.
constexpr uint32_t kTrailerMagic = 0x47495353;  // "SSIG"
constexpr size_t kTrailerTailBytes = 8;

std::vector<uint8_t> BuildTrailer(const Digest& contentDigest,
                                  const std::vector<uint8_t>& chainBytes,
                                  const Seed& authorSeed);

enum class Authenticity {
  // No signature.
  Unsigned,
  // Signed by the author the certificate names, unchanged since.
  Signed,
  // A signature that does not match the file, or one no trusted key
  // vouches for.
  Modified,
};

struct CheckResult {
  Authenticity authenticity = Authenticity::Unsigned;
  std::string identity;
  std::string name;
  std::string reason;
  // Bytes of the file before the signature block: the cROMc as it was
  // written. The whole file for one without a signature.
  uint64_t contentBytes = 0;
};

// The size of the signature block at the end of a file of `size` bytes, 0
// for a file without one. Leaves the file position anywhere.
uint64_t TrailerBytes(FILE* fp, uint64_t size);

// Reads the signature block of a cROMc, if it has one, and checks it against
// the bytes before it. chunkDigests, when given, are the SHA-512 of the
// chunks of those bytes, which are then not read; digests of another number
// of chunks are not used.
CheckResult CheckFile(const char* path,
                      const std::vector<Digest>* chunkDigests = nullptr);

}  // namespace serum_signing

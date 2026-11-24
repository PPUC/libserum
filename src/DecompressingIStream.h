#pragma once

#include <cstdio>
#include <istream>
#include <stdexcept>
#include <vector>

#include "miniz/miniz.h"
#include "serum-accel.h"

// Buffers - use conservative sizes that account for high compression ratios
#define COMPRESSED_BUFFER_SIZE (16 * 1024)     // 16KB compressed chunks
#define DECOMPRESSED_BUFFER_SIZE (512 * 1024)  // 512KB decompressed buffer

// Custom stream class that decompresses on the fly
class DecompressingIStream : public std::istream {
 private:
  class DecompressingStreamBuf : public std::streambuf {
   private:
    FILE *m_fp;
    uint32_t m_compressedSize;
    uint32_t m_originalSize;
    uint32_t m_compressedRead;
    // Where the compressed data start in the file, for ContentRead().
    uint64_t m_fileOffset;
    mz_stream m_stream;
    bool m_streamInitialized;
    bool m_finished;

    std::vector<unsigned char> m_compressedBuffer;
    std::vector<char> m_decompressedBuffer;

    size_t m_decompressedPos;
    size_t m_decompressedAvailable;

   public:
    DecompressingStreamBuf(FILE *fp, uint32_t compressedSize,
                           uint32_t originalSize)
        : m_fp(fp),
          m_compressedSize(compressedSize),
          m_originalSize(originalSize),
          m_compressedRead(0),
          m_fileOffset(0),
          m_streamInitialized(false),
          m_finished(false),
          m_decompressedPos(0),
          m_decompressedAvailable(0) {
      const long start = ftell(fp);
      if (start > 0) m_fileOffset = (uint64_t)start;
      // Initialize miniz stream
      memset(&m_stream, 0, sizeof(m_stream));
      m_stream.zalloc = Z_NULL;
      m_stream.zfree = Z_NULL;
      m_stream.opaque = Z_NULL;

      int status = mz_inflateInit(&m_stream);
      if (status != MZ_OK) {
        throw std::runtime_error("Failed to initialize inflate stream: " +
                                 std::to_string(status));
      }
      m_streamInitialized = true;

      m_compressedBuffer.resize(COMPRESSED_BUFFER_SIZE);
      m_decompressedBuffer.resize(DECOMPRESSED_BUFFER_SIZE);

      // Set up streambuf
      setg(0, 0, 0);
    }

    virtual ~DecompressingStreamBuf() {
      if (m_streamInitialized) {
        mz_inflateEnd(&m_stream);
      }
    }

   protected:
    int_type underflow() override {
      if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
      }

      if (m_finished) {
        return traits_type::eof();
      }

      // Need to decompress more data
      if (!decompressMore()) {
        return traits_type::eof();
      }

      setg(m_decompressedBuffer.data(),
           m_decompressedBuffer.data() + m_decompressedPos,
           m_decompressedBuffer.data() + m_decompressedAvailable);

      m_decompressedPos = 0;  // Reset for next read

      return traits_type::to_int_type(*gptr());
    }

   private:
    bool decompressMore() {
      if (m_finished) {
        return false;
      }

      m_decompressedAvailable = 0;
      m_decompressedPos = 0;

      // Continue decompressing until we fill the buffer or run out of data
      while (m_decompressedAvailable < m_decompressedBuffer.size()) {
        // Read more compressed data if needed
        if (m_stream.avail_in == 0 && m_compressedRead < m_compressedSize) {
          size_t toRead =
              std::min((size_t)(m_compressedSize - m_compressedRead),
                       (size_t)COMPRESSED_BUFFER_SIZE);
          size_t bytesRead = fread(m_compressedBuffer.data(), 1, toRead, m_fp);
          if (bytesRead == 0) {
            if (feof(m_fp)) {
              break;
            }
            throw std::runtime_error("Failed to read compressed data");
          }

          serum_accel::ContentRead(m_fileOffset + m_compressedRead,
                                   m_compressedBuffer.data(), bytesRead);
          m_compressedRead += bytesRead;
          m_stream.avail_in = (uint32_t)bytesRead;
          m_stream.next_in = m_compressedBuffer.data();
        }

        // If we have no more input and no data in the decompressor, we're done
        if (m_stream.avail_in == 0 && m_compressedRead >= m_compressedSize) {
          break;
        }

        // Decompress to our buffer
        size_t remainingSpace =
            m_decompressedBuffer.size() - m_decompressedAvailable;
        m_stream.avail_out = (uint32_t)remainingSpace;
        m_stream.next_out = reinterpret_cast<unsigned char *>(
            m_decompressedBuffer.data() + m_decompressedAvailable);

        int status = mz_inflate(&m_stream, MZ_NO_FLUSH);

        size_t decompressedThisRound = remainingSpace - m_stream.avail_out;
        m_decompressedAvailable += decompressedThisRound;

        if (status == MZ_STREAM_END) {
          m_finished = true;
          break;
        } else if (status != MZ_OK && status != MZ_BUF_ERROR) {
          throw std::runtime_error("Decompression failed: " +
                                   std::to_string(status));
        }

        // If we didn't make progress and have no more input, we're done
        if (decompressedThisRound == 0 && m_stream.avail_in == 0 &&
            m_compressedRead >= m_compressedSize) {
          break;
        }
      }

      return m_decompressedAvailable > 0;
    }
  };

  DecompressingStreamBuf m_buf;

 public:
  DecompressingIStream(FILE *fp, uint32_t compressedSize, uint32_t originalSize)
      : std::istream(&m_buf), m_buf(fp, compressedSize, originalSize) {}
};

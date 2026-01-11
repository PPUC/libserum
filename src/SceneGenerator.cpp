#include "SceneGenerator.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TimeUtils.h"

std::string formatNumber(int num, int width) {
  std::string s = std::to_string(num);
  if (s.length() < width) {
    s = std::string(width - s.length(), '0') + s;
  }
  return s;
}

// Constants for text positioning
const int SCENE_Y = 2;
const int GROUP_Y = 12;
const int FRAME_Y = 22;
const int RIGHT_ALIGN_X = 127;
const int NUMBER_WIDTH = 5;
const int NUMBER_PIXELS =
    NUMBER_WIDTH * 6 - 1;  // 5 digits * 6px/digit - last space

// Right-aligned starting position for numbers
const int NUM_X = RIGHT_ALIGN_X - NUMBER_PIXELS + 1;

SceneGenerator::SceneGenerator() : m_templateInitialized(false) {
  initializeTemplate();
}

void SceneGenerator::setDepth(uint8_t depth) {
  m_depth = depth;
  m_templateInitialized = false;
  initializeTemplate();
}

bool SceneGenerator::parseCSV(const std::string &csv_filename) {
  std::ifstream in_csv(csv_filename);
  if (!in_csv.is_open()) {
    // Log(DMDUtil_LogLevel_ERROR, "SceneGenerator: Could not open CSV file:
    // %s", csv_filename.c_str());
    return false;
  }

  m_sceneData.clear();
  std::string line;
  int lineNum = 0;
  while (std::getline(in_csv, line)) {
    lineNum++;
    if (line.empty()) continue;

    // Handle Windows line endings (CRLF)
    if (!line.empty() && line[line.size() - 1] == '\r') {
      line.pop_back();
    }

    // Skip comment lines
    if (!line.empty() && line[0] == '#') {
      continue;
    }

    std::istringstream ss(line);
    std::vector<std::string> row;
    std::string cell;
    while (std::getline(ss, cell, ',')) {
      row.push_back(cell);
    }

    // Need at least 3 columns, accept up to 4
    if (row.size() < 3) {
      // Log(DMDUtil_LogLevel_ERROR, "SceneGenerator: Skipping invalid line %d -
      // expected at least 3 columns", lineNum);
      continue;
    }

    try {
      SceneData data;
      data.sceneId = (uint16_t)std::stoi(row[0]);
      data.frameCount = (uint16_t)std::stoi(row[1]);
      data.durationPerFrame = (uint16_t)std::stoi(row[2]);
      if (row.size() >= 4) data.interruptable = (std::stoi(row[3]) == 1);
      if (row.size() >= 5) data.immediateStart = (std::stoi(row[4]) == 1);
      if (row.size() >= 6) data.repeat = (uint8_t)std::stoi(row[5]);
      if (row.size() >= 7)
        data.frameGroups =
            (uint8_t)std::stoi(row[6]) == 0 ? 1 : std::stoi(row[6]);
      if (row.size() >= 8) data.random = (std::stoi(row[7]) == 1);
      if (row.size() >= 9) {
        data.autoStart = (uint8_t)std::stoi(row[8]);
        if (data.autoStart > 0) {
          m_autoStartTimer = data.autoStart;
          m_autoStartSceneId = data.sceneId;
        }
      }
      if (row.size() >= 10) data.endFrame = (uint8_t)std::stoi(row[9]);

      m_sceneData.push_back(data);
    } catch (...) {
      // Log(DMDUtil_LogLevel_ERROR, "SceneGenerator: Skipping invalid line %d -
      // non-integer value", lineNum);
      continue;
    }
  }

  m_active = !m_sceneData.empty();

  return true;
}

bool SceneGenerator::generateDump(const std::string &dump_filename, int id) {
  std::ofstream out_dump(dump_filename, std::ios::binary);
  if (!out_dump.is_open()) {
    // Log(DMDUtil_LogLevel_ERROR, "SceneGenerator: Could not create dump file:
    // %s", dump_filename.c_str());
    return false;
  }

  uint32_t cumulative_duration = 0;
  for (const auto &scene : m_sceneData) {
    if (id != -1 && scene.sceneId != (uint16_t)id) {
      continue;  // Skip scenes that don't match the specified ID
    }

    int goups = scene.frameGroups > 0 ? scene.frameGroups : 1;
    for (int group = 1; group <= goups; group++) {
      for (int frameIndex = 0; frameIndex < scene.frameCount; frameIndex++) {
        cumulative_duration += static_cast<uint32_t>(scene.durationPerFrame);

        // Format as 8-digit hex with 0x prefix
        char hex_line[11];
        std::snprintf(hex_line, sizeof(hex_line), "0x%08x",
                      cumulative_duration);
        out_dump << hex_line << "\r\n";

        uint8_t frameBuffer[4096];
        if (!generateFrame(scene.sceneId, frameIndex, frameBuffer, group, true)) {
          // Log(DMDUtil_LogLevel_ERROR, "SceneGenerator: Error generating frame
          // %d for scene %d", frameIndex,
          //    scene.sceneId);
          continue;
        }

        // Write 128x32 grid
        for (int row = 0; row < 32; row++) {
          for (int col = 0; col < 128; col++) {
            uint8_t value = frameBuffer[row * 128 + col];
            out_dump << static_cast<char>(value ? (m_depth == 2 ? '3' : 'f')
                                                : '0');
          }
          out_dump << "\r\n";
        }
        out_dump << "\r\n";  // Empty line between frames
      }
    }
  }

  return true;
}

bool SceneGenerator::getSceneInfo(uint16_t sceneId, uint16_t &frameCount,
                                  uint16_t &durationPerFrame,
                                  bool &interruptable, bool &startImmediately,
                                  uint8_t &repeat, uint8_t &endFrame) const {
  auto it = std::find_if(
      m_sceneData.begin(), m_sceneData.end(),
      [sceneId](const SceneData &data) { return data.sceneId == sceneId; });

  if (it == m_sceneData.end()) {
    frameCount = 0;
    durationPerFrame = 0;
    interruptable = 0;
    startImmediately = false;
    repeat = 0;
    endFrame = 0;
    return false;
  }

  frameCount = it->frameCount;
  durationPerFrame = it->durationPerFrame;
  interruptable = it->interruptable;
  startImmediately = it->immediateStart;
  repeat = it->repeat;
  endFrame = it->endFrame;
  return true;
}

bool SceneGenerator::getAutoStartSceneInfo(
    uint16_t &frameCount, uint16_t &durationPerFrame, bool &interruptable,
    bool &startImmediately, uint8_t &repeat, uint8_t &endFrame) const {
  if (m_autoStartSceneId > 0) {
    return getSceneInfo(m_autoStartSceneId, frameCount, durationPerFrame,
                        interruptable, startImmediately, repeat, endFrame);
  }

  return false;
}

uint16_t SceneGenerator::generateFrame(uint16_t sceneId, uint16_t frameIndex,
                                       uint8_t *buffer, int group,
                                       bool disableTimer) {
  static uint32_t lastTime = 0;
  if (frameIndex == 0) lastTime = 0;  // Reset timer for new scene
  uint32_t now = GetMonotonicTimeMs();

  auto it = std::find_if(
      m_sceneData.begin(), m_sceneData.end(),
      [sceneId](const SceneData &data) { return data.sceneId == sceneId; });

  if (it == m_sceneData.end()) {
    return 0;
  }

  if (frameIndex < 0 || frameIndex >= it->frameCount) {
    return 0;
  }

  if (!disableTimer && (lastTime + it->durationPerFrame) > now) {
    // Too soon to generate the next frame, return remaining time
    return it->durationPerFrame - (now - lastTime);
  }
  lastTime = now;

  if (frameIndex == 0) {
    if (group == -1) {
      if (it->frameGroups > 1) {
        if (it->random) {
          it->currentGroup = rand() % it->frameGroups + 1;
        } else {
          it->currentGroup++;
          if (it->currentGroup > it->frameGroups) it->currentGroup = 1;
        }
      } else {
        it->currentGroup = 1;
      }
    } else {
      it->currentGroup = (uint8_t)group;
    }
  }

  // Copy pre-rendered template
  std::memcpy(buffer, m_template.fullFrame, 4096);

  // Render dynamic numbers at right-aligned positions
  std::string sceneIdStr = formatNumber(sceneId, NUMBER_WIDTH);
  renderString(buffer, sceneIdStr, NUM_X, SCENE_Y);

  std::string groupStr = formatNumber(it->currentGroup, NUMBER_WIDTH);
  renderString(buffer, groupStr, NUM_X, GROUP_Y);

  std::string frameStr = formatNumber(frameIndex + 1, NUMBER_WIDTH);
  renderString(buffer, frameStr, NUM_X, FRAME_Y);

  return 0xffff;  // Success
}

void SceneGenerator::initializeTemplate() {
  if (m_templateInitialized) return;

  // Initialize with all zeros (off pixels)
  std::memset(m_template.fullFrame, 0, 4096);

  // Render fixed text to template at new positions
  renderString(m_template.fullFrame, "PUP SCENE ID", 0, SCENE_Y);
  renderString(m_template.fullFrame, "FRAME GROUP", 0, GROUP_Y);
  renderString(m_template.fullFrame, "FRAME NUMBER", 0, FRAME_Y);

  m_templateInitialized = true;
}

const unsigned char *SceneGenerator::getCharFont(char c) const {
  static const unsigned char digits[10][7] = {
      {14, 17, 17, 17, 17, 17, 14},  // '0'
      {4, 12, 4, 4, 4, 4, 14},       // '1'
      {14, 17, 1, 2, 4, 8, 31},      // '2'
      {14, 17, 1, 6, 1, 17, 14},     // '3'
      {2, 6, 10, 18, 31, 2, 2},      // '4'
      {31, 16, 30, 1, 1, 17, 14},    // '5'
      {14, 16, 16, 30, 17, 17, 14},  // '6'
      {31, 1, 2, 4, 4, 4, 4},        // '7'
      {14, 17, 17, 14, 17, 17, 14},  // '8'
      {14, 17, 17, 15, 1, 1, 14}     // '9'
  };
  static const unsigned char A[7] = {14, 17, 17, 31, 17, 17, 17};  // 'A'
  static const unsigned char B[7] = {30, 17, 17, 30, 17, 17, 30};  // 'B'
  static const unsigned char C[7] = {14, 17, 16, 16, 16, 17, 14};  // 'C'
  static const unsigned char D[7] = {30, 17, 17, 17, 17, 17, 30};  // 'D'
  static const unsigned char E[7] = {31, 16, 16, 30, 16, 16, 31};  // 'E'
  static const unsigned char F[7] = {31, 16, 16, 30, 16, 16, 16};  // 'F'
  static const unsigned char G[7] = {14, 17, 16, 19, 17, 17, 14};  // 'G'
  static const unsigned char H[7] = {17, 17, 17, 31, 17, 17, 17};  // 'H'
  static const unsigned char I[7] = {31, 4, 4, 4, 4, 4, 31};       // 'I'
  static const unsigned char J[7] = {15, 2, 2, 2, 18, 18, 12};     // 'J'
  static const unsigned char K[7] = {17, 18, 20, 24, 20, 18, 17};  // 'K'
  static const unsigned char L[7] = {16, 16, 16, 16, 16, 16, 31};  // 'L'
  static const unsigned char M[7] = {17, 27, 21, 17, 17, 17, 17};  // 'M'
  static const unsigned char N[7] = {17, 17, 25, 21, 19, 17, 17};  // 'N'
  static const unsigned char O[7] = {14, 17, 17, 17, 17, 17, 14};  // 'O'
  static const unsigned char P[7] = {30, 17, 17, 30, 16, 16, 16};  // 'P'
  static const unsigned char Q[7] = {14, 17, 17, 17, 21, 18, 13};  // 'Q'
  static const unsigned char R[7] = {30, 17, 17, 30, 18, 17, 17};  // 'R'
  static const unsigned char S[7] = {14, 17, 16, 14, 1, 17, 14};   // 'S'
  static const unsigned char T[7] = {31, 4, 4, 4, 4, 4, 4};        // 'T'
  static const unsigned char U[7] = {17, 17, 17, 17, 17, 17, 14};  // 'U'
  static const unsigned char V[7] = {17, 17, 17, 17, 10, 10, 4};   // 'V'
  static const unsigned char W[7] = {17, 17, 17, 21, 21, 21, 10};  // 'W'
  static const unsigned char X[7] = {17, 17, 10, 4, 10, 17, 17};   // 'X'
  static const unsigned char Y[7] = {17, 17, 10, 4, 4, 4, 4};      // 'Y'
  static const unsigned char Z[7] = {31, 1, 2, 4, 8, 16, 31};      // 'Z'
  static const unsigned char space[7] = {0};                       // ' '

  if (c >= '0' && c <= '9') {
    return digits[c - '0'];
  } else {
    switch (c) {
      case 'A':
        return A;
      case 'B':
        return B;
      case 'C':
        return C;
      case 'D':
        return D;
      case 'E':
        return E;
      case 'F':
        return F;
      case 'G':
        return G;
      case 'H':
        return H;
      case 'I':
        return I;
      case 'J':
        return J;
      case 'K':
        return K;
      case 'L':
        return L;
      case 'M':
        return M;
      case 'N':
        return N;
      case 'O':
        return O;
      case 'P':
        return P;
      case 'Q':
        return Q;
      case 'R':
        return R;
      case 'S':
        return S;
      case 'T':
        return T;
      case 'U':
        return U;
      case 'V':
        return V;
      case 'W':
        return W;
      case 'X':
        return X;
      case 'Y':
        return Y;
      case 'Z':
        return Z;
      default:
        return space;
    }
  }
}

void SceneGenerator::renderChar(uint8_t *buffer, char c, uint8_t x,
                                uint8_t y) const {
  const unsigned char *font_data = getCharFont(c);
  for (uint8_t row = 0; row < 7; row++) {
    unsigned char byte = font_data[row];
    for (uint8_t col = 0; col < 5; col++) {
      int bit = (byte >> (4 - col)) & 1;
      if (bit) {
        int abs_x = x + col;
        int abs_y = y + row;
        if (abs_x < 128 && abs_y < 32) {
          buffer[abs_y * 128 + abs_x] =
              (m_depth == 2 ? 3 : 15);  // Use 3 for depth 2, 15 for depth 4
        }
      }
    }
  }
}

void SceneGenerator::renderString(uint8_t *buffer, const std::string &str,
                                  uint8_t x, uint8_t y) const {
  uint8_t currentX = x;
  for (char c : str) {
    renderChar(buffer, c, currentX, y);
    currentX += 6;  // 5 pixels width + 1 pixel space
  }
}

void SceneGenerator::Log(const char *format, ...) {
  if (!m_logCallback) {
    return;
  }

  va_list args;
  va_start(args, format);
  (*(m_logCallback))(format, args, m_logUserData);
  va_end(args);
}

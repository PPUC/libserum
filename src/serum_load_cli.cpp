#include "serum-decode.h"

#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void PrintUsage(const char* exe) {
  std::cout << "Usage: " << exe
            << " <serum-file-or-rom-dir> [--no-generate] [--quiet]\n"
            << "\n"
            << "Examples:\n"
            << "  " << exe << " /path/to/altcolor/afm_113b/afm_113b.cRZ\n"
            << "  " << exe << " /path/to/altcolor/afm_113b\n";
}

static std::string ToLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

static bool IsSerumFileExt(const fs::path& p) {
  const std::string ext = ToLower(p.extension().string());
  return ext == ".crz" || ext == ".crom" || ext == ".cromc";
}

static void SERUM_CALLBACK CliLog(const char* format, va_list args,
                                  const void* /*userData*/) {
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
}

int main(int argc, char** argv) {
  bool generateCRomC = true;
  bool quiet = false;
  std::string inputArg;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (arg == "--no-generate") {
      generateCRomC = false;
      continue;
    }
    if (arg == "--quiet") {
      quiet = true;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
    if (!inputArg.empty()) {
      std::cerr << "Multiple input paths provided\n";
      PrintUsage(argv[0]);
      return 2;
    }
    inputArg = arg;
  }

  if (inputArg.empty()) {
    PrintUsage(argv[0]);
    return 2;
  }

  fs::path input = fs::path(inputArg);
  std::error_code ec;
  input = fs::weakly_canonical(input, ec);
  if (ec) {
    input = fs::absolute(input, ec);
  }
  if (ec || !fs::exists(input)) {
    std::cerr << "Input path not found: " << inputArg << "\n";
    return 2;
  }

  fs::path romDir;
  bool inputIsRawSerum = false;
  if (fs::is_directory(input)) {
    romDir = input;
  } else if (fs::is_regular_file(input) && IsSerumFileExt(input)) {
    romDir = input.parent_path();
    const std::string ext = ToLower(input.extension().string());
    inputIsRawSerum = (ext == ".crz" || ext == ".crom");
  } else {
    std::cerr << "Input must be a rom directory or a .cRZ/.cROM/.cROMc file\n";
    return 2;
  }

  if (romDir.empty() || romDir.parent_path().empty()) {
    std::cerr << "Unable to derive altcolor path and rom name from input\n";
    return 2;
  }

  const std::string romname = romDir.filename().string();
  const std::string altcolorPath = romDir.parent_path().string();

  if (!quiet) {
    Serum_SetLogCallback(&CliLog, nullptr);
  }
  Serum_SetGenerateCRomC(generateCRomC);

  Serum_Frame_Struc* loaded =
      Serum_Load(altcolorPath.c_str(), romname.c_str(), 0);
  if (!loaded) {
    std::cerr << "Failed to load serum for rom '" << romname << "' from "
              << romDir.string() << "\n";
    return 1;
  }

  std::cout << "Loaded serum data for rom '" << romname
            << "' (version " << loaded->SerumVersion << ")\n";

  if (inputIsRawSerum && generateCRomC) {
    const fs::path concentratePath = romDir / (romname + ".cROMc");
    if (fs::exists(concentratePath)) {
      std::cout << "Generated/updated concentrate: "
                << concentratePath.string() << "\n";
    } else {
      std::cout << "Load succeeded, but no .cROMc detected at "
                << concentratePath.string() << "\n";
    }
  }

  Serum_Dispose();
  return 0;
}

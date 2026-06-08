#include "HalStorage.h"

#include <BoardProfile.h>
#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <SDCardManager.h>

#ifdef CROSSPOINT_BOARD_MURPHY_M4
#include <SD_MMC.h>
#define CROSSPOINT_HAS_SD_MMC_BACKEND 1
#endif

#include <cassert>
#include <cstring>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

namespace {

const BoardCapabilityProfile& activeBoardProfile() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  return boardProfileFor(BoardModel::MurphyM4);
#else
  return boardProfileFor(BoardModel::X4);
#endif
}

bool useSdMmcBackend() { return activeBoardProfile().sdUsesSdMmc; }

const char* modeForOflag(const oflag_t oflag) {
  if (oflag & O_APPEND) return FILE_APPEND;
  if (oflag & (O_WRITE | O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) return FILE_WRITE;
  return FILE_READ;
}

#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
bool openSdMmcForRead(const char* moduleName, const char* path, fs::File& file) {
  if (!SD_MMC.exists(path)) {
    LOG_ERR(moduleName, "File does not exist: %s", path);
    return false;
  }

  file = SD_MMC.open(path, FILE_READ);
  if (!file) {
    LOG_ERR(moduleName, "Failed to open file for reading: %s", path);
    return false;
  }
  return true;
}

bool openSdMmcForWrite(const char* moduleName, const char* path, fs::File& file) {
  file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    LOG_ERR(moduleName, "Failed to open file for writing: %s", path);
    return false;
  }
  return true;
}
#endif

}  // namespace

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  if (!useSdMmcBackend()) {
    initialized = SDCard.begin();
    return initialized;
  }

#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  const auto& board = activeBoardProfile();
  if (board.sdEnablePin >= 0) {
    pinMode(board.sdEnablePin, OUTPUT);
    digitalWrite(board.sdEnablePin, board.sdEnableActiveLow ? LOW : HIGH);
    delay(150);
  }

  const bool pinsOk = board.sdMmc4Bit
                          ? SD_MMC.setPins(board.sdClkPin, board.sdCmdPin, board.sdD0Pin, board.sdD1Pin, board.sdD2Pin,
                                           board.sdD3Pin)
                          : SD_MMC.setPins(board.sdClkPin, board.sdCmdPin, board.sdD0Pin);
  if (!pinsOk) {
    LOG_ERR("STOR", "SD_MMC.setPins failed");
    initialized = false;
    return false;
  }

  // EPUB cache finalization can hold book.bin plus several temporary index files
  // open at once. The SD_MMC VFS default is too small for that workload.
  initialized = SD_MMC.begin("/sdcard", !board.sdMmc4Bit, false, SDMMC_FREQ_DEFAULT, 12);
  LOG_INF("STOR", "SD_MMC begin: ok=%d mode=%s cardType=%d cardSize=%llu", initialized,
          board.sdMmc4Bit ? "4-bit" : "1-bit", static_cast<int>(SD_MMC.cardType()),
          static_cast<unsigned long long>(SD_MMC.cardSize()));
  return initialized;
#else
  initialized = false;
  return false;
#endif
}

bool HalStorage::ready() const { return useSdMmcBackend() ? initialized : SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.listFiles(path, maxFiles);

  std::vector<String> ret;
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  if (!initialized) return ret;

  fs::File root = SD_MMC.open(path, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return ret;
  }

  int count = 0;
  for (fs::File file = root.openNextFile(); file && count < maxFiles; file = root.openNextFile()) {
    if (!file.isDirectory()) {
      ret.emplace_back(file.name());
      count++;
    }
    file.close();
  }
  root.close();
#endif
  return ret;
}

String HalStorage::readFile(const char* path) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.readFile(path);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  if (!initialized) return {""};

  fs::File f;
  if (!openSdMmcForRead("SD", path, f)) return {""};

  String content = "";
  constexpr size_t maxSize = 50000;
  size_t readSize = 0;
  while (f.available() && readSize < maxSize) {
    content += static_cast<char>(f.read());
    readSize++;
  }
  f.close();
  return content;
#else
  return {""};
#endif
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.readFileToStream(path, out, chunkSize);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  if (!initialized) return false;

  fs::File f;
  if (!openSdMmcForRead("SD", path, f)) return false;

  constexpr size_t localBufSize = 256;
  uint8_t buf[localBufSize];
  const size_t toRead = (chunkSize == 0) ? localBufSize : (chunkSize < localBufSize ? chunkSize : localBufSize);
  while (f.available()) {
    const int r = f.read(buf, toRead);
    if (r > 0) {
      out.write(buf, static_cast<size_t>(r));
    } else {
      break;
    }
  }
  f.close();
  return true;
#else
  return false;
#endif
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.readFileToBuffer(path, buffer, bufferSize, maxBytes);
  if (!buffer || bufferSize == 0) return 0;
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  if (!initialized) {
    buffer[0] = '\0';
    return 0;
  }

  fs::File f;
  if (!openSdMmcForRead("SD", path, f)) {
    buffer[0] = '\0';
    return 0;
  }

  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : min(maxBytes, bufferSize - 1);
  size_t total = 0;
  while (f.available() && total < maxToRead) {
    constexpr size_t chunk = 64;
    const size_t want = maxToRead - total;
    const size_t readLen = (want < chunk) ? want : chunk;
    const int r = f.read(reinterpret_cast<uint8_t*>(buffer + total), readLen);
    if (r > 0) {
      total += static_cast<size_t>(r);
    } else {
      break;
    }
  }

  buffer[total] = '\0';
  f.close();
  return total;
#else
  buffer[0] = '\0';
  return 0;
#endif
}

bool HalStorage::writeFile(const char* path, const String& content) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.writeFile(path, content);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  if (!initialized) return false;

  if (SD_MMC.exists(path)) SD_MMC.remove(path);
  fs::File f;
  if (!openSdMmcForWrite("SD", path, f)) return false;
  const size_t written = f.print(content);
  f.close();
  return written == content.length();
#else
  return false;
#endif
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.ensureDirectoryExists(path);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  if (!initialized) return false;

  if (SD_MMC.exists(path)) {
    fs::File dir = SD_MMC.open(path, FILE_READ);
    const bool ok = dir && dir.isDirectory();
    if (dir) dir.close();
    return ok;
  }
  return SD_MMC.mkdir(path);
#else
  return false;
#endif
}

class HalFile::Impl {
 public:
  enum class Backend : uint8_t { SdFat, SdMmc };

  Impl(FsFile&& fsFile) : backend(Backend::SdFat), fsFile(std::move(fsFile)) {}
  Impl(fs::File&& mmcFile, const char* path) : backend(Backend::SdMmc), mmcFile(std::move(mmcFile)), path(path ? path : "") {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock.
  ~Impl() {
    HalStorage::StorageLock lock;
    close();
  }

  void flush() {
    if (backend == Backend::SdFat) {
      fsFile.flush();
    } else {
      mmcFile.flush();
    }
  }

  size_t getName(char* name, size_t len) {
    if (backend == Backend::SdFat) return fsFile.getName(name, len);
    if (!name || len == 0) return 0;
    const char* source = mmcFile.name();
    if (!source || source[0] == '\0') source = path.c_str();
    const size_t copyLen = strnlen(source, len - 1);
    memcpy(name, source, copyLen);
    name[copyLen] = '\0';
    return copyLen;
  }

  size_t size() { return backend == Backend::SdFat ? fsFile.size() : static_cast<size_t>(mmcFile.size()); }
  uint64_t fileSize64() { return backend == Backend::SdFat ? fsFile.fileSize() : mmcFile.size(); }

  bool seekSet(uint64_t pos) {
    if (backend == Backend::SdFat) return fsFile.seekSet(pos);
    return mmcFile.seek(pos, SeekSet);
  }

  bool seekCur(int64_t offset) {
    if (backend == Backend::SdFat) return fsFile.seekCur(offset);
    const int64_t next = static_cast<int64_t>(mmcFile.position()) + offset;
    if (next < 0) return false;
    return mmcFile.seek(static_cast<uint32_t>(next), SeekSet);
  }

  int available() { return backend == Backend::SdFat ? fsFile.available() : mmcFile.available(); }
  size_t position() { return backend == Backend::SdFat ? fsFile.position() : mmcFile.position(); }

  int read(void* buf, size_t count) {
    if (backend == Backend::SdFat) return fsFile.read(buf, count);
    return mmcFile.read(reinterpret_cast<uint8_t*>(buf), count);
  }

  int read() { return backend == Backend::SdFat ? fsFile.read() : mmcFile.read(); }

  size_t write(const void* buf, size_t count) {
    if (backend == Backend::SdFat) return fsFile.write(buf, count);
    return mmcFile.write(reinterpret_cast<const uint8_t*>(buf), count);
  }

  size_t write(uint8_t b) {
    if (backend == Backend::SdFat) return fsFile.write(b);
    return mmcFile.write(b);
  }

  bool rename(const char* newPath) {
    if (backend == Backend::SdFat) return fsFile.rename(newPath);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
    if (path.empty() || !newPath) return false;
    flush();
    close();
    const bool ok = SD_MMC.rename(path.c_str(), newPath);
    if (ok) path = newPath;
    return ok;
#else
    return false;
#endif
  }

  bool isDirectory() { return backend == Backend::SdFat ? fsFile.isDirectory() : mmcFile.isDirectory(); }
  void rewindDirectory() {
    if (backend == Backend::SdFat) {
      fsFile.rewindDirectory();
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
    } else if (!path.empty()) {
      mmcFile.close();
      mmcFile = SD_MMC.open(path.c_str(), FILE_READ);
#endif
    }
  }

  bool close() {
    if (backend == Backend::SdFat) return fsFile.close();
    mmcFile.close();
    return true;
  }

  HalFile openNextFile() {
    if (backend == Backend::SdFat) return HalFile(std::make_unique<Impl>(fsFile.openNextFile()));
    fs::File next = mmcFile.openNextFile();
    return HalFile(std::make_unique<Impl>(std::move(next), next ? next.path() : ""));
  }

  bool isOpen() const { return backend == Backend::SdFat ? fsFile.isOpen() : static_cast<bool>(mmcFile); }

  Backend backend;
  FsFile fsFile;
  fs::File mmcFile;
  std::string path;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  if (!useSdMmcBackend()) return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  return HalFile(std::make_unique<HalFile::Impl>(SD_MMC.open(path, modeForOflag(oflag)), path));
#else
  return HalFile();
#endif
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.mkdir(path, pFlag);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  return SD_MMC.mkdir(path);
#else
  return false;
#endif
}

bool HalStorage::exists(const char* path) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.exists(path);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  return SD_MMC.exists(path);
#else
  return false;
#endif
}

bool HalStorage::remove(const char* path) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.remove(path);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  return SD_MMC.remove(path);
#else
  return false;
#endif
}
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.rename(oldPath, newPath);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  return SD_MMC.rename(oldPath, newPath);
#else
  return false;
#endif
}

bool HalStorage::rmdir(const char* path) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.rmdir(path);
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  return SD_MMC.rmdir(path);
#else
  return false;
#endif
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  if (useSdMmcBackend()) {
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
    fs::File mmcFile;
    const bool ok = openSdMmcForRead(moduleName, path, mmcFile);
    file = HalFile(std::make_unique<HalFile::Impl>(std::move(mmcFile), path));
    return ok;
#else
    file = HalFile();
    return false;
#endif
  }

  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  if (useSdMmcBackend()) {
#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
    fs::File mmcFile;
    const bool ok = openSdMmcForWrite(moduleName, path, mmcFile);
    file = HalFile(std::make_unique<HalFile::Impl>(std::move(mmcFile), path));
    return ok;
#else
    file = HalFile();
    return false;
#endif
  }

  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) {
  StorageLock lock;
  if (!useSdMmcBackend()) return SDCard.removeDir(path);

#ifdef CROSSPOINT_HAS_SD_MMC_BACKEND
  fs::File dir = SD_MMC.open(path, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  for (fs::File file = dir.openNextFile(); file; file = dir.openNextFile()) {
    String filePath = path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += file.name();

    const bool ok = file.isDirectory() ? removeDir(filePath.c_str()) : SD_MMC.remove(filePath.c_str());
    file.close();
    if (!ok) {
      dir.close();
      return false;
    }
  }
  dir.close();
  return SD_MMC.rmdir(path);
#else
  return false;
#endif
}

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

void HalFile::flush() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  impl->flush();
}
size_t HalFile::getName(char* name, size_t len) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->getName(name, len);
}
size_t HalFile::size() {
  assert(impl != nullptr);
  return impl->size();
}
size_t HalFile::fileSize() {
  assert(impl != nullptr);
  return impl->size();
}
uint64_t HalFile::fileSize64() {
  assert(impl != nullptr);
  return impl->fileSize64();
}
bool HalFile::seek(size_t pos) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekSet(pos);
}
bool HalFile::seek64(uint64_t pos) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekSet(pos);
}
bool HalFile::seekCur(int64_t offset) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekCur(offset);
}
bool HalFile::seekSet(size_t offset) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->seekSet(offset);
}
int HalFile::available() const {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->available();
}
size_t HalFile::position() const {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->position();
}
int HalFile::read(void* buf, size_t count) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->read(buf, count);
}
int HalFile::read() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->read();
}
size_t HalFile::write(const void* buf, size_t count) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->write(buf, count);
}
size_t HalFile::write(const uint8_t* buf, size_t count) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->write(buf, count);
}
size_t HalFile::write(uint8_t b) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->write(b);
}
bool HalFile::rename(const char* newPath) {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->rename(newPath);
}
bool HalFile::isDirectory() const {
  assert(impl != nullptr);
  return impl->isDirectory();
}
void HalFile::rewindDirectory() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  impl->rewindDirectory();
}
bool HalFile::close() {
  HalStorage::StorageLock lock;
  if (impl == nullptr) {
    return false;
  }
  return impl->close();
}
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return impl->openNextFile();
}
bool HalFile::isOpen() const { return impl != nullptr && impl->isOpen(); }
HalFile::operator bool() const { return isOpen(); }

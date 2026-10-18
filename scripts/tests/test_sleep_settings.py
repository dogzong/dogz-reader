"""Exercise real sleep JSON persistence against short-write and invalid-cache cases."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SleepSettingsTest(unittest.TestCase):
    def test_settings_validation_and_atomic_save(self):
        deps = list(ROOT.glob('.pio/libdeps/*/ArduinoJson/src/ArduinoJson.h'))
        self.assertTrue(deps, 'Build a firmware environment to provide ArduinoJson')
        with tempfile.TemporaryDirectory(prefix='sleep-settings-') as temp:
            work = Path(temp)
            (work / 'Arduino.h').write_text('#pragma once\n#include <string>\nusing String = std::string;\n')
            (work / 'Logging.h').write_text('''#pragma once
struct Log { void println(const char*) {} }; inline Log logSerial;
''')
            (work / 'HalStorage.h').write_text('''#pragma once
#include <map>
#include <string>
#include <cstdint>
constexpr int O_WRITE=1, O_CREAT=2, O_TRUNC=4;
struct HalFile {
  std::string* data = nullptr; bool shortWrite = false;
  explicit operator bool() const { return data; }
  size_t write(uint8_t byte) { if (shortWrite) return 0; data->push_back(byte); return 1; }
  size_t write(const uint8_t* bytes, size_t count) {
    if (shortWrite) return 0; data->append(reinterpret_cast<const char*>(bytes), count); return count;
  }
  void flush() {}
};
struct FakeStorage {
  std::map<std::string, std::string> files;
  bool shortWrite = false, failReplace = false;
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    auto it=files.find(path); if(it==files.end() || it->second.empty() || it->second.size()>cap) return false;
    out=it->second; return true;
  }
  bool ensureDirectoryExists(const char*) { return true; }
  HalFile open(const char* path, int) { files[path].clear(); return {&files[path], shortWrite}; }
  bool replaceFile(const char* part, const char* path) {
    if(failReplace) return false; files[path]=files[part]; files.erase(part); return true;
  }
}; inline FakeStorage Storage;
''')
            (work / 'test.cpp').write_text('''#include <cassert>
#include <cstring>
#include <HalStorage.h>
#include "SleepSettings.h"
int main() {
  constexpr const char* path="/.crosspoint/sleep/settings.json";
  sleepscreen::Settings original; original.mode=1; original.crop=true;
  strcpy(original.image,"/Pictures/Sleep/landscape.bmp");
  assert(original.save()); const auto saved=Storage.files[path];
  sleepscreen::Settings loaded; assert(loaded.load());
  assert(loaded.mode==1 && loaded.crop && !strcmp(loaded.image,original.image));
  Storage.shortWrite=true; original.mode=0; assert(!original.save()); assert(Storage.files[path]==saved);
  Storage.shortWrite=false; Storage.failReplace=true; assert(!original.save()); assert(Storage.files[path]==saved);
  Storage.failReplace=false;
  for(const char* invalid : {"{\\"version\\":2}", "{\\"version\\":1,\\"mode\\":8}",
                           "{\\"version\\":1,\\"image\\":\\"../sleep.bmp\\"}", "broken json"}) {
    Storage.files[path]=invalid; assert(!loaded.load()); assert(loaded.mode==1);
  }
  Storage.files[path]="{\\"version\\":1,\\"modules\\":511}";
  assert(loaded.load()); assert(loaded.modules==sleepscreen::kModules); // No quota/clock/lunar modules.
  assert(loaded.mode==0 && !loaded.image[0]);
  Storage.files[path]=std::string(1025,'x'); assert(!loaded.load());
  original.mode=2; assert(!original.save());
}
''')
            binary = work / 'test'
            subprocess.run(['c++', '-std=c++17', '-I'+str(work), '-I'+str(deps[0].parent),
                            '-I'+str(ROOT / 'src/activities/boot_sleep'), str(work / 'test.cpp'),
                            str(ROOT / 'src/activities/boot_sleep/SleepSettings.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

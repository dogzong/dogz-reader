#!/usr/bin/env python3
"""Exercise production logical-key mapping with synthetic physical key snapshots."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'src/MappedInputManager.cpp').read_text()
start = source.index('bool MappedInputManager::mapButton(')
production = source[start:source.index('\n}\n', start) + 3]
stubs = r'''
#include <cassert>
#include <cstdint>
struct HalGPIO {
  enum { BTN_UP = 2, BTN_DOWN = 3, BTN_POWER = 4 };
  unsigned pressed = 0;
  bool wasPressed(uint8_t n) const { return pressed & (1u << n); }
};
struct CrossPointSettings {
  enum { PREV_NEXT, NEXT_PREV, PREV_PREV, NEXT_NEXT, SIDE_BUTTONS_DISABLED };
};
struct Settings {
  int sideButtonLayout = CrossPointSettings::PREV_NEXT;
  uint8_t frontButtonBack = 5, frontButtonConfirm = 6;
  uint8_t frontButtonLeft = 0, frontButtonRight = 1;
} SETTINGS;
class MappedInputManager {
public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward,
                      NavNext, NavPrevious, ScreenLeft, ScreenRight, ScreenUp, ScreenDown, Count };
  HalGPIO gpio;
  bool swapped = false;
  bool isNavDirectionSwapped() const { return swapped; }
  Button mapScreenDirection(Button b) const { return b; }
  bool mapButton(Button, bool (HalGPIO::*)(uint8_t) const) const;
};
'''
tests = r'''
int main() {
  MappedInputManager input;
  using B = MappedInputManager::Button;
  auto down = [&](B b) { return input.mapButton(b, &HalGPIO::wasPressed); };
  input.gpio.pressed = 1u << SETTINGS.frontButtonLeft;
  assert(!down(B::Up)); // Regression: the old audio-page key query misses this key.
  assert(down(B::NavPrevious)); assert(!down(B::NavNext));
  input.gpio.pressed = 1u << SETTINGS.frontButtonRight;
  assert(!down(B::Down)); assert(down(B::NavNext)); assert(!down(B::NavPrevious));
  input.gpio.pressed = 1u << HalGPIO::BTN_UP;
  assert(down(B::NavPrevious)); assert(!down(B::NavNext));
  input.gpio.pressed = 1u << HalGPIO::BTN_DOWN;
  assert(down(B::NavNext)); assert(!down(B::NavPrevious));
  input.swapped = true;
  assert(down(B::NavPrevious)); assert(!down(B::NavNext));
  input.gpio.pressed = 0;
  assert(!down(B::NavNext)); assert(!down(B::NavPrevious));
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'navigation.cpp'
    binary = Path(directory) / 'navigation'
    cpp.write_text(stubs + production + tests)
    subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: production navigation handles front keys, side keys, orientation and release')

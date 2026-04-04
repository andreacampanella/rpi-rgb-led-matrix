"""Keyboard input via evdev."""

import evdev
from evdev import ecodes

KEY_MAP = {
    ecodes.KEY_UP: "UP",
    ecodes.KEY_DOWN: "DOWN",
    ecodes.KEY_LEFT: "LEFT",
    ecodes.KEY_RIGHT: "RIGHT",
    ecodes.KEY_ENTER: "ENTER",
    ecodes.KEY_SPACE: "ENTER",
    ecodes.KEY_BACKSPACE: "BACK",
    ecodes.KEY_ESC: "BACK",
}


def find_keyboard():
    """Return the first input device that looks like a keyboard."""
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        caps = dev.capabilities().get(ecodes.EV_KEY, [])
        if ecodes.KEY_ENTER in caps and ecodes.KEY_UP in caps:
            return dev
    # Fallback: first device with any keys
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        if ecodes.EV_KEY in dev.capabilities():
            return dev
    return None


class InputHandler:
    def __init__(self, device_path=None):
        if device_path:
            self.device = evdev.InputDevice(device_path)
        else:
            self.device = find_keyboard()
        if not self.device:
            raise RuntimeError(
                "No keyboard found. List devices: python3 -m evdev.evtest"
            )
        print(f"Using input device: {self.device.name} ({self.device.path})")
        self.device.grab()

    def read_key(self):
        """Blocking read. Returns a mapped key name or None."""
        for event in self.device.read_loop():
            if event.type == ecodes.EV_KEY and event.value == 1:  # key-down
                mapped = KEY_MAP.get(event.code)
                if mapped:
                    return mapped

    def close(self):
        try:
            self.device.ungrab()
        except OSError:
            pass
        self.device.close()

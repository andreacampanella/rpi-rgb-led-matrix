#!/usr/bin/env python3
"""HUB75 LED Matrix Menu — keyboard-driven menu with auto-discovered plugins."""

import os
import sys
import importlib
import pkgutil

# Ensure this directory is importable
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from plugin_base import PluginBase
from display import Display
from input_handler import InputHandler


def load_config(path):
    cfg = {}
    if os.path.isfile(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip()
    return cfg


def discover_plugins():
    """Scan plugins/ for PluginBase subclasses."""
    plugins_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "plugins")
    found = []

    for finder, name, _ in pkgutil.iter_modules([plugins_dir]):
        module = importlib.import_module(f"plugins.{name}")
        for attr_name in dir(module):
            attr = getattr(module, attr_name)
            if (
                isinstance(attr, type)
                and issubclass(attr, PluginBase)
                and attr is not PluginBase
            ):
                found.append(attr)

    found.sort(key=lambda p: (p.order, p.name))
    return found


def main():
    config = load_config(os.path.expanduser("~/.hub75_bot.conf"))

    display = Display(
        width=int(config.get("DISPLAY_WIDTH", "64")),
        height=int(config.get("DISPLAY_HEIGHT", "64")),
        ft_host=config.get("FT_HOST", "127.0.0.1"),
    )

    kbd = InputHandler(device_path=config.get("INPUT_DEVICE"))

    ctx = {"display": display, "config": config}

    plugin_classes = discover_plugins()
    if not plugin_classes:
        print("No plugins found in plugins/")
        return

    items = [p.name for p in plugin_classes]
    selected = 0
    active_plugin = None

    try:
        display.render_menu("Main Menu", items, selected)

        while True:
            key = kbd.read_key()
            if key is None:
                continue

            if active_plugin:
                if active_plugin.handle_key(key) is False:
                    active_plugin.on_exit()
                    active_plugin = None
                    display.render_menu("Main Menu", items, selected)
            else:
                if key == "UP":
                    selected = (selected - 1) % len(items)
                    display.render_menu("Main Menu", items, selected)
                elif key == "DOWN":
                    selected = (selected + 1) % len(items)
                    display.render_menu("Main Menu", items, selected)
                elif key == "ENTER":
                    active_plugin = plugin_classes[selected](ctx)
                    active_plugin.on_enter()

    except KeyboardInterrupt:
        pass
    finally:
        display.kill()
        display.clear()
        kbd.close()
        print("Menu exited.")


if __name__ == "__main__":
    main()

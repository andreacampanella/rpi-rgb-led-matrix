"""Browse images/GIFs in the media folder. Enter opens a delete sub-menu."""

import os

from plugin_base import PluginBase


class GifBrowser(PluginBase):
    name = "GIF Browser"
    order = 10

    def __init__(self, ctx):
        super().__init__(ctx)
        self.gif_dir = os.path.expanduser(self.config.get("GIF_DIR", "~/gifs"))
        self.files = []
        self.selected = 0
        self.in_submenu = False
        self.submenu_sel = 0

    # ------------------------------------------------------------------
    def _scan(self):
        exts = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".mp4"}
        self.files = sorted(
            f
            for f in os.listdir(self.gif_dir)
            if os.path.isfile(os.path.join(self.gif_dir, f))
            and os.path.splitext(f)[1].lower() in exts
        )
        if self.selected >= len(self.files):
            self.selected = max(0, len(self.files) - 1)

    def _show_current(self):
        if not self.files:
            self.display.render_menu("GIF Browser", ["(empty)"], 0)
            return
        path = os.path.join(self.gif_dir, self.files[self.selected])
        self.display.send_image(path)

    def _show_submenu(self):
        name = self.files[self.selected]
        # Truncate long filenames
        label = name if len(name) <= 14 else name[:11] + "..."
        self.display.render_menu(label, ["Delete", "Cancel"], self.submenu_sel)

    # ------------------------------------------------------------------
    def on_enter(self):
        self._scan()
        self._show_current()

    def on_exit(self):
        self.display.kill()

    # ------------------------------------------------------------------
    def handle_key(self, key):
        if self.in_submenu:
            return self._key_submenu(key)

        if not self.files:
            return key != "BACK"  # BACK exits, anything else stays

        if key in ("LEFT", "UP"):
            self.selected = (self.selected - 1) % len(self.files)
            self._show_current()
        elif key in ("RIGHT", "DOWN"):
            self.selected = (self.selected + 1) % len(self.files)
            self._show_current()
        elif key == "ENTER":
            self.in_submenu = True
            self.submenu_sel = 1  # default to Cancel
            self._show_submenu()
        elif key == "BACK":
            return False
        return True

    def _key_submenu(self, key):
        if key in ("UP", "DOWN"):
            self.submenu_sel = 1 - self.submenu_sel
            self._show_submenu()
        elif key == "ENTER":
            if self.submenu_sel == 0:  # Delete
                path = os.path.join(self.gif_dir, self.files[self.selected])
                os.remove(path)
                self._scan()
            self.in_submenu = False
            self._show_current()
        elif key == "BACK":
            self.in_submenu = False
            self._show_current()
        return True

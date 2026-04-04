"""Display helper: renders to the LED matrix via send-image / send-video."""

import os
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont


class Display:
    def __init__(self, width=64, height=64, ft_host="127.0.0.1", bin_dir="~/bin"):
        self.width = width
        self.height = height
        self.ft_host = ft_host
        self.bin_dir = os.path.expanduser(bin_dir)
        self._proc = None
        self._tmp = os.path.join(tempfile.gettempdir(), "hub75_menu_frame.png")

        # Try to load a small font
        try:
            self._font = ImageFont.load_default(size=8)
        except TypeError:
            self._font = ImageFont.load_default()

    def _bin(self, name):
        local = os.path.join(self.bin_dir, name)
        return local if os.path.isfile(local) else name

    def kill(self):
        """Kill any running send-image / send-video process."""
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._proc.kill()
        self._proc = None

    def send_image(self, filepath, layer=0, timeout=None):
        """Send a still image or animated GIF to the display."""
        self.kill()
        cmd = [
            self._bin("send-image"),
            "-h", self.ft_host,
            "-g", f"{self.width}x{self.height}+0+0+{layer}",
        ]
        if timeout is not None:
            cmd += ["-t", str(timeout)]
        cmd.append(filepath)
        self._proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )

    def send_video(self, source, layer=0):
        """Stream a video source (camera device, file, URL) to the display."""
        self.kill()
        cmd = [
            self._bin("send-video"),
            "-h", self.ft_host,
            "-g", f"{self.width}x{self.height}+0+0+{layer}",
            source,
        ]
        self._proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )

    def render_menu(self, title, items, selected, layer=0):
        """Render a text menu as a PNG and send it to the display.

        Args:
            title:    header string
            items:    list of menu item strings
            selected: index of the highlighted item
            layer:    ft layer
        """
        img = Image.new("RGB", (self.width, self.height), (0, 0, 0))
        draw = ImageDraw.Draw(img)
        font = self._font

        y = 1
        # Title
        draw.text((2, y), title, fill=(255, 255, 0), font=font)
        y += 10
        draw.line([(0, y), (self.width, y)], fill=(80, 80, 80))
        y += 2

        # Items
        for i, item in enumerate(items):
            if y > self.height - 9:
                draw.text((2, y), "...", fill=(128, 128, 128), font=font)
                break
            if i == selected:
                draw.rectangle([(0, y), (self.width, y + 9)], fill=(40, 40, 80))
                draw.text((2, y), f"> {item}", fill=(0, 255, 0), font=font)
            else:
                draw.text((2, y), f"  {item}", fill=(180, 180, 180), font=font)
            y += 10

        img.save(self._tmp)
        self.send_image(self._tmp, layer=layer)

    def render_message(self, lines, layer=0):
        """Render a simple message (list of strings) centered on screen."""
        img = Image.new("RGB", (self.width, self.height), (0, 0, 0))
        draw = ImageDraw.Draw(img)
        font = self._font

        total_h = len(lines) * 10
        y = max(0, (self.height - total_h) // 2)

        for line in lines:
            draw.text((2, y), line, fill=(255, 255, 255), font=font)
            y += 10

        img.save(self._tmp)
        self.send_image(self._tmp, layer=layer)

    def clear(self, layer=0):
        """Send a black frame to clear the display."""
        img = Image.new("RGB", (self.width, self.height), (0, 0, 0))
        img.save(self._tmp)
        self.send_image(self._tmp, layer=layer)

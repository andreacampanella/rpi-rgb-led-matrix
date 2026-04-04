"""Live camera preview. Press Enter to capture a frame with a flash effect."""

import os
import subprocess
import time

from PIL import Image, ImageDraw

from plugin_base import PluginBase


class CameraPreview(PluginBase):
    name = "Camera"
    order = 20

    def __init__(self, ctx):
        super().__init__(ctx)
        self.camera = self.config.get("CAMERA_DEVICE", "/dev/video0")
        self.photos_dir = os.path.expanduser(self.config.get("PHOTOS_DIR", "~/photos"))
        os.makedirs(self.photos_dir, exist_ok=True)
        self._previewing = False

    def _start_preview(self):
        if not os.path.exists(self.camera):
            self.display.render_message(["No camera", self.camera])
            self._previewing = False
            return
        self.display.send_video(self.camera)
        self._previewing = True

    def _flash(self):
        """Send a white-border frame to simulate a camera flash."""
        w, h = self.display.width, self.display.height
        img = Image.new("RGB", (w, h), (255, 255, 255))
        draw = ImageDraw.Draw(img)
        # Black center, leaving a white border
        b = 3  # border thickness
        draw.rectangle([(b, b), (w - b - 1, h - b - 1)], fill=(0, 0, 0))
        tmp = "/tmp/hub75_flash.png"
        img.save(tmp)
        self.display.kill()
        self.display.send_image(tmp)
        time.sleep(0.15)

    def _capture(self):
        """Grab one frame, scale to display size, save to photos folder."""
        ts = int(time.time())
        outpath = os.path.join(self.photos_dir, f"capture_{ts}.png")
        try:
            subprocess.run(
                [
                    "ffmpeg", "-y",
                    "-f", "v4l2",
                    "-i", self.camera,
                    "-vframes", "1",
                    "-vf", f"scale=64:64,scale=512:512:flags=neighbor",
                    outpath,
                ],
                capture_output=True,
                timeout=10,
            )
            if os.path.isfile(outpath):
                return outpath
        except Exception:
            pass
        return None

    # ------------------------------------------------------------------
    def on_enter(self):
        self._start_preview()

    def on_exit(self):
        self.display.kill()
        self._previewing = False

    def handle_key(self, key):
        if key == "BACK":
            return False

        if key == "ENTER":
            # Flash → capture → show result briefly → resume preview
            self._flash()
            path = self._capture()
            if path:
                self.display.send_image(path)
                time.sleep(1)
            else:
                self.display.render_message(["Capture failed"])
                time.sleep(1)
            self._start_preview()
            return True

        # Any other key: resume preview if stopped
        if not self._previewing:
            self._start_preview()
        return True

#!/usr/bin/env python3
"""HUB75 Telegram Bot — receives images/GIFs and cycles them on an LED matrix via ft-server."""

import os
import asyncio
import logging

from telegram import Update
from telegram.ext import ApplicationBuilder, MessageHandler, CommandHandler, filters, ContextTypes

# --- Config ---
CONF_PATH = os.path.expanduser("~/.hub75_bot.conf")


def load_config(path):
    """Parse key=value config file."""
    cfg = {}
    if not os.path.isfile(path):
        raise SystemExit(f"Config file not found: {path}")
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    return cfg


config = load_config(CONF_PATH)

BOT_TOKEN = config.get("BOT_TOKEN", "")
if not BOT_TOKEN or BOT_TOKEN == "YOUR_TELEGRAM_BOT_TOKEN_HERE":
    raise SystemExit(f"ERROR: Set BOT_TOKEN in {CONF_PATH}")

SAVE_FOLDER = os.path.expanduser(config.get("GIF_DIR", "~/gifs"))
DISPLAY_W = config.get("DISPLAY_WIDTH", "64")
DISPLAY_H = config.get("DISPLAY_HEIGHT", "64")
GEOMETRY = f"{DISPLAY_W}x{DISPLAY_H}"
DISPLAY_TIME = config.get("DISPLAY_TIME", "6")
REPEAT_EACH_IMAGE = config.get("REPEAT_EACH_IMAGE", "false").lower() == "true"
FT_HOST = config.get("FT_HOST", "127.0.0.1")

os.makedirs(SAVE_FOLDER, exist_ok=True)

# --- Logging ---
logging.basicConfig(format="[%(asctime)s] %(levelname)s: %(message)s", level=logging.INFO)

# --- Resolve binaries ---
BIN_DIR = os.path.expanduser("~/bin")


def find_bin(name):
    local = os.path.join(BIN_DIR, name)
    if os.path.isfile(local) and os.access(local, os.X_OK):
        return local
    return name


SEND_IMAGE = find_bin("send-image")
SEND_VIDEO = find_bin("send-video")

# --- Global task reference ---
send_video_task = None

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"}


# --- Media upload handler ---
async def handle_media(update: Update, context: ContextTypes.DEFAULT_TYPE):
    file = None
    ext = None

    if update.message.photo:
        media = update.message.photo[-1]
        ext = ".jpg"
        file = await context.bot.get_file(media.file_id)
    elif update.message.animation:
        media = update.message.animation
        ext = ".mp4"
        file = await context.bot.get_file(media.file_id)
    elif update.message.document:
        mime = update.message.document.mime_type or ""
        if mime.startswith("image/"):
            media = update.message.document
            ext = os.path.splitext(media.file_name or "file.bin")[1] or ".bin"
            file = await context.bot.get_file(media.file_id)
        elif mime.startswith("video/"):
            media = update.message.document
            ext = os.path.splitext(media.file_name or "file.mp4")[1] or ".mp4"
            file = await context.bot.get_file(media.file_id)

    if file and ext:
        filename = os.path.join(SAVE_FOLDER, f"{media.file_id}{ext}")
        await file.download_to_drive(filename)
        logging.info(f"Saved: {filename}")
        await update.message.reply_text(f"Saved: {os.path.basename(filename)}")
    else:
        await update.message.reply_text("Unsupported media type.")


# --- Background image loop ---
async def send_images_loop():
    logging.info("Display loop started")
    while True:
        files = sorted(
            f
            for f in os.listdir(SAVE_FOLDER)
            if os.path.isfile(os.path.join(SAVE_FOLDER, f))
        )
        if not files:
            await asyncio.sleep(2)
            continue
        for fname in files:
            filepath = os.path.join(SAVE_FOLDER, fname)
            ext = os.path.splitext(fname)[1].lower()
            cmd = SEND_IMAGE if ext in IMAGE_EXTS else SEND_VIDEO
            repeat_count = 2 if REPEAT_EACH_IMAGE else 1
            for _ in range(repeat_count):
                proc = await asyncio.create_subprocess_exec(
                    cmd,
                    "-h", FT_HOST,
                    "-t", DISPLAY_TIME,
                    "-g", GEOMETRY,
                    filepath,
                    stdout=asyncio.subprocess.DEVNULL,
                    stderr=asyncio.subprocess.DEVNULL,
                )
                await proc.wait()
        await asyncio.sleep(1)


# --- Commands ---
async def start_command(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global send_video_task
    if send_video_task and not send_video_task.done():
        await update.message.reply_text("Loop already running.")
        return
    send_video_task = asyncio.create_task(send_images_loop())
    await update.message.reply_text("Loop started.")


async def stop_command(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global send_video_task
    if send_video_task and not send_video_task.done():
        send_video_task.cancel()
        try:
            await send_video_task
        except asyncio.CancelledError:
            pass
        send_video_task = None
        await update.message.reply_text("Loop stopped.")
    else:
        await update.message.reply_text("Loop is not running.")


async def status_command(update: Update, context: ContextTypes.DEFAULT_TYPE):
    n_files = len(
        [f for f in os.listdir(SAVE_FOLDER) if os.path.isfile(os.path.join(SAVE_FOLDER, f))]
    )
    running = send_video_task and not send_video_task.done()
    await update.message.reply_text(
        f"Loop: {'running' if running else 'stopped'}\n"
        f"Files in queue: {n_files}\n"
        f"Geometry: {GEOMETRY}\n"
        f"Display time: {DISPLAY_TIME}s"
    )


async def clear_command(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Delete all files in the GIF folder."""
    count = 0
    for f in os.listdir(SAVE_FOLDER):
        fp = os.path.join(SAVE_FOLDER, f)
        if os.path.isfile(fp):
            os.remove(fp)
            count += 1
    await update.message.reply_text(f"Cleared {count} file(s).")


async def help_command(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await update.message.reply_text(
        "/start  — Start display loop\n"
        "/stop   — Stop display loop\n"
        "/status — Show current status\n"
        "/clear  — Delete all queued media\n"
        "/help   — Show this message\n\n"
        "Send any image, GIF, or video to add it to the queue."
    )


# --- Bot setup ---
app = ApplicationBuilder().token(BOT_TOKEN).build()
app.add_handler(CommandHandler("start", start_command))
app.add_handler(CommandHandler("stop", stop_command))
app.add_handler(CommandHandler("status", status_command))
app.add_handler(CommandHandler("clear", clear_command))
app.add_handler(CommandHandler("help", help_command))
app.add_handler(
    MessageHandler(
        filters.PHOTO | filters.ANIMATION | filters.Document.IMAGE | filters.VIDEO,
        handle_media,
    )
)

logging.info(f"HUB75 bot starting (geometry={GEOMETRY}, gif_dir={SAVE_FOLDER})")
app.run_polling()

# swaylock-slots

Ever wanted to gamble whilst unlocking your computer?

This is a personal fork of the awesome [swaylock](https://github.com/swaywm/swaylock) that adds slot machine mechanics to your lock screen.

## Demo
<img width="3430" height="1934" alt="image" src="https://github.com/user-attachments/assets/4ca77f43-f9bd-4705-b681-60e3f398167c" />
[demo.mp4](./demo.mp4)

## How it works

The ring indicator is replaced by three slot reels.

- Each keypress rolls 🍒 ⭐ 💎 🍌
- Reels spin while the password is verified
- Wrong password: ❌ ❌ ❌

Reels are random and never depend on what you type. Authentication code (PAM, shadow, comm) is unmodified upstream swaylock.

## Installation

Build from source using meson:

```bash
meson build
ninja -C build
sudo ninja -C build install
```

## Original swaylock

This is a fork of [swaylock](https://github.com/swaywm/swaylock) - a screen locking utility for Wayland compositors.

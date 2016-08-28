# v4l2-decode

This is a forked version of [v4l2-decode][v4l2-decode.git], which can decode
video using the MSM V4L2 driver and ION allocations. Decoded frames are
displayed using [wayland][], via the [linux-dmabuf][] protocol.

Dependencies:

* linux-msm-3.18 kernel headers
* [wayland-client][wayland.git] library
* [wayland-protocols][wayland-protocols.git] files
* [ffmpeg 3.1][ffmpeg]

[ffmpeg]: http://www.ffmpeg.org
[wayland]: http://wayland.freedesktop.org
[wayland.git]: https://cgit.freedesktop.org/wayland/wayland
[wayland-protocols.git]: https://cgit.freedesktop.org/wayland/wayland-protocols
[linux-dmabuf]: https://cgit.freedesktop.org/wayland/wayland-protocols/tree/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
[v4l2-decode.git]: https://git.linaro.org/people/stanimir.varbanov/v4l2-decode.git

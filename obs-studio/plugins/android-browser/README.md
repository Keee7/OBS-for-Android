# Android browser source

WebView renders into an app-owned virtual display backed by a SurfaceTexture.
The OBS graphics thread consumes the external texture; no MediaProjection grant
or per-frame CPU readback is used. Width, height, maximum consumption rate, URL,
CSS, refresh, transparent background and OBS visibility callbacks are supported.
In an OBS interaction window, primary pointer down/move/up is forwarded as touch;
keyboard, mouse wheel, other buttons and multi-touch are not implemented.

"Transparent background" gives the Presentation window a translucent RGBA surface
and makes the WebView clear to transparent, so page areas with no background show
the scene below. Pages that paint their own opaque background stay opaque. The
option is off by default; enabling it recreates the WebView and reloads the page.

Transparency also changes how frames reach OBS. With the option off the display
writes into a SurfaceTexture, which the hardware composer scans out directly and
which therefore arrives fully opaque on devices whose composer ignores alpha. With
it on, the display writes into an ImageReader instead: those buffers keep the
compositor on its GPU path, so alpha survives, and each frame is bound as an
EGLImage on the OBS render thread and sampled as GL_TEXTURE_EXTERNAL_OES - no CPU
copy in either mode. The reader path flips V once in the existing uv matrix
because a gralloc image is bottom-up in GL; that is a shader constant, not an
extra pass. If a device's EGL cannot wrap AHardwareBuffer the log prints
"no EGL image path" and the transparent path stays blank rather than falling back
to an opaque picture.

There is no independent PCM audio output. Android network security policy applies
to HTTP URLs. Device validation is required for rendering, color, frame rate,
resource teardown, interaction and renderer recovery. CSS, URL and transparency
changes recreate the WebView and reset its in-memory page state. The properties
status is a snapshot: reopen the properties to refresh it.

For repeatable device checks, host browser-test.html on a reachable HTTPS server
with a trusted certificate and enter its URL. It exercises a transparent page,
a clickable button, animated Canvas, and a changing WebGL canvas. Place a bright
OBS source behind it to assess transparency. Check pointer and button behavior,
then change source dimensions and URL, refresh, hide/show and remove the source;
check logs for startup or renderer failures and verify resource recovery.

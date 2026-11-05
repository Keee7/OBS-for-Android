# Android browser source

WebView renders into an app-owned virtual display backed by a SurfaceTexture.
The OBS graphics thread consumes the external texture; no MediaProjection grant
or per-frame CPU readback is used. Width, height, maximum consumption rate, URL,
CSS, refresh and OBS visibility callbacks are supported. In an OBS interaction
window, primary pointer down/move/up is forwarded as touch; keyboard, mouse wheel,
other buttons and multi-touch are not implemented.

There is no independent PCM audio output, and preservation of page transparency
is not guaranteed. Android network security policy applies to HTTP URLs. Device
validation is required for rendering, color, frame rate, resource teardown,
interaction and renderer recovery. CSS and URL changes recreate the WebView and
reset its in-memory page state. The properties status is a snapshot: reopen the
properties to refresh it.

For repeatable device checks, host browser-test.html on a reachable HTTPS server
with a trusted certificate and enter its URL. It exercises a transparent page,
a clickable button, animated Canvas, and a changing WebGL canvas. Place a bright
OBS source behind it to assess transparency. Check pointer and button behavior,
then change source dimensions and URL, refresh, hide/show and remove the source;
check logs for startup or renderer failures and verify resource recovery.

# Gamma ramp transport

The payload copies exactly 256 red, 256 green and 256 blue unsigned 16-bit values
in little-endian order. Requests preserve the swapchain index and Set flags,
distinguish NULL ramp pointers and seed Get output from caller bytes. Thus an actual
backend no-op, such as an unsupported swapchain index, leaves output unchanged.

The request is exactly 1560 bytes and reply exactly 1552 bytes; version, reserved
fields, method and output presence are checked. Native methods are void: successful
reply status acknowledges dispatch, never an invented backend return value. Failed
transport replies contain no ramp. No local gamma cache or fabricated identity
ramp is used. Portable tests cover exact framing, endian encoding, malformed
fields and NULL/output rules. Native and frontend integration follow separately.

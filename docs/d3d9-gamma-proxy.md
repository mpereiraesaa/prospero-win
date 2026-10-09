# Gamma COM forwarding

Installs only SetGammaRamp and GetGammaRamp (device slots21/22). Each method pins
the parent across transport, preserves indices, flags and NULL ramps, and copies
all channel values into owned request storage. Get seeds the request with existing
caller bytes and updates the output only after validating the complete reply.

Both methods remain void. Transport failure or malformed dispatch acknowledgement
sticks/cancels the session via the nonblocking fail callback; it does not report an
invented backend HRESULT or overwrite caller output. No metadata lock spans RPC.

The controlled PE32/PE64 fixture checks all channel bytes, high flag/index bits,
NULL calls, backend no-op preservation, malformed reply and transport-failure
handling, and parent pins. Real native gamma behavior has a separate DXVK fixture.
No console or production-session integration is claimed here.

# PE32 shader and declaration ownership

Typed device creation methods safely measure bounded guest shader token streams
using ReadProcessMemory, copy the complete validated data, and upload owned
chunks through BEGIN/WRITE/COMMIT. Declaration elements are copied and encoded
field by field through the bounded END marker. Failed partial uploads abort.
Guest pointers never enter requests.

Returned declaration/vertex-shader/pixel-shader proxies have canonical local
IUnknown identity and a strong parent-device reference. Cache lookup compares
interface addresses without dereferencing foreign pointers. Final remote release
runs outside cache locks and defers during pumped callbacks; enqueue failure
retains ownership and marks the session failed.

GetDevice is local parent ownership. GetFunction/GetDeclaration delegate genuine
native queries through the supplied callback, preserving backend size behavior;
this module does not synthesize backend readback from creation inputs.

The fixture checks both actual PE32 and PE64 ABI execution against controlled
transport callbacks: identity reuse, copied uploads, safe rejected pointers,
query delegation, parent retention, typed resolver checks and deferred cleanup.
It does not claim an integrated game run or substitute a backend in production.
